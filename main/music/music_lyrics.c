/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：歌词加载与行解析（见 music_lyrics.h）。单块 PSRAM 缓冲：原始文本 → 行表 +
 * 文本池；发布前后用计数的原子读写同步，读者最多看到一次切换中的旧文本，不会
 * 解引用已释放的内存（缓冲只分配一次）。
 *
 * English: lyric loading and line parsing (see music_lyrics.h). One PSRAM block holds
 * raw text, the line table and the text pool; the count's atomic publish synchronizes
 * the sides, so a reader at worst sees one mid-switch stale text but never a freed
 * pointer (the block is allocated once).
 *
 * 冻结：不写歌词文件、不做播放决策。
 * Frozen: never writes lyric files; no playback policy.
 */

#include "music_lyrics.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "music_tags.h"

#define LYR_RAW_SIZE (32 * 1024)   // 歌词源文本上限 / cap on the raw lyric source
#define LYR_POOL_SIZE (32 * 1024)  // 解析后文本池 / parsed text pool
#define LYR_MAX_LINES 400
#define LYR_INHERIT 0xFFFFFFFFu    // 无时间戳行：跟随上一行 / untimed line: follow the previous

typedef struct {
    uint32_t ms;
    uint16_t off;  // 文本池偏移（含 NUL 结尾）/ pool offset (text is NUL-terminated)
    uint16_t len;
} lyr_line_t;

// 单块布局：[原始文本][行表][文本池]；s_blob 恒非空后不再改指针。
// One block: [raw][line table][pool]; s_blob never moves once it exists.
static uint8_t *s_blob;
static char *s_raw;
static lyr_line_t *s_lines;
static char *s_pool;
static volatile size_t s_count;

static void ensure_blob(void) {
    if (s_blob) return;
    const size_t total = LYR_RAW_SIZE + LYR_MAX_LINES * sizeof(lyr_line_t) + LYR_POOL_SIZE;
    uint8_t *b = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) b = malloc(total);
    if (!b) return;
    s_blob = b;
    s_raw = (char *)b;
    s_lines = (lyr_line_t *)(b + LYR_RAW_SIZE);
    s_pool = (char *)(b + LYR_RAW_SIZE + LYR_MAX_LINES * sizeof(lyr_line_t));
    s_pool[LYR_POOL_SIZE - 1] = 0;
}

/* ---- LRC 时间标签 / LRC time tags ---- */

// 从 '[' 起解析 [m:ss]、[m:ss.x]、[m:ss.xx]、[m:ss.xxx]；成功返回消耗长度，否则 0。
// Parse [m:ss(.x..xx...)] starting at '['; returns bytes consumed, 0 on no match.
static int time_tag_ms(const char *p, const char *end, uint32_t *ms) {
    const char *q = p + 1;
    uint32_t m = 0, s = 0, frac = 0;
    int digits = 0, frac_digits = 0;
    while (q < end && isdigit((unsigned char)*q)) {
        m = m * 10 + (uint32_t)(*q++ - '0');
        if (++digits > 4) return 0;
    }
    if (!digits || q >= end || *q != ':') return 0;
    ++q;
    digits = 0;
    while (q < end && isdigit((unsigned char)*q)) {
        s = s * 10 + (uint32_t)(*q++ - '0');
        if (++digits > 2) return 0;
    }
    if (!digits) return 0;
    if (q < end && (*q == '.' || *q == ',')) {
        ++q;
        while (q < end && isdigit((unsigned char)*q) && frac_digits < 3) {
            frac = frac * 10 + (uint32_t)(*q++ - '0');
            ++frac_digits;
        }
        while (frac_digits++ < 3) frac *= 10;
    }
    if (q >= end || *q != ']') return 0;
    *ms = (m * 60 + s) * 1000 + frac;
    return (int)(q - p + 1);
}

// 文本池游标：整个文件共享，解析开始时归零。/ Pool cursor shared by the file, zeroed
// when a parse starts.
static size_t s_pool_pos;

// 把一行文本追加进文本池；返回池偏移。/ Append a line into the pool; returns the pool offset.
static bool pool_add(const char *text, size_t len, uint16_t *off, uint16_t *llen) {
    if (s_pool_pos + len + 1 > LYR_POOL_SIZE) return false;
    memcpy(s_pool + s_pool_pos, text, len);
    s_pool[s_pool_pos + len] = 0;
    *off = (uint16_t)s_pool_pos;
    *llen = (uint16_t)len;
    s_pool_pos += len + 1;
    return true;
}

