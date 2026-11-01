/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：构造系统信息前翻译固件格式与字符串参数；数值类型和RAM用户数据保持不变。
 * English: Translate firmware formats/string arguments before constructing UI messages; preserve numeric types and RAM user data.
 */
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "ui_locale.h"
// _Generic只求值选中分支一次；未选中的数值转换不执行。/ Only the selected branch is evaluated once; numeric casts in unselected branches never execute.
#define UI_LOCALE_ARG(value) _Generic((value), char*:ui_tr((const char*)(uintptr_t)(value)), const char*:ui_tr((const char*)(uintptr_t)(value)), default:(value))
#define UI_LOCALE_JOIN_(a,b) a##b
#define UI_LOCALE_JOIN(a,b) UI_LOCALE_JOIN_(a,b)
#define UI_LOCALE_NARGS_(_0,_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,N,...) N
#define UI_LOCALE_NARGS(...) UI_LOCALE_NARGS_(_,##__VA_ARGS__,12,11,10,9,8,7,6,5,4,3,2,1,0)
#define UI_LOCALE_PRINTF0(dst, cap, fmt) (snprintf)((dst), (cap), ui_tr(fmt), 0)
#define UI_LOCALE_PRINTF1(dst, cap, fmt, a0) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0))
#define UI_LOCALE_PRINTF2(dst, cap, fmt, a0, a1) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1))
#define UI_LOCALE_PRINTF3(dst, cap, fmt, a0, a1, a2) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1), UI_LOCALE_ARG(a2))
#define UI_LOCALE_PRINTF4(dst, cap, fmt, a0, a1, a2, a3) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1), UI_LOCALE_ARG(a2), UI_LOCALE_ARG(a3))
#define UI_LOCALE_PRINTF5(dst, cap, fmt, a0, a1, a2, a3, a4) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1), UI_LOCALE_ARG(a2), UI_LOCALE_ARG(a3), UI_LOCALE_ARG(a4))
#define UI_LOCALE_PRINTF6(dst, cap, fmt, a0, a1, a2, a3, a4, a5) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1), UI_LOCALE_ARG(a2), UI_LOCALE_ARG(a3), UI_LOCALE_ARG(a4), UI_LOCALE_ARG(a5))
#define UI_LOCALE_PRINTF7(dst, cap, fmt, a0, a1, a2, a3, a4, a5, a6) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1), UI_LOCALE_ARG(a2), UI_LOCALE_ARG(a3), UI_LOCALE_ARG(a4), UI_LOCALE_ARG(a5), UI_LOCALE_ARG(a6))
#define UI_LOCALE_PRINTF8(dst, cap, fmt, a0, a1, a2, a3, a4, a5, a6, a7) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1), UI_LOCALE_ARG(a2), UI_LOCALE_ARG(a3), UI_LOCALE_ARG(a4), UI_LOCALE_ARG(a5), UI_LOCALE_ARG(a6), UI_LOCALE_ARG(a7))
#define UI_LOCALE_PRINTF9(dst, cap, fmt, a0, a1, a2, a3, a4, a5, a6, a7, a8) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1), UI_LOCALE_ARG(a2), UI_LOCALE_ARG(a3), UI_LOCALE_ARG(a4), UI_LOCALE_ARG(a5), UI_LOCALE_ARG(a6), UI_LOCALE_ARG(a7), UI_LOCALE_ARG(a8))
#define UI_LOCALE_PRINTF10(dst, cap, fmt, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1), UI_LOCALE_ARG(a2), UI_LOCALE_ARG(a3), UI_LOCALE_ARG(a4), UI_LOCALE_ARG(a5), UI_LOCALE_ARG(a6), UI_LOCALE_ARG(a7), UI_LOCALE_ARG(a8), UI_LOCALE_ARG(a9))
#define UI_LOCALE_PRINTF11(dst, cap, fmt, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1), UI_LOCALE_ARG(a2), UI_LOCALE_ARG(a3), UI_LOCALE_ARG(a4), UI_LOCALE_ARG(a5), UI_LOCALE_ARG(a6), UI_LOCALE_ARG(a7), UI_LOCALE_ARG(a8), UI_LOCALE_ARG(a9), UI_LOCALE_ARG(a10))
#define UI_LOCALE_PRINTF12(dst, cap, fmt, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) (snprintf)((dst), (cap), ui_tr(fmt), UI_LOCALE_ARG(a0), UI_LOCALE_ARG(a1), UI_LOCALE_ARG(a2), UI_LOCALE_ARG(a3), UI_LOCALE_ARG(a4), UI_LOCALE_ARG(a5), UI_LOCALE_ARG(a6), UI_LOCALE_ARG(a7), UI_LOCALE_ARG(a8), UI_LOCALE_ARG(a9), UI_LOCALE_ARG(a10), UI_LOCALE_ARG(a11))
#define UI_LOCALE_DISPATCH(dst,cap,fmt,...) UI_LOCALE_JOIN(UI_LOCALE_PRINTF,UI_LOCALE_NARGS(__VA_ARGS__))(dst,cap,fmt,##__VA_ARGS__)
// 先展开IP2STR等多参数宏，再计数分派。/ Expand multi-argument macros such as IP2STR before counting.
#define snprintf(...) UI_LOCALE_DISPATCH(__VA_ARGS__)
#define strlcpy(dst,src,cap) (strlcpy)((dst),ui_tr(src),(cap))
