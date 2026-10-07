// ESP32-S3 NTP 时间服务器
// WiFi Station 链路（替代原项目的以太网连接）。
// 使用 app_config.h 或配置页面保存的凭据将 ESP32-S3 接入 WiFi，
// 跟踪 IPv4/IPv6 地址并暴露给应用其余部分。

#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_GOT_IP_BIT BIT1
#define WIFI_GOT_IP6_BIT BIT2

// 创建 netif 与事件处理器，并开始非阻塞连接。
void wifi_link_init(void);

// 切换 Station 到新凭据并重连（供 WiFi 设置页面使用）；空密码表示开放网络。
bool wifi_link_apply_credentials(const char *ssid, const char *password);

// 阻塞等待至少获取到一个 IP 地址（IPv4 或 IPv6）。
void wifi_link_wait_for_ip(void);

bool wifi_link_is_connected(void);
bool wifi_link_has_ip(void);

const char *wifi_link_ipv4(void);
const char *wifi_link_ipv6(void);
const char *wifi_link_primary_ip(void);

esp_netif_t *wifi_link_netif(void);
EventGroupHandle_t wifi_link_event_group(void);
