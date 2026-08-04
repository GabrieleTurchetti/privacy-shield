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
 * @brief Connect the Hub to the home WiFi as a station.
 *
 * Uses CONFIG_PRIVACY_SHIELD_WIFI_SSID / _PASSWORD. The STA netif and radio
 * are already brought up by mesh_init(WIFI_MODE_STA); this adds the credentials,
 * connects, and (best-effort) waits for a DHCP address. Auto-reconnects on drop.
 *
 * @return ESP_OK once an IP is obtained, ESP_ERR_TIMEOUT if not yet (non-fatal —
 *         it keeps retrying in the background).
 */
esp_err_t wifi_sta_connect(void);

/**
 * @brief Advertise the dashboard over mDNS as http://privacyshield.local.
 *        Call after the station has an IP.
 *
 * @return ESP_OK on success.
 */
esp_err_t dashboard_mdns_init(void);

/**
 * @brief Start the HTTP web server and register all routes.
 *
 * Serves the dashboard at / and REST API at /api/.
 * Must be called after wifi_sta_connect() and mesh_init().
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
