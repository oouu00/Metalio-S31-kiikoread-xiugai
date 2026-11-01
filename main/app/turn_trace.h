/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：翻页分步计时。一次翻页拆成"章节加载与分页 / 正文绘制 / 页外围 / render / 推屏 /
 * 收尾"六段，先攒在 PSRAM 环形缓冲里，再由低优先级任务写进 TF 卡的 CSV。用来判断翻页
 * 的耗时到底在 CPU 绘制还是屏幕波形上，决定优化做在哪一层。
 *
 * 写卡必须异步：翻页期间一次 fopen/fwrite 要几毫秒，抢在推屏中间会让行队列欠载从而触发
 * 清屏恢复。所以这里只在"没有翻页正在进行"时落盘，并且任务优先级低于所有绘制与推屏任务。
 *
 * English: Per-stage page-turn timing. A turn is split into chapter load+pagination / body draw /
 * outer chrome / render / panel update / teardown, buffered in a PSRAM ring and written to a
 * TF-card CSV by a low-priority task. It tells us whether a turn is CPU-draw bound or panel
 * bound, i.e. where the next optimisation belongs.
 *
 * Flushing must be asynchronous: a fopen/fwrite during a turn costs milliseconds, and landing
 * inside the scan starves the line queue into a full clear-and-recover. Records are therefore
 * only written while no turn is in flight, from a task below every drawing and update task.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// 计时阶段。枚举无条件定义：调用点在关闭计时的构建里也要能编译。
/// / Stages. Defined unconditionally so call sites still compile when tracing is off.
typedef enum {
    TT_TURN = 0,  ///< 章节加载、解码与分页；页内翻页时接近 0 / chapter load, decode, pagination
    TT_BODY,      ///< 正文绘制，或命中预渲染时的整帧复制 / body draw, or the prefetch memcpy
    TT_OUTER,     ///< 页眉页脚、状态栏、入页缓存 / header, footer, status bar, page-cache store
    TT_RENDER,    ///< present 内的 render() / render() inside present
    TT_DISPLAY,   ///< 推屏本身（update_display_*）/ the panel update itself
    TT_JOIN,      ///< present 收尾（派发预渲染之后）/ present teardown after the prefetch kick
    TT_STAGES
} tt_stage_t;

#ifdef METALIO_TURN_TRACE

/// 开始一次翻页计时。重复调用会丢弃上一条未结束的记录。
/// / Start timing one turn. A repeat call drops the previous unfinished record.
void turn_trace_begin(int dir, unsigned chapter, size_t page, size_t pages);
/// 把"上一个标记到现在"的耗时累加进 stage，并把计时原点推到当前。未激活时为空操作。
/// / Add the elapsed time since the previous mark into stage and move the origin. No-op when idle.
void turn_trace_mark(tt_stage_t stage);
/// 记录本页是否命中预渲染/页缓存。/ Record whether this page hit the prefetch/page cache.
void turn_trace_set_cached(bool cached);
/// 绘制前后成对调用：采集字形缓存命中与光栅化耗时（会略微拖慢绘制）。
/// / Call in pairs around drawing: collects glyph cache hits and raster time (slightly slows drawing).
void turn_trace_glyph_begin(void);
void turn_trace_glyph_end(void);
/// 记录本次推屏走的是哪条出口与区域。/ Record which present route and area this update used.
void turn_trace_set_route(bool text_turn, bool water_turn, bool cleanup, int mode,
                          int area_y, int area_h);
