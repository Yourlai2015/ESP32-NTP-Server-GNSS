// NTP 服务器接口。
// 通过 UDP 提供 IPv4、IPv6 及 IPv6 链路本地地址的 NTP 服务。

#pragma once

#include <stdbool.h>

// 创建 NTP 缓存、清理任务与服务器主任务，并等待监听套接字就绪。
void ntp_server_start(void);

// 外部响应开关：关闭时仅应答内部（本机）请求，供启动自检使用。
void ntp_server_set_external_responses(bool enabled);
bool ntp_server_external_responses_enabled(void);

// 监听套接字绑定完成后返回 true
bool ntp_server_is_ready(void);

// FreeRTOS 任务：清理过期的 NTP 客户端缓存条目
void ntp_cache_purge_task(void *parameter);
