#pragma once
#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include "BluetoothA2DP.h"

class DisplayClass {
public:
    enum class JoystickEvent : uint8_t { Up, Down, Left, Right, Press };

    void init();
    void render();

    void setBacklight(bool on);
    void backlightOn()  { setBacklight(true); }
    void backlightOff() { setBacklight(false); }
    bool isBacklightOn() const { return _blOn; }
    void noteActivity();

    // 状态栏数据（后续接 WiFi / ADC 电量）
    void setWifiConnected(bool on);
    void setBatteryPercent(uint8_t pct);  // 0–100
    void setNetworkServiceState(bool active, const char *status,
                                const char *ip, const char *apSsid,
                                const char *qrPayload);
    void setTemperatureC(int8_t celsius);
    void setClock(uint8_t hour, uint8_t minute, uint8_t second = 0);
    void setDate(uint16_t year, uint8_t month, uint8_t day);

    void enterNetworkSettings();
    void exitNetworkSettings();
    bool isNetworkSettings() const { return _page == Page::Network; }
    void networkMove(int8_t delta);
    void networkActivate();
    bool takeNetworkRescanRequest();
    void handleJoystick(JoystickEvent event);

private:
    enum class Page : uint8_t {
        None, Welcome, Player, Menu, Settings, Version, Network, NeteaseCloud
    };

    void showWelcome(bool force);
    void showPlayer(bool force);
    void showSettingsPage(bool force);
    void showVersionPage(bool force);
    void showNetworkSettings(bool force);
    void showMenu(bool force);
    void showNeteaseCloud(bool force);
    void updateWelcomeIdle();
    void updatePlayer(bool force);
    void updateSettingsPage(bool force);
    void updateNetworkSettings(bool force);
    void updateMenu(bool force);
    void serviceBacklight();

    void settingsMove(int8_t delta);
    void settingsActivate();
    void drawSettingsOption(uint8_t option, bool selected);
    void drawSettingsSelectionMarker(uint8_t option, bool selected);
    enum class CjkGlyph : uint8_t {
        Fan, Hui, She, Zhi, Xi, Tong, Ban, Ben
    };
    void drawCjkGlyph(int16_t x, int16_t y, CjkGlyph glyph, uint16_t color);

    void paintPlayerChrome();
    void drawStatusBar(bool force);
    void drawWifiIcon(int16_t x, int16_t y, bool on);
    void drawBtIcon(int16_t x, int16_t y, bool on);
    void drawBtIconAt(int16_t x, int16_t y, uint16_t color, uint16_t bg);
    void drawBatteryIcon(int16_t x, int16_t y, uint8_t pct);
    void drawMusicIcon(int16_t x, int16_t y, int16_t size, bool selected);
    void drawSettingsIcon(int16_t x, int16_t y, int16_t size, bool selected);
    void drawMenuApp(uint8_t appIndex, bool selected, uint8_t selectionInset = 0);
    void drawMenuSelectionFrame(int16_t x, int16_t y, uint16_t color, bool highlight);
    void drawMenuSelectionMotion(int16_t x, int16_t y, uint16_t color,
                                 bool highlight, bool horizontal);
    void drawFlipClock(bool force);
    void drawFlipCard(int16_t x, int16_t y, int16_t w, int16_t h, char digit);
    void drawFlipCardHalf(int16_t x, int16_t y, int16_t w, int16_t h, char digit, bool lowerHalf);
    void drawFlipDigitClip(int16_t x, int16_t y, int16_t w, int16_t h,
                           char digit, int16_t clipY, int16_t clipH);
    void drawFlipColon(int16_t x, int16_t y, int16_t h);
    void drawFlipUnitLabel(int16_t cx, int16_t y, const char *label);
    void drawWelcomeWeather(bool force);
    void drawWelcomeDate(int16_t y, bool force);
    void advanceFlipAnimation();
    void updateTrackInfo(bool force);
    void updateSpectrum(bool force);
    void updatePlayIcon(bool force);
    void updateVolume(bool force);

    void drawText(int16_t x, int16_t y, const char *s, uint8_t size, uint16_t fg, uint16_t bg);
    int16_t measureTextWidth(const char *s, uint8_t size);
    void fillScreenBg();
    void applyBlPin(bool on);
    void tickClock();

    Page     _page       = Page::None;
    uint32_t _lastFrame  = 0;
    uint32_t _activityMs = 0;
    uint32_t _clockTick  = 0;
    bool     _inited     = false;
    bool     _blOn       = false;

