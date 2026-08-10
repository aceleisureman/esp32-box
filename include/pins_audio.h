#pragma once
// I2S 音频输出（MAX98357A 单声道 D 类功放）
//
// 选脚原则：
//   - 避开 GPIO26–37（N16R8 的 Flash / Octal PSRAM 占用）
//   - 避开 strapping 脚 0 / 3 / 45 / 46 与 USB 的 19 / 20、串口的 43 / 44
//   - 避开 ADC1 的 1 / 9 / 10，留给后续电池电压检测
//   - 选板子右侧连续的 38 / 39 / 40：功放模块放在面包板右半区时，
//     三根信号线可以短距直连，不必绕过整块开发板
//     （长跨线是数字音频底噪的主因：BCLK 1.4MHz 方波会耦合 WiFi/SPI 噪声）
//
// 接线（MAX98357A → ESP32-S3）:
//   VIN   → 5V（用 USB 5V，别接 3V3，否则功率上不去）
//   GND   → 右侧 G 排针（就近取地，别只靠面包板电源轨共地）
//   BCLK  → GPIO38   位时钟
//   LRC   → GPIO39   左右声道时钟（帧同步 / WS）
//   DIN   → GPIO40   串行数据
//   GAIN  → 接 VIN（5V）= 3dB（增益档位见下表；原接 GND=12dB 声音偏大，
//           改 VIN 直降 9dB，听感最舒服。别悬空，悬空 15dB 且易拾噪）
//   SD    → 悬空 = 常开；也可接 GPIO 做软关断省电（当前不接）
//   喇叭   → 功放的 + / -（4Ω 3W 或 8Ω 2W 均可）
//
// 降噪要点：
//   1. 三根信号线尽量短（<15cm）并排走，别分散绕行
//   2. 功放 VIN-GND 引脚旁并 470µF~1000µF 电解电容（正极接 VIN），
//      必须靠近功放，放远了滤波无效
//   3. 单独拉一根地线连功放 GND 与开发板 G，与信号线同路径
//
// 注意：MAX98357A 是单声道，SD 脚悬空时默认取 (L+R)/2 混合声道。
//
// MAX98357A GAIN 脚增益档位（决定固定模拟增益）：
//   GAIN→VIN     = 3dB    （低，当前接线）
//   GAIN→GND     = 12dB   （中）
//   GAIN 悬空     = 15dB   （高，易拾噪，不推荐）
//   GAIN→VIN经100kΩ = 9dB （可选微调档）

#define PIN_I2S_BCLK 38
#define PIN_I2S_LRC  39
#define PIN_I2S_DOUT 40

// 采样率与位深：网易云 MP3 解码后统一重采样到这组参数
#define I2S_SAMPLE_RATE   44100
#define I2S_BITS_PER_SAMPLE 16

// 使用的 I2S 外设编号（S3 有 I2S0 / I2S1，取 0）
#define I2S_PORT_NUM 0
