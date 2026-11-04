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
#include <stdatomic.h>
#if PET_P4_CAPABILITY_EXT
#include "pet_p4_component_ext.h" /* types precede transition/runtime declarations */
#endif
#include "pet_p4_local_tools.h"
#if PET_P4_CELLS_V1
#include "pet_p4_cells.h"
#endif
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
#include "nvs.h"
#include "pet_p4_store_budget.h"
#include "pet_p4_file_read.h"
#include "pet_p4_file_exact.h"
#include "pet_p4_app_host.h"
#include "pet_p4_variable_owners.h"
#include "pet_p4_component_program_session.h"
#include "pet_p4_program_registry.h"
#if PET_COMPONENT_PROGRAM_ENABLED
#include "pet_p4_program_host_device.h"
#include "pet_p4_program_persistence.h"
#endif
#include "pet_p4_app_navigation.h"
#include "pet_p4_app_form_json.h"
#include "pet_p4_form_v2_json.h"
#if PET_FORM_V3_ENABLED
#include "pet_p4_form_v3_json.h"
#endif
#if PET_P4_DATA_ROWS
#include "pet_p4_data_rows.h"
#endif
#if PET_P4_NOTIFY
#include "pet_p4_notify.h"
#endif
#include "pet_p4_selection.h"
#include "pet_p4_widget_trace.h"
#include "pet_p4_file_cleanup.h"
#include "pet_p4_fs_activity.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_storage_io_trace.h"
#include "pet_p4_storage_cache_lease.h"
#include "pet_p4_native_work_window.h"
#if PET_P4_H264_PREFETCH
#include "pet_p4_renderer.h"
#endif
#if PET_P4_H264_PREFETCH || PET_P4_NATIVE_WORK_WINDOW
static bool g_program_slice_admitted=true; /* main parent, model-lock serialized */
#endif

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if PET_P4_SPARE_UPDATE
#include <unistd.h>
#endif

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

/* Array capacities are the p4-quota-v2 ceiling; packages that do not declare
 * quota keep the original v1 limits below (enforced at parse time). The runtime
 * struct lives in external RAM; growth is measured on device, not assumed. */
/* p4-quota-v2 ceilings: the array capacity unless p4-quota-v3 is compiled in. */
#define MINIAPP_V2_MAX_VARS 16
#define MINIAPP_V2_MAX_STATES 8
#define MINIAPP_V2_MAX_PAGES 6
#define MINIAPP_V2_MAX_TRANSITIONS 32
#define MINIAPP_V2_MAX_TICKS 12
#define MINIAPP_V2_MAX_CHOICES 8
#define MINIAPP_V2_SURFACE_NODES 48
#define MINIAPP_V2_LOCAL_ITEMS 24
#define MINIAPP_V2_SAVE_VARS 16
#define MINIAPP_V2_STRING_MAX 64
#define MINIAPP_V2_WIDGET_JSON_MAX 8192
/* p4-quota-v3 ceilings (PET_P4_CELLS/TEXT tracks' r150 capability parent): the
 * PSRAM runtime arrays grow to these only when PET_P4_QUOTA_V3 is on; v1/v2
 * packages keep their own numbers through MINIAPP_CAP at parse time. */
#define MINIAPP_V3_MAX_VARS 32
#define MINIAPP_V3_MAX_STATES 16
#define MINIAPP_V3_MAX_PAGES 12
#define MINIAPP_V3_MAX_TRANSITIONS 48
#define MINIAPP_V3_MAX_TICKS 16
#define MINIAPP_V3_MAX_CHOICES 12
#define MINIAPP_V3_SURFACE_NODES 64
#define MINIAPP_V3_LOCAL_ITEMS 64
#define MINIAPP_V3_SAVE_VARS 32
#define MINIAPP_V3_STRING_MAX 128
#define MINIAPP_V3_WIDGET_JSON_MAX 12288
#if PET_P4_QUOTA_V3
#define MINIAPP_MAX_VARS MINIAPP_V3_MAX_VARS
#define MINIAPP_MAX_STATES MINIAPP_V3_MAX_STATES
#define MINIAPP_MAX_PAGES MINIAPP_V3_MAX_PAGES
#define MINIAPP_MAX_TRANSITIONS MINIAPP_V3_MAX_TRANSITIONS
#define MINIAPP_MAX_TICKS MINIAPP_V3_MAX_TICKS
#define MINIAPP_MAX_CHOICES MINIAPP_V3_MAX_CHOICES
#define MINIAPP_STRING_MAX MINIAPP_V3_STRING_MAX
#define MINIAPP_WIDGET_JSON_MAX MINIAPP_V3_WIDGET_JSON_MAX
#else
#define MINIAPP_MAX_VARS MINIAPP_V2_MAX_VARS
#define MINIAPP_MAX_STATES MINIAPP_V2_MAX_STATES
#define MINIAPP_MAX_PAGES MINIAPP_V2_MAX_PAGES
#define MINIAPP_MAX_TRANSITIONS MINIAPP_V2_MAX_TRANSITIONS
#define MINIAPP_MAX_TICKS MINIAPP_V2_MAX_TICKS
#define MINIAPP_MAX_CHOICES MINIAPP_V2_MAX_CHOICES
#define MINIAPP_STRING_MAX MINIAPP_V2_STRING_MAX
#define MINIAPP_WIDGET_JSON_MAX MINIAPP_V2_WIDGET_JSON_MAX
#endif
/* Negotiated per-package ceiling: v1 default, v2 and v3 only when declared. */
#define MINIAPP_CAP(r,v1,v2,v3) ((r)->quota_v3?(v3):(r)->quota_v2?(v2):(v1))
#if PET_P4_GRAPHICS_V2
#define MINIAPP_GRAPHICS_V2(r) ((r)->graphics_v2)
#else
#define MINIAPP_GRAPHICS_V2(r) ((void)(r),false)
#endif
#define MINIAPP_MAX_EFFECTS 4
#define MINIAPP_MAX_BUTTONS 8
#define MINIAPP_MAX_CELL_OPS 4
#define MINIAPP_TEXT_MAX 64
#define MINIAPP_TEXT_ENTRY_MAX 120
#define MINIAPP_TEXT_POOL_MAX (4096+MINIAPP_TEXT_MAX)
#define MINIAPP_V1_MAX_VARS 8
#define MINIAPP_V1_MAX_STATES 6
#define MINIAPP_V1_MAX_PAGES 4
#define MINIAPP_V1_MAX_TRANSITIONS 24
#define MINIAPP_V1_MAX_TICKS 8
#define MINIAPP_V1_MAX_CHOICES 6
#define MINIAPP_V1_SURFACE_NODES 32
#define MINIAPP_V1_LOCAL_ITEMS 16
#define MINIAPP_V1_SAVE_VARS 8
#define MINIAPP_VAR_NAME_MAX 32
#define MINIAPP_VALUE_MAX 160
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
  char string_value[MINIAPP_STRING_MAX];
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
  char string_value[MINIAPP_STRING_MAX];
} miniapp_effect_t;

/* p4-logic-v1 guard: int var <op> literal-or-var. Legacy tick {var,lte} maps to
 * op LTE with a literal so older packages keep their exact behavior. */
#define MINIAPP_MAX_CONDITIONS 2
typedef struct {
  int8_t var;
  int8_t value_var;
  uint8_t op; /* pet_p4_game_cmp_t */
  int32_t value;
#if PET_P4_CELLS_V1
  /* p4-cells-v1: {"cell":{x,y}} reads one grid cell (literal or {var} coordinates) instead of var. */
  bool cell;
  int8_t cell_x_var,cell_y_var;
  int16_t cell_x,cell_y;
#endif
} miniapp_condition_t;
#if PET_P4_CELLS_V1
/* p4-cells-v1 bounded ops run after a branch's set/inc effects, in order:
 * cell set, fill, count -> var, longest run through (x,y) -> var. */
typedef enum { MINIAPP_CELL_SET, MINIAPP_CELL_FILL, MINIAPP_CELL_COUNT, MINIAPP_CELL_RUN } miniapp_cell_op_kind_t;
typedef struct {
  uint8_t kind;
  int8_t x_var,y_var,value_var,out_var; /* -1 = literal */
  int16_t x,y;
  int32_t value;
} miniapp_cell_op_t;
#endif

typedef struct {
  int8_t from_state;
  int8_t to_state;
  int8_t page_op;
  pet_nav_rule_t navigation_rule;
  char action[PET_P4_MINIAPP_ACTION_MAX];
  /* effects[0..effect_count) are the main branch; the optional p4-logic-v1
   * else branch follows in the same bounded array so memory does not grow. */
  uint8_t effect_count;
  miniapp_effect_t effects[MINIAPP_MAX_EFFECTS];
  uint8_t when_count;
  miniapp_condition_t when[MINIAPP_MAX_CONDITIONS];
  bool has_else;
  int8_t else_to_state;
  int8_t else_page_op;
  uint8_t else_effect_count;
  #if PET_P4_NOTIFY
  bool notify;
  #endif
  int8_t sfx_cue; /* p4-sfx-v1: cue index played when this transition executes, or -1 */
#if PET_P4_CELLS_V1
  /* Main branch ops first, the else branch's ops follow in the same bounded array. */
  uint8_t cell_op_count,else_cell_op_count;
  miniapp_cell_op_t cell_ops[MINIAPP_MAX_CELL_OPS];
#endif
#if PET_P4_CAPABILITY_EXT
  pet_cext_ops_t cext; /* Track C: sequence/sample/emit ops */
#endif
} miniapp_transition_t;

typedef struct {
  uint32_t every_ms;
  uint64_t last_ms;
  int8_t while_state;
  uint8_t effect_count;
  miniapp_effect_t effects[MINIAPP_MAX_EFFECTS];
  uint8_t when_count;
  miniapp_condition_t when[MINIAPP_MAX_CONDITIONS];
  int8_t completion_state;
  uint8_t completion_effect_count;
  miniapp_effect_t completion_effects[MINIAPP_MAX_EFFECTS];
  #if PET_P4_NOTIFY
  bool completion_notify;
  #endif
  int8_t completion_sfx_cue; /* p4-sfx-v1: cue played once when then fires, or -1 */
#if PET_P4_CELLS_V1
  uint8_t cell_op_count,completion_cell_op_count;
  miniapp_cell_op_t cell_ops[MINIAPP_MAX_CELL_OPS],completion_cell_ops[MINIAPP_MAX_CELL_OPS];
#endif
#if PET_P4_CAPABILITY_EXT
  pet_cext_ops_t completion_cext;
#endif
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
  char place_action[PET_P4_MINIAPP_ACTION_MAX],undo_action[PET_P4_MINIAPP_ACTION_MAX];
  bool absolute_directions;
  pet_p4_game_engine_t engine;
} miniapp_game_t;

typedef enum {
  MINIAPP_VALUE_LITERAL,
  MINIAPP_VALUE_VAR,
  MINIAPP_VALUE_MMSS,
  MINIAPP_VALUE_HMS,
  MINIAPP_VALUE_TEXT_AT, /* p4-texts-v1: text table entry selected by an int var */
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
  bool navigation;
  pet_nav_history_t navigation_history;
  pet_form_t form;
#if PET_FORM_V2_ENABLED
  pet_form_text_config_t form_text;
#endif
#if PET_FORM_V3_ENABLED
  pet_form3_t form3; /* p4-app-form-v3 keyboard forms (mutually exclusive with form/form_text) */
#endif
#if PET_P4_NOTIFY
  bool notice_enabled,notice_pending;
  int8_t notice_badge_var;
  pet_notify_t notice;
#endif
#if PET_P4_DATA_ROWS
  bool data_refresh_pending,data_refresh_used;
  uint64_t data_refresh_last;
#endif
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
#if PET_P4_DATA_ROWS
  pet_data_rows_config_t data_rows; /* p4-data-rows-v1 filters/select/refresh */
#endif
  bool media;
  char media_source[48];
#if PET_SERVICE_BRIDGE_ENABLED
  char host_contract[65];
#endif
  pet_surface_t surface;
  uint16_t save_version;
  uint32_t save_vars; /* one bit per declared var, covers the v3 ceiling */
  bool save_page,app_expressions,app_host;
  bool logic; /* p4-logic-v1 declared and accepted */
  bool quota_v2; /* p4-quota-v2 (or v3) declared and accepted: v2 array ceilings apply */
#if PET_P4_GRAPHICS_V2
  bool graphics_v2; /* p4-graphics-v2 declared and accepted: sprite/image/font ceilings of that profile apply */
#endif
  bool quota_v3; /* p4-quota-v3 declared and accepted: v3 ceilings apply */
#if PET_P4_TEXT_TABLE
  /* p4-texts-v1 package text table: NUL-separated UTF-8 pool, <=64 entries. */
  uint8_t text_count;
  uint16_t text_offset[MINIAPP_TEXT_MAX];
  char text_pool[MINIAPP_TEXT_POOL_MAX];
#endif
#if PET_P4_CELLS_V1
  pet_cells_t cells;
  bool cells_program_owned; /* program v2 mirrors the grid; transitions may only read it */
#endif
  bool sfx; uint8_t sfx_volume, sfx_count, sfx_pending; /* p4-sfx-v1 cues: index -> preset */
  uint8_t sfx_cue_preset[PET_SFX_MAX_CUES];
  char sfx_cue_id[PET_SFX_MAX_CUES][16];
#if PET_P4_CAPABILITY_EXT
  pet_cext_runtime_t cext; /* Track C capability expansion state */
#endif
#if PET_VARIABLE_OWNERS_ENABLED
  pet_variable_owners_t variable_owners;
#endif
#if PET_COMPONENT_PROGRAM_ENABLED
  pet_component_program_config_t program;
  pet_program_snapshot_t program_seed;
  bool program_seed_ready,program_needs_start,program_suspended;
#if PET_COMPONENT_BACKGROUND_ENABLED
  uint64_t program_content[3];
  bool program_background_owned;
#endif
#endif
  int8_t save_states[MINIAPP_MAX_STATES];
  bool instrument;
  pet_local_config_t local;
  pet_local_state_t local_state;
  int8_t local_vars[PET_LOCAL_VARS];
  bool slicing;
  pet_fruit_t fruit;
  uint8_t effect_volume, ambience_volume;
  bool side_enabled,side_axis_live;
  uint32_t side_last_step_us,side_max_step_us;
  uint64_t side_axis_until,side_jump_until;
  pet_side_scene_t side;
  pet_side_art_t side_art;
  char side_actions[4][48];
  int8_t side_states[4],side_vars[4];
} miniapp_runtime_t;
#include "pet_p4_miniapp_ext.h" /* Track C hooks; bodies in pet_p4_miniapp_ext.inc */

static bool runtime_host_variable(const miniapp_runtime_t *r,int index){
  if(!r->app_host||index<0)return false;
#if PET_VARIABLE_OWNERS_ENABLED
  if(r->variable_owners.mixed)return pet_variable_owners_host(r->variable_owners,index);
#endif
  return true;
}
static bool runtime_host_action(const miniapp_runtime_t *r,const char *action){
  if(!r->app_host)return false;
#if PET_VARIABLE_OWNERS_ENABLED
  if(r->variable_owners.mixed)return action&&!strncmp(action,"host.",5);
#endif
  (void)action;return true;
}
static bool runtime_host_dashboard(const miniapp_runtime_t *r){
#if PET_VARIABLE_OWNERS_ENABLED
  if(r->variable_owners.mixed)return false;
#endif
  return r->app_host;
}
static bool runtime_action_host_dependent(const miniapp_runtime_t *r,const char *action){
  if(runtime_host_action(r,action))return true;
#if PET_VARIABLE_OWNERS_ENABLED && PET_COMPONENT_PROGRAM_ENABLED
  if(r->variable_owners.mixed&&r->program.enabled&&action)
    for(unsigned i=0;i<r->program.action_count;i++)if(!strcmp(action,r->program.actions[i].name))
      for(unsigned k=0;k<r->program.actions[i].event.count;k++)
        if(runtime_host_variable(r,r->program.actions[i].input[k]))return true;
#endif
  return false;
}
/* Input acceptance, not eventual VM completion or a consumed stale gesture,
 * starts cooldown. Caller owns the native model lock. Never persisted. */
static void runtime_action_accepted(miniapp_runtime_t *r,const char *action,uint64_t now){
  for(int i=0;i<3;i++)if(r->cooldown_ms[i]&&!strcmp(r->cooldown_actions[i],action)){
    r->cooldown_used|=1u<<i;r->cooldown_at[i]=now;
  }
}

typedef struct {
  bool active;
  char transfer_id[64];
  bool repair_resources;
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
  bool preserve_unchanged;
#endif
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
  char selected_widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];
  bool seed_pending; /* missing old field means authoritative membership */
  miniapp_catalog_item_t items[PET_P4_MINIAPP_CATALOG_MAX];
  size_t count;
} miniapp_catalog_snapshot_t;

static const char *TAG = "pet-p4-miniapp";
static _Atomic bool g_catalog_recovery_blocked;
static char g_storage_failure[128];
static EXT_RAM_BSS_ATTR miniapp_runtime_t g_runtime;
#if PET_COMPONENT_PROGRAM_ENABLED
static EXT_RAM_BSS_ATTR pet_component_program_session_t g_component_program;
#if !PET_COMPONENT_PROGRAM_PARENT_UNIFIED
static EXT_RAM_BSS_ATTR pet_program_scheduler_t g_component_program_scheduler;
#endif
static EXT_RAM_BSS_ATTR pet_program_snapshot_t g_component_program_save;
static bool g_component_program_initialized;
static bool g_component_program_scheduler_initialized;
static uint32_t g_component_program_last_us,g_component_program_max_us;
static uint32_t g_component_program_attempt_revision;
static pet_program_runtime_result_t g_component_program_start_result;
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
static bool component_parent_enter(void);
static void component_parent_leave(bool *held);
#endif
/* Same lifecycle/model mutex as runtime publication, inputs and rendering.
 * These functions never initialize audio, open files or drain VM execution. */
static void component_program_retire_locked(void){
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
  bool held __attribute__((cleanup(component_parent_leave)))=component_parent_enter();if(!held)return;
#endif
  if(g_component_program_initialized&&(g_component_program.live||g_component_program.retiring))(void)pet_component_program_session_stop(&g_component_program);
}
static bool component_program_snapshot_locked(pet_program_snapshot_t *out){
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
  bool held __attribute__((cleanup(component_parent_leave)))=component_parent_enter();if(!held)return false;
#endif
  return pet_component_program_session_snapshot(&g_component_program,out);
}
#endif
static uint32_t g_image_revision=1;
#if PET_COMPONENT_BACKGROUND_ENABLED
static EXT_RAM_BSS_ATTR pet_program_registry_t g_background_programs;
static SemaphoreHandle_t g_background_mutex;
static bool g_background_ready;
static uint32_t g_background_last_us,g_background_max_us;
static bool background_init(void);
static bool background_current_locked(const miniapp_runtime_t *r,pet_program_registration_t *h);
static bool background_seed_locked(miniapp_runtime_t *r,bool *found);
static bool background_project_locked(miniapp_runtime_t *r);
static bool background_action_enabled_locked(const miniapp_runtime_t *r);
static bool background_status_current_locked(pet_p4_miniapp_program_status_t *out);
static bool background_enqueue_locked(const char *action,const int32_t base[8],bool *accepted);
static void background_process_locked(uint64_t now);
static bool background_checkpoint_one(void);
static bool background_prepare_replace(miniapp_runtime_t *next);
static bool background_prepare_remove(const char *id);
static uint32_t miniapp_checksum(const char *data,size_t len);
static uint32_t sprite_pack_checksum(const pet_p4_miniapp_sprite_pack_t *pack);
#endif
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
/* The model mutex is already held by every foreground caller. Registry-only
 * checkpoint/admission can run on the storage parent, so all accesses to its
 * shared scheduler also take this mutex, in model -> registry order. */
static bool component_parent_enter(void){
  return g_background_mutex&&xSemaphoreTake(g_background_mutex,portMAX_DELAY)==pdTRUE;
}
static void component_parent_leave(bool *held){if(*held)xSemaphoreGive(g_background_mutex);}
#endif
static int g_pressed_node=-1;
static uint64_t g_pressed_context;
static int32_t g_pressed_scroll[PET_SURFACE_VIEWPORTS+1];
#if PET_HOST_COLLECTION_ENABLED
static uint64_t g_pressed_collection_epoch;
#endif
/* Pointer-only generation: scrolling must not reset held joystick repeats. */
static uint32_t g_viewport_epoch;
static char g_confirm_action[48],g_confirm_prompt[121];
static char g_confirm_host_token[65];
#if PET_VARIABLE_OWNERS_ENABLED
static EXT_RAM_BSS_ATTR pet_host_observation_t g_confirm_host_observation;
#endif
static uint64_t g_confirm_until,g_confirm_context;
static uint8_t g_input_epoch;
static unsigned g_hold_owner;
static uint64_t g_hold_context;
static bool g_wall_valid;
static int64_t g_wall_seconds;
static uint64_t g_wall_at,g_utc_ms,g_clock_generation;
static portMUX_TYPE g_clock_lock = portMUX_INITIALIZER_UNLOCKED;
#if PET_P4_EVENTS
static pet_cev_clock_snapshot_t miniapp_utc_snapshot(uint64_t ms){
  pet_cev_clock_snapshot_t out={0};
  portENTER_CRITICAL(&g_clock_lock);
  out.monotonic_at_sync=g_wall_at;out.calibration_generation=g_clock_generation;
  out.valid=g_wall_valid&&g_clock_generation&&ms>=g_wall_at&&ms-g_wall_at<=UINT64_MAX-g_utc_ms;
  if(out.valid)out.utc_ms=g_utc_ms+(ms-g_wall_at);
  portEXIT_CRITICAL(&g_clock_lock);return out;
}
#endif
static int64_t local_wall_seconds(uint64_t ms){
  portENTER_CRITICAL(&g_clock_lock);
  int64_t value=g_wall_valid?g_wall_seconds+(int64_t)((ms>=g_wall_at?ms-g_wall_at:0)/1000):-1;
  portEXIT_CRITICAL(&g_clock_lock);return value;
}
bool pet_p4_miniapp_clock_sync(const cJSON *payload,uint64_t ms){
  const cJSON *time=cJSON_GetObjectItemCaseSensitive(payload,"unixSeconds"),*offset=cJSON_GetObjectItemCaseSensitive(payload,"utcOffsetMinutes");
  if(!cJSON_IsNumber(time)||time->valuedouble<1577836800.0||time->valuedouble>4102444799.0||floor(time->valuedouble)!=time->valuedouble||!cJSON_IsNumber(offset)||offset->valuedouble!=offset->valueint||offset->valueint< -840||offset->valueint>840)return false;
  portENTER_CRITICAL(&g_clock_lock);
  if(g_clock_generation==UINT64_MAX){portEXIT_CRITICAL(&g_clock_lock);return false;}
  g_clock_generation++;g_utc_ms=(uint64_t)time->valuedouble*1000U;
  g_wall_seconds=(int64_t)time->valuedouble+offset->valueint*60;g_wall_at=ms;g_wall_valid=true;portEXIT_CRITICAL(&g_clock_lock);return true;
}
static bool g_input_visible;
static EXT_RAM_BSS_ATTR miniapp_staging_t g_staging;
static EXT_RAM_BSS_ATTR uint8_t g_image_staging[PET_IMAGE_MAX_BYTES+1];
static size_t g_image_used;
static uint32_t g_image_next;
static char g_image_id[16];
static bool g_builtin_sync_in_progress;
/* Catalog absence is a durable membership tombstone after any user transaction.
 * Only an explicitly unfinished first-boot seed may add missing defaults. */
static bool g_catalog_seed_pending;
static pet_p4_miniapp_catalog_cursor_schedule_fn g_catalog_cursor_schedule;
static void *g_catalog_cursor_context;
static uint64_t g_catalog_cursor_serial;
static uint32_t g_catalog_cursor_sequence;
static bool g_catalog_cursor_dirty;
static uint64_t g_catalog_cursor_retry_ms;
static void catalog_cursor_retry(uint64_t now_ms);

static miniapp_catalog_item_t g_catalog[PET_P4_MINIAPP_CATALOG_MAX];
static size_t g_catalog_count;
static size_t g_catalog_selected;
static int g_catalog_file_generation = -1;
static uint32_t g_catalog_sequence;
/* Task-only runtime: publication/sprite snapshots can copy over 128 KiB and
 * must not mask interrupts. Keep the RTOS object in internal BSS (not PSRAM).
 * Lock order: state/render/media/diagnostics mutex -> this mutex -> bounded
 * clock/data/host/resource spinlocks. None of those spinlocks calls back here.
 * Initialization is explicit before any input/render/USB receiver starts;
 * never lazily create or silently bypass this lock on a bad call path. */
static StaticSemaphore_t g_runtime_mutex_storage;
static SemaphoreHandle_t g_runtime_mutex;
static bool miniapp_runtime_lock_init(void) {
  if(xPortInIsrContext() || !xPortCanYield())return false;
  if(!g_runtime_mutex)g_runtime_mutex=xSemaphoreCreateRecursiveMutexStatic(&g_runtime_mutex_storage);
  return g_runtime_mutex!=NULL;
}
static void miniapp_runtime_lock(void) {
  if(xPortInIsrContext() || !xPortCanYield() || !g_runtime_mutex
      || xSemaphoreTakeRecursive(g_runtime_mutex,portMAX_DELAY)!=pdTRUE)abort();
}
static void miniapp_runtime_unlock(void) {
  if(xPortInIsrContext() || !xPortCanYield() || !g_runtime_mutex
      || xSemaphoreGiveRecursive(g_runtime_mutex)!=pdTRUE)abort();
}
static pet_storage_lane_t g_storage_lane;
static pet_resource_owner_t g_storage_resource_owner;
static pet_resource_lease_t g_storage_resource_lease;
/* This slot owns only bounded metadata; package bytes are prepared by the
 * existing protocol worker. Cancellation never frees an in-flight job or
 * permits its physical package names to be reused. */
typedef struct {
  uint64_t serial;
  uint32_t catalog_sequence,image_revision;
  miniapp_catalog_item_t item;
  bool active,cancelled,published,running;
} miniapp_activation_t;
static miniapp_activation_t g_activation;
static pet_p4_miniapp_activation_schedule_fn g_activation_schedule;
static void *g_activation_schedule_context;

bool pet_p4_miniapp_storage_lane_configure(pet_storage_lock_fn unlock,
  pet_storage_lock_fn lock,pet_storage_owner_fn owner,void *context){
  return pet_storage_lane_configure(&g_storage_lane,unlock,lock,owner,context);
}
bool pet_p4_miniapp_storage_lane_enter(void){return pet_storage_lane_enter(&g_storage_lane);}
bool pet_p4_miniapp_storage_lane_leave(void){return pet_storage_lane_leave(&g_storage_lane);}
bool pet_p4_miniapp_storage_busy(void){return g_storage_lane.occupied||g_activation.active;}
static bool storage_work_current(void){
  return !g_storage_lane.occupied||pet_resources_device_current(g_storage_resource_lease);
}
#include "pet_p4_storage_cache_scope.inc"
static bool storage_scope_begin(pet_storage_scope_t *scope,bool activation_job){
  if(g_catalog_recovery_blocked||(g_activation.active&&!activation_job)||!pet_storage_scope_begin(&g_storage_lane,scope))return false;
  pet_resource_result_t opened=pet_resources_device_owner_open(
    PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&g_storage_resource_owner);
  pet_resource_result_t acquired=opened==PET_RESOURCE_OK
    ?pet_resources_device_acquire(g_storage_resource_owner,PET_RESOURCE_STORAGE_WORK,1,&g_storage_resource_lease)
    :PET_RESOURCE_UNAVAILABLE;
  if(opened==PET_RESOURCE_OK&&acquired==PET_RESOURCE_OK){
#if PET_P4_STORAGE_CACHE_SCOPE_ENABLED
    storage_cache_scope_begin(scope);
#endif
    return true;
  }
#if PET_P4_RESET_PHASE_TRACE
  ESP_LOGW(TAG,"P4 storage admission rejected open=%u acquire=%u",(unsigned)opened,(unsigned)acquired);
#endif
  pet_resources_device_revoke(g_storage_resource_owner);
  memset(&g_storage_resource_owner,0,sizeof(g_storage_resource_owner));
  (void)pet_storage_scope_finish(scope);return false;
}
static void storage_scope_finish(pet_storage_scope_t *scope){
  if(!scope->active)return;
  if(!pet_storage_scope_finish(scope)){
    g_catalog_recovery_blocked=true;
    ESP_LOGE(TAG,"storage ownership invariant failed; further catalog mutations blocked");
    return;
  }
#if PET_P4_STORAGE_CACHE_SCOPE_ENABLED
  storage_cache_scope_finish(scope);
#endif
  /* Only after physical I/O, allocator cleanup and state-lock reacquisition.
   * Revocation during audio retains occupancy until this exact completion. */
  (void)pet_resources_device_finish(g_storage_resource_lease);
  pet_resources_device_revoke(g_storage_resource_owner);
  memset(&g_storage_resource_lease,0,sizeof(g_storage_resource_lease));
  memset(&g_storage_resource_owner,0,sizeof(g_storage_resource_owner));
}
static bool storage_audio_busy(void){
  return pet_p4_audio_active()||pet_p4_audio_playback_active()
    ||pet_p4_audio_conversation_active()||pet_p4_audio_music_active()
    ||pet_p4_audio_stream_playing();
}

static const char *const g_retired_builtin_ids[] = {
  "falling-catch",
};

static bool remove_file_if_present(const char *path);
/* A single storage scope owns package names through durable publication and
 * cleanup, even while the shared state lock is released for flash I/O. No
 * queued cleanup may retain these names after that scope/slot reuse. The only other
 * SPIFFS writer is app_store; the common activity epoch also covers independent
 * readers whose VFS error handling can clear shared SPIFFS errno mid-scan.
 * A future filesystem actor must join that epoch or disable this shortcut. */
static bool cleanup_owner_current(void *context) {
  return *(const uint32_t *)context==g_catalog_sequence&&storage_work_current();
}
static bool cleanup_storage_idle(void *context,uint64_t *epoch) {
  (void)context;return pet_fs_activity_idle_epoch(epoch);
}
static pet_file_cleanup_guard_t cleanup_guard(uint32_t *sequence) {
  return (pet_file_cleanup_guard_t){.owner_current=cleanup_owner_current,
    .idle_epoch=cleanup_storage_idle,.context=sequence,.remove_path=remove_file_if_present};
}
static void free_bounded_scene_config(pet_p4_bounded_game_config_t **config) {
  free(*config);
}

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
#if PET_VARIABLE_OWNERS_ENABLED && PET_COMPONENT_PROGRAM_ENABLED
static bool component_program_action_values(const miniapp_runtime_t *r,const char *action,
    int32_t values[8],const pet_host_observation_t *observed,uint64_t now_ms);
#endif
static int32_t surface_integer(int index,uint64_t now);
static int32_t surface_scroll(const pet_surface_viewport_t *v,uint64_t now){return v&&v->enabled?pet_surface_clamp_offset(v,surface_integer(find_var(&g_runtime,v->offset_var),now)):0;}
static void surface_scroll_context(int32_t out[PET_SURFACE_VIEWPORTS+1],uint64_t now){
  out[0]=surface_scroll(&g_runtime.surface.viewport,now);
  for(int i=0;i<PET_SURFACE_VIEWPORTS;i++)out[i+1]=i<g_runtime.surface.viewport_count?surface_scroll(&g_runtime.surface.viewports[i],now):0;
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
    if (runtime->var_count >= MINIAPP_CAP(runtime, MINIAPP_V1_MAX_VARS, MINIAPP_V2_MAX_VARS, MINIAPP_MAX_VARS) || !safe_id(decl->string, MINIAPP_VAR_NAME_MAX)) {
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
      /* p4-quota-v3 raises string vars to 127 bytes; v1/v2 packages keep 63 even when the array holds 128. */
      if (init && (!cJSON_IsString(init) || strlen(init->valuestring) >= (runtime->quota_v3 ? sizeof(var->string_value) : (size_t) MINIAPP_V2_STRING_MAX))) {
        snprintf(error, error_size, "var %s string init must fit in %d UTF-8 bytes", decl->string, runtime->quota_v3 ? MINIAPP_V3_STRING_MAX - 1 : MINIAPP_V2_STRING_MAX - 1);
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
      || cJSON_GetArraySize(pages) > MINIAPP_CAP(runtime, MINIAPP_V1_MAX_PAGES, MINIAPP_V2_MAX_PAGES, MINIAPP_MAX_PAGES)) {
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
    if(((runtime->app_expressions&&op==MINIAPP_EFFECT_SET)||runtime->logic)&&cJSON_IsObject(item)&&cJSON_GetArraySize(item)==1){
      const char *source=json_string(item,"var");source_var=find_var(runtime,source);
      if(source_var<0||var->type!=MINIAPP_VAR_INT||runtime->vars[source_var].type!=MINIAPP_VAR_INT){set_error(error,error_size,"set/inc var reference must use integers");return false;}
    }
    if (op == MINIAPP_EFFECT_INC && (var->type != MINIAPP_VAR_INT || !(cJSON_IsNumber(item)||source_var>=0))) {
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
      if (strlen(item->valuestring) >= (runtime->quota_v3 ? sizeof(effect->string_value) : (size_t) MINIAPP_V2_STRING_MAX)) {
        set_error(error, error_size, "set string exceeds the negotiated string variable size");
        return false;
      }
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

static bool json_bounded_int(const cJSON *item, int32_t *out) {
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)
      || item->valuedouble < MINIAPP_INT_MIN || item->valuedouble > MINIAPP_INT_MAX
      || item->valuedouble != (double) ((long long) item->valuedouble)) return false;
  *out = (int32_t) item->valuedouble;
  return true;
}

#if PET_P4_CELLS_V1
/* A cell op/condition argument: a bounded literal within lo..hi or {"var": "<int var>"}. */
static bool parse_cell_arg(const miniapp_runtime_t *runtime, const cJSON *item, int8_t *var, int32_t *literal, int32_t lo, int32_t hi) {
  *var = -1; *literal = 0;
  if (cJSON_IsObject(item) && cJSON_GetArraySize(item) == 1) {
    int source = find_var(runtime, json_string(item, "var"));
    if (source < 0 || runtime->vars[source].type != MINIAPP_VAR_INT) return false;
    *var = (int8_t) source; return true;
  }
  return json_bounded_int(item, literal) && *literal >= lo && *literal <= hi;
}
/* "cells": [ {op:cell,x,y,set} | {op:cellfill,set} | {op:cellcount,value,var} | {op:cellrun,x,y,value,var} ] (<= 4). */
static bool parse_cell_ops(miniapp_runtime_t *runtime, const cJSON *array, miniapp_cell_op_t *ops, uint8_t *count, char *error, size_t error_size) {
  static const char *const names[] = {"cell", "cellfill", "cellcount", "cellrun"};
  static const char *const set_keys[] = {"op", "x", "y", "set"}, *const fill_keys[] = {"op", "set"};
  static const char *const count_keys[] = {"op", "value", "var"}, *const run_keys[] = {"op", "x", "y", "value", "var"};
  const cJSON *item;
  if (!array) return true;
  if (!runtime->cells.enabled || !cJSON_IsArray(array) || cJSON_GetArraySize(array) < 1) {
    set_error(error, error_size, "cells ops require a declared cells grid and a non-empty array");
    return false;
  }
  const pet_cells_t *grid = &runtime->cells;
  cJSON_ArrayForEach(item, array) {
    if (*count >= MINIAPP_MAX_CELL_OPS || !cJSON_IsObject(item)) { set_error(error, error_size, "too many or invalid cells ops"); return false; }
    miniapp_cell_op_t *op = &ops[(*count)];
    memset(op, 0, sizeof(*op)); op->x_var = op->y_var = op->value_var = op->out_var = -1;
    const char *name = json_string(item, "op"); int kind = -1;
    for (unsigned k = 0; k < 4; k++) if (!strcmp(name, names[k])) kind = (int) k;
    bool ok = kind >= 0;
    int32_t x = 0, y = 0, value = 0;
    if (kind == MINIAPP_CELL_SET || kind == MINIAPP_CELL_FILL) {
      ok = ok && !runtime->cells_program_owned && object_keys_allowed(item, kind == MINIAPP_CELL_SET ? set_keys : fill_keys, kind == MINIAPP_CELL_SET ? 4 : 2)
        && parse_cell_arg(runtime, cJSON_GetObjectItemCaseSensitive(item, "set"), &op->value_var, &value, 0, pet_cells_max_value(grid));
    } else {
      ok = ok && object_keys_allowed(item, kind == MINIAPP_CELL_COUNT ? count_keys : run_keys, kind == MINIAPP_CELL_COUNT ? 3 : 5)
        && parse_cell_arg(runtime, cJSON_GetObjectItemCaseSensitive(item, "value"), &op->value_var, &value, 0, pet_cells_max_value(grid));
      int out = ok ? find_var(runtime, json_string(item, "var")) : -1;
      ok = ok && out >= 0 && runtime->vars[out].type == MINIAPP_VAR_INT; op->out_var = (int8_t) out;
    }
    if (ok && (kind == MINIAPP_CELL_SET || kind == MINIAPP_CELL_RUN))
      ok = parse_cell_arg(runtime, cJSON_GetObjectItemCaseSensitive(item, "x"), &op->x_var, &x, 0, grid->cols - 1)
        && parse_cell_arg(runtime, cJSON_GetObjectItemCaseSensitive(item, "y"), &op->y_var, &y, 0, grid->rows - 1);
    if (!ok) {
      set_error(error, error_size, runtime->cells_program_owned && (kind == MINIAPP_CELL_SET || kind == MINIAPP_CELL_FILL)
        ? "the component program owns cell writes" : "invalid cells op");
      return false;
    }
    op->kind = (uint8_t) kind; op->x = (int16_t) x; op->y = (int16_t) y; op->value = value;
    (*count)++;
  }
  return true;
}
/* Runs after a branch's set/inc: literal or current var arguments, outputs clamped by limits. */
static bool apply_cell_ops(miniapp_runtime_t *runtime, const miniapp_cell_op_t *ops, int count) {
  bool changed = false;
  for (int i = 0; i < count; i++) {
    const miniapp_cell_op_t *op = &ops[i];
    int32_t x = op->x_var >= 0 ? runtime->vars[op->x_var].int_value : op->x;
    int32_t y = op->y_var >= 0 ? runtime->vars[op->y_var].int_value : op->y;
    int32_t value = op->value_var >= 0 ? runtime->vars[op->value_var].int_value : op->value;
    if (op->kind == MINIAPP_CELL_SET) changed |= pet_cells_set(&runtime->cells, x, y, value);
    else if (op->kind == MINIAPP_CELL_FILL) changed |= pet_cells_fill(&runtime->cells, value);
    else {
      miniapp_var_t *out = &runtime->vars[op->out_var];
      int32_t next = op->kind == MINIAPP_CELL_COUNT ? pet_cells_count(&runtime->cells, value) : pet_cells_run(&runtime->cells, x, y, value);
      if (next < out->int_min) next = out->int_min; if (next > out->int_max) next = out->int_max;
      if (next != out->int_value) { out->int_value = next; changed = true; }
    }
  }
  return changed;
}
#endif

static bool parse_condition_item(
  const miniapp_runtime_t *runtime,
  const cJSON *item,
  miniapp_condition_t *condition,
  bool legacy_only,
  char *error,
  size_t error_size
) {
  static const char *const allowed[] = {"var", "lte", "op", "value", "cell"};
  static const char *const ops[] = {"lte", "lt", "gte", "gt", "eq", "ne"};
  const cJSON *lte;
  const cJSON *value;
  const char *op_name;
  int var;
  if (!cJSON_IsObject(item) || !object_keys_allowed(item, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
    set_error(error, error_size, "condition contains an unsupported field");
    return false;
  }
  memset(condition, 0, sizeof(*condition));
  const cJSON *cell = cJSON_GetObjectItemCaseSensitive(item, "cell");
  if (cell) {
#if PET_P4_CELLS_V1
    /* p4-cells-v1: {"cell":{"x":..,"y":..},"op":..,"value":..} reads one grid cell; out of range reads -1. */
    static const char *const cell_keys[] = {"x", "y"};
    int32_t x = 0, y = 0;
    if (!runtime->cells.enabled || legacy_only || cJSON_HasObjectItem(item, "var") || cJSON_HasObjectItem(item, "lte")
        || !object_keys_allowed(cell, cell_keys, 2)
        || !parse_cell_arg(runtime, cJSON_GetObjectItemCaseSensitive(cell, "x"), &condition->cell_x_var, &x, 0, runtime->cells.cols - 1)
        || !parse_cell_arg(runtime, cJSON_GetObjectItemCaseSensitive(cell, "y"), &condition->cell_y_var, &y, 0, runtime->cells.rows - 1)) {
      set_error(error, error_size, "cell condition requires cells, logic and x/y in range or {var}");
      return false;
    }
    condition->cell = true; condition->cell_x = (int16_t) x; condition->cell_y = (int16_t) y; var = -1;
#else
    set_error(error, error_size, "cell conditions require the cells capability");
    return false;
#endif
  } else {
    var = find_var(runtime, json_string(item, "var"));
    if (var < 0 || runtime->vars[var].type != MINIAPP_VAR_INT) {
      set_error(error, error_size, "condition requires an int var");
      return false;
    }
  }
  condition->var = (int8_t) var;
  condition->value_var = -1;
  condition->op = PET_P4_GAME_CMP_LTE;
  lte = cJSON_GetObjectItemCaseSensitive(item, "lte");
  value = cJSON_GetObjectItemCaseSensitive(item, "value");
  op_name = json_string(item, "op");
  if (lte) {
    if (op_name[0] || value || !json_bounded_int(lte, &condition->value)) {
      set_error(error, error_size, "condition lte requires one bounded integer threshold");
      return false;
    }
    return true;
  }
  if (legacy_only) {
    set_error(error, error_size, "condition op/value requires logic p4-logic-v1");
    return false;
  }
  condition->op = 0xFF;
  for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i += 1) {
    if (strcmp(op_name, ops[i]) == 0) condition->op = (uint8_t) i;
  }
  if (condition->op == 0xFF) {
    set_error(error, error_size, "condition op must be lte/lt/gte/gt/eq/ne");
    return false;
  }
  if (cJSON_IsObject(value) && cJSON_GetArraySize(value) == 1) {
    int source = find_var(runtime, json_string(value, "var"));
    if (source < 0 || runtime->vars[source].type != MINIAPP_VAR_INT) {
      set_error(error, error_size, "condition value var must be an int var");
      return false;
    }
    condition->value_var = (int8_t) source;
    condition->value = 0;
    return true;
  }
  if (!json_bounded_int(value, &condition->value)) {
    set_error(error, error_size, "condition value must be a bounded integer or {var}");
    return false;
  }
  return true;
}

/* when: {var,lte} | {var,op,value} | {all:[c1,c2]}. legacy_only keeps the
 * pre-logic tick contract for packages that did not declare p4-logic-v1. */
static bool parse_condition(
  const miniapp_runtime_t *runtime,
  const cJSON *when,
  miniapp_condition_t *out,
  uint8_t *count,
  bool legacy_only,
  char *error,
  size_t error_size
) {
  const cJSON *all;
  const cJSON *item;
  *count = 0;
  if (!cJSON_IsObject(when)) {
    set_error(error, error_size, "when must be an object");
    return false;
  }
  all = cJSON_GetObjectItemCaseSensitive(when, "all");
  if (!all) {
    if (!parse_condition_item(runtime, when, &out[0], legacy_only, error, error_size)) return false;
    *count = 1;
    return true;
  }
  if (legacy_only || !cJSON_IsArray(all) || cJSON_GetArraySize(when) != 1
      || cJSON_GetArraySize(all) < 1 || cJSON_GetArraySize(all) > MINIAPP_MAX_CONDITIONS) {
    set_error(error, error_size, "when all requires logic p4-logic-v1 and 1-2 conditions");
    return false;
  }
  cJSON_ArrayForEach(item, all) {
    if (!parse_condition_item(runtime, item, &out[*count], false, error, error_size)) return false;
    *count += 1;
  }
  return true;
}

static bool conditions_hold(const miniapp_runtime_t *runtime, const miniapp_condition_t *conditions, int count) {
  for (int i = 0; i < count; i += 1) {
    const miniapp_condition_t *c = &conditions[i];
    int32_t right = c->value_var >= 0 ? runtime->vars[c->value_var].int_value : c->value;
    int32_t left;
#if PET_P4_CELLS_V1
    if (c->cell) left = pet_cells_get(&runtime->cells, c->cell_x_var >= 0 ? runtime->vars[c->cell_x_var].int_value : c->cell_x,
                                        c->cell_y_var >= 0 ? runtime->vars[c->cell_y_var].int_value : c->cell_y);
    else
#endif
    left = runtime->vars[c->var].int_value;
    if (!pet_p4_game_compare(left, c->op, right)) return false;
  }
  return true;
}

static int find_sfx_cue(const miniapp_runtime_t *runtime, const char *id) {
  if (!runtime->sfx || !id || !id[0]) return -1;
  for (int i = 0; i < runtime->sfx_count; i += 1) if (!strcmp(runtime->sfx_cue_id[i], id)) return i;
  return -1;
}

/* Optional "sfx": "<cue id>" on a transition or tick.then; requires the sfx profile. */
static bool parse_sfx_ref(const miniapp_runtime_t *runtime, const cJSON *object, int8_t *out, char *error, size_t error_size) {
  const cJSON *ref = cJSON_GetObjectItemCaseSensitive(object, "sfx");
  *out = -1;
  if (!ref) return true;
  int cue = cJSON_IsString(ref) ? find_sfx_cue(runtime, ref->valuestring) : -1;
  if (cue < 0) {
    set_error(error, error_size, runtime->sfx ? "sfx references an unknown cue" : "sfx requires the p4-sfx-v1 profile");
    return false;
  }
  *out = (int8_t) cue;
  return true;
}

static bool parse_page_op(miniapp_runtime_t *runtime, const char *page, int8_t *op) {
  *op = -1;
  if (!page[0]) return true;
  if (strcmp(page, "next") == 0) *op = -2;
  else if (strcmp(page, "prev") == 0) *op = -3;
  else *op = (int8_t) find_name(runtime->pages, runtime->page_count, page);
  return *op != -1;
}

#include "pet_p4_miniapp_d.inc"

static bool parse_transitions(
  miniapp_runtime_t *runtime,
  const cJSON *array,
  char *error,
  size_t error_size
) {
  const cJSON *item;
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > MINIAPP_CAP(runtime, MINIAPP_V1_MAX_TRANSITIONS, MINIAPP_V2_MAX_TRANSITIONS, MINIAPP_MAX_TRANSITIONS)) {
    set_error(error, error_size, "transitions must be a bounded array");
    return false;
  }
  cJSON_ArrayForEach(item, array) {
    static const char *const allowed[] = {"from", "on", "to", "set", "inc", "page", "from_page", "when", "else", "sfx", "cells"
#if PET_P4_NOTIFY
      , "notify"
#endif
#if PET_P4_CAPABILITY_EXT
      , PET_CEXT_OP_KEYS
#endif
    };
    const char *from;
    const char *action;
    const char *to;
    const char *page;
    const cJSON *when;
    const cJSON *otherwise;
    miniapp_transition_t *transition = &runtime->transitions[runtime->transition_count++];
    if (!object_keys_allowed(item, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
      set_error(error, error_size, "transition contains an unsupported field");
      return false;
    }
    when = cJSON_GetObjectItemCaseSensitive(item, "when");
    otherwise = cJSON_GetObjectItemCaseSensitive(item, "else");
    transition->else_to_state = -1;
    transition->else_page_op = -1;
    if ((when || otherwise) && !runtime->logic) {
      set_error(error, error_size, "transition when/else requires logic p4-logic-v1");
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
    if(!pet_nav_parse_rule(runtime->navigation,runtime->pages,runtime->page_count,item,&transition->navigation_rule)){
      set_error(error,error_size,"invalid application navigation rule");return false;
    }
    if (!(safe_id(action, sizeof(transition->action)) MINIAPP_EXT_OR_EVENT_ACTION(action))
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
    if (when && !parse_condition(runtime, when, transition->when, &transition->when_count, false, error, error_size)) {
      return false;
    }
    if (!parse_sfx_ref(runtime, item, &transition->sfx_cue, error, error_size)) return false;
#if PET_P4_NOTIFY
    if(!miniapp_parse_notify_rule(runtime,item,&transition->notify)){set_error(error,error_size,"notify requires runtime.notify and literal true");return false;}
#endif
#if PET_P4_CAPABILITY_EXT
    if (!pet_cext_parse_ops(&runtime->cext, item, &transition->cext, error, error_size)) return false;
    if (!pet_cext_add_subscription(&runtime->cext, action)) { set_error(error, error_size, "event subscription requires events p4-events-v1 and at most 8 safe names"); return false; }
#endif
#if PET_P4_CELLS_V1
    if (!parse_cell_ops(runtime, cJSON_GetObjectItemCaseSensitive(item, "cells"), transition->cell_ops, &transition->cell_op_count, error, error_size)) return false;
#else
    if (cJSON_HasObjectItem(item, "cells")) { set_error(error, error_size, "cells ops require the cells capability"); return false; }
#endif
    if (otherwise) {
      static const char *const else_allowed[] = {"to", "page", "set", "inc", "cells"};
      uint8_t total = transition->effect_count;
      const char *else_to;
      if (!when || !cJSON_IsObject(otherwise)
          || !object_keys_allowed(otherwise, else_allowed, sizeof(else_allowed) / sizeof(else_allowed[0]))) {
        set_error(error, error_size, "transition else requires when and only to/page/set/inc");
        return false;
      }
      else_to = json_string(otherwise, "to");
      transition->else_to_state = else_to[0]
        ? (int8_t) find_name(runtime->states, runtime->state_count, else_to) : -1;
      if ((else_to[0] && transition->else_to_state < 0)
          || !parse_page_op(runtime, json_string(otherwise, "page"), &transition->else_page_op)) {
        set_error(error, error_size, "transition else references an invalid state or page");
        return false;
      }
      /* The else branch shares the bounded 4-effect array with the main branch. */
      if (!parse_effects(runtime, otherwise, transition->effects, &total, error, error_size)) return false;
      transition->else_effect_count = (uint8_t) (total - transition->effect_count);
      transition->has_else = true;
      bool else_cells = false;
#if PET_P4_CELLS_V1
      uint8_t cell_total = transition->cell_op_count;
      if (!parse_cell_ops(runtime, cJSON_GetObjectItemCaseSensitive(otherwise, "cells"), transition->cell_ops, &cell_total, error, error_size)) return false;
      transition->else_cell_op_count = (uint8_t) (cell_total - transition->cell_op_count);
      else_cells = transition->else_cell_op_count > 0;
#else
      if (cJSON_HasObjectItem(otherwise, "cells")) { set_error(error, error_size, "cells ops require the cells capability"); return false; }
#endif
      if (transition->else_to_state < 0 && transition->else_page_op == -1 && transition->else_effect_count == 0 && !else_cells) {
        set_error(error, error_size, "transition else must change state, page or variables");
        return false;
      }
    }
  }
  return true;
}

static bool parse_ticks(miniapp_runtime_t *runtime, const cJSON *array, char *error, size_t error_size) {
  const cJSON *item;
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > MINIAPP_CAP(runtime, MINIAPP_V1_MAX_TICKS, MINIAPP_V2_MAX_TICKS, MINIAPP_MAX_TICKS)) {
    set_error(error, error_size, "tick must be a bounded array");
    return false;
  }
  cJSON_ArrayForEach(item, array) {
    static const char *const allowed[] = {
      "every_ms", "while_state", "set", "inc", "when", "then", "cells",
    };
    static const char *const then_allowed[] = {"to", "set", "inc", "sfx", "cells"
#if PET_P4_NOTIFY
      , "notify"
#endif
#if PET_P4_CAPABILITY_EXT
      , PET_CEXT_OP_KEYS
#endif
    };
    const cJSON *every;
    const cJSON *when;
    const cJSON *then;
    const char *while_state;
    miniapp_tick_t *tick = &runtime->ticks[runtime->tick_count++];
    tick->when_count = 0;
    tick->completion_state = -1;
    tick->completion_sfx_cue = -1;
#if PET_P4_CAPABILITY_EXT
    tick->completion_cext = (pet_cext_ops_t) {-1, -1, 0};
#endif
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
#if PET_P4_CELLS_V1
    if (!parse_cell_ops(runtime, cJSON_GetObjectItemCaseSensitive(item, "cells"), tick->cell_ops, &tick->cell_op_count, error, error_size)) return false;
#else
    if (cJSON_HasObjectItem(item, "cells")) { set_error(error, error_size, "cells ops require the cells capability"); return false; }
#endif
    when = cJSON_GetObjectItemCaseSensitive(item, "when");
    then = cJSON_GetObjectItemCaseSensitive(item, "then");
    if ((when && !then) || (!when && then) || (when && (!cJSON_IsObject(when) || !cJSON_IsObject(then)))) {
      set_error(error, error_size, "tick when and then must be paired objects");
      return false;
    }
    if (when) {
      const char *completion_state = json_string(then, "to");
      if (!object_keys_allowed(then, then_allowed, sizeof(then_allowed) / sizeof(then_allowed[0]))) {
        set_error(error, error_size, "tick completion contains an unsupported field");
        return false;
      }
      if (!parse_condition(runtime, when, tick->when, &tick->when_count, !runtime->logic, error, error_size)) {
        return false;
      }
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
      if (!parse_sfx_ref(runtime, then, &tick->completion_sfx_cue, error, error_size)) return false;
#if PET_P4_NOTIFY
      if(!miniapp_parse_notify_rule(runtime,then,&tick->completion_notify)){set_error(error,error_size,"notify requires runtime.notify and literal true");return false;}
#endif
#if PET_P4_CAPABILITY_EXT
      if (!pet_cext_parse_ops(&runtime->cext, then, &tick->completion_cext, error, error_size)) return false;
#endif
      bool then_cells = false;
#if PET_P4_CELLS_V1
      if (!parse_cell_ops(runtime, cJSON_GetObjectItemCaseSensitive(then, "cells"), tick->completion_cell_ops, &tick->completion_cell_op_count, error, error_size)) return false;
      then_cells = tick->completion_cell_op_count > 0;
#else
      if (cJSON_HasObjectItem(then, "cells")) { set_error(error, error_size, "cells ops require the cells capability"); return false; }
#endif
      if (tick->completion_state < 0 && tick->completion_effect_count == 0 && !then_cells
#if PET_P4_NOTIFY
          && !tick->completion_notify
#endif
      ) {
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
#if PET_P4_TEXT_TABLE
  else if ((name = json_string(item, "text_at"))[0]) {
    if (!runtime->text_count) { set_error(error, error_size, "text_at requires a package texts table"); return false; }
    value->kind = MINIAPP_VALUE_TEXT_AT;
  }
#endif
  else {
    set_error(error, error_size, "dashboard value rule is unsupported");
    return false;
  }
  value->var_index = (int8_t) find_var(runtime, name);
  if (value->var_index < 0) {
    set_error(error, error_size, "dashboard value references an unknown var");
    return false;
  }
  if ((value->kind == MINIAPP_VALUE_MMSS || value->kind == MINIAPP_VALUE_HMS || value->kind == MINIAPP_VALUE_TEXT_AT)
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
    if (rule->choice_count >= MINIAPP_CAP(runtime, MINIAPP_V1_MAX_CHOICES, MINIAPP_V2_MAX_CHOICES, MINIAPP_MAX_CHOICES)) {
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
  root = cJSON_ParseWithOpts(buttons_json,NULL,true);
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

static void form_values(const miniapp_runtime_t *r,int32_t values[PET_FORM_VARS]){
  for(int i=0;i<PET_FORM_VARS;i++)values[i]=i<r->var_count?r->vars[i].int_value:0;
}
#if PET_FORM_V2_ENABLED
static void form2_values(const miniapp_runtime_t *r,pet_form_value_t values[PET_FORM_VARS]){
  memset(values,0,sizeof(pet_form_value_t)*PET_FORM_VARS);
  if(r->var_count>PET_FORM_VARS)return; /* fail closed if a bad runtime bypassed import */
  for(int i=0;i<r->var_count;i++){
    if(r->vars[i].type==MINIAPP_VAR_INT)values[i].number=r->vars[i].int_value;
    else copy_utf8(values[i].text,sizeof(values[i].text),r->vars[i].string_value);
  }
}
static bool form2_write(miniapp_runtime_t *r,const pet_form_value_t values[PET_FORM_VARS]){
  if(r->var_count>PET_FORM_VARS)return false;
  bool changed=false;
  for(int i=0;i<r->var_count;i++){
    if(r->vars[i].type==MINIAPP_VAR_INT){if(r->vars[i].int_value!=values[i].number){r->vars[i].int_value=values[i].number;changed=true;}}
    else if(strcmp(r->vars[i].string_value,values[i].text)){copy_utf8(r->vars[i].string_value,sizeof(r->vars[i].string_value),values[i].text);changed=true;}
  }
  if(changed){g_viewport_epoch++;g_pressed_node=-1;}return changed;
}
#endif
#if PET_FORM_V3_ENABLED
/* Only these roles belong to the bounded editor. Unrelated quota-v3 strings
 * never pass through its 64-byte intermediate values or writeback. */
/* Recursive model lock also serializes preparation of unpublished candidates. */
static EXT_RAM_BSS_ATTR pet_form3_value_t g_form3_workspace[PET_FORM3_VARS];
static pet_form3_value_t *form3_workspace_begin(void){miniapp_runtime_lock();return g_form3_workspace;}
static void form3_workspace_end(void){miniapp_runtime_unlock();}
static bool form3_owns(const miniapp_runtime_t *r,unsigned at){
  const pet_form3_t *f=&r->form3;if(!f->enabled)return false;
  if(at==f->focus_var||at==f->active_var||at==f->key_var||at==f->cursor_var||at==f->buffer_var||at==f->label_var||at==f->candidates_var)return true;
  for(unsigned i=0;i<f->count;i++)if(at==f->fields[i].var||at==f->fields[i].draft_var)return true;
  return false;
}
static unsigned form3_string_limit(const miniapp_runtime_t *r,unsigned at){
  if(!form3_owns(r,at))return 0;
  for(unsigned i=0;i<r->form3.count;i++)if(at==r->form3.fields[i].var||at==r->form3.fields[i].draft_var)return r->form3.fields[i].max_bytes;
  return at==r->form3.buffer_var?PET_FORM3_BUFFER_DIGITS:63;
}
static void form3_values(const miniapp_runtime_t *r,pet_form3_value_t values[PET_FORM3_VARS]){
  memset(values,0,sizeof(pet_form3_value_t)*PET_FORM3_VARS);
  for(int i=0;i<r->var_count&&i<PET_FORM3_VARS;i++){
    if(!form3_owns(r,(unsigned)i))continue;
    if(r->vars[i].type==MINIAPP_VAR_INT)values[i].number=r->vars[i].int_value;
    else copy_utf8(values[i].text,sizeof(values[i].text),r->vars[i].string_value);
  }
}
static bool form3_write(miniapp_runtime_t *r,const pet_form3_value_t values[PET_FORM3_VARS]){
  bool changed=false;
  for(int i=0;i<r->var_count&&i<PET_FORM3_VARS;i++){
    if(!form3_owns(r,(unsigned)i))continue;
    if(r->vars[i].type==MINIAPP_VAR_INT){if(r->vars[i].int_value!=values[i].number){r->vars[i].int_value=values[i].number;changed=true;}}
    else if(strcmp(r->vars[i].string_value,values[i].text)){copy_utf8(r->vars[i].string_value,sizeof(r->vars[i].string_value),values[i].text);changed=true;}
  }
  if(changed){g_viewport_epoch++;g_pressed_node=-1;}return changed;
}
#endif
static bool form_write(miniapp_runtime_t *r,const int32_t values[PET_FORM_VARS]){
  if(r->var_count>PET_FORM_VARS)return false;
  bool changed=false;
  for(int i=0;i<r->var_count;i++)if(r->vars[i].type==MINIAPP_VAR_INT&&r->vars[i].int_value!=values[i]){r->vars[i].int_value=values[i];changed=true;}
  if(changed){g_viewport_epoch++;g_pressed_node=-1;}
  return changed;
}
static bool form_cancel(miniapp_runtime_t *r){
#if PET_FORM_V3_ENABLED
  if(r->form3.enabled){pet_form3_value_t *values=form3_workspace_begin();form3_values(r,values);pet_form3_cancel(&r->form3,values);bool changed=form3_write(r,values);form3_workspace_end();return changed;}
#endif
  if(!r->form.enabled)return false;
#if PET_FORM_V2_ENABLED
  if(r->form_text.enabled){pet_form_value_t values[PET_FORM_VARS];form2_values(r,values);pet_form2_cancel(&r->form,&r->form_text,values);return form2_write(r,values);}
#endif
  int32_t values[PET_FORM_VARS];form_values(r,values);pet_form_cancel(&r->form,values);return form_write(r,values);
}
static int form_operation(const miniapp_runtime_t *r,const char *action){
#if PET_FORM_V3_ENABLED
  if(r->form3.enabled)return pet_form3_operation(&r->form3,action);
#endif
#if PET_FORM_V2_ENABLED
  if(r->form_text.enabled)return pet_form2_operation(&r->form,&r->form_text,action);
#endif
  return pet_form_operation(&r->form,action);
}
/* Keep the early admission clock/strcmp temporaries out of the much larger
 * form availability frame. Both mutually exclusive call paths are audited. */
static bool runtime_action_cooled(const miniapp_runtime_t *r,const char *action) __attribute__((noinline));
static bool runtime_action_cooled(const miniapp_runtime_t *r,const char *action){
  uint64_t now=esp_timer_get_time()/1000ULL;
  for(int i=0;i<3;i++)if(r->cooldown_ms[i]&&(r->cooldown_used&(1u<<i))&&!strcmp(r->cooldown_actions[i],action)
      &&(now<r->cooldown_at[i]||now-r->cooldown_at[i]<r->cooldown_ms[i]))return false;
  return true;
}
/* Admission and dispatch must select the same guarded branch. from_page is
 * shared by both branches; only the selected main branch owns its navigation. */
static bool runtime_transition_branch(const miniapp_runtime_t *r,const miniapp_transition_t *t,const char *action,bool *taken) {
  if(strcmp(t->action,action)||(t->from_state>=0&&t->from_state!=r->current_state))return false;
  if(r->navigation&&t->navigation_rule.from_page>=0&&t->navigation_rule.from_page!=r->current_page)return false;
  *taken=t->when_count==0||conditions_hold(r,t->when,t->when_count);
  if(!*taken&&!t->has_else)return false;
  if(r->navigation){
    pet_nav_rule_t rule=t->navigation_rule;
    if(!*taken){rule.op=PET_NAV_NONE;rule.target=-1;}
    if(!pet_nav_allowed(&r->navigation_history,r->current_page,&rule))return false;
  }
  return true;
}
static bool runtime_action_enabled(const miniapp_runtime_t *r,const char *action) {
  if(!r->input_v1)return true; /* Legacy packages keep their original contract. */
  if(!runtime_action_cooled(r,action))return false;
#if PET_COMPONENT_PROGRAM_ENABLED
  if(r->program.enabled&&r->program_suspended)return false;
  if(r==&g_runtime&&r->program.enabled)for(unsigned i=0;i<r->program.action_count;i++)if(!strcmp(r->program.actions[i].name,action)){
#if PET_VARIABLE_OWNERS_ENABLED
    if(runtime_action_host_dependent(r,action)){
      int32_t values[8];if(!component_program_action_values(r,action,values,NULL,esp_timer_get_time()/1000ULL))return false;
    }
#endif
#if PET_COMPONENT_BACKGROUND_ENABLED
    pet_program_registration_t h={0};if(background_current_locked(r,&h))return background_action_enabled_locked(r);
#endif
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
    bool held __attribute__((cleanup(component_parent_leave)))=component_parent_enter();if(!held)return false;
#endif
    return g_component_program_initialized&&pet_component_program_session_can_enqueue(&g_component_program,g_image_revision);
  }
#endif
  int form_op=form_operation(r,action);
#if PET_FORM_V3_ENABLED
  if(form_op>=0&&r->form3.enabled){pet_form3_value_t *values=form3_workspace_begin();form3_values(r,values);bool allowed=pet_form3_allowed(&r->form3,values,r->current_page,form_op);form3_workspace_end();return allowed;}
#endif
#if PET_FORM_V2_ENABLED
  if(form_op>=0&&r->form_text.enabled){pet_form_value_t values[PET_FORM_VARS];form2_values(r,values);return pet_form2_allowed(&r->form,&r->form_text,values,r->current_page,form_op);}
#endif
  if(form_op>=0){int32_t values[PET_FORM_VARS];form_values(r,values);return pet_form_allowed(&r->form,values,r->current_page,form_op);}
  if(r->local.enabled&&!pet_local_enabled(&r->local,&r->local_state,action))return false;
#if PET_P4_DATA_ROWS
  if(!r->data_rows.enabled)pet_p4_widget_data_set_filter(NULL,NULL,NULL);
#endif
  for(int i=0;i<r->transition_count;i++){
    const miniapp_transition_t *t=&r->transitions[i];
    bool taken;
    if(runtime_transition_branch(r,t,action,&taken)){
      const int to_state=taken?t->to_state:t->else_to_state;
      const int page_op=taken?t->page_op:t->else_page_op;
      const int effect_count=taken?t->effect_count:t->else_effect_count;
      const miniapp_effect_t *effects=t->effects+(taken?0:t->effect_count);
      if(r->data_source[0]&&to_state<0&&page_op<0&&effect_count==1&&effects[0].var_index==r->data_page_var){
        pet_p4_data_paging_t data=pet_p4_widget_data_paging(r->data_source,r->vars[r->data_page_var].int_value);
        const miniapp_effect_t *e=&effects[0];int64_t value=e->source_var>=0?r->vars[e->source_var].int_value:e->int_value;
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
    bool side=!strcmp(json_string(scene,"engine"),"p4-side-scene-v1");
    if(!cJSON_IsString(hold)||!runtime_has_transition_action(r,hold->valuestring)||(!side&&!cJSON_GetObjectItemCaseSensitive(scene,"motion")))return false;
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
  static const char *const gomoku_action_allowed[] = {
    "place", "left", "right", "up", "down", "undo",
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
    : kind == PET_P4_GAME_SNAKE ? snake_action_allowed
    : kind == PET_P4_GAME_GOMOKU ? gomoku_action_allowed : flappy_action_allowed;
  size_t action_allowed_count = kind == PET_P4_GAME_BLOCKS
    ? sizeof(blocks_action_allowed) / sizeof(blocks_action_allowed[0])
    : kind == PET_P4_GAME_SNAKE
      ? sizeof(snake_action_allowed) / sizeof(snake_action_allowed[0])
    : kind == PET_P4_GAME_GOMOKU
      ? sizeof(gomoku_action_allowed) / sizeof(gomoku_action_allowed[0])
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
  copy_utf8(runtime->game.place_action,sizeof(runtime->game.place_action),json_string(actions,"place"));
  copy_utf8(runtime->game.undo_action,sizeof(runtime->game.undo_action),json_string(actions,"undo"));
  const cJSON *mode=cJSON_GetObjectItemCaseSensitive(game,"direction_mode");
  if(mode){
    if(!runtime->input_v1||kind!=PET_P4_GAME_SNAKE||!cJSON_IsString(mode)||strcmp(mode->valuestring,"absolute"))return false;
    runtime->game.absolute_directions=true;
  }
  /* gomoku moves a board cursor, so up/down are part of its fixed action table. */
  if((runtime->game.up_action[0]||runtime->game.down_action[0])&&!runtime->game.absolute_directions&&kind!=PET_P4_GAME_GOMOKU)return false;
  if(runtime->game.absolute_directions&&(!runtime->game.up_action[0]||!runtime->game.down_action[0]))return false;
  if(cJSON_GetObjectItemCaseSensitive(actions,"soft_drop")&&(!runtime->input_v1||!runtime->game.soft_drop_action[0]))return false;
  const char *extras[]={runtime->game.soft_drop_action,runtime->game.up_action,runtime->game.down_action};
  for(int i=0;i<3;i++)if(extras[i][0]&&(!safe_id(extras[i],PET_P4_MINIAPP_ACTION_MAX)||!runtime_has_transition_action(runtime,extras[i])||!runtime_has_button_action(runtime,extras[i])))return false;
  if (kind == PET_P4_GAME_FLAPPY) {
    required_actions[0] = runtime->game.flap_action;
    required_count = 1;
  } else if (kind == PET_P4_GAME_GOMOKU) {
    /* place doubles as start/restart (flappy convention); up/down are checked
     * with the extras above. */
    if(!runtime->game.up_action[0]||!runtime->game.down_action[0])return false;
    required_actions[0] = runtime->game.place_action;
    required_actions[1] = runtime->game.left_action;
    required_actions[2] = runtime->game.right_action;
    required_actions[3] = runtime->game.undo_action;
    required_count = 4;
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

#if PET_P4_TEXT_TABLE
/* p4-texts-v1: index outside the table resolves to the empty string. */
static const char *text_at(const miniapp_runtime_t *runtime, int32_t index) {
  if (index < 0 || index >= runtime->text_count) return "";
  return runtime->text_pool + runtime->text_offset[index];
}
static bool parse_texts(miniapp_runtime_t *runtime, const cJSON *texts) {
  const cJSON *item; size_t used = 0;
  if (!cJSON_IsArray(texts) || cJSON_GetArraySize(texts) < 1 || cJSON_GetArraySize(texts) > MINIAPP_TEXT_MAX) return false;
  cJSON_ArrayForEach(item, texts) {
    if (!cJSON_IsString(item)) return false;
    size_t length = strlen(item->valuestring);
    if (length > MINIAPP_TEXT_ENTRY_MAX || used + length + 1 > sizeof(runtime->text_pool) || used + length > 4096) return false;
    for (const unsigned char *s = (const unsigned char *) item->valuestring; *s; s++) if (*s < 32 || *s == 127) return false;
    runtime->text_offset[runtime->text_count++] = (uint16_t) used;
    memcpy(runtime->text_pool + used, item->valuestring, length + 1); used += length + 1;
  }
  return true;
}
#endif

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
  } else if (value->kind == MINIAPP_VALUE_TEXT_AT) {
#if PET_P4_TEXT_TABLE
    copy_utf8(out, out_size, text_at(runtime, var->int_value));
#endif
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

static void local_values(miniapp_runtime_t *r,int32_t v[PET_LOCAL_VARS],bool write){
  if(!write)memset(v,0,sizeof(int32_t)*PET_LOCAL_VARS);
  unsigned count=r->local.wide_masks?PET_LOCAL_VARS:PET_LOCAL_LEGACY_VARS;
  for(unsigned i=0;i<count;i++){if(write)r->vars[r->local_vars[i]].int_value=v[i];else v[i]=r->vars[r->local_vars[i]].int_value;}
}
static void refresh_view(miniapp_runtime_t *runtime) {
#if PET_P4_DATA_ROWS
  if(runtime==&g_runtime&&!runtime->data_rows.enabled)pet_p4_widget_data_set_filter(NULL,NULL,NULL);
#endif
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
  view->app_host=runtime_host_dashboard(runtime);
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
  int32_t v[PET_LOCAL_VARS];local_values(r,v,false);pet_local_view_t o;
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
  if(!r->surface.app||!r->input_v1||r->tick_count||r->page_count||r->state_count!=6||r->transition_count<2||r->game.active||cJSON_HasObjectItem(root,"host")||cJSON_HasObjectItem(root,"scene")||cJSON_HasObjectItem(root,"media")||cJSON_HasObjectItem(root,"data")||cJSON_HasObjectItem(root,"instrument"))return false;
  const char *states[]={"ready","busy","result","editing","empty","offline"};
  for(int i=0;i<6;i++)if(strcmp(r->states[i],states[i])||r->save_states[i]!=0)return false;
  if(!pet_local_parse(local,&r->local)||r->local.action_count!=r->transition_count)return false;
  unsigned local_count=r->local.wide_masks?PET_LOCAL_VARS:PET_LOCAL_LEGACY_VARS;
  if(r->var_count!=local_count||r->save_vars!=((UINT32_C(1)<<local_count)-1)
      ||r->save_version!=(r->local.wide_masks?2:1))return false;
  for(unsigned i=0;i<local_count;i++){char name[12];snprintf(name,sizeof(name),"local%u",i);int index=find_var(r,name);if(index<0||r->vars[index].type!=MINIAPP_VAR_INT)return false;r->local_vars[i]=index;}

  /* v1 packages keep the 16-item deck even though the array now holds the v2 ceiling. */
  for(int i=0;i<r->local.count;i++)if(r->local.groups[i].count>MINIAPP_CAP(r,MINIAPP_V1_LOCAL_ITEMS,MINIAPP_V2_LOCAL_ITEMS,PET_LOCAL_ITEMS))return false;
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
  if(!r->surface.app||!object_keys_allowed(save,keys,4)||!cJSON_IsNumber(v)||v->valuedouble!=v->valueint||v->valueint<1||v->valueint>65535||!cJSON_IsArray(vars)||cJSON_GetArraySize(vars)<1||cJSON_GetArraySize(vars)>MINIAPP_CAP(r,MINIAPP_V1_SAVE_VARS,MINIAPP_V2_SAVE_VARS,MINIAPP_MAX_VARS)||!cJSON_IsObject(states)||cJSON_GetArraySize(states)!=r->state_count)return false;
  const cJSON *page=cJSON_GetObjectItemCaseSensitive(save,"page");if(page&&!cJSON_IsBool(page))return false;r->save_page=cJSON_IsTrue(page);if(r->save_page&&!r->page_count)return false;
  r->save_version=v->valueint;
  bool alarms=false;
#if PET_P4_EVENTS
  alarms=r->cext.alarms.count!=0;
#endif
  size_t worst=pet_store_budget_begin(r->save_page,alarms);
  cJSON_ArrayForEach(item,vars){
    if(!cJSON_IsString(item))return false;int i=find_var(r,item->valuestring);
    if(i<0||(r->save_vars&(1u<<i)))return false;
    bool string=r->vars[i].type!=MINIAPP_VAR_INT;
    bool permitted=!string||r->quota_v3;
    unsigned text_bytes=r->quota_v3?127U:63U;
#if PET_FORM_V2_ENABLED
    if(r->form_text.enabled)for(unsigned f=0;f<r->form.count;f++)if((r->form_text.text_mask&(1U<<f))&&r->form.fields[f].var==i){permitted=true;text_bytes=r->form_text.max_bytes[f];}
#endif
#if PET_FORM_V3_ENABLED
    if(r->form3.enabled)for(unsigned f=0;f<r->form3.count;f++)if(r->form3.fields[f].var==i){permitted=true;text_bytes=r->form3.fields[f].max_bytes;}
#endif
    if(!permitted)return false;r->save_vars|=1u<<i;
    worst+=pet_store_budget_field(r->vars[i].name,string,text_bytes);
  }
  for(int i=0;i<r->state_count;i++){
    const cJSON *s=cJSON_GetObjectItemCaseSensitive(states,r->states[i]);
    if(!cJSON_IsString(s))return false;r->save_states[i]=find_name(r->states,r->state_count,s->valuestring);
    if(r->save_states[i]<0)return false;
  }
  return pet_store_budget_fits(worst,r->quota_v3);
}

/* Stack-owned cJSON nodes plus PrintPreallocated preserve its string escaping
 * without allocating while capturing the latest state at publication. These
 * borrowed nodes are never passed to Delete/AddItem or retained by the store. */
static void save_json_child(cJSON *parent,cJSON *node,const char *key,int type,
                            const char *text,double number){
  node->type=type;node->string=(char *)key;node->valuestring=(char *)text;
  node->valuedouble=number;
  node->valueint=number>=INT_MAX?INT_MAX:number<=INT_MIN?INT_MIN:(int)number;
  if(!parent->child){parent->child=node;node->prev=node;}
  else {cJSON *last=parent->child->prev;last->next=node;node->prev=last;parent->child->prev=node;}
}
#if PET_COMPONENT_PROGRAM_ENABLED
static void component_program_values(const miniapp_runtime_t *r,int32_t values[8]){
  memset(values,0,8*sizeof(*values));for(unsigned i=0;i<r->var_count;i++)if(r->vars[i].type==MINIAPP_VAR_INT)values[i]=r->vars[i].int_value;
}
#if PET_VARIABLE_OWNERS_ENABLED
/* Shared scratch is native-owned under the caller's model mutex, never an
 * input/task-stack host reply or a retained grant. Capture all host words
 * from ONE immutable fresh publication; later replies cannot alter the event. */
static EXT_RAM_BSS_ATTR char g_program_input_names[4][32];
static EXT_RAM_BSS_ATTR int32_t g_program_input_values[8];
static bool component_program_action_values(const miniapp_runtime_t *r,const char *action,
    int32_t values[8],const pet_host_observation_t *observed,uint64_t now_ms){
  component_program_values(r,values);
  if(!r->variable_owners.mixed)return true;
  uint8_t indices[4]={0};unsigned count=0;
  for(unsigned i=0;i<r->program.action_count;i++)if(!strcmp(action,r->program.actions[i].name)){
    for(unsigned k=0;k<r->program.actions[i].event.count;k++){
      int v=r->program.actions[i].input[k];if(!runtime_host_variable(r,v))continue;
      bool found=false;for(unsigned j=0;j<count;j++)if(indices[j]==v)found=true;
      if(found)continue;if(v<0||(unsigned)v>=r->var_count||count>=4||r->vars[v].type!=MINIAPP_VAR_INT)return false;
      indices[count]=(uint8_t)v;memcpy(g_program_input_names[count],r->vars[v].name,32);count++;
    }
    break;
  }
  if(!count)return true; /* Pure native actions do not wait for the PC. */
  bool captured=observed?pet_app_host_integer_snapshot_observed(r->widget_id,g_program_input_names,count,
      g_program_input_values,observed,now_ms):pet_app_host_owned_integer_snapshot(r->widget_id,g_program_input_names,count,g_program_input_values,now_ms);
  if(!captured)return false;
  for(unsigned i=0;i<count;i++){
    unsigned v=indices[i];int32_t value=g_program_input_values[i];
    if(value<r->vars[v].int_min)value=r->vars[v].int_min;
    if(value>r->vars[v].int_max)value=r->vars[v].int_max;
    values[v]=value;
  }
  return true;
}
#endif
static bool component_program_apply_seed(miniapp_runtime_t *r){
  if(!r->program.enabled)return true;
  if(!r->program_seed_ready||!pet_component_program_seed_valid(&r->program,&r->program_seed))return false;
  for(unsigned i=0;i<8;i++){unsigned at=r->program.projection.target[i];if(at!=PET_PROGRAM_UI_UNBOUND)r->vars[at].int_value=r->program_seed.outputs[i];}
#if PET_P4_CELLS_V1
  if(r->cells.enabled)(void)pet_cells_import(&r->cells,r->program_seed.values,(unsigned)r->cells.cols*r->cells.rows,NULL);
#endif
  return true;
}
static bool save_component_program_snapshot(pet_program_snapshot_t *snapshot){
  char id[48]={0};bool capture=false;
  miniapp_runtime_lock();
#if PET_COMPONENT_BACKGROUND_ENABLED
  pet_program_registration_t h={0};
  if(background_current_locked(&g_runtime,&h)){miniapp_runtime_unlock();return true;}
  if(g_runtime.active&&g_runtime.program.enabled&&g_runtime.program_background_owned&&g_runtime.program_seed_ready){
    *snapshot=g_runtime.program_seed;copy_utf8(id,sizeof(id),g_runtime.widget_id);capture=true;
    miniapp_runtime_unlock();return pet_program_persistence_enqueue(id,snapshot);
  }
#endif
  if(g_runtime.active&&g_runtime.program.enabled&&g_runtime.program.image.cells<=PET_PROGRAM_CELLS&&g_component_program_initialized&&g_component_program.revision==g_image_revision){
    capture=component_program_snapshot_locked(snapshot);
    if(capture)copy_utf8(id,sizeof(id),g_runtime.widget_id);
  }
  miniapp_runtime_unlock();
  /* Caller is the serialized component/save parent. This is the existing RAM
   * checkpoint enqueue, not synchronous flash IO or proof of durability. */
  return !capture||pet_program_persistence_enqueue(id,snapshot);
}
static bool save_component_program(void){
#if PET_COMPONENT_BACKGROUND_ENABLED
  /* Independent background checkpoints cannot be replaced by the stale
   * retired foreground session of the same ID. A failure retains its pinned
   * native model, rather than preventing unrelated ordinary navigation. */
  (void)background_checkpoint_one();
#endif
#if PET_P4_PROGRAM_V2
  /* Wide snapshots live in PSRAM; SAVE/protocol stacks keep their existing budgets.
   * Ownership is local to this serialized capture/enqueue call, never shared with a live VM. */
  miniapp_runtime_lock();bool needed=g_runtime.active&&g_runtime.program.enabled;miniapp_runtime_unlock();
  if(!needed)return true;
  pet_program_snapshot_t *snapshot=heap_caps_malloc(sizeof(*snapshot),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!snapshot)return false;
  bool saved=save_component_program_snapshot(snapshot);
  heap_caps_free(snapshot);
  return saved;
#else
  pet_program_snapshot_t snapshot;
  return save_component_program_snapshot(&snapshot);
#endif
}
static void component_program_process_locked(uint64_t now,bool visible){
#if PET_COMPONENT_BACKGROUND_ENABLED
  if(background_project_locked(&g_runtime))return;
#endif
  if(!g_runtime.program.enabled)return;
  /* The storage owner can publish the new runtime, then yield the state lane
   * for cleanup before the outer navigation command publishes screen_page.
   * That intermediate visibility belongs to the OLD view. Neither execute nor
   * latch a hidden-stop against a not-yet-started new lifecycle in this gap.
   * Existing live owners still run/retire normally during unrelated storage;
   * show:false and a real hide after command completion keep their stop latch. */
  if(g_runtime.program_needs_start&&g_storage_lane.occupied)return;
  if(!visible){component_program_retire_locked();g_runtime.program_needs_start=false;g_runtime.program_suspended=true;return;}
  if(g_runtime.program_suspended)return;
#if PET_P4_H264_PREFETCH || PET_P4_NATIVE_WORK_WINDOW
  if(!g_program_slice_admitted)return; /* lifecycle above is never deferred */
#endif
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
  bool held __attribute__((cleanup(component_parent_leave)))=component_parent_enter();if(!held)return;
#endif
  int64_t started_us=esp_timer_get_time();
  if(g_runtime.program_needs_start){
    g_runtime.program_needs_start=false;
    g_component_program_attempt_revision=g_image_revision;
    g_component_program_last_us=g_component_program_max_us=0;
    g_component_program_start_result=PET_RUNTIME_BAD_STATE;
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
    g_component_program_scheduler_initialized=g_background_ready;
    if(g_component_program_scheduler_initialized&&g_background_programs.scheduler.stopped
        &&pet_program_scheduler_rearm(&g_background_programs.scheduler)!=PET_SCHED_OK)return;
    if(!g_component_program_initialized)g_component_program_initialized=g_component_program_scheduler_initialized
      &&pet_program_host_device_init(&g_component_program.host)
      &&pet_component_program_session_bind_scheduler(&g_component_program,&g_background_programs.scheduler);
#else
    if(!g_component_program_scheduler_initialized)
      g_component_program_scheduler_initialized=pet_program_scheduler_init(&g_component_program_scheduler);
    if(g_component_program_scheduler_initialized&&g_component_program_scheduler.stopped
        &&pet_program_scheduler_rearm(&g_component_program_scheduler)!=PET_SCHED_OK)return;
    if(!g_component_program_initialized)g_component_program_initialized=g_component_program_scheduler_initialized
      &&pet_program_host_device_init(&g_component_program.host)
      &&pet_component_program_session_bind_scheduler(&g_component_program,&g_component_program_scheduler);
#endif
    if(!g_component_program_initialized||!g_runtime.program_seed_ready)return;
    g_component_program_start_result=pet_component_program_session_start(&g_component_program,&g_runtime.program,&g_runtime.program_seed,g_image_revision,now);
  }
  int32_t base[8],outputs[8];component_program_values(&g_runtime,base);
#if !PET_COMPONENT_PROGRAM_PARENT_UNIFIED
  if(g_component_program_scheduler_initialized){
    pet_program_schedule_diagnostic_t turn={0};
    (void)pet_program_scheduler_step(&g_component_program_scheduler,now,&turn);
  }
#endif
  if(pet_component_program_session_collect(&g_component_program,g_image_revision,base,outputs)){
    for(unsigned i=0;i<g_runtime.var_count;i++)if(g_runtime.vars[i].type==MINIAPP_VAR_INT)g_runtime.vars[i].int_value=outputs[i];
    (void)pet_component_program_session_snapshot(&g_component_program,&g_runtime.program_seed);
#if PET_P4_CELLS_V1
    if(g_runtime.cells.enabled)(void)pet_cells_import(&g_runtime.cells,g_runtime.program_seed.values,(unsigned)g_runtime.cells.cols*g_runtime.cells.rows,NULL);
#endif
    g_runtime.program_seed_ready=true;g_viewport_epoch++;refresh_view(&g_runtime);
  }
  int64_t elapsed=esp_timer_get_time()-started_us;
  g_component_program_last_us=elapsed<0?0:elapsed>UINT32_MAX?UINT32_MAX:(uint32_t)elapsed;
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
  /* Include the one aggregate parent turn, not merely projection overhead. */
  if(g_background_last_us>g_component_program_last_us)g_component_program_last_us=g_background_last_us;
#endif
  if(g_component_program_last_us>g_component_program_max_us)g_component_program_max_us=g_component_program_last_us;
}
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
static void component_program_visibility_locked(bool visible){
  /* Storage may have published the new model before its navigation ACK. That
   * intermediate old view is not a hide of the not-yet-started lifecycle. */
  if(!visible&&g_runtime.active&&g_runtime.program.enabled&&!g_runtime.program_background_owned
      &&!(g_runtime.program_needs_start&&g_storage_lane.occupied)){
    component_program_retire_locked();g_runtime.program_needs_start=false;g_runtime.program_suspended=true;
  }
}
#endif
#endif
/* Per-call PSRAM workspace: quota-v3 variables plus JSON nodes must never
 * reside on the fixed SAVE stack. No shared scratch survives a concurrent call. */
typedef struct {
  miniapp_var_t vars[MINIAPP_MAX_VARS];
  cJSON nodes[6+MINIAPP_MAX_VARS];
  char json[PET_APP_SAVE_BYTES];
#if PET_P4_EVENTS
  char alarms_text[PET_CEV_SAVE_TEXT_MAX];
#endif
} miniapp_save_workspace_t;
static bool save_app_runtime_workspace(miniapp_save_workspace_t *workspace){
  miniapp_var_t *vars=workspace->vars;char id[48],state[24],page[24]={0};uint16_t version;uint32_t mask;uint8_t count;
  bool fruit;unsigned records[3],mode;
  miniapp_runtime_lock();
  fruit=g_runtime.active&&g_runtime.slicing;
  if(fruit){memcpy(records,g_runtime.fruit.records,sizeof(records));mode=g_runtime.fruit.mode;copy_utf8(id,sizeof(id),g_runtime.widget_id);}
  version=g_runtime.save_version;mask=g_runtime.save_vars;count=g_runtime.var_count;
  if(version){memcpy(vars,g_runtime.vars,sizeof(workspace->vars));copy_utf8(id,sizeof(id),g_runtime.widget_id);copy_utf8(state,sizeof(state),g_runtime.states[g_runtime.current_state]);}
  if(version&&g_runtime.save_page)copy_utf8(page,sizeof(page),g_runtime.pages[g_runtime.current_page]);
#if PET_P4_EVENTS
  char *alarms_text=workspace->alarms_text;
  bool alarms_saved=version&&miniapp_ext_save_text(alarms_text,sizeof(workspace->alarms_text));
#endif
  miniapp_runtime_unlock();
  if(!fruit&&!version)return true;
  cJSON *nodes=workspace->nodes;unsigned used=1;nodes[0].type=cJSON_Object;
  if(fruit){
    save_json_child(nodes,&nodes[used++],"fruitVersion",cJSON_Number,NULL,1);
    save_json_child(nodes,&nodes[used++],"mode",cJSON_Number,NULL,mode);
    cJSON *list=&nodes[used++];save_json_child(nodes,list,"fruitRecords",cJSON_Array,NULL,0);
    for(int i=0;i<3;i++)save_json_child(list,&nodes[used++],NULL,cJSON_Number,NULL,records[i]);
  }else{
    save_json_child(nodes,&nodes[used++],"version",cJSON_Number,NULL,version);
    save_json_child(nodes,&nodes[used++],"state",cJSON_String,state,0);
    if(page[0])save_json_child(nodes,&nodes[used++],"page",cJSON_String,page,0);
#if PET_P4_EVENTS
    if(alarms_saved)save_json_child(nodes,&nodes[used++],"alarms",cJSON_String,alarms_text,0);
#endif
    cJSON *values=&nodes[used++];save_json_child(nodes,values,"vars",cJSON_Object,NULL,0);
    for(int i=0;i<count;i++)if(mask&(1u<<i)){
      if(used>=sizeof(workspace->nodes)/sizeof(workspace->nodes[0]))return false;
      save_json_child(values,&nodes[used++],vars[i].name,
        vars[i].type==MINIAPP_VAR_INT?cJSON_Number:cJSON_String,
        vars[i].type==MINIAPP_VAR_INT?NULL:vars[i].string_value,vars[i].int_value);
    }
  }
  return cJSON_PrintPreallocated(nodes,workspace->json,sizeof(workspace->json),false)&&pet_app_store_update(id,workspace->json);
}

#if PET_P4_EVENTS
static bool miniapp_ext_hidden_checkpoint(void);
#endif
static bool save_app_runtime(void){
  PET_WIDGET_TRACE_SCOPE(trace_save, PET_WTRACE_SAVE_RESTORE);
#if PET_P4_EVENTS
  if(!miniapp_ext_hidden_checkpoint())return false;
#endif
#if PET_COMPONENT_PROGRAM_ENABLED
  if(!save_component_program())return false;
#endif
  miniapp_runtime_lock();
  bool needed=g_runtime.save_version||(g_runtime.active&&g_runtime.slicing);
  miniapp_runtime_unlock();
  if(!needed)return true;
  miniapp_save_workspace_t *workspace=heap_caps_calloc(1,sizeof(*workspace),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!workspace)return false;
  bool saved=save_app_runtime_workspace(workspace);
  heap_caps_free(workspace);
  return saved;
}

static bool restore_app_runtime_buffer(miniapp_runtime_t *r,bool cached_only,char *json){
  if(!(cached_only?pet_app_store_read_cached(r->widget_id,json):pet_app_store_read(r->widget_id,json)))return true;
  if(!pet_form_source_valid(json))return false;
  cJSON *root=cJSON_Parse(json);if(!root)return false;
  if(r->slicing){
    const cJSON *list=cJSON_GetObjectItemCaseSensitive(root,"fruitRecords"),*mode=cJSON_GetObjectItemCaseSensitive(root,"mode"),*version=cJSON_GetObjectItemCaseSensitive(root,"fruitVersion");
    static const char *const fruit_keys[]={"fruitVersion","mode","fruitRecords"};
    bool valid=pet_form_keys(root,fruit_keys,3)&&cJSON_IsNumber(version)&&version->valuedouble==1&&cJSON_IsArray(list)&&cJSON_GetArraySize(list)==3&&cJSON_IsNumber(mode)&&mode->valuedouble>=0&&mode->valuedouble<=2&&mode->valuedouble==mode->valueint;
    for(int i=0;valid&&i<3;i++){const cJSON *v=cJSON_GetArrayItem(list,i);valid=cJSON_IsNumber(v)&&v->valuedouble>=0&&v->valuedouble<=1000000000&&v->valuedouble==v->valueint;}
    if(valid){for(int i=0;i<3;i++)r->fruit.records[i]=cJSON_GetArrayItem(list,i)->valueint;pet_fruit_select_mode(&r->fruit,mode->valueint);}
    cJSON_Delete(root);return valid;
  }
  const cJSON *version=cJSON_GetObjectItemCaseSensitive(root,"version"),*values=cJSON_GetObjectItemCaseSensitive(root,"vars");
  int state=find_name(r->states,r->state_count,json_string(root,"state"));
  const cJSON *saved_page=cJSON_GetObjectItemCaseSensitive(root,"page");
  static const char *const save_keys[]={"version","state","vars","page"};
#if PET_P4_EVENTS
  const cJSON *saved_alarms=cJSON_GetObjectItemCaseSensitive(root,"alarms");
  static const char *const save_keys_alarms[]={"version","state","vars","alarms","page"};
  bool valid=pet_form_keys(root,saved_alarms?save_keys_alarms:save_keys,(saved_page?4:3)+(saved_alarms?1:0))&&
#else
  bool valid=pet_form_keys(root,save_keys,saved_page?4:3)&&
#endif
cJSON_IsNumber(version)&&version->valuedouble==r->save_version&&state>=0&&cJSON_IsObject(values);
  if(valid&&saved_page)valid=r->save_page&&cJSON_IsString(saved_page)&&find_name(r->pages,r->page_count,saved_page->valuestring)>=0;
#if PET_P4_EVENTS
  if(valid)valid=miniapp_ext_restore_text(r,saved_alarms);
#endif
  const cJSON *existing;
  cJSON_ArrayForEach(existing,values){
    if(!valid)break;
    int i=find_var(r,existing->string);
    if(i<0||!(r->save_vars&(1u<<i))){valid=false;break;}
    for(const cJSON *previous=values->child;previous!=existing;previous=previous->next)
      if(!strcmp(previous->string,existing->string)){valid=false;break;}
  }
  for(int i=0;valid&&i<r->var_count;i++)if(r->save_vars&(1u<<i)){
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(values,r->vars[i].name);
    /* Additive fields in the same save version keep their declared defaults.
     * Reject malformed existing values atomically, but preserve earlier progress. */
    if(!v)continue;
    valid=r->vars[i].type==MINIAPP_VAR_INT?(cJSON_IsNumber(v)&&v->valuedouble>=r->vars[i].int_min&&v->valuedouble<=r->vars[i].int_max&&v->valuedouble==v->valueint):(cJSON_IsString(v)&&strlen(v->valuestring)<(r->quota_v3?sizeof(r->vars[i].string_value):(size_t)MINIAPP_V2_STRING_MAX));
#if PET_FORM_V2_ENABLED
    if(valid&&r->form_text.enabled&&r->vars[i].type!=MINIAPP_VAR_INT)valid=pet_form_utf8(v->valuestring,63,NULL,NULL,NULL);
#endif
#if PET_FORM_V3_ENABLED
    if(valid&&form3_owns(r,(unsigned)i)&&r->vars[i].type!=MINIAPP_VAR_INT)valid=pet_form_utf8(v->valuestring,form3_string_limit(r,(unsigned)i),NULL,NULL,NULL);
#endif
  }
  if(valid){
    for(int i=0;i<r->var_count;i++)if(r->save_vars&(1u<<i)){
      const cJSON *v=cJSON_GetObjectItemCaseSensitive(values,r->vars[i].name);
      if(!v)continue;
      if(r->vars[i].type==MINIAPP_VAR_INT)r->vars[i].int_value=v->valueint;else copy_utf8(r->vars[i].string_value,sizeof(r->vars[i].string_value),v->valuestring);
    }
    r->current_state=r->save_states[state];refresh_view(r);
    if(r->save_page){int page=find_name(r->pages,r->page_count,json_string(root,"page"));if(page>=0)r->current_page=page;refresh_view(r);}
  }
  /* Invalid saved data must stop publication. Continuing with declared defaults
   * would let the next ordinary save overwrite the user's preserved file. */
  cJSON_Delete(root);return valid;
}
static bool restore_app_runtime_from(miniapp_runtime_t *r,bool cached_only){
  if(!r||(!r->save_version&&!r->slicing))return true;
  char *json=heap_caps_malloc(PET_APP_SAVE_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!json)return false;
  bool restored=restore_app_runtime_buffer(r,cached_only,json);
  heap_caps_free(json);
  return restored;
}
static bool restore_app_runtime(miniapp_runtime_t *r){return restore_app_runtime_from(r,false);}

static bool sync_side_runtime(miniapp_runtime_t*r){
  if(!r->side_enabled)return false;
  int32_t values[]={r->side.score,r->side.coins,r->side.lives,r->side.checkpoint};bool changed=false;
  for(int i=0;i<4;i++)if(r->vars[r->side_vars[i]].int_value!=values[i]){r->vars[r->side_vars[i]].int_value=values[i];changed=true;}
  int state=r->side.phase==PET_SIDE_RUNNING?r->side_states[0]:r->side.phase==PET_SIDE_LOST?r->side_states[1]:r->side.phase==PET_SIDE_WON?r->side_states[2]:r->side.phase==PET_SIDE_PAUSED?r->side_states[3]:r->current_state;
  if(state!=r->current_state){r->current_state=state;changed=true;}return changed;
}
static bool parse_side_runtime(miniapp_runtime_t*r,const cJSON*root,const cJSON*scene,char*error,size_t size){
  pet_side_declaration_t*d=calloc(1,sizeof(*d));bool ok=false;if(!d)return false;
  if(!pet_side_parse(scene,d,error,size)||!r->surface.app||!r->surface.graphics||!r->input_v1||r->tick_count||r->page_count)goto done;
  const char*conflicts[]={"host","game","data","media","instrument","local","lifecycle","pages"};for(unsigned i=0;i<sizeof(conflicts)/sizeof(conflicts[0]);i++)if(cJSON_HasObjectItem(root,conflicts[i]))goto done;
  if(!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root,"initial_state")))goto done;
  if(r->surface.game_view||r->surface.scene_view.enabled||r->surface.data_view.enabled)goto done;
  const char*states[]={d->active_state,d->result_state,d->win_state};
  for(int i=0;i<3;i++){r->side_states[i]=find_name(r->states,r->state_count,states[i]);if(r->side_states[i]<0)goto done;for(int j=0;j<i;j++)if(r->side_states[j]==r->side_states[i])goto done;}
  int32_t maximums[]={1000000000,65535,d->config.lives,d->config.actor_count};
  for(int i=0;i<4;i++){int v=find_var(r,d->vars[i]);if(v<0||r->vars[v].type!=MINIAPP_VAR_INT||r->vars[v].int_min>0||r->vars[v].int_max<maximums[i])goto done;for(int j=0;j<i;j++)if(r->side_vars[j]==v)goto done;r->side_vars[i]=v;}
  int image=-1;for(int i=0;i<r->surface.image_count;i++)if(!strcmp(r->surface.image_ids[i],d->art.image))image=i;
  if(image<0||r->surface.image_jpeg[image]||r->surface.image_w[image]!=d->art.columns*16||r->surface.image_h[image]!=d->art.rows*16)goto done;
  const char*events[]={"knob.rotate_ccw","knob.rotate_cw","button.sw1.short_press","button.sw2.short_press"};
  for(int i=0;i<4;i++){
    bool found=false;for(int b=0;b<r->button_count;b++)if(!strcmp(r->buttons[b].action,d->actions[i])&&!strcmp(r->buttons[b].event,events[i]))found=true;
    if(!found)goto done;copy_utf8(r->side_actions[i],48,d->actions[i]);
  }
  if(strcmp(r->hold_action,d->actions[2]))goto done;
  r->side_states[3]=-1;
  for(int i=0;i<r->transition_count;i++){
    const miniapp_transition_t*t=&r->transitions[i];bool known=false;for(int a=0;a<4;a++)if(!strcmp(t->action,d->actions[a]))known=true;
    if(!known||t->effect_count||t->page_op!=-1)goto done;
    if(!strcmp(t->action,d->actions[3])&&t->from_state==r->side_states[0]&&t->to_state>=0&&t->to_state!=r->side_states[0]&&t->to_state!=r->side_states[1]&&t->to_state!=r->side_states[2]){
      if(r->side_states[3]>=0)goto done;r->side_states[3]=t->to_state;
    }
  }
  if(r->side_states[3]<0)goto done;
  bool resume=false;for(int i=0;i<r->transition_count;i++){const miniapp_transition_t*t=&r->transitions[i];if(!strcmp(t->action,d->actions[3])&&t->from_state==r->side_states[3]&&t->to_state==r->side_states[0])resume=true;}
  if(!resume||r->current_state==r->side_states[0]||r->current_state==r->side_states[1]||r->current_state==r->side_states[2]||r->current_state==r->side_states[3]||!pet_side_init(&r->side,&d->config))goto done;
  r->side_art=d->art;r->side_enabled=true;sync_side_runtime(r);ok=true;
done:free(d);if(!ok)set_error(error,size,"invalid side scene resources, state or input references");return ok;
}

static void finish_component_restore(miniapp_runtime_t*r){
#if PET_FORM_V3_ENABLED
  if(r&&r->form3.enabled){pet_form3_value_t *values=form3_workspace_begin();form3_values(r,values);pet_form3_initialize(&r->form3,values);form3_write(r,values);form3_workspace_end();refresh_view(r);}
  else
#endif
#if PET_FORM_V2_ENABLED
  if(r&&r->form_text.enabled){pet_form_value_t values[PET_FORM_VARS];form2_values(r,values);pet_form2_initialize(&r->form,&r->form_text,values);form2_write(r,values);refresh_view(r);}
  else
#endif
  if(r&&r->form.enabled){int32_t values[PET_FORM_VARS];form_values(r,values);pet_form_initialize(&r->form,values);form_write(r,values);refresh_view(r);}
  if(r&&r->side_enabled){
    unsigned checkpoint=(unsigned)r->vars[r->side_vars[3]].int_value;
    /* Restored root variables must enter the core before its first tick sync.
     * A removed marker is not an arbitrary position and safely starts at zero. */
    if(!pet_side_restore_checkpoint(&r->side,checkpoint))pet_side_restore_checkpoint(&r->side,0);
    sync_side_runtime(r);
  }
}
static bool restore_component_runtime_from(miniapp_runtime_t*r,bool cached_only){
  PET_WIDGET_TRACE_SCOPE(trace_restore, PET_WTRACE_SAVE_RESTORE);
  if(!(cached_only?restore_app_runtime_from(r,true):restore_app_runtime(r)))return false;
  finish_component_restore(r);return true;
}
static bool restore_component_runtime(miniapp_runtime_t*r){
#if PET_COMPONENT_PROGRAM_ENABLED
  if(r&&r->program.enabled){
    bool from_live=false,compatible=true;
    miniapp_runtime_lock();
#if PET_COMPONENT_BACKGROUND_ENABLED
    compatible=background_seed_locked(r,&from_live);
#endif
    if(!from_live&&g_runtime.active&&g_runtime.program.enabled&&!strcmp(r->widget_id,g_runtime.widget_id)){
      if(g_component_program_initialized&&g_component_program.revision==g_image_revision
#if PET_COMPONENT_BACKGROUND_ENABLED
          &&!g_runtime.program_background_owned
#endif
          )
        from_live=component_program_snapshot_locked(&r->program_seed);
      else if(g_runtime.program_seed_ready){r->program_seed=g_runtime.program_seed;from_live=true;}
      compatible=from_live&&pet_component_program_seed_valid(&r->program,&r->program_seed);
      if(compatible){component_program_retire_locked();g_runtime.program_needs_start=false;g_runtime.program_suspended=true;}
    }
    miniapp_runtime_unlock();
    if(!compatible)return false;
    /* P4S1 remains byte-identical for <=64 cells; wide v2 images use the versioned P4S2 codec. */
    if(!from_live&&r->program.image.cells<=PET_PROGRAM_CELLS){
      pet_program_save_result_t status=g_storage_lane.occupied
        ?pet_program_persistence_load_with_lease(r->widget_id,&r->program.image,&r->program_seed,g_storage_resource_lease)
        :pet_program_persistence_load(r->widget_id,&r->program.image,&r->program_seed);
      if(status!=PET_PROGRAM_SAVE_EMPTY&&status!=PET_PROGRAM_SAVE_VALUE)return false;
    }
    r->program_seed_ready=true;
    if(!component_program_apply_seed(r))return false;
    r->program_suspended=false; /* Explicit prepared activation, never focus recovery. */
  }
#endif
  if(r&&(r->save_version||r->slicing)){
    char *json=heap_caps_malloc(PET_APP_SAVE_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!json)return false;
    pet_app_store_status_t status=g_storage_lane.occupied
      ?pet_app_store_load_with_lease(r->widget_id,json,g_storage_resource_lease)
      :pet_app_store_load(r->widget_id,json);
    heap_caps_free(json);
    if(status!=PET_APP_STORE_EMPTY&&status!=PET_APP_STORE_VALUE)return false;
  }
  return restore_component_runtime_from(r,false);
}
/* Maximum valid saved UTF-8 bytes, according to the actual profile and role.
 * A form3 package does not shrink unrelated quota-v3 strings. */
static unsigned runtime_string_limit(const miniapp_runtime_t *r,unsigned at){
  unsigned limit=r->quota_v3?sizeof(r->vars[at].string_value)-1:MINIAPP_V2_STRING_MAX-1;
#if PET_FORM_V3_ENABLED
  if(form3_owns(r,at)){unsigned role=form3_string_limit(r,at);if(role<limit)limit=role;}
#endif
#if PET_FORM_V2_ENABLED
  if(r->form_text.enabled)for(unsigned i=0;i<r->form.count;i++)if((r->form_text.text_mask&(1u<<i))
      &&(at==r->form.fields[i].var||at==r->form.fields[i].draft_var)&&r->form_text.max_bytes[i]<limit)limit=r->form_text.max_bytes[i];
#endif
  return limit;
}
/* The active runtime may advance while prepare writes flash. Merge only the
 * declared persisted fields into its replacement. No JSON, allocation, I/O or
 * old full-runtime copy is allowed in this publication step. */
static void merge_live_persistent_runtime(miniapp_runtime_t *next){
  bool valid=false;
  miniapp_runtime_lock();
  if(next&&g_runtime.active&&!strcmp(next->widget_id,g_runtime.widget_id)){
#if PET_COMPONENT_PROGRAM_ENABLED
#if PET_COMPONENT_BACKGROUND_ENABLED
    bool background_seed=false;
    if(next->program.enabled)(void)background_seed_locked(next,&background_seed);
#endif
    if(next->program.enabled&&g_runtime.program.enabled&&g_component_program_initialized&&g_component_program.revision==g_image_revision
#if PET_COMPONENT_BACKGROUND_ENABLED
        &&!background_seed&&!g_runtime.program_background_owned
#endif
        &&component_program_snapshot_locked(&g_component_program_save)){
      /* Program preflight quiesced this exact owner before storage work, so
       * this reads the same retained committed model, not a stale disk seed. */
      if(pet_component_program_seed_valid(&next->program,&g_component_program_save)){
        next->program_seed=g_component_program_save;next->program_seed_ready=true;(void)component_program_apply_seed(next);
      }
    }
#endif
    if(next->slicing&&g_runtime.slicing){
      memcpy(next->fruit.records,g_runtime.fruit.records,sizeof(next->fruit.records));
      pet_fruit_select_mode(&next->fruit,g_runtime.fruit.mode);
    }else if(next->save_version&&next->save_version==g_runtime.save_version){
      int state=find_name(next->states,next->state_count,g_runtime.states[g_runtime.current_state]);
      valid=state>=0;
      for(int i=0;valid&&i<next->var_count;i++)if(next->save_vars&(1u<<i)){
        int old=find_var(&g_runtime,next->vars[i].name);
        if(old<0||!(g_runtime.save_vars&(1u<<old)))continue;
        const miniapp_var_t *value=&g_runtime.vars[old];
        valid=value->type==next->vars[i].type&&(value->type==MINIAPP_VAR_INT
          ?(value->int_value>=next->vars[i].int_min&&value->int_value<=next->vars[i].int_max)
          :strlen(value->string_value)<=runtime_string_limit(next,(unsigned)i));
      }
      if(valid){
        for(int i=0;i<next->var_count;i++)if(next->save_vars&(1u<<i)){
          int old=find_var(&g_runtime,next->vars[i].name);
          if(old<0||!(g_runtime.save_vars&(1u<<old)))continue;
          next->vars[i].int_value=g_runtime.vars[old].int_value;
          copy_utf8(next->vars[i].string_value,sizeof(next->vars[i].string_value),g_runtime.vars[old].string_value);
        }
        next->current_state=next->save_states[state];
        if(next->save_page&&g_runtime.save_page){
          int page=find_name(next->pages,next->page_count,g_runtime.pages[g_runtime.current_page]);
          if(page>=0)next->current_page=page;
        }
      }
    }
  }
  miniapp_runtime_unlock();
  if(valid)refresh_view(next);
  finish_component_restore(next);
}

#if PET_P4_LONGTEXT
/* Count mirrors are native derived writers, not an additional form, host or
 * VM output writer. Check after all three declarations have been parsed. */
static bool longtext_writers_valid(const miniapp_runtime_t *r){
  for(unsigned t=0;t<r->surface.text_count;t++){
    const pet_surface_longtext_t *lt=&r->surface.texts[t];if(!lt->used||!lt->count_var[0])continue;
    int at=find_var(r,lt->count_var);if(at<0||r->vars[at].type!=MINIAPP_VAR_INT||runtime_host_variable(r,at))return false;
    if(r->form.enabled){
      if(at==r->form.focus_var||at==r->form.active_var)return false;
      for(unsigned f=0;f<r->form.count;f++)if(at==r->form.fields[f].var||at==r->form.fields[f].draft_var)return false;
    }
#if PET_FORM_V2_ENABLED
    if(r->form_text.enabled&&(at==r->form_text.index_var||at==r->form_text.label_var))return false;
#endif
#if PET_FORM_V3_ENABLED
    if(form3_owns(r,(unsigned)at))return false;
#endif
#if PET_COMPONENT_PROGRAM_ENABLED
    if(r->program.enabled)for(unsigned i=0;i<8;i++)if(r->program.projection.target[i]==at)return false;
#endif
    /* Only direct effect targets are writers. Conditions, source_var and VM
     * action inputs may still read this derived value. */
    for(unsigned i=0;i<r->transition_count;i++){
      const miniapp_transition_t *rule=&r->transitions[i];
      for(unsigned j=0;j<(unsigned)rule->effect_count+rule->else_effect_count;j++)if(rule->effects[j].var_index==at)return false;
    }
    for(unsigned i=0;i<r->tick_count;i++){
      const miniapp_tick_t *tick=&r->ticks[i];
      for(unsigned j=0;j<tick->effect_count;j++)if(tick->effects[j].var_index==at)return false;
      for(unsigned j=0;j<tick->completion_effect_count;j++)if(tick->completion_effects[j].var_index==at)return false;
    }
    for(unsigned j=0;j<t;j++)if(r->surface.texts[j].used&&!strcmp(lt->count_var,r->surface.texts[j].count_var))return false;
  }
  return true;
}
#endif

static bool parse_runtime(
  miniapp_runtime_t *runtime,
  const char *widget_id,
  const char *widget_json,
  const char *buttons_json,
  const pet_p4_miniapp_sprite_pack_t *sprites,
  char *error,
  size_t error_size
) {
  PET_WIDGET_TRACE_SCOPE(trace_parse, PET_WTRACE_PARSE);
  static const char *const allowed[] = {
    "schema_version", "vars", "states", "initial_state", "pages", "initial_page",
    "transitions", "tick", "dashboard", "fetchers", "readers", "engine", "scene",
    "game", "data", "media", "instrument", "surface", "persist", "limits", "host", "host_context", "host_contract", "lifecycle", "input", "local", "navigation", "form", "program", "ownership", "logic", "quota", "sfx", "texts", "cells", "graphics",
#if PET_P4_SEQUENCE
    "sequence",
#endif
#if PET_P4_NOTIFY
    "notify",
#endif
#if PET_P4_SAMPLES
    "samples",
#endif
#if PET_P4_EVENTS
    "events", "alarms",
#endif
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
  if(!pet_json_source_bounded_text(widget_json)||!pet_json_source_bounded_text(buttons_json?buttons_json:"[]")){
    set_error(error,error_size,"device JSON text is invalid or exceeds the container depth limit");return false;
  }
  root = cJSON_ParseWithOpts(widget_json,NULL,true);
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
  bool side_scene=scene&&!strcmp(json_string(scene,"engine"),"p4-side-scene-v1");
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
  if(cJSON_HasObjectItem(root,"logic")){
    /* Capability negotiation: only v4 packages that name the exact profile get
     * guards/else/var-sources; everything else keeps the pre-logic contract. */
    /* Native presets (game) may use guards and tick conditions too: the var
     * mirror is pushed/pulled unchanged because presets never write vars[]. */
    if(!runtime_v4||strcmp(json_string(root,"logic"),"p4-logic-v1")||cJSON_HasObjectItem(root,"local")||cJSON_HasObjectItem(root,"media")||cJSON_HasObjectItem(root,"instrument")||cJSON_HasObjectItem(root,"host")||cJSON_HasObjectItem(root,"program")){
      set_error(error,error_size,"logic requires p4-bounded-runtime-v4 and p4-logic-v1 without host/local/media/instrument/program");goto fail;
    }
    parsed->logic=true;
  }
  if(cJSON_HasObjectItem(root,"quota")){
    /* v2/v3 ceilings are negotiated, never inferred from the runtime version. */
    const char *quota=json_string(root,"quota");
    bool v3=PET_P4_QUOTA_V3&&!strcmp(quota,"p4-quota-v3");
    if(!runtime_v4||(strcmp(quota,"p4-quota-v2")&&!v3)||legacy_game){
      set_error(error,error_size,"quota requires p4-bounded-runtime-v4 and p4-quota-v2/p4-quota-v3");goto fail;
    }
    parsed->quota_v2=true;parsed->quota_v3=v3;
  }
  if(cJSON_HasObjectItem(root,"graphics")){
#if PET_P4_GRAPHICS_V2
    /* p4-graphics-v2 is negotiated by name; it never follows from the runtime version. */
    if(!runtime_v4||strcmp(json_string(root,"graphics"),"p4-graphics-v2")){set_error(error,error_size,"graphics requires p4-bounded-runtime-v4 and p4-graphics-v2");goto fail;}
    parsed->graphics_v2=true;
#else
    set_error(error,error_size,"graphics profile is not available on this device");goto fail;
#endif
  }
  /* Packages below p4-quota-v3 keep the 8191-byte compact widget.json even when the staging buffer is larger. */
  if(!parsed->quota_v3&&strlen(widget_json)>=MINIAPP_V2_WIDGET_JSON_MAX){set_error(error,error_size,"widget.json exceeds the negotiated size");goto fail;}
  if(cJSON_HasObjectItem(root,"texts")){
#if PET_P4_TEXT_TABLE
    if(!runtime_v4||!parse_texts(parsed,cJSON_GetObjectItemCaseSensitive(root,"texts"))){set_error(error,error_size,"texts requires p4-bounded-runtime-v4 and 1-64 bounded UTF-8 entries (<=120 bytes each, <=4096 total)");goto fail;}
#else
    set_error(error,error_size,"package text table capability is unavailable");goto fail;
#endif
  }
  if(cJSON_HasObjectItem(root,"cells")){
#if PET_P4_CELLS_V1
    static const char *const cell_keys[]={"engine","cols","rows","bits","init"};
    const cJSON *cells=cJSON_GetObjectItemCaseSensitive(root,"cells"),*cols=cJSON_GetObjectItemCaseSensitive(cells,"cols"),*rows=cJSON_GetObjectItemCaseSensitive(cells,"rows"),*bits=cJSON_GetObjectItemCaseSensitive(cells,"bits"),*init=cJSON_GetObjectItemCaseSensitive(cells,"init");
    if(!runtime_v4||!cJSON_IsObject(cells)||!object_keys_allowed(cells,cell_keys,5)||strcmp(json_string(cells,"engine"),PET_CELLS_ENGINE)
       ||!cJSON_IsNumber(cols)||cols->valuedouble!=cols->valueint||!cJSON_IsNumber(rows)||rows->valuedouble!=rows->valueint||!cJSON_IsNumber(bits)||bits->valuedouble!=bits->valueint
       ||cols->valueint<1||cols->valueint>(int)PET_CELLS_MAX_SIDE||rows->valueint<1||rows->valueint>(int)PET_CELLS_MAX_SIDE
       ||!pet_cells_init(&parsed->cells,(unsigned)cols->valueint,(unsigned)rows->valueint,(unsigned)bits->valueint)
       ||legacy_game||scene||cJSON_HasObjectItem(root,"local")||cJSON_HasObjectItem(root,"media")||cJSON_HasObjectItem(root,"instrument")||cJSON_HasObjectItem(root,"data")||cJSON_HasObjectItem(root,"host")){
      set_error(error,error_size,"cells requires p4-bounded-runtime-v4, p4-cells-v1, cols/rows 1-16 (<=256 cells), bits 2|4, without game/scene/local/media/instrument/data/host");goto fail;
    }
    if(init){
      if(!cJSON_IsArray(init)||cJSON_GetArraySize(init)!=rows->valueint){set_error(error,error_size,"cells init needs one row string per row");goto fail;}
      for(int y=0;y<rows->valueint;y++){const cJSON *row=cJSON_GetArrayItem(init,y);
        if(!cJSON_IsString(row)||!pet_cells_init_row(&parsed->cells,(unsigned)y,row->valuestring)){set_error(error,error_size,"cells init rows need exactly cols hex digits below 2^bits");goto fail;}}
    }
    parsed->cells_program_owned=cJSON_HasObjectItem(root,"program");
#else
    set_error(error,error_size,"cells capability is unavailable");goto fail;
#endif
  }
  if(cJSON_HasObjectItem(root,"sfx")){
    /* p4-sfx-v1: named cues over fixed presets. Not with the instrument or media paths,
     * which own the speaker worker in a different way. */
    static const char *const sfx_keys[]={"engine","volume","cues"};
    static const char *const cue_keys[]={"id","preset"};
    const cJSON *sfx=cJSON_GetObjectItemCaseSensitive(root,"sfx"),*volume,*cues,*cue;
    if(!runtime_v4||!cJSON_IsObject(sfx)||!object_keys_allowed(sfx,sfx_keys,3)||strcmp(json_string(sfx,"engine"),"p4-sfx-v1")
       ||cJSON_HasObjectItem(root,"instrument")||cJSON_HasObjectItem(root,"media")||cJSON_HasObjectItem(root,"local")||cJSON_HasObjectItem(root,"program")){
      set_error(error,error_size,"sfx requires p4-bounded-runtime-v4 and p4-sfx-v1 without instrument/media/local/program");goto fail;
    }
    volume=cJSON_GetObjectItemCaseSensitive(sfx,"volume");cues=cJSON_GetObjectItemCaseSensitive(sfx,"cues");
    if(!cJSON_IsNumber(volume)||volume->valuedouble!=volume->valueint||volume->valueint<0||volume->valueint>100
       ||!cJSON_IsArray(cues)||cJSON_GetArraySize(cues)<1||cJSON_GetArraySize(cues)>(int)PET_SFX_MAX_CUES){
      set_error(error,error_size,"sfx needs volume 0-100 and 1-8 cues");goto fail;
    }
    cJSON_ArrayForEach(cue,cues){
      const char *id=json_string(cue,"id");int preset=pet_sfx_preset_index(json_string(cue,"preset"));
      if(!cJSON_IsObject(cue)||!object_keys_allowed(cue,cue_keys,2)||!safe_id(id,16)||preset<0||find_sfx_cue(parsed,id)>=0){
        set_error(error,error_size,"sfx cue needs a unique safe id and a known preset");goto fail;
      }
      copy_utf8(parsed->sfx_cue_id[parsed->sfx_count],sizeof(parsed->sfx_cue_id[0]),id);
      parsed->sfx_cue_preset[parsed->sfx_count++]=(uint8_t)preset;
      parsed->sfx=true;
    }
    parsed->sfx_volume=(uint8_t)volume->valueint;
  }
  bool mixed_host=false;
  if(cJSON_HasObjectItem(root,"ownership")){
#if PET_VARIABLE_OWNERS_ENABLED
    if(!pet_variable_owners_parse(root,&parsed->variable_owners)||!parsed->variable_owners.mixed){set_error(error,error_size,"invalid variable writer ownership");goto fail;}
    mixed_host=true;
#else
    set_error(error,error_size,"variable ownership capability is unavailable");goto fail;
#endif
  }
  if(cJSON_HasObjectItem(root,"navigation")){
    const cJSON *input=cJSON_GetObjectItemCaseSensitive(root,"input");
    const cJSON *pages=cJSON_GetObjectItemCaseSensitive(root,"pages");
    if(strcmp(json_string(root,"navigation"),"p4-app-navigation-v1")||!parsed->app_expressions||strcmp(json_string(input,"engine"),"p4-input-v1")
       ||!cJSON_IsArray(pages)||cJSON_GetArraySize(pages)<2||cJSON_GetArraySize(pages)>4||scene||legacy_game
       ||(cJSON_HasObjectItem(root,"host")&&!mixed_host)||cJSON_HasObjectItem(root,"local")||cJSON_HasObjectItem(root,"media")||cJSON_HasObjectItem(root,"instrument")||cJSON_HasObjectItem(root,"data")){
      set_error(error,error_size,"invalid local application navigation profile");goto fail;
    }
    parsed->navigation=true;
  }
  if ((engine[0] && strcmp(engine, "p4-bounded-runtime-v3") != 0 && !runtime_v4)
      || (scene && !engine[0]) || (scene && legacy_game)) {
    set_error(error, error_size, "engine/scene declaration is unsupported or conflicts with legacy game");
    goto fail;
  }
  if (!parse_vars(parsed, cJSON_GetObjectItemCaseSensitive(root, "vars"), error, error_size)) goto fail;
#if PET_P4_NOTIFY
  if(!miniapp_parse_notify(parsed,root)){set_error(error,error_size,"invalid runtime.notify declaration");goto fail;}
#endif
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
    static const char *const data_keys[] = {"source", "page_var", "select_var", "filters", "refresh_action"};
    const char *source = json_string(data, "source");
    int page_var = find_var(parsed, json_string(data, "page_var"));
    bool data_rows_keys=cJSON_HasObjectItem(data,"select_var")||cJSON_HasObjectItem(data,"filters")||cJSON_HasObjectItem(data,"refresh_action");
#if PET_P4_DATA_ROWS
    if(data_rows_keys){
      /* p4-data-rows-v1: optional selection var, <=4 enum filters bound to int vars, reserved refresh action. */
      pet_data_rows_config_t *dr=&parsed->data_rows;const cJSON *filters=cJSON_GetObjectItemCaseSensitive(data,"filters"),*filter;
      if(cJSON_HasObjectItem(data,"select_var")){int sv=find_var(parsed,json_string(data,"select_var"));if(sv<0||parsed->vars[sv].type!=MINIAPP_VAR_INT||sv==page_var){set_error(error,error_size,"data.select_var must be a distinct int var");goto fail;}copy_utf8(dr->select_var,sizeof(dr->select_var),json_string(data,"select_var"));}
      if(cJSON_HasObjectItem(data,"refresh_action")){const char *ra=json_string(data,"refresh_action");if(strcmp(ra,"data.refresh")){set_error(error,error_size,"data.refresh_action must be the declared data.refresh transition");goto fail;}copy_utf8(dr->refresh_action,sizeof(dr->refresh_action),ra);}
      if(filters){
        if(!cJSON_IsArray(filters)||cJSON_GetArraySize(filters)<1||cJSON_GetArraySize(filters)>PET_DATA_FILTERS_MAX){set_error(error,error_size,"data.filters needs 1-4 entries");goto fail;}
        cJSON_ArrayForEach(filter,filters){
          static const char *const fk[]={"var","field","values"};pet_data_filter_t *f=&dr->filters[dr->filter_count];
          const cJSON *values=cJSON_GetObjectItemCaseSensitive(filter,"values"),*value;int fv=find_var(parsed,json_string(filter,"var")),field=pet_data_field_index(json_string(filter,"field"));
          if(!cJSON_IsObject(filter)||!object_keys_allowed(filter,fk,3)||fv<0||parsed->vars[fv].type!=MINIAPP_VAR_INT||fv==page_var||field<0||!cJSON_IsArray(values)||cJSON_GetArraySize(values)<1||cJSON_GetArraySize(values)>PET_DATA_FILTER_VALUES_MAX){set_error(error,error_size,"data filter needs an int var, a row field and 1-8 values");goto fail;}
          for(unsigned k=0;k<dr->filter_count;k++)if(!strcmp(dr->filters[k].var,json_string(filter,"var"))){set_error(error,error_size,"duplicate data filter var");goto fail;}
          copy_utf8(f->var,sizeof(f->var),json_string(filter,"var"));f->field=(uint8_t)field;
          cJSON_ArrayForEach(value,values){
            char text[PET_DATA_FILTER_VALUE_BYTES+1];
            if(cJSON_IsString(value)){if(strlen(value->valuestring)>PET_DATA_FILTER_VALUE_BYTES){set_error(error,error_size,"data filter value too long");goto fail;}copy_utf8(text,sizeof(text),value->valuestring);}
            else if(cJSON_IsNumber(value)&&value->valuedouble==value->valueint&&value->valueint>=-1&&value->valueint<=1&&field==PET_DATA_FIELD_TONE)snprintf(text,sizeof(text),"%d",value->valueint);
            else{set_error(error,error_size,"data filter values are strings (tone: -1/0/1)");goto fail;}
            copy_utf8(f->values[f->count++],sizeof(f->values[0]),text);
          }
          dr->filter_count++;
        }
      }
      dr->enabled=true;
    }
#else
    if(data_rows_keys){set_error(error,error_size,"data filters/select need p4-data-rows-v1");goto fail;}
#endif
    if (!runtime_v4 || scene || legacy_game || !cJSON_IsObject(data)
        || !object_keys_allowed(data, data_keys, 5) || !source[0] || strlen(source) > 47
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
                        &parsed->state_count, MINIAPP_CAP(parsed, MINIAPP_V1_MAX_STATES, MINIAPP_V2_MAX_STATES, MINIAPP_MAX_STATES), "states", error, error_size)) goto fail;
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
#if PET_P4_CAPABILITY_EXT
  if (!miniapp_ext_parse(parsed, root, runtime_v4, error, error_size)) goto fail;
#endif
  if (!parse_transitions(parsed, cJSON_GetObjectItemCaseSensitive(root, "transitions"), error, error_size)) goto fail;
#if PET_P4_DATA_ROWS
  if(parsed->data_rows.refresh_action[0]&&!runtime_has_transition_action(parsed,parsed->data_rows.refresh_action)){set_error(error,error_size,"data.refresh requires a declared transition");goto fail;}
#endif
  if (!parse_ticks(parsed, cJSON_GetObjectItemCaseSensitive(root, "tick"), error, error_size)) goto fail;
  if (!parse_dashboard(parsed, cJSON_GetObjectItemCaseSensitive(root, "dashboard"), error, error_size)) goto fail;
  if (!parse_buttons(parsed, buttons_json, error, error_size)) goto fail;
  if(!parse_input_policy(parsed,root)){set_error(error,error_size,"invalid input policy");goto fail;}
  if(parsed->transition_count>12&&!parsed->input_v1){set_error(error,error_size,"extended transitions require input v1");goto fail;}
  const cJSON *surface=cJSON_GetObjectItemCaseSensitive(root,"surface");
  if(surface&&!media){
    if(!runtime_v4||instrument||!pet_surface_parse_profile(surface,&parsed->surface,MINIAPP_CAP(parsed,PET_SURFACE_V1_NODES,MINIAPP_V2_SURFACE_NODES,PET_SURFACE_NODES),MINIAPP_GRAPHICS_V2(parsed))||!parsed->surface.app||
      (!!legacy_game!=parsed->surface.game_view)|| (!!data!=parsed->surface.data_view.enabled)||
      ((!!scene&&!side_scene)!=parsed->surface.scene_view.enabled)){
      set_error(error,error_size,"invalid application surface");goto fail;
    }
    if(scene&&!side_scene&&cJSON_HasObjectItem(scene,"sprites")){set_error(error,error_size,"scene view sprites belong to surface");goto fail;}
    _Static_assert(MINIAPP_V1_SURFACE_NODES==PET_SURFACE_V1_NODES&&MINIAPP_V2_SURFACE_NODES<=PET_SURFACE_NODES,"surface budgets must match the surface parser");
    if(parsed->surface.viewport.enabled||parsed->surface.viewport_count){
      if(!parsed->input_v1||scene||legacy_game||data||media||instrument){set_error(error,error_size,"viewport requires input v1 and an application");goto fail;}
      int count=parsed->surface.viewport_count?parsed->surface.viewport_count:1;
      for(int i=0;i<count;i++){
        const pet_surface_viewport_t *v=parsed->surface.viewport_count?&parsed->surface.viewports[i]:&parsed->surface.viewport;
        int offset=find_var(parsed,v->offset_var);
        if(offset<0||parsed->vars[offset].type!=MINIAPP_VAR_INT){set_error(error,error_size,"viewport requires an integer offset");goto fail;}
      }
    }
    if(scene&&!side_scene){const cJSON *grid=cJSON_GetObjectItemCaseSensitive(scene,"grid");if(cJSON_HasObjectItem(grid,"rows")||cJSON_HasObjectItem(grid,"solid_tones")){set_error(error,error_size,"scene surface v1 requires an open field");goto fail;}}
    if(scene&&!side_scene){const cJSON *e;cJSON_ArrayForEach(e,cJSON_GetObjectItemCaseSensitive(scene,"entities"))if(cJSON_HasObjectItem(e,"sprite")){set_error(error,error_size,"scene view uses entity image mapping");goto fail;}}
    for(int i=0;i<parsed->surface.count;i++){
      const pet_surface_node_t *n=&parsed->surface.nodes[i];
      if(n->var_bind[0]){
        int v=find_var(parsed,n->var_bind);
        if(v<0||(n->type==2&&parsed->vars[v].type!=MINIAPP_VAR_INT)){set_error(error,error_size,"unknown or wrongly typed variable binding");goto fail;}
      }
#if PET_P4_TEXT_TABLE
      if(n->text_at[0]){
        int v=find_var(parsed,n->text_at);
        if(!parsed->text_count||v<0||parsed->vars[v].type!=MINIAPP_VAR_INT){set_error(error,error_size,"text_at requires a texts table and an int var");goto fail;}
      }
#endif
#if PET_P4_CELLS_V1
      if(n->type==PET_SURFACE_NODE_CELLS){
        /* Tiles must divide exactly so every cell draws the same whole-pixel square. */
        const pet_cells_t *g=&parsed->cells;
        if(!g->enabled||n->w%g->cols||n->h%g->rows||n->w/g->cols<4||n->h/g->rows<4){set_error(error,error_size,"cells node needs a cells grid and w/h divisible by cols/rows (tile >= 4 px)");goto fail;}
        if(n->image>=0&&(parsed->surface.image_jpeg[n->image]||n->tile_cols<2||n->tile_cols>pet_cells_max_value(g)+1||parsed->surface.image_w[n->image]%n->tile_cols)){set_error(error,error_size,"cells atlas needs 2..2^bits frames dividing the image width");goto fail;}
        const char *cursor_refs[]={n->cursor_x,n->cursor_y};
        for(int k=0;k<2;k++)if(cursor_refs[k][0]){int v=find_var(parsed,cursor_refs[k]);if(v<0||parsed->vars[v].type!=MINIAPP_VAR_INT){set_error(error,error_size,"cells cursor vars must be declared integers");goto fail;}}
      }
#endif
      if(n->state_mask&&(!parsed->input_v1||(n->state_mask>>parsed->state_count))){set_error(error,error_size,"state visibility requires input v1 and declared states");goto fail;}
      if(n->host_mask&&(!parsed->input_v1||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"host")))){set_error(error,error_size,"host visibility requires input v1 host");goto fail;}
      const char *refs[]={n->tile_var,n->tile_max_var,n->show_var};
      for(int k=0;k<3;k++)if(refs[k][0]){int v=find_var(parsed,refs[k]);if(v<0||parsed->vars[v].type!=MINIAPP_VAR_INT){set_error(error,error_size,"surface variable must be a declared integer");goto fail;}}
      if((n->action[0]&&!runtime_has_transition_action(parsed,n->action))||(n->key_for[0]&&!runtime_has_button_action(parsed,n->key_for))||(pet_surface_node_page_index(n)>=(parsed->page_count?parsed->page_count:1))){set_error(error,error_size,"unknown surface action/page");goto fail;}
#if PET_P4_DATA_ROWS
      if(n->row_bind&&(!data||!parsed->surface.data_view.enabled)){set_error(error,error_size,"row_bind requires a data list and data_view");goto fail;}
#endif
    }
#if PET_P4_APP_SHELL
    if(parsed->surface.shell&&!parsed->quota_v3){set_error(error,error_size,"app shell requires quota v3");goto fail;}
    if(parsed->surface.tabs.enabled){
      int tv=find_var(parsed,parsed->surface.tabs.var);
      if(tv<0||parsed->vars[tv].type!=MINIAPP_VAR_INT||!runtime_has_transition_action(parsed,"tab.next")||!runtime_has_transition_action(parsed,"tab.prev")){set_error(error,error_size,"tabs need an int var and tab.next/tab.prev transitions");goto fail;}
      for(unsigned t=0;t<parsed->surface.tabs.count;t++)if(parsed->surface.tabs.items[t].page>=(parsed->page_count?parsed->page_count:1)){set_error(error,error_size,"tab page is not declared");goto fail;}
    }
#endif
#if PET_P4_LONGTEXT
    for(int t=0;t<parsed->surface.text_count;t++)if(parsed->surface.texts[t].used){
      int pv=find_var(parsed,parsed->surface.texts[t].page_var),cv=parsed->surface.texts[t].count_var[0]?find_var(parsed,parsed->surface.texts[t].count_var):-2;
      if(pv<0||parsed->vars[pv].type!=MINIAPP_VAR_INT||cv==-1||(cv>=0&&(parsed->vars[cv].type!=MINIAPP_VAR_INT||cv==pv))){set_error(error,error_size,"longtext page/count vars must be distinct ints");goto fail;}
    }
#endif
#if PET_CANVAS_ENABLED
    if(parsed->surface.canvas.count&&!pet_form_source_valid(widget_json)){set_error(error,error_size,"canvas source contains unsupported NUL");goto fail;}
    if(parsed->surface.canvas.count&&parsed->surface.canvas.page!=4&&parsed->surface.canvas.page>=(parsed->page_count?parsed->page_count:1)){set_error(error,error_size,"canvas page is not declared");goto fail;}
    for(unsigned i=0;i<parsed->surface.canvas.variable_count;i++){
      int v=find_var(parsed,parsed->surface.canvas.names[i]);
      if(v<0||parsed->vars[v].type!=MINIAPP_VAR_INT){set_error(error,error_size,"canvas binding must be a declared integer");goto fail;}
    }
#endif
#if PET_P4_PARTICLES
    for(unsigned i=0;i<parsed->surface.particle_count;i++){
      const pet_surface_particles_t *e=&parsed->surface.particles[i];
      if(e->when_var[0]){int v=find_var(parsed,e->when_var);if(v<0||parsed->vars[v].type!=MINIAPP_VAR_INT){set_error(error,error_size,"particle guard must be a declared integer");goto fail;}}
      if(e->trigger_action[0]&&!runtime_has_transition_action(parsed,e->trigger_action)){set_error(error,error_size,"particle trigger must be a declared action");goto fail;}
      if(e->page!=4&&e->page>=(parsed->page_count?parsed->page_count:1)){set_error(error,error_size,"particle page is not declared");goto fail;}
    }
#endif
  }
  bool has_form=cJSON_HasObjectItem(root,"form");
  cJSON *form_buttons=has_form?cJSON_ParseWithOpts(buttons_json?buttons_json:"[]",NULL,true):NULL;
  bool form_ok;
#if PET_FORM_V3_ENABLED
  if(!strcmp(json_string(cJSON_GetObjectItemCaseSensitive(root,"form"),"engine"),PET_FORM3_ENGINE))
    form_ok=pet_form_source_valid(widget_json)&&pet_form_source_valid(buttons_json?buttons_json:"[]")&&pet_form3_parse(root,form_buttons,&parsed->form3);
  else
#endif
#if PET_FORM_V2_ENABLED
  if(!strcmp(json_string(cJSON_GetObjectItemCaseSensitive(root,"form"),"engine"),"p4-app-form-v2"))
    form_ok=pet_form_source_valid(widget_json)&&pet_form_source_valid(buttons_json?buttons_json:"[]")&&pet_form2_parse(root,form_buttons,&parsed->form,&parsed->form_text);
  else
#endif
    form_ok=(!has_form||(pet_form_source_valid(widget_json)&&pet_form_source_valid(buttons_json?buttons_json:"[]")))&&pet_form_parse(root,form_buttons,&parsed->form);
  cJSON_Delete(form_buttons);
  if(!form_ok){set_error(error,error_size,"invalid bounded form or physical action bindings");goto fail;}
  if(!parse_save(parsed,cJSON_GetObjectItemCaseSensitive(root,"persist"))){set_error(error,error_size,"invalid app save declaration");goto fail;}
#if PET_P4_CAPABILITY_EXT
  if(!miniapp_ext_validate_bindings(parsed,root,buttons_json,error,error_size))goto fail;
#endif
  if(!parse_local(parsed,root)){set_error(error,error_size,"invalid offline tools declaration");goto fail;}
#if !PET_SERVICE_BRIDGE_ENABLED
  if(cJSON_HasObjectItem(root,"host_contract")){set_error(error,error_size,"service bridge unavailable");goto fail;}
#endif
  const cJSON *host=cJSON_GetObjectItemCaseSensitive(root,"host");
  if(cJSON_HasObjectItem(root,"host_context")&&(!cJSON_IsTrue(host)||!parsed->input_v1||strcmp(json_string(root,"host_context"),"p4-host-context-v1"))){set_error(error,error_size,"invalid host context capability");goto fail;}
  if(host){
    if(!cJSON_IsTrue(host)||!parsed->surface.app||(!mixed_host&&(parsed->save_version||parsed->tick_count))||(parsed->var_count&&!parsed->surface.variable_bindings)){set_error(error,error_size,"invalid host application");goto fail;}
    const cJSON *r; cJSON_ArrayForEach(r,cJSON_GetObjectItemCaseSensitive(root,"transitions")){
      static const char *const keys[]={"from","on"};
      if(!mixed_host&&(!object_keys_allowed(r,keys,2)||strcmp(json_string(r,"from"),"*")||strncmp(json_string(r,"on"),"host.",5))){set_error(error,error_size,"invalid host action");goto fail;}
    }
    parsed->app_host=true;
  }
#if PET_HOST_COLLECTION_ENABLED
  if(parsed->surface.collection.enabled&&(!parsed->app_host||!parsed->input_v1||strcmp(json_string(root,"host_context"),"p4-host-context-v1"))){set_error(error,error_size,"host collection requires current host context");goto fail;}
#endif
#if PET_SERVICE_BRIDGE_ENABLED
  if(cJSON_HasObjectItem(root,"host_contract")&&!parsed->surface.service_bridge){set_error(error,error_size,"binding requires service bridge");goto fail;}
  if(parsed->surface.service_bridge){
    if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"host"))||strcmp(json_string(root,"host_context"),"p4-host-context-v1")||!parsed->input_v1){set_error(error,error_size,"service bridge requires a guarded host application");goto fail;}
    const char*contract=json_string(root,"host_contract");
    if(strlen(contract)!=64){set_error(error,error_size,"service bridge binding is missing");goto fail;}
    for(const char*c=contract;*c;c++)if(!((*c>='0'&&*c<='9')||(*c>='a'&&*c<='f'))){set_error(error,error_size,"invalid service bridge binding");goto fail;}
    strcpy(parsed->host_contract,contract);
    const cJSON*confirm=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"input"),"confirm");
    for(unsigned i=0;i<parsed->surface.count;i++)if(parsed->surface.nodes[i].value_param[0]&&cJSON_HasObjectItem(confirm,parsed->surface.nodes[i].action)){set_error(error,error_size,"parameter slider cannot bypass confirmation");goto fail;}
  }
#endif
  if (!parse_game(parsed, legacy_game, error, error_size)) goto fail;
  if (sprites) parsed->sprites = *sprites;
  if(parsed->surface.image_count){
    unsigned extra=0;
#if PET_P4_KEYFRAME_BG
    extra=parsed->surface.keyframes>1?parsed->surface.keyframes-1U:0U; /* appended P4J1 frames "<id>.2".."<id>.N" */
#endif
    if(parsed->surface.image_count+extra!=parsed->sprites.count){set_error(error,error_size,"surface images missing");goto fail;}
    for(int i=0;i<parsed->surface.image_count;i++){
      pet_p4_miniapp_sprite_t *im=&parsed->sprites.items[i];
      if(strcmp(im->id,parsed->surface.image_ids[i])||im->frame_width!=parsed->surface.image_w[i]||im->frame_height!=parsed->surface.image_h[i]||im->frames!=1||im->fps!=0){
        set_error(error,error_size,"surface image metadata mismatch");goto fail;
      }
    }
#if PET_P4_KEYFRAME_BG
    for(unsigned k=0;k<extra;k++){
      const pet_p4_miniapp_sprite_t *im=&parsed->sprites.items[parsed->surface.image_count+k];char id[PET_P4_MINIAPP_SPRITE_ID_MAX];
      snprintf(id,sizeof(id),"%s.%u",parsed->surface.image_ids[0],k+2U);
      if(strcmp(im->id,id)||im->frame_width!=640||im->frame_height!=480||!pet_image_jpeg(parsed->sprites.pixels+im->data_offset,im->data_length)){set_error(error,error_size,"keyframe background frame mismatch");goto fail;}
    }
#endif
  }
  if(!parsed->surface.image_count && parsed->sprites.data_length>(MINIAPP_GRAPHICS_V2(parsed)?PET_P4_MINIAPP_SPRITE_V2_PIXELS:PET_P4_MINIAPP_SPRITE_V1_PIXELS)*3U){set_error(error,error_size,"scene sprites exceed pixel budget");goto fail;}
  if(side_scene){if(!parse_side_runtime(parsed,root,scene,error,error_size))goto fail;}
  else if (!parse_bounded_scene(parsed, scene, runtime_v4, error, error_size)) goto fail;
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
  if(cJSON_HasObjectItem(root,"program")){
#if PET_COMPONENT_PROGRAM_ENABLED
    if(!pet_component_program_parse(root,&parsed->program)){set_error(error,error_size,"invalid component program or output ownership");goto fail;}
    parsed->program_seed.schema=parsed->program.image.schema;parsed->program_seed.cells=parsed->program.image.cells;
    for(unsigned i=0;i<8;i++){unsigned at=parsed->program.projection.target[i];if(at!=PET_PROGRAM_UI_UNBOUND)parsed->program_seed.outputs[i]=parsed->vars[at].int_value;}
#if PET_P4_CELLS_V1
    /* p4-cells-v1 + program v2: VM cells [0, cols*rows) mirror the grid; the init rows seed them. */
    if(parsed->cells.enabled){
      if(!parsed->program.v2||parsed->program.image.cells<(unsigned)parsed->cells.cols*parsed->cells.rows){set_error(error,error_size,"program must declare cells >= cols*rows to mirror the grid");goto fail;}
      pet_cells_export(&parsed->cells,parsed->program_seed.values,PET_PROGRAM_CELLS);
    }
#endif
    parsed->program_seed_ready=parsed->program_needs_start=true;
#if PET_COMPONENT_BACKGROUND_ENABLED
    parsed->program_content[0]=miniapp_checksum(widget_json,strlen(widget_json));
    parsed->program_content[1]=miniapp_checksum(buttons_json,strlen(buttons_json));
    parsed->program_content[2]=sprite_pack_checksum(sprites);
#endif
    if(!component_program_apply_seed(parsed)){set_error(error,error_size,"component program initial outputs are out of bounds");goto fail;}
#else
    set_error(error,error_size,"component program capability is unavailable");goto fail;
#endif
  }
#if PET_P4_LONGTEXT
  if(!longtext_writers_valid(parsed)){set_error(error,error_size,"longtext count_var conflicts with another writer");goto fail;}
#endif
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

/* Invalidates immediately, including a scroll-away-and-back before the next
 * frame. Changes beyond an already-clamped edge do not invalidate input. */
static void viewport_offset_changed(miniapp_runtime_t *runtime,const char *name,int32_t before,int32_t after){
  if(runtime!=&g_runtime||runtime_host_variable(runtime,find_var(runtime,name)))return;
  int count=runtime->surface.viewport_count?runtime->surface.viewport_count:runtime->surface.viewport.enabled?1:0;
  for(int i=0;i<count;i++){
    const pet_surface_viewport_t *v=runtime->surface.viewport_count?&runtime->surface.viewports[i]:&runtime->surface.viewport;
    if(!strcmp(v->offset_var,name)&&pet_surface_clamp_offset(v,before)!=pet_surface_clamp_offset(v,after)){g_viewport_epoch++;g_pressed_node=-1;return;}
  }
}
static bool apply_effects(miniapp_runtime_t *runtime, const miniapp_effect_t *effects, int count) {
  bool changed = false;
  int32_t original[MINIAPP_MAX_VARS];for(int i=0;i<runtime->var_count;i++)original[i]=runtime->vars[i].int_value;
  for (int i = 0; i < count; i += 1) {
    const miniapp_effect_t *effect = &effects[i];
    miniapp_var_t *var = &runtime->vars[effect->var_index];
    if (var->type == MINIAPP_VAR_INT) {
      /* Sources read the pre-effect snapshot, for both set and p4-logic-v1 inc. */
      long long amount = effect->source_var >= 0 ? original[effect->source_var] : effect->int_value;
      int32_t next = effect->op == MINIAPP_EFFECT_INC
        ? clamp_i64((long long) var->int_value + amount) : clamp_i64(amount);
      if(next<var->int_min)next=var->int_min;if(next>var->int_max)next=var->int_max;
      if (next != var->int_value) {
        viewport_offset_changed(runtime,var->name,var->int_value,next);
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
  if (game->start_action[0] && strcmp(action, game->start_action) == 0) {
    *command = PET_P4_GAME_COMMAND_START;
  } else if (game->place_action[0] && strcmp(action, game->place_action) == 0) {
    *command = PET_P4_GAME_COMMAND_PLACE;
  } else if (game->undo_action[0] && strcmp(action, game->undo_action) == 0) {
    *command = PET_P4_GAME_COMMAND_UNDO;
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

/* p4-logic-v1: scene rules read/write runtime int vars through the engine's
 * bounded mirror. Slot index equals the runtime var declaration index. */
static void scene_push_vars(miniapp_runtime_t *runtime) {
  if (!runtime->logic || !runtime->game.active) return;
  int count = runtime->var_count < PET_P4_GAME_MAX_VARS ? runtime->var_count : PET_P4_GAME_MAX_VARS;
  for (int i = 0; i < count; i += 1) {
    runtime->game.engine.vars[i] = runtime->vars[i].type == MINIAPP_VAR_INT ? runtime->vars[i].int_value : 0;
  }
  runtime->game.engine.var_count = (uint8_t) count;
}

static bool scene_pull_vars(miniapp_runtime_t *runtime) {
  bool changed = false;
  if (!runtime->logic || !runtime->game.active) return false;
  for (int i = 0; i < runtime->game.engine.var_count; i += 1) {
    miniapp_var_t *var = &runtime->vars[i];
    int32_t next = runtime->game.engine.vars[i];
    if (var->type != MINIAPP_VAR_INT) continue;
    if (next < var->int_min) next = var->int_min;
    if (next > var->int_max) next = var->int_max;
    if (next != var->int_value) {
      viewport_offset_changed(runtime, var->name, var->int_value, next);
      var->int_value = next;
      changed = true;
    }
  }
  return changed;
}

static bool sync_game_runtime(miniapp_runtime_t *runtime) {
  bool changed = false;
  miniapp_game_t *game = &runtime->game;
  if (!game->active) return false;
  changed |= scene_pull_vars(runtime);
  if (runtime->sfx && game->engine.sfx_pending) { runtime->sfx_pending |= game->engine.sfx_pending; game->engine.sfx_pending = 0; }
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

static bool dispatch_action_observed(const char *action,uint64_t now_ms,const pet_host_observation_t *observed) {
  bool handled = false;
  if (!action || !action[0]) return false;
  bool media=false;
  char confirmed[48],confirmed_context[65]={0};
#if PET_HOST_COLLECTION_ENABLED
  pet_host_observation_t collection_observed;
#endif
  miniapp_runtime_lock();
#if PET_HOST_COLLECTION_ENABLED
  if(g_runtime.active&&g_runtime.app_host&&g_runtime.surface.collection.enabled){
    const pet_host_collection_config_t *c=&g_runtime.surface.collection;
    for(unsigned i=0;i<c->visible;i++)if(!strcmp(action,c->actions[i])){
      if(!pet_app_host_collection_observe(g_runtime.widget_id,c,i,now_ms,&collection_observed)
          ||(observed&&!pet_host_observation_equal(observed,&collection_observed))){miniapp_runtime_unlock();return true;}
      observed=&collection_observed;break;
    }
  }
#endif
  /* A pointer may have been released before a host reply or runtime switch.
   * Never reinterpret its old action against a newer host/local component. */
  if(observed&&(!g_runtime.active||!g_runtime.app_host||observed->revision!=g_image_revision
      ||strcmp(observed->component_id,g_runtime.widget_id)
      ||!pet_app_host_observation_current(g_runtime.widget_id,observed))){miniapp_runtime_unlock();return true;}
  uint64_t context=((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;
  if(g_confirm_action[0]&&(now_ms>=g_confirm_until||context!=g_confirm_context))g_confirm_action[0]=0;
#if PET_VARIABLE_OWNERS_ENABLED
  if(g_confirm_action[0]&&g_runtime.variable_owners.mixed&&g_confirm_host_observation.valid
      &&!pet_app_host_owned_observation_current(g_runtime.widget_id,&g_confirm_host_observation,now_ms))g_confirm_action[0]=0;
#endif
  bool approved=false;
  if(g_confirm_action[0]){
    bool yes=!strcmp(action,"input.confirm"),no=!strcmp(action,"input.cancel");
    for(int i=0;i<g_runtime.button_count;i++)if(!strcmp(action,g_runtime.buttons[i].action)){
      yes|=!strcmp(g_runtime.buttons[i].event,"button.sw1.short_press");
      no|=!strcmp(g_runtime.buttons[i].event,"button.sw2.short_press");
    }
    if(no){g_confirm_action[0]=0;g_runtime.view.revision++;miniapp_runtime_unlock();return true;}
    if(!yes){miniapp_runtime_unlock();return true;}
    copy_utf8(confirmed,sizeof(confirmed),g_confirm_action);copy_utf8(confirmed_context,sizeof(confirmed_context),g_confirm_host_token);action=confirmed;approved=true;g_confirm_action[0]=0;g_runtime.view.revision++;
#if PET_VARIABLE_OWNERS_ENABLED
    if(g_runtime.variable_owners.mixed&&g_confirm_host_observation.valid)observed=&g_confirm_host_observation;
#endif
  }
  if(g_runtime.active&&g_runtime.input_v1&&!approved&&runtime_action_enabled(&g_runtime,action)){
    if(runtime_host_action(&g_runtime,action)&&!pet_app_host_enabled(g_runtime.widget_id,action,now_ms)){miniapp_runtime_unlock();return true;}
    for(int i=0;i<3;i++)if(g_runtime.confirm_actions[i][0]&&!strcmp(action,g_runtime.confirm_actions[i])){
      copy_utf8(g_confirm_action,48,action);copy_utf8(g_confirm_prompt,121,g_runtime.confirm_prompts[i]);
      g_confirm_host_token[0]=0;if(runtime_host_action(&g_runtime,action)){
        if(observed)copy_utf8(g_confirm_host_token,sizeof(g_confirm_host_token),observed->context);
        else pet_app_host_context(g_runtime.widget_id,g_confirm_host_token);
      }
#if PET_VARIABLE_OWNERS_ENABLED
      memset(&g_confirm_host_observation,0,sizeof(g_confirm_host_observation));
      if(g_runtime.variable_owners.mixed&&(observed||runtime_action_host_dependent(&g_runtime,action))){
        if(observed)g_confirm_host_observation=*observed;
        else pet_app_host_observe(g_runtime.widget_id,&g_confirm_host_observation);
        if(!g_confirm_host_observation.valid||!pet_app_host_owned_observation_current(g_runtime.widget_id,&g_confirm_host_observation,now_ms)){
          g_confirm_action[0]=0;miniapp_runtime_unlock();return false;
        }
      }
#endif
      g_confirm_until=now_ms+30000;g_confirm_context=context;g_runtime.view.revision++;
      if(g_runtime.game.engine.bounded.motion){
        pet_p4_game_release(&g_runtime.game.engine,true,now_ms);
        sync_game_runtime(&g_runtime);refresh_view(&g_runtime);
      }
      miniapp_runtime_unlock();return true;
    }
  }
  if(g_runtime.active&&g_runtime.input_v1&&!runtime_action_enabled(&g_runtime,action)){
    miniapp_runtime_unlock();return false;
  }
#if PET_COMPONENT_PROGRAM_ENABLED
  if(g_runtime.active&&g_runtime.program.enabled){
    int32_t values[8];pet_program_event_t event;
#if PET_VARIABLE_OWNERS_ENABLED
    if(!component_program_action_values(&g_runtime,action,values,observed,now_ms)){miniapp_runtime_unlock();return false;}
#else
    component_program_values(&g_runtime,values);
#endif
    if(pet_component_program_event(&g_runtime.program,action,values,&event)){
#if PET_COMPONENT_BACKGROUND_ENABLED
      bool accepted=false;
      if(background_enqueue_locked(action,values,&accepted)){if(accepted)runtime_action_accepted(&g_runtime,action,now_ms);miniapp_runtime_unlock();return accepted;}
#endif
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
      bool held __attribute__((cleanup(component_parent_leave)))=component_parent_enter();
      if(!held){miniapp_runtime_unlock();return false;}
#endif
      bool foreground_accepted=g_component_program_initialized
        &&pet_component_program_session_enqueue(&g_component_program,g_image_revision,&event)==PET_RUNTIME_OK;
      if(foreground_accepted)runtime_action_accepted(&g_runtime,action,now_ms);
      miniapp_runtime_unlock();return foreground_accepted;
    }
  }
#endif
  int form_op=form_operation(&g_runtime,action);
#if PET_FORM_V3_ENABLED
  if(g_runtime.active&&form_op>=0&&g_runtime.form3.enabled){
    pet_form3_value_t *values=form3_workspace_begin();form3_values(&g_runtime,values);
    bool ok=pet_form3_apply(&g_runtime.form3,values,g_runtime.current_page,form_op);
    if(ok)form3_write(&g_runtime,values);
    form3_workspace_end();if(ok)refresh_view(&g_runtime);
    miniapp_runtime_unlock();return ok;
  }
#endif
#if PET_FORM_V2_ENABLED
  if(g_runtime.active&&form_op>=0&&g_runtime.form_text.enabled){
    pet_form_value_t values[PET_FORM_VARS];form2_values(&g_runtime,values);
    bool ok=pet_form2_apply(&g_runtime.form,&g_runtime.form_text,values,g_runtime.current_page,form_op);
    if(ok){form2_write(&g_runtime,values);refresh_view(&g_runtime);}
    miniapp_runtime_unlock();return ok;
  }
#endif
  if(g_runtime.active&&form_op>=0){
    int32_t values[PET_FORM_VARS];form_values(&g_runtime,values);
    bool ok=pet_form_apply(&g_runtime.form,values,g_runtime.current_page,form_op);
    if(ok){form_write(&g_runtime,values);refresh_view(&g_runtime);}
    miniapp_runtime_unlock();return ok;
  }
  if(g_runtime.active&&runtime_host_action(&g_runtime,action)){
    char id[48];copy_utf8(id,sizeof(id),g_runtime.widget_id);uint32_t rev=g_image_revision;uint8_t page=g_runtime.current_page<0?0:g_runtime.current_page;
    bool declared=runtime_has_transition_action(&g_runtime,action);miniapp_runtime_unlock();
    return declared&&(approved?pet_app_host_confirmed_action(id,rev,page,action,confirmed_context,now_ms):observed?pet_app_host_observed_action(id,rev,page,action,observed,now_ms):pet_app_host_action(id,rev,page,action,now_ms));
  }
  if(g_runtime.active&&g_runtime.side_enabled){
    int op=-1;for(int i=0;i<4;i++)if(!strcmp(action,g_runtime.side_actions[i]))op=i;
    if(op==0||op==1){pet_side_axis(&g_runtime.side,op==0?-1:1);g_runtime.side_axis_until=now_ms+160;}
    else if(op==2){
      if(g_runtime.side.phase==PET_SIDE_READY){unsigned checkpoint=(unsigned)g_runtime.vars[g_runtime.side_vars[3]].int_value;pet_side_restart(&g_runtime.side,now_ms);if(checkpoint&&!pet_side_restore_checkpoint(&g_runtime.side,checkpoint)){g_runtime.side.phase=PET_SIDE_READY;miniapp_runtime_unlock();return false;}}
      pet_side_jump(&g_runtime.side,true,now_ms);g_runtime.side_jump_until=now_ms+120;
    }else if(op==3){pet_side_pause(&g_runtime.side,g_runtime.side.phase!=PET_SIDE_PAUSED,now_ms);g_runtime.side_axis_live=false;}
    if(op>=0){sync_side_runtime(&g_runtime);refresh_view(&g_runtime);runtime_action_accepted(&g_runtime,action,now_ms);}
    miniapp_runtime_unlock();return op>=0;
  }
  if(g_runtime.active&&g_runtime.local.enabled){
    int32_t values[PET_LOCAL_VARS];local_values(&g_runtime,values,false);
    bool ok=pet_local_action(&g_runtime.local,&g_runtime.local_state,values,action,now_ms,local_wall_seconds(now_ms),esp_random());
    if(ok){local_values(&g_runtime,values,true);refresh_local_view(&g_runtime,now_ms);runtime_action_accepted(&g_runtime,action,now_ms);}
    miniapp_runtime_unlock();return ok;
  }
  if(g_runtime.active&&g_runtime.slicing) {
    if(!pet_p4_touch_ready()){miniapp_runtime_unlock();return false;}
    if(!strcmp(action,"slice.restart")) {pet_fruit_start_or_resume(&g_runtime.fruit,now_ms,(uint32_t)now_ms);handled=true;}
    else if(!strcmp(action,"slice.pause")) {pet_fruit_pause(&g_runtime.fruit,now_ms);handled=true;}
    if(handled)runtime_action_accepted(&g_runtime,action,now_ms);
    miniapp_runtime_unlock();
    return handled;
  }
  if(g_runtime.active&&g_runtime.instrument) {
    miniapp_runtime_unlock();
    return !strcmp(action,"instrument.strike") && pet_p4_audio_instrument_strike(now_ms);
  }
  if(g_runtime.active&&g_runtime.media) {
    for(int i=0;i<g_runtime.transition_count;i++)if(!strcmp(g_runtime.transitions[i].action,action))media=true;
    if(!media) {miniapp_runtime_unlock();return false;}
  }
  miniapp_runtime_unlock();
  if(media){char source[48];if(pet_p4_miniapp_media_config(source,NULL))return pet_p4_media_live_action(source,action);return pet_p4_media_action(action);}
  miniapp_runtime_lock();
  if (!g_runtime.active) goto done;
#if PET_P4_APP_SHELL
  {
    /* p4-app-shell-v1 reserved actions: switch the active tab, jump to its page and
     * clear the navigation history. Their wildcard transitions carry no effects. */
    int tab_op=pet_shell_tab_operation(action);
    if(tab_op!=PET_SHELL_TAB_NONE){
      if(g_runtime.surface.tabs.enabled){
        int tab_var=find_var(&g_runtime,g_runtime.surface.tabs.var);
        if(tab_var>=0&&g_runtime.vars[tab_var].type==MINIAPP_VAR_INT){
          int32_t value=g_runtime.vars[tab_var].int_value;
          form_cancel(&g_runtime);
          if(pet_shell_tab_apply(&g_runtime.surface.tabs,tab_op,&value,&g_runtime.current_page,&g_runtime.navigation_history)){
            g_runtime.vars[tab_var].int_value=value;g_input_epoch++;g_viewport_epoch++;g_pressed_node=-1;
            refresh_view(&g_runtime);handled=true;
          }
        }
      }
      goto done;
    }
  }
#endif
  for (int i = 0; i < g_runtime.transition_count; i += 1) {
    miniapp_transition_t *transition = &g_runtime.transitions[i];
    bool changed = false;
    pet_p4_game_command_t game_command;
    bool taken;
    if(!runtime_transition_branch(&g_runtime,transition,action,&taken))continue;
    const int8_t to_state = taken ? transition->to_state : transition->else_to_state;
    const int8_t page_op = taken ? transition->page_op : transition->else_page_op;
    const miniapp_effect_t *effects = taken ? transition->effects : transition->effects + transition->effect_count;
    const int effect_count = taken ? transition->effect_count : transition->else_effect_count;
    if(g_runtime.navigation){
      changed|=form_cancel(&g_runtime);
      if(taken){
        int8_t previous_page=g_runtime.current_page;
        pet_nav_apply(&g_runtime.navigation_history,&g_runtime.current_page,&transition->navigation_rule);
        if(transition->navigation_rule.op!=PET_NAV_NONE){g_input_epoch++;changed=true;}
        if(previous_page!=g_runtime.current_page)changed=true;
      }
    }
    else changed|=form_cancel(&g_runtime);
    bool resuming=false;
    if(g_runtime.game.active&&g_runtime.pause_action[0]&&!strcmp(action,g_runtime.pause_action)
        &&transition->to_state==g_runtime.game.playing_state&&g_runtime.current_state!=g_runtime.game.playing_state)
      for(int t=0;t<g_runtime.transition_count;t++){
        const miniapp_transition_t *p=&g_runtime.transitions[t];
        if(g_runtime.pause_action[0]&&!strcmp(p->action,g_runtime.pause_action)&&p->from_state==g_runtime.current_state&&p->to_state==g_runtime.game.playing_state)resuming=true;
      }
    if (to_state >= 0 && to_state != g_runtime.current_state) {
      pet_p4_game_release(&g_runtime.game.engine,true,now_ms);
      g_runtime.current_state = to_state;
      // Paused time must never be replayed as catch-up motion on resume.
      if(g_runtime.game.active)g_runtime.game.engine.last_tick_ms=now_ms;
      for(int t=0;t<g_runtime.tick_count;t++)g_runtime.ticks[t].last_ms=now_ms;
      changed = true;
    }
    if (page_op != -1 && g_runtime.page_count > 0) {
      if(g_runtime.navigation)g_runtime.navigation_history.depth=0;
      int next = g_runtime.current_page;
      if (page_op == -2) next = (next + 1) % g_runtime.page_count;
      else if (page_op == -3) next = (next + g_runtime.page_count - 1) % g_runtime.page_count;
      else next = page_op;
      if (next != g_runtime.current_page) {
        g_runtime.current_page = (int8_t) next;
        changed = true;
      }
    }
    changed |= apply_effects(&g_runtime, effects, effect_count);
#if PET_P4_CELLS_V1
    changed |= apply_cell_ops(&g_runtime, taken ? transition->cell_ops : transition->cell_ops + transition->cell_op_count,
      taken ? transition->cell_op_count : transition->else_cell_op_count);
#endif
#if PET_P4_NOTIFY
    if(taken&&transition->notify)miniapp_queue_notice(&g_runtime,now_ms);
#endif
#if PET_P4_DATA_ROWS
    if(taken&&!strcmp(action,g_runtime.data_rows.refresh_action))miniapp_queue_refresh(&g_runtime,now_ms);
#endif
    if (transition->sfx_cue >= 0) g_runtime.sfx_pending |= (uint8_t) (1u << transition->sfx_cue);
#if PET_P4_CAPABILITY_EXT
    miniapp_ext_fire(&g_runtime, &transition->cext);
#endif
    /* A committed field may change through an ordinary allowed transition.
     * Invalidate an old DOWN even if a later transition restores its value. */
    if(changed&&g_runtime.form.enabled){g_viewport_epoch++;g_pressed_node=-1;}
#if PET_FORM_V3_ENABLED
    if(changed&&g_runtime.form3.enabled){g_viewport_epoch++;g_pressed_node=-1;}
#endif
    if (!resuming&&g_runtime.game.active
        && g_runtime.game.engine.kind == PET_P4_GAME_BOUNDED) {
      scene_push_vars(&g_runtime);
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
  if(handled)runtime_action_accepted(&g_runtime,action,now_ms);
#if PET_P4_PARTICLES
  if(handled)pet_p4_renderer_note_action(action,g_image_revision); /* decorative emitters only observe accepted actions */
#endif
  miniapp_runtime_unlock();
  return handled;
}
bool pet_p4_miniapp_dispatch_action(const char *action,uint64_t now_ms){
  return dispatch_action_observed(action,now_ms,NULL);
}

static bool button_matches_event(const char *binding, const char *event_name) {
  if (strcmp(binding, event_name) == 0) return true;
  return strcmp(binding, "knob.rotate_cw / knob.rotate_ccw") == 0
    && (strcmp(event_name, "knob.rotate_cw") == 0 || strcmp(event_name, "knob.rotate_ccw") == 0);
}

bool pet_p4_miniapp_has_input(const char *event_name) {
  bool found = false;
  if (!event_name) return false;
  miniapp_runtime_lock();
  if (!g_runtime.active) goto done;
  for (int i = 0; i < g_runtime.button_count; i += 1) {
    if (button_matches_event(g_runtime.buttons[i].event, event_name)) {
      found = true;
      break;
    }
  }
done:
  miniapp_runtime_unlock();
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
  miniapp_runtime_lock();
  if (!g_runtime.active) goto done;
  for (int i = 0; i < g_runtime.button_count; i += 1) {
    if (!button_matches_event(g_runtime.buttons[i].event, event_name)) continue;
    copy_utf8(resolved_action, sizeof(resolved_action), g_runtime.buttons[i].action);
    break;
  }
done:
  miniapp_runtime_unlock();
  if (!resolved_action[0]) return false;
  if (action && action_size > 0) copy_utf8(action, action_size, resolved_action);
  return true;
}

static uint64_t hold_context(void){return ((uint64_t)g_input_epoch<<56)|((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;}
bool pet_p4_miniapp_hold_input(const char *event_name){
  char action[48];if(!pet_p4_miniapp_resolve_input(event_name,action,sizeof(action)))return false;
  miniapp_runtime_lock();bool yes=g_runtime.hold_action[0]&&!strcmp(action,g_runtime.hold_action);miniapp_runtime_unlock();return yes;
}
bool pet_p4_miniapp_hold_event(const char *action,int phase,unsigned owner,uint64_t now_ms){
  if(owner<1||owner>2||phase<0||phase>2)return false;
  miniapp_runtime_lock();
  if(phase){
    bool accepted=g_hold_owner==owner;
    if(accepted){if(g_runtime.side_enabled){pet_side_jump(&g_runtime.side,false,now_ms);g_runtime.side_jump_until=0;sync_side_runtime(&g_runtime);}else{pet_p4_game_release(&g_runtime.game.engine,phase==2||g_hold_context!=hold_context()||!g_input_visible,now_ms);sync_game_runtime(&g_runtime);}g_hold_owner=0;refresh_view(&g_runtime);}
    miniapp_runtime_unlock();return accepted;
  }
  if(g_hold_owner&&g_hold_context!=hold_context()){if(g_runtime.side_enabled)pet_side_jump(&g_runtime.side,false,now_ms);else pet_p4_game_release(&g_runtime.game.engine,true,now_ms);g_hold_owner=0;}
  bool accepted=!g_hold_owner&&g_runtime.active&&action&&g_runtime.hold_action[0]&&!strcmp(action,g_runtime.hold_action);
  unsigned revision=g_image_revision;
  miniapp_runtime_unlock();
  if(!accepted||!pet_p4_miniapp_dispatch_action(action,now_ms))return false;
  miniapp_runtime_lock();
  accepted=revision==g_image_revision&&(g_runtime.side_enabled?g_runtime.side.jump_down:g_runtime.game.engine.charging);
  if(accepted){g_hold_owner=owner;g_hold_context=hold_context();}
  miniapp_runtime_unlock();return accepted;
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
#if PET_P4_H264_PREFETCH
  bool native_window=pet_p4_renderer_program_window_begin();
#endif
  char pause[PET_P4_MINIAPP_ACTION_MAX]={0};
  miniapp_runtime_lock();
  catalog_cursor_retry(now_ms);
  if(g_input_visible!=visible){g_input_visible=visible;g_input_epoch++;g_confirm_action[0]=0;g_pressed_node=-1;pet_p4_game_release(&g_runtime.game.engine,true,now_ms);}
  if(!visible&&g_runtime.active&&g_runtime.game.active
     &&g_runtime.current_state==g_runtime.game.playing_state)
    copy_utf8(pause,sizeof(pause),g_runtime.pause_action);
  miniapp_runtime_unlock();
  if(pause[0])pet_p4_miniapp_dispatch_action(pause,now_ms);
  pet_p4_media_tick(now_ms);
#if PET_P4_EVENTS
  miniapp_ext_process(now_ms, visible);
#endif
  bool changed = false;
  miniapp_runtime_lock();
#if PET_P4_H264_PREFETCH
  g_program_slice_admitted=native_window;
#endif
#if PET_P4_NATIVE_WORK_WINDOW
  /* Pure native work only, AFTER media_tick and model acquisition. A denied
   * try never blocks the UI, host/media ticks, retirement or cancellation. */
  bool work_window=pet_native_work_window_try_begin();
#if PET_P4_H264_PREFETCH
  g_program_slice_admitted=native_window&&work_window;
#else
  g_program_slice_admitted=work_window;
#endif
#endif
#if PET_COMPONENT_BACKGROUND_ENABLED
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
  /* Stop hidden foreground BEFORE the shared pump can select it. The storage
   * publication gap is not a real hide of a newly prepared lifecycle. */
  component_program_visibility_locked(visible);
#endif
  background_process_locked(now_ms);
#endif
  if (!g_runtime.active) {
#if PET_P4_NATIVE_WORK_WINDOW
    if(work_window){bool released=pet_native_work_window_end();configASSERT(released);(void)released;}
#endif
    goto done;
  }
#if PET_COMPONENT_PROGRAM_ENABLED
  component_program_process_locked(now_ms,visible);
#endif
#if PET_P4_NATIVE_WORK_WINDOW
  if(work_window){bool released=pet_native_work_window_end();configASSERT(released);(void)released;}
#endif
  if(!visible)changed|=form_cancel(&g_runtime);
#if PET_COMPONENT_PROGRAM_ENABLED
  /* New foreground program profile has one activation lifetime. Its ordinary
   * ticks cannot keep changing the model after hide/cancel. Legacy components
   * retain their existing background timer behavior. Do not catch up hidden
   * time if the parent explicitly opens this foreground profile again. */
  if(g_runtime.program.enabled&&g_runtime.program_suspended){
    for(int i=0;i<g_runtime.tick_count;i++)g_runtime.ticks[i].last_ms=now_ms;
    if(changed)refresh_view(&g_runtime);
    goto done;
  }
#endif
  if(g_runtime.side_enabled){
    if(g_hold_owner&&g_hold_context!=hold_context()){pet_side_jump(&g_runtime.side,false,now_ms);g_hold_owner=0;}
    if(!visible&&g_runtime.side.phase==PET_SIDE_RUNNING){pet_side_pause(&g_runtime.side,true,now_ms);g_runtime.side_axis_live=false;}
    if(!g_runtime.side_axis_live&&g_runtime.side_axis_until&&now_ms>=g_runtime.side_axis_until){pet_side_axis(&g_runtime.side,0);g_runtime.side_axis_until=0;}
    if(!g_hold_owner&&g_runtime.side_jump_until&&now_ms>=g_runtime.side_jump_until){pet_side_jump(&g_runtime.side,false,now_ms);g_runtime.side_jump_until=0;}
    int64_t began=esp_timer_get_time();bool moved=pet_side_process(&g_runtime.side,now_ms);bool updated=sync_side_runtime(&g_runtime);
    if(moved){g_runtime.side_last_step_us=(uint32_t)(esp_timer_get_time()-began);if(g_runtime.side_last_step_us>g_runtime.side_max_step_us)g_runtime.side_max_step_us=g_runtime.side_last_step_us;}
    if(moved||updated)refresh_view(&g_runtime);
    goto done;
  }
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
#if PET_P4_CELLS_V1
      changed |= apply_cell_ops(&g_runtime, tick->cell_ops, tick->cell_op_count);
#endif
      if (tick->when_count && conditions_hold(&g_runtime, tick->when, tick->when_count)) {
        if (tick->completion_state >= 0 && tick->completion_state != g_runtime.current_state) {
          g_runtime.current_state = tick->completion_state;
          changed = true;
        }
        changed |= apply_effects(
          &g_runtime,
          tick->completion_effects,
          tick->completion_effect_count
        );
#if PET_P4_CELLS_V1
        changed |= apply_cell_ops(&g_runtime, tick->completion_cell_ops, tick->completion_cell_op_count);
#endif
#if PET_P4_NOTIFY
        if(tick->completion_notify)miniapp_queue_notice(&g_runtime,now_ms);
#endif
        if (tick->completion_sfx_cue >= 0) g_runtime.sfx_pending |= (uint8_t) (1u << tick->completion_sfx_cue);
#if PET_P4_CAPABILITY_EXT
        miniapp_ext_fire(&g_runtime, &tick->completion_cext);
#endif
        for (int reset = 0; reset < g_runtime.tick_count; reset += 1) {
          g_runtime.ticks[reset].last_ms = now_ms;
        }
        completed = true;
        break;
      }
    }
    if (!completed) tick->last_ms += (uint64_t) steps * tick->every_ms;
  }
  bool motion_confirm=(g_runtime.game.engine.bounded.motion||g_runtime.game.engine.bounded.bodies)&&g_confirm_action[0]
    &&now_ms<g_confirm_until&&g_confirm_context==(((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page);
  if(g_runtime.game.active&&(!visible||motion_confirm||g_runtime.current_state!=g_runtime.game.playing_state))
    g_runtime.game.engine.last_tick_ms=now_ms;
  if (visible && !motion_confirm && g_runtime.game.active
      && g_runtime.current_state == g_runtime.game.playing_state) {
    scene_push_vars(&g_runtime);
    changed |= pet_p4_game_process(&g_runtime.game.engine, now_ms);
    changed |= sync_game_runtime(&g_runtime);
  }
  if (changed) refresh_view(&g_runtime);
  if(g_runtime.local.enabled){
    int32_t values[PET_LOCAL_VARS];local_values(&g_runtime,values,false);
    bool update=pet_local_tick(&g_runtime.local,&g_runtime.local_state,values,now_ms,local_wall_seconds(now_ms));
    local_values(&g_runtime,values,true);
    if(update||!g_runtime.view.eyebrow[0])refresh_local_view(&g_runtime,now_ms);
  }
done:
  miniapp_runtime_unlock();
#if PET_P4_H264_PREFETCH
  if(native_window)pet_p4_renderer_program_window_end();
#endif
  static uint64_t saved_at;
  if(now_ms-saved_at>=1000){saved_at=now_ms;save_app_runtime();}
}

bool pet_p4_miniapp_active(void) {
  bool active;
  miniapp_runtime_lock();
  active = g_runtime.active;
  miniapp_runtime_unlock();
  return active;
}

bool pet_p4_miniapp_program_status(pet_p4_miniapp_program_status_t *out){
  if(!out)return false;memset(out,0,sizeof(*out));
#if PET_COMPONENT_PROGRAM_ENABLED
  miniapp_runtime_lock();bool current=g_runtime.active&&g_runtime.program.enabled;
#if PET_COMPONENT_BACKGROUND_ENABLED
  if(current&&background_status_current_locked(out)){miniapp_runtime_unlock();return true;}
  if(current&&g_runtime.program_background_owned){out->phase=PET_RUNTIME_STOPPED;out->revision=g_runtime.program_seed.revision;miniapp_runtime_unlock();return true;}
#endif
  if(current){
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
    bool held __attribute__((cleanup(component_parent_leave)))=component_parent_enter();
    if(!held){miniapp_runtime_unlock();return false;}
#endif
    /* A failed/new activation keeps the old committed session readable for
     * recovery. Its report/timing never belongs to the new UI revision. */
    bool attempted=!g_runtime.program_needs_start&&g_component_program_attempt_revision==g_image_revision;
    bool owner=attempted&&g_component_program_start_result==PET_RUNTIME_OK&&g_component_program.revision==g_image_revision;
    out->phase=(uint8_t)(owner?g_component_program.phase:attempted?g_component_program_start_result:PET_RUNTIME_IDLE);
    out->live=owner&&g_component_program.live;out->retiring=owner&&g_component_program.retiring;
    if(owner){out->slice_budget_us=(uint16_t)g_component_program.host.slice_budget_us;out->slice_stalled=g_component_program.host.slice_stalled;}
    if(attempted){out->last_step_us=g_component_program_last_us;out->max_step_us=g_component_program_max_us;}
    out->report_valid=owner&&g_component_program.report_valid&&g_component_program.last_report.generation==g_component_program.generation;
    if(out->report_valid){out->revision=g_component_program.last_report.revision;out->used_fuel=g_component_program.last_report.used_fuel;out->result=(uint8_t)g_component_program.last_report.result;}
  }
  miniapp_runtime_unlock();return current;
#else
  return false;
#endif
}

bool pet_p4_miniapp_slicing_active(void) {
  miniapp_runtime_lock();
  bool active=g_runtime.active&&g_runtime.slicing;
  miniapp_runtime_unlock();return active;
}
bool pet_p4_miniapp_fruit_snapshot(pet_fruit_t *out) {
  miniapp_runtime_lock();
  bool active=g_runtime.active&&g_runtime.slicing;
  if(active&&out)*out=g_runtime.fruit;
  miniapp_runtime_unlock();return active;
}
void pet_p4_miniapp_fruit_process(uint64_t now_ms,bool visible) {
  unsigned sounds=0,volume=0;
  bool audible=false;
  miniapp_runtime_lock();
  if(g_runtime.active&&g_runtime.slicing) {
    if(visible)pet_fruit_tick(&g_runtime.fruit,now_ms);
    else pet_fruit_suspend(&g_runtime.fruit,now_ms);
    sounds=g_runtime.fruit.sounds;volume=g_runtime.fruit.sound_volume;g_runtime.fruit.sounds=0;
    audible=visible&&g_runtime.fruit.phase!=PET_FRUIT_PAUSED;
  }
  miniapp_runtime_unlock();
  pet_p4_audio_fruit_sound(audible?sounds:0,volume,audible);
}
void pet_p4_miniapp_sfx_process(uint64_t now_ms, bool audible) {
  (void) now_ms;
  unsigned pending = 0, volume = 0;
  bool active = false;
  uint8_t presets[PET_SFX_MAX_CUES];
  miniapp_runtime_lock();
  if (g_runtime.active && g_runtime.sfx) {
    active = audible;
    volume = g_runtime.sfx_volume;
    pending = active ? g_runtime.sfx_pending : 0;
    g_runtime.sfx_pending = 0;
    memcpy(presets, g_runtime.sfx_cue_preset, sizeof(presets));
  }
#if PET_P4_CAPABILITY_EXT
  pet_cext_audio_t ext_audio; static pet_seq_program_t ext_program;
  pet_cext_audio_take(&g_runtime.cext, g_runtime.active && audible, &ext_audio);
  if (ext_audio.sequence_active) ext_program = g_runtime.cext.program;
#endif
  miniapp_runtime_unlock();
  pet_p4_audio_sfx_set(active, volume);
#if PET_P4_CAPABILITY_EXT
  miniapp_ext_audio_apply(&ext_audio, &ext_program);
#endif
  for (unsigned i = 0; i < PET_SFX_MAX_CUES; i += 1) if (pending & (1u << i)) (void) pet_p4_audio_sfx_trigger(presets[i]);
}
bool pet_p4_miniapp_pointer_active(void){
  miniapp_runtime_lock();
  bool active=g_runtime.active&&(g_runtime.surface.app||g_runtime.surface.media_touch||(g_runtime.instrument&&g_runtime.input_v1));
  miniapp_runtime_unlock();return active;
}
bool pet_p4_miniapp_input_policy(const char *event_name,bool press) {
  bool enabled=false;
  miniapp_runtime_lock();
  /* Only explicit opt-in permits repeated mutations. */
  if(g_runtime.active&&g_runtime.input_v1)for(int i=0;i<g_runtime.button_count;i++)
    if(!strcmp(event_name,g_runtime.buttons[i].event)){
      enabled=((press?g_runtime.press_mask:g_runtime.repeat_mask)&(1u<<i))!=0;
      break;
    }
  miniapp_runtime_unlock();return enabled;
}
uint64_t pet_p4_miniapp_input_context(void) {
  miniapp_runtime_lock();
  uint64_t context=((uint64_t)g_image_revision<<24)|((uint64_t)g_input_epoch<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;
  miniapp_runtime_unlock();return context;
}
static bool confirmation_locked(char prompt[121],uint64_t now_ms) {
  uint64_t context=((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;
  bool active=g_confirm_action[0]&&now_ms<g_confirm_until&&context==g_confirm_context;
#if PET_VARIABLE_OWNERS_ENABLED
  if(active&&g_runtime.variable_owners.mixed&&g_confirm_host_observation.valid
      &&!pet_app_host_owned_observation_current(g_runtime.widget_id,&g_confirm_host_observation,now_ms))active=false;
#endif
  if(!active)g_confirm_action[0]=0;
  if(active&&prompt)copy_utf8(prompt,121,g_confirm_prompt);
  return active;
}
bool pet_p4_miniapp_confirmation(char prompt[121],uint64_t now_ms) {
  miniapp_runtime_lock();bool active=confirmation_locked(prompt,now_ms);
  miniapp_runtime_unlock();return active;
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
/* Host row identity can change without scrolling or geometry changes. Capture
 * every host view, not only viewports, so a released pointer cannot select a
 * different record now occupying the same fixed button. Caller holds runtime. */
#if PET_VARIABLE_OWNERS_ENABLED
static EXT_RAM_BSS_ATTR pet_host_render_t g_pointer_host;
#endif
static int pointer_host_mode(uint64_t now){
#if PET_VARIABLE_OWNERS_ENABLED
  if(g_runtime.variable_owners.mixed)return g_pointer_host.mode;
#endif
  return pet_app_host_mode(g_runtime.widget_id,now);
}
static bool pointer_host_enabled(const char *action,uint64_t now){
#if PET_VARIABLE_OWNERS_ENABLED
  if(g_runtime.variable_owners.mixed)return pet_host_render_enabled(&g_pointer_host,action);
#endif
  return pet_app_host_enabled(g_runtime.widget_id,action,now);
}
/* Pure native geometry/actions do not acquire a host epoch dependency. A node
 * explicitly bound to host visibility/scroll or row identity still must keep
 * its observed publication, including away-and-back changes. */
static bool pointer_action_host_dependent(const char *action,int node){
  (void)node;
  if(runtime_action_host_dependent(&g_runtime,action))return true;
#if PET_VARIABLE_OWNERS_ENABLED
  if(g_runtime.variable_owners.mixed){
    if(node>=0&&node<g_runtime.surface.count){
      const pet_surface_node_t *n=&g_runtime.surface.nodes[node];
      if(n->host_mask||runtime_host_variable(&g_runtime,find_var(&g_runtime,n->show_var)))return true;
#if PET_HOST_COLLECTION_ENABLED
      if(n->collection_slot)return true;
#endif
    }
    if(g_runtime.surface.viewport.enabled&&runtime_host_variable(&g_runtime,find_var(&g_runtime,g_runtime.surface.viewport.offset_var)))return true;
    for(unsigned i=0;i<g_runtime.surface.viewport_count;i++)if(runtime_host_variable(&g_runtime,find_var(&g_runtime,g_runtime.surface.viewports[i].offset_var)))return true;
  }
#endif
  return false;
}
static void host_pointer_capture(pet_host_observation_t *out){
  memset(out,0,sizeof(*out));if(!g_runtime.app_host)return;
#if PET_VARIABLE_OWNERS_ENABLED
  if(g_runtime.variable_owners.mixed){pet_app_host_render_capture_observed(g_runtime.widget_id,&g_pointer_host,out,esp_timer_get_time()/1000ULL);return;}
#endif
  pet_app_host_observe(g_runtime.widget_id,out);
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
  static int32_t press_scroll[PET_SURFACE_VIEWPORTS+1];
  static uint32_t press_viewport_epoch;
  static pet_host_observation_t press_host;
  static char press_widget[48],press_action[48];
  char dispatch[48]={0};
  pet_host_observation_t dispatch_host={0};bool dispatch_host_guarded=false;
  int select_row=-1,set_volume=-1;
  char media_source[48]={0};
#if PET_SERVICE_BRIDGE_ENABLED
  char value_parameter[32]={0};int32_t dispatched_value=0;bool value_action=false;
#endif
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
  miniapp_runtime_lock();
  if(dragging&&event->kind!=PET_P4_POINTER_DOWN){
    bool same=press_epoch==g_input_epoch&&press_state==g_runtime.current_state&&press_revision==g_image_revision&&!strcmp(press_widget,g_runtime.widget_id);
    if(same&&(event->kind==PET_P4_POINTER_MOVE||event->kind==PET_P4_POINTER_UP))miniapp_drag_point(&g_runtime,event->x,event->y);
    if(!same||event->kind==PET_P4_POINTER_UP||event->kind==PET_P4_POINTER_CANCEL)dragging=false;
    stroke=pressed=false;g_pressed_node=-1;miniapp_runtime_unlock();return;
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
      press_viewport_epoch=g_viewport_epoch;
      host_pointer_capture(&press_host);
#if PET_HOST_COLLECTION_ENABLED
      g_pressed_collection_epoch=press_host.variable_epoch;
#endif
      surface_scroll_context(press_scroll,event->ts_ms);
      int page=current_page<0?0:current_page;
      for(int i=g_runtime.surface.count-1;i>=0;i--){
        const pet_surface_node_t *n=&g_runtime.surface.nodes[i];
        if(n->state_mask&&!(n->state_mask&(1u<<g_runtime.current_state)))continue;
        if(g_runtime.app_host&&n->host_mask&&!(n->host_mask&(1u<<pointer_host_mode(event->ts_ms))))continue;
        if(runtime_host_action(&g_runtime,n->action)&&!pointer_host_enabled(n->action,event->ts_ms))continue;
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
        int32_t show_value=surface_integer(show,event->ts_ms);
        if(show>=0&&(show_value<n->show_min||show_value>n->show_max))continue;
        const pet_surface_viewport_t *nv=pet_surface_node_viewport(&g_runtime.surface,n);
        if(n->action[0]&&(g_runtime.surface.media_touch?(n->page==2||n->page==page):pet_surface_node_on_page(n,page))&&pet_surface_hit(nv,n,surface_scroll(nv,event->ts_ms),event->x,event->y)){copy_utf8(press_action,sizeof(press_action),n->action);press_node=i;break;}
      }
      g_pressed_node=press_node;
      if(press_node>=0&&g_runtime.hold_action[0]&&!strcmp(press_action,g_runtime.hold_action)){
        const pet_surface_node_t *n=&g_runtime.surface.nodes[press_node];hold_x=n->x;hold_y=n->y;hold_w=n->w;hold_h=n->h;
        copy_utf8(dispatch,sizeof(dispatch),press_action);begin_hold=true;early=true;
      }
      g_pressed_context=((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;
      memcpy(g_pressed_scroll,press_scroll,sizeof(press_scroll));
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
      pet_host_observation_t host_observed;host_pointer_capture(&host_observed);
      int32_t current_scroll[PET_SURFACE_VIEWPORTS+1];surface_scroll_context(current_scroll,event->ts_ms);
      bool same=stroke&&!early&&(!pointer_action_host_dependent(press_action,press_node)||pet_host_observation_equal(&press_host,&host_observed))&&!memcmp(press_scroll,current_scroll,sizeof(press_scroll))&&press_viewport_epoch==g_viewport_epoch&&press_epoch==g_input_epoch&&press_page==current_page&&press_state==g_runtime.current_state&&press_revision==g_image_revision&&!strcmp(press_widget,g_runtime.widget_id)&&event->ts_ms>=start_ms&&event->ts_ms-start_ms<10000;
      bool quick=same&&event->ts_ms-start_ms<700;
      int dx=event->x-start_x,dy=event->y-start_y,ax=abs(dx),ay=abs(dy);
      bool media_same=stroke&&press_epoch==g_input_epoch&&press_page==current_page&&press_revision==g_image_revision&&media_revision==media_view.queue_revision&&!strcmp(press_widget,g_runtime.widget_id)&&event->ts_ms>=start_ms;
      if(media_same&&media_row>=0&&event->ts_ms-start_ms<700&&ax<=14&&ay<=14){
        select_row=media_row;copy_utf8(media_source,48,g_runtime.media_source);pressed=false;
      }else if(media_same&&slider_node>=0&&slider_node<g_runtime.surface.count&&event->ts_ms-start_ms<10000&&ay<=28){
        const pet_surface_node_t *n=&g_runtime.surface.nodes[slider_node];
        set_volume=(event->x-n->x)*100/n->w;if(set_volume<0)set_volume=0;if(set_volume>100)set_volume=100;
        copy_utf8(media_source,48,g_runtime.media_source);pressed=false;
      }
#if PET_SERVICE_BRIDGE_ENABLED
      if(same&&press_node>=0&&press_node<g_runtime.surface.count&&ay<=28){
        const pet_surface_node_t*n=&g_runtime.surface.nodes[press_node];
        if(n->value_param[0]&&runtime_action_enabled(&g_runtime,press_action)&&!strcmp(n->action,press_action)){
          int dx_value=event->x-n->x;if(dx_value<0)dx_value=0;if(dx_value>n->w)dx_value=n->w;
          dispatched_value=(int32_t)((int64_t)n->value_min+((int64_t)n->value_max-n->value_min)*dx_value/n->w);
          copy_utf8(value_parameter,sizeof(value_parameter),n->value_param);copy_utf8(dispatch,sizeof(dispatch),press_action);value_action=true;pressed=false;
        }
      }
#endif
      if(
#if PET_SERVICE_BRIDGE_ENABLED
         !value_action&&
#endif
         quick&&g_runtime.input_v1&&(ax>=40||ay>=40)&&(ax*2>=ay*3||ay*2>=ax*3)){
        int direction=ax>ay?(dx<0?0:1):(dy<0?2:3);
        const char *action=g_runtime.swipe_actions[direction];
        if(action[0]&&runtime_action_enabled(&g_runtime,action)&&(!pointer_action_host_dependent(action,-1)||pet_host_observation_equal(&press_host,&host_observed)))copy_utf8(dispatch,sizeof(dispatch),action);
        pressed=false;
      }
      if(press_node<0||press_node>=g_runtime.surface.count)pressed=false;
      else {
        const pet_surface_node_t *n=&g_runtime.surface.nodes[press_node];
        int show=n->show_var[0]?find_var(&g_runtime,n->show_var):-1;
        int32_t show_value=surface_integer(show,event->ts_ms);
        if(show>=0&&(show_value<n->show_min||show_value>n->show_max))pressed=false;
        const pet_surface_viewport_t *nv=pet_surface_node_viewport(&g_runtime.surface,n);
        if(!pet_surface_hit(nv,n,surface_scroll(nv,event->ts_ms),event->x,event->y))pressed=false;
      }
      if(pressed&&quick&&runtime_action_enabled(&g_runtime,press_action)&&abs(event->x-start_x)<=14&&abs(event->y-start_y)<=14)copy_utf8(dispatch,sizeof(dispatch),press_action);
      stroke=pressed=false;g_pressed_node=-1;
    }else if(event->kind==PET_P4_POINTER_CANCEL){stroke=pressed=false;g_pressed_node=-1;}
  }else {stroke=pressed=false;g_pressed_node=-1;}
  if(dispatch[0]&&pointer_action_host_dependent(dispatch,press_node)){dispatch_host=press_host;dispatch_host_guarded=true;}
  miniapp_runtime_unlock();
  if(media_source[0])pet_p4_media_touch(media_source,select_row,set_volume,media_revision,event->ts_ms);
#if PET_SERVICE_BRIDGE_ENABLED
  if(value_action&&dispatch[0]){(void)pet_app_host_observed_value_action(press_widget,press_revision,(uint8_t)press_page,dispatch,value_parameter,dispatched_value,&dispatch_host,event->ts_ms);return;}
#endif
  if(dispatch[0]){if(begin_hold){touch_holding=pet_p4_miniapp_hold_event(dispatch,0,2,event->ts_ms);stroke=pressed=false;}else dispatch_action_observed(dispatch,event->ts_ms,dispatch_host_guarded?&dispatch_host:NULL);}
}

bool pet_p4_miniapp_media_active(void) {
  miniapp_runtime_lock();
  bool active=g_runtime.active&&g_runtime.media;
  miniapp_runtime_unlock();return active;
}
/* Called with g_runtime_lock held. Host variables are ephemeral, typed and
 * shared by rendering and hit testing; local packages keep their original path. */
static int32_t surface_integer(int index,uint64_t now){
  if(index<0)return 0;
  const miniapp_var_t *var=&g_runtime.vars[index];
  if(!runtime_host_variable(&g_runtime,index))return var->int_value;
  char text[64];int32_t value=0;
#if PET_VARIABLE_OWNERS_ENABLED
  if(g_runtime.variable_owners.mixed){
    if(!pet_host_render_variable(&g_pointer_host,var->name,true,text,&value))return 0;
  }else
#endif
  if(!pet_app_host_variable(g_runtime.widget_id,var->name,true,text,&value,now))return 0;
  if(value<var->int_min)value=var->int_min;
  if(value>var->int_max)value=var->int_max;
  return value;
}
static int32_t render_integer(int index,const pet_host_render_t *host){
  if(index<0)return 0;
  const miniapp_var_t *v=&g_runtime.vars[index];int32_t value=v->int_value;
  if(runtime_host_variable(&g_runtime,index)){
    if(!pet_host_render_variable(host,v->name,true,NULL,&value))return 0;
    if(value<v->int_min)value=v->int_min;
    if(value>v->int_max)value=v->int_max;
  }
  return value;
}
static int32_t render_scroll(const pet_surface_viewport_t *v,const pet_host_render_t *host){
  return v&&v->enabled?pet_surface_clamp_offset(v,render_integer(find_var(&g_runtime,v->offset_var),host)):0;
}
/* Runtime lock held. The host capture is immutable throughout resolution. */
static bool app_surface_locked(pet_surface_t *surface,const pet_host_render_t *host) {
  bool active=g_runtime.active&&g_runtime.surface.app;
#if PET_P4_DATA_ROWS
  if(!active||!g_runtime.data_rows.enabled)pet_p4_widget_data_set_filter(NULL,NULL,NULL);
#endif
  if(active&&surface){
    *surface=g_runtime.surface;
#if PET_CANVAS_ENABLED
    surface->canvas_bindings_ready=true;
    for(unsigned i=0;i<surface->canvas.variable_count;i++){
      int v=find_var(&g_runtime,surface->canvas.names[i]);
      if(v<0||g_runtime.vars[v].type!=MINIAPP_VAR_INT){surface->canvas_bindings_ready=false;break;}
      if(runtime_host_variable(&g_runtime,v)){
        if(!pet_host_render_variable(host,surface->canvas.names[i],true,NULL,&surface->canvas.values[i])){surface->canvas_bindings_ready=false;break;}
      }else surface->canvas.values[i]=g_runtime.vars[v].int_value;
    }
#endif
#if PET_P4_PARTICLES
    for(unsigned i=0;i<surface->particle_count;i++){
      pet_surface_particles_t *e=&surface->particles[i];e->when_ready=false;e->when_current=0;
      if(!e->when_var[0])continue;
      int v=find_var(&g_runtime,e->when_var);
      if(v<0||g_runtime.vars[v].type!=MINIAPP_VAR_INT)continue;
      if(runtime_host_variable(&g_runtime,v))e->when_ready=pet_host_render_variable(host,e->when_var,true,NULL,&e->when_current);
      else{e->when_current=g_runtime.vars[v].int_value;e->when_ready=true;}
    }
#endif
    int32_t scroll[PET_SURFACE_VIEWPORTS+1];scroll[0]=render_scroll(&surface->viewport,host);
    for(int i=0;i<PET_SURFACE_VIEWPORTS;i++)scroll[i+1]=i<surface->viewport_count?render_scroll(&surface->viewports[i],host):0;
    if(g_pressed_node>=0&&memcmp(g_pressed_scroll,scroll,sizeof(scroll))){g_viewport_epoch++;g_pressed_node=-1;}
    surface->viewport.offset=scroll[0];
    for(int i=0;i<surface->viewport_count;i++)surface->viewports[i].offset=scroll[i+1];
    static const char *const events[]={"button.sw1.short_press","button.sw2.short_press","button.sw3.short_press","button.encoder.short_press","button.encoder.long_press","knob.rotate_ccw","knob.rotate_cw","joystick.up","joystick.down"};
    static const char *const labels[]={"SW1","SW2","SW3","中按","中长","左","右","上","下"};
#if PET_FORM_V2_ENABLED
    /* Capture/format each field once under this runtime lock, not once for
     * every bound node (up to 32). Normal builds have no extra scratch/calls. */
    char form2_labels[PET_FORM_FIELDS][121];
    if(g_runtime.form_text.enabled){
      pet_form_value_t values[PET_FORM_VARS];form2_values(&g_runtime,values);
      for(unsigned i=0;i<g_runtime.form.count;i++)if(!pet_form2_text(&g_runtime.form,&g_runtime.form_text,values,i,form2_labels[i]))form2_labels[i][0]=0;
    }
#endif
#if PET_FORM_V3_ENABLED
    char form3_labels[PET_FORM3_FIELDS][121];
    if(g_runtime.form3.enabled){
      pet_form3_value_t *values=form3_workspace_begin();form3_values(&g_runtime,values);
      for(unsigned i=0;i<g_runtime.form3.count;i++)if(!pet_form3_text(&g_runtime.form3,values,i,form3_labels[i]))form3_labels[i][0]=0;
      form3_workspace_end();
    }
#endif
    for(int i=0;i<surface->count;i++){
      pet_surface_node_t *n=&surface->nodes[i];
      if(n->field_bind[0]){
#if PET_FORM_V3_ENABLED
        if(g_runtime.form3.enabled){int field=pet_form3_field_index(&g_runtime.form3,n->field_bind);copy_utf8(n->text,sizeof(n->text),field>=0?form3_labels[field]:"");}
        else
#endif
#if PET_FORM_V2_ENABLED
        if(g_runtime.form_text.enabled){int field=pet_form_field_index(&g_runtime.form,n->field_bind);copy_utf8(n->text,sizeof(n->text),field>=0?form2_labels[field]:"");}
        else
#endif
        {
        int32_t values[PET_FORM_VARS];form_values(&g_runtime,values);
        pet_form_text(&g_runtime.form,values,pet_form_field_index(&g_runtime.form,n->field_bind),n->text);
        }
      }
#if PET_P4_TEXT_TABLE
      else if(n->text_at[0]){
        int v=find_var(&g_runtime,n->text_at);
        copy_utf8(n->text,sizeof(n->text),v>=0?text_at(&g_runtime,render_integer(v,host)):"");
      }
#endif
#if PET_P4_CELLS_V1
      if(n->type==PET_SURFACE_NODE_CELLS){
        int cx=n->cursor_x[0]?find_var(&g_runtime,n->cursor_x):-1,cy=n->cursor_y[0]?find_var(&g_runtime,n->cursor_y):-1;
        n->cursor_col=cx>=0?(int16_t)(g_runtime.vars[cx].int_value<-1?-1:g_runtime.vars[cx].int_value>255?255:g_runtime.vars[cx].int_value):-1;
        n->cursor_row=cy>=0?(int16_t)(g_runtime.vars[cy].int_value<-1?-1:g_runtime.vars[cy].int_value>255?255:g_runtime.vars[cy].int_value):-1;
      }
#endif
#if PET_P4_BIG_IMAGE
      if(n->big>=0){
        int bx=n->big_x_var[0]?find_var(&g_runtime,n->big_x_var):-1,by=n->big_y_var[0]?find_var(&g_runtime,n->big_y_var):-1;
        n->big_x=render_integer(bx,host);n->big_y=render_integer(by,host);
      }
#endif
      if(n->var_bind[0]){
        int v=find_var(&g_runtime,n->var_bind);
        if(runtime_host_variable(&g_runtime,v)){
          char text[64];int32_t number=0;
          bool found=v>=0&&pet_host_render_variable(host,n->var_bind,g_runtime.vars[v].type==MINIAPP_VAR_INT,text,&number);
          copy_utf8(n->text,sizeof(n->text),found?text:"—");n->bound_value=number;
        }else if(v>=0){
          const miniapp_var_t *value=&g_runtime.vars[v];
          n->bound_value=value->int_value;
          if(value->type==MINIAPP_VAR_INT)snprintf(n->text,sizeof(n->text),"%ld",(long)value->int_value);
          else copy_utf8(n->text,sizeof(n->text),value->string_value);
        }
      }
      n->disabled=n->action[0]&&!runtime_action_enabled(&g_runtime,n->action);
      if(runtime_host_action(&g_runtime,n->action)&&n->action[0]&&!pet_host_render_enabled(host,n->action))n->disabled=true;
      uint64_t context=((uint64_t)g_image_revision<<16)|((uint8_t)g_runtime.current_state<<8)|(uint8_t)g_runtime.current_page;
      n->pressed=!n->disabled&&i==g_pressed_node&&g_pressed_context==context&&!memcmp(g_pressed_scroll,scroll,sizeof(scroll));
#if PET_HOST_COLLECTION_ENABLED
      if(n->collection_slot&&(!host||host->collection_epoch!=g_pressed_collection_epoch))n->pressed=false;
#endif
      int show=n->show_var[0]?find_var(&g_runtime,n->show_var):-1;
      int32_t show_value=render_integer(show,host);
      n->hidden=(show>=0&&(show_value<n->show_min||show_value>n->show_max))||(n->state_mask&&!(n->state_mask&(1u<<g_runtime.current_state)));
      if(n->host_mask&&!(n->host_mask&(1u<<(host?host->mode:7))))n->hidden=true;
      int tile=n->tile_var[0]?find_var(&g_runtime,n->tile_var):-1;
      if(tile>=0&&n->tile_cols){
        int64_t value=render_integer(tile,host);
        int max=n->tile_max_var[0]?find_var(&g_runtime,n->tile_max_var):-1;
        if(max>=0){int32_t total=render_integer(max,host);value=total>0?value*n->tile_cols/total:0;if(value<0)value=0;if(value>=n->tile_cols)value=n->tile_cols-1;}
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
#if PET_HOST_COLLECTION_ENABLED
    pet_surface_resolve_collection(surface,host?&host->collection:NULL,host&&host->current&&host->fresh);
#endif
#if PET_P4_APP_SHELL
    if(surface->tabs.enabled){int tv=find_var(&g_runtime,surface->tabs.var);pet_surface_resolve_tabs(surface,tv>=0?render_integer(tv,host):0);}
#endif
#if PET_P4_LONGTEXT
    for(int t=0;t<surface->text_count;t++)if(surface->texts[t].used){
      int pv=find_var(&g_runtime,surface->texts[t].page_var);
      unsigned pages=pet_surface_resolve_longtext(surface,t,pv>=0?render_integer(pv,host):0);
      /* Derived page count: a read-only mirror the package may bind, never an input. */
      if(surface->texts[t].count_var[0]){int cv=find_var(&g_runtime,surface->texts[t].count_var);if(cv>=0&&g_runtime.vars[cv].type==MINIAPP_VAR_INT)g_runtime.vars[cv].int_value=(int32_t)pages;}
    }
#endif
#if PET_P4_DATA_ROWS
    if(g_runtime.data_rows.enabled&&g_runtime.data_source[0]){
      int32_t filter_values[PET_DATA_FILTERS_MAX]={0};
      for(unsigned f=0;f<g_runtime.data_rows.filter_count;f++){int fv=find_var(&g_runtime,g_runtime.data_rows.filters[f].var);filter_values[f]=fv>=0?render_integer(fv,host):0;}
      pet_p4_widget_data_set_filter(g_runtime.data_source,&g_runtime.data_rows,filter_values);
      pet_p4_data_row_t selected;int sv=g_runtime.data_rows.select_var[0]?find_var(&g_runtime,g_runtime.data_rows.select_var):-1;
      bool has=sv>=0&&pet_p4_widget_data_select(g_runtime.data_source,render_integer(sv,host),esp_timer_get_time()/1000ULL,&selected);
      pet_surface_resolve_rows(surface,has?&selected:NULL);
    }else pet_p4_widget_data_set_filter(NULL,NULL,NULL);
#endif
  }
  return active;
}
/* Shared only under the runtime lock, never retained by input callbacks. */
static EXT_RAM_BSS_ATTR pet_host_render_t g_render_host;
bool pet_p4_miniapp_app_surface(pet_surface_t *surface) {
  miniapp_runtime_lock();
  if(g_runtime.app_host){
#if PET_VARIABLE_OWNERS_ENABLED
    if(g_runtime.variable_owners.mixed)pet_app_host_render_capture_observed(g_runtime.widget_id,&g_render_host,NULL,esp_timer_get_time()/1000ULL);else
#endif
    pet_app_host_render_capture(g_runtime.widget_id,&g_render_host,NULL,esp_timer_get_time()/1000ULL);
  }
  bool active=app_surface_locked(surface,g_runtime.app_host?&g_render_host:NULL);
  miniapp_runtime_unlock();return active;
}
bool pet_p4_miniapp_media_config(char source[48],pet_surface_t *surface) {
  miniapp_runtime_lock();
  bool active=g_runtime.active&&g_runtime.media&&g_runtime.surface.count;
  if(active){memcpy(source,g_runtime.media_source,48);if(surface)*surface=g_runtime.surface;}
  miniapp_runtime_unlock();
  return active;
}

bool pet_p4_miniapp_media_surface_animated(void) {
  bool animated=false;
  miniapp_runtime_lock();
  if(g_runtime.active&&g_runtime.media)for(int i=0;i<g_runtime.surface.count;i++){
    if(g_runtime.surface.nodes[i].rotate_ms){animated=true;break;}
  }
  miniapp_runtime_unlock();
  return animated;
}

bool pet_p4_miniapp_instrument_config(uint8_t *effect, uint8_t *ambience) {
  /* Control loops need two bytes, not a multi-kilobyte render snapshot. */
  miniapp_runtime_lock();
  bool active = g_runtime.active && g_runtime.instrument;
  if (effect) *effect = active ? g_runtime.effect_volume : 0;
  if (ambience) *ambience = active ? g_runtime.ambience_volume : 0;
  miniapp_runtime_unlock();
  return active;
}

bool pet_p4_miniapp_active_id(char *out, size_t out_size) {
  bool active;
  if (!out || out_size == 0) return false;
  miniapp_runtime_lock();
  active = g_runtime.active && g_runtime.widget_id[0];
  copy_utf8(out, out_size, active ? g_runtime.widget_id : "");
  miniapp_runtime_unlock();
  return active && out[0];
}

bool pet_p4_miniapp_get_view(pet_p4_miniapp_view_t *out) {
  if (!out) return false;
  miniapp_runtime_lock();
#if PET_P4_DATA_ROWS
  if(!g_runtime.data_rows.enabled)pet_p4_widget_data_set_filter(NULL,NULL,NULL);
#endif
  *out = g_runtime.view;
  miniapp_runtime_unlock();
  if(out->app_host)pet_app_host_view(out,esp_timer_get_time()/1000ULL);
  pet_p4_widget_data_view(out->data_source, out->data_page, esp_timer_get_time() / 1000ULL, &out->data);
  return true;
}
static bool surface_images_locked(pet_p4_miniapp_sprite_pack_t *out,uint32_t *revision){
  bool changed=*revision!=g_image_revision;
#if PET_P4_APP_DIRTY_CACHE
  /* Empty -> empty revisions have no visual image content to invalidate.
   * Still copy metadata and advance the exact revision under the same lock.
   * Nonempty resources retain the original conservative invalidation. */
  bool visually_changed=changed&&(out->count||out->data_length||g_runtime.sprites.count||g_runtime.sprites.data_length);
#endif
  if(changed){*revision=g_image_revision;out->count=g_runtime.sprites.count;out->data_length=g_runtime.sprites.data_length;
    memcpy(out->items,g_runtime.sprites.items,sizeof(out->items));memcpy(out->pixels,g_runtime.sprites.pixels,out->data_length);}
#if PET_P4_APP_DIRTY_CACHE
  return visually_changed;
#else
  return changed;
#endif
}
bool pet_p4_miniapp_render_snapshot(pet_p4_miniapp_render_snapshot_t *out,
    pet_p4_miniapp_sprite_pack_t *images,uint32_t *cached_image_revision){
  if(!out||!images||!cached_image_revision)return false;
#if PET_P4_APP_PHASE_TRACE
  int64_t capture_started=esp_timer_get_time();
#endif
  miniapp_runtime_lock();
#if PET_P4_APP_PHASE_TRACE
  int64_t capture_locked=esp_timer_get_time();
  uint64_t wait_us=capture_locked>=capture_started?(uint64_t)(capture_locked-capture_started):0;
  out->capture_wait_us=wait_us>UINT32_MAX?UINT32_MAX:(uint32_t)wait_us;
#endif
  const uint64_t now=esp_timer_get_time()/1000ULL;
#if PET_P4_DATA_ROWS
  if(!g_runtime.data_rows.enabled)pet_p4_widget_data_set_filter(NULL,NULL,NULL);
#endif
  out->view=g_runtime.view;out->surface_active=false;out->side_active=false;out->images_changed=false;
  out->media_source[0]=0;out->image_revision=g_image_revision;
#if PET_P4_CAPABILITY_EXT
  out->media_generation=g_runtime.cext.media_generation;
#else
  out->media_generation=0;
#endif
  out->confirmation_prompt[0]=0;out->confirmation_active=confirmation_locked(out->confirmation_prompt,now);
  if(g_runtime.app_host){
#if PET_VARIABLE_OWNERS_ENABLED
    if(g_runtime.variable_owners.mixed)pet_app_host_render_capture_observed(g_runtime.widget_id,&g_render_host,NULL,now);else
#endif
    pet_app_host_render_capture(g_runtime.widget_id,&g_render_host,runtime_host_dashboard(&g_runtime)?&out->view:NULL,now);
  }
  if(g_runtime.active&&g_runtime.surface.app){
    out->surface_active=app_surface_locked(&out->surface,g_runtime.app_host?&g_render_host:NULL);
#if PET_SERVICE_BRIDGE_ENABLED
    for(unsigned i=0;i<out->surface.count;i++)if(out->surface.nodes[i].image_bind[0])out->surface.nodes[i].bound_value=g_render_host.fresh?(int32_t)g_render_host.image_epoch:0;
#endif
#if PET_P4_CELLS_V1
    out->cells=g_runtime.cells;
#endif
    out->side_active=g_runtime.side_enabled;
    if(out->side_active){out->scene=g_runtime.side;out->art=g_runtime.side_art;}
  }else if(g_runtime.active&&g_runtime.media&&g_runtime.surface.count){
    out->surface=g_runtime.surface;out->surface_active=true;
    memcpy(out->media_source,g_runtime.media_source,sizeof(out->media_source));
  }
  if(out->surface_active)out->images_changed=surface_images_locked(images,cached_image_revision);
  miniapp_runtime_unlock();
  /* Data source/page are from the captured publication. No runtime/host lock
   * spans external data lookup, image decoding, drawing or storage. */
  pet_p4_widget_data_view(out->view.data_source,out->view.data_page,now,&out->view.data);
#if PET_P4_APP_PHASE_TRACE
  int64_t capture_ended=esp_timer_get_time();
  uint64_t total_us=capture_ended>=capture_started?(uint64_t)(capture_ended-capture_started):0;
  out->capture_total_us=total_us>UINT32_MAX?UINT32_MAX:(uint32_t)total_us;
#endif
  return true;
}
bool pet_p4_miniapp_side_snapshot(pet_side_scene_t*scene,pet_side_art_t*art){
  miniapp_runtime_lock();bool active=g_runtime.active&&g_runtime.side_enabled;
  if(active){if(scene)*scene=g_runtime.side;if(art)*art=g_runtime.side_art;}
  miniapp_runtime_unlock();return active;
}
bool pet_p4_miniapp_side_status(pet_p4_miniapp_side_status_t*out){
  if(!out)return false;
  miniapp_runtime_lock();bool active=g_runtime.active&&g_runtime.side_enabled;
  if(active){const pet_side_scene_t*s=&g_runtime.side;const pet_side_actor_t*p=&s->actors[s->player];
    *out=(pet_p4_miniapp_side_status_t){.x=p->x,.y=p->y,.vx=p->vx,.vy=p->vy,.score=s->score,.elapsed_ms=s->elapsed_ms,.revision=s->revision,.last_step_us=g_runtime.side_last_step_us,.max_step_us=g_runtime.side_max_step_us,.coins=s->coins,.phase=s->phase,.lives=s->lives,.checkpoint=s->checkpoint,.direction=s->direction,.grounded=p->grounded,.jump_down=s->jump_down};
  }
  miniapp_runtime_unlock();return active;
}
bool pet_p4_miniapp_side_axis(int horizontal){
  miniapp_runtime_lock();bool active=g_runtime.active&&g_runtime.side_enabled&&g_input_visible;
  if(active){g_runtime.side_axis_live=horizontal!=0;pet_side_axis(&g_runtime.side,horizontal);if(!horizontal)g_runtime.side_axis_until=0;}
  miniapp_runtime_unlock();return active;
}

void pet_p4_miniapp_host_process(uint64_t now_ms,bool visible){
  char media_source[48]="";
  bool media_active=pet_p4_miniapp_media_config(media_source,NULL);
  pet_p4_media_visibility(media_source,visible&&media_active,now_ms);
  static char prior[48];
  char id[48]="";uint32_t rev;uint8_t page;
#if PET_SERVICE_BRIDGE_ENABLED
  char binding[65]={0};
#endif
  miniapp_runtime_lock();
#if PET_P4_NOTIFY || PET_P4_DATA_ROWS
  miniapp_d_drain_locked(visible);
#endif
  if(g_runtime.active&&(g_runtime.app_host||!strncmp(g_runtime.data_source,"fetch.",6)))copy_utf8(id,sizeof(id),g_runtime.widget_id);
  rev=g_image_revision;page=g_runtime.current_page<0?0:g_runtime.current_page;
#if PET_SERVICE_BRIDGE_ENABLED
  if(g_runtime.surface.service_bridge)memcpy(binding,g_runtime.host_contract,sizeof(binding));
#endif
  miniapp_runtime_unlock();
  if(prior[0]&&(!visible||strcmp(prior,id))){
    static EXT_RAM_BSS_ATTR pet_p4_media_view_t media;pet_p4_media_view(&media);
    if(!strncmp(media.source,"app.",4)&&!strcmp(media.source+4,prior)&&pet_p4_audio_music_active())pet_p4_audio_stream_flush();
  }
  copy_utf8(prior,sizeof(prior),id);
  /* An empty current ID still closes the previous host instance. */
#if PET_SERVICE_BRIDGE_ENABLED
  pet_app_host_tick_bound(id,rev,page,visible&&id[0],now_ms,binding);
#else
  pet_app_host_tick(id,rev,page,visible&&id[0],now_ms);
#endif
}
bool pet_p4_miniapp_host_source(const char *source){
  if(!source||strncmp(source,"app.",4))return false;
  miniapp_runtime_lock();
  bool ok=g_runtime.active&&g_runtime.app_host&&!strcmp(source+4,g_runtime.widget_id);
  miniapp_runtime_unlock();return ok;
}

bool pet_p4_miniapp_get_surface_images(pet_p4_miniapp_sprite_pack_t *out,uint32_t *revision) {
  if(!out||!revision)return false;
  miniapp_runtime_lock();
  bool changed=surface_images_locked(out,revision);
  miniapp_runtime_unlock();return changed;
}

bool pet_p4_miniapp_get_sprites(pet_p4_miniapp_sprite_pack_t *out) {
  if (!out) return false;
  miniapp_runtime_lock();
  out->count=g_runtime.sprites.count;out->data_length=g_runtime.sprites.data_length;
  memcpy(out->items,g_runtime.sprites.items,sizeof(out->items));
  memcpy(out->pixels,g_runtime.sprites.pixels,out->data_length);
  miniapp_runtime_unlock();
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

  miniapp_runtime_lock();
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
  miniapp_runtime_unlock();
  return changed;
}

static bool read_file_kind(const char *path, char *buffer, size_t capacity,bool json_text) {
  if(!storage_work_current()||!path||!buffer||capacity<2)return false;
  PET_WIDGET_TRACE_SCOPE(trace_open, PET_WTRACE_FS_OPEN);
  FILE *file = fopen(path, "rb");
  pet_widget_trace_scope_end(&trace_open);
  size_t len;
  if (!file) return false;
  PET_WIDGET_TRACE_SCOPE(trace_data, PET_WTRACE_FS_DATA);
  size_t expected=0,after=0;
  bool admitted=storage_work_current()&&pet_file_read_extent(file,capacity-1,&expected)==PET_FILE_SIZE_OK;
  len = admitted&&expected?fread(buffer, 1, expected, file):0;
  bool failed=!admitted||len!=expected||ferror(file)!=0
    ||pet_file_read_extent(file,capacity-1,&after)!=PET_FILE_SIZE_OK||after!=expected
    ||(json_text&&!pet_json_source_bounded_text_bytes(buffer,len));
  pet_widget_trace_scope_end(&trace_data);
  PET_WIDGET_TRACE_SCOPE(trace_close, PET_WTRACE_FS_CLOSE);
  failed=fclose(file)!=0||failed;
  pet_widget_trace_scope_end(&trace_close);
  if (failed||!storage_work_current()) return false;
  buffer[len] = '\0';
  return true;
}

/* Preserve the old plain-text identity/marker reader. JSON sources additionally
 * reject embedded raw NUL before a C-string length/checksum can hide its tail.
 * Neither path changes bytes on disk; binary image/audio reads stay separate. */
static bool read_file(const char *path,char *buffer,size_t capacity){
  return read_file_kind(path,buffer,capacity,false);
}
static bool read_json_file(const char *path,char *buffer,size_t capacity){
  return read_file_kind(path,buffer,capacity,true);
}

static bool read_binary_file(
  const char *path,
  uint8_t *buffer,
  size_t capacity,
  size_t *out_length
) {
  FILE *file;
  size_t length;
  if (!storage_work_current()||!path || !buffer || capacity == 0 || !out_length) return false;
  PET_WIDGET_TRACE_SCOPE(trace_open, PET_WTRACE_FS_OPEN);
  file = fopen(path, "rb");
  pet_widget_trace_scope_end(&trace_open);
  if (!file) return false;
  PET_WIDGET_TRACE_SCOPE(trace_data, PET_WTRACE_FS_DATA);
  size_t expected=0,after=0;
  bool admitted=storage_work_current()&&pet_file_read_extent(file,capacity,&expected)==PET_FILE_SIZE_OK;
  length = admitted&&expected?fread(buffer, 1, expected, file):0;
  bool failed=!admitted||length!=expected||ferror(file)!=0
    ||pet_file_read_extent(file,capacity,&after)!=PET_FILE_SIZE_OK||after!=expected;
  pet_widget_trace_scope_end(&trace_data);
  PET_WIDGET_TRACE_SCOPE(trace_close, PET_WTRACE_FS_CLOSE);
  failed=fclose(file)!=0||failed;
  pet_widget_trace_scope_end(&trace_close);
  if (failed||!storage_work_current()) return false;
  *out_length = length;
  return true;
}

bool pet_p4_miniapp_installed_id(char *out, size_t out_size) {
  return pet_p4_miniapp_active_id(out, out_size);
}

static bool write_file_atomic(const char *tmp_path, const char *path, const char *data, size_t len) {
  if(!storage_work_current())return false;
  PET_WIDGET_TRACE_SCOPE(trace_open, PET_WTRACE_FS_OPEN);
  FILE *file = fopen(tmp_path, "wb");
  pet_widget_trace_scope_end(&trace_open);
  if (!file) {
    snprintf(g_storage_failure, sizeof(g_storage_failure), "open %s errno=%d", tmp_path, errno);
    return false;
  }
  PET_WIDGET_TRACE_SCOPE(trace_data, PET_WTRACE_FS_DATA);
  bool ok = storage_work_current()&&fwrite(data, 1, len, file) == len && fflush(file) == 0;
  pet_widget_trace_scope_end(&trace_data);
  int saved_errno = errno;
  PET_WIDGET_TRACE_SCOPE(trace_close, PET_WTRACE_FS_CLOSE);
  if (fclose(file) != 0) { ok = false; saved_errno = errno; }
  pet_widget_trace_scope_end(&trace_close);
  /*
   * SPIFFS garbage collection can take several seconds when the appearance
   * partition is populated. Let the idle task run between transactional file
   * writes so component commit never starves the task watchdog.
   */
  vTaskDelay(pdMS_TO_TICKS(1));
  if(!storage_work_current())return false; /* Closed tmp may be recovered/overwritten later. */
  if (!ok) {
    snprintf(g_storage_failure, sizeof(g_storage_failure), "write/close %s errno=%d bytes=%u", tmp_path, saved_errno, (unsigned)len);
    { PET_WIDGET_TRACE_SCOPE(trace_remove, PET_WTRACE_FS_REMOVE); remove(tmp_path); }
    return false;
  }
  PET_WIDGET_TRACE_SCOPE(trace_remove, PET_WTRACE_FS_REMOVE);
  int remove_result = remove(path);
  int remove_errno = errno;
  pet_widget_trace_scope_end(&trace_remove);
  if (remove_result != 0 && remove_errno != ENOENT) {
    snprintf(g_storage_failure, sizeof(g_storage_failure), "replace %s errno=%d", path, errno);
    { PET_WIDGET_TRACE_SCOPE(trace_cleanup, PET_WTRACE_FS_REMOVE); remove(tmp_path); }
    return false;
  }
  PET_WIDGET_TRACE_SCOPE(trace_rename, PET_WTRACE_FS_RENAME);
  int rename_result = rename(tmp_path, path);
  pet_widget_trace_scope_end(&trace_rename);
  if (rename_result != 0) {
    snprintf(g_storage_failure, sizeof(g_storage_failure), "rename %s errno=%d", path, errno);
    { PET_WIDGET_TRACE_SCOPE(trace_cleanup, PET_WTRACE_FS_REMOVE); remove(tmp_path); }
    return false;
  }
  return true;
}

/* Catalog completion has three outcomes: true=confirmed completion; false
 * with may_have_changed=false=known pre-commit failure; false with true=unknown.
 * The ordinary package writer's tmp cleanup is NOT a catalog rollback policy. */
#if !PET_P4_COMMIT_LEAN_IO
static bool write_catalog_atomic(const char *tmp_path,const char *path,
    const char *data,size_t len,bool *may_have_changed) {
  if(!storage_work_current())return false;
  FILE *file=fopen(tmp_path,"wb");
  if(!file)return false;
  bool ok=storage_work_current()&&fwrite(data,1,len,file)==len&&fflush(file)==0;
  if(fclose(file)!=0)ok=false;
  vTaskDelay(pdMS_TO_TICKS(1));
  if(!ok||!storage_work_current())return false; /* Retain tmp evidence. */
  /* From the first canonical-name mutation onwards no failure means rollback.
   * Even an error result from rename cannot prove the new generation absent. */
  *may_have_changed=true;
  if(remove(path)!=0&&errno!=ENOENT)return false;
  if(!storage_work_current()||rename(tmp_path,path)!=0)return false;
  return storage_work_current();
}
#endif

#if PET_P4_COMMIT_LEAN_IO
/* Reuse only the inactive catalog name, but never append into a shortened
 * SPIFFS last page: drop all old data pages before rewriting from offset zero.
 * The seal, final-length check and generation publication stay unchanged. */
static bool catalog_file_matches(const char *path,const char *data,size_t length){
  if(!storage_work_current()||!path||!data||!length||length>=MINIAPP_CATALOG_JSON_MAX)return false;
  FILE *file=fopen(path,"rb");
  if(!file)return false;
  size_t extent=0,offset=0;
  bool ok=pet_file_read_extent(file,length,&extent)==PET_FILE_SIZE_OK&&extent==length;
  unsigned char block[64];
  while(ok&&offset<length){
    size_t count=length-offset;if(count>sizeof(block))count=sizeof(block);
    ok=storage_work_current()&&fread(block,1,count,file)==count
      &&!ferror(file)&&!memcmp(block,data+offset,count);
    offset+=count;
  }
  if(ok)ok=fgetc(file)==EOF&&!ferror(file);
  if(fclose(file)!=0)ok=false;
  return ok&&storage_work_current();
}
static bool write_catalog_in_place(const char *path,const char *data,size_t len,bool *may_have_changed){
  if(!storage_work_current()||!path||!data||!may_have_changed)return false;
  PET_WIDGET_TRACE_SCOPE(trace_open, PET_WTRACE_FS_OPEN);
  FILE *file=fopen(path,"r+b");
  if(!file&&errno==ENOENT){
    /* A failing wb open can already have created/truncated the name. */
    *may_have_changed=true;
    file=fopen(path,"wb");
  }
  pet_widget_trace_scope_end(&trace_open);
  if(!file){snprintf(g_storage_failure,sizeof(g_storage_failure),"open %s errno=%d",path,errno);return false;}
  PET_WIDGET_TRACE_SCOPE(trace_data, PET_WTRACE_FS_DATA);
  bool ok=false;
  if(storage_work_current()){
    *may_have_changed=true; /* Before the first truncate/write, never infer rollback. */
    ok=ftruncate(fileno(file),0)==0&&fseek(file,0,SEEK_SET)==0
      &&storage_work_current()&&fwrite(data,1,len,file)==len&&fflush(file)==0;
  }
  if(ok)ok=storage_work_current()&&ftruncate(fileno(file),(off_t)len)==0;
  int saved_errno=errno;
  pet_widget_trace_scope_end(&trace_data);
  PET_WIDGET_TRACE_SCOPE(trace_close, PET_WTRACE_FS_CLOSE);
  if(fclose(file)!=0){ok=false;saved_errno=errno;}
  pet_widget_trace_scope_end(&trace_close);
  vTaskDelay(pdMS_TO_TICKS(1));
  if(!ok)snprintf(g_storage_failure,sizeof(g_storage_failure),"write/close %s errno=%d bytes=%u",path,saved_errno,(unsigned)len);
  if(!ok||!storage_work_current())return false;
  if(!catalog_file_matches(path,data,len)){
    snprintf(g_storage_failure,sizeof(g_storage_failure),"catalog readback failed; completion unknown");
    return false;
  }
  return true;
}
#define MINIAPP_CATALOG_CHECK_KEY "\"check\":\""
static bool catalog_hex_digit(char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}
/* The eight check digits inside the first JSON value, or NULL. */
static char *catalog_check_digits(char *json,size_t len){
  char *at=json?strstr(json,MINIAPP_CATALOG_CHECK_KEY):NULL;
  if(!at)return NULL;
  char *digits=at+sizeof(MINIAPP_CATALOG_CHECK_KEY)-1;
  if((size_t)(digits-json)+9>len||digits[8]!='"')return NULL;
  for(int i=0;i<8;i++)if(!catalog_hex_digit(digits[i]))return NULL;
  return digits;
}
/* FNV-1a over the snapshot text with its own eight digits read as '0'. A torn
 * or mixed in-place write cannot reproduce it; this is not a signature. */
static uint32_t catalog_check_value(const char *json,size_t len,const char *digits){
  uint32_t n=2166136261U;
  for(size_t i=0;i<len;i++){
    unsigned char b=(json+i>=digits&&json+i<digits+8)?(unsigned char)'0':(unsigned char)json[i];
    n^=b;n*=16777619U;
  }
  return n;
}
static bool catalog_check_seal(char *json){
  size_t len=json?strlen(json):0;char *digits=catalog_check_digits(json,len);
  if(!digits||strstr(digits+9,MINIAPP_CATALOG_CHECK_KEY))return false;
  for(int i=0;i<8;i++)if(digits[i]!='0')return false;
  char hex[9];snprintf(hex,sizeof(hex),"%08lx",(unsigned long)catalog_check_value(json,len,digits));
  memcpy(digits,hex,8);return true;
}
/* Snapshots without a check (older firmware) stay accepted. A check inside the
 * first JSON value must match exactly those value bytes. */
static bool catalog_check_verify(const char *json,size_t value_len){
  const char *key=json?strstr(json,MINIAPP_CATALOG_CHECK_KEY):NULL;
  if(!key||(size_t)(key-json)>=value_len)return true;
  const char *digits=catalog_check_digits((char*)json,value_len);
  if(!digits)return false;
  char hex[9];snprintf(hex,sizeof(hex),"%08lx",(unsigned long)catalog_check_value(json,value_len,digits));
  return memcmp(hex,digits,8)==0;
}
#endif

static uint32_t miniapp_checksum(const char *data, size_t len) {
  PET_WIDGET_TRACE_SCOPE(trace_checksum, PET_WTRACE_CHECKSUM);
  uint32_t checksum = 2166136261U;
  if (!data && len > 0) return 0;
  for (size_t i = 0; i < len; i += 1) {
    checksum ^= (uint8_t) data[i];
    checksum *= 16777619U;
  }
  return checksum;
}

static uint32_t sprite_pack_checksum(const pet_p4_miniapp_sprite_pack_t *pack) {
  PET_WIDGET_TRACE_SCOPE(trace_checksum, PET_WTRACE_CHECKSUM);
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

/* Package durability belongs to the later atomic catalog commit. Writing an
 * unreferenced generation directly cannot expose partial bytes to that catalog:
 * full logical filesystem readback/checksums still precede publication, not
 * physical NOR proof when caches can serve reads. Never use this for
 * catalog files, private saves, legacy migration or a currently referenced pair.
 * A failure retains the current generation and leaves only an unpublished spare.
 * The previous catalog can already refer to an overwritten spare in the old
 * rename-based path too; this does not grant permission to discard the live one. */
#if PET_P4_SPARE_EXACT_REUSE
static bool unchanged_storage_current(void);
/* Positive, boot-volatile optimization filter, NEVER existence or byte
 * authority. Unknown/new generations take the original writer without a
 * failed read-open scan. Every reuse still compares actual bytes/EOF. */
static uint8_t g_spare_json_seen[PET_P4_MINIAPP_CATALOG_MAX][MINIAPP_PACKAGE_GENERATION_COUNT];
#if PET_P4_SPARE_TARGET_FILTER
_Static_assert(MINIAPP_WIDGET_JSON_MAX<=UINT16_MAX&&MINIAPP_BUTTONS_JSON_MAX<=UINT16_MAX,
  "target filter length must cover every accepted JSON byte count");
/* Last successful writes to EACH spare path, not the currently live OTHER
 * generation. Only a rejection filter: stale metadata/collisions still require
 * exact physical bytes and EOF. Serialized by the existing storage lane. */
static uint32_t g_spare_json_crc[PET_P4_MINIAPP_CATALOG_MAX][MINIAPP_PACKAGE_GENERATION_COUNT][2];
static uint16_t g_spare_json_length[PET_P4_MINIAPP_CATALOG_MAX][MINIAPP_PACKAGE_GENERATION_COUNT][2];
#endif
#endif
static bool write_spare_package_json(size_t slot,uint8_t generation,bool buttons,
    const char *data,size_t length) {
  if(!data||slot>=PET_P4_MINIAPP_CATALOG_MAX||generation>=MINIAPP_PACKAGE_GENERATION_COUNT
      ||g_catalog_count>PET_P4_MINIAPP_CATALOG_MAX
      ||length>=(buttons?MINIAPP_BUTTONS_JSON_MAX:MINIAPP_WIDGET_JSON_MAX)
      ||!g_storage_lane.occupied||!g_storage_lane.scope_owner||!storage_work_current())return false;
  uintptr_t owner=g_storage_lane.owner?g_storage_lane.owner(g_storage_lane.context):0;
  if(g_storage_lane.scope_owner->owner!=owner
      ||(g_storage_lane.owner&&!g_storage_lane.scope_owner->unlocked))return false;
  for(size_t i=0;i<g_catalog_count;i++)
    if(g_catalog[i].slot==slot&&g_catalog[i].package_generation==generation)return false;
  char path[40];catalog_slot_path(slot,generation,buttons,false,path,sizeof(path));
#if PET_P4_SPARE_EXACT_REUSE
  /* Only reject candidates with the current checksum. A checksum NEVER
   * authorizes reuse: read this unreferenced target's bytes and exact EOF.
   * New installs and changed-current files avoid a redundant read-open scan.
   * The later full readback/parse/catalog transaction is entirely unchanged. */
  bool candidate=false;
#if PET_P4_SPARE_TARGET_FILTER
  if(g_spare_json_seen[slot][generation]&(buttons?2u:1u)){
    candidate=g_spare_json_length[slot][generation][buttons?1:0]==length
      &&g_spare_json_crc[slot][generation][buttons?1:0]==miniapp_checksum(data,length);
  }
#else
  for(size_t i=0;(g_spare_json_seen[slot][generation]&(buttons?2u:1u))&&i<g_catalog_count;i++)if(g_catalog[i].slot==slot&&g_catalog[i].resource_state==1){
    uint32_t checksum=buttons?g_catalog[i].buttons_checksum:g_catalog[i].widget_checksum;
    candidate=checksum==miniapp_checksum(data,length);break;
  }
#endif
  if(candidate){
    if(!unchanged_storage_current())return false;
    pet_storage_physical_scope_t physical={0};
    if(!pet_storage_physical_begin(&physical))return false;
    int exact=pet_file_exact(path,data,length,NULL,0,unchanged_storage_current);
    bool finished=pet_storage_physical_finish(&physical);
    if(!finished||exact==PET_FILE_EXACT_CANCELLED||!unchanged_storage_current())return false;
    if(exact==PET_FILE_EXACT_MATCH)return true;
    /* A missing/different/unreadable spare never becomes existence authority:
     * use the same already-authorized legacy write, with its error handling. */
  }
#endif
#if PET_P4_SPARE_RECREATE
  /* fopen(wb) truncates the existing spare page by page on SPIFFS. Removing
   * that SAME already-unreferenced generation first avoids that truncate path.
   * The live catalog/generation exclusion above is mandatory; no existence
   * cache, publication, rename, checksum or durable readback is bypassed. A
   * failure leaves only an unpublished spare absent/partial, as truncation did.
   * Never use this helper for catalogs, private saves or any live generation. */
  PET_WIDGET_TRACE_SCOPE(remove_spare,PET_WTRACE_FS_REMOVE);
  int removed=remove(path),remove_errno=errno;
  pet_widget_trace_scope_end(&remove_spare);
  if(removed!=0&&remove_errno!=ENOENT){
    snprintf(g_storage_failure,sizeof(g_storage_failure),"spare package remove errno=%d",remove_errno);return false;
  }
  if(!storage_work_current())return false;
#endif
  PET_WIDGET_TRACE_SCOPE(open_write,PET_WTRACE_FS_OPEN);
#if PET_P4_SPARE_UPDATE
  /* Only this already-authorized unreferenced generation may be updated.
   * Positive metadata is a hint, NEVER content/existence authority. Unknown
   * targets keep wb; only ENOENT may fall back. Do not apply to catalogs/saves.
   * An UPDATE must drop all old data pages before writing from offset zero;
   * a cross-page shrink<507 on SPIFFS can otherwise retain programmed tail bits.
   * Full logical readback/EOF, parse and A/B publication remain downstream;
   * cache-visible reads are not cold NOR verification. */
  bool update=(g_spare_json_seen[slot][generation]&(buttons?2u:1u))!=0;
#if PET_P4_SPARE_OPEN_TRACE
  PET_WIDGET_TRACE_SCOPE(open_purpose,update?PET_WTRACE_SPARE_OPEN_UPDATE:PET_WTRACE_SPARE_OPEN_CREATE);
#endif
  FILE *file=update?fopen(path,"r+b"):fopen(path,"wb");
  if(!file&&update&&errno==ENOENT){
#if PET_P4_SPARE_OPEN_TRACE
    PET_WIDGET_TRACE_SCOPE(create_fallback,PET_WTRACE_SPARE_OPEN_CREATE);
#endif
    update=false;file=fopen(path,"wb");
  }
#if PET_P4_SPARE_OPEN_TRACE
  pet_widget_trace_scope_end(&open_purpose);
#endif
#else
  FILE *file=fopen(path,"wb");
#endif
  pet_widget_trace_scope_end(&open_write);
  if(!file){snprintf(g_storage_failure,sizeof(g_storage_failure),"spare package open errno=%d",errno);return false;}
  bool stream_ready=true;
#if PET_P4_SPARE_UNBUFFERED
  /* Only this authorized unpublished package stream, before its first IO.
   * Avoid repeated small newlib writes; all flush/EOF/readback/catalog rules
   * remain. Failure closes the spare without publishing or touching live data. */
  stream_ready=setvbuf(file,NULL,_IONBF,0)==0;
  if(!stream_ready)errno=EIO;
#endif
  PET_WIDGET_TRACE_SCOPE(write_data,PET_WTRACE_FS_DATA);
#if PET_P4_SPARE_UPDATE
  if(stream_ready&&update)stream_ready=storage_work_current()
    &&ftruncate(fileno(file),0)==0&&fseek(file,0,SEEK_SET)==0;
#endif
  bool ok=stream_ready&&storage_work_current()&&fwrite(data,1,length,file)==length&&fflush(file)==0;
#if PET_P4_SPARE_UPDATE
  if(ok&&update)ok=storage_work_current()&&ftruncate(fileno(file),(off_t)length)==0;
#endif
  int write_errno=errno;
  pet_widget_trace_scope_end(&write_data);
  PET_WIDGET_TRACE_SCOPE(close_write,PET_WTRACE_FS_CLOSE);
  if(fclose(file)!=0){ok=false;write_errno=errno;}
  pet_widget_trace_scope_end(&close_write);
  vTaskDelay(pdMS_TO_TICKS(1));
  if(!ok)snprintf(g_storage_failure,sizeof(g_storage_failure),"spare package write/close errno=%d",write_errno);
#if PET_P4_SPARE_EXACT_REUSE
  if(ok&&storage_work_current()){
#if PET_P4_SPARE_TARGET_FILTER
    g_spare_json_crc[slot][generation][buttons?1:0]=miniapp_checksum(data,length);
    g_spare_json_length[slot][generation][buttons?1:0]=(uint16_t)length;
#endif
    g_spare_json_seen[slot][generation]|=buttons?2u:1u;
  }
#endif
  return ok&&storage_work_current();
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
  if (pixel_bytes>PET_P4_MINIAPP_SPRITE_V2_PIXELS*3U || file_length != MINIAPP_SPRITE_FILE_HEADER_SIZE + pixel_bytes
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
  PET_WIDGET_TRACE_SCOPE(trace_sprites, PET_WTRACE_SPRITE_READ);
  cJSON *root = NULL;
  const cJSON *sprites;
  uint8_t *file_data = NULL;
  bool ok = false;
  if (!pet_json_source_bounded_text(widget_json) || !pack) return false;
  memset(pack, 0, sizeof(*pack));
  root = cJSON_ParseWithOpts(widget_json,NULL,true);
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
      || cJSON_GetArraySize(sprites) > PET_P4_MINIAPP_SPRITE_DECLARED_MAX) {
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
#if PET_P4_KEYFRAME_BG
  {
    /* p4-keyframe-bg-v1: the JPEG background's frames 2..N follow the declared images as "<id>.k". */
    const cJSON *first=cJSON_GetArrayItem(sprites,0);int frames=0,declared=cJSON_GetArraySize(sprites);
    if(cJSON_IsObject(first)&&!strcmp(json_string(first,"encoding"),"jpeg")
        &&json_int_range(cJSON_GetObjectItemCaseSensitive(first,"frames"),2,PET_SURFACE_KEYFRAMES,&frames)){
      for(int k=2;k<=frames;k++){
        char id[PET_P4_MINIAPP_SPRITE_ID_MAX],path[48];size_t file_length=0;
        snprintf(id,sizeof(id),"%s.%d",json_string(first,"id"),k);
        catalog_sprite_path(slot,package_generation,(uint8_t)(declared+k-2),false,path,sizeof(path));
        if(!read_binary_file(path,file_data,MINIAPP_SPRITE_FILE_MAX,&file_length)
            ||!append_sprite_file(pack,id,file_data,file_length,error,error_size)){
          if(!error||!error[0])set_error(error,error_size,"keyframe background frame file is missing");
          goto done;
        }
      }
    }
  }
#endif
  ok = true;

done:
  free(file_data);
  cJSON_Delete(root);
  return ok;
}

#if PET_P4_COMMIT_LEAN_IO
/* Boot-local positive memo: sprite count + 1 last persisted to each unreferenced
 * (slot, generation) by this boot, 0 = unknown. It only skips the best-effort
 * unused-index cleanup scan when this boot already left that generation with
 * zero sprite files; it is never existence or content authority. */
static uint8_t g_sprite_count_seen[PET_P4_MINIAPP_CATALOG_MAX][MINIAPP_PACKAGE_GENERATION_COUNT];
#endif
/* Like spare JSON, these bytes are not published until full physical
 * readback and the A/B catalog commit below. No temporary-name rename is
 * needed for an unreferenced image generation. Never use for a live package,
 * catalogs, saves, or callers outside the unlocked storage command scope. */
static bool write_spare_sprite(size_t slot, uint8_t generation, uint8_t index,
    const uint8_t *data, size_t length) {
  if (!data || !length || length > MINIAPP_SPRITE_FILE_MAX
      || slot >= PET_P4_MINIAPP_CATALOG_MAX
      || generation >= MINIAPP_PACKAGE_GENERATION_COUNT
      || index >= PET_P4_MINIAPP_SPRITE_MAX
      || g_catalog_count > PET_P4_MINIAPP_CATALOG_MAX
      || !g_storage_lane.occupied || !g_storage_lane.scope_owner
      || !storage_work_current()) return false;
  uintptr_t owner = g_storage_lane.owner ? g_storage_lane.owner(g_storage_lane.context) : 0;
  if (g_storage_lane.scope_owner->owner != owner
      || (g_storage_lane.owner && !g_storage_lane.scope_owner->unlocked)) return false;
  for (size_t i = 0; i < g_catalog_count; ++i)
    if (g_catalog[i].slot == slot && g_catalog[i].package_generation == generation) return false;
  char path[48];
  catalog_sprite_path(slot, generation, index, false, path, sizeof(path));
  /* Recreate only this unpublished spare: avoid SPIFFS truncating an old
   * atlas page by page. The current catalog and its image files stay intact. */
  { PET_WIDGET_TRACE_SCOPE(trace_remove, PET_WTRACE_FS_REMOVE);
    int result = remove(path), saved = errno;
    if (result != 0 && saved != ENOENT) {
      snprintf(g_storage_failure, sizeof(g_storage_failure), "spare image remove errno=%d", saved);
      return false;
    }
  }
  vTaskDelay(1);
  if (!storage_work_current()) return false;
  FILE *file;
  { PET_WIDGET_TRACE_SCOPE(trace_open, PET_WTRACE_FS_OPEN); file = fopen(path, "wb"); }
  if (!file) {
    snprintf(g_storage_failure, sizeof(g_storage_failure), "spare image open errno=%d", errno);
    return false;
  }
  bool ok = setvbuf(file, NULL, _IONBF, 0) == 0;
  size_t offset = 0;
  int saved = ok ? 0 : EIO;
  while (ok && offset < length) {
    size_t bytes = length - offset;
    if (bytes > 4096U) bytes = 4096U;
    { PET_WIDGET_TRACE_SCOPE(trace_data, PET_WTRACE_FS_DATA);
      ok = storage_work_current() && fwrite(data + offset, 1, bytes, file) == bytes;
      if (!ok) saved = errno;
    }
    if (!ok) break;
    offset += bytes;
    /* fwrite has returned from VFS: no SPIFFS/cache-off lock spans this tick.
     * A whole JPEG/atlas must not monopolize the protocol worker until close. */
    vTaskDelay(1);
  }
  if (ok) {
    PET_WIDGET_TRACE_SCOPE(trace_flush, PET_WTRACE_FS_DATA);
    ok = storage_work_current() && fflush(file) == 0;
    if (!ok) saved = errno;
  }
  { PET_WIDGET_TRACE_SCOPE(trace_close, PET_WTRACE_FS_CLOSE);
    if (fclose(file) != 0) { ok = false; saved = errno; }
  }
  vTaskDelay(1);
  if (!ok) {
    snprintf(g_storage_failure, sizeof(g_storage_failure), "spare image write/close errno=%d bytes=%u", saved, (unsigned)offset);
    /* Only this failed job's unpublished image: never retain its partial
     * allocation after a known write failure, and never delete after revocation. */
    if (storage_work_current()) {
      PET_WIDGET_TRACE_SCOPE(trace_cleanup, PET_WTRACE_FS_REMOVE);
      (void)remove(path);
    }
  }
  return ok && storage_work_current();
}

static bool persist_sprite_pack(
  size_t slot,
  uint8_t package_generation,
  const pet_p4_miniapp_sprite_pack_t *pack
) {
  uint8_t *file_data = NULL;
  bool ok = true;
#if PET_P4_COMMIT_LEAN_IO
  const bool memo_slot=slot<PET_P4_MINIAPP_CATALOG_MAX&&package_generation<MINIAPP_PACKAGE_GENERATION_COUNT;
  const bool skip_cleanup=pack&&pack->count==0&&memo_slot&&g_sprite_count_seen[slot][package_generation]==1;
#endif
  pet_file_cleanup_target_t unused[PET_P4_MINIAPP_SPRITE_MAX*2];
  pet_file_cleanup_plan_t cleanup={0};
  uint32_t cleanup_sequence=g_catalog_sequence;
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
    if(!storage_work_current()){ok=false;break;}
    char path[48];
    char temporary_path[48];
    catalog_sprite_path(slot, package_generation, index, false, path, sizeof(path));
    catalog_sprite_path(slot, package_generation, index, true, temporary_path, sizeof(temporary_path));
    if (index >= pack->count) {
#if PET_P4_COMMIT_LEAN_IO
      if(skip_cleanup)continue;
#endif
      if(index==pack->count){
        size_t count=0;
        for(uint8_t unused_index=index;unused_index<PET_P4_MINIAPP_SPRITE_MAX;unused_index++){
          catalog_sprite_path(slot,package_generation,unused_index,true,unused[count].path,sizeof(unused[count].path));count++;
          catalog_sprite_path(slot,package_generation,unused_index,false,unused[count].path,sizeof(unused[count].path));count++;
        }
        (void)pet_file_cleanup_prepare(&cleanup,"/spiffs",unused,count,cleanup_guard(&cleanup_sequence));
      }
      /* Preserve best-effort cleanup of unused sprite slots and its order. */
      (void)pet_file_cleanup_remove_at(&cleanup,(index-pack->count)*2);
      (void)pet_file_cleanup_remove_at(&cleanup,(index-pack->count)*2+1);
      continue;
    }
    const pet_p4_miniapp_sprite_t *sprite = &pack->items[index];
    if ((size_t) sprite->data_offset + sprite->data_length > pack->data_length) {
      ok = false;
      break;
    }
    if(sprite->fps==0){
      bool written = g_storage_lane.occupied
        ? write_spare_sprite(slot, package_generation, index, pack->pixels + sprite->data_offset, sprite->data_length)
        : write_file_atomic(temporary_path,path,(const char*)pack->pixels+sprite->data_offset,sprite->data_length);
      if (!written) { ok=false; break; }
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
    ok = g_storage_lane.occupied
      ? write_spare_sprite(slot, package_generation, index, file_data,
                          MINIAPP_SPRITE_FILE_HEADER_SIZE + sprite->data_length)
      : write_file_atomic(temporary_path, path, (const char *)file_data,
                          MINIAPP_SPRITE_FILE_HEADER_SIZE + sprite->data_length);
    if (!ok) break;
  }
  free(file_data);
#if PET_P4_COMMIT_LEAN_IO
  if(ok&&memo_slot&&pack->count<=PET_P4_MINIAPP_SPRITE_MAX)g_sprite_count_seen[slot][package_generation]=(uint8_t)(pack->count+1);
#endif
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

/* The old "selection" key is a last-opened ACTIVE hint. A separate, bounded
 * cursor key keeps browsing selection independent and never touches app saves. */
typedef struct {uint32_t magic,sequence;char id[48];} catalog_cursor_t;
#define CATALOG_CURSOR_MAGIC 0x50434331u
static bool catalog_cursor_read(uint32_t sequence,char out[48]){
  out[0]=0;nvs_handle_t handle;
  if(nvs_open("p4_widgets",NVS_READONLY,&handle)!=ESP_OK)return false;
  catalog_cursor_t saved={0};size_t size=sizeof(saved);
  esp_err_t err=nvs_get_blob(handle,"cursor",&saved,&size);nvs_close(handle);
  if(err!=ESP_OK||size!=sizeof(saved)||saved.magic!=CATALOG_CURSOR_MAGIC
      ||saved.sequence!=sequence||!memchr(saved.id,0,sizeof(saved.id))||!safe_widget_id(saved.id))return false;
  memcpy(out,saved.id,48);return true;
}
static bool catalog_cursor_write(uint32_t sequence,const char *id){
  if(!safe_widget_id(id))return false;
  char previous[48];if(catalog_cursor_read(sequence,previous)&&!strcmp(previous,id))return true;
  nvs_handle_t handle;if(nvs_open("p4_widgets",NVS_READWRITE,&handle)!=ESP_OK)return false;
  catalog_cursor_t saved={.magic=CATALOG_CURSOR_MAGIC,.sequence=sequence};
  memcpy(saved.id,id,strlen(id));
  esp_err_t err=nvs_set_blob(handle,"cursor",&saved,sizeof(saved));
  if(err==ESP_OK)err=nvs_commit(handle);
  nvs_close(handle);return err==ESP_OK;
}

typedef struct { uint8_t generation; uint32_t sequence; } miniapp_catalog_commit_t;
static bool write_catalog_snapshot_at(
  const miniapp_catalog_item_t *catalog,
  size_t count,
  const char *active_widget_id,
  const char *selected_widget_id,bool seed_pending,
  int base_generation,uint32_t base_sequence,
  miniapp_catalog_commit_t *commit
) {
  PET_WIDGET_TRACE_SCOPE(trace_catalog, PET_WTRACE_CATALOG_WRITE);
  cJSON *root = NULL;
  cJSON *items = NULL;
  char *json = NULL;
  bool ok = false;
  bool may_have_changed = false;
  if(g_catalog_recovery_blocked)return false;
  char catalog_path[48];
  char catalog_tmp_path[48];
  uint8_t target_generation;
  uint32_t next_sequence;
  if (count > PET_P4_MINIAPP_CATALOG_MAX) return false;
  if (active_widget_id && active_widget_id[0]
      && catalog_find(catalog, count, active_widget_id) < 0) {
    return false;
  }
  if(selected_widget_id&&selected_widget_id[0]&&catalog_find(catalog,count,selected_widget_id)<0)return false;
  if (base_sequence == UINT32_MAX || !commit) return false;
  root = cJSON_CreateObject();
  items = cJSON_CreateArray();
  target_generation = base_generation < 0
    ? 0
    : (uint8_t) (1 - base_generation);
  next_sequence = base_sequence + 1U;
  if (!root || !items) goto done;
  if(!cJSON_AddNumberToObject(root, "version", MINIAPP_CATALOG_VERSION)
      ||!cJSON_AddNumberToObject(root, "sequence", next_sequence))goto done;
#if PET_P4_COMMIT_LEAN_IO
  if(!cJSON_AddStringToObject(root,"check","00000000"))goto done;
#endif
  if (active_widget_id && active_widget_id[0]) {
    if(!cJSON_AddStringToObject(root, "activeWidgetId", active_widget_id))goto done;
  } else {
    if(!cJSON_AddNullToObject(root, "activeWidgetId"))goto done;
  }
  if(!cJSON_AddBoolToObject(root,"seedPending",seed_pending))goto done;
  if(selected_widget_id&&selected_widget_id[0]){
    if(!cJSON_AddStringToObject(root,"selectedWidgetId",selected_widget_id))goto done;
  }else if(!cJSON_AddNullToObject(root,"selectedWidgetId"))goto done;
  if(!cJSON_AddItemToObject(root, "items", items))goto done;
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
    if(!cJSON_AddStringToObject(item, "widgetId", catalog[i].widget_id)
        ||!cJSON_AddStringToObject(item, "title", catalog[i].title)
        ||!cJSON_AddNumberToObject(item, "slot", catalog[i].slot)
        ||!cJSON_AddNumberToObject(item, "packageGeneration", catalog[i].package_generation)
        ||!cJSON_AddStringToObject(item, "widgetChecksum", widget_checksum)
        ||!cJSON_AddStringToObject(item, "buttonsChecksum", buttons_checksum)
        ||!cJSON_AddStringToObject(item, "spritesChecksum", sprites_checksum)
        ||!cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root, "items"), item)){
      cJSON_Delete(item);goto done;
    }
  }
  json = cJSON_PrintUnformatted(root);
  if (!json || strlen(json) >= MINIAPP_CATALOG_JSON_MAX) goto done;
  catalog_snapshot_path(target_generation, false, catalog_path, sizeof(catalog_path));
  catalog_snapshot_path(target_generation, true, catalog_tmp_path, sizeof(catalog_tmp_path));
#if PET_P4_COMMIT_LEAN_IO
  (void)catalog_tmp_path;
  ok = catalog_check_seal(json) && write_catalog_in_place(catalog_path, json, strlen(json), &may_have_changed);
#else
  ok = write_catalog_atomic(
    catalog_tmp_path,
    catalog_path,
    json,
    strlen(json), &may_have_changed
  );
#endif
  if (ok) {
    commit->generation = target_generation;
    commit->sequence = next_sequence;
  }

done:
  if(!ok&&may_have_changed){
    /* No fake DURABLE transition: the bytes may be torn OR fully committed.
     * Release the finished physical scope, but quarantine every later writer. */
    g_catalog_recovery_blocked=true;
    snprintf(g_storage_failure,sizeof(g_storage_failure),
      "catalog outcome unknown; inspect sealed generations; do not replay");
  }
  cJSON_free(json);
  cJSON_Delete(items);
  cJSON_Delete(root);
  return ok;
}
/* Boot-only/non-yielding callers retain the original synchronous contract. */
static bool persist_catalog_snapshot(const miniapp_catalog_item_t *catalog,
  size_t count,const char *active_widget_id){
  miniapp_catalog_commit_t commit;
  const char *cursor=g_catalog_selected<g_catalog_count?g_catalog[g_catalog_selected].widget_id:active_widget_id;
  if(cursor&&cursor[0]&&catalog_find(catalog,count,cursor)<0)cursor=active_widget_id;
  if(!write_catalog_snapshot_at(catalog,count,active_widget_id,cursor,g_catalog_seed_pending,
      g_catalog_file_generation,g_catalog_sequence,&commit))return false;
  g_catalog_file_generation=commit.generation;g_catalog_sequence=commit.sequence;return true;
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
  PET_WIDGET_TRACE_SCOPE(trace_alloc, PET_WTRACE_ALLOC);
  widget_json = (char *) calloc(1, MINIAPP_WIDGET_JSON_MAX);
  buttons_json = (char *) calloc(1, MINIAPP_BUTTONS_JSON_MAX);
  sprites = (pet_p4_miniapp_sprite_pack_t *) calloc(1, sizeof(*sprites));
  pet_widget_trace_scope_end(&trace_alloc);
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
  bool files_ok;
  { PET_WIDGET_TRACE_SCOPE(trace_read, PET_WTRACE_READ_WIDGET);
    files_ok = read_json_file(widget_path, widget_json, MINIAPP_WIDGET_JSON_MAX); }
  if (files_ok) {
    PET_WIDGET_TRACE_SCOPE(trace_read, PET_WTRACE_READ_BUTTONS);
    files_ok = read_json_file(buttons_path, buttons_json, MINIAPP_BUTTONS_JSON_MAX);
  }
  if (!files_ok) {
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
    /* Boot-only package boundary: finish native IO and its cache lease before
     * blocking main so CPU0 idle can run. Never yield from cache-off/IRAM IO. */
    {
#if PET_P4_STORAGE_BOOT_CACHE_BATCH_ENABLED
      pet_storage_cache_work_t boot_cache __attribute__((cleanup(pet_storage_cache_work_finish)))={0};
      (void)pet_storage_cache_work_step(&boot_cache);
#elif PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED
      /* A fresh finite epoch per physical package, never a filename/existence
       * result or an extension of an earlier epoch. No changed recovery rules.
       * Admission/registration failure only bypasses this optimization. */
      pet_storage_cache_work_t boot_cache __attribute__((cleanup(pet_storage_cache_work_finish)))={0};
      (void)pet_storage_cache_work_begin(&boot_cache);
#endif
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
      } else {
        snapshot->needs_upgrade |= missing_checksums;
      }
    } /* cache lease cleanup, including failed package/admission paths */
    vTaskDelay(1); /* One actual tick, independent of milliseconds conversion. */
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
  bool read_ok;
  {
#if PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED
    pet_storage_cache_work_t boot_cache __attribute__((cleanup(pet_storage_cache_work_finish)))={0};
    (void)pet_storage_cache_work_begin(&boot_cache);
#endif
    read_ok=json&&read_json_file(path,json,MINIAPP_CATALOG_JSON_MAX);
  }
  if(!read_ok)goto fail;
#if PET_P4_COMMIT_LEAN_IO
  {
    const char *parse_end=NULL;
    root = cJSON_ParseWithOpts(json,&parse_end,0);
    if(root&&(!parse_end||parse_end<=json||!catalog_check_verify(json,(size_t)(parse_end-json))))goto fail;
  }
#else
  root = cJSON_Parse(json);
#endif
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
  const cJSON *seed_pending=cJSON_GetObjectItemCaseSensitive(root,"seedPending");
  if(seed_pending&&!cJSON_IsBool(seed_pending))goto fail;
  snapshot->seed_pending=cJSON_IsTrue(seed_pending);
  const cJSON *cursor=cJSON_GetObjectItemCaseSensitive(root,"selectedWidgetId");
  if(cursor&&!cJSON_IsNull(cursor)){
    if(!cJSON_IsString(cursor)||!safe_widget_id(cursor->valuestring))goto fail;
    copy_utf8(snapshot->selected_widget_id,sizeof(snapshot->selected_widget_id),cursor->valuestring);
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
  if(snapshot->selected_widget_id[0]&&catalog_find(snapshot->items,count,snapshot->selected_widget_id)<0){
    snapshot->selected_widget_id[0]=0;snapshot->needs_upgrade=true;
  }
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
      || cJSON_GetArraySize(sprites) > (MINIAPP_GRAPHICS_V2(runtime)?PET_P4_MINIAPP_SPRITE_DECLARED_MAX:PET_P4_MINIAPP_SPRITE_V1_MAX)
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

#include "pet_p4_bodies_json.h"

static bool parse_bounded_op(
  const miniapp_runtime_t *runtime,
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
  if(!strcmp(name,"impulse")){
    static const char *const keys[]={"op","entity","dx","dy"};int dx,dy;
    if(!config->bodies||!object_keys_allowed(object,keys,4)||!parse_bounded_entity_ref(config,object,"entity",&op->entity_index)
      ||!config->entities[op->entity_index].body||!json_int_range(cJSON_GetObjectItemCaseSensitive(object,"dx"),-64,64,&dx)
      ||!json_int_range(cJSON_GetObjectItemCaseSensitive(object,"dy"),-64,64,&dy)||(!dx&&!dy))return false;
    op->kind=PET_P4_GAME_OP_IMPULSE;op->dx=dx;op->dy=dy;return true;
  }
  if (strcmp(name, "sfx") == 0) {
    /* p4-sfx-v1: {op:"sfx", cue} queues a cue; the engine only records it. */
    static const char *const allowed[] = {"op", "cue"};
    int cue = find_sfx_cue(runtime, json_string(object, "cue"));
    if (!runtime->sfx || !object_keys_allowed(object, allowed, sizeof(allowed) / sizeof(allowed[0])) || cue < 0) {
      set_error(error, error_size, "game sfx op requires the p4-sfx-v1 profile and a declared cue");
      return false;
    }
    op->kind = PET_P4_GAME_OP_SFX;
    op->value = (int16_t) cue;
    return true;
  }
  if (strcmp(name, "var") == 0) {
    /* p4-logic-v1: {op:"var", name, set|inc} on a runtime int var slot. */
    static const char *const allowed[] = {"op", "name", "set", "inc"};
    const cJSON *set_value = cJSON_GetObjectItemCaseSensitive(object, "set");
    const cJSON *inc_value = cJSON_GetObjectItemCaseSensitive(object, "inc");
    int var = find_var(runtime, json_string(object, "name"));
    if (!runtime->logic || !object_keys_allowed(object, allowed, sizeof(allowed) / sizeof(allowed[0]))
        || var < 0 || var >= PET_P4_GAME_MAX_VARS || runtime->vars[var].type != MINIAPP_VAR_INT
        || (set_value == NULL) == (inc_value == NULL)
        || !json_int_range(set_value ? set_value : inc_value, -10000, 10000, &number)) {
      set_error(error, error_size, "game var op requires logic p4-logic-v1, an int var and one -10000..10000 set/inc");
      return false;
    }
    op->kind = set_value ? PET_P4_GAME_OP_VAR_SET : PET_P4_GAME_OP_VAR_INC;
    op->entity_index = (int8_t) var;
    op->value = (int16_t) number;
    return true;
  }
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
  static const char *const allowed[] = {"on", "entity", "with", "edge", "do", "when"};
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
    const cJSON *when;
    if (!object_keys_allowed(item, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
      set_error(error, error_size, "game rule contains an unsupported field");
      return false;
    }
    on = json_string(item, "on");
    rule = &config->rules[config->rule_count];
    rule->entity_index = -1;
    rule->with_index = -1;
    when = cJSON_GetObjectItemCaseSensitive(item, "when");
    if (when) {
      miniapp_condition_t guard[MINIAPP_MAX_CONDITIONS];
      uint8_t guard_count = 0;
      if (!runtime->logic
          || !parse_condition(runtime, when, guard, &guard_count, false, error, error_size)
          || guard_count != 1 || guard[0].var >= PET_P4_GAME_MAX_VARS
          || guard[0].value_var >= PET_P4_GAME_MAX_VARS) {
        set_error(error, error_size, "game rule when requires logic p4-logic-v1 and one int condition");
        return false;
      }
      rule->has_when = true;
      rule->when_var = guard[0].var;
      rule->when_value_var = guard[0].value_var;
      rule->when_op = guard[0].op;
      rule->when_value = guard[0].value;
    }
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
            runtime,
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
    "grid", "sprites", "entities", "rules", "motion", "bodies", "gravity",
  };
  pet_p4_bounded_game_config_t *config __attribute__((cleanup(free_bounded_scene_config))) = NULL;
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
  /* Preparation can run on a bounded worker stack. Keep its ~4 KiB scratch
   * configuration off that stack; cleanup also covers every validation exit. */
  config = calloc(1, sizeof(*config));
  if (!config) {
    set_error(error, error_size, "not enough memory to prepare bounded scene");
    return false;
  }
  if(!runtime_v4&&(cJSON_HasObjectItem(scene,"bodies")||cJSON_HasObjectItem(scene,"gravity"))){
    set_error(error,error_size,"bodies requires the v4 runtime");return false;
  }
  config->motion=cJSON_GetObjectItemCaseSensitive(scene,"motion")!=NULL;
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
  if (!json_int_range(tick, cJSON_HasObjectItem(scene,"bodies")?20:config->motion?16:100, cJSON_HasObjectItem(scene,"bodies")?200:config->motion?40:2000, &tick_ms)) {
    set_error(error, error_size, "scene.tick_ms must be in 100-2000");
    return false;
  }
  config->tick_ms = (uint32_t) tick_ms;
  if (!parse_bounded_sprites(
        runtime,
        cJSON_GetObjectItemCaseSensitive(scene, "sprites"),
        runtime_v4,
        error,
        error_size
      )
      || !parse_bounded_grid(
        config,
        cJSON_GetObjectItemCaseSensitive(scene, "grid"),
        error,
        error_size
      )
      || !parse_bounded_entities(
        config,
        &runtime->sprites,
        cJSON_GetObjectItemCaseSensitive(scene, "entities"),
        error,
        error_size
      )
      || !pet_bodies_parse(scene,config)
      || !parse_bounded_rules(
        runtime,
        config,
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
  if(config->motion){
    const cJSON *m=cJSON_GetObjectItemCaseSensitive(scene,"motion"),*platforms=cJSON_GetObjectItemCaseSensitive(m,"platforms"),*speed=cJSON_GetObjectItemCaseSensitive(m,"speed");
    static const char *const keys[]={"engine","body","platforms","gravity","lift","speed","charge_ms","charge_var","track"};
    int gravity,lift,low,high,ms;
    config->motion_body=bounded_find_entity(config,json_string(m,"body"));
    runtime->game.charge_var=find_var(runtime,json_string(m,"charge_var"));
    if(!object_keys_allowed(m,keys,9)||strcmp(json_string(m,"engine"),"p4-motion-v1")||config->motion_body<0
      ||runtime->game.charge_var<0||runtime->game.charge_var==runtime->game.score_var||runtime->vars[runtime->game.charge_var].type!=MINIAPP_VAR_INT
      ||!cJSON_IsArray(platforms)||cJSON_GetArraySize(platforms)<1||cJSON_GetArraySize(platforms)>11
      ||!json_int_range(cJSON_GetObjectItemCaseSensitive(m,"gravity"),4,64,&gravity)
      ||!json_int_range(cJSON_GetObjectItemCaseSensitive(m,"lift"),2,16,&lift)
      ||!json_int_range(cJSON_GetObjectItemCaseSensitive(m,"charge_ms"),300,3000,&ms)
      ||!cJSON_IsArray(speed)||cJSON_GetArraySize(speed)!=2
      ||!json_int_range(cJSON_GetArrayItem(speed,0),1,24,&low)||!json_int_range(cJSON_GetArrayItem(speed,1),low,24,&high)
      ||config->solid_tone_mask||cJSON_GetObjectItemCaseSensitive(scene,"sprites"))return false;
    const cJSON *p;cJSON_ArrayForEach(p,platforms){int i=cJSON_IsString(p)?bounded_find_entity(config,p->valuestring):-1;if(i<0||i==config->motion_body||(config->platform_mask&(1u<<i)))return false;config->platform_mask|=1u<<i;}
    config->gravity=gravity;config->lift=lift;config->launch_min=low;config->launch_max=high;config->charge_ms=ms;
    const cJSON *track=cJSON_GetObjectItemCaseSensitive(m,"track");
    if(track){
      static const char *const tk[]={"engine","camera_x","gap","width","tones"};
      const cJSON *gap=cJSON_GetObjectItemCaseSensitive(track,"gap"),*width=cJSON_GetObjectItemCaseSensitive(track,"width"),*tones=cJSON_GetObjectItemCaseSensitive(track,"tones");
      int camera,gap_min,gap_max,width_min,width_max;
      if(!object_keys_allowed(track,tk,5)||strcmp(json_string(track,"engine"),"p4-platform-track-v1")
        ||!json_int_range(cJSON_GetObjectItemCaseSensitive(track,"camera_x"),2,config->width/2,&camera)
        ||!cJSON_IsArray(gap)||cJSON_GetArraySize(gap)!=2||!cJSON_IsArray(width)||cJSON_GetArraySize(width)!=2
        ||!json_int_range(cJSON_GetArrayItem(gap,0),1,6,&gap_min)||!json_int_range(cJSON_GetArrayItem(gap,1),gap_min,6,&gap_max)
        ||!json_int_range(cJSON_GetArrayItem(width,0),2,4,&width_min)||!json_int_range(cJSON_GetArrayItem(width,1),width_min,4,&width_max)
        ||!cJSON_IsArray(tones)||cJSON_GetArraySize(tones)<2||cJSON_GetArraySize(tones)>4)return false;
      const cJSON *tone;cJSON_ArrayForEach(tone,tones){int t;if(!json_int_range(tone,1,4,&t)||(config->tone_mask&(1u<<t)))return false;config->tone_mask|=1u<<t;}
      config->track=true;config->camera_x=camera;config->gap_min=gap_min;config->gap_max=gap_max;config->width_min=width_min;config->width_max=width_max;
    }
    for(int i=0;i<config->entity_count;i++)if(config->entities[i].vx||config->entities[i].vy)return false;
    bool has_charge=false;
    for(int i=0;i<config->rule_count;i++){
      const pet_p4_game_rule_t *r=&config->rules[i];
      if(r->trigger==PET_P4_GAME_TRIGGER_COLLISION||r->trigger==PET_P4_GAME_TRIGGER_BLOCKED||r->trigger==PET_P4_GAME_TRIGGER_TICK)return false;
      for(int j=0;j<r->op_count;j++){
        const pet_p4_game_op_t *o=&r->ops[j];
        if(o->kind==PET_P4_GAME_OP_CHARGE){if(o->entity_index!=config->motion_body||r->trigger!=PET_P4_GAME_TRIGGER_ACTION||strcmp(r->action,runtime->hold_action))return false;has_charge=true;}
        if(o->kind==PET_P4_GAME_OP_MOVE||o->kind==PET_P4_GAME_OP_VELOCITY||o->kind==PET_P4_GAME_OP_ACCELERATE||o->kind==PET_P4_GAME_OP_BOUNCE)return false;
      }
    }
    if(!has_charge||!runtime->hold_action[0])return false;
  }
  if (!pet_p4_game_configure_bounded(
        &runtime->game.engine,
        config,
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
  if (!json || !read_json_file(MINIAPP_CATALOG_PATH, json, MINIAPP_CATALOG_JSON_MAX)) goto fail;
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

static bool catalog_item_visible(const miniapp_catalog_item_t *item) {
  return strcmp(item->widget_id, "music-player") != 0;
}
/* One visibility policy for navigation and transactional fallback. The input
 * array may be the uncommitted next catalog; never consult stale global slots. */
static int catalog_visible_preferred_in(const miniapp_catalog_item_t *items,size_t count,int preferred){
  if(preferred>=0&&(size_t)preferred<count&&catalog_item_visible(&items[preferred]))return preferred;
  for(size_t i=0;i<count;i++)if(catalog_item_visible(&items[i]))return (int)i;
  return -1;
}
static bool catalog_entry_visible(size_t index) {
  return index < g_catalog_count && catalog_item_visible(&g_catalog[index]);
}

static int catalog_visible_index(size_t visible_index) {
  for (size_t i = 0; i < g_catalog_count; ++i) {
    if (!catalog_entry_visible(i)) continue;
    if (visible_index-- == 0) return (int)i;
  }
  return -1;
}

static int catalog_visible_preferred(int preferred) {
  return catalog_visible_preferred_in(g_catalog,g_catalog_count,preferred);
}

static bool activate_catalog_index_in_memory(size_t index) {
  miniapp_runtime_t *next;
  char error[128] = {0};
  if (g_catalog_recovery_blocked||!catalog_entry_visible(index)) return false;
  next = (miniapp_runtime_t *) calloc(1, sizeof(*next));
  if (!next) return false;
  bool loaded;
  {
#if PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED
    pet_storage_cache_work_t boot_cache __attribute__((cleanup(pet_storage_cache_work_finish)))={0};
    (void)pet_storage_cache_work_begin(&boot_cache);
#endif
    loaded=load_catalog_runtime(index,next,error,sizeof(error));
  }
  /* End the package-read lease before private save restoration admits its
   * independent native IO scope. Do not borrow somebody else's occupancy. */
  if(!loaded){
    ESP_LOGW(TAG, "failed to open component %s: %s", g_catalog[index].widget_id, error);
    free(next);
    return false;
  }
  if(!save_app_runtime()){free(next);return false;}
  if(!restore_component_runtime(next)){free(next);return false;}
#if PET_P4_CAPABILITY_EXT
  pet_cext_publish_ticket_t media_ticket={0};
  if(!miniapp_ext_prepare(next,g_catalog[index].slot,g_catalog[index].package_generation,&media_ticket)){
    free(next);return false;
  }
  if(!miniapp_ext_commit(&media_ticket)){
    (void)miniapp_ext_abort(&media_ticket);free(next);return false;
  }
#endif
  miniapp_runtime_lock();
#if PET_COMPONENT_PROGRAM_ENABLED
  component_program_retire_locked();
#endif
  g_runtime = *next; g_image_revision++;
  g_catalog_selected = index;
  miniapp_runtime_unlock();
  free(next);
  ESP_LOGI(
    TAG,
    "opened component id=%s slot=%u",
    g_runtime.widget_id,
    (unsigned int) g_catalog[index].slot
  );
  return true;
}

#if PET_COMPONENT_PROGRAM_RAM_RESUME
static bool resume_component_program_locked(void){
  if(!g_runtime.program.enabled||!g_runtime.program_seed_ready||g_runtime.program_needs_start)return false;
  pet_program_registration_t registration={0};
  if(background_current_locked(&g_runtime,&registration))
    return background_project_locked(&g_runtime); /* Includes the manual STOP latch. */
  if(g_runtime.program_background_owned)return false; /* Never resurrect as foreground. */
  bool held __attribute__((cleanup(component_parent_leave)))=component_parent_enter();if(!held)return false;
  if(!g_component_program_initialized||g_component_program.revision!=g_image_revision)return false;
  if(g_component_program.live&&!g_component_program.retiring&&!g_component_program.starting
      &&g_component_program.scheduled&&!g_component_program.stop_requested)return true;
  /* Session resume validates before writing; reuse the caller-owned seed. */
  if(!pet_component_program_session_resume_snapshot(&g_component_program,&g_runtime.program,g_image_revision,&g_runtime.program_seed))return false;
  g_runtime.program_seed_ready=true;
  if(!component_program_apply_seed(&g_runtime))return false;
  /* Only this explicit navigation prepares a NEW admission. Visibility alone
   * never reaches here; the real backend still owns every execution lease. */
  g_runtime.program_needs_start=true;g_runtime.program_suspended=false;
  refresh_view(&g_runtime);return true;
}
#endif
static bool resume_catalog_index(size_t index) {
  /* Reopening the already loaded media component is navigation, not an
   * install/reset. Rewriting its unchanged catalog can stall the serial task
   * behind Flash I/O long enough to exhaust live audio. Package commit/delete
   * replace g_runtime atomically, so this never bypasses package validation.
   * Games deliberately retain their existing restart-on-open behavior. */
  miniapp_runtime_lock();
#if PET_COMPONENT_PROGRAM_RAM_RESUME
  if(index<g_catalog_count&&g_runtime.active&&g_runtime.program.enabled
      &&g_catalog[index].resource_state==1
      &&!strcmp(g_runtime.widget_id,g_catalog[index].widget_id)
      &&g_runtime.program_content[0]==g_catalog[index].widget_checksum
      &&g_runtime.program_content[1]==g_catalog[index].buttons_checksum
      &&g_runtime.program_content[2]==g_catalog[index].sprites_checksum
      &&resume_component_program_locked()){
    g_catalog_selected=index;miniapp_runtime_unlock();return true;
  }
#endif
  bool resume_media = g_runtime.active && (g_runtime.media||g_runtime.save_version)
    && strcmp(g_runtime.widget_id, g_catalog[index].widget_id) == 0;
#if PET_COMPONENT_PROGRAM_ENABLED
  resume_media=resume_media&&!g_runtime.program.enabled;
#endif
  if (resume_media) g_catalog_selected = index;
  miniapp_runtime_unlock();
  return resume_media;
}

static bool activation_identity_current(size_t index,const miniapp_catalog_item_t *item,
  uint32_t sequence,uint32_t image_revision,uint64_t token){
  return index<g_catalog_count&&g_catalog_sequence==sequence&&g_image_revision==image_revision
    &&!memcmp(&g_catalog[index],item,sizeof(*item))
    &&(!token||(g_activation.active&&g_activation.serial==token&&!g_activation.cancelled));
}

/* Caller owns the state lock and, at runtime, an explicit worker command
 * token. A selected UI request is a reservation, not successful activation. */
static pet_p4_miniapp_activation_result_t activate_catalog_index_prepared(size_t index,uint64_t token) {
  pet_p4_miniapp_activation_result_t result=PET_MINIAPP_ACTIVATION_FAILED;
  miniapp_runtime_t *next=NULL;
#if PET_P4_CAPABILITY_EXT
  pet_cext_publish_ticket_t media_ticket={0};bool media_prepared=false;
#endif
  pet_storage_scope_t storage __attribute__((cleanup(storage_scope_finish)))={0};
  char error[128]={0};
  if(g_catalog_recovery_blocked||g_storage_lane.occupied||!catalog_entry_visible(index)
      ||(g_activation.active&&(!token||g_activation.serial!=token))){
    ESP_LOGW(TAG,"P4 activation rejected gate=1");return result;
  }
  if(token&&g_activation.cancelled)return PET_MINIAPP_ACTIVATION_CANCELLED;
  if(token&&!activation_identity_current(index,&g_activation.item,g_activation.catalog_sequence,
      g_activation.image_revision,token))return PET_MINIAPP_ACTIVATION_CANCELLED;
  if(resume_catalog_index(index))return PET_MINIAPP_ACTIVATION_OPENED;
  if(storage_audio_busy()){ESP_LOGW(TAG,"P4 activation rejected gate=2");return result;}
  if(g_catalog[index].resource_state!=1){
    ESP_LOGW(TAG,"component activation requires verified complete resources; previous runtime retained");return result;
  }
  /* Boot remains synchronous. After configuration, no caller without the
   * explicit worker token may silently put flash I/O back on the main loop. */
  if(g_storage_lane.owner&&(!g_storage_lane.command_active
      ||g_storage_lane.command_owner!=g_storage_lane.owner(g_storage_lane.context))){
    ESP_LOGW(TAG,"P4 activation rejected gate=3");return result;
  }
  if(!storage_scope_begin(&storage,token!=0)){ESP_LOGW(TAG,"P4 activation rejected gate=4");return result;}
  miniapp_catalog_item_t item=g_catalog[index];
  uint32_t sequence=g_catalog_sequence,image_revision=g_image_revision;
  if(token&&(sequence!=g_activation.catalog_sequence||image_revision!=g_activation.image_revision
      ||memcmp(&item,&g_activation.item,sizeof(item))))return PET_MINIAPP_ACTIVATION_CANCELLED;
  if(!save_app_runtime()){ESP_LOGW(TAG,"P4 activation rejected gate=5");return result;}
  if(!pet_storage_scope_advance(&storage,PET_STORAGE_PREPARE)||!pet_storage_scope_unlock(&storage)){
    ESP_LOGW(TAG,"P4 activation rejected gate=6");return result;
  }
  int64_t started_us=esp_timer_get_time(),loaded_us=started_us;
  PET_WIDGET_TRACE_SCOPE(trace_alloc, PET_WTRACE_ALLOC);
  next = (miniapp_runtime_t *) calloc(1, sizeof(*next));
  pet_widget_trace_scope_end(&trace_alloc);
  if(!next){ESP_LOGW(TAG,"P4 activation rejected gate=7");goto cleanup;}
  /* The loader may update resource metadata. It receives an owned copy, not
   * g_catalog, while rendering/catalog queries continue on the state lane.
   * A transient read error cannot replace the old view with a broken stub. */
  miniapp_catalog_item_t prepared_item=item;
  if(!storage_work_current()||!load_catalog_runtime_item(&prepared_item,next,false,error,sizeof(error))){
    ESP_LOGW(TAG,"component activation prepare failed; previous runtime retained: %s",error);
    goto cleanup;
  }
  if(!storage_work_current()||!restore_component_runtime(next)){
    ESP_LOGW(TAG,"component activation save unavailable; previous runtime retained");goto cleanup;
  }
#if PET_P4_CAPABILITY_EXT
  if(!miniapp_ext_prepare(next,item.slot,item.package_generation,&media_ticket))goto cleanup;
  media_prepared=true;
#endif
  loaded_us=esp_timer_get_time();
  if(!pet_storage_scope_lock(&storage)){ESP_LOGW(TAG,"P4 activation rejected gate=8");goto cleanup;}
  if(!activation_identity_current(index,&item,sequence,image_revision,token)){
    result=PET_MINIAPP_ACTIVATION_CANCELLED;goto cleanup;
  }
  if(!storage_work_current()||storage_audio_busy()||!save_app_runtime()){
    ESP_LOGW(TAG,"P4 activation rejected gate=9");goto cleanup;
  }
  /* This is the last cancel point. Persist all old live updates immediately
   * before swapping; only bounded in-memory work follows under the state lock. */
  merge_live_persistent_runtime(next);
#if PET_P4_CAPABILITY_EXT
  if(!miniapp_ext_commit(&media_ticket))goto cleanup;
  media_prepared=false;
#endif
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_DURABLE);
  PET_WIDGET_TRACE_SCOPE(trace_publish, PET_WTRACE_PUBLISH);
  miniapp_runtime_lock();
#if PET_COMPONENT_PROGRAM_ENABLED
  component_program_retire_locked();
#endif
  g_runtime = *next; g_image_revision++;
  g_catalog_selected = index;
  if(token)g_activation.published=true;
  miniapp_runtime_unlock();
  pet_widget_trace_scope_end(&trace_publish);
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_PUBLISHED);
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_CLEANUP);
  (void)pet_storage_scope_unlock(&storage);
  /* NVS selection is only a next-boot navigation hint, not package durability.
   * A failure cannot undo the published runtime or trigger an automatic retry. */
  result=storage_work_current()&&pet_selection_write(sequence,item.widget_id)
    &&catalog_cursor_write(sequence,item.widget_id)?PET_MINIAPP_ACTIVATION_OPENED
    :PET_MINIAPP_ACTIVATION_OPENED_UNSAVED;
  if(result==PET_MINIAPP_ACTIVATION_OPENED_UNSAVED)
    ESP_LOGW(TAG,"component activated; navigation selection persistence failed; no replay");
cleanup:
  /* Even free/allocator cleanup stays off the state lane. The physical job
   * remains reserved until cleanup finishes and the cleanup guard relocks. */
  if(!storage.unlocked)(void)pet_storage_scope_unlock(&storage);
#if PET_P4_CAPABILITY_EXT
  if(media_prepared)(void)miniapp_ext_abort(&media_ticket);
#endif
  free(next);
  ESP_LOGI(TAG,"P4 activation result=%u prepare_us=%llu remainder_us=%llu",
    (unsigned)result,(unsigned long long)(loaded_us-started_us),
    (unsigned long long)(esp_timer_get_time()-loaded_us));
  return result;
}

static bool activate_catalog_index(size_t index) {
  pet_p4_miniapp_activation_result_t result=activate_catalog_index_prepared(index,0);
  return result==PET_MINIAPP_ACTIVATION_OPENED||result==PET_MINIAPP_ACTIVATION_OPENED_UNSAVED;
}

bool pet_p4_miniapp_activation_configure(pet_p4_miniapp_activation_schedule_fn schedule,void *context){
  if(!schedule||pet_p4_miniapp_storage_busy())return false;
  g_activation_schedule=schedule;g_activation_schedule_context=context;return true;
}

pet_p4_miniapp_activation_request_t pet_p4_miniapp_activation_request_selected(void){
  if(g_catalog_recovery_blocked||!g_activation_schedule||g_staging.active
      ||!catalog_entry_visible(g_catalog_selected))return PET_MINIAPP_ACTIVATION_REJECTED;
  size_t index=g_catalog_selected;
  if(g_activation.active){
    if(!g_activation.cancelled&&!strcmp(g_activation.item.widget_id,g_catalog[index].widget_id))
      return PET_MINIAPP_ACTIVATION_REJECTED;
    /* An old queued token may become stale, but a running job retains its
     * physical reservation until its own I/O and cleanup have returned. */
    g_activation.cancelled=true;
    if(g_activation.running||g_storage_lane.occupied)return PET_MINIAPP_ACTIVATION_REJECTED;
  }else if(g_storage_lane.occupied)return PET_MINIAPP_ACTIVATION_REJECTED;
  if(resume_catalog_index(index))return PET_MINIAPP_ACTIVATION_READY;
  if(storage_audio_busy())return PET_MINIAPP_ACTIVATION_REJECTED;
  if(g_activation.serial==UINT64_MAX)return PET_MINIAPP_ACTIVATION_REJECTED;
  g_activation.serial++;
  g_activation.catalog_sequence=g_catalog_sequence;g_activation.image_revision=g_image_revision;
  g_activation.item=g_catalog[index];
  g_activation.active=true;g_activation.cancelled=false;g_activation.published=false;g_activation.running=false;
  if(!g_activation_schedule(g_activation.serial,g_activation_schedule_context)){
    g_activation.active=false;return PET_MINIAPP_ACTIVATION_REJECTED;
  }
  return PET_MINIAPP_ACTIVATION_QUEUED;
}

void pet_p4_miniapp_activation_cancel(void){
  /* Before publication this cancels the load; after publication it cancels only
   * the deferred navigation. It never rolls back a live runtime or frees I/O. */
  if(g_activation.active||g_activation.published)g_activation.cancelled=true;
}

void pet_p4_miniapp_activation_guard(bool component_center_visible){
  if((g_activation.active||g_activation.published)&&(!component_center_visible||g_catalog_selected>=g_catalog_count
      ||strcmp(g_catalog[g_catalog_selected].widget_id,g_activation.item.widget_id)))
    pet_p4_miniapp_activation_cancel();
}

bool pet_p4_miniapp_activation_navigation_current(uint64_t token){
  return token&&token==g_activation.serial&&g_activation.published&&!g_activation.cancelled;
}

bool pet_p4_miniapp_activation_pending(void){return g_activation.active;}

bool pet_p4_miniapp_activation_reject_not_started(uint64_t token){
  if(!token||!g_activation.active||g_activation.serial!=token||g_activation.running)return false;
  g_activation.cancelled=true;g_activation.active=false;return true;
}

pet_p4_miniapp_activation_result_t pet_p4_miniapp_activation_run(uint64_t token){
  if(!token||!g_activation.active||g_activation.serial!=token)return PET_MINIAPP_ACTIVATION_CANCELLED;
  if(!g_storage_lane.owner||!g_storage_lane.command_active
      ||g_storage_lane.command_owner!=g_storage_lane.owner(g_storage_lane.context)){
    (void)pet_p4_miniapp_activation_reject_not_started(token);
    return PET_MINIAPP_ACTIVATION_FAILED;
  }
  if(g_activation.running)return PET_MINIAPP_ACTIVATION_FAILED;
  g_activation.running=true;
  int index=catalog_find(g_catalog,g_catalog_count,g_activation.item.widget_id);
  pet_p4_miniapp_activation_result_t result=PET_MINIAPP_ACTIVATION_CANCELLED;
  if(index>=0&&!g_activation.cancelled)
    result=activate_catalog_index_prepared((size_t)index,token);
  /* A stale worker token cannot release a newer job. No request replaces this
   * occupied slot while prepare is running, including after user cancellation. */
  if(g_activation.serial==token){g_activation.active=false;g_activation.running=false;}
  return result;
}

static bool remove_component(const char *widget_id, char *error, size_t error_size, bool preserve_save);
bool pet_p4_miniapp_remove(const char *widget_id, char *error, size_t error_size) {
  return remove_component(widget_id, error, error_size, false);
}
bool pet_p4_miniapp_remove_preserving_save(const char *widget_id, char *error, size_t error_size) {
  return remove_component(widget_id, error, error_size, true);
}

/* The old selection key chooses active; cursor is a separate sequence-bound
 * hint. Catalog metadata remains the fallback whenever an NVS epoch is stale. */
static void catalog_restore_navigation(const miniapp_catalog_snapshot_t *snapshot,
    char active[48],char cursor[48],bool *selection_exists){
  copy_utf8(active,48,snapshot->active_widget_id);
  copy_utf8(cursor,48,snapshot->selected_widget_id);
  char saved_cursor[48]={0};
  if(catalog_cursor_read(g_catalog_sequence,saved_cursor)
      &&catalog_find(g_catalog,g_catalog_count,saved_cursor)>=0)
    copy_utf8(cursor,48,saved_cursor);
  char last_opened[48]={0};
  bool selection_valid=pet_selection_read(g_catalog_sequence,last_opened,selection_exists);
  /* Import the legacy file only when the NVS key genuinely does not exist.
   * A new catalog sequence always beats a stale old selection epoch. */
  if((selection_valid||(!*selection_exists&&read_file(MINIAPP_LAST_ID_PATH,last_opened,sizeof(last_opened))))
      &&catalog_find(g_catalog,g_catalog_count,last_opened)>=0)
    copy_utf8(active,48,last_opened);
  if(!cursor[0])copy_utf8(cursor,48,active);
  int index=catalog_find(g_catalog,g_catalog_count,cursor);
  if(index>=0&&catalog_entry_visible((size_t)index))g_catalog_selected=(size_t)index;
}

/* Choose membership by catalog commit epoch, never union older generations.
 * Per-item repair may recover matching resources but cannot re-add absent IDs. */
static miniapp_catalog_snapshot_t *catalog_latest_snapshot(miniapp_catalog_snapshot_t *a,
    miniapp_catalog_snapshot_t *b,uint32_t *sequence){
  miniapp_catalog_snapshot_t *latest=a->parsed?a:NULL;
  if(b->parsed&&(!latest||b->sequence>latest->sequence))latest=b;
  *sequence=latest?latest->sequence:0;return latest;
}

esp_err_t pet_p4_miniapp_init(void) {
  if(!miniapp_runtime_lock_init())return ESP_ERR_NO_MEM;
  pet_p4_media_init();
  if(!pet_app_store_init())return ESP_ERR_NO_MEM;
#if PET_COMPONENT_BACKGROUND_ENABLED
  if(!background_init())return ESP_ERR_INVALID_STATE;
#endif
  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX + 1] = {0};
  char error[128] = {0};
  miniapp_catalog_snapshot_t *snapshot0;
  miniapp_catalog_snapshot_t *snapshot1;
  miniapp_catalog_snapshot_t *selected = NULL;
  uint32_t highest_seen_sequence = 0;
#if PET_COMPONENT_PROGRAM_ENABLED
  miniapp_runtime_lock();component_program_retire_locked();miniapp_runtime_unlock();
#endif
  memset(&g_runtime, 0, sizeof(g_runtime));
  memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
  memset(g_catalog, 0, sizeof(g_catalog));
  g_catalog_count = 0;
  g_catalog_selected = 0;
  g_catalog_file_generation = -1;
  g_catalog_sequence = 0;
  g_catalog_recovery_blocked = false;
  g_catalog_seed_pending = false;
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
  selected=catalog_latest_snapshot(snapshot0,snapshot1,&highest_seen_sequence);

  if (selected) {
    bool recovered = recover_catalog_items(selected, selected == snapshot0 ? snapshot1 : snapshot0);
    memcpy(g_catalog, selected->items, sizeof(g_catalog));
    g_catalog_count = selected->count;
    g_catalog_file_generation = selected->file_generation;
    g_catalog_sequence = highest_seen_sequence;
    g_catalog_seed_pending=selected->seed_pending;
    char restore_cursor[48]={0};bool selection_exists=false;
    catalog_restore_navigation(selected,widget_id,restore_cursor,&selection_exists);
    int cursor_index;
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
    if(g_catalog_recovery_blocked){free(snapshot0);free(snapshot1);return ESP_ERR_INVALID_STATE;}
    preferred = catalog_visible_preferred(preferred);
    bool activated = preferred < 0 || activate_catalog_index_in_memory((size_t) preferred);
    /* Import once before interaction starts. Otherwise the first new catalog
     * after upgrade could still be overridden by the untouched legacy file. */
    if(activated&&preferred>=0&&!selection_exists
        &&!pet_selection_write(g_catalog_sequence,g_catalog[preferred].widget_id))
      ESP_LOGW(TAG,"component selection metadata import unavailable; catalog retained");
    /* Activation restoration must not collapse a separately saved cursor. */
    cursor_index=catalog_find(g_catalog,g_catalog_count,restore_cursor);
    if(cursor_index>=0&&catalog_entry_visible((size_t)cursor_index))g_catalog_selected=(size_t)cursor_index;
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
    if(g_catalog_recovery_blocked){free(snapshot0);free(snapshot1);return ESP_ERR_INVALID_STATE;}
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
      || !read_json_file(MINIAPP_WIDGET_PATH, g_staging.widget_json, sizeof(g_staging.widget_json))) {
    /* Old catalog membership (including an empty catalog) is authoritative.
     * Only prove a pristine first boot when every legacy/seed authority is absent;
     * IO errors or leftover user metadata are never an invitation to repopulate. */
    const char *authorities[]={MINIAPP_CATALOG_PATH,MINIAPP_ID_PATH,MINIAPP_WIDGET_PATH,
      MINIAPP_BUTTONS_PATH,MINIAPP_LAST_ID_PATH,MINIAPP_BUILTIN_MARKER_PATH};
    bool pristine=true;
    for(size_t i=0;i<sizeof(authorities)/sizeof(authorities[0]);i++)
      if(stat(authorities[i],&catalog_stat)==0||errno!=ENOENT)pristine=false;
    char prior_selection[48]={0};bool prior_selection_exists=false;
    (void)pet_selection_read(0,prior_selection,&prior_selection_exists);
    g_catalog_seed_pending=pristine&&!prior_selection_exists;
    return ESP_OK;
  }
  (void) read_json_file(MINIAPP_BUTTONS_PATH, g_staging.buttons_json, sizeof(g_staging.buttons_json));
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
  if(g_catalog_recovery_blocked)return ESP_ERR_INVALID_STATE;
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
  miniapp_runtime_lock();
  count = 0;
  for (size_t i = 0; i < g_catalog_count; ++i) if (catalog_entry_visible(i)) ++count;
  miniapp_runtime_unlock();
  return count;
}
size_t pet_p4_miniapp_catalog_capacity(void) {
  /* Retained hidden data still consumes a storage slot. Report the visible
   * capacity to clients that only receive the visible inventory projection. */
  size_t hidden=0;
  miniapp_runtime_lock();
  for(size_t i=0;i<g_catalog_count;i++)if(!catalog_entry_visible(i))++hidden;
  miniapp_runtime_unlock();
  return PET_P4_MINIAPP_CATALOG_MAX-hidden;
}

size_t pet_p4_miniapp_catalog_selected(void) {
  size_t selected;
  miniapp_runtime_lock();
  selected = 0;
  for (size_t i = 0; i < g_catalog_selected; ++i) if (catalog_entry_visible(i)) ++selected;
  miniapp_runtime_unlock();
  return selected;
}

bool pet_p4_miniapp_catalog_get(size_t index, pet_p4_miniapp_catalog_entry_t *out) {
  bool available = false;
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  miniapp_runtime_lock();
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
  miniapp_runtime_unlock();
  return available;
}

static bool catalog_select_id_queued(const char *id);
void pet_p4_miniapp_catalog_focus_active(void) {
  char id[48]={0};miniapp_runtime_lock();
  int index=g_runtime.active?catalog_find(g_catalog,g_catalog_count,g_runtime.widget_id):-1;
  if(index>=0&&catalog_entry_visible((size_t)index))copy_utf8(id,sizeof(id),g_runtime.widget_id);
  miniapp_runtime_unlock();if(id[0])(void)catalog_select_id_queued(id);
}

/* Cursor persistence is a control-plane transaction, outside the runtime mutex
 * and never admitted during audio or another storage job. Failed persistence
 * leaves the published durable cursor unchanged. Local cursor changes keep
 * only the latest dirty token and retry through the existing queue, at 1 Hz. */
static bool catalog_select_id_durable(const char *id,uint64_t token){
  if(!safe_widget_id(id)||g_catalog_recovery_blocked||(token&&token!=g_catalog_cursor_serial))return false;
  pet_storage_scope_t storage __attribute__((cleanup(storage_scope_finish)))={0};
  if(storage_audio_busy()||!storage_scope_begin(&storage,false))return false;
  uint32_t sequence=g_catalog_sequence;
  miniapp_runtime_lock();int index=catalog_find(g_catalog,g_catalog_count,id);
  bool valid=index>=0&&catalog_entry_visible((size_t)index);miniapp_runtime_unlock();
  if(!valid||!pet_storage_scope_unlock(&storage))return false;
  if(!storage_work_current()||!catalog_cursor_write(sequence,id))return false;
  if(!pet_storage_scope_lock(&storage)||!storage_work_current()||g_catalog_sequence!=sequence)return false;
  miniapp_runtime_lock();index=catalog_find(g_catalog,g_catalog_count,id);
  valid=index>=0&&catalog_entry_visible((size_t)index)&&(!token||token==g_catalog_cursor_serial);
  if(valid)g_catalog_selected=(size_t)index;
  miniapp_runtime_unlock();return valid;
}
bool pet_p4_miniapp_catalog_cursor_configure(pet_p4_miniapp_catalog_cursor_schedule_fn schedule,void *context){
  if(!schedule||pet_p4_miniapp_storage_busy())return false;
  g_catalog_cursor_schedule=schedule;g_catalog_cursor_context=context;return true;
}
static bool catalog_select_id_queued(const char *id){
  if(g_catalog_recovery_blocked||!safe_widget_id(id))return false;
  int index=catalog_find(g_catalog,g_catalog_count,id);
  if(index<0||!catalog_entry_visible((size_t)index))return false;
  if((size_t)index==g_catalog_selected)return true;
  if(!g_catalog_cursor_schedule||g_catalog_cursor_serial==UINT64_MAX)return false;
  uint64_t token=g_catalog_cursor_serial+1;
  if(!g_catalog_cursor_schedule(token,g_catalog_cursor_context))return false;
  miniapp_runtime_lock();g_catalog_cursor_serial=token;g_catalog_cursor_sequence=g_catalog_sequence;g_catalog_selected=(size_t)index;g_catalog_cursor_dirty=true;
  g_catalog_cursor_retry_ms=esp_timer_get_time()/1000ULL+1000;
  miniapp_runtime_unlock();return true;
}
static void catalog_cursor_retry(uint64_t now_ms){
  if(!g_catalog_cursor_dirty)return;
  if(g_catalog_recovery_blocked||g_catalog_cursor_sequence!=g_catalog_sequence){g_catalog_cursor_dirty=false;return;}
  if(now_ms<g_catalog_cursor_retry_ms)return;
  g_catalog_cursor_retry_ms=now_ms+1000;
  if(g_catalog_cursor_schedule)(void)g_catalog_cursor_schedule(g_catalog_cursor_serial,g_catalog_cursor_context);
}
bool pet_p4_miniapp_catalog_cursor_run(uint64_t token){
  if(!g_catalog_cursor_dirty||!token||token!=g_catalog_cursor_serial||g_catalog_cursor_sequence!=g_catalog_sequence||g_catalog_selected>=g_catalog_count
      ||!g_storage_lane.command_active||!g_storage_lane.owner
      ||g_storage_lane.command_owner!=g_storage_lane.owner(g_storage_lane.context))return false;
  char id[48];copy_utf8(id,sizeof(id),g_catalog[g_catalog_selected].widget_id);
  bool ok=catalog_select_id_durable(id,token);
  if(ok&&token==g_catalog_cursor_serial)g_catalog_cursor_dirty=false;
  return ok;
}
bool pet_p4_miniapp_catalog_select_durable(size_t index){
  int at=catalog_visible_index(index);
  if(at<0)return false;
  char id[48];copy_utf8(id,sizeof(id),g_catalog[at].widget_id);
  bool ok=catalog_select_id_durable(id,0);
  if(ok){g_catalog_cursor_dirty=false;if(g_catalog_cursor_serial<UINT64_MAX)g_catalog_cursor_serial++;}
  return ok;
}
bool pet_p4_miniapp_catalog_select(size_t index) {
  char id[PET_P4_MINIAPP_WIDGET_ID_MAX]={0};
  miniapp_runtime_lock();int raw_index=catalog_visible_index(index);
  if(raw_index>=0)copy_utf8(id,sizeof(id),g_catalog[raw_index].widget_id);
  miniapp_runtime_unlock();return id[0]&&catalog_select_id_queued(id);
}
bool pet_p4_miniapp_catalog_move(int delta) {
  char id[PET_P4_MINIAPP_WIDGET_ID_MAX]={0};
  miniapp_runtime_lock();
  if(g_catalog_count&&delta){
    size_t target=g_catalog_selected;
    for(size_t i=0;i<g_catalog_count;i++){
      target=delta>0?(target+1)%g_catalog_count:(target+g_catalog_count-1)%g_catalog_count;
      if(catalog_entry_visible(target))break;
    }
    if(catalog_entry_visible(target)&&(target!=g_catalog_selected||g_catalog_count==1))
      copy_utf8(id,sizeof(id),g_catalog[target].widget_id);
  }
  miniapp_runtime_unlock();return id[0]&&catalog_select_id_queued(id);
}

#include "pet_p4_catalog_admin.inc"

bool pet_p4_miniapp_catalog_activate_selected(void) {
  miniapp_runtime_lock();
  size_t selected = g_catalog_selected;
  miniapp_runtime_unlock();
  return activate_catalog_index(selected);
}

bool pet_p4_miniapp_catalog_activate_id(const char *widget_id) {
  if(pet_p4_miniapp_storage_busy())return false;
  if (!safe_widget_id(widget_id)||g_staging.active) return false;
  miniapp_runtime_lock();
  int index = catalog_find(g_catalog, g_catalog_count, widget_id);
  miniapp_runtime_unlock();
  return index >= 0 && activate_catalog_index((size_t) index);
}

static bool transfer_matches(const cJSON *payload) {
  return g_staging.active
    && strcmp(json_string(payload, "transferId"), g_staging.transfer_id) == 0;
}

#if PET_P4_STORAGE_PRESERVE_UNCHANGED
static bool g_install_unchanged;
bool pet_p4_miniapp_install_unchanged(void){return g_install_unchanged;}
#endif

bool pet_p4_miniapp_install_begin(const cJSON *payload, char *error, size_t error_size) {
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
  g_install_unchanged=false;
  const cJSON *preserve=cJSON_GetObjectItemCaseSensitive(payload,"preserveUnchanged");
  if(preserve&&!cJSON_IsBool(preserve)){
    set_error(error,error_size,"preserveUnchanged must be a boolean; existing transfer preserved");return false;
  }
#endif
  if(g_catalog_recovery_blocked||pet_p4_miniapp_storage_busy()){
    set_error(error,error_size,"component storage is busy or recovery blocked; existing transfer preserved");return false;
  }
  const char *transfer_id = json_string(payload, "transferId");
  const char *widget_id = json_string(payload, "widgetId");
  if (!g_builtin_sync_in_progress && strcmp(widget_id, "music-player") == 0) {
    set_error(error, error_size, "music player is unavailable in this release");
    return false;
  }
  /* Rejected requests do not own the accepted transaction's staging. */
  if (!safe_id(transfer_id, sizeof(g_staging.transfer_id))
      || !safe_widget_id(widget_id)) {
    set_error(error, error_size, "invalid transferId or widgetId; existing transfer preserved");
    return false;
  }
  const bool repair_resources = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "repairResources"));
  if (repair_resources && (pet_p4_audio_active() || pet_p4_audio_playback_active()
      || pet_p4_audio_conversation_active() || pet_p4_audio_music_active())) {
    set_error(error, error_size, "audio is active; resource repair deferred without changing data");
    return false;
  }
  memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
#if PET_P4_CAPABILITY_EXT
  pet_cmedia_stage_reset();
#endif
  g_staging.active = true;
  g_staging.repair_resources = repair_resources;
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
  g_staging.preserve_unchanged=cJSON_IsTrue(preserve);
#endif
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
  if (size && (!cJSON_IsNumber(size) || !isfinite(size->valuedouble)
               || size->valuedouble < 0 || size->valuedouble != (double) decoded_len)) {
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
  if(pet_p4_miniapp_storage_busy()){
    set_error(error,error_size,"component storage is busy; existing transfer preserved");return false;
  }
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
  }
#if PET_P4_CAPABILITY_EXT
  else if (pet_cext_media_path_kind(path, NULL) != PET_CEXT_MEDIA_NONE) {
    if (!miniapp_ext_install_chunk(payload, path, data, index, error, error_size)) return false;
  }
#endif
  else if (strcmp(path, "component.json") != 0 && strcmp(path, "negative-screen.json") != 0
             && strcmp(path, "share.json") != 0) {
    set_error(error, error_size, "P4 mini-app package contains an unsupported file");
    return false;
  }
  return true;
}

/* A same-version hot update must represent every value the old declaration
 * can produce, not just the value sampled during prepare. Otherwise an input
 * arriving during flash I/O could become unrepresentable after durability.
 * The storage scope pins identity/declarations; this short lock reads metadata
 * only. Additive fields are fine; narrowing/removal requires an explicit,
 * separately supported migration rather than a silent partial merge. */
static bool hot_update_save_compatible(miniapp_runtime_t *next){
  if(!next)return false;
  bool compatible=true;
  miniapp_runtime_lock();
  if(g_runtime.active&&g_runtime.save_version
      &&next->save_version==g_runtime.save_version
      &&!strcmp(next->widget_id,g_runtime.widget_id)){
    for(int i=0;compatible&&i<g_runtime.var_count;i++)if(g_runtime.save_vars&(1u<<i)){
      int target=find_var(next,g_runtime.vars[i].name);
      compatible=target>=0&&(next->save_vars&(1u<<target))
        &&next->vars[target].type==g_runtime.vars[i].type;
      if(compatible&&g_runtime.vars[i].type==MINIAPP_VAR_INT)
        compatible=next->vars[target].int_min<=g_runtime.vars[i].int_min
          &&next->vars[target].int_max>=g_runtime.vars[i].int_max;
      else if(compatible)compatible=runtime_string_limit(next,(unsigned)target)>=runtime_string_limit(&g_runtime,(unsigned)i);
    }
    for(int i=0;compatible&&i<g_runtime.state_count;i++){
      int target=find_name(next->states,next->state_count,g_runtime.states[i]);
      int old_restore=g_runtime.save_states[i];
      compatible=target>=0&&old_restore>=0&&old_restore<g_runtime.state_count;
      if(compatible){
        int new_restore=next->save_states[target];
        compatible=new_restore>=0&&new_restore<next->state_count
          &&!strcmp(next->states[new_restore],g_runtime.states[old_restore]);
      }
    }
    if(compatible&&g_runtime.save_page){
      compatible=next->save_page;
      for(int i=0;compatible&&i<g_runtime.page_count;i++)
        compatible=find_name(next->pages,next->page_count,g_runtime.pages[i])>=0;
    }
  }
  miniapp_runtime_unlock();return compatible;
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

#if PET_P4_STORAGE_PRESERVE_UNCHANGED
static bool unchanged_storage_current(void){return storage_work_current()&&!storage_audio_busy();}
/* A checksum can reject a candidate cheaply, never authorize it. All accepted
 * filesystem bytes and EOF are read through the product HAL cache bypass;
 * native SPIFFS coherency is unchanged, not raw-media scrubbing. The existing
 * storage job pins catalog/staging identity; runtime inputs stay untouched. */
static int preserve_unchanged_package(const miniapp_runtime_t *next){
  if(!next||!g_staging.preserve_unchanged||g_staging.repair_resources||g_builtin_sync_in_progress)return 0;
  int at=catalog_find(g_catalog,g_catalog_count,g_staging.widget_id);
  if(at<0)return 0;
  miniapp_catalog_item_t item=g_catalog[at];
  const char *title=next->view.title[0]?next->view.title:g_staging.widget_id;
  miniapp_runtime_lock();
  bool eligible=g_runtime.active&&g_catalog_selected==(size_t)at
    &&!strcmp(g_runtime.widget_id,g_staging.widget_id);
  uint32_t revision=g_image_revision;
  miniapp_runtime_unlock();
  if(!eligible||item.resource_state!=1||!item.widget_checksum||!item.buttons_checksum
      ||strcmp(item.title,title)
      ||item.widget_checksum!=miniapp_checksum(g_staging.widget_json,g_staging.widget_len)
      ||item.buttons_checksum!=miniapp_checksum(g_staging.buttons_json,g_staging.buttons_len)
      ||item.sprites_checksum!=sprite_pack_checksum(&g_staging.sprites))return 0;
  if(!unchanged_storage_current())return -1;
  pet_storage_physical_scope_t physical={0};
  if(!pet_storage_physical_begin(&physical))return -1;
  char path[48];
  catalog_slot_path(item.slot,item.package_generation,false,false,path,sizeof(path));
  int result=pet_file_exact(path,g_staging.widget_json,g_staging.widget_len,NULL,0,unchanged_storage_current);
  if(result==1){
    catalog_slot_path(item.slot,item.package_generation,true,false,path,sizeof(path));
    result=pet_file_exact(path,g_staging.buttons_json,g_staging.buttons_len,NULL,0,unchanged_storage_current);
  }
  for(unsigned i=0;result==1&&i<g_staging.sprites.count;i++){
    const pet_p4_miniapp_sprite_t *s=&g_staging.sprites.items[i];
    if((size_t)s->data_offset>g_staging.sprites.data_length
        ||s->data_length>g_staging.sprites.data_length-(size_t)s->data_offset){result=0;break;}
    uint8_t header[MINIAPP_SPRITE_FILE_HEADER_SIZE]={'P','4','S','1',s->frame_width,s->frame_height,s->frames,s->fps};
    catalog_sprite_path(item.slot,item.package_generation,i,false,path,sizeof(path));
    result=pet_file_exact(path,s->fps?header:NULL,s->fps?sizeof(header):0,
      g_staging.sprites.pixels+s->data_offset,s->data_length,unchanged_storage_current);
  }
  if(!pet_storage_physical_finish(&physical)||!unchanged_storage_current())return -1;
  if(result!=1)return result;
  miniapp_runtime_lock();
  eligible=g_runtime.active&&g_catalog_selected==(size_t)at&&g_image_revision==revision
    &&!strcmp(g_runtime.widget_id,g_staging.widget_id);
  miniapp_runtime_unlock();
  return eligible?1:-1;
}
#endif

bool pet_p4_miniapp_install_commit(const cJSON *payload, char *error, size_t error_size) {
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
  g_install_unchanged=false;
#endif
  if (!transfer_matches(payload)
      || strcmp(json_string(payload, "widgetId"), g_staging.widget_id) != 0
      || g_staging.widget_len == 0) {
    set_error(error, error_size, "mini-app staging is incomplete or invalid; existing transfer preserved");
    return false;
  }
  if (g_catalog_recovery_blocked) {
    set_error(error, error_size, "component directories need recovery; stored packages were preserved");
    return false;
  }
  pet_storage_scope_t storage __attribute__((cleanup(storage_scope_finish))) = {0};
  if((!g_builtin_sync_in_progress&&storage_audio_busy())
      ||!storage_scope_begin(&storage,false)){
    set_error(error,error_size,"audio or component storage is busy; staged package preserved");return false;
  }
  g_storage_failure[0] = '\0';
  const int base_generation=g_catalog_file_generation;
  const uint32_t base_sequence=g_catalog_sequence;
  char prior_active[PET_P4_MINIAPP_WIDGET_ID_MAX]={0};
  if(g_runtime.active)copy_utf8(prior_active,sizeof(prior_active),g_runtime.widget_id);
  /* Pin staging and catalog with the storage job, not the render/state mutex.
   * Runtime input remains live. Cache its current save before any cold I/O. */
  if(!save_app_runtime()){
    set_error(error,error_size,"active component save is unavailable; staged package preserved");return false;
  }
  if(!pet_storage_scope_advance(&storage,PET_STORAGE_PREPARE)
      ||!pet_storage_scope_unlock(&storage)){
    set_error(error,error_size,"storage command owner was not current");return false;
  }
  miniapp_runtime_t *next;
  PET_WIDGET_TRACE_SCOPE(trace_catalog_alloc, PET_WTRACE_ALLOC);
  miniapp_catalog_item_t *next_catalog __attribute__((cleanup(catalog_scratch_free))) = calloc(PET_P4_MINIAPP_CATALOG_MAX, sizeof(*next_catalog));
  pet_widget_trace_scope_end(&trace_catalog_alloc);
  if (!next_catalog) { set_error(error,error_size,"not enough memory for component catalog");return false; }
  size_t next_count;
  size_t catalog_index;
  int slot;
  uint8_t package_generation;
  bool new_component = false;
  PET_WIDGET_TRACE_SCOPE(trace_alloc, PET_WTRACE_ALLOC);
  next = (miniapp_runtime_t *) calloc(1, sizeof(*next));
  pet_widget_trace_scope_end(&trace_alloc);
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
    if (!read_json_file(old_buttons_path, g_staging.buttons_json, sizeof(g_staging.buttons_json))
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
#if PET_P4_CAPABILITY_EXT
  if(!pet_cmedia_stage_matches(&next->cext,error,error_size)){free(next);return false;}
#endif
  if(!hot_update_save_compatible(next)){
    free(next);
    set_error(error,error_size,"hot update changes the saved schema incompatibly; original package and saved state preserved");
    return false;
  }
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
  int unchanged=preserve_unchanged_package(next);
  if(unchanged<0){
    free(next);set_error(error,error_size,"physical verification lost storage ownership; staged package preserved");return false;
  }
  if(unchanged==1){
    free(next);memset(&g_staging,0,sizeof(g_staging));
    g_image_used=0;g_image_next=0;g_image_id[0]=0;g_install_unchanged=true;
    return true;
  }
#endif
#if PET_COMPONENT_BACKGROUND_ENABLED
  if(!background_prepare_replace(next)){
    free(next);set_error(error,error_size,"background owner could not retire/checkpoint; original data retained");return false;
  }
#endif
  memcpy(next_catalog, g_catalog, sizeof(g_catalog));
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
  if (!write_spare_package_json((size_t)slot,package_generation,false,
                                g_staging.widget_json,g_staging.widget_len)
      || !write_spare_package_json((size_t)slot,package_generation,true,
                                   g_staging.buttons_json,g_staging.buttons_len)
      || !persist_sprite_pack((size_t) slot, package_generation, &g_staging.sprites)
#if PET_P4_CAPABILITY_EXT
      || !pet_cmedia_persist((size_t) slot, package_generation, &next->cext)
#endif
      || (memset(next, 0, sizeof(*next)),
          !load_catalog_runtime_item(
            &next_catalog[catalog_index],
            next,
            false,
            error,
            error_size
          ))
      ) {
    free(next);
    if (!error || !error[0]) {
      if (error && error_size) snprintf(error, error_size, "component commit failed: %s", g_storage_failure[0] ? g_storage_failure : "catalog allocation/validation");
    }
    memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
    return false;
  }
  /* Preload the target save outside the state lane. Publication re-reads only
   * RAM after saving the still-running old runtime, so input during flash work
   * is never overwritten by a stale prepare-time snapshot. */
  if(!storage_work_current()||!restore_component_runtime(next)){
    free(next);set_error(error,error_size,"component save is unavailable; original package and data preserved");return false;
  }
  miniapp_catalog_commit_t commit;
  if(!storage_work_current()||(storage_audio_busy()&&!g_builtin_sync_in_progress)){
    free(next);set_error(error,error_size,"audio started; catalog commit deferred and staged package preserved");return false;
  }
#if PET_P4_CAPABILITY_EXT
  pet_cext_publish_ticket_t media_ticket={0};
  miniapp_runtime_lock();
  const bool ext_swap=!g_staging.repair_resources||!g_runtime.active||!strcmp(g_runtime.widget_id,g_staging.widget_id);
  miniapp_runtime_unlock();
  if(ext_swap&&!miniapp_ext_prepare(next,next_catalog[catalog_index].slot,next_catalog[catalog_index].package_generation,&media_ticket)){
    free(next);set_error(error,error_size,"component media prepare failed; existing runtime and catalog preserved");return false;
  }
  if(!storage_work_current()||(storage_audio_busy()&&!g_builtin_sync_in_progress)){
    if(ext_swap)(void)miniapp_ext_abort(&media_ticket);
    free(next);set_error(error,error_size,"component publication cancelled before catalog commit; existing runtime preserved");return false;
  }
#endif
  const char *next_cursor=g_staging.repair_resources&&g_catalog_selected<g_catalog_count
    ?g_catalog[g_catalog_selected].widget_id:g_staging.widget_id;
  if(!write_catalog_snapshot_at(next_catalog,next_count,
       g_staging.repair_resources&&prior_active[0]?prior_active:g_staging.widget_id,
       next_cursor,g_builtin_sync_in_progress&&g_catalog_seed_pending,
       base_generation,base_sequence,&commit)){
#if PET_P4_CAPABILITY_EXT
    if(ext_swap)(void)miniapp_ext_abort(&media_ticket);
#endif
    free(next);set_error(error,error_size,g_catalog_recovery_blocked
      ? "component catalog outcome unknown; inspect only, do not replay"
      : "component catalog not committed; existing package preserved");return false;
  }
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_DURABLE);
  (void)pet_storage_scope_lock(&storage);
  (void)save_app_runtime();merge_live_persistent_runtime(next);
#if PET_P4_CAPABILITY_EXT
  if(ext_swap&&!miniapp_ext_commit(&media_ticket)){
    g_catalog_recovery_blocked=true;
    /* Terminal reconciliation quarantines the durable result. This is NOT a
     * successful runtime/catalog publication, nor permission to delete old data. */
    (void)pet_storage_scope_advance(&storage,PET_STORAGE_PUBLISHED);
    (void)pet_storage_scope_advance(&storage,PET_STORAGE_CLEANUP);
    (void)pet_storage_scope_unlock(&storage);
    (void)miniapp_ext_abort(&media_ticket);
    free(next);set_error(error,error_size,"durable catalog media publication failed; old runtime retained, recovery required");return false;
  }
#endif
  PET_WIDGET_TRACE_SCOPE(trace_publish, PET_WTRACE_PUBLISH);
  miniapp_runtime_lock();
#if PET_P4_CAPABILITY_EXT
  if(ext_swap){
#else
  if (!g_staging.repair_resources || !g_runtime.active
      || strcmp(g_runtime.widget_id, g_staging.widget_id) == 0) {
#endif
#if PET_COMPONENT_PROGRAM_ENABLED
    component_program_retire_locked();
#endif
    g_runtime = *next; g_image_revision++;
    g_catalog_selected = catalog_index;
  }
  memcpy(g_catalog, next_catalog, sizeof(g_catalog));
  g_catalog_count = next_count;
  g_catalog_file_generation=commit.generation;g_catalog_sequence=commit.sequence;
  if(!g_builtin_sync_in_progress)g_catalog_seed_pending=false;
  miniapp_runtime_unlock();
#if PET_P4_CAPABILITY_EXT
  miniapp_ext_installed(next);
#endif
  pet_widget_trace_scope_end(&trace_publish);
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_PUBLISHED);
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_CLEANUP);
  (void)pet_storage_scope_unlock(&storage);
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

  miniapp_runtime_lock();
  memcpy(g_catalog, ordered, sizeof(g_catalog));
  int selected = g_runtime.active
    ? catalog_find(g_catalog, g_catalog_count, g_runtime.widget_id)
    : -1;
  g_catalog_selected = selected >= 0 ? (size_t) selected : 0;
  miniapp_runtime_unlock();
  return true;
}

esp_err_t pet_p4_miniapp_sync_builtins(void) {
  if (g_catalog_recovery_blocked) return ESP_ERR_INVALID_STATE;
  char marker[128] = {0};
  char expected_marker[128] = {0};
  char restore_widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  char restore_selected_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  const bool seed_missing=g_catalog_seed_pending;
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
      && strcmp(marker, expected_marker) == 0 && !seed_missing) {
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
  if(g_catalog_selected<g_catalog_count)
    copy_utf8(restore_selected_id,sizeof(restore_selected_id),g_catalog[g_catalog_selected].widget_id);

  for (size_t i = 0;
       i < sizeof(g_retired_builtin_ids) / sizeof(g_retired_builtin_ids[0]);
       i += 1) {
    /* Established user membership is never retired/repopulated by an OTA. */
    if(!seed_missing)break;
    if (catalog_find(g_catalog, g_catalog_count, g_retired_builtin_ids[i]) < 0) continue;
    error[0] = '\0';
    if (!pet_p4_miniapp_remove_preserving_save(g_retired_builtin_ids[i], error, sizeof(error))) {
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
    if(g_catalog_recovery_blocked)break;
    const char *widget_id = json_string(package, "id");
    const int existing = catalog_find(g_catalog,g_catalog_count,widget_id);
    /* Missing membership is the durable uninstall tombstone, independent of
     * firmware build/bundle markers. Explicit PC/factory install can add it. */
    if(existing<0&&!seed_missing)continue;
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
  if(g_catalog_recovery_blocked){cJSON_Delete(bundle);return ESP_ERR_INVALID_STATE;}

  if (seed_missing && reorder_catalog_for_builtin_bundle(components)) {
    catalog_changed = true;
  }

  if (catalog_changed && !restore_active_after_builtin_sync(restore_widget_id)) {
    ESP_LOGW(TAG, "built-in components synced but active component could not be restored");
    cJSON_Delete(bundle);
    return ESP_ERR_INVALID_STATE;
  }
  if(catalog_changed&&restore_selected_id[0]){
    int selected=catalog_find(g_catalog,g_catalog_count,restore_selected_id);
    if(selected>=0&&!catalog_select_id_durable(restore_selected_id,0)){
      cJSON_Delete(bundle);return ESP_ERR_INVALID_STATE;
    }
  }
  if (failed) {
    /* Keep successful additions; no completion marker means the failed ones
     * are retried on a later boot. Never hide all packages after one failure. */
    ESP_LOGW(TAG, "built-in sync incomplete failed=%u updated=%u catalog=%u",
      (unsigned int) failed, (unsigned int) updated, (unsigned int) g_catalog_count);
    cJSON_Delete(bundle);
    return ESP_FAIL;
  }
  if(seed_missing){
    /* Seal completion in the SAME catalog generations that own membership.
     * A crash before this commit can retry first seed; after it, no marker or
     * new firmware may invent absent packages. Empty catalogs stay empty. */
    g_catalog_seed_pending=false;
    if(!persist_catalog_snapshot(g_catalog,g_catalog_count,
        g_runtime.active?g_runtime.widget_id:NULL)){
      g_catalog_seed_pending=true;cJSON_Delete(bundle);return ESP_FAIL;
    }
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
  if(!storage_work_current())return false;
  PET_WIDGET_TRACE_SCOPE(trace_cleanup, PET_WTRACE_CLEANUP);
  PET_WIDGET_TRACE_SCOPE(trace_remove, PET_WTRACE_FS_REMOVE);
  if (remove(path) == 0) return true;
  return errno == ENOENT;
}

#define MINIAPP_DELETE_TARGETS (6+MINIAPP_PACKAGE_GENERATION_COUNT*(2+PET_P4_MINIAPP_SPRITE_MAX))
/* Keep the scanner's original per-plan bound. Expanded image/keyframe slots
 * use at most two exact-name plans under the same storage owner. */
_Static_assert(MINIAPP_DELETE_TARGETS<=2*PET_FILE_CLEANUP_MAX_TARGETS,"bounded package cleanup batches");
static void component_cleanup_targets(size_t slot,pet_file_cleanup_target_t targets[MINIAPP_DELETE_TARGETS]) {
  size_t next=0;
  snprintf(targets[next++].path,PET_FILE_CLEANUP_PATH_BYTES,"%s",MINIAPP_ID_PATH);
  snprintf(targets[next++].path,PET_FILE_CLEANUP_PATH_BYTES,"%s",MINIAPP_ID_TMP_PATH);
  for(uint8_t generation=0;generation<MINIAPP_PACKAGE_GENERATION_COUNT;generation++){
    /* Keep both committed generations available to the prior catalog. */
    catalog_slot_path(slot,generation,false,true,targets[next].path,sizeof(targets[next].path));next++;
    catalog_slot_path(slot,generation,true,true,targets[next].path,sizeof(targets[next].path));next++;
    for(uint8_t image=0;image<PET_P4_MINIAPP_SPRITE_MAX;image++){
      catalog_sprite_path(slot,generation,image,true,targets[next].path,sizeof(targets[next].path));next++;
    }
  }
  snprintf(targets[next++].path,PET_FILE_CLEANUP_PATH_BYTES,"%s",MINIAPP_WIDGET_PATH);
  snprintf(targets[next++].path,PET_FILE_CLEANUP_PATH_BYTES,"%s",MINIAPP_WIDGET_TMP_PATH);
  snprintf(targets[next++].path,PET_FILE_CLEANUP_PATH_BYTES,"%s",MINIAPP_BUTTONS_PATH);
  snprintf(targets[next++].path,PET_FILE_CLEANUP_PATH_BYTES,"%s",MINIAPP_BUTTONS_TMP_PATH);
}

static bool remove_component(const char *widget_id, char *error, size_t error_size, bool preserve_save) {
  pet_storage_scope_t storage __attribute__((cleanup(storage_scope_finish))) = {0};
  if((!g_builtin_sync_in_progress&&storage_audio_busy())
      ||!storage_scope_begin(&storage,false)){
    set_error(error,error_size,"audio or component storage is busy; original data preserved");return false;
  }
  if(g_catalog_recovery_blocked){
    set_error(error,error_size,"component directories need recovery; original data preserved");return false;
  }
  const int base_generation=g_catalog_file_generation;
  const uint32_t base_sequence=g_catalog_sequence;
  char active_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  char next_active_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  PET_WIDGET_TRACE_SCOPE(trace_catalog_alloc, PET_WTRACE_ALLOC);
  miniapp_catalog_item_t *next_catalog __attribute__((cleanup(catalog_scratch_free))) = calloc(PET_P4_MINIAPP_CATALOG_MAX, sizeof(*next_catalog));
  pet_widget_trace_scope_end(&trace_catalog_alloc);
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

  miniapp_runtime_lock();
  if (g_runtime.active) {
    copy_utf8(active_id, sizeof(active_id), g_runtime.widget_id);
  }
  memcpy(next_catalog, g_catalog, sizeof(g_catalog));
  next_count = g_catalog_count;
  next_selected = g_catalog_selected;
  miniapp_runtime_unlock();

  catalog_index = catalog_find(next_catalog, next_count, widget_id);
  if (catalog_index < 0) {
    memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
    return true;
  }
  removed_slot = next_catalog[catalog_index].slot;
#if PET_COMPONENT_BACKGROUND_ENABLED
  if(!background_prepare_remove(widget_id)){
    set_error(error,error_size,"background owner could not retire/checkpoint; original component retained");return false;
  }
#endif
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
  if(!save_app_runtime()){
    set_error(error,error_size,"active component save is unavailable; original component preserved");return false;
  }
  if(!pet_storage_scope_advance(&storage,PET_STORAGE_PREPARE)
      ||!pet_storage_scope_unlock(&storage)){
    set_error(error,error_size,"storage command owner was not current");return false;
  }
  if (removing_active) {
    int visible=catalog_visible_preferred_in(next_catalog,next_count,(int)next_selected);
    if (visible >= 0) {
      next_selected=(size_t)visible;
      copy_utf8(next_active_id, sizeof(next_active_id), next_catalog[next_selected].widget_id);
      PET_WIDGET_TRACE_SCOPE(trace_alloc, PET_WTRACE_ALLOC);
      next_runtime = (miniapp_runtime_t *) calloc(1, sizeof(*next_runtime));
      pet_widget_trace_scope_end(&trace_alloc);
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
    int visible=catalog_visible_preferred_in(next_catalog,next_count,0);
    if(visible>=0){next_selected=(size_t)visible;copy_utf8(next_active_id,sizeof(next_active_id),next_catalog[next_selected].widget_id);}
  }
  if(removing_active&&next_runtime&&(!storage_work_current()||!restore_component_runtime(next_runtime))){
    free(next_runtime);set_error(error,error_size,"next component save is unavailable; original component preserved");return false;
  }
  if(!storage_work_current()||(storage_audio_busy()&&!g_builtin_sync_in_progress)){
    free(next_runtime);set_error(error,error_size,"audio started; catalog deletion deferred and original data preserved");return false;
  }
  miniapp_catalog_commit_t commit;
#if PET_P4_CAPABILITY_EXT
  pet_cext_publish_ticket_t media_ticket={0};
  if(removing_active&&!miniapp_ext_prepare(next_runtime,next_runtime?next_catalog[next_selected].slot:0,
      next_runtime?next_catalog[next_selected].package_generation:0,&media_ticket)){
    free(next_runtime);set_error(error,error_size,"replacement media prepare failed; original component preserved");return false;
  }
  if(!storage_work_current()||(storage_audio_busy()&&!g_builtin_sync_in_progress)){
    if(removing_active)(void)miniapp_ext_abort(&media_ticket);
    free(next_runtime);set_error(error,error_size,"deletion cancelled before catalog commit; original component preserved");return false;
  }
#endif
  if (!write_catalog_snapshot_at(next_catalog, next_count, next_active_id,
                                 next_count?next_catalog[next_selected].widget_id:NULL,false,
                                 base_generation,base_sequence,&commit)) {
#if PET_P4_CAPABILITY_EXT
    if(removing_active)(void)miniapp_ext_abort(&media_ticket);
#endif
    free(next_runtime);
    set_error(error, error_size, g_catalog_recovery_blocked
      ? "catalog deletion outcome unknown; inspect only, do not replay"
      : "component catalog deletion not committed");
    return false;
  }
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_DURABLE);
  (void)pet_storage_scope_lock(&storage);
  (void)save_app_runtime();
  if(removing_active&&next_runtime)merge_live_persistent_runtime(next_runtime);
#if PET_P4_CAPABILITY_EXT
  if(removing_active&&!miniapp_ext_commit(&media_ticket)){
    g_catalog_recovery_blocked=true;
    /* Resolve the physical job as quarantined, keeping the old runtime/files. */
    (void)pet_storage_scope_advance(&storage,PET_STORAGE_PUBLISHED);
    (void)pet_storage_scope_advance(&storage,PET_STORAGE_CLEANUP);
    (void)pet_storage_scope_unlock(&storage);
    (void)miniapp_ext_abort(&media_ticket);
    free(next_runtime);set_error(error,error_size,"durable deletion media publication failed; old runtime retained, recovery required");return false;
  }
#endif
  PET_WIDGET_TRACE_SCOPE(trace_publish, PET_WTRACE_PUBLISH);
  miniapp_runtime_lock();
  /* A user may browse the catalog while flash is busy. Keep that selection by
   * identity unless this transaction intentionally replaces the active item. */
  if(!removing_active&&g_catalog_selected<g_catalog_count){
    int selected=catalog_find(next_catalog,next_count,g_catalog[g_catalog_selected].widget_id);
    if(selected>=0)next_selected=(size_t)selected;
  }
  memcpy(g_catalog, next_catalog, sizeof(g_catalog));
  g_catalog_count = next_count;
  g_catalog_selected = next_selected;
  g_catalog_file_generation=commit.generation;g_catalog_sequence=commit.sequence;
  g_catalog_seed_pending=false;
  if (removing_active) {
#if PET_COMPONENT_PROGRAM_ENABLED
    component_program_retire_locked();
#endif
    if (next_runtime) {g_runtime = *next_runtime;g_image_revision++;}
    else memset(&g_runtime, 0, sizeof(g_runtime));
  }
  memset(&g_staging, 0, sizeof(g_staging));
  g_image_used=0;g_image_next=0;g_image_id[0]=0;
  miniapp_runtime_unlock();
  pet_widget_trace_scope_end(&trace_publish);
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_PUBLISHED);
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_CLEANUP);
  (void)pet_storage_scope_unlock(&storage);
  free(next_runtime);
#if PET_P4_CAPABILITY_EXT
  miniapp_ext_removed(widget_id, removed_slot);
#endif

  pet_file_cleanup_target_t targets[MINIAPP_DELETE_TARGETS];
  pet_file_cleanup_plan_t cleanup;
  uint32_t cleanup_sequence=g_catalog_sequence;
  component_cleanup_targets(removed_slot,targets);
  bool package_cleanup_ok=storage_work_current();
  const size_t first_count=MINIAPP_DELETE_TARGETS<PET_FILE_CLEANUP_MAX_TARGETS?MINIAPP_DELETE_TARGETS:PET_FILE_CLEANUP_MAX_TARGETS;
  if(package_cleanup_ok){
    (void)pet_file_cleanup_prepare(&cleanup,"/spiffs",targets,first_count,cleanup_guard(&cleanup_sequence));
    package_cleanup_ok=storage_work_current()&&pet_file_cleanup_remove_at(&cleanup,0);
  }
  if (!preserve_save&&!pet_app_store_forget_with_lease(widget_id,g_storage_resource_lease)){
    set_error(error,error_size,"component removed; private save clear intent could not be made durable; inspect before retry");
    return false;
  }
  for(size_t target=1;package_cleanup_ok&&target<first_count;target++)
    package_cleanup_ok=storage_work_current()&&pet_file_cleanup_remove_at(&cleanup,target);
  for(size_t begin=first_count;package_cleanup_ok&&begin<MINIAPP_DELETE_TARGETS;begin+=PET_FILE_CLEANUP_MAX_TARGETS){
    size_t count=MINIAPP_DELETE_TARGETS-begin;
    if(count>PET_FILE_CLEANUP_MAX_TARGETS)count=PET_FILE_CLEANUP_MAX_TARGETS;
    (void)pet_file_cleanup_prepare(&cleanup,"/spiffs",targets+begin,count,cleanup_guard(&cleanup_sequence));
    for(size_t target=0;package_cleanup_ok&&target<count;target++)
      package_cleanup_ok=storage_work_current()&&pet_file_cleanup_remove_at(&cleanup,target);
  }
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

/* Private parent fragment deliberately shares this file's verified loader,
 * storage lane and model lock. Never compiled as a separate owner. */
#include "pet_p4_miniapp_background.inc"
/* Track C capability-expansion hooks share the same loader, lane and model lock. */
#include "pet_p4_miniapp_ext.inc"
#include "pet_p4_miniapp_observation.inc"
