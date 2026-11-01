/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 板级开机装配：墨水屏、SD 探测、加速度计、PMU、触摸。
 *
 * Board bring-up: EPD, SD probe, accelerometer, PMU, touch.
 */

#include "read_pico_init.h"

#include "epdiy.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_board.h"
#include "read_pico_epd_timing.h"
#include "e0470_epaper_waveform.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "read_pico_crashlog.h"
#include "metalio_hw.h"

static const char* TAG = "read_pico_init";
extern bool epd_pixel_order_selftest(const EpdWaveform *waveform);

#define READ_PICO_PCLK_MHZ 20
#define READ_PICO_VCOM_MV 1200
#define READ_PICO_ACCEL_INT1 GPIO_NUM_NC

// read_pico_init() 只发复位脉冲，真正的识别推迟到擦白之后，配置留在这里。
// read_pico_init() only pulses reset; identification is deferred until after the whitening and
// reuses this saved configuration.
static cst836u_config_t touch_config;

// FT6336U 退出复位需要自举时间；过早碰 I2C 会把芯片拖死（实测 5ms 就探，1.5s 都不再应答）。
// 原先这段等待由 946ms 的同步擦白顺带盖住；擦白改并发后必须显式守住，否则识别会在
// 复位后几十毫秒就开始探。擦白仍是关键路径（946ms 对首帧准备 ~700ms），这段等待藏在里面。
// FT6336U needs boot time after reset; talking to it too early wedges the chip (probing at 5ms
// left it silent for 1.5s in a measured run). The old synchronous 946ms whitening used to cover
// that wait; now that the whitening runs concurrently the window must be explicit, or the probe
// starts tens of milliseconds after reset. The whitening is still the critical path (946ms vs
// ~700ms of first-frame preparation), so this wait stays hidden.
#define READ_PICO_TOUCH_SETTLE_MS 300
static int64_t s_touch_reset_us;

