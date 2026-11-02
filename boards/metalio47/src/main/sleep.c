/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 锁屏进睡、浅睡等待按键或拿起、软睡/关机拉掉 EN。
 *
 * Enter lock and sleep, light-sleep wait for key or pickup, and drop EN
 * for soft sleep / power-off.
 * 用户修订：票根从任意页面进入均用系统字体；浅睡唤醒恢复此前阅读字体，深睡由启动检查点恢复。
 * User revision: tickets use the system face from every page; light wake restores the previous reader face, while deep wake uses the startup checkpoint.
 * 用户修订：单击锁屏提示复用Pico布局；双击/PIN和S31外设休眠顺序保持。
 * User revision: use Pico's single-click hint layout, retaining double-click/PIN and S31 peripheral sleep sequencing.
 */

#include "sleep.h"
#include "ui_avatar.h"
#include "book_cover_jobs.h"
#include "metalio_hw.h"
#include "metalio_sleep.h"
#include "ble_page_turner.h"
// 蓝牙诊断：锁屏/浅睡/唤醒的每个节点与 bt_host 的事件写到同一条时间线上。
// Bluetooth diagnostics: every lock/light-sleep/wake milestone joins bt_host's timeline.
#include "bt_trace.h"
// 音乐：进睡时暂停并挂起蓝牙宿主；恢复顺序必须「音乐先、翻页器后」——bt_host 栈由
// 音乐的引用撑着，翻页器 resume 走 nimble_port_init 时要求协议栈已经回到 enabled。
// Music: pause and suspend the Bluetooth host around sleep; resume order must be music
// first, page turner second — the music ref keeps bt_host's stack allocated, and the
// page turner's nimble_port_init needs the stack enabled again by then.
#include "music_player.h"
#include "driver/usb_serial_jtag.h"
#include "ttf_font.h"
#include "output_lcd/lcd_driver.h"
#include "boot_state.h"
#include "lock_pin.h"
#include "lock_screen.h"
#include "read_pico_sd.h"
#include "usb_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <nvs.h>

// 电源/锁屏/唤醒轨迹日志，写入 SD 卡，供实机排查「壁纸 + 再按」等问题。
// Power/lock/wake trace log on SD card for field diagnosis.
// 仅 debug 档编入：release 档不定义 METALIO_DIAG_SD_LOGS，整段调用编译成空操作，
// 交付固件不写卡、不占栈。卡未挂载时 fopen 失败则静默跳过，不影响正常流程。
// Compiled in only for the debug profile: release leaves METALIO_DIAG_SD_LOGS undefined, so
// every call site compiles to a no-op — delivery writes nothing and allocates no stack.
// Silently no-op when the card is unavailable.
#ifndef METALIO_DIAG_SD_LOGS
#define power_trace(...) ((void)0)
#else
static void power_trace(const char *fmt, ...) {
    static bool dir_ok = false;
    if (!dir_ok) { (void)mkdir("/sdcard/.metalio_s31", 0777); dir_ok = true; }
    FILE *f = fopen("/sdcard/.metalio_s31/power_trace.log", "a");
    if (!f) return;
    int64_t t = esp_timer_get_time() / 1000;
    fprintf(f, "[%lld] ", (long long)t);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n");
    (void)fclose(f);
}
#endif
#include <time.h>

#include "app.h"
#include "app_font_context.h"
#include "book_cover.h"
#include "book_source.h"
#include "book_lock_collage.h"
#include "book_lock_bookmark.h"
#include "book_ticket.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pmu_selftest.h"
#include "read_pico_board.h"
#include "read_pico_pmu.h"
#include "read_pico_transfer.h"
#include "sc7a20h_lab.h"
#include "settings.h"
#include "ui_kit.h"
#include "ui_image_dither.h"
#include "ui_wallpaper.h"
#include "ui_lock_feedback.h"
#include "e0470_epaper_waveform.h"
#include "ota_online.h"

