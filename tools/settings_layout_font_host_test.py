"""Actual settings geometry/navigation and opt-in reading-font synchronization.

SPDX-License-Identifier: Apache-2.0
中文：所有系统字号检查布局/触摸边界，确认弹窗不穿透、不提前修改阅读字体。
English: Check every scale, layout/touch bounds, and modal isolation without eagerly changing the reading face.

中文：设置根页已从「四组行直接铺在主页」改成「分类入口卡 + 各自分组子页」，Metalio 有 7 张
分类卡（多出语音翻页与 AI 快捷键），分组页的行从 SETTINGS_GROUP_CARD_Y 起算。
枚举/宏/函数均按 metalio47 视图抽取，否则会抽到 #else 分支而留下未闭合的 #if。
English: the settings root moved from four inline groups to category cards opening their own group
pages, with seven cards on Metalio (voice page-turn and the AI shortcut added). Group rows start at
SETTINGS_GROUP_CARD_Y. The enum, macros and functions are extracted from the metalio47 view, or the
#else branches would leave an unterminated #if behind.
"""
from pathlib import Path
import re, subprocess, tempfile
from board_source_view import select_source

root=Path(__file__).resolve().parents[1]
s=select_source((root/'main/apps/app_device_settings.c').read_text(encoding='utf-8'),'metalio47')
tokens=r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
scan=re.sub(tokens,lambda m:' '*len(m[0]) if m[0].startswith(('//','/*')) else m[0],s)
def block(start):
    at=scan.index('{',start)+1;depth=1;quote=None;escape=False
    while depth:
        c=scan[at]
        if quote:
            if escape:escape=False
            elif c=='\\':escape=True
            elif c==quote:quote=None
        elif c in '\"\'':quote=c
        elif c=='{':depth+=1
        elif c=='}':depth-=1
        at+=1
    return s[start:at]
def fn(name):
    m=re.search(r'^static [^\n]+\b'+name+r'\([^;{}]*\)\s*\{',s,re.M)
    assert m,name
    return block(m.start())
