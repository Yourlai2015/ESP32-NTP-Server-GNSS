// ESP32-S3 NTP Stratum 1 时间服务器
//
// 本文件集中存放所有可配置项。
//
// GNSS 通过 UART 接收 NMEA 报文（RMC 提供时间、GGA 提供定位），PPS 秒脉冲由硬件捕获并用于驯服系统时钟，可达亚毫秒级精度。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 调试支持（开启后会略微降低高负载下的精度）
// ---------------------------------------------------------------------------
#define DEBUG_ENABLED 0 // 0 = 关闭；1 = 开启

// ---------------------------------------------------------------------------
// 可选功能
// ---------------------------------------------------------------------------
// NTPv4 对称密钥认证（实现见 ntp/ntp_auth.c）。开启后，仅携带有效密钥的
// NTPv4 请求会收到带 MAC 的应答；未认证的 NTPv4 请求仍会被处理。
#define SYMMETRIC_KEY_AUTHENTICATION_ENABLED 0 // 0 = 关闭；1 = 开启

// 启动自检：向本机的回环与已分配地址发送 NTPv3/NTPv4（含交错模式）请求并校验
// 应答。默认关闭（会增加启动时间与资源占用）。
#define STARTUP_HEALTH_TEST_ENABLED 0 // 0 = 关闭；1 = 开启

// ---------------------------------------------------------------------------
// 默认 WiFi 连接凭证
// 请替换为你自己的WiFi
// ---------------------------------------------------------------------------
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ---------------------------------------------------------------------------
// GNSS UART 接线
// ---------------------------------------------------------------------------
#define GNSS_UART_PORT 1
#define GNSS_TX_PIN 41 // ESP32-S3 TX -> GNSS RXD
#define GNSS_RX_PIN 42 // ESP32-S3 RX <- GNSS TXD
#define GNSS_PPS_PIN 45
#define GNSS_UART_RX_BUFFER_SIZE 2048

// 后备波特率（用于非 u-blox 兼容模块）。本项目 M10 模块默认 38400。
#define GNSS_FALLBACK_BAUD 38400

// ---------------------------------------------------------------------------
// SSD1306 OLED（128x64）I2C 接线
// ---------------------------------------------------------------------------
#define OLED_SCK_PIN 2 // I2C 时钟（SCL）
#define OLED_SDA_PIN 1 // I2C 数据（SDA）

// ---------------------------------------------------------------------------
// OLED 翻页按键
// 按键另一端接地，按下低电平。短按翻页，长按启动 WiFi 配网热点。
// ---------------------------------------------------------------------------
#define BUTTON_PIN 21
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_SHORT_PRESS_MIN_MS 50
#define BUTTON_LONG_PRESS_MS 5000

// ---------------------------------------------------------------------------
// WiFi 配网热点（长按按键启动）
// 热点名为 <前缀><MAC 末两字节>，例如 NTP-1A2B。
// ---------------------------------------------------------------------------
#define PROVISION_AP_SSID_PREFIX "NTP-"
// 留空则为开放热点；WPA2 至少需要 8 位密码。建议修改默认值。
#define PROVISION_AP_PASSWORD "12345678"

// ---------------------------------------------------------------------------
// 时区（POSIX TZ 字符串）
// 参考：https://gist.github.com/alwynallan/24d96091655391107939
// ---------------------------------------------------------------------------
// 北京时间（UTC+8，无夏令时）
#define TIME_ZONE_SPEC "CST-8"

// ---------------------------------------------------------------------------
// NTP 服务参数
// ---------------------------------------------------------------------------
#define NTP_PORT 123
#define NTP_PACKET_SIZE 48
#define NTP_EPOCH_OFFSET 2208988800ULL
#define NTP_PRECISION_EXPONENT (-13)
#define NTP_ROOT_DISPERSION 66
#define NTP_CACHE_TIMESTAMP_MATCH_TOLERANCE 8ULL
#define NTP_SOCKET_BATCH_LIMIT 16
#define IP_ADDRESS_TEXT_SIZE 46 /* INET6_ADDRSTRLEN */

