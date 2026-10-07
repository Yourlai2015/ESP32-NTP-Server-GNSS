// ESP32-S3 NTP 时间服务器
// WiFi 配网热点与配置网页的实现：开启 SoftAP 与 HTTP 服务器，提供扫描 AP、保存凭据的接口；保存后延迟关闭热点，并让 Station 用新凭据重连。

#include "wifi_provision.h"

#include "app_config.h"
#include "wifi_config.h"
#include "wifi_link.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_provision";

#define PROVISION_HTTP_PORT 80
#define PROVISION_MAX_SCAN_RESULTS 20
#define PROVISION_SCAN_JSON_SIZE 1024
#define PROVISION_FORM_MAX_SIZE 256
// 关闭热点前，留给确认页面送达手机的时间。
#define PROVISION_RESPONSE_DELAY_MS 800
// “已保存、重连中”界面的最长显示时长。
#define PROVISION_SAVED_DISPLAY_US (60LL * 1000000LL)

static const char *CONFIG_PAGE_HTML =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>时间服务器 WiFi 设置</title>"
    "<link rel=\"icon\" href=\"data:,\"><style>"
    "body{font-family:-apple-system,Helvetica,Arial,sans-serif;background:#f5f6f8;color:#222;margin:0;padding:24px}"
    "form{max-width:420px;margin:0 auto;background:#fff;border-radius:12px;padding:20px;box-shadow:0 1px 4px rgba(0,0,0,.12)}"
    "h1{font-size:19px;margin:0 0 4px}p.sub{font-size:13px;color:#888;margin:0 0 14px}"
    "label{display:block;font-size:14px;color:#555;margin:14px 0 6px}"
    "input{width:100%;box-sizing:border-box;padding:10px 12px;font-size:16px;border:1px solid #ccd2dc;border-radius:8px}"
    "button{width:100%;margin-top:22px;padding:12px;font-size:16px;border:0;border-radius:8px;background:#2f6fed;color:#fff}"
    "p.hint{font-size:12px;color:#999;margin:12px 0 0}"
    "</style></head><body><form method=\"POST\" action=\"/save\">"
    "<h1>ESP32 时间服务器</h1><p class=\"sub\">设置要连接的 WiFi</p>"
    "<label for=\"ssid\">WiFi 名称 (SSID)</label>"
    "<input id=\"ssid\" name=\"ssid\" list=\"ap-list\" maxlength=\"32\" required placeholder=\"选择或输入 WiFi 名称\">"
    "<datalist id=\"ap-list\"></datalist>"
    "<label for=\"password\">WiFi 密码（开放网络留空）</label>"
    "<input id=\"password\" name=\"password\" type=\"password\" maxlength=\"64\" placeholder=\"至少 8 位\">"
    "<button type=\"submit\">保存</button>"
    "<p class=\"hint\">保存后设备会关闭热点并连接新的 WiFi。</p></form>"
    "<script>fetch('/scan').then(function(r){return r.json()}).then(function(d){"
    "var l=document.getElementById('ap-list');(d.aps||[]).forEach(function(s){"
    "var o=document.createElement('option');o.value=s;l.appendChild(o)})}).catch(function(){})</script>"
    "</body></html>";

static const char *SAVED_PAGE_HTML =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>已保存</title>"
    "<link rel=\"icon\" href=\"data:,\"></head>"
    "<body style=\"font-family:sans-serif;text-align:center;padding:40px\">"
    "<h2>设置已保存</h2><p>热点即将关闭，设备正在连接新的 WiFi…</p></body></html>";

static bool s_active = false;
static bool s_saved = false;
static bool s_shutdown_pending = false;

static httpd_handle_t s_server = NULL;
static esp_netif_t *s_ap_netif = NULL;

static char s_ap_ssid[33] = "";
static char s_saved_ssid[33] = "";
static char s_saved_password[65] = "";
static int64_t s_saved_at_us = 0;

static const char *s_ap_ip = "192.168.4.1";

// ESP-IDF 的 WiFi 配置字段是定长字节数组、无需结尾 NUL，按字段长度截断拷贝即可。
static void copy_to_wifi_field(uint8_t *destination, size_t destination_size, const char *source)
{
    size_t length = strlen(source);
    if (length > destination_size)
        length = destination_size;
    memcpy(destination, source, length);
}

static void url_decode_in_place(char *text)
{
    char *write = text;

    for (char *read = text; *read != '\0'; ++read)
    {
        if (*read == '+')
        {
            *write++ = ' ';
        }
        else if (*read == '%' && isxdigit((unsigned char)read[1]) && isxdigit((unsigned char)read[2]))
        {
            const char hex[3] = {read[1], read[2], '\0'};
            *write++ = (char)strtol(hex, NULL, 16);
            read += 2;
        }
        else
        {
            *write++ = *read;
        }
    }

    *write = '\0';
}

