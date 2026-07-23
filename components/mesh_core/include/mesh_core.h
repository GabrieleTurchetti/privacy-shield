#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Constants                                                                 */
/* -------------------------------------------------------------------------- */

/** Maximum number of neighbors a node tracks. */
#define MESH_MAX_NEIGHBORS         16

/** How long (ms) without a HELLO before a neighbor is considered gone. */
#define MESH_NEIGHBOR_TIMEOUT_MS   30000

/** Broadcast MAC address — all nodes receive. */
#define MESH_BROADCAST_MAC         {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}

/** Maximum payload size per ESP-NOW packet (ESP-NOW limit is 250 bytes). */
#define MESH_PAYLOAD_MAX           250

/** HELLO broadcast interval (ms). Also sets how fast a scanning node can
 *  discover the hub's channel — a node must dwell at least this long on the
 *  hub's channel to catch a HELLO, so keep it short enough for discovery. */
#define MESH_HELLO_INTERVAL_MS     3000

/** Node channel-scan: hub's src_id, dwell per channel, sweep range, and the
 *  fallback channel used if no hub is found (so hub-less nodes still agree). */
#define MESH_HUB_SRC_ID            0
#define MESH_CHANNEL_SCAN_DWELL_MS (MESH_HELLO_INTERVAL_MS + 300)
#define MESH_CHANNEL_MAX           13
#define MESH_CHANNEL_DEFAULT       1

/** Re-scan / hub-loss recovery. Once locked, a node checks every
 *  MESH_CHANNEL_MONITOR_INTERVAL_MS whether it still hears the hub; if the hub
 *  goes silent for MESH_HUB_LOST_TIMEOUT_MS (moved channel, rebooted, powered
 *  off) it re-enters scanning. If a full sweep finds no hub, the node parks on
 *  the default channel and waits an exponentially growing backoff (MIN..MAX,
 *  doubling each failed sweep, reset on lock) before trying again. */
#define MESH_HUB_LOST_TIMEOUT_MS         60000
#define MESH_CHANNEL_MONITOR_INTERVAL_MS 5000
#define MESH_RESCAN_BACKOFF_MIN_MS       5000
#define MESH_RESCAN_BACKOFF_MAX_MS       300000

/* -------------------------------------------------------------------------- */
/*  Packet types                                                              */
/* -------------------------------------------------------------------------- */

typedef enum {
    MESH_PKT_HELLO      = 0x01,   /* "I'm here" — broadcast on boot + periodic */
    MESH_PKT_STATUS     = 0x02,   /* Node status update (masking state, battery, etc.) */
    MESH_PKT_COMMAND    = 0x03,   /* Hub → node command (mute, volume, etc.) */
    MESH_PKT_ACK        = 0x04,   /* Acknowledgement */
} mesh_packet_type_t;

/* -------------------------------------------------------------------------- */
/*  Common packet header (every packet starts with this)                      */
/* -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  type;          /* mesh_packet_type_t */
    uint8_t  src_id;        /* Sending node's ID (0 = hub, 1-254 = masking nodes) */
    uint32_t timestamp_ms;  /* FreeRTOS tick in ms (for latency measurements) */
} mesh_header_t;

/* -------------------------------------------------------------------------- */
/*  HELLO packet (broadcast periodically)                                      */
/* -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    mesh_header_t header;
    /* Future: firmware version, capabilities bitmap, battery level */
} mesh_hello_pkt_t;

/* -------------------------------------------------------------------------- */
/*  Delay metrics (end-to-end / attack / release), min / max / avg            */
/*  Plain transport struct — the node fills it via status_task_params.        */
/* -------------------------------------------------------------------------- */

typedef struct {
    float   e2e_avg,   e2e_min,   e2e_max;      /* mic→speaker delay (ms) */
    float   attack_avg; int16_t attack_min, attack_max;  /* masking attack delay */
    float   release_avg; int16_t release_min, release_max; /* masking release delay */
} delay_metrics_t;

