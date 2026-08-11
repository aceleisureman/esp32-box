#include "CjkFont.h"

#include <SPIFFS.h>

CjkFontClass CjkFont("/cjk16.bin", 16);
CjkFontClass CjkFont32("/cjk32.bin", 32);

namespace {

constexpr uint8_t  kHeaderBytes = 16;
constexpr uint16_t kExpectedVersion = 1;

}  // namespace

bool CjkFontClass::begin() {
    if (_ready) return true;

    // 两个字库实例都会调用；SPIFFS.begin 重复调用是安全的
    if (!SPIFFS.begin(false, "/spiffs", 10, "spiffs")) {
        Serial.println("[FONT] SPIFFS mount failed; run: pio run -t uploadfs");
        return false;
    }

    _file = SPIFFS.open(_path, "r");
    if (!_file) {
        Serial.printf("[FONT] %s missing; run: pio run -t uploadfs\n", _path);
        return false;
    }

    uint8_t header[kHeaderBytes];
    if (_file.read(header, kHeaderBytes) != kHeaderBytes ||
        memcmp(header, "CJK1", 4) != 0) {
        Serial.printf("[FONT] %s bad header\n", _path);
        _file.close();
        return false;
    }

    const uint16_t version = (uint16_t)header[4] | ((uint16_t)header[5] << 8);
    const uint8_t bpp = header[6];
    const uint8_t size = header[7];
    _count = (uint16_t)header[8] | ((uint16_t)header[9] << 8);

    if (version != kExpectedVersion || bpp != 4 || size != _glyphSize ||
        _count == 0) {
        Serial.printf("[FONT] %s unsupported: v%u bpp%u size%u count%u\n",
                      _path, (unsigned)version, (unsigned)bpp, (unsigned)size,
                      (unsigned)_count);
        _file.close();
        return false;
    }

    // 索引常驻内存（3817 字约 7.5KB），优先放 PSRAM 省 SRAM
    const size_t indexBytes = (size_t)_count * sizeof(uint16_t);
    _index = (uint16_t *)ps_malloc(indexBytes);
    if (!_index) _index = (uint16_t *)malloc(indexBytes);
    if (!_index) {
        Serial.printf("[FONT] %s index alloc failed\n", _path);
        _file.close();
        return false;
    }
    if (_file.read((uint8_t *)_index, indexBytes) != (int)indexBytes) {
        Serial.printf("[FONT] %s index read failed\n", _path);
        free(_index);
        _index = nullptr;
        _file.close();
        return false;
    }

    // 关键：把全部字形数据一次性载入 PSRAM（16px 485KB / 32px 1.87MB，
    // 8MB PSRAM 装得下）。运行期从 SPIFFS 按需读会在每次读取时暂停两个核
    // 的 flash 缓存，跑在 flash 里的 MP3 解码代码随之停摆 → I2S 欠载 →
    // 播放爆音。全量驻留后绘制路径完全不碰 flash，杂音源头被消除。
    const size_t dataBytes = (size_t)_count * _glyphBytes;
    _data = (uint8_t *)ps_malloc(dataBytes);
    if (_data) {
        // 大块读取分片进行：一次 read 近 2MB 在部分 SPIFFS 实现上会失败
        size_t done = 0;
        bool ok = true;
        while (done < dataBytes) {
            const size_t chunk = (dataBytes - done) > 32768u
                               ? 32768u : (dataBytes - done);
            const int got = _file.read(_data + done, chunk);
            if (got != (int)chunk) { ok = false; break; }
            done += chunk;
        }
        if (ok) {
            _file.close();   // 数据已全部驻留，文件不再需要
            _ready = true;
            Serial.printf("[FONT] %s ready: %u glyphs %ux%u in PSRAM (%uKB)\n",
                          _path, (unsigned)_count, (unsigned)_glyphSize,
                          (unsigned)_glyphSize, (unsigned)(dataBytes / 1024));
            return true;
        }
        Serial.printf("[FONT] %s bulk load failed; fall back to on-demand\n",
                      _path);
        free(_data);
        _data = nullptr;
    } else {
        Serial.printf("[FONT] %s no PSRAM (%uKB); fall back to on-demand\n",
                      _path, (unsigned)(dataBytes / 1024));
    }

    // 回退路径：无 PSRAM 时保持按需读取 + LRU 缓存（会有 flash 停顿）
    _cache = (Slot *)ps_calloc(kCacheSlots, sizeof(Slot));
    if (!_cache) _cache = (Slot *)calloc(kCacheSlots, sizeof(Slot));
    if (!_cache) {
        Serial.printf("[FONT] %s cache alloc failed\n", _path);
        free(_index);
        _index = nullptr;
        _file.close();
        return false;
    }

    _dataOffset = kHeaderBytes + indexBytes;
    _ready = true;
    Serial.printf("[FONT] %s ready: %u glyphs, on-demand + LRU\n",
                  _path, (unsigned)_count);
    return true;
}

int32_t CjkFontClass::indexOf(uint16_t codepoint) const {
    uint32_t lo = 0;
    uint32_t hi = _count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const uint16_t v = _index[mid];
        if (v == codepoint) return (int32_t)mid;
        if (v < codepoint) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

int8_t CjkFontClass::findCached(uint16_t codepoint) const {
    for (int8_t i = 0; i < (int8_t)kCacheSlots; i++) {
        if (_cache[i].code == codepoint) return i;
    }
    return -1;
}

int8_t CjkFontClass::evictSlot() {
    int8_t victim = 0;
    uint32_t oldest = UINT32_MAX;
    for (int8_t i = 0; i < (int8_t)kCacheSlots; i++) {
        if (_cache[i].code == 0) return i;       // 优先用空槽
        if (_cache[i].lastUsed < oldest) {
            oldest = _cache[i].lastUsed;
            victim = i;
        }
    }
    return victim;
}

const uint8_t *CjkFontClass::glyph(uint16_t codepoint) {
    if (!_ready) return nullptr;

    // 快速路径：字库已全量驻留 PSRAM，二分定位后直接返回，零 flash 访问
    if (_data) {
        const int32_t idx = indexOf(codepoint);
        if (idx < 0) return nullptr;
        return _data + (size_t)idx * _glyphBytes;
    }

    const int8_t hit = findCached(codepoint);
    if (hit >= 0) {
        _cache[hit].lastUsed = ++_tick;
        return _cache[hit].data;
    }

    const int32_t idx = indexOf(codepoint);
    if (idx < 0) return nullptr;                 // 字库里没有这个字

    const int8_t slot = evictSlot();
    const uint32_t offset = _dataOffset + (uint32_t)idx * _glyphBytes;
    if (!_file.seek(offset)) return nullptr;
    if (_file.read(_cache[slot].data, _glyphBytes) != (int)_glyphBytes) {
        _cache[slot].code = 0;                   // 读失败：标记空槽避免脏数据
        return nullptr;
    }

    _cache[slot].code = codepoint;
    _cache[slot].lastUsed = ++_tick;
    return _cache[slot].data;
}
