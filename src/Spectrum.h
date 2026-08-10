#pragma once
#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <Arduino.h>
#include <vector>

class SpectrumClass {
public:
    void init();
    void update();
    const std::vector<uint8_t>& getLevels() const { return _levels; }

    // 音频解码任务逐样本喂入（已混为单声道、按 1/4 降采样）。
    // 热路径：单写（音频任务）单读（UI 任务）的无锁环形，
    // 个别样本撕裂对可视化毫无影响，不加锁。
    inline void feedSample(int16_t mono) {
        _ring[_ringHead & (kRingSize - 1)] = mono;
        _ringHead++;
        // 时间戳低频更新即可，避免每样本读 tick
        if ((_ringHead & 63) == 0) _lastFeedMs = millis();
    }

private:
    void binToBars();

    static constexpr size_t FFT_SIZE = 256;   // 与 256 点正弦表分辨率匹配
    static constexpr size_t BAR_COUNT = 16;
    static constexpr uint16_t kRingSize = 1024;   // 2^n，须 ≥ FFT_SIZE

    std::vector<int32_t> _fftBuf;   // interleaved real/imag, size = 2*FFT_SIZE
                                    // int32：定点蝶形每级幅度可翻倍，
                                    // int16 会溢出翻卷成噪声
    std::vector<uint8_t> _levels;   // 16 bar levels
    int16_t  _ring[kRingSize] = {};
    volatile uint16_t _ringHead = 0;
    volatile uint32_t _lastFeedMs = 0;
    uint32_t _lastFftMs = 0;    // 上次 FFT 时刻
    uint16_t _fftDtMs   = 20;   // 上次 FFT 的真实间隔，供 dt 感知平滑用
};

extern SpectrumClass Spectrum;

#endif // SPECTRUM_H
