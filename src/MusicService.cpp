#include "MusicService.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <JPEGDEC.h>
#include <string.h>

MusicServiceClass MusicService;

namespace {

// CloudMusic Tools 后端，局域网直连。
// 明文 HTTP，固件侧走 WiFiClient。
constexpr const char *kApiBase = "http://192.168.28.50:9965";
constexpr uint32_t kHttpTimeoutMs = 8000;

// ---- 轻量 JSON 取值：后端返回体积可观，避免引入完整解析库占内存 ----

// 在「当前对象层级」内查找 key，不下钻嵌套对象/数组。
// 必要性：网易云的 al 对象里嵌套了 artist 子对象，它也有 name / picUrl，
// 且出现在 al.name 之前；朴素的 strstr 会取到错误的值。
const char *findValueAtLevel(const char *obj, const char *key) {
    if (!obj || *obj != '{') return nullptr;
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const size_t patLen = strlen(pattern);

    const char *p = obj + 1;
    int depth = 0;
    bool inString = false;

    while (*p) {
        if (inString) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == '"') inString = false;
            p++;
            continue;
        }
        if (*p == '"') {
            // 仅在本层（depth==0）比对键名
            if (depth == 0 && strncmp(p, pattern, patLen) == 0) {
                const char *v = p + patLen;
                while (*v == ' ' || *v == ':') v++;
                return *v ? v : nullptr;
            }
            inString = true;
            p++;
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') {
            if (depth == 0) break;      // 本对象结束
            depth--;
        }
        p++;
    }
    return nullptr;
}

// 找到 "key" 之后的值起始位置（全局搜索，用于顶层唯一字段）
const char *findValue(const char *json, const char *key) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return nullptr;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    return *p ? p : nullptr;
}

// 从值起始位置读取字符串，处理 \" \\ \n \/ \uXXXX 转义。
// 非 ASCII 原样保留（UTF-8），由字库负责渲染。
bool readString(const char *p, char *out, size_t outSize) {
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outSize) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; p++; break;
                case 't': out[i++] = ' ';  p++; break;
                case 'u': {
                    // \uXXXX → UTF-8
                    if (!p[1] || !p[2] || !p[3] || !p[4]) { p++; break; }
                    char hex[5] = {p[1], p[2], p[3], p[4], 0};
                    const uint16_t cp = (uint16_t)strtoul(hex, nullptr, 16);
                    p += 5;
                    if (cp < 0x80) {
                        if (i + 1 < outSize) out[i++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (i + 2 < outSize) {
                            out[i++] = (char)(0xC0 | (cp >> 6));
                            out[i++] = (char)(0x80 | (cp & 0x3F));
                        } else i = outSize;
                    } else {
                        if (i + 3 < outSize) {
                            out[i++] = (char)(0xE0 | (cp >> 12));
                            out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[i++] = (char)(0x80 | (cp & 0x3F));
                        } else i = outSize;
                    }
                    break;
                }
                default: out[i++] = *p++; break;   // \" \\ \/ 等原样
            }
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return true;
}

// 在当前对象层级取字符串字段
bool jsonStringAtLevel(const char *obj, const char *key,
                       char *out, size_t outSize) {
    return readString(findValueAtLevel(obj, key), out, outSize);
}

// 全局搜索取字符串（用于顶层唯一字段，如 lyric / url）
bool jsonString(const char *json, const char *key, char *out, size_t outSize) {
    return readString(findValue(json, key), out, outSize);
}

bool jsonNumberAtLevel(const char *obj, const char *key, uint32_t &out) {
    const char *p = findValueAtLevel(obj, key);
    if (!p) return false;
    char *end = nullptr;
    const unsigned long v = strtoul(p, &end, 10);
    if (end == p) return false;
    out = (uint32_t)v;
    return true;
}

