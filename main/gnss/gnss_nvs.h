// ESP32-S3 NTP 时间服务器
// GNSS 启动数据的非易失存储。
//本项目仅保存检测到的波特率，使后续启动可直接复用、跳过扫描。

#pragma once

#include <stdbool.h>
#include <stdint.h>

// 初始化 NVS Flash 分区
bool gnss_nvs_init(void);

// 读取上次保存的可用波特率（未保存时返回 false）
bool gnss_nvs_load_baud(uint32_t *baud);

// 保存可用的波特率
bool gnss_nvs_save_baud(uint32_t baud);

// 清除已保存的 GNSS 数据
void gnss_nvs_clear(void);
