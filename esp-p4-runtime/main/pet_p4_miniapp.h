/*
 * [Input] Persisted/embedded component packages and runtime input actions.
 * [Output] Bounded component catalog, firmware-builtin synchronization, and active views.
 * [Pos] Public contract for the ESP32-P4 component runtime.
 * [Sync] If this file changes, update `esp-p4-runtime/.folder.md` and `protocol.md`.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "pet_p4_game.h"
#include "pet_p4_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
#define PET_P4_MINIAPP_ACTION_MAX 48
#define PET_P4_MINIAPP_CATALOG_MAX 16

typedef struct {
  bool active;
  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];
  char state[24];
  char page[24];
  char title[64];
  char eyebrow[96];
  char headline[160];
  char metric_label[96];
  char metric_value[64];
  char metric_unit[32];
  char badge[16];
  char note[160];
  char footer[160];
  int progress_percent;
  char progress_label[64];
  char visual_style[20];
  char visual_palette[20];
  char visual_layout[20];
  char visual_sprite[20];
  pet_p4_game_frame_t game;
  uint32_t revision;
} pet_p4_miniapp_view_t;

typedef struct {
  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];
  char title[64];
  bool active;
} pet_p4_miniapp_catalog_entry_t;

esp_err_t pet_p4_miniapp_init(void);
esp_err_t pet_p4_miniapp_sync_builtins(void);
bool pet_p4_miniapp_active(void);
bool pet_p4_miniapp_active_id(char *out, size_t out_size);
bool pet_p4_miniapp_get_view(pet_p4_miniapp_view_t *out);
bool pet_p4_miniapp_installed_id(char *out, size_t out_size);
size_t pet_p4_miniapp_catalog_count(void);
size_t pet_p4_miniapp_catalog_selected(void);
bool pet_p4_miniapp_catalog_get(size_t index, pet_p4_miniapp_catalog_entry_t *out);
void pet_p4_miniapp_catalog_focus_active(void);
bool pet_p4_miniapp_catalog_move(int delta);
bool pet_p4_miniapp_catalog_activate_selected(void);
bool pet_p4_miniapp_sync_stats(const pet_p4_stats_model_t *stats);
void pet_p4_miniapp_process(uint64_t now_ms);
bool pet_p4_miniapp_dispatch_action(const char *action, uint64_t now_ms);
bool pet_p4_miniapp_has_input(const char *event_name);
bool pet_p4_miniapp_resolve_input(
  const char *event_name,
  char *action,
  size_t action_size
);
bool pet_p4_miniapp_dispatch_input(
  const char *event_name,
  uint64_t now_ms,
  char *action,
  size_t action_size
);

bool pet_p4_miniapp_install_begin(const cJSON *payload, char *error, size_t error_size);
bool pet_p4_miniapp_install_chunk(const cJSON *payload, char *error, size_t error_size);
bool pet_p4_miniapp_install_commit(const cJSON *payload, char *error, size_t error_size);
bool pet_p4_miniapp_remove(const char *widget_id, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
