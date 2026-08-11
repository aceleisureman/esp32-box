#include "AudioPlayer.h"
#include "pins_audio.h"
#include "Spectrum.h"

#include <Preferences.h>

#include <AudioFileSourceHTTPStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorFLAC.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <driver/i2s.h>

AudioPlayerClass AudioPlayer;

namespace {

// 二级缓冲：WiFi / 公网隧道抖动时靠它续上。当前播 higher/exhigh MP3
// （192k≈24KB/s、320k≈40KB/s）。256KB 对 320k 可缓冲约 6 秒，足以吸收
// 内网与 cpolar 隧道的中短时抖动；PSRAM 充足，加大无副作用。
constexpr int kStreamBufferBytes = 256 * 1024;

// DMA 缓冲：块数越多越抗断流，但延迟略增。16×512 样本约 185ms @44.1k
constexpr int kDmaBufCount = 16;

// I2S 输出 + 频谱分流 + 音频时钟。
// 每个解码样本在送入 DMA 前混单声道喂给 Spectrum 的 FFT（1/4 降采样，
// 开销可忽略）。拿到的是未加增益的原始解码样本——频谱不随音量大小
// 变化，这是正确行为。
//
// 同时在这里统计已消费样本数：这是唯一精确的音频时间轴。用墙钟
// （millis()-startedMs）推算播放位置会在缓冲欠载、网络卡顿、解码
// 被抢占时继续走，而声音并没有走 —— 歌词就会越跑越靠前。
class SpectrumTapI2S : public AudioOutputI2S {
public:
    using AudioOutputI2S::AudioOutputI2S;

    bool ConsumeSample(int16_t sample[2]) override {
        // DMA 满时基类返回 false，样本未被接收，解码器稍后会重投同一个
        // 样本。此时既不能计时间也不能喂频谱，否则时间轴会越走越快。
        if (!AudioOutputI2S::ConsumeSample(sample)) return false;

        if ((++_decim & 0x03) == 0) {
            Spectrum.feedSample(
                (int16_t)(((int32_t)sample[0] + sample[1]) / 2));
        }
        _samples++;   // 单写单读，UI 侧容忍个别读撕裂
        return true;
    }

    // 已送入 DMA 的样本数（按采样率换算即播放位置）
    uint32_t samplesConsumed() const { return _samples; }
    void resetSamples() { _samples = 0; }
    // 解码器通过 SetRate 告知真实采样率（MP3 可能是 44100/48000/22050）
    uint16_t sampleRate() const { return hertz ? hertz : 44100; }

