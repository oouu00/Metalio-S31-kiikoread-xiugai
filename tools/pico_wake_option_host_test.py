#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：真实唤醒选项、设置命中与深睡开机门。/ English: Actual wake preferences, setting hits and the deep-boot gate."""
from pathlib import Path
import subprocess,tempfile,re
from home_cover_cache_host_test import function
ROOT=Path(__file__).resolve().parents[1]
settings=ROOT/'main/settings.c'
ui=(ROOT/'main/apps/app_device_settings.c').read_text()
boot_path=ROOT/'boards/pico/src/main/app_main.c'
if not boot_path.exists(): boot_path=ROOT/'main/app_main.c'
boot=boot_path.read_text()
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define NVS_KEY_WAKE_PRESSES "wake_press"
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
static uint8_t stored;static unsigned writes,gate,ready,held;
static bool pin,boot_press;
static void nvs_put_u8(const char *key,uint8_t value){assert(!strcmp(key,NVS_KEY_WAKE_PRESSES));stored=value;++writes;}
'''
unit+=re.search(r'^static uint8_t s_wake_presses = [12];',settings.read_text(),re.M)[0]+'\n'
for name in ('app_settings_wake_presses','app_settings_set_wake_presses'):
    unit+=function(name,settings)+'\n'
unit+=r'''
#include "ui_nav_layout.h"
#define KIIKO_BOARD_METALIO47 0
typedef struct {int x,y,width,height;}EpdRect;
typedef enum {SETTINGS_MAIN,SETTINGS_POWER_SLEEP,SETTINGS_AUTO_LOCK,SETTINGS_SYSTEM_FONT,
 SETTINGS_SYSTEM_CONTRAST,SETTINGS_MAIN_REFRESH,SETTINGS_FRONTLIGHT,
 SETTINGS_GROUP_DISPLAY,SETTINGS_GROUP_REFRESH,SETTINGS_GROUP_POWER,
 APP_REDRAW_PAGE,APP_REDRAW_NONE} settings_page_t;
static int s_page=SETTINGS_MAIN,s_font_page;static char s_notice[96];
static settings_page_t s_parent=SETTINGS_MAIN;
static bool home_full;
static void ttf_font_scan(void){}
static bool app_settings_home_full_refresh(void){return home_full;}
static void app_settings_set_home_full_refresh(bool on){home_full=on;}
'''
for name in ('settings_profile_height','settings_main_row_height','settings_open'):
    unit+=function(name,ROOT/'main/apps/app_device_settings.c')+'\n'
a=ui.index('#define SETTINGS_ROW_H');b=ui.index('// 设置行图标',a)
unit+=ui[a:b]
# 二级分组页：原显示区拆成显示/刷新/电源三张二级卡片，切片随新导航路径改锚点。
# Group pages: the former display section is split into display/refresh/power level-two cards; slices follow the new navigation anchors.
start=ui.index('static app_redraw_t on_gesture')
def group_block(name,after):
    a=ui.index('        if (s_page==SETTINGS_GROUP_%s) {'%name,after)
    e=ui.index('        return APP_REDRAW_PAGE;',a)
    e=ui.index('\n',e)+1
    e=ui.index('\n',ui.index('        }',e))+1
    return ui[a:e]
guard='        if (y<SETTINGS_GROUP_CARD_Y) return APP_REDRAW_NONE;'
row_line='        const int row=(y-SETTINGS_GROUP_CARD_Y)/SETTINGS_ROW_H;'
a=ui.index(guard,start);b=ui.index(row_line,a)+len(row_line)
piece=ui[a:b]
for group in ('DISPLAY','REFRESH','POWER'):
    piece+='\n'+group_block(group,b)
unit+=r'''
static int setting_tap(int x,int y){
 if (y<160 || y>=UI_NAV_TOP || x<36 || x>=648) return APP_REDRAW_NONE;
'''+piece+'''
 return APP_REDRAW_NONE;}
