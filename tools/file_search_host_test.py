#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：用真实目录验证增量递归查询与句柄清理。/ English: Real recursive queries, bounded slices and handle cleanup."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UNIT = r'''
#include <assert.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "file_search.h"
static unsigned handles, matches;
DIR *search_test_opendir(const char *path){DIR *d=opendir(path);if(d)++handles;return d;}
int search_test_closedir(DIR *d){assert(handles);--handles;return closedir(d);}
static void found(const char *path,const struct stat *info,void *context){
    assert(context==&matches&&path&&(S_ISREG(info->st_mode)||S_ISDIR(info->st_mode)));++matches;
}
static void query(const char *root,const char *text,unsigned expected,unsigned errors){
    matches=0;file_search_t *s=file_search_begin(root,text,found,&matches);assert(s&&handles==1);
    file_search_step(s,0);assert(!file_search_stats(s).visited&&!matches);
    unsigned iterations=0;
    while(!file_search_stats(s).done){
        unsigned before=file_search_stats(s).visited;
        file_search_step(s,1);assert(file_search_stats(s).visited<=before+1);
        assert(++iterations<10000);
    }
    file_search_stats_t result=file_search_stats(s);
    assert(matches==expected&&result.matched==expected&&result.errors==errors&&!handles);
    file_search_step(s,100);assert(file_search_stats(s).matched==expected);
    file_search_close(s);assert(!handles);
}
int main(int argc,char **argv){
    assert(argc==3);const char *root=argv[1];
    query(root,"needle",650,0);query(root,"lvtu",2,0);query(root,"ltbj",2,0);
    query(root,"旅途",2,0);query(root,"absent",0,0);query(root,"hidden",2,0);
    for(unsigned i=0;i<80;++i){
        matches=0;file_search_t *s=file_search_begin(root,"needle",found,&matches);assert(s);
        file_search_step(s,i);file_search_close(s);assert(!handles);
    }
    query(argv[2],"deep-book",0,1);
    char long_query[67];memset(long_query,'x',66);long_query[66]=0;
    assert(!file_search_begin(root,long_query,found,&matches));
    assert(!file_search_begin("/this/folder/does/not/exist","x",found,&matches));
    assert(!file_search_begin(root,"",found,&matches));assert(!handles);
    file_search_close(NULL);
    puts("PASS: real multilevel search beyond512 matches; pinyin/initials/Chinese, duplicate names, hidden files, symlink isolation, one-entry slices, cancellation handles and depth errors");
}
'''

with tempfile.TemporaryDirectory(prefix='kiiko-recursive-search-') as temporary:
    work = Path(temporary)
    root, deep = work / 'card', work / 'deep'
    root.mkdir()
    deep.mkdir()
    for index in range(650):
        parent = root / f'shelf{index % 13}' / 'nested' / f'series{index % 7}'
        parent.mkdir(parents=True, exist_ok=True)
        (parent / f'needle-{index}.epub').touch()
    for parent in [root, root / 'shelf0/nested']:
        (parent / '旅途笔记.EPUB').touch()
    (root / '._needle-ignore').touch()
    (root / '.hidden').mkdir()
    (root / '.hidden/hidden.txt').touch()
    (root / 'cycle').symlink_to(root, target_is_directory=True)
    (root / 'escape').symlink_to(work, target_is_directory=True)
    nested = deep
    for _ in range(35):
        nested /= 'd'
        nested.mkdir()
    (nested / 'deep-book.epub').touch()
    (work / 'test.c').write_text(UNIT)
    flags = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g',
             '-fsanitize=address,undefined', '-I'+str(ROOT/'main/apps'),
             '-I'+str(ROOT/'components/read_pico_search/include')]
    subprocess.run(flags + ['-Dopendir=search_test_opendir', '-Dclosedir=search_test_closedir',
                           '-c', str(ROOT/'main/apps/file_search.c'), '-o', str(work/'search.o')], check=True)
    subprocess.run(flags + [str(work/'test.c'), str(work/'search.o'),
                           str(ROOT/'components/read_pico_search/read_pico_search.c'),
                           '-o', str(work/'test')], check=True)
    subprocess.run([str(work/'test'), str(root), str(deep)], check=True)
