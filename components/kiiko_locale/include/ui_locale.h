/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：共享系统文案语言，不读取或改写用户书名、正文、路径及网络名称。
 * English: Shared UI language; never rewrite user titles, text, paths or network names.
 */
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void ui_locale_set_english(bool english);
bool ui_locale_english(void);
/// 仅翻译只读固件字符串；RAM用户数据原样返回。/ Translate only readonly firmware literals; preserve RAM data.
const char *ui_tr(const char *literal);
#ifdef __cplusplus
}
#endif
