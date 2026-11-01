/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：没有串口时的蓝牙现场。把 bt_host 的 AT 收发、模式切换、链路状态、喂帧统计，以及
 * 播放器与睡眠的关键节点排队写进 TF 卡上的独立文件，用户拔卡就能把现场带出来。
 * 生产者只往环形缓冲里塞一行（不阻塞、不碰卡），真正的写卡在低优先级任务里做。
 *
 * English: the Bluetooth crime scene without a console. AT traffic, mode switches, link state,
 * feed stats plus the player/sleep milestones are queued into a dedicated file on the TF card,
 * so the card itself carries the evidence. Producers only push a line into a ring buffer (no
 * blocking, no card access); a low-priority task does the writing.
 *
 * 关闭方式：-DMETALIO_BT_TRACE=OFF（交付固件建议关掉，别把写卡调试代码带上）。
 * Disable with -DMETALIO_BT_TRACE=OFF; delivery images should not carry the debug writer.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 建缓冲与写卡任务，并把自己挂成 bt_host 的埋点接收方；可重复调用。
/// Builds the buffer and the writer task and registers as bt_host's trace sink; idempotent.
void bt_trace_start(void);

#if defined(METALIO_BT_TRACE) && METALIO_BT_TRACE

/// 写一行（自动加时间戳）；任何任务上下文都能调，绝不阻塞。
/// Append one line (timestamped); callable from any task context, never blocks.
void bt_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// 已经格式化好的一行，给 bt_host 的埋点回调直接用。
/// An already formatted line, for bt_host's trace callback.
void bt_trace_line(const char *line);

/// 立刻把缓冲里的内容写出去（诊断点用，比如挂起前）。
/// Flush what is buffered right away, for diagnostic points such as "just before suspend".
void bt_trace_flush(void);

#else

// 关闭诊断时编成空语句，而不是留一个空函数：只有这样编译器才会把实参连同字符串
// 字面量一起消掉，正式版固件里不留半句日志文本（-Os/-O2 都不能删除已求值的实参）。
// With tracing disabled these become no-op macros rather than empty function calls, so the
// compiler drops the arguments — including the string literals — from the shipping image.
#define bt_trace(...)        ((void)0)
#define bt_trace_line(line)  ((void)(line))
#define bt_trace_flush()     ((void)0)

#endif

#ifdef __cplusplus
}
#endif
