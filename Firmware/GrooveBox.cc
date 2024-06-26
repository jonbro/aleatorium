#include "GrooveBox.h"
#include <string.h>
#include "m6x118pt7b.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include "VoiceDataInternal.pb.h"
#include "tlv320driver.h"
#include "multicore_support.h"
#include "GlobalDefines.h"

static inline void u32_urgb(uint32_t urgb, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = (urgb>>0)&0xff;
    *g = (urgb>>16)&0xff;
    *b = (urgb>>8)&0xff;
}

int16_t temp_buffer[SAMPLES_PER_BLOCK];
GrooveBox* groovebox;

void OnCCChangedBare(uint8_t cc, uint8_t newValue)
{
    groovebox->OnCCChanged(cc, newValue);
}
void GrooveBox::CalculateTempoIncrement()
{
    tempoPhaseIncrement = lut_tempo_phase_increment[songData.GetBpm()];
}
void GrooveBox::init(uint32_t *_color, lua_State *L)
{
    // usbSerialDevice = _usbSerialDevice;
    needsInitialADC = 30;
    groovebox = this;
    // midi.Init();
    // midi.OnCCChanged = OnCCChangedBare;
    ResetADCLatch();
    tempoPhase = 0;
    for(int i=0;i<VOICE_COUNT;i++)
    {
        instruments[i].Init(&midi, temp_buffer);
        instruments[i].songData = &songData;
    }
    for(int i=0;i<16;i++)
    {
        patterns[i].InitDefaults();
        patterns[i].SetInstrumentType(INSTRUMENT_MACRO);
    }
    this->L = L;
    int error = luaL_dostring(L, "function tempoSync() end");
    if (error) {
        fprintf(stderr, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);  /* pop error message from the stack */
    }
}

int16_t workBuffer[SAMPLES_PER_BLOCK];
int16_t workBuffer3[SAMPLES_PER_BLOCK];
int32_t workBuffer2[SAMPLES_PER_BLOCK*2];
static uint8_t sync_buffer[SAMPLES_PER_BLOCK];
int16_t toDelayBuffer[SAMPLES_PER_BLOCK];
int16_t toReverbBuffer[SAMPLES_PER_BLOCK];
int16_t recordBuffer[128]; // this must be 128 due to the requirements of the filesystem
uint8_t recordBufferOffset = 0;
//absolute_time_t lastRenderTime = -1;
int16_t last_delay = 0;
int16_t last_input;
uint32_t counter = 0;
uint32_t countToHalfSecond = 0;
uint8_t sync_count = 0;
bool audio_sync_state;
uint16_t audio_sync_countdown = 0;
uint32_t samples_since_last_sync = 0;
uint32_t ssls = 0;

