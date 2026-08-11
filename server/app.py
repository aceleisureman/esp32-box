"""语音助手后端 Flask 入口。

运行：  cd server && pip install -r requirements.txt && python app.py

接口：
  POST /api/voice/audio   上传录音（multipart 字段 audio，WAV/PCM），
                          返回 {text, audio_url, command}
  GET  /tts_cache/<id>.mp3 设备拉流播放 TTS 音频（明文 HTTP，局域网内）
  GET  /health            健康检查
"""
from __future__ import annotations

import os
import json
import logging
from pathlib import Path

from dotenv import load_dotenv
from flask import Flask, jsonify, request, send_from_directory
from flask_sock import Sock

import realtime
import voice

load_dotenv(override=True)
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

app = Flask(__name__)
sock = Sock(app)

TTS_CACHE_DIR = os.path.join(os.path.dirname(__file__), "tts_cache")
os.makedirs(TTS_CACHE_DIR, exist_ok=True)


@app.get("/health")
def health():
    return jsonify({"ok": True})


def _send_realtime_error(ws, code: str, message: str, retryable: bool) -> None:
    try:
        ws.send(json.dumps({
            "type": "error",
            "code": code,
            "message": message,
            "retryable": retryable,
        }, ensure_ascii=False))
    except Exception:  # WebSocket may already be closed.
        pass


@sock.route("/api/voice/realtime")
def api_voice_realtime(ws):
    api_key = os.environ.get("STEPFUN_API_KEY", "").strip()
    if not api_key:
        _send_realtime_error(ws, "missing_api_key", "STEPFUN_API_KEY 未配置", False)
        return

    try:
        realtime.StepFunRealtimeSession.from_env(ws, api_key).run()
    except Exception:  # noqa: BLE001 - keep WebSocket failures device-readable.
        app.logger.exception("realtime session failed")
        _send_realtime_error(
            ws, "internal_error", "实时会话内部错误", True
        )


@app.post("/api/voice/audio")
def api_voice_audio():
    if "audio" not in request.files:
        return jsonify({"error": "缺少 audio 字段（multipart/form-data）"}), 400

    audio_file = request.files["audio"]
    save_path = os.path.join(TTS_CACHE_DIR, "upload_tmp.wav")
    audio_file.save(save_path)

    # 用请求的 Host 作为音频直链的 base（设备用 http://<server-ip>:port 访问）
    base_url = f"http://{request.host}"
    try:
        result = voice.handle_voice_audio(save_path, base_url)
        return jsonify(result)
    except Exception as exc:  # noqa: BLE001 —— 后端错误要回给设备可读信息
        app.logger.exception("voice handling failed")
        return jsonify({"error": str(exc)}), 500


@app.get("/tts_cache/<path:filename>")
def tts_cache(filename: str):
    return send_from_directory(Path(TTS_CACHE_DIR).resolve(), filename)


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "5000"))
    host = os.environ.get("HOST", "0.0.0.0")
    app.run(host=host, port=port, debug=False)
