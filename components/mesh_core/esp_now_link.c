#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
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
#include "freertos/queue.h"
#include "delivery_ratio.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MESH_CORE;

/* ---- ACK worker (keeps esp_now_send out of the Wi-Fi recv callback) ---- */
typedef struct { uint8_t mac[ESP_NOW_ETH_ALEN]; uint32_t status_ts; } ack_req_t;
static QueueHandle_t s_ack_queue = NULL;

/* -------------------------------------------------------------------------- */
/*  Internal state                                                            */
/* -------------------------------------------------------------------------- */

mesh_state_t s_mesh = {0};
wifi_mode_t l_wifi_mode = WIFI_MODE_NULL;
static mesh_recv_callback_t s_user_callback = NULL;
static uint8_t node_id = 0;
static mesh_status_callback_t s_status_callback;
static volume_command_cb *s_volume_command_callback;
static SemaphoreHandle_t s_mesh_mutex = NULL;

/* Channel-keeper state (node only). s_channel_locked = we are currently parked
 * on the hub's channel; owned by the keeper task. s_last_hub_heard_ms = tick
 * (ms) of the most recent packet from the hub (src_id 0), written from the
 * Wi-Fi recv callback and read by the keeper to detect hub loss / find the
 * hub's channel. Both are single 32-bit accesses, so volatile is enough. */
static volatile bool s_channel_locked = false;
static volatile uint32_t s_last_hub_heard_ms = 0;

static void  ack_task(void *arg);


/* -------------------------------------------------------------------------- */
/* Packet received callback — handle incoming mesh packets,                   */
/*   called inside mesh_init                                                  */
/* -------------------------------------------------------------------------- */

static void on_mesh_packet(const uint8_t *src_mac, const void *data, size_t len) {
    const mesh_header_t *hdr = (const mesh_header_t *)data;

    switch (hdr->type) {
        case MESH_PKT_HELLO:
            if(len < sizeof(mesh_hello_pkt_t)) {
                ESP_LOGW(LOG_TAG_DISCOVERY, "Received HELLO packet too short (%zu bytes)", len);
                return;
            }
            ESP_LOGI(LOG_TAG_DISCOVERY, "HELLO from node %u (" MACSTR ")", hdr->src_id, MAC2STR(src_mac));
            break;

        case MESH_PKT_STATUS:
            if (len >= sizeof(mesh_status_pkt_t)) {
                const mesh_status_pkt_t *status = (const mesh_status_pkt_t *)data;
                ESP_LOGI(LOG_TAG_DISCOVERY, "STATUS from node %u: masking=%s vol=%u batt=%u%% cpu0=%u%% cpu1=%u%% heap_free=%u heap_largest_block=%u",
                         status->header.src_id, status->masking_active ? "ON" : "OFF",
                         status->volume, status->battery_pct, status->cpu0_utilization, status->cpu1_utilization, status->heap_free, status->heap_largest_block);
                if (s_status_callback) s_status_callback(status, src_mac);
                ack_req_t req = { .status_ts = status->header.timestamp_ms };
                memcpy(req.mac, src_mac, ESP_NOW_ETH_ALEN);
                if (s_ack_queue) xQueueSend(s_ack_queue, &req, 0);   /* non-blocking */
            }
            break;

        case MESH_PKT_COMMAND:
            //ESP_LOGI(LOG_TAG_DISCOVERY, "COMMAND packet received from node %u", hdr->src_id);
            if (len >= sizeof(mesh_command_pkt_t)) {
                const mesh_command_pkt_t *cmd = (const mesh_command_pkt_t *)data;
                ESP_LOGI(LOG_TAG_DISCOVERY, "COMMAND from node %u: cmd=%u val=%u", cmd->header.src_id,
                         cmd->command, cmd->value);
                if (s_volume_command_callback != NULL) {
                    switch (cmd->command) {
                    case MESH_CMD_MUTE:
                        s_volume_command_callback->set_masking(0);
                        break;
                    case MESH_CMD_UNMUTE:
                        s_volume_command_callback->set_masking(1);
                        break;
                    case MESH_CMD_SET_VOLUME:
                        s_volume_command_callback->set_volume_percentage(cmd->value);
                        break;
                    case MESH_CMD_REBOOT:
                        vTaskDelay(pdMS_TO_TICKS(100));  // let log flush
                        esp_restart();
                        break;
                    case MESH_CMD_UNLOCK:
                        s_volume_command_callback->unlock();
                        break;
                    default:
                        break;
                    }
                }
                
            }
            break;
        case MESH_PKT_ACK:
            ESP_LOGD(LOG_TAG_DISCOVERY, "ACK from node %u", hdr->src_id);
            if (len >= sizeof(mesh_ack_pkt_t)) {
                const mesh_ack_pkt_t *ack = (const mesh_ack_pkt_t *)data;
                mesh_lock(); 
                pending_match(src_mac, ack->ack_timestamp_ms); 
                mesh_unlock();
            }
            break;

        default:
            ESP_LOGD(LOG_TAG_DISCOVERY, "Unknown packet type 0x%02X from node %u", hdr->type, hdr->src_id);
            break;
    }
}


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

    /* Node channel keeper: timestamp every packet from the hub. The keeper task
     * uses this both to pick the hub's channel while scanning and to notice when
     * the hub goes silent. Harmless on the hub itself. */
    if (hdr->src_id == MESH_HUB_SRC_ID) {
        s_last_hub_heard_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    }

    /* Update neighbor table: record last-heard time for this MAC */
    mesh_discovery_heard(src_mac, hdr->src_id);

    /* Fire user callback if registered */
    if (s_user_callback) {
        s_user_callback(src_mac, data, len);
    }
}

