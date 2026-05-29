#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "log_tags.h"
#include "mesh_core.h"
#include "esp_mac.h"

static const char *TAG = LOG_TAG_DISCOVERY;

/* Forward-declare the internal mesh state (defined in esp_now_link.c).
 * We access it through the getter, but this helper operates directly for speed. */
extern mesh_state_t s_mesh;

/* -------------------------------------------------------------------------- */
/*  Neighbor management                                                       */
/* -------------------------------------------------------------------------- */

void mesh_discovery_heard(const uint8_t *mac, uint8_t node_id) {
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    /* 1. Look for an existing entry for this MAC */
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (s_mesh.neighbors[i].active &&
            memcmp(s_mesh.neighbors[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
            /* Existing neighbor — update last_heard */
            s_mesh.neighbors[i].last_heard_ms = now;
            s_mesh.neighbors[i].node_id = node_id; /* May have changed */
            return;
        }
    }

    /* 2. Not found — find an empty slot */
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (!s_mesh.neighbors[i].active) {
            memcpy(s_mesh.neighbors[i].mac, mac, ESP_NOW_ETH_ALEN);
            s_mesh.neighbors[i].node_id = node_id;
            s_mesh.neighbors[i].last_heard_ms = now;
            s_mesh.neighbors[i].active = true;

            ESP_LOGI(TAG, "New neighbor: node_id=%u, MAC=" MACSTR,
                     node_id, MAC2STR(mac));
            return;
        }
    }

    /* 3. Table full — replace the oldest entry */
    uint32_t oldest = UINT32_MAX;
    int oldest_idx = 0;
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (s_mesh.neighbors[i].last_heard_ms < oldest) {
            oldest = s_mesh.neighbors[i].last_heard_ms;
            oldest_idx = i;
        }
    }

    ESP_LOGW(TAG, "Neighbor table full — replacing node %u",
             s_mesh.neighbors[oldest_idx].node_id);

    memcpy(s_mesh.neighbors[oldest_idx].mac, mac, ESP_NOW_ETH_ALEN);
    s_mesh.neighbors[oldest_idx].node_id = node_id;
    s_mesh.neighbors[oldest_idx].last_heard_ms = now;
    s_mesh.neighbors[oldest_idx].active = true;
}

/* -------------------------------------------------------------------------- */
/*  Timeout management (call periodically, e.g., every second)                */
/* -------------------------------------------------------------------------- */

void mesh_discovery_prune(void) {
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (!s_mesh.neighbors[i].active) continue;

        if (now - s_mesh.neighbors[i].last_heard_ms > MESH_NEIGHBOR_TIMEOUT_MS) {
            ESP_LOGI(TAG, "Neighbor timed out: node_id=%u, MAC=" MACSTR,
                     s_mesh.neighbors[i].node_id,
                     MAC2STR(s_mesh.neighbors[i].mac));

            s_mesh.neighbors[i].active = false;
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Count active neighbors                                                    */
/* -------------------------------------------------------------------------- */

int mesh_discovery_count(void) {
    int count = 0;
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (s_mesh.neighbors[i].active) count++;
    }
    return count;
}

/* -------------------------------------------------------------------------- */
/*  Lookup neighbor by MAC                                                    */
/* -------------------------------------------------------------------------- */

const mesh_neighbor_t *mesh_discovery_find_mac(const uint8_t *mac) {
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (s_mesh.neighbors[i].active &&
            memcmp(s_mesh.neighbors[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
            return &s_mesh.neighbors[i];
        }
    }
    return NULL;
}
