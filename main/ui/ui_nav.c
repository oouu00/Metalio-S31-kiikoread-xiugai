/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：绘制与命中五栏导航；细线图标分别表示首页、书架、音乐、文件管理和设置，图形来自 Lucide。
 * English: Draw and hit-test the five-tab navigation with thin-line home, books, music, files and settings icons from Lucide.
 * 用户增加点击反馈：共享返回键和底栏图标登记局部命中，主循环只在按下和复位推该控件。
 * User adds click feedback: register local hits for shared backs and navigation icons; the loop presents only that control on press and restoration.
 * 用户修订：所有模式把内容扫描截止到分隔线之间的8行固定为白色留白；滚动不能把列表画进这条扫描保护带。
 * User revision: every mode keeps the eight rows between content scans and the divider blank white; scrolling must not paint list content into this guard.
 * 用户修订：状态栏签名按系统字体和内建界面字体的共同安全宽度截断，阅读页复用小型字宽表，不重载系统字体。
 * User revision: fit status signatures to widths safe for the system and built-in UI faces; reading reuses a small metrics table without reloading the system font.
 */
#ifdef ESP_PLATFORM
#include "ui_locale_format.h"
#endif
#include "ui_nav.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_registry.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "read_pico_transfer.h"
#include "ui_kit.h"
#include "settings.h"
#include "voice_turn.h"
#include "ui_click_feedback.h"
#if KIIKO_BOARD_METALIO47
#include "metalio_hw.h"
#else
#endif
#include "ttf_font.h"
#include "ui_font.h"

// 底栏图标盒、返回键箭头、充电闪电的边长。
// Box sizes for the bar icons, the back chevron and the charging bolt.
#define UI_NAV_TAB_ICON_PX 44
#define UI_NAV_BACK_ICON_PX 48
#define UI_NAV_BOLT_ICON_PX 26
#define UI_NAV_NETWORK_ICON_PX 28
#define UI_STATUS_TIME_PX 28
#define UI_STATUS_PERCENT_PX 28
#define UI_STATUS_SIGNATURE_PX 26
#define UI_STATUS_CHAPTER_PX 24

static bool s_status_reading;
static struct {
    bool ready;
    char signature[96], face[TTF_FONT_PATH_MAX], chosen[TTF_FONT_PATH_MAX];
    uint16_t widths[96], digits[10], colon, percent, dash;
} s_status_metrics;
_Static_assert(UI_STATUS_TIME_PX == UI_STATUS_PERCENT_PX, "status numbers share one native metrics cache");
_Static_assert(sizeof(s_status_metrics) <= 1024, "status metrics never retain a second font or bitmap");

static int status_safe_width(int px, const char *text, const char *sample) {
    int active = ui_text_fixed_context_width_px(px, text, sample);
    int builtin = ui_font_has_text(text) ? ui_font_text_width_px(px, text) : 0;
    return active > builtin ? active : builtin;
}

static void status_prepare_metrics(void) {
    if (s_status_reading) return;
    const char *signature = app_settings_status_signature();
    const char *face = ttf_font_path(), *chosen = app_settings_system_font_path();
    if (s_status_metrics.ready && !strcmp(signature, s_status_metrics.signature) &&
        !strcmp(face, s_status_metrics.face) && !strcmp(chosen, s_status_metrics.chosen)) return;
    memset(&s_status_metrics, 0, sizeof(s_status_metrics));
    snprintf(s_status_metrics.signature, sizeof(s_status_metrics.signature), "%s", signature);
    snprintf(s_status_metrics.face, sizeof(s_status_metrics.face), "%s", face);
    snprintf(s_status_metrics.chosen, sizeof(s_status_metrics.chosen), "%s", chosen);
    char prefix[96] = {0};
    const size_t length = strlen(s_status_metrics.signature);
    for (size_t n = 1; n <= length; ++n) {
        prefix[n - 1] = signature[n - 1];
        if (n < length && ((unsigned char)signature[n] & 0xc0) == 0x80) continue;
        s_status_metrics.widths[n] = status_safe_width(UI_STATUS_SIGNATURE_PX, prefix, signature);
    }
    for (int digit = 0; digit < 10; ++digit) {
        char text[] = {(char)('0' + digit), 0};
        s_status_metrics.digits[digit] = status_safe_width(UI_STATUS_TIME_PX, text, "0123456789:%-");
    }
    s_status_metrics.colon = status_safe_width(UI_STATUS_TIME_PX, ":", "0123456789:%-");
    s_status_metrics.percent = status_safe_width(UI_STATUS_PERCENT_PX, "%", "0123456789:%-");
    s_status_metrics.dash = status_safe_width(UI_STATUS_PERCENT_PX, "-", "0123456789:%-");
    s_status_metrics.ready = true;
}

