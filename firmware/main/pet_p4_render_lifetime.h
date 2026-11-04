#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "pet_p4_app_draw_trace.h"
#include "pet_p4_render_policy.h"
#ifndef PET_P4_RETAIN_PEAK_PHASES
#define PET_P4_RETAIN_PEAK_PHASES 0
#endif
/* Diagnostic-only boot lifetime counters. Window resets and log throttling
 * never erase a stall. Numeric frame IDs correlate with existing phase logs;
 * neither an endpoint observation nor a peak establishes its cause. */
#if PET_P4_FRAME_IO_OVERLAP && (!PET_P4_RETAIN_PEAK_PHASES || !PET_P4_APP_PHASE_TRACE || !PET_P4_APP_NODE_TRACE)
#error Frame/IO overlap evidence requires the retained same-frame phase and node traces
#endif
#if PET_P4_FRAME_IO_OVERLAP
typedef struct {uint32_t max_us,frame,app_draw_us,rotate_us,lcd_us,clear_us,nodes_us,overlay_us;} pet_render_overlap_peak_t;
#endif
#if PET_P4_GAP_PEAK_CONTEXT
/* Wall-clock maxima observed by the main loop between the previous frame and
 * this one; lane phases are the widget operation observations at frame start
 * (bit 16 active, bits 8..15 kind, bits 0..7 stage). Correlation only. */
typedef struct {uint32_t gap_us,frame,wait_max_us,held_max_us,miniapp_max_us,held_total_us,iterations,lane0_phase,lane1_phase;
#if PET_P4_GAP_PEAK_HOLDER
  /* Sampled before the main loop's longest wait of this gap: holder class
   * ("" free, "protocol", "other"), protocol worker topic hash (0 otherwise),
   * and the peak frame's end clock for host-side correlation. */
  uint64_t at_us;uint32_t wait_topic_hash;char wait_holder[16];
#endif
} pet_render_gap_context_t;
#endif
typedef struct {
  uint32_t samples,max_us,max_gap_us,over_budget,gap_over_budget,invalid_clock;
  uint32_t peak_frame,peak_page,gap_peak_frame,window_resets,completed_windows,window_end_frame;
  uint64_t peak_start_us,peak_end_us;
#if PET_P4_RETAIN_PEAK_PHASES
  /* Same boot-lifetime winning frame, not independent stage maxima. */
  uint32_t asset_us,decode_us,pack_us,convert_us,overlay_us,rotate_us,lcd_us;
  uint32_t content_us,common_us,resolve_us,cache_us,scale_us;
  /* Content wall sub-stages are inclusive: select/blit/voice can be inside
   * bubble_draw or cache_draw. Never sum these as disjoint CPU durations. */
  uint32_t bubble_select_us,pinned_blit_us,bubble_draw_us,session_cache_fill_us,session_cache_draw_us,session_cache_scan_us,session_cache_copy_us,voice_us;
#if PET_P4_LOGICAL_COMPARE
  uint32_t logical_compare_us;
#endif
#if PET_P4_APP_PHASE_TRACE
  uint32_t app_snapshot_us,app_snapshot_wait_us,app_draw_us;
#endif
#if PET_P4_APP_NODE_TRACE
  pet_app_draw_trace_t app_nodes;
#endif
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  uint32_t compare_us,lcd_submit_us,lcd_wait_us;
  bool compare_valid,lcd_valid,lcd_submitted,lcd_waited;
#endif
#if PET_P4_APP_RETAIN_LOGICAL
  bool logical_retained;
#endif
#endif
#if PET_P4_FRAME_IO_OVERLAP
  /* Same-boot peaks split by whether a widget operation lane overlapped the
   * frame window. Correlation only; neither peak is CPU time or a cause. */
  uint32_t overlap_frames;
  bool peak_overlapped;
  pet_render_overlap_peak_t quiet_peak,overlap_peak;
  /* Quiet peak restricted to the app page, so a boot-time main-page frame
   * cannot mask the quiet cost of a cold app repaint. */
  pet_render_overlap_peak_t quiet_app_peak;
#endif
#if PET_P4_APP_CLEAR_OPAQUE_CENTER
  /* How often the full clear skipped the first opaque layer's center. */
  uint32_t clear_center_frames,clear_full_frames;
#endif
#if PET_P4_APP_COLD_FAST
  uint32_t cold_fast_image_nodes,cold_fast_panel_nodes,cold_fast_clear_skips;
#endif
#if PET_P4_APP_PPA_COPY
  uint32_t ppa_copies,ppa_copy_fallbacks,ppa_copy_mismatches;
#endif
#if PET_P4_H264_WORKER_PACK
  /* Presented frames adopted from a worker-packed buffer, taken frames that
   * still needed the render-thread pack, and the worker-side pack wall time. */
  uint32_t worker_packs,worker_pack_fallbacks,worker_pack_max_us,worker_pack_last_us;
#endif
#if PET_P4_GAP_PEAK_CONTEXT
  pet_render_gap_context_t gap_peak;
#endif
} pet_render_lifetime_t;
static inline void pet_render_counter_increment(uint32_t *value){
  if(*value!=UINT32_MAX)(*value)++;
}
static inline void pet_render_lifetime_record(pet_render_lifetime_t *out,
    uint32_t total,uint32_t gap,uint32_t frame,uint32_t page,int64_t start,int64_t end,
    uint32_t frame_budget,uint32_t gap_budget){
  pet_render_counter_increment(&out->samples);
  if(start<0||end<start)pet_render_counter_increment(&out->invalid_clock);
  if(total>frame_budget)pet_render_counter_increment(&out->over_budget);
  if(gap>gap_budget)pet_render_counter_increment(&out->gap_over_budget);
  if(total>out->max_us){
    out->max_us=total;out->peak_frame=frame;out->peak_page=page;
    out->peak_start_us=start>0?(uint64_t)start:0;
    out->peak_end_us=end>0?(uint64_t)end:0;
  }
  if(gap>out->max_gap_us){out->max_gap_us=gap;out->gap_peak_frame=frame;}
}
#if PET_P4_GAP_PEAK_CONTEXT
/* Called BEFORE pet_render_lifetime_record for the same frame: keeps the
 * context of the frame whose preceding gap becomes the boot-lifetime maximum. */
