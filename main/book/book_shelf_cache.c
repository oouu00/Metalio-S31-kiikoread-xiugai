/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：书架快照的原子文件读写，只依赖标准 C。
 * English: Atomic file IO for the shelf snapshot, standard C only.
 */
#include "book_shelf_cache.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SHELF_CACHE_MAGIC UINT32_C(0x53484631) /* "SHF1" */
#define SHELF_CACHE_DIR "/sdcard/.readpico"
#define SHELF_CACHE_PATH SHELF_CACHE_DIR "/shelf.bin"
#define SHELF_CACHE_TEMP SHELF_CACHE_PATH ".tmp"
#define SHELF_CACHE_COUNT_LIMIT 8192u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t record_size;
    uint32_t count;
    uint32_t store_revision;
    uint32_t checksum;
    uint64_t root_hash;
} shelf_cache_header_t;

static uint32_t hash32(uint32_t seed, const void *data, size_t size) {
    const unsigned char *p = data;
    while (size--) seed = (seed ^ *p++) * UINT32_C(16777619);
    return seed;
}

static uint64_t hash_root(const char *root) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)root; p && *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return hash;
}

static bool finite_string(const char *text, size_t cap) {
    return memchr(text, 0, cap) != NULL;
}

// 逐条检查记录：路径必须是绝对路径，三个字符串必须在界内，尺寸/进度在合理区间。
// Validate every record: an absolute path, all three strings terminated inside their bounds and
// plausible size/progress values.
static bool record_plausible(const unsigned char *record, size_t record_size) {
    // 记录布局前段固定：name[256] / author[128] / path[288]，后面才是数值字段。
    // The leading layout is fixed: name[256], author[128], path[288], then the numeric fields.
    if (record_size < 256 + 128 + 4) return false;
    const char *name = (const char *)record;
    const char *author = name + 256;
    const char *path = author + 128;
    if (!finite_string(name, 256) || !finite_string(author, 128)) return false;
    if (record_size < 256 + 128 + 288) return false;
    if (!finite_string(path, 288) || path[0] != '/') return false;
    return true;
}

bool book_shelf_cache_peek(size_t record_size, size_t *count) {
    if (!record_size || !count) return false;
    *count = 0;
    FILE *file = fopen(SHELF_CACHE_PATH, "rb");
    if (!file) return false;
    shelf_cache_header_t header;
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
        header.magic == SHELF_CACHE_MAGIC && header.version == BOOK_SHELF_CACHE_VERSION &&
        header.record_size == record_size && header.count > 0 &&
        header.count <= SHELF_CACHE_COUNT_LIMIT;
    fclose(file);
    if (!ok) return false;
    *count = header.count;
    return true;
}

bool book_shelf_cache_read(void *records, size_t record_size, size_t capacity,
                           size_t *count, const char *root) {
    if (!records || !record_size || !count || !root) return false;
    *count = 0;
    FILE *file = fopen(SHELF_CACHE_PATH, "rb");
    if (!file) return false;
    shelf_cache_header_t header;
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
        header.magic == SHELF_CACHE_MAGIC && header.version == BOOK_SHELF_CACHE_VERSION &&
        header.record_size == record_size && header.count > 0 &&
        header.count <= SHELF_CACHE_COUNT_LIMIT && header.count <= capacity &&
        header.root_hash == hash_root(root);
    if (!ok) { fclose(file); return false; }
    size_t bytes = (size_t)header.count * record_size;
    ok = fread(records, 1, bytes, file) == bytes && fgetc(file) == EOF;
    if (ok) {
        unsigned char *base = records;
        for (uint32_t i = 0; ok && i < header.count; ++i)
            ok = record_plausible(base + (size_t)i * record_size, record_size);
    }
    if (ok) {
        uint32_t expected = header.checksum;
        header.checksum = 0;
        uint32_t digest = hash32(UINT32_C(2166136261), &header, sizeof(header));
        digest = hash32(digest, records, bytes);
        ok = expected == digest;
    }
    fclose(file);
    if (!ok) return false;
    *count = header.count;
    return true;
}

bool book_shelf_cache_write(const void *records, size_t record_size, size_t count,
                            const char *root, unsigned revision) {
    if (!records || !record_size || !count || count > SHELF_CACHE_COUNT_LIMIT || !root) return false;
    if (mkdir(SHELF_CACHE_DIR, 0777) && errno != EEXIST) return false;
    shelf_cache_header_t header = {
        .magic = SHELF_CACHE_MAGIC,
        .version = BOOK_SHELF_CACHE_VERSION,
        .record_size = (uint32_t)record_size,
        .count = (uint32_t)count,
        .store_revision = revision,
        .checksum = 0,
        .root_hash = hash_root(root),
    };
    size_t bytes = count * record_size;
    uint32_t digest = hash32(UINT32_C(2166136261), &header, sizeof(header));
    header.checksum = hash32(digest, records, bytes);
    // 先写临时文件再替换：掉电最多丢掉这次快照，绝不会留下半个书架。
    // Temp file then rename: a power loss can only lose this snapshot, never leave half a shelf.
    FILE *file = fopen(SHELF_CACHE_TEMP, "wb");
    if (!file) return false;
    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
        fwrite(records, 1, bytes, file) == bytes && fflush(file) == 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) { remove(SHELF_CACHE_TEMP); return false; }
    remove(SHELF_CACHE_PATH);
    if (rename(SHELF_CACHE_TEMP, SHELF_CACHE_PATH) == 0) return true;
    remove(SHELF_CACHE_TEMP);
    return false;
}

void book_shelf_cache_drop(void) {
    remove(SHELF_CACHE_PATH);
    remove(SHELF_CACHE_TEMP);
}
