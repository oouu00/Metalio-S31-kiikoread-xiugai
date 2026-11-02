/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 见 mem_trace.h。整个文件只在 METALIO_MEM_TRACE 构建里参与编译，交付构建为空目标。
 * / See mem_trace.h. The whole file compiles only in METALIO_MEM_TRACE builds and is an
 * empty object in delivery builds.
 */

#include "mem_trace.h"

#ifdef METALIO_MEM_TRACE

#include <stdio.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_sd.h"

static const char *TAG = "memtrace";

/// 落盘路径；根目录最容易找，可用 -DMEM_TRACE_PATH 覆盖。
/// / Output path; the card root is easiest to find, override with -DMEM_TRACE_PATH.
#ifndef MEM_TRACE_PATH
#define MEM_TRACE_PATH "/sdcard/mem_trace.csv"
#endif

/// 采样周期：默认 3 秒一次，足够看到内部 SRAM / PSRAM 的趋势，又不至于频繁写卡。
/// / Sample period: 3 s by default — coarse enough to show SRAM/PSRAM trends without
/// hammering the card.
#ifndef MEM_TRACE_PERIOD_MS
#define MEM_TRACE_PERIOD_MS 3000
#endif

/// SD 挂载前最多攒这么多条（PSRAM），挂载后一次性补写。3 秒周期下 256 条 ≈ 12 分钟窗口。
/// / At most this many samples buffered in PSRAM before the card mounts, then written in one go.
#define MEM_TRACE_RECORDS 256
/// 文件超过这个大小就停止写入，避免把卡写满。
/// / Stop past this size so the card is not filled.
#define MEM_TRACE_MAX_BYTES (2u * 1024u * 1024u)

typedef struct {
    uint32_t uptime_ms;
    uint32_t sram_free_kb;
    uint32_t sram_largest_kb;
    uint32_t sram_min_kb;
    uint32_t psram_free_kb;
    uint32_t psram_largest_kb;
    uint32_t psram_min_kb;
} mem_sample_t;

/// 环形缓冲放 PSRAM：这次采样本来就是盯内部 SRAM，缓冲自己就不该再占内部 SRAM。
/// / The ring lives in PSRAM: the whole point is watching internal SRAM, so the buffer
/// itself must not consume it.
static mem_sample_t *s_ring;
static unsigned s_head;
static unsigned s_pending;
static bool s_truncated;
static bool s_started;

static const char *const MEM_TRACE_HEADER =
    "uptime_ms,sram_free_kb,sram_largest_kb,sram_min_kb,"
    "psram_free_kb,psram_largest_kb,psram_min_kb\n";

static void mem_sample(mem_sample_t *s) {
    const uint32_t sram = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s->sram_free_kb = (uint32_t)(heap_caps_get_free_size(sram) / 1024);
    s->sram_largest_kb = (uint32_t)(heap_caps_get_largest_free_block(sram) / 1024);
    s->sram_min_kb = (uint32_t)(heap_caps_get_minimum_free_size(sram) / 1024);
    s->psram_free_kb = (uint32_t)(heap_caps_get_free_size(psram) / 1024);
    s->psram_largest_kb = (uint32_t)(heap_caps_get_largest_free_block(psram) / 1024);
    s->psram_min_kb = (uint32_t)(heap_caps_get_minimum_free_size(psram) / 1024);
}

static void mem_write_pending(void) {
    if (s_pending == 0 || s_truncated) return;
    if (!read_pico_sd_io_ready()) return;

    struct stat st;
    const bool fresh = stat(MEM_TRACE_PATH, &st) != 0;
    if (!fresh && (uint64_t)st.st_size > MEM_TRACE_MAX_BYTES) {
        s_truncated = true;
        ESP_LOGW(TAG, "mem trace above %u KB, stopping", (unsigned)(MEM_TRACE_MAX_BYTES / 1024));
        return;
    }

    FILE *out = fopen(MEM_TRACE_PATH, "a");
    if (out == NULL) return;
    if (fresh) fputs(MEM_TRACE_HEADER, out);
    // 队首 = 下一个写入位往前推 pending 条；满环时正好从最旧样本开始，仍是时间顺序。
    // / Head = next write slot walked back by pending; when the ring is full this starts at the
    // oldest sample and still reads in time order.
    const unsigned base = (s_head + MEM_TRACE_RECORDS - s_pending) % MEM_TRACE_RECORDS;
    for (unsigned i = 0; i < s_pending; i++) {
        const mem_sample_t *r = &s_ring[(base + i) % MEM_TRACE_RECORDS];
        fprintf(out, "%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
                (unsigned long)r->uptime_ms,
                (unsigned long)r->sram_free_kb, (unsigned long)r->sram_largest_kb,
                (unsigned long)r->sram_min_kb,
                (unsigned long)r->psram_free_kb, (unsigned long)r->psram_largest_kb,
                (unsigned long)r->psram_min_kb);
    }
    fclose(out);
    s_pending = 0;
}

static void mem_trace_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MEM_TRACE_PERIOD_MS));
        mem_sample(&s_ring[s_head]);
        s_head = (s_head + 1) % MEM_TRACE_RECORDS;
        if (s_pending < MEM_TRACE_RECORDS) ++s_pending;
        mem_write_pending();
    }
}

void mem_trace_start(void) {
    if (s_started) return;
    mem_sample_t *ring = heap_caps_calloc(MEM_TRACE_RECORDS, sizeof(mem_sample_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring == NULL) {
        // 分配失败不置 started，后续入口仍可重试（与 lock/字体读取的做法一致）。
        // / Leave started unset on failure so a later entry can retry.
        ESP_LOGW(TAG, "no PSRAM for mem trace ring, sampling disabled");
        return;
    }
    s_ring = ring;
    s_started = true;
    // 低于绘制与推屏任务：宁可晚一点写卡，也不能抢总线（与 turn_trace 一致）。
    // / Below every drawing and update task: better a late write than a stolen bus.
    // 栈与 turn_trace 一致取 4 KB：fprintf 走 newlib 格式化，栈余量要留够。
    // / 4 KB like turn_trace: fprintf goes through newlib formatting and needs headroom.
    xTaskCreatePinnedToCore(mem_trace_task, "mem_trace", 4096, NULL, 1, NULL, 0);
}

#else

#include "mem_trace.h"

#endif
