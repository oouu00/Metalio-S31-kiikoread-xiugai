/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 见 turn_trace.h。整个文件只在 METALIO_TURN_TRACE 构建里参与编译，交付构建为空目标。
 * / See turn_trace.h. The whole file compiles only in METALIO_TURN_TRACE builds and is an
 * empty object in delivery builds.
 */

#include "turn_trace.h"

#ifdef METALIO_TURN_TRACE

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_sd.h"
#include "ttf_font.h"

static const char *TAG = "turntrace";

/// 落盘路径；根目录最容易找，可用 -DTURN_TRACE_PATH 覆盖。
/// / Output path; the card root is easiest to find, override with -DTURN_TRACE_PATH.
#ifndef TURN_TRACE_PATH
#define TURN_TRACE_PATH "/sdcard/turn_trace.csv"
#endif

#define TURN_TRACE_RECORDS 128
/// 攒够这么多条就立刻唤醒写卡任务；等不到就按定时器周期落盘。
/// / Wake the writer once this many records pile up; otherwise the periodic tick flushes.
#define TURN_TRACE_FLUSH_AT 4
#define TURN_TRACE_FLUSH_MS 3000
/// 超过这么久还没结束的记录视为中断（present 走了别的出口），不再挡住落盘。
/// / A record still open after this long was abandoned (present took another exit) and must
/// not keep blocking the flush.
#define TURN_TRACE_STALE_US (5 * 1000 * 1000)
/// 单次最多写这么多行，写完让出 CPU，别在推屏附近占住总线。
/// / At most this many lines per round, then yield so the bus is not held near an update.
#define TURN_TRACE_FLUSH_MAX 32
/// 文件超过这个大小就停止写入，避免把卡写满。/ Stop past this size so the card is not filled.
#define TURN_TRACE_MAX_BYTES (4u * 1024u * 1024u)

enum {
    TT_ROUTE_OTHER = 0,
    TT_ROUTE_TEXT,   ///< 普通文字翻页 / ordinary text turn
    TT_ROUTE_WATER,  ///< 水波纹翻页 / ripple turn
    TT_ROUTE_IMAGE,  ///< 图文混排 / mixed image page
    TT_ROUTE_FULL,   ///< 周期或手动全刷 / periodic or manual cleanup
    TT_ROUTE_ENTRY,  ///< 入书一次性 GC16 / one-shot GC16 on reader entry
    TT_ROUTE_SLIDER  ///< 滑杆拖动 DU（含被 150ms 限频丢掉的帧）/ slider drag DU, including throttled frames
};

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    int32_t  stage_us[TT_STAGES];
    int32_t  total_us;
    uint32_t glyphs;
    uint32_t hits;
    uint32_t misses;
    int32_t  read_us;
    int32_t  raster_us;
    int32_t  glyph_us;
    uint32_t psram_free_kb;
    uint32_t psram_largest_kb;
    uint16_t chapter;
    uint16_t page;
    uint16_t pages;
    uint16_t area_y;
    uint16_t area_h;
    uint16_t upd_x;
    uint16_t upd_y;
    uint16_t upd_w;
    uint16_t upd_h;
    int16_t  jumps;
    int16_t  moved;
    char     where[16];
    uint8_t  redraw;
    uint8_t  fullscreen;
    uint8_t  toolbar;
    uint8_t  chrome_before;
    int8_t   dir;
    uint8_t  cached;
    uint8_t  cleanup;
    uint8_t  night;
    uint8_t  status_fused;
    uint8_t  throttled;
    uint8_t  mode;
    uint8_t  route;
} tt_record_t;

static tt_record_t *s_records;
static unsigned s_head;      ///< 下一条写入位置 / next write slot
static unsigned s_pending;   ///< 尚未落盘的条数 / records not yet on disk
static uint32_t s_seq;
static uint32_t s_dropped;
static bool s_active;
static bool s_truncated;
static int64_t s_begin_us;
static int64_t s_last_us;
static tt_record_t s_current;
static ttf_bench_stats_t s_glyph;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;

/// 首次计时时惰性建缓冲和任务，避免改动各板型的 app_main.c。
/// / Build the buffer and task lazily on the first turn so no board's app_main.c needs a change.
static void turn_trace_ensure(void);

