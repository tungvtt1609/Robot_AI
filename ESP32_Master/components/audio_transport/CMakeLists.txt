idf_component_register(SRCS "audio_transport.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos esp_ringbuf audio_common audio_playback robot_connectivity)