    // 状态栏缓存
    bool     _wifiOn     = false;
    bool     _lastWifi   = false;
    bool     _lastBtOn   = false;
    uint8_t  _battPct    = 82;
    int8_t   _temperatureC = -128;
    uint8_t  _lastBatt   = 0xFF;
    uint8_t  _clockH     = 12;
    uint8_t  _clockM     = 30;
    uint8_t  _clockS     = 0;
    uint16_t _clockY     = 2026;
    uint8_t  _clockMo    = 1;
    uint8_t  _clockD     = 1;
    uint8_t  _lastClockH = 0xFF;
    uint8_t  _lastClockM = 0xFF;
    uint16_t _lastClockY  = 0xFFFF;
    uint8_t  _lastClockMo = 0xFF;
    uint8_t  _lastClockD  = 0xFF;
    int8_t   _lastWelcomeTemperatureC = -127;
    uint8_t  _lastFlipH  = 0xFF;
    uint8_t  _lastFlipM  = 0xFF;
    uint8_t  _lastFlipS  = 0xFF;
    uint16_t _lastWelcomeY  = 0xFFFF;
    uint8_t  _lastWelcomeMo = 0xFF;
    uint8_t  _lastWelcomeD  = 0xFF;

    // 翻页动画状态
    enum class FlipAnim : uint8_t { Idle, SyncReveal };
    FlipAnim  _flipAnim   = FlipAnim::Idle;
    uint32_t  _flipEnterMs = 0;
    uint8_t   _flipOldDig[6] = {0};
    uint8_t   _flipNewDig[6] = {0};
    bool      _flipChanged[6] = {false};
    uint8_t   _flipLastReveal = 0xFF;

    uint8_t  _networkSelection = 0;
    const char *_networkMessage = "WiFi service not configured";
    uint8_t  _lastNetworkSelection = 0xFF;
    const char *_lastNetworkMessage = nullptr;
    bool     _networkServiceActive = false;
    bool     _networkServiceDirty = true;
    bool     _networkRescanRequested = false;
    String   _networkServiceStatus;
    String   _networkServiceIp;
    String   _networkServiceSsid;
    String   _networkQrPayload;
    String   _lastNetworkQrPayload;

    uint8_t  _settingsSelection = 0;
    uint8_t  _lastSettingsSelection = 0xFF;

    uint8_t  _menuSelection = 0;
    uint8_t  _menuRowOffset = 0;
    uint8_t  _lastMenuSelection = 0xFF;
    uint8_t  _lastMenuRowOffset = 0xFF;
    bool     _menuSelectionAnimating = false;
    uint32_t _menuSelectionAnimStart = 0;
    uint8_t  _menuAnimationFrom = 0xFF;
    uint8_t  _menuAnimationTo = 0xFF;
    int16_t  _menuFrameX = 0;
    int16_t  _menuFrameY = 0;
    int16_t  _menuAnimationStartX = 0;
    int16_t  _menuAnimationStartY = 0;
    bool     _menuAnimationHorizontal = true;

    PlayerState _lastState = PlayerState::Stopped;
    uint8_t  _lastVol    = 0xFF;
    String   _lastTitle;
    String   _lastArtist;
    uint8_t  _barH[16]   = {};

    static constexpr uint16_t STATUS_H   = 28;

    // 翻页时钟布局常量
    static constexpr int16_t kFlipCardW     = 44;
    static constexpr int16_t kFlipCardH     = 68;
    static constexpr int16_t kFlipGapIn     = 4;    // 同组两卡间距
    static constexpr int16_t kFlipColonW    = 14;   // 冒号占位宽
    static constexpr int16_t kFlipPairW     = 2 * kFlipCardW + kFlipGapIn;
    static constexpr int16_t kFlipTotalW    = 3 * kFlipPairW + 2 * kFlipColonW;
    static constexpr int16_t kFlipLabelH    = 14;
    static constexpr int16_t kFlipBlockH    = kFlipCardH;
    static constexpr int16_t kWelcomeWeatherY = 47;
    static constexpr int16_t kFlipClockY      = 118;

    // 翻页动画参数
    // Upper and lower halves reveal together from the center seam.
    static constexpr uint32_t kFlipAnimationMs = 280;

    int16_t flipCardX(int idx, int16_t x0) const;

    static constexpr uint16_t COLOR_BG     = 0x0000;
    static constexpr uint16_t COLOR_BAR    = 0x18E3;  // 深灰状态栏底
    static constexpr uint16_t COLOR_TEXT   = 0xFFFF;
    static constexpr uint16_t COLOR_ACCENT = 0x07FF;
    static constexpr uint16_t COLOR_DIM    = 0x8410;
    static constexpr uint16_t COLOR_LINE   = 0x4208;
    // RGB565：翻页卡底（相对纯黑背景要足够亮，正面才分得清）
    // #2A3A52 ≈ R5=5 G6=14 B5=10 → 0x29CA
    static constexpr uint16_t COLOR_CARD        = 0x2188;  // 下半片深蓝灰
    static constexpr uint16_t COLOR_CARD_TOP    = 0x2A0A;  // 上半片略亮
    static constexpr uint16_t COLOR_CARD_EDGE   = 0x42AD;  // 卡片外缘
    static constexpr uint16_t COLOR_FLIP_LINE   = 0xB5B6;  // 中缝高光
    static constexpr uint16_t COLOR_FLIP_SHADOW = 0x1082;  // 中缝阴影
    static constexpr uint16_t COLOR_PANEL       = 0x10A2;  // 设置页面板
};

extern DisplayClass Display;

#endif // DISPLAY_H
