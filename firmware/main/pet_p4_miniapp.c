/*
 * [Input] Bounded v3/v4 widget/button JSON, compiled RGB565-alpha sprites,
 *         sprite-complete firmware-embedded built-ins, install/delete actions, ticks, and stats.
 * [Output] Persisted declarative mini-app catalog, active state, context-resolvable
 *          buttons, post-OTA built-in package migration, and fixed-size
 *          dashboard/component-center views plus generation-safe sprite packs.
 *          Newly installed user packages lead the device catalog; firmware-owned
 *          defaults follow in product order without pinning over user content.
 * [Pos] ESP32-P4 negative-screen runtime and transactional multi-widget installer.
 * [Sync] If this file changes, update `firmware/.folder.md` and `protocol.md`.
 */

#include "pet_p4_miniapp.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#define MINIAPP_MAX_VARS 8
#define MINIAPP_MAX_STATES 6
#define MINIAPP_MAX_PAGES 4
#define MINIAPP_MAX_TRANSITIONS 12
#define MINIAPP_MAX_TICKS 8
#define MINIAPP_MAX_EFFECTS 4
#define MINIAPP_MAX_BUTTONS 8
#define MINIAPP_MAX_CHOICES 6
#define MINIAPP_VAR_NAME_MAX 32
#define MINIAPP_VALUE_MAX 160
#define MINIAPP_WIDGET_JSON_MAX 4096
#define MINIAPP_BUTTONS_JSON_MAX 2048
#define MINIAPP_SPRITE_FILE_HEADER_SIZE 8
#define MINIAPP_SPRITE_FILE_MAX (MINIAPP_SPRITE_FILE_HEADER_SIZE + PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX)
#define MINIAPP_SPRITE_FRAME_MIN 8
#define MINIAPP_SPRITE_FRAME_MAX 64
#define MINIAPP_SPRITE_FRAMES_MAX 8
#define MINIAPP_SPRITE_FPS_MAX 20
#define MINIAPP_TICK_MIN_MS 100
#define MINIAPP_INT_MIN (-1000000000)
#define MINIAPP_INT_MAX 1000000000

#define MINIAPP_WIDGET_PATH "/spiffs/p4-miniapp-widget.json"
#define MINIAPP_WIDGET_TMP_PATH "/spiffs/p4-miniapp-widget.tmp"
#define MINIAPP_BUTTONS_PATH "/spiffs/p4-miniapp-buttons.json"
#define MINIAPP_BUTTONS_TMP_PATH "/spiffs/p4-miniapp-buttons.tmp"
#define MINIAPP_ID_PATH "/spiffs/p4-miniapp-id.txt"
#define MINIAPP_ID_TMP_PATH "/spiffs/p4-miniapp-id.tmp"
#define MINIAPP_CATALOG_PATH "/spiffs/p4-miniapps.json"
#define MINIAPP_BUILTIN_MARKER_PATH "/spiffs/p4-builtins.txt"
#define MINIAPP_BUILTIN_MARKER_TMP_PATH "/spiffs/p4-builtins.tmp"
#define MINIAPP_CATALOG_GENERATION_COUNT 2
#define MINIAPP_PACKAGE_GENERATION_COUNT 2
#define MINIAPP_CATALOG_VERSION 3
#define MINIAPP_CATALOG_JSON_MAX 4096

#ifndef PET_P4_BUILD_ID
#define PET_P4_BUILD_ID "unknown-build"
#endif

extern const unsigned char pet_p4_builtin_components_json[];

typedef enum {
  MINIAPP_VAR_INT,
  MINIAPP_VAR_STRING,
} miniapp_var_type_t;

typedef struct {
  char name[MINIAPP_VAR_NAME_MAX];
  miniapp_var_type_t type;
  int32_t int_value;
  char string_value[64];
} miniapp_var_t;

typedef enum {
  MINIAPP_EFFECT_SET,
  MINIAPP_EFFECT_INC,
} miniapp_effect_op_t;

typedef struct {
  uint8_t var_index;
  miniapp_effect_op_t op;
  int32_t int_value;
  char string_value[64];
} miniapp_effect_t;

typedef struct {
  int8_t from_state;
  int8_t to_state;
  int8_t page_op;
  char action[PET_P4_MINIAPP_ACTION_MAX];
  uint8_t effect_count;
  miniapp_effect_t effects[MINIAPP_MAX_EFFECTS];
} miniapp_transition_t;

typedef struct {
  uint32_t every_ms;
  uint64_t last_ms;
  int8_t while_state;
  uint8_t effect_count;
  miniapp_effect_t effects[MINIAPP_MAX_EFFECTS];
  int8_t condition_var;
  int32_t condition_lte;
  int8_t completion_state;
  uint8_t completion_effect_count;
  miniapp_effect_t completion_effects[MINIAPP_MAX_EFFECTS];
} miniapp_tick_t;

typedef struct {
  char event[48];
  char action[PET_P4_MINIAPP_ACTION_MAX];
} miniapp_button_t;

typedef struct {
  bool active;
  int8_t score_var;
  int8_t playing_state;
  int8_t result_state;
  char start_action[PET_P4_MINIAPP_ACTION_MAX];
  char left_action[PET_P4_MINIAPP_ACTION_MAX];
  char right_action[PET_P4_MINIAPP_ACTION_MAX];
  char rotate_action[PET_P4_MINIAPP_ACTION_MAX];
  char drop_action[PET_P4_MINIAPP_ACTION_MAX];
  char flap_action[PET_P4_MINIAPP_ACTION_MAX];
  pet_p4_game_engine_t engine;
} miniapp_game_t;

typedef enum {
  MINIAPP_VALUE_LITERAL,
  MINIAPP_VALUE_VAR,
  MINIAPP_VALUE_MMSS,
  MINIAPP_VALUE_HMS,
} miniapp_value_kind_t;

typedef struct {
  miniapp_value_kind_t kind;
  int8_t var_index;
  char literal[MINIAPP_VALUE_MAX];
} miniapp_value_t;

typedef struct {
  int8_t selector;
  miniapp_value_t value;
} miniapp_choice_t;

typedef enum {
  MINIAPP_RULE_EMPTY,
  MINIAPP_RULE_VALUE,
  MINIAPP_RULE_SWITCH_STATE,
  MINIAPP_RULE_SWITCH_PAGE,
  MINIAPP_RULE_PROGRESS,
} miniapp_rule_kind_t;

typedef struct {
  miniapp_rule_kind_t kind;
  miniapp_value_t value;
  uint8_t choice_count;
  miniapp_choice_t choices[MINIAPP_MAX_CHOICES];
  int8_t progress_value_var;
  int8_t progress_max_var;
  char progress_label[64];
} miniapp_rule_t;

typedef enum {
  MINIAPP_SLOT_TITLE,
  MINIAPP_SLOT_EYEBROW,
  MINIAPP_SLOT_HEADLINE,
  MINIAPP_SLOT_METRIC_LABEL,
  MINIAPP_SLOT_METRIC_VALUE,
  MINIAPP_SLOT_METRIC_UNIT,
  MINIAPP_SLOT_BADGE,
  MINIAPP_SLOT_NOTE,
  MINIAPP_SLOT_FOOTER,
  MINIAPP_SLOT_PROGRESS,
  MINIAPP_SLOT_VISUAL_STYLE,
  MINIAPP_SLOT_VISUAL_PALETTE,
  MINIAPP_SLOT_VISUAL_LAYOUT,
  MINIAPP_SLOT_VISUAL_SPRITE,
  MINIAPP_SLOT_COUNT,
} miniapp_slot_t;

typedef struct {
  bool active;
  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];
  uint8_t var_count;
  miniapp_var_t vars[MINIAPP_MAX_VARS];
  uint8_t state_count;
  char states[MINIAPP_MAX_STATES][24];
  int8_t current_state;
  uint8_t page_count;
  char pages[MINIAPP_MAX_PAGES][24];
  int8_t current_page;
  uint8_t transition_count;
  miniapp_transition_t transitions[MINIAPP_MAX_TRANSITIONS];
  uint8_t tick_count;
  miniapp_tick_t ticks[MINIAPP_MAX_TICKS];
  uint8_t button_count;
  miniapp_button_t buttons[MINIAPP_MAX_BUTTONS];
  miniapp_game_t game;
  pet_p4_miniapp_sprite_pack_t sprites;
  miniapp_rule_t rules[MINIAPP_SLOT_COUNT];
  pet_p4_miniapp_view_t view;
} miniapp_runtime_t;

typedef struct {
  bool active;
  char transfer_id[64];
  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];
  char widget_json[MINIAPP_WIDGET_JSON_MAX];
  size_t widget_len;
  uint32_t widget_next_index;
  char buttons_json[MINIAPP_BUTTONS_JSON_MAX];
  size_t buttons_len;
  uint32_t buttons_next_index;
  pet_p4_miniapp_sprite_pack_t sprites;
} miniapp_staging_t;

typedef struct {
  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];
  char title[64];
  uint8_t slot;
  uint8_t package_generation;
  uint32_t widget_checksum;
  uint32_t buttons_checksum;
  uint32_t sprites_checksum;
} miniapp_catalog_item_t;

typedef struct {
  bool parsed;
  bool valid;
  bool needs_upgrade;
  uint8_t file_generation;
  uint32_t sequence;
  char active_widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];
  miniapp_catalog_item_t items[PET_P4_MINIAPP_CATALOG_MAX];
  size_t count;
} miniapp_catalog_snapshot_t;

static const char *TAG = "pet-p4-miniapp";
static miniapp_runtime_t g_runtime;
static miniapp_staging_t g_staging;
static bool g_builtin_sync_in_progress;
static miniapp_catalog_item_t g_catalog[PET_P4_MINIAPP_CATALOG_MAX];
static size_t g_catalog_count;
static size_t g_catalog_selected;
static int g_catalog_file_generation = -1;
static uint32_t g_catalog_sequence;
static portMUX_TYPE g_runtime_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *const g_retired_builtin_ids[] = {
  "falling-catch",
};

static bool remove_file_if_present(const char *path);
static bool parse_bounded_scene(
  miniapp_runtime_t *runtime,
  const cJSON *scene,
  bool runtime_v4,
  char *error,
  size_t error_size
);

static void set_error(char *error, size_t error_size, const char *message) {
  if (!error || error_size == 0) return;
  snprintf(error, error_size, "%s", message ? message : "mini-app error");
}

static const char *json_string(const cJSON *object, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

static bool safe_id(const char *value, size_t max_len) {
  size_t len;
  if (!value || !value[0]) return false;
  len = strlen(value);
  if (len >= max_len) return false;
  for (size_t i = 0; i < len; i += 1) {
    char ch = value[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
          || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
      return false;
    }
  }
  return true;
}

static bool safe_widget_id(const char *value) {
  size_t len;
  if (!value || !value[0]) return false;
  len = strlen(value);
  if (len >= PET_P4_MINIAPP_WIDGET_ID_MAX) return false;
  if (value[0] < 'a' || value[0] > 'z') return false;
  for (size_t i = 0; i < len; i += 1) {
    char ch = value[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
          || ch == '_' || ch == '-')) {
      return false;
    }
  }
  return true;
}

static void copy_utf8(char *dest, size_t dest_size, const char *src) {
  size_t len;
  if (!dest || dest_size == 0) return;
  if (!src) src = "";
  len = strlen(src);
  if (len >= dest_size) {
    len = dest_size - 1;
    while (len > 0 && (((unsigned char) src[len] & 0xc0U) == 0x80U)) len -= 1;
  }
  memcpy(dest, src, len);
  dest[len] = '\0';
}

static int32_t clamp_i64(long long value) {
  if (value < MINIAPP_INT_MIN) return MINIAPP_INT_MIN;
  if (value > MINIAPP_INT_MAX) return MINIAPP_INT_MAX;
  return (int32_t) value;
}

static int find_name(char names[][24], int count, const char *name) {
  if (!name) return -1;
  for (int i = 0; i < count; i += 1) {
    if (strcmp(names[i], name) == 0) return i;
  }
  return -1;
}

static int find_var(const miniapp_runtime_t *runtime, const char *name) {
  if (!runtime || !name) return -1;
  for (int i = 0; i < runtime->var_count; i += 1) {
    if (strcmp(runtime->vars[i].name, name) == 0) return i;
  }
  return -1;
}

static void format_compact_u64(char *out, size_t out_size, uint64_t value, bool available) {
  static const struct {
    uint64_t divisor;
    char suffix;
  } scales[] = {
    {1000000000ULL, 'B'},
    {1000000ULL, 'M'},
    {1000ULL, 'K'},
  };
  if (!available) {
    snprintf(out, out_size, "-");
    return;
  }
  for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i += 1) {
    if (value >= scales[i].divisor) {
      uint64_t whole = value / scales[i].divisor;
      uint64_t tenth = (value % scales[i].divisor) * 10ULL / scales[i].divisor;
      if (tenth == 0) snprintf(out, out_size, "%llu%c", (unsigned long long) whole, scales[i].suffix);
      else snprintf(out, out_size, "%llu.%llu%c", (unsigned long long) whole,
                    (unsigned long long) tenth, scales[i].suffix);
      return;
    }
  }
  snprintf(out, out_size, "%llu", (unsigned long long) value);
}

static bool set_string_var(miniapp_runtime_t *runtime, const char *name, const char *value) {
  int index = find_var(runtime, name);
  miniapp_var_t *var;
  char next[sizeof(runtime->vars[0].string_value)];
  if (index < 0 || runtime->vars[index].type != MINIAPP_VAR_STRING) return false;
  var = &runtime->vars[index];
  copy_utf8(next, sizeof(next), value ? value : "");
  if (strcmp(var->string_value, next) == 0) return false;
  copy_utf8(var->string_value, sizeof(var->string_value), next);
  return true;
}

static bool object_keys_allowed(const cJSON *object, const char *const *allowed, size_t count) {
  const cJSON *item;
  if (!cJSON_IsObject(object)) return false;
  cJSON_ArrayForEach(item, object) {
    bool known = false;
    for (size_t i = 0; i < count; i += 1) {
      if (item->string && strcmp(item->string, allowed[i]) == 0) {
        known = true;
        break;
      }
    }
    if (!known) return false;
  }
  return true;
}

static bool parse_vars(miniapp_runtime_t *runtime, const cJSON *vars, char *error, size_t error_size) {
  const cJSON *decl;
  if (!cJSON_IsObject(vars)) {
    set_error(error, error_size, "vars must be an object");
    return false;
  }
  cJSON_ArrayForEach(decl, vars) {
    const char *type;
    const cJSON *init;
    miniapp_var_t *var;
    if (runtime->var_count >= MINIAPP_MAX_VARS || !safe_id(decl->string, MINIAPP_VAR_NAME_MAX)) {
      set_error(error, error_size, "vars exceed bounds or contain an invalid id");
      return false;
    }
    if (!cJSON_IsObject(decl)) {
      snprintf(error, error_size, "var %s declaration must be an object", decl->string);
      return false;
    }
    const cJSON *field;
    cJSON_ArrayForEach(field, decl) {
      if (!field->string
          || (strcmp(field->string, "type") != 0 && strcmp(field->string, "init") != 0)) {
        snprintf(
          error,
          error_size,
          "var %s contains unsupported field %s",
          decl->string,
          field->string ? field->string : "?"
        );
        return false;
      }
    }
    type = json_string(decl, "type");
    init = cJSON_GetObjectItemCaseSensitive(decl, "init");
    var = &runtime->vars[runtime->var_count++];
    copy_utf8(var->name, sizeof(var->name), decl->string);
    if (strcmp(type, "int") == 0) {
      if (init && (!cJSON_IsNumber(init)
          || init->valuedouble < MINIAPP_INT_MIN
          || init->valuedouble > MINIAPP_INT_MAX
          || init->valuedouble != (double) ((long long) init->valuedouble))) {
        snprintf(error, error_size, "var %s int init must be an integer in bounds", decl->string);
        return false;
      }
      var->type = MINIAPP_VAR_INT;
      var->int_value = init ? clamp_i64((long long) init->valuedouble) : 0;
    } else if (strcmp(type, "string") == 0) {
      if (init && (!cJSON_IsString(init) || strlen(init->valuestring) >= sizeof(var->string_value))) {
        snprintf(error, error_size, "var %s string init must fit in 63 UTF-8 bytes", decl->string);
        return false;
      }
      var->type = MINIAPP_VAR_STRING;
      copy_utf8(var->string_value, sizeof(var->string_value), init ? init->valuestring : "");
    } else {
      snprintf(error, error_size, "var %s type must be int or string", decl->string);
      return false;
    }
  }
  return true;
}

static bool parse_name_array(
  const cJSON *array,
  char names[][24],
  uint8_t *out_count,
  int max_count,
  const char *label,
  char *error,
  size_t error_size
) {
  const cJSON *item;
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) <= 0 || cJSON_GetArraySize(array) > max_count) {
    snprintf(error, error_size, "%s must be a non-empty bounded array", label);
    return false;
  }
  cJSON_ArrayForEach(item, array) {
    if (!cJSON_IsString(item) || !safe_id(item->valuestring, 24)
        || find_name(names, *out_count, item->valuestring) >= 0) {
      snprintf(error, error_size, "%s contains an invalid or duplicate id", label);
      return false;
    }
    copy_utf8(names[*out_count], 24, item->valuestring);
    *out_count += 1;
  }
  return true;
}

static bool parse_pages(miniapp_runtime_t *runtime, const cJSON *pages, char *error, size_t error_size) {
  const cJSON *item;
  if (!pages) {
    runtime->current_page = -1;
    return true;
  }
  if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) <= 0
      || cJSON_GetArraySize(pages) > MINIAPP_MAX_PAGES) {
    set_error(error, error_size, "pages must be a non-empty bounded array");
    return false;
  }
  cJSON_ArrayForEach(item, pages) {
    static const char *const allowed[] = {"id"};
    const char *id;
    if (!object_keys_allowed(item, allowed, 1)) {
      set_error(error, error_size, "page contains an unsupported field");
      return false;
    }
    id = json_string(item, "id");
    if (!safe_id(id, 24) || find_name(runtime->pages, runtime->page_count, id) >= 0) {
      set_error(error, error_size, "page contains an invalid or duplicate id");
      return false;
    }
    copy_utf8(runtime->pages[runtime->page_count++], 24, id);
  }
  return true;
}

static bool parse_effect_object(
  miniapp_runtime_t *runtime,
  const cJSON *object,
  miniapp_effect_op_t op,
  miniapp_effect_t *effects,
  uint8_t *effect_count,
  char *error,
  size_t error_size
) {
  const cJSON *item;
  if (!object) return true;
  if (!cJSON_IsObject(object)) {
    set_error(error, error_size, "set/inc must be an object");
    return false;
  }
  cJSON_ArrayForEach(item, object) {
    int var_index;
    miniapp_var_t *var;
    miniapp_effect_t *effect;
    if (*effect_count >= MINIAPP_MAX_EFFECTS) {
      set_error(error, error_size, "transition/tick has too many effects");
      return false;
    }
    var_index = find_var(runtime, item->string);
    if (var_index < 0) {
      set_error(error, error_size, "effect references an unknown var");
      return false;
    }
    var = &runtime->vars[var_index];
    if (op == MINIAPP_EFFECT_INC && (var->type != MINIAPP_VAR_INT || !cJSON_IsNumber(item))) {
      set_error(error, error_size, "inc requires an int var and numeric value");
      return false;
    }
    if (op == MINIAPP_EFFECT_SET
        && !((var->type == MINIAPP_VAR_INT && cJSON_IsNumber(item))
             || (var->type == MINIAPP_VAR_STRING && cJSON_IsString(item)))) {
      set_error(error, error_size, "set value does not match var type");
      return false;
    }
    effect = &effects[(*effect_count)++];
    effect->var_index = (uint8_t) var_index;
    effect->op = op;
    if (var->type == MINIAPP_VAR_INT) {
      effect->int_value = clamp_i64((long long) item->valuedouble);
    } else {
      copy_utf8(effect->string_value, sizeof(effect->string_value), item->valuestring);
    }
  }
  return true;
}

static bool parse_effects(
  miniapp_runtime_t *runtime,
  const cJSON *object,
  miniapp_effect_t *effects,
  uint8_t *effect_count,
  char *error,
  size_t error_size
) {
  if (!parse_effect_object(runtime, cJSON_GetObjectItemCaseSensitive(object, "set"),
                           MINIAPP_EFFECT_SET, effects, effect_count, error, error_size)) return false;
  return parse_effect_object(runtime, cJSON_GetObjectItemCaseSensitive(object, "inc"),
                             MINIAPP_EFFECT_INC, effects, effect_count, error, error_size);
}

