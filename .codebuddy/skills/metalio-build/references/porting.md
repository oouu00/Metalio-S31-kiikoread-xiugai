# 移植清单（kiiko → rc104）：A–H + 检查单

> 本文件自包含收录《移植笔记-夜间模式与滑块限频.md》的 A–H 改动清单，供 skill 分发。
> 查找方式：**用"锚点"一栏的函数名/唯一字符串在新版本里搜索**，不要依赖行号。

## 语义（先读懂再动手）

- **夜间模式 = 输出极性反转**，不是两套配色。framebuffer 永远是浅色语义，只在送屏时
  把波形表的 (to,from) 索引反转（to↔15-to、from↔15-from）。差分、局刷、抗锯齿、玻璃读回全部不用改。
- **只反相小说正文页**：极性跟随"当前呈现的主体"——正文页(无面板/无确认框)开，
  阅读菜单/面板/快捷菜单/主菜单/其它 app 一律浅色。
- **极性切换必须搭整页刷新**（差分按逻辑灰度算，旧极性的已显示像素 diff=0 不会重画）。
  因此：夜间下面板开合强制走整页路径（绕过 rc104 的两条面板快速路径）。
- **正文插图要正片补偿**：极性开着时图片先自己取反，驱动层再反一次 → 照片保持原色。

---

## A. epdiy 驱动层（4 个文件）

### A1. `components/epdiy/src/epdiy.h`
- 锚点：`void epd_clear_phase_luts(void);` 之后
- 新增声明：`epd_set_inverted(bool)` / `epd_get_inverted(void)`（共享头，两块板共用）。

### A2. `components/epdiy/src/output_common/render_context.h`
- 锚点：`epd_populate_line_mask` 声明之后
- 新增声明：`const EpdWaveformPhases* epd_polarity_phases(const EpdWaveformPhases* phases);`

### A3. `boards/metalio47/src/components/epdiy/src/output_common/render_context.c`（Metalio 实际编译副本）
- includes 加 `#include "esp_heap_caps.h"`。
- 文件头部（`render_method.h` include 之后）插入整段极性实现：
  `s_inverted / epd_set_inverted / epd_get_inverted / polarity_lut_get / polarity_lut_put / epd_polarity_phases`。
  `epd_polarity_phases`：未开启原样返回；开启时把每相 16×16×2bit LUT 按 (15-to,15-from) 重排，
  反转副本缓冲用 `heap_caps_malloc(..., MALLOC_CAP_INTERNAL)` 按需分配，**分配失败降级为不反色**（绝不刷错画面）。
- 锚点：`prepare_context_for_next_frame` 里 `ctx->lut_build_func(ctx->conversion_lut, phases, ...)`，
  把 `phases` 换成 `epd_polarity_phases(phases)`。

### A4. `boards/pico/src/components/epdiy/src/output_common/render_context.c`
- 与 A3 完全相同的两处改动（共享目录没有这份文件，只有两份板级副本；pico 不改会链接失败）。

### A5. `boards/metalio47/src/components/epdiy/src/render.c` —— **关键坑**
- 删除 `static const EpdWaveformPhases* s_first_active_phases;`（指针缓存变量）。
- `build_first_active()`：开头加
  `const EpdWaveformPhases* table = epd_polarity_phases(phases);`，全函数用 `table` 代替 `phases`，
  **并删掉函数尾的 `s_first_active_phases = phases;`**。
- `leading_skip_frames()`：`if (phases != s_first_active_phases) build_first_active(phases);`
  改成无条件 `build_first_active(phases);`
- 原因：极性反转时表**内容**变而指针复用同一块缓冲，按指针缓存会拿旧表算前导跳过 → 反转后首帧跳相错误。
- **每个含 leading-skip 的 render.c 副本都要同样改**（pico 副本 `boards/pico/src/components/epdiy/src/render.c` 已同步）。

---

## B. 设置层（2 个文件）

