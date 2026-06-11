# ESP-NOW Mesh Core — Implementation Guide

> **Component:** `mesh_core`  
> **Sprint:** 1  
> **ESP-IDF:** v5.5.4  
> **Protocol:** ESP-NOW (IEEE 802.11 action frames, connectionless)  

---

## Overview

The `mesh_core` component provides a lightweight, decentralized communication layer for Privacy Shield nodes. It is built on **ESP-NOW** — a peer-to-peer wireless protocol that operates directly at the Wi-Fi link layer, requiring no access point, no IP stack, and no pairing handshake.

Each node broadcasts periodic HELLO packets, discovers neighbors automatically, and can exchange status updates and commands. There is no master, no leader election, and no single point of failure.

---

## Architecture

```
┌─────────────────────────────────────┐
│            app_main()               │
│  ┌──────────────┐  ┌─────────────┐  │
│  │  hello_task  │  │  prune_task │  │
│  │  (every 10s)  │  │  (every 2s) │  │
│  └──────┬───────┘  └──────┬──────┘  │
│         │                 │         │
│  ┌──────▼─────────────────▼──────┐  │
│  │         mesh_core             │  │
│  │  ┌───────────────────────┐    │  │
│  │  │   esp_now_link.c      │    │  │
│  │  │   • init / send / recv│    │  │
│  │  │   • WiFi PHY control  │    │  │
│  │  │   • callback dispatch │    │  │
│  │  └───────────┬───────────┘    │  │
│  │              │                │  │
│  │  ┌───────────▼───────────┐    │  │
│  │  │  node_discovery.c     │    │  │
│  │  │  • neighbor table     │    │  │
│  │  │  • timeout/prune      │    │  │
│  │  │  • lookup/count       │    │  │
│  │  └───────────────────────┘    │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
```

### Files

| File | Purpose |
|---|---|
| `include/mesh_core.h` | Public API, packet structs, constants |
| `esp_now_link.c` | ESP-NOW init, send, receive, WiFi management |
| `node_discovery.c` | Neighbor table, timeout management |
| `CMakeLists.txt` | Build dependencies |

**Log tags:** All TAG strings are defined in `main/include/log_tags.h`. See `docs/Logging_Guide.md` for log level control.

---

## Getting Started

### 1. Include the component

In your `main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mesh_core)
```

### 2. Initialize in `app_main()`

```c
#include "mesh_core.h"
#include "global_config.h"

void app_main(void) {
    // Initialize mesh with this node's ID
    ESP_ERROR_CHECK(mesh_init(DEFAULT_NODE_ID));

    // Optional: register a packet handler
    mesh_register_recv_callback(my_packet_handler);

    // Start periodic tasks
    xTaskCreate(hello_task, "hello", 2048, NULL, 1, NULL);
    xTaskCreate(prune_task, "prune", 4096, NULL, 1, NULL);
}
```

### 3. Flash two devices

Change `DEFAULT_NODE_ID` to `1` on device A, `2` on device B. Both devices will discover each other automatically within 5 seconds.

---

## API Reference

### Initialization

```c
esp_err_t mesh_init(uint8_t node_id);
```

Initializes NVS, Wi-Fi (station mode), and ESP-NOW. Registers internal send/receive callbacks. Adds the broadcast MAC as a peer. Sends an initial HELLO packet.

- `node_id`: 0 = Hub, 1–254 = masking node.
- Returns `ESP_OK` on success.

**What happens under the hood:**

1. `nvs_flash_init()` — NVS for Wi-Fi calibration data
2. `esp_netif_init()` + `esp_event_loop_create_default()` — TCP/IP stack bootstrap
3. `esp_wifi_init()` + `esp_wifi_set_mode(WIFI_MODE_STA)` — radio on, no AP association
4. `esp_now_init()` — ESP-NOW protocol ready
5. `esp_now_register_send_cb()` / `esp_now_register_recv_cb()` — callback registration
6. `esp_now_add_peer(broadcast)` — enable broadcast sends

### Sending Data

```c
esp_err_t mesh_send(const uint8_t *mac, const void *data, size_t len);
esp_err_t mesh_broadcast(const void *data, size_t len);
esp_err_t mesh_send_hello(void);
```

- **`mesh_send`**: Unicast to a specific MAC. Max `len` = 250 bytes (ESP-NOW limit).
- **`mesh_broadcast`**: Send to `FF:FF:FF:FF:FF:FF`. All nodes on the same channel receive.
- **`mesh_send_hello`**: Convenience — broadcasts a `mesh_hello_pkt_t` with the node's ID and timestamp.

> **Channel:** All nodes use the same Wi-Fi channel (set during `esp_wifi_init`). ESP-NOW peer channel is set to `0` = "use current channel."

### Receiving Data

```c
typedef void (*mesh_recv_callback_t)(const uint8_t *src_mac,
                                     const void *data, size_t len);
void mesh_register_recv_callback(mesh_recv_callback_t cb);
```

Register one callback to handle all incoming packets. The internal ESP-NOW receive callback already updates the neighbor table before dispatching to your handler.

### Neighbor Discovery

```c
void mesh_discovery_heard(const uint8_t *mac, uint8_t node_id);
void mesh_discovery_prune(void);
int  mesh_discovery_count(void);
const mesh_neighbor_t *mesh_discovery_find_mac(const uint8_t *mac);
```

