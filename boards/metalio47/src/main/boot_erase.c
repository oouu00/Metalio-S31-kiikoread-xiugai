/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 冷启动有一次约 950ms 的整屏物理擦白（3 轮 10 黑 / 13 白 / 3 保持），全程只是把像素
 * 往面板上推，不碰帧缓冲，也不依赖任何后续初始化。原先它把 app_main 干等一段；这里
 * 挪到 Core1 后台跑，主任务用这段时间做触摸芯片识别、系统字体加载和首帧绘制。
 *
 * Cold boot whitens the whole panel for ~950ms (3x 10 dark / 13 white / 3 hold). It only
 * pushes pixels to the panel: it neither touches the frame buffers nor needs any later
 * init step. Instead of blocking app_main, it now runs on Core1 while the main task
 * identifies the touch chip, loads the system face and paints the first frame.
 *
 * 安全性 / Safety:
 *  - 擦白只写面板与 render_context，帧缓冲由主任务独占，两者不共享内存。
 *    The erase writes the panel and render_context only; the frame buffers stay with the
 *    main task, so the two share no memory.
 *  - 喂行线程（epd_prep）是最高优先级并按核绑定，Core1 上的擦白任务随时让出。
 *    The feeder tasks are top priority and core-pinned, so the Core1 erase task always
 *    yields to them.
 *  - 首次扫屏（差分 + 波形）与擦白共用 render_context，因此 display.c 的 hl_update
 *    入口会先汇合；汇合之后所有推屏路径都不再等待。
 *    The first scan (diff + waveform) shares render_context with the erase, so display.c
 *    joins at the hl_update entry; after that no present path waits again.
 */

#include "boot_erase.h"

#include "epdiy.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "boot_erase";

// Core1 只跑 epd_prep（configMAX_PRIORITIES-1）；擦白放在它下面，始终给它让路。
// Core1 only hosts epd_prep (top priority); the erase sits below it and always yields.
#define BOOT_ERASE_TASK_PRIO 4
#define BOOT_ERASE_TASK_STACK 6144
#define BOOT_ERASE_TASK_CORE 1

static SemaphoreHandle_t s_erase_done;
static volatile bool s_erase_started;
static volatile bool s_erase_finished;
static volatile int64_t s_erase_start_us;

static void boot_erase_task(void *arg) {
    (void)arg;
    epd_clear();
    s_erase_finished = true;
    // 先置标志再给信号，等待方看到标志即可免于获取信号量。
    // Set the flag before the give so a waiter that sees it can skip the semaphore.
    xSemaphoreGive(s_erase_done);
    vTaskDelete(NULL);
}

bool boot_erase_start(void) {
    if (s_erase_started) return true;
    s_erase_done = xSemaphoreCreateBinary();
    if (s_erase_done == NULL) {
        ESP_LOGW(TAG, "no semaphore, erasing synchronously");
        return false;
    }
    s_erase_start_us = esp_timer_get_time();
    if (xTaskCreatePinnedToCore(boot_erase_task, "boot_erase", BOOT_ERASE_TASK_STACK, NULL,
                               BOOT_ERASE_TASK_PRIO, NULL, BOOT_ERASE_TASK_CORE) != pdPASS) {
        vSemaphoreDelete(s_erase_done);
        s_erase_done = NULL;
        ESP_LOGW(TAG, "no task, erasing synchronously");
        return false;
    }
    s_erase_started = true;
    return true;
}

bool boot_erase_busy(void) {
    return s_erase_started && !s_erase_finished;
}

void boot_erase_join(void) {
    if (!s_erase_started || s_erase_finished) return;
    if (s_erase_done != NULL) {
        (void)xSemaphoreTake(s_erase_done, portMAX_DELAY);
    }
    s_erase_finished = true;
    // 汇合点在首帧扫屏入口，这条日志正好把「擦白藏进首帧准备」的实际效果量化出来。
    // The join sits at the first scan entry; this line quantifies how much erase time
    // actually hid behind first-frame preparation.
    ESP_LOGI(TAG, "erase joined %lldms after start",
             (long long)((esp_timer_get_time() - s_erase_start_us) / 1000));
}
