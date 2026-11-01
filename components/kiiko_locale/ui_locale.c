/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Flash只读字典二分查询，语言标记原子更新；无堆分配或设备I/O。
 * English: Binary lookup in a flash-only catalog, with an atomic locale flag; no heap or device IO.
 */
#include "ui_locale.h"
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_memory_utils.h"
#elif defined(KIIKO_LOCALE_TEST)
extern bool ui_locale_test_readonly(const void *text);
#endif
static atomic_bool s_english;
typedef struct {const char *zh,*en;} ui_locale_pair_t;
#include "ui_locale_catalog.inc"
void ui_locale_set_english(bool value){atomic_store_explicit(&s_english,value,memory_order_relaxed);}
bool ui_locale_english(void){return atomic_load_explicit(&s_english,memory_order_relaxed);}
const char *ui_tr(const char *text){
    if(!text||!ui_locale_english())return text;
#ifdef ESP_PLATFORM
    if(!esp_ptr_in_drom(text))return text;
#elif defined(KIIKO_LOCALE_TEST)
    if(!ui_locale_test_readonly(text))return text;
#else
    return text;
#endif
    size_t lo=0,hi=sizeof(s_pairs)/sizeof(s_pairs[0]);
    while(lo<hi){size_t mid=lo+(hi-lo)/2;int order=strcmp(text,s_pairs[mid].zh);
        if(!order)return s_pairs[mid].en;
        if(order<0)hi=mid;else lo=mid+1;
    }
    return text;
}
