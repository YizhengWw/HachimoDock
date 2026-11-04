#pragma once
#include "cJSON.h"
#include "pet_p4_side_scene.h"
#include <stddef.h>

typedef struct {
  pet_side_config_t config;
  pet_side_art_t art;
  char actions[4][48]; /* left, right, jump, pause */
  char active_state[24],result_state[24],win_state[24];
  char vars[4][32]; /* score, coins, lives, checkpoint */
} pet_side_declaration_t;
/* Strict JSON parser shared by device and sanitizer tests. Renderer resources
 * and root state/action references are checked by the surrounding interpreter. */
bool pet_side_parse(const cJSON *scene,pet_side_declaration_t *out,char *error,size_t size);
