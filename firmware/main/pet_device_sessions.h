#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

enum { PET_DEVICE_SESSION_MAX = 8, PET_DEVICE_TERMINAL_MS = 60000,
       PET_DEVICE_SESSION_TIMEOUT_MS = 30000 };
typedef struct {
  char id[128], title[192], content[384], state[24];
  uint64_t revision, terminal_until_ms;
} pet_device_session_t;
typedef struct {
  pet_device_session_t sessions[PET_DEVICE_SESSION_MAX];
  /* Persistent staging avoids putting eight cards on the RX task stack. */
  pet_device_session_t staging[PET_DEVICE_SESSION_MAX];
  unsigned count, selected;
  char agent[64], state[32], title[192], content[384];
  uint64_t last_snapshot_ms, last_host_ms;
  bool snapshot_seen, host_seen;
} pet_device_sessions_t;

void pet_device_text_copy(char *dst, size_t capacity, const char *src);
const char *pet_device_json_string(const cJSON *object, const char *key);
bool pet_device_sessions_handle(pet_device_sessions_t *model, const char *topic,
                                const cJSON *payload, uint64_t now_ms);
void pet_device_sessions_tick(pet_device_sessions_t *model, uint64_t now_ms);
const pet_device_session_t *pet_device_sessions_selected(const pet_device_sessions_t *model);
bool pet_device_sessions_select(pet_device_sessions_t *model, int direction);
const char *pet_device_sessions_lifecycle(const pet_device_sessions_t *model);
