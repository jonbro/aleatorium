#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/sleep.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/adc.h"
#include "hardware/flash.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/structs/scb.h"
#include "hardware/rosc.h"

#include <math.h>

#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"
#include "input_output_copy_i2s.pio.h"
#include "ws2812.pio.h"

extern "C" {
#include "ssd1306.h"
}
#include "m6x118pt7b.h"

#include "GrooveBox.h"
#include "audio/macro_oscillator.h"

#include "ws2812.h"
#include "hardware.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include "USBSerialDevice.h"
#include "multicore_support.h"
#include "GlobalDefines.h"
#include "bsp/board.h"
#include "lua.hpp"
#include "filesystem.h"
#include "printwrapper.h"
#include "globals.lua.h"

using namespace braids;


#define USING_DEMO_BOARD 0

void on_usb_microphone_tx_ready();

static float clip(float value)
{
    value = value < -1.0f ? -1.0f : value;
    value = value > +1.0f ? +1.0f : value;
    return value;
}

int dma_chan_input;
int dma_chan_output;
uint sm;

bool audioInReady = false;
bool audioOutReady = false;
uint32_t capture_buf[SAMPLES_PER_BLOCK*BLOCKS_PER_SEND*2];
uint32_t output_buf[SAMPLES_PER_BLOCK*BLOCKS_PER_SEND*2];

int outBufOffset = 0;
int inBufOffset = 0;
int audioGenOffset = 0;
GrooveBox gbox;
USBSerialDevice usbSerialDevice;
auto_init_mutex(audioProcessMutex);

int triCount = 0;
int note = 60;


/*
* double buffered audio - if the signal that the next frame of audio hasn't completed sending,
*/ 

void __not_in_flash_func(dma_input_handler)() {
    if (dma_hw->ints0 & (1u<<dma_chan_input)) {
        dma_hw->ints0 = (1u << dma_chan_input);
        dma_channel_set_write_addr(dma_chan_input, capture_buf+(inBufOffset)*SAMPLES_PER_SEND, true);
        uint32_t x;
        if(mutex_try_enter(&audioProcessMutex, &x))
        {
            inBufOffset = (inBufOffset+1)%2;
            audioInReady = true;
            mutex_exit(&audioProcessMutex);
        }
    }
}

void __not_in_flash_func(dma_output_handler)() {
    if (dma_hw->ints0 & (1u<<dma_chan_output)) {
        dma_hw->ints0 = 1u << dma_chan_output;
        dma_channel_set_read_addr(dma_chan_output, output_buf+(outBufOffset)*SAMPLES_PER_SEND, true);
        uint32_t x;
        if(mutex_try_enter(&audioProcessMutex, &x))
        {
            outBufOffset = (outBufOffset+1)%2;
            audioOutReady = true;
            mutex_exit(&audioProcessMutex);
        }
    }
}

int flashReadPos = 0;

static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

bool needsScreenupdate;
bool repeating_timer_callback(struct repeating_timer *t) {
    needsScreenupdate = true;
    return true;
}

bool usb_timer_callback(struct repeating_timer *t) {
    //usbaudio_update();
    return true;
}

bool screen_flip_ready = false;
int drawY = 0;
void __not_in_flash_func(draw_screen)()
{
    // this needs to be called here, because the multicore launch relies on
    // the fifo being functional
    sleep_ms(20);
    multicore_lockout_victim_init();
    while(true)
    {
        queue_entry_t entry;
        if(queue_try_remove(&signal_queue, &entry))
        {
            if(entry.renderInstrument >= 0)
            {
                gbox.instruments[entry.renderInstrument].Render(entry.sync_buffer, entry.workBuffer, 64);
                queue_entry_complete_t result;
                result.screenFlipComplete = false;
                result.renderInstrumentComplete = true;
                queue_add_blocking(&renderCompleteQueue, &result);
            }
        }
    }
}


