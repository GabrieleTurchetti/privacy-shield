#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "log_tags.h"
#include "mesh_core.h"
#include "esp_mac.h"

#define MESH_LOCK()    mesh_lock()
#define MESH_UNLOCK()  mesh_unlock()

static const char *TAG = LOG_TAG_DISCOVERY;

/* Forward-declare the internal mesh state (defined in esp_now_link.c).
 * We access it through the getter, but this helper operates directly for speed. */
extern mesh_state_t s_mesh;

/* -------------------------------------------------------------------------- */
/*  Neighbor management                                                       */
/* -------------------------------------------------------------------------- */

void mesh_discovery_heard(const uint8_t *mac, uint8_t node_id) {
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    MESH_LOCK();

    /* Look for an existing entry for this MAC */
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (s_mesh.neighbors[i].active &&
            memcmp(s_mesh.neighbors[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
            /* Existing neighbor — update last_heard */
            s_mesh.neighbors[i].last_heard_ms = now;
            s_mesh.neighbors[i].node_id = node_id; /* May have changed */
            MESH_UNLOCK();
            return;
        }
    }

    /* Not found — find an empty slot */
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (!s_mesh.neighbors[i].active) {
            memcpy(s_mesh.neighbors[i].mac, mac, ESP_NOW_ETH_ALEN);
            s_mesh.neighbors[i].node_id = node_id;
            s_mesh.neighbors[i].last_heard_ms = now;
            s_mesh.neighbors[i].active = true;

            ESP_LOGI(TAG, "New neighbor: node_id=%u, MAC=" MACSTR,
                     node_id, MAC2STR(mac));
            MESH_UNLOCK();
            return;
        }
    }

    /* Table full — replace the oldest entry */
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

    MESH_UNLOCK();
}

/* -------------------------------------------------------------------------- */
/*  Timeout management (call periodically, e.g., every second)                */
/* -------------------------------------------------------------------------- */

void mesh_discovery_prune(void) {
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    MESH_LOCK();
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (!s_mesh.neighbors[i].active) continue;

        if (now - s_mesh.neighbors[i].last_heard_ms > MESH_NEIGHBOR_TIMEOUT_MS) {
            ESP_LOGI(TAG, "Neighbor timed out: node_id=%u, MAC=" MACSTR,
                     s_mesh.neighbors[i].node_id,
                     MAC2STR(s_mesh.neighbors[i].mac));

            s_mesh.neighbors[i].active = false;
        }
    }
    MESH_UNLOCK();
}

/* -------------------------------------------------------------------------- */
/*  Count active neighbors                                                    */
/* -------------------------------------------------------------------------- */

int mesh_discovery_count(void) {
    int count = 0;
    MESH_LOCK();
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (s_mesh.neighbors[i].active) count++;
    }
    MESH_UNLOCK();
    return count;
}

/* -------------------------------------------------------------------------- */
/*  Lookup neighbor by MAC                                                    */
/* -------------------------------------------------------------------------- */

const mesh_neighbor_t *mesh_discovery_find_mac(const uint8_t *mac) {
    MESH_LOCK();
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (s_mesh.neighbors[i].active &&
            memcmp(s_mesh.neighbors[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
            const mesh_neighbor_t *result = &s_mesh.neighbors[i];
            MESH_UNLOCK();
            return result;
        }
    }
    MESH_UNLOCK();
    return NULL;
}

/* -------------------------------------------------------------------------- */
/*  TASKS                                                                     */
/* -------------------------------------------------------------------------- */

void hello_task(void *arg) {
	TickType_t last_wake = xTaskGetTickCount();
	while (1) {
		mesh_send_hello();
		vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MESH_HELLO_INTERVAL_MS));
	}
}


void prune_task(void *arg) {
    while (1) {
        mesh_discovery_prune();
        int count = mesh_discovery_count();
        if (count > 0) {
            ESP_LOGI(LOG_TAG_MESH_CORE, "%d neighbor(s) online", count);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}


void status_task(void *arg) {
    // Maybe I should check this
    status_task_params_t *params = (status_task_params_t *)arg;
    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        mesh_send_status(arg);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5000));
    }
}