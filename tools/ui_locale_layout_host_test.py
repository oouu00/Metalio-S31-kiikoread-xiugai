#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""English uses the actual reader-sheet fitter at every existing font/scale case.
中文：英文运行真实阅读面板测量/绘制夹具，保留原中文回归。
"""
from pathlib import Path
import contextlib,io,runpy,subprocess,tempfile
root=Path(__file__).resolve().parents[1];component=root/'components/kiiko_locale'
with contextlib.redirect_stdout(io.StringIO()):ctx=runpy.run_path(str(root/'tools/reader_ui_scale_host_test.py'))
unit=ctx['unit'];prefix='''#include <stdbool.h>
static bool ui_locale_test_readonly(const void *p){(void)p;return true;}
'''
prefix+='#include "'+str(component/'ui_locale.c')+'"\n#include "ui_locale_format.h"\n'
# The callback is a storage-boundary stub; the separate locale test verifies mutable-user buffers.
prefix=prefix.replace('static bool ui_locale_test_readonly','bool ui_locale_test_readonly')
unit=prefix+unit.replace('int main(void){uint8_t fb[1]={0};','int main(void){ui_locale_set_english(true);uint8_t fb[1]={0};')
with tempfile.TemporaryDirectory() as folder:
 p=Path(folder);(p/'test.c').write_text(unit);exe=p/'test'
 for board in ctx['boards']:
  subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',f'-DKIIKO_BOARD_METALIO47={board}','-DKIIKO_UI_LOCALIZATION=1','-DKIIKO_LOCALE_TEST=1','-fsanitize=address,undefined','-I'+str(component/'include'),str(p/'test.c'),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
print('PASS: real English sheet typography at100–200%, three font metrics, wrapped controls and preserved glyph/slot state')
