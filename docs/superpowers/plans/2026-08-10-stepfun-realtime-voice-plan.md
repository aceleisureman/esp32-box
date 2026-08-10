# StepFun Realtime Voice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 ESP32 语音助手改造成经自有后端代理 StepFun Realtime 的半双工连续语音对话。

**Architecture:** 保留 Flask HTTP 回退接口，在同一服务新增 Flask-Sock WebSocket 入口。后端为每个设备连接建立一个 StepFun Realtime 上游会话；ESP32 上传 PCM16 二进制帧，并将下行 PCM16 放入 PSRAM 环形缓冲后通过现有 I2S0 播放。

**Tech Stack:** Python 3.10+、Flask、Flask-Sock、websockets、unittest、Arduino ESP32、arduinoWebSockets、FreeRTOS、I2S。

---

## 文件结构

- Create: `server/realtime.py`：StepFun 会话配置、事件转换和双向转发。
- Create: `server/tests/test_realtime.py`：后端协议转换与会话测试。
- Modify: `server/app.py`：注册设备 WebSocket 路由，保留现有 HTTP 路由。
- Modify: `server/requirements.txt`：加入 WebSocket 依赖。
- Create: `server/.env.example`：记录非敏感配置项。
- Create: `src/VoiceWebSocket.h`：设备 WebSocket 连接接口与回调契约。
- Create: `src/VoiceWebSocket.cpp`：连接、收发、心跳和指数重连。
- Modify: `src/AudioPlayer.h`：增加 PCM 提示音流接口。
- Modify: `src/AudioPlayer.cpp`：增加有界 PSRAM PCM 环形缓冲和 I2S 播放模式。
- Modify: `src/VoiceAssistant.h`：改成连续会话状态机，并保留 HTTP 回退入口。
- Modify: `src/VoiceAssistant.cpp`：实时采集、状态切换、协议事件和命令执行。
- Modify: `src/Input.cpp`、`src/Input.h`：长按播放键切换实时会话开关。
- Modify: `include/pins_audio.h`：增加后端 WebSocket URL 和音频参数。
- Modify: `platformio.ini`：加入 WebSockets 与 ArduinoJson 依赖。

### Task 1: 后端协议转换测试

**Files:**
- Create: `server/tests/__init__.py`
- Create: `server/tests/test_realtime.py`

- [ ] **Step 1: 写失败测试**

测试以下纯函数契约：

```python
from realtime import build_session_update, device_audio_to_stepfun, stepfun_event_to_device


def test_session_uses_pcm16_server_vad():
    event = build_session_update("wenrounansheng")
    assert event["type"] == "session.update"
    assert event["session"]["input_audio_format"] == "pcm16"
    assert event["session"]["output_audio_format"] == "pcm16"
    assert event["session"]["turn_detection"]["type"] == "server_vad"


def test_audio_frame_is_base64_append():
    event = device_audio_to_stepfun(b"\x01\x02")
    assert event == {"type": "input_audio_buffer.append", "audio": "AQI="}


def test_audio_delta_becomes_binary():
    kind, payload = stepfun_event_to_device(
        {"type": "response.audio.delta", "delta": "AQI="}
    )
    assert kind == "binary"
    assert payload == b"\x01\x02"
```

- [ ] **Step 2: 运行测试并确认失败**

Run: `cd server && python -m unittest tests.test_realtime -v`

Expected: `ModuleNotFoundError: No module named 'realtime'`。

- [ ] **Step 3: 提交测试**

```bash
git add server/tests
git commit -m "test: define realtime voice protocol"
```

### Task 2: StepFun Realtime 适配器

**Files:**
- Create: `server/realtime.py`
- Modify: `server/requirements.txt`
- Create: `server/.env.example`

- [ ] **Step 1: 实现可测试的协议函数**

实现：

```python
def build_session_update(voice: str) -> dict: ...
def device_audio_to_stepfun(pcm: bytes) -> dict: ...
def stepfun_event_to_device(event: dict) -> tuple[str, bytes | dict] | None: ...
```

事件映射必须覆盖 `session.ready`、`speech.started`、`speech.stopped`、用户/助手最终文本、`response.audio.delta`、`response.audio.done` 和 `error`。Base64 非法时返回协议错误，不得让转发线程崩溃。

- [ ] **Step 2: 实现单设备会话**

增加 `StepFunRealtimeSession`：

```python
class StepFunRealtimeSession:
    def __init__(self, device_socket, api_key: str, model: str, voice: str): ...
    def run(self) -> None: ...
    def close(self) -> None: ...
```

`run()` 创建上游连接与两个转发线程；任意方向结束时设置共享停止事件、关闭两个 socket，并等待线程退出。设备二进制帧上限 4096 字节；文本帧只接受 `session.start`、`playback.done` 和 `ping`。

- [ ] **Step 3: 增加配置依赖**

`requirements.txt` 加入：

```text
flask-sock>=0.7
websockets>=12,<16
```

`.env.example` 包含：

