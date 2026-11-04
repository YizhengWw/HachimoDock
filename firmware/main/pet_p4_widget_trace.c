#include "pet_p4_widget_trace.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <limits.h>
#include <errno.h>
#include <string.h>

#define PET_WTRACE_DEPTH_MAX 8U
typedef struct { uint64_t sum_us, max_us; uint32_t count; } trace_stage_t;
typedef struct {
  TaskHandle_t task;
  uint64_t seq, start_us, end_us;
  uint64_t last_seq, last_start_us, last_end_us;
  uint32_t bad_scopes, rejected;
  uint8_t kind, stage, depth;
  bool active, ok;
  trace_stage_t stages[PET_WTRACE_STAGE_COUNT];
} trace_lane_t;
static portMUX_TYPE trace_mux = portMUX_INITIALIZER_UNLOCKED;
static trace_lane_t trace_lanes[2];
static uint64_t trace_sequence;
static void restore_trace_errno(int *saved) { errno=*saved; }
#define PRESERVE_TRACE_ERRNO int trace_errno __attribute__((cleanup(restore_trace_errno))) = errno

pet_widget_trace_operation_t pet_widget_trace_begin(pet_widget_trace_kind_t kind) {
  PRESERVE_TRACE_ERRNO;
  pet_widget_trace_operation_t token = {0};
  if (kind < PET_WTRACE_COMMIT || kind > PET_WTRACE_SAVE_FLUSH) return token;
  unsigned lane = kind == PET_WTRACE_SAVE_FLUSH ? 1U : 0U;
  TaskHandle_t task = xTaskGetCurrentTaskHandle();
  uint64_t now = (uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&trace_mux);
  trace_lane_t *state = &trace_lanes[lane];
  if (state->active || trace_sequence == UINT64_MAX) {
    if (state->rejected != UINT32_MAX) state->rejected++;
  } else {
    uint64_t last_seq=state->last_seq, last_start=state->last_start_us, last_end=state->last_end_us;
    memset(state,0,sizeof(*state));
    state->last_seq=last_seq;state->last_start_us=last_start;state->last_end_us=last_end;
    state->task=task;state->seq=++trace_sequence;state->start_us=now;
    state->kind=(uint8_t)kind;state->active=true;
    token=(pet_widget_trace_operation_t){state->seq,(uint8_t)lane,true};
  }
  portEXIT_CRITICAL(&trace_mux);
  return token;
}

pet_widget_trace_scope_t pet_widget_trace_scope_begin(pet_widget_trace_stage_t stage) {
  PRESERVE_TRACE_ERRNO;
  pet_widget_trace_scope_t token={0};
  if(stage<=0||stage>=PET_WTRACE_STAGE_COUNT)return token;
  TaskHandle_t task=xTaskGetCurrentTaskHandle();
  uint64_t now=(uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&trace_mux);
  int selected=-1;
  for(unsigned lane=0;lane<2;lane++){
    const trace_lane_t *state=&trace_lanes[lane];
    if(state->active&&state->task==task&&(selected<0||state->seq>trace_lanes[selected].seq))selected=(int)lane;
  }
  if(selected>=0){
    trace_lane_t *state=&trace_lanes[selected];
    if(state->depth==PET_WTRACE_DEPTH_MAX){if(state->bad_scopes!=UINT32_MAX)state->bad_scopes++;}
    else {
    token=(pet_widget_trace_scope_t){state->seq,now,(uint8_t)selected,(uint8_t)stage,state->stage,(uint8_t)(state->depth+1),true};
    state->stage=(uint8_t)stage;state->depth++;
    }
  }
  portEXIT_CRITICAL(&trace_mux);
  return token;
}