static bool parse_transitions(
  miniapp_runtime_t *runtime,
  const cJSON *array,
  char *error,
  size_t error_size
) {
  const cJSON *item;
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > MINIAPP_MAX_TRANSITIONS) {
    set_error(error, error_size, "transitions must be a bounded array");
    return false;
  }
  cJSON_ArrayForEach(item, array) {
    static const char *const allowed[] = {"from", "on", "to", "set", "inc", "page"};
    const char *from;
    const char *action;
    const char *to;
    const char *page;
    miniapp_transition_t *transition = &runtime->transitions[runtime->transition_count++];
    if (!object_keys_allowed(item, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
      set_error(error, error_size, "transition contains an unsupported field");
      return false;
    }
    from = json_string(item, "from");
    action = json_string(item, "on");
    to = json_string(item, "to");
    page = json_string(item, "page");
    if (strcmp(from, "*") == 0) {
      transition->from_state = -1;
    } else {
      transition->from_state = (int8_t) find_name(runtime->states, runtime->state_count, from);
      if (transition->from_state < 0) {
        set_error(error, error_size, "transition references an invalid source state");
        return false;
      }
    }
    transition->to_state = to[0]
      ? (int8_t) find_name(runtime->states, runtime->state_count, to) : -1;
    transition->page_op = -1;
    if (!safe_id(action, sizeof(transition->action))
        || (to[0] && transition->to_state < 0)) {
      set_error(error, error_size, "transition references an invalid state or action");
      return false;
    }
    copy_utf8(transition->action, sizeof(transition->action), action);
    if (page[0]) {
      if (strcmp(page, "next") == 0) transition->page_op = -2;
      else if (strcmp(page, "prev") == 0) transition->page_op = -3;
      else transition->page_op = (int8_t) find_name(runtime->pages, runtime->page_count, page);
      if (transition->page_op == -1) {
        set_error(error, error_size, "transition references an invalid page");
        return false;
      }
    }
    if (!parse_effects(runtime, item, transition->effects, &transition->effect_count, error, error_size)) {
      return false;
    }
  }
  return true;
}

static bool parse_ticks(miniapp_runtime_t *runtime, const cJSON *array, char *error, size_t error_size) {
  const cJSON *item;
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > MINIAPP_MAX_TICKS) {
    set_error(error, error_size, "tick must be a bounded array");
    return false;
  }
  cJSON_ArrayForEach(item, array) {
    static const char *const allowed[] = {
      "every_ms", "while_state", "set", "inc", "when", "then",
    };
    static const char *const when_allowed[] = {"var", "lte"};
    static const char *const then_allowed[] = {"to", "set", "inc"};
    const cJSON *every;
    const cJSON *when;
    const cJSON *then;
    const char *while_state;
    miniapp_tick_t *tick = &runtime->ticks[runtime->tick_count++];
    tick->condition_var = -1;
    tick->completion_state = -1;
    if (!object_keys_allowed(item, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
      set_error(error, error_size, "tick contains an unsupported field");
      return false;
    }
    every = cJSON_GetObjectItemCaseSensitive(item, "every_ms");
    while_state = json_string(item, "while_state");
    if (!cJSON_IsNumber(every) || every->valuedouble < MINIAPP_TICK_MIN_MS
        || every->valuedouble > 86400000.0) {
      set_error(error, error_size, "tick interval is outside bounds");
      return false;
    }
    tick->every_ms = (uint32_t) every->valuedouble;
    tick->while_state = while_state[0]
      ? (int8_t) find_name(runtime->states, runtime->state_count, while_state) : -1;
    if (while_state[0] && tick->while_state < 0) {
      set_error(error, error_size, "tick references an invalid state");
      return false;
    }
    if (!parse_effects(runtime, item, tick->effects, &tick->effect_count, error, error_size)) return false;
    when = cJSON_GetObjectItemCaseSensitive(item, "when");
    then = cJSON_GetObjectItemCaseSensitive(item, "then");
    if ((when && !then) || (!when && then) || (when && (!cJSON_IsObject(when) || !cJSON_IsObject(then)))) {
      set_error(error, error_size, "tick when and then must be paired objects");
      return false;
    }
    if (when) {
      const char *condition_var = json_string(when, "var");
      const cJSON *condition_lte = cJSON_GetObjectItemCaseSensitive(when, "lte");
      const char *completion_state = json_string(then, "to");
      if (!object_keys_allowed(when, when_allowed, sizeof(when_allowed) / sizeof(when_allowed[0]))
          || !object_keys_allowed(then, then_allowed, sizeof(then_allowed) / sizeof(then_allowed[0]))) {
        set_error(error, error_size, "tick completion contains an unsupported field");
        return false;
      }
      tick->condition_var = (int8_t) find_var(runtime, condition_var);
      if (tick->condition_var < 0
          || runtime->vars[tick->condition_var].type != MINIAPP_VAR_INT
          || !cJSON_IsNumber(condition_lte)
          || condition_lte->valuedouble < MINIAPP_INT_MIN
          || condition_lte->valuedouble > MINIAPP_INT_MAX
          || condition_lte->valuedouble != (double) ((long long) condition_lte->valuedouble)) {
        set_error(error, error_size, "tick when requires an int var and integer lte threshold");
        return false;
      }
      tick->condition_lte = (int32_t) condition_lte->valuedouble;
      tick->completion_state = completion_state[0]
        ? (int8_t) find_name(runtime->states, runtime->state_count, completion_state) : -1;
      if (completion_state[0] && tick->completion_state < 0) {
        set_error(error, error_size, "tick then references an invalid state");
        return false;
      }
      if (!parse_effects(runtime, then, tick->completion_effects,
                         &tick->completion_effect_count, error, error_size)) {
        return false;
      }
      if (tick->completion_state < 0 && tick->completion_effect_count == 0) {
        set_error(error, error_size, "tick then must change state or variables");
        return false;
      }
    }
  }
  return true;
}

static bool parse_value(
  const miniapp_runtime_t *runtime,
  const cJSON *item,
  miniapp_value_t *value,
  char *error,
  size_t error_size
) {
  if (cJSON_IsString(item)) {
    value->kind = MINIAPP_VALUE_LITERAL;
    copy_utf8(value->literal, sizeof(value->literal), item->valuestring);
    return true;
  }
  if (!cJSON_IsObject(item)) {
    set_error(error, error_size, "dashboard value must be text or a value rule");
    return false;
  }
  const char *name = json_string(item, "var");
  if (name[0]) value->kind = MINIAPP_VALUE_VAR;
  else if ((name = json_string(item, "fmt_mmss"))[0]) value->kind = MINIAPP_VALUE_MMSS;
  else if ((name = json_string(item, "fmt_hms"))[0]) value->kind = MINIAPP_VALUE_HMS;
  else {
    set_error(error, error_size, "dashboard value rule is unsupported");
    return false;
  }
  value->var_index = (int8_t) find_var(runtime, name);
  if (value->var_index < 0) {
    set_error(error, error_size, "dashboard value references an unknown var");
    return false;
  }
  if ((value->kind == MINIAPP_VALUE_MMSS || value->kind == MINIAPP_VALUE_HMS)
      && runtime->vars[value->var_index].type != MINIAPP_VAR_INT) {
    set_error(error, error_size, "time formatter requires an int var");
    return false;
  }
  return true;
}

static int slot_from_name(const char *name) {
  static const char *const names[] = {
    "title", "eyebrow", "headline", "metricLabel", "metricValue",
    "metricUnit", "badge", "note", "footer", "progress",
    "visualStyle", "visualPalette", "visualLayout", "visualSprite",
  };
  for (int i = 0; i < MINIAPP_SLOT_COUNT; i += 1) {
    if (strcmp(name, names[i]) == 0) return i;
  }
  return -1;
}

static bool visual_literal_allowed(int slot, const miniapp_value_t *value) {
  if (!value || value->kind != MINIAPP_VALUE_LITERAL) return false;
  const char *literal = value->literal;
  if (slot == MINIAPP_SLOT_VISUAL_STYLE) {
    return strcmp(literal, "classic") == 0 || strcmp(literal, "pixel") == 0
      || strcmp(literal, "clean") == 0;
  }
  if (slot == MINIAPP_SLOT_VISUAL_PALETTE) {
    return strcmp(literal, "candy") == 0 || strcmp(literal, "sunset") == 0
      || strcmp(literal, "mint") == 0 || strcmp(literal, "arcade") == 0
      || strcmp(literal, "ocean") == 0 || strcmp(literal, "forest") == 0
      || strcmp(literal, "ember") == 0 || strcmp(literal, "mono") == 0;
  }
  if (slot == MINIAPP_SLOT_VISUAL_LAYOUT) {
    return strcmp(literal, "arcade") == 0 || strcmp(literal, "scoreboard") == 0
      || strcmp(literal, "tool") == 0;
  }
  if (slot == MINIAPP_SLOT_VISUAL_SPRITE) {
    return strcmp(literal, "target") == 0 || strcmp(literal, "trophy") == 0
      || strcmp(literal, "star") == 0 || strcmp(literal, "bolt") == 0
      || strcmp(literal, "coffee") == 0 || strcmp(literal, "timer") == 0
      || strcmp(literal, "droplet") == 0 || strcmp(literal, "gauge") == 0
      || strcmp(literal, "blocks") == 0 || strcmp(literal, "snake") == 0
      || strcmp(literal, "flappy") == 0
      || strcmp(literal, "mole-ready") == 0 || strcmp(literal, "mole-left") == 0
      || strcmp(literal, "mole-center") == 0 || strcmp(literal, "mole-right") == 0;
  }
  return true;
}

static bool validate_visual_rule(int slot, const miniapp_rule_t *rule) {
  if (slot < MINIAPP_SLOT_VISUAL_STYLE || slot > MINIAPP_SLOT_VISUAL_SPRITE) return true;
  if (rule->kind == MINIAPP_RULE_VALUE) return visual_literal_allowed(slot, &rule->value);
  if (rule->kind == MINIAPP_RULE_SWITCH_STATE || rule->kind == MINIAPP_RULE_SWITCH_PAGE) {
    for (int i = 0; i < rule->choice_count; i += 1) {
      if (!visual_literal_allowed(slot, &rule->choices[i].value)) return false;
    }
    return rule->choice_count > 0;
  }
  return false;
}

static bool parse_switch_rule(
  const miniapp_runtime_t *runtime,
  const cJSON *map,
  bool by_state,
  miniapp_rule_t *rule,
  char *error,
  size_t error_size
) {
  const cJSON *choice;
  if (!cJSON_IsObject(map)) {
    set_error(error, error_size, "dashboard switch must be an object");
    return false;
  }
  cJSON_ArrayForEach(choice, map) {
    int selector;
    miniapp_choice_t *target;
    if (rule->choice_count >= MINIAPP_MAX_CHOICES) {
      set_error(error, error_size, "dashboard switch has too many choices");
      return false;
    }
    selector = by_state
      ? find_name((char (*)[24]) runtime->states, runtime->state_count, choice->string)
      : find_name((char (*)[24]) runtime->pages, runtime->page_count, choice->string);
    if (selector < 0) {
      set_error(error, error_size, "dashboard switch references an unknown selector");
      return false;
    }
    target = &rule->choices[rule->choice_count++];
    target->selector = (int8_t) selector;
    if (!parse_value(runtime, choice, &target->value, error, error_size)) return false;
  }
  return true;
}

static bool parse_dashboard(
  miniapp_runtime_t *runtime,
  const cJSON *dashboard,
  char *error,
  size_t error_size
) {
  const cJSON *item;
  if (!cJSON_IsObject(dashboard)) {
    set_error(error, error_size, "dashboard must be an object");
    return false;
  }
  cJSON_ArrayForEach(item, dashboard) {
    int slot = slot_from_name(item->string);
    miniapp_rule_t *rule;
    const cJSON *map;
    if (slot < 0) {
      set_error(error, error_size, "dashboard contains an unknown slot");
      return false;
    }
    rule = &runtime->rules[slot];
    if (slot == MINIAPP_SLOT_PROGRESS && cJSON_IsObject(item)
        && cJSON_GetObjectItemCaseSensitive(item, "pct_of")) {
      rule->kind = MINIAPP_RULE_PROGRESS;
      rule->progress_value_var = (int8_t) find_var(runtime, json_string(item, "pct_of"));
      rule->progress_max_var = (int8_t) find_var(runtime, json_string(item, "of_max"));
      copy_utf8(rule->progress_label, sizeof(rule->progress_label), json_string(item, "label"));
      if (rule->progress_value_var < 0 || rule->progress_max_var < 0
          || runtime->vars[rule->progress_value_var].type != MINIAPP_VAR_INT
          || runtime->vars[rule->progress_max_var].type != MINIAPP_VAR_INT) {
        set_error(error, error_size, "progress requires two int vars");
        return false;
      }
      continue;
    }
    map = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "switch_state") : NULL;
    if (map) {
      rule->kind = MINIAPP_RULE_SWITCH_STATE;
      if (!parse_switch_rule(runtime, map, true, rule, error, error_size)) return false;
      if (!validate_visual_rule(slot, rule)) {
        set_error(error, error_size, "visual slot contains an unsupported preset");
        return false;
      }
      continue;
    }
    map = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "switch_page") : NULL;
    if (map) {
      rule->kind = MINIAPP_RULE_SWITCH_PAGE;
      if (!parse_switch_rule(runtime, map, false, rule, error, error_size)) return false;
      if (!validate_visual_rule(slot, rule)) {
        set_error(error, error_size, "visual slot contains an unsupported preset");
        return false;
      }
      continue;
    }
    rule->kind = MINIAPP_RULE_VALUE;
    if (!parse_value(runtime, item, &rule->value, error, error_size)) return false;
    if (!validate_visual_rule(slot, rule)) {
      set_error(error, error_size, "visual slot contains an unsupported preset");
      return false;
    }
  }
  return true;
}

static bool button_event_contains(const char *binding, const char *event_name) {
  if (strcmp(binding, event_name) == 0) return true;
  return strcmp(binding, "knob.rotate_cw / knob.rotate_ccw") == 0
    && (strcmp(event_name, "knob.rotate_cw") == 0
        || strcmp(event_name, "knob.rotate_ccw") == 0);
}

static bool button_events_conflict(const char *left, const char *right) {
  return button_event_contains(left, right)
    || button_event_contains(right, left)
    || (button_event_contains(left, "knob.rotate_cw")
        && button_event_contains(right, "knob.rotate_cw"))
    || (button_event_contains(left, "knob.rotate_ccw")
        && button_event_contains(right, "knob.rotate_ccw"));
}

static bool parse_buttons(
  miniapp_runtime_t *runtime,
  const char *buttons_json,
  char *error,
  size_t error_size
) {
  cJSON *root;
  const cJSON *item;
  if (!buttons_json || !buttons_json[0]) return true;
  root = cJSON_Parse(buttons_json);
  if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) > MINIAPP_MAX_BUTTONS) {
    cJSON_Delete(root);
    set_error(error, error_size, "buttons.json must be a bounded array");
    return false;
  }
  cJSON_ArrayForEach(item, root) {
    const char *event = json_string(item, "event");
    const char *action = json_string(item, "action");
    miniapp_button_t *button;
    if (!event[0] || strlen(event) >= sizeof(runtime->buttons[0].event)
        || !safe_id(action, PET_P4_MINIAPP_ACTION_MAX)) {
      cJSON_Delete(root);
      set_error(error, error_size, "button binding contains an invalid event or action");
      return false;
    }
    for (int i = 0; i < runtime->button_count; i += 1) {
      if (button_events_conflict(runtime->buttons[i].event, event)) {
        cJSON_Delete(root);
        set_error(error, error_size, "button binding event is already assigned");
        return false;
      }
      if (strcmp(runtime->buttons[i].action, action) == 0) {
        cJSON_Delete(root);
        set_error(error, error_size, "button binding action must be unique");
        return false;
      }
    }
    button = &runtime->buttons[runtime->button_count++];
    copy_utf8(button->event, sizeof(button->event), event);
    copy_utf8(button->action, sizeof(button->action), action);
  }
  cJSON_Delete(root);
  return true;
}

static uint32_t stable_widget_seed(const char *widget_id) {
  uint32_t hash = 2166136261U;
  if (!widget_id) return hash;
  for (const unsigned char *cursor = (const unsigned char *) widget_id; *cursor; cursor += 1) {
    hash ^= *cursor;
    hash *= 16777619U;
  }
  return hash;
}

static bool runtime_has_transition_action(
  const miniapp_runtime_t *runtime,
  const char *action
) {
  for (int i = 0; i < runtime->transition_count; i += 1) {
    if (strcmp(runtime->transitions[i].action, action) == 0) return true;
  }
  return false;
}

static bool runtime_has_button_action(
  const miniapp_runtime_t *runtime,
  const char *action
) {
  for (int i = 0; i < runtime->button_count; i += 1) {
    if (strcmp(runtime->buttons[i].action, action) == 0) return true;
  }
  return false;
}

