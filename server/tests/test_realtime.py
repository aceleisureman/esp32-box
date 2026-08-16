import base64
import json
import queue
import threading
import time
import unittest
from unittest.mock import patch

from app import app

from realtime import (
    DEVICE_AUDIO_CHUNK_BYTES,
    MAX_DEVICE_AUDIO_BYTES,
    StepFunRealtimeSession,
    build_session_update,
    device_audio_to_stepfun,
    stepfun_event_to_device,
)


class RealtimeProtocolTest(unittest.TestCase):
    def test_session_uses_pcm16_server_vad(self):
        event = build_session_update("wenrounansheng")

        self.assertEqual(event["type"], "session.update")
        self.assertEqual(event["session"]["input_audio_format"], "pcm16")
        self.assertEqual(event["session"]["output_audio_format"], "pcm16")
        self.assertEqual(
            event["session"]["turn_detection"]["type"],
            "server_vad",
        )

    def test_audio_frame_is_base64_append(self):
        event = device_audio_to_stepfun(b"\x01\x02")

        self.assertEqual(
            event,
            {"type": "input_audio_buffer.append", "audio": "AQI="},
        )

    def test_audio_delta_becomes_binary(self):
        kind, payload = stepfun_event_to_device(
            {"type": "response.audio.delta", "delta": "AQI="}
        )

        self.assertEqual(kind, "binary")
        self.assertEqual(payload, b"\x01\x02")

    def test_session_created_is_not_ready(self):
        self.assertIsNone(
            stepfun_event_to_device({"type": "session.created"})
        )

    def test_session_updated_becomes_ready(self):
        self.assertEqual(
            stepfun_event_to_device({"type": "session.updated"}),
            ("json", {"type": "session.ready"}),
        )

    def test_speech_events_are_mapped(self):
        self.assertEqual(
            stepfun_event_to_device(
                {"type": "input_audio_buffer.speech_started"}
            ),
            ("json", {"type": "speech.started"}),
        )
        self.assertEqual(
            stepfun_event_to_device(
                {"type": "input_audio_buffer.speech_stopped"}
            ),
            ("json", {"type": "speech.stopped"}),
        )

    def test_final_transcripts_are_mapped(self):
        self.assertEqual(
            stepfun_event_to_device(
                {
                    "type": "conversation.item.input_audio_transcription.completed",
                    "transcript": "你好",
                }
            ),
            ("json", {"type": "transcript.user", "text": "你好"}),
        )
        self.assertEqual(
            stepfun_event_to_device(
                {"type": "response.audio_transcript.done", "transcript": "您好"}
            ),
            ("json", {"type": "transcript.assistant", "text": "您好"}),
        )

    def test_audio_done_and_error_are_mapped(self):
        self.assertEqual(
            stepfun_event_to_device({"type": "response.audio.done"}),
            ("json", {"type": "audio.done"}),
        )
        self.assertEqual(
            stepfun_event_to_device(
                {
                    "type": "error",
                    "error": {"code": "bad_request", "message": "错误"},
                }
            ),
            (
                "json",
                {
                    "type": "error",
                    "code": "bad_request",
                    "message": "错误",
                    "retryable": False,
                },
            ),
        )

    def test_invalid_audio_base64_becomes_protocol_error(self):
        self.assertEqual(
            stepfun_event_to_device(
                {"type": "response.audio.delta", "delta": "%%%"}
            ),
            (
                "json",
                {
                    "type": "protocol_error",
                    "code": "invalid_audio_base64",
                    "message": "上游音频数据不是有效的 Base64",
                },
            ),
        )

    @patch.dict("os.environ", {"STEPFUN_VAD_SILENCE_MS": "900"})
    def test_session_configuration_uses_environment(self):
        event = build_session_update("wenrounansheng")

        self.assertEqual(event["session"]["modalities"], ["text", "audio"])
        self.assertEqual(event["session"]["voice"], "wenrounansheng")
        self.assertEqual(
            event["session"]["turn_detection"]["silence_duration_ms"], 900
        )


class FakeSocket:
    def __init__(self, frames=()):
        self.frames = iter(frames)
        self.sent = []
        self.closed = False

    def recv(self):
        try:
            return next(self.frames)
        except StopIteration:
            return None

    def send(self, payload):
        self.sent.append(payload)

    def close(self):
        self.closed = True


class BlockingSocket:
    CLOSED = object()

    def __init__(self, frames=()):
        self.frames = queue.Queue()
        for frame in frames:
            self.frames.put(frame)
        self.sent = []
        self.closed = False

    def recv(self):
        frame = self.frames.get(timeout=2)
        if frame is self.CLOSED:
            return None
        return frame

    def send(self, payload):
        self.sent.append(payload)

    def close(self):
        if not self.closed:
            self.closed = True
            self.frames.put(self.CLOSED)


