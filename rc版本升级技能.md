# rc 版本升级技能（上游合并流程）

把上游新的 `Metalio-S31-kiikoread-mainrcNNN` 合并进本仓库，**只落上游增量、本地改动按行保留**。
本仓库的基线：`rc104` 分叉 → 已合并 `rc105` → `rc106` → `rc107` → **`rc108`（当前，产物 `rc108.1`）**。
下次（rc109/rc112）照此流程走。

> **2026-11-01 rc107.1 → rc108.1 的经验（最重要，先看这个）**
> 上游 rc108 只动了 **8 个文件**（普通翻页改 full GL48 55 相、长按开机直进门禁、三份测试与 BOARDS.md），
> 但 `tools/rc_upgrade_merge.py` 在本仓库**不可直接用**：上游发布包是 LF，本地部分文件被检出成 CRLF，
> 逐字节三方合并会把「只差行尾」的文件当成「双方都改过」，要么整文件判冲突（草稿里只有一个巨大冲突块），
> 要么把本地内容整文件重写。本轮改为**逐文件人工合并 + 两个脚本校验**，结论已写进第 0.9 节。
> 另：`rc_upgrade_merge.py` 遇到「base 和 theirs 都有、本地已删」的文件会 `FileNotFoundError` 崩在半路
> （例如 `boards/pico/partitions.csv`），所以它跑不完时后面的目录（`components/`、`main/`）根本没被处理。

---

## 0. 有 git 分支时优先用 git（推荐）

仓库已是 git 仓库：分支 `kiikoread-custom`（= 标签 `baseline-rc106` + 一次本地改动提交），
`origin` = `oouu00/Metalio-S31-kiikoread`，`upstream` = `wegooo-cell/Metalio-S31-kiikoread`。

```powershell
cd C:\esp\metalio
git fetch upstream --tags              # 网络不稳时多试几次，或先 --depth=1 只取标签
git tag baseline-rc107 v0.3.3-rc107    # 记住这一版基线，下次用
git merge upstream/main                # 或 git rebase upstream/main
git commit -am "merge upstream rc107"  # 冲突按第 2 节判据处理
git push origin kiikoread-custom
```

> 历史已接到真实上游：`kiikoread-custom` = 上游 `v0.3.3-rc106`（提交 29fd646）+ 本地改动提交 + 文档提交，
> 与 `upstream/main` 有共同祖先，rc107 可直接 `git merge`。最初那个用发布包重建的临时基线提交已用
> `git rebase --onto v0.3.3-rc106` 抹掉，源码内容零差异（仅上游那份 pmu 文档的行尾与若干文件模式回归上游）。

## 0.9 一句话流程（rc108 起的推荐做法：先探增量，再逐文件人工合并）

上游增量通常只有个位数文件（rc107→rc108 是 8 个），**先量出来再决定怎么合**，比盲跑三方合并快得多：

```powershell
# 1) 上游增量清单（rc107 vs rc108）：只剩这几行输出就说明增量很小
diff -rq "C:\esp\老版本固件\Metalio-S31-kiikoread-mainrc107\Metalio-S31-kiikoread-main" `
         "C:\esp\老版本固件\Metalio-S31-kiikoread-main-rc108\Metalio-S31-kiikoread-main"

# 2) 每个增量文件先看「本地对基线改了多少行」：A=新增行、D=删除行
git --no-pager diff --no-index --numstat -- "<base>\<文件>" "C:\esp\Metalio-S31_0.3.3-rc108.1_src\<文件>"
#    A=0            ⇒ 本地只是旧版残留，直接取 theirs
#    A>0 且与上游改动不重叠（例如本地是纯新增函数）⇒ 取 theirs 再把本地增量贴回去
#    A>0 且改在同一处 ⇒ 按语义合并（第 2 节判据）

# 3) 合并后全树校验（0 LOST 才算干净）
python tools/rc_upgrade_verify.py --base <rc107> --theirs <rc108> --quiet