void configure_audio_driver()
{
    /**/
    PIO pio = pio1;
    uint offset = pio_add_program(pio, &input_output_copy_i2s_program);
    printf("loaded program at offset: %i\n", offset);
    sm = pio_claim_unused_sm(pio, true);
    printf("claimed sm: %i\n", sm); //I2S_DATA_PIN
    int sample_freq = 32000;
    printf("setting pio freq %d\n", (int) sample_freq);
    uint32_t system_clock_frequency = clock_get_hz(clk_sys);
    assert(system_clock_frequency < 0x40000000);

    uint32_t divider = system_clock_frequency*2 / sample_freq; // avoid arithmetic overflow
    // uint32_t divider = system_clock_frequency/(sample_freq*3*32);
    printf("System clock at %u, I2S clock divider 0x%x/256\n", (uint) system_clock_frequency, (uint)divider);
    assert(divider < 0x1000000);
    input_output_copy_i2s_program_init(pio, sm, offset, I2S_DATA_PIN, I2S_DATA_PIN+1, I2S_BCLK_PIN);
    pio_sm_set_enabled(pio, sm, true);
    pio_sm_set_clkdiv_int_frac(pio, sm, divider >> 8u, divider & 0xffu);

    // just use a dma to pull the data out. Whee!
    dma_chan_input = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_chan_input);
    channel_config_set_read_increment(&c,false);
    // increment on write
    channel_config_set_write_increment(&c,true);
    channel_config_set_dreq(&c,pio_get_dreq(pio,sm,false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);

    dma_channel_configure(
        dma_chan_input,
        &c,
        capture_buf, // Destination pointer
        &pio->rxf[sm], // Source pointer
        SAMPLES_PER_SEND, // Number of transfers
        true// Start immediately
    );
    // Tell the DMA to raise IRQ line 0 when the channel finishes a block
    dma_channel_set_irq0_enabled(dma_chan_input, true);

    // Configure the processor to run dma_handler() when DMA IRQ 0 is asserted
    irq_add_shared_handler(DMA_IRQ_0, dma_input_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY-1);
    irq_set_enabled(DMA_IRQ_0, true);
    // need another dma channel for output
    dma_chan_output = dma_claim_unused_channel(true);
    dma_channel_config cc = dma_channel_get_default_config(dma_chan_output);
    // increment on read, but not on write
    channel_config_set_read_increment(&cc,true);
    channel_config_set_write_increment(&cc,false);
    channel_config_set_dreq(&cc,pio_get_dreq(pio,sm,true));
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    dma_channel_configure(
        dma_chan_output,
        &cc,
        &pio->txf[sm], // Destination pointer
        output_buf, // Source pointer
        SAMPLES_PER_SEND, // Number of transfers
        true// Start immediately
    );
    dma_channel_set_irq0_enabled(dma_chan_output, true);
    // Configure the processor to run dma_handler() when DMA IRQ 0 is asserted
    irq_add_shared_handler(DMA_IRQ_0, dma_output_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY-2);
    irq_set_enabled(DMA_IRQ_0, true);
}

uint8_t adc1_prev;
uint8_t adc2_prev;

#define LINE_IN_DETECT 24
#define HEADPHONE_DETECT 16

// Variable that holds the current position in the sequence.
uint32_t note_pos = 0;

// Store example melody as an array of note values
uint8_t note_sequence[] =
{
  74,78,81,86,90,93,98,102,57,61,66,69,73,78,81,85,88,92,97,100,97,92,88,85,81,78,
  74,69,66,62,57,62,66,69,74,78,81,86,90,93,97,102,97,93,90,85,81,78,73,68,64,61,
  56,61,64,68,74,78,81,86,90,93,98,102
};


uint32_t color[25];
const uint8_t gamma8[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
    2,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  5,  5,  5,
    5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10,
    10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
    25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
    37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
    51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
    69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
    90, 92, 93, 95, 96, 98, 99,101,102,104,105,107,109,110,112,114,
    115,117,119,120,122,124,126,127,129,131,133,135,137,138,140,142,
    144,146,148,150,152,154,156,158,160,162,164,167,169,171,173,175,
    177,180,182,184,186,189,191,193,196,198,200,203,205,208,210,213,
    215,218,220,223,225,228,231,233,236,239,241,244,247,249,252,255 };

