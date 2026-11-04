#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "pet_p4_render_policy.h"
/* Protocol worker locked spans: state mutex held outside the unlocked storage
 * IO windows. Pure arithmetic here; the worker owns the single instance and the
 * diagnostics reader runs on that same worker under the state mutex. */
typedef struct {uint32_t max_us,max_topic_hash,spans,over_budget_spans;uint64_t max_at_us;} pet_worker_locked_peak_t;
#define PET_WORKER_LOCKED_BUDGET_US 20000U
static inline uint32_t pet_topic_hash(const char *topic,size_t len){
  uint32_t h=2166136261u;
  for(size_t i=0;i<len;i++){h^=(uint8_t)topic[i];h*=16777619u;}
  return h;
}
/* FNV-1a of the "topic" string of a compact protocol line ({"topic":"..."}).
 * 0 when the key is absent, unterminated or empty; never parses further. */
static inline uint32_t pet_line_topic_hash(const char *line){
  if(!line)return 0;
  const char *key=strstr(line,"\"topic\":\"");
  if(!key)return 0;
  const char *start=key+9;const char *end=strchr(start,'"');
  if(!end||end==start)return 0;
  return pet_topic_hash(start,(size_t)(end-start));
}
static inline void pet_worker_locked_record(pet_worker_locked_peak_t *peak,uint32_t held_us,uint32_t topic_hash,uint64_t at_us){
  peak->spans++;
  if(held_us>PET_WORKER_LOCKED_BUDGET_US)peak->over_budget_spans++;
  if(held_us>peak->max_us){peak->max_us=held_us;peak->max_topic_hash=topic_hash;peak->max_at_us=at_us;}
}
#if PET_P4_GAP_PEAK_HOLDER
const pet_worker_locked_peak_t *pet_p4_worker_locked_peak(void);
#endif
