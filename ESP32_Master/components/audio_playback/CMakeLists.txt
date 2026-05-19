idf_component_register(SRCS "audio_playback.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos esp_ringbuf esp_driver_i2s audio_common)
