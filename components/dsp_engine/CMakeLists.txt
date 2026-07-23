idf_component_register(SRCS "noise_gen.c" "volume.c" "afe.c" "aec_filter.c"
                       INCLUDE_DIRS "include" "../../main/include/"
                       REQUIRES driver esp-sr esp_timer audio_hal)
