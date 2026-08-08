#pragma once
// 2.8" 很多实为 ILI9341，本配置给备用 env 使用

#define USER_SETUP_LOADED 1
#define DISABLE_ALL_LIBRARY_WARNINGS

// 同 ST7789：ESP32-S3 避免 TFT_eSPI SPI 寄存器基址为 0
#define USE_HSPI_PORT

#define ILI9341_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define TFT_BL   15
#define TFT_BACKLIGHT_ON HIGH

#define SPI_FREQUENCY       10000000
#define SPI_READ_FREQUENCY   5000000

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