    // 停止播放时不卸载 I2S 驱动（基类 stop 的行为），只清零 DMA。
    // 卸载后 BCLK/LRC/DIN 悬空，功放输入变天线，待机时拾取 WiFi/
    // 数字噪声出杂音；保持时钟运行 + 全零数据才是干净的静音。
    bool stop() override {
        i2s_zero_dma_buffer((i2s_port_t)portNo);
        return true;
    }

private:
    uint8_t  _decim = 0;
    volatile uint32_t _samples = 0;
};

AudioGenerator             *gDec    = nullptr;   // MP3 / FLAC 共用基类指针
AudioFileSourceHTTPStream  *gStream = nullptr;
AudioFileSourceBuffer      *gBuffer = nullptr;
AudioOutputI2S             *gOut    = nullptr;
// 流缓冲放 PSRAM：从内部 SRAM 抠会加剧堆压力与碎片
uint8_t                    *gBufMem = nullptr;

// 本地提示音（"叮"）：init 时在 PSRAM 合成一次，后续反复用。
// 一个带指数衰减包络的 880Hz 正弦，约 200ms，16kHz 16bit 单声道 WAV。
uint8_t                    *gBeepWav = nullptr;
uint32_t                    gBeepWavLen = 0;
// 提示音的内存源：与 gStream/gBuffer 独立，closeStream 一并释放
AudioFileSourcePROGMEM     *gBeepSource = nullptr;

// Realtime API 常会以高于播放速度下发 24kHz PCM。
// 512KB 可容纳约 10.9 秒单声道 PCM16，避免回复尚未播完就溢出中止。
constexpr size_t kPcmBufferBytes = 512 * 1024;
constexpr uint32_t kPcmPrebufferMs = 100;
uint8_t *gPcmBuffer = nullptr;
volatile size_t gPcmRead = 0;
volatile size_t gPcmWrite = 0;
volatile uint32_t gPcmGeneration = 0;
volatile uint32_t gPcmSampleRate = 24000;
volatile bool gPcmPrimed = false;
uint32_t gPcmBackpressure = 0;
uint32_t gPcmUnderflows = 0;
bool gPcmStarved = false;
portMUX_TYPE gPcmMux = portMUX_INITIALIZER_UNLOCKED;

size_t pcmUsed() {
    const size_t read = gPcmRead;
    const size_t write = gPcmWrite;
    return write >= read ? write - read : kPcmBufferBytes - read + write;
}

void buildBeepWav() {
    constexpr uint32_t kSampleRate = 16000;
    constexpr float    kFreq = 880.0f;
    constexpr float    kDurS = 0.20f;
    constexpr uint32_t kSamples = (uint32_t)(kSampleRate * kDurS);
    const uint32_t dataLen = kSamples * 2;
    const uint32_t wavLen = 44 + dataLen;
    uint8_t *wav = (uint8_t *)ps_malloc(wavLen);
    if (!wav) return;

    uint8_t *p = wav;
    auto put = [&p](uint32_t v, uint8_t n) {
        for (uint8_t i = 0; i < n; i++) *p++ = (uint8_t)(v >> (8 * i));
    };
    auto putStr = [&p](const char *s) {
        while (*s) *p++ = (uint8_t)*s++;
    };
    putStr("RIFF");      put(wavLen - 8, 4);
    putStr("WAVEfmt ");  put(16, 4); put(1, 2); put(1, 2);
    put(kSampleRate, 4); put(kSampleRate * 2, 4); put(2, 2); put(16, 2);
    putStr("data");      put(dataLen, 4);

    for (uint32_t i = 0; i < kSamples; i++) {
        const float t = (float)i / kSampleRate;
        const float env = expf(-5.0f * t / kDurS);      // 指数衰减
        const float v = sinf(2.0f * (float)M_PI * kFreq * t) * env * 0.8f;
        const int16_t s = (int16_t)(v * 32767.0f);
        *p++ = (uint8_t)(s & 0xff);
        *p++ = (uint8_t)((uint16_t)s >> 8);
    }
    gBeepWav = wav;
    gBeepWavLen = wavLen;
}

}  // namespace

