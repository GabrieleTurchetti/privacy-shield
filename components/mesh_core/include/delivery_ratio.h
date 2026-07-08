#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif


/** -------------------------------------------------------------------------- 
* @brief Record a pending STATUS→neighbor expectation, awaiting its ACK
* @param mac  neighbor MAC we expect an ACK from
* @param ts   STATUS timestamp_ms the ACK will echo back (used for matching)
* @param now  current time in ms, used to age out the entry if no ACK arrives
* -------------------------------------------------------------------------- */
void  pending_add(const uint8_t *mac, uint32_t ts, uint32_t now);


/** -------------------------------------------------------------------------- 
* @brief Mark the matching expectation as delivered when an ACK arrives
* @param mac  MAC of the node that sent the ACK
* @param ts   ack_timestamp_ms echoed by the ACK; matched against pending ts
* -------------------------------------------------------------------------- */
void  pending_match(const uint8_t *mac, uint32_t ts);


/** -------------------------------------------------------------------------- 
* @brief Sweep pending entries — acked = hit, timed-out = loss — into the window
* @param now  current time in ms (same clock as pending_add)
* -------------------------------------------------------------------------- */
void  pending_reap(uint32_t now);


/** -------------------------------------------------------------------------- 
* @brief Return the current delivery ratio over the rolling window (0.0–1.0)
* -------------------------------------------------------------------------- */
float delivery_ratio_now(void);


#ifdef __cplusplus
}
#endif