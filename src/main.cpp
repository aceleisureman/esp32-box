#include <Arduino.h>
#include "BluetoothA2DP.h"
#include "Display.h"
#include "Spectrum.h"
#include "Input.h"
#include "WifiProvisioning.h"
#include "Weather.h"
#include "MusicService.h"
#include "AudioPlayer.h"
#include "VoiceAssistant.h"
#include "WakeWord.h"
#include <esp_system.h>
#include <esp_sntp.h>
#include <time.h>

namespace {

bool weatherSnapshotsEqual(const WeatherClass::Snapshot &a,
                           const WeatherClass::Snapshot &b) {
    if (a.valid != b.valid || a.tempC != b.tempC || a.icon != b.icon ||
        a.humidityPct != b.humidityPct || a.windKph != b.windKph ||
        a.rainChancePct != b.rainChancePct ||
        a.forecastCount != b.forecastCount ||
        strcmp(a.condition, b.condition) != 0 || strcmp(a.city, b.city) != 0) {
        return false;
    }
    for (uint8_t i = 0; i < WeatherClass::kForecastDays; i++) {
        if (a.forecast[i].minC != b.forecast[i].minC ||
            a.forecast[i].maxC != b.forecast[i].maxC ||
            a.forecast[i].icon != b.forecast[i].icon) {
            return false;
        }
    }
    return true;
}

// 将天气任务的快照同步到显示层（有变化才推送，避免多余重绘）
void updateWeatherDisplay(bool wifiConnected) {
    Weather.update(wifiConnected);

    static WeatherClass::Snapshot lastShown;
    static bool everShown = false;
    WeatherClass::Snapshot snap;
    Weather.snapshot(snap);

    const bool changed = !everShown || !weatherSnapshotsEqual(snap, lastShown);
    if (!changed) return;

    Display.setWeather(snap);
    lastShown = snap;
    everShown = true;
    if (snap.valid) {
        Serial.printf("[WX] display %dC %s %s icon=%s\n",
                      (int)snap.tempC, snap.condition, snap.city,
                      weatherIconName(snap.icon));
    }
}

// 音乐服务 → 显示层同步。歌单/歌词由内网 CloudMusic Tools 后端提供，
// 曲目或歌词变化时才推送给 Display，避免多余重绘。
void updateMusicDisplay(bool wifiConnected) {
    MusicService.update(wifiConnected);

    static uint32_t lastTrackId = 0;
    static uint16_t lastLyricCount = 0xFFFF;
    static MusicServiceClass::State lastState = MusicServiceClass::State::Idle;
    static bool lastCoverReady = false;
    static uint8_t lastIndex = 0xFF;

    const MusicServiceClass::State state = MusicService.state();
    const uint8_t index = MusicService.currentIndex();
    const uint16_t lyricCount = MusicService.lyricCount();
    const bool coverReady = MusicService.coverReady();

    // 切歌瞬间（index 变化）：立即进入加载态，清空旧内容并停止旧音频。
    // 新歌词/直链/封面由网络任务异步到达，逐个更新到显示层。
    if (index != lastIndex) {
        lastIndex = index;
        Display.setMusicLoading();
        AudioPlayer.stop();
        Serial.printf("[NCM] switching to #%u, loading...\n", (unsigned)index);
    }

    MusicServiceClass::Track track;
    const bool hasTrack = state == MusicServiceClass::State::Ready &&
                          MusicService.track(index, track);
    // 用曲目 ID 而非下标判断变化：漫游续拉新批次后下标同样归 0，
    // 只看下标会漏掉"换了一批歌但仍在第 0 首"的情况。
    const uint32_t trackId = hasTrack ? track.id : 0;

    const bool changed = trackId != lastTrackId ||
                         lyricCount != lastLyricCount ||
                         state != lastState || coverReady != lastCoverReady;
    if (!changed) return;
    lastTrackId = trackId;
    lastLyricCount = lyricCount;
    lastState = state;
    lastCoverReady = coverReady;

    if (hasTrack) {
        // 封面就绪才传位图，否则传 nullptr 走黑胶占位图
        const uint16_t *cover = coverReady ? MusicService.coverPixels() : nullptr;
        const uint16_t coverSize = coverReady ? MusicService.coverSize() : 0;
        Display.setNowPlaying(track.title, track.artist, track.album,
                              track.durationMs,
                              cover, (int16_t)coverSize, (int16_t)coverSize);
        Serial.printf("[NCM] now playing #%u %s - %s (%u lyric lines, cover=%d)\n",
                      (unsigned)index, track.title, track.artist,
                      (unsigned)lyricCount, coverReady ? 1 : 0);
    } else {
        const char *tip = state == MusicServiceClass::State::Loading
                              ? "Loading..."
                          : state == MusicServiceClass::State::Failed
                              ? "歌单获取失败"
                              : "未连接网络";
        Display.setNowPlaying(tip, "", "", 0);
    }

    // 歌词整体替换：服务侧是定长数组，这里转成显示层的 LyricLine
    static DisplayClass::LyricLine lines[MusicServiceClass::kMaxLyrics];
    const uint16_t count = lyricCount > MusicServiceClass::kMaxLyrics
                               ? MusicServiceClass::kMaxLyrics : lyricCount;
    for (uint16_t i = 0; i < count; i++) {
        MusicServiceClass::LyricLine src;
        if (!MusicService.lyric(i, src)) break;
        lines[i].startMs = src.startMs;
        lines[i].text = src.text;
    }
    Display.setLyrics(lines, count);
}

// 播放链路：拿到新曲目的直链就备妥/开播，进度取解码器的真实位置。
void updateMusicPlayback() {
    // 直链检查每 300ms 一次即可：playUrl() 要加锁并拷贝字符串，
    // 每轮 loop 都调会和音频任务抢锁，拖慢 UI 刷新。
    static uint32_t lastCheckMs = 0;
    static String lastUrl;
    // 开机停在备妥态，进入音乐页后才自动出声——上电即响太突兀，
    // 而进了播放页就是明确的收听意图，不该再等一次按键。
    static bool autoPlay = false;

    // 语音控制在确认回复播完后转交播放权。
    if (VoiceAssistant.takeMusicPlayRequest()) {
        autoPlay = true;
        const String url = MusicService.playUrl();
        if (url.length()) {
            AudioPlayer.play(url, MusicService.playFormat());
            lastUrl = url;
            Serial.println("[AUDIO] started by voice command");
        }
    }

    // 进入音乐页：立刻播。直链还没到就先置位，等它到达时直接开播。
    if (Display.takeMusicEnterRequest() && !autoPlay) {
        autoPlay = true;
        const String url = MusicService.playUrl();
        if (url.length() && !VoiceAssistant.enabled()) {
            AudioPlayer.play(url, MusicService.playFormat());
            lastUrl = url;
            Serial.println("[AUDIO] auto-play on entering music page");
        }
    }

    const uint32_t now = millis();
    if (now - lastCheckMs >= 300) {
        lastCheckMs = now;
        const String url = MusicService.playUrl();
        if (url.length() && url != lastUrl) {
            lastUrl = url;
            // 直链就绪时一并取格式，供播放器选择解码器（MP3/FLAC）
            const AudioPlayerClass::PlayFormat fmt = MusicService.playFormat();
            if (VoiceAssistant.enabled()) {
                AudioPlayer.cancelPending();
                Serial.println("[AUDIO] ignored while voice assistant is active");
            } else if (autoPlay && !AudioPlayer.isPromptActive()) {
                // 已进过音乐页：切歌 / 播完续曲 / 进页时直链刚到，都直接播
                AudioPlayer.play(url, fmt);
                Serial.printf("[AUDIO] start: %.60s...\n", url.c_str());
            } else {
                // 还没进过音乐页，或当前在播 TTS 提示音：先备妥/缓着，
                // 提示音播完后再接续
                AudioPlayer.prepare(url, fmt);
                Serial.println("[AUDIO] armed (prompt or not-yet-open)");
            }
        }

        // 播放自然结束 → 自动下一首
        if (AudioPlayer.takeFinishedEvent()) {
            MusicService.next();
        }
    }

    // 进度是 volatile 读取，无锁，可以每轮更新
    Display.setPlaybackPosition(AudioPlayer.positionMs());
}

void updateNetworkTime(bool wifiConnected) {
    static bool previousWifiConnected = false;
    static bool syncPending = false;
    static bool syncApplied = false;
    static uint32_t syncStartedMs = 0;
    static uint32_t lastPollMs = 0;

    if (wifiConnected && !previousWifiConnected) {
        // China Standard Time is UTC+8 with no daylight-saving adjustment.
        sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
        configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org",
                     "time.windows.com");
        syncPending = true;
        syncApplied = false;
        syncStartedMs = millis();
        lastPollMs = 0;
        Serial.printf("[TIME][%lu] WiFi connected; NTP sync requested\n",
                      (unsigned long)syncStartedMs);
    }

