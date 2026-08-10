#!/usr/bin/env python3
"""用苹果 PingFang SC 渲染 GB2312 一级汉字，生成 4bpp 抗锯齿二进制字库。

输出 data/cjk16.bin（16px，正文）与 data/cjk32.bin（32px，歌词大字），
由 `pio run -t uploadfs` 烧进 SPIFFS，固件启动时载入 PSRAM。

为什么要 32px 原生字库：16px 字模按 2x2 像素块放大（最近邻）不会增加
任何细节，只会把边缘阶梯一并放大——看起来又糊又有锯齿。32px 原生渲染
才有真实的笔画细节与抗锯齿过渡，PingFang 的曲线才还原得出来。

文件格式（小端）：
  magic   4B  "CJK1"
  version 2B  = 1
  bpp     1B  = 4
  size    1B  = 16 或 32   字形宽高（像素）
  count   2B                字形数量
  reserved 6B 0
  ---- 索引区：count × 2B，Unicode 码点，升序 ----
  ---- 数据区：count × (size*size/2)B，与索引一一对应 ----
字形数据：size 行 × (size/2) 字节，每字节含两个像素的 4bit 覆盖率，高半字节在左。

用法:
  python3 scripts/gen_cjk_font.py              # 生成 16px + 32px 两个字库
  python3 scripts/gen_cjk_font.py --size 32    # 只生成 32px
  python3 scripts/gen_cjk_font.py --preview 晴  # 终端预览某字（16px）
  python3 scripts/gen_cjk_font.py --preview 晴 --size 32
"""
from PIL import Image, ImageDraw, ImageFont
import glob
import os
import struct
import sys

SS = 4             # 超采样倍率：4x 渲染再缩小 = 抗锯齿

# 每个字号的画布与字号设定。字号略小于画布避免裁边；
# 32px 下留白比例可以小一些（相对边距随尺寸变大而显得过宽）。
SIZE_PRESETS = {
    16: {"font_px": 15, "out": "cjk16.bin"},
    32: {"font_px": 30, "out": "cjk32.bin"},
}

# 苹果中文系统字体 PingFang SC（与 SF 同设计语言；SF 本体不含汉字）。
# macOS 新版把它放进 AssetsV2 资源包，路径含哈希，用通配符定位。
def font_candidates():
    for hit in sorted(glob.glob(
            "/System/Library/AssetsV2/com_apple_MobileAsset_Font*/"
            "*.asset/AssetData/PingFang.ttc")):
        yield (hit, 7)   # PingFang SC Medium
        yield (hit, 3)   # PingFang SC Regular
    yield ("/System/Library/Fonts/PingFang.ttc", 7)
    yield ("/System/Library/Fonts/Hiragino Sans GB.ttc", 2)  # W6
    yield ("/System/Library/Fonts/Hiragino Sans GB.ttc", 0)  # W3


def pick_font(font_px):
    for path, index in font_candidates():
        if not os.path.exists(path):
            continue
        try:
            font = ImageFont.truetype(path, font_px * SS, index=index)
            probe_font = ImageFont.truetype(path, 40, index=index)
        except OSError:
            continue
        # 确认这个子字体真的含汉字：否则 PIL 渲染出的是 .notdef 空心方块
        probe = Image.new("L", (64, 64), 0)
        ImageDraw.Draw(probe).text((4, 4), "晴", fill=255, font=probe_font)
        if not any(probe.getpixel((x, 30)) > 40 for x in range(10, 44)):
            continue
        name = font.getname()
        return font, f"{os.path.basename(path)}#{index} ({name[0]} {name[1]})"
    raise SystemExit("找不到含汉字的可用字体")


