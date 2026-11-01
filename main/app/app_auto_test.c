/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：无人值守自测的三个相位。相位 0 打开传书页起热点并轮询网络状态；相位 1 初始化
 * 蓝牙模组并扫描一圈设备（只扫描，不配对不播放）；相位 2 把模组压进模式 1 并喂 1 kHz 测试音，
 * 验证「不连蓝牙、本机扬声器外放」这条通路。相位跑完写 NVS 后重启，下一轮换另一相位；
 * 崩溃重启时靠 run/ok 两个键发现上一相位没跑完，直接跳到另一相位，避免卡死在同一个崩溃点上。
 *
 * English: the three unattended self-test phases. Phase 0 opens the Transfer page's hotspot and
 * polls the network status; phase 1 brings up the Bluetooth module and scans once (scan only, no
 * pairing, no playback); phase 2 forces mode 1 and pushes a 1 kHz tone to prove the local-speaker
 * path works without a headset. Each finished phase is recorded in NVS before a restart, and the
 * next boot runs the other phase; a crash between the run and ok markers makes the next boot skip
 * to the other phase instead of looping on the same crash.
 *
 * 冻结：只在 KIIKO_AUTO_TEST 构建里生效；不上屏、不改用户设置，NVS 只用私有命名空间，
 * 结果同时写 /sdcard/selftest.log，回家插卡即可查看。
 * Frozen: KIIKO_AUTO_TEST builds only; no drawing, no user-setting writes, private NVS
 * namespace only. Every line also goes to /sdcard/selftest.log for later inspection.
 */
#ifdef KIIKO_AUTO_TEST

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_auto_test.h"
#include "bt_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "metalio_hw.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "read_pico_transfer.h"

static const char *TAG = "selftest";

// 相位编号保存在 NVS：next = 本轮要跑的相位，run = 本轮开跑的相位，ok = 最后跑完的相位。
// Phase numbers in NVS: next = phase of this boot, run = phase started, ok = last finished phase.
#define AT_NS "autotest"
#define PHASE_AP 0
#define PHASE_BT 1
#define PHASE_SPK 2
#define PHASE_COUNT 3

#define AT_LOG_PATH "/sdcard/selftest.log"

#define AP_SETTLE_MS 10000
#define AP_POLL_MS 1000
#define AP_LIMIT_MS 40000
#define BT_READY_LIMIT_MS 20000
#define BT_SCAN_MS 12000
#define RESTART_DELAY_MS 2000

static int s_phase = -1;

/* ---- 日志：串口 + TF 卡 / Logging: console + TF card ---- */

