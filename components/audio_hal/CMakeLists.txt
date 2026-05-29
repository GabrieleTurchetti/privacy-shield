idf_component_register(SRCS "i2s_mic.c" "max_amp.c"
                       INCLUDE_DIRS "include" "../../main/include"
                       REQUIRES driver esp_timer)