static inline uint32_t gammarbg_u32(uint8_t r, uint8_t g, uint8_t b)
{
    return urgb_u32(gamma8[r], gamma8[g], gamma8[b]);
}

static int l_setLed(lua_State *L)
{
    int8_t index = luaL_checknumber(L, 1);
    index = index<0?0:index;
    index = index > 24?24:index;
    uint8_t r = luaL_checknumber(L, 2);
    uint8_t g = luaL_checknumber(L, 3);
    uint8_t b = luaL_checknumber(L, 4);
    color[index] = gammarbg_u32(r,g,b);
    return 0;
}
static int l_playNote(lua_State *L)
{
    int8_t voiceIndex = luaL_checknumber(L, 1);
    gbox.instruments[voiceIndex].NoteOn(0, luaL_checknumber(L, 2), 0, 0, false, gbox.patterns[voiceIndex]);
    return 0;
}
static int l_setParam(lua_State *L)
{
    int8_t index = luaL_checknumber(L, 1);
    uint8_t& current_a = gbox.patterns[index].GetParam(luaL_checknumber(L, 2), 0, 0);
    current_a = luaL_checknumber(L, 3);
    return 0;
}
static int l_getPot(lua_State *L)
{
    lua_pushnumber(L, gbox.globalVolume);
    return 1;
}
static int l_resetParams(lua_State *L)
{
    for(int i=0;i<8;i++)
    {
        gbox.patterns[i].InitDefaults();
    }
    return 0;
}

char incomingData[16384];
int charCount = 0;

static int SaveFile()
{
    lfs_file_t file;
    // extract the filename
    char *s = incomingData;
    int searchCount = charCount;
    while(searchCount--)
    {
        if( *s++ == '^' )
        {
            if( *s++ == '^' )
            {
                if(*s == 'S')
                {
                    s++;
                    break;
                }
            }
        }
    }
    char filename[64];
    char *fn = filename;
    // extract the filename
    // TODO: deal with buffer overflow
    while(searchCount--)
    {
        if(*s != '^')
        {
            *fn = *s;
            s++; fn++;
        }
        else
        {
            s++;
            break;
        }
    }
    *fn = 0;
    lfs_file_open(GetFilesystem(), &file, filename, LFS_O_RDWR | LFS_O_CREAT);
    // reset back to zero in case we've been appending to the end
    lfs_file_truncate(GetFilesystem(), &file, 0);
    lfs_file_rewind(GetFilesystem(), &file);
    lfs_file_write(GetFilesystem(), &file, s, searchCount);
    lfs_file_close(GetFilesystem(), &file);
    lfs_file_open(GetFilesystem(), &file, filename, LFS_O_RDONLY);
    lfs_soff_t size = lfs_file_size(GetFilesystem(), &file);
    lfs_file_close(GetFilesystem(), &file);
    printf("wrote file: %s - %i bytes\n", filename, size);
    return 0;
}

static int LoadFile()
{
    lfs_file_t file;
    // extract the filename
    char *s = incomingData;
    int searchCount = charCount;
    while(searchCount--)
    {
        if( *s++ == '^' )
        {
            if( *s++ == '^' )
            {
                if(*s == 'L')
                {
                    s++;
                    break;
                }
            }
        }
    }
    char filename[64];
    char *fn = filename;
    // extract the filename
    // TODO: deal with buffer overflow
    while(searchCount--)
    {
        if(*s != 0x03)
        {
            *fn = *s;
            s++; fn++;
        }
        else
        {
            break;
        }
    }
    *fn = 0;
    printf("attempt to open file: %s\n", filename);
    int e = lfs_file_open(GetFilesystem(), &file, filename, LFS_O_RDONLY);
    if(e < LFS_ERR_OK)
    {
        printf("file open error: %i\n", e);
    } 
    int size = lfs_file_size(GetFilesystem(), &file);
    e = lfs_file_read(GetFilesystem(), &file, incomingData, size);
    if(e < LFS_ERR_OK)
    {
        printf("file read error: %i\n", e);
    }
    lfs_file_close(GetFilesystem(), &file);
    printf("^^L%s^^E", incomingData);
    return 0;
}