```text
STEPFUN_API_KEY=
STEPFUN_REALTIME_MODEL=stepaudio-2.5-realtime
STEPFUN_REALTIME_VOICE=wenrounansheng
STEPFUN_VAD_SILENCE_MS=800
```

- [ ] **Step 4: 运行测试**

Run: `cd server && python -m unittest tests.test_realtime -v`

Expected: 全部通过。

- [ ] **Step 5: 提交适配器**

```bash
git add server/realtime.py server/requirements.txt server/.env.example
git commit -m "feat: add StepFun realtime adapter"
```

### Task 3: 设备 WebSocket 后端入口

**Files:**
- Modify: `server/app.py`
- Modify: `server/tests/test_realtime.py`

- [ ] **Step 1: 写路由失败测试**

使用 mock `StepFunRealtimeSession`，断言 `/api/voice/realtime` 缺少 API Key 时发送 `missing_api_key` 错误并关闭；配置 API Key 时创建并运行会话。

- [ ] **Step 2: 运行测试并确认失败**

Run: `cd server && python -m unittest tests.test_realtime -v`

Expected: WebSocket 路由测试失败。

- [ ] **Step 3: 注册 Flask-Sock 路由**

```python
sock = Sock(app)

@sock.route("/api/voice/realtime")
def realtime_voice(ws):
    api_key = os.environ.get("STEPFUN_API_KEY", "").strip()
    if not api_key:
        ws.send(json.dumps({"type": "error", "code": "missing_api_key",
                            "message": "STEPFUN_API_KEY 未配置", "retryable": False}))
        return
    StepFunRealtimeSession.from_env(ws, api_key).run()
```

- [ ] **Step 4: 验证 HTTP 回退未回归**

Run: `cd server && python -m unittest discover -s tests -v`

Expected: 全部通过；`GET /health` 仍返回 200。

- [ ] **Step 5: 提交入口**

```bash
git add server/app.py server/tests/test_realtime.py
git commit -m "feat: expose realtime voice websocket"
```

### Task 4: ESP32 WebSocket 连接模块

**Files:**
- Create: `src/VoiceWebSocket.h`
- Create: `src/VoiceWebSocket.cpp`
- Modify: `platformio.ini`
- Modify: `include/pins_audio.h`

- [ ] **Step 1: 定义接口**

`VoiceWebSocketClass` 提供：

```cpp
using BinaryHandler = void (*)(const uint8_t *, size_t);
using TextHandler = void (*)(const char *, size_t);

void init(BinaryHandler binaryHandler, TextHandler textHandler);
void update(bool wifiConnected);
bool sendAudio(const uint8_t *data, size_t len);
bool sendEvent(const char *json);
bool connected() const;
void disconnect();
```

- [ ] **Step 2: 实现连接状态机**

解析 `VOICE_WS_URL` 的 `ws://host:port/path`；使用 `WebSocketsClient`，限制二进制帧为 4096 字节。每 15 秒发送 `ping`，30 秒没有 `pong` 主动断开。重连间隔依次为 1、2、4、8、16、30 秒，连接成功清零。

WebSocket 回调只复制数据并通知语音任务，不直接调用 I2S、播放器或 UI。

- [ ] **Step 3: 增加依赖和配置**

`platformio.ini` 加入：

```ini
links2004/WebSockets@^2.6.1
bblanchon/ArduinoJson@^7.4.2
```

`pins_audio.h` 增加 `VOICE_WS_URL`，默认指向当前局域网服务器的 `/api/voice/realtime`。

- [ ] **Step 4: 编译验证**

Run: `pio run -e esp32-s3-st7789`

Expected: 编译成功，无未定义符号。

- [ ] **Step 5: 提交连接模块**

```bash
git add src/VoiceWebSocket.h src/VoiceWebSocket.cpp platformio.ini include/pins_audio.h
git commit -m "feat: add ESP32 voice websocket client"
```

### Task 5: PCM 流式播放

**Files:**
- Modify: `src/AudioPlayer.h`
- Modify: `src/AudioPlayer.cpp`

- [ ] **Step 1: 增加 PCM 播放 API**

```cpp
bool beginPcmPrompt(uint32_t sampleRate = 16000);
size_t writePcm(const uint8_t *data, size_t len);
void endPcmPrompt();
void abortPcmPrompt();
bool isPcmPromptDrained() const;
size_t pcmBufferedBytes() const;
```

- [ ] **Step 2: 实现有界环形缓冲**

在 PSRAM 分配 128 KiB 缓冲。读写索引受临界区保护；写入不得覆盖未播放数据，空间不足返回实际写入字节数。`abortPcmPrompt()` 清空缓冲并将 I2S DMA 置零。

- [ ] **Step 3: 集成现有音频任务**

PCM 模式启动时关闭解码流、暂停音乐并将 `gOut` 设置为 16 kHz/16-bit/单声道。音频任务持续取 PCM 样本，经现有音量增益后写入 `gOut->ConsumeSample()`。收到结束标志且缓冲清空后退出 prompt 模式，保持 I2S 零数据时钟。

