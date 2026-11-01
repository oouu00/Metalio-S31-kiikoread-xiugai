/**
 * @file simple_ftp_server.c
 * @brief ESP-IDF FTP server — порт логики SimpleFTPServer на lwIP + VFS.
 *
 * Based on Jean-Michel Gallego / Renzo Mischianti SimpleFTPServer (MIT).
 * Reference: www.mischianti.org
 */

#include "simple_ftp_server.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "simple_ftp";

#define FTP_CMD_SIZE  263
#define FTP_CWD_SIZE  263
#define FTP_CRED_SIZE 64
#define FTP_NAME_SIZE 200

typedef enum {
    FTP_STOP = 0,
    FTP_INIT,
    FTP_WAIT_CLIENT,
    FTP_USER,
    FTP_PASS,
    FTP_CMD,
} ftp_cmd_stage_t;

typedef enum {
    XFER_CLOSE = 0,
    XFER_RETRIEVE,
    XFER_STORE,
    XFER_LIST,
    XFER_NLST,
    XFER_MLSD,
} ftp_xfer_stage_t;

typedef enum {
    DATA_NONE = 0,
    DATA_PASV,
    DATA_ACTIVE,
} ftp_data_mode_t;

struct simple_ftp_handle_s {
    char user[FTP_CRED_SIZE];
    char pass[FTP_CRED_SIZE];
    char base_path[FTP_CWD_SIZE];
    char welcome[128];
    uint16_t cmd_port;
    uint16_t pasv_port;
    uint32_t local_ip;
    size_t buf_size;
    uint16_t timeout_s;
    uint16_t auth_timeout_s;
    bool anonymous;
    int task_priority;
    uint32_t task_stack;
    simple_ftp_event_cb_t event_cb;
    simple_ftp_xfer_cb_t xfer_cb;
    void *user_ctx;

    TaskHandle_t task;
    volatile bool running;
    volatile bool stop_req;
    SemaphoreHandle_t lock;

    int listen_fd;
    int pasv_listen_fd;
    int ctrl_fd;
    int data_fd;

    ftp_cmd_stage_t cmd_stage;
    ftp_xfer_stage_t xfer_stage;
    ftp_data_mode_t data_mode;

    char cwd[FTP_CWD_SIZE];
    char rnfr[FTP_CWD_SIZE];
    char cmd_line[FTP_CMD_SIZE];
    char command[5];
    char *parameter;
    uint16_t i_cl;
    bool rnfr_cmd;
    uint16_t data_port;
    uint8_t data_ip[4];
    uint32_t restart_pos;
    uint32_t bytes_xfer;
    int64_t deadline_ms;
    int64_t delay_until_ms;

    FILE *file;
    DIR *dir;
    uint8_t *buf;
    char xfer_name[FTP_NAME_SIZE];
};

/* ---------- time helpers ---------- */

static int64_t now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void fire_event(simple_ftp_handle_t h, simple_ftp_event_t e, uint32_t free_b, uint32_t total_b)
{
    if (h->event_cb) {
        h->event_cb(e, free_b, total_b, h->user_ctx);
    }
}

static void fire_xfer(simple_ftp_handle_t h, simple_ftp_xfer_event_t e, const char *name, uint32_t n)
{
    if (h->xfer_cb) {
        h->xfer_cb(e, name ? name : "", n, h->user_ctx);
    }
}

static void fs_space(simple_ftp_handle_t h, uint32_t *free_b, uint32_t *total_b)
{
    struct statvfs st;
    *free_b = 0;
    *total_b = 0;
    if (statvfs(h->base_path, &st) == 0) {
        uint64_t tot = (uint64_t)st.f_blocks * (uint64_t)st.f_frsize;
        uint64_t fr = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
        *total_b = tot > UINT32_MAX ? UINT32_MAX : (uint32_t)tot;
        *free_b = fr > UINT32_MAX ? UINT32_MAX : (uint32_t)fr;
    }
}

/* ---------- sockets ---------- */

static void sock_close(int *fd)
{
    if (fd && *fd >= 0) {
        shutdown(*fd, SHUT_RDWR);
        close(*fd);
        *fd = -1;
    }
}

static int sock_listen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    // 控制连接排队：客户端重连/多拨时不被直接拒绝（热点只允许1个客户端，队列只是抗抖动）。
    // Control backlog: survive reconnect bursts; the hotspot still admits one client at a time.
    if (listen(fd, 5) < 0) {
        close(fd);
        return -1;
    }
    /* non-blocking accept */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static void sock_set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static ssize_t sock_send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

static void reply_raw(simple_ftp_handle_t h, const char *s)
{
    if (h->ctrl_fd < 0 || !s) {
        return;
    }
    sock_send_all(h->ctrl_fd, s, strlen(s));
}

static void reply_line(simple_ftp_handle_t h, const char *fmt, ...)
{
    char buf[FTP_CWD_SIZE + 80];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
    if (h->ctrl_fd >= 0) {
        sock_send_all(h->ctrl_fd, buf, len);
    }
}

static uint32_t detect_local_ip(simple_ftp_handle_t h)
{
    if (h->local_ip != 0) {
        return h->local_ip;
    }
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    }
    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    }
    if (!netif) {
        return 0;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return 0;
    }
    return ntohl(info.ip.addr);
}

/* ---------- path / VFS ---------- */

static bool legal_char(char c)
{
    if (c == '"' || c == '*' || c == '?' || c == ':' || c == '<' || c == '>' || c == '|') {
        return false;
    }
    return (unsigned char)c > 0x1f;
}

