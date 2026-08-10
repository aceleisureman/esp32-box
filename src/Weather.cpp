#include "Weather.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

WeatherClass Weather;

namespace {

// 从扁平 JSON 中取出 "key": 后的原始片段（不做完整解析，够用且省 RAM）
const char *findValue(const char *json, const char *key) {
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return nullptr;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    return *p ? p : nullptr;
}

bool jsonNumber(const char *json, const char *key, float &out) {
    const char *p = findValue(json, key);
    if (!p) return false;
    char *end = nullptr;
    const float v = strtof(p, &end);
    if (end == p) return false;
    out = v;
    return true;
}

bool jsonString(const char *json, const char *key, char *out, size_t outSize) {
    const char *p = findValue(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outSize) {
        // 无 CJK 字库，非 ASCII 一律降级为 '?'，避免画出乱码方块
        out[i++] = (*p >= 0x20 && *p < 0x7F) ? *p : '?';
        p++;
    }
    out[i] = '\0';
    return i > 0;
}

// 读取形如 "temperature_2m_max":[28.1,29.0,...] 的数值数组。
// 返回实际读取数量，避免引入完整 JSON 库和额外堆内存。
size_t jsonNumberArray(const char *json, const char *key,
                       float *out, size_t maxCount) {
    const char *p = findValue(json, key);
    if (!p || *p != '[' || !out || maxCount == 0) return 0;
    p++;

    size_t count = 0;
    while (*p && *p != ']' && count < maxCount) {
        while (*p == ' ' || *p == '\t' || *p == '\r' ||
               *p == '\n' || *p == ',') {
            p++;
        }
        if (*p == ']') break;

        char *end = nullptr;
        const float value = strtof(p, &end);
        if (end == p) break;
        out[count++] = value;
        p = end;
    }
    return count;
}

// WMO weather code → 图标类别 + 英文短描述
WeatherIcon iconFromWmo(int code, bool isDay, const char *&label) {
    switch (code) {
        case 0:
            label = "Clear";
            return isDay ? WeatherIcon::ClearDay : WeatherIcon::ClearNight;
        case 1:
            label = "Mostly Clear";
            return isDay ? WeatherIcon::ClearDay : WeatherIcon::ClearNight;
        case 2:
            label = "Partly Cloudy";
            return isDay ? WeatherIcon::PartlyDay : WeatherIcon::PartlyNight;
        case 3:
            label = "Overcast";
            return WeatherIcon::Cloudy;
        case 45: case 48:
            label = "Fog";
            return WeatherIcon::Fog;
        case 51: case 53: case 55:
        case 56: case 57:
            label = "Drizzle";
            return WeatherIcon::Drizzle;
        case 61: case 63: case 65:
        case 66: case 67:
            label = "Rain";
            return WeatherIcon::Rain;
        case 71: case 73: case 75: case 77:
            label = "Snow";
            return WeatherIcon::Snow;
        case 80: case 81: case 82:
            label = "Showers";
            return WeatherIcon::Showers;
        case 85: case 86:
            label = "Snow Showers";
            return WeatherIcon::Snow;
        case 95: case 96: case 99:
            label = "Thunderstorm";
            return WeatherIcon::Thunder;
        default:
            label = "--";
            return WeatherIcon::Unknown;
    }
}

bool httpGet(const char *url, bool secure, String &body) {
    HTTPClient http;
    bool begun = false;

    WiFiClientSecure tlsClient;
    WiFiClient plainClient;
    if (secure) {
        // 天气/定位为公开只读数据，不校验证书以省去证书维护
        tlsClient.setInsecure();
        tlsClient.setTimeout(8);
        begun = http.begin(tlsClient, url);
    } else {
        plainClient.setTimeout(8);
        begun = http.begin(plainClient, url);
    }
    if (!begun) {
        Serial.printf("[WX] begin failed %s\n", url);
        return false;
    }

    http.setTimeout(8000);
    http.setConnectTimeout(8000);
    http.useHTTP10(true);
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[WX] GET %s -> %d\n", url, code);
        http.end();
        return false;
    }
    body = http.getString();
    http.end();
    return body.length() > 0;
}

}  // namespace

