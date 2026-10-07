// ESP32-S3 Stratum 1 NTP 时间服务器
// 系统时间状态：时钟是否已设置、是否正在同步、安全保护标志及已发布的 NTP 参考时间戳。

#pragma once

#include <stdbool.h>
#include <stdint.h>

void time_state_init(void);

// 保护系统时钟（settimeofday / adjtime）的互斥锁，超时单位为毫秒
bool time_state_lock(uint32_t timeout_ms);
void time_state_unlock(void);

bool time_state_has_been_set(void);
void time_state_mark_time_set(bool value);

bool time_state_setting_in_progress(void);
void time_state_mark_setting_in_progress(bool value);

bool time_state_safe_guard_tripped(void);
void time_state_mark_safe_guard(bool value);

uint64_t time_state_reference_64(void);
bool time_state_reference_valid(void);

// 发布 NTP 应答参考字段所用的参考时间戳
void time_state_publish_reference(uint64_t reference_64, bool valid);
