/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：音乐元数据解析（见 music_tags.h）。只读文件头部；解析失败的字段保持
 * 回退值（标题=去扩展名/去前导序号的文件名，艺人=空，时长=0）。
 *
 * English: music metadata parsing (see music_tags.h). Header reads only; any
 * field that fails to parse keeps its fallback (title = file name minus
 * extension and leading track number, artist = empty, duration = 0).
 *
 * 冻结：不写文件、不缓存、不做播放决策。
 * Frozen: no writes, no caching, no playback policy.
 */

#include "music_tags.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"

/* ---- 基础读取 / Primitive readers ---- */

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t rd_le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

// ID3 的 syncsafe 整数：每字节只用低 7 位。/ ID3 syncsafe integer: low 7 bits of each byte.
static uint32_t rd_syncsafe(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

// 尾部 NUL 与空白一律去掉，标签文件几乎都带终止符。/ Drop trailing NULs and blanks; tag files nearly always carry terminators.
static void trim_end(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == 0 || s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
}

/* ---- 文本编码 / Text encodings ---- */

static size_t utf8_copy(const uint8_t *src, size_t len, char *out, size_t cap) {
    if (!cap) return 0;
    size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(out, src, n);
    out[n] = 0;
    return n;
}

static size_t latin1_to_utf8(const uint8_t *src, size_t len, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; ++i) {
        if (src[i] < 0x80) {
            out[o++] = (char)src[i];
        } else if (o + 2 < cap) {
            out[o++] = (char)(0xC0 | (src[i] >> 6));
            out[o++] = (char)(0x80 | (src[i] & 0x3F));
        } else {
            break;
        }
    }
    out[o] = 0;
    return o;
}

static size_t utf16_to_utf8(const uint8_t *src, size_t len, bool big_endian, char *out,
                            size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i + 1 < len && o + 1 < cap; i += 2) {
        uint32_t cp = big_endian ? (uint32_t)((src[i] << 8) | src[i + 1])
                                 : (uint32_t)(src[i] | (src[i + 1] << 8));
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {  // 代理对高位 / high surrogate
            uint32_t lo = big_endian ? (uint32_t)((src[i + 2] << 8) | src[i + 3])
                                     : (uint32_t)(src[i + 2] | (src[i + 3] << 8));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp == 0) break;  // 终止 NUL / terminator
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= cap) break;
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (o + 3 >= cap) break;
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 4 >= cap) break;
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

// ID3 文本编码字节：0=Latin-1，1=UTF-16 带 BOM，2=UTF-16BE，3=UTF-8。
// ID3 text encoding byte: 0 = Latin-1, 1 = UTF-16 with BOM, 2 = UTF-16BE, 3 = UTF-8.
static void id3_text(uint8_t enc, const uint8_t *src, size_t len, char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    switch (enc) {
        case 0:
            latin1_to_utf8(src, len, out, cap);
            break;
        case 1:
            if (len >= 2 && src[0] == 0xFF && src[1] == 0xFE)
                utf16_to_utf8(src + 2, len - 2, false, out, cap);
            else if (len >= 2 && src[0] == 0xFE && src[1] == 0xFF)
                utf16_to_utf8(src + 2, len - 2, true, out, cap);
            else
                utf16_to_utf8(src, len, false, out, cap);
            break;
        case 2:
            utf16_to_utf8(src, len, true, out, cap);
            break;
        default:
            utf8_copy(src, len, out, cap);
            break;
    }
    trim_end(out);
}

// USLT 帧体：[enc][lang 3][desc（按 enc 终止）][歌词文本]。跳过描述后按原编码解码。
// USLT body: [enc][lang 3][desc, terminated per enc][lyric text]. Skip the descriptor, then
// decode the text with its own encoding.
static size_t uslt_desc_len(uint8_t enc, const uint8_t *p, size_t len) {
    if (enc == 1 || enc == 2) {  // UTF-16：描述以 0x0000 结束（配对对齐）/ UTF-16: descriptor ends at 0x0000 on a code-unit boundary
        size_t i = 0;
        bool be = enc == 2;
        if (enc == 1 && len >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
            be = false;
            i = 2;
        } else if (enc == 1 && len >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
            be = true;
            i = 2;
        }
        for (; i + 1 < len; i += 2) {
            uint16_t u = be ? (uint16_t)((p[i] << 8) | p[i + 1]) : (uint16_t)(p[i] | (p[i + 1] << 8));
            if (u == 0) return i + 2;
        }
        return len;
    }
    for (size_t i = 0; i < len; ++i)
        if (p[i] == 0) return i + 1;
    return len;
}

