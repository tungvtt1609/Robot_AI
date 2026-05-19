#include "audio_pipeline.h"
#include "audio_codec_opus.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "robot_connectivity.h"
#include "dto_ws.h"
#include "db.h"
#include "cmn.h"
#include <string.h>
#include "esp_timer.h"

static const char *TAG = "TTL_MAIN";

#define BOOT_BUTTON_GPIO ((gpio_num_t)0)
#define BOOT_BUTTON_ACTIVE_LEVEL 0
#define RESET_WIFI_HOLD_MS 3000

static void check_factory_reset(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        return;
    }

    if (gpio_get_level(BOOT_BUTTON_GPIO) != BOOT_BUTTON_ACTIVE_LEVEL) {
        return;
    }

    ESP_LOGW(TAG, "BOOT held at startup, hold %d ms to reset WiFi...", RESET_WIFI_HOLD_MS);
    int hold_count = 0;
    while (gpio_get_level(BOOT_BUTTON_GPIO) == BOOT_BUTTON_ACTIVE_LEVEL) {
        vTaskDelay(pdMS_TO_TICKS(50));
        hold_count += 50;
        if (hold_count >= RESET_WIFI_HOLD_MS) {
            ESP_LOGW(TAG, "WiFi config reset! Switching to AP mode...");
            memset(g_sysParamSave.wifi_ssid, 0, sizeof(g_sysParamSave.wifi_ssid));
            memset(g_sysParamSave.wifi_password, 0, sizeof(g_sysParamSave.wifi_password));
            g_sysParamSave.wifiMode = WIFI_ACCESS_POINT_MODE;
            db_Save();
            // Wait for button release then restart
            while (gpio_get_level(BOOT_BUTTON_GPIO) == BOOT_BUTTON_ACTIVE_LEVEL) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            esp_restart();
        }
    }
    ESP_LOGI(TAG, "BOOT released early, normal boot");
}

static void wait_for_boot_button(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT button GPIO init failed: %s (0x%x), start audio immediately",
                 esp_err_to_name(err), (unsigned int)err);
        return;
    }

    ESP_LOGI(TAG, "Press BOOT (GPIO%d) to start audio", BOOT_BUTTON_GPIO);

    int stable_pressed = 0;
    while (stable_pressed < 1) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == BOOT_BUTTON_ACTIVE_LEVEL) {
            stable_pressed++;
        } else {
            stable_pressed = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Wait until button release to avoid accidental long-press side effects.
    while (gpio_get_level(BOOT_BUTTON_GPIO) == BOOT_BUTTON_ACTIVE_LEVEL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "BOOT pressed, starting audio pipeline");
}

static bool check_boot_button_pressed(void)
{
    int stable = 0;
    while (gpio_get_level(BOOT_BUTTON_GPIO) == BOOT_BUTTON_ACTIVE_LEVEL) {
        stable++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return (stable > 0);
}

extern void dto_parse_server_msg(const char *);

// Period of mid-session OGG/Opus chunk flush. Mirrors the reference test1.py
// client (chunk_ms default 500). Sending data every ~500 ms keeps the GCP
// streaming STT session alive (no "stream timed out" errors) and lets the
// server start transcription before the user has finished speaking.
#define STREAM_FLUSH_MS 500

// Wall-clock (esp_timer) when the current session was resumed, or 0 if no
// active session. Updated whenever we start/stop a session in app_main.
static int64_t s_session_start_us = 0;
// Wall-clock of the last mid-session streaming flush, used to schedule the
// next 500 ms tick.
static int64_t s_last_stream_flush_us = 0;

// Heartbeat period (per README §11.2: keep WS alive, debug). Sent whenever the
// WebSocket is connected, regardless of session state.
#define HEARTBEAT_PERIOD_MS 5000
static int64_t s_last_heartbeat_us = 0;

// Send whatever has been encoded since the previous take_*() as a single
// audio.frame. is_final controls whether this is also the end-of-turn marker.
// Returns the number of bytes actually sent.
static size_t flush_session_chunk(bool is_final, uint32_t duration_ms)
{
    const uint8_t *buf = NULL;
    size_t n = 0;
    esp_err_t err = is_final
        ? audio_opus_encoder_take_session(&buf, &n)
        : audio_opus_encoder_take_session_chunk(&buf, &n);
    if (err != ESP_OK) {
        return 0;
    }

    if (n == 0 && !is_final) {
        // Nothing new to send and not the final marker -> skip silently.
        return 0;
    }

    // Even if n == 0 and is_final, we still emit a single empty audio.frame
    // with is_final=true so the server knows the turn is complete.
    dto_ws_send_audio_frame(dto_ws_get_session_id(), 0, buf, n, duration_ms, is_final);
    return n;
}

// End-of-turn flush: drain any remaining encoded bytes with is_final=true.
// The encoder is paused first so no new bytes can be appended after the take.
static void flush_session_to_server(void)
{
    int64_t t0 = esp_timer_get_time();
    audio_opus_encoder_pause();

    int64_t span_ms = s_session_start_us != 0
        ? (esp_timer_get_time() - s_last_stream_flush_us) / 1000
        : 0;
    if (span_ms < 0) span_ms = 0;

    size_t n = flush_session_chunk(true, (uint32_t)span_ms);
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "final flush: %u bytes is_final=true | %ldms",
             (unsigned)n, (long)((t1 - t0) / 1000));
}

