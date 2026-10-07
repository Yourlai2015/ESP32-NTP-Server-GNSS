// ESP32-S3 NTP 时间服务器
// GNSS 启动、周期同步与运行时恢复实现。
// 负责波特率检测、启动资格判定、周期校时，以及多次同步失败后的 GNSS 重连。
#include "gnss_sync.h"

#include "app_config.h"
#include "gnss_nvs.h"
#include "gnss_uart.h"
#include "ntp_time.h"
#include "pps.h"
#include "restart.h"
#include "sync_state.h"
#include "time_state.h"

#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gnss_sync";

typedef struct
{
    time_t candidate_time;
    bool use_pps_alignment;
    bool used_nmea_fallback;
    int64_t pps_release_time_us;
    sync_faults_t failures;
} sync_candidate_t;

static bool s_gnss_recovery_in_progress = false;
static int64_t s_last_gnss_recovery_us = 0;

bool gnss_current_timing_is_valid(void)
{
    nmea_time_t nmea_time;
    return gnss_uart_wait_for_time(&nmea_time, 1200);
}

// 未定位的接收机仍会输出 NMEA，UART 完全静默说明链路已断（接线、供电或模块故障）；
// 当尚未收到任何数据时使用 fallback_reference_us 作为参考时间。
static bool gnss_link_has_gone_silent(int64_t now_us, int64_t fallback_reference_us)
{
    int64_t last_rx_us = gnss_uart_last_rx_us();
    int64_t reference_us = (last_rx_us > 0) ? last_rx_us : fallback_reference_us;
    return (now_us - reference_us) > (int64_t)GNSS_RX_SILENCE_TIMEOUT_MS * 1000LL;
}

