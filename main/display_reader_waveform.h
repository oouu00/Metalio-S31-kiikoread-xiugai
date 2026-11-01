/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "epdiy.h"
#include <stdbool.h>

/// 柔和文字序列：白底到中灰的少量回浅脉冲提前；具体阅读模式由板级出口选择。
/// Soft text phases advance a few gray-edge settling pulses; board output selects the reading modes that use them.
const EpdWaveform* display_reader_text_waveform(void);
#if !KIIKO_BOARD_METALIO47
/// Pico reading ripple retains its gray sequence plus four changed-white phases before settling.
const EpdWaveform* display_reader_water_waveform(void);
#endif

#include "epd_highlevel.h"
/// Pico速读保留原厂DU；S31保留所选DU原序，定稿前追加四相目标白，使用批量模式别名。
/// Pico FastBW retains native DU; S31 retains the selected DU order, inserts four white-target phases before settling and aliases the batch mode.
const EpdWaveform* display_reader_fast_text_waveform(void);
/// 两板普通翻页分别派生本板波形：Pico等灰保持、变白补四相；S31沿Moku GL37补六相目标白。
/// Ordinary uses board-native profiles: Pico holds equal grays and adds four changed-white phases; S31 retains Moku GL37 plus six white-target phases.
const EpdWaveform* display_reader_soft_turn_waveform(void);
#if KIIKO_BOARD_METALIO47
/// 普通翻页·快（rc107 行为）：GL37 差分页，比完整GL48少约12相；由阅读设置“普通翻页速度”选择。
/// Ordinary-fast (rc107): GL37 diff page, ~12 phases fewer than full GL48; chosen by the reader "turn speed" setting.
const EpdWaveform* display_reader_soft_turn_fast_waveform(void);
#endif
/// 只归一文字区域目标，保留真实back和区域外灰阶。/ Normalize only the text target, retaining real history and outside grays.
bool display_reader_fast_text_prepare(EpdiyHighlevelState* hl,EpdRect area);
#if KIIKO_BOARD_METALIO47
/// S31独立DU14档：14相加四相目标白共18相，与速读DU20档互不覆盖。/ S31 DU14 mode:14 phases plus four white targets (18), independent of FastBW DU20.
const EpdWaveform* display_reader_du14_fast_text_waveform(void);
/// 与速读相同只归一文字目标。/ Normalizes the same text target as FastBW.
bool display_reader_du14_fast_text_prepare(EpdiyHighlevelState* hl,EpdRect area);
#endif

#if KIIKO_BOARD_METALIO47
/// 两个阅读设置面板的显式本地四灰实验，默认false。/ Explicit local four-gray experiment for the two primary reader settings; false by default.
bool display_reader_fast_panels_enabled(void);
/// UI目标映射0/5/10/15，近白控件底色保留浅灰，不改区域外正文/历史帧。
/// Map UI targets to0/5/10/15,retaining near-white control fills as light gray and outside body/history pixels.
bool display_reader_panel_gray4_prepare(uint8_t* target,EpdRect area);
/// 两个面板四灰实验：进入时26相统一重建中灰（含等灰），反馈16相；关闭实验返回NULL。
/// Four-gray panels:26-phase uniform midgray entry including equal pairs,16-phase feedback; NULL when disabled.
const EpdWaveform* display_reader_panel_gray4_waveform(bool erase_underlay);
/// 普通/水波纹共用的22相默认启用；只有显式关闭构建才回退GL48。/ Shared Ordinary/Ripple 22 phases are on by default; an explicit OFF build falls back to GL48.
bool display_reader_fast_ripple_enabled(void);
#endif