static bool parse_game(
  miniapp_runtime_t *runtime,
  const cJSON *game,
  char *error,
  size_t error_size
) {
  static const char *const allowed[] = {
    "type", "tick_ms", "playing_state", "result_state", "score_var", "actions",
  };
  static const char *const blocks_action_allowed[] = {
    "start", "left", "right", "rotate", "drop",
  };
  static const char *const snake_action_allowed[] = {
    "start", "left", "right",
  };
  static const char *const flappy_action_allowed[] = {
    "flap",
  };
  const cJSON *tick;
  const cJSON *actions;
  const char *type;
  const char *score_var;
  const char *playing_state;
  const char *result_state;
  pet_p4_game_kind_t kind;
  const char *required_actions[5];
  int required_count;
  if (!game) return true;
  if (!object_keys_allowed(game, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
    set_error(error, error_size, "game contains an unsupported field");
    return false;
  }
  type = json_string(game, "type");
  kind = pet_p4_game_kind_from_name(type);
  tick = cJSON_GetObjectItemCaseSensitive(game, "tick_ms");
  actions = cJSON_GetObjectItemCaseSensitive(game, "actions");
  score_var = json_string(game, "score_var");
  playing_state = json_string(game, "playing_state");
  result_state = json_string(game, "result_state");
  const char *const *action_allowed = kind == PET_P4_GAME_BLOCKS
    ? blocks_action_allowed
    : kind == PET_P4_GAME_SNAKE ? snake_action_allowed : flappy_action_allowed;
  size_t action_allowed_count = kind == PET_P4_GAME_BLOCKS
    ? sizeof(blocks_action_allowed) / sizeof(blocks_action_allowed[0])
    : kind == PET_P4_GAME_SNAKE
      ? sizeof(snake_action_allowed) / sizeof(snake_action_allowed[0])
      : sizeof(flappy_action_allowed) / sizeof(flappy_action_allowed[0]);
  if (kind == PET_P4_GAME_NONE || !cJSON_IsNumber(tick)
      || tick->valuedouble < 100.0 || tick->valuedouble > 2000.0
      || tick->valuedouble != (double) ((uint32_t) tick->valuedouble)
      || !object_keys_allowed(actions, action_allowed, action_allowed_count)) {
    set_error(error, error_size, "game requires a supported type, bounded tick_ms, and actions");
    return false;
  }
  runtime->game.score_var = (int8_t) find_var(runtime, score_var);
  runtime->game.playing_state =
    (int8_t) find_name(runtime->states, runtime->state_count, playing_state);
  runtime->game.result_state =
    (int8_t) find_name(runtime->states, runtime->state_count, result_state);
  if (runtime->game.score_var < 0
      || runtime->vars[runtime->game.score_var].type != MINIAPP_VAR_INT
      || runtime->game.playing_state < 0 || runtime->game.result_state < 0) {
    set_error(error, error_size, "game references an invalid score var or state");
    return false;
  }
  copy_utf8(runtime->game.start_action, sizeof(runtime->game.start_action),
            json_string(actions, "start"));
  copy_utf8(runtime->game.left_action, sizeof(runtime->game.left_action),
            json_string(actions, "left"));
  copy_utf8(runtime->game.right_action, sizeof(runtime->game.right_action),
            json_string(actions, "right"));
  copy_utf8(runtime->game.rotate_action, sizeof(runtime->game.rotate_action),
            json_string(actions, "rotate"));
  copy_utf8(runtime->game.drop_action, sizeof(runtime->game.drop_action),
            json_string(actions, "drop"));
  copy_utf8(runtime->game.flap_action, sizeof(runtime->game.flap_action),
            json_string(actions, "flap"));
  if (kind == PET_P4_GAME_FLAPPY) {
    required_actions[0] = runtime->game.flap_action;
    required_count = 1;
  } else {
    required_actions[0] = runtime->game.start_action;
    required_actions[1] = runtime->game.left_action;
    required_actions[2] = runtime->game.right_action;
    required_actions[3] = runtime->game.rotate_action;
    required_actions[4] = runtime->game.drop_action;
    required_count = kind == PET_P4_GAME_BLOCKS ? 5 : 3;
  }
  for (int i = 0; i < required_count; i += 1) {
    if (!safe_id(required_actions[i], PET_P4_MINIAPP_ACTION_MAX)
        || !runtime_has_transition_action(runtime, required_actions[i])
        || !runtime_has_button_action(runtime, required_actions[i])) {
      set_error(error, error_size, "game actions must match transitions and buttons");
      return false;
    }
  }
  if (!pet_p4_game_configure(
        &runtime->game.engine,
        kind,
        (uint32_t) tick->valuedouble,
        stable_widget_seed(runtime->widget_id)
      )) {
    set_error(error, error_size, "game engine configuration is invalid");
    return false;
  }
  runtime->game.active = true;
  return true;
}

static void resolve_value(
  const miniapp_runtime_t *runtime,
  const miniapp_value_t *value,
  char *out,
  size_t out_size
) {
  const miniapp_var_t *var;
  out[0] = '\0';
  if (value->kind == MINIAPP_VALUE_LITERAL) {
    copy_utf8(out, out_size, value->literal);
    return;
  }
  if (value->var_index < 0 || value->var_index >= runtime->var_count) return;
  var = &runtime->vars[value->var_index];
  if (value->kind == MINIAPP_VALUE_VAR) {
    if (var->type == MINIAPP_VAR_INT) snprintf(out, out_size, "%ld", (long) var->int_value);
    else copy_utf8(out, out_size, var->string_value);
  } else {
    int32_t seconds = var->int_value > 0 ? var->int_value : 0;
    if (value->kind == MINIAPP_VALUE_MMSS) {
      snprintf(out, out_size, "%02ld:%02ld", (long) (seconds / 60), (long) (seconds % 60));
    } else {
      snprintf(out, out_size, "%ld:%02ld:%02ld", (long) (seconds / 3600),
               (long) ((seconds / 60) % 60), (long) (seconds % 60));
    }
  }
}

static void resolve_rule(
  const miniapp_runtime_t *runtime,
  const miniapp_rule_t *rule,
  char *out,
  size_t out_size
) {
  int selector;
  out[0] = '\0';
  if (rule->kind == MINIAPP_RULE_VALUE) {
    resolve_value(runtime, &rule->value, out, out_size);
    return;
  }
  if (rule->kind != MINIAPP_RULE_SWITCH_STATE && rule->kind != MINIAPP_RULE_SWITCH_PAGE) return;
  selector = rule->kind == MINIAPP_RULE_SWITCH_STATE ? runtime->current_state : runtime->current_page;
  for (int i = 0; i < rule->choice_count; i += 1) {
    if (rule->choices[i].selector == selector) {
      resolve_value(runtime, &rule->choices[i].value, out, out_size);
      return;
    }
  }
}

static void refresh_view(miniapp_runtime_t *runtime) {
  pet_p4_miniapp_view_t *view = &runtime->view;
  uint32_t next_revision = view->revision + 1;
  memset(view, 0, sizeof(*view));
  view->active = runtime->active;
  view->progress_percent = -1;
  view->revision = next_revision;
  copy_utf8(view->widget_id, sizeof(view->widget_id), runtime->widget_id);
  if (runtime->current_state >= 0) {
    copy_utf8(view->state, sizeof(view->state), runtime->states[runtime->current_state]);
  }
  if (runtime->current_page >= 0) {
    copy_utf8(view->page, sizeof(view->page), runtime->pages[runtime->current_page]);
  }
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_TITLE], view->title, sizeof(view->title));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_EYEBROW], view->eyebrow, sizeof(view->eyebrow));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_HEADLINE], view->headline, sizeof(view->headline));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_METRIC_LABEL], view->metric_label, sizeof(view->metric_label));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_METRIC_VALUE], view->metric_value, sizeof(view->metric_value));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_METRIC_UNIT], view->metric_unit, sizeof(view->metric_unit));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_BADGE], view->badge, sizeof(view->badge));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_NOTE], view->note, sizeof(view->note));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_FOOTER], view->footer, sizeof(view->footer));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_VISUAL_STYLE], view->visual_style, sizeof(view->visual_style));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_VISUAL_PALETTE], view->visual_palette, sizeof(view->visual_palette));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_VISUAL_LAYOUT], view->visual_layout, sizeof(view->visual_layout));
  resolve_rule(runtime, &runtime->rules[MINIAPP_SLOT_VISUAL_SPRITE], view->visual_sprite, sizeof(view->visual_sprite));
  if (runtime->game.active) {
    pet_p4_game_get_frame(&runtime->game.engine, &view->game);
  }
  const miniapp_rule_t *progress = &runtime->rules[MINIAPP_SLOT_PROGRESS];
  if (progress->kind == MINIAPP_RULE_PROGRESS) {
    int32_t value = runtime->vars[progress->progress_value_var].int_value;
    int32_t maximum = runtime->vars[progress->progress_max_var].int_value;
    view->progress_percent = maximum > 0 ? (int) (((long long) value * 100LL) / maximum) : 0;
    if (view->progress_percent < 0) view->progress_percent = 0;
    if (view->progress_percent > 100) view->progress_percent = 100;
    copy_utf8(view->progress_label, sizeof(view->progress_label), progress->progress_label);
  } else {
    char raw[MINIAPP_VALUE_MAX];
    resolve_rule(runtime, progress, raw, sizeof(raw));
    char *colon = strchr(raw, ':');
    if (raw[0]) {
      view->progress_percent = atoi(raw);
      if (view->progress_percent < 0) view->progress_percent = 0;
      if (view->progress_percent > 100) view->progress_percent = 100;
      if (colon) copy_utf8(view->progress_label, sizeof(view->progress_label), colon + 1);
    }
  }
}

static bool parse_runtime(
  miniapp_runtime_t *runtime,
  const char *widget_id,
  const char *widget_json,
  const char *buttons_json,
  const pet_p4_miniapp_sprite_pack_t *sprites,
  char *error,
  size_t error_size
) {
  static const char *const allowed[] = {
    "schema_version", "vars", "states", "initial_state", "pages", "initial_page",
    "transitions", "tick", "dashboard", "fetchers", "readers", "engine", "scene",
    "game",
  };
  cJSON *root;
  const cJSON *schema;
  const cJSON *fetchers;
  const cJSON *readers;
  const cJSON *scene;
  const cJSON *legacy_game;
  const char *engine;
  const char *initial_state;
  const char *initial_page;
  miniapp_runtime_t *parsed;
  if (!safe_widget_id(widget_id)) {
    set_error(error, error_size, "widgetId must be a bounded ASCII id");
    return false;
  }
  root = cJSON_Parse(widget_json);
  if (!cJSON_IsObject(root) || !object_keys_allowed(root, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
    cJSON_Delete(root);
    set_error(error, error_size, "widget.json is invalid or contains unsupported fields");
    return false;
  }
  parsed = (miniapp_runtime_t *) calloc(1, sizeof(*parsed));
  if (!parsed) {
    cJSON_Delete(root);
    set_error(error, error_size, "not enough memory for mini-app");
    return false;
  }
  copy_utf8(parsed->widget_id, sizeof(parsed->widget_id), widget_id);
  schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
  fetchers = cJSON_GetObjectItemCaseSensitive(root, "fetchers");
  readers = cJSON_GetObjectItemCaseSensitive(root, "readers");
  scene = cJSON_GetObjectItemCaseSensitive(root, "scene");
  legacy_game = cJSON_GetObjectItemCaseSensitive(root, "game");
  engine = json_string(root, "engine");
  if (!cJSON_IsNumber(schema) || schema->valueint != 1) {
    set_error(error, error_size, "schema_version must be 1");
    goto fail;
  }
  if ((cJSON_IsObject(fetchers) && fetchers->child) || (cJSON_IsObject(readers) && readers->child)) {
    set_error(error, error_size, "P4 mini-apps do not allow fetchers or readers");
    goto fail;
  }
  const bool runtime_v4 = strcmp(engine, "p4-bounded-runtime-v4") == 0;
  if ((engine[0] && strcmp(engine, "p4-bounded-runtime-v3") != 0 && !runtime_v4)
      || (scene && !engine[0]) || (scene && legacy_game)) {
    set_error(error, error_size, "engine/scene declaration is unsupported or conflicts with legacy game");
    goto fail;
  }
  if (!parse_vars(parsed, cJSON_GetObjectItemCaseSensitive(root, "vars"), error, error_size)) goto fail;
  if (!parse_name_array(cJSON_GetObjectItemCaseSensitive(root, "states"), parsed->states,
                        &parsed->state_count, MINIAPP_MAX_STATES, "states", error, error_size)) goto fail;
  initial_state = json_string(root, "initial_state");
  parsed->current_state = (int8_t) find_name(parsed->states, parsed->state_count, initial_state);
  if (parsed->current_state < 0) {
    set_error(error, error_size, "initial_state is unknown");
    goto fail;
  }
  if (!parse_pages(parsed, cJSON_GetObjectItemCaseSensitive(root, "pages"), error, error_size)) goto fail;
  initial_page = json_string(root, "initial_page");
  if (parsed->page_count > 0) {
    parsed->current_page = (int8_t) find_name(parsed->pages, parsed->page_count, initial_page);
    if (parsed->current_page < 0) {
      set_error(error, error_size, "initial_page is unknown");
      goto fail;
    }
  }
  if (!parse_transitions(parsed, cJSON_GetObjectItemCaseSensitive(root, "transitions"), error, error_size)) goto fail;
  if (!parse_ticks(parsed, cJSON_GetObjectItemCaseSensitive(root, "tick"), error, error_size)) goto fail;
  if (!parse_dashboard(parsed, cJSON_GetObjectItemCaseSensitive(root, "dashboard"), error, error_size)) goto fail;
  if (!parse_buttons(parsed, buttons_json, error, error_size)) goto fail;
  if (!parse_game(parsed, legacy_game, error, error_size)) goto fail;
  if (sprites) parsed->sprites = *sprites;
  if (!parse_bounded_scene(parsed, scene, runtime_v4, error, error_size)) goto fail;
  parsed->active = true;
  refresh_view(parsed);
  *runtime = *parsed;
  free(parsed);
  cJSON_Delete(root);
  return true;

fail:
  free(parsed);
  cJSON_Delete(root);
  return false;
}

static bool apply_effects(miniapp_runtime_t *runtime, const miniapp_effect_t *effects, int count) {
  bool changed = false;
  for (int i = 0; i < count; i += 1) {
    const miniapp_effect_t *effect = &effects[i];
    miniapp_var_t *var = &runtime->vars[effect->var_index];
    if (var->type == MINIAPP_VAR_INT) {
      int32_t next = effect->op == MINIAPP_EFFECT_INC
        ? clamp_i64((long long) var->int_value + effect->int_value) : effect->int_value;
      if (next != var->int_value) {
        var->int_value = next;
        changed = true;
      }
    } else if (strcmp(var->string_value, effect->string_value) != 0) {
      copy_utf8(var->string_value, sizeof(var->string_value), effect->string_value);
      changed = true;
    }
  }
  return changed;
}

static bool game_command_for_action(
  const miniapp_game_t *game,
  const char *action,
  pet_p4_game_command_t *command
) {
  if (!game || !game->active || !action || !command) return false;
  if (game->engine.kind == PET_P4_GAME_BOUNDED) return false;
  if (strcmp(action, game->start_action) == 0) {
    *command = PET_P4_GAME_COMMAND_START;
  } else if (strcmp(action, game->left_action) == 0) {
    *command = PET_P4_GAME_COMMAND_LEFT;
  } else if (strcmp(action, game->right_action) == 0) {
    *command = PET_P4_GAME_COMMAND_RIGHT;
  } else if (game->rotate_action[0] && strcmp(action, game->rotate_action) == 0) {
    *command = PET_P4_GAME_COMMAND_ROTATE;
  } else if (game->drop_action[0] && strcmp(action, game->drop_action) == 0) {
    *command = PET_P4_GAME_COMMAND_DROP;
  } else if (game->flap_action[0] && strcmp(action, game->flap_action) == 0) {
    *command = PET_P4_GAME_COMMAND_FLAP;
  } else {
    return false;
  }
  return true;
}

static bool sync_game_runtime(miniapp_runtime_t *runtime) {
  bool changed = false;
  miniapp_game_t *game = &runtime->game;
  if (!game->active) return false;
  if (game->score_var >= 0
      && runtime->vars[game->score_var].int_value != game->engine.score) {
    runtime->vars[game->score_var].int_value = game->engine.score;
    changed = true;
  }
  if (game->result_state >= 0 && game->engine.game_over
      && runtime->current_state != game->result_state) {
    runtime->current_state = game->result_state;
    changed = true;
  }
  return changed;
}

bool pet_p4_miniapp_dispatch_action(const char *action, uint64_t now_ms) {
  bool handled = false;
  if (!action || !action[0]) return false;
  portENTER_CRITICAL(&g_runtime_lock);
  if (!g_runtime.active) goto done;
  for (int i = 0; i < g_runtime.transition_count; i += 1) {
    miniapp_transition_t *transition = &g_runtime.transitions[i];
    bool changed = false;
    pet_p4_game_command_t game_command;
    if (strcmp(transition->action, action) != 0) continue;
    if (transition->from_state >= 0 && transition->from_state != g_runtime.current_state) continue;
    if (transition->to_state >= 0 && transition->to_state != g_runtime.current_state) {
      g_runtime.current_state = transition->to_state;
      changed = true;
    }
    if (transition->page_op != -1 && g_runtime.page_count > 0) {
      int next = g_runtime.current_page;
      if (transition->page_op == -2) next = (next + 1) % g_runtime.page_count;
      else if (transition->page_op == -3) next = (next + g_runtime.page_count - 1) % g_runtime.page_count;
      else next = transition->page_op;
      if (next != g_runtime.current_page) {
        g_runtime.current_page = (int8_t) next;
        changed = true;
      }
    }
    changed |= apply_effects(&g_runtime, transition->effects, transition->effect_count);
    if (g_runtime.game.active
        && g_runtime.game.engine.kind == PET_P4_GAME_BOUNDED) {
      changed |= pet_p4_game_dispatch_action(
        &g_runtime.game.engine,
        action,
        now_ms
      );
      changed |= sync_game_runtime(&g_runtime);
    } else if (game_command_for_action(&g_runtime.game, action, &game_command)) {
      changed |= pet_p4_game_command(&g_runtime.game.engine, game_command, now_ms);
      changed |= sync_game_runtime(&g_runtime);
    }
    if (changed) refresh_view(&g_runtime);
    handled = true;
    break;
  }
done:
  portEXIT_CRITICAL(&g_runtime_lock);
  return handled;
}

static bool button_matches_event(const char *binding, const char *event_name) {
  if (strcmp(binding, event_name) == 0) return true;
  return strcmp(binding, "knob.rotate_cw / knob.rotate_ccw") == 0
    && (strcmp(event_name, "knob.rotate_cw") == 0 || strcmp(event_name, "knob.rotate_ccw") == 0);
}

bool pet_p4_miniapp_has_input(const char *event_name) {
  bool found = false;
  if (!event_name) return false;
  portENTER_CRITICAL(&g_runtime_lock);
  if (!g_runtime.active) goto done;
  for (int i = 0; i < g_runtime.button_count; i += 1) {
    if (button_matches_event(g_runtime.buttons[i].event, event_name)) {
      found = true;
      break;
    }
  }
done:
  portEXIT_CRITICAL(&g_runtime_lock);
  return found;
}

bool pet_p4_miniapp_resolve_input(
  const char *event_name,
  char *action,
  size_t action_size
) {
  char resolved_action[PET_P4_MINIAPP_ACTION_MAX] = {0};
  if (action && action_size > 0) action[0] = '\0';
  if (!event_name) return false;
  portENTER_CRITICAL(&g_runtime_lock);
  if (!g_runtime.active) goto done;
  for (int i = 0; i < g_runtime.button_count; i += 1) {
    if (!button_matches_event(g_runtime.buttons[i].event, event_name)) continue;
    copy_utf8(resolved_action, sizeof(resolved_action), g_runtime.buttons[i].action);
    break;
  }
done:
  portEXIT_CRITICAL(&g_runtime_lock);
  if (!resolved_action[0]) return false;
  if (action && action_size > 0) copy_utf8(action, action_size, resolved_action);
  return true;
}

bool pet_p4_miniapp_dispatch_input(
  const char *event_name,
  uint64_t now_ms,
  char *action,
  size_t action_size
) {
  char resolved_action[PET_P4_MINIAPP_ACTION_MAX] = {0};
  if (!pet_p4_miniapp_resolve_input(
        event_name,
        resolved_action,
        sizeof(resolved_action)
      )) return false;
  if (action && action_size > 0) copy_utf8(action, action_size, resolved_action);
  return pet_p4_miniapp_dispatch_action(resolved_action, now_ms);
}

void pet_p4_miniapp_process(uint64_t now_ms) {
  bool changed = false;
  portENTER_CRITICAL(&g_runtime_lock);
  if (!g_runtime.active) goto done;
  for (int i = 0; i < g_runtime.tick_count; i += 1) {
    miniapp_tick_t *tick = &g_runtime.ticks[i];
    if (tick->last_ms == 0) {
      tick->last_ms = now_ms;
      continue;
    }
    if (tick->while_state >= 0 && tick->while_state != g_runtime.current_state) {
      tick->last_ms = now_ms;
      continue;
    }
    if (now_ms < tick->last_ms) {
      tick->last_ms = now_ms;
      continue;
    }
    uint64_t elapsed = now_ms - tick->last_ms;
    uint32_t steps = (uint32_t) (elapsed / tick->every_ms);
    if (steps == 0) continue;
    if (steps > 10) steps = 10;
    bool completed = false;
    for (uint32_t step = 0; step < steps; step += 1) {
      changed |= apply_effects(&g_runtime, tick->effects, tick->effect_count);
      if (tick->condition_var >= 0
          && g_runtime.vars[tick->condition_var].int_value <= tick->condition_lte) {
        if (tick->completion_state >= 0 && tick->completion_state != g_runtime.current_state) {
          g_runtime.current_state = tick->completion_state;
          changed = true;
        }
        changed |= apply_effects(
          &g_runtime,
          tick->completion_effects,
          tick->completion_effect_count
        );
        for (int reset = 0; reset < g_runtime.tick_count; reset += 1) {
          g_runtime.ticks[reset].last_ms = now_ms;
        }
        completed = true;
        break;
      }
    }
    if (!completed) tick->last_ms += (uint64_t) steps * tick->every_ms;
  }
  if (g_runtime.game.active
      && g_runtime.current_state == g_runtime.game.playing_state) {
    changed |= pet_p4_game_process(&g_runtime.game.engine, now_ms);
    changed |= sync_game_runtime(&g_runtime);
  }
  if (changed) refresh_view(&g_runtime);
done:
  portEXIT_CRITICAL(&g_runtime_lock);
}

bool pet_p4_miniapp_active(void) {
  bool active;
  portENTER_CRITICAL(&g_runtime_lock);
  active = g_runtime.active;
  portEXIT_CRITICAL(&g_runtime_lock);
  return active;
}

bool pet_p4_miniapp_active_id(char *out, size_t out_size) {
  bool active;
  if (!out || out_size == 0) return false;
  portENTER_CRITICAL(&g_runtime_lock);
  active = g_runtime.active && g_runtime.widget_id[0];
  copy_utf8(out, out_size, active ? g_runtime.widget_id : "");
  portEXIT_CRITICAL(&g_runtime_lock);
  return active && out[0];
}

bool pet_p4_miniapp_get_view(pet_p4_miniapp_view_t *out) {
  if (!out) return false;
  portENTER_CRITICAL(&g_runtime_lock);
  *out = g_runtime.view;
  portEXIT_CRITICAL(&g_runtime_lock);
  return true;
}

bool pet_p4_miniapp_get_sprites(pet_p4_miniapp_sprite_pack_t *out) {
  if (!out) return false;
  portENTER_CRITICAL(&g_runtime_lock);
  *out = g_runtime.sprites;
  portEXIT_CRITICAL(&g_runtime_lock);
  return out->count > 0;
}

bool pet_p4_miniapp_sync_stats(const pet_p4_stats_model_t *stats) {
  char total[24];
  char input[24];
  char output[24];
  char cache[24];
  char cache_label[40];
  char breakdown[64];
  const char *agent_label;
  bool changed = false;
  if (!stats || (stats->valid == 0 && !stats->source[0] && !stats->session_title[0])) return false;

  format_compact_u64(total, sizeof(total), stats->total_tokens,
                     stats->valid & PET_P4_STATS_HAS_TOTAL_TOKENS);
  format_compact_u64(input, sizeof(input), stats->input_tokens,
                     stats->valid & PET_P4_STATS_HAS_INPUT_TOKENS);
  format_compact_u64(output, sizeof(output), stats->output_tokens,
                     stats->valid & PET_P4_STATS_HAS_OUTPUT_TOKENS);
  format_compact_u64(cache, sizeof(cache), stats->cached_tokens,
                     stats->valid & PET_P4_STATS_HAS_CACHED_TOKENS);
  snprintf(cache_label, sizeof(cache_label), "缓存 %s", cache);
  if (stats->valid & PET_P4_STATS_HAS_CACHED_TOKENS) {
    snprintf(breakdown, sizeof(breakdown), "IN %s  OUT %s  CACHE %s", input, output, cache);
  } else {
    snprintf(breakdown, sizeof(breakdown), "IN %s  OUT %s", input, output);
  }
  if (strcmp(stats->source, "codex") == 0) {
    agent_label = "ChatGPT (Codex)";
  } else if (strcmp(stats->source, "claude-code") == 0 || strcmp(stats->source, "claude") == 0) {
    agent_label = "Claude";
  } else if (strcmp(stats->source, "mimocode") == 0 || strcmp(stats->source, "mimo-code") == 0) {
    agent_label = "MiMoCode";
  } else if (strcmp(stats->source, "openclaw") == 0) {
    agent_label = "OpenClaw";
  } else {
    agent_label = stats->source[0] ? stats->source : "Agent";
  }

  portENTER_CRITICAL(&g_runtime_lock);
  if (g_runtime.active && strcmp(g_runtime.widget_id, "token-usage") == 0) {
    changed |= set_string_var(&g_runtime, "agent_label", agent_label);
    changed |= set_string_var(&g_runtime, "total_display", total);
    changed |= set_string_var(&g_runtime, "input_display", input);
    changed |= set_string_var(&g_runtime, "output_display", output);
    changed |= set_string_var(&g_runtime, "cache_display", cache_label);
    changed |= set_string_var(&g_runtime, "headline_text", "今日消耗");
    changed |= set_string_var(&g_runtime, "breakdown_text", breakdown);
    if (changed) refresh_view(&g_runtime);
  }
  portEXIT_CRITICAL(&g_runtime_lock);
  return changed;
}

static bool read_file(const char *path, char *buffer, size_t capacity) {
  FILE *file = fopen(path, "rb");
  size_t len;
  if (!file) return false;
  len = fread(buffer, 1, capacity - 1, file);
  bool overflow = !feof(file);
  fclose(file);
  if (overflow) return false;
  buffer[len] = '\0';
  return true;
}

static bool read_binary_file(
  const char *path,
  uint8_t *buffer,
  size_t capacity,
  size_t *out_length
) {
  FILE *file;
  size_t length;
  bool overflow;
  if (!path || !buffer || capacity == 0 || !out_length) return false;
  file = fopen(path, "rb");
  if (!file) return false;
  length = fread(buffer, 1, capacity, file);
  overflow = length == capacity && fgetc(file) != EOF;
  fclose(file);
  if (overflow) return false;
  *out_length = length;
  return true;
}

bool pet_p4_miniapp_installed_id(char *out, size_t out_size) {
  return pet_p4_miniapp_active_id(out, out_size);
}

static bool write_file_atomic(const char *tmp_path, const char *path, const char *data, size_t len) {
  FILE *file = fopen(tmp_path, "wb");
  if (!file) return false;
  bool ok = fwrite(data, 1, len, file) == len && fflush(file) == 0;
  fclose(file);
  /*
   * SPIFFS garbage collection can take several seconds when the appearance
   * partition is populated. Let the idle task run between transactional file
   * writes so component commit never starves the task watchdog.
   */
  vTaskDelay(pdMS_TO_TICKS(1));
  if (!ok) {
    remove(tmp_path);
    return false;
  }
  remove(path);
  if (rename(tmp_path, path) != 0) {
    remove(tmp_path);
    return false;
  }
  return true;
}

static uint32_t miniapp_checksum(const char *data, size_t len) {
  uint32_t checksum = 2166136261U;
  if (!data && len > 0) return 0;
  for (size_t i = 0; i < len; i += 1) {
    checksum ^= (uint8_t) data[i];
    checksum *= 16777619U;
  }
  return checksum;
}

static uint32_t sprite_pack_checksum(const pet_p4_miniapp_sprite_pack_t *pack) {
  uint32_t checksum = 2166136261U;
  if (!pack || pack->count == 0) return 0;
#define SPRITE_CHECKSUM_BYTES(pointer, length) do { \
    const uint8_t *bytes__ = (const uint8_t *) (pointer); \
    for (size_t byte__ = 0; byte__ < (length); byte__ += 1) { \
      checksum ^= bytes__[byte__]; \
      checksum *= 16777619U; \
    } \
  } while (0)
  SPRITE_CHECKSUM_BYTES(&pack->count, sizeof(pack->count));
  for (uint8_t index = 0; index < pack->count; index += 1) {
    const pet_p4_miniapp_sprite_t *sprite = &pack->items[index];
    SPRITE_CHECKSUM_BYTES(sprite->id, strlen(sprite->id) + 1);
    SPRITE_CHECKSUM_BYTES(&sprite->frame_width, 1);
    SPRITE_CHECKSUM_BYTES(&sprite->frame_height, 1);
    SPRITE_CHECKSUM_BYTES(&sprite->frames, 1);
    SPRITE_CHECKSUM_BYTES(&sprite->fps, 1);
    if ((size_t) sprite->data_offset + sprite->data_length > pack->data_length) return 0;
    SPRITE_CHECKSUM_BYTES(pack->pixels + sprite->data_offset, sprite->data_length);
  }
#undef SPRITE_CHECKSUM_BYTES
  return checksum;
}

