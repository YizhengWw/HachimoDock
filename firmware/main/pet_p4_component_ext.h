/*
 * [Input] runtime/widget.json sections sequence/samples/events/alarms, surface big_images and
 *         clip nodes, transition/tick ops, install file paths and compiled media headers.
 * [Output] One bounded pet_cext_runtime_t per parsed component, pending audio/event bits,
 *          clip frame scheduling and big-image visible-tile selection.
 * [Pos] Track C capability-expansion parser/state core (p4-sequence-v1, p4-samples-v1,
 *       p4-clip-v1, p4-big-image-v1, p4-events-v1). Pure C + cJSON; no tasks, files or
 *       allocation. The miniapp hooks own the lock; the media store owns bytes.
 * [Sync] If this file changes, update firmware/protocol.md and the petui references.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "pet_p4_sfx_core.h"
#include "pet_p4_component_events.h"
#define PET_CEXT_ID_MAX 16u
#define PET_CEXT_SEQUENCE_ENGINE "p4-sequence-v1"
#define PET_CEXT_SAMPLES_ENGINE "p4-samples-v1"
#define PET_CEXT_CLIP_ENGINE "p4-clip-v1"
#define PET_CEXT_BIG_ENGINE "p4-big-image-v1"
#define PET_CEXT_EVENTS_ENGINE "p4-events-v1"
#define PET_CEXT_CLIP_MAX 1u
#define PET_CEXT_CLIP_MAX_BYTES (512u * 1024u)
#define PET_CEXT_CLIP_FPS 15u
#define PET_CEXT_CLIP_MAX_FRAMES 75u /* 5 s at 15 fps */
#define PET_CEXT_BIG_MAX 2u
#define PET_CEXT_BIG_TILE 240u
#define PET_CEXT_BIG_MAX_TILES 16u
#define PET_CEXT_BIG_MAX_W 1280u
#define PET_CEXT_BIG_MAX_H 960u
#define PET_CEXT_BIG_BUDGET_BYTES (1024u * 1024u)
#define PET_CEXT_BIG_TILE_MAX_BYTES (64u * 1024u)
#define PET_CEXT_SAMPLE_HEADER 8u  /* "P4A1" + u32 frames */
#define PET_CEXT_CLIP_HEADER 16u   /* "P4V1" w u16 h u16 fps u8 pad u8 frames u16 bytes u32 */
#define PET_CEXT_BIG_HEADER 20    /* "P4T1" cols u8 rows u8 tile_w u16 tile_h u16 w u16 h u16 count u8 pad u8 reserved u32; then count x {offset u32, bytes u32} */
typedef enum { PET_CEXT_MEDIA_NONE = 0, PET_CEXT_MEDIA_SAMPLE, PET_CEXT_MEDIA_CLIP, PET_CEXT_MEDIA_BIG } pet_cext_media_kind_t;
typedef struct { char id[PET_CEXT_ID_MAX]; uint32_t frames; uint32_t bytes; } pet_cext_sample_t;
typedef struct { char id[PET_CEXT_ID_MAX]; uint16_t w, h, frames; uint8_t fps; uint32_t bytes; } pet_cext_clip_t;
typedef struct { char id[PET_CEXT_ID_MAX]; uint16_t w, h, tile_w, tile_h; uint8_t cols, rows; uint32_t bytes; } pet_cext_big_t;
typedef struct {
  uint32_t media_generation; /* bound by publication, copied with the runtime/view snapshot */
  bool sequence, samples, events, clip, big;
  pet_seq_program_t program;
  uint8_t sequence_volume;
  bool sequence_playing;   /* authored state; the mixer follows it while audible */
  uint8_t sample_count, sample_pending; /* bitmask */
  pet_cext_sample_t sample[PET_SAMPLE_MAX_SLOTS];
  uint8_t samples_volume;
  uint8_t emit_count, emit_pending, sub_count;
  char emit_names[PET_CEV_NAMES_PER_PACKAGE][PET_CEV_NAME_MAX];
  char sub_names[PET_CEV_NAMES_PER_PACKAGE][PET_CEV_NAME_MAX];
  pet_cev_alarm_set_t alarms;
  uint8_t clip_count;
  pet_cext_clip_t clips[PET_CEXT_CLIP_MAX];
  uint8_t big_count;
  pet_cext_big_t bigs[PET_CEXT_BIG_MAX];
} pet_cext_runtime_t;
typedef struct {
  bool runtime_v4, exclusive_audio_owner, persist, surface_app;
  void *ctx;
  int (*find_int_var)(void *ctx, const char *name);      /* -1 when unknown/not int */
  bool (*has_action)(void *ctx, const char *action);     /* transition on == action */
} pet_cext_parse_env_t;
/* Parses the optional sections; sections whose compile flag is off must not be
 * present (the caller filters the allowed-key list). Fills out (zeroed first). */
