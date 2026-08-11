import base64
import binascii
import json
import logging
import os
import threading
import time
from typing import Callable, Optional

import commands
import memory


logger = logging.getLogger(__name__)


MAX_DEVICE_AUDIO_BYTES = 4096
THREAD_JOIN_TIMEOUT_SECONDS = 2.0
DEFAULT_MODEL = "stepaudio-2.5-realtime"
DEFAULT_VOICE = "qingchunshaonv"
REALTIME_URL = "wss://api.stepfun.com/step_plan/v1/realtime?model={model}"


def build_session_update(voice: str, device_id: str = "") -> dict:
    """构造 session.update，instructions 注入情感人设 + 该设备记忆。

    device_id 用于隔离每台设备的记忆（memory.py），设备间互不干扰。
    """
    try:
        silence_ms = int(os.environ.get("STEPFUN_VAD_SILENCE_MS", "800"))
    except ValueError:
        silence_ms = 800

    instructions = memory.build_instructions(device_id)

    return {
        "type": "session.update",
        "session": {
            "modalities": ["text", "audio"],
            "instructions": instructions,
            "voice": voice,
            "input_audio_format": "pcm16",
            "output_audio_format": "pcm16",
            "turn_detection": {
                "type": "server_vad",
                "silence_duration_ms": silence_ms,
            },
        },
    }


def device_audio_to_stepfun(pcm: bytes) -> dict:
    return {
        "type": "input_audio_buffer.append",
        "audio": base64.b64encode(pcm).decode("ascii"),
    }


def _protocol_error(code: str, message: str) -> tuple[str, dict]:
    return "json", {"type": "protocol_error", "code": code, "message": message}


def stepfun_event_to_device(event: dict):
    event_type = event.get("type")
    # session.created 只代表 WebSocket 会话建立；设备必须等配置真正
    # 生效后再开麦，否则首轮录音可能使用默认音色/VAD 参数。
    if event_type == "session.updated":
        return "json", {"type": "session.ready"}
    if event_type == "input_audio_buffer.speech_started":
        return "json", {"type": "speech.started"}
    if event_type == "input_audio_buffer.speech_stopped":
        return "json", {"type": "speech.stopped"}
    if event_type == "conversation.item.input_audio_transcription.completed":
        return "json", {
            "type": "transcript.user",
            "text": event.get("transcript", ""),
        }
    if event_type in ("response.audio_transcript.done", "response.output_text.done"):
        return "json", {
            "type": "transcript.assistant",
            "text": event.get("transcript", event.get("text", "")),
        }
    if event_type == "response.audio.delta":
        try:
            audio = base64.b64decode(event.get("delta", ""), validate=True)
        except (binascii.Error, ValueError, TypeError):
            return _protocol_error(
                "invalid_audio_base64", "上游音频数据不是有效的 Base64"
            )
        return "binary", audio
    if event_type == "response.audio.done":
        return "json", {"type": "audio.done"}
    if event_type == "error":
        error = event.get("error") or {}
        return "json", {
            "type": "error",
            "code": error.get("code", "upstream_error"),
            "message": error.get("message", "StepFun 实时服务错误"),
            "retryable": False,
        }
    return None