### B1. `main/settings.c`
- includes 加 `#include "epdiy.h"`。
- 锚点 `#define NVS_KEY_VERTICAL_TURN "rd_vertical"` 后加 `#define NVS_KEY_NIGHT "night"`。
- 锚点 `static bool s_reader_vertical_turn;` 后加 `static bool s_night_mode;`。
- `app_settings_init()`：锚点 `NVS_KEY_PICKUP` 读取之后加：
  ```c
  uint8_t night = 0;
  if (nvs_get_u8(h, NVS_KEY_NIGHT, &night) == ESP_OK) s_night_mode = night == 1;
  epd_set_inverted(s_night_mode);   // 启动锁屏/首帧前恢复极性
  ```
- 锚点 `app_settings_set_pickup_wake` 之后加 getter/setter：
  `app_settings_night_mode()` / `app_settings_set_night_mode(bool)`（setter 只存 NVS，**不直接设极性**，极性由 app_loop 跟随页面）。
- 亮度上限（G-2）同法加：`NVS_KEY_BRIGHT_MAX "blmax"`、`static unsigned s_brightness_max = 240;`、
  读取（钳 1..240）、`app_settings_brightness_max()` / `app_settings_set_brightness_max(unsigned)`。

### B2. `main/settings.h`
- 锚点 `app_settings_set_pickup_wake` 声明之后加 `app_settings_night_mode` / `app_settings_set_night_mode` 声明；
  亮度上限声明加 `app_settings_brightness_max` / `app_settings_set_brightness_max`。

---

## C. app_loop.c（极性跟随 + 强制全刷，7 处）

1. 锚点 `static bool ai_overlay_open(void)` 函数之后，新增：
   ```c
   static bool s_last_body_dark;
   static void apply_output_polarity(const app_desc_t *app) {
       epd_set_inverted(app_settings_night_mode() && app == &app_book &&
                        app_book_reader_body_visible() && !s_quick.open && !ai_overlay_open());
   }
   ```
   （比 kiiko 多了 `!ai_overlay_open()`：rc104 有 AI 菜单浮层，打开时保持浅色。）
2. `app_present_impl` 开头（`enum EpdDrawError result = EPD_DRAW_SUCCESS;` 之后）：`apply_output_polarity(app);`
   **紧接着**加"进出深色正文页强制整页刷新"：
   ```c
   const bool body_dark = epd_get_inverted();
   if (body_dark != s_last_body_dark) {
       s_last_body_dark = body_dark;
       if (app_settings_night_mode() && (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_AREA))
           redraw = APP_REDRAW_FULL;
   }
   ```
   只在夜间模式开启时升级为 FULL；日间模式不受影响。去条件即无条件全刷。
3. `quick_present`（下拉栏）：在推屏段之前（`s_quick_status=status;` 之后）：`apply_output_polarity(current);`
   **紧接着**加"进出下拉栏强制整页刷新"：
   ```c
   const bool quick_body_dark = epd_get_inverted();
   if (quick_body_dark != s_last_body_dark) {
       s_last_body_dark = quick_body_dark;
       if (app_settings_night_mode()) full = true;
   }
   ```
   只改 `full` 标志，且只在极性翻转时置位——下拉栏拖亮度/色温滑杆不改变极性，仍走局刷不被拖慢。
4. `present_menu` 开头：`apply_output_polarity(current);`
5. `ai_push`：`s_ai.open?...:update_display_full(...)` 改为
   `(s_ai.open||app_settings_night_mode())?update_display_gray_transition(...):update_display_full(...)`
   —— 夜间下 AI 菜单开合都整页。
6. `run_power_action`：`ui_power_final_draw` 之前 `epd_set_inverted(false);`（关机/重启画面恒浅色）。

---

## D. 锁屏/睡眠（2 个文件）

### D1. `boards/metalio47/src/main/sleep.c`（sleep.c 在板级副本里，main/ 没有）
- `enter_lock_and_sleep()`：`epd_hl_get_framebuffer` 之后加 `epd_set_inverted(false);`
- `lock_resume_service()`（启动锁）：函数开头同样 `epd_set_inverted(false);`

### D2. `main/apps/app_sleep.c`
- `sleep_show_notice()`：`ui_clear_page` 之前 `epd_set_inverted(false);`
- app_sleep 的设置页走 app_present_impl → 已被 C-2 覆盖，无需改。

