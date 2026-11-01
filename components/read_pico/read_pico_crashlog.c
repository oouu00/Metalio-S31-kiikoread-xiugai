/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：崩溃日志的实现。RTC 内存跨软复位保留，所以这些行在崩溃重启后还在；
 * 冷启动（断电）时 magic 不匹配，视为空。
 *
 * English: Crash log implementation. RTC memory survives a soft reset, so the lines are still
 * there after a crash restart; on a cold boot the magic does not match and the log reads empty.
 */

#include "read_pico_crashlog.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 一行含时间戳（"12.345 "）与正文。40 字节装得下"connect/link up/subscribe"这类短标记。
// A line holds a timestamp ("12.345 ") and the text. Forty bytes fits short markers such as
// connect, link up and subscribe.
#define RP_CRASH_LOG_LEN 40
#define RP_CRASH_LOG_MAGIC 0x524C4F47u  // "RLOG"

RTC_NOINIT_ATTR static char s_lines[RP_CRASH_LOG_LINES][RP_CRASH_LOG_LEN];
RTC_NOINIT_ATTR static uint32_t s_written;
RTC_NOINIT_ATTR static uint32_t s_magic;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
// 首屏后停止开机标记，普通回首页不挤占诊断轨迹；无需额外 RTC 存储。
// Stop boot marks after the first frame so normal Home visits do not evict diagnostics; no extra RTC storage.
static bool s_boot_marks_done;

// 冷启动阶段耗时：与 crashlog 环形缓冲并行收集，便于在首帧一次性落盘到 SD 轨迹日志。
// Cold-boot stage timings: collected in parallel with the crashlog ring so they can be flushed to the SD
// trace once at the first frame.
#define RP_BOOT_MARKS_MAX 24
typedef struct { const char *name; uint32_t ms; } rp_boot_mark_t;
static rp_boot_mark_t s_boot_marks[RP_BOOT_MARKS_MAX];
static unsigned s_boot_marks_n;

// 复用 sleep.c 中 power_trace() 的同一文件路径，把冷启动各阶段耗时追加进去；
// 卡未挂载时 fopen 失败即静默跳过，与既有轨迹日志行为一致。
// Reuse sleep.c's power_trace() path so cold-boot timing lands in the same file;
// a failed fopen (no card) is silently ignored, matching the existing trace behavior.
#define RP_BOOT_TRACE_PATH "/sdcard/.metalio_s31/power_trace.log"

// 串口在这块板子上会在 SD 初始化时重新枚举，抓包只能看到开机前 1.6s，首帧那段永远拿不到。
// 因此把本次开机的时间表存进 RTC 保留内存，下一次开机一进来就打印，串口就能读到上一次
// 完整的时间线；SD 那份仍是给没有串口时用的。
// The console re-enumerates at SD init on this board, so a capture only ever sees the first
// ~1.6s and the first-frame stages are unreachable. The table is therefore kept in RTC
// retained memory and printed at the start of the next boot, which the console can read;
// the SD copy stays for the no-console case.
#define RP_BOOT_PREV_MAGIC 0x424F4F54u  // "BOOT"
typedef struct {
    uint32_t magic;
    uint32_t image_tag;  // 用函数地址区分固件版本，避免换镜像后指针失效
    uint32_t count;
    uint32_t checksum;
    uint32_t ms[RP_BOOT_MARKS_MAX];
    const char *names[RP_BOOT_MARKS_MAX];
} rp_boot_prev_t;
RTC_NOINIT_ATTR static rp_boot_prev_t s_prev_boot;
static bool s_prev_boot_reported;

static uint32_t rp_boot_checksum(const uint32_t *ms, unsigned n) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < n; ++i) {
        hash = (hash ^ ms[i]) * 16777619u;
    }
    return hash;
}

static void rp_boot_prev_store(void) {
    if (s_boot_marks_n == 0) return;
    s_prev_boot.magic = RP_BOOT_PREV_MAGIC;
    s_prev_boot.image_tag = (uint32_t)(uintptr_t)&rp_boot_mark;
    s_prev_boot.count = s_boot_marks_n;
    for (unsigned i = 0; i < s_boot_marks_n; ++i) {
        s_prev_boot.ms[i] = s_boot_marks[i].ms;
        s_prev_boot.names[i] = s_boot_marks[i].name;
    }
    s_prev_boot.checksum = rp_boot_checksum(s_prev_boot.ms, s_boot_marks_n);
}

// 上一次开机的时间线：只在同一份镜像、且校验和对得上时才敢解引用那些名字指针。
// Previous boot's timeline: only dereference the name pointers when it is the same image and
// the checksum matches.
static void rp_boot_prev_report(void) {
    if (s_prev_boot_reported) return;
    s_prev_boot_reported = true;
    if (s_prev_boot.magic != RP_BOOT_PREV_MAGIC) return;
    if (s_prev_boot.image_tag != (uint32_t)(uintptr_t)&rp_boot_mark) return;
    if (s_prev_boot.count == 0 || s_prev_boot.count > RP_BOOT_MARKS_MAX) return;
    if (s_prev_boot.checksum != rp_boot_checksum(s_prev_boot.ms, s_prev_boot.count)) return;
    ESP_LOGI("boottime", "prev boot timeline (ms since reset, replayed from RTC)");
    for (unsigned i = 0; i < s_prev_boot.count; ++i) {
        const uint32_t ms = s_prev_boot.ms[i];
        const uint32_t delta = i ? ms - s_prev_boot.ms[i - 1] : 0;
        ESP_LOGI("boottime", "  prev %-8s %6ums (+%ums)",
                 s_prev_boot.names[i], (unsigned)ms, (unsigned)delta);
    }
    ESP_LOGI("boottime", "prev boot total-to-first-frame %ums", (unsigned)s_prev_boot.ms[s_prev_boot.count - 1]);
}


