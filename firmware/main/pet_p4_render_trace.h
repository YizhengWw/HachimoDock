/*
 * [Input] One renderer frame's wall-clock measurements, in microseconds.
 * [Output] Bounded numeric diagnostics and a same-frame window-peak snapshot.
 * [Pos] Renderer-only diagnostics; no allocation, I/O, device state or policy.
 * [Sync] Keep renderer instrumentation and p4_render_trace_test.py in agreement.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_widget_trace.h"
#include "pet_p4_ppa_trace.h"
#ifndef PET_P4_APP_PHASE_TRACE
#define PET_P4_APP_PHASE_TRACE 0
#endif
#include "pet_p4_app_draw_trace.h"
#include "pet_p4_render_policy.h"

enum {
  PET_RENDER_PAGE_OTHER = 0,
  PET_RENDER_PAGE_MAIN = 1,
  PET_RENDER_PAGE_APP = 2,
  PET_RENDER_PAGE_COMPONENTS = 3,
  PET_RENDER_PAGE_TOUCH_TEST = 4,
};
enum {
  PET_RENDER_CACHE_UNCHECKED = 0,
  PET_RENDER_CACHE_MISS = 1,
  PET_RENDER_CACHE_HIT = 2,
};
/* Bit mask; cold can accompany revision, and path can accompany rewind. */
enum {
  PET_RENDER_RESTART_COLD = 1U,
  PET_RENDER_RESTART_PATH = 2U,
  PET_RENDER_RESTART_REWIND = 4U,
  PET_RENDER_RESTART_REVISION = 8U,
  PET_RENDER_RESTART_CODEC = 16U,
};
/* Selected/attempted decode backend, not proof of a successfully drawn frame. */
enum {
  PET_RENDER_BACKEND_NONE = 0,
  PET_RENDER_BACKEND_H264_SINGLE = 1,
  PET_RENDER_BACKEND_H264_DUAL = 2,
  PET_RENDER_BACKEND_JPEG_HARDWARE = 3,
  PET_RENDER_BACKEND_JPEG_TJPGD = 4,
};
/* Bit mask of fallback paths actually entered (even if decoding then fails). */
enum {
  PET_RENDER_FALLBACK_H264_CPU = 1U,
  PET_RENDER_FALLBACK_JPEG_TJPGD = 2U,
  PET_RENDER_FALLBACK_JPEG_SCALE_CPU = 4U,
};

/*
 * Every duration is wall time, including scheduler waits and existing logs.
 * resolve: selection + filesystem-path resolution.
 * cache: cache invalidation + lookup/allocation/read; cacheHit describes lookup.
 * validate: H264 access-unit validation (only when not already validated).
 * reset: H264 close/delete + decoder-state clear, including failure cleanup.
 * copy: H264 work-buffer realloc/copy, or JPEG compressed-frame copy.
 * init: H264 new/open, excluding failure cleanup (which belongs to reset).
 * decode_outer: H264 decode-next loop, or the existing JPEG decode interval.
 * scale: H264 layout/pack/conversion, or JPEG scaling/copy to display.
 * Nested timings MUST NOT be summed: legacy decode is inside decode_outer;
 * legacy pack/convert are inside scale; all these asset stages are inside asset.
 * Renderer init and external reset_playback are outside existing frame timing.
 * Unvisited stages are zero. frame wraps modulo 2^32; all counters are numeric.
 */
typedef struct {
  uint64_t seq;
  /* Numeric observation, not a resource lock or a cause of a long frame:
   * active in bit 16, operation kind in bits 8..15, stage in bits 0..7. */
  uint32_t phase;
} pet_p4_render_operation_observation_t;

typedef struct {
  uint32_t frame, page;
  uint64_t start_us, end_us, previous_end_us;
  uint32_t gap_us;
  uint32_t content_us, common_overlay_us;
  /* Content wall sub-stages are inclusive: select/blit/voice can be inside
   * bubble_draw or cache_draw. Never sum these as disjoint CPU durations. */
  uint32_t bubble_select_us,pinned_blit_us,bubble_draw_us,session_cache_fill_us,session_cache_draw_us,session_cache_scan_us,session_cache_copy_us,voice_us;
#if PET_P4_LOGICAL_COMPARE
  uint32_t logical_compare_us;
#endif
#if PET_P4_APP_PHASE_TRACE
  uint64_t app_draw_started_us;
  uint32_t app_snapshot_us,app_snapshot_wait_us,app_draw_us;
#endif
#if PET_P4_APP_NODE_TRACE
  pet_app_draw_trace_t app_nodes;
#endif
#if PET_P4_APP_RETAIN_LOGICAL
  bool logical_retained;
#endif
  pet_p4_render_operation_observation_t operation_before[2], operation_after[2];
  uint64_t completed_after[2];
#if PET_P4_FRAME_IO_OVERLAP
  uint64_t completed_before[2];
#endif
  pet_ppa_trace_frame_t ppa;
  uint32_t resolve_us, cache_us, validate_us, reset_us, copy_us, init_us;
  uint32_t decode_outer_us, scale_us;
  uint32_t cache_hit, restart_reason, stream_size, backend, fallback;
} pet_p4_render_trace_t;

