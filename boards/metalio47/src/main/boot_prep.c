/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 冷启动擦白期间的 CPU 活并发实现，详见 boot_prep.h 的说明。
 * Concurrent CPU work during the cold-boot whitening; see boot_prep.h.
 */

#include "boot_prep.h"

#include <stdint.h>

#include "app_font_context.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ttf_font.h"

static const char *TAG = "boot_prep";

// Core1 上最高优先级是 epd_prep（configMAX_PRIORITIES-1）；这个任务只在它阻塞时运行，
// 不会拖慢供数。栈给足：字体解析在这条任务里做。
// epd_prep owns the top priority on Core1; this task only runs while it blocks, so line
// feeding never slows down. Generous stack: the face is parsed on this task.
#define BOOT_PREP_TASK_PRIO 3
#define BOOT_PREP_TASK_STACK 8192
#define BOOT_PREP_TASK_CORE 1

static SemaphoreHandle_t s_done;
static volatile bool s_started;
static volatile bool s_finished;
static bool s_want_system_font;
static volatile bool s_font_done;
static int64_t s_start_us;

static void boot_prep_task(void *arg) {
    read_pico_handle_t *hw = (read_pico_handle_t *)arg;

    // 触摸识别自带「复位后 300ms 自举窗口」的等待，正好落在擦白里面。
    // Touch identification already waits the 300ms post-reset settle window internally,
    // which lands inside the whitening.
    const esp_err_t err = read_pico_touch_init(hw);
    ESP_LOGI(TAG, "touch %s at +%lldms", esp_err_to_name(err),
             (long long)((esp_timer_get_time() - s_start_us) / 1000));

    if (s_want_system_font) {
        s_font_done = app_font_activate_system();
    } else {
        s_font_done = ttf_font_open_builtin() == ESP_OK;
    }
    ESP_LOGI(TAG, "font %s at +%lldms", s_font_done ? "ok" : "failed",
             (long long)((esp_timer_get_time() - s_start_us) / 1000));

    s_finished = true;
    if (s_done != NULL) xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

bool boot_prep_start(read_pico_handle_t *hw, bool want_system_font) {
    if (hw == NULL) return false;
    if (s_started) return true;
    s_want_system_font = want_system_font;
    s_done = xSemaphoreCreateBinary();
    if (s_done == NULL) {
        ESP_LOGW(TAG, "no semaphore, running inline");
        return false;
    }
    s_start_us = esp_timer_get_time();
    if (xTaskCreatePinnedToCore(boot_prep_task, "boot_prep", BOOT_PREP_TASK_STACK, hw,
                                BOOT_PREP_TASK_PRIO, NULL, BOOT_PREP_TASK_CORE) != pdPASS) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
        ESP_LOGW(TAG, "no task, running inline");
        return false;
    }
    s_started = true;
    return true;
}

void boot_prep_join(void) {
    if (!s_started || s_finished) return;
    if (s_done != NULL) (void)xSemaphoreTake(s_done, portMAX_DELAY);
    s_finished = true;
    ESP_LOGI(TAG, "joined after %lldms",
             (long long)((esp_timer_get_time() - s_start_us) / 1000));
}

bool boot_prep_font_ready(void) { return s_font_done; }