class FailingSendSocket(BlockingSocket):
    def send(self, payload):
        raise RuntimeError("send failed")


class FailingUpstreamSendSocket(BlockingSocket):
    def send(self, payload):
        raise RuntimeError("upstream send failed")


class BlockingSendSocket(BlockingSocket):
    def __init__(self):
        super().__init__()
        self.send_started = threading.Event()
        self.release_send = threading.Event()

    def send(self, payload):
        self.send_started.set()
        self.release_send.wait(timeout=2)
        self.sent.append(payload)


class CloseRaceSendSocket(BlockingSendSocket):
    def __init__(self):
        super().__init__()
        self.socket_closed = False

    def close(self):
        self.socket_closed = True
        super().close()

    def send(self, payload):
        self.send_started.set()
        self.release_send.wait(timeout=2)
        if self.socket_closed:
            raise ConnectionError("socket closed")
        self.sent.append(payload)


class StuckSocket(FakeSocket):
    def __init__(self):
        super().__init__()
        self.release = threading.Event()

    def recv(self):
        self.release.wait()
        return None


class ConcurrentSendDetector(BlockingSocket):
    def __init__(self):
        super().__init__()
        self.first_send_started = threading.Event()
        self.release_first_send = threading.Event()
        self._active = 0
        self.concurrent_send = False
        self._state_lock = threading.Lock()

    def recv(self):
        if not self.first_send_started.wait(timeout=2):
            raise RuntimeError("first send did not start")
        return json.dumps({"type": "ping"})

    def send(self, payload):
        with self._state_lock:
            self._active += 1
            if self._active > 1:
                self.concurrent_send = True
            first = not self.first_send_started.is_set()
            if first:
                self.first_send_started.set()
        if first:
            self.release_first_send.wait(timeout=2)
        with self._state_lock:
            self.sent.append(payload)
            self._active -= 1