static inline void pet_p4_render_trace_observe(
  pet_p4_render_trace_t *trace, const pet_widget_trace_snapshot_t *snapshot, bool after
) {
  for (unsigned i = 0; i < 2; ++i) {
    pet_p4_render_operation_observation_t *out = after
      ? &trace->operation_after[i] : &trace->operation_before[i];
    const pet_widget_trace_lane_snapshot_t *lane = &snapshot->lanes[i];
    out->seq = lane->seq;
    out->phase = ((uint32_t)(lane->active != 0) << 16)
      | ((uint32_t)lane->kind << 8) | lane->current_stage;
    if (after) trace->completed_after[i] = lane->last_completed_seq;
#if PET_P4_FRAME_IO_OVERLAP
    else trace->completed_before[i] = lane->last_completed_seq;
#endif
  }
}

typedef struct {
  pet_p4_render_trace_t trace;
  uint32_t total_us, asset_us, decode_us, pack_us, convert_us;
  uint32_t overlay_us, rotate_us, lcd_us;
  bool valid;
} pet_p4_render_peak_t;

static inline void pet_p4_render_trace_begin(
  pet_p4_render_trace_t *trace, uint32_t frame, const char *page
) {
  memset(trace, 0, sizeof(*trace));
  trace->frame = frame;
  if (!page || strcmp(page, "main") == 0) trace->page = PET_RENDER_PAGE_MAIN;
  else if (strcmp(page, "app") == 0) trace->page = PET_RENDER_PAGE_APP;
  else if (strcmp(page, "components") == 0) trace->page = PET_RENDER_PAGE_COMPONENTS;
  else if (strcmp(page, "touch_test") == 0) trace->page = PET_RENDER_PAGE_TOUCH_TEST;
}

static inline void pet_p4_render_trace_add(uint32_t *field, uint32_t elapsed) {
  *field = elapsed > UINT32_MAX - *field ? UINT32_MAX : *field + elapsed;
}

static inline uint32_t pet_p4_render_restart_reason(
  bool has_decoder, bool same_path, bool rewind
) {
  if (!has_decoder) return PET_RENDER_RESTART_COLD;
  return (same_path ? 0U : PET_RENDER_RESTART_PATH)
    | (rewind ? PET_RENDER_RESTART_REWIND : 0U);
}

static inline void pet_p4_render_peak_record(
  pet_p4_render_peak_t *peak, const pet_p4_render_trace_t *trace,
  uint32_t total_us, uint32_t asset_us, uint32_t decode_us, uint32_t pack_us,
  uint32_t convert_us, uint32_t overlay_us, uint32_t rotate_us, uint32_t lcd_us
) {
  if (peak->valid && total_us <= peak->total_us) return;
  peak->trace = *trace;
  peak->total_us = total_us;
  peak->asset_us = asset_us;
  peak->decode_us = decode_us;
  peak->pack_us = pack_us;
  peak->convert_us = convert_us;
  peak->overlay_us = overlay_us;
  peak->rotate_us = rotate_us;
  peak->lcd_us = lcd_us;
  peak->valid = true;
}

/* Casts keep printf types portable across uint32_t implementations. */
#define PET_RENDER_TRACE_FORMAT \
  " frame=%u page=%u resolve_us=%u cache_us=%u validate_us=%u reset_us=%u" \
  " copy_us=%u init_us=%u decode_outer_us=%u scale_us=%u cacheHit=%u" \
  " restartReason=%u streamSize=%u backend=%u fallback=%u"
#define PET_RENDER_TRACE_VALUES(t) \
  (unsigned int) (t).frame, (unsigned int) (t).page, \
  (unsigned int) (t).resolve_us, (unsigned int) (t).cache_us, \
  (unsigned int) (t).validate_us, (unsigned int) (t).reset_us, \
  (unsigned int) (t).copy_us, (unsigned int) (t).init_us, \
  (unsigned int) (t).decode_outer_us, (unsigned int) (t).scale_us, \
  (unsigned int) (t).cache_hit, (unsigned int) (t).restart_reason, \
  (unsigned int) (t).stream_size, (unsigned int) (t).backend, \
  (unsigned int) (t).fallback