void AudioPlayerClass::init() {
    if (_task) return;
    _lock = xSemaphoreCreateMutex();

    // 恢复上次保存的音量（默认 45%）
    {
        Preferences p;
        if (p.begin("audio", true)) {
            _volume = p.getUChar("vol", 45);
            if (_volume > 100) _volume = 100;
            p.end();
        }
    }

    // dma_buf_count 加大到 16：默认 8 块在 WiFi 忙时容易欠载出爆音
    gOut = new SpectrumTapI2S(I2S_PORT_NUM, AudioOutputI2S::EXTERNAL_I2S,
                              kDmaBufCount);
    gOut->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
    gOut->SetOutputModeMono(true);           // MAX98357A 是单声道
    gOut->SetBitsPerSample(16);
    _gainPercent = _volume;
    gOut->SetGain((float)_volume * 0.9f / 100.0f);
    // 开机即安装 I2S 驱动并终身常驻：引脚从上电起就被持续驱动
    // （时钟 + 零数据），待机不悬空、不拾噪。配合上面的 stop()
    // 覆写，任何时刻功放收到的都是合法 I2S 信号。
    gOut->begin();

    // 语音助手的提示音：PSRAM 里合成一次，反复可用
    buildBeepWav();

    // 解码任务放 core 0：Arduino loop（UI 刷新）固定跑在 core 1，
    // 分核可避免解码把 UI 挤掉。优先级取 2，高于网络任务但低于系统关键任务。
    // 10KB 栈：libFLAC 每帧解码调用链比 libmad 更深，留足余量。
    if (!_lock || xTaskCreatePinnedToCore(&AudioPlayerClass::taskEntry, "audio",
                                          10240, this, 2, &_task, 0) != pdPASS) {
        _task = nullptr;
        Serial.println("[AUDIO] task creation failed");
        return;
    }
    Serial.printf("[AUDIO] init: BCLK=%d LRC=%d DIN=%d\n",
                  PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
}

void AudioPlayerClass::prepare(const String &url,
                               PlayFormat format) {
    if (!url.length()) return;
    // 只记住 URL，不建立连接、不出声。等用户按播放键再真正开始。
    stop();
    xSemaphoreTake(_lock, portMAX_DELAY);
    _armedUrl = url;
    _armedFormat = format;
    xSemaphoreGive(_lock);
    _armed = true;
    _positionMs = 0;
    _state = State::Paused;
}

void AudioPlayerClass::play(const String &url,
                            PlayFormat format) {
    if (!url.length()) return;
    xSemaphoreTake(_lock, portMAX_DELAY);
    _pendingUrl = url;
    _pendingFormat = format;
    xSemaphoreGive(_lock);
    _armed = false;
    _hasPending = true;
    _stopRequest = false;
    _pauseRequest = false;
    _state = State::Connecting;
}

void AudioPlayerClass::stop() {
    _stopRequest = true;
}

void AudioPlayerClass::cancelPending() {
    _hasPending = false;
    _armed = false;
    _beepRequest = false;
    _stopRequest = true;
}

void AudioPlayerClass::pause() {
    if (_state == State::Playing) {
        _pauseRequest = true;
        _state = State::Paused;
    }
}

void AudioPlayerClass::resume() {
    // 备妥但尚未拉流：此刻才真正开始播放
    if (_armed) {
        String url;
        PlayFormat format;
        xSemaphoreTake(_lock, portMAX_DELAY);
        url = _armedUrl;
        format = _armedFormat;
        xSemaphoreGive(_lock);
        if (url.length()) {
            play(url, format);
            return;
        }
    }
    if (_state == State::Paused) {
        _pauseRequest = false;
        _state = State::Playing;
    }
}

void AudioPlayerClass::togglePause() {
    if (_state == State::Playing) pause();
    else resume();   // Paused / Idle / 备妥态都走 resume
}

void AudioPlayerClass::playPrompt(const String &url) {
    if (!url.length()) return;
    // TTS 提示音：先暂停当前音乐，用提示音占住播放链路。
    // 结束时（taskLoop 里 isRunning 变 false）不会置 _finished，
    // 主循环因此不会误触自动切歌。
    _promptMode = true;
    if (_state == State::Playing) pause();
    play(url, PlayFormat::Mp3);
}

void AudioPlayerClass::playBeep() {
    // 请求播放本地提示音：交给音频任务处理（要停当前流、建临时源）。
    _beepRequest = true;
}

bool AudioPlayerClass::beginPcmPrompt(uint32_t sampleRate) {
    if (!gPcmBuffer) gPcmBuffer = (uint8_t *)ps_malloc(kPcmBufferBytes);
    if (!gPcmBuffer || !gOut) return false;
    _fadeStartMs = 0;
    _fadePauseAtEnd = false;
    _pauseRequest = false;
    closeStream();
    portENTER_CRITICAL(&gPcmMux);
    gPcmRead = gPcmWrite = 0;
    gPcmGeneration++;
    gPcmSampleRate = sampleRate;
    gPcmPrimed = false;
    portEXIT_CRITICAL(&gPcmMux);
    gPcmBackpressure = 0;
    gPcmUnderflows = 0;
    gPcmStarved = false;
    gOut->SetRate(sampleRate);
    gOut->SetBitsPerSample(16);
    gOut->SetChannels(1);
    // 语音合成 PCM 的平均响度低于音乐，回复播放时单独提高增益。
    gOut->SetGain(0.82f);
    _pcmEndRequested = false;
    _pcmMode = true;
    _promptMode = true;
    _state = State::Playing;
    Serial.printf("[AUDIO] PCM playback started %luHz\n",
                  (unsigned long)sampleRate);
    return true;
}

size_t AudioPlayerClass::writePcm(const uint8_t *data, size_t len) {
    if (!_pcmMode || !data) return 0;
    len &= ~(size_t)1;
    portENTER_CRITICAL(&gPcmMux);
    const size_t used = pcmUsed();
    size_t written = min(len, kPcmBufferBytes - used - 1) & ~(size_t)1;
    const size_t first = min(written, kPcmBufferBytes - gPcmWrite);
    memcpy(gPcmBuffer + gPcmWrite, data, first);
    if (written > first) {
        memcpy(gPcmBuffer, data + first, written - first);
    }
    gPcmWrite = (gPcmWrite + written) % kPcmBufferBytes;
    portEXIT_CRITICAL(&gPcmMux);
    return written;
}

void AudioPlayerClass::endPcmPrompt() { _pcmEndRequested = true; }

void AudioPlayerClass::abortPcmPrompt() {
    _pcmMode = false;
    _pcmEndRequested = false;
    _promptMode = false;
    portENTER_CRITICAL(&gPcmMux);
    gPcmRead = gPcmWrite = 0;
    gPcmGeneration++;
    gPcmPrimed = false;
    portEXIT_CRITICAL(&gPcmMux);
    if (gOut) gOut->stop();
    _state = State::Idle;
}

size_t AudioPlayerClass::pcmBufferedBytes() const { return pcmUsed(); }

bool AudioPlayerClass::isPcmPromptDrained() const {
    return _pcmMode && _pcmEndRequested && pcmUsed() == 0;
}

// 用内存里的 WAV 提示音打开一条临时的播放流。
// 源是 PROGMEM 内存源，用独立全局 gBeepSource 持有（closeStream 释放），
// 不经过 gStream/gBuffer 两个 HTTP 全局；gDec 直接挂 WAV 解码器。
bool AudioPlayerClass::openBeepStream() {
    if (!gBeepWav || !gBeepWavLen) return false;
    _fadeStartMs = 0;
    _fadePauseAtEnd = false;
    _pauseRequest = false;
    closeStream();
    applyGain(_volume);

    gBeepSource = new AudioFileSourcePROGMEM();
    if (!gBeepSource->open(gBeepWav, gBeepWavLen)) {
        delete gBeepSource;
        gBeepSource = nullptr;
        return false;
    }

    AudioGeneratorWAV *wav = new AudioGeneratorWAV();
    if (!wav->begin(gBeepSource, gOut)) {
        delete wav;
        delete gBeepSource;
        gBeepSource = nullptr;
        return false;
    }
    gDec = wav;
    return true;
}

void AudioPlayerClass::setVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    _fadeStartMs = 0;
    _fadePauseAtEnd = false;
    _volume = percent;
    _gainPercent = percent;
    _volDirtyMs = millis();   // 防抖持久化：停止调节 2s 后落盘
    // 增益上限 0.9 而非 1.0：MP3 解码后的样本本就接近满幅，
    // 增益拉到 1.0 时响度峰值会削波，听感就是持续的破音/沙沙声。
    if (gOut) gOut->SetGain((float)_volume * 0.9f / 100.0f);
}

