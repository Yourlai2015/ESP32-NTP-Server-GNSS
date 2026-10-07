// ESP32-S3 NTP 时间服务器
// GNSS UART 驱动实现：NMEA 语句的读取、校验与解析。
// 逐字节接收并组装 NMEA 语句，解析 RMC（日期时间）与 GGA（定位信息），
// 并提供波特率自动扫描以适配不同接收机。

#include "gnss_uart.h"

#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gnss_uart";

static const uint32_t s_candidate_baud_rates[] = {38400, 9600, 115200, 4800, 19200, 57600, 230400, 460800, 921600};
static const size_t s_candidate_baud_count = sizeof(s_candidate_baud_rates) / sizeof(s_candidate_baud_rates[0]);

static bool s_uart_installed = false;
static uint32_t s_current_baud = 0;

// 最近一次定位信息，每条 GGA 语句都会刷新
static gnss_fix_info_t s_fix_info = {0};

// 最近收到字节的时间戳，用于区分“链路存活但未定位”与“链路静默”
static int64_t s_last_rx_us = 0;

// NMEA 语句组装状态
static char s_sentence[128];
static size_t s_sentence_index = 0;
static bool s_collecting = false;

void gnss_uart_init(void)
{
    if (s_uart_installed)
        return;

    const uart_config_t config = {
        .baud_rate = GNSS_FALLBACK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(GNSS_UART_PORT, GNSS_UART_RX_BUFFER_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GNSS_UART_PORT, &config));
    ESP_ERROR_CHECK(uart_set_pin(GNSS_UART_PORT, GNSS_TX_PIN, GNSS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    s_current_baud = GNSS_FALLBACK_BAUD;
    s_uart_installed = true;
}

void gnss_uart_set_baud(uint32_t baud)
{
    gnss_uart_init();
    if (uart_set_baudrate(GNSS_UART_PORT, baud) == ESP_OK)
    {
        uart_flush_input(GNSS_UART_PORT);
        s_current_baud = baud;
    }
}

uint32_t gnss_uart_current_baud(void)
{
    return s_current_baud;
}

static bool is_digit_char(char c)
{
    return c >= '0' && c <= '9';
}

static bool nmea_get_field(const char *sentence, int field_index, char *output, size_t output_size)
{
    if (sentence == NULL || output == NULL || output_size == 0 || field_index < 0)
        return false;

    const char *cursor = sentence;
    if (*cursor == '$')
        cursor++;

    int current_field = 0;
    const char *field_start = cursor;

    for (;;)
    {
        char current = *cursor;
        bool is_delimiter = (current == ',') || (current == '*') || (current == '\0') || (current == '\r') || (current == '\n');

        if (is_delimiter)
        {
            if (current_field == field_index)
            {
                size_t length = (size_t)(cursor - field_start);
                if (length >= output_size)
                    length = output_size - 1;
                memcpy(output, field_start, length);
                output[length] = '\0';
                return true;
            }

            if (current != ',')
                break;

            current_field++;
            cursor++;
            field_start = cursor;
            continue;
        }

        cursor++;
    }

    output[0] = '\0';
    return false;
}

static bool parse_two_digits(const char *text, int *value)
{
    if (text == NULL || !is_digit_char(text[0]) || !is_digit_char(text[1]))
        return false;

    *value = (text[0] - '0') * 10 + (text[1] - '0');
    return true;
}

bool gnss_nmea_checksum_valid(const char *sentence)
{
    if (sentence == NULL || sentence[0] != '$')
        return false;

    const char *checksum_marker = strrchr(sentence, '*');
    if (checksum_marker == NULL || checksum_marker[1] == '\0' || checksum_marker[2] == '\0' || checksum_marker[3] != '\0')
        return false;

    unsigned int expected_checksum = 0;
    if (sscanf(checksum_marker + 1, "%2X", &expected_checksum) != 1)
        return false;

    uint8_t actual_checksum = 0;
    for (const char *cursor = sentence + 1; cursor < checksum_marker; ++cursor)
        actual_checksum ^= (uint8_t)(*cursor);

    return actual_checksum == expected_checksum;
}

bool gnss_parse_rmc_sentence(const char *sentence, nmea_time_t *out)
{
    if (sentence == NULL || out == NULL)
        return false;

    char sentence_type[16] = "";
    if (!nmea_get_field(sentence, 0, sentence_type, sizeof(sentence_type)))
        return false;

    size_t sentence_type_len = strlen(sentence_type);
    if (sentence_type_len < 3 || strcmp(sentence_type + sentence_type_len - 3, "RMC") != 0)
        return false;

    char status_field[4] = "";
    if (!nmea_get_field(sentence, 2, status_field, sizeof(status_field)))
        return false;
    if (status_field[0] != 'A')
        return false;

    char time_field[16] = "";
    char date_field[16] = "";
    if (!nmea_get_field(sentence, 1, time_field, sizeof(time_field)) ||
        !nmea_get_field(sentence, 9, date_field, sizeof(date_field)))
        return false;

    if (strlen(time_field) < 6 || strlen(date_field) < 6)
        return false;

    int hour = 0;
    int minute = 0;
    int second = 0;
    int day = 0;
    int month = 0;
    int year_two_digit = 0;

    if (!parse_two_digits(time_field + 0, &hour) ||
        !parse_two_digits(time_field + 2, &minute) ||
        !parse_two_digits(time_field + 4, &second) ||
        !parse_two_digits(date_field + 0, &day) ||
        !parse_two_digits(date_field + 2, &month) ||
        !parse_two_digits(date_field + 4, &year_two_digit))
        return false;

    int year = 2000 + year_two_digit;

    if (year <= 2022 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60)
        return false;

    out->year = year;
    out->month = month;
    out->day = day;
    out->hour = hour;
    out->minute = minute;
    out->second = second;
    return true;
}

static int parse_int_field(const char *text)
{
    int value = 0;
    bool have_digit = false;
    for (const char *cursor = text; *cursor != '\0'; ++cursor)
    {
        if (!is_digit_char(*cursor))
            break;
        value = value * 10 + (*cursor - '0');
        have_digit = true;
    }
    return have_digit ? value : 0;
}

static float parse_decimal_field(const char *text)
{
    if (text == NULL || text[0] == '\0')
        return 0.0f;
    return strtof(text, NULL);
}

// 将 NMEA 坐标（ddmm.mmmm / dddmm.mmmm）转换为十进制度，并附带半球字符
static void parse_coordinate_field(const char *text, char hemisphere, float *out_degrees, char *out_hemisphere)
{
    *out_degrees = 0.0f;
    *out_hemisphere = hemisphere;

    float raw = parse_decimal_field(text);
    if (raw <= 0.0f)
        return;

    int degrees = (int)(raw / 100.0f);
    float minutes = raw - (float)degrees * 100.0f;
    *out_degrees = (float)degrees + minutes / 60.0f;
}

// 从 GGA 语句刷新缓存的定位信息（卫星数、质量、位置）
static void gnss_update_fix_from_gga(const char *sentence)
{
    char sentence_type[16] = "";
    if (!nmea_get_field(sentence, 0, sentence_type, sizeof(sentence_type)))
        return;

    size_t sentence_type_len = strlen(sentence_type);
    if (sentence_type_len < 3 || strcmp(sentence_type + sentence_type_len - 3, "GGA") != 0)
        return;

    char latitude_field[16] = "";
    char latitude_ns[4] = "";
    char longitude_field[16] = "";
    char longitude_ew[4] = "";
    char quality_field[8] = "";
    char satellites_field[8] = "";

    nmea_get_field(sentence, 2, latitude_field, sizeof(latitude_field));
    nmea_get_field(sentence, 3, latitude_ns, sizeof(latitude_ns));
    nmea_get_field(sentence, 4, longitude_field, sizeof(longitude_field));
    nmea_get_field(sentence, 5, longitude_ew, sizeof(longitude_ew));
    nmea_get_field(sentence, 6, quality_field, sizeof(quality_field));
    nmea_get_field(sentence, 7, satellites_field, sizeof(satellites_field));

    s_fix_info.fix_quality = parse_int_field(quality_field);
    s_fix_info.satellites = parse_int_field(satellites_field);

    parse_coordinate_field(latitude_field, latitude_ns[0], &s_fix_info.latitude, &s_fix_info.latitude_hemisphere);
    parse_coordinate_field(longitude_field, longitude_ew[0], &s_fix_info.longitude, &s_fix_info.longitude_hemisphere);
    s_fix_info.position_valid = (s_fix_info.fix_quality > 0) &&
                                (latitude_field[0] != '\0') && (longitude_field[0] != '\0');

    s_fix_info.valid = true;
}

bool gnss_get_fix_info(gnss_fix_info_t *out)
{
    if (out == NULL)
        return false;

    *out = s_fix_info;
    return s_fix_info.valid;
}

int64_t gnss_uart_last_rx_us(void)
{
    return s_last_rx_us;
}

bool gnss_uart_read_sentence(char *out, size_t out_size, uint32_t timeout_ms)
{
    if (out == NULL || out_size == 0)
        return false;

    gnss_uart_init();

    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    uint8_t ch = 0;

    while (esp_timer_get_time() < deadline_us)
    {
        int received = uart_read_bytes(GNSS_UART_PORT, &ch, 1, pdMS_TO_TICKS(20));
        if (received <= 0)
            continue;

        s_last_rx_us = esp_timer_get_time();

        if (ch == '$')
        {
            s_collecting = true;
            s_sentence_index = 0;
            s_sentence[s_sentence_index++] = '$';
            continue;
        }

        if (!s_collecting)
            continue;

        if (ch == '\r')
            continue;

        if (ch == '\n')
        {
            s_sentence[s_sentence_index] = '\0';
            s_collecting = false;
            if (s_sentence_index > 0)
            {
                gnss_update_fix_from_gga(s_sentence);
                snprintf(out, out_size, "%s", s_sentence);
                s_sentence_index = 0;
                return true;
            }
            continue;
        }

        if (ch >= 32 && ch <= 126)
        {
            if (s_sentence_index < sizeof(s_sentence) - 1)
                s_sentence[s_sentence_index++] = (char)ch;
            else
            {
                s_collecting = false;
                s_sentence_index = 0;
            }
        }
    }

    return false;
}

bool gnss_uart_wait_for_time(nmea_time_t *out, uint32_t timeout_ms)
{
    if (out == NULL)
        return false;

    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    char sentence[128] = "";

    while (esp_timer_get_time() < deadline_us)
    {
        uint32_t remaining_ms = (uint32_t)((deadline_us - esp_timer_get_time()) / 1000LL);
        if (remaining_ms == 0)
            break;
        if (remaining_ms > 250)
            remaining_ms = 250;

        if (!gnss_uart_read_sentence(sentence, sizeof(sentence), remaining_ms))
            continue;

        if (gnss_parse_rmc_sentence(sentence, out))
            return true;
    }

    return false;
}

// 只要看到任一校验通过的 NMEA 即可接受该波特率，无需定位：
// 未定位的接收机仍会输出 NMEA（RMC 状态 'V'），在此等待定位会误判链路。
static bool baud_produces_valid_nmea(uint32_t baud, uint32_t listen_ms)
{
    gnss_uart_set_baud(baud);
    vTaskDelay(pdMS_TO_TICKS(150));

    const int64_t deadline_us = esp_timer_get_time() + (int64_t)listen_ms * 1000LL;
    size_t valid_sentences = 0;
    char sample[128] = "";
    char sentence[128] = "";

    while (esp_timer_get_time() < deadline_us)
    {
        uint32_t remaining_ms = (uint32_t)((deadline_us - esp_timer_get_time()) / 1000LL);
        if (remaining_ms == 0)
            break;
        if (remaining_ms > 250)
            remaining_ms = 250;

        if (!gnss_uart_read_sentence(sentence, sizeof(sentence), remaining_ms))
            continue;

        if (gnss_nmea_checksum_valid(sentence))
        {
            if (valid_sentences == 0)
                snprintf(sample, sizeof(sample), "%s", sentence);
            valid_sentences++;
        }
    }

    ESP_LOGW(TAG, "%lu baud: %u valid NMEA sentence(s)%s%s",
             (unsigned long)baud, (unsigned)valid_sentences,
             valid_sentences ? ", sample: " : "", sample);
    return valid_sentences > 0;
}

bool gnss_uart_detect_baud(uint32_t preferred_baud, uint32_t *detected_baud)
{
    gnss_uart_init();

    if (preferred_baud != 0 && baud_produces_valid_nmea(preferred_baud, 2500))
    {
        if (detected_baud != NULL)
            *detected_baud = preferred_baud;
        ESP_LOGI(TAG, "GNSS detected at the previously saved baud rate %lu.", (unsigned long)preferred_baud);
        return true;
    }

    for (size_t index = 0; index < s_candidate_baud_count; ++index)
    {
        uint32_t candidate_baud = s_candidate_baud_rates[index];
        if (candidate_baud == preferred_baud)
            continue;

        if (baud_produces_valid_nmea(candidate_baud, 2500))
        {
            if (detected_baud != NULL)
                *detected_baud = candidate_baud;
            ESP_LOGI(TAG, "GNSS detected at %lu baud.", (unsigned long)candidate_baud);
            return true;
        }
    }

    ESP_LOGW(TAG, "No GNSS NMEA data found at any tested baud rate. Check power, TX/RX wiring and signal levels.");
    return false;
}
