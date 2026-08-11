#include "VoiceAssistant.h"
#include "VoiceWebSocket.h"
#include "pins_audio.h"
#include "AudioPlayer.h"
#include "MusicService.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <driver/i2s.h>

VoiceAssistantClass VoiceAssistant;

void VoiceAssistantClass::init() {
    if (_task) return;
    _micLock = xSemaphoreCreateMutex();
    VoiceWebSocket.init(onBinary, onText);
    if (!_micLock || xTaskCreatePinnedToCore(taskEntry, "voice", 12288, this,
                                             2, &_task, 0) != pdPASS) {
        _task = nullptr;
        Serial.println("[VOICE] task creation failed");
        return;
    }
    Serial.println("[VOICE] realtime assistant ready; long press to enable");
}

void VoiceAssistantClass::update() {
    if (!_enabled) return;
    VoiceWebSocket.update(WiFi.status() == WL_CONNECTED);
    if (!VoiceWebSocket.connected() && _state != State::Connecting) {
        _sessionReady = false;
        endMicI2s();
        _state = State::Connecting;
    }
}

void VoiceAssistantClass::setEnabled(bool enabled) {
    if (_enabled == enabled) return;
    _enabled = enabled;
    if (enabled) {
        // 记录唤醒前是否在播音乐：唤醒后语音会暂停它，会话结束无指令时恢复。
        // 淡出而非骤停：唤醒瞬间音乐平滑消失，像手机来电那样自然。
        _wasMusicPlaying = AudioPlayer.isPlaying();
        if (_wasMusicPlaying) {
            AudioPlayer.fadeOutPause();
        } else {
            AudioPlayer.cancelPending();
        }
        _state = State::Connecting;
        _sessionReady = false;
        _pendingCommand = PendingCommand::None;
        _greeted = false;   // 新一轮唤醒：等 session.ready 后再响提示音
        Serial.println("[VOICE] continuous conversation enabled");
    } else {
        endMicI2s();
        VoiceWebSocket.disconnect();
        AudioPlayer.abortPcmPrompt();
        _audioStarted = false;
        _audioDone = false;
        _sessionReady = false;
        _userText = "";   // 会话结束清空说话内容
        _state = State::Disabled;
        // 无指令会话结束：淡入恢复唤醒前在播的音乐
        if (_wasMusicPlaying) {
            const bool resumeMusic = _pendingCommand != PendingCommand::Pause;
            _wasMusicPlaying = false;
            if (resumeMusic) {
                _musicPlayRequested = true;
                AudioPlayer.fadeInResume();
                Serial.println("[VOICE] music restart requested after conversation");
            }
        }
        Serial.println("[VOICE] continuous conversation disabled");
    }
}

void VoiceAssistantClass::disableAndRestoreMusic() {
    // 长按播放键退出对话：等同 setEnabled(false)，恢复音乐
    setEnabled(false);
}

void VoiceAssistantClass::toggleEnabled() { setEnabled(!_enabled); }

bool VoiceAssistantClass::beginMicI2s() {
    if (_micLock) xSemaphoreTake(_micLock, portMAX_DELAY);
    if (_micActive) {
        if (_micLock) xSemaphoreGive(_micLock);
        return true;
    }
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = MIC_SAMPLE_RATE;
    // INMP441 输出 24-bit 有效数据，承载在 32-bit I2S 时隙中。
    // 直接按 16-bit 读取会落在无效低位，表现为持续全零。
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 320;
    cfg.use_apll = false;
    if (i2s_driver_install((i2s_port_t)I2S_MIC_PORT_NUM, &cfg, 0, nullptr) != ESP_OK) {
        if (_micLock) xSemaphoreGive(_micLock);
        return false;
    }
    i2s_pin_config_t pins = {};
    pins.bck_io_num = PIN_MIC_SCK;
    pins.ws_io_num = PIN_MIC_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = PIN_MIC_SD;
    if (i2s_set_pin((i2s_port_t)I2S_MIC_PORT_NUM, &pins) != ESP_OK) {
        i2s_driver_uninstall((i2s_port_t)I2S_MIC_PORT_NUM);
        if (_micLock) xSemaphoreGive(_micLock);
        return false;
    }
    i2s_zero_dma_buffer((i2s_port_t)I2S_MIC_PORT_NUM);
    _micActive = true;
    if (_micLock) xSemaphoreGive(_micLock);
    return true;
}