static bool parse_checksum_hex(const cJSON *object, const char *key, uint32_t *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  uint32_t value = 0;
  if (!out || !cJSON_IsString(item) || strlen(item->valuestring) != 8) return false;
  for (size_t i = 0; i < 8; i += 1) {
    char ch = item->valuestring[i];
    uint32_t digit;
    if (ch >= '0' && ch <= '9') digit = (uint32_t) (ch - '0');
    else if (ch >= 'a' && ch <= 'f') digit = (uint32_t) (ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') digit = (uint32_t) (ch - 'A' + 10);
    else return false;
    value = (value << 4U) | digit;
  }
  *out = value;
  return true;
}

static void catalog_slot_path(
  size_t slot,
  uint8_t package_generation,
  bool buttons,
  bool temporary,
  char *out,
  size_t out_size
) {
  const bool alternate = package_generation == 1;
  snprintf(
    out,
    out_size,
    buttons
      ? (alternate
          ? (temporary ? "/spiffs/p4b%02u.t1" : "/spiffs/p4b%02u.g1")
          : (temporary ? "/spiffs/p4b%02u.tmp" : "/spiffs/p4b%02u.json"))
      : (alternate
          ? (temporary ? "/spiffs/p4w%02u.t1" : "/spiffs/p4w%02u.g1")
          : (temporary ? "/spiffs/p4w%02u.tmp" : "/spiffs/p4w%02u.json")),
    (unsigned int) slot
  );
}

static void catalog_sprite_path(
  size_t slot,
  uint8_t package_generation,
  uint8_t sprite_index,
  bool temporary,
  char *out,
  size_t out_size
) {
  snprintf(
    out,
    out_size,
    temporary ? "/spiffs/p4s%02u-%u-%u.tmp" : "/spiffs/p4s%02u-%u-%u.bin",
    (unsigned int) slot,
    (unsigned int) package_generation,
    (unsigned int) sprite_index
  );
}

static int sprite_pack_find(
  const pet_p4_miniapp_sprite_pack_t *pack,
  const char *id
) {
  if (!pack || !id) return -1;
  for (uint8_t index = 0; index < pack->count; index += 1) {
    if (strcmp(pack->items[index].id, id) == 0) return (int) index;
  }
  return -1;
}

static bool append_sprite_file(
  pet_p4_miniapp_sprite_pack_t *pack,
  const char *id,
  const uint8_t *file_data,
  size_t file_length,
  char *error,
  size_t error_size
) {
  size_t pixel_count;
  size_t pixel_bytes;
  pet_p4_miniapp_sprite_t *sprite;
  if (!pack || !safe_id(id, PET_P4_MINIAPP_SPRITE_ID_MAX)
      || !file_data || file_length < MINIAPP_SPRITE_FILE_HEADER_SIZE) {
    set_error(error, error_size, "sprite file envelope is invalid");
    return false;
  }
  if (sprite_pack_find(pack, id) >= 0) return true;
  if (pack->count >= PET_P4_MINIAPP_SPRITE_MAX
      || memcmp(file_data, "P4S1", 4) != 0) {
    set_error(error, error_size, "sprite file header is unsupported");
    return false;
  }
  const uint8_t frame_width = file_data[4];
  const uint8_t frame_height = file_data[5];
  const uint8_t frames = file_data[6];
  const uint8_t fps = file_data[7];
  if (frame_width < MINIAPP_SPRITE_FRAME_MIN || frame_width > MINIAPP_SPRITE_FRAME_MAX
      || frame_height < MINIAPP_SPRITE_FRAME_MIN || frame_height > MINIAPP_SPRITE_FRAME_MAX
      || frames == 0 || frames > MINIAPP_SPRITE_FRAMES_MAX
      || fps == 0 || fps > MINIAPP_SPRITE_FPS_MAX) {
    set_error(error, error_size, "sprite frame declaration is out of bounds");
    return false;
  }
  pixel_count = (size_t) frame_width * frame_height * frames;
  pixel_bytes = pixel_count * 3U;
  if (file_length != MINIAPP_SPRITE_FILE_HEADER_SIZE + pixel_bytes
      || (size_t) pack->data_length + pixel_bytes > sizeof(pack->pixels)) {
    set_error(error, error_size, "sprite pixel payload is invalid or too large");
    return false;
  }
  sprite = &pack->items[pack->count];
  memset(sprite, 0, sizeof(*sprite));
  copy_utf8(sprite->id, sizeof(sprite->id), id);
  sprite->frame_width = frame_width;
  sprite->frame_height = frame_height;
  sprite->frames = frames;
  sprite->fps = fps;
  sprite->data_offset = pack->data_length;
  sprite->data_length = (uint16_t) pixel_bytes;
  memcpy(
    pack->pixels + pack->data_length,
    file_data + MINIAPP_SPRITE_FILE_HEADER_SIZE,
    pixel_bytes
  );
  pack->data_length = (uint16_t) ((size_t) pack->data_length + pixel_bytes);
  pack->count += 1;
  return true;
}

static bool load_sprite_pack(
  const char *widget_json,
  size_t slot,
  uint8_t package_generation,
  pet_p4_miniapp_sprite_pack_t *pack,
  char *error,
  size_t error_size
) {
  cJSON *root = NULL;
  const cJSON *sprites;
  uint8_t *file_data = NULL;
  bool ok = false;
  if (!widget_json || !pack) return false;
  memset(pack, 0, sizeof(*pack));
  root = cJSON_Parse(widget_json);
  sprites = root
    ? cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "scene"),
        "sprites"
      )
    : NULL;
  if (!sprites) {
    ok = true;
    goto done;
  }
  if (!cJSON_IsArray(sprites)
      || cJSON_GetArraySize(sprites) > PET_P4_MINIAPP_SPRITE_MAX) {
    set_error(error, error_size, "scene.sprites is invalid");
    goto done;
  }
  file_data = (uint8_t *) malloc(MINIAPP_SPRITE_FILE_MAX);
  if (!file_data) {
    set_error(error, error_size, "not enough memory to open component sprites");
    goto done;
  }
  for (int index = 0; index < cJSON_GetArraySize(sprites); index += 1) {
    const cJSON *declaration = cJSON_GetArrayItem(sprites, index);
    const char *id = json_string(declaration, "id");
    char path[48];
    size_t file_length = 0;
    catalog_sprite_path(slot, package_generation, (uint8_t) index, false, path, sizeof(path));
    if (!read_binary_file(path, file_data, MINIAPP_SPRITE_FILE_MAX, &file_length)
        || !append_sprite_file(pack, id, file_data, file_length, error, error_size)) {
      if (!error || !error[0]) set_error(error, error_size, "component sprite file is missing");
      goto done;
    }
  }
  ok = true;

done:
  free(file_data);
  cJSON_Delete(root);
  return ok;
}

static bool persist_sprite_pack(
  size_t slot,
  uint8_t package_generation,
  const pet_p4_miniapp_sprite_pack_t *pack
) {
  uint8_t *file_data = NULL;
  bool ok = true;
  if (!pack) return false;
  file_data = (uint8_t *) malloc(MINIAPP_SPRITE_FILE_MAX);
  if (!file_data && pack->count > 0) return false;
  for (uint8_t index = 0; index < PET_P4_MINIAPP_SPRITE_MAX; index += 1) {
    char path[48];
    char temporary_path[48];
    catalog_sprite_path(slot, package_generation, index, false, path, sizeof(path));
    catalog_sprite_path(slot, package_generation, index, true, temporary_path, sizeof(temporary_path));
    if (index >= pack->count) {
      remove(temporary_path);
      remove(path);
      continue;
    }
    const pet_p4_miniapp_sprite_t *sprite = &pack->items[index];
    if ((size_t) sprite->data_offset + sprite->data_length > pack->data_length) {
      ok = false;
      break;
    }
    memcpy(file_data, "P4S1", 4);
    file_data[4] = sprite->frame_width;
    file_data[5] = sprite->frame_height;
    file_data[6] = sprite->frames;
    file_data[7] = sprite->fps;
    memcpy(
      file_data + MINIAPP_SPRITE_FILE_HEADER_SIZE,
      pack->pixels + sprite->data_offset,
      sprite->data_length
    );
    ok = write_file_atomic(
      temporary_path,
      path,
      (const char *) file_data,
      MINIAPP_SPRITE_FILE_HEADER_SIZE + sprite->data_length
    );
    if (!ok) break;
  }
  free(file_data);
  return ok;
}

static void catalog_snapshot_path(
  uint8_t file_generation,
  bool temporary,
  char *out,
  size_t out_size
) {
  snprintf(
    out,
    out_size,
    temporary ? "/spiffs/p4-miniapps-%u.tmp" : "/spiffs/p4-miniapps-%u.json",
    (unsigned int) file_generation
  );
}

static int catalog_find(
  const miniapp_catalog_item_t *catalog,
  size_t count,
  const char *widget_id
) {
  if (!catalog || !widget_id) return -1;
  for (size_t i = 0; i < count; i += 1) {
    if (strcmp(catalog[i].widget_id, widget_id) == 0) return (int) i;
  }
  return -1;
}

static int catalog_first_free_slot(
  const miniapp_catalog_item_t *catalog,
  size_t count
) {
  bool used[PET_P4_MINIAPP_CATALOG_MAX] = {0};
  for (size_t i = 0; i < count; i += 1) {
    if (catalog[i].slot < PET_P4_MINIAPP_CATALOG_MAX) used[catalog[i].slot] = true;
  }
  for (size_t slot = 0; slot < PET_P4_MINIAPP_CATALOG_MAX; slot += 1) {
    if (!used[slot]) return (int) slot;
  }
  return -1;
}

static bool persist_catalog_snapshot(
  const miniapp_catalog_item_t *catalog,
  size_t count,
  const char *active_widget_id
) {
  cJSON *root = NULL;
  cJSON *items = NULL;
  char *json = NULL;
  bool ok = false;
  char catalog_path[48];
  char catalog_tmp_path[48];
  uint8_t target_generation;
  uint32_t next_sequence;
  if (count > PET_P4_MINIAPP_CATALOG_MAX) return false;
  if (active_widget_id && active_widget_id[0]
      && catalog_find(catalog, count, active_widget_id) < 0) {
    return false;
  }
  if (g_catalog_sequence == UINT32_MAX) return false;
  root = cJSON_CreateObject();
  items = cJSON_CreateArray();
  target_generation = g_catalog_file_generation < 0
    ? 0
    : (uint8_t) (1 - g_catalog_file_generation);
  next_sequence = g_catalog_sequence + 1U;
  if (!root || !items) goto done;
  cJSON_AddNumberToObject(root, "version", MINIAPP_CATALOG_VERSION);
  cJSON_AddNumberToObject(root, "sequence", next_sequence);
  if (active_widget_id && active_widget_id[0]) {
    cJSON_AddStringToObject(root, "activeWidgetId", active_widget_id);
  } else {
    cJSON_AddNullToObject(root, "activeWidgetId");
  }
  cJSON_AddItemToObject(root, "items", items);
  items = NULL;
  for (size_t i = 0; i < count; i += 1) {
    cJSON *item = cJSON_CreateObject();
    char widget_checksum[9];
    char buttons_checksum[9];
    char sprites_checksum[9];
    if (!item) goto done;
    snprintf(widget_checksum, sizeof(widget_checksum), "%08x", catalog[i].widget_checksum);
    snprintf(buttons_checksum, sizeof(buttons_checksum), "%08x", catalog[i].buttons_checksum);
    snprintf(sprites_checksum, sizeof(sprites_checksum), "%08x", catalog[i].sprites_checksum);
    cJSON_AddStringToObject(item, "widgetId", catalog[i].widget_id);
    cJSON_AddStringToObject(item, "title", catalog[i].title);
    cJSON_AddNumberToObject(item, "slot", catalog[i].slot);
    cJSON_AddNumberToObject(item, "packageGeneration", catalog[i].package_generation);
    cJSON_AddStringToObject(item, "widgetChecksum", widget_checksum);
    cJSON_AddStringToObject(item, "buttonsChecksum", buttons_checksum);
    cJSON_AddStringToObject(item, "spritesChecksum", sprites_checksum);
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root, "items"), item);
  }
  json = cJSON_PrintUnformatted(root);
  if (!json || strlen(json) >= MINIAPP_CATALOG_JSON_MAX) goto done;
  catalog_snapshot_path(target_generation, false, catalog_path, sizeof(catalog_path));
  catalog_snapshot_path(target_generation, true, catalog_tmp_path, sizeof(catalog_tmp_path));
  ok = write_file_atomic(
    catalog_tmp_path,
    catalog_path,
    json,
    strlen(json)
  );
  if (ok) {
    g_catalog_file_generation = target_generation;
    g_catalog_sequence = next_sequence;
  }

done:
  cJSON_free(json);
  cJSON_Delete(items);
  cJSON_Delete(root);
  return ok;
}

