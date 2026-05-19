#include "dto_ws.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char *robot_id_g = "robot_001";
static char session_id_g[32] = "sess_123";

const char *dto_ws_get_session_id(void)
{
    return session_id_g;
}

void dto_ws_set_session_id(const char *id)
{
    if (id == NULL) {
        return;
    }
    strncpy(session_id_g, id, sizeof(session_id_g) - 1);
    session_id_g[sizeof(session_id_g) - 1] = '\0';
}

static uint64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

// Generate a fresh message_id per call. Mirrors test.py:
//   f"msg_{uuid.uuid4().hex[:8]}"
// Server appears to dedupe / reject messages with repeated message_ids,
// which caused premature TLS FIN mid-TTS-burst when ESP reused "msg_0001".
static void make_msg_id(char out[20])
{
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    snprintf(out, 20, "msg_%04lx%04lx",
             (unsigned long)(r1 & 0xFFFF),
             (unsigned long)(r2 & 0xFFFF));
}

static char *base64_encode(const uint8_t *data, size_t len)
{
    size_t out_len = 0;
    size_t buf_len = len * 4 / 3 + 16;
    unsigned char *out = malloc(buf_len);
    if (out == NULL) {
        return NULL;
    }

    if (data != NULL && len > 0) {
        mbedtls_base64_encode(out, buf_len, &out_len, data, len);
    }
    out[out_len] = 0;

    return (char *)out;
}

char *dto_build_session_start(const char *session_id)
{
    cJSON *root = cJSON_CreateObject();

    char mid[20]; make_msg_id(mid);
    cJSON_AddStringToObject(root, "type", "session.start");
    cJSON_AddStringToObject(root, "message_id", mid);
    cJSON_AddStringToObject(root, "session_id", session_id);
    cJSON_AddStringToObject(root, "robot_id", robot_id_g);
    cJSON_AddNumberToObject(root, "timestamp_ms", now_ms());

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "robot_model", "esp32-bot");
    cJSON_AddStringToObject(payload, "firmware_version", "1.0.0");

    cJSON *fmt = cJSON_CreateObject();
    cJSON_AddStringToObject(fmt, "codec", "ogg_opus");
    cJSON_AddNumberToObject(fmt, "sample_rate_hz", 16000);
    cJSON_AddNumberToObject(fmt, "channels", 1);
    cJSON_AddNumberToObject(fmt, "frame_duration_ms", 20);

    cJSON_AddItemToObject(payload, "audio_format", fmt);
    cJSON_AddItemToObject(root, "payload", payload);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *dto_build_audio_frame(const char *session_id,
                            uint32_t seq,
                            const uint8_t *data,
                            size_t len,
                            uint32_t duration_ms,
                            bool is_last)
{
    char *b64 = base64_encode(data, len);

    cJSON *root = cJSON_CreateObject();
    char mid[20]; make_msg_id(mid);
    cJSON_AddStringToObject(root, "type", "audio.frame");
    cJSON_AddStringToObject(root, "message_id", mid);
    cJSON_AddStringToObject(root, "session_id", session_id);
    cJSON_AddStringToObject(root, "robot_id", robot_id_g);
    cJSON_AddNumberToObject(root, "timestamp_ms", now_ms());

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "sequence_no", seq);
    cJSON_AddStringToObject(payload, "audio_bytes", b64);
    cJSON_AddNumberToObject(payload, "duration_ms", duration_ms);
    cJSON_AddBoolToObject(payload, "is_final", is_last);

    cJSON_AddItemToObject(root, "payload", payload);

    char *out = cJSON_PrintUnformatted(root);

    free(b64);
    cJSON_Delete(root);
    return out;
}

char *dto_build_heartbeat(const char *session_id)
{
    cJSON *root = cJSON_CreateObject();

    char mid[20]; make_msg_id(mid);
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddStringToObject(root, "message_id", mid);
    cJSON_AddStringToObject(root, "session_id", session_id);
    cJSON_AddStringToObject(root, "robot_id", robot_id_g);
    cJSON_AddNumberToObject(root, "timestamp_ms", now_ms());

    cJSON_AddItemToObject(root, "payload", cJSON_CreateObject());

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *dto_build_session_end(const char *session_id, const char *reason)
{
    cJSON *root = cJSON_CreateObject();

    char mid[20]; make_msg_id(mid);
    cJSON_AddStringToObject(root, "type", "session.end");
    cJSON_AddStringToObject(root, "message_id", mid);
    cJSON_AddStringToObject(root, "session_id", session_id);
    cJSON_AddStringToObject(root, "robot_id", robot_id_g);
    cJSON_AddNumberToObject(root, "timestamp_ms", now_ms());

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "reason", reason);

    cJSON_AddItemToObject(root, "payload", payload);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* expose setter */
void dto_ws_set_robot_id(const char *id)
{
    if (id == NULL) {
        return;
    }
    if (robot_id_g != NULL && robot_id_g != (char *)"robot_001") {
        free(robot_id_g);
    }
    robot_id_g = strdup(id);
}