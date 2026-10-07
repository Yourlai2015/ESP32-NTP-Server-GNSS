// ESP32-S3 Stratum 1 NTP 时间服务器

#include "app_config.h"
#include "gnss_nvs.h"
#include "gnss_sync.h"
#include "gnss_uart.h"
#include "ntp_auth.h"
#include "ntp_health.h"
#include "ntp_server.h"
#include "ntp_time.h"
#include "oled.h"
#include "pps.h"
#include "restart.h"
#include "sync_state.h"
#include "time_state.h"
#include "wifi_link.h"
#include "wifi_provision.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_main";

// OLED 刷新周期与翻页按键轮询
#define OLED_POLL_INTERVAL_MS 20U
#define OLED_REFRESH_INTERVAL_MS 1000U
#define OLED_PAGE_COUNT 2

// 按固定 16 字符宽度写行，避免短文本残留旧字符。
static void oled_show_line(u8 y, const char *text)
{
    char padded[17];
    size_t length = strlen(text);
    if (length > 16)
        length = 16;
    memcpy(padded, text, length);
    memset(padded + length, ' ', 16 - length);
    padded[16] = '\0';
    OLED_ShowString(0, y, padded, 16);
}

// 配置翻页按键（按下为低电平）。
static void button_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

typedef enum
{
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_SHORT_PRESS,
    BUTTON_EVENT_LONG_PRESS,
} button_event_t;

// 按键消抖：松开时报告短按，按住达 BUTTON_LONG_PRESS_MS 后报告长按。
static button_event_t button_poll(void)
{
    static bool s_last_level = true;   // true = 松开
    static bool s_stable_level = true;
    static bool s_long_press_reported = false;
    static int64_t s_last_change_us = 0;
    static int64_t s_pressed_since_us = 0;

    const bool level = gpio_get_level(BUTTON_PIN) != 0; // true = 松开
    const int64_t now_us = esp_timer_get_time();

    if (level != s_last_level)
    {
        // 电平变化，重新开始消抖计时。
        s_last_level = level;
        s_last_change_us = now_us;
        return BUTTON_EVENT_NONE;
    }

    if ((now_us - s_last_change_us) < (int64_t)BUTTON_DEBOUNCE_MS * 1000LL)
        return BUTTON_EVENT_NONE;

    if (level != s_stable_level)
    {
        s_stable_level = level;

        if (!level)
        {
            s_pressed_since_us = now_us;
            s_long_press_reported = false;
        }
        else if (!s_long_press_reported &&
                 (now_us - s_pressed_since_us) >= (int64_t)BUTTON_SHORT_PRESS_MIN_MS * 1000LL)
        {
            return BUTTON_EVENT_SHORT_PRESS;
        }

        return BUTTON_EVENT_NONE;
    }

    if (!level && !s_long_press_reported &&
        (now_us - s_pressed_since_us) >= (int64_t)BUTTON_LONG_PRESS_MS * 1000LL)
    {
        s_long_press_reported = true;
        return BUTTON_EVENT_LONG_PRESS;
    }

    return BUTTON_EVENT_NONE;
}

// 第 1 页：卫星数/定位状态、当前时间与上次同步残差。
static void oled_render_status_page(void)
{
    char line[24];

    gnss_fix_info_t fix = {0};
    bool have_fix_info = gnss_get_fix_info(&fix);
    if (!have_fix_info)
        snprintf(line, sizeof(line), "SAT:-- no data");
    else if (fix.fix_quality > 0)
        snprintf(line, sizeof(line), "SAT:%02d FIX", fix.satellites);
    else
        snprintf(line, sizeof(line), "SAT:%02d NO FIX", fix.satellites);
    oled_show_line(0, line);

    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);

    strftime(line, sizeof(line), "%Y-%m-%d", &local_time);
    oled_show_line(2, line);

    strftime(line, sizeof(line), "%H:%M:%S", &local_time);
    oled_show_line(4, line);

    sync_state_t sync = sync_state_snapshot();
    if (sync.last_successful_sync_us == 0)
        snprintf(line, sizeof(line), "DELTA:--");
    else if (sync.last_sync_delta_us > -1000000LL && sync.last_sync_delta_us < 1000000LL)
        snprintf(line, sizeof(line), "DELTA:%+lldms", (long long)(sync.last_sync_delta_us / 1000LL));
    else
        snprintf(line, sizeof(line), "DELTA:%+.3fs", (double)sync.last_sync_delta_us / 1000000.0);
    oled_show_line(6, line);
}