static void uslt_text(uint8_t enc, const uint8_t *src, size_t len, char *out, size_t cap) {
    if (!cap) return;
    size_t off = uslt_desc_len(enc, src, len);
    id3_text(enc, src + off, len - off, out, cap);
}

/* ---- 文件名回退 / File-name fallback ---- */

static void name_fallback(const char *path, char *out, size_t cap) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strlcpy(out, base, cap);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
    // 去前导序号："03 - 蓝色" → " - 蓝色" → "蓝色"。/ Drop a leading track number: "03 - x" → "x".
    if (isdigit((unsigned char)out[0])) {
        char *p = out;
        while (isdigit((unsigned char)*p)) ++p;
        if (*p == '.') {
            ++p;
            while (*p == ' ') ++p;
            memmove(out, p, strlen(p) + 1);
        }
    }
    trim_end(out);
}

/* ---- MP3 ---- */

// 仅 Layer III / MPEG1、2、2.5 的比特率表（kbps，索引 1..14）。
// Layer III only, for MPEG1/2/2.5 (kbps, index 1..14).
static const uint16_t kBitrateMpeg1L3[15] = {0,  32,  40,  48,  56,  64,  80, 96,
                                             112, 128, 160, 192, 224, 256, 320};
static const uint16_t kBitrateMpeg2L3[15] = {0, 8,  16, 24, 32,  40,  48, 56,
                                             64, 80, 96, 112, 128, 144, 160};

static const uint32_t kSampleRate[3][3] = {{11025, 12000, 8000},    // MPEG 2.5 / MPEG 2.5
                                           {0, 0, 0},               // reserved
                                           {22050, 24000, 16000}};  // MPEG 2 / MPEG 2

static void parse_id3(FILE *f, long fsize, char *title, size_t tcap, char *artist, size_t acap,
                      long *audio_off, char *lyrics, size_t lcap) {
    (void)fsize;
    *audio_off = 0;
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10) return;
    if (memcmp(hdr, "ID3", 3) != 0) {
        fseek(f, 0, SEEK_SET);
        return;
    }
    const uint8_t major = hdr[3];
    const uint32_t tag_size = rd_syncsafe(hdr + 6);
    long total = 10 + (long)tag_size;
    if (major == 4 && (hdr[5] & 0x10)) total += 10;  // v2.4 尾部 footer / footer present
    *audio_off = total;
    if (major != 3 && major != 4) {  // v2.2 帧格式不同，只跳过不解析 / v2.2 frames differ: skip only
        fseek(f, total, SEEK_SET);
        return;
    }
    // 标签通常几 KB；超大只解析前 64KB（audio_off 已按真实尺寸跳过）。
    // Tags are usually a few KB; anything larger parses only the first 64 KB (audio_off still skips the real size).
    const uint32_t want = tag_size < 65536 ? tag_size : 65536;
    uint8_t *buf = want ? heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!buf) buf = want ? malloc(want) : NULL;
    if (!buf) {
        fseek(f, total, SEEK_SET);
        return;
    }
    const uint32_t got = (uint32_t)fread(buf, 1, want, f);
    long pos = 0;
    while (pos + 10 <= (long)got) {
        const uint8_t *fr = buf + pos;
        if (!isalnum(fr[0])) break;
        uint32_t size = (major == 4) ? rd_syncsafe(fr + 4) : rd_be32(fr + 4);
        if (size == 0 || pos + 10 + (long)size > (long)got) break;
        if (memcmp(fr, "TIT2", 4) == 0 && size > 1)
            id3_text(fr[10], fr + 11, size - 1, title, tcap);
        else if (memcmp(fr, "TPE1", 4) == 0 && size > 1)
            id3_text(fr[10], fr + 11, size - 1, artist, acap);
        else if (memcmp(fr, "USLT", 4) == 0 && size > 5 && lyrics && !lyrics[0])
            uslt_text(fr[10], fr + 11, size - 1, lyrics, lcap);
        pos += 10 + (long)size;
    }
    free(buf);
    fseek(f, total, SEEK_SET);
}