/* -------------------------------------------------------------------------- */
/*  WiFi initialization (required for ESP-NOW)                                */
/* -------------------------------------------------------------------------- */

static esp_err_t wifi_init(wifi_mode_t wifi_mode) {
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

    /* WiFi init — ESP-NOW needs the radio, works with any WiFi mode */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (wifi_mode == WIFI_MODE_AP) {
        esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    } else {
        esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }

    ESP_ERROR_CHECK(esp_wifi_start()); //powers up the antenna

    /* Set a long-term PM policy — ESP-NOW needs the radio on.
     * WIFI_PS_MIN_MODEM keeps the modem awake. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t mesh_init(wifi_mode_t wifi_mode, mesh_status_callback_t status_cb, volume_command_cb *command_cb){
    if (s_mesh.initialized) {
        ESP_LOGW(TAG, "Mesh already initialized");
        return ESP_OK;
    }

    /* Bring up WiFi (needed for ESP-NOW radio) */
    ESP_ERROR_CHECK(wifi_init(wifi_mode));

    /* Initialize ESP-NOW */
    ESP_ERROR_CHECK(esp_now_init());

    /* Register callbacks */
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    /* Add broadcast peer (so we can send broadcast packets) */
    esp_now_peer_info_t broadcast_peer = {0};
    uint8_t broadcast_mac[] = MESH_BROADCAST_MAC;
    memcpy(broadcast_peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    broadcast_peer.channel = 0;     /* Use current channel */
    broadcast_peer.encrypt = false; /* No encryption for now */
    l_wifi_mode = wifi_mode;

    /* Adding broadcast peer may fail if already added — ignore */
    esp_now_add_peer(&broadcast_peer);

    /* Get our own MAC */
    ESP_ERROR_CHECK(esp_wifi_get_mac(
        wifi_mode == WIFI_MODE_AP ? WIFI_IF_AP : WIFI_IF_STA,
        s_mesh.my_mac));

    /* Fill in our state */
    // This way each node get its own id automatically
    node_id = get_node_id();
#ifdef CONFIG_PRIVACY_SHIELD_ROLE_HUB
    /* The hub is always node 0 (see mesh_header src_id convention). Nodes use
     * this to recognize the hub while channel-scanning. */
    node_id = MESH_HUB_SRC_ID;
#endif
    s_mesh.my_id = node_id;
    s_mesh.initialized = true;

    ESP_LOGI(TAG, "Mesh initialized — node_id=%u, MAC=" MACSTR,
             node_id, MAC2STR(s_mesh.my_mac));

    s_mesh_mutex = xSemaphoreCreateMutex();
    if (s_mesh_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mesh mutex");
        return ESP_FAIL;
    }
    
    // Create a queue for ACK requests and start the ACK task
    s_ack_queue = xQueueCreate(16, sizeof(ack_req_t));
    xTaskCreate(ack_task, "ack", 3072, NULL, 3, NULL);
    
    s_status_callback = status_cb;
    s_volume_command_callback = command_cb;

    //we also register callback during init
    mesh_register_recv_callback(on_mesh_packet);

    /* Send initial HELLO to announce presence */
    mesh_send_hello();

    return ESP_OK;
}

uint8_t get_node_id(void) {
    //use all 6 bytes of MAC to derive a node ID in the range 1–254
    uint8_t mac[6]; esp_efuse_mac_get_default(mac);
    uint32_t h = 2166136261u;                 
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return (uint8_t)(h % 254) + 1;            
}

bool mesh_channel_is_locked(void) {
    return s_channel_locked;
}

/* Channels to try, hub-favourites first (2.4 GHz routers usually pick 1/6/11). */
static const uint8_t k_scan_channels[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};

/* One full channel sweep. Returns true (and leaves the radio on that channel)
 * if the hub was heard while parked on one of the channels; false otherwise. */
static bool channel_scan_sweep(void) {
    const size_t n = sizeof(k_scan_channels) / sizeof(k_scan_channels[0]);
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = k_scan_channels[i];
        /* STA is not associated to an AP, so we're free to force the channel. */
        esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_channel(%u) failed: %s", ch, esp_err_to_name(err));
        }
        uint32_t t_enter = pdTICKS_TO_MS(xTaskGetTickCount());
        ESP_LOGD(TAG, "Scanning for hub on channel %u", ch);

        vTaskDelay(pdMS_TO_TICKS(MESH_CHANNEL_SCAN_DWELL_MS));

        /* Heard a hub packet at/after we tuned here? Then the hub is on this
         * channel. Signed diff keeps this correct across tick wrap. */
        if (s_last_hub_heard_ms != 0 &&
            (int32_t)(s_last_hub_heard_ms - t_enter) >= 0) {
            return true;
        }
    }
    return false;
}

