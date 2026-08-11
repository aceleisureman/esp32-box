"""播放器语音指令：识别 + 结构化输出。

指令集（与固件 AudioPlayer / MusicService 对应）：

    next          下一首         MusicService.next()
    prev          上一首         MusicService.previous()
    play          播放/继续      AudioPlayer.togglePause()（暂停态时）
    pause         暂停           AudioPlayer.togglePause()（播放态时）
    volume_up     大声点/音量+   AudioPlayer.volumeUp()
    volume_down   小声点/音量-   AudioPlayer.volumeDown()
    play_song     播放歌曲 X     （后端只回传指令，由固件决定如何按歌名播放）

识别策略：先用轻量规则在 ASR 文字里粗匹配（不依赖大模型，省时省钱），
未命中再让 LLM 判断。规则命中时，LLM 只需生成一句确认回复。

每条指令以 dict 表示：{"action": "next"} 或 {"action": "play_song", "song": "歌名"}
"""
from __future__ import annotations

from typing import Any, Optional


# (动作, 关键词列表)——按优先级排列，命中即返回
_COMMANDS: list[tuple[str, tuple[str, ...]]] = [
    ("next",        ("下一首", "换一首", "切歌", "下首歌", "下一曲", "下首")),
    ("prev",        ("上一首", "上一曲", "上一首歌", "回上一首")),
    ("volume_up",   ("大声点", "音量加大", "音量大点", "调大音量", "音量加", "大声")),
    ("volume_down", ("小声点", "音量减小", "音量小点", "调小音量", "音量减", "小声")),
    ("pause",       ("暂停", "停止播放", "别唱了", "停一下")),
    ("play",        ("播放音乐", "播放", "继续播放", "接着唱", "开始播放", "放起来")),
]


def match_rule(text: str) -> Optional[dict[str, Any]]:
    """用关键词在 ASR 文字里匹配播放器指令。

    返回指令 dict；未命中返回 None（交由 LLM 判断）。
    注意顺序：先匹配「上一首」再匹配「播放」——"播放上一首"应优先切歌。
    """
    if not text:
        return None
    # prev 的关键词含 "上一首"，play 含 "播放"，先检查 prev/next 类
    for action, kws in _COMMANDS:
        if action in ("prev", "next"):
            for kw in kws:
                if kw in text:
                    return {"action": action}
    # 播放指定歌曲："播放 <歌名>"/"唱 <歌名>"/"来一首 <歌名>"
    for kw in ("播放", "来一首", "唱一首", "给我唱", "放一首"):
        idx = text.find(kw)
        if idx >= 0:
            song = text[idx + len(kw):].strip(" ，。,.！？!?")
            if song and song != "音乐":
                return {"action": "play_song", "song": song}
    # 其余简单指令
    for action, kws in _COMMANDS:
        if action in ("prev", "next"):
            continue
        for kw in kws:
            if kw in text:
                return {"action": action}
    return None


def confirm_reply(command: dict[str, Any]) -> str:
    """指令命中时给用户的确认回复（后端据此做 TTS）。"""
    action = command.get("action")
    replies = {
        "next":        "好的，下一首",
        "prev":        "好的，上一首",
        "volume_up":   "好的，声音大一点",
        "volume_down": "好的，声音小一点",
        "play":        "好的，开始播放",
        "pause":       "好的，已暂停",
    }
    if action == "play_song":
        return f"好的，为你播放《{command.get('song', '')}》"
    return replies.get(action, "好的")