// ---------------------------------------------------------------------------
// 启动资格判定：需获得有效 GNSS 日期时间与稳定 PPS 序列后才允许对外授时。
// ---------------------------------------------------------------------------
static bool wait_for_gnss_startup_qualification(void)
{
    int64_t valid_started_us = 0;
    int64_t last_pps_us = 0;
    uint32_t stable_pps_edges = 0;
    const int64_t qualification_start_us = esp_timer_get_time();

    pps_clear_events();

#if DEBUG_ENABLED
    ESP_LOGI(TAG, "Qualifying GNSS and PPS stability before startup.");
#endif

    for (;;)
    {
        int64_t now_us = esp_timer_get_time();

        // 不永久阻塞启动：链路失效或无法产生稳定 PPS 的情况返回给上层重试。
        if (gnss_link_has_gone_silent(now_us, qualification_start_us))
            return false;

        if ((now_us - qualification_start_us) > (int64_t)GNSS_STARTUP_QUALIFICATION_TIMEOUT_MS * 1000LL)
        {
            ESP_LOGE(TAG, "GNSS/PPS did not qualify within %lu s - check the PPS connection.",
                     (unsigned long)(GNSS_STARTUP_QUALIFICATION_TIMEOUT_MS / 1000U));
            return false;
        }

        if (!gnss_current_timing_is_valid())
        {
            valid_started_us = 0;
            last_pps_us = 0;
            stable_pps_edges = 0;
            pps_clear_events();
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (valid_started_us == 0)
            valid_started_us = now_us;

        int64_t qualification_deadline_us = valid_started_us + (int64_t)GNSS_STARTUP_QUALIFICATION_DURATION_MS * 1000LL;
        if (now_us >= qualification_deadline_us)
        {
            if (stable_pps_edges >= GNSS_STARTUP_QUALIFICATION_PPS_EDGES)
            {
#if DEBUG_ENABLED
                ESP_LOGI(TAG, "GNSS and PPS startup qualification complete after %lu stable PPS edges.",
                         (unsigned long)stable_pps_edges);
#endif
                return true;
            }

            valid_started_us = 0;
            last_pps_us = 0;
            stable_pps_edges = 0;
            pps_clear_events();
            valid_started_us = esp_timer_get_time();
            continue;
        }

        if (pps_take_edge())
        {
            now_us = esp_timer_get_time();
            if (last_pps_us == 0 ||
                ((now_us - last_pps_us) >= GNSS_STARTUP_MIN_PPS_INTERVAL_US &&
                 (now_us - last_pps_us) <= GNSS_STARTUP_MAX_PPS_INTERVAL_US))
                stable_pps_edges++;
            else
                stable_pps_edges = 1;
            last_pps_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------------------
// 获取单个同步候选（日期时间 + PPS 对齐）
// ---------------------------------------------------------------------------
static bool acquire_sync_candidate(sync_candidate_t *candidate)
{
    if (candidate == NULL)
        return false;

    memset(candidate, 0, sizeof(*candidate));
    candidate->used_nmea_fallback = true;
    candidate->pps_release_time_us = esp_timer_get_time();
    int64_t attempt_start_us = candidate->pps_release_time_us;

    nmea_time_t nmea_time;
    if (!gnss_uart_wait_for_time(&nmea_time, 3000))
    {
        candidate->failures.gnss_invalid = true;
#if DEBUG_ENABLED
        ESP_LOGE(TAG, "GNSS invalid checkpoint 1.");
#endif
        return false;
    }

    candidate->candidate_time = ntp_epoch_from_utc(nmea_time.year, nmea_time.month, nmea_time.day,
                                                   nmea_time.hour, nmea_time.minute, nmea_time.second);

    pps_clear_events();
    pps_capture_event_t capture_event;
    if (pps_wait_event(&capture_event, pdMS_TO_TICKS(1500)))
    {
        candidate->use_pps_alignment = true;
        candidate->pps_release_time_us = capture_event.approximate_edge_us;
        candidate->candidate_time += 1;
    }
    else
    {
#if DEBUG_ENABLED
        ESP_LOGE(TAG, "GNSS time is running without PPS.");
#endif
        candidate->failures.pps_missing = true;
        return false;
    }

    if ((esp_timer_get_time() - attempt_start_us) > MAX_SYNC_ATTEMPT_US)
    {
        candidate->failures.gnss_invalid = true;
#if DEBUG_ENABLED
        ESP_LOGE(TAG, "GNSS invalid checkpoint 2.");
#endif
        return false;
    }

    return true;
}

static bool first_sync_candidates_are_plausible(const sync_candidate_t *first, const sync_candidate_t *second)
{
    time_t delta = second->candidate_time - first->candidate_time;
    return delta >= 1 && delta <= 3;
}

// ---------------------------------------------------------------------------
// 启动流程
// ---------------------------------------------------------------------------
bool gnss_setup(void)
{
    uint32_t saved_baud = 0;
    bool have_saved_baud = gnss_nvs_load_baud(&saved_baud);

    if (!have_saved_baud)
    {
#if DEBUG_ENABLED
        ESP_LOGI(TAG, "Performing a first-time GNSS baud rate scan.");
#endif
    }

    uint32_t detected_baud = 0;
    if (!gnss_uart_detect_baud(have_saved_baud ? saved_baud : 0, &detected_baud))
    {
        // 首次启动时接收机可能尚未就绪，重试一次。
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!gnss_uart_detect_baud(have_saved_baud ? saved_baud : 0, &detected_baud))
        {
            ESP_LOGE(TAG, "GNSS comms failed - check TX/RX + power");
            return false;
        }
    }

    if (!have_saved_baud || saved_baud != detected_baud)
        gnss_nvs_save_baud(detected_baud);

    ESP_LOGI(TAG, "GNSS initialised at %lu baud; waiting for time fix.", (unsigned long)detected_baud);

    // 等待接收机给出有效日期时间，长时间未定位时输出告警。
    const int64_t wait_start_us = esp_timer_get_time();
    int64_t last_heartbeat_us = 0;

    for (;;)
    {
        if (gnss_current_timing_is_valid())
        {
#if DEBUG_ENABLED
            ESP_LOGI(TAG, "GNSS fix obtained after %lu ms.",
                     (unsigned long)((esp_timer_get_time() - wait_start_us) / 1000LL));
#endif
            // 资格判定失败（链路失效或 PPS 不稳定）会返回给调用者，便于重试启动。
            return wait_for_gnss_startup_qualification();
        }

        int64_t now_us = esp_timer_get_time();

        if (gnss_link_has_gone_silent(now_us, wait_start_us))
        {
            ESP_LOGE(TAG, "No NMEA traffic for %lu s - GNSS link lost; check module, wiring and power.",
                     (unsigned long)(GNSS_RX_SILENCE_TIMEOUT_MS / 1000U));
            return false;
        }

        // 心跳：默认关闭调试日志，若无此提示将完全看不到等待定位的活动。
        if (last_heartbeat_us == 0 || (now_us - last_heartbeat_us) >= 60000000LL)
        {
            ESP_LOGW(TAG, "Waiting for a GNSS fix (%lu s). Check antenna placement, sky view and module compatibility.",
                     (unsigned long)((now_us - wait_start_us) / 1000000LL));
            last_heartbeat_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------------------------------------------------------------------------
// 运行时恢复
// ---------------------------------------------------------------------------
static void gnss_runtime_recovery_task(void *parameter)
{
    TaskHandle_t sync_task_handle = (TaskHandle_t)parameter;

    // gnss_setup() 总会返回，即使接收机完全无法连通，同步任务不会
    // 一直等待一个永远不会到来的通知。
    if (!gnss_setup())
        ESP_LOGW(TAG, "GNSS recovery did not complete; the sync task will keep retrying.");

    s_gnss_recovery_in_progress = false;
    xTaskNotifyGive(sync_task_handle);
    vTaskDelete(NULL);
}

static void handle_runtime_sync_failure(sync_faults_t faults, time_t update_delta, uint32_t retry_delay_ms)
{
    uint32_t failure_count = sync_state_note_failure(faults, update_delta);
    sync_state_t snapshot = sync_state_snapshot();

    time_state_mark_setting_in_progress(false);

    if (failure_count >= SYNC_FAILURES_BEFORE_RUNTIME_RECOVERY &&
        (failure_count % SYNC_FAILURES_BEFORE_RUNTIME_RECOVERY) == 0)
    {
        int64_t now_us = esp_timer_get_time();
        bool recovery_due = s_last_gnss_recovery_us == 0 ||
                            (now_us - s_last_gnss_recovery_us) >= RUNTIME_GNSS_RECOVERY_MIN_INTERVAL_US;

        if (recovery_due && !s_gnss_recovery_in_progress)
        {
            s_gnss_recovery_in_progress = true;
#if DEBUG_ENABLED
            ESP_LOGW(TAG, "Runtime GNSS recovery attempt after %lu consecutive sync failures.", (unsigned long)failure_count);
#endif
            s_last_gnss_recovery_us = now_us;
            TaskHandle_t sync_task_handle = xTaskGetCurrentTaskHandle();
            BaseType_t created = xTaskCreatePinnedToCore(gnss_runtime_recovery_task,
                                                         "gnss_recovery",
                                                         GNSS_RECOVERY_TASK_STACK_SIZE,
                                                         sync_task_handle,
                                                         15,
                                                         NULL,
                                                         tskNO_AFFINITY);
            if (created == pdPASS)
            {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                sync_state_reset_failure_counters();
            }
            else
            {
                s_gnss_recovery_in_progress = false;
#if DEBUG_ENABLED
                ESP_LOGE(TAG, "Unable to create the GNSS recovery task.");
#endif
            }
        }
    }

    if (snapshot.last_successful_sync_us > 0 &&
        (esp_timer_get_time() - snapshot.last_successful_sync_us) > SYNC_REBOOT_AFTER_US)
        controlled_restart("more_than_20_consecutive_time_sync_failures");

    vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
}

// ---------------------------------------------------------------------------
// 周期同步任务
// ---------------------------------------------------------------------------
static void gnss_time_sync_task(void *parameter)
{
    (void)parameter;
    bool first_sync = true;

    for (;;)
    {
        time_state_mark_setting_in_progress(true);
        sync_state_note_attempt();

        sync_candidate_t candidate;
        if (!acquire_sync_candidate(&candidate))
        {
            handle_runtime_sync_failure(candidate.failures, 0, 1000);
            continue;
        }

        if (first_sync)
        {
            sync_candidate_t confirmation_candidate;
            if (!acquire_sync_candidate(&confirmation_candidate))
            {
                handle_runtime_sync_failure(confirmation_candidate.failures, 0, 1000);
                continue;
            }

            if (!first_sync_candidates_are_plausible(&candidate, &confirmation_candidate))
            {
                sync_faults_t faults = {false, true, false, false};
                handle_runtime_sync_failure(faults, 0, 1000);
                continue;
            }

            candidate = confirmation_candidate;
        }

        time_t update_delta = 0;
        int64_t update_delta_us = 0;
        if (!first_sync)
        {
            // 以微秒分辨率采样本地时钟：显示的残差才是真实（经 PPS 驯服）的相位误差，而非取整到秒的值。
            struct timeval now_tv;
            gettimeofday(&now_tv, NULL);
            update_delta = now_tv.tv_sec - candidate.candidate_time;
            update_delta_us = ((int64_t)now_tv.tv_sec - (int64_t)candidate.candidate_time) * 1000000LL +
                              (int64_t)now_tv.tv_usec;
            bool sanity_check_passed = (update_delta >= -SAFEGUARD_THRESHOLD_SECONDS) &&
                                       (update_delta <= SAFEGUARD_THRESHOLD_SECONDS);
            if (!sanity_check_passed)
            {
                uint32_t sanity_failure_count = sync_state_note_sanity_retry(update_delta);
                time_state_mark_setting_in_progress(false);

#if DEBUG_ENABLED
                ESP_LOGE(TAG, "Sanity check failed with delta %lld on attempt %lu.",
                         (long long)update_delta, (unsigned long)sanity_failure_count);
#endif

                if (sanity_failure_count < SANITY_FAILURES_BEFORE_FAULT)
                {
                    vTaskDelay(pdMS_TO_TICKS(250));
                    continue;
                }

                time_state_mark_safe_guard(true);
                sync_faults_t faults = {false, false, true, false};
                sync_state_note_failure(faults, update_delta);

                if (REBOOT_IF_SANITY_CHECK_FAILS)
                    controlled_restart("time_sync_failed_sanity_check");

                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        sync_state_clear_sanity_failures();

        if (time_state_lock(portMAX_DELAY))
        {
            int64_t elapsed_us = candidate.use_pps_alignment ? (esp_timer_get_time() - candidate.pps_release_time_us) : 0;
            if (elapsed_us < 0)
                elapsed_us = 0;

            struct timeval tv;
            tv.tv_sec = candidate.candidate_time + (time_t)(elapsed_us / 1000000LL);
            tv.tv_usec = (suseconds_t)(elapsed_us % 1000000LL);
            settimeofday(&tv, NULL);
            ntp_refresh_reference();

            time_state_unlock();

            time_state_mark_safe_guard(false);
            time_state_mark_setting_in_progress(false);
            time_state_mark_time_set(true);
            first_sync = false;
            sync_state_note_success(update_delta_us);

#if DEBUG_ENABLED
            char date_string[16] = "";
            char time_string[24] = "";
            ntp_format_local_date_time(time(NULL), date_string, sizeof(date_string), time_string, sizeof(time_string));
            ESP_LOGI(TAG, "GNSS time sync on %s at %s", date_string, time_string);
#endif

            uint64_t refresh_start_us = esp_timer_get_time();
            const uint64_t refresh_interval_us = (uint64_t)PERIODIC_GNSS_REFRESH_MINUTES * 60ULL * 1000000ULL;
            int64_t invalid_started_us = 0;

            while ((esp_timer_get_time() - refresh_start_us) < (int64_t)refresh_interval_us)
            {
                bool gnss_valid = gnss_current_timing_is_valid();

                if (gnss_valid)
                {
                    invalid_started_us = 0;
                    sync_state_note_gnss_validity(true);
                }
                else
                {
                    if (invalid_started_us == 0)
                        invalid_started_us = esp_timer_get_time();

                    if ((esp_timer_get_time() - invalid_started_us) >= GNSS_INVALID_REACQUISITION_AFTER_US)
                    {
                        sync_state_note_gnss_validity(false);
#if DEBUG_ENABLED
                        ESP_LOGW(TAG, "GNSS timing invalid for more than %lu seconds; starting reacquisition.",
                                 (unsigned long)(GNSS_INVALID_REACQUISITION_AFTER_US / 1000000LL));
#endif
                        break;
                    }
                }

                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void gnss_start_sync_tasks(void)
{
    xTaskCreatePinnedToCore(gnss_time_sync_task, "gnss_time_sync", GNSS_TIME_SYNC_TASK_STACK_SIZE, NULL, 15, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(pps_discipline_task, "pps_discipline", PPS_DISCIPLINE_TASK_STACK_SIZE, NULL, 14, NULL, tskNO_AFFINITY);
}
