#include "dto_ws.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "esp_crt_bundle.h"
#include "audio_codec_opus.h"
#include "audio_capture.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <stdio.h>

// Timestamp (us) when client sent session.end. Reset to 0 once first
// audio.play arrives so the latency is logged exactly once per session end.
volatile int64_t g_session_end_us = 0;

static const char *TAG = "WS_CLIENT";

static esp_websocket_client_handle_t client;
audio_play_cb_t g_audio_cb = NULL;
static bool ws_connected = false;
static bool session_active = false;
static bool s_user_ended_session = false;  // true after user presses BOOT to end

#define REASSEMBLY_BUF_SIZE 8192
static char *s_reassembly_buf = NULL;
static size_t s_reassembly_len = 0;
static size_t s_reassembly_total = 0;

// Static buffer for null-terminating received messages (avoids heap alloc per message)
#define MSG_BUF_SIZE 4096
static char s_msg_buf[MSG_BUF_SIZE];

extern char *dto_build_session_start(const char *);
extern char *dto_build_audio_frame(const char *, uint32_t, const uint8_t *, size_t, uint32_t, bool);
extern char *dto_build_heartbeat(const char *);
extern char *dto_build_session_end(const char *, const char *);
extern void dto_parse_server_msg(const char *);
extern void dto_audio_burst_log_and_reset(void);

static void ws_process_msg(const char *data, size_t len)
{
    if (len >= MSG_BUF_SIZE) {
        ESP_LOGW(TAG, "msg too large %u, drop", (unsigned)len);
        return;
    }
    memcpy(s_msg_buf, data, len);
    s_msg_buf[len] = '\0';
    dto_parse_server_msg(s_msg_buf);
}

static void ws_event(void *arg,
                     esp_event_base_t base,
                     int32_t event_id,
                     void *event_data)
{
    esp_websocket_event_data_t *data = event_data;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected (heap free=%lu)",
                 (unsigned long)esp_get_free_heap_size());
        ws_connected = true;
        session_active = false;
        ESP_LOGI(TAG, "Ready. Press BOOT to start session.");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket disconnected");
        dto_audio_burst_log_and_reset();  // flush any in-progress TTS burst to playback
        ws_connected = false;
        session_active = false;
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error: %s", data->error_handle.esp_transport_sock_errno > 0 
                 ? strerror(data->error_handle.esp_transport_sock_errno) 
                 : "Unknown");
        ws_connected = false;
        session_active = false;
        break;

    case WEBSOCKET_EVENT_DATA:
        // Skip control frames (ping=0x09, pong=0x0A, close=0x08)
        if (data->op_code == 0x09 || data->op_code == 0x0A || data->op_code == 0x08) {
            break;
        }

        if (data->data_ptr && data->data_len > 0)
        {
            if (data->payload_len > data->data_len) {
                if (data->payload_offset == 0) {
                    if (data->payload_len > REASSEMBLY_BUF_SIZE) {
                        ESP_LOGE(TAG, "reassembly: msg too large %u > %u",
                                 (unsigned)data->payload_len,
                                 (unsigned)REASSEMBLY_BUF_SIZE);
                        s_reassembly_total = 0;
                        s_reassembly_len = 0;
                        break;
                    }
                    s_reassembly_total = data->payload_len;
                    s_reassembly_len = 0;
                }
                if (s_reassembly_buf && s_reassembly_total > 0 &&
                    (s_reassembly_len + data->data_len <= s_reassembly_total)) {
                    memcpy(s_reassembly_buf + s_reassembly_len, data->data_ptr, data->data_len);
                    s_reassembly_len += data->data_len;
                }
                if (s_reassembly_total > 0 && s_reassembly_len >= s_reassembly_total) {
                    s_reassembly_buf[s_reassembly_total] = '\0';
                    dto_parse_server_msg(s_reassembly_buf);
                    s_reassembly_len = 0;
                    s_reassembly_total = 0;
                }
            } else {
                ws_process_msg((const char *)data->data_ptr, data->data_len);
            }
        }
        break;

    default:
        ESP_LOGD(TAG, "WebSocket event: %ld", event_id);
        break;
    }
}

void dto_ws_init(const dto_ws_config_t *cfg)
{
    ESP_LOGI(TAG, "Initializing WebSocket client with URI: %s", cfg->uri);
    if (s_reassembly_buf == NULL) {
        s_reassembly_buf = (char *)heap_caps_malloc(REASSEMBLY_BUF_SIZE + 1, MALLOC_CAP_8BIT);
        if (s_reassembly_buf == NULL) {
            ESP_LOGE(TAG, "Failed to pre-alloc reassembly buffer (%d bytes)", REASSEMBLY_BUF_SIZE + 1);
        } else {
            ESP_LOGI(TAG, "Pre-allocated reassembly buffer: %d bytes", REASSEMBLY_BUF_SIZE + 1);
        }
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = cfg->uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = 8192,
        .task_prio = 7,
        // .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
    };

    client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event, NULL);
}

