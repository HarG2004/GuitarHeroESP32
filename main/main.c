#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/i2c_master.h"
#include "lcd_I2C.h"

#define TAG "LCD"



void app_main(void)
{


    // 3) Init + print
    lcd_init();
    lcd_put_cur(0, 0);
    lcd_send_string("Play song #1");
    lcd_put_cur(1, 0);
    lcd_send_string("-> Enter");



    ESP_LOGI(TAG, "Send");
}
