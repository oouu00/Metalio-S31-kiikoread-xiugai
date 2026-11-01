#pragma once
#include <stdbool.h>

// File manager opens USB storage directly; WiFi/hotspot go through the method picker
// so the user can pick the web/FTP protocol before the service starts.
void app_transfer_request_usb_start(void);
void app_transfer_request_wifi_upload(void);
void app_transfer_request_hotspot_start(void);

// Settings uses this before opening Transfer to enter WiFi provisioning directly.
void app_transfer_request_wifi_setup(void);

// 文件管理打开 WiFi/热点方式选择，选择前不启动传输。
// File manager opens a WiFi/hotspot chooser before starting either service.
void app_transfer_request_method_picker(void);

// 仅首次引导的临时页面容器使用；离页必须复位。/ Setup child wrapper only; reset on exit.
void app_transfer_set_setup_chrome(bool active);