// 在 json 中定位第 n 个 "songs" 数组元素的起始位置（简易扫描，够用）
const char *nthArrayElement(const char *arrayStart, uint8_t n) {
    if (!arrayStart || *arrayStart != '[') return nullptr;
    const char *p = arrayStart + 1;
    uint8_t index = 0;
    int depth = 0;
    const char *elemStart = nullptr;

    while (*p) {
        if (*p == '{') {
            if (depth == 0) elemStart = p;
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                if (index == n) return elemStart;
                index++;
            }
        } else if (*p == ']' && depth == 0) {
            break;
        }
        p++;
    }
    return nullptr;
}

bool httpGetJson(const String &url, String &body) {
    if (WiFi.status() != WL_CONNECTED) return false;

    const bool secure = url.startsWith("https");
    WiFiClientSecure tlsClient;
    WiFiClient plainClient;
    HTTPClient http;
    bool begun;
    if (secure) {
        tlsClient.setInsecure();          // cpolar 动态证书，跳过校验
        tlsClient.setTimeout(kHttpTimeoutMs / 1000);
        begun = http.begin(tlsClient, url);
    } else {
        plainClient.setTimeout(kHttpTimeoutMs / 1000);
        begun = http.begin(plainClient, url);
    }
    if (!begun) {
        Serial.printf("[NCM] begin failed: %s\n", url.c_str());
        return false;
    }
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    http.useHTTP10(true);

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[NCM] GET %s -> %d\n", url.c_str(), code);
        http.end();
        return false;
    }
    body = http.getString();
    http.end();
    return body.length() > 0;
}

// 下载二进制到固定缓冲（封面 JPEG）。返回实际长度，失败返回 0。
size_t httpGetBinary(const String &url, uint8_t *buf, size_t maxLen) {
    if (WiFi.status() != WL_CONNECTED) return 0;

    const bool secure = url.startsWith("https");
    WiFiClientSecure tlsClient;
    WiFiClient plainClient;
    HTTPClient http;
    bool begun;
    if (secure) {
        tlsClient.setInsecure();
        tlsClient.setTimeout(kHttpTimeoutMs / 1000);
        begun = http.begin(tlsClient, url);
    } else {
        plainClient.setTimeout(kHttpTimeoutMs / 1000);
        begun = http.begin(plainClient, url);
    }
    if (!begun) return 0;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    http.useHTTP10(true);

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        return 0;
    }
    // 流式读取并限制字节数（writeToStream 会写满整个响应，可能溢出缓冲）
    Stream &s = http.getStream();
    size_t got = 0;
    while (got < maxLen) {
        const int c = s.read();
        if (c < 0) break;
        buf[got++] = (uint8_t)c;
    }
    http.end();
    return got;
}

// ---- JPEG 封面解码：输出到固定 RGB565 缓冲（画布式），支持任意缩放 ----
constexpr uint8_t  kCoverSize = 88;                 // 与播放页封面 box 一致
uint16_t          *gCoverDst = nullptr;             // 解码目标（由 fetchCover 指向）
uint16_t           gCoverW = 0;
uint16_t           gCoverH = 0;
uint16_t           gCoverSrcH = 0;
uint16_t           gCoverSrcW = 0;

// JPEGDEC 逐块回调：把解码出的像素写进目标画布（必要时跳行取整缩放）
int coverDrawCallback(JPEGDRAW *pDraw) {
    const uint16_t dstW = gCoverW;
    const uint16_t dstH = gCoverH;
    if (!dstW || !dstH || !gCoverDst) return 0;
    const uint32_t srcW = pDraw->iWidth;
    const uint32_t srcH = pDraw->iHeight;
    if (!srcW || !srcH) return 0;

    const int16_t startY = pDraw->y;
    const int16_t endY = startY + (int16_t)srcH;
    for (int16_t sy = startY; sy < endY; sy++) {
        const uint16_t dy = (uint16_t)((uint32_t)sy * dstH / gCoverSrcH);
        if (dy >= dstH) continue;
        for (uint16_t sx = 0; sx < srcW; sx++) {
            const uint16_t dx = (uint16_t)(((uint32_t)pDraw->x + sx) *
                                           dstW / gCoverSrcW);
            if (dx >= dstW) continue;
            const uint16_t idx = (uint16_t)(dy * dstW + dx);
            gCoverDst[idx] = pDraw->pPixels[sx];
        }
    }
    return 1;
}