class RealtimeSessionTest(unittest.TestCase):
    def make_session(self, frames):
        device = FakeSocket(frames)
        upstream = FakeSocket()
        session = StepFunRealtimeSession(
            device, "secret", "stepaudio-2.5-realtime", "wenrounansheng",
            connect_factory=lambda *args, **kwargs: upstream,
        )
        return session, device, upstream

    def test_rejects_oversized_device_audio_frame(self):
        session, device, upstream = self.make_session(
            [b"x" * (MAX_DEVICE_AUDIO_BYTES + 1)]
        )

        session.run()

        messages = [json.loads(value) for value in device.sent]
        self.assertIn(
            {
                "type": "protocol_error",
                "code": "audio_frame_too_large",
                "message": "音频帧不能超过 4096 字节",
            },
            messages,
        )
        self.assertFalse(
            any("input_audio_buffer.append" in value for value in upstream.sent)
        )

    def test_rejects_empty_and_unaligned_audio_frames(self):
        session, device, upstream = self.make_session([b"", b"x"])

        session.run()

        messages = [json.loads(value) for value in device.sent]
        self.assertIn(
            {
                "type": "protocol_error",
                "code": "empty_audio_frame",
                "message": "音频帧不能为空",
            },
            messages,
        )
        self.assertIn(
            {
                "type": "protocol_error",
                "code": "unaligned_audio_frame",
                "message": "PCM16 音频帧必须是偶数字节",
            },
            messages,
        )
        self.assertEqual(upstream.sent, [])

    def test_large_audio_delta_is_chunked_for_device(self):
        pcm = b"\x01\x02" * 10241  # 20482 字节，超过设备端 15KB 单帧上限
        device = BlockingSocket()
        upstream = FakeSocket([
            json.dumps({
                "type": "response.audio.delta",
                "delta": base64.b64encode(pcm).decode("ascii"),
            })
        ])
        session = StepFunRealtimeSession(
            device, "secret", "stepaudio-2.5-realtime", "wenrounansheng",
            connect_factory=lambda *args, **kwargs: upstream,
        )

        session.run()

        binary_frames = [
            frame for frame in device.sent if isinstance(frame, bytes)
        ]
        self.assertEqual(
            [len(frame) for frame in binary_frames],
            [DEVICE_AUDIO_CHUNK_BYTES] * 5 + [2],
        )
        self.assertEqual(b"".join(binary_frames), pcm)

    def test_session_updated_triggers_wake_greeting_once(self):
        device = BlockingSocket()
        upstream = FakeSocket([
            json.dumps({"type": "session.updated"}),
            json.dumps({"type": "session.updated"}),
        ])
        session = StepFunRealtimeSession(
            device, "secret", "stepaudio-2.5-realtime", "wenrounansheng",
            connect_factory=lambda *args, **kwargs: upstream,
        )

        session.run()

        greetings = [
            json.loads(value) for value in upstream.sent
            if json.loads(value).get("type") == "response.create"
        ]
        self.assertEqual(len(greetings), 1)
        self.assertEqual(
            greetings[0]["response"]["modalities"], ["text", "audio"]
        )
        self.assertTrue(greetings[0]["response"]["instructions"])

    @patch.dict("os.environ", {"STEPFUN_GREETING_PROMPT": "  "})
    def test_wake_greeting_disabled_by_empty_prompt(self):
        device = BlockingSocket()
        upstream = FakeSocket([json.dumps({"type": "session.updated"})])
        session = StepFunRealtimeSession(
            device, "secret", "stepaudio-2.5-realtime", "wenrounansheng",
            connect_factory=lambda *args, **kwargs: upstream,
        )

        session.run()

        self.assertEqual(
            [value for value in upstream.sent
             if json.loads(value).get("type") == "response.create"],
            [],
        )

    def test_accepts_only_whitelisted_text_events(self):
        frames = [
            json.dumps({"type": "session.start"}),
            json.dumps({"type": "playback.done"}),
            json.dumps({"type": "ping"}),
            json.dumps({"type": "not.allowed"}),
        ]
        device = FakeSocket(frames)
        upstream = BlockingSocket()
        session = StepFunRealtimeSession(
            device, "secret", "stepaudio-2.5-realtime", "wenrounansheng",
            connect_factory=lambda *args, **kwargs: upstream,
        )

        session.run()

        upstream_events = [json.loads(value) for value in upstream.sent]
        self.assertEqual(
            upstream_events,
            [
                build_session_update("wenrounansheng"),
                {"type": "input_audio_buffer.clear"},
            ],
        )
        device_events = [json.loads(value) for value in device.sent]
        self.assertIn({"type": "pong"}, device_events)
        self.assertIn(
            {
                "type": "protocol_error",
                "code": "unsupported_event",
                "message": "不支持的设备事件: not.allowed",
            },
            device_events,
        )

    @patch.dict(
        "os.environ",
        {
            "STEPFUN_REALTIME_MODEL": "custom-model",
            "STEPFUN_REALTIME_VOICE": "custom-voice",
        },
    )
    def test_from_env_reads_model_and_voice(self):
        device = FakeSocket()

        session = StepFunRealtimeSession.from_env(device, "secret")

        self.assertEqual(session.model, "custom-model")
        self.assertEqual(session.voice, "custom-voice")

    def test_non_object_json_returns_protocol_error_and_continues(self):
        session, device, upstream = self.make_session(
            [json.dumps([]), json.dumps({"type": "ping"})]
        )

        session.run()

        messages = [json.loads(value) for value in device.sent]
        self.assertIn(
            {
                "type": "protocol_error",
                "code": "invalid_event",
                "message": "设备事件必须是 JSON 对象",
            },
            messages,
        )
        self.assertIn({"type": "pong"}, messages)

    def test_close_unblocks_both_receiver_threads(self):
        device = BlockingSocket()
        upstream = BlockingSocket()
        session = StepFunRealtimeSession(
            device,
            "secret",
            "stepaudio-2.5-realtime",
            "wenrounansheng",
            connect_factory=lambda *args, **kwargs: upstream,
        )
        completed = threading.Event()
        runner = threading.Thread(
            target=lambda: (session.run(), completed.set()), daemon=True
        )
        runner.start()
        time.sleep(0.02)

        session.close()

        self.assertTrue(completed.wait(timeout=1))
        self.assertTrue(device.closed)
        self.assertTrue(upstream.closed)

    def test_close_does_not_wait_for_blocking_device_send(self):
        device = BlockingSendSocket()
        session = StepFunRealtimeSession(
            device,
            "secret",
            "stepaudio-2.5-realtime",
            "wenrounansheng",
            connect_factory=lambda *args, **kwargs: BlockingSocket(),
        )
        sender = threading.Thread(
            target=lambda: session._send_device(b"audio"), daemon=True
        )
        sender.start()
        self.assertTrue(device.send_started.wait(timeout=1))

        closer = threading.Thread(target=session.close, daemon=True)
        closer.start()
        closer.join(timeout=0.2)

        self.assertFalse(closer.is_alive())
        device.release_send.set()
        sender.join(timeout=1)

    def test_send_closed_by_concurrent_close_is_normal_shutdown(self):
        device = CloseRaceSendSocket()
        session = StepFunRealtimeSession(
            device,
            "secret",
            "stepaudio-2.5-realtime",
            "wenrounansheng",
            connect_factory=lambda *args, **kwargs: BlockingSocket(),
        )
        sender_error = []

        def send():
            try:
                session._send_device(b"audio")
            except Exception as error:
                sender_error.append(error)

        sender = threading.Thread(target=send, daemon=True)
        sender.start()
        self.assertTrue(device.send_started.wait(timeout=1))
        session.close()
        device.release_send.set()
        sender.join(timeout=1)

        self.assertFalse(sender.is_alive())
        self.assertEqual(sender_error, [])

    def test_send_failure_is_reported_and_reraised(self):
        device = FailingSendSocket()
        upstream = BlockingSocket(
            [json.dumps({"type": "session.updated"})]
        )
        session = StepFunRealtimeSession(
            device,
            "secret",
            "stepaudio-2.5-realtime",
            "wenrounansheng",
            connect_factory=lambda *args, **kwargs: upstream,
        )

        with self.assertRaisesRegex(RuntimeError, "send failed"):
            session.run()

        self.assertTrue(device.closed)
        self.assertTrue(upstream.closed)

    def test_thread_failure_reports_retryable_internal_error(self):
        device = BlockingSocket([b"\x00\x00"])
        upstream = FailingUpstreamSendSocket()
        session = StepFunRealtimeSession(
            device,
            "secret",
            "stepaudio-2.5-realtime",
            "wenrounansheng",
            connect_factory=lambda *args, **kwargs: upstream,
        )

        with self.assertRaisesRegex(RuntimeError, "upstream send failed"):
            session.run()

        messages = [json.loads(value) for value in device.sent]
        self.assertIn(
            {
                "type": "error",
                "code": "internal_error",
                "message": "实时会话内部错误",
                "retryable": True,
            },
            messages,
        )

    def test_device_sends_are_serialized(self):
        device = ConcurrentSendDetector()

        class OneEventThenStopSocket(BlockingSocket):
            def recv(inner_self):
                if not inner_self.sent:
                    inner_self.sent.append("received")
                    return json.dumps({"type": "session.updated"})
                device.release_first_send.wait(timeout=2)
                return None

        upstream = OneEventThenStopSocket()
        session = StepFunRealtimeSession(
            device,
            "secret",
            "stepaudio-2.5-realtime",
            "wenrounansheng",
            connect_factory=lambda *args, **kwargs: upstream,
        )
        runner = threading.Thread(target=session.run, daemon=True)
        runner.start()
        self.assertTrue(device.first_send_started.wait(timeout=1))
        time.sleep(0.05)
        device.release_first_send.set()
        runner.join(timeout=1)

        self.assertFalse(runner.is_alive())
        self.assertFalse(device.concurrent_send)

    @patch("realtime.THREAD_JOIN_TIMEOUT_SECONDS", 0.05)
    def test_join_timeout_raises_runtime_error(self):
        device = StuckSocket()
        upstream = FakeSocket()
        session = StepFunRealtimeSession(
            device,
            "secret",
            "stepaudio-2.5-realtime",
            "wenrounansheng",
            connect_factory=lambda *args, **kwargs: upstream,
        )

        try:
            with self.assertRaisesRegex(RuntimeError, "转发线程未在期限内退出"):
                session.run()
        finally:
            device.release.set()


