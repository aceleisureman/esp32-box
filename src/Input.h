#pragma once
#ifndef INPUT_H
#define INPUT_H

#include <Arduino.h>

// 模拟摇杆：VCC 必须接 3.3V，VRX/VRY 不可向 ESP32-S3 ADC 输入 5V。
// GPIO6/GPIO7 属于 ADC1，启用 WiFi 后仍可稳定读取。
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
    // N16R8：GPIO26–37 被 Flash/PSRAM 占用，勿用 27
    static constexpr uint8_t PIN_PLAY     = 13;
    static constexpr uint8_t PIN_VOL_UP   = 14;
    static constexpr uint8_t PIN_VOL_DOWN = 21;

    struct Btn {
        uint8_t pin;
        uint8_t state;       // 消抖后的稳定电平（HIGH = 释放）
        uint8_t triggered;   // 本次按下是否已触发
        uint32_t ms;         // 上次状态变化时间
    };

    Btn _play;
    Btn _volUp;
    Btn _volDown;
    Btn _joyPress;
    uint32_t _comboMs = 0;
    bool _comboActive = false;
    bool _comboTriggered = false;
    int8_t _joyDirection = 0;
};

extern InputClass Input;

#endif // INPUT_H
