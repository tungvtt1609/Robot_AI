idf_component_register(SRCS "audio_capture.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos esp_timer esp_ringbuf esp_driver_i2s esp_driver_gpio audio_common)