// ---------------------------------------------------------------------------
// GNSS 同步 / 驯服参数
// ---------------------------------------------------------------------------
// 每隔 N 分钟与 GNSS 重新同步一次（建议 5）
#define PERIODIC_GNSS_REFRESH_MINUTES 1U

// 新的 GNSS 读数与上一次相比应为亚秒级差异，否则触发安全检查
#define SAFEGUARD_THRESHOLD_SECONDS 1

// 安全检查失败时是否自动重启；为 0 则继续以未同步时间运行
#define REBOOT_IF_SANITY_CHECK_FAILS 1

// 对部分较旧 / 非 u-blox 兼容模块的额外处理
#define ALLOW_FALLBACK_PROCESSING 1

// ---------------------------------------------------------------------------
// 任务栈大小（字节）
// ---------------------------------------------------------------------------
#define GNSS_RECOVERY_TASK_STACK_SIZE 12288
#define GNSS_TIME_SYNC_TASK_STACK_SIZE 3217
#define PPS_DISCIPLINE_TASK_STACK_SIZE 2390
#define NTP_CACHE_PURGE_TASK_STACK_SIZE 2304
#define NTP_SERVER_TASK_STACK_SIZE 3560
#define STARTUP_HEALTH_TASK_STACK_SIZE 4096

// ---------------------------------------------------------------------------
// 派生内部常量（请勿修改）
// ---------------------------------------------------------------------------
// 对称密钥 MAC 布局（与 NTPv4 MAC 布局一致）
#define NTP_AUTH_KEY_ID_SIZE 4
#define NTP_AUTH_DIGEST_SIZE 20
#define NTP_AUTH_TRAILER_SIZE (NTP_AUTH_KEY_ID_SIZE + NTP_AUTH_DIGEST_SIZE)

#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
#define NTP_MAX_PACKET_SIZE (NTP_PACKET_SIZE + NTP_AUTH_TRAILER_SIZE)
#else
#define NTP_MAX_PACKET_SIZE (NTP_PACKET_SIZE)
#endif

// GNSS NVS 持久化
#define GNSS_NVS_NAMESPACE "gnss_state"

// WiFi 凭证持久化（由配网网页写入）
#define WIFI_NVS_NAMESPACE "wifi_cfg"

// 同步状态时间阈值
#define SYNC_STALE_AFTER_US ((int64_t)PERIODIC_GNSS_REFRESH_MINUTES * 60LL * 1000000LL * 3LL)
#define GNSS_VALIDITY_TIMEOUT_US 3500000LL
#define SYNC_REBOOT_AFTER_US (30LL * 60LL * 1000000LL)
#define MAX_SYNC_ATTEMPT_US 10000000LL
#define GNSS_INVALID_REACQUISITION_AFTER_US (30LL * 1000000LL)
#define RUNTIME_GNSS_RECOVERY_MIN_INTERVAL_US (5LL * 60LL * 1000000LL)
#define SYNC_FAILURES_BEFORE_RUNTIME_RECOVERY 20U
#define SANITY_FAILURES_BEFORE_FAULT 2U

// GNSS 启动韧性：接收机完全静默（接线/供电/模块故障）时，不能让服务器卡在无输出的循环里——需要监测链路、重试启动，必要时重启。
#define GNSS_RX_SILENCE_TIMEOUT_MS 15000U
#define GNSS_STARTUP_QUALIFICATION_TIMEOUT_MS 90000U
#define GNSS_SETUP_RETRY_DELAY_MS 10000U
#define GNSS_SETUP_MAX_ATTEMPTS 6U

// 启动时 PPS 资格判定
#define GNSS_STARTUP_QUALIFICATION_DURATION_MS 15000U
#define GNSS_STARTUP_QUALIFICATION_PPS_EDGES 10U
#define GNSS_STARTUP_MIN_PPS_INTERVAL_US 800000LL
#define GNSS_STARTUP_MAX_PPS_INTERVAL_US 1200000LL

// PPS 捕获
#define PPS_CAPTURE_RESOLUTION_HZ 80000000U
#define PPS_TIMEOUT_US 3500000LL

// 对称密钥存储上限
#define NTP_AUTH_MAX_KEYS 4
#define NTP_AUTH_MAX_KEY_SIZE 64
// 密钥值在 ntp/ntp_auth.c 中配置
