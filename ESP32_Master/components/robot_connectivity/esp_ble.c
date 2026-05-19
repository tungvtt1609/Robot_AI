#include "esp_ble.h"

#include <assert.h>
#include <string.h>

#include "cmn.h"
#include "db.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/ans/ble_svc_ans.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "wifi.h"

static const char *TAG = "robot_ble";

static int copy_mbuf_to_cstr(struct os_mbuf *om, char *dst, size_t dst_len)
{
    uint16_t copied = 0;
    if (dst_len == 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    int rc = ble_hs_mbuf_to_flat(om, dst, dst_len - 1, &copied);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    dst[copied] = '\0';
    return 0;
}

static int gatt_write_cb(uint16_t conn, uint16_t attr,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;

    switch ((uintptr_t)arg) {
    case 1:
        if (copy_mbuf_to_cstr(ctxt->om, g_sysParamSave.wifi_ssid, sizeof(g_sysParamSave.wifi_ssid)) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        ESP_LOGI(TAG, "Received SSID: %s", g_sysParamSave.wifi_ssid);
        g_sysDiagnostic.wifi_reConfigured = true;
        break;

    case 2:
        if (copy_mbuf_to_cstr(ctxt->om, g_sysParamSave.wifi_password, sizeof(g_sysParamSave.wifi_password)) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        ESP_LOGI(TAG, "Received WiFi password (%u bytes)", (unsigned)strlen(g_sysParamSave.wifi_password));
        g_sysDiagnostic.wifi_reConfigured = true;
        break;

    case 3: {
        uint8_t volume = 0;
        uint16_t copied = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, &volume, sizeof(volume), &copied);
        if (rc != 0 || copied != 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        g_sysParamSave.volume = volume;
        ESP_LOGI(TAG, "Received Volume: %u", (unsigned)g_sysParamSave.volume);
        break;
    }

    case 4:
        g_sysDiagnostic.config_applied = true;
        ESP_LOGI(TAG, "Configuration apply requested");
        db_Save();
        if (g_sysDiagnostic.wifi_reConfigured) {
            ESP_LOGI(TAG, "Reconfiguring WiFi...");
            wifi_reconfigure();
            g_sysDiagnostic.wifi_reConfigured = false;
        }
        break;

    default:
        ESP_LOGW(TAG, "Unknown BLE write arg=%u", (unsigned)(uintptr_t)arg);
        break;
    }

    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                                    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x10),
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = BLE_UUID128_DECLARE(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                                                0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x11),
                    .access_cb = gatt_write_cb,
                    .flags = BLE_GATT_CHR_F_WRITE,
                    .arg = (void *)1,
                },
                {
                    .uuid = BLE_UUID128_DECLARE(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                                                0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12),
                    .access_cb = gatt_write_cb,
                    .flags = BLE_GATT_CHR_F_WRITE,
                    .arg = (void *)2,
                },
                {
                    .uuid = BLE_UUID128_DECLARE(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                                                0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x13),
                    .access_cb = gatt_write_cb,
                    .flags = BLE_GATT_CHR_F_WRITE,
                    .arg = (void *)3,
                },
                {
                    .uuid = BLE_UUID128_DECLARE(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                                                0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x14),
                    .access_cb = gatt_write_cb,
                    .flags = BLE_GATT_CHR_F_WRITE,
                    .arg = (void *)4,
                },
                {0},
            },
    },
    {0},
};

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void esp_ble_Init(void)
{
    int rc;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_ans_init();

    rc = ble_gatts_count_cfg(gatt_svcs);
    assert(rc == 0);

    rc = ble_gatts_add_svcs(gatt_svcs);
    assert(rc == 0);

    ble_svc_gap_device_name_set("AI Robot");
    nimble_port_freertos_init(ble_host_task);
}
