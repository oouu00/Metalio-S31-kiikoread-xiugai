/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 冷启动擦白（约 946ms）期间 CPU 基本空转：触摸识别和系统字体加载都是纯 CPU/IO 活，
 * 不碰面板。把它们放到 Core1 上跑，主任务专心擦白，擦白返回后立刻汇合。
 *
 * 与上一版「把擦白挪到 Core1」的区别（那次导致开机无限闪烁）：
 * 面板动作始终留在主任务，副任务一行 epd_* 都不调用。差分被 epdiy 派给 Core1 的
 * epd_prep 任务、行供数共用同一套通知，跨核同时推屏会撞死它；这里不碰这条路径。
 *
 * Cold boot whitens the panel for ~946ms while the CPU idles: touch identification and
 * system face loading are pure CPU/IO work that never touches the panel, so they run on
 * Core1 while the main task whitens, and both sides meet right after the erase.
 *
 * Difference from the previous attempt that made boot flicker forever: the panel work
 * stays on the main task and the helper task calls no epd_* API at all. epdiy dispatches
 * the diff to Core1's epd_prep task and shares its notification with line feeding, so
 * driving the panel from two cores at once wedges it; this path never goes there.
 */

#pragma once

#include <stdbool.h>

#include "read_pico_init.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动后台准备任务（触摸识别 + 字体）。返回 false 表示无法并发，调用方应原地顺序执行。
// want_system_font=false 时加载内建字体，与原来的续读分支一致。
// Start the background preparation task (touch identification + face). Returns false when
// concurrency is unavailable and the caller must run the steps inline instead.
// want_system_font=false loads the built-in face, matching the resume branch.
bool boot_prep_start(read_pico_handle_t *hw, bool want_system_font);

// 等到准备任务结束。没启动过就立刻返回；擦白返回后必须调用。
// Wait for the preparation task. Returns immediately when nothing was started; must be
// called right after the whitening returns.
void boot_prep_join(void);

// 准备任务是否已把字体装上；未并发时恒为 false，调用方按原顺序自己加载。
// Whether the background task already installed a face; always false without the
// offload, in which case the caller loads it in the original order.
bool boot_prep_font_ready(void);

#ifdef __cplusplus
}
#endif
