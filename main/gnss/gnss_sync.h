// ESP32-S3 NTP 时间服务器
// GNSS 启动、周期时间同步与运行时恢复。
// 负责接收机上线、PPS 资格判定、周期校时，以及链路异常时的重连与恢复。
#pragma once

#include <stdbool.h>

// 启动接收机（检测波特率、等待有效定位）并执行启动阶段的 PPS 资格判定；
// 当无法连通（UART 静默）或始终不合格时返回 false，便于上层重试或重启。
bool gnss_setup(void);

// 创建 PPS 驯服任务与 GNSS 时间同步任务
void gnss_start_sync_tasks(void);

// 接收机当前上报有效日期时间时为 true
bool gnss_current_timing_is_valid(void);
