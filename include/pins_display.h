#pragma once
// 2.8" SPI 屏: GND 3V3 SCL SDA RES DC CS BLK SDO TE
//
// 背光（低功耗用 GPIO 控制）:
//   BLK → GPIO15   只接 15，不要并联 3V3
//   高电平点亮（BL_ACTIVE_HIGH=1）；若接反改 0
//   3V3/GND 仍接电源；关背光只关 BLK，屏逻辑可保持

#define PIN_TFT_MOSI 11
#define PIN_TFT_SCLK 12
#define PIN_TFT_CS    5
#define PIN_TFT_DC    2
#define PIN_TFT_RST   4
#define PIN_TFT_BL   15

// 1=高电平亮，0=低电平亮
#ifndef BL_ACTIVE_HIGH
#define BL_ACTIVE_HIGH 1
#endif

// 欢迎页无操作多久后关背光（ms），0=不自动关
#ifndef BL_IDLE_OFF_MS
#define BL_IDLE_OFF_MS 600000
#endif

#define TFT_PHYS_W  240
#define TFT_PHYS_H  320
