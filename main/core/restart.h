// ESP32-S3 Stratum 1 NTP 时间服务器
// 可控重启辅助函数（本项目已移除 MQTT）。

#pragma once

void controlled_restart(const char *reason);