esp_err_t read_pico_init(read_pico_handle_t* hw) {
    if (hw == NULL) return ESP_ERR_INVALID_ARG;
    *hw = (read_pico_handle_t){ 0 };

    ESP_LOGI(TAG, "%s", READ_PICO_PRODUCT_NAME);
    ESP_LOGI(TAG, "Initializing 684x1216 EPD");
    // S31 用 RISC-V C 查表和 1 KiB LUT。/ S31 uses the RISC-V C lookup and a 1 KiB LUT.
    epd_init(&epd_board_read_pico, &E0470_DISPLAY, EPD_LUT_1K);
    // S31 无独立 CD：通过卡通信探测。/ S31 has no dedicated card detect; probe card I/O.
    read_pico_sd_start_probe();
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
    // 保持参考 76 的 20MHz 时序，光学效果待本机验证。
    // Keep reference 76 at 20MHz; optical behavior awaits this device's test.
    read_pico_epd_set_pclk(READ_PICO_PCLK_MHZ);
    esp_log_level_set("epdiy", ESP_LOG_WARN);
    epd_set_vcom(READ_PICO_VCOM_MV);

    // 默认灰阶表是开机从原厂表裁出来的，必须先算好再挂给 highlevel；
    // 差分刷新跳过整屏都在保持的前导相位。
    // / Default gray table is trimmed from the vendor table at boot; build it
    // before highlevel. Diff refresh skips leading all-hold phases.
    e0470_waveform_init();
    if (!epd_pixel_order_selftest(&E0470_WAVEFORM)) {
        ESP_LOGE(TAG, "Metalio pixel order/waveform self-test failed");
        return ESP_FAIL;
    }
    epd_set_leading_skip(false);
    hw->hl = epd_hl_init(&E0470_WAVEFORM);
    hw->framebuffer = epd_hl_get_framebuffer(&hw->hl);
    if (hw->framebuffer == NULL) {
        ESP_LOGE(TAG, "EPD framebuffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    rp_boot_mark("epd");

    epd_poweron();
    rp_boot_mark("rail");
    if (!metalio_tps_ready()) { epd_poweroff(); return ESP_FAIL; }
    // 校验通过后保持升压轨上电：紧接着的开机图擦白还要用它，省掉一次 TPS 上电时序（实测约 93ms）。
    // Keep the rails up after the check: the boot whitening needs them next, saving one TPS
    // power-up sequence (~93ms measured). Later stages power down through rails_keepalive().

    sc7a20h_config_t acc = {
        .i2c_addr = SC7A20H_ADDR_DEFAULT,
        .int1_gpio = READ_PICO_ACCEL_INT1,
        .sensor = SC7A20H_CONFIG_IDLE,
    };
    if (sc7a20h_init(read_pico_i2c_bus(), &acc, &hw->sensor) == ESP_OK) {
        // V1.5的INT1与电源键共享GPIO2；仅采样，不路由加速度中断。
        // V1.5 INT1 shares GPIO2 with keys; keep the reference polling-only policy.
        esp_err_t irq_err=sc7a20h_shared_irq_polling(hw->sensor);
        if (irq_err!=ESP_OK) {
            sc7a20h_deinit(hw->sensor); hw->sensor=NULL;
            ESP_LOGW(TAG,"SC7A20H IRQ disable failed: %s",esp_err_to_name(irq_err));
        }
    }
    if (hw->sensor) {
        sc7a20h_sample_t sample;
        if (read_pico_accel_read(hw->sensor, &sample) == ESP_OK) {
            ESP_LOGI(
                TAG, "SC7A20H: X=%dmg Y=%dmg Z=%dmg",
                sample.x_mg, sample.y_mg, sample.z_mg
            );
        }
        sc7a20h_version(hw->sensor, &hw->sensor_whoami, &hw->sensor_version);
        sc7a20h_shared_irq_idle(hw->sensor);
        hw->sensor_ready = true;
    } else {
        ESP_LOGW(TAG, "SC7A20H not ready");
    }
    rp_boot_mark("acc");

    read_pico_pmu_config_t pmu = READ_PICO_PMU_CONFIG_DEFAULT();
    hw->pmu_ready = read_pico_pmu_init(read_pico_i2c_bus(), &pmu) == ESP_OK;
    if (!hw->pmu_ready) {
        ESP_LOGW(TAG, "Metalio TCA9555 power service not ready");
    }
    rp_boot_mark("pmu");

    cst836u_config_t tp = {
        .i2c_addr = CST836U_ADDR_DEFAULT,
        .scl_speed_hz = CST836U_I2C_HZ_DEFAULT,
        .int_gpio = (gpio_num_t)READ_PICO_TP_INT_GPIO,
        .rst_gpio = GPIO_NUM_NC,
        // 复位钩子必须登记：FT6336U 只能靠 RST 脉冲退出深睡（写 0xA5=0 唤不醒）。
        // 之前给 NULL，锁屏浅睡把触摸压进深睡后 cst836u_wake 直接 INVALID_STATE 返回，
        // 表现为唤醒后画面正常、触屏全死，只能重启恢复。
        // The reset hook must be registered: FT6336U only leaves deep sleep through an RST pulse
        // (writing 0xA5=0 cannot wake it). With NULL, cst836u_wake returned INVALID_STATE after a
        // lock light-sleep put the chip to sleep, so a wake painted fine but never saw touch.
        .reset_fn = read_pico_touch_reset,
        // 开机这条路径已在下面发过脉冲，init 若再打一次就要重新等芯片自举（>300ms），
        // 实测直接导致「FT6336U not ready」，所以这次只登记钩子供唤醒使用。
        // Boot already pulses RST below, and a second pulse makes init wait out another >300 ms boot
        // (measured as "FT6336U not ready"), so the hook is registered for wake only.
        .no_boot_reset = true,
    };
    touch_config = tp;
    (void)read_pico_touch_reset();
    s_touch_reset_us = esp_timer_get_time();
    rp_boot_mark("tp");

    return ESP_OK;
}

// 擦白之后再识别触摸：等够芯片自举时间再碰 I2C，第一读就能应答。
// Identify the touch chip after the whitening starts, once the chip has had its boot time, so
// the first read answers.
esp_err_t read_pico_touch_init(read_pico_handle_t *hw) {
    if (hw == NULL) return ESP_ERR_INVALID_ARG;
    if (hw->touch != NULL) { hw->touch_ready = true; return ESP_OK; }
    if (s_touch_reset_us > 0) {
        const int64_t settled = (esp_timer_get_time() - s_touch_reset_us) / 1000;
        if (settled < READ_PICO_TOUCH_SETTLE_MS)
            vTaskDelay(pdMS_TO_TICKS(READ_PICO_TOUCH_SETTLE_MS - settled));
    }
    if (cst836u_init(read_pico_i2c_bus(), &touch_config, &hw->touch) == ESP_OK) {
        hw->touch_ready = true;
        ESP_LOGI(TAG,"FT6336U ready, chip=0x64, cover keys Y1600");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "FT6336U not ready");
    return ESP_FAIL;
}

void read_pico_deinit(read_pico_handle_t* hw) {
    if (hw == NULL) return;
    if (hw->touch != NULL) {
        (void)cst836u_deinit(hw->touch);
        hw->touch = NULL;
    }
    if (hw->pmu_ready) {
        (void)read_pico_pmu_deinit();
        hw->pmu_ready = false;
    }
    if (hw->sensor != NULL) {
        (void)sc7a20h_deinit(hw->sensor);
        hw->sensor = NULL;
    }
    epd_deinit();
    *hw = (read_pico_handle_t){ 0 };
}

void read_pico_sensor_wake(sc7a20h_handle_t h) {
    if (h && !sc7a20h_powered(h)) {
        sc7a20h_power_up(h);
    }
}

void read_pico_sensor_sleep(sc7a20h_handle_t h) {
    if (h && sc7a20h_powered(h)) {
        sc7a20h_shared_irq_idle(h);
    }
}

void read_pico_accel_to_device(sc7a20h_sample_t* sample) {
    if (sample == NULL) return;
    int16_t xc = sample->x_mg;
    int16_t yc = sample->y_mg;
    int16_t zc = sample->z_mg;
    sample->x_mg = (int16_t)(-yc);
    sample->y_mg = (int16_t)(-xc);
    sample->z_mg = (int16_t)(-zc);
    xc = sample->x_raw;
    yc = sample->y_raw;
    zc = sample->z_raw;
    sample->x_raw = (int16_t)(-yc);
    sample->y_raw = (int16_t)(-xc);
    sample->z_raw = (int16_t)(-zc);
}

uint8_t read_pico_accel_map_aoi(uint8_t src) {
    uint8_t out = (uint8_t)(src & 0x40);
    if (src & 0x01) out |= 0x08;
    if (src & 0x02) out |= 0x04;
    if (src & 0x04) out |= 0x02;
    if (src & 0x08) out |= 0x01;
    if (src & 0x10) out |= 0x20;
    if (src & 0x20) out |= 0x10;
    return out;
}

esp_err_t read_pico_accel_read(sc7a20h_handle_t h, sc7a20h_sample_t* sample) {
    esp_err_t err = sc7a20h_read(h, sample);
    if (err == ESP_OK) read_pico_accel_to_device(sample);
    return err;
}