// 第 2 页：GNSS 经纬度、IPv4 地址与 PPS 驯服状态。
static void oled_render_position_page(void)
{
    char line[24];

    gnss_fix_info_t fix = {0};
    bool have_fix_info = gnss_get_fix_info(&fix);

    if (have_fix_info && fix.position_valid)
    {
        snprintf(line, sizeof(line), "LAT:%.6f%c", fix.latitude, fix.latitude_hemisphere);
        oled_show_line(0, line);
        snprintf(line, sizeof(line), "LON:%.6f%c", fix.longitude, fix.longitude_hemisphere);
        oled_show_line(2, line);
    }
    else
    {
        oled_show_line(0, "LAT:--");
        oled_show_line(2, "LON:--");
    }

    const char *ipv4 = wifi_link_ipv4();
    snprintf(line, sizeof(line), "IP:%s", ipv4[0] != '\0' ? ipv4 : "waiting");
    oled_show_line(4, line);

    sync_state_t sync = sync_state_snapshot();
    snprintf(line, sizeof(line), "PPS:%s", sync.pps_active ? "ACTIVE" : "LOST");
    oled_show_line(6, line);
}

// 配网热点运行期间显示的页面。
static void oled_render_provision_page(void)
{
    char line[24];

    oled_show_line(0, "CONFIG MODE");

    snprintf(line, sizeof(line), "AP:%s", wifi_provision_ap_ssid());
    oled_show_line(2, line);

    snprintf(line, sizeof(line), "PW:%s", wifi_provision_ap_password());
    oled_show_line(4, line);

    snprintf(line, sizeof(line), "IP:%s", wifi_provision_ap_ip());
    oled_show_line(6, line);
}

// 凭据保存后、站点用新配置重连期间显示的页面。
static void oled_render_saved_page(void)
{
    char line[24];

    oled_show_line(0, "WIFI SAVED");

    snprintf(line, sizeof(line), "SSID:%s", wifi_provision_saved_ssid());
    oled_show_line(2, line);

    oled_show_line(4, wifi_link_has_ip() ? "IP acquired" : "connecting...");
    oled_show_line(6, "");
}

// 每秒刷新 OLED；短按翻页，长按进入 WiFi 配网热点。
static void oled_status_task(void *parameter)
{
    (void)parameter;

    uint32_t page = 0;
    uint32_t elapsed_ms = OLED_REFRESH_INTERVAL_MS; // 启动时立即绘制

    for (;;)
    {
        // 完成挂起的"配置已保存"流程（关闭热点并重连站点）。
        wifi_provision_poll();

        const button_event_t event = button_poll();

        if (event == BUTTON_EVENT_SHORT_PRESS)
        {
            page = (page + 1U) % OLED_PAGE_COUNT;
            elapsed_ms = OLED_REFRESH_INTERVAL_MS; // 立即重绘
        }
        else if (event == BUTTON_EVENT_LONG_PRESS)
        {
            if (wifi_provision_start())
                ESP_LOGI(TAG, "WiFi configuration mode enabled");
            else
                ESP_LOGE(TAG, "Unable to enable WiFi configuration mode");
            elapsed_ms = OLED_REFRESH_INTERVAL_MS;
        }

        if (elapsed_ms >= OLED_REFRESH_INTERVAL_MS)
        {
            switch (wifi_provision_state())
            {
            case WIFI_PROVISION_AP_ACTIVE:
                oled_render_provision_page();
                break;

            case WIFI_PROVISION_SAVED:
                oled_render_saved_page();
                break;

            default:
                if (page == 0)
                    oled_render_status_page();
                else
                    oled_render_position_page();
                break;
            }

            elapsed_ms = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(OLED_POLL_INTERVAL_MS));
        elapsed_ms += OLED_POLL_INTERVAL_MS;
    }
}

static void write_opening_messages_to_the_console(void)
{
    ESP_LOGI(TAG, " ");
    ESP_LOGI(TAG, "ESP32-S3 Stratum 1 NTP Time Server");
    ESP_LOGI(TAG, "GNSS disciplined, PPS stabilised (C port of the original project)");
    ESP_LOGI(TAG, "Time zone: %s", TIME_ZONE_SPEC);
    ESP_LOGI(TAG, " ");
}

