"""情感陪伴记忆模块：按设备（device_id）隔离记忆。

每个设备有独立的记忆文件（server/memory/<device_id>.json），包含：
  - profile：长期记忆（用户偏好、称呼、重要事实），由记忆摘要定期提炼
  - history：最近对话轮次（短期记忆），注入会话供模型参考
  - 每次对话后追加轮次，历史过长时自动裁剪，并让模型提炼长期记忆

用途：多设备各自绑定独立记忆（例如客厅盒子和卧室盒子记得不同的事），
设备间互不干扰。
"""
from __future__ import annotations

import json
import os
import re
import threading
from typing import Any, Optional

from dotenv import load_dotenv

load_dotenv()

MEMORY_DIR = os.path.join(os.path.dirname(__file__), "memory")
# 短期对话历史保留的轮数（每轮 user+assistant 两条）
MAX_HISTORY_TURNS = int(os.environ.get("MEMORY_MAX_TURNS", "12"))
# 历史超长时触发一次记忆提炼（把重要事实归入 profile）
MAX_HISTORY_BEFORE_SUMMARIZE = int(os.environ.get("MEMORY_SUMMARIZE_TURNS", "24"))

_LOCK = threading.Lock()


def _safe_device_id(device_id: str) -> str:
    """device_id 只允许安全字符，防路径穿越。"""
    cleaned = re.sub(r"[^A-Za-z0-9_-]", "_", device_id or "unknown")
    return cleaned[:64] or "unknown"


def _path_for(device_id: str) -> str:
    return os.path.join(MEMORY_DIR, _safe_device_id(device_id) + ".json")


def _empty() -> dict[str, Any]:
    return {
        "device_id": "",
        "profile": {},          # 长期记忆：{"称呼": "小敏", "喜欢的音乐": "周杰伦"}
        "history": [],          # 短期历史：[{"role": "user"|"assistant", "content": "..."}]
        "turns": 0,             # 累计对话轮数
    }


def load(device_id: str) -> dict[str, Any]:
    """读取某设备的记忆；无则返回空记忆（不写盘）。"""
    os.makedirs(MEMORY_DIR, exist_ok=True)
    path = _path_for(device_id)
    with _LOCK:
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            if not isinstance(data, dict):
                return _empty()
            data.setdefault("profile", {})
            data.setdefault("history", [])
            data.setdefault("turns", 0)
            return data
        except (OSError, json.JSONDecodeError):
            return _empty()


def _save(device_id: str, data: dict[str, Any]) -> None:
    path = _path_for(device_id)
    os.makedirs(MEMORY_DIR, exist_ok=True)
    tmp = path + ".tmp"
    with _LOCK:
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
            os.replace(tmp, path)
        except OSError:
            pass  # 记忆写盘失败不阻断对话


def append(device_id: str, role: str, content: str) -> None:
    """追加一条对话轮次，超长时裁剪（保留最近 MAX_HISTORY_TURNS 轮）。"""
    if not content:
        return
    data = load(device_id)
    data["history"].append({"role": role, "content": content})
    if role == "user":
        data["turns"] += 1
    # 裁剪：保留最近 MAX_HISTORY_TURNS*2 条（user+assistant 各半）
    max_entries = MAX_HISTORY_TURNS * 2
    if len(data["history"]) > max_entries:
        data["history"] = data["history"][-max_entries:]
    _save(device_id, data)


def build_instructions(device_id: str) -> str:
    """为一次会话生成 instructions：情感人设 + 该设备的记忆摘要。"""
    data = load(device_id)
    lines = [persona()]

    profile = data.get("profile") or {}
    if profile:
        facts = "；".join(f"{k}：{v}" for k, v in profile.items() if v)
        if facts:
            lines.append(f"关于这位用户，你记得：{facts}。")

    history = data.get("history") or []
    if history:
        lines.append("以下是最近的对话（可作为话题延续）：")
        for item in history[-6:]:   # 只注入最近 6 轮，控制长度
            role = "用户" if item.get("role") == "user" else "你"
            lines.append(f"{role}说：{item.get('content', '')}")
    else:
        lines.append("这是你们的第一次对话，可以自然地自我介绍并开始聊天。")

    lines.append(
        "请用简洁、温暖、口语化的中文回答，适合语音播报，"
        "单次回答不超过 3 句话。"
    )
    return "\n".join(lines)


def update_profile(device_id: str, summary: Optional[str]) -> None:
    """把一次提炼的长期记忆摘要并入 profile（用于后续提炼，暂未自动触发）。"""
    if not summary:
        return
    data = load(device_id)
    data["profile"].setdefault("记忆摘要", summary)
    _save(device_id, data)


# 情感陪伴人设：可被 VIRTUAL_COMPANION_PROMPT 环境变量覆盖，便于按设备/场景调
_PERSONA = (
    "你是一位温柔、有耐心的情感陪伴伙伴，不是机械的语音助手。"
    "你会记住用户说过的话，关心他的感受，聊天气氛轻松自然。"
    "用户心情低落时先共情，再给建议；开心时一起开心。"
    "不要说你只是'AI'或'助手'，像朋友一样陪伴。"
)


def persona() -> str:
    return os.environ.get("VIRTUAL_COMPANION_PROMPT", _PERSONA)