static int DeleteFile()
{
    lfs_file_t file;
    // extract the filename
    char *s = incomingData;
    int searchCount = charCount;
    while(searchCount--)
    {
        if( *s++ == '^' )
        {
            if( *s++ == '^' )
            {
                if(*s == 'D')
                {
                    s++;
                    break;
                }
            }
        }
    }
    char filename[64];
    char *fn = filename;
    // extract the filename
    // TODO: deal with buffer overflow
    while(searchCount--)
    {
        if(*s != 0x03)
        {
            *fn = *s;
            s++; fn++;
        }
        else
        {
            break;
        }
    }
    *fn = 0;
    int e = lfs_remove(GetFilesystem(), filename);
    if(e < LFS_ERR_OK)
    {
        printf("file remove error: %i\n", e);
    }
    else
    {
        printf("file removed: %s\n", filename);
    }
    return 0;
}
lua_State *L;
#define UART_ID uart0

// void incoming_uart() {
//     while (uart_is_readable(UART_ID)) {
//         char ch = uart_getc(UART_ID);
//         incomingData[charCount++] = ch;
//         if(ch=='\r')
//             printf("\r\n");
//         else
//             printf("%c", ch);
//         // end of text / control c
//         if(ch == 0x03)
//         {
//             incomingData[--charCount] = 0;
//             printf("\n\n attempting to execute \n ----------------------- \n %s", incomingData);
//             int error = luaL_dostring(L, incomingData);
//             if (error) {
//                 printf("\n%s", lua_tostring(L, -1));
//                 lua_pop(L, 1);  /* pop error message from the stack */
//             }
//             charCount = 0;
//         }
//     }
// } 


static void echo_serial_port(uint8_t itf, uint8_t buf[], uint32_t count) {
  uint8_t const case_diff = 'a' - 'A';

  for (uint32_t i = 0; i < count; i++) {
    tud_cdc_n_write_char(itf, buf[i]);
  }
  tud_cdc_n_write_flush(itf);
}

// Invoked when device is mounted
void tud_mount_cb(void) {
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
}


//--------------------------------------------------------------------+
// USB CDC
//--------------------------------------------------------------------+
static void cdc_task(void) {
  uint8_t itf;

  for (itf = 0; itf < CFG_TUD_CDC; itf++) {
    // connected() check for DTR bit
    // Most but not all terminal client set this when making connection
    // if ( tud_cdc_n_connected(itf) )
    {
      if (tud_cdc_n_available(itf)) {
        uint8_t buf[256];

        uint32_t count = tud_cdc_n_read(itf, buf, sizeof(buf));
        for(int i=0;i<count;i++)
        {
            char ch = buf[i];
            incomingData[charCount++] = ch;
            // end of text / control c
            if(ch == 0x03)
            {
                // clear the end byte of the data
                incomingData[--charCount] = 0;
                bool save = false;
                bool load = false;
                bool del = false;
                // search for the command at the beginning
                char *s = incomingData;
                int searchCount = charCount;
                while(searchCount--)
                {
                    if( *s++ == '^' )
                    {
                        if( *s++ == '^' )
                        {
                            if(*s == 'S')
                            {
                                save = true;
                            }
                            if(*s == 'L')
                            {
                                load = true;
                            }
                            if(*s == 'D')
                            {
                                del = true;
                            }
                        }
                    }
                }
                if(load)
                {
                    LoadFile();
                    charCount = 0;
                    return;
                }
                if(del)
                {
                    DeleteFile();
                    charCount = 0;
                    return;
                }
                if(save)
                {
                    s++;
                }
                else
                {
                    s = incomingData;
                }
                printf("attempting to execute\n");
                int error = luaL_dostring(L, s);
                if (error) {
                    printf("%s\n", lua_tostring(L, -1));
                    lua_pop(L, 1);  /* pop error message from the stack */
                }
                if(save)
                {
                    SaveFile();
                }
                charCount = 0;
            }

        }

        // echo back to both serial ports
        //echo_serial_port(0, buf, count);
      }
    }
  }
}

