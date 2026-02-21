idf_component_register(SRCS "lcd_I2C.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_i2c
                    REQUIRES fatfs 
                    REQUIRES sdmmc
                    REQUIRES driver
                    REQUIRES vfs)