'''
unit+=r'''
static bool load_exists;static uint8_t load_value;
static int nvs_get_u8(int h,const char *key,uint8_t *value){(void)h;assert(!strcmp(key,NVS_KEY_WAKE_PRESSES));if(!load_exists)return -1;*value=load_value;return ESP_OK;}
static void reload_wake(bool exists,uint8_t value){load_exists=exists;load_value=value;int h=0;uint8_t raw;
'''
a=settings.read_text().index('    raw = 1;\n    if (nvs_get_u8(h, NVS_KEY_WAKE_PRESSES,')
b=settings.read_text().index('    s_wake_presses = raw;',a)+len('    s_wake_presses = raw;')
unit+=settings.read_text()[a:b]+'}\n'
unit+=r'''
static bool read_pico_pmu_take_boot_short_press(void){bool value=boot_press;boot_press=false;return value;}
static bool late_press;
static bool read_pico_pmu_take_key_wakeup_single(void){bool value=late_press;late_press=false;return value;}
static void vTaskDelay(unsigned ticks){(void)ticks;}
#define pdMS_TO_TICKS(ms) (ms)
static bool lock_pin_enabled(void){return pin;}
static void pico_boot_ready(void){++ready;}
static void pico_boot_hold_resume(void){++held;}
static bool app_lock_resume_frame(void *hl){(void)hl;return false;}
static void app_lock_boot_gate(void *a,int b,int c){(void)a;(void)b;(void)c;++gate;}
static void boot_gate(bool resuming){int hl=0;struct {int touch,sensor;bool pmu_ready;}hw={0};hw.pmu_ready=true;bool restored_lock=false;(void)restored_lock;
'''
start=boot.index('    const bool one_press_boot = resuming && !lock_pin_enabled() && hw.pmu_ready &&')
end=boot.index('    rp_boot_mark("lock");',start)
unit+=boot[start:end]+'}\n'
unit+=r'''
int main(void){
 assert(app_settings_wake_presses()==1);
 reload_wake(false,2);assert(app_settings_wake_presses()==1);
 app_settings_set_wake_presses(0);assert(writes==0&&app_settings_wake_presses()==1);
 app_settings_set_wake_presses(2);assert(stored==2&&writes==1);
 app_settings_set_wake_presses(2);assert(writes==1);
 reload_wake(true,2);assert(app_settings_wake_presses()==2);
 reload_wake(true,1);assert(app_settings_wake_presses()==1);
 for(unsigned i=0;i<256;i++)if(i!=1&&i!=2){reload_wake(true,i);assert(app_settings_wake_presses()==1);}
 // 显示组与刷新组承接原显示区其余行：字体、对比度、刷新方式、强刷开关。
 // Display and refresh cards take the rest of the former display section: font, contrast, refresh mode and the full-refresh switch.
 s_page=SETTINGS_GROUP_DISPLAY;s_notice[0]='x';s_font_page=5;
 assert(setting_tap(300,SETTINGS_GROUP_CARD_Y)==APP_REDRAW_PAGE&&s_page==SETTINGS_SYSTEM_FONT&&s_font_page==0&&s_notice[0]==0);
 s_page=SETTINGS_GROUP_DISPLAY;
 assert(setting_tap(300,SETTINGS_GROUP_CARD_Y+SETTINGS_ROW_H)==APP_REDRAW_PAGE&&s_page==SETTINGS_SYSTEM_CONTRAST);
 s_page=SETTINGS_GROUP_REFRESH;
 assert(setting_tap(300,SETTINGS_GROUP_CARD_Y)==APP_REDRAW_PAGE&&s_page==SETTINGS_MAIN_REFRESH);
 s_page=SETTINGS_GROUP_REFRESH;
 assert(setting_tap(300,SETTINGS_GROUP_CARD_Y+SETTINGS_ROW_H)==APP_REDRAW_PAGE&&home_full&&s_page==SETTINGS_GROUP_REFRESH);
 assert(setting_tap(300,SETTINGS_GROUP_CARD_Y+SETTINGS_ROW_H)==APP_REDRAW_PAGE&&!home_full&&s_page==SETTINGS_GROUP_REFRESH);
 assert(SETTINGS_POWER_ROWS==2&&SETTINGS_GROUP_CARD_Y==214);
 const int wake=SETTINGS_GROUP_CARD_Y+SETTINGS_ROW_H;
 // 电源组首行自动锁屏进入子页，唤醒开关不动。
 // The power card's first row opens auto lock, leaving the wake switch alone.
 s_page=SETTINGS_GROUP_POWER;
 assert(setting_tap(300,SETTINGS_GROUP_CARD_Y)==APP_REDRAW_PAGE&&s_page==SETTINGS_AUTO_LOCK&&s_wake_presses==1);
 // 次行整行都是唤醒次数开关：1/2 之间来回切换且不离开分组页，左右边缘都生效。
 // The second row is the wake-count switch across the whole width: toggles 1/2 without leaving the group, both edges included.
 s_page=SETTINGS_GROUP_POWER;
 assert(setting_tap(36,wake)==APP_REDRAW_PAGE&&s_wake_presses==2&&s_page==SETTINGS_GROUP_POWER);
 assert(setting_tap(647,wake+SETTINGS_ROW_H-1)==APP_REDRAW_PAGE&&s_wake_presses==1&&s_page==SETTINGS_GROUP_POWER);
 // 行外左右边界、卡片上方与导航栏高度都不响应。
 // Column edges, the strip above the card and the nav bar stay inert.
 assert(setting_tap(35,wake)==APP_REDRAW_NONE&&s_wake_presses==1);
 assert(setting_tap(648,wake)==APP_REDRAW_NONE&&s_wake_presses==1);
 assert(setting_tap(300,SETTINGS_GROUP_CARD_Y-1)==APP_REDRAW_NONE&&s_wake_presses==1);
 assert(setting_tap(300,UI_NAV_TOP)==APP_REDRAW_NONE);
 // Pico 电源组只有两行（深睡三档为 Metalio 独有）：第三行不进入深睡页。
 // The pico power card has two rows (sleep choices are Metalio-only): a third row must not open the sleep page.
 s_page=SETTINGS_GROUP_POWER;
 assert(setting_tap(300,SETTINGS_GROUP_CARD_Y+2*SETTINGS_ROW_H)==APP_REDRAW_PAGE&&s_page==SETTINGS_GROUP_POWER&&s_wake_presses==1);
 for(unsigned resume=0;resume<2;++resume)for(unsigned password=0;password<2;++password)
 for(unsigned key=0;key<2;++key)for(unsigned count=1;count<=2;++count){
  s_wake_presses=count;pin=password;boot_press=key;late_press=false;gate=ready=held=0;
  boot_gate(resume);bool direct=resume&&!password&&key&&count==1;
  assert(gate==!direct&&ready==direct&&held==direct&&!boot_press);
 }
 s_wake_presses=1;pin=false;boot_press=false;late_press=true;gate=ready=held=0;
 boot_gate(true);assert(!gate&&ready==1&&held==1&&!late_press);
 s_wake_presses=2;pin=false;boot_press=false;late_press=true;gate=ready=held=0;
 boot_gate(true);assert(gate&&!ready&&!held&&late_press);
 puts("PASS: single-press default/missing/invalid NVS, explicit double choice retained, power-card switch below auto lock, group row/nav boundaries, wake/PIN/late press guards");
}
'''
with tempfile.TemporaryDirectory() as folder:
    p=Path(folder);(p/'test.c').write_text(unit)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-I'+str(ROOT/'main/ui'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
