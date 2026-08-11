#pragma once
#ifndef INPUT_H
#define INPUT_H

#include <Arduino.h>

// 模拟摇杆：VCC 必须接 3.3V，VRX/VRY 不可向 ESP32-S3 ADC 输入 5V。
// GPIO6/GPIO7 属于 ADC1，启用 WiFi 后仍可稳定读取。
// 三根信号线即可完成上下左右 + 按下五向操作。
#ifndef JOYSTICK_VRX_PIN
#define JOYSTICK_VRX_PIN 6
#endif
#ifndef JOYSTICK_VRY_PIN
#define JOYSTICK_VRY_PIN 7
#endif
#ifndef JOYSTICK_SW_PIN
#define JOYSTICK_SW_PIN 8
#endif

class InputClass {
public:
    void init();
    void update();

private:
    // N16R8：GPIO26–37 被 Flash/PSRAM 占用；38–40 已分配给 I2S 功放
    static constexpr uint8_t PIN_PLAY     = 13;
    static constexpr uint8_t PIN_VOL_UP   = 14;
    static constexpr uint8_t PIN_VOL_DOWN = 21;

    struct Btn {
        uint8_t  pin;
        uint8_t  state;       // 消抖后的稳定电平（HIGH = 释放）
        uint8_t  triggered;   // 本次按下是否已触发
        uint32_t ms;          // 上次状态变化时间
        uint32_t repeatMs;    // 长按连发的下次触发时刻（0 = 未进入连发）
    };

    Btn _play;
    Btn _volUp;
    Btn _volDown;
    Btn _joyPress;
    uint32_t _comboMs = 0;
    bool _comboActive = false;
    bool _comboTriggered = false;
    int8_t _joyDirection = 0;
    uint32_t _joyRepeatMs = 0;   // 摇杆推住不放时的连发计时

    // 长按连发：首次延迟后按固定间隔重复触发，便于快速翻列表/调音量
    static constexpr uint32_t kDebounceMs    = 20;
    static constexpr uint32_t kRepeatDelayMs = 450;
    static constexpr uint32_t kRepeatRateMs  = 140;
    // 播放键长按切换连续语音助手的阈值
    static constexpr uint32_t kVoiceHoldMs   = 800;
    bool _playHolding = false;
    uint32_t _playHoldMs = 0;
    bool _voiceStarted = false;
    bool _playWakeOnly = false;   // 按下播放键时背光本就熄灭（仅唤醒）
};

extern InputClass Input;

#endif // INPUT_H