---

## E. app_book.c（6 处）

### E1. 夜间开关 = 阅读工具栏第 6 个按钮（月亮/太阳）
> 位置固定在底部工具栏（READER_PANEL_TOOLS），在「字体设置」右侧；面板标题栏**不放**开关。
> 图标与文字**一致地表示当前模式**：月亮 +「夜间」，太阳 +「日间」；点按切换。

1. 锚点 `#define BOOK_TOOL_COUNT 5` → 改 `6`（`tool_rect(i)` 按 `UI_LOCK_WIDTH / BOOK_TOOL_COUNT` 均分）。
2. 锚点 `static void draw_reader_tool_icon(...)`：
   - 函数前加前置声明 `static void draw_night_icon(uint8_t *fb, int cx, int cy, int r);`
   - 越界判断由 `>=` 放宽为 `>`，并加分支 `if (kind == (int)(sizeof(tools)/sizeof(tools[0]))) { draw_night_icon(fb, cx, cy, 15); return; }`
   - `draw_night_icon()`（替换原先 `draw_night_toggle`）：按传入中心/半径画月亮或太阳。
3. 锚点 `const char* labels[] = {"目录","书签","阅读统计","阅读设置","字体设置"};`：
   末尾加 `"夜间"`，第 6 个文字动态（与图标同"当前模式"）：
   ```c
   const char* label = i == BOOK_TOOL_COUNT - 1
       ? (app_settings_night_mode() ? "夜间" : "日间")
       : (bookmarked ? "已书签" : labels[i]);
   ```
4. 点击处理：锚点 `if (s_reader_panel == READER_PANEL_TOOLS)` 的 `for` 分支，原 `else`（字体设置）改显式
   `else if (i == 4)`，再加 `else`（i==5 夜间）：
   ```c
   } else {
       app_settings_set_night_mode(!app_settings_night_mode());
       s_reader_panel = READER_PANEL_NONE;
   }
   ```
5. 按压命中（锚点 `for (...){ *rect = tool_rect(i); ... return 200 + i; }`）自动覆盖第 6 个，无需改。

### E2. 夜间禁用两条面板快速路径（极性切换必须整页）
- `reader_panel_fast_present()` 开头：`if(app_settings_night_mode())return false;`
- `reader_panel_short_ready()` 开头：同样一行。

### E3. 滑块拖动限频（卡顿优化本体）
- 锚点 `static int s_reader_slider = -1;` 后加：
  ```c
  #define READER_SLIDER_REFRESH_MS 150
  static int64_t s_slider_present_ms;
  ```
- `reader_slider_feedback()`（Metalio 分支）设置 `s_area` 之后、返回之前插：
  ```c
  if (ctx->now_ms < s_slider_present_ms) s_slider_present_ms = 0;
  if (ctx->now_ms - s_slider_present_ms < READER_SLIDER_REFRESH_MS) return APP_REDRAW_NONE;
  s_slider_present_ms = ctx->now_ms;
  s_mode=MODE_DU;
  ```
- PRESS 分支：`reader_slider_feedback` 调用前 `s_slider_present_ms = 0;`
- 目录跳转拖动条（`s_toc_jump_drag && UI_GESTURE_MOVE` 分支）：同样 DU+150ms 限频；
  其 PRESS 分支加 `s_slider_present_ms = 0;`
- 通用 AREA 出口：`update_display_area_with(ctx->hl, &E0470_WAVEFORM, s_mode, s_area)` 改为
  `s_mode == MODE_DU ? &E0470_FOLLOW_WAVEFORM : &E0470_WAVEFORM`。
- 松手 commit 不动（仍整页 GL16 定稿）。

### E4. 正文插图正片补偿（2 个像素点）
- `draw_zoomed_image()`：`uint8_t gray = reader_sample_bilinear(...)` 外面包 `ui_image_night_gray(...)`。
- 正文内联插图（锚点 `ui_image_dither_gray(inline_ink_gray(image->gray[...])`）：
  `inline_ink_gray(...)` 外面包 `ui_image_night_gray(...)`。

