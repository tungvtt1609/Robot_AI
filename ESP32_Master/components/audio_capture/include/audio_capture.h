#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

esp_err_t audio_capture_start(RingbufHandle_t out_rb);

/**
 * Pause/resume the capture pipeline.
 * When paused the I2S RX channel is disabled and the task sleeps,
 * which stops the entire mic→aec→enc chain at the source so heap and CPU
 * are freed for downlink (TTS) reception.
 */
void audio_capture_set_paused(bool paused);
