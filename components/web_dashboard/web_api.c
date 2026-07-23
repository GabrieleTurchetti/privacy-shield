#include "web_dashboard.h"

#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
//#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "log_tags.h"
#include "mesh_core.h"

static const char *TAG = LOG_TAG_WEB;

/* -------------------------------------------------------------------------- */
/*  Node status cache                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  mac[ESP_NOW_ETH_ALEN];
    uint8_t  node_id;
    bool     masking_active;
    uint8_t  volume;
    uint8_t  battery_pct;
    float delivery_ratio;
    float packet_loss_rate;
    uint32_t uptime_s;
    uint32_t last_update_ms;
    uint8_t  cpu0;
    uint8_t  cpu1;
    uint32_t heap_free;
    uint32_t heap_largest_block;
    delay_metrics_t delays;   /* e2e / attack / release min-max-avg (ms) */
    bool     active;
} node_status_cache_t;

/* Pull the delay KPIs out of a STATUS packet into a plain struct. */
static delay_metrics_t status_delays(const mesh_status_pkt_t *s) {
    delay_metrics_t d = {
        .e2e_avg = s->e2e_avg, .e2e_min = s->e2e_min, .e2e_max = s->e2e_max,
        .attack_avg = s->attack_avg, .attack_min = s->attack_min, .attack_max = s->attack_max,
        .release_avg = s->release_avg, .release_min = s->release_min, .release_max = s->release_max };
    return d;
}

#define MAX_CACHED_NODES 16

static node_status_cache_t s_node_cache[MAX_CACHED_NODES];
static SemaphoreHandle_t s_cache_mutex = NULL;

void web_dashboard_init(void) {
    if (s_cache_mutex == NULL) {
        s_cache_mutex = xSemaphoreCreateMutex();
    }
}

void web_dashboard_update_status(const mesh_status_pkt_t *status,
                                 const uint8_t *mac) {
    if (s_cache_mutex == NULL || status == NULL || mac == NULL) {
        return;
    }

    uint8_t node_id = status->header.src_id;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_CACHED_NODES; i++) {
        if (s_node_cache[i].active && s_node_cache[i].node_id == node_id) {
            s_node_cache[i].masking_active = status->masking_active;
            s_node_cache[i].volume        = status->volume;
            s_node_cache[i].battery_pct   = status->battery_pct;
            s_node_cache[i].delivery_ratio = status->delivery_ratio;
            s_node_cache[i].packet_loss_rate = status->packet_loss_rate;
            s_node_cache[i].uptime_s      = status->uptime_s;
            s_node_cache[i].cpu0 = status->cpu0_utilization;
            s_node_cache[i].cpu1 = status->cpu1_utilization;
            s_node_cache[i].heap_free = status->heap_free;
            s_node_cache[i].heap_largest_block = status->heap_largest_block;
            s_node_cache[i].delays = status_delays(status);
            s_node_cache[i].last_update_ms = now;
            xSemaphoreGive(s_cache_mutex);
            return;
        }
    }

    for (int i = 0; i < MAX_CACHED_NODES; i++) {
        if (!s_node_cache[i].active) {
            memcpy(s_node_cache[i].mac, mac, ESP_NOW_ETH_ALEN);
            s_node_cache[i].node_id       = node_id;
            s_node_cache[i].masking_active = status->masking_active;
            s_node_cache[i].volume        = status->volume;
            s_node_cache[i].battery_pct   = status->battery_pct;
            s_node_cache[i].delivery_ratio = status->delivery_ratio;
            s_node_cache[i].packet_loss_rate = status->packet_loss_rate;
            s_node_cache[i].uptime_s      = status->uptime_s;
            s_node_cache[i].cpu0 = status->cpu0_utilization;
            s_node_cache[i].cpu1 = status->cpu1_utilization;
            s_node_cache[i].heap_free = status->heap_free;
            s_node_cache[i].heap_largest_block = status->heap_largest_block;
            s_node_cache[i].delays = status_delays(status);
            s_node_cache[i].last_update_ms = now;
            s_node_cache[i].active        = true;
            xSemaphoreGive(s_cache_mutex);
            return;
        }
    }
    xSemaphoreGive(s_cache_mutex);
}

/* -------------------------------------------------------------------------- */
/*  JSON helpers                                                               */
/* -------------------------------------------------------------------------- */

// Returns false if the text didn't fit (caller should stop appending).
static bool json_appendf(char *buf, size_t buf_size, int *offset,
                         const char *fmt, ...) {
    if (*offset < 0 || (size_t)*offset >= buf_size) return false;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *offset, buf_size - *offset, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= buf_size - *offset) return false;  // truncated
    *offset += n;
    return true;
}

static void json_append_nodes(char *buf, size_t buf_size) {
    if (s_cache_mutex == NULL) {
        snprintf(buf, buf_size, "[]");
        return;
    }

    /* Take locks in fixed order to avoid deadlock: cache first, then mesh */
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    mesh_lock();

    const mesh_state_t *mesh = mesh_get_state();
    bool first = true;
    int offset = 0;

    json_appendf(buf, buf_size, &offset, "[");

    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (!mesh->neighbors[i].active) continue;

        uint8_t nid = mesh->neighbors[i].node_id;
        //These two ifs are required to avoid showing the hub itself in the list of nodes
        if (memcmp(mesh->neighbors[i].mac, mesh->my_mac, ESP_NOW_ETH_ALEN) == 0)
            continue; /* skip self (hub) */
        if (nid == 0) continue; /* Skip hub itself */

        /* Pull cached status for this node */
        bool mask = false;
        uint8_t vol = 0, batt = 0;
        uint32_t uptime = 0;
        bool has_status = false;
        uint8_t cpu0 = 0, cpu1 = 0;
        uint32_t heap_free = 0, heap_largest_block = 0;
        float delivery_ratio = 0.0f, packet_loss_rate = 0.0f;
        delay_metrics_t delays = {0};

        for (int j = 0; j < MAX_CACHED_NODES; j++) {
            if (s_node_cache[j].active &&
                s_node_cache[j].node_id == nid) {
                mask  = s_node_cache[j].masking_active;
                vol   = s_node_cache[j].volume;
                batt  = s_node_cache[j].battery_pct;
                delivery_ratio = s_node_cache[j].delivery_ratio;
                packet_loss_rate = s_node_cache[j].packet_loss_rate;
                uptime = s_node_cache[j].uptime_s;
                cpu0 = s_node_cache[j].cpu0;
                cpu1 = s_node_cache[j].cpu1;
                heap_free = s_node_cache[j].heap_free;
                heap_largest_block = s_node_cache[j].heap_largest_block;
                delays = s_node_cache[j].delays;

                has_status = true;
                break;
            }
        }

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR,
                 MAC2STR(mesh->neighbors[i].mac));

        if (!first && !json_appendf(buf, buf_size, &offset, ",")) break;
        first = false;
        if (!json_appendf(buf, buf_size, &offset, "{"
            "\"node_id\":%u,"
            "\"mac\":\"%s\","
            "\"online\":true,"
            "\"masking_active\":%s,"
            "\"volume\":%u,"
            "\"battery_pct\":%u,"
            "\"delivery_ratio\":%.2f,"
            "\"packet_loss_rate\":%.2f,"
            "\"cpu0\":%u,"
            "\"cpu1\":%u,"
            "\"heap_free\":%lu,"
            "\"heap_largest_block\":%lu,"
            "\"uptime_s\":%lu,"
            "\"e2e_avg\":%.1f,\"e2e_min\":%.1f,\"e2e_max\":%.1f,"
            "\"attack_avg\":%.1f,\"attack_min\":%d,\"attack_max\":%d,"
            "\"release_avg\":%.1f,\"release_min\":%d,\"release_max\":%d"
            "}", nid, mac_str,
            has_status ? (mask ? "true" : "false") : "false",
            has_status ? vol : 0,
            has_status ? batt : 0,
            has_status ? delivery_ratio : 0.0f,
            has_status ? packet_loss_rate : 0.0f,
            has_status ? cpu0 : 0,                                 
            has_status ? cpu1 : 0,                                
            (unsigned long)(has_status ? heap_free : 0),         
            (unsigned long)(has_status ? heap_largest_block : 0),
            (unsigned long)(has_status ? uptime : 0),
            has_status ? delays.e2e_avg : 0.0f,
            has_status ? delays.e2e_min : 0.0f,
            has_status ? delays.e2e_max : 0.0f,
            has_status ? delays.attack_avg : 0.0f,
            has_status ? (int)delays.attack_min : 0,
            has_status ? (int)delays.attack_max : 0,
            has_status ? delays.release_avg : 0.0f,
            has_status ? (int)delays.release_min : 0,
            has_status ? (int)delays.release_max : 0))
            break;
    }

    json_appendf(buf, buf_size, &offset, "]");

    mesh_unlock();
    xSemaphoreGive(s_cache_mutex);
}

/* -------------------------------------------------------------------------- */
/*  Mesh helpers                                                               */
/* -------------------------------------------------------------------------- */

static bool node_mac_by_id(uint8_t node_id, uint8_t *out_mac) {
    mesh_lock();
    const mesh_state_t *mesh = mesh_get_state();
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (mesh->neighbors[i].active &&
            mesh->neighbors[i].node_id == node_id) {
            memcpy(out_mac, mesh->neighbors[i].mac, ESP_NOW_ETH_ALEN);
            mesh_unlock();
            return true;
        }
    }
    mesh_unlock();
    return false;
}