static bool load_catalog_runtime_item(
  miniapp_catalog_item_t *item,
  miniapp_runtime_t *runtime,
  bool allow_missing_checksums,
  char *error,
  size_t error_size
) {
  char widget_path[40];
  char buttons_path[40];
  char *widget_json;
  char *buttons_json;
  uint32_t widget_checksum;
  uint32_t buttons_checksum;
  uint32_t sprites_checksum;
  pet_p4_miniapp_sprite_pack_t *sprites = NULL;
  bool ok = false;
  if (!item || !runtime || item->slot >= PET_P4_MINIAPP_CATALOG_MAX
      || item->package_generation >= MINIAPP_PACKAGE_GENERATION_COUNT) {
    return false;
  }
  widget_json = (char *) calloc(1, MINIAPP_WIDGET_JSON_MAX);
  buttons_json = (char *) calloc(1, MINIAPP_BUTTONS_JSON_MAX);
  sprites = (pet_p4_miniapp_sprite_pack_t *) calloc(1, sizeof(*sprites));
  if (!widget_json || !buttons_json || !sprites) {
    set_error(error, error_size, "not enough memory to open component");
    goto done;
  }
  catalog_slot_path(
    item->slot,
    item->package_generation,
    false,
    false,
    widget_path,
    sizeof(widget_path)
  );
  catalog_slot_path(
    item->slot,
    item->package_generation,
    true,
    false,
    buttons_path,
    sizeof(buttons_path)
  );
  if (!read_file(widget_path, widget_json, MINIAPP_WIDGET_JSON_MAX)
      || !read_file(buttons_path, buttons_json, MINIAPP_BUTTONS_JSON_MAX)) {
    set_error(error, error_size, "component package is missing");
    goto done;
  }
  widget_checksum = miniapp_checksum(widget_json, strlen(widget_json));
  buttons_checksum = miniapp_checksum(buttons_json, strlen(buttons_json));
  if (!load_sprite_pack(
        widget_json,
        item->slot,
        item->package_generation,
        sprites,
        error,
        error_size
      )) {
    goto done;
  }
  sprites_checksum = sprite_pack_checksum(sprites);
  if (!allow_missing_checksums
      && (widget_checksum != item->widget_checksum
          || buttons_checksum != item->buttons_checksum
          || sprites_checksum != item->sprites_checksum)) {
    set_error(error, error_size, "component package checksum mismatch");
    goto done;
  }
  if (allow_missing_checksums) {
    item->widget_checksum = widget_checksum;
    item->buttons_checksum = buttons_checksum;
    item->sprites_checksum = sprites_checksum;
  }
  ok = parse_runtime(
    runtime,
    item->widget_id,
    widget_json,
    buttons_json,
    sprites,
    error,
    error_size
  );

done:
  free(widget_json);
  free(buttons_json);
  free(sprites);
  return ok;
}

static bool validate_catalog_snapshot(miniapp_catalog_snapshot_t *snapshot) {
  miniapp_runtime_t *runtime;
  char error[128] = {0};
  if (!snapshot) return false;
  runtime = (miniapp_runtime_t *) calloc(1, sizeof(*runtime));
  if (!runtime) return false;
  for (size_t i = 0; i < snapshot->count; i += 1) {
    bool missing_checksums = snapshot->items[i].widget_checksum == 0
      || snapshot->items[i].buttons_checksum == 0;
    memset(runtime, 0, sizeof(*runtime));
    error[0] = '\0';
    if (!load_catalog_runtime_item(
          &snapshot->items[i],
          runtime,
          missing_checksums,
          error,
          sizeof(error)
        )) {
      ESP_LOGW(
        TAG,
        "rejected component catalog generation=%u sequence=%u item=%s: %s",
        (unsigned int) snapshot->file_generation,
        (unsigned int) snapshot->sequence,
        snapshot->items[i].widget_id,
        error
      );
      free(runtime);
      return false;
    }
    snapshot->needs_upgrade |= missing_checksums;
  }
  free(runtime);
  snapshot->valid = true;
  return true;
}

static bool load_catalog_snapshot(
  uint8_t file_generation,
  miniapp_catalog_snapshot_t *snapshot
) {
  char *json = (char *) calloc(1, MINIAPP_CATALOG_JSON_MAX);
  cJSON *root = NULL;
  const cJSON *items;
  const cJSON *version;
  const cJSON *sequence;
  const cJSON *active_widget_id;
  char path[48];
  size_t count = 0;
  if (!snapshot) return false;
  if (file_generation >= MINIAPP_CATALOG_GENERATION_COUNT) return false;
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->file_generation = file_generation;
  catalog_snapshot_path(file_generation, false, path, sizeof(path));
  if (!json || !read_file(path, json, MINIAPP_CATALOG_JSON_MAX)) goto fail;
  root = cJSON_Parse(json);
  version = cJSON_GetObjectItemCaseSensitive(root, "version");
  sequence = cJSON_GetObjectItemCaseSensitive(root, "sequence");
  if (!cJSON_IsObject(root) || !cJSON_IsNumber(version)
      || (version->valueint != 2 && version->valueint != MINIAPP_CATALOG_VERSION)
      || !cJSON_IsNumber(sequence) || sequence->valuedouble < 1
      || sequence->valuedouble > UINT32_MAX) {
    goto fail;
  }
  snapshot->parsed = true;
  snapshot->needs_upgrade = version->valueint != MINIAPP_CATALOG_VERSION;
  snapshot->sequence = (uint32_t) sequence->valuedouble;
  active_widget_id = cJSON_GetObjectItemCaseSensitive(root, "activeWidgetId");
  if (cJSON_IsString(active_widget_id)) {
    if (!safe_widget_id(active_widget_id->valuestring)) goto fail;
    copy_utf8(
      snapshot->active_widget_id,
      sizeof(snapshot->active_widget_id),
      active_widget_id->valuestring
    );
  } else if (!cJSON_IsNull(active_widget_id)) {
    goto fail;
  }
  items = cJSON_GetObjectItemCaseSensitive(root, "items");
  if (!cJSON_IsArray(items)) goto fail;
  const cJSON *item;
  cJSON_ArrayForEach(item, items) {
    const char *widget_id;
    const cJSON *stored_slot;
    const cJSON *stored_package_generation;
    int slot;
    int package_generation;
    if (count >= PET_P4_MINIAPP_CATALOG_MAX || !cJSON_IsObject(item)) goto fail;
    widget_id = json_string(item, "widgetId");
    stored_slot = cJSON_GetObjectItemCaseSensitive(item, "slot");
    stored_package_generation = cJSON_GetObjectItemCaseSensitive(item, "packageGeneration");
    if (!safe_widget_id(widget_id) || !cJSON_IsNumber(stored_slot)
        || !cJSON_IsNumber(stored_package_generation)) {
      goto fail;
    }
    slot = stored_slot->valueint;
    package_generation = stored_package_generation->valueint;
    if (slot < 0 || slot >= PET_P4_MINIAPP_CATALOG_MAX
        || package_generation < 0
        || package_generation >= MINIAPP_PACKAGE_GENERATION_COUNT) {
      goto fail;
    }
    for (size_t used = 0; used < count; used += 1) {
      if (snapshot->items[used].slot == (uint8_t) slot
          || strcmp(snapshot->items[used].widget_id, widget_id) == 0) {
        goto fail;
      }
    }
    copy_utf8(
      snapshot->items[count].widget_id,
      sizeof(snapshot->items[count].widget_id),
      widget_id
    );
    copy_utf8(
      snapshot->items[count].title,
      sizeof(snapshot->items[count].title),
      json_string(item, "title")
    );
    snapshot->items[count].slot = (uint8_t) slot;
    snapshot->items[count].package_generation = (uint8_t) package_generation;
    if (!parse_checksum_hex(item, "widgetChecksum", &snapshot->items[count].widget_checksum)
        || !parse_checksum_hex(item, "buttonsChecksum", &snapshot->items[count].buttons_checksum)
        || (version->valueint >= 3
            && !parse_checksum_hex(
              item,
              "spritesChecksum",
              &snapshot->items[count].sprites_checksum
            ))) {
      goto fail;
    }
    count += 1;
  }
  snapshot->count = count;
  if (snapshot->active_widget_id[0]
      && catalog_find(snapshot->items, count, snapshot->active_widget_id) < 0) {
    snapshot->active_widget_id[0] = '\0';
    snapshot->needs_upgrade = true;
  }
  if (!validate_catalog_snapshot(snapshot)) goto fail;
  cJSON_Delete(root);
  free(json);
  return true;

fail:
  cJSON_Delete(root);
  free(json);
  return false;
}

static bool json_int_range(
  const cJSON *item,
  int minimum,
  int maximum,
  int *out
) {
  int value;
  if (!cJSON_IsNumber(item)) return false;
  value = (int) item->valuedouble;
  if (item->valuedouble != (double) value || value < minimum || value > maximum) {
    return false;
  }
  if (out) *out = value;
  return true;
}

static int bounded_find_entity(
  const pet_p4_bounded_game_config_t *config,
  const char *id
) {
  if (!config || !id) return -1;
  for (int index = 0; index < config->entity_count; index += 1) {
    if (strcmp(config->entities[index].id, id) == 0) return index;
  }
  return -1;
}

static bool parse_bounded_sprites(
  miniapp_runtime_t *runtime,
  const cJSON *sprites,
  bool runtime_v4,
  char *error,
  size_t error_size
) {
  static const char *const allowed[] = {
    "id", "asset", "frame_width", "frame_height", "frames", "fps",
  };
  if (!sprites) {
    if (runtime && runtime->sprites.count > 0) {
      set_error(error, error_size, "component contains undeclared sprite data");
      return false;
    }
    return true;
  }
  if (!runtime_v4 || !runtime || !cJSON_IsArray(sprites)
      || cJSON_GetArraySize(sprites) > PET_P4_MINIAPP_SPRITE_MAX
      || cJSON_GetArraySize(sprites) != runtime->sprites.count) {
    set_error(error, error_size, "scene.sprites requires the v4 runtime and matching sprite files");
    return false;
  }
  for (int index = 0; index < cJSON_GetArraySize(sprites); index += 1) {
    const cJSON *declaration = cJSON_GetArrayItem(sprites, index);
    const char *id = json_string(declaration, "id");
    const char *asset = json_string(declaration, "asset");
    int frame_width;
    int frame_height;
    int frames;
    int fps;
    if (!object_keys_allowed(
          declaration,
          allowed,
          sizeof(allowed) / sizeof(allowed[0])
        )
        || !safe_id(id, PET_P4_MINIAPP_SPRITE_ID_MAX)
        || strncmp(asset, "assets/", 7) != 0
        || strlen(asset) < 11
        || strcmp(asset + strlen(asset) - 4, ".png") != 0
        || !json_int_range(
          cJSON_GetObjectItemCaseSensitive(declaration, "frame_width"),
          MINIAPP_SPRITE_FRAME_MIN,
          MINIAPP_SPRITE_FRAME_MAX,
          &frame_width
        )
        || !json_int_range(
          cJSON_GetObjectItemCaseSensitive(declaration, "frame_height"),
          MINIAPP_SPRITE_FRAME_MIN,
          MINIAPP_SPRITE_FRAME_MAX,
          &frame_height
        )
        || !json_int_range(
          cJSON_GetObjectItemCaseSensitive(declaration, "frames"),
          1,
          MINIAPP_SPRITE_FRAMES_MAX,
          &frames
        )
        || !json_int_range(
          cJSON_GetObjectItemCaseSensitive(declaration, "fps"),
          1,
          MINIAPP_SPRITE_FPS_MAX,
          &fps
        )) {
      set_error(error, error_size, "scene sprite declaration is invalid");
      return false;
    }
    const pet_p4_miniapp_sprite_t *sprite = &runtime->sprites.items[index];
    if (strcmp(sprite->id, id) != 0
        || sprite->frame_width != frame_width
        || sprite->frame_height != frame_height
        || sprite->frames != frames
        || sprite->fps != fps) {
      set_error(error, error_size, "scene sprite metadata does not match its compiled asset");
      return false;
    }
  }
  return true;
}

static bool parse_bounded_grid(
  pet_p4_bounded_game_config_t *config,
  const cJSON *grid,
  char *error,
  size_t error_size
) {
  static const char *const allowed[] = {
    "width", "height", "rows", "solid_tones",
  };
  const cJSON *rows;
  const cJSON *solid_tones;
  int width;
  int height;
  if (!config || !object_keys_allowed(grid, allowed, sizeof(allowed) / sizeof(allowed[0]))
      || !json_int_range(cJSON_GetObjectItemCaseSensitive(grid, "width"), 4, 16, &width)
      || !json_int_range(cJSON_GetObjectItemCaseSensitive(grid, "height"), 4, 16, &height)) {
    set_error(error, error_size, "game.grid requires width/height in 4-16");
    return false;
  }
  config->width = (uint8_t) width;
  config->height = (uint8_t) height;
  rows = cJSON_GetObjectItemCaseSensitive(grid, "rows");
  if (rows) {
    if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) != height) {
      set_error(error, error_size, "game.grid.rows must match grid height");
      return false;
    }
    for (int y = 0; y < height; y += 1) {
      const cJSON *row = cJSON_GetArrayItem(rows, y);
      if (!cJSON_IsString(row) || strlen(row->valuestring) != (size_t) width) {
        set_error(error, error_size, "game.grid row width is invalid");
        return false;
      }
      for (int x = 0; x < width; x += 1) {
        char tone = row->valuestring[x];
        if (tone < '0' || tone > '4') {
          set_error(error, error_size, "game.grid rows only allow tones 0-4");
          return false;
        }
        config->base_cells[y * width + x] = (uint8_t) (tone - '0');
      }
    }
  }
  solid_tones = cJSON_GetObjectItemCaseSensitive(grid, "solid_tones");
  if (solid_tones) {
    const cJSON *tone;
    if (!cJSON_IsArray(solid_tones) || cJSON_GetArraySize(solid_tones) > 4) {
      set_error(error, error_size, "game.grid.solid_tones must be a bounded array");
      return false;
    }
    cJSON_ArrayForEach(tone, solid_tones) {
      int value;
      if (!json_int_range(tone, 1, 4, &value)
          || (config->solid_tone_mask & (1U << value)) != 0) {
        set_error(error, error_size, "game.grid.solid_tones is invalid or duplicated");
        return false;
      }
      config->solid_tone_mask |= (uint8_t) (1U << value);
    }
  }
  return true;
}

static bool parse_bounded_entities(
  pet_p4_bounded_game_config_t *config,
  const pet_p4_miniapp_sprite_pack_t *sprites,
  const cJSON *entities,
  char *error,
  size_t error_size
) {
  static const char *const allowed[] = {
    "id", "x", "y", "width", "height", "tone", "vx", "vy",
    "bounds", "shape", "sprite", "active", "collidable",
  };
  const cJSON *item;
  if (!config || !cJSON_IsArray(entities)
      || cJSON_GetArraySize(entities) <= 0
      || cJSON_GetArraySize(entities) > PET_P4_GAME_MAX_ENTITIES) {
    set_error(error, error_size, "game.entities must be a non-empty bounded array");
    return false;
  }
  cJSON_ArrayForEach(item, entities) {
    pet_p4_game_entity_t *entity;
    const char *id;
    const char *bounds;
    const char *shape;
    const cJSON *value;
    int number;
    if (!object_keys_allowed(item, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
      set_error(error, error_size, "game entity contains an unsupported field");
      return false;
    }
    id = json_string(item, "id");
    if (!safe_id(id, PET_P4_GAME_ENTITY_ID_MAX)
        || bounded_find_entity(config, id) >= 0) {
      set_error(error, error_size, "game entity id is invalid or duplicated");
      return false;
    }
    entity = &config->entities[config->entity_count];
    copy_utf8(entity->id, sizeof(entity->id), id);
    entity->width = 1;
    entity->height = 1;
    entity->tone = 1;
    entity->shape = PET_P4_GAME_SHAPE_RECT;
    entity->sprite_index = -1;
    entity->active = true;
    entity->collidable = true;
    if (!json_int_range(cJSON_GetObjectItemCaseSensitive(item, "x"), 0, 15, &number)) {
      set_error(error, error_size, "game entity x is invalid");
      return false;
    }
    entity->x = (int8_t) number;
    if (!json_int_range(cJSON_GetObjectItemCaseSensitive(item, "y"), 0, 15, &number)) {
      set_error(error, error_size, "game entity y is invalid");
      return false;
    }
    entity->y = (int8_t) number;
    value = cJSON_GetObjectItemCaseSensitive(item, "width");
    if (value && !json_int_range(value, 1, 8, &number)) {
      set_error(error, error_size, "game entity width is invalid");
      return false;
    }
    if (value) entity->width = (uint8_t) number;
    value = cJSON_GetObjectItemCaseSensitive(item, "height");
    if (value && !json_int_range(value, 1, 8, &number)) {
      set_error(error, error_size, "game entity height is invalid");
      return false;
    }
    if (value) entity->height = (uint8_t) number;
    value = cJSON_GetObjectItemCaseSensitive(item, "tone");
    if (value && !json_int_range(value, 1, 4, &number)) {
      set_error(error, error_size, "game entity tone is invalid");
      return false;
    }
    if (value) entity->tone = (uint8_t) number;
    value = cJSON_GetObjectItemCaseSensitive(item, "vx");
    if (value && !json_int_range(value, -4, 4, &number)) {
      set_error(error, error_size, "game entity vx is invalid");
      return false;
    }
    if (value) entity->vx = (int8_t) number;
    value = cJSON_GetObjectItemCaseSensitive(item, "vy");
    if (value && !json_int_range(value, -4, 4, &number)) {
      set_error(error, error_size, "game entity vy is invalid");
      return false;
    }
    if (value) entity->vy = (int8_t) number;
    bounds = json_string(item, "bounds");
    if (!bounds[0] || strcmp(bounds, "clamp") == 0) {
      entity->bounds = PET_P4_GAME_BOUNDS_CLAMP;
    } else if (strcmp(bounds, "wrap") == 0) {
      entity->bounds = PET_P4_GAME_BOUNDS_WRAP;
    } else if (strcmp(bounds, "bounce") == 0) {
      entity->bounds = PET_P4_GAME_BOUNDS_BOUNCE;
    } else if (strcmp(bounds, "hide") == 0) {
      entity->bounds = PET_P4_GAME_BOUNDS_HIDE;
    } else if (strcmp(bounds, "stop") == 0) {
      entity->bounds = PET_P4_GAME_BOUNDS_STOP;
    } else {
      set_error(error, error_size, "game entity bounds is unsupported");
      return false;
    }
    shape = json_string(item, "shape");
    value = cJSON_GetObjectItemCaseSensitive(item, "shape");
    if (value && !cJSON_IsString(value)) {
      set_error(error, error_size, "game entity shape must be text");
      return false;
    }
    if (!shape[0] || strcmp(shape, "rect") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_RECT;
    } else if (strcmp(shape, "player-ship") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_PLAYER_SHIP;
    } else if (strcmp(shape, "enemy-ship") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_ENEMY_SHIP;
    } else if (strcmp(shape, "bullet") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_BULLET;
    } else if (strcmp(shape, "star") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_STAR;
    } else if (strcmp(shape, "paddle") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_PADDLE;
    } else if (strcmp(shape, "ball") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_BALL;
    } else if (strcmp(shape, "circle") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_CIRCLE;
    } else if (strcmp(shape, "capsule") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_CAPSULE;
    } else if (strcmp(shape, "triangle") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_TRIANGLE;
    } else if (strcmp(shape, "diamond") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_DIAMOND;
    } else if (strcmp(shape, "heart") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_HEART;
    } else if (strcmp(shape, "cloud") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_CLOUD;
    } else if (strcmp(shape, "coin") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_COIN;
    } else if (strcmp(shape, "character") == 0) {
      entity->shape = PET_P4_GAME_SHAPE_CHARACTER;
    } else {
      set_error(error, error_size, "game entity shape is unsupported");
      return false;
    }
    const char *sprite_id = json_string(item, "sprite");
    value = cJSON_GetObjectItemCaseSensitive(item, "sprite");
    if (value && !cJSON_IsString(value)) {
      set_error(error, error_size, "game entity sprite must be text");
      return false;
    }
    if (sprite_id[0]) {
      int sprite_index = sprite_pack_find(sprites, sprite_id);
      if (sprite_index < 0) {
        set_error(error, error_size, "game entity references an unknown sprite");
        return false;
      }
      entity->sprite_index = (int8_t) sprite_index;
    }
    value = cJSON_GetObjectItemCaseSensitive(item, "active");
    if (value && !cJSON_IsBool(value)) {
      set_error(error, error_size, "game entity active must be boolean");
      return false;
    }
    if (value) entity->active = cJSON_IsTrue(value);
    value = cJSON_GetObjectItemCaseSensitive(item, "collidable");
    if (value && !cJSON_IsBool(value)) {
      set_error(error, error_size, "game entity collidable must be boolean");
      return false;
    }
    if (value) entity->collidable = cJSON_IsTrue(value);
    if (entity->x + entity->width > config->width
        || entity->y + entity->height > config->height) {
      set_error(error, error_size, "game entity starts outside the grid");
      return false;
    }
    config->entity_count += 1;
  }
  return true;
}

static bool parse_bounded_coord(
  const cJSON *item,
  bool *has_value,
  bool *random,
  int8_t *minimum,
  int8_t *maximum
) {
  int first;
  int last;
  if (!item) {
    *has_value = false;
    return true;
  }
  *has_value = true;
  if (json_int_range(item, 0, 15, &first)) {
    *random = false;
    *minimum = (int8_t) first;
    *maximum = (int8_t) first;
    return true;
  }
  if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 2
      || !json_int_range(cJSON_GetArrayItem(item, 0), 0, 15, &first)
      || !json_int_range(cJSON_GetArrayItem(item, 1), 0, 15, &last)
      || last < first) {
    return false;
  }
  *random = true;
  *minimum = (int8_t) first;
  *maximum = (int8_t) last;
  return true;
}