/** FTP path "/" + relative → absolute under base_path. Prevents escaping base. */
static bool resolve_path(simple_ftp_handle_t h, const char *ftp_rel, char *out, size_t out_sz)
{
    char combined[FTP_CWD_SIZE * 2];
    const char *rel = ftp_rel ? ftp_rel : "/";

    if (rel[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", rel);
    } else {
        if (strcmp(h->cwd, "/") == 0) {
            snprintf(combined, sizeof(combined), "/%s", rel);
        } else {
            snprintf(combined, sizeof(combined), "%s/%s", h->cwd, rel);
        }
    }

    /* normalize: collapse //, resolve . and .. */
    char norm[FTP_CWD_SIZE];
    char *parts[64];
    int nparts = 0;
    char tmp[FTP_CWD_SIZE];
    strncpy(tmp, combined, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;

    for (char *tok = strtok(tmp, "/"); tok; tok = strtok(NULL, "/")) {
        if (strcmp(tok, ".") == 0) {
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            if (nparts > 0) {
                nparts--;
            }
            continue;
        }
        if (nparts < 64) {
            parts[nparts++] = tok;
        }
    }

    norm[0] = '/';
    norm[1] = 0;
    for (int i = 0; i < nparts; i++) {
        size_t len = strlen(norm);
        if (len > 1) {
            strncat(norm, "/", sizeof(norm) - len - 1);
        }
        strncat(norm, parts[i], sizeof(norm) - strlen(norm) - 1);
    }

    /* map FTP root → base_path */
    if (strcmp(norm, "/") == 0) {
        snprintf(out, out_sz, "%s", h->base_path);
    } else {
        if (strcmp(h->base_path, "/") == 0) {
            snprintf(out, out_sz, "%s", norm);
        } else {
            snprintf(out, out_sz, "%s%s", h->base_path, norm);
        }
    }
    return true;
}

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static bool make_ftp_path(simple_ftp_handle_t h, char *ftp_out, size_t sz, const char *param)
{
    if (!param || !*param) {
        reply_raw(h, "501 No file name\r\n");
        return false;
    }
    for (const char *p = param; *p; p++) {
        if (!legal_char(*p) && *p != '/' && *p != '\\') {
            reply_raw(h, "553 File name not allowed\r\n");
            return false;
        }
    }
    if (param[0] == '/') {
        strncpy(ftp_out, param, sz - 1);
    } else if (strcmp(h->cwd, "/") == 0) {
        snprintf(ftp_out, sz, "/%s", param);
    } else {
        snprintf(ftp_out, sz, "%s/%s", h->cwd, param);
    }
    ftp_out[sz - 1] = 0;
    return true;
}

static bool have_parameter(simple_ftp_handle_t h)
{
    if (!h->parameter || !*h->parameter) {
        reply_raw(h, "501 No file name\r\n");
        return false;
    }
    return true;
}

static bool cmd_is(simple_ftp_handle_t h, const char *c)
{
    return strcasecmp(h->command, c) == 0;
}

static bool param_is(simple_ftp_handle_t h, const char *p)
{
    return h->parameter && strcasecmp(h->parameter, p) == 0;
}

/* ---------- transfer helpers ---------- */

static void abort_transfer(simple_ftp_handle_t h)
{
    if (h->file) {
        fclose(h->file);
        h->file = NULL;
        fire_xfer(h, SIMPLE_FTP_XFER_ERROR, h->xfer_name, h->bytes_xfer);
    }
    if (h->dir) {
        closedir(h->dir);
        h->dir = NULL;
    }
    sock_close(&h->data_fd);
    h->xfer_stage = XFER_CLOSE;
    h->data_mode = DATA_NONE;
}

static void close_transfer(simple_ftp_handle_t h)
{
    if (h->file) {
        fclose(h->file);
        h->file = NULL;
    }
    if (h->dir) {
        closedir(h->dir);
        h->dir = NULL;
    }
    sock_close(&h->data_fd);
    h->data_mode = DATA_NONE;
    h->xfer_stage = XFER_CLOSE;
    reply_raw(h, "226 Transfer complete\r\n");
    fire_xfer(h, SIMPLE_FTP_XFER_STOP, h->xfer_name, h->bytes_xfer);
}

static bool data_connect(simple_ftp_handle_t h, bool out150)
{
    sock_close(&h->data_fd);

    if (h->data_mode == DATA_PASV) {
        int64_t start = now_ms();
        while (h->data_fd < 0 && (now_ms() - start) < 10000) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int fd = accept(h->pasv_listen_fd, (struct sockaddr *)&peer, &plen);
            if (fd >= 0) {
                h->data_fd = fd;
                sock_set_nodelay(fd);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else if (h->data_mode == DATA_ACTIVE) {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (fd >= 0) {
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(h->data_port);
            addr.sin_addr.s_addr = htonl(((uint32_t)h->data_ip[0] << 24) |
                                         ((uint32_t)h->data_ip[1] << 16) |
                                         ((uint32_t)h->data_ip[2] << 8) |
                                         (uint32_t)h->data_ip[3]);
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                h->data_fd = fd;
                sock_set_nodelay(fd);
            } else {
                close(fd);
            }
        }
    }

    if (h->data_fd < 0) {
        reply_raw(h, "425 No data connection\r\n");
        return false;
    }
    if (out150) {
        reply_line(h, "150 Accepted data connection to port %u\r\n", (unsigned)h->data_port);
    }
    return true;
}

static bool data_ok(simple_ftp_handle_t h)
{
    if (h->data_fd >= 0) {
        return true;
    }
    h->data_mode = DATA_NONE;
    h->xfer_stage = XFER_CLOSE;
    if (h->ctrl_fd >= 0) {
        reply_raw(h, "426 Data connection closed. Transfer aborted\r\n");
    }
    return false;
}

/* ---------- LIST / transfer loops ---------- */

static void format_mtime(time_t t, char *out, size_t sz)
{
    struct tm tm;
    localtime_r(&t, &tm);
    /* Unix LIST style: "Jan 01 12:00" or "Jan 01  2024" */
    time_t now = time(NULL);
    if (difftime(now, t) > 180 * 24 * 3600.0 || difftime(t, now) > 0) {
        strftime(out, sz, "%b %d  %Y", &tm);
    } else {
        strftime(out, sz, "%b %d %H:%M", &tm);
    }
}

static void format_mdtm(time_t t, char *out, size_t sz)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, sz, "%04d%02d%02d%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static bool do_list_one(simple_ftp_handle_t h, bool nlst)
{
    if (!data_ok(h) || !h->dir) {
        if (h->dir) {
            closedir(h->dir);
            h->dir = NULL;
        }
        return false;
    }

    struct dirent *de = readdir(h->dir);
    if (!de) {
        close_transfer(h);
        return false;
    }
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
        return true;
    }

    char vfs[FTP_CWD_SIZE];
    char ftp_child[FTP_CWD_SIZE];
    if (strcmp(h->cwd, "/") == 0) {
        snprintf(ftp_child, sizeof(ftp_child), "/%s", de->d_name);
    } else {
        snprintf(ftp_child, sizeof(ftp_child), "%s/%s", h->cwd, de->d_name);
    }
    resolve_path(h, ftp_child, vfs, sizeof(vfs));

    struct stat st;
    if (stat(vfs, &st) != 0) {
        return true;
    }

    char line[512];
    if (nlst) {
        snprintf(line, sizeof(line), "%s\r\n", de->d_name);
    } else {
        char tbuf[32];
        format_mtime(st.st_mtime, tbuf, sizeof(tbuf));
        if (S_ISDIR(st.st_mode)) {
            snprintf(line, sizeof(line), "drwxrwsr-x\t2\t%s\t%ld\t%s\t%s\r\n",
                     h->user, 4096L, tbuf, de->d_name);
        } else {
            snprintf(line, sizeof(line), "-rw-rw-r--\t1\t%s\t%ld\t%s\t%s\r\n",
                     h->user, (long)st.st_size, tbuf, de->d_name);
        }
    }
    if (sock_send_all(h->data_fd, line, strlen(line)) < 0) {
        abort_transfer(h);
        return false;
    }
    return true;
}

static bool do_mlsd_one(simple_ftp_handle_t h)
{
    if (!data_ok(h) || !h->dir) {
        if (h->dir) {
            closedir(h->dir);
            h->dir = NULL;
        }
        return false;
    }
    struct dirent *de = readdir(h->dir);
    if (!de) {
        close_transfer(h);
        return false;
    }
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
        return true;
    }

    char vfs[FTP_CWD_SIZE];
    char ftp_child[FTP_CWD_SIZE];
    if (strcmp(h->cwd, "/") == 0) {
        snprintf(ftp_child, sizeof(ftp_child), "/%s", de->d_name);
    } else {
        snprintf(ftp_child, sizeof(ftp_child), "%s/%s", h->cwd, de->d_name);
    }
    resolve_path(h, ftp_child, vfs, sizeof(vfs));
    struct stat st;
    if (stat(vfs, &st) != 0) {
        return true;
    }
    char mdtm[20];
    format_mdtm(st.st_mtime, mdtm, sizeof(mdtm));
    char line[512];
    if (S_ISDIR(st.st_mode)) {
        snprintf(line, sizeof(line), "Type=dir;Modify=%s; %s\r\n", mdtm, de->d_name);
    } else {
        snprintf(line, sizeof(line), "Type=file;Modify=%s;Size=%ld; %s\r\n",
                 mdtm, (long)st.st_size, de->d_name);
    }
    if (sock_send_all(h->data_fd, line, strlen(line)) < 0) {
        abort_transfer(h);
        return false;
    }
    return true;
}

static bool do_retrieve(simple_ftp_handle_t h)
{
    if (!data_ok(h) || !h->file) {
        if (h->file) {
            fclose(h->file);
            h->file = NULL;
        }
        return false;
    }
    if (h->restart_pos > 0) {
        if (fseek(h->file, (long)h->restart_pos, SEEK_SET) != 0) {
            reply_raw(h, "450 Cannot seek to restart position.\r\n");
            abort_transfer(h);
            return false;
        }
        h->bytes_xfer = h->restart_pos;
        h->restart_pos = 0;
    }

    size_t n = fread(h->buf, 1, h->buf_size, h->file);
    if (n > 0) {
        if (sock_send_all(h->data_fd, h->buf, n) < 0) {
            abort_transfer(h);
            return false;
        }
        h->bytes_xfer += (uint32_t)n;
        fire_xfer(h, SIMPLE_FTP_XFER_DOWNLOAD, h->xfer_name, h->bytes_xfer);
        return true;
    }
    close_transfer(h);
    return false;
}

static bool do_store(simple_ftp_handle_t h)
{
    if (!h->file) {
        return false;
    }
    if (h->data_fd < 0) {
        close_transfer(h);
        return false;
    }

    /* wait a bit for data if empty */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(h->data_fd, &rfds);
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    int sel = select(h->data_fd + 1, &rfds, NULL, NULL, &tv);
    if (sel == 0) {
        close_transfer(h);
        return false;
    }
    if (sel < 0) {
        abort_transfer(h);
        return false;
    }

    ssize_t n = recv(h->data_fd, h->buf, h->buf_size, 0);
    if (n == 0) {
        uint32_t free_b, total_b;
        fs_space(h, &free_b, &total_b);
        close_transfer(h);
        fire_event(h, SIMPLE_FTP_EVT_FREE_SPACE_CHANGE, free_b, total_b);
        return false;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        abort_transfer(h);
        return false;
    }
    size_t wr = fwrite(h->buf, 1, (size_t)n, h->file);
    if (wr != (size_t)n) {
        reply_raw(h, "552 Probably insufficient storage space\r\n");
        abort_transfer(h);
        return false;
    }
    h->bytes_xfer += (uint32_t)n;
    fire_xfer(h, SIMPLE_FTP_XFER_UPLOAD, h->xfer_name, h->bytes_xfer);
    return true;
}

/* ---------- session ---------- */

static void ini_variables(simple_ftp_handle_t h)
{
    h->data_port = h->pasv_port;
    h->data_mode = DATA_NONE;
    strcpy(h->cwd, "/");
    h->rnfr_cmd = false;
    h->rnfr[0] = 0;
    h->xfer_stage = XFER_CLOSE;
    h->restart_pos = 0;
    h->i_cl = 0;
    h->cmd_line[0] = 0;
}

static void client_connected(simple_ftp_handle_t h)
{
    ESP_LOGI(TAG, "client connected");
    reply_line(h, "220--- %s ---\r\n", h->welcome);
    reply_line(h, "220 --   Version %s (ESP-IDF)   --\r\n", SIMPLE_FTP_SERVER_VERSION);
    h->i_cl = 0;
    uint32_t free_b, total_b;
    fs_space(h, &free_b, &total_b);
    fire_event(h, SIMPLE_FTP_EVT_CONNECT, free_b, total_b);
}

static void disconnect_client(simple_ftp_handle_t h)
{
    ESP_LOGI(TAG, "disconnect client");
    abort_transfer(h);
    if (h->ctrl_fd >= 0) {
        reply_raw(h, "221 Goodbye\r\n");
    }
    uint32_t free_b, total_b;
    fs_space(h, &free_b, &total_b);
    fire_event(h, SIMPLE_FTP_EVT_DISCONNECT, free_b, total_b);
    sock_close(&h->ctrl_fd);
    sock_close(&h->data_fd);
}

static int32_t read_char(simple_ftp_handle_t h)
{
    if (h->ctrl_fd < 0) {
        return -1;
    }
    uint8_t c;
    ssize_t n = recv(h->ctrl_fd, &c, 1, MSG_DONTWAIT);
    if (n <= 0) {
        return -1;
    }
    if (c == '\r') {
        return -1;
    }
    if (c == '\n') {
        h->cmd_line[h->i_cl] = 0;
        /* parse COMMAND [param] */
        char *p = h->cmd_line;
        while (*p == ' ') {
            p++;
        }
        size_t i = 0;
        while (*p && *p != ' ' && i < 4) {
            h->command[i++] = (char)toupper((unsigned char)*p++);
        }
        h->command[i] = 0;
        while (*p == ' ') {
            p++;
        }
        h->parameter = (*p) ? p : NULL;
        uint16_t len = h->i_cl;
        h->i_cl = 0;
        return (int32_t)len;
    }
    if (h->i_cl < FTP_CMD_SIZE - 1) {
        h->cmd_line[h->i_cl++] = (char)c;
        return -1;
    }
    /* overflow */
    h->i_cl = 0;
    reply_raw(h, "500 Line too long\r\n");
    return -2;
}

static void process_command(simple_ftp_handle_t h)
{
    ESP_LOGD(TAG, "CMD %s [%s]", h->command, h->parameter ? h->parameter : "");

    if (cmd_is(h, "USER")) {
        if (h->anonymous && h->parameter && strcmp(h->parameter, h->user) == 0) {
            reply_raw(h, "230 Ok\r\n");
            h->cmd_stage = FTP_CMD;
        } else if (h->parameter && strcmp(h->parameter, h->user) == 0) {
            reply_raw(h, "331 Ok. Password required\r\n");
            strcpy(h->cwd, "/");
            h->cmd_stage = FTP_PASS;
        } else {
            reply_raw(h, "530 \r\n");
            h->cmd_stage = FTP_STOP;
        }
        return;
    }

    if (cmd_is(h, "PASS")) {
        if (h->cmd_stage != FTP_PASS) {
            reply_raw(h, "503 \r\n");
            h->cmd_stage = FTP_STOP;
            return;
        }
        if (h->parameter && strcmp(h->parameter, h->pass) == 0) {
            reply_raw(h, "230 Ok\r\n");
            h->cmd_stage = FTP_CMD;
        } else {
            reply_raw(h, "530 Wrong password!\r\n");
            h->cmd_stage = FTP_STOP;
        }
        return;
    }

    if (cmd_is(h, "FEAT")) {
        reply_raw(h,
                  "211-Extensions supported:\r\n"
                  " MLST type*;modify*;size*;\r\n"
                  " MLSD\r\n"
                  " MDTM\r\n"
                  " UTF8\r\n"
                  " SIZE\r\n"
                  " SITE FREE\r\n"
                  "211 End.\r\n");
        return;
    }
    if (cmd_is(h, "AUTH")) {
        reply_raw(h, "502 \r\n");
        return;
    }
    if (cmd_is(h, "SYST")) {
        reply_raw(h, "215 UNIX Type: L8\r\n");
        return;
    }
    if (cmd_is(h, "OPTS")) {
        if (param_is(h, "UTF8 ON") || param_is(h, "utf8 on")) {
            reply_raw(h, "200 OK, UTF8 ON\r\n");
        } else {
            reply_raw(h, "504 Unknown OPTS\r\n");
        }
        return;
    }

    if (h->cmd_stage < FTP_CMD) {
        reply_raw(h, "530 \r\n");
        h->cmd_stage = FTP_STOP;
        return;
    }

    if (cmd_is(h, "PWD") || (cmd_is(h, "CWD") && param_is(h, "."))) {
        reply_line(h, "257 \"%s\" is your current directory\r\n", h->cwd);
        return;
    }

    if (cmd_is(h, "CDUP") || (cmd_is(h, "CWD") && param_is(h, ".."))) {
        if (strlen(h->cwd) > 1) {
            size_t L = strlen(h->cwd);
            if (h->cwd[L - 1] == '/') {
                h->cwd[L - 1] = 0;
            }
            char *sep = strrchr(h->cwd, '/');
            if (sep && sep > h->cwd) {
                *sep = 0;
            } else {
                strcpy(h->cwd, "/");
            }
        }
        reply_line(h, "250 Ok. Current directory is %s\r\n", h->cwd);
        return;
    }

    if (cmd_is(h, "CWD")) {
        char ftp_path[FTP_CWD_SIZE];
        char vfs[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_path, sizeof(ftp_path), h->parameter)) {
            return;
        }
        resolve_path(h, ftp_path, vfs, sizeof(vfs));
        if (!path_is_dir(vfs)) {
            reply_line(h, "550 Can't change directory to %s\r\n", h->parameter);
            return;
        }
        strncpy(h->cwd, ftp_path, sizeof(h->cwd) - 1);
        h->cwd[sizeof(h->cwd) - 1] = 0;
        reply_line(h, "250 Directory changed to %s\r\n", h->cwd);
        return;
    }

    if (cmd_is(h, "QUIT")) {
        reply_raw(h, "221 Goodbye\r\n");
        disconnect_client(h);
        h->cmd_stage = FTP_STOP;
        return;
    }

    if (cmd_is(h, "MODE")) {
        reply_raw(h, param_is(h, "S") ? "200 S Ok\r\n" : "504 Only S(tream) is supported\r\n");
        return;
    }

    if (cmd_is(h, "PASV")) {
        sock_close(&h->data_fd);
        sock_close(&h->pasv_listen_fd);
        h->pasv_listen_fd = sock_listen(h->pasv_port);
        if (h->pasv_listen_fd < 0) {
            reply_raw(h, "425 Can't open data connection\r\n");
            return;
        }
        uint32_t ip = detect_local_ip(h);
        uint8_t a = (ip >> 24) & 0xff;
        uint8_t b = (ip >> 16) & 0xff;
        uint8_t c = (ip >> 8) & 0xff;
        uint8_t d = ip & 0xff;
        h->data_port = h->pasv_port;
        h->data_mode = DATA_PASV;
        reply_line(h, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
                   a, b, c, d, h->data_port >> 8, h->data_port & 255);
        return;
    }

    if (cmd_is(h, "PORT")) {
        sock_close(&h->data_fd);
        if (!have_parameter(h)) {
            return;
        }
        unsigned v[6];
        if (sscanf(h->parameter, "%u,%u,%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
            reply_raw(h, "501 Can't interpret parameters\r\n");
            return;
        }
        for (int i = 0; i < 4; i++) {
            h->data_ip[i] = (uint8_t)v[i];
        }
        h->data_port = (uint16_t)(256 * v[4] + v[5]);
        h->data_mode = DATA_ACTIVE;
        reply_raw(h, "200 PORT command successful\r\n");
        return;
    }

    if (cmd_is(h, "STRU")) {
        reply_raw(h, param_is(h, "F") ? "200 F Ok\r\n" : "504 Only F(ile) is supported\r\n");
        return;
    }
    if (cmd_is(h, "TYPE")) {
        if (param_is(h, "A")) {
            reply_raw(h, "200 TYPE is now ASCII\r\n");
        } else if (param_is(h, "I")) {
            reply_raw(h, "200 TYPE is now 8-bit binary\r\n");
        } else {
            reply_raw(h, "504 Unknown TYPE\r\n");
        }
        return;
    }

    if (cmd_is(h, "ABOR")) {
        abort_transfer(h);
        reply_raw(h, "226 Data connection closed\r\n");
        return;
    }

    if (cmd_is(h, "NOOP")) {
        reply_raw(h, "200 Zzz...\r\n");
        return;
    }

    if (cmd_is(h, "HELP")) {
        reply_raw(h,
                  "200 Commands: USER PASS CWD CDUP PWD QUIT NOOP MODE PASV PORT STRU TYPE\r\n"
                  " ABOR DELE LIST NLST MLSD MLST APPE RETR STOR MKD RMD RNFR RNTO\r\n"
                  " MDTM SIZE FEAT SITE REST SYST HELP\r\n");
        return;
    }

    if (cmd_is(h, "DELE")) {
        char ftp_path[FTP_CWD_SIZE], vfs[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_path, sizeof(ftp_path), h->parameter)) {
            return;
        }
        resolve_path(h, ftp_path, vfs, sizeof(vfs));
        if (!path_exists(vfs) || path_is_dir(vfs)) {
            reply_line(h, "550 %s not found\r\n", h->parameter);
            return;
        }
        if (unlink(vfs) == 0) {
            uint32_t free_b, total_b;
            fs_space(h, &free_b, &total_b);
            fire_event(h, SIMPLE_FTP_EVT_FREE_SPACE_CHANGE, free_b, total_b);
            reply_line(h, "250 Deleted %s\r\n", h->parameter);
        } else {
            reply_line(h, "450 Can't delete %s\r\n", h->parameter);
        }
        return;
    }

    if (cmd_is(h, "MKD")) {
        char ftp_path[FTP_CWD_SIZE], vfs[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_path, sizeof(ftp_path), h->parameter)) {
            return;
        }
        resolve_path(h, ftp_path, vfs, sizeof(vfs));
        if (mkdir(vfs, 0775) == 0 || errno == EEXIST) {
            reply_line(h, "257 \"%s\" created\r\n", ftp_path);
        } else {
            reply_line(h, "550 Can't create %s\r\n", h->parameter);
        }
        return;
    }

    if (cmd_is(h, "RMD")) {
        char ftp_path[FTP_CWD_SIZE], vfs[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_path, sizeof(ftp_path), h->parameter)) {
            return;
        }
        resolve_path(h, ftp_path, vfs, sizeof(vfs));
        if (rmdir(vfs) == 0) {
            reply_line(h, "250 \"%s\" removed\r\n", h->parameter);
        } else {
            reply_line(h, "550 Can't remove %s\r\n", h->parameter);
        }
        return;
    }

    if (cmd_is(h, "RNFR")) {
        char ftp_path[FTP_CWD_SIZE], vfs[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_path, sizeof(ftp_path), h->parameter)) {
            return;
        }
        resolve_path(h, ftp_path, vfs, sizeof(vfs));
        if (!path_exists(vfs)) {
            reply_raw(h, "550 File not found\r\n");
            return;
        }
        strncpy(h->rnfr, ftp_path, sizeof(h->rnfr) - 1);
        h->rnfr_cmd = true;
        reply_raw(h, "350 RNFR accepted - file exists, ready for destination\r\n");
        return;
    }

    if (cmd_is(h, "RNTO")) {
        if (!h->rnfr_cmd) {
            reply_raw(h, "503 Need RNFR before RNTO\r\n");
            return;
        }
        char ftp_to[FTP_CWD_SIZE], vfs_from[FTP_CWD_SIZE], vfs_to[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_to, sizeof(ftp_to), h->parameter)) {
            return;
        }
        resolve_path(h, h->rnfr, vfs_from, sizeof(vfs_from));
        resolve_path(h, ftp_to, vfs_to, sizeof(vfs_to));
        h->rnfr_cmd = false;
        if (rename(vfs_from, vfs_to) == 0) {
            reply_raw(h, "250 File successfully renamed or moved\r\n");
        } else {
            reply_raw(h, "451 Rename/move failure\r\n");
        }
        return;
    }

    if (cmd_is(h, "SIZE")) {
        char ftp_path[FTP_CWD_SIZE], vfs[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_path, sizeof(ftp_path), h->parameter)) {
            return;
        }
        resolve_path(h, ftp_path, vfs, sizeof(vfs));
        struct stat st;
        if (stat(vfs, &st) != 0 || S_ISDIR(st.st_mode)) {
            reply_raw(h, "550 Could not get file size.\r\n");
            return;
        }
        reply_line(h, "213 %ld\r\n", (long)st.st_size);
        return;
    }

    if (cmd_is(h, "MDTM")) {
        char ftp_path[FTP_CWD_SIZE], vfs[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_path, sizeof(ftp_path), h->parameter)) {
            return;
        }
        resolve_path(h, ftp_path, vfs, sizeof(vfs));
        struct stat st;
        if (stat(vfs, &st) != 0) {
            reply_raw(h, "550 File unavailable\r\n");
            return;
        }
        char mdtm[20];
        format_mdtm(st.st_mtime, mdtm, sizeof(mdtm));
        reply_line(h, "213 %s\r\n", mdtm);
        return;
    }

    if (cmd_is(h, "REST")) {
        if (!have_parameter(h)) {
            return;
        }
        h->restart_pos = (uint32_t)atol(h->parameter);
        reply_line(h, "350 Restarting at %lu. Ready to resume transfer.\r\n",
                   (unsigned long)h->restart_pos);
        return;
    }

    if (cmd_is(h, "SITE")) {
        if (h->parameter && strncasecmp(h->parameter, "FREE", 4) == 0) {
            uint32_t free_b, total_b;
            fs_space(h, &free_b, &total_b);
            reply_line(h, "200 %lu %lu\r\n", (unsigned long)free_b, (unsigned long)total_b);
        } else {
            reply_raw(h, "500 Unknown SITE command\r\n");
        }
        return;
    }

    if (cmd_is(h, "ALLO") || cmd_is(h, "STAT")) {
        reply_raw(h, "202 Command not implemented, superfluous at this site\r\n");
        return;
    }

    if (cmd_is(h, "LIST") || cmd_is(h, "NLST") || cmd_is(h, "MLSD")) {
        if (!data_connect(h, true)) {
            return;
        }
        char vfs[FTP_CWD_SIZE];
        resolve_path(h, h->cwd, vfs, sizeof(vfs));
        if (h->dir) {
            closedir(h->dir);
        }
        h->dir = opendir(vfs);
        if (!h->dir) {
            reply_line(h, "550 Can't open directory %s\r\n", h->cwd);
            sock_close(&h->data_fd);
            return;
        }
        if (cmd_is(h, "LIST")) {
            h->xfer_stage = XFER_LIST;
        } else if (cmd_is(h, "NLST")) {
            h->xfer_stage = XFER_NLST;
        } else {
            h->xfer_stage = XFER_MLSD;
        }
        return;
    }

    if (cmd_is(h, "MLST")) {
        char ftp_path[FTP_CWD_SIZE], vfs[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_path, sizeof(ftp_path), h->parameter)) {
            return;
        }
        resolve_path(h, ftp_path, vfs, sizeof(vfs));
        struct stat st;
        if (stat(vfs, &st) != 0) {
            reply_raw(h, "550 File unavailable\r\n");
            return;
        }
        char mdtm[20];
        format_mdtm(st.st_mtime, mdtm, sizeof(mdtm));
        reply_raw(h, "250-Begin\r\n");
        if (S_ISDIR(st.st_mode)) {
            reply_line(h, " Type=dir;Modify=%s; %s\r\n", mdtm, ftp_path);
        } else {
            reply_line(h, " Type=file;Modify=%s;Size=%ld; %s\r\n", mdtm, (long)st.st_size, ftp_path);
        }
        reply_raw(h, "250 End.\r\n");
        return;
    }

    if (cmd_is(h, "RETR")) {
        char ftp_path[FTP_CWD_SIZE], vfs[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_path, sizeof(ftp_path), h->parameter)) {
            return;
        }
        resolve_path(h, ftp_path, vfs, sizeof(vfs));
        if (h->file) {
            fclose(h->file);
        }
        h->file = fopen(vfs, "rb");
        if (!h->file) {
            reply_line(h, "450 Can't open %s\r\n", h->parameter);
            return;
        }
        if (!data_connect(h, false)) {
            fclose(h->file);
            h->file = NULL;
            return;
        }
        fseek(h->file, 0, SEEK_END);
        long sz = ftell(h->file);
        fseek(h->file, 0, SEEK_SET);
        strncpy(h->xfer_name, h->parameter, sizeof(h->xfer_name) - 1);
        fire_xfer(h, SIMPLE_FTP_XFER_DOWNLOAD_START, h->parameter, (uint32_t)sz);
        reply_line(h, "150-Connected to port %u\r\n", (unsigned)h->data_port);
        reply_line(h, "150 %ld bytes to download\r\n", sz);
        h->bytes_xfer = 0;
        h->xfer_stage = XFER_RETRIEVE;
        return;
    }

    if (cmd_is(h, "STOR") || cmd_is(h, "APPE")) {
        char ftp_path[FTP_CWD_SIZE], vfs[FTP_CWD_SIZE];
        if (!have_parameter(h) || !make_ftp_path(h, ftp_path, sizeof(ftp_path), h->parameter)) {
            return;
        }
        resolve_path(h, ftp_path, vfs, sizeof(vfs));
        if (h->file) {
            fclose(h->file);
        }
        h->file = fopen(vfs, cmd_is(h, "APPE") ? "ab" : "wb");
        if (!h->file) {
            reply_line(h, "451 Can't open/create %s\r\n", h->parameter);
            return;
        }
        if (!data_connect(h, true)) {
            fclose(h->file);
            h->file = NULL;
            return;
        }
        strncpy(h->xfer_name, h->parameter, sizeof(h->xfer_name) - 1);
        fire_xfer(h, SIMPLE_FTP_XFER_UPLOAD_START, h->parameter, 0);
        h->bytes_xfer = 0;
        h->xfer_stage = XFER_STORE;
        return;
    }

    reply_line(h, "500 Unknown command %s\r\n", h->command);
}

