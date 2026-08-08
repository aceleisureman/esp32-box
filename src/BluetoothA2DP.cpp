#include "BluetoothA2DP.h"

BluetoothA2DPClass::BluetoothA2DPClass()
    : _connected(false),
      _state(PlayerState::Stopped),
      _volume(50),
      _connectTime(0) {}

void BluetoothA2DPClass::init() {
    // S3 无 Classic BT：不启动 A2DP / I2S sink。
    // I2S 引脚预留（MAX98357A: BCLK=26, LRC=25, DIN=22），待外挂音源接入。
    _connected = false;
    _state = PlayerState::Stopped;
    _connectTime = 0;
    _track.title = "";
    _track.artist = "";
    _track.album = "";

    Serial.println("[BT] Stub: disconnected (S3 no Classic A2DP)");
    Serial.println("[BT] UI shows welcome until connected");
}

void BluetoothA2DPClass::update() {
    // 无流可处理；外挂模块时可在此轮询 UART/状态
}

void BluetoothA2DPClass::togglePlayPause() {
    if (_state == PlayerState::Playing) {
        _state = PlayerState::Paused;
        Serial.println("[BT] Stub: pause");
    } else {
        _state = PlayerState::Playing;
        Serial.println("[BT] Stub: play");
    }
}

void BluetoothA2DPClass::volumeUp() {
    if (_volume < 100) {
        _volume++;
    }
    Serial.printf("[BT] Stub vol %u%%\n", _volume);
}

void BluetoothA2DPClass::volumeDown() {
    if (_volume > 0) {
        _volume--;
    }
    Serial.printf("[BT] Stub vol %u%%\n", _volume);
}

BluetoothA2DPClass BluetoothA2DP;
