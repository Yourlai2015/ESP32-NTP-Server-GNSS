// NTP 服务器主逻辑：创建监听套接字（IPv4 / IPv6 / IPv6 链路本地），
// 接收并校验 NTP 请求、生成应答（含 RFC 9769 交错模式与对称密钥认证）。


#include "ntp_server.h"

#include "app_config.h"
#include "ntp_auth.h"
#include "ntp_cache.h"
#include "ntp_time.h"
#include "sync_state.h"
#include "time_state.h"
#include "wifi_link.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "ntp_server";

static atomic_bool s_ntp_server_ready = false;
static atomic_bool s_external_responses_enabled = false;

#if DEBUG_ENABLED
// 将套接字地址格式化为可读文本（调试用）
static void format_socket_address(const struct sockaddr_storage *address, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0)
        return;
    buffer[0] = '\0';

    if (address->ss_family == AF_INET)
    {
        const struct sockaddr_in *address_ipv4 = (const struct sockaddr_in *)address;
        char text[IP_ADDRESS_TEXT_SIZE] = "";
        inet_ntop(AF_INET, &address_ipv4->sin_addr, text, sizeof(text));
        snprintf(buffer, buffer_size, "%s:%u", text, (unsigned)ntohs(address_ipv4->sin_port));
    }
    else if (address->ss_family == AF_INET6)
    {
        const struct sockaddr_in6 *address_ipv6 = (const struct sockaddr_in6 *)address;
        char text[IP_ADDRESS_TEXT_SIZE] = "";
        inet_ntop(AF_INET6, &address_ipv6->sin6_addr, text, sizeof(text));
        snprintf(buffer, buffer_size, "[%s]:%u", text, (unsigned)ntohs(address_ipv6->sin6_port));
    }
}
#endif

// 判断是否为本机（回环或已分配）IPv4 地址
static bool is_internal_ntp_ipv4_address(const struct in_addr *address)
{
    if (address->s_addr == htonl(INADDR_LOOPBACK))
        return true;

    struct in_addr assigned_address;
    const char *own_address = wifi_link_ipv4();
    return own_address[0] != '\0' && inet_pton(AF_INET, own_address, &assigned_address) == 1 &&
           address->s_addr == assigned_address.s_addr;
}

// 判断是否为本机（回环或已分配）IPv6 地址
static bool is_internal_ntp_ipv6_address(const struct in6_addr *address)
{
    struct in6_addr loopback_address;
    if (inet_pton(AF_INET6, "::1", &loopback_address) == 1 &&
        memcmp(address, &loopback_address, sizeof(*address)) == 0)
        return true;

    struct in6_addr assigned_address;
    const char *own_address = wifi_link_ipv6();
    return own_address[0] != '\0' && inet_pton(AF_INET6, own_address, &assigned_address) == 1 &&
           memcmp(address, &assigned_address, sizeof(*address)) == 0;
}

// 判断请求是否来自本机地址
static bool is_internal_ntp_client(const struct sockaddr_storage *address)
{
    if (address->ss_family == AF_INET)
        return is_internal_ntp_ipv4_address(&((const struct sockaddr_in *)address)->sin_addr);
    if (address->ss_family == AF_INET6)
        return is_internal_ntp_ipv6_address(&((const struct sockaddr_in6 *)address)->sin6_addr);
    return false;
}

