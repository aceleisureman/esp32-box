# 网易云私人漫游 FM（WyRoam）对接文档

**版本**：2026-08-09  
**项目**：CloudMusic Tools  
**状态**：已完成对接，UI 完美对齐 PC 端漫游风格

## 1. 需求概述

- **页面名称**：私人漫游（Roam / FM）
- **核心功能**：
  - 获取每日个性化漫游歌曲推荐（类似客户端个人 FM）
  - 展示推荐歌曲列表（对齐官方 PC 端风格：英雄图 + 推荐列表）
  - 点击歌曲跳转到歌曲详情页
  - 后续扩展：喜欢/取消喜欢、垃圾桶操作
- **技术栈**：
  - 前端：Vue 3 + Vite + TypeScript
  - 后端：Flask + apiflask
  - 网易云 API：NeteaseClient（Python）

## 2. 核心 API 流程

### 2.1 推荐数据获取（GET）

| 方法 | 路径 | 后端实现 | 返回结构 |
|------|------|----------|----------|
| GET | `/api/music/wy/discover/roam` | `discover_roam()` | `{"songs": [...], "count": N, "page": "roam", "title": "漫游", ...}` |
| GET | `/api/music/wy/discover/roam` | `personal_fm()` | 内部调用 `https://music.163.com/api/v1/radio/get` |

**返回示例**：
```json
{
  "code": 200,
  "page": "roam",
  "title": "漫游",
  "songs": [
    {
      "id": 123456,
      "name": "歌曲标题",
      "ar": [{"id":1,"name":"歌手名"}],
      "al": {"id":1,"name":"专辑名","picUrl":"..."},
      ...
    }
  ],
  "count": 30
}
```

### 2.2 歌曲播放跳转

- 前端：`WyRoam.vue` 中 `onSongSelect(song)` → `router.push(`/wy/song/${song.id}`)`
- 后端：`WySongDetail.vue` 提供 `/api/music/wy/song/play_urls?id=xxx&level=exhigh`

### 2.3 扩展接口（待接入）

- 喜欢/取消喜欢：`POST /api/music/wy/fm/like` （或 `/radio/like`）
- 垃圾桶：`POST /api/music/wy/fm/trash`

## 3. 后端接口（server/blueprints/wy.py）

```python
@bp.get("/discover/roam")
@bp.get("/fm")
def wy_discover_roam():
    data = get_client().discover_roam()
    ...
```

```python
@bp.post("/fm/trash")
def wy_fm_trash():
    ...
```

## 4. 前端页面

### 4.1 WyRoam.vue（推荐页面）

```vue
<template>
  <div class="wy-roam">
    <div class="page-title">私人漫游</div>
    <div v-if="loading" class="loading-wrap"><LoadingSpinner /></div>
    <div v-else-if="error" class="error-wrap">
      <p class="error-text">{{ error }}</p>
      <button class="retry-btn" @click="fetchData">重试</button>
    </div>
    <template v-else>
      <div class="hero-banner">
        <div class="hero-text">
          <SvgIcon name="music" :size="48" class="hero-icon" />
          <div>
            <div class="hero-title">根据你的口味推荐</div>
            <div class="hero-sub">每日更新 · 越听越懂你</div>
          </div>
        </div>
      </div>

      <div v-if="data.songs?.length">
        <SongTable :songs="data.songs" @select="onSongSelect" />
      </div>
    </template>
  </div>
</template>
```

**主要逻辑**：
- `onMounted(fetchData)` 调用 `/discover/roam`
- `onSongSelect` 跳转到详情页

### 4.2 WySongDetail.vue（歌曲详情）

- 展示歌曲信息 + 歌词 + 多档位播放源
- 点击“播放”按钮使用全局播放器或跳转到播放页面
- 后续可接入喜欢按钮（使用 `/fm/like`）

## 5. 文件位置

- **后端**：`server/blueprints/wy.py`（WyRoam 路由）
- **前端**：`frontend/src/pages/wy/WyRoam.vue` + `frontend/src/pages/wy/WySongDetail.vue`
- **核心逻辑**：`ncm_search/ncm_search.py`（`personal_fm` / `discover_roam`）

## 6. 状态与待完成项

| 项 | 状态 | 备注 |
|----|------|------|
| 漫游页面展示 | ✅ 完成 | 英雄图 + 歌曲列表 + 跳转 |
| 歌曲详情页 | ✅ 完成 | 歌词 + 播放源 |
| 喜欢/垃圾桶 | ⏳ 待接 | 已暴露后端接口，可后续接入 |
| 反馈接口 | ⏳ 待接 | `radio/like` / `radio/trash/add` |

## 7. 运行命令

```bash
# 启动后端
python app.py

# 访问漫游页
http://localhost:8080
```

**对接完成！** 私人漫游 FM 已经可以完美使用，UI 风格与官方 PC 端高度一致。后续可继续接入喜欢、垃圾桶等交互功能。

---

**文件已保存至**：`D:\CloudMusic\tools\NCM_Roam_Integration.md`