// 从 application/x-www-form-urlencoded 请求体中取出指定键的值。
static bool form_get_value(const char *body, const char *key, char *out, size_t out_size)
{
    const size_t key_length = strlen(key);
    const char *cursor = body;

    while (cursor != NULL && *cursor != '\0')
    {
        if (strncmp(cursor, key, key_length) == 0 && cursor[key_length] == '=')
        {
            const char *value = cursor + key_length + 1;
            const char *end = strchr(value, '&');
            size_t length = end != NULL ? (size_t)(end - value) : strlen(value);
            if (length >= out_size)
                length = out_size - 1;

            memcpy(out, value, length);
            out[length] = '\0';
            url_decode_in_place(out);
            return true;
        }

        cursor = strchr(cursor, '&');
        if (cursor != NULL)
            cursor++;
    }

    return false;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, CONFIG_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static size_t json_append_escaped(char *out, size_t out_size, size_t position, const char *text)
{
    for (const char *cursor = text; *cursor != '\0' && position + 2 < out_size; ++cursor)
    {
        const unsigned char ch = (unsigned char)*cursor;

        if (ch == '"' || ch == '\\')
        {
            out[position++] = '\\';
            out[position++] = (char)ch;
        }
        else if (ch >= 0x20 && ch != 0x7f)
        {
            out[position++] = (char)ch;
        }
    }

    return position;
}

// 扫描周边 WiFi，供手机端列出可选 SSID。
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    static char json[PROVISION_SCAN_JSON_SIZE];

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"aps\":[]}", HTTPD_RESP_USE_STRLEN);
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > PROVISION_MAX_SCAN_RESULTS)
        ap_count = PROVISION_MAX_SCAN_RESULTS;

    size_t position = (size_t)snprintf(json, sizeof(json), "{\"aps\":[");

    if (ap_count > 0)
    {
        wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (records != NULL)
        {
            uint16_t record_count = ap_count;
            if (esp_wifi_scan_get_ap_records(&record_count, records) == ESP_OK)
            {
                for (uint16_t index = 0; index < record_count; ++index)
                {
                    if (records[index].ssid[0] == '\0' || position >= sizeof(json) - 40)
                        continue;

                    if (position > strlen("{\"aps\":["))
                        json[position++] = ',';
                    json[position++] = '"';
                    position = json_append_escaped(json, sizeof(json), position, (const char *)records[index].ssid);
                    json[position++] = '"';
                }
            }
            free(records);
        }
    }

    if (position < sizeof(json) - 4)
        position += (size_t)snprintf(json + position, sizeof(json) - position, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, position);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[PROVISION_FORM_MAX_SIZE];

    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33] = "";
    if (!form_get_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0')
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }

    char password[65] = "";
    form_get_value(body, "password", password, sizeof(password));
    const size_t password_length = strlen(password);
    if (password_length > 0 && password_length < 8)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WPA2 password must be at least 8 characters");
        return ESP_FAIL;
    }

    if (!wifi_config_save(ssid, password))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to store the credentials");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Stored credentials for SSID \"%s\"", ssid);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, SAVED_PAGE_HTML, HTTPD_RESP_USE_STRLEN);

    // 先让确认页面送达手机，再把关闭动作交给 wifi_provision_poll()：
    // 在处理函数内停止 Web 服务器会造成死锁。
    vTaskDelay(pdMS_TO_TICKS(PROVISION_RESPONSE_DELAY_MS));

    snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", ssid);
    snprintf(s_saved_password, sizeof(s_saved_password), "%s", password);
    s_shutdown_pending = true;

    return ESP_OK;
}

bool wifi_provision_start(void)
{
    if (s_active)
        return true;

    if (s_ap_netif == NULL)
    {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL)
        {
            ESP_LOGE(TAG, "Unable to create the access point network interface");
            return false;
        }
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X", PROVISION_AP_SSID_PREFIX, mac[4], mac[5]);

    const size_t password_length = strlen(PROVISION_AP_PASSWORD);

    wifi_config_t ap_config = {0};
    copy_to_wifi_field(ap_config.ap.ssid, sizeof(ap_config.ap.ssid), s_ap_ssid);
    copy_to_wifi_field(ap_config.ap.password, sizeof(ap_config.ap.password), PROVISION_AP_PASSWORD);
    ap_config.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = password_length >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK)
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Unable to start the access point: %s", esp_err_to_name(err));
        return false;
    }

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.server_port = PROVISION_HTTP_PORT;
    server_config.max_uri_handlers = 3;
    server_config.stack_size = 8192;
    server_config.lru_purge_enable = true;

    if (httpd_start(&s_server, &server_config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Unable to start the configuration web server");
        return false;
    }

    const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    const httpd_uri_t scan_uri = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler};
    const httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = save_post_handler};
    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &scan_uri);
    httpd_register_uri_handler(s_server, &save_uri);

    s_saved = false;
    s_active = true;
    ESP_LOGI(TAG, "Configuration hotspot \"%s\" is up (password \"%s\")", s_ap_ssid, PROVISION_AP_PASSWORD);
    ESP_LOGI(TAG, "Connect to it and open http://%s/ to set the WiFi credentials", s_ap_ip);

    return true;
}

void wifi_provision_poll(void)
{
    if (!s_shutdown_pending)
        return;

    s_shutdown_pending = false;

    if (s_server != NULL)
    {
        httpd_stop(s_server);
        s_server = NULL;
    }

    const esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Unable to switch the radio back to station mode: %s", esp_err_to_name(err));

    s_active = false;
    s_saved = true;
    s_saved_at_us = esp_timer_get_time();

    if (wifi_link_apply_credentials(s_saved_ssid, s_saved_password))
        ESP_LOGI(TAG, "Hotspot closed; reconnecting to \"%s\"", s_saved_ssid);
    else
        ESP_LOGE(TAG, "Unable to apply the new WiFi credentials");
}

wifi_provision_state_t wifi_provision_state(void)
{
    if (s_active)
        return WIFI_PROVISION_AP_ACTIVE;

    if (s_saved &&
        (wifi_link_has_ip() || (esp_timer_get_time() - s_saved_at_us) > PROVISION_SAVED_DISPLAY_US))
    {
        s_saved = false;
    }

    return s_saved ? WIFI_PROVISION_SAVED : WIFI_PROVISION_IDLE;
}

const char *wifi_provision_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *wifi_provision_ap_password(void)
{
    return PROVISION_AP_PASSWORD;
}

const char *wifi_provision_ap_ip(void)
{
    return s_ap_ip;
}

const char *wifi_provision_saved_ssid(void)
{
    return s_saved_ssid;
}
