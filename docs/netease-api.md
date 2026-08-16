# 网易云后端接口调用文档（固件侧）

> 固件（ESP32-S3）调用网易云 CloudMusic Tools 后端的接口契约。
> 适用对象：固件 `src/MusicService.cpp`，后端基址 `http://192.168.28.50:9965`。

## 基本约定

| 项 | 内容 |
|----|------|
| 后端基址 | `http://192.168.28.50:9965`（常量 `kApiBase`，见 `src/MusicService.cpp:16`） |
| 请求方式 | 全部 `GET`，明文 HTTP |
| 请求库 | `HTTPClient`，开启 `useHTTP10(true)` |
| 超时 | 连接/读取 8000ms（`kHttpTimeoutMs`） |
| 客户端选择 | URL 以 `https` 开头走 `WiFiClientSecure + setInsecure()`，否则走 `WiFiClient` |
| 响应判定 | HTTP 状态码 `200` 为成功，其余判失败，不解析错误体 |
| JSON 解析 | 固件用极简字符串扫描（`strstr` + 层级查找），**非完整解析库**，对字段名与嵌套结构敏感 |

## 接口总览

| # | 方法 | 路径 | 用途 | 调用处 |
|---|------|------|------|--------|
| 1 | GET | `/api/music/wy/discover/roam` | 漫游歌单（每日推荐） | `MusicService.cpp:606` |
| 2 | GET | `/api/music/wy/song/lyric?id=` | 歌词（LRC） | `MusicService.cpp:659` |
| 3 | GET | `/api/music/wy/song/play_urls?id=&level=` | 播放直链 | `MusicService.cpp:736` |
| 4 | GET | `/api/music/fm/cell_change` | 听书列表 | `MusicService.cpp:476` |
| 5 | GET | `<picUrl>?param=88y88` | 封面图（网易云 CDN，非后端） | `MusicService.cpp:515` |

---

## 1. 漫游歌单

| 项 | 内容 |
|----|------|
| 方法 | `GET` |
| 路径 | `/api/music/wy/discover/roam` |
| 参数 | 无 |
| 用途 | 拉取一批推荐歌曲，固件最多取前 20 首（`kMaxTracks`） |

### 固件读取字段

层级敏感，逐元素解析 `songs` 数组：

| 字段 | 层级 | 类型 | 用途 | 缓冲上限 |
|------|------|------|------|----------|
| `songs` | 顶层 | array | 歌曲数组 | 20 |
| `id` | song 内 | number | 歌曲 ID | — |
| `name` | song 内 | string | 歌名 | 48 |
| `ar` | song 内 | array | 歌手数组，取 `ar[0].name` | — |
| `ar[].name` | ar 元素内 | string | 歌手名 | 32 |
| `al` | song 内 | object | 专辑对象 | — |
| `al.name` | al 内 | string | 专辑名 | 32 |
| `al.picUrl` | al 内 | string | 封面直链（给接口5用） | 128 |
| `dt` | song 内 | number | 时长(ms) | — |

### 返回示例

```json
{
  "code": 200,
  "page": "roam",
  "title": "漫游",
  "songs": [
    {
      "id": 123456,
      "name": "歌曲标题",
      "ar": [{"id": 1, "name": "歌手名"}],
      "al": {"id": 1, "name": "专辑名", "picUrl": "https://p1.music.126.net/xxx.jpg"},
      "dt": 234567
    }
  ],
  "count": 30
}
```

### 关键约束

- `id` 缺失或为 0 → 该曲被跳过。
- `ar` 必须是数组，固件只取第一个元素的 `name`。
- `al` 内若嵌套同名 `artist` 子对象（同样含 `name`/`picUrl`），固件用层级查找规避——后端若改字段结构会受影响。

---

## 2. 歌词

| 项 | 内容 |
|----|------|
| 方法 | `GET` |
| 路径 | `/api/music/wy/song/lyric?id=<songId>` |
| 参数 | `id`：歌曲 ID（必填，来自接口1的 `id`） |
| 用途 | 取 LRC 歌词，逐行解析时间戳，最多 64 行（`kMaxLyrics`） |

### 固件读取字段

| 字段 | 层级 | 类型 | 用途 |
|------|------|------|------|
| `lrc.lyric` | 顶层.lrc 内 | string | 原始 LRC 文本（含 `\n` 转义） |

### 返回示例

```json
{
  "code": 200,
  "lrc": {
    "lyric": "[00:00.00]词\n[00:05.00]词\n"
  }
}
```

### 关键约束

- 固件用全局 `strstr` 查找 `"lyric"`，取其后的字符串值。要求 `lyric` 在整个 JSON 中唯一或最靠前。
- 缓冲上限 4096 字节（`raw[4096]`），超长歌词会被截断。
- 解析支持多时间戳压缩 LRC（`[t1][t2]词`），每个时间戳生成一行；按时间升序插入排序。
- 跳过空行与制作人员信息行（含「演唱 : 」「作词 : 」等关键词且带 ` : ` 分隔符）。
- 每行文本缓冲上限 48 字节（`text[48]`）。

