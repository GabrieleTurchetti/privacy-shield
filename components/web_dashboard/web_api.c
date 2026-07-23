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
    bool     active;
} node_status_cache_t;

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
            "\"uptime_s\":%lu"
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
            (unsigned long)(has_status ? uptime : 0)))
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
    char buf[3072];
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
":translateY(-2px)}\n.card.expanded{grid-column:span 2; cursor:default; border-color:var(--line2); box-shadow:0 12px 34px rgba(0,0,0,.32)}\n.head-right{display:flex; align-items:cente"
"r; gap:10px}\n.chev{color:var(--faint); display:grid; place-items:center; transition:transform .2s ease}\n.chev svg{width:18px; height:18px}\n.chev.open{transform:rotate(180deg); colo"
"r:var(--muted)}\n.expanded-body{display:grid; grid-template-columns:repeat(auto-fit,minmax(240px,1fr)); gap:20px; align-items:start; margin-top:4px; animation:expandIn .4s ease both"
"}\n.col-left{display:flex; flex-direction:column; min-width:0}\n.col-right{display:flex; flex-direction:column; gap:14px; min-width:0}\n.col-right .chart{margin-top:0; padding-top:0; "
"border-top:none}\n@keyframes expandIn{from{opacity:0; transform:translateY(-6px)}to{opacity:1; transform:none}}\n.card-head{display:flex; align-items:center; justify-content:space-be"
"tween; margin-bottom:2px}\n.card-title{display:flex; align-items:center; gap:9px; font-size:15.5px; font-weight:700}\n.node-dot{width:9px; height:9px; border-radius:50%; background:l"
"inear-gradient(135deg,var(--accent),var(--accent2))}\n.mac{font-size:11.5px; color:var(--faint); font-family:ui-monospace,SFMono-Regular,Menlo,monospace; margin-bottom:14px}\n\n.pill{"
"display:inline-flex; align-items:center; gap:6px; padding:4px 10px; border-radius:999px; font-size:11px; font-weight:700; letter-spacing:.3px; text-transform:uppercase}\n.pill-dot{w"
"idth:7px; height:7px; border-radius:50%}\n.pill-on{background:var(--pill-on-bg); color:var(--pill-on-fg); border:1px solid var(--pill-on-bd)}\n.pill-on .pill-dot{background:var(--goo"
"d); box-shadow:0 0 0 3px rgba(52,211,153,.2); animation:pulse 1.8s infinite}\n.pill-off{background:rgba(139,152,181,.1); color:var(--muted); border:1px solid var(--line2)}\n.pill-off"
" .pill-dot{background:var(--faint)}\n\n.metrics{display:grid; grid-template-columns:1fr 1fr; gap:10px 14px; margin-bottom:14px}\n.metric{display:flex; flex-direction:column; gap:2px}\n"
".metric-label{font-size:11px; color:var(--muted)}\n.metric-value{font-size:17px; font-weight:700}\n\n.submetric{margin-bottom:12px}\n.submetric-head{display:flex; justify-content:space"
"-between; font-size:11px; color:var(--muted); margin-bottom:5px}\n.bar{height:6px; border-radius:999px; background:var(--bg2); overflow:hidden}\n.bar-fill{height:100%; border-radius:"
"999px; transition:width .4s ease}\n.tone-good{background:linear-gradient(90deg,#10b981,#34d399)}\n.tone-warn{background:linear-gradient(90deg,#f59e0b,#fbbf24)}\n.tone-bad{background:l"
"inear-gradient(90deg,#e11d48,#f87171)}\n.tone-mem{background:linear-gradient(90deg,#6366f1,#a855f7)}\n\n.chart{margin-top:12px; padding-top:12px; border-top:1px solid var(--line)}\n.ch"
"art-head{display:flex; justify-content:space-between; align-items:center; font-size:11px; color:var(--muted); margin-bottom:6px}\n.legend{display:flex; gap:10px} .legend b{font-weig"
"ht:700; font-size:11px}\n.seg{display:inline-flex; background:var(--bg2); border:1px solid var(--line); border-radius:8px; padding:2px; gap:2px}\n.seg-btn{border:none; background:tra"
"nsparent; color:var(--muted); font-size:10px; font-weight:700; letter-spacing:.2px; padding:3px 9px; border-radius:6px; cursor:pointer; transition:background .15s ease, color .15s "
"ease}\n.seg-btn.on{background:var(--panel); color:var(--text); box-shadow:0 1px 3px rgba(0,0,0,.25)}\n.seg-btn:hover:not(.on){color:var(--text)}\n.spark-wrap{position:relative; width:"
"100%; height:46px}\n.spark{width:100%; height:46px; display:block; cursor:crosshair}\n.spark-guide{position:absolute; top:0; bottom:0; width:1px; background:var(--line2); transform:t"
"ranslateX(-.5px); pointer-events:none}\n.spark-dot{position:absolute; width:9px; height:9px; border-radius:50%; transform:translate(-50%,-50%); box-shadow:0 0 0 2px var(--panel); po"
"inter-events:none}\n.spark-tip{position:absolute; top:-4px; transform:translate(-50%,-100%); background:var(--panel); border:1px solid var(--line2); border-radius:8px; padding:4px 8"
"px; font-size:11px; font-weight:700; white-space:nowrap; display:flex; gap:8px; pointer-events:none; box-shadow:0 6px 16px rgba(0,0,0,.28); z-index:3}\n\n.slider-row{display:flex; al"
"ign-items:center; gap:10px; margin-top:16px}\n.slider-cap{font-size:11px; color:var(--muted); width:26px}\n.slider-val{font-size:12px; color:var(--text); width:38px; text-align:right"
"; font-weight:600}\n.slider{flex:1; -webkit-appearance:none; appearance:none; height:6px; border-radius:999px; background:var(--line); outline:none}\n.slider::-webkit-slider-thumb{-w"
"ebkit-appearance:none; width:16px; height:16px; border-radius:50%; background:var(--accent); cursor:pointer; box-shadow:0 0 0 4px rgba(99,102,241,.25)}\n.slider::-moz-range-thumb{wi"
"dth:16px; height:16px; border:none; border-radius:50%; background:var(--accent); cursor:pointer; box-shadow:0 0 0 4px rgba(99,102,241,.25)}\n.slider::-moz-range-track{background:tra"
"nsparent; height:6px; border-radius:999px}\n\n.actions{display:flex; gap:10px; margin-top:14px}\n\n/* Empty / footer */\n.empty{text-align:center; padding:64px 20px; color:var(--muted)}"
"\n.empty-title{font-size:16px; font-weight:600; color:var(--text)}\n.empty-sub{margin-top:6px; font-size:13px}\n.foot{text-align:center; padding:28px 0 4px; color:var(--faint); font-s"
"ize:12px}\n\n@media (max-width:560px){\n  .tiles{grid-template-columns:1fr 1fr}\n  .wrap{padding:18px 14px 32px}\n  .card{cursor:default}   /* no expand on mobile */\n  .chev{display:non"
"e}\n}\n</style></head><body><div id=\"app\"></div><script>(()=>{var Z,k,St,re,L,Ct,Et,Ut,at,K,O,Tt,dt,ct,ut,_e,J={},Q=[],le=/acit|ex(?:s|g|n|p|$)|rph|grid|ows|mnc|ntw|ine[ch]|zoo|^ord|"
"itera/i,tt=Array.isArray;function H(t,e){for(var n in e)t[n]=e[n];return t}function pt(t){t&&t.parentNode&&t.parentNode.removeChild(t)}function ft(t,e,n){var s,l,o,_={};for(o in e)"
"o==\"key\"?s=e[o]:o==\"ref\"?l=e[o]:_[o]=e[o];if(arguments.length>2&&(_.children=arguments.length>3?Z.call(arguments,2):n),typeof t==\"function\"&&t.defaultProps!=null)for(o in t.default"
"Props)_[o]===void 0&&(_[o]=t.defaultProps[o]);return G(t,_,s,l,null)}function G(t,e,n,s,l){var o={type:t,props:e,key:n,ref:s,__k:null,__:null,__b:0,__e:null,__c:null,constructor:vo"
"id 0,__v:l==null?++St:l,__i:-1,__u:0};return l==null&&k.vnode!=null&&k.vnode(o),o}function et(t){return t.children}function X(t,e){this.props=t,this.context=e}function I(t,e){if(e="
"=null)return t.__?I(t.__,t.__i+1):null;for(var n;e<t.__k.length;e++)if((n=t.__k[e])!=null&&n.__e!=null)return n.__e;return typeof t.type==\"function\"?I(t):null}function ie(t){if(t._"
"_P&&t.__d){var e=t.__v,n=e.__e,s=[],l=[],o=H({},e);o.__v=e.__v+1,k.vnode&&k.vnode(o),ht(t.__P,o,e,t.__n,t.__P.namespaceURI,32&e.__u?[n]:null,s,n==null?I(e):n,!!(32&e.__u),l),o.__v="
"e.__v,o.__.__k[o.__i]=o,Rt(s,o,l),e.__e=e.__=null,o.__e!=n&&At(o)}}function At(t){if((t=t.__)!=null&&t.__c!=null)return t.__e=t.__c.base=null,t.__k.some(function(e){if(e!=null&&e._"
"_e!=null)return t.__e=t.__c.base=e.__e}),At(t)}function Mt(t){(!t.__d&&(t.__d=!0)&&L.push(t)&&!Y.__r++||Ct!=k.debounceRendering)&&((Ct=k.debounceRendering)||Et)(Y)}function Y(){try"
"{for(var t,e=1;L.length;)L.length>e&&L.sort(Ut),t=L.shift(),e=L.length,ie(t)}finally{L.length=Y.__r=0}}function Ht(t,e,n,s,l,o,_,c,u,i,d){var h,r,a,g,C,m,y,f=s&&s.__k||Q,w=e.length"
";for(u=ae(n,e,f,u,w),h=0;h<w;h++)(a=n.__k[h])!=null&&(r=a.__i!=-1&&f[a.__i]||J,a.__i=h,m=ht(t,a,r,l,o,_,c,u,i,d),g=a.__e,a.ref&&r.ref!=a.ref&&(r.ref&&vt(r.ref,null,a),d.push(a.ref,"
"a.__c||g,a)),C==null&&g!=null&&(C=g),(y=!!(4&a.__u))||r.__k===a.__k?(u=Lt(a,u,t,y),y&&r.__e&&(r.__e=null)):typeof a.type==\"function\"&&m!==void 0?u=m:g&&(u=g.nextSibling),a.__u&=-7)"
";return n.__e=C,u}function ae(t,e,n,s,l){var o,_,c,u,i,d=n.length,h=d,r=0;for(t.__k=new Array(l),o=0;o<l;o++)(_=e[o])!=null&&typeof _!=\"boolean\"&&typeof _!=\"function\"?(typeof _==\"s"
"tring\"||typeof _==\"number\"||typeof _==\"bigint\"||_.constructor==String?_=t.__k[o]=G(null,_,null,null,null):tt(_)?_=t.__k[o]=G(et,{children:_},null,null,null):_.constructor===void 0&"
"&_.__b>0?_=t.__k[o]=G(_.type,_.props,_.key,_.ref?_.ref:null,_.__v):t.__k[o]=_,u=o+r,_.__=t,_.__b=t.__b+1,c=null,(i=_.__i=ce(_,n,u,h))!=-1&&(h--,(c=n[i])&&(c.__u|=2)),c==null||c.__v"
"==null?(i==-1&&(l>d?r--:l<d&&r++),typeof _.type!=\"function\"&&(_.__u|=4)):i!=u&&(i==u-1?r--:i==u+1?r++:(i>u?r--:r++,_.__u|=4))):t.__k[o]=null;if(h)for(o=0;o<d;o++)(c=n[o])!=null&&(2"
"&c.__u)==0&&(c.__e==s&&(s=I(c)),Ft(c,c));return s}function Lt(t,e,n,s){var l,o;if(typeof t.type==\"function\"){for(l=t.__k,o=0;l&&o<l.length;o++)l[o]&&(l[o].__=t,e=Lt(l[o],e,n,s));re"
"turn e}t.__e!=e&&(s&&(e&&t.type&&!e.parentNode&&(e=I(t)),n.insertBefore(t.__e,e||null)),e=t.__e);do e=e&&e.nextSibling;while(e!=null&&e.nodeType==8);return e}function ce(t,e,n,s){v"
"ar l,o,_,c=t.key,u=t.type,i=e[n],d=i!=null&&(2&i.__u)==0;if(i===null&&c==null||d&&c==i.key&&u==i.type)return n;if(s>(d?1:0)){for(l=n-1,o=n+1;l>=0||o<e.length;)if((i=e[_=l>=0?l--:o+"
"+])!=null&&(2&i.__u)==0&&c==i.key&&u==i.type)return _}return-1}function xt(t,e,n){e[0]==\"-\"?t.setProperty(e,n==null?\"\":n):t[e]=n==null?\"\":typeof n!=\"number\"||le.test(e)?n:n+\"px\"}fu"
"nction z(t,e,n,s,l){var o,_;t:if(e==\"style\")if(typeof n==\"string\")t.style.cssText=n;else{if(typeof s==\"string\"&&(t.style.cssText=s=\"\"),s)for(e in s)n&&e in n||xt(t.style,e,\"\");if(n"
")for(e in n)s&&n[e]==s[e]||xt(t.style,e,n[e])}else if(e[0]==\"o\"&&e[1]==\"n\")o=e!=(e=e.replace(Tt,\"$1\")),_=e.toLowerCase(),e=_ in t||e==\"onFocusOut\"||e==\"onFocusIn\"?_.slice(2):e.slic"
"e(2),t.l||(t.l={}),t.l[e+o]=n,n?s?n[O]=s[O]:(n[O]=dt,t.addEventListener(e,o?ut:ct,o)):t.removeEventListener(e,o?ut:ct,o);else{if(l==\"http://www.w3.org/2000/svg\")e=e.replace(/xlink("
"H|:h)/,\"h\").replace(/sName$/,\"s\");else if(e!=\"width\"&&e!=\"height\"&&e!=\"href\"&&e!=\"list\"&&e!=\"form\"&&e!=\"tabIndex\"&&e!=\"download\"&&e!=\"rowSpan\"&&e!=\"colSpan\"&&e!=\"role\"&&e!=\"popover"
"\"&&e in t)try{t[e]=n==null?\"\":n;break t}catch{}typeof n==\"function\"||(n==null||n===!1&&e[4]!=\"-\"?t.removeAttribute(e):t.setAttribute(e,e==\"popover\"&&n==1?\"\":n))}}function Pt(t){ret"
"urn function(e){if(this.l){var n=this.l[e.type+t];if(e[K]==null)e[K]=dt++;else if(e[K]<n[O])return;return n(k.event?k.event(e):e)}}}function ht(t,e,n,s,l,o,_,c,u,i){var d,h,r,a,g,C"
",m,y,f,w,U,p,v,P,E,S,$=e.type;if(e.constructor!==void 0)return null;128&n.__u&&(u=!!(32&n.__u),o=[c=e.__e=n.__e]),(d=k.__b)&&d(e);t:if(typeof $==\"function\"){h=_.length;try{if(f=e.p"
"rops,w=$.prototype&&$.prototype.render,U=(d=$.contextType)&&s[d.__c],p=d?U?U.props.value:d.__:s,n.__c?y=(r=e.__c=n.__c).__=r.__E:(w?e.__c=r=new $(f,p):(e.__c=r=new X(f,p),r.constru"
"ctor=$,r.render=de),U&&U.sub(r),r.state||(r.state={}),r.__n=s,a=r.__d=!0,r.__h=[],r._sb=[]),w&&r.__s==null&&(r.__s=r.state),w&&$.getDerivedStateFromProps!=null&&(r.__s==r.state&&(r"
".__s=H({},r.__s)),H(r.__s,$.getDerivedStateFromProps(f,r.__s))),g=r.props,C=r.state,r.__v=e,a)w&&$.getDerivedStateFromProps==null&&r.componentWillMount!=null&&r.componentWillMount("
"),w&&r.componentDidMount!=null&&r.__h.push(r.componentDidMount);else{if(w&&$.getDerivedStateFromProps==null&&f!==g&&r.componentWillReceiveProps!=null&&r.componentWillReceiveProps(f"
",p),e.__v==n.__v||!r.__e&&r.shouldComponentUpdate!=null&&r.shouldComponentUpdate(f,r.__s,p)===!1){e.__v!=n.__v&&(r.props=f,r.state=r.__s,r.__d=!1),e.__e=n.__e,e.__k=n.__k,e.__k.som"
"e(function(T){T&&(T.__=e)}),Q.push.apply(r.__h,r._sb),r._sb=[],r.__h.length&&_.push(r);break t}r.componentWillUpdate!=null&&r.componentWillUpdate(f,r.__s,p),w&&r.componentDidUpdate"
"!=null&&r.__h.push(function(){r.componentDidUpdate(g,C,m)})}if(r.context=p,r.props=f,r.__P=t,r.__e=!1,v=k.__r,P=0,w)r.state=r.__s,r.__d=!1,v&&v(e),d=r.render(r.props,r.state,r.cont"
"ext),Q.push.apply(r.__h,r._sb),r._sb=[];else do r.__d=!1,v&&v(e),d=r.render(r.props,r.state,r.context),r.state=r.__s;while(r.__d&&++P<25);r.state=r.__s,r.getChildContext!=null&&(s="
"H(H({},s),r.getChildContext())),w&&!a&&r.getSnapshotBeforeUpdate!=null&&(m=r.getSnapshotBeforeUpdate(g,C)),E=d!=null&&d.type===et&&d.key==null?Dt(d.props.children):d,c=Ht(t,tt(E)?E"
":[E],e,n,s,l,o,_,c,u,i),r.base=e.__e,e.__u&=-161,r.__h.length&&_.push(r),y&&(r.__E=r.__=null)}catch(T){if(_.length=h,e.__v=null,u||o!=null){if(T.then){for(e.__u|=u?160:128;c&&c.nod"
"eType==8&&c.nextSibling;)c=c.nextSibling;o!=null&&(o[o.indexOf(c)]=null),e.__e=c}else if(o!=null)for(S=o.length;S--;)pt(o[S])}else e.__e=n.__e;e.__k==null&&(e.__k=n.__k||[]),T.then"
"||Nt(e),k.__e(T,e,n)}}else o==null&&e.__v==n.__v?(e.__k=n.__k,e.__e=n.__e):c=e.__e=ue(n.__e,e,n,s,l,o,_,u,i);return(d=k.diffed)&&d(e),128&e.__u?void 0:c}function Nt(t){t&&(t.__c&&("
"t.__c.__e=!0),t.__k&&t.__k.some(Nt))}function Rt(t,e,n){for(var s=0;s<n.length;s++)vt(n[s],n[++s],n[++s]);k.__c&&k.__c(e,t),t.some(function(l){try{t=l.__h,l.__h=[],t.some(function("
"o){o.call(l)})}catch(o){k.__e(o,l.__v)}})}function Dt(t){return typeof t!=\"object\"||t==null||t.__b>0?t:tt(t)?t.map(Dt):t.constructor!==void 0?null:H({},t)}function ue(t,e,n,s,l,o,_"
",c,u){var i,d,h,r,a,g,C,m=n.props||J,y=e.props,f=e.type;if(f==\"svg\"?l=\"http://www.w3.org/2000/svg\":f==\"math\"?l=\"http://www.w3.org/1998/Math/MathML\":l||(l=\"http://www.w3.org/1999/xh"
"tml\"),o!=null){for(i=0;i<o.length;i++)if((a=o[i])&&\"setAttribute\"in a==!!f&&(f?a.localName==f:a.nodeType==3)){t=a,o[i]=null;break}}if(t==null){if(f==null)return document.createText"
"Node(y);t=document.createElementNS(l,f,y.is&&y),c&&(k.__m&&k.__m(e,o),c=!1),o=null}if(f==null)m===y||c&&t.data==y||(t.data=y);else{if(o=f==\"textarea\"&&y.defaultValue!=null?null:o&&"
"Z.call(t.childNodes),!c&&o!=null)for(m={},i=0;i<t.attributes.length;i++)m[(a=t.attributes[i]).name]=a.value;for(i in m)a=m[i],i==\"dangerouslySetInnerHTML\"?h=a:i==\"children\"||i in y"
"||i==\"value\"&&\"defaultValue\"in y||i==\"checked\"&&\"defaultChecked\"in y||z(t,i,null,a,l);for(i in y)a=y[i],i==\"children\"?r=a:i==\"dangerouslySetInnerHTML\"?d=a:i==\"value\"?g=a:i==\"checke"
"d\"?C=a:c&&typeof a!=\"function\"||m[i]===a||z(t,i,a,m[i],l);if(d)c||h&&(d.__html==h.__html||d.__html==t.innerHTML)||(t.innerHTML=d.__html),e.__k=[];else if(h&&(t.innerHTML=\"\"),Ht(e.t"
"ype==\"template\"?t.content:t,tt(r)?r:[r],e,n,s,f==\"foreignObject\"?\"http://www.w3.org/1999/xhtml\":l,o,_,o?o[0]:n.__k&&I(n,0),c,u),o!=null)for(i=o.length;i--;)pt(o[i]);c&&f!=\"textarea"
"\"||(i=\"value\",f==\"progress\"&&g==null?t.removeAttribute(\"value\"):g!=null&&(g!==t[i]||f==\"progress\"&&!g||f==\"option\"&&g!=m[i])&&z(t,i,g,m[i],l),i=\"checked\",C!=null&&C!=t[i]&&z(t,i,C,"
"m[i],l))}return t}function vt(t,e,n){try{if(typeof t==\"function\"){var s=typeof t.__u==\"function\";s&&t.__u(),s&&e==null||(t.__u=t(e))}else t.current=e}catch(l){k.__e(l,n)}}function "
"Ft(t,e,n){var s,l;if(k.unmount&&k.unmount(t),(s=t.ref)&&(s.current&&s.current!=t.__e||vt(s,null,e)),(s=t.__c)!=null){if(s.componentWillUnmount)try{s.componentWillUnmount()}catch(o)"
"{k.__e(o,e)}s.base=s.__P=s.__n=null}if(s=t.__k)for(l=0;l<s.length;l++)s[l]&&Ft(s[l],e,n||typeof t.type!=\"function\");n||pt(t.__e),t.__c=t.__=t.__e=void 0}function de(t,e,n){return t"
"his.constructor(t,n)}function It(t,e,n){var s,l,o,_;e==document&&(e=document.documentElement),k.__&&k.__(t,e),l=(s=typeof n==\"function\")?null:n&&n.__k||e.__k,o=[],_=[],ht(e,t=(!s&&"
"n||e).__k=ft(et,null,[t]),l||J,J,e.namespaceURI,!s&&n?[n]:l?null:e.firstChild?Z.call(e.childNodes):null,o,!s&&n?n:l?l.__e:e.firstChild,s,_),Rt(o,t,_),t.props.children=null}Z=Q.slic"
"e,k={__e:function(t,e,n,s){for(var l,o,_;e=e.__;)if((l=e.__c)&&!l.__)try{if((o=l.constructor)&&o.getDerivedStateFromError!=null&&(l.setState(o.getDerivedStateFromError(t)),_=l.__d)"
",l.componentDidCatch!=null&&(l.componentDidCatch(t,s||{}),_=l.__d),_)return l.__E=l}catch(c){t=c}throw t}},St=0,re=function(t){return t!=null&&t.constructor===void 0},X.prototype.s"
"etState=function(t,e){var n;n=this.__s!=null&&this.__s!=this.state?this.__s:this.__s=H({},this.state),typeof t==\"function\"&&(t=t(H({},n),this.props)),t&&H(n,t),t!=null&&this.__v&&("
"e&&this._sb.push(e),Mt(this))},X.prototype.forceUpdate=function(t){this.__v&&(this.__e=!0,t&&this.__h.push(t),Mt(this))},X.prototype.render=et,L=[],Et=typeof Promise==\"function\"?Pr"
"omise.prototype.then.bind(Promise.resolve()):setTimeout,Ut=function(t,e){return t.__v.__b-e.__v.__b},Y.__r=0,at=Math.random().toString(8),K=\"__d\"+at,O=\"__a\"+at,Tt=/(PointerCapture)"
"$|Capture$/i,dt=0,ct=Pt(!1),ut=Pt(!0),_e=0;var B,M,mt,Bt,ot=0,Gt=[],x=k,Wt=x.__b,Ot=x.__r,Vt=x.diffed,jt=x.__c,qt=x.unmount,zt=x.__;function st(t,e){x.__h&&x.__h(M,t,ot||e),ot=0;va"
"r n=M.__H||(M.__H={__:[],__h:[]});return t>=n.__.length&&n.__.push({}),n.__[t]}function A(t){return ot=1,pe(Jt,t)}function pe(t,e,n){var s=st(B++,2);if(s.t=t,!s.__c&&(s.__=[n?n(e):"
"Jt(void 0,e),function(c){var u=s.__N?s.__N[0]:s.__[0],i=s.t(u,c);u!==i&&(s.__N=[i,s.__[1]],s.__c.setState({}))}],s.__c=M,!M.__f)){var l=function(c,u,i){if(!s.__c.__H)return!0;var d"
"=!1,h=s.__c.props!==c;if(s.__c.__H.__.some(function(a){if(a.__N){d=!0;var g=a.__[0];a.__=a.__N,a.__N=void 0,g!==a.__[0]&&(h=!0)}}),o){var r=o.call(this,c,u,i);return d?r||h:r}retur"
"n!d||h};M.__f=!0;var o=M.shouldComponentUpdate,_=M.componentWillUpdate;M.componentWillUpdate=function(c,u,i){if(this.__e){var d=o;o=void 0,l(c,u,i),o=d}_&&_.call(this,c,u,i)},M.sho"
"uldComponentUpdate=l}return s.__N||s.__}function V(t,e){var n=st(B++,3);!x.__s&&bt(n.__H,e)&&(n.__=t,n.u=e,M.__H.__h.push(n))}function Xt(t,e){var n=st(B++,4);!x.__s&&bt(n.__H,e)&&"
"(n.__=t,n.u=e,M.__h.push(n))}function j(t){return ot=5,fe(function(){return{current:t}},[])}function fe(t,e){var n=st(B++,7);return bt(n.__H,e)&&(n.__=t(),n.__H=e,n.__h=t),n.__}fun"
"ction he(){for(var t;t=Gt.shift();){var e=t.__H;if(t.__P&&e)try{e.__h.some(nt),e.__h.some(gt),e.__h=[]}catch(n){e.__h=[],x.__e(n,t.__v)}}}x.__b=function(t){M=null,Wt&&Wt(t)},x.__=f"
"unction(t,e){t&&e.__k&&e.__k.__m&&(t.__m=e.__k.__m),zt&&zt(t,e)},x.__r=function(t){Ot&&Ot(t),B=0;var e=(M=t.__c).__H;e&&(mt===M?(e.__h=[],M.__h=[],e.__.some(function(n){n.__N&&(n._"
"_=n.__N),n.u=n.__N=void 0})):(e.__h.some(nt),e.__h.some(gt),e.__h=[],B=0)),mt=M},x.diffed=function(t){Vt&&Vt(t);var e=t.__c;e&&e.__H&&(e.__H.__h.length&&(Gt.push(e)!==1&&Bt===x.req"
"uestAnimationFrame||((Bt=x.requestAnimationFrame)||ve)(he)),e.__H.__.some(function(n){n.u&&(n.__H=n.u,n.u=void 0)})),mt=M=null},x.__c=function(t,e){e.some(function(n){try{n.__h.som"
"e(nt),n.__h=n.__h.filter(function(s){return!s.__||gt(s)})}catch(s){e.some(function(l){l.__h&&(l.__h=[])}),e=[],x.__e(s,n.__v)}}),jt&&jt(t,e)},x.unmount=function(t){qt&&qt(t);var e,"
"n=t.__c;n&&n.__H&&(n.__H.__.some(function(s){try{nt(s)}catch(l){e=l}}),n.__H=void 0,e&&x.__e(e,n.__v))};var Kt=typeof requestAnimationFrame==\"function\";function ve(t){var e,n=funct"
"ion(){clearTimeout(s),Kt&&cancelAnimationFrame(e),setTimeout(t)},s=setTimeout(n,35);Kt&&(e=requestAnimationFrame(n))}function nt(t){var e=M,n=t.__c;typeof n==\"function\"&&(t.__c=voi"
"d 0,n()),M=e}function gt(t){var e=M;t.__c=t.__(),M=e}function bt(t,e){return!t||t.length!==e.length||e.some(function(n,s){return n!==t[s]})}function Jt(t,e){return typeof e==\"funct"
"ion\"?e(t):e}var Yt=function(t,e,n,s){var l;e[0]=0;for(var o=1;o<e.length;o++){var _=e[o++],c=e[o]?(e[0]|=_?1:2,n[e[o++]]):e[++o];_===3?s[0]=c:_===4?s[1]=Object.assign(s[1]||{},c):_"
"===5?(s[1]=s[1]||{})[e[++o]]=c:_===6?s[1][e[++o]]+=c+\"\":_?(l=t.apply(c,Yt(t,c,n,[\"\",null])),s.push(l),c[0]?e[0]|=2:(e[o-2]=0,e[o]=l)):s.push(c)}return s},Qt=new Map;function Zt(t){"
"var e=Qt.get(this);return e||(e=new Map,Qt.set(this,e)),(e=Yt(this,e.get(t)||(e.set(t,e=(function(n){for(var s,l,o=1,_=\"\",c=\"\",u=[0],i=function(r){o===1&&(r||(_=_.replace(/^\\s*\\n\\s"
"*|\\s*\\n\\s*$/g,\"\")))?u.push(0,r,_):o===3&&(r||_)?(u.push(3,r,_),o=2):o===2&&_===\"...\"&&r?u.push(4,r,0):o===2&&_&&!r?u.push(5,0,!0,_):o>=5&&((_||!r&&o===5)&&(u.push(o,0,_,l),o=6),r&&"
"(u.push(o,r,0,l),o=6)),_=\"\"},d=0;d<n.length;d++){d&&(o===1&&i(),i(d));for(var h=0;h<n[d].length;h++)s=n[d][h],o===1?s===\"<\"?(i(),u=[u],o=3):_+=s:o===4?_===\"--\"&&s===\">\"?(o=1,_=\"\"):"
"_=s+_[0]:c?s===c?c=\"\":_+=s:s==='\"'||s===\"'\"?c=s:s===\">\"?(i(),o=1):o&&(s===\"=\"?(o=5,l=_,_=\"\"):s===\"/\"&&(o<5||n[d][h+1]===\">\")?(i(),o===3&&(u=u[0]),o=u,(u=u[0]).push(2,0,o),o=0):s==="
"\" \"||s===\"\t\"||s===`\n`||s===\"\\r\"?(i(),o=2):_+=s),o===3&&_===\"!--\"&&(o=4,u=u[0])}return i(),u})(t)),e),arguments,[])).length>1?e:e[0]}var b=Zt.bind(ft),F=20,te=5e3,me=8192,$t=!1,D=t="
">window.PS_MOCK?Promise.resolve({ok:!0}):fetch(t,{method:\"POST\"}),N={nodes:()=>fetch(\"/api/nodes\").then(t=>t.json()),mute:t=>D(\"/api/node/mute?id=\"+t),unmute:t=>D(\"/api/node/unmute"
"?id=\"+t),volume:(t,e)=>D(\"/api/node/volume?id=\"+t+\"&level=\"+e),reset:t=>D(\"/api/node/unlock?id=\"+t),reboot:t=>D(\"/api/node/reboot?id=\"+t),globalMute:()=>D(\"/api/global/mute\"),globa"
"lUnmute:()=>D(\"/api/global/unmute\")};function ge(){return window.__mock||(window.__mock=[1,2,3].map(t=>({node_id:t,mac:\"84:F7:03:A1:0\"+t+\":7C\",masking_active:t!==2,volume:[65,0,40]"
"[t-1],battery_pct:[92,47,78][t-1],delivery_ratio:.9,packet_loss_rate:[.03,.08,.02][t-1],cpu0:20,cpu1:15,heap_free:46e5,heap_largest_block:32e5,uptime_s:t*5400}))),window.__mock.map"
"(t=>{let e=Math.max(0,Math.min(.18,t.packet_loss_rate+(Math.random()*.05-.025)));return{...t,cpu0:Math.max(3,Math.min(95,t.cpu0+(Math.random()*24-12)))|0,cpu1:Math.max(3,Math.min(9"
"5,t.cpu1+(Math.random()*20-10)))|0,packet_loss_rate:e,delivery_ratio:1-e,masking_active:t.masking_active,volume:t.volume}})}var be=()=>window.PS_MOCK?Promise.resolve(ge()):N.nodes("
");function ye(t){let e=Math.floor(t/86400),n=Math.floor(t%86400/3600),s=Math.floor(t%3600/60);return e>0?e+\"d \"+n+\"h\":n>0?n+\"h \"+s+\"m\":s+\"m\"}var ee=t=>Math.round(t/1024),rt=\"#3b82f"
"6\",_t=\"#8b5cf6\",$e=\"#ef4444\",ke=\"#10b981\";function we(){try{let t=localStorage.getItem(\"ps-theme\");if(t===\"light\"||t===\"dark\")return t}catch{}return window.matchMedia&&window.match"
"Media(\"(prefers-color-scheme: light)\").matches?\"light\":\"dark\"}var Ce=b`<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-lin"
"ejoin=\"round\"><circle cx=\"12\" cy=\"12\" r=\"4\"/><path d=\"M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4\"/></svg>`,Me=b`<svg viewBox"
"=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z\"/></svg>`,xe=b`"
"<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2.2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M6 9l6 6 6-6\"/></svg>`;function lt({series:t,m"
"ax:e,fmt:n=_=>Math.round(_),unit:s=\"%\",w:l=260,hgt:o=46}){let[_,c]=A(-1),u=l/(F-1),i=a=>o-Math.min(a,e)/e*(o-4)-2,d=t.reduce((a,g)=>Math.max(a,(g.data||[]).length),0),h=a=>{let g=a"
".currentTarget.getBoundingClientRect();if(!g.width||d<1)return;let C=Math.min(1,Math.max(0,(a.clientX-g.left)/g.width)),m=Math.round(C*(F-1));m>d-1&&(m=d-1),m<0&&(m=0),c(m)},r=_>=0"
"?_/(F-1)*100:0;return b`\n    <div class=\"spark-wrap\" style=${\"height:\"+o+\"px\"}>\n      <svg class=\"spark\" style=${\"height:\"+o+\"px\"} viewBox=${\"0 0 \"+l+\" \"+o} preserveAspectRatio=\"no"
"ne\"\n           onMouseMove=${h} onMouseLeave=${()=>c(-1)}>\n        ${t.map((a,g)=>{let C=a.data;if(!C||C.length<2)return null;let m=C.map((U,p)=>[+(p*u).toFixed(1),+i(U).toFixed(1)"
"]),y=m.map((U,p)=>(p?\"L\":\"M\")+U[0]+\",\"+U[1]).join(\" \"),f=y+\" L\"+m[m.length-1][0]+\",\"+o+\" L\"+m[0][0]+\",\"+o+\" Z\",w=\"g\"+g+\"_\"+a.color.replace(\"#\",\"\");return b`\n            <defs>\n    "
"          <linearGradient id=${w} x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">\n                <stop offset=\"0%\" stop-color=${a.color} stop-opacity=\"0.35\" />\n                <stop offset=\"100%\" st"
"op-color=${a.color} stop-opacity=\"0\" />\n              </linearGradient>\n            </defs>\n            <path d=${f} fill=${\"url(#\"+w+\")\"} />\n            <path d=${y} fill=\"none\" s"
"troke=${a.color} stroke-width=\"2\"\n                  stroke-linejoin=\"round\" stroke-linecap=\"round\" />`})}\n      </svg>\n      ${_>=0&&b`<div class=\"spark-guide\" style=${\"left:\"+r+\"%"
"\"}></div>`}\n      ${_>=0&&t.map(a=>a.data&&a.data[_]!=null?b`<div class=\"spark-dot\" style=${\"left:\"+r+\"%;top:\"+i(a.data[_])/o*100+\"%;background:\"+a.color}></div>`:null)}\n      ${_>"
"=0&&b`<div class=\"spark-tip\" style=${\"left:\"+r+\"%\"}>\n        ${t.map(a=>a.data&&a.data[_]!=null?b`<span style=${\"color:\"+a.color}>${(a.label?a.label+\" \":\"\")+n(a.data[_])+s}</span>`"
":null)}\n      </div>`}\n    </div>`}var it=({label:t,value:e,sub:n})=>b`\n  <div class=\"metric\">\n    <span class=\"metric-label\">${t}</span>\n    <span class=\"metric-value\">${e}${n&&b`"
"<span class=\"metric-sub\">${n}</span>`}</span>\n  </div>`;function ne({pct:t,tone:e}){let n=Math.max(0,Math.min(100,t));return b`<div class=\"bar\"><div class=${\"bar-fill tone-\"+e} sty"
"le=${\"width:\"+n+\"%\"}></div></div>`}function yt({label:t,value:e,accent:n}){return b`\n    <div class=\"tile\">\n      <div class=${\"tile-value\"+(n?\" accent\":\"\")}>${e}</div>\n      <div "
"class=\"tile-label\">${t}</div>\n    </div>`}function Pe({node:t,hist:e,expanded:n,onToggle:s,onMute:l,onUnmute:o,onVolume:_,onReset:c,onReboot:u}){let[i,d]=A(t.volume),h=j(!1);V(()=>"
"{h.current||d(t.volume)},[t.volume]);let r=t.battery_pct>50?\"good\":t.battery_pct>20?\"warn\":\"bad\",a=ee(t.heap_free)/me*100,g=t.packet_loss_rate*100,C=\"background:linear-gradient(90d"
"eg,var(--accent) \"+i+\"%,var(--line) \"+i+\"%)\",m=R=>R.stopPropagation(),[y,f]=A(\"loss\"),w=y===\"loss\",U=t.delivery_ratio*100,p=w?e.loss:e.delivery,v=w?$e:ke,P=w?g:U,E=R=>b`\n    <div c"
"lass=\"chart\">\n      <div class=\"chart-head\">\n        <div class=\"seg\" onClick=${m}>\n          <button class=${\"seg-btn\"+(w?\" on\":\"\")} onClick=${()=>f(\"loss\")}>Loss</button>\n       "
"   <button class=${\"seg-btn\"+(w?\"\":\" on\")} onClick=${()=>f(\"delivery\")}>Delivery</button>\n        </div>\n        <span class=\"legend\"><b style=${\"color:\"+v}>${P.toFixed(1)}%</b></s"
"pan>\n      </div>\n      ${lt({max:100,hgt:R,fmt:q=>q.toFixed(1),series:[{data:p,color:v}]})}\n    </div>`,S=b`\n    <div class=\"card-head\">\n      <div class=\"card-title\"><span class="
"\"node-dot\"></span>Node ${t.node_id}</div>\n      <div class=\"head-right\">\n        <span class=${\"pill \"+(t.masking_active?\"pill-on\":\"pill-off\")}>\n          <span class=\"pill-dot\"></"
"span>${t.masking_active?\"Masking\":\"Silent\"}\n        </span>\n        <span class=${\"chev\"+(n?\" open\":\"\")}>${xe}</span>\n      </div>\n    </div>\n    <div class=\"mac\">${t.mac}</div>`,$"
"=b`\n    <div class=\"metrics\">\n      ${it({label:\"Volume\",value:t.volume+\"%\"})}\n      ${it({label:\"Battery\",value:t.battery_pct+\"%\"})}\n      ${it({label:\"Uptime\",value:ye(t.uptime_s"
")})}\n      ${it({label:\"Delivery\",value:Math.round(t.delivery_ratio*100)+\"%\"})}\n    </div>`,T=b`\n    <div class=\"submetric\">\n      <div class=\"submetric-head\"><span>Battery</span><"
"span>${t.battery_pct}%</span></div>\n      ${ne({pct:t.battery_pct,tone:r})}\n    </div>\n    <div class=\"submetric\">\n      <div class=\"submetric-head\"><span>Free memory</span><span>$"
"{ee(t.heap_free)} KB</span></div>\n      ${ne({pct:a,tone:\"mem\"})}\n    </div>`,W=b`\n    <div class=\"slider-row\" onClick=${m}>\n      <span class=\"slider-cap\">Vol</span>\n      <input "
"type=\"range\" min=\"0\" max=\"100\" value=${i} class=\"slider\" style=${C}\n        onInput=${R=>{$t=!0,h.current=!0,d(+R.target.value)}}\n        onChange=${R=>{let q=+R.target.value;d(q),"
"_(t.node_id,q),setTimeout(()=>{$t=!1,h.current=!1},1e3)}} />\n      <span class=\"slider-val\">${i}%</span>\n    </div>`,kt=b`\n    <div class=\"actions\" onClick=${m}>\n      <button clas"
"s=\"btn btn-mute\" onClick=${()=>l(t.node_id)}>Mute</button>\n      <button class=\"btn btn-unmute\" onClick=${()=>o(t.node_id)}>Unmute</button>\n    </div>`,wt=b`\n    <div class=\"action"
"s\" onClick=${m}>\n      <button class=\"btn btn-reset\" onClick=${()=>c(t.node_id)}>Reset</button>\n      <button class=\"btn btn-reboot\"\n        onClick=${()=>{window.confirm(\"Reboot n"
"ode \"+t.node_id+\"?\")&&u(t.node_id)}}>Reboot</button>\n    </div>`,oe=b`\n    <div class=\"chart\">\n      <div class=\"chart-head\">\n        <span>CPU load</span>\n        <span class=\"leg"
"end\">\n          <b style=${\"color:\"+rt}>C0 ${t.cpu0}%</b>\n          <b style=${\"color:\"+_t}>C1 ${t.cpu1}%</b>\n        </span>\n      </div>\n      ${lt({max:100,series:[{data:e.cpu0,"
"color:rt,label:\"C0\"},{data:e.cpu1,color:_t,label:\"C1\"}]})}\n    </div>\n    ${E(46)}`,se=b`\n    <div class=\"chart\">\n      <div class=\"chart-head\"><span>CPU Core 0</span><span class=\""
"legend\"><b style=${\"color:\"+rt}>${t.cpu0}%</b></span></div>\n      ${lt({max:100,hgt:104,series:[{data:e.cpu0,color:rt}]})}\n    </div>\n    <div class=\"chart\">\n      <div class=\"char"
"t-head\"><span>CPU Core 1</span><span class=\"legend\"><b style=${\"color:\"+_t}>${t.cpu1}%</b></span></div>\n      ${lt({max:100,hgt:104,series:[{data:e.cpu1,color:_t}]})}\n    </div>\n  "
"  ${E(104)}`;return b`\n    <div class=${\"card\"+(n?\" expanded\":\"\")} data-id=${t.node_id} onClick=${s}>\n      ${S}\n      ${n?b`<div class=\"expanded-body\">\n            <div class=\"col"
"-left\">${$}${T}${W}${kt}${wt}</div>\n            <div class=\"col-right\">${se}</div>\n          </div>`:b`${$}${T}${oe}${W}${kt}${wt}`}\n    </div>`}function Se(){let[t,e]=A([]),[n,s]="
"A(null),[l,o]=A(!1),[_,c]=A(we),[u,i]=A(null),d=j({}),h=j(null),r=j(null);V(()=>{document.documentElement.dataset.theme=_;try{localStorage.setItem(\"ps-theme\",_)}catch{}},[_]);let a"
"=()=>window.matchMedia(\"(max-width:560px)\").matches,[g,C]=A(a);V(()=>{let p=window.matchMedia(\"(max-width:560px)\"),v=()=>{C(p.matches),p.matches&&i(null)};return p.addEventListener"
"(\"change\",v),()=>p.removeEventListener(\"change\",v)},[]);let m=p=>{if(g)return;let v={};h.current&&h.current.querySelectorAll(\".card\").forEach(P=>{v[P.dataset.id]=P.getBoundingClien"
"tRect()}),r.current=v,i(P=>P===p?null:p)};Xt(()=>{let p=r.current;r.current=null;let v=h.current;if(!p||!v)return;let P=[];v.querySelectorAll(\".card\").forEach(E=>{let S=p[E.dataset"
".id];if(!S)return;let $=E.getBoundingClientRect(),T=S.left-$.left,W=S.top-$.top;Math.abs(T)<1&&Math.abs(W)<1||(E.style.transition=\"none\",E.style.transform=\"translate(\"+T+\"px,\"+W+\"p"
"x)\",P.push(E))}),P.length&&(v.offsetWidth,requestAnimationFrame(()=>{P.forEach(E=>{E.style.transition=\"transform .44s cubic-bezier(.2,.7,.2,1)\",E.style.transform=\"\"})}))},[u]);let "
"y=()=>{$t||be().then(p=>{let v=Array.isArray(p)?p:[],P=d.current,E=[];v.forEach(S=>{E.push(S.node_id);let $=P[S.node_id]||(P[S.node_id]={cpu0:[],cpu1:[],loss:[],delivery:[]});$.cpu"
"0.push(S.cpu0),$.cpu0.length>F&&$.cpu0.shift(),$.cpu1.push(S.cpu1),$.cpu1.length>F&&$.cpu1.shift(),$.loss.push(S.packet_loss_rate*100),$.loss.length>F&&$.loss.shift(),$.delivery.pu"
"sh(S.delivery_ratio*100),$.delivery.length>F&&$.delivery.shift()}),Object.keys(P).forEach(S=>{E.indexOf(+S)===-1&&delete P[S]}),e(v),s(null),o(!0)}).catch(p=>{s(p.message||String(p"
")),o(!0)})};V(()=>{y();let p=setInterval(y,te);return()=>clearInterval(p)},[]);let f=p=>p().then(y).catch(v=>s(v.message||String(v))),w=t.filter(p=>p.masking_active).length,U=t.len"
"gth?Math.round(t.reduce((p,v)=>p+v.battery_pct,0)/t.length):0;return b`\n    <div class=\"app\">\n      <header class=\"topbar\">\n        <div class=\"brand\">\n          <div class=\"logo\">"
"PS</div>\n          <div>\n            <div class=\"brand-title\">Privacy Shield</div>\n            <div class=\"brand-sub\">Hub dashboard</div>\n          </div>\n        </div>\n        <d"
"iv class=\"topbar-right\">\n          <div class=${\"conn \"+(n?\"conn-bad\":\"conn-ok\")}>\n            <span class=\"conn-dot\"></span>${n?\"Disconnected\":\"Live\"}\n          </div>\n          <"
"button class=\"theme-btn\" title=\"Toggle light / dark\" aria-label=\"Toggle light / dark\"\n            onClick=${()=>c(p=>p===\"light\"?\"dark\":\"light\")}>\n            ${_===\"light\"?Me:Ce}\n"
"          </button>\n        </div>\n      </header>\n\n      <main class=\"wrap\">\n        <section class=\"tiles\">\n          ${yt({label:\"Nodes online\",value:t.length,accent:!0})}\n     "
"     ${yt({label:\"Masking\",value:w})}\n          ${yt({label:\"Avg battery\",value:t.length?U+\"%\":\"\\u2014\"})}\n        </section>\n\n        <section class=\"toolbar\">\n          <button c"
"lass=\"btn btn-danger\" onClick=${()=>f(N.globalMute)}>Mute all</button>\n          <button class=\"btn btn-success\" onClick=${()=>f(N.globalUnmute)}>Unmute all</button>\n          <but"
"ton class=\"btn btn-ghost\" onClick=${y}>Refresh</button>\n          <span class=\"toolbar-spacer\"></span>\n          <span class=\"refresh-note\">Auto-refresh \302\267 ${te/1e3}s</span>\n      "
"  </section>\n\n        ${n&&b`<div class=\"banner\">Can't reach the hub \342\200\224 ${n}</div>`}\n\n        ${l?t.length===0?b`<div class=\"empty\">\n              <div class=\"empty-title\">No mask"
"ing nodes detected</div>\n              <div class=\"empty-sub\">Power on a node to get started.</div>\n            </div>`:b`<section class=\"grid\" ref=${h}>\n              ${t.map(p=>b"
"`<${Pe}\n                key=${p.node_id}\n                node=${p}\n                hist=${d.current[p.node_id]||{cpu0:[],cpu1:[],loss:[],delivery:[]}}\n                expanded=${u="
"==p.node_id}\n                onToggle=${()=>m(p.node_id)}\n                onMute=${v=>f(()=>N.mute(v))}\n                onUnmute=${v=>f(()=>N.unmute(v))}\n                onVolume=$"
"{(v,P)=>f(()=>N.volume(v,P))}\n                onReset=${v=>f(()=>N.reset(v))}\n                onReboot=${v=>f(()=>N.reboot(v))} />`)}\n            </section>`:b`<div class=\"empty\">L"
"oading\342\200\246</div>`}\n\n        <footer class=\"foot\">Privacy Shield \302\267 ESP-NOW mesh</footer>\n      </main>\n    </div>`}It(b`<${Se} />`,document.getElementById(\"app\"));})();</script></bo"
"dy></html>";

esp_err_t dashboard_get_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
