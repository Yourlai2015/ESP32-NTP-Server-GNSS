// ESP32-S3 Stratum 1 NTP 时间服务器
// 可控重启的实现：记录原因并（调试模式下短暂延时后）重启设备。

#include "restart.h"

#include "app_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "restart";

void controlled_restart(const char *reason)
{
    ESP_LOGW(TAG, "Controlled restart requested: %s", reason != NULL ? reason : "unknown");

#if DEBUG_ENABLED
    vTaskDelay(pdMS_TO_TICKS(200));
#endif

    esp_restart();
}