// 解码 JPEG 到 dst，size 为目标边长（服务端已用 param 缩放成小图）
bool decodeCoverToRgb565(const uint8_t *jpg, size_t jpgLen, uint16_t size,
                         uint16_t *dst) {
    // JPEGDEC 是零 malloc 设计：约 20KB 工作缓冲全部内嵌在对象里。
    // 绝不能放任务栈（ncm 栈才 12KB，放栈上必溢出崩溃重启），static 进 .bss。
    static JPEGDEC dec;
    if (dec.openRAM((uint8_t *)jpg, jpgLen, coverDrawCallback) != 1) {
        Serial.println("[NCM] cover: JPEGDEC open failed");
        return false;
    }
    gCoverDst = dst;
    gCoverW = size;
    gCoverH = size;
    gCoverSrcW = dec.getWidth();
    gCoverSrcH = dec.getHeight();
    if (!gCoverSrcW || !gCoverSrcH) {
        gCoverDst = nullptr;
        dec.close();
        return false;
    }
    const int rc = dec.decode(0, 0, 0);
    gCoverDst = nullptr;
    dec.close();
    if (rc != 1) {
        Serial.printf("[NCM] cover: decode failed rc=%d\n", rc);
        return false;
    }
    return true;
}

// 解析一行 LRC 行首的全部时间戳。标准行是"[mm:ss.xx]歌词"，
// 压缩格式则是"[t1][t2]歌词"——同一句在多个时刻出现（副歌复用），
// 之前只认第一个时间戳，第二个会被当作歌词文本显示出来。
// 返回时间戳个数；*textOut 指向最后一个时间戳之后的歌词文本。
uint8_t parseLrcTimestamps(const char *line, uint32_t *tsOut, uint8_t maxTs,
                           const char **textOut) {
    uint8_t n = 0;
    const char *p = line;
    while (*p == '[') {
        // 仅接受数字开头的时间戳，[ar:xx] 之类元信息标签直接停
        if (p[1] < '0' || p[1] > '9') break;
        const char *colon = strchr(p, ':');
        const char *close = strchr(p, ']');
        if (!colon || !close || close < colon) break;
        const uint32_t minutes = (uint32_t)strtoul(p + 1, nullptr, 10);
        const float seconds = strtof(colon + 1, nullptr);
        if (n < maxTs) {
            tsOut[n++] = minutes * 60000UL + (uint32_t)(seconds * 1000.0f);
        }
        p = close + 1;
    }
    if (textOut) *textOut = p;
    return n;
}

// 网易云歌词开头往往有一批「演唱 : xxx」「作词 : xxx」的制作人员信息行，
// 它们同样带时间戳，但对播放展示无意义，这里识别并跳过。
bool isLrcMetaLine(const char *text) {
    static const char *const kMetaKeys[] = {
        "演唱", "作词", "作曲", "编曲", "制作人", "混音", "母带",
        "监制", "录音", "吉他", "贝斯", "鼓", "键盘", "和声",
        "音乐总监", "音响总监", "舞台总监", "音乐设计", "出品",
    };
    // 制作信息的共同特征：包含 " : " 分隔符
    if (!strstr(text, " : ")) return false;
    for (const char *key : kMetaKeys) {
        if (strstr(text, key)) return true;
    }
    return false;
}

}  // namespace

void MusicServiceClass::init() {
    if (_lock) return;
    _lock = xSemaphoreCreateMutex();
    // 20KB 栈：HTTPS（mbedTLS 握手）+ JSON 解析 + JPEG 封面解码调用链
    // 都在这一个任务里叠加；栈给足避免 Stack canary 溢出重启。
    if (!_lock || xTaskCreatePinnedToCore(&MusicServiceClass::taskEntry, "ncm",
                                          20480, this, 2, &_task, 0) != pdPASS) {
        _task = nullptr;
        Serial.println("[NCM] task creation failed");
        return;
    }
    Serial.println("[NCM] music service task started");
}