def charset():
    """GB2312 一级汉字（3755 字，按拼音序）+ 常用全角标点。

    一级字表覆盖现代中文文本 99.7% 以上，歌词/UI 足够。
    直接从 GB2312 编码区间反解，无需外部字表文件。
    """
    chars = []
    # 一级汉字：区位 16–55 区，每区 94 字
    for area in range(16, 56):
        for pos in range(1, 95):
            raw = bytes([0xA0 + area, 0xA0 + pos])
            try:
                ch = raw.decode("gb2312")
            except UnicodeDecodeError:
                continue
            chars.append(ch)
    # UI 常用的全角标点与符号（GB2312 第 1 区节选）
    chars += list("　、。〈〉《》「」『』【】〔〕—…‰′″※→←↑↓")
    chars += list("！＂＃￥％＆＇（）＊＋，－．／：；＜＝＞？＠［］｛｝")
    chars += list("°℃±×÷≈≠≤≥∞√")
    # 去重并按码点排序（固件侧二分查找依赖有序）
    return sorted(set(chars), key=ord)


def render(ch, font, size):
    """渲染单字，返回 size×size 的 4bpp 覆盖率矩阵（0~15）。"""
    big = Image.new("L", (size * SS, size * SS), 0)
    draw = ImageDraw.Draw(big)
    left, top, right, bottom = draw.textbbox((0, 0), ch, font=font)
    x = (size * SS - (right - left)) // 2 - left
    y = (size * SS - (bottom - top)) // 2 - top
    draw.text((x, y), ch, fill=255, font=font)

    # 超采样降尺寸即抗锯齿：每个目标像素取 SS×SS 区域的平均覆盖率
    img = big.resize((size, size), Image.LANCZOS)
    return [[min(15, (img.getpixel((xx, yy)) + 8) // 17) for xx in range(size)]
            for yy in range(size)]


def pack(rows, size):
    out = bytearray()
    for row in rows:
        for i in range(0, size, 2):
            out.append((row[i] << 4) | row[i + 1])
    return bytes(out)


def preview(ch, font, size):
    ramp = " .:-=+*#%@"
    rows = render(ch, font, size)
    print(f"--- {ch} (U+{ord(ch):04X})  {size}x{size}")
    for row in rows:
        print("".join(ramp[min(len(ramp) - 1, v * len(ramp) // 16)] for v in row))


def build(size, glyphs, root):
    """生成指定字号的字库文件，返回 (输出路径, 字节数)。"""
    preset = SIZE_PRESETS[size]
    font, font_desc = pick_font(preset["font_px"])

    dest_dir = os.path.join(root, "data")
    os.makedirs(dest_dir, exist_ok=True)
    dest = os.path.join(dest_dir, preset["out"])

    index = bytearray()
    data = bytearray()
    for ch in glyphs:
        index += struct.pack("<H", ord(ch))
        data += pack(render(ch, font, size), size)

    header = struct.pack("<4sHBBH6x", b"CJK1", 1, 4, size, len(glyphs))
    with open(dest, "wb") as f:
        f.write(header)
        f.write(index)
        f.write(data)

    total = len(header) + len(index) + len(data)
    print(f"  字体: {font_desc}")
    print(f"  输出: {dest}")
    print(f"  大小: {total:,} 字节 = {total / 1024:.0f} KB")
    return dest, total


def main():
    args = sys.argv[1:]

    size = None
    if "--size" in args:
        i = args.index("--size")
        size = int(args[i + 1])
        del args[i:i + 2]
        if size not in SIZE_PRESETS:
            raise SystemExit(f"仅支持字号 {sorted(SIZE_PRESETS)}")

    if args and args[0] == "--preview":
        psize = size or 16
        font, _ = pick_font(SIZE_PRESETS[psize]["font_px"])
        for ch in args[1]:
            preview(ch, font, psize)
        return

    glyphs = charset()
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    targets = [size] if size else sorted(SIZE_PRESETS)

    print(f"字形: {len(glyphs)} 个（GB2312 一级 + 常用标点）")
    grand = 0
    for sz in targets:
        print(f"\n[{sz}x{sz}] 渲染中...")
        _, total = build(sz, glyphs, root)
        grand += total

    print(f"\n合计: {grand / 1024:.0f} KB"
          f"（SPIFFS 3.38MB 的 {grand / 3538944 * 100:.1f}%）")
    print("烧录字库: pio run -t uploadfs")


if __name__ == "__main__":
    main()