void dto_ws_start(void)
{
    esp_websocket_client_start(client);
}

void dto_ws_stop(void)
{
    esp_websocket_client_stop(client);
}

static void ws_client_send(char *json)
{
    if (!ws_connected)
    {
        ESP_LOGW(TAG, "WebSocket not connected, dropping message");
        free(json);
        return;
    }
    
    int len = (int)strlen(json);
    int ret = esp_websocket_client_send_text(client, json, len, pdMS_TO_TICKS(2000));
    if (ret < 0) {
        ESP_LOGW(TAG, "WS send FAILED len=%d", len);
    } else {
        ESP_LOGD(TAG, "WS sent %d bytes: %.60s...", ret, json);
    }
    free(json);
}

/* API */

static uint32_t s_audio_send_count = 0;
static uint32_t s_session_seq = 0;  // Per-session audio.frame sequence_no (1..N)

void dto_ws_session_start(const char *session_id)
{
    s_user_ended_session = false;  // Starting new session clears the flag
    s_session_seq = 0;             // Reset audio.frame counter for new session

    audio_capture_set_paused(false);
    audio_opus_encoder_resume();

    char fresh_id[20];
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    snprintf(fresh_id, sizeof(fresh_id), "sess_%04lx%04lx",
             (unsigned long)(r1 & 0xFFFF), (unsigned long)(r2 & 0xFFFF));
    dto_ws_set_session_id(fresh_id);
    ESP_LOGI(TAG, "session.start with fresh id=%s", fresh_id);

    ws_client_send(dto_build_session_start(fresh_id));
}

void dto_ws_session_end(const char *session_id, const char *reason)
{
    session_active = false;  // Stop transport from sending more audio
    s_user_ended_session = true;  // Prevent auto-restart on reconnect
    audio_capture_set_paused(true);
    audio_opus_encoder_pause();  // Stop encoder to free CPU for receiving audio.play
    g_session_end_us = esp_timer_get_time(); // add timestamp for latency logging when session.end -> audio.play
    ESP_LOGI(TAG, "session.end sent at t=%lld us, waiting for first audio.play...",
             (long long)g_session_end_us);
    ws_client_send(dto_build_session_end(session_id, reason));
}

void dto_ws_send_audio_frame(const char *session_id,
                             uint32_t seq,
                             const uint8_t *data,
                             size_t len,
                             uint32_t duration_ms,
                             bool is_last)
{
    (void)seq;
    s_audio_send_count++;
    s_session_seq++;
    uint32_t out_seq = s_session_seq;
    if (s_audio_send_count <= 3 || (s_audio_send_count % 100) == 0) {
        ESP_LOGI(TAG, ">> audio.frame #%lu seq=%lu len=%u dur=%lums",
                 (unsigned long)s_audio_send_count, (unsigned long)out_seq,
                 (unsigned)len, (unsigned long)duration_ms);
    }
    ws_client_send(dto_build_audio_frame(session_id, out_seq, data, len, duration_ms, is_last));

    // is_last=true is the signal to stop the uplink stream WITHOUT sending
    // session.end. Mirror the stop side-effects of dto_ws_session_end():
    //   - mark session inactive so transport task stops sending frames
    //   - pause capture + encoder to free CPU and stop producing data
    //   - record timestamp for end->audio.play latency logging
    if (is_last) {
        session_active = false;
        s_user_ended_session = true;
        audio_capture_set_paused(true);
        audio_opus_encoder_pause();
        g_session_end_us = esp_timer_get_time();
        ESP_LOGI(TAG, "audio.frame is_last=true sent at t=%lld us, uplink stopped, waiting for audio.play...",
                 (long long)g_session_end_us);
    }
}

void dto_ws_heartbeat(const char *session_id)
{
    ws_client_send(dto_build_heartbeat(session_id));
}

void dto_ws_register_audio_cb(audio_play_cb_t cb)
{
    g_audio_cb = cb;
}

void ws_client_init(void){
    ESP_LOGI(TAG, "Starting WebSocket client initialization");

    dto_ws_config_t cfg = {
        .uri = "wss://voicebot-service-163783299662.asia-southeast1.run.app/ws",
        .robot_id = "robot_001",
        .keep_alive_enable = true,
        .keep_alive_idle = 60000,
        .keep_alive_interval = 50000,
        .keep_alive_count = 3,
    };

    ws_connected = false;
    dto_ws_init(&cfg);
    dto_ws_set_robot_id(cfg.robot_id);
    dto_ws_start();
    
    ESP_LOGI(TAG, "WebSocket client started, waiting for connection...");
}

bool ws_is_connected(void)
{
    return ws_connected;
}

bool ws_is_session_active(void)
{
    return session_active;
}

void ws_set_session_active(bool active)
{
    session_active = active;
}