/* ---- 解析 / Parsing ---- */

static size_t parse_raw(const char *raw, uint32_t duration_ms) {
    size_t n = 0;
    bool any_stamp = false;
    int32_t offset_ms = 0;
    s_pool_pos = 0;

    const char *p = raw;
    while (*p && n < LYR_MAX_LINES) {
        const char *eol = strchr(p, '\n');
        const char *end = eol ? eol : p + strlen(p);
        const char *le = end;
        while (le > p && (le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t')) --le;

        uint32_t times[8];
        int tcount = 0;
        bool meta = false;
        const char *q = p;
        while (q < le && *q == '[') {
            const char *rb = memchr(q, ']', (size_t)(le - q));
            if (!rb) break;
            uint32_t ms = 0;
            int used = time_tag_ms(q, le, &ms);
            if (used > 0 && tcount < (int)(sizeof(times) / sizeof(times[0]))) {
                times[tcount++] = ms;
                any_stamp = true;
                q += used;
                continue;
            }
            // 非时间标签：[offset:±ms] 记偏移，[ar:] 等元数据整行跳过。
            // Non-time tag: [offset:±ms] records an adjustment, [ar:] and friends skip the line.
            const char *inner = q + 1;
            size_t ilen = (size_t)(rb - inner);
            if (ilen > 7 && strncasecmp(inner, "offset:", 7) == 0) {
                char tmp[16];
                size_t c = ilen - 7 < sizeof(tmp) - 1 ? ilen - 7 : sizeof(tmp) - 1;
                memcpy(tmp, inner + 7, c);
                tmp[c] = 0;
                offset_ms = (int32_t)strtol(tmp, NULL, 10);
            }
            meta = true;
            q = rb + 1;
        }
        while (q < le && *q == ' ') ++q;
        const size_t text_len = (size_t)(le - q);

        if (tcount == 0) {
            // 纯文本行：元数据行跳过，其余标记为跟随上一行（全文件无时间戳时按
            // 时长均分）。/ Plain line: metadata rows are dropped, the rest inherit the
            // previous line (evenly spread by duration when nothing is stamped).
            if (meta || text_len == 0) {
                p = eol ? eol + 1 : end;
                continue;
            }
            uint16_t off = 0, len = 0;
            if (pool_add(q, text_len, &off, &len)) {
                s_lines[n].ms = LYR_INHERIT;
                s_lines[n].off = off;
                s_lines[n].len = len;
                ++n;
            }
        } else {
            for (int t = 0; t < tcount && n < LYR_MAX_LINES; ++t) {
                uint16_t off = 0, len = 0;
                if (text_len && !pool_add(q, text_len, &off, &len)) break;
                s_lines[n].ms = times[t];
                s_lines[n].off = off;
                s_lines[n].len = len;
                ++n;
            }
        }
        p = eol ? eol + 1 : end;
    }
    if (!n) return 0;

    if (!any_stamp) {
        // 无时间戳：有总时长就按行均分，否则只让首行在开头可见。
        // No stamps: spread evenly when the duration is known, otherwise only the first
        // line shows at the start.
        for (size_t i = 0; i < n; ++i)
            s_lines[i].ms = duration_ms ? (uint32_t)(((uint64_t)i * duration_ms) / n)
                                        : (i == 0 ? 0 : LYR_INHERIT);
    }
    // 跟随行继承前一条的真实时间（仅当文件里确实有时间戳的混合情形）。
    // Inheriting rows take the previous row's real time (only for a mixed stamped file).
    if (any_stamp) {
        uint32_t prev = 0;
        for (size_t i = 0; i < n; ++i) {
            if (s_lines[i].ms == LYR_INHERIT) s_lines[i].ms = prev;
            prev = s_lines[i].ms;
        }
    }
    // [offset:+N] 让歌词提前显示，故时间减去偏移。/ A positive [offset] means lyrics show
    // earlier, so subtract it.
    if (offset_ms)
        for (size_t i = 0; i < n; ++i) {
            int64_t v = (int64_t)s_lines[i].ms - offset_ms;
            s_lines[i].ms = v < 0 ? 0 : (uint32_t)v;
        }
    // 行按时间排序（插入排序，行数有限）。/ Sort by time (insertion sort; the count is small).
    for (size_t i = 1; i < n; ++i) {
        lyr_line_t key = s_lines[i];
        size_t j = i;
        while (j && s_lines[j - 1].ms > key.ms) {
            s_lines[j] = s_lines[j - 1];
            --j;
        }
        s_lines[j] = key;
    }
    return n;
}

/* ---- 入口 / Entry ---- */

void music_lyrics_clear(void) { __atomic_store_n(&s_count, 0, __ATOMIC_RELEASE); }

void music_lyrics_load(const char *track_path, uint32_t duration_ms) {
    music_lyrics_clear();
    if (!track_path) return;
    ensure_blob();
    if (!s_blob) return;

    // 先建同目录同名 .lrc（替换最后一个扩展名）。/ Build the same-directory .lrc first
    // (replace the last extension).
    char lrc[256];
    size_t plen = strlen(track_path);
    if (plen >= sizeof(lrc)) return;
    memcpy(lrc, track_path, plen + 1);
    char *slash = strrchr(lrc, '/');
    char *dot = strrchr(lrc, '.');
    if (dot && (!slash || dot > slash)) {
        strcpy(dot, ".lrc");
    } else if (plen + 4 < sizeof(lrc)) {
        strcpy(lrc + plen, ".lrc");
    }

    s_raw[0] = 0;
    FILE *f = fopen(lrc, "rb");
    if (f) {
        size_t pos = 0;
        while (pos + 1 < LYR_RAW_SIZE) {
            size_t got = fread(s_raw + pos, 1, LYR_RAW_SIZE - 1 - pos, f);
            if (!got) break;
            pos += got;
        }
        s_raw[pos] = 0;
        fclose(f);
    }
    // 同目录歌词缺失或为空时回退内嵌歌词。/ Fall back to embedded lyrics when the sidecar
    // file is missing or empty.
    if (!s_raw[0]) music_tags_lyrics(track_path, s_raw, LYR_RAW_SIZE);

    size_t n = s_raw[0] ? parse_raw(s_raw, duration_ms) : 0;
    __atomic_store_n(&s_count, n, __ATOMIC_RELEASE);
}

/* ---- 查询辅助 / Query helpers ---- */

// 上界二分：第一条 ms > 播放位置的下标（其前一条即当前句）。/ Binary upper bound: index of
// the first line past the position (the line before it is the current one).
static size_t lyric_upper_bound(size_t n, uint32_t ms) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_lines[mid].ms <= ms)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// 取下标行的文本；越界、隐藏行（无时长均分的跟随行）与换曲瞬间的撕裂读返回 ""。
// Text of a line; "" on out-of-range, hidden rows (untimed followers) and mid-switch tears.
static const char *lyric_text(size_t i, size_t n) {
    if (i >= n) return "";
    const lyr_line_t *L = &s_lines[i];
    if (L->ms == LYR_INHERIT || !L->len) return "";
    if ((size_t)L->off + L->len >= LYR_POOL_SIZE) return "";
    return s_pool + L->off;
}

const char *music_lyrics_at(uint32_t ms) {
    size_t n = __atomic_load_n(&s_count, __ATOMIC_ACQUIRE);
    if (!n || !s_blob) return "";
    // 最后一条 ms <= 播放位置的行。/ The last line whose ms is at or before the position.
    size_t lo = lyric_upper_bound(n, ms);
    if (!lo) return "";
    return lyric_text(lo - 1, n);
}

// 当前句的前一行 / 后一行（越界、隐藏行与撕裂读都返回 ""）。
// The line before / after the current one ("" on boundary, hidden or torn entries).
const char *music_lyrics_prev(uint32_t ms) {
    size_t n = __atomic_load_n(&s_count, __ATOMIC_ACQUIRE);
    if (!n || !s_blob) return "";
    size_t lo = lyric_upper_bound(n, ms);
    if (lo < 2) return "";
    return lyric_text(lo - 2, n);
}

const char *music_lyrics_next(uint32_t ms) {
    size_t n = __atomic_load_n(&s_count, __ATOMIC_ACQUIRE);
    if (!n || !s_blob) return "";
    size_t lo = lyric_upper_bound(n, ms);
    return lyric_text(lo, n);
}