static bool parse_bounded_entity_ref(
  const pet_p4_bounded_game_config_t *config,
  const cJSON *object,
  const char *field,
  int8_t *out
) {
  const char *id = json_string(object, field);
  int index = bounded_find_entity(config, id);
  if (index < 0) return false;
  *out = (int8_t) index;
  return true;
}

static bool parse_bounded_op(
  const pet_p4_bounded_game_config_t *config,
  const cJSON *object,
  pet_p4_game_op_t *op,
  char *error,
  size_t error_size
) {
  const char *name;
  const char *axis;
  const cJSON *value;
  int number;
  if (!config || !cJSON_IsObject(object) || !op) return false;
  memset(op, 0, sizeof(*op));
  op->entity_index = -1;
  op->source_index = -1;
  name = json_string(object, "op");
  if (strcmp(name, "move") == 0) {
    static const char *const allowed[] = {"op", "entity", "dx", "dy"};
    if (!object_keys_allowed(object, allowed, sizeof(allowed) / sizeof(allowed[0]))
        || !parse_bounded_entity_ref(config, object, "entity", &op->entity_index)
        || !json_int_range(cJSON_GetObjectItemCaseSensitive(object, "dx"), -4, 4, &number)) {
      set_error(error, error_size, "game move op is invalid");
      return false;
    }
    op->dx = (int8_t) number;
    if (!json_int_range(cJSON_GetObjectItemCaseSensitive(object, "dy"), -4, 4, &number)
        || (op->dx == 0 && number == 0)) {
      set_error(error, error_size, "game move op requires bounded dx/dy");
      return false;
    }
    op->dy = (int8_t) number;
    op->kind = PET_P4_GAME_OP_MOVE;
    return true;
  }
  if (strcmp(name, "velocity") == 0 || strcmp(name, "accelerate") == 0) {
    static const char *const allowed[] = {"op", "entity", "vx", "vy"};
    if (!object_keys_allowed(object, allowed, sizeof(allowed) / sizeof(allowed[0]))
        || !parse_bounded_entity_ref(config, object, "entity", &op->entity_index)
        || !json_int_range(cJSON_GetObjectItemCaseSensitive(object, "vx"), -4, 4, &number)) {
      set_error(error, error_size, "game velocity op is invalid");
      return false;
    }
    op->vx = (int8_t) number;
    if (!json_int_range(cJSON_GetObjectItemCaseSensitive(object, "vy"), -4, 4, &number)) {
      set_error(error, error_size, "game velocity op requires bounded vx/vy");
      return false;
    }
    op->vy = (int8_t) number;
    op->kind = strcmp(name, "velocity") == 0
      ? PET_P4_GAME_OP_VELOCITY : PET_P4_GAME_OP_ACCELERATE;
    return true;
  }
  if (strcmp(name, "place") == 0) {
    static const char *const allowed[] = {
      "op", "entity", "source", "x", "y", "dx", "dy",
    };
    if (!object_keys_allowed(object, allowed, sizeof(allowed) / sizeof(allowed[0]))
        || !parse_bounded_entity_ref(config, object, "entity", &op->entity_index)
        || !parse_bounded_coord(
          cJSON_GetObjectItemCaseSensitive(object, "x"),
          &op->has_x, &op->random_x, &op->x_min, &op->x_max
        )
        || !parse_bounded_coord(
          cJSON_GetObjectItemCaseSensitive(object, "y"),
          &op->has_y, &op->random_y, &op->y_min, &op->y_max
        )) {
      set_error(error, error_size, "game place op is invalid");
      return false;
    }
    if (cJSON_GetObjectItemCaseSensitive(object, "source")
        && !parse_bounded_entity_ref(config, object, "source", &op->source_index)) {
      set_error(error, error_size, "game place source is unknown");
      return false;
    }
    value = cJSON_GetObjectItemCaseSensitive(object, "dx");
    if (value && !json_int_range(value, -4, 4, &number)) {
      set_error(error, error_size, "game place dx is invalid");
      return false;
    }
    if (value) op->dx = (int8_t) number;
    value = cJSON_GetObjectItemCaseSensitive(object, "dy");
    if (value && !json_int_range(value, -4, 4, &number)) {
      set_error(error, error_size, "game place dy is invalid");
      return false;
    }
    if (value) op->dy = (int8_t) number;
    if (!op->has_x && !op->has_y && op->source_index < 0) {
      set_error(error, error_size, "game place requires coordinates or a source entity");
      return false;
    }
    op->kind = PET_P4_GAME_OP_PLACE;
    return true;
  }
  if (strcmp(name, "show") == 0 || strcmp(name, "hide") == 0) {
    static const char *const allowed[] = {"op", "entity"};
    if (!object_keys_allowed(object, allowed, sizeof(allowed) / sizeof(allowed[0]))
        || !parse_bounded_entity_ref(config, object, "entity", &op->entity_index)) {
      set_error(error, error_size, "game show/hide op is invalid");
      return false;
    }
    op->kind = strcmp(name, "show") == 0 ? PET_P4_GAME_OP_SHOW : PET_P4_GAME_OP_HIDE;
    return true;
  }
  if (strcmp(name, "score") == 0) {
    static const char *const allowed[] = {"op", "add", "set"};
    const cJSON *add = cJSON_GetObjectItemCaseSensitive(object, "add");
    const cJSON *set = cJSON_GetObjectItemCaseSensitive(object, "set");
    if (!object_keys_allowed(object, allowed, sizeof(allowed) / sizeof(allowed[0]))
        || ((add != NULL) == (set != NULL))
        || !json_int_range(add ? add : set, -10000, 10000, &number)) {
      set_error(error, error_size, "game score op requires exactly one bounded add/set");
      return false;
    }
    op->kind = add ? PET_P4_GAME_OP_SCORE_ADD : PET_P4_GAME_OP_SCORE_SET;
    op->value = (int16_t) number;
    return true;
  }
  if (strcmp(name, "run") == 0 || strcmp(name, "stop") == 0
      || strcmp(name, "restart") == 0) {
    static const char *const allowed[] = {"op"};
    if (!object_keys_allowed(object, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
      set_error(error, error_size, "game lifecycle op contains an unsupported field");
      return false;
    }
    op->kind = strcmp(name, "run") == 0
      ? PET_P4_GAME_OP_RUN
      : strcmp(name, "stop") == 0 ? PET_P4_GAME_OP_STOP : PET_P4_GAME_OP_RESTART;
    return true;
  }
  if (strcmp(name, "bounce") == 0) {
    static const char *const allowed[] = {"op", "entity", "axis"};
    axis = json_string(object, "axis");
    if (!object_keys_allowed(object, allowed, sizeof(allowed) / sizeof(allowed[0]))
        || !parse_bounded_entity_ref(config, object, "entity", &op->entity_index)) {
      set_error(error, error_size, "game bounce op is invalid");
      return false;
    }
    if (strcmp(axis, "x") == 0) op->axis_mask = 1;
    else if (strcmp(axis, "y") == 0) op->axis_mask = 2;
    else if (strcmp(axis, "both") == 0) op->axis_mask = 3;
    else {
      set_error(error, error_size, "game bounce axis must be x/y/both");
      return false;
    }
    op->kind = PET_P4_GAME_OP_BOUNCE;
    return true;
  }
  if (strcmp(name, "tone") == 0) {
    static const char *const allowed[] = {"op", "entity", "tone"};
    if (!object_keys_allowed(object, allowed, sizeof(allowed) / sizeof(allowed[0]))
        || !parse_bounded_entity_ref(config, object, "entity", &op->entity_index)
        || !json_int_range(cJSON_GetObjectItemCaseSensitive(object, "tone"), 1, 4, &number)) {
      set_error(error, error_size, "game tone op is invalid");
      return false;
    }
    op->tone = (uint8_t) number;
    op->kind = PET_P4_GAME_OP_TONE;
    return true;
  }
  set_error(error, error_size, "game rule contains an unsupported op");
  return false;
}

static pet_p4_game_edge_t bounded_edge_from_name(const char *name) {
  if (!name || !name[0] || strcmp(name, "any") == 0) return PET_P4_GAME_EDGE_NONE;
  if (strcmp(name, "left") == 0) return PET_P4_GAME_EDGE_LEFT;
  if (strcmp(name, "right") == 0) return PET_P4_GAME_EDGE_RIGHT;
  if (strcmp(name, "top") == 0) return PET_P4_GAME_EDGE_TOP;
  if (strcmp(name, "bottom") == 0) return PET_P4_GAME_EDGE_BOTTOM;
  return (pet_p4_game_edge_t) -1;
}

static bool parse_bounded_rules(
  const miniapp_runtime_t *runtime,
  pet_p4_bounded_game_config_t *config,
  const cJSON *rules,
  char *error,
  size_t error_size
) {
  static const char *const allowed[] = {"on", "entity", "with", "edge", "do"};
  const cJSON *item;
  if (!cJSON_IsArray(rules) || cJSON_GetArraySize(rules) <= 0
      || cJSON_GetArraySize(rules) > PET_P4_GAME_MAX_RULES) {
    set_error(error, error_size, "game.rules must be a non-empty bounded array");
    return false;
  }
  cJSON_ArrayForEach(item, rules) {
    pet_p4_game_rule_t *rule;
    const char *on;
    const cJSON *ops;
    if (!object_keys_allowed(item, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
      set_error(error, error_size, "game rule contains an unsupported field");
      return false;
    }
    on = json_string(item, "on");
    rule = &config->rules[config->rule_count];
    rule->entity_index = -1;
    rule->with_index = -1;
    if (strcmp(on, "tick") == 0) {
      rule->trigger = PET_P4_GAME_TRIGGER_TICK;
    } else if (strcmp(on, "collision") == 0) {
      rule->trigger = PET_P4_GAME_TRIGGER_COLLISION;
      if (!parse_bounded_entity_ref(config, item, "entity", &rule->entity_index)
          || !parse_bounded_entity_ref(config, item, "with", &rule->with_index)
          || rule->entity_index == rule->with_index) {
        set_error(error, error_size, "collision rule requires two known entities");
        return false;
      }
    } else if (strcmp(on, "edge") == 0 || strcmp(on, "blocked") == 0) {
      rule->trigger = strcmp(on, "edge") == 0
        ? PET_P4_GAME_TRIGGER_EDGE : PET_P4_GAME_TRIGGER_BLOCKED;
      if (!parse_bounded_entity_ref(config, item, "entity", &rule->entity_index)) {
        set_error(error, error_size, "edge/blocked rule requires a known entity");
        return false;
      }
      rule->edge = bounded_edge_from_name(json_string(item, "edge"));
      if ((int) rule->edge < 0 || (rule->trigger == PET_P4_GAME_TRIGGER_BLOCKED
          && rule->edge != PET_P4_GAME_EDGE_NONE)) {
        set_error(error, error_size, "game rule edge is unsupported");
        return false;
      }
    } else {
      if (!safe_id(on, PET_P4_GAME_ACTION_MAX)
          || !runtime_has_transition_action(runtime, on)
          || !runtime_has_button_action(runtime, on)) {
        set_error(error, error_size, "game action rule must match a transition and button");
        return false;
      }
      rule->trigger = PET_P4_GAME_TRIGGER_ACTION;
      copy_utf8(rule->action, sizeof(rule->action), on);
    }
    ops = cJSON_GetObjectItemCaseSensitive(item, "do");
    if (!cJSON_IsArray(ops) || cJSON_GetArraySize(ops) <= 0
        || cJSON_GetArraySize(ops) > PET_P4_GAME_MAX_OPS_PER_RULE) {
      set_error(error, error_size, "game rule do must contain 1-4 operations");
      return false;
    }
    for (int op_index = 0; op_index < cJSON_GetArraySize(ops); op_index += 1) {
      if (!parse_bounded_op(
            config,
            cJSON_GetArrayItem(ops, op_index),
            &rule->ops[rule->op_count],
            error,
            error_size
          )) {
        return false;
      }
      rule->op_count += 1;
    }
    config->rule_count += 1;
  }
  return true;
}

static bool parse_bounded_scene(
  miniapp_runtime_t *runtime,
  const cJSON *scene,
  bool runtime_v4,
  char *error,
  size_t error_size
) {
  static const char *const allowed[] = {
    "tick_ms", "active_state", "result_state", "score_var", "auto_start",
    "grid", "sprites", "entities", "rules",
  };
  pet_p4_bounded_game_config_t config;
  const cJSON *tick;
  const cJSON *auto_start;
  const char *score_var;
  const char *result_state;
  int tick_ms;
  if (!scene) return true;
  if (!runtime || !object_keys_allowed(scene, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
    set_error(error, error_size, "scene contains an unsupported field");
    return false;
  }
  memset(&config, 0, sizeof(config));
  runtime->game.score_var = -1;
  runtime->game.playing_state = (int8_t) find_name(
    runtime->states,
    runtime->state_count,
    json_string(scene, "active_state")
  );
  runtime->game.result_state = -1;
  if (runtime->game.playing_state < 0) {
    set_error(error, error_size, "scene.active_state must reference a known state");
    return false;
  }
  score_var = json_string(scene, "score_var");
  if (score_var[0]) {
    runtime->game.score_var = (int8_t) find_var(runtime, score_var);
    if (runtime->game.score_var < 0
        || runtime->vars[runtime->game.score_var].type != MINIAPP_VAR_INT) {
      set_error(error, error_size, "scene.score_var must reference an int var");
      return false;
    }
  }
  result_state = json_string(scene, "result_state");
  if (result_state[0]) {
    runtime->game.result_state = (int8_t) find_name(
      runtime->states,
      runtime->state_count,
      result_state
    );
    if (runtime->game.result_state < 0) {
      set_error(error, error_size, "scene.result_state must reference a known state");
      return false;
    }
  }
  tick = cJSON_GetObjectItemCaseSensitive(scene, "tick_ms");
  if (!json_int_range(tick, 100, 2000, &tick_ms)) {
    set_error(error, error_size, "scene.tick_ms must be in 100-2000");
    return false;
  }
  config.tick_ms = (uint32_t) tick_ms;
  if (!parse_bounded_sprites(
        runtime,
        cJSON_GetObjectItemCaseSensitive(scene, "sprites"),
        runtime_v4,
        error,
        error_size
      )
      || !parse_bounded_grid(
        &config,
        cJSON_GetObjectItemCaseSensitive(scene, "grid"),
        error,
        error_size
      )
      || !parse_bounded_entities(
        &config,
        &runtime->sprites,
        cJSON_GetObjectItemCaseSensitive(scene, "entities"),
        error,
        error_size
      )
      || !parse_bounded_rules(
        runtime,
        &config,
        cJSON_GetObjectItemCaseSensitive(scene, "rules"),
        error,
        error_size
      )) {
    return false;
  }
  auto_start = cJSON_GetObjectItemCaseSensitive(scene, "auto_start");
  if (auto_start && !cJSON_IsBool(auto_start)) {
    set_error(error, error_size, "scene.auto_start must be boolean");
    return false;
  }
  if (cJSON_IsTrue(auto_start)
      && runtime->current_state != runtime->game.playing_state) {
    set_error(error, error_size, "scene.auto_start requires initial_state == active_state");
    return false;
  }
  if (!pet_p4_game_configure_bounded(
        &runtime->game.engine,
        &config,
        stable_widget_seed(runtime->widget_id)
      )) {
    set_error(error, error_size, "bounded scene configuration is invalid");
    return false;
  }
  if (cJSON_IsTrue(auto_start)) {
    runtime->game.engine.running = true;
    runtime->game.engine.game_over = false;
  }
  runtime->game.active = true;
  return true;
}

static bool load_legacy_catalog(miniapp_catalog_snapshot_t *snapshot) {
  char *json = (char *) calloc(1, MINIAPP_CATALOG_JSON_MAX);
  cJSON *root = NULL;
  const cJSON *items;
  char active_widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX + 1] = {0};
  size_t count = 0;
  if (!snapshot) return false;
  memset(snapshot, 0, sizeof(*snapshot));
  if (!json || !read_file(MINIAPP_CATALOG_PATH, json, MINIAPP_CATALOG_JSON_MAX)) goto fail;
  root = cJSON_Parse(json);
  items = cJSON_GetObjectItemCaseSensitive(root, "items");
  if (!cJSON_IsObject(root) || !cJSON_IsArray(items)) goto fail;
  const cJSON *item;
  cJSON_ArrayForEach(item, items) {
    const char *widget_id;
    const cJSON *stored_slot;
    int slot;
    if (count >= PET_P4_MINIAPP_CATALOG_MAX || !cJSON_IsObject(item)) goto fail;
    widget_id = json_string(item, "widgetId");
    stored_slot = cJSON_GetObjectItemCaseSensitive(item, "slot");
    slot = cJSON_IsNumber(stored_slot) ? stored_slot->valueint : (int) count;
    if (!safe_widget_id(widget_id) || slot < 0 || slot >= PET_P4_MINIAPP_CATALOG_MAX) goto fail;
    for (size_t used = 0; used < count; used += 1) {
      if (snapshot->items[used].slot == (uint8_t) slot
          || strcmp(snapshot->items[used].widget_id, widget_id) == 0) {
        goto fail;
      }
    }
    copy_utf8(snapshot->items[count].widget_id, sizeof(snapshot->items[count].widget_id), widget_id);
    copy_utf8(
      snapshot->items[count].title,
      sizeof(snapshot->items[count].title),
      json_string(item, "title")
    );
    snapshot->items[count].slot = (uint8_t) slot;
    snapshot->items[count].package_generation = 0;
    count += 1;
  }
  snapshot->count = count;
  snapshot->needs_upgrade = true;
  if (read_file(MINIAPP_ID_PATH, active_widget_id, sizeof(active_widget_id))) {
    char *newline = strpbrk(active_widget_id, "\r\n");
    if (newline) *newline = '\0';
    if (catalog_find(snapshot->items, count, active_widget_id) >= 0) {
      copy_utf8(
        snapshot->active_widget_id,
        sizeof(snapshot->active_widget_id),
        active_widget_id
      );
    }
  }
  if (!validate_catalog_snapshot(snapshot)) goto fail;
  cJSON_Delete(root);
  free(json);
  return true;

fail:
  cJSON_Delete(root);
  free(json);
  return false;
}

static bool load_catalog_runtime(
  size_t index,
  miniapp_runtime_t *runtime,
  char *error,
  size_t error_size
) {
  if (!runtime || index >= g_catalog_count) return false;
  return load_catalog_runtime_item(&g_catalog[index], runtime, false, error, error_size);
}

static bool activate_catalog_index_in_memory(size_t index) {
  miniapp_runtime_t *next;
  char error[128] = {0};
  if (index >= g_catalog_count) return false;
  next = (miniapp_runtime_t *) calloc(1, sizeof(*next));
  if (!next) return false;
  if (!load_catalog_runtime(index, next, error, sizeof(error))) {
    ESP_LOGW(TAG, "failed to open component %s: %s", g_catalog[index].widget_id, error);
    free(next);
    return false;
  }
  portENTER_CRITICAL(&g_runtime_lock);
  g_runtime = *next;
  g_catalog_selected = index;
  portEXIT_CRITICAL(&g_runtime_lock);
  free(next);
  ESP_LOGI(
    TAG,
    "opened component id=%s slot=%u",
    g_runtime.widget_id,
    (unsigned int) g_catalog[index].slot
  );
  return true;
}

static bool activate_catalog_index(size_t index) {
  miniapp_runtime_t *next;
  char error[128] = {0};
  if (index >= g_catalog_count) return false;
  next = (miniapp_runtime_t *) calloc(1, sizeof(*next));
  if (!next) return false;
  if (!load_catalog_runtime(index, next, error, sizeof(error))) {
    ESP_LOGW(TAG, "failed to open component %s: %s", g_catalog[index].widget_id, error);
    free(next);
    return false;
  }
  if (!persist_catalog_snapshot(g_catalog, g_catalog_count, g_catalog[index].widget_id)) {
    ESP_LOGW(TAG, "failed to commit active component id=%s", g_catalog[index].widget_id);
    free(next);
    return false;
  }
  portENTER_CRITICAL(&g_runtime_lock);
  g_runtime = *next;
  g_catalog_selected = index;
  portEXIT_CRITICAL(&g_runtime_lock);
  free(next);
  ESP_LOGI(
    TAG,
    "opened component id=%s slot=%u catalog-generation=%d sequence=%u",
    g_runtime.widget_id,
    (unsigned int) g_catalog[index].slot,
    g_catalog_file_generation,
    (unsigned int) g_catalog_sequence
  );
  return true;
}

esp_err_t pet_p4_miniapp_init(void) {
  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX + 1] = {0};
  char error[128] = {0};
  miniapp_catalog_snapshot_t *snapshot0;
  miniapp_catalog_snapshot_t *snapshot1;
  miniapp_catalog_snapshot_t *selected = NULL;
  uint32_t highest_seen_sequence = 0;
  memset(&g_runtime, 0, sizeof(g_runtime));
  memset(&g_staging, 0, sizeof(g_staging));
  memset(g_catalog, 0, sizeof(g_catalog));
  g_catalog_count = 0;
  g_catalog_selected = 0;
  g_catalog_file_generation = -1;
  g_catalog_sequence = 0;

  snapshot0 = (miniapp_catalog_snapshot_t *) calloc(1, sizeof(*snapshot0));
  snapshot1 = (miniapp_catalog_snapshot_t *) calloc(1, sizeof(*snapshot1));
  if (!snapshot0 || !snapshot1) {
    free(snapshot0);
    free(snapshot1);
    return ESP_ERR_NO_MEM;
  }
  (void) load_catalog_snapshot(0, snapshot0);
  (void) load_catalog_snapshot(1, snapshot1);
  if (snapshot0->parsed && snapshot0->sequence > highest_seen_sequence) {
    highest_seen_sequence = snapshot0->sequence;
  }
  if (snapshot1->parsed && snapshot1->sequence > highest_seen_sequence) {
    highest_seen_sequence = snapshot1->sequence;
  }
  if (snapshot0->valid) selected = snapshot0;
  if (snapshot1->valid && (!selected || snapshot1->sequence > selected->sequence)) {
    selected = snapshot1;
  }

  if (selected) {
    memcpy(g_catalog, selected->items, sizeof(g_catalog));
    g_catalog_count = selected->count;
    g_catalog_file_generation = selected->file_generation;
    g_catalog_sequence = highest_seen_sequence;
    copy_utf8(widget_id, sizeof(widget_id), selected->active_widget_id);
    if (!widget_id[0] && g_catalog_count > 0) {
      copy_utf8(widget_id, sizeof(widget_id), g_catalog[0].widget_id);
      selected->needs_upgrade = true;
    }
    int preferred = catalog_find(g_catalog, g_catalog_count, widget_id);
    if (preferred < 0 && g_catalog_count > 0) preferred = 0;
    bool recovered = selected->sequence < highest_seen_sequence;
    if ((selected->needs_upgrade || recovered)
        && !persist_catalog_snapshot(g_catalog, g_catalog_count, widget_id)) {
      ESP_LOGW(TAG, "component catalog recovery could not be committed");
    }
    bool activated = preferred < 0 || activate_catalog_index_in_memory((size_t) preferred);
    ESP_LOGI(
      TAG,
      "restored component catalog count=%u generation=%d sequence=%u%s",
      (unsigned int) g_catalog_count,
      g_catalog_file_generation,
      (unsigned int) g_catalog_sequence,
      recovered ? " after rollback" : ""
    );
    free(snapshot0);
    free(snapshot1);
    return activated ? ESP_OK : ESP_ERR_INVALID_STATE;
  }

  miniapp_catalog_snapshot_t *legacy = snapshot0;
  memset(legacy, 0, sizeof(*legacy));
  if (load_legacy_catalog(legacy)) {
    memcpy(g_catalog, legacy->items, sizeof(g_catalog));
    g_catalog_count = legacy->count;
    copy_utf8(widget_id, sizeof(widget_id), legacy->active_widget_id);
    if (!widget_id[0] && g_catalog_count > 0) {
      copy_utf8(widget_id, sizeof(widget_id), g_catalog[0].widget_id);
    }
    bool committed = persist_catalog_snapshot(g_catalog, g_catalog_count, widget_id);
    int preferred = catalog_find(g_catalog, g_catalog_count, widget_id);
    bool activated = preferred < 0 || activate_catalog_index_in_memory((size_t) preferred);
    free(snapshot0);
    free(snapshot1);
    if (!committed) {
      ESP_LOGW(TAG, "legacy component catalog is valid but migration could not be committed");
    }
    return activated ? ESP_OK : ESP_ERR_INVALID_STATE;
  }
  free(snapshot0);
  free(snapshot1);

  if (!read_file(MINIAPP_ID_PATH, widget_id, sizeof(widget_id))
      || !read_file(MINIAPP_WIDGET_PATH, g_staging.widget_json, sizeof(g_staging.widget_json))) {
    return ESP_OK;
  }
  (void) read_file(MINIAPP_BUTTONS_PATH, g_staging.buttons_json, sizeof(g_staging.buttons_json));
  char *newline = strpbrk(widget_id, "\r\n");
  if (newline) *newline = '\0';
  if (!parse_runtime(&g_runtime, widget_id, g_staging.widget_json,
                     g_staging.buttons_json, &g_staging.sprites, error, sizeof(error))) {
    ESP_LOGW(TAG, "ignored persisted mini-app: %s", error);
    memset(&g_runtime, 0, sizeof(g_runtime));
    memset(&g_staging, 0, sizeof(g_staging));
    return ESP_ERR_INVALID_STATE;
  }
  copy_utf8(g_catalog[0].widget_id, sizeof(g_catalog[0].widget_id), g_runtime.widget_id);
  copy_utf8(g_catalog[0].title, sizeof(g_catalog[0].title), g_runtime.view.title);
  g_catalog[0].slot = 0;
  g_catalog[0].package_generation = 0;
  g_catalog[0].widget_checksum = miniapp_checksum(
    g_staging.widget_json,
    strlen(g_staging.widget_json)
  );
  g_catalog[0].buttons_checksum = miniapp_checksum(
    g_staging.buttons_json,
    strlen(g_staging.buttons_json)
  );
  g_catalog[0].sprites_checksum = sprite_pack_checksum(&g_staging.sprites);
  g_catalog_count = 1;
  char widget_slot[40];
  char widget_tmp[40];
  char buttons_slot[40];
  char buttons_tmp[40];
  catalog_slot_path(0, 0, false, false, widget_slot, sizeof(widget_slot));
  catalog_slot_path(0, 0, false, true, widget_tmp, sizeof(widget_tmp));
  catalog_slot_path(0, 0, true, false, buttons_slot, sizeof(buttons_slot));
  catalog_slot_path(0, 0, true, true, buttons_tmp, sizeof(buttons_tmp));
  if (!write_file_atomic(widget_tmp, widget_slot, g_staging.widget_json, strlen(g_staging.widget_json))
      || !write_file_atomic(buttons_tmp, buttons_slot, g_staging.buttons_json, strlen(g_staging.buttons_json))
      || !persist_sprite_pack(0, 0, &g_staging.sprites)
      || !persist_catalog_snapshot(g_catalog, g_catalog_count, widget_id)) {
    ESP_LOGW(TAG, "legacy component opened but catalog migration could not be persisted");
  }
  memset(&g_staging, 0, sizeof(g_staging));
  ESP_LOGI(TAG, "migrated bounded component id=%s states=%u vars=%u", g_runtime.widget_id,
           (unsigned int) g_runtime.state_count, (unsigned int) g_runtime.var_count);
  return ESP_OK;
}

size_t pet_p4_miniapp_catalog_count(void) {
  size_t count;
  portENTER_CRITICAL(&g_runtime_lock);
  count = g_catalog_count;
  portEXIT_CRITICAL(&g_runtime_lock);
  return count;
}

size_t pet_p4_miniapp_catalog_selected(void) {
  size_t selected;
  portENTER_CRITICAL(&g_runtime_lock);
  selected = g_catalog_selected;
  portEXIT_CRITICAL(&g_runtime_lock);
  return selected;
}

bool pet_p4_miniapp_catalog_get(size_t index, pet_p4_miniapp_catalog_entry_t *out) {
  bool available = false;
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  portENTER_CRITICAL(&g_runtime_lock);
  if (index < g_catalog_count) {
    copy_utf8(out->widget_id, sizeof(out->widget_id), g_catalog[index].widget_id);
    copy_utf8(out->title, sizeof(out->title), g_catalog[index].title);
    out->active = g_runtime.active
      && strcmp(g_runtime.widget_id, g_catalog[index].widget_id) == 0;
    available = true;
  }
  portEXIT_CRITICAL(&g_runtime_lock);
  return available;
}

void pet_p4_miniapp_catalog_focus_active(void) {
  portENTER_CRITICAL(&g_runtime_lock);
  for (size_t i = 0; i < g_catalog_count; i += 1) {
    if (g_runtime.active && strcmp(g_runtime.widget_id, g_catalog[i].widget_id) == 0) {
      g_catalog_selected = i;
      break;
    }
  }
  portEXIT_CRITICAL(&g_runtime_lock);
}

bool pet_p4_miniapp_catalog_move(int delta) {
  bool moved = false;
  portENTER_CRITICAL(&g_runtime_lock);
  if (g_catalog_count > 0 && delta != 0) {
    size_t previous = g_catalog_selected;
    if (delta > 0) {
      g_catalog_selected = (g_catalog_selected + 1) % g_catalog_count;
    } else {
      g_catalog_selected = (g_catalog_selected + g_catalog_count - 1) % g_catalog_count;
    }
    moved = previous != g_catalog_selected || g_catalog_count == 1;
  }
  portEXIT_CRITICAL(&g_runtime_lock);
  return moved;
}

bool pet_p4_miniapp_catalog_activate_selected(void) {
  size_t selected = pet_p4_miniapp_catalog_selected();
  return activate_catalog_index(selected);
}

static bool transfer_matches(const cJSON *payload) {
  return g_staging.active
    && strcmp(json_string(payload, "transferId"), g_staging.transfer_id) == 0;
}

bool pet_p4_miniapp_install_begin(const cJSON *payload, char *error, size_t error_size) {
  const char *transfer_id = json_string(payload, "transferId");
  const char *widget_id = json_string(payload, "widgetId");
  memset(&g_staging, 0, sizeof(g_staging));
  if (!safe_id(transfer_id, sizeof(g_staging.transfer_id))
      || !safe_widget_id(widget_id)) {
    set_error(error, error_size, "invalid transferId or widgetId");
    return false;
  }
  g_staging.active = true;
  copy_utf8(g_staging.transfer_id, sizeof(g_staging.transfer_id), transfer_id);
  copy_utf8(g_staging.widget_id, sizeof(g_staging.widget_id), widget_id);
  return true;
}

static long json_index(const cJSON *payload) {
  const cJSON *index = cJSON_GetObjectItemCaseSensitive(payload, "index");
  if (cJSON_IsNumber(index)) return index->valueint;
  if (cJSON_IsString(index)) return strtol(index->valuestring, NULL, 10);
  return 0;
}

static uint64_t fnv1a64(const uint8_t *data, size_t len) {
  uint64_t checksum = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; i += 1) {
    checksum ^= data[i];
    checksum *= 0x00000100000001b3ULL;
  }
  return checksum;
}