# 4) 单文件「本地增量一致性」校验：合并前对 base 的 diff 应逐行等于合并后对 theirs 的 diff
#    （脚本片段见第 3.2 节）
```

先复制一份新目录（`Metalio-S31_0.3.3-rc107.1_src` → `...rc108.1_src`）再改，**旧版本目录一律不动**。

### 兜底：rc_upgrade_merge.py（只在行尾干净时可用）
```powershell
python tools/rc_upgrade_merge.py --base <上一次干净的上游> --theirs <新版本>            # 预览
python tools/rc_upgrade_merge.py --base <上一次干净的上游> --theirs <新版本> --apply    # 落盘
python tools/rc_upgrade_merge.py --base <上一次干净的上游> --theirs <新版本> --check    # 复查
```
`--base` **必须**是「上一次已合并干净的上游版本」，不能用本地树，否则本地改动会被当成上游改动回灌。
工具会分类 ADD / TAKE / MERGE / CONFLICT / DEL，冲突草稿写到 `build/rc_upgrade_merge/<文件>/merged`，
落盘前备份到 `build/rc_upgrade_backup/<时间戳>/`，写盘后刷新 mtime（否则 ninja 会复用旧 `.obj`）。
**注意两个坑**：① 行尾不一致会让它把整文件判成冲突/整文件重写（见文首）；② 遇到「base+theirs 有、本地已删」
的文件会 `FileNotFoundError` 中断，后半程目录不会被处理——崩溃后必须用 `rc_upgrade_verify.py` 补校验。


---

## 1. 前置准备

1. 新版本解压到 `C:\esp\老版本固件\Metalio-S31-kiikoread-mainrcNNN\`（注意多套一层同名目录，实际仓库根在里面一层）。
2. 确认新版本版本号：`boards/metalio47/board.json` 的 `version`。
3. 把当前树做一次源码备份（`main components boards tools docs .github` 五个目录），出问题能整体回退。

---

## 2. 冲突处理判据（最重要）

逐个看 `CONFLICT` 文件，用一条命令判断「本地这版是有人有意改的，还是只是旧版残留」：

```powershell
git --no-pager diff --no-index --numstat -- "<base>\<文件>" "C:\esp\metalio\<文件>"
# 输出 A<TAB>D：A=本地新增行数，D=本地删掉的上游行数
```

- **A = 0（只有 D）** ⇒ 本地文件只是旧版残留（上一次同步漏掉的），**直接取 theirs 覆盖**。
- **A > 0** ⇒ 有本地意图，必须三方合并看语义，合并完做第 3.2 的增量一致性校验。
- 常见「A>0 但其实是旧版残留」特征：本地文件里出现上游已经改名的旧 API（如 `APP_SLEEP_OFF`、
  `metalio_light_sleep_start`、`s_lock_io_no_media`、硬编码 `pico_startup`），这类直接取 theirs。

文档类（`docs/CHANGELOG.md`、`docs/BOARDS.md`）默认取 theirs：上游描述的是新行为，本地旧文案会自相矛盾。

---

## 3. 必做校验清单

### 3.1 版本号
- `boards/metalio47/board.json` 的 `version` 已是新 rc。
- `build_wrapper.ps1` 里写死的 `--ver "0.3.3-rcNNN"` 要同步改（否则设备上报旧版本号）。
- pico 的版本号在 `boards/pico/board.json`，上游不一定跟着升，不要乱改。

### 3.2 本地增量一致性校验（防止合并悄悄丢改动）

对每个「双方都改」的文件，比较**合并前**对 base 的 diff 与**合并后**对 theirs 的 diff，两者应逐行相同：

```powershell
$d1 = git --no-pager diff --no-index --unified=0 -- "<base>\<文件>" "<备份>\<文件>" | ? { $_ -match '^[+-]' -and $_ -notmatch '^(\+\+\+|---)' }
$d2 = git --no-pager diff --no-index --unified=0 -- "<theirs>\<文件>" "C:\esp\metalio\<文件>" | ? { $_ -match '^[+-]' -and $_ -notmatch '^(\+\+\+|---)' }
($d1 -join "`n") -eq ($d2 -join "`n")   # 应为 True
```

### 3.3 编译（两板都要）
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:\esp\metalio\build_wrapper.ps1   # S31，双击编译.bat 等价
# pico：
. "C:\esp\idf-v6.1\export.ps1" | Out-Null
python tools/build_firmware.py --board pico --idf-pico C:\esp\idf-v6.1 `
  --python "$env:USERPROFILE\.espressif\python_env\idf6.1_py3.12_env\Scripts\python.exe" `
  --tools "$env:USERPROFILE\.espressif" --managed-pico C:\esp\metalio\managed_components
```
- S31 成功标志：`METALIO_EXITCODE=0` + `[sd] package ready: ...Metalio-S31-update.bin`。
- 已知告警（可忽略）：`esp_sleep_get_wakeup_cause` depr、`app_book.c`/`read_pico_transfer.c` 的
  `defined but not used`（本地实验代码，S31 与 pico 分支各留一份）。

### 3.4 共享组件被两板各编译一次
`components/` 下的代码被两个板共用，而文件里常有 `#if KIIKO_BOARD_METALIO47 ... #else ... #endif`
两份重复实现。**只改了一处分支的声明/定义**时，另一板会报 `undeclared`/`implicit declaration`。
判据：S31 编过但 pico 报错 ⇒ 100% 是 `#else` 分支缺前向声明，补一行声明即可，不要改逻辑。