void pet_widget_trace_scope_end(pet_widget_trace_scope_t *token) {
  PRESERVE_TRACE_ERRNO;
  if(!token||!token->active)return;
  token->active=false;
  if(token->lane>=2||token->stage<=0||token->stage>=PET_WTRACE_STAGE_COUNT)return;
  uint64_t now=(uint64_t)esp_timer_get_time();
  TaskHandle_t task=xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&trace_mux);
  trace_lane_t *state=&trace_lanes[token->lane];
  if(state->active&&state->seq==token->seq&&state->task==task){
    if(state->depth!=token->depth||state->stage!=token->stage){
      if(state->bad_scopes!=UINT32_MAX)state->bad_scopes++;
    }else{
      uint64_t elapsed=now>=token->start_us?now-token->start_us:0;
      trace_stage_t *metric=&state->stages[token->stage];
      if(metric->count!=UINT32_MAX)metric->count++;
      metric->sum_us=UINT64_MAX-metric->sum_us<elapsed?UINT64_MAX:metric->sum_us+elapsed;
      if(elapsed>metric->max_us)metric->max_us=elapsed;
      state->stage=token->previous_stage;state->depth--;
    }
  }
  portEXIT_CRITICAL(&trace_mux);
}

/* Fixed two-line numeric report per completed operation, outside all trace
 * critical sections. vN=count,sum_wall_us,max_wall_us; nested vN overlap. */
static void emit_trace(unsigned lane,const trace_lane_t *done) {
  ESP_LOGI("pet-widget-trace", "P4 widget trace part=1 lane=%u seq=%llu kind=%u start_us=%llu end_us=%llu ok=%u bad=%u rejected=%u v1=%u,%llu,%llu v2=%u,%llu,%llu v3=%u,%llu,%llu v4=%u,%llu,%llu v5=%u,%llu,%llu v6=%u,%llu,%llu v7=%u,%llu,%llu v8=%u,%llu,%llu v9=%u,%llu,%llu v10=%u,%llu,%llu",
    lane,(unsigned long long)done->seq,(unsigned)done->kind,
    (unsigned long long)done->start_us,(unsigned long long)done->end_us,
    done->ok?1U:0U,(unsigned)done->bad_scopes,(unsigned)done->rejected,
    (unsigned)done->stages[1].count,(unsigned long long)done->stages[1].sum_us,(unsigned long long)done->stages[1].max_us,
    (unsigned)done->stages[2].count,(unsigned long long)done->stages[2].sum_us,(unsigned long long)done->stages[2].max_us,
    (unsigned)done->stages[3].count,(unsigned long long)done->stages[3].sum_us,(unsigned long long)done->stages[3].max_us,
    (unsigned)done->stages[4].count,(unsigned long long)done->stages[4].sum_us,(unsigned long long)done->stages[4].max_us,
    (unsigned)done->stages[5].count,(unsigned long long)done->stages[5].sum_us,(unsigned long long)done->stages[5].max_us,
    (unsigned)done->stages[6].count,(unsigned long long)done->stages[6].sum_us,(unsigned long long)done->stages[6].max_us,
    (unsigned)done->stages[7].count,(unsigned long long)done->stages[7].sum_us,(unsigned long long)done->stages[7].max_us,
    (unsigned)done->stages[8].count,(unsigned long long)done->stages[8].sum_us,(unsigned long long)done->stages[8].max_us,
    (unsigned)done->stages[9].count,(unsigned long long)done->stages[9].sum_us,(unsigned long long)done->stages[9].max_us,
    (unsigned)done->stages[10].count,(unsigned long long)done->stages[10].sum_us,(unsigned long long)done->stages[10].max_us);
  ESP_LOGI("pet-widget-trace", "P4 widget trace part=2 lane=%u seq=%llu kind=%u start_us=%llu end_us=%llu ok=%u bad=%u rejected=%u v11=%u,%llu,%llu v12=%u,%llu,%llu v13=%u,%llu,%llu v14=%u,%llu,%llu v15=%u,%llu,%llu v16=%u,%llu,%llu v17=%u,%llu,%llu v18=%u,%llu,%llu v19=%u,%llu,%llu v20=%u,%llu,%llu",
    lane,(unsigned long long)done->seq,(unsigned)done->kind,
    (unsigned long long)done->start_us,(unsigned long long)done->end_us,
    done->ok?1U:0U,(unsigned)done->bad_scopes,(unsigned)done->rejected,
    (unsigned)done->stages[11].count,(unsigned long long)done->stages[11].sum_us,(unsigned long long)done->stages[11].max_us,
    (unsigned)done->stages[12].count,(unsigned long long)done->stages[12].sum_us,(unsigned long long)done->stages[12].max_us,
    (unsigned)done->stages[13].count,(unsigned long long)done->stages[13].sum_us,(unsigned long long)done->stages[13].max_us,
    (unsigned)done->stages[14].count,(unsigned long long)done->stages[14].sum_us,(unsigned long long)done->stages[14].max_us,
    (unsigned)done->stages[15].count,(unsigned long long)done->stages[15].sum_us,(unsigned long long)done->stages[15].max_us,
    (unsigned)done->stages[16].count,(unsigned long long)done->stages[16].sum_us,(unsigned long long)done->stages[16].max_us,
    (unsigned)done->stages[17].count,(unsigned long long)done->stages[17].sum_us,(unsigned long long)done->stages[17].max_us,
    (unsigned)done->stages[18].count,(unsigned long long)done->stages[18].sum_us,(unsigned long long)done->stages[18].max_us,
    (unsigned)done->stages[19].count,(unsigned long long)done->stages[19].sum_us,(unsigned long long)done->stages[19].max_us,
    (unsigned)done->stages[20].count,(unsigned long long)done->stages[20].sum_us,(unsigned long long)done->stages[20].max_us);
#if PET_P4_SPARE_OPEN_TRACE
  ESP_LOGI("pet-widget-trace", "P4 widget trace part=3 lane=%u seq=%llu kind=%u start_us=%llu end_us=%llu ok=%u bad=%u rejected=%u v21=%u,%llu,%llu v22=%u,%llu,%llu",
    lane,(unsigned long long)done->seq,(unsigned)done->kind,
    (unsigned long long)done->start_us,(unsigned long long)done->end_us,
    done->ok?1U:0U,(unsigned)done->bad_scopes,(unsigned)done->rejected,
    (unsigned)done->stages[21].count,(unsigned long long)done->stages[21].sum_us,(unsigned long long)done->stages[21].max_us,
    (unsigned)done->stages[22].count,(unsigned long long)done->stages[22].sum_us,(unsigned long long)done->stages[22].max_us);
#endif
}

