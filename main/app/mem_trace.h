/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：定时内存水位采样。每隔几秒把内部 SRAM 与外部 PSRAM 的空闲 / 最大连续块 / 历史最低
 * 追加到 TF 卡的 CSV，用来判断长时间阅读是否在漏内存、哪一段把内部 SRAM 啃薄。
 * 只在 METALIO_MEM_TRACE 构建里参与编译，交付构建为空目标。
 *
 * English: Periodic memory-watermark sampling. Every few seconds the free / largest / minimum
 * sizes of internal SRAM and external PSRAM are appended to a TF-card CSV, so a slow leak or the
 * phase that thins internal SRAM is visible without a console. Compiles only in METALIO_MEM_TRACE
 * builds and is an empty object in delivery builds.
 */

#pragma once

#ifdef METALIO_MEM_TRACE

/// 启动定时采样任务。重复调用安全；SD 未就绪时样本先攒在 PSRAM，挂载后补写。
/// / Start the periodic sampler. Repeat calls are safe; samples wait in PSRAM until the card mounts.
void mem_trace_start(void);

#else

/// 关闭构建里是空操作，调用点无需条件编译。
/// / A no-op in builds without the trace, so call sites need no conditional compilation.
static inline void mem_trace_start(void) {}

#endif
