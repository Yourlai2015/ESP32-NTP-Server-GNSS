// ESP32-S3 Stratum 1 NTP 时间服务器
// 同步状态机的实现：维护同步/故障状态，并以 seqlock 方式发布无锁快照。

#include "sync_state.h"

#include "app_config.h"
#include "time_state.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sync_state";

static sync_state_t s_sync_state;
static SemaphoreHandle_t s_sync_state_mutex = NULL;

// 供 NTP 应答路径读取的无锁副本
static atomic_uint s_ntp_sync_state_sequence;
static atomic_llong s_ntp_last_successful_sync_us;
static atomic_llong s_ntp_last_gnss_valid_us;
static atomic_bool s_ntp_pps_active;
static atomic_bool s_ntp_gnss_timing_valid;
static atomic_bool s_ntp_pps_missing;
static atomic_bool s_ntp_gnss_invalid;
static atomic_bool s_ntp_sanity_mismatch;
static atomic_bool s_ntp_sync_stale;

// GNSS 锁定状态统计
static atomic_bool s_gnss_locked;
static atomic_llong s_gnss_lock_started_us;

bool sync_state_has_any_fault(sync_faults_t faults)
{
    return faults.pps_missing || faults.gnss_invalid || faults.sanity_mismatch || faults.sync_stale;
}

static void refresh_sync_state_locked(sync_state_t *state, int64_t now_us)
{
    if (state == NULL)
        return;

    bool sync_stale = state->last_successful_sync_us > 0 && (now_us - state->last_successful_sync_us) > SYNC_STALE_AFTER_US;
    bool pps_missing = time_state_has_been_set() && !state->pps_active;
    bool gnss_missing = time_state_has_been_set() && (!state->gnss_timing_valid ||
                                                     state->last_gnss_valid_us == 0 ||
                                                     (now_us - state->last_gnss_valid_us) > GNSS_VALIDITY_TIMEOUT_US);

    state->faults.sync_stale = sync_stale;
    state->faults.gnss_invalid = gnss_missing;
    state->faults.pps_missing = pps_missing;

#if DEBUG_ENABLED
    if (gnss_missing)
        ESP_LOGE(TAG, "GNSS invalid - GNSS missing.");
#endif

    state->holdover_mode = sync_state_has_any_fault(state->faults);
}

static void publish_ntp_sync_state_locked(const sync_state_t *state)
{
    atomic_fetch_add_explicit(&s_ntp_sync_state_sequence, 1, memory_order_release);
    atomic_store_explicit(&s_ntp_last_successful_sync_us, state->last_successful_sync_us, memory_order_relaxed);
    atomic_store_explicit(&s_ntp_last_gnss_valid_us, state->last_gnss_valid_us, memory_order_relaxed);
    atomic_store_explicit(&s_ntp_pps_active, state->pps_active, memory_order_relaxed);
    atomic_store_explicit(&s_ntp_gnss_timing_valid, state->gnss_timing_valid, memory_order_relaxed);
    atomic_store_explicit(&s_ntp_pps_missing, state->faults.pps_missing, memory_order_relaxed);
    atomic_store_explicit(&s_ntp_gnss_invalid, state->faults.gnss_invalid, memory_order_relaxed);
    atomic_store_explicit(&s_ntp_sanity_mismatch, state->faults.sanity_mismatch, memory_order_relaxed);
    atomic_store_explicit(&s_ntp_sync_stale, state->faults.sync_stale, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_ntp_sync_state_sequence, 1, memory_order_release);
}

void sync_state_init(void)
{
    memset(&s_sync_state, 0, sizeof(s_sync_state));
    s_sync_state_mutex = xSemaphoreCreateMutex();

    atomic_store(&s_ntp_sync_state_sequence, 0);
    atomic_store(&s_ntp_last_successful_sync_us, 0);
    atomic_store(&s_ntp_last_gnss_valid_us, 0);
    atomic_store(&s_ntp_pps_active, false);
    atomic_store(&s_ntp_gnss_timing_valid, false);
    atomic_store(&s_ntp_pps_missing, false);
    atomic_store(&s_ntp_gnss_invalid, false);
    atomic_store(&s_ntp_sanity_mismatch, false);
    atomic_store(&s_ntp_sync_stale, false);

    atomic_store(&s_gnss_locked, false);
    atomic_store(&s_gnss_lock_started_us, 0);
}

