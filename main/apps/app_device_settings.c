/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：分组设置页；网络短时对时后 PMU 持续走时，开机恢复系统时钟。
 * English: Grouped settings; temporary WiFi sync seeds the PMU, whose RTC restores time at boot.
 * 用户修订：设置列表滑动仅差分刷新内容，滑动过程中不周期插入黑白清屏。
 * User revision: settings lists scroll with content-only differentials and no periodic black/white wipe during swipes.
 * 用户授权新输入法：资料卡和签名共用九宫格/全键盘及离线词语候选，保存仍由设置页负责。
 * Authorized keyboard revision: profile and signature share T9/QWERTY and offline phrases; this page still owns saving.
 * 用户修订：资料卡与签名输入只画变化区域，不因输入次数触发整屏黑白清屏。
 * User revision: profile/signature input paints changed regions without whole-screen wipes based on typing counts.
 * 用户修订：快刷说明标注棋盘格亚克力，普通与水波纹继续保留灰阶；三种主页面刷新模式均固定底栏。
 * User revision: the fast-mode description identifies checkerboard acrylic; ordinary/water keep grays and all main-screen modes retain navigation.
 * 用户修订：联网升级先画反馈再启动任务；进度只追加黑色段，不擦除已完成段，不累计整页清屏。升级前释放不在显示的封面缓存。
 * User revision: present feedback before starting online work; append black progress without erasing completed segments or counting page cleanup. Release off-screen cover caches before updating.
 * 用户确认：字号并入系统字体并预览；选择字体后询问同步阅读字体，设置按连接/显示/个性化/升级分组。
 * Approved revision: size and preview live with the UI face; ask before syncing the reading face, and group connection/display/personalization/maintenance.
 * 用户修订：设置题头与列表周围留白保持纯白，普通主页切入/周期整理复用书架同轮分区输出；题头柔和，内容白像素只向白擦除，灰阶保留。普通列表滑动使用柔和差分；手动全刷、离散滚动和快刷/水波纹节奏保持。
 * User revision: keep setting chrome/list whitespace white. Ordinary root entry/periodic cleanup reuse simultaneous shelf regions with soft chrome, directional white content and preserved grays. Scroll ordinary lists with soft differences; retain manual full refresh, discrete gestures and fast/ripple pacing.
 * 用户2026-10-20修订：设置二级页常驻灰底栏，内容止于底栏上方，返回清理和不可中断任务守卫保持。
 * User revision2026-10-20: settings children retain gray navigation; content stops above it, reusing back cleanup and noninterruptible-job guards.
 * S31用户修订：设置主列表内容规整为黑白并使用DU20，固定底栏不变；子页的字体/壁纸预览仍保留灰阶。
 * S31 user revision: normalize root settings content to BW and use DU20, retaining fixed navigation and gray font/wallpaper previews in subpages.
 * 用户撤回跟手滚动：三种主页模式统一恢复一次手势推进一段列表并差分刷新的原逻辑，无惯性、浅墨或停稳补刷。
 * User rollback: all main modes return to one discrete list step per gesture with differential updates, without inertia, light motion ink or idle restoration.
 * 用户2026-10-21修订：设置列表滚动保留文字抗锯齿和灰阶头像，撤掉书架额外四相补白，列表仍一次输出且不影响固定底栏。
 * User revision2026-10-21: keep AA text and gray avatars during list scrolls, omit the shelf's four added white phases, and retain one output with fixed navigation.
 * 用户修订：蓝牙扫描结果行数变化时改走整页刷新，差分会把旧字留在屏上造成重叠残影。
 * User revision: repaint the whole page when the Bluetooth scan row count changes; a differential leaves the old glyphs behind and overlaps the new text.
 * 用户2026-10-30修订：语音学习改为手动开始/手动结束按钮，不再自动录音；语音页新增识别测试开关，开启后说话实时显示识别结果。
 * User revision2026-10-30: voice learning uses explicit start/stop buttons instead of auto-capture, and the voice page gains a recognition test that shows live verdicts while you speak.
 * 用户2026-10-30修订：学习页只教「上一页」「下一页」两个方向（词由用户自定），每个方向各配一个「开始学习」按钮；模板改存 NVS 新命名空间并减半体积，修复保存失败。
 * User revision2026-10-30: the learn page teaches only the previous/next directions (any user-chosen words), each with its own 「开始学习」 button; templates move to a new NVS namespace at half the size to fix save failures.
 * 用户修订：语音页新增「识别阈值」行（数值夹在可见的 − / + 按钮之间，单步 0.1，手动值直接显示数字如 4.0，首次点按转手动，清除学习恢复自动，引擎默认阈值 2）；麦克风连开失败在学习页明确报错并可重试。
 * User revision: the voice page gains a threshold row (the value sits between visible − / + buttons, stepping 0.1, a manual value shown bare like 4.0; the first tap switches to manual, clearing learning restores auto, engine default threshold 2), and repeated mic-open failures fail visibly on the learn page with a retry.
 */
#if KIIKO_BOARD_METALIO47
#include "app_content_open.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "app_content_open.h"
#include "app_registry.h"
#include "app_transfer_mode.h"
#include "settings.h"
#include "factory_reset.h"
#include "book_cover_jobs.h"
#if KIIKO_BOARD_METALIO47
#include "sleep.h"
#include "sd_recovery.h"
#else
#endif
#include "lock_pin_flow.h"
#include "ui_pinpad.h"
#include "book_lock_collage.h"
#include "book_lock_bookmark.h"
#include "boot_state.h"
#include "ble_page_turner.h"
#include "voice_turn.h"
#include "bt_host.h"
#if KIIKO_BOARD_METALIO47
#include "music_player.h"  // 耳机扫描/连接前起音频栈 / bring the audio stack up before scanning
#endif
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "read_pico_transfer.h"
#include "esp_log.h"
#include "ttf_font.h"
#if KIIKO_BOARD_METALIO47
#include "font_filename.h"
#endif
#include "app_font_context.h"
#if KIIKO_BOARD_METALIO47
#include "ui_online_fonts.h"
static ui_online_fonts_t s_online_fonts;
#endif
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_text_input.h"
#include "ui_keyboard.h"
#include "ui_nav.h"
#include "ui_wallpaper.h"
#include "ui_avatar.h"
#include "read_pico_search.h"
#include "book_store.h"
#include "book_cover.h"
#include "ota_update.h"
#include "ota_online.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "display.h"
#include "display_main_waveform.h"
#include "e0470_epaper_waveform.h"
#include "pmu_selftest.h"
#if KIIKO_BOARD_METALIO47
#include "metalio_hw.h"
#else
#include "soc/rtc_cntl_reg.h"
#endif

static void fit_value(char *value, int width);
static void settings_card(uint8_t *fb, EpdRect rect, int radius, uint8_t fill, uint8_t edge);
static char s_notice[96];
// 分类按"同一功能域"划分：连接/显示/电源锁屏/个性化/系统维护。锁屏不再拆到个性化，
// 刷新并入显示，升级与重置合并为系统维护。
// Categories follow one functional domain each: connectivity, display, power-and-lock,
// personalisation, system maintenance. Lock settings no longer live under personalisation,
// refresh joins display, and upgrade and reset merge into system maintenance.
typedef enum { SETTINGS_MAIN, SETTINGS_GROUP_CONNECT, SETTINGS_GROUP_DISPLAY,
               SETTINGS_GROUP_POWER, SETTINGS_GROUP_PERSONAL, SETTINGS_GROUP_SYSTEM,
               SETTINGS_WIFI, SETTINGS_TIME,
               SETTINGS_TIME_EDIT, SETTINGS_SHELF_STYLE, SETTINGS_SYSTEM_FONT,
#if KIIKO_BOARD_METALIO47
               SETTINGS_ONLINE_FONTS,
               SETTINGS_SD_RECOVERY,
#endif
               SETTINGS_SYSTEM_SIZE, SETTINGS_SYSTEM_CONTRAST, SETTINGS_MAIN_REFRESH, SETTINGS_LOCK_STYLE,
               SETTINGS_WALLPAPER, SETTINGS_WALLPAPER_PREVIEW,
               SETTINGS_CONFIG, SETTINGS_UPGRADE, SETTINGS_BOOT, SETTINGS_FACTORY_RESET,
#if KIIKO_BOARD_METALIO47
               SETTINGS_POWER_SLEEP, SETTINGS_AUTO_LOCK, SETTINGS_PROFILE, SETTINGS_AVATAR,
#else
               SETTINGS_POWER_SLEEP, SETTINGS_AUTO_LOCK, SETTINGS_PROFILE, SETTINGS_AVATAR,
#endif
               SETTINGS_BLUETOOTH, SETTINGS_BLE_SCAN,
#if KIIKO_BOARD_METALIO47
               SETTINGS_LOCK_PIN, SETTINGS_PIN_ENTRY, SETTINGS_TEXT_EDIT, SETTINGS_FRONTLIGHT,
               SETTINGS_VOICE, SETTINGS_VOICE_LEARN, SETTINGS_AI_KEY, SETTINGS_AI_PICK } settings_page_t;
#else
               SETTINGS_LOCK_PIN, SETTINGS_PIN_ENTRY, SETTINGS_TEXT_EDIT } settings_page_t;
#endif
static settings_page_t s_page;
// 二级分组：主页面只留分类入口，点进分类后返回分类而不是回到主页。
// Group pages: the root list keeps category entries only; children return to their category.
static settings_page_t s_parent = SETTINGS_MAIN;
static bool settings_group_page(settings_page_t page) {
    return page == SETTINGS_GROUP_CONNECT || page == SETTINGS_GROUP_DISPLAY ||
           page == SETTINGS_GROUP_POWER || page == SETTINGS_GROUP_PERSONAL ||
           page == SETTINGS_GROUP_SYSTEM;
}
static void settings_open(settings_page_t page) {
    s_parent = s_page;
    s_page = page;
}
static settings_page_t settings_back(void) {
    settings_page_t page = s_parent;
    s_parent = SETTINGS_MAIN;
    return page;
}
#if KIIKO_BOARD_METALIO47
static char s_sd_notice[96];
static EpdRect sd_recovery_button(void) { return (EpdRect){54,850,576,78}; }
#endif
static int settings_content_end(void) {
    return UI_NAV_REFRESH_END;
}
static int settings_viewport_end(void) {
    return UI_NAV_TOP;
}
static void settings_navigation(uint8_t *fb) {
#if KIIKO_BOARD_METALIO47
    if(s_page==SETTINGS_ONLINE_FONTS)return;
#endif
    ui_nav_draw(fb,4);  // 第5栏 / 5th tab in the five-tab bar
}
static int settings_nav_hit(uint16_t x,uint16_t y) {
    return ui_nav_hit(x,y);
}

static bool s_reset_confirm, s_reset_pending;
static char s_reset_notice[96];
static ui_pinpad_t s_pinpad;
static lock_pin_flow_t s_pinflow;
static bool s_pin_pending;
static unsigned s_pin_failures;
static int64_t s_pin_retry_at;
static void pin_setup_close(void) {
    ui_pinpad_end(&s_pinpad); lock_pin_flow_end(&s_pinflow); s_pin_pending = false;
}
static void pin_setup_open(app_ctx_t *ctx, bool disable) {
    pin_setup_close(); lock_pin_flow_begin(&s_pinflow, disable);
    ui_pinpad_begin(&s_pinpad, ctx->fb, lock_pin_flow_title(&s_pinflow));
    s_pinpad.navigation=true;
    s_page = SETTINGS_PIN_ENTRY;
}
static app_redraw_t pin_paint(app_ctx_t *ctx, EpdRect area) {
    ui_pinpad_paint(ctx->fb, &s_pinpad, area);
#if KIIKO_BOARD_METALIO47
    bool previous=display_system_binary_set(true);
    bool previous_nav=display_system_navigation_set(true);
#endif
    guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, area));
#if KIIKO_BOARD_METALIO47
    display_system_navigation_set(previous_nav);
    display_system_binary_set(previous);
