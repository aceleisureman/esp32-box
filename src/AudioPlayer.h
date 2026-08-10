#pragma once
#ifndef AUDIOPLAYER_H
#define AUDIOPLAYER_H

#include <Arduino.h>

// HTTP 流式 MP3 / FLAC 播放 → I2S → MAX98357A。
//
// 设计要点：
//   - 解码跑在独立任务（core 1），主循环只发指令、读状态，UI 不卡顿
//   - 用 AudioFileSourceBuffer 做二级缓冲，抵抗 WiFi 抖动导致的断音
//   - 播放位置由已解码样本数推算，比软件计时准，歌词滚动才跟得上
//   - 解码器按格式分支：MP3（libmad）与 FLAC（libFLAC）共用基类指针
class AudioPlayerClass {
public:
    // 播放源的编码格式。MusicService 选源时确定，随 URL 一起传给播放器。
    enum class PlayFormat : uint8_t {
        Unknown,   // 未知/未指定，openStream 按需探测或默认 MP3
        Mp3,
        Flac,
    };

    enum class State : uint8_t {
        Idle,        // 空闲
        Connecting,  // 正在建立 HTTP 连接与缓冲
        Playing,
        Paused,
        Failed,      // 连接或解码失败
    };

    void init();

    // 准备曲目但不出声：仅记下 URL，等 resume()/togglePause() 才真正拉流。
    // 用于开机或切歌后停在暂停态，避免设备一上电就自己响。
    void prepare(const String &url,
                 PlayFormat format = PlayFormat::Unknown);
    // 播放指定 URL（内网后端给出的直链）。会中断当前播放。
    void play(const String &url,
              PlayFormat format = PlayFormat::Unknown);
    void stop();
    void pause();
    void resume();
    void togglePause();

    State    state() const   { return _state; }
    bool     isPlaying() const { return _state == State::Playing; }
    // 已播放毫秒数，供歌词滚动与进度条使用
    uint32_t positionMs() const { return _positionMs; }
    // 音量 0–100
    uint8_t  volume() const  { return _volume; }
    void     setVolume(uint8_t percent);
    void     volumeUp();
    void     volumeDown();
    // 播放自然结束的一次性事件（读取后清除），供上层自动切下一首
    bool     takeFinishedEvent();

private:
    static void taskEntry(void *arg);
    void taskLoop();
    bool openStream(const String &url, PlayFormat format);
    void closeStream();

    TaskHandle_t _task = nullptr;
    SemaphoreHandle_t _lock = nullptr;

    volatile State    _state = State::Idle;
    volatile uint32_t _positionMs = 0;
    volatile bool     _finished = false;
    volatile bool     _stopRequest = false;
    volatile bool     _pauseRequest = false;
    volatile bool     _hasPending = false;   // 有新 URL 待播放
    volatile bool     _armed = false;        // 已备妥 URL 但等待用户按播放
    uint8_t  _volume = 45;                   // 默认音量，避免上电就吵
    volatile uint32_t _volDirtyMs = 0;       // 音量变更时刻（防抖持久化）

    String _pendingUrl;   // 受 _lock 保护
    String _armedUrl;     // 受 _lock 保护：备妥待播的 URL
    PlayFormat _pendingFormat = PlayFormat::Unknown;  // 受 _lock 保护
    PlayFormat _armedFormat   = PlayFormat::Unknown;  // 受 _lock 保护
};

extern AudioPlayerClass AudioPlayer;

#endif // AUDIOPLAYER_H
