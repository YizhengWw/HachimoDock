#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pet_p4_assets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char lifecycle[32];
  uint64_t state_since_ms;
  uint64_t welcome_until_ms;
  uint64_t selected_since_ms;
  uint32_t asset_revision;
  int selected_index;
  bool initialized;
} pet_p4_behavior_t;

void pet_p4_behavior_init(pet_p4_behavior_t *behavior);
int pet_p4_behavior_select(
  pet_p4_behavior_t *behavior,
  const pet_p4_asset_catalog_t *catalog,
  const char *lifecycle,
  uint32_t asset_revision,
  uint64_t now_ms
);

#ifdef __cplusplus
}
#endif
