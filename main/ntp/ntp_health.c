// 本地 NTP 服务启动自检实现：通过 IPv4/IPv6 回环与已分配地址验证
// NTPv3、NTPv4 及 RFC 9769 交错行为（启用时含认证变体）。
// 原实现还会交叉校验 MQTT 遥测计数器，本移植版已移除 MQTT，故不做该项检查。



#include "ntp_health.h"

#include "app_config.h"
#include "ntp_auth.h"
#include "ntp_time.h"
#include "pps.h"
#include "time_state.h"
#include "wifi_link.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#if STARTUP_HEALTH_TEST_ENABLED

static const char *TAG = "ntp_health";

static const uint32_t HEALTH_TEST_READY_TIMEOUT_MS = 30000;
static const uint32_t HEALTH_TEST_RESPONSE_TIMEOUT_MS = 1000;
static const uint32_t HEALTH_TEST_STANDARD_RETRIES = 3;
static const uint64_t HEALTH_TEST_INTERLEAVED_TOLERANCE = (1ULL << 32) * 5ULL / 1000ULL;
static const size_t HEALTH_TEST_TASK_STACK_SIZE = 4096;
static const size_t HEALTH_TEST_LOG_CAPACITY = 64;
static const size_t HEALTH_TEST_LOG_MESSAGE_SIZE = 192;

typedef struct
{
    bool ntpv3_standard;
    bool ntpv4_standard;
    bool ntpv4_interleaved;
#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
    bool ntpv4_authenticated_standard;
    bool ntpv4_authenticated_interleaved;
    bool ntpv4_invalid_authentication_rejected;
    bool ntpv4_unknown_key_rejected;
    bool ntpv4_truncated_mac_rejected;
    bool ntpv4_altered_response_rejected;
#endif
} health_endpoint_results_t;

typedef struct
{
    bool prerequisites_ready;
    health_endpoint_results_t ipv4_loopback;
    health_endpoint_results_t ipv4_assigned;
    health_endpoint_results_t ipv6_loopback;
    health_endpoint_results_t ipv6_assigned;
} health_test_results_t;

typedef struct
{
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;
    uint64_t t4;
} health_exchange_t;

typedef struct
{
    esp_log_level_t level;
    char message[HEALTH_TEST_LOG_MESSAGE_SIZE];
} health_log_entry_t;

static health_log_entry_t s_health_log[HEALTH_TEST_LOG_CAPACITY];
static size_t s_health_log_count = 0;
static uint32_t s_health_test_number = 0;

// 将日志条目暂存到缓冲区
static void queue_health_log(esp_log_level_t level, const char *format, ...)
{
    if (s_health_log_count >= HEALTH_TEST_LOG_CAPACITY)
        return;

    health_log_entry_t *entry = &s_health_log[s_health_log_count++];
    entry->level = level;

    va_list arguments;
    va_start(arguments, format);
    vsnprintf(entry->message, sizeof(entry->message), format, arguments);
    va_end(arguments);
}

// 记录一项自检结果
static void queue_health_test_result(const char *endpoint, const char *test_name, bool passed)
{
    s_health_test_number++;
    queue_health_log(passed ? ESP_LOG_INFO : ESP_LOG_WARN,
                     "Health Check %02lu - %s: %s - %s",
                     (unsigned long)s_health_test_number, endpoint, test_name,
                     passed ? "Passed" : "Failed");
}

// 输出并清空暂存的日志
static void flush_health_log(void)
{
    for (size_t index = 0; index < s_health_log_count; ++index)
        ESP_LOG_LEVEL(s_health_log[index].level, TAG, "%s", s_health_log[index].message);
    s_health_log_count = 0;
}

// 等待并接收应答，同时记录到达时间
static bool receive_health_response(int socket_fd, uint8_t *reply, size_t reply_capacity, size_t *reply_length, uint64_t *arrival_time)
{
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = HEALTH_TEST_RESPONSE_TIMEOUT_MS / 1000;
    timeout.tv_usec = (suseconds_t)(HEALTH_TEST_RESPONSE_TIMEOUT_MS % 1000) * 1000;
    if (select(socket_fd + 1, &read_fds, NULL, NULL, &timeout) != 1)
        return false;

    int received_length = recvfrom(socket_fd, reply, reply_capacity, 0, NULL, NULL);
    if (received_length < 0)
        return false;

    *reply_length = (size_t)received_length;
    *arrival_time = ntp_now_64();
    return true;
}