    if (!wifiConnected) {
        syncPending = false;
        syncApplied = false;
    }

    previousWifiConnected = wifiConnected;
    if (!syncPending || syncApplied || millis() - lastPollMs < 500) return;
    lastPollMs = millis();

    const time_t now = time(nullptr);
    if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        if (millis() - syncStartedMs >= 30000) {
            Serial.printf("[TIME][%lu] NTP sync timeout status=%d epoch=%ld\n",
                          (unsigned long)millis(),
                          (int)sntp_get_sync_status(),
                          (long)now);
            syncPending = false;
        }
        return;
    }
    // A valid NTP timestamp is well beyond the default pre-sync epoch.
    if (now < 1700000000) {
        if (millis() - syncStartedMs >= 30000) {
            Serial.printf("[TIME][%lu] NTP sync timeout epoch=%ld\n",
                          (unsigned long)millis(), (long)now);
            syncPending = false;
        }
        return;
    }

    struct tm localTime = {};
    if (!localtime_r(&now, &localTime)) return;
    if (localTime.tm_year + 1900 < 2024 || localTime.tm_year + 1900 > 2100) return;

    Display.setClock((uint8_t)localTime.tm_hour,
                     (uint8_t)localTime.tm_min,
                     (uint8_t)localTime.tm_sec);
    Display.setDate((uint16_t)(localTime.tm_year + 1900),
                    (uint8_t)(localTime.tm_mon + 1),
                    (uint8_t)localTime.tm_mday);
    syncApplied = true;
    syncPending = false;
    Serial.printf("[TIME][%lu] NTP sync applied %04d-%02d-%02d %02d:%02d:%02d\n",
                  (unsigned long)millis(),
                  localTime.tm_year + 1900,
                  localTime.tm_mon + 1,
                  localTime.tm_mday,
                  localTime.tm_hour,
                  localTime.tm_min,
                  localTime.tm_sec);
}

}