static uint32_t mp3_duration(FILE *f, long audio_off, long fsize) {
    if (audio_off < 0 || fsize <= audio_off + 4) return 0;
    if (fseek(f, audio_off, SEEK_SET) != 0) return 0;
    uint8_t b[16];
    if (fread(b, 1, 16, f) != 16) return 0;
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return 0;
    const uint8_t version = (b[1] >> 3) & 3;   // 3=MPEG1 2=MPEG2 0=MPEG2.5
    const uint8_t layer = (b[1] >> 1) & 3;     // 1=Layer III
    const uint8_t br_idx = b[2] >> 4;
    const uint8_t sr_idx = (b[2] >> 2) & 3;
    if (version == 1 || layer != 1 || sr_idx == 3) return 0;
    const uint32_t sample_rate = kSampleRate[version == 3 ? 2 : version][sr_idx];
    const uint32_t spf = (version == 3) ? 1152 : 576;  // 每帧样本数 / samples per frame
    if (!sample_rate) return 0;
    const bool mono = ((b[3] >> 6) & 3) == 3;
    const int side_info = (version == 3) ? (mono ? 17 : 32) : (mono ? 9 : 17);

    // Xing/Info：VBR 帧数。/ Xing/Info: VBR frame count.
    if (fseek(f, audio_off + 4 + side_info, SEEK_SET) == 0 && fread(b, 1, 12, f) == 12) {
        if (memcmp(b, "Xing", 4) == 0 || memcmp(b, "Info", 4) == 0) {
            const uint32_t flags = rd_be32(b + 4);
            if (flags & 1) {
                const uint32_t frames = rd_be32(b + 8);
                if (frames) return (uint32_t)(((uint64_t)frames * spf * 1000) / sample_rate);
            }
        }
    }
    // CBR 估算：文件大小 / 比特率；顺带扣掉尾部 ID3v1。
    // CBR estimate: file size / bitrate, minus a trailing ID3v1 tag if present.
    if (br_idx == 0 || br_idx > 14) return 0;
    const uint16_t kbps = (version == 3) ? kBitrateMpeg1L3[br_idx] : kBitrateMpeg2L3[br_idx];
    if (!kbps) return 0;
    long bytes = fsize - audio_off;
    if (fsize > 128 && fseek(f, fsize - 128, SEEK_SET) == 0) {
        uint8_t tag[3];
        if (fread(tag, 1, 3, f) == 3 && memcmp(tag, "TAG", 3) == 0) bytes -= 128;
    }
    if (bytes <= 0) return 0;
    return (uint32_t)(((uint64_t)bytes * 8) / kbps);
}

/* ---- FLAC ---- */

// Vorbis 注释里的 TITLE / ARTIST（UTF-8 原样拷贝）；lyrics 非空时顺带收 LYRICS 系键。
// TITLE / ARTIST from a Vorbis comment block (raw UTF-8 copy); with a lyrics buffer it also
// collects the LYRICS-family keys.
static void parse_vorbis(const uint8_t *buf, uint32_t len, char *title, size_t tcap, char *artist,
                         size_t acap, char *lyrics, size_t lcap) {
    uint32_t p = 0;
    if (p + 4 > len) return;
    p += 4 + rd_le32(buf + p);  // vendor / vendor string
    if (p + 4 > len) return;
    const uint32_t count = rd_le32(buf + p);
    p += 4;
    for (uint32_t i = 0; i < count && p + 4 <= len; ++i) {
        const uint32_t l = rd_le32(buf + p);
        p += 4;
        if (l == 0 || p + l > len) break;
        const uint8_t *eq = memchr(buf + p, '=', l);
        if (eq) {
            const size_t key_len = (size_t)(eq - (buf + p));
            const uint8_t *val = eq + 1;
            const size_t val_len = l - key_len - 1;
            if (key_len == 5 && strncasecmp((const char *)(buf + p), "TITLE", 5) == 0) {
                utf8_copy(val, val_len, title, tcap);
                trim_end(title);
            } else if (key_len == 6 && strncasecmp((const char *)(buf + p), "ARTIST", 6) == 0) {
                utf8_copy(val, val_len, artist, acap);
                trim_end(artist);
            } else if (lyrics && !lyrics[0] &&
                       ((key_len == 6 && strncasecmp((const char *)(buf + p), "LYRICS", 6) == 0) ||
                        (key_len == 14 &&
                         strncasecmp((const char *)(buf + p), "UNSYNCEDLYRICS", 14) == 0) ||
                        (key_len == 12 &&
                         strncasecmp((const char *)(buf + p), "SYNCEDLYRICS", 12) == 0))) {
                utf8_copy(val, val_len, lyrics, lcap);
                trim_end(lyrics);
            }
        }
        p += l;
    }
}

