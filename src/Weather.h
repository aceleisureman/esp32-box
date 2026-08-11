#pragma once
#ifndef WEATHER_H
#define WEATHER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// 天气图标类别（与具体气象编码解耦，Display 只认这一套）
enum class WeatherIcon : uint8_t {
    Unknown = 0,
    ClearDay,
    ClearNight,
    PartlyDay,
    PartlyNight,
    Cloudy,
    Fog,
    Drizzle,
    Rain,
    Showers,
    Snow,
    Thunder,
};

// 联网后：先按出口 IP 定位，再拉取该经纬度的实况天气。
// 网络请求在独立任务中执行，主循环只读快照，不会阻塞 UI 刷新。
class WeatherClass {
public:
    static constexpr uint8_t kForecastDays = 5;

    struct ForecastDay {
        int8_t      minC = -128;
        int8_t      maxC = -128;
        WeatherIcon icon = WeatherIcon::Unknown;
    };

    struct Snapshot {
        bool        valid      = false;
        int8_t      tempC      = 0;
        WeatherIcon icon       = WeatherIcon::Unknown;
        uint8_t     humidityPct = 255;   // 255 = unavailable
        uint16_t    windKph     = 0xFFFF;
        uint8_t     rainChancePct = 255;
        uint8_t     forecastCount = 0;
        ForecastDay forecast[kForecastDays] = {};
        char        condition[20] = "";
        char        city[24]      = "";
    };

    void init();
    // 每轮主循环调用；wifiConnected 变化时触发/停止抓取
    void update(bool wifiConnected);
    // 拷贝一份当前快照（线程安全）
    void snapshot(Snapshot &out);
    // 强制下次循环立即重新抓取
    void requestRefresh() { _forceRefresh = true; }

private:
    static void taskEntry(void *arg);
    void        taskLoop();
    bool        fetchLocation();
    bool        fetchWeather();
    void        publish(bool valid);

    SemaphoreHandle_t _lock       = nullptr;
    TaskHandle_t      _task       = nullptr;
    volatile bool     _wifiUp     = false;
    volatile bool     _forceRefresh = false;
    volatile uint32_t _wifiConnectedAtMs = 0;

    // 仅天气任务内部访问
    bool   _located = false;
    float  _lat = 0.0f;
    float  _lon = 0.0f;
    char   _city[24] = "";
    int8_t _tempC = 0;
    WeatherIcon _icon = WeatherIcon::Unknown;
    uint8_t  _humidityPct = 255;
    uint16_t _windKph = 0xFFFF;
    uint8_t  _rainChancePct = 255;
    uint8_t  _forecastCount = 0;
    ForecastDay _forecast[kForecastDays] = {};
    char   _condition[20] = "";

    // 受 _lock 保护
    Snapshot _shared;

    static constexpr uint32_t kRefreshMs    = 10UL * 60UL * 1000UL;  // 正常刷新周期
    static constexpr uint32_t kInitialDelayMs = 6000;                 // 联网后错开音乐/TLS 初始化
    static constexpr uint32_t kRetryInitialMs = 15UL * 1000UL;
    static constexpr uint32_t kRetryMaxMs     = 5UL * 60UL * 1000UL;
    static constexpr uint32_t kHttpTimeout  = 8000;
};

extern WeatherClass Weather;

const char *weatherIconName(WeatherIcon icon);

#endif // WEATHER_H
