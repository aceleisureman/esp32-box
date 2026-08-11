# 语音助手后端（Voice Assistant Backend）

ESP32-S3 盒子语音助手的服务端：麦克风录音 → 云端 ASR 转文字 → 阶跃星辰（StepFun）大模型对话 + 播放器指令识别 → TTS 语音合成 → 设备扬声器播放。

## 快速开始

```bash
cd server
python3 -m venv .venv
source .venv/bin/activate        # Windows: .venv\Scripts\activate
pip install -r requirements.txt
cp .env.example .env             # 填入真实密钥
python app.py                    # 默认监听 0.0.0.0:5000
```

设备与后端须在同一局域网（音频直链走明文 HTTP，供 ESP32 的
`AudioFileSourceHTTPStream` 直接拉流）。

## 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/voice/audio` | 上传录音（multipart `audio` 字段，WAV/16kHz PCM），返回 `{text, user_text, audio_url, command}` |
| GET | `/tts_cache/<id>.mp3` | 设备拉流 TTS 音频（明文 HTTP） |
| GET | `/health` | 健康检查 |

`command` 字段：`null`（无指令）或 `{"action": "next" | ...}`，见 `commands.py`。

## 配置（.env）

| 变量 | 说明 | 状态 |
|------|------|------|
| `STEPFUN_API_KEY` | 阶跃星辰密钥 | 待填入 |
| `STEPFUN_CHAT_MODEL` | 对话模型名 | 待确认 |
| `STEPFUN_TTS_MODEL` | StepFun TTS 模型 | 待确认（可能无此接口） |
| `TTS_BACKEND` | `stepfun` / `edge` / `mock` | 默认 stepfun |
| `ASR_BACKEND` | `mock` / 厂商实现 | 默认 mock |
| `LAN_IP` | 设备可访问的本机 IP | 留空自动探测 |
| `VIRTUAL_COMPANION_PROMPT` | 人设提示词，留空用内置陪伴人格 | 可选 |
| `MEMORY_MAX_TURNS` | 每设备保留的短期对话轮数 | 默认 12 |

## 情感陪伴与多设备记忆

设备通过 WebSocket 的 `session.start` 事件上报 `device_id`，后端据此为
**每台设备维护独立记忆**，互不干扰（客厅盒子和卧室盒子记得不同的事）。

```
server/memory/<device_id>.json
├── profile   长期记忆（称呼、偏好等重要事实）
├── history   最近对话轮次（短期记忆，默认 12 轮）
└── turns     累计对话轮数
```

每次会话建立时，`memory.build_instructions(device_id)` 会把
**情感人设 + 该设备的记忆**拼进 StepFun 实时会话的 `instructions`，
所以模型能记住这台设备的用户是谁、上次聊过什么。

对话过程中，用户的每句话与助手的回复都会自动追加进该设备的记忆，
超过上限时保留最近若干轮。

**换人设**：在 `.env` 设 `VIRTUAL_COMPANION_PROMPT`，例如
`VIRTUAL_COMPANION_PROMPT=你是一只会说话的猫，说话带喵。`

**隐私**：`server/memory/` 已加入 `.gitignore`，对话内容不会入库。

## 当前实现状态

- **ASR**：`mock`（返回固定文字）。厂商实现（阿里/讯飞/百度/OpenAI）为占位，待确认。
- **LLM**：StepFun OpenAI 兼容客户端已写好；无密钥时自动走 `mock`。
- **TTS**：`stepfun`（接口待确认）、`edge`（微软 Edge TTS，免费）、`mock`（不产音频）。
- **指令识别**：`commands.py` 规则优先，未命中交 LLM 兜底。

## 验证

```bash
# 用文字模拟识别结果（无需麦克风/ASR 密钥）
ASR_BACKEND=mock ASR_MOCK_TEXT="下一首" python app.py
curl -X POST -F "audio=@some.wav" http://localhost:5000/api/voice/audio
# → {"text":"好的，下一首","user_text":"下一首","audio_url":null,"command":{"action":"next"}}
```
