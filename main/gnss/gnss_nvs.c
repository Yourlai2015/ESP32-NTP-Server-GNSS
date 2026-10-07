// ESP32-S3 NTP 时间服务器
// GNSS 启动数据（波特率）在 NVS 中的读写实现。
// 使用独立的 NVS 命名空间保存、读取与清除波特率，供启动时快速配置。

#include "gnss_nvs.h"

#include "app_config.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "gnss_nvs";

static const char *NVS_KEY_BAUD = "baud";

bool gnss_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition requires re-initialisation; erasing and retrying.");
        if (nvs_flash_erase() != ESP_OK || nvs_flash_init() != ESP_OK)
        {
            ESP_LOGE(TAG, "NVS initialisation failed.");
            return false;
        }
        return true;
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

bool gnss_nvs_load_baud(uint32_t *baud)
{
    if (baud == NULL)
        return false;

    nvs_handle_t handle = 0;
    if (nvs_open(GNSS_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false;

    uint32_t stored_baud = 0;
    esp_err_t err = nvs_get_u32(handle, NVS_KEY_BAUD, &stored_baud);
    nvs_close(handle);

    if (err != ESP_OK || stored_baud == 0)
        return false;

    *baud = stored_baud;
    return true;
}

bool gnss_nvs_save_baud(uint32_t baud)
{
    if (baud == 0)
        return false;

    nvs_handle_t handle = 0;
    if (nvs_open(GNSS_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;

    esp_err_t err = nvs_set_u32(handle, NVS_KEY_BAUD, baud);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);

    return err == ESP_OK;
}

void gnss_nvs_clear(void)
{
    nvs_handle_t handle = 0;
    if (nvs_open(GNSS_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return;

    esp_err_t err = nvs_erase_key(handle, NVS_KEY_BAUD);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND)
        nvs_commit(handle);
    nvs_close(handle);
}
