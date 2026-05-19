#include "cJSON.h"
#include "dto_ws.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "audio_transport.h"

static const char *TAG = "DTO_PARSE";

extern void ws_set_session_active(bool active);
extern volatile int64_t g_session_end_us;

// ─── audio.play streaming (heap-fragmentation-proof) ────────────────────────
//
// Earlier design accumulated every chunk into per-chunk malloc'd buffers and
// flushed them at end-of-burst. This caused heap fragmentation and
// `audio.play malloc(1924) failed, free=4104` even with 4KB free total.
//
// New design: decode each chunk into ONE static buffer and push it straight
// to the playback callback. The server is observed to send chunks in
// sequence_no order so no sorting is needed (a warning is logged if not).
//
// State across chunks of a single burst:
//   - s_burst_active        : we have already seen at least one audio.play
//   - s_burst_first_seq/last: range of seq numbers seen
//   - s_burst_total_bytes   : total PCM forwarded
//   - s_burst_chunk_count   : how many chunks streamed
// Reset at end-of-burst (non-audio msg / WS disconnect / is_last=true).

#define DECODE_BUF_SIZE 4096   // > base64-decoded size of typical 1920-byte audio chunk + slack

static uint8_t s_decode_buf[DECODE_BUF_SIZE];
static bool   s_burst_active        = false;
static int    s_burst_first_seq     = -1;
static int    s_burst_last_seq      = -1;
static int    s_burst_chunk_count   = 0;
static size_t s_burst_total_bytes   = 0;

// Public flag mirroring s_burst_active. Read by the playback task so it
// does NOT prematurely drop back to idle during a mid-burst server gap
// (Cloud Run pacing can stall up to ~600 ms between chunks). Only when
// this is false (is_last received OR ws disconnect) is the playback task
// allowed to declare end-of-burst on ringbuffer-empty.
volatile bool g_downlink_burst_active = false;

// Strip WAV/RIFF header in-place from a chunk buffer.
// Returns offset past the "data" sub-chunk header, or 0 if no WAV header found.
static size_t wav_header_offset(const uint8_t *p, size_t n)
{
    if (n < 44 || p[0] != 'R' || p[1] != 'I' || p[2] != 'F' || p[3] != 'F') return 0;
    for (size_t i = 12; i + 8 <= n; i++) {
        if (p[i] == 'd' && p[i+1] == 'a' && p[i+2] == 't' && p[i+3] == 'a') {
            return i + 8;
        }
    }
    return 0;
}

static void burst_log_end(const char *reason)
{
    if (!s_burst_active) return;
    ESP_LOGI(TAG, "TTS burst end (%s): %d chunks, %u PCM bytes (seq %d..%d)",
             reason, s_burst_chunk_count, (unsigned)s_burst_total_bytes,
             s_burst_first_seq, s_burst_last_seq);
    s_burst_active      = false;
    g_downlink_burst_active = false;
    s_burst_first_seq   = -1;
    s_burst_last_seq    = -1;
    s_burst_chunk_count = 0;
    s_burst_total_bytes = 0;
    // Flush any leftover bytes (zero-padded final frame) and reset the
    // downlink frame-aligner so the next burst starts cleanly.
    if (g_audio_cb) {
        g_audio_cb(NULL, 0, true);
    }
    // audio_downlink_reset();
}

// Public: invoked from ws_client when WebSocket disconnects.
void dto_audio_burst_log_and_reset(void)
{
    burst_log_end("ws_disconnect");
}

// ─── audio.play fast-path (no cJSON) ────────────────────────────────────────
// cJSON_Parse on a ~2700B audio.play message peaks ~8 KB heap. During a TTS
// burst free heap drops to ~3-5 KB and the parse silently fails, dropping
// entire chunks (notably seq=0 carrying the WAV header). The message format
// is fixed and the base64 payload contains no JSON-escapable characters, so
// we can extract the three fields we need with a small linear scan.
//
// Find a JSON key like "name" and return pointer to the first non-whitespace
// character after the colon, or NULL if not found. Tolerates "key":val and
// "key" : val style spacing produced by python json.dumps default settings.
static const char *find_json_value(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        const char *q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            return q;
        }
        p += klen;
    }
    return NULL;
}

