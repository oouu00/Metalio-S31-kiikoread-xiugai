#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""真实只读文案与printf宏：完整目录、用户内容、数值/求值及并发。 / Actual locale, data protection and formatting."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];component=root/'components/kiiko_locale'
u=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
static char user_name[64]="书架",other_user[96]="名称";
bool ui_locale_test_readonly(const void *p);
'''.replace('bool ui_locale_test_readonly','#include <stdbool.h>\nbool ui_locale_test_readonly')
u+='#include "'+str(component/'ui_locale.c')+'"\n'
u+=r'''
bool ui_locale_test_readonly(const void *p){uintptr_t x=(uintptr_t)p;return !(x>=(uintptr_t)user_name&&x<(uintptr_t)user_name+sizeof(user_name))&&!(x>=(uintptr_t)other_user&&x<(uintptr_t)other_user+sizeof(other_user));}
#include "ui_locale_format.h"
static void *toggle(void *unused){(void)unused;for(unsigned i=0;i<50000;++i)ui_locale_set_english(i&1);return NULL;}
static void *read_ui(void *unused){(void)unused;for(unsigned i=0;i<50000;++i){const char *s=ui_tr("书架");assert(!strcmp(s,"书架")||!strcmp(s,"Bookshelf"));assert(ui_tr(user_name)==user_name);}return NULL;}
int main(void){
 char out[200];ui_locale_set_english(false);assert(!strcmp(ui_tr("书架"),"书架"));snprintf(out,sizeof(out),"已读 %u%%",15u);assert(!strcmp(out,"已读 15%"));
 ui_locale_set_english(true);assert(!strcmp(ui_tr("书架"),"Bookshelf"));assert(ui_tr(user_name)==user_name);assert(ui_tr(other_user)==other_user);
 assert(ui_tr(NULL)==NULL);assert(!strcmp(ui_tr("unchanged"),"unchanged"));
 for(size_t i=0;i<sizeof(s_pairs)/sizeof(s_pairs[0]);++i){assert(!strcmp(ui_tr(s_pairs[i].zh),s_pairs[i].en));if(i)assert(strcmp(s_pairs[i-1].zh,s_pairs[i].zh)<0);}
 snprintf(out,sizeof(out),"%s · %u", "书架",42u);assert(!strcmp(out,"Bookshelf · 42"));
 snprintf(out,sizeof(out),"%s · %s",user_name,"名称");assert(!strcmp(out,"书架 · Name"));
 snprintf(out,sizeof(out),"已读 %u%%",15u);assert(!strcmp(out,"Read 15%"));
 snprintf(out,sizeof(out),"%llu / %.2f / %.*s",18446744073709551615ULL,1.25,3,"hello");assert(!strcmp(out,"18446744073709551615 / 1.25 / hel"));
 unsigned ip[4]={192,168,31,238};
#define FOUR_IP(a) (a)[0],(a)[1],(a)[2],(a)[3]
 snprintf(out,sizeof(out),"http://%u.%u.%u.%u",FOUR_IP(ip));assert(!strcmp(out,"http://192.168.31.238"));
 snprintf(out,sizeof(out),"100%%");assert(!strcmp(out,"100%"));
 snprintf(out,sizeof(out),"plain");assert(!strcmp(out,"plain"));
 const char *choices[]={"保存","取消"};unsigned n=0;snprintf(out,sizeof(out),"%s",choices[n++]);assert(n==1&&!strcmp(out,"Save"));
 strcpy(out,"书架");assert(!strcmp(out,"书架"));strcpy(out,user_name);assert(!strcmp(out,"书架"));
 snprintf(out,sizeof(out),"%s %s %s %s %u %d %ld %zu %.1f %c %x %s","保存","取消",user_name,"名称",1u,2,3L,(size_t)4,5.0,'A',0x12,"z");assert(strstr(out,"Save Cancel 书架 Name 1 2 3 4 5.0 A 12 z"));
 pthread_t a,b;assert(!pthread_create(&a,NULL,toggle,NULL)&&!pthread_create(&b,NULL,read_ui,NULL));assert(!pthread_join(a,NULL)&&!pthread_join(b,NULL));
 puts("PASS: full catalog, readonly-only lookup, RAM titles/SSID/input preserved, zero/12 printf arguments, uint64/double/precision, single evaluation and concurrent language access");
}
'''
with tempfile.TemporaryDirectory() as folder:
 p=Path(folder);(p/'test.c').write_text(u);exe=p/'test'
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-DKIIKO_LOCALE_TEST=1','-fsanitize=address,undefined','-pthread','-I'+str(component/'include'),str(p/'test.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
