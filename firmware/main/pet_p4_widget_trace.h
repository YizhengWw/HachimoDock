#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef PET_P4_SPARE_OPEN_TRACE
#define PET_P4_SPARE_OPEN_TRACE 0
#endif

/* Numeric wall-time diagnostics only. Nested stage totals overlap: do not add
 * them together or interpret them as CPU execution time. No payloads/paths. */
typedef enum {
  PET_WTRACE_NONE=0, PET_WTRACE_COMMIT=1, PET_WTRACE_DELETE=2,
  PET_WTRACE_ACTIVATE=3, PET_WTRACE_SAVE_FLUSH=4
} pet_widget_trace_kind_t;
typedef enum {
  PET_WTRACE_ALLOC=1, PET_WTRACE_READ_WIDGET, PET_WTRACE_READ_BUTTONS,
  PET_WTRACE_SPRITE_READ, PET_WTRACE_CHECKSUM, PET_WTRACE_PARSE,
  PET_WTRACE_NVS, PET_WTRACE_SAVE_RESTORE, PET_WTRACE_CATALOG_WRITE,
  PET_WTRACE_CLEANUP, PET_WTRACE_PUBLISH, PET_WTRACE_ACK,
  PET_WTRACE_FS_OPEN, PET_WTRACE_FS_DATA, PET_WTRACE_FS_CLOSE,
  PET_WTRACE_FS_REMOVE, PET_WTRACE_FS_RENAME, PET_WTRACE_SAVE_WRITE,
  PET_WTRACE_SAVE_READ, PET_WTRACE_FS_SCAN,
#if PET_P4_SPARE_OPEN_TRACE
  PET_WTRACE_SPARE_OPEN_UPDATE, PET_WTRACE_SPARE_OPEN_CREATE,
#endif
  PET_WTRACE_STAGE_COUNT
} pet_widget_trace_stage_t;
typedef struct { uint64_t seq; uint8_t lane; bool owned; } pet_widget_trace_operation_t;
typedef struct {
  uint64_t seq, start_us;
  uint8_t lane, stage, previous_stage, depth;
  bool active;
} pet_widget_trace_scope_t;
typedef struct {
  uint64_t seq, start_us, last_completed_seq, last_start_us, last_end_us;
  uint8_t lane, kind, current_stage, active;
} pet_widget_trace_lane_snapshot_t;
typedef struct { pet_widget_trace_lane_snapshot_t lanes[2]; } pet_widget_trace_snapshot_t;

#if defined(ESP_PLATFORM) || defined(PET_WIDGET_TRACE_TEST)
pet_widget_trace_operation_t pet_widget_trace_begin(pet_widget_trace_kind_t kind);
void pet_widget_trace_end(pet_widget_trace_operation_t *operation, bool ok);
void pet_widget_trace_operation_cleanup(pet_widget_trace_operation_t *operation);
pet_widget_trace_scope_t pet_widget_trace_scope_begin(pet_widget_trace_stage_t stage);
void pet_widget_trace_scope_end(pet_widget_trace_scope_t *scope);
void pet_widget_trace_snapshot(pet_widget_trace_snapshot_t *out);
#else
/* Existing portable algorithm harnesses do not initialize a scheduler. The
 * dedicated diagnostic tests enable PET_WIDGET_TRACE_TEST and the real module. */
static inline pet_widget_trace_operation_t pet_widget_trace_begin(pet_widget_trace_kind_t kind) {
  (void)kind; return (pet_widget_trace_operation_t){0};
}
static inline void pet_widget_trace_end(pet_widget_trace_operation_t *op,bool ok) {(void)op;(void)ok;}
static inline void pet_widget_trace_operation_cleanup(pet_widget_trace_operation_t *op) {(void)op;}
static inline pet_widget_trace_scope_t pet_widget_trace_scope_begin(pet_widget_trace_stage_t stage) {
  (void)stage; return (pet_widget_trace_scope_t){0};
}
static inline void pet_widget_trace_scope_end(pet_widget_trace_scope_t *scope) {(void)scope;}
static inline void pet_widget_trace_snapshot(pet_widget_trace_snapshot_t *out) {
  if(out)*out=(pet_widget_trace_snapshot_t){0};
}
#endif
#define PET_WIDGET_TRACE_SCOPE(name,stage) \
  pet_widget_trace_scope_t name __attribute__((cleanup(pet_widget_trace_scope_end))) = pet_widget_trace_scope_begin(stage)
#define PET_WIDGET_TRACE_OPERATION(name,kind) \
  pet_widget_trace_operation_t name __attribute__((cleanup(pet_widget_trace_operation_cleanup))) = pet_widget_trace_begin(kind)
