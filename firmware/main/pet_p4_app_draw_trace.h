/* Numeric wall observations only. No UI content, IO, allocation or policy. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifndef PET_P4_APP_NODE_TRACE
#define PET_P4_APP_NODE_TRACE 0
#endif
#if PET_P4_APP_NODE_TRACE && !PET_P4_APP_PHASE_TRACE
#error "App node trace requires the original whole-app phase trace"
#endif
/* One slot per surface node; the renderer asserts this equals PET_SURFACE_NODES
 * so the trace never silently drops the upper nodes. */
#if PET_P4_QUOTA_V3
#define PET_APP_TRACE_NODES 64
#else
#define PET_APP_TRACE_NODES 48
#endif
#if PET_P4_APP_NODE_TRACE
#include <stdio.h>
typedef struct {
  uint32_t images_us,key_us,cache_copy_us,prefix_us,clear_us,remember_us;
  uint32_t nodes_us[PET_APP_TRACE_NODES];
  uint64_t nodes_mask;
  uint32_t cache_hit,invalid_clock,cache_deferred;
} pet_app_draw_trace_t;
static inline bool pet_app_draw_trace_add(uint32_t *out,int64_t start,int64_t end){
  if(!out||start<0||end<start)return false;
  uint64_t value=(uint64_t)(end-start);
  *out=value>UINT32_MAX-*out?UINT32_MAX:*out+(uint32_t)value;return true;
}
/* Same complete node set as the trace formatter, saturating on long spans. */
static inline uint32_t pet_app_draw_trace_nodes_total(const pet_app_draw_trace_t *t){
  if(!t)return 0;
  uint32_t total=0;
  for(unsigned i=0;i<PET_APP_TRACE_NODES;i++){
    if(t->nodes_us[i]>UINT32_MAX-total)return UINT32_MAX;
    total+=t->nodes_us[i];
  }
  return total;
}
/* A truncated line is never emitted as complete evidence. Input is fixed
 * native numeric storage; no component strings are accepted by this helper. */
static inline bool pet_app_draw_trace_format(const pet_app_draw_trace_t*t,
    uint32_t frame,char*out,size_t size){
  if(!t||!out||!size)return false;
  int n=snprintf(out,size,"P4 app nodes frame=%u images_us=%u key_us=%u cache_copy_us=%u prefix_us=%u clear_us=%u remember_us=%u hit=%u mask=%llu invalid_clock=%u",
    (unsigned)frame,(unsigned)t->images_us,(unsigned)t->key_us,(unsigned)t->cache_copy_us,
    (unsigned)t->prefix_us,(unsigned)t->clear_us,(unsigned)t->remember_us,
    (unsigned)t->cache_hit,(unsigned long long)t->nodes_mask,(unsigned)t->invalid_clock);
  if(n<0||(size_t)n>=size)return false;
  size_t used=(size_t)n;
  for(unsigned i=0;i<PET_APP_TRACE_NODES;i++)if(t->nodes_mask&(UINT64_C(1)<<i)){
    n=snprintf(out+used,size-used," n%u_us=%u",i,(unsigned)t->nodes_us[i]);
    if(n<0||(size_t)n>=size-used)return false;
    used+=(size_t)n;
  }
  return true;
}
#endif
