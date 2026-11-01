/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：无人值守自测入口。仅 KIIKO_AUTO_TEST 构建有效：开机在「热点」与「蓝牙耳机」
 * 两个相位之间轮换，每相位跑完写 NVS 并重启，便于人不在场时只接串口看日志。
 *
 * English: unattended self-test entry. KIIKO_AUTO_TEST builds only: alternate the
 * hotspot and Bluetooth-earphone phases across reboots and record progress in NVS,
 * so a console log alone is enough when nobody is around.
 *
 * 冻结：不改变交付固件行为；相位状态只写私有 NVS 命名空间。
 * Frozen: never affects delivery builds; phase state stays in a private NVS namespace.
 */
#pragma once

#include <stdbool.h>

/// 板级初始化前调用：按自测需要保留 USJ 控制台并确定本轮相位。
/// Call before board init: retain the USJ console for the test and settle this boot's phase.
void app_auto_test_prepare(void);

/// 本机是否要以传书页（热点）作为开机首屏。/ Whether this boot should open the Transfer page for the hotspot phase.
bool app_auto_test_transfer_page_wanted(void);

/// 读取/推进相位并启动自测任务；在 app_loop_run 之前调用。/ Read/advance the phase and start the test task before app_loop_run.
void app_auto_test_start(void);
