idf_component_register(SRCS "audio_pipeline.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos esp_ringbuf audio_common audio_capture audio_aec audio_codec_opus audio_transport audio_playback)
