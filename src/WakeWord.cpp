#include "WakeWord.h"
#include "VoiceAssistant.h"
#include "pins_audio.h"

#include <driver/i2s.h>
#include <math.h>
extern "C" {
#include <esp_wn_models.h>
#include <esp_wn_iface.h>
#include <model_path.h>
}

WakeWordClass WakeWord;

namespace {
constexpr float kWakeThreshold = 0.52f;
}

void WakeWordClass::init() {
    if (_task) return;
    if (!psramFound()) {
        _enabled = false;
        Serial.println("[WAKE] PSRAM unavailable; wake word disabled");
        return;
    }
    _lock = xSemaphoreCreateMutex();
    _enabled = true;
    if (!_lock || xTaskCreatePinnedToCore(taskEntry, "wake", 12288, this, 3,
                                          &_task, 1) != pdPASS) {
        _task = nullptr;
        _enabled = false;
        Serial.println("[WAKE] task creation failed");
        return;
    }
    Serial.println("[WAKE] ESP-SR ready; word=你好小智");
}

void WakeWordClass::update() {}

bool WakeWordClass::beginMic(int frameSamples) {
    if (_micActive) return true;
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = 16000;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = constrain(frameSamples, 64, 1024);
    if (i2s_driver_install((i2s_port_t)I2S_MIC_PORT_NUM, &cfg, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.bck_io_num = PIN_MIC_SCK;
    pins.ws_io_num = PIN_MIC_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = PIN_MIC_SD;
    if (i2s_set_pin((i2s_port_t)I2S_MIC_PORT_NUM, &pins) != ESP_OK) {
        i2s_driver_uninstall((i2s_port_t)I2S_MIC_PORT_NUM);
        return false;
    }
    i2s_zero_dma_buffer((i2s_port_t)I2S_MIC_PORT_NUM);
    _micActive = true;
    return true;
}

void WakeWordClass::endMic() {
    if (!_micActive) return;
    _micActive = false;
    i2s_driver_uninstall((i2s_port_t)I2S_MIC_PORT_NUM);
}

void WakeWordClass::taskEntry(void *arg) { static_cast<WakeWordClass *>(arg)->taskLoop(); }

void WakeWordClass::taskLoop() {
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        Serial.println("[WAKE] model init failed; upload model SPIFFS required");
        _enabled = false;
        vTaskDelete(nullptr);
        return;
    }
    char *name = esp_srmodel_filter(models, ESP_WN_PREFIX, "nihaoxiaozhi");
    if (!name) name = (char *)"wn9_nihaoxiaozhi";
    const esp_wn_iface_t *wakenet = esp_wn_handle_from_name(name);
    model_iface_data_t *model = wakenet ? wakenet->create(name, DET_MODE_95) : nullptr;
    if (!wakenet || !model) {
        Serial.println("[WAKE] WakeNet model init failed");
        esp_srmodel_deinit(models);
        _enabled = false;
        vTaskDelete(nullptr);
        return;
    }
    const int chunk = wakenet->get_samp_chunksize(model);
    const int sampleRate = wakenet->get_samp_rate(model);
    const int channels = wakenet->get_channel_num(model);
    if (chunk <= 0 || chunk > 2048 || sampleRate != 16000 || channels != 1) {
        Serial.printf("[WAKE] invalid model chunk=%d rate=%d channels=%d\n",
                      chunk, sampleRate, channels);
        wakenet->destroy(model);
        esp_srmodel_deinit(models);
        _enabled = false;
        vTaskDelete(nullptr);
        return;
    }
    const int wordCount = wakenet->get_word_num(model);
    for (int word = 1; word <= wordCount; ++word) {
        const float original = wakenet->get_det_threshold(model, word);
        const char *wordName = wakenet->get_word_name(model, word);
        const int changed = wakenet->set_det_threshold(
            model, kWakeThreshold, word);
        Serial.printf("[WAKE] word=%d name=%s threshold=%.3f->%.3f set=%d\n",
                      word, wordName ? wordName : "-", original,
                      kWakeThreshold, changed);
    }
    int32_t *raw = static_cast<int32_t *>(malloc(chunk * sizeof(int32_t)));
    int16_t *samples = static_cast<int16_t *>(malloc(chunk * sizeof(int16_t)));
    if (!raw || !samples || !beginMic(chunk)) {
        Serial.println("[WAKE] audio buffer or mic init failed");
        free(raw);
        free(samples);
        wakenet->destroy(model);
        esp_srmodel_deinit(models);
        _enabled = false;
        vTaskDelete(nullptr);
        return;
    }
    Serial.printf("[WAKE] listening model=%s chunk=%d heap=%u psram=%u\n",
                  name, chunk, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram());
    uint32_t lastPeakLog = millis();
    int16_t intervalRawPeak = 0;
    int16_t intervalPeak = 0;
    uint64_t intervalSquareSum = 0;
    uint32_t intervalSamples = 0;
    uint32_t intervalClipped = 0;
    for (;;) {
        // 语音会话进行中：释放麦克风交给 VoiceAssistant，不抢
        if (VoiceAssistant.enabled()) {
            endMic();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!_micActive && !beginMic(chunk)) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        const size_t wantedBytes = chunk * sizeof(int32_t);
        size_t bytes = 0;
        while (bytes < wantedBytes && !VoiceAssistant.enabled()) {
            size_t received = 0;
            const esp_err_t err = i2s_read(
                (i2s_port_t)I2S_MIC_PORT_NUM,
                reinterpret_cast<uint8_t *>(raw) + bytes,
                wantedBytes - bytes, &received, pdMS_TO_TICKS(100));
            if (err != ESP_OK) break;
            bytes += received;
        }
        if (bytes != wantedBytes) continue;
        int count = bytes / sizeof(int32_t);
        int16_t rawPeak = 0;
        int16_t peak = 0;
        for (int i = 0; i < count; ++i) {
            int32_t s = raw[i] >> 11;
            const int32_t rawAbs = s < 0 ? -s : s;
            if (rawAbs > rawPeak) rawPeak = min(rawAbs, (int32_t)32767);
            s *= 4;
            if (s > 32767) s = 32767;
            else if (s < -32768) s = -32768;
            samples[i] = (int16_t)s;
            const int32_t a = samples[i] < 0 ? -(int32_t)samples[i] : samples[i];
            if (a > peak) peak = a;
            intervalSquareSum += (uint64_t)((int64_t)samples[i] * samples[i]);
            intervalSamples++;
            if (samples[i] == 32767 || samples[i] == -32768) intervalClipped++;
        }
        if (rawPeak > intervalRawPeak) intervalRawPeak = rawPeak;
        if (peak > intervalPeak) intervalPeak = peak;
        if (millis() - lastPeakLog >= 2000) {
            const uint32_t rms = intervalSamples
                ? (uint32_t)sqrt((double)intervalSquareSum / intervalSamples)
                : 0;
            Serial.printf("[WAKE] mic raw=%d gain4=%d rms=%lu clip=%lu heap=%u\n",
                          (int)intervalRawPeak, (int)intervalPeak,
                          (unsigned long)rms, (unsigned long)intervalClipped,
                          (unsigned)ESP.getFreeHeap());
            intervalRawPeak = 0;
            intervalPeak = 0;
            intervalSquareSum = 0;
            intervalSamples = 0;
            intervalClipped = 0;
            lastPeakLog = millis();
        }
        const wakenet_state_t result = wakenet->detect(model, samples);
        if (result == WAKENET_DETECTED) {
            Serial.println("[WAKE] detected: 你好小智");
            endMic();
            // 唤醒即交给 VoiceAssistant：它会淡出音乐、连 WebSocket，
            // 待上游 session.ready 后才响提示音——"叮"响起时才真正在听，
            // 用户不会因为抢在连接完成前说话而被吞掉前几个字。
            VoiceAssistant.setEnabled(true);
            continue;
        }
    }
}
