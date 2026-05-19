#pragma once

#include <stdint.h>

#define AUDIO_PCM_SAMPLE_RATE_HZ 16000
#define AUDIO_PCM_CHANNELS 1
#define AUDIO_PCM_BITS_PER_SAMPLE 16
#define AUDIO_FRAME_MS 30

#define AUDIO_PCM_SAMPLES_PER_FRAME ((AUDIO_PCM_SAMPLE_RATE_HZ * AUDIO_FRAME_MS) / 1000)
#define AUDIO_PCM_BYTES_PER_SAMPLE (AUDIO_PCM_BITS_PER_SAMPLE / 8)
#define AUDIO_PCM_FRAME_BYTES (AUDIO_PCM_SAMPLES_PER_FRAME * AUDIO_PCM_CHANNELS * AUDIO_PCM_BYTES_PER_SAMPLE)

#define AUDIO_PACKET_HEADER_BYTES 16
#define AUDIO_RB_ITEM_MAX_BYTES 1024

typedef struct {
    uint64_t timestamp_ms;
    uint32_t sequence_no;
    uint16_t payload_len;
    uint8_t duration_ms;
    uint8_t codec_id;
    uint8_t payload[AUDIO_RB_ITEM_MAX_BYTES - AUDIO_PACKET_HEADER_BYTES];
} audio_packet_t;

#define AUDIO_CODEC_ID_OPUS 1
#define AUDIO_CODEC_ID_PCM  0

// Set to 1 for direct Mic -> Speaker loopback during hardware bring-up.
// Keep 0 to validate full packaging flow (capture -> aec -> encoder -> transport).
#define AUDIO_LOCAL_LOOPBACK 0

// Set to 1 to force speaker test tone (ignores mic input in playback stage).
#define AUDIO_OUTPUT_TEST_TONE 0

// Mic input mode: 0 = I2S STD (WS/BCLK/SD), 1 = I2S PDM (CLK/DIN).
// Current board wiring is validated with I2S STD.
#define AUDIO_MIC_INPUT_PDM 0
