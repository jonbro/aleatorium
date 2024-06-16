#ifdef HARDWARE_AUDCALC
#include "hardware.h"
#include "tlv320driver.h"

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
    adc_gpio_init(27);

    // power latch
    gpio_init(23);
    gpio_set_dir(23, GPIO_IN);
    gpio_set_pulls(23, true, false);

    gpio_init(SUBSYSTEM_RESET_PIN);
    gpio_set_dir(SUBSYSTEM_RESET_PIN, GPIO_OUT);

    gpio_put(SUBSYSTEM_RESET_PIN, 1);
    sleep_ms(10);
    gpio_put(SUBSYSTEM_RESET_PIN, 0);
    sleep_ms(40);
    gpio_put(SUBSYSTEM_RESET_PIN, 1);
    sleep_ms(20);

    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    
    i2c_init(I2C_PORT, 400*1000);
    adc_select_input(1);
    // setup the rows / colums for input
    for (size_t i = 0; i < 5; i++)
    {
        gpio_init(col_pin_base+i);
        gpio_set_dir(col_pin_base+i, GPIO_OUT);
        gpio_disable_pulls(col_pin_base+i);
        gpio_init(row_pin_base+i);
        gpio_set_dir(row_pin_base+i, GPIO_IN);
        gpio_pull_down(row_pin_base+i);
    }
    
    // only check the fouth column
    gpio_put_masked(0x7c0, 1<<(col_pin_base+4));

    {
        // if the user isn't holding the powerkey, 
        // or if holding power & esc then immediately shutdown
        if(!hardware_get_button_state())
        {
            hardware_shutdown();
        }
        else if(gpio_get(row_pin_base+4))
        {
            hardware_reboot_usb();
        }
    }
}

void hardware_reboot_usb()
{
    reset_usb_boot(1<<BLINK_PIN_LED, 0);
}

void hardware_shutdown()
{
    // shutdown all subsystems
    gpio_put(SUBSYSTEM_RESET_PIN, 0);

    // wait for the powerkey to go low, then shutdown
    while(hardware_get_button_state(0,0))
    {
        // do nothing
    }
    watchdog_enable(1,true);
    while(1);
}

void hardware_post_audio_init()
{
    // initialize the TLV driver
    tlvDriverInit();
}

bool hardware_get_button_state()
{
    // our main button state is inverted
    return !gpio_get(23);
}

uint16_t hardware_get_adc_value()
{
    return adc_read();
}

void hardware_update()
{
    if(gpio_get(row_pin_base+4))
    {
        hardware_shutdown();
    }
}

#endif // HARDWARE_AUDCALC