/* -------------------------------------------------------------------------- */
/*  STATUS packet (node reports to hub / neighbors)                           */
/* -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    mesh_header_t header;
    bool     masking_active;
    uint8_t  volume;         /* 0-100 */
    uint8_t  battery_pct;    /* 0-100 */
    float delivery_ratio;  /* ratio of ACKs received vs STATUS sent (0.0-1.0) */
    float packet_loss_rate; /* (0.0-1.0) */
    uint32_t uptime_s;       /* seconds since boot */
    uint8_t  cpu0_utilization; /* CPU0 utilization percentage (0-100) */
    uint8_t  cpu1_utilization; /* CPU1 utilization percentage (0-100) */
    uint32_t heap_free;        /* Free heap size in bytes */
    uint32_t heap_largest_block; /* Largest free heap block in bytes */
    /* Delay KPIs (ms). avg = rolling mean, min/max = running extremes. */
    float e2e_avg,   e2e_min,   e2e_max;
    float attack_avg;  int16_t attack_min,  attack_max;
    float release_avg; int16_t release_min, release_max;
} mesh_status_pkt_t;

//Used to pass web dashboard refresh method
typedef void (*mesh_status_callback_t)(const mesh_status_pkt_t *status, const uint8_t *mac);

/* -------------------------------------------------------------------------- */
/*  ACK packet (only for status messages)                                     */
/* -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    mesh_header_t header;
    uint32_t ack_timestamp_ms;    // which STATUS this ACK is for
} mesh_ack_pkt_t;

/* -------------------------------------------------------------------------- */
/*  COMMAND packet (hub → node)                                               */
/* -------------------------------------------------------------------------- */

typedef enum {
    MESH_CMD_MUTE        = 0x01,
    MESH_CMD_UNMUTE      = 0x02,
    MESH_CMD_SET_VOLUME  = 0x03,
    MESH_CMD_REBOOT      = 0x04,
    MESH_CMD_UNLOCK      = 0x05,
} mesh_command_t;

typedef struct __attribute__((packed)) {
    mesh_header_t header;
    uint8_t  command;       /* mesh_command_t */
    uint8_t  value;         /* e.g., volume 0-100, or 0/1 for mute */
} mesh_command_pkt_t;


typedef struct {
    void (*set_volume)(uint8_t);
    void (*set_masking)(uint8_t);
    void (*unlock)(void);
    void (*set_volume_percentage)(uint8_t);
} volume_command_cb;

/* -------------------------------------------------------------------------- */
/*  Neighbor record                                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  mac[ESP_NOW_ETH_ALEN];  /* MAC address */
    uint8_t  node_id;                /* Node ID */
    uint32_t last_heard_ms;          /* FreeRTOS tick of last message received */
    bool     active;                 /* Is this slot in use? */
} mesh_neighbor_t;

/* -------------------------------------------------------------------------- */
/*  Global mesh state                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    mesh_neighbor_t neighbors[MESH_MAX_NEIGHBORS];
    uint8_t         my_id;           /* This node's ID */
    uint8_t         my_mac[ESP_NOW_ETH_ALEN];
    bool            initialized;
} mesh_state_t;

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize ESP-NOW and start the mesh.
 *
 * @param wifi_mode  WiFi mode for the underlying radio (WIFI_MODE_STA for nodes,
 * @param status_cb pass web dashboard refresh function
 *                   WIFI_MODE_AP for the Hub). ESP-NOW coexists with either.
 * @return ESP_OK on success.
 */
esp_err_t mesh_init(wifi_mode_t wifi_mode, mesh_status_callback_t status_cb, volume_command_cb *command_cb);


/**
 * @brief Send a raw payload to a specific MAC address.
 *
 * @param mac   Destination MAC (use a broadcast MAC array for broadcast).
 * @param data  Payload buffer.
 * @param len   Payload length (max MESH_PAYLOAD_MAX).
 * @return ESP_OK on success.
 */
esp_err_t mesh_send(const uint8_t *mac, const void *data, size_t len);

/**
 * @brief Broadcast a packet to all nodes.
 */
esp_err_t mesh_broadcast(const void *data, size_t len);

/**
 * @brief Broadcast a HELLO packet (call periodically, e.g. every 10s).
 */
esp_err_t mesh_send_hello(void);

/**
 * @brief Send ACK packet (ACK are sent only for status messages).
 * 
 * @param mac   Destination MAC (the node that sent the STATUS).
 */
esp_err_t mesh_send_status(void *arg);

/**
 * @brief Send ACK packet (ACK are sent only for status messages).
 *
 * @param mac        Destination MAC (the node that sent the STATUS).
 * @param status_ts  timestamp_ms of the STATUS being acknowledged (echoed back
 *                   so the sender can match the ACK to that specific STATUS).
 */
esp_err_t mesh_send_ack(const uint8_t *mac, uint32_t status_ts);

/**
 * @brief Get a pointer to the global mesh state (for dashboards, etc.).
 */
