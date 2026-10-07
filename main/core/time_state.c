// ESP32-S3 Stratum 1 NTP 时间服务器
// 系统时间状态与 NTP 参考时间戳的原子变量读写实现。

#include "time_state.h"

#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static atomic_bool s_time_has_been_set;
static atomic_bool s_time_setting_in_progress;
static atomic_bool s_safe_guard_tripped;
static atomic_bool s_ntp_reference_valid;
static atomic_ullong s_ntp_reference_time_64;
static SemaphoreHandle_t s_time_mutex = NULL;

void time_state_init(void)
{
    atomic_store(&s_time_has_been_set, false);
    atomic_store(&s_time_setting_in_progress, false);
    atomic_store(&s_safe_guard_tripped, false);
    atomic_store(&s_ntp_reference_valid, false);
    atomic_store(&s_ntp_reference_time_64, 0);

    if (s_time_mutex == NULL)
        s_time_mutex = xSemaphoreCreateMutex();
}

bool time_state_lock(uint32_t timeout_ms)
{
    if (s_time_mutex == NULL)
        return false;
    return xSemaphoreTake(s_time_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void time_state_unlock(void)
{
    if (s_time_mutex != NULL)
        xSemaphoreGive(s_time_mutex);
}

bool time_state_has_been_set(void)
{
    return atomic_load(&s_time_has_been_set);
}

void time_state_mark_time_set(bool value)
{
    atomic_store(&s_time_has_been_set, value);
}

bool time_state_setting_in_progress(void)
{
    return atomic_load(&s_time_setting_in_progress);
}

void time_state_mark_setting_in_progress(bool value)
{
    atomic_store(&s_time_setting_in_progress, value);
}

bool time_state_safe_guard_tripped(void)
{
    return atomic_load(&s_safe_guard_tripped);
}

void time_state_mark_safe_guard(bool value)
{
    atomic_store(&s_safe_guard_tripped, value);
}

uint64_t time_state_reference_64(void)
{
    return atomic_load(&s_ntp_reference_time_64);
}

bool time_state_reference_valid(void)
{
    return atomic_load(&s_ntp_reference_valid);
}

void time_state_publish_reference(uint64_t reference_64, bool valid)
{
    atomic_store(&s_ntp_reference_time_64, reference_64);
    atomic_store(&s_ntp_reference_valid, valid);
}
