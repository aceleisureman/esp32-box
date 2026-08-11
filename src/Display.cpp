#include "Display.h"
#include "Spectrum.h"
#include "WifiProvisioning.h"
#include "MusicService.h"
#include "AudioPlayer.h"
#include "VoiceAssistant.h"
#include "pins_display.h"
#include "CjkFont.h"

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <qrcode.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#if defined(DISP_DRIVER_ILI9341)
  #include <Adafruit_ILI9341.h>
  static Adafruit_ILI9341 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
  #define C_RED    ILI9341_RED
  #define C_YELLOW ILI9341_YELLOW
#else
  #include <Adafruit_ST7789.h>
  static Adafruit_ST7789 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
  #define C_RED    ST77XX_RED
  #define C_YELLOW ST77XX_YELLOW
#endif

static constexpr int16_t  kScrW     = 320;

// WiFi 配网页固定为 320×240：左侧二维码白卡垂直居中，右侧为无边框信息列，
// 底部一行状态 + 一行操作。QR 回调和页面布局共用这组坐标。
static constexpr int16_t NETWORK_QR_BOX_X = 21;
static constexpr int16_t NETWORK_QR_BOX_Y = 45;
static constexpr int16_t NETWORK_QR_BOX    = 122;

static constexpr uint16_t SPEC_Y    = 64;
static constexpr uint16_t SPEC_H    = 110;
static constexpr uint8_t  BARS      = 16;
static constexpr uint16_t BAR_W     = 16;
static constexpr uint16_t BAR_GAP   = 4;
static constexpr uint16_t BAR_TOTAL = BARS * (BAR_W + BAR_GAP) - BAR_GAP;
static constexpr uint16_t kBarX0    = (kScrW - BAR_TOTAL) / 2;
static constexpr uint16_t BAR_BASE  = SPEC_Y + SPEC_H - 2;
static constexpr uint16_t BAR_MAX_H = SPEC_H - 8;
static constexpr uint8_t  MENU_COLUMNS = 4;
static constexpr uint8_t  MENU_VISIBLE_ROWS = 1;
static const char *const MENU_APP_NAMES[] = {"音乐", "听书", "设置"};
static constexpr uint8_t MENU_APP_COUNT = sizeof(MENU_APP_NAMES) / sizeof(MENU_APP_NAMES[0]);
static constexpr const char *APP_VERSION = "DEV BUILD";
static constexpr int16_t MENU_GRID_X = 6;
static constexpr int16_t MENU_GRID_Y = 48;
static constexpr int16_t MENU_CELL_W = 74;
static constexpr int16_t MENU_CELL_H = 90;
static constexpr int16_t MENU_CELL_GAP_X = 4;
static constexpr int16_t MENU_CELL_GAP_Y = 0;
static constexpr int16_t MENU_ICON_SIZE = 50;
static constexpr uint32_t MENU_SELECTION_ANIM_MS = 160;
static constexpr uint32_t MENU_SELECTION_HIGHLIGHT_MS = 42;

DisplayClass Display;

// RGB565 颜色混合，用于天气趋势图的边缘像素抗锯齿。
// 线条只绘制在趋势卡片内，因此以卡片底色作为混合背景即可。
static uint16_t blendRgb565(uint16_t background, uint16_t foreground,
                            uint8_t alpha) {
    if (alpha == 0) return background;
    if (alpha >= 255) return foreground;

    const uint16_t inverse = (uint16_t)(255U - alpha);
    const uint16_t red = (uint16_t)(((((background >> 11) & 0x1FU) * inverse) +
                                     (((foreground >> 11) & 0x1FU) * alpha) +
                                     127U) / 255U);
    const uint16_t green = (uint16_t)(((((background >> 5) & 0x3FU) * inverse) +
                                       (((foreground >> 5) & 0x3FU) * alpha) +
                                       127U) / 255U);
    const uint16_t blue = (uint16_t)((((background & 0x1FU) * inverse) +
                                      ((foreground & 0x1FU) * alpha) +
                                      127U) / 255U);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static void drawAaPixel(Adafruit_GFX &gfx, int16_t x, int16_t y,
                        uint16_t foreground, float coverage,
                        uint16_t background, int16_t clipLeft,
                        int16_t clipTop, int16_t clipRight,
                        int16_t clipBottom) {
    if (coverage <= 0.0f || x < clipLeft || x > clipRight ||
        y < clipTop || y > clipBottom) {
        return;
    }
    if (coverage > 1.0f) coverage = 1.0f;
    const uint8_t alpha = (uint8_t)(coverage * 255.0f + 0.5f);
    if (alpha == 0) return;
    gfx.writePixel(x, y, blendRgb565(background, foreground, alpha));
}

static float catmullRom(float p0, float p1, float p2, float p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * (2.0f * p1 + (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

static void drawSmoothAaCurve(Adafruit_GFX &gfx,
                              const int16_t *pointX,
                              const int16_t *pointY, uint8_t count,
                              uint16_t foreground,
                              uint16_t backgroundAbove,
                              uint16_t backgroundBelow,
                              int16_t clipLeft, int16_t clipTop,
                              int16_t clipRight, int16_t clipBottom) {
    if (count < 2) return;

    // 每个横向像素采样一次 Catmull-Rom，并绘制约 1.5 px 的抗锯齿笔触。
    // 上下背景可分别指定，曲线压在温差带边缘时不会出现黑色锯齿边。
    constexpr float kStrokeHalfWidth = 0.78f;
    gfx.startWrite();
    for (uint8_t i = 0; i + 1 < count; i++) {
        const float p0 = (float)pointY[i == 0 ? 0 : i - 1];
        const float p1 = (float)pointY[i];
        const float p2 = (float)pointY[i + 1];
        const float p3 = (float)pointY[i + 2 < count ? i + 2 : i + 1];
        const int16_t segmentWidth = pointX[i + 1] - pointX[i];
        if (segmentWidth <= 0) continue;
        const int16_t firstX = pointX[i] + (i == 0 ? 0 : 1);

        for (int16_t x = firstX; x <= pointX[i + 1]; x++) {
            const float t = (float)(x - pointX[i]) / (float)segmentWidth;
            float curveY = catmullRom(p0, p1, p2, p3, t);
            if (curveY < clipTop) curveY = (float)clipTop;
            if (curveY > clipBottom) curveY = (float)clipBottom;

            const int16_t firstY = (int16_t)floorf(curveY) - 1;
            for (int16_t y = firstY; y <= firstY + 3; y++) {
                const float distance = fabsf((float)y - curveY);
                const float coverage = kStrokeHalfWidth + 0.5f - distance;
                const uint16_t background = (float)y < curveY
                    ? backgroundAbove : backgroundBelow;
                drawAaPixel(gfx, x, y, foreground, coverage, background,
                            clipLeft, clipTop, clipRight, clipBottom);
            }
        }
    }
    gfx.endWrite();
}

static void drawSmoothRangeBand(Adafruit_GFX &gfx,
                                const int16_t *pointX,
                                const int16_t *upperY,
                                const int16_t *lowerY, uint8_t count,
                                uint16_t color, int16_t clipTop,
                                int16_t clipBottom) {
    if (count < 2) return;

    gfx.startWrite();
    for (uint8_t i = 0; i + 1 < count; i++) {
        const float upper0 = (float)upperY[i == 0 ? 0 : i - 1];
        const float upper1 = (float)upperY[i];
        const float upper2 = (float)upperY[i + 1];
        const float upper3 = (float)upperY[i + 2 < count ? i + 2 : i + 1];
        const float lower0 = (float)lowerY[i == 0 ? 0 : i - 1];
        const float lower1 = (float)lowerY[i];
        const float lower2 = (float)lowerY[i + 1];
        const float lower3 = (float)lowerY[i + 2 < count ? i + 2 : i + 1];
        const int16_t segmentWidth = pointX[i + 1] - pointX[i];
        if (segmentWidth <= 0) continue;
        const int16_t firstX = pointX[i] + (i == 0 ? 0 : 1);

        for (int16_t x = firstX; x <= pointX[i + 1]; x++) {
            const float t = (float)(x - pointX[i]) / (float)segmentWidth;
            float top = catmullRom(upper0, upper1, upper2, upper3, t);
            float bottom = catmullRom(lower0, lower1, lower2, lower3, t);
            if (top > bottom) {
                const float swap = top;
                top = bottom;
                bottom = swap;
            }
            if (top < clipTop) top = (float)clipTop;
            if (bottom > clipBottom) bottom = (float)clipBottom;

            const int16_t y0 = (int16_t)ceilf(top);
            const int16_t y1 = (int16_t)floorf(bottom);
            if (y1 >= y0) gfx.writeFastVLine(x, y0, y1 - y0 + 1, color);
        }
    }
    gfx.endWrite();
}

static void drawProvisioningQr(esp_qrcode_handle_t qr) {
    const int size = esp_qrcode_get_size(qr);
    if (size <= 0) return;
    constexpr int16_t qrBoxX = NETWORK_QR_BOX_X;
    constexpr int16_t qrBoxY = NETWORK_QR_BOX_Y;
    constexpr int16_t qrBox = NETWORK_QR_BOX;
    const int16_t scale = (int16_t)(qrBox / (size + 8));
    if (scale < 1) return;
    const int16_t drawn = (int16_t)((size + 8) * scale);
    const int16_t x0 = qrBoxX + (qrBox - drawn) / 2;
    const int16_t y0 = qrBoxY + (qrBox - drawn) / 2;
    tft.fillRect(qrBoxX, qrBoxY, qrBox, qrBox, 0xFFFF);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qr, x, y)) {
                tft.fillRect(x0 + (x + 4) * scale, y0 + (y + 4) * scale,
                             scale, scale, 0x0000);
            }
        }
    }
}

static uint8_t daysInMonth(uint16_t year, uint8_t month) {
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return (month == 4 || month == 6 || month == 9 || month == 11) ? 30 : 31;
}

static uint8_t weekdayFromDate(uint16_t year, uint8_t month, uint8_t day) {
    // Sakamoto algorithm: 0=SUN ... 6=SAT.
    static const uint8_t offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    uint16_t y = year;
    if (month < 3) --y;
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 +
                      offsets[month - 1] + day) % 7);
}

static uint8_t monthFromName(const char *name) {
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (uint8_t i = 0; i < 12; i++) {
        if (strncmp(name, months[i], 3) == 0) return i + 1;
    }
    return 0;
}

// 天气图标 → 中文短描述（点阵字库覆盖的字符集内）
static const char *weatherIconLabelCn(WeatherIcon icon) {
    switch (icon) {
        case WeatherIcon::ClearDay:    return "晴";
        case WeatherIcon::ClearNight:  return "晴";
        case WeatherIcon::PartlyDay:   return "多云";
        case WeatherIcon::PartlyNight: return "多云";
        case WeatherIcon::Cloudy:      return "阴";
        case WeatherIcon::Fog:         return "雾";
        case WeatherIcon::Drizzle:     return "毛毛雨";
        case WeatherIcon::Rain:        return "雨";
        case WeatherIcon::Showers:     return "阵雨";
        case WeatherIcon::Snow:        return "雪";
        case WeatherIcon::Thunder:     return "雷";
        default:                       return "--";
    }
}

static const GFXfont *uiFontForSize(uint8_t size) {
    switch (size) {
        case 1: return &FreeSans9pt7b;
        case 2: return &FreeSansBold12pt7b;
        case 3: return &FreeSansBold18pt7b;
        case 4: return &FreeSansBold24pt7b;
        default: return nullptr;
    }
}

void DisplayClass::drawText(int16_t x, int16_t y, const char *s,
                            uint8_t size, uint16_t fg, uint16_t bg) {
    const GFXfont *font = uiFontForSize(size);

    tft.setFont(font);
    tft.setTextSize(font ? 1 : size);
    tft.setTextColor(fg, bg);
    if (font) {
        int16_t bx = 0;
        int16_t by = 0;
        uint16_t bw = 0;
        uint16_t bh = 0;
        tft.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
        tft.setCursor(x - bx, y - by);
    } else {
        tft.setCursor(x, y);
    }
    tft.print(s);
}

int16_t DisplayClass::measureTextWidth(const char *s, uint8_t size) {
    const GFXfont *font = uiFontForSize(size);
    if (!font) return (int16_t)(strlen(s) * 6 * size);

    tft.setFont(font);
    tft.setTextSize(1);
    int16_t bx = 0;
    int16_t by = 0;
    uint16_t bw = 0;
    uint16_t bh = 0;
    tft.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
    tft.setFont(nullptr);
    return (int16_t)bw;
}

// ASCII 字符在 GFXfont 下的步进宽度。
// 注意不能用 getTextBounds 的返回值——那是墨迹宽度（字形实际着色范围），
// 窄字母如 i/l 只有 1~2px，逐字累加会让整个单词挤成一团。
// 正确的前进量是字形的 xAdvance。
static int16_t asciiAdvance(const GFXfont *font, uint8_t size, char c) {
    if (!font) return 6 * size;
    const uint8_t first = pgm_read_byte(&font->first);
    const uint8_t last  = pgm_read_byte(&font->last);
    const uint8_t ch    = (uint8_t)c;
    if (ch < first || ch > last) {
        // 字体没有该字形（含空格未定义的情况）：按半角宽度兜底
        return (int16_t)(5 + 3 * size);
    }
    const GFXglyph *glyph =
        &(((GFXglyph *)pgm_read_ptr(&font->glyph))[ch - first]);
    return (int16_t)pgm_read_byte(&glyph->xAdvance);
}

// 中文混排宽度：CJK 16px 基准，size=1→16px，每级 +8px；ASCII 走系统字体
int16_t DisplayClass::measureCjkText(const char *s, uint8_t size) {
    if (!s) return 0;
    const int16_t unit = 16 * size;   // 与点阵实绘 col*size 保持一致
    int16_t total = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80) {
            cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            cp = 0xFFFD;   // 非法字节
            p++;
        }
        if (cp < 0x80) {
            total += asciiAdvance(uiFontForSize(size), size, (char)cp);
        } else {
            total += unit;   // 缺字也按全角占位
        }
    }
    return total;
}

// UTF-8 中文绘制：命中字库→16x16 点阵；ASCII→系统字体；缺字→空格占位
void DisplayClass::drawCjkText(int16_t x, int16_t y, const char *s,
                               uint8_t size, uint16_t fg, uint16_t bg) {
    if (!s) return;
    const int16_t unit = 16 * size;   // 与点阵实绘 col*size 保持一致
    const GFXfont *font = uiFontForSize(size);
    // 整串共用一条基线：逐字用 getTextBounds 定位会让 o/g/T 上下参差。
    // CJK 点阵从 y 顶部起画高 unit，让 ASCII 基线落在同一视觉行内。
    const int16_t baseline = y + unit - 3;
    int16_t cx = x;
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80) {
            cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            cp = 0xFFFD;
            p++;
        }

        if (cp < 0x80) {
            const int16_t adv = asciiAdvance(font, size, (char)cp);
            if (cp != ' ') {
                if (font) {
                    tft.setFont(font);
                    tft.setTextSize(1);
                    tft.setTextColor(fg, bg);
                    tft.setCursor(cx, baseline);
                    tft.write((uint8_t)cp);
                    tft.setFont(nullptr);
                } else {
                    tft.setFont(nullptr);
                    tft.setTextSize(size);
                    tft.setTextColor(fg, bg);
                    tft.setCursor(cx, y);
                    tft.write((uint8_t)cp);
                }
            }
            cx += adv;
            continue;
        }

        const uint8_t *cov = CjkFont.glyph((uint16_t)cp);
        if (cov) {
            // 4bpp 抗锯齿：逐像素按覆盖率在前景与背景之间混色。
            // 必须走批量 drawRGBBitmap：逐点 fillRect 每像素一次 SPI 事务，
            // size=2 时单字就是 1024 次调用，会长时间占住 SPI 总线，
            // 把音频解码任务饿死导致 I2S 欠载爆音。
            if (size == 1) {
                static uint16_t glyphBuf[16 * 16];
                for (uint8_t row = 0; row < 16; row++) {
                    for (uint8_t col = 0; col < 16; col++) {
                        const uint8_t packed = cov[row * 8 + col / 2];
                        const uint8_t c4 = (col & 1) ? (packed & 0x0F)
                                                     : (packed >> 4);
                        glyphBuf[row * 16 + col] = c4 == 0  ? bg
                                                 : c4 >= 15 ? fg
                                                 : blendRgb565(bg, fg,
                                                       (uint8_t)((c4 * 255 + 7) / 15));
                    }
                }
                tft.drawRGBBitmap(cx, y, glyphBuf, 16, 16);
            } else if (size == 2) {
                // 32x32：优先用原生 32px 字模。16px 按 2x2 块放大不增加
                // 任何细节，只把边缘阶梯一并放大——又糊又有锯齿。
                // 原生字模有真实笔画层次与抗锯齿过渡。
                static uint16_t glyphBuf2[32 * 32];
                const uint8_t *big = CjkFont32.glyph((uint16_t)cp);
                if (big) {
                    for (uint8_t row = 0; row < 32; row++) {
                        for (uint8_t col = 0; col < 32; col++) {
                            const uint8_t packed = big[row * 16 + col / 2];
                            const uint8_t c4 = (col & 1) ? (packed & 0x0F)
                                                         : (packed >> 4);
                            glyphBuf2[row * 32 + col] =
                                c4 == 0  ? bg
                              : c4 >= 15 ? fg
                              : blendRgb565(bg, fg,
                                    (uint8_t)((c4 * 255 + 7) / 15));
                        }
                    }
                } else {
                    // 32px 字库缺字或未就绪：退回 2x2 放大
                    for (uint8_t row = 0; row < 16; row++) {
                        for (uint8_t col = 0; col < 16; col++) {
                            const uint8_t packed = cov[row * 8 + col / 2];
                            const uint8_t c4 = (col & 1) ? (packed & 0x0F)
                                                         : (packed >> 4);
                            const uint16_t c = c4 == 0  ? bg
                                             : c4 >= 15 ? fg
                                             : blendRgb565(bg, fg,
                                                   (uint8_t)((c4 * 255 + 7) / 15));
                            const uint16_t base =
                                (uint16_t)(row * 2) * 32 + col * 2;
                            glyphBuf2[base]          = c;
                            glyphBuf2[base + 1]      = c;
                            glyphBuf2[base + 32]     = c;
                            glyphBuf2[base + 32 + 1] = c;
                        }
                    }
                }
                tft.drawRGBBitmap(cx, y, glyphBuf2, 32, 32);
            } else {
                for (uint8_t row = 0; row < 16; row++) {
                    for (uint8_t col = 0; col < 16; col++) {
                        const uint8_t packed = cov[row * 8 + col / 2];
                        const uint8_t c4 = (col & 1) ? (packed & 0x0F)
                                                     : (packed >> 4);
                        if (c4 == 0) continue;
                        const uint16_t c = c4 >= 15 ? fg
                            : blendRgb565(bg, fg, (uint8_t)((c4 * 255 + 7) / 15));
                        tft.fillRect(cx + col * size, y + row * size,
                                     size, size, c);
                    }
                }
            }
        } else if (CjkFont.isReady()) {
            tft.fillRect(cx, y, unit, unit, bg);   // 字库里没有该字
        } else {
            // 字库未烧录：画空心框而不是空白，避免误以为是丢字
            tft.fillRect(cx, y, unit, unit, bg);
            tft.drawRect(cx + 1, y + 2, unit - 3, unit - 4, COLOR_LINE);
        }
        cx += unit;
    }
}

void DisplayClass::fillScreenBg() {
    tft.fillScreen(COLOR_BG);
}

void DisplayClass::applyBlPin(bool on) {
#if PIN_TFT_BL >= 0
#if BL_ACTIVE_HIGH
    digitalWrite(PIN_TFT_BL, on ? HIGH : LOW);
#else
    digitalWrite(PIN_TFT_BL, on ? LOW : HIGH);
#endif
#else
    (void)on;
#endif
}

void DisplayClass::setBacklight(bool on) {
    const bool wasOn = _blOn;
    if (_blOn == on && _inited) {
        if (on) _activityMs = millis();
        return;
    }
    _blOn = on;
    applyBlPin(on);
    if (on) {
        _activityMs = millis();
        // 背光唤醒后从当前页重新计时，避免刚亮屏就立刻跳页。
        if (!wasOn && _page == Page::Welcome) _homeViewSinceMs = _activityMs;
    }
    Serial.printf("[DISP] backlight %s (GPIO%d)\n", on ? "ON" : "OFF", PIN_TFT_BL);
    Serial.printf("[CLOCK][%lu] backlight %s page=%u activity=%lu\n",
                  (unsigned long)millis(),
                  on ? "resumed" : "sleep",
                  (unsigned)_page,
                  (unsigned long)_activityMs);
}

void DisplayClass::noteActivity() {
    _activityMs = millis();
    if (!_blOn) setBacklight(true);
}

void DisplayClass::serviceBacklight() {
#if BL_IDLE_OFF_MS > 0
    if (_page != Page::Welcome) return;
    if (!_blOn) return;
    if (millis() - _activityMs < (uint32_t)BL_IDLE_OFF_MS) return;
    setBacklight(false);
#endif
}

void DisplayClass::setWifiConnected(bool on) {
    if (_wifiOn != on) _weatherDashboardDirty = true;
    _wifiOn = on;
    if (_page == Page::Network) {
        _networkMessage = on ? "WiFi connected" : "WiFi service not configured";
    }
}

void DisplayClass::setBatteryPercent(uint8_t pct) {
    if (pct > 100) pct = 100;
    _battPct = pct;
}

void DisplayClass::setNetworkServiceState(bool active, const char *status,
                                           const char *ip, const char *apSsid,
                                           const char *qrPayload) {
    const String nextStatus = status ? status : "";
    const String nextIp = ip ? ip : "192.168.4.1";
    const String nextSsid = apSsid ? apSsid : "MY-SMALL-BOX";
    const String nextQrPayload = qrPayload ? qrPayload : "";
    if (_networkServiceActive == active && _networkServiceStatus == nextStatus &&
        _networkServiceIp == nextIp && _networkServiceSsid == nextSsid &&
        _networkQrPayload == nextQrPayload) {
        return;
    }
    _networkServiceActive = active;
    _networkServiceStatus = nextStatus;
    _networkServiceIp = nextIp;
    _networkServiceSsid = nextSsid;
    _networkQrPayload = nextQrPayload;
    _networkServiceDirty = true;
    Serial.printf("[DISP][%lu][heap=%u] network service state active=%s status=%s ip=%s ssid=%s\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  active ? "true" : "false",
                  _networkServiceStatus.c_str(),
                  _networkServiceIp.c_str(),
                  _networkServiceSsid.c_str());
}

void DisplayClass::setTemperatureC(int8_t celsius) {
    if (celsius < -50 || celsius > 80) return;
    _temperatureC = celsius;
}

void DisplayClass::setWeather(const WeatherClass::Snapshot &weather) {
    _weatherSnapshot = weather;
    _weatherValid = weather.valid;
    _weatherIcon = weather.valid ? weather.icon : WeatherIcon::Unknown;
    _weatherText = weather.valid ? weather.condition : "";
    _weatherCity = weather.valid ? weather.city : "";
    _temperatureC = weather.valid ? weather.tempC : -128;
    _weatherDashboardDirty = true;
}

void DisplayClass::setClock(uint8_t hour, uint8_t minute, uint8_t second) {
    const uint8_t oldH = _clockH;
    const uint8_t oldM = _clockM;
    const uint8_t oldS = _clockS;
    if (hour < 24) _clockH = hour;
    if (minute < 60) _clockM = minute;
    if (second < 60) _clockS = second;
    _clockTick = millis();
    Serial.printf("[CLOCK][%lu] setClock requested=%02u:%02u:%02u applied=%02u:%02u:%02u old=%02u:%02u:%02u\n",
                  (unsigned long)_clockTick,
                  (unsigned)hour, (unsigned)minute, (unsigned)second,
                  (unsigned)_clockH, (unsigned)_clockM, (unsigned)_clockS,
                  (unsigned)oldH, (unsigned)oldM, (unsigned)oldS);
}

void DisplayClass::setDate(uint16_t year, uint8_t month, uint8_t day) {
    if (year < 2000 || month < 1 || month > 12) return;
    if (day < 1 || day > daysInMonth(year, month)) return;
    _clockY = year;
    _clockMo = month;
    _clockD = day;
}