bool pet_cext_parse(const cJSON *root, pet_cext_runtime_t *out, const pet_cext_parse_env_t *env, char *error, size_t error_size);
/* Surface-level big_images[] and clip nodes are parsed by pet_surface; this records
 * the declared assets so the media store can stage/load them. */
bool pet_cext_parse_surface_media(const cJSON *surface, pet_cext_runtime_t *out, char *error, size_t error_size);
/* Subscriptions "event:<name>" come from transition on fields. */
bool pet_cext_add_subscription(pet_cext_runtime_t *out, const char *on);
typedef struct { int8_t sample, emit; uint8_t sequence; /* 0 none, 1 play, 2 stop */ } pet_cext_ops_t;
#define PET_CEXT_OP_KEYS "sequence", "sample", "emit"
#define PET_CEXT_OP_KEY_COUNT 3
bool pet_cext_parse_ops(const pet_cext_runtime_t *r, const cJSON *object, pet_cext_ops_t *out, char *error, size_t error_size);
void pet_cext_fire(pet_cext_runtime_t *r, const pet_cext_ops_t *ops);
/* Audio snapshot for the speaker worker: play/stop command and pending samples. */
typedef struct { uint32_t generation; bool enabled, sequence_active; uint8_t sequence_volume, sample_pending, samples_volume; } pet_cext_audio_t;
void pet_cext_audio_take(pet_cext_runtime_t *r, bool audible, pet_cext_audio_t *out);
/* ---- media containers ---- */
pet_cext_media_kind_t pet_cext_media_path_kind(const char *path, char id[PET_CEXT_ID_MAX]);
bool pet_cext_sample_header(const uint8_t *data, size_t size, pet_cext_sample_t *out);
bool pet_cext_clip_header(const uint8_t *data, size_t size, pet_cext_clip_t *out);
/* Exact bounded P4I1 edge-tile validation; no decoding allocation. */
bool pet_cext_big_image_valid(const uint8_t *data,size_t bytes,unsigned width,unsigned height);
bool pet_cext_big_header(const uint8_t *data, size_t size, pet_cext_big_t *out);
/* Tile table entry (offset from file start, bytes) for tile index; false when out of range. */
bool pet_cext_big_tile(const uint8_t *data, size_t size, unsigned index, uint32_t *offset, uint32_t *bytes);
/* Tiles intersecting the node window at the given offsets (clamped). Returns count. */
unsigned pet_cext_big_visible_tiles(const pet_cext_big_t *big, int32_t offset_x, int32_t offset_y, unsigned node_w, unsigned node_h, uint8_t out[PET_CEXT_BIG_MAX_TILES]);
/* ---- clip scheduling (render thread) ---- */
typedef struct { bool visible; uint64_t started_ms, hidden_ms; int16_t presented; } pet_cext_clip_state_t;
/* Returns the frame to show now (loops), or -1 while hidden. Hiding pauses; showing resumes. */
int pet_cext_clip_target(pet_cext_clip_state_t *s, const pet_cext_clip_t *clip, bool visible, uint64_t now_ms);

/* Only parsed subscriber names / exact declared alarm targets gain exemption.
 * Caller must have parsed all subscriptions before checking binding coverage. */
bool pet_cext_internal_action(const pet_cext_runtime_t *r, const char *action);
bool pet_cext_binding_coverage(const pet_cext_runtime_t *r, const cJSON *root, const cJSON *buttons, char *error, size_t error_size);
