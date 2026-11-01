/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：当前曲目的歌词。同目录同名 .lrc 优先，其次文件内嵌歌词（ID3 USLT /
 * Vorbis LYRICS / M4A ©lyr）；按播放位置返回当前行。加载在解码任务侧，查询在
 * UI 侧，缓冲一旦分配就不再释放，指针跨曲目保持稳定。
 *
 * English: lyrics for the current track. A same-directory .lrc wins over embedded
 * text (ID3 USLT / Vorbis LYRICS / M4A ©lyr); returns the line for a playback
 * position. Loads run on the decode task, queries on the UI side; the buffer is
 * allocated once and never freed, so pointers stay valid across tracks.
 *
 * 冻结：只读歌词来源，不写文件、不参与播放决策。
 * Frozen: reads lyric sources only; no writes, no playback policy.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 载入曲目歌词；duration_ms 用于无时间戳纯文本按行均分。可重复调用。
/// Load a track's lyrics; duration_ms spreads untimed plain text evenly across lines.
/// Safe to call again for the next track.
void music_lyrics_load(const char *track_path, uint32_t duration_ms);

/// 清空当前歌词（换曲/停止时调用）。/ Drop the current lyrics (on track change or stop).
void music_lyrics_clear(void);

/// 播放位置对应的当前歌词行；无歌词或位置早于首行返回 ""。/ The line at a playback
/// position; "" when there are no lyrics or the position precedes the first line.
const char *music_lyrics_at(uint32_t ms);

/// 当前句的上一句；开头之前或无歌词返回 ""。/ The line before the current one; "" before
/// the first line or without lyrics.
const char *music_lyrics_prev(uint32_t ms);

/// 当前句的下一句；末句之后或无歌词返回 ""。/ The line after the current one; "" past the
/// last line or without lyrics.
const char *music_lyrics_next(uint32_t ms);

#ifdef __cplusplus
}
#endif
