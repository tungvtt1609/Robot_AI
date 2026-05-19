#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char uri[128];
    char robot_id[32];
    bool keep_alive_enable;
    uint32_t keep_alive_idle;
    uint32_t keep_alive_interval;
    uint32_t keep_alive_count;
} dto_ws_config_t;

/* ===== INIT ===== */
void dto_ws_init(const dto_ws_config_t *cfg);
void dto_ws_start(void);
void dto_ws_stop(void);
void ws_client_init(void);
bool ws_is_connected(void);
bool ws_is_session_active(void);

/* ===== SESSION ===== */
void dto_ws_session_start(const char *session_id);
void dto_ws_session_end(const char *session_id, const char *reason);
void dto_ws_set_session_id(const char *id);
const char *dto_ws_get_session_id(void);

/* ===== AUDIO ===== */
// duration_ms = wall-clock milliseconds of audio in this chunk (matches the
// reference test1.py client). Use 0 if unknown.
void dto_ws_send_audio_frame(const char *session_id,
                             uint32_t seq,
                             const uint8_t *data,
                             size_t len,
                             uint32_t duration_ms,
                             bool is_last);

/* ===== HEARTBEAT ===== */
void dto_ws_heartbeat(const char *session_id);

void dto_ws_set_robot_id(const char *id);

/* ===== CALLBACK ===== */
typedef void (*audio_play_cb_t)(const uint8_t *data,
                                size_t len,
                                bool is_final);

void dto_ws_register_audio_cb(audio_play_cb_t cb);


extern audio_play_cb_t g_audio_cb;

#ifdef __cplusplus
}
#endif