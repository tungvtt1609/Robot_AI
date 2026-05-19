#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/**
 * @param in_rb       Uplink: encoded packets from encoder → send to server
 * @param downlink_rb Downlink: packets from server → feed to decoder
 */
esp_err_t audio_transport_start(RingbufHandle_t in_rb, RingbufHandle_t downlink_rb);

/**
 * Reset downlink frame-alignment state. Call at the end of a TTS burst so
 * leftover bytes from the previous burst do not bleed into the next one.
 */
void audio_downlink_reset(void);