const char *weatherIconName(WeatherIcon icon) {
    switch (icon) {
        case WeatherIcon::ClearDay:    return "clear-day";
        case WeatherIcon::ClearNight:  return "clear-night";
        case WeatherIcon::PartlyDay:   return "partly-day";
        case WeatherIcon::PartlyNight: return "partly-night";
        case WeatherIcon::Cloudy:      return "cloudy";
        case WeatherIcon::Fog:         return "fog";
        case WeatherIcon::Drizzle:     return "drizzle";
        case WeatherIcon::Rain:        return "rain";
        case WeatherIcon::Showers:     return "showers";
        case WeatherIcon::Snow:        return "snow";
        case WeatherIcon::Thunder:     return "thunder";
        default:                       return "unknown";
    }
}

void WeatherClass::init() {
    if (_lock) return;
    _lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(&WeatherClass::taskEntry, "weather", 6144, this,
                            2, &_task, 0);
    Serial.println("[WX] weather task started");
}

void WeatherClass::update(bool wifiConnected) {
    if (wifiConnected != _wifiUp) {
        _wifiUp = wifiConnected;
        if (wifiConnected) {
            _forceRefresh = true;
        } else {
            // 断网时保留上次读数，但标记为过期由 UI 灰显
            _located = false;
            publish(false);
        }
    }
}

void WeatherClass::snapshot(Snapshot &out) {
    if (!_lock) {
        out = Snapshot();
        return;
    }
    xSemaphoreTake(_lock, portMAX_DELAY);
    out = _shared;
    xSemaphoreGive(_lock);
}

void WeatherClass::publish(bool valid) {
    if (!_lock) return;
    xSemaphoreTake(_lock, portMAX_DELAY);
    _shared.valid = valid;
    if (valid) {
        _shared.tempC = _tempC;
        _shared.icon  = _icon;
        _shared.humidityPct = _humidityPct;
        _shared.windKph = _windKph;
        _shared.rainChancePct = _rainChancePct;
        _shared.forecastCount = _forecastCount;
        for (uint8_t i = 0; i < kForecastDays; i++) {
            _shared.forecast[i] = _forecast[i];
        }
        snprintf(_shared.condition, sizeof(_shared.condition), "%s", _condition);
        snprintf(_shared.city, sizeof(_shared.city), "%s", _city);
    }
    xSemaphoreGive(_lock);
}

void WeatherClass::taskEntry(void *arg) {
    static_cast<WeatherClass *>(arg)->taskLoop();
}

void WeatherClass::taskLoop() {
    uint32_t nextFetchMs = 0;

    for (;;) {
        const bool wifiUp = _wifiUp;
        const uint32_t now = millis();

        if (!wifiUp) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!_forceRefresh && (int32_t)(now - nextFetchMs) < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        _forceRefresh = false;

        bool ok = _located || fetchLocation();
        if (ok) ok = fetchWeather();
        publish(ok);

        nextFetchMs = millis() + (ok ? kRefreshMs : kRetryMs);
        Serial.printf("[WX] fetch %s next in %lus\n",
                      ok ? "ok" : "failed",
                      (unsigned long)((ok ? kRefreshMs : kRetryMs) / 1000));
    }
}

// 出口 IP 定位。ip-api.com 免费额度对单设备轮询足够，且支持纯 HTTP。
bool WeatherClass::fetchLocation() {
    String body;
    if (!httpGet("http://ip-api.com/json/?fields=status,city,lat,lon",
                 false, body)) {
        return false;
    }

    char status[12] = "";
    if (!jsonString(body.c_str(), "status", status, sizeof(status)) ||
        strcmp(status, "success") != 0) {
        Serial.printf("[WX] locate rejected: %s\n", status);
        return false;
    }
    if (!jsonNumber(body.c_str(), "lat", _lat) ||
        !jsonNumber(body.c_str(), "lon", _lon)) {
        return false;
    }
    if (!jsonString(body.c_str(), "city", _city, sizeof(_city))) {
        snprintf(_city, sizeof(_city), "--");
    }

    _located = true;
    Serial.printf("[WX] located %s %.3f,%.3f\n", _city, _lat, _lon);
    return true;
}

