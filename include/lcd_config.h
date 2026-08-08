#pragma once
// TFT_eSPI 用户配置 — 由 platformio -include 注入
// 2.8" 模组：GND 3V3 SCL SDA RES DC CS BLK SDO TE
// SCL=SCLK, SDA=MOSI

#define USER_SETUP_LOADED 1
#define DISABLE_ALL_LIBRARY_WARNINGS

// ESP32-S3 必须：默认 FSPI=0 → REG_SPI_BASE(0)=0 → tft.init() StoreProhibited@0x10
// USE_HSPI_PORT 将寄存器端口设为 3（GPSPI3），避免空指针写
#define USE_HSPI_PORT

// 若仍全黑/花屏，在 platformio 里改用 env:esp32-s3-ili9341
#define ST7789_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// 颜色顺序：不对就改成 TFT_BGR
#define TFT_RGB_ORDER TFT_RGB

// 多数 ST7789 需要反色；全白时注释下一行
#define TFT_INVERSION_ON

// ---- 接线（N16R8 安全脚，勿用 33–37）----
// 屏 SDA -> 11
// 屏 SCL -> 12
// 屏 CS  -> 5
// 屏 DC  -> 2
// 屏 RES -> 4
// 屏 BLK -> 15  （可先直连 3V3 测背光）
// 屏 GND -> GND
// 屏 3V3 -> 3V3
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define TFT_BL   15
#define TFT_BACKLIGHT_ON HIGH

// 不定义 TFT_MISO（避免 PlatformIO 把 -DTFT_MISO=-1 解析坏）

// 面包板先用 10MHz，点亮后再提高
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
