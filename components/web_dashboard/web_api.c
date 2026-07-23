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
/*  URI parameter extraction                                                   */
/* -------------------------------------------------------------------------- */

static int extract_node_id(const char *uri) {
    /*
     * URI format: /api/node/{id}/mute  or  /api/node/{id}/unmute
     *                                          or  /api/node/{id}/volume
     */
    const char *prefix = "/api/node/";
    const char *start = strstr(uri, prefix);
    if (start == NULL) return -1;

    start += strlen(prefix);
    char *end = NULL;
    long id = strtol(start, &end, 10);
    if (end == start || id < 1 || id > 254) return -1;
    return (int)id;
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

esp_err_t api_node_unlock_post_handler(httpd_req_t *req) {
    int node_id = extract_node_id(req->uri);
    if (node_id < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node ID");
        return ESP_FAIL;
    }
    send_command_to_node(node_id, MESH_CMD_UNLOCK, -1); //value is not necessary here
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}


esp_err_t api_node_reboot_post_handler(httpd_req_t *req) {
    int node_id = extract_node_id(req->uri);
    if (node_id < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node ID");
        return ESP_FAIL;
    }
    send_command_to_node(node_id, MESH_CMD_REBOOT, -1); //value is not necessary here
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Dashboard HTML (minimal, responsive, auto-refreshing)                     */
/* -------------------------------------------------------------------------- */

static const char *DASHBOARD_HTML = 
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<meta http-equiv=\"Cache-Control\" content=\"no-cache,no-store,must-revalidate\">"
"<title>Privacy Shield</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font:-apple-system,BlinkMacSystemFont,sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh}"
".header{background:#1e293b;padding:16px 24px;display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #334155}"
".header h1{font-size:20px;font-weight:600}"
".header .status{font-size:13px;color:#94a3b8}"
".container{max-width:900px;margin:0 auto;padding:24px}"
".toolbar{display:flex;gap:12px;margin-bottom:24px;flex-wrap:wrap}"
"button{padding:10px 20px;border:none;border-radius:8px;cursor:pointer;font-size:14px;font-weight:500;transition:background .2s}"
".btn-global{background:#334155;color:#e2e8f0}"
".btn-global:hover{background:#475569}"
".btn-mute{background:#991b1b;color:#fff}"
".btn-mute:hover{background:#b91c1c}"
".btn-unmute{background:#166534;color:#fff}"
".btn-unmute:hover{background:#15803d}"
".btn-refresh{background:#1d4ed8;color:#fff}"
".btn-refresh:hover{background:#2563eb}"
".nodes{display:grid;gap:16px;grid-template-columns:repeat(auto-fill,minmax(280px,1fr))}"
".card{background:#1e293b;border-radius:12px;padding:20px;border:1px solid #334155}"
".card .name{font-size:16px;font-weight:600;margin-bottom:8px}"
".card .mac{font-size:11px;color:#64748b;margin-bottom:12px}"
".card .row{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;font-size:13px}"
".card .row .label{color:#94a3b8}"
".card .row .val{font-weight:500}"
".card .badge{display:inline-block;padding:2px 8px;border-radius:12px;font-size:11px;font-weight:600}"
".badge-on{background:#166534;color:#4ade80}"
".badge-off{background:#334155;color:#94a3b8}"
".card .actions{margin-top:14px;display:flex;gap:8px}"
".card .actions button{padding:6px 14px;font-size:12px}"
".vol-slider{width:100%;margin-top:8px;accent-color:#3b82f6}"
".vol-row{display:flex;align-items:center;gap:8px;font-size:12px;margin-top:4px}"
".empty{text-align:center;color:#64748b;padding:60px 0;font-size:15px;grid-column:1/-1}"
".footer{text-align:center;padding:24px;color:#475569;font-size:12px}"
".chart-box{margin-top:12px;padding-top:10px;border-top:1px solid #334155}"
".chart-title{font-size:11px;color:#94a3b8;margin-bottom:4px;display:flex;justify-content:space-between}"
".svg-chart{width:100%;height:50px;background:#0f172a;border-radius:4px;overflow:hidden}"
"</style>"
"</head>"
"<body>"
"<div class=\"header\">"
"<div><h1>Privacy Shield</h1></div>"
"<div class=\"status\">Hub • <span id=\"nodeCount\">0</span> nodes</div>"
"</div>"
"<div class=\"container\">"
"<div class=\"toolbar\">"
"<button class=\"btn-mute\" onclick=\"globalMute()\">Mute All</button>"
"<button class=\"btn-unmute\" onclick=\"globalUnmute()\">Unmute All</button>"
"<button class=\"btn-refresh\" onclick=\"refresh()\">Refresh</button>"
"</div>"
"<div id=\"error\" style=\"display:none;background:#7f1d1d;color:#fca5a5;padding:10px 16px;border-radius:8px;margin-bottom:16px;font-size:13px\"></div>"
"<div id=\"nodes\" class=\"nodes\"></div>"
"<div class=\"footer\">Auto-refreshes every 5s</div>"
"</div>"
"<script>"
"var errEl=document.getElementById('error');"
"var refreshCount=0;"
"window.dragging=false;"
"var historyData={};"
"var MAX_PTS=15;"

"function showErr(e){errEl.style.display='block';errEl.textContent='Error: '+(e.message||e);}"
"function hideErr(){errEl.style.display='none'}"
"function fmtUptime(s){var h=Math.floor(s/3600),m=Math.floor((s%3600)/60);return h+'h '+m+'m'}"

"function makeSvgPath(arr,maxVal){"
"if(!arr||arr.length<2)return '';"
"var w=260,h=50;"
"var step=w/(MAX_PTS-1);"
"return arr.map(function(v,i){"
"var x=(i*step).toFixed(1);"
"var y=(h-(Math.min(v,maxVal)/maxVal)*h).toFixed(1);"
"return (i===0?'M':'L')+x+','+y;"
"}).join(' ');"
"}"

"function render(nodes){"
"if(window.dragging) return;"
"hideErr();refreshCount++;"
"document.getElementById('nodeCount').textContent=nodes.length;"
"var container=document.getElementById('nodes');"
"if(!nodes.length){container.innerHTML='<div class=empty>No masking nodes detected.<br>Power on a node to get started.</div>';historyData={};return}"

"var activeIds=[];"
"nodes.forEach(function(n){"
"activeIds.push(n.node_id);"
"if(!historyData[n.node_id]) historyData[n.node_id]={cpu0:[],cpu1:[],loss:[]};"
"var h=historyData[n.node_id];"
"h.cpu0.push(n.cpu0); if(h.cpu0.length>MAX_PTS) h.cpu0.shift();"
"h.cpu1.push(n.cpu1); if(h.cpu1.length>MAX_PTS) h.cpu1.shift();"
"h.loss.push(n.packet_loss_rate); if(h.loss.length>MAX_PTS) h.loss.shift();"

"var cardEl=document.getElementById('node-'+n.node_id);"
"var badge=n.masking_active?'<span class=\"badge badge-on\">MASKING</span>':'<span class=\"badge badge-off\">SILENT</span>';"

"var cpu0Path=makeSvgPath(h.cpu0,100);"
"var cpu1Path=makeSvgPath(h.cpu1,100);"
"var lossPath=makeSvgPath(h.loss,5);"

"if(!cardEl){"
"cardEl=document.createElement('div');"
"cardEl.className='card';"
"cardEl.id='node-'+n.node_id;"
"container.appendChild(cardEl);"
"}"

"cardEl.innerHTML='<div class=name>Node '+n.node_id+' '+badge+'</div>'"
"+'<div class=mac>'+n.mac+'</div>'"
"+'<div class=row><span class=label>Volume</span><span class=val>'+n.volume+'%</span></div>'"
"+'<div class=row><span class=label>Battery</span><span class=val>'+n.battery_pct+'%</span></div>'"
"+'<div class=row><span class=label>Memory</span><span class=val>'+parseInt(n.heap_free/1024)+' / 7822 KB</span></div>'"
"+'<div class=row><span class=label>Uptime</span><span class=val>'+fmtUptime(n.uptime_s)+'</span></div>'"

"+'<div class=chart-box>'"
"+'<div class=chart-title><span>CPU Utilization</span><span style=\"color:#3b82f6\">CPU0: '+n.cpu0+'%</span> <span style=\"color:#a855f7\">CPU1: '+n.cpu1+'%</span></div>'"
"+'<svg class=svg-chart viewBox=\"0 0 260 50\">'"
"+'<path d=\"'+cpu0Path+'\" fill=\"none\" stroke=\"#3b82f6\" stroke-width=\"2\"/>'"
"+'<path d=\"'+cpu1Path+'\" fill=\"none\" stroke=\"#a855f7\" stroke-width=\"2\"/>'"
"+'</svg></div>'"

"+'<div class=chart-box>'"
"+'<div class=chart-title><span>Packet Loss</span><span style=\"color:#ef4444\">PL: '+n.packet_loss_rate.toFixed(2)+'%</span></div>'"
"+'<svg class=svg-chart viewBox=\"0 0 260 50\">'"
"+'<path d=\"'+lossPath+'\" fill=\"none\" stroke=\"#ef4444\" stroke-width=\"2\"/>'"
"+'</svg></div>'"

"+'<div class=vol-row><span>Vol</span><input type=range min=0 max=100 value='+n.volume+' class=vol-slider data-node-id='+n.node_id+'><span>'+n.volume+'%</span></div>'"
"+'<div class=actions>'"
"+'<button class=btn-mute onclick=\"muteNode('+n.node_id+')\">Mute</button>'"
"+'<button class=btn-unmute onclick=\"unmuteNode('+n.node_id+')\">Unmute</button>'"
"+'</div>';"
"});"

"var allCards=container.querySelectorAll('.card');"
"allCards.forEach(function(c){"
"var id=parseInt(c.id.replace('node-',''));"
"if(activeIds.indexOf(id)===-1){c.remove();delete historyData[id];}"
"});"

"document.querySelectorAll('.vol-slider').forEach(function(slider){"
"slider.addEventListener('input',function(){"
"window.dragging=true;"
"this.nextSibling.textContent=this.value+'%';"
"});"
"slider.addEventListener('change',function(){"
"var nodeId=this.getAttribute('data-node-id');"
"setVolume(nodeId,this.value);"
"});"
"});"
"}"

"function refresh(){fetch('/api/nodes').then(function(r){return r.json()}).then(render).catch(showErr)}"
"function muteNode(id){fetch('/api/node/mute?id='+id,{method:'POST'}).then(refresh).catch(showErr)}"
"function unmuteNode(id){fetch('/api/node/unmute?id='+id,{method:'POST'}).then(refresh).catch(showErr)}"
"function setVolume(id,v){window.dragging=true;fetch('/api/node/volume?level='+v+'&id='+id,{method:'POST'})"
".then(function(){setTimeout(function(){window.dragging=false;},1000);})"
".catch(function(e){window.dragging=false;showErr(e);});}"
"function globalMute(){fetch('/api/global/mute',{method:'POST'}).then(refresh).catch(showErr)}"
"function globalUnmute(){fetch('/api/global/unmute',{method:'POST'}).then(refresh).catch(showErr)}"
"refresh();setInterval(refresh,5000)"
"</script>"
"</body>"
"</html>";

esp_err_t dashboard_get_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