static void flac_parse(FILE *f, char *title, size_t tcap, char *artist, size_t acap,
                       uint32_t *duration_ms) {
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "fLaC", 4) != 0) return;
    for (int guard = 0; guard < 16; ++guard) {
        uint8_t hdr[4];
        if (fread(hdr, 1, 4, f) != 4) return;
        const bool last = (hdr[0] & 0x80) != 0;
        const uint8_t type = hdr[0] & 0x7F;
        const uint32_t len = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
        if (type == 0 && len == 34) {  // STREAMINFO / STREAMINFO
            uint8_t si[34];
            if (fread(si, 1, 34, f) != 34) return;
            const uint32_t sample_rate = ((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) |
                                         (si[12] >> 4);
            const uint64_t total = ((uint64_t)(si[12] & 0x0F) << 32) | ((uint64_t)si[13] << 24) |
                                   ((uint64_t)si[14] << 16) | ((uint64_t)si[15] << 8) | si[16];
            if (sample_rate && total)
                *duration_ms = (uint32_t)((total * 1000) / sample_rate);
        } else if (type == 4) {  // VORBIS_COMMENT / VORBIS_COMMENT
            const uint32_t want = len < 8192 ? len : 8192;
            uint8_t *buf = heap_caps_malloc(want ? want : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) buf = malloc(want ? want : 1);
            if (buf) {
                const uint32_t got = (uint32_t)fread(buf, 1, want, f);
                parse_vorbis(buf, got, title, tcap, artist, acap, NULL, 0);
                free(buf);
            }
            if (want < len) fseek(f, (long)(len - want), SEEK_CUR);
        } else {
            fseek(f, (long)len, SEEK_CUR);
        }
        if (last) return;
    }
}

/* ---- WAV ---- */

static void wav_parse(FILE *f, uint32_t *duration_ms) {
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12) return;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return;
    uint32_t byte_rate = 0;
    bool have_fmt = false;
    for (int guard = 0; guard < 8; ++guard) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) return;
        const uint32_t size = rd_le32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) return;
            byte_rate = rd_le32(fmt + 8);
            have_fmt = true;
            fseek(f, (long)(size - 16), SEEK_CUR);
        } else if (memcmp(ch, "data", 4) == 0) {
            if (have_fmt && byte_rate && size)
                *duration_ms = (uint32_t)(((uint64_t)size * 1000) / byte_rate);
            return;
        } else {
            fseek(f, (long)size, SEEK_CUR);
        }
        if (size & 1) fseek(f, 1, SEEK_CUR);  // 块按偶数字节对齐 / chunks pad to even size
    }
}

/* ---- M4A ---- */

// 在文件的头/尾各 256KB 里找 "mvhd"（moov 盒可前可后）。/ Hunt "mvhd" in the first and last 256 KB (moov may sit at either end).
static bool find_atom(FILE *f, long fsize, const char *atom, long *out_off) {
    const long win = 256 * 1024;
    const long spans[2][2] = {{0, fsize < win ? fsize : win},
                              {fsize > win ? fsize - win : 0, fsize}};
    uint8_t *buf = heap_caps_malloc(win, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(win);
    if (!buf) return false;
    bool found = false;
    for (int s = 0; s < 2 && !found; ++s) {
        const long start = spans[s][0], end = spans[s][1];
        if (end <= start) continue;
        if (fseek(f, start, SEEK_SET) != 0) break;
        const long n = end - start;
        const size_t got = fread(buf, 1, (size_t)n, f);
        if (got < 4) continue;
        for (size_t i = 0; i + 4 <= got; ++i) {
            if (memcmp(buf + i, atom, 4) == 0) {
                *out_off = start + (long)i;
                found = true;
                break;
            }
        }
    }
    free(buf);
    return found;
}

static void m4a_parse(FILE *f, long fsize, uint32_t *duration_ms) {
    long off = 0;
    if (!find_atom(f, fsize, "mvhd", &off)) return;
    if (fseek(f, off, SEEK_SET) != 0) return;
    uint8_t b[36];
    if (fread(b, 1, 36, f) != 36) return;
    if (memcmp(b, "mvhd", 4) != 0) return;
    const uint8_t version = b[4];
    uint32_t timescale = 0;
    uint64_t dur = 0;
    if (version == 1) {
        timescale = rd_be32(b + 24);
        dur = ((uint64_t)rd_be32(b + 28) << 32) | rd_be32(b + 32);
    } else {
        timescale = rd_be32(b + 16);
        dur = rd_be32(b + 20);
    }
    if (timescale && dur != 0xFFFFFFFFull)
        *duration_ms = (uint32_t)(((uint64_t)dur * 1000) / timescale);
}

/* ---- 入口 / Entry ---- */

void music_tags_read(const char *path, char *title, size_t title_cap, char *artist,
                     size_t artist_cap, uint32_t *duration_ms) {
    if (title_cap) title[0] = 0;
    if (artist_cap) artist[0] = 0;
    *duration_ms = 0;
    name_fallback(path, title, title_cap);  // 失败即回退 / fallback in place

    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    const char *ext = strrchr(path, '.');
    long audio_off = 0;
    if (ext && strcasecmp(ext, ".mp3") == 0) {
        parse_id3(f, fsize, title, title_cap, artist, artist_cap, &audio_off, NULL, 0);
        *duration_ms = mp3_duration(f, audio_off, fsize);
    } else if (ext && strcasecmp(ext, ".flac") == 0) {
        flac_parse(f, title, title_cap, artist, artist_cap, duration_ms);
    } else if (ext && strcasecmp(ext, ".wav") == 0) {
        wav_parse(f, duration_ms);
    } else if (ext && (strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".aac") == 0)) {
        m4a_parse(f, fsize, duration_ms);
    }
    fclose(f);
    if (title_cap && !title[0]) name_fallback(path, title, title_cap);
}