static bool verify_chunk_integrity(
  const cJSON *payload,
  const uint8_t *decoded,
  size_t decoded_len,
  char *error,
  size_t error_size
) {
  const cJSON *size = cJSON_GetObjectItemCaseSensitive(payload, "decodedSize");
  const char *expected = json_string(payload, "checksum");
  if (size && (!cJSON_IsNumber(size) || size->valuedouble < 0
               || (size_t) size->valuedouble != decoded_len)) {
    set_error(error, error_size, "mini-app chunk decoded size mismatch");
    return false;
  }
  if (expected[0]) {
    char actual[17];
    snprintf(actual, sizeof(actual), "%016llx", (unsigned long long) fnv1a64(decoded, decoded_len));
    if (strlen(expected) != 16 || strcmp(actual, expected) != 0) {
      set_error(error, error_size, "mini-app chunk checksum mismatch");
      return false;
    }
  }
  return true;
}

static bool decode_chunk(
  const char *data,
  char *target,
  size_t capacity,
  size_t *used,
  size_t *decoded_now,
  bool reset,
  char *error,
  size_t error_size
) {
  size_t decoded_len = 0;
  size_t offset = reset ? 0 : *used;
  if (offset >= capacity - 1
      || mbedtls_base64_decode((unsigned char *) target + offset, capacity - 1 - offset,
                               &decoded_len, (const unsigned char *) data, strlen(data)) != 0) {
    set_error(error, error_size, "mini-app file exceeds its decoded size bound");
    return false;
  }
  *used = offset + decoded_len;
  if (decoded_now) *decoded_now = decoded_len;
  target[*used] = '\0';
  return true;
}

bool pet_p4_miniapp_install_chunk(const cJSON *payload, char *error, size_t error_size) {
  const char *path = json_string(payload, "path");
  const char *data = json_string(payload, "data");
  long index = json_index(payload);
  size_t decoded_len = 0;
  size_t decoded_offset = 0;
  if (!transfer_matches(payload)) {
    set_error(error, error_size, "mini-app chunk does not match the active transfer");
    return false;
  }
  if (!path[0] || !data[0] || index < 0) {
    set_error(error, error_size, "invalid mini-app chunk envelope");
    return false;
  }
  if (strcmp(path, "runtime/widget.json") == 0) {
    if ((uint32_t) index + 1U == g_staging.widget_next_index) return true;
    if ((uint32_t) index != g_staging.widget_next_index) {
      set_error(error, error_size, "mini-app widget chunk index is out of sequence");
      return false;
    }
    decoded_offset = g_staging.widget_len;
    if (!decode_chunk(data, g_staging.widget_json, sizeof(g_staging.widget_json),
                       &g_staging.widget_len, &decoded_len, index == 0, error, error_size)) {
      return false;
    }
    if (!verify_chunk_integrity(payload, (const uint8_t *) g_staging.widget_json + decoded_offset,
                                decoded_len, error, error_size)) {
      g_staging.widget_len = decoded_offset;
      g_staging.widget_json[decoded_offset] = '\0';
      return false;
    }
    g_staging.widget_next_index += 1U;
  } else if (strcmp(path, "buttons.json") == 0) {
    if ((uint32_t) index + 1U == g_staging.buttons_next_index) return true;
    if ((uint32_t) index != g_staging.buttons_next_index) {
      set_error(error, error_size, "mini-app buttons chunk index is out of sequence");
      return false;
    }
    decoded_offset = g_staging.buttons_len;
    if (!decode_chunk(data, g_staging.buttons_json, sizeof(g_staging.buttons_json),
                       &g_staging.buttons_len, &decoded_len, index == 0, error, error_size)) {
      return false;
    }
    if (!verify_chunk_integrity(payload, (const uint8_t *) g_staging.buttons_json + decoded_offset,
                                decoded_len, error, error_size)) {
      g_staging.buttons_len = decoded_offset;
      g_staging.buttons_json[decoded_offset] = '\0';
      return false;
    }
    g_staging.buttons_next_index += 1U;
  } else if (strncmp(path, "runtime/sprites/", 16) == 0) {
    const char *file_name = path + 16;
    const size_t file_name_length = strlen(file_name);
    char sprite_id[PET_P4_MINIAPP_SPRITE_ID_MAX] = {0};
    uint8_t *decoded = NULL;
    size_t decoded_size = 0;
    if (index != 0 || file_name_length <= 4
        || strcmp(file_name + file_name_length - 4, ".p4s") != 0
        || file_name_length - 4 >= sizeof(sprite_id)) {
      set_error(error, error_size, "sprite chunk path or index is invalid");
      return false;
    }
    memcpy(sprite_id, file_name, file_name_length - 4);
    if (!safe_id(sprite_id, sizeof(sprite_id))) {
      set_error(error, error_size, "sprite id is invalid");
      return false;
    }
    if (sprite_pack_find(&g_staging.sprites, sprite_id) >= 0) return true;
    decoded = (uint8_t *) malloc(MINIAPP_SPRITE_FILE_MAX);
    if (!decoded
        || mbedtls_base64_decode(
          decoded,
          MINIAPP_SPRITE_FILE_MAX,
          &decoded_size,
          (const unsigned char *) data,
          strlen(data)
        ) != 0) {
      free(decoded);
      set_error(error, error_size, "sprite file exceeds its decoded size bound");
      return false;
    }
    if (!verify_chunk_integrity(payload, decoded, decoded_size, error, error_size)
        || !append_sprite_file(
          &g_staging.sprites,
          sprite_id,
          decoded,
          decoded_size,
          error,
          error_size
        )) {
      free(decoded);
      return false;
    }
    free(decoded);
  } else if (strcmp(path, "component.json") != 0 && strcmp(path, "negative-screen.json") != 0
             && strcmp(path, "share.json") != 0) {
    set_error(error, error_size, "P4 mini-app package contains an unsupported file");
    return false;
  }
  return true;
}

bool pet_p4_miniapp_install_commit(const cJSON *payload, char *error, size_t error_size) {
  const char *widget_id = json_string(payload, "widgetId");
  miniapp_runtime_t *next;
  miniapp_catalog_item_t next_catalog[PET_P4_MINIAPP_CATALOG_MAX];
  size_t next_count;
  size_t catalog_index;
  int slot;
  uint8_t package_generation;
  bool new_component = false;
  char widget_path[40];
  char widget_tmp_path[40];
  char buttons_path[40];
  char buttons_tmp_path[40];
  if (!transfer_matches(payload) || strcmp(widget_id, g_staging.widget_id) != 0
      || g_staging.widget_len == 0) {
    set_error(error, error_size, "mini-app staging is incomplete or invalid");
    memset(&g_staging, 0, sizeof(g_staging));
    return false;
  }
  next = (miniapp_runtime_t *) calloc(1, sizeof(*next));
  if (!next) {
    set_error(error, error_size, "not enough memory to activate mini-app");
    return false;
  }
  if (!parse_runtime(next, g_staging.widget_id, g_staging.widget_json,
                     g_staging.buttons_json, &g_staging.sprites, error, error_size)) {
    free(next);
    memset(&g_staging, 0, sizeof(g_staging));
    return false;
  }
  memcpy(next_catalog, g_catalog, sizeof(next_catalog));
  next_count = g_catalog_count;
  int existing = catalog_find(next_catalog, next_count, g_staging.widget_id);
  if (existing >= 0) {
    catalog_index = (size_t) existing;
    slot = next_catalog[catalog_index].slot;
    package_generation = (uint8_t) (1 - next_catalog[catalog_index].package_generation);
  } else if (next_count < PET_P4_MINIAPP_CATALOG_MAX) {
    slot = catalog_first_free_slot(next_catalog, next_count);
    if (slot < 0) {
      free(next);
      set_error(error, error_size, "component center has no free storage slot");
      memset(&g_staging, 0, sizeof(g_staging));
      return false;
    }
    catalog_index = next_count++;
    next_catalog[catalog_index].slot = (uint8_t) slot;
    package_generation = 0;
    new_component = true;
  } else {
    free(next);
    set_error(error, error_size, "component center is full");
    memset(&g_staging, 0, sizeof(g_staging));
    return false;
  }
  copy_utf8(
    next_catalog[catalog_index].widget_id,
    sizeof(next_catalog[catalog_index].widget_id),
    g_staging.widget_id
  );
  copy_utf8(
    next_catalog[catalog_index].title,
    sizeof(next_catalog[catalog_index].title),
    next->view.title[0] ? next->view.title : g_staging.widget_id
  );
  next_catalog[catalog_index].package_generation = package_generation;
  next_catalog[catalog_index].widget_checksum = miniapp_checksum(
    g_staging.widget_json,
    g_staging.widget_len
  );
  next_catalog[catalog_index].buttons_checksum = miniapp_checksum(
    g_staging.buttons_json,
    g_staging.buttons_len
  );
  next_catalog[catalog_index].sprites_checksum = sprite_pack_checksum(&g_staging.sprites);
  if (new_component && !g_builtin_sync_in_progress && catalog_index > 0) {
    const miniapp_catalog_item_t installed = next_catalog[catalog_index];
    memmove(
      &next_catalog[1],
      &next_catalog[0],
      catalog_index * sizeof(next_catalog[0])
    );
    next_catalog[0] = installed;
    catalog_index = 0;
  }
  catalog_slot_path(
    (size_t) slot,
    package_generation,
    false,
    false,
    widget_path,
    sizeof(widget_path)
  );
  catalog_slot_path(
    (size_t) slot,
    package_generation,
    false,
    true,
    widget_tmp_path,
    sizeof(widget_tmp_path)
  );
  catalog_slot_path(
    (size_t) slot,
    package_generation,
    true,
    false,
    buttons_path,
    sizeof(buttons_path)
  );
  catalog_slot_path(
    (size_t) slot,
    package_generation,
    true,
    true,
    buttons_tmp_path,
    sizeof(buttons_tmp_path)
  );
  if (!write_file_atomic(widget_tmp_path, widget_path,
                         g_staging.widget_json, g_staging.widget_len)
      || !write_file_atomic(buttons_tmp_path, buttons_path,
                            g_staging.buttons_json, g_staging.buttons_len)
      || !persist_sprite_pack((size_t) slot, package_generation, &g_staging.sprites)
      || (memset(next, 0, sizeof(*next)),
          !load_catalog_runtime_item(
            &next_catalog[catalog_index],
            next,
            false,
            error,
            error_size
          ))
      || !persist_catalog_snapshot(next_catalog, next_count, g_staging.widget_id)) {
    free(next);
    if (!error || !error[0]) {
      set_error(error, error_size, "failed to commit mini-app package generation");
    }
    memset(&g_staging, 0, sizeof(g_staging));
    return false;
  }
  portENTER_CRITICAL(&g_runtime_lock);
  g_runtime = *next;
  memcpy(g_catalog, next_catalog, sizeof(g_catalog));
  g_catalog_count = next_count;
  g_catalog_selected = catalog_index;
  portEXIT_CRITICAL(&g_runtime_lock);
  free(next);
  ESP_LOGI(TAG, "installed bounded component id=%s slot=%u package-generation=%u catalog-generation=%d sequence=%u catalog=%u states=%u vars=%u transitions=%u ticks=%u",
           g_runtime.widget_id, (unsigned int) slot, (unsigned int) package_generation,
           g_catalog_file_generation, (unsigned int) g_catalog_sequence,
           (unsigned int) g_catalog_count,
           (unsigned int) g_runtime.state_count,
           (unsigned int) g_runtime.var_count, (unsigned int) g_runtime.transition_count,
           (unsigned int) g_runtime.tick_count);
  memset(&g_staging, 0, sizeof(g_staging));
  return true;
}

