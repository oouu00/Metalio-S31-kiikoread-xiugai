# Cezap/simple_ftp_server

FTP-сервер для **ESP-IDF** (порт [SimpleFTPServer](https://github.com/xreef/SimpleFTPServer) Renzo Mischianti).

* Сеть: **lwIP sockets** (WiFi / Ethernet поднимаешь сам)
* Файлы: **POSIX VFS** — SPIFFS, LittleFS, FAT, SD (монтируешь сам, указываешь `base_path`)
* В фоне крутится FreeRTOS-задача (аналог `handleFTP()` в `loop()`)

Оригинал (Arduino): https://www.mischianti.org/2020/02/08/ftp-server-on-esp8266-and-esp32  

## Installation

```sh
idf.py add-dependency "Cezap/simple_ftp_server^1.0.0"
```

Или в `main/idf_component.yml`:

```yaml
dependencies:
  Cezap/simple_ftp_server: "^1.0.0"
```

В коде: `#include "simple_ftp_server.h"`

## Quick start

Сначала WiFi + mount FS (пример — SPIFFS), потом FTP:

```c
#include "simple_ftp_server.h"

void app_main(void)
{
    /* ... wifi_init_sta(); esp_vfs_spiffs_register(...); ... */

    simple_ftp_config_t cfg = SIMPLE_FTP_CONFIG_DEFAULT();
    cfg.user = "user";
    cfg.pass = "password";
    cfg.base_path = "/spiffs";   /* FTP "/" = этот путь в VFS */

    simple_ftp_handle_t ftp;
    ESP_ERROR_CHECK(simple_ftp_create(&cfg, &ftp));
    ESP_ERROR_CHECK(simple_ftp_start(ftp));
}
```

Подключение с ПК (FileZilla / WinSCP):

* Хост = IP платы, порт **21**
* Логин/пароль как в `cfg`
* Режим: **Passive** (PASV), порт данных по умолчанию **50009**

Если PASV отдаёт неверный IP (NAT / AP) — задай вручную:

```c
simple_ftp_set_local_ip(ftp, /* host-order */ 0xC0A8010A); /* 192.168.1.10 */
```

## Соответствие Arduino API

| Arduino SimpleFTPServer | ESP-IDF |
|-------------------------|---------|
| `FtpServer ftp;` | `simple_ftp_create()` |
| `ftp.begin(user, pass)` | `simple_ftp_start()` |
| `ftp.handleFTP()` в `loop` | задача внутри компонента |
| `ftp.end()` | `simple_ftp_stop()` |
| `setCallback` / `setTransferCallback` | `cfg.event_cb` / `cfg.xfer_cb` |
| `SD` / `LittleFS` / `SPIFFS` | VFS `base_path` |

## Команды FTP

USER, PASS, CWD, CDUP, PWD, QUIT, NOOP, MODE, PASV, PORT, STRU, TYPE,  
ABOR, DELE, LIST, NLST, MLSD, MLST, RETR, STOR, APPE, MKD, RMD,  
RNFR, RNTO, MDTM, SIZE, FEAT, SITE FREE, REST, SYST, HELP.

## Example

```sh
cd examples/default
idf.py set-target esp32
idf.py menuconfig   # WiFi SSID / password
idf.py build
idf.py -p COMx flash monitor
```

## License

MIT. Основано на SimpleFTPServer © Renzo Mischianti (www.mischianti.org).
