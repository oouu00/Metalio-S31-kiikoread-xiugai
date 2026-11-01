/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：亮度滑轨用指数曲线——每往下一像素，亮度 ×0.975（低亮度区手感更细）。
 *      满值取用户在"设置→显示与刷新→前光→亮度上限"里设的天花板；滑块仍是 0..range 像素，
 *      按 max×0.975^(range−p) 递减。仅做界面坐标与亮度值的换算，不改底层 PWM 与保存值。
 * English: Brightness follows an exponential track: each pixel down multiplies by 0.975. The
 *      ceiling is the user-set "brightness max"; the track stays 0..range px and decays as
 *      max×0.975^(range−p). Only converts coordinates to light levels; stored PWM units are unchanged.
 *
 *   value(p) = max * 0.975^(RANGE - p)        p = 0 底部 / bottom, RANGE 顶部 / top
 *   例 / e.g.（max=240）：p=240 → 240(满)；p=120 → 约 12；底部约 24px 因整数化归 0（即关灯）。
 */
#pragma once
#include <math.h>
#include "settings.h"

#ifndef UI_QUICK_BRIGHTNESS_STEP
#define UI_QUICK_BRIGHTNESS_STEP 0.975f
#endif

// 灭灯阈值：对应 10-bit PWM 最小非零 duty=1 的亮度位置（= 2*RANGE/1023 ≈ 0.47）。
// Off threshold: the light level at the 10-bit minimum non-zero duty=1 (= 2*RANGE/1023 ≈ 0.47).
#ifndef UI_QUICK_BRIGHT_MIN
#define UI_QUICK_BRIGHT_MIN (2.0f * UI_QUICK_SLIDER_RANGE / 1023.0f)
#endif

static inline float ui_quick_brightness_value(unsigned position) {
    const unsigned range = UI_QUICK_SLIDER_RANGE;
    if (position > range) position = range;
    const unsigned top = app_settings_brightness_max();
    // 未设上限时退化为满量程（240）。
    // With no ceiling configured, fall back to the full range.
    if (top == 0) return 0.0f;
    const float v = (float)top * powf(UI_QUICK_BRIGHTNESS_STEP, (float)(range - position));
    // 低于硬件最小非零亮度（duty=1 对应值）视为 0：底部一小段是"熄灯"。
    // Below the hardware minimum non-zero level (duty=1) collapses to 0 -> "off".
    if (v < UI_QUICK_BRIGHT_MIN) return 0.0f;
    return v;
}

static inline unsigned ui_quick_brightness_position(unsigned value) {
    const unsigned range = UI_QUICK_SLIDER_RANGE;
    if (value > range) value = range;
    const unsigned top = app_settings_brightness_max();
    if (top == 0) return 0;
    if (value == 0) return 0;
    // value = top * STEP^(RANGE - p)  =>  p = RANGE - ln(value/top)/ln(STEP)
    const float p = (float)range - logf((float)value / (float)top) / logf(UI_QUICK_BRIGHTNESS_STEP);
    if (!(p > 0.0f)) return 0;
    const unsigned out = (unsigned)(p + 0.5f);
    return out > range ? range : out;
}

// 相对上限的百分比：滑块顶部即 100%（=上限），底部约 0%。
// Percent relative to the ceiling: the top of the track is 100% (= max), the bottom ~0%.
static inline unsigned ui_quick_brightness_percent(unsigned position) {
    const unsigned top = app_settings_brightness_max();
    if (top == 0) return 0;
    const unsigned v = ui_quick_brightness_value(position);
    return (v * 100 + top / 2) / top;
}
