idf_component_register(SRCS "audio_aec.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos esp_ringbuf audio_common)