start=s.index('static app_redraw_t on_gesture(')
modal=block(s.index('    if (s_page==SETTINGS_SYSTEM_FONT && s_font_sync_confirm)',start))
choose=block(s.index('    if (s_page == SETTINGS_SYSTEM_FONT) {',start))
main=s[s.index('    if (y<160 || y>=UI_NAV_TOP || ev->x0<36',start):s.index('\nstatic app_redraw_t on_key(',start)].rstrip()
assert main.endswith('}')
main=main[:-1]
enum=s[s.index('typedef enum { SETTINGS_MAIN'):s.index('} settings_page_t;')+len('} settings_page_t;')]
# 分类卡取代了原来的四组行；MAINTENANCE_* 已被 SYSTEM_ROWS / CATEGORY_* 取代。
# Category cards replaced the four inline groups; MAINTENANCE_* became SYSTEM_ROWS / CATEGORY_*.
macros='\n'.join(re.findall(r'^#define SETTINGS_(?:ROW_H|WIRELESS_ROWS|DISPLAY_ROWS|POWER_ROWS|PERSONAL_ROWS|SYSTEM_ROWS|CATEGORY_ROWS|WIRELESS_Y|CATEGORY_Y|GROUP_CARD_Y|SCROLL_MAX)[^\n]*',s,re.M))
layout=s[s.index('typedef struct {\n    EpdRect size, preview;'):s.index('static system_font_layout_t system_font_layout(')]
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct{int x,y,width,height;}EpdRect;
typedef enum{APP_REDRAW_NONE,APP_REDRAW_PAGE,APP_REDRAW_AREA}app_redraw_t;
typedef struct{int type,x0,y0;}ui_gesture_event_t;
enum{UI_GESTURE_TAP,UI_GESTURE_SWIPE_U};
typedef struct{int unused;}app_desc_t;
typedef struct{const app_desc_t*request_app;}app_ctx_t;
const app_desc_t app_transfer={0};
typedef struct{int unused;}ble_pt_raw_t;
typedef struct{const char *path;}ttf_font_item_t;
static const ttf_font_item_t fonts[]={{"/sdcard/fonts/A.ttf"},{"/sdcard/fonts/B.ttf"},{"/sdcard/fonts/C.ttf"}};
static int ttf_font_count(void){return 3;}
static const ttf_font_item_t*ttf_font_item(int i){return i>=0&&i<3?&fonts[i]:NULL;}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
static uint8_t scale=100;
static bool write_fail;
static uint8_t app_settings_system_font_size(void){return scale;}
static void app_settings_set_system_font_size(uint8_t n){if(!write_fail)scale=n;}
static char system_path[192]="/sdcard/fonts/Old.ttf",reader_path[192]="/sdcard/fonts/Reader.ttf";
static unsigned reader_writes,system_activations;
static const char*app_settings_system_font_path(void){return system_path;}
static const char*app_settings_font_path(void){return reader_path;}
static void app_settings_set_system_font_path(const char*p){if(!write_fail)snprintf(system_path,sizeof(system_path),"%s",p);}
static void app_settings_set_font_path(const char*p){++reader_writes;if(!write_fail)snprintf(reader_path,sizeof(reader_path),"%s",p);}
static void app_font_activate_system(void){++system_activations;}
static void ui_text_set_system_scale(bool b){assert(b);}
static int s_font_page,s_main_scroll,s_style_scroll,s_ble_scroll,s_ble_learning;
static bool s_font_sync_confirm,s_config_confirm,s_boot_pending,s_reset_confirm,s_reset_pending,s_panel_repair_pending;
static bool s_scroll_present_pending;
// 蓝牙页耳机列表展开标记。/ Headset list expansion flag on the Bluetooth page.
static bool s_bt_show;
static char s_reset_notice[96];
// 分组子页新增：内存卡恢复提示、BOOT 提示。/ New group-page notices: card recovery and BOOT.
static char s_sd_notice[96],s_boot_notice[96];
static char s_font_sync_path[192],s_notice[96],s_ble_notice[96],s_ble_feedback[96];
static unsigned font_scans,profile_edits,wifi_requests;
static bool home_full;
static unsigned home_toggles;
static void app_settings_set_home_full_refresh(bool v){home_full=v;++home_toggles;}
static bool app_settings_home_full_refresh(void){return home_full;}
// 电源组新增「浅睡播放音乐」开关行。/ The power group gained a music-through-light-sleep toggle.
static bool music_in_sleep;
static unsigned music_toggles;
static bool app_settings_music_in_sleep(void){return music_in_sleep;}
static void app_settings_set_music_in_sleep(bool v){music_in_sleep=v;++music_toggles;}
// 个性化组新增触摸振动开关行。/ Personalisation gained a haptics toggle.
static bool haptic;
static unsigned haptic_toggles;
static bool metalio_haptic_enabled(void){return haptic;}
static void metalio_haptic_set_enabled(bool v){haptic=v;++haptic_toggles;}
// 蓝牙行进页即停扫描并自动连旧设备。/ Entering the Bluetooth row stops scanning and auto-connects.
static unsigned bt_scan_stops,hp_auto_connects;
static void bt_audio_scan_stop(void){++bt_scan_stops;}
static void hp_auto_connect_on_entry(void){++hp_auto_connects;}
// 根页语音翻页入口：进页先起引擎。/ The root voice entry brings the engine up first.
static unsigned voice_inits;
static int voice_turn_init(void){++voice_inits;return 0;}
static void ttf_font_scan(void){++font_scans;}
static void app_transfer_request_wifi_setup(void){++wifi_requests;}
static bool ble_pt_pop_raw(ble_pt_raw_t*p){(void)p;return false;}
static void profile_editor_open(bool signature){assert(signature);++profile_edits;}
#define UI_NAV_TOP 1096
#define UI_NAV_REFRESH_END 1088
#define UI_LOCK_HEIGHT 1216
'''+enum+'\nstatic settings_page_t s_page;\nstatic settings_page_t s_parent;\n'+macros+'''
static void upgrade_open(void){s_page=SETTINGS_UPGRADE;}
// settings_group_page 必须先于 settings_main_row_height：行高宏要调用它。/ settings_group_page comes first: the row-height macro calls it.
'''+fn('settings_viewport_end')+'\n'+fn('settings_group_page')+'\n'+fn('settings_open')+'\n'+fn('settings_profile_height')+'\n'+fn('settings_main_row_height')+'\n'+layout+fn('system_font_layout')+'\n'+fn('system_font_item_rect')+'\n'+fn('system_font_page_button')+'\n'+fn('system_font_sync_button')+'\n'
unit+='static app_redraw_t font_tap(int x,int y){ui_gesture_event_t e={UI_GESTURE_TAP,x,y};const ui_gesture_event_t*ev=&e;'+modal+choose+'return APP_REDRAW_NONE;}\n'
unit+='static app_redraw_t modal_swipe(void){ui_gesture_event_t e={UI_GESTURE_SWIPE_U,400,700};const ui_gesture_event_t*ev=&e;'+modal+'return APP_REDRAW_NONE;}\n'
unit+='static app_redraw_t main_tap(app_ctx_t*ctx,int x,int y){ui_gesture_event_t e={UI_GESTURE_TAP,x,y};const ui_gesture_event_t*ev=&e;'+main+'}\n'
unit+=r'''
int main(void){
 app_ctx_t ctx={0};
 for(int percent=100;percent<=200;percent+=10){
   scale=percent;s_font_sync_confirm=false;
   system_font_layout_t l=system_font_layout();
   assert(l.size.y+l.size.height<l.preview_title&&l.preview.y+l.preview.height<l.font_title);
   assert(l.font_title<l.first&&l.first+l.slots*l.row_height<settings_viewport_end());
   for(int i=0;i<l.slots;++i){EpdRect r=system_font_item_rect(i);assert(r.y+r.height<settings_viewport_end());if(i)assert(system_font_item_rect(i-1).y+system_font_item_rect(i-1).height<r.y);}
   // 分类卡仍在资料卡之下；分组页最多的系统维护六行也要完整落在底栏之上。
   // Category cards still sit below the profile, and the longest group (six rows) clears the nav bar.
   assert(SETTINGS_CATEGORY_Y>=180+settings_profile_height()+70);
   assert(SETTINGS_GROUP_CARD_Y>=160&&SETTINGS_GROUP_CARD_Y+SETTINGS_SYSTEM_ROWS*82<settings_viewport_end());
   // 五个分组子页的每一行。/ Every row of the five group pages.
   const settings_page_t groups[]={SETTINGS_GROUP_CONNECT,SETTINGS_GROUP_DISPLAY,SETTINGS_GROUP_POWER,
     SETTINGS_GROUP_PERSONAL,SETTINGS_GROUP_SYSTEM};
   const int group_rows[]={SETTINGS_WIRELESS_ROWS,SETTINGS_DISPLAY_ROWS,SETTINGS_POWER_ROWS,
     SETTINGS_PERSONAL_ROWS,SETTINGS_SYSTEM_ROWS};
   // 负数代表不是「开某个页面」的目标：-1 请求传书 app，-2 首页强刷开关，-3 浅睡音乐开关，
   // -4 资料卡，-5 触摸振动开关，-6 屏幕修复。/ Negative codes are non-page destinations.
   const int codes[][6]={
     {-1,SETTINGS_BLUETOOTH,SETTINGS_TIME},
     {SETTINGS_FRONTLIGHT,SETTINGS_SYSTEM_FONT,SETTINGS_SYSTEM_CONTRAST,SETTINGS_MAIN_REFRESH,-2},
     {SETTINGS_AUTO_LOCK,SETTINGS_POWER_SLEEP,-3,SETTINGS_LOCK_STYLE,SETTINGS_LOCK_PIN},
     {SETTINGS_SHELF_STYLE,-4,-5},
     {SETTINGS_UPGRADE,SETTINGS_SD_RECOVERY,SETTINGS_CONFIG,-6,SETTINGS_BOOT,SETTINGS_FACTORY_RESET}};
   for(int g=0;g<5;++g)for(int i=0;i<group_rows[g];++i){
     s_page=groups[g];s_main_scroll=0;ctx.request_app=NULL;s_panel_repair_pending=false;
     unsigned home_before=home_toggles,music_before=music_toggles,haptic_before=haptic_toggles,
              profile_before=profile_edits;
     int center=SETTINGS_GROUP_CARD_Y+i*SETTINGS_ROW_H+SETTINGS_ROW_H/2;
     app_redraw_t redraw=main_tap(&ctx,300,center);
     int code=codes[g][i];
     if(code>=0)assert(redraw==APP_REDRAW_PAGE&&(int)s_page==code);
     else if(code==-1)assert(redraw==APP_REDRAW_NONE&&ctx.request_app==&app_transfer);
     else if(code==-2)assert(redraw==APP_REDRAW_PAGE&&home_toggles==home_before+1&&s_page==SETTINGS_GROUP_DISPLAY);
     else if(code==-3)assert(redraw==APP_REDRAW_PAGE&&music_toggles==music_before+1&&s_page==SETTINGS_GROUP_POWER);
     else if(code==-4)assert(redraw==APP_REDRAW_PAGE&&profile_edits==profile_before+1&&s_page==SETTINGS_GROUP_PERSONAL);
     else if(code==-5)assert(redraw==APP_REDRAW_PAGE&&haptic_toggles==haptic_before+1&&s_page==SETTINGS_GROUP_PERSONAL);
     else assert(redraw==APP_REDRAW_PAGE&&s_panel_repair_pending&&s_page==SETTINGS_GROUP_SYSTEM);
     s_page=groups[g];assert(main_tap(&ctx,10,center)==APP_REDRAW_NONE&&s_page==groups[g]);
   }
   // 根页七张分类卡；-7 是语音翻页（进页先起引擎）。/ Seven root category cards; -7 is voice page-turn.
   const int root_codes[]={SETTINGS_GROUP_CONNECT,SETTINGS_GROUP_DISPLAY,SETTINGS_GROUP_POWER,
     SETTINGS_GROUP_PERSONAL,-7,SETTINGS_GROUP_SYSTEM,SETTINGS_AI_KEY};
   for(int i=0;i<SETTINGS_CATEGORY_ROWS;++i){
     s_page=SETTINGS_MAIN;s_main_scroll=0;ctx.request_app=NULL;unsigned voice_before=voice_inits;
     int center=SETTINGS_CATEGORY_Y+i*SETTINGS_ROW_H+SETTINGS_ROW_H/2;
     app_redraw_t redraw=main_tap(&ctx,300,center);
     if(root_codes[i]>=0)assert(redraw==APP_REDRAW_PAGE&&(int)s_page==root_codes[i]);
     else assert(redraw==APP_REDRAW_PAGE&&voice_inits==voice_before+1&&s_page==SETTINGS_VOICE);
   }
   s_page=SETTINGS_MAIN;s_main_scroll=0;
   assert(main_tap(&ctx,300,180+settings_profile_height()/2)==APP_REDRAW_PAGE&&s_page==SETTINGS_PROFILE);
   // 最后一张分类卡（AI 快捷键）必须完整落在底栏之上并可点。/ The last card (AI shortcut) stays above the nav bar and is tappable.
   s_main_scroll=SETTINGS_SCROLL_MAX;s_page=SETTINGS_MAIN;
   int last=SETTINGS_CATEGORY_Y+(SETTINGS_CATEGORY_ROWS-1)*SETTINGS_ROW_H+SETTINGS_ROW_H-1;
   assert(last<UI_NAV_TOP&&main_tap(&ctx,300,last)==APP_REDRAW_PAGE&&s_page==SETTINGS_AI_KEY);
 }
 scale=100;s_page=SETTINGS_SYSTEM_FONT;s_font_page=0;s_main_scroll=0;s_font_sync_confirm=false;
 for(int i=0;i<11;++i){EpdRect r=system_font_layout().size;assert(font_tap(300,r.y+r.height/2)==APP_REDRAW_PAGE);assert(scale==((i+1)%11)*10+100);}
 EpdRect r=system_font_item_rect(1);
 assert(font_tap(300,r.y+r.height/2)==APP_REDRAW_PAGE&&s_font_sync_confirm);
 assert(!strcmp(system_path,fonts[0].path)&&!strcmp(reader_path,"/sdcard/fonts/Reader.ttf")&&!reader_writes&&system_activations==1);
 assert(modal_swipe()==APP_REDRAW_NONE&&s_font_sync_confirm);
 assert(font_tap(300,220)==APP_REDRAW_NONE&&s_font_sync_confirm&&scale==100);
 assert(font_tap(202,720)==APP_REDRAW_PAGE&&!s_font_sync_confirm&&!reader_writes);
 assert(font_tap(300,r.y+r.height/2)==APP_REDRAW_PAGE&&s_font_sync_confirm);
 write_fail=true;assert(font_tap(481,720)==APP_REDRAW_PAGE&&s_font_sync_confirm&&s_notice[0]&&reader_writes==1);
 write_fail=false;assert(font_tap(481,720)==APP_REDRAW_PAGE&&!s_font_sync_confirm&&reader_writes==2&&!strcmp(reader_path,system_path));
 assert(font_tap(300,r.y+r.height/2)==APP_REDRAW_PAGE&&!s_font_sync_confirm);
 write_fail=true;unsigned before=system_activations;r=system_font_item_rect(2);
 assert(font_tap(300,r.y+r.height/2)==APP_REDRAW_PAGE&&!s_font_sync_confirm&&system_activations==before&&!strcmp(system_path,fonts[0].path));
 write_fail=false;assert(font_tap(300,r.y+r.height+7)==APP_REDRAW_NONE);
 puts("PASS: 11 scales, all 30 group/root destinations incl. AI shortcut and voice, reachable final row, font-size cycle, modal isolation, opt-in sync, no reading-font load and failed-save retry");
}
'''
with tempfile.TemporaryDirectory() as folder:
    c,b=Path(folder)/'test.c',Path(folder)/'test'
    c.write_text(unit)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(b)],check=True)
    subprocess.run([str(b)],check=True)
