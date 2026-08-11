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
    void cancelPending();
    void pause();
    void resume();
    void togglePause();
    // 播放语音助手的 TTS 回复音频。与普通播放不同：结束后不置 finished
    // 事件（避免主循环误以为音乐播完而自动切下一首），
    // 播放前会暂停当前音乐，播完不自动恢复（由调用方决定）。
    void playPrompt(const String &url);
    // 播放本地合成的提示音（"叮"，约 200ms，无网络依赖）。
    // 用于语音助手开始/结束录音的听觉反馈。
    void playBeep();
    bool beginPcmPrompt(uint32_t sampleRate = 16000);
    size_t writePcm(const uint8_t *data, size_t len);
    void endPcmPrompt();
    void abortPcmPrompt();
    bool isPcmPromptDrained() const;
    size_t pcmBufferedBytes() const;

    State    state() const   { return _state; }
    bool     isPlaying() const { return _state == State::Playing; }
    // TTS 提示音正在播放（主循环据此暂缓自动切歌/自动续播）
    bool     isPromptActive() const { return _promptMode; }
    // 已播放毫秒数，供歌词滚动与进度条使用
    uint32_t positionMs() const { return _positionMs; }
    // 音量 0–100
    uint8_t  volume() const  { return _volume; }
    void     setVolume(uint8_t percent);
    // 音量渐变（非阻塞）：设定目标后立即返回，由音频任务逐步逼近。
    // 调用方多在 UI 线程（按键/摇杆），阻塞会让界面卡顿，故不能同步等待。
    void     fadeVolume(uint8_t target, uint16_t durationMs);
    // 淡出到静音后暂停；会话结束用 fadeInResume 还原（均非阻塞）
    void     fadeOutPause(uint16_t durationMs = 300);
    void     fadeInResume(uint16_t durationMs = 400);
    void     volumeUp();
    void     volumeDown();
    // 播放自然结束的一次性事件（读取后清除），供上层自动切下一首
    bool     takeFinishedEvent();

private:
    static void taskEntry(void *arg);
    void taskLoop();
    bool openStream(const String &url, PlayFormat format);
    bool openBeepStream();
    void closeStream();
    // 推进一步音量渐变（音频任务每轮调用；无渐变时立即返回）
    void serviceFade();
    // 只改输出增益，不触碰 _volDirtyMs（渐变中间值不该写进 NVS）
    void applyGain(uint8_t percent);

    TaskHandle_t _task = nullptr;
    SemaphoreHandle_t _lock = nullptr;

    volatile State    _state = State::Idle;
    volatile uint32_t _positionMs = 0;
    volatile bool     _finished = false;
    volatile bool     _stopRequest = false;
    volatile bool     _pauseRequest = false;
    volatile bool     _hasPending = false;   // 有新 URL 待播放
    volatile bool     _beepRequest = false;  // 请求播放本地提示音
    volatile bool     _armed = false;        // 已备妥 URL 但等待用户按播放
    volatile bool     _promptMode = false;   // 当前播放的是 TTS 提示音
    volatile bool     _pcmMode = false;
    volatile bool     _pcmEndRequested = false;
    uint8_t  _volume = 45;                   // 默认音量，避免上电就吵
    uint8_t  _gainPercent = 45;              // 当前输出增益，可被临时渐变修改
    uint8_t  _fadeSavedVolume = 45;          // 淡出前的音量（淡入时还原）
    // 非阻塞渐变状态：由音频任务在 taskLoop 里推进
    volatile uint8_t  _fadeTarget = 0;       // 目标音量
    volatile uint8_t  _fadeFrom = 0;         // 起始音量
    volatile uint32_t _fadeStartMs = 0;      // 渐变开始时刻（0=未在渐变）
    volatile uint16_t _fadeDurationMs = 0;
    volatile bool     _fadePauseAtEnd = false;   // 渐变结束后暂停播放
    volatile uint32_t _volDirtyMs = 0;       // 音量变更时刻（防抖持久化）

    String _pendingUrl;   // 受 _lock 保护
    String _armedUrl;     // 受 _lock 保护：备妥待播的 URL
    PlayFormat _pendingFormat = PlayFormat::Unknown;  // 受 _lock 保护
    PlayFormat _armedFormat   = PlayFormat::Unknown;  // 受 _lock 保护
};

extern AudioPlayerClass AudioPlayer;

#endif // AUDIOPLAYER_H