// Returns true if we recognised audio.play and consumed the message
// (success or controlled drop). Returns false if it isn't audio.play and
// the caller should fall back to cJSON for full parsing.
static bool try_handle_audio_play_fast(const char *json)
{
    const char *type_val = find_json_value(json, "\"type\"");
    if (!type_val || strncmp(type_val, "\"audio.play\"", 12) != 0) return false;

    // sequence_no
    int seq_no = -1;
    const char *p = find_json_value(json, "\"sequence_no\"");
    if (p) seq_no = (int)strtol(p, NULL, 10);

    // is_final: server may use either "is_last", "is_final_chunk" or "is_final"
    bool is_final = false;
    const char *q = find_json_value(json, "\"is_last\"");
    if (!q) q = find_json_value(json, "\"is_final_chunk\"");
    if (!q) q = find_json_value(json, "\"is_final\"");
    if (q && strncmp(q, "true", 4) == 0) is_final = true;

    // pcm_bytes:"...."
    const char *kb = find_json_value(json, "\"pcm_bytes\"");
    if (!kb || *kb != '\"') {
        ESP_LOGW(TAG, "audio.play fast: missing pcm_bytes (seq=%d)", seq_no);
        return true;
    }
    const char *b64 = kb + 1;
    const char *end = strchr(b64, '\"');
    if (!end) {
        ESP_LOGW(TAG, "audio.play fast: unterminated pcm_bytes (seq=%d)", seq_no);
        return true;
    }
    size_t b64_len = (size_t)(end - b64);

    size_t out_len = 0;
    int rc = mbedtls_base64_decode(s_decode_buf, sizeof(s_decode_buf), &out_len,
                                   (const unsigned char *)b64, b64_len);
    if (rc != 0) {
        ESP_LOGW(TAG, "audio.play fast: b64 decode rc=%d (in=%u)", rc, (unsigned)b64_len);
        return true;
    }

    uint8_t *pcm_ptr = s_decode_buf;
    size_t   pcm_len = out_len;
    if (!s_burst_active) {
        size_t off = wav_header_offset(s_decode_buf, out_len);
        if (off > 0 && off < out_len) {
            pcm_ptr += off;
            pcm_len -= off;
        }
        s_burst_active    = true;
        g_downlink_burst_active = true;
        s_burst_first_seq = seq_no;

        // Log latency from session.end -> first audio.play arrival.
        int64_t t_end = g_session_end_us;
        if (t_end != 0) {
            int64_t dt_us = esp_timer_get_time() - t_end;
            ESP_LOGI(TAG, "LATENCY session.end -> audio.play[0] = %lld ms",
                     (long long)(dt_us / 1000));
            g_session_end_us = 0;  // arm only once per session.end
        }
    } else if (seq_no <= s_burst_last_seq) {
        ESP_LOGW(TAG, "audio.play out-of-order seq=%d (prev=%d)",
                 seq_no, s_burst_last_seq);
    }
    s_burst_last_seq = seq_no;
    s_burst_chunk_count++;
    s_burst_total_bytes += pcm_len;

    ESP_LOGI(TAG, "audio.play fast: seq=%d len=%u final=%s (burst: %d chunks, %u bytes)",
             seq_no, (unsigned)pcm_len, is_final ? "yes" : "no",
             s_burst_chunk_count, (unsigned)s_burst_total_bytes);

    // NO per-chunk log here. Each ESP_LOGI call serialises ~100 bytes over
    // UART @115200 baud (~9 ms) and the WS event task can't drain the next
    // chunk until this one returns. Cloud Run / GFE on the server side
    // observes the back-pressure and RSTs the connection mid-burst, so the
    // ESP receives only ~6-13 of 28 chunks. Burst stats are still logged
    // once at end-of-burst by burst_log_end().

    if (g_audio_cb && pcm_len > 0) {
        g_audio_cb(pcm_ptr, pcm_len, is_final);
    }

    if (is_final) burst_log_end("is_last");
    return true;
}

void dto_parse_server_msg(const char *json)
{
    // Fast-path: handle audio.play without invoking cJSON. Avoids ~8 KB heap
    // peak that otherwise causes mid-burst chunk drops.
    if (try_handle_audio_play_fast(json)) return;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        size_t jlen = json ? strlen(json) : 0;
        ESP_LOGE(TAG, "cJSON_Parse FAILED (msg_len=%u, free=%lu)",
                 (unsigned)jlen,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        return;
    }
    const char *type = type_item->valuestring;

    // Any non-audio message arriving mid-burst marks end-of-TTS.
    if (s_burst_active) {
        burst_log_end("non_audio_msg");
    }

    // ESP_LOGI(TAG, "Received: %s", json);

    if (strcmp(type, "session.started") == 0)
    {
        ESP_LOGI(TAG, "Session confirmed by server");
        ws_set_session_active(true);
    }
    else if (strcmp(type, "error") == 0)
    {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        cJSON *code = cJSON_GetObjectItem(payload, "code");
        cJSON *msg = cJSON_GetObjectItem(payload, "message");
        ESP_LOGE(TAG, "Server error: code=%s msg=%s",
                 (code && cJSON_IsString(code)) ? code->valuestring : "?",
                 (msg && cJSON_IsString(msg)) ? msg->valuestring : "?");
    }

    cJSON_Delete(root);
}