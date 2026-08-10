#pragma once
#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include "BluetoothA2DP.h"
#include "Weather.h"

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
    // 首页天气快照由网络任务缓存后一次性推送，显示线程不发网络请求。
    void setWeather(const WeatherClass::Snapshot &weather);
    void setClock(uint8_t hour, uint8_t minute, uint8_t second = 0);
    void setDate(uint16_t year, uint8_t month, uint8_t day);

    void enterNetworkSettings();
    void exitNetworkSettings();
    bool isNetworkSettings() const { return _page == Page::Network; }
    void networkMove(int8_t delta);
    void networkActivate();
    bool takeNetworkRescanRequest();
    // 听书列表请求事件（供 main 轮询）
    bool takeBookListRequest();
    void handleJoystick(JoystickEvent event);

    // ---- 音乐播放器（当前为 UI 骨架，数据由外部注入）----
    struct LyricLine {
        uint32_t startMs = 0;   // 该句起始时间
        String   text;          // 歌词文本（UTF-8）
    };
    // 曲目信息；coverRgb565 为 nullptr 时画黑胶唱片占位图
    void setNowPlaying(const char *title, const char *artist, const char *album,
                       uint32_t durationMs,
                       const uint16_t *coverRgb565 = nullptr,
                       int16_t coverW = 0, int16_t coverH = 0);
    // 切歌过渡：立即清空旧曲目信息，显示加载状态
    void setMusicLoading();
    void setPlaybackPosition(uint32_t positionMs);
    void setLyrics(const LyricLine *lines, uint16_t count);
    void showMusicPlayer(bool force);
    bool isMusicPlayer() const { return _page == Page::Music; }