// 只改输出增益，不碰 _volDirtyMs：渐变的中间值不是用户设定，
// 不该写进 NVS（既伤 flash，也会让"恢复原音量"取到错误基准）。
void AudioPlayerClass::applyGain(uint8_t percent) {
    if (percent > 100) percent = 100;
    _gainPercent = percent;
    if (gOut) gOut->SetGain((float)_gainPercent * 0.9f / 100.0f);
}

// 非阻塞渐变：只记录起止与时长，实际推进在音频任务的 serviceFade()。
// 调用方多在 UI 线程，若在此阻塞等待会让界面卡住整个渐变时长。
void AudioPlayerClass::fadeVolume(uint8_t target, uint16_t durationMs) {
    if (target > 100) target = 100;
    if (durationMs == 0) {
        _fadeStartMs = 0;
        applyGain(target);
        return;
    }
    _fadeFrom = _gainPercent;
    _fadeTarget = target;
    _fadeDurationMs = durationMs;
    _fadeStartMs = millis();
    if (_fadeStartMs == 0) _fadeStartMs = 1;   // 0 表示"未在渐变"
}

void AudioPlayerClass::fadeOutPause(uint16_t durationMs) {
    if (_state != State::Playing) return;
    _fadeSavedVolume = _volume;      // 记住用户设定的音量，淡入时还原
    _fadePauseAtEnd = true;          // 渐变到 0 后由 serviceFade 暂停
    fadeVolume(0, durationMs);
}