void MusicServiceClass::update(bool wifiConnected) {
    if (wifiConnected != _wifiUp) {
        _wifiUp = wifiConnected;
        if (wifiConnected) {
            _forceRefresh = true;
        } else {
            setState(State::Idle);
        }
    }
}

void MusicServiceClass::setState(State s) {
    _state = s;
}

uint8_t MusicServiceClass::trackCount() {
    if (!_lock) return 0;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const uint8_t n = _trackCount;
    xSemaphoreGive(_lock);
    return n;
}

bool MusicServiceClass::track(uint8_t index, Track &out) {
    if (!_lock) return false;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const bool ok = index < _trackCount;
    if (ok) out = _tracks[index];
    xSemaphoreGive(_lock);
    return ok;
}

uint16_t MusicServiceClass::lyricCount() {
    if (!_lock) return 0;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const uint16_t n = _lyricCount;
    xSemaphoreGive(_lock);
    return n;
}

bool MusicServiceClass::lyric(uint16_t index, LyricLine &out) {
    if (!_lock) return false;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const bool ok = index < _lyricCount;
    if (ok) out = _lyrics[index];
    xSemaphoreGive(_lock);
    return ok;
}

String MusicServiceClass::playUrl() {
    if (!_lock) return String();
    xSemaphoreTake(_lock, portMAX_DELAY);
    const String url = _playUrl;
    xSemaphoreGive(_lock);
    return url;
}

AudioPlayerClass::PlayFormat MusicServiceClass::playFormat() {
    if (!_lock) return AudioPlayerClass::PlayFormat::Unknown;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const AudioPlayerClass::PlayFormat f = _playFormat;
    xSemaphoreGive(_lock);
    return f;
}

void MusicServiceClass::selectTrack(uint8_t index) {
    const uint8_t total = trackCount();
    if (!total) return;
    _currentIndex = index % total;
    // 立即清空旧直链、格式与歌词：网络任务拉取新数据需要时间，
    // 这个窗口期里若残留旧值，上层会误判为"新曲已就绪"而重播上一首。
    xSemaphoreTake(_lock, portMAX_DELAY);
    _playUrl = "";
    _playFormat = AudioPlayerClass::PlayFormat::Unknown;
    _lyricCount = 0;
    xSemaphoreGive(_lock);
    _needTrackDetail = true;
}

void MusicServiceClass::next() {
    const uint8_t total = trackCount();
    if (!total) return;
    // 漫游 FM 每次调用后端都会返回一批新歌，播到末尾就续拉下一批，
    // 否则会一直在开机时取到的那几首里打转。
    if (_currentIndex + 1 >= total) {
        Serial.println("[NCM] reached end of batch, fetching next roam batch");
        _forceRefresh = true;
        return;
    }
    selectTrack((uint8_t)(_currentIndex + 1));
}

void MusicServiceClass::previous() {
    const uint8_t total = trackCount();
    if (!total) return;
    // 上一首不跨批：停在本批第一首，避免回退时反而拉到全新的歌
    if (_currentIndex == 0) return;
    selectTrack((uint8_t)(_currentIndex - 1));
}

uint8_t MusicServiceClass::bookCount() {
    if (!_lock) return 0;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const uint8_t n = _bookCount;
    xSemaphoreGive(_lock);
    return n;
}

bool MusicServiceClass::book(uint8_t index, Book &out) {
    if (!_lock) return false;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const bool ok = index < _bookCount;
    if (ok) out = _books[index];
    xSemaphoreGive(_lock);
    return ok;
}

