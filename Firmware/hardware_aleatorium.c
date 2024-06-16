#ifdef HARDWARE_ALEATORIUM

#include "hardware.h"
#include "ssd1306.h"

#define LINE_IN_DETECT 24
#define HEADPHONE_DETECT 16
#define row_pin_base 11
#define col_pin_base 6
#define BLINK_PIN_LED 25
#define USB_POWER_SENSE 29
#define SUBSYSTEM_RESET_PIN 21

// I2C defines
#define I2C_SDA 2
#define I2C_SCL 3

static bool last_amp_state;
static uint8_t battery_level;

i2c_dma_t* i2c_dma;

#include "hardware.h"
#include "ssd1306.h"

#define LINE_IN_DETECT 24
#define HEADPHONE_DETECT 16
#define row_pin_base 11
#define col_pin_base 6
#define BLINK_PIN_LED 25
#define USB_POWER_SENSE 29
#define SUBSYSTEM_RESET_PIN 21

// I2C defines
#define I2C_SDA 2
#define I2C_SCL 3

void hardware_init()
{
    // give all the caps some time to warm up
    sleep_ms(100);
    set_sys_clock_khz(200000, true);

    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);
    
    i2c_init(I2C_PORT, 400*1000);

    gpio_init(23);
    gpio_set_dir(23, GPIO_IN);
    gpio_set_pulls(23, true, false);
    int gpioDownCount = 0;
    int gpioUpCount = 0;
    for(int i=0;i<10;i++)
    {
        if(!gpio_get(23))
        {
            gpioDownCount++;
        }
        else
        {
            gpioUpCount++;
        }
        sleep_ms(2);
    }

    if(gpioDownCount>gpioUpCount)
    {
        hardware_reboot_usb();
    }

}

void hardware_reboot_usb()
{
    reset_usb_boot(1<<BLINK_PIN_LED, 0);
}

void hardware_shutdown()
{
}

void hardware_post_audio_init()
{
}

bool hardware_get_button_state()
{
    // our main button state is inverted
    return gpio_get(23);
}

uint16_t hardware_get_adc_value()
{
    // our pot is wired backwards, invert
    return 0xfff-adc_read();
}

void hardware_update()
{
   // no op
}


#endif // HARDWARE_ALEATORIUM