---

## F. ui_kit（2 个文件）

- `main/ui/ui_kit.c`：includes 加 `#include "epdiy.h"`；`ui_clear_page` 之前加：
  ```c
  uint8_t ui_image_night_gray(uint8_t gray) {
      return epd_get_inverted() ? (uint8_t)(255u - gray) : gray;
  }
  ```
- `main/ui/ui_kit.h`：`ui_contrast_gray` 声明后加 `uint8_t ui_image_night_gray(uint8_t gray);`

---

## G. 下拉栏亮度滑轨改指数曲线（1 个文件）

文件：`main/ui/ui_quick_brightness.h`（整个重写）

- 背景：原实现分段线性（低亮度占半条滑轨），改成**指数**——每往下一像素，亮度 ×0.975。
- 公式（顶部为满值，底部约 24px 因整数化归 0 即熄灯）：
  ```
  value(p) = RANGE * 0.975^(RANGE - p)      RANGE = UI_QUICK_SLIDER_RANGE = 240
  反解     p = RANGE - ln(value/RANGE) / ln(0.975)
  ```
- 实现要点：
  - 需 `#include <math.h>`，用 `powf` / `logf`。
  - `ui_quick_brightness_value()`：算出浮点后 `if (!(v >= 1.0f)) return 0;`（低于 1 归 0，否则 `value(0)=1` 反解回 position=24，滑块跳）。
  - `ui_quick_brightness_position()`：`if (value == 0) return 0;`（否则 `logf(0)` 得 `-inf`，转 unsigned 未定义行为）。
  - 系数写成宏 `UI_QUICK_BRIGHTNESS_STEP`（现 0.975f），换手感只改这一处。
- 改前/改后对照（满值 240）：顶部 240 / 中间 48→约 12 / 底部 0。
- 只改界面换算；底层 PWM 与保存值（0..240 像素单位）不变。硬件：LEDC 10-bit@20kHz、`duty = 位置*1023/(2*240)` → 0..511 共 512 级，平滑连续。

### G-2. 亮度滑块上限设置（"设置 → 前光与振动 → 亮度上限"）

滑块最高亮度可设上限 `max`，滑轨按 **`max × 0.975^级`** 递减（满值不再是固定 240）。

- 设置层（`settings.h` / `settings.c`）：NVS 键 `blmax`，`app_settings_brightness_max()` / `app_settings_set_brightness_max()`，
  静态 `s_brightness_max` 默认 **240**（不配置退化为原满量程），范围钳 **1..240**。
- 换算层（`ui_quick_brightness.h`）：`value()` / `position()` 满值由 `RANGE(240)` 改为 `app_settings_brightness_max()`；
  新增 `ui_quick_brightness_percent()` 按 `value*100/top`，**滑块顶部即 100% = 上限**。
- 设置页（`app_device_settings.c`，仅 Metalio `#if KIIKO_BOARD_METALIO47` 的 `SETTINGS_FRONTLIGHT` 页）：
  - 显示：在"触摸振动"行后加 `row(930,"亮度上限",百分比)` 与 `row(1020,"降低 5%","增加 5%")`。
  - 点击：两处 FRONTLIGHT 手势分支都加 `y>=1020 && y<1088` —— 左半（`x0<342`）`max-12`（下限 1）、右半 `max+12`（上限 240），步进 12（=240 的 5%）。
  - `ui_quick_menu_input.c` 两处滑块百分比改为调 `ui_quick_brightness_percent()`。
- 行为：设低上限只压暗滑轨天花板，不动当前已存亮度；一旦在下拉栏拖动亮度滑块，亮度即被限到 ≤max。
- ⚠️ 布局：930/1020 两行凑在振动行（850）之后，若与底部 `settings_navigation` 状态条重叠，调小这两处 y（屏幕高 1216）。

---

## H. 构建与 TF 卡升级包（操作要点）