// NTP 服务器主任务：绑定套接字并循环收发请求
static void ntp_server_task(void *parameter)
{
    TaskHandle_t startup_task = (TaskHandle_t)parameter;

    int ipv4_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int ipv6_socket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    int ipv6_link_local_socket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);

    struct sockaddr_in ipv4_listen_addr;
    struct sockaddr_in6 ipv6_listen_addr;
    struct sockaddr_in6 ipv6_link_local_listen_addr;
    esp_ip6_addr_t link_local_address;
    esp_netif_t *netif = wifi_link_netif();
    int ipv6_only = 1;
    int reuse_address = 1;

    memset(&ipv4_listen_addr, 0, sizeof(ipv4_listen_addr));
    memset(&ipv6_listen_addr, 0, sizeof(ipv6_listen_addr));
    memset(&ipv6_link_local_listen_addr, 0, sizeof(ipv6_link_local_listen_addr));
    memset(&link_local_address, 0, sizeof(link_local_address));

    if (ipv4_socket < 0 || ipv6_socket < 0)
    {
        ESP_LOGE(TAG, "Unable to create NTP UDP sockets: errno %d", errno);
        goto fail;
    }

    ipv4_listen_addr.sin_family = AF_INET;
    ipv4_listen_addr.sin_port = htons(NTP_PORT);
    ipv4_listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (setsockopt(ipv6_socket, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_only, sizeof(ipv6_only)) != 0 ||
        setsockopt(ipv6_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) != 0 ||
        bind(ipv4_socket, (struct sockaddr *)&ipv4_listen_addr, sizeof(ipv4_listen_addr)) != 0)
    {
        ESP_LOGE(TAG, "Unable to configure NTP UDP sockets: errno %d", errno);
        goto fail;
    }

    ipv6_listen_addr.sin6_family = AF_INET6;
    ipv6_listen_addr.sin6_port = htons(NTP_PORT);
    ipv6_listen_addr.sin6_addr = in6addr_any;
    if (bind(ipv6_socket, (struct sockaddr *)&ipv6_listen_addr, sizeof(ipv6_listen_addr)) != 0)
    {
        ESP_LOGE(TAG, "Unable to bind IPv6 NTP UDP socket: errno %d", errno);
        goto fail;
    }

    if (ipv6_link_local_socket < 0 || netif == NULL ||
        setsockopt(ipv6_link_local_socket, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_only, sizeof(ipv6_only)) != 0 ||
        setsockopt(ipv6_link_local_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) != 0 ||
        esp_netif_get_ip6_linklocal(netif, &link_local_address) != ESP_OK)
    {
#if DEBUG_ENABLED
        ESP_LOGW(TAG, "IPv6 link-local NTP listener unavailable: errno %d", errno);
#endif
        if (ipv6_link_local_socket >= 0)
            closesocket(ipv6_link_local_socket);
        ipv6_link_local_socket = -1;
    }
    else
    {
        ipv6_link_local_listen_addr.sin6_family = AF_INET6;
        ipv6_link_local_listen_addr.sin6_port = htons(NTP_PORT);
        memcpy(&ipv6_link_local_listen_addr.sin6_addr, link_local_address.addr, sizeof(ipv6_link_local_listen_addr.sin6_addr));
        ipv6_link_local_listen_addr.sin6_scope_id = esp_netif_get_netif_impl_index(netif);
        if (bind(ipv6_link_local_socket,
                 (struct sockaddr *)&ipv6_link_local_listen_addr,
                 sizeof(ipv6_link_local_listen_addr)) != 0)
        {
#if DEBUG_ENABLED
            ESP_LOGW(TAG, "IPv6 link-local NTP listener unavailable: errno %d", errno);
#endif
            closesocket(ipv6_link_local_socket);
            ipv6_link_local_socket = -1;
        }
    }

    atomic_store(&s_ntp_server_ready, true);
    if (startup_task != NULL)
        xTaskNotifyGive(startup_task);

#if DEBUG_ENABLED
    ESP_LOGI(TAG, "NTP server listening on UDP %d (IPv4, IPv6%s).", NTP_PORT, ipv6_link_local_socket >= 0 ? ", link-local" : "");
