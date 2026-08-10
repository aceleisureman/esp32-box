#include "AudioPlayer.h"
#include "pins_audio.h"
#include "Spectrum.h"

#include <Preferences.h>

#include <AudioFileSourceHTTPStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorFLAC.h>
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
    gOut->SetGain((float)_volume * 0.9f / 100.0f);
    // 开机即安装 I2S 驱动并终身常驻：引脚从上电起就被持续驱动
    // （时钟 + 零数据），待机不悬空、不拾噪。配合上面的 stop()
    // 覆写，任何时刻功放收到的都是合法 I2S 信号。
    gOut->begin();

    // 解码任务放 core 0：Arduino loop（UI 刷新）固定跑在 core 1，
    // 分核可避免解码把 UI 挤掉。优先级取 2，高于网络任务但低于系统关键任务。
    // 10KB 栈：libFLAC 每帧解码调用链比 libmad 更深，留足余量。
    xTaskCreatePinnedToCore(&AudioPlayerClass::taskEntry, "audio", 10240, this,
                            2, &_task, 0);
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

void AudioPlayerClass::setVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    _volume = percent;
    _volDirtyMs = millis();   // 防抖持久化：停止调节 2s 后落盘
    // 增益上限 0.9 而非 1.0：MP3 解码后的样本本就接近满幅，
    // 增益拉到 1.0 时响度峰值会削波，听感就是持续的破音/沙沙声。
    if (gOut) gOut->SetGain((float)_volume * 0.9f / 100.0f);
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
    if (gBuffer) { delete gBuffer; gBuffer = nullptr; }
    if (gStream) { delete gStream; gStream = nullptr; }
}

void AudioPlayerClass::taskEntry(void *arg) {
    static_cast<AudioPlayerClass *>(arg)->taskLoop();
}

void AudioPlayerClass::taskLoop() {
    uint32_t frameCount = 0;

    for (;;) {
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
            _finished = true;
            Serial.println("[AUDIO] track finished");
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

        // 让出策略：每帧都 vTaskDelay(1) 会让解码跟不上 I2S 消耗而爆音，
        // 每帧都不让又会饿死 UI。折中为按帧计数周期性让出——
        // MP3 每帧 26ms 音频，每 8 帧（约 200ms 音频）让出 1 tick。
        // FLAC 帧更长（约 0.5s 音频），让出会更稀疏，但 FLAC 每帧解码
        // 更重，靠帧计数让出仍能保证 DMA 不欠载、UI 有调度窗口。
        if ((++frameCount & 0x07) == 0) {
            vTaskDelay(1);
        }
    }
}
