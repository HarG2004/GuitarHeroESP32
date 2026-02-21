
#include "driver/i2c_master.h"

#define I2C_MASTER_SCL_IO  GPIO_NUM_21
#define I2C_MASTER_SDA_IO  GPIO_NUM_22
#define I2C_PORT           I2C_NUM_0
#define I2C_FREQ_HZ        100000
#define SLAVE_ADDRESS_LCD  0x27

// Public Functions
void lcd_init(void);
void lcd_put_cur(int row, int col);
void lcd_send_string(const char *str);


