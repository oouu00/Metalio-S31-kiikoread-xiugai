/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：主页浅灰底与白色模块的轻阴影；仅绘制，不推屏或分配缓存。
 * English: Light-gray root canvas and white module shadows; paint only, without caches or presentation.
 */
#pragma once
#include "ui_kit.h"
#define UI_ROOT_GRAY 0xf0
static inline void ui_root_background(uint8_t *fb) {
    ui_fill_rect_fast((EpdRect){0,0,684,1096},UI_ROOT_GRAY,fb);
}
static inline void ui_root_shadow(uint8_t *fb,EpdRect r,int radius) {
    EpdRect shadow=r;shadow.x+=2;shadow.y+=6;
    ui_fill_round_rect(fb,shadow,radius,0xe0);
    shadow.x-=1;shadow.y-=3;ui_fill_round_rect(fb,shadow,radius,0xd8);
}
static inline void ui_root_card(uint8_t *fb,EpdRect r,int radius) {
    ui_root_shadow(fb,r,radius);ui_fill_round_rect(fb,r,radius,UI_GRAY_WHITE);
}
