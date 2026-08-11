"""语音合成（TTS）可插拔接口。

回答文本 → MP3 文件。默认后端 stepfun（待确认 StepFun 是否有 TTS 接口），
可选 edge（微软 Edge TTS，免费、中文自然、无需密钥），mock 用于开发链路。

配置（.env）：
  TTS_BACKEND=stepfun | edge | mock
  TTS_VOICE=zh-CN-XiaoxiaoNeural   （edge 用）
"""
from __future__ import annotations

import os
from abc import ABC, abstractmethod
from typing import Optional

import requests


class TtsBackend(ABC):
    @abstractmethod
    def synthesize(self, text: str, out_path: str) -> Optional[str]:
        """把 text 合成语音写入 out_path，返回实际文件路径（或 None=失败）。"""
        raise NotImplementedError


class MockTts(TtsBackend):
    """占位：不合成语音，返回 None，链路返回 text + command（无 audio_url）。

    用于开发阶段验证「录音 → ASR → 指令识别 → LLM」编排逻辑；
    真实 TTS 接入后不再使用。
    """

    def synthesize(self, text: str, out_path: str) -> Optional[str]:
        return None


class EdgeTts(TtsBackend):
    """微软 Edge TTS：免费、中文自然，无需 API key。

    依赖 edge-tts 库（未加入 requirements，启用时自行安装：
    pip install edge-tts）。
    """

    def synthesize(self, text: str, out_path: str) -> Optional[str]:
        import edge_tts  # 延迟导入，未安装时才报错

        voice = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
        communicate = edge_tts.Communicate(text, voice)
        import asyncio

        asyncio.run(communicate.save(out_path))
        return out_path


class StepFunTts(TtsBackend):
    """阶跃星辰 TTS（待确认接口存在性与调用方式）。"""

    def synthesize(self, text: str, out_path: str) -> Optional[str]:
        # 待确认：StepFun 是否有 /audio/speech 之类的 TTS 端点。
        # 若有（OpenAI 兼容），大致为：
        #   POST {base}/audio/speech
        #   {"model": ..., "input": text, "voice": ...} → 二进制音频
        api_key = os.environ.get("STEPFUN_API_KEY", "")
        if not api_key:
            return None
        base_url = os.environ.get(
            "STEPFUN_BASE_URL", "https://api.stepfun.com/step_plan/v1"
        ).rstrip("/")
        model = os.environ.get("STEPFUN_TTS_MODEL", "stepaudio-2.5-tts")
        voice = os.environ.get("STEPFUN_TTS_VOICE", "qingchunshaonv")
        if not model:
            raise RuntimeError(
                "STEPFUN_TTS_MODEL 未配置，且 StepFun TTS 接口细节待确认。"
                "可先用 TTS_BACKEND=edge 或 mock。"
            )
        resp = requests.post(
            f"{base_url}/audio/speech",
            headers={"Authorization": f"Bearer {api_key}"},
            json={"model": model, "input": text, "voice": voice},
            timeout=60,
        )
        resp.raise_for_status()
        with open(out_path, "wb") as f:
            f.write(resp.content)
        return out_path


BACKENDS: dict[str, type[TtsBackend]] = {
    "stepfun": StepFunTts,
    "edge": EdgeTts,
    "mock": MockTts,
}


def get_backend() -> TtsBackend:
    name = os.environ.get("TTS_BACKEND", "stepfun").lower()
    cls = BACKENDS.get(name)
    if cls is None:
        raise RuntimeError(f"未知的 TTS_BACKEND: {name!r}，可用: {list(BACKENDS)}")
    return cls()
