/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 冷启动擦白并发化：整屏物理擦白放到另一个核跑，主任务同时做触摸识别、字体加载
 * 和首帧绘制，第一次真正扫屏之前再汇合。
 * Cold-boot erase offload: the full-panel whitening runs on the other core while the
 * main task identifies the touch chip, loads the face and paints the first frame;
 * the two sides meet right before the first panel scan.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动后台擦白任务。返回 false 表示无法并发（信号量/任务创建失败或已启动过），
// 调用方应退回同步擦白。重复调用只会在第一次真正创建任务。
// Start the background erase. Returns false when concurrency is unavailable (semaphore or
// task creation failed, or it was already started); the caller must then erase synchronously.
bool boot_erase_start(void);

// 是否还有擦白在跑。用于标记/统计，不影响正确性。
// Whether a background erase is still running; diagnostics only.
bool boot_erase_busy(void);

// 等到擦白完成。可重复调用；没有启动过就立刻返回。第一次真正开工扫描前必须调用。
// Wait for the erase to finish. Repeat calls are free, and it returns immediately when
// nothing was started. Must be called before the first real panel scan.
void boot_erase_join(void);

#ifdef __cplusplus
}
#endif
