#include "pet_p4_program_runtime.h"
#include <limits.h>

#define RUNTIME_MARKER UINT32_C(0x50345231)
static void zero_bytes(void *target, size_t count) {
  unsigned char *out = target;
  for (size_t i = 0; i < count; i++) out[i] = 0;
}
static void copy_bytes(void *target, const void *source, size_t count) {
  unsigned char *out = target;
  const unsigned char *in = source;
  for (size_t i = 0; i < count; i++) out[i] = in[i];
}
/* Keep the full native snapshot byte contract (including padding and invalid
 * unused cells for the caller's validator), without clearing arrays immediately
 * before overwriting every byte of them. No short/declared-cells-only copy. */
static void copy_snapshot(const pet_program_image_t *image,const pet_program_state_t *state,
    pet_program_snapshot_t *out) {
  const size_t values_end=offsetof(pet_program_snapshot_t,values)+sizeof(out->values);
  const size_t outputs_at=offsetof(pet_program_snapshot_t,outputs);
  const size_t outputs_end=outputs_at+sizeof(out->outputs);
  zero_bytes(out,offsetof(pet_program_snapshot_t,values));
  out->schema=image->schema;out->cells=image->cells;out->revision=state->revision;
  copy_bytes(out->values,state->cells,sizeof(out->values));
  zero_bytes((unsigned char*)out+values_end,outputs_at-values_end);
  copy_bytes(out->outputs,state->outputs,sizeof(out->outputs));
  zero_bytes((unsigned char*)out+outputs_end,sizeof(*out)-outputs_end);
}
static bool overlaps(const void *a, size_t an, const void *b, size_t bn) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
  return x <= y ? y - x < an : x - y < bn;
}
static pet_program_runtime_result_t enter(pet_program_runtime_t *r) {
  if (!r || r->instance!=r || r->marker != RUNTIME_MARKER) return PET_RUNTIME_INVALID;
  if (r->inside) return PET_RUNTIME_BUSY;
  r->inside = true;
  return PET_RUNTIME_OK;
}
static pet_program_runtime_result_t leave(pet_program_runtime_t *r, pet_program_runtime_result_t result) {
  r->inside = false;
  return result;
}
static void report(pet_program_runtime_t *r, pet_program_result_t result) {
  r->report = (pet_program_report_t){r->generation, r->inflight, result,
    pet_program_state_read(&r->state)->revision, r->workspace.used_fuel};
  r->report_pending = true;
  r->inflight = 0;
}
static void stop(pet_program_runtime_t *r) {
  r->cancelled_events += (uint32_t)r->queued + (r->inflight ? 1U : 0U);
  if (r->inflight && !r->report_pending) report(r, PET_PROGRAM_CANCELLED);
  r->active = false; r->head = 0; r->queued = 0; r->inflight = 0;
  zero_bytes(r->queue, sizeof(r->queue));
  zero_bytes(r->sequences, sizeof(r->sequences));
  zero_bytes(&r->workspace, sizeof(r->workspace));
}
static bool current(pet_program_runtime_t *r) {
  return r->active && r->authority.current(r->authority.context, r->generation);
}
void pet_program_runtime_init(pet_program_runtime_t *r) {
  if (!r) return;
  zero_bytes(r, sizeof(*r));
  r->instance=r;
  r->marker = RUNTIME_MARKER;
}
pet_program_runtime_result_t pet_program_runtime_start_with_publication(pet_program_runtime_t *r,
    const pet_program_image_t *image, uint64_t generation,
    bool (*check)(void *, uint64_t), void *context, uint16_t quantum,
    const pet_program_snapshot_t *seed,const pet_program_output_contract_t *contract,
    pet_program_publish_fn publish,void *publish_context) {
  pet_program_runtime_result_t result = enter(r);
  if (result != PET_RUNTIME_OK) return result;
  if (!image || !check || !generation || !quantum || quantum > PET_PROGRAM_QUANTUM || (!publish&&publish_context)
      || overlaps(r, sizeof(*r), image, sizeof(*image))
      || (seed && overlaps(r, sizeof(*r), seed, sizeof(*seed)))
      || (contract && (overlaps(r,sizeof(*r),contract,sizeof(*contract))||!pet_program_output_contract_valid(contract))))
    return leave(r, PET_RUNTIME_INVALID);
  if (r->active || r->report_pending) return leave(r, PET_RUNTIME_BUSY);
  if (generation <= r->generation) return leave(r, PET_RUNTIME_STALE);
  pet_program_t program;
  if (!pet_program_image_program(image, &program)) return leave(r, PET_RUNTIME_BAD_IMAGE);
  if (seed) {
    if (seed->schema != image->schema || seed->cells != image->cells)
      return leave(r, PET_RUNTIME_BAD_STATE);
    for (unsigned i = seed->cells; i < PET_PROGRAM_CELLS; i++)
      if (seed->values[i]) return leave(r, PET_RUNTIME_BAD_STATE);
  }
  if(contract){
    const int32_t blank[PET_PROGRAM_OUTPUTS]={0};
    if(!pet_program_outputs_fit(seed?seed->outputs:blank,contract))return leave(r,PET_RUNTIME_BAD_STATE);
  }
  if (!check(context, generation)) return leave(r, PET_RUNTIME_DENIED);
  /* Caller holds the owner/state lock through this bounded local publication.
   * The check cannot mutate inputs or revoke authority after returning true. */
  copy_bytes(&r->image, image, sizeof(*image));
  r->output_limited=contract!=NULL;
  if(contract)copy_bytes(&r->output_contract,contract,sizeof(*contract));
  else zero_bytes(&r->output_contract,sizeof(r->output_contract));
  (void)pet_program_init(&r->state, generation);
  if (seed) {
    r->state.revision = seed->revision;
    copy_bytes(r->state.cells, seed->values, sizeof(seed->values));
    copy_bytes(r->state.outputs, seed->outputs, sizeof(seed->outputs));
  }
#if PET_P4_PROGRAM_V2
  zero_bytes(&r->publication,sizeof(r->publication));
  if(publish && !pet_program_state_bind_publication(&r->state,&r->publication,r->publication_slots))return leave(r,PET_RUNTIME_INVALID);
#endif
  zero_bytes(&r->workspace, sizeof(r->workspace));
  zero_bytes(r->queue, sizeof(r->queue)); zero_bytes(r->sequences, sizeof(r->sequences));
  zero_bytes(&r->report, sizeof(r->report));
  r->authority = (pet_program_authority_t){generation, check, context, NULL, 0};
  r->publish=publish;r->publish_context=publish_context;
  r->generation = generation; r->quantum = quantum; r->active = true;
  r->head = 0; r->queued = 0; r->last_sequence = 0; r->inflight = 0;
  r->cancelled_events = 0; r->report_pending = false;
  return leave(r, PET_RUNTIME_OK);
}
pet_program_runtime_result_t pet_program_runtime_start_with_contract(pet_program_runtime_t *r,
    const pet_program_image_t *image,uint64_t generation,bool (*check)(void *,uint64_t),void *context,
    uint16_t quantum,const pet_program_snapshot_t *seed,const pet_program_output_contract_t *contract){
  return pet_program_runtime_start_with_publication(r,image,generation,check,context,quantum,seed,contract,NULL,NULL);
}
pet_program_runtime_result_t pet_program_runtime_start(pet_program_runtime_t *r,
    const pet_program_image_t *image,uint64_t generation,bool (*check)(void *,uint64_t),void *context,
    uint16_t quantum,const pet_program_snapshot_t *seed){
  return pet_program_runtime_start_with_contract(r,image,generation,check,context,quantum,seed,NULL);
}
pet_program_runtime_result_t pet_program_runtime_enqueue(pet_program_runtime_t *r,
    uint64_t generation, uint64_t sequence, const pet_program_event_t *event) {
  pet_program_runtime_result_t result = enter(r);
  if (result != PET_RUNTIME_OK) return result;
  if (!event || !sequence || event->count > PET_PROGRAM_EVENT_WORDS
      || overlaps(r, sizeof(*r), event, sizeof(*event))) return leave(r, PET_RUNTIME_INVALID);
  if (!generation || generation != r->generation) return leave(r, PET_RUNTIME_STALE);
  if (!r->active) return leave(r, PET_RUNTIME_STOPPED);
  if (!current(r)) { stop(r); return leave(r, PET_RUNTIME_DENIED); }
  if (sequence <= r->last_sequence) return leave(r, PET_RUNTIME_DUPLICATE);
  if (r->queued == PET_PROGRAM_QUEUE) return leave(r, PET_RUNTIME_QUEUE_FULL);
  unsigned tail = ((unsigned)r->head + r->queued) % PET_PROGRAM_QUEUE;
  /* Ignore unused event words, so stale host buffers never reach the program. */
  zero_bytes(&r->queue[tail], sizeof(r->queue[tail]));
  r->queue[tail].count = event->count;
  for (unsigned i = 0; i < event->count; i++) r->queue[tail].words[i] = event->words[i];
  r->sequences[tail] = sequence; r->last_sequence = sequence; r->queued++;
  return leave(r, PET_RUNTIME_OK);
}
pet_program_runtime_result_t pet_program_runtime_pump_with_checkpoint(pet_program_runtime_t *r,uint64_t generation,
    pet_program_checkpoint_fn checkpoint,void *context) {
  pet_program_runtime_result_t result = enter(r);
  if (result != PET_RUNTIME_OK) return result;
  if (!generation || generation != r->generation) return leave(r, PET_RUNTIME_STALE);
  if (!r->active) return leave(r, PET_RUNTIME_STOPPED);
  if (!current(r)) { stop(r); return leave(r, PET_RUNTIME_DENIED); }
  if (r->report_pending) return leave(r, PET_RUNTIME_REPORT_PENDING);
  pet_program_result_t core;
  if (!r->inflight) {
    if (!r->queued) return leave(r, PET_RUNTIME_IDLE);
    r->inflight = r->sequences[r->head];
    pet_program_t p = {r->image.code, r->image.count, r->image.fuel, r->image.cells, 0
#if PET_P4_PROGRAM_V2
      , r->image.grid_cols, r->image.grid_rows, r->image.texts
#endif
    };
    core = pet_program_begin(&p, &r->queue[r->head], &r->state, &r->authority, &r->workspace);
    zero_bytes(&r->queue[r->head], sizeof(r->queue[r->head]));
    r->sequences[r->head] = 0;
    r->head = (uint8_t)((r->head + 1U) % PET_PROGRAM_QUEUE); r->queued--;
  } else if (r->workspace.result == PET_PROGRAM_READY) {
    core = pet_program_commit_with_publication(&r->state,&r->workspace,&r->authority,r->output_limited?&r->output_contract:NULL,r->publish,r->publish_context);
  } else {
    core = checkpoint?pet_program_step_with_checkpoint(&r->workspace,&r->authority,r->quantum,checkpoint,context)
      :pet_program_step(&r->workspace,&r->authority,r->quantum);
  }
  if (core == PET_PROGRAM_RUNNING || core == PET_PROGRAM_READY) return leave(r, PET_RUNTIME_YIELDED);
  if (core == PET_PROGRAM_CANCELLED) r->cancelled_events++;
  report(r, core);
  if (core == PET_PROGRAM_CANCELLED) stop(r);
  /* Intentionally no current() call after commit: its outcome is final. */
  return leave(r, PET_RUNTIME_COMPLETE);
}
pet_program_runtime_result_t pet_program_runtime_pump(pet_program_runtime_t *r,uint64_t generation){
  return pet_program_runtime_pump_with_checkpoint(r,generation,NULL,NULL);
}
pet_program_runtime_result_t pet_program_runtime_cancel(pet_program_runtime_t *r, uint64_t generation) {
  pet_program_runtime_result_t result = enter(r);
  if (result != PET_RUNTIME_OK) return result;
  if (!generation || generation != r->generation) return leave(r, PET_RUNTIME_STALE);
  stop(r);
  return leave(r, PET_RUNTIME_OK);
}
pet_program_runtime_result_t pet_program_runtime_take_report(pet_program_runtime_t *r,
    uint64_t generation, pet_program_report_t *out) {
  pet_program_runtime_result_t result = enter(r);
  if (result != PET_RUNTIME_OK) return result;
  if (!out || overlaps(r, sizeof(*r), out, sizeof(*out))) return leave(r, PET_RUNTIME_INVALID);
  if (!generation || generation != r->generation) return leave(r, PET_RUNTIME_STALE);
  if (!r->report_pending) return leave(r, PET_RUNTIME_IDLE);
  *out = r->report; r->report_pending = false;
  return leave(r, PET_RUNTIME_OK);
}
pet_program_runtime_result_t pet_program_runtime_snapshot(pet_program_runtime_t *r,
    uint64_t generation, pet_program_snapshot_t *out) {
  pet_program_runtime_result_t result = enter(r);
  if (result != PET_RUNTIME_OK) return result;
  if (!out || overlaps(r, sizeof(*r), out, sizeof(*out))) return leave(r, PET_RUNTIME_INVALID);
  if (!generation || generation != r->generation) return leave(r, PET_RUNTIME_STALE);
  const pet_program_state_t *state=pet_program_state_read(&r->state);
  if(!state)return leave(r,PET_RUNTIME_BAD_STATE);
  copy_snapshot(&r->image,state,out);
  return leave(r, PET_RUNTIME_OK);
}