void AudioPlayerClass::fadeInResume(uint16_t durationMs) {
    _fadePauseAtEnd = false;
    applyGain(0);                    // 静音起播，避免恢复瞬间的爆音
    resume();
    fadeVolume(_fadeSavedVolume, durationMs);
}

// 音频任务每轮推进渐变；未在渐变时是一次极廉价的判断。
void AudioPlayerClass::serviceFade() {
    if (_fadeStartMs == 0) return;
    const uint32_t elapsed = millis() - _fadeStartMs;
    if (elapsed >= _fadeDurationMs) {
        applyGain(_fadeTarget);
        _fadeStartMs = 0;
        if (_fadePauseAtEnd) {
            _fadePauseAtEnd = false;
            pause();                 // 淡出到静音后才真正暂停
        }
        return;
    }
    const int16_t diff = (int16_t)_fadeTarget - (int16_t)_fadeFrom;
    const int16_t v = (int16_t)_fadeFrom +
                      (int16_t)((int32_t)diff * (int32_t)elapsed /
                                (int32_t)_fadeDurationMs);
    applyGain((uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v)));
}

void AudioPlayerClass::volumeUp() {
    setVolume(_volume >= 95 ? 100 : (uint8_t)(_volume + 5));
}

void AudioPlayerClass::volumeDown() {
    setVolume(_volume <= 5 ? 0 : (uint8_t)(_volume - 5));
}

bool AudioPlayerClass::takeFinishedEvent() {
    if (!_finished) return false;
    _finished = false;
    return true;
}

bool AudioPlayerClass::openStream(const String &url, PlayFormat format) {
    closeStream();

    gStream = new AudioFileSourceHTTPStream();
    if (!gStream->open(url.c_str())) {
        Serial.printf("[AUDIO] stream open failed: %s\n", url.c_str());
        closeStream();
        return false;
    }
    // 缓冲区放 PSRAM（库支持外部预分配缓冲）；PSRAM 不可用再退回内部堆
    if (!gBufMem) gBufMem = (uint8_t *)ps_malloc(kStreamBufferBytes);
    gBuffer = gBufMem
        ? new AudioFileSourceBuffer(gStream, gBufMem, kStreamBufferBytes)
        : new AudioFileSourceBuffer(gStream, kStreamBufferBytes);

    // 按 MusicService 选定的格式实例化解码器（均继承 AudioGenerator，
    // 播放位置/暂停/频谱分流等逻辑共用基类接口）。
    switch (format) {
        case PlayFormat::Flac:
            gDec = new AudioGeneratorFLAC();
            if (!gDec->begin(gBuffer, gOut)) {
                Serial.println("[AUDIO] flac begin failed");
                closeStream();
                return false;
            }
            break;
        case PlayFormat::Unknown:
        case PlayFormat::Mp3:
        default:
            gDec = new AudioGeneratorMP3();
            if (!gDec->begin(gBuffer, gOut)) {
                Serial.println("[AUDIO] mp3 begin failed");
                closeStream();
                return false;
            }
            break;
    }

    _positionMs = 0;
    Serial.printf("[AUDIO] playing %s (buffer %dKB)\n",
                  format == PlayFormat::Flac ? "flac" : "mp3",
                  kStreamBufferBytes / 1024);
    return true;
}

