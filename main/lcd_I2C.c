


#include "lcd_I2C.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/i2c_master.h"

/**
 * This is a library to work on a standard lcd using the I2C protocal.
 * @author Harjap Grewal
 * @brief Library for esp32/esp-idf for my project in using an lcd with I2C.
 */

// Define logging tag
#define TAG "LCD"



// Create lcd device handle, global variable.
i2c_master_dev_handle_t lcd_dev;

// Method to delay program in microseconds.
inline void delay_us(uint32_t us) { 
    esp_rom_delay_us(us); 
}

/**
 * Method to send a command to the lcd.
 * Need to send 16 bits total for upper and lower bytes.
 * But command is only 8 bits, split into upper and lower bytes to be sent with control bits.
 * 0x0C = EN=1, RS=0, BL=1 ; 0x08 = EN=0, RS=0, BL=1  Control bits,RS in command mode.
 * Change Enable bit from 1 to 0 for a command to be accepted by lcd.
 */
static void lcd_send_cmd(uint8_t cmd)
{
    uint8_t data_u = (cmd & 0xF0);  // make last 4 bits of cmd 0's, creating upper 4 bits
    uint8_t data_l = ((cmd << 4) & 0xF0); // move last 4 bits left by 4, creating lower 4 bits

    uint8_t data_t[4]; // array to hold bytes to be transmitted.
    data_t[0] = data_u | 0x0C;  // EN=1, RS=0
    data_t[1] = data_u | 0x08;  // EN=0, RS=0
    data_t[2] = data_l | 0x0C;  // EN=1, RS=0
    data_t[3] = data_l | 0x08;  // EN=0, RS=0

    // Transmit command to lcd_dev, wait forever for data to complete
    ESP_ERROR_CHECK(i2c_master_transmit(lcd_dev, data_t, sizeof(data_t), -1));
}

/**
 * Method to send data to the lcd.
 * Need to send 16 bits total for upper and lower bytes.
 * But command is only 8 bits, split into upper and lower bytes to be sent with control bits.
 * 0x0D = EN=1, RS=1, BL=1 ; 0x09 = EN=0, RS=1, BL=1  Control bits, RS in data mode
 * Change Enable bit from 1 to 0 for data to be accepted by lcd.
 */
static void lcd_send_data(uint8_t data)
{
    uint8_t data_u = (data & 0xF0); // make last 4 bits of cmd 0's, creating upper 4 bits
    uint8_t data_l = ((data << 4) & 0xF0); // move last 4 bits left by 4, creating lower 4 bits

    uint8_t data_t[4]; // Array to hold data bytes to be transmitted.
    data_t[0] = data_u | 0x0D;  // EN=1, RS=1
    data_t[1] = data_u | 0x09;  // EN=0, RS=1
    data_t[2] = data_l | 0x0D;  // EN=1, RS=1
    data_t[3] = data_l | 0x09;  // EN=0, RS=1

    // Transmit byytes to lcd_dev handle, wait forever for transmit to complete.
    ESP_ERROR_CHECK(i2c_master_transmit(lcd_dev, data_t, sizeof(data_t), -1));
}

/**
 * Method to initialize lcd.
 */
void lcd_init(void)
{
    // Create master bus
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus; // master bus handle
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    // add lcd device with 0x27 as addrs.
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SLAVE_ADDRESS_LCD,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &lcd_dev));

    // Initialization commands for 4 bit mode.
    delay_us(50000);
    lcd_send_cmd(0x30); 
    delay_us(4500);
    lcd_send_cmd(0x30); 
    delay_us(200);
    lcd_send_cmd(0x30); 
    delay_us(200);
    lcd_send_cmd(0x20); 
    delay_us(200);   // 4-bit mode

    lcd_send_cmd(0x28); 
    delay_us(1000);  // 4-bit, 2-line, 5x8
    lcd_send_cmd(0x08); 
    delay_us(1000);  // display off
    lcd_send_cmd(0x01); 
    delay_us(2000);  // clear
    lcd_send_cmd(0x06); 
    delay_us(1000);  // entry mode
    lcd_send_cmd(0x0C); 
    delay_us(2000);  // display on, cursor off
}

/**
 * Method to change row and col
 */
void lcd_put_cur(int row, int col)
{
    //  0x80 row0, 0xC0 row1
    uint8_t addr = (row == 0) ? (0x80 + col) : (0xC0 + col);
    lcd_send_cmd(addr);
}

/**
 * Method to send string.
 */
void lcd_send_string(const char *str)
{
    while (*str) lcd_send_data((uint8_t)*str++);
}