static void rp_boot_marks_dump_sd(void) {
    if (s_boot_marks_n == 0) return;
    const int64_t now = esp_timer_get_time() / 1000;
    const uint32_t total = s_boot_marks[s_boot_marks_n - 1].ms;
    const uint32_t pre_init = s_boot_marks[0].ms; // 首个标记前已含 bootloader + PSRAM 预检
    // 串口同步打一份：不开卡也能直接看各阶段耗时，落盘那份留给没有串口的时候。
    // Mirror the table to the console so the stages are readable without the card.
    ESP_LOGI("boottime", "cold boot: pre-init(bootloader+psram) ~%ums", (unsigned)pre_init);
    for (unsigned i = 0; i < s_boot_marks_n; ++i) {
        const uint32_t delta = i ? s_boot_marks[i].ms - s_boot_marks[i - 1].ms : 0;
        ESP_LOGI("boottime", "  %-8s %6ums (+%ums)",
                 s_boot_marks[i].name, (unsigned)s_boot_marks[i].ms, (unsigned)delta);
    }
    ESP_LOGI("boottime", "cold boot total-to-first-frame %ums", (unsigned)total);
    // 落盘那份留给没有串口的时候，仅 debug 档写卡；release 档不定义 METALIO_DIAG_SD_LOGS，跳过。
    // The SD copy is for the no-console case; only the debug profile writes it. release leaves
    // METALIO_DIAG_SD_LOGS undefined and skips the file.
#ifdef METALIO_DIAG_SD_LOGS
    static bool dir_ok = false;
    if (!dir_ok) { (void)mkdir("/sdcard/.metalio_s31", 0777); dir_ok = true; }
    FILE *f = fopen(RP_BOOT_TRACE_PATH, "a");
    if (!f) return;
    fprintf(f, "[%lld] === cold boot timing (ms since reset) ===\n", (long long)now);
    fprintf(f, "[%lld] boot pre-init(bootloader+psram) ~%ums\n", (long long)now, (unsigned)pre_init);
    for (unsigned i = 0; i < s_boot_marks_n; ++i) {
        const uint32_t delta = i ? s_boot_marks[i].ms - s_boot_marks[i - 1].ms : 0;
        fprintf(f, "[%lld] boot %-8s %6ums (+%ums)\n",
                (long long)now, s_boot_marks[i].name, (unsigned)s_boot_marks[i].ms, (unsigned)delta);
    }
    fprintf(f, "[%lld] boot total-to-first-frame %ums\n", (long long)now, (unsigned)total);
    (void)fclose(f);
#endif
}

void rp_crash_log(const char *fmt, ...) {
    char line[RP_CRASH_LOG_LEN];
    const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    int used = snprintf(line, sizeof(line), "%lu.%03lu ", (unsigned long)(ms / 1000),
                        (unsigned long)(ms % 1000));
    if (used < 0) used = 0;
    if ((size_t)used < sizeof(line)) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(line + used, sizeof(line) - (size_t)used, fmt, args);
        va_end(args);
    }
    // 时间戳在 panic 之外的上下文里取；这里不做分配，也不碰文件系统。
    // The timestamp comes from outside the panic context; nothing here allocates or touches the FS.
    portENTER_CRITICAL(&s_lock);
    if (s_magic != RP_CRASH_LOG_MAGIC) {
        memset(s_lines, 0, sizeof(s_lines));
        s_written = 0;
        s_magic = RP_CRASH_LOG_MAGIC;
    }
    memcpy(s_lines[s_written % RP_CRASH_LOG_LINES], line, sizeof(s_lines[0]));
    ++s_written;
    portEXIT_CRITICAL(&s_lock);
}

uint8_t rp_crash_log_count(void) {
    if (s_magic != RP_CRASH_LOG_MAGIC || s_written == 0) return 0;
    return s_written < RP_CRASH_LOG_LINES ? (uint8_t)s_written : RP_CRASH_LOG_LINES;
}

const char *rp_crash_log_line(uint8_t index) {
    if (index >= rp_crash_log_count()) return NULL;
    return s_lines[(s_written - 1 - index) % RP_CRASH_LOG_LINES];
}

uint32_t rp_crash_log_serial(void) { return s_magic == RP_CRASH_LOG_MAGIC ? s_written : 0; }

void rp_crash_log_clear(void) {
    portENTER_CRITICAL(&s_lock);
    s_magic = 0;
    s_written = 0;
    portEXIT_CRITICAL(&s_lock);
}

void rp_boot_mark(const char *name) {
    if (s_boot_marks_done) return;
    const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_boot_marks_n == 0) rp_boot_prev_report();  // 本次开机第一件事：回放上一次的完整时间线
    if (s_boot_marks_n < RP_BOOT_MARKS_MAX) {
        s_boot_marks[s_boot_marks_n].name = name ? name : "?";
        s_boot_marks[s_boot_marks_n].ms = ms;
        ++s_boot_marks_n;
    }
    rp_crash_log("boot %s %ums", name ? name : "?", (unsigned)ms);
    if (name && strcmp(name, "frame") == 0) {
        s_boot_marks_done = true;
        rp_boot_prev_store();
        rp_boot_marks_dump_sd();
    }
}