void VoiceAssistantClass::endMicI2s() {
    if (_micLock) xSemaphoreTake(_micLock, portMAX_DELAY);
    if (!_micActive) {
        if (_micLock) xSemaphoreGive(_micLock);
        return;
    }
    _micActive = false;
    i2s_driver_uninstall((i2s_port_t)I2S_MIC_PORT_NUM);
    if (_micLock) xSemaphoreGive(_micLock);
}

void VoiceAssistantClass::onBinary(const uint8_t *data, size_t len) {
    if (!VoiceAssistant._enabled || !data || !len) return;
    if (!VoiceAssistant._audioStarted) {
        VoiceAssistant.endMicI2s();
        if (!AudioPlayer.beginPcmPrompt(VOICE_OUTPUT_SAMPLE_RATE)) {
            Serial.println("[VOICE] PCM playback start failed");
            VoiceAssistant._state = State::Error;
            return;
        }
        VoiceAssistant._audioStarted = true;
        VoiceAssistant._state = State::Playing;
        Serial.printf("[VOICE] response audio started rate=%u first=%uB\n",
                      VOICE_OUTPUT_SAMPLE_RATE, (unsigned)len);
    }
    if (AudioPlayer.writePcm(data, len) != len) {
        Serial.println("[VOICE] PCM buffer overflow; aborting response");
        AudioPlayer.abortPcmPrompt();
        VoiceAssistant._state = State::Error;
    }
}

void VoiceAssistantClass::onText(const char *data, size_t len) {
    VoiceAssistant.handleText(data, len);
}

void VoiceAssistantClass::handleText(const char *data, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) return;
    const char *type = doc["type"] | "";
    if (!strcmp(type, "session.ready")) {
        _sessionReady = true;
        if (beginMicI2s()) {
            _state = State::Listening;
            // 提示音放在这里而非唤醒瞬间：此刻上游会话已就绪、麦克风已开，
            // "叮"响起时说的每个字都能被收到，不会像连接前提示那样吞字。
            // 仅本轮会话的第一次就绪才响，连续对话的后续轮次不重复提示。
            if (!_greeted) {
                _greeted = true;
                AudioPlayer.playBeep();
            }
            Serial.println("[VOICE] session ready; listening");
        }
    } else if (!strcmp(type, "speech.started")) {
        _state = State::UserSpeaking;
        Serial.println("[VOICE] speech started");
    } else if (!strcmp(type, "speech.stopped")) {
        endMicI2s();
        _state = State::Waiting;
        Serial.println("[VOICE] speech stopped; waiting response");
    } else if (!strcmp(type, "audio.done")) {
        _audioDone = true;
        AudioPlayer.endPcmPrompt();
        Serial.println("[VOICE] response audio done");
    } else if (!strcmp(type, "transcript.user")) {
        // 用户语音转文字结果：缓存供唤醒页实时显示"你说了什么"
        const char *text = doc["text"] | "";
        if (text && *text) {
            _userText = String(text);
            Serial.printf("[VOICE] user said: %.80s\n", _userText.c_str());
        }
    } else if (!strcmp(type, "command")) {
        applyCommand(doc["action"] | "", doc["song"] | "");
    } else if (!strcmp(type, "error")) {
        Serial.printf("[VOICE] server error: %s\n", (const char *)(doc["message"] | ""));
        if (!(bool)(doc["retryable"] | false)) _state = State::Error;
    } else if (!strcmp(type, "pong")) {
        // Library heartbeat already handles connection liveness.
    }
}

void VoiceAssistantClass::applyCommand(const char *action, const char *song) {
    if (!strcmp(action, "next")) _pendingCommand = PendingCommand::Next;
    else if (!strcmp(action, "prev")) _pendingCommand = PendingCommand::Previous;
    else if (!strcmp(action, "volume_up")) _pendingCommand = PendingCommand::VolumeUp;
    else if (!strcmp(action, "volume_down")) _pendingCommand = PendingCommand::VolumeDown;
    else if (!strcmp(action, "pause")) _pendingCommand = PendingCommand::Pause;
    else if (!strcmp(action, "play")) _pendingCommand = PendingCommand::Play;
    else if (!strcmp(action, "play_song")) {
        Serial.printf("[VOICE] named song unsupported by music backend: %.80s\n",
                      song ? song : "");
    }
    else Serial.printf("[VOICE] unsupported command=%s\n", action ? action : "");
}