### 3.5 板级宏与实验开关
- 新增的 `bootloader_components/`、`Kconfig.projbuild` 要确认默认值（上游默认关就保持关，例如
  `CONFIG_METALIO_EARLY_POWER_KEY=n`）。
- 本地实验开关（`--s31-pie`、`--s31-text-ripple`、`--s31-early-power-key` 等）交付一律关。

---

## 4. 踩过的坑（速查）

| 现象 | 原因 | 处理 |
|---|---|---|
| 改完编译成功但刷机后行为没变 | `Copy-Item` 保留旧时间戳，ninja 复用旧 `.obj` | 改动后必须 touch mtime；或删 `build/` 里对应 `.obj` |
| `present` 抽取器 `IndexError` | 宿主测试用正则按花括号深度抽函数体，`#if/#else` 两支各一个 `{` 共用一个 `}` | 抽取器已改为「深度优先 + 行首 `}` 兜底」 |
| `-lasan`/`-lubsan` 找不到 | 本机 mingw 缺 sanitizer 运行库 | 本地跑测试时去掉 `-fsanitize=address,undefined` |
| 直接对维护源跑板级宿主测试报 `implicit declaration` | 维护源里板级 `#if` 未解析 | 用 `tools/board_source_view.py --tests` 物化视图跑 |
| 上游删了某个测试文件，本地还在 | 上游把文件搬了位置（如 `boards/.../tests/tools/boot_state_host_test.c` → `tools/`） | 以 theirs 为准：本地同名旧文件删掉 |
| 卡刷包版本号没变 | `build_wrapper.ps1` 写死 `--ver` | 同步改版本号 |

---

## 5. 归档

