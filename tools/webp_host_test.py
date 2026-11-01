#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""中文：WebP 适配的主机回归；用提交进仓库的小图驱动真实的 libwebp 解码与 EPUB 路径。
English: Host regression for WebP support; drives the real libwebp decoder and the EPUB path with
committed tiny fixtures.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WEBP = ROOT / 'tools' / 'data' / 'webp'
COMPONENT = ROOT / 'components' / 'read_pico_webp'
LIBWEBP = COMPONENT / 'libwebp'

# 与 components/read_pico_webp/CMakeLists.txt 保持一致：只编解码器需要的源码。
# Mirrors components/read_pico_webp/CMakeLists.txt: decoder-only sources.
LIBWEBP_SOURCES = [
    *LIBWEBP.glob('src/dec/*.c'),
    *[LIBWEBP / 'src' / 'dsp' / name for name in (
        'alpha_processing.c', 'cpu.c', 'dec.c', 'dec_clip_tables.c', 'filters.c', 'lossless.c',
        'rescaler.c', 'upsampling.c', 'yuv.c')],
    *[LIBWEBP / 'src' / 'utils' / name for name in (
        'bit_reader_utils.c', 'color_cache_utils.c', 'filters_utils.c', 'huffman_utils.c',
        'palette.c', 'quant_levels_dec_utils.c', 'rescaler_utils.c', 'random_utils.c',
        'thread_utils.c', 'utils.c')],
]
OWN_SOURCES = [
    ROOT / 'tools' / 'webp_host_test.c',
    ROOT / 'main' / 'book' / 'book_epub.c',
    ROOT / 'main' / 'book' / 'zip_reader.c',
    ROOT / 'main' / 'book' / 'html_text.c',
    ROOT / 'main' / 'book' / 'book_index_cache.c',
    ROOT / 'main' / 'book' / 'book_image_header.c',
    COMPONENT / 'src' / 'read_pico_webp.c',
]
INCLUDES = [
    '-I' + str(ROOT / 'tools' / 'book_epub_stubs'),
    '-I' + str(ROOT / 'tools' / 'zip_host_stubs'),
    '-I' + str(ROOT / 'main' / 'book'),
    '-I' + str(COMPONENT / 'include'),
    '-I' + str(LIBWEBP),
]
SANITIZE = ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']


def local_flags() -> list[str]:
    # 本机 Windows 只是方便自查；CI 走 Linux，不需要这层兼容头。
    # Local Windows convenience only; CI is Linux and never needs this compatibility header.
    compat = ROOT / 'build' / 'mingw_compat.h'
    if sys.platform == 'win32' and compat.is_file():
        return ['-D_DEFAULT_SOURCE', '-include', str(compat)]
    return []


def make_epub(path: Path, image: bytes) -> None:
    opf = ('<package><manifest>'
           '<item id="c1" href="../Text/ch1.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="img" href="../Images/photo.webp" media-type="image/webp"/>'
           '<item id="toc" href="../ncx.ncx" media-type="application/x-dtbncx+xml"/>'
           '</manifest><spine toc="toc"><itemref idref="c1"/></spine></package>')
    chapter = ('<html><body><h1>第1卷</h1><p>正文</p>'
               '<img src="../Images/photo.webp"/></body></html>')
    ncx = ('<ncx><navMap><navPoint><navLabel><text>第1卷</text></navLabel>'
           '<content src="../Text/ch1.xhtml"/></navPoint></navMap></ncx>')
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('mimetype', 'application/epub+zip')
        archive.writestr('META-INF/container.xml',
                         '<container><rootfiles><rootfile full-path="OPS/pkg/book.opf" '
                         'media-type="application/oebps-package+xml"/></rootfiles></container>')
        archive.writestr('OPS/pkg/book.opf', opf)
        archive.writestr('OPS/Text/ch1.xhtml', chapter)
        archive.writestr('OPS/ncx.ncx', ncx)
        archive.writestr('OPS/Images/photo.webp', image)


def main() -> int:
    fixtures = {
        'lossy': WEBP / 'lossy.webp',
        'lossless': WEBP / 'lossless.webp',
        'alpha': WEBP / 'alpha.webp',
        'animated': WEBP / 'animated.webp',
        'quads': WEBP / 'quads.webp',
    }
    for name, path in fixtures.items():
        if not path.is_file():
            raise SystemExit(f'missing fixture: {path}')

    common = ['cc', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', *SANITIZE, *local_flags()]
    with tempfile.TemporaryDirectory(prefix='kiiko-webp-host-') as directory:
        work = Path(directory)
        objects = []
        # 上游源码单独编：体积/告警不参与本工程的 -Werror 口径。
        # Upstream builds separately: its warnings never join this project's -Werror policy.
        for index, source in enumerate(LIBWEBP_SOURCES):
            obj = work / f'libwebp{index:02d}.o'
            # 主机是 x86，libwebp 会拉 SSE2 实现；本仓库只带通用 C 实现，所以关掉 SIMD 检测。
            # The host is x86 and libwebp would pull SSE2 paths; only the generic C sources are
            # vendored, so disable its SIMD detection.
            subprocess.run(['cc', '-std=c11', '-O2', '-w', '-U__SSE2__', '-U__SSSE3__',
                            '-U__SSE4_1__', '-U__AVX2__', '-U__ARM_NEON__', '-U__aarch64__', *SANITIZE, *INCLUDES,
                            '-c', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        exe = work / 'webp-test'
        subprocess.run([*common, '-Werror', *INCLUDES, *map(str, OWN_SOURCES), *objects,
                        '-lz', '-lm', '-o', str(exe)], check=True)

        epub = work / 'webp.epub'
        make_epub(epub, fixtures['lossy'].read_bytes())
        subprocess.run([str(exe), str(fixtures['lossy']), str(fixtures['lossless']),
                        str(fixtures['alpha']), str(fixtures['animated']), str(epub),
                        str(fixtures['quads'])],
                       check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