void VoiceAssistantClass::taskEntry(void *arg) {
    static_cast<VoiceAssistantClass *>(arg)->taskLoop();
}

void VoiceAssistantClass::taskLoop() {
    uint8_t pcm[640];
    int32_t raw[320];
    uint32_t sentFrames = 0;
    uint32_t lastDiag = millis();
    for (;;) {
        if (_enabled && _micActive && VoiceWebSocket.connected() &&
            (_state == State::Listening || _state == State::UserSpeaking)) {
            size_t rawBytes = 0;
            if (_micLock) xSemaphoreTake(_micLock, portMAX_DELAY);
            const esp_err_t readResult = _micActive
                ? i2s_read((i2s_port_t)I2S_MIC_PORT_NUM, raw, sizeof(raw),
                           &rawBytes, pdMS_TO_TICKS(40))
                : ESP_ERR_INVALID_STATE;
            if (_micLock) xSemaphoreGive(_micLock);
            if (readResult == ESP_OK && rawBytes) {
                const size_t samples = rawBytes / sizeof(int32_t);
                for (size_t i = 0; i < samples; i++) {
                    // INMP441 实机语音幅度较低，放大后再饱和到 PCM16，
                    // 避免上游 VAD 将正常说话判定为静音。
                    int32_t sample = raw[i] >> 11;
                    if (sample > 32767) sample = 32767;
                    if (sample < -32768) sample = -32768;
                    const int16_t out = (int16_t)sample;
                    pcm[i * 2] = (uint8_t)(out & 0xff);
                    pcm[i * 2 + 1] = (uint8_t)((uint16_t)out >> 8);
                }
                const size_t bytesRead = samples * 2;
                if (VoiceWebSocket.sendAudio(pcm, bytesRead)) sentFrames++;
                if (millis() - lastDiag >= 2000) {
                    int16_t peak = 0;
                    for (size_t i = 0; i + 1 < bytesRead; i += 2) {
                        const int16_t sample = (int16_t)((uint16_t)pcm[i] |
                                                         ((uint16_t)pcm[i + 1] << 8));
                        if (abs(sample) > peak) peak = abs(sample);
                    }
                    Serial.printf("[VOICE] mic frames=%lu peak=%d state=%u\n",
                                  (unsigned long)sentFrames, (int)peak,
                                  (unsigned)_state);
                    lastDiag = millis();
                }
            }
        } else if (_enabled && _audioDone && _audioStarted &&
                   !AudioPlayer.isPromptActive()) {
            _audioDone = false;
            _audioStarted = false;
            VoiceWebSocket.sendEvent("{\"type\":\"playback.done\"}");
            const PendingCommand command = _pendingCommand;
            if (command != PendingCommand::None) {
                setEnabled(false);
                _pendingCommand = PendingCommand::None;
                if (command == PendingCommand::Play ||
                    command == PendingCommand::Next ||
                    command == PendingCommand::Previous) {
                    _musicPlayRequested = true;
                }
                if (command == PendingCommand::Next) MusicService.next();
                else if (command == PendingCommand::Previous) MusicService.previous();
                else if (command == PendingCommand::Pause) {
                    AudioPlayer.prepare(MusicService.playUrl(),
                                        MusicService.playFormat());
                }
                else if (command == PendingCommand::Play) AudioPlayer.resume();
                else if (command == PendingCommand::VolumeUp) AudioPlayer.volumeUp();
                else if (command == PendingCommand::VolumeDown) AudioPlayer.volumeDown();
                Serial.printf("[VOICE] music command executed=%u\n", (unsigned)command);
            } else if (beginMicI2s()) {
                _pendingCommand = PendingCommand::None;
                _state = State::Listening;
            }
        } else if (_enabled && _sessionReady && VoiceWebSocket.connected() &&
                   !_micActive &&
                   _state != State::Playing && _state != State::Waiting) {
            // WakeWord 与 VoiceAssistant 共享 I2S 端口；若交接瞬间仍在释放，
            // 后续轮询重试，避免把一次安装失败当成永久会话故障。
            if (beginMicI2s()) _state = State::Listening;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