static const char *route_name(unsigned route) {
    switch (route) {
        case TT_ROUTE_TEXT:  return "text";
        case TT_ROUTE_WATER: return "water";
        case TT_ROUTE_IMAGE: return "image";
        case TT_ROUTE_FULL:  return "full";
        case TT_ROUTE_ENTRY: return "entry";
        case TT_ROUTE_SLIDER:return "slider";
        default:             return "other";
    }
}

static const char *mode_name(int mode) {
    switch (mode & 0x0f) {
        case 1:  return "DU";
        case 2:  return "GC16";
        case 5:  return "GL16";
        default: return "?";
    }
}

/* ---- 采集 / Collection ---- */

void turn_trace_begin(int dir, unsigned chapter, size_t page, size_t pages) {
    turn_trace_ensure();
    if (s_records == NULL) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    memset(&s_current, 0, sizeof(s_current));
    s_current.seq = ++s_seq;
    s_current.uptime_ms = (uint32_t)(now / 1000);
    s_current.dir = (int8_t)dir;
    s_current.chapter = chapter > 0xFFFFu ? 0xFFFFu : (uint16_t)chapter;
    s_current.page = page > 0xFFFFu ? 0xFFFFu : (uint16_t)page;
    s_current.pages = pages > 0xFFFFu ? 0xFFFFu : (uint16_t)pages;
    s_current.psram_free_kb =
        (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    s_current.psram_largest_kb =
        (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);
    s_begin_us = now;
    s_last_us = now;
    s_active = true;
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_mark(tt_stage_t stage) {
    if (!s_active || stage >= TT_STAGES) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_current.stage_us[stage] += (int32_t)(now - s_last_us);
    s_last_us = now;
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_set_cached(bool cached) {
    if (!s_active) return;
    portENTER_CRITICAL(&s_lock);
    s_current.cached = cached ? 1 : 0;
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_glyph_begin(void) {
    if (!s_active) return;
    ttf_bench_begin();
}

void turn_trace_glyph_end(void) {
    if (!s_active) return;
    ttf_bench_end(&s_glyph);
    portENTER_CRITICAL(&s_lock);
    s_current.glyphs = s_glyph.glyphs;
    s_current.hits = s_glyph.hits;
    s_current.misses = s_glyph.misses;
    s_current.read_us = (int32_t)s_glyph.read_us;
    s_current.raster_us = (int32_t)s_glyph.raster_us;
    s_current.glyph_us = (int32_t)s_glyph.total_us;
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_set_route(bool text_turn, bool water_turn, bool cleanup, int mode,
                          int area_y, int area_h) {
    if (!s_active) return;
    unsigned route = TT_ROUTE_OTHER;
    if (cleanup) route = TT_ROUTE_FULL;
    else if (water_turn) route = TT_ROUTE_WATER;
    else if (text_turn) route = TT_ROUTE_TEXT;
    portENTER_CRITICAL(&s_lock);
    s_current.route = (uint8_t)route;
    s_current.cleanup = cleanup ? 1 : 0;
    s_current.mode = (uint8_t)mode;
    s_current.area_y = area_y < 0 ? 0 : (uint16_t)area_y;
    s_current.area_h = area_h < 0 ? 0 : (uint16_t)area_h;
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_set_where(const char* where) {
    // 先到为准：一次翻页可能被多次 present 触碰，第一个出口就是真正推动屏幕的那个。
    // / First writes win: a turn's record can be touched by several present calls, and the first outlet is the one that actually drove the panel.
    if (!s_active || where == NULL || s_current.where[0] != '\0') return;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_current.where, sizeof(s_current.where), "%s", where);
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_set_outcome(int redraw, bool fullscreen, bool toolbar) {
    if (!s_active) return;
    portENTER_CRITICAL(&s_lock);
    s_current.redraw = redraw < 0 ? 0xFFu : (uint8_t)redraw;
    s_current.fullscreen = fullscreen ? 1 : 0;
    s_current.toolbar = toolbar ? 1 : 0;
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_set_chrome_before(bool not_fullscreen, bool toolbar_open) {
    if (!s_active) return;
    portENTER_CRITICAL(&s_lock);
    s_current.chrome_before = (not_fullscreen ? 1u : 0u) | (toolbar_open ? 2u : 0u);
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_set_batch(int jumps, int moved) {
    if (!s_active) return;
    portENTER_CRITICAL(&s_lock);
    s_current.jumps = jumps > INT16_MAX ? INT16_MAX : (int16_t)jumps;
    s_current.moved = moved > INT16_MAX ? INT16_MAX : (int16_t)moved;
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_set_night(bool night) {
    if (!s_active) return;
    portENTER_CRITICAL(&s_lock);
    s_current.night = night ? 1 : 0;
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_set_status(bool fused) {
    if (!s_active) return;
    portENTER_CRITICAL(&s_lock);
    s_current.status_fused = fused ? 1 : 0;
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_set_update_area(int x, int y, int width, int height) {
    if (!s_active) return;
    portENTER_CRITICAL(&s_lock);
    s_current.upd_x = x < 0 ? 0 : (uint16_t)x;
    s_current.upd_y = y < 0 ? 0 : (uint16_t)y;
    s_current.upd_w = width < 0 ? 0 : (uint16_t)width;
    s_current.upd_h = height < 0 ? 0 : (uint16_t)height;
    portEXIT_CRITICAL(&s_lock);
}

void turn_trace_set_slider(bool throttled, int mode, int x, int y, int width, int height) {
    if (!s_active) return;
    portENTER_CRITICAL(&s_lock);
    s_current.route = (uint8_t)TT_ROUTE_SLIDER;
    s_current.throttled = throttled ? 1 : 0;
    s_current.mode = (uint8_t)mode;
    s_current.upd_x = x < 0 ? 0 : (uint16_t)x;
    s_current.upd_y = y < 0 ? 0 : (uint16_t)y;
    s_current.upd_w = width < 0 ? 0 : (uint16_t)width;
    s_current.upd_h = height < 0 ? 0 : (uint16_t)height;
    portEXIT_CRITICAL(&s_lock);
}

bool turn_trace_end(void) {
    if (!s_active) return false;
    int64_t now = esp_timer_get_time();
    bool wake = false;
    portENTER_CRITICAL(&s_lock);
    s_current.total_us = (int32_t)(now - s_begin_us);
    s_records[s_head] = s_current;
    s_head = (s_head + 1) % TURN_TRACE_RECORDS;
    if (s_pending < TURN_TRACE_RECORDS) ++s_pending;
    else ++s_dropped;
    wake = s_pending >= TURN_TRACE_FLUSH_AT;
    s_active = false;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "#%lu %s %s where=%s redraw=%u p%u/%u turn=%ld body=%ld outer=%ld render=%ld display=%ld "
                  "join=%ld total=%ld glyph=%lu miss=%lu cached=%d",
             (unsigned long)s_current.seq,
             route_name(s_current.route), mode_name(s_current.mode),
             s_current.where[0] ? s_current.where : "-", (unsigned)s_current.redraw,
             (unsigned)s_current.page + 1, (unsigned)s_current.pages,
             (long)s_current.stage_us[TT_TURN], (long)s_current.stage_us[TT_BODY],
             (long)s_current.stage_us[TT_OUTER], (long)s_current.stage_us[TT_RENDER],
             (long)s_current.stage_us[TT_DISPLAY], (long)s_current.stage_us[TT_JOIN],
             (long)s_current.total_us, (unsigned long)s_current.glyphs,
             (unsigned long)s_current.misses, (int)s_current.cached);
    if (wake && s_task != NULL) xTaskNotifyGive(s_task);
    return true;
}

void turn_trace_cancel(void) {
    portENTER_CRITICAL(&s_lock);
    s_active = false;
    portEXIT_CRITICAL(&s_lock);
}

/* ---- 落盘 / Flushing ---- */

static void write_records(FILE *out, unsigned base, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        const tt_record_t *r = &s_records[(base + i) % TURN_TRACE_RECORDS];
        fprintf(out,
                "%lu,%lu,%d,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,%s,%s,%s,%u,%u,%u,%u,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%lu,%lu,%lu,"
                "%ld,%ld,%ld,%u,%u,%u,%u,%u,%u,%u,%u\n",
                (unsigned long)r->seq, (unsigned long)r->uptime_ms, (int)r->dir,
                (unsigned)r->chapter, (unsigned)r->page + 1, (unsigned)r->pages,
                (int)r->jumps, (int)r->moved,
                (unsigned)r->cached, (unsigned)r->cleanup,
                (unsigned)r->night, (unsigned)r->status_fused, (unsigned)r->throttled,
                route_name(r->route), mode_name(r->mode),
                r->where[0] ? r->where : "-",
                (unsigned)r->redraw, (unsigned)r->fullscreen, (unsigned)r->toolbar, (unsigned)r->chrome_before,
                (long)r->stage_us[TT_TURN], (long)r->stage_us[TT_BODY],
                (long)r->stage_us[TT_OUTER], (long)r->stage_us[TT_RENDER],
                (long)r->stage_us[TT_DISPLAY], (long)r->stage_us[TT_JOIN],
                (long)r->total_us,
                (unsigned long)r->glyphs, (unsigned long)r->hits, (unsigned long)r->misses,
                (long)r->read_us, (long)r->raster_us, (long)r->glyph_us,
                (unsigned)r->psram_free_kb, (unsigned)r->psram_largest_kb,
                (unsigned)r->area_y, (unsigned)r->area_h,
                (unsigned)r->upd_x, (unsigned)r->upd_y, (unsigned)r->upd_w, (unsigned)r->upd_h);
    }
}

static const char *const TURN_TRACE_HEADER =
    "seq,uptime_ms,dir,chapter,page,pages,jumps,moved,cached,cleanup,night,status_fused,throttled,"
    "route,mode,where,redraw,fullscreen,toolbar,chrome_before,turn_us,body_us,outer_us,render_us,display_us,join_us,total_us,"
    "glyphs,hits,misses,read_us,raster_us,glyph_us,"
    "psram_free_kb,psram_largest_kb,area_y,area_h,upd_x,upd_y,upd_w,upd_h\n";

void turn_trace_flush(bool force) {
    if (s_records == NULL) return;
    // 翻页进行中不碰卡：一次写卡可能让行队列欠载，触发清屏恢复。
    // / Never touch the card mid-turn: a write can starve the line queue into a clear-and-recover.
    if (s_active && !force) {
        if (esp_timer_get_time() - s_begin_us < TURN_TRACE_STALE_US) return;
        s_active = false;  // present 走了别的出口，这条记录作废 / present took another exit
    }
    if (s_truncated) return;

    portENTER_CRITICAL(&s_lock);
    unsigned pending = s_pending;
    // 队首 = 下一个写入位往前推 pending 条。一次取走，之后写入端不会覆盖这些槽。
    // / Head = next write slot walked back by pending. Taken in one shot so the producer
    // cannot overwrite these slots while we print them.
    unsigned base = (s_head + TURN_TRACE_RECORDS - pending) % TURN_TRACE_RECORDS;
    portEXIT_CRITICAL(&s_lock);
    if (pending == 0 || (!force && pending < TURN_TRACE_FLUSH_AT)) return;
    if (!read_pico_sd_io_ready()) return;

    // 单条记录也要写（force）；否则一次最多写一批，剩下的下一轮继续。
    // / A force flush writes even one record; otherwise a bounded batch and the rest next round.
    unsigned batch = pending > TURN_TRACE_FLUSH_MAX ? TURN_TRACE_FLUSH_MAX : pending;

    struct stat st;
    bool fresh = stat(TURN_TRACE_PATH, &st) != 0;
    if (!fresh && (uint64_t)st.st_size > TURN_TRACE_MAX_BYTES) {
        s_truncated = true;
        ESP_LOGW(TAG, "trace file above %u KB, stopping", (unsigned)(TURN_TRACE_MAX_BYTES / 1024));
        return;
    }

    FILE *out = fopen(TURN_TRACE_PATH, "a");
    if (out == NULL) return;
    if (fresh) fputs(TURN_TRACE_HEADER, out);
    write_records(out, base, batch);
    fclose(out);

    portENTER_CRITICAL(&s_lock);
    if (s_pending > batch) s_pending -= batch;
    else s_pending = 0;
    portEXIT_CRITICAL(&s_lock);
    if (s_dropped) ESP_LOGW(TAG, "trace buffer overflowed, %lu records lost", (unsigned long)s_dropped);
}

static void turn_trace_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(TURN_TRACE_FLUSH_MS));
        turn_trace_flush(false);
    }
}

static void turn_trace_ensure(void) {
    if (s_records != NULL) return;
    portENTER_CRITICAL(&s_lock);
    if (s_records == NULL) {
        s_records = heap_caps_calloc(TURN_TRACE_RECORDS, sizeof(tt_record_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    portEXIT_CRITICAL(&s_lock);
    if (s_records == NULL) return;
    if (s_task == NULL) {
        // 低于绘制与推屏任务：宁可晚一点写卡，也不能抢总线。
        // / Below every drawing and update task: better a late write than a stolen bus.
        xTaskCreatePinnedToCore(turn_trace_task, "turn_trace", 4096, NULL, 1, &s_task, 0);
    }
}

#else

#include "turn_trace.h"

#endif
