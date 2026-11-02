/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 用户修订：硬件前建立持久化启动保护；深睡恢复入口先消费再开书，中断图书进入可操作恢复页。
 * User revision: persist startup protection before hardware; consume deep-sleep checkpoints before opening books, and show an operable recovery page after interrupted opening.
 * 只做开机装配：拉起板级硬件、读设置、定 VCOM、放开机图、开字体；
 * 未标定则先拦住进设定页。然后把控制权交给 app_loop。
 *
 * Boot wiring only: board init, settings, PMU RTC restore, VCOM, splash, fonts. If VCOM
 * is unset, the factory page blocks first. Then control goes to app_loop.
 */

#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "app_loop.h"
#include "setup_state.h"
#include "app_auto_test.h"
#include "bt_trace.h"
#include "mem_trace.h"
#include "app_content_open.h"
#include "boot_state.h"
#include "lock_screen.h"
#include "ui_lock_feedback.h"
#include "lock_pin.h"
#include "ttf_font.h"
#include "book_store.h"
#include "esp_system.h"
#include "read_pico_sd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/stat.h>
#include "app_registry.h"
#include "display.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "esp_log.h"
#include "ota_update.h"
#include "pmu_selftest.h"
#include "metalio_hw.h"
#include "output_lcd/lcd_driver.h"
#include "read_pico_board.h"
#include "read_pico_crashlog.h"
#include "read_pico_init.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "settings.h"
#include "sleep.h"
#include "ttf_font.h"
#include "usb_storage.h"
#include "app_font_context.h"
#include "ui_kit.h"
#include "ui_font.h"
#include "vcom_setup.h"
#include "debug_panel.h"
#include "metalio_sleep.h"

extern const app_desc_t app_onboarding;

static const char* TAG = "read_pico";
extern const app_desc_t app_boot_recovery;

// 主机上电会丢失系统时间；PMU 在主机断电后继续走时。先取实时值再恢复，
// 不使用启动时缓存的快照，以免将已经经过的开机耗时丢掉。
static void restore_time_from_pmu(void) {
    struct timeval current;
    if (gettimeofday(&current,NULL)==0 && current.tv_sec>=1704067200 && current.tv_sec<=4102444799U) return;
    if (read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) != ESP_OK) return;
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (!pmu || !pmu->time_ok || !pmu->time_synced || pmu->unix_sec < 1704067200 || pmu->unix_sec > 4102444799U) return;
    struct timeval tv = {.tv_sec = (time_t)pmu->unix_sec, .tv_usec = 0};
    if (settimeofday(&tv, NULL) == 0) ESP_LOGI(TAG, "system clock restored from Metalio PCF8563");
}

extern const uint8_t lock_4bpp_bin_start[] asm("_binary_lock_4bpp_bin_start");
extern const uint8_t lock_4bpp_bin_end[] asm("_binary_lock_4bpp_bin_end");

// VCOM 从 NVS 读取（默认 3000，实测最优）。V1.5 没有 Pico PMU，标定值只存本机 NVS。
// Load VCOM from NVS (3000 by default, the measured optimum). V1.5 has no Pico
// PMU, so the calibrated value lives in local NVS only.
static bool resolve_vcom_at_boot(void) {
    const int mv = vcom_setup_load_mv();
    epd_set_vcom((uint16_t)mv);
    ESP_LOGI(TAG,"Metalio V1.5 runtime VCOM=%dmV (NVS, default 3000)", mv);
    return true;
}

// 开机按住音量键进 VCOM 校准页；V1.5 音量键在 FCA9555 P1.0/P1.1，低有效。
// Held volume keys (active low on FCA9555 P1.0/P1.1) open the VCOM calibration page.
static bool volume_key_held(void) {
    uint16_t first = 0xffff, again = 0xffff;
    if (metalio_io_read(&first) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(30));
    if (metalio_io_read(&again) != ESP_OK) return false;
    const bool pressed_first = ((first >> 8) & 0x03) != 0x03;
    const bool pressed_again = ((again >> 8) & 0x03) != 0x03;
    return pressed_first && pressed_again;
}

