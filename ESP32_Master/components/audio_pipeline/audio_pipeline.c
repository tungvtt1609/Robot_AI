#include "audio_pipeline.h"

#include "audio_aec.h"
#include "audio_capture.h"
#include "audio_codec_opus.h"
#include "audio_common.h"
#include "audio_playback.h"
#include "audio_transport.h"
#include "esp_log.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"

static const char *TAG = "TTL_audio_pipeline";

esp_err_t audio_pipeline_start(void)
{
    RingbufHandle_t rb_mic_to_aec = xRingbufferCreate(4 * AUDIO_PCM_FRAME_BYTES, RINGBUF_TYPE_BYTEBUF);
    RingbufHandle_t rb_aec_to_enc = xRingbufferCreate(4 * AUDIO_PCM_FRAME_BYTES, RINGBUF_TYPE_BYTEBUF);
    // rb_enc_to_net removed: encoder now writes directly to a PSRAM session
    // buffer that main.c flushes over WebSocket at end-of-turn.
    RingbufHandle_t rb_net_to_dec = xRingbufferCreate(12 * sizeof(audio_packet_t), RINGBUF_TYPE_BYTEBUF);
    // RingbufHandle_t rb_dec_to_spk = xRingbufferCreate(16 * AUDIO_PCM_FRAME_BYTES, RINGBUF_TYPE_BYTEBUF);
    // 400 frames = ~12 s of PCM in PSRAM; absorbs cloud-server bursty pacing
    // (we observe sustained gaps where input rate < playback rate of 30 ms/frame).
    RingbufHandle_t rb_dec_to_spk = xRingbufferCreateWithCaps(
    400 * AUDIO_PCM_FRAME_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);

    if (rb_mic_to_aec == NULL || rb_aec_to_enc == NULL ||
        rb_net_to_dec == NULL || rb_dec_to_spk == NULL) {
        ESP_LOGE(TAG, "failed to create ringbuffers");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(audio_capture_start(rb_mic_to_aec));
    ESP_ERROR_CHECK(audio_aec_start(rb_mic_to_aec, rb_aec_to_enc));
    // Encoder still receives a non-NULL out_rb arg for backward compat but
    // does not push uplink packets there anymore (session-buffer flush model).
    ESP_ERROR_CHECK(audio_opus_encoder_start(rb_aec_to_enc, NULL));
    ESP_ERROR_CHECK(audio_transport_start(NULL, rb_net_to_dec));
    ESP_ERROR_CHECK(audio_opus_decoder_start(rb_net_to_dec, rb_dec_to_spk));
    ESP_ERROR_CHECK(audio_playback_start(rb_dec_to_spk));
    return ESP_OK;
}
