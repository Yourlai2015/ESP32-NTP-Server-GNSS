// ESP32-S3 NTP 时间服务器
// WiFi Station 链路实现：初始化 netif 与事件处理器，处理连接/断连及 IP 获取事件，
// 维护 IPv4/IPv6 地址并对外提供查询接口。


#include "wifi_link.h"

#include "app_config.h"
#include "wifi_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi_link";

static EventGroupHandle_t s_net_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;

static volatile bool s_connected = false;
static char s_ssid[33] = "";
static char s_ipv4_address[IP_ADDRESS_TEXT_SIZE] = "";
static char s_ipv6_address[IP_ADDRESS_TEXT_SIZE] = "";
static char s_primary_address[IP_ADDRESS_TEXT_SIZE] = "";

static void update_primary_address(void)
{
    const char *selected = "";
    if (s_ipv4_address[0] != '\0')
        selected = s_ipv4_address;
    else if (s_ipv6_address[0] != '\0')
        selected = s_ipv6_address;

    snprintf(s_primary_address, sizeof(s_primary_address), "%s", selected);
}

// ESP-IDF 的 WiFi 配置字段是定长字节数组、无需结尾 NUL，按字段长度截断拷贝即可。
static void copy_to_wifi_field(uint8_t *destination, size_t destination_size, const char *source)
{
    size_t length = strlen(source);
    if (length > destination_size)
        length = destination_size;
    memcpy(destination, source, length);
}

static bool ipv6_address_is_link_local(const esp_ip6_addr_t *address)
{
    return (address->addr[0] & 0xFF) == 0xFE && (address->addr[1] & 0xC0) == 0x80;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "WiFi station started; connecting to SSID \"%s\"", s_ssid);
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        s_connected = true;
        xEventGroupSetBits(s_net_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi link connected");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_connected = false;
        s_ipv4_address[0] = '\0';
        s_ipv6_address[0] = '\0';
        update_primary_address();
        xEventGroupClearBits(s_net_event_group, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT | WIFI_GOT_IP6_BIT);
        ESP_LOGW(TAG, "WiFi disconnected; reconnecting");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ipv4_address, sizeof(s_ipv4_address), IPSTR, IP2STR(&event->ip_info.ip));
        update_primary_address();
        xEventGroupSetBits(s_net_event_group, WIFI_GOT_IP_BIT);
        ESP_LOGI(TAG, "WiFi IPv4 acquired: %s", s_ipv4_address);

        if (s_sta_netif != NULL)
        {
            esp_err_t err = esp_netif_create_ip6_linklocal(s_sta_netif);
            if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_IP6_ADDR_FAILED)
                ESP_LOGW(TAG, "Unable to create IPv6 link-local address: %s", esp_err_to_name(err));
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6)
    {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;

        char address_text[IP_ADDRESS_TEXT_SIZE] = "";
        if (inet_ntop(AF_INET6, &event->ip6_info.ip, address_text, sizeof(address_text)) == NULL)
            return;

        // 优先使用可路由的全局地址，否则回退到链路本地地址；两者都可服务 IPv6 NTP 客户端。
        if (ipv6_address_is_link_local(&event->ip6_info.ip))
        {
            if (s_ipv6_address[0] == '\0')
                snprintf(s_ipv6_address, sizeof(s_ipv6_address), "%s", address_text);
        }
        else
        {
            snprintf(s_ipv6_address, sizeof(s_ipv6_address), "%s", address_text);
        }

        update_primary_address();
        xEventGroupSetBits(s_net_event_group, WIFI_GOT_IP6_BIT);
        ESP_LOGI(TAG, "WiFi IPv6 acquired: %s", address_text);
    }
}

void wifi_link_init(void)
{
    if (s_net_event_group == NULL)
        s_net_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    // 配置页面保存的凭据优先于 app_config.h 中的编译期值。
    char ssid[33] = "";
    char password[65] = "";
    if (wifi_config_load(ssid, sizeof(ssid), password, sizeof(password)))
        ESP_LOGI(TAG, "Using the WiFi credentials stored by the setup page");
    else
    {
        snprintf(ssid, sizeof(ssid), "%s", WIFI_SSID);
        snprintf(password, sizeof(password), "%s", WIFI_PASSWORD);
        ESP_LOGI(TAG, "Using the WiFi credentials from app_config.h");
    }

    wifi_config_t wifi_config = {0};
    copy_to_wifi_field(wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), ssid);
    copy_to_wifi_field(wifi_config.sta.password, sizeof(wifi_config.sta.password), password);
    // 空密码表示开放网络，若用 WPA2_PSK 会被拒绝。
    wifi_config.sta.threshold.authmode = password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 保持射频满功率，保证时间源稳定
    esp_wifi_set_ps(WIFI_PS_NONE);
}

bool wifi_link_apply_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0')
        return false;

    wifi_config_t wifi_config = {0};
    copy_to_wifi_field(wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), ssid);
    if (password != NULL)
        copy_to_wifi_field(wifi_config.sta.password, sizeof(wifi_config.sta.password), password);
    wifi_config.sta.threshold.authmode =
        (password != NULL && password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);

    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK)
        return false;

    // 断开当前连接（若有）并用新配置重连。故意忽略错误：Station 可能已断开并正在重试，
    // 此时 disconnect 会报错，但下面的 connect 仍需执行。
    esp_wifi_disconnect();
    esp_wifi_connect();
    return true;
}

void wifi_link_wait_for_ip(void)
{
    EventBits_t bits = xEventGroupWaitBits(s_net_event_group,
                                           WIFI_GOT_IP_BIT | WIFI_GOT_IP6_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(30000));
    if ((bits & (WIFI_GOT_IP_BIT | WIFI_GOT_IP6_BIT)) == 0)
        ESP_LOGW(TAG, "No IP address acquired yet; continuing without a confirmed link.");
}

bool wifi_link_is_connected(void)
{
    return s_connected;
}

bool wifi_link_has_ip(void)
{
    EventBits_t bits = xEventGroupGetBits(s_net_event_group);
    return (bits & (WIFI_GOT_IP_BIT | WIFI_GOT_IP6_BIT)) != 0;
}

const char *wifi_link_ipv4(void)
{
    return s_ipv4_address;
}

const char *wifi_link_ipv6(void)
{
    return s_ipv6_address;
}

const char *wifi_link_primary_ip(void)
{
    return s_primary_address;
}

esp_netif_t *wifi_link_netif(void)
{
    return s_sta_netif;
}

EventGroupHandle_t wifi_link_event_group(void)
{
    return s_net_event_group;
}
