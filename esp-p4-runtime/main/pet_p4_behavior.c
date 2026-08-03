#include "pet_p4_behavior.h"

#include <stdio.h>
#include <string.h>

#define PET_P4_IDLE_VARIANT_FALLBACK_MS 6500ULL
#define PET_P4_WORKING_VARIANT_FALLBACK_MS 4800ULL
#define PET_P4_WELCOME_MIN_MS 1200ULL

static const char *canonical_lifecycle(const char *lifecycle) {
  if (!lifecycle || !lifecycle[0]) return "idle";
  if (!strcmp(lifecycle, "active") || !strcmp(lifecycle, "thinking") || !strcmp(lifecycle, "tool_running")) {
    return "working";
  }
  if (!strcmp(lifecycle, "notification")) return "waiting_user";
  if (!strcmp(lifecycle, "complete") || !strcmp(lifecycle, "completed")) return "done";
  if (!strcmp(lifecycle, "failed")) return "error";
  return lifecycle;
}

static uint64_t asset_duration_ms(
  const pet_p4_asset_entry_t *entry,
  uint64_t fallback_ms
) {
  uint64_t duration = 0;
  if (entry && entry->frames > 0) {
    if (entry->duration_ms > 0) {
      duration = entry->duration_ms;
    } else if (entry->frame_duration_ms > 0) {
      duration = (uint64_t) entry->frames * entry->frame_duration_ms;
    } else if (entry->fps > 0) {
      duration = ((uint64_t) entry->frames * 1000ULL) / entry->fps;
    }
  }
  return duration > 0 ? duration : fallback_ms;
}

static uint64_t welcome_duration_ms(const pet_p4_asset_catalog_t *catalog, int welcome_index) {
  uint64_t duration;
  if (!catalog || welcome_index < 0 || welcome_index >= catalog->count) return 0;
  duration = asset_duration_ms(&catalog->entries[welcome_index], PET_P4_WELCOME_MIN_MS);
  if (duration < PET_P4_WELCOME_MIN_MS) duration = PET_P4_WELCOME_MIN_MS;
  return duration;
}

static int select_tracked_asset(
  pet_p4_behavior_t *behavior,
  int index,
  uint64_t now_ms
) {
  if (!behavior || index < 0) return index;
  if (behavior->selected_index != index) {
    behavior->selected_index = index;
    behavior->selected_since_ms = now_ms;
  }
  return index;
}

static int rotating_prefix(
  pet_p4_behavior_t *behavior,
  const pet_p4_asset_catalog_t *catalog,
  const char *prefix,
  uint64_t now_ms,
  uint64_t fallback_ms,
  uint32_t seed
) {
  int count = pet_p4_asset_catalog_count_prefix(catalog, prefix);
  int position = -1;
  int index = behavior ? behavior->selected_index : -1;
  if (!behavior || count <= 0) return -1;

  for (int i = 0; i < count; i += 1) {
    if (pet_p4_asset_catalog_nth_prefix(catalog, prefix, i) == index) {
      position = i;
      break;
    }
  }
  if (position < 0) {
    position = (int) (seed % (uint32_t) count);
    index = pet_p4_asset_catalog_nth_prefix(catalog, prefix, position);
    behavior->selected_index = index;
    behavior->selected_since_ms = now_ms;
    return index;
  }
  if (count == 1) return index;

  uint64_t cycle_duration_ms = 0;
  for (int i = 0; i < count; i += 1) {
    int candidate = pet_p4_asset_catalog_nth_prefix(catalog, prefix, i);
    if (candidate < 0 || candidate >= catalog->count) continue;
    uint64_t duration = asset_duration_ms(&catalog->entries[candidate], fallback_ms);
    if (UINT64_MAX - cycle_duration_ms < duration) {
      cycle_duration_ms = 0;
      break;
    }
    cycle_duration_ms += duration;
  }

  uint64_t elapsed_ms = now_ms >= behavior->selected_since_ms
    ? now_ms - behavior->selected_since_ms : 0;
  if (cycle_duration_ms > 0 && elapsed_ms >= cycle_duration_ms) {
    uint64_t skipped_ms = (elapsed_ms / cycle_duration_ms) * cycle_duration_ms;
    behavior->selected_since_ms += skipped_ms;
    elapsed_ms -= skipped_ms;
  }
  for (int step = 0; step < count; step += 1) {
    uint64_t duration = asset_duration_ms(&catalog->entries[index], fallback_ms);
    if (elapsed_ms < duration) break;
    elapsed_ms -= duration;
    behavior->selected_since_ms += duration;
    position = (position + 1) % count;
    index = pet_p4_asset_catalog_nth_prefix(catalog, prefix, position);
    behavior->selected_index = index;
  }
  return index;
}