class RealtimeRouteTest(unittest.TestCase):
    @staticmethod
    def call_route(ws):
        return app.view_functions["api_voice_realtime"].__wrapped__(ws)

    def test_health_remains_available(self):
        response = app.test_client().get("/health")
        self.assertEqual(response.status_code, 200)

    def test_missing_api_key_returns_non_retryable_error(self):
        fake_ws = FakeSocket()
        with patch.dict("os.environ", {}, clear=True):
            self.call_route(fake_ws)

        self.assertEqual(
            json.loads(fake_ws.sent[0]),
            {
                "type": "error",
                "code": "missing_api_key",
                "message": "STEPFUN_API_KEY 未配置",
                "retryable": False,
            },
        )

    def test_configured_api_key_runs_realtime_session(self):
        fake_ws = FakeSocket()
        with patch.dict("os.environ", {"STEPFUN_API_KEY": "secret"}):
            with patch("app.realtime.StepFunRealtimeSession.from_env") as factory:
                session = factory.return_value
                self.call_route(fake_ws)

        factory.assert_called_once_with(fake_ws, "secret")
        session.run.assert_called_once_with()

    def test_session_failure_returns_retryable_internal_error(self):
        fake_ws = FakeSocket()
        with patch.dict("os.environ", {"STEPFUN_API_KEY": "secret"}):
            with patch("app.realtime.StepFunRealtimeSession.from_env") as factory:
                factory.return_value.run.side_effect = RuntimeError("failed")
                self.call_route(fake_ws)

        self.assertEqual(
            json.loads(fake_ws.sent[-1]),
            {
                "type": "error",
                "code": "internal_error",
                "message": "实时会话内部错误",
                "retryable": True,
            },
        )

    def test_closed_socket_does_not_propagate_error_send_failure(self):
        fake_ws = FailingSendSocket()
        with patch.dict("os.environ", {}, clear=True):
            self.call_route(fake_ws)


if __name__ == "__main__":
    unittest.main()
