/* SPDX-License-Identifier: Apache-2.0
 * 中文：轻量底栏边界，绘制和刷新共用，防止局部扫描碰到固定图标。
 * English: Lightweight navigation bounds shared by painting and presentation to protect fixed icons.
 */
#pragma once
// 底栏分栏数：首页/书架/音乐/文件管理/设置，与 app_registry 前五项同序。
// 绘制、命中、切页和点击反馈都读这一个数，改栏数只改这里。
// Tab count: home/shelf/music/files/settings, matching the first registry entries.
// Painting, hit-testing, paging and tap feedback all read this single number.
#define UI_NAV_TABS 5
// 每栏宽度与中心（列中心取整，和旧的四栏等分公式同一套算法）。
// Per-tab width and centre, sharing one rounding rule with the old four-tab split.
#define UI_NAV_TAB_WIDTH (UI_LOCK_WIDTH / UI_NAV_TABS)
#define UI_NAV_TAB_CENTER(i) (((i) * 2 + 1) * UI_LOCK_WIDTH / (UI_NAV_TABS * 2))
// 触摸/图标归属的列号；越界返回 -1。/ Column owning a coordinate; -1 when outside.
#define UI_NAV_TAB_AT(x) ((x) * UI_NAV_TABS / UI_LOCK_WIDTH)
#define UI_NAV_TOP 1096
// 只扩大触控热区，不移动图标或改变内容/底栏的刷新边界。
// Expand touch admission only; icons and content/navigation refresh boundaries stay fixed.
#define UI_NAV_TOUCH_TOP (UI_NAV_TOP - 16)
#define UI_NAV_MARKER_TOP (UI_NAV_TOP + 1)
#define UI_NAV_MARKER_BOTTOM (UI_NAV_TOP + 6)
// 内容向内收至32列边界；横条单独短刷或并入快刷，外扩范围均止于图标首笔之前。
// Round content inward to 32 columns; marker-only or fused fast scans end before any icon ink.
#define UI_NAV_REFRESH_END (UI_NAV_TOP & ~31)
#define UI_NAV_MARKER_SCAN_END ((UI_NAV_MARKER_BOTTOM + 31) & ~31)