static int fallback_idle(const pet_p4_asset_catalog_t *catalog) {
  int index = pet_p4_asset_catalog_find_exact(catalog, "idle.default");
  if (index >= 0) return index;
  index = pet_p4_asset_catalog_nth_prefix(catalog, "idle.", 0);
  if (index >= 0) return index;
  return catalog && catalog->count > 0 ? 0 : -1;
}

void pet_p4_behavior_init(pet_p4_behavior_t *behavior) {
  if (!behavior) return;
  memset(behavior, 0, sizeof(*behavior));
  behavior->selected_index = -1;
}

int pet_p4_behavior_select(
  pet_p4_behavior_t *behavior,
  const pet_p4_asset_catalog_t *catalog,
  const char *lifecycle,
  uint32_t asset_revision,
  uint64_t now_ms
) {
  const char *canonical = canonical_lifecycle(lifecycle);
  int index;
  if (!behavior || !catalog || catalog->count == 0) return -1;

  if (!behavior->initialized) {
    behavior->initialized = true;
    behavior->state_since_ms = now_ms;
    behavior->asset_revision = asset_revision;
    behavior->selected_index = -1;
    snprintf(behavior->lifecycle, sizeof(behavior->lifecycle), "%s", canonical);
    if (!strcmp(canonical, "idle") || !strcmp(canonical, "welcome")) {
      index = pet_p4_asset_catalog_find_exact(catalog, "welcome");
      if (index >= 0) behavior->welcome_until_ms = now_ms + welcome_duration_ms(catalog, index);
    }
  } else if (behavior->asset_revision != asset_revision) {
    behavior->asset_revision = asset_revision;
    behavior->selected_index = -1;
    behavior->welcome_until_ms = 0;
    if (!strcmp(canonical, "idle") || !strcmp(canonical, "welcome")) {
      index = pet_p4_asset_catalog_find_exact(catalog, "welcome");
      if (index >= 0) behavior->welcome_until_ms = now_ms + welcome_duration_ms(catalog, index);
    }
  }

  if (strcmp(behavior->lifecycle, canonical) != 0) {
    snprintf(behavior->lifecycle, sizeof(behavior->lifecycle), "%s", canonical);
    behavior->state_since_ms = now_ms;
    behavior->selected_index = -1;
    behavior->welcome_until_ms = 0;
  }
  if (behavior->welcome_until_ms > now_ms
      && (!strcmp(canonical, "idle") || !strcmp(canonical, "welcome"))) {
    index = pet_p4_asset_catalog_find_exact(catalog, "welcome");
    if (index >= 0) return select_tracked_asset(behavior, index, now_ms);
  }

  if (!strcmp(canonical, "welcome") || !strcmp(canonical, "waiting_user")
      || !strcmp(canonical, "done") || !strcmp(canonical, "error")) {
    index = pet_p4_asset_catalog_find_exact(catalog, canonical);
    if (index >= 0) return select_tracked_asset(behavior, index, now_ms);
  }
  if (!strncmp(canonical, "touch.", 6)) {
    index = pet_p4_asset_catalog_find_exact(catalog, canonical);
    if (index >= 0) return select_tracked_asset(behavior, index, now_ms);
    index = pet_p4_asset_catalog_nth_prefix(catalog, "touch.", 0);
    if (index >= 0) return select_tracked_asset(behavior, index, now_ms);
  }
  if (!strcmp(canonical, "speaking")) {
    index = pet_p4_asset_catalog_find_exact(catalog, "working.browsing");
    if (index >= 0) return select_tracked_asset(behavior, index, now_ms);
    canonical = "working";
  }
  if (!strcmp(canonical, "working")) {
    index = rotating_prefix(
      behavior,
      catalog,
      "working",
      now_ms,
      PET_P4_WORKING_VARIANT_FALLBACK_MS,
      asset_revision
    );
    if (index >= 0) return index;
  }
  if (!strcmp(canonical, "idle")) {
    index = rotating_prefix(
      behavior,
      catalog,
      "idle.",
      now_ms,
      PET_P4_IDLE_VARIANT_FALLBACK_MS,
      asset_revision
    );
    if (index >= 0) return index;
  }
  return select_tracked_asset(behavior, fallback_idle(catalog), now_ms);
}