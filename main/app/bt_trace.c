/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 见 bt_trace.h。仅在 METALIO_BT_TRACE 构建里参与编译，其余构建只留空实现。
 * / See bt_trace.h. This file only does work in METALIO_BT_TRACE builds; other builds keep
 * empty stubs so callers never need an #ifdef.
 */

#include "bt_trace.h"

#if defined(METALIO_BT_TRACE) && METALIO_BT_TRACE

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bt_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "read_pico_sd.h"

static const char *TAG = "bttrace";

/// 与 power_trace.log 同目录：卡根目录下唯一一个隐藏目录，用户一眼能找到。
/// Same folder as power_trace.log: one hidden folder at the card root, easy to find.
#define BT_TRACE_DIR   "/sdcard/.metalio_s31"
#define BT_TRACE_PATH  BT_TRACE_DIR "/bt_trace.log"
#define BT_TRACE_PREV  BT_TRACE_DIR "/bt_trace.1.log"

/// 环形槽数与单行上限：192×108B ≈ 20KB，放 PSRAM，不占内部堆。
/// Ring slots and line width: 192×108B ≈ 20KB in PSRAM, never internal heap.
#define BT_TRACE_SLOTS 192
#define BT_TRACE_TEXT  108
/// 单文件上限；超了就轮转一次，只留上一份，卡不会被写满。
/// Per-file cap with a single rotation, so the card can never fill up.
#define BT_TRACE_MAX_BYTES (192u * 1024u)
/// 写卡周期与单轮条数：低于绘制任务，宁可晚一点也不能抢总线。
/// Flush period and lines per round; below the drawing tasks, better late than stealing the bus.
#define BT_TRACE_FLUSH_MS 700
#define BT_TRACE_BATCH    48

typedef struct {
    uint32_t ms;
    char text[BT_TRACE_TEXT];
} bt_trace_rec_t;

static bt_trace_rec_t *s_rec;
static volatile uint16_t s_tail;
static volatile uint16_t s_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dropped;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_flush_lock;
static bool s_dir_ok;

static void trace_writer_task(void *arg);

/// 把一段文本塞进环形缓冲；满则丢新记录（保住正在写的那批不被覆盖），绝不阻塞。
/// Queue one payload; when full the newest is dropped (a batch being written must not be
/// overwritten) and nothing blocks.
static void push_raw(const char *text) {
    if (s_rec == NULL || text == NULL) return;
    const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool queued = false;
    portENTER_CRITICAL(&s_lock);
    if (s_count < BT_TRACE_SLOTS) {
        const uint16_t slot = (uint16_t)((s_tail + s_count) % BT_TRACE_SLOTS);
        s_rec[slot].ms = ms;
        strlcpy(s_rec[slot].text, text, BT_TRACE_TEXT);
        ++s_count;
        queued = true;
    } else {
        ++s_dropped;
    }
    portEXIT_CRITICAL(&s_lock);
    if (queued && s_task) xTaskNotifyGive(s_task);
}

static void ensure_dir(void) {
    if (s_dir_ok) return;
    struct stat st;
    if (stat(BT_TRACE_DIR, &st) == 0) {
        s_dir_ok = true;
        return;
    }
    if (mkdir(BT_TRACE_DIR, 0777) == 0) s_dir_ok = true;
}

void bt_trace_flush(void) {
    if (s_rec == NULL) return;
    // 短超时：锁屏/入睡路径也会调它，等不到就直接跳过（记录留在缓冲里，下次再写）。
    // Short timeout: the lock/sleep path calls it too; if the writer is busy just skip — the
    // records stay buffered for the next round.
    if (s_flush_lock && xSemaphoreTake(s_flush_lock, pdMS_TO_TICKS(300)) != pdTRUE) return;

    uint16_t batch = 0;
    uint16_t base = 0;
    portENTER_CRITICAL(&s_lock);
    if (s_count) {
        batch = s_count > BT_TRACE_BATCH ? BT_TRACE_BATCH : s_count;
        base = s_tail;
    }
    portEXIT_CRITICAL(&s_lock);
    if (batch == 0) goto done;
    // 卡在睡觉/重挂/传输期间一律不碰：记录留在缓冲里，醒了再写。
    // Never touch the card while it sleeps, remounts or serves a transfer: keep the records.
    if (!read_pico_sd_io_ready()) goto done;

    ensure_dir();
    struct stat st;
    bool fresh = stat(BT_TRACE_PATH, &st) != 0;
    if (!fresh && (uint64_t)st.st_size > BT_TRACE_MAX_BYTES) {
        (void)remove(BT_TRACE_PREV);
        (void)rename(BT_TRACE_PATH, BT_TRACE_PREV);
        fresh = true;
    }
    FILE *out = fopen(BT_TRACE_PATH, "a");
    if (out == NULL) goto done;
    if (fresh) {
        fprintf(out, "=== metalio bt trace, app uptime %lu ms, reset %d ===\n",
                (unsigned long)(esp_timer_get_time() / 1000), (int)esp_reset_reason());
    }
    for (uint16_t i = 0; i < batch; ++i) {
        const bt_trace_rec_t *r = &s_rec[(base + i) % BT_TRACE_SLOTS];
        fprintf(out, "[%lu] %s\n", (unsigned long)r->ms, r->text);
    }
    (void)fclose(out);

    portENTER_CRITICAL(&s_lock);
    s_tail = (uint16_t)((s_tail + batch) % BT_TRACE_SLOTS);
    s_count = (uint16_t)(s_count - batch);
    portEXIT_CRITICAL(&s_lock);

done:
    if (s_flush_lock) xSemaphoreGive(s_flush_lock);
}

void bt_trace(const char *fmt, ...) {
    if (s_rec == NULL || fmt == NULL) return;
    char buf[BT_TRACE_TEXT];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    push_raw(buf);
}

void bt_trace_line(const char *line) { push_raw(line); }

static void trace_writer_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BT_TRACE_FLUSH_MS));
        bt_trace_flush();
        if (s_dropped) {
            // 溢出只在下一轮报一次，别让报警本身把缓冲塞满。
            // Report overflow once per round so the warning cannot fill the buffer itself.
            const uint32_t lost = s_dropped;
            s_dropped = 0;
            ESP_LOGW(TAG, "trace overflow, %lu lines lost", (unsigned long)lost);
        }
    }
}

void bt_trace_start(void) {
    if (s_task) return;
    if (s_rec == NULL) {
        s_rec = heap_caps_calloc(BT_TRACE_SLOTS, sizeof(bt_trace_rec_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_rec == NULL) {
            ESP_LOGW(TAG, "no PSRAM for the trace ring; Bluetooth diagnostics off");
            return;
        }
    }
    if (s_flush_lock == NULL) s_flush_lock = xSemaphoreCreateMutex();
    if (xTaskCreatePinnedToCore(trace_writer_task, "bt_trace", 4096, NULL, 1, &s_task, 0) != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "trace writer task failed");
        return;
    }
    bt_host_set_trace(bt_trace_line);
    bt_trace("==== bt trace armed (reset %d) ====", (int)esp_reset_reason());
}

#else

// 三个写入口在关闭档里已经由头文件编成空宏，这里只需给出 start 的空实现。
// The three writers are no-op macros in the disabled build (see bt_trace.h); only start needs
// a body so callers never need an #ifdef of their own.
void bt_trace_start(void) {}

#endif