static void set_gnss_lock_state(bool valid)
{
    int64_t now_us = esp_timer_get_time();
    bool previous = atomic_exchange(&s_gnss_locked, valid);
    if (previous == valid)
        return;

    if (valid)
        atomic_store(&s_gnss_lock_started_us, now_us);
    else
        atomic_store(&s_gnss_lock_started_us, 0);
}

bool sync_state_gnss_locked(void)
{
    return atomic_load(&s_gnss_locked);
}

void sync_state_note_attempt(void)
{
    if (xSemaphoreTake(s_sync_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_sync_state.last_sync_attempt_us = esp_timer_get_time();
        refresh_sync_state_locked(&s_sync_state, s_sync_state.last_sync_attempt_us);
        publish_ntp_sync_state_locked(&s_sync_state);
        xSemaphoreGive(s_sync_state_mutex);
    }
}

void sync_state_note_pps_edge(int64_t edge_us)
{
    if (xSemaphoreTake(s_sync_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_sync_state.last_pps_seen_us = edge_us;
        s_sync_state.pps_active = true;
        refresh_sync_state_locked(&s_sync_state, edge_us);
        publish_ntp_sync_state_locked(&s_sync_state);
        xSemaphoreGive(s_sync_state_mutex);
    }
}

void sync_state_note_pps_timeout(int64_t now_us)
{
    if (xSemaphoreTake(s_sync_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_sync_state.pps_active = false;
        refresh_sync_state_locked(&s_sync_state, now_us);
        publish_ntp_sync_state_locked(&s_sync_state);
        xSemaphoreGive(s_sync_state_mutex);
    }
}

void sync_state_note_gnss_validity(bool valid)
{
    set_gnss_lock_state(valid);

    if (xSemaphoreTake(s_sync_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        int64_t now_us = esp_timer_get_time();
        s_sync_state.gnss_timing_valid = valid;
        if (valid)
            s_sync_state.last_gnss_valid_us = now_us;
        refresh_sync_state_locked(&s_sync_state, now_us);
        publish_ntp_sync_state_locked(&s_sync_state);
        xSemaphoreGive(s_sync_state_mutex);
    }
}

uint32_t sync_state_note_failure(sync_faults_t faults, time_t update_delta)
{
    uint32_t failure_count = 0;

    if (faults.gnss_invalid)
        set_gnss_lock_state(false);

    if (xSemaphoreTake(s_sync_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_sync_state.last_sync_delta_us = (int64_t)update_delta * 1000000LL;
        s_sync_state.consecutive_sync_failures++;
        s_sync_state.faults.pps_missing = s_sync_state.faults.pps_missing || faults.pps_missing;
        s_sync_state.faults.gnss_invalid = s_sync_state.faults.gnss_invalid || faults.gnss_invalid;
        s_sync_state.faults.sanity_mismatch = s_sync_state.faults.sanity_mismatch || faults.sanity_mismatch;
        s_sync_state.faults.sync_stale = s_sync_state.faults.sync_stale || faults.sync_stale;
        refresh_sync_state_locked(&s_sync_state, esp_timer_get_time());
        publish_ntp_sync_state_locked(&s_sync_state);
        failure_count = s_sync_state.consecutive_sync_failures;
        xSemaphoreGive(s_sync_state_mutex);
    }

    return failure_count;
}

uint32_t sync_state_note_sanity_retry(time_t update_delta)
{
    uint32_t failure_count = 0;

    if (xSemaphoreTake(s_sync_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_sync_state.last_sync_delta_us = (int64_t)update_delta * 1000000LL;
        s_sync_state.consecutive_sync_failures++;
        s_sync_state.consecutive_sanity_failures++;
        refresh_sync_state_locked(&s_sync_state, esp_timer_get_time());
        publish_ntp_sync_state_locked(&s_sync_state);
        failure_count = s_sync_state.consecutive_sanity_failures;
        xSemaphoreGive(s_sync_state_mutex);
    }

    return failure_count;
}

void sync_state_clear_sanity_failures(void)
{
    if (xSemaphoreTake(s_sync_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_sync_state.consecutive_sanity_failures = 0;
        refresh_sync_state_locked(&s_sync_state, esp_timer_get_time());
        publish_ntp_sync_state_locked(&s_sync_state);
        xSemaphoreGive(s_sync_state_mutex);
    }
}

void sync_state_reset_failure_counters(void)
{
    if (xSemaphoreTake(s_sync_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_sync_state.consecutive_sync_failures = 0;
        s_sync_state.consecutive_sanity_failures = 0;
        refresh_sync_state_locked(&s_sync_state, esp_timer_get_time());
        publish_ntp_sync_state_locked(&s_sync_state);
        xSemaphoreGive(s_sync_state_mutex);
    }
}

void sync_state_note_success(int64_t update_delta_us)
{
    set_gnss_lock_state(true);

    if (xSemaphoreTake(s_sync_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_sync_state.last_successful_sync_us = esp_timer_get_time();
        s_sync_state.gnss_timing_valid = true;
        s_sync_state.last_gnss_valid_us = s_sync_state.last_successful_sync_us;
        s_sync_state.last_sync_delta_us = update_delta_us;
        s_sync_state.consecutive_sync_failures = 0;
        s_sync_state.consecutive_sanity_failures = 0;
        memset(&s_sync_state.faults, 0, sizeof(s_sync_state.faults));
        refresh_sync_state_locked(&s_sync_state, s_sync_state.last_successful_sync_us);
        publish_ntp_sync_state_locked(&s_sync_state);
        xSemaphoreGive(s_sync_state_mutex);
    }
}

sync_state_t sync_state_snapshot(void)
{
    sync_state_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    if (xSemaphoreTake(s_sync_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        refresh_sync_state_locked(&s_sync_state, esp_timer_get_time());
        publish_ntp_sync_state_locked(&s_sync_state);
        snapshot = s_sync_state;
        xSemaphoreGive(s_sync_state_mutex);
    }

    return snapshot;
}

bool sync_state_read_published(sync_published_t *out)
{
    if (out == NULL)
        return false;

    for (size_t attempt = 0; attempt < 3; ++attempt)
    {
        unsigned int sequence_before = atomic_load_explicit(&s_ntp_sync_state_sequence, memory_order_acquire);
        if ((sequence_before & 1U) != 0)
            continue;

        int64_t last_successful_sync_us = atomic_load_explicit(&s_ntp_last_successful_sync_us, memory_order_relaxed);
        int64_t last_gnss_valid_us = atomic_load_explicit(&s_ntp_last_gnss_valid_us, memory_order_relaxed);
        bool pps_active = atomic_load_explicit(&s_ntp_pps_active, memory_order_relaxed);
        bool gnss_timing_valid = atomic_load_explicit(&s_ntp_gnss_timing_valid, memory_order_relaxed);
        sync_faults_t faults;
        faults.pps_missing = atomic_load_explicit(&s_ntp_pps_missing, memory_order_relaxed);
        faults.gnss_invalid = atomic_load_explicit(&s_ntp_gnss_invalid, memory_order_relaxed);
        faults.sanity_mismatch = atomic_load_explicit(&s_ntp_sanity_mismatch, memory_order_relaxed);
        faults.sync_stale = atomic_load_explicit(&s_ntp_sync_stale, memory_order_relaxed);

        if (sequence_before != atomic_load_explicit(&s_ntp_sync_state_sequence, memory_order_acquire))
            continue;

        out->last_successful_sync_us = last_successful_sync_us;
        out->last_gnss_valid_us = last_gnss_valid_us;
        out->pps_active = pps_active;
        out->gnss_timing_valid = gnss_timing_valid;
        out->faults = faults;
        return true;
    }

    return false;
}
