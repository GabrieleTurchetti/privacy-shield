idf_component_register(SRCS "esp_now_link.c" "node_discovery.c"
                       INCLUDE_DIRS "include" "../../main/include"
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash)
