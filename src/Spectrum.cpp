#include "Spectrum.h"
#include <math.h>

SpectrumClass Spectrum;

namespace {

// 预计算正弦表（256 点，Q15 定点）
static const int16_t SIN_TABLE[256] = {
    #define S(n) int16_t(sin(2 * M_PI * n / 256.0) * 32767)
    S(  0), S(  1), S(  2), S(  3), S(  4), S(  5), S(  6), S(  7),
    S(  8), S(  9), S( 10), S( 11), S( 12), S( 13), S( 14), S( 15),
    S( 16), S( 17), S( 18), S( 19), S( 20), S( 21), S( 22), S( 23),
    S( 24), S( 25), S( 26), S( 27), S( 28), S( 29), S( 30), S( 31),
    S( 32), S( 33), S( 34), S( 35), S( 36), S( 37), S( 38), S( 39),
    S( 40), S( 41), S( 42), S( 43), S( 44), S( 45), S( 46), S( 47),
    S( 48), S( 49), S( 50), S( 51), S( 52), S( 53), S( 54), S( 55),
    S( 56), S( 57), S( 58), S( 59), S( 60), S( 61), S( 62), S( 63),
    S( 64), S( 65), S( 66), S( 67), S( 68), S( 69), S( 70), S( 71),
    S( 72), S( 73), S( 74), S( 75), S( 76), S( 77), S( 78), S( 79),
    S( 80), S( 81), S( 82), S( 83), S( 84), S( 85), S( 86), S( 87),
    S( 88), S( 89), S( 90), S( 91), S( 92), S( 93), S( 94), S( 95),
    S( 96), S( 97), S( 98), S( 99), S(100), S(101), S(102), S(103),
    S(104), S(105), S(106), S(107), S(108), S(109), S(110), S(111),
    S(112), S(113), S(114), S(115), S(116), S(117), S(118), S(119),
    S(120), S(121), S(122), S(123), S(124), S(125), S(126), S(127),
    S(128), S(129), S(130), S(131), S(132), S(133), S(134), S(135),
    S(136), S(137), S(138), S(139), S(140), S(141), S(142), S(143),
    S(144), S(145), S(146), S(147), S(148), S(149), S(150), S(151),
    S(152), S(153), S(154), S(155), S(156), S(157), S(158), S(159),
    S(160), S(161), S(162), S(163), S(164), S(165), S(166), S(167),
    S(168), S(169), S(170), S(171), S(172), S(173), S(174), S(175),
    S(176), S(177), S(178), S(179), S(180), S(181), S(182), S(183),
    S(184), S(185), S(186), S(187), S(188), S(189), S(190), S(191),
    S(192), S(193), S(194), S(195), S(196), S(197), S(198), S(199),
    S(200), S(201), S(202), S(203), S(204), S(205), S(206), S(207),
    S(208), S(209), S(210), S(211), S(212), S(213), S(214), S(215),
    S(216), S(217), S(218), S(219), S(220), S(221), S(222), S(223),
    S(224), S(225), S(226), S(227), S(228), S(229), S(230), S(231),
    S(232), S(233), S(234), S(235), S(236), S(237), S(238), S(239),
    S(240), S(241), S(242), S(243), S(244), S(245), S(246), S(247),
    S(248), S(249), S(250), S(251), S(252), S(253), S(254), S(255)
    #undef S
};

static void bitreverse(int16_t *x, uint16_t n) {
    uint16_t j = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (i < j) {
            uint16_t ti = i << 1, tj = j << 1;
            int16_t t  = x[ti];
            x[ti] = x[tj];
            x[tj] = t;
            t = x[ti + 1];
            x[ti + 1] = x[tj + 1];
            x[tj + 1] = t;
        }
        uint16_t m = n >> 1;
        while (m && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }
}

static void fft_run(int16_t *buf, uint16_t n) {
    bitreverse(buf, n);
    for (uint16_t step = 2; step <= n; step <<= 1) {
        uint16_t half = step >> 1;
        uint16_t skip = 256 / step;
        for (uint16_t grp = 0; grp < n; grp += step) {
            for (uint16_t pair = 0; pair < half; pair++) {
                uint16_t even = (grp + pair) << 1;
                uint16_t odd  = even + (half << 1);
                if (odd >= (n << 1)) break;
                int16_t wr = SIN_TABLE[(pair * skip) % 256];
                int16_t wi = SIN_TABLE[((pair * skip) + 64) % 256];
                int32_t tr = ((int32_t)wr * buf[odd]     - (int32_t)wi * buf[odd + 1]) >> 15;
                int32_t ti = ((int32_t)wr * buf[odd + 1] + (int32_t)wi * buf[odd])     >> 15;
                int32_t er = buf[even];
                int32_t ei = buf[even + 1];
                buf[even]     = (int16_t)(er - tr);
                buf[even + 1] = (int16_t)(ei - ti);
                buf[odd]      = (int16_t)(er + tr);
                buf[odd + 1]  = (int16_t)(ei + ti);
            }
        }
    }
}

} // namespace

void SpectrumClass::init() {
    _fftBuf.assign(FFT_SIZE * 2, 0);
    _levels.assign(BAR_COUNT, 0);
    _phase = 0;
    // 勿对 TFT SPI 引脚做 analogRead（旧代码读 34/35 会拆掉显示）
}

void SpectrumClass::update() {
    static uint32_t tick = 0;
    if (millis() - tick < 100) return;  // 与显示帧率对齐，降低刷新尖峰
    tick = millis();

    // 演示波形：无真实音频时仍有可见频谱柱
    // 日后接 I2S DMA 时，用采样填 _fftBuf 即可
    int16_t *buf = _fftBuf.data();
    _phase = (_phase + 3) & 0xFF;
    for (size_t i = 0; i < FFT_SIZE; i++) {
        int16_t s1 = SIN_TABLE[(_phase + i) & 0xFF];
        int16_t s2 = SIN_TABLE[((_phase * 2) + i * 3) & 0xFF];
        int16_t s3 = SIN_TABLE[((_phase * 5) + i * 7) & 0xFF] / 3;
        int16_t mixed = (int16_t)((s1 / 2) + (s2 / 4) + s3);
        buf[i << 1]     = mixed;
        buf[(i << 1) + 1] = 0;
    }

    fft_run(buf, FFT_SIZE);
    binToBars();
}

void SpectrumClass::binToBars() {
    for (uint8_t b = 0; b < BAR_COUNT; b++) {
        uint32_t sum = 0;
        uint16_t base = 2 + b * 3;
        for (uint8_t k = 0; k < 3; k++) {
            uint16_t idx = (base + k) << 1;
            if (idx + 1 >= _fftBuf.size()) break;
            int16_t re = _fftBuf[idx];
            int16_t im = _fftBuf[idx + 1];
            uint32_t mag = (uint32_t)sqrt((float)((int32_t)re * re + (int32_t)im * im));
            sum += mag;
        }
        uint16_t level16 = (uint16_t)(sum >> 6);
        if (level16 > 255) level16 = 255;
        uint8_t level = (uint8_t)level16;

        // 保证演示时柱子不完全贴地
        if (level < 20) level = (uint8_t)(20 + (b * 7 + _phase) % 40);

        if (level > _levels[b]) _levels[b] = level;
        else if (_levels[b] > 8) _levels[b] -= 8;
        else _levels[b] = level;
    }
}