// 执行标准（非交错）NTP 交互测试
static bool run_standard_health_test(int socket_fd, const struct sockaddr *destination, socklen_t destination_length,
                                     uint8_t version, health_exchange_t *exchange)
{
    uint8_t request[NTP_PACKET_SIZE];
    uint8_t reply[NTP_PACKET_SIZE];
    memset(request, 0, sizeof(request));
    memset(reply, 0, sizeof(reply));

    exchange->t1 = ntp_now_64();
    request[0] = (uint8_t)((version << 3) | 3);
    ntp_write_timestamp(request, 40, exchange->t1);

    for (uint32_t attempt = 0; attempt < HEALTH_TEST_STANDARD_RETRIES; ++attempt)
    {
        if (sendto(socket_fd, request, sizeof(request), 0, destination, destination_length) != (int)sizeof(request))
            continue;

        size_t reply_length = 0;
        if (!receive_health_response(socket_fd, reply, sizeof(reply), &reply_length, &exchange->t4) || reply_length != sizeof(reply))
            continue;

        if (((reply[0] >> 3) & 0x07) != version || (reply[0] & 0x07) != 4 || ntp_read_timestamp(reply, 24) != exchange->t1)
            continue;

        exchange->t2 = ntp_read_timestamp(reply, 32);
        exchange->t3 = ntp_read_timestamp(reply, 40);
        return exchange->t2 != 0 && exchange->t3 != 0;
    }

    return false;
}

// 执行 RFC 9769 交错交互测试
static bool run_interleaved_health_test(int socket_fd, const struct sockaddr *destination, socklen_t destination_length,
                                        uint8_t version, const health_exchange_t *previous_exchange)
{
    uint8_t request[NTP_PACKET_SIZE];
    uint8_t reply[NTP_PACKET_SIZE];
    memset(request, 0, sizeof(request));
    memset(reply, 0, sizeof(reply));

    size_t reply_length = 0;
    uint64_t arrival_time = 0;

    request[0] = (uint8_t)((version << 3) | 3);
    ntp_write_timestamp(request, 24, previous_exchange->t2);
    ntp_write_timestamp(request, 32, previous_exchange->t4);
    ntp_write_timestamp(request, 40, previous_exchange->t1);

    if (sendto(socket_fd, request, sizeof(request), 0, destination, destination_length) != (int)sizeof(request) ||
        !receive_health_response(socket_fd, reply, sizeof(reply), &reply_length, &arrival_time) || reply_length != sizeof(reply))
        return false;

    uint64_t reply_transmit_time = ntp_read_timestamp(reply, 40);
    uint64_t transmit_difference = reply_transmit_time >= previous_exchange->t3
                                       ? reply_transmit_time - previous_exchange->t3
                                       : previous_exchange->t3 - reply_transmit_time;
    return ((reply[0] >> 3) & 0x07) == version && (reply[0] & 0x07) == 4 &&
           ntp_read_timestamp(reply, 24) == previous_exchange->t4 &&
           transmit_difference <= HEALTH_TEST_INTERLEAVED_TOLERANCE;
}

#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
typedef enum
{
    HEALTH_INVALID_AUTH_WRONG_MAC,
    HEALTH_INVALID_AUTH_UNKNOWN_KEY,
    HEALTH_INVALID_AUTH_TRUNCATED_MAC,
} health_invalid_auth_t;

