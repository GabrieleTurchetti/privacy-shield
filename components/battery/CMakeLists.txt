idf_component_register(SRCS "battery.c"
                       INCLUDE_DIRS "include" "../../main/include"
                       REQUIRES esp_adc driver)