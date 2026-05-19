#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t audio_opus_encoder_start(RingbufHandle_t in_rb, RingbufHandle_t out_rb);
esp_err_t audio_opus_decoder_start(RingbufHandle_t in_rb, RingbufHandle_t out_rb);

// Pause/resume encoder output (still drains input to avoid upstream backpressure).
// resume() also clears any buffered OGG bytes from the previous session and
// arms a fresh OGG stream (BOS) on the next encoded frame.
void audio_opus_encoder_pause(void);
void audio_opus_encoder_resume(void);
bool audio_opus_encoder_is_paused(void);

// Snapshot the OGG/Opus byte stream accumulated since the last resume()/pause()
// boundary. After this call the internal buffer is logically empty so the next
// resume() starts from byte 0. The returned pointer remains owned by the
// encoder and is valid until the next resume() (which may realloc / overwrite).
// Also logs per-session encode statistics (intended for end-of-turn flush).
//
// Returns ESP_OK with *len == 0 if no audio was captured.
esp_err_t audio_opus_encoder_take_session(const uint8_t **out_buf, size_t *out_len);

// Like take_session() but for periodic mid-session streaming flush:
//   - copies the new bytes appended since the previous take_*() into out_buf
//   - rewinds the internal write head to 0 (next encoded frame appends at start)
//   - does NOT log session stats (those are saved for the final take_session)
//
// Caller MUST consume out_buf synchronously before calling any take_* again,
// because the underlying storage is reused.
esp_err_t audio_opus_encoder_take_session_chunk(const uint8_t **out_buf, size_t *out_len);
