#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：实际目录绘制/命中/筛选；可导出684×1216几何预览。/ English: Actual directory paint, hits and filtering; optional native-size geometry previews."""
from pathlib import Path
import subprocess,tempfile,sys,re
from home_cover_cache_host_test import function
R=Path(__file__).resolve().parents[1];S=R/'main/apps/app_files.c'
u=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "ui_icons.h"
#include "epdiy.h"
#include "ui_files_layout.h"
#include "read_pico_search.h"
#define UI_GRAY_WHITE 255
#define UI_GRAY_BLACK 0
#define UI_ROOT_GRAY 240
#define EPD_DRAW_ALIGN_LEFT 0
#define EPD_DRAW_ALIGN_CENTER 1
#define EPD_DRAW_ALIGN_RIGHT 2
#define UI_SYSTEM_KEYBOARD_TOP 560
#define UI_FILES_FOOTER_TOP 1096
#define UI_BTN_RADIUS 18
#define UI_GRAY_LIGHT 192
#define UI_FILES_ROWS_NATIVE UI_FILES_ROWS
#define UI_LOCK_HEIGHT 1216
typedef struct {int unused;} app_ctx_t;
typedef struct{char path[288];long size;long long modified;bool is_dir,selected;} file_item_t;
typedef struct{bool mounted;unsigned long long filesystem_bytes,free_bytes;}sd_t;
enum{FILE_VIEW_LIST,FILE_VIEW_ACTIONS,FILE_VIEW_RENAME,FILE_VIEW_MOVE,FILE_VIEW_SORT,FILE_VIEW_BATCH_DELETE,FILE_VIEW_DIR_OPTIONS,FILE_VIEW_SEARCH};
typedef int file_view_t;
static file_item_t entries[30],*s_files=entries;
static int s_view,s_count=30,s_folder=3,s_page,s_file_sort,s_counts[3]={12,3,4};
static void file_count_label(int folder,char *out,size_t size){snprintf(out,size,"%d %s",s_counts[folder],folder==0?"本":folder==1?"张":"款");}
static bool s_file_grid,s_file_selecting,s_file_desc,s_sd_format_confirm,logging;
static char s_search_root[288]="/sdcard";
static char s_file_query[65],s_dir[288]="/sdcard",s_message[96],s_sd_notice[64];
static int s_editor_input;
static sd_t s_sd={true,32000000000ULL,20000000000ULL};
static const char *names[]={"书籍","壁纸","Fonts","TF 卡根目录"};
static void box(EpdRect r){assert(r.x>=0&&r.y>=0&&r.x+r.width<=684&&r.y+r.height<=1216&&r.width>=0&&r.height>=0);}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.width&&y<r.y+r.height;}
static void ui_fill_rect_fast(EpdRect r,int c,uint8_t*f){(void)f;box(r);if(logging)printf("R %d %d %d %d %d\n",r.x,r.y,r.width,r.height,c);}
static void ui_fill_round_rect(uint8_t*f,EpdRect r,int radius,int c){(void)f;box(r);if(logging)printf("F %d %d %d %d %d %d\n",r.x,r.y,r.width,r.height,radius,c);}
static void ui_draw_control_frame(uint8_t*f,EpdRect r,int radius,int c){(void)f;box(r);if(logging)printf("B %d %d %d %d %d %d\n",r.x,r.y,r.width,r.height,radius,c);}
static void ui_hairline(uint8_t*f,int y,int x,int w,int c){ui_fill_rect_fast((EpdRect){x,y,w,1},c,f);}
static void ui_draw_separator(uint8_t*f,int y,int x,int w,int c){ui_fill_rect_fast((EpdRect){x,y,w,2},c,f);}
static void ui_draw_icon(uint8_t*f,int x,int y,int px,int icon,int c){(void)f;assert(icon>=0&&icon<UI_ICON_COUNT);box((EpdRect){x-px/2,y-px/2,px,px});if(logging)printf("I %d %d %d %d %d\n",x,y,px,icon,c);}
static void ui_text_box(uint8_t*f,EpdRect r,int px,const char*t,int align,bool inverted){(void)f;box(r);assert(px>0);if(logging)printf("T %d %d %d %d %d %d %d\t%s\n",r.x,r.y,r.width,r.height,px,align,inverted,t);}
static void ui_clear_page(uint8_t*f){ui_fill_rect_fast((EpdRect){0,0,684,1216},255,f);}
static void ui_nav_status(uint8_t*f){ui_text_box(f,(EpdRect){36,12,612,56},22,"10:30                          Wi-Fi  80%",0,false);}
static void ui_nav_back(uint8_t*f,int x,int y){ui_draw_icon(f,x+28,y+33,44,UI_ICON_CHEVRON_LEFT,48);}
static void ui_nav_draw(uint8_t*f,int tab){(void)tab;ui_fill_rect_fast((EpdRect){0,1096,684,120},224,f);const char*n[]={"首页","书架","文件","设置"};for(int i=0;i<4;++i){ui_draw_icon(f,85+i*171,1141,44,(int[]){UI_ICON_HOUSE,UI_ICON_LIBRARY_BIG,UI_ICON_FOLDER_OPEN,UI_ICON_SLIDERS_HORIZONTAL}[i],0);ui_text_box(f,(EpdRect){i*171,1170,171,30},17,n[i],1,false);}}
static void epd_draw_circle(int x,int y,int radius,int c,uint8_t*f){ui_draw_control_frame(f,(EpdRect){x-radius,y-radius,radius*2,radius*2},radius,c);}
static void epd_fill_circle(int x,int y,int radius,int c,uint8_t*f){ui_fill_round_rect(f,(EpdRect){x-radius,y-radius,radius*2,radius*2},radius,c);}
static bool main_page_visible(app_ctx_t*c){(void)c;return s_folder<0&&s_view==FILE_VIEW_LIST&&!s_file_selecting;}
static void render_file_sort(uint8_t*f){(void)f;}
static void render_file_batch_confirmation(uint8_t*f){(void)f;}
static void render_actions(uint8_t*f){(void)f;}
static void render_rename(uint8_t*f){(void)f;}
static void ui_text_input_draw(uint8_t*f,int*i,EpdRect r,int px,bool b,const char*t){(void)i;(void)t;ui_draw_control_frame(f,r,16,128);ui_text_box(f,r,px,"示例搜索",0,b);}
static void ui_keyboard_draw(uint8_t*f,int y){ui_fill_rect_fast((EpdRect){0,y,684,1096-y},240,f);}
static void ui_draw_button(uint8_t*f,EpdRect r,const char*t,bool b){ui_draw_control_frame(f,r,18,64);ui_text_box(f,r,24,t,1,b);}
static bool sd_needs_format(void){return false;}
static bool storage_space_valid(const sd_t*s){return s->mounted;}
static void storage_amount(char*t,size_t n,unsigned long long b){snprintf(t,n,"%.1f GB",b/1e9);}
static void storage_usage(const sd_t*s,char*t,size_t n,char*u,size_t k){storage_amount(t,n,s->filesystem_bytes);snprintf(u,k,"已用 12.0 GB");}
static void copy_utf8(char*t,size_t n,const char*s){snprintf(t,n,"%s",s);}
static void fit_name(char*t,int px,int w){(void)t;(void)px;(void)w;}
static void file_display_name(const file_item_t*i,char*n,char*a){const char*p=strrchr(i->path,'/');snprintf(n,128,"%s",p?p+1:i->path);a[0]=0;}
static void file_detail(char*t,size_t n,const file_item_t*i){snprintf(t,n,"%s",i->is_dir?"文件夹":"EPUB · 1.2 MB");}
#define FILE_ICON_PX 46
static int file_pager_top(void){return 1006;}
static EpdRect file_action_rect(int i){return (EpdRect){i?355:36,875,293,76};}
static void file_card_border(uint8_t*f,EpdRect r,int radius){ui_draw_control_frame(f,r,radius,80);}
static void file_management_button(uint8_t*f,EpdRect r,const char*t,bool b){ui_draw_control_frame(f,r,18,128);ui_text_box(f,r,26,t,1,b);}
'''
for n,p in [(n,S) for n in ['file_item_name','file_item_icon','file_icon','file_selected_count','file_matches','file_visible_count','file_visible_index','file_entry_rect','file_entry_hit','file_search_rect','file_layout_rect','file_select_rect','file_sort_button','visible_rows','root_rows','file_pager_pages','home_transfer_rect','storage_card_rect']]+[(n,R/'main/ui/ui_root_style.h') for n in ['ui_root_background','ui_root_shadow','ui_root_card']]:
 u+=function(n,p).replace('static inline','static')+'\n'
u+='static void draw_file_pager(uint8_t*f){char p[40];snprintf(p,sizeof(p),"%d / %d",s_page+1,file_pager_pages());ui_text_box(f,(EpdRect){240,1006,204,60},22,p,1,false);}\n'
u+=function('files_navigation',S)+'\nstatic void render(app_ctx_t*,uint8_t*);\n'+function('render_content',S)+'\nstatic void render(app_ctx_t*c,uint8_t*f){render_content(c,f);}\n'
u+=r'''
int main(int argc,char**argv){for(int i=0;i<30;++i){snprintf(entries[i].path,288,"/sdcard/%s-%02d.epub",i%2?"旅途笔记":"阅读记录",i);entries[i].is_dir=i==0;}
 const char *types[]={"Book.EPUB","read.txt","a.pdf","a.mobi","a.azw3","a.fb2","A.TTF","a.otf","a.ttc","a.woff","a.woff2","P.JPG","a.jpeg","a.png","a.gif","a.bmp","a.webp","a.svg","a.zip","noext"};
 for(unsigned i=0;i<sizeof(types)/sizeof(*types);++i){file_item_t f={0};snprintf(f.path,sizeof(f.path),"/sdcard/%s",types[i]);assert(file_item_icon(&f)==(i<6?UI_ICON_BOOK_OPEN:i<11?UI_ICON_TYPE:i<18?UI_ICON_FILE_IMAGE:UI_ICON_FILE));f.is_dir=true;assert(file_item_icon(&f)==UI_ICON_FOLDER);}
 assert(file_visible_count()==30);entries[23].selected=true;strcpy(s_file_query,"lvtu");assert(file_visible_count()==15);assert(file_visible_index(0)==1&&file_visible_index(14)==29&&file_visible_index(15)==-1&&entries[23].selected);
 for(int g=0;g<2;++g){s_file_grid=g;assert(visible_rows()==(g?9:7));assert(file_pager_pages()==(g?2:3));for(int i=0;i<visible_rows();++i){EpdRect r=file_entry_rect(i);box(r);assert(r.y+r.height<1006);assert(file_entry_hit(r.x+r.width/2,r.y+r.height/2)==i);}}
 s_file_query[0]=0;assert(file_entry_hit(10,400)==-1&&file_entry_hit(340,1100)==-1);s_view=FILE_VIEW_MOVE;strcpy(s_file_query,"absent");assert(file_visible_count()==30);s_file_query[0]=0;s_view=FILE_VIEW_LIST;
 assert(!ui_rect_hit(file_layout_rect(),file_search_rect().x,file_search_rect().y));
 logging=argc>1;if(logging){s_file_grid=!strcmp(argv[1],"grid");if(s_file_grid){const char *preview[]={"古典文学","山海经.epub","读书笔记.txt","思源黑体.ttf","霞鹜文楷.otf","插图.png","封面.jpg","配置.json","资料.pdf"};for(int i=0;i<9;++i)snprintf(entries[i].path,288,"/sdcard/%s",preview[i]);}if(!strcmp(argv[1],"hub"))s_folder=-1;else if(!strcmp(argv[1],"selection"))s_file_selecting=true;app_ctx_t c={0};uint8_t f=0;render(&c,&f);}else puts("PASS: actual filtered indices retain selected identities; native list/grid cells and hit regions agree; pager tracks visible results; move ignores search filters");
}
'''
with tempfile.TemporaryDirectory(prefix='files-view-') as d:
 w=Path(d);(w/'test.c').write_text(u);(w/'epdiy.h').write_text('#pragma once\ntypedef struct{int x,y,width,height;}EpdRect;\n')
 exe=w/'test';subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-I'+str(w),'-I'+str(R/'main/assets'),'-I'+str(R/'main/ui'),'-I'+str(R/'components/read_pico_search/include'),str(w/'test.c'),str(R/'components/read_pico_search/read_pico_search.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
 if len(sys.argv)>1:
  out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
  for mode in ['hub','list','grid','selection']:
   paint=subprocess.check_output([str(exe),mode]);(out/(mode+'.paint')).write_bytes(paint)
   if mode=='grid':
    for row in paint.decode().splitlines():
     parts=row.split()
     if parts[0] in ('F','B'):
      y,width,height=map(int,(parts[2],parts[3],parts[4]));assert not (302<=y<984 and width>100 and height>100),row