/// 记录连点合并：这一行代表 jumps 次连点、实际前进 moved 页。
/// / Record batched taps: this row covers jumps taps that advanced moved pages.
void turn_trace_set_batch(int jumps, int moved);
/// 记录夜间极性；翻转后面板必须走整页，耗时不可与日间直接比较。
/// / Record night polarity; flipped panels must take the full-page route and are not comparable with daytime rows.
void turn_trace_set_night(bool night);
/// 记录状态栏里的章节内页码是否并入了本次差分。/ Record whether the status-strip page counter joined this differential.
void turn_trace_set_status(bool fused);
/// 记录真正推屏的区域，即状态栏与页脚并入之后的合成结果。
/// / Record the actually updated area, i.e. the union after any status-strip and footer fusion.
void turn_trace_set_update_area(int x, int y, int width, int height);
/// 滑块拖动一帧一行，throttled 表示这一帧被 150ms 限频丢掉、根本没有推屏。
/// / One row per slider drag frame; throttled marks a frame dropped by the 150 ms throttle without any update.
void turn_trace_set_slider(bool throttled, int mode, int x, int y, int width, int height);
/// 记录 present 真正走进的那个出口分支。名字进 CSV 的 where 列，用来解释为什么离开了局部差分；
/// 这是唯一能证明出口的依据，route 只是调用方的预期，不是实际结果。
/// / Record the outlet present actually entered. Its name lands in the CSV `where` column and explains why the
/// differential was left; route is only the caller's expectation, where is what really happened.
void turn_trace_set_where(const char* where);
/// 记录 present 收到的 redraw 与当时的 chrome 状态，便于区分"转整页"是被谁触发的。
/// / Record the redraw present received plus the chrome state then, showing what forced a full page.
void turn_trace_set_outcome(int redraw, bool fullscreen, bool toolbar);
/// 记录翻页开始前的 chrome 形态（bit0 非全屏 / bit1 开着面板）。reader_turn_chrome 会把它抹平，
/// 所以必须在这之前调；这正是"为什么升级成整页出口"的证据。
/// / Snapshot the chrome before the turn (bit0 not full screen / bit1 panel open). reader_turn_chrome flattens it,
/// so this runs first; it is the evidence for why a turn became a full page.
void turn_trace_set_chrome_before(bool not_fullscreen, bool toolbar_open);
/// 结束并压入环形缓冲；返回是否真的落了一条记录。
/// / Finish and push into the ring; returns whether a record was actually queued.
bool turn_trace_end(void);
/// 放弃当前记录（翻页被中断、切页等）。/ Drop the current record (turn aborted, view changed).
void turn_trace_cancel(void);
/// 立刻把缓冲写卡；force 忽略"还有多少条"的阈值。/ Write the buffer now; force ignores the threshold.
void turn_trace_flush(bool force);

#else

static inline void turn_trace_begin(int dir, unsigned chapter, size_t page, size_t pages) {
    (void)dir; (void)chapter; (void)page; (void)pages;
}
static inline void turn_trace_mark(tt_stage_t stage) { (void)stage; }
static inline void turn_trace_set_cached(bool cached) { (void)cached; }
static inline void turn_trace_glyph_begin(void) {}
static inline void turn_trace_glyph_end(void) {}
static inline void turn_trace_set_route(bool a, bool b, bool c, int m, int y, int h) {
    (void)a; (void)b; (void)c; (void)m; (void)y; (void)h;
}
static inline void turn_trace_set_batch(int jumps, int moved) { (void)jumps; (void)moved; }
static inline void turn_trace_set_night(bool night) { (void)night; }
static inline void turn_trace_set_status(bool fused) { (void)fused; }
static inline void turn_trace_set_update_area(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
static inline void turn_trace_set_slider(bool throttled, int m, int x, int y, int w, int h) {
    (void)throttled; (void)m; (void)x; (void)y; (void)w; (void)h;
}
static inline void turn_trace_set_where(const char* where) { (void)where; }
static inline void turn_trace_set_outcome(int r, bool f, bool t) { (void)r; (void)f; (void)t; }
static inline void turn_trace_set_chrome_before(bool n, bool t) { (void)n; (void)t; }
static inline bool turn_trace_end(void) { return false; }
static inline void turn_trace_cancel(void) {}
static inline void turn_trace_flush(bool force) { (void)force; }

#endif
