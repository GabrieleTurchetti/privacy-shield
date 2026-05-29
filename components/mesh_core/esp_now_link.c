#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "log_tags.h"
#include "mesh_core.h"

static const char *TAG = LOG_TAG_MESH_CORE;

/* -------------------------------------------------------------------------- */
/*  Internal state                                                            */
/* -------------------------------------------------------------------------- */

mesh_state_t s_mesh = {0};
static mesh_recv_callback_t s_user_callback = NULL;

/* -------------------------------------------------------------------------- */
/*  ESP-NOW send callback                                                     */
/* -------------------------------------------------------------------------- */

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    /* Could log failures, but ESP-NOW is fire-and-forget by design.
     * Delivery confirmation is not guaranteed in broadcast mode. */
    if (status != ESP_NOW_SEND_SUCCESS) {
        // In v5.5+, the MAC address is stored inside the tx_info struct as des_addr
        ESP_LOGW(TAG, "Send to " MACSTR " failed", MAC2STR(tx_info->des_addr));
    }
}

/* -------------------------------------------------------------------------- */
/*  ESP-NOW receive callback                                                  */
/* -------------------------------------------------------------------------- */

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int len) {
    if (len < sizeof(mesh_header_t) || data == NULL) return;

    const mesh_header_t *hdr = (const mesh_header_t *)data;
    const uint8_t *src_mac = recv_info->src_addr;

    /* Update neighbor table: record last-heard time for this MAC */
    mesh_discovery_heard(src_mac, hdr->src_id);

    /* Fire user callback if registered */
    if (s_user_callback) {
        s_user_callback(src_mac, data, len);
    }
}

/* -------------------------------------------------------------------------- */
/*  WiFi initialization (station mode — required for ESP-NOW)                 */
/* -------------------------------------------------------------------------- */

static esp_err_t wifi_init(void) {
    /* NVS flash must be initialized for WiFi to store calibration data */
    esp_err_t ret = nvs_flash_init(); //Non-Volatible Storage
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize the underlying TCP/IP stack and default event loop */
    ESP_ERROR_CHECK(esp_netif_init()); //underlying networking stack (LwIP)
    ESP_ERROR_CHECK(esp_event_loop_create_default()); //system for passing messages
    /* ---------------- */

    /* Minimal WiFi init — we just need the radio, not an actual network */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); //Station mode
    ESP_ERROR_CHECK(esp_wifi_start()); //powers up the antenna

    /* Set a long-term PM policy — ESP-NOW needs the radio on.
     * WIFI_PS_MIN_MODEM keeps the modem awake. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t mesh_init(uint8_t node_id) {
    if (s_mesh.initialized) {
        ESP_LOGW(TAG, "Mesh already initialized");
        return ESP_OK;
    }

    /* 1. Bring up WiFi (needed for ESP-NOW radio) */
    ESP_ERROR_CHECK(wifi_init());

    /* 2. Initialize ESP-NOW */
    ESP_ERROR_CHECK(esp_now_init());

    /* 3. Register callbacks */
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    /* 4. Add broadcast peer (so we can send broadcast packets) */
    esp_now_peer_info_t broadcast_peer = {0};
    uint8_t broadcast_mac[] = MESH_BROADCAST_MAC;
    memcpy(broadcast_peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    broadcast_peer.channel = 0;     /* Use current channel */
    broadcast_peer.encrypt = false; /* No encryption for now */

    /* Adding broadcast peer may fail if already added — ignore */
    esp_now_add_peer(&broadcast_peer);

    /* 5. Get our own MAC */
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_mesh.my_mac));

    /* 6. Fill in our state */
    s_mesh.my_id = node_id;
    s_mesh.initialized = true;

    ESP_LOGI(TAG, "Mesh initialized — node_id=%u, MAC=" MACSTR,
             node_id, MAC2STR(s_mesh.my_mac));

    /* 7. Send initial HELLO to announce presence */
    mesh_send_hello();

    return ESP_OK;
}

esp_err_t mesh_send(const uint8_t *mac, const void *data, size_t len) {
    if (!s_mesh.initialized) return ESP_ERR_INVALID_STATE;
    if (len > MESH_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;

    return esp_now_send(mac, (const uint8_t *)data, len);
}

esp_err_t mesh_broadcast(const void *data, size_t len) {
    uint8_t broadcast_mac[] = MESH_BROADCAST_MAC;
    return mesh_send(broadcast_mac, data, len);
}

esp_err_t mesh_send_hello(void) {
    mesh_hello_pkt_t pkt = {0};
    pkt.header.type        = MESH_PKT_HELLO;
    pkt.header.src_id      = s_mesh.my_id;
    pkt.header.timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount());

    ESP_LOGD(TAG, "Sending HELLO (node %u)", s_mesh.my_id);
    return mesh_broadcast(&pkt, sizeof(pkt));
}

const mesh_state_t *mesh_get_state(void) {
    return &s_mesh;
}

void mesh_register_recv_callback(mesh_recv_callback_t cb) {
    s_user_callback = cb;
}
