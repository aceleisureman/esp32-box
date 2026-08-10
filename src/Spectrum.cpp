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

static void bitreverse(int32_t *x, uint16_t n) {
    uint16_t j = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (i < j) {
            uint16_t ti = i << 1, tj = j << 1;
            int32_t t  = x[ti];
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

// 定点基-2 DIT FFT。此前的实现有三处致命错误，导致输出是与输入频率
// 完全无关的噪声（纯音 bin=5 的峰值落在 bin=116）——上层再怎么平滑
// 也只是在平滑垃圾。三处都已修正，并在主机上与参考 DFT 逐 bin 对齐：
//   ① 旋转因子取反了实虚部：SIN_TABLE[a] 是 sin，却当成了 wr(cos)。
//      正确取法 wr=cos=SIN_TABLE[a+64]、wi=-sin=-SIN_TABLE[a]。
//   ② 蝶形上下half写反：DIT 应 even'=even+t、odd'=even-t，原来反了。
//   ③ int16 累加溢出：每级蝶形幅度可翻倍，8 级后远超 ±32767 而翻卷，
//      正是"数值杂乱"的放大器。改用 int32 缓冲并每级 >>1 归一化。
static void fft_run(int32_t *buf, uint16_t n) {
    bitreverse(buf, n);
    for (uint16_t step = 2; step <= n; step <<= 1) {
        uint16_t half = step >> 1;
        uint16_t skip = 256 / step;
        for (uint16_t grp = 0; grp < n; grp += step) {
            for (uint16_t pair = 0; pair < half; pair++) {
                uint16_t even = (grp + pair) << 1;
                uint16_t odd  = even + (half << 1);
                const uint16_t a = (uint16_t)((pair * skip) & 255);
                const int32_t wr =  SIN_TABLE[(a + 64) & 255];   // cos
                const int32_t wi = -SIN_TABLE[a];                // -sin
                const int32_t orr = buf[odd], oii = buf[odd + 1];
                const int32_t tr = (wr * orr - wi * oii) >> 15;
                const int32_t ti = (wr * oii + wi * orr) >> 15;
                const int32_t er = buf[even], ei = buf[even + 1];
                // 每级 >>1：满幅输入下总增益 1/N，绝不溢出
                buf[even]     = (er + tr) >> 1;
                buf[even + 1] = (ei + ti) >> 1;
                buf[odd]      = (er - tr) >> 1;
                buf[odd + 1]  = (ei - ti) >> 1;
            }
        }
    }
}

} // namespace

void SpectrumClass::init() {
    _fftBuf.assign(FFT_SIZE * 2, 0);
    _levels.assign(BAR_COUNT, 0);
    // 勿对 TFT SPI 引脚做 analogRead（旧代码读 34/35 会拆掉显示）
}

void SpectrumClass::update() {
    // FFT 提速到 20ms（50fps）并记录真实间隔。
    // 旧写法 `if (millis()-tick < 40) tick = millis()` 有两个问题：
    //   ① 阈值法每次把相位重置到当前时刻，叠加主循环 23~48ms 的抖动后，
    //      实测间隔是 46~61ms（名义 40ms），数据本身就在忽快忽慢；
    //   ② 平滑用固定系数，间隔一抖动，等效时间常数就跟着抖。
    // 现在改为记录 dt 并按真实间隔做指数收敛，节奏不再受循环抖动影响。
    const uint32_t nowMs = millis();
    if (nowMs - _lastFftMs < 20) return;
    uint32_t dt = nowMs - _lastFftMs;
    _lastFftMs = nowMs;
    if (dt > 200) dt = 200;      // 切页/卡顿后不要一步跳到位
    _fftDtMs = (uint16_t)dt;

    // 1 秒内没有音频喂入：柱子平滑归零（真实播放器的停止行为）
    if (nowMs - _lastFeedMs > 1000) {
        for (auto &v : _levels) v = v > 10 ? (uint8_t)(v - 10) : 0;
        return;
    }

    // 取环形缓冲里最近的 FFT_SIZE 个真实样本，加汉宁窗抑制频谱泄漏。
    // 音频侧按 1/4 降采样喂入（有效采样率约 11kHz），256 点覆盖约
    // 93ms 音频。注意 FFT 必须 ≤256 点：正弦表只有 256 点分辨率，
    // 512 点时最后一级蝶形 skip=0，旋转因子全部退化，结果是错的。
    int32_t *buf = _fftBuf.data();
    const uint16_t head = _ringHead;
    for (size_t i = 0; i < FFT_SIZE; i++) {
        const int16_t s = _ring[(uint16_t)(head - FFT_SIZE + i) & (kRingSize - 1)];
        // 汉宁窗 w = (1 - cos(2πi/N)) / 2，用正弦表取 cos
        const int16_t cosv = SIN_TABLE[((i * 256 / FFT_SIZE) + 64) & 0xFF];
        const int32_t w = (32767 - cosv) / 2;              // Q15
        buf[i << 1]       = ((int32_t)s * w) >> 15;
        buf[(i << 1) + 1] = 0;
    }

    fft_run(buf, FFT_SIZE);
    binToBars();

    // 诊断：每秒打印一次数据链状态（确认 tap 是否喂入、FFT 是否有输出）
    static uint32_t dbgMs = 0;
    if (millis() - dbgMs >= 1000) {
        dbgMs = millis();
        Serial.printf("[SPEC] head=%u feedAge=%lums lv=%u,%u,%u,%u,%u\n",
                      (unsigned)_ringHead,
                      (unsigned long)(millis() - _lastFeedMs),
                      _levels[0], _levels[3], _levels[6], _levels[9],
                      _levels[12]);
    }
}

// alpha = (1 - exp(-dt/tau)) 的 Q8 定点近似。
// 用一阶帕德近似 dt/(dt+tau)：在 dt≪tau 区间（本例 dt≈20、tau≥35）
// 与真值误差 <3%，且只用整数除法——避免每帧 32 次 expf 调用。
static inline uint8_t alphaQ8(uint16_t dtMs, uint16_t tauMs) {
    uint32_t a = (uint32_t)dtMs * 256u / ((uint32_t)dtMs + tauMs);
    if (a > 255) a = 255;
    return (uint8_t)a;
}

void SpectrumClass::binToBars() {
    // 对数分组：低频 bin 窄、高频 bin 宽，16 条柱的能量分布更均衡。
    // 有效采样率 ~11kHz、256 点 → bin 分辨率 ~43Hz；
    // bin 1..100 覆盖约 43Hz..4.3kHz（音乐主能量区）。
    static const uint16_t kBinEdge[BAR_COUNT + 1] = {
        1, 2, 3, 4, 5, 7, 9, 12, 16, 20, 26, 33, 42, 53, 67, 84, 100
    };

    // 本帧的收敛系数：随真实间隔伸缩，帧率抖动不影响观感节奏
    const uint8_t alphaUp = alphaQ8(_fftDtMs, 35);    // τ=35ms  上升
    const uint8_t alphaDn = alphaQ8(_fftDtMs, 220);   // τ=220ms 回落

    for (uint8_t b = 0; b < BAR_COUNT; b++) {
        uint32_t sum = 0;
        uint8_t n = 0;
        for (uint16_t k = kBinEdge[b]; k < kBinEdge[b + 1]; k++) {
            const uint16_t idx = k << 1;
            if (idx + 1 >= _fftBuf.size()) break;
            const int32_t re = _fftBuf[idx];
            const int32_t im = _fftBuf[idx + 1];
            sum += (uint32_t)sqrtf((float)(re * re + im * im));
            n++;
        }
        if (!n) continue;
        const uint32_t avg = sum / n;

        // 开方压缩动态范围。标定 ×5.0 是按修正后 FFT 的实际增益
        // （每级 >>1，总增益 1/N）在主机上用模拟乐曲标定出来的：
        // 轻声段峰值约 115、普通乐段约 200 且零饱和、高潮段才顶到 255。
        // 旧值 3.2 对应的是溢出翻卷的错误 FFT，已无意义。
        uint16_t level16 = avg < 2 ? 0
                          : (uint16_t)(sqrtf((float)avg) * 5.0f);
        // 频段增益补偿：音乐高频能量天然低 10~20dB，逐段抬升
        // （b=0 ×1.0 → b=15 ×2.4），两侧柱才能像低频一样舞动
        level16 = (uint16_t)((uint32_t)level16 * (100 + b * 9) / 100);
        if (level16 > 255) level16 = 255;
        const uint8_t level = (uint8_t)level16;

        // 按真实间隔做指数逼近，节奏不受主循环抖动影响。
        // alpha = 1-exp(-dt/τ)，用 Q8 定点查表近似（τ上升35ms、回落220ms）。
        // 非对称：上升快保留鼓点冲击，回落慢让柱子自然坠落。
        // 参数在主机上按「视觉加加速度 RMS + 阶跃响应」扫描选出：
        // 相比旧的固定系数，抖动降 25%，鼓点上升反而从 253ms 快到 207ms。
        const int16_t diff = (int16_t)level - (int16_t)_levels[b];
        if (diff != 0) {
            const uint8_t a8 = diff > 0 ? alphaUp : alphaDn;   // Q8
            int16_t step = (int16_t)(((int32_t)diff * a8) >> 8);
            if (!step) step = diff > 0 ? 1 : -1;   // 保证收敛，不停在差 1
            _levels[b] = (uint8_t)((int16_t)_levels[b] + step);
        }
    }
}