void app_main(void)
{
    ESP_LOGI(TAG, "app_main enter");

    check_factory_reset();

    esp_err_t conn_err = robot_connectivity_init();
    if (conn_err != ESP_OK) {
        ESP_LOGE(TAG, "robot connectivity init failed: %s (0x%x)",
                 esp_err_to_name(conn_err), (unsigned int)conn_err);
    } else {
        ESP_LOGI(TAG, "robot connectivity initialized (BLE + WiFi)");
    }

    wait_for_boot_button();

    esp_err_t err = audio_pipeline_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio start failed: %s (0x%x)", esp_err_to_name(err), (unsigned int)err);
    } else {
        ESP_LOGI(TAG, "Audio started");
    }

    // Start session immediately after pipeline if WebSocket already connected
    if (ws_is_connected()) {
        ESP_LOGI(TAG, "WS already connected, starting session");
        audio_opus_encoder_resume();
        dto_ws_session_start(dto_ws_get_session_id());
        s_session_start_us = esp_timer_get_time();
        s_last_stream_flush_us = s_session_start_us;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        // Periodic heartbeat to keep the WebSocket connection alive. Runs
        // independently of session state so the server can still see the
        // robot online between sessions.
        if (ws_is_connected()) {
            int64_t now = esp_timer_get_time();
            if (s_last_heartbeat_us == 0 ||
                (now - s_last_heartbeat_us) / 1000 >= HEARTBEAT_PERIOD_MS) {
                dto_ws_heartbeat(dto_ws_get_session_id());
                s_last_heartbeat_us = now;
            }
        } else {
            // Reset so first heartbeat fires immediately after reconnect.
            s_last_heartbeat_us = 0;
        }

        // Mid-session streaming flush (every STREAM_FLUSH_MS) so the server's
        // upstream STT never sees a long gap. Mirrors the reference client.
        if (ws_is_connected() && ws_is_session_active() && s_session_start_us != 0) {
            int64_t now = esp_timer_get_time();
            int64_t since_last_ms = (now - s_last_stream_flush_us) / 1000;
            if (since_last_ms >= STREAM_FLUSH_MS) {
                size_t sent = flush_session_chunk(false, (uint32_t)since_last_ms);
                s_last_stream_flush_us = now;
                if (sent > 0) {
                    ESP_LOGD(TAG, "stream chunk: %u bytes (%lldms window)",
                             (unsigned)sent, (long long)since_last_ms);
                }
            }
        }

        if (check_boot_button_pressed()) {
            if (ws_is_connected() && ws_is_session_active()) {
                ESP_LOGI(TAG, "BOOT pressed, finalizing session");
                flush_session_to_server();
                s_session_start_us = 0;
            } else if (ws_is_connected() && !ws_is_session_active()) {
                // User wants to start a new session after previous ended
                ESP_LOGI(TAG, "BOOT pressed, starting new session");
                audio_opus_encoder_resume();
                dto_ws_session_start(dto_ws_get_session_id());
                s_session_start_us = esp_timer_get_time();
                s_last_stream_flush_us = s_session_start_us;
            }
        }
    }
}
