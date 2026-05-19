#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

esp_err_t audio_playback_start(RingbufHandle_t in_rb);

// Reset playback state to idle so the next incoming burst is prebuffered
// before the I2S valve opens again. Should be called at TTS burst end.
void audio_playback_reset_state(void);

// Direct write path (bypasses ringbuffer). Applies AGC + soft clip, expands
// mono -> stereo and writes to I2S TX. `mono_samples` is the number of
// 16-bit mono samples in `pcm`. Safe to call from any task.
esp_err_t audio_playback_write_pcm(const int16_t *pcm, size_t mono_samples);