private:
    enum class Page : uint8_t {
        None, Welcome, Player, Menu, Settings, Version, Network, NeteaseCloud,
        Music, WifiList, Books
    };
    enum class HomeView : uint8_t { Clock = 0, Weather = 1 };
    // 音乐播放器三个子页，摇杆左右切换
    enum class MusicView : uint8_t { NowPlaying = 0, Lyrics = 1, Spectrum = 2 };

    void showWelcome(bool force);
    void showHomeView(HomeView view, bool force);
    void drawHomePageIndicator();
    void drawWeatherDashboard(bool force);
    void showPlayer(bool force);
    void showSettingsPage(bool force);
    void showVersionPage(bool force);
    void showNetworkSettings(bool force);
    void showMenu(bool force);
    void showNeteaseCloud(bool force);
    // ---- 听书列表页 ----
    void showBooks(bool force);
    void updateBooks(bool force);
    void booksMove(int8_t delta);
    void booksActivate();      // 确定：暂无正文接口，暂显示简介
    // ---- WiFi 网络列表页 ----
    void showWifiList(bool force);
    void updateWifiList(bool force);
    void wifiListMove(int8_t delta);
    void wifiListActivate();      // 确定：切换 / 删除
    void wifiListToggleMode();    // 左右：切换「切换/删除」操作模式

    // ---- 音乐播放器绘制 ----
    void showMusicView(MusicView view, bool force);
    void updateMusicPlayer(bool force);
    void drawMusicNowPlaying(bool force);
    void drawMusicLyrics(bool force);
    void drawMusicSpectrum(bool force);
    void drawMusicCover(int16_t x, int16_t y, int16_t box);
    // 切歌加载动画：iOS 风格 12 点菊花，headPhase 为当前亮点位置
    void drawMusicLoadingSpinner(int16_t cx, int16_t cy, int16_t r,
                                 uint8_t headPhase);
    void drawMusicProgress(int16_t y, bool force);
    void drawMusicControls(int16_t cy, bool force);
    void drawMusicVolume(int16_t y, bool force);
    void drawMusicPageIndicator();
    void drawTransportIcon(int16_t cx, int16_t cy, int8_t kind, uint16_t color);
    // 返回当前时间对应的歌词索引，无歌词或未开始时返回 -1
    int16_t currentLyricIndex() const;
    void drawCjkTextClipped(int16_t x, int16_t y, const char *s, uint8_t size,
                            uint16_t fg, uint16_t bg, int16_t maxWidth);
    // 卡拉OK逐字高亮：解析当前句字符边界并记录布局
    void buildKaraokeLine(int16_t lineIdx, int16_t x, int16_t y, uint8_t size,
                          uint16_t litColor, uint16_t baseColor,
                          int16_t maxWidth);
    // 按播放进度推进高亮：整字翻色 + 当前字内部按列分割（像素级扫过）
    void updateKaraoke();
    void drawKaraokeSplitChar(uint8_t charIndex, int16_t fromPx, int16_t toPx);
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
    // iOS 风格应用图标：统一渐变圆角底块 + 白色图形
    void drawAppTile(int16_t x, int16_t y, int16_t size,
                     uint16_t topColor, uint16_t bottomColor);
    void drawMusicIcon(int16_t x, int16_t y, int16_t size, bool selected);
    void drawBooksIcon(int16_t x, int16_t y, int16_t size);
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
    void drawWeatherIcon(int16_t cx, int16_t cy, int16_t r, WeatherIcon icon,
                         bool dimmed);
    void drawWeatherSun(int16_t cx, int16_t cy, int16_t r, uint16_t color,
                        bool rays);
    void drawWeatherMoon(int16_t cx, int16_t cy, int16_t r, uint16_t color);
    void drawWeatherCloud(int16_t cx, int16_t cy, int16_t w, uint16_t color);
    void drawWeatherDrops(int16_t cx, int16_t cy, int16_t w, uint16_t color,
                          uint8_t count, bool snow, uint8_t phase);
    void drawWeatherBolt(int16_t cx, int16_t cy, uint16_t color);
    void updateWeatherAnimation();
    void drawWelcomeDate(int16_t y, bool force);
    void advanceFlipAnimation();
    void updateTrackInfo(bool force);
    void updateSpectrum(bool force);
    void updatePlayIcon(bool force);
    void updateVolume(bool force);

    void drawText(int16_t x, int16_t y, const char *s, uint8_t size, uint16_t fg, uint16_t bg);
    // 内嵌 16x16 点阵中文字库绘制（UTF-8），ASCII 仍走系统字体
    void drawCjkText(int16_t x, int16_t y, const char *s, uint8_t size,
                     uint16_t fg, uint16_t bg);
    int16_t measureCjkText(const char *s, uint8_t size);
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

    // 首页双页轮换：第 1 页翻页时钟，第 2 页天气详情。
    HomeView _homeView = HomeView::Clock;
    uint32_t _homeViewSinceMs = 0;

    // ---- 音乐播放器状态 ----
    MusicView _musicView = MusicView::NowPlaying;
    String    _musicTitle;
    String    _musicArtist;
    String    _musicAlbum;
    uint32_t  _musicDurationMs = 0;
    uint32_t  _musicPositionMs = 0;
    // 歌词扫描专用的平滑时钟：跟随 _musicPositionMs 但单调、少抖动
    uint32_t  _karClockMs = 0;
    uint32_t  _karClockWallMs = 0;
    const uint16_t *_musicCover = nullptr;   // RGB565 封面，nullptr 走占位图
    int16_t   _musicCoverW = 0;
    int16_t   _musicCoverH = 0;
    static constexpr uint16_t kMaxLyricLines = 64;
    LyricLine _musicLyrics[kMaxLyricLines];
    uint16_t  _musicLyricCount = 0;
    int16_t   _lastLyricIndex = -2;
    uint32_t  _lastProgressSec = 0xFFFFFFFF;
    int16_t   _lastProgressFill = -1;   // 上次进度条填充像素宽（游标增量重绘）
    PlayerState _lastMusicState = PlayerState::Stopped;
    uint8_t   _lastMusicVol = 0xFF;
    bool      _musicChromeDirty = true;
    // 曲目加载中（切歌过渡态，歌词/直链尚未就绪）
    bool      _musicLoading = false;
    // 歌词五行上一帧的文本指针，用于跳过未变化的行（局部刷新）
    const char *_lyricRowCache[5] = {nullptr};

    // 卡拉OK逐字高亮状态（LRC 无逐字时间戳，句内按像素宽度线性插值）
    struct Karaoke {
        int16_t  lineIdx = -1;     // 对应歌词行；-1 = 未激活
        int16_t  x = 0;            // 行起始 x
        int16_t  y = 0;
        uint8_t  size = 2;
        uint16_t litColor = 0;     // 已唱颜色
        uint16_t baseColor = 0;    // 未唱颜色（分割字右半用）
        uint8_t  charCount = 0;    // 可见字符数（截断后）
        uint8_t  lit = 0;          // 已完整高亮的字符数
        int16_t  lineW = 0;        // 可见部分总宽（像素）
        int16_t  lastSplit = -1;   // 当前分割字上次绘制的分割列
        uint8_t  truncated = 0;    // 行被截断，屏上尾部有 ".."
        uint8_t  ellipsisLit = 0;  // ".." 已随行尾翻成已唱色
        uint8_t  byteOff[26] = {}; // 每字符起始字节偏移
        int16_t  xOff[26] = {};    // 每字符相对行首的 x 偏移
        uint32_t startMs = 0;
        uint32_t endMs = 0;
    };
    Karaoke _kar;
    uint8_t   _musicDiscPhase = 0;    // 占位唱片旋转相位
    uint32_t  _musicDiscMs = 0;
    uint8_t   _musicSpinPhase = 0;    // 加载菊花相位（12 点环）
    uint32_t  _musicSpinMs = 0;
    uint32_t  _musicRefreshMs = 0;    // 播放页刷新节流时间戳
    uint32_t  _volAdjustMs = 0;       // 频谱页音量调节节流

    // 首页天气缓存
    WeatherClass::Snapshot _weatherSnapshot;
    bool        _weatherValid   = false;
    WeatherIcon _weatherIcon    = WeatherIcon::Unknown;
    String      _weatherText;
    String      _weatherCity;
    bool        _lastWeatherValid = false;
    WeatherIcon _lastWeatherIcon  = WeatherIcon::Unknown;
    String      _lastWeatherText;
    String      _lastWeatherCity;
    bool        _weatherDashboardDirty = true;
    uint8_t     _lastWeatherDashboardMinute = 0xFF;
    uint16_t    _lastWeatherDashboardYear = 0xFFFF;
    uint8_t     _lastWeatherDashboardMonth = 0xFF;
    uint8_t     _lastWeatherDashboardDay = 0xFF;
    // 天气图标动画：相位循环 + 图标中心缓存（局部重绘用）
    uint8_t  _weatherAnimPhase = 0;
    uint32_t _weatherAnimMs   = 0;
    int16_t  _weatherIconCx   = 0;
    int16_t  _weatherIconCy   = 0;
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
    bool     _lastNetworkServiceActive = false;
    String   _lastNetworkServiceIp;
    String   _lastNetworkServiceSsid;

    uint8_t  _settingsSelection = 0;
    uint8_t  _lastSettingsSelection = 0xFF;
    // WiFi 网络列表页状态
    uint8_t  _wifiListSelection = 0;
    uint8_t  _lastWifiListSelection = 0xFF;
    bool     _wifiListModeSwitch = true;   // true=切换 false=删除
    bool     _lastWifiListMode = true;
    uint8_t  _wifiListCount = 0;
    String   _wifiListCurrent;             // 当前连接 SSID
    bool     _wifiListDirty = true;
    // 听书列表页状态
    uint8_t  _booksSelection = 0;
    uint8_t  _lastBooksSelection = 0xFF;
    uint8_t  _booksCount = 0;
    bool     _booksDirty = true;
    bool     _bookListRequested = false;
    bool     _showBookDetail = false;   // 简介页开关

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
    // 24 覆盖频谱页（24 柱）与旧播放页（16 柱）两种用途
    uint8_t  _barH[24]   = {};
    // 音乐频谱的 Q4 亚像素高度（h×16）：整数插值步长忽大忽小是抽搐的
    // 显示端来源，亚像素域插值可以 <1px 的步幅连续滑动
    uint16_t _barHQ[24]  = {};
    uint32_t _specFrameMs = 0;   // 上帧频谱绘制时刻（dt 感知插值用）

    static constexpr uint16_t STATUS_H   = 28;
    // 状态栏图标边长：16px 下笔画太细，在这块压暗严重的 TFT 上发灰发糊
    static constexpr int16_t kStatusIcon = 18;

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
    static constexpr uint32_t kHomeRotateMs = 12000;

    int16_t flipCardX(int idx, int16_t x0) const;

    // Apple 深色主题：iOS dark 官方色板 → RGB565
    static constexpr uint16_t COLOR_BG     = 0x0000;  // 纯黑（iOS/watchOS 深色底）
    static constexpr uint16_t COLOR_BAR    = 0x0000;  // 状态栏与背景同色，靠发丝线分隔
    static constexpr uint16_t COLOR_TEXT   = 0xFFFF;  // #FFFFFF 主文字
    static constexpr uint16_t COLOR_ACCENT = 0x0C3F;  // #0A84FF 系统蓝
    static constexpr uint16_t COLOR_DIM    = 0x8C72;  // #8E8E93 次要文字
    // 这块 TFT 压暗明显，标签类小字用更亮一档才看得清（iOS secondaryLabel 级）
    static constexpr uint16_t COLOR_LABEL  = 0xC618;  // #C7C7CC 标签文字
    static constexpr uint16_t COLOR_LINE   = 0x39C7;  // #38383A 发丝分隔线
    static constexpr uint16_t COLOR_GREEN  = 0x368B;  // #30D158 系统绿（连接成功/电量）
    static constexpr uint16_t COLOR_ALERT  = 0xFA27;  // #FF453A 系统红（错误）
    // 翻页卡：iOS 灰阶，但比标准 secondaryBackground 提亮两档——
    // 这块 TFT 面板低灰压黑严重，#1C1C1E 在实机上与纯黑不可分。
    static constexpr uint16_t COLOR_CARD        = 0x2966;  // #2C2C2E 下半片
    static constexpr uint16_t COLOR_CARD_TOP    = 0x39E8;  // #3C3C3E 上半片
    static constexpr uint16_t COLOR_CARD_EDGE   = 0x52AA;  // #545456 卡片外缘
    static constexpr uint16_t COLOR_FLIP_LINE   = 0x6B4D;  // #6A6A6C 中缝高光
    static constexpr uint16_t COLOR_FLIP_SHADOW = 0x1082;  // 中缝阴影
    static constexpr uint16_t COLOR_PANEL       = 0x2104;  // #202022 面板/卡片底
};

extern DisplayClass Display;

#endif // DISPLAY_H
