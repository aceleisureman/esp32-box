"""语音识别（ASR）可插拔接口。

设备录音（16kHz 16bit PCM/WAV）→ 后端 → 文字。

具体厂商接入方式：
  1. 在 asr_impls 下新建模块，实现 transcribe(wav_path) -> str
  2. 在 get_backend() 的 BACKENDS 字典里注册 backend 名
  3. 在 .env 里设 ASR_BACKEND=<名字> 并填对应密钥

当前实现：
  - mock：不做真实识别，返回固定文本，用于跑通全链路
  - aliyun / xfyun / baidu / openai：占位，待确认厂商后实现
"""
from __future__ import annotations

import os
from abc import ABC, abstractmethod


class AsrBackend(ABC):
    """ASR 后端基类。transcribe 输入录音文件路径，输出识别文字。"""

    @abstractmethod
    def transcribe(self, wav_path: str) -> str:
        raise NotImplementedError


class MockAsr(AsrBackend):
    """占位实现：不联网，返回可配置的固定文本。

    供开发阶段跑通「录音上传 → 后端 → LLM → TTS」链路，
    也方便在没有麦克风/ASR 密钥时先验证后端逻辑。
    """

    def transcribe(self, wav_path: str) -> str:
        # 便于本地测试指令识别：返回一句带指令的话
        return os.environ.get("ASR_MOCK_TEXT", "下一首")


# ---- 厂商实现占位（待确认后填充）----
# class AliyunAsr(AsrBackend):
#     def transcribe(self, wav_path: str) -> str:
#         # 阿里云智能语音交互：一句话识别（RESTful）
#         # https://help.aliyun.com/document_detail/324265.html
#         raise NotImplementedError
#
# class XfyunAsr(AsrBackend):
#     def transcribe(self, wav_path: str) -> str:
#         # 讯飞短语音识别（WebSocket）
#         raise NotImplementedError
#
# class BaiduAsr(AsrBackend):
#     def transcribe(self, wav_path: str) -> str:
#         # 百度短语音识别极速版
#         raise NotImplementedError
#
# class OpenAIAsr(AsrBackend):
#     def transcribe(self, wav_path: str) -> str:
#         # OpenAI Whisper（files API）
#         raise NotImplementedError


BACKENDS: dict[str, type[AsrBackend]] = {
    "mock": MockAsr,
    # "aliyun": AliyunAsr,
    # "xfyun": XfyunAsr,
    # "baidu": BaiduAsr,
    # "openai": OpenAIAsr,
}


def get_backend() -> AsrBackend:
    name = os.environ.get("ASR_BACKEND", "mock").lower()
    cls = BACKENDS.get(name)
    if cls is None:
        raise RuntimeError(f"未知的 ASR_BACKEND: {name!r}，可用: {list(BACKENDS)}")
    return cls()
