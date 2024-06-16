#ifndef HARDWARE_H_
#define HARDWARE_H_
#define I2C_PORT i2c1

#include "pico/stdlib.h"
#include "tlv320driver.h"
#include "hardware/watchdog.h"
#include "hardware/adc.h"
#include "pico/bootrom.h"
#include "ws2812.h"
#include "i2c_dma.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HARDWARE_AUDCALC
#define I2S_DATA_PIN 19
#define I2S_BCLK_PIN 17
#elif HARDWARE_ALEATORIUM
#define I2S_DATA_PIN 20
#define I2S_BCLK_PIN 18
#endif

void hardware_init();
void hardware_post_audio_init();
void hardware_shutdown();
void hardware_reboot_usb();
bool hardware_get_button_state();
uint16_t hardware_get_adc_value();
void hardware_update();

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_H_