static void ftp_tick(simple_ftp_handle_t h)
{
    if (now_ms() < h->delay_until_ms) {
        return;
    }

    if (h->cmd_stage == FTP_STOP) {
        if (h->ctrl_fd >= 0) {
            disconnect_client(h);
        }
        h->cmd_stage = FTP_INIT;
    } else if (h->cmd_stage == FTP_INIT) {
        abort_transfer(h);
        ini_variables(h);
        ESP_LOGI(TAG, "waiting for connection on port %u", h->cmd_port);
        h->cmd_stage = FTP_WAIT_CLIENT;
    } else if (h->cmd_stage == FTP_WAIT_CLIENT) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(h->listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd >= 0) {
            if (h->ctrl_fd >= 0) {
                abort_transfer(h);
                sock_close(&h->ctrl_fd);
            }
            h->ctrl_fd = fd;
            sock_set_nodelay(fd);
            /* blocking ctrl reads via select in loop — leave default */
            client_connected(h);
            h->deadline_ms = now_ms() + 1000LL * h->auth_timeout_s;
            h->cmd_stage = FTP_USER;
        }
    } else if (h->ctrl_fd >= 0) {
        int32_t rc = read_char(h);
        if (rc > 0) {
            process_command(h);
            if (h->ctrl_fd < 0) {
                h->cmd_stage = FTP_INIT;
            } else if (h->cmd_stage == FTP_STOP) {
                h->deadline_ms = now_ms() + 1000LL * h->auth_timeout_s;
            } else if (h->cmd_stage < FTP_CMD) {
                h->delay_until_ms = now_ms() + 200;
            } else {
                h->deadline_ms = now_ms() + 1000LL * h->timeout_s;
            }
        } else {
            char peek;
            ssize_t pr = recv(h->ctrl_fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
            if (pr == 0) {
                ESP_LOGI(TAG, "client disconnected");
                abort_transfer(h);
                uint32_t free_b, total_b;
                fs_space(h, &free_b, &total_b);
                fire_event(h, SIMPLE_FTP_EVT_DISCONNECT, free_b, total_b);
                sock_close(&h->ctrl_fd);
                sock_close(&h->data_fd);
                h->cmd_stage = FTP_INIT;
            } else if (now_ms() > h->deadline_ms) {
                reply_raw(h, "530 Timeout\r\n");
                h->delay_until_ms = now_ms() + 200;
                h->cmd_stage = FTP_STOP;
            }
        }
    }

    if (h->xfer_stage == XFER_RETRIEVE) {
        if (!do_retrieve(h)) {
            h->xfer_stage = XFER_CLOSE;
        }
    } else if (h->xfer_stage == XFER_STORE) {
        if (!do_store(h)) {
            h->xfer_stage = XFER_CLOSE;
        }
    } else if (h->xfer_stage == XFER_LIST || h->xfer_stage == XFER_NLST) {
        if (!do_list_one(h, h->xfer_stage == XFER_NLST)) {
            h->xfer_stage = XFER_CLOSE;
        }
    } else if (h->xfer_stage == XFER_MLSD) {
        if (!do_mlsd_one(h)) {
            h->xfer_stage = XFER_CLOSE;
        }
    }
}

