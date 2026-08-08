#include "Display.h"
#include "Spectrum.h"
#include "WifiProvisioning.h"
#include "pins_display.h"
#include "assets/music_icon.h"
#include "assets/settings_icon.h"

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
static const char *const MENU_APP_NAMES[] = {"Music", "Settings"};
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

static void drawProvisioningQr(esp_qrcode_handle_t qr) {
    const int size = esp_qrcode_get_size(qr);
    if (size <= 0) return;
    constexpr int16_t qrBoxX = 18;
    constexpr int16_t qrBoxY = 74;
    constexpr int16_t qrBox = 122;
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

static const GFXfont *uiFontForSize(uint8_t size) {
    switch (size) {
        case 1: return &FreeSans9pt7b;
        case 2: return &FreeSansBold12pt7b;
        case 3: return &FreeSansBold18pt7b;
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
    if (_blOn == on && _inited) {
        if (on) _activityMs = millis();
        return;
    }
    _blOn = on;
    applyBlPin(on);
    if (on) _activityMs = millis();
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

void DisplayClass::settingsMove(int8_t delta) {
    if (_page != Page::Settings) return;
    int16_t next = (int16_t)_settingsSelection + delta;
    if (next < 0) next = 1;
    if (next > 1) next = 0;
    _settingsSelection = (uint8_t)next;
    noteActivity();
}

void DisplayClass::settingsActivate() {
    if (_page != Page::Settings) return;
    noteActivity();
    if (_settingsSelection == 0) showNetworkSettings(true);
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
            if (next == 0) showNeteaseCloud(true);
            else if (next == 1) showSettingsPage(true);
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

// ---------- 状态栏图标 16×16 点阵（每行 2 字节，MSB 在左）----------

// WiFi：三层信号弧 + 圆点
static const uint8_t WIFI_ICON[32] PROGMEM = {
    0x00, 0x00,
    0x07, 0xE0, //    ######
    0x1F, 0xF8, //  ##########
    0x38, 0x1C, // ###      ###
    0x70, 0x0E, //###        ###
    0x00, 0x00,
    0x03, 0xC0, //     ####
    0x0F, 0xF0, //    ######
    0x1C, 0x38, //   ###  ###
    0x18, 0x18, //   ##    ##
    0x00, 0x00,
    0x01, 0x80, //      ##
    0x03, 0xC0, //     ####
    0x01, 0x80, //      ##
    0x00, 0x00,
    0x00, 0x00,
};

// 蓝牙：标准 runic「Β」折线
static const uint8_t BT_ICON[32] PROGMEM = {
    0x08, 0x00,
    0x0C, 0x00,
    0x0A, 0x00,
    0x09, 0x00,
    0x48, 0x80,
    0x29, 0x00,
    0x1A, 0x00,
    0x0C, 0x00,
    0x1A, 0x00,
    0x29, 0x00,
    0x48, 0x80,
    0x09, 0x00,
    0x0A, 0x00,
    0x0C, 0x00,
    0x08, 0x00,
    0x00, 0x00,
};

void DisplayClass::drawWifiIcon(int16_t x, int16_t y, bool on) {
    const uint16_t c = on ? COLOR_TEXT : COLOR_DIM;
    tft.drawBitmap(x, y, WIFI_ICON, 16, 16, c, COLOR_BAR);
    if (!on) {
        // 未连接：双线红斜杠
        tft.drawLine(x + 1, y + 14, x + 14, y + 1, 0xF800);
        tft.drawLine(x + 2, y + 14, x + 15, y + 1, 0xF800);
    }
}

void DisplayClass::drawBtIconAt(int16_t x, int16_t y, uint16_t color, uint16_t bg) {
    tft.drawBitmap(x, y, BT_ICON, 16, 16, color, bg);
}

void DisplayClass::drawBtIcon(int16_t x, int16_t y, bool on) {
    const uint16_t c = on ? COLOR_ACCENT : COLOR_DIM;
    drawBtIconAt(x, y, c, COLOR_BAR);
    if (!on) {
        tft.drawLine(x + 1, y + 14, x + 14, y + 1, 0xF800);
        tft.drawLine(x + 2, y + 14, x + 15, y + 1, 0xF800);
    }
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
    if (!force && _temperatureC == _lastWelcomeTemperatureC) return;
    _lastWelcomeTemperatureC = _temperatureC;

    tft.fillRect(0, STATUS_H + 1, kScrW, kFlipClockY - STATUS_H - 5, COLOR_BG);

    char value[6];
    if (_temperatureC == -128) snprintf(value, sizeof(value), "--");
    else snprintf(value, sizeof(value), "%d", _temperatureC);

    const int16_t valueW = measureTextWidth(value, 3);
    const int16_t unitW = measureTextWidth("C", 2);
    constexpr int16_t degreeW = 10;
    constexpr int16_t gap = 4;
    const int16_t totalW = valueW + gap + degreeW + unitW;
    const int16_t x = (kScrW - totalW) / 2;

    drawText(x, kWelcomeWeatherY, value, 3, COLOR_TEXT, COLOR_BG);
    const int16_t degreeX = x + valueW + gap + 3;
    tft.drawCircle(degreeX, kWelcomeWeatherY + 4, 3, COLOR_ACCENT);
    drawText(degreeX + 7, kWelcomeWeatherY + 10, "C", 2, COLOR_ACCENT, COLOR_BG);
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

    char buf[20];
    snprintf(buf, sizeof(buf), "%04u / %02u / %02u",
             _clockY, _clockMo, _clockD);
    const int16_t tw = measureTextWidth(buf, 1);
    tft.fillRect(0, y, kScrW, 14, COLOR_BG);
    drawText((kScrW - tw) / 2, y, buf, 1, COLOR_DIM, COLOR_BG);
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

    char dig[7];
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
    constexpr int16_t BW = 27;
    constexpr int16_t BH = 16;
    constexpr int16_t CAP_W = 3;
    constexpr int16_t CAP_H = 8;
    if (pct > 100) pct = 100;

    const uint16_t fillC = pct <= 15 ? 0xF800 :
                           (pct <= 30 ? 0xFE60 :
                           (pct <= 50 ? 0xFFE0 : 0x07E0));
    tft.fillRoundRect(x, y, BW, BH, 3, 0x10A2);
    const int16_t innerW = BW - 4;
    const int16_t innerH = BH - 4;
    int16_t fillW = (int16_t)((innerW * (int16_t)pct) / 100);
    if (pct > 0 && fillW < 2) fillW = 2;
    if (fillW > 0) tft.fillRoundRect(x + 2, y + 2, fillW, innerH, 2, fillC);
    tft.drawRoundRect(x, y, BW, BH, 3, COLOR_TEXT);
    tft.fillRect(x + BW, y + (BH - CAP_H) / 2, CAP_W, CAP_H, COLOR_TEXT);

    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", pct);
    drawText(x + BW + CAP_W + 3, y + 1, buf, 1,
             pct <= 15 ? 0xF800 : COLOR_TEXT, COLOR_BAR);
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

    // 中：状态文案
    const char *mid;
    if (_page == Page::Network) mid = "Network Setup";
    else if (_page == Page::Menu) mid = "Apps";
    else if (_page == Page::Settings) mid = "Settings";
    else if (_page == Page::Version) mid = "Version";
    else if (_page == Page::NeteaseCloud) mid = "Music";
    else if (_page == Page::Welcome) mid = "";
    else if (wifi && bt) mid = "WiFi + BT";
    else if (wifi) mid = "WiFi Connected";
    else if (bt) mid = "BT Connected";
    else mid = "Standby";
    if (mid[0]) {
        const int16_t tw = measureTextWidth(mid, 1);
        drawText((kScrW - tw) / 2, 10, mid, 1, COLOR_TEXT, COLOR_BAR);
    }

    // 右：WiFi | BT | 扁平电池（百分比在内部）
    // 16 + 4 + 16 + 6 + 39 ≈ 81
    const int16_t rx = kScrW - 94;
    drawWifiIcon(rx, 6, wifi);
    drawBtIcon(rx + 20, 6, bt);
    drawBatteryIcon(rx + 42, 6, batt);
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
    constexpr int16_t rowY = 88;
    constexpr int16_t rowW = 288;
    constexpr int16_t rowH = 54;
    const int16_t y = rowY + option * rowH;
    (void)selected;
    const uint16_t ink = COLOR_TEXT;

    tft.fillRect(rowX, y, rowW, rowH, COLOR_BG);
    tft.drawFastHLine(rowX + 10, y + rowH - 1,
                      rowW - 20, COLOR_LINE);

    if (option == 0) {
        drawText(rowX + 18, y + 18, "WIFI", 1, ink, COLOR_BG);
        drawCjkGlyph(rowX + 70, y + 18, CjkGlyph::She, ink);
        drawCjkGlyph(rowX + 87, y + 18, CjkGlyph::Zhi, ink);
    } else {
        drawCjkGlyph(rowX + 18, y + 18, CjkGlyph::Xi, ink);
        drawCjkGlyph(rowX + 35, y + 18, CjkGlyph::Tong, ink);
        drawCjkGlyph(rowX + 52, y + 18, CjkGlyph::Ban, ink);
        drawCjkGlyph(rowX + 69, y + 18, CjkGlyph::Ben, ink);
    }

}

void DisplayClass::drawSettingsSelectionMarker(uint8_t option, bool selected) {
    constexpr int16_t rowX = 16;
    constexpr int16_t rowY = 88;
    constexpr int16_t rowW = 288;
    constexpr int16_t rowH = 54;
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
    if (!selectionChanged && !qrChanged && !_networkServiceDirty) return;

    if (qrChanged) {
        tft.fillRect(0, STATUS_H + 1, kScrW, 240 - STATUS_H - 1, COLOR_BG);
        drawText(16, 44, "WIFI", 1, COLOR_TEXT, COLOR_BG);
        drawCjkGlyph(66, 44, CjkGlyph::She, COLOR_TEXT);
        drawCjkGlyph(84, 44, CjkGlyph::Zhi, COLOR_TEXT);
        drawText(16, 66, "SCAN TO CONFIGURE", 1,
                 COLOR_DIM, COLOR_BG);

        if (_networkQrPayload.length()) {
            tft.fillRoundRect(14, 70, 130, 130, 6, COLOR_PANEL);
            tft.drawRoundRect(14, 70, 130, 130, 6, COLOR_ACCENT);
            esp_qrcode_config_t qrConfig = ESP_QRCODE_CONFIG_DEFAULT();
            qrConfig.display_func = drawProvisioningQr;
            qrConfig.max_qrcode_version = 5;
            qrConfig.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
            const esp_err_t qrResult = esp_qrcode_generate(
                &qrConfig, _networkQrPayload.c_str());
            if (qrResult != ESP_OK) {
                tft.fillRect(18, 74, 122, 122, COLOR_PANEL);
                drawText(35, 119, "QR ERROR", 1, COLOR_ACCENT, COLOR_PANEL);
            }
        } else {
            tft.fillRoundRect(14, 70, 130, 130, 6, COLOR_PANEL);
            tft.drawRoundRect(14, 70, 130, 130, 6, COLOR_LINE);
            tft.fillRect(18, 74, 122, 122, COLOR_PANEL);
            drawText(38, 119, "STARTING", 1, COLOR_DIM, COLOR_PANEL);
        }

        const char *ssid = _networkServiceSsid.length()
            ? _networkServiceSsid.c_str() : "MY-SMALL-BOX";
        tft.drawFastVLine(154, 76, 120, COLOR_LINE);
        drawText(166, 77, "HOTSPOT", 1, COLOR_DIM, COLOR_BG);
        drawText(166, 98, ssid, 1, COLOR_TEXT, COLOR_BG);
        drawText(166, 123, "PASSWORD", 1, COLOR_DIM, COLOR_BG);
        drawText(166, 144, WifiProvisioningClass::PROVISIONING_PASSWORD,
                 1, COLOR_TEXT, COLOR_BG);
        drawText(166, 169, "OPEN BROWSER", 1, COLOR_DIM, COLOR_BG);
        drawText(166, 181,
                 _networkServiceIp.length() ? _networkServiceIp.c_str()
                                            : "192.168.4.1",
                 1, COLOR_ACCENT, COLOR_BG);
        _lastNetworkQrPayload = _networkQrPayload;
    }

    if (selectionChanged || qrChanged) {
        constexpr int16_t buttonY = 199;
        constexpr int16_t buttonH = 24;
        tft.fillRect(16, buttonY, 288, buttonH, COLOR_BG);
        const bool rescanSelected = _networkSelection == 0;
        const bool backSelected = _networkSelection == 1;
        tft.drawRoundRect(16, buttonY, 130, buttonH, 5,
                          rescanSelected ? COLOR_ACCENT : COLOR_LINE);
        tft.drawRoundRect(174, buttonY, 130, buttonH, 5,
                          backSelected ? COLOR_ACCENT : COLOR_LINE);
        drawText(48, buttonY + 6, "SCAN WIFI", 1,
                 rescanSelected ? COLOR_TEXT : COLOR_DIM, COLOR_BG);
        const uint16_t backColor = backSelected ? COLOR_TEXT : COLOR_DIM;
        drawCjkGlyph(222, buttonY + 4, CjkGlyph::Fan, backColor);
        drawCjkGlyph(240, buttonY + 4, CjkGlyph::Hui, backColor);
    }

    if (_networkServiceDirty || qrChanged) {
        tft.fillRect(16, 224, 288, 16, COLOR_BG);
        drawText(16, 224,
                 _networkServiceStatus.length() ? _networkServiceStatus.c_str()
                                                : "Starting setup hotspot",
                 1, _networkServiceActive ? COLOR_ACCENT : COLOR_DIM, COLOR_BG);
    }

    _lastNetworkSelection = _networkSelection;
    _lastNetworkMessage = _networkMessage;
    _networkServiceDirty = false;
}

void DisplayClass::drawMusicIcon(int16_t x, int16_t y, int16_t size, bool selected) {
    if (size == MUSIC_ICON_W) {
        tft.drawRGBBitmap(x, y, MUSIC_ICON_RGB565, MUSIC_ICON_MASK,
                          MUSIC_ICON_W, MUSIC_ICON_H);
        return;
    }

    const uint16_t accent = selected ? 0xFD20 : COLOR_ACCENT;
    const uint16_t mark = COLOR_TEXT;
    const int16_t cx = x + size / 2;
    const int16_t cy = y + size / 2;

    tft.fillRoundRect(x, y, size, size, 10, COLOR_CARD);
    tft.drawRoundRect(x, y, size, size, 10, selected ? COLOR_ACCENT : COLOR_LINE);
    tft.fillCircle(cx, cy, size * 31 / 100, 0x18E3);
    tft.drawCircle(cx, cy, size * 31 / 100, accent);
    tft.fillCircle(cx, cy, size * 8 / 100, COLOR_BG);

    const int16_t noteY = y + size * 67 / 100;
    const int16_t noteR = size / 10;
    const int16_t stemX = x + size * 59 / 100;
    tft.fillCircle(x + size * 38 / 100, noteY, noteR, mark);
    tft.fillCircle(x + size * 63 / 100, noteY - size / 9, noteR, mark);
    tft.fillRect(x + size * 38 / 100 + noteR - 1, y + size * 34 / 100,
                 3, noteY - (y + size * 34 / 100), mark);
    tft.fillRect(stemX, y + size * 25 / 100, 3,
                 noteY - size / 9 - (y + size * 25 / 100), mark);
    tft.drawFastHLine(x + size * 38 / 100 + noteR, y + size * 34 / 100,
                      size * 28 / 100, mark);
}

void DisplayClass::drawSettingsIcon(int16_t x, int16_t y, int16_t size, bool selected) {
    if (size == SETTINGS_ICON_W) {
        tft.drawRGBBitmap(x, y, SETTINGS_ICON_RGB565, SETTINGS_ICON_MASK,
                          SETTINGS_ICON_W, SETTINGS_ICON_H);
        return;
    }

    const uint16_t accent = selected ? COLOR_ACCENT : 0x7DFF;
    const int16_t cx = x + size / 2;
    const int16_t cy = y + size / 2;
    const int16_t outer = size * 27 / 100;
    const int16_t inner = size * 10 / 100;

    tft.fillRoundRect(x, y, size, size, 10, COLOR_CARD);
    tft.drawRoundRect(x, y, size, size, 10, selected ? COLOR_ACCENT : COLOR_LINE);
    tft.drawCircle(cx, cy, outer, accent);
    tft.fillCircle(cx, cy, inner, COLOR_CARD);
    tft.drawCircle(cx, cy, inner, accent);

    for (uint8_t i = 0; i < 8; i++) {
        const bool diagonal = (i & 1) != 0;
        const int16_t sx = (i == 1 || i == 2 || i == 3) ? 1 :
                           (i == 5 || i == 6 || i == 7) ? -1 : 0;
        const int16_t sy = (i == 3 || i == 4 || i == 5) ? 1 :
                           (i == 0 || i == 1 || i == 7) ? -1 : 0;
        tft.drawLine(cx + sx * (outer - 1), cy + sy * (outer - 1),
                     cx + sx * (outer + (diagonal ? 5 : 4)),
                     cy + sy * (outer + (diagonal ? 5 : 4)), accent);
    }
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
    } else {
        drawSettingsIcon(x + (MENU_CELL_W - MENU_ICON_SIZE) / 2, y + 8,
                         MENU_ICON_SIZE, false);
    }
    const int16_t labelW = measureTextWidth(MENU_APP_NAMES[appIndex], 1);
    drawText(x + (MENU_CELL_W - labelW) / 2, y + 68,
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

// ---------- 生命周期 ----------

void DisplayClass::init() {
    Serial.println("[DISP] init");
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
    tft.setSPISpeed(16000000);
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

    fillScreenBg();
    drawStatusBar(true);

    // 中部：时分秒翻页时钟（无底部波形）
    _lastFlipH = 0xFF;
    _lastFlipM = 0xFF;
    _lastFlipS = 0xFF;
    _lastWelcomeY = 0xFFFF;
    _lastWelcomeMo = 0xFF;
    _lastWelcomeD = 0xFF;
    _flipAnim = FlipAnim::Idle;
    drawFlipClock(true);
    _lastClockM = 0xFF;
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
    drawStatusBar(false);
    drawWelcomeWeather(false);
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

    if (_page == Page::Menu || _page == Page::Settings ||
        _page == Page::Version || _page == Page::NeteaseCloud) {
        if (!_blOn) return;
        drawStatusBar(false);
        if (_page == Page::Menu) updateMenu(false);
        else if (_page == Page::Settings) updateSettingsPage(false);
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
        const uint16_t color = (v > 180) ? C_RED : (v > 90 ? C_YELLOW : COLOR_ACCENT);

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
