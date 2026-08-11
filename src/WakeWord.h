#pragma once

#include <Arduino.h>

class WakeWordClass {
public:
    void init();
    void update();
    bool enabled() const { return _enabled; }

private:
    static void taskEntry(void *arg);
    void taskLoop();
    bool beginMic(int frameSamples);
    void endMic();
    TaskHandle_t _task = nullptr;
    SemaphoreHandle_t _lock = nullptr;
    volatile bool _enabled = false;
    volatile bool _micActive = false;
};

extern WakeWordClass WakeWord;
