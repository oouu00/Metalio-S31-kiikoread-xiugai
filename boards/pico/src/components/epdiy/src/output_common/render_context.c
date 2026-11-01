#include "render_context.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "../epdiy.h"
#include "lut.h"
#include "render_method.h"

// ---- 夜间模式输出极性（kiiko 移植）----
// 反转发生在波形表的 (to, from) 索引上：液晶收到的驱动序列与「整屏反色后的画面」完全一致，
// framebuffer 仍是原来的浅色语义，所以差分、局刷、玻璃读回、字体抗锯齿都不用改。
// Inversion happens on the waveform (to, from) indexes, so the panel receives exactly the
// sequence of an inverted image while the framebuffer keeps its light-theme meaning.

static const char* POLARITY_TAG = "epd_pol";
static bool s_inverted;
static uint8_t* s_inv_luts;
static size_t s_inv_cap;
static EpdWaveformPhases s_inv_phases;

void epd_set_inverted(bool inverted) {
    s_inverted = inverted;
}

bool epd_get_inverted(void) {
    return s_inverted;
}

/// 波形表是每相 16(to) × 16(from) 的 2bit 矩阵，按 to 分 4 字节、字节内 from 从高到低。
/// The waveform table is a 16(to) × 16(from) 2-bit matrix per phase, packed as 4 bytes per
/// `to` with `from` running from the high bits down.
static inline uint8_t polarity_lut_get(const uint8_t* luts, int to, int from) {
    const uint8_t byte = luts[(size_t)to * 4u + (size_t)(from >> 2)];
    return (uint8_t)((byte >> ((3 - (from & 3)) * 2)) & 3u);
}

static inline void polarity_lut_put(uint8_t* luts, int to, int from, uint8_t value) {
    uint8_t* byte = &luts[(size_t)to * 4u + (size_t)(from >> 2)];
    const int shift = (3 - (from & 3)) * 2;
    *byte = (uint8_t)((*byte & ~(3u << shift)) | ((value & 3u) << shift));
}

const EpdWaveformPhases* epd_polarity_phases(const EpdWaveformPhases* phases) {
    if (!s_inverted || phases == NULL || phases->luts == NULL || phases->phases <= 0) {
        return phases;
    }
    const size_t need = (size_t)phases->phases * 64u;
    if (s_inv_cap < need) {
        uint8_t* grown = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (grown == NULL) {
            // 内存不够就退化成正常极性，宁可不反色也不能刷错画面。
            // Fall back to normal polarity rather than driving a wrong image.
            ESP_LOGE(POLARITY_TAG, "no RAM for inverted waveform (%u bytes)", (unsigned)need);
            s_inverted = false;
            return phases;
        }
        heap_caps_free(s_inv_luts);
        s_inv_luts = grown;
        s_inv_cap = need;
    }
    memset(s_inv_luts, 0, need);
    for (int f = 0; f < phases->phases; ++f) {
        const uint8_t* src = phases->luts + (size_t)f * 64u;
        uint8_t* dst = s_inv_luts + (size_t)f * 64u;
        for (int to = 0; to < 16; ++to) {
            for (int from = 0; from < 16; ++from) {
                polarity_lut_put(dst, to, from, polarity_lut_get(src, 15 - to, 15 - from));
            }
        }
    }
    s_inv_phases = *phases;
    s_inv_phases.luts = s_inv_luts;
    return &s_inv_phases;
}

/// For waveforms without per-phase timing, the default hold time for each line is 12us
const static int DEFAULT_FRAME_TIME = 120;

static inline int min(int x, int y) {
    return x < y ? x : y;
}

void get_buffer_params(
    RenderContext_t* ctx,
    int* bytes_per_line,
    const uint8_t** start_ptr,
    int* min_y,
    int* max_y,
    int* pixels_per_byte
) {
    EpdRect area = ctx->area;

    enum EpdDrawMode mode = ctx->mode;
    const EpdRect crop_to = ctx->crop_to;
    const bool horizontally_cropped = !(crop_to.x == 0 && crop_to.width == area.width);
    const bool vertically_cropped = !(crop_to.y == 0 && crop_to.height == area.height);

    // number of pixels per byte of input data
    int width_divider = 0;

    if (mode & MODE_PACKING_1PPB_DIFFERENCE) {
        *bytes_per_line = area.width;
        width_divider = 1;
    } else if (mode & MODE_PACKING_2PPB) {
        *bytes_per_line = area.width / 2 + area.width % 2;
        width_divider = 2;
    } else if (mode & MODE_PACKING_8PPB) {
        *bytes_per_line = (area.width / 8 + (area.width % 8 > 0));
        width_divider = 8;
    } else {
        ctx->error |= EPD_DRAW_INVALID_PACKING_MODE;
    }

    int crop_x = (horizontally_cropped ? crop_to.x : 0);
    int crop_y = (vertically_cropped ? crop_to.y : 0);
    int crop_h = (vertically_cropped ? crop_to.height : 0);

    const uint8_t* ptr_start = ctx->data_ptr;

    // Adjust for negative starting coordinates with optional crop
    if (area.x - crop_x < 0) {
        ptr_start += -(area.x - crop_x) / width_divider;
    }

    if (area.y - crop_y < 0) {
        ptr_start += -(area.y - crop_y) * *bytes_per_line;
    }

    // calculate start and end row with crop
    *min_y = area.y + crop_y;
    *max_y = min(*min_y + (vertically_cropped ? crop_h : area.height), area.height);
    *start_ptr = ptr_start;
    *pixels_per_byte = width_divider;
}

void IRAM_ATTR prepare_context_for_next_frame(RenderContext_t* ctx) {
    int frame_time = DEFAULT_FRAME_TIME;
    if (ctx->phase_times != NULL) {
        frame_time = ctx->phase_times[ctx->current_frame];
    }

    if (ctx->mode & MODE_EPDIY_MONOCHROME) {
        frame_time = MONOCHROME_FRAME_TIME;
    }
    ctx->frame_time = frame_time;

    const EpdWaveformPhases* phases
        = ctx->waveform->mode_data[ctx->waveform_index]->range_data[ctx->waveform_range];

    assert(ctx->lut_build_func != NULL);
    ctx->lut_build_func(ctx->conversion_lut, epd_polarity_phases(phases), ctx->current_frame);

    ctx->lines_prepared = 0;
    ctx->lines_consumed = 0;
    if (ctx->line_threads != NULL) {
        for (int i = 0; i < ctx->lines_total; i++) {
            ctx->line_threads[i] = i % NUM_RENDER_THREADS;
        }
    }
}

void epd_populate_line_mask(uint8_t* line_mask, const uint8_t* dirty_columns, int mask_len) {
    if (dirty_columns == NULL) {
        memset(line_mask, 0xFF, mask_len);
    } else {
        int pixels = mask_len * 4;
        for (int c = 0; c < pixels / 2; c += 2) {
            uint8_t mask = 0;
            mask |= (dirty_columns[c + 1] & 0xF0) != 0 ? 0xC0 : 0x00;
            mask |= (dirty_columns[c + 1] & 0x0F) != 0 ? 0x30 : 0x00;
            mask |= (dirty_columns[c] & 0xF0) != 0 ? 0x0C : 0x00;
            mask |= (dirty_columns[c] & 0x0F) != 0 ? 0x03 : 0x00;
            line_mask[c / 2] = mask;
        }
    }
}