#pragma once

#include <Arduino.h>

class VoiceWebSocketClass {
public:
    using BinaryHandler = void (*)(const uint8_t *, size_t);
    using TextHandler = void (*)(const char *, size_t);

    void init(BinaryHandler binaryHandler, TextHandler textHandler);
    void update(bool wifiConnected);
    bool sendAudio(const uint8_t *data, size_t len);
    bool sendEvent(const char *json);
    bool connected() const;
    void disconnect();
    // 本机唯一设备标识（由 MAC 生成），后端据此隔离每台设备的记忆
    static const char *deviceId();

private:
    static void onEvent(uint8_t type, uint8_t *payload, size_t length);
    void handleEvent(uint8_t type, uint8_t *payload, size_t length);

    BinaryHandler _binaryHandler = nullptr;
    TextHandler _textHandler = nullptr;
    bool _started = false;
    bool _connected = false;
    SemaphoreHandle_t _lock = nullptr;
};

extern VoiceWebSocketClass VoiceWebSocket;