- **`mesh_discovery_heard`**: Called automatically by the receive callback. Manual calls are safe for testing.
- **`mesh_discovery_prune`**: Remove neighbors not heard from in `MESH_NEIGHBOR_TIMEOUT_MS` (30 seconds). Call periodically.
- **`mesh_discovery_count`**: Return number of active neighbors.
- **`mesh_discovery_find_mac`**: Look up a neighbor by MAC. Returns `NULL` if not found.

### State Access

```c
const mesh_state_t *mesh_get_state(void);
```

Returns a read-only pointer to the global mesh state, including the full neighbor table.

---

## Packet Format

Every packet begins with a 6-byte header:

```
Byte 0      1          2  3  4  5
┌──────┬──────┬───────────────────┐
│ type │src_id│   timestamp_ms    │
│ (u8) │ (u8) │      (u32)       │
└──────┴──────┴───────────────────┘
```

### Packet Types

| Type | Value | Struct | Direction |
|---|---|---|---|
| `MESH_PKT_HELLO` | 0x01 | `mesh_hello_pkt_t` | Broadcast |
| `MESH_PKT_STATUS` | 0x02 | `mesh_status_pkt_t` | Node → Hub/Neighbors |
| `MESH_PKT_COMMAND` | 0x03 | `mesh_command_pkt_t` | Hub → Node |
| `MESH_PKT_ACK` | 0x04 | (reserved) | Any |

### HELLO Packet (10 bytes)

```
┌────────┬────────┬──────────┐
│ header │        │
│ 6 bytes│ (empty)│
└────────┴────────┘
```

### STATUS Packet (14 bytes)

```
┌────────┬───────┬──────┬─────────┬──────────┐
│ header │masking│volume│ battery │ uptime_s │
│ 6 bytes│ (bool)│ (u8) │  (u8)   │  (u32)   │
└────────┴───────┴──────┴─────────┴──────────┘
```

### COMMAND Packet (8 bytes)

```
┌────────┬─────────┬───────┐
│ header │ command │ value │
│ 6 bytes│  (u8)   │ (u8)  │
└────────┴─────────┴───────┘
```

Commands: `MUTE (0x01)`, `UNMUTE (0x02)`, `SET_VOLUME (0x03)`, `REBOOT (0x04)`.

---

## Configuration Constants

| Constant | Value | Description |
|---|---|---|
| `MESH_MAX_NEIGHBORS` | 16 | Max entries in neighbor table |
| `MESH_NEIGHBOR_TIMEOUT_MS` | 30000 | Inactivity before neighbor is pruned |
| `MESH_PAYLOAD_MAX` | 250 | ESP-NOW maximum payload (bytes) |

---

## Task Model

The reference `main.c` spawns two FreeRTOS tasks:

| Task | Stack | Period | Purpose |
|---|---|---|---|
| `hello_task` | 2048 | 10s | Broadcast HELLO packets |
| `prune_task` | 4096 | 2s | Clean up timed-out neighbors, log count |

Both run at priority 1 (idle is 0, `app_main` is 1). The ESP-NOW callbacks run in the Wi-Fi task context (high priority) — keep callback code fast and non-blocking.

---

## ESP-IDF v5.5.4 Notes

### Send callback

This component uses the v1 send callback API — in ESP-IDF v5.5.4, `esp_now_send_info_t` is a typedef for `wifi_tx_info_t`:

```c
esp_now_register_send_cb(espnow_send_cb);
```

The callback receives a `const esp_now_send_info_t *` struct that includes the destination MAC (`des_addr`), enabling per-packet send status logging.

### Wi-Fi initialization

ESP-NOW requires the Wi-Fi radio but not the IP stack. However, ESP-IDF v5.x requires:

```c
esp_netif_init();
esp_event_loop_create_default();
```

before `esp_wifi_init()`. These are handled inside `wifi_init()`.

### Power management

`WIFI_PS_MIN_MODEM` keeps the modem awake. ESP-NOW cannot operate reliably with modem sleep enabled — packets would be lost during sleep cycles.

---

## Testing

### Two-node test

1. Flash device A with `DEFAULT_NODE_ID=1`
2. Flash device B with `DEFAULT_NODE_ID=2`
3. Power both devices
4. Monitor serial output:

```
I (1234) mesh_core: Mesh initialized — node_id=1, MAC=aa:bb:cc:dd:ee:01
I (1234) main: Mesh node ready — listening for ESP-NOW packets
I (6234) discovery: New neighbor: node_id=2, MAC=aa:bb:cc:dd:ee:02
I (7234) main: HELLO from node 2 (aa:bb:cc:dd:ee:02)
I (9234) main: 1 neighbor(s) online
```

### Latency test

The `timestamp_ms` field in `mesh_header_t` records the sender's millisecond tick at send time. Subtract from the receiver's tick at receive time to measure round-trip latency. Expect < 5ms for ESP-NOW on the same channel.

### Stress test

With 3+ nodes broadcasting HELLO every 10s, memory usage is:
- Neighbor table: 16 × `sizeof(mesh_neighbor_t)` = 16 × 16 = 256 bytes
- Mesh state: ~300 bytes total
- Task stacks: ~4KB total

All well within the ESP32-S3's 512KB SRAM.

---

## Future Extensions

- **Encryption**: Add PMK/LMK keys via `esp_now_set_pmk()` for authenticated mesh
- **Node ID assignment**: Auto-assign IDs via DHCP-like negotiation
- **RSSI tracking**: Store signal strength per neighbor for proximity awareness
- **Hub discovery**: Hub broadcasts a `MESH_PKT_BEACON`; nodes auto-register