static void at_log(const char *fmt, ...) {
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", line);
    // 串口可能被 USJ 重连打断，卡上的副本是无人值守时的唯一凭据；仅 debug 档写卡，
    // release 档不定义 METALIO_DIAG_SD_LOGS，跳过文件输出。
    // The console may drop on a USJ reconnect; the card copy is the only record when unattended.
    // SD write only in the debug profile; release leaves METALIO_DIAG_SD_LOGS undefined.
#ifdef METALIO_DIAG_SD_LOGS
    FILE *f = fopen(AT_LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
#endif
}

static void log_heap(const char *where) {
    at_log("heap %s: internal=%u largest=%u dma=%u psram=%u", where,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ---- NVS 相位记录 / Phase bookkeeping ---- */

static void nvs_ready(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        (void)nvs_flash_init();
    }
}

static int nvs_get_int(const char *key, int fallback) {
    nvs_handle_t h;
    int32_t value = fallback;
    if (nvs_open(AT_NS, NVS_READONLY, &h) != ESP_OK) return fallback;
    if (nvs_get_i32(h, key, &value) != ESP_OK) value = fallback;
    nvs_close(h);
    return (int)value;
}

static void nvs_set_int(const char *key, int value) {
    nvs_handle_t h;
    if (nvs_open(AT_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_i32(h, key, (int32_t)value);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void finish_phase(int phase) {
    nvs_set_int("ok", phase);
    nvs_set_int("next", (phase + 1) % PHASE_COUNT);
    at_log("phase %d done, restarting", phase);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}

/* ---- 相位 0：热点 / Phase 0: hotspot ---- */

static void ap_phase(void) {
    // 传书页由 app_loop 进入后自己起热点，这里只等它并轮询组件状态。
    // The Transfer page starts the hotspot itself; wait for it and poll the component status.
    vTaskDelay(pdMS_TO_TICKS(AP_SETTLE_MS));
    log_heap("ap after enter");
    bool started = false;
    for (int waited = 0; waited < AP_LIMIT_MS; waited += AP_POLL_MS) {
        read_pico_transfer_status_t st;
        read_pico_transfer_get_status(&st);
        at_log("ap state=%d mode=%d ready=%d sta=%u ssid=%s url=%s err=%s",
               (int)st.state, (int)st.mode, (int)st.network_ready, st.sta_count,
               st.ssid, st.url, esp_err_to_name(st.last_error));
        if (st.network_ready && st.state != READ_PICO_TRANSFER_ERROR) {
            started = true;
            break;
        }
        if (st.state == READ_PICO_TRANSFER_ERROR) break;
        vTaskDelay(pdMS_TO_TICKS(AP_POLL_MS));
    }
    log_heap(started ? "ap ready" : "ap failed");
    finish_phase(PHASE_AP);
}

/* ---- 相位 1：蓝牙模组只扫描 / Phase 1: Bluetooth scan only ---- */

static void format_bda(const uint8_t bda[6], char out[13]) {
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool bt_wait_ready(void) {
    for (int waited = 0; waited < BT_READY_LIMIT_MS; waited += 500) {
        if (bt_audio_ready()) return true;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return bt_audio_ready();
}

static void bt_phase(void) {
    // 只验证模组启动与 AT 扫描；空 pull 回调让喂帧任务持续送静音，不做配对与播放。
    // Only verify module bring-up and AT scanning; a null pull keeps the feed task on silence.
    if (bt_audio_init(NULL) != ESP_OK) at_log("bt init failed");
    if (!bt_wait_ready()) {
        at_log("bt module not ready");
        log_heap("bt failed");
        finish_phase(PHASE_BT);
        return;
    }
    at_log("bt module ready");
    log_heap("bt ready");

    if (bt_audio_scan_start(BT_SCAN_MS) != ESP_OK) at_log("bt scan start failed");
    vTaskDelay(pdMS_TO_TICKS(BT_SCAN_MS + 1500));
    bt_audio_scan_stop();
    const uint8_t count = bt_audio_device_count();
    at_log("bt devices=%u", count);
    for (uint8_t i = 0; i < count; ++i) {
        const bt_audio_device_t *dev = bt_audio_device(i);
        if (!dev) continue;
        char bda[13];
        format_bda(dev->bda, bda);
        at_log("bt dev %u bda=%s rssi=%d audio=%d name=%s",
               i, bda, (int)dev->rssi, (int)dev->audio, dev->name);
    }
    log_heap("bt after scan");
    finish_phase(PHASE_BT);
}

/* ---- 相位 2：本机扬声器 / Phase 2: local speaker ---- */

// 不碰蓝牙耳机、不依赖播放器：直接把模组压进模式 1，喂 1 kHz 测试音到同一路 I2S。
// 判读方式（日志）：
//   `module local speaker ready (mode 1)` 出现 → 模组回了 `SET MODE 1`，模式 1 成立；
//   随后 `FEED got=1024/1024 write=2048/2048 err=ESP_OK` → PCM 真的被模组拉走了（I2S 有时钟）；
//   写不满（TIMEOUT）→ 模组没在供时钟，问题在模式/功放，不在播放器。
// No headset and no player involved: force the module into mode 1 and push a 1 kHz tone into the
// same I2S. Reading the log: "module local speaker ready (mode 1)" means the mode took; a full
// FEED write right after means the module is clocking I2S; short writes mean it is not.
static void spk_phase(void) {
    at_log("spk: local speaker path");
    log_heap("spk boot");
    if (bt_audio_init_local(NULL) != ESP_OK) at_log("spk init failed");
    if (!bt_wait_ready()) {
        at_log("spk module not ready (mode 1 never acked)");
        log_heap("spk failed");
        finish_phase(PHASE_SPK);
        return;
    }
    at_log("spk module ready");
    log_heap("spk ready");
    const esp_err_t terr = bt_audio_play_test_tone(5000);
    at_log("spk tone=%s", esp_err_to_name(terr));
    if (terr == ESP_OK) vTaskDelay(pdMS_TO_TICKS(5500));
    // 停音后再看一次堆：I2S/UART 都还开着，这里的数字才是「外放时的常驻占用」。
    // Heap once more after the tone: I2S and UART are still up, so this is the steady-state cost.
    log_heap("spk after tone");
    bt_audio_deinit();
    vTaskDelay(pdMS_TO_TICKS(2000));
    log_heap("spk after deinit");
    finish_phase(PHASE_SPK);
}

/* ---- 任务与入口 / Task and entry ---- */

static void auto_test_task(void *arg) {
    const int phase = (int)(intptr_t)arg;
    static const char *k_names[PHASE_COUNT] = {"hotspot", "bluetooth scan", "local speaker"};
    at_log("phase %d start (%s)", phase,
           (phase >= 0 && phase < PHASE_COUNT) ? k_names[phase] : "?");
    log_heap("boot");
    if (phase == PHASE_AP) ap_phase();
    else if (phase == PHASE_BT) bt_phase();
    else spk_phase();
    vTaskDelete(NULL);
}

static int current_phase(void) {
    // 诊断用强制相位：定义 AT_FORCE_PHASE 时跳过 NVS 轮转，只跑指定相位。
    // 需要它是因为 NVS 分区写满后相位计数根本推进不了（nvs_set_i32 返回 NOT_ENOUGH_SPACE）。
    // Diagnostic override: with AT_FORCE_PHASE defined the NVS rotation is skipped and only that
    // phase runs. Needed because a full NVS partition cannot advance the phase counter at all.
#ifdef AT_FORCE_PHASE
    s_phase = AT_FORCE_PHASE;
    return s_phase;
#endif
    if (s_phase >= 0) return s_phase;
    nvs_ready();
    const int run = nvs_get_int("run", -1);
    const int ok = nvs_get_int("ok", -1);
    if (run >= 0 && run != ok) {
        // 上一轮没写完成标记：多半是起热点时崩溃或超时，记录后换另一相位。
        // Last boot never wrote its completion marker (crash or timeout); log it and switch phases.
        nvs_set_int("next", (run + 1) % PHASE_COUNT);
    }
    int phase = nvs_get_int("next", PHASE_AP);
    if (phase < 0 || phase >= PHASE_COUNT) phase = PHASE_AP;
    nvs_set_int("run", phase);
    s_phase = phase;
    return s_phase;
}

void app_auto_test_prepare(void) {
    // 在板级初始化前置位，SD 挂载后的 USJ 软恢复才不会把控制台带走。
    // Set before board init so the post-mount USJ recovery cannot drop the console.
    metalio_usb_serial_hold(true);
    const int phase = current_phase();
    at_log("prepare phase=%d (console held across SD mount)", phase);
}

bool app_auto_test_transfer_page_wanted(void) {
    return current_phase() == PHASE_AP;
}

void app_auto_test_start(void) {
    const int phase = current_phase();
    // 栈给 6 KB：相位任务里只做轮询与打日志，蓝牙栈在 bt_host 组件里。
    // 6 KB stack: the task only polls and logs; the Bluetooth stack lives in bt_host.
    xTaskCreate(auto_test_task, "auto_test", 6144, (void *)(intptr_t)phase, 3, NULL);
}

#else

#include "app_auto_test.h"

void app_auto_test_prepare(void) {}
bool app_auto_test_transfer_page_wanted(void) { return false; }
void app_auto_test_start(void) {}

#endif