const mesh_state_t *mesh_get_state(void);

/**
 * @brief Lock the mesh state mutex.
 *
 * Must be held while reading/writing the neighbor table or other mesh state.
 * Always pair with mesh_unlock().
 */
void mesh_lock(void);

/**
 * @brief Unlock the mesh state mutex.
 */
void mesh_unlock(void);

/**
 * @brief Callback type: fired when a packet is received.
 * @param src_mac  Source MAC address.
 * @param data     Packet payload (starts with mesh_header_t).
 * @param len      Payload length.
 */
typedef void (*mesh_recv_callback_t)(const uint8_t *src_mac, const void *data, size_t len);

/**
 * @brief Register a callback for received packets.
 *
 * Only one callback can be registered at a time (call again to replace).
 */
void mesh_register_recv_callback(mesh_recv_callback_t cb);

/* -------------------------------------------------------------------------- */
/*  Node Discovery (node_discovery.c)                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Record that we heard from a MAC address (updates neighbor table).
 *
 * Called internally by the ESP-NOW receive callback. Also callable externally
 * for testing or manual registration.
 */
void mesh_discovery_heard(const uint8_t *mac, uint8_t node_id);

/**
 * @brief Remove neighbors that haven't been heard from within the timeout.
 *
 * Call periodically (e.g., every 1-2 seconds) to clean up stale entries.
 */
void mesh_discovery_prune(void);

/**
 * @brief Return the number of currently active neighbors.
 */
int mesh_discovery_count(void);

/**
 * @brief Look up a neighbor by MAC address.
 *
 * @return Pointer to the neighbor record, or NULL if not found.
 */
const mesh_neighbor_t *mesh_discovery_find_mac(const uint8_t *mac);

/**
 * @brief This method returns the node id of the device
 *
 * @return node id identifier
 */
uint8_t get_node_id();


/* -------------------------------------------------------------------------- */
/*  TASKS                                                                     */
/* -------------------------------------------------------------------------- */


/** --------------------------------------------------------------------------
 * @brief Hello task — broadcast our presence every 10 seconds                      
* -------------------------------------------------------------------------- */

void hello_task(void *arg);


/** -------------------------------------------------------------------------- 
* @brief Status task — Broadcast status of the node, like masking, volume etc
* @param pass arg as struct status_task_params_t to arg
* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  node_id;
    bool   (*is_speech)(void); //is speech
    uint8_t (*get_volume)(void); //get volume
    uint8_t (*get_battery)(void); //get battery
    uint8_t (*get_volume_percentage)(void); //get volume percentage
    void (*update_system_metrics)(void); //update system metrics
    uint8_t (*get_cpu0_utilization)(void); //get CPU0 utilization
    uint8_t (*get_cpu1_utilization)(void); //get CPU1 utilization
    uint32_t (*get_heap_free)(void); //get free heap size
    uint32_t (*get_heap_largest_block)(void); //get largest free heap block
    void (*get_delays)(delay_metrics_t *out); //fill e2e/attack/release min/max/avg
} status_task_params_t;

void status_task(void *arg);


/** -------------------------------------------------------------------------- 
* @brief Prune task — clean up timed-out neighbors every 10 seconds           
* -------------------------------------------------------------------------- */

void prune_task(void *arg);


/** --------------------------------------------------------------------------
* @brief Channel keeper task (NODE ONLY) — a persistent state machine that
*        keeps the node's radio on the hub's WiFi channel.
*
*        SCANNING: hop channels until we hear the hub (src_id MESH_HUB_SRC_ID),
*                  then lock the radio to that channel.
*        LOCKED:   stay put; every MESH_CHANNEL_MONITOR_INTERVAL_MS check that
*                  the hub is still heard. If it goes silent for
*                  MESH_HUB_LOST_TIMEOUT_MS, drop back to SCANNING.
*        BACKOFF:  if a full sweep finds no hub, park on MESH_CHANNEL_DEFAULT
*                  (so hub-less nodes still converge) and wait an exponentially
*                  growing delay before sweeping again.
*
*        Runs forever (never self-deletes). Masking audio is unaffected by
*        scanning — only mesh coordination pauses briefly. Spawn only on nodes,
*        after mesh_init(); the hub's channel is fixed by its router.
* -------------------------------------------------------------------------- */

void mesh_channel_scan_task(void *arg);

/** @brief True while the node is currently locked onto the hub's channel. */
bool mesh_channel_is_locked(void);


#ifdef __cplusplus
}
#endif
