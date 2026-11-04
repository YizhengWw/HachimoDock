/* Bounded numeric observation only; no image, path, identity or payload. The
 * protocol worker owns it. It never changes admission, retries or deadlines. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {
  PET_SCREENSHOT_TX_OK, PET_SCREENSHOT_TX_AUDIO, PET_SCREENSHOT_TX_DEADLINE,
  PET_SCREENSHOT_TX_REVOKED, PET_SCREENSHOT_TX_DRIVER_ZERO,
  PET_SCREENSHOT_TX_DRIVER_COUNT, PET_SCREENSHOT_TX_DRAIN,
  PET_SCREENSHOT_TX_MUTEX, PET_SCREENSHOT_TX_ENCODE, PET_SCREENSHOT_TX_ROUTE
} pet_screenshot_tx_reason_t;
typedef struct {
  uint32_t requested,accepted,calls,elapsed_ticks;
  int32_t last_return,drain_return;
  uint8_t reason,part;
} pet_screenshot_tx_observation_t;
static inline bool pet_screenshot_tx_fail(pet_screenshot_tx_observation_t *o,
    pet_screenshot_tx_reason_t reason,uint32_t elapsed) {
  if(!o->reason){o->reason=(uint8_t)reason;o->elapsed_ticks=elapsed;}
  return false;
}
static inline bool pet_screenshot_tx_unwritten_busy(const pet_screenshot_tx_observation_t *o){
  return o->reason==PET_SCREENSHOT_TX_MUTEX&&!o->accepted&&!o->requested&&!o->calls&&!o->part;
}
