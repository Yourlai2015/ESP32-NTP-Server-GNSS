// ESP32-S3 NTP 时间服务器
// WiFi Station 凭据的 NVS 读写实现：以 "ssid"/"password" 键存入 app_config.h 定义的命名空间，供启动加载与配置页面保存使用。

#include "wifi_config.h"

#include "app_config.h"

#include "nvs.h"

static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASSWORD = "password";

bool wifi_config_load(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    if (ssid == NULL || ssid_size == 0 || password == NULL || password_size == 0)
        return false;

    ssid[0] = '\0';
    password[0] = '\0';

    nvs_handle_t handle = 0;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false;

    size_t ssid_length = ssid_size;
    esp_err_t err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_length);
    if (err == ESP_OK)
    {
        size_t password_length = password_size;
        err = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &password_length);
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            // 开放网络：该键不存在，而非内容为空。
            password[0] = '\0';
            err = ESP_OK;
        }
    }

    nvs_close(handle);

    return err == ESP_OK && ssid[0] != '\0';
}

bool wifi_config_save(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0')
        return false;

    nvs_handle_t handle = 0;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;

    esp_err_t err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK)
        err = nvs_set_str(handle, NVS_KEY_PASSWORD, password != NULL ? password : "");
    if (err == ESP_OK)
        err = nvs_commit(handle);

    nvs_close(handle);

    return err == ESP_OK;
}