- 编译脚本 `build_metalio.py`（板型 `-DKIIKO_BOARD=metalio47` 选板，自动锁 esp32s31）：
  - `python build_metalio.py` 出正式版（release 档）；`-p` 编译后串口烧写（COM5）
  - `--profile debug` 出调试版：默认 DEBUG 日志、`--trace` / `--bt-trace` 默认开；两档各有
    独立构建目录（`build/` 与 `build-debug/`）和独立 sdkconfig，来回切不用重编对方
  - `--ver 版本号` 覆盖 `PROJECT_VER`（touch 根 CMakeLists.txt 强制 cmake 重新配置）
  - `--sd` 编译后把 `build/Kiikoread_Metalio_S31.bin` 复制成项目根 `Metalio-S31-update.bin`
- **版本号 `0.3.3-rc108.1`**（`boards/metalio47/board.json`，脚本默认取它；`build_metalio.py`
  的 `PROJECT_VER` 与 `双击编译.bat/.py` 横幅同步）。同版本卡刷是允许的：`ota_update.c` 只把
  `strcmp(current,candidate)==0` 标成 `same_version`，界面提示“重装 TF 卡固件”。日常只在
  合入新上游 rc（rc108 → rc109 …）时递增，仓内小修用 `.1` 后缀；只有刷进仍在拒同版本的老固件时才临时 `--ver`。
- 设备端路径 `PICO_OTA_UPDATE_PATH` = `/sdcard/Metalio-S31-update.bin`（见 `ota_update.h`）。
- 升级入口：设置 → 设备设置 → 固件升级（`upgrade_open()`）。
- 校验三项：chip_id==32、项目名一致、版本不同。写入空闲 OTA 分区，失败回滚。
- 串口烧写偏移：bootloader 0x2000 / 分区表 0x8000 / otadata 0xd000 / app 0x10000，保留 NVS。

---

## rc108 移植检查单（按顺序）

1. A1/A2 头文件声明 → 2. A3/A4 两份 render_context.c（实现 + lut_build_func 行）→
3. A5 所有含 leading-skip 的 render.c 副本（去指针缓存）→ 4. B1/B2 设置 →
5. C 七处 app_loop（含"进出深色正文强制全刷"与 C-3 下拉栏开合全刷）→ 6. D 锁屏/睡眠 →
7. E1 工具栏第 6 按钮（BOOK_TOOL_COUNT 改 6 + 图标/文字/点击分支）→
   E2 面板快速路径让路 → E3 滑块限频 → E4 插图补偿 → 8. F ui_kit →
9. **G 亮度指数曲线 + G-2 亮度上限设置**（`ui_quick_brightness.h` + settings + app_device_settings）。
10. 编译 + 出卡刷包（见 H 节，记得改版本号）；自测路径：
   - 夜间：正文页(黑底白字) ↔ 底部工具栏第 6 个日月按钮切换（回正文并整页刷新）↔
     开阅读菜单 / 拉下快捷菜单 / 返回主页 / 再进阅读，每次整页刷新、无半屏残留；
   - 快捷菜单、主菜单、AI 菜单、锁屏、关机画面保持浅色；
   - 下拉栏拖亮度滑杆仍跟手（不被全刷拖慢），低亮度段更细；
   - 阅读设置滑杆拖动跟手无逐帧顿挫。
11. 工程移动目录后 `build/` 的 CMake 缓存失效，删掉 `build/` 重新完整编译。

## 已知限制 / 未移植项

- kiiko 温度自动选档（灰斑根治）、书架封面/壁纸/图片查看器补偿**未移植**——
  rc104 极性只在正文页开启，其它界面不反色所以不需要；温度问题 rc104 上未复现。
- 睡眠锁屏 opt-out 用"入口退极性"实现（rc104 锁屏推送全是全表面，等价于 kiiko 整帧反转方案）。
- 透明书封锁屏（lock_style==3）从夜间正文进入：入口已退极性且走 image_gray 全表面输出，已覆盖。
- 强制全刷目前**仅在夜间模式开启时**生效（C-2、C-3 带 `app_settings_night_mode() &&`），
  日间模式进出正文/下拉栏不额外全刷。无条件全刷去掉该条件即可。