#endif
    return APP_REDRAW_DONE;
}
static app_redraw_t pin_setup_tick(app_ctx_t *ctx) {
    if (s_pinpad.blocked && ctx->now_ms >= s_pin_retry_at) {
        ui_pinpad_reset(&s_pinpad, NULL, "请重新输入");
        return pin_paint(ctx, ui_pinpad_entry_area());
    }
    if (!s_pin_pending) return APP_REDRAW_NONE;
    s_pin_pending = false;
    lock_pin_step_t prior = s_pinflow.step;
    esp_err_t error = lock_pin_flow_input(&s_pinflow, s_pinpad.digits);
    if (error == ESP_OK && s_pinflow.step == PIN_FLOW_DONE) {
        pin_setup_close(); s_page = SETTINGS_LOCK_PIN;
        snprintf(s_notice, sizeof(s_notice), "%s", lock_pin_enabled() ? "锁屏密码已保存" : "锁屏密码已关闭");
        return APP_REDRAW_PAGE;
    }
    const char *notice = error == ESP_OK ? "" : error == ESP_ERR_INVALID_RESPONSE ? "两次输入不同，请重设" :
        error == ESP_ERR_INVALID_ARG ? "原密码不正确，请重试" : "保存失败，请重试";
    ui_pinpad_reset(&s_pinpad, lock_pin_flow_title(&s_pinflow), notice);
    if (error == ESP_ERR_INVALID_ARG && prior == PIN_FLOW_OLD && ++s_pin_failures >= 3) {
        s_pin_failures = 0; s_pin_retry_at = ctx->now_ms + 10000;
        snprintf(s_pinpad.notice, sizeof(s_pinpad.notice), "请等待 10 秒后重试"); s_pinpad.blocked = true;
    }
    return pin_paint(ctx, (EpdRect){122, 218, 440, 247});
}
static int s_style_scroll, s_main_scroll;
// 新样式使用新编号；旧3/4仍退回亚克力，列表保持5，6为上游液态玻璃，7为本地浅色书架。
// New style gets a new ID; retired3/4 still fall back to acrylic, list stays5, 6 is upstream liquid glass, 7 the local pale shelf.
static const uint8_t s_shelf_style_ids[] = {1, 2, 6, 5, 7};
#define SHELF_STYLE_CHOICES 5
static bool s_collage_prepare_pending;
static bool s_scroll_drag_consumed, s_scroll_present_pending;
// 蓝牙子页：滚动位置、正在学习哪个动作（0 未学，1 上一页，2 下一页）、提示行，
// 以及耳机设备列表是否展开（扫描后就地显示，连上或进页时收起）。
// Bluetooth sub-page: scroll offset, which action is being learned (0 idle, 1 prev, 2 next),
// a notice line, and whether the headset device list is expanded (shown in place after a scan,
// collapsed once linked or on page entry).
static int s_ble_scroll, s_ble_learning;
static char s_ble_notice[64];
static char s_ble_feedback[64];
static bool s_bt_show;
static int s_font_page, s_wallpaper_page;
static bool s_sync_pending;
static bool s_font_sync_confirm;
static char s_font_sync_path[TTF_FONT_PATH_MAX];
static bool s_config_confirm;
// 屏幕修复：点一下就开始。整屏盲刷要几秒，活儿交给 tick——不能在触摸回调里做。
// Panel repair: one tap starts it. The blind full-screen drive takes seconds, so the work goes to the
// tick rather than a touch callback.
static bool s_panel_repair_pending;
static bool s_boot_pending;
#if KIIKO_BOARD_METALIO47
static bool s_boot_connected;
static char s_boot_notice[96];
#else
#endif
static bool s_upgrade_confirm, s_upgrade_online, s_local_pending, s_upgrade_restart;
static pico_ota_info_t s_local_update;
static EXT_RAM_BSS_ATTR pico_update_status_t s_update;
static char s_upgrade_notice[96];
static uint32_t s_update_drawn_percent, s_update_drawn_at;
static pico_update_state_t s_update_drawn_state;
static bool s_update_drawn_busy, s_upgrade_offer_pending;
typedef enum { UPGRADE_JOB_NONE, UPGRADE_JOB_CHECK, UPGRADE_JOB_DOWNLOAD } upgrade_job_t;
static upgrade_job_t s_upgrade_job;
static int s_upgrade_bar_width;
static uint32_t upgrade_percent(void) {
    uint32_t percent = s_update.release.size ? (uint64_t)s_update.received * 100 / s_update.release.size : 0;
    return percent > 100 ? 100 : percent;
}
static void upgrade_remember(uint32_t now) {
    s_update_drawn_state = s_update.state;
    s_update_drawn_busy = s_update.busy;
    s_update_drawn_percent = upgrade_percent();
    s_update_drawn_at = now;
}
// 结果等任务收尾后一次显示；进度只在五秒且增长五个百分点后补画新增像素。
// Show results once after worker cleanup; append progress after five seconds and five percentage points.
static app_redraw_t upgrade_status_redraw(uint32_t now) {
    bool downloading = s_update.state == PICO_UPDATE_DOWNLOADING;
    if (s_update.busy && s_update.state != PICO_UPDATE_CHECKING && !downloading)
        return APP_REDRAW_NONE;
    bool offer = s_upgrade_offer_pending && !s_update.busy && s_update.state == PICO_UPDATE_AVAILABLE;
    if (offer) {
        s_upgrade_offer_pending = false;
        s_upgrade_confirm = s_upgrade_online = true;
    } else if (!s_update.busy) s_upgrade_offer_pending = false;
    if (offer || s_update.state != s_update_drawn_state || s_update.busy != s_update_drawn_busy) {
        upgrade_remember(now);
        return APP_REDRAW_PAGE;
    }
    uint32_t percent = upgrade_percent();
    if (downloading && percent >= s_update_drawn_percent + 5 && now - s_update_drawn_at >= 5000) {
        upgrade_remember(now);
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}
static EpdRect upgrade_progress_area(void) { return (EpdRect){52, 468, 580, 64}; }
static void draw_upgrade_percent(uint8_t *fb) {
    uint32_t percent = upgrade_percent();
    EpdRect number = {496, 472, 132, 42};
    ui_fill_rect_fast(number, UI_GRAY_WHITE, fb);
    char progress[32]; snprintf(progress, sizeof(progress), "%lu%%", (unsigned long)percent);
    ui_text_fixed(fb, 624, 477, 26, progress, EPD_DRAW_ALIGN_RIGHT, false);
    ui_image_bw_rect(fb, number);
}
static void draw_upgrade_progress(uint8_t *fb) {
    ui_fill_rect_fast(upgrade_progress_area(), UI_GRAY_WHITE, fb);
    draw_upgrade_percent(fb);
    ui_fill_rect_fast((EpdRect){60, 518, 564, 10}, UI_GRAY_BLACK, fb);
    ui_fill_rect_fast((EpdRect){61, 519, 562, 8}, UI_GRAY_WHITE, fb);
    s_upgrade_bar_width = (int)(562 * upgrade_percent() / 100);
    if (s_upgrade_bar_width)
        ui_fill_rect_fast((EpdRect){61, 519, s_upgrade_bar_width, 8}, UI_GRAY_BLACK, fb);
}
static void draw_upgrade_progress_delta(uint8_t *fb) {
    int width = (int)(562 * upgrade_percent() / 100);
    if (width <= s_upgrade_bar_width) return;
    draw_upgrade_percent(fb);
    ui_fill_rect_fast((EpdRect){61 + s_upgrade_bar_width, 519, width - s_upgrade_bar_width, 8}, UI_GRAY_BLACK, fb);
    s_upgrade_bar_width = width;
}
// 排队时只改 UI 快照；本次画面推送结束后的 tick 才启动 TLS/擦写。
// Scheduling changes only the UI snapshot; start TLS/flash on the tick after its presentation.
static void upgrade_schedule(bool downloading, uint32_t now) {
    s_upgrade_job = downloading ? UPGRADE_JOB_DOWNLOAD : UPGRADE_JOB_CHECK;
    s_update.state = downloading ? PICO_UPDATE_DOWNLOADING : PICO_UPDATE_CHECKING;
    s_update.busy = true;
    s_update.received = 0;
    snprintf(s_update.message, sizeof(s_update.message), "%s", downloading
        ? "正在下载，短暂断网会自动重连" : "正在连接并检查更新");
    s_upgrade_offer_pending = !downloading;
    upgrade_remember(now);
}
static app_redraw_t upgrade_start_pending(void) {
    bool downloading = s_upgrade_job == UPGRADE_JOB_DOWNLOAD;
    s_upgrade_job = UPGRADE_JOB_NONE;
    app_home_cover_mode_changed();
    app_book_cover_mode_changed();
    esp_err_t error = downloading ? pico_online_download() : pico_online_check();
    pico_online_get_status(&s_update);
    if (error == ESP_OK) return APP_REDRAW_NONE;
    s_upgrade_offer_pending = false;
    s_update.busy = false;
    if (s_update.state != PICO_UPDATE_FAILED) s_update.message[0] = 0;
    s_update.state = PICO_UPDATE_FAILED;
    if (!s_update.message[0]) snprintf(s_update.message, sizeof(s_update.message), "无法开始升级，请退出传书后重试");
    snprintf(s_upgrade_notice, sizeof(s_upgrade_notice), "%s", s_update.message);
    return APP_REDRAW_PAGE;
}
// 日志按 UTF-8 字符和实际字宽换行，限制六行，不影响下载时的局部刷新。
// Wrap notes on UTF-8 boundaries using measured widths; cap at six lines without changing download refreshes.
static bool upgrade_note_line(const char **cursor, char *line, size_t capacity) {
    if (!cursor || !*cursor || !line || capacity < 5) return false;
    const char *p = *cursor;
    while (*p && (strchr("\r\n\t ;", *p) || !strncmp(p, "；", 3)))
        p += !strncmp(p, "；", 3) ? 3 : 1;
    size_t used = 0;
    line[0] = 0;
    while (*p && !strchr("\r\n;", *p) && strncmp(p, "；", 3)) {
        unsigned char lead = (unsigned char)*p;
        size_t bytes = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (bytes > strlen(p) || used + bytes >= capacity) break;
        memcpy(line + used, p, bytes);
        line[used + bytes] = 0;
        if (used && ui_text_fixed_width_px(26, line) > 540) { line[used] = 0; break; }
        used += bytes;
        p += bytes;
    }
    *cursor = p;
    return used != 0;
}
static void draw_upgrade_notes(uint8_t *fb) {
    const char *cursor = s_update.release.notes;
    char line[sizeof(s_update.release.notes)];
    if (!*cursor) {
        ui_text_fixed(fb, 60, 530, 26, "本次更新暂无说明", EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    for (int row = 0; row < 6 && upgrade_note_line(&cursor, line, sizeof(line)); ++row) {
        if (row == 5 && *cursor) {
            size_t used = strlen(line);
            while (used && (used + 4 > sizeof(line) || ui_text_fixed_width_px(26, line) > 514)) {
                do { --used; } while (used && ((unsigned char)line[used] & 0xc0) == 0x80);
                line[used] = 0;
            }
            memcpy(line + used, "…", 4);
        }
        ui_text_fixed(fb, 60, 530 + row * 38, 26, line, EPD_DRAW_ALIGN_LEFT, false);
    }
}
static EpdRect upgrade_confirm_button(bool online, bool accept) {
    return (EpdRect){accept ? 364 : 60, online ? 842 : 613, 260, 64};
}
#if KIIKO_BOARD_METALIO47
// 中文：本地TF升级与显示同属UI任务；先归还缓存并关面板轨，再擦写应用槽。
// English: Local TF installation runs on the display's UI task; release caches/rails before flash writes.
static esp_err_t install_local_update(void) {
    if (!(book_cover_jobs_quiesce(2000) && ui_avatar_quiesce(2000))) {
        snprintf(s_upgrade_notice,sizeof(s_upgrade_notice),"封面任务正在收尾，请稍后重试");
        return ESP_ERR_NOT_FINISHED;
    }
    app_home_cover_mode_changed();
    app_book_cover_mode_changed();
    epd_poweroff();
    return pico_ota_install(PICO_OTA_UPDATE_PATH, s_upgrade_notice, sizeof(s_upgrade_notice));
}

#else
#endif
static void upgrade_open(void) {
    (void)pico_ota_inspect(PICO_OTA_UPDATE_PATH, &s_local_update);
    pico_online_get_status(&s_update);
    s_upgrade_confirm = s_local_pending = s_upgrade_restart = false;
    s_upgrade_offer_pending = false;
    s_upgrade_job = UPGRADE_JOB_NONE;
    s_upgrade_bar_width = 0;
    upgrade_remember(esp_timer_get_time() / 1000);
    s_upgrade_notice[0] = 0;
    s_page = SETTINGS_UPGRADE;
}
static const char *const TAG = "device_settings";
// 主设置始终单行；大字号只改变字形，字体预览单独保留足够空间。
// Main rows never stack; larger glyphs share one centerline, with extra space only in font previews.
// metalio47 的布局是写死的常量，这里的分支只服务旧板，按板型裁掉以免留下死函数。
// metalio47 hard-codes its layout, so this predicate only serves the older board.
#if !KIIKO_BOARD_METALIO47
static bool settings_stacked(void) { return app_settings_system_font_size() >= 150; }
#endif
static int settings_profile_height(void) { return 106; }
static int settings_main_row_height(void) {
    // 根列表合并成 5 类后项数变少，行高放大到 104 让分类卡铺满整屏；分组页仍用 82，
    // 6 项也能一屏放下。渲染与点击命中都走这一个宏，两者永远同步。
    // The root list is down to five entries, so its rows grow to 104 and the card fills the
    // screen; group pages keep 82, which still fits six rows. Rendering and hit-testing share
    // this one macro, so they can never disagree.
    return settings_group_page(s_page) ? 82 : 104;
}
#define SETTINGS_ROW_H settings_main_row_height()
#if KIIKO_BOARD_METALIO47
#define SETTINGS_WIRELESS_ROWS 3
#else
#define SETTINGS_WIRELESS_ROWS 3
#endif
// 显示与刷新：前光（仅 Metalio）、系统字体、系统对比度、主页刷新模式、首页强刷。
// Display and refresh: frontlight (Metalio only), font, contrast, refresh mode, full refresh.
#if KIIKO_BOARD_METALIO47
#define SETTINGS_DISPLAY_ROWS 5
#else
#define SETTINGS_DISPLAY_ROWS 4
#endif
// 电源与锁屏：自动休眠锁屏、关机睡眠（仅 Metalio）、浅睡播放音乐（仅 Metalio）、锁屏样式、锁屏密码。
// 没有「双击解锁」：唤醒一律单击，这是产品规则而不是偏好。
// Power and lock: auto lock, shutdown sleep (Metalio only), music through light sleep (Metalio
// only), lock style, lock PIN. No "double-press unlock": a single press always wakes.
#if KIIKO_BOARD_METALIO47
#define SETTINGS_POWER_ROWS 5
// 关机睡眠档位的显示顺序与文案：1 分钟是调试档，枚举里排在末尾（不影响已存 NVS 的值），
// 但界面上放在「立即」之后，方便调试时一眼找到。
// Display order for the deep-sleep steps: the 1-minute debug step lives last in the enum but
// right after "immediate" in the UI, so it is easy to reach while debugging.
static const uint8_t k_sleep_off_order[] = {APP_SLEEP_OFF_IMMEDIATE, APP_SLEEP_OFF_1MIN,
    APP_SLEEP_OFF_10, APP_SLEEP_OFF_30, APP_SLEEP_OFF_60, APP_SLEEP_OFF_NEVER};
static const char *const k_sleep_off_labels[] = {"立即", "1 分钟", "10 分钟", "30 分钟", "60 分钟", "从不"};
#define SLEEP_OFF_ROWS ((int)(sizeof(k_sleep_off_order) / sizeof(k_sleep_off_order[0])))
#else
#define SETTINGS_POWER_ROWS 3
#endif
// 个性化：书架样式、状态栏签名、触摸振动（振动只属于触感偏好，不再挂在前光页里）。
// Personalisation: shelf style, status signature, touch haptics (haptics is a feel preference
// and no longer lives on the frontlight page).
#if KIIKO_BOARD_METALIO47
#define SETTINGS_PERSONAL_ROWS 3
#else
#define SETTINGS_PERSONAL_ROWS 2
#endif
// 系统维护：系统升级、内存卡恢复（仅 Metalio）、保存与恢复、屏幕修复、BOOT 刷机、恢复出厂。
// System maintenance: upgrade, card recovery (Metalio only), config backup, panel repair,
// BOOT flashing, factory reset.
#if KIIKO_BOARD_METALIO47
#define SETTINGS_SYSTEM_ROWS 6
#else
#define SETTINGS_SYSTEM_ROWS 5
#endif
// 主页分类入口：Metalio 6 类（多一条语音翻页直达），其它板 5 类。
// Root categories: six on Metalio (voice page-turn opens directly), five elsewhere.
#if KIIKO_BOARD_METALIO47
#define SETTINGS_CATEGORY_ROWS 7
#else
#define SETTINGS_CATEGORY_ROWS 5
#endif
#define SETTINGS_WIRELESS_Y (180 + settings_profile_height() + 77)
// 主页面只放分类入口；分组内容各自进子页，主页面不再需要滚动。
// The root list holds category entries only; each category opens its own page, so the root never scrolls.
#define SETTINGS_GROUP_CARD_Y 214
// 分类入口卡片：与原来的首组同一位置。
// Category card sits where the first group used to start.
#define SETTINGS_CATEGORY_Y SETTINGS_WIRELESS_Y
// 设置行图标：32 像素盒，在行高 68 里垂直居中；墨色统一，避免一行一个灰度。
// Setting row icons: a 32 px box centred in the 68 px row with one shared ink level.
#define SETTINGS_UNLOCK_ICON 18
#define SETTINGS_ICON_PX 32
#define SETTINGS_ICON_INK 0x58
// 主页面已无分组内容，滚动关闭。
// The root page no longer carries group content, so scrolling is disabled.
#define SETTINGS_SCROLL_MAX 0
static char s_editor[96], s_editor_notice[80];
static ui_text_edit_t s_editor_input;
static bool s_editor_signature;
static const char *system_font_label(const char *path) {
    if (!path || !path[0]) return "思源黑体";
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    static char stem[TTF_FONT_NAME_MAX];
#if KIIKO_BOARD_METALIO47
    for(int i=0;i<ttf_font_count();++i) {
        const ttf_font_item_t *item=ttf_font_item(i);
        if(item&&!strcmp(item->path,path))return ttf_font_localized_name(item->name);
    }
    if(!font_filename_make(name,stem,sizeof(stem),false))snprintf(stem,sizeof(stem),"Font-%08x",(unsigned)font_filename_hash(name));
#else
    snprintf(stem, sizeof(stem), "%s", name);
    size_t len = strlen(stem);
    if (len >= 4 && !strcasecmp(stem + len - 4, ".ttf")) stem[len - 4] = 0;
#endif
    if (!strcasecmp(stem, "Hei")) return "思源黑体（TF 卡）";
    return ttf_font_localized_name(stem);
}
typedef struct {
    EpdRect size, preview;
    int preview_title, font_title, first, row_height, slots;
} system_font_layout_t;
static system_font_layout_t system_font_layout(void) {
#if KIIKO_BOARD_METALIO47
    // 顶部预览保持紧凑，为100%–200%字号统一留四行字体；文字仍按系统字号绘制。
    // Keep previews compact and reserve four font rows at100%–200%, retaining scaled text.
    system_font_layout_t l = {{36,197,612,118},{36,398,612,154},344,580,640,94,4};
#else
    system_font_layout_t l = settings_stacked()
        ? (system_font_layout_t){{36,197,612,183},{36,471,612,216},413,722,785,142,2}
        : (system_font_layout_t){{36,197,612,100},{36,379,612,154},322,560,615,99,4};
#endif
    l.slots=(settings_viewport_end()-42-l.first)/l.row_height;
    return l;
}
static EpdRect system_font_item_rect(int slot) {
    system_font_layout_t l = system_font_layout();
    return (EpdRect){36,l.first + slot * l.row_height,612,l.row_height - 16};
}
#if KIIKO_BOARD_METALIO47
static EpdRect system_font_page_button(bool next) {
    return (EpdRect){next?504:36,settings_viewport_end()-66,144,48};
}
#endif
static EpdRect system_font_sync_button(bool sync) { return (EpdRect){sync ? 351 : 72,683,261,74}; }
static void draw_system_font_sync(uint8_t *fb) {
    // 仅一个有界弹窗，不额外打开阅读字体。/ One bounded dialog; never load the reading face here.
    settings_card(fb,(EpdRect){48,389,588,401},28,UI_GRAY_WHITE,0x58);
    ui_text_fixed_vc(fb,342,448,32,"同步到阅读字体？",EPD_DRAW_ALIGN_CENTER,false);
    char name[80]; snprintf(name,sizeof(name),"已选择「%s」",system_font_label(s_font_sync_path));
    while (ui_text_fixed_width_px(27,name) > 520 && strlen(name) > 6) {
        size_t n = strlen(name) - 1;
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
        name[n] = 0;
    }
    ui_text_fixed_vc(fb,342,516,27,name,EPD_DRAW_ALIGN_CENTER,false);
    ui_text_fixed_vc(fb,342,570,24,"是否让阅读正文也使用这个字体？",EPD_DRAW_ALIGN_CENTER,false);
    ui_text_fixed_vc(fb,342,614,23,s_notice[0]?s_notice:"不同步时，保留原来的阅读字体。",EPD_DRAW_ALIGN_CENTER,false);
    for (int i=0;i<2;++i) {
        EpdRect r=system_font_sync_button(i);
        settings_card(fb,r,20,i ? UI_GRAY_BLACK : UI_GRAY_WHITE,0x58);
        ui_text_fixed_vc(fb,r.x+r.width/2,r.y+r.height/2,25,i ? "同步阅读字体" : "仅系统字体",EPD_DRAW_ALIGN_CENTER,i);
    }
}
static int s_year, s_month, s_day, s_hour, s_minute;
#define WALLPAPER_MAX 64
typedef struct { char path[288]; char name[96]; } wallpaper_item_t;
// 壁纸列表放 PSRAM，理由同上。
// The wallpaper list lives in PSRAM for the same reason.
static wallpaper_item_t *s_wallpapers;

static bool wallpapers_alloc(void) {
    if (!s_wallpapers)
        s_wallpapers = heap_caps_calloc(WALLPAPER_MAX, sizeof(wallpaper_item_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_wallpapers != NULL;
}
static int s_wallpaper_count;
static int s_wallpaper_selected;
static bool s_wallpaper_confirm, s_wallpaper_preview_ok;
static bool s_bookmark_picker;
static uint8_t *s_bookmark_preview;
static uint8_t *s_bookmark_preview_base;

static void wallpaper_scan_dir(const char *root) {
    if (!wallpapers_alloc()) return;
    DIR *dir = opendir(root);
    if (!dir) return;
    struct dirent *entry;
    while (s_wallpaper_count < WALLPAPER_MAX && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.' || !strncmp(entry->d_name, "._", 2)) continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg") && strcasecmp(ext, ".png"))) continue;
        if(s_bookmark_picker && strcasecmp(ext,".png"))continue;
        wallpaper_item_t *item = &s_wallpapers[s_wallpaper_count];
        if (snprintf(item->path, sizeof(item->path), "%s/%s", root, entry->d_name) >= sizeof(item->path)) continue;
        struct stat st;
        if (stat(item->path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > BOOK_IMAGE_FILE_MAX) continue;
        size_t name_len = strnlen(entry->d_name, sizeof(item->name) - 1);
        memcpy(item->name, entry->d_name, name_len);
        item->name[name_len] = 0;
        ++s_wallpaper_count;
    }
    closedir(dir);
}
static void bookmark_preview_release(void) {
    free(s_bookmark_preview);s_bookmark_preview=NULL;
    free(s_bookmark_preview_base);s_bookmark_preview_base=NULL;
}
static void bookmark_preview_prepare(const uint8_t *background) {
    free(s_bookmark_preview);s_bookmark_preview=NULL;
    if(!s_bookmark_picker || s_wallpaper_selected<0 || s_wallpaper_selected>=s_wallpaper_count)return;
    const size_t bytes=684u*1216u/2u;
    if(!background || heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)<(s_bookmark_preview_base?bytes:2*bytes)+2304u*1024u)return;
    // 一次保存目录画面，切换PNG预览时不反复叠加上一张；离开预览即释放。
    // Save the directory frame once so swiping never nests old PNG previews; release on leaving preview.
    if(!s_bookmark_preview_base){
        s_bookmark_preview_base=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(s_bookmark_preview_base)memcpy(s_bookmark_preview_base,background,bytes);
    }
    if(!s_bookmark_preview_base)return;
    uint8_t *canvas=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(canvas)memcpy(canvas,s_bookmark_preview_base,bytes);
    if(canvas && book_lock_bookmark_draw(canvas,s_wallpapers[s_wallpaper_selected].path))s_bookmark_preview=canvas;
    else free(canvas);
}
static void wallpaper_scan(void) {
    s_wallpaper_count = s_wallpaper_page = 0;
    wallpaper_scan_dir("/sdcard/pictures");
    wallpaper_scan_dir("/sdcard/images");
    wallpaper_scan_dir("/sdcard");
}


static void profile_editor_open(bool signature) {
    s_editor_signature = signature;
    snprintf(s_editor, sizeof(s_editor), "%s", signature ? app_settings_status_signature() : app_settings_device_name());
    ui_text_edit_init(&s_editor_input, s_editor, sizeof(s_editor));
    s_editor_notice[0] = 0;
    ui_keyboard_begin(&s_editor_input, false);
    s_page = SETTINGS_TEXT_EDIT;
}




static int days_in_month(int year, int month) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return 29;
    return days[month - 1];
}

// 公历日期转 UTC 天数；界面统一按北京时间 UTC+8 编辑。
// Convert a Gregorian date to UTC days; the editor uses Beijing time (UTC+8).
static int64_t civil_days(int year, int month, int day) {
    year -= month <= 2;
    int era = year / 400;
    int yoe = year - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static void time_open(void) {
    time_t now = time(NULL);
    if (now < 1704067200) {
        const pmu_snapshot_t *pmu = read_pico_pmu_get();
        if (pmu && pmu->time_synced && pmu->unix_sec >= 1704067200) now = pmu->unix_sec;
    }
    if (now < 1704067200) {
        char month[4] = "Jan"; int day = 1, year = 2024;
        if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3) {
            strcpy(month, "Jan"); day = 1; year = 2024;
        }
        static const char *const months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        int m = 1; while (m < 12 && strcmp(month, months[m - 1])) ++m;
        s_year = year; s_month = m; s_day = day; s_hour = s_minute = 0;
    } else {
        struct tm date;
        time_t local = now + 8 * 3600;
        gmtime_r(&local, &date);
        s_year = date.tm_year + 1900; s_month = date.tm_mon + 1; s_day = date.tm_mday;
        s_hour = date.tm_hour; s_minute = date.tm_min;
    }
    s_notice[0] = 0;
    s_page = SETTINGS_TIME_EDIT;
}

static void time_adjust(int field, int step) {
    if (field == 0) s_year = s_year + step > 2099 ? 2024 : s_year + step < 2024 ? 2099 : s_year + step;
    if (field == 1) s_month = s_month + step > 12 ? 1 : s_month + step < 1 ? 12 : s_month + step;
    if (field == 2) s_day = s_day + step > days_in_month(s_year, s_month) ? 1 :
                            s_day + step < 1 ? days_in_month(s_year, s_month) : s_day + step;
    if (field == 3) s_hour = (s_hour + step + 24) % 24;
    if (field == 4) s_minute = (s_minute + step + 60) % 60;
    if (s_day > days_in_month(s_year, s_month)) s_day = days_in_month(s_year, s_month);
}

static bool time_save(void) {
    int64_t epoch = civil_days(s_year, s_month, s_day) * 86400 +
                    s_hour * 3600 + s_minute * 60 - 8 * 3600;
    if (epoch < 1704067200 || epoch > 4102444799LL) return false;
    struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    if (settimeofday(&tv, NULL)) return false;
    uint8_t payload[4] = {(uint8_t)epoch, (uint8_t)(epoch >> 8),
                          (uint8_t)(epoch >> 16), (uint8_t)(epoch >> 24)};
    esp_err_t err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
    if (err == ESP_OK) err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (err != ESP_OK || !pmu || !pmu->time_synced) {
        snprintf(s_notice, sizeof(s_notice), "系统时间已设置，PMU 同步失败");
        return false;
    }
    snprintf(s_notice, sizeof(s_notice), "时间已保存，并同步到电源管理芯片");
    s_page = SETTINGS_TIME;
    return true;
}

#if KIIKO_BOARD_METALIO47
static bool settings_plain_list(void) {
    return s_page==SETTINGS_MAIN||s_page==SETTINGS_BLUETOOTH||s_page==SETTINGS_BLE_SCAN;
}
#endif
static void settings_card(uint8_t *fb, EpdRect rect, int radius, uint8_t fill, uint8_t edge) {
#if KIIKO_BOARD_METALIO47
    // 只把卡片装饰改成黑白；字缘和头像保持原灰阶。
    // Binarize decoration only; retain glyph and avatar grays.
    if(settings_plain_list()) {
        ui_fill_round_rect(fb,rect,radius,fill<128?UI_GRAY_BLACK:UI_GRAY_WHITE);
        ui_draw_round_rect(fb,rect,radius,UI_GRAY_BLACK);
        return;
    }
#endif
    ui_fill_round_rect(fb, rect, radius, fill);
    ui_draw_control_frame(fb, rect, radius, edge);
}
static void settings_divider(uint8_t *fb, int y, int x, int width) {
#if KIIKO_BOARD_METALIO47
    if(settings_plain_list()){ui_hairline(fb,y,x,width,UI_GRAY_BLACK);return;}
#endif
    ui_draw_separator(fb, y, x, width, 0x60);
}
static void settings_value(uint8_t *fb,int x,int cy,int px,const char *value) {
    char shown[128]; snprintf(shown,sizeof(shown),"%s",value);
    size_t n=strlen(shown);
    bool arrow=n>=3 && !strcmp(shown+n-3,"›");
    if (arrow) {
        shown[n-=3]=0;
        while (n && shown[n-1]==' ') shown[--n]=0;
        ui_draw_icon(fb,x-4,cy,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
    }
    ui_text_vc(fb,x-(arrow?28:0),cy,px,shown,EPD_DRAW_ALIGN_RIGHT,false);
}
#if KIIKO_BOARD_METALIO47
static EpdRect pin_grace_row(void) { return (EpdRect){36,606,612,84}; }
#endif
static void row(uint8_t *fb, int y, const char *label, const char *value) {
    ui_text(fb, 54, y + 17, 27, label, EPD_DRAW_ALIGN_LEFT, false);
    settings_value(fb,627,y+20+ui_text_effective_px(21)/2,21,value);
    settings_divider(fb, y + 68, 54, 575);
}
static void section(uint8_t *fb, int y, const char *title) {
    ui_text(fb, 40, y, 22, title, EPD_DRAW_ALIGN_LEFT, false);
}

/* ---- 蓝牙子页的版式 / Layout of the Bluetooth sub-page ---- */

// 行高比 setting_group 的 68 略大，因为这一页每行右侧常常要放一个按钮。
// Rows are a little taller than setting_group's 68 because these rows often carry a button.
#define BLE_ROW_H 84
// 按钮右缘与 row() 的值右缘对齐；值文字右端再让出按钮宽度，两者不重叠。
// The button's right edge aligns with row()'s value edge; the value text then stops short of the
// button, so the two never overlap.
#define BLE_BTN_W 118
#define BLE_BTN_RIGHT 624
#define BLE_VALUE_RIGHT (BLE_BTN_RIGHT - BLE_BTN_W - 17)

// 渲染和命中测试都调这个函数，两边看到的是同一份坐标——否则会出现「按钮画在这儿、
// 点在别处」，这一页之前就是这么坏的。
// Rendering and hit testing both call this, so they see identical coordinates. Without it a
// control ends up drawn in one place and tapped in another, which is exactly how this page was
// broken before.
typedef struct {
    int hp_section_top;               // 「耳机」分组标题 / headset section title
    int hp_bt_top;                    // 「蓝牙音频」开关卡顶 / Bluetooth-audio switch card top
    int hp_status_top;                // 耳机状态行（右侧连接按钮）/ headset status row (action button at right)
    int hp_tone_top;                  // 发声测试行（右侧播放按钮）/ test-tone row (play button at right)
    int hp_dev_top[BT_AUDIO_DEV_MAX]; // 扫描结果行顶 / scanned device row tops
    int hp_dev_count;                 // 结果行数（0=列表收起）/ result rows (0 = collapsed)
    int hp_empty_top;                 // 空结果行（-1 无）/-1 when absent
    int hp_auto_top;                  // 「自动连接上次设备」开关卡顶 / auto-reconnect switch card top
    int hp_volkey_top;                // 「连接耳机时侧键仅调音量」开关卡顶 / side-keys-volume-only switch card top
    int hp_pair_title_top;            // 耳机侧「已配对」分组标题 / paired-headset section title
    int hp_pair_top[APP_BT_PAIRED_MAX]; // 已配对耳机行顶；-1 表示该位不存在 / paired row tops, -1 when absent
    int hp_pair_rows;                 // 已配对行数（0=只画提示）/ paired rows (0 = hint only)
    int bt_section_top;               // 「翻页器」分组标题 / page-turner section title
    int toggle_top;                  // 开关卡片顶；学习/反馈时此位画提示卡 / switch card top; the prompt card takes this slot while learning
    int status_top;
    int notice_top;                  // -1 表示没有提示 / -1 when absent
    int bonds_title_top;
    int bond_top[BLE_PT_MAX_BONDS];  // 已配对行顶；-1 表示该位不存在 / bonded row tops, -1 when absent
    int bond_rows;
    int scan_entry_top;              // 「扫描设备 ›」入口行顶 / the "scan devices" entry row
    int learn_title_top;
    int learn_top[2];
    int footer_top;
    int content_bottom;              // 内容底，用于滚动上限 / content bottom, for the scroll limit
} ble_layout_t;

static ble_layout_t ble_layout(void) {
    ble_layout_t l;
    memset(&l, 0, sizeof(l));
    l.hp_empty_top = -1;
    int y = 202 - s_ble_scroll;
#if KIIKO_BOARD_METALIO47
    // 耳机区在最上：标题 + 状态行，扫描结果就地展开在状态行下方。学习/反馈时先收起结果，
    // 让提示卡保持在首屏内。
    // The headset block sits first: title plus status row, with scan results expanding below it.
    // Results collapse while learning or showing feedback so the prompt stays on the first screen.
    const bool hp_expand = s_bt_show && !s_ble_learning && !s_ble_feedback[0];
    l.hp_section_top = y;
    y += 38;
    // 「蓝牙音频」开关排在状态行之前：关掉后下面的状态与扫描都失去意义。
    // The Bluetooth-audio switch precedes the status row; switched off, the status and scan below are moot.
    l.hp_bt_top = y;
    y += 126 + 16;
    l.hp_status_top = y;
    y += BLE_ROW_H;
    // 发声测试紧跟状态行：它必须在「连上了到底有没有声」的判断现场，翻页才找得到。
    // The test tone sits right under the status row: it belongs where the "linked but silent"
    // question is asked, not two pages away.
    l.hp_tone_top = y;
    y += BLE_ROW_H;
    l.hp_dev_count = hp_expand ? (int)bt_audio_device_count() : 0;
    if (l.hp_dev_count > BT_AUDIO_DEV_MAX) l.hp_dev_count = BT_AUDIO_DEV_MAX;
    for (int i = 0; i < l.hp_dev_count; ++i) l.hp_dev_top[i] = y + i * BLE_ROW_H;
    y += l.hp_dev_count * BLE_ROW_H;
    if (hp_expand && !l.hp_dev_count) {  // 扫完一无所获时占一行 / reserve one row when the scan found nothing
        l.hp_empty_top = y;
        y += BLE_ROW_H;
    }
    y += 14;
    l.hp_auto_top = y;
    y += 126 + 24;
    // 已配对耳机表：行数进滚动总高，空表留一行放提示。
    // The paired-headset table joins the scroll height; an empty table keeps one row for the hint.
    l.hp_pair_title_top = y;
    y += 38;
    l.hp_pair_rows = (int)app_settings_bt_paired_count();
    if (l.hp_pair_rows > APP_BT_PAIRED_MAX) l.hp_pair_rows = APP_BT_PAIRED_MAX;
    for (int i = 0; i < APP_BT_PAIRED_MAX; ++i)
        l.hp_pair_top[i] = i < l.hp_pair_rows ? y + i * BLE_ROW_H : -1;
    y += (l.hp_pair_rows ? l.hp_pair_rows : 1) * BLE_ROW_H;
    l.hp_volkey_top = y;
    y += 126 + 24;
    l.bt_section_top = y;
#else
    l.bt_section_top = y;
#endif
    y += 38;
    l.toggle_top = y;
    y += 126 + 38;
    l.status_top = y;
    y += BLE_ROW_H + 20;
    l.notice_top = s_ble_notice[0] ? y : -1;
    if (s_ble_notice[0]) y += 38;
    l.bonds_title_top = y;
    y += 38;
    l.bond_rows = (int)ble_pt_bond_count();
    for (int i = 0; i < BLE_PT_MAX_BONDS; ++i)
        l.bond_top[i] = i < l.bond_rows ? y + i * BLE_ROW_H : -1;
    y += (l.bond_rows ? l.bond_rows : 1) * BLE_ROW_H;
    l.scan_entry_top = y + 16;
    y = l.scan_entry_top + BLE_ROW_H + 30;
    l.learn_title_top = y;
    l.learn_top[0] = y + 38;
    l.learn_top[1] = l.learn_top[0] + BLE_ROW_H;
    l.footer_top = l.learn_top[1] + BLE_ROW_H + 25;
    l.content_bottom = l.footer_top + 66;
    return l;
}

// 一行的左标签 + 让出按钮后的右值，外加分隔线。与 row() 同高，但右端不会压到按钮。
// A row's left label plus a right value that stops short of the button, with the usual divider.
// Same height as row(), but the value never runs under the button.
// 学到的键按整帧显示：帧里非零的字节照原样列出来。同一个键每次按下发的字节一样，所以这串
// 标识每次都一样；不同的键只要有一个字节不同，读出来就不同。
// A learned key shows its whole frame: the non-zero bytes, in order. The same key sends the same
// bytes every press, so this identifier never changes, and two keys that differ in any byte read
// differently.
static void format_bound_key(const ble_pt_frame_t *frame, char *out, size_t size) {
    if (!frame || !frame->len) {
        if (size) out[0] = 0;
        return;
    }
    size_t used = 0;
    unsigned shown = 0;
    for (size_t i = 0; i < frame->len && used + 4 < size; ++i) {
        if (!frame->bytes[i]) continue;
        if (shown == 3) {  // 值列只有 140px：最多列 3 个字节，多的用 + 表示 / the value column holds three
            snprintf(out + used, size - used, " +");
            return;
        }
        const int written = snprintf(out + used, size - used, shown ? " %02X" : "%02X", frame->bytes[i]);
        if (written <= 0) break;
        used += (size_t)written;
        ++shown;
    }
    if (!shown) snprintf(out, size, "00");  // 理论上到不了：全零帧不给绑 / unreachable: a zero frame is never bound
}

static void ble_row(uint8_t *fb, int y, const char *label, const char *value) {
    if (y + BLE_ROW_H < 190 || y >= settings_viewport_end()) return;
    settings_card(fb, (EpdRect){36, y, 612, BLE_ROW_H - 8}, 18, UI_GRAY_WHITE, 0x70);
    char name[96]; snprintf(name, sizeof(name), "%s", label);
    fit_value(name, 290);
    ui_text_vc(fb, 60, y + 38, 26, name, EPD_DRAW_ALIGN_LEFT, false);
    if (value && value[0]) {
        char shown[96]; snprintf(shown, sizeof(shown), "%s", value); fit_value(shown, 140);
        settings_value(fb,BLE_VALUE_RIGHT,y+38,21,shown);
    }
}

// 行右侧的按钮。竖直居中于该行，右缘与值文字对齐。
// The button on a row's right: vertically centred in the row, right edge aligned with the value.
static EpdRect ble_button_rect(int row_top) {
    return (EpdRect){BLE_BTN_RIGHT - BLE_BTN_W, row_top + 10, BLE_BTN_W, 56};
}

// 三级页：进页即持续扫描，选中一台连上就退回二级页。列表长了也不影响二级页。
// Third-level page: scanning starts on entry and runs continuously; picking a device connects
// and returns to level two. A long list never affects the page above it.
typedef struct {
    int devices_top;
    int device_rows;
    int status_top;
    int content_bottom;
} ble_scan_layout_t;

// 扫描列表只列真的广播了名字的设备。没有广播名时组件把地址回落成名字，那一行对用户
// 认不出是什么设备，而且地址太长会把信号强度挤掉。
// The scan list shows only devices that actually advertised a name. Without one the component
// falls back to the address: nothing the user can recognise, and long enough to crowd the signal
// strength off the row.
static bool ble_scan_visible(const ble_pt_device_t *dev) {
    return dev != NULL && dev->has_name;
}

static int ble_scan_visible_count(void) {
    int visible = 0;
    const uint8_t count = ble_pt_device_count();
    for (uint8_t i = 0; i < count; ++i) {
        if (ble_scan_visible(ble_pt_device(i))) ++visible;
    }
    return visible;
}

// 取第 row 台有名字的设备；遍历顺序仍是组件的原始顺序。
// The row-th device that has a name; the walk keeps the component's order.
static const ble_pt_device_t *ble_scan_visible_at(int row) {
    if (row < 0) return NULL;
    const uint8_t count = ble_pt_device_count();
    int seen = 0;
    for (uint8_t i = 0; i < count; ++i) {
        const ble_pt_device_t *dev = ble_pt_device(i);
        if (!ble_scan_visible(dev)) continue;
        if (seen++ == row) return dev;
    }
    return NULL;
}

static ble_scan_layout_t ble_scan_layout(void) {
    ble_scan_layout_t l;
    memset(&l, 0, sizeof(l));
    l.status_top = 200;
    l.devices_top = 300;
    l.device_rows = ble_scan_visible_count();
    l.content_bottom = l.devices_top + (l.device_rows ? l.device_rows * BLE_ROW_H : 68) + 40;
    return l;
}
static void back_header(uint8_t *fb, const char *title) {
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, title, EPD_DRAW_ALIGN_CENTER, false);
    settings_divider(fb, 174, 36, 612);
}

static void setting_icon(uint8_t *fb, int index, int cx, int cy) {
    // 每个设置项对应一个 Lucide 图标；索引顺序由各分组传入的 icons[] 数组决定。
    // One Lucide icon per setting row; the index order comes from each group's icons[] array.
    static const ui_icon_t icons[] = {
        UI_ICON_WIFI,              // 0 无线连接
        UI_ICON_BLUETOOTH,         // 1 蓝牙
        UI_ICON_TYPE,              // 2 系统字体、阅读操作
        UI_ICON_A_LARGE_SMALL,     // 3 系统字号
        UI_ICON_LIBRARY_BIG,       // 4 书架样式
        UI_ICON_TICKET,            // 5 锁屏样式
        UI_ICON_CLOCK,             // 6 日期与时间
        UI_ICON_CONTRAST,          // 7 系统对比度
        UI_ICON_COPY,              // 8 保存与恢复
        UI_ICON_POWER,             // 9 关机睡眠
        UI_ICON_TEXT_ALIGN_START,  // 10 状态栏签名
        UI_ICON_REFRESH_CW,        // 11 首页强刷
        UI_ICON_DOWNLOAD,          // 12 系统升级
        UI_ICON_CPU,               // 13 BOOT 刷机
        UI_ICON_EYE_CLOSED,        // 14 自动休眠
        UI_ICON_SLIDERS_HORIZONTAL, // 15 主页刷新模式 / Main refresh mode
        UI_ICON_LOCK,              // 16 锁屏密码 / lock screen password
        UI_ICON_ROTATE_CW,         // 17 屏幕修复 / panel repair
        UI_ICON_LOCK,              // 18 双击解锁 / double-press unlock
        UI_ICON_BOOK_OPEN,         // 19 预留 / spare
        UI_ICON_ZAP,               // 20 触摸振动 / touch haptics
        UI_ICON_MUSIC,             // 21 浅睡播放音乐 / music through light sleep
        UI_ICON_ZAP,               // 22 AI 快捷键 / AI shortcut key
    };
    if (index < 0 || index >= (int)(sizeof(icons) / sizeof(icons[0]))) return;
    ui_draw_icon(fb, cx, cy, SETTINGS_ICON_PX, icons[index], SETTINGS_ICON_INK);
}

static void settings_fit_text(char *value, int px, int width) {
    while (value[0] && ui_text_fixed_width_px(ui_text_effective_px(px),value)>width) {
        size_t n=strlen(value)-1;while(n&&((unsigned char)value[n]&0xc0)==0x80)--n;value[n]=0;
    }
}
static void fit_value(char *value, int width) { settings_fit_text(value,20,width); }

static void settings_switch(uint8_t *fb, int center_y, bool active) {
    // 灰色轨道在滚动差分扫描下会经过深色相位产生黑闪；纯黑白无灰阶过渡，任何刷新模式都稳定。
    // 开启时不再把整条轨道填黑（用户要求去掉那块黑），轨道恒白底黑框，状态只靠圆点：
    // 开=右端实心黑点，关=左端空心圈。
    // A gray track transits dark phases under the scroll's differential scan and flashes black;
    // pure black/white holds clean in every mode. The rail no longer turns solid black when on —
    // it stays white with a dark frame and only the knob shows state.
    EpdRect track = {555, center_y - 19, 69, 39};
    ui_fill_round_rect(fb, track, 19, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, track, 19, 0x30);
    int cx = active ? track.x + 49 : track.x + 20;
    if (active) {
        epd_fill_circle(cx, track.y + 19, 16, UI_GRAY_BLACK, fb);
    } else {
        epd_fill_circle(cx, track.y + 19, 16, UI_GRAY_WHITE, fb);
        epd_draw_circle(cx, track.y + 19, 16, 0x30, fb);
    }
}

static void settings_rows(uint8_t *fb, int card_y, const int icons[], const char *const labels[],
                          const char *const values[], int count) {
    const int row_height = SETTINGS_ROW_H;
    // 屏外的行不测字、不生成字形；保留卡片边界与原滚动坐标。
    // Skip off-screen glyph work while preserving card boundaries and scroll coordinates.
    if (card_y + count * row_height <= 160 || card_y >= settings_viewport_end()) return;
    settings_card(fb, (EpdRect){36, card_y, 612, count * row_height}, 22,
                  UI_GRAY_WHITE, 0x70);
    for (int i = 0; i < count; ++i) {
        int y = card_y + i * row_height;
        if (y + row_height <= 160 || y >= settings_viewport_end()) continue;
        if (i) settings_divider(fb, y, 88, 544);
        setting_icon(fb, icons[i], 67, y + row_height / 2);
        if (icons[i] == SETTINGS_UNLOCK_ICON) {
            ui_text_vc(fb, 100, y + row_height / 2, 25, labels[i], EPD_DRAW_ALIGN_LEFT, false);
            settings_switch(fb, y + row_height / 2, app_settings_wake_presses() == 2);
            continue;
        }
        char shown[128]; snprintf(shown,sizeof(shown),"%s",values[i]);
        int value_width = 594 - 100 - 20 - ui_text_fixed_width_px(ui_text_effective_px(25),labels[i]);
        if (value_width > 215) value_width = 215;
        if (value_width < 0) value_width = 0;
        fit_value(shown,value_width);
        ui_text_vc(fb,100,y+row_height/2,25,labels[i],EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,594,y+row_height/2,20,shown,EPD_DRAW_ALIGN_RIGHT,false);
        // 导航是图标，不依赖用户字体是否包含 › 字形，也不会被截短掉。
        // Navigation is an icon, independent of the font cmap and value truncation.
        ui_draw_icon(fb,625,y+row_height/2,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
    }
}

static void setting_group(uint8_t *fb, int title_y, const char *title,
                          int card_y, const int icons[], const char *const labels[],
                          const char *const values[], int count) {
    title_y -= s_main_scroll;
    card_y -= s_main_scroll;
    if (card_y + count * SETTINGS_ROW_H <= 160 || title_y >= settings_viewport_end()) return;
    if (title && title_y + ui_text_effective_px(20) > 160 && title_y < settings_viewport_end())
        ui_text(fb, 42, title_y, 20, title, EPD_DRAW_ALIGN_LEFT, false);
    settings_rows(fb, card_y, icons, labels, values, count);
}

static void setting_toggle(uint8_t *fb, int y, const char *title, const char *detail, bool active) {
    settings_card(fb, (EpdRect){36, y, 612, 126}, 22, UI_GRAY_WHITE, 0x70);
    ui_text(fb, 59, y + 24, 27, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 59, y + 75, 19, detail, EPD_DRAW_ALIGN_LEFT, false);
    settings_switch(fb, y + 62, active);
}

static void draw_style_thumbnail(uint8_t *fb, int style, int top) {
    if (top < 242 || top + 170 >= settings_viewport_end()) return;
    if (style == 5) {
        ui_fill_round_rect(fb, (EpdRect){68,top,548,170}, 12, UI_GRAY_WHITE);
        for (int i=0;i<3;++i) {
            int y=top+9+i*52;
            ui_fill_rect_fast((EpdRect){86,y,31,43},i==1?0x80:0x40,fb);
            ui_fill_rect_fast((EpdRect){137,y+6,218,3},0,fb);
            ui_fill_rect_fast((EpdRect){137,y+19,84,2},0x70,fb);
            ui_fill_rect_fast((EpdRect){137,y+34,435,4},0xd0,fb);
            ui_fill_rect_fast((EpdRect){137,y+34,120+i*75,4},0x40,fb);
        }
        return;
    }
    const int left = 99;
    // 浅色书架：书本整体用淡色调，且不画层板黑条。
    // Pale shelf: draw every book in a light tone and omit the dark shelf bar.
    const bool pale = style == 7;
    for (int i = 0; i < 3; ++i) {
        int x = left + 20 + i * 155;
        EpdRect cover = {x, top, 116, 145};
        const uint8_t ink = pale ? 0x88 : (i == 1 ? 0xc0 : 0xe0);
        ui_fill_rect_fast(cover, pale ? 0xd8 : (i == 1 ? 0x60 : i == 2 ? 0xa0 : 0x40), fb);
        ui_draw_round_rect(fb, cover, 1, pale ? 0x70 : 0x30);
        ui_draw_round_rect(fb, (EpdRect){x + 7, top + 7, 102, 130}, 1, ink);
        ui_hairline(fb, top + 47, x + 28, 60, ink);
        ui_hairline(fb, top + 54, x + 39, 38, ink);
        ui_hairline(fb, top + 111, x + 44, 28, ink);
    }
    if (style == 1) {
        ui_fill_rect_fast((EpdRect){left, top + 143, 486, 16}, 0x30, fb);
        ui_hairline(fb, top + 143, left, 486, 0x80);
    } else if (style == 2) {
        ui_draw_acrylic_guard(fb, (EpdRect){left, top + 94, 486, 70});
    } else if (style == 6) {
        ui_draw_liquid_shelf_rail(fb, (EpdRect){left, top + 126, 486, 36});
    }
}

// 仅 82x82 灰阶头像，不缓存整屏或第二份字库；文件变更/拔卡会使缓存失效。
// One 82x82 gray avatar, never a full frame or second font; file changes/removal invalidate it.
static EXT_RAM_BSS_ATTR uint8_t s_avatar_pixels[82*82];
static char s_avatar_path[288];
static bool s_avatar_ready, s_avatar_pending;
static char s_main_wifi[33];
static bool s_main_wifi_saved;
static const char *settings_shelf_label(void) {
    return app_settings_shelf_style()==7?"浅色书架":app_settings_shelf_style()==6?"液态玻璃书架":
           app_settings_shelf_style()==5?"列表书架":
           app_settings_shelf_style()==1?"深色书轨":"亚克力书架";
}
static const char *settings_lock_label(void) {
    return app_settings_lock_style()==3?"透明书封":app_settings_lock_style()==2?"书架拼贴":
           app_settings_lock_style()==1?"壁纸":"阅读票根";
}
static const char *settings_refresh_label(void) {
    return app_settings_main_refresh_mode()==APP_MAIN_REFRESH_FAST?"快刷":
           app_settings_main_refresh_mode()==APP_MAIN_REFRESH_WATER?"水波纹":"普通";
}
// 二级分类页：把原先主列表里的一整组搬到这里，列表不再需要长距离滚动。
// Category pages move one whole group here, so the list no longer needs long scrolling.
static void settings_group_render(uint8_t *fb) {
    back_header(fb, s_page==SETTINGS_GROUP_CONNECT?"连接与时间":
                    s_page==SETTINGS_GROUP_DISPLAY?"显示与刷新":
                    s_page==SETTINGS_GROUP_POWER?"电源与锁屏":
                    s_page==SETTINGS_GROUP_PERSONAL?"个性化":"系统维护");
    if (s_page==SETTINGS_GROUP_CONNECT) {
        char wifi[56];
        snprintf(wifi,sizeof(wifi),"%s",s_main_wifi_saved?s_main_wifi:"未配置");
        // 语音翻页已独立成根列表的一项，这里只剩真正的连接与时间项。
        // Voice page-turn is its own root entry now; this page keeps connectivity and time only.
#if KIIKO_BOARD_METALIO47
        // 「已连接」只是模组的说法；音频通路没成立时如实写成「耳机无音频」，别让用户
        // 以为能放歌了。判据见 bt_audio_audio_ready()。
        // "Connected" is only the module's claim; when the audio path never came up, say
        // "no audio" instead of letting the user assume playback works.
        // 「已关闭」不准确：耳机连着、翻页器关着时也会落到这里。改为按真实连接态，
        // 其余都归为「未连接」，与耳机区状态行口径一致。
        // "Disabled" was inaccurate: a linked headset with the turner off still fell here.
        // Reflect the real link state; everything else reads "未连接" to match the headset row.
        const char *ble_value;
        if (bt_audio_link_active())
            ble_value = bt_audio_audio_ready() ? "耳机已连接" : "耳机无音频";
        else if (ble_pt_connected())
            ble_value = "翻页器已连接";
        else
            ble_value = "未连接";
#else
        const char *ble_value=app_settings_ble_turner()?"已开启":"已关闭";
#endif
        const char *labels[]={"WiFi","蓝牙","日期与时间"};
        const char *values[]={wifi,ble_value,"设置"};
        const int icons[]={0,1,6};
        settings_rows(fb,SETTINGS_GROUP_CARD_Y,icons,labels,values,SETTINGS_WIRELESS_ROWS);
    } else if (s_page==SETTINGS_GROUP_DISPLAY) {
        char font_name[96],contrast[24];
        snprintf(font_name,sizeof(font_name),"%s · %u%%",system_font_label(app_settings_system_font_path()),app_settings_system_font_size());
        snprintf(contrast,sizeof(contrast),"%u%%",app_settings_system_contrast());
#if KIIKO_BOARD_METALIO47
        char light[24];
        if(metalio_frontlight_enabled())
            snprintf(light,sizeof(light),"%.1f%%",metalio_frontlight_position(false)*100.0f/METALIO_FRONTLIGHT_RANGE);
        else strcpy(light,"已关闭");
        const char *labels[]={"前光","系统字体","系统对比度","主页刷新模式","首页强刷"};
        const char *values[]={light,font_name,contrast,settings_refresh_label(),
                              app_settings_home_full_refresh()?"开启":"关闭"};
        const int icons[]={7,2,7,15,11};
#else
        const char *labels[]={"系统字体","系统对比度","主页刷新模式","首页强刷"};
        const char *values[]={font_name,contrast,settings_refresh_label(),
                              app_settings_home_full_refresh()?"开启":"关闭"};
        const int icons[]={2,7,15,11};
#endif
        settings_rows(fb,SETTINGS_GROUP_CARD_Y,icons,labels,values,SETTINGS_DISPLAY_ROWS);
    } else if (s_page==SETTINGS_GROUP_POWER) {
        char idle[24];
        unsigned minutes=app_settings_auto_lock_minutes();
        if(minutes)snprintf(idle,sizeof(idle),"%u 分钟",minutes);else strcpy(idle,"关闭");
        // 锁屏样式与密码跟休眠、解锁同属一屏，不再拆到"个性化"。
        // Lock style and PIN share this page with sleep and unlock instead of personalisation.
#if KIIKO_BOARD_METALIO47
        const char *labels[]={"自动休眠锁屏","关机睡眠","浅睡播放音乐","锁屏样式","锁屏密码"};
        const char *values[]={idle,app_settings_sleep_off_label(),
            app_settings_music_in_sleep()?"开启":"关闭",
            settings_lock_label(),lock_pin_enabled()?"已开启":"已关闭"};
        const int icons[]={14,9,21,5,16};
#else
        const char *labels[]={"自动休眠锁屏","锁屏样式","锁屏密码"};
        const char *values[]={idle,settings_lock_label(),lock_pin_enabled()?"已开启":"已关闭"};
        const int icons[]={14,5,16};
#endif
        settings_rows(fb,SETTINGS_GROUP_CARD_Y,icons,labels,values,SETTINGS_POWER_ROWS);
    } else if (s_page==SETTINGS_GROUP_PERSONAL) {
        char signature[96];
        snprintf(signature,sizeof(signature),"%s",app_settings_status_signature()[0]?app_settings_status_signature():"未设置");
        // 触摸振动是触感偏好，跟书架/签名同属个性化；前光页只留光相关项。
        // Touch haptics is a feel preference and belongs with shelf and signature; the frontlight
        // page keeps light-only rows.
#if KIIKO_BOARD_METALIO47
        const char *labels[]={"书架样式","状态栏签名","触摸振动"};
        const char *values[]={settings_shelf_label(),signature,
                              metalio_haptic_enabled()?"开启":"关闭"};
        const int icons[]={4,10,20};
#else
        const char *labels[]={"书架样式","状态栏签名"};
        const char *values[]={settings_shelf_label(),signature};
        const int icons[]={4,10};
#endif
        settings_rows(fb,SETTINGS_GROUP_CARD_Y,icons,labels,values,SETTINGS_PERSONAL_ROWS);
    } else {
        // 系统维护：升级、备份、维修、重置同一域，一个页面放完。
        // System maintenance: upgrade, backup, repair and reset in one domain and one page.
#if KIIKO_BOARD_METALIO47
        const char *labels[]={"系统升级","内存卡恢复","保存与恢复","屏幕修复","BOOT 刷机","恢复出厂设置"};
        const char *values[]={"OTA / TF 卡","挂卡与字体","配置备份","清除条纹","电脑刷机","重置系统"};
        const int icons[]={12,11,8,17,13,11};
#else
        const char *labels[]={"系统升级","保存与恢复","屏幕修复","BOOT 刷机","恢复出厂设置"};
        const char *values[]={"OTA / TF 卡","配置备份","清除条纹","电脑刷机","重置系统"};
        const int icons[]={12,8,17,13,11};
#endif
        settings_rows(fb,SETTINGS_GROUP_CARD_Y,icons,labels,values,SETTINGS_SYSTEM_ROWS);
    }
    if (s_notice[0]) ui_text_box(fb,(EpdRect){54,920,576,120},22,s_notice,EPD_DRAW_ALIGN_LEFT,false);
    settings_navigation(fb);
}
#if KIIKO_BOARD_METALIO47
/* ---- 语音翻页 / Voice page-turn ---- */

static const char *voice_word_label(int word) {
    switch (word) {
        case VOICE_TURN_WORD_NEXT: return "下一页";
        case VOICE_TURN_WORD_PREV: return "上一页";
        default: return "—";
    }
}

static int voice_learned_words(void) {
    int n = 0;
    for (int w = 0; w < VOICE_TURN_WORD_COUNT; ++w)
        if (voice_turn_word_reps((voice_turn_word_t)w)) ++n;
    return n;
}

// 学习页当前在学的方向：-1 表示还没开课，两个方向各有自己的按钮。
// The direction being taught: -1 means no session yet; each direction owns its own button.
static int s_voice_word;

// 识别测试只活在语音页：页内开关，不入 NVS，重启即关。
// The recognition test lives on the voice page only: an in-page switch, never persisted.
static bool s_voice_test;
static uint32_t s_voice_test_seq0;  // 开测时的判定序号，只有之后的新结果才显示 / decision seq at start; only later verdicts show

// 关掉测试：停麦并丢掉残留方向，避免回到阅读页时误翻一页。
// Stop the test: drop the mic and any leftover direction so returning to the reader never flips a page.
static void voice_test_stop(void) {
    s_voice_test = false;
    voice_turn_set_listen(false);
    (void)voice_turn_take_direction();
}

// 每个方向自己的按钮：渲染和命中共用同一矩形，画在哪儿就点在哪儿。
// One button per direction: render and hit test share one rect.
static EpdRect voice_learn_button(int word) { return (EpdRect){36, 480 + word * 200, 612, 96}; }

static void voice_learn_open(void) {
    voice_test_stop();  // 进学习页必经：测试先收掉 / entering the learn page always drops the test first
    (void)voice_turn_init();
    s_voice_word = -1;  // 不自动开课，等点某个方向的「开始学习」/ no auto-session; a direction's button starts it
}

// 阈值行的 − / + 按钮与中间数值槽：渲染和命中共用同一份坐标，画在哪就点在哪。
// The threshold row's minus/plus buttons and centre value slot: render and hit share one geometry.
#define THR_ROW_Y 436
#define THR_BTN_W 96
#define THR_BTN_H 60
#define THR_BTN_Y (THR_ROW_Y + 4)
#define THR_MINUS_X 332
#define THR_PLUS_X 528
static EpdRect thr_button_rect(bool plus) {
    return plus ? (EpdRect){THR_PLUS_X, THR_BTN_Y, THR_BTN_W, THR_BTN_H}
                : (EpdRect){THR_MINUS_X, THR_BTN_Y, THR_BTN_W, THR_BTN_H};
}
#define THR_VALUE_CX 478
#define THR_CONTROL_CY (THR_BTN_Y + THR_BTN_H / 2)

static void voice_render_page(uint8_t *fb) {
    ui_clear_page(fb);  // 整屏清白，避免叠在上一页（连接分组列表）上重影
    back_header(fb, "语音翻页");
    // 持久化的阈值每轮同步进引擎（幂等，值没变不打日志）；设置存 0.1 计数，引擎要浮点。
    // Push the persisted threshold into the engine each frame; settings hold tenths, the engine wants a float.
    voice_turn_set_accept(app_settings_voice_threshold() ? (float)app_settings_voice_threshold() / 10.0f : 0.0f);
    section(fb, 232, "教自己的词（尽量简短），阅读时说出来即可翻页");
    char reps[32];
    snprintf(reps, sizeof(reps), "%d/%d 已学", voice_learned_words(), (int)VOICE_TURN_WORD_COUNT);
    row(fb, 276, "语音翻页", app_settings_voice_turn() ? "已开启" : "已关闭");
    row(fb, 356, "学习词语", reps);
    // 阈值行：数值夹在 − / + 两枚描边按钮之间，按钮看得见也点得中；首次点按把自动转手动，
    // 「清除学习」恢复自动。点 − 降低、点 + 提高。
    // Threshold row: the value sits between outlined minus/plus buttons you can see and hit; the
    // first tap turns auto into manual, clearing the learning restores auto. Tap − to lower, + to raise.
    {
        const unsigned thr = app_settings_voice_threshold();
        char thrv[32];
        // 手动值直接显示裸数字（4.0 / 6.0），自动态仍带「自动」前缀以示区分。
        // A manual value shows bare (4.0 / 6.0); the auto state keeps its prefix so the two read apart.
        if (thr) snprintf(thrv, sizeof(thrv), "%.1f", (double)thr / 10.0);
        else snprintf(thrv, sizeof(thrv), "自动 %.1f", (double)voice_turn_accept());
        row(fb, THR_ROW_Y, "识别阈值", "");
        for (int i = 0; i < 2; ++i) {
            const EpdRect btn = thr_button_rect(i != 0);
            ui_draw_control_frame(fb, btn, 8, UI_GRAY_BLACK);
            ui_text_vc(fb, btn.x + btn.width / 2, THR_CONTROL_CY, 30, i ? "+" : "−",
                       EPD_DRAW_ALIGN_CENTER, false);
        }
        ui_text_vc(fb, THR_VALUE_CX, THR_CONTROL_CY, 21, thrv, EPD_DRAW_ALIGN_CENTER, false);
    }
    row(fb, 516, "清除学习", "清空");
    row(fb, 596, "识别测试", s_voice_test ? "已开启" : "已关闭");
    if (s_voice_test) {
        // 测试面板：每说一句显示判定，用来确认模板学得能不能认出来。
        // Test panel: show the verdict after every utterance so you can see the templates work.
        if (!voice_turn_listening()) {
            ui_text(fb, 54, 680, 22, "正在开启麦克风…（耳机连接时会等待）", EPD_DRAW_ALIGN_LEFT, false);
        } else {
            voice_turn_match_t m;
            voice_turn_last_match(&m);
            if (m.seq == s_voice_test_seq0) {
                ui_text(fb, 54, 680, 24, "说一遍你教过的词", EPD_DRAW_ALIGN_LEFT, false);
                ui_text(fb, 54, 720, 20, "说完一句，这里会显示识别结果", EPD_DRAW_ALIGN_LEFT, false);
            } else {
                char verdict[96], detail[96];
                if (m.word >= 0) snprintf(verdict, sizeof(verdict), "识别为「%s」", voice_word_label(m.word));
                else snprintf(verdict, sizeof(verdict), "未识别（没过阈值）");
                if (m.best >= 1e8f) snprintf(detail, sizeof(detail), "还没有模板距离，请先学词");
                else snprintf(detail, sizeof(detail), "距离 %.2f / 阈值 %.2f（可继续说）", m.best, m.threshold);
                ui_text(fb, 54, 680, 24, verdict, EPD_DRAW_ALIGN_LEFT, false);
                ui_text(fb, 54, 720, 20, detail, EPD_DRAW_ALIGN_LEFT, false);
            }
        }
    } else {
        ui_text(fb, 54, 680, 20, "任一个方向即可开启；每个词建议录 3 遍，阈值自动。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, 720, 20, "阈值行点 − / + 每次 0.1；只在阅读页开麦，识别完全离线。", EPD_DRAW_ALIGN_LEFT, false);
    }
    if (s_notice[0]) ui_text_box(fb, (EpdRect){54, 760, 576, 120}, 22, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    settings_navigation(fb);
}

static void voice_render_learn(uint8_t *fb) {
    ui_clear_page(fb);  // 整屏清白，避免与上一屏叠字
    back_header(fb, "学习语音命令");
    ui_text(fb, 54, 276, 24, "词尽量简短", EPD_DRAW_ALIGN_LEFT, false);
    const voice_turn_learn_state_t st = voice_turn_learn_poll();
    // 状态行只跟着"正在学的那个方向"走；没开课时给一句引导。
    // The status line follows the active direction; with no session it shows a hint instead.
    char msg[120];
    if (s_voice_word < 0) {
        snprintf(msg, sizeof(msg), "点某个方向的「开始学习」，跟着说一个词");
    } else switch (st) {
        case VOICE_TURN_LEARN_RECORD: {
            // 录音中显示时长，让用户看到"还在录"，不会以为已经结束。
            // Show the running length so the user can see the take is still open.
            const int fr = voice_turn_learn_record_frames();
            if (fr < 0) snprintf(msg, sizeof(msg), "正在开启麦克风…");
            else if (fr == 0) snprintf(msg, sizeof(msg), "麦克风已就绪，请开口说这个词");
            else snprintf(msg, sizeof(msg), "录音中 %.1f 秒，说完点「结束录音」", fr * 0.016);
            break;
        }
        case VOICE_TURN_LEARN_SPOKE: snprintf(msg, sizeof(msg), "正在保存…"); break;
        case VOICE_TURN_LEARN_OK: snprintf(msg, sizeof(msg), "已学会；再录一遍阈值更准，或学另一个方向"); break;
        case VOICE_TURN_LEARN_FAIL: snprintf(msg, sizeof(msg), "没录到或太短，请重新录音"); break;
        case VOICE_TURN_LEARN_BUSY: snprintf(msg, sizeof(msg), "耳机正在播放，请断开后重试"); break;
        case VOICE_TURN_LEARN_FAIL_SAVE: snprintf(msg, sizeof(msg), "保存失败（存储空间不足），请重试"); break;
        case VOICE_TURN_LEARN_FAIL_OPEN: snprintf(msg, sizeof(msg), "麦克风开不起来，点按钮重试"); break;
        case VOICE_TURN_LEARN_WAIT: snprintf(msg, sizeof(msg), "准备好了，点「开始学习」开始录音"); break;
        default: snprintf(msg, sizeof(msg), "准备中…"); break;
    }
    ui_text(fb, 54, 324, 22, msg, EPD_DRAW_ALIGN_LEFT, false);
    // 每个方向一行一条按钮：本方向录音中按下=结束，其余按下=开始学习/重录。
    // One row and one button per direction: a tap stops while it records, otherwise it starts.
    for (int w = 0; w < VOICE_TURN_WORD_COUNT; ++w) {
        char value[32];
        const uint8_t reps = voice_turn_word_reps((voice_turn_word_t)w);
        if (reps) snprintf(value, sizeof(value), "已学 %u 遍", (unsigned)reps);
        else if (w == s_voice_word) snprintf(value, sizeof(value), "正在学");
        else strcpy(value, "未学");
        row(fb, 400 + w * 200, voice_word_label(w), value);
        const bool active = (s_voice_word == w);
        const char *label = "开始学习";
        if (active && st == VOICE_TURN_LEARN_RECORD) label = "结束录音";
        else if (active && (st == VOICE_TURN_LEARN_FAIL || st == VOICE_TURN_LEARN_FAIL_SAVE ||
                            st == VOICE_TURN_LEARN_FAIL_OPEN || st == VOICE_TURN_LEARN_BUSY)) label = "重新录音";
        ui_draw_button(fb, voice_learn_button(w), label, active && st == VOICE_TURN_LEARN_RECORD);
    }
    ui_text(fb, 54, 820, 20, "安静环境下、离麦克风一臂距离内说，识别最稳", EPD_DRAW_ALIGN_LEFT, false);
    settings_navigation(fb);
}
#endif  // KIIKO_BOARD_METALIO47

#if KIIKO_BOARD_METALIO47
// AI 实体键（右上角 GPIO61）短按/长按功能配置：设置页入口 + 选择器。
// AI shortcut key (top-right GPIO61): settings entry plus a short/long action picker.
static int s_ai_pick_target;   // 0=短按 1=长按
static int s_ai_pick_count;    // 选择器项数（短按 5 / 长按 6）
static const char *ai_action_label(app_ai_action_t a) {
    switch (a) {
        case APP_AI_REFRESH: return "全刷一次";
        case APP_AI_BOOKMARK: return "书签开关";
        case APP_AI_FRONTLIGHT: return "前光开关";
        case APP_AI_HOME: return "返回首页";
        case APP_AI_NONE: return "不设功能";
        case APP_AI_MENU: return "打开快捷菜单";
        default: return "—";
    }
}
static void settings_render_ai_key(uint8_t *fb) {
    back_header(fb, "AI 快捷键");
    section(fb, 248, "右上角 AI 实体键 · 分别设置短按与长按");
    const char *labels[] = {"短按", "长按"};
    const char *values[] = {ai_action_label(app_settings_ai_short()), ai_action_label(app_settings_ai_long())};
    for (int i = 0; i < 2; ++i) {
        EpdRect box = {36, 330 + i * 116, 612, 96};
        settings_card(fb, box, 22, UI_GRAY_WHITE, 0x70);
        ui_text_vc(fb, 64, box.y + 48, 30, labels[i], EPD_DRAW_ALIGN_LEFT, false);
        ui_text_vc(fb, 594, box.y + 48, 22, values[i], EPD_DRAW_ALIGN_RIGHT, false);
        ui_draw_icon(fb, 625, box.y + 48, 26, UI_ICON_CHEVRON_RIGHT, UI_GRAY_BLACK);
    }
    settings_navigation(fb);
}
static void settings_render_ai_pick(uint8_t *fb) {
    back_header(fb, s_ai_pick_target ? "AI 键 · 长按" : "AI 键 · 短按");
    section(fb, 248, "选择功能");
    app_ai_action_t cur = s_ai_pick_target ? app_settings_ai_long() : app_settings_ai_short();
    for (int i = 0; i < s_ai_pick_count; ++i) {
        EpdRect box = {36, 298 + i * 116, 612, 90};
        bool active = cur == (app_ai_action_t)i;
        settings_card(fb, box, 20, UI_GRAY_WHITE, active ? UI_GRAY_BLACK : 0x70);
        ui_text_vc(fb, 64, box.y + 45, 28, ai_action_label((app_ai_action_t)i), EPD_DRAW_ALIGN_LEFT, false);
        if (active) epd_fill_circle(605, box.y + 45, 8, UI_GRAY_BLACK, fb);
    }
    settings_navigation(fb);
}
#endif

static void render_content(app_ctx_t *ctx, uint8_t *fb) {
#if KIIKO_BOARD_METALIO47
    if(s_page==SETTINGS_ONLINE_FONTS){ui_online_fonts_draw(&s_online_fonts,fb);return;}
    if(s_page==SETTINGS_VOICE){voice_render_page(fb);return;}
    if(s_page==SETTINGS_VOICE_LEARN){voice_render_learn(fb);return;}
    if(s_page==SETTINGS_FRONTLIGHT) {
        // 只放光相关项：亮度、暖光、开关、亮度上限。触摸振动已移到"个性化"。
        // Light-only rows: brightness, warmth, on/off and the cap. Touch haptics moved to
        // "personalisation" so a light page never carries a motor setting.
        epd_fill_rect((EpdRect){0,0,684,1216},UI_GRAY_WHITE,fb); back_header(fb,"前光");
        char value[48]; snprintf(value,sizeof(value),"%.1f%%",metalio_frontlight_position(false)*100.0f/METALIO_FRONTLIGHT_RANGE);
        row(fb,300,"亮度",value); row(fb,390,"降低 10%","增加 10%");
        snprintf(value,sizeof(value),"%u%%",metalio_warmth()); row(fb,530,"暖光比例",value);
        row(fb,620,"冷光","暖光"); row(fb,750,"前光",metalio_frontlight_enabled()?"开启":"关闭");
        snprintf(value,sizeof(value),"%u%%", app_settings_brightness_max()*100/240);
        row(fb,850,"亮度上限",value); row(fb,940,"降低 5%","增加 5%");
        settings_navigation(fb); return;
    }
#else
#endif
    (void)ctx;
    if (s_page == SETTINGS_PIN_ENTRY) { ui_pinpad_paint(fb, &s_pinpad, ui_pinpad_full()); return; }
    ui_clear_page(fb);
    if (s_page == SETTINGS_FACTORY_RESET) {
        ui_nav_status(fb);
        back_header(fb,"恢复出厂设置");
        ui_text_box(fb,(EpdRect){54,260,576,170},26,
            "重置系统偏好、资料卡、WiFi、蓝牙配对和锁屏密码。",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_box(fb,(EpdRect){54,450,576,170},23,
            "保留书籍、字体、图片、阅读记录和硬件校准。",EPD_DRAW_ALIGN_LEFT,false);
        if(s_reset_confirm)ui_text_box(fb,(EpdRect){54,645,576,80},23,"确认后将恢复默认设置并重启。",EPD_DRAW_ALIGN_LEFT,false);
        if(s_reset_notice[0])ui_text_box(fb,(EpdRect){54,735,576,90},21,s_reset_notice,EPD_DRAW_ALIGN_LEFT,false);
        ui_draw_button(fb,(EpdRect){54,850,576,78},s_reset_pending?"正在恢复…":s_reset_confirm?"确认恢复并重启":"恢复出厂设置",true);
        ui_draw_button(fb,(EpdRect){54,950,576,78},"取消",false);
        settings_navigation(fb);return;
    }

    // 保留 ui_clear_page 的白底，灰阶只由头像、卡片与字形生成。
    // Keep ui_clear_page's white background; avatars, cards and glyphs alone generate grays.
    ui_nav_status(fb);
    if (settings_group_page(s_page)) { settings_group_render(fb); return; }
#if KIIKO_BOARD_METALIO47
    if (s_page == SETTINGS_AI_KEY) { settings_render_ai_key(fb); return; }
    if (s_page == SETTINGS_AI_PICK) { settings_render_ai_pick(fb); return; }
#endif
#if KIIKO_BOARD_METALIO47
    if (s_page == SETTINGS_SD_RECOVERY) {
        back_header(fb,"内存卡恢复");
        ui_text_fixed(fb,54,270,26,"重新检测内存卡与导入字体",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_fixed(fb,54,325,23,"依次尝试降频、单线等兼容方式",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_fixed(fb,54,380,23,"不格式化，不清除书籍和设置",EPD_DRAW_ALIGN_LEFT,false);
        app_sd_recovery_status_t status=app_sd_recovery_status();
        const char *message="挂卡失败或字体未显示时，可手动重试";
        switch(status.phase) {
            case APP_SD_RECOVERY_PREPARE: message="正在释放内存卡资源…";break;
            case APP_SD_RECOVERY_WAIT: message="正在等待存储任务结束…";break;
            case APP_SD_RECOVERY_MOUNT: message="正在重新挂载，请勿拔卡…";break;
            case APP_SD_RECOVERY_FONTS: message="已挂载，正在重新识别字体…";break;
            case APP_SD_RECOVERY_DONE: message="内存卡已恢复挂载";break;
            case APP_SD_RECOVERY_FAILED: message="恢复未完成，可稍后重试";break;
            default: break;
        }
        ui_text_fixed(fb,54,510,25,message,EPD_DRAW_ALIGN_LEFT,false);
        if(status.phase==APP_SD_RECOVERY_DONE) {
            char fonts[64];snprintf(fonts,sizeof(fonts),"已识别 %d 款导入字体",status.fonts);
            ui_text_fixed(fb,54,575,24,fonts,EPD_DRAW_ALIGN_LEFT,false);
            if(!status.selected_font_ready)
                ui_text_fixed(fb,54,635,22,"原选字体未能加载，请重新选择",EPD_DRAW_ALIGN_LEFT,false);
        } else if(status.phase==APP_SD_RECOVERY_FAILED) {
            ui_text_fixed(fb,54,575,21,esp_err_to_name(status.error),EPD_DRAW_ALIGN_LEFT,false);
            ui_text_fixed(fb,54,635,22,"请检查卡是否插好，或更换内存卡",EPD_DRAW_ALIGN_LEFT,false);
        }
        if(s_sd_notice[0])ui_text_box(fb,(EpdRect){54,710,576,110},22,s_sd_notice,EPD_DRAW_ALIGN_LEFT,false);
        ui_draw_button(fb,sd_recovery_button(),app_sd_recovery_busy()?"正在恢复…":"开始恢复",false);
        settings_navigation(fb);return;
    }
#endif
#if KIIKO_BOARD_METALIO47
#endif
    if (s_page == SETTINGS_LOCK_PIN) {
        back_header(fb, "锁屏密码");
        row(fb, 266, "四位数字密码", !lock_pin_available() ? "暂不可用" : lock_pin_enabled() ? "已开启" : "已关闭");
        if (!lock_pin_available()) ui_text_vc(fb, 342, 408, 24, "锁屏设置暂不可用", EPD_DRAW_ALIGN_CENTER, false);
        else if (lock_pin_enabled()) {
            row(fb, 406, "修改密码", "验证原密码  ›");
            row(fb, 512, "关闭密码", "验证原密码  ›");
#if KIIKO_BOARD_METALIO47
            const unsigned grace=app_settings_pin_grace_code();
            const char *text=grace==1?"5 分钟":grace==2?"1 小时":grace==3?"8 小时":grace==4?"24 小时":"关闭";
            row(fb,pin_grace_row().y+12,"解锁宽限期",text);
#endif
        } else row(fb, 406, "开启密码", "设置四位密码  ›");
#if KIIKO_BOARD_METALIO47
        if(s_notice[0])ui_text_vc(fb,342,730,23,s_notice,EPD_DRAW_ALIGN_CENTER,false);
        ui_text_vc(fb,342,800,21,"修改和关闭密码需验证原密码",EPD_DRAW_ALIGN_CENTER,false);
#else
        if (s_notice[0]) ui_text_vc(fb, 342, 680, 23, s_notice, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 752, 21, "修改和关闭密码需验证原密码", EPD_DRAW_ALIGN_CENTER, false);
#endif
        settings_navigation(fb); return;
    }
    if (s_page == SETTINGS_PROFILE) {
        back_header(fb, "个人资料");
        ui_text(fb, 42, 214, 21, "自定义名称与头像", EPD_DRAW_ALIGN_LEFT, false);
        row(fb, 266, "设备名称", app_settings_device_name());
        row(fb, 366, "更换头像", app_settings_avatar_path()[0] ? "已选择图片  ›" : "默认图标  ›");
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_AVATAR) {
        back_header(fb, "选择头像");
        section(fb, 232, "TF 卡 pictures 文件夹中的 JPG / PNG 图片");
        row(fb, 270, "使用默认图标", app_settings_avatar_path()[0] ? "选择  ›" : "当前  ✓");
        if (!s_wallpaper_count) ui_text(fb, 52, 384, 23, "未找到图片，请放入 pictures 文件夹", EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < 7; ++i) {
            int index = s_wallpaper_page * 7 + i;
            if (index >= s_wallpaper_count) break;
            EpdRect box = {36, 374 + i * 96, 612, 80};
            bool active = !strcmp(s_wallpapers[index].path, app_settings_avatar_path());
            settings_card(fb, box, 17, UI_GRAY_WHITE, active ? UI_GRAY_BLACK : 0x70);
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[index].name);
            fit_value(name, 490);
            ui_text_vc(fb, 60, box.y + 40, 24, name, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(611, box.y + 40, 7, UI_GRAY_BLACK, fb);
        }
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_TEXT_EDIT) {
        back_header(fb, s_editor_signature ? "状态栏签名" : "设备名称");
        ui_text(fb, 642, 95, 24, "完成", EPD_DRAW_ALIGN_RIGHT, false);
        ui_text(fb, 36, 201, 21, s_editor_signature ? "状态栏中间显示，留空则隐藏" : "显示在设置页的资料卡", EPD_DRAW_ALIGN_LEFT, false);
        ui_text_input_draw(fb, &s_editor_input, (EpdRect){36, 243, 612, 82}, 28, false, NULL);
        if (s_editor_notice[0]) ui_text_fixed_vc(fb, 36, 510, 22, s_editor_notice, EPD_DRAW_ALIGN_LEFT, false);
        ui_keyboard_draw(fb, UI_SYSTEM_KEYBOARD_TOP);
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_SHELF_STYLE) {
        static const char *const styles[] = {"深色书轨", "亚克力书架", "液态玻璃书架", "列表书架", "浅色书架"};
        static const char *const descriptions[] = {"每页 9 本", "每页 9 本", "薄玻璃挡条 · 每页 9 本",
                                                   "作者与进度 · 每页 4 本", "淡色书本 · 无层板黑条"};
        for (int i = 0; i < SHELF_STYLE_CHOICES; ++i) {
            int y = 263 + i * 253 - s_style_scroll;
            if (y + 230 < 242 || y >= settings_viewport_end()) continue;
            EpdRect card = {36, y, 612, 230};
            int style = s_shelf_style_ids[i];
            bool active = app_settings_shelf_style() == style;
            settings_card(fb, card, 24, UI_GRAY_WHITE,
                          active ? UI_GRAY_BLACK : 0x70);
            draw_style_thumbnail(fb, style, y + 11);
            if (y + 188 >= 242 && y + 222 < settings_viewport_end()) ui_text_box(fb, (EpdRect){68,y+180,230,42}, 23, styles[i], EPD_DRAW_ALIGN_LEFT, false);
            if (y + 192 >= 242 && y + 220 < settings_viewport_end()) ui_text_box(fb, (EpdRect){310,y+184,272,36}, 17, descriptions[i], EPD_DRAW_ALIGN_LEFT, false);
            if (y + 194 >= 242 && y + 220 < settings_viewport_end()) {
                epd_draw_circle(609, y + 207, 13, UI_GRAY_BLACK, fb);
                if (active) epd_fill_circle(609, y + 207, 6, UI_GRAY_BLACK, fb);
            }
        }
        ui_fill_rect_fast((EpdRect){0,0,UI_LOCK_WIDTH,242},UI_GRAY_WHITE,fb);
        ui_fill_rect_fast((EpdRect){0,settings_viewport_end(),UI_LOCK_WIDTH,UI_LOCK_HEIGHT-settings_viewport_end()},UI_GRAY_WHITE,fb);
        ui_nav_status(fb);ui_nav_back(fb,36,79);
        ui_text_vc(fb,342,107,34,"书架样式",EPD_DRAW_ALIGN_CENTER,false);
        ui_text(fb,36,207,23,"选择书架外观与排列方式",EPD_DRAW_ALIGN_LEFT,false);
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_TIME_EDIT) {
        ui_nav_back(fb, 36, 79);
        ui_text(fb, 36, 151, 47, "日期与时间", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 36, 216, 23, "北京时间 · UTC+8", EPD_DRAW_ALIGN_LEFT, false);
        ui_hairline(fb, 259, 36, 612, UI_GRAY_LIGHT);
        static const char *const labels[] = {"年", "月", "日", "时", "分"};
        int values[] = {s_year, s_month, s_day, s_hour, s_minute};
        for (int i = 0; i < 5; ++i) {
            int x = 36 + i * 122;
            ui_text(fb, x + 52, 300, 24, labels[i], EPD_DRAW_ALIGN_CENTER, false);
            ui_draw_control_frame(fb, (EpdRect){x, 354, 106, 62}, 8, UI_GRAY_BLACK);
            ui_text_vc(fb, x + 53, 385, 30, "+", EPD_DRAW_ALIGN_CENTER, false);
            char value[8]; snprintf(value, sizeof(value), i == 0 ? "%04d" : "%02d", values[i]);
            ui_text(fb, x + 53, 456, i == 0 ? 34 : 39, value, EPD_DRAW_ALIGN_CENTER, false);
            ui_draw_control_frame(fb, (EpdRect){x, 535, 106, 62}, 8, UI_GRAY_BLACK);
            ui_text_vc(fb, x + 53, 566, 30, "−", EPD_DRAW_ALIGN_CENTER, false);
        }
        ui_text(fb, 36, 649, 23, "点上方 + 或下方 − 调整数字", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){36, 767, 612, 78}, "保存时间", false);
        ui_draw_button(fb, (EpdRect){36, 874, 612, 78}, "取消", false);
        if (s_notice[0]) ui_text(fb, 36, 996, 22, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    if (s_page == SETTINGS_WIFI) {
        back_header(fb, "WiFi");
        section(fb, 253, "已保存网络");
        row(fb, 296, s_main_wifi_saved ? s_main_wifi : "尚未配置", s_main_wifi_saved ? "已保存" : "");
        row(fb, 408, "连接或更改网络", "前往传输  ›");
        ui_text(fb, 54, 529, 21, "配网后可在日期与时间中进行 WiFi 对时。", EPD_DRAW_ALIGN_LEFT, false);
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_FONT) {
        back_header(fb, "系统字体");
#if KIIKO_BOARD_METALIO47
        ui_online_fonts_draw_entry(fb,79);
#endif
        system_font_layout_t l = system_font_layout();
        settings_card(fb,l.size,22,UI_GRAY_WHITE,0x70);
        char size[16]; snprintf(size,sizeof(size),"%u%%",app_settings_system_font_size());
#if KIIKO_BOARD_METALIO47
        ui_text_vc(fb,60,231,27,"系统字号",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,589,231,27,size,EPD_DRAW_ALIGN_RIGHT,false);
        ui_text_vc(fb,60,285,18,"轻点切换，最大 200%",EPD_DRAW_ALIGN_LEFT,false);
        ui_draw_icon(fb,619,231,28,UI_ICON_ROTATE_CW,0x38);
#else
        if (settings_stacked()) {
            ui_text_vc(fb,60,247,27,"系统字号",EPD_DRAW_ALIGN_LEFT,false);
            ui_text_vc(fb,589,247,27,size,EPD_DRAW_ALIGN_RIGHT,false);
            ui_text_vc(fb,60,328,18,"轻点切换，最大 200%",EPD_DRAW_ALIGN_LEFT,false);
        } else {
            ui_text_vc(fb,58,227,25,"系统字号",EPD_DRAW_ALIGN_LEFT,false);
            ui_text_vc(fb,58,269,18,"轻点切换，最大 200%",EPD_DRAW_ALIGN_LEFT,false);
            ui_text_vc(fb,589,247,26,size,EPD_DRAW_ALIGN_RIGHT,false);
        }
        ui_draw_icon(fb,619,247,28,UI_ICON_ROTATE_CW,0x38);
#endif
        section(fb,l.preview_title,"效果预览");
        settings_card(fb,l.preview,22,UI_GRAY_WHITE,0x70);
#if KIIKO_BOARD_METALIO47
        ui_text_vc(fb,60,l.preview.y+43,29,"阅读，让时间慢下来",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,60,l.preview.y+112,25,"Aa 123",EPD_DRAW_ALIGN_LEFT,false);
#else
        ui_text_vc(fb,60,l.preview.y+(settings_stacked()?68:51),29,"阅读，让时间慢下来",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,60,l.preview.y+(settings_stacked()?151:107),25,"Aa 123",EPD_DRAW_ALIGN_LEFT,false);
#endif
        section(fb,l.font_title,"选择字体");
        const char *selected = app_settings_system_font_path();
        int count = ttf_font_count(), first = s_font_page*l.slots;
#if KIIKO_BOARD_METALIO47
        char total[32];snprintf(total,sizeof(total),"共 %d 款",count+1);
        ui_text_vc(fb,648,l.font_title+ui_text_effective_px(22)/2,18,total,EPD_DRAW_ALIGN_RIGHT,false);
#endif
        for (int slot=0;slot<l.slots && first+slot<=count;++slot) {
            int index=first+slot;
            const ttf_font_item_t *item=index?ttf_font_item(index-1):NULL;
            bool active=item?!strcmp(selected,item->path):!selected[0];
            EpdRect box=system_font_item_rect(slot);
            settings_card(fb,box,20,UI_GRAY_WHITE,active?UI_GRAY_BLACK:0x70);
            char label[TTF_FONT_NAME_MAX];
            snprintf(label,sizeof(label),"%s",item?system_font_label(item->path):"思源黑体（内建）");
            settings_fit_text(label,26,box.width-100);
#if KIIKO_BOARD_METALIO47
            ui_text_box(fb,(EpdRect){60,box.y+8,box.width-100,box.height-16},26,label,EPD_DRAW_ALIGN_LEFT,false);
#else
            ui_text_vc(fb,60,box.y+box.height/2,26,label,EPD_DRAW_ALIGN_LEFT,false);
#endif
            if (active) epd_fill_circle(613,box.y+box.height/2,6,UI_GRAY_BLACK,fb);
        }
#if KIIKO_BOARD_METALIO47
        char pages[32];
#else
        char pages[24];
#endif
        snprintf(pages,sizeof(pages),"%d / %d",s_font_page+1,(count+l.slots)/l.slots);
#if KIIKO_BOARD_METALIO47
        int page_count=(count+l.slots)/l.slots;
        for(int next=0;next<2;++next) {
            EpdRect r=system_font_page_button(next);
            bool enabled=next?s_font_page+1<page_count:s_font_page>0;
            settings_card(fb,r,18,UI_GRAY_WHITE,enabled?0x50:0xc0);
            ui_draw_icon(fb,r.x+r.width/2,r.y+r.height/2,28,
                next?UI_ICON_CHEVRON_RIGHT:UI_ICON_CHEVRON_LEFT,enabled?UI_GRAY_BLACK:0xc0);
        }
        if(s_notice[0])ui_text_box(fb,(EpdRect){190,settings_viewport_end()-66,304,48},18,s_notice,EPD_DRAW_ALIGN_CENTER,false);
        else ui_text_vc(fb,342,settings_viewport_end()-42,20,pages,EPD_DRAW_ALIGN_CENTER,false);
#else
        ui_text_fixed_vc(fb,648,settings_viewport_end()-16,18,pages,EPD_DRAW_ALIGN_RIGHT,false);
        if (s_notice[0]) ui_text_fixed_vc(fb,42,settings_viewport_end()-16,18,s_notice,EPD_DRAW_ALIGN_LEFT,false);
#endif
        settings_navigation(fb);
        if (s_font_sync_confirm) draw_system_font_sync(fb);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_SIZE) {
        back_header(fb, "系统字号");
        section(fb, 248, "界面文字大小");
        for (int i = 0; i < 11; ++i) {
            int value = 100 + i * 10;
            EpdRect box = {36 + (i % 2) * 316, 298 + (i / 2) * 122, 296, 102};
            bool active = app_settings_system_font_size() == value;
            settings_card(fb, box, 20, UI_GRAY_WHITE, active ? UI_GRAY_BLACK : 0x70);
            char label[48]; snprintf(label, sizeof(label), "%d%%", value);
            ui_text_vc(fb, box.x + 24, box.y + 51, 29, label, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(box.x + box.width - 25, box.y + 51, 8, UI_GRAY_BLACK, fb);
        }
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_MAIN_REFRESH) {
        back_header(fb, "主页刷新模式");
        section(fb, 248, "首页 · 书架 · 文件管理 · 设置");
        const char* names[] = {"普通", "快刷", "水波纹"};
        const char* details[] = {"保留灰阶层次，兼顾画面清晰度", "黑白快刷，亚克力用细棋盘格", "以水波纹切换页面，保留灰阶"};
        for (int i = 0; i < 3; ++i) {
            const EpdRect box = {36, 298 + i * 158, 612, 132};
            const bool active = app_settings_main_refresh_mode() == (app_main_refresh_mode_t)i;
            settings_card(fb, box, 22, UI_GRAY_WHITE, active ? UI_GRAY_BLACK : 0x58);
            ui_text_vc(fb, 64, box.y + 38, 30, names[i], EPD_DRAW_ALIGN_LEFT, false);
            char detail[128]; snprintf(detail, sizeof(detail), "%s", details[i]); fit_value(detail, 498);
            ui_text_vc(fb, 64, box.y + 89, 22, detail, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(607, box.y + 38, 8, UI_GRAY_BLACK, fb);
        }
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_CONTRAST) {
        back_header(fb, "系统对比度");
        section(fb, 248, "调整系统界面的灰阶层次与清晰度");
        static const char *const names[] = {"柔和", "标准", "清晰", "鲜明", "高对比"};
        for (int i = 0; i < 5; ++i) {
            int value = 100 + i * 10;
            EpdRect box = {36, 298 + i * 116, 612, 90};
            bool active = app_settings_system_contrast() == value;
            settings_card(fb, box, 20, UI_GRAY_WHITE, active ? UI_GRAY_BLACK : 0x70);
            ui_text_vc(fb, 64, box.y + 45, 28, names[i], EPD_DRAW_ALIGN_LEFT, false);
            char shown[16]; snprintf(shown, sizeof(shown), "%d%%", value);
            ui_text_vc(fb, 570, box.y + 45, 22, shown, EPD_DRAW_ALIGN_RIGHT, false);
            if (active) epd_fill_circle(605, box.y + 45, 8, UI_GRAY_BLACK, fb);
        }
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_LOCK_STYLE) {
        back_header(fb, "锁屏样式");
        section(fb, 248, "选择电源键锁屏后的画面");
        const char *labels[] = {"壁纸锁屏", "阅读票根", "书架拼贴", "透明 PNG 书封"};
        const char *details[] = {"使用 TF 卡中的 JPG / PNG 图片", "书封、进度和阅读记录", "真实封面 · 名称与数量 · 系统字体", "当前页面作底 · 白底 PNG 自动去白"};
        const uint8_t values[] = {1, 0, 2, 3};
        for (int i = 0; i < 4; ++i) {
            EpdRect box = {36, 300 + i * 154, 612, 132};
            bool active = app_settings_lock_style() == values[i];
            settings_card(fb, box, 22, UI_GRAY_WHITE, active ? UI_GRAY_BLACK : 0x70);
            ui_text(fb, 64, box.y + 23, 31, labels[i], EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 64, box.y + 79, 20, details[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 66, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb,54,966,21,"透明书封放入 pictures · 推荐 684 × 1216",EPD_DRAW_ALIGN_LEFT,false);
        if (app_settings_wallpaper_path()[0])
            row(fb, 1010, "当前壁纸", strrchr(app_settings_wallpaper_path(), '/') + 1);
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_WALLPAPER) {
        back_header(fb,s_bookmark_picker?"选择透明书封":"选择壁纸");
        section(fb,244,s_bookmark_picker?"pictures · 透明 PNG · 保留整张画布":"TF 卡图片 · JPG / PNG · 不超过 50 MB");
        if (!s_wallpaper_count)
            ui_text(fb, 54, 325, 25, "未找到图片，请放入 TF 卡的 pictures 文件夹", EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < 8; ++i) {
            int index = s_wallpaper_page * 8 + i;
            if (index >= s_wallpaper_count) break;
            EpdRect box = {36, 292 + i * 92, 612, 78};
            bool active = !strcmp(s_wallpapers[index].path,s_bookmark_picker?app_settings_bookmark_path():app_settings_wallpaper_path());
            settings_card(fb, box, 18, UI_GRAY_WHITE, active ? UI_GRAY_BLACK : 0x70);
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[index].name);
            fit_value(name, 490);
            ui_text_vc(fb, 60, box.y + 39, 24, name, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 39, 6, UI_GRAY_BLACK, fb);
        }
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_WALLPAPER_PREVIEW) {
        back_header(fb,s_bookmark_picker?"预览透明书封":"预览壁纸");
        EpdRect image = {166, 194, 352, 626};
        ui_fill_round_rect(fb, image, 8, UI_GRAY_WHITE);
        s_wallpaper_preview_ok = s_wallpaper_selected >= 0 && s_wallpaper_selected < s_wallpaper_count &&
            (s_bookmark_picker ? s_bookmark_preview != NULL : ui_wallpaper_draw(fb, s_wallpapers[s_wallpaper_selected].path, image));
        if(s_bookmark_picker && s_bookmark_preview)book_lock_bookmark_preview_paint(fb,s_bookmark_preview,image);
        ui_draw_round_rect(fb, image, 8, 0x70);
        if (!s_wallpaper_preview_ok)
            ui_text_vc(fb, 342, 507, 23, s_bookmark_picker?"PNG无法加载，请更换图片":"图片无法预览", EPD_DRAW_ALIGN_CENTER, false);
        if (s_wallpaper_selected >= 0 && s_wallpaper_selected < s_wallpaper_count) {
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[s_wallpaper_selected].name);
            fit_value(name, 570);
            ui_text_vc(fb, 342, 859, 24, name, EPD_DRAW_ALIGN_CENTER, false);
            char position[40];
            snprintf(position, sizeof(position), "%d / %d · 上下滑动切换", s_wallpaper_selected + 1, s_wallpaper_count);
            ui_text_vc(fb, 342, 908, 20, position, EPD_DRAW_ALIGN_CENTER, false);
        }
        if (s_wallpaper_preview_ok) {
            EpdRect button = {94, 962, 496, 70};
            ui_fill_round_rect(fb, button, 22, UI_GRAY_WHITE); ui_draw_control_frame(fb, button, 22, 0x30);
            ui_text_vc(fb, 342, 997, 25, s_bookmark_picker?"使用透明书封":"设为锁屏壁纸", EPD_DRAW_ALIGN_CENTER, false);
        }
        if (s_wallpaper_confirm) {
            EpdRect dialog = {68, 435, 548, 306};
            ui_fill_round_rect(fb, dialog, 26, UI_GRAY_WHITE);
            ui_draw_control_frame(fb, dialog, 26, 0x50);
            ui_text_vc(fb, 342, 503, 31, s_bookmark_picker?"开启透明书封锁屏？":"设为壁纸锁屏？", EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 342, 565, 20, s_bookmark_picker?"以锁屏前当前页面为背景":"确认后，短按电源键将显示此图片", EPD_DRAW_ALIGN_CENTER, false);
            EpdRect cancel = {94, 643, 232, 68}, confirm = {358, 643, 232, 68};
            ui_fill_round_rect(fb, cancel, 18, UI_GRAY_WHITE);
            ui_draw_control_frame(fb, cancel, 18, 0x50);
            ui_fill_round_rect(fb, confirm, 18, UI_GRAY_WHITE); ui_draw_control_frame(fb, confirm, 18, 0x30);
            ui_text_vc(fb, 210, 677, 24, "取消", EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 474, 677, 24, "确认", EPD_DRAW_ALIGN_CENTER, false);
        }
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_TIME) {
        back_header(fb, "日期与时间");
        time_t now = time(NULL);
        char shown[64] = "未设置";
        if (now >= 1704067200) {
            struct tm tm; time_t local = now + 8 * 3600;
            if (gmtime_r(&local, &tm)) snprintf(shown, sizeof(shown), "%04d-%02d-%02d  %02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        }
        section(fb, 253, "当前时间 · 北京时间");
        row(fb, 296, shown, "");
        row(fb, 407, "通过 WiFi 对时", "立即同步  ›");
        row(fb, 518, "手动设置", "编辑  ›");
        ui_text(fb, 54, 656, 21, "对时成功后由内部时钟持续走时；无需保持联网。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, 694, 21, "长时间离线可能产生误差，建议定期重新对时。", EPD_DRAW_ALIGN_LEFT, false);
        if (s_notice[0]) ui_text(fb, 54, 772, 22, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_AUTO_LOCK) {
        back_header(fb, "自动休眠锁屏");
        section(fb, 228, "无操作后自动锁屏");
        const char *labels[] = {"1 分钟", "5 分钟", "10 分钟", "关闭"};
        const uint8_t values[] = {1, 5, 10, 0};
        for (int i = 0; i < 4; ++i) {
            EpdRect box = {36, 272 + i * 112, 612, 92};
            bool active = app_settings_auto_lock_minutes() == values[i];
            settings_card(fb, box, 20, UI_GRAY_WHITE, active ? UI_GRAY_BLACK : 0x70);
            ui_text_vc(fb, 64, box.y + box.height / 2, 30, labels[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + box.height / 2, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 48, 760, 23, "操作后重新计时，传输和升级期间暂停", EPD_DRAW_ALIGN_LEFT, false);
#if KIIKO_BOARD_METALIO47
        ui_text(fb, 48, 810, 23, "省电选项见“关机睡眠”", EPD_DRAW_ALIGN_LEFT, false);
#else
        ui_text(fb, 48, 810, 23, "锁屏先浅睡，10 分钟后进入深睡", EPD_DRAW_ALIGN_LEFT, false);
#endif
        settings_navigation(fb);
        return;
    }
#if KIIKO_BOARD_METALIO47
    if (s_page == SETTINGS_POWER_SLEEP) {
        back_header(fb, "关机睡眠");
        section(fb, 232, "锁屏后多久进入深睡（都是一次按键唤醒）");
        // 六档，顺序见 k_sleep_off_order：立即 / 1 / 10 / 30 / 60 分钟 / 从不。
        // 六行把注释整体下移，仍落在 UI_NAV_TOP(1096) 之上。
        // Cards carry the duration only; the subtext line was noise, so it is gone.
        for (int i = 0; i < SLEEP_OFF_ROWS; ++i) {
            EpdRect box = {36, 272 + i * 96, 612, 84};
            bool active = app_settings_sleep_off_code() == k_sleep_off_order[i];
            settings_card(fb, box, 20, UI_GRAY_WHITE, active ? UI_GRAY_BLACK : 0x70);
            ui_text(fb, 64, box.y + 29, 24, k_sleep_off_labels[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 42, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 54, 886, 18, "深睡前保存进度并同步 RTC", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, 922, 18, "USB 连接时暂缓自动休眠", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, 958, 18, "电源菜单的关机同样是深睡，短按即可唤醒", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, 994, 18, "设置了锁屏密码时仍需验证", EPD_DRAW_ALIGN_LEFT, false);
        settings_navigation(fb);
        return;
    }
#else
    if (s_page == SETTINGS_POWER_SLEEP) {
        back_header(fb, "关机睡眠");
        section(fb, 236, "选择电源菜单中“关机”的方式");
        const char *shutdown_labels[] = {"彻底关机", "先浅睡，10 分钟后深睡"};
        const char *shutdown_details[] = {"完全断电，长按电源键开机",
                                          "锁屏画面保留；深睡后短按电源键开机"};
        for (int i = 0; i < 2; ++i) {
            int y = 278 + i * 136;
            bool active = app_settings_staged_shutdown() == (i == 1);
            settings_card(fb, (EpdRect){36, y, 612, 124}, 22, UI_GRAY_WHITE, active ? UI_GRAY_BLACK : 0x70);
            ui_text(fb, 64, y + 20, 29, shutdown_labels[i], EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 64, y + 72, 20, shutdown_details[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, y + 62, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 54, 558, 21, "浅睡时按键直接返回，深睡时会重新开机。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, 594, 21, "锁屏样式同时适用于壁纸与阅读票根。", EPD_DRAW_ALIGN_LEFT, false);
        settings_navigation(fb);
        return;
    }
#endif
    if (s_page == SETTINGS_BOOT) {
        back_header(fb, "电脑刷机");
        settings_card(fb, (EpdRect){36, 254, 612, 298}, 22, UI_GRAY_WHITE, 0x70);
#if KIIKO_BOARD_METALIO47
        ui_text(fb, 60, 288, 29, "准备 BOOT 下载口", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 350, 22, "用于本机 S31 固件更新或故障恢复", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){60, 438, 564, 78}, s_boot_pending ? "正在准备" :
            app_download_active() ? "重新恢复下载口" : "恢复下载口", false);
        ui_text(fb, 48, 620, 22, s_boot_notice[0] ? s_boot_notice :
            app_download_active() ? (s_boot_connected ? "USB 已连接，请在电脑点击“检查连接”" :
                "等待 USB 连接，可点击上方按钮重新检测") :
            "连接 USB 后准备下载口，再到电脑检查连接", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 670, 20, "电脑将触发下载模式；等待时可返回退出。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 714, 20, "电脑确认 S31 身份后，才表示进入 ROM。", EPD_DRAW_ALIGN_LEFT, false);
#else
        ui_text(fb, 60, 288, 29, "进入 BOOT 模式", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 350, 22, "用于官网完整刷机或故障恢复", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){60, 438, 564, 78}, s_boot_pending ? "正在进入" : "进入 BOOT 模式", false);
        ui_text(fb, 48, 620, 22, "请用 USB 连接电脑，再打开官网刷机页", EPD_DRAW_ALIGN_LEFT, false);
#endif
        settings_navigation(fb); return;
    }
    if (s_page == SETTINGS_UPGRADE) {
        back_header(fb, "系统升级");
        settings_card(fb, (EpdRect){36, 202, 612, 120}, 22, UI_GRAY_WHITE, 0x70);
        ui_text(fb, 60, 224, 22, "当前版本", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 264, 29, esp_app_get_description()->version, EPD_DRAW_ALIGN_LEFT, false);
        if (s_upgrade_confirm) {
            settings_card(fb, (EpdRect){36, 356, 612, s_upgrade_online ? 574 : 344}, 22, UI_GRAY_WHITE, 0x70);
            ui_text_fixed(fb, 60, 383, 31, s_upgrade_online ? "发现新版本" :
                s_local_update.same_version ? "重装 TF 卡固件" : "确认 TF 卡升级", EPD_DRAW_ALIGN_LEFT, false);
            ui_text_fixed(fb, 60, 447, 28, s_upgrade_online ? s_update.release.version : s_local_update.candidate_version,
                    EPD_DRAW_ALIGN_LEFT, false);
            if (s_upgrade_online) {
                ui_text_fixed(fb, 60, 494, 26, "本次更新", EPD_DRAW_ALIGN_LEFT, false);
                draw_upgrade_notes(fb);
            }
            ui_text_fixed(fb, 60, s_upgrade_online ? 770 : 513, 24, "升级期间请保持供电", EPD_DRAW_ALIGN_LEFT, false);
            ui_text_fixed(fb, 60, s_upgrade_online ? 806 : 560, 23, "设置、阅读记录与 TF 卡文件保留", EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_button(fb, upgrade_confirm_button(s_upgrade_online, false), s_upgrade_online ? "稍后" : "取消", false);
            ui_draw_button(fb, upgrade_confirm_button(s_upgrade_online, true), s_upgrade_online ? "开始更新" : "确认升级", true);
        } else {
            settings_card(fb, (EpdRect){36, 356, 612, 278}, 22, UI_GRAY_WHITE, 0x70);
            ui_text(fb, 60, 383, 29, "联网 OTA", EPD_DRAW_ALIGN_LEFT, false);
            char detail[96];
#if KIIKO_BOARD_METALIO47
            snprintf(detail, sizeof(detail), "%s", s_update.message[0] ? s_update.message : "连接 WiFi，优先检查 GitHub 新版本");
#else
            snprintf(detail, sizeof(detail), "%s", s_update.message[0] ? s_update.message : "连接 WiFi，检查官网发布的新版本");
#endif
            fit_value(detail, 540);
            ui_text(fb, 60, 436, 21, detail, EPD_DRAW_ALIGN_LEFT, false);
            if (s_update.state == PICO_UPDATE_AVAILABLE)
                ui_text(fb, 60, 477, 24, s_update.release.version, EPD_DRAW_ALIGN_LEFT, false);
            if (s_update.state == PICO_UPDATE_DOWNLOADING) draw_upgrade_progress(fb);
            ui_draw_button(fb, (EpdRect){60, 550, 564, 62}, s_update.busy ? "取消联网更新" :
                s_update.state == PICO_UPDATE_AVAILABLE ? "开始更新" : "检查更新", s_update.state == PICO_UPDATE_AVAILABLE);
            settings_card(fb, (EpdRect){36, 662, 612, 262}, 22, UI_GRAY_WHITE, 0x70);
            ui_text(fb, 60, 689, 29, "TF 卡升级", EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 60, 739, 23, "升级包放在 TF 卡根目录", EPD_DRAW_ALIGN_LEFT, false);
            char local_ver[80];
            if (!s_local_update.ready) {
#if KIIKO_BOARD_METALIO47
                snprintf(local_ver, sizeof(local_ver), "放入 Metalio-S31-update.bin");
#else
                snprintf(local_ver, sizeof(local_ver), "把官网升级包放入 TF 卡根目录");
#endif
            } else if (s_local_update.same_version)
                snprintf(local_ver, sizeof(local_ver), "%s（与当前相同）", s_local_update.candidate_version);
            else
                snprintf(local_ver, sizeof(local_ver), "%s", s_local_update.candidate_version);
            ui_text(fb, 60, 786, 21, local_ver, EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_button(fb, (EpdRect){60, 840, 564, 62}, s_local_pending ? "正在安装，请勿断电" :
                s_local_update.ready ? "安装 TF 卡升级包" : "重新检查 TF 卡", s_local_update.ready);
        }
        if (s_upgrade_notice[0]) ui_text(fb, 48, 957, 21, s_upgrade_notice, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 1010, 20, "首次安装或修复，请通过官网完整刷机", EPD_DRAW_ALIGN_LEFT, false);
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_CONFIG) {
        back_header(fb, "保存与恢复配置");
        section(fb, 248, "换机或刷机后，快速恢复个性化设置");
        settings_card(fb, (EpdRect){36, 299, 612, 197}, 22, UI_GRAY_WHITE, 0x70);
        ui_text(fb, 60, 326, 26, "TF 卡根目录", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 377, 23, "配置备份文件", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 435, 19, "文字排版、设备资料、阅读记录与书签", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){36, 561, 612, 83}, "保存当前配置到 TF 卡", false);
        ui_draw_button(fb, (EpdRect){36, 681, 612, 83},
                       s_config_confirm ? "再次点按，确认恢复配置" : "从 TF 卡恢复配置", false);
        if (s_notice[0]) ui_text(fb, 48, 819, 21, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 899, 19, "包含阅读记录、设备资料和已存 WiFi。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 939, 19, "书籍、字体和图片仍需留在 TF 卡。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 979, 19, "备份含 WiFi 密码，请妥善保管 TF 卡。", EPD_DRAW_ALIGN_LEFT, false);
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_BLE_SCAN) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, "扫描设备", EPD_DRAW_ALIGN_CENTER, false);
        const ble_scan_layout_t l = ble_scan_layout();
        const int scroll = s_ble_scroll;
        // 持续扫描中：告诉用户正在找，以及找到几台。
        // Continuous scan: say that it is running and how many turned up.
        char head[64];
        if (ble_pt_scanning()) snprintf(head, sizeof(head), "正在搜索…已找到 %u 台", (unsigned)ble_scan_visible_count());
        else snprintf(head, sizeof(head), "扫描已停止");
        ui_text(fb, 44, l.status_top - scroll, 23, head, EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < l.device_rows; ++i) {
            const ble_pt_device_t *dev = ble_scan_visible_at(i);
            if (!dev) continue;
            char label[48];
            snprintf(label, sizeof(label), "%s", dev->name);
            fit_value(label, 360);
            char value[44];
            snprintf(value, sizeof(value), "%d dBm%s%s", dev->rssi, dev->hid ? " · HID" : "",
                     dev->bonded ? " · 已配对" : "");
            int top = l.devices_top - scroll + i * BLE_ROW_H;
            if (top + BLE_ROW_H >= 190 && top < settings_viewport_end()) {
                settings_card(fb, (EpdRect){36, top, 612, BLE_ROW_H - 8}, 18, UI_GRAY_WHITE, 0x70);
                ui_text(fb, 60, top + 12, 25, label, EPD_DRAW_ALIGN_LEFT, false);
                ui_text(fb, 60, top + 45, 19, value, EPD_DRAW_ALIGN_LEFT, false);
                ui_draw_icon(fb,(624)-8,top + 38,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
            }
        }
        if (!l.device_rows)
            ui_text(fb, 44, l.devices_top - scroll + 18, 23, "附近还没有发现设备",
                    EPD_DRAW_ALIGN_LEFT, false);
        ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, 190}, UI_GRAY_WHITE, fb);
        ui_nav_status(fb);
        back_header(fb, "扫描设备");
        settings_navigation(fb);
        return;
    }
    if (s_page == SETTINGS_BLUETOOTH) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, "蓝牙", EPD_DRAW_ALIGN_CENTER, false);
        const ble_layout_t l = ble_layout();
#if KIIKO_BOARD_METALIO47
        // 耳机：状态行 + 右侧断开/停止/扫描按钮；扫描结果就地展开，点行连接。
        // Headset: a status row with disconnect/stop/scan at right; results expand in place, tap a row to connect.
        section(fb, l.hp_section_top, "耳机");
        // 「蓝牙音频」总开关：关掉给音频模组断电（区分于下方"自动连接"策略开关）。
        // The Bluetooth-audio master switch: off powers the module down (distinct from the auto-connect policy below).
        setting_toggle(fb, l.hp_bt_top, "蓝牙音频",
                       "关闭后模组断电，音乐将没有声音",
                       app_settings_bt_enabled());
        char hp_status[80];
        const bool hp_link = bt_audio_link_active(), hp_scan = bt_audio_scanning();
        // 链路在但音频通路没成立（模组还在 SCO 窄带上/发射使能丢了）：这正是「显示已连接、
        // 耳机没声音」的现场。值列只有 140 px，故障态优先显示诊断而不是耳机名。
        // Linked but the audio path never came up (module stuck on SCO / transmit enable lost)
        // is exactly "shows connected, headset silent". The value column is 140 px wide, so in
        // the fault state the diagnostic wins over the device name.
        const bool hp_ready = hp_link && bt_audio_audio_ready();
        if (hp_link && hp_ready) snprintf(hp_status, sizeof(hp_status), "已连接 %s", bt_audio_name());
        else if (hp_link) snprintf(hp_status, sizeof(hp_status), "已连接·无音频");
        else if (hp_scan) snprintf(hp_status, sizeof(hp_status), "搜索设备…");
        else snprintf(hp_status, sizeof(hp_status), "未连接");
        ble_row(fb, l.hp_status_top, "耳机", hp_status);
        ui_draw_button(fb, ble_button_rect(l.hp_status_top),
                       hp_link ? "断开" : hp_scan ? "停止" : "扫描", false);
        // 发声测试：绕过播放器与解码，直接给模组送 1 kHz。出声与否一刀切开「蓝牙/模组通路」
        // 和「播放器/解码」两类原因，正是「显示已连接却没声音」时最需要的那个判据。
        // Test tone: skip the player and decoder and feed the module 1 kHz directly. Audible or
        // not splits "Bluetooth/module path" from "player/decoder" — exactly the discriminator
        // needed for "shows connected yet silent".
        const bool tone_on = bt_audio_test_tone_active();
        ble_row(fb, l.hp_tone_top, "测试发声",
                tone_on ? "1 kHz 播放中…" : hp_link ? "播两秒 1 kHz" : "需先连耳机");
        ui_draw_button(fb, ble_button_rect(l.hp_tone_top), tone_on ? "停止" : "播放", tone_on);
        for (int i = 0; i < l.hp_dev_count; ++i) {
            const bt_audio_device_t *dev = bt_audio_device((uint8_t)i);
            if (!dev) continue;
            char label[40];
            snprintf(label, sizeof(label), "%s", dev->name[0] ? dev->name : "未知设备");
            fit_value(label, 300);
            ble_row(fb, l.hp_dev_top[i], label, "连接  ›");
        }
        if (l.hp_empty_top >= 0)
            ble_row(fb, l.hp_empty_top, hp_scan ? "正在搜索设备…" : "未发现音频设备", "");
        setting_toggle(fb, l.hp_auto_top, "自动连接上次设备",
                       "蓝牙开启后自动连接上次连过的耳机（断链即关蓝牙，需重新开启）",
                       app_settings_bt_auto_connect());
        // 已配对耳机：点行连接/断开，右侧按钮忘记；空表给一句引导。
        // Paired headsets: the row connects or disconnects, the button forgets; empty tables get a hint.
        section(fb, l.hp_pair_title_top, "已配对");
        uint8_t link_bda[6];
        const bool hp_live = bt_audio_link_addr(link_bda);
        if (!l.hp_pair_rows)
            ui_text(fb, 44, l.hp_pair_title_top + 50, 21, "还没有已配对设备 · 扫描连接后自动记住",
                    EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < l.hp_pair_rows; ++i) {
            uint8_t bda[6];
            char pname[36];
            if (!app_settings_bt_paired_get((uint8_t)i, bda, pname, sizeof(pname))) continue;
            char label[40];
            snprintf(label, sizeof(label), "%s", pname[0] ? pname : "未知设备");
            fit_value(label, 300);
            const bool live = hp_live && !memcmp(link_bda, bda, 6);
            ble_row(fb, l.hp_pair_top[i], label, live ? "已连接" : "已配对");
            ui_draw_button(fb, ble_button_rect(l.hp_pair_top[i]), "忘记", false);
        }
        // 连接耳机时实体侧键/音量键仅调音量：避免音乐播放时误翻页。
        // Side-keys-volume-only while a headset is linked: stop accidental page turns during audio.
        // 本机扬声器也是同一条音频通路（没有耳机链路），文案一并覆盖，否则用户在扬声器
        // 模式下会以为这个开关和自己无关。
        // The local speaker is the same audio route with no headset link, so the copy covers it
        // too; otherwise speaker users assume the switch does not apply to them.
        setting_toggle(fb, l.hp_volkey_top, "播放音频时侧键仅调音量",
                       "耳机连接或本机扬声器出声时，实体音量键只调音量，不再翻页或导航",
                       app_settings_bt_volume_keys());
#endif
        section(fb, l.bt_section_top, "翻页器");
        // 学习/反馈时提示卡占开关位；结果列表此时收起，提示仍在首屏。
        // While learning or showing feedback the prompt card takes the switch slot; the result list
        // collapses then, so the prompt stays on the first screen.
        if (s_ble_learning || s_ble_feedback[0]) {
            settings_card(fb, (EpdRect){36, l.toggle_top, 612, 126}, 22, UI_GRAY_WHITE, 0x58);
            ui_text_vc(fb, 342, l.toggle_top + 63, 24,
                       !s_ble_learning ? s_ble_feedback :
                       s_ble_learning == 1 ? "请按翻页器上「上一页」要用的键"
                                           : "请按翻页器上「下一页」要用的键",
                       EPD_DRAW_ALIGN_CENTER, false);
            if (!s_ble_learning)
                ui_text_vc(fb, 342, l.toggle_top + 103, 19, "轻点此处返回蓝牙开关", EPD_DRAW_ALIGN_CENTER, false);
        } else {
            setting_toggle(fb, l.toggle_top, "启用翻页器",
                           "开启后可配对翻页器；连接耳机播放音乐时翻页器自动停用",
                           app_settings_ble_turner());
        }

        section(fb, l.status_top - 38, "状态");
        char ble_status[80];
#if KIIKO_BOARD_METALIO47
        // 耳机连接优先（用户最关心是否在放歌）；翻页器只在耳机没连时才描述。
        // 之前「翻页器关着」会优先命中并打「已关闭」，连着耳机也跟着显示已关闭，明显不准。
        // Headset first (what the user cares about); the turner is only described when no headset.
        // Previously a turned-off turner short-circuited to "已关闭" even with a headset linked.
        if (bt_audio_link_active())
            snprintf(ble_status, sizeof(ble_status),
                     bt_audio_audio_ready() ? "耳机播放中，翻页器停用" : "耳机已连·音频未通，翻页器停用");
        else if (ble_pt_connected())
            snprintf(ble_status, sizeof(ble_status), "已连接 %s", ble_pt_connected_name());
        else if (ble_pt_connecting())
            snprintf(ble_status, sizeof(ble_status), "连接中…");
        else if (ble_pt_running())
            snprintf(ble_status, sizeof(ble_status), "未连接");
        else if (!app_settings_ble_turner())
            snprintf(ble_status, sizeof(ble_status), "未连接");
        else
            snprintf(ble_status, sizeof(ble_status), "启动中…");
#else
        if (!app_settings_ble_turner()) snprintf(ble_status, sizeof(ble_status), "未连接");
        else if (!ble_pt_running()) {
            read_pico_transfer_status_t network;
            read_pico_transfer_get_status(&network);
            snprintf(ble_status, sizeof(ble_status), network.state != READ_PICO_TRANSFER_STOPPED ? "WiFi 使用中，蓝牙暂停" : "启动中…");
        }
        else if (ble_pt_connected()) snprintf(ble_status, sizeof(ble_status), "已连接 %s", ble_pt_connected_name());
        else if (ble_pt_connecting()) snprintf(ble_status, sizeof(ble_status), "连接中…");
        else snprintf(ble_status, sizeof(ble_status), "未连接");
#endif
        ble_row(fb, l.status_top, "当前", ble_status);
        if (l.notice_top >= 0) ui_text(fb, 44, l.notice_top, 21, s_ble_notice, EPD_DRAW_ALIGN_LEFT, false);

        // 已配对：点行连接，右侧按钮删除。/ Bonded peers: the row connects, the button forgets.
        section(fb, l.bonds_title_top, "已配对");
        if (!l.bond_rows)
            ble_row(fb, l.bonds_title_top + 38, "暂无已配对设备", "");
        for (int i = 0; i < l.bond_rows && i < BLE_PT_MAX_BONDS; ++i) {
            const ble_pt_bond_t *bond = ble_pt_bond((uint8_t)i);
            if (!bond) continue;
            char label[40];
            snprintf(label, sizeof(label), "%s", bond->name[0] ? bond->name : bond->addr);
            fit_value(label, 300);
            const bool live = ble_pt_connected() && !strcmp(ble_pt_connected_name(), bond->name);
            ble_row(fb, l.bond_top[i], label, live ? "已连接" : "连接  ›");
            ui_draw_button(fb, ble_button_rect(l.bond_top[i]), "删除", false);
        }

        ble_row(fb, l.scan_entry_top, "添加翻页器", "扫描设备");
        ui_draw_icon(fb,(624)-8,l.scan_entry_top + 38,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);

        section(fb, l.learn_title_top, "按键映射");
        static const char *const learn_names[] = {"上一页", "下一页"};
        static const ble_pt_action_t learn_actions[] = {BLE_PT_ACTION_PREV, BLE_PT_ACTION_NEXT};
        for (int i = 0; i < 2; ++i) {
            // 学到的键直接显示出来；没学过就走内置映射。
            // Show the learned key itself; with none, the built-in map applies.
            char bound_label[40];
            ble_pt_frame_t bound;
            if (ble_pt_binding(learn_actions[i], &bound)) format_bound_key(&bound, bound_label, sizeof(bound_label));
            else snprintf(bound_label, sizeof(bound_label), "内置按键");
            ble_row(fb, l.learn_top[i], learn_names[i], bound_label);
            ui_draw_button(fb, ble_button_rect(l.learn_top[i]), "学习", false);
        }
        ui_text(fb, 48, l.footer_top, 20, "上下、左右及 PageUp / PageDown 均可翻页", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, l.footer_top + 34, 20, "仅支持 BLE 翻页器，按键只在阅读时生效", EPD_DRAW_ALIGN_LEFT, false);
        ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, 190}, UI_GRAY_WHITE, fb);
        ui_nav_status(fb);
        back_header(fb, "蓝牙");
        settings_navigation(fb);
        return;
    }
    const int profile_y=180-s_main_scroll, profile_h=settings_profile_height();
    if (profile_y+profile_h>160) {
        settings_card(fb,(EpdRect){36,profile_y,612,profile_h},24,UI_GRAY_WHITE,0x70);
        int avatar_y=profile_y+(profile_h-82)/2;
        bool avatar_ok=s_avatar_ready;
        if (avatar_ok) ui_wallpaper_draw_avatar(fb,s_avatar_pixels,(EpdRect){57,avatar_y,82,82},20);
#if KIIKO_BOARD_METALIO47
        if (app_settings_main_fast_refresh() && avatar_ok) ui_image_bw_rect(fb,(EpdRect){57,avatar_y,82,82});
#endif
        if (!avatar_ok) {
            // 默认头像白底黑字 + 黑框，与首页头像一致。/ Default avatar: dark K on white, dark frame.
            ui_fill_round_rect(fb,(EpdRect){57,avatar_y,82,82},20,UI_GRAY_WHITE);
            ui_draw_control_frame(fb,(EpdRect){57,avatar_y,82,82},20,0x30);
            ui_text_fixed_vc(fb,98,avatar_y+41,49,"K",EPD_DRAW_ALIGN_CENTER,false);
        }
        char name[64];snprintf(name,sizeof(name),"%s",app_settings_device_name());
        int width=588-ui_text_fixed_width_px(ui_text_effective_px(20),"编辑")-188;
        while (name[0] && ui_text_fixed_width_px(ui_text_effective_px(32),name)>width) {
            size_t n=strlen(name)-1;while(n&&((unsigned char)name[n]&0xc0)==0x80)--n;name[n]=0;
        }
        ui_text_vc(fb,164,profile_y+53,32,name,EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,588,profile_y+53,20,"编辑",EPD_DRAW_ALIGN_RIGHT,false);
        ui_draw_icon(fb,625,profile_y+profile_h/2,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
    }
    char wifi[56];snprintf(wifi,sizeof(wifi),"%s",s_main_wifi_saved?s_main_wifi:"未配置");
    char font_name[96];
    snprintf(font_name,sizeof(font_name),"%s · %u%%",system_font_label(app_settings_system_font_path()),app_settings_system_font_size());
    char power[24];
    unsigned minutes=app_settings_auto_lock_minutes();
    if(minutes)snprintf(power,sizeof(power),"%u 分钟",minutes);else strcpy(power,"关闭");
    // 主页面只留功能域入口，细节各自进子页：刷新并入显示、升级与重置合并为系统维护、
    // 锁屏并入电源、前光并入显示；语音翻页是独立入口，直接进语音页。
    // The root keeps domain entries with details one level down: refresh joins display, upgrade
    // and reset merge into system maintenance, lock joins power, frontlight joins display, and
    // voice page-turn is its own entry that opens the voice page directly.
#if KIIKO_BOARD_METALIO47
    const char *category_labels[]={"连接与时间","显示与刷新","电源与锁屏","个性化","语音翻页","系统维护","AI 快捷键"};
    const char *category_values[]={wifi,font_name,power,settings_shelf_label(),
                                   app_settings_voice_turn()?"已开启":"已关闭","刷机 / 重置",
                                   ai_action_label(app_settings_ai_short())};
    const int category_icons[]={0,2,14,4,19,12,22};
#else
    const char *category_labels[]={"连接与时间","显示与刷新","电源与锁屏","个性化","系统维护"};
    const char *category_values[]={wifi,font_name,power,settings_shelf_label(),"刷机 / 重置"};
    const int category_icons[]={0,2,14,4,12};
#endif
    setting_group(fb,0,NULL,SETTINGS_CATEGORY_Y,
                  category_icons,category_labels,category_values,SETTINGS_CATEGORY_ROWS);
    // 主列表也要有一行提示位：确认、修复结果都写在这里，不然点了没有任何回音。
    // The main list needs a notice line too: confirmation and the repair result land here, or a tap
    // would have no answer at all.
    ui_fill_rect_fast((EpdRect){0,0,UI_LOCK_WIDTH,160},UI_GRAY_WHITE,fb);
    ui_nav_status(fb);
    ui_text(fb, 36, 91, 52, "设置", EPD_DRAW_ALIGN_LEFT, false);
    if (s_notice[0]) {
        const int notice_x=36+2*ui_text_effective_px(52)+24;
        char notice[sizeof(s_notice)];snprintf(notice,sizeof(notice),"%s",s_notice);
        while(notice[0] && ui_text_fixed_width_px(20,notice)>UI_LOCK_WIDTH-notice_x-36) {
            size_t n=strlen(notice)-1;while(n && ((unsigned char)notice[n]&0xc0)==0x80)--n;notice[n]=0;
        }
        ui_text_fixed_vc(fb,notice_x,ui_text_ink_center_y(91,52,"设置"),20,notice,EPD_DRAW_ALIGN_LEFT,false);
    }
    settings_navigation(fb);
#if KIIKO_BOARD_METALIO47
    if(app_settings_main_refresh_mode()==APP_MAIN_REFRESH_NORMAL){
        // 与书架共用逐相输出：状态栏/题头柔和，内容白像素不反向压黑；不影响其他模式。
        // Reuse shelf scan phases: soft status/title and no reverse-blackening on content whites, without changing other modes.
        const EpdRect content={0,160,UI_LOCK_WIDTH,UI_NAV_REFRESH_END-160};
        display_main_transition_gray_bands(&content,1);
    }
#endif
}

#if KIIKO_BOARD_METALIO47
static bool s_lock_style_return;
void app_device_settings_resume_lock_style(void) { s_lock_style_return=true; }
static bool settings_take_lock_style_return(void) {
    bool pending=s_lock_style_return;s_lock_style_return=false;return pending;
}
static app_redraw_t settings_open_lock_images(app_ctx_t *ctx, const char *path, bool transparent) {
    extern const app_desc_t app_image;
    if(!app_image_request_lock_picker(path,transparent)) {
        snprintf(s_notice,sizeof(s_notice),"图片路径无法打开，请重新选择");
        return APP_REDRAW_PAGE;
    }
    ctx->request_app=&app_image;
    return APP_REDRAW_NONE;
}
static bool settings_gray_preview(void) {
    return s_page==SETTINGS_WALLPAPER_PREVIEW||s_page==SETTINGS_SHELF_STYLE;
}
#endif
static void render(app_ctx_t *ctx,uint8_t *fb) {
    render_content(ctx,fb);
#if KIIKO_BOARD_METALIO47
    if(!settings_gray_preview()&&!settings_plain_list())display_monochrome_content(fb);
#endif
    settings_navigation(fb);
}

static void settings_prepare_profile(void) {
    const char *path=app_settings_avatar_path();
    if(pico_boot_recovery()||!path[0]){ui_avatar_request("",60);s_avatar_ready=s_avatar_pending=false;s_avatar_path[0]=0;return;}
    if(strcmp(path,s_avatar_path))s_avatar_ready=false;
    snprintf(s_avatar_path,sizeof(s_avatar_path),"%s",path);
    ui_avatar_request(path,82);
    ui_avatar_state_t state=ui_avatar_poll(path,82,s_avatar_pixels);
    s_avatar_ready=state==UI_AVATAR_READY;s_avatar_pending=state==UI_AVATAR_PENDING;
}
static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    pin_setup_close();
    s_notice[0] = 0;
    s_ble_feedback[0] = 0;
    s_page = SETTINGS_MAIN;
    s_parent = SETTINGS_MAIN;
    s_style_scroll = s_main_scroll = s_font_page = s_wallpaper_page = 0;
    s_collage_prepare_pending = false;
    s_scroll_drag_consumed = s_scroll_present_pending = false;
    s_wallpaper_selected = -1;
    s_bookmark_picker=false;bookmark_preview_release();
    s_wallpaper_confirm = s_wallpaper_preview_ok = false;
    s_sync_pending = false;
    s_font_sync_confirm = false; s_font_sync_path[0] = 0;
    s_config_confirm = false;
    s_panel_repair_pending = false;
    s_boot_pending = s_local_pending = s_upgrade_restart = false;
    s_upgrade_offer_pending = s_upgrade_confirm = false;
    (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved);
    settings_prepare_profile();
#if KIIKO_BOARD_METALIO47
    if(settings_take_lock_style_return()){s_page=SETTINGS_LOCK_STYLE;s_parent=SETTINGS_GROUP_POWER;}
#endif
}

// 主循环主动消费按键；识别不需要再触摸屏幕。/ Consume key edges on ticks without requiring another touch.
static bool ble_receive_feedback(void) {
    ble_pt_event_t unused;
    while (ble_pt_pop_key(&unused)) {}
    bool changed = false;
    ble_pt_raw_t raw;
    while (ble_pt_pop_raw(&raw)) {
        if (!raw.pressed || !ble_pt_connected()) continue;
        if (s_ble_learning) {
            const ble_pt_action_t action = s_ble_learning == 1 ? BLE_PT_ACTION_PREV : BLE_PT_ACTION_NEXT;
            const esp_err_t error = ble_pt_bind(action, &raw.frame);
            snprintf(s_ble_feedback, sizeof(s_ble_feedback), "%s",
                     error == ESP_OK ? (action == BLE_PT_ACTION_PREV ? "识别成功 · 已绑定上一页" : "识别成功 · 已绑定下一页")
                                     : "识别成功 · 绑定保存失败，请重试");
            s_ble_learning = 0;
            s_ble_scroll = 0;
            changed = true;
            // 同一帧的其余字节不得覆盖这次学习结果。/ Drain remaining bytes without overwriting this learned result.
            while (ble_pt_pop_raw(&raw)) {}
            break;
        }
        if (!s_ble_feedback[0]) {
            snprintf(s_ble_feedback, sizeof(s_ble_feedback), "识别成功 · 已收到翻页器按键");
            s_ble_scroll = 0;
            changed = true;
        }
    }
    return changed;
}

static app_redraw_t profile_editor_paint(app_ctx_t *ctx, bool field);
static app_redraw_t reset_tick(void) {
    s_reset_pending=false;
    read_pico_transfer_status_t state;read_pico_transfer_get_status(&state);
    esp_err_t error=ESP_ERR_INVALID_STATE;
    if(!pico_online_busy() && state.state!=READ_PICO_TRANSFER_UPLOADING &&
       book_cover_jobs_quiesce(2000) && ui_avatar_quiesce(2000)) {
        ble_pt_cancel_recent();
        error=ble_pt_stop(2000);
        if(error==ESP_OK) {
            read_pico_transfer_stop();
            error=app_factory_reset();
        }
    }
    if(error==ESP_OK) {esp_restart();return APP_REDRAW_NONE;}
    s_reset_confirm=false;
    snprintf(s_reset_notice,sizeof(s_reset_notice),"恢复未完成，请稍后重试：%s",esp_err_to_name(error));
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
#if KIIKO_BOARD_METALIO47
    // 识别测试只属于语音页：翻到别的页就把麦收掉，不留后台麦克风。
    // The recognition test belongs to the voice page alone; any other page drops the mic.
    if (s_voice_test && s_page != SETTINGS_VOICE) voice_test_stop();
    // 学习页：录音时长每秒刷一次，其余状态只在换状态时刷。
    // Learn page: the record timer repaints once per second; other states repaint on a change.
    if (s_page == SETTINGS_VOICE_LEARN) {
        static int last_state = -1;
        static int last_sec = -1;
        const int st = (int)voice_turn_learn_poll();
        int sec = -1;
        if (st == (int)VOICE_TURN_LEARN_RECORD) {
            const int fr = voice_turn_learn_record_frames();
            if (fr >= 0) sec = fr / 62;
        }
        if (st != last_state || sec != last_sec) {
            last_state = st;
            last_sec = sec;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    // 识别测试：持续要麦、丢弃方向（测试不翻页）；有新判定或麦克风状态变化才刷屏。
    // Recognition test: keep requesting the mic, drop directions (a test never turns pages),
    // and repaint only on a new verdict or a mic-state change.
    if (s_page == SETTINGS_VOICE && s_voice_test) {
        voice_turn_set_listen(true);
        (void)voice_turn_take_direction();
        voice_turn_match_t m;
        voice_turn_last_match(&m);
        static uint32_t last_seq;
        static bool last_mic;
        const bool mic = voice_turn_listening();
        const bool fresh = m.seq != last_seq || mic != last_mic;
        last_seq = m.seq;
        last_mic = mic;
        if (fresh) return APP_REDRAW_PAGE;
        // 没变化就落进通用 tick，时钟等照常刷新。/ With no change, fall through so the shared tick still runs.
    }
    if(s_page==SETTINGS_SD_RECOVERY) {
        if(!app_sd_recovery_tick(ctx->now_ms))return APP_REDRAW_NONE;
        if(!app_sd_recovery_busy())settings_prepare_profile();
        return APP_REDRAW_PAGE;
    }
    if(s_page==SETTINGS_ONLINE_FONTS)return ui_online_fonts_tick(&s_online_fonts,ctx->now_ms);
#endif
    // 屏幕修复：先画最终结果，再在 tick 中整屏恢复；成功后不重复推页。
    // Paint the final notice before the tick's full restoration; do not push the page twice on success.
    if (s_panel_repair_pending) {
        s_panel_repair_pending = false;
        if (s_page != SETTINGS_MAIN && !settings_group_page(s_page)) return APP_REDRAW_NONE;
        s_scroll_present_pending = false;
        snprintf(s_notice, sizeof(s_notice), "屏幕修复完成；若条纹仍在可再做一次");
        render(ctx, ctx->fb);
        if (display_panel_repair(ctx->hl) != EPD_DRAW_SUCCESS) {
            snprintf(s_notice, sizeof(s_notice), "屏幕输出未完成，请稍后重试");
            return APP_REDRAW_FULL;
        }
        return APP_REDRAW_DONE;
    }

    if (s_page==SETTINGS_MAIN && s_avatar_pending && !(ctx->touch && ctx->touch->touched)) {
        ui_avatar_state_t state=ui_avatar_poll(s_avatar_path,82,s_avatar_pixels);
        s_avatar_pending=state==UI_AVATAR_PENDING;
        s_avatar_ready=state==UI_AVATAR_READY;
        if (s_avatar_ready && s_main_scroll < 180+settings_profile_height()-160) {
            render(ctx,ctx->fb);
            EpdRect area={57,180-s_main_scroll+(settings_profile_height()-82)/2,82,82};
            if (area.y<160) { area.height-=160-area.y; area.y=160; }
            if(area.height>0)guard_draw_result(ctx->hl,update_display_area_diff_with(ctx->hl,
#if KIIKO_BOARD_METALIO47
                display_main_bw_waveform(),MODE_DU,area));
#else
                app_settings_main_refresh_mode()==APP_MAIN_REFRESH_NORMAL?display_main_normal_waveform():display_main_gray_waveform(),MODE_GL16,area));
#endif
            return APP_REDRAW_DONE;
        }
    }
    if(s_reset_pending && s_page==SETTINGS_FACTORY_RESET)return reset_tick();
    if (s_page == SETTINGS_PIN_ENTRY) return pin_setup_tick(ctx);
#if KIIKO_BOARD_METALIO47
    // 选择拼贴只保存样式；实际锁屏按原有缓存路径生成，不在设置tick里阻塞导航。
    // Selecting collage saves its style only; build through the existing lock/cache path without blocking settings navigation.
#else
    if (s_collage_prepare_pending && s_page == SETTINGS_LOCK_STYLE) {
        s_collage_prepare_pending = false;
        // 先让选中反馈可见，再生成卡上缓存；render 本身只绘图。
        // Show selection feedback first, then prepare the SD cache; render stays paint-only.
        const size_t bytes = UI_LOCK_WIDTH * UI_LOCK_HEIGHT / 2u;
        size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *preview = available > bytes + 1024u * 1024u
            ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (preview) {
            app_lock_font_t saved;
            app_font_begin_lock(&saved);
            (void)book_lock_collage_draw(preview);
            app_font_end_lock(&saved);
            free(preview);
        }
        return APP_REDRAW_NONE;
    }
#endif

    if (s_page == SETTINGS_TEXT_EDIT) {
        if (ctx->consumed) return APP_REDRAW_NONE;
        bool held = !ctx->released && ctx->touch && ctx->touch->touched && ctx->touch->count == 1;
        if (ui_keyboard_hold_tick(held, ctx->touch ? ctx->touch->x : 0,
            ctx->touch ? ctx->touch->y : 0, ctx->now_ms) == UI_KEYBOARD_CHANGED) return profile_editor_paint(ctx, false);
        return ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms) == UI_KEYBOARD_CHANGED
            ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BLUETOOTH && ble_receive_feedback()) return APP_REDRAW_PAGE;
    // 蓝牙启动失败的原因转成提示。放在这里而不是 render()：render 必须是纯绘制，
    // 而 take_* 会清空状态。
    // Fold a Bluetooth start failure into the notice here rather than in render(), which must
    // stay pure, and because take_* clears the state.
    // 蓝牙页要跟着栈的状态重绘：开关拨开后栈是异步起来的，配对表、扫描结果和连接状态
    // 都在页面画完以后才变。不重绘的话用户看到的永远是进页那一刻的快照，只能退出去再进来。
    // The Bluetooth pages redraw with the stack: enabling it starts it asynchronously, and the
    // bond list, scan results and link state all change after the page was painted. Without a
    // poll the user only ever sees the snapshot from when they entered, and has to leave and
    // come back.
    if (s_page == SETTINGS_BLUETOOTH || s_page == SETTINGS_BLE_SCAN) {
        static uint32_t ble_seen;
        uint32_t sig = (app_settings_ble_turner() ? 1u : 0u) | (ble_pt_running() ? 2u : 0u) |
                       (ble_pt_connected() ? 4u : 0u) | (ble_pt_connecting() ? 8u : 0u) |
                       (ble_pt_scanning() ? 16u : 0u) |
                       ((uint32_t)ble_pt_bond_count() << 8) |
                       ((uint32_t)ble_scan_visible_count() << 16);
#if KIIKO_BOARD_METALIO47
        // 耳机侧的连接、扫描与结果数也进签名；连上后收起设备列表。
        // The headset link, scan and result count join the signature; linking collapses the list.
        const bool hp_link = bt_audio_link_active();
        sig |= (hp_link ? 1u << 20 : 0u) | (bt_audio_scanning() ? 1u << 21 : 0u) |
               ((uint32_t)bt_audio_device_count() << 22);
        // 音频就绪与否也要进签名：链路一直是通的、只是 `DISC SCO` 迟到，界面状态行必须跟着翻，
        // 否则用户看到的永远是定格在“已连接·无音频”的旧画面。
        // Readiness joins the signature too: the link may stay up while `DISC SCO` arrives late,
        // so the status row has to repaint or the screen stays frozen on a stale state.
        sig |= (hp_link && bt_audio_audio_ready() ? 1u << 30 : 0u);
        // 测试音的起停也要进签名：按钮文案与状态行文字都要跟着翻。
        // The tone's start/stop joins the signature too: the button label and the row text flip with it.
        sig |= (bt_audio_test_tone_active() ? 1u << 31 : 0u);
        // 配对表行数占 26..28、蓝牙音频开关占 29（设备数已占 22..25）。
        // The paired-row count takes bits26..28 and the audio switch bit29 (device count owns 22..25).
        sig |= ((uint32_t)app_settings_bt_paired_count() << 26) |
               (app_settings_bt_enabled() ? 1u << 29 : 0u);
        if (hp_link && s_bt_show) s_bt_show = false;
#endif
        if (sig != ble_seen) {
            ble_seen = sig;
            // 列表行数一变，下面的内容整体上下移动；MODE_DU 差分会把旧字留在屏上，
            // 于是新旧文字叠在一起。行数变化时改用全刷，其他状态变化仍走差分。
            // A row-count change shifts everything below it, and a MODE_DU differential
            // leaves the old glyphs behind, so new text lands on their ghost. Repaint fully
            // on row-count changes only; every other state change stays differential.
            static int rows_seen = -1;
            int rows_now = s_page == SETTINGS_BLE_SCAN
                               ? ble_scan_visible_count()
                               : (s_bt_show ? (int)bt_audio_device_count() : 0) +
                                     (int)ble_pt_bond_count();
#if KIIKO_BOARD_METALIO47
            rows_now += (int)app_settings_bt_paired_count();  // 耳机配对表也占行 / the headset table takes rows too
#endif
            if (rows_now != rows_seen) {
                rows_seen = rows_now;
                return APP_REDRAW_FULL;
            }
            return APP_REDRAW_PAGE;
        }
    }
    if (s_page == SETTINGS_BLUETOOTH && !s_ble_notice[0]) {
        char why[64];
        if (ble_pt_take_start_failure(why, sizeof(why))) {
            snprintf(s_ble_notice, sizeof(s_ble_notice), "%s", why);
            return APP_REDRAW_PAGE;
        }
    }

    (void)ctx;
    if (s_boot_pending) {
        s_boot_pending = false;
#if KIIKO_BOARD_METALIO47
        esp_err_t err=app_enter_download();
        s_boot_connected=metalio_download_connected();
        if (err==ESP_OK) s_boot_notice[0]=0;
        else snprintf(s_boot_notice,sizeof(s_boot_notice),err==ESP_ERR_NOT_FINISHED ?
            "存储或传输正在使用，请稍后重试" : "BOOT 切换失败：%s",esp_err_to_name(err));
        return APP_REDRAW_PAGE;
    }
    if (s_page==SETTINGS_BOOT && app_download_active()) {
        const bool connected=metalio_download_connected();
        if (connected!=s_boot_connected) { s_boot_connected=connected; return APP_REDRAW_PAGE; }
#else
        pmu_selftest_prepare_powerdown();
        uint8_t req[2] = {0, 0};
        esp_err_t error = read_pico_pmu_cmd(PMU_CMD_HOST_REQUEST_RESET, req, sizeof(req));
        if (error != ESP_OK) ESP_LOGW(TAG, "BOOT PMU notice: %s", esp_err_to_name(error));
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart(); return APP_REDRAW_NONE;
#endif
    }
    if (s_upgrade_restart) { s_upgrade_restart = false; esp_restart(); return APP_REDRAW_NONE; }
    if (s_page == SETTINGS_UPGRADE) {
        if (s_upgrade_job != UPGRADE_JOB_NONE) return upgrade_start_pending();
        if (s_local_pending) {
            s_local_pending = false;
#if KIIKO_BOARD_METALIO47
            esp_err_t error = install_local_update();
#else
            esp_err_t error = pico_ota_install(PICO_OTA_UPDATE_PATH, s_upgrade_notice, sizeof(s_upgrade_notice));
#endif
            s_upgrade_restart = error == ESP_OK;
            return APP_REDRAW_PAGE;
        }
        pico_online_get_status(&s_update);
        if (s_update.state == PICO_UPDATE_READY && !s_update.busy) {
            if (pico_online_commit() == ESP_OK) { esp_restart(); return APP_REDRAW_NONE; }
            pico_online_get_status(&s_update);
        }
        uint32_t now = esp_timer_get_time() / 1000;
        app_redraw_t redraw = upgrade_status_redraw(now);
        if (redraw == APP_REDRAW_AREA) draw_upgrade_progress_delta(ctx->fb);
        return redraw;
    }
    if (!s_sync_pending) return APP_REDRAW_NONE;
    s_sync_pending = false;
    uint32_t utc = 0;
    read_pico_transfer_status_t network = {0};
    read_pico_transfer_get_status(&network);
    esp_err_t err = read_pico_transfer_sync_time(&utc);
    bool network_time_received = err == ESP_OK;
    if (network_time_received) {
        // 临时 WiFi 收尾可能耗时；写入 PMU 时取当前 S3 时间，避免回写旧秒数。
        // Temporary WiFi teardown takes time; seed the PMU from the current S3 clock.
        utc = (uint32_t)time(NULL);
        uint8_t payload[4] = {(uint8_t)utc, (uint8_t)(utc >> 8), (uint8_t)(utc >> 16), (uint8_t)(utc >> 24)};
        err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
        if (err == ESP_OK) err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
        if (err == ESP_OK) {
            const pmu_snapshot_t *pmu = read_pico_pmu_get();
            if (!pmu || !pmu->time_synced || pmu->unix_sec < utc || pmu->unix_sec - utc > 10)
                err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    ESP_LOGI(TAG, "time sync network=%d ready=%d utc=%lu result=%s",
             network.mode, network.network_ready, (unsigned long)utc, esp_err_to_name(err));
    const char *notice = err == ESP_OK ? "WiFi 对时完成，内部时钟已接管" :
        network_time_received ? "已获取网络时间，内部时钟写入失败" :
        err == ESP_ERR_NOT_FOUND ? "请先在 WiFi 设置中配置网络" :
        err == ESP_ERR_INVALID_STATE ? "WiFi 尚未连接，请稍后再试" :
        err == ESP_ERR_TIMEOUT ? "网络对时超时，请检查网络后重试" :
        "网络对时失败，请检查网络后重试";
    snprintf(s_notice, sizeof(s_notice), "%s", notice);
    return APP_REDRAW_PAGE;
}

static EpdRect s_input_area;
static bool s_input_layout, s_input_settle;
static app_redraw_t profile_editor_paint(app_ctx_t *ctx, bool field) {
    ui_keyboard_update_t update = ui_keyboard_update(ctx->fb, UI_SYSTEM_KEYBOARD_TOP, (EpdRect){36, 243, 612, 82}, 28, false, NULL, field);
    s_input_area = update.area; s_input_layout = update.layout; s_input_settle = update.settle;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}
static app_redraw_t profile_editor_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(ev->x0, ev->y0, UI_SYSTEM_KEYBOARD_TOP, ctx->now_ms) ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    bool feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        bool page = ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (page || feedback) ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return feedback ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (y < 160) {
        if (x < 160) { ui_keyboard_end(); s_page = s_editor_signature ? settings_back() : SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (x > 510) goto save_text;
    }
    if (ui_text_input_tap(&s_editor_input, (EpdRect){36, 243, 612, 82}, 28,
                          false, NULL, x, y)) return profile_editor_paint(ctx, true);
    ui_keyboard_result_t result = ui_keyboard_tap(x, y, UI_SYSTEM_KEYBOARD_TOP, esp_timer_get_time() / 1000);
    if (result == UI_KEYBOARD_DONE) goto save_text;
    return result == UI_KEYBOARD_CHANGED ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
save_text:
    if (ui_keyboard_pending()) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "请先选择候选字");
        return APP_REDRAW_PAGE;
    }
    if (s_editor_signature) app_settings_set_status_signature(s_editor);
    else if (s_editor[0]) app_settings_set_device_name(s_editor);
    else { snprintf(s_editor_notice, sizeof(s_editor_notice), "设备名称不能为空"); return APP_REDRAW_PAGE; }
    const char *saved = s_editor_signature ? app_settings_status_signature() : app_settings_device_name();
    if (strcmp(saved, s_editor)) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "保存失败，请检查设置存储空间");
        return APP_REDRAW_PAGE;
    }
    ui_keyboard_end();
    s_page = s_editor_signature ? settings_back() : SETTINGS_PROFILE;
    return APP_REDRAW_PAGE;
}

// 越过阈值按段推进，一次手势只更新一次，松手不再播放惯性。
// Advance one discrete step after crossing the threshold, with one update per gesture and no release inertia.
static app_redraw_t scroll_gesture(const ui_gesture_event_t *ev, int *offset, int limit, bool *handled) {
    *handled = ev->type != UI_GESTURE_TAP || s_scroll_drag_consumed;
    if (ev->type == UI_GESTURE_PRESS) {
        s_scroll_drag_consumed = false;
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_TAP || ev->type == UI_GESTURE_CANCEL) {
        s_scroll_drag_consumed = false;
        return APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_MOVE && ev->type != UI_GESTURE_SWIPE_U && ev->type != UI_GESTURE_SWIPE_D)
        return APP_REDRAW_NONE;
    if (ev->y0 < (s_page == SETTINGS_MAIN ? 160 : 190) || ev->y0 >= settings_viewport_end())
        return APP_REDRAW_NONE;
    if (s_scroll_drag_consumed) {
        if (ev->type != UI_GESTURE_MOVE) s_scroll_drag_consumed = false;
        return APP_REDRAW_NONE;
    }
    int dy = (int)ev->y - ev->y0, dx = (int)ev->x - ev->x0;
    int distance = abs(dy);
    if (ev->type == UI_GESTURE_MOVE && (distance < 108 || distance <= abs(dx))) return APP_REDRAW_NONE;
    int step = distance < 204 ? 204 : distance > 450 ? 450 : distance;
    bool up = ev->type == UI_GESTURE_SWIPE_U || (ev->type == UI_GESTURE_MOVE && dy < 0);
    int next = *offset + (up ? step : -step);
    if (limit < 0) limit = 0;
    if (next < 0) next = 0;
    if (next > limit) next = limit;
    if (ev->type == UI_GESTURE_MOVE) s_scroll_drag_consumed = true;
    if (next == *offset) return APP_REDRAW_NONE;
    *offset = next;
    s_scroll_present_pending = true;
    return APP_REDRAW_AREA;
}

static app_redraw_t reset_gesture(app_ctx_t *ctx,const ui_gesture_event_t *ev) {
    if(ev->type!=UI_GESTURE_TAP || s_reset_pending)return APP_REDRAW_NONE;
    int tab=settings_nav_hit(ev->x0,ev->y0);
    if(tab>=0){s_reset_confirm=false;ui_nav_request(ctx,tab);return APP_REDRAW_NONE;}
    if((ev->y0<190 && ev->x0<170)||ui_rect_hit((EpdRect){54,950,576,78},ev->x0,ev->y0)) {
        s_reset_confirm=false;s_page=settings_back();return APP_REDRAW_PAGE;
    }
    if(ui_rect_hit((EpdRect){54,850,576,78},ev->x0,ev->y0)) {
        s_reset_notice[0]=0;
        if(s_reset_confirm)s_reset_pending=true;
        else s_reset_confirm=true;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

#if KIIKO_BOARD_METALIO47
static app_redraw_t sd_recovery_gesture(app_ctx_t *ctx,const ui_gesture_event_t *ev) {
    if(app_sd_recovery_busy() || ev->type!=UI_GESTURE_TAP)return APP_REDRAW_NONE;
    if(ui_rect_hit(sd_recovery_button(),ev->x0,ev->y0)) {
        esp_err_t error=app_sd_recovery_begin(ctx->now_ms);
        snprintf(s_sd_notice,sizeof(s_sd_notice),"%s",error==ESP_OK?"":
            "请先退出传书、USB读卡和升级，再重试");
        return APP_REDRAW_PAGE;
    }
    if(ev->y0<190 && ev->x0<170) {s_page=settings_back();settings_prepare_profile();return APP_REDRAW_PAGE;}
    int tab=settings_nav_hit(ev->x0,ev->y0);
    if(tab>=0)ui_nav_request(ctx,tab);
    return APP_REDRAW_NONE;
}
#endif

#if KIIKO_BOARD_METALIO47
// 两开关都开且未连接时，进蓝牙页自动连旧设备（零操作）。开关语义不变：这里不改任何设置。
// With both switches on and no link, entering the page auto-connects the old headset;
// settings stay untouched — this never flips the switches.
static void hp_auto_connect_on_entry(void) {
    if (!app_settings_bt_enabled() || !app_settings_bt_auto_connect()) return;
    if (bt_audio_link_active() || bt_audio_scanning()) return;
    if (music_player_start() != ESP_OK) return;
    uint8_t bda[6];
    char name[36];
    bool have = app_settings_bt_paired_count() > 0 &&
                app_settings_bt_paired_get(0, bda, name, sizeof(name));
    if (!have) have = app_settings_bt_last_device(bda, name, sizeof(name));
    if (have && bt_audio_connect(bda) == ESP_OK)
        snprintf(s_ble_notice, sizeof(s_ble_notice), "正在连接耳机…");
}
#endif

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
#if KIIKO_BOARD_METALIO47
    if(app_sd_recovery_busy())return APP_REDRAW_NONE;
    if(s_page==SETTINGS_SD_RECOVERY)return sd_recovery_gesture(ctx,ev);
    if(s_page==SETTINGS_ONLINE_FONTS){
        app_redraw_t redraw=ui_online_fonts_gesture(&s_online_fonts,ev);
        if(s_online_fonts.closed){online_font_cancel_join();ttf_font_scan();s_font_page=0;s_page=SETTINGS_SYSTEM_FONT;}
        return redraw;
    }
    if(s_page==SETTINGS_SYSTEM_FONT&&!s_font_sync_confirm&&ev->type==UI_GESTURE_TAP&&
       ui_rect_hit(ui_online_fonts_entry_rect(79),ev->x0,ev->y0)){
        ui_online_fonts_open(&s_online_fonts,false);s_page=SETTINGS_ONLINE_FONTS;return APP_REDRAW_PAGE;
    }
#endif
    if (s_page==SETTINGS_SYSTEM_FONT && s_font_sync_confirm) {
        if (ev->type!=UI_GESTURE_TAP) return APP_REDRAW_NONE;
        for (int i=0;i<2;++i) {
            if (!ui_rect_hit(system_font_sync_button(i),ev->x0,ev->y0)) continue;
            if (i) {
                app_settings_set_font_path(s_font_sync_path);
                if (strcmp(app_settings_font_path(),s_font_sync_path)) {
                    snprintf(s_notice,sizeof(s_notice),"保存失败，请重试");return APP_REDRAW_PAGE;
                }
            }
            s_notice[0]=0;s_font_sync_confirm=false;return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_PIN_ENTRY) {
        EpdRect dirty; ui_pin_result_t result = ui_pinpad_handle(&s_pinpad, ev, &dirty);
        if (result == UI_PIN_CANCEL) { pin_setup_close(); s_page = SETTINGS_LOCK_PIN; return APP_REDRAW_PAGE; }
        if (result == UI_PIN_COMPLETE) s_pin_pending = true;
        if (result == UI_PIN_NONE) return APP_REDRAW_NONE;
        ui_pinpad_paint_input(ctx->fb, &s_pinpad);
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, dirty));
        return APP_REDRAW_DONE;
    }
    if (s_page == SETTINGS_LOCK_PIN && ev->type == UI_GESTURE_TAP && lock_pin_available() &&
        (ui_rect_hit((EpdRect){36,406,612,70},ev->x0,ev->y0) ||
         (lock_pin_enabled() && ui_rect_hit((EpdRect){36,512,612,70},ev->x0,ev->y0)))) {
        if (ctx->now_ms < s_pin_retry_at) { snprintf(s_notice, sizeof(s_notice), "请等待后重试"); return APP_REDRAW_PAGE; }
        pin_setup_open(ctx, ev->y0 >= 512); return APP_REDRAW_PAGE;
    }

#if KIIKO_BOARD_METALIO47
    if(s_page==SETTINGS_LOCK_PIN && ev->type==UI_GESTURE_TAP && lock_pin_available() && lock_pin_enabled() &&
       ui_rect_hit(pin_grace_row(),ev->x0,ev->y0)) {
        app_settings_set_pin_grace_code((app_settings_pin_grace_code()+1)%5);
        return APP_REDRAW_PAGE;
    }
#endif

    if (s_page == SETTINGS_BOOT) {
        if (ev->type != UI_GESTURE_TAP || s_boot_pending) return APP_REDRAW_NONE;
#if KIIKO_BOARD_METALIO47
        if (ev->y0 < 190 && ev->x0 < 170) { app_cancel_download(); s_page = settings_back(); (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        if (ui_rect_hit((EpdRect){60, 438, 564, 78}, ev->x0, ev->y0)) { s_boot_notice[0]=0; s_boot_pending = true; return APP_REDRAW_PAGE; }
#else
        if (ev->y0 < 190 && ev->x0 < 170) { s_page = settings_back(); (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        if (ui_rect_hit((EpdRect){60, 438, 564, 78}, ev->x0, ev->y0)) { s_boot_pending = true; return APP_REDRAW_PAGE; }
#endif
        int tab = settings_nav_hit(ev->x0, ev->y0);
        if (tab >= 0) ui_nav_request(ctx, tab);
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_UPGRADE) {
        if (ev->type != UI_GESTURE_TAP || s_local_pending || s_upgrade_restart) return APP_REDRAW_NONE;
        int y = ev->y0;
#if KIIKO_BOARD_METALIO47
    if(s_page==SETTINGS_FRONTLIGHT) {
        if(y<190) { s_page=settings_back(); return APP_REDRAW_PAGE; }
        float bpos = metalio_frontlight_position(false);
        float wpos = metalio_frontlight_position(true);
        float bmax_pos = (float)app_settings_brightness_max();          // 上限（0..240，与 RANGE 同单位）
        const float min_pos = 2.0f*METALIO_FRONTLIGHT_RANGE/2047.0f;                                  // 最低非灭亮度 ≈0.1%（11bit：total≥1 经四舍五入必点亮一路）
        // 指数曲线在底部步长趋近 0，降到最低后按“增加”几乎不动；补一个 0.1% 最小步长。
        // The exponential curve's step shrinks to nearly zero at the bottom, so "+" barely moves
        // after reaching the minimum; enforce a 0.1% minimum step per press.
        const float min_step = METALIO_FRONTLIGHT_RANGE*0.001f;
        // 亮度按钮改指数：一下 ×0.950625（=下拉栏两像素），与下拉手感一致
        if(y>=390 && y<458) {
            if(ev->x0<342) {                                         // 降低
                float next = bpos*0.950625f;
                if(next > bpos-min_step) next = bpos-min_step;
                bpos = next < min_pos ? min_pos : next;
            } else {                                                 // 提高
                float next = bpos/0.950625f;
                if(next < bpos+min_step) next = bpos+min_step;
                bpos = next > bmax_pos ? bmax_pos : next;
            }
        } else if(y>=620 && y<688) {
            if(ev->x0<342) wpos = wpos>=10.0f?wpos-10.0f:0.0f; else wpos = wpos<=230.0f?wpos+10.0f:240.0f;
        } else if(y>=750 && y<818) { metalio_frontlight_toggle(); return APP_REDRAW_PAGE; }
        else if(y>=940 && y<1008) {
            unsigned m = app_settings_brightness_max();
            m = ev->x0<342 ? (m>=12?m-12:1) : (m<=228?m+12:240);
            app_settings_set_brightness_max(m);
            return APP_REDRAW_PAGE;
        }
        else return APP_REDRAW_NONE;
        metalio_frontlight_preview_position(bpos, wpos);
        metalio_frontlight_commit();
        return APP_REDRAW_PAGE;
    }
#else
#endif
        if (y < 190 && ev->x0 < 170) {
            s_upgrade_job = UPGRADE_JOB_NONE;
            s_upgrade_offer_pending = false;
            if (s_upgrade_confirm) s_upgrade_confirm = false;
            else { pico_online_cancel_join(); s_page = settings_back(); }
            return APP_REDRAW_PAGE;
        }
        int tab = settings_nav_hit(ev->x0, y);
        if (tab >= 0) { s_upgrade_job = UPGRADE_JOB_NONE; pico_online_cancel_join(); ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
        if (s_upgrade_confirm) {
            bool accept = ui_rect_hit(upgrade_confirm_button(s_upgrade_online, true), ev->x0, y);
            if (accept || ui_rect_hit(upgrade_confirm_button(s_upgrade_online, false), ev->x0, y)) {
                s_upgrade_confirm = false;
                s_upgrade_offer_pending = false;
                if (accept) {
                    if (s_upgrade_online) {
                        upgrade_schedule(true, esp_timer_get_time() / 1000);
                    } else s_local_pending = true;
                }
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        if (ev->x0 < 60 || ev->x0 >= 624) return APP_REDRAW_NONE;
        if (y >= 550 && y < 612) {
            s_upgrade_notice[0] = 0;
            if (s_update.busy) { s_upgrade_job = UPGRADE_JOB_NONE; s_upgrade_offer_pending = false; pico_online_cancel_join(); }
            else if (s_update.state == PICO_UPDATE_AVAILABLE) { s_upgrade_confirm = true; s_upgrade_online = true; }
            else {
                upgrade_schedule(false, esp_timer_get_time() / 1000);
                return APP_REDRAW_PAGE;
            }
            pico_online_get_status(&s_update);
            upgrade_remember(esp_timer_get_time() / 1000);
            return APP_REDRAW_PAGE;
        }
        if (y >= 840 && y < 902 && !s_update.busy) {
            (void)pico_ota_inspect(PICO_OTA_UPDATE_PATH, &s_local_update);
            if (s_local_update.ready) { s_upgrade_confirm = true; s_upgrade_online = false; }
            else snprintf(s_upgrade_notice, sizeof(s_upgrade_notice), "%s", s_local_update.message);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if(s_page==SETTINGS_FACTORY_RESET)return reset_gesture(ctx,ev);
    if (s_page == SETTINGS_TEXT_EDIT) return profile_editor_gesture(ctx, ev);
    if (s_page == SETTINGS_MAIN || s_page == SETTINGS_BLUETOOTH || s_page == SETTINGS_BLE_SCAN || s_page == SETTINGS_SHELF_STYLE) {
        int limit = SETTINGS_SCROLL_MAX;
        int *offset = &s_main_scroll;
        if (s_page == SETTINGS_SHELF_STYLE) {
            limit=263+SHELF_STYLE_CHOICES*253-23-settings_viewport_end()+12;
            offset=&s_style_scroll;
        } else if (s_page != SETTINGS_MAIN) {
            int bottom = s_page == SETTINGS_BLUETOOTH ? ble_layout().content_bottom + s_ble_scroll : ble_scan_layout().content_bottom;
            limit = bottom - settings_content_end() + 12;
            offset = &s_ble_scroll;
        }
        bool handled;
        app_redraw_t redraw = scroll_gesture(ev, offset, limit, &handled);
        if (handled) return redraw;
    }

    if (s_page == SETTINGS_AVATAR && (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int pages = (s_wallpaper_count + 6) / 7;
        int old = s_wallpaper_page;
        if (ev->type == UI_GESTURE_SWIPE_U && s_wallpaper_page + 1 < pages) ++s_wallpaper_page;
        if (ev->type == UI_GESTURE_SWIPE_D && s_wallpaper_page > 0) --s_wallpaper_page;
        if (s_wallpaper_page == old) return APP_REDRAW_NONE;
        s_scroll_present_pending = true;
        return APP_REDRAW_AREA;
    }
    if (s_page == SETTINGS_WALLPAPER_PREVIEW && !s_wallpaper_confirm &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int next = s_wallpaper_selected + (ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
        if (next < 0 || next >= s_wallpaper_count) return APP_REDRAW_NONE;
        s_wallpaper_selected = next;
        bookmark_preview_prepare(ctx->fb);
        s_wallpaper_page = next / 8;
        return APP_REDRAW_PAGE;
    }
    if ((s_page == SETTINGS_SYSTEM_FONT || s_page == SETTINGS_WALLPAPER) &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int *page = s_page == SETTINGS_SYSTEM_FONT ? &s_font_page : &s_wallpaper_page;
        int count = s_page == SETTINGS_SYSTEM_FONT ? ttf_font_count() + 1 : s_wallpaper_count;
        int slots=s_page==SETTINGS_SYSTEM_FONT?system_font_layout().slots:8;
        int pages = count > 0 ? (count + slots - 1) / slots : 1;
        int old = *page;
        if (ev->type == UI_GESTURE_SWIPE_U && *page + 1 < pages) ++*page;
        if (ev->type == UI_GESTURE_SWIPE_D && *page > 0) --*page;
        if (*page == old) return APP_REDRAW_NONE;
        s_scroll_present_pending = true;
        return APP_REDRAW_AREA;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (s_page == SETTINGS_WALLPAPER_PREVIEW && s_wallpaper_confirm) {
        if (ev->y0 >= 643 && ev->y0 < 711 && ev->x0 >= 358 && ev->x0 < 590) {
            const char *path = s_wallpapers[s_wallpaper_selected].path;
            if(s_bookmark_picker)app_settings_set_bookmark_path(path);
            else app_settings_set_wallpaper_path(path);
            if (!strcmp(s_bookmark_picker?app_settings_bookmark_path():app_settings_wallpaper_path(), path)) {
                app_settings_set_lock_style(s_bookmark_picker?3:1);
                s_page = SETTINGS_LOCK_STYLE;
                bookmark_preview_release();
            }
        }
        s_wallpaper_confirm = false;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_TIME_EDIT) {
        if (ev->y0 < 190) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
        if (ev->y0 >= 354 && ev->y0 < 597) {
            int field = (ev->x0 - 36) / 122;
            if (ev->x0 >= 36 && field >= 0 && field < 5 && (ev->x0 - 36) % 122 < 106) {
                if (ev->y0 < 416) time_adjust(field, 1);
                else if (ev->y0 >= 535) time_adjust(field, -1);
                else return APP_REDRAW_NONE;
                return APP_REDRAW_PAGE;
            }
        }
        if (ev->y0 >= 767 && ev->y0 < 845) { (void)time_save(); return APP_REDRAW_PAGE; }
        if (ev->y0 >= 874 && ev->y0 < 952) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    int tab = settings_nav_hit(ev->x0, ev->y0);
    if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    int y = ev->y0;
#if KIIKO_BOARD_METALIO47
    if(s_page==SETTINGS_FRONTLIGHT) {
        if(y<190) { s_page=settings_back(); return APP_REDRAW_PAGE; }
        float bpos = metalio_frontlight_position(false);
        float wpos = metalio_frontlight_position(true);
        float bmax_pos = (float)app_settings_brightness_max();          // 上限（0..240，与 RANGE 同单位）
        const float min_pos = 2.0f*METALIO_FRONTLIGHT_RANGE/2047.0f;                                  // 最低非灭亮度 ≈0.1%（11bit：total≥1 经四舍五入必点亮一路）
        // 指数曲线在底部步长趋近 0，降到最低后按“增加”几乎不动；补一个 0.1% 最小步长。
        // The exponential curve's step shrinks to nearly zero at the bottom, so "+" barely moves
        // after reaching the minimum; enforce a 0.1% minimum step per press.
        const float min_step = METALIO_FRONTLIGHT_RANGE*0.001f;
        // 亮度按钮改指数：一下 ×0.950625（=下拉栏两像素），与下拉手感一致
        if(y>=390 && y<458) {
            if(ev->x0<342) {                                         // 降低
                float next = bpos*0.950625f;
                if(next > bpos-min_step) next = bpos-min_step;
                bpos = next < min_pos ? min_pos : next;
            } else {                                                 // 提高
                float next = bpos/0.950625f;
                if(next < bpos+min_step) next = bpos+min_step;
                bpos = next > bmax_pos ? bmax_pos : next;
            }
        } else if(y>=620 && y<688) {
            if(ev->x0<342) wpos = wpos>=10.0f?wpos-10.0f:0.0f; else wpos = wpos<=230.0f?wpos+10.0f:240.0f;
        } else if(y>=750 && y<818) { metalio_frontlight_toggle(); return APP_REDRAW_PAGE; }
        else if(y>=940 && y<1008) {
            unsigned m = app_settings_brightness_max();
            m = ev->x0<342 ? (m>=12?m-12:1) : (m<=228?m+12:240);
            app_settings_set_brightness_max(m);
            return APP_REDRAW_PAGE;
        }
        else return APP_REDRAW_NONE;
        metalio_frontlight_preview_position(bpos, wpos);
        metalio_frontlight_commit();
        return APP_REDRAW_PAGE;
    }
#else
#endif
    if (s_page == SETTINGS_PROFILE) {
        if (y < 190) { s_page = settings_back(); (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        if (y >= 266 && y < 350) { profile_editor_open(false); return APP_REDRAW_PAGE; }
        if (y >= 366 && y < 450) { s_bookmark_picker=false;wallpaper_scan(); s_page = SETTINGS_AVATAR; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_AVATAR) {
        if (y < 190) { s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (y >= 270 && y < 354) { app_settings_set_avatar_path(""); s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (y >= 374 && y < 1046) {
            int i = (y - 374) / 96, index = s_wallpaper_page * 7 + i;
            if (index >= 0 && index < s_wallpaper_count && (y - 374) % 96 < 80) {
                app_settings_set_avatar_path(s_wallpapers[index].path);
                s_page = SETTINGS_PROFILE;
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_WALLPAPER_PREVIEW) {
        if (y < 190) { bookmark_preview_release(); s_page = SETTINGS_WALLPAPER; return APP_REDRAW_PAGE; }
        if (s_wallpaper_preview_ok && y >= 194 && y < 1032) {
            s_wallpaper_confirm = true;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_WALLPAPER && y < 190) {
        s_page = SETTINGS_LOCK_STYLE;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_SHELF_STYLE) {
        if (y < 190) { s_page = settings_back(); (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        for (int i = 0; i < SHELF_STYLE_CHOICES; ++i) {
            if (y >= 242 && y >= 263 + i * 253 - s_style_scroll && y < 493 + i * 253 - s_style_scroll &&
                y < settings_viewport_end() && ev->x0 >= 36 && ev->x0 < 648) {
                app_settings_set_shelf_style(s_shelf_style_ids[i]);
                s_page = settings_back();
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BLE_SCAN && y < 190) {
        ble_pt_scan_stop(); s_page = SETTINGS_BLUETOOTH; s_ble_scroll = 0; return APP_REDRAW_PAGE;
    }
#if KIIKO_BOARD_METALIO47
    // 学习页的返回：先停采集，别让麦克风跟着设置页继续开。
    // Leaving the learn page stops capture first so the mic does not follow the settings page.
    if (s_page == SETTINGS_VOICE_LEARN && y < 190) {
        voice_turn_learn_cancel();
        s_page = SETTINGS_VOICE;
        return APP_REDRAW_PAGE;
    }
    // 语音页返回：识别测试跟着关掉，麦不留。/ Leaving the voice page also drops the recognition test.
    if (s_page == SETTINGS_VOICE && y < 190) voice_test_stop();
#endif
    if (s_page != SETTINGS_MAIN && y < 190) { s_page = settings_back(); (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
#if KIIKO_BOARD_METALIO47
#endif
#if KIIKO_BOARD_METALIO47
    if (s_page == SETTINGS_VOICE) {
        if (ui_rect_hit((EpdRect){36, 276, 612, 80}, ev->x0, y)) {
            // 一个方向都没学才拦：只教「上一页」或「下一页」一个也能开。
            // Block only when nothing is taught: a single direction is enough to enable.
            if (!app_settings_voice_turn() && voice_learned_words() == 0) {
                snprintf(s_notice, sizeof(s_notice), "先学「上一页」或「下一页」任一个，再回来开启");
                voice_learn_open();
                s_page = SETTINGS_VOICE_LEARN;
                return APP_REDRAW_PAGE;
            }
            app_settings_set_voice_turn(!app_settings_voice_turn());
            s_notice[0] = 0;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit((EpdRect){36, 356, 612, 80}, ev->x0, y)) {
            voice_learn_open();
            s_page = SETTINGS_VOICE_LEARN;
            return APP_REDRAW_PAGE;
        }
        // 阈值行：只认看得见的 − / + 按钮（与绘制共用同一份矩形）；自动状态下首次点按取引擎当前自动值转手动。
        // Threshold row: only the visible − / + buttons count (same rects as rendering); in auto the first tap adopts the
        // engine's current auto value and goes manual.
        const bool thr_tap = ui_rect_hit(thr_button_rect(false), ev->x0, y) ||
                             ui_rect_hit(thr_button_rect(true), ev->x0, y);
        if (thr_tap) {
            const bool thr_raise = ui_rect_hit(thr_button_rect(true), ev->x0, y);
            unsigned v = app_settings_voice_threshold();  // 0.1 计数 / tenths
            int nv = v ? (int)v : (int)(voice_turn_accept() * 10.0f + 0.5f);
            if (nv < 20) nv = 20;
            if (nv > 200) nv = 200;
            nv += thr_raise ? 1 : -1;  // 单步 0.1 / one tenth per tap
            if (nv < 20) nv = 20;
            if (nv > 200) nv = 200;
            app_settings_set_voice_threshold((unsigned)nv);
            voice_turn_set_accept((float)nv / 10.0f);
            snprintf(s_notice, sizeof(s_notice), "阈值已设为 %.1f，清除学习可恢复自动", (double)nv / 10.0);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit((EpdRect){36, 516, 612, 80}, ev->x0, y)) {
            voice_turn_learn_erase();
            app_settings_set_voice_turn(false);
            app_settings_set_voice_threshold(0);  // 模板清了，阈值一起回自动 / templates cleared, so is the threshold
            voice_turn_set_accept(0.0f);
            snprintf(s_notice, sizeof(s_notice), "已清除全部语音模板与阈值");
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit((EpdRect){36, 596, 612, 80}, ev->x0, y)) {
            if (s_voice_test) {
                voice_test_stop();
            } else {
                (void)voice_turn_init();
                voice_turn_match_t m;
                voice_turn_last_match(&m);
                s_voice_test_seq0 = m.seq;  // 只显示开测之后的新结果 / show only verdicts from after the switch
                s_voice_test = true;
                voice_turn_set_listen(true);
            }
            s_notice[0] = 0;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_VOICE_LEARN) {
        // 每个方向自己的按钮：本方向录音中按下=结束；还在存上一段时不接新活；
        // 其余情况（含点了另一个方向）都切到该方向开课并立即开始录音。
        // One button per direction: a tap stops its own take; a still-saving take accepts no new
        // work; anything else (including tapping the other direction) switches and starts there.
        for (int w = 0; w < VOICE_TURN_WORD_COUNT; ++w) {
            if (!ui_rect_hit(voice_learn_button(w), ev->x0, y)) continue;
            const voice_turn_learn_state_t st = voice_turn_learn_poll();
            if (s_voice_word == w && st == VOICE_TURN_LEARN_RECORD) {
                voice_turn_learn_record_stop();
                return APP_REDRAW_PAGE;
            }
            if (st == VOICE_TURN_LEARN_SPOKE) return APP_REDRAW_NONE;  // 还在存上一段 / the previous take is still saving
            s_voice_word = w;
            (void)voice_turn_learn_begin((voice_turn_word_t)w);
            (void)voice_turn_learn_record_start();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
#endif
    if (s_page == SETTINGS_LOCK_PIN) return APP_REDRAW_NONE;
    if (s_page == SETTINGS_AUTO_LOCK) {
        const uint8_t values[] = {1, 5, 10, 0};
        for (int i = 0; i < 4; ++i) if (ui_rect_hit((EpdRect){36, 272 + i * 112, 612, 92}, ev->x0, y)) {
            app_settings_set_auto_lock_minutes(values[i]);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    if (s_page == SETTINGS_WIFI) {
        if (y >= 408 && y < 478) {
            extern const app_desc_t app_transfer;
            app_transfer_request_wifi_setup();
            ctx->request_app = &app_transfer;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_SYSTEM_FONT) {
        system_font_layout_t l=system_font_layout();
        if (ui_rect_hit(l.size,ev->x0,y)) {
            unsigned prior=app_settings_system_font_size();
            unsigned next=prior>=200?100:prior+10;
            app_settings_set_system_font_size(next);
            if (app_settings_system_font_size()!=next) {
                snprintf(s_notice,sizeof(s_notice),"保存失败，请重试");return APP_REDRAW_PAGE;
            }
            s_font_page=0;
            if (s_main_scroll>SETTINGS_SCROLL_MAX) s_main_scroll=SETTINGS_SCROLL_MAX;
            ui_text_set_system_scale(true);s_notice[0]=0;return APP_REDRAW_PAGE;
        }
#if KIIKO_BOARD_METALIO47
        for(int next=0;next<2;++next)if(ui_rect_hit(system_font_page_button(next),ev->x0,y)) {
            int pages=(ttf_font_count()+l.slots)/l.slots;
            int page=s_font_page+(next?1:-1);
            if(page<0||page>=pages)return APP_REDRAW_NONE;
            s_font_page=page;s_scroll_present_pending=true;return APP_REDRAW_AREA;
        }
#endif
        for (int slot=0;slot<l.slots;++slot) {
            if (!ui_rect_hit(system_font_item_rect(slot),ev->x0,y)) continue;
            int index=s_font_page*l.slots+slot;
            if (index>ttf_font_count()) return APP_REDRAW_NONE;
            const ttf_font_item_t *item=index?ttf_font_item(index-1):NULL;
            if (index && !item) return APP_REDRAW_NONE;
            snprintf(s_font_sync_path,sizeof(s_font_sync_path),"%s",item?item->path:"");
            app_settings_set_system_font_path(s_font_sync_path);
            if (strcmp(app_settings_system_font_path(),s_font_sync_path)) {
                snprintf(s_notice,sizeof(s_notice),"保存失败，请重试");return APP_REDRAW_PAGE;
            }
            // 手动选定系统字体即解除异常启动的内建锁定；否则周期重试会把它打回内建。
            // An explicit system-face pick lifts the abnormal-start lock; otherwise the periodic
            // retry would revert it.
            app_font_manual_committed();
            app_font_activate_system();s_notice[0]=0;
            s_font_sync_confirm=strcmp(app_settings_font_path(),s_font_sync_path)!=0;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_MAIN_REFRESH) {
        for (int i = 0; i < 3; ++i) {
            if (!ui_rect_hit((EpdRect){36, 298 + i * 158, 612, 132}, ev->x0, y)) continue;
            app_settings_set_main_refresh_mode((app_main_refresh_mode_t)i);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
#if KIIKO_BOARD_METALIO47
    if (s_page == SETTINGS_AI_KEY) {
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (ev->y0 < 190) { s_page = settings_back(); (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        int ay = ev->y0;
        if (ay >= 330 && ay < 426) { s_ai_pick_target = 0; s_ai_pick_count = APP_AI_CHOOSER_COUNT; settings_open(SETTINGS_AI_PICK); return APP_REDRAW_PAGE; }
        if (ay >= 446 && ay < 542) { s_ai_pick_target = 1; s_ai_pick_count = APP_AI_ACTION_COUNT; settings_open(SETTINGS_AI_PICK); return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_AI_PICK) {
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (ev->y0 < 190) { s_page = settings_back(); return APP_REDRAW_PAGE; }
        int ay = ev->y0;
        if (ay < 298 || ay >= 298 + s_ai_pick_count * 116) return APP_REDRAW_NONE;
        int index = (ay - 298) / 116;
        app_ai_action_t chosen = (app_ai_action_t)index;
        esp_err_t err = s_ai_pick_target ? app_settings_set_ai_long(chosen) : app_settings_set_ai_short(chosen);
        if (err != ESP_OK) { snprintf(s_notice,sizeof(s_notice),"保存失败，请重试"); return APP_REDRAW_PAGE; }
        s_page = settings_back();
        return APP_REDRAW_PAGE;
    }
#endif
    if (s_page == SETTINGS_SYSTEM_SIZE) {
        for (int index = 0; index < 11; ++index) {
            EpdRect box = {36 + (index % 2) * 316, 298 + (index / 2) * 122, 296, 102};
            if (!ui_rect_hit(box, ev->x0, y)) continue;
            app_settings_set_system_font_size((uint8_t)(100 + index * 10));
            ui_text_set_system_scale(true);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_SYSTEM_CONTRAST) {
        if (y < 298 || y >= 878) return APP_REDRAW_NONE;
        int index = (y - 298) / 116;
        app_settings_set_system_contrast((uint8_t)(100 + index * 10));
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_LOCK_STYLE) {
        if (y >= 300 && y < 432) {
#if KIIKO_BOARD_METALIO47
            return settings_open_lock_images(ctx,NULL,false);
#else
            s_bookmark_picker=false;wallpaper_scan();
            s_page = SETTINGS_WALLPAPER;
            return APP_REDRAW_PAGE;
#endif
        }
        if (y >= 454 && y < 586) {
            app_settings_set_lock_style(0);
            return APP_REDRAW_PAGE;
        }
        if (y >= 608 && y < 740) {
            app_settings_set_lock_style(2);
#if KIIKO_BOARD_METALIO47
#else
            s_collage_prepare_pending = true;
#endif
            return APP_REDRAW_PAGE;
        }
        if(y>=762 && y<894){
#if KIIKO_BOARD_METALIO47
            return settings_open_lock_images(ctx,NULL,true);
#else
            s_bookmark_picker=true;wallpaper_scan();s_page=SETTINGS_WALLPAPER;
            return APP_REDRAW_PAGE;
#endif
        }
        if (y >= 1010 && app_settings_wallpaper_path()[0]) {
#if KIIKO_BOARD_METALIO47
            return settings_open_lock_images(ctx,NULL,false);
#else
            s_bookmark_picker=false;wallpaper_scan();
            s_page = SETTINGS_WALLPAPER;
            return APP_REDRAW_PAGE;
#endif
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_WALLPAPER) {
        if (y < 292 || y >= 1028) return APP_REDRAW_NONE;
        int index = s_wallpaper_page * 8 + (y - 292) / 92;
        if (index >= s_wallpaper_count) return APP_REDRAW_NONE;
#if KIIKO_BOARD_METALIO47
        return settings_open_lock_images(ctx,s_wallpapers[index].path,s_bookmark_picker);
#else
        s_wallpaper_selected = index;
        bookmark_preview_prepare(ctx->fb);
        s_wallpaper_confirm = false;
        s_page = SETTINGS_WALLPAPER_PREVIEW;
        return APP_REDRAW_PAGE;
#endif
    }
    if (s_page == SETTINGS_TIME) {
        if (y >= 407 && y < 477) {
            s_notice[0] = 0; s_sync_pending = true;
            snprintf(s_notice, sizeof(s_notice), "正在获取网络时间…");
            return APP_REDRAW_PAGE;
        }
        if (y >= 518 && y < 588) { time_open(); return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
#if KIIKO_BOARD_METALIO47
    if (s_page == SETTINGS_POWER_SLEEP) {
        for (int i = 0; i < SLEEP_OFF_ROWS; ++i) {
            if (ui_rect_hit((EpdRect){36, 272 + i * 96, 612, 84}, ev->x0, ev->y0)) {
                app_settings_set_sleep_off_code(k_sleep_off_order[i]);
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
#else
    if (s_page == SETTINGS_POWER_SLEEP) {
        if (y >= 278 && y < 402) {
            app_settings_set_staged_shutdown(false);
            return APP_REDRAW_PAGE;
        }
        if (y >= 414 && y < 538) {
            app_settings_set_staged_shutdown(true);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
#endif
    if (s_page == SETTINGS_CONFIG) {
        if (y >= 561 && y < 644) {
            s_config_confirm = false;
            esp_err_t err = app_settings_backup_save();
            snprintf(s_notice, sizeof(s_notice), "%s",
                     err == ESP_OK ? "已保存到 TF 卡根目录" :
                     err == ESP_ERR_INVALID_STATE ? "未识别到 TF 卡，请插卡后重试" :
                     "保存失败，请检查 TF 卡剩余空间");
            return APP_REDRAW_PAGE;
        }
        if (y >= 681 && y < 764) {
            if (!s_config_confirm) {
                s_config_confirm = true;
                snprintf(s_notice, sizeof(s_notice), "恢复将覆盖当前设置，请再点按一次确认");
                return APP_REDRAW_PAGE;
            }
            s_config_confirm = false;
            esp_err_t err = app_settings_backup_restore();
            if (err == ESP_OK) {
                ttf_font_scan();
                app_font_activate_system();
                ui_text_set_system_scale(true);
                // 阅读记录恢复后令首页和书架重新取进度、时长与排序。
                // Rebuild home and shelf caches after restoring reading records.
                book_store_notify_changed();
            }
            char saved_ssid[33] = {0};
            bool wifi_saved = false;
            if (err == ESP_OK)
                (void)read_pico_transfer_get_saved_wifi(saved_ssid, &wifi_saved);
            snprintf(s_notice, sizeof(s_notice), "%s",
                     err == ESP_OK ? (wifi_saved ? "配置与阅读记录已恢复；WiFi 请重新连接" :
                                                   "配置与阅读记录已恢复") :
                     err == ESP_ERR_INVALID_STATE ? "未识别到 TF 卡，请插卡后重试" :
                     err == ESP_ERR_NOT_FOUND ? "未找到配置备份文件" :
                     err == ESP_ERR_INVALID_RESPONSE ? "配置文件损坏或版本不兼容" :
                     "恢复未完成，请重试");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BLUETOOTH) {
        const ble_layout_t l = ble_layout();
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        // 版式给的是屏幕坐标（已减去滚动量），所以这里直接用 y 比，不再加 scroll。
        // The layout yields screen coordinates (scroll already subtracted), so compare y directly.
        const int ty = y;

        // 学习时轻点取消；识别结果轻点后返回开关。/ Tap to cancel learning or dismiss the received-key result.
        if (s_ble_learning) {
            s_ble_learning = 0;
            snprintf(s_ble_notice, sizeof(s_ble_notice), "已取消学习");
            return APP_REDRAW_PAGE;
        }
        if (s_ble_feedback[0] && ty >= l.toggle_top && ty < l.toggle_top + 126) {
            s_ble_feedback[0] = 0;
            return APP_REDRAW_PAGE;
        }
        if (ty < 160 || ty >= settings_viewport_end()) return APP_REDRAW_NONE;

#if KIIKO_BOARD_METALIO47
        // 「蓝牙音频」开关：关掉即给音频通路断电，打开则起栈并自动连接上次耳机。
        // The Bluetooth-audio switch: off powers the path down, on brings the stack up and reconnects the last headset.
        if (ty >= l.hp_bt_top && ty < l.hp_bt_top + 126) {
            const bool on = !app_settings_bt_enabled();
            music_bt_set_enabled(on);
            snprintf(s_ble_notice, sizeof(s_ble_notice), on ? "已开启蓝牙音频" : "已关闭蓝牙音频");
            if (on && music_player_start() == ESP_OK) {
                uint8_t bda[6];
                char name[36];
                bool have = app_settings_bt_paired_count() > 0 &&
                            app_settings_bt_paired_get(0, bda, name, sizeof(name));
                if (!have) have = app_settings_bt_last_device(bda, name, sizeof(name));
                if (have && bt_audio_connect(bda) == ESP_OK)
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "正在连接耳机…");
            }
            return APP_REDRAW_PAGE;
        }
        // 发声测试按钮：没连耳机时只给提示，不空发一条注定没有听众的测试音。
        // Test-tone button: with no headset, just say so instead of firing a tone at nobody.
        {
            const EpdRect tone_btn = ble_button_rect(l.hp_tone_top);
            if (ev->x0 >= tone_btn.x && ev->x0 < tone_btn.x + tone_btn.width &&
                ty >= tone_btn.y && ty < tone_btn.y + tone_btn.height) {
                if (bt_audio_test_tone_active()) {
                    bt_audio_test_tone_stop();
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "已停止测试音");
                } else if (!bt_audio_link_active()) {
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "请先连接耳机再测试");
                } else if (bt_audio_play_test_tone(2000) == ESP_OK) {
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "播放测试音：有声说明蓝牙通路正常");
                } else {
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "测试音未发出，请查看日志");
                }
                return APP_REDRAW_PAGE;
            }
        }
        // 耳机按钮：已连接=断开，搜索中=停止，否则扫描（先起音频栈再发现）。
        // Headset button: disconnect when linked, stop while scanning, otherwise scan (audio stack first).
        {
            const EpdRect hp_btn = ble_button_rect(l.hp_status_top);
            if (ev->x0 >= hp_btn.x && ev->x0 < hp_btn.x + hp_btn.width &&
                ty >= hp_btn.y && ty < hp_btn.y + hp_btn.height) {
                if (bt_audio_link_active()) {
                    bt_audio_disconnect();
                    s_bt_show = false;
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "已断开耳机");
                } else if (bt_audio_scanning()) {
                    bt_audio_scan_stop();
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "已停止搜索");
                } else if (!app_settings_bt_enabled()) {  // 关着时扫描必失败，先给明确指引 / scanning is doomed while off; say so first
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "蓝牙音频已关闭，请先开启");
                } else if (music_player_start() != ESP_OK) {
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "音频栈未就绪，请稍后重试");
                } else if (bt_audio_scan_start(15000) == ESP_OK) {
                    s_bt_show = true;
                    s_ble_scroll = 0;
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "正在搜索附近耳机…");
                } else {
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "蓝牙未就绪，请稍后重试");
                }
                return APP_REDRAW_PAGE;
            }
            // 扫描结果行：停扫并连接该设备。/ A result row: stop scanning and connect to it.
            for (int i = 0; i < l.hp_dev_count; ++i) {
                if (ty < l.hp_dev_top[i] || ty >= l.hp_dev_top[i] + BLE_ROW_H) continue;
                const bt_audio_device_t *dev = bt_audio_device((uint8_t)i);
                if (!dev) return APP_REDRAW_NONE;
                bt_audio_scan_stop();
                if (bt_audio_connect(dev->bda) == ESP_OK) {
                    app_settings_set_bt_last_device(dev->bda, dev->name);  // 记住，供自动连接 / remember for auto-reconnect
                    app_settings_bt_paired_add(dev->bda, dev->name);       // 记入已配对表 / remember in the paired table
                    s_bt_show = false;
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "正在连接耳机…");
                } else snprintf(s_ble_notice, sizeof(s_ble_notice), "连接失败，请重试");
                return APP_REDRAW_PAGE;
            }
            // 「自动连接上次设备」开关。/ The auto-reconnect switch.
            if (ty >= l.hp_auto_top && ty < l.hp_auto_top + 126) {
                app_settings_set_bt_auto_connect(!app_settings_bt_auto_connect());
                return APP_REDRAW_PAGE;
            }
            // 「连接耳机时侧键仅调音量」开关。/ The side-keys-volume-only switch.
            if (ty >= l.hp_volkey_top && ty < l.hp_volkey_top + 126) {
                app_settings_set_bt_volume_keys(!app_settings_bt_volume_keys());
                return APP_REDRAW_PAGE;
            }
            // 已配对耳机行：右侧按钮忘记（连着先断开），行本身连接/断开。
            // Paired headset rows: the button forgets (disconnecting first), the row connects or disconnects.
            for (int i = 0; i < l.hp_pair_rows && i < APP_BT_PAIRED_MAX; ++i) {
                uint8_t bda[6];
                char name[36];
                if (!app_settings_bt_paired_get((uint8_t)i, bda, name, sizeof(name))) continue;
                const EpdRect btn = ble_button_rect(l.hp_pair_top[i]);
                if (ev->x0 >= btn.x && ev->x0 < btn.x + btn.width && ty >= btn.y && ty < btn.y + btn.height) {
                    uint8_t link[6];
                    if (bt_audio_link_addr(link) && !memcmp(link, bda, 6)) bt_audio_disconnect();
                    app_settings_bt_paired_remove(bda);
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "已忘记该耳机");
                    return APP_REDRAW_PAGE;
                }
                if (ty >= l.hp_pair_top[i] && ty < l.hp_pair_top[i] + BLE_ROW_H) {
                    uint8_t link[6];
                    if (bt_audio_link_addr(link) && !memcmp(link, bda, 6)) {
                        bt_audio_disconnect();
                        snprintf(s_ble_notice, sizeof(s_ble_notice), "已断开耳机");
                    } else {
                        bt_audio_scan_stop();
                        if (bt_audio_connect(bda) == ESP_OK) {
                            app_settings_set_bt_last_device(bda, name);
                            snprintf(s_ble_notice, sizeof(s_ble_notice), "正在连接耳机…");
                        } else snprintf(s_ble_notice, sizeof(s_ble_notice), "连接失败，请重试");
                    }
                    return APP_REDRAW_PAGE;
                }
            }
        }
#endif

        if (l.toggle_top >= 0 && ty >= l.toggle_top && ty < l.toggle_top + 126) {
            app_settings_set_ble_turner(!app_settings_ble_turner());
            // 明确关掉时把失败计数和原因一起清掉：否则自锁之后用户没有重试的途径。
            // Switching it off explicitly clears the failure count and reason; without this a
            // self-locked stack leaves the user no way to try again.
            if (!app_settings_ble_turner()) ble_pt_reset_failure();
            snprintf(s_ble_notice, sizeof(s_ble_notice), "%s",
                     app_settings_ble_turner() ? "已开启，正在等待设备" : "已关闭，蓝牙内存已释放");
            return APP_REDRAW_PAGE;
        }
        if (ty >= l.scan_entry_top && ty < l.scan_entry_top + BLE_ROW_H) {
            if (!app_settings_ble_turner() || !ble_pt_running()) {
                snprintf(s_ble_notice, sizeof(s_ble_notice), "请先开启翻页器"); return APP_REDRAW_PAGE;
            }
            // 进扫描页就开持续扫描，离页时停掉。/ Scan continuously while the page is open.
            ble_pt_scan_start(BLE_PT_SCAN_FOREVER);
            s_ble_scroll = 0;
            s_ble_notice[0] = 0;
            s_page = SETTINGS_BLE_SCAN;
            return APP_REDRAW_PAGE;
        }
        for (int i = 0; i < 2; ++i) {
            const EpdRect btn = ble_button_rect(l.learn_top[i]);
            // 命中判定必须同时比 x：按钮的纵向范围几乎覆盖整行，只比 y 会让整行都算按钮。
            // The hit test needs the horizontal test too: the button's vertical span covers almost
            // the whole row, so comparing y alone makes the entire row count as the button.
            if (ev->x0 >= btn.x && ev->x0 < btn.x + btn.width && ty >= btn.y && ty < btn.y + btn.height) {
                if (!ble_pt_connected()) {
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "请先连接翻页器再学习按键");
                    return APP_REDRAW_PAGE;
                }
                ble_pt_raw_t stale;
                while (ble_pt_pop_raw(&stale)) {}
                // 把此刻那一帧当基准：点「学习」时用户还没按键，外设发的就是静止帧。有些遥控器
                // 的静止帧带着非零常量，不这样标一下，那串常量会被当成一次按下学进去。
                // Pin the frame present now as the baseline: the user has not pressed yet, so what the
                // remote sends is its resting frame. Some remotes keep non-zero constants there, and
                // without this the constants get learned as if they were a press.
                ble_pt_learn_begin();
                s_ble_feedback[0] = s_ble_notice[0] = 0;
                s_ble_learning = i + 1;
                s_ble_scroll = 0;
                return APP_REDRAW_PAGE;
            }
        }
        // 已配对行：右侧按钮忘记，行本身连接。/ Bonded rows: the button forgets, the row connects.
        for (int i = 0; i < l.bond_rows && i < BLE_PT_MAX_BONDS; ++i) {
            const ble_pt_bond_t *bond = ble_pt_bond((uint8_t)i);
            if (!bond) continue;
            const EpdRect btn = ble_button_rect(l.bond_top[i]);
            if (ev->x0 >= btn.x && ev->x0 < btn.x + btn.width && ty >= btn.y && ty < btn.y + btn.height) {
#if KIIKO_BOARD_METALIO47
                const esp_err_t forgotten=ble_pt_forget(bond->addr);
                snprintf(s_ble_notice,sizeof(s_ble_notice),"%s",forgotten==ESP_OK?"已删除该配对":
                    forgotten==ESP_ERR_INVALID_STATE?"请先开启蓝牙，再删除配对":"删除失败，请重试");
#else
                if (ble_pt_connected()) ble_pt_disconnect();
                ble_pt_forget(bond->addr);
                snprintf(s_ble_notice, sizeof(s_ble_notice), "已删除该配对");
#endif
                return APP_REDRAW_PAGE;
            }
            if (ty >= l.bond_top[i] && ty < l.bond_top[i] + BLE_ROW_H) {
                if (ble_pt_connected() || ble_pt_connecting()) ble_pt_disconnect();
                snprintf(s_ble_notice, sizeof(s_ble_notice),
                         ble_pt_connect(bond->addr) == ESP_OK ? "正在连接…" : "连接失败，请重试");
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BLE_SCAN) {
        const ble_scan_layout_t l = ble_scan_layout();
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (y < 160 || y >= settings_viewport_end()) return APP_REDRAW_NONE;
        const int ty = y + s_ble_scroll;
        // 选一台就连，连上后退回二级页。扫描在离页时停。
        // Picking one connects, then returns to level two; scanning stops on the way out.
        for (int i = 0; i < l.device_rows; ++i) {
            const ble_pt_device_t *dev = ble_scan_visible_at(i);
            if (!dev) continue;
            const int top = l.devices_top + i * BLE_ROW_H;
            if (ty < top || ty >= top + BLE_ROW_H) continue;
            ble_pt_scan_stop();
            if (ble_pt_connected() || ble_pt_connecting()) ble_pt_disconnect();
            snprintf(s_ble_notice, sizeof(s_ble_notice),
                     ble_pt_connect(dev->addr) == ESP_OK ? "正在连接…" : "连接失败，请重试");
            s_page = SETTINGS_BLUETOOTH;
            s_ble_scroll = 0;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (y<160 || y>=UI_NAV_TOP || ev->x0<36 || ev->x0>=648) return APP_REDRAW_NONE;
    if (settings_group_page(s_page)) {
        if (y<SETTINGS_GROUP_CARD_Y) return APP_REDRAW_NONE;
        const int row=(y-SETTINGS_GROUP_CARD_Y)/SETTINGS_ROW_H;
        if (s_page==SETTINGS_GROUP_CONNECT) {
            if(row==0) {
                extern const app_desc_t app_transfer;app_transfer_request_wifi_setup();ctx->request_app=&app_transfer;return APP_REDRAW_NONE;
            }
            if(row==1) {
                settings_open(SETTINGS_BLUETOOTH);s_ble_scroll=s_ble_learning=0;s_ble_notice[0]=s_ble_feedback[0]=0;
#if KIIKO_BOARD_METALIO47
                bt_audio_scan_stop();s_bt_show=false;  // 每次进页回到收起的初始态 / re-enter collapsed every time
                hp_auto_connect_on_entry();            // 进页即自动连旧设备 / entering connects the old headset
#endif
                ble_pt_raw_t unused;while(ble_pt_pop_raw(&unused)){}return APP_REDRAW_PAGE;
            }
            if(row==2){settings_open(SETTINGS_TIME);return APP_REDRAW_PAGE;}
            return APP_REDRAW_NONE;
        }
        if (s_page==SETTINGS_GROUP_DISPLAY) {
            // 前光排在第一行（仅 Metalio），其后是字体、对比度、刷新模式、首页强刷。
            // Frontlight leads the list on Metalio, then font, contrast, refresh mode, full refresh.
#if KIIKO_BOARD_METALIO47
            if(row==0){settings_open(SETTINGS_FRONTLIGHT);return APP_REDRAW_PAGE;}
            if(row==1) { ttf_font_scan();s_font_page=0;s_notice[0]=0;settings_open(SETTINGS_SYSTEM_FONT); }
            else if(row==2)settings_open(SETTINGS_SYSTEM_CONTRAST);
            else if(row==3)settings_open(SETTINGS_MAIN_REFRESH);
            else if(row==4)app_settings_set_home_full_refresh(!app_settings_home_full_refresh());
#else
            if(row==0) { ttf_font_scan();s_font_page=0;s_notice[0]=0;settings_open(SETTINGS_SYSTEM_FONT); }
            else if(row==1)settings_open(SETTINGS_SYSTEM_CONTRAST);
            else if(row==2)settings_open(SETTINGS_MAIN_REFRESH);
            else if(row==3)app_settings_set_home_full_refresh(!app_settings_home_full_refresh());
#endif
            return APP_REDRAW_PAGE;
        }
        if (s_page==SETTINGS_GROUP_POWER) {
            // 电源组：自动休眠锁屏 / 关机睡眠 / 浅睡播放音乐（仅 Metalio）/ 锁屏样式 / 锁屏密码。
            // 唤醒固定为单击，不再有可切换的双击行。
            // Power group: auto lock / shutdown sleep / music through light sleep (Metalio only) /
            // lock style / lock PIN. Waking is always a single press.
            if(row==0)settings_open(SETTINGS_AUTO_LOCK);
#if KIIKO_BOARD_METALIO47
            else if(row==1)settings_open(SETTINGS_POWER_SLEEP);
            else if(row==2)app_settings_set_music_in_sleep(!app_settings_music_in_sleep());
            else if(row==3)settings_open(SETTINGS_LOCK_STYLE);
            else if(row==4){settings_open(SETTINGS_LOCK_PIN);s_notice[0]=0;}
#else
            else if(row==1)settings_open(SETTINGS_LOCK_STYLE);
            else if(row==2){settings_open(SETTINGS_LOCK_PIN);s_notice[0]=0;}
#endif
            return APP_REDRAW_PAGE;
        }
        if (s_page==SETTINGS_GROUP_PERSONAL) {
            if(row==0){settings_open(SETTINGS_SHELF_STYLE);s_style_scroll=0;}
            else if(row==1)profile_editor_open(true);
#if KIIKO_BOARD_METALIO47
            else if(row==2)metalio_haptic_set_enabled(!metalio_haptic_enabled());
#endif
            return APP_REDRAW_PAGE;
        }
        // 系统维护：升级 / 内存卡恢复（仅 Metalio）/ 保存与恢复 / 屏幕修复 / BOOT 刷机 / 恢复出厂。
        // System maintenance: upgrade, card recovery (Metalio only), backup, panel repair, BOOT, reset.
        if(row==0){upgrade_open();return APP_REDRAW_PAGE;}
#if KIIKO_BOARD_METALIO47
        if(row==1){settings_open(SETTINGS_SD_RECOVERY);s_sd_notice[0]=0;return APP_REDRAW_PAGE;}
        if(row==2){settings_open(SETTINGS_CONFIG);s_config_confirm=false;s_notice[0]=0;return APP_REDRAW_PAGE;}
        if(row==3) {
#else
        if(row==1){settings_open(SETTINGS_CONFIG);s_config_confirm=false;s_notice[0]=0;return APP_REDRAW_PAGE;}
        if(row==2) {
#endif
            // 屏幕在刷新中途断电会停在半帧，常规差分刷新永远盖不掉那道条纹；点一下就整屏盲刷
            // 一遍，然后自己把当前页面（含底栏）推回面板。
            // A power cut mid-refresh leaves the panel half-driven, and an ordinary differential
            // refresh can never cover that stripe. One tap runs the blind full-screen pass, which
            // then pushes the current page -- navigation bar included -- back onto the panel.
            s_panel_repair_pending=true;
            snprintf(s_notice,sizeof(s_notice),"屏幕修复中…");
            return APP_REDRAW_PAGE;
        }
#if KIIKO_BOARD_METALIO47
        if(row==4){
#else
        if(row==3){
#endif
            s_boot_pending=false;
#if KIIKO_BOARD_METALIO47
            s_boot_notice[0]=0;
#endif
            settings_open(SETTINGS_BOOT);
            return APP_REDRAW_PAGE;
        }
#if KIIKO_BOARD_METALIO47
        if(row==5){settings_open(SETTINGS_FACTORY_RESET);s_reset_confirm=s_reset_pending=false;s_reset_notice[0]=0;}
#else
        if(row==4){settings_open(SETTINGS_FACTORY_RESET);s_reset_confirm=s_reset_pending=false;s_reset_notice[0]=0;}
#endif
        return APP_REDRAW_PAGE;
    }
    y+=s_main_scroll;
    if (y>=180 && y<180+settings_profile_height()) { settings_open(SETTINGS_PROFILE);return APP_REDRAW_PAGE; }
    if (y>=SETTINGS_CATEGORY_Y && y<SETTINGS_CATEGORY_Y+SETTINGS_CATEGORY_ROWS*SETTINGS_ROW_H) {
        const int row=(y-SETTINGS_CATEGORY_Y)/SETTINGS_ROW_H;
        if(row==0)settings_open(SETTINGS_GROUP_CONNECT);
        else if(row==1)settings_open(SETTINGS_GROUP_DISPLAY);
        else if(row==2)settings_open(SETTINGS_GROUP_POWER);
        else if(row==3)settings_open(SETTINGS_GROUP_PERSONAL);
#if KIIKO_BOARD_METALIO47
        else if(row==4){
            // 语音翻页是独立入口：进页先确保引擎在，才读得到"学会了几个词"。
            // Voice page-turn is its own entry; bring the engine up first so the page can report
            // how many words are learned.
            (void)voice_turn_init();
            s_notice[0]=0;
            settings_open(SETTINGS_VOICE);
        }
        else if(row==5)settings_open(SETTINGS_GROUP_SYSTEM);
        else if(row==6)settings_open(SETTINGS_AI_KEY);
#else
        else settings_open(SETTINGS_GROUP_SYSTEM);
#endif
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
#if KIIKO_BOARD_METALIO47
    if(app_sd_recovery_busy())return APP_REDRAW_NONE;
    if(s_page==SETTINGS_ONLINE_FONTS){
        ui_online_fonts_close(&s_online_fonts);ttf_font_scan();s_font_page=0;s_page=SETTINGS_SYSTEM_FONT;
        return APP_REDRAW_PAGE;
    }
#endif
    s_reset_confirm=s_reset_pending=false;
#if KIIKO_BOARD_METALIO47
    if (s_page==SETTINGS_BOOT) { s_boot_pending=false; app_cancel_download(); }
#else
#endif
    if(s_font_sync_confirm) {
        s_font_sync_confirm=false;s_notice[0]=0;
        if(key!=1)return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_PIN_ENTRY) { pin_setup_close(); s_page = SETTINGS_LOCK_PIN; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_UPGRADE) {
        s_upgrade_job = UPGRADE_JOB_NONE;
        if (s_local_pending || s_upgrade_restart) return APP_REDRAW_NONE;
        pico_online_cancel_join(); s_upgrade_confirm = false;
    }
    if (s_page == SETTINGS_BLE_SCAN) {
        ble_pt_scan_stop();
        if (key != 1) { s_page = SETTINGS_BLUETOOTH; s_ble_scroll = 0; return APP_REDRAW_PAGE; }
    }
    if (key == 1) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (s_page == SETTINGS_WALLPAPER_PREVIEW) {
        if (s_wallpaper_confirm) s_wallpaper_confirm = false;
        else { bookmark_preview_release(); s_page = SETTINGS_WALLPAPER; }
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_WALLPAPER) { s_page = SETTINGS_LOCK_STYLE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_AVATAR) { s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_TEXT_EDIT) { ui_keyboard_end(); s_page = s_editor_signature ? settings_back() : SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_PROFILE) { s_page = settings_back(); (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_TIME_EDIT) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
    if (s_page != SETTINGS_MAIN) { s_page = settings_back(); (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
    ui_nav_request(ctx, 0);
    return APP_REDRAW_NONE;
}
static void settings_exit(app_ctx_t *ctx) {
#if KIIKO_BOARD_METALIO47
    if(s_page==SETTINGS_ONLINE_FONTS)ui_online_fonts_close(&s_online_fonts);
    // 离开设置页一律停采集：学习流程可能还在等用户开口。
    // Always stop capture when leaving settings; the learn flow may still be waiting for speech.
    voice_turn_learn_cancel();
    voice_test_stop();  // 识别测试同样不留麦 / the recognition test leaves no mic running either
#endif
    s_reset_confirm=s_reset_pending=false;
#if KIIKO_BOARD_METALIO47
    s_boot_pending=false; app_cancel_download();
#else
#endif
    bookmark_preview_release();s_bookmark_picker=false;
    s_font_sync_confirm=false;s_font_sync_path[0]=0;
    pin_setup_close();
    s_collage_prepare_pending = false;
    s_upgrade_job = UPGRADE_JOB_NONE;
    (void)ctx; ui_keyboard_end(); pico_online_cancel_join(); ble_pt_scan_stop(); s_ble_learning = 0;
}
static void on_before_lock(app_ctx_t *ctx) {
#if KIIKO_BOARD_METALIO47
    if(s_page==SETTINGS_ONLINE_FONTS){ui_online_fonts_close(&s_online_fonts);ttf_font_scan();s_font_page=0;s_page=SETTINGS_SYSTEM_FONT;}
#endif
    s_reset_confirm=s_reset_pending=false;
#if KIIKO_BOARD_METALIO47
    s_boot_pending=false; app_cancel_download();
#else
#endif
    s_font_sync_confirm=false;s_font_sync_path[0]=0;
    if (s_page == SETTINGS_PIN_ENTRY) { pin_setup_close(); s_page = SETTINGS_LOCK_PIN; }
    s_upgrade_job = UPGRADE_JOB_NONE;
    (void)ctx; pico_online_cancel_join(); ble_pt_scan_stop(); s_ble_learning = 0;
    (void)ble_pt_stop(2000);
}
static bool no_menu_handle(app_ctx_t *ctx) { (void)ctx; return false; }
static void settings_media_lost(app_ctx_t *ctx) {
#if KIIKO_BOARD_METALIO47
    if(s_page==SETTINGS_ONLINE_FONTS){ui_online_fonts_close(&s_online_fonts);s_page=SETTINGS_SYSTEM_FONT;s_font_page=0;}
#endif
    bookmark_preview_release();
    (void)ctx; s_avatar_ready=s_avatar_pending=false; s_avatar_path[0]=0;
}
static void settings_media_ready(app_ctx_t *ctx) { (void)ctx; settings_prepare_profile(); }
static bool settings_edge_back(app_ctx_t *ctx) {
    (void)ctx;
#if KIIKO_BOARD_METALIO47
    if(app_sd_recovery_busy())return false;
#endif
    return s_page != SETTINGS_MAIN && !s_reset_pending && !s_local_pending &&
        !s_upgrade_restart && !s_pin_pending && !s_font_sync_confirm;
}
static bool settings_back_swipe_area(app_ctx_t *ctx,const ui_gesture_event_t *event) {
    (void)ctx;
    return s_page!=SETTINGS_TEXT_EDIT || event->y0<UI_SYSTEM_KEYBOARD_TOP;
}
static bool main_page_visible(app_ctx_t *ctx) { (void)ctx; return s_page == SETTINGS_MAIN; }
static bool settings_navigation_enabled(app_ctx_t *ctx) {
    (void)ctx;
#if KIIKO_BOARD_METALIO47
    if(app_sd_recovery_busy())return false;
    if(s_page==SETTINGS_ONLINE_FONTS)return false;
#endif
    return !s_reset_pending && !s_local_pending && !s_upgrade_restart && !s_pin_pending && !s_boot_pending;
}
#if KIIKO_BOARD_METALIO47
static bool settings_grayscale_ui(app_ctx_t *ctx) {
    (void)ctx;return settings_plain_list()||settings_gray_preview();
}
#endif
static bool settings_present(app_ctx_t *ctx, app_redraw_t redraw) {
#if KIIKO_BOARD_METALIO47
    if(settings_plain_list()&&(redraw==APP_REDRAW_PAGE||redraw==APP_REDRAW_FULL)) {
        // 进页与滚动保留相同字缘灰阶；根页切换由外层六/八相许可负责，不叠加列表尾段。
        // Entry and scroll retain the same glyph grays; root scope owns six/eight drives without stacking the list tail.
        const bool binary=display_system_binary_set(false);
        display_main_transition_cancel();s_scroll_present_pending=false;
        render(ctx,ctx->fb);
        const EpdRect area={0,0,UI_LOCK_WIDTH,UI_NAV_MARKER_SCAN_END};
        guard_draw_result(ctx->hl,redraw==APP_REDRAW_FULL?update_display_full(ctx->hl):
            s_page==SETTINGS_MAIN&&app_settings_main_refresh_mode()==APP_MAIN_REFRESH_WATER?
                update_display_main_turn(ctx->hl,area,E0470_TURN_RTL):
                update_display_area_diff_with(ctx->hl,display_main_normal_waveform(),MODE_GL16,area));
        display_system_binary_set(binary);
        return true;
    }
    if(s_page==SETTINGS_ONLINE_FONTS)return ui_online_fonts_present(&s_online_fonts,ctx,redraw);
    if(settings_gray_preview()&&(redraw==APP_REDRAW_PAGE||redraw==APP_REDRAW_FULL||redraw==APP_REDRAW_AREA)){
        if(ctx->request_app)return true;
        // 壁纸/透明书封及书架样式预览不继承系统二值化；同轮输出完整灰阶，退出后恢复原策略。
        // Wallpaper/transparent-cover/shelf previews bypass system thresholding; present full grays once and restore policy afterward.
        const bool binary=display_system_binary_set(false);
        display_main_transition_cancel();s_scroll_present_pending=false;
        render(ctx,ctx->fb);
        guard_draw_result(ctx->hl,redraw==APP_REDRAW_FULL?update_display_full(ctx->hl):
            update_display_area_full_with(ctx->hl,&E0470_FULL_WAVEFORM,MODE_GL16,
                (EpdRect){0,0,UI_LOCK_WIDTH,UI_LOCK_HEIGHT}));
        display_system_binary_set(binary);
        return true;
    }
#endif
    if (s_page == SETTINGS_PIN_ENTRY && (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL)) {
        display_main_transition_cancel(); render(ctx, ctx->fb);
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, (EpdRect){0,0,UI_LOCK_WIDTH,UI_NAV_REFRESH_END}));
        return true;
    }
    if (redraw == APP_REDRAW_AREA && s_page == SETTINGS_TEXT_EDIT) {
        if (s_input_settle) {
            guard_draw_result(ctx->hl, update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_input_area));
            s_input_settle = false;
            return true;
        }
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl,
            s_input_layout ? &E0470_WAVEFORM : &E0470_FOLLOW_WAVEFORM,
            s_input_layout ? MODE_GL16 : MODE_DU, s_input_area));
        return true;
    }
    if (redraw == APP_REDRAW_AREA && s_scroll_present_pending &&
        (s_page == SETTINGS_MAIN || s_page == SETTINGS_BLUETOOTH || s_page == SETTINGS_BLE_SCAN ||
         s_page == SETTINGS_SHELF_STYLE || s_page == SETTINGS_SYSTEM_FONT ||
         s_page == SETTINGS_WALLPAPER || s_page == SETTINGS_AVATAR)) {
        s_scroll_present_pending = false;
        render(ctx, ctx->fb);
        int top = s_page == SETTINGS_MAIN ? 160 : s_page == SETTINGS_SHELF_STYLE ? 242 : 190;
        EpdRect area = {0, top, UI_LOCK_WIDTH, settings_content_end() - top};
#if KIIKO_BOARD_METALIO47
        // 列表独立累计真实滚动；到期用原厂区域清理替换当次输出，不叠加松手/空闲补刷。
        // Accrue changed list scrolls independently; replace a due output with native regional cleanup, never release/idle settling.
        const bool binary=display_system_binary_set(false);
        display_main_transition_cancel();
        guard_draw_result(ctx->hl,update_display_settings_list(ctx->hl,(unsigned)s_page+1,area));
        display_system_binary_set(binary);
#else
        // 列表仅一次短灰阶差分；保留抗锯齿目标，取消主页替换、清白预刷与松手补刷。
        // Present the list once with short gray differences; retain AA targets without main substitution, white prepasses or release settling.
        display_main_transition_cancel();
        const EpdWaveform *wave=display_settings_scroll_waveform();
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl,wave,MODE_DU,area));
#endif
        return true;
    }
    if (s_page != SETTINGS_UPGRADE) return false;
    if (redraw == APP_REDRAW_PAGE) {
        display_main_transition_cancel();
        render(ctx, ctx->fb);
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM,
                          MODE_GL16, (EpdRect){0, 0, UI_LOCK_WIDTH, settings_content_end()}));
        return true;
    }
    if (redraw == APP_REDRAW_AREA) {
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_FOLLOW_WAVEFORM,
                          MODE_DU, upgrade_progress_area()));
        return true;
    }
    return false;
}

const app_desc_t app_device_settings = {
#if KIIKO_BOARD_METALIO47
    .binary_ui = true,
    .grayscale_ui = settings_grayscale_ui,
#else
    .binary_ui = false,
#endif
    .title = "设置 Settings", .detail = "显示、连接与设备", .enter_full = false,
    .owns_keys = true, .menu_handle_enabled = no_menu_handle,
    .main_page_visible = main_page_visible, .edge_back_enabled = settings_edge_back,
    .back_swipe_area=settings_back_swipe_area,
    .persistent_navigation=true,.navigation_enabled=settings_navigation_enabled,
    .on_enter = on_enter, .on_exit = settings_exit, .on_before_lock = on_before_lock, .render = render, .on_tick = on_tick, .on_gesture = on_gesture, .on_key = on_key,
    .present = settings_present, .on_media_lost=settings_media_lost, .on_media_ready=settings_media_ready,
};
