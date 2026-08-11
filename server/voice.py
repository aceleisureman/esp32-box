"""语音助手编排：录音 → ASR → 指令识别/LLM → TTS → 音频直链。"""
from __future__ import annotations

import hashlib
import os
import uuid
from typing import Any, Optional

import asr
import commands
import llm
import tts

TTS_CACHE_DIR = os.path.join(os.path.dirname(__file__), "tts_cache")


def _lan_ip() -> str:
    """探测本机局域网地址，供设备拉流 TTS 音频。"""
    configured = os.environ.get("LAN_IP", "").strip()
    if configured:
        return configured
    import socket

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))  # 不真正发包，仅取本机出网 IP
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def handle_voice_audio(wav_path: str, base_url: str) -> dict[str, Any]:
    """处理一次语音请求，返回给设备的 JSON。"""
    os.makedirs(TTS_CACHE_DIR, exist_ok=True)

    # 1. 录音 → 文字
    asr_backend = asr.get_backend()
    user_text = asr_backend.transcribe(wav_path).strip()

    # 2. 播放器指令：规则优先，未命中交给 LLM
    command: Optional[dict[str, Any]] = commands.match_rule(user_text)
    if command is None:
        command = llm.get_llm().classify_command(user_text)

    # 3. 回答文本：指令命中则给确认回复，否则走 LLM 对话
    if command is not None:
        reply_text = commands.confirm_reply(command)
    else:
        reply_text = llm.get_llm().chat(user_text)

    # 4. TTS → MP3，生成设备可拉取的 http 直链
    audio_url: Optional[str] = None
    if reply_text:
        digest = hashlib.sha1(
            (reply_text + uuid.uuid4().hex).encode("utf-8")
        ).hexdigest()[:12]
        out_path = os.path.join(TTS_CACHE_DIR, f"{digest}.mp3")
        if tts.get_backend().synthesize(reply_text, out_path):
            port = os.environ.get("PORT", "5000")
            audio_url = f"{base_url}/tts_cache/{os.path.basename(out_path)}"

    return {
        "text": reply_text,
        "user_text": user_text,
        "audio_url": audio_url,
        "command": command,
    }
