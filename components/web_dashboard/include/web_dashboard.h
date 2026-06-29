#pragma once

#include "esp_err.h"
#include "mesh_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the web dashboard internal state (must be called before
 *        mesh_init() in Hub mode).
 */
void web_dashboard_init(void);

/**
 * @brief Configure and start the WiFi SoftAP.
 *
 * Creates an open access point named "PrivacyShield" on 192.168.4.1.
 * ESP-NOW coexists on the same radio (same channel).
 *
 * @return ESP_OK on success.
 */
esp_err_t wifi_ap_init(void);

/**
 * @brief Start the HTTP web server and register all routes.
 *
 * Serves the dashboard at / and REST API at /api/_.
 * Must be called after wifi_ap_init() and mesh_init().
 *
 * @return ESP_OK on success.
 */
esp_err_t web_server_init(void);

/**
 * @brief Update the cached status for a node (called from the mesh receive
 *        callback whenever a STATUS packet arrives).
 *
 * @param status  Pointer to the decoded STATUS packet.
 * @param mac     Source MAC address of the packet.
 */
void web_dashboard_update_status(const mesh_status_pkt_t *status,
                                 const uint8_t *mac);

#ifdef __cplusplus
}
#endif
