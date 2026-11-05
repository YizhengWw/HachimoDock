#pragma once
#include <string.h>

/* Hardware-independent vocabulary shared with the existing P4 behavior. */
static inline const char *pet_device_canonical_lifecycle(const char *value) {
  if (!value || !value[0]) return "idle";
  if (!strcmp(value, "active") || !strcmp(value, "thinking") || !strcmp(value, "tool_running")) return "working";
  if (!strcmp(value, "notification")) return "waiting_user";
  if (!strcmp(value, "complete") || !strcmp(value, "completed")) return "done";
  if (!strcmp(value, "failed")) return "error";
  return value;
}