void __not_in_flash_func(GrooveBox::Render)(int16_t* output_buffer, int16_t* input_buffer, size_t size)
{
    absolute_time_t renderStartTime = get_absolute_time();
    memset(workBuffer2, 0, sizeof(int32_t)*SAMPLES_PER_BLOCK*2);
    memset(toDelayBuffer, 0, sizeof(int16_t)*SAMPLES_PER_BLOCK);
    memset(toReverbBuffer, 0, sizeof(int16_t)*SAMPLES_PER_BLOCK);
    memset(output_buffer, 0, sizeof(int16_t)*SAMPLES_PER_BLOCK);
    last_input = 0;
    
    // update some song parameters
    delay.SetFeedback(songData.GetDelayFeedback());
    delay.SetTime(songData.GetDelayTime());

    if(songData.GetSyncInMode() == SyncModeNone)
        CalculateTempoIncrement();
    for(int v=0;v<VOICE_COUNT;v++)
    {
        // put in a request to render the other voice on the second core
        memset(sync_buffer, 0, SAMPLES_PER_BLOCK);
        memset(workBuffer, 0, sizeof(int16_t)*SAMPLES_PER_BLOCK);
        bool hasSecondCore = false;
        {
            memset(workBuffer3, 0, sizeof(int16_t)*SAMPLES_PER_BLOCK);
            queue_entry_t entry = {false, v, sync_buffer, workBuffer3};
            queue_add_blocking(&signal_queue, &entry);
            hasSecondCore = true;
            v++;
        }
        // queue_add_blocking(&signal_queue, &entry);
        instruments[v].Render(sync_buffer, workBuffer, SAMPLES_PER_BLOCK);
        // block until second thread render complete
        if(hasSecondCore)
        {
            queue_entry_complete_t result;
            queue_remove_blocking(&renderCompleteQueue, &result);
        }

        // mix in the instrument
        for(int i=0;i<SAMPLES_PER_BLOCK;i++)
        {
            workBuffer2[i*2] += mult_q15(workBuffer[i], 0x7fff-instruments[v].GetPan());
            workBuffer2[i*2+1] += mult_q15(workBuffer[i], instruments[v].GetPan());
            toDelayBuffer[i] = add_q15(toDelayBuffer[i], mult_q15(workBuffer[i], ((int16_t)instruments[v].delaySend)<<7));
            toReverbBuffer[i] = add_q15(toReverbBuffer[i], mult_q15(workBuffer[i], ((int16_t)instruments[v].reverbSend)<<7));
            if(hasSecondCore)
            {
                workBuffer2[i*2] += mult_q15(workBuffer3[i], 0x7fff-instruments[v-1].GetPan());
                workBuffer2[i*2+1] += mult_q15(workBuffer3[i], instruments[v-1].GetPan());
                toDelayBuffer[i] = add_q15(toDelayBuffer[i], mult_q15(workBuffer3[i], ((int16_t)instruments[v-1].delaySend)<<7));
                toReverbBuffer[i] = add_q15(toReverbBuffer[i], mult_q15(workBuffer3[i], ((int16_t)instruments[v-1].reverbSend)<<7));
            }
        }
    }
    bool clipping = false;
    int16_t l = 0;
    int16_t r = 0;
    int32_t mainL = 0;
    int32_t mainR = 0;
    for(int i=0;i<SAMPLES_PER_BLOCK;i++)
    {
        int16_t* chan = (output_buffer+i*2);
        mainL = workBuffer2[i*2];
        mainR = workBuffer2[i*2+1];
        delay.process(toDelayBuffer[i], l, r);
        int32_t lres = l;
        int32_t rres = r;
        // lower delay volume
        lres *= 0xe0;
        rres *= 0xe0;
        lres = lres >> 8;
        rres = rres >> 8;
        workBuffer2[i*2] += lres;
        workBuffer2[i*2+1] += rres;
    }
    for(int i=0;i<SAMPLES_PER_BLOCK;i++)
    {
        int16_t* chan = (output_buffer+i*2);
        mainL = workBuffer2[i*2];
        mainR = workBuffer2[i*2+1];
        verb.process(toReverbBuffer[i], l, r);
        int32_t lres = l;
        int32_t rres = r;
        lres = l;
        rres = r;
        // lower verb volume
        lres *= 0xe0;
        rres *= 0xe0;
        lres = lres >> 8;
        rres = rres >> 8;
        workBuffer2[i*2] += lres;
        workBuffer2[i*2+1] += rres;
    }
    for(int i=0;i<SAMPLES_PER_BLOCK;i++)
    {
        int16_t* chan = (output_buffer+i*2);
        mainL = workBuffer2[i*2];
        mainR = workBuffer2[i*2+1];
        mainL *= globalVolume;
        mainR *= globalVolume;
        mainL = mainL >> 8;
        mainR = mainR >> 8;
        // one more headroom clip
        // and then hard limit so we don't get overflows
        int32_t max = 0x7fff;
        if(mainL > max)
        {
            clipping = true;
            mainL = max;
        }
        if(mainR > max)
        {
            clipping = true;
            mainR = max;
        }
        if(mainL < -max)
        {
            clipping = true;
            mainL = -max;
        }
        if(mainR < -max)
        {
            clipping = true;
            mainR = -max;
        }

        chan[0] = mainL;
        chan[1] = mainR;
    }
    if(clipping)
    {
        printf("clipping\n");
    }

    if(IsPlaying())
    {
        bool tempoPulse = false;
        tempoPhase += tempoPhaseIncrement;
        if(songData.GetSyncInMode() == SyncModeNone)
        {
            if((tempoPhase >> 31) > 0)
            {
                tempoPhase &= 0x7fffffff;
                tempoPulse = true;
            }
        }
        if(tempoPulse)
        {
            OnTempoPulse();
        }
    }
    // absolute_time_t renderEndTime = get_absolute_time();
    // int64_t currentRender = absolute_time_diff_us(renderStartTime, renderEndTime);
    // renderTime += currentRender;
    sampleCount++;
    // midi.Flush();
}
void GrooveBox::OnTempoPulse()
{
    syncsRequired++;
}
void GrooveBox::TriggerInstrument(uint8_t key, int16_t midi_note, uint8_t step, uint8_t pattern, bool livePlay, VoiceData &voiceData, int channel)
{
    // lets just simplify - gang together the instruments that are above each other
    // 1+5, 2+6, 9+13 etc
    voiceCounter = channel%4+(channel/8)*4;
    Instrument *nextPlay = &instruments[voiceCounter];
    voiceChannel[voiceCounter] = channel;
    //printf("playing file for voice: %i %x\n", channel, voiceData.GetFile());

    nextPlay->NoteOn(key, midi_note, step, pattern, livePlay, voiceData);
    return;    
}
void GrooveBox::TriggerInstrumentMidi(int16_t midi_note, uint8_t step, uint8_t pattern, VoiceData &voiceData, int channel)
{
    Instrument *nextPlay = &instruments[channel];
    // determine if any of the voices are done playing
    bool foundVoice = false;
    for (size_t i = 0; i < VOICE_COUNT; i++)
    {
        if(!instruments[i].IsPlaying())
        {
            nextPlay = &instruments[i];
            foundVoice = true;
            voiceChannel[i] = channel;
            break;
        }
    }
    uint8_t _key = {0}; 
    nextPlay->NoteOn(_key, midi_note, step, pattern, true, voiceData);
    if(!foundVoice)
    {
        voiceChannel[voiceCounter] = channel;
        voiceCounter = (++voiceCounter)%VOICE_COUNT;
    }
}
int GrooveBox::GetNote()
{
    int res = needsNoteTrigger;
    needsNoteTrigger = -1;
    return res;
}
void GrooveBox::OnCCChanged(uint8_t cc, uint8_t newValue)
{
    if(paramSelectMode && lastEditedParam > 0)
    {
        // set the midi mapping
        //lastEditedParam = param*2+1;
        midiMap.SetCCTarget(cc, currentVoice, lastEditedParam, lastKeyPlayed);
    }
    else
    {
        midiMap.UpdateCC(patterns, cc, newValue*2, GetCurrentPattern());
        ResetADCLatch(); // I'm not sure what this does :0 - but without it, we can't update the graphics :(
    }
}
bool GrooveBox::IsPlaying()
{
    return playing;
}