pet_program_runtime_result_t pet_program_runtime_snapshot_if_changed(pet_program_runtime_t *r,
    uint64_t generation,const pet_program_snapshot_cursor_t *known,
    pet_program_snapshot_t *out,pet_program_snapshot_cursor_t *cursor) {
  pet_program_runtime_result_t result = enter(r);
  if (result != PET_RUNTIME_OK) return result;
  if (!out || !cursor || overlaps(r,sizeof(*r),out,sizeof(*out))
      || overlaps(r,sizeof(*r),cursor,sizeof(*cursor))
      || overlaps(out,sizeof(*out),cursor,sizeof(*cursor))
      || (known && (overlaps(r,sizeof(*r),known,sizeof(*known))
          || overlaps(known,sizeof(*known),out,sizeof(*out))
          || overlaps(known,sizeof(*known),cursor,sizeof(*cursor)))))
    return leave(r,PET_RUNTIME_INVALID);
  if (!generation || generation != r->generation) return leave(r,PET_RUNTIME_STALE);
  const pet_program_state_t *state=pet_program_state_read(&r->state);
  if (!state || state->generation != generation) return leave(r,PET_RUNTIME_BAD_STATE);
  const pet_program_snapshot_cursor_t next={r,generation,r->image.schema,state->revision,r->image.cells};
  if (known && known->runtime==r && known->generation==next.generation
      && known->schema==next.schema && known->cells==next.cells && known->revision==next.revision) {
    *cursor=next;
    return leave(r,PET_RUNTIME_UNCHANGED);
  }
  copy_snapshot(&r->image,state,out);
  *cursor=next;
  return leave(r,PET_RUNTIME_OK);
}