void mesh_channel_scan_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Channel keeper started — hub src_id %u", MESH_HUB_SRC_ID);

    uint32_t backoff_ms = MESH_RESCAN_BACKOFF_MIN_MS;

    for (;;) {
        /* ---- SCANNING ---- */
        s_channel_locked = false;
        if (channel_scan_sweep()) {
            /* ---- LOCKED ---- */
            uint8_t ch = 0; wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
            esp_wifi_get_channel(&ch, &sec);
            s_channel_locked = true;
            backoff_ms = MESH_RESCAN_BACKOFF_MIN_MS;   /* reset on success */
            ESP_LOGI(TAG, "Locked to hub on channel %u", ch);

            /* Stay put; wake periodically to confirm the hub is still there. */
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(MESH_CHANNEL_MONITOR_INTERVAL_MS));
                uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
                if ((int32_t)(now - s_last_hub_heard_ms) > MESH_HUB_LOST_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "Hub silent for >%d ms — re-scanning",
                             MESH_HUB_LOST_TIMEOUT_MS);
                    break;   /* back to SCANNING */
                }
            }
        } else {
            /* ---- BACKOFF ---- : a full sweep found no hub. Stay usable on the
             * default channel so hub-less nodes converge */
            ESP_LOGW(TAG, "No hub found — parking on channel %d, retry in %u ms",
                     MESH_CHANNEL_DEFAULT, (unsigned)backoff_ms);
            esp_wifi_set_channel(MESH_CHANNEL_DEFAULT, WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms >= MESH_RESCAN_BACKOFF_MAX_MS / 2)
                             ? MESH_RESCAN_BACKOFF_MAX_MS
                             : backoff_ms * 2;
        }
    }
}

void mesh_lock(void) {
    if (s_mesh_mutex != NULL) {
        xSemaphoreTake(s_mesh_mutex, portMAX_DELAY);
    }
}

void mesh_unlock(void) {
    if (s_mesh_mutex != NULL) {
        xSemaphoreGive(s_mesh_mutex);
    }
}

bool is_broadcast(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

esp_err_t mesh_send(const uint8_t *mac, const void *data, size_t len) {
    //ESP_LOGI(TAG, "MESH INITALIZED: %d, LEN: %d", s_mesh.initialized, len);
    if (!s_mesh.initialized) return ESP_ERR_INVALID_STATE;
    if (len > MESH_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;

    //ESP_LOGI(TAG, "Sending packet to " MACSTR " (len=%zu)", MAC2STR(mac), len);
    if (!is_broadcast(mac) && !esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t p = {0}; 
        memcpy(p.peer_addr, mac, 6);
        p.channel = 0; 
        p.encrypt = false;
        p.ifidx   = (l_wifi_mode == WIFI_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA;
        esp_now_add_peer(&p);
    }

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


esp_err_t mesh_send_ack(const uint8_t *mac, uint32_t status_ts) {
    mesh_ack_pkt_t pkt = {0};
    pkt.header.type = MESH_PKT_ACK;
    pkt.header.src_id = get_node_id();
    pkt.header.timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    pkt.ack_timestamp_ms = status_ts;          /* the STATUS we're acking */
    return mesh_send(mac, &pkt, sizeof(pkt));
}

esp_err_t mesh_send_status(void *arg){
    status_task_params_t *params = (status_task_params_t *)arg;
    mesh_status_pkt_t status = {0};
    status.header.type = MESH_PKT_STATUS;
    status.header.src_id = params->node_id;
    uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    status.header.timestamp_ms = now_ms;
    status.masking_active = params->is_speech() /* read from VAD state */;
    status.volume = params->get_volume_percentage() /* read from current volume */;
    status.battery_pct = params->get_battery();  // placeholder, real sensor later
    status.uptime_s = now_ms / 1000;
    params->update_system_metrics();
    status.cpu0_utilization = params->get_cpu0_utilization();
    status.cpu1_utilization = params->get_cpu1_utilization();
    status.heap_free = params->get_heap_free();
    status.heap_largest_block = params->get_heap_largest_block();
    
    //DELIVERY RATIO CALCULATION
    mesh_lock();
    pending_reap(now_ms);
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++)          /* read table directly – we hold the lock */
        if (s_mesh.neighbors[i].active) pending_add(s_mesh.neighbors[i].mac, now_ms, now_ms);
    status.delivery_ratio = delivery_ratio_now();
    status.packet_loss_rate = 1.0f - status.delivery_ratio;
    mesh_unlock();
    return mesh_broadcast(&status, sizeof(status));
}

const mesh_state_t *mesh_get_state(void) {
    return &s_mesh;
}

void mesh_register_recv_callback(mesh_recv_callback_t cb) {
    s_user_callback = cb;
}

static void ack_task(void *arg) {
    ack_req_t req;
    for (;;)
        if (xQueueReceive(s_ack_queue, &req, portMAX_DELAY) == pdTRUE)
            mesh_send_ack(req.mac, req.status_ts);
}