---

## 3. 播放直链

| 项 | 内容 |
|----|------|
| 方法 | `GET` |
| 路径 | `/api/music/wy/song/play_urls?id=<songId>&level=standard` |
| 参数 | `id`：歌曲 ID（必填）；`level`：音质档位，固件固定传 `standard` |
| 用途 | 取可播放直链，固件筛选 `type=="mp3"` 中码率最高的一条 |

### 固件读取字段

遍历 `items` 数组，最多 8 个元素：

| 字段 | 层级 | 类型 | 用途 |
|------|------|------|------|
| `items` | 顶层 | array | 播放源数组 |
| `items[].type` | 元素内 | string | 编码类型，固件只接受 `"mp3"` |
| `items[].url` | 元素内 | string | 播放直链 |
| `items[].br` | 元素内 | number | 码率，用于选最高码率 |

### 返回示例

```json
{
  "code": 200,
  "items": [
    {"type": "mp3", "br": 128000, "url": "https://..."},
    {"type": "mp3", "br": 320000, "url": "https://..."}
  ]
}
```

### 关键约束

- 非 `mp3` 类型一律跳过（即便 `level` 下后端返回 FLAC 也不用）。
- `url` 为空或缺失 → 跳过该源。
- 无任何可用 `mp3` 源 → 返回失败，设备不播放。
- URL 缓冲上限 256 字节。
- 选定后固件标记播放格式为 `Mp3`。

### 音质档位对照

| level | 名称 | 码率 br | 说明 |
|-------|------|---------|------|
| standard | 标准 | 128000 | 当前固件使用 |
| higher | 较高 | 192000 | — |
| exhigh | 极高 | 320000 | 代码注释推荐，ESP32-S3 可解码 |
| lossless | 无损 | 999000 | FLAC，未启用 |
| hires | Hi-Res | 999000 | 高解析，未启用 |

> 待确认：注释建议用 `exhigh`，但实际代码传 `standard`，是否需要统一。

---

## 4. 听书列表

| 项 | 内容 |
|----|------|
| 方法 | `GET` |
| 路径 | `/api/music/fm/cell_change` |
| 参数 | 无 |
| 用途 | 拉取听书单元，最多 16 本（`kMaxBooks`） |

### 固件读取字段

遍历 `books` 数组：

| 字段 | 层级 | 类型 | 用途 | 缓冲上限 |
|------|------|------|------|----------|
| `books` | 顶层 | array | 书籍数组 | 16 |
| `books[].book_name` | 元素内 | string | 书名 | 64 |
| `books[].author` | 元素内 | string | 作者 | 32 |
| `books[].abstract` | 元素内 | string | 摘要 | 200 |

### 返回示例

```json
{
  "code": 200,
  "books": [
    {"book_name": "书名", "author": "作者", "abstract": "摘要文本"}
  ]
}
```

### 关键约束

- `book_name` 为空 → 该条跳过。
- UI 展示逻辑见 `src/Display.cpp:3832`。

---

## 5. 封面下载（网易云 CDN，非后端接口）

| 项 | 内容 |
|----|------|
| 方法 | `GET` |
| 路径 | `<al.picUrl>?param=88y88` |
| 主机 | 网易云 CDN（如 `p1.music.126.net`），非 `kApiBase` |
| 协议 | 通常 HTTPS → 走 `WiFiClientSecure + setInsecure()` |
| 用途 | 下载 88×88 缩放 JPEG，解码为 RGB565 |

### 关键约束

- URL 来自接口1的 `al.picUrl`，固件自动追加 `?param=88y88` 请求缩略图。
- 下载缓冲上限 48KB；解码目标 88×88 RGB565。
- 失败时保持旧封面或不显示。
- 此接口不经后端，CDN 协议若为 HTTPS 仍走加密通道，与改后端 IP 无关。

---

## 固件侧解析约束汇总

后端改动需注意以下约束，否则固件解析会失效：

| 约束 | 说明 |
|------|------|
| 非完整 JSON 解析 | 固件用 `strstr` + 层级扫描，对字段名与嵌套结构敏感，后端改字段名/层级会直接失效 |
| 字段唯一性 | `lyric`、`songs`、`items`、`books` 用全局查找，需在 JSON 中唯一或最靠前 |
| 字符串长度上限 | title 48 / artist 32 / album 32 / coverUrl 128 / 歌词行 48 / 书名 64 / 摘要 200，超长截断 |
| HTTP/1.0 兼容 | `useHTTP10(true)`，后端需兼容 HTTP/1.0 无分块响应 |
| 错误处理 | 非 200 即判失败，不解析错误体 |

---

## 待确认项

- 接口3 的 `level` 实际传 `standard`，但注释推荐 `exhigh`，是否需要改回 `exhigh`？
- 若后端字段名/返回结构有变化，需同步核对固件解析逻辑。