// 执行带认证的标准交互测试，并顺带验证被篡改的应答会遭拒绝
static bool run_authenticated_health_test(int socket_fd, const struct sockaddr *destination, socklen_t destination_length,
                                          health_exchange_t *exchange, bool *altered_response_rejected)
{
    uint8_t request[NTP_MAX_PACKET_SIZE];
    uint8_t reply[NTP_MAX_PACKET_SIZE];
    memset(request, 0, sizeof(request));
    memset(reply, 0, sizeof(reply));

    size_t request_length = NTP_PACKET_SIZE;
    exchange->t1 = ntp_now_64();
    request[0] = (uint8_t)((4U << 3) | 3U);
    ntp_write_timestamp(request, 40, exchange->t1);
    if (!ntp_auth_append_response(request, sizeof(request), &request_length, ntp_auth_default_key_id()))
        return false;

    for (uint32_t attempt = 0; attempt < HEALTH_TEST_STANDARD_RETRIES; ++attempt)
    {
        if (sendto(socket_fd, request, request_length, 0, destination, destination_length) != (int)request_length)
            continue;

        size_t reply_length = 0;
        if (!receive_health_response(socket_fd, reply, sizeof(reply), &reply_length, &exchange->t4) ||
            ntp_auth_verify_request(reply, reply_length, 4, NULL) != NTP_AUTH_VALID)
            continue;

        if (((reply[0] >> 3) & 0x07) != 4 || (reply[0] & 0x07) != 4 || ntp_read_timestamp(reply, 24) != exchange->t1)
            continue;

        reply[40] ^= 0x01;
        *altered_response_rejected = ntp_auth_verify_request(reply, reply_length, 4, NULL) == NTP_AUTH_INVALID_MAC;
        reply[40] ^= 0x01;

        exchange->t2 = ntp_read_timestamp(reply, 32);
        exchange->t3 = ntp_read_timestamp(reply, 40);
        return exchange->t2 != 0 && exchange->t3 != 0;
    }

    return false;
}

// 发送异常认证报文，验证其被丢弃（应无应答）
static bool run_invalid_authenticated_health_test(int socket_fd, const struct sockaddr *destination, socklen_t destination_length,
                                                  health_invalid_auth_t invalid_auth)
{
    uint8_t request[NTP_MAX_PACKET_SIZE];
    memset(request, 0, sizeof(request));

    size_t request_length = NTP_PACKET_SIZE;
    request[0] = (uint8_t)((4U << 3) | 3U);
    ntp_write_timestamp(request, 40, ntp_now_64());
    if (!ntp_auth_append_response(request, sizeof(request), &request_length, ntp_auth_default_key_id()))
        return false;

    switch (invalid_auth)
    {
    case HEALTH_INVALID_AUTH_WRONG_MAC:
        request[request_length - 1] ^= 0x01;
        break;
    case HEALTH_INVALID_AUTH_UNKNOWN_KEY:
        memset(request + NTP_PACKET_SIZE, 0, NTP_AUTH_KEY_ID_SIZE);
        break;
    case HEALTH_INVALID_AUTH_TRUNCATED_MAC:
        request_length--;
        break;
    }

    if (sendto(socket_fd, request, request_length, 0, destination, destination_length) != (int)request_length)
        return false;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = HEALTH_TEST_RESPONSE_TIMEOUT_MS / 1000;
    timeout.tv_usec = (suseconds_t)(HEALTH_TEST_RESPONSE_TIMEOUT_MS % 1000) * 1000;
    return select(socket_fd + 1, &read_fds, NULL, NULL, &timeout) == 0;
}

// 执行带认证的交错交互测试
static bool run_authenticated_interleaved_health_test(int socket_fd, const struct sockaddr *destination, socklen_t destination_length,
                                                      const health_exchange_t *previous_exchange)
{
    uint8_t request[NTP_MAX_PACKET_SIZE];
    uint8_t reply[NTP_MAX_PACKET_SIZE];
    memset(request, 0, sizeof(request));
    memset(reply, 0, sizeof(reply));

    size_t request_length = NTP_PACKET_SIZE;
    uint64_t arrival_time = 0;

    request[0] = (uint8_t)((4U << 3) | 3U);
    ntp_write_timestamp(request, 24, previous_exchange->t2);
    ntp_write_timestamp(request, 32, previous_exchange->t4);
    ntp_write_timestamp(request, 40, previous_exchange->t1);

    if (!ntp_auth_append_response(request, sizeof(request), &request_length, ntp_auth_default_key_id()) ||
        sendto(socket_fd, request, request_length, 0, destination, destination_length) != (int)request_length)
        return false;

    size_t reply_length = 0;
    if (!receive_health_response(socket_fd, reply, sizeof(reply), &reply_length, &arrival_time) ||
        ntp_auth_verify_request(reply, reply_length, 4, NULL) != NTP_AUTH_VALID)
        return false;

    uint64_t reply_transmit_time = ntp_read_timestamp(reply, 40);
    uint64_t transmit_difference = reply_transmit_time >= previous_exchange->t3
                                       ? reply_transmit_time - previous_exchange->t3
                                       : previous_exchange->t3 - reply_transmit_time;
    return (reply[0] & 0x07) == 4 && ntp_read_timestamp(reply, 24) == previous_exchange->t4 &&
           transmit_difference <= HEALTH_TEST_INTERLEAVED_TOLERANCE;
}
#endif // SYMMETRIC_KEY_AUTHENTICATION_ENABLED

