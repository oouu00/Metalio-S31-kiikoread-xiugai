/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：书架列表在 TF 卡上的快照，进书架先显示快照、后台再校验。
 * English: TF-card snapshot of the shelf listing so the first frame appears before a rescan.
 *
 * 冻结：快照只存"书架看到的字段"，从不存阅读正文；写入走临时文件 + rename。
 * Frozen: the snapshot stores only the fields the shelf paints, never book text; writes are
 * temp-file plus rename. Records are passed as opaque blobs, so the caller's entry struct stays
 * the single definition; a record-size mismatch simply invalidates the snapshot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define BOOK_SHELF_CACHE_VERSION 1u

/// 只读文件头返回条目数，供调用方按需预留缓冲；无快照时返回 false。
/// Read only the header for the record count so the caller can size its buffer; false when there
/// is no usable snapshot.
bool book_shelf_cache_peek(size_t record_size, size_t *count);
/// 读取快照到调用方缓冲；根目录、记录尺寸、校验和任一不符即视为无快照。
/// Load the snapshot into the caller's buffer; a different root, record size or checksum means
/// there is no usable snapshot.
bool book_shelf_cache_read(void *records, size_t record_size, size_t capacity,
                           size_t *count, const char *root);
/// 原子写入快照；目录不可写或空间不足时静默失败。
/// Write the snapshot atomically; unwritable media or a full card fails silently.
bool book_shelf_cache_write(const void *records, size_t record_size, size_t count,
                            const char *root, unsigned revision);
/// 删除快照；换书目录或快照失效时调用。/ Drop the snapshot when the root changes or it goes stale.
void book_shelf_cache_drop(void);