#endif

    for (;;)
    {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(ipv4_socket, &read_fds);
        FD_SET(ipv6_socket, &read_fds);
        if (ipv6_link_local_socket >= 0)
            FD_SET(ipv6_link_local_socket, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int max_socket = ipv4_socket;
        if (ipv6_socket > max_socket)
            max_socket = ipv6_socket;
        if (ipv6_link_local_socket >= 0 && ipv6_link_local_socket > max_socket)
            max_socket = ipv6_link_local_socket;

        int ready = select(max_socket + 1, &read_fds, NULL, NULL, &timeout);
        if (ready < 0)
        {
#if DEBUG_ENABLED
            ESP_LOGW(TAG, "NTP socket select failed: errno %d", errno);
#endif
            continue;
        }
        if (ready == 0)
            continue;

        static uint8_t next_socket = 0;
        const int sockets[] = {ipv4_socket, ipv6_socket, ipv6_link_local_socket};
        const size_t socket_count = sizeof(sockets) / sizeof(sockets[0]);
        uint8_t start_socket = next_socket;

        for (size_t offset = 0; offset < socket_count; ++offset)
        {
            size_t index = (start_socket + offset) % socket_count;
            int sock = sockets[index];
            if (sock < 0 || !FD_ISSET(sock, &read_fds))
                continue;

            next_socket = (uint8_t)((index + 1) % socket_count);
            for (size_t batch_count = 0; batch_count < NTP_SOCKET_BATCH_LIMIT; ++batch_count)
            {
                uint8_t request[NTP_MAX_PACKET_SIZE + 1];
                uint8_t reply[NTP_MAX_PACKET_SIZE];
                struct sockaddr_storage source_addr;
                socklen_t source_addr_len = sizeof(source_addr);

                memset(reply, 0, sizeof(reply));
                memset(&source_addr, 0, sizeof(source_addr));

                int len = recvfrom(sock, request, sizeof(request), MSG_DONTWAIT,
                                   (struct sockaddr *)&source_addr, &source_addr_len);
                if (len < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
#if DEBUG_ENABLED
                    ESP_LOGW(TAG, "recvfrom failed: errno %d", errno);
#endif
                    break;
                }

                uint64_t receive_time = ntp_now_64();
                if (len < (int)NTP_PACKET_SIZE || len > (int)NTP_MAX_PACKET_SIZE)
                    continue;

                uint8_t ntp_version = (request[0] >> 3) & 0x07;
                uint8_t ntp_mode = request[0] & 0x07;
                if (ntp_version < 3 || ntp_version > 4 || ntp_mode != 3)
                    continue;

#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
                uint32_t authentication_key_id = 0;
                ntp_auth_result_t authentication_result =
                    ntp_auth_verify_request(request, (size_t)len, ntp_version, &authentication_key_id);
                if (authentication_result != NTP_AUTH_UNAUTHENTICATED && authentication_result != NTP_AUTH_VALID)
                    continue;
                bool authenticated_request = authentication_result == NTP_AUTH_VALID;
#endif

                if (!atomic_load(&s_external_responses_enabled) && !is_internal_ntp_client(&source_addr))
                    continue;

                const struct sockaddr_in *source_ipv4 = NULL;
                const struct in6_addr *client_ipv6 = NULL;
                ntp_client_record_t client_ipv4_record;
                ntp_client_record_ipv6_t client_ipv6_record;
                memset(&client_ipv4_record, 0, sizeof(client_ipv4_record));
                memset(&client_ipv6_record, 0, sizeof(client_ipv6_record));

                if (source_addr.ss_family == AF_INET)
                {
                    source_ipv4 = (const struct sockaddr_in *)&source_addr;
                    ntp_cache_find_or_create(ntohl(source_ipv4->sin_addr.s_addr), ntohs(source_ipv4->sin_port), &client_ipv4_record);
                }
                else if (source_addr.ss_family == AF_INET6)
                {
                    client_ipv6 = &((const struct sockaddr_in6 *)&source_addr)->sin6_addr;
                    ntp_cache_find_or_create_ipv6(client_ipv6,
                                                  ntohs(((const struct sockaddr_in6 *)&source_addr)->sin6_port),
                                                  &client_ipv6_record);
                }

                ntp_reply_status_t status = ntp_get_reply_status();
                ntp_build_reply(request, reply, ntp_version, receive_time, &status);

                uint64_t client_receive_timestamp = ntp_read_timestamp(request, 32);
                uint64_t client_origin_timestamp = ntp_read_timestamp(request, 24);
                uint64_t client_transmit_timestamp = ntp_read_timestamp(request, 40);

                uint64_t previous_ipv4_t2 = client_ipv4_record.prev_t2;
                uint64_t previous_ipv4_t3 = client_ipv4_record.prev_t3;
                uint64_t ipv4_t2_difference = client_origin_timestamp >= previous_ipv4_t2
                                                  ? client_origin_timestamp - previous_ipv4_t2
                                                  : previous_ipv4_t2 - client_origin_timestamp;
                bool ipv4_interleaved_reply = source_ipv4 != NULL && previous_ipv4_t2 != 0 &&
                                              client_receive_timestamp != client_transmit_timestamp &&
                                              ipv4_t2_difference <= NTP_CACHE_TIMESTAMP_MATCH_TOLERANCE;
                bool ipv6_interleaved_reply = client_ipv6 != NULL && client_ipv6_record.prev_t2 != 0 &&
                                              client_receive_timestamp != client_transmit_timestamp &&
                                              client_origin_timestamp == client_ipv6_record.prev_t2;
                bool interleaved_reply = ipv4_interleaved_reply || ipv6_interleaved_reply;

                uint64_t transmit_time = ntp_now_64();
                if (interleaved_reply)
                {
                    ntp_write_timestamp(reply, 24, client_receive_timestamp);
                    ntp_write_timestamp(reply, 40, ipv4_interleaved_reply ? previous_ipv4_t3 : client_ipv6_record.prev_t3);
                }
                else
                {
                    ntp_write_timestamp(reply, 40, transmit_time);
                }

                size_t reply_length = NTP_PACKET_SIZE;
#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
                if (authenticated_request && !ntp_auth_append_response(reply, sizeof(reply), &reply_length, authentication_key_id))
                    continue;
#endif

                int sent = sendto(sock, reply, reply_length, 0, (struct sockaddr *)&source_addr, source_addr_len);
                if (sent == (int)reply_length)
                {
                    if (source_ipv4 != NULL)
                        ntp_cache_update(ntohl(source_ipv4->sin_addr.s_addr), ntohs(source_ipv4->sin_port),
                                         receive_time, ntp_read_timestamp(reply, 40));
                    else if (client_ipv6 != NULL)
                        ntp_cache_update_ipv6(client_ipv6,
                                              ntohs(((const struct sockaddr_in6 *)&source_addr)->sin6_port),
                                              receive_time, ntp_read_timestamp(reply, 40));
                }

#if DEBUG_ENABLED
                char source_address[IP_ADDRESS_TEXT_SIZE + 12] = "";
                format_socket_address(&source_addr, source_address, sizeof(source_address));
                ESP_LOGI(TAG, "NTP -> %s", source_address);
#endif
            }
        }
    }

fail:
    if (ipv4_socket >= 0)
        closesocket(ipv4_socket);
    if (ipv6_socket >= 0)
        closesocket(ipv6_socket);
    if (ipv6_link_local_socket >= 0)
        closesocket(ipv6_link_local_socket);

    atomic_store(&s_ntp_server_ready, false);
    if (startup_task != NULL)
        xTaskNotifyGive(startup_task);
    vTaskDelete(NULL);
}

// 定期清理过期的客户端缓存条目
void ntp_cache_purge_task(void *parameter)
{
    (void)parameter;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ntp_cache_purge_expired();
    }
}

// 启动 NTP 服务器及清理任务，并等待监听就绪
void ntp_server_start(void)
{
    ntp_cache_init();

    xTaskCreatePinnedToCore(ntp_cache_purge_task, "ntp_cache_purge", NTP_CACHE_PURGE_TASK_STACK_SIZE, NULL, 5, NULL, tskNO_AFFINITY);

    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    xTaskCreatePinnedToCore(ntp_server_task, "ntp_server", NTP_SERVER_TASK_STACK_SIZE, current_task, 20, NULL, tskNO_AFFINITY);

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
}

void ntp_server_set_external_responses(bool enabled)
{
    atomic_store(&s_external_responses_enabled, enabled);
}

bool ntp_server_external_responses_enabled(void)
{
    return atomic_load(&s_external_responses_enabled);
}

bool ntp_server_is_ready(void)
{
    return atomic_load(&s_ntp_server_ready);
}
