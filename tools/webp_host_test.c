/* SPDX-License-Identifier: Apache-2.0
 * 中文：WebP 适配的主机回归：头部尺寸、RGB565 解码、动画拒绝、限幅缩放，以及 EPUB 内的
 * 尺寸探测与取图。
 * English: Host regression for WebP support: header sizes, RGB565 decoding, animation rejection,
 * pixel-cap scaling, and EPUB-side dimension probing plus extraction.
 */
#include "book_cover.h"
#include "book_epub.h"
#include "read_pico_webp.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 书内字体在主机上不加载：只让 book_epub.c 的字体契约有落点。
// Embedded faces never load on the host; this only satisfies book_epub.c's font contract.
int ttf_font_open_mem(uint8_t *data, size_t len, const char *label) {
    (void)len; (void)label;
    free(data);
    return -1;
}
void ttf_font_close_embedded(void) {}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *data = (uint8_t *)malloc((size_t)length);
    assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return data;
}

static void expect_header(const char *path, unsigned width, unsigned height, bool alpha,
                          bool animation) {
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    assert(book_webp_is(data, size));
    unsigned w = 0, h = 0;
    assert(book_webp_frame(data, size, &w, &h));
    assert(w == width && h == height);
    // 文件尺寸探测只读64字节，VP8/VP8L的大数据块不能使它误判。
    // File dimension probes read only64 bytes; large VP8/VP8L payloads must not invalidate them.
    w=h=0;
    assert(book_webp_frame(data,size<64?size:64,&w,&h));
    assert(w==width&&h==height);
    for(size_t n=0;n<20&&n<size;++n)assert(!book_webp_frame(data,n,&w,&h));
    read_pico_webp_info_t info;
    assert(read_pico_webp_info(data, size, &info));
    assert(info.width == width && info.height == height);
    assert(info.has_alpha == alpha);
    assert(info.has_animation == animation);
    free(data);
}

static void expect_rgb565(const char *path, unsigned width, unsigned height, bool decodes) {
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    uint8_t *plane = NULL;
    unsigned w = 0, h = 0;
    const bool ok = read_pico_webp_rgb565(data, size, 0, &plane, &w, &h);
    assert(ok == decodes);
    if (ok) {
        assert(w == width && h == height);
        // 平面必须真的有像素：看最小值与最大值是否拉开，避免只检查“非空”。
        // The plane must really carry pixels: compare its darkest and lightest values.
        const uint16_t *pixels = (const uint16_t *)plane;
        uint16_t low = 0xFFFF, high = 0;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            if (pixels[i] < low) low = pixels[i];
            if (pixels[i] > high) high = pixels[i];
        }
        assert(high > low);
        free(plane);
    }
    free(data);
}

static void expect_scaled(const char *path) {
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    uint8_t *plane = NULL;
    unsigned w = 0, h = 0;
    // 64x48 缩到 1/4 像素（32x24），长宽比保持不变。
    // 64x48 to a quarter of the pixels (32x24), keeping the aspect ratio.
    assert(read_pico_webp_rgb565(data, size, 32u * 24u, &plane, &w, &h));
    assert(w >= 31 && w <= 32 && h >= 23 && h <= 24);
    free(plane);
    free(data);
}

static void expect_epub(const char *path, const char *image_path, const char *image_file) {
    book_epub_t *book = NULL;
    assert(book_epub_open(path, &book) == ESP_OK && book);
    assert(book_epub_chapter_count(book) == 1);
    unsigned w = 0, h = 0;
    assert(book_epub_image_dimensions(book, 0, image_path, &w, &h) == ESP_OK);
    assert(w == 64 && h == 48);
    uint8_t *data = NULL;
    size_t size = 0;
    bool png = true;
    assert(book_epub_image(book, 0, image_path, &data, &size, &png) == ESP_OK);
    assert(!png && data && size > 0);
    assert(book_webp_is(data, size));
    // 与原始文件比对：取出的字节必须与 EPUB 包里的一致。
    // Compare against the source file: the extracted bytes must match the packed image.
    size_t source_size = 0;
    uint8_t *source = read_file(image_file, &source_size);
    assert(source_size == size && !memcmp(source, data, size));
    free(source);
    free(data);
    book_epub_close(book);
}

static void expect_exact_rgb565(const char *path) {
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    uint8_t *plane = NULL;
    unsigned w = 0, h = 0;
    assert(read_pico_webp_rgb565(data, size, 0, &plane, &w, &h));
    assert(w == 8 && h == 8);
    // 本机 uint16 顺序的 RGB565：红 F800、绿 07E0、蓝 001F、白 FFFF。
    // Native uint16 RGB565: red F800, green 07E0, blue 001F, white FFFF.
    const uint16_t *pixels = (const uint16_t *)plane;
    assert(pixels[1 * 8 + 1] == 0xF800);
    assert(pixels[1 * 8 + 5] == 0x07E0);
    assert(pixels[5 * 8 + 1] == 0x001F);
    assert(pixels[5 * 8 + 5] == 0xFFFF);
    free(plane);
    free(data);
}

int main(int argc, char **argv) {
    assert(argc == 7);
    expect_header(argv[1], 64, 48, false, false);   // lossy
    expect_header(argv[2], 40, 32, false, false);   // lossless
    expect_header(argv[3], 32, 24, true, false);    // alpha (VP8X)
    expect_header(argv[4], 16, 16, false, true);    // animated
    expect_rgb565(argv[1], 64, 48, true);
    expect_rgb565(argv[2], 40, 32, true);
    expect_rgb565(argv[3], 32, 24, true);
    expect_rgb565(argv[4], 16, 16, false);          // animation is rejected, not half-decoded
    expect_scaled(argv[1]);
    expect_epub(argv[5], "../Images/photo.webp", argv[1]);
    expect_exact_rgb565(argv[6]);
    puts("PASS: WebP header, RGB565 decode, animation rejection, pixel-cap scaling and EPUB image paths");
    return 0;
}