void ui_nav_status_font_context(bool reading, bool face_changed) {
    s_status_reading = reading;
    if (!reading && face_changed) s_status_metrics.ready = false;
    status_prepare_metrics();
}

static int status_number_width(const char *text) {
    if (!s_status_metrics.ready) return ui_text_fixed_width_px(UI_STATUS_TIME_PX, text);
    int width = 0;
    for (; *text; ++text) width += *text >= '0' && *text <= '9' ? s_status_metrics.digits[*text - '0']
        : *text == ':' ? s_status_metrics.colon : *text == '%' ? s_status_metrics.percent : s_status_metrics.dash;
    return width;
}

static void status_fit_signature(char *text, int width) {
    bool cached = s_status_metrics.ready && !strcmp(text, s_status_metrics.signature);
    size_t n = strlen(text);
    while (n && (cached ? s_status_metrics.widths[n]
                       : ui_text_fixed_context_width_px(UI_STATUS_SIGNATURE_PX, text, app_settings_status_signature())) > width) {
        --n;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}

// 章节名按系统字体与内建字体中较宽者截断，避免换字体后压到时间。
// Truncate the chapter name by the wider of the system and built-in faces so a face change cannot reach the clock.
static int status_chapter_width(const char *text) {
    int active = ui_text_fixed_context_width_px(UI_STATUS_CHAPTER_PX, text, text);
    int builtin = ui_font_has_text(text) ? ui_font_text_width_px(UI_STATUS_CHAPTER_PX, text) : 0;
    return active > builtin ? active : builtin;
}

static void status_fit_chapter(char *text, int width) {
    size_t n = strlen(text);
    while (n && status_chapter_width(text) > width) {
        --n;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
        if (!n) break;
    }
}

static const char *const labels[] = {"首页", "书架", "音乐", "文件管理", "设置"};
static int s_power_signature=-1000;
// 麦克风图标的屏上状态，绘制时同步，与电量签章同一套变化判定。
// On-screen mic icon state, synced while painting with the same change check as power.
static bool s_status_mic_listening;
static int power_signature(void) {
    const pmu_snapshot_t *pmu=read_pico_pmu_get();
    return (pmu_battery_percent(pmu)+1)*2+(pmu_battery_charging(pmu)?1:0);
}
// 蓝牙图标是否显示也算状态栏内容：开关一翻，下一次 ui_nav_refresh_power 就会把它重画。
// Whether the Bluetooth mark shows is part of the header too, so flipping the switch makes the
// next ui_nav_refresh_power repaint it.
static int s_status_bt_shown = -1;
static int bt_signature(void) { return app_settings_bt_enabled() ? 1 : 0; }

EpdRect ui_nav_status_area(void) { return (EpdRect){0,12,UI_LOCK_WIDTH,56}; }

bool ui_nav_refresh_power(uint8_t *fb) {
    if (!ui_status_visible(fb)) return false;
    if (s_power_signature==power_signature() && s_status_bt_shown==bt_signature()) return false;
    epd_fill_rect(ui_nav_status_area(),UI_GRAY_WHITE,fb);
    ui_nav_status(fb);
    return true;
}
bool ui_nav_power_changed(void) { return s_power_signature != power_signature(); }
bool ui_nav_mic_changed(void) { return s_status_mic_listening != voice_turn_listening(); }

void ui_nav_back(uint8_t *fb, int x, int y) {
    ui_click_feedback_register(fb,(EpdRect){x-10,y+3,64,64},UI_CLICK_BACK_SMALL,UI_ICON_ANIMATE_CIRCLE_CHEVRON_LEFT);
    // 24px原画以48px输出，圆外径约44px，中心与触控区域保持原位。
    // Render the original 24px artwork at 48px for an approximately 44px circle without changing center or hits.
    ui_draw_icon(fb,x+22,y+35,UI_NAV_BACK_ICON_PX,UI_ICON_ANIMATE_CIRCLE_CHEVRON_LEFT,0x30);
}

void ui_nav_wifi_icon(uint8_t *fb, int cx, int cy, int size, uint8_t gray) {
    if (!fb || size < 16 || size > 40) return;
    ui_draw_icon(fb, cx, cy, size, UI_ICON_WIFI, gray);
}

static void icon(uint8_t *fb, int tab, int cx, int y) {
    // 底栏图标固定用 Lucide 细线原图，线宽与状态栏一致；y 是图标盒顶边。
    // The bar icons stay on upstream Lucide line art at the status-bar stroke weight; y is the box top.
    static const ui_icon_t tabs[UI_NAV_TABS] = {
        UI_ICON_HOUSE, UI_ICON_LIBRARY_BIG, UI_ICON_MUSIC,
        UI_ICON_FOLDER_OPEN, UI_ICON_SLIDERS_HORIZONTAL,
    };
    if (tab < 0 || tab >= UI_NAV_TABS) return;
    ui_click_feedback_register(fb, (EpdRect){cx - 30, y - 12, 60, 60}, UI_CLICK_NAV, tabs[tab]);
    ui_draw_icon(fb, cx, y + 18, UI_NAV_TAB_ICON_PX, tabs[tab], UI_GRAY_BLACK);
}

void ui_nav_draw(uint8_t *fb, int active) {
    // 用户2026-11-01：底栏改纯黑白——白底、黑色分隔线、黑色图标文字，不再有浅灰底和灰过渡带。
    // User 2026-11-01: the tab bar is monochrome — white fill, black divider, black icons and labels.
    ui_fill_rect_fast((EpdRect){0, UI_NAV_TOP, UI_LOCK_WIDTH, UI_LOCK_HEIGHT - UI_NAV_TOP}, UI_GRAY_WHITE, fb);
    ui_hairline(fb, UI_NAV_TOP, 0, UI_LOCK_WIDTH, UI_GRAY_BLACK);
    ui_fill_rect_fast((EpdRect){0, UI_NAV_REFRESH_END, UI_LOCK_WIDTH, UI_NAV_TOP - UI_NAV_REFRESH_END}, UI_GRAY_WHITE, fb);
    if (app_settings_main_refresh_mode() != APP_MAIN_REFRESH_NORMAL) {
        // 快刷内容边界预留空白；横条在白底上单独短刷，图标和文字保持固定。
        // Reserve a blank fast-refresh boundary; short-update the marker on white while retaining icons and text.
        ui_fill_rect_fast((EpdRect){0, UI_NAV_MARKER_TOP, UI_LOCK_WIDTH, UI_NAV_MARKER_BOTTOM - UI_NAV_MARKER_TOP}, UI_GRAY_WHITE, fb);
    }
    for (int i = 0; i < UI_NAV_TABS; ++i) {
        int center = UI_NAV_TAB_CENTER(i);
        if (i == active) ui_fill_rect_fast((EpdRect){center - 28, UI_NAV_TOP + 1, 56, 5}, UI_GRAY_BLACK, fb);
        icon(fb, i, center, UI_NAV_TOP + 27);
        ui_text(fb, center, UI_NAV_TOP + 74, 17, labels[i], EPD_DRAW_ALIGN_CENTER, false);
    }
}

static time_t status_time(void) {
    time_t now = time(NULL);
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (now < 1704067200 && pmu && pmu->time_synced && pmu->unix_sec >= 1704067200)
        now = pmu->unix_sec;
    return now;
}
int64_t ui_nav_clock_minute(void) {
    time_t now = status_time();
    return now >= 1704067200 ? (int64_t)now / 60 : -1;
}
static void clock_text(char *clock, size_t capacity) {
    time_t now = status_time();
    snprintf(clock,capacity,"--:--");
    if (now > 1704067200) {
        time_t china = now + 8 * 3600;
        struct tm date;
        if (gmtime_r(&china, &date)) snprintf(clock, capacity, "%02d:%02d", date.tm_hour, date.tm_min);
    }
}
void ui_nav_clock_draw(uint8_t *fb) {
    char clock[12]; clock_text(clock,sizeof(clock));
    ui_text_fixed_vc(fb,36,40,UI_STATUS_TIME_PX,clock,EPD_DRAW_ALIGN_LEFT,false);
}
static void status_draw(uint8_t *fb, const char *left_text, const char *chapter_text) {
    status_prepare_metrics();
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    char clock[12]; clock_text(clock,sizeof(clock));
    const int status_center_y = 40;
    // 固定状态栏字号，让系统字体缩放时文字仍与图标共用中心线。
    // Keep status type fixed so text and glyphs share one centerline at every system font scale.
    // 左栏默认是时间；阅读页传入进度文本，时间随之改排到右侧电量组之前。
    // The left slot holds the clock by default; the reader passes progress and the time moves ahead of the right battery group.
    const char *left = left_text ? left_text : clock;
    ui_text_fixed_vc(fb, 36, status_center_y, UI_STATUS_TIME_PX, left, EPD_DRAW_ALIGN_LEFT, false);
    read_pico_transfer_status_t network = {0};
    read_pico_transfer_get_status(&network);
    const bool wifi_visible = network.network_ready && network.mode == READ_PICO_TRANSFER_MODE_STA;
    // 蓝牙图标只在「蓝牙音频开关真的打开」时出现。以前用的是翻页器 BLE 的开关（默认开），
    // 于是图标永远挂在状态栏上，用户以为蓝牙模组一直开着。
    // The Bluetooth mark appears only while the audio switch is really on. It used to follow the
    // page-turner BLE switch, which defaults to on, so the icon sat in the status bar forever and
    // looked as if the audio module were always powered.
    const bool bluetooth_visible = app_settings_bt_enabled();
    char percent[8] = "--%";
    int gauge_percent = pmu_battery_percent(pmu);
    if (gauge_percent >= 0) snprintf(percent, sizeof(percent), "%u%%", (unsigned)gauge_percent);
    // 按实际百分比宽度排列网络图标，留 10 像素间隔；墨色与电池相同。
    // Place the network marks 10 px before the measured percentage, using the battery ink.
    const int percent_left = 595 - status_number_width(percent);
    const int network_right = percent_left - 10;
    const int wifi_center = network_right - UI_NAV_NETWORK_ICON_PX / 2;
    const int bluetooth_center = wifi_center - (wifi_visible ? UI_NAV_NETWORK_ICON_PX + 8 : 0);
    int signature_width = 280;
    const int left_right = 36 + status_number_width(left);
    int available = 2 * (UI_LOCK_WIDTH / 2 - left_right - 12);
    if (available < signature_width) signature_width = available > 0 ? available : 0;
    int group_left = percent_left;
    if (wifi_visible || bluetooth_visible) {
        group_left = (bluetooth_visible ? bluetooth_center : wifi_center) - UI_NAV_NETWORK_ICON_PX / 2;
    }
    if (left_text) {
        // 阅读页时间靠右：排在网络图标与百分比之前，与右侧组留 12 像素间隔。
        // The reading clock sits at the right, ahead of the marks and percent with a 12 px gap.
        const int clock_right = group_left - 12;
        ui_text_fixed_vc(fb, clock_right, status_center_y, UI_STATUS_TIME_PX, clock, EPD_DRAW_ALIGN_RIGHT, false);
        group_left = clock_right - status_number_width(clock);
        // 时间左侧永远放一枚麦克风状态：监听中是麦克风，未工作/异常是带斜杠的麦克风。
        // Always place a mic state left of the clock: the mic while listening, the struck-through mic while idle or failing.
        const int mic_center = group_left - 10 - UI_NAV_NETWORK_ICON_PX / 2;
        ui_draw_icon(fb, mic_center, status_center_y, UI_NAV_NETWORK_ICON_PX,
                     voice_turn_listening() ? UI_ICON_MIC : UI_ICON_MIC_OFF, UI_GRAY_BLACK);
        group_left = mic_center - UI_NAV_NETWORK_ICON_PX / 2;
    }
    available = 2 * (group_left - 12 - UI_LOCK_WIDTH / 2);
    if (available < signature_width) signature_width = available > 0 ? available : 0;
    char signature[96];
    snprintf(signature, sizeof(signature), "%s", app_settings_status_signature());
    status_fit_signature(signature, signature_width);
    if (signature[0]) ui_text_fixed_vc(fb, UI_LOCK_WIDTH / 2, status_center_y, UI_STATUS_SIGNATURE_PX,
                                      signature, EPD_DRAW_ALIGN_CENTER, false);
    if (chapter_text && chapter_text[0]) {
        // 章节名紧随进度；遇到签名或时间就按剩余宽度截断，固定字号不受系统缩放影响。
        // The chapter name follows the progress; truncate before the signature or clock at a fixed size.
        const int chapter_left = 36 + status_number_width(left) + 14;
        const int chapter_right = signature[0]
            ? UI_LOCK_WIDTH / 2 - status_safe_width(UI_STATUS_SIGNATURE_PX, signature, signature) / 2 - 14
            : group_left - 12;
        const int room = chapter_right - chapter_left;
        if (room >= UI_STATUS_CHAPTER_PX) {
            char chapter[128];
            snprintf(chapter, sizeof(chapter), "%s", chapter_text);
            status_fit_chapter(chapter, room);
            if (chapter[0]) ui_text_fixed_vc(fb, chapter_left, status_center_y, UI_STATUS_CHAPTER_PX,
                                             chapter, EPD_DRAW_ALIGN_LEFT, false);
        }
    }
    if (wifi_visible) ui_nav_wifi_icon(fb, wifi_center, status_center_y, UI_NAV_NETWORK_ICON_PX, UI_GRAY_BLACK);
    if (bluetooth_visible)
        ui_draw_icon(fb, bluetooth_center, status_center_y, UI_NAV_NETWORK_ICON_PX,
                     UI_ICON_BLUETOOTH, UI_GRAY_BLACK);
    EpdRect battery = {606, status_center_y - 11, 40, 22};
    ui_draw_round_rect(fb, battery, 6, UI_GRAY_BLACK);
    ui_fill_round_rect(fb, (EpdRect){648, status_center_y - 4, 5, 8}, 2, UI_GRAY_BLACK);
    bool charging = pmu_battery_charging(pmu);
    if (gauge_percent >= 0) {
        int width = (31 * gauge_percent + 50) / 100;
        if (width > 0) ui_fill_round_rect(fb, (EpdRect){610, status_center_y - 7, width, 14}, 4, UI_GRAY_BLACK);
    }
    if (charging) {
        // 固定黑底与白色闪电描边，0%时也有完整对比，不依赖电量填充宽度。
        // A fixed black well and white bolt outline retain contrast even at0%, independent of the charge fill.
        ui_fill_round_rect(fb,(EpdRect){614,status_center_y-10,26,20},3,UI_GRAY_BLACK);
        ui_draw_icon(fb, 627, status_center_y, 22, UI_ICON_ZAP, UI_GRAY_WHITE);

    }
    ui_text_fixed_vc(fb, 595, status_center_y, UI_STATUS_PERCENT_PX, percent, EPD_DRAW_ALIGN_RIGHT, false);
    ui_status_mark_visible(fb);
    s_power_signature=power_signature();
    s_status_bt_shown = bt_signature();
    s_status_mic_listening = voice_turn_listening();
}
void ui_nav_status(uint8_t *fb) { status_draw(fb, NULL, NULL); }
void ui_nav_status_reading(uint8_t *fb, const char *left_text, const char *chapter_text) {
    status_draw(fb, left_text, chapter_text);
}

int ui_nav_hit(uint16_t x, uint16_t y) {
    if (x >= UI_LOCK_WIDTH || y < UI_NAV_TOUCH_TOP || y >= UI_LOCK_HEIGHT) return -1;
    int tab = UI_NAV_TAB_AT(x);
    return tab < UI_NAV_TABS ? tab : -1;
}

void ui_nav_request(app_ctx_t *ctx, int index) {
    if (index >= 0 && index < UI_NAV_TABS) ctx->request_app = app_at(index);
}
