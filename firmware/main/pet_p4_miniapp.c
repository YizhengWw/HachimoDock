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
#include "pet_p4_local_tools.h"
#include "esp_random.h"
#include "pet_p4_media.h"
#include "pet_p4_image.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
#include "pet_p4_touch.h"
#include "pet_p4_audio.h"
#include "pet_p4_app_store.h"
#include "pet_p4_app_host.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#define MINIAPP_MAX_VARS 8
#define MINIAPP_MAX_STATES 6
#define MINIAPP_MAX_PAGES 4
#define MINIAPP_MAX_TRANSITIONS 24
#define MINIAPP_MAX_TICKS 8
#define MINIAPP_MAX_EFFECTS 4
#define MINIAPP_MAX_BUTTONS 8
#define MINIAPP_MAX_CHOICES 6
#define MINIAPP_VAR_NAME_MAX 32
#define MINIAPP_VALUE_MAX 160
#define MINIAPP_WIDGET_JSON_MAX 8192
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
#define MINIAPP_LAST_ID_PATH "/spiffs/p4-miniapp-last.txt"
#define MINIAPP_LAST_ID_TMP_PATH "/spiffs/p4-miniapp-last.tmp"
#define MINIAPP_CATALOG_PATH "/spiffs/p4-miniapps.json"
#define MINIAPP_BUILTIN_MARKER_PATH "/spiffs/p4-builtins.txt"
#define MINIAPP_BUILTIN_MARKER_TMP_PATH "/spiffs/p4-builtins.tmp"
#define MINIAPP_CATALOG_GENERATION_COUNT 2
#define MINIAPP_PACKAGE_GENERATION_COUNT 2
#define MINIAPP_CATALOG_VERSION 3
#define MINIAPP_CATALOG_JSON_MAX 16384

#ifndef PET_P4_BUILD_ID
#define PET_P4_BUILD_ID "unknown-build"
#endif

#include "miniz.h"
extern const unsigned char pet_p4_builtin_components_zlib[];
extern const size_t pet_p4_builtin_components_zlib_size;
extern const size_t pet_p4_builtin_components_json_size;
extern bool pet_p4_builtin_is_bootstrap(const char *id, uint32_t checksum);

typedef enum {
  MINIAPP_VAR_INT,
  MINIAPP_VAR_STRING,
} miniapp_var_type_t;

typedef struct {
  char name[MINIAPP_VAR_NAME_MAX];
  miniapp_var_type_t type;
  int32_t int_value;
  int32_t int_min,int_max;
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
  int8_t source_var;
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
  int8_t charge_var;
  int8_t playing_state;
  int8_t result_state;
  char start_action[PET_P4_MINIAPP_ACTION_MAX];
  char left_action[PET_P4_MINIAPP_ACTION_MAX];
  char right_action[PET_P4_MINIAPP_ACTION_MAX];
  char rotate_action[PET_P4_MINIAPP_ACTION_MAX];
  char drop_action[PET_P4_MINIAPP_ACTION_MAX];
  char flap_action[PET_P4_MINIAPP_ACTION_MAX];
  char soft_drop_action[PET_P4_MINIAPP_ACTION_MAX];
  char up_action[PET_P4_MINIAPP_ACTION_MAX],down_action[PET_P4_MINIAPP_ACTION_MAX];
  bool absolute_directions;
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
  bool resource_incomplete;
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
  bool input_v1;
  uint8_t repeat_mask,press_mask;
  char hold_action[48];
  char drag_entity[16];
  uint8_t drag_axis;
  char swipe_actions[4][PET_P4_MINIAPP_ACTION_MAX];
  char confirm_actions[3][PET_P4_MINIAPP_ACTION_MAX],confirm_prompts[3][121];
  char cooldown_actions[3][PET_P4_MINIAPP_ACTION_MAX];
  uint16_t cooldown_ms[3];
  uint64_t cooldown_at[3];
  uint8_t cooldown_used;
  miniapp_game_t game;
  char pause_action[PET_P4_MINIAPP_ACTION_MAX];
  int8_t record_var;
  pet_p4_miniapp_sprite_pack_t sprites;
  miniapp_rule_t rules[MINIAPP_SLOT_COUNT];
  pet_p4_miniapp_view_t view;
  char data_source[48];
  int8_t data_page_var;
  bool media;
  char media_source[48];
  pet_surface_t surface;
  uint16_t save_version;
  uint8_t save_vars;
  bool save_page,app_expressions,app_host;
  int8_t save_states[MINIAPP_MAX_STATES];
  bool instrument;
  pet_local_config_t local;
  pet_local_state_t local_state;
  int8_t local_vars[7];
  bool slicing;
  pet_fruit_t fruit;
  uint8_t effect_volume, ambience_volume;
} miniapp_runtime_t;

typedef struct {
  bool active;
  char transfer_id[64];
  bool repair_resources;
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
  /* Recomputed from committed files, never trusted from catalog JSON. */
  uint8_t resource_state; /* 0 unchecked, 1 ready, 2 bootstrap, 3 damaged */
  uint8_t image_count;
} miniapp_catalog_item_t;

// Catalog transactions must not grow the USB task stack with the slot limit.
// GCC/Clang cleanup releases scratch storage on every error and success return.
static void catalog_scratch_free(miniapp_catalog_item_t **p) { free(*p); }

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
static bool g_catalog_recovery_blocked;
static char g_storage_failure[128];
static EXT_RAM_BSS_ATTR miniapp_runtime_t g_runtime;
static uint32_t g_image_revision=1;
static int g_pressed_node=-1;
static uint64_t g_pressed_context;
static char g_confirm_action[48],g_confirm_prompt[121];
static char g_confirm_host_token[65];
static uint64_t g_confirm_until,g_confirm_context;
static uint8_t g_input_epoch;
static bool g_wall_valid;
static int64_t g_wall_seconds;
static uint64_t g_wall_at;
static portMUX_TYPE g_clock_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t local_wall_seconds(uint64_t ms){
  portENTER_CRITICAL(&g_clock_lock);
  int64_t value=g_wall_valid?g_wall_seconds+(int64_t)((ms>=g_wall_at?ms-g_wall_at:0)/1000):-1;
  portEXIT_CRITICAL(&g_clock_lock);return value;
}
bool pet_p4_miniapp_clock_sync(const cJSON *payload,uint64_t ms){
  const cJSON *time=cJSON_GetObjectItemCaseSensitive(payload,"unixSeconds"),*offset=cJSON_GetObjectItemCaseSensitive(payload,"utcOffsetMinutes");
  if(!cJSON_IsNumber(time)||time->valuedouble<1577836800.0||time->valuedouble>4102444799.0||floor(time->valuedouble)!=time->valuedouble||!cJSON_IsNumber(offset)||offset->valuedouble!=offset->valueint||offset->valueint< -840||offset->valueint>840)return false;
  portENTER_CRITICAL(&g_clock_lock);g_wall_seconds=(int64_t)time->valuedouble+offset->valueint*60;g_wall_at=ms;g_wall_valid=true;portEXIT_CRITICAL(&g_clock_lock);return true;
}
static bool g_input_visible;
static EXT_RAM_BSS_ATTR miniapp_staging_t g_staging;
static EXT_RAM_BSS_ATTR uint8_t g_image_staging[PET_IMAGE_MAX_BYTES+1];
static size_t g_image_used;
static uint32_t g_image_next;
static char g_image_id[16];
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
      var->int_min=MINIAPP_INT_MIN;var->int_max=MINIAPP_INT_MAX;
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
    int source_var=-1;
    if(runtime->app_expressions&&op==MINIAPP_EFFECT_SET&&cJSON_IsObject(item)&&cJSON_GetArraySize(item)==1){
      const char *source=json_string(item,"var");source_var=find_var(runtime,source);
      if(source_var<0||var->type!=MINIAPP_VAR_INT||runtime->vars[source_var].type!=MINIAPP_VAR_INT){set_error(error,error_size,"set var reference must use integers");return false;}
    }
    if (op == MINIAPP_EFFECT_INC && (var->type != MINIAPP_VAR_INT || !cJSON_IsNumber(item))) {
      set_error(error, error_size, "inc requires an int var and numeric value");
      return false;
    }
    if (op == MINIAPP_EFFECT_SET
        && !((var->type == MINIAPP_VAR_INT && (cJSON_IsNumber(item)||source_var>=0))
             || (var->type == MINIAPP_VAR_STRING && cJSON_IsString(item)))) {
      set_error(error, error_size, "set value does not match var type");
      return false;
    }
    if(runtime->app_expressions&&cJSON_IsNumber(item)&&(!isfinite(item->valuedouble)||floor(item->valuedouble)!=item->valuedouble||item->valuedouble < -1000000000.0||item->valuedouble>1000000000.0)){
      set_error(error,error_size,"app effects require bounded integers");return false;
    }
    effect = &effects[(*effect_count)++];
    effect->var_index = (uint8_t) var_index;
    effect->op = op;
    effect->source_var=source_var;
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
      || strcmp(literal, "mole-center") == 0 || strcmp(literal, "mole-right") == 0 || strcmp(literal,"music")==0;
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

static bool runtime_action_enabled(const miniapp_runtime_t *r,const char *action) {
  if(!r->input_v1)return true; /* Legacy packages keep their original contract. */
  if(r->local.enabled&&!pet_local_enabled(&r->local,&r->local_state,action))return false;
  uint64_t now=esp_timer_get_time()/1000ULL;
  for(int i=0;i<3;i++)if(r->cooldown_ms[i]&&(r->cooldown_used&(1u<<i))&&!strcmp(r->cooldown_actions[i],action)
      &&(now<r->cooldown_at[i]||now-r->cooldown_at[i]<r->cooldown_ms[i]))return false;
  for(int i=0;i<r->transition_count;i++){
    const miniapp_transition_t *t=&r->transitions[i];
    if(!strcmp(t->action,action)&&(t->from_state<0||t->from_state==r->current_state)){
      if(r->data_source[0]&&t->to_state<0&&t->page_op<0&&t->effect_count==1&&t->effects[0].var_index==r->data_page_var){
        pet_p4_data_view_t data;pet_p4_widget_data_view(r->data_source,r->vars[r->data_page_var].int_value,esp_timer_get_time()/1000ULL,&data);
        const miniapp_effect_t *e=&t->effects[0];int64_t value=e->source_var>=0?r->vars[e->source_var].int_value:e->int_value;
        if(e->op==MINIAPP_EFFECT_INC)value+=r->vars[r->data_page_var].int_value;
        int pages=data.pages?data.pages:1;
        return ((value%pages)+pages)%pages!=data.page;
      }
      return true;
    }
  }
  return false;
}

static bool parse_input_policy(miniapp_runtime_t *r,const cJSON *root) {
  const cJSON *p=cJSON_GetObjectItemCaseSensitive(root,"input");
  if(!p)return true;
static const char *const keys[]={"engine","repeat","press","swipe","confirm","drag","cooldown","hold"};
  static const char *const dirs[]={"left","right","up","down"};
  if(!cJSON_IsObject(p)||!object_keys_allowed(p,keys,8)||strcmp(json_string(p,"engine"),"p4-input-v1"))return false;
  r->input_v1=true;
  const cJSON *hold=cJSON_GetObjectItemCaseSensitive(p,"hold");
  if(hold){
    const cJSON *scene=cJSON_GetObjectItemCaseSensitive(root,"scene");
    if(!cJSON_IsString(hold)||!runtime_has_transition_action(r,hold->valuestring)||!cJSON_GetObjectItemCaseSensitive(scene,"motion"))return false;
    bool bound=false;for(int b=0;b<r->button_count;b++)if(!strcmp(r->buttons[b].action,hold->valuestring)&&!strcmp(r->buttons[b].event,"button.sw1.short_press"))bound=true;
    if(!bound||cJSON_HasObjectItem(p,"drag"))return false;
    const char *conflicts[]={"repeat","press","confirm","cooldown"};
    for(int k=0;k<4;k++){const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,conflicts[k]),*item;
      if(k<2){cJSON_ArrayForEach(item,v)if(cJSON_IsString(item)&&!strcmp(item->valuestring,hold->valuestring))return false;}
      else if(cJSON_HasObjectItem(v,hold->valuestring))return false;
    }
    copy_utf8(r->hold_action,48,hold->valuestring);
  }
  for(int mode=0;mode<2;mode++){
    const cJSON *a=cJSON_GetObjectItemCaseSensitive(p,mode?"press":"repeat");
    if(!a)continue;
    if(!cJSON_IsArray(a)||cJSON_GetArraySize(a)>8)return false;
    uint8_t mask=0;const cJSON *v;
    cJSON_ArrayForEach(v,a){
      if(!cJSON_IsString(v)||!runtime_has_transition_action(r,v->valuestring))return false;
      int b=-1;
      for(int i=0;i<r->button_count;i++)if(!strcmp(r->buttons[i].action,v->valuestring)){b=i;break;}
      if(b<0||(mask&(1u<<b)))return false;
      const char *event=r->buttons[b].event;
      if(mode){
        const cJSON *game=cJSON_GetObjectItemCaseSensitive(root,"game");
        const char *flap=json_string(cJSON_GetObjectItemCaseSensitive(game,"actions"),"flap");
        if(strcmp(event,"button.sw1.short_press")||
           (strcmp(v->valuestring,flap)&&!(cJSON_GetObjectItemCaseSensitive(root,"instrument")&&!strcmp(v->valuestring,"instrument.strike"))))return false;
      }else if(strcmp(event,"knob.rotate_ccw")&&strcmp(event,"knob.rotate_cw")&&strcmp(event,"joystick.up")&&strcmp(event,"joystick.down"))return false;
      mask|=1u<<b;
    }
    if(mode)r->press_mask=mask;else r->repeat_mask=mask;
  }
  const cJSON *swipe=cJSON_GetObjectItemCaseSensitive(p,"swipe");
  if(swipe){
    if(!cJSON_IsObject(swipe)||!object_keys_allowed(swipe,dirs,4))return false;
    for(int i=0;i<4;i++){
      const cJSON *v=cJSON_GetObjectItemCaseSensitive(swipe,dirs[i]);
      if(!v)continue;
      if(!cJSON_IsString(v)||!runtime_has_transition_action(r,v->valuestring))return false;
      copy_utf8(r->swipe_actions[i],sizeof(r->swipe_actions[i]),v->valuestring);
    }
  }
  const cJSON *confirm=cJSON_GetObjectItemCaseSensitive(p,"confirm");
  if(confirm){
    if(!cJSON_IsObject(confirm)||cJSON_GetArraySize(confirm)>3)return false;
    int i=0;const cJSON *v;
    cJSON_ArrayForEach(v,confirm){
      if(!cJSON_IsString(v)||!v->string||!runtime_has_transition_action(r,v->string)||!v->valuestring[0]||strlen(v->valuestring)>120)return false;
      copy_utf8(r->confirm_actions[i],48,v->string);copy_utf8(r->confirm_prompts[i],121,v->valuestring);i++;
    }
  }
  const cJSON *drag=cJSON_GetObjectItemCaseSensitive(p,"drag");
  if(drag){
    static const char *const dk[]={"entity","axis"};
    const char *id=json_string(drag,"entity"),*axis=json_string(drag,"axis");
    const cJSON *scene=cJSON_GetObjectItemCaseSensitive(root,"scene"),*surface=cJSON_GetObjectItemCaseSensitive(root,"surface");
    if(!object_keys_allowed(drag,dk,2)||!id[0]||strlen(id)>15||strcmp(json_string(cJSON_GetObjectItemCaseSensitive(surface,"scene_view"),"engine"),"p4-scene-surface-v1"))return false;
    r->drag_axis=!strcmp(axis,"x")?1:!strcmp(axis,"y")?2:!strcmp(axis,"xy")?3:0;
    if(!r->drag_axis)return false;
    bool found=false;const cJSON *e;cJSON_ArrayForEach(e,cJSON_GetObjectItemCaseSensitive(scene,"entities")){
      const char *bounds=json_string(e,"bounds");
      if(!strcmp(json_string(e,"id"),id)&&(!bounds[0]||!strcmp(bounds,"clamp")))found=true;
    }
    if(!found)return false;copy_utf8(r->drag_entity,16,id);
  }
  const cJSON *cooldown=cJSON_GetObjectItemCaseSensitive(p,"cooldown");
  if(cooldown){
    if(!cJSON_IsObject(cooldown)||cJSON_GetArraySize(cooldown)>3)return false;
    if(cJSON_GetArraySize(cooldown)&&(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"host"))||cJSON_GetObjectItemCaseSensitive(root,"media")||cJSON_GetObjectItemCaseSensitive(root,"instrument")))return false;
    int i=0;const cJSON *v;cJSON_ArrayForEach(v,cooldown){
      if(!v->string||!runtime_has_transition_action(r,v->string)||!cJSON_IsNumber(v)||v->valuedouble!=v->valueint||v->valueint<250||v->valueint>60000)return false;
      for(int j=0;j<i;j++)if(!strcmp(r->cooldown_actions[j],v->string))return false;
      copy_utf8(r->cooldown_actions[i],48,v->string);r->cooldown_ms[i++]=v->valueint;
    }
  }
  return true;
}

