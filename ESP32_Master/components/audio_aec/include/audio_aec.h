#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

esp_err_t audio_aec_start(RingbufHandle_t in_rb, RingbufHandle_t out_rb);