static void write_open_for_business_messages_to_the_console(void)
{
    // 对外开放 NTP 响应需满足：已获取至少一个 IP、NTP 服务已就绪、
    // 时间已同步且 PPS 正在驯服时钟（每次请求由应答状态/质量检查保证）。
    if (wifi_link_ipv4()[0] != '\0')
        ESP_LOGI(TAG, "IPv4 link is up (%s)", wifi_link_ipv4());
    else
        ESP_LOGW(TAG, "IPv4 link is down");

    if (wifi_link_ipv6()[0] != '\0')
        ESP_LOGI(TAG, "IPv6 link is up (%s)", wifi_link_ipv6());
    else
        ESP_LOGW(TAG, "IPv6 link is down");

    if (ntp_auth_available())
    {
        char authentication_key_hex[NTP_AUTH_MAX_KEY_SIZE * 2 + 1] = "";
        uint32_t authentication_key_id = 0;
        ESP_LOGI(TAG, "Authorization key(s) for use with Meinberg (client ntp.keys):");
        for (size_t index = 0; index < ntp_auth_key_count(); ++index)
        {
            if (ntp_auth_key_hex(index, &authentication_key_id, authentication_key_hex, sizeof(authentication_key_hex)))
                ESP_LOGI(TAG, "%lu SHA256 %s", (unsigned long)authentication_key_id, authentication_key_hex);
        }
        ESP_LOGI(TAG, "Authorization key(s) for use with Chrony (client chrony.keys):");
        for (size_t index = 0; index < ntp_auth_key_count(); ++index)
        {
            if (ntp_auth_key_hex(index, &authentication_key_id, authentication_key_hex, sizeof(authentication_key_hex)))
                ESP_LOGI(TAG, "%lu SHA256 HEX:%s", (unsigned long)authentication_key_id, authentication_key_hex);
        }
    }

    ESP_LOGI(TAG, " ");
    ESP_LOGI(TAG, "NTP server is now open for business");
    ESP_LOGI(TAG, " ");
}

void app_main(void)
{
    // PPS 捕获须在流程最开始初始化
    pps_init();

    write_opening_messages_to_the_console();

    // OLED 状态显示（由 oled_status_task 刷新）与翻页按键
    OLED_Init();
    OLED_Clear();
    button_init();
    xTaskCreate(oled_status_task, "oled_status", 4096, NULL, 3, NULL);

    // 非易失存储（GNSS 波特率；同时初始化 NVS 分区）
    gnss_nvs_init();

    time_state_init();
    sync_state_init();

    // 以 WiFi station 取代原项目的以太网连接
    wifi_link_init();
    wifi_link_wait_for_ip();

    // 时区必须在任何本地时间格式化之前设置
    ntp_apply_timezone();

    // GNSS 初始化：探测接收机波特率并等待有效定位与合格 PPS 序列。
    // 无法连通的接收机会重试；始终无法就绪则重启，避免卡死在静默串口上。
    uint32_t gnss_attempts = 0;
    while (!gnss_setup())
    {
        gnss_attempts++;
        if (gnss_attempts >= GNSS_SETUP_MAX_ATTEMPTS)
        {
            ESP_LOGE(TAG, "GNSS unavailable after %u attempt(s) - restarting.", (unsigned)gnss_attempts);
            controlled_restart("gnss_unavailable_at_startup");
        }

        ESP_LOGW(TAG, "GNSS bring-up failed (attempt %u/%u); retrying in %u s.",
                 (unsigned)gnss_attempts, (unsigned)GNSS_SETUP_MAX_ATTEMPTS,
                 (unsigned)(GNSS_SETUP_RETRY_DELAY_MS / 1000U));
        vTaskDelay(pdMS_TO_TICKS(GNSS_SETUP_RETRY_DELAY_MS));
    }

    gnss_start_sync_tasks();

    // 等待首次时间同步成功后再对外提供时间
    while (!time_state_has_been_set())
        vTaskDelay(pdMS_TO_TICKS(100));

    // 对称密钥认证（仅在 app_config.h 启用时生效）
    if (ntp_auth_initialize())
        ESP_LOGI(TAG, "NTP symmetric-key authentication is available");
#if SYMMETRIC_KEY_AUTHENTICATION_ENABLED
    else
        ESP_LOGW(TAG, "NTP symmetric-key authentication is unavailable");
#endif

    // 启动 NTP 服务（IPv4、IPv6 及 IPv6 链路本地）
    ntp_server_start();
    if (!ntp_server_is_ready())
        ESP_LOGE(TAG, "NTP server did not become ready");

    // 可选的启动自检（未在 app_config.h 启用时为空操作）
    ntp_health_run();

    // 对外开放 NTP 响应
    ntp_server_set_external_responses(true);

    write_open_for_business_messages_to_the_console();
}
