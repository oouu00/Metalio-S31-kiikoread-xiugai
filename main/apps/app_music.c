/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：完整播放页（二级页，仅 metalio47）。曲名与进度、上一首/播放/下一首、
 * 音量、单行歌词（歌名下方）、播放列表与翻页、行内收藏，以及全屏
 * 「全部歌曲」列表（曲库有子文件夹时按目录层级浏览，没有则平铺收藏优先；
 * 按键或按钮翻页）。文字随系统字号按比例缩放（各元素保持
 * 自身基准字号，不统一放大）。播放本身由 music_player 后台任务负责，本页只读
 * 状态、发指令；切走后音乐继续。
 *
 * English: the full player (second-level page, metalio47 only). Title and progress,
 * prev/play/next, volume, one lyric line under the title, playlist
 * and paging, per-row favorites, and the full-screen "all tracks" list (favorites
 * first, paged by keys). Text scales proportionally with the system font size (each
 * element keeps its own base; nothing is flattened to one size). music_player owns
 * playback in the background; this page only reads state and issues commands, and
 * music keeps playing after navigating away.
 *
 * 冻结：不做拖动式进度（2026-10-29 用户要求点按进度条跳转，已完成；scrub 仍不提供）、
 * 不做随机播放、不做内嵌封面；耳机只做基础控制；蓝牙连接/扫描/配对管理只在设置页
 * （2026-10-29 用户确认播放页与设置页重复，播放页不再提供蓝牙界面）。
 * Frozen: no scrubbing (the user asked for tap-to-seek on 2026-10-29, done; drag scrub
 * stays out), no shuffle, no embedded cover art; the headset gets basic controls only;
 * Bluetooth connect/scan/pairing management lives only in Settings (the user confirmed
 * on 2026-10-29 that the player duplicated it, so the player no longer shows Bluetooth).
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app.h"
#include "app_registry.h"
#include "bt_host.h"
#include "esp_log.h"
#include "music_lyrics.h"
#include "music_player.h"
#include "settings.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"

static const char *TAG = "app_music";

#define MUSIC_ROWS 4                      // 播放列表每页行数 / playlist rows per page
#define FULL_ROWS 12                      // 全部歌曲每页行数（不滚动，按键翻页）/ full-list rows per page (paged, never scrolled)
// 内容脏区：顶部模式按钮到翻页键（模式按钮在曲名之上，也要一起刷）。
// Content dirty span: from the top mode button through the pager (the mode button sits above
// the title and must be repainted with it).
#define MUSIC_DIRTY ((EpdRect){36,96,612,972})
#define MODE_RECT ((EpdRect){492,96,156,46})      // 播放模式胶囊：列表/单曲/随机/收藏 / play-mode capsule
#define OUTPUT_RECT ((EpdRect){312,96,168,46})    // 输出通路胶囊：蓝牙耳机/本机扬声器 / output route capsule
// 重连胶囊：模组彻底断电重启（等同关开蓝牙开关），无声时的手动兜底。放在输出胶囊左边。
// Relink capsule: full power-cycle of the module (a manual switch toggle), the fallback when
// there is no sound. Sits left of the output capsule.
#define RESTART_RECT ((EpdRect){176,96,128,46})
#define FULL_DIRTY ((EpdRect){36,140,612,932})   // 全屏列表脏区：标题到翻页键 / full-list dirty span: title to the pager
#define LYRIC_DIRTY ((EpdRect){36,300,612,60})   // 歌词行：换句只刷这一行，进度条走自己的窄条 / the lyric line only: a new line refreshes just it, the bar has its own sliver

static int s_page;                        // 播放列表页 / playlist page
static int s_seen_track = -1;             // 已跟随的曲目索引 / last track index we paged to
static char s_message[96];
static bool s_show_all;                   // 全屏「全部歌曲」视图 / the full-screen "all tracks" view
static int s_full_page;                   // 全屏列表页 / full-list page
static int s_order[MUSIC_MAX_TRACKS];     // 展示顺序：收藏优先（曲库下标）/ display order, favorites first (library indices)
static int s_order_count;
// on_tick 快照：当前歌词行与 music_serial。/ on_tick snapshot: the current lyric line plus music_serial.
static char s_lyric_seen[192 + 1];
static uint32_t s_serial;
// 进度增量刷新：只推条上新增的一窄条与变化的时间标签，避免整块反复刷新。
// Incremental progress refresh: push only the new sliver of the bar and the changed time
// label instead of repainting a big block over and over.
static int s_bar_drawn;        // 最近一次绘制已达宽度 / filled width in the latest render
static int s_bar_shown;        // 屏上已达宽度 / filled width now on the panel
static bool s_tick_partial;    // 本轮按增量刷 / this round takes the incremental path

static EpdRect s_pressed;
static int s_press_radius;
static bool s_press_pending;
// 重连按钮的「重启中」读秒：从点击起 9 s 内显示重启中（回模式 1 等应答最多 4 s + 重建后端
// + 重进模式 2 + 重连耳机）。不断电，所以没有 3 s 冷启动那一段。
// Countdown for the relink capsule: "restarting" for 9 s after the tap (leave mode 1 up to 4 s,
// backend rebuild, mode-2 re-entry, reconnection). No power cut, so no 3 s cold boot.
static TickType_t s_restart_tick;
#define RESTART_SHOW_MS 9000

static bool restarting(void) {
    return s_restart_tick && (int32_t)(xTaskGetTickCount() - s_restart_tick) <
                                 pdMS_TO_TICKS(RESTART_SHOW_MS);
}

