#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "db.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "html.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "dto_ws.h"

static const char *TAG = "robot_wifi";
static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

#define DEFAULT_SCAN_LIST_SIZE 10

static esp_err_t setUpInfor_handler(httpd_req_t *req)
{
    char *wifiList = wifi_scan();
    char *html = generateHTMLSetUp(wifiList);

    if (wifiList) {
        free(wifiList);
    }

    if (html == NULL) {
        return httpd_resp_send_500(req);
    }

    esp_err_t err = httpd_resp_sendstr(req, html);
    free(html);
    return err;
}

static esp_err_t showResponse_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return ESP_FAIL;
    }

    buf[ret] = '\0';
    wifi_parseDataFromWebserver(buf,
                                g_sysParamSave.wifi_ssid,
                                sizeof(g_sysParamSave.wifi_ssid),
                                g_sysParamSave.wifi_password,
                                sizeof(g_sysParamSave.wifi_password));

    g_sysParamSave.wifiMode = WIFI_STATION_MODE;
    db_Save();

    httpd_resp_sendstr(req, htmlShowResponse());
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t post_handler(httpd_req_t *req)
{
    char content[384] = {0};
    int content_len = req->content_len;
    if (content_len >= (int)sizeof(content)) {
        content_len = (int)sizeof(content) - 1;
    }

    int ret = httpd_req_recv(req, content, content_len);
    if (ret <= 0) {
        return httpd_resp_send_500(req);
    }

    content[ret] = '\0';
    ESP_LOGI(TAG, "Received POST: %s", content);
    return httpd_resp_sendstr(req, "{\"message\":\"OK\",\"error_code\":\"0\"}");
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting HTTP server");
        return NULL;
    }

    const httpd_uri_t setUpInfor_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = setUpInfor_handler,
    };

    const httpd_uri_t showClientResponse_uri = {
        .uri = "/show",
        .method = HTTP_POST,
        .handler = showResponse_handler,
        .user_ctx = NULL,
    };

    const httpd_uri_t uri_post = {
        .uri = "/api/v1/config",
        .method = HTTP_POST,
        .handler = post_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(server, &setUpInfor_uri);
    httpd_register_uri_handler(server, &showClientResponse_uri);
    httpd_register_uri_handler(server, &uri_post);

    return server;
}

static esp_err_t stop_webserver(httpd_handle_t server)
{
    return httpd_stop(server);
}

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)event_base;
    (void)event_id;
    (void)event_data;
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (server && *server) {
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    (void)event_base;
    (void)event_id;
    (void)event_data;
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (server && *server == NULL) {
        *server = start_webserver();
    }
}

static void wifi_event_handler_ap(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}

static void wifi_event_handler_sta(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 1000) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry connecting to AP");
            g_sysDiagnostic.wifi_connected = false;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        g_sysDiagnostic.wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_softap(void)
{
    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler_ap, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "AI_Robot",
            .ssid_len = strlen("AI_Robot"),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {.required = true},
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 1, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 1, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(netif));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    server = start_webserver();
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler_sta, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler_sta, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    strncpy((char *)wifi_config.sta.ssid, g_sysParamSave.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, g_sysParamSave.wifi_password, sizeof(wifi_config.sta.password) - 1);

    if (strlen((char *)wifi_config.sta.password) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", g_sysParamSave.wifi_ssid);
        ws_client_init();
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "failed to connect to SSID:%s", g_sysParamSave.wifi_ssid);
    }
}

void wifi_parseDataFromWebserver(char *buf,
                                 char *wifiName,
                                 uint8_t wifiNameLen,
                                 char *wifiPass,
                                 uint8_t wifiPassLen)
{
    char *user_ptr = strstr(buf, "wifi_username=");
    char *pass_ptr = strstr(buf, "wifi_password=");
    char temp[128] = {0};

    if (user_ptr) {
        user_ptr += 14;
        if (sscanf(user_ptr, "%127[^\r\n&]", temp) == 1) {
            strncpy(wifiName, temp, wifiNameLen - 1);
            wifiName[wifiNameLen - 1] = '\0';
        }
    }

    if (pass_ptr) {
        pass_ptr += 14;
        if (sscanf(pass_ptr, "%127[^\r\n&]", temp) == 1) {
            strncpy(wifiPass, temp, wifiPassLen - 1);
            wifiPass[wifiPassLen - 1] = '\0';
        } else {
            memset(wifiPass, 0x00, wifiPassLen);
        }
    }

    ESP_LOGI(TAG, "wifi parsed: <%s>", wifiName);
}

char *wifi_scan(void)
{
    size_t bufferSize = 1024;
    char *list = (char *)malloc(bufferSize);
    if (!list) {
        return NULL;
    }
    list[0] = '\0';

    uint16_t number = DEFAULT_SCAN_LIST_SIZE;
    wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];
    uint16_t ap_count = 0;
    memset(ap_info, 0, sizeof(ap_info));

    esp_wifi_scan_start(NULL, true);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));

    for (int i = 0; i < number; i++) {
        char entry[96];
        snprintf(entry, sizeof(entry), "%s\"%s\"", (i == 0 ? "" : ","), (char *)ap_info[i].ssid);
        if (strlen(list) + strlen(entry) + 1 >= bufferSize) {
            break;
        }
        strcat(list, entry);
    }

    return list;
}

void wifi_reconfigure(void)
{
    wifi_config_t cfg = {0};

    esp_wifi_disconnect();

    strncpy((char *)cfg.sta.ssid, g_sysParamSave.wifi_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, g_sysParamSave.wifi_password, sizeof(cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
}
