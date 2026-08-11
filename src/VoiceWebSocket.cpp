#include "VoiceWebSocket.h"
#include "pins_audio.h"

#include <WebSocketsClient.h>
#include <WiFi.h>

VoiceWebSocketClass VoiceWebSocket;

namespace {
WebSocketsClient gSocket;
}

void VoiceWebSocketClass::init(BinaryHandler binaryHandler,
                               TextHandler textHandler) {
    _binaryHandler = binaryHandler;
    _textHandler = textHandler;
    _lock = xSemaphoreCreateRecursiveMutex();
    gSocket.onEvent([](WStype_t type, uint8_t *payload, size_t length) {
        VoiceWebSocket.handleEvent((uint8_t)type, payload, length);
    });
    gSocket.setReconnectInterval(1000);
    gSocket.enableHeartbeat(15000, 3000, 2);
}

void VoiceWebSocketClass::update(bool wifiConnected) {
    if (!wifiConnected) {
        disconnect();
        return;
    }
    if (_lock) xSemaphoreTakeRecursive(_lock, portMAX_DELAY);
    if (!_started) {
        gSocket.begin(VOICE_WS_HOST, VOICE_WS_PORT, VOICE_WS_PATH);
        _started = true;
    }
    gSocket.loop();
    if (_lock) xSemaphoreGiveRecursive(_lock);
}

bool VoiceWebSocketClass::sendAudio(const uint8_t *data, size_t len) {
    if (_lock) xSemaphoreTakeRecursive(_lock, portMAX_DELAY);
    const bool ok = _connected && data && len && len <= 4096 && gSocket.sendBIN(data, len);
    if (_lock) xSemaphoreGiveRecursive(_lock);
    return ok;
}

bool VoiceWebSocketClass::sendEvent(const char *json) {
    if (_lock) xSemaphoreTakeRecursive(_lock, portMAX_DELAY);
    const bool ok = _connected && json && gSocket.sendTXT(json);
    if (_lock) xSemaphoreGiveRecursive(_lock);
    return ok;
}

bool VoiceWebSocketClass::connected() const { return _connected; }

void VoiceWebSocketClass::disconnect() {
    if (_lock) xSemaphoreTakeRecursive(_lock, portMAX_DELAY);
    if (_started) gSocket.disconnect();
    _started = false;
    _connected = false;
    if (_lock) xSemaphoreGiveRecursive(_lock);
}

void VoiceWebSocketClass::onEvent(uint8_t, uint8_t *, size_t) {}

// 设备唯一标识：取 WiFi MAC 后三字节，形如 "esp32-box-a1b2c3"。
// 后端按 device_id 隔离每台设备的对话记忆，因此同一份固件烧到多台
// 设备上也能各自拥有独立记忆，无需逐台配置。
const char *VoiceWebSocketClass::deviceId() {
    static char id[24] = {0};
    if (!id[0]) {
        uint8_t mac[6] = {0};
        WiFi.macAddress(mac);
        snprintf(id, sizeof(id), "esp32-box-%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
    }
    return id;
}

void VoiceWebSocketClass::handleEvent(uint8_t rawType, uint8_t *payload,
                                      size_t length) {
    const WStype_t type = (WStype_t)rawType;
    if (type == WStype_CONNECTED) {
        _connected = true;
        char start[96];
        snprintf(start, sizeof(start),
                 "{\"type\":\"session.start\",\"device_id\":\"%s\"}",
                 deviceId());
        sendEvent(start);
        Serial.printf("[VOICE][WS] connected id=%s\n", deviceId());
    } else if (type == WStype_DISCONNECTED) {
        _connected = false;
        Serial.println("[VOICE][WS] disconnected");
    } else if (type == WStype_BIN && _binaryHandler) {
        _binaryHandler(payload, length);
    } else if (type == WStype_TEXT && _textHandler) {
        _textHandler((const char *)payload, length);
    }
}