class StepFunRealtimeSession:
    def __init__(
        self,
        device_socket,
        api_key: str,
        model: str,
        voice: str,
        connect_factory: Optional[Callable] = None,
    ):
        self.device_socket = device_socket
        self.api_key = api_key
        self.model = model
        self.voice = voice
        self._connect_factory = connect_factory
        self._upstream = None
        self._stop = threading.Event()
        self._close_lock = threading.Lock()
        self._device_send_lock = threading.Lock()
        self._failure_lock = threading.Lock()
        self._failure = None
        self._closed = False
        self._audio_frames = 0
        self._audio_bytes = 0
        self._audio_log_at = time.monotonic()
        self._device_id = ""           # session.start 时确定，用于记忆隔离
        self._last_user_turn = ""      # 当前轮用户输入（等待回复后落库）
        self._reply_pending = False    # 回复是否完成、需写入记忆

    @classmethod
    def from_env(cls, device_socket, api_key: str):
        return cls(
            device_socket,
            api_key,
            os.environ.get("STEPFUN_REALTIME_MODEL", DEFAULT_MODEL),
            os.environ.get("STEPFUN_REALTIME_VOICE", DEFAULT_VOICE),
        )

    def _connect_upstream(self):
        url = REALTIME_URL.format(model=self.model)
        headers = {"Authorization": f"Bearer {self.api_key}"}
        logger.info("[REALTIME] connecting model=%s voice=%s", self.model, self.voice)
        if self._connect_factory is not None:
            return self._connect_factory(url, additional_headers=headers)

        from websockets.sync.client import connect

        try:
            return connect(url, additional_headers=headers)
        except TypeError as error:
            if "additional_headers" not in str(error):
                raise
            return connect(url, extra_headers=headers)

    def _send_device_json(self, event: dict) -> None:
        self._send_device(json.dumps(event, ensure_ascii=False))

    def _send_device(self, payload) -> None:
        with self._device_send_lock:
            with self._close_lock:
                if self._closed:
                    raise RuntimeError("设备连接已关闭")
            try:
                self.device_socket.send(payload)
            except OSError:
                with self._close_lock:
                    if self._closed:
                        return
                raise

    def _record_worker_failure(self, error: Exception) -> None:
        with self._failure_lock:
            if self._failure is not None:
                return
            self._failure = error
        try:
            self._send_device_json(
                {
                    "type": "error",
                    "code": "internal_error",
                    "message": "实时会话内部错误",
                    "retryable": True,
                }
            )
        except Exception:
            pass

    @staticmethod
    def _is_normal_disconnect(error: Exception) -> bool:
        """Flask-Sock 1000/1001 close is a normal device lifecycle event."""
        name = type(error).__name__
        code = getattr(error, "code", None)
        return name == "ConnectionClosed" and code in (1000, 1001)

    def _device_to_upstream(self) -> None:
        try:
            while not self._stop.is_set():
                receive = getattr(self.device_socket, "receive", None)
                if receive is None:
                    receive = self.device_socket.recv
                frame = receive()
                if frame is None:
                    break
                if isinstance(frame, bytes):
                    if not frame:
                        self._send_device_json(
                            {
                                "type": "protocol_error",
                                "code": "empty_audio_frame",
                                "message": "音频帧不能为空",
                            }
                        )
                        continue
                    if len(frame) > MAX_DEVICE_AUDIO_BYTES:
                        self._send_device_json(
                            {
                                "type": "protocol_error",
                                "code": "audio_frame_too_large",
                                "message": "音频帧不能超过 4096 字节",
                            }
                        )
                        continue
                    if len(frame) % 2:
                        self._send_device_json(
                            {
                                "type": "protocol_error",
                                "code": "unaligned_audio_frame",
                                "message": "PCM16 音频帧必须是偶数字节",
                            }
                        )
                        continue
                    self._audio_frames += 1
                    self._audio_bytes += len(frame)
                    now = time.monotonic()
                    if now - self._audio_log_at >= 2.0:
                        samples = memoryview(frame).cast("h")
                        peak = max((abs(sample) for sample in samples), default=0)
                        rms = int(
                            (sum(sample * sample for sample in samples) /
                             max(1, len(samples))) ** 0.5
                        )
                        logger.info(
                            "[REALTIME] device audio frames=%d bytes=%d peak=%d rms=%d",
                            self._audio_frames, self._audio_bytes, peak, rms,
                        )
                        self._audio_log_at = now
                    self._upstream.send(json.dumps(device_audio_to_stepfun(frame)))
                    continue
                self._handle_device_text(frame)
        except Exception as error:
            if not self._stop.is_set() and not self._is_normal_disconnect(error):
                self._record_worker_failure(error)
        finally:
            self._stop.set()

    def _handle_device_text(self, frame: str) -> None:
        try:
            event = json.loads(frame)
        except (json.JSONDecodeError, TypeError):
            self._send_device_json(
                {
                    "type": "protocol_error",
                    "code": "invalid_json",
                    "message": "设备文本帧不是有效的 JSON",
                }
            )
            return

        if not isinstance(event, dict):
            self._send_device_json(
                {
                    "type": "protocol_error",
                    "code": "invalid_event",
                    "message": "设备事件必须是 JSON 对象",
                }
            )
            return

        event_type = event.get("type")
        if event_type == "session.start":
            self._device_id = str(event.get("device_id", ""))
            logger.info(
                "[REALTIME] device session.start id=%s",
                self._device_id or "-",
            )
            self._upstream.send(
                json.dumps(build_session_update(self.voice, self._device_id))
            )
        elif event_type == "playback.done":
            # 设备已播完上一轮，清掉可能残留的输入帧，下一轮 VAD
            # 从干净缓冲区开始。
            self._upstream.send(json.dumps({"type": "input_audio_buffer.clear"}))
            logger.info("[REALTIME] playback done; input buffer cleared")
            return
        elif event_type == "ping":
            self._send_device_json({"type": "pong"})
        else:
            self._send_device_json(
                {
                    "type": "protocol_error",
                    "code": "unsupported_event",
                    "message": f"不支持的设备事件: {event_type}",
                }
            )

    def _upstream_to_device(self) -> None:
        try:
            while not self._stop.is_set():
                frame = self._upstream.recv()
                if frame is None:
                    break
                try:
                    event = json.loads(frame)
                except (json.JSONDecodeError, TypeError):
                    self._send_device_json(
                        _protocol_error(
                            "invalid_upstream_json", "上游文本帧不是有效的 JSON"
                        )[1]
                    )
                    continue
                if not isinstance(event, dict):
                    self._send_device_json(
                        _protocol_error(
                            "invalid_upstream_event", "上游事件必须是 JSON 对象"
                        )[1]
                    )
                    continue
                event_type = event.get("type", "unknown")
                if event_type not in ("response.audio.delta",):
                    detail = ""
                    if event_type == "session.updated":
                        session = event.get("session") or {}
                        detail = f" voice={session.get('voice', '-')!r}"
                    if event_type == "conversation.item.input_audio_transcription.completed":
                        detail = f" transcript={event.get('transcript', '')!r}"
                    elif event_type == "error":
                        detail = f" error={event.get('error', {})!r}"
                    logger.info("[REALTIME] upstream event=%s%s", event_type, detail)
                mapped = stepfun_event_to_device(event)
                if mapped is None:
                    continue
                kind, payload = mapped
                if (kind == "json" and payload.get("type") == "transcript.user"):
                    # 记录用户输入到该设备记忆，并尝试匹配播放器指令
                    user_text = payload.get("text", "")
                    memory.append(self._device_id, "user", user_text)
                    self._last_user_turn = user_text
                    command = commands.match_rule(user_text)
                    if command is not None:
                        self._send_device_json({"type": "command", **command})
                if (kind == "json" and
                        payload.get("type") == "transcript.assistant"):
                    # 回复完成，把助手回答一并写入记忆（形成完整一轮）
                    reply_text = payload.get("text", "")
                    if reply_text:
                        memory.append(self._device_id, "assistant", reply_text)
                if kind == "binary":
                    self._send_device(payload)
                else:
                    self._send_device_json(payload)
        except Exception as error:
            if not self._stop.is_set():
                # 上游异常断线属于可重连故障，避免把正常网络抖动升级为
                # Flask 请求堆栈；设备会在下一轮连接中重新建立会话。
                print(f"[REALTIME] upstream disconnected: {type(error).__name__}")
                self._record_worker_failure(error)
        finally:
            self._stop.set()

    def run(self) -> None:
        self._upstream = self._connect_upstream()
        logger.info("[REALTIME] upstream connected")
        device_thread = threading.Thread(target=self._device_to_upstream, daemon=True)
        upstream_thread = threading.Thread(
            target=self._upstream_to_device, daemon=True
        )
        device_thread.start()
        upstream_thread.start()
        self._stop.wait()
        self.close()
        for thread in (device_thread, upstream_thread):
            thread.join(timeout=THREAD_JOIN_TIMEOUT_SECONDS)
        if device_thread.is_alive() or upstream_thread.is_alive():
            raise RuntimeError("转发线程未在期限内退出")
        logger.info(
            "[REALTIME] session closed audio_frames=%d audio_bytes=%d",
            self._audio_frames, self._audio_bytes,
        )
        with self._failure_lock:
            failure = self._failure
        if failure is not None:
            raise failure

    def close(self) -> None:
        with self._close_lock:
            if self._closed:
                return
            self._closed = True
            self._stop.set()
        if self._upstream is not None:
            try:
                self._upstream.close()
            except Exception:
                pass
        try:
            self.device_socket.close()
        except Exception:
            pass
