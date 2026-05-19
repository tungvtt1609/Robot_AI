#include "audio_transport.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio_common.h"
#include "audio_playback.h"
#include "dto_ws.h"
#include "esp_log.h"

static const char *TAG = "TTL_audio_transport";

typedef struct {
    RingbufHandle_t downlink_rb;
} transport_task_ctx_t;

static transport_task_ctx_t s_ctx;

static uint32_t s_downlink_seq = 0;
static bool     s_burst_finalized = false;  // prevent double-flush from is_last + ws_disconnect

void audio_downlink_reset(void)
{
    s_downlink_seq = 0;
    s_burst_finalized = false;
}

// Forward one PCM chunk to the decoder/playback ringbuffer with its actual
// length (no zero padding, no re-framing). The downstream playback task uses
// xRingbufferReceiveUpTo and i2s_channel_write, both of which handle variable
// chunk sizes natively, so we no longer force every packet to a fixed 30 ms
// frame. This removes the trailing silence that the old frame-aligner was
// padding onto the final partial frame of every TTS burst (server sends 20 ms
// chunks, our pipeline used 30 ms frames -> last 10-20 ms was always zero).
static void downlink_emit_chunk(const uint8_t *data, size_t len)
{
    if (len == 0) return;

    audio_packet_t packet;
    while (len > 0) {
        size_t take = len;
        if (take > sizeof(packet.payload)) {
            take = sizeof(packet.payload);
        }
        packet.sequence_no  = s_downlink_seq++;
        packet.timestamp_ms = 0;
        packet.payload_len  = (uint16_t)take;
        // duration_ms is informational only (downstream doesn't use it).
        packet.duration_ms  = (uint8_t)((take * 1000) /
                              (AUDIO_PCM_SAMPLE_RATE_HZ * AUDIO_PCM_CHANNELS *
                               (AUDIO_PCM_BITS_PER_SAMPLE / 8)));
        packet.codec_id     = AUDIO_CODEC_ID_PCM;
        memcpy(packet.payload, data, take);

        if (xRingbufferSend(s_ctx.downlink_rb, &packet, sizeof(packet),
                            pdMS_TO_TICKS(50)) != pdTRUE) {
            static uint32_t s_drop_cnt = 0;
            s_drop_cnt++;
            if (s_drop_cnt <= 3 || (s_drop_cnt % 10) == 0) {
                ESP_LOGW(TAG, "downlink rb full, drop seq=%lu (total=%lu)",
                         (unsigned long)packet.sequence_no, (unsigned long)s_drop_cnt);
            }
        }
        data += take;
        len  -= take;
    }
}

static void audio_downlink_cb(const uint8_t *data, size_t len, bool is_final)
{
    if (s_ctx.downlink_rb == NULL || data == NULL || len == 0) {
        if (is_final && !s_burst_finalized) {
            s_burst_finalized = true;
            // NOTE: do NOT call audio_playback_reset_state() here. The
            // ringbuffer still contains tail frames the playback task
            // hasn't drained yet; resetting now would re-arm the prebuffer
            // gate while those frames sit in buffer and never play.
            // Playback task auto-resets when it hits underrun (ringbuf
            // empty), which is exactly when the tail has finished.
            ESP_LOGI(TAG, "downlink: final signal, total seq=%lu",
                     (unsigned long)s_downlink_seq);
        }
        return;
    }

    // New audio data arriving - clear finalized flag (this might be a new burst)
    s_burst_finalized = false;

    downlink_emit_chunk(data, len);

    if (is_final) {
        if (s_burst_finalized) {
            return;
        }
        s_burst_finalized = true;
        ESP_LOGI(TAG, "downlink: final signal, total seq=%lu",
                 (unsigned long)s_downlink_seq);
    }
}

// NOTE: Uplink is no longer driven by a per-frame ringbuffer task. The Opus
// encoder buffers an entire user turn in a PSRAM session buffer, then
// flush_session_to_server() (in main.c) chunks it into audio.frame messages
// over WebSocket. The legacy `in_rb` parameter is kept for API compatibility
// but is unused.
esp_err_t audio_transport_start(RingbufHandle_t in_rb, RingbufHandle_t downlink_rb)
{
    (void)in_rb;
    if (downlink_rb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ctx.downlink_rb = downlink_rb;

    dto_ws_register_audio_cb(audio_downlink_cb);

    ESP_LOGI(TAG, "transport started (uplink via session flush, downlink via callback)");
    return ESP_OK;
}
