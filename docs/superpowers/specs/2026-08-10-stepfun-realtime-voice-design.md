# StepFun 实时语音助手设计

## 目标

将当前“录音完成后 HTTP 上传、等待整段 MP3”的语音链路改造成半双工连续对话：设备自动监听，StepFun 服务端 VAD 判断用户说完，回答音频边生成边播放，播放结束后自动恢复监听。

首版不实现回声消除、语音打断和唤醒词。保留现有 `POST /api/voice/audio` 作为故障回退。

## 架构

```text
INMP441 -> ESP32 PCM16 -> 自有后端 WebSocket -> StepFun Realtime
MAX98357A <- ESP32 PCM16 <- 自有后端 WebSocket <- StepFun Realtime
```

ESP32 不直接持有 StepFun API Key。自有后端负责认证 StepFun、建立上游会话、转发音频和文本事件，并将识别出的本地播放器命令发送给设备。

## 后端

新增异步 WebSocket 服务，与现有 Flask HTTP 服务并存。每个设备连接独占一个 StepFun Realtime 会话：

- 上游地址：`wss://api.stepfun.com/v1/realtime?model=stepaudio-2.5-realtime`
- 输入与输出：PCM16、16 kHz、16-bit、单声道、小端序
- 模态：`text` 和 `audio`
- VAD：`server_vad`，初始静音判定 800 ms
- 默认音色：`wenrounansheng`，允许通过环境变量覆盖
- API Key 仅从 `STEPFUN_API_KEY` 读取，不写入代码或固件

后端收到 ESP32 二进制帧后转为 Base64，通过 `input_audio_buffer.append` 发给 StepFun。收到 `response.audio.delta` 后解码 Base64，并以二进制帧转发给 ESP32。

最终用户转写、回答文本、播放结束和错误使用 JSON 文本帧发送。后端在最终用户转写完成时执行现有播放器规则匹配，命中后向设备发送 `command` 事件。

## 设备协议

连接地址：`ws://<server>:<port>/api/voice/realtime`。生产部署可升级为 `wss://`。

设备到后端：

- 二进制帧：原始 PCM16 音频，推荐每帧 640 或 1280 字节，对应 20 或 40 ms
- `{"type":"session.start","device_id":"..."}`：设备就绪
- `{"type":"playback.done"}`：回答音频已全部播放
- `{"type":"ping"}`：应用层心跳

后端到设备：

- 二进制帧：原始 PCM16 回答音频
- `{"type":"session.ready"}`：上游会话可用
- `{"type":"speech.started"}`、`speech.stopped`：VAD 状态
- `{"type":"transcript","role":"user|assistant","text":"..."}`：最终文本
- `{"type":"audio.done"}`：本轮回答音频发送完毕
- `{"type":"command","action":"next|prev|play|pause|volume_up|volume_down"}`
- `{"type":"error","code":"...","message":"...","retryable":true}`
- `{"type":"pong"}`：心跳响应

单个二进制帧最大 4096 字节。WebSocket 自带帧完整性检查，不重复增加 CRC。协议事件必须带明确类型，未知事件忽略并记录。

## ESP32 状态机

```text
DISCONNECTED -> CONNECTING -> LISTENING -> USER_SPEAKING
       ^                         |              |
       |                         +---- WAITING -+
       |                                  |
       +---------- backoff <--- PLAYING <-+
                                      |
                                      +-> LISTENING
```

- `LISTENING`、`USER_SPEAKING`：持续采集并上传麦克风 PCM。
- `WAITING`：停止上传，等待首个回答音频帧。
- `PLAYING`：停止麦克风上传，将下行 PCM 写入播放环形缓冲区。
- 收到 `audio.done` 且播放缓冲区清空后发送 `playback.done`，回到 `LISTENING`。
- 用户按键可手动停止或重新开始会话，保留现有长按录音流程作为 HTTP 回退入口。

播放缓冲区使用 PSRAM，目标容量 128 KiB。达到高水位时暂停读取网络，低于低水位后恢复；不得无限缓存。音频任务优先于 UI 刷新，避免 I2S 欠载。

## 连接恢复

- WebSocket 连接和上游 StepFun 会话均设置 10 秒连接超时。
- 设备每 15 秒发送心跳，连续 2 次无响应则断开。
- 重连退避为 1、2、4、8、16、30 秒，成功连接后清零。
- 断线时停止录音上传和播放，清空本轮音频缓冲，避免重复播放残留音频。
- 上游失败由后端返回可重试错误；设备保持可操作并显示离线状态。
- 不离线缓存语音，防止恢复后提交过期对话。

## 模块边界

- `server/realtime.py`：StepFun 协议适配与单会话生命周期。
- `server/app.py`：HTTP 与设备 WebSocket 入口，不包含 StepFun 事件细节。
- `src/VoiceAssistant.*`：设备状态机、录音和控制事件。
- 新增设备 WebSocket 客户端模块：连接、帧收发、心跳和重连。
- 新增 PCM 播放缓冲模块或扩展 `AudioPlayer`：只负责 PCM 队列与 I2S 输出。

业务层不直接操作 WebSocket 帧或 I2S 寄存器。播放器命令继续通过 `MusicService` 和 `AudioPlayer` 的公开接口执行。

## 资源预算

- 上行带宽：约 32 KiB/s，不含 WebSocket 开销。
- 下行带宽：约 32 KiB/s。
- 音频播放环形缓冲：128 KiB PSRAM。
- 网络收发临时缓冲：每方向不超过 8 KiB。
- 语音任务栈：先维持 12 KiB，通过运行时高水位测量再调整。
- CPU：I2S DMA 与网络任务分离；播放期间不进行 MP3 解码。

## 验证标准

1. 安静环境下说话后无需按键，VAD 自动结束本轮并开始回答。
2. 首包回答音频延迟可记录，目标局域网环境小于 2 秒。
3. 回答音频连续播放，无明显断音；播放期间不上传麦克风音频。
4. 回答播放完成后 300 ms 内恢复监听。
5. 下一首、暂停和音量命令能正确执行且不会重复执行。
6. WiFi、后端和 StepFun 任一连接中断后可自动重连，不播放残留音频。
7. 连续对话 30 分钟无内存持续下降、任务栈溢出或看门狗复位。
8. StepFun 不可用时，原 HTTP 录音接口仍可独立启动和调用。

