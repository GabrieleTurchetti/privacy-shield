idf_component_register(SRCS "web_api.c" "web_server.c"
                       INCLUDE_DIRS "include" "../../main/include"
                       REQUIRES esp_http_server mesh_core)
