# Aleatorium

![buddha box with heads](Docs/ImageWithHeads.png)

A lua scriptable buddha box board swap.

The hardware on this project is forked from [Eric Brombaugh's RP2040 boardswap for buddha boxes](https://github.com/emeb/RP2040_Projects/tree/main/buddhabox). I'm so glad he opensourced this, a programmable buddha machine is something that I've long wanted.

The firmware contains an lua scriptable 8 channel synthesizer and sequencer. The synth has the following features:
- oscilators from MI braids  
- 1 assignable lfo + 2 semi assignable Envs per voice
- lowpass filter per voice
- reverb & delay send per voice

The sequencer is a callback that is run at 96ppq. All further sequencing must be done by you.

You write new songs for aleatorium by connecting to it with usb, and loading lua scripts onto it with the [web based code editor](https://github.com/jonbro/aleatorium-editor).

## Scripting API

The scripting interface is quite simple, but there are some example files, [jam1](Firmware/jam1.lua) and [jam2](Firmware/jam2.lua). The [globals.lua file](Firmware/globals.lua) contains parameter constants for altering the synth voices.

```
-- setParam(voiceNumber, targetParameter, value)
-- voiceNumber: 0-7, the voice you want to set
-- targetParameter: use the params table from globals.lua
-- value: 0-255. Some parameters take a value from globals here, see below
setParam(0, param.timbre, 80)

-- playNote(voiceNumber, midiNoteValue)
-- voiceNumber: 0-7, the voice you want to set
-- midiNoteValue: 60 is middle c
playNote(0, 60)

-- tempoSync
-- called 96 times per quarternote. Currently there is no way to set the bpm other than recompling the firmware.

function tempoSync()

end

-- Special param targets:

-- shape - pick the voice shape, these are a subset of the oscilators from MI Braids
setParam(0, param.shape, shape.morph)

-- Env target - env1Target always goes to volume and the assigned target, env2Target only goes to the assigned target
setParam(0, param.env1Target, envTarget.cutoff)

-- LFO target - can be sent into params including env parameters
setParam(0, param.lfo1Target, lfoTarget.env12Attack)

```

## Editor notes

[A live version of the editor is available here](https://jonbro.github.io/aleatorium-editor/). Requires a non-firefox browser, due to using webserial. Make sure you connect to the device before you start trying to use it.

Use `ctrl+p` to send the current script to device and execute. You can run the following commands in the console:

```

-- save
-- provide a filename without quotes to save the file. This will add it to the set of songs that is looped through when pressing the button.
-- scripts are limited to a maximum of 16k
-- folders are not currently supported
save mysong.lua

-- load
-- provide a filename without quotes to load the file. This will replace the contents of the editor with the script.
-- folders are not currently supported. You cannot load the globals file.
load mysong.lua

-- del / rm
-- provide a filename without quotes to delete the file. This will remove it from the filesystem, and it will no longer be part of the songs on device
rm mysong.lua
```