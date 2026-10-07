// ESP32-S3 Stratum 1 NTP 时间服务器
// 同步状态机：跟踪 GNSS 有效性、PPS 活动、一致性校验结果与同步陈旧度。
// 通过 seqlock 发布无锁快照供 NTP 应答路径读取，避免热路径上加锁。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct
{
    bool pps_missing;
    bool gnss_invalid;
    bool sanity_mismatch;
    bool sync_stale;
} sync_faults_t;

typedef struct
{
    int64_t last_successful_sync_us;
    int64_t last_sync_attempt_us;
    int64_t last_pps_seen_us;
    // 上次成功同步的残差：本地时钟与 GNSS 秒边界的偏差（微秒，正值表示本地时钟超前）
    int64_t last_sync_delta_us;
    uint32_t consecutive_sync_failures;
    uint32_t consecutive_sanity_failures;
    bool holdover_mode;
    bool pps_active;
    bool gnss_timing_valid;
    int64_t last_gnss_valid_us;
    sync_faults_t faults;
} sync_state_t;

// 供 NTP 应答路径读取的已发布快照（无锁读取）
typedef struct
{
    int64_t last_successful_sync_us;
    int64_t last_gnss_valid_us;
    bool pps_active;
    bool gnss_timing_valid;
    sync_faults_t faults;
} sync_published_t;

void sync_state_init(void);

bool sync_state_has_any_fault(sync_faults_t faults);

void sync_state_note_attempt(void);
void sync_state_note_pps_edge(int64_t edge_us);
void sync_state_note_pps_timeout(int64_t now_us);
void sync_state_note_gnss_validity(bool valid);
uint32_t sync_state_note_failure(sync_faults_t faults, time_t update_delta);
uint32_t sync_state_note_sanity_retry(time_t update_delta);
void sync_state_clear_sanity_failures(void);
void sync_state_reset_failure_counters(void);
void sync_state_note_success(int64_t update_delta_us);

sync_state_t sync_state_snapshot(void);

// 无锁读取已发布快照；若每次尝试都发生 seqlock 竞争则返回 false（调用方应视为数据不可用）
bool sync_state_read_published(sync_published_t *out);

// GNSS 锁定状态统计（用于诊断）
bool sync_state_gnss_locked(void);
