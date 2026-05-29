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

/* -------------------------------------------------------------------------- */
/*  Packet types (what kind of message is this?)                              */
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
/*  STATUS packet (node reports to hub / neighbors)                           */
/* -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    mesh_header_t header;
    bool     masking_active;
    uint8_t  volume;         /* 0-100 */
    uint8_t  battery_pct;    /* 0-100 */
    uint32_t uptime_s;       /* seconds since boot */
} mesh_status_pkt_t;

/* -------------------------------------------------------------------------- */
/*  COMMAND packet (hub → node)                                               */
/* -------------------------------------------------------------------------- */

typedef enum {
    MESH_CMD_MUTE       = 0x01,
    MESH_CMD_UNMUTE     = 0x02,
    MESH_CMD_SET_VOLUME = 0x03,
    MESH_CMD_REBOOT     = 0x04,
} mesh_command_t;

typedef struct __attribute__((packed)) {
    mesh_header_t header;
    uint8_t  command;       /* mesh_command_t */
    uint8_t  value;         /* e.g., volume 0-100, or 0/1 for mute */
} mesh_command_pkt_t;

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
 * @param node_id  Unique ID for this node (0 = hub, 1-254 = masking nodes).
 * @return ESP_OK on success.
 */
esp_err_t mesh_init(uint8_t node_id);

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
 * @brief Get a pointer to the global mesh state (for dashboards, etc.).
 */
const mesh_state_t *mesh_get_state(void);

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

#ifdef __cplusplus
}
#endif