void GrooveBox::SetGlobalParameter(uint8_t a, uint8_t b, bool setA, bool setB)
{
    // references must be initialized and can't change
    uint8_t* current_a = &songData.GetParam(param*2, GetCurrentPattern());
    // this needs to be special cased just for the octave setting - which is on a global param button, but stored
    // in the active voice
    if(param == 24)
    {
        current_a = &patterns[currentVoice].GetParam(param*2, lastKeyPlayed, GetCurrentPattern());
    }
    uint8_t& current_b = songData.GetParam(param*2+1, GetCurrentPattern());
    if(paramSetA)
    {
        *current_a = a;
        lastAdcValA = a;
    }
    if(paramSetB)
    {
        current_b = b;
        lastAdcValB = b;
    }
}

void GrooveBox::OnAdcUpdate(uint16_t a_in)
{
    // interpolate the input values
    uint16_t interpolationbias = 0xd000; 
    AdcInterpolatedA = (((uint32_t)a_in)*(0xffff-interpolationbias) + ((uint32_t)AdcInterpolatedA)*interpolationbias)>>16;
    uint8_t a = AdcInterpolatedA>>4;
    if(needsInitialADC > 0)
    {
        lastAdcValA = a;
        needsInitialADC--;
        return;
    }
    globalVolume = a;
}
void GrooveBox::OnFinishRecording()
{
    // determine how the file should be split
    if(patterns[recordingTarget].GetSampler() == SAMPLE_PLAYER_SLICE)
    {
        for (size_t i = 0; i < 16; i++)
        {
            // TODO: FIX ME
            // patterns[recordingTarget].sampleLength[i] = 16;
            // patterns[recordingTarget].sampleStart[i] = i*16;
        }
    }   
}
void GrooveBox::StartPlaying()
{
    playing = true;
    ResetPatternOffset();
}