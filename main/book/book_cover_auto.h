/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：无封面书籍的确定性灰阶模板。只绘制目标尺寸，不执行屏幕抖动。
 * English: Deterministic grayscale covers for coverless books. Render at target size, without display dithering.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 缓存键的一部分：改动封面生成或封面的解码能力后必须递增，否则旧结果一直命中。
// Part of the cover cache key: bump it whenever cover rendering or the set of decodable cover
// formats changes, or stale entries keep winning.
#define BOOK_AUTO_COVER_VERSION 4u

/// 用稳定标识、书名和作者选择模板。/ Select a template from stable identity, title and author.
unsigned book_auto_cover_template(const char *identity, const char *title, const char *author);

/// 生成 8 位灰阶封面；调用方提供 width*height 字节。/ Generate an 8-bit gray cover into caller-owned width*height bytes.
bool book_auto_cover_render(const char *identity, const char *title, const char *author,
                            unsigned width, unsigned height, uint8_t *out);

/// 在第一个「作者」或第一个空白片段前插入换行，使书名与作者各自成行。
/// Split a possibly packed "title + author" string at the first "作者" or first
/// whitespace run so the book name and author land on separate lines.
void book_cover_title_split(const char *title, char *out, size_t cap);