static const char* TAG = "read_pico";
// 只消费新动作；单击选项不把持续按住的锁屏长按当作下一次解锁。
// Consume new actions only: a held lock-initiating press cannot unlock again.
static int64_t s_take_key_last_log_us;
static bool lock_take_key_wakeup(void) {
    bool r = app_settings_wake_presses() == 1 ? read_pico_pmu_take_key_wakeup_single() :
        read_pico_pmu_take_key_wakeup();
    // 节流：无按键的轮询每 10s 才写一条，避免锁屏循环把 power_trace.log 刷爆；
    // 真正命中按键立即写（调试深睡唤醒现场时这一条最关键）。power_trace 仅 debug 档生效。
    const int64_t now = esp_timer_get_time();
    if (r || now - s_take_key_last_log_us >= 10000000) {
        s_take_key_last_log_us = now;
        power_trace("take_key_wakeup: presses=%u -> %u", (unsigned)app_settings_wake_presses(), (unsigned)r);
    }
    return r;
}
// 去抖窗口内再确认一次：只等按键序号变化，不写轨迹，避免每次入睡都刷日志。
// Wait one debounce window for a fresh click, polling only the serial so no trace is spammed.
static bool lock_settle_key(void) {
    const uint32_t serial=read_pico_pmu_lock_key_serial();
    for(unsigned i=0;i<4;i++) {
        if(read_pico_pmu_lock_key_serial()!=serial) return lock_take_key_wakeup();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}
static bool s_lock_io_pending;
static bool s_lock_io_no_media;
static int64_t s_lock_io_retry_us;
static unsigned s_lock_io_failures;
static bool s_lock_io_failed;
// 恢复失败不能无限挡住主循环的卡恢复；真正的重挂由消费者释放后执行。
// Bounded reopen failures yield to loop-owned card recovery after consumers close.
static esp_err_t lock_media_result(esp_err_t err) {
    if (err == ESP_OK) { s_lock_io_failures = 0; s_lock_io_failed = false; }
    else if (err != ESP_ERR_NOT_FINISHED && ++s_lock_io_failures >= 3) s_lock_io_failed = true;
    return err;
}
// TF异步挂载完成才恢复书籍/字体；失败保持原路径和挂起句柄，允许后续重试。
// Reopen book/font IO only after asynchronous TF mount; retain paths and suspended handles on failure.
static esp_err_t lock_resume_media_io(uint32_t wait_ms) {
    if (!s_lock_io_pending) return ESP_OK;
    if (usb_storage_active()) return ESP_ERR_INVALID_STATE;
    if (s_lock_io_failed) return ESP_FAIL;
    // 无卡且无待重开的句柄时可以完成恢复；不为内建字体强制挂卡。
    // With no card or suspended handles, complete recovery without mounting for the built-in face.
    if (s_lock_io_no_media) {
        s_lock_io_no_media=false;
        if (!ttf_font_io_pending() && !book_io_pending()) {
            s_lock_io_pending=false; return lock_media_result(ESP_OK);
        }
    }
    read_pico_sd_info_t info = {0};
    esp_err_t err = read_pico_sd_get_info(&info);
    if (err != ESP_ERR_NOT_FINISHED && !info.mounted) err = read_pico_sd_remount();
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (err == ESP_OK || err == ESP_ERR_NOT_FINISHED) {
        err = read_pico_sd_get_info(&info);
        if (err != ESP_ERR_NOT_FINISHED) break;
        if (esp_timer_get_time() >= deadline) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (err != ESP_OK || !info.mounted)
        return lock_media_result(err == ESP_OK ? ESP_ERR_INVALID_STATE : err);
    esp_err_t font = ttf_font_resume_io(), book = book_resume_io();
    if (font == ESP_OK && book == ESP_OK) s_lock_io_pending = false;
    return lock_media_result(font != ESP_OK ? font : book);
}
static void lock_resume_io_service(void) {
    metalio_sleep_report_reset();
    if (s_lock_io_pending && esp_timer_get_time() >= s_lock_io_retry_us) {
        s_lock_io_retry_us = esp_timer_get_time() + INT64_C(1000000);
        (void)lock_resume_media_io(0);
    }
}
bool app_sleep_media_ready(void) {
    // 解锁后仍继续恢复；不能把有意卸载当拔卡，也不能销毁尚待恢复的字体槽。
    // Continue after unlock; intentional unmount is not card removal and pending font slots must survive.
    lock_resume_io_service();
    return !s_lock_io_pending;
}
bool app_sleep_media_failed(void) { return s_lock_io_pending && s_lock_io_failed; }
bool app_sleep_media_pending(void) { return s_lock_io_pending; }
void app_sleep_discard_media_io(void) {
    s_lock_io_pending = false;
    s_lock_io_no_media = false;
    s_lock_io_retry_us = 0;
    s_lock_io_failures = 0;
    s_lock_io_failed = false;
}
static bool s_lock_feedback_started;
static bool s_lock_deep_ready;
static bool s_lock_frame_cached;
static cst836u_handle_t s_lock_sleep_touch;
static bool s_boot_from_deep, s_boot_power_wake, s_boot_power_on;
static bool s_boot_key_proven;
static uint32_t s_boot_reset_code;
// 深睡前写入、开机读走的一次性 RTC 标记。
// One-shot RTC marker written just before deep sleep and consumed on the next boot.
//
// 为什么需要它（实测日志与代码对齐后的结论）：
//   10 分钟锁屏走 lock_light_sleep_once → lock_deep_parked → esp_deep_sleep_start，
//   但唤醒后 boot_gate 打出来的是 deep=0 power_on=0 —— 这块板子深睡唤醒后
//   esp_reset_reason() 既不是 ESP_RST_DEEPSLEEP 也不是 ESP_RST_POWERON。
//   门禁只认复位原因，于是「唤醒那一按」被判丢：先画壁纸，等用户再按一次。
//   深睡前 lock_prepare_key_wake() 已经把唤醒源清空、只留 P17 一条 EXT1 线
//   （TIMER 也被 lock_deep_parked 关掉），所以「带着标记醒过来」本身就等于按过一次键。
// Why: the ten-minute lock does park in deep sleep, yet the log shows deep=0 power_on=0 —
// this board reports neither DEEPSLEEP nor POWERON after a deep wake. The gate keyed off the
// reset reason therefore dropped the wake press (wallpaper first, second press needed).
// Deep sleep leaves only P17 armed as EXT1 (TIMER is disabled too), so a surviving marker is
// itself proof that the key was pressed.
#define S31_DEEP_WAKE_MAGIC UINT32_C(0x53314450)  // "S1DP"
RTC_NOINIT_ATTR static uint32_t s_deep_wake_mark;
// 实测（power_trace.log）：深睡唤醒的复位原因是 14 = ESP_RST_PWR_GLITCH 或 7 = ESP_RST_WDT
// （2026-11-01 的 bt_trace/font_diag 这一轮报的是 7），两种情况都不是 DEEPSLEEP。
// 也就是说这块板在深睡里经历过电源域跌落，RTC_NOINIT 标记（以及 bootloader 写在同一块的
// 早期按键记录）全都保不住，门禁于是判「没有按键」→ 先画壁纸 → 等第二下。
// 所以真正的证据改放在 NVS：深睡前落一个标记（flash 由主电源供电，与外设轨无关），
// 开机在 NVS 就绪后读出来，命中就清掉并直接按「按键唤醒」放行。
// 同一个标记也被启动保护复用：reset=7 落在 WDT 名单里，未消费的标记即为按键唤醒的证据，
// 不再把深睡唤醒误判成异常启动（否则字体整会话被锁在内建）。
// Measured on device: a deep wake reports reset=14 (ESP_RST_PWR_GLITCH) or reset=7
// (ESP_RST_WDT, seen on 2026-11-01), never DEEPSLEEP, so the RTC domain is not preserved and both
// the RTC_NOINIT mark and the bootloader's early-key record are gone. Keep the proof in NVS
// instead: write it before deep sleep, consume it on next boot. Startup protection reuses the same
// marker to excuse that WDT code, otherwise every deep wake would latch font recovery.
#define S31_WAKE_NS  "s31wake"
#define S31_WAKE_KEY "deep"
static bool s_boot_nvs_mark, s_wake_mark_resolved;

static bool lock_wake_mark_read(void) {
    nvs_handle_t h;
    if (nvs_open(S31_WAKE_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    const esp_err_t err = nvs_get_u8(h, S31_WAKE_KEY, &v);
    nvs_close(h);
    return err == ESP_OK && v == 1;
}
static void lock_wake_mark_write(uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(S31_WAKE_NS, NVS_READWRITE, &h) != ESP_OK) return;
    // 只在值真的变了才写，避免每次开机都白擦一次 flash。
    // Only write on a value change so a normal boot does not touch flash.
    uint8_t old = 0xff;
    if (nvs_get_u8(h, S31_WAKE_KEY, &old) == ESP_OK && old == v) { nvs_close(h); return; }
    if (nvs_set_u8(h, S31_WAKE_KEY, v) == ESP_OK) (void)nvs_commit(h);
    nvs_close(h);
}
static void lock_mark_deep_wake(void) {
    s_deep_wake_mark = S31_DEEP_WAKE_MAGIC;
    lock_wake_mark_write(1);
}
// 深睡进入标记是否仍未消费（只读，不清除）。pico_boot_init 用它豁免「切轨毛刺→中途
// 重启」链留下的 starting=1：标记还在说明上次深睡进入是干净的（检查点写完才切轨）。
// True when the deep-entry mark is still unconsumed (read-only). pico_boot_init uses it to
// excuse a leftover starting=1 from a rail-cut glitch reboot chain: the mark surviving means
// the deep entry itself was clean (checkpoint written before the rails were cut).
bool metalio_deep_wake_mark_pending(void) {
    return lock_wake_mark_read();
}
static void lock_clear_deep_wake(void) {
    s_deep_wake_mark = 0;
    lock_wake_mark_write(0);
}
// NVS 在 app_settings_init() 之后才可用，所以这一步单独做：命中标记就把它算成按键唤醒。
// NVS only works after app_settings_init(), so resolve the mark separately from the capture.
void app_lock_resolve_wake_mark(void) {
    if (s_wake_mark_resolved) return;
    s_wake_mark_resolved = true;
    if (s_boot_key_proven) { lock_wake_mark_write(0); return; }
    if (!lock_wake_mark_read()) return;
    lock_wake_mark_write(0);
    s_boot_nvs_mark = true;
    s_boot_power_wake = true;
    s_boot_key_proven = true;
    power_trace("=== deep wake mark: NVS hit reset=%u -> key wake, gate open ===",
        (unsigned)s_boot_reset_code);
}
void app_lock_capture_boot_wakeup(void) {
    s_boot_reset_code=(uint32_t)esp_reset_reason();
    const bool marked=s_deep_wake_mark==S31_DEEP_WAKE_MAGIC;
    if(marked)lock_clear_deep_wake();
    s_boot_power_on=s_boot_reset_code==ESP_RST_POWERON;
    s_boot_from_deep=s_boot_reset_code==ESP_RST_DEEPSLEEP || marked;
    // 深睡前 lock_prepare_key_wake() 先 disable 掉所有唤醒源，再只留
    // 1<<READ_PICO_IOE_INT_GPIO 这一条 EXT1 线，所以「唤醒原因=EXT1」本身就已经是
    // 一次电源键，不需要再拿位图确认。旧实现多要了那一位，一旦位图读回 0
    // （不同芯片/复位后清库差异），这一按就被判丢，深睡唤醒便停在壁纸等第二下。
    // Deep sleep arms exactly one EXT1 line, so "cause == EXT1" already proves a press.
    // The old code also demanded the status bit; when that bitmap read back 0 the press was
    // dropped and the wake parked on the wallpaper until a second tap.
    const uint64_t ext1=s_boot_from_deep?esp_sleep_get_ext1_wakeup_status():0;
    const bool ext1_cause=esp_sleep_get_wakeup_cause()==ESP_SLEEP_WAKEUP_EXT1;
    s_boot_power_wake=marked || (s_boot_from_deep && ext1_cause &&
        (ext1==0 || (ext1&(UINT64_C(1)<<READ_PICO_IOE_INT_GPIO))!=0));
    // 唤醒那一按是否成立：门禁与 app_main 的「要不要先画壁纸」都只看这一个结论。
    // Whether the wake press counts: both the gate and app_main's wallpaper decision use it.
    s_boot_key_proven=s_boot_power_wake;
    power_trace("=== boot wake === reset=%u mark=%u deep=%u power_on=%u cause_ext1=%u ext1=%llu power_wake=%u",
        (unsigned)s_boot_reset_code, (unsigned)marked, (unsigned)s_boot_from_deep, (unsigned)s_boot_power_on,
        (unsigned)ext1_cause, (unsigned long long)ext1, (unsigned)s_boot_power_wake);
}
bool app_lock_deep_wake_key(void) { return s_boot_key_proven; }
static esp_err_t lock_light_sleep_once(sc7a20h_handle_t acc, cst836u_handle_t touch, bool *key_wake, int64_t wake_by, bool off_at_deadline);
void app_lock_set_resume_ready(bool ready) { s_lock_deep_ready = ready; }



extern const uint8_t lock_4bpp_bin_start[] asm("_binary_lock_4bpp_bin_start");

// 中文：保留S31灭灯/按键边界，反馈期间的新按键留给锁屏唤醒。
// English: Keep S31 lamp/key boundaries; preserve new presses during feedback for waking.
void app_lock_feedback(EpdiyHighlevelState *hl) {
    s_lock_feedback_started = false;
    if (!hl || app_settings_lock_style() != 2 || pico_online_busy()) return;
    read_pico_transfer_status_t transfer = {0};
    read_pico_transfer_get_status(&transfer);
    if (transfer.state == READ_PICO_TRANSFER_UPLOADING) return;
    uint8_t *fb = epd_hl_get_framebuffer(hl);
    if (!fb || epd_rotated_display_width() < 97 || epd_rotated_display_height() < 97) return;
    metalio_frontlight(0, 50);
    read_pico_pmu_drain_events();
    s_lock_feedback_started = true;
    display_main_transition_cancel();
    epd_hl_set_all_white(hl);
    if (update_display_full(hl) != EPD_DRAW_SUCCESS) return;
    EpdRect area = ui_lock_feedback_draw(fb);
    (void)update_display_area_diff_with(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, area);
    epd_poweroff();
}

static bool draw_wallpaper(uint8_t *framebuffer) {
    return app_settings_lock_style() == 1 &&
        ui_wallpaper_prepare_lock(framebuffer, app_settings_wallpaper_path());
}

void app_lock_wait_key_idle(int timeout_ms) {
    int64_t start = esp_timer_get_time() / 1000;
    int64_t released_from = -1;
    while (esp_timer_get_time() / 1000 - start < timeout_ms) {
        read_pico_pmu_drain_events();
        read_pico_clear_ioe_int();
        bool held = true;
        if (read_pico_pmu_poll() == ESP_OK) {
            held = (read_pico_pmu_get()->key_state & 0x01) != 0;
        }
        // GPIO2还承载触摸/传感器中断，不能用它判断电源键是否松开。
        // GPIO2 also carries touch/sensor IRQs; use the debounced TCA power-key state.
        if (!held) {
            if (released_from < 0) released_from = esp_timer_get_time() / 1000;
            if (esp_timer_get_time() / 1000 - released_from >= 30) return;
        } else {
            released_from = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

app_wake_source_t app_last_wake_source(void) {
    return (app_wake_source_t)app_settings_last_wake();
}

// 单击等待650ms再局部提示，三秒后还原原图；不唤灯、不把触摸当作解锁。
// Wait650ms for a double click, show locally for3s, then restore art without waking the lamp or granting touch unlock.
static EpdiyHighlevelState *s_lock_hint_hl;
static EpdiyHighlevelState *s_boot_lock_hl;
static bool s_boot_lock_active, s_boot_lock_known, s_boot_lock_draw;

static enum EpdDrawError lock_present_art(EpdiyHighlevelState *hl,uint8_t style) {
    if (style == 1) {
        // 壁纸直接从真实旧帧走一轮完整GC48，避免先白屏再输出；不继承文件/设置页的黑白策略。
        // Present wallpaper from the real prior frame in one native GC48 pass, bypassing inherited system BW policy.
        const bool binary = display_system_binary_set(false);
        enum EpdDrawError result = update_display_full(hl);
        display_system_binary_set(binary);
        return result;
    }
    // PNG已合成在目标帧中；只清除物理旧态再输出，透明区的底页和不透明主体都保留。
    // The PNG is already composed into the target; clear only the prior optical state, retaining both the backdrop and opaque artwork.
    return style==3 ? update_display_image_gray(hl) : update_display_full(hl);
}

// 未恢复像素前不做差分提示；缺卡/坏透明底图仍保留物理锁图，不假设白底。
// Never hint against unknown pixels; absent media or a damaged transparent backdrop keeps physical art rather than assuming white.
static void lock_resume_service(void) {
    EpdiyHighlevelState *hl=s_boot_lock_hl;
    if(!hl)return;
    // 启动锁屏恒为浅色输出：退出夜间极性，锁屏画面与其后差分都按浅色语义。/ The boot lock stays light; drop night polarity first.
    epd_set_inverted(false);
    read_pico_sd_info_t info={0};
    if(read_pico_sd_get_info(&info)==ESP_ERR_NOT_FINISHED)return;
    if(!info.mounted){s_boot_lock_hl=NULL;return;}
    uint8_t *fb=epd_hl_get_framebuffer(hl);
    bool restored=lock_screen_restore(fb);
    if(!restored && !s_boot_lock_draw)return;
    s_boot_lock_hl=NULL;
    if(!restored) {
        uint8_t style=app_settings_lock_style();
        if(style==3)return;
        epd_hl_set_all_white(hl);
        app_lock_font_t saved;app_font_begin_lock(&saved);
        bool drawn=style==0?book_ticket_draw(fb,false):style==2?book_lock_collage_prepare(fb):draw_wallpaper(fb);
        app_font_end_lock(&saved);
        if(!drawn)return;
    }
    if(restored && pico_boot_resume_frame_cached())
        memcpy(hl->back_fb,fb,(size_t)epd_width()*epd_height()/2);
    else {
        enum EpdDrawError result=lock_present_art(hl,app_settings_lock_style());
        guard_draw_result(hl,result);epd_poweroff();
        if(result!=EPD_DRAW_SUCCESS)return;
        if(app_settings_lock_style()==2)book_lock_collage_cache_commit(fb);
        if(app_settings_lock_style()==1)ui_wallpaper_cache_commit(fb);
        if(lock_screen_save(fb))(void)pico_boot_mark_frame_cached();
    }
    s_boot_lock_known=true;s_lock_hint_hl=hl;
}

bool app_lock_resume_frame(EpdiyHighlevelState *hl) {
    s_boot_lock_hl=hl;s_boot_lock_active=hl!=NULL;
    s_boot_lock_known=s_boot_lock_draw=false;s_lock_hint_hl=NULL;
    lock_resume_service();return s_boot_lock_known;
}

static struct {
    uint8_t *pixels;
    EpdRect area;
    uint32_t serial;
    int64_t pending_at, until;
} s_lock_hint;

static uint8_t lock_hint_pixel(uint8_t *fb,int x,int y) {
    int px=x,py=y;
    switch(epd_get_rotation()) {
        case EPD_ROT_PORTRAIT:px=epd_width()-y-1;py=x;break;
        case EPD_ROT_INVERTED_LANDSCAPE:px=epd_width()-x-1;py=epd_height()-y-1;break;
        case EPD_ROT_INVERTED_PORTRAIT:px=y;py=epd_height()-x-1;break;
        default:break;
    }
    return epd_get_pixel(px,py,epd_width(),epd_height(),fb);
}

static void lock_hint_present(void) {
    epd_poweron();
    enum EpdDrawError result=update_display_area_diff_with(s_lock_hint_hl,&E0470_WAVEFORM,MODE_GL16,s_lock_hint.area);
    guard_draw_result(s_lock_hint_hl,result);
    epd_poweroff();
}

static void lock_hint_finish(bool restore) {
    if(s_lock_hint.pixels) {
        uint8_t *fb=epd_hl_get_framebuffer(s_lock_hint_hl);
        EpdRect r=s_lock_hint.area;
        for(int y=0;y<r.height;++y)for(int x=0;x<r.width;++x) {
            size_t i=(size_t)y*r.width+x;
            uint8_t v=(s_lock_hint.pixels[i/2]>>((i&1)*4))&15;
            epd_draw_pixel(r.x+x,r.y+y,(uint8_t)(v*17),fb);
        }
        if(restore)lock_hint_present();
        heap_caps_free(s_lock_hint.pixels);s_lock_hint.pixels=NULL;
    }
    s_lock_hint.pending_at=s_lock_hint.until=0;
}

static void lock_hint_service(void) {
    if(!s_lock_hint_hl || app_settings_wake_presses() != 2)return;
    int64_t now=esp_timer_get_time()/1000;
    uint32_t serial=read_pico_pmu_lock_key_serial();
    if(serial!=s_lock_hint.serial) {
        s_lock_hint.serial=serial;s_lock_hint.pending_at=now+650;
    }
    if(s_lock_hint.pending_at && now>=s_lock_hint.pending_at) {
        s_lock_hint.pending_at=0;s_lock_hint.until=now+3000;
        if(!s_lock_hint.pixels) {
            uint8_t *fb=epd_hl_get_framebuffer(s_lock_hint_hl);
            const int w=epd_rotated_display_width(),h=epd_rotated_display_height();
            s_lock_hint.area=(EpdRect){(w-448)/2,h-196,448,72};
            if(fb && w>=448 && h>=268)
                s_lock_hint.pixels=heap_caps_calloc(1,448u*72u/2u,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(s_lock_hint.pixels) {
                EpdRect r=s_lock_hint.area;
                for(int y=0;y<r.height;++y)for(int x=0;x<r.width;++x) {
                    size_t i=(size_t)y*r.width+x;
                    s_lock_hint.pixels[i/2]|=(lock_hint_pixel(fb,r.x+x,r.y+y)>>4)<<((i&1)*4);
                }
                ui_fill_round_rect(fb,r,28,UI_GRAY_WHITE);
                ui_draw_round_rect(fb,r,28,UI_GRAY_BLACK);
                ui_text_fixed_vc(fb,w/2,r.y+r.height/2,28,"双击电源键解锁",EPD_DRAW_ALIGN_CENTER,false);
                lock_hint_present();
            }
        }
    }
    if(s_lock_hint.until && now>=s_lock_hint.until)lock_hint_finish(true);
}


static void lock_hint_begin(EpdiyHighlevelState *hl) {
    s_lock_hint_hl=s_boot_lock_active&&!s_boot_lock_known?NULL:hl;
    s_lock_hint.serial=read_pico_pmu_lock_key_serial();
}
static void lock_hint_end(void) {
    lock_hint_finish(false);
    s_lock_hint_hl=NULL;
}

// 锁屏先浅睡，十分钟后同步外挂RTC并硬关机；USB/输入和交接失败保持锁定。
// Light-sleep locks, then sync the external RTC and power off after ten minutes; failed handoffs retain the gate.
app_wake_source_t app_light_sleep_wait_timed(sc7a20h_handle_t acc,uint32_t timeout_ms) {
    if (!(book_cover_jobs_quiesce(2000) && ui_avatar_quiesce(2000))) return APP_WAKE_TIMEOUT;
    const int64_t start=esp_timer_get_time();
    const int64_t deadline=timeout_ms?start+(int64_t)timeout_ms*1000:0;
    bool pickup=acc && app_settings_pickup_wake();
    sc7a20h_sample_t last={0}; bool last_valid=false;
    int64_t motion_at=start;
    if(pickup) sc7a20h_power_up(acc);
    else if(acc) (void)sc7a20h_shared_irq_idle(acc);
    metalio_frontlight(0,50);
    app_wake_source_t wake=APP_WAKE_NONE;
    bool usb_was_blocked=metalio_usb_blocks_sleep();
    // 锁屏后多久进深睡由设置决定：立即/10/30/60 分钟/从不；0 表示从不（整夜浅睡）。
    // The lock-to-deep-sleep delay is a setting now (immediate/10/30/60/never); 0 means never.
    const int64_t off_delay=app_settings_sleep_off_us();
    int64_t off_at=s_lock_sleep_touch&&!timeout_ms&&off_delay>0 ? start+off_delay : 0;
    int64_t off_retry_at=off_at;
    // 浅睡播放音乐：只在锁屏上下文生效；音乐播完/暂停后按设置重新计时。
    // Music through light sleep applies to the lock context only; once playback ends the
    // configured delay restarts from that moment.
    const bool music_awake=s_lock_sleep_touch&&app_settings_music_in_sleep();
    bool frame_marked=false,frame_mark_attempted=false;
    unsigned light_failures=0;
    int64_t light_retry_at=0;
    ESP_LOGI(TAG,"S31 lock wait: off_delay_ms=%lld usb=%u pickup=%u checkpoint=%u music=%u",
        (long long)(off_delay/1000),(unsigned)usb_was_blocked,(unsigned)pickup,
        (unsigned)s_lock_deep_ready,(unsigned)music_awake);
    // 锁屏动作本身那一按的 PMU 事件可能晚于入口 drain 才落进 FIFO（去抖延迟），
    // 若不清掉，循环首轮 lock_take_key_wakeup() 会把它当成一次新的电源键按下而直接解锁——
    // 表现就是「锁屏后没按电源键，却被唤醒」。
    // 走到这里时锁图已经推完（秒级），这一按的事件必然已在队列里，清掉之后 FIFO 里再出现的
    // 才是锁屏期间真正的新按键。
    // The lock-initiating press can land in the PMU FIFO after the entry drain (debounce).
    // Clear it once more after the art is presented; only later presses may then wake.
    if(s_lock_sleep_touch) read_pico_pmu_drain_events();
    for(;;) {
        // 锁屏等待期间只接受带硬件证据（P17 此刻仍为低）的按下：PMU 里的一条历史键事件
        // 不足以授权解锁，否则锁屏动作本身的那一按就会在循环首轮把设备唤醒。
        // 无硬件证据时不消费事件，交给 lock_light_sleep_once() 的 P17/EXT1 判定，
        // 避免把一次真实的轻按吞掉。
        // Only a hardware-backed press (P17 low) may unlock during the lock wait; a lone
        // PMU history event must not. Without evidence the event is left for the P17/EXT1
        // path in lock_light_sleep_once(), so a real quick tap is never swallowed.
        if(metalio_power_key_evidence().seen && lock_take_key_wakeup()) { wake=APP_WAKE_KEY; break; }
        const bool keep_music=music_awake && music_state()==MUSIC_PLAYING;
        // 放着音乐就不进浅睡、也不深睡：浅睡会停 CPU 与电源轨，I2S 立刻断流。
        // Keep the CPU up while audio plays: light sleep stops it and the I2S stream dies.
        if(keep_music && off_at) off_retry_at=off_at=esp_timer_get_time()+off_delay;
        bool usb=metalio_usb_blocks_sleep();
        if (usb && !usb_was_blocked) metalio_usb_serial_reconnect();
        // 充电暂缓关机；拔线后重新计满十分钟，不能执行充电期间已过期的截止时间。
        // Charging defers shutdown; unplugging starts a fresh ten-minute interval.
        if (usb && off_at) off_retry_at=off_at=esp_timer_get_time()+off_delay;
        if (!usb && usb_was_blocked && off_at)
            off_retry_at=off_at=esp_timer_get_time()+off_delay;
        usb_was_blocked=usb;
        lock_resume_io_service();
        lock_resume_service();
        lock_hint_service();
        if (!s_lock_hint.pending_at && !s_lock_hint.until && s_lock_sleep_touch && !pickup && !usb && !keep_music) {
            // 入睡/关机前的最后确认：键采样任务每 10 ms 读一次、要 25 ms 才认一次短按，
            // 30 ms 的轮询可能刚好在它成立前跑过，随后这段准备要 2 s 以上，
            // 这一按就要等准备完才被消费——用户看到的就是「按了没反应」。
            // Last check before the multi-second suspend: the sampler needs 25 ms to accept a
            // click, so a 30 ms poll can pass just before it lands and the press then waits for
            // the whole suspend before anyone consumes it — which reads as "the press did nothing".
            if(lock_settle_key()) { wake=APP_WAKE_KEY; break; }
            if(off_at && esp_timer_get_time()>=off_retry_at && s_lock_deep_ready) {
                if(s_lock_frame_cached && !lock_pin_enabled() && !frame_marked && !frame_mark_attempted) {
                    frame_mark_attempted=true;
                    frame_marked=pico_boot_mark_frame_cached()==ESP_OK;
                }
                esp_err_t err=app_enter_host_sleep(APP_SLEEP_OFF);
                (void)lock_resume_media_io(0);
                // 关机未完成仍保持锁定；30秒后重试，不因卡未挂载永远禁用关机。
                // A failed power-off stays locked and retries after30s, without requiring a mounted card.
                off_retry_at=esp_timer_get_time()+INT64_C(30000000);
                ESP_LOGW(TAG,"S31 lock power-off deferred: %s; retry_ms=30000",esp_err_to_name(err));
            }
            if(!s_lock_io_pending && esp_timer_get_time()>=light_retry_at) {
                bool key=false;
                int64_t wake_by=deadline;
                if(off_at && s_lock_deep_ready && (!wake_by || off_retry_at<wake_by)) wake_by=off_retry_at;
                bool off_deadline=off_at && s_lock_deep_ready && wake_by==off_retry_at;
                if(off_deadline && s_lock_frame_cached && !lock_pin_enabled() && !frame_marked && !frame_mark_attempted) {
                    frame_mark_attempted=true;
                    frame_marked=pico_boot_mark_frame_cached()==ESP_OK;
                }
                power_trace("lock_wait: off_deadline=%u wake_by_set=%u deep_ready=%u -> enter light sleep once",
                    (unsigned)off_deadline,(unsigned)(wake_by!=0),(unsigned)s_lock_deep_ready);
                esp_err_t err=lock_light_sleep_once(acc,s_lock_sleep_touch,&key,wake_by,off_deadline);
                if(key) { wake=APP_WAKE_KEY; break; }
                if(off_deadline && err==ESP_ERR_TIMEOUT && esp_timer_get_time()>=off_retry_at)
                    off_retry_at=esp_timer_get_time()+INT64_C(30000000);
                // 为按键去抖/双击和提示保留窗口；一次中断或I/O失败不能整晚禁止浅睡。
                // Leave time for debounce/double-click/hints; one IRQ or IO failure must not disable sleep all night.
                static const unsigned retry_ms[]={1000,3000,10000,30000};
                unsigned delay=1000;
                if(err==ESP_OK) light_failures=0;
                else {
                    unsigned step=light_failures<4?light_failures++:3;
                    delay=retry_ms[step];
                    ESP_LOGW(TAG,"S31 light deferred: %s; retry_ms=%u",esp_err_to_name(err),delay);
                }
                light_retry_at=esp_timer_get_time()+(int64_t)delay*1000;
            }
        }
        int64_t now_us=esp_timer_get_time();
        if(pickup && now_us>=motion_at) {
            motion_at=now_us+250000;
            sc7a20h_sample_t now;
            // 仅比较新的完整样本，降低采样频率；短按由原厂TCA后台任务去抖。
            // Compare fresh complete samples at 4Hz; vendor TCA background sampling debounces short keys.
            if(sc7a20h_read(acc,&now)==ESP_OK && (now.status&0x08)) {
                if(last_valid && (abs(now.x_mg-last.x_mg)+abs(now.y_mg-last.y_mg)+abs(now.z_mg-last.z_mg)>550)) { wake=APP_WAKE_PICKUP; break; }
                last=now;last_valid=true;
            }
        }
        if(deadline && esp_timer_get_time()>=deadline && !usb) { wake=APP_WAKE_TIMEOUT; break; }
        read_pico_clear_ioe_int();
        // 不重复操作低功耗/唤醒寄存器；每轮阻塞以保证按键、空闲任务和看门狗得到调度。
        // Avoid repeated sleep/wake register transitions; block each loop for key/idle/watchdog scheduling.
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    lock_hint_finish(wake!=APP_WAKE_KEY);
    if(pickup) (void)sc7a20h_shared_irq_idle(acc);
    if (wake != APP_WAKE_TIMEOUT) metalio_frontlight_restore();
    app_settings_set_last_wake((uint8_t)(wake==APP_WAKE_TIMEOUT?APP_WAKE_NONE:wake));
    ESP_LOGI(TAG,"S31 retained lock exit cause=%u elapsed_ms=%lld irq=%d",(unsigned)wake,
        (long long)((esp_timer_get_time()-start)/1000),read_pico_ioe_int_level());
    return wake;
}

app_wake_source_t app_light_sleep_wait(sc7a20h_handle_t acc) {
    return app_light_sleep_wait_timed(acc, 0);
}

static void lock_restore_key_wake(void) {
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_err_t err=metalio_restore_key_io();
    read_pico_pmu_keys_resume();
    if(err!=ESP_OK) ESP_LOGE(TAG,"S31 sleep input restore: %s",esp_err_to_name(err));
}

static bool lock_prepare_key_wake(void) {
    if(read_pico_pmu_keys_pause()!=ESP_OK) return false;
    esp_err_t err=metalio_prepare_key_wake();
    bool idle=false;
    // 对齐原厂 WaitIntLineHigh 的有界等待；确认电源键和共享低有效线已释放。
    // Follow the vendor bounded WaitIntLineHigh check; confirm key and shared active-low IRQ release.
    for(unsigned tries=0;err==ESP_OK && tries<30;tries++) {
        uint16_t inputs;
        err=metalio_io_read(&inputs);
        if(err==ESP_OK && (inputs&(1u<<15)) && read_pico_ioe_int_level()!=0) { idle=true; break; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if(idle) {
        err=esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        if(err==ESP_OK) err=esp_sleep_enable_ext1_wakeup_io(1ULL<<READ_PICO_IOE_INT_GPIO,ESP_EXT1_WAKEUP_ANY_LOW);
    }
    if(err==ESP_OK && idle) return true;
    ESP_LOGW(TAG,"S31 deep sleep deferred: shared IRQ=%d result=%s",read_pico_ioe_int_level(),esp_err_to_name(err));
    lock_restore_key_wake();
    return false;
}

// 中文：按官方 power_hw 的顺序执行；保留PSRAM/排版/字体缓存，定时唤醒不解锁或点灯。
// English: Follow vendor power_hw ordering; timer wakes retain RAM/layout/font caches and never unlock/light.
// 维护切片不跨越十分钟交接点；定时到达只恢复外设，不作为解锁动作。
// Maintenance slices cannot overrun the ten-minute handoff; a deadline restores peripherals, never unlocks.
static uint64_t lock_light_sleep_slice_us(int64_t wake_by) {
    int64_t remaining=wake_by?wake_by-esp_timer_get_time():INT64_C(60000000);
    if(remaining<1) return 1;
    return remaining<INT64_C(60000000)?(uint64_t)remaining:UINT64_C(60000000);
}
// TF/字体已同步关闭且电源轨仍停放时直接转入深睡，不先恢复LCD/TF/前光。
// With media already closed/synced and rails parked, drop straight into deep sleep
// without reviving LCD/SD/light.
static esp_err_t lock_deep_parked(void) {
    power_trace("deep_parked: enter (10min lock -> deep sleep)");
    esp_err_t rtc=app_sync_time_to_pmu();
    if(rtc!=ESP_OK && rtc!=ESP_ERR_INVALID_STATE) {
        vTaskDelay(pdMS_TO_TICKS(20));rtc=app_sync_time_to_pmu();
    }
    if(rtc!=ESP_OK) ESP_LOGW(TAG,"S31 parked deep RTC: %s; keep existing time",esp_err_to_name(rtc));
    if(metalio_usb_blocks_sleep() || metalio_confirm_power_key())return ESP_ERR_INVALID_STATE;
    metalio_frontlight(0,50);
    // 原来这里硬切电源；真断电后重新开机要按住电源键约 1 s（由电源电路决定，
    // 发生在 ESP32 上电之前），用户要求一次按键就进系统，所以改为保电深睡：
    // 调用点已经停好屏/轨/SD，并由 lock_prepare_key_wake() 配好 EXT1 唤醒。
    // This used to hard-cut power; booting afterwards needs a ~1 s key hold handled
    // by the power circuit before the ESP32 is powered, so park the CPU in deep
    // sleep instead — the caller has already parked panel/rails/SD and armed EXT1.
    // 关掉浅睡的定时切片，否则深睡会被每 ≤60 s 的定时器反复叫醒。
    // Drop the light-sleep timer slice, or deep sleep would wake every ≤60 s.
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    // 只留 P17 一条唤醒线，标记就是「下一次开机=按键唤醒」的证据。
    // 标记要写 flash（NVS），必须赶在 metalio_hold_for_deep_sleep() 锁住引脚之前。
    // With P17 as the only wake source, the marker proves the next boot was a key wake.
    // The marker lives in NVS (flash), so write it before hold_for_deep_sleep pins the IOs.
    lock_mark_deep_wake();
    metalio_hold_for_deep_sleep();
    esp_deep_sleep_start();
    lock_clear_deep_wake();
    vTaskDelay(pdMS_TO_TICKS(1000));
    return ESP_ERR_TIMEOUT; // 深睡不返回。/ Deep sleep never returns.
}
static esp_err_t lock_light_sleep_once(sc7a20h_handle_t acc,cst836u_handle_t touch,bool *key_wake,int64_t wake_by,bool off_at_deadline) {
    *key_wake=false;
    if (!touch || metalio_usb_blocks_sleep() || usb_storage_active() || pico_online_busy()) {
        power_trace("lso skip@562: touch=%u usb=%u storage=%u online=%u",
            (unsigned)(touch==NULL),(unsigned)metalio_usb_blocks_sleep(),
            (unsigned)usb_storage_active(),(unsigned)pico_online_busy());
        return ESP_ERR_INVALID_STATE;
    }
    read_pico_sd_info_t info={0};
    // 挂载/写卡事务仍需等待；无卡或已挂载失败不等于有活动I/O。
    // Defer active mount/write transactions; absent or failed media is not active IO.
    esp_err_t media_state=read_pico_sd_get_info(&info);
    if(media_state==ESP_ERR_NOT_FINISHED || (media_state!=ESP_OK && info.mounted)) {
        power_trace("lso skip@568: media=%s mounted=%u", esp_err_to_name(media_state),(unsigned)info.mounted);
        return media_state;
    }
    bool touch_asleep=false,sd_closed=false,lcd_stopped=false,keys_ready=false,rails_cut=false,pins_prepared=false;
    uint16_t saved_rails=0;
    bool key_irq=false,power_confirmed=false,key_seen=false,key_stable=false,key_readable=false;
    s_lock_io_no_media=!info.mounted && !s_lock_io_pending;
    s_lock_io_pending=true;
    // 诊断日志必须在动 SD 总线之前落盘：下面 read_pico_sd_sync 之后就要重配 SDIO 引脚、
    // 断轨，那时再写卡会踩在已停的总线上。写不进去的记录留在缓冲里，唤醒后补写。
    // The diagnostics must hit the card before the SD bus is touched: read_pico_sd_sync is
    // followed by SDIO pad reconfiguration and rail cuts, where a write would land on a dead
    // bus. Whatever cannot be written stays buffered and goes out after wake.
    bt_trace_flush();
    esp_err_t err=book_suspend_io();
    if(err==ESP_OK) err=ttf_font_suspend_io();
    if(err==ESP_OK) {
        err=cst836u_set_mode(touch,CST836U_MODE_DEEPSLEEP);
        touch_asleep=err==ESP_OK;
    }
    if(err==ESP_OK) { err=read_pico_sd_sync(); sd_closed=err==ESP_OK; }
    if(err==ESP_OK) {
        metalio_haptic_stop();
        metalio_frontlight(0,50);
        epd_poweroff();
        vTaskDelay(pdMS_TO_TICKS(120));
        epd_lcd_deinit(); lcd_stopped=true;
        pins_prepared=true;
        err=metalio_prepare_light_sleep_pins();
        if(err==ESP_OK && acc) err=sc7a20h_shared_irq_idle(acc);
    }
    // SD/屏总线准备期间仍由正常按键任务采样，不吞掉这段时间的新短按。
    // Keep clicks sampled during SD/panel preparation instead of masking them away.
    if(err==ESP_OK && lock_take_key_wakeup()) {
        *key_wake=true; err=ESP_ERR_INVALID_STATE;
    }
    if(err==ESP_OK) { keys_ready=lock_prepare_key_wake(); if(!keys_ready){ power_trace("lso skip@596: keys_ready=0"); err=ESP_ERR_INVALID_STATE; } }
    if(err==ESP_OK) err=esp_sleep_enable_timer_wakeup(lock_light_sleep_slice_us(wake_by));
    if(err==ESP_OK && metalio_usb_blocks_sleep()) err=ESP_ERR_INVALID_STATE;
    if(err==ESP_OK) {
        rails_cut=true; err=metalio_sleep_cut_rails(&saved_rails);
    }
    if(err==ESP_OK) {
        err=metalio_hold_for_light_sleep();
    }
    if(err==ESP_OK) {
        ESP_LOGI(TAG,"S31 light sleep: slice<=60s; handles paused, RAM/font/index retained");
        // 浅睡切掉 P14，模组等于断电：唤醒后必须是冷启动 → 模式 2 → 耳机回连，
        // 这条记录和唤醒后的 AT 时间线对齐，才能判断卡在哪一步。
        // Light sleep cuts P14, so the module really power-cycles: after wake it must cold
        // boot → mode 2 → relink. This line, aligned with the AT timeline, shows which step fails.
        bt_trace("SLEEP light enter wake_by=%d rails_cut=%d", (int)(wake_by != 0), (int)rails_cut);
        unsigned maintenance=0;
        for(;;) {
            // 本片浅睡内被判为电气毛刺的 EXT1 不算唤醒，继续睡，不恢复外设。
            // An EXT1 discarded as an electrical glitch does not end this slice.
            bool glitch_dropped=false;
            if(wake_by && esp_timer_get_time()>=wake_by) break;
            err=metalio_light_sleep_start();
            if(err!=ESP_OK){ power_trace("lso start fail@612: err=%s",esp_err_to_name(err)); break; }
            uint32_t causes=esp_sleep_get_wakeup_causes();
            key_irq=(causes & (1u<<ESP_SLEEP_WAKEUP_EXT1))!=0;
            // 恢复方向和慢挂卡之前验证P17；定时/USB/共享线毛刺不能补造解锁事件。
            // Verify P17 before direction/media restoration; timers, USB and shared-line glitches cannot synthesize unlocks.
            // key_seen 记「这一按真实存在」：轻按在恢复外设时就松开，靠它才不会把这一按判丢。
            // key_seen records that the press really happened: a tap released during restoration
            // is otherwise dropped and forces a second press.
            if(key_irq){
                metalio_power_key_evidence_t evidence=metalio_power_key_evidence();
                key_seen=evidence.seen;
                key_readable=evidence.readable;
                key_stable=evidence.stable;
                power_confirmed=evidence.held;
                // EXT1 是 RTC 域硬件锁存：P17 或共享 INT 线上任何窄脉冲都会把它置位，所以
                // key_irq 本身不足以授权解锁。只有「唤醒瞬间连续多次读到 P17 为低」才算电源键；
                // 判定为毛刺时丢弃本次 EXT1 继续本片浅睡——不恢复外设、不解锁。读失败无法判定，
                // 保守放行，避免 I2C 抖动时把真实轻按判丢。
                // EXT1 is latched in the RTC domain, so any narrow pulse on P17 or the shared INT
                // line sets it — key_irq alone must never authorize an unlock. Only a press that
                // still reads low on consecutive samples counts; a clear glitch is dropped and this
                // slice keeps sleeping (no peripheral restore, no unlock). A failed read cannot
                // decide, so it falls through rather than dropping a real tap.
                if(key_readable && !(key_seen && key_stable)) {
                    power_trace("wake drop: P17 glitch seen=%u stable=%u int=%d",
                        (unsigned)key_seen,(unsigned)key_stable,read_pico_ioe_int_level());
                    key_irq=false;
                    glitch_dropped=true;
                }
            }
            if(key_irq) break;
            if(!glitch_dropped && !(causes & (1u<<ESP_SLEEP_WAKEUP_TIMER))) break;
            if(wake_by && esp_timer_get_time()>=wake_by) break;
            // 定时切片仅检查USB和共享唤醒线；保持TF/字体/LCD和电源轨关闭。
            // Timer slices check USB and the shared wake line with media/LCD/rails still suspended.
            ++maintenance;
            if(metalio_usb_blocks_sleep() || read_pico_ioe_int_level()==0) break;
            vTaskDelay(pdMS_TO_TICKS(30));
            if(read_pico_ioe_int_level()==0) break;
            err=esp_sleep_enable_timer_wakeup(lock_light_sleep_slice_us(wake_by));
            if(err!=ESP_OK) break;
        }
        ESP_LOGI(TAG,"S31 light wake: maintenance=%u ext1=%u key=%u held=%u result=%s",
            maintenance,(unsigned)key_irq,(unsigned)key_seen,(unsigned)power_confirmed,esp_err_to_name(err));
        power_trace("light wake: ext1=%u key_seen=%u held=%u result=%s",
            (unsigned)key_irq,(unsigned)key_seen,(unsigned)power_confirmed,esp_err_to_name(err));
        bt_trace("SLEEP light wake ext1=%u key=%u result=%s", (unsigned)key_irq,
                 (unsigned)key_seen, esp_err_to_name(err));
    }
    if(err==ESP_OK && off_at_deadline && wake_by && esp_timer_get_time()>=wake_by &&
       sd_closed && rails_cut && pins_prepared && keys_ready && !key_irq && !metalio_usb_blocks_sleep()) {
        power_confirmed=metalio_confirm_power_key();
        if(power_confirmed)key_irq=key_seen=true;
        else {
            err=lock_deep_parked();
            // 深睡未成功才恢复外设，仍保持锁屏；交接期间的新按键继续保留。
            // Only a failed drop restores peripherals, retaining the lock and new physical input.
            power_confirmed=metalio_confirm_power_key();
            if(power_confirmed)key_irq=key_seen=true;
        }
    }
    if(pins_prepared) metalio_release_light_sleep_pins();
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    if(rails_cut) {
        esp_err_t restore=metalio_sleep_restore_rails(saved_rails);
        if(err==ESP_OK) err=restore;
        metalio_frontlight(0,50);
    }
    if(keys_ready) {
        lock_restore_key_wake();
        // 只看「这一按真实存在」（key_seen）而不是「还按着」：补记函数自身要求键已松开，
        // 而 confirm_power_key 要求键仍按住——两者互斥时，轻按就必然被丢掉。
        // Gate on key_seen ("a press happened") rather than "still held": the latch itself
        // requires the key to be released while confirm_power_key requires it to be held, so
        // with mutually exclusive conditions every quick tap was dropped.
        // 恢复期间即已松开的真实按键才补记；共享IRQ本身不提供解锁授权。
        // Retain only a real press released during restoration; a shared IRQ alone cannot authorize unlock.
        if(key_seen) {
            // 轻按在这里被救回来：日志能直接看到「按得轻」和「按得久」的差别。
            // A quick tap is rescued here; the log shows the difference between a tap and a hold.
            if(!power_confirmed) ESP_LOGI(TAG,"S31 key wake latched: tap released during restore");
            read_pico_pmu_latch_power_wakeup();
        }
    }
    const int64_t restore_at=esp_timer_get_time();
    if(sd_closed) (void)read_pico_sd_remount();
    if(lcd_stopped) read_pico_epd_restore_lcd();
    if(touch_asleep) {
        // 电源轨刚恢复：先给触摸芯片一点上电稳定时间，复位脉冲才落得稳。
        // Rails just came back: give the touch chip a moment to settle so the reset pulse lands.
        if(rails_cut) vTaskDelay(pdMS_TO_TICKS(20));
        esp_err_t restore=cst836u_wake(touch);
        if(restore!=ESP_OK){ vTaskDelay(pdMS_TO_TICKS(50)); restore=cst836u_wake(touch); }
        // 唤醒失败要明确打日志：触摸停在深睡时只有屏幕会恢复，界面看着正常但完全点不动。
        // Log a failed touch wake loudly: a chip left asleep restores the panel only, so the UI
        // looks fine yet nothing responds.
        if(restore!=ESP_OK) {
            ESP_LOGW(TAG,"S31 touch wake failed: %s; force normal mode",esp_err_to_name(restore));
            // 兜底：把 mode 拉回 normal，至少让 feed 继续采样（I2C 一恢复就自愈），
            // 不能把触摸永久留在深睡里。
            // Fallback: force normal so feed keeps sampling and self-heals once I2C returns;
            // never leave touch parked in deep sleep forever.
            (void)cst836u_set_mode(touch,CST836U_MODE_NORMAL);
        }
        power_trace("touch wake: result=%s",esp_err_to_name(restore));
        if(err==ESP_OK) err=restore;
    }
    metalio_haptic_resume();
    // 恢复输入先于TF；正常卡短暂等待，慢卡由已有主循环重试，不阻塞解锁十五秒。
    // Restore input before TF; briefly allow normal media, then let the main loop retry slow media.
    esp_err_t restored_io=lock_resume_media_io(500);
    if(restored_io!=ESP_OK)
        ESP_LOGW(TAG,"S31 wake media pending: %s",esp_err_to_name(restored_io));
    ESP_LOGI(TAG,"S31 wake peripherals_ms=%lld media=%s",
        (long long)((esp_timer_get_time()-restore_at)/1000),esp_err_to_name(restored_io));
    // GPIO仅唤醒CPU，双击才解锁；未完成时回到保留待机收齐按键。/ GPIO wakes only the CPU; retain standby until the sampled double click completes.
    if(key_irq){
        // 锁屏浅睡只把 P17(电源键)配成唯一 EXT1 唤醒源，key_irq 为真即确证电源键唤醒；
        // 不再依赖 PMU 事件 FIFO 的二次确认——轻按快速松开时 FIFO 无记录，会把这一按吞掉，
        // 表现为「壁纸 + 再按一次」。以硬件唤醒源为准直接解锁。
        // Light-sleep wires P17 (power key) as the sole EXT1 wake source, so a key_irq is
        // conclusive; skip the PMU FIFO recheck that drops quick taps (wallpaper + second press).
        if(app_settings_wake_presses()==1){
            *key_wake=true;
            (void)lock_take_key_wakeup(); // 消费残留 FIFO 事件，避免干扰后续轮询
        } else if(lock_take_key_wakeup()){
            *key_wake=true;
        } else if(err==ESP_OK){
            err=ESP_ERR_INVALID_STATE;
        }
    }
    // 恢复失败仍保持锁屏；仅真正的电源键可交给PIN/解锁逻辑。
    // Restore failure retains the lock; only a real power wake reaches PIN/unlock handling.
    ESP_LOGI(TAG,"S31 light restore: result=%s power_wake=%u",esp_err_to_name(err),*key_wake);
    return err;
}

// 原来的 lock_power_off()（IO11 硬切电源）已删除：真断电后的开机门限在硬件侧，
// 固件无法缩短，APP_SLEEP_OFF 现在折成深睡走同一套停放流程。
// The old lock_power_off() (IO11 hard cut) is gone: the boot threshold after a real
// power cut lives in hardware, so APP_SLEEP_OFF now folds into the deep-sleep path.

esp_err_t app_enter_host_sleep(app_sleep_mode_t mode) {
    if (!(book_cover_jobs_quiesce(2000) && ui_avatar_quiesce(2000))) return ESP_ERR_NOT_FINISHED;
    if (mode != APP_SLEEP_OFF && mode != APP_SLEEP_DEEP) return ESP_ERR_INVALID_ARG;
    // 本板没有 PMU：「关机」原来是让扩展口 IO11 硬切电源，之后的开机要按住电源键
    // 让硬件电路上电（门限约 1 s）。那一段发生在 ESP32 拿到电之前，任何固件都改不到
    // ——改 bootloader 采样、改按键判定都无用，这就是「关机后必须长按」的原因。
    // 用户要的是「不管锁屏、休眠还是关机，按一次电源键就进系统」，所以把 OFF 折成深睡：
    // 屏、电源轨、SD 的停放动作和原来一模一样，观感就是关机，但按键唤醒仍归本芯片管，
    // 短按一次即可唤醒并进系统。
    // This board has no PMU: "off" hard-cut power through expander IO11, and booting
    // afterwards needs a ~1 s hold handled by the power circuit before the ESP32 is even
    // powered — no firmware change can shorten that. OFF folds into deep sleep instead:
    // display, rails and SD are parked exactly as before, so it still looks like a
    // shutdown, but the key wake stays with this chip and one tap gets back in.
    const bool off_folded = mode == APP_SLEEP_OFF;
    if (off_folded) {
        ESP_LOGI(TAG, "S31 off folded into deep sleep: a real power cut needs a ~1 s hold");
        mode = APP_SLEEP_DEEP;
    }
    // 折进来的「关机」不因 USB 在位而失败：插着线关机本来就是用户明确的动作。
    // A folded "off" must not fail just because USB is present; the user asked for it.
    if (mode == APP_SLEEP_DEEP && !off_folded && metalio_usb_blocks_sleep()) return ESP_ERR_INVALID_STATE;
    if (pico_online_busy() || !read_pico_transfer_pause_for_sleep()) return ESP_ERR_NOT_FINISHED;
    // 锁屏调用方已经持有无线暂停；失败时不能擅自恢复其会话。
    // An active lock owns the radio pause; failed deep admission must not release that ownership.
    bool restore_radios=s_lock_sleep_touch==NULL;
    esp_err_t err = ble_pt_suspend_for_sleep();
    if (err != ESP_OK) { if(restore_radios) read_pico_transfer_resume_after_sleep(); return err; }
    music_set_suspended_for_sleep();  // 翻页器已拆，音乐再收栈（引用计数仍归音乐持有）/ turner down first; music then tears the stack down
    if (mode == APP_SLEEP_DEEP && !lock_prepare_key_wake()) {
        if(restore_radios) { music_set_suspended(false); ble_pt_resume_after_sleep(); read_pico_transfer_resume_after_sleep(); }
        return ESP_ERR_INVALID_STATE;
    }
    (void)app_sync_time_to_pmu();
    app_lock_font_t saved_font = {0};
    app_font_begin_lock(&saved_font);
    (void)ttf_font_open_builtin();
    if(mode==APP_SLEEP_DEEP) {
        s_lock_io_pending=true;
        err=ttf_font_suspend_io();
        if(err!=ESP_OK) power_trace("deep prep: suspend_io err=%d",err);
    }
    if(err==ESP_OK) err = read_pico_sd_sync();
    if (err != ESP_OK && mode == APP_SLEEP_DEEP) power_trace("deep prep: sd_sync err=%d",err);
    if (err != ESP_OK) {
        if (mode == APP_SLEEP_DEEP) {
            lock_restore_key_wake();
            (void)lock_resume_media_io(15000);
        }
        app_font_end_lock(&saved_font);
        if(restore_radios) { music_set_suspended(false); ble_pt_resume_after_sleep(); read_pico_transfer_resume_after_sleep(); }
        return err;
    }
    metalio_frontlight(0, 50); epd_poweroff();
    if (mode == APP_SLEEP_DEEP) {
        // 复用已验证的原厂浅睡断轨/总线准备，避免仅停CPU而外设仍带电。
        // Reuse validated vendor light-sleep rails/bus preparation so peripherals are parked as well as the CPU.
        bool pins_prepared=false,rails_cut=false,touch_asleep=false;
        uint16_t saved_rails=0;
        if(s_lock_sleep_touch) {
            err=cst836u_set_mode(s_lock_sleep_touch,CST836U_MODE_DEEPSLEEP);
            touch_asleep=err==ESP_OK;
            if(err!=ESP_OK) power_trace("deep prep: touch deep err=%d",err);
        }
        epd_lcd_deinit();
        if(err==ESP_OK) { pins_prepared=true;err=metalio_prepare_light_sleep_pins(); }
        if(err!=ESP_OK) power_trace("deep prep: pins err=%d",err);
        // 折进来的「关机」插着 USB 也要走下去（用户明确按了关机）；共享中断线低仍需推迟。
        // A folded "off" proceeds even with USB attached; a low shared IRQ line still defers.
        if(err==ESP_OK && ((!off_folded && metalio_usb_blocks_sleep()) || read_pico_ioe_int_level()==0)) {
            power_trace("deep prep: gate usb=%d ioe_low=%d",
                (int)metalio_usb_blocks_sleep(),(int)(read_pico_ioe_int_level()==0));
            err=ESP_ERR_INVALID_STATE;
        }
        // 标记写 flash（NVS），必须赶在切轨与锁引脚之前；睡不成就在下面清掉。
        // The marker lives in NVS, so write it before rails/pins are parked; cleared below on failure.
        if(err==ESP_OK) lock_mark_deep_wake();
        if(err==ESP_OK) { rails_cut=true;err=metalio_sleep_cut_rails(&saved_rails); }
        if(err!=ESP_OK) power_trace("deep prep: rails err=%d",err);
        if(err==ESP_OK) {
            power_trace("deep prep: entering deep sleep");
            metalio_hold_for_deep_sleep();
            esp_deep_sleep_start();
            lock_clear_deep_wake();
        }
        if(err!=ESP_OK) lock_clear_deep_wake();
        if(pins_prepared) metalio_release_light_sleep_pins();
        if(rails_cut) (void)metalio_sleep_restore_rails(saved_rails);
        metalio_frontlight(0,50);
        lock_restore_key_wake();
        (void)read_pico_sd_remount();
        (void)lock_resume_media_io(15000);
        read_pico_epd_restore_lcd();
        if(touch_asleep) {
            esp_err_t tp=cst836u_wake(s_lock_sleep_touch);
            if(tp!=ESP_OK){ vTaskDelay(pdMS_TO_TICKS(50)); tp=cst836u_wake(s_lock_sleep_touch); }
            if(tp!=ESP_OK) {
                ESP_LOGW(TAG,"S31 deep-abort touch wake failed: %s; force normal mode",esp_err_to_name(tp));
                (void)cst836u_set_mode(s_lock_sleep_touch,CST836U_MODE_NORMAL);
            }
        }
        app_font_end_lock(&saved_font);
        metalio_haptic_resume();
        if(restore_radios) { music_set_suspended(false); ble_pt_resume_after_sleep(); read_pico_transfer_resume_after_sleep(); }
        return err;
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t app_sync_time_to_pmu(void) {
    time_t now = time(NULL);
    if (now < 946684800 || now > 4102444799U) {
        ESP_LOGW(TAG, "skip PMU time sync: system clock invalid");
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t seconds = (uint32_t)now;
    uint8_t payload[4] = {
        (uint8_t)(seconds & 0xff),
        (uint8_t)((seconds >> 8) & 0xff),
        (uint8_t)((seconds >> 16) & 0xff),
        (uint8_t)((seconds >> 24) & 0xff),
    };
    esp_err_t err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
    ESP_LOGI(TAG, "PMU time sync before power action: %s", esp_err_to_name(err));
    return err;
}

void app_restart_host(void) {
    (void)app_sync_time_to_pmu();
    ttf_font_open_builtin(); (void)read_pico_sd_sync();
    metalio_haptic_stop(); metalio_frontlight(0,50); epd_poweroff(); esp_restart();
}

static bool s_download_active;
static void download_trace(const char *stage,bool first) {
    char state[256];metalio_usb_describe(state,sizeof(state));
    ESP_LOGI(TAG,"USB download %s: %s",stage,state);
    // 只记录本次准备过程，覆盖旧记录，不在滑动/常规轮询中写卡。
    // Keep one bounded preparation trace; no writes during normal polling or dragging.
    // 仅 debug 档写卡；release 档不定义 METALIO_DIAG_SD_LOGS，跳过文件输出。
    // SD write only in the debug profile; release skips the file output.
#ifdef METALIO_DIAG_SD_LOGS
    (void)mkdir("/sdcard/.metalio_s31",0777);
    FILE *log=fopen("/sdcard/.metalio_s31/usb-download.log",first?"w":"a");
    if (log) {
        fprintf(log,"%s %s %s\n",esp_app_get_description()->version,stage,state);
        (void)fclose(log);
    }
#endif
}
bool app_download_active(void) { return s_download_active; }
void app_cancel_download(void) {
    if (!s_download_active) return;
    metalio_download_cancel();
    s_download_active=false;
    music_set_suspended(false); ble_pt_resume_after_sleep();
    read_pico_transfer_resume_after_sleep();
}
esp_err_t app_enter_download(void) {
    if (usb_storage_active()) return ESP_ERR_INVALID_STATE;
    if (s_download_active) {
        download_trace("before_recover",true);
        esp_err_t retry=metalio_request_download();
        download_trace(retry==ESP_OK?"waiting_host":"recovery_failed",false);
        if (retry!=ESP_OK) app_cancel_download();
        return retry;
    }
    if (!(book_cover_jobs_quiesce(2000) && ui_avatar_quiesce(2000))) return ESP_ERR_NOT_FINISHED;
    if (!read_pico_transfer_pause_for_sleep()) return ESP_ERR_NOT_FINISHED;
    esp_err_t err=ble_pt_suspend_for_sleep();
    if (err!=ESP_OK) { read_pico_transfer_resume_after_sleep(); return err; }
    music_set_suspended(true);
    download_trace("before_prepare",true);
    err=metalio_download_prepare();
    if (err!=ESP_OK) {
        music_set_suspended(false); ble_pt_resume_after_sleep(); read_pico_transfer_resume_after_sleep(); return err;
    }
    (void)app_sync_time_to_pmu();
    // 保持TF/字体/LCD和调度器，由电脑使用官方USJ控制线进入ROM；等待时仍可返回退出。
    // Retain TF/fonts/LCD and scheduling; the host enters ROM via official USJ control lines. Cancel stays usable.
    err=metalio_request_download();
    if (err!=ESP_OK) {
        download_trace("recovery_failed",false);
        metalio_download_cancel();
        music_set_suspended(false); ble_pt_resume_after_sleep(); read_pico_transfer_resume_after_sleep();
        return err;
    }
    s_download_active=true;
    download_trace("waiting_host",false);
    return ESP_OK;
}

static void lock_wait_for_unlock(sc7a20h_handle_t acc) {
    (void)acc;
    for(;;) {
        app_wake_source_t wake=app_light_sleep_wait_timed(NULL,APP_LOCK_IDLE_TIMEOUT_MS);
        if(wake==APP_WAKE_KEY || wake==APP_WAKE_PICKUP) return;
        // 超时不能触发重启或解锁；稳定性版继续保留当前锁屏内存。
        // A timeout cannot reboot or unlock; retain the current lock context in this stability build.
    }
}

bool lock_pin_grace_active(void) {
    if(!lock_pin_enabled() || !lock_pin_available() || !app_settings_pin_grace_minutes())return false;
    const time_t now=time(NULL);
    const uint32_t locked_at=app_settings_pin_locked_at();
    if(now<1704067200 || now>4102444799LL || locked_at<1704067200 || (uint64_t)now<locked_at)return false;
    return (uint64_t)now-locked_at<(uint64_t)app_settings_pin_grace_minutes()*60u;
}

void enter_lock_and_sleep(
    EpdiyHighlevelState* hl, int64_t* ignore_until_ms, sc7a20h_handle_t acc, cst836u_handle_t touch,
    bool reader_background
) {
    const int64_t lock_at = esp_timer_get_time();
    // 先灭灯并消费本次锁屏动作；后续生成/推屏期间的新短按必须保留给唤醒。
    // Turn the lamp off and consume the initiating action before composition; retain later wake presses.
    metalio_frontlight(0, 50);
    if (!s_lock_feedback_started) read_pico_pmu_drain_events();
    s_lock_feedback_started = false;
    // Refuse to interrupt an active upload. The caller repaints its page when
    // this function returns, and the user can lock again after the transfer.
    bool radio_paused = read_pico_transfer_pause_for_sleep();
    const int64_t transfer_at = esp_timer_get_time();
    if (!radio_paused) {
        metalio_frontlight_restore();
        pico_boot_clear_resume();
        ESP_LOGW(TAG, "lock postponed while transfer is busy");
        return;
    }
    // 「浅睡播放音乐」开着且此刻在放，就整段不碰蓝牙：翻页器与音乐都不挂起，
    // 锁屏后只关屏关灯，音频链路保持原样；解锁时也不需要恢复。
    // With "music through light sleep" on and audio playing, the whole lock leaves Bluetooth
    // alone: only the panel and the lamp go dark, and nothing has to be restored on unlock.
    const bool music_awake = app_settings_music_in_sleep() && music_state() == MUSIC_PLAYING;
    bt_trace("LOCK enter music_awake=%d state=%d", (int)music_awake, (int)music_state());
    if (!music_awake && ble_pt_suspend_for_sleep()!=ESP_OK) {
        read_pico_transfer_resume_after_sleep();
        metalio_frontlight_restore();
        pico_boot_clear_resume();
        ESP_LOGW(TAG,"S31 lock postponed: BLE shutdown failed"); return;
    }
    if (!music_awake) music_set_suspended_for_sleep();
    const int64_t radio_at = esp_timer_get_time();
    uint8_t* framebuffer = epd_hl_get_framebuffer(hl);
    // 锁屏恒为浅色输出：从夜间正文进入也先退极性；锁屏各推送都是全表面，退极性后自洽。
    // The lock screen always outputs light: drop night polarity even when locking from a night body page.
    epd_set_inverted(false);
    // 仅清目标内存，旧页面在拼贴准备期间仍留在屏幕；不提前清白物理面板。
    // Clear only target memory; retain the old physical page until the complete collage is ready.
    epd_poweroff();
    uint8_t lock_style = app_settings_lock_style();
    // 壁纸先在内存准备完整画面，再从真实back一次全像素GC48；不提前推白，也不伪造白底历史。
    // 票根/拼贴保留既有擦白；透明PNG仍先从最后成功帧合成，再由图片出口清理物理旧态。
    // Prepare wallpaper in memory and present once with full-pixel GC48 from actual back history.
    // Retain ticket/collage whitening and transparent-PNG composition/optical cleanup.
    if (lock_style == 0 || lock_style == 2) guard_draw_result(hl, update_display_white(hl));
    s_lock_frame_cached=false;
    // 透明书封以最后成功推屏的真实参考作为底图，不取未显示的目标。
    // Overlay the last successfully presented reference, never an uncommitted target.
    if(lock_style==3)memcpy(framebuffer,hl->back_fb,(size_t)epd_width()*epd_height()/2);
    else epd_hl_set_all_white(hl);
    bool ticket = lock_style == 0;
    bool system_lock = ticket || lock_style == 2 || lock_pin_enabled();
    app_lock_font_t lock_font = {0};
    if (system_lock) app_font_begin_lock(&lock_font);
    // 阅读票根独占锁屏画布：始终先画最近书籍封面，再叠票根；自定义壁纸只属于壁纸模式。
    // Ticket mode owns the lock canvas: book cover first, ticket on top. Custom wallpaper is wallpaper-only.
    bool lock_drawn = ticket
        ? book_ticket_draw(framebuffer, reader_background)
        : lock_style == 2 ? book_lock_collage_prepare(framebuffer) :
          lock_style == 3 ? book_lock_bookmark_draw(framebuffer,app_settings_bookmark_path()) : draw_wallpaper(framebuffer);
    if (!lock_drawn) {
        ui_lock_fallback_draw(framebuffer);
    }
    const int64_t draw_at = esp_timer_get_time();
    enum EpdDrawError result = lock_present_art(hl,lock_style);
    if (result != EPD_DRAW_SUCCESS) {
        guard_draw_result(hl, result);
        result = lock_present_art(hl,lock_style);
    }
    const int64_t presented_at = esp_timer_get_time();
    ESP_LOGI(TAG, "S31 lock style=%u GC48 result=%d transfer_ms=%lld ble_ms=%lld prepare_ms=%lld push_ms=%lld visible_ms=%lld", lock_style,
        (int)result, (long long)((transfer_at - lock_at) / 1000),
        (long long)((radio_at - transfer_at) / 1000), (long long)((draw_at - radio_at) / 1000),
        (long long)((presented_at - draw_at) / 1000), (long long)((presented_at - lock_at) / 1000));
    if (result != EPD_DRAW_SUCCESS && !lock_pin_enabled()) {
        // 推屏失败不宣布锁屏完成；恢复本页与灯光，允许再次尝试。
        // A failed presentation does not enter sleep; restore the page and lamp for another attempt.
        epd_poweroff();
        if (system_lock) app_font_end_lock(&lock_font);
        read_pico_transfer_resume_after_sleep();
        if (!music_awake) { music_set_suspended(false); ble_pt_resume_after_sleep(); }
        metalio_frontlight_restore();
        pico_boot_clear_resume();
        return;
    }
    // 中文：只缓存成功显示的完整锁屏，必须在密码层覆盖前提交。
    // English: Commit only successfully presented lock art, before the PIN overlay.
    if (lock_drawn && result == EPD_DRAW_SUCCESS) {
        if (lock_style == 2) book_lock_collage_cache_commit(framebuffer);
        if (lock_style == 1) ui_wallpaper_cache_commit(framebuffer);
        s_lock_frame_cached=lock_screen_save(framebuffer);
    }
    // 密码启用时即使显示失败也保持验证边界，绝不回到未验证页面。
    // A display failure must never return an unauthenticated page when the PIN is enabled.
    // 锁屏按所选策略定时或直接硬件关机；始终保留锁图、密码和阅读检查点。
    // Apply the selected timed/immediate hardware-off policy, retaining artwork, PIN and reader checkpoints.
    ESP_LOGI(TAG, "lock LIGHT");
    epd_poweroff();
    s_lock_sleep_touch=touch;
    lock_hint_begin(hl);
    if(lock_pin_enabled()) {
        time_t now=time(NULL);
        app_settings_record_pin_lock(now>=1704067200 && now<=4102444799LL?(uint32_t)now:0);
        lock_screen_authenticate(hl,touch,acc,false);
    }
    else lock_wait_for_unlock(acc);
    lock_hint_end();
    s_lock_sleep_touch=NULL;
    s_lock_deep_ready=false;s_lock_frame_cached=false;
    pico_boot_clear_resume();
    (void)lock_resume_media_io(0);
    if (system_lock) app_font_end_lock(&lock_font);
    read_pico_transfer_resume_after_sleep();
    bt_trace("UNLOCK resume music_awake=%d state=%d", (int)music_awake, (int)music_state());
    // 真人解锁：先解除锁屏时置下的「不自动续播」，再解除挂起。顺序反了就会漏掉这一次唤醒——
    // eval_auto 只看 s_suspend_no_resume，晚清一秒它就已经跳过续播、之后要等用户点播放。
    // A genuine unlock first drops the "no auto-resume" latch, then the suspend. Reversed, this
    // wake would be missed: eval_auto checks the latch and would skip the resume for good.
    if (!music_awake) { music_allow_resume_after_unlock(); music_set_suspended(false); ble_pt_resume_after_sleep(); }

    // 仅清目标画布，保留面板和真实参考帧；当前页接着单次重绘，无额外白屏。
    // Clear only the target canvas, retaining panel/reference history for the next page presentation.
    read_pico_pmu_drain_events();
    epd_hl_set_all_white(hl);
    read_pico_pmu_drain_events();
    if (ignore_until_ms) {
        *ignore_until_ms = esp_timer_get_time() / 1000 + APP_LOCK_IGNORE_BOOT_MS;
    }
    ESP_LOGI(TAG, "unlocked");
}

void app_lock_boot_gate(EpdiyHighlevelState *hl, cst836u_handle_t touch, sc7a20h_handle_t acc) {
    lock_hint_begin(hl);
    s_boot_lock_draw=true;
    lock_resume_service();
    if (!lock_pin_enabled()) {
        if(pico_boot_has_resume() || pico_boot_lock_pending()) {
            pico_boot_ready();pico_boot_hold_resume();
            // 单击硬件开机直接恢复；其他复位保持门禁。
            // Single-press hardware boots restore directly; other resets retain the gate.
            // 深睡唤醒要单独看：bootloader 加载 5~6 MB 镜像要几百 ms，用户的手指早就离开，
            // 唤醒后采样 P17 几乎必然读到高。但 EXT1 唤醒本身就证明按过一次——休眠期间
            // 扩展口只有 P17 是输入（metalio_prepare_key_wake 的 0x80 掩码），触摸/传感器/
            // 前光都已休眠，共享 GPIO2 上不会有别的东西拉低。
            // Deep wake needs its own rule: loading the 5~6 MB image takes hundreds of ms, so the
            // finger is long gone and the post-wake P17 sample almost always reads high. The EXT1
            // wake itself proves one press — only P17 is an input during sleep (the 0x80 mask in
            // metalio_prepare_key_wake) and touch/sensor/frontlight are all parked.
            const bool boot_key = metalio_boot_power_key();
            // 深睡只配了 P17 一条 EXT1 唤醒线，能走到这里且是 DEEPSLEEP 复位，
            // 就必然是按键唤醒（TIMER 已在 lock_deep_parked 里 disable）。
            // 加 s_boot_from_deep 作保底，避免某些芯片 ext1 位图读回异常时把这一按判丢，
            // 从而落到锁屏「壁纸 + 再按」。这是 f07d424 注释规划的证据放宽。
            // Deep sleep arms only the P17 EXT1 line; reaching here on a DEEPSLEEP reset
            // proves a key wake. OR s_boot_from_deep as a fallback so an odd ext1 bitmap
            // readback cannot drop this press into the wallpaper + second-tap path.
            const bool key_seen = boot_key || s_boot_power_wake || s_boot_from_deep;
            power_trace("boot_gate: reset=%u nvs=%u pin=%u wake_presses=%u boot_key=%u deep=%u power_on=%u power_wake=%u key_seen=%u -> %s",
                (unsigned)s_boot_reset_code, (unsigned)s_boot_nvs_mark,
                (unsigned)lock_pin_enabled(), (unsigned)app_settings_wake_presses(),
                (unsigned)boot_key, (unsigned)s_boot_from_deep, (unsigned)s_boot_power_on,
                (unsigned)s_boot_power_wake, (unsigned)key_seen, key_seen?"OPEN":"LOCK");
            // 只要这一次按键有证据就直接进系统，不再额外要求「必须是断电启动」：
            //   断电启动 → bootloader/P17 早期采样，或「无 USB 上电」兜底（metalio_hw）；
            //   深睡唤醒 → EXT1 唤醒源（深睡只配了这一条线）。
            // 两种证据都没有，才说明不是用户按的（软件复位、插线、异常重启），此时才保留门禁。
            // 旧条件里的 !s_boot_power_on 是一刀切：DEEPSLEEP 复位时它恒为真，于是每一次
            // 深睡唤醒都被挡在壁纸前，正是用户看到的「按一下没反应、再按一次才行」。
            // A proven press enters the system directly, no matter which reset path brought us here:
            // a cold boot has the bootloader/P17 sample (or the "no USB" fallback), a deep wake has
            // its EXT1 source. Only resets with neither evidence keep the gate. The old
            // "!s_boot_power_on" clause was true for every DEEPSLEEP reset, so each deep wake
            // parked on the wallpaper — exactly the second-press symptom.
            if (!key_seen) {
                lock_wait_for_unlock(NULL);
            } else {
                // 直接放行的这一按不补记：等待循环不跑，多记一次短按会变成进系统后的多余按键。
                // A pass-through press is not latched: the wait loop is skipped, and an extra
                // recorded short press would surface as a stray key event after unlock.
                ESP_LOGI(TAG, "S31 boot gate open: power_on=%u deep=%u boot_key=%u ext1=%u",
                    (unsigned)s_boot_power_on, (unsigned)s_boot_from_deep,
                    (unsigned)boot_key, (unsigned)s_boot_power_wake);
            }
        }
        lock_hint_end();
        (void)app_lock_resume_frame(NULL);
        // 异常启动不自动续读，但真实解锁后可以消费已保留的锁定标记。
        // Abnormal startup cannot resume books, but verified unlock can consume the retained lock marker.
        if(pico_boot_lock_pending() && !pico_boot_has_resume())pico_boot_clear_resume();
        return;
    }
    // 卡挂载有界等待仅为了字体/锁屏图；没有卡仍必须验证。
    // Bound mount waiting for the face/art only; absent media still requires authentication.
    read_pico_sd_info_t info;
    for (unsigned i = 0; i < 150 && read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    app_lock_font_t saved; app_font_begin_lock(&saved);
    uint8_t *fb = epd_hl_get_framebuffer(hl);
    if (!lock_screen_restore(fb)) {
        if(app_settings_lock_style()==3)memset(fb,255,(size_t)epd_width()*epd_height()/2);
        bool drawn = app_settings_lock_style() == 0 ? book_ticket_draw(fb, false) :
            app_settings_lock_style() == 2 ? book_lock_collage_prepare(fb) :
            app_settings_lock_style() == 3 ? book_lock_bookmark_draw(fb,app_settings_bookmark_path()) : draw_wallpaper(fb);
        if (!drawn) ui_lock_fallback_draw(fb);
        enum EpdDrawError result=lock_present_art(hl,app_settings_lock_style());
        guard_draw_result(hl,result);epd_poweroff();
        if(drawn && result==EPD_DRAW_SUCCESS)(void)lock_screen_save(fb);
    }
    lock_screen_authenticate(hl, touch, acc, true);
    lock_hint_end();
    (void)app_lock_resume_frame(NULL);
    app_font_end_lock(&saved);
    if(pico_boot_lock_pending() && !pico_boot_has_resume())pico_boot_clear_resume();
}