合并且刷机验收通过后，用 `双击编译.bat` 出包 → `C:\esp\metalio-release\` 会拿到
`Metalio-S31-update.bin`、`build/Kiikoread_Metalio_S31.bin`、bootloader / partition-table / ota_data_initial。
源码归档：排除 `build/`、`managed_components/`、`*.log`、`__pycache__`，保留
`main components boards tools docs assets cmake licenses .github` + 根目录源码与 `Metalio-S31-update.bin`。

---

## 6. 本地功能清单（合并后应仍存在，用来点检）

夜间模式极性（`epdiy.h` / `render_context.h` / 两板 `render.c` 与 `render_context.c` / `ui_kit` / `app_sleep` / `sleep.c` / `app_loop`）·
亮度上限与浮点下限（`metalio_hw.c/.h` / `ui_quick_brightness.h` / `ui_quick_menu_input.c` / `app_device_settings.c`）·
连点合并跳页（`cst836u` / `app_reading.c` / `app_loop.c`）· 状态栏章内页码随翻页刷新（`ui_nav.c/.h` / `app_book.c`）·
同版本卡刷（`ota_update.c/.h` / `metalio_ota_host_test.c`）· TXT 目录与上传页 `/file` 路由（`book_txt.c` / `read_pico_transfer.c` / `upload.html`）·
DU14 独立速读档（`display_reader_du14_fast_text_waveform` / `display_reader_du14_fast_text_prepare` /
`display_reader_waveform.c` + `.h` + `metalio_du14_host_test.py`）· 深睡唤醒证据链（`sleep.c` 里
`app_lock_resolve_wake_mark` / `app_lock_deep_wake_key` / NVS `s31wake/deep`，配 `metalio_boot_wake_host_test.py`）·
音乐与蓝牙（`music_player` / `bt_host` / `bt_trace`）· 本地独有文件 `main/app/turn_trace.c/.h`、
`components/epdiy/src/pie_pixels.h`。

### 6.1 rc108 的具体取舍（下次遇到同类冲突照这个判据）

| 文件 | 上游 rc108 改了什么 | 本地是什么 | 结论 |
|---|---|---|---|
| `display_reader_waveform.c` | 普通翻页整块重写：GL37+六相白(43) → full GL48 + 四相白 + 三相释放(55) | 只在文件里**新增** DU14 两个函数，不碰普通翻页 | 取 theirs，再把 DU14 两段贴回（本地增量 39 行一字未改） |
| `main/display_reader_waveform.h` | 只改 `soft_turn` 的两行注释 | 新增 DU14 声明 + 改 `fast_ripple` 注释 | 注释取 theirs，本地声明保留 |
| `app_main.c` | `retain_lock` 删掉 `app_settings_wake_presses()==2` 例外 | 另加了 `!deep_key_wake &&`（深睡标记豁免） | 两者合并：保留 `!deep_key_wake`，同时采纳上游的删除 |
| `sleep.c` | 门禁同一处放宽（长按开机直进） | **整段重写**（NVS 标记 + `key_seen` 证据链），语义已是上游超集 | 保留本地，不落上游这段 |
| 三份测试 / `docs/BOARDS.md` | 随 43→55 相更新 | 两份测试与 BOARDS 本地没动；`metalio_boot_wake_host_test.py` 本地整体重写 | 前者直接取 theirs；后者保留本地，按上游原意补一条「双击设置+长按开机→放行」断言 |

### 6.2 rc109 的具体取舍（大版本：179 改动 + 57 新增、0 删除）

- **先量后合**：`diff -rq` 得增量，再用 `git merge-file -p --diff3`（本地=current / 上一版上游=base / 新版=other）。
  本次分类 **122 TAKE / 59 MERGE（35 零冲突自动 + 24 人工）/ 410 NEW**（多数是 `read_pico_webp` 内 vendored libwebp）。
- **交互/渲染层一律保本地**：`app_book.c`（双击全屏、状态栏/页脚并入差分、批量书架）、`book_reader_input.h`
  （单击分区+双击全屏）、`app_device_settings.c`（七大类分类页）、`app_files.c`（曲库/目录设置/移动/格式化）、
  `book_toc.c`（目录速读黑白）、`sleep.c`（深睡证据链）、`app_main.c`（深睡豁免，另补首次引导重定向）。
- **纯新增取上游**：新组件 `kiiko_locale` / `read_pico_webp`、`app_onboarding.c`、`file_search.c/.h`、
  `book_added.c/.h`、`setup_state.c/.h`、`ui_*_layout.h`、`book_cover.*`（WebP/GIF）、`book_layout.*`
  （纯新增接口）、`ui_online_fonts.c`（i18n）。
- **图标表并集**：上游 +7 / 本地 +3（music/mic/mic-off），enum 与 mask 数组同序扩展，`gen_ui_icons.py` 列表同步。
- **依赖**：`idf_component.yml` 上游 + 本地音频/语音依赖；`dependencies.lock` 保留本地 `cezap` 组件并加
  `lowzag`，`target` 保 `esp32s31`。
- **验证**：`rc_upgrade_verify.py` **LOST=0**；release 编译 `METALIO_EXITCODE=0` / `COMBINED_EXIT=0`，
  产物 `Metalio-S31-update.bin` 6,015,728 B、内嵌版本 `0.3.3-rc109.1`。

#### 6.2.1 二轮：用户裁决后改为采用上游的三块（教训）

首轮按「交互冲突一律保本地」处理后，用户逐条裁决，把三块改成**采用上游**。结论：**遇到交互层冲突，不要自己拍板保本地，先出取舍表让用户选**。

1. **阅读手势**：`book_reader_input.h` 直接取上游（中间 28%~72% 全高竖条单击=全屏，两侧单击翻页，插图外侧仍双击缩放）；
   `app_book.c` 在返回/收藏判定之后插「中间竖条 → `toggle_reader_fullscreen`」，**必须排在工具栏判定之前**；
   `reader_tools_swipe()` 改签名 `app_redraw_t(..., bool *handled)`，底边上滑=全屏先退出再开工具栏；
   `reader_turn_chrome()` 只收面板、不再强制回全屏。默认仍全屏（`s_requested_fullscreen = true`）。
   → 中心点击被提前截获后，两侧单击不再等 340ms 双击窗口，翻页响应更跟手。
2. **管理书架**：整页换成上游 `book_manage_ui.h`（+`ui_book_manage_layout.h`），配套换掉
   `batch_rect / bulk_filter_rect / bulk_nav_* / batch_action / bulk_turn_page / bulk_finish / bulk_action / bulk_control_at`，
   新增 `bulk_sort_rect / shelf_order_mode / shelf_order_desc / draw_bulk_sort / bulk_select_row / shelf_added_load`，
   `shelf_entry_t` 加 `added`，`compare_books` 走 `s_shelf_order` 六态；`BOOK_BULK_ROWS` 与 `select_page` 一并删掉。
   删除本地 `draw_bulk`，改由头文件提供。**书架页封面样式不动**（仍 `SHELF_TEXT_ONLY_COVERS`）。
3. **文件管理**：上游重写 505 行、本地只 66 行 → **以上游为底再回贴本地**（本地上游都改同一区域时，这是唯一省力且不丢功能的做法）：
   回贴 `music_player.h`、五栏导航 `ui_nav_draw(fb,3)`、`file_action_rect` 三格、传输两卡、面板白底黑框、
   TF 卡三入口（书籍/字体/曲库）绘制与点击、`audio_ext()` 打开音频。
4. **机械项也别忘了**：`gen_ui_icons.py` 的图标名单要跟 `ui_icons.h` 并集同步，否则重跑生成器会丢本地图标（本次差点漏）。

