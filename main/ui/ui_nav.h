/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：五个产品主页面共用的底部导航；阅读页自行隐藏。
 * English: Shared five-tab product navigation; the reader hides it.
 */
#pragma once

#include "app.h"
#include "ui_nav_layout.h"

#define UI_NAV_BACK_BORDER_GRAY 0x58

void ui_nav_draw(uint8_t *fb, int active);
void ui_nav_status(uint8_t *fb);
/// 阅读页状态栏：左栏换成阅读进度文本，时间排到右侧电量组之前；其余页面用 ui_nav_status。
/// 章节名放在进度右侧，空间不足时按进度右侧到签名/时间之间的宽度截断。
/// / Reader status bar: the left slot shows reading progress and the time joins the right battery group; other pages use ui_nav_status.
/// The chapter name follows the progress and is truncated to the space before the signature or clock.
void ui_nav_status_reading(uint8_t *fb, const char *left_text, const char *chapter_text);
/// 电量/充电自上次状态栏绘制后是否变化；阅读页据此自行重画，避免通用重画盖掉进度。
/// Whether battery/charging changed since the last status paint; the reader repaints itself so the generic pass cannot cover progress.
bool ui_nav_power_changed(void);
/// 麦克风监听自上次状态栏绘制后是否变化；阅读页据此重画，让时间左侧的麦克风图标及时亮灭。
/// Whether mic listening changed since the last status paint; the reader repaints so the mic mark left of the clock stays current.
bool ui_nav_mic_changed(void);
EpdRect ui_nav_status_area(void);
/// 电量/充电变化且状态栏可见时重画；调用方只差分推状态栏区域。
/// Paint changed power state only when visible; callers present only the status-bar diff.
bool ui_nav_refresh_power(uint8_t *fb);
/// 系统字体就绪时缓存状态栏字宽，阅读页只复用，不重载整份系统字体。
/// Cache status metrics when the system face is ready; reading reuses them without reloading that font.
void ui_nav_status_font_context(bool reading, bool face_changed);
/// 当前 UTC 分钟，用于显示更新判断；不读取卡或请求网络校时。
/// Current UTC minute for display updates, without card IO or network time requests.
int64_t ui_nav_clock_minute(void);
/// 只绘制状态栏时间，供主页面按分钟局部更新。/ Draw only the status clock for local minute updates.
void ui_nav_clock_draw(uint8_t *fb);
/// 绘制设置页与状态栏共用的 WiFi 标志。/ Draw the WiFi mark shared by Settings and the status bar.
void ui_nav_wifi_icon(uint8_t *fb, int cx, int cy, int size, uint8_t gray);
/// 二级页面共用的圆形返回按钮。/ Shared circular back control for secondary pages.
void ui_nav_back(uint8_t *fb, int x, int y);
/// 每栏整列热区向上扩16px，互不重叠；不改变画面和刷新区域。/ Nonoverlapping full-column hits per tab extend 16px upward, without changing paint or refresh bounds.
int ui_nav_hit(uint16_t x, uint16_t y);
void ui_nav_request(app_ctx_t *ctx, int index);