bool MusicServiceClass::fetchBookList() {
    String url = String(kApiBase) + "/api/music/fm/cell_change";
    String body;
    if (!httpGetJson(url, body)) return false;

    const char *books = findValue(body.c_str(), "books");
    if (!books || *books != '[') {
        Serial.println("[NCM] books: array missing");
        return false;
    }

    xSemaphoreTake(_lock, portMAX_DELAY);
    _bookCount = 0;
    for (uint8_t i = 0; i < kMaxBooks; i++) {
        const char *elem = nthArrayElement(books, i);
        if (!elem) break;
        Book &b = _books[_bookCount];
        b = Book();
        jsonStringAtLevel(elem, "book_name", b.name, sizeof(b.name));
        jsonStringAtLevel(elem, "author", b.author, sizeof(b.author));
        jsonStringAtLevel(elem, "abstract", b.abstract, sizeof(b.abstract));
        if (!b.name[0]) continue;
        _bookCount++;
    }
    const uint8_t got = _bookCount;
    xSemaphoreGive(_lock);

    Serial.printf("[NCM] book list: %u titles\n", (unsigned)got);
    return got > 0;
}

void MusicServiceClass::taskEntry(void *arg) {
    static_cast<MusicServiceClass *>(arg)->taskLoop();
}

// 下载并解码当前曲目封面。URL 加 ?param=88y88 让网易云 CDN 直接返回
// 88x88 缩放图（约 12KB），省去本地缩放。失败时保持旧封面（或不显示）。
bool MusicServiceClass::fetchCover(const char *url) {
    if (!url || !url[0]) return false;

    String coverUrl = String(url) + "?param=" + kCoverSize + "y" + kCoverSize;

    // 缓冲：PSRAM 优先
    static uint8_t *jpg = nullptr;
    if (!jpg) {
        jpg = (uint8_t *)ps_malloc(48 * 1024);
        if (!jpg) return false;
    }

    const size_t jpgLen = httpGetBinary(coverUrl, jpg, 48 * 1024);
    if (jpgLen == 0) {
        Serial.println("[NCM] cover: download failed");
        return false;
    }

    // 解码输出到 PSRAM 的 88x88 RGB565 缓冲
    if (!_cover) {
        _cover = (uint16_t *)ps_malloc(kCoverSize * kCoverSize * 2);
        if (!_cover) return false;
    }
    if (!decodeCoverToRgb565(jpg, jpgLen, kCoverSize, _cover)) return false;

    _coverSize = kCoverSize;
    _coverReady = true;
    Serial.printf("[NCM] cover ready %ux%u (%uB jpg)\n",
                  (unsigned)kCoverSize, (unsigned)kCoverSize,
                  (unsigned)jpgLen);
    return true;
}

