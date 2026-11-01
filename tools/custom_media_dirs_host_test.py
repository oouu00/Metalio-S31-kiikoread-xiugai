#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：真实目录选项验证命中、保存通知、失败与取消。/ English: Actual directory options test hits, save notifications, failures and cancellation."""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
path=root/'main/apps/app_files.c';source=path.read_text()
start=source.index('    if(s_view==FILE_VIEW_DIR_OPTIONS){',source.index('static app_redraw_t on_gesture'))
end=source.index('\n    }',start)+6
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
enum {APP_REDRAW_NONE,APP_REDRAW_PAGE,UI_GESTURE_TAP,UI_GESTURE_LONG_PRESS,FILE_VIEW_LIST,FILE_VIEW_MOVE,FILE_VIEW_DIR_OPTIONS};
#define UI_FILES_ROWS 7
static int s_view=FILE_VIEW_DIR_OPTIONS,s_counts[3],book_saves,font_saves,changed,scans;
static char s_dir[288]="/sdcard/books",s_message[96],book_path[288],font_path[288];
static bool save_ok=true,s_file_grid;
typedef struct{int type,x0,y0;}ui_gesture_event_t;
typedef struct{int x,y,width,height;}EpdRect;
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
static bool app_settings_set_books_dir(const char *p){++book_saves;if(save_ok)snprintf(book_path,sizeof(book_path),"%s",p);return save_ok;}
static bool app_settings_set_fonts_dir(const char *p){++font_saves;if(save_ok)snprintf(font_path,sizeof(font_path),"%s",p);return save_ok;}
static void book_store_notify_changed(void){++changed;}
static void ttf_font_scan(void){++scans;}
static int count_root(int n){return n==0?13:7;}
'''
unit+=function('file_action_rect',path)+'\n'+function('visible_rows',path)+'\n'
unit+='static int tap(int type,int x,int y){ui_gesture_event_t event={type,x,y};const ui_gesture_event_t *ev=&event;\n'+source[start:end]+'return APP_REDRAW_NONE;}\n'
unit+=r'''
int main(void){
 assert(visible_rows()==7);s_file_grid=true;assert(visible_rows()==9);
 s_view=FILE_VIEW_MOVE;assert(visible_rows()==7);s_view=FILE_VIEW_DIR_OPTIONS;
 assert(tap(UI_GESTURE_LONG_PRESS,100,500)==APP_REDRAW_NONE&&!book_saves);
 assert(tap(UI_GESTURE_TAP,59,500)==APP_REDRAW_NONE&&s_view==FILE_VIEW_DIR_OPTIONS);
 assert(tap(UI_GESTURE_TAP,624,500)==APP_REDRAW_NONE);
 assert(tap(UI_GESTURE_TAP,100,489)==APP_REDRAW_NONE);
 assert(tap(UI_GESTURE_TAP,100,566)==APP_REDRAW_NONE);
 assert(tap(UI_GESTURE_TAP,60,490)==APP_REDRAW_PAGE&&s_view==FILE_VIEW_LIST&&book_saves==1&&changed==1&&!scans);
 assert(!strcmp(book_path,s_dir)&&strstr(s_message,"已设为书籍目录"));
 s_view=FILE_VIEW_DIR_OPTIONS;
 assert(tap(UI_GESTURE_TAP,623,665)==APP_REDRAW_PAGE&&font_saves==1&&scans==1&&!strcmp(font_path,s_dir));
 assert(s_counts[0]==13&&s_counts[2]==7);
 int saves=book_saves+font_saves;
 s_view=FILE_VIEW_DIR_OPTIONS;assert(tap(UI_GESTURE_TAP,100,700)==APP_REDRAW_PAGE&&s_view==FILE_VIEW_LIST&&saves==book_saves+font_saves);
 snprintf(s_dir,sizeof(s_dir),"/sdcard");s_view=FILE_VIEW_DIR_OPTIONS;
 assert(tap(UI_GESTURE_TAP,100,500)==APP_REDRAW_PAGE&&saves==book_saves+font_saves&&strstr(s_message,"文件夹"));
 snprintf(s_dir,sizeof(s_dir),"/sdcard/library");save_ok=false;int notified=changed+scans;
 for(int y=500;y<=600;y+=100){s_view=FILE_VIEW_DIR_OPTIONS;assert(tap(UI_GESTURE_TAP,100,y)==APP_REDRAW_PAGE&&strstr(s_message,"保存失败")&&changed+scans==notified);}
 puts("PASS: directory option boundaries/cancel/root rejection; books/fonts notify only on saved success; seven list/nine grid/seven move rows");
}
'''
with tempfile.TemporaryDirectory() as folder:
 p=Path(folder);(p/'test.c').write_text(unit)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
