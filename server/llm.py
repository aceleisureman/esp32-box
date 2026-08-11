"""大模型对话（StepFun / OpenAI 兼容）客户端。

对接 https://platform.stepfun.com/docs/zh/api-reference/chat/chat-completion-create
（OpenAI 兼容格式，具体模型名/端点待确认，集中在此文件改）。

职责：
  1. 对话：给定用户文字与简短历史，生成回答
  2. 指令兜底：规则未命中时，问模型是否包含播放器指令

不接入任何密钥时（MOCK_LLM=1 或缺少 API key）走 mock 实现，
返回固定回答，便于先跑通后端链路。
"""
from __future__ import annotations

import json
import os
from typing import Any, Optional

import requests

SYSTEM_PROMPT = (
    "你是 ESP32 智能音箱的语音助手，用简洁的中文回答问题。"
    "回答控制在 1~2 句话以内，适合语音播报，不要用表情符号。"
)


class StepFunLLM:
    def __init__(self) -> None:
        self.api_key = os.environ.get("STEPFUN_API_KEY", "")
        self.base_url = os.environ.get(
            "STEPFUN_BASE_URL", "https://api.stepfun.com/step_plan/v1"
        ).rstrip("/")
        self.model = os.environ.get("STEPFUN_CHAT_MODEL", "stepaudio-2.5-chat")

    @property
    def available(self) -> bool:
        return bool(self.api_key)

    def chat(self, user_text: str) -> str:
        """返回模型回答文本。"""
        payload = {
            "model": self.model,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": user_text},
            ],
            "temperature": 0.7,
            "stream": False,
        }
        resp = requests.post(
            f"{self.base_url}/chat/completions",
            headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"},
            json=payload,
            timeout=60,
        )
        resp.raise_for_status()
        data = resp.json()
        return data["choices"][0]["message"]["content"].strip()

    def classify_command(self, user_text: str) -> Optional[dict[str, Any]]:
        """规则未命中时：让模型判断是否含播放器指令。

        期望模型只输出 JSON，如 {"action":"next"} 或 {"action":null}。
        """
        prompt = (
            "下面这句话是否包含「播放器控制」意图？"
            "控制意图包括：下一首、上一首、播放、暂停、调大/调小音量、播放指定歌曲。\n"
            "只输出 JSON，不要任何其它文字："
            '{"action": "next"|"prev"|"play"|"pause"|"volume_up"|"volume_down"'
            '|"play_song", "song": "<歌名，没有则空>", "has_command": true|false}。\n'
            f"用户说：{user_text}"
        )
        payload = {
            "model": self.model,
            "messages": [{"role": "user", "content": prompt}],
            "temperature": 0,
            "stream": False,
        }
        resp = requests.post(
            f"{self.base_url}/chat/completions",
            headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"},
            json=payload,
            timeout=60,
        )
        resp.raise_for_status()
        content = resp.json()["choices"][0]["message"]["content"].strip()
        try:
            # 剥掉可能包裹的 ```json ... ```
            cleaned = content.strip("` ")
            if cleaned.lower().startswith("json"):
                cleaned = cleaned[4:].strip()
            obj = json.loads(cleaned)
            if not obj.get("has_command"):
                return None
            action = obj.get("action")
            if action == "play_song":
                return {"action": "play_song", "song": obj.get("song") or ""}
            if action in ("next", "prev", "play", "pause", "volume_up", "volume_down"):
                return {"action": action}
            return None
        except (json.JSONDecodeError, AttributeError):
            return None


class MockLLM:
    """占位：不联网，返回固定回答，用于跑通链路。"""

    def __init__(self) -> None:
        self.available = True

    def chat(self, user_text: str) -> str:
        return f"（模拟回复）你说的是：{user_text}。接入 StepFun 后这里会变成真实回答。"

    def classify_command(self, user_text: str) -> Optional[dict[str, Any]]:
        return None


def get_llm():
    llm = StepFunLLM()
    if llm.available and os.environ.get("MOCK_LLM", "0") != "1":
        return llm
    return MockLLM()
