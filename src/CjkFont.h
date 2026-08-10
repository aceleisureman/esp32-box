#pragma once
#ifndef CJKFONT_H
#define CJKFONT_H

#include <Arduino.h>
#include <FS.h>

// SPIFFS 上的 4bpp 抗锯齿中文字库，由 scripts/gen_cjk_font.py 生成，
// 用 `pio run -t uploadfs` 烧录。两个实例：
//   CjkFont    16x16（正文，485KB）
//   CjkFont32  32x32（歌词大字，1.87MB）
//
// 为什么要独立的 32px 字库而不是放大 16px：
// 2x2 像素块放大（最近邻）不增加任何细节，只把边缘阶梯一并放大，
// 结果又糊又有锯齿。32px 原生渲染才有真实笔画细节与抗锯齿过渡。
//
// 设计取舍：
//   - 字形全量载入 PSRAM：运行期读 SPIFFS 会暂停 flash 缓存，
//     跑在 flash 里的 MP3 解码代码随之停摆 → I2S 欠载 → 爆音
//   - PSRAM 不足时回退到按需读取 + LRU 缓存（会有 flash 停顿）
//   - 索引区常驻内存，二分查找定位字形偏移
class CjkFontClass {
public:
    // path: SPIFFS 路径；glyphSize: 期望字形宽高（须与文件头一致）
    CjkFontClass(const char *path, uint8_t glyphSize)
        : _path(path), _glyphSize(glyphSize),
          _glyphBytes((uint16_t)glyphSize * glyphSize / 2) {}

    uint8_t  glyphSize() const  { return _glyphSize; }
    uint16_t glyphBytes() const { return _glyphBytes; }

    // 挂载 SPIFFS 并载入字库。失败时 isReady() 返回 false，
    // 上层退回"缺字占位"或降级到小字号，不影响其它功能。
    bool begin();
    bool isReady() const { return _ready; }
    uint16_t glyphCount() const { return _count; }

    // 取字形的 4bpp 位图（glyphBytes() 字节）。命中返回指针，缺字返回 nullptr。
    // 全量驻留模式下指针稳定；按需模式指向内部缓存，用完即用不要长期持有。
    const uint8_t *glyph(uint16_t codepoint);

private:
    static constexpr uint8_t kCacheSlots = 64;   // LRU 槽位数（按需模式用）
    static constexpr uint16_t kMaxGlyphBytes = 32 * 32 / 2;   // 32px 上限

    int32_t indexOf(uint16_t codepoint) const;   // 二分查找，未命中返回 -1
    int8_t  findCached(uint16_t codepoint) const;
    int8_t  evictSlot();

    const char *_path;
    uint8_t   _glyphSize;
    uint16_t  _glyphBytes;

    bool      _ready = false;
    uint16_t  _count = 0;
    uint32_t  _dataOffset = 0;    // 数据区在文件中的起始偏移（按需模式用）
    uint16_t *_index = nullptr;   // 码点索引表（升序），常驻
    uint8_t  *_data  = nullptr;   // 全量字形数据（PSRAM 驻留；null=按需模式）
    File      _file;              // 按需模式的常驻句柄

    struct Slot {
        uint16_t code = 0;        // 0 = 空槽
        uint32_t lastUsed = 0;    // LRU 时间戳（单调递增计数）
        uint8_t  data[kMaxGlyphBytes];
    };
    Slot     *_cache = nullptr;
    uint32_t  _tick = 0;
};

extern CjkFontClass CjkFont;     // 16x16 正文
extern CjkFontClass CjkFont32;   // 32x32 歌词大字

#endif // CJKFONT_H