// 锁屏后备位图保留面板尺寸检查；冷启动改用现有内建字形绘制。
// Keep the panel-size check for fallback lock art; draw the cold splash using existing built-in glyphs.
static bool images_match_panel(void) {
    size_t lock_size = (size_t)(lock_4bpp_bin_end - lock_4bpp_bin_start);
    size_t expected = (size_t)epd_width() * epd_height() / 2;
    if (lock_size == expected) return true;
    ESP_LOGE(
        TAG, "Image size mismatch: lock %u expected %u",
        (unsigned)lock_size, (unsigned)expected
    );
    return false;
}

// 开机不再画品牌图：擦白之后直接进首帧，少一次整屏绘制与一次全屏刷新。
// No boot splash: after whitening, the first product frame is the first thing shown,
// saving both a full-screen paint and a full-screen refresh.
static void draw_startup_frame(uint8_t *framebuffer) {
    (void)framebuffer;
}

// 深睡保持原物理锁图；卡与参考帧稍后就绪，不能用临时圆锁覆盖。
// Preserve physical art on deep boot; pending media/reference recovery must not replace it with a badge.
static bool prepare_startup_frame(EpdiyHighlevelState *hl,uint8_t *framebuffer,bool resuming,bool vcom_ok) {
    if(resuming && vcom_ok && !lock_pin_enabled())return app_lock_resume_frame(hl);
    epd_poweron();
    rp_boot_mark("pwr");
    if(!resuming)read_pico_i2c_census_take();
    // 擦白保持同步：面板推屏依赖全局 render_context 与 LCD 中断，跨核并发会让首帧与擦白
    // 抢同一套供数队列，实机表现为开机反复闪烁。这里不做并发，稳定性优先。
    // Keep the whitening synchronous: panel pushes share the global render_context and the LCD
    // interrupt, so running it on the other core lets it race the first frame for the same line
    // queues and the panel ends up flickering on boot. Stability wins over the ~400ms saving.
    epd_clear();
    rp_boot_mark("erase");
    epd_hl_set_all_white(hl);
    if(resuming)ui_lock_fallback_draw(framebuffer);
    else draw_startup_frame(framebuffer);
    // 冷启动不在这里推屏：物理擦白已经把屏置白，首帧那一轮整屏 GC16 就是唯一的开机输出，
    // 省掉一次约 720ms 的全屏刷新；开机图仍画进 fb，保持与原先逐像素一致的前后帧语义。
    // Cold boot skips this push: the physical clear already whitened the panel, so the first
    // product frame is the single full-screen GC16, saving one ~720ms pass. The splash is
    // still drawn into the fb so the front/back frame semantics stay pixel-identical.
    // 深睡带锁唤醒仍要立刻出原图，保留原有推屏。
    // Deep wake with a PIN still needs its retained art immediately; keep that push.
    if(resuming)update_display_from_white(hl);
    else memset(hl->back_fb,0xFF,(size_t)epd_width()/2*epd_height());
    // 后缓冲直接声称「屏上已经是纯白」，与刚做完的物理擦白一致；否则首帧差分会把
    // 白底像素当成未变化而跳过，旧页残影留在白底上。
    // Claim the back buffer is pure white, matching the just-finished physical clear; otherwise the
    // first differential skips white pixels and leaves the previous page's ghost in the blank areas.
    return false;
}

// 调试档把「每笔 I/O 打一行」的几个底层 tag 提到 WARN。这些日志在正常运行时会持续刷屏
// （实测一次启动 SD_HOST 就打出 23 万行，NVS/FAT 又是每几毫秒一行），真正有用的应用日志
// 全被淹没，串口也一直在占带宽。release 档本来就是 ERROR，不受影响。
// In the debug profile, raise the chatty per-transaction tags to WARN. They stream continuously
// in normal operation (one boot produced 230k SD_HOST lines and NVS/FAT one line every few ms),
// burying the application logs and eating console bandwidth. The release profile is ERROR-only
// already, so it is unaffected.
static void log_quiet_noise(void) {
#if defined(CONFIG_LOG_DEFAULT_LEVEL) && (CONFIG_LOG_DEFAULT_LEVEL >= 4)
    static const char *const k_noisy[] = {
        "SD_HOST", "sdmmc_common", "sdmmc_sd",  // TF 卡的每笔传输 / per-transfer SD
        "vfs_fat", "vfs_calls",                 // FAT 与 VFS 的每次调用 / every FAT/VFS call
        "nvs",                                  // 每次 nvs_open / every namespace open
        "httpd_uri", "httpd_txrx", "httpd_req", // 网页每请求一行 / one line per HTTP request
        "epd_lcd",                              // 每次推屏 48 行相位 / 48 phase lines per push
    };
    for (size_t i = 0; i < sizeof(k_noisy) / sizeof(k_noisy[0]); ++i)
        esp_log_level_set(k_noisy[i], ESP_LOG_WARN);
#endif
}

