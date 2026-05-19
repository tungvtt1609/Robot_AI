idf_component_register(SRCS "audio_codec_opus.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos esp_ringbuf audio_common esp_timer
                       PRIV_REQUIRES espressif__esp_audio_codec)
