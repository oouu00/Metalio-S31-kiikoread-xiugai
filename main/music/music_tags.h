/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：音乐元数据解析。标题/艺人（MP3 ID3v2.3/2.4、FLAC Vorbis 注释），时长
 * （MP3 Xing/比特率估算、FLAC STREAMINFO、WAV fmt/data、M4A mvhd）。失败的字段
 * 由调用方回退到文件名或 0。
 *
 * English: music metadata parsing — title/artist (MP3 ID3v2.3/2.4, FLAC Vorbis
 * comments) and duration (MP3 Xing/bitrate estimate, FLAC STREAMINFO, WAV
 * fmt/data, M4A mvhd). Callers fall back to the file name or 0 on failure.
 *
 * 冻结：只读文件头部，不改文件、不做播放决策。
 * Frozen: reads file headers only; no writes, no playback policy.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 单次打开文件解析全部元数据；title/artist 失败时回退文件名（去扩展名、去前导
// 序号如 "01."），duration 解析失败写 0（未知）。
// One open: parse everything. On failure title/artist fall back to the file name
// (extension and a leading "01." dropped); duration_ms is 0 when unknown.
void music_tags_read(const char *path, char *title, size_t title_cap, char *artist,
                     size_t artist_cap, uint32_t *duration_ms);

/// 提取内嵌歌词（MP3 ID3 USLT、FLAC Vorbis LYRICS、M4A ©lyr）；无则返回 0、out 为空。
/// Extract embedded lyrics (MP3 ID3 USLT, FLAC Vorbis LYRICS, M4A ©lyr); 0 and an empty
/// out when the file carries none. Plain text, may contain line breaks.
size_t music_tags_lyrics(const char *path, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
