/**
 * @file simple_ftp_server.h
 * @brief FTP-сервер для ESP-IDF (порт SimpleFTPServer / Mischianti).
 *
 * Сеть: lwIP sockets. Файлы: POSIX VFS (SPIFFS / LittleFS / FAT / SD —
 * смонтируй сам до старта, укажи base_path).
 *
 * Основано на: https://github.com/xreef/SimpleFTPServer (MIT)
 * Автор оригинала: Renzo Mischianti — www.mischianti.org
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIMPLE_FTP_SERVER_VERSION "1.0.0"

#define SIMPLE_FTP_DEFAULT_CMD_PORT   21
#define SIMPLE_FTP_DEFAULT_PASV_PORT  50009
#define SIMPLE_FTP_DEFAULT_BUF_SIZE   2048
#define SIMPLE_FTP_DEFAULT_TIMEOUT_S  30
#define SIMPLE_FTP_DEFAULT_AUTH_S     10

/** События сессии (как FtpOperation в Arduino-библиотеке) */
typedef enum {
    SIMPLE_FTP_EVT_CONNECT = 0,
    SIMPLE_FTP_EVT_DISCONNECT,
    SIMPLE_FTP_EVT_FREE_SPACE_CHANGE,
} simple_ftp_event_t;

/** События передачи файла (как FtpTransferOperation) */
typedef enum {
    SIMPLE_FTP_XFER_UPLOAD_START = 0,
    SIMPLE_FTP_XFER_UPLOAD,
    SIMPLE_FTP_XFER_DOWNLOAD_START,
    SIMPLE_FTP_XFER_DOWNLOAD,
    SIMPLE_FTP_XFER_STOP,
    SIMPLE_FTP_XFER_ERROR,
} simple_ftp_xfer_event_t;

typedef void (*simple_ftp_event_cb_t)(simple_ftp_event_t evt,
                                      uint32_t free_bytes,
                                      uint32_t total_bytes,
                                      void *user_ctx);

typedef void (*simple_ftp_xfer_cb_t)(simple_ftp_xfer_event_t evt,
                                     const char *name,
                                     uint32_t transferred,
                                     void *user_ctx);

/**
 * @brief Конфиг сервера.
 *
 * base_path — корень VFS, который видит FTP как "/".
 * Пример: "/spiffs", "/littlefs", "/sdcard".
 * WiFi/Ethernet и mount FS — снаружи (как SD.begin + WiFi в Arduino).
 */
typedef struct {
    const char *user;              /**< логин; NULL → "anonymous" */
    const char *pass;              /**< пароль; NULL → "anonymous" */
    const char *base_path;         /**< корень FS; NULL → "/" */
    const char *welcome;           /**< текст в 220; NULL → дефолт */
    uint16_t cmd_port;             /**< 0 → 21 */
    uint16_t pasv_port;            /**< 0 → 50009 */
    uint32_t local_ip;             /**< host-order IPv4 для PASV; 0 = взять с netif */
    size_t buf_size;               /**< 0 → 2048 */
    uint16_t timeout_s;            /**< 0 → 30 */
    uint16_t auth_timeout_s;       /**< 0 → 10 */
    bool anonymous;                /**< true: USER anonymous без пароля */
    int task_priority;             /**< 0 → 5 */
    uint32_t task_stack;           /**< 0 → 8192 */
    simple_ftp_event_cb_t event_cb;
    simple_ftp_xfer_cb_t xfer_cb;
    void *user_ctx;
} simple_ftp_config_t;

typedef struct simple_ftp_handle_s *simple_ftp_handle_t;

#define SIMPLE_FTP_CONFIG_DEFAULT()              \
    {                                            \
        .user = "user",                          \
        .pass = "password",                      \
        .base_path = "/",                        \
        .welcome = "Welcome to Simply FTP server", \
        .cmd_port = SIMPLE_FTP_DEFAULT_CMD_PORT, \
        .pasv_port = SIMPLE_FTP_DEFAULT_PASV_PORT, \
        .local_ip = 0,                           \
        .buf_size = SIMPLE_FTP_DEFAULT_BUF_SIZE, \
        .timeout_s = SIMPLE_FTP_DEFAULT_TIMEOUT_S, \
        .auth_timeout_s = SIMPLE_FTP_DEFAULT_AUTH_S, \
        .anonymous = false,                      \
        .task_priority = 5,                      \
        .task_stack = 8192,                      \
        .event_cb = NULL,                        \
        .xfer_cb = NULL,                         \
        .user_ctx = NULL,                        \
    }

esp_err_t simple_ftp_create(const simple_ftp_config_t *cfg, simple_ftp_handle_t *out);
esp_err_t simple_ftp_start(simple_ftp_handle_t h);
esp_err_t simple_ftp_stop(simple_ftp_handle_t h);
esp_err_t simple_ftp_destroy(simple_ftp_handle_t h);

esp_err_t simple_ftp_set_credentials(simple_ftp_handle_t h, const char *user, const char *pass);
esp_err_t simple_ftp_set_local_ip(simple_ftp_handle_t h, uint32_t ip_host_order);
bool simple_ftp_is_running(simple_ftp_handle_t h);

#ifdef __cplusplus
}
#endif
