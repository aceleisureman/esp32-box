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

private:
    void binToBars();

    std::vector<int16_t> _fftBuf;   // interleaved real/imag, size = 2*FFT_SIZE
    std::vector<uint8_t> _levels;   // 16 bar levels
    uint8_t _phase = 0;
    static constexpr size_t FFT_SIZE = 512;
    static constexpr size_t BAR_COUNT = 16;
    static constexpr int32_t SAMPLE_RATE = 44100;
};

extern SpectrumClass Spectrum;

#endif // SPECTRUM_H