/* ---- 内嵌歌词 / Embedded lyrics ---- */

// FLAC：走到 VORBIS_COMMENT 块，只取 LYRICS 系键。/ FLAC: walk to the VORBIS_COMMENT block and
// take the LYRICS-family key only.
static bool flac_lyrics(FILE *f, char *out, size_t cap) {
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "fLaC", 4) != 0) return false;
    for (int guard = 0; guard < 16; ++guard) {
        uint8_t hdr[4];
        if (fread(hdr, 1, 4, f) != 4) return false;
        const bool last = (hdr[0] & 0x80) != 0;
        const uint8_t type = hdr[0] & 0x7F;
        const uint32_t len = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
        if (type == 4) {  // 同步歌词常整段塞进 VORBIS_COMMENT，缓冲给足 48KB
            // Synced lyrics often ride in the Vorbis comment; give the buffer 48 KB.
            const uint32_t want = len < 49152 ? len : 49152;
            uint8_t *buf = heap_caps_malloc(want ? want : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) buf = malloc(want ? want : 1);
            if (buf) {
                const uint32_t got = (uint32_t)fread(buf, 1, want, f);
                parse_vorbis(buf, got, NULL, 0, NULL, 0, out, cap);
                free(buf);
            }
            return out[0] != 0;
        }
        fseek(f, (long)len, SEEK_CUR);
        if (last) break;
    }
    return false;
}

// M4A：找 ©lyr 盒，校验尺寸后取 data 子盒负载（UTF-8 文本）。/ M4A: find the ©lyr atom, check
// sizes, then take the data child's payload (UTF-8 text).
static bool m4a_lyrics(FILE *f, long fsize, char *out, size_t cap) {
    long off = 0;
    if (!find_atom(f, fsize, "\xa9lyr", &off) || off < 4) return false;
    if (fseek(f, off - 4, SEEK_SET) != 0) return false;
    uint8_t hdr[36];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    if (memcmp(hdr + 4, "\xa9lyr", 4) != 0) return false;
    const uint32_t size0 = rd_be32(hdr);
    const uint32_t csize = rd_be32(hdr + 8);
    if (size0 != csize + 8 || memcmp(hdr + 12, "data", 4) != 0) return false;
    if (csize < 17) return false;  // 负载至少 1 字节 / payload is at least one byte
    const uint32_t plen = csize - 16;
    const size_t want = plen < cap - 1 ? plen : cap - 1;  // 超长截断 / truncate when too long
    if (fseek(f, off + 20, SEEK_SET) != 0) return false;
    if (fread(out, 1, want, f) != want) {
        out[0] = 0;
        return false;
    }
    out[want] = 0;
    trim_end(out);
    return out[0] != 0;
}

size_t music_tags_lyrics(const char *path, char *out, size_t cap) {
    if (!cap) return 0;
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    const char *ext = strrchr(path, '.');
    bool ok = false;
    if (ext && strcasecmp(ext, ".mp3") == 0) {
        long audio_off = 0;
        parse_id3(f, fsize, NULL, 0, NULL, 0, &audio_off, out, cap);
        ok = out[0] != 0;
    } else if (ext && strcasecmp(ext, ".flac") == 0) {
        ok = flac_lyrics(f, out, cap);
    } else if (ext && (strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".aac") == 0)) {
        ok = m4a_lyrics(f, fsize, out, cap);
    }
    fclose(f);
    if (!ok) out[0] = 0;
    return ok ? strlen(out) : 0;
}
