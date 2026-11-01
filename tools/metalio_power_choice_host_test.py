"""Execute the actual S31 six-step deep-sleep render and touch blocks together.

SPDX-License-Identifier: Apache-2.0
中文：设置页「关机睡眠」已从「彻底断电 / 先浅睡10分钟」两档改成锁屏转深睡延时的六档卡片，
旧的 staged_shutdown 分支只留给非 S31 板型。测试改为抽取 metalio47 视图下的六档卡片代码，
并直接用源码里的 k_sleep_off_order / k_sleep_off_labels / SLEEP_OFF_ROWS 与
app_settings_sleep_off_code() / app_settings_set_sleep_off_code()。
English: the settings "shutdown sleep" page moved from the two-stage shutdown choice to six
lock-to-deep-sleep delay cards; the old staged_shutdown branch survives for non-S31 boards only.
The test now extracts the six-card code from the metalio47 view and drives the real
k_sleep_off_order / k_sleep_off_labels / SLEEP_OFF_ROWS constants plus
app_settings_sleep_off_code() / app_settings_set_sleep_off_code().
"""
from pathlib import Path
import re
import subprocess
import tempfile
from board_source_view import select_source

root = Path(__file__).resolve().parents[1]
# 只解释本工程的板型分支，避免抽到 #else 里的旧两档关机页面。
# Preselect this project's board branch so the legacy two-choice #else page can never be extracted.
source = select_source((root / 'main/apps/app_device_settings.c').read_text(encoding='utf-8'), 'metalio47')
settings = select_source((root / 'main/settings.h').read_text(encoding='utf-8'), 'metalio47')
start = source.index('    if (s_page == SETTINGS_POWER_SLEEP) {', source.index('省电选项见'))
paint = source[start:source.index('\n    if (s_page == SETTINGS_BOOT)', start)]
start = source.index('    if (s_page == SETTINGS_POWER_SLEEP) {', source.index('static app_redraw_t on_gesture'))
tap = source[start:source.index('    if (s_page == SETTINGS_CONFIG) {', start)]
assert 'SETTINGS_LOCK_SLEEP' not in source


def const_array(name):
    """中文：原样取出源码里的档位数组，保证顺序/文案与固件一致。/ English: lift the real step arrays so order and labels follow the firmware."""
    at = source.index(name)
    at = source.rindex('static', 0, at)
    return source[at:source.index(';', at) + 1]


order = const_array('k_sleep_off_order[]')
labels = const_array('k_sleep_off_labels[]')
rows = re.search(r'^#define SLEEP_OFF_ROWS[^\n]*', source, re.M).group(0)
# 档位枚举在 settings.h；不可在桩里另写一套数值。/ The step enum lives in settings.h; never restate its values in the stub.
off_enum = settings[settings.rindex('typedef enum {', 0, settings.index('} app_sleep_off_code_t;')):
                    settings.index('} app_sleep_off_code_t;') + len('} app_sleep_off_code_t;')]
unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {int x0,y0;} Event;
enum {SETTINGS_POWER_SLEEP=1,APP_REDRAW_NONE=0,APP_REDRAW_PAGE=2};
#define UI_GRAY_WHITE 255
#define EPD_DRAW_ALIGN_LEFT 0
#define UI_GRAY_BLACK 0
'''+off_enum+'''
static int s_page=SETTINGS_POWER_SLEEP,cards,dots,titles;
// 关机睡眠档位：仅保存/读取当前选择，失败不改值。/ Shutdown-sleep step: store/read the choice only.
static uint8_t s_sleep_off_code=APP_SLEEP_OFF_IMMEDIATE;
static uint8_t app_settings_sleep_off_code(void){return s_sleep_off_code;}
static void app_settings_set_sleep_off_code(uint8_t code){assert(code<APP_SLEEP_OFF_CODE_COUNT);s_sleep_off_code=code;}
'''+order+'\n'+labels+'\n'+rows+'''
static EpdRect boxes[SLEEP_OFF_ROWS];
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
static void back_header(uint8_t*f,const char*t){(void)f;assert(!strcmp(t,"关机睡眠"));}
static void section(uint8_t*f,int y,const char*t){(void)f;(void)y;assert(t&&t[0]);}
static void settings_card(uint8_t*f,EpdRect r,int radius,int fill,int border){
 (void)f;(void)radius;(void)border;assert(cards<SLEEP_OFF_ROWS);boxes[cards]=r;
 assert(fill==((s_sleep_off_code==k_sleep_off_order[cards])?0xd0:255));++cards;
}
static void ui_text(uint8_t*f,int x,int y,int px,const char*t,int align,bool bold){
 (void)f;(void)x;(void)y;(void)px;(void)align;(void)bold;
 assert(!strstr(t,"锁屏省电方式")&&!strstr(t,"保留待机"));
 for(int i=0;i<SLEEP_OFF_ROWS;i++)if(!strcmp(t,k_sleep_off_labels[i]))++titles;
}
static void epd_fill_circle(int x,int y,int r,int c,uint8_t*f){(void)x;(void)y;(void)r;(void)c;(void)f;++dots;}
static void settings_navigation(uint8_t*f){(void)f;}
'''
unit += 'static void paint(void){uint8_t *fb=NULL;' + paint + '}\n'
unit += 'static int tap(int x,int y){Event e={x,y};const Event *ev=&e;' + tap + 'return APP_REDRAW_NONE;}\n'
unit += r'''
int main(void){
 // 界面顺序把 1 分钟调试档放在「立即」之后；枚举里它仍在末尾，避免迁移已存的 NVS 数字。
 // The debug 1-minute step is shown right after "immediate" while staying last in the enum, so
 // stored NVS numbers keep their meaning.
 assert(k_sleep_off_order[0]==APP_SLEEP_OFF_IMMEDIATE&&k_sleep_off_order[1]==APP_SLEEP_OFF_1MIN);
 assert(SLEEP_OFF_ROWS==APP_SLEEP_OFF_CODE_COUNT);
 for(int initial=0;initial<SLEEP_OFF_ROWS;initial++){
  s_sleep_off_code=k_sleep_off_order[initial];cards=dots=titles=0;paint();
  assert(cards==SLEEP_OFF_ROWS&&dots==1&&titles==SLEEP_OFF_ROWS);
  // Every drawn pixel shares the same hit rectangle; gaps and the retired shutdown choice do nothing.
  for(int y=0;y<1216;y+=3)for(int x=0;x<684;x+=3){
   s_sleep_off_code=k_sleep_off_order[initial];int target=-1;
   for(int i=0;i<SLEEP_OFF_ROWS;i++)if(ui_rect_hit(boxes[i],x,y))target=i;
   int result=tap(x,y);
   assert(result==(target<0?APP_REDRAW_NONE:APP_REDRAW_PAGE));
   assert(s_sleep_off_code==(target<0?k_sleep_off_order[initial]:k_sleep_off_order[target]));
  }
  // 卡片下方的说明文字不可点。/ Subtext below the cards is not tappable.
  s_sleep_off_code=k_sleep_off_order[initial];assert(tap(300,900)==APP_REDRAW_NONE&&s_sleep_off_code==k_sleep_off_order[initial]);
 }
 puts("PASS: actual S31 render/touch selects one of six deep-sleep steps; identical card/hit bounds, gap isolation, one selection marker");
}
'''
with tempfile.TemporaryDirectory() as folder:
    path = Path(folder)
    (path / 'test.c').write_text(unit)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
