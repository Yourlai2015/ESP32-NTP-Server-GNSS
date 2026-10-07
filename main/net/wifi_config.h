// ESP32-S3 NTP 时间服务器
// WiFi Station 凭据的非易失存储。
// 凭据由配置页面写入（见 wifi_provision.c），下次启动时由 wifi_link_init() 读回；
// 若未保存任何内容，则改用 app_config.h 中的编译期值。



#pragma once

#include <stdbool.h>
#include <stddef.h>

// 读取已保存的凭据；未保存或 SSID 为空时返回 false；开放网络下密码为空串。
bool wifi_config_load(char *ssid, size_t ssid_size, char *password, size_t password_size);

// 保存凭据（开放网络下密码可为 NULL 或空串）
bool wifi_config_save(const char *ssid, const char *password);
