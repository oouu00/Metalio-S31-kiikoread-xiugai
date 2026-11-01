/* SPDX-License-Identifier: Apache-2.0
 * 中文：FT6336U 转为 RC90 事件，后台计数跨刷屏等待的短按以合并翻页。
 * English: FT6336U feeds RC90 events; count short taps across display updates to merge turns.
 */
#include "cst836u.h"
#include "metalio_hw.h"
#include "metalio_touch_decode.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
struct cst836u_dev_t {
    cst836u_config_t cfg; cst836u_mode_t mode; TaskHandle_t task;
    portMUX_TYPE lock; cst836u_touch_t sample,press; uint16_t presses; esp_err_t error;
    SemaphoreHandle_t io_lock;
    bool irq_ready;
};
static void touch_isr(void *arg) {
    cst836u_handle_t h=arg;
    BaseType_t higher=pdFALSE;
    if (h->task) vTaskNotifyGiveFromISR(h->task,&higher);
    if (higher) portYIELD_FROM_ISR();
}
static esp_err_t sample_ft(cst836u_handle_t h,cst836u_touch_t *t) {
    memset(t,0,sizeof(*t));
    esp_err_t err=metalio_read(0x38,0x00,t->raw,sizeof(t->raw));
    if (err!=ESP_OK) return err;
    unsigned count=0;
    metalio_touch_point_t points[2];
    if (!metalio_touch_decode(t->raw,sizeof(t->raw),points,&count)) return ESP_ERR_INVALID_RESPONSE;
    for (unsigned i=0;i<count;i++) {
        t->points[t->count++]=(cst836u_point_t){.active=true,.id=points[i].id,
            .event=points[i].event,.x=points[i].x,.y=points[i].y};
    }
    t->touched=t->count>0;
    if (t->touched) { t->x=t->points[0].x; t->y=t->points[0].y; }
    (void)h; return ESP_OK;
}
static void feed(void *arg) {
    cst836u_handle_t h=arg;
    for (;;) {
        cst836u_touch_t t={0};
        // 模式切换与采样串行，不能在写入深睡后又发出一次I2C读取。
        // Serialize sampling and mode changes: never read I2C after committing touch sleep.
        xSemaphoreTake(h->io_lock,portMAX_DELAY);
        bool normal=h->mode==CST836U_MODE_NORMAL;
        esp_err_t err=normal?sample_ft(h,&t):ESP_OK;
        portENTER_CRITICAL(&h->lock);
        bool was_down=h->sample.touched;
        if (err==ESP_OK) {
            if (t.touched && !was_down) { h->press=t; ++h->presses; }
            h->sample=t;
        }
        h->error=err;
        portEXIT_CRITICAL(&h->lock);
        xSemaphoreGive(h->io_lock);
        if (err==ESP_OK && t.touched && !was_down) {
            unsigned rx=((t.raw[3]&15)<<8)|t.raw[4], ry=((t.raw[5]&15)<<8)|t.raw[6];
            ESP_LOGI("metalio_touch","press raw=(%u,%u) screen=(%u,%u)",rx,ry,t.x,t.y);
        }
        // 原厂irq+feed：空闲等GPIO5下降沿，按住/INT低/读取失败才10ms续采。
        // Vendor irq+feed: wait for GPIO5 while idle; poll at 10ms for contact, low IRQ or read retry.
        bool retry=normal && (t.touched || err!=ESP_OK || !h->irq_ready ||
                    gpio_get_level(h->cfg.int_gpio)==0);
        // 空闲等待给 1s 上限而不是无限：中断一旦在浅睡后失效，无限等待会让触摸彻底没反应，
        // 有上限就至少每秒自采一次。本工程没开自动低功耗，1s 一次唤醒不构成额外耗电。
        // Cap the idle wait at 1 s instead of waiting forever: if the INT stops firing after a
        // light sleep, an infinite wait kills touch completely, while a cap self-samples every
        // second. Automatic power management is off here, so this costs nothing measurable.
        ulTaskNotifyTake(pdTRUE,retry?pdMS_TO_TICKS(10):pdMS_TO_TICKS(1000));
    }
}
esp_err_t cst836u_init(i2c_master_bus_handle_t bus,const cst836u_config_t *cfg,cst836u_handle_t *out) {
    (void)bus;
    if (!cfg || !out) return ESP_ERR_INVALID_ARG;
    cst836u_handle_t h=calloc(1,sizeof(*h)); if (!h) return ESP_ERR_NO_MEM;
    h->lock=(portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED; h->cfg=*cfg; h->error=ESP_OK;
    h->io_lock=xSemaphoreCreateMutex();
    if (!h->io_lock) { free(h); return ESP_ERR_NO_MEM; }
    esp_err_t err=ESP_OK;
    if (cfg->reset_fn && !cfg->no_boot_reset && (err=cfg->reset_fn())!=ESP_OK) goto fail;
    uint8_t id=0;
    // 复位后 FT6336U 通常十几毫秒就能应答，固定等 300ms 全算在冷启动里。
    // 改成短轮询：读到 0x64 立刻往下走，最坏情况仍等满原来的 300ms 上限。
    // FT6336U normally answers within tens of ms of reset, so the old fixed 300ms wait was pure
    // cold-boot cost. Poll instead: continue as soon as id reads 0x64, capped at that same 300ms.
    err=ESP_ERR_TIMEOUT;
    for (unsigned poll=0;poll<30;poll++) {
        esp_err_t rd=metalio_read(0x38,0xa3,&id,1);
        if (rd==ESP_OK&&id==0x64) { err=ESP_OK; break; }
        if (rd!=ESP_OK) err=rd;
        if (poll+1<30) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err!=ESP_OK) goto fail;
    err=metalio_write8(0x38,0xa4,0); if (err!=ESP_OK) goto fail;
    uint8_t fw=0,id2=0;
    esp_err_t fw_err=metalio_read(0x38,0xa6,&fw,1);
    esp_err_t id2_err=metalio_read(0x38,0x9f,&id2,1);
    // 只读识别；未确认供应商模组型号时绝不擦写触控芯片。
    // Read-only identification: never erase touch firmware without a confirmed supplier module.
    ESP_LOGI("metalio_touch","FT6336U id=%02x fw=%02x(%d) secondary=%02x(%d) Y offset=0",id,fw,(int)fw_err,id2,(int)id2_err);
    if (cfg->int_gpio!=GPIO_NUM_NC) {
        gpio_config_t irq={.pin_bit_mask=1ULL<<cfg->int_gpio,.mode=GPIO_MODE_INPUT,
            .pull_up_en=GPIO_PULLUP_ENABLE,.intr_type=GPIO_INTR_NEGEDGE};
        esp_err_t setup=gpio_config(&irq);
        if (setup==ESP_OK) {
            setup=gpio_install_isr_service(0);
            if (setup==ESP_OK || setup==ESP_ERR_INVALID_STATE)
                setup=gpio_isr_handler_add(cfg->int_gpio,touch_isr,h);
        }
        h->irq_ready=setup==ESP_OK;
        if (!h->irq_ready) ESP_LOGW("metalio_touch","IRQ unavailable (%d); using 10ms polling",(int)setup);
    }
    if (xTaskCreate(feed,"ft6336_feed",3072,h,5,&h->task)!=pdPASS) {
        err=ESP_ERR_NO_MEM; goto fail;
    }
    *out=h; return ESP_OK;
fail:
    if (h->irq_ready) { gpio_intr_disable(cfg->int_gpio); gpio_isr_handler_remove(cfg->int_gpio); }
    vSemaphoreDelete(h->io_lock); free(h); return err;
}
esp_err_t cst836u_read(cst836u_handle_t h,cst836u_touch_t *out) {
    if (!h || !out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock); *out=h->presses?h->press:h->sample;
    out->presses=h->presses; h->presses=0;
    esp_err_t err=h->error; portEXIT_CRITICAL(&h->lock); return err;
}
esp_err_t cst836u_deinit(cst836u_handle_t h) {
    if (h) {
        if (h->irq_ready) { gpio_intr_disable(h->cfg.int_gpio); gpio_isr_handler_remove(h->cfg.int_gpio); }
        vTaskDelete(h->task); vSemaphoreDelete(h->io_lock); free(h);
    }
    return ESP_OK;
}
esp_err_t cst836u_get_info(cst836u_handle_t h,cst836u_info_t *i) {
    if (!h || !i) return ESP_ERR_INVALID_ARG;
    memset(i,0,sizeof(*i)); esp_err_t err=metalio_read(0x38,0xa3,&i->id,1);
    if (err==ESP_OK) err=metalio_read(0x38,0xa6,&i->fw,1);
    i->type=0x6336; return err;
}
esp_err_t cst836u_set_mode(cst836u_handle_t h,cst836u_mode_t mode) {
    if (!h) return ESP_ERR_INVALID_ARG;
    if (mode!=CST836U_MODE_NORMAL && mode!=CST836U_MODE_DEEPSLEEP) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(h->io_lock,portMAX_DELAY);
    esp_err_t err=metalio_write8(0x38,0xa5,mode==CST836U_MODE_DEEPSLEEP?3:0);
    if (err==ESP_OK) {
        portENTER_CRITICAL(&h->lock);
        h->mode=mode; h->sample=(cst836u_touch_t){0}; h->presses=0;
        portEXIT_CRITICAL(&h->lock);
    }
    xSemaphoreGive(h->io_lock);
    if (err==ESP_OK) xTaskNotifyGive(h->task);
    return err;
}
cst836u_mode_t cst836u_get_mode(cst836u_handle_t h) {
    if (!h) return CST836U_MODE_DEEPSLEEP;
    portENTER_CRITICAL(&h->lock); cst836u_mode_t mode=h->mode; portEXIT_CRITICAL(&h->lock); return mode;
}
esp_err_t cst836u_reset(cst836u_handle_t h) {
    return cst836u_wake(h);
}
// 打一次复位脉冲并等芯片自举应答。掉电后的 FT6336U 自举实测长于 300ms，轮询窗口给到 600ms。
// Pulse reset once and wait for the chip to answer; a cold FT6336U boots in more than 300 ms, so
// the poll window is 600 ms.
static esp_err_t wake_once(cst836u_handle_t h) {
    esp_err_t err=h->cfg.reset_fn();
    if (err!=ESP_OK) return err;
    err=ESP_ERR_TIMEOUT;
    for (unsigned poll=0;poll<120;poll++) {
        uint8_t id=0;
        if (metalio_read(0x38,0xa3,&id,1)==ESP_OK && id==0x64) { err=ESP_OK; break; }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (err!=ESP_OK) return err;
    err=metalio_write8(0x38,0xa4,0);
    if (err!=ESP_OK) return err;
    portENTER_CRITICAL(&h->lock);
    h->mode=CST836U_MODE_NORMAL; h->sample=(cst836u_touch_t){0};h->presses=0;
    portEXIT_CRITICAL(&h->lock);
    return ESP_OK;
}
esp_err_t cst836u_wake(cst836u_handle_t h) {
    if (!h || !h->cfg.reset_fn) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(h->io_lock,portMAX_DELAY);
    esp_err_t err=ESP_FAIL;
    // 掉电唤醒的第一次脉冲常落在电源轨稳定之前，最多补两次，中间给芯片一点喘息。
    // After a power-up the first pulse often lands before the rail settles; allow two more with
    // a short breather in between.
    for (unsigned attempt=0;attempt<3 && err!=ESP_OK;attempt++) {
        err=wake_once(h);
        if (err!=ESP_OK) vTaskDelay(pdMS_TO_TICKS(60));
    }
    // 浅睡前后 GPIO 被整批重配过，INT 下降沿可能不再触发；重挂一次 ISR，失败就退回 10ms
    // 轮询——feed 不会停在一个永远不来的通知上（表现就是画面正常、触屏全死）。
    // Sleep reconfigures GPIOs wholesale, so the INT edge can stop firing. Re-hook the ISR and
    // fall back to 10 ms polling if that fails, so feed never parks on a notification that never
    // arrives (which reads as a normal screen with a dead touch panel).
    if (err==ESP_OK && h->irq_ready) {
        gpio_intr_disable(h->cfg.int_gpio);
        (void)gpio_isr_handler_remove(h->cfg.int_gpio);
        if (gpio_isr_handler_add(h->cfg.int_gpio,touch_isr,h)!=ESP_OK) {
            h->irq_ready=false;
            ESP_LOGW("metalio_touch","INT re-hook failed; falling back to 10ms polling");
        }
    }
    if (err!=ESP_OK) ESP_LOGW("metalio_touch","wake failed: %s",esp_err_to_name(err));
    xSemaphoreGive(h->io_lock);
    if (err==ESP_OK) xTaskNotifyGive(h->task);
    return err;
}
int cst836u_int_level(cst836u_handle_t h) { return h?gpio_get_level(h->cfg.int_gpio):1; }
bool cst836u_int_asserted(cst836u_handle_t h) { return cst836u_int_level(h)==0; }
gpio_num_t cst836u_int_gpio(cst836u_handle_t h) { return h?h->cfg.int_gpio:GPIO_NUM_NC; }
const char *cst836u_mode_name(cst836u_mode_t mode) { return mode==CST836U_MODE_DEEPSLEEP?"sleep":"normal"; }
const char *cst836u_event_name(uint8_t event) { return event==0?"down":event==1?"up":event==2?"contact":"reserved"; }