static bool commit_builtin_package(
  const char *widget_id,
  const char *widget_json,
  const char *buttons_json,
  const cJSON *sprite_files,
  char *error,
  size_t error_size
) {
  static const char *const transfer_id = "firmware-builtins";
  cJSON *payload;
  if (!safe_widget_id(widget_id) || !widget_json || !buttons_json) return false;
  memset(&g_staging, 0, sizeof(g_staging));
  g_staging.active = true;
  copy_utf8(g_staging.transfer_id, sizeof(g_staging.transfer_id), transfer_id);
  copy_utf8(g_staging.widget_id, sizeof(g_staging.widget_id), widget_id);
  g_staging.widget_len = strlen(widget_json);
  g_staging.buttons_len = strlen(buttons_json);
  memcpy(g_staging.widget_json, widget_json, g_staging.widget_len + 1);
  memcpy(g_staging.buttons_json, buttons_json, g_staging.buttons_len + 1);

  if (!cJSON_IsArray(sprite_files)
      || cJSON_GetArraySize(sprite_files) > PET_P4_MINIAPP_SPRITE_MAX) {
    memset(&g_staging, 0, sizeof(g_staging));
    set_error(error, error_size, "embedded built-in sprite list is invalid");
    return false;
  }
  const cJSON *sprite_file;
  cJSON_ArrayForEach(sprite_file, sprite_files) {
    const char *sprite_id = json_string(sprite_file, "id");
    const char *data = json_string(sprite_file, "data");
    char path[64];
    cJSON *chunk = NULL;
    if (!cJSON_IsObject(sprite_file)
        || !safe_id(sprite_id, PET_P4_MINIAPP_SPRITE_ID_MAX)
        || !data[0]) {
      memset(&g_staging, 0, sizeof(g_staging));
      set_error(error, error_size, "embedded built-in sprite file is invalid");
      return false;
    }
    snprintf(path, sizeof(path), "runtime/sprites/%s.p4s", sprite_id);
    chunk = cJSON_CreateObject();
    if (!chunk
        || !cJSON_AddStringToObject(chunk, "transferId", transfer_id)
        || !cJSON_AddStringToObject(chunk, "widgetId", widget_id)
        || !cJSON_AddStringToObject(chunk, "path", path)
        || !cJSON_AddStringToObject(chunk, "data", data)
        || !cJSON_AddNumberToObject(chunk, "index", 0)
        || !pet_p4_miniapp_install_chunk(chunk, error, error_size)) {
      cJSON_Delete(chunk);
      memset(&g_staging, 0, sizeof(g_staging));
      if (!error || !error[0]) set_error(error, error_size, "failed to stage built-in sprite");
      return false;
    }
    cJSON_Delete(chunk);
  }

  payload = cJSON_CreateObject();
  if (!payload
      || !cJSON_AddStringToObject(payload, "transferId", transfer_id)
      || !cJSON_AddStringToObject(payload, "widgetId", widget_id)) {
    cJSON_Delete(payload);
    memset(&g_staging, 0, sizeof(g_staging));
    set_error(error, error_size, "not enough memory to stage built-in component");
    return false;
  }
  bool committed = pet_p4_miniapp_install_commit(payload, error, error_size);
  cJSON_Delete(payload);
  return committed;
}

static bool restore_active_after_builtin_sync(const char *preferred_widget_id) {
  int index = preferred_widget_id && preferred_widget_id[0]
    ? catalog_find(g_catalog, g_catalog_count, preferred_widget_id)
    : -1;
  if (index < 0) index = catalog_find(g_catalog, g_catalog_count, "two-key-pong");
  if (index < 0 && g_catalog_count > 0) index = 0;
  return index < 0 || activate_catalog_index((size_t) index);
}

static bool builtin_bundle_contains_id(const cJSON *components, const char *widget_id) {
  const cJSON *package;
  if (!cJSON_IsArray(components) || !safe_widget_id(widget_id)) return false;
  cJSON_ArrayForEach(package, components) {
    if (strcmp(json_string(package, "id"), widget_id) == 0) return true;
  }
  return false;
}

static bool reorder_catalog_for_builtin_bundle(const cJSON *components) {
  miniapp_catalog_item_t ordered[PET_P4_MINIAPP_CATALOG_MAX] = {0};
  bool used[PET_P4_MINIAPP_CATALOG_MAX] = {0};
  size_t ordered_count = 0;
  const cJSON *package;
  if (!cJSON_IsArray(components)) return false;

  // User-created packages remain ahead of product defaults. With no user
  // packages this still yields the factory order (Pong first, Frog second).
  for (size_t index = 0; index < g_catalog_count; index += 1) {
    if (builtin_bundle_contains_id(components, g_catalog[index].widget_id)) continue;
    ordered[ordered_count++] = g_catalog[index];
    used[index] = true;
  }
  cJSON_ArrayForEach(package, components) {
    const int index = catalog_find(g_catalog, g_catalog_count, json_string(package, "id"));
    if (index < 0 || used[index]) continue;
    ordered[ordered_count++] = g_catalog[index];
    used[index] = true;
  }
  for (size_t index = 0; index < g_catalog_count; index += 1) {
    if (!used[index]) ordered[ordered_count++] = g_catalog[index];
  }
  if (ordered_count != g_catalog_count
      || memcmp(ordered, g_catalog, g_catalog_count * sizeof(g_catalog[0])) == 0) {
    return false;
  }

  portENTER_CRITICAL(&g_runtime_lock);
  memcpy(g_catalog, ordered, sizeof(g_catalog));
  int selected = g_runtime.active
    ? catalog_find(g_catalog, g_catalog_count, g_runtime.widget_id)
    : -1;
  g_catalog_selected = selected >= 0 ? (size_t) selected : 0;
  portEXIT_CRITICAL(&g_runtime_lock);
  return true;
}

esp_err_t pet_p4_miniapp_sync_builtins(void) {
  char marker[128] = {0};
  char restore_widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  char error[160] = {0};
  cJSON *bundle = NULL;
  const cJSON *bundle_version;
  const cJSON *components;
  bool catalog_changed = false;
  size_t updated = 0;

  if (read_file(MINIAPP_BUILTIN_MARKER_PATH, marker, sizeof(marker))
      && strcmp(marker, PET_P4_BUILD_ID) == 0) {
    return ESP_OK;
  }
  bundle = cJSON_Parse((const char *) pet_p4_builtin_components_json);
  bundle_version = cJSON_GetObjectItemCaseSensitive(bundle, "version");
  components = cJSON_GetObjectItemCaseSensitive(bundle, "components");
  if (!cJSON_IsObject(bundle)
      || !cJSON_IsNumber(bundle_version)
      || bundle_version->valueint != 1
      || !cJSON_IsArray(components)
      || cJSON_GetArraySize(components) != 8) {
    cJSON_Delete(bundle);
    ESP_LOGW(TAG, "embedded built-in component bundle is invalid");
    return ESP_ERR_INVALID_RESPONSE;
  }
  (void) pet_p4_miniapp_active_id(restore_widget_id, sizeof(restore_widget_id));

  for (size_t i = 0;
       i < sizeof(g_retired_builtin_ids) / sizeof(g_retired_builtin_ids[0]);
       i += 1) {
    if (catalog_find(g_catalog, g_catalog_count, g_retired_builtin_ids[i]) < 0) continue;
    error[0] = '\0';
    if (!pet_p4_miniapp_remove(g_retired_builtin_ids[i], error, sizeof(error))) {
      ESP_LOGW(TAG, "failed to retire built-in id=%s: %s", g_retired_builtin_ids[i], error);
      (void) restore_active_after_builtin_sync(restore_widget_id);
      cJSON_Delete(bundle);
      return ESP_FAIL;
    }
    catalog_changed = true;
  }

  const cJSON *package;
  g_builtin_sync_in_progress = true;
  cJSON_ArrayForEach(package, components) {
    const char *widget_id = json_string(package, "id");
    const cJSON *widget = cJSON_GetObjectItemCaseSensitive(package, "widget");
    const cJSON *buttons = cJSON_GetObjectItemCaseSensitive(package, "buttons");
    const cJSON *sprite_files = cJSON_GetObjectItemCaseSensitive(package, "spriteFiles");
    uint32_t sprites_checksum = 0;
    char *widget_json = cJSON_IsObject(widget) ? cJSON_PrintUnformatted(widget) : NULL;
    char *buttons_json = cJSON_IsArray(buttons) ? cJSON_PrintUnformatted(buttons) : NULL;
    if (!safe_widget_id(widget_id)
        || !widget_json
        || strlen(widget_json) >= MINIAPP_WIDGET_JSON_MAX
        || !buttons_json
        || strlen(buttons_json) >= MINIAPP_BUTTONS_JSON_MAX
        || !cJSON_IsArray(sprite_files)
        || !parse_checksum_hex(package, "spritesChecksum", &sprites_checksum)) {
      cJSON_free(widget_json);
      cJSON_free(buttons_json);
      ESP_LOGW(TAG, "embedded built-in id=%s exceeds runtime JSON limits", widget_id);
      g_builtin_sync_in_progress = false;
      (void) restore_active_after_builtin_sync(restore_widget_id);
      cJSON_Delete(bundle);
      return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t widget_checksum = miniapp_checksum(widget_json, strlen(widget_json));
    const uint32_t buttons_checksum = miniapp_checksum(buttons_json, strlen(buttons_json));
    const int existing = catalog_find(g_catalog, g_catalog_count, widget_id);
    const bool current = existing >= 0
      && g_catalog[existing].widget_checksum == widget_checksum
      && g_catalog[existing].buttons_checksum == buttons_checksum
      && g_catalog[existing].sprites_checksum == sprites_checksum;
    if (!current) {
      error[0] = '\0';
      if (!commit_builtin_package(
            widget_id,
            widget_json,
            buttons_json,
            sprite_files,
            error,
            sizeof(error)
          )) {
        cJSON_free(widget_json);
        cJSON_free(buttons_json);
        ESP_LOGW(TAG, "failed to sync built-in id=%s: %s", widget_id, error);
        g_builtin_sync_in_progress = false;
        (void) restore_active_after_builtin_sync(restore_widget_id);
        cJSON_Delete(bundle);
        return ESP_FAIL;
      }
      catalog_changed = true;
      updated += 1;
    }
    cJSON_free(widget_json);
    cJSON_free(buttons_json);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  g_builtin_sync_in_progress = false;

  if (reorder_catalog_for_builtin_bundle(components)) {
    catalog_changed = true;
  }

  if (catalog_changed && !restore_active_after_builtin_sync(restore_widget_id)) {
    ESP_LOGW(TAG, "built-in components synced but active component could not be restored");
    cJSON_Delete(bundle);
    return ESP_ERR_INVALID_STATE;
  }
  if (!write_file_atomic(
        MINIAPP_BUILTIN_MARKER_TMP_PATH,
        MINIAPP_BUILTIN_MARKER_PATH,
        PET_P4_BUILD_ID,
        strlen(PET_P4_BUILD_ID)
      )) {
    ESP_LOGW(TAG, "built-in component marker could not be committed");
    cJSON_Delete(bundle);
    return ESP_FAIL;
  }
  ESP_LOGI(
    TAG,
    "firmware built-ins ready build=%s updated=%u catalog=%u",
    PET_P4_BUILD_ID,
    (unsigned int) updated,
    (unsigned int) g_catalog_count
  );
  cJSON_Delete(bundle);
  return ESP_OK;
}

static bool remove_file_if_present(const char *path) {
  if (remove(path) == 0) return true;
  return errno == ENOENT;
}

bool pet_p4_miniapp_remove(const char *widget_id, char *error, size_t error_size) {
  char active_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  char next_active_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  miniapp_catalog_item_t next_catalog[PET_P4_MINIAPP_CATALOG_MAX];
  miniapp_runtime_t *next_runtime = NULL;
  size_t next_count;
  size_t next_selected;
  int catalog_index;
  uint8_t removed_slot = 0;
  if (!safe_widget_id(widget_id)) {
    set_error(error, error_size, "invalid widgetId");
    return false;
  }

  portENTER_CRITICAL(&g_runtime_lock);
  if (g_runtime.active) {
    copy_utf8(active_id, sizeof(active_id), g_runtime.widget_id);
  }
  memcpy(next_catalog, g_catalog, sizeof(next_catalog));
  next_count = g_catalog_count;
  next_selected = g_catalog_selected;
  portEXIT_CRITICAL(&g_runtime_lock);

  catalog_index = catalog_find(next_catalog, next_count, widget_id);
  if (catalog_index < 0) {
    memset(&g_staging, 0, sizeof(g_staging));
    return true;
  }
  removed_slot = next_catalog[catalog_index].slot;
  const bool removing_active = active_id[0] && strcmp(active_id, widget_id) == 0;
  if ((size_t) catalog_index + 1 < next_count) {
    memmove(
      &next_catalog[catalog_index],
      &next_catalog[catalog_index + 1],
      (next_count - (size_t) catalog_index - 1) * sizeof(next_catalog[0])
    );
  }
  next_count -= 1;
  memset(&next_catalog[next_count], 0, sizeof(next_catalog[next_count]));
  if (next_count == 0) {
    next_selected = 0;
  } else if (next_selected > (size_t) catalog_index) {
    next_selected -= 1;
  } else if (next_selected >= next_count) {
    next_selected = next_count - 1;
  }
  if (removing_active) {
    if (next_count > 0) {
      copy_utf8(next_active_id, sizeof(next_active_id), next_catalog[next_selected].widget_id);
      next_runtime = (miniapp_runtime_t *) calloc(1, sizeof(*next_runtime));
      if (!next_runtime
          || !load_catalog_runtime_item(
            &next_catalog[next_selected],
            next_runtime,
            false,
            error,
            error_size
          )) {
        free(next_runtime);
        set_error(error, error_size, "failed to validate the next active component");
        return false;
      }
    }
  } else if (active_id[0] && catalog_find(next_catalog, next_count, active_id) >= 0) {
    copy_utf8(next_active_id, sizeof(next_active_id), active_id);
  } else if (next_count > 0) {
    next_selected = 0;
    copy_utf8(next_active_id, sizeof(next_active_id), next_catalog[0].widget_id);
  }
  if (!persist_catalog_snapshot(next_catalog, next_count, next_active_id)) {
    free(next_runtime);
    set_error(error, error_size, "failed to commit component catalog deletion");
    return false;
  }
  portENTER_CRITICAL(&g_runtime_lock);
  memcpy(g_catalog, next_catalog, sizeof(g_catalog));
  g_catalog_count = next_count;
  g_catalog_selected = next_selected;
  if (removing_active) {
    if (next_runtime) g_runtime = *next_runtime;
    else memset(&g_runtime, 0, sizeof(g_runtime));
  }
  memset(&g_staging, 0, sizeof(g_staging));
  portEXIT_CRITICAL(&g_runtime_lock);
  free(next_runtime);

  bool package_cleanup_ok = remove_file_if_present(MINIAPP_ID_PATH);
  package_cleanup_ok = remove_file_if_present(MINIAPP_ID_TMP_PATH) && package_cleanup_ok;
  for (uint8_t package_generation = 0;
       package_generation < MINIAPP_PACKAGE_GENERATION_COUNT;
       package_generation += 1) {
    char widget_path[40];
    char widget_tmp_path[40];
    char buttons_path[40];
    char buttons_tmp_path[40];
    catalog_slot_path(
      removed_slot,
      package_generation,
      false,
      false,
      widget_path,
      sizeof(widget_path)
    );
    catalog_slot_path(
      removed_slot,
      package_generation,
      false,
      true,
      widget_tmp_path,
      sizeof(widget_tmp_path)
    );
    catalog_slot_path(
      removed_slot,
      package_generation,
      true,
      false,
      buttons_path,
      sizeof(buttons_path)
    );
    catalog_slot_path(
      removed_slot,
      package_generation,
      true,
      true,
      buttons_tmp_path,
      sizeof(buttons_tmp_path)
    );
    /* Keep both committed generations: the previous catalog snapshot may need them. */
    (void) widget_path;
    (void) buttons_path;
    package_cleanup_ok = remove_file_if_present(widget_tmp_path) && package_cleanup_ok;
    package_cleanup_ok = remove_file_if_present(buttons_tmp_path) && package_cleanup_ok;
    for (uint8_t sprite_index = 0;
         sprite_index < PET_P4_MINIAPP_SPRITE_MAX;
         sprite_index += 1) {
      char sprite_tmp_path[48];
      catalog_sprite_path(
        removed_slot,
        package_generation,
        sprite_index,
        true,
        sprite_tmp_path,
        sizeof(sprite_tmp_path)
      );
      package_cleanup_ok = remove_file_if_present(sprite_tmp_path) && package_cleanup_ok;
    }
  }
  package_cleanup_ok = remove_file_if_present(MINIAPP_WIDGET_PATH) && package_cleanup_ok;
  package_cleanup_ok = remove_file_if_present(MINIAPP_WIDGET_TMP_PATH) && package_cleanup_ok;
  package_cleanup_ok = remove_file_if_present(MINIAPP_BUTTONS_PATH) && package_cleanup_ok;
  package_cleanup_ok = remove_file_if_present(MINIAPP_BUTTONS_TMP_PATH) && package_cleanup_ok;
  if (!package_cleanup_ok) {
    ESP_LOGW(TAG, "component id=%s removed; staging cleanup was incomplete", widget_id);
  }
  ESP_LOGI(
    TAG,
    "removed bounded component id=%s remaining=%u catalog-generation=%d sequence=%u",
    widget_id,
    (unsigned int) next_count,
    g_catalog_file_generation,
    (unsigned int) g_catalog_sequence
  );
  return true;
}
