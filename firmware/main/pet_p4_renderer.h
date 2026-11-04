/*
 * [Input] Runtime/view state and bulk-transfer activity supplied by main.
 * [Output] Normal pet frames plus asset-independent transfer-status frames.
 * [Pos] Public renderer contract for the ESP32-P4 runtime.
 * [Sync] If this file changes, update `firmware/.folder.md` and tests.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pet_p4_protocol.h"
#include "pet_p4_view.h"
#include "pet_p4_render_lifetime.h"
#ifndef PET_P4_H264_PREFETCH
#define PET_P4_H264_PREFETCH 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pet_p4_renderer_init(void);
typedef struct {
  uint32_t last_us, p95_us, max_us, last_decode_us;
  uint32_t window_samples, submitted_frames, unchanged_frames;
  pet_render_lifetime_t lifetime;
#if PET_P4_H264_PREFETCH
  struct {uint32_t jobs,completed,cancelled,decoded_frames,presented_frames;
    uint32_t last_worker_us,max_worker_us,last_decode_us,max_decode_us;
#if PET_P4_H264_WORKER_PACK
    uint32_t last_offer_us,max_offer_us;
#endif
    uint32_t max_ready_latency_us,min_worker_free_bytes;} prefetch;
#endif
} pet_p4_render_metrics_t;
/* Thread-safe bounded snapshot; p95/max represent the last complete window.
 * Lifetime fields include excluded >2s samples and separately count gaps. */
pet_p4_render_metrics_t pet_p4_renderer_metrics(void);
/* Private native scheduling observation; no guest grant or time subtraction.
 * NEW program slices defer while the secondary codec physically owns a job. */
bool pet_p4_renderer_program_window_begin(void);
void pet_p4_renderer_program_window_end(void);
esp_err_t pet_p4_renderer_render(
  const pet_p4_runtime_state_t *state,
  const pet_p4_view_model_t *view,
  unsigned long long now_ms
);
esp_err_t pet_p4_renderer_render_transfer_status(
  bool firmware_update,
  unsigned long long now_ms
);
void pet_p4_renderer_reset_playback(void);
/* Thread-safe cancellation before storage mutation. Revokes pending preparation
 * only; does not wait, free buffers, or touch decoder/render state. */
void pet_p4_renderer_cancel_asset_preparation(void);
/* Trusted storage lane sets true BEFORE mutation, false only when storage is
 * stable. Persistent admission gate, unlike one-shot cancellation. Thread-safe;
 * false does not revive cancelled jobs. Caller coordinates transfer lifetime. */
void pet_p4_renderer_set_asset_preparation_suspended(bool suspended);
#include "pet_p4_render_policy.h"
#if PET_P4_GAP_PEAK_CONTEXT
/* Main task only: wall maxima of one loop iteration between two frames. */
void pet_p4_renderer_note_loop(uint32_t wait_us,uint32_t held_us,uint32_t miniapp_us);
#else
static inline void pet_p4_renderer_note_loop(uint32_t wait_us,uint32_t held_us,uint32_t miniapp_us){(void)wait_us;(void)held_us;(void)miniapp_us;}
#endif
#if PET_P4_GAP_PEAK_HOLDER
/* Main task only, before pet_p4_renderer_note_loop of the same iteration. */
void pet_p4_renderer_note_wait_holder(uint32_t wait_us,const char *holder,uint32_t topic_hash);
#else
static inline void pet_p4_renderer_note_wait_holder(uint32_t wait_us,const char *holder,uint32_t topic_hash){(void)wait_us;(void)holder;(void)topic_hash;}
#endif
const uint16_t *pet_p4_renderer_logical_framebuffer(int *width, int *height);
#if PET_P4_PARTICLES
/* Any task: accepted transition plus its exact native component revision. */
void pet_p4_renderer_note_action(const char *action,uint32_t revision);
#else
static inline void pet_p4_renderer_note_action(const char *action,uint32_t revision){(void)action;(void)revision;}
#endif
#if PET_P4_KEYFRAME_BG
/* True once the worker decode buffers exist; hello advertises keyframes only then. */
bool pet_p4_renderer_keyframe_ready(void);
#endif

#ifdef __cplusplus
}
#endif