static void fit(char *text, int px, int width) {
    while (*text && ui_text_fixed_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((uint8_t)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}
// 系统字号按比例换算：每个文本按自己的基准字号缩放，不统一放大到同一档。
// Scale text by the system font ratio: every element keeps its own base size instead
// of being flattened to one size.
static int sy(int px) { return (px * app_settings_system_font_size() + 50) / 100; }
static void mmss(char *dst, size_t cap, uint32_t ms) {
    uint32_t s = ms / 1000;
    snprintf(dst, cap, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}
static bool hit(EpdRect r, int x, int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}
static bool hit_circle(int cx, int cy, int radius, int x, int y) {
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}
static int page_count(void) {
    int count = music_track_count();
    return (count + MUSIC_ROWS - 1) / MUSIC_ROWS;
}
// 收藏优先排序：先收藏组、后其余，组内保持曲库顺序。每次绘制重建（收藏位在 RAM 缓存）。
// Favorites-first order: favorites, then the rest, each group in library order. Rebuilt per
// frame (the favorite bits live in a RAM cache, so this never touches NVS).
static void rebuild_order(void) {
    const int count = music_track_count();
    s_order_count = 0;
    for (int fav = 1; fav >= 0; --fav)
        for (int i = 0; i < count && s_order_count < MUSIC_MAX_TRACKS; ++i)
            if ((music_favorite(i) ? 1 : 0) == fav) s_order[s_order_count++] = i;
}
static int full_page_count(void) {
    int pages = (s_order_count + FULL_ROWS - 1) / FULL_ROWS;
    return pages < 1 ? 1 : pages;
}

/* ---- 曲库子文件夹：层级浏览 / Sub-folders: hierarchy browsing ---- */
#define DIR_MAX 40        // 每层识别的子文件夹上限 / sub-folders recognized per level
#define DIR_NAME_MAX 72   // 文件夹显示名长度 / folder display-name length

static bool s_browse;              // 曲库有子文件夹 → 按层级浏览 / hierarchy mode: sub-folders exist
static char s_dir[160];            // 当前目录（相对 music 根，"" 为根）/ current folder relative to the root
static int s_entry_page;           // 层级模式页码 / page in hierarchy mode
static int s_level_dirs;           // 本层子文件夹数 / sub-folders at this level
static int s_dir_first[DIR_MAX];   // 每个子文件夹的代表曲目 / a representative track per sub-folder
// s_order / s_order_count 在层级模式下复用为「本层曲目清单（收藏优先）」。
// In hierarchy mode s_order/s_order_count hold this level's tracks (favorites first).

// 曲目所在目录（相对曲库根）："" 是根，其余形如 "周杰伦" 或 "周杰伦/叶惠美"。
// The track's folder relative to the library root: "" is the root, else like "Artist/Album".
static void track_dir(int idx, char *out, size_t cap) {
    out[0] = 0;
    const music_track_t *t = music_track(idx);
    if (!t) return;
    const char *root = music_root();
    const char *p = t->path;
    const size_t n = root ? strlen(root) : 0;
    if (n && strncmp(p, root, n) == 0) {
        p += n;
        while (*p == '/') ++p;
    }
    const char *slash = strrchr(p, '/');
    if (!slash) return;
    size_t len = (size_t)(slash - p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = 0;
}
// 当前层下面的一级目录名（曲目在更深层时只取第一段）。/ The first component below the current folder.
static void child_name(int idx, char *out, size_t cap) {
    out[0] = 0;
    char dir[256];
    track_dir(idx, dir, sizeof(dir));
    const size_t n = strlen(s_dir);
    if (n) {
        if (strncmp(dir, s_dir, n) != 0 || dir[n] != '/') return;
    } else if (!dir[0]) {
        return;  // 根目录下的散曲 / a loose track at the root
    }
    const char *rest = dir + (n ? n + 1 : 0);
    if (!*rest) return;  // 正好是本层 / exactly this folder
    const char *slash = strchr(rest, '/');
    size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (len >= cap) len = cap - 1;
    memcpy(out, rest, len);
    out[len] = 0;
}
// 曲目是否就在该目录里（不含其子目录）。/ Whether the track sits directly in that folder.
static bool dir_is(int idx, const char *dir) {
    char d[256];
    track_dir(idx, d, sizeof(d));
    return strcmp(d, dir) == 0;
}
// 曲目是否在该目录或其子目录下。/ Whether the track lives in that folder or below it.
static bool dir_holds(int idx, const char *dir) {
    char d[256];
    track_dir(idx, d, sizeof(d));
    if (!dir[0]) return true;
    const size_t n = strlen(dir);
    return strncmp(d, dir, n) == 0 && (d[n] == 0 || d[n] == '/');
}
static int dir_track_count(const char *dir) {
    int n = 0;
    for (int i = 0, c = music_track_count(); i < c; ++i)
        if (dir_holds(i, dir)) ++n;
    return n;
}
// 曲库里是否存在子文件夹：是则「全部歌曲」按层级显示，否则沿用原平铺列表。
// Whether the library has sub-folders: if so the all-tracks view goes hierarchical, else it
// keeps the original flat list.
static bool library_has_dirs(void) {
    for (int i = 0, c = music_track_count(); i < c; ++i) {
        char d[256];
        track_dir(i, d, sizeof(d));
        if (d[0]) return true;
    }
    return false;
}
// 重建当前层：子文件夹（按名排序）在前，本层曲目（收藏优先）在后。
// Rebuild this level: sub-folders (name-sorted) first, then this folder's tracks (favorites first).
static void level_build(void) {
    const int count = music_track_count();
    s_level_dirs = 0;
    s_order_count = 0;
    for (int i = 0; i < count; ++i) {
        char name[DIR_NAME_MAX];
        child_name(i, name, sizeof(name));
        if (!name[0]) continue;
        bool dup = false;
        for (int k = 0; k < s_level_dirs && !dup; ++k) {
            char other[DIR_NAME_MAX];
            child_name(s_dir_first[k], other, sizeof(other));
            if (strcmp(other, name) == 0) dup = true;
        }
        if (!dup && s_level_dirs < DIR_MAX) s_dir_first[s_level_dirs++] = i;
    }
    for (int a = 1; a < s_level_dirs; ++a) {  // 插入排序，每层至多几十项 / insertion sort, a few dozen at most
        const int key = s_dir_first[a];
        char key_name[DIR_NAME_MAX];
        child_name(key, key_name, sizeof(key_name));
        int b = a - 1;
        while (b >= 0) {
            char other[DIR_NAME_MAX];
            child_name(s_dir_first[b], other, sizeof(other));
            if (strcmp(other, key_name) <= 0) break;
            s_dir_first[b + 1] = s_dir_first[b];
            --b;
        }
        s_dir_first[b + 1] = key;
    }
    for (int fav = 1; fav >= 0; --fav)
        for (int i = 0; i < count && s_order_count < MUSIC_MAX_TRACKS; ++i)
            if (music_favorite(i) == (fav != 0) && dir_is(i, s_dir)) s_order[s_order_count++] = i;
}
static int level_total(void) { return s_level_dirs + s_order_count; }
static int browse_page_count(void) {
    int pages = (level_total() + FULL_ROWS - 1) / FULL_ROWS;
    return pages < 1 ? 1 : pages;
}
// 进入子文件夹 / 回到上一级（已在根则清空）。/ Enter a sub-folder / go one level up (already at the root).
static void dir_enter(const char *name) {
    if (!name[0]) return;
    char path[sizeof(s_dir)];
    if (s_dir[0]) snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    else strlcpy(path, name, sizeof(path));
    strlcpy(s_dir, path, sizeof(s_dir));
    s_entry_page = 0;
}
static void dir_up(void) {
    char *slash = strrchr(s_dir, '/');
    if (slash) *slash = 0;
    else s_dir[0] = 0;
    s_entry_page = 0;
}

typedef struct { EpdRect r; int radius; } music_hit_t;

// 当前视图的内容脏区。/ The active view's content dirty span.
static EpdRect current_dirty(void) {
    return s_show_all ? FULL_DIRTY : MUSIC_DIRTY;
}
// 只取当前这句歌词，换句即触发刷新。/ Only the current line; a new line redraws.
static void lyric_snapshot(char *dst, size_t cap) {
    snprintf(dst, cap, "%s", music_lyrics_at(music_elapsed_ms()));
}
// 把歌词快照同步（进出视图与动作后调用，避免 on_tick 紧接着重复刷）。
// Sync the lyric snapshot (called around view switches and actions so on_tick does not
// redraw right after).
static void sync_lyric(void) { lyric_snapshot(s_lyric_seen, sizeof(s_lyric_seen)); }

// 按压反馈与命中共用同一套矩形；圆形键用外接方框做反馈。/ Press feedback and hits share one table; round keys feed their bounding box.
static music_hit_t target_at(int x, int y) {
    if (s_show_all) {
        for (int i = 0; i < FULL_ROWS; ++i)
            if (hit((EpdRect){44,196+i*64,596,56}, x, y)) return (music_hit_t){{44,196+i*64,596,56},14};
        if (hit((EpdRect){36,1012,144,52}, x, y)) return (music_hit_t){{36,1012,144,52},26};
        if (hit((EpdRect){492,1012,144,52}, x, y)) return (music_hit_t){{492,1012,144,52},26};
        return (music_hit_t){{0,0,0,0},0};
    }
    if (hit(MODE_RECT, x, y)) return (music_hit_t){MODE_RECT, 23};  // 播放模式 / play mode
    if (hit(OUTPUT_RECT, x, y)) return (music_hit_t){OUTPUT_RECT, 23};  // 输出通路 / output route
    if (hit(RESTART_RECT, x, y)) return (music_hit_t){RESTART_RECT, 23};  // 重连兜底 / relink
    // 进度条连同两侧时间共用一个命中区：点按跳转。/ The bar and both times share one hit: tap to seek.
    if (hit((EpdRect){48,414,588,66}, x, y)) return (music_hit_t){{48,414,588,66},14};
    if (hit_circle(222,540,40,x,y)) return (music_hit_t){{186,504,72,72},36};
    if (hit_circle(342,540,52,x,y)) return (music_hit_t){{294,492,96,96},48};
    if (hit_circle(462,540,40,x,y)) return (music_hit_t){{426,504,72,72},36};
    if (hit((EpdRect){190,612,64,56}, x, y)) return (music_hit_t){{190,612,64,56},28};
    if (hit((EpdRect){430,612,64,56}, x, y)) return (music_hit_t){{430,612,64,56},28};
    if (hit((EpdRect){468,704,160,40}, x, y)) return (music_hit_t){{468,704,160,40},20};  // 全部歌曲 / all tracks
    for (int i = 0; i < MUSIC_ROWS; ++i)
        if (hit((EpdRect){44,748+i*64,596,56}, x, y)) return (music_hit_t){{44,748+i*64,596,56},14};
    if (hit((EpdRect){36,1012,144,52}, x, y)) return (music_hit_t){{36,1012,144,52},26};
    if (hit((EpdRect){492,1012,144,52}, x, y)) return (music_hit_t){{492,1012,144,52},26};
    return (music_hit_t){{0,0,0,0},0};
}

// 动作后统一出口：同步快照避免 on_tick 紧接着重复刷一遍。/ Shared action exit: sync the snapshot so on_tick does not redraw again.
static EpdRect s_area;
static app_redraw_t dirty(void) {
    s_serial = music_serial();
    sync_lyric();
    s_area = current_dirty();
    return APP_REDRAW_AREA;
}

static void draw_frame(uint8_t *fb, EpdRect box, int radius) {
    for (int i = 0; i < 3; ++i) ui_draw_round_rect(fb, ui_inset_rect(box, i), radius > i ? radius - i : 0, 0x30);
}
static void circle_frame(uint8_t *fb, int x, int y, int radius) {
    for (int i = 0; i < 3 && i < radius; ++i) epd_draw_circle(x, y, radius - i, 0x30, fb);
}
// 播放三角 / 暂停双竖杠（白色画在深色圆上或深色画在白圆上由 caller 定）。
// Play triangle / pause bars; the caller decides dark-on-light or light-on-dark.
static void play_glyph(uint8_t *fb, int cx, int cy, int height, int width, uint8_t color, bool playing) {
    int top = cy - height / 2;
    if (playing) {
        int bar = width / 4;
        ui_fill_rect_fast((EpdRect){cx - width / 2, top, bar, height}, color, fb);
        ui_fill_rect_fast((EpdRect){cx + width / 2 - bar, top, bar, height}, color, fb);
    } else {
        // 中空三角：只描左边竖线与两条斜边，中间留白。/ Hollow triangle: left edge plus both slants.
        const int stroke = height / 16 + 1;
        int x0 = cx - width / 2, half = height / 2;
        for (int i = 0; i < height; ++i) {
            int w = (i < half ? i : height - 1 - i) * width / half;
            if (w < 1) w = 1;
            if (w + 1 <= stroke * 2) {
                // 靠近尖端：两笔并成一行，避免出现断口。/ Near the tip the two strokes merge.
                ui_fill_rect_fast((EpdRect){x0, top + i, w + 1, 1}, color, fb);
            } else {
                ui_fill_rect_fast((EpdRect){x0, top + i, stroke, 1}, color, fb);
                ui_fill_rect_fast((EpdRect){x0 + w + 1 - stroke, top + i, stroke, 1}, color, fb);
            }
        }
    }
}

// 一行文件夹：图标 + 名字 + 曲目数；含当前播放曲目的文件夹用打开图标标出。
// One folder row: icon, name and track count; the folder holding the current track gets the
// open-folder icon.
static void folder_row(uint8_t *fb, EpdRect row, const char *name, int kids, bool holds, bool pressed) {
    char text[DIR_NAME_MAX];
    ui_fill_round_rect(fb, row, 14, UI_GRAY_WHITE);
    if (holds || pressed) ui_draw_round_rect(fb, row, 14, UI_GRAY_BLACK);
    ui_draw_icon(fb, 74, row.y + 28, 22, holds ? UI_ICON_FOLDER_OPEN : UI_ICON_FOLDER,
                 holds ? 0x20 : 0x60);
    strlcpy(text, name, sizeof(text));
    fit(text, sy(22), 420);
    ui_text_fixed_vc(fb, 110, row.y + 28, sy(22), text, EPD_DRAW_ALIGN_LEFT, false);
    char t[32];
    snprintf(t, sizeof(t), "%d 首", kids);
    ui_text_fixed_ink_vc(fb, 620, row.y + 28, sy(18), t, EPD_DRAW_ALIGN_RIGHT, holds ? 0x30 : 0x60);
}
// 一行曲目：书签列切换收藏、时长靠右。/ One track row: bookmark column and duration on the right.
static void track_row(uint8_t *fb, EpdRect row, int idx, bool selected, bool pressed) {
    const music_track_t *track = music_track(idx);
    if (!track) return;
    char text[192];
    ui_fill_round_rect(fb, row, 14, UI_GRAY_WHITE);
    if (selected || pressed) ui_draw_round_rect(fb, row, 14, UI_GRAY_BLACK);
    const bool fav = music_favorite(idx);
    ui_draw_icon(fb, 74, row.y + 28, 22, fav ? UI_ICON_BOOKMARK_CHECK : UI_ICON_BOOKMARK,
                 fav ? 0x20 : 0xa8);
    strlcpy(text, track->title, sizeof(text));
    fit(text, sy(22), 430);
    ui_text_fixed_vc(fb, 110, row.y + 28, sy(22), text, EPD_DRAW_ALIGN_LEFT, false);
    if (track->duration_ms) {
        char t[16];
        mmss(t, sizeof(t), track->duration_ms);
        ui_text_fixed_ink_vc(fb, 620, row.y + 28, sy(18), t, EPD_DRAW_ALIGN_RIGHT, 0x50);
    }
}

// 全屏「全部歌曲」：曲库有子文件夹时按目录层级浏览（点文件夹进下一层、返回键回上一层），
// 没有子文件夹时沿用原平铺列表（收藏优先）。点书签列切换收藏、点行播放；不滚动，靠按键或
// 按钮翻页。
// Full-screen "all tracks": with sub-folders it browses the folder tree (tap a folder to descend,
// back to go up); a flat library keeps the original favorites-first list. Tap the bookmark column
// to toggle a favorite, the row to play; no scrolling — page with the keys or the on-screen
// buttons.
static void render_full_list(uint8_t *fb) {
    char text[192];
    const int total = music_track_count();
    int pages = 1;
    if (s_browse) {
        level_build();
        // 目录消失（换卡、改名）：回根，别停在一个空层。
        // Folder gone (new card, rename): back to the root instead of an empty level.
        if (s_dir[0] && level_total() == 0) {
            s_dir[0] = 0;
            s_entry_page = 0;
            level_build();
        }
        pages = browse_page_count();
        if (s_entry_page >= pages) s_entry_page = pages - 1;
        if (s_entry_page < 0) s_entry_page = 0;
        if (s_dir[0]) {
            // 「‹ 路径」提示返回键是回上一层，不是退出列表。
            // "‹ path" hints that Back goes up a level instead of leaving the view.
            snprintf(text, sizeof(text), "\xe2\x80\xb9 %s", s_dir);
            fit(text, sy(26), 430);
        } else {
            snprintf(text, sizeof(text), "全部歌曲 · %d 首", total);
        }
        ui_text_fixed_vc(fb, 60, 158, sy(26), text, EPD_DRAW_ALIGN_LEFT, false);
        if (s_dir[0]) snprintf(text, sizeof(text), "%d 首", s_order_count);
        else snprintf(text, sizeof(text), "%d 个文件夹", s_level_dirs);
        ui_text_fixed_ink_vc(fb, 624, 160, sy(18), text, EPD_DRAW_ALIGN_RIGHT, 0x60);
    } else {
        rebuild_order();
        pages = full_page_count();
        if (s_full_page >= pages) s_full_page = pages - 1;
        if (s_full_page < 0) s_full_page = 0;
        snprintf(text, sizeof(text), "全部歌曲 · %d 首", total);
        ui_text_fixed_vc(fb, 60, 158, sy(26), text, EPD_DRAW_ALIGN_LEFT, false);
        ui_text_fixed_ink_vc(fb, 624, 160, sy(18), "已收藏优先", EPD_DRAW_ALIGN_RIGHT, 0x60);
    }

    const int current = music_track_index();
    const int page = s_browse ? s_entry_page : s_full_page;
    const int entries = s_browse ? level_total() : s_order_count;
    for (int i = 0; i < FULL_ROWS; ++i) {
        const int pos = page * FULL_ROWS + i;
        if (pos >= entries) break;
        EpdRect row = {44,196+i*64,596,56};
        const bool pressed = s_press_pending && s_pressed.x == row.x && s_pressed.y == row.y;
        if (s_browse && pos < s_level_dirs) {
            char name[DIR_NAME_MAX];
            child_name(s_dir_first[pos], name, sizeof(name));
            if (!name[0]) continue;
            char full[256];
            if (s_dir[0]) snprintf(full, sizeof(full), "%s/%s", s_dir, name);
            else strlcpy(full, name, sizeof(full));
            folder_row(fb, row, name, dir_track_count(full),
                       current >= 0 && dir_holds(current, full), pressed);
        } else {
            const int idx = s_browse ? s_order[pos - s_level_dirs] : s_order[pos];
            track_row(fb, row, idx, idx == current, pressed);
        }
    }
    // 翻页行与播放列表页同一套坐标。/ The pager shares the playlist's coordinates.
    const int cur_page = s_browse ? s_entry_page : s_full_page;
    const bool p_left = s_press_pending && s_pressed.x == 36 && s_pressed.y == 1012;
    const bool p_right = s_press_pending && s_pressed.x == 492 && s_pressed.y == 1012;
    ui_fill_round_rect(fb, (EpdRect){36,1012,144,52}, 26, UI_GRAY_WHITE);
    draw_frame(fb, (EpdRect){36,1012,144,52}, 26);
    ui_text_fixed_ink_vc(fb, 108, 1038, sy(22), "上一页", EPD_DRAW_ALIGN_CENTER,
                         cur_page > 0 ? 0x20 : 0xb0);
    ui_fill_round_rect(fb, (EpdRect){492,1012,144,52}, 26, UI_GRAY_WHITE);
    draw_frame(fb, (EpdRect){492,1012,144,52}, 26);
    ui_text_fixed_ink_vc(fb, 564, 1038, sy(22), "下一页", EPD_DRAW_ALIGN_CENTER,
                         cur_page + 1 < pages ? 0x20 : 0xb0);
    snprintf(text, sizeof(text), "%d / %d", cur_page + 1, pages);
    ui_text_fixed_ink_vc(fb, 342, 1038, sy(20), text, EPD_DRAW_ALIGN_CENTER, 0x60);
    if (s_press_pending)
        ui_draw_selected_round_rect(fb, s_pressed,
                                    s_pressed.width == 596 ? 14 : s_press_radius);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    ui_fill_rect_fast((EpdRect){0,0,UI_LOCK_WIDTH,UI_LOCK_HEIGHT},0xff,fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    // 音乐是底栏第 3 栏，播放页和全屏列表都常驻底栏，随时能切走。
    // Music is the 3rd tab; both the player and the full list keep the bar so the user can leave.
    ui_nav_draw(fb, 2);
    if (s_show_all) {
        render_full_list(fb);
        return;
    }
    // 播放模式胶囊：点一下循环切换。/ Play-mode capsule: tap to cycle.
    {
        EpdRect box = MODE_RECT;
        ui_fill_round_rect(fb, box, 23, 0xf0);
        ui_draw_round_rect(fb, box, 23, 0x30);
        ui_text_fixed_vc(fb, box.x + box.width / 2, box.y + box.height / 2, sy(20),
                         music_mode_label(), EPD_DRAW_ALIGN_CENTER, false);
    }
    // 输出通路胶囊：蓝牙耳机 / 本机扬声器，点一下切换。两种状态同一套观感——白底、细边框、
    // 黑字，只有文字不同（用户要求与「蓝牙」胶囊一致，不要再出现整块深色）。
    // Output capsule: headset / local speaker, tap to toggle. Both states share one look — white
    // fill, thin border, black text; only the word differs (the user asked for the same treatment
    // as the Bluetooth capsule, with no solid dark block).
    {
        EpdRect box = OUTPUT_RECT;
        ui_fill_round_rect(fb, box, 23, 0xf0);
        ui_draw_round_rect(fb, box, 23, 0x30);
        ui_text_fixed_vc(fb, box.x + box.width / 2, box.y + box.height / 2, sy(20),
                         music_output() == MUSIC_OUT_SPK ? "扬声器" : "蓝牙",
                         EPD_DRAW_ALIGN_CENTER, false);
    }
    // 重连胶囊：点一下让模组整条断电重启（几秒）。重启期间显示「重启中」，到点自动变回「重连」。
    // Relink capsule: tap to power-cycle the module (a few seconds). It reads "重启中" until the
    // countdown ends and then returns to "重连" on its own.
    {
        EpdRect box = RESTART_RECT;
        const bool busy = restarting();
        ui_fill_round_rect(fb, box, 23, 0xf0);
        ui_draw_round_rect(fb, box, 23, busy ? 0x80 : 0x30);
        ui_text_fixed_vc(fb, box.x + box.width / 2, box.y + box.height / 2, sy(20),
                         busy ? "重启中" : "重连", EPD_DRAW_ALIGN_CENTER, false);
    }
    char text[192];
    music_state_t state = music_state();

    // 曲名两行 + 艺人 / Title (up to two lines) + artist
    strlcpy(text, music_title(), sizeof(text));
    if (!text[0]) strlcpy(text, state == MUSIC_ERROR ? music_error() : "暂无音乐", sizeof(text));
    char line2[192] = "";
    const int title_px = sy(34);
    if (ui_text_fixed_width_px(title_px, text) > 564) {
        size_t cut = strlen(text);
        while (cut) {
            char save = text[cut];
            text[cut] = 0;
            bool fits = ui_text_fixed_width_px(title_px, text) <= 564;
            text[cut] = save;
            if (fits) break;
            --cut;
            while (cut && ((uint8_t)text[cut] & 0xc0) == 0x80) --cut;
        }
        strlcpy(line2, text + cut, sizeof(line2));
        text[cut] = 0;
        fit(line2, title_px, 564);
    }
    // 上方大片空白留给曲名/歌词：整体从 280 上移到 190，歌词不再贴着进度条。
    // The empty band above now hosts the title and lyric: everything moves up from 280 to 190 so
    // the lyric no longer hugs the progress bar.
    ui_text_fixed_vc(fb, 342, 190, title_px, text, EPD_DRAW_ALIGN_CENTER, false);
    if (line2[0]) ui_text_fixed_vc(fb, 342, 232, title_px, line2, EPD_DRAW_ALIGN_CENTER, false);
    const char *artist = music_artist();
    if (artist[0]) {
        char a[64];
        strlcpy(a, artist, sizeof(a));
        fit(a, sy(22), 564);
        ui_text_fixed_ink_vc(fb, 342, 262, sy(22), a, EPD_DRAW_ALIGN_CENTER, 0x60);
    }

    // 歌词只显示当前一句，放在歌名下方；存储卡插拔提示占用同一行。
    // Only the current lyric line shows, below the title; a card notice takes the same row.
    if (s_message[0]) {
        ui_text_fixed_ink_vc(fb, 342, 330, sy(18), s_message, EPD_DRAW_ALIGN_CENTER, 0x60);
    } else {
        const char *lyric = music_lyrics_at(music_elapsed_ms());
        if (lyric[0]) {
            char l[192];
            strlcpy(l, lyric, sizeof(l));
            fit(l, sy(20), 564);
            ui_text_fixed_vc(fb, 342, 330, sy(20), l, EPD_DRAW_ALIGN_CENTER, false);
        }
    }

    // 进度 + 两侧时间 / progress bar + times on both sides
    uint32_t dur = music_duration_ms(), el = music_elapsed_ms();
    EpdRect bar = {60,430,564,8};
    int filled = 0;
    if (dur) filled = (int)((uint64_t)564 * (el > dur ? dur : el) / dur);
    ui_fill_round_rect(fb, bar, 4, 0xcc);
    if (filled) {
        EpdRect done = bar;
        done.width = filled;
        ui_fill_round_rect(fb, done, 4, 0x30);
    }
    s_bar_drawn = filled;
    // 只画总时长：已播时间每秒变化，墨水屏上只会带来无谓的局部刷新。
    // Total time only: the elapsed counter changes every second and would only cause needless
    // partial refreshes on e-paper.
    char t2[16];
    if (dur) mmss(t2, sizeof(t2), dur);
    else strlcpy(t2, "--:--", sizeof(t2));
    ui_text_fixed_ink_vc(fb, 624, 462, sy(18), t2, EPD_DRAW_ALIGN_RIGHT, 0x50);

    // 传输键：上一首 / 播放暂停 / 下一首 / transport: prev / play-pause / next
    const bool p_prev = s_press_pending && s_pressed.x == 186;
    const bool p_play = s_press_pending && s_pressed.x == 294;
    const bool p_next = s_press_pending && s_pressed.x == 426;
    epd_fill_circle(222, 540, 36, p_prev ? 0xe0 : 0xff, fb);
    circle_frame(fb, 222, 540, 36);
    ui_draw_icon(fb, 222, 540, 34, UI_ICON_PAGER_CHEVRON_FIRST, 0x30);
    epd_fill_circle(462, 540, 36, p_next ? 0xe0 : 0xff, fb);
    circle_frame(fb, 462, 540, 36);
    ui_draw_icon(fb, 462, 540, 34, UI_ICON_PAGER_CHEVRON_LAST, 0x30);
    bool playing = state == MUSIC_PLAYING;
    // 播放键与上一首/下一首同一套观感：白底、细边框、黑字（用户要求去掉黑色底块）。
    // The play key matches prev/next: white fill, thin frame, dark glyph (no solid dark block).
    epd_fill_circle(342, 540, 46, p_play ? 0xe0 : 0xff, fb);
    circle_frame(fb, 342, 540, 46);
    play_glyph(fb, 342, 540, 44, 32, 0x20, playing);

    // 音量行：减/加对齐上方传输键两列（222 / 462），中间合并显示「音量 NN%」，整行以 342 对称。
    // Volume row: minus/plus align under the transport columns (222 / 462) and the centre reads
    // "音量 NN%", so the whole row is symmetric about 342.
    bool p_minus = s_press_pending && s_pressed.x == 190;
    bool p_plus = s_press_pending && s_pressed.x == 430;
    ui_fill_round_rect(fb, (EpdRect){190,612,64,56}, 28, UI_GRAY_WHITE);
    draw_frame(fb, (EpdRect){190,612,64,56}, 28);
    ui_text_fixed_vc(fb, 222, 640, sy(34), "-", EPD_DRAW_ALIGN_CENTER, false);
    ui_fill_round_rect(fb, (EpdRect){430,612,64,56}, 28, UI_GRAY_WHITE);
    draw_frame(fb, (EpdRect){430,612,64,56}, 28);
    ui_text_fixed_vc(fb, 462, 640, sy(34), "+", EPD_DRAW_ALIGN_CENTER, false);
    unsigned pct = (unsigned)((music_volume() * 100u + 63u) / 127u);
    snprintf(text, sizeof(text), "音量 %u%%", pct);
    ui_text_fixed_vc(fb, 342, 640, sy(26), text, EPD_DRAW_ALIGN_CENTER, false);

    // 播放列表卡 / The playlist card
    EpdRect card = {36,700,612,300};
    ui_fill_round_rect(fb, card, 28, 0xff);
    draw_frame(fb, card, 28);
    int count = music_track_count();
    snprintf(text, sizeof(text), "播放列表 · %d 首", count);
    ui_text_fixed_ink_vc(fb, 60, 722, sy(18), text, EPD_DRAW_ALIGN_LEFT, 0x60);
    // 全部歌曲：进全屏列表（收藏优先、按键翻页）。/ All tracks: open the full-screen list (favorites first, key-paged).
    const bool p_all = s_press_pending && s_pressed.x == 468 && s_pressed.y == 704;
    ui_fill_round_rect(fb, (EpdRect){468,704,160,40}, 20, UI_GRAY_WHITE);
    draw_frame(fb, (EpdRect){468,704,160,40}, 20);
    ui_text_fixed_ink_vc(fb, 548, 724, sy(20), "全部歌曲", EPD_DRAW_ALIGN_CENTER, 0x20);
    if (!count) {
        ui_text_fixed_ink_vc(fb, 342, 870, sy(22), "暂无音乐，请从网页上传到 music 文件夹",
                             EPD_DRAW_ALIGN_CENTER, 0x60);
    }
    int current = music_track_index();
    for (int i = 0; i < MUSIC_ROWS; ++i) {
        int idx = s_page * MUSIC_ROWS + i;
        if (idx >= count) break;
        const music_track_t *track = music_track(idx);
        if (!track) break;
        EpdRect row = {44,748+i*64,596,56};
        bool selected = idx == current;
        bool pressed = s_press_pending && s_pressed.x == row.x && s_pressed.y == row.y;
        ui_fill_round_rect(fb, row, 14, UI_GRAY_WHITE);
    if (selected || pressed) ui_draw_round_rect(fb, row, 14, UI_GRAY_BLACK);
        // 序号列改为收藏：书签实心=已收藏，浅色=未收藏。/ The index column becomes favorites: solid bookmark = saved, pale = not.
        const bool fav = music_favorite(idx);
        ui_draw_icon(fb, 70, row.y + 28, 20, fav ? UI_ICON_BOOKMARK_CHECK : UI_ICON_BOOKMARK,
                     fav ? 0x20 : 0xa8);
        strlcpy(text, track->title, sizeof(text));
        fit(text, sy(22), 420);
        ui_text_fixed_vc(fb, 104, row.y + 28, sy(22), text, EPD_DRAW_ALIGN_LEFT, false);
        if (track->duration_ms) {
            char t1[16];
            mmss(t1, sizeof(t1), track->duration_ms);
            ui_text_fixed_ink_vc(fb, 620, row.y + 28, sy(18), t1, EPD_DRAW_ALIGN_RIGHT, 0x50);
        }
    }
    if (s_press_pending && s_pressed.width == 596 && s_pressed.y < 1000)
        ui_draw_selected_round_rect(fb, s_pressed, 14);

    // 翻页 / paging
    int pages = page_count();
    if (pages < 1) pages = 1;
    bool p_left = s_press_pending && s_pressed.x == 36 && s_pressed.y == 1012;
    bool p_right = s_press_pending && s_pressed.x == 492 && s_pressed.y == 1012;
    ui_fill_round_rect(fb, (EpdRect){36,1012,144,52}, 26, UI_GRAY_WHITE);
    draw_frame(fb, (EpdRect){36,1012,144,52}, 26);
    ui_text_fixed_ink_vc(fb, 108, 1038, sy(22), "上一页", EPD_DRAW_ALIGN_CENTER,
                         s_page > 0 ? 0x20 : 0xb0);
    ui_fill_round_rect(fb, (EpdRect){492,1012,144,52}, 26, UI_GRAY_WHITE);
    draw_frame(fb, (EpdRect){492,1012,144,52}, 26);
    ui_text_fixed_ink_vc(fb, 564, 1038, sy(22), "下一页", EPD_DRAW_ALIGN_CENTER,
                         s_page + 1 < pages ? 0x20 : 0xb0);
    snprintf(text, sizeof(text), "%d / %d", s_page + 1, pages);
    ui_text_fixed_ink_vc(fb, 342, 1038, sy(20), text, EPD_DRAW_ALIGN_CENTER, 0x60);
    if (s_press_pending && s_pressed.width != 596)
        ui_draw_selected_round_rect(fb, s_pressed, s_press_radius);
}

// 左侧已播时间标签区（按系统字号算，不写死，避免大字号被裁）：标签以 y=462 居中。
// The left elapsed-time label box, sized by the system font (never hardcoded, so larger
// sizes are not clipped); the label is centered on y=462.
static bool present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_NONE) return true;
    render(ctx, ctx->fb);
    if (redraw == APP_REDRAW_AREA) {
        const EpdWaveform *wave = s_press_pending ? &E0470_FOLLOW_WAVEFORM : &E0470_WAVEFORM;
        const enum EpdDrawMode mode = s_press_pending ? MODE_DU : MODE_GL16;
        if (s_tick_partial) {
            // 增量：只推新增的那一窄条进度。/ Incremental: push only the newly filled sliver.
            if (s_area.width > 0)
                guard_draw_result(ctx->hl,
                                  update_display_area_diff_with(ctx->hl, wave, mode, s_area));
            s_bar_shown = s_bar_drawn;
            s_tick_partial = false;
            return true;
        }
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, wave, mode, s_area));
    } else if (redraw == APP_REDRAW_FULL) {
        guard_draw_result(ctx->hl, update_display_full(ctx->hl));
    } else {
        guard_draw_result(ctx->hl, update_display_fast_page(ctx->hl));
    }
    // 整块刷新已经把条一起推上屏。/ A block refresh already pushed the bar up.
    s_bar_shown = s_bar_drawn;
    s_tick_partial = false;
    return true;
}
static EpdRect area_hint(app_ctx_t *ctx) { (void)ctx; return s_area; }

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_message[0] = 0;
    s_press_pending = false;
    s_tick_partial = false;
    s_show_all = false;  // 每次进页从播放页开始 / always enter at the player, not the full list
    s_dir[0] = 0;        // 全屏列表下次从曲库根开始 / the full list starts at the library root next time
    s_entry_page = 0;
    // 首次进入（曲库为空）才同步扫描；插卡事件已由 on_media_ready 扫过。
    // Scan synchronously only when the library is empty; the card event already scanned otherwise.
    if (music_track_count() == 0) music_scan();
    int current = music_track_index();
    s_seen_track = current;
    s_page = current >= 0 ? current / MUSIC_ROWS : 0;
    s_serial = music_serial();
    sync_lyric();
    s_area = MUSIC_DIRTY;
}
static void music_on_exit(app_ctx_t *ctx) { (void)ctx; s_press_pending = false; }
static void on_media_lost(app_ctx_t *ctx) {
    (void)ctx;
    music_media_lost();
    strlcpy(s_message, "存储卡已移除", sizeof(s_message));
    s_dir[0] = 0;  // 卡拔了，层级路径作废 / the folder path is void once the card is gone
    s_entry_page = 0;
    s_press_pending = false;
}
static void on_media_ready(app_ctx_t *ctx) {
    (void)ctx;
    music_scan();  // 清丢卡闩锁并重建曲库 / clear the lost latch and rebuild the library
    s_dir[0] = 0;  // 曲库可能整体换过，层级回到根 / the library may be different: back to the root
    s_entry_page = 0;
    int current = music_track_index();
    if (current / MUSIC_ROWS != s_page && current >= 0) s_page = current / MUSIC_ROWS;
    strlcpy(s_message, "存储卡已就绪", sizeof(s_message));
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    (void)ctx;
    // 重连读秒到点：把「重启中」擦回「重连」。只刷那一小块，不惊动整页。
    // Countdown done: turn "restarting" back into "relink", repainting just that capsule.
    if (s_restart_tick && !restarting()) {
        s_restart_tick = 0;
        if (!s_show_all) {
            s_area = RESTART_RECT;
            return APP_REDRAW_AREA;
        }
    }
    uint32_t serial = music_serial();
    const bool in_view = s_show_all;
    // 歌词随播放位置推进：只在播放页检测，换句才刷新。
    // Lyrics advance with playback: tracked on the player only, redrawn when the line changes.
    bool lyric_changed = false;
    if (!in_view) {
        char snap[sizeof(s_lyric_seen)];
        lyric_snapshot(snap, sizeof(snap));
        lyric_changed = strcmp(snap, s_lyric_seen) != 0;
        if (lyric_changed) memcpy(s_lyric_seen, snap, sizeof(snap));
    }
    bool state_changed = serial != s_serial;
    if (state_changed) {
        int current = music_track_index();
        if (current != s_seen_track) {  // 换曲时列表跟随到所在页 / follow the list to the playing track's page
            s_seen_track = current;
            if (current >= 0) s_page = current / MUSIC_ROWS;
        }
        s_serial = serial;
        s_tick_partial = false;
        s_area = current_dirty();
        return APP_REDRAW_AREA;
    }
    if (lyric_changed) {
        // 歌词变了就只刷歌词区（含消息、进度与时间），全屏列表则整块刷。
        // A lyric change refreshes the lyric span (with the message, progress and times);
        // the full list repaints its whole view.
        s_tick_partial = false;
        s_area = in_view ? current_dirty() : LYRIC_DIRTY;
        return APP_REDRAW_AREA;
    }
    if (in_view) return APP_REDRAW_NONE;
    // 纯进度推进：条只推新增的一窄条（像素慢慢变黑，不整块重刷），时间文字不再每秒刷。
    // Pure progress: the bar pushes only the newly filled sliver (pixels darken incrementally,
    // never a block repaint); the time label is not repainted every second.
    const uint32_t dur = music_duration_ms(), el = music_elapsed_ms();
    const int filled = dur ? (int)((uint64_t)564 * (el > dur ? dur : el) / dur) : 0;
    EpdRect strip = {0, 0, 0, 0};
    if (filled > s_bar_shown)
        strip = (EpdRect){60 + s_bar_shown, 428, filled - s_bar_shown + 2, 12};
    else if (filled < s_bar_shown)
        strip = (EpdRect){58, 428, 570, 12};  // 后退（跳转/换曲）：整条重画 / a rewind repaints the whole bar
    if (strip.width == 0) return APP_REDRAW_NONE;
    s_area = strip;
    s_tick_partial = true;
    return APP_REDRAW_AREA;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_PRESS) {
        music_hit_t target = target_at(ev->x0, ev->y0);
        s_press_pending = target.r.width > 0;
        if (s_press_pending) {
            s_pressed = target.r;
            s_press_radius = target.radius;
            s_area = current_dirty();
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_MOVE) {
        if (s_press_pending && !hit(s_pressed, ev->x, ev->y)) {
            s_press_pending = false;
            s_area = current_dirty();
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    bool restore = s_press_pending;
    s_press_pending = false;
    if (restore) s_area = current_dirty();

    if (y < 160 && x < 120) {  // 返回：先退一层文件夹、再退视图、最后退播放页 / Back: up a folder, then the view, then the player
        if (s_show_all) {
            if (s_browse && s_dir[0]) { dir_up(); return dirty(); }
            s_show_all = false;
            return APP_REDRAW_PAGE;
        }
        ctx->request_return = true;
        return APP_REDRAW_NONE;
    }
    if (s_show_all) {
        // 层级模式先列子文件夹再列曲目，平铺模式只有曲目。
        // Hierarchy mode lists sub-folders then tracks; the flat list only has tracks.
        const int dirs = s_browse ? s_level_dirs : 0;
        const int entries = s_browse ? level_total() : s_order_count;
        const int page = s_browse ? s_entry_page : s_full_page;
        for (int i = 0; i < FULL_ROWS; ++i) {
            if (!hit((EpdRect){44,196+i*64,596,56}, x, y)) continue;
            const int pos = page * FULL_ROWS + i;
            if (pos >= entries) break;
            if (pos < dirs) {
                char name[DIR_NAME_MAX];
                child_name(s_dir_first[pos], name, sizeof(name));
                if (name[0]) dir_enter(name);
                return dirty();
            }
            const int idx = s_order[pos - dirs];
            if (x < 104) music_favorite_toggle(idx);  // 书签列切换收藏 / the bookmark column toggles the favorite
            else {
                music_play_index(idx);
                s_seen_track = idx;
            }
            return dirty();
        }
        if (hit((EpdRect){36,1012,144,52}, x, y)) {
            if (s_browse) {
                if (s_entry_page > 0) --s_entry_page;
            } else if (s_full_page > 0) {
                --s_full_page;
            }
            return dirty();
        }
        if (hit((EpdRect){492,1012,144,52}, x, y)) {
            if (s_browse) {
                if (s_entry_page + 1 < browse_page_count()) ++s_entry_page;
            } else if (s_full_page + 1 < full_page_count()) {
                ++s_full_page;
            }
            return dirty();
        }
        return restore ? dirty() : APP_REDRAW_NONE;
    }
    // 模式胶囊：循环切换列表/单曲/随机/收藏循环，只刷这一小块。
    // Mode capsule: cycle list/single/shuffle/favorites, repainting only this capsule.
    if (hit(MODE_RECT, x, y)) {
        music_cycle_mode();
        s_serial = music_serial();  // 别让 on_tick 因为计数变化再刷一遍整页 / keep on_tick from repainting the page
        s_area = MODE_RECT;
        return APP_REDRAW_AREA;
    }
    // 输出胶囊：蓝牙耳机 ⇄ 本机扬声器，只刷这一小块；模组会在两种模式间重走一遍 AT 序列。
    // Output capsule: headset ⇄ local speaker, repainting only this capsule; the module replays
    // its AT sequence between the two modes.
    if (hit(OUTPUT_RECT, x, y)) {
        music_set_output(music_output() == MUSIC_OUT_SPK ? MUSIC_OUT_BT : MUSIC_OUT_SPK);
        s_serial = music_serial();
        s_area = OUTPUT_RECT;
        return APP_REDRAW_AREA;
    }
    // 重连：模组整条断电重启，输出通路不变。没声音时的手动兜底（等同关开蓝牙开关）。
    // Relink: power-cycle the module end to end without changing the route; the manual fallback
    // when there is no sound (the same as toggling the Bluetooth switch).
    if (hit(RESTART_RECT, x, y)) {
        // 重连正在跑（读秒中或 bt_host 里整条序列还没结束）：这一下直接丢掉——不重排读秒，
        // 也不排队第二次重连。以前连按几下会排出好几轮，用户看到「按了没反应，十几秒后才出声」。
        // A relink is already running (either still counting down here or inside bt_host): drop the
        // tap — no countdown reset, no second relink queued. Repeated taps used to queue several
        // rounds and looked like "the button does nothing, sound only comes 15 s later".
        if (restarting() || bt_audio_relinking()) {
            ESP_LOGI(TAG, "relink tap ignored (already restarting)");
            return APP_REDRAW_NONE;
        }
        music_restart_output();
        s_restart_tick = xTaskGetTickCount();
        if (!s_restart_tick) s_restart_tick = 1;  // 防止在 tick 0 被当成“未在重启”
        s_serial = music_serial();
        s_area = RESTART_RECT;
        return APP_REDRAW_AREA;
    }
    // 进度条点按跳转：按落点比例换算绝对毫秒。/ Tap the progress bar to seek: map the tap to absolute milliseconds.
    if (hit((EpdRect){48,414,588,66}, x, y)) {
        const uint32_t dur = music_duration_ms();
        if (dur) {
            int rel = x - 60;
            if (rel < 0) rel = 0;
            if (rel > 564) rel = 564;
            music_seek((uint32_t)((uint64_t)dur * (uint32_t)rel / 564u));
        }
        return dirty();
    }
    // 全部歌曲入口：有子文件夹按目录层级从根开始，否则沿用平铺的收藏优先列表；
    // 打开时定位到当前曲目所在页。
    // All-tracks entry: start at the library root when sub-folders exist, otherwise keep the flat
    // favorites-first list; open on the page holding the current track.
    if (hit((EpdRect){468,704,160,40}, x, y)) {
        s_browse = library_has_dirs();
        s_dir[0] = 0;
        s_entry_page = 0;
        s_full_page = 0;
        const int current = music_track_index();
        if (s_browse) {
            level_build();
            for (int i = 0; i < s_order_count; ++i)
                if (s_order[i] == current) { s_entry_page = i / FULL_ROWS; break; }
        } else {
            rebuild_order();
            for (int i = 0; i < s_order_count; ++i)
                if (s_order[i] == current) { s_full_page = i / FULL_ROWS; break; }
        }
        s_show_all = true;
        return APP_REDRAW_PAGE;
    }
    if (hit_circle(222, 540, 40, x, y)) { music_prev(); return dirty(); }
    if (hit_circle(342, 540, 52, x, y)) { music_toggle(); return dirty(); }
    if (hit_circle(462, 540, 40, x, y)) { music_next(); return dirty(); }
    if (hit((EpdRect){190,612,64,56}, x, y)) {
        int vol = (int)music_volume() - 8;
        if (vol < 0) vol = 0;
        music_set_volume((uint8_t)vol);
        return dirty();
    }
    if (hit((EpdRect){430,612,64,56}, x, y)) {
        int vol = (int)music_volume() + 8;
        if (vol > 127) vol = 127;
        music_set_volume((uint8_t)vol);
        return dirty();
    }
    for (int i = 0; i < MUSIC_ROWS; ++i) {
        if (!hit((EpdRect){44,748+i*64,596,56}, x, y)) continue;
        int idx = s_page * MUSIC_ROWS + i;
        if (idx >= music_track_count()) break;
        if (x < 104) music_favorite_toggle(idx);  // 书签列切换收藏 / the bookmark column toggles the favorite
        else {
            music_play_index(idx);
            s_seen_track = idx;
        }
        return dirty();
    }
    int pages = page_count();
    if (hit((EpdRect){36,1012,144,52}, x, y)) {
        if (s_page > 0) --s_page;
        return restore ? dirty() : APP_REDRAW_NONE;
    }
    if (hit((EpdRect){492,1012,144,52}, x, y)) {
        if (s_page + 1 < pages) ++s_page;
        return restore ? dirty() : APP_REDRAW_NONE;
    }
    return restore ? dirty() : APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (s_show_all) {  // 全屏列表：左右键翻页、中键退一层文件夹或退回播放页 / Full list: side keys page, the middle key goes up a level or back to the player
        if (key == UI_KEY_1) {
            if (s_browse) {
                if (s_entry_page > 0) --s_entry_page;
            } else if (s_full_page > 0) {
                --s_full_page;
            }
            return dirty();
        }
        if (key == UI_KEY_3) {
            if (s_browse) {
                if (s_entry_page + 1 < browse_page_count()) ++s_entry_page;
            } else if (s_full_page + 1 < full_page_count()) {
                ++s_full_page;
            }
            return dirty();
        }
        if (key == UI_KEY_2) {
            if (s_browse && s_dir[0]) { dir_up(); return dirty(); }
            s_show_all = false;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_2) { ctx->request_app = app_home_page(); return APP_REDRAW_NONE; }
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}
static bool menu_handle_enabled(app_ctx_t *ctx) { (void)ctx; return false; }
// 播放页是主页面：侧键跟其他主页面一样循环切栏；全屏列表算子页，侧键留给翻页。
// The player is a main page, so the side keys cycle tabs; the full list is a subpage and keeps them for paging.
static bool main_page_visible(app_ctx_t *ctx) { (void)ctx; return !s_show_all; }

const app_desc_t app_music = {
    .binary_ui = false,
    .title = "音乐 Music", .detail = "TF 卡音乐 · 蓝牙耳机播放", .enter_full = false,
    .owns_keys = true, .menu_handle_enabled = menu_handle_enabled,
    // 与书架/文件管理/设置一致：底栏常驻，返回首页时不会被整屏刷新擦掉。
    // Same as shelf/files/settings: the bottom bar stays, so returning home never wipes it.
    .main_page_visible = main_page_visible,
    .persistent_navigation = true,
    .on_enter = on_enter, .on_exit = music_on_exit,
    .on_media_lost = on_media_lost, .on_media_ready = on_media_ready,
    .render = render, .present = present, .on_tick = on_tick,
    .on_gesture = on_gesture, .on_key = on_key, .area_hint = area_hint,
};