static const char *resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "OTHER";
    }
}

void setup() {
    Serial.begin(115200);
    delay(800);

    Serial.println();
    Serial.println("=================================");
    Serial.println("[BOOT] ESP32-S3-WROOM-1-N16R8");
    Serial.println("[BOOT] Home clock/weather pages rotate when BT is off");
    Serial.println("=================================");
    const esp_reset_reason_t resetReason = esp_reset_reason();
    Serial.printf("[BOOT] reset reason=%d (%s) free heap=%u min heap=%u\n",
                  (int)resetReason,
                  resetReasonName(resetReason),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap());

    BluetoothA2DP.init();
    Display.init();
    Input.init();
    Spectrum.init();
    WifiProvisioning.init();
    Weather.init();
    MusicService.init();
    AudioPlayer.init();
    VoiceAssistant.init();
    WakeWord.init();
    WifiProvisioning.autoConnect();

    Serial.println("[BOOT] setup done");
}

void loop() {
    BluetoothA2DP.update();
    Input.update();
    VoiceAssistant.update();
    WakeWord.update();

    // 语音会话激活 → 自动进入对话页；结束 → 自动退出
    if (VoiceAssistant.enabled()) {
        if (!Display.isVoicePage()) {
            Display.showVoicePage(true);
        } else {
            Display.updateVoicePage(false);
        }
    } else if (Display.isVoicePage()) {
        // 会话结束：回到首页（欢迎页即主页）
        Display.showHome();
    }

    static bool networkPage = false;
    const bool networkPageNow = Display.isNetworkSettings();
    if (networkPageNow && !networkPage) {
        Serial.printf("[LOOP][%lu][heap=%u] network page entered, starting AP provisioning\n",
                      (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
        WifiProvisioning.start();
        Serial.printf("[LOOP][%lu][heap=%u] provisioning.start returned active=%s\n",
                      (unsigned long)millis(), (unsigned)ESP.getFreeHeap(),
                      WifiProvisioning.isActive() ? "true" : "false");
    }
    if (!networkPageNow && networkPage) {
        Serial.printf("[LOOP][%lu][heap=%u] network page exited, stopping provisioning\n",
                      (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
        WifiProvisioning.stop();
    }
    networkPage = networkPageNow;
    if (Display.takeNetworkRescanRequest()) WifiProvisioning.requestScan();
    WifiProvisioning.update();
    const bool wifiConnected = WifiProvisioning.isStationConnected();
    updateNetworkTime(wifiConnected);
    updateWeatherDisplay(wifiConnected);
    updateMusicDisplay(wifiConnected);
    Display.setWifiConnected(wifiConnected);
    Display.setNetworkServiceState(WifiProvisioning.isActive(),
                                    WifiProvisioning.status(),
                                    WifiProvisioning.ipAddress(),
                                    WifiProvisioningClass::PROVISIONING_NAME,
                                    WifiProvisioning.qrPayload());

    // 仅蓝牙已连接时跑频谱，待机页不算
    if (BluetoothA2DP.isConnected()) {
        Spectrum.update();
    }
    // 音乐播放链路常驻运行（切到别的页面也不断音）
    updateMusicPlayback();
    if (Display.isMusicPlayer()) {
        Spectrum.update();
    }
    // 听书页请求书籍列表
    if (Display.takeBookListRequest()) MusicService.requestBookList();

    Display.render();
    delay(20);
}
