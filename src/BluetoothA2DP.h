#pragma once
#ifndef BLUETOOTHA2DP_H
#define BLUETOOTHA2DP_H

#include <Arduino.h>

// ESP32-S3 无 Bluetooth Classic，无法使用 A2DP。
// 本模块为 UI / 按键保留的桩实现，接口与原先 A2DP 封装一致。

struct TrackInfo {
    String title;
    String artist;
    String album;
};

enum class PlayerState : uint8_t { Stopped, Playing, Paused };

class BluetoothA2DPClass {
public:
    BluetoothA2DPClass();

    void init();
    void update();

    bool isConnected() const { return _connected; }
    PlayerState getState() const { return _state; }
    uint8_t getVolume() const { return _volume; }
    const TrackInfo& getTrack() const { return _track; }
    uint32_t getConnectTime() const { return _connectTime; }

    void togglePlayPause();
    void volumeUp();
    void volumeDown();

private:
    bool _connected;
    PlayerState _state;
    uint8_t _volume;
    TrackInfo _track;
    uint32_t _connectTime;
};

extern BluetoothA2DPClass BluetoothA2DP;

#endif // BLUETOOTHA2DP_H
