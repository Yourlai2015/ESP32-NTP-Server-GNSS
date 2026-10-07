// SSD1306 128x64 OLED 驱动（ESP32-S3 硬件 I2C）。
// 这里改用 ESP-IDF I2C 主机外设，写入统一走批量缓冲。

#pragma once

#include <stdint.h>

#include "app_config.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef uint8_t u8;
typedef uint32_t u32;

#define OLED_CMD 0  // 写命令
#define OLED_DATA 1 // 写数据

// SSD1306 7 位 I2C 地址（8 位写字节 0x78 右移一位）
#define OLED_I2C_ADDRESS 0x3C
#define OLED_I2C_PORT I2C_NUM_0
#define OLED_I2C_SPEED_HZ 400000
#define OLED_I2C_TIMEOUT_MS 100

#define SIZE 16
#define XLevelL 0x02
#define XLevelH 0x10
#define Max_Column 128
#define Max_Row 64
#define Brightness 0xFF
#define X_WIDTH 128
#define Y_WIDTH 64

// 配置 I2C 主机总线与 SSD1306 设备（幂等）
void OLED_I2C_Init(void);

// 将待发送的批量缓冲刷到屏幕
void OLED_Flush(void);

// OLED 底层接口
void OLED_WR_Byte(unsigned char dat, unsigned char cmd);
void OLED_Display_On(void);
void OLED_Display_Off(void);
void OLED_Init(void);
void OLED_Clear(void);
void OLED_On(void);
void OLED_ShowChar(u8 x, u8 y, u8 chr, u8 Char_Size);
void OLED_ShowNum(u8 x, u8 y, u32 num, u8 len, u8 size);
void OLED_ShowString(u8 x, u8 y, const char *p, u8 Char_Size);
void OLED_Set_Pos(unsigned char x, unsigned char y);
void OLED_DrawBMP(unsigned char x0, unsigned char y0, unsigned char x1, unsigned char y1, unsigned char BMP[]);
void Delay_50ms(unsigned int Del_50ms);
void Delay_1ms(unsigned int Del_1ms);
void fill_picture(unsigned char fill_Data);