/* One separate bounded line preserves the old timing format and lets a reader
 * intersect real operation start/end records with this exact frame interval.
 * End-point observations alone never establish causality. */
#define PET_RENDER_CONTEXT_FORMAT \
  "P4 frame context frame=%u start_us=%llu end_us=%llu prev_end_us=%llu gap_us=%u content_us=%u common_us=%u" \
  " wb_seq=%llu wb_phase=%u wa_seq=%llu wa_phase=%u w_done=%llu" \
  " sb_seq=%llu sb_phase=%u sa_seq=%llu sa_phase=%u s_done=%llu" PET_RENDER_APP_PHASE_FORMAT
#if PET_P4_APP_PHASE_TRACE
#define PET_RENDER_APP_PHASE_FORMAT " app_snapshot_us=%u app_snapshot_wait_us=%u app_draw_us=%u"
#define PET_RENDER_APP_PHASE_VALUES(t) ,(unsigned)(t).app_snapshot_us,(unsigned)(t).app_snapshot_wait_us,(unsigned)(t).app_draw_us
#else
#define PET_RENDER_APP_PHASE_FORMAT ""
#define PET_RENDER_APP_PHASE_VALUES(t)
#endif
#define PET_RENDER_CONTEXT_VALUES(t) \
  (unsigned)(t).frame, (unsigned long long)(t).start_us, (unsigned long long)(t).end_us, \
  (unsigned long long)(t).previous_end_us, (unsigned)(t).gap_us, \
  (unsigned)(t).content_us, (unsigned)(t).common_overlay_us, \
  (unsigned long long)(t).operation_before[0].seq, (unsigned)(t).operation_before[0].phase, \
  (unsigned long long)(t).operation_after[0].seq, (unsigned)(t).operation_after[0].phase, \
  (unsigned long long)(t).completed_after[0], \
  (unsigned long long)(t).operation_before[1].seq, (unsigned)(t).operation_before[1].phase, \
  (unsigned long long)(t).operation_after[1].seq, (unsigned)(t).operation_after[1].phase, \
  (unsigned long long)(t).completed_after[1] PET_RENDER_APP_PHASE_VALUES(t)

#define PET_RENDER_PPA_FORMAT \
  "P4 frame ppa frame=%u scale_api=%u scale_cb=%u scale_return=%u scale_flags=%u" \
  " convert_api=%u convert_cb=%u convert_return=%u convert_flags=%u" \
  " rotate_api=%u rotate_cb=%u rotate_return=%u rotate_flags=%u" PET_RENDER_PPA_QUEUE_FORMAT
#if PET_P4_PPA_QUEUE_PROBE
#define PET_RENDER_PPA_QUEUE_FORMAT \
  " scale_prepare=%u scale_queue_cb=%u convert_prepare=%u convert_queue_cb=%u rotate_prepare=%u rotate_queue_cb=%u"
#define PET_RENDER_PPA_QUEUE_VALUES(t) \
  ,(unsigned)(t).ppa.op[0].prepare_us,(unsigned)(t).ppa.op[0].queue_to_callback_us \
  ,(unsigned)(t).ppa.op[1].prepare_us,(unsigned)(t).ppa.op[1].queue_to_callback_us \
  ,(unsigned)(t).ppa.op[2].prepare_us,(unsigned)(t).ppa.op[2].queue_to_callback_us
#else
#define PET_RENDER_PPA_QUEUE_FORMAT ""
#define PET_RENDER_PPA_QUEUE_VALUES(t)
#endif
#define PET_RENDER_PPA_VALUES(t) \
  (unsigned)(t).frame, \
  (unsigned)(t).ppa.op[0].api_us, (unsigned)(t).ppa.op[0].call_to_callback_us, \
  (unsigned)(t).ppa.op[0].callback_to_return_us, (unsigned)(t).ppa.op[0].flags, \
  (unsigned)(t).ppa.op[1].api_us, (unsigned)(t).ppa.op[1].call_to_callback_us, \
  (unsigned)(t).ppa.op[1].callback_to_return_us, (unsigned)(t).ppa.op[1].flags, \
  (unsigned)(t).ppa.op[2].api_us, (unsigned)(t).ppa.op[2].call_to_callback_us, \
  (unsigned)(t).ppa.op[2].callback_to_return_us, (unsigned)(t).ppa.op[2].flags PET_RENDER_PPA_QUEUE_VALUES(t)