void DisplayClass::enterNetworkSettings() {
    Serial.printf("[DISP][%lu][heap=%u] enterNetworkSettings: before page=%u\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)_page);
    noteActivity();
    showNetworkSettings(true);
    setBacklight(true);
    Serial.printf("[DISP][%lu][heap=%u] enterNetworkSettings: after page=%u\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)_page);
}

void DisplayClass::exitNetworkSettings() {
    if (_page != Page::Network) return;
    showSettingsPage(true);
}

// ==================== WiFi 网络列表页 ====================
// 列出已保存的网络，上下选择，确定执行当前模式操作：
//   - 切换模式：连接选中的网络
//   - 删除模式：移除选中的网络
// 左右键在两种模式间切换；当前连接的网络标记 ●。
void DisplayClass::showWifiList(bool force) {
    if (!force && _page == Page::WifiList) return;
    _page = Page::WifiList;
    _wifiListSelection = 0;
    _lastWifiListSelection = 0xFF;
    _wifiListModeSwitch = true;
    _wifiListDirty = true;
    fillScreenBg();
    drawStatusBar(true);
    updateWifiList(true);
}

void DisplayClass::wifiListMove(int8_t delta) {
    if (_page != Page::WifiList) return;
    const int16_t total = (int16_t)WifiProvisioning.networkCount();
    if (total <= 0) return;
    int16_t next = (int16_t)_wifiListSelection + delta;
    if (next < 0) next = total - 1;
    if (next >= total) next = 0;
    _wifiListSelection = (uint8_t)next;
    noteActivity();
}

void DisplayClass::wifiListToggleMode() {
    if (_page != Page::WifiList) return;
    _wifiListModeSwitch = !_wifiListModeSwitch;
    noteActivity();
}

void DisplayClass::wifiListActivate() {
    if (_page != Page::WifiList) return;
    const uint8_t total = WifiProvisioning.networkCount();
    if (_wifiListSelection >= total) return;
    noteActivity();

    if (_wifiListModeSwitch) {
        // 切换：连接选中网络
        WifiProvisioning.selectNetwork(_wifiListSelection);
        _wifiListDirty = true;
    } else {
        // 删除
        WifiProvisioning.removeNetwork(_wifiListSelection);
        if (_wifiListSelection >= WifiProvisioning.networkCount() &&
            WifiProvisioning.networkCount() > 0) {
            _wifiListSelection = WifiProvisioning.networkCount() - 1;
        }
        _wifiListDirty = true;
    }
}

void DisplayClass::updateWifiList(bool force) {
    const uint8_t total = WifiProvisioning.networkCount();
    const String current = WifiProvisioning.currentSsid();

    const bool changed = force || _wifiListDirty ||
                         total != _wifiListCount ||
                         current != _wifiListCurrent ||
                         _wifiListSelection != _lastWifiListSelection ||
                         _wifiListModeSwitch != _lastWifiListMode;
    if (!changed) return;
    _wifiListDirty = false;
    _wifiListCount = total;
    _wifiListCurrent = current;
    _lastWifiListSelection = _wifiListSelection;
    _lastWifiListMode = _wifiListModeSwitch;

    // 标题与模式提示
    tft.fillRect(0, STATUS_H + 1, kScrW, 240 - STATUS_H - 1, COLOR_BG);
    const char *title = "WiFi 网络";
    const int16_t tw = measureCjkText(title, 1);
    drawCjkText((kScrW - tw) / 2, 36, title, 1, COLOR_ACCENT, COLOR_BG);

    const char *mode = _wifiListModeSwitch ? "确定:切换  右:删除"
                                           : "确定:删除  右:切换";
    const int16_t mw = measureCjkText(mode, 1);
    drawCjkText((kScrW - mw) / 2, 222, mode, 1, COLOR_DIM, COLOR_BG);

    if (total == 0) {
        tft.fillRect(0, 60, kScrW, 140, COLOR_BG);
        const char *tip = "请先在配网页添加网络";
        const int16_t tw2 = measureCjkText(tip, 1);
        drawCjkText((kScrW - tw2) / 2, 130, tip, 1, COLOR_DIM, COLOR_BG);
        return;
    }

    // 网络列表行：每行 34px，最多显示 4 行
    constexpr int16_t rowY0 = 58;
    constexpr int16_t rowH = 34;
    constexpr uint8_t maxRows = 4;
    const uint8_t startRow = _wifiListSelection >= maxRows
                                 ? _wifiListSelection - maxRows + 1 : 0;
    for (uint8_t row = 0; row < maxRows; row++) {
        const uint8_t idx = startRow + row;
        if (idx >= total) break;
        const int16_t y = rowY0 + row * rowH;

        String ssid;
        String pass;
        WifiProvisioning.networkAt(idx, ssid, pass);
        const bool isSel = idx == _wifiListSelection;
        const bool isCur = ssid == current;

        tft.fillRect(0, y, kScrW, rowH - 2, COLOR_BG);
        if (isCur) {
            // 当前连接：左侧绿色圆点
            tft.fillCircle(18, y + rowH / 2, 3, COLOR_GREEN);
        }
        const uint16_t fg = isCur ? COLOR_GREEN
                           : (isSel ? COLOR_ACCENT : COLOR_TEXT);
        drawCjkTextClipped(30, y + 8, ssid.c_str(), 1, fg, COLOR_BG,
                           kScrW - 80);
        tft.drawFastHLine(16, y + rowH - 2, kScrW - 32, COLOR_LINE);

        if (isSel) {
            // 选中行左侧竖条
            tft.fillRect(6, y + 4, 3, rowH - 8, COLOR_ACCENT);
        }
    }
}

void DisplayClass::settingsMove(int8_t delta) {
    if (_page != Page::Settings) return;
    constexpr uint8_t optionCount = 3;
    int16_t next = (int16_t)_settingsSelection + delta;
    if (next < 0) next = optionCount - 1;
    if (next >= optionCount) next = 0;
    _settingsSelection = (uint8_t)next;
    noteActivity();
}

void DisplayClass::settingsActivate() {
    if (_page != Page::Settings) return;
    noteActivity();
    if (_settingsSelection == 0) showNetworkSettings(true);
    else if (_settingsSelection == 1) showWifiList(true);
    else showVersionPage(true);
}

void DisplayClass::networkMove(int8_t delta) {
    if (_page != Page::Network) return;
    constexpr uint8_t optionCount = 2;
    int16_t next = (int16_t)_networkSelection + delta;
    if (next < 0) next = optionCount - 1;
    if (next >= optionCount) next = 0;
    _networkSelection = (uint8_t)next;
    noteActivity();
}

void DisplayClass::networkActivate() {
    if (_page != Page::Network) return;
    Serial.printf("[DISP][%lu][heap=%u] networkActivate: selection=%u\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)_networkSelection);
    noteActivity();
    if (_networkSelection == 1) {
        exitNetworkSettings();
        return;
    }
    _networkMessage = "Scanning nearby WiFi";
    _networkRescanRequested = true;
    _networkServiceDirty = true;
}

bool DisplayClass::takeNetworkRescanRequest() {
    const bool requested = _networkRescanRequested;
    _networkRescanRequested = false;
    return requested;
}

void DisplayClass::handleJoystick(JoystickEvent event) {
    noteActivity();

    if (_page == Page::Welcome) {
        // 首页左右键可立即切换；按下/上下仍进入应用菜单。
        if (event == JoystickEvent::Left || event == JoystickEvent::Right) {
            const HomeView next = _homeView == HomeView::Clock
                ? HomeView::Weather : HomeView::Clock;
            showHomeView(next, true);
            return;
        }
        _menuSelection = 0;
        _menuRowOffset = 0;
        showMenu(true);
        return;
    }

    if (_page == Page::Network) {
        if (event == JoystickEvent::Up) networkMove(-1);
        else if (event == JoystickEvent::Down) networkMove(1);
        else if (event == JoystickEvent::Left) exitNetworkSettings();
        else if (event == JoystickEvent::Press) networkActivate();
        return;
    }

    if (_page == Page::Settings) {
        if (event == JoystickEvent::Up) settingsMove(-1);
        else if (event == JoystickEvent::Down) settingsMove(1);
        else if (event == JoystickEvent::Left) showMenu(true);
        else if (event == JoystickEvent::Press) settingsActivate();
        return;
    }

    if (_page == Page::Version) {
        if (event == JoystickEvent::Left || event == JoystickEvent::Press) {
            showSettingsPage(true);
        }
        return;
    }

    if (_page == Page::WifiList) {
        // 上下选择网络；左右切换「切换/删除」模式；确定执行；左返回设置页
        if (event == JoystickEvent::Up) wifiListMove(-1);
        else if (event == JoystickEvent::Down) wifiListMove(1);
        else if (event == JoystickEvent::Left) showSettingsPage(true);
        else if (event == JoystickEvent::Right) wifiListToggleMode();
        else if (event == JoystickEvent::Press) wifiListActivate();
        return;
    }

    if (_page == Page::Books) {
        // 上下选择；确定：简介/列表切换；左返回应用菜单
        if (event == JoystickEvent::Up) booksMove(-1);
        else if (event == JoystickEvent::Down) booksMove(1);
        else if (event == JoystickEvent::Left) {
            if (_showBookDetail) _showBookDetail = false;
            else showMenu(true);
        } else if (event == JoystickEvent::Press) booksActivate();
        return;
    }

    if (_page == Page::Music) {
        // 左右切子页；第一页再按左返回应用菜单
        if (event == JoystickEvent::Right) {
            const uint8_t next = ((uint8_t)_musicView + 1) % 3;
            showMusicView((MusicView)next, false);
        } else if (event == JoystickEvent::Left) {
            if (_musicView == MusicView::NowPlaying) {
                showMenu(true);
            } else {
                showMusicView((MusicView)((uint8_t)_musicView - 1), false);
            }
        } else if (event == JoystickEvent::Up || event == JoystickEvent::Down) {
            // 频谱页：上下调音量（音量条就在本页）；其余页：上下切歌
            if (_musicView == MusicView::Spectrum) {
                // 音量调节节流：摇杆连发 140ms 一次，这里再压到 150ms，
                // 并靠 Input 层的滞回兜底，防止 ADC 毛刺让音量自己跳
                const uint32_t now = millis();
                if (now - _volAdjustMs >= 150) {
                    _volAdjustMs = now;
                    if (event == JoystickEvent::Up) AudioPlayer.volumeUp();
                    else AudioPlayer.volumeDown();
                }
            } else if (event == JoystickEvent::Up) {
                MusicService.previous();
            } else {
                MusicService.next();
            }
        } else if (event == JoystickEvent::Press) {
            AudioPlayer.togglePause();
        }
        return;
    }

    if (_page == Page::Voice) {
        // 语音对话页：左/右键或按下都退出会话
        if (event == JoystickEvent::Left ||
            event == JoystickEvent::Right ||
            event == JoystickEvent::Press) {
            VoiceAssistant.disableAndRestoreMusic();
        }
        return;
    }

    if (_page == Page::NeteaseCloud) {
        if (event == JoystickEvent::Left) showMenu(true);
        return;
    }

    if (_page != Page::Menu) return;

    uint8_t next = _menuSelection;
    switch (event) {
        case JoystickEvent::Up:
            if (next >= MENU_COLUMNS) next -= MENU_COLUMNS;
            break;
        case JoystickEvent::Down:
            if (next / MENU_COLUMNS + 1 < (MENU_APP_COUNT + MENU_COLUMNS - 1) / MENU_COLUMNS) {
                next += MENU_COLUMNS;
                if (next >= MENU_APP_COUNT) next = MENU_APP_COUNT - 1;
            }
            break;
        case JoystickEvent::Left:
            if (next == 0) {
                _page = Page::None;
                return;
            }
            --next;
            break;
        case JoystickEvent::Right:
            if (next + 1 < MENU_APP_COUNT) ++next;
            break;
        case JoystickEvent::Press:
            if (next == 0) showMusicPlayer(true);
            else if (next == 1) showBooks(true);
            else if (next == 2) showSettingsPage(true);
            return;
    }

    _menuSelection = next;
    const uint8_t selectedRow = _menuSelection / MENU_COLUMNS;
    if (selectedRow < _menuRowOffset) {
        _menuRowOffset = selectedRow;
    } else if (selectedRow >= _menuRowOffset + MENU_VISIBLE_ROWS) {
        _menuRowOffset = selectedRow - MENU_VISIBLE_ROWS + 1;
    }
}

void DisplayClass::tickClock() {
    // 无 RTC 时用软件走时（秒级，从 setClock / 默认 12:30:00 起）
    uint32_t now = millis();
    if (now - _clockTick < 1000) return;
    const uint8_t oldH = _clockH;
    const uint8_t oldM = _clockM;
    const uint8_t oldS = _clockS;
    uint32_t secs = (now - _clockTick) / 1000;
    _clockTick += secs * 1000UL;
    uint32_t total = (uint32_t)_clockH * 3600UL + (uint32_t)_clockM * 60UL + _clockS + secs;
    uint32_t elapsedDays = total / 86400UL;
    total %= 86400UL;
    _clockH = (uint8_t)(total / 3600UL);
    _clockM = (uint8_t)((total % 3600UL) / 60UL);
    _clockS = (uint8_t)(total % 60UL);

    while (elapsedDays-- > 0) {
        if (++_clockD > daysInMonth(_clockY, _clockMo)) {
            _clockD = 1;
            if (++_clockMo > 12) {
                _clockMo = 1;
                ++_clockY;
            }
        }
    }

    Serial.printf("[CLOCK][%lu] tick elapsed=%lus old=%02u:%02u:%02u new=%02u:%02u:%02u next=%lu page=%u bl=%u anim=%u\n",
                  (unsigned long)now,
                  (unsigned long)secs,
                  (unsigned)oldH, (unsigned)oldM, (unsigned)oldS,
                  (unsigned)_clockH, (unsigned)_clockM, (unsigned)_clockS,
                  (unsigned long)_clockTick,
                  (unsigned)_page, _blOn ? 1U : 0U, (unsigned)_flipAnim);
}

// ---------- 状态栏图标（矢量绘制，18×18 视觉尺寸）----------

// 图标覆盖度 → 颜色。这块 TFT 对低灰压暗极重（见 COLOR_CARD 等处注释），
// 线性 alpha 混合的软边像素会被压得几乎不可见，细笔画看起来又灰又糊。
// 这里对覆盖度做 gamma 提亮（cov^0.45，近似 sRGB 反 gamma），让边缘
// 保��应有的亮度，观感才接近 Retina 上的 SF Symbols。
static uint16_t iconBlend(uint16_t bg, uint16_t fg, float cov) {
    if (cov <= 0.0f) return bg;
    if (cov >= 1.0f) return fg;
    const float boosted = powf(cov, 0.45f);
    return blendRgb565(bg, fg, (uint8_t)(boosted * 255.0f + 0.5f));
}

// SF Symbols "wifi" 风格：三层同心实心圆弧 + 底部圆点。
//
// 逐像素渲染进 18×18 缓冲后一次性送屏，原因有二：
//   ① 严格不越界——圆弧半径大于图标框，用"画圆再裁切"会溢出到相邻
//      图标区；按像素判定则天然被缓冲边界限制。
//   ② 边缘抗锯齿——弧线在这个尺度上锯齿很显眼，按到弧心距离做
//      带 gamma 校正的软边过渡才有 SF Symbols 的顺滑感。
void DisplayClass::drawWifiIcon(int16_t x, int16_t y, bool on) {
    // iOS 语义：连接 = 白色，未连接 = 灰显。未连接色不能太暗：
    // 这块屏上 #48484A 几乎与黑背景不可分，用 #8E8E93 才看得出"存在但未连"。
    const uint16_t c = on ? COLOR_TEXT : COLOR_DIM;

    static uint16_t buf[kStatusIcon * kStatusIcon];
    // 弧心在图标下方中央；三层弧半径 3.6 / 7.4 / 11.2，线宽 2.6px
    constexpr float kCx = 8.5f, kCy = 15.0f;
    constexpr float kBands[3] = {3.6f, 7.4f, 11.2f};
    constexpr float kHalfW = 1.3f;      // 半线宽（加粗，避免细线被压暗）
    // 扇形张角：从竖直方向左右各 50°
    constexpr float kSlope = 1.19f;

    for (int16_t py = 0; py < kStatusIcon; py++) {
        for (int16_t px = 0; px < kStatusIcon; px++) {
            const float dx = (float)px - kCx;
            const float dy = (float)py - kCy;      // 弧在上方，dy 为负
            const float d = sqrtf(dx * dx + dy * dy);
            float cov = 0.0f;
            // 只在弧心上方的扇形张角内绘制
            if (dy < 0.0f && fabsf(dx) <= -dy * kSlope) {
                for (uint8_t b = 0; b < 3; b++) {
                    // 到该层弧中线的距离 → 覆盖度（软边 1px）
                    const float e = kHalfW - fabsf(d - kBands[b]);
                    if (e > 0.0f) {
                        const float a = e > 1.0f ? 1.0f : e;
                        if (a > cov) cov = a;
                    }
                }
            }
            // 底部圆点：半径 1.8 的实心圆
            const float pe = 1.8f - d;
            if (pe > 0.0f) {
                const float a = pe > 1.0f ? 1.0f : pe;
                if (a > cov) cov = a;
            }
            buf[py * kStatusIcon + px] = iconBlend(COLOR_BAR, c, cov);
        }
    }
    tft.drawRGBBitmap(x, y, buf, kStatusIcon, kStatusIcon);
}

// SF Symbols "bluetooth" 风格：标准符文形（竖干 + 上下两个折返三角）。
// 同样逐像素抗锯齿渲染：折线是斜的，直线画法在这个尺寸下毛刺明显。
void DisplayClass::drawBtIconAt(int16_t x, int16_t y, uint16_t color, uint16_t bg) {
    static uint16_t buf[kStatusIcon * kStatusIcon];
    // 符文关键点（图标坐标系，18×18）：竖干 x=8，顶 y=1.6，底 y=16.4，
    // 折返点 (12.4, 5.3) 与 (12.4, 12.7)，交叉点在竖干左侧 (3.8, …)
    constexpr float kStemX = 8.0f, kTopY = 1.6f, kBotY = 16.4f;
    constexpr float kRx = 12.4f, kUpY = 5.3f, kLoY = 12.7f;
    constexpr float kLx = 3.8f;
    constexpr float kHalfW = 1.35f;     // 半线宽（≈2.7px 描边）

    // 点到线段距离
    auto segDist = [](float px, float py, float ax, float ay,
                      float bx, float by) -> float {
        const float vx = bx - ax, vy = by - ay;
        const float wx = px - ax, wy = py - ay;
        const float len2 = vx * vx + vy * vy;
        float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        const float ex = wx - t * vx, ey = wy - t * vy;
        return sqrtf(ex * ex + ey * ey);
    };

    for (int16_t py = 0; py < kStatusIcon; py++) {
        for (int16_t px = 0; px < kStatusIcon; px++) {
            const float fx = (float)px, fy = (float)py;
            // 五段：竖干、右上斜、左上斜、右下斜、左下斜
            float d = segDist(fx, fy, kStemX, kTopY, kStemX, kBotY);
            const float d2 = segDist(fx, fy, kStemX, kTopY, kRx, kUpY);
            const float d3 = segDist(fx, fy, kRx, kUpY, kLx, kLoY);
            const float d4 = segDist(fx, fy, kStemX, kBotY, kRx, kLoY);
            const float d5 = segDist(fx, fy, kRx, kLoY, kLx, kUpY);
            if (d2 < d) d = d2;
            if (d3 < d) d = d3;
            if (d4 < d) d = d4;
            if (d5 < d) d = d5;

            const float e = kHalfW - d;
            const float cov = e <= 0.0f ? 0.0f : (e > 1.0f ? 1.0f : e);
            buf[py * kStatusIcon + px] = iconBlend(bg, color, cov);
        }
    }
    tft.drawRGBBitmap(x, y, buf, kStatusIcon, kStatusIcon);
}

void DisplayClass::drawBtIcon(int16_t x, int16_t y, bool on) {
    // iOS 语义：连接 = 系统蓝，未连接 = 灰显（同 WiFi，用 #8E8E93
    // 而非更深的灰——这块屏上深灰几乎与黑背景不可分）
    drawBtIconAt(x, y, on ? COLOR_ACCENT : COLOR_DIM, COLOR_BAR);
}

// 绘制字形的指定行。Adafruit_GFX 没有裁剪区，使用单色缓冲后按行偏移
// 送入屏幕，保证翻页动画只更新当前半片，不会覆盖另一半数字。
void DisplayClass::drawFlipDigitClip(int16_t x, int16_t y, int16_t w, int16_t h,
                                     char digit, int16_t clipY, int16_t clipH) {
    static GFXcanvas1 canvas(kFlipCardW, kFlipCardH);
    constexpr uint16_t byteWidth = (kFlipCardW + 7) / 8;
    constexpr uint16_t cacheBytes = byteWidth * kFlipCardH;
    static uint8_t digitCache[10][cacheBytes] = {};
    static bool digitCached[10] = {};
    if (w != kFlipCardW || h != kFlipCardH) return;

    const int8_t digitIndex =
        (digit >= '0' && digit <= '9') ? (int8_t)(digit - '0') : -1;
    const uint8_t *buffer = nullptr;
    if (digitIndex >= 0 && digitCached[digitIndex]) {
        buffer = digitCache[digitIndex];
    } else {
        uint8_t *canvasBuffer = canvas.getBuffer();
        if (!canvasBuffer) return;

        canvas.fillScreen(0);
        canvas.setTextWrap(false);
        canvas.setFont(&FreeSansBold24pt7b);
        canvas.setTextSize(1);
        canvas.setTextColor(1);

        char s[2] = { digit, '\0' };
        int16_t bx = 0;
        int16_t by = 0;
        uint16_t bw = 0;
        uint16_t bh = 0;
        canvas.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
        const int16_t tx = (w - (int16_t)bw) / 2 - bx;
        const int16_t ty = (h - (int16_t)bh) / 2 - by;
        canvas.setCursor(tx, ty);
        canvas.print(s);

        if (digitIndex >= 0) {
            memcpy(digitCache[digitIndex], canvasBuffer, cacheBytes);
            digitCached[digitIndex] = true;
            buffer = digitCache[digitIndex];
        } else {
            buffer = canvasBuffer;
        }
    }

    // Adafruit_GFX::drawBitmap() writes every set bit as a separate pixel
    // window. That creates hundreds of SPI address changes during a flip.
    // Build the clipped interior as one RGB565 span and send it in one window.
    constexpr int16_t innerW = kFlipCardW - 2;
    static uint16_t pixelBuffer[innerW * kFlipCardH];
    if (clipY < 0 || clipH <= 0 || clipY + clipH > h) return;

    const uint8_t *bitmap = buffer + clipY * byteWidth;
    const int16_t mid = h / 2;
    uint32_t out = 0;
    for (int16_t row = 0; row < clipH; row++) {
        const uint8_t *src = bitmap + row * byteWidth;
        const uint16_t background = (clipY + row < mid)
                                        ? COLOR_CARD_TOP
                                        : COLOR_CARD;
        for (int16_t col = 0; col < innerW; col++) {
            const int16_t sourceX = col + 1;
            const bool set = (src[sourceX >> 3] & (0x80 >> (sourceX & 7))) != 0;
            pixelBuffer[out++] = set ? COLOR_TEXT : background;
        }
    }

    tft.startWrite();
    tft.setAddrWindow(x + 1, y + clipY, innerW, clipH);
    tft.writePixels(pixelBuffer, out);
    tft.endWrite();
}

// 翻页单卡：上半略亮 + 下半略暗，正面也能分清卡面与纯黑底
void DisplayClass::drawFlipCard(int16_t x, int16_t y, int16_t w, int16_t h, char digit) {
    const int16_t mid = h / 2;

    // 轻微下投影，让卡片与纯黑背景分离
    tft.fillRoundRect(x + 1, y + 2, w, h, 6, COLOR_FLIP_SHADOW);
    tft.fillRoundRect(x, y, w, h, 6, COLOR_CARD);
    // 上半片提亮（翻页立体感 + 提高对比）
    tft.fillRect(x + 1, y + 1, w - 2, mid - 1, COLOR_CARD_TOP);
    // 圆角外框
    tft.drawRoundRect(x, y, w, h, 6, COLOR_CARD_EDGE);
    tft.drawFastVLine(x, y + 4, h - 8, COLOR_CARD_EDGE);
    tft.drawFastVLine(x + w - 1, y + 4, h - 8, COLOR_CARD_EDGE);
    drawFlipDigitClip(x, y, w, h, digit, 0, h);

    // 中缝：暗线 + 亮线，正面更明显
    tft.drawFastHLine(x + 2, mid + y - 1, w - 4, COLOR_FLIP_SHADOW);
    tft.drawFastHLine(x + 2, mid + y,     w - 4, COLOR_FLIP_LINE);
    // 两侧铰链细节，强化真实翻页卡质感
    tft.fillRect(x + 2, mid + y - 2, 3, 2, COLOR_FLIP_SHADOW);
    tft.fillRect(x + w - 5, mid + y - 2, 3, 2, COLOR_FLIP_SHADOW);
}

// 组间冒号：两个方点，垂直居中
void DisplayClass::drawFlipColon(int16_t x, int16_t y, int16_t h) {
    const int16_t dot = 5;
    const int16_t cx = x;
    const int16_t cy1 = y + h / 2 - 13;
    const int16_t cy2 = y + h / 2 + 8;
    tft.fillRoundRect(cx, cy1, dot, dot, 1, COLOR_ACCENT);
    tft.fillRoundRect(cx, cy2, dot, dot, 1, COLOR_ACCENT);
}

// 组下方单位标签（英文缩写，无中文字库）
void DisplayClass::drawFlipUnitLabel(int16_t cx, int16_t y, const char *label) {
    const int16_t tw = measureTextWidth(label, 1);
    drawText(cx - tw / 2, y, label, 1, COLOR_DIM, COLOR_BG);
    tft.drawFastHLine(cx - 12, y - 3, 24, COLOR_FLIP_SHADOW);
}

void DisplayClass::drawWelcomeWeather(bool force) {
    if (!force &&
        _temperatureC == _lastWelcomeTemperatureC &&
        _weatherValid == _lastWeatherValid &&
        _weatherIcon == _lastWeatherIcon &&
        _weatherText == _lastWeatherText &&
        _weatherCity == _lastWeatherCity) {
        return;
    }
    _lastWelcomeTemperatureC = _temperatureC;
    _lastWeatherValid = _weatherValid;
    _lastWeatherIcon = _weatherIcon;
    _lastWeatherText = _weatherText;
    _lastWeatherCity = _weatherCity;

    tft.fillRect(0, STATUS_H + 1, kScrW, kFlipClockY - STATUS_H - 5, COLOR_BG);

    // 离线/未获取时整块灰显
    const bool dimmed = !_weatherValid;
    const uint16_t valueColor  = dimmed ? COLOR_DIM : COLOR_TEXT;
    const uint16_t accentColor = dimmed ? COLOR_DIM : COLOR_ACCENT;

    char value[6];
    if (!_weatherValid || _temperatureC == -128) {
        snprintf(value, sizeof(value), "--");
    } else {
        snprintf(value, sizeof(value), "%d", _temperatureC);
    }

    // 主行：天气图标 + 温度值 + °C，整体水平居中
    constexpr int16_t iconR   = 16;
    constexpr int16_t iconBox = 2 * iconR + 10;   // 图标占位（含云朵外扩）
    constexpr int16_t iconGap = 14;
    const int16_t valueW = measureTextWidth(value, 3);
    const int16_t unitW = measureTextWidth("C", 2);
    constexpr int16_t degreeW = 10;
    constexpr int16_t gap = 4;
    const int16_t totalW = iconBox + iconGap + valueW + gap + degreeW + unitW;
    const int16_t x = (kScrW - totalW) / 2;

    const int16_t rowCy = kWelcomeWeatherY + 13;   // 主行垂直中心
    _weatherIconCx = x + iconBox / 2;
    _weatherIconCy = rowCy;
    drawWeatherIcon(_weatherIconCx, rowCy, iconR, _weatherIcon, dimmed);

    const int16_t valueX = x + iconBox + iconGap;
    drawText(valueX, kWelcomeWeatherY, value, 3, valueColor, COLOR_BG);
    const int16_t degreeX = valueX + valueW + gap + 3;
    tft.drawCircle(degreeX, kWelcomeWeatherY + 4, 3, accentColor);
    drawText(degreeX + 7, kWelcomeWeatherY + 10, "C", 2, accentColor, COLOR_BG);

    // 副行：天气状况（中文，由图标类别映射）· 城市（ASCII）
    char sub[60];
    if (!_weatherValid) {
        snprintf(sub, sizeof(sub), "%s",
                 _wifiOn ? "天气更新中" : "天气离线");
    } else if (_weatherCity.length() > 0) {
        snprintf(sub, sizeof(sub), "%s %s",
                 weatherIconLabelCn(_weatherIcon), _weatherCity.c_str());
    } else {
        snprintf(sub, sizeof(sub), "%s", weatherIconLabelCn(_weatherIcon));
    }
    const int16_t subW = measureCjkText(sub, 1);
    drawCjkText((kScrW - subW) / 2, kWelcomeWeatherY + 40, sub, 1,
                COLOR_DIM, COLOR_BG);
}

// ---- 天气图标绘制（矢量小图标，RGB565 直绘） ----

void DisplayClass::drawWeatherSun(int16_t cx, int16_t cy, int16_t r,
                                  uint16_t color, bool rays) {
    tft.fillCircle(cx, cy, r, color);
    if (!rays) return;
    // 光芒随动画相位旋转（每相位转 1/16 圈）
    const float spin = (_weatherAnimPhase % 8) * (PI / 16.0f);
    for (int i = 0; i < 8; i++) {
        const float a = i * (PI / 4.0f) + spin;
        const int16_t x0 = cx + (int16_t)lroundf(cosf(a) * (r + 3));
        const int16_t y0 = cy + (int16_t)lroundf(sinf(a) * (r + 3));
        const int16_t x1 = cx + (int16_t)lroundf(cosf(a) * (r + 7));
        const int16_t y1 = cy + (int16_t)lroundf(sinf(a) * (r + 7));
        tft.drawLine(x0, y0, x1, y1, color);
    }
}

void DisplayClass::drawWeatherMoon(int16_t cx, int16_t cy, int16_t r,
                                   uint16_t color) {
    // 满圆减去偏移圆 → 月牙
    tft.fillCircle(cx, cy, r, color);
    tft.fillCircle(cx + r / 2, cy - r / 3, r, COLOR_BG);
    // 两颗小星随相位交替闪烁
    const uint8_t ph = _weatherAnimPhase % 4;
    if (ph == 1 || ph == 2) {
        tft.drawFastHLine(cx + r - 2, cy - r + 1, 3, color);
        tft.drawFastVLine(cx + r - 1, cy - r,     3, color);
    }
    if (ph == 2 || ph == 3) {
        tft.drawFastHLine(cx + r / 2, cy + r / 2 + 2, 3, color);
        tft.drawFastVLine(cx + r / 2 + 1, cy + r / 2 + 1, 3, color);
    }
}

void DisplayClass::drawWeatherCloud(int16_t cx, int16_t cy, int16_t w,
                                    uint16_t color) {
    // 三圆 + 圆角矩形拼云朵，cy 为云底
    const int16_t r1 = w / 4;            // 左肩
    const int16_t r2 = w / 3;            // 中顶
    const int16_t r3 = w / 5;            // 右肩
    tft.fillCircle(cx - w / 4, cy - r1 + 1, r1, color);
    tft.fillCircle(cx + 1,     cy - r2 + 1, r2, color);
    tft.fillCircle(cx + w / 4, cy - r3 + 1, r3, color);
    tft.fillRoundRect(cx - w / 3, cy - 5, (int16_t)(w * 2 / 3 + w / 8), 7, 3,
                      color);
}

void DisplayClass::drawWeatherDrops(int16_t cx, int16_t cy, int16_t w,
                                    uint16_t color, uint8_t count, bool snow,
                                    uint8_t phase) {
    const int16_t span = w / 2;
    for (uint8_t i = 0; i < count; i++) {
        const int16_t dx = cx - span / 2 +
                           (count > 1 ? (span * i) / (count - 1) : 0);
        // 相位驱动下落：每颗错开循环，形成连续落体感
        const int16_t fall = ((phase + i * 2) % 4) * 2;
        const int16_t dy = cy + ((i % 2) ? 3 : 0) + fall - 3;
        if (snow) {
            // 小雪花：十字 + 斜叉（下落时横向轻微摆动）
            const int16_t sway = ((phase + i) % 2) ? 1 : -1;
            tft.drawFastHLine(dx + sway - 2, dy, 5, color);
            tft.drawFastVLine(dx + sway, dy - 2, 5, color);
            tft.drawPixel(dx + sway - 1, dy - 1, color);
            tft.drawPixel(dx + sway + 1, dy - 1, color);
            tft.drawPixel(dx + sway - 1, dy + 1, color);
            tft.drawPixel(dx + sway + 1, dy + 1, color);
        } else {
            // 雨滴：短斜线
            tft.drawLine(dx + 1, dy - 2, dx - 1, dy + 3, color);
            tft.drawLine(dx + 2, dy - 2, dx,     dy + 3, color);
        }
    }
}

void DisplayClass::drawWeatherBolt(int16_t cx, int16_t cy, uint16_t color) {
    // 简化闪电：两个三角拼折线
    tft.fillTriangle(cx - 1, cy - 4, cx + 4, cy - 4, cx - 1, cy + 2, color);
    tft.fillTriangle(cx + 2, cy - 1, cx + 1, cy + 6, cx - 3, cy + 1, color);
}

void DisplayClass::drawWeatherIcon(int16_t cx, int16_t cy, int16_t r,
                                   WeatherIcon icon, bool dimmed) {
    // iOS 系统色（深色模式变体）；灰显时统一降为 DIM
    const uint16_t sunC   = dimmed ? COLOR_DIM : 0xFEA1;  // #FFD60A 系统黄
    const uint16_t moonC  = dimmed ? COLOR_DIM : 0xCF1E;  // #D1E3F5 月白
    const uint16_t cloudC = dimmed ? COLOR_DIM : 0xAD76;  // #AEAEB2 浅灰云
    const uint16_t darkC  = dimmed ? COLOR_DIM : 0x630C;  // #636366 深灰云
    const uint16_t rainC  = dimmed ? COLOR_DIM : 0x669F;  // #64D2FF 天青
    const uint16_t snowC  = dimmed ? COLOR_DIM : 0xE73C;  // 雪白
    const uint16_t boltC  = dimmed ? COLOR_DIM : 0xFEA1;  // #FFD60A 系统黄

    // 云朵随相位左右缓慢漂移：0,1,2,1,0,-1,-2,-1
    static const int8_t kDrift[8] = {0, 1, 2, 1, 0, -1, -2, -1};
    const int16_t drift = kDrift[_weatherAnimPhase % 8];
    const uint8_t ph = _weatherAnimPhase;

    switch (icon) {
        case WeatherIcon::ClearDay:
            drawWeatherSun(cx, cy, r - 5, sunC, true);
            break;
        case WeatherIcon::ClearNight:
            drawWeatherMoon(cx, cy, r - 4, moonC);
            break;
        case WeatherIcon::PartlyDay:
            drawWeatherSun(cx - r / 2, cy - r / 3, r / 2, sunC, true);
            drawWeatherCloud(cx + 2 + drift, cy + r / 2, r + 6, cloudC);
            break;
        case WeatherIcon::PartlyNight:
            drawWeatherMoon(cx - r / 2, cy - r / 3, r / 2 + 1, moonC);
            drawWeatherCloud(cx + 2 + drift, cy + r / 2, r + 6, cloudC);
            break;
        case WeatherIcon::Cloudy:
            // 两层云反向漂移，制造视差
            drawWeatherCloud(cx - r / 3 - drift, cy - r / 4, r, darkC);
            drawWeatherCloud(cx + r / 4 + drift, cy + r / 2, r + 4, cloudC);
            break;
        case WeatherIcon::Fog:
            drawWeatherCloud(cx, cy - r / 4, r, cloudC);
            // 雾带随相位横向摆动
            for (int i = 0; i < 3; i++) {
                const int16_t off = ((ph + i) % 2) ? drift : -drift;
                tft.drawFastHLine(cx - r + 2 + i * 3 + off, cy + r / 4 + i * 5,
                                  2 * r - 6, darkC);
            }
            break;
        case WeatherIcon::Drizzle:
            drawWeatherCloud(cx, cy, r + 4, cloudC);
            drawWeatherDrops(cx, cy + r / 2 + 3, r, rainC, 2, false, ph);
            break;
        case WeatherIcon::Rain:
            drawWeatherCloud(cx, cy, r + 4, darkC);
            drawWeatherDrops(cx, cy + r / 2 + 3, r + 4, rainC, 3, false, ph);
            break;
        case WeatherIcon::Showers:
            drawWeatherSun(cx - r + 3, cy - r + 5, r / 2 - 1, sunC, false);
            drawWeatherCloud(cx + 2, cy, r + 2, cloudC);
            drawWeatherDrops(cx + 2, cy + r / 2 + 3, r, rainC, 3, false, ph);
            break;
        case WeatherIcon::Snow:
            drawWeatherCloud(cx, cy, r + 4, cloudC);
            drawWeatherDrops(cx, cy + r / 2 + 4, r + 2, snowC, 3, true, ph);
            break;
        case WeatherIcon::Thunder:
            drawWeatherCloud(cx, cy, r + 4, darkC);
            // 闪电闪烁：4 相位里亮 3 灭 1，另在亮相位加分支小闪
            if (ph % 4 != 3) {
                drawWeatherBolt(cx, cy + r / 2 + 3, boltC);
                if (ph % 4 == 1) {
                    tft.drawLine(cx + 6, cy + r / 2, cx + 8, cy + r / 2 + 5,
                                 boltC);
                }
            }
            break;
        case WeatherIcon::Unknown:
        default: {
            // 占位：圆圈 + 问号
            tft.drawCircle(cx, cy, r - 4, COLOR_DIM);
            const int16_t qw = measureTextWidth("?", 2);
            drawText(cx - qw / 2, cy - 9, "?", 2, COLOR_DIM, COLOR_BG);
            break;
        }
    }
}

// 天气图标动画：每 400ms 推进一个相位，只重绘图标小区域。
// 无动画意义的图标（未知/灰显）不刷新，避免无谓的 SPI 流量。
void DisplayClass::updateWeatherAnimation() {
    if (!_weatherValid) return;
    if (_weatherIcon == WeatherIcon::Unknown) return;
    if (_weatherIconCx == 0) return;   // 尚未完成首次布局

    const uint32_t now = millis();
    if (now - _weatherAnimMs < 400) return;
    _weatherAnimMs = now;
    _weatherAnimPhase = (uint8_t)((_weatherAnimPhase + 1) % 8);

    constexpr int16_t iconR = 16;
    constexpr int16_t half  = iconR + 5;   // 覆盖云朵外扩与光芒
    tft.fillRect(_weatherIconCx - half, _weatherIconCy - half,
                 2 * half, 2 * half + 4, COLOR_BG);
    drawWeatherIcon(_weatherIconCx, _weatherIconCy, iconR, _weatherIcon,
                    false);
}

void DisplayClass::drawWelcomeDate(int16_t y, bool force) {
    if (!force &&
        _clockY == _lastWelcomeY &&
        _clockMo == _lastWelcomeMo &&
        _clockD == _lastWelcomeD) {
        return;
    }
    _lastWelcomeY = _clockY;
    _lastWelcomeMo = _clockMo;
    _lastWelcomeD = _clockD;

    char buf[40];
    const uint8_t weekday = weekdayFromDate(_clockY, _clockMo, _clockD);
    static const char *const weekCn[] = {
        "日", "一", "二", "三", "四", "五", "六"
    };
    // ASCII 数字走系统字体，汉字走内嵌点阵
    snprintf(buf, sizeof(buf), "%04u 年 %02u 月 %02u 日 星期%s",
             _clockY, _clockMo, _clockD, weekCn[weekday % 7]);
    const int16_t tw = measureCjkText(buf, 1);
    tft.fillRect(0, y, kScrW, 18, COLOR_BG);
    drawCjkText((kScrW - tw) / 2, y, buf, 1, COLOR_TEXT, COLOR_BG);
}

// 时分秒布局：
//   [H][H]  :  [M][M]  :  [S][S]
//    HR         MIN        SEC
// 组内密贴，组间冒号分隔；去掉底部波形后垂直居中放大
// 计算第 i 个数字卡的 x 坐标（i=0..5，HHMMSS）
int16_t DisplayClass::flipCardX(int idx, int16_t x0) const {
    const int g = idx / 2;                     // 组 0=HR, 1=MIN, 2=SEC
    const int j = idx % 2;                     // 组内 0=十位, 1=个位
    return x0 + g * (kFlipPairW + kFlipColonW) + j * (kFlipCardW + kFlipGapIn);
}

// 翻页卡半区绘制（动画用）
// 两阶段都只用 fillRect + drawLine，绝不调 fillRoundRect（圆角溢出像素是抖动根因）
// Phase 1: 上半覆盖旧数字上半 + 画新数字上半（严格限制在 mid 以上）
// Phase 2: 下半覆盖旧数字下半 + 画新数字下半（与 Phase 1 同 y 坐标，避免视觉跳动）
void DisplayClass::drawFlipCardHalf(int16_t x, int16_t y, int16_t w, int16_t h,
                                    char digit, bool lowerHalf) {
    const int16_t mid = h / 2;

    if (!lowerHalf) {
        // ---- Phase 1: 上半翻落 ----
        // 上半背景（覆盖旧数字上半）
        tft.fillRect(x + 1, y + 1, w - 2, mid - 2, COLOR_CARD_TOP);
        // 上半左/右边线
        tft.drawFastVLine(x,         y + 1, mid - 2, COLOR_CARD_EDGE);
        tft.drawFastVLine(x + w - 1, y + 1, mid - 2, COLOR_CARD_EDGE);
        // 中缝折痕
        tft.drawFastHLine(x + 2, y + mid - 1, w - 4, COLOR_FLIP_SHADOW);
        tft.drawFastHLine(x + 2, y + mid,     w - 4, COLOR_FLIP_LINE);
        drawFlipDigitClip(x, y, w, h, digit, 0, mid);
    } else {
        // ---- Phase 2: 下半翻落 ----
        // 下半背景（覆盖旧数字下半）
        tft.fillRect(x + 1, y + mid, w - 2, h - mid - 1, COLOR_CARD);
        // 下半左/右边线
        tft.drawFastVLine(x,         y + mid, h - mid - 1, COLOR_CARD_EDGE);
        tft.drawFastVLine(x + w - 1, y + mid, h - mid - 1, COLOR_CARD_EDGE);
        // 中缝折痕
        tft.drawFastHLine(x + 2, y + mid - 1, w - 4, COLOR_FLIP_SHADOW);
        tft.drawFastHLine(x + 2, y + mid,     w - 4, COLOR_FLIP_LINE);
        drawFlipDigitClip(x, y, w, h, digit, mid, h - mid);
    }
}

// 翻页动画状态机：每 ms 推进，到达时刻即绘制对应半片
#if 0
void DisplayClass::advanceFlipAnimation() {
    if (_flipAnim == FlipAnim::Idle) return;

    constexpr int16_t y0 = kFlipClockY;
    const int16_t x0 = (kScrW - kFlipTotalW) / 2;
    uint32_t elapsed = millis() - _flipEnterMs;

    // Recover from a missed frame or a pre-empted redraw so the clock cannot stay frozen.
    if (elapsed > kFlipPhase2Ms + 250) {
        Serial.printf("[CLOCK][%lu] flip timeout recovery elapsed=%lu phase=%u\n",
                      (unsigned long)millis(),
                      (unsigned long)elapsed,
                      (unsigned)_flipAnim);
        _flipAnim = FlipAnim::Idle;
        _flipLastReveal = 0xFF;
        _lastFlipH = 0xFF;
        _lastFlipM = 0xFF;
        _lastFlipS = 0xFF;
        drawFlipClock(true);
        return;
    }

    const int16_t mid = kFlipCardH / 2;
    // Finish the upper reveal at the phase boundary. This happens once so
    // phase 2 only needs to update the lower moving region.
    if (_flipAnim == FlipAnim::Phase1Upper && elapsed >= kFlipPhase1Ms) {
        for (int i = 0; i < 6; i++) {
            if (!_flipChanged[i]) continue;
            const int16_t x = flipCardX(i, x0);
            const char newDigit = (char)_flipNewDig[i];
            drawFlipDigitClip(x, y0, kFlipCardW, kFlipCardH,
                              newDigit, 0, mid);
        }
        tft.startWrite();
        for (int i = 0; i < 6; i++) {
            if (!_flipChanged[i]) continue;
            const int16_t x = flipCardX(i, x0);
            tft.writeFastHLine(x + 2, y0 + mid - 1,
                               kFlipCardW - 4, COLOR_FLIP_SHADOW);
            tft.writeFastHLine(x + 2, y0 + mid,
                               kFlipCardW - 4, COLOR_FLIP_LINE);
        }
        tft.endWrite();
        _flipAnim = FlipAnim::Phase2Lower;
        _flipLastReveal = 0xFF;
        Serial.printf("[CLOCK][%lu] flip phase1->phase2 elapsed=%lu\n",
                      (unsigned long)millis(),
                      (unsigned long)elapsed);
    }

    int16_t reveal = 0;
    if (_flipAnim == FlipAnim::Phase1Upper) {
        const uint16_t progress = (uint16_t)(elapsed * 255UL /
                                              kFlipPhase1Ms);
        const uint16_t inverse = 255U - progress;
        const uint16_t eased = 255U -
            (uint16_t)((inverse * inverse) / 255U);
        reveal = (int16_t)(mid * eased / 255U);
    } else {
        const uint32_t phaseElapsed = elapsed - kFlipPhase1Ms;
        const uint32_t phaseLength = kFlipPhase2Ms - kFlipPhase1Ms;
        uint16_t progress = (uint16_t)(phaseElapsed * 255UL /
                                        phaseLength);
        if (progress > 255U) progress = 255U;
        const uint16_t inverse = 255U - progress;
        const uint16_t eased = 255U -
            (uint16_t)((inverse * inverse) / 255U);
        reveal = (int16_t)(mid * eased / 255U);
        if (reveal > mid) reveal = mid;
    }

    // The render loop can run much faster than the integer pixel height
    // changes. Skip duplicate transfers while keeping the animation clock
    // responsive to input and page changes.
    const int16_t previousReveal = (_flipLastReveal == 0xFF)
                                       ? 0
                                       : (int16_t)_flipLastReveal;
    const bool revealChanged = (reveal != _flipLastReveal);
    if (revealChanged) _flipLastReveal = (uint8_t)reveal;

    for (int i = 0; i < 6; i++) {
        if (!_flipChanged[i]) continue;
        if (!revealChanged) continue;

        const int16_t x = flipCardX(i, x0);
        const char newDigit = (char)_flipNewDig[i];

        if (_flipAnim == FlipAnim::Phase1Upper) {
            if (reveal > previousReveal) {
                const int16_t clipY = mid - reveal;
                const int16_t clipH = reveal - previousReveal;
                // Only the newly exposed strip is sent. Earlier strips are
                // already correct on the panel and need no second transfer.
                drawFlipDigitClip(x, y0, kFlipCardW, kFlipCardH,
                                  newDigit, clipY, clipH);
            }
        } else {
            if (reveal > previousReveal) {
                const int16_t clipY = mid + previousReveal;
                const int16_t clipH = reveal - previousReveal;
                // Only the newly revealed lower strip is transferred.
                drawFlipDigitClip(x, y0, kFlipCardW, kFlipCardH,
                                  newDigit, clipY, clipH);
            }
        }

    }

    if (revealChanged) {
        // The card crease is fixed in the middle. Keep all crease writes in
        // one SPI transaction after the newly exposed strips are painted.
        tft.startWrite();
        for (int i = 0; i < 6; i++) {
            if (!_flipChanged[i]) continue;
            const int16_t x = flipCardX(i, x0);
            tft.writeFastHLine(x + 2, y0 + mid - 1,
                               kFlipCardW - 4, COLOR_FLIP_SHADOW);
            tft.writeFastHLine(x + 2, y0 + mid,
                               kFlipCardW - 4, COLOR_FLIP_LINE);
        }
        tft.endWrite();
    }

    if (elapsed >= kFlipPhase2Ms) {
        Serial.printf("[CLOCK][%lu] flip complete elapsed=%lu\n",
                      (unsigned long)millis(),
                      (unsigned long)elapsed);
        _flipAnim = FlipAnim::Idle;
        _flipLastReveal = 0xFF;
        _lastFlipH = _clockH;
        _lastFlipM = _clockM;
        _lastFlipS = _clockS;
    }
    return;

#if 0
    if (_flipAnim == FlipAnim::Phase1Upper && elapsed >= kFlipPhase1Ms) {
        // 上半切新数字
        for (int i = 0; i < 6; i++) {
            if (!_flipChanged[i]) continue;
            const int16_t x = flipCardX(i, x0);
            char s[2] = { (char)_flipNewDig[i], '\0' };
            drawFlipCardHalf(x, y0, kFlipCardW, kFlipCardH, s[0], false);
        }
        _flipAnim = FlipAnim::Phase2Lower;
    }

    if (_flipAnim == FlipAnim::Phase2Lower && elapsed >= kFlipPhase2Ms) {
        // 下半切新数字
        for (int i = 0; i < 6; i++) {
            if (!_flipChanged[i]) continue;
            const int16_t x = flipCardX(i, x0);
            char s[2] = { (char)_flipNewDig[i], '\0' };
            drawFlipCardHalf(x, y0, kFlipCardW, kFlipCardH, s[0], true);
        }
        // 动画结束
        _flipAnim = FlipAnim::Idle;
        _lastFlipH = _clockH;
        _lastFlipM = _clockM;
        _lastFlipS = _clockS;
    }
#endif
}
#endif

void DisplayClass::advanceFlipAnimation() {
    if (_flipAnim == FlipAnim::Idle) return;

    constexpr int16_t y0 = kFlipClockY;
    const int16_t x0 = (kScrW - kFlipTotalW) / 2;
    const uint32_t elapsed = millis() - _flipEnterMs;

    // Recover from a missed frame without leaving the clock half-revealed.
    if (elapsed > kFlipAnimationMs + 250U) {
        Serial.printf("[CLOCK][%lu] flip timeout recovery elapsed=%lu\n",
                      (unsigned long)millis(),
                      (unsigned long)elapsed);
        _flipAnim = FlipAnim::Idle;
        _flipLastReveal = 0xFF;
        _lastFlipH = 0xFF;
        _lastFlipM = 0xFF;
        _lastFlipS = 0xFF;
        drawFlipClock(true);
        return;
    }

    const int16_t mid = kFlipCardH / 2;
    uint32_t progress = elapsed * 255UL / kFlipAnimationMs;
    if (progress > 255U) progress = 255U;
    const uint32_t inverse = 255U - progress;
    const uint16_t eased = (uint16_t)(255U - (inverse * inverse) / 255U);
    const int16_t reveal = (int16_t)(mid * eased / 255U);
    const int16_t previousReveal = (_flipLastReveal == 0xFF)
                                       ? 0
                                       : (int16_t)_flipLastReveal;
    const bool revealChanged = reveal != previousReveal;
    if (revealChanged) _flipLastReveal = (uint8_t)reveal;

    if (revealChanged && reveal > 0) {
        for (int i = 0; i < 6; i++) {
            if (!_flipChanged[i]) continue;
            const int16_t x = flipCardX(i, x0);
            const char newDigit = (char)_flipNewDig[i];

            // One contiguous center band updates both halves in one transfer.
            // This removes the previous top-then-bottom visual delay.
            drawFlipDigitClip(x, y0, kFlipCardW, kFlipCardH,
                              newDigit, mid - reveal, reveal * 2);
        }

        tft.startWrite();
        for (int i = 0; i < 6; i++) {
            if (!_flipChanged[i]) continue;
            const int16_t x = flipCardX(i, x0);
            tft.writeFastHLine(x + 2, y0 + mid - 1,
                               kFlipCardW - 4, COLOR_FLIP_SHADOW);
            tft.writeFastHLine(x + 2, y0 + mid,
                               kFlipCardW - 4, COLOR_FLIP_LINE);
        }
        tft.endWrite();
    }

    if (elapsed >= kFlipAnimationMs) {
        Serial.printf("[CLOCK][%lu] flip complete elapsed=%lu sync=true\n",
                      (unsigned long)millis(),
                      (unsigned long)elapsed);
        _flipAnim = FlipAnim::Idle;
        _flipLastReveal = 0xFF;
        _lastFlipH = _clockH;
        _lastFlipM = _clockM;
        _lastFlipS = _clockS;
    }
}

void DisplayClass::drawFlipClock(bool force) {
    // 动画进行中时不接受新的翻页请求
    if (_flipAnim != FlipAnim::Idle) return;

    if (!force &&
        _clockH == _lastFlipH &&
        _clockM == _lastFlipM &&
        _clockS == _lastFlipS) {
        return;
    }

    constexpr int16_t y0 = kFlipClockY;
    const int16_t x0 = (kScrW - kFlipTotalW) / 2;

    char dig[12];
    snprintf(dig, sizeof(dig), "%02u%02u%02u", _clockH, _clockM, _clockS);

    const bool first = force || (_lastFlipH == 0xFF);

    if (first) {
        // 全量绘制 — 清底、冒号、标签、所有卡
        tft.fillRect(0, STATUS_H + 1, kScrW, 240 - STATUS_H - 1, COLOR_BG);
        drawWelcomeWeather(true);
        drawFlipColon(x0 + kFlipPairW + (kFlipColonW - 4) / 2, y0, kFlipCardH);
        drawFlipColon(x0 + 2 * kFlipPairW + kFlipColonW + (kFlipColonW - 4) / 2, y0, kFlipCardH);
        drawWelcomeDate(y0 + kFlipBlockH + 14, true);

        for (int i = 0; i < 6; i++) {
            const int16_t x = flipCardX(i, x0);
            drawFlipCard(x, y0, kFlipCardW, kFlipCardH, dig[i]);
        }

        _lastFlipH = _clockH;
        _lastFlipM = _clockM;
        _lastFlipS = _clockS;
        _flipAnim = FlipAnim::Idle;
        return;
    }

    drawWelcomeDate(y0 + kFlipBlockH + 14, false);

    // 差分：检测哪些位变化
    const char oldDig[6] = {
        (char)('0' + (_lastFlipH / 10)),
        (char)('0' + (_lastFlipH % 10)),
        (char)('0' + (_lastFlipM / 10)),
        (char)('0' + (_lastFlipM % 10)),
        (char)('0' + (_lastFlipS / 10)),
        (char)('0' + (_lastFlipS % 10)),
    };

    bool any = false;
    uint8_t changedMask = 0;
    for (int i = 0; i < 6; i++) {
        _flipChanged[i] = (dig[i] != oldDig[i]);
        if (_flipChanged[i]) {
            _flipOldDig[i] = (uint8_t)oldDig[i];
            _flipNewDig[i] = (uint8_t)dig[i];
            changedMask |= (uint8_t)(1U << i);
            any = true;
        }
    }

    if (!any) return;

    // 启动翻页动画：Phase1Upper → 当前仍显示旧数字，等待 kFlipPhase1Ms 后切上半
    _flipAnim = FlipAnim::SyncReveal;
    _flipEnterMs = millis();
    _flipLastReveal = 0xFF;
    Serial.printf("[CLOCK][%lu] flip start old=%02u:%02u:%02u new=%02u:%02u:%02u changedMask=0x%02X\n",
                  (unsigned long)_flipEnterMs,
                  (unsigned)_lastFlipH, (unsigned)_lastFlipM, (unsigned)_lastFlipS,
                  (unsigned)_clockH, (unsigned)_clockM, (unsigned)_clockS,
                  (unsigned)changedMask);
}

#if 0
/*
void DisplayClass::drawBatteryIcon(int16_t x, int16_t y, uint8_t pct) {
    // 扁平电量：圆角感用 1px 描边矩形，百分比居中写在电池体内
    // 外形： [  82%  ]▪   总宽 40（体 36 + 头 3）× 高 16
    constexpr int16_t BW = 36;   // 电池身宽
    constexpr int16_t BH = 16;   // 电池身高
    constexpr int16_t CAP_W = 3;
    constexpr int16_t CAP_H = 8;

    if (pct > 100) pct = 100;

    // 电量色（扁平纯色，无渐变）
    uint16_t fillC;
    if (pct <= 15)      fillC = 0xF800;      // 红
    else if (pct <= 30) fillC = 0xFE60;      // 橙黄
    else if (pct <= 50) fillC = 0xFFE0;      // 黄
    else                fillC = 0x07E0;      // 绿

    const uint16_t border = COLOR_TEXT;
    const uint16_t empty  = 0x10A2;          // 略浅于状态栏的空槽

    // 空槽底
    tft.fillRect(x, y, BW, BH, empty);
    // 电量填充（左 → 右，内缩 2px 更扁平）
    const int16_t innerW = BW - 4;
    const int16_t innerH = BH - 4;
    int16_t fillW = (int16_t)((innerW * (int16_t)pct) / 100);
    if (pct > 0 && fillW < 1) fillW = 1;
    if (fillW > 0) {
        tft.fillRect(x + 2, y + 2, fillW, innerH, fillC);
    }
    // 1px 外框
    tft.drawRect(x, y, BW, BH, border);
    // 正极小块（扁平，垂直居中）
    tft.fillRect(x + BW, y + (BH - CAP_H) / 2, CAP_W, CAP_H, border);

    // 百分比居中写在电池内（透明底，保证压在填充色上仍可读）
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", pct);
    const int16_t tw = measureTextWidth(buf, 1);
    const int16_t tx = x + (BW - tw) / 2;
    const int16_t ty = y + 1;

    // 深色描边字（先四向偏移再正文），低电量红底也清晰
    const uint16_t outline = 0x0000;
    for (int8_t ox = -1; ox <= 1; ox++) {
        for (int8_t oy = -1; oy <= 1; oy++) {
            if (ox == 0 && oy == 0) continue;
            drawText(tx + ox, ty + oy, buf, 1, outline, outline);
        }
    }
    drawText(tx, ty, buf, 1, COLOR_TEXT, COLOR_TEXT);
}

*/
#endif

void DisplayClass::drawBatteryIcon(int16_t x, int16_t y, uint8_t pct) {
    // iOS 风格：灰描边圆角壳 + 圆角电量条（绿/黄/红三档）+ 右侧极头
    constexpr int16_t BW = 27;
    constexpr int16_t BH = 14;
    constexpr int16_t CAP_W = 2;
    constexpr int16_t CAP_H = 6;
    if (pct > 100) pct = 100;

    const uint16_t fillC = pct <= 15 ? COLOR_ALERT :
                           (pct <= 30 ? 0xFEA1 : COLOR_GREEN);  // 黄=#FFD60A
    tft.fillRect(x, y, BW + CAP_W + 1, BH + 2, COLOR_BAR);
    tft.drawRoundRect(x, y + 1, BW, BH, 4, COLOR_DIM);
    const int16_t innerW = BW - 6;
    int16_t fillW = (int16_t)((innerW * (int16_t)pct) / 100);
    if (pct > 0 && fillW < 2) fillW = 2;
    if (fillW > 0) {
        tft.fillRoundRect(x + 3, y + 4, fillW, BH - 6, 2, fillC);
    }
    // 极头（圆角小凸块，垂直居中）
    tft.fillRoundRect(x + BW, y + 1 + (BH - CAP_H) / 2, CAP_W, CAP_H, 1,
                      COLOR_DIM);

    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", pct);
    drawText(x + BW + CAP_W + 4, y + 1, buf, 1,
             pct <= 15 ? COLOR_ALERT : COLOR_TEXT, COLOR_BAR);
}

void DisplayClass::drawStatusBar(bool force) {
    tickClock();

    const bool bt = BluetoothA2DP.isConnected();
    const bool wifi = _wifiOn;
    const uint8_t batt = _battPct;
    const uint8_t hh = _clockH;
    const uint8_t mm = _clockM;
    const uint8_t dd = _clockD;
    const uint8_t mo = _clockMo;

    const bool changed =
        force ||
        bt != _lastBtOn ||
        wifi != _lastWifi ||
        batt != _lastBatt ||
        hh != _lastClockH ||
        mm != _lastClockM ||
        dd != _lastClockD ||
        mo != _lastClockMo ||
        _clockY != _lastClockY;

    if (!changed) return;

    _lastBtOn = bt;
    _lastWifi = wifi;
    _lastBatt = batt;
    _lastClockH = hh;
    _lastClockM = mm;
    _lastClockY = _clockY;
    _lastClockMo = mo;
    _lastClockD = dd;

    tft.fillRect(0, 0, kScrW, STATUS_H, COLOR_BAR);
    tft.drawFastHLine(0, STATUS_H - 1, kScrW, COLOR_LINE);

    // 深色条 + 底部分割线
    tft.fillRect(0, 0, kScrW, STATUS_H, COLOR_BAR);
    tft.drawFastHLine(0, STATUS_H - 1, kScrW, COLOR_LINE);

    // 左：日期（主区域保留 HH:MM:SS 翻页时钟）
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02u/%02u", mo, dd);
    drawText(6, 7, tbuf, 2, COLOR_TEXT, COLOR_BAR);

    // 中：状态文案（中文）
    const char *mid;
    if (_page == Page::Network) mid = "网络设置";
    else if (_page == Page::Menu) mid = "应用";
    else if (_page == Page::Settings) mid = "设置";
    else if (_page == Page::Version) mid = "版本";
    else if (_page == Page::NeteaseCloud) mid = "音乐";
    else if (_page == Page::Music) mid = "音乐";
    else if (_page == Page::WifiList) mid = "WiFi 网络";
    else if (_page == Page::Books) mid = "听书";
    else if (_page == Page::Welcome) mid = "";
    else if (wifi && bt) mid = "WiFi + 蓝牙";
    else if (wifi) mid = "WiFi 已连接";
    else if (bt) mid = "蓝牙已连接";
    else mid = "待机";
    if (mid[0]) {
        const int16_t tw = measureCjkText(mid, 1);
        drawCjkText((kScrW - tw) / 2, 10, mid, 1, COLOR_TEXT, COLOR_BAR);
    }

    // 右：WiFi | BT | 扁平电池（百分比在内部）
    // 18 + 4 + 18 + 6 + 39 ≈ 85；图标 18px 在 28px 状态栏内垂直居中
    const int16_t rx = kScrW - 98;
    constexpr int16_t iconY = (STATUS_H - 1 - kStatusIcon) / 2;
    drawWifiIcon(rx, iconY, wifi);
    drawBtIcon(rx + kStatusIcon + 4, iconY, bt);
    drawBatteryIcon(rx + 2 * kStatusIcon + 12, 6, batt);
}

void DisplayClass::showSettingsPage(bool force) {
    if (!force && _page == Page::Settings) return;
    Serial.printf("[DISP][%lu][heap=%u] showSettingsPage: begin force=%s\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  force ? "true" : "false");
    _page = Page::Settings;
    _lastSettingsSelection = 0xFF;
    fillScreenBg();
    drawStatusBar(true);
    updateSettingsPage(true);
    Serial.printf("[DISP][%lu][heap=%u] showSettingsPage: complete\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap());
}

// Fixed 16x16 monochrome glyphs in enum order:
// Fan, Hui, She, Zhi, Xi, Tong, Ban, Ben.
// Each word stores one row, MSB first. No source-encoding conversion is used.
static const uint16_t kSettingsGlyphs[8][16] = {
    {0x0038, 0x23C0, 0x1200, 0x1200, 0x03F8, 0x0208, 0x7290, 0x1250,
     0x1220, 0x1250, 0x1488, 0x1108, 0x2800, 0x47FC, 0x0000, 0x0000},
    {0x0000, 0x3FFC, 0x2004, 0x2004, 0x27E4, 0x2424, 0x2424, 0x2424,
     0x2424, 0x27E4, 0x2004, 0x2004, 0x3FFC, 0x0000, 0x0000, 0x0000},
    {0x2000, 0x11F0, 0x1110, 0x0110, 0x0110, 0x720C, 0x1400, 0x13F8,
     0x1108, 0x1110, 0x14A0, 0x1840, 0x11B0, 0x060C, 0x0000, 0x0000},
    {0x3FF8, 0x2448, 0x2448, 0x3FF8, 0x0100, 0x3FF8, 0x0100, 0x1FF0,
     0x1010, 0x1010, 0x1110, 0x1110, 0x1110, 0x7FFC, 0x0000, 0x0000},
    {0x00F0, 0x3F00, 0x0200, 0x0420, 0x0840, 0x1F80, 0x0220, 0x0C10,
     0x3FF8, 0x0108, 0x0920, 0x1110, 0x2108, 0x0700, 0x0000, 0x0000},
    {0x1080, 0x1040, 0x2000, 0x4BFC, 0x7880, 0x1090, 0x2108, 0x43F8,
     0x78A8, 0x00A0, 0x0120, 0x1924, 0x6224, 0x041C, 0x0000, 0x0000},
    {0x0400, 0x240C, 0x24F0, 0x2480, 0x3E80, 0x20F8, 0x20A8, 0x3CA8,
     0x24A8, 0x24A8, 0x2490, 0x2490, 0x2528, 0x4644, 0x0000, 0x0000},
    {0x0100, 0x0100, 0x0100, 0x7FFC, 0x0380, 0x0540, 0x0540, 0x0920,
     0x1110, 0x2108, 0x4FE4, 0x0100, 0x0100, 0x0100, 0x0000, 0x0000}
};

void DisplayClass::drawCjkGlyph(int16_t x, int16_t y,
                                CjkGlyph glyph, uint16_t color) {
    const uint8_t index = static_cast<uint8_t>(glyph);
    if (index >= 8) return;

#if 1
    for (uint8_t row = 0; row < 16; row++) {
        const uint16_t bits = kSettingsGlyphs[index][row];
        int8_t runStart = -1;
        for (uint8_t col = 0; col <= 16; col++) {
            const bool ink = col < 16 && (bits & (0x8000U >> col));
            if (ink && runStart < 0) runStart = (int8_t)col;
            else if (!ink && runStart >= 0) {
                tft.fillRect(x + runStart, y + row,
                             col - runStart, 1, color);
                runStart = -1;
            }
        }
    }
#endif
#if 0
    // Compact 16px stroke glyphs for the settings page. The rest of the UI
    // remains Latin-font based, so a full CJK font is unnecessary here.
    switch (glyph) {
        case CjkGlyph::Hui: // 回
            tft.drawRect(x + 1, y + 1, 14, 14, color);
            tft.drawRect(x + 5, y + 5, 6, 6, color);
            break;
        case CjkGlyph::She: // 设
            tft.drawFastHLine(x + 1, y + 3, 4, color);
            tft.fillCircle(x + 3, y + 8, 1, color);
            tft.drawFastHLine(x + 1, y + 13, 4, color);
            tft.drawFastHLine(x + 8, y + 2, 6, color);
            tft.drawLine(x + 11, y + 2, x + 8, y + 8, color);
            tft.drawFastHLine(x + 8, y + 8, 6, color);
            tft.drawLine(x + 10, y + 8, x + 7, y + 14, color);
            tft.drawLine(x + 10, y + 9, x + 14, y + 14, color);
            break;
        case CjkGlyph::Zhi: // 置
            tft.drawRect(x + 1, y + 1, 14, 7, color);
            tft.drawFastVLine(x + 5, y + 2, 5, color);
            tft.drawFastVLine(x + 10, y + 2, 5, color);
            tft.drawFastHLine(x + 3, y + 10, 10, color);
            tft.drawFastVLine(x + 8, y + 9, 5, color);
            tft.drawFastHLine(x + 2, y + 14, 12, color);
            break;
        case CjkGlyph::Xi: // 系
            tft.drawLine(x + 8, y + 1, x + 4, y + 5, color);
            tft.drawLine(x + 4, y + 5, x + 11, y + 6, color);
            tft.drawLine(x + 8, y + 6, x + 3, y + 11, color);
            tft.drawLine(x + 8, y + 6, x + 13, y + 11, color);
            tft.drawFastHLine(x + 3, y + 8, 10, color);
            tft.drawFastVLine(x + 8, y + 8, 7, color);
            tft.drawLine(x + 8, y + 11, x + 3, y + 15, color);
            tft.drawLine(x + 8, y + 11, x + 13, y + 15, color);
            break;
        case CjkGlyph::Tong: // 统
            tft.drawFastHLine(x + 1, y + 3, 4, color);
            tft.drawFastHLine(x + 1, y + 8, 4, color);
            tft.drawLine(x + 4, y + 11, x + 1, y + 15, color);
            tft.drawFastHLine(x + 7, y + 4, 7, color);
            tft.drawFastHLine(x + 8, y + 8, 6, color);
            tft.drawFastVLine(x + 11, y + 3, 11, color);
            tft.drawLine(x + 11, y + 8, x + 7, y + 14, color);
            tft.drawLine(x + 11, y + 9, x + 14, y + 14, color);
            break;
        case CjkGlyph::Ban: // 版
            tft.drawFastVLine(x + 3, y + 1, 13, color);
            tft.drawFastHLine(x + 3, y + 4, 5, color);
            tft.drawLine(x + 3, y + 4, x + 7, y + 1, color);
            tft.drawFastHLine(x + 9, y + 3, 5, color);
            tft.drawLine(x + 11, y + 3, x + 9, y + 9, color);
            tft.drawLine(x + 9, y + 9, x + 14, y + 9, color);
            tft.drawLine(x + 11, y + 9, x + 8, y + 15, color);
            tft.drawLine(x + 11, y + 10, x + 14, y + 15, color);
            break;
        case CjkGlyph::Ben: // 本
            tft.drawFastVLine(x + 8, y + 1, 14, color);
            tft.drawFastHLine(x + 2, y + 6, 12, color);
            tft.drawLine(x + 8, y + 8, x + 3, y + 14, color);
            tft.drawLine(x + 8, y + 8, x + 13, y + 14, color);
            break;
        case CjkGlyph::Fan: // 返
            tft.drawFastHLine(x + 8, y + 2, 6, color);
            tft.drawLine(x + 11, y + 2, x + 8, y + 8, color);
            tft.drawFastHLine(x + 8, y + 8, 6, color);
            tft.drawLine(x + 10, y + 8, x + 7, y + 12, color);
            tft.drawLine(x + 10, y + 9, x + 14, y + 14, color);
            tft.drawFastHLine(x + 2, y + 5, 4, color);
            tft.drawLine(x + 5, y + 9, x + 11, y + 9, color);
            tft.drawLine(x + 11, y + 9, x + 6, y + 15, color);
            tft.drawFastHLine(x + 2, y + 15, 5, color);
            break;
    }

    // Restore the first stroke of each glyph. The source file uses a legacy
    // mixed-encoding comment block, so these are kept as plain ASCII code.
    switch (glyph) {
        case CjkGlyph::Hui:
            tft.drawRect(x + 1, y + 1, 14, 14, color);
            break;
        case CjkGlyph::She:
            tft.drawFastHLine(x + 1, y + 3, 4, color);
            break;
        case CjkGlyph::Zhi:
            tft.drawRect(x + 1, y + 1, 14, 7, color);
            break;
        case CjkGlyph::Xi:
            tft.drawLine(x + 8, y + 1, x + 4, y + 5, color);
            break;
        case CjkGlyph::Tong:
            tft.drawFastHLine(x + 1, y + 3, 4, color);
            break;
        case CjkGlyph::Ban:
            tft.drawFastVLine(x + 3, y + 1, 13, color);
            break;
        case CjkGlyph::Ben:
            tft.drawFastVLine(x + 8, y + 1, 14, color);
            break;
        case CjkGlyph::Fan:
            tft.drawFastHLine(x + 8, y + 2, 6, color);
            break;
    }
#endif
}

void DisplayClass::drawSettingsOption(uint8_t option, bool selected) {
    constexpr int16_t rowX = 16;
    constexpr int16_t rowY = 84;
    constexpr int16_t rowW = 288;
    constexpr int16_t rowH = 46;
    const int16_t y = rowY + option * rowH;
    (void)selected;
    const uint16_t ink = COLOR_TEXT;

    tft.fillRect(rowX, y, rowW, rowH, COLOR_BG);
    tft.drawFastHLine(rowX + 10, y + rowH - 1,
                      rowW - 20, COLOR_LINE);

    if (option == 0) {
        const char *label = "WiFi 配网";
        const int16_t tw = measureCjkText(label, 1);
        drawCjkText((kScrW - tw) / 2, y + 18, label, 1, ink, COLOR_BG);
    } else if (option == 1) {
        const char *label = "WiFi 网络";
        const int16_t tw = measureCjkText(label, 1);
        drawCjkText((kScrW - tw) / 2, y + 18, label, 1, ink, COLOR_BG);
    } else {
        const char *label = "系统版本";
        const int16_t tw = measureCjkText(label, 1);
        drawCjkText((kScrW - tw) / 2, y + 18, label, 1, ink, COLOR_BG);
    }

}

void DisplayClass::drawSettingsSelectionMarker(uint8_t option, bool selected) {
    constexpr int16_t rowX = 16;
    constexpr int16_t rowY = 84;
    constexpr int16_t rowW = 288;
    constexpr int16_t rowH = 46;
    const int16_t y = rowY + option * rowH;
    const int16_t arrowX = rowX + rowW - 18;

    // Only clear the two marker regions. Text, glyphs and separators remain
    // untouched when the joystick changes selection. Keep all marker work in
    // one transaction so a direction change does not queue several redraws.
    tft.startWrite();
    tft.writeFillRect(rowX, y + 10, 3, rowH - 20, COLOR_BG);
    tft.writeFillRect(arrowX - 1, y + 20, 8, 14, COLOR_BG);
    if (selected) {
        tft.writeFillRect(rowX, y + 10, 3, rowH - 20, COLOR_ACCENT);
        for (int16_t step = 0; step <= 5; step++) {
            tft.writePixel(arrowX + step, y + 22 + step * 4 / 5,
                           COLOR_ACCENT);
            tft.writePixel(arrowX + step, y + 30 - step * 4 / 5,
                           COLOR_ACCENT);
        }
    }
    tft.endWrite();
}

void DisplayClass::updateSettingsPage(bool force) {
    if (!force && _settingsSelection == _lastSettingsSelection) return;

    const uint8_t previous = _lastSettingsSelection;
    _lastSettingsSelection = _settingsSelection;

    if (force || previous >= 2) {
        tft.fillRect(0, STATUS_H + 1, kScrW,
                     240 - STATUS_H - 1, COLOR_BG);
        tft.drawLine(26, 49, 19, 56, COLOR_ACCENT);
        tft.drawLine(19, 56, 26, 63, COLOR_ACCENT);
        drawCjkGlyph(40, 48, CjkGlyph::Fan, COLOR_TEXT);
        drawCjkGlyph(57, 48, CjkGlyph::Hui, COLOR_TEXT);
        drawCjkGlyph(142, 48, CjkGlyph::She, COLOR_TEXT);
        drawCjkGlyph(159, 48, CjkGlyph::Zhi, COLOR_TEXT);
        tft.drawFastHLine(16, 74, 288, COLOR_LINE);
        drawSettingsOption(0, false);
        drawSettingsOption(1, false);
        drawSettingsOption(2, false);
        drawSettingsSelectionMarker(_settingsSelection, true);
        return;
    }

    drawSettingsSelectionMarker(previous, false);
    drawSettingsSelectionMarker(_settingsSelection, true);
}

void DisplayClass::showVersionPage(bool force) {
    if (!force && _page == Page::Version) return;
    _page = Page::Version;
    fillScreenBg();
    drawStatusBar(true);

    drawText(16, 48, "CURRENT VERSION", 2, COLOR_TEXT, COLOR_BG);
    drawText(16, 79, APP_VERSION, 3, COLOR_ACCENT, COLOR_BG);
    drawText(16, 124, "ESP32-BT-Amp", 2, COLOR_TEXT, COLOR_BG);
    drawText(16, 151, "Firmware build information", 1, COLOR_DIM, COLOR_BG);
    tft.drawFastHLine(16, 183, 288, COLOR_LINE);
    drawText(16, 205, "LEFT  BACK", 1, COLOR_ACCENT, COLOR_BG);
}

void DisplayClass::showNetworkSettings(bool force) {
    if (!force && _page == Page::Network) return;
    Serial.printf("[DISP][%lu][heap=%u] showNetworkSettings: begin force=%s\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  force ? "true" : "false");
    _page = Page::Network;
    _networkSelection = 0;
    _networkMessage = "WiFi service not configured";
    _networkRescanRequested = false;
    _lastNetworkSelection = 0xFF;
    _lastNetworkMessage = nullptr;
    _networkServiceDirty = true;
    _lastNetworkQrPayload = "";
    _lastNetworkServiceActive = false;
    _lastNetworkServiceIp = "";
    _lastNetworkServiceSsid = "";
    Serial.printf("[DISP][%lu][heap=%u] showNetworkSettings: before fillScreen\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap());
    fillScreenBg();
    Serial.printf("[DISP][%lu][heap=%u] showNetworkSettings: after fillScreen\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap());
    drawStatusBar(true);
    Serial.printf("[DISP][%lu][heap=%u] showNetworkSettings: after status bar\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap());
    updateNetworkSettings(true);
    Serial.printf("[DISP][%lu][heap=%u] showNetworkSettings: complete\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap());
}

#if 0
void DisplayClass::updateNetworkSettings(bool force) {
    if (!force &&
        _networkSelection == _lastNetworkSelection &&
        _networkMessage == _lastNetworkMessage) {
        return;
    }

    _lastNetworkSelection = _networkSelection;
    _lastNetworkMessage = _networkMessage;

    tft.fillRect(0, STATUS_H + 1, kScrW, 240 - STATUS_H - 1, COLOR_BG);
    drawText(16, 46, "NETWORK SETUP", 2, COLOR_TEXT, COLOR_BG);
    drawText(16, 68, "Choose a provisioning mode", 1, COLOR_DIM, COLOR_BG);

    static const char *const options[] = {"Scan WiFi", "AP mode", "Back"};
    constexpr int16_t optionX = 16;
    constexpr int16_t optionY = 92;
    constexpr int16_t optionW = 288;
    constexpr int16_t optionH = 28;
    constexpr int16_t optionGap = 7;

    for (uint8_t i = 0; i < 3; i++) {
        const int16_t y = optionY + i * (optionH + optionGap);
        const bool selected = i == _networkSelection;
        if (selected) {
            tft.fillRoundRect(optionX, y, optionW, optionH, 4, COLOR_PANEL);
            tft.drawRoundRect(optionX, y, optionW, optionH, 4, COLOR_ACCENT);
            tft.fillRect(optionX, y + 6, 3, optionH - 12, COLOR_ACCENT);
        } else {
            tft.drawRoundRect(optionX, y, optionW, optionH, 4, COLOR_LINE);
        }
        drawText(optionX + 14, y + 8, options[i], 2,
                 selected ? COLOR_ACCENT : COLOR_TEXT, selected ? COLOR_PANEL : COLOR_BG);
    }

    drawText(16, 202, _networkMessage, 1, COLOR_DIM, COLOR_BG);
    drawText(16, 224, "VOL +/-  SELECT    PLAY  CONFIRM", 1, COLOR_DIM, COLOR_BG);
}

#endif

void DisplayClass::updateNetworkSettings(bool force) {
    const bool selectionChanged = force || _networkSelection != _lastNetworkSelection;
    const bool qrChanged = force || _networkQrPayload != _lastNetworkQrPayload;
    const bool detailsChanged =
        force || _networkServiceActive != _lastNetworkServiceActive ||
        _networkServiceIp != _lastNetworkServiceIp ||
        _networkServiceSsid != _lastNetworkServiceSsid;
    const bool contentChanged = qrChanged || detailsChanged;
    if (!selectionChanged && !contentChanged && !_networkServiceDirty) return;

    // 布局：QR 白卡(16,40,132x132) | 右侧信息列 x=164 | 状态行 y=178 | 按钮行 y=202
    constexpr int16_t contentY = STATUS_H + 1;
    constexpr int16_t contentH = 240 - contentY;
    constexpr int16_t cardY = 40;
    constexpr int16_t cardH = 132;
    constexpr int16_t qrCardX = 16;
    constexpr int16_t qrCardW = 132;
    constexpr int16_t infoX = 164;
    constexpr int16_t actionY = 202;
    constexpr int16_t actionH = 30;
    constexpr int16_t actionW = 140;
    constexpr int16_t actionGap = 8;

    if (contentChanged) {
        // 一次清空主体区域，保证状态切换时不会残留旧 SSID、IP 或二维码。
        tft.fillRect(0, contentY, kScrW, contentH, COLOR_BG);

        // 左卡：二维码白底圆角卡，配网就绪后边框点亮为强调色。
        const uint16_t qrBorder = _networkQrPayload.length()
            ? COLOR_ACCENT : COLOR_LINE;
        tft.fillRoundRect(qrCardX, cardY, qrCardW, cardH, 8, COLOR_PANEL);
        tft.drawRoundRect(qrCardX, cardY, qrCardW, cardH, 8, qrBorder);
        if (_networkQrPayload.length()) {
            esp_qrcode_config_t qrConfig = ESP_QRCODE_CONFIG_DEFAULT();
            qrConfig.display_func = drawProvisioningQr;
            qrConfig.max_qrcode_version = 5;
            qrConfig.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
            const esp_err_t qrResult = esp_qrcode_generate(
                &qrConfig, _networkQrPayload.c_str());
            if (qrResult != ESP_OK) {
                tft.fillRect(NETWORK_QR_BOX_X, NETWORK_QR_BOX_Y,
                             NETWORK_QR_BOX, NETWORK_QR_BOX, COLOR_PANEL);
                const int16_t ew = measureTextWidth("QR ERROR", 1);
                drawText(qrCardX + (qrCardW - ew) / 2, 100, "QR ERROR", 1,
                         COLOR_ACCENT, COLOR_PANEL);
            }
        } else {
            tft.fillRect(NETWORK_QR_BOX_X, NETWORK_QR_BOX_Y,
                         NETWORK_QR_BOX, NETWORK_QR_BOX, COLOR_PANEL);
            const int16_t sw = measureTextWidth("STARTING", 1);
            drawText(qrCardX + (qrCardW - sw) / 2, 100, "STARTING", 1,
                     COLOR_DIM, COLOR_PANEL);
        }

        // 右列：无边框信息列，中文标签 + 值，两倍行距留白。
        const String rawSsid = _networkServiceSsid.length()
            ? _networkServiceSsid : "MY-SMALL-BOX";
        String displaySsid = rawSsid;
        if (displaySsid.length() > 16) {
            displaySsid = displaySsid.substring(0, 13) + "...";
        }
        const String displayIp = _networkServiceIp.length()
            ? _networkServiceIp : "192.168.4.1";

        drawCjkText(infoX, 44, "热点名称", 1, COLOR_LABEL, COLOR_BG);
        drawText(infoX, 64, displaySsid.c_str(), 1, COLOR_TEXT, COLOR_BG);
        drawCjkText(infoX, 90, "密码", 1, COLOR_LABEL, COLOR_BG);
        drawText(infoX, 110, WifiProvisioningClass::PROVISIONING_PASSWORD,
                 1, COLOR_ACCENT, COLOR_BG);
        drawCjkText(infoX, 136, "管理地址", 1, COLOR_LABEL, COLOR_BG);
        drawText(infoX, 156, displayIp.c_str(), 1, COLOR_ACCENT, COLOR_BG);

        _lastNetworkQrPayload = _networkQrPayload;
        _lastNetworkServiceActive = _networkServiceActive;
        _lastNetworkServiceIp = _networkServiceIp;
        _lastNetworkServiceSsid = _networkServiceSsid;
    }

    if (selectionChanged || contentChanged) {
        auto drawAction = [&](int16_t x, const char *label, bool selected) {
            tft.fillRoundRect(x, actionY, actionW, actionH, 8,
                              selected ? COLOR_PANEL : COLOR_BG);
            tft.drawRoundRect(x, actionY, actionW, actionH, 8,
                              selected ? COLOR_ACCENT : COLOR_LINE);
            const int16_t textW = measureCjkText(label, 1);
            drawCjkText(x + (actionW - textW) / 2, actionY + 7, label, 1,
                        selected ? COLOR_ACCENT : COLOR_DIM,
                        selected ? COLOR_PANEL : COLOR_BG);
        };
        drawAction(16, "重新扫描", _networkSelection == 0);
        drawAction(16 + actionW + actionGap, "返回", _networkSelection == 1);
    }

    if (_networkServiceDirty || force) {
        tft.fillRect(16, 178, 288, 16, COLOR_BG);
        const char *status = _networkServiceStatus.length()
            ? _networkServiceStatus.c_str()
            : (_networkMessage ? _networkMessage : "Starting setup hotspot");
        const bool failed = strstr(status, "failed") || strstr(status, "Failed");
        const uint16_t statusColor = failed ? COLOR_ALERT :
            (_networkServiceActive ? COLOR_GREEN : COLOR_DIM);
        tft.fillCircle(21, 185, 2, statusColor);
        drawText(30, 179, status, 1, statusColor, COLOR_BG);
    }

    _lastNetworkSelection = _networkSelection;
    _lastNetworkMessage = _networkMessage;
    _networkServiceDirty = false;
}

// ==================== iOS 风格应用图标 ====================
// 三个应用图标统一 iOS 图标语言：同尺寸圆角方块（圆角约 24% 边长，
// 接近 iOS 图标圆角率）+ 纵向渐变底（上亮下暗）+ 居中白色图形。
// 配色对齐苹果自家 App：音乐=Apple Music 红粉、听书=Apple Books 橙、
// 设置=iOS 设置银灰，观感大小与风格完全一致。
void DisplayClass::drawAppTile(int16_t x, int16_t y, int16_t size,
                               uint16_t topColor, uint16_t bottomColor) {
    const int16_t r = size * 24 / 100;
    for (int16_t dy = 0; dy < size; dy++) {
        const uint8_t alpha =
            (uint8_t)((uint32_t)dy * 255U / (uint32_t)(size - 1));
        const uint16_t c = blendRgb565(topColor, bottomColor, alpha);
        // 圆角行内缩：按圆方程算出该行距圆心的水平弦长
        float d = 0.0f;
        if (dy < r) d = (float)(r - dy) - 0.5f;
        else if (dy >= size - r) d = (float)(dy - (size - r)) + 0.5f;
        int16_t inset = 0;
        if (d > 0.0f) {
            inset = (int16_t)((float)r - sqrtf((float)r * r - d * d) + 0.5f);
        }
        tft.drawFastHLine(x + inset, y + dy, size - 2 * inset, c);
    }
}

void DisplayClass::drawMusicIcon(int16_t x, int16_t y, int16_t size, bool selected) {
    (void)selected;
    // Apple Music：红粉渐变 + 白色双八分音符（符梁上斜、右符头略高）
    drawAppTile(x, y, size, 0xFAEE /*#FB5C74*/, 0xF927 /*#FA233B*/);
    const uint16_t ink = 0xFFFF;
    // 符杆在符头右侧，图形重心偏右，整体左移一点配平
    const int16_t cx = x + size / 2 - size * 4 / 100;
    const int16_t cy = y + size / 2;
    const int16_t hr    = size * 10 / 100;            // 符头半径
    const int16_t stemW = size >= 40 ? 3 : 2;         // 符杆宽
    const int16_t lHeadX = cx - size * 14 / 100;
    const int16_t rHeadX = cx + size * 18 / 100;
    const int16_t lHeadY = cy + size * 16 / 100;
    const int16_t rHeadY = lHeadY - size * 5 / 100;
    const int16_t lTopY  = cy - size * 18 / 100;
    const int16_t rTopY  = lTopY - size * 5 / 100;
    const int16_t beamH  = size * 9 / 100;
    const int16_t lStemX = lHeadX + hr - stemW + 1;
    const int16_t rStemX = rHeadX + hr - stemW + 1;
    tft.fillCircle(lHeadX, lHeadY, hr, ink);
    tft.fillCircle(rHeadX, rHeadY, hr, ink);
    tft.fillRect(lStemX, lTopY, stemW, lHeadY - lTopY, ink);
    tft.fillRect(rStemX, rTopY, stemW, rHeadY - rTopY, ink);
    // 斜符梁：平行四边形 = 两个三角形
    tft.fillTriangle(lStemX, lTopY, rStemX + stemW - 1, rTopY,
                     lStemX, lTopY + beamH, ink);
    tft.fillTriangle(rStemX + stemW - 1, rTopY,
                     rStemX + stemW - 1, rTopY + beamH,
                     lStemX, lTopY + beamH, ink);
}

void DisplayClass::drawBooksIcon(int16_t x, int16_t y, int16_t size) {
    // Apple Books：橙色渐变 + 白色摊开的书（外缘上翘，中缝透出底色）
    drawAppTile(x, y, size, 0xFD88 /*#FFB340*/, 0xEC20 /*#F28500*/);
    const uint16_t ink = 0xFFFF;
    const int16_t cx = x + size / 2;
    const int16_t cy = y + size / 2;
    const int16_t pw   = size * 22 / 100;             // 单页宽
    const int16_t ph   = size * 26 / 100;             // 页半高
    const int16_t tilt = size * 7 / 100;              // 外缘上翘量
    const int16_t gap  = size >= 40 ? 2 : 1;          // 中缝半宽
    // 每页是一个四边形（内上/内下/外下/外上），拆成两个三角形
    tft.fillTriangle(cx - gap, cy - ph + tilt, cx - gap, cy + ph,
                     cx - gap - pw, cy + ph - tilt, ink);
    tft.fillTriangle(cx - gap, cy - ph + tilt, cx - gap - pw, cy + ph - tilt,
                     cx - gap - pw, cy - ph, ink);
    tft.fillTriangle(cx + gap, cy - ph + tilt, cx + gap, cy + ph,
                     cx + gap + pw, cy + ph - tilt, ink);
    tft.fillTriangle(cx + gap, cy - ph + tilt, cx + gap + pw, cy + ph - tilt,
                     cx + gap + pw, cy - ph, ink);
}

void DisplayClass::drawSettingsIcon(int16_t x, int16_t y, int16_t size, bool selected) {
    (void)selected;
    // iOS 设置：银灰渐变 + 白色圆头齿轮，中孔回填渐变中点色。
    // 灰阶比 iOS 原版整体提亮：这块 TFT 低灰压黑严重。
    constexpr uint16_t kGrayTop = 0xBDB7;    // #B8B8C0
    constexpr uint16_t kGrayBot = 0x6B6E;    // #6E6E76
    drawAppTile(x, y, size, kGrayTop, kGrayBot);
    const uint16_t ink = 0xFFFF;
    const int16_t cx = x + size / 2;
    const int16_t cy = y + size / 2;
    const int16_t ringR  = size * 24 / 100;   // 齿心分布圆半径
    const int16_t toothR = size * 7 / 100;    // 圆头齿半径
    const int16_t bodyR  = size * 21 / 100;   // 轮体半径
    const int16_t holeR  = size * 9 / 100;    // 中孔半径
    const int16_t diag   = (int16_t)((ringR * 707L + 500) / 1000);
    const int16_t dxs[8] = {ringR, diag, 0, (int16_t)-diag,
                            (int16_t)-ringR, (int16_t)-diag, 0, diag};
    const int16_t dys[8] = {0, (int16_t)-diag, (int16_t)-ringR,
                            (int16_t)-diag, 0, diag, ringR, diag};
    for (uint8_t i = 0; i < 8; i++) {
        tft.fillCircle(cx + dxs[i], cy + dys[i], toothR, ink);
    }
    tft.fillCircle(cx, cy, bodyR, ink);
    tft.fillCircle(cx, cy, holeR, blendRgb565(kGrayTop, kGrayBot, 128));
}

void DisplayClass::showMenu(bool force) {
    if (!force && _page == Page::Menu) return;
    _page = Page::Menu;
    _lastMenuSelection = 0xFF;
    _lastMenuRowOffset = 0xFF;
    _menuSelectionAnimating = false;
    _menuAnimationFrom = 0xFF;
    _menuAnimationTo = 0xFF;
    _menuFrameX = 0;
    _menuFrameY = 0;
    _menuAnimationStartX = 0;
    _menuAnimationStartY = 0;
    fillScreenBg();
    drawStatusBar(true);
    updateMenu(true);
}

void DisplayClass::drawMenuSelectionFrame(int16_t x, int16_t y,
                                           uint16_t color, bool highlight) {
    const int16_t frameX = x + 2;
    const int16_t frameY = y + 2;
    const int16_t frameW = MENU_CELL_W - 4;
    const int16_t frameH = MENU_CELL_H - 4;
    const int16_t corner = 12;

    if (color == COLOR_BG) {
        tft.drawRoundRect(frameX, frameY, frameW, frameH, 8, COLOR_BG);
        tft.drawFastHLine(frameX + 3, frameY, corner, COLOR_BG);
        tft.drawFastHLine(frameX + frameW - corner - 3, frameY,
                          corner, COLOR_BG);
        tft.drawFastHLine(frameX + 3, frameY + frameH - 1,
                          corner, COLOR_BG);
        tft.drawFastHLine(frameX + frameW - corner - 3,
                          frameY + frameH - 1, corner, COLOR_BG);
        tft.drawFastVLine(frameX, frameY + 3, corner, COLOR_BG);
        tft.drawFastVLine(frameX, frameY + frameH - corner - 3,
                          corner, COLOR_BG);
        tft.drawFastVLine(frameX + frameW - 1, frameY + 3,
                          corner, COLOR_BG);
        tft.drawFastVLine(frameX + frameW - 1,
                          frameY + frameH - corner - 3, corner, COLOR_BG);
        return;
    }
    if (highlight) {
        tft.drawRoundRect(frameX - 1, frameY - 1, frameW + 2, frameH + 2,
                          9, COLOR_TEXT);
    }
    tft.drawRoundRect(frameX, frameY, frameW, frameH, 8, COLOR_LINE);

    // Four bright corner brackets keep the selection readable without making
    // the whole app tile look like a filled card.
    tft.drawFastHLine(frameX + 3, frameY, corner, color);
    tft.drawFastHLine(frameX + frameW - corner - 3, frameY,
                      corner, color);
    tft.drawFastHLine(frameX + 3, frameY + frameH - 1,
                      corner, color);
    tft.drawFastHLine(frameX + frameW - corner - 3,
                      frameY + frameH - 1, corner, color);
    tft.drawFastVLine(frameX, frameY + 3, corner, color);
    tft.drawFastVLine(frameX, frameY + frameH - corner - 3,
                      corner, color);
    tft.drawFastVLine(frameX + frameW - 1, frameY + 3,
                      corner, color);
    tft.drawFastVLine(frameX + frameW - 1,
                      frameY + frameH - corner - 3, corner, color);
}

void DisplayClass::drawMenuSelectionMotion(int16_t x, int16_t y,
                                            uint16_t color, bool highlight,
                                            bool horizontal) {
    const int16_t frameX = x + 2;
    const int16_t frameY = y + 2;
    const int16_t frameW = MENU_CELL_W - 4;
    const int16_t frameH = MENU_CELL_H - 4;
    (void)highlight;
    const int16_t railInset = 4;
    if (horizontal) {
        // The top and bottom rails stay outside the icon and label, so they
        // can be erased without repainting either app cell.
        tft.drawFastHLine(frameX + railInset, frameY,
                          frameW - railInset * 2, color);
        tft.drawFastHLine(frameX + railInset, frameY + frameH - 1,
                          frameW - railInset * 2, color);
    } else {
        tft.drawFastVLine(frameX, frameY + railInset,
                          frameH - railInset * 2, color);
        tft.drawFastVLine(frameX + frameW - 1, frameY + railInset,
                          frameH - railInset * 2, color);
    }
}

void DisplayClass::drawMenuApp(uint8_t appIndex, bool selected, uint8_t selectionInset) {
    if (appIndex >= MENU_APP_COUNT) return;

    const uint8_t col = appIndex % MENU_COLUMNS;
    const uint8_t row = appIndex / MENU_COLUMNS;
    const int16_t x = MENU_GRID_X + col * (MENU_CELL_W + MENU_CELL_GAP_X);
    const int16_t y = MENU_GRID_Y + row * (MENU_CELL_H + MENU_CELL_GAP_Y);

    // Redraw one cell only. This removes the old frame without blanking the whole menu.
    tft.fillRect(x, y, MENU_CELL_W, MENU_CELL_H, COLOR_BG);
    if (selected) {
        if (selectionInset == 0) {
            drawMenuSelectionFrame(x, y, COLOR_ACCENT, false);
        } else {
            const int16_t inset = selectionInset > 4 ? 4 : selectionInset;
            tft.drawRoundRect(x + inset, y + inset,
                              MENU_CELL_W - inset * 2, MENU_CELL_H - inset * 2,
                              6, COLOR_ACCENT);
        }
    }

    if (appIndex == 0) {
        drawMusicIcon(x + (MENU_CELL_W - MENU_ICON_SIZE) / 2, y + 8,
                      MENU_ICON_SIZE, false);
    } else if (appIndex == 1) {
        drawBooksIcon(x + (MENU_CELL_W - MENU_ICON_SIZE) / 2, y + 8,
                      MENU_ICON_SIZE);
    } else {
        drawSettingsIcon(x + (MENU_CELL_W - MENU_ICON_SIZE) / 2, y + 8,
                         MENU_ICON_SIZE, false);
    }
    const int16_t labelW = measureCjkText(MENU_APP_NAMES[appIndex], 1);
    drawCjkText(x + (MENU_CELL_W - labelW) / 2, y + 68,
                MENU_APP_NAMES[appIndex], 1, COLOR_TEXT, COLOR_BG);
}

void DisplayClass::updateMenu(bool force) {
    const bool selectionChanged = _menuSelection != _lastMenuSelection;
    const bool layoutChanged = _menuRowOffset != _lastMenuRowOffset;

    if (!force && !selectionChanged && !layoutChanged && !_menuSelectionAnimating) {
        return;
    }

    if (force || layoutChanged) {
        _menuSelectionAnimating = false;
        _menuAnimationFrom = 0xFF;
        _menuAnimationTo = _menuSelection;
        tft.fillRect(0, STATUS_H + 1, kScrW, 240 - STATUS_H - 1, COLOR_BG);
        const uint8_t firstApp = _menuRowOffset * MENU_COLUMNS;
        const uint8_t visibleCount = MENU_VISIBLE_ROWS * MENU_COLUMNS;
        for (uint8_t slot = 0; slot < visibleCount; slot++) {
            const uint8_t appIndex = firstApp + slot;
            if (appIndex >= MENU_APP_COUNT) break;
            drawMenuApp(appIndex, appIndex == _menuSelection);
        }
        const uint8_t col = _menuSelection % MENU_COLUMNS;
        const uint8_t row = _menuSelection / MENU_COLUMNS;
        _menuFrameX = MENU_GRID_X + col * (MENU_CELL_W + MENU_CELL_GAP_X);
        _menuFrameY = MENU_GRID_Y + row * (MENU_CELL_H + MENU_CELL_GAP_Y);
        _menuAnimationStartX = _menuFrameX;
        _menuAnimationStartY = _menuFrameY;
        _menuAnimationHorizontal = true;
    } else if (selectionChanged) {
        const bool wasAnimating = _menuSelectionAnimating &&
                                  _menuAnimationTo < MENU_APP_COUNT;
        const uint8_t oldSelection = _lastMenuSelection;

        // Erase only the previous border. The app artwork remains untouched,
        // including when the joystick reverses direction mid-transition.
        if (wasAnimating) {
            drawMenuSelectionMotion(_menuFrameX, _menuFrameY, COLOR_BG,
                                    false, _menuAnimationHorizontal);
        } else if (oldSelection < MENU_APP_COUNT) {
            const uint8_t oldCol = oldSelection % MENU_COLUMNS;
            const uint8_t oldRow = oldSelection / MENU_COLUMNS;
            const int16_t oldX = MENU_GRID_X +
                oldCol * (MENU_CELL_W + MENU_CELL_GAP_X);
            const int16_t oldY = MENU_GRID_Y +
                oldRow * (MENU_CELL_H + MENU_CELL_GAP_Y);
            drawMenuSelectionFrame(oldX, oldY, COLOR_BG, false);
        }

        _menuAnimationFrom = oldSelection < MENU_APP_COUNT ? oldSelection : _menuSelection;
        _menuAnimationTo = _menuSelection;
        if (!wasAnimating) {
            const uint8_t col = _menuAnimationFrom % MENU_COLUMNS;
            const uint8_t row = _menuAnimationFrom / MENU_COLUMNS;
            _menuFrameX = MENU_GRID_X + col * (MENU_CELL_W + MENU_CELL_GAP_X);
            _menuFrameY = MENU_GRID_Y + row * (MENU_CELL_H + MENU_CELL_GAP_Y);
        }
        _menuAnimationStartX = _menuFrameX;
        _menuAnimationStartY = _menuFrameY;
        const uint8_t targetCol = _menuAnimationTo % MENU_COLUMNS;
        const uint8_t targetRow = _menuAnimationTo / MENU_COLUMNS;
        const int16_t targetX = MENU_GRID_X +
            targetCol * (MENU_CELL_W + MENU_CELL_GAP_X);
        const int16_t targetY = MENU_GRID_Y +
            targetRow * (MENU_CELL_H + MENU_CELL_GAP_Y);
        const int16_t dx = targetX - _menuAnimationStartX;
        const int16_t dy = targetY - _menuAnimationStartY;
        _menuAnimationHorizontal = (dx < 0 ? -dx : dx) >=
                                   (dy < 0 ? -dy : dy);
        _menuSelectionAnimating = true;
        _menuSelectionAnimStart = millis();
    }

    _lastMenuSelection = _menuSelection;
    _lastMenuRowOffset = _menuRowOffset;

    if (_menuSelectionAnimating) {
        const uint32_t elapsed = millis() - _menuSelectionAnimStart;
        if (elapsed >= MENU_SELECTION_ANIM_MS) {
            drawMenuSelectionMotion(_menuFrameX, _menuFrameY, COLOR_BG,
                                    false, _menuAnimationHorizontal);
            _menuSelectionAnimating = false;
            const uint8_t col = _menuAnimationTo % MENU_COLUMNS;
            const uint8_t row = _menuAnimationTo / MENU_COLUMNS;
            _menuFrameX = MENU_GRID_X + col * (MENU_CELL_W + MENU_CELL_GAP_X);
            _menuFrameY = MENU_GRID_Y + row * (MENU_CELL_H + MENU_CELL_GAP_Y);
            drawMenuSelectionFrame(_menuFrameX, _menuFrameY,
                                   COLOR_ACCENT, false);
        } else {
            // Ease-out interpolation gives the frame a quick directional
            // departure and a softer settle at the destination.
            const uint16_t progress = (uint16_t)(elapsed * 255UL /
                                                  MENU_SELECTION_ANIM_MS);
            const uint16_t inverse = 255U - progress;
            const uint16_t eased = 255U -
                (uint16_t)((inverse * inverse) / 255U);
            const uint8_t col = _menuAnimationTo % MENU_COLUMNS;
            const uint8_t row = _menuAnimationTo / MENU_COLUMNS;
            const int16_t targetX = MENU_GRID_X +
                col * (MENU_CELL_W + MENU_CELL_GAP_X);
            const int16_t targetY = MENU_GRID_Y +
                row * (MENU_CELL_H + MENU_CELL_GAP_Y);

            drawMenuSelectionMotion(_menuFrameX, _menuFrameY, COLOR_BG,
                                    false, _menuAnimationHorizontal);

            _menuFrameX = _menuAnimationStartX +
                (int16_t)(((int32_t)(targetX - _menuAnimationStartX) * eased) / 255);
            _menuFrameY = _menuAnimationStartY +
                (int16_t)(((int32_t)(targetY - _menuAnimationStartY) * eased) / 255);
            const bool highlight = elapsed + MENU_SELECTION_HIGHLIGHT_MS >=
                                   MENU_SELECTION_ANIM_MS;
            const bool pulse = highlight && (((elapsed / 8U) & 1U) != 0U);
            drawMenuSelectionMotion(_menuFrameX, _menuFrameY,
                                    pulse ? COLOR_TEXT : COLOR_ACCENT,
                                    highlight, _menuAnimationHorizontal);
        }
    }
}

// ==================== 音乐播放器（方案 A · 三页滑动）====================
//
// 布局（320x240，状态栏 28px 以下为内容区）：
//   ① NowPlaying  封面 88x88 @ (13,38) | 右侧曲目信息 | 当前歌词 | 进度 | 控制键
//   ② Lyrics      五行歌词居中，当前句大号白字，前后句灰度递减 | 底部进度
//   ③ Spectrum    曲目两行 | 16 条 FFT 柱 | 进度 | 音量
// 底部统一三个页面指示点，摇杆左右切页、左键在第一页时返回应用菜单。

void DisplayClass::setNowPlaying(const char *title, const char *artist,
                                 const char *album, uint32_t durationMs,
                                 const uint16_t *coverRgb565,
                                 int16_t coverW, int16_t coverH) {
    _musicTitle  = title  ? title  : "";
    _musicArtist = artist ? artist : "";
    _musicAlbum  = album  ? album  : "";
    _musicDurationMs = durationMs;
    _musicCover = coverRgb565;
    _musicCoverW = coverW;
    _musicCoverH = coverH;
    _musicLoading = false;
    _musicChromeDirty = true;
    _lastLyricIndex = -2;
}

// 切歌过渡：立即清空旧曲目，三个子页显示加载状态。
// 此时新的歌词/直链还在路上，UI 不能干等网络任务全部完成再跳变。
void DisplayClass::setMusicLoading() {
    _musicTitle = "";
    _musicArtist = "";
    _musicAlbum = "";
    _musicDurationMs = 0;
    _musicCover = nullptr;
    _musicCoverW = 0;
    _musicCoverH = 0;
    _musicLyricCount = 0;
    _musicPositionMs = 0;
    _karClockMs = 0;
    _karClockWallMs = 0;
    _musicLoading = true;
    _musicChromeDirty = true;
    _lastLyricIndex = -2;
    _lastProgressSec = 0xFFFFFFFF;
    _kar = Karaoke();
    _lastMusicState = (PlayerState)0xFF;
    _lastMusicVol = 0xFF;
}

void DisplayClass::setPlaybackPosition(uint32_t positionMs) {
    if (_musicDurationMs && positionMs > _musicDurationMs) {
        positionMs = _musicDurationMs;
    }
    _musicPositionMs = positionMs;

    // 平滑时钟：给歌词扫描用。
    // 音频侧的位置本身是精确的，但 UI 只能在每帧读一次，而解码是
    // 「一帧 MP3 = 26ms 音频」的块状推进 —— UI 读到的是阶梯值，
    // 单帧前进量在 0~52ms 间跳（标准差 13ms），扫描线就一顿一顿。
    //
    // 这里用误差驱动的变速本地时钟跟随它：本地按墙钟流逝，
    // 落后就加速追、超前就减速等，速率钳在 [0, 2×] 保证永不回退
    // （歌词倒退比抖动更难看）。实测单帧抖动降约 25% 且完全单调。
    const uint32_t now = millis();
    if (!_karClockMs || positionMs < _karClockMs) {
        // 首次或跳转/切歌：直接对齐，不做追赶
        _karClockMs = positionMs;
        _karClockWallMs = now;
        return;
    }
    uint32_t dtw = now - _karClockWallMs;
    _karClockWallMs = now;
    if (dtw > 500) dtw = 500;          // 长卡顿后不要一步窜出去

    const int32_t err = (int32_t)positionMs - (int32_t)_karClockMs;
    // rate = 1 + err/τ，τ=165ms；用 Q8 定点，钳到 [0, 2.0]
    int32_t rateQ8 = 256 + (err * 256) / 165;
    if (rateQ8 < 0) rateQ8 = 0;
    if (rateQ8 > 512) rateQ8 = 512;
    _karClockMs += (uint32_t)(((int32_t)dtw * rateQ8) >> 8);
}

void DisplayClass::setLyrics(const LyricLine *lines, uint16_t count) {
    _musicLyricCount = count > kMaxLyricLines ? kMaxLyricLines : count;
    for (uint16_t i = 0; i < _musicLyricCount; i++) {
        _musicLyrics[i] = lines[i];
    }
    _lastLyricIndex = -2;
    _musicChromeDirty = true;
    // String 重新赋值后旧的 c_str() 可能指向已释放内存，
    // 行缓存必须一并作废，否则局部刷新会比对到悬垂指针。
    for (uint8_t i = 0; i < 5; i++) _lyricRowCache[i] = nullptr;
    _kar = Karaoke();
}

int16_t DisplayClass::currentLyricIndex() const {
    if (!_musicLyricCount) return -1;
    int16_t found = -1;
    for (uint16_t i = 0; i < _musicLyricCount; i++) {
        if (_musicLyrics[i].startMs <= _musicPositionMs) found = (int16_t)i;
        else break;
    }
    return found;
}

// 解析当前句的字符边界与 x 布局，供逐字高亮增量重绘。
// 时长取下一句起点，但要按字数封顶——见下方说明。
void DisplayClass::buildKaraokeLine(int16_t lineIdx, int16_t x, int16_t y,
                                    uint8_t size, uint16_t litColor,
                                    uint16_t baseColor, int16_t maxWidth) {
    _kar = Karaoke();
    if (lineIdx < 0 || lineIdx >= (int16_t)_musicLyricCount) return;

    const char *s = _musicLyrics[lineIdx].text.c_str();
    const GFXfont *font = uiFontForSize(size);
    const int16_t unit = 16 * size;

    _kar.lineIdx = lineIdx;
    _kar.x = x;
    _kar.y = y;
    _kar.size = size;
    _kar.litColor = litColor;
    _kar.baseColor = baseColor;
    _kar.startMs = _musicLyrics[lineIdx].startMs;
    if (lineIdx + 1 < (int16_t)_musicLyricCount) {
        _kar.endMs = _musicLyrics[lineIdx + 1].startMs;
    } else if (_musicDurationMs > _kar.startMs) {
        _kar.endMs = _musicDurationMs;
    } else {
        _kar.endMs = _kar.startMs + 8000;
    }

    // 与 measureCjkText 相同的步进规则逐字符记录偏移。
    // 截断行必须与 drawCjkTextClipped 的裁剪完全对齐：绘制端为 ".."
    // 预留了宽度，这里若按整个 maxWidth 收字符，会比屏上多登记 1~2
    // 个字，高亮扫到时就把这些"屏上没有的字"画出来，盖掉 ".."
    // 并越出显示区。
    int16_t budget = maxWidth;
    if (measureCjkText(s, size) > maxWidth) {
        _kar.truncated = 1;
        budget = maxWidth - measureCjkText("..", size);
    }
    int16_t cx = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && _kar.charCount < (uint8_t)(sizeof(_kar.byteOff))) {
        const uint16_t off = (uint16_t)(p - (const uint8_t *)s);
        uint32_t cp;
        uint8_t len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); len = 2;
        } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80) {
            cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            len = 3;
        } else { cp = 0xFFFD; len = 1; }

        const int16_t adv = cp < 0x80
            ? asciiAdvance(font, size, (char)cp) : unit;
        if (cx + adv > budget) break;   // 截断处停止（".." 不参与逐字扫过）

        _kar.byteOff[_kar.charCount] = (uint8_t)off;
        _kar.xOff[_kar.charCount] = cx;
        _kar.charCount++;
        cx += adv;
        p += len;
    }
    _kar.lineW = cx;

    // 句长按字数封顶。LRC 只给句子起点，句间常有 3~15 秒间奏，
    // 直接用「下一句起点 - 本句起点」会把间奏算进本句：一句 8 字
    // 摊到 12 秒 = 每字 1.5 秒，人声早唱完了扫描线还在慢慢爬，
    // 约 70% 的字拖在没有人声的时段里，看起来就是卡住。
    // 中文演唱每字约 320~420ms，取 420ms 上限（宁可扫完等下一句，
    // 也不要跟不上人声）；ASCII 折半计。
    if (_kar.charCount) {
        const uint32_t cap = (uint32_t)_kar.charCount * 420u + 600u;
        if (_kar.endMs - _kar.startMs > cap) _kar.endMs = _kar.startMs + cap;
    }
}

// 分割绘制第 charIndex 个字：左侧 splitPx 列用已唱色，其余用未唱色。
// 仅 CJK 走点阵分割；ASCII 字形由 FreeFont 输出，无法按列分色，
// 过半即整字翻色（拉丁字符窄，视觉差异可忽略）。
// 绘制正在被扫过的字：只重画 [fromPx, toPx) 这几列。
// 逐字扫描时每帧只前进 2~4 列（0.08~0.17 px/ms × 帧长），
// 整字重绘要传 32×32×2=2048 字节，只画变化列约 128~256 字节，省约 90%。
// SPI 占用少了，歌词扫描才不会与音频解码抢总线。
void DisplayClass::drawKaraokeSplitChar(uint8_t charIndex, int16_t fromPx,
                                        int16_t toPx) {
    const char *s = _musicLyrics[_kar.lineIdx].text.c_str();
    const uint8_t *p = (const uint8_t *)s + _kar.byteOff[charIndex];
    const int16_t cx = _kar.x + _kar.xOff[charIndex];
    const uint8_t size = _kar.size;

    uint32_t cp;
    if (*p < 0x80) {
        cp = *p;
    } else if ((*p & 0xE0) == 0xC0) {
        cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
    } else {
        cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }

    if (cp < 0x80) {
        // ASCII：过半整字翻色
        const GFXfont *font = uiFontForSize(size);
        const int16_t adv = asciiAdvance(font, size, (char)cp);
        if (toPx * 2 >= adv && fromPx * 2 < adv) {
            char one[2] = {(char)cp, '\0'};
            drawCjkText(cx, _kar.y, one, size, _kar.litColor, COLOR_BG);
        }
        return;
    }

    // 与 drawCjkText 一致：size=2 用原生 32px 字模，否则放大 16px
    const int16_t px = 16 * size;          // 目标宽高（size≤2 → 最大 32）
    const uint8_t *big = (size == 2) ? CjkFont32.glyph((uint16_t)cp) : nullptr;
    const uint8_t *cov = big ? big : CjkFont.glyph((uint16_t)cp);
    if (!cov) return;
    // 原生字模：源尺寸即目标尺寸；放大路径：源 16px
    const uint8_t srcSize  = big ? 32 : 16;
    const uint8_t srcStride = (uint8_t)(srcSize / 2);

    if (fromPx < 0) fromPx = 0;
    if (toPx > px) toPx = px;
    if (toPx <= fromPx) return;
    const int16_t w = toPx - fromPx;

    // 只构建变化列的条带：新扫过的列一律用已唱色
    static uint16_t buf[32 * 32];
    for (int16_t row = 0; row < px; row++) {
        const uint8_t srcRow = (uint8_t)((int32_t)row * srcSize / px);
        for (int16_t col = 0; col < w; col++) {
            const uint8_t srcCol =
                (uint8_t)((int32_t)(fromPx + col) * srcSize / px);
            const uint8_t packed = cov[srcRow * srcStride + srcCol / 2];
            const uint8_t c4 = (srcCol & 1) ? (packed & 0x0F) : (packed >> 4);
            buf[row * w + col] =
                c4 == 0  ? COLOR_BG
              : c4 >= 15 ? _kar.litColor
              : blendRgb565(COLOR_BG, _kar.litColor,
                            (uint8_t)((c4 * 255 + 7) / 15));
        }
    }
    tft.drawRGBBitmap(cx + fromPx, _kar.y, buf, w, px);
}

// 按播放进度推进高亮。进度换算为句内像素位置：
//   - 已完整越过的字符：整字画成已唱色（每字只画一次）
//   - 正在扫过的字符：内部按列分割重绘，分割线随主循环节奏平滑移动
void DisplayClass::updateKaraoke() {
    if (_kar.lineIdx < 0 || !_kar.charCount || !_kar.lineW) return;
    if (_kar.endMs <= _kar.startMs) return;
    // 扫描位置用平滑时钟：原始 positionMs 是 26ms 步进的阶梯值，
    // 直接用会让扫描线一顿一顿（详见 setPlaybackPosition）
    const uint32_t posMs = _karClockMs ? _karClockMs : _musicPositionMs;
    if (posMs <= _kar.startMs) return;

    int16_t progressPx;
    if (posMs >= _kar.endMs) {
        progressPx = _kar.lineW;
    } else {
        progressPx = (int16_t)(((uint64_t)(posMs - _kar.startMs) *
                                (uint32_t)_kar.lineW) /
                               (_kar.endMs - _kar.startMs));
    }

    const char *s = _musicLyrics[_kar.lineIdx].text.c_str();
    const uint16_t total = (uint16_t)strlen(s);

    // 完整越过的字符整字翻色
    while (_kar.lit < _kar.charCount) {
        const uint8_t i = _kar.lit;
        const int16_t w = ((i + 1 < _kar.charCount) ? _kar.xOff[i + 1]
                                                    : _kar.lineW) -
                          _kar.xOff[i];
        if (_kar.xOff[i] + w > progressPx) break;

        const uint16_t off = _kar.byteOff[i];
        const uint16_t next = (i + 1 < _kar.charCount) ? _kar.byteOff[i + 1]
                                                       : total;
        char one[8];
        uint16_t n = next - off;
        if (n > sizeof(one) - 1) n = sizeof(one) - 1;
        memcpy(one, s + off, n);
        one[n] = '\0';
        drawCjkText(_kar.x + _kar.xOff[i], _kar.y, one, _kar.size,
                    _kar.litColor, COLOR_BG);
        _kar.lit++;
        _kar.lastSplit = -1;   // 进入下一个分割字，分割位置重新起算
    }

    // 截断行的收尾：可见字符全部扫完后，把 ".." 一并翻成已唱色，
    // 高亮止步于此、不越出显示区（后面被裁掉的字不再参与扫过）
    if (_kar.truncated && !_kar.ellipsisLit && _kar.lit >= _kar.charCount) {
        drawCjkText(_kar.x + _kar.lineW, _kar.y, "..", _kar.size,
                    _kar.litColor, COLOR_BG);
        _kar.ellipsisLit = 1;
    }

    // 当前正被扫过的字符：只补画新扫过的列
    if (_kar.lit < _kar.charCount) {
        const int16_t split = progressPx - _kar.xOff[_kar.lit];
        if (split > 0 && split != _kar.lastSplit) {
            // lastSplit=-1 表示刚进入这个字，从第 0 列开始补
            const int16_t from = _kar.lastSplit > 0 ? _kar.lastSplit : 0;
            if (split > from) drawKaraokeSplitChar(_kar.lit, from, split);
            _kar.lastSplit = split;
        }
    }
}

// 定宽绘制中英混排文本，超出部分截断并留出省略号位置
void DisplayClass::drawCjkTextClipped(int16_t x, int16_t y, const char *s,
                                      uint8_t size, uint16_t fg, uint16_t bg,
                                      int16_t maxWidth) {
    if (!s || maxWidth <= 0) return;
    if (measureCjkText(s, size) <= maxWidth) {
        drawCjkText(x, y, s, size, fg, bg);
        return;
    }
    // 逐字节按 UTF-8 边界回退，直到加上 ".." 也能放下
    String cut = s;
    while (cut.length() > 0) {
        // 退到上一个 UTF-8 字符起始处
        int idx = cut.length() - 1;
        while (idx > 0 && ((uint8_t)cut[idx] & 0xC0) == 0x80) idx--;
        cut.remove(idx);
        if (measureCjkText((cut + "..").c_str(), size) <= maxWidth) break;
    }
    cut += "..";
    drawCjkText(x, y, cut.c_str(), size, fg, bg);
}

// iOS 风格加载菊花：12 个圆点绕环，头部亮白、两级尾迹渐隐、其余暗灰。
// 点位固定、只按相位重涂颜色，无需擦除，增量开销可忽略（12 个 r=3 圆点）。
void DisplayClass::drawMusicLoadingSpinner(int16_t cx, int16_t cy, int16_t r,
                                           uint8_t headPhase) {
    for (uint8_t i = 0; i < 12; i++) {
        const uint8_t lag = (uint8_t)((headPhase + 12 - i) % 12);
        const uint16_t c = lag == 0 ? COLOR_TEXT
                         : lag == 1 ? COLOR_LABEL
                         : lag == 2 ? COLOR_DIM
                                    : COLOR_LINE;
        const float a = i * (PI / 6.0f);
        tft.fillCircle(cx + (int16_t)lroundf(cosf(a) * r),
                       cy + (int16_t)lroundf(sinf(a) * r), 3, c);
    }
}

// 封面：有位图则直绘；加载中画菊花；否则画黑胶唱片占位（播放时缓慢旋转）
void DisplayClass::drawMusicCover(int16_t x, int16_t y, int16_t box) {
    if (_musicCover && _musicCoverW > 0 && _musicCoverH > 0) {
        const int16_t w = _musicCoverW < box ? _musicCoverW : box;
        const int16_t h = _musicCoverH < box ? _musicCoverH : box;
        tft.drawRGBBitmap(x + (box - w) / 2, y + (box - h) / 2,
                          (uint16_t *)_musicCover, w, h);
        tft.drawRoundRect(x, y, box, box, 8, COLOR_CARD_EDGE);
        return;
    }

    // 切歌加载中：卡片内画菊花底图，动画相位由 updateMusicPlayer 推进
    if (_musicLoading) {
        tft.fillRoundRect(x, y, box, box, 8, COLOR_CARD);
        tft.drawRoundRect(x, y, box, box, 8, COLOR_CARD_EDGE);
        drawMusicLoadingSpinner(x + box / 2, y + box / 2, box * 25 / 100,
                                _musicSpinPhase);
        return;
    }

    // 占位：圆角卡片 + 同心纹唱片 + 中心蓝点
    tft.fillRoundRect(x, y, box, box, 8, COLOR_CARD);
    tft.drawRoundRect(x, y, box, box, 8, COLOR_CARD_EDGE);
    const int16_t cx = x + box / 2;
    const int16_t cy = y + box / 2;
    const int16_t r  = box * 36 / 100;
    tft.fillCircle(cx, cy, r, COLOR_CARD_TOP);
    // 三圈纹路，随相位偏移形成旋转感
    for (int8_t i = 0; i < 3; i++) {
        const int16_t rr = r - 4 - i * 5;
        if (rr > 3) tft.drawCircle(cx, cy, rr, COLOR_CARD_EDGE);
    }
    // 高光短弧：位置随相位走一圈，暗示唱片在转
    const float a = _musicDiscPhase * (PI / 6.0f);
    const int16_t hx = cx + (int16_t)lroundf(cosf(a) * (r - 3));
    const int16_t hy = cy + (int16_t)lroundf(sinf(a) * (r - 3));
    tft.fillCircle(hx, hy, 2, COLOR_FLIP_LINE);
    tft.fillCircle(cx, cy, box * 9 / 100, COLOR_ACCENT);
    tft.fillCircle(cx, cy, box * 3 / 100, COLOR_BG);
}

// 传输控制图标：kind 0=上一曲 1=播放 2=暂停 3=下一曲
void DisplayClass::drawTransportIcon(int16_t cx, int16_t cy, int8_t kind,
                                     uint16_t color) {
    switch (kind) {
        case 0:   // 上一曲：竖线 + 左三角
            tft.fillRect(cx - 7, cy - 6, 2, 12, color);
            tft.fillTriangle(cx + 7, cy - 6, cx + 7, cy + 6, cx - 4, cy, color);
            break;
        case 1:   // 播放：右三角
            tft.fillTriangle(cx - 5, cy - 7, cx - 5, cy + 7, cx + 7, cy, color);
            break;
        case 2:   // 暂停：双竖条
            tft.fillRoundRect(cx - 6, cy - 7, 4, 14, 1, color);
            tft.fillRoundRect(cx + 2, cy - 7, 4, 14, 1, color);
            break;
        default:  // 下一曲：右三角 + 竖线
            tft.fillTriangle(cx - 7, cy - 6, cx - 7, cy + 6, cx + 4, cy, color);
            tft.fillRect(cx + 5, cy - 6, 2, 12, color);
            break;
    }
}

// 进度条：左右时间 + 中间细条（iOS 风格，无游标圆点）。
// 每秒刷新一次且只做增量：前进补画新增蓝段，回退（切歌/播完归零）
// 把多出的段抹回轨道色——双向都干净，不会残留旧游标或旧进度。
void DisplayClass::drawMusicProgress(int16_t y, bool force) {
    const uint32_t sec = _musicPositionMs / 1000;
    if (!force && sec == _lastProgressSec) return;
    _lastProgressSec = sec;

    constexpr int16_t left = 13;
    constexpr int16_t right = kScrW - 13;
    constexpr int16_t barH = 4;

    char cur[12];
    snprintf(cur, sizeof(cur), "%u:%02u",
             (unsigned)(sec / 60), (unsigned)(sec % 60));
    const uint32_t dsec = _musicDurationMs / 1000;
    char total[12];
    snprintf(total, sizeof(total), "%u:%02u",
             (unsigned)(dsec / 60), (unsigned)(dsec % 60));
    const int16_t curW = measureTextWidth(cur, 1);
    const int16_t totalW = measureTextWidth(total, 1);

    const int16_t barX = left + curW + 8;
    const int16_t barW = right - totalW - 8 - barX;
    const int16_t barY = y + 4;

    if (force) {
        // 首帧画静态部分：总时长文字与整条轨道
        tft.fillRect(left, y - 2, right - left, 14, COLOR_BG);
        drawText(right - totalW, y, total, 1, COLOR_DIM, COLOR_BG);
        if (barW > 4) tft.fillRect(barX, barY, barW, barH, COLOR_LINE);
        _lastProgressFill = 0;
    }

    // 已播时间：只擦这几个字符的宽度
    tft.fillRect(left, y - 2, curW + 6, 14, COLOR_BG);
    drawText(left, y, cur, 1, COLOR_DIM, COLOR_BG);

    if (barW <= 4 || !_musicDurationMs) return;

    int16_t fill = (int16_t)(((uint64_t)barW * _musicPositionMs) /
                             _musicDurationMs);
    if (fill > barW) fill = barW;

    if (fill > _lastProgressFill) {
        // 前进：只补画新增的蓝段
        tft.fillRect(barX + _lastProgressFill, barY,
                     fill - _lastProgressFill, barH, COLOR_ACCENT);
    } else if (fill < _lastProgressFill) {
        // 回退：把多出的段抹回轨道色
        tft.fillRect(barX + fill, barY,
                     _lastProgressFill - fill, barH, COLOR_LINE);
    }
    _lastProgressFill = fill;
}

// 三键控制：上一曲 / 播放暂停（白色实心圆）/ 下一曲
void DisplayClass::drawMusicControls(int16_t cy, bool force) {
    // 播放态来自 I2S 解码器而非蓝牙桩实现
    const PlayerState st = AudioPlayer.isPlaying() ? PlayerState::Playing
                                                   : PlayerState::Paused;
    if (!force && st == _lastMusicState) return;
    _lastMusicState = st;

    constexpr int16_t gap = 46;
    const int16_t cx = kScrW / 2;
    tft.fillRect(cx - gap - 20, cy - 20, 2 * (gap + 20), 40, COLOR_BG);

    drawTransportIcon(cx - gap, cy, 0, COLOR_TEXT);
    drawTransportIcon(cx + gap, cy, 3, COLOR_TEXT);
    // 中间主键：白色实心圆 + 黑色图标（iOS 播放器样式）
    tft.fillCircle(cx, cy, 17, COLOR_TEXT);
    drawTransportIcon(cx, cy, st == PlayerState::Playing ? 2 : 1, COLOR_BG);
}

void DisplayClass::drawMusicVolume(int16_t y, bool force) {
    const uint8_t vol = AudioPlayer.volume();
    if (!force && vol == _lastMusicVol) return;
    _lastMusicVol = vol;

    constexpr int16_t left = 13;
    constexpr int16_t right = kScrW - 13;
    tft.fillRect(left, y - 2, right - left, 14, COLOR_BG);

    // 喇叭图标
    tft.fillTriangle(left, y + 5, left + 6, y, left + 6, y + 10, COLOR_DIM);
    tft.fillRect(left, y + 3, 3, 5, COLOR_DIM);
    tft.drawCircle(left + 9, y + 5, 3, COLOR_DIM);

    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", vol);
    const int16_t bw = measureTextWidth(buf, 1);
    drawText(right - bw, y, buf, 1, COLOR_DIM, COLOR_BG);

    const int16_t barX = left + 18;
    const int16_t barW = right - bw - 8 - barX;
    if (barW <= 4) return;
    tft.fillRoundRect(barX, y + 4, barW, 3, 1, COLOR_LINE);
    const int16_t fill = (int16_t)((barW * (int16_t)vol) / 100);
    if (fill > 0) tft.fillRoundRect(barX, y + 4, fill, 3, 1, COLOR_LABEL);
}

void DisplayClass::drawMusicPageIndicator() {
    constexpr int16_t centerX = kScrW / 2;
    constexpr int16_t y = 233;
    tft.fillRect(centerX - 24, y - 5, 48, 10, COLOR_BG);
    for (uint8_t i = 0; i < 3; i++) {
        const bool on = (uint8_t)_musicView == i;
        const int16_t x = centerX - 12 + i * 12;
        if (on) tft.fillCircle(x, y, 3, COLOR_ACCENT);
        else    tft.fillCircle(x, y, 2, COLOR_LINE);
    }
}

// ---------- ① Now Playing ----------
void DisplayClass::drawMusicNowPlaying(bool force) {
    constexpr int16_t coverX = 13;
    constexpr int16_t coverY = 38;
    constexpr int16_t coverBox = 88;
    constexpr int16_t infoX = coverX + coverBox + 14;
    const int16_t infoW = kScrW - infoX - 13;

    // chrome 重绘（切歌）会清掉整屏，进度条等增量元素必须跟着全量重画
    const bool full = force || _musicChromeDirty;
    if (full) {
        tft.fillRect(0, STATUS_H + 1, kScrW, 240 - STATUS_H - 1, COLOR_BG);
        drawMusicCover(coverX, coverY, coverBox);

        const char *title = _musicTitle.length() ? _musicTitle.c_str()
                             : (_musicLoading ? "Loading..." : "未在播放");
        drawCjkTextClipped(infoX, coverY + 6, title, 1, COLOR_TEXT, COLOR_BG,
                           infoW);
        if (_musicArtist.length()) {
            drawCjkTextClipped(infoX, coverY + 30, _musicArtist.c_str(), 1,
                               COLOR_LABEL, COLOR_BG, infoW);
        }
        if (_musicAlbum.length()) {
            drawCjkTextClipped(infoX, coverY + 52, _musicAlbum.c_str(), 1,
                               COLOR_DIM, COLOR_BG, infoW);
        }
        _musicChromeDirty = false;
        _lastLyricIndex = -2;
        _lastProgressSec = 0xFFFFFFFF;
        _lastMusicState = (PlayerState)0xFF;
        drawMusicPageIndicator();
    }

    // 当前歌词一行，居中
    const int16_t idx = currentLyricIndex();
    if (force || idx != _lastLyricIndex) {
        _lastLyricIndex = idx;
        constexpr int16_t ly = 140;
        tft.fillRect(0, ly - 2, kScrW, 22, COLOR_BG);
        if (idx >= 0) {
            const char *line = _musicLyrics[idx].text.c_str();
            const int16_t w = measureCjkText(line, 1);
            const int16_t x = w < kScrW - 26 ? (kScrW - w) / 2 : 13;
            // 未唱灰色打底，系统蓝逐字扫过（updateKaraoke 增量补画）
            drawCjkTextClipped(x, ly, line, 1, COLOR_LABEL, COLOR_BG,
                               kScrW - 26);
            buildKaraokeLine(idx, x, ly, 1, COLOR_ACCENT, COLOR_LABEL,
                             kScrW - 26);
        } else {
            _kar = Karaoke();
        }
    }

    drawMusicProgress(170, full);
    drawMusicControls(210, full);
}

// ---------- ② 歌词全屏 ----------
// 逐行局部刷新：整块 320x172 清屏经 SPI 要 100ms+，切句时会明显闪烁，
// 且与解码任务抢总线会让歌词"闪一下就没了"。这里每行只清自己那条带，
// 并缓存上一帧各行的文本指针，内容没变的行完全不重画。
void DisplayClass::drawMusicLyrics(bool force) {
    const int16_t idx = currentLyricIndex();
    const bool full = force || _musicChromeDirty;

    if (!full && idx == _lastLyricIndex) {
        drawMusicProgress(212, false);
        return;
    }
    _musicChromeDirty = false;
    _lastLyricIndex = idx;

    if (full) {
        tft.fillRect(0, STATUS_H + 1, kScrW, 240 - STATUS_H - 1, COLOR_BG);
        _lastProgressSec = 0xFFFFFFFF;
        for (uint8_t i = 0; i < 5; i++) _lyricRowCache[i] = nullptr;
        drawMusicPageIndicator();
    }

    if (!_musicLyricCount) {
        const char *tip = _musicLoading ? "Loading..." : "暂无歌词";
        const int16_t w = measureCjkText(tip, 1);
        tft.fillRect(0, 100, kScrW, 22, COLOR_BG);
        drawCjkText((kScrW - w) / 2, 102, tip, 1, COLOR_DIM, COLOR_BG);
        drawMusicProgress(212, full);
        return;
    }

    // 五行布局：当前句居中放大，上下各两句渐隐（模拟 Apple Music 淡出）
    constexpr int16_t centerY = 104;
    constexpr int16_t stepNear = 34;   // 当前句与相邻句的间距
    constexpr int16_t stepFar  = 26;   // 再外一层的间距
    constexpr int16_t rowH     = 22;   // 普通行清除带高度
    constexpr int16_t curRowH  = 34;   // 当前句（size2）清除带更高

    for (int8_t off = -2; off <= 2; off++) {
        const uint8_t slot = (uint8_t)(off + 2);
        const int16_t i = idx + off;
        const int8_t dist = off < 0 ? -off : off;

        int16_t y = centerY;
        if (off < 0) y -= stepNear + (dist - 1) * stepFar;
        else if (off > 0) y += stepNear + (dist - 1) * stepFar;

        const char *line = (i >= 0 && i < (int16_t)_musicLyricCount)
                               ? _musicLyrics[i].text.c_str() : nullptr;
        // 内容与上一帧相同：跳过，避免无谓的 SPI 传输
        if (!full && line == _lyricRowCache[slot]) continue;
        _lyricRowCache[slot] = line;

        const uint8_t sz = (off == 0) ? 2 : 1;
        const int16_t bandH = (off == 0) ? curRowH : rowH;
        const int16_t bandY = (off == 0) ? y - 2 : y - 3;
        // 只清这一行所在的窄带
        tft.fillRect(0, bandY, kScrW, bandH, COLOR_BG);
        if (!line) continue;

        const uint16_t color = (off == 0) ? COLOR_TEXT
                             : (dist == 1 ? COLOR_LABEL : 0x630C);  // 递减灰
        const int16_t w = measureCjkText(line, sz);
        const int16_t x = w < kScrW - 24 ? (kScrW - w) / 2 : 12;
        drawCjkTextClipped(x, y, line, sz, color, COLOR_BG, kScrW - 24);
        if (off == 0) {
            // 当前句登记逐字高亮：白色未唱，系统蓝随演唱进度扫过
            buildKaraokeLine(i, x, y, sz, COLOR_ACCENT, COLOR_TEXT,
                             kScrW - 24);
        }
    }

    drawMusicProgress(212, full);
}

// ---------- ③ 频谱 ----------
void DisplayClass::drawMusicSpectrum(bool force) {
    // 中心声波（方案 B）：柱子以水平中线为轴上下对称伸展，
    // 低频排中间、高频往两侧，白色中线贯穿。
    // 平滑链（治抽搐）：① FFT 侧 25fps，升/降都朝目标指数逼近
    //                  ② 相邻柱 [1,2,1] 空间平滑，轮廓像连续声波
    //                  ③ 显示层 Q4 亚像素插值 + 单帧限速（50fps）
    constexpr uint8_t  bars   = 24;
    constexpr int16_t barGap  = 3;
    constexpr int16_t midY    = 122;   // 对称轴
    constexpr int16_t maxHalf = 50;    // 单侧最大伸展（上顶标题线下沿）

    const bool full = force || _musicChromeDirty;
    if (full) {
        tft.fillRect(0, STATUS_H + 1, kScrW, 240 - STATUS_H - 1, COLOR_BG);
        const char *title = _musicTitle.length() ? _musicTitle.c_str()
                             : (_musicLoading ? "Loading..." : "未在播放");
        drawCjkTextClipped(13, 34, title, 1, COLOR_TEXT, COLOR_BG, kScrW - 26);
        if (_musicArtist.length()) {
            drawCjkTextClipped(13, 52, _musicArtist.c_str(), 1, COLOR_LABEL,
                               COLOR_BG, kScrW - 26);
        }
        tft.drawFastHLine(13, 71, kScrW - 26, COLOR_LINE);
        tft.drawFastHLine(13, midY, kScrW - 26, COLOR_TEXT);
        memset(_barH, 0, sizeof(_barH));
        memset(_barHQ, 0, sizeof(_barHQ));
        _musicChromeDirty = false;
        _lastProgressSec = 0xFFFFFFFF;
        _lastMusicVol = 0xFF;
        drawMusicPageIndicator();
    }

    const auto &levels = Spectrum.getLevels();
    const int16_t barW = (kScrW - 26 - (bars - 1) * barGap) / bars;
    const int16_t x0 = (kScrW - (barW * bars + (bars - 1) * barGap)) / 2;

    // 第一遍：算出每根柱的目标高度（对称排布 + 空间平滑）
    int16_t target[bars];
    for (uint8_t i = 0; i < bars; i++) {
        const uint8_t half = bars / 2;                       // 12
        const uint8_t dist = i < half ? (uint8_t)(half - 1 - i)
                                      : (uint8_t)(i - half); // 0..11 距中心
        const uint8_t srcIdx = (uint8_t)((dist * 16) / half);
        const uint8_t v = (srcIdx < levels.size()) ? levels[srcIdx] : 0;
        // 线性映射：最大化摆动幅度（FFT 侧已做开方压缩，这里不再压）
        int16_t h = (int16_t)((int32_t)v * maxHalf / 255);
        if (h > maxHalf) h = maxHalf;
        target[i] = h;
    }
    // [1,2,1]/4 空间平滑：相邻柱高连续，轮廓像声波而不是一排孤柱
    // （+2 四舍五入：截断误差会让边界值逐帧翻动，产生 1px 闪跳）
    int16_t smooth[bars];
    for (uint8_t i = 0; i < bars; i++) {
        const int16_t l = i > 0 ? target[i - 1] : target[i];
        const int16_t r = i < bars - 1 ? target[i + 1] : target[i];
        int16_t h = (int16_t)((l + target[i] * 2 + r + 2) / 4);
        if (h < 6) h = 6;   // 最小 6px：单侧胶囊完整圆头（示例图的静止态）
        smooth[i] = h;
    }

    // 第二遍：Q4 亚像素插值（按真实帧间隔收敛）+ 单帧限速，差异条带绘制。
    // 两处治抽搐的要点：
    //   ① 亚像素：整数插值步长忽大忽小（差8走2px、差2走1px、差1强制±1），
    //      Q4 域（h×16）能以 <1px 的步幅连续累积。
    //   ② dt 感知：主循环周期在 23~48ms 间浮动，固定分数步长会让
    //      视觉速度跟着抖。alpha=dt/(dt+τ)，τ=70ms，帧长变了速度不变。
    // 关键性能点：不整根重画。柱分解为「圆头帽(8px) + 柱身」，
    // 每帧只传输高度差 Δ 的条带和重定位的圆头——SPI 流量降到 1/3，
    // 24 根柱同时舞动也不会压垮总线（此前整根重画导致全局卡顿）。
    const uint32_t nowMs = millis();
    uint32_t frameDt = nowMs - _specFrameMs;
    _specFrameMs = nowMs;
    if (frameDt < 1) frameDt = 1;
    if (frameDt > 200) frameDt = 200;   // 切页/长卡顿后不要一步到位
    const uint32_t alphaQ8 = frameDt * 256u / (frameDt + 70u);   // τ=70ms

    constexpr int16_t capH = 8;   // 圆头帽高（含 r=4 圆角）
    for (uint8_t i = 0; i < bars; i++) {
        const int16_t cur = _barH[i];
        const int16_t hq = (int16_t)_barHQ[i];
        const int16_t targetQ = (int16_t)(smooth[i] << 4);
        int16_t stepQ = (int16_t)(((int32_t)(targetQ - hq) * (int32_t)alphaQ8) >> 8);
        if (stepQ == 0 && targetQ != hq) stepQ = targetQ > hq ? 1 : -1;
        if (stepQ > 80) stepQ = 80;          // 限速 ±5px/帧
        else if (stepQ < -80) stepQ = -80;
        _barHQ[i] = (uint16_t)(hq + stepQ);
        int16_t h = (int16_t)((_barHQ[i] + 8) >> 4);   // 四舍五入到像素
        if (h < capH / 2 + 1) h = capH / 2 + 1;   // 保证圆头完整
        if (!force && h == cur) continue;

        const int16_t x = x0 + i * (barW + barGap);
        const uint8_t half = bars / 2;
        const uint8_t dist = i < half ? (uint8_t)(half - 1 - i)
                                      : (uint8_t)(i - half);
        uint16_t c;
        if (dist < 6) {
            c = blendRgb565(COLOR_ACCENT, 0x669F, (uint8_t)(dist * 255 / 5));
        } else {
            c = blendRgb565(0x669F, 0xBADE, (uint8_t)((dist - 6) * 255 / 5));
        }

        if (force || cur == 0) {
            // 全量：整根胶囊一次画出
            tft.fillRoundRect(x, midY - h, barW, h * 2 + 1, barW / 2, c);
        } else if (h > cur) {
            const int16_t d = h - cur;
            // 圆头弧只存在于帽的外侧半段（capH/2）。原则：
            // 柱身补差 d → 清出新弧区 → 画圆头。弧区必须先清后画，
            // 否则旧方角会凸出在新弧线外；帽的内侧半段与柱身同色重叠。
            // 上侧
            tft.fillRect(x, midY - h + capH / 2, barW, d, c);
            tft.fillRect(x, midY - h, barW, capH / 2, COLOR_BG);
            tft.fillRoundRect(x, midY - h, barW, capH, barW / 2, c);
            // 下侧镜像
            tft.fillRect(x, midY + cur + 1 - capH / 2, barW, d, c);
            tft.fillRect(x, midY + h + 1 - capH / 2, barW, capH / 2,
                         COLOR_BG);
            tft.fillRoundRect(x, midY + h + 1 - capH, barW, capH,
                              barW / 2, c);
        } else {
            const int16_t d = cur - h;
            // 收缩：清掉缩短段+旧弧区（连续一段），再画新圆头。
            // 只清到新弧区为止——多清会在弧角外留下背景缺口，
            // 柱底在方角/圆角间逐帧翻动就是闪烁感的来源。
            // 上侧
            tft.fillRect(x, midY - cur, barW, d + capH / 2, COLOR_BG);
            tft.fillRoundRect(x, midY - h, barW, capH, barW / 2, c);
            // 下侧镜像
            tft.fillRect(x, midY + h + 1 - capH / 2, barW, d + capH / 2,
                         COLOR_BG);
            tft.fillRoundRect(x, midY + h + 1 - capH, barW, capH,
                              barW / 2, c);
        }
        tft.drawFastHLine(x, midY, barW, COLOR_TEXT);
        _barH[i] = (uint8_t)h;
    }

    drawMusicProgress(182, full);
    drawMusicVolume(206, full);
}

void DisplayClass::showMusicView(MusicView view, bool force) {
    if (!force && _musicView == view) return;
    _musicView = view;
    _musicChromeDirty = true;
    _lastLyricIndex = -2;
    _kar = Karaoke();   // 换页后由新页面的绘制重新登记逐字高亮
    _lastProgressSec = 0xFFFFFFFF;
    _lastMusicState = (PlayerState)0xFF;
    _lastMusicVol = 0xFF;
    updateMusicPlayer(true);
    Serial.printf("[DISP] music view %u/3\n", (unsigned)_musicView + 1U);
}

void DisplayClass::showMusicPlayer(bool force) {
    if (!force && _page == Page::Music) return;
    const bool entering = _page != Page::Music;
    _page = Page::Music;
    _musicView = MusicView::NowPlaying;
    _musicChromeDirty = true;
    _lastLyricIndex = -2;
    _lastProgressSec = 0xFFFFFFFF;
    _lastMusicState = (PlayerState)0xFF;
    _lastMusicVol = 0xFF;
    // 从别的页面进来才请求自动播放；页内强制重绘不应打断当前播放
    if (entering) _musicEnterRequested = true;

    fillScreenBg();
    drawStatusBar(true);
    updateMusicPlayer(true);
}

bool DisplayClass::takeMusicEnterRequest() {
    if (!_musicEnterRequested) return false;
    _musicEnterRequested = false;
    return true;
}

// ---------- 语音助手对话页（方案2 全屏） ----------
// 会话激活（唤醒词/长按）时由 main 调用进入，状态驱动刷新：
//   Listening/UserSpeaking → 波形 + "聆听中"
//   Waiting                → 菊花 + "思考中"
//   Playing                → 波形 + 回复文字
//   Error/Disconnected     → 错误提示
// 左键/播放键退出会话（main 层处理）。
void DisplayClass::showVoicePage(bool force) {
    if (!force && _page == Page::Voice) return;
    _page = Page::Voice;
    fillScreenBg();
    drawStatusBar(true);
    updateVoicePage(true);
}

void DisplayClass::showHome() {
    // 语音会话结束回首页：欢迎页即主页（私有 showWelcome 的公开入口）
    showWelcome(true);
}

void DisplayClass::updateVoicePage(bool force) {
    if (_page != Page::Voice) return;

    const auto st = VoiceAssistant.state();
    const uint32_t now = millis();

    // 状态文字：只在变化时重绘（避免整行反复擦写）
    const char *statusText = "语音助手";
    switch (st) {
        case VoiceAssistantClass::State::Listening:
        case VoiceAssistantClass::State::UserSpeaking:
            statusText = "聆听中";
            break;
        case VoiceAssistantClass::State::Waiting:
            statusText = "思考中";
            break;
        case VoiceAssistantClass::State::Playing:
            statusText = "回复中";
            break;
        case VoiceAssistantClass::State::Connecting:
        case VoiceAssistantClass::State::Disconnected:
            statusText = "连接中";
            break;
        case VoiceAssistantClass::State::Error:
            statusText = "出错了";
            break;
        default:
            break;
    }
    static const char *lastStatus = nullptr;
    if (force || lastStatus != statusText) {
        lastStatus = statusText;
        // 只清这一行所在的条带再重画
        tft.fillRect(0, 148, kScrW, 30, COLOR_BG);
        const int16_t tw = measureCjkText(statusText, 2);
        drawCjkText((kScrW - tw) / 2, 150, statusText, 2, COLOR_TEXT,
                    COLOR_BG);
    }

    // 波形区域：仅重画窄条带（约 70px 高），不清整屏
    constexpr int16_t waveTop = 76;
    constexpr int16_t waveH   = 56;
    tft.fillRect(0, waveTop, kScrW, waveH, COLOR_BG);

    bool showWave = (st == VoiceAssistantClass::State::Listening ||
                     st == VoiceAssistantClass::State::UserSpeaking ||
                     st == VoiceAssistantClass::State::Playing);
    if (showWave) {
        const auto &levels = Spectrum.getLevels();
        constexpr int16_t cy = waveTop + waveH / 2;
        constexpr int16_t barW = 8;
        constexpr int16_t gap = 3;
        constexpr int16_t totalW = 24 * (barW + gap) - gap;
        const int16_t x0 = (kScrW - totalW) / 2;
        for (uint8_t i = 0; i < 24 && i < levels.size(); i++) {
            uint16_t h = (uint16_t)(levels[i] * 2);
            if (h > waveH - 4) h = waveH - 4;
            tft.fillRect(x0 + i * (barW + gap), cy - h / 2, barW, h,
                         COLOR_ACCENT);
        }
        tft.fillRect(x0, cy, totalW, 1, COLOR_LINE);
    } else {
        // 连接/思考：居中菊花，仅 12 点增量重涂
        drawMusicLoadingSpinner(kScrW / 2, waveTop + waveH / 2, 22,
                                (uint8_t)(now / 90 % 12));
    }

    // 用户实时说话内容（transcript.user 事件驱动），只在内容变化时重绘
    const char *userText = VoiceAssistant.lastUserText();
    if (userText) {
        static const char *lastText = nullptr;
        if (force || lastText != userText) {
            lastText = userText;
            tft.fillRect(0, 180, kScrW, 26, COLOR_BG);
            const int16_t tw2 = measureCjkText(userText, 1);
            const int16_t x = tw2 < kScrW - 24 ? (kScrW - tw2) / 2 : 12;
            drawCjkTextClipped(x, 182, userText, 1, COLOR_LABEL, COLOR_BG,
                               kScrW - 24);
        }
    }
}

void DisplayClass::updateMusicPlayer(bool force) {
    // 播放期间限制刷新频率：主循环 20ms 一轮，但播放页的信息
    // （进度按秒、歌词按句）没有那么快的变化。降到 ~10fps 可显著减少
    // SPI 占用，给音频解码留出带宽，避免 I2S 欠载爆音。
    if (!force) {
        const uint32_t now = millis();
        if (now - _musicRefreshMs < 100) {
            // 页面元素 10fps 足够，但两类动画要跟主循环节奏（20ms）：
            // ① 逐字歌词扫过 ② 频谱插值动画——都为增量绘制，开销极小。
            updateKaraoke();
            if (_musicView == MusicView::Spectrum) drawMusicSpectrum(false);
            return;
        }
        _musicRefreshMs = now;
    }

    drawStatusBar(force);

    // 切歌加载中：封面框内菊花转圈（每步 90ms，一圈约 1.1s）。
    // 点位固定仅重涂颜色，不清屏不擦除，SPI 占用可忽略。
    if (_musicLoading && _musicView == MusicView::NowPlaying &&
        !_musicChromeDirty) {
        const uint32_t now = millis();
        if (now - _musicSpinMs >= 90) {
            _musicSpinMs = now;
            _musicSpinPhase = (uint8_t)((_musicSpinPhase + 1) % 12);
            constexpr int16_t box = 88;
            drawMusicLoadingSpinner(13 + box / 2, 38 + box / 2,
                                    box * 25 / 100, _musicSpinPhase);
        }
    }

    // 无封面时唱片高光缓慢绕圈（仅播放中，加载态让位给菊花）。
    // 只擦旧点、画新点，不重绘整张 88x88 卡片——后者每 250ms 一次
    // 会长时间占用 SPI，把音频解码挤到欠载而产生爆音。
    if (!_musicCover && !_musicLoading && AudioPlayer.isPlaying() &&
        _musicView == MusicView::NowPlaying && !_musicChromeDirty) {
        const uint32_t now = millis();
        if (now - _musicDiscMs >= 250) {
            _musicDiscMs = now;
            constexpr int16_t box = 88;
            const int16_t cx = 13 + box / 2;
            const int16_t cy = 38 + box / 2;
            const int16_t r  = box * 36 / 100;

            // 擦除上一相位的高光：用唱片盘面色补回
            const float oldA = _musicDiscPhase * (PI / 6.0f);
            tft.fillCircle(cx + (int16_t)lroundf(cosf(oldA) * (r - 3)),
                           cy + (int16_t)lroundf(sinf(oldA) * (r - 3)),
                           2, COLOR_CARD_TOP);

            _musicDiscPhase = (uint8_t)((_musicDiscPhase + 1) % 12);
            const float a = _musicDiscPhase * (PI / 6.0f);
            tft.fillCircle(cx + (int16_t)lroundf(cosf(a) * (r - 3)),
                           cy + (int16_t)lroundf(sinf(a) * (r - 3)),
                           2, COLOR_FLIP_LINE);
        }
    }

    switch (_musicView) {
        case MusicView::Lyrics:   drawMusicLyrics(force);   break;
        case MusicView::Spectrum: drawMusicSpectrum(force); break;
        default:                  drawMusicNowPlaying(force); break;
    }

    // 逐字高亮推进（频谱页无歌词行，_kar 未登记时自然跳过）
    updateKaraoke();
}

void DisplayClass::showNeteaseCloud(bool force) {
    if (!force && _page == Page::NeteaseCloud) return;
    _page = Page::NeteaseCloud;
    fillScreenBg();
    drawStatusBar(true);

    drawMusicIcon(135, 69, 50, true);
    const int16_t titleW = measureTextWidth("MUSIC", 2);
    drawText((kScrW - titleW) / 2, 142, "MUSIC", 2, COLOR_TEXT, COLOR_BG);
    drawText(106, 169, "APP PLACEHOLDER", 1, COLOR_ACCENT, COLOR_BG);
    drawText(82, 190, "Service integration pending", 1, COLOR_DIM, COLOR_BG);
    drawText(16, 222, "LEFT  BACK TO APPS", 1, COLOR_ACCENT, COLOR_BG);
}

// ==================== 听书列表页 ====================
// 数据来自 /api/music/fm/cell_change（听书单元），展示书名+作者。
// 确定键暂显示简介；正文/音频接口确认后再接。
void DisplayClass::showBooks(bool force) {
    if (!force && _page == Page::Books) return;
    _page = Page::Books;
    _booksSelection = 0;
    _lastBooksSelection = 0xFF;
    _booksDirty = true;
    _bookListRequested = true;
    fillScreenBg();
    drawStatusBar(true);
    updateBooks(true);
}

bool DisplayClass::takeBookListRequest() {
    const bool r = _bookListRequested;
    _bookListRequested = false;
    return r;
}

void DisplayClass::booksMove(int8_t delta) {
    if (_page != Page::Books) return;
    const int16_t total = (int16_t)MusicService.bookCount();
    if (total <= 0) return;
    int16_t next = (int16_t)_booksSelection + delta;
    if (next < 0) next = total - 1;
    if (next >= total) next = 0;
    _booksSelection = (uint8_t)next;
    noteActivity();
}

void DisplayClass::booksActivate() {
    if (_page != Page::Books) return;
    const uint8_t total = MusicService.bookCount();
    if (_booksSelection >= total) return;
    noteActivity();
    // 暂无正文/音频接口：确定后显示简介（后续替换为正文页）
    _booksDirty = true;
    _showBookDetail = !_showBookDetail;
}

void DisplayClass::updateBooks(bool force) {
    const uint8_t total = MusicService.bookCount();
    const bool changed = force || _booksDirty || total != _booksCount ||
                         _booksSelection != _lastBooksSelection;
    if (!changed) return;
    _booksDirty = false;
    _booksCount = total;
    _lastBooksSelection = _booksSelection;

    tft.fillRect(0, STATUS_H + 1, kScrW, 240 - STATUS_H - 1, COLOR_BG);

    // 标题
    const char *title = "听书";
    const int16_t tw = measureCjkText(title, 1);
    drawCjkText((kScrW - tw) / 2, 34, title, 1, COLOR_ACCENT, COLOR_BG);
    tft.drawFastHLine(16, 56, kScrW - 32, COLOR_LINE);

    if (total == 0) {
        const char *tip = _bookListRequested ? "加载中..." : "暂无书籍";
        const int16_t tw2 = measureCjkText(tip, 1);
        drawCjkText((kScrW - tw2) / 2, 140, tip, 1, COLOR_DIM, COLOR_BG);
        return;
    }

    if (_showBookDetail) {
        // 简介页
        MusicServiceClass::Book b;
        if (!MusicService.book(_booksSelection, b)) {
            _showBookDetail = false;
            return;
        }
        const char *tip = "简介";
        const int16_t tw2 = measureCjkText(tip, 1);
        drawCjkText((kScrW - tw2) / 2, 70, tip, 1, COLOR_ACCENT, COLOR_BG);
        drawCjkTextClipped(16, 92, b.name, 1, COLOR_TEXT, COLOR_BG,
                           kScrW - 32);
        drawCjkTextClipped(16, 118, b.abstract, 1, COLOR_LABEL, COLOR_BG,
                           kScrW - 32);
        const char *back = "左键返回";
        const int16_t bw = measureCjkText(back, 1);
        drawCjkText((kScrW - bw) / 2, 220, back, 1, COLOR_DIM, COLOR_BG);
        return;
    }

    // 列表：每行 46px，显示书名+作者，最多 4 行
    constexpr int16_t rowY0 = 62;
    constexpr int16_t rowH = 44;
    constexpr uint8_t maxRows = 4;
    const uint8_t startRow = _booksSelection >= maxRows
                                 ? _booksSelection - maxRows + 1 : 0;
    for (uint8_t row = 0; row < maxRows; row++) {
        const uint8_t idx = startRow + row;
        if (idx >= total) break;
        const int16_t y = rowY0 + row * rowH;

        MusicServiceClass::Book b;
        if (!MusicService.book(idx, b)) break;
        const bool isSel = idx == _booksSelection;

        tft.fillRect(0, y, kScrW, rowH - 2, COLOR_BG);
        // 书名
        const uint16_t nameFg = isSel ? COLOR_ACCENT : COLOR_TEXT;
        drawCjkTextClipped(16, y + 4, b.name, 1, nameFg, COLOR_BG,
                           kScrW - 40);
        // 作者
        if (b.author[0]) {
            drawCjkTextClipped(16, y + 24, b.author, 1, COLOR_DIM, COLOR_BG,
                               kScrW - 40);
        }
        tft.drawFastHLine(16, y + rowH - 2, kScrW - 32, COLOR_LINE);

        if (isSel) {
            tft.fillRect(6, y + 4, 3, rowH - 8, COLOR_ACCENT);
        }
    }
}

// ---------- 首页双页轮换 ----------

void DisplayClass::drawHomePageIndicator() {
    constexpr int16_t centerX = kScrW / 2;
    constexpr int16_t y = 234;
    tft.fillRect(centerX - 18, y - 5, 36, 10, COLOR_BG);
    for (uint8_t i = 0; i < 2; i++) {
        const bool selected = (uint8_t)_homeView == i;
        const int16_t x = centerX - 6 + i * 12;
        if (selected) {
            tft.fillCircle(x, y, 3, COLOR_ACCENT);
        } else {
            tft.drawCircle(x, y, 2, COLOR_DIM);
        }
    }
}

void DisplayClass::drawWeatherDashboard(bool force) {
    const bool dateOrMinuteChanged =
        _clockM != _lastWeatherDashboardMinute ||
        _clockY != _lastWeatherDashboardYear ||
        _clockMo != _lastWeatherDashboardMonth ||
        _clockD != _lastWeatherDashboardDay;
    if (!force && !_weatherDashboardDirty && !dateOrMinuteChanged) return;

    _weatherDashboardDirty = false;
    _lastWeatherDashboardMinute = _clockM;
    _lastWeatherDashboardYear = _clockY;
    _lastWeatherDashboardMonth = _clockMo;
    _lastWeatherDashboardDay = _clockD;

    constexpr int16_t contentY = STATUS_H + 1;
    constexpr uint16_t TREND_HIGH = 0xFCE1;  // #FF9F0A 系统橙
    constexpr uint16_t TREND_LOW  = 0x669F;  // #64D2FF 天青
    tft.fillRect(0, contentY, kScrW, 240 - contentY, COLOR_BG);

    const uint8_t count = _weatherSnapshot.forecastCount > WeatherClass::kForecastDays
        ? WeatherClass::kForecastDays : _weatherSnapshot.forecastCount;

    // ---------- 顶部：城市 + 天气状况 | 时间 ----------
    int16_t headX = 13;
    const bool cityCjk = !(_weatherValid && _weatherCity.length());
    if (cityCjk) {
        drawCjkText(headX, 34, "本地天气", 1, COLOR_ACCENT, COLOR_BG);
        headX += measureCjkText("本地天气", 1);
    } else {
        String city = _weatherCity;
        city.toUpperCase();
        if (measureTextWidth(city.c_str(), 1) > 150) {
            while (city.length() > 3 &&
                   measureTextWidth((city + "...").c_str(), 1) > 150) {
                city.remove(city.length() - 1);
            }
            city += "...";
        }
        drawText(headX, 34, city.c_str(), 1, COLOR_ACCENT, COLOR_BG);
        headX += measureTextWidth(city.c_str(), 1);
    }

    // 天气状况紧跟城市之后，避免与下方大号温度重叠
    const char *condition = _weatherValid
        ? weatherIconLabelCn(_weatherSnapshot.icon)
        : (_wifiOn ? "天气更新中" : "天气离线");
    drawCjkText(headX + 10, 33, condition, 1, COLOR_LABEL, COLOR_BG);

    char clockText[8];
    snprintf(clockText, sizeof(clockText), "%02u:%02u", _clockH, _clockM);
    const int16_t clockW = measureTextWidth(clockText, 1);
    drawText(kScrW - 13 - clockW, 34, clockText, 1, COLOR_DIM, COLOR_BG);

    // ---------- 大号温度 + 天气图标 ----------
    if (_weatherValid && _weatherSnapshot.tempC != -128) {
        char tempText[8];
        snprintf(tempText, sizeof(tempText), "%d", (int)_weatherSnapshot.tempC);
        drawText(13, 58, tempText, 4, COLOR_TEXT, COLOR_BG);
        const int16_t tempW = measureTextWidth(tempText, 4);
        const int16_t degreeX = 13 + tempW + 7;
        tft.drawCircle(degreeX, 63, 3, TREND_HIGH);
        tft.drawCircle(degreeX, 63, 2, TREND_HIGH);
        drawText(degreeX + 8, 74, "C", 2, TREND_HIGH, COLOR_BG);
        drawWeatherIcon(270, 76, 20, _weatherSnapshot.icon, false);
    } else {
        drawText(13, 58, "--", 4, COLOR_DIM, COLOR_BG);
        drawWeatherIcon(270, 76, 20, WeatherIcon::Unknown, true);
    }

    // ---------- 三项指标：无卡片，只使用细分隔线 ----------
    constexpr int16_t metricLeft[] = {18, 119, 220};
    constexpr const char *metricLabels[] = {"湿度", "降水", "风力"};
    char metricValues[3][16];
    if (_weatherValid && _weatherSnapshot.humidityPct <= 100) {
        snprintf(metricValues[0], sizeof(metricValues[0]), "%u%%",
                 (unsigned)_weatherSnapshot.humidityPct);
    } else {
        snprintf(metricValues[0], sizeof(metricValues[0]), "--");
    }
    if (_weatherValid && _weatherSnapshot.rainChancePct <= 100) {
        snprintf(metricValues[1], sizeof(metricValues[1]), "%u%%",
                 (unsigned)_weatherSnapshot.rainChancePct);
    } else {
        snprintf(metricValues[1], sizeof(metricValues[1]), "--");
    }
    if (_weatherValid && _weatherSnapshot.windKph != 0xFFFF) {
        snprintf(metricValues[2], sizeof(metricValues[2]), "%u km/h",
                 (unsigned)_weatherSnapshot.windKph);
    } else {
        snprintf(metricValues[2], sizeof(metricValues[2]), "--");
    }

    for (uint8_t i = 0; i < 3; i++) {
        drawCjkText(metricLeft[i], 107, metricLabels[i], 1, COLOR_LABEL,
                    COLOR_BG);
        drawText(metricLeft[i], 119, metricValues[i], 2,
                 COLOR_TEXT, COLOR_BG);
    }
    tft.drawFastVLine(100, 106, 27, COLOR_LINE);
    tft.drawFastVLine(201, 106, 27, COLOR_LINE);

    // ---------- 放大的温度走势：连续温差带 + 无节点双曲线 ----------
    drawCjkText(13, 142, "温度趋势", 1, COLOR_LABEL, COLOR_BG);
    tft.fillCircle(152, 148, 2, TREND_HIGH);
    drawCjkText(158, 142, "高", 1, COLOR_LABEL, COLOR_BG);
    tft.fillCircle(182, 148, 2, TREND_LOW);
    drawCjkText(188, 142, "低", 1, COLOR_LABEL, COLOR_BG);

    bool graphReady = _weatherValid && count >= 2;
    for (uint8_t i = 0; graphReady && i < count; i++) {
        if (_weatherSnapshot.forecast[i].maxC == -128 ||
            _weatherSnapshot.forecast[i].minC == -128) {
            graphReady = false;
        }
    }

    constexpr int16_t chartTop = 157;
    constexpr int16_t chartBottom = 184;
    if (graphReady) {
        int16_t minTemp = 100;
        int16_t maxTemp = -100;
        for (uint8_t i = 0; i < count; i++) {
            const int16_t high = _weatherSnapshot.forecast[i].maxC;
            const int16_t low = _weatherSnapshot.forecast[i].minC;
            if (low < minTemp) minTemp = low;
            if (high > maxTemp) maxTemp = high;
        }

        // 上下各留 2°C；小温差时至少保留 8°C 量程，避免曲线贴边。
        int16_t scaleMin = minTemp - 2;
        int16_t scaleMax = maxTemp + 2;
        if (scaleMax - scaleMin < 8) {
            const int16_t center = (scaleMax + scaleMin) / 2;
            scaleMin = center - 4;
            scaleMax = scaleMin + 8;
        }
        const int16_t range = scaleMax - scaleMin;

        int16_t pointX[WeatherClass::kForecastDays] = {};
        int16_t highY[WeatherClass::kForecastDays] = {};
        int16_t lowY[WeatherClass::kForecastDays] = {};
        for (uint8_t i = 0; i < count; i++) {
            // 与底部预报栏的日期中心严格对齐。
            pointX[i] = (int16_t)(((int32_t)(2 * i + 1) * kScrW) /
                                  (2 * count));
            highY[i] = chartBottom -
                (int16_t)(((int32_t)(_weatherSnapshot.forecast[i].maxC - scaleMin) *
                           (chartBottom - chartTop)) / range);
            lowY[i] = chartBottom -
                (int16_t)(((int32_t)(_weatherSnapshot.forecast[i].minC - scaleMin) *
                           (chartBottom - chartTop)) / range);
        }

        const uint16_t bandColor = blendRgb565(COLOR_BG, TREND_LOW, 48);
        drawSmoothRangeBand(tft, pointX, highY, lowY, count, bandColor,
                            chartTop, chartBottom);
        drawSmoothAaCurve(tft, pointX, highY, count, TREND_HIGH,
                          COLOR_BG, bandColor,
                          pointX[0], chartTop, pointX[count - 1], chartBottom);
        drawSmoothAaCurve(tft, pointX, lowY, count, TREND_LOW,
                          bandColor, COLOR_BG,
                          pointX[0], chartTop, pointX[count - 1], chartBottom);
    } else {
        const char *status = _weatherValid ? "预报等待中"
                                           : (_wifiOn ? "天气更新中"
                                                      : "连接 WiFi 查看");
        const int16_t statusW = measureCjkText(status, 1);
        drawCjkText((kScrW - statusW) / 2, 168, status, 1, COLOR_LABEL,
                    COLOR_BG);
    }

    // ---------- 底部五日预报：仅保留日期和高低温 ----------
    constexpr int16_t forecastLineY = 192;
    tft.drawFastHLine(12, forecastLineY, kScrW - 24, COLOR_LINE);

    if (count > 0) {
        static const char *const dayNames[] = {
            "日", "一", "二", "三", "四", "五", "六"
        };
        const uint8_t firstWeekday = weekdayFromDate(_clockY, _clockMo, _clockD);
        for (uint8_t i = 0; i < count; i++) {
            const int16_t cx = (int16_t)(((int32_t)(2 * i + 1) * kScrW) /
                                         (2 * count));
            const char *day = i == 0 ? "今" : dayNames[(firstWeekday + i) % 7];
            const int16_t dayW = measureCjkText(day, 1);
            drawCjkText(cx - dayW / 2, 199, day, 1,
                        i == 0 ? COLOR_ACCENT : COLOR_LABEL, COLOR_BG);

            char rangeText[12];
            if (_weatherSnapshot.forecast[i].maxC != -128 &&
                _weatherSnapshot.forecast[i].minC != -128) {
                snprintf(rangeText, sizeof(rangeText), "%d/%d",
                         (int)_weatherSnapshot.forecast[i].maxC,
                         (int)_weatherSnapshot.forecast[i].minC);
            } else {
                snprintf(rangeText, sizeof(rangeText), "--/--");
            }
            const int16_t rangeW = measureTextWidth(rangeText, 1);
            drawText(cx - rangeW / 2, 217, rangeText, 1,
                     COLOR_TEXT, COLOR_BG);
        }
    } else {
        const char *status = _weatherValid ? "暂无预报" : "等待天气数据";
        const int16_t statusW = measureCjkText(status, 1);
        drawCjkText((kScrW - statusW) / 2, 205, status, 1, COLOR_LABEL,
                    COLOR_BG);
    }
    drawHomePageIndicator();
}

void DisplayClass::showHomeView(HomeView view, bool force) {
    if (!force && _homeView == view) return;
    _homeView = view;
    _homeViewSinceMs = millis();
    _flipAnim = FlipAnim::Idle;
    _weatherIconCx = 0;

    fillScreenBg();
    drawStatusBar(true);
    if (_homeView == HomeView::Clock) {
        _lastFlipH = 0xFF;
        _lastFlipM = 0xFF;
        _lastFlipS = 0xFF;
        _lastWelcomeY = 0xFFFF;
        _lastWelcomeMo = 0xFF;
        _lastWelcomeD = 0xFF;
        drawFlipClock(true);
        drawHomePageIndicator();
    } else {
        _weatherDashboardDirty = true;
        drawWeatherDashboard(true);
    }
    Serial.printf("[DISP] home view %u/2\n", (unsigned)_homeView + 1U);
}

// ---------- 生命周期 ----------

void DisplayClass::init() {
    Serial.println("[DISP] init");
    // 中文字库在 SPIFFS 上，绘制任何中文之前先挂载。
    // 32px 字库供歌词大字用；缺失时 drawCjkText 自动退回 2x2 放大，
    // 只是糊一点，不影响功能（首次需 pio run -t uploadfs 烧录）。
    CjkFont.begin();
    CjkFont32.begin();
    Serial.printf("[DISP] SDA=%d SCL=%d CS=%d DC=%d RST=%d BL=%d\n",
                  PIN_TFT_MOSI, PIN_TFT_SCLK, PIN_TFT_CS,
                  PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_BL);

#if PIN_TFT_BL >= 0
    pinMode(PIN_TFT_BL, OUTPUT);
    _blOn = false;
    applyBlPin(false);
#endif

    pinMode(PIN_TFT_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);

    SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
    delay(20);

#if defined(DISP_DRIVER_ILI9341)
    tft.begin();
#else
    tft.init(TFT_PHYS_W, TFT_PHYS_H);
#endif
    // 26MHz：频谱动画的传输量较大，16MHz 会让 SPI 饱和拖卡主循环。
    // ST7789 写时钟规格可到 62MHz；杜邦线飞线取保守的 26MHz，
    // 若出现花屏/条纹再退回 16MHz。
    tft.setSPISpeed(26000000);
    tft.setRotation(1);
    tft.setTextWrap(false);

    memset(_barH, 0, sizeof(_barH));
    _lastVol = 0xFF;
    _lastTitle = "";
    _lastArtist = "";
    _lastFrame = 0;
    _wifiOn = false;
    _battPct = 82;
    _clockH = 12;
    _clockM = 30;
    _clockS = 0;
    unsigned buildHour = 12;
    unsigned buildMinute = 30;
    unsigned buildSecond = 0;
    if (sscanf(__TIME__, "%u:%u:%u", &buildHour, &buildMinute, &buildSecond) == 3 &&
        buildHour < 24 && buildMinute < 60 && buildSecond < 60) {
        _clockH = (uint8_t)buildHour;
        _clockM = (uint8_t)buildMinute;
        _clockS = (uint8_t)buildSecond;
    }
    char buildMonth[4] = {};
    unsigned buildDay = 1;
    unsigned buildYear = 2026;
    if (sscanf(__DATE__, "%3s %u %u", buildMonth, &buildDay, &buildYear) == 3) {
        const uint8_t month = monthFromName(buildMonth);
        if (month != 0) {
            _clockY = (uint16_t)buildYear;
            _clockMo = month;
            _clockD = (uint8_t)buildDay;
        }
    }
    _clockTick = millis();
    _page = Page::None;
    _inited = true;

    showWelcome(true);
    setBacklight(true);
    Serial.println("[DISP] status bar + flip clock HH:MM:SS");
}

void DisplayClass::showWelcome(bool force) {
    if (!force && _page == Page::Welcome) return;
    _page = Page::Welcome;
    _lastClockM = 0xFF;
    showHomeView(HomeView::Clock, true);
}

void DisplayClass::showPlayer(bool force) {
    if (!force && _page == Page::Player) return;
    _page = Page::Player;

    memset(_barH, 0, sizeof(_barH));
    _lastVol = 0xFF;
    _lastTitle = "";
    _lastArtist = "";
    _lastState = PlayerState::Stopped;
    _lastBtOn = false;
    _lastWifi = !_wifiOn;
    _lastBatt = 0xFF;
    _lastClockM = 0xFF;

    paintPlayerChrome();
    updatePlayer(true);
}

void DisplayClass::paintPlayerChrome() {
    fillScreenBg();
    drawStatusBar(true);
    tft.drawFastHLine(0, BAR_BASE + 1, kScrW, COLOR_DIM);
    tft.drawRect(60, 206, 200, 10, COLOR_DIM);
}

#if 0
void DisplayClass::updateWelcomeIdle() {
    // 动画进行中：高频推进动画帧（~50fps），不检查时钟变化
    if (_flipAnim != FlipAnim::Idle) {
        advanceFlipAnimation();
        return;
    }

    // 空闲状态：低频检查时钟（每 250ms 一次即可，秒级变化足够了）
    static uint32_t lastCheck = 0;
    uint32_t now = millis();
    if (now - lastCheck < 250) return;
    lastCheck = now;

    tickClock();
    drawStatusBar(false);
    // 秒变化时启动翻页动画
    drawFlipClock(false);
}

#endif

void DisplayClass::updateWelcomeIdle() {
    tickClock();
    const uint32_t now = millis();
    if (now - _homeViewSinceMs >= kHomeRotateMs) {
        const HomeView next = _homeView == HomeView::Clock
            ? HomeView::Weather : HomeView::Clock;
        showHomeView(next, true);
        return;
    }

    drawStatusBar(false);
    if (_homeView == HomeView::Weather) {
        drawWeatherDashboard(false);
        return;
    }

    drawWelcomeWeather(false);
    // 仅背光亮着才跑图标动画；翻页动画帧优先，避免同帧抢 SPI
    if (_blOn && _flipAnim == FlipAnim::Idle) updateWeatherAnimation();
    if (_flipAnim != FlipAnim::Idle) {
        advanceFlipAnimation();
    } else {
        drawFlipClock(false);
    }
}

void DisplayClass::updatePlayer(bool force) {
    drawStatusBar(force);
    updateTrackInfo(force);
    updateSpectrum(force);
    updatePlayIcon(force);
    updateVolume(force);
}

void DisplayClass::render() {
    if (!_inited) return;

    // Keep the software clock advancing even while the welcome-page backlight
    // is off. The display may sleep, but time must not stop with it.
    tickClock();

    // Low-rate heartbeat: confirms that the main loop reaches render even
    // when the display is asleep or a page-specific branch returns early.
    static uint32_t lastClockHeartbeat = 0;
    const uint32_t heartbeatNow = millis();
    if (heartbeatNow - lastClockHeartbeat >= 5000U) {
        lastClockHeartbeat = heartbeatNow;
        Serial.printf("[CLOCK][%lu] heartbeat page=%u bl=%u time=%02u:%02u:%02u flip=%u lastFlip=%02u:%02u:%02u\n",
                      (unsigned long)heartbeatNow,
                      (unsigned)_page,
                      _blOn ? 1U : 0U,
                      (unsigned)_clockH, (unsigned)_clockM, (unsigned)_clockS,
                      (unsigned)_flipAnim,
                      (unsigned)_lastFlipH, (unsigned)_lastFlipM, (unsigned)_lastFlipS);
    }

    const bool connected = BluetoothA2DP.isConnected();

    if (_page == Page::Network) {
        if (!_blOn) return;
        drawStatusBar(false);
        updateNetworkSettings(false);
        return;
    }

    if (_page == Page::Music) {
        if (!_blOn) return;
        updateMusicPlayer(false);
        return;
    }

    if (_page == Page::Books) {
        if (!_blOn) return;
        drawStatusBar(false);
        updateBooks(false);
        return;
    }

    if (_page == Page::WifiList) {
        if (!_blOn) return;
        drawStatusBar(false);
        updateWifiList(false);
        return;
    }

    if (_page == Page::Menu || _page == Page::Settings ||
        _page == Page::Version || _page == Page::NeteaseCloud) {
        if (!_blOn) return;
        drawStatusBar(false);
        if (_page == Page::Menu) updateMenu(false);
        else if (_page == Page::Settings) updateSettingsPage(false);
        return;
    }

    if (_page == Page::Voice) {
        if (!_blOn) return;
        updateVoicePage(false);
        return;
    }

    if (connected && _page != Page::Player) {
        noteActivity();
        showPlayer(true);
        setBacklight(true);
        return;
    }
    if (!connected && _page != Page::Welcome) {
        showWelcome(true);
        setBacklight(true);
        noteActivity();
        return;
    }

    serviceBacklight();
    if (!_blOn) return;

    if (_page == Page::Welcome) {
        updateWelcomeIdle();
        return;
    }

    uint32_t now = millis();
    if (now - _lastFrame < 100) return;
    _lastFrame = now;
    updatePlayer(false);
}

void DisplayClass::updateTrackInfo(bool force) {
    const auto &track = BluetoothA2DP.getTrack();
    String title = track.title.length() ? track.title : String("Unknown title");
    String artist = track.artist.length() ? track.artist : String("Unknown artist");

    if (!force && title == _lastTitle && artist == _lastArtist) return;
    _lastTitle = title;
    _lastArtist = artist;

    tft.fillRect(0, STATUS_H + 2, kScrW, 30, COLOR_BG);
    drawText(4, STATUS_H + 6, title.c_str(), 2, COLOR_TEXT, COLOR_BG);
    drawText(4, STATUS_H + 24, artist.c_str(), 1, COLOR_DIM, COLOR_BG);
}

void DisplayClass::updateSpectrum(bool force) {
    const auto &levels = Spectrum.getLevels();

    for (uint8_t i = 0; i < BARS; i++) {
        uint8_t v = (i < levels.size()) ? levels[i] : 0;
        if (v < 8) v = 8;

        uint16_t h = (uint16_t)v * BAR_MAX_H / 255;
        if (h < 2) h = 2;
        if (h > BAR_MAX_H) h = BAR_MAX_H;
        h = (h / 4) * 4;

        uint8_t oldH = _barH[i];
        if (!force && h == oldH) continue;

        const uint16_t x = kBarX0 + i * (BAR_W + BAR_GAP);
        const uint16_t color = (v > 180) ? COLOR_ALERT
                             : (v > 90 ? 0xFEA1 : COLOR_ACCENT);  // 红/黄/蓝三档

        if (oldH > 0) tft.fillRect(x, BAR_BASE - oldH, BAR_W, oldH, COLOR_BG);
        if (h > 0) tft.fillRect(x, BAR_BASE - h, BAR_W, h, color);
        _barH[i] = (uint8_t)h;
    }
}

void DisplayClass::updatePlayIcon(bool force) {
    auto st = BluetoothA2DP.getState();
    if (!force && st == _lastState) return;
    _lastState = st;

    tft.fillRect(0, 176, 56, 28, COLOR_BG);
    const char *icon = "o";
    if (st == PlayerState::Playing) icon = ">";
    else if (st == PlayerState::Paused) icon = "||";
    drawText(8, 182, icon, 3, COLOR_TEXT, COLOR_BG);
}

void DisplayClass::updateVolume(bool force) {
    uint8_t vol = BluetoothA2DP.getVolume();
    if (!force && vol == _lastVol) return;
    _lastVol = vol;

    tft.fillRect(56, 176, 120, 28, COLOR_BG);
    char buf[16];
    snprintf(buf, sizeof(buf), "Vol %u%%", vol);
    drawText(60, 182, buf, 2, COLOR_TEXT, COLOR_BG);

    tft.fillRect(61, 207, 198, 8, COLOR_BG);
    uint16_t w = (uint16_t)vol * 198 / 100;
    if (w > 0) tft.fillRect(61, 207, w, 8, COLOR_ACCENT);
}