// 对单个地址端点执行全部自检项
static health_endpoint_results_t run_health_endpoint_test(const char *name, const struct sockaddr *destination,
                                                          socklen_t destination_length, sa_family_t family)
{
    health_endpoint_results_t results;
    memset(&results, 0, sizeof(results));

    int socket_fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0)
    {
        queue_health_log(ESP_LOG_WARN, "Health Check %s: unable to create socket: errno %d", name, errno);
        queue_health_test_result(name, "NTPv3 standard     ", false);
        queue_health_test_result(name, "NTPv4 standard     ", false);
        queue_health_test_result(name, "NTPv4 interleaved  ", false);
#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
        if (ntp_auth_available())
        {
            queue_health_test_result(name, "NTPv4 auth standard", false);
            queue_health_test_result(name, "NTPv4 auth interlv ", false);
            queue_health_test_result(name, "NTPv4 invalid auth ", false);
            queue_health_test_result(name, "NTPv4 unknown key  ", false);
            queue_health_test_result(name, "NTPv4 truncated MAC", false);
            queue_health_test_result(name, "NTPv4 altered reply", false);
        }
#endif
        return results;
    }

    health_exchange_t ntpv3_exchange;
    health_exchange_t ntpv4_exchange;
    memset(&ntpv3_exchange, 0, sizeof(ntpv3_exchange));
    memset(&ntpv4_exchange, 0, sizeof(ntpv4_exchange));

    results.ntpv3_standard = run_standard_health_test(socket_fd, destination, destination_length, 3, &ntpv3_exchange);
    results.ntpv4_standard = run_standard_health_test(socket_fd, destination, destination_length, 4, &ntpv4_exchange);
    if (results.ntpv4_standard)
        results.ntpv4_interleaved = run_interleaved_health_test(socket_fd, destination, destination_length, 4, &ntpv4_exchange);

#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
    if (ntp_auth_available())
    {
        health_exchange_t authenticated_exchange;
        memset(&authenticated_exchange, 0, sizeof(authenticated_exchange));

        results.ntpv4_authenticated_standard =
            run_authenticated_health_test(socket_fd, destination, destination_length, &authenticated_exchange, &results.ntpv4_altered_response_rejected);
        if (results.ntpv4_authenticated_standard)
            results.ntpv4_authenticated_interleaved =
                run_authenticated_interleaved_health_test(socket_fd, destination, destination_length, &authenticated_exchange);

        results.ntpv4_invalid_authentication_rejected =
            run_invalid_authenticated_health_test(socket_fd, destination, destination_length, HEALTH_INVALID_AUTH_WRONG_MAC);
        results.ntpv4_unknown_key_rejected =
            run_invalid_authenticated_health_test(socket_fd, destination, destination_length, HEALTH_INVALID_AUTH_UNKNOWN_KEY);
        results.ntpv4_truncated_mac_rejected =
            run_invalid_authenticated_health_test(socket_fd, destination, destination_length, HEALTH_INVALID_AUTH_TRUNCATED_MAC);
    }
#endif

    closesocket(socket_fd);

    queue_health_test_result(name, "NTPv3 standard     ", results.ntpv3_standard);
    queue_health_test_result(name, "NTPv4 standard     ", results.ntpv4_standard);
    queue_health_test_result(name, "NTPv4 interleaved  ", results.ntpv4_interleaved);
#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
    if (ntp_auth_available())
    {
        queue_health_test_result(name, "NTPv4 auth standard", results.ntpv4_authenticated_standard);
        queue_health_test_result(name, "NTPv4 auth interlv ", results.ntpv4_authenticated_interleaved);
        queue_health_test_result(name, "NTPv4 invalid auth ", results.ntpv4_invalid_authentication_rejected);
        queue_health_test_result(name, "NTPv4 unknown key  ", results.ntpv4_unknown_key_rejected);
        queue_health_test_result(name, "NTPv4 truncated MAC", results.ntpv4_truncated_mac_rejected);
        queue_health_test_result(name, "NTPv4 altered reply", results.ntpv4_altered_response_rejected);
    }
