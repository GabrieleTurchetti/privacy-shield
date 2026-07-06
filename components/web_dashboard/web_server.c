#include "web_dashboard.h"

#include <string.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_WEB;

/* -------------------------------------------------------------------------- */
/*  Forward declarations — REST handlers (defined in web_api.c)               */
/* -------------------------------------------------------------------------- */

esp_err_t api_nodes_get_handler(httpd_req_t *req);
esp_err_t api_node_mute_post_handler(httpd_req_t *req);
esp_err_t api_node_unmute_post_handler(httpd_req_t *req);
esp_err_t api_node_volume_post_handler(httpd_req_t *req);
esp_err_t api_global_mute_post_handler(httpd_req_t *req);
esp_err_t api_global_unmute_post_handler(httpd_req_t *req);
esp_err_t api_node_unlock_post_handler(httpd_req_t *req);
esp_err_t api_node_reboot_post_handler(httpd_req_t *req);
esp_err_t dashboard_get_handler(httpd_req_t *req);

/* -------------------------------------------------------------------------- */
/*  WiFi SoftAP configuration                                                 */
/* -------------------------------------------------------------------------- */

#define WIFI_AP_SSID      "PrivacyShield"
#define WIFI_AP_PASSWORD  ""              /* Open network — no password */
#define WIFI_AP_CHANNEL   1
#define WIFI_AP_MAX_CONN  4

esp_err_t wifi_ap_init(void) {
    ESP_LOGI(TAG, "Starting WiFi SoftAP...");

    /*
     * The default AP netif was already created by mesh_init's wifi_init()
     * when wifi_mode == WIFI_MODE_AP.  We just need to apply the AP config.
     */
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .password = WIFI_AP_PASSWORD,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    if (strlen(WIFI_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_LOGI(TAG, "WiFi SoftAP started — SSID: %s, channel: %d",
             WIFI_AP_SSID, WIFI_AP_CHANNEL);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  HTTP server                                                                */
/* -------------------------------------------------------------------------- */

static httpd_handle_t s_server = NULL;

esp_err_t web_server_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    config.stack_size = 10240;  /* Default 4096 overflows — dashboard HTML is ~4.5KB */
    config.uri_match_fn = httpd_uri_match_wildcard;


    ESP_LOGI(TAG, "Starting HTTP server on port %d...", config.server_port);

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
        return ESP_FAIL;
    }

    /* ── Dashboard page ── */
    httpd_uri_t dashboard_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = dashboard_get_handler,
        .user_ctx  = NULL,
    };
    httpd_register_uri_handler(s_server, &dashboard_uri);
    //Maybe we can split dashboard and rest api in two files
    /* ── REST API ── */
    httpd_uri_t nodes_get = {
        .uri       = "/api/nodes",
        .method    = HTTP_GET,
        .handler   = api_nodes_get_handler,
    };
    httpd_register_uri_handler(s_server, &nodes_get);

    httpd_uri_t node_mute = {
        .uri       = "/api/node/mute*",
        .method    = HTTP_POST,
        .handler   = api_node_mute_post_handler,
    };
    httpd_register_uri_handler(s_server, &node_mute);

    httpd_uri_t node_unmute = {
        .uri       = "/api/node/unmute",
        .method    = HTTP_POST,
        .handler   = api_node_unmute_post_handler,
    };
    httpd_register_uri_handler(s_server, &node_unmute);

    httpd_uri_t node_volume = {
        .uri       = "/api/node/volume*",
        .method    = HTTP_POST,
        .handler   = api_node_volume_post_handler,
    };
    httpd_register_uri_handler(s_server, &node_volume);

    httpd_uri_t global_mute = {
        .uri       = "/api/global/mute",
        .method    = HTTP_POST,
        .handler   = api_global_mute_post_handler,
    };
    httpd_register_uri_handler(s_server, &global_mute);

    httpd_uri_t global_unmute = {
        .uri       = "/api/global/unmute",
        .method    = HTTP_POST,
        .handler   = api_global_unmute_post_handler,
    };
    httpd_register_uri_handler(s_server, &global_unmute);

    httpd_uri_t node_unlock = {
        .uri       = "/api/node/unlock*",
        .method    = HTTP_POST,
        .handler   = api_node_unlock_post_handler,
    };
    httpd_register_uri_handler(s_server, &node_unlock);

    httpd_uri_t node_reboot = {
        .uri       = "/api/node/reboot*",
        .method    = HTTP_POST,
        .handler   = api_node_reboot_post_handler,
    };
    httpd_register_uri_handler(s_server, &node_reboot);

    ESP_LOGI(TAG, "HTTP server started — http://192.168.4.1");
    return ESP_OK;
}
