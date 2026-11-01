#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：真实引导统一出口门禁。/ English: Actual setup global-exit guard."""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
R=Path(__file__).resolve().parents[1]
u=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
typedef struct {bool setup_flow;} app_desc_t;
typedef struct {bool request_return,request_menu;const app_desc_t *request_app;} app_ctx_t;
static bool pending;
static bool app_setup_pending(void){return pending;}
'''+function('setup_navigation_guard',R/'main/app/app_loop.c')+r'''
int main(void){app_desc_t guide={true},child={true},home={false};
 for(int p=0;p<2;++p)for(int t=0;t<3;++t)for(int flags=0;flags<4;++flags){
  pending=p;app_ctx_t c={flags&1,flags&2,t==0?NULL:t==1?&home:&child};
  setup_navigation_guard(&c,&guide);assert(!c.request_return&&!c.request_menu);
  assert(c.request_app==(t==0||t==1&&p?NULL:t==1?&home:&child));
 }
 app_ctx_t c={true,true,&home};pending=true;setup_navigation_guard(&c,&home);
 assert(c.request_return&&c.request_menu&&c.request_app==&home);
 puts("PASS: real setup guard blocks menu/return/home while Pending, retains guide children, permits only persisted completion, and leaves normal pages unchanged");}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(u);subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-logical-op-parentheses','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
