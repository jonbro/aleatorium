# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
string(APPEND CMAKE_EXE_LINKER_FLAGS "-Wl,--print-memory-usage")

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/nanopb/extra)

add_definitions(-DLUA_USE_C89)
set(luaSrcDir "${CMAKE_CURRENT_LIST_DIR}/lua-5.4.3/src")
set(luaSrcFiles lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c
	ldump.c lfunc.c lgc.c linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c
	loslib.c lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c
	lutf8lib.c lvm.c lzio.c)
list(TRANSFORM luaSrcFiles PREPEND ${luaSrcDir}/)
set(LUA_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/lua-5.4.3/src")
include_directories(${LUA_INCLUDE_DIRS})
include_directories("${CMAKE_CURRENT_LIST_DIR}/LuaAutoC")

set(NANOPB_SRC_ROOT_FOLDER "${CMAKE_CURRENT_LIST_DIR}/nanopb")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${NANOPB_SRC_ROOT_FOLDER}/extra)
find_package(Nanopb REQUIRED)
include_directories(${NANOPB_INCLUDE_DIRS})
nanopb_generate_cpp(PROTO_SRCS PROTO_HDRS
        VoiceDataInternal.proto
        SongDataInternal.proto
        GlobalData.proto
)


# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(aleatorium C CXX ASM)

set (py_cmd "lua_to_cstrings.py")

add_custom_target(
 generateLuaStrings ALL
 COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/lua_to_cstrings.py ${CMAKE_CURRENT_SOURCE_DIR}
 BYPRODUCTS globals.lua.h
 COMMENT "Generating lua headers"
)


# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1
add_executable(aleatorium      
        main.cc
        multicore_support.c
        # usb_audio.c
        usb_descriptors.c
        GrooveBox.cc
        GrooveBoxTestUtils.cc
        voice_data.cc
        SongData.cc
        Instrument.cc
        Midi.cc
        MidiParamMapper.cc
        tlv320driver.c
        ssd1306.c
        ws2812.c
        hardware_audcalc.c
        hardware_aleatorium.c
        audio/macro_oscillator.cc
        i2c_dma.c
        audio/analog_oscillator.cc
        audio/digital_oscillator.cc
        audio/resources.cc
        printwrapper.c
        audio/random.cc 
        AudioSampleSecondbloop.cpp
        m6x118pt7b.cc 
        ${luaSrcFiles}
        ${PROTO_SRCS} ${PROTO_HDRS}
        flash.c
        filesystem.c
        littlefs/lfs.c
        littlefs/lfs_util.c
)
pico_generate_pio_header(aleatorium ${CMAKE_CURRENT_LIST_DIR}/input_output_copy_i2s.pio)
pico_generate_pio_header(aleatorium ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

pico_set_program_name(aleatorium "aleatorium")
pico_set_program_version(aleatorium "0.1")

pico_enable_stdio_usb(aleatorium 0)
pico_enable_stdio_uart(aleatorium 1)

add_dependencies(aleatorium generateLuaStrings)


target_include_directories(aleatorium PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# Add any user requested libraries
target_link_libraries(aleatorium
        pico_stdlib
        pico_multicore
        hardware_sleep
        hardware_i2c
        hardware_pio
        hardware_adc
        hardware_flash
        pico_rand
        pico_audio_i2s
        tinyusb_device
        tinyusb_board
        )
target_compile_definitions(aleatorium PRIVATE
# HARDWARE_AUDCALC=1
HARDWARE_ALEATORIUM=1
PICO_XOSC_STARTUP_DELAY_MULTIPLIER=32
)

pico_add_extra_outputs(aleatorium)