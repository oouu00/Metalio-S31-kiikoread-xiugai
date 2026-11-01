/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 产品页清单。五个主页面对应底部导航（首页/书架/音乐/文件管理/设置）；传输由文件首页的直达按钮进入，工厂页独立。
 *
 * Product page table. Five main pages match the tab bar (home/shelf/music/files/settings);
 * file-home buttons open transfer directly.
 */

#include "app_registry.h"

extern const app_desc_t app_book;
extern const app_desc_t app_image;
extern const app_desc_t app_dashboard;
extern const app_desc_t app_files;
extern const app_desc_t app_device_settings;
extern const app_desc_t app_music;

// 正式产品页面清单；工厂测试与历史示例不进入用户导航。
// 前五个顺序即底栏顺序（见 ui_nav_layout.h 的 UI_NAV_TABS）；图片查看器不在导航里。
// Production page registry; factory diagnostics and legacy examples are not user-navigable.
// The first five follow the tab order (see UI_NAV_TABS); the image viewer is not a tab.
static const app_desc_t* const s_apps[] = {
    &app_dashboard,
    &app_book,
    &app_music,
    &app_files,
    &app_device_settings,
    &app_image,
};

#define APP_COUNT ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

int app_count(void) {
    return APP_COUNT;
}

const app_desc_t* app_at(int index) {
    if (index < 0 || index >= APP_COUNT) return NULL;
    return s_apps[index];
}

int app_index_of(const app_desc_t* app) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (s_apps[i] == app) return i;
    }
    return -1;
}

const app_desc_t* app_home_page(void) {
    return &app_dashboard;
}
