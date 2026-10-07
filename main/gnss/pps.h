// ESP32-S3 NTP 时间服务器
// PPS（秒脉冲）硬件捕获与时钟驯服。
// 通过 MCPWM 捕获 PPS 引脚，用每个脉冲测量系统时钟的亚秒相位误差，再用 adjtime() 平滑修正，实现亚毫秒级精度。



#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

typedef struct
{
    int64_t approximate_edge_us;
} pps_capture_event_t;

// 在 PPS 引脚上配置 MCPWM 捕获通道
void pps_init(void);

// 丢弃所有待处理的 PPS 事件
void pps_clear_events(void);

// 非阻塞检查是否有 PPS 边沿（用于启动资格判定）
bool pps_take_edge(void);

// 正在接收 PPS 脉冲并用于驯服时钟时为 true
bool pps_discipline_active(void);

// 等待一个捕获到的 PPS 边沿（用于获取同步候选）
bool pps_wait_event(pps_capture_event_t *event, TickType_t timeout);

// FreeRTOS 任务入口：持续驯服系统时钟
void pps_discipline_task(void *parameter);
