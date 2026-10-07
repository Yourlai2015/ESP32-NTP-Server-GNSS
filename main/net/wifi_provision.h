// ESP32-S3 NTP 时间服务器
// WiFi 配网热点与配置网页。

// 长按翻页按键会在 Station 之外开启 SoftAP；手机连上该热点后可访问 http://192.168.4.1/
// 选择目标 WiFi 并保存密码，凭据写入 NVS（见 wifi_config.c），保存后热点随即关闭。
//如果已经连接到目标 WiFi，长按按键可通过设备IP访问配置页面（默认 http://192.168.4.1/）。

#pragma once

#include <stdbool.h>

typedef enum
{
    WIFI_PROVISION_IDLE = 0,  // 无热点，无待处理事项
    WIFI_PROVISION_AP_ACTIVE, // 热点已开启，等待配置页面
    WIFI_PROVISION_SAVED,     // 凭据已保存，Station 正在重连
} wifi_provision_state_t;

// 启动接入点与配置 Web 服务器；幂等，启动失败返回 false。
bool wifi_provision_start(void);

// 推进延迟的“配置完成”切换（关闭热点并重连 Station）；需在任务中周期性调用，
// 不可在 HTTP 处理函数内调用。
void wifi_provision_poll(void);

wifi_provision_state_t wifi_provision_state(void);

// 热点信息，用于 OLED 显示
const char *wifi_provision_ap_ssid(void);
const char *wifi_provision_ap_password(void);
const char *wifi_provision_ap_ip(void);

// 配置页面保存的 SSID（仅在 WIFI_PROVISION_SAVED 状态有效）
const char *wifi_provision_saved_ssid(void);