int main()
{
    board_init();
    tusb_init();

    L = luaL_newstate();   /* opens Lua */
    luaL_openlibs(L);
    int e = luaL_dostring(L, "print(collectgarbage(\"count\"))");
    if(e)
    {
        printf("%s\n", lua_tostring(L, -1));
    }

    lua_pushcfunction(L, l_setLed);
    lua_setglobal(L, "setLed");
    lua_pushcfunction(L, l_playNote);
    lua_setglobal(L, "playNote");
    lua_pushcfunction(L, l_setParam);
    lua_setglobal(L, "setParam");
    lua_pushcfunction(L, l_resetParams);
    lua_setglobal(L, "resetParams");
    lua_pushcfunction(L, l_getPot);
    lua_setglobal(L, "getPot");
    
    e = luaL_dostring(L, global_lua);
    if(e)
    {
        printf("%s\n", lua_tostring(L, -1));
    }

    hardware_init();

    sleep_ms(10);

    stdio_init_all();
    usb_printf_init();
    ws2812_init();
    memset(color, 0, 25 * sizeof(uint32_t));
    ws2812_setColors(color+5);
    ws2812_trigger();

    
    queue_init(&signal_queue, sizeof(queue_entry_t), 3);
    queue_init(&complete_queue, sizeof(queue_entry_complete_t), 2);
    queue_init(&renderCompleteQueue, sizeof(queue_entry_complete_t), 2);
    queue_entry_complete_t result;
    result.screenFlipComplete = true;
    result.renderInstrumentComplete = false;
    queue_add_blocking(&complete_queue, &result);
    multicore_launch_core1(draw_screen);
    struct repeating_timer timer;
    struct repeating_timer timer2;

    InitFilesystem();

    // if the user is holding down specific keys on powerup, then clear the full file system
    // InitializeFilesystem(hardware_get_key_state(0,4) && hardware_get_key_state(3, 4), get_rand_32());
    //usbSerialDevice.Init();
    gbox.init(color, L);

    memset(output_buf, SAMPLES_PER_SEND*2, sizeof(uint32_t));
    memset(capture_buf, SAMPLES_PER_SEND*2, sizeof(uint32_t));

    configure_audio_driver();
    hardware_post_audio_init();

    int step = 0;
    bool buttonState = false;
    bool lastButtonState = false;

    add_repeating_timer_ms(-33, repeating_timer_callback, NULL, &timer);

    int16_t touchCounter = 0x7fff;
    int16_t headphoneCheck = 60;
    uint8_t brightnesscount = 0;
    int lostCount = 0;
    int error = luaL_dostring(L, "function update() end");
    if (error) {
        fprintf(stderr, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);  /* pop error message from the stack */
    }
    gbox.StartPlaying();

    lfs_dir_t scriptsDir;
    lfs_dir_open(GetFilesystem(), &scriptsDir, "scripts");
    struct lfs_info info;
    bool ranNextFile = false;
    while(lfs_dir_read(GetFilesystem(), &scriptsDir, &info) && !ranNextFile)
    {
        if(info.type == LFS_TYPE_REG)
        {
            char path[1024];
            sprintf(path, "scripts/%s", info.name);

            lfs_file_t file;
            // attempt to open this file
            lfs_file_open(GetFilesystem(), &file, path, LFS_O_RDONLY);
            lfs_file_read(GetFilesystem(), &file, incomingData, info.size);
            error = luaL_dostring(L, incomingData);
            if (error) {
                fprintf(stderr, "%s", lua_tostring(L, -1));
                lua_pop(L, 1);  /* pop error message from the stack */
                lfs_file_close(GetFilesystem(), &file);
            }
            // this should work fine, lets just go with it
            lfs_file_close(GetFilesystem(), &file);
            ranNextFile = true;
        }
    }


    while(true)
    {
        tud_task(); // tinyusb device task
        cdc_task();
        if(audioOutReady && audioInReady)
        {
            mutex_enter_blocking(&audioProcessMutex);
            for(int i=0;i<BLOCKS_PER_SEND;i++)
            {
                uint32_t *input = capture_buf+inBufOffset*SAMPLES_PER_SEND+SAMPLES_PER_BLOCK*i;
                uint32_t *output = output_buf+outBufOffset*SAMPLES_PER_SEND+SAMPLES_PER_BLOCK*i;
                gbox.Render((int16_t*)(output), (int16_t*)(input), SAMPLES_PER_BLOCK);
            }
            audioInReady = false;
            audioOutReady = false;
            mutex_exit(&audioProcessMutex);
        }
        else if(gbox.syncsRequired > 0)
        {
            int error = luaL_dostring(L, "tempoSync()");
            if (error) {
                fprintf(stderr, "%s", lua_tostring(L, -1));
                lua_pop(L, 1);  /* pop error message from the stack */
            }
            gbox.syncsRequired--;
        }
        else
        {
            lua_gc(L, LUA_GCSTEP, 1);
        }

        // button handler
        {
            bool btn = hardware_get_button_state();
            buttonState = hardware_get_button_state();
            if(buttonState && !lastButtonState)
            {
                // get the current files offset
                lfs_soff_t off = lfs_dir_tell(GetFilesystem(), &scriptsDir);

                // attempt to read next file
                bool stopFinding = false;
                while(!stopFinding)
                {
                    int res = lfs_dir_read(GetFilesystem(), &scriptsDir, &info);
                    if(res == 0)
                    {
                        lfs_dir_rewind(GetFilesystem(), &scriptsDir);
                        continue;
                    }
                    lfs_soff_t newOff = lfs_dir_tell(GetFilesystem(), &scriptsDir);
                    if(newOff == off)
                    {
                        char path[1024];
                        sprintf(path, "scripts/%s", info.name);
                        printf("only one file in scripts: %s\n", path);
                        stopFinding = true;
                        continue;
                    }
                    if(res >= 0 && info.type == LFS_TYPE_REG)
                    {
                        char path[1024];
                        sprintf(path, "scripts/%s", info.name);

                        lfs_file_t file;
                        // attempt to open this file
                        printf("attempt to open file: %s\n", path);
                        int e = lfs_file_open(GetFilesystem(), &file, path, LFS_O_RDONLY);
                        if(e < LFS_ERR_OK)
                        {
                            printf("file open error: %i\n", e);
                        }
                        e = lfs_file_read(GetFilesystem(), &file, incomingData, info.size);
                        if(e < LFS_ERR_OK)
                        {
                            printf("file read error: %i\n", e);
                        }
                        error = luaL_dostring(L, incomingData);
                        if (error) {
                            fprintf(stderr, "%s", lua_tostring(L, -1));
                            lua_pop(L, 1);  /* pop error message from the stack */
                            lfs_file_close(GetFilesystem(), &file);
                        }
                        // this should work fine, lets just go with it
                        lfs_file_close(GetFilesystem(), &file);
                        stopFinding = true;
                        continue;
                    }
                }
                
            }
            lastButtonState = buttonState;
        }

        if(needsScreenupdate)
        {
            gbox.OnAdcUpdate(hardware_get_adc_value());
            needsScreenupdate = false;
            hardware_update();
            int error = luaL_dostring(L, "update()");
            if (error) {
                fprintf(stderr, "%s", lua_tostring(L, -1));
                lua_pop(L, 1);
            }

        }
        
    }
    return 0;
}
