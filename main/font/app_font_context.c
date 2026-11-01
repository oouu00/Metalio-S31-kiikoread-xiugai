/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 用户修订：异常启动暂用内建字体，避免自动重载故障资源；不覆盖用户字体设置。
 * User revision: recovery starts use the built-in face without retrying faulty resources or overwriting saved font choices.
 * 中文：系统默认使用内建思源黑体子集，TF 卡黑体补足缺字；用户选择的字体覆盖默认外观。
 * English: The built-in Source Han Sans face is the default; the SD face fills missing glyphs and explicit choices override it.
 * 用户修订：票根始终借用系统字体，浅睡解锁恢复锁屏前的实际字体和上下文，不覆盖保存的选择。
 * User revision: tickets always borrow the system face; light-sleep wake restores the actual previous face/context without altering saved choices.
 */
#include "app_font_context.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "settings.h"
#include "boot_state.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_nav.h"
#if KIIKO_BOARD_METALIO47
#include <time.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "read_pico_sd.h"
#include "sleep.h"
#endif
#if KIIKO_BOARD_METALIO47
// 谁触发的字体激活：system/reading/retry/begin-lock/end-lock，回退瞬间 caller 一目了然。
// Which entry triggered the activation; the caller makes the instant of a fallback obvious.
static const char *s_caller = "-";
static bool s_in_retry;
#ifdef METALIO_DIAG_SD_LOGS
// 异常路径落卡诊断：深睡唤醒后字体回退的现场（io_ready/pending/SD/heap）一次看清。
// Diagnostics for abnormal paths: one line captures the whole wake-fallback scene.
// 仅 debug 档编入（与 sleep.c 的 power_trace 同一道 METALIO_DIAG_SD_LOGS 闸）。
// 高频路径（skip-not-ready/invalid-path/rejected-path）按 5s 节流，其余关键事件逐条落卡。
static void font_diag(const char *why, const char *path, esp_err_t err) {
    static int64_t s_diag_last_us;
    const bool noisy = why && (strcmp(why, "skip-not-ready") == 0 ||
                               strcmp(why, "invalid-path") == 0 ||
                               strcmp(why, "rejected-path") == 0);
    if (noisy && esp_timer_get_time() - s_diag_last_us < 5000000) return;
    s_diag_last_us = esp_timer_get_time();
    FILE *f = fopen("/sdcard/.metalio_s31/font_diag.log", "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[20];
    strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tmv);
    read_pico_sd_info_t info = {0};
    (void)read_pico_sd_get_info(&info);
    fprintf(f, "%s %s path=%s err=%s caller=%s io_ready=%d pending=%d sd_mounted=%d recovery=%d reasons=0x%02x "
        "psram_largest=%u internal_largest=%u\n",
        ts, why, path ? path : "-", esp_err_to_name(err),
        s_in_retry ? "retry" : s_caller,
        (int)read_pico_sd_io_ready(), (int)app_sleep_media_pending(),
        (int)info.mounted, (int)pico_boot_recovery(),
        pico_boot_recovery_reasons(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    fclose(f);
}
#else
#define font_diag(...) ((void)0)
#endif
#endif

static bool s_reading;
// 异常启动保护只拦「自动重载可能故障的字库」，绝不覆盖用户的手动选择：
// 手动加载并排版成功本身就是资源健康的证据，因此解除本会话的内建锁定，
// 避免 750ms 周期的 app_font_retry_active() 在 1 秒内把刚选好的 TTF 打回内建。
// The abnormal-start guard only blocks automatic reloads of a possibly faulty face; it never
// overrides an explicit user pick. A successful manual load proves the resource is healthy, so it
// lifts the session's built-in lock and the periodic retry can no longer undo the choice.
static bool s_manual_pick_lifted;
static bool recovery_font_locked(void) { return pico_boot_recovery() && !s_manual_pick_lifted; }
#if KIIKO_BOARD_METALIO47
#define FONT_CALLER(tag) (s_caller = (tag))
#else
#define FONT_CALLER(tag) ((void)0)
#endif
// Failed unchanged files are not repeatedly opened (opening unloads the active slot).
static char s_rejected[TTF_FONT_PATH_MAX];
static time_t s_rejected_mtime;
static off_t s_rejected_size;
#if KIIKO_BOARD_METALIO47
static int64_t s_rejected_retry_us;
static unsigned s_rejected_attempts;
#endif
static bool font_file(const char *path, struct stat *st) {
    return path && stat(path, st) == 0 && S_ISREG(st->st_mode) && st->st_size > 0;
}

static const char* system_path(char fallback[TTF_FONT_PATH_MAX]) {
    if (recovery_font_locked()) return TTF_FONT_BUILTIN;
#if KIIKO_BOARD_METALIO47
    // SD 未就绪时不能返回"当前路径"：activate(内建) 会把用户字体主动降级且难以自动恢复。
    // 返回保存的选择，交给 activate() 的未就绪保护（直接 return false，不碰 SD、不卸载旧槽）。
    // Never hand back the live path here: it demotes the saved face to built-in.
    if(app_sleep_media_pending() || !read_pico_sd_io_ready()) {
        const char* saved = app_settings_system_font_path();
        if (saved[0]) return saved;
        return ttf_font_path();
    }
#endif
    const char* chosen = app_settings_system_font_path();
    if (chosen[0]) return chosen;
    if (snprintf(fallback, TTF_FONT_PATH_MAX, "%s/Hei.ttf", app_settings_fonts_dir()) >= TTF_FONT_PATH_MAX)
        return TTF_FONT_BUILTIN;
    struct stat st;
    if (stat(fallback, &st) == 0 && S_ISREG(st.st_mode)) return fallback;
    if (!ttf_font_count()) ttf_font_scan();
    const ttf_font_item_t *item = ttf_font_item(0);
    return item && font_file(item->path, &st) ? item->path : TTF_FONT_BUILTIN;
}

static bool activate(const char* path) {
#if KIIKO_BOARD_METALIO47
    // 有意暂停TF时不stat/卸载旧槽；挂载恢复后沿原选择重试。
    // Do not stat/unload retained faces while media is intentionally paused; retry the saved choice after mount.
    if(!ttf_font_path_is_builtin(path) && (app_sleep_media_pending() || !read_pico_sd_io_ready())) {
        font_diag("skip-not-ready", path, ESP_OK);
        return false;
    }
    const bool had_builtin=ttf_font_ready()&&ttf_font_is_builtin();
#endif
    struct stat st = {0};
    bool builtin = ttf_font_path_is_builtin(path);
    bool valid = builtin || font_file(path, &st);
    bool rejected = !builtin && !strcmp(s_rejected, path) &&
        s_rejected_mtime == st.st_mtime && s_rejected_size == st.st_size;
#if KIIKO_BOARD_METALIO47
    // 手动加载成功即可解除旧拒绝；临时I/O或内存失败有界退避，不永久封锁同一文件。
    // A successful manual load clears old rejection; transient I/O/OOM failures back off without permanently blocking the file.
    if (valid && ttf_font_ready() && !strcmp(ttf_font_path(), path)) {
        if (!builtin) { s_rejected[0]=0; s_rejected_attempts=0; }
        return false;
    }
    if (rejected && s_rejected_retry_us && esp_timer_get_time() >= s_rejected_retry_us)
        rejected=false;
    // 真正的切换尝试与"被迫回内建"的原因逐条落卡：手动切换成功后被谁改回内建，一眼定位。
    if (ttf_font_ready() && strcmp(ttf_font_path(), path)) font_diag("activate", path, ESP_OK);
    if (!valid) font_diag("invalid-path", path, ESP_OK);
    else if (rejected) font_diag("rejected-path", path, ESP_OK);
#endif
    if (!valid || rejected) path = TTF_FONT_BUILTIN;
    if (ttf_font_ready() && !strcmp(ttf_font_path(), path)) return false;
#if KIIKO_BOARD_METALIO47
    esp_err_t err=ttf_font_open(path);
    if (err == ESP_OK) {
        font_diag("open-ok", path, ESP_OK);
#else
    if (ttf_font_open(path) == ESP_OK) {
#endif
        if (!ttf_font_path_is_builtin(path)) s_rejected[0] = 0;
#if KIIKO_BOARD_METALIO47
        if (!ttf_font_path_is_builtin(path)) s_rejected_attempts=0;
#endif
        return true;
    }
    if (!ttf_font_path_is_builtin(path)) {
        font_diag("open-fail", path, err);
#if KIIKO_BOARD_METALIO47
        if (strcmp(s_rejected,path) || s_rejected_mtime!=st.st_mtime || s_rejected_size!=st.st_size)
            s_rejected_attempts=0;
        static const unsigned retry_ms[]={1000,3000,10000,30000};
        unsigned index=s_rejected_attempts<3?s_rejected_attempts:3;
        if(s_rejected_attempts<4)++s_rejected_attempts;
        s_rejected_retry_us=err==ESP_ERR_INVALID_RESPONSE?0:
            esp_timer_get_time()+(int64_t)retry_ms[index]*1000;
#endif
        snprintf(s_rejected, sizeof(s_rejected), "%s", path);
        s_rejected_mtime = st.st_mtime;
        s_rejected_size = st.st_size;
    }
#if KIIKO_BOARD_METALIO47
    if (ttf_font_ready() && ttf_font_is_builtin()) return !had_builtin;
#else
    if (ttf_font_ready() && ttf_font_is_builtin()) return false;
#endif
    return ttf_font_open_builtin() == ESP_OK;
}

bool app_font_activate_system(void) {
    FONT_CALLER("system");
    s_reading = false;
    ttf_set_stroke_adjust(0);
    ttf_set_antialias(true);
    const char* chosen = app_settings_system_font_path();
    char fallback[TTF_FONT_PATH_MAX];
    const char* path = system_path(fallback);
    bool changed = activate(path);
    // 未选字体时由内建思源黑体绘制界面；TTF 仍保留完整字库供文件名缺字回退。
    // Built-in Source Han Sans draws default UI; the full TTF remains loaded for missing glyphs.
    ui_text_set_system_font(!recovery_font_locked() && chosen[0] && ttf_font_ready() &&
                           !strcmp(chosen, ttf_font_path()) && !ttf_font_is_builtin());
    ui_text_set_system_scale(true);
    ui_nav_status_font_context(false, changed);
    return changed;
}

bool app_font_activate_reading(void) {
    FONT_CALLER("reading");
    s_reading = true;
    ttf_set_stroke_adjust_fine(app_settings_book_weight_fine());
    ttf_set_antialias(app_settings_book_antialias());
    const char* saved = recovery_font_locked() ? TTF_FONT_BUILTIN : app_settings_font_path();
    bool changed = activate(ttf_font_path_is_builtin(saved) ? TTF_FONT_BUILTIN : saved);
    ui_text_set_system_font(false);
    ui_text_set_system_scale(false);
    ui_nav_status_font_context(true, changed);
    return changed;
}

bool app_font_retry_active(void) {
#if KIIKO_BOARD_METALIO47
    s_in_retry = true;
#endif
    bool changed = s_reading ? app_font_activate_reading() : app_font_activate_system();
#if KIIKO_BOARD_METALIO47
    s_in_retry = false;
#endif
    return changed;
}
#if KIIKO_BOARD_METALIO47
bool app_font_recover_media(void) {
    s_rejected[0] = 0;
    s_rejected_attempts = 0;
    s_rejected_retry_us = 0;
    return app_font_retry_active();
}
#endif

void app_font_begin_lock(app_lock_font_t *saved) {
    if (!saved) return;
    FONT_CALLER("begin-lock");
    snprintf(saved->path, sizeof(saved->path), "%s", ttf_font_path());
    saved->reading = s_reading;
    app_font_activate_system();
}

void app_font_end_lock(const app_lock_font_t *saved) {
    if (!saved) return;
    FONT_CALLER("end-lock");
#if KIIKO_BOARD_METALIO47
#else
    (void)activate(saved->path);
#endif
    s_reading = saved->reading;
#if KIIKO_BOARD_METALIO47
    const char *restore=saved->path;
    // 暂时退回内建不能覆盖阅读意图；锁屏前未加载成功的TF字体在解锁时再尝试。
    // A temporary built-in fallback does not replace reader intent; retry the selected TF face on unlock.
    if (recovery_font_locked()) restore=TTF_FONT_BUILTIN;
    else if(s_reading && ttf_font_path_is_builtin(restore)) restore=app_settings_font_path();
    if(ttf_font_path_is_builtin(restore)) restore=TTF_FONT_BUILTIN;
    if ((ttf_font_path_is_builtin(restore)||read_pico_sd_io_ready()) &&
        (!ttf_font_ready() || strcmp(restore, ttf_font_path())) && !activate(restore) && !ttf_font_is_builtin())
        (void)ttf_font_open_builtin();
#else
#endif
    ttf_set_stroke_adjust_fine(s_reading ? app_settings_book_weight_fine() : 0);
    ttf_set_antialias(!s_reading || app_settings_book_antialias());
    const char *chosen = app_settings_system_font_path();
    ui_text_set_system_font(!s_reading && !recovery_font_locked() && chosen[0] &&
                           !strcmp(chosen, ttf_font_path()) && !ttf_font_is_builtin());
    ui_text_set_system_scale(!s_reading);
    ui_nav_status_font_context(s_reading, false);
}

// 手动切换等关键节点落卡标记；release 档/非 metalio 板为空操作。
// Key milestones such as manual switching; no-op in release builds and on other boards.
void app_font_diag_note(const char *why, const char *path) {
#if KIIKO_BOARD_METALIO47
    font_diag(why, path, ESP_OK);
#else
    (void)why; (void)path;
#endif
}

// 用户手动选定并成功加载/保存字体后调用：解除本会话的内建锁定。
// 保护只用于「自动重载可能故障的字库」，手动成功加载即证明资源健康，不能被周期重试打回。
// Call after the user's explicit choice loaded and persisted successfully: lift the session's
// built-in lock. The guard only governs automatic reloads, and a successful manual load is proof
// the resource is healthy, so the periodic retry must not undo the choice.
void app_font_manual_committed(void) {
    s_manual_pick_lifted = true;
}
