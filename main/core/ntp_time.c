// ESP32-S3 Stratum 1 NTP 时间服务器（C 语言移植版）
// NTP 时间戳编解码、时区设置与 NTP 应答报文构造的实现。

#include "ntp_time.h"

#include "app_config.h"
#include "sync_state.h"
#include "time_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_timer.h"

// NTP 应答模板：仅预置精度与根色散字段，其余内容由 ntp_build_reply() 填充
static const uint8_t NTP_REPLY_TEMPLATE[NTP_PACKET_SIZE] = {
    0, 0, 4, (uint8_t)NTP_PRECISION_EXPONENT,
    0, 0, 0, 0,
    0, 0, 0, (uint8_t)NTP_ROOT_DISPERSION};

uint64_t ntp_now_64(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);

    uint64_t seconds = NTP_EPOCH_OFFSET + (uint64_t)now.tv_sec;
    uint64_t fraction = ((uint64_t)now.tv_usec << 32) / 1000000ULL;
    return (seconds << 32) | fraction;
}

void ntp_write_timestamp(uint8_t *packet, size_t offset, uint64_t timestamp)
{
    packet[offset + 0] = (uint8_t)((timestamp >> 56) & 0xFF);
    packet[offset + 1] = (uint8_t)((timestamp >> 48) & 0xFF);
    packet[offset + 2] = (uint8_t)((timestamp >> 40) & 0xFF);
    packet[offset + 3] = (uint8_t)((timestamp >> 32) & 0xFF);
    packet[offset + 4] = (uint8_t)((timestamp >> 24) & 0xFF);
    packet[offset + 5] = (uint8_t)((timestamp >> 16) & 0xFF);
    packet[offset + 6] = (uint8_t)((timestamp >> 8) & 0xFF);
    packet[offset + 7] = (uint8_t)(timestamp & 0xFF);
}

uint64_t ntp_read_timestamp(const uint8_t *packet, size_t offset)
{
    return ((uint64_t)packet[offset + 0] << 56) | ((uint64_t)packet[offset + 1] << 48) |
           ((uint64_t)packet[offset + 2] << 40) | ((uint64_t)packet[offset + 3] << 32) |
           ((uint64_t)packet[offset + 4] << 24) | ((uint64_t)packet[offset + 5] << 16) |
           ((uint64_t)packet[offset + 6] << 8) | (uint64_t)packet[offset + 7];
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153U * (month + (month > 2 ? (unsigned)-3 : 9U)) + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

time_t ntp_epoch_from_utc(int year, int month, int day, int hour, int minute, int second)
{
    int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    int64_t seconds = days * 86400LL + hour * 3600LL + minute * 60LL + second;
    return (time_t)seconds;
}

void ntp_apply_timezone(void)
{
    setenv("TZ", TIME_ZONE_SPEC, 1);
    tzset();
}

void ntp_format_local_date_time(time_t utc_time, char *date_string, size_t date_size, char *time_string, size_t time_size)
{
    struct tm local_tm;
    localtime_r(&utc_time, &local_tm);

    snprintf(date_string, date_size, "%04d-%02d-%02d", local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday);

    int hour_value = local_tm.tm_hour % 12;
    if (hour_value == 0)
        hour_value = 12;

    const char *ampm = local_tm.tm_hour < 12 ? "AM" : "PM";
    snprintf(time_string, time_size, "%d:%02d:%02d %s", hour_value, local_tm.tm_min, local_tm.tm_sec, ampm);
}

void ntp_format_iso8601(time_t value, char *output, size_t output_size)
{
    // 本地时间的 ISO-8601 格式，例如 2026-08-25T21:57:14-0400
    struct tm local_tm;
    localtime_r(&value, &local_tm);

    char date_time[32] = "";
    char zone[8] = "";
    strftime(date_time, sizeof(date_time), "%Y-%m-%dT%H:%M:%S", &local_tm);
    strftime(zone, sizeof(zone), "%z", &local_tm);
    snprintf(output, output_size, "%s%s", date_time, zone);
}

void ntp_refresh_reference(void)
{
    time_state_publish_reference(ntp_now_64(), true);
}

ntp_reply_status_t ntp_get_reply_status(void)
{
    ntp_reply_status_t not_synchronised = {3, 16, "INIT", false, false, false};

    sync_published_t published;
    if (!sync_state_read_published(&published))
        return not_synchronised;

    int64_t now_us = esp_timer_get_time();
    bool gnss_recent = published.gnss_timing_valid && published.last_gnss_valid_us > 0 &&
                       (now_us - published.last_gnss_valid_us) <= GNSS_VALIDITY_TIMEOUT_US;
    bool sync_stale = published.last_successful_sync_us > 0 &&
                      (now_us - published.last_successful_sync_us) > SYNC_STALE_AFTER_US;
    bool gnss_synchronized = gnss_recent && !sync_stale && !published.faults.gnss_invalid;

    bool stratum_one = time_state_has_been_set() &&
                       !time_state_setting_in_progress() &&
                       !sync_state_has_any_fault(published.faults) &&
                       published.pps_active &&
                       gnss_synchronized &&
                       time_state_reference_valid();

    if (stratum_one)
    {
        ntp_reply_status_t status = {0, 1, "GPS", true, gnss_synchronized, published.pps_active};
        return status;
    }

    not_synchronised.gnss_synchronized = gnss_synchronized;
    not_synchronised.pps_disciplined = published.pps_active;
    return not_synchronised;
}

void ntp_build_reply(const uint8_t *request, uint8_t *reply, uint8_t version, uint64_t receive_time, const ntp_reply_status_t *status)
{
    memcpy(reply, NTP_REPLY_TEMPLATE, sizeof(NTP_REPLY_TEMPLATE));

    reply[0] = (uint8_t)((status->leap_indicator << 6) | (version << 3) | 4);
    reply[1] = status->stratum;
    memcpy(reply + 12, status->reference_id, 4);

    if (status->reference_time_valid)
        ntp_write_timestamp(reply, 16, time_state_reference_64());

    memcpy(reply + 24, request + 40, 8); // 原始时间戳 = 客户端发送时间戳
    ntp_write_timestamp(reply, 32, receive_time);
}