#endif

    return results;
}

// 对回环与已分配地址执行全部自检
static health_test_results_t run_health_tests(void)
{
    health_test_results_t results;
    memset(&results, 0, sizeof(results));

    const EventBits_t ready_bits = WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT | WIFI_GOT_IP6_BIT;
    const EventBits_t ready = xEventGroupWaitBits(wifi_link_event_group(), ready_bits, pdFALSE, pdTRUE,
                                                  pdMS_TO_TICKS(HEALTH_TEST_READY_TIMEOUT_MS));
    if ((ready & ready_bits) != ready_bits || !time_state_has_been_set() || !pps_discipline_active())
    {
        queue_health_log(ESP_LOG_ERROR, "Health Check prerequisites were not ready before timeout");
        return results;
    }

    results.prerequisites_ready = true;

    struct sockaddr_in ipv4_loopback;
    memset(&ipv4_loopback, 0, sizeof(ipv4_loopback));
    ipv4_loopback.sin_family = AF_INET;
    ipv4_loopback.sin_port = htons(NTP_PORT);
    ipv4_loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    results.ipv4_loopback = run_health_endpoint_test("IPv4 loopback", (const struct sockaddr *)&ipv4_loopback, sizeof(ipv4_loopback), AF_INET);

    struct sockaddr_in ipv4_assigned;
    memset(&ipv4_assigned, 0, sizeof(ipv4_assigned));
    ipv4_assigned.sin_family = AF_INET;
    ipv4_assigned.sin_port = htons(NTP_PORT);
    if (inet_pton(AF_INET, wifi_link_ipv4(), &ipv4_assigned.sin_addr) == 1)
        results.ipv4_assigned = run_health_endpoint_test("IPv4 assigned", (const struct sockaddr *)&ipv4_assigned, sizeof(ipv4_assigned), AF_INET);
    else
        queue_health_log(ESP_LOG_ERROR, "Health Check IPv4 assigned: invalid address");

    struct sockaddr_in6 ipv6_loopback;
    memset(&ipv6_loopback, 0, sizeof(ipv6_loopback));
    ipv6_loopback.sin6_family = AF_INET6;
    ipv6_loopback.sin6_port = htons(NTP_PORT);
    if (inet_pton(AF_INET6, "::1", &ipv6_loopback.sin6_addr) != 1)
    {
        queue_health_log(ESP_LOG_ERROR, "Health Check IPv6 loopback: unable to initialize address");
        return results;
    }
    results.ipv6_loopback = run_health_endpoint_test("IPv6 loopback", (const struct sockaddr *)&ipv6_loopback, sizeof(ipv6_loopback), AF_INET6);

    struct sockaddr_in6 ipv6_assigned;
    memset(&ipv6_assigned, 0, sizeof(ipv6_assigned));
    ipv6_assigned.sin6_family = AF_INET6;
    ipv6_assigned.sin6_port = htons(NTP_PORT);
    ipv6_assigned.sin6_scope_id = esp_netif_get_netif_impl_index(wifi_link_netif());
    if (inet_pton(AF_INET6, wifi_link_ipv6(), &ipv6_assigned.sin6_addr) == 1)
        results.ipv6_assigned = run_health_endpoint_test("IPv6 assigned", (const struct sockaddr *)&ipv6_assigned, sizeof(ipv6_assigned), AF_INET6);
    else
        queue_health_log(ESP_LOG_ERROR, "Health Check IPv6 assigned: invalid address");

    return results;
}

// 自检任务入口
static void startup_health_test_task(void *parameter)
{
    queue_health_log(ESP_LOG_INFO, "Health Check started");

    health_test_results_t results = run_health_tests();

    queue_health_log(ESP_LOG_INFO, "Health Check %s", results.prerequisites_ready ? "completed" : "failed before testing");
    flush_health_log();

    if (parameter != NULL)
        xTaskNotifyGive((TaskHandle_t)parameter);
    vTaskDelete(NULL);
}

#endif // STARTUP_HEALTH_TEST_ENABLED

void ntp_health_run(void)
{
#if STARTUP_HEALTH_TEST_ENABLED
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(startup_health_test_task, "ntp_health", HEALTH_TEST_TASK_STACK_SIZE,
                                current_task, 5, NULL, tskNO_AFFINITY) == pdPASS)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));
#endif
}
