// ESP32-S3 Stratum 1 NTP 时间服务器（C 语言移植版）
// 提供 NTP 时间戳编解码、时区处理与 NTP 应答报文构造的接口。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// 以 64 位 NTP 时间戳格式返回当前时间
uint64_t ntp_now_64(void);

// 在报文指定偏移处写入 / 读取 64 位 NTP 时间戳
void ntp_write_timestamp(uint8_t *packet, size_t offset, uint64_t timestamp);
uint64_t ntp_read_timestamp(const uint8_t *packet, size_t offset);

// 将 UTC 日历时间转换为 Unix 时间戳
time_t ntp_epoch_from_utc(int year, int month, int day, int hour, int minute, int second);

// 应用配置的 POSIX 时区
void ntp_apply_timezone(void);

// 格式化辅助函数（控制台 / 日志输出）
void ntp_format_local_date_time(time_t utc_time, char *date_string, size_t date_size, char *time_string, size_t time_size);
void ntp_format_iso8601(time_t value, char *output, size_t output_size);

// 重新读取当前时间并发布为 NTP 参考时间戳
void ntp_refresh_reference(void);

// NTP 应答中报告的状态（层号 / 参考标识 / 闰秒标志）
typedef struct
{
    uint8_t leap_indicator;
    uint8_t stratum;
    const char *reference_id;
    bool reference_time_valid;
    bool gnss_synchronized;
    bool pps_disciplined;
} ntp_reply_status_t;

// 依据同步状态判定应答状态（仅当时间已设置、GNSS 已同步且 PPS 授时成立时才是 Stratum 1）
ntp_reply_status_t ntp_get_reply_status(void);

// 构造标准 NTP 应答；发送时间戳（偏移 40）由调用方随后填写，以支持 RFC 9769 交错模式
void ntp_build_reply(const uint8_t *request, uint8_t *reply, uint8_t version, uint64_t receive_time, const ntp_reply_status_t *status);
