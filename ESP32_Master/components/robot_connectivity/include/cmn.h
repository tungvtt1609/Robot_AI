#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WIFI_SSID_SIZE 128
#define WIFI_PASSWORD_SIZE 128

#define WIFI_ACCESS_POINT_MODE 0
#define WIFI_STATION_MODE 1

typedef struct system_diagnostic_ {
    volatile bool wifi_check;
    volatile bool wifi_connected;
    volatile uint32_t count_ms_wifi;
    volatile uint32_t count_ms_mqtt;
    volatile bool config_applied;
    volatile bool wifi_reConfigured;
} sSysDiagnostic;

typedef struct system_param_ {
    char wifi_ssid[WIFI_SSID_SIZE];
    char wifi_password[WIFI_PASSWORD_SIZE];
    uint8_t volume;
    uint8_t wifiMode; // 1: Station mode, 0: Access Point mode
} sSysParamSave;

extern volatile sSysDiagnostic g_sysDiagnostic;
extern sSysParamSave g_sysParamSave;
