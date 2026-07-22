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
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "sdkconfig.h"
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
/*  WiFi Station — Hub joins the home network so the dashboard lives on the    */
/*  LAN (http://privacyshield.local) and clients keep their internet.          */
/* -------------------------------------------------------------------------- */

#define WIFI_STA_CONNECTED_BIT   BIT0
#define WIFI_STA_GOT_IP_TIMEOUT  pdMS_TO_TICKS(15000)

static EventGroupHandle_t s_wifi_event_group = NULL;

static void sta_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_event_group) xEventGroupClearBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi disconnected — reconnecting...");
        esp_wifi_connect();  /* keep trying so the hub survives router reboots */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_connect(void) {
    /* The STA netif + esp_wifi_start() were already done by mesh_init's
     * wifi_init() (WIFI_MODE_STA). Here we just add the SSID and connect. */
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_PRIVACY_SHIELD_WIFI_SSID,
            .password = CONFIG_PRIVACY_SHIELD_WIFI_PASSWORD,
            /* For an open network, change this to WIFI_AUTH_OPEN. */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s ...", CONFIG_PRIVACY_SHIELD_WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_connect());

    /* Wait (best-effort) for an IP so the startup banner is accurate. If it
     * times out we still return — the disconnect handler keeps retrying. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, WIFI_STA_GOT_IP_TIMEOUT);
    if (!(bits & WIFI_STA_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "No IP yet — will keep retrying in the background");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* Advertise the dashboard as http://privacyshield.local via mDNS. */
esp_err_t dashboard_mdns_init(void) {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("privacyshield"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Privacy Shield Hub"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mDNS ready — http://privacyshield.local");
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
        .uri       = "/api/node/unmute*",
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

    ESP_LOGI(TAG, "HTTP server started — http://privacyshield.local");
    return ESP_OK;
}
