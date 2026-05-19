#pragma once

#include "cmn.h"
#include "esp_bit_defs.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

void wifi_init_softap(void);
void wifi_init_sta(void);
void wifi_reconfigure(void);
char *wifi_scan(void);
void wifi_parseDataFromWebserver(char *buf,
                                 char *wifiName,
                                 uint8_t wifiNameLen,
                                 char *wifiPass,
                                 uint8_t wifiPassLen);