- [ ] **Step 4: 编译验证**

Run: `pio run -e esp32-s3-st7789`

Expected: 编译成功；原 MP3/FLAC/WAV 路径仍可链接。

- [ ] **Step 5: 提交 PCM 播放**

```bash
git add src/AudioPlayer.h src/AudioPlayer.cpp
git commit -m "feat: stream PCM voice replies"
```

### Task 6: 连续语音状态机

**Files:**
- Modify: `src/VoiceAssistant.h`
- Modify: `src/VoiceAssistant.cpp`
- Modify: `src/Input.h`
- Modify: `src/Input.cpp`

- [ ] **Step 1: 定义状态和公开接口**

```cpp
enum class State : uint8_t {
    Disabled, Disconnected, Connecting, Listening,
    UserSpeaking, Waiting, Playing, Error
};

void setEnabled(bool enabled);
void toggleEnabled();
bool enabled() const;
State state() const;
```

保留 `uploadAudio()` 和旧录音缓冲，仅作为明确调用的 HTTP 回退，不参与实时路径。

- [ ] **Step 2: 实现上行采集**

进入 `Listening` 后安装 I2S1。每次读取 640 字节 PCM（20 ms）并立即调用 `VoiceWebSocket.sendAudio()`；发送失败不缓存。收到 `speech.stopped` 后停止上传并进入 `Waiting`。

- [ ] **Step 3: 实现下行播放**

首个二进制帧触发 `AudioPlayer.beginPcmPrompt()` 并进入 `Playing`。后续帧写入 PCM 缓冲；空间不足记录 overflow 并中止本轮，防止错位音频。收到 `audio.done` 后等待缓冲清空，发送 `playback.done`，重新安装麦克风并进入 `Listening`。

- [ ] **Step 4: 解析控制事件**

使用 ArduinoJson 按 `type` 分发。`command.action` 只接受白名单；同一会话轮次只执行一次。`error.retryable=true` 进入重连，false 进入 `Error` 并保持停止采集。

- [ ] **Step 5: 调整按键语义**

播放键长按 800 ms 改为切换实时助手启用/禁用；短按继续控制音乐。关闭助手必须停止 I2S1、断开 WebSocket、清空 PCM 缓冲。

- [ ] **Step 6: 编译验证**

Run: `pio run -e esp32-s3-st7789`

Expected: 编译成功。

- [ ] **Step 7: 提交状态机**

```bash
git add src/VoiceAssistant.h src/VoiceAssistant.cpp src/Input.h src/Input.cpp
git commit -m "feat: enable continuous half-duplex voice"
```

### Task 7: 静态验证与本地集成测试

**Files:**
- Modify: `server/README.md`（不存在则创建）

- [ ] **Step 1: 补充启动说明**

记录虚拟环境安装、`.env` 配置、`python app.py` 启动、健康检查和设备 URL。明确不能把 API Key 写入固件。

- [ ] **Step 2: 运行后端测试和语法检查**

Run: `cd server && python -m unittest discover -s tests -v`

Run: `cd server && python -m py_compile app.py realtime.py voice.py asr.py llm.py tts.py commands.py`

Expected: 两条命令均成功。

- [ ] **Step 3: 运行固件完整编译**

Run: `pio run -e esp32-s3-st7789`

Expected: 编译成功并输出 RAM、Flash 使用量。

- [ ] **Step 4: 启动后端冒烟测试**

Run: `cd server && python app.py`

另一个终端运行：`curl http://127.0.0.1:5000/health`

Expected: `{"ok":true}`。没有真实 API Key 时，WebSocket 返回 `missing_api_key`，而不是服务崩溃。

- [ ] **Step 5: 提交文档**

```bash
git add server/README.md
git commit -m "docs: document realtime voice setup"
```

### Task 8: 真机验收

**Files:**
- No code changes expected

- [ ] **Step 1: 配置真实密钥并启动服务**

设置 `STEPFUN_API_KEY`，确认日志出现设备连接、StepFun session ready，日志不得打印密钥或完整音频内容。

- [ ] **Step 2: 验证正常对话**

长按启用助手，说三轮短句。确认 VAD 自动结束、首包延迟有日志、回答边生成边播放、每轮播放结束自动恢复监听。

- [ ] **Step 3: 验证半双工**

AI 播放期间对麦克风说话，确认设备没有上传该段音频；回答结束后再次说话可正常识别。

- [ ] **Step 4: 验证命令和断线恢复**

测试下一首、暂停、音量调整各一次。依次断开 WiFi、停止后端、恢复服务，确认指数重连且不播放断线前残留音频。

- [ ] **Step 5: 运行稳定性测试**

连续运行 30 分钟，每分钟记录 `ESP.getFreeHeap()`、`ESP.getFreePsram()` 和语音/音频任务栈高水位。通过标准：无持续下降趋势、无看门狗复位、无 PCM overflow。

