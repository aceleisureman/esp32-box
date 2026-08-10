#pragma once
#ifndef MUSICSERVICE_H
#define MUSICSERVICE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "AudioPlayer.h"

// 对接内网 CloudMusic Tools 后端（Flask），拉取私人漫游歌单、歌词与播放直链。
// 接口来自 NCM_Roam_Integration.md：
//   GET /api/music/wy/discover/roam            → 歌曲列表
//   GET /api/music/wy/song/lyric?id=           → 歌词（LRC）
//   GET /api/music/wy/song/play_urls?id=&level= → 播放直链
//
// 网络请求在独立任务中执行，UI 线程只读快照，不会阻塞刷新。
class MusicServiceClass {
public:
    static constexpr uint8_t  kMaxTracks = 20;
    static constexpr uint16_t kMaxLyrics = 64;

    struct Track {
        uint32_t id = 0;
        char title[48]  = "";
        char artist[32] = "";
        char album[32]  = "";
        char coverUrl[128] = "";
        uint32_t durationMs = 0;
    };

    struct LyricLine {
        uint32_t startMs = 0;
        char     text[48] = "";
    };

    // 听书列表项（/api/music/fm/cell_change）
    struct Book {
        char name[64] = "";
        char author[32] = "";
        char abstract[200] = "";
    };
    static constexpr uint8_t kMaxBooks = 16;

    enum class State : uint8_t {
        Idle,          // 未联网 / 未启动
        Loading,       // 正在拉取歌单
        Ready,         // 歌单就绪
        Failed,        // 拉取失败，等待重试
    };

    void init();
    // 每轮主循环调用；wifiConnected 变化时触发拉取
    void update(bool wifiConnected);

    // ---- UI 线程读取（内部加锁）----
    State    state() const { return _state; }
    uint8_t  trackCount();
    bool     track(uint8_t index, Track &out);
    // 当前选中曲目的歌词行数与内容
    uint16_t lyricCount();
    bool     lyric(uint16_t index, LyricLine &out);
    // 当前选中的曲目下标
    uint8_t  currentIndex() const { return _currentIndex; }

    // ---- UI 线程控制 ----
    void selectTrack(uint8_t index);   // 切歌：请求该曲的歌词与播放链接
    void next();
    void previous();
    void requestRefresh() { _forceRefresh = true; }
    // 拉取听书列表（返回 true 表示已在网络任务中排队）
    void requestBookList() { _needBookList = true; }
    // 书籍列表
    uint8_t bookCount();
    bool    book(uint8_t index, Book &out);
    // 取当前曲目的播放直链（供音频播放器使用），未就绪返回空串
    String playUrl();
    // 当前直链对应的编码格式（AudioPlayer::PlayFormat），未就绪返回 Unknown。
    // 与 playUrl() 配套读取，供播放器选择解码器。
    AudioPlayerClass::PlayFormat playFormat();
    // 封面：返回 RGB565 缓冲指针与尺寸，未就绪返回 nullptr。
    // 缓冲由 MusicService 内部持有，切歌后失效；大小为 88x88。
    const uint16_t *coverPixels() const { return _cover; }
    uint16_t        coverSize()  const { return _coverSize; }
    // 当前曲目的封面是否已解码就绪
    bool     coverReady() const { return _coverReady; }

private:
    static void taskEntry(void *arg);
    void taskLoop();
    bool fetchRoamList();
    bool fetchBookList();
    bool fetchLyrics(uint32_t songId);
    bool fetchPlayUrl(uint32_t songId);
    bool fetchCover(const char *url);
    void setState(State s);

    SemaphoreHandle_t _lock = nullptr;
    TaskHandle_t      _task = nullptr;
    volatile bool     _wifiUp = false;
    volatile bool     _forceRefresh = false;
    volatile bool     _needTrackDetail = false;   // 切歌后需拉歌词/直链
    volatile bool     _needBookList = false;      // 听书列表请求
    volatile State    _state = State::Idle;
    volatile uint8_t  _currentIndex = 0;

    // 受 _lock 保护
    Track     _tracks[kMaxTracks];
    uint8_t   _trackCount = 0;
    LyricLine _lyrics[kMaxLyrics];
    uint16_t  _lyricCount = 0;
    Book      _books[kMaxBooks];
    uint8_t   _bookCount = 0;
    String    _playUrl;
    AudioPlayerClass::PlayFormat _playFormat = AudioPlayerClass::PlayFormat::Unknown;
    // 封面解码结果（仅任务线程写，UI 读；整块替换，单字节写不可见即可）
    uint16_t *_cover = nullptr;
    uint16_t  _coverSize = 0;
    volatile bool _coverReady = false;

    static constexpr uint32_t kRetryMs = 15UL * 1000UL;
};

extern MusicServiceClass MusicService;

#endif // MUSICSERVICE_H