static bool json_int_range(const cJSON *value, int minimum, int maximum, int *out);
static bool parse_game(
  miniapp_runtime_t *runtime,
  const cJSON *game,
  char *error,
  size_t error_size
) {
  static const char *const allowed[] = {
    "type", "tick_ms", "playing_state", "result_state", "score_var", "actions", "vertical_speed_percent", "direction_mode",
  };
  static const char *const blocks_action_allowed[] = {
    "start", "left", "right", "rotate", "drop", "soft_drop",
  };
  static const char *const snake_action_allowed[] = {
    "start", "left", "right", "up", "down",
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
  copy_utf8(runtime->game.soft_drop_action,sizeof(runtime->game.soft_drop_action),json_string(actions,"soft_drop"));
  copy_utf8(runtime->game.up_action,sizeof(runtime->game.up_action),json_string(actions,"up"));
  copy_utf8(runtime->game.down_action,sizeof(runtime->game.down_action),json_string(actions,"down"));
  const cJSON *mode=cJSON_GetObjectItemCaseSensitive(game,"direction_mode");
  if(mode){
    if(!runtime->input_v1||kind!=PET_P4_GAME_SNAKE||!cJSON_IsString(mode)||strcmp(mode->valuestring,"absolute"))return false;
    runtime->game.absolute_directions=true;
  }
  if((runtime->game.up_action[0]||runtime->game.down_action[0])&&!runtime->game.absolute_directions)return false;
  if(runtime->game.absolute_directions&&(!runtime->game.up_action[0]||!runtime->game.down_action[0]))return false;
  if(cJSON_GetObjectItemCaseSensitive(actions,"soft_drop")&&(!runtime->input_v1||!runtime->game.soft_drop_action[0]))return false;
  const char *extras[]={runtime->game.soft_drop_action,runtime->game.up_action,runtime->game.down_action};
  for(int i=0;i<3;i++)if(extras[i][0]&&(!safe_id(extras[i],PET_P4_MINIAPP_ACTION_MAX)||!runtime_has_transition_action(runtime,extras[i])||!runtime_has_button_action(runtime,extras[i])))return false;
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
  const cJSON *speed=cJSON_GetObjectItemCaseSensitive(game,"vertical_speed_percent");
  int percent=100;
  if(speed&&(!json_int_range(speed,25,100,&percent)
      ||!pet_p4_game_set_vertical_speed(&runtime->game.engine,(unsigned)percent))){
    set_error(error,error_size,"vertical_speed_percent requires flappy and an integer in 25-100");
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

static void local_values(miniapp_runtime_t *r,int32_t v[7],bool write){
  for(int i=0;i<7;i++){if(write)r->vars[r->local_vars[i]].int_value=v[i];else v[i]=r->vars[r->local_vars[i]].int_value;}
}
static void refresh_view(miniapp_runtime_t *runtime) {
  if (runtime->resource_incomplete) return;
  pet_p4_miniapp_view_t *view = &runtime->view;
  uint32_t next_revision = view->revision + 1;
  memset(view, 0, sizeof(*view));
  view->active = runtime->active;
  view->progress_percent = -1;
  view->revision = next_revision;
  copy_utf8(view->data_source, sizeof(view->data_source), runtime->data_source);
  view->media=runtime->media;
  view->app_surface=runtime->surface.app;
  view->app_host=runtime->app_host;
  view->surface_page=runtime->current_page<0?0:runtime->current_page;
  view->instrument=runtime->instrument;
  view->slicing=runtime->slicing;
  view->effect_volume=runtime->effect_volume;
  view->ambience_volume=runtime->ambience_volume;
  if (runtime->data_source[0]) view->data_page = runtime->vars[runtime->data_page_var].int_value;
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

static void refresh_local_view(miniapp_runtime_t *r,uint64_t now){
  int32_t v[7];local_values(r,v,false);pet_local_view_t o;
  pet_local_view(&r->local,&r->local_state,v,now,local_wall_seconds(now),&o);
  r->current_state=o.phase;refresh_view(r);
  copy_utf8(r->view.eyebrow,sizeof(r->view.eyebrow),o.eyebrow);
  copy_utf8(r->view.headline,sizeof(r->view.headline),o.headline);
  copy_utf8(r->view.note,sizeof(r->view.note),o.note);
  copy_utf8(r->view.metric_value,sizeof(r->view.metric_value),o.metric);
  copy_utf8(r->view.metric_label,sizeof(r->view.metric_label),o.label);
  copy_utf8(r->view.metric_unit,sizeof(r->view.metric_unit),o.unit);
  copy_utf8(r->view.badge,sizeof(r->view.badge),o.badge);
  r->view.progress_percent=o.progress;
}
static bool parse_local(miniapp_runtime_t *r,const cJSON *root){
  const cJSON *local=cJSON_GetObjectItemCaseSensitive(root,"local");if(!local)return true;
  if(!r->surface.app||!r->input_v1||r->var_count!=7||r->tick_count||r->page_count||r->save_vars!=127||r->save_version!=1||r->state_count!=6||r->transition_count<2||r->game.active||cJSON_HasObjectItem(root,"host")||cJSON_HasObjectItem(root,"scene")||cJSON_HasObjectItem(root,"media")||cJSON_HasObjectItem(root,"data")||cJSON_HasObjectItem(root,"instrument"))return false;
  const char *states[]={"ready","busy","result","editing","empty","offline"};
  for(int i=0;i<6;i++)if(strcmp(r->states[i],states[i])||r->save_states[i]!=0)return false;
  for(int i=0;i<7;i++){char name[8];snprintf(name,sizeof(name),"local%d",i);int index=find_var(r,name);if(index<0||r->vars[index].type!=MINIAPP_VAR_INT)return false;r->local_vars[i]=index;}
  if(!pet_local_parse(local,&r->local)||r->local.action_count!=r->transition_count)return false;
  for(int i=0;i<r->transition_count;i++){
    miniapp_transition_t *t=&r->transitions[i];bool found=false;
    for(int j=0;j<r->local.action_count;j++)if(!strcmp(t->action,r->local.actions[j]))found=true;
    if(!found||t->from_state!=-1||t->to_state!=-1||t->page_op!=-1||t->effect_count)return false;
  }
  if(cJSON_HasObjectItem(root,"game"))return false;
  pet_local_init(&r->local_state);return true;
}
static bool parse_save(miniapp_runtime_t *r,const cJSON *save){
  if(!save)return true;
  static const char *const keys[]={"version","vars","states","page"};
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(save,"version"),*vars=cJSON_GetObjectItemCaseSensitive(save,"vars"),*states=cJSON_GetObjectItemCaseSensitive(save,"states"),*item;
  if(!r->surface.app||!object_keys_allowed(save,keys,4)||!cJSON_IsNumber(v)||v->valuedouble!=v->valueint||v->valueint<1||v->valueint>65535||!cJSON_IsArray(vars)||cJSON_GetArraySize(vars)<1||cJSON_GetArraySize(vars)>8||!cJSON_IsObject(states)||cJSON_GetArraySize(states)!=r->state_count)return false;
  const cJSON *page=cJSON_GetObjectItemCaseSensitive(save,"page");if(page&&!cJSON_IsBool(page))return false;r->save_page=cJSON_IsTrue(page);if(r->save_page&&!r->page_count)return false;
  r->save_version=v->valueint;
  cJSON_ArrayForEach(item,vars){
    if(!cJSON_IsString(item))return false;int i=find_var(r,item->valuestring);
    if(i<0||r->vars[i].type!=MINIAPP_VAR_INT||(r->save_vars&(1u<<i)))return false;r->save_vars|=1u<<i;
  }
  for(int i=0;i<r->state_count;i++){
    const cJSON *s=cJSON_GetObjectItemCaseSensitive(states,r->states[i]);
    if(!cJSON_IsString(s))return false;r->save_states[i]=find_name(r->states,r->state_count,s->valuestring);
    if(r->save_states[i]<0)return false;
  }
  return true;
}

static void save_app_runtime(void){
  miniapp_var_t vars[MINIAPP_MAX_VARS];char id[48],state[24],page[24]={0};uint16_t version;uint8_t mask,count;
  bool fruit;unsigned records[3],mode;
  portENTER_CRITICAL(&g_runtime_lock);
  fruit=g_runtime.active&&g_runtime.slicing;
  if(fruit){memcpy(records,g_runtime.fruit.records,sizeof(records));mode=g_runtime.fruit.mode;copy_utf8(id,sizeof(id),g_runtime.widget_id);}
  version=g_runtime.save_version;mask=g_runtime.save_vars;count=g_runtime.var_count;
  if(version){memcpy(vars,g_runtime.vars,sizeof(vars));copy_utf8(id,sizeof(id),g_runtime.widget_id);copy_utf8(state,sizeof(state),g_runtime.states[g_runtime.current_state]);}
  if(version&&g_runtime.save_page)copy_utf8(page,sizeof(page),g_runtime.pages[g_runtime.current_page]);
  portEXIT_CRITICAL(&g_runtime_lock);
  if(fruit){
    cJSON *root=cJSON_CreateObject(),*list=root?cJSON_AddArrayToObject(root,"fruitRecords"):NULL;
    if(list){cJSON_AddNumberToObject(root,"fruitVersion",1);cJSON_AddNumberToObject(root,"mode",mode);
      for(int i=0;i<3;i++)cJSON_AddItemToArray(list,cJSON_CreateNumber(records[i]));
      char *json=cJSON_PrintUnformatted(root);if(json){pet_app_store_update(id,json);cJSON_free(json);}
    }cJSON_Delete(root);return;
  }
  if(!version)return;
  cJSON *root=cJSON_CreateObject();if(!root)return;
  cJSON_AddNumberToObject(root,"version",version);cJSON_AddStringToObject(root,"state",state);
  if(page[0])cJSON_AddStringToObject(root,"page",page);
  cJSON *values=cJSON_AddObjectToObject(root,"vars");
  if(values)for(int i=0;i<count;i++)if(mask&(1u<<i)){
    if(vars[i].type==MINIAPP_VAR_INT)cJSON_AddNumberToObject(values,vars[i].name,vars[i].int_value);
    else cJSON_AddStringToObject(values,vars[i].name,vars[i].string_value);
  }
  char *json=values?cJSON_PrintUnformatted(root):NULL;
  if(json){pet_app_store_update(id,json);cJSON_free(json);}cJSON_Delete(root);
}

static void restore_app_runtime(miniapp_runtime_t *r){
  if(!r||(!r->save_version&&!r->slicing))return;
  char json[PET_APP_SAVE_BYTES];if(!pet_app_store_read(r->widget_id,json))return;
  cJSON *root=cJSON_Parse(json);if(!root)return;
  if(r->slicing){
    const cJSON *list=cJSON_GetObjectItemCaseSensitive(root,"fruitRecords"),*mode=cJSON_GetObjectItemCaseSensitive(root,"mode"),*version=cJSON_GetObjectItemCaseSensitive(root,"fruitVersion");
    bool valid=cJSON_IsNumber(version)&&version->valuedouble==1&&cJSON_IsArray(list)&&cJSON_GetArraySize(list)==3&&cJSON_IsNumber(mode)&&mode->valuedouble>=0&&mode->valuedouble<=2&&mode->valuedouble==mode->valueint;
    for(int i=0;valid&&i<3;i++){const cJSON *v=cJSON_GetArrayItem(list,i);valid=cJSON_IsNumber(v)&&v->valuedouble>=0&&v->valuedouble<=1000000000&&v->valuedouble==v->valueint;}
    if(valid){for(int i=0;i<3;i++)r->fruit.records[i]=cJSON_GetArrayItem(list,i)->valueint;pet_fruit_select_mode(&r->fruit,mode->valueint);}
    cJSON_Delete(root);return;
  }
  const cJSON *version=cJSON_GetObjectItemCaseSensitive(root,"version"),*values=cJSON_GetObjectItemCaseSensitive(root,"vars");
  int state=find_name(r->states,r->state_count,json_string(root,"state"));
  bool valid=cJSON_IsNumber(version)&&version->valuedouble==r->save_version&&state>=0&&cJSON_IsObject(values);
  for(int i=0;valid&&i<r->var_count;i++)if(r->save_vars&(1u<<i)){
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(values,r->vars[i].name);
    /* Additive fields in the same save version keep their declared defaults.
     * Reject malformed existing values atomically, but preserve earlier progress. */
    if(!v)continue;
    valid=r->vars[i].type==MINIAPP_VAR_INT?(cJSON_IsNumber(v)&&v->valuedouble>=r->vars[i].int_min&&v->valuedouble<=r->vars[i].int_max&&v->valuedouble==v->valueint):(cJSON_IsString(v)&&strlen(v->valuestring)<64);
  }
  if(valid){
    for(int i=0;i<r->var_count;i++)if(r->save_vars&(1u<<i)){
      const cJSON *v=cJSON_GetObjectItemCaseSensitive(values,r->vars[i].name);
      if(!v)continue;
      if(r->vars[i].type==MINIAPP_VAR_INT)r->vars[i].int_value=v->valueint;else copy_utf8(r->vars[i].string_value,64,v->valuestring);
    }
    r->current_state=r->save_states[state];refresh_view(r);
    if(r->save_page){int page=find_name(r->pages,r->page_count,json_string(root,"page"));if(page>=0)r->current_page=page;refresh_view(r);}
  }
  cJSON_Delete(root);
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
    "game", "data", "media", "instrument", "surface", "persist", "limits", "host", "lifecycle", "input", "local",
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
  parsed->app_expressions=runtime_v4&&!strcmp(json_string(cJSON_GetObjectItemCaseSensitive(root,"surface"),"engine"),"p4-app-surface-v1");
  if ((engine[0] && strcmp(engine, "p4-bounded-runtime-v3") != 0 && !runtime_v4)
      || (scene && !engine[0]) || (scene && legacy_game)) {
    set_error(error, error_size, "engine/scene declaration is unsupported or conflicts with legacy game");
    goto fail;
  }
  if (!parse_vars(parsed, cJSON_GetObjectItemCaseSensitive(root, "vars"), error, error_size)) goto fail;
  const cJSON *limits=cJSON_GetObjectItemCaseSensitive(root,"limits"),*limit;
  if(limits){
    if(!parsed->app_expressions||!cJSON_IsObject(limits)){set_error(error,error_size,"limits require application surface");goto fail;}
    cJSON_ArrayForEach(limit,limits){
      int i=find_var(parsed,limit->string);const cJSON *lo=cJSON_GetArrayItem(limit,0),*hi=cJSON_GetArrayItem(limit,1);
      if(i<0||parsed->vars[i].type!=MINIAPP_VAR_INT||!cJSON_IsArray(limit)||cJSON_GetArraySize(limit)!=2||!cJSON_IsNumber(lo)||!cJSON_IsNumber(hi)||lo->valuedouble<MINIAPP_INT_MIN||hi->valuedouble>MINIAPP_INT_MAX||lo->valuedouble>hi->valuedouble||lo->valuedouble!=lo->valueint||hi->valuedouble!=hi->valueint||parsed->vars[i].int_value<lo->valueint||parsed->vars[i].int_value>hi->valueint){set_error(error,error_size,"invalid application variable limits");goto fail;}
      parsed->vars[i].int_min=lo->valueint;parsed->vars[i].int_max=hi->valueint;
    }
  }
  const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
  const cJSON *media=cJSON_GetObjectItemCaseSensitive(root,"media");
  const cJSON *instrument=cJSON_GetObjectItemCaseSensitive(root,"instrument");
  if(instrument) {
    static const char *const keys[]={"source","effect_volume","ambience_volume"};
    const cJSON *effect=cJSON_GetObjectItemCaseSensitive(instrument,"effect_volume");
    const cJSON *ambience=cJSON_GetObjectItemCaseSensitive(instrument,"ambience_volume");
    const cJSON *rules=cJSON_GetObjectItemCaseSensitive(root,"transitions");
    const cJSON *rule=cJSON_GetArrayItem(rules,0);
    static const char *const rule_keys[]={"on","from"};
    if(!runtime_v4 || data || media || scene || legacy_game || parsed->var_count
       || cJSON_GetObjectItemCaseSensitive(root,"pages")
       || cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root,"tick"))
       || !cJSON_IsObject(instrument) || !object_keys_allowed(instrument,keys,3)
       || strcmp(json_string(instrument,"source"),"percussion.wooden-fish")
       || !cJSON_IsNumber(effect) || effect->valuedouble!=effect->valueint || effect->valueint<0 || effect->valueint>100
       || !cJSON_IsNumber(ambience) || ambience->valuedouble!=ambience->valueint || ambience->valueint<0 || ambience->valueint>100
       || !cJSON_IsArray(rules) || cJSON_GetArraySize(rules)!=1 || !cJSON_IsObject(rule)
       || !object_keys_allowed(rule,rule_keys,2) || strcmp(json_string(rule,"on"),"instrument.strike")
       || strcmp(json_string(rule,"from"),"*")) {
      set_error(error,error_size,"invalid bounded instrument surface");goto fail;
    }
    parsed->instrument=true;parsed->effect_volume=effect->valueint;parsed->ambience_volume=ambience->valueint;
  }
  if(media) {
    static const char *const media_keys[]={"source","mode"};
    const char *source=json_string(media,"source");
    char expected[48];snprintf(expected,sizeof(expected),"pc.%s",widget_id);
    bool generic=!strcmp(source,expected)&&strlen(widget_id)<=44&&!strcmp(json_string(media,"mode"),"live");
    if(!runtime_v4 || data || scene || legacy_game || parsed->var_count || !cJSON_IsObject(media)
       || cJSON_GetObjectItemCaseSensitive(root,"pages")
       || cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root,"tick"))
       || !object_keys_allowed(media,media_keys,generic?2:1)
       || (!generic && strcmp(source,"audio.player"))) {
      set_error(error,error_size,"invalid media surface");goto fail;
    }
    parsed->media=true;
    snprintf(parsed->media_source,sizeof(parsed->media_source),"%s",source);
    if(generic&&!pet_surface_parse(cJSON_GetObjectItemCaseSensitive(root,"surface"),&parsed->surface)){set_error(error,error_size,"invalid bounded media layout");goto fail;}
    const cJSON *rules=cJSON_GetObjectItemCaseSensitive(root,"transitions");
    static const char *const rule_keys[]={"on","from"};
    static const char *const actions[]={"media.toggle","media.queue","media.previous","media.next","media.up","media.down","media.select","media.lyrics"};
    const cJSON *rule=NULL;
    cJSON_ArrayForEach(rule,rules) {
      bool known=false;
      for(size_t i=0;i<sizeof(actions)/sizeof(actions[0]);i++)if(!strcmp(json_string(rule,"on"),actions[i]))known=true;
      if(!known||(generic&&!strcmp(json_string(rule,"on"),"media.lyrics"))||!object_keys_allowed(rule,rule_keys,2)||strcmp(json_string(rule,"from"),"*")) {
        set_error(error,error_size,"invalid media action");goto fail;
      }
    }
  }
  if (data) {
    static const char *const data_keys[] = {"source", "page_var"};
    const char *source = json_string(data, "source");
    int page_var = find_var(parsed, json_string(data, "page_var"));
    if (!runtime_v4 || scene || legacy_game || !cJSON_IsObject(data)
        || !object_keys_allowed(data, data_keys, 2) || !source[0] || strlen(source) > 47
        || page_var < 0 || parsed->vars[page_var].type != MINIAPP_VAR_INT) {
      set_error(error, error_size, "invalid bounded data source/page_var"); goto fail;
    }
    for (const char *p = source; *p; p++) {
      if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) {
        set_error(error, error_size, "invalid data source id"); goto fail;
      }
    }
    copy_utf8(parsed->data_source, sizeof(parsed->data_source), source);
    parsed->data_page_var = page_var;
  }
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
  if(!parse_input_policy(parsed,root)){set_error(error,error_size,"invalid input policy");goto fail;}
  if(parsed->transition_count>12&&!parsed->input_v1){set_error(error,error_size,"extended transitions require input v1");goto fail;}
  const cJSON *surface=cJSON_GetObjectItemCaseSensitive(root,"surface");
  if(surface&&!media){
    if(!runtime_v4||instrument||!pet_surface_parse(surface,&parsed->surface)||!parsed->surface.app||
      (!!legacy_game!=parsed->surface.game_view)|| (!!data!=parsed->surface.data_view.enabled)||
      (!!scene!=parsed->surface.scene_view.enabled)){
      set_error(error,error_size,"invalid application surface");goto fail;
    }
    if(scene&&cJSON_HasObjectItem(scene,"sprites")){set_error(error,error_size,"scene view sprites belong to surface");goto fail;}
    if(scene){const cJSON *grid=cJSON_GetObjectItemCaseSensitive(scene,"grid");if(cJSON_HasObjectItem(grid,"rows")||cJSON_HasObjectItem(grid,"solid_tones")){set_error(error,error_size,"scene surface v1 requires an open field");goto fail;}}
    if(scene){const cJSON *e;cJSON_ArrayForEach(e,cJSON_GetObjectItemCaseSensitive(scene,"entities"))if(cJSON_HasObjectItem(e,"sprite")){set_error(error,error_size,"scene view uses entity image mapping");goto fail;}}
    for(int i=0;i<parsed->surface.count;i++){
      const pet_surface_node_t *n=&parsed->surface.nodes[i];
      if(n->state_mask&&(!parsed->input_v1||(n->state_mask>>parsed->state_count))){set_error(error,error_size,"state visibility requires input v1 and declared states");goto fail;}
      if(n->host_mask&&(!parsed->input_v1||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"host")))){set_error(error,error_size,"host visibility requires input v1 host");goto fail;}
      const char *refs[]={n->tile_var,n->tile_max_var,n->show_var};
      for(int k=0;k<3;k++)if(refs[k][0]){int v=find_var(parsed,refs[k]);if(v<0||parsed->vars[v].type!=MINIAPP_VAR_INT){set_error(error,error_size,"surface variable must be a declared integer");goto fail;}}
      if((n->action[0]&&!runtime_has_transition_action(parsed,n->action))||(n->key_for[0]&&!runtime_has_button_action(parsed,n->key_for))||(n->page!=4&&n->page>=(parsed->page_count?parsed->page_count:1))){set_error(error,error_size,"unknown surface action/page");goto fail;}
    }
  }
  if(!parse_save(parsed,cJSON_GetObjectItemCaseSensitive(root,"persist"))){set_error(error,error_size,"invalid app save declaration");goto fail;}
  if(!parse_local(parsed,root)){set_error(error,error_size,"invalid offline tools declaration");goto fail;}
  const cJSON *host=cJSON_GetObjectItemCaseSensitive(root,"host");
  if(host){
    if(!cJSON_IsTrue(host)||!parsed->surface.app||parsed->save_version||parsed->tick_count||parsed->var_count){set_error(error,error_size,"invalid host application");goto fail;}
    const cJSON *r; cJSON_ArrayForEach(r,cJSON_GetObjectItemCaseSensitive(root,"transitions")){
      static const char *const keys[]={"from","on"};
      if(!object_keys_allowed(r,keys,2)||strcmp(json_string(r,"from"),"*")||strncmp(json_string(r,"on"),"host.",5)){set_error(error,error_size,"invalid host action");goto fail;}
    }
    parsed->app_host=true;
  }
  if (!parse_game(parsed, legacy_game, error, error_size)) goto fail;
  if (sprites) parsed->sprites = *sprites;
  if(parsed->surface.image_count){
    if(parsed->surface.image_count!=parsed->sprites.count){set_error(error,error_size,"surface images missing");goto fail;}
    for(int i=0;i<parsed->surface.image_count;i++){
      pet_p4_miniapp_sprite_t *im=&parsed->sprites.items[i];
      if(strcmp(im->id,parsed->surface.image_ids[i])||im->frame_width!=parsed->surface.image_w[i]||im->frame_height!=parsed->surface.image_h[i]||im->frames!=1||im->fps!=0){
        set_error(error,error_size,"surface image metadata mismatch");goto fail;
      }
    }
  }
  if(!parsed->surface.image_count && parsed->sprites.data_length>4096*3){set_error(error,error_size,"scene sprites exceed pixel budget");goto fail;}
  if (!parse_bounded_scene(parsed, scene, runtime_v4, error, error_size)) goto fail;
  parsed->record_var=-1;
  const cJSON *lifecycle=cJSON_GetObjectItemCaseSensitive(root,"lifecycle");
  if(lifecycle){
    static const char *const keys[]={"pause_action","record_var"};
    if(!parsed->game.active||parsed->game.result_state<0||!object_keys_allowed(lifecycle,keys,2)){
      set_error(error,error_size,"lifecycle requires a scored round-based game");goto fail;
    }
    const char *pause=json_string(lifecycle,"pause_action");
    bool valid_pause=false;
    for(int i=0;i<parsed->transition_count;i++){
      const miniapp_transition_t *t=&parsed->transitions[i];
      if(!strcmp(t->action,pause)&&t->from_state==parsed->game.playing_state&&t->to_state>=0
        &&t->to_state!=parsed->game.playing_state&&t->to_state!=parsed->game.result_state){
        for(int j=0;j<parsed->transition_count;j++){
          const miniapp_transition_t *back=&parsed->transitions[j];
          if(!strcmp(back->action,pause)&&back->from_state==t->to_state&&back->to_state==parsed->game.playing_state)valid_pause=true;
        }
      }
    }
    if(!pause[0]||!valid_pause||!runtime_has_button_action(parsed,pause)){
      set_error(error,error_size,"lifecycle pause must leave playing through a declared input");goto fail;
    }
    copy_utf8(parsed->pause_action,sizeof(parsed->pause_action),pause);
    if(cJSON_GetObjectItemCaseSensitive(lifecycle,"record_var")){
      parsed->record_var=find_var(parsed,json_string(lifecycle,"record_var"));
      if(parsed->record_var<0||parsed->vars[parsed->record_var].type!=MINIAPP_VAR_INT
        ||parsed->record_var==parsed->game.score_var
        ||(parsed->game.engine.bounded.motion&&parsed->record_var==parsed->game.charge_var)
        ||!(parsed->save_vars&(1u<<parsed->record_var))){
        set_error(error,error_size,"record_var requires an independent saved integer");goto fail;
      }
    }
  }
  if(parsed->surface.scene_view.enabled){
    if(parsed->game.engine.kind!=PET_P4_GAME_BOUNDED){set_error(error,error_size,"scene view requires bounded scene");goto fail;}
    for(int i=0;i<parsed->surface.scene_view.count;i++){
      bool found=false;for(int j=0;j<parsed->game.engine.bounded.entity_count;j++)if(!strcmp(parsed->surface.scene_view.sprites[i].entity,parsed->game.engine.bounded.entities[j].id))found=true;
      if(!found){set_error(error,error_size,"unknown scene view entity");goto fail;}
    }
  }
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
  int32_t original[MINIAPP_MAX_VARS];for(int i=0;i<runtime->var_count;i++)original[i]=runtime->vars[i].int_value;
  for (int i = 0; i < count; i += 1) {
    const miniapp_effect_t *effect = &effects[i];
    miniapp_var_t *var = &runtime->vars[effect->var_index];
    if (var->type == MINIAPP_VAR_INT) {
      int32_t next = effect->op == MINIAPP_EFFECT_INC
        ? clamp_i64((long long) var->int_value + effect->int_value) : effect->int_value;
      if(effect->source_var>=0)next=original[effect->source_var];
      if(next<var->int_min)next=var->int_min;if(next>var->int_max)next=var->int_max;
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
    *command = game->absolute_directions?PET_P4_GAME_COMMAND_WEST:PET_P4_GAME_COMMAND_LEFT;
  } else if (strcmp(action, game->right_action) == 0) {
    *command = game->absolute_directions?PET_P4_GAME_COMMAND_EAST:PET_P4_GAME_COMMAND_RIGHT;
  } else if(game->up_action[0]&&!strcmp(action,game->up_action)){
    *command=PET_P4_GAME_COMMAND_NORTH;
  } else if(game->down_action[0]&&!strcmp(action,game->down_action)){
    *command=PET_P4_GAME_COMMAND_SOUTH;
  } else if(game->soft_drop_action[0]&&!strcmp(action,game->soft_drop_action)){
    *command=PET_P4_GAME_COMMAND_SOFT_DROP;
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
  if(game->engine.bounded.motion&&game->charge_var>=0){
    int charge=game->engine.frame.charge_percent;
    if(runtime->vars[game->charge_var].int_value!=charge){runtime->vars[game->charge_var].int_value=charge;changed=true;}
  }
  if (game->score_var >= 0
      && runtime->vars[game->score_var].int_value != game->engine.score) {
    runtime->vars[game->score_var].int_value = game->engine.score;
    changed = true;
  }
  if(runtime->record_var>=0&&game->engine.score>runtime->vars[runtime->record_var].int_value){
    runtime->vars[runtime->record_var].int_value=game->engine.score;
    changed=true;
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
  bool media=false;
  char confirmed[48],confirmed_context[65]={0};
  portENTER_CRITICAL(&g_runtime_lock);
  uint64_t context=((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;
  if(g_confirm_action[0]&&(now_ms>=g_confirm_until||context!=g_confirm_context))g_confirm_action[0]=0;
  bool approved=false;
  if(g_confirm_action[0]){
    bool yes=!strcmp(action,"input.confirm"),no=!strcmp(action,"input.cancel");
    for(int i=0;i<g_runtime.button_count;i++)if(!strcmp(action,g_runtime.buttons[i].action)){
      yes|=!strcmp(g_runtime.buttons[i].event,"button.sw1.short_press");
      no|=!strcmp(g_runtime.buttons[i].event,"button.sw2.short_press");
    }
    if(no){g_confirm_action[0]=0;g_runtime.view.revision++;portEXIT_CRITICAL(&g_runtime_lock);return true;}
    if(!yes){portEXIT_CRITICAL(&g_runtime_lock);return true;}
    copy_utf8(confirmed,sizeof(confirmed),g_confirm_action);copy_utf8(confirmed_context,sizeof(confirmed_context),g_confirm_host_token);action=confirmed;approved=true;g_confirm_action[0]=0;g_runtime.view.revision++;
  }
  if(g_runtime.active&&g_runtime.input_v1&&!approved&&runtime_action_enabled(&g_runtime,action)){
    if(g_runtime.app_host&&!pet_app_host_enabled(g_runtime.widget_id,action,now_ms)){portEXIT_CRITICAL(&g_runtime_lock);return true;}
    for(int i=0;i<3;i++)if(g_runtime.confirm_actions[i][0]&&!strcmp(action,g_runtime.confirm_actions[i])){
      copy_utf8(g_confirm_action,48,action);copy_utf8(g_confirm_prompt,121,g_runtime.confirm_prompts[i]);
      g_confirm_host_token[0]=0;if(g_runtime.app_host)pet_app_host_context(g_runtime.widget_id,g_confirm_host_token);
      g_confirm_until=now_ms+30000;g_confirm_context=context;g_runtime.view.revision++;
      if(g_runtime.game.engine.bounded.motion){
        pet_p4_game_release(&g_runtime.game.engine,true,now_ms);
        sync_game_runtime(&g_runtime);refresh_view(&g_runtime);
      }
      portEXIT_CRITICAL(&g_runtime_lock);return true;
    }
  }
  if(g_runtime.active&&g_runtime.input_v1&&!runtime_action_enabled(&g_runtime,action)){
    portEXIT_CRITICAL(&g_runtime_lock);return false;
  }
  if(g_runtime.active&&g_runtime.app_host){
    char id[48];copy_utf8(id,sizeof(id),g_runtime.widget_id);uint32_t rev=g_image_revision;uint8_t page=g_runtime.current_page<0?0:g_runtime.current_page;
    bool declared=runtime_has_transition_action(&g_runtime,action);portEXIT_CRITICAL(&g_runtime_lock);
    return declared&&(approved?pet_app_host_confirmed_action(id,rev,page,action,confirmed_context,now_ms):pet_app_host_action(id,rev,page,action,now_ms));
  }
  if(g_runtime.active&&g_runtime.local.enabled){
    int32_t values[7];local_values(&g_runtime,values,false);
    bool ok=pet_local_action(&g_runtime.local,&g_runtime.local_state,values,action,now_ms,local_wall_seconds(now_ms),esp_random());
    if(ok){local_values(&g_runtime,values,true);refresh_local_view(&g_runtime,now_ms);}
    portEXIT_CRITICAL(&g_runtime_lock);return ok;
  }
  if(g_runtime.active&&g_runtime.slicing) {
    if(!pet_p4_touch_ready()){portEXIT_CRITICAL(&g_runtime_lock);return false;}
    if(!strcmp(action,"slice.restart")) {pet_fruit_start_or_resume(&g_runtime.fruit,now_ms,(uint32_t)now_ms);handled=true;}
    else if(!strcmp(action,"slice.pause")) {pet_fruit_pause(&g_runtime.fruit,now_ms);handled=true;}
    portEXIT_CRITICAL(&g_runtime_lock);
    return handled;
  }
  if(g_runtime.active&&g_runtime.instrument) {
    portEXIT_CRITICAL(&g_runtime_lock);
    return !strcmp(action,"instrument.strike") && pet_p4_audio_instrument_strike(now_ms);
  }
  if(g_runtime.active&&g_runtime.media) {
    for(int i=0;i<g_runtime.transition_count;i++)if(!strcmp(g_runtime.transitions[i].action,action))media=true;
    if(!media) {portEXIT_CRITICAL(&g_runtime_lock);return false;}
  }
  portEXIT_CRITICAL(&g_runtime_lock);
  if(media){char source[48];if(pet_p4_miniapp_media_config(source,NULL))return pet_p4_media_live_action(source,action);return pet_p4_media_action(action);}
  portENTER_CRITICAL(&g_runtime_lock);
  if (!g_runtime.active) goto done;
  for (int i = 0; i < g_runtime.transition_count; i += 1) {
    miniapp_transition_t *transition = &g_runtime.transitions[i];
    bool changed = false;
    pet_p4_game_command_t game_command;
    if (strcmp(transition->action, action) != 0) continue;
    if (transition->from_state >= 0 && transition->from_state != g_runtime.current_state) continue;
    bool resuming=false;
    if(g_runtime.game.active&&g_runtime.pause_action[0]&&!strcmp(action,g_runtime.pause_action)
        &&transition->to_state==g_runtime.game.playing_state&&g_runtime.current_state!=g_runtime.game.playing_state)
      for(int t=0;t<g_runtime.transition_count;t++){
        const miniapp_transition_t *p=&g_runtime.transitions[t];
        if(g_runtime.pause_action[0]&&!strcmp(p->action,g_runtime.pause_action)&&p->from_state==g_runtime.current_state&&p->to_state==g_runtime.game.playing_state)resuming=true;
      }
    if (transition->to_state >= 0 && transition->to_state != g_runtime.current_state) {
      pet_p4_game_release(&g_runtime.game.engine,true,now_ms);
      g_runtime.current_state = transition->to_state;
      // Paused time must never be replayed as catch-up motion on resume.
      if(g_runtime.game.active)g_runtime.game.engine.last_tick_ms=now_ms;
      for(int t=0;t<g_runtime.tick_count;t++)g_runtime.ticks[t].last_ms=now_ms;
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
    if (!resuming&&g_runtime.game.active
        && g_runtime.game.engine.kind == PET_P4_GAME_BOUNDED) {
      changed |= pet_p4_game_dispatch_action(
        &g_runtime.game.engine,
        action,
        now_ms
      );
      changed |= sync_game_runtime(&g_runtime);
    } else if (!resuming&&game_command_for_action(&g_runtime.game, action, &game_command)) {
      changed |= pet_p4_game_command(&g_runtime.game.engine, game_command, now_ms);
      changed |= sync_game_runtime(&g_runtime);
    }
    if (changed) refresh_view(&g_runtime);
    handled = true;
    break;
  }
done:
  if(handled)for(int i=0;i<3;i++)if(g_runtime.cooldown_ms[i]&&!strcmp(g_runtime.cooldown_actions[i],action)){
    g_runtime.cooldown_used|=1u<<i;g_runtime.cooldown_at[i]=now_ms;
  }
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

static unsigned g_hold_owner;
static uint64_t g_hold_context;
static uint64_t hold_context(void){return ((uint64_t)g_input_epoch<<56)|((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;}
bool pet_p4_miniapp_hold_input(const char *event_name){
  char action[48];if(!pet_p4_miniapp_resolve_input(event_name,action,sizeof(action)))return false;
  portENTER_CRITICAL(&g_runtime_lock);bool yes=g_runtime.hold_action[0]&&!strcmp(action,g_runtime.hold_action);portEXIT_CRITICAL(&g_runtime_lock);return yes;
}
bool pet_p4_miniapp_hold_event(const char *action,int phase,unsigned owner,uint64_t now_ms){
  if(owner<1||owner>2||phase<0||phase>2)return false;
  portENTER_CRITICAL(&g_runtime_lock);
  if(phase){
    bool accepted=g_hold_owner==owner;
    if(accepted){pet_p4_game_release(&g_runtime.game.engine,phase==2||g_hold_context!=hold_context()||!g_input_visible,now_ms);g_hold_owner=0;sync_game_runtime(&g_runtime);refresh_view(&g_runtime);}
    portEXIT_CRITICAL(&g_runtime_lock);return accepted;
  }
  if(g_hold_owner&&g_hold_context!=hold_context()){pet_p4_game_release(&g_runtime.game.engine,true,now_ms);g_hold_owner=0;}
  bool accepted=!g_hold_owner&&g_runtime.active&&action&&g_runtime.hold_action[0]&&!strcmp(action,g_runtime.hold_action);
  unsigned revision=g_image_revision;
  portEXIT_CRITICAL(&g_runtime_lock);
  if(!accepted||!pet_p4_miniapp_dispatch_action(action,now_ms))return false;
  portENTER_CRITICAL(&g_runtime_lock);
  accepted=revision==g_image_revision&&g_runtime.game.engine.charging;
  if(accepted){g_hold_owner=owner;g_hold_context=hold_context();}
  portEXIT_CRITICAL(&g_runtime_lock);return accepted;
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

void pet_p4_miniapp_process(uint64_t now_ms, bool visible) {
  char pause[PET_P4_MINIAPP_ACTION_MAX]={0};
  portENTER_CRITICAL(&g_runtime_lock);
  if(g_input_visible!=visible){g_input_visible=visible;g_input_epoch++;g_confirm_action[0]=0;g_pressed_node=-1;pet_p4_game_release(&g_runtime.game.engine,true,now_ms);}
  if(!visible&&g_runtime.active&&g_runtime.game.active
     &&g_runtime.current_state==g_runtime.game.playing_state)
    copy_utf8(pause,sizeof(pause),g_runtime.pause_action);
  portEXIT_CRITICAL(&g_runtime_lock);
  if(pause[0])pet_p4_miniapp_dispatch_action(pause,now_ms);
  pet_p4_media_tick(now_ms);
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
  bool motion_confirm=g_runtime.game.engine.bounded.motion&&g_confirm_action[0]
    &&now_ms<g_confirm_until&&g_confirm_context==(((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page);
  if(g_runtime.game.active&&(!visible||motion_confirm||g_runtime.current_state!=g_runtime.game.playing_state))
    g_runtime.game.engine.last_tick_ms=now_ms;
  if (visible && !motion_confirm && g_runtime.game.active
      && g_runtime.current_state == g_runtime.game.playing_state) {
    changed |= pet_p4_game_process(&g_runtime.game.engine, now_ms);
    changed |= sync_game_runtime(&g_runtime);
  }
  if (changed) refresh_view(&g_runtime);
  if(g_runtime.local.enabled){
    int32_t values[7];local_values(&g_runtime,values,false);
    bool update=pet_local_tick(&g_runtime.local,&g_runtime.local_state,values,now_ms,local_wall_seconds(now_ms));
    local_values(&g_runtime,values,true);
    if(update||!g_runtime.view.eyebrow[0])refresh_local_view(&g_runtime,now_ms);
  }
done:
  portEXIT_CRITICAL(&g_runtime_lock);
  static uint64_t saved_at;
  if(now_ms-saved_at>=1000){saved_at=now_ms;save_app_runtime();}
}

bool pet_p4_miniapp_active(void) {
  bool active;
  portENTER_CRITICAL(&g_runtime_lock);
  active = g_runtime.active;
  portEXIT_CRITICAL(&g_runtime_lock);
  return active;
}

bool pet_p4_miniapp_slicing_active(void) {
  portENTER_CRITICAL(&g_runtime_lock);
  bool active=g_runtime.active&&g_runtime.slicing;
  portEXIT_CRITICAL(&g_runtime_lock);return active;
}
bool pet_p4_miniapp_fruit_snapshot(pet_fruit_t *out) {
  portENTER_CRITICAL(&g_runtime_lock);
  bool active=g_runtime.active&&g_runtime.slicing;
  if(active&&out)*out=g_runtime.fruit;
  portEXIT_CRITICAL(&g_runtime_lock);return active;
}
void pet_p4_miniapp_fruit_process(uint64_t now_ms,bool visible) {
  unsigned sounds=0,volume=0;
  bool audible=false;
  portENTER_CRITICAL(&g_runtime_lock);
  if(g_runtime.active&&g_runtime.slicing) {
    if(visible)pet_fruit_tick(&g_runtime.fruit,now_ms);
    else pet_fruit_suspend(&g_runtime.fruit,now_ms);
    sounds=g_runtime.fruit.sounds;volume=g_runtime.fruit.sound_volume;g_runtime.fruit.sounds=0;
    audible=visible&&g_runtime.fruit.phase!=PET_FRUIT_PAUSED;
  }
  portEXIT_CRITICAL(&g_runtime_lock);
  pet_p4_audio_fruit_sound(audible?sounds:0,volume,audible);
}
bool pet_p4_miniapp_pointer_active(void){
  portENTER_CRITICAL(&g_runtime_lock);
  bool active=g_runtime.active&&(g_runtime.surface.app||g_runtime.surface.media_touch||(g_runtime.instrument&&g_runtime.input_v1));
  portEXIT_CRITICAL(&g_runtime_lock);return active;
}
bool pet_p4_miniapp_input_policy(const char *event_name,bool press) {
  bool enabled=false;
  portENTER_CRITICAL(&g_runtime_lock);
  /* Only explicit opt-in permits repeated mutations. */
  if(g_runtime.active&&g_runtime.input_v1)for(int i=0;i<g_runtime.button_count;i++)
    if(!strcmp(event_name,g_runtime.buttons[i].event)){
      enabled=((press?g_runtime.press_mask:g_runtime.repeat_mask)&(1u<<i))!=0;
      break;
    }
  portEXIT_CRITICAL(&g_runtime_lock);return enabled;
}
uint64_t pet_p4_miniapp_input_context(void) {
  portENTER_CRITICAL(&g_runtime_lock);
  uint64_t context=((uint64_t)g_image_revision<<24)|((uint64_t)g_input_epoch<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;
  portEXIT_CRITICAL(&g_runtime_lock);return context;
}
bool pet_p4_miniapp_confirmation(char prompt[121],uint64_t now_ms) {
  portENTER_CRITICAL(&g_runtime_lock);
  uint64_t context=((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;
  bool active=g_confirm_action[0]&&now_ms<g_confirm_until&&context==g_confirm_context;
  if(!active)g_confirm_action[0]=0;
  if(active&&prompt)copy_utf8(prompt,121,g_confirm_prompt);
  portEXIT_CRITICAL(&g_runtime_lock);return active;
}

static void miniapp_drag_point(miniapp_runtime_t *r,int x,int y){
  const pet_surface_scene_view_t *v=&r->surface.scene_view;
  if(!r->drag_axis||!v->enabled||r->game.engine.kind!=PET_P4_GAME_BOUNDED||r->current_state!=r->game.playing_state)return;
  int w=r->game.engine.width,h=r->game.engine.height;if(!w||!h)return;
  int cell=v->w/w;if(v->h/h<cell)cell=v->h/h;if(cell<1)return;
  int ox=v->x+(v->w-cell*w)/2,oy=v->y+(v->h-cell*h)/2;
  for(int i=0;i<r->game.engine.bounded.entity_count;i++)if(!strcmp(r->drag_entity,r->game.engine.bounded.entities[i].id)){
    if(pet_p4_game_pointer(&r->game.engine,i,(x-ox)/cell,(y-oy)/cell,r->drag_axis)){sync_game_runtime(r);refresh_view(r);}
    break;
  }
}
void pet_p4_miniapp_pointer(const pet_p4_pointer_event_t *event) {
  static bool touch_holding=false;
  static int hold_x,hold_y,hold_w,hold_h;
  static bool pressed=false;
  static bool stroke=false;
  static bool early=false;
  static bool dragging=false;
  static int media_row=-1,slider_node=-1;
  static uint32_t media_revision;
  static int start_x,start_y;
  static uint64_t start_ms;
  static int8_t press_page,press_state;
  static int press_node=-1;
  static unsigned press_revision;
  static uint8_t press_epoch;
  static char press_widget[48],press_action[48];
  char dispatch[48]={0};
  int select_row=-1,set_volume=-1;
  char media_source[48]={0};
  if(!event)return;
  if(touch_holding&&event->kind!=PET_P4_POINTER_DOWN){
    bool outside=event->x<hold_x||event->x>=hold_x+hold_w||event->y<hold_y||event->y>=hold_y+hold_h;
    if(event->kind==PET_P4_POINTER_UP||event->kind==PET_P4_POINTER_CANCEL||outside){
      pet_p4_miniapp_hold_event(NULL,event->kind==PET_P4_POINTER_UP&&!outside?1:2,2,event->ts_ms);touch_holding=false;g_pressed_node=-1;
    }
    return;
  }
  if(touch_holding){pet_p4_miniapp_hold_event(NULL,2,2,event->ts_ms);touch_holding=false;}
  bool begin_hold=false;
    static EXT_RAM_BSS_ATTR pet_p4_media_view_t media_view;pet_p4_media_view(&media_view);
  if(pet_p4_miniapp_confirmation(NULL,event->ts_ms)){
    static int confirm_button;
    if(event->kind==PET_P4_POINTER_DOWN){
      confirm_button=event->y>=276&&event->y<332&&event->x>=136&&event->x<504?(event->x<312?1:2):0;
      start_x=event->x;start_y=event->y;start_ms=event->ts_ms;
    }else if(event->kind==PET_P4_POINTER_UP){
      int target=event->y>=276&&event->y<332&&event->x>=136&&event->x<504?(event->x<312?1:2):0;
      if(target&&target==confirm_button&&event->ts_ms-start_ms<700&&abs(event->x-start_x)<=14&&abs(event->y-start_y)<=14)
        pet_p4_miniapp_dispatch_action(target==1?"input.cancel":"input.confirm",event->ts_ms);
      confirm_button=0;
    }else if(event->kind==PET_P4_POINTER_CANCEL)confirm_button=0;
    stroke=pressed=dragging=false;g_pressed_node=-1;return;
  }
  portENTER_CRITICAL(&g_runtime_lock);
  if(dragging&&event->kind!=PET_P4_POINTER_DOWN){
    bool same=press_epoch==g_input_epoch&&press_state==g_runtime.current_state&&press_revision==g_image_revision&&!strcmp(press_widget,g_runtime.widget_id);
    if(same&&(event->kind==PET_P4_POINTER_MOVE||event->kind==PET_P4_POINTER_UP))miniapp_drag_point(&g_runtime,event->x,event->y);
    if(!same||event->kind==PET_P4_POINTER_UP||event->kind==PET_P4_POINTER_CANCEL)dragging=false;
    stroke=pressed=false;g_pressed_node=-1;portEXIT_CRITICAL(&g_runtime_lock);return;
  }
  if(event->kind==PET_P4_POINTER_DOWN)dragging=false;
  if(g_runtime.active&&g_runtime.slicing)pet_fruit_pointer(&g_runtime.fruit,event);
  if(g_runtime.active&&g_runtime.instrument&&g_runtime.input_v1&&event->kind==PET_P4_POINTER_DOWN)
    copy_utf8(dispatch,sizeof(dispatch),"instrument.strike");
  if(g_runtime.active&&(g_runtime.surface.app||g_runtime.surface.media_touch)){
    int current_page=g_runtime.surface.media_touch?media_view.page:g_runtime.current_page;
    if(event->kind==PET_P4_POINTER_DOWN){
      stroke=pressed=true;early=false;start_x=event->x;start_y=event->y;start_ms=event->ts_ms;press_action[0]=0;press_node=-1;
      media_row=slider_node=-1;media_revision=media_view.queue_revision;
      copy_utf8(press_widget,sizeof(press_widget),g_runtime.widget_id);
      press_page=current_page;press_state=g_runtime.current_state;press_revision=g_image_revision;
      press_epoch=g_input_epoch;
      int page=current_page<0?0:current_page;
      for(int i=g_runtime.surface.count-1;i>=0;i--){
        const pet_surface_node_t *n=&g_runtime.surface.nodes[i];
        if(n->state_mask&&!(n->state_mask&(1u<<g_runtime.current_state)))continue;
        if(g_runtime.app_host&&((n->host_mask&&!(n->host_mask&(1u<<pet_app_host_mode(g_runtime.widget_id,event->ts_ms))))||!pet_app_host_enabled(g_runtime.widget_id,n->action,event->ts_ms)))continue;
        if(g_runtime.input_v1&&g_runtime.surface.media_touch&&(n->page==2||n->page==current_page)){
          if(n->type==4&&n->h>=48&&event->x>=n->x&&event->x<n->x+n->w&&event->y>=n->y&&event->y<n->y+n->h){
            int rows=n->h/48,row=(media_view.selected/rows)*rows+(event->y-n->y)/48;
            if(row<media_view.count){media_row=row;press_node=i;break;}
          }
          if(n->type==2&&n->bind==4&&event->x>=n->x&&event->x<n->x+n->w&&event->y>=n->y-20&&event->y<n->y+n->h+20){
            slider_node=press_node=i;break;
          }
        }
        if(!runtime_action_enabled(&g_runtime,n->action))continue;
        int show=n->show_var[0]?find_var(&g_runtime,n->show_var):-1;
        if(show>=0&&(g_runtime.vars[show].int_value<n->show_min||g_runtime.vars[show].int_value>n->show_max))continue;
        if(n->action[0]&&(n->page==(g_runtime.surface.media_touch?2:4)||n->page==page)&&event->x>=n->x&&event->x<n->x+n->w&&event->y>=n->y&&event->y<n->y+n->h){copy_utf8(press_action,sizeof(press_action),n->action);press_node=i;break;}
      }
      g_pressed_node=press_node;
      if(press_node>=0&&g_runtime.hold_action[0]&&!strcmp(press_action,g_runtime.hold_action)){
        const pet_surface_node_t *n=&g_runtime.surface.nodes[press_node];hold_x=n->x;hold_y=n->y;hold_w=n->w;hold_h=n->h;
        copy_utf8(dispatch,sizeof(dispatch),press_action);begin_hold=true;early=true;
      }
      g_pressed_context=((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;
      for(int b=0;b<g_runtime.button_count;b++)if(g_runtime.input_v1&&(g_runtime.press_mask&(1u<<b))&&!strcmp(press_action,g_runtime.buttons[b].action)){
        copy_utf8(dispatch,sizeof(dispatch),press_action);early=true;break;
      }
      const pet_surface_scene_view_t *v=&g_runtime.surface.scene_view;
      if(press_node<0&&g_runtime.drag_axis&&v->enabled&&g_runtime.current_state==g_runtime.game.playing_state
         &&event->x>=v->x&&event->x<v->x+v->w&&event->y>=v->y&&event->y<v->y+v->h){
        dragging=true;early=true;miniapp_drag_point(&g_runtime,event->x,event->y);
      }
    }else if(event->kind==PET_P4_POINTER_MOVE&&(abs(event->x-start_x)>14||abs(event->y-start_y)>14)){pressed=false;g_pressed_node=-1;}
    else if(event->kind==PET_P4_POINTER_UP){
      bool same=stroke&&!early&&press_epoch==g_input_epoch&&press_page==current_page&&press_state==g_runtime.current_state&&press_revision==g_image_revision&&!strcmp(press_widget,g_runtime.widget_id)&&event->ts_ms>=start_ms&&event->ts_ms-start_ms<700;
      int dx=event->x-start_x,dy=event->y-start_y,ax=abs(dx),ay=abs(dy);
      bool media_same=stroke&&press_epoch==g_input_epoch&&press_page==current_page&&press_revision==g_image_revision&&media_revision==media_view.queue_revision&&!strcmp(press_widget,g_runtime.widget_id)&&event->ts_ms>=start_ms;
      if(media_same&&media_row>=0&&event->ts_ms-start_ms<700&&ax<=14&&ay<=14){
        select_row=media_row;copy_utf8(media_source,48,g_runtime.media_source);pressed=false;
      }else if(media_same&&slider_node>=0&&slider_node<g_runtime.surface.count&&event->ts_ms-start_ms<10000&&ay<=28){
        const pet_surface_node_t *n=&g_runtime.surface.nodes[slider_node];
        set_volume=(event->x-n->x)*100/n->w;if(set_volume<0)set_volume=0;if(set_volume>100)set_volume=100;
        copy_utf8(media_source,48,g_runtime.media_source);pressed=false;
      }
      if(same&&g_runtime.input_v1&&(ax>=40||ay>=40)&&(ax*2>=ay*3||ay*2>=ax*3)){
        int direction=ax>ay?(dx<0?0:1):(dy<0?2:3);
        const char *action=g_runtime.swipe_actions[direction];
        if(action[0]&&runtime_action_enabled(&g_runtime,action))copy_utf8(dispatch,sizeof(dispatch),action);
        pressed=false;
      }
      if(press_node<0||press_node>=g_runtime.surface.count)pressed=false;
      else {
        const pet_surface_node_t *n=&g_runtime.surface.nodes[press_node];
        int show=n->show_var[0]?find_var(&g_runtime,n->show_var):-1;
        if(show>=0&&(g_runtime.vars[show].int_value<n->show_min||g_runtime.vars[show].int_value>n->show_max))pressed=false;
        if(event->x<n->x||event->x>=n->x+n->w||event->y<n->y||event->y>=n->y+n->h)pressed=false;
      }
      if(pressed&&same&&runtime_action_enabled(&g_runtime,press_action)&&abs(event->x-start_x)<=14&&abs(event->y-start_y)<=14)copy_utf8(dispatch,sizeof(dispatch),press_action);
      stroke=pressed=false;g_pressed_node=-1;
    }else if(event->kind==PET_P4_POINTER_CANCEL){stroke=pressed=false;g_pressed_node=-1;}
  }else {stroke=pressed=false;g_pressed_node=-1;}
  portEXIT_CRITICAL(&g_runtime_lock);
  if(media_source[0])pet_p4_media_touch(media_source,select_row,set_volume,media_revision,event->ts_ms);
  if(dispatch[0]){if(begin_hold){touch_holding=pet_p4_miniapp_hold_event(dispatch,0,2,event->ts_ms);stroke=pressed=false;}else pet_p4_miniapp_dispatch_action(dispatch,event->ts_ms);}
}

bool pet_p4_miniapp_media_active(void) {
  portENTER_CRITICAL(&g_runtime_lock);
  bool active=g_runtime.active&&g_runtime.media;
  portEXIT_CRITICAL(&g_runtime_lock);return active;
}
bool pet_p4_miniapp_app_surface(pet_surface_t *surface) {
  portENTER_CRITICAL(&g_runtime_lock);
  bool active=g_runtime.active&&g_runtime.surface.app;
  if(active&&surface){
    *surface=g_runtime.surface;
    static const char *const events[]={"button.sw1.short_press","button.sw2.short_press","button.sw3.short_press","button.encoder.short_press","button.encoder.long_press","knob.rotate_ccw","knob.rotate_cw","joystick.up","joystick.down"};
    static const char *const labels[]={"SW1","SW2","SW3","中按","中长","左","右","上","下"};
    for(int i=0;i<surface->count;i++){
      pet_surface_node_t *n=&surface->nodes[i];
      n->disabled=n->action[0]&&!runtime_action_enabled(&g_runtime,n->action);
      if(g_runtime.app_host&&n->action[0]&&!pet_app_host_enabled(g_runtime.widget_id,n->action,esp_timer_get_time()/1000ULL))n->disabled=true;
      uint64_t context=((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;
      n->pressed=!n->disabled&&i==g_pressed_node&&g_pressed_context==context;
      int show=n->show_var[0]?find_var(&g_runtime,n->show_var):-1;
      n->hidden=(show>=0&&(g_runtime.vars[show].int_value<n->show_min||g_runtime.vars[show].int_value>n->show_max))||(n->state_mask&&!(n->state_mask&(1u<<g_runtime.current_state)));
      if(n->host_mask&&!(n->host_mask&(1u<<pet_app_host_mode(g_runtime.widget_id,esp_timer_get_time()/1000ULL))))n->hidden=true;
      int tile=n->tile_var[0]?find_var(&g_runtime,n->tile_var):-1;
      if(tile>=0&&n->tile_cols){
        int64_t value=g_runtime.vars[tile].int_value;
        int max=n->tile_max_var[0]?find_var(&g_runtime,n->tile_max_var):-1;
        if(max>=0){int32_t total=g_runtime.vars[max].int_value;value=total>0?value*n->tile_cols/total:0;if(value<0)value=0;if(value>=n->tile_cols)value=n->tile_cols-1;}
        else value=(value%n->tile_cols+n->tile_cols)%n->tile_cols;
        n->tile_index=value;
      }
      if(!n->key_for[0])continue;
      copy_utf8(n->text,sizeof(n->text),"—");
      for(int b=0;b<g_runtime.button_count;b++)if(!strcmp(n->key_for,g_runtime.buttons[b].action)){
        for(int e=0;e<9;e++)if(!strcmp(g_runtime.buttons[b].event,events[e]))copy_utf8(n->text,sizeof(n->text),labels[e]);
        break;
      }
    }
  }
  portEXIT_CRITICAL(&g_runtime_lock);return active;
}
bool pet_p4_miniapp_media_config(char source[48],pet_surface_t *surface) {
  portENTER_CRITICAL(&g_runtime_lock);
  bool active=g_runtime.active&&g_runtime.media&&g_runtime.surface.count;
  if(active){memcpy(source,g_runtime.media_source,48);if(surface)*surface=g_runtime.surface;}
  portEXIT_CRITICAL(&g_runtime_lock);
  return active;
}

bool pet_p4_miniapp_media_surface_animated(void) {
  bool animated=false;
  portENTER_CRITICAL(&g_runtime_lock);
  if(g_runtime.active&&g_runtime.media)for(int i=0;i<g_runtime.surface.count;i++){
    if(g_runtime.surface.nodes[i].rotate_ms){animated=true;break;}
  }
  portEXIT_CRITICAL(&g_runtime_lock);
  return animated;
}

bool pet_p4_miniapp_instrument_config(uint8_t *effect, uint8_t *ambience) {
  /* Control loops need two bytes, not a multi-kilobyte render snapshot. */
  portENTER_CRITICAL(&g_runtime_lock);
  bool active = g_runtime.active && g_runtime.instrument;
  if (effect) *effect = active ? g_runtime.effect_volume : 0;
  if (ambience) *ambience = active ? g_runtime.ambience_volume : 0;
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
  if(out->app_host)pet_app_host_view(out,esp_timer_get_time()/1000ULL);
  pet_p4_widget_data_view(out->data_source, out->data_page, esp_timer_get_time() / 1000ULL, &out->data);
  return true;
}

void pet_p4_miniapp_host_process(uint64_t now_ms,bool visible){
  char media_source[48]="";
  bool media_active=pet_p4_miniapp_media_config(media_source,NULL);
  pet_p4_media_visibility(media_source,visible&&media_active,now_ms);
  static char prior[48];
  char id[48]="";uint32_t rev;uint8_t page;
  portENTER_CRITICAL(&g_runtime_lock);
  if(g_runtime.active&&g_runtime.app_host)copy_utf8(id,sizeof(id),g_runtime.widget_id);
  rev=g_image_revision;page=g_runtime.current_page<0?0:g_runtime.current_page;
  portEXIT_CRITICAL(&g_runtime_lock);
  if(prior[0]&&(!visible||strcmp(prior,id))){
    static EXT_RAM_BSS_ATTR pet_p4_media_view_t media;pet_p4_media_view(&media);
    if(!strncmp(media.source,"app.",4)&&!strcmp(media.source+4,prior)&&pet_p4_audio_music_active())pet_p4_audio_stream_flush();
  }
  copy_utf8(prior,sizeof(prior),id);
  if(id[0])pet_app_host_tick(id,rev,page,visible,now_ms);
}
bool pet_p4_miniapp_host_source(const char *source){
  if(!source||strncmp(source,"app.",4))return false;
  portENTER_CRITICAL(&g_runtime_lock);
  bool ok=g_runtime.active&&g_runtime.app_host&&!strcmp(source+4,g_runtime.widget_id);
  portEXIT_CRITICAL(&g_runtime_lock);return ok;
}

bool pet_p4_miniapp_get_surface_images(pet_p4_miniapp_sprite_pack_t *out,uint32_t *revision) {
  portENTER_CRITICAL(&g_runtime_lock);
  bool changed=*revision!=g_image_revision;
  if(changed){*revision=g_image_revision;out->count=g_runtime.sprites.count;out->data_length=g_runtime.sprites.data_length;
    memcpy(out->items,g_runtime.sprites.items,sizeof(out->items));memcpy(out->pixels,g_runtime.sprites.pixels,out->data_length);}
  portEXIT_CRITICAL(&g_runtime_lock);return changed;
}

bool pet_p4_miniapp_get_sprites(pet_p4_miniapp_sprite_pack_t *out) {
  if (!out) return false;
  portENTER_CRITICAL(&g_runtime_lock);
  out->count=g_runtime.sprites.count;out->data_length=g_runtime.sprites.data_length;
  memcpy(out->items,g_runtime.sprites.items,sizeof(out->items));
  memcpy(out->pixels,g_runtime.sprites.pixels,out->data_length);
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
  if (!file) {
    snprintf(g_storage_failure, sizeof(g_storage_failure), "open %s errno=%d", tmp_path, errno);
    return false;
  }
  bool ok = fwrite(data, 1, len, file) == len && fflush(file) == 0;
  int saved_errno = errno;
  if (fclose(file) != 0) { ok = false; saved_errno = errno; }
  /*
   * SPIFFS garbage collection can take several seconds when the appearance
   * partition is populated. Let the idle task run between transactional file
   * writes so component commit never starves the task watchdog.
   */
  vTaskDelay(pdMS_TO_TICKS(1));
  if (!ok) {
    snprintf(g_storage_failure, sizeof(g_storage_failure), "write/close %s errno=%d bytes=%u", tmp_path, saved_errno, (unsigned)len);
    remove(tmp_path);
    return false;
  }
  if (remove(path) != 0 && errno != ENOENT) {
    snprintf(g_storage_failure, sizeof(g_storage_failure), "replace %s errno=%d", path, errno);
    remove(tmp_path);
    return false;
  }
  if (rename(tmp_path, path) != 0) {
    snprintf(g_storage_failure, sizeof(g_storage_failure), "rename %s errno=%d", path, errno);
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
    // Preserve existing package checksums; high dimensions only exist in P4J1.
    if(sprite->frame_width>255||sprite->frame_height>255){
      uint8_t hi[2]={sprite->frame_width>>8,sprite->frame_height>>8};SPRITE_CHECKSUM_BYTES(hi,2);
    }
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
      || (memcmp(file_data, "P4S1", 4) != 0 && memcmp(file_data,"P4I1",4)!=0 && memcmp(file_data,"P4I2",4)!=0 && memcmp(file_data,"P4J1",4)!=0)) {
    set_error(error, error_size, "sprite file header is unsupported");
    return false;
  }
  if(!memcmp(file_data,"P4I1",4)||!memcmp(file_data,"P4I2",4)||!memcmp(file_data,"P4J1",4)) {
    if(!pet_image_decode(file_data,file_length,NULL,0)||pack->data_length+file_length>sizeof(pack->pixels)){set_error(error,error_size,"invalid bounded image");return false;}
    sprite=&pack->items[pack->count++];memset(sprite,0,sizeof(*sprite));copy_utf8(sprite->id,sizeof(sprite->id),id);
    bool wide=file_data[2]=='J'||file_data[3]=='2';sprite->frame_width=wide?(file_data[4]|(uint16_t)file_data[5]<<8):file_data[4];sprite->frame_height=wide?(file_data[6]|(uint16_t)file_data[7]<<8):file_data[5];sprite->frames=1;sprite->fps=0;
    sprite->data_offset=pack->data_length;sprite->data_length=file_length;
    memcpy(pack->pixels+pack->data_length,file_data,file_length);pack->data_length+=file_length;return true;
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
  if (pixel_bytes>4096*3 || file_length != MINIAPP_SPRITE_FILE_HEADER_SIZE + pixel_bytes
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
  sprite->data_length = (uint32_t) pixel_bytes;
  memcpy(
    pack->pixels + pack->data_length,
    file_data + MINIAPP_SPRITE_FILE_HEADER_SIZE,
    pixel_bytes
  );
  pack->data_length = (uint32_t) ((size_t) pack->data_length + pixel_bytes);
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
  if(!sprites && root) sprites=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"surface"),"images");
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
  /* Surface images are already encoded and written directly. Only legacy
   * animated sprites need a temporary P4S header; avoid an unused 128 KiB
   * allocation during every full-resolution surface transaction. */
  for (uint8_t i = 0; i < pack->count; ++i) {
    if (pack->items[i].fps) {
      file_data = (uint8_t *) malloc(MINIAPP_SPRITE_FILE_MAX);
      if (!file_data) { snprintf(g_storage_failure, sizeof(g_storage_failure), "sprite scratch allocation failed"); return false; }
      break;
    }
  }
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
    if(sprite->fps==0){
      if(!write_file_atomic(temporary_path,path,(const char*)pack->pixels+sprite->data_offset,sprite->data_length)){ok=false;break;}
      continue;
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
  if (ok) {
    item->image_count = sprites->count;
    item->resource_state = pet_p4_builtin_is_bootstrap(item->widget_id, widget_checksum) ? 2 : 1;
  }

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
  bool complete = true;
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
      snapshot->items[i].resource_state = 3;
      complete = false;
      continue;
    }
    snapshot->needs_upgrade |= missing_checksums;
  }
  free(runtime);
  snapshot->valid = complete;
  return complete;
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
  snapshot->parsed = true;
  (void) validate_catalog_snapshot(snapshot);
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
    if(runtime&&runtime->surface.scene_view.enabled)return true;
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
  if(!strcmp(name,"charge")){
    static const char *const keys[]={"op","entity"};
    if(!config->motion||!object_keys_allowed(object,keys,2)||!parse_bounded_entity_ref(config,object,"entity",&op->entity_index))return false;
    op->kind=PET_P4_GAME_OP_CHARGE;return true;
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
    } else if (strcmp(on, "collision") == 0 || (config->motion&&!strcmp(on,"landed"))) {
      rule->trigger = !strcmp(on,"landed")?PET_P4_GAME_TRIGGER_LANDED:PET_P4_GAME_TRIGGER_COLLISION;
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
    "grid", "sprites", "entities", "rules", "motion",
  };
  pet_p4_bounded_game_config_t config;
  const cJSON *tick;
  const cJSON *auto_start;
  const char *score_var;
  const char *result_state;
  int tick_ms;
  if (!scene) return true;
  if (!strcmp(json_string(scene,"engine"),"p4-touch-slice-v1")) {
    static const char *const keys[]={"engine","duration_sec","lives","sound_volume","sprites","revision"};
    int seconds,lives,volume,revision=1;
    const cJSON *revision_value=cJSON_GetObjectItemCaseSensitive(scene,"revision");
    if(!runtime_v4 || !object_keys_allowed(scene,keys,6)
       || (revision_value&&!json_int_range(revision_value,1,2,&revision))
       || runtime->var_count || runtime->tick_count || runtime->page_count || runtime->media || runtime->instrument || runtime->data_source[0]
       || runtime->state_count!=1 || strcmp(runtime->states[0],"idle")
       || !json_int_range(cJSON_GetObjectItemCaseSensitive(scene,"duration_sec"),30,120,&seconds)
       || !json_int_range(cJSON_GetObjectItemCaseSensitive(scene,"lives"),1,5,&lives)
       || !json_int_range(cJSON_GetObjectItemCaseSensitive(scene,"sound_volume"),0,100,&volume)
       || runtime->transition_count!=2 || runtime->button_count!=2
       || !runtime_has_transition_action(runtime,"slice.restart") || !runtime_has_transition_action(runtime,"slice.pause")
       || !runtime_has_button_action(runtime,"slice.restart") || !runtime_has_button_action(runtime,"slice.pause")) {
      set_error(error,error_size,"touch slice requires ready touch, bounded config and restart/pause controls");return false;
    }
    for(unsigned i=0;i<runtime->transition_count;i++) {
      const miniapp_transition_t *t=&runtime->transitions[i];
      if(t->from_state>=0 || t->to_state>=0 || t->effect_count || t->page_op!=-1) {
        set_error(error,error_size,"touch slice actions cannot contain extra effects");return false;
      }
    }
    if(!parse_bounded_sprites(runtime,cJSON_GetObjectItemCaseSensitive(scene,"sprites"),true,error,error_size))return false;
    if(runtime->sprites.count!=4){set_error(error,error_size,"touch slice requires four fruit sprites");return false;}
    for(unsigned i=0;i<4;i++)if(runtime->sprites.items[i].frames!=1){set_error(error,error_size,"fruit sprites use one frame");return false;}
    runtime->slicing=true;pet_fruit_init(&runtime->fruit,seconds,lives,volume);return true;
  }
  if (!runtime || !object_keys_allowed(scene, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
    set_error(error, error_size, "scene contains an unsupported field");
    return false;
  }
  memset(&config, 0, sizeof(config));
  config.motion=cJSON_GetObjectItemCaseSensitive(scene,"motion")!=NULL;
  runtime->game.charge_var=-1;
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
  if (!json_int_range(tick, config.motion?16:100, config.motion?40:2000, &tick_ms)) {
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
  if(config.motion){
    const cJSON *m=cJSON_GetObjectItemCaseSensitive(scene,"motion"),*platforms=cJSON_GetObjectItemCaseSensitive(m,"platforms"),*speed=cJSON_GetObjectItemCaseSensitive(m,"speed");
    static const char *const keys[]={"engine","body","platforms","gravity","lift","speed","charge_ms","charge_var","track"};
    int gravity,lift,low,high,ms;
    config.motion_body=bounded_find_entity(&config,json_string(m,"body"));
    runtime->game.charge_var=find_var(runtime,json_string(m,"charge_var"));
    if(!object_keys_allowed(m,keys,9)||strcmp(json_string(m,"engine"),"p4-motion-v1")||config.motion_body<0
      ||runtime->game.charge_var<0||runtime->game.charge_var==runtime->game.score_var||runtime->vars[runtime->game.charge_var].type!=MINIAPP_VAR_INT
      ||!cJSON_IsArray(platforms)||cJSON_GetArraySize(platforms)<1||cJSON_GetArraySize(platforms)>11
      ||!json_int_range(cJSON_GetObjectItemCaseSensitive(m,"gravity"),4,64,&gravity)
      ||!json_int_range(cJSON_GetObjectItemCaseSensitive(m,"lift"),2,16,&lift)
      ||!json_int_range(cJSON_GetObjectItemCaseSensitive(m,"charge_ms"),300,3000,&ms)
      ||!cJSON_IsArray(speed)||cJSON_GetArraySize(speed)!=2
      ||!json_int_range(cJSON_GetArrayItem(speed,0),1,24,&low)||!json_int_range(cJSON_GetArrayItem(speed,1),low,24,&high)
      ||config.solid_tone_mask||cJSON_GetObjectItemCaseSensitive(scene,"sprites"))return false;
    const cJSON *p;cJSON_ArrayForEach(p,platforms){int i=cJSON_IsString(p)?bounded_find_entity(&config,p->valuestring):-1;if(i<0||i==config.motion_body||(config.platform_mask&(1u<<i)))return false;config.platform_mask|=1u<<i;}
    config.gravity=gravity;config.lift=lift;config.launch_min=low;config.launch_max=high;config.charge_ms=ms;
    const cJSON *track=cJSON_GetObjectItemCaseSensitive(m,"track");
    if(track){
      static const char *const tk[]={"engine","camera_x","gap","width","tones"};
      const cJSON *gap=cJSON_GetObjectItemCaseSensitive(track,"gap"),*width=cJSON_GetObjectItemCaseSensitive(track,"width"),*tones=cJSON_GetObjectItemCaseSensitive(track,"tones");
      int camera,gap_min,gap_max,width_min,width_max;
      if(!object_keys_allowed(track,tk,5)||strcmp(json_string(track,"engine"),"p4-platform-track-v1")
        ||!json_int_range(cJSON_GetObjectItemCaseSensitive(track,"camera_x"),2,config.width/2,&camera)
        ||!cJSON_IsArray(gap)||cJSON_GetArraySize(gap)!=2||!cJSON_IsArray(width)||cJSON_GetArraySize(width)!=2
        ||!json_int_range(cJSON_GetArrayItem(gap,0),1,6,&gap_min)||!json_int_range(cJSON_GetArrayItem(gap,1),gap_min,6,&gap_max)
        ||!json_int_range(cJSON_GetArrayItem(width,0),2,4,&width_min)||!json_int_range(cJSON_GetArrayItem(width,1),width_min,4,&width_max)
        ||!cJSON_IsArray(tones)||cJSON_GetArraySize(tones)<2||cJSON_GetArraySize(tones)>4)return false;
      const cJSON *tone;cJSON_ArrayForEach(tone,tones){int t;if(!json_int_range(tone,1,4,&t)||(config.tone_mask&(1u<<t)))return false;config.tone_mask|=1u<<t;}
      config.track=true;config.camera_x=camera;config.gap_min=gap_min;config.gap_max=gap_max;config.width_min=width_min;config.width_max=width_max;
    }
    for(int i=0;i<config.entity_count;i++)if(config.entities[i].vx||config.entities[i].vy)return false;
    bool has_charge=false;
    for(int i=0;i<config.rule_count;i++){
      const pet_p4_game_rule_t *r=&config.rules[i];
      if(r->trigger==PET_P4_GAME_TRIGGER_COLLISION||r->trigger==PET_P4_GAME_TRIGGER_BLOCKED||r->trigger==PET_P4_GAME_TRIGGER_TICK)return false;
      for(int j=0;j<r->op_count;j++){
        const pet_p4_game_op_t *o=&r->ops[j];
        if(o->kind==PET_P4_GAME_OP_CHARGE){if(o->entity_index!=config.motion_body||r->trigger!=PET_P4_GAME_TRIGGER_ACTION||strcmp(r->action,runtime->hold_action))return false;has_charge=true;}
        if(o->kind==PET_P4_GAME_OP_MOVE||o->kind==PET_P4_GAME_OP_VELOCITY||o->kind==PET_P4_GAME_OP_ACCELERATE||o->kind==PET_P4_GAME_OP_BOUNCE)return false;
      }
    }
    if(!has_charge||!runtime->hold_action[0])return false;
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
  if (g_catalog[index].resource_state == 1
      && load_catalog_runtime_item(&g_catalog[index], runtime, false, error, error_size)) return true;
  if (g_catalog[index].resource_state == 1) g_catalog[index].resource_state = 3;
  {
    memset(runtime, 0, sizeof(*runtime));
    runtime->active = true;
    runtime->resource_incomplete = true;
    copy_utf8(runtime->widget_id, sizeof(runtime->widget_id), g_catalog[index].widget_id);
    runtime->view.active = true;
    copy_utf8(runtime->view.widget_id, sizeof(runtime->view.widget_id), g_catalog[index].widget_id);
    copy_utf8(runtime->view.title, sizeof(runtime->view.title), g_catalog[index].title);
    copy_utf8(runtime->view.headline, sizeof(runtime->view.headline), "组件资源需要恢复");
    copy_utf8(runtime->view.note, sizeof(runtime->view.note), "请连接客户端恢复完整资源，原有记录已保留。");
    return true;
  }
}

/* Recover only this component from an independently verified committed catalog.
 * Never roll the entire directory back or trust an uncommitted spare generation.
 * A known complete package must not degrade to an image-free fallback. */
static bool recover_catalog_items(miniapp_catalog_snapshot_t *latest,
                                  const miniapp_catalog_snapshot_t *older) {
  bool changed = false;
  if (!latest || !older || !older->parsed) return false;
  for (size_t i = 0; i < latest->count; ++i) {
    miniapp_catalog_item_t *item = &latest->items[i];
    if (item->resource_state == 1) continue;
    int index = catalog_find(older->items, older->count, item->widget_id);
    if (index < 0) continue;
    const miniapp_catalog_item_t *fallback = &older->items[index];
    if (fallback->resource_state != 1 || fallback->slot != item->slot
        || (item->sprites_checksum && !fallback->sprites_checksum)) continue;
    *item = *fallback;
    changed = true;
  }
  return changed;
}

static bool catalog_entry_visible(size_t index) {
  return index < g_catalog_count && strcmp(g_catalog[index].widget_id, "music-player") != 0;
}

static int catalog_visible_index(size_t visible_index) {
  for (size_t i = 0; i < g_catalog_count; ++i) {
    if (!catalog_entry_visible(i)) continue;
    if (visible_index-- == 0) return (int)i;
  }
  return -1;
}

static int catalog_visible_preferred(int preferred) {
  return preferred >= 0 && catalog_entry_visible((size_t)preferred)
    ? preferred : catalog_visible_index(0);
}

static bool activate_catalog_index_in_memory(size_t index) {
  miniapp_runtime_t *next;
  char error[128] = {0};
  if (!catalog_entry_visible(index)) return false;
  next = (miniapp_runtime_t *) calloc(1, sizeof(*next));
  if (!next) return false;
  if (!load_catalog_runtime(index, next, error, sizeof(error))) {
    ESP_LOGW(TAG, "failed to open component %s: %s", g_catalog[index].widget_id, error);
    free(next);
    return false;
  }
  save_app_runtime();restore_app_runtime(next);
  portENTER_CRITICAL(&g_runtime_lock);
  g_runtime = *next; g_image_revision++;
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
  if (!catalog_entry_visible(index)) return false;
  /* Reopening the already loaded media component is navigation, not an
   * install/reset. Rewriting its unchanged catalog can stall the serial task
   * behind Flash I/O long enough to exhaust live audio. Package commit/delete
   * replace g_runtime atomically, so this never bypasses package validation.
   * Games deliberately retain their existing restart-on-open behavior. */
  portENTER_CRITICAL(&g_runtime_lock);
  bool resume_media = g_runtime.active && (g_runtime.media||g_runtime.save_version)
    && strcmp(g_runtime.widget_id, g_catalog[index].widget_id) == 0;
  if (resume_media) g_catalog_selected = index;
  portEXIT_CRITICAL(&g_runtime_lock);
  if (resume_media) return true;
  next = (miniapp_runtime_t *) calloc(1, sizeof(*next));
  if (!next) return false;
  if (!load_catalog_runtime(index, next, error, sizeof(error))) {
    ESP_LOGW(TAG, "failed to open component %s: %s", g_catalog[index].widget_id, error);
    free(next);
    return false;
  }
  /* Navigation is not a package transaction. Keep the previous committed
   * directory as a recovery source instead of overwriting it on each open. */
  if (!write_file_atomic(MINIAPP_LAST_ID_TMP_PATH, MINIAPP_LAST_ID_PATH,
                         g_catalog[index].widget_id, strlen(g_catalog[index].widget_id))) {
    ESP_LOGW(TAG, "failed to commit active component id=%s", g_catalog[index].widget_id);
    free(next);
    return false;
  }
  save_app_runtime();restore_app_runtime(next);
  portENTER_CRITICAL(&g_runtime_lock);
  g_runtime = *next; g_image_revision++;
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

static bool remove_component(const char *widget_id, char *error, size_t error_size, bool preserve_save);
bool pet_p4_miniapp_remove(const char *widget_id, char *error, size_t error_size) {
  return remove_component(widget_id, error, error_size, false);
}
bool pet_p4_miniapp_remove_preserving_save(const char *widget_id, char *error, size_t error_size) {
  return remove_component(widget_id, error, error_size, true);
}

esp_err_t pet_p4_miniapp_init(void) {
  pet_p4_media_init();
  if(!pet_app_store_init())return ESP_ERR_NO_MEM;
  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX + 1] = {0};
  char error[128] = {0};
  miniapp_catalog_snapshot_t *snapshot0;
  miniapp_catalog_snapshot_t *snapshot1;
  miniapp_catalog_snapshot_t *selected = NULL;
  uint32_t highest_seen_sequence = 0;
  memset(&g_runtime, 0, sizeof(g_runtime));
  memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
  memset(g_catalog, 0, sizeof(g_catalog));
  g_catalog_count = 0;
  g_catalog_selected = 0;
  g_catalog_file_generation = -1;
  g_catalog_sequence = 0;
  g_catalog_recovery_blocked = false;
  struct stat catalog_stat;
  char catalog_path[48];
  bool catalog_files_present = false;
  for (uint8_t i = 0; i < MINIAPP_CATALOG_GENERATION_COUNT; ++i) {
    catalog_snapshot_path(i, false, catalog_path, sizeof(catalog_path));
    if (stat(catalog_path, &catalog_stat) == 0 || errno != ENOENT) catalog_files_present = true;
  }

  snapshot0 = (miniapp_catalog_snapshot_t *) calloc(1, sizeof(*snapshot0));
  snapshot1 = (miniapp_catalog_snapshot_t *) calloc(1, sizeof(*snapshot1));
  if (!snapshot0 || !snapshot1) {
    g_catalog_recovery_blocked = true;
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
  if (snapshot0->parsed) selected = snapshot0;
  if (snapshot1->parsed && (!selected || snapshot1->sequence > selected->sequence)) {
    selected = snapshot1;
  }

  if (selected) {
    bool recovered = recover_catalog_items(selected, selected == snapshot0 ? snapshot1 : snapshot0);
    memcpy(g_catalog, selected->items, sizeof(g_catalog));
    g_catalog_count = selected->count;
    g_catalog_file_generation = selected->file_generation;
    g_catalog_sequence = highest_seen_sequence;
    copy_utf8(widget_id, sizeof(widget_id), selected->active_widget_id);
    char last_opened[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
    if (read_file(MINIAPP_LAST_ID_PATH, last_opened, sizeof(last_opened))
        && catalog_find(g_catalog, g_catalog_count, last_opened) >= 0) {
      copy_utf8(widget_id, sizeof(widget_id), last_opened);
    }
    if (!widget_id[0] && g_catalog_count > 0) {
      copy_utf8(widget_id, sizeof(widget_id), g_catalog[0].widget_id);
      selected->needs_upgrade = true;
    }
    int preferred = catalog_find(g_catalog, g_catalog_count, widget_id);
    if (preferred < 0 && g_catalog_count > 0) preferred = 0;
    if ((selected->needs_upgrade || recovered)
        && !persist_catalog_snapshot(g_catalog, g_catalog_count, widget_id)) {
      ESP_LOGW(TAG, "component catalog recovery could not be committed");
    }
    preferred = catalog_visible_preferred(preferred);
    bool activated = preferred < 0 || activate_catalog_index_in_memory((size_t) preferred);
    ESP_LOGI(
      TAG,
      "restored component catalog count=%u generation=%d sequence=%u%s",
      (unsigned int) g_catalog_count,
      g_catalog_file_generation,
      (unsigned int) g_catalog_sequence,
      recovered ? " after per-component recovery" : ""
    );
    free(snapshot0);
    free(snapshot1);
    return activated ? ESP_OK : ESP_ERR_INVALID_STATE;
  }

  if (catalog_files_present) {
    /* Do not overwrite unrecognized/corrupt modern directories with legacy or
     * bootstrap data. Leave every file/save intact for explicit recovery. */
    g_catalog_recovery_blocked = true;
    free(snapshot0); free(snapshot1);
    ESP_LOGE(TAG, "component directories unreadable; automatic bootstrap replacement blocked");
    return ESP_ERR_INVALID_STATE;
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
    preferred = catalog_visible_preferred(preferred);
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
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
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
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
  ESP_LOGI(TAG, "migrated bounded component id=%s states=%u vars=%u", g_runtime.widget_id,
           (unsigned int) g_runtime.state_count, (unsigned int) g_runtime.var_count);
  if (!catalog_entry_visible(0)) memset(&g_runtime, 0, sizeof(g_runtime));
  return ESP_OK;
}

bool pet_p4_miniapp_catalog_recovery_blocked(void) {
  return g_catalog_recovery_blocked;
}

bool pet_p4_miniapp_resource_repair_active(void) {
  return g_staging.active && g_staging.repair_resources;
}

size_t pet_p4_miniapp_catalog_count(void) {
  size_t count;
  portENTER_CRITICAL(&g_runtime_lock);
  count = 0;
  for (size_t i = 0; i < g_catalog_count; ++i) if (catalog_entry_visible(i)) ++count;
  portEXIT_CRITICAL(&g_runtime_lock);
  return count;
}

size_t pet_p4_miniapp_catalog_selected(void) {
  size_t selected;
  portENTER_CRITICAL(&g_runtime_lock);
  selected = 0;
  for (size_t i = 0; i < g_catalog_selected; ++i) if (catalog_entry_visible(i)) ++selected;
  portEXIT_CRITICAL(&g_runtime_lock);
  return selected;
}

bool pet_p4_miniapp_catalog_get(size_t index, pet_p4_miniapp_catalog_entry_t *out) {
  bool available = false;
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  portENTER_CRITICAL(&g_runtime_lock);
  int raw_index = catalog_visible_index(index);
  if (raw_index >= 0) {
    index = (size_t)raw_index;
    copy_utf8(out->widget_id, sizeof(out->widget_id), g_catalog[index].widget_id);
    copy_utf8(out->title, sizeof(out->title), g_catalog[index].title);
    out->active = g_runtime.active
      && strcmp(g_runtime.widget_id, g_catalog[index].widget_id) == 0;
    out->resource_state = g_catalog[index].resource_state;
    out->image_count = g_catalog[index].image_count;
    available = true;
  }
  portEXIT_CRITICAL(&g_runtime_lock);
  return available;
}

void pet_p4_miniapp_catalog_focus_active(void) {
  portENTER_CRITICAL(&g_runtime_lock);
  for (size_t i = 0; i < g_catalog_count; i += 1) {
    if (catalog_entry_visible(i) && g_runtime.active && strcmp(g_runtime.widget_id, g_catalog[i].widget_id) == 0) {
      g_catalog_selected = i;
      break;
    }
  }
  portEXIT_CRITICAL(&g_runtime_lock);
}

bool pet_p4_miniapp_catalog_select(size_t index) {
  portENTER_CRITICAL(&g_runtime_lock);
  bool valid=index<g_catalog_count&&catalog_entry_visible(index);
  if(valid)g_catalog_selected=index;
  portEXIT_CRITICAL(&g_runtime_lock);
  return valid;
}

bool pet_p4_miniapp_catalog_move(int delta) {
  bool moved = false;
  portENTER_CRITICAL(&g_runtime_lock);
  if (g_catalog_count > 0 && delta != 0) {
    size_t previous = g_catalog_selected;
    for (size_t attempt = 0; attempt < g_catalog_count; ++attempt) {
      g_catalog_selected = delta > 0 ? (g_catalog_selected + 1) % g_catalog_count
        : (g_catalog_selected + g_catalog_count - 1) % g_catalog_count;
      if (catalog_entry_visible(g_catalog_selected)) break;
    }
    moved = catalog_entry_visible(g_catalog_selected)
      && (previous != g_catalog_selected || g_catalog_count == 1);
  }
  portEXIT_CRITICAL(&g_runtime_lock);
  return moved;
}

bool pet_p4_miniapp_catalog_activate_selected(void) {
  portENTER_CRITICAL(&g_runtime_lock);
  size_t selected = g_catalog_selected;
  portEXIT_CRITICAL(&g_runtime_lock);
  return activate_catalog_index(selected);
}

bool pet_p4_miniapp_catalog_activate_id(const char *widget_id) {
  if (!widget_id || !widget_id[0]) return false;
  portENTER_CRITICAL(&g_runtime_lock);
  int index = catalog_find(g_catalog, g_catalog_count, widget_id);
  portEXIT_CRITICAL(&g_runtime_lock);
  return index >= 0 && activate_catalog_index((size_t) index);
}

static bool transfer_matches(const cJSON *payload) {
  return g_staging.active
    && strcmp(json_string(payload, "transferId"), g_staging.transfer_id) == 0;
}

bool pet_p4_miniapp_install_begin(const cJSON *payload, char *error, size_t error_size) {
  const char *transfer_id = json_string(payload, "transferId");
  const char *widget_id = json_string(payload, "widgetId");
  if (!g_builtin_sync_in_progress && strcmp(widget_id, "music-player") == 0) {
    set_error(error, error_size, "music player is unavailable in this release");
    return false;
  }
  memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
  if (!safe_id(transfer_id, sizeof(g_staging.transfer_id))
      || !safe_widget_id(widget_id)) {
    set_error(error, error_size, "invalid transferId or widgetId");
    return false;
  }
  g_staging.active = true;
  g_staging.repair_resources = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "repairResources"));
  if (g_staging.repair_resources && (pet_p4_audio_active() || pet_p4_audio_playback_active()
      || pet_p4_audio_conversation_active() || pet_p4_audio_music_active())) {
    memset(&g_staging, 0, sizeof(g_staging));
    set_error(error, error_size, "audio is active; resource repair deferred without changing data");
    return false;
  }
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
  } else if (strncmp(path,"runtime/images/",15)==0) {
    const char *name=path+15;size_t len=strlen(name);char id[16]={0};
    if(len<=4||len-4>=sizeof(id)||strcmp(name+len-4,".p4i")){set_error(error,error_size,"invalid image path");return false;}
    memcpy(id,name,len-4);
    if(!safe_id(id,sizeof(id))){set_error(error,error_size,"invalid image id");return false;}
    if(sprite_pack_find(&g_staging.sprites,id)>=0)return true;
    if(index==0 && (strcmp(id,g_image_id)||!g_image_next)){g_image_used=0;g_image_next=0;strcpy(g_image_id,id);}
    if(strcmp(id,g_image_id)){set_error(error,error_size,"image transfer interleaved");return false;}
    if((uint32_t)index+1==g_image_next)return true;
    if((uint32_t)index!=g_image_next){set_error(error,error_size,"image chunk sequence");return false;}
    size_t before=g_image_used;
    if(!decode_chunk(data,(char*)g_image_staging,sizeof(g_image_staging),&g_image_used,&decoded_len,false,error,error_size))return false;
    if(!verify_chunk_integrity(payload,g_image_staging+before,decoded_len,error,error_size)){g_image_used=before;return false;}
    if(g_image_used<12||(memcmp(g_image_staging,"P4I1",4)&&memcmp(g_image_staging,"P4I2",4)&&memcmp(g_image_staging,"P4J1",4))){g_image_used=before;set_error(error,error_size,"invalid image header");return false;}
    size_t expected=(uint32_t)g_image_staging[8]|(uint32_t)g_image_staging[9]<<8|(uint32_t)g_image_staging[10]<<16|(uint32_t)g_image_staging[11]<<24;
    if(expected<(g_image_staging[2]=='J'?16:PET_IMAGE_HEADER)||expected>PET_IMAGE_MAX_BYTES||g_image_used>expected){g_image_used=before;set_error(error,error_size,"image overflow");return false;}
    if(g_image_used==expected&&!append_sprite_file(&g_staging.sprites,id,g_image_staging,g_image_used,error,error_size)){g_image_used=before;return false;}
    g_image_next++;
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

static bool component_install_touch_allowed(bool requires_touch) {
  /* Firmware-owned preinstallation runs before LCD/I2C touch initialization.
   * It stores a verified package, not a claim that touch hardware is ready.
   * External PC installs and live slice input must still require ready touch. */
  return !requires_touch || g_builtin_sync_in_progress || pet_p4_touch_ready();
}

static bool resource_repair_package_matches(const miniapp_catalog_item_t *item,
                                          uint32_t widget_checksum, uint32_t images_checksum) {
  if (!item || !images_checksum) return false;
  if (item->resource_state == 2) return true;
  /* A damaged full package may be repaired only by its exact committed
   * payload, never silently upgraded/downgraded to the client's bundle. */
  return item->resource_state == 3 && item->widget_checksum == widget_checksum
    && item->sprites_checksum == images_checksum;
}

bool pet_p4_miniapp_install_commit(const cJSON *payload, char *error, size_t error_size) {
  g_storage_failure[0] = '\0';
  if (g_catalog_recovery_blocked) {
    set_error(error, error_size, "component directories need recovery; stored packages were preserved");
    return false;
  }
  const char *widget_id = json_string(payload, "widgetId");
  miniapp_runtime_t *next;
  miniapp_catalog_item_t *next_catalog __attribute__((cleanup(catalog_scratch_free))) = calloc(PET_P4_MINIAPP_CATALOG_MAX, sizeof(*next_catalog));
  if (!next_catalog) { set_error(error,error_size,"not enough memory for component catalog");return false; }
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
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
    return false;
  }
  next = (miniapp_runtime_t *) calloc(1, sizeof(*next));
  if (!next) {
    set_error(error, error_size, "not enough memory to activate mini-app");
    return false;
  }
  if (g_staging.repair_resources) {
    if (pet_p4_audio_active() || pet_p4_audio_playback_active()
        || pet_p4_audio_conversation_active() || pet_p4_audio_music_active()) {
      free(next);
      set_error(error, error_size, "audio started; resource repair deferred before flash writes");
      return false;
    }
    int repair_index = catalog_find(g_catalog, g_catalog_count, g_staging.widget_id);
    char old_buttons_path[40];
    if (repair_index < 0 || !g_staging.sprites.count
        || !resource_repair_package_matches(&g_catalog[repair_index],
            miniapp_checksum(g_staging.widget_json, g_staging.widget_len),
            sprite_pack_checksum(&g_staging.sprites))) {
      free(next);
      set_error(error, error_size, "resource repair requires bootstrap or exact committed damaged package; original data preserved");
      return false;
    }
    catalog_slot_path(g_catalog[repair_index].slot, g_catalog[repair_index].package_generation,
                     true, false, old_buttons_path, sizeof(old_buttons_path));
    if (!read_file(old_buttons_path, g_staging.buttons_json, sizeof(g_staging.buttons_json))
        || miniapp_checksum(g_staging.buttons_json, strlen(g_staging.buttons_json)) != g_catalog[repair_index].buttons_checksum) {
      free(next);
      set_error(error, error_size, "existing button configuration is damaged; automatic resource repair stopped");
      return false;
    }
    g_staging.buttons_len = strlen(g_staging.buttons_json);
  }
  if (!parse_runtime(next, g_staging.widget_id, g_staging.widget_json,
                     g_staging.buttons_json, &g_staging.sprites, error, error_size)) {
    free(next);
    memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
    return false;
  }
  memcpy(next_catalog, g_catalog, sizeof(g_catalog));
  if(!component_install_touch_allowed(next->slicing)) {
    set_error(error,error_size,"continuous touch is not ready; component was not installed");
    free(next);memset(&g_staging,0,sizeof(g_staging));return false;
  }
  next_count = g_catalog_count;
  int existing = catalog_find(next_catalog, next_count, g_staging.widget_id);
  if (existing >= 0 && next_catalog[existing].sprites_checksum
      && pet_p4_builtin_is_bootstrap(g_staging.widget_id,
           miniapp_checksum(g_staging.widget_json, g_staging.widget_len))) {
    free(next);
    memset(&g_staging, 0, sizeof(g_staging));
    g_image_used=0;g_image_next=0;g_image_id[0]=0;
    set_error(error, error_size, "refused image-free bootstrap over existing full resource package");
    return false;
  }
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
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
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
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
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
      || !persist_catalog_snapshot(next_catalog, next_count,
           g_staging.repair_resources && g_runtime.active ? g_runtime.widget_id : g_staging.widget_id)) {
    free(next);
    if (!error || !error[0]) {
      if (error && error_size) snprintf(error, error_size, "component commit failed: %s", g_storage_failure[0] ? g_storage_failure : "catalog allocation/validation");
    }
    memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
    return false;
  }
  save_app_runtime();restore_app_runtime(next);
  portENTER_CRITICAL(&g_runtime_lock);
  if (!g_staging.repair_resources || !g_runtime.active
      || strcmp(g_runtime.widget_id, g_staging.widget_id) == 0) {
    g_runtime = *next; g_image_revision++;
    g_catalog_selected = catalog_index;
  }
  memcpy(g_catalog, next_catalog, sizeof(g_catalog));
  g_catalog_count = next_count;
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
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
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
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
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
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
    set_error(error, error_size, "embedded built-in sprite list is invalid");
    return false;
  }
  const cJSON *sprite_file;
  cJSON_ArrayForEach(sprite_file, sprite_files) {
    const char *sprite_id = json_string(sprite_file, "id");
    const char *data = json_string(sprite_file, "data");
    size_t used=0,decoded=0;
    if (!cJSON_IsObject(sprite_file)
        || !safe_id(sprite_id, PET_P4_MINIAPP_SPRITE_ID_MAX)
        || !data[0]) {
      memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
      set_error(error, error_size, "embedded built-in sprite file is invalid");
      return false;
    }
    /* Firmware-owned base64 is already in memory. Decode into the bounded
     * staging buffer; accept P4S1 and native P4I1/P4J1 through one validator.
     * Do not turn a 100 KiB image into a synthetic oversized USB chunk. */
    if (!decode_chunk(data,(char*)g_image_staging,sizeof(g_image_staging),&used,&decoded,true,error,error_size)
        || !append_sprite_file(&g_staging.sprites,sprite_id,g_image_staging,used,error,error_size)) {
      memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
      if (!error || !error[0]) set_error(error, error_size, "failed to stage built-in sprite");
      return false;
    }
  }

  payload = cJSON_CreateObject();
  if (!payload
      || !cJSON_AddStringToObject(payload, "transferId", transfer_id)
      || !cJSON_AddStringToObject(payload, "widgetId", widget_id)) {
    cJSON_Delete(payload);
    memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
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
  if (index < 0 && g_catalog_count > 0) index = 0;
  index = catalog_visible_preferred(index);
  return index < 0 || activate_catalog_index((size_t) index);
}

static bool reorder_catalog_for_builtin_bundle(const cJSON *components) {
  miniapp_catalog_item_t *ordered __attribute__((cleanup(catalog_scratch_free))) = calloc(PET_P4_MINIAPP_CATALOG_MAX, sizeof(*ordered));
  if (!ordered) return false;
  bool used[PET_P4_MINIAPP_CATALOG_MAX] = {0};
  size_t ordered_count = 0;
  const cJSON *package;
  if (!cJSON_IsArray(components)) return false;

  // Bundle order is generated from the same factory-config.json used by the PC.
  // Move records, never slot contents, checksums, saved state or component ids.
  for (size_t index = 0; index < g_catalog_count; ++index) {
    bool builtin = false;
    cJSON_ArrayForEach(package, components) {
      if (strcmp(json_string(package, "id"), g_catalog[index].widget_id) == 0) {
        builtin = true;
        break;
      }
    }
    if (!builtin) {
      ordered[ordered_count++] = g_catalog[index];
      used[index] = true;
    }
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
  if (g_catalog_recovery_blocked) return ESP_ERR_INVALID_STATE;
  char marker[128] = {0};
  char expected_marker[128] = {0};
  char restore_widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  char error[160] = {0};
  cJSON *bundle = NULL;
  const cJSON *bundle_version;
  const cJSON *components;
  bool catalog_changed = false;
  size_t updated = 0;
  size_t failed = 0;

  /* Dirty builds may share an identity while their bundled components change. */
  snprintf(expected_marker, sizeof(expected_marker), "%s:%08lx", PET_P4_BUILD_ID,
    (unsigned long) miniapp_checksum((const char *) pet_p4_builtin_components_zlib,
      pet_p4_builtin_components_zlib_size));
  if (read_file(MINIAPP_BUILTIN_MARKER_PATH, marker, sizeof(marker))
      && strcmp(marker, expected_marker) == 0) {
    return ESP_OK;
  }
  /* Build-owned, bounded data only. Allocate once on migration, release before
   * loading packages; do not keep decompression scratch on the task stack. */
  if (!pet_p4_builtin_components_json_size || pet_p4_builtin_components_json_size > 160*1024)
    return ESP_ERR_INVALID_SIZE;
  char *json = malloc(pet_p4_builtin_components_json_size);
  if (!json) return ESP_ERR_NO_MEM;
  /* The ROM convenience wrapper puts ~11 KiB of Huffman state on stack.
   * app_main has only 10 KiB: use the low-level API with heap-owned state. */
  tinfl_decompressor *inflater = calloc(1, sizeof(*inflater));
  if (!inflater) { free(json); return ESP_ERR_NO_MEM; }
  tinfl_init(inflater);
  size_t consumed=pet_p4_builtin_components_zlib_size;
  size_t decoded=pet_p4_builtin_components_json_size;
  const tinfl_status inflated=tinfl_decompress(inflater,
    pet_p4_builtin_components_zlib, &consumed,
    (mz_uint8*)json, (mz_uint8*)json, &decoded,
    TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  free(inflater);
  if (inflated != TINFL_STATUS_DONE || consumed != pet_p4_builtin_components_zlib_size
      || decoded != pet_p4_builtin_components_json_size || json[decoded-1] != '\0') {
    free(json);
    return ESP_ERR_INVALID_RESPONSE;
  }
  bundle = cJSON_Parse(json);
  free(json);
  bundle_version = cJSON_GetObjectItemCaseSensitive(bundle, "version");
  components = cJSON_GetObjectItemCaseSensitive(bundle, "components");
  if (!cJSON_IsObject(bundle)
      || !cJSON_IsNumber(bundle_version)
      || bundle_version->valueint != 1
      || !cJSON_IsArray(components)
      || cJSON_GetArraySize(components) < 1
      || cJSON_GetArraySize(components) > PET_P4_MINIAPP_CATALOG_MAX) {
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
    /* Artwork-rich packages are upgraded by PC/factory resource transactions.
     * Never replace an installed complete package with the OTA bootstrap. */
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(package, "resourcePackage"))
        && catalog_find(g_catalog, g_catalog_count, widget_id) >= 0) continue;
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
      failed += 1;
      continue;
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
        failed += 1;
        continue;
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
  if (failed) {
    /* Keep successful additions; no completion marker means the failed ones
     * are retried on a later boot. Never hide all packages after one failure. */
    ESP_LOGW(TAG, "built-in sync incomplete failed=%u updated=%u catalog=%u",
      (unsigned int) failed, (unsigned int) updated, (unsigned int) g_catalog_count);
    cJSON_Delete(bundle);
    return ESP_FAIL;
  }
  if (!write_file_atomic(
        MINIAPP_BUILTIN_MARKER_TMP_PATH,
        MINIAPP_BUILTIN_MARKER_PATH,
        expected_marker,
        strlen(expected_marker)
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

static bool remove_component(const char *widget_id, char *error, size_t error_size, bool preserve_save) {
  char active_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  char next_active_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  miniapp_catalog_item_t *next_catalog __attribute__((cleanup(catalog_scratch_free))) = calloc(PET_P4_MINIAPP_CATALOG_MAX, sizeof(*next_catalog));
  if (!next_catalog) { set_error(error,error_size,"not enough memory for component catalog");return false; }
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
  memcpy(next_catalog, g_catalog, sizeof(g_catalog));
  next_count = g_catalog_count;
  next_selected = g_catalog_selected;
  portEXIT_CRITICAL(&g_runtime_lock);

  catalog_index = catalog_find(next_catalog, next_count, widget_id);
  if (catalog_index < 0) {
    memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
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
  if(removing_active&&next_runtime)restore_app_runtime(next_runtime);
  portENTER_CRITICAL(&g_runtime_lock);
  memcpy(g_catalog, next_catalog, sizeof(g_catalog));
  g_catalog_count = next_count;
  g_catalog_selected = next_selected;
  if (removing_active) {
    if (next_runtime) {g_runtime = *next_runtime;g_image_revision++;}
    else memset(&g_runtime, 0, sizeof(g_runtime));
  }
  memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
  portEXIT_CRITICAL(&g_runtime_lock);
  free(next_runtime);

  bool package_cleanup_ok = remove_file_if_present(MINIAPP_ID_PATH);
  if (!preserve_save) pet_app_store_forget(widget_id);
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
