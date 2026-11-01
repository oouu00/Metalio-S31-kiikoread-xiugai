#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Actual file-manager counts with the sliced directory walker; no list/result cap."""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
R=Path(__file__).resolve().parents[1]
u=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "file_search.h"
static file_search_t *s_book_count_search,*s_file_search;
static int s_book_count_total,s_counts[3];
static bool s_book_count_dirty,s_book_count_incomplete;
static struct{bool mounted;}s_sd={true};
static const char *root;
static const char *folder_root(int folder){(void)folder;return root;}
'''
for name in ['supported','file_book_count_invalidate','file_book_count_visit','file_book_count_tick','file_count_label','count_root','file_search_stop']:
    u+=function(name,R/'main/apps/app_files.c')+'\n'
u+=r'''
static void finish(void){unsigned steps=0;while(!file_book_count_tick())assert(++steps<10000);assert(!s_book_count_search&&!s_book_count_dirty);}
int main(int argc,char **argv){
 assert(argc==3);root=argv[1];char label[48];s_counts[0]=999;
 assert(count_root(0)==999&&s_book_count_dirty);file_count_label(0,label,sizeof(label));assert(!strcmp(label,"统计中"));
 finish();assert(s_counts[0]==653&&!s_book_count_incomplete);file_count_label(0,label,sizeof(label));assert(!strcmp(label,"653 本"));
 assert(count_root(0)==653);assert(!file_book_count_tick()&&s_book_count_search);file_search_stop();assert(!s_book_count_search&&s_book_count_dirty);
 finish();assert(s_counts[0]==653);
 char path[512];snprintf(path,sizeof(path),"%s/root.TXT",root);assert(!unlink(path));count_root(0);finish();assert(s_counts[0]==652);
 s_sd.mounted=false;assert(count_root(0)==0);s_sd.mounted=true;
 root=argv[2];count_root(0);finish();assert(s_counts[0]==0&&s_book_count_incomplete);file_count_label(0,label,sizeof(label));assert(!strcmp(label,"0+ 本"));
 root="/does/not/exist";count_root(0);finish();assert(s_counts[0]==0&&s_book_count_incomplete);
 file_search_stop();assert(!s_book_count_search);
 puts("PASS: recursive653 books across multiple levels and beyond96/512 caps; regular TXT/EPUB only, hidden/AppleDouble/symlink/folder exclusion, sliced completion/cancellation/recount, removed card and explicit incomplete-depth/read errors");
}
'''
with tempfile.TemporaryDirectory(prefix='book-count-') as d:
 p=Path(d);root=p/'card';root.mkdir();deep=p/'deep';deep.mkdir()
 for i in range(650):
  parent=root/str(i%7)/'second'/'third';parent.mkdir(parents=True,exist_ok=True);(parent/f'{i}.epub').touch()
 for rel in ['root.TXT','0/second/同名.EPUB','1/second/同名.EPUB','font.ttf','picture.png','._ghost.txt','.hidden.txt']:(root/rel).touch()
 (root/'folder.epub').mkdir();(root/'.hidden').mkdir();(root/'.hidden/secret.epub').touch();(root/'cycle').symlink_to(root,target_is_directory=True)
 nested=deep
 for _ in range(35):nested=nested/'d';nested.mkdir()
 (nested/'too-deep.epub').touch();(p/'test.c').write_text(u)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(R/'main/apps'),'-I'+str(R/'components/read_pico_search/include'),str(p/'test.c'),str(R/'main/apps/file_search.c'),str(R/'components/read_pico_search/read_pico_search.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(root),str(deep)],check=True)