void pet_widget_trace_end(pet_widget_trace_operation_t *token,bool ok) {
  PRESERVE_TRACE_ERRNO;
  if(!token||!token->owned)return;
  token->owned=false;
  if(token->lane>=2)return;
  trace_lane_t done={0};
  bool emit=false;
  TaskHandle_t task=xTaskGetCurrentTaskHandle();
  uint64_t now=(uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&trace_mux);
  trace_lane_t *state=&trace_lanes[token->lane];
  if(state->active&&state->seq==token->seq&&state->task==task){
    state->end_us=now;state->ok=ok;
    if(state->depth&&state->bad_scopes!=UINT32_MAX)state->bad_scopes++;
    done=*state;emit=true;
    state->last_seq=state->seq;state->last_start_us=state->start_us;state->last_end_us=now;
    state->active=false;state->stage=state->depth=0;state->task=NULL;
  }
  portEXIT_CRITICAL(&trace_mux);
  if(emit)emit_trace(token->lane,&done);
}
void pet_widget_trace_operation_cleanup(pet_widget_trace_operation_t *token) {
  pet_widget_trace_end(token,false);
}
void pet_widget_trace_snapshot(pet_widget_trace_snapshot_t *out) {
  PRESERVE_TRACE_ERRNO;
  if(!out)return;
  portENTER_CRITICAL(&trace_mux);
  for(unsigned lane=0;lane<2;lane++){
    const trace_lane_t *state=&trace_lanes[lane];
    out->lanes[lane]=(pet_widget_trace_lane_snapshot_t){
      state->active?state->seq:0,state->active?state->start_us:0,
      state->last_seq,state->last_start_us,state->last_end_us,
      (uint8_t)lane,state->active?state->kind:0,state->stage,state->active?1:0};
  }
  portEXIT_CRITICAL(&trace_mux);
}