void app_main(void) {
    log_quiet_noise();
    // 第一个标记，用来把 bootloader 与本文件的装配耗时分开统计。
    // First mark: separates bootloader time from the board bring-up below.
    rp_boot_mark("main");
    metalio_capture_boot_power_key();
    app_lock_capture_boot_wakeup();
    ESP_ERROR_CHECK(metalio_sleep_sdk_init());
    // 共享 TF 卡期间复位后，先把内部 PHY 交回串口，再清理已用完的 BOOT 请求。
    // Reclaim Serial/JTAG after a reset during card sharing, then clear the served BOOT request.
    usb_storage_phy_init();
    metalio_clear_download_request();

    const esp_reset_reason_t reset = esp_reset_reason();
    ESP_LOGI(TAG, "startup reset=%d stack=%u", (int)reset, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    app_settings_init();
    rp_boot_mark("set");
    // 蓝牙/睡眠诊断先挂上：之后的每次 AT 收发、模式切换、挂起/唤醒都落进
    // /sdcard/.metalio_s31/bt_trace.log。卡还没挂好时记录留在内存里，挂好再写。
    // Arm Bluetooth/sleep diagnostics early: every AT exchange, mode switch and suspend/wake
    // lands in /sdcard/.metalio_s31/bt_trace.log; until the card mounts the lines wait in RAM.
    bt_trace_start();
    // 定时内存水位：debug 档每 3 秒把 SRAM/PSRAM 用量写 /sdcard/mem_trace.csv，
    // 用于定位长时间阅读的内存泄漏与内部 SRAM 被啃薄的阶段。
    // Periodic memory watermark: in debug builds, SRAM/PSRAM usage lands in
    // /sdcard/mem_trace.csv every 3 s to pin down leaks and the phase that thins internal SRAM.
    mem_trace_start();
    // 自测构建在 SD 挂载前保留 USJ，避免挂载后的软重连把串口日志带走。
    // Self-test builds retain USJ before the SD mount so the post-mount soft reconnect cannot drop the console.
    app_auto_test_prepare();
    // 大缓冲/字体之前读取凭据；失败不缓存，后续入口仍可重试。/ Read credentials before large buffers/fonts; retry failed reads later.
    (void)lock_pin_enabled();
    // 实测（bt_trace.log / font_diag.log，2026-11-01）：本板深睡唤醒报 reset=7（ESP_RST_WDT），
    // 既不是 DEEPSLEEP 也不是 PWR_GLITCH。若照旧把 WDT 一律判成异常启动，深睡回来整个会话
    // 都挂 recovery：系统字体锁在内建，阅读里手动选的 TTF 会被 750ms 周期的字体重试打回
    // 内建，只有软重启（reset=ESP_RST_SW）才恢复。深睡进入标记（NVS s31wake/deep）此刻
    // 仍未消费本身就证明这一次是按键唤醒；真正的看门狗复位不会留下未消费的标记，仍按原样
    // 进异常保护。运行期不改变内存占用与打开顺序，只影响 recovery 的判定入口。
    // Measured on device: a deep wake reports reset=7 (ESP_RST_WDT), not DEEPSLEEP/PWR_GLITCH.
    // Treating every WDT as abnormal pinned recovery for the whole session — the system face
    // stayed built-in and each periodic 750 ms font retry undid the reader's manual TTF pick
    // until a soft reboot. An unconsumed deep-entry marker proves this boot was a key wake, so
    // that WDT code is not abnormal; a real watchdog reset never leaves the marker behind.
    const bool deep_wake_mark = metalio_deep_wake_mark_pending();
    const bool reset_abnormal = reset == ESP_RST_PANIC || reset == ESP_RST_INT_WDT ||
                                reset == ESP_RST_TASK_WDT || reset == ESP_RST_WDT;
    if (reset_abnormal && deep_wake_mark)
        ESP_LOGI(TAG, "reset=%d excused by pending deep-wake mark", (int)reset);
    pico_boot_init(reset_abnormal && !deep_wake_mark);
    rp_boot_mark("boot");

    read_pico_handle_t hw;
    if (read_pico_init(&hw) != ESP_OK) return;
    rp_boot_mark("hw");
    if (hw.pmu_ready) restore_time_from_pmu();
    const bool vcom_ok = resolve_vcom_at_boot();
    rp_boot_mark("vcom");
    ESP_LOGI(TAG,"TPS65185 last VCOM register readback=%dmV",metalio_tps_vcom_mv());
    pmu_selftest_bind(hw.sensor);

    // 升压轨在 read_pico_init 校验后保持上电，异常退出时补一次断电。
    // The rails stay up after the init check; drop them on this abnormal exit.
    if (!images_match_panel()) { epd_poweroff(); return; }

    EpdiyHighlevelState hl = hw.hl;
    uint8_t* framebuffer = hw.framebuffer;

    // 深睡保留屏幕已有画面：验证缓存后同时恢复前后帧，省去清白和开机图两次推屏。
    // Deep sleep retains the panel image: validate its cache and restore both references, skipping white erase and splash output.
    const bool resuming = pico_boot_has_resume() && reset == ESP_RST_DEEPSLEEP;
    // 双击门禁始终显示锁图，不能在“准备资源”页暗中等按键。
    // A double-press gate retains lock art rather than waiting behind the resource splash.
    // 深睡只把电源键(P17)留成唤醒源，所以「带回深睡标记」本身就证明这一次是按出来的：
    // 不该先画壁纸等人按第二下，直接按电源菜单关机那条路走（开机画面 → 首页）。
    // Deep sleep arms only the power key, so a surviving marker proves this boot was a key wake;
    // such a boot must not park on the wallpaper waiting for a second press.
    // 本板深睡唤醒报 ESP_RST_PWR_GLITCH，RTC 域保不住标记；NVS 已在 app_settings_init()
    // 里就绪，这里补判定，让「唤醒那一按」在门禁与开机画面决策上都算数。
    // This board wakes from deep sleep as ESP_RST_PWR_GLITCH with the RTC domain lost; NVS is
    // ready after app_settings_init(), so resolve the marker before the wallpaper decision.
    app_lock_resolve_wake_mark();
    const bool deep_key_wake = app_lock_deep_wake_key();
    // rc108：长按开机的硬件上电直接进应用，不先停在锁图等短按；其他复位仍保留门禁。
    // 本地差异：另外多豁免一条「深睡标记命中」（deep_key_wake），其余条件与上游一致，
    // 上游这一版删掉的 app_settings_wake_presses()==2 例外本地同步删除。
    // rc108: a confirmed long-press hardware boot goes straight to the app instead of stopping on
    // the lock art; other resets keep the gate. Local adds the deep-wake exemption on top and
    // adopts upstream's removal of the wake_presses()==2 escape.
    const bool retain_lock = !deep_key_wake &&
        (pico_boot_has_resume() || pico_boot_lock_pending()) &&
        (reset != ESP_RST_POWERON || !metalio_boot_power_key());
    (void)prepare_startup_frame(&hl,framebuffer,retain_lock,vcom_ok);
    rp_boot_mark("splash");
    // 触摸识别挪到这里：复位脉冲在 read_pico_init 已发出，擦白这段时间足够芯片自举。
    // Identify the touch chip here: its reset pulse went out in read_pico_init and the whitening
    // gave it far more than the old fixed 300ms to come up.
    (void)read_pico_touch_init(&hw);
    rp_boot_mark("touch");
    // 直接续读不先加载整份系统字体；密码验证仍先使用所选系统字体。
    // Direct reader resume skips loading the whole system face first; PIN authentication still uses the selected UI face.
    if(!resuming || lock_pin_enabled())app_font_activate_system();
    else { app_font_diag_note("boot-resume-builtin", "-"); (void)ttf_font_open_builtin(); }
    rp_boot_mark("font");

    if (!hw.touch_ready) {
        ESP_LOGE(TAG, "No touch controller, UI cannot run");
        epd_poweroff();
        return;
    }

    // V1.5 没有 Pico PMU：改为开机按住音量键进入 VCOM 校准页；写入后立即生效。
    // No Pico PMU on V1.5: hold a volume key while powering on to open VCOM
    // calibration; an accepted value is applied on the next rail power-up.
    if (!vcom_ok) {
        vcom_setup_run(&hl, framebuffer, hw.touch);
        display_request_full_refresh();
    } else if(volume_key_held()) {
        debug_panel_run(&hl, framebuffer, hw.touch);
        display_request_full_refresh();
    }

    // 硬件、显示、触摸与必要的出厂设置均成功后确认新槽。
    // Confirm a new slot only after hardware, display, touch, and required setup succeed.
    pico_ota_confirm_running();

#ifdef KIIKO_AUTO_TEST
    // 无人值守自测不等待解锁：软件重启进入此门禁会一直等人按键，没人可按。
    // Unattended self-test skips the unlock gate: a software reboot would wait here for a key press.
#else
    app_lock_boot_gate(&hl, hw.touch, hw.sensor);
#endif
    metalio_frontlight_restore();
    rp_boot_mark("lock");
    const app_desc_t *first = app_home_page();
    pico_resume_t resume = {0};
    if (pico_boot_take_resume(&resume)) {
        if (!resume.reader) first = app_at(resume.tab);
        else {
            // 挂载等待有界；恢复入口已消费，无卡或资源失败不会反复自动开书。
            // Bound mount waiting after consuming the checkpoint; absent media or failed resources cannot auto-open in a loop.
            if (!strncmp(resume.path, "/sdcard/", 8)) {
                read_pico_sd_info_t info;
                for (unsigned wait = 0; wait < 150 && read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED; ++wait)
                    vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (!strncmp(resume.path, "/flash/books/", 13) && !book_store_flash_ready()) {
                book_store_root_t roots[BOOK_STORE_ROOT_MAX]; int count;
                (void)book_store_roots(roots, &count);
            }
            struct stat st;
            if (!stat(resume.path, &st) && S_ISREG(st.st_mode) &&
                app_book_request_resume(resume.path, resume.fullscreen)) first = app_at(1);
        }
    }
    char interrupted[PICO_BOOT_PATH_MAX];
    if (pico_boot_interrupted_book(interrupted, sizeof(interrupted))) first = &app_boot_recovery;
    if(resuming && !lock_pin_enabled()) {
        if(first==app_at(1) && resume.reader)app_font_activate_reading();
        else app_font_activate_system();
    }
    rp_boot_mark("resume");
    // 首次引导服从现有密码/异常启动门禁，老配置不会重新触发。
    // Setup retains PIN/crash gates and never reopens for legacy profiles.
    if(first==app_home_page()&&!lock_pin_enabled()&&app_setup_pending())first=&app_onboarding;
    // 无人值守自测：热点相位强制进传书页并直接起热点，相位由 NVS 在两次重启间轮换；
    // 自测任务随后打状态并重启。交付构建里这两个入口是空实现。
    // Unattended self-test: the hotspot phase forces the Transfer page and starts the AP;
    // NVS rotates the phase across reboots while the test task logs status and restarts.
    // Both entries are no-ops in delivery builds.
    if (app_auto_test_transfer_page_wanted() && !lock_pin_enabled()) {
        extern const app_desc_t app_transfer;
        extern void app_transfer_request_hotspot_start(void);
        app_transfer_request_hotspot_start();
        first = &app_transfer;
    }
    app_auto_test_start();
#ifdef KIIKO_LOCAL_TRANSFER_START
    // Explicit unattended local tests only; preserve PIN and recovery gates.
    if (!lock_pin_enabled() && first == app_home_page()) {
        extern const app_desc_t app_transfer;
        extern void app_transfer_request_wifi_upload(void);
        app_transfer_request_wifi_upload();
        first = &app_transfer;
    }
#endif
    app_loop_run(&(app_loop_config_t){
        .hl = &hl,
        .fb = framebuffer,
        .acc = hw.sensor,
        .tp = hw.touch,
        .sensor_ready = hw.sensor_ready,
        // 用户修订：深睡一次性恢复上次阅读或主页面；异常启动安全回首页。
        // User revision: resume the last reader/main tab once after deep sleep; abnormal starts safely enter Home.
        .first_app = first,
    });
}
