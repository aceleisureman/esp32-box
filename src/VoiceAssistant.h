#pragma once

#include <Arduino.h>

class VoiceAssistantClass {
public:
    enum class State : uint8_t {
        Disabled, Disconnected, Connecting, Listening,
        UserSpeaking, Waiting, Playing, Error
    };

    void init();
    void update();
    void setEnabled(bool enabled);
    void toggleEnabled();
    void startRecording() { setEnabled(true); }
    void stopRecording() {}
    // 强制关闭会话并恢复音乐（供长按播放键退出对话时调用）
    void disableAndRestoreMusic();

    bool enabled() const { return _enabled; }
    bool takeMusicPlayRequest() {
        const bool requested = _musicPlayRequested;
        _musicPlayRequested = false;
        return requested;
    }
    bool isRecording() const {
        return _state == State::Listening || _state == State::UserSpeaking;
    }
    bool isBusy() const { return _state == State::Waiting || _state == State::Playing; }
    State state() const { return _state; }
    // 用户最近一次语音转文字内容（供唤醒页实时显示），无则返回 nullptr
    const char *lastUserText() const {
        return _userText.length() ? _userText.c_str() : nullptr;
    }

private:
    enum class PendingCommand : uint8_t {
        None, Play, Pause, Next, Previous, VolumeUp, VolumeDown
    };
    static void taskEntry(void *arg);
    static void onBinary(const uint8_t *data, size_t len);
    static void onText(const char *data, size_t len);
    void taskLoop();
    bool beginMicI2s();
    void endMicI2s();
    void handleText(const char *data, size_t len);
    void applyCommand(const char *action, const char *song = nullptr);

    TaskHandle_t _task = nullptr;
    SemaphoreHandle_t _micLock = nullptr;
    volatile State _state = State::Disabled;
    volatile bool _enabled = false;
    volatile bool _micActive = false;
    volatile bool _audioStarted = false;
    volatile bool _audioDone = false;
    volatile bool _sessionReady = false;
    volatile bool _musicPlayRequested = false;
    volatile PendingCommand _pendingCommand = PendingCommand::None;
    bool _wasMusicPlaying = false;   // 进入会话前是否在播音乐（会话结束恢复用）
    volatile bool _greeted = false;  // 本轮会话已响过就绪提示音
    String _userText;                // 用户最近一次语音转文字（唤醒页显示用）
};

extern VoiceAssistantClass VoiceAssistant;