static inline void pet_render_gap_context_record(pet_render_lifetime_t *out,uint32_t gap_us,const pet_render_gap_context_t *context){
  if(gap_us>out->max_gap_us){out->gap_peak=*context;out->gap_peak.gap_us=gap_us;}
}
#endif
#if PET_P4_FRAME_IO_OVERLAP
/* Called BEFORE pet_render_lifetime_record for the same frame; new_peak says
 * whether this frame becomes the boot-lifetime peak. Wall durations only. */
static inline void pet_render_overlap_peak_update(pet_render_overlap_peak_t *peak,uint32_t total_us,uint32_t frame,
    uint32_t app_draw_us,uint32_t rotate_us,uint32_t lcd_us,uint32_t clear_us,uint32_t nodes_us,uint32_t overlay_us){
  if(total_us>peak->max_us||!peak->frame){
    *peak=(pet_render_overlap_peak_t){total_us,frame,app_draw_us,rotate_us,lcd_us,clear_us,nodes_us,overlay_us};
  }
}
static inline void pet_render_overlap_record(pet_render_lifetime_t *out,bool overlapped,bool app_page,uint32_t total_us,uint32_t frame,
    uint32_t app_draw_us,uint32_t rotate_us,uint32_t lcd_us,uint32_t clear_us,uint32_t nodes_us,uint32_t overlay_us,bool new_peak){
  if(overlapped)pet_render_counter_increment(&out->overlap_frames);
  if(new_peak)out->peak_overlapped=overlapped;
  pet_render_overlap_peak_update(overlapped?&out->overlap_peak:&out->quiet_peak,total_us,frame,app_draw_us,rotate_us,lcd_us,clear_us,nodes_us,overlay_us);
  if(!overlapped&&app_page)pet_render_overlap_peak_update(&out->quiet_app_peak,total_us,frame,app_draw_us,rotate_us,lcd_us,clear_us,nodes_us,overlay_us);
}
#endif