static void ftp_task(void *arg)
{
    simple_ftp_handle_t h = arg;
    h->running = true;
    h->cmd_stage = FTP_STOP;

    while (!h->stop_req) {
        ftp_tick(h);
        /* during transfers poll faster */
        // 传书时不再强制 1 ms 休眠：一轮只搬运一个 recv，1 tick 会把大缓冲吞吐压到
        // 「每毫秒一个 recv」，2.4G 传书实测被这条休眠卡住。0 只让出同优先级，不独占 CPU。
        // During a transfer do not force a 1 ms sleep: one recv per tick caps a large buffer at
        // one recv per millisecond, which throttles 2.4G book uploads. 0 only yields to equal
        // priority tasks, so the CPU is not hogged.
        TickType_t d = (h->xfer_stage != XFER_CLOSE) ? (TickType_t)0 : pdMS_TO_TICKS(10);
        vTaskDelay(d);
    }

    abort_transfer(h);
    sock_close(&h->ctrl_fd);
    sock_close(&h->data_fd);
    sock_close(&h->pasv_listen_fd);
    sock_close(&h->listen_fd);
    h->running = false;
    h->task = NULL;
    vTaskDelete(NULL);
}

/* ---------- public API ---------- */

esp_err_t simple_ftp_create(const simple_ftp_config_t *cfg, simple_ftp_handle_t *out)
{
    if (!cfg || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    simple_ftp_handle_t h = calloc(1, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }

    const char *user = cfg->user ? cfg->user : "anonymous";
    const char *pass = cfg->pass ? cfg->pass : "anonymous";
    const char *base = cfg->base_path ? cfg->base_path : "/";
    const char *welcome = cfg->welcome ? cfg->welcome : "Welcome to Simply FTP server";

    strncpy(h->user, user, FTP_CRED_SIZE - 1);
    strncpy(h->pass, pass, FTP_CRED_SIZE - 1);
    strncpy(h->base_path, base, FTP_CWD_SIZE - 1);
    /* strip trailing slash except root */
    size_t bl = strlen(h->base_path);
    if (bl > 1 && h->base_path[bl - 1] == '/') {
        h->base_path[bl - 1] = 0;
    }
    strncpy(h->welcome, welcome, sizeof(h->welcome) - 1);

    h->cmd_port = cfg->cmd_port ? cfg->cmd_port : SIMPLE_FTP_DEFAULT_CMD_PORT;
    h->pasv_port = cfg->pasv_port ? cfg->pasv_port : SIMPLE_FTP_DEFAULT_PASV_PORT;
    h->local_ip = cfg->local_ip;
    h->buf_size = cfg->buf_size ? cfg->buf_size : SIMPLE_FTP_DEFAULT_BUF_SIZE;
    h->timeout_s = cfg->timeout_s ? cfg->timeout_s : SIMPLE_FTP_DEFAULT_TIMEOUT_S;
    h->auth_timeout_s = cfg->auth_timeout_s ? cfg->auth_timeout_s : SIMPLE_FTP_DEFAULT_AUTH_S;
    h->anonymous = cfg->anonymous;
    h->task_priority = cfg->task_priority ? cfg->task_priority : 5;
    h->task_stack = cfg->task_stack ? cfg->task_stack : 8192;
    h->event_cb = cfg->event_cb;
    h->xfer_cb = cfg->xfer_cb;
    h->user_ctx = cfg->user_ctx;

    h->listen_fd = -1;
    h->pasv_listen_fd = -1;
    h->ctrl_fd = -1;
    h->data_fd = -1;
    h->lock = xSemaphoreCreateMutex();
    h->buf = malloc(h->buf_size);
    if (!h->lock || !h->buf) {
        if (h->lock) {
            vSemaphoreDelete(h->lock);
        }
        free(h->buf);
        free(h);
        return ESP_ERR_NO_MEM;
    }

    *out = h;
    return ESP_OK;
}

esp_err_t simple_ftp_start(simple_ftp_handle_t h)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->running) {
        return ESP_OK;
    }
    h->stop_req = false;
    h->listen_fd = sock_listen(h->cmd_port);
    if (h->listen_fd < 0) {
        ESP_LOGE(TAG, "bind/listen cmd port %u failed: %d", h->cmd_port, errno);
        return ESP_FAIL;
    }
    BaseType_t ok = xTaskCreate(ftp_task, "simple_ftp", h->task_stack, h,
                                h->task_priority, &h->task);
    if (ok != pdPASS) {
        sock_close(&h->listen_fd);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "FTP started on :%u (PASV :%u), base=%s",
             h->cmd_port, h->pasv_port, h->base_path);
    return ESP_OK;
}

esp_err_t simple_ftp_stop(simple_ftp_handle_t h)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!h->running && !h->task) {
        return ESP_OK;
    }
    h->stop_req = true;
    for (int i = 0; i < 100 && h->running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

esp_err_t simple_ftp_destroy(simple_ftp_handle_t h)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    simple_ftp_stop(h);
    free(h->buf);
    if (h->lock) {
        vSemaphoreDelete(h->lock);
    }
    free(h);
    return ESP_OK;
}

esp_err_t simple_ftp_set_credentials(simple_ftp_handle_t h, const char *user, const char *pass)
{
    if (!h || !user || !pass) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(h->user, user, FTP_CRED_SIZE - 1);
    strncpy(h->pass, pass, FTP_CRED_SIZE - 1);
    return ESP_OK;
}

esp_err_t simple_ftp_set_local_ip(simple_ftp_handle_t h, uint32_t ip_host_order)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    h->local_ip = ip_host_order;
    return ESP_OK;
}

bool simple_ftp_is_running(simple_ftp_handle_t h)
{
    return h && h->running;
}
