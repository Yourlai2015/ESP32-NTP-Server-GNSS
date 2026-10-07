// ESP32-S3 NTP 时间服务器
// PPS 捕获与时钟驯服实现。
// 由 MCPWM 捕获回调产生 PPS 事件，驯服任务据此计算相位误差并用 adjtime() 修正。

#include "pps.h"

#include "app_config.h"
#include "ntp_time.h"
#include "sync_state.h"
#include "time_state.h"

#include <stdlib.h>
#include <sys/time.h>

#include "driver/mcpwm_cap.h"
#include "driver/mcpwm_types.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "pps";

static SemaphoreHandle_t s_pps_semaphore = NULL;
static QueueHandle_t s_pps_timestamp_queue = NULL;
static QueueHandle_t s_pps_sync_timestamp_queue = NULL;
static mcpwm_cap_timer_handle_t s_pps_capture_timer = NULL;
static mcpwm_cap_channel_handle_t s_pps_capture_channel = NULL;

static volatile bool s_pps_discipline_active = false;

static bool IRAM_ATTR pps_capture_callback(mcpwm_cap_channel_handle_t channel,
                                           const mcpwm_capture_event_data_t *event_data,
                                           void *user_data)
{
    (void)channel;
    (void)event_data;
    (void)user_data;

    pps_capture_event_t capture_event;
    capture_event.approximate_edge_us = esp_timer_get_time();

    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_pps_semaphore != NULL)
        xSemaphoreGiveFromISR(s_pps_semaphore, &higher_priority_task_woken);
    if (s_pps_timestamp_queue != NULL)
        xQueueOverwriteFromISR(s_pps_timestamp_queue, &capture_event, &higher_priority_task_woken);
    if (s_pps_sync_timestamp_queue != NULL)
        xQueueOverwriteFromISR(s_pps_sync_timestamp_queue, &capture_event, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

void pps_init(void)
{
    if (s_pps_semaphore == NULL)
        s_pps_semaphore = xSemaphoreCreateBinary();
    if (s_pps_timestamp_queue == NULL)
        s_pps_timestamp_queue = xQueueCreate(1, sizeof(pps_capture_event_t));
    if (s_pps_sync_timestamp_queue == NULL)
        s_pps_sync_timestamp_queue = xQueueCreate(1, sizeof(pps_capture_event_t));

    mcpwm_capture_timer_config_t timer_config = {0};
    timer_config.group_id = 0;
    timer_config.clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT;
    timer_config.resolution_hz = PPS_CAPTURE_RESOLUTION_HZ;
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&timer_config, &s_pps_capture_timer));

    mcpwm_capture_channel_config_t channel_config = {0};
    channel_config.gpio_num = GNSS_PPS_PIN;
    channel_config.prescale = 1;
    channel_config.flags.pos_edge = true;
    channel_config.flags.neg_edge = false;
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(s_pps_capture_timer, &channel_config, &s_pps_capture_channel));

    mcpwm_capture_event_callbacks_t callbacks = {0};
    callbacks.on_cap = pps_capture_callback;
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(s_pps_capture_channel, &callbacks, NULL));

    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(s_pps_capture_channel));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(s_pps_capture_timer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(s_pps_capture_timer));
}

void pps_clear_events(void)
{
    if (s_pps_semaphore != NULL)
    {
        while (xSemaphoreTake(s_pps_semaphore, 0) == pdTRUE)
        {
        }
    }

    if (s_pps_sync_timestamp_queue != NULL)
    {
        pps_capture_event_t event;
        while (xQueueReceive(s_pps_sync_timestamp_queue, &event, 0) == pdTRUE)
        {
        }
    }
}

bool pps_take_edge(void)
{
    if (s_pps_semaphore == NULL)
        return false;
    return xSemaphoreTake(s_pps_semaphore, 0) == pdTRUE;
}

bool pps_wait_event(pps_capture_event_t *event, TickType_t timeout)
{
    if (event == NULL || s_pps_sync_timestamp_queue == NULL)
        return false;
    return xQueueReceive(s_pps_sync_timestamp_queue, event, timeout) == pdTRUE;
}

bool pps_discipline_active(void)
{
    return s_pps_discipline_active;
}

void pps_discipline_task(void *parameter)
{
    (void)parameter;

    const int64_t max_phase_error_us_to_correct = 250000;
    const int64_t min_correction_magnitude_us = 2;

    int64_t last_pps_us = esp_timer_get_time();
    bool logged_active = false;

    for (;;)
    {
        pps_capture_event_t capture_event;
        if (xQueueReceive(s_pps_timestamp_queue, &capture_event, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            int64_t edge_us = capture_event.approximate_edge_us;
            last_pps_us = edge_us;
            s_pps_discipline_active = true;
            sync_state_note_pps_edge(edge_us);

            if (!logged_active)
            {
#if DEBUG_ENABLED
                ESP_LOGI(TAG, "PPS discipline active.");
#endif
                logged_active = true;
            }

            if (!time_state_has_been_set() || time_state_setting_in_progress())
                continue;

            if (!time_state_lock(20))
                continue;

            int64_t processing_now_us = esp_timer_get_time();
            struct timeval now;
            gettimeofday(&now, NULL);

            int64_t current_time_us = (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_usec;
            int64_t wake_delay_us = processing_now_us - edge_us;
            if (wake_delay_us < 0)
                wake_delay_us = 0;
            int64_t edge_time_us = current_time_us - wake_delay_us;

            int64_t phase_error_us = edge_time_us % 1000000LL;
            if (phase_error_us > 500000)
                phase_error_us -= 1000000;
            else if (phase_error_us < -500000)
                phase_error_us += 1000000;

            int64_t correction_us = -phase_error_us;
            if (llabs(correction_us) >= min_correction_magnitude_us && llabs(phase_error_us) <= max_phase_error_us_to_correct)
            {
                struct timeval delta;
                delta.tv_sec = (time_t)(correction_us / 1000000LL);
                delta.tv_usec = (suseconds_t)(correction_us % 1000000LL);
                if (delta.tv_usec < 0)
                {
                    delta.tv_usec += 1000000;
                    delta.tv_sec -= 1;
                }

                if (adjtime(&delta, NULL) == 0)
                    ntp_refresh_reference();
            }

            time_state_unlock();
        }
        else
        {
            int64_t now_us = esp_timer_get_time();
            if ((now_us - last_pps_us) > PPS_TIMEOUT_US)
            {
                if (logged_active && DEBUG_ENABLED)
                    ESP_LOGW(TAG, "PPS discipline inactive (PPS signal unavailable).");
                logged_active = false;
                s_pps_discipline_active = false;
                sync_state_note_pps_timeout(now_us);
            }
        }
    }
}