static void send_command_to_node(uint8_t node_id, mesh_command_t cmd,
                                 uint8_t value) {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    if (!node_mac_by_id(node_id, mac)) {
        ESP_LOGW(TAG, "Cannot send command — node %u not found", node_id);
        return;
    }

    mesh_command_pkt_t pkt = {0};
    pkt.header.type        = MESH_PKT_COMMAND;
    pkt.header.src_id      = 0; /* Hub */
    pkt.header.timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    pkt.command = cmd;
    pkt.value   = value;

    //TODO: Check Error in Mesh Send
    esp_err_t err =  mesh_send(mac, &pkt, sizeof(pkt));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send command %u to node %u: %s",
                 cmd, node_id, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Sent command %u (value=%u) to node %u", cmd, value, node_id);
}

static void send_command_to_all(mesh_command_t cmd, uint8_t value) {
    const mesh_state_t *mesh = mesh_get_state();
    for (int i = 0; i < MESH_MAX_NEIGHBORS; i++) {
        if (mesh->neighbors[i].active && mesh->neighbors[i].node_id != 0) {
            send_command_to_node(mesh->neighbors[i].node_id, cmd, value);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  REST Handlers                                                              */
/* -------------------------------------------------------------------------- */

esp_err_t api_nodes_get_handler(httpd_req_t *req) {
    /* static (not stack): the extra delay fields push a full 16-node payload
     * past the task stack budget. The HTTP server handles one request at a
     * time, so a single shared buffer is safe here. */
    static char buf[8192];
    json_append_nodes(buf, sizeof(buf));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t api_node_mute_post_handler(httpd_req_t *req) {
    int node_id = 0;
    char id_str[8] = {0};
    char query[32] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK
    ) {
        node_id = atoi(id_str);
    }

    if (node_id < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node ID");
        return ESP_FAIL;
    }

    send_command_to_node(node_id, MESH_CMD_MUTE, 1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t api_node_unmute_post_handler(httpd_req_t *req) {
    int node_id = 0;
    char id_str[8] = {0};
    char query[32] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK
    ) {
        node_id = atoi(id_str);
    }

    if (node_id < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node ID");
        return ESP_FAIL;
    }

    send_command_to_node(node_id, MESH_CMD_UNMUTE, 1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t api_node_volume_post_handler(httpd_req_t *req) {
    /* Read query string: ?level=50 */
    char query[32] = {0};
    char id_str[8] = {0};
    char level_str[8] = {0};
    int node_id = 0;
    int level = 50; /* default */
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK &&
        httpd_query_key_value(query, "level", level_str, sizeof(level_str)) == ESP_OK
    )  {
        node_id = atoi(id_str);
        level = atoi(level_str);
        if (level < 0) level = 0;
        if (level > 100) level = 100;
    }

    if (node_id < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node ID");
        return ESP_FAIL;
    }

    send_command_to_node(node_id, MESH_CMD_SET_VOLUME, (uint8_t)level);
    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"level\":%d}", level);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

esp_err_t api_global_mute_post_handler(httpd_req_t *req) {
    send_command_to_all(MESH_CMD_MUTE, 1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t api_global_unmute_post_handler(httpd_req_t *req) {
    send_command_to_all(MESH_CMD_UNMUTE, 1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* Parse ?id=N from the query string; returns -1 if missing/invalid. */
static int query_node_id(httpd_req_t *req) {
    char query[32] = {0}, id_str[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK) {
        int id = atoi(id_str);
        if (id >= 1 && id <= 254) return id;
    }
    return -1;
}

esp_err_t api_node_unlock_post_handler(httpd_req_t *req) {
    int node_id = query_node_id(req);
    if (node_id < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node ID");
        return ESP_FAIL;
    }
    send_command_to_node(node_id, MESH_CMD_UNLOCK, 0); /* value unused */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}


esp_err_t api_node_reboot_post_handler(httpd_req_t *req) {
    int node_id = query_node_id(req);
    if (node_id < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node ID");
        return ESP_FAIL;
    }
    send_command_to_node(node_id, MESH_CMD_REBOOT, 0); /* value unused */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Dashboard HTML (minimal, responsive, auto-refreshing)                     */
/* -------------------------------------------------------------------------- */

/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */
static const char *DASHBOARD_HTML =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><meta name=\"color-scheme\" content=\"dark light\"><title>"
"Privacy Shield</title><script>(function(){try{var t=localStorage.getItem(\"ps-theme\");if(t!==\"light\"&&t!==\"dark\")t=(window.matchMedia&&matchMedia(\"(prefers-color-scheme: light)\").ma"
"tches)?\"light\":\"dark\";document.documentElement.dataset.theme=t;}catch(e){}})();</script><style>:root, [data-theme=\"dark\"]{\n  --bg:#0a0e1a; --bg2:#0f1526; --panel:#121a2e; --panel2:"
"#16203a;\n  --line:#243049; --line2:#2f3d5c;\n  --text:#e8edf7; --muted:#8b98b5; --faint:#5f6c8a;\n  --accent:#6366f1; --accent2:#a855f7;\n  --good:#34d399; --warn:#fbbf24; --bad:#f871"
"71;\n  --body-grad1:rgba(99,102,241,.18); --body-grad2:rgba(168,85,247,.14);\n  --topbar-bg:rgba(10,14,26,.72);\n  --card-shadow:0 8px 24px rgba(0,0,0,.28);\n  --tile-accent1:#a5b4fc; "
"--tile-accent2:#e9d5ff;\n  --pill-on-bg:rgba(52,211,153,.14); --pill-on-fg:#6ee7b7; --pill-on-bd:rgba(52,211,153,.3);\n  --mute-fg:#fca5a5; --mute-bd:#5b2330; --mute-hover:#3a1620;\n "
" --unmute-fg:#6ee7b7; --unmute-bd:#1f4a3a; --unmute-hover:#123027;\n  --r:16px; --r-sm:10px;\n}\n[data-theme=\"light\"]{\n  --bg:#eef2f8; --bg2:#e2e8f2; --panel:#ffffff; --panel2:#f7f9fc"
";\n  --line:#e2e8f0; --line2:#cbd5e1;\n  --text:#1e293b; --muted:#5b6b86; --faint:#94a3b8;\n  --accent:#6366f1; --accent2:#a855f7;\n  --good:#059669; --warn:#d97706; --bad:#dc2626;\n  -"
"-body-grad1:rgba(99,102,241,.12); --body-grad2:rgba(168,85,247,.10);\n  --topbar-bg:rgba(255,255,255,.75);\n  --card-shadow:0 8px 22px rgba(15,23,42,.08);\n  --tile-accent1:#6366f1; -"
"-tile-accent2:#a855f7;\n  --pill-on-bg:rgba(5,150,105,.12); --pill-on-fg:#047857; --pill-on-bd:rgba(5,150,105,.32);\n  --mute-fg:#dc2626; --mute-bd:#fecaca; --mute-hover:#fef2f2;\n  -"
"-unmute-fg:#059669; --unmute-bd:#bbf7d0; --unmute-hover:#f0fdf4;\n}\n*{margin:0;padding:0;box-sizing:border-box}\nhtml{-webkit-text-size-adjust:100%}\nbody{\n  font-family:-apple-system"
",BlinkMacSystemFont,\"Segoe UI\",Roboto,Helvetica,Arial,sans-serif;\n  color:var(--text); min-height:100vh;\n  background:\n    radial-gradient(1100px 600px at 12% -8%, var(--body-grad1"
"), transparent 60%),\n    radial-gradient(900px 500px at 100% 0%, var(--body-grad2), transparent 55%),\n    var(--bg);\n  -webkit-font-smoothing:antialiased;\n  transition:background ."
"3s ease, color .3s ease;\n}\n.metric-value,.tile-value,.slider-val{font-variant-numeric:tabular-nums}\nbutton{font-family:inherit}\n\n/* Topbar */\n.topbar{\n  position:sticky; top:0; z-i"
"ndex:10;\n  display:flex; align-items:center; justify-content:space-between;\n  padding:14px 22px;\n  background:var(--topbar-bg); backdrop-filter:blur(12px);\n  border-bottom:1px soli"
"d var(--line);\n}\n.brand{display:flex; align-items:center; gap:12px}\n.logo{\n  width:40px; height:40px; border-radius:12px;\n  display:grid; place-items:center; font-weight:800; font-"
"size:15px; letter-spacing:.5px;\n  color:#fff; background:linear-gradient(135deg,var(--accent),var(--accent2));\n  box-shadow:0 6px 20px rgba(99,102,241,.45);\n}\n.brand-title{font-siz"
"e:16px; font-weight:700; letter-spacing:.2px}\n.brand-sub{font-size:12px; color:var(--muted)}\n.topbar-right{display:flex; align-items:center; gap:10px}\n.conn{display:flex; align-ite"
"ms:center; gap:7px; font-size:12.5px; font-weight:600; padding:6px 12px; border-radius:999px; border:1px solid var(--line2)}\n.conn-dot{width:8px; height:8px; border-radius:50%}\n.co"
"nn-ok{color:var(--good)} .conn-ok .conn-dot{background:var(--good); box-shadow:0 0 0 3px rgba(52,211,153,.18); animation:pulse 2s infinite}\n.conn-bad{color:var(--bad)} .conn-bad .c"
"onn-dot{background:var(--bad)}\n.theme-btn{\n  width:36px; height:36px; display:grid; place-items:center; cursor:pointer;\n  border-radius:10px; border:1px solid var(--line2); backgro"
"und:var(--panel2); color:var(--text);\n  transition:background .18s ease, transform .12s ease;\n}\n.theme-btn:hover{background:var(--line); transform:translateY(-1px)}\n.theme-btn:focu"
"s-visible{outline:2px solid var(--accent); outline-offset:2px}\n.theme-btn svg{width:18px; height:18px}\n@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}\n\n/* Layout */\n.wrap{max-"
"width:1080px; margin:0 auto; padding:24px 22px 40px}\n.tiles{display:grid; grid-template-columns:repeat(3,1fr); gap:14px; margin-bottom:20px}\n.tile{background:linear-gradient(180deg"
",var(--panel2),var(--panel)); border:1px solid var(--line); border-radius:var(--r); padding:16px 18px}\n.tile-value{font-size:26px; font-weight:750; line-height:1}\n.tile-value.accen"
"t{background:linear-gradient(135deg,var(--tile-accent1),var(--tile-accent2)); -webkit-background-clip:text; background-clip:text; -webkit-text-fill-color:transparent}\n.tile-label{m"
"argin-top:6px; font-size:12px; color:var(--muted)}\n\n/* Toolbar */\n.toolbar{display:flex; align-items:center; gap:10px; margin-bottom:20px; flex-wrap:wrap}\n.toolbar-spacer{flex:1}\n."
"refresh-note{font-size:12px; color:var(--faint)}\n.btn{\n  border:1px solid transparent; border-radius:var(--r-sm); cursor:pointer;\n  padding:9px 16px; font-size:13.5px; font-weight:"
"600; color:#fff;\n  transition:transform .12s ease, background .18s ease, box-shadow .18s ease;\n}\n.btn:hover{transform:translateY(-1px)}\n.btn:active{transform:translateY(0)}\n.btn:fo"
"cus-visible{outline:2px solid var(--accent); outline-offset:2px}\n.btn-danger{background:linear-gradient(135deg,#e11d48,#9f1239); box-shadow:0 6px 16px rgba(225,29,72,.25)}\n.btn-suc"
"cess{background:linear-gradient(135deg,#059669,#065f46); box-shadow:0 6px 16px rgba(5,150,105,.22)}\n.btn-ghost{background:var(--panel2); color:var(--text); border-color:var(--line2"
")}\n.btn-ghost:hover{background:var(--line)}\n.btn-mute{background:var(--panel2); color:var(--mute-fg); border-color:var(--mute-bd); flex:1}\n.btn-mute:hover{background:var(--mute-hov"
"er)}\n.btn-unmute{background:var(--panel2); color:var(--unmute-fg); border-color:var(--unmute-bd); flex:1}\n.btn-unmute:hover{background:var(--unmute-hover)}\n.btn-reset{background:va"
"r(--panel2); color:var(--accent); border-color:var(--line2); flex:1}\n.btn-reset:hover{background:var(--line)}\n.btn-reboot{background:var(--panel2); color:#e0821a; border-color:rgba"
"(224,130,26,.4); flex:1}\n.btn-reboot:hover{background:rgba(224,130,26,.12)}\n\n/* Banner */\n.banner{background:rgba(220,38,38,.14); border:1px solid rgba(220,38,38,.4); color:var(--b"
"ad); padding:11px 15px; border-radius:var(--r-sm); font-size:13px; margin-bottom:18px}\n\n/* Grid + cards */\n.grid{display:grid; gap:16px; grid-template-columns:repeat(auto-fill,minm"
"ax(300px,1fr)); grid-auto-flow:row dense}\n.card{\n  background:linear-gradient(180deg,var(--panel2),var(--panel));\n  border:1px solid var(--line); border-radius:var(--r); padding:18"
"px;\n  box-shadow:var(--card-shadow); cursor:pointer;\n  transition:border-color .2s ease, transform .2s ease, box-shadow .2s ease;\n}\n.card:hover{border-color:var(--line2); transform"
":translateY(-2px)}\n.card.expanded{grid-column:span 3; cursor:default; border-color:var(--line2); box-shadow:0 12px 34px rgba(0,0,0,.32)}\n.head-right{display:flex; align-items:cente"
"r; gap:10px}\n.chev{color:var(--faint); display:grid; place-items:center; transition:transform .2s ease}\n.chev svg{width:18px; height:18px}\n.chev.open{transform:rotate(180deg); colo"
"r:var(--muted)}\n/* Three equal columns: controls + two chart columns (matching gaps \342\206\222 equal widths). */\n.expanded-body{display:grid; grid-template-columns:repeat(3,1fr); gap:16px"
"; align-items:start; margin-top:4px; animation:expandIn .4s ease both}\n.col-left{display:flex; flex-direction:column; min-width:0}\n.col-right{grid-column:span 2; display:grid; grid"
"-template-columns:1fr 1fr; gap:14px 16px; min-width:0; align-content:start}\n.col-right .chart{margin-top:0; padding-top:0; border-top:none}\n@media (max-width:720px){ .expanded-body"
"{grid-template-columns:1fr} }\n@keyframes expandIn{from{opacity:0; transform:translateY(-6px)}to{opacity:1; transform:none}}\n.card-head{display:flex; align-items:center; justify-con"
"tent:space-between; margin-bottom:2px}\n.card-title{display:flex; align-items:center; gap:9px; font-size:15.5px; font-weight:700}\n.node-dot{width:9px; height:9px; border-radius:50%;"
" background:linear-gradient(135deg,var(--accent),var(--accent2))}\n.mac{font-size:11.5px; color:var(--faint); font-family:ui-monospace,SFMono-Regular,Menlo,monospace; margin-bottom:"
"14px}\n\n.pill{display:inline-flex; align-items:center; gap:6px; padding:4px 10px; border-radius:999px; font-size:11px; font-weight:700; letter-spacing:.3px; text-transform:uppercase"
"}\n.pill-dot{width:7px; height:7px; border-radius:50%}\n.pill-on{background:var(--pill-on-bg); color:var(--pill-on-fg); border:1px solid var(--pill-on-bd)}\n.pill-on .pill-dot{backgro"
"und:var(--good); box-shadow:0 0 0 3px rgba(52,211,153,.2); animation:pulse 1.8s infinite}\n.pill-off{background:rgba(139,152,181,.1); color:var(--muted); border:1px solid var(--line"
"2)}\n.pill-off .pill-dot{background:var(--faint)}\n\n.metrics{display:grid; grid-template-columns:1fr 1fr; gap:10px 14px; margin-bottom:14px}\n.metric{display:flex; flex-direction:colu"
"mn; gap:2px}\n.metric-label{font-size:11px; color:var(--muted)}\n.metric-value{font-size:17px; font-weight:700}\n\n.submetric{margin-bottom:12px}\n.submetric-head{display:flex; justify-"
"content:space-between; font-size:11px; color:var(--muted); margin-bottom:5px}\n.bar{height:6px; border-radius:999px; background:var(--bg2); overflow:hidden}\n.bar-fill{height:100%; b"
"order-radius:999px; transition:width .4s ease}\n.tone-good{background:linear-gradient(90deg,#10b981,#34d399)}\n.tone-warn{background:linear-gradient(90deg,#f59e0b,#fbbf24)}\n.tone-bad"
"{background:linear-gradient(90deg,#e11d48,#f87171)}\n.tone-mem{background:linear-gradient(90deg,#6366f1,#a855f7)}\n\n.chart{margin-top:12px; padding-top:12px; border-top:1px solid var"
"(--line)}\n.chart-head{display:flex; justify-content:space-between; align-items:center; font-size:11px; color:var(--muted); margin-bottom:6px}\n.legend{display:flex; gap:10px; align-"
"items:baseline} .legend b{font-weight:700; font-size:11px}\n.legend-mut{color:var(--faint); font-weight:600; font-size:10px}\n.seg{display:inline-flex; background:var(--bg2); border:"
"1px solid var(--line); border-radius:8px; padding:2px; gap:2px}\n.seg-btn{border:none; background:transparent; color:var(--muted); font-size:10px; font-weight:700; letter-spacing:.2"
"px; padding:3px 9px; border-radius:6px; cursor:pointer; transition:background .15s ease, color .15s ease}\n.seg-btn.on{background:var(--panel); color:var(--text); box-shadow:0 1px 3"
"px rgba(0,0,0,.25)}\n.seg-btn:hover:not(.on){color:var(--text)}\n.spark-wrap{position:relative; width:100%; height:46px}\n.spark{width:100%; height:46px; display:block; cursor:crossha"
"ir}\n.spark-guide{position:absolute; top:0; bottom:0; width:1px; background:var(--line2); transform:translateX(-.5px); pointer-events:none}\n.spark-dot{position:absolute; width:9px; "
"height:9px; border-radius:50%; transform:translate(-50%,-50%); box-shadow:0 0 0 2px var(--panel); pointer-events:none}\n.spark-tip{position:absolute; top:-4px; transform:translate(-"
"50%,-100%); background:var(--panel); border:1px solid var(--line2); border-radius:8px; padding:4px 8px; font-size:11px; font-weight:700; white-space:nowrap; display:flex; gap:8px; "
"pointer-events:none; box-shadow:0 6px 16px rgba(0,0,0,.28); z-index:3}\n\n.slider-row{display:flex; align-items:center; gap:10px; margin-top:16px}\n.slider-cap{font-size:11px; color:v"
"ar(--muted); width:26px}\n.slider-val{font-size:12px; color:var(--text); width:38px; text-align:right; font-weight:600}\n.slider{flex:1; -webkit-appearance:none; appearance:none; hei"
"ght:6px; border-radius:999px; background:var(--line); outline:none}\n.slider::-webkit-slider-thumb{-webkit-appearance:none; width:16px; height:16px; border-radius:50%; background:va"
"r(--accent); cursor:pointer; box-shadow:0 0 0 4px rgba(99,102,241,.25)}\n.slider::-moz-range-thumb{width:16px; height:16px; border:none; border-radius:50%; background:var(--accent);"
" cursor:pointer; box-shadow:0 0 0 4px rgba(99,102,241,.25)}\n.slider::-moz-range-track{background:transparent; height:6px; border-radius:999px}\n\n.actions{display:flex; gap:10px; mar"
"gin-top:14px}\n\n/* Empty / footer */\n.empty{text-align:center; padding:64px 20px; color:var(--muted)}\n.empty-title{font-size:16px; font-weight:600; color:var(--text)}\n.empty-sub{mar"
"gin-top:6px; font-size:13px}\n.foot{text-align:center; padding:28px 0 4px; color:var(--faint); font-size:12px}\n\n@media (max-width:560px){\n  .tiles{grid-template-columns:1fr 1fr}\n  ."
"wrap{padding:18px 14px 32px}\n  .card{cursor:default}   /* no expand on mobile */\n  .chev{display:none}\n}\n</style></head><body><div id=\"app\"></div><script>(()=>{var ee,$,Ee,_t,R,Me,"
"Ae,Te,ce,Y,W,Le,pe,ue,de,lt,J={},Q=[],it=/acit|ex(?:s|g|n|p|$)|rph|grid|ows|mnc|ntw|ine[ch]|zoo|^ord|itera/i,te=Array.isArray;function F(e,t){for(var n in t)e[n]=t[n];return e}func"
"tion fe(e){e&&e.parentNode&&e.parentNode.removeChild(e)}function ve(e,t,n){var s,_,o,r={};for(o in t)o==\"key\"?s=t[o]:o==\"ref\"?_=t[o]:r[o]=t[o];if(arguments.length>2&&(r.children=ar"
"guments.length>3?ee.call(arguments,2):n),typeof e==\"function\"&&e.defaultProps!=null)for(o in e.defaultProps)r[o]===void 0&&(r[o]=e.defaultProps[o]);return G(e,r,s,_,null)}function "
"G(e,t,n,s,_){var o={type:e,props:t,key:n,ref:s,__k:null,__:null,__b:0,__e:null,__c:null,constructor:void 0,__v:_==null?++Ee:_,__i:-1,__u:0};return _==null&&$.vnode!=null&&$.vnode(o"
"),o}function ne(e){return e.children}function X(e,t){this.props=e,this.context=t}function I(e,t){if(t==null)return e.__?I(e.__,e.__i+1):null;for(var n;t<e.__k.length;t++)if((n=e.__"
"k[t])!=null&&n.__e!=null)return n.__e;return typeof e.type==\"function\"?I(e):null}function ct(e){if(e.__P&&e.__d){var t=e.__v,n=t.__e,s=[],_=[],o=F({},t);o.__v=t.__v+1,$.vnode&&$.vn"
"ode(o),he(e.__P,o,t,e.__n,e.__P.namespaceURI,32&t.__u?[n]:null,s,n==null?I(t):n,!!(32&t.__u),_),o.__v=t.__v,o.__.__k[o.__i]=o,De(s,o,_),t.__e=t.__=null,o.__e!=n&&Ue(o)}}function Ue"
"(e){if((e=e.__)!=null&&e.__c!=null)return e.__e=e.__c.base=null,e.__k.some(function(t){if(t!=null&&t.__e!=null)return e.__e=e.__c.base=t.__e}),Ue(e)}function we(e){(!e.__d&&(e.__d="
"!0)&&R.push(e)&&!Z.__r++||Me!=$.debounceRendering)&&((Me=$.debounceRendering)||Ae)(Z)}function Z(){try{for(var e,t=1;R.length;)R.length>t&&R.sort(Te),e=R.shift(),t=R.length,ct(e)}f"
"inally{R.length=Z.__r=0}}function Fe(e,t,n,s,_,o,r,i,c,l,u){var m,a,d,p,k,y,g,v=s&&s.__k||Q,C=t.length;for(c=ut(n,t,v,c,C),m=0;m<C;m++)(d=n.__k[m])!=null&&(a=d.__i!=-1&&v[d.__i]||J"
",d.__i=m,y=he(e,d,a,_,o,r,i,c,l,u),p=d.__e,d.ref&&a.ref!=d.ref&&(a.ref&&me(a.ref,null,d),u.push(d.ref,d.__c||p,d)),k==null&&p!=null&&(k=p),(g=!!(4&d.__u))||a.__k===d.__k?(c=Re(d,c,"
"e,g),g&&a.__e&&(a.__e=null)):typeof d.type==\"function\"&&y!==void 0?c=y:p&&(c=p.nextSibling),d.__u&=-7);return n.__e=k,c}function ut(e,t,n,s,_){var o,r,i,c,l,u=n.length,m=u,a=0;for("
"e.__k=new Array(_),o=0;o<_;o++)(r=t[o])!=null&&typeof r!=\"boolean\"&&typeof r!=\"function\"?(typeof r==\"string\"||typeof r==\"number\"||typeof r==\"bigint\"||r.constructor==String?r=e.__k["
"o]=G(null,r,null,null,null):te(r)?r=e.__k[o]=G(ne,{children:r},null,null,null):r.constructor===void 0&&r.__b>0?r=e.__k[o]=G(r.type,r.props,r.key,r.ref?r.ref:null,r.__v):e.__k[o]=r,"
"c=o+a,r.__=e,r.__b=e.__b+1,i=null,(l=r.__i=dt(r,n,c,m))!=-1&&(m--,(i=n[l])&&(i.__u|=2)),i==null||i.__v==null?(l==-1&&(_>u?a--:_<u&&a++),typeof r.type!=\"function\"&&(r.__u|=4)):l!=c&"
"&(l==c-1?a--:l==c+1?a++:(l>c?a--:a++,r.__u|=4))):e.__k[o]=null;if(m)for(o=0;o<u;o++)(i=n[o])!=null&&(2&i.__u)==0&&(i.__e==s&&(s=I(i)),Ie(i,i));return s}function Re(e,t,n,s){var _,o"
";if(typeof e.type==\"function\"){for(_=e.__k,o=0;_&&o<_.length;o++)_[o]&&(_[o].__=e,t=Re(_[o],t,n,s));return t}e.__e!=t&&(s&&(t&&e.type&&!t.parentNode&&(t=I(e)),n.insertBefore(e.__e,"
"t||null)),t=e.__e);do t=t&&t.nextSibling;while(t!=null&&t.nodeType==8);return t}function dt(e,t,n,s){var _,o,r,i=e.key,c=e.type,l=t[n],u=l!=null&&(2&l.__u)==0;if(l===null&&i==null|"
"|u&&i==l.key&&c==l.type)return n;if(s>(u?1:0)){for(_=n-1,o=n+1;_>=0||o<t.length;)if((l=t[r=_>=0?_--:o++])!=null&&(2&l.__u)==0&&i==l.key&&c==l.type)return r}return-1}function Pe(e,t"
",n){t[0]==\"-\"?e.setProperty(t,n==null?\"\":n):e[t]=n==null?\"\":typeof n!=\"number\"||it.test(t)?n:n+\"px\"}function K(e,t,n,s,_){var o,r;e:if(t==\"style\")if(typeof n==\"string\")e.style.cssT"
"ext=n;else{if(typeof s==\"string\"&&(e.style.cssText=s=\"\"),s)for(t in s)n&&t in n||Pe(e.style,t,\"\");if(n)for(t in n)s&&n[t]==s[t]||Pe(e.style,t,n[t])}else if(t[0]==\"o\"&&t[1]==\"n\")o=t"
"!=(t=t.replace(Le,\"$1\")),r=t.toLowerCase(),t=r in e||t==\"onFocusOut\"||t==\"onFocusIn\"?r.slice(2):t.slice(2),e.l||(e.l={}),e.l[t+o]=n,n?s?n[W]=s[W]:(n[W]=pe,e.addEventListener(t,o?de"
":ue,o)):e.removeEventListener(t,o?de:ue,o);else{if(_==\"http://www.w3.org/2000/svg\")t=t.replace(/xlink(H|:h)/,\"h\").replace(/sName$/,\"s\");else if(t!=\"width\"&&t!=\"height\"&&t!=\"href\"&&"
"t!=\"list\"&&t!=\"form\"&&t!=\"tabIndex\"&&t!=\"download\"&&t!=\"rowSpan\"&&t!=\"colSpan\"&&t!=\"role\"&&t!=\"popover\"&&t in e)try{e[t]=n==null?\"\":n;break e}catch{}typeof n==\"function\"||(n==null|"
"|n===!1&&t[4]!=\"-\"?e.removeAttribute(t):e.setAttribute(t,t==\"popover\"&&n==1?\"\":n))}}function Se(e){return function(t){if(this.l){var n=this.l[t.type+e];if(t[Y]==null)t[Y]=pe++;else"
" if(t[Y]<n[W])return;return n($.event?$.event(t):t)}}}function he(e,t,n,s,_,o,r,i,c,l){var u,m,a,d,p,k,y,g,v,C,T,f,h,S,A,M,x=t.type;if(t.constructor!==void 0)return null;128&n.__u&"
"&(c=!!(32&n.__u),o=[i=t.__e=n.__e]),(u=$.__b)&&u(t);e:if(typeof x==\"function\"){m=r.length;try{if(v=t.props,C=x.prototype&&x.prototype.render,T=(u=x.contextType)&&s[u.__c],f=u?T?T.p"
"rops.value:u.__:s,n.__c?g=(a=t.__c=n.__c).__=a.__E:(C?t.__c=a=new x(v,f):(t.__c=a=new X(v,f),a.constructor=x,a.render=ft),T&&T.sub(a),a.state||(a.state={}),a.__n=s,d=a.__d=!0,a.__h"
"=[],a._sb=[]),C&&a.__s==null&&(a.__s=a.state),C&&x.getDerivedStateFromProps!=null&&(a.__s==a.state&&(a.__s=F({},a.__s)),F(a.__s,x.getDerivedStateFromProps(v,a.__s))),p=a.props,k=a."
"state,a.__v=t,d)C&&x.getDerivedStateFromProps==null&&a.componentWillMount!=null&&a.componentWillMount(),C&&a.componentDidMount!=null&&a.__h.push(a.componentDidMount);else{if(C&&x.g"
"etDerivedStateFromProps==null&&v!==p&&a.componentWillReceiveProps!=null&&a.componentWillReceiveProps(v,f),t.__v==n.__v||!a.__e&&a.shouldComponentUpdate!=null&&a.shouldComponentUpda"
"te(v,a.__s,f)===!1){t.__v!=n.__v&&(a.props=v,a.state=a.__s,a.__d=!1),t.__e=n.__e,t.__k=n.__k,t.__k.some(function(E){E&&(E.__=t)}),Q.push.apply(a.__h,a._sb),a._sb=[],a.__h.length&&r"
".push(a);break e}a.componentWillUpdate!=null&&a.componentWillUpdate(v,a.__s,f),C&&a.componentDidUpdate!=null&&a.__h.push(function(){a.componentDidUpdate(p,k,y)})}if(a.context=f,a.p"
"rops=v,a.__P=e,a.__e=!1,h=$.__r,S=0,C)a.state=a.__s,a.__d=!1,h&&h(t),u=a.render(a.props,a.state,a.context),Q.push.apply(a.__h,a._sb),a._sb=[];else do a.__d=!1,h&&h(t),u=a.render(a."
"props,a.state,a.context),a.state=a.__s;while(a.__d&&++S<25);a.state=a.__s,a.getChildContext!=null&&(s=F(F({},s),a.getChildContext())),C&&!d&&a.getSnapshotBeforeUpdate!=null&&(y=a.g"
"etSnapshotBeforeUpdate(p,k)),A=u!=null&&u.type===ne&&u.key==null?Ne(u.props.children):u,i=Fe(e,te(A)?A:[A],t,n,s,_,o,r,i,c,l),a.base=t.__e,t.__u&=-161,a.__h.length&&r.push(a),g&&(a"
".__E=a.__=null)}catch(E){if(r.length=m,t.__v=null,c||o!=null){if(E.then){for(t.__u|=c?160:128;i&&i.nodeType==8&&i.nextSibling;)i=i.nextSibling;o!=null&&(o[o.indexOf(i)]=null),t.__e"
"=i}else if(o!=null)for(M=o.length;M--;)fe(o[M])}else t.__e=n.__e;t.__k==null&&(t.__k=n.__k||[]),E.then||He(t),$.__e(E,t,n)}}else o==null&&t.__v==n.__v?(t.__k=n.__k,t.__e=n.__e):i=t"
".__e=pt(n.__e,t,n,s,_,o,r,c,l);return(u=$.diffed)&&u(t),128&t.__u?void 0:i}function He(e){e&&(e.__c&&(e.__c.__e=!0),e.__k&&e.__k.some(He))}function De(e,t,n){for(var s=0;s<n.length"
";s++)me(n[s],n[++s],n[++s]);$.__c&&$.__c(t,e),e.some(function(_){try{e=_.__h,_.__h=[],e.some(function(o){o.call(_)})}catch(o){$.__e(o,_.__v)}})}function Ne(e){return typeof e!=\"obj"
"ect\"||e==null||e.__b>0?e:te(e)?e.map(Ne):e.constructor!==void 0?null:F({},e)}function pt(e,t,n,s,_,o,r,i,c){var l,u,m,a,d,p,k,y=n.props||J,g=t.props,v=t.type;if(v==\"svg\"?_=\"http://"
"www.w3.org/2000/svg\":v==\"math\"?_=\"http://www.w3.org/1998/Math/MathML\":_||(_=\"http://www.w3.org/1999/xhtml\"),o!=null){for(l=0;l<o.length;l++)if((d=o[l])&&\"setAttribute\"in d==!!v&&(v"
"?d.localName==v:d.nodeType==3)){e=d,o[l]=null;break}}if(e==null){if(v==null)return document.createTextNode(g);e=document.createElementNS(_,v,g.is&&g),i&&($.__m&&$.__m(t,o),i=!1),o="
"null}if(v==null)y===g||i&&e.data==g||(e.data=g);else{if(o=v==\"textarea\"&&g.defaultValue!=null?null:o&&ee.call(e.childNodes),!i&&o!=null)for(y={},l=0;l<e.attributes.length;l++)y[(d="
"e.attributes[l]).name]=d.value;for(l in y)d=y[l],l==\"dangerouslySetInnerHTML\"?m=d:l==\"children\"||l in g||l==\"value\"&&\"defaultValue\"in g||l==\"checked\"&&\"defaultChecked\"in g||K(e,l,n"
"ull,d,_);for(l in g)d=g[l],l==\"children\"?a=d:l==\"dangerouslySetInnerHTML\"?u=d:l==\"value\"?p=d:l==\"checked\"?k=d:i&&typeof d!=\"function\"||y[l]===d||K(e,l,d,y[l],_);if(u)i||m&&(u.__htm"
"l==m.__html||u.__html==e.innerHTML)||(e.innerHTML=u.__html),t.__k=[];else if(m&&(e.innerHTML=\"\"),Fe(t.type==\"template\"?e.content:e,te(a)?a:[a],t,n,s,v==\"foreignObject\"?\"http://www."
"w3.org/1999/xhtml\":_,o,r,o?o[0]:n.__k&&I(n,0),i,c),o!=null)for(l=o.length;l--;)fe(o[l]);i&&v!=\"textarea\"||(l=\"value\",v==\"progress\"&&p==null?e.removeAttribute(\"value\"):p!=null&&(p!="
"=e[l]||v==\"progress\"&&!p||v==\"option\"&&p!=y[l])&&K(e,l,p,y[l],_),l=\"checked\",k!=null&&k!=e[l]&&K(e,l,k,y[l],_))}return e}function me(e,t,n){try{if(typeof e==\"function\"){var s=typeo"
"f e.__u==\"function\";s&&e.__u(),s&&t==null||(e.__u=e(t))}else e.current=t}catch(_){$.__e(_,n)}}function Ie(e,t,n){var s,_;if($.unmount&&$.unmount(e),(s=e.ref)&&(s.current&&s.current"
"!=e.__e||me(s,null,t)),(s=e.__c)!=null){if(s.componentWillUnmount)try{s.componentWillUnmount()}catch(o){$.__e(o,t)}s.base=s.__P=s.__n=null}if(s=e.__k)for(_=0;_<s.length;_++)s[_]&&I"
"e(s[_],t,n||typeof e.type!=\"function\");n||fe(e.__e),e.__c=e.__=e.__e=void 0}function ft(e,t,n){return this.constructor(e,n)}function Be(e,t,n){var s,_,o,r;t==document&&(t=document."
"documentElement),$.__&&$.__(e,t),_=(s=typeof n==\"function\")?null:n&&n.__k||t.__k,o=[],r=[],he(t,e=(!s&&n||t).__k=ve(ne,null,[e]),_||J,J,t.namespaceURI,!s&&n?[n]:_?null:t.firstChild"
"?ee.call(t.childNodes):null,o,!s&&n?n:_?_.__e:t.firstChild,s,r),De(o,e,r),e.props.children=null}ee=Q.slice,$={__e:function(e,t,n,s){for(var _,o,r;t=t.__;)if((_=t.__c)&&!_.__)try{if"
"((o=_.constructor)&&o.getDerivedStateFromError!=null&&(_.setState(o.getDerivedStateFromError(e)),r=_.__d),_.componentDidCatch!=null&&(_.componentDidCatch(e,s||{}),r=_.__d),r)return"
" _.__E=_}catch(i){e=i}throw e}},Ee=0,_t=function(e){return e!=null&&e.constructor===void 0},X.prototype.setState=function(e,t){var n;n=this.__s!=null&&this.__s!=this.state?this.__s"
":this.__s=F({},this.state),typeof e==\"function\"&&(e=e(F({},n),this.props)),e&&F(n,e),e!=null&&this.__v&&(t&&this._sb.push(t),we(this))},X.prototype.forceUpdate=function(e){this.__v"
"&&(this.__e=!0,e&&this.__h.push(e),we(this))},X.prototype.render=ne,R=[],Ae=typeof Promise==\"function\"?Promise.prototype.then.bind(Promise.resolve()):setTimeout,Te=function(e,t){re"
"turn e.__v.__b-t.__v.__b},Z.__r=0,ce=Math.random().toString(8),Y=\"__d\"+ce,W=\"__a\"+ce,Le=/(PointerCapture)$|Capture$/i,pe=0,ue=Se(!1),de=Se(!0),lt=0;var B,w,ge,We,se=0,Ge=[],P=$,Oe="
"P.__b,Ve=P.__r,je=P.diffed,qe=P.__c,ze=P.unmount,Ke=P.__;function ae(e,t){P.__h&&P.__h(w,e,se||t),se=0;var n=w.__H||(w.__H={__:[],__h:[]});return e>=n.__.length&&n.__.push({}),n.__"
"[e]}function L(e){return se=1,vt(Je,e)}function vt(e,t,n){var s=ae(B++,2);if(s.t=e,!s.__c&&(s.__=[n?n(t):Je(void 0,t),function(i){var c=s.__N?s.__N[0]:s.__[0],l=s.t(c,i);c!==l&&(s."
"__N=[l,s.__[1]],s.__c.setState({}))}],s.__c=w,!w.__f)){var _=function(i,c,l){if(!s.__c.__H)return!0;var u=!1,m=s.__c.props!==i;if(s.__c.__H.__.some(function(d){if(d.__N){u=!0;var p"
"=d.__[0];d.__=d.__N,d.__N=void 0,p!==d.__[0]&&(m=!0)}}),o){var a=o.call(this,i,c,l);return u?a||m:a}return!u||m};w.__f=!0;var o=w.shouldComponentUpdate,r=w.componentWillUpdate;w.co"
"mponentWillUpdate=function(i,c,l){if(this.__e){var u=o;o=void 0,_(i,c,l),o=u}r&&r.call(this,i,c,l)},w.shouldComponentUpdate=_}return s.__N||s.__}function O(e,t){var n=ae(B++,3);!P."
"__s&&ye(n.__H,t)&&(n.__=e,n.u=t,w.__H.__h.push(n))}function Xe(e,t){var n=ae(B++,4);!P.__s&&ye(n.__H,t)&&(n.__=e,n.u=t,w.__h.push(n))}function V(e){return se=5,ht(function(){return"
"{current:e}},[])}function ht(e,t){var n=ae(B++,7);return ye(n.__H,t)&&(n.__=e(),n.__H=t,n.__h=e),n.__}function mt(){for(var e;e=Ge.shift();){var t=e.__H;if(e.__P&&t)try{t.__h.some("
"oe),t.__h.some(be),t.__h=[]}catch(n){t.__h=[],P.__e(n,e.__v)}}}P.__b=function(e){w=null,Oe&&Oe(e)},P.__=function(e,t){e&&t.__k&&t.__k.__m&&(e.__m=t.__k.__m),Ke&&Ke(e,t)},P.__r=func"
"tion(e){Ve&&Ve(e),B=0;var t=(w=e.__c).__H;t&&(ge===w?(t.__h=[],w.__h=[],t.__.some(function(n){n.__N&&(n.__=n.__N),n.u=n.__N=void 0})):(t.__h.some(oe),t.__h.some(be),t.__h=[],B=0)),"
"ge=w},P.diffed=function(e){je&&je(e);var t=e.__c;t&&t.__H&&(t.__H.__h.length&&(Ge.push(t)!==1&&We===P.requestAnimationFrame||((We=P.requestAnimationFrame)||gt)(mt)),t.__H.__.some(f"
"unction(n){n.u&&(n.__H=n.u,n.u=void 0)})),ge=w=null},P.__c=function(e,t){t.some(function(n){try{n.__h.some(oe),n.__h=n.__h.filter(function(s){return!s.__||be(s)})}catch(s){t.some(f"
"unction(_){_.__h&&(_.__h=[])}),t=[],P.__e(s,n.__v)}}),qe&&qe(e,t)},P.unmount=function(e){ze&&ze(e);var t,n=e.__c;n&&n.__H&&(n.__H.__.some(function(s){try{oe(s)}catch(_){t=_}}),n.__"
"H=void 0,t&&P.__e(t,n.__v))};var Ye=typeof requestAnimationFrame==\"function\";function gt(e){var t,n=function(){clearTimeout(s),Ye&&cancelAnimationFrame(t),setTimeout(e)},s=setTimeo"
"ut(n,35);Ye&&(t=requestAnimationFrame(n))}function oe(e){var t=w,n=e.__c;typeof n==\"function\"&&(e.__c=void 0,n()),w=t}function be(e){var t=w;e.__c=e.__(),w=t}function ye(e,t){retur"
"n!e||e.length!==t.length||t.some(function(n,s){return n!==e[s]})}function Je(e,t){return typeof t==\"function\"?t(e):t}var Ze=function(e,t,n,s){var _;t[0]=0;for(var o=1;o<t.length;o+"
"+){var r=t[o++],i=t[o]?(t[0]|=r?1:2,n[t[o++]]):t[++o];r===3?s[0]=i:r===4?s[1]=Object.assign(s[1]||{},i):r===5?(s[1]=s[1]||{})[t[++o]]=i:r===6?s[1][t[++o]]+=i+\"\":r?(_=e.apply(i,Ze(e"
",i,n,[\"\",null])),s.push(_),i[0]?t[0]|=2:(t[o-2]=0,t[o]=_)):s.push(i)}return s},Qe=new Map;function et(e){var t=Qe.get(this);return t||(t=new Map,Qe.set(this,t)),(t=Ze(this,t.get(e)"
"||(t.set(e,t=(function(n){for(var s,_,o=1,r=\"\",i=\"\",c=[0],l=function(a){o===1&&(a||(r=r.replace(/^\\s*\\n\\s*|\\s*\\n\\s*$/g,\"\")))?c.push(0,a,r):o===3&&(a||r)?(c.push(3,a,r),o=2):o===2&&"
"r===\"...\"&&a?c.push(4,a,0):o===2&&r&&!a?c.push(5,0,!0,r):o>=5&&((r||!a&&o===5)&&(c.push(o,0,r,_),o=6),a&&(c.push(o,a,0,_),o=6)),r=\"\"},u=0;u<n.length;u++){u&&(o===1&&l(),l(u));for(v"
"ar m=0;m<n[u].length;m++)s=n[u][m],o===1?s===\"<\"?(l(),c=[c],o=3):r+=s:o===4?r===\"--\"&&s===\">\"?(o=1,r=\"\"):r=s+r[0]:i?s===i?i=\"\":r+=s:s==='\"'||s===\"'\"?i=s:s===\">\"?(l(),o=1):o&&(s===\""
"=\"?(o=5,_=r,r=\"\"):s===\"/\"&&(o<5||n[u][m+1]===\">\")?(l(),o===3&&(c=c[0]),o=c,(c=c[0]).push(2,0,o),o=0):s===\" \"||s===\"\t\"||s===`\n`||s===\"\\r\"?(l(),o=2):r+=s),o===3&&r===\"!--\"&&(o=4,c=c["
"0])}return l(),c})(e)),t),arguments,[])).length>1?t:t[0]}var b=et.bind(ve),ie=20,tt=5e3,bt=8192,xe=!1,N=e=>window.PS_MOCK?Promise.resolve({ok:!0}):fetch(e,{method:\"POST\"}),H={nodes"
":()=>fetch(\"/api/nodes\").then(e=>e.json()),mute:e=>N(\"/api/node/mute?id=\"+e),unmute:e=>N(\"/api/node/unmute?id=\"+e),volume:(e,t)=>N(\"/api/node/volume?id=\"+e+\"&level=\"+t),reset:e=>N("
"\"/api/node/unlock?id=\"+e),reboot:e=>N(\"/api/node/reboot?id=\"+e),globalMute:()=>N(\"/api/global/mute\"),globalUnmute:()=>N(\"/api/global/unmute\")};function yt(){return window.__mock||("
"window.__mock=[1,2,3].map(e=>({node_id:e,mac:\"84:F7:03:A1:0\"+e+\":7C\",masking_active:e!==2,volume:[65,0,40][e-1],battery_pct:[92,47,78][e-1],delivery_ratio:.9,packet_loss_rate:[.03,"
".08,.02][e-1],cpu0:20,cpu1:15,heap_free:46e5,heap_largest_block:32e5,uptime_s:e*5400,e2e_min:6,e2e_max:18,attack_min:3,attack_max:12,release_min:40,release_max:120}))),window.__moc"
"k.map(e=>{let t=Math.max(0,Math.min(.18,e.packet_loss_rate+(Math.random()*.05-.025)));return{...e,cpu0:Math.max(3,Math.min(95,e.cpu0+(Math.random()*24-12)))|0,cpu1:Math.max(3,Math."
"min(95,e.cpu1+(Math.random()*20-10)))|0,packet_loss_rate:t,delivery_ratio:1-t,masking_active:e.masking_active,volume:e.volume,e2e_avg:8+Math.random()*8,attack_avg:4+Math.random()*6"
",release_avg:55+Math.random()*50}})}var $t=()=>window.PS_MOCK?Promise.resolve(yt()):H.nodes();function kt(e){let t=Math.floor(e/86400),n=Math.floor(e%86400/3600),s=Math.floor(e%360"
"0/60);return t>0?t+\"d \"+n+\"h\":n>0?n+\"h \"+s+\"m\":s+\"m\"}var nt=e=>Math.round(e/1024),re=\"#3b82f6\",_e=\"#8b5cf6\",xt=\"#ef4444\",Ct=\"#10b981\",Mt=\"#22d3ee\",wt=\"#f59e0b\",Pt=\"#f472b6\",ot=\"#94"
"a3b8\";function St(){try{let e=localStorage.getItem(\"ps-theme\");if(e===\"light\"||e===\"dark\")return e}catch{}return window.matchMedia&&window.matchMedia(\"(prefers-color-scheme: light)"
"\").matches?\"light\":\"dark\"}var Et=b`<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><circle cx=\"12\" cy=\"12"
"\" r=\"4\"/><path d=\"M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4\"/></svg>`,At=b`<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"cur"
"rentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z\"/></svg>`,Tt=b`<svg viewBox=\"0 0 24 24\" fill=\"none\""
" stroke=\"currentColor\" stroke-width=\"2.2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M6 9l6 6 6-6\"/></svg>`;function j({series:e,max:t,refs:n=[],fmt:s=i=>Math.round(i)"
",unit:_=\"%\",w:o=260,hgt:r=46}){let[i,c]=L(-1),l=o/(ie-1),u=p=>r-Math.min(p,t)/t*(r-4)-2,m=e.reduce((p,k)=>Math.max(p,(k.data||[]).length),0),a=p=>{let k=p.currentTarget.getBounding"
"ClientRect();if(!k.width||m<1)return;let y=Math.min(1,Math.max(0,(p.clientX-k.left)/k.width)),g=Math.round(y*(ie-1));g>m-1&&(g=m-1),g<0&&(g=0),c(g)},d=i>=0?i/(ie-1)*100:0;return b`"
"\n    <div class=\"spark-wrap\" style=${\"height:\"+r+\"px\"}>\n      <svg class=\"spark\" style=${\"height:\"+r+\"px\"} viewBox=${\"0 0 \"+o+\" \"+r} preserveAspectRatio=\"none\"\n           onMouseMo"
"ve=${a} onMouseLeave=${()=>c(-1)}>\n        ${n.map(p=>b`<line x1=\"0\" y1=${+u(p.value).toFixed(1)}\n            x2=${o} y2=${+u(p.value).toFixed(1)} stroke=${p.color}\n            str"
"oke-width=\"1\" stroke-dasharray=\"3 3\" opacity=\"0.55\" />`)}\n        ${e.map((p,k)=>{let y=p.data;if(!y||y.length<2)return null;let g=y.map((f,h)=>[+(h*l).toFixed(1),+u(f).toFixed(1)]"
"),v=g.map((f,h)=>(h?\"L\":\"M\")+f[0]+\",\"+f[1]).join(\" \"),C=v+\" L\"+g[g.length-1][0]+\",\"+r+\" L\"+g[0][0]+\",\"+r+\" Z\",T=\"g\"+k+\"_\"+p.color.replace(\"#\",\"\");return b`\n            <defs>\n     "
"         <linearGradient id=${T} x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">\n                <stop offset=\"0%\" stop-color=${p.color} stop-opacity=\"0.35\" />\n                <stop offset=\"100%\" sto"
"p-color=${p.color} stop-opacity=\"0\" />\n              </linearGradient>\n            </defs>\n            <path d=${C} fill=${\"url(#\"+T+\")\"} />\n            <path d=${v} fill=\"none\" st"
"roke=${p.color} stroke-width=\"2\"\n                  stroke-linejoin=\"round\" stroke-linecap=\"round\" />`})}\n      </svg>\n      ${i>=0&&b`<div class=\"spark-guide\" style=${\"left:\"+d+\"%\""
"}></div>`}\n      ${i>=0&&e.map(p=>p.data&&p.data[i]!=null?b`<div class=\"spark-dot\" style=${\"left:\"+d+\"%;top:\"+u(p.data[i])/r*100+\"%;background:\"+p.color}></div>`:null)}\n      ${i>="
"0&&b`<div class=\"spark-tip\" style=${\"left:\"+d+\"%\"}>\n        ${e.map(p=>p.data&&p.data[i]!=null?b`<span style=${\"color:\"+p.color}>${(p.label?p.label+\" \":\"\")+s(p.data[i])+_}</span>`:"
"null)}\n      </div>`}\n    </div>`}var le=({label:e,value:t,sub:n})=>b`\n  <div class=\"metric\">\n    <span class=\"metric-label\">${e}</span>\n    <span class=\"metric-value\">${t}${n&&b`<"
"span class=\"metric-sub\">${n}</span>`}</span>\n  </div>`;function st({pct:e,tone:t}){let n=Math.max(0,Math.min(100,e));return b`<div class=\"bar\"><div class=${\"bar-fill tone-\"+t} styl"
"e=${\"width:\"+n+\"%\"}></div></div>`}function $e({label:e,value:t,accent:n}){return b`\n    <div class=\"tile\">\n      <div class=${\"tile-value\"+(n?\" accent\":\"\")}>${t}</div>\n      <div c"
"lass=\"tile-label\">${e}</div>\n    </div>`}function ke({label:e,data:t,avg:n,min:s,max:_,color:o,hgt:r=96}){let i=Math.max(_,n,1)*1.15;return b`\n    <div class=\"chart\">\n      <div cl"
"ass=\"chart-head\">\n        <span>${e}</span>\n        <span class=\"legend\">\n          <b style=${\"color:\"+o}>${n.toFixed(1)} ms</b>\n          <span class=\"legend-mut\">min ${s.toFixed"
"(0)} \302\267 max ${_.toFixed(0)}</span>\n        </span>\n      </div>\n      ${j({max:i,hgt:r,unit:\" ms\",fmt:c=>c.toFixed(1),series:[{data:t,color:o}],refs:[{value:s,color:ot},{value:_,co"
"lor:ot}]})}\n    </div>`}function Lt({node:e,hist:t,expanded:n,onToggle:s,onMute:_,onUnmute:o,onVolume:r,onReset:i,onReboot:c}){let[l,u]=L(e.volume),m=V(!1);O(()=>{m.current||u(e.vo"
"lume)},[e.volume]);let a=e.battery_pct>50?\"good\":e.battery_pct>20?\"warn\":\"bad\",d=nt(e.heap_free)/bt*100,p=e.packet_loss_rate*100,k=\"background:linear-gradient(90deg,var(--accent) \""
"+l+\"%,var(--line) \"+l+\"%)\",y=D=>D.stopPropagation(),[g,v]=L(\"loss\"),C=g===\"loss\",T=e.delivery_ratio*100,f=C?t.loss:t.delivery,h=C?xt:Ct,S=C?p:T,A=D=>b`\n    <div class=\"chart\">\n    "
"  <div class=\"chart-head\">\n        <div class=\"seg\" onClick=${y}>\n          <button class=${\"seg-btn\"+(C?\" on\":\"\")} onClick=${()=>v(\"loss\")}>Loss</button>\n          <button class=$"
"{\"seg-btn\"+(C?\"\":\" on\")} onClick=${()=>v(\"delivery\")}>Delivery</button>\n        </div>\n        <span class=\"legend\"><b style=${\"color:\"+h}>${S.toFixed(1)}%</b></span>\n      </div>\n"
"      ${j({max:100,hgt:D,fmt:z=>z.toFixed(1),series:[{data:f,color:h}]})}\n    </div>`,M=b`\n    <div class=\"card-head\">\n      <div class=\"card-title\"><span class=\"node-dot\"></span>N"
"ode ${e.node_id}</div>\n      <div class=\"head-right\">\n        <span class=${\"pill \"+(e.masking_active?\"pill-on\":\"pill-off\")}>\n          <span class=\"pill-dot\"></span>${e.masking_ac"
"tive?\"Masking\":\"Silent\"}\n        </span>\n        <span class=${\"chev\"+(n?\" open\":\"\")}>${Tt}</span>\n      </div>\n    </div>\n    <div class=\"mac\">${e.mac}</div>`,x=b`\n    <div class="
"\"metrics\">\n      ${le({label:\"Volume\",value:e.volume+\"%\"})}\n      ${le({label:\"Battery\",value:e.battery_pct+\"%\"})}\n      ${le({label:\"Uptime\",value:kt(e.uptime_s)})}\n      ${le({la"
"bel:\"Delivery\",value:Math.round(e.delivery_ratio*100)+\"%\"})}\n    </div>`,E=b`\n    <div class=\"submetric\">\n      <div class=\"submetric-head\"><span>Battery</span><span>${e.battery_pc"
"t}%</span></div>\n      ${st({pct:e.battery_pct,tone:a})}\n    </div>\n    <div class=\"submetric\">\n      <div class=\"submetric-head\"><span>Free memory</span><span>${nt(e.heap_free)} K"
"B</span></div>\n      ${st({pct:d,tone:\"mem\"})}\n    </div>`,U=b`\n    <div class=\"slider-row\" onClick=${y}>\n      <span class=\"slider-cap\">Vol</span>\n      <input type=\"range\" min=\"0"
"\" max=\"100\" value=${l} class=\"slider\" style=${k}\n        onInput=${D=>{xe=!0,m.current=!0,u(+D.target.value)}}\n        onChange=${D=>{let z=+D.target.value;u(z),r(e.node_id,z),setT"
"imeout(()=>{xe=!1,m.current=!1},1e3)}} />\n      <span class=\"slider-val\">${l}%</span>\n    </div>`,q=b`\n    <div class=\"actions\" onClick=${y}>\n      <button class=\"btn btn-mute\" onC"
"lick=${()=>_(e.node_id)}>Mute</button>\n      <button class=\"btn btn-unmute\" onClick=${()=>o(e.node_id)}>Unmute</button>\n    </div>`,Ce=b`\n    <div class=\"actions\" onClick=${y}>\n   "
"   <button class=\"btn btn-reset\" onClick=${()=>i(e.node_id)}>Reset</button>\n      <button class=\"btn btn-reboot\"\n        onClick=${()=>{window.confirm(\"Reboot node \"+e.node_id+\"?\")"
"&&c(e.node_id)}}>Reboot</button>\n    </div>`,at=b`\n    <div class=\"chart\">\n      <div class=\"chart-head\">\n        <span>CPU load</span>\n        <span class=\"legend\">\n          <b s"
"tyle=${\"color:\"+re}>C0 ${e.cpu0}%</b>\n          <b style=${\"color:\"+_e}>C1 ${e.cpu1}%</b>\n        </span>\n      </div>\n      ${j({max:100,series:[{data:t.cpu0,color:re,label:\"C0\"},"
"{data:t.cpu1,color:_e,label:\"C1\"}]})}\n    </div>\n    ${A(46)}`,rt=b`\n    <div class=\"chart\">\n      <div class=\"chart-head\"><span>CPU Core 0</span><span class=\"legend\"><b style=${\"c"
"olor:\"+re}>${e.cpu0}%</b></span></div>\n      ${j({max:100,hgt:84,series:[{data:t.cpu0,color:re}]})}\n    </div>\n    <div class=\"chart\">\n      <div class=\"chart-head\"><span>CPU Core "
"1</span><span class=\"legend\"><b style=${\"color:\"+_e}>${e.cpu1}%</b></span></div>\n      ${j({max:100,hgt:84,series:[{data:t.cpu1,color:_e}]})}\n    </div>\n    ${A(84)}\n    ${ke({labe"
"l:\"End-to-end delay\",hgt:84,data:t.e2e,avg:e.e2e_avg,min:e.e2e_min,max:e.e2e_max,color:Mt})}\n    ${ke({label:\"Attack delay\",hgt:84,data:t.attack,avg:e.attack_avg,min:e.attack_min,m"
"ax:e.attack_max,color:wt})}\n    ${ke({label:\"Release delay\",hgt:84,data:t.release,avg:e.release_avg,min:e.release_min,max:e.release_max,color:Pt})}`;return b`\n    <div class=${\"car"
"d\"+(n?\" expanded\":\"\")} data-id=${e.node_id} onClick=${s}>\n      ${M}\n      ${n?b`<div class=\"expanded-body\">\n            <div class=\"col-left\">${x}${E}${U}${q}${Ce}</div>\n         "
"   <div class=\"col-right\">${rt}</div>\n          </div>`:b`${x}${E}${at}${U}${q}${Ce}`}\n    </div>`}function Ut(){let[e,t]=L([]),[n,s]=L(null),[_,o]=L(!1),[r,i]=L(St),[c,l]=L(null),"
"u=V({}),m=V(null),a=V(null);O(()=>{document.documentElement.dataset.theme=r;try{localStorage.setItem(\"ps-theme\",r)}catch{}},[r]);let d=()=>window.matchMedia(\"(max-width:560px)\").ma"
"tches,[p,k]=L(d);O(()=>{let f=window.matchMedia(\"(max-width:560px)\"),h=()=>{k(f.matches),f.matches&&l(null)};return f.addEventListener(\"change\",h),()=>f.removeEventListener(\"change"
"\",h)},[]);let y=f=>{if(p)return;let h={};m.current&&m.current.querySelectorAll(\".card\").forEach(S=>{h[S.dataset.id]=S.getBoundingClientRect()}),a.current=h,l(S=>S===f?null:f)};Xe(("
")=>{let f=a.current;a.current=null;let h=m.current;if(!f||!h)return;let S=[];h.querySelectorAll(\".card\").forEach(A=>{let M=f[A.dataset.id];if(!M)return;let x=A.getBoundingClientRec"
"t(),E=M.left-x.left,U=M.top-x.top;Math.abs(E)<1&&Math.abs(U)<1||(A.style.transition=\"none\",A.style.transform=\"translate(\"+E+\"px,\"+U+\"px)\",S.push(A))}),S.length&&(h.offsetWidth,requ"
"estAnimationFrame(()=>{S.forEach(A=>{A.style.transition=\"transform .44s cubic-bezier(.2,.7,.2,1)\",A.style.transform=\"\"})}))},[c]);let g=()=>{xe||$t().then(f=>{let h=Array.isArray(f"
")?f:[],S=u.current,A=[];h.forEach(M=>{A.push(M.node_id);let x=S[M.node_id]||(S[M.node_id]={cpu0:[],cpu1:[],loss:[],delivery:[],e2e:[],attack:[],release:[]}),E=(U,q)=>{U.push(q),U.l"
"ength>ie&&U.shift()};E(x.cpu0,M.cpu0),E(x.cpu1,M.cpu1),E(x.loss,M.packet_loss_rate*100),E(x.delivery,M.delivery_ratio*100),E(x.e2e,M.e2e_avg),E(x.attack,M.attack_avg),E(x.release,M"
".release_avg)}),Object.keys(S).forEach(M=>{A.indexOf(+M)===-1&&delete S[M]}),t(h),s(null),o(!0)}).catch(f=>{s(f.message||String(f)),o(!0)})};O(()=>{g();let f=setInterval(g,tt);retu"
"rn()=>clearInterval(f)},[]);let v=f=>f().then(g).catch(h=>s(h.message||String(h))),C=e.filter(f=>f.masking_active).length,T=e.length?Math.round(e.reduce((f,h)=>f+h.battery_pct,0)/e"
".length):0;return b`\n    <div class=\"app\">\n      <header class=\"topbar\">\n        <div class=\"brand\">\n          <div class=\"logo\">PS</div>\n          <div>\n            <div class=\"br"
"and-title\">Privacy Shield</div>\n            <div class=\"brand-sub\">Hub dashboard</div>\n          </div>\n        </div>\n        <div class=\"topbar-right\">\n          <div class=${\"co"
"nn \"+(n?\"conn-bad\":\"conn-ok\")}>\n            <span class=\"conn-dot\"></span>${n?\"Disconnected\":\"Live\"}\n          </div>\n          <button class=\"theme-btn\" title=\"Toggle light / dark"
"\" aria-label=\"Toggle light / dark\"\n            onClick=${()=>i(f=>f===\"light\"?\"dark\":\"light\")}>\n            ${r===\"light\"?At:Et}\n          </button>\n        </div>\n      </header>\n"
"\n      <main class=\"wrap\">\n        <section class=\"tiles\">\n          ${$e({label:\"Nodes online\",value:e.length,accent:!0})}\n          ${$e({label:\"Masking\",value:C})}\n          ${$"
"e({label:\"Avg battery\",value:e.length?T+\"%\":\"\\u2014\"})}\n        </section>\n\n        <section class=\"toolbar\">\n          <button class=\"btn btn-danger\" onClick=${()=>v(H.globalMute)"
"}>Mute all</button>\n          <button class=\"btn btn-success\" onClick=${()=>v(H.globalUnmute)}>Unmute all</button>\n          <button class=\"btn btn-ghost\" onClick=${g}>Refresh</but"
"ton>\n          <span class=\"toolbar-spacer\"></span>\n          <span class=\"refresh-note\">Auto-refresh \302\267 ${tt/1e3}s</span>\n        </section>\n\n        ${n&&b`<div class=\"banner\">Ca"
"n't reach the hub \342\200\224 ${n}</div>`}\n\n        ${_?e.length===0?b`<div class=\"empty\">\n              <div class=\"empty-title\">No masking nodes detected</div>\n              <div class=\""
"empty-sub\">Power on a node to get started.</div>\n            </div>`:b`<section class=\"grid\" ref=${m}>\n              ${e.map(f=>b`<${Lt}\n                key=${f.node_id}\n          "
"      node=${f}\n                hist=${u.current[f.node_id]||{cpu0:[],cpu1:[],loss:[],delivery:[],e2e:[],attack:[],release:[]}}\n                expanded=${c===f.node_id}\n          "
"      onToggle=${()=>y(f.node_id)}\n                onMute=${h=>v(()=>H.mute(h))}\n                onUnmute=${h=>v(()=>H.unmute(h))}\n                onVolume=${(h,S)=>v(()=>H.volume("
"h,S))}\n                onReset=${h=>v(()=>H.reset(h))}\n                onReboot=${h=>v(()=>H.reboot(h))} />`)}\n            </section>`:b`<div class=\"empty\">Loading\342\200\246</div>`}\n\n    "
"    <footer class=\"foot\">Privacy Shield \302\267 ESP-NOW mesh</footer>\n      </main>\n    </div>`}Be(b`<${Ut} />`,document.getElementById(\"app\"));})();</script></body></html>";

esp_err_t dashboard_get_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