void MusicServiceClass::taskLoop() {
    uint32_t nextRetryMs = 0;

    for (;;) {
        if (!_wifiUp) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 首次联网或手动刷新：拉取漫游歌单
        const bool needList = _forceRefresh ||
            (_state == State::Failed &&
             (int32_t)(millis() - nextRetryMs) >= 0);
        if (needList) {
            _forceRefresh = false;
            setState(State::Loading);
            // 续拉新一批前清空旧直链、格式与歌词：否则在新数据到达之前，
            // 上层看到残留的旧直链会误判为"已就绪"而重播当前这首。
            xSemaphoreTake(_lock, portMAX_DELAY);
            _playUrl = "";
            _playFormat = AudioPlayerClass::PlayFormat::Unknown;
            _lyricCount = 0;
            xSemaphoreGive(_lock);

            if (fetchRoamList()) {
                setState(State::Ready);
                _currentIndex = 0;
                _needTrackDetail = true;
            } else {
                setState(State::Failed);
                nextRetryMs = millis() + kRetryMs;
            }
            continue;
        }

        // 切歌后拉取该曲的歌词、播放直链与封面
        if (_needTrackDetail && _state == State::Ready) {
            _needTrackDetail = false;
            _coverReady = false;   // 旧封面作废，等待新封面解码
            Track t;
            if (track(_currentIndex, t) && t.id) {
                fetchLyrics(t.id);
                fetchPlayUrl(t.id);
                // 封面放最后：解码耗时较长，不应拖住歌词/直链就绪
                if (t.coverUrl[0]) fetchCover(t.coverUrl);
            }
            continue;
        }

        // 听书列表请求
        if (_needBookList) {
            _needBookList = false;
            fetchBookList();
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

bool MusicServiceClass::fetchRoamList() {
    String url = String(kApiBase) + "/api/music/wy/discover/roam";
    String body;
    if (!httpGetJson(url, body)) return false;

    const char *songs = findValue(body.c_str(), "songs");
    if (!songs || *songs != '[') {
        Serial.println("[NCM] roam: songs array missing");
        return false;
    }

    xSemaphoreTake(_lock, portMAX_DELAY);
    _trackCount = 0;
    for (uint8_t i = 0; i < kMaxTracks; i++) {
        const char *elem = nthArrayElement(songs, i);
        if (!elem) break;

        Track &t = _tracks[_trackCount];
        t = Track();
        uint32_t id = 0;
        if (!jsonNumberAtLevel(elem, "id", id) || id == 0) continue;
        t.id = id;
        jsonStringAtLevel(elem, "name", t.title, sizeof(t.title));

        // 歌手在 "ar":[{"id":..,"name":".."}]，取第一个元素的 name。
        // 必须用层级查找：ar[0] 里 name 之前还有 img1v1Url 等字段。
        const char *ar = findValueAtLevel(elem, "ar");
        if (ar && *ar == '[') {
            const char *first = nthArrayElement(ar, 0);
            if (first) {
                jsonStringAtLevel(first, "name", t.artist, sizeof(t.artist));
            }
        }
        // 专辑在 "al":{"name":"..","picUrl":".."}。
        // al 内嵌了 artist 子对象（同样有 name/picUrl 且位置更靠前），
        // 层级查找可避免取到专辑歌手的名字与头像。
        const char *al = findValueAtLevel(elem, "al");
        if (al && *al == '{') {
            jsonStringAtLevel(al, "name", t.album, sizeof(t.album));
            jsonStringAtLevel(al, "picUrl", t.coverUrl, sizeof(t.coverUrl));
        }
        uint32_t dt = 0;
        if (jsonNumberAtLevel(elem, "dt", dt)) t.durationMs = dt;

        _trackCount++;
    }
    const uint8_t got = _trackCount;
    xSemaphoreGive(_lock);

    Serial.printf("[NCM] roam list: %u tracks\n", (unsigned)got);
    return got > 0;
}

bool MusicServiceClass::fetchLyrics(uint32_t songId) {
    String url = String(kApiBase) + "/api/music/wy/song/lyric?id=" + songId;
    String body;

    xSemaphoreTake(_lock, portMAX_DELAY);
    _lyricCount = 0;
    xSemaphoreGive(_lock);

    if (!httpGetJson(url, body)) return false;

    // 后端返回形如 {"lrc":{"lyric":"[00:00.00]词\n[00:05.00]词"}}
    // 先取到 lyric 字段的原始串（含 \n 转义），再逐行解析
    static char raw[4096];
    if (!jsonString(body.c_str(), "lyric", raw, sizeof(raw))) {
        Serial.println("[NCM] lyric: field missing");
        return false;
    }

    xSemaphoreTake(_lock, portMAX_DELAY);
    _lyricCount = 0;
    char *line = strtok(raw, "\n");
    while (line && _lyricCount < kMaxLyrics) {
        // 一行可带多个时间戳（压缩 LRC），每个时间戳都生成一条记录
        uint32_t ts[8];
        const char *text = nullptr;
        const uint8_t n = parseLrcTimestamps(line, ts, 8, &text);
        if (n && text) {
            while (*text == ' ') text++;
            // 跳过空行与「演唱 : xxx」这类制作信息
            if (*text && !isLrcMetaLine(text)) {
                for (uint8_t k = 0; k < n && _lyricCount < kMaxLyrics; k++) {
                    _lyrics[_lyricCount].startMs = ts[k];
                    snprintf(_lyrics[_lyricCount].text,
                             sizeof(_lyrics[_lyricCount].text), "%s", text);
                    _lyricCount++;
                }
            }
        }
        line = strtok(nullptr, "\n");
    }
    // 压缩 LRC 的重复句时间乱序（副歌行挂着第一次出现的时刻），
    // 而显示端的行定位假定按时间升序——必须排序。插入排序对
    // 近乎有序的输入接近 O(n)，n≤64 开销可忽略。
    for (uint16_t i = 1; i < _lyricCount; i++) {
        const LyricLine key = _lyrics[i];
        int16_t j = (int16_t)i - 1;
        while (j >= 0 && _lyrics[j].startMs > key.startMs) {
            _lyrics[j + 1] = _lyrics[j];
            j--;
        }
        _lyrics[j + 1] = key;
    }
    const uint16_t got = _lyricCount;
    xSemaphoreGive(_lock);

    Serial.printf("[NCM] lyric: %u lines for id=%lu\n",
                  (unsigned)got, (unsigned long)songId);
    return got > 0;
}

bool MusicServiceClass::fetchPlayUrl(uint32_t songId) {
    // 后端返回多档音质：{"items":[{type,br,url},...]}。此处请求 exhigh
    // （极高 320k MP3）：MP3 解码压力远小于 FLAC，听感比 standard/higher
    // 明显更好，ESP32-S3 完全跑得动；不选 lossless/hires（FLAC 解码与
    // 公网隧道带宽都有风险）。
    // ┌──────────┬────────────┬─────────────┬──────────────┐
    // │ level    │ 常见名称   │ 码率映射 br │ 说明         │
    // ├──────────┼────────────┼─────────────┼──────────────┤
    // │ standard │ 标准       │ 128000      │ 约 128kbps   │
    // ├──────────┼────────────┼─────────────┼──────────────┤
    // │ higher   │ 较高       │ 192000      │ 约 192kbps   │
    // ├──────────┼────────────┼─────────────┼å──────────────┤
    // │ exhigh   │ 极高       │ 320000      │ 约 320kbps   │
    // ├──────────┼────────────┼─────────────┼──────────────┤
    // │ lossless │ 无损       │ 999000      │ FLAC 等      │
    // ├──────────┼────────────┼─────────────┼──────────────┤
    // │ hires    │ Hi-Res     │ 999000      │ 高解析       │
    // └──────────┴────────────┴─────────────┴──────────────┘
    String url = String(kApiBase) +
                 "/api/music/wy/song/play_urls?id=" + songId + "&level=standard";
    String body;

    xSemaphoreTake(_lock, portMAX_DELAY);
    _playUrl = "";
    _playFormat = AudioPlayerClass::PlayFormat::Unknown;
    xSemaphoreGive(_lock);

    if (!httpGetJson(url, body)) return false;

    const char *items = findValue(body.c_str(), "items");
    if (!items || *items != '[') {
        Serial.println("[NCM] play_urls: items array missing");
        return false;
    }

    char bestUrl[256] = "";
    uint32_t bestBr = 0;
    for (uint8_t i = 0; i < 8; i++) {
        const char *elem = nthArrayElement(items, i);
        if (!elem) break;

        char type[8] = "";
        jsonStringAtLevel(elem, "type", type, sizeof(type));
        if (strcmp(type, "mp3") != 0) continue;   // exhigh 下应是 mp3，跳过其它

        char candidate[256] = "";
        if (!jsonStringAtLevel(elem, "url", candidate, sizeof(candidate)) ||
            !candidate[0]) {
            continue;
        }

        uint32_t br = 0;
        jsonNumberAtLevel(elem, "br", br);
        // 取码率最高的 MP3：exhigh 档下就是 320k，听感最好
        if (bestUrl[0] && br <= bestBr) continue;
        snprintf(bestUrl, sizeof(bestUrl), "%s", candidate);
        bestBr = br;
    }

    if (!bestUrl[0]) {
        Serial.println("[NCM] play_urls: no playable source");
        return false;
    }

    xSemaphoreTake(_lock, portMAX_DELAY);
    _playUrl = bestUrl;
    _playFormat = AudioPlayerClass::PlayFormat::Mp3;
    xSemaphoreGive(_lock);
    Serial.printf("[NCM] play url ready id=%lu br=%lu (mp3)\n",
                  (unsigned long)songId, (unsigned long)bestBr);
    return true;
}