void AudioPlayerClass::closeStream() {
    if (gDec) {
        if (gDec->isRunning()) gDec->stop();
        delete gDec;
        gDec = nullptr;
    }
    if (gBeepSource) { delete gBeepSource; gBeepSource = nullptr; }
    if (gBuffer) { delete gBuffer; gBuffer = nullptr; }
    if (gStream) { delete gStream; gStream = nullptr; }
}

void AudioPlayerClass::taskEntry(void *arg) {
    static_cast<AudioPlayerClass *>(arg)->taskLoop();
}

void AudioPlayerClass::taskLoop() {
    uint32_t lastYieldMs = millis();

    for (;;) {
        if (_pcmMode) {
            const size_t prebufferBytes =
                ((size_t)gPcmSampleRate * 2 * kPcmPrebufferMs) / 1000;
            if (!gPcmPrimed) {
                const size_t buffered = pcmUsed();
                if (buffered < prebufferBytes && !_pcmEndRequested) {
                    vTaskDelay(1);
                    continue;
                }
                gPcmPrimed = true;
                Serial.printf("[AUDIO] PCM primed %uB\n", (unsigned)buffered);
            }

            uint8_t bytes[2];
            bool haveSample = false;
            size_t expectedRead = 0;
            uint32_t generation = 0;
            portENTER_CRITICAL(&gPcmMux);
            if (pcmUsed() >= 2) {
                expectedRead = gPcmRead;
                generation = gPcmGeneration;
                bytes[0] = gPcmBuffer[expectedRead];
                bytes[1] = gPcmBuffer[(expectedRead + 1) % kPcmBufferBytes];
                haveSample = true;
            }
            portEXIT_CRITICAL(&gPcmMux);
            if (haveSample) {
                gPcmStarved = false;
                const int16_t mono = (int16_t)((uint16_t)bytes[0] |
                                               ((uint16_t)bytes[1] << 8));
                int16_t stereo[2] = {mono, mono};
                if (gOut->ConsumeSample(stereo)) {
                    portENTER_CRITICAL(&gPcmMux);
                    if (gPcmGeneration == generation && gPcmRead == expectedRead) {
                        gPcmRead = (expectedRead + 2) % kPcmBufferBytes;
                    }
                    portEXIT_CRITICAL(&gPcmMux);
                } else {
                    gPcmBackpressure++;
                    vTaskDelay(1);
                }
            } else if (_pcmEndRequested) {
                _pcmMode = false;
                _pcmEndRequested = false;
                _promptMode = false;
                gOut->stop();
                gOut->SetGain((float)_volume * 0.9f / 100.0f);
                _state = State::Idle;
                Serial.printf("[AUDIO] PCM playback complete retries=%lu underflows=%lu\n",
                              (unsigned long)gPcmBackpressure,
                              (unsigned long)gPcmUnderflows);
            } else {
                if (!gPcmStarved) {
                    gPcmStarved = true;
                    gPcmUnderflows++;
                }
                vTaskDelay(1);
            }
            continue;
        }

        // 音量防抖持久化：停止调节 2s 后写一次 NVS。
        // 连发调音量时不能每次都写：flash 擦写有寿命，且写入会
        // 短暂暂停 flash 缓存（一次性几 ms，64KB 音频缓冲可兜住）。
        if (_volDirtyMs && millis() - _volDirtyMs >= 2000) {
            _volDirtyMs = 0;
            Preferences p;
            if (p.begin("audio", false)) {
                p.putUChar("vol", _volume);
                p.end();
                Serial.printf("[AUDIO] volume saved: %u%%\n", _volume);
            }
        }

        // 语音提示音请求（优先级高于新曲目：录音反馈不能被切歌打断）
        if (_beepRequest) {
            _beepRequest = false;
            _promptMode = true;
            if (openBeepStream()) {
                if (gOut) static_cast<SpectrumTapI2S *>(gOut)->resetSamples();
                _positionMs = 0;
                _state = State::Playing;
            } else {
                _promptMode = false;
            }
            continue;
        }

        // 新曲目请求
        if (_hasPending) {
            _hasPending = false;
            String url;
            PlayFormat format;
            xSemaphoreTake(_lock, portMAX_DELAY);
            url = _pendingUrl;
            format = _pendingFormat;
            xSemaphoreGive(_lock);

            if (openStream(url, format)) {
                // 音频时钟归零：新曲目从 0 开始计样本
                if (gOut) static_cast<SpectrumTapI2S *>(gOut)->resetSamples();
                _positionMs = 0;
                _state = State::Playing;
            } else {
                _state = State::Failed;
            }
            continue;
        }

        if (_stopRequest) {
            _stopRequest = false;
            closeStream();
            _state = State::Idle;
            _positionMs = 0;
            continue;
        }

        if (!gDec || !gDec->isRunning()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 暂停：不解码即不消费样本，音频时钟自然停住，
        // 无需像墙钟那样累计补偿暂停时长
        if (_pauseRequest) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 解码一帧。loop() 返回 false 表示流结束或出错
        if (!gDec->loop()) {
            closeStream();
            _state = State::Idle;
            if (_promptMode) {
                // TTS 提示音结束：不产生 finished 事件，避免主循环
                // 误以为音乐播完而自动切歌。调用方自行决定后续。
                _promptMode = false;
                Serial.println("[AUDIO] prompt finished");
            } else {
                _finished = true;
                Serial.println("[AUDIO] track finished");
            }
            continue;
        }

        // 缓冲欠载（网络跟不上 / 歌尾流已读完）：解码器拿不到数据时
        // loop() 仍返回 true，只是空转不产出帧。此时继续全速轮询毫无
        // 意义——数据要靠网络任务填进来，抢着 CPU 反而拖慢它。
        // 主动睡一小会儿，把 core 0 让给网络与空闲任务。
        if (gBuffer && gBuffer->getFillLevel() == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            lastYieldMs = millis();
            continue;
        }

        // 播放位置取自 I2S 实际消费的样本数——这是真正的音频时间轴。
        // 墙钟会在缓冲欠载/网络卡顿时继续走而声音没走，歌词就会
        // 越跑越靠前，且一旦跑偏永不回正。样本计数不会漂。
        //
        // 减去 DMA 队列深度：计数是"已交给 DMA"，而喇叭要等队列排完
        // 才发声（16 块 ×128 样本 = 2048 样本，44.1k 下约 46ms）。
        // 不减的话歌词会稳定地早于人声半拍。
        if (gOut) {
            SpectrumTapI2S *tap = static_cast<SpectrumTapI2S *>(gOut);
            const uint32_t rate = tap->sampleRate();
            if (rate) {
                uint32_t s = tap->samplesConsumed();
                constexpr uint32_t kDmaQueueSamples = kDmaBufCount * 128;
                s = s > kDmaQueueSamples ? s - kDmaQueueSamples : 0;
                _positionMs = (uint32_t)((uint64_t)s * 1000ULL / rate);
            }
        }

        // 非阻塞音量渐变：音频任务每轮推进（约 20ms 一次，
        // 300~400ms 渐变有足够的分辨率），调用方无需等待。
        serviceFade();

        // 让出策略：按「距上次让出的时间」而非帧数。
        //
        // 帧计数的写法在歌尾会触发看门狗复位：流读完后 loop() 拿不到
        // 数据仍返回 true，循环空转但不产出帧，而库内部的 HTTP 读取
        // 又可能阻塞自旋数百毫秒——core 0 的 IDLE0 连续数秒得不到调度，
        // TASK_WDT 判定系统卡死并 abort()。（实测崩溃前 SPEC 的
        // feedAge 已从 3ms 恶化到 575ms，正是这个过程。）
        //
        // 改为时间驱动后，无论解码走哪条路径、是否产出帧，最多
        // kYieldEveryMs 就必定让出一次，看门狗不可能再饿死。
        // 20ms 兼顾两头：DMA 有约 185ms 存量不会欠载，UI 与网络
        // 任务也拿得到稳定的调度窗口。
        constexpr uint32_t kYieldEveryMs = 20;
        const uint32_t nowMs = millis();
        if (nowMs - lastYieldMs >= kYieldEveryMs) {
            lastYieldMs = nowMs;
            vTaskDelay(1);
        }
    }
}
