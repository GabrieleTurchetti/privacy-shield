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

#define PENDING_MAX     64
#define ACK_TIMEOUT_MS  1000
#define RESULT_WINDOW   100
typedef struct { bool used, acked; uint8_t mac[ESP_NOW_ETH_ALEN]; uint32_t ts, sent_ms; } pending_ack_t;
static pending_ack_t s_pending[PENDING_MAX];
static uint8_t s_results[RESULT_WINDOW];
static int s_res_idx = 0, s_res_cnt = 0, s_res_sum = 0;

/* all of these assume the mesh mutex is already held */
void result_push(int acked) {
    if (s_res_cnt == RESULT_WINDOW) s_res_sum -= s_results[s_res_idx]; else s_res_cnt++;
    s_results[s_res_idx] = acked ? 1 : 0; s_res_sum += s_results[s_res_idx];
    s_res_idx = (s_res_idx + 1) % RESULT_WINDOW;
}


void pending_add(const uint8_t *mac, uint32_t ts, uint32_t now) {
    int slot = -1;
    for (int i = 0; i < PENDING_MAX; i++) if (!s_pending[i].used) { slot = i; break; }
    if (slot < 0) {                 /* full: evict oldest as its current outcome */
        uint32_t o = UINT32_MAX;
        for (int i = 0; i < PENDING_MAX; i++) if (s_pending[i].sent_ms < o) { o = s_pending[i].sent_ms; slot = i; }
        result_push(s_pending[slot].acked);
    }
    s_pending[slot] = (pending_ack_t){ .used = true, .acked = false, .ts = ts, .sent_ms = now };
    memcpy(s_pending[slot].mac, mac, ESP_NOW_ETH_ALEN);
}


void pending_match(const uint8_t *mac, uint32_t ts) {
    for (int i = 0; i < PENDING_MAX; i++)
        if (s_pending[i].used && !s_pending[i].acked && s_pending[i].ts == ts &&
            memcmp(s_pending[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) { s_pending[i].acked = true; return; }
}


void pending_reap(uint32_t now) {
    for (int i = 0; i < PENDING_MAX; i++) {
        if (!s_pending[i].used) continue;
        if (s_pending[i].acked)                         { result_push(1); s_pending[i].used = false; }
        else if (now - s_pending[i].sent_ms > ACK_TIMEOUT_MS) { result_push(0); s_pending[i].used = false; }
    }
}


float delivery_ratio_now(void) {
    if (s_res_cnt == 0) return 1.0f;
    float r = (float)s_res_sum / (float)s_res_cnt;
    ESP_LOGI(LOG_TAG_DISCOVERY, "Delivery ratio: %.2f (%d/%d)", r, s_res_sum, s_res_cnt);
    return r < 0.f ? 0.f : (r > 1.f ? 1.f : r);
}