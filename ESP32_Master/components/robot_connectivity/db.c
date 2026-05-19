#include "db.h"

#include <string.h>

#include "cmn.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "robot_db";

sSysParamSave g_sysParamSave = {0};
volatile sSysDiagnostic g_sysDiagnostic = {0};

void db_Init(void)
{
    nvs_handle_t nvsHandle;
    esp_err_t err = nvs_open(COMMON_STORAGE, NVS_READWRITE, &nvsHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return;
    }

    size_t required_size = sizeof(sSysParamSave);
    err = nvs_get_blob(nvsHandle, "sys_param", &g_sysParamSave, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No data in NVS, loading defaults");
        memset(&g_sysParamSave, 0, sizeof(g_sysParamSave));
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error reading NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Loaded NVS config, wifiMode=%u, ssid=%s",
                 (unsigned)g_sysParamSave.wifiMode,
                 g_sysParamSave.wifi_ssid);
    }

    nvs_close(nvsHandle);
}

void db_Save(void)
{
    nvs_handle_t nvsHandle;
    esp_err_t err = nvs_open(COMMON_STORAGE, NVS_READWRITE, &nvsHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(nvsHandle, "sys_param", &g_sysParamSave, sizeof(g_sysParamSave));
    if (err == ESP_OK) {
        err = nvs_commit(nvsHandle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error writing NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved config to NVS");
    }

    nvs_close(nvsHandle);
}