bool WeatherClass::fetchWeather() {
    char url[384];
    const int urlLength = snprintf(
        url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.3f&longitude=%.3f"
        "&current=temperature_2m,relative_humidity_2m,weather_code,is_day,wind_speed_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
        "&forecast_days=%u&timezone=auto",
        _lat, _lon, (unsigned)kForecastDays);
    if (urlLength <= 0 || (size_t)urlLength >= sizeof(url)) {
        Serial.println("[WX] forecast URL too long");
        return false;
    }

    String body;
    if (!httpGet(url, true, body)) {
        // 定位可能已失效（换网络/换出口），下轮重新定位
        _located = false;
        return false;
    }

    // "current" 对象里才有实况值，从该处起解析避免命中 units 段。
    const char *cur = strstr(body.c_str(), "\"current\":");
    if (!cur) return false;

    float temp = 0.0f;
    float code = -1.0f;
    float isDay = 1.0f;
    float humidity = -1.0f;
    float wind = -1.0f;
    if (!jsonNumber(cur, "temperature_2m", temp)) return false;
    jsonNumber(cur, "weather_code", code);
    jsonNumber(cur, "is_day", isDay);
    _humidityPct = jsonNumber(cur, "relative_humidity_2m", humidity)
        ? (uint8_t)constrain(lroundf(humidity), 0L, 100L) : 255;
    _windKph = jsonNumber(cur, "wind_speed_10m", wind)
        ? (uint16_t)constrain(lroundf(wind), 0L, 999L) : 0xFFFF;

    const char *label = "--";
    _icon  = iconFromWmo((int)lroundf(code), isDay > 0.5f, label);
    _tempC = (int8_t)constrain(lroundf(temp), -60L, 60L);
    snprintf(_condition, sizeof(_condition), "%s", label);

    // 日预报用于第二首页的趋势线和 5 日卡片。
    _forecastCount = 0;
    _rainChancePct = 255;
    for (uint8_t i = 0; i < kForecastDays; i++) {
        _forecast[i] = ForecastDay();
    }

    const char *daily = strstr(body.c_str(), "\"daily\":");
    if (daily) {
        float codes[kForecastDays] = {};
        float maxTemps[kForecastDays] = {};
        float minTemps[kForecastDays] = {};
        float rainChance[kForecastDays] = {};
        const size_t codeCount = jsonNumberArray(
            daily, "weather_code", codes, kForecastDays);
        const size_t maxCount = jsonNumberArray(
            daily, "temperature_2m_max", maxTemps, kForecastDays);
        const size_t minCount = jsonNumberArray(
            daily, "temperature_2m_min", minTemps, kForecastDays);
        const size_t rainCount = jsonNumberArray(
            daily, "precipitation_probability_max", rainChance, kForecastDays);

        size_t count = codeCount;
        if (maxCount < count) count = maxCount;
        if (minCount < count) count = minCount;
        if (count > kForecastDays) count = kForecastDays;
        _forecastCount = (uint8_t)count;
        for (uint8_t i = 0; i < _forecastCount; i++) {
            const char *forecastLabel = "--";
            _forecast[i].icon = iconFromWmo(
                (int)lroundf(codes[i]), true, forecastLabel);
            _forecast[i].maxC = (int8_t)constrain(
                lroundf(maxTemps[i]), -60L, 60L);
            _forecast[i].minC = (int8_t)constrain(
                lroundf(minTemps[i]), -60L, 60L);
        }
        if (rainCount > 0) {
            _rainChancePct = (uint8_t)constrain(
                lroundf(rainChance[0]), 0L, 100L);
        }
    }

    Serial.printf(
        "[WX] %s %dC %s humidity=%u%% wind=%ukm/h rain=%u%% forecast=%u "
        "(wmo=%d day=%d)\n",
        _city, (int)_tempC, _condition,
        _humidityPct <= 100 ? (unsigned)_humidityPct : 0U,
        _windKph != 0xFFFF ? (unsigned)_windKph : 0U,
        _rainChancePct <= 100 ? (unsigned)_rainChancePct : 0U,
        (unsigned)_forecastCount,
        (int)lroundf(code), isDay > 0.5f);
    return true;
}

