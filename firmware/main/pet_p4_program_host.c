#include "pet_p4_program_host.h"
#include <limits.h>
#include <string.h>

#define HOST_MARKER UINT32_C(0x50344831)
static bool ranges_overlap(const void *a,size_t an,const void *b,size_t bn){
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
  return x<=y?y-x<an:x-y<bn;
}
static bool overlaps(const pet_program_host_t *h, const void *p, size_t n) {
  uintptr_t a = (uintptr_t)h, b = (uintptr_t)p;
  return a <= b ? b - a < sizeof(*h) : a - b < n;
}
static bool valid_backend(const pet_program_host_backend_t *b) {
  return b && b->audio_busy && b->owner_open && b->acquire && b->current && b->revoke && b->finish;
}
static bool valid_owner(pet_resource_owner_t o) {
  return o.broker && o.generation && o.slot < PET_RESOURCE_OWNER_SLOTS;
}
static bool same_owner(pet_resource_owner_t a, pet_resource_owner_t b) {
  return a.broker == b.broker && a.generation == b.generation && a.slot == b.slot;
}
static bool empty_owner(pet_resource_owner_t owner) {
  return !owner.broker && !owner.generation && !owner.slot;
}
static bool own_ticket(const pet_program_host_t *h,uint16_t fuel) {
  return valid_owner(h->owner) && same_owner(h->lease.owner, h->owner)
    && h->lease.serial && h->lease.slot < PET_RESOURCE_LEASE_SLOTS
    && h->lease.kind == h->admission_kind && h->lease.units == fuel;
}
static pet_program_runtime_result_t enter(pet_program_host_t *h) {
  if (!h || h->instance!=h || h->runtime.instance!=&h->runtime || h->marker != HOST_MARKER
      ||(h->admission_kind!=PET_RESOURCE_PROGRAM&&h->admission_kind!=PET_RESOURCE_NATIVE_PROBE)) return PET_RUNTIME_INVALID;
  if (h->inside) return PET_RUNTIME_BUSY;
  h->inside = true;
  return PET_RUNTIME_OK;
}
static pet_program_runtime_result_t leave(pet_program_host_t *h, pet_program_runtime_result_t result) {
  h->inside = false;
  return result;
}
/* The runtime only receives this wrapper and this fixed host address. There is
 * no public current callback or authority argument on any execution API. */
static bool current(void *context, uint64_t generation) {
  pet_program_host_t *h = context;
  return h->instance==h&&h->runtime.instance==&h->runtime&&h->marker == HOST_MARKER && h->inside && h->bound
    && generation == h->owner.generation && valid_owner(h->owner)
    && same_owner(h->lease.owner, h->owner) && h->lease.kind == h->admission_kind
    && h->lease.serial && h->lease.slot < PET_RESOURCE_LEASE_SLOTS
    && h->admitted_fuel && h->lease.units == h->admitted_fuel
    && !h->backend.audio_busy(h->backend.context)
    && h->backend.current(h->backend.context, h->lease);
}
static bool publish(void *context,uint64_t generation,pet_program_state_t *target,const pet_program_state_t *candidate){
  pet_program_host_t *h=context;
  return h->instance==h&&h->runtime.instance==&h->runtime&&h->inside&&h->bound&&h->publish&&generation==h->owner.generation
    &&h->publish(h->publish_context,h->lease,target,candidate);
}
/* Only after runtime start failed, or actual runtime work has stopped. Never
 * finish a foreign owner/kind: a malformed trusted-backend ticket is a backend
 * contract error, not authority to release another owner's physical work. */
static void release(pet_program_host_t *h) {
  if (valid_owner(h->owner)) h->backend.revoke(h->backend.context, h->owner);
  if (h->bound) h->last_finish = h->backend.finish(h->backend.context, h->lease);
  h->bound = false;
  h->admitted_fuel = 0;
  memset(&h->owner, 0, sizeof(h->owner));
  memset(&h->lease, 0, sizeof(h->lease));
}
static pet_program_runtime_result_t admission(pet_resource_result_t result) {
  if (result == PET_RESOURCE_BUSY || result == PET_RESOURCE_EXHAUSTED) return PET_RUNTIME_BUSY;
  if (result == PET_RESOURCE_STALE) return PET_RUNTIME_STALE;
  return PET_RUNTIME_DENIED;
}
static bool init_kind(pet_program_host_t *h,const pet_program_host_backend_t *b,pet_resource_kind_t kind) {
  if (!h || !valid_backend(b) || overlaps(h,b,sizeof(*b))) return false;
  /* Test semantic lifecycle/identity fields, never padding: C's {0} does not
   * promise a byte-zero representation for padding (or for null pointers). */
  if (h->instance || h->runtime.instance || h->marker || h->inside || h->bound || h->publish || h->publish_context || h->clock || h->clock_context || h->slice_budget_us || h->last_clock_us || h->clock_seen || h->zero_progress_turns || h->slice_stalled || h->admission_attempted || h->projection_bound || h->group_owner || h->staged_ready || h->greatest_generation || h->admitted_fuel || h->admission_kind
      || !empty_owner(h->owner) || !empty_owner(h->lease.owner) || h->lease.serial || h->lease.units
      || h->lease.kind != 0 || h->lease.slot || h->runtime.marker || h->runtime.inside || h->runtime.active
      || h->runtime.report_pending || h->runtime.output_limited || h->runtime.publish || h->runtime.publish_context || h->runtime.generation || h->runtime.last_sequence || h->runtime.inflight
      || h->runtime.head || h->runtime.queued || h->runtime.state.generation || h->runtime.state.revision
      || h->runtime.state.outbox_count || h->staged.valid || h->runtime.image.valid) return false;
  memset(h,0,sizeof(*h));
  h->instance=h;h->backend = *b; h->marker = HOST_MARKER;
  h->admission_kind=kind;
  h->last_admission = PET_RESOURCE_INVALID; h->last_finish = PET_RESOURCE_FINISH_STALE;
  pet_program_runtime_init(&h->runtime);
  return true;
}
bool pet_program_host_init(pet_program_host_t *h,const pet_program_host_backend_t *b){
  return init_kind(h,b,PET_RESOURCE_PROGRAM);
}
bool pet_program_host_init_native_probe(pet_program_host_t *h,const pet_program_host_backend_t *b){
  return init_kind(h,b,PET_RESOURCE_NATIVE_PROBE);
}
bool pet_program_host_bind_publication(pet_program_host_t *h,pet_program_host_publish_fn callback,void *context){
  if(!h||h->instance!=h||h->runtime.instance!=&h->runtime||h->marker!=HOST_MARKER||h->inside||h->bound||h->publish||h->greatest_generation||h->admission_attempted||!callback
      ||(context&&overlaps(h,context,1)))return false;
  h->publish=callback;h->publish_context=context;return true;
}
bool pet_program_host_bind_clock(pet_program_host_t *h,pet_program_clock_fn clock,void *context,uint32_t budget_us){
  if(!h||h->instance!=h||h->runtime.instance!=&h->runtime||h->marker!=HOST_MARKER||h->inside||h->bound||h->clock||h->greatest_generation||h->admission_attempted
      ||!clock||!budget_us||budget_us>2000||(context&&overlaps(h,context,1)))return false;
  h->clock=clock;h->clock_context=context;h->slice_budget_us=budget_us;return true;
}
pet_program_runtime_result_t pet_program_host_stage(pet_program_host_t *h, const pet_program_image_t *image) {
  pet_program_runtime_result_t result = enter(h);
  if (result != PET_RUNTIME_OK) return result;
  if (!image || overlaps(h,image,sizeof(*image))) return leave(h,PET_RUNTIME_INVALID);
  pet_program_t program;
  if (!pet_program_image_program(image,&program)) return leave(h,PET_RUNTIME_BAD_IMAGE);
  memcpy(&h->staged,image,sizeof(*image)); h->staged_ready = true;
  return leave(h,PET_RUNTIME_OK);
}
static pet_program_runtime_result_t start_image(pet_program_host_t *h,
    const pet_program_image_t *image,uint16_t quantum,const pet_program_snapshot_t *seed,
    const pet_program_output_contract_t *contract,uint64_t *generation) {
  pet_program_runtime_result_t result;
  if (!generation || overlaps(h,generation,sizeof(*generation))
      || (seed && (overlaps(h,seed,sizeof(*seed))||ranges_overlap(seed,sizeof(*seed),generation,sizeof(*generation))))
      || (contract&&(overlaps(h,contract,sizeof(*contract))||ranges_overlap(contract,sizeof(*contract),generation,sizeof(*generation))
          ||!pet_program_output_contract_valid(contract)))
      || !quantum || quantum > PET_PROGRAM_QUANTUM)
    return PET_RUNTIME_INVALID;
  if (h->bound || h->runtime.active || h->runtime.report_pending) return PET_RUNTIME_BUSY;
  pet_program_t program;
  if(!image||!pet_program_image_program(image,&program))return PET_RUNTIME_BAD_IMAGE;
  h->admission_attempted=true;
  if (h->backend.audio_busy(h->backend.context)) {
    h->last_admission = PET_RESOURCE_PROTECTED; return PET_RUNTIME_DENIED;
  }
  h->last_finish = PET_RESOURCE_FINISH_STALE;
  h->last_admission = h->backend.owner_open(h->backend.context, PET_RESOURCE_MASK(h->admission_kind), &h->owner);
  if (h->last_admission != PET_RESOURCE_OK) {
    /* Failed backends must leave outputs zero, as required by the broker. */
    memset(&h->owner,0,sizeof(h->owner)); return admission(h->last_admission);
  }
  if (!valid_owner(h->owner) || h->owner.generation <= h->greatest_generation) {
    release(h); return PET_RUNTIME_STALE;
  }
  h->greatest_generation = h->owner.generation;
  h->last_admission = h->backend.acquire(h->backend.context, h->owner, h->admission_kind, image->fuel, &h->lease);
  if (h->last_admission != PET_RESOURCE_OK) {
    release(h); return admission(h->last_admission);
  }
  if (!own_ticket(h,image->fuel)) { release(h); return PET_RUNTIME_DENIED; }
  h->bound = true; h->admitted_fuel = image->fuel;
  result = pet_program_runtime_start_with_publication(&h->runtime,image,h->owner.generation,current,h,quantum,seed,contract,h->publish?publish:NULL,h->publish?h:NULL);
  if (result != PET_RUNTIME_OK) release(h);
  else {
    memset(&h->projection,0,sizeof(h->projection));h->projection_bound=false;
    h->zero_progress_turns=0;h->slice_stalled=false;
    h->group_owner=NULL;
    *generation = h->owner.generation;
  }
  return result;
}
pet_program_runtime_result_t pet_program_host_start(pet_program_host_t *h,
    uint16_t quantum,const pet_program_snapshot_t *seed,uint64_t *generation){
  pet_program_runtime_result_t result=enter(h);
  if(result!=PET_RUNTIME_OK)return result;
  return leave(h,h->staged_ready?start_image(h,&h->staged,quantum,seed,NULL,generation):PET_RUNTIME_BAD_IMAGE);
}
pet_program_runtime_result_t pet_program_host_start_image_with_contract(pet_program_host_t *h,
    const pet_program_image_t *image,uint16_t quantum,const pet_program_snapshot_t *seed,
    const pet_program_output_contract_t *contract,uint64_t *generation){
  pet_program_runtime_result_t result=enter(h);
  if(result!=PET_RUNTIME_OK)return result;
  if(!image||overlaps(h,image,sizeof(*image))
      ||(generation&&ranges_overlap(image,sizeof(*image),generation,sizeof(*generation))))return leave(h,PET_RUNTIME_INVALID);
  result=start_image(h,image,quantum,seed,contract,generation);
  if(result==PET_RUNTIME_OK){memcpy(&h->staged,image,sizeof(*image));h->staged_ready=true;}
  return leave(h,result);
}
pet_program_runtime_result_t pet_program_host_start_image(pet_program_host_t *h,
    const pet_program_image_t *image,uint16_t quantum,const pet_program_snapshot_t *seed,uint64_t *generation){
  return pet_program_host_start_image_with_contract(h,image,quantum,seed,NULL,generation);
}
static bool projection_contract(const pet_program_ui_projection_t *p,pet_program_output_contract_t *contract){
  if(!p||!p->schema||!p->variables||p->variables>PET_PROGRAM_UI_VARIABLES)return false;
  for(unsigned i=0;i<PET_PROGRAM_UI_VARIABLES;i++){
    if(i<p->variables){if(p->minimum[i]>p->maximum[i])return false;}
    else if(p->minimum[i]||p->maximum[i])return false;
  }
  uint16_t seen=0;
  for(unsigned i=0;i<PET_PROGRAM_OUTPUTS;i++){
    unsigned target=p->target[i];
    contract->minimum[i]=INT32_MIN;contract->maximum[i]=INT32_MAX;
    if(target==PET_PROGRAM_UI_UNBOUND)continue;
    if(target>=p->variables||(seen&(1U<<target)))return false;
    seen|=(uint16_t)(1U<<target);
    contract->minimum[i]=p->minimum[target];contract->maximum[i]=p->maximum[target];
  }
  return seen!=0;
}
pet_program_runtime_result_t pet_program_host_start_image_with_projection(pet_program_host_t *h,
    const pet_program_image_t *image,uint16_t quantum,const pet_program_snapshot_t *seed,
    const pet_program_ui_projection_t *projection,uint64_t *generation){
  pet_program_runtime_result_t result=enter(h);
  if(result!=PET_RUNTIME_OK)return result;
  pet_program_output_contract_t contract;
  if(!image||!projection||overlaps(h,image,sizeof(*image))||overlaps(h,projection,sizeof(*projection))
      ||(generation&&(ranges_overlap(image,sizeof(*image),generation,sizeof(*generation))
        ||ranges_overlap(projection,sizeof(*projection),generation,sizeof(*generation))))
      ||!projection_contract(projection,&contract))return leave(h,PET_RUNTIME_INVALID);
  if(projection->schema!=image->schema)return leave(h,PET_RUNTIME_BAD_STATE);
  result=start_image(h,image,quantum,seed,&contract,generation);
  if(result==PET_RUNTIME_OK){
    memcpy(&h->staged,image,sizeof(*image));h->staged_ready=true;
    memcpy(&h->projection,projection,sizeof(*projection));h->projection_bound=true;
  }
  return leave(h,result);
}
pet_program_runtime_result_t pet_program_host_enqueue(pet_program_host_t *h,
    uint64_t generation, uint64_t sequence, const pet_program_event_t *event) {
  pet_program_runtime_result_t result = enter(h);
  if (result != PET_RUNTIME_OK) return result;
  if (!event || overlaps(h,event,sizeof(*event))) return leave(h,PET_RUNTIME_INVALID);
  result = pet_program_runtime_enqueue(&h->runtime,generation,sequence,event);
  if (h->bound && !h->runtime.active) release(h);
  return leave(h,result);
}
pet_program_runtime_result_t pet_program_host_pump(pet_program_host_t *h, uint64_t generation) {
  pet_program_runtime_result_t result = enter(h);
  if (result != PET_RUNTIME_OK) return result;
  if(h->clock&&h->bound&&h->runtime.active&&generation==h->runtime.generation
      &&h->runtime.inflight&&!h->runtime.report_pending&&h->runtime.workspace.result==PET_PROGRAM_RUNNING){
    pet_program_deadline_t deadline={0};
    if(!pet_program_deadline_begin(&deadline,h->clock,h->clock_context,h->slice_budget_us)
        ||(h->clock_seen&&deadline.started<h->last_clock_us)){
      (void)pet_program_runtime_cancel(&h->runtime,generation);result=PET_RUNTIME_DENIED;
    }else{
      uint16_t previous_fuel=h->runtime.workspace.used_fuel;
      result=pet_program_runtime_pump_with_checkpoint(&h->runtime,generation,pet_program_deadline_check,&deadline);
      h->last_clock_us=deadline.last;h->clock_seen=true;
      if(result==PET_RUNTIME_YIELDED&&deadline.yielded&&previous_fuel==h->runtime.workspace.used_fuel){
        if(++h->zero_progress_turns>=8){
          h->slice_stalled=true;(void)pet_program_runtime_cancel(&h->runtime,generation);result=PET_RUNTIME_DENIED;
        }
      }else h->zero_progress_turns=0;
    }
  }else result=pet_program_runtime_pump(&h->runtime,generation);
  if (h->bound && !h->runtime.active) release(h);
  return leave(h,result);
}
pet_program_runtime_result_t pet_program_host_cancel(pet_program_host_t *h, uint64_t generation) {
  pet_program_runtime_result_t result = enter(h);
  if (result != PET_RUNTIME_OK) return result;
  result = pet_program_runtime_cancel(&h->runtime,generation);
  if (result == PET_RUNTIME_OK && h->bound) release(h);
  return leave(h,result);
}
pet_program_runtime_result_t pet_program_host_take_report(pet_program_host_t *h,
    uint64_t generation, pet_program_report_t *out) {
  pet_program_runtime_result_t result = enter(h);
  if (result != PET_RUNTIME_OK) return result;
  if (!out || overlaps(h,out,sizeof(*out))) return leave(h,PET_RUNTIME_INVALID);
  return leave(h,pet_program_runtime_take_report(&h->runtime,generation,out));
}
pet_program_runtime_result_t pet_program_host_snapshot(pet_program_host_t *h,
    uint64_t generation, pet_program_snapshot_t *out) {
  pet_program_runtime_result_t result = enter(h);
  if (result != PET_RUNTIME_OK) return result;
  if (!out || overlaps(h,out,sizeof(*out))) return leave(h,PET_RUNTIME_INVALID);
  return leave(h,pet_program_runtime_snapshot(&h->runtime,generation,out));
}
pet_program_runtime_result_t pet_program_host_snapshot_if_changed(pet_program_host_t *h,
    uint64_t generation,const pet_program_snapshot_cursor_t *known,
    pet_program_snapshot_t *out,pet_program_snapshot_cursor_t *cursor) {
  pet_program_runtime_result_t result=enter(h);
  if(result!=PET_RUNTIME_OK)return result;
  if(!out||!cursor||overlaps(h,out,sizeof(*out))||overlaps(h,cursor,sizeof(*cursor))
      ||(known&&overlaps(h,known,sizeof(*known))))return leave(h,PET_RUNTIME_INVALID);
  return leave(h,pet_program_runtime_snapshot_if_changed(&h->runtime,generation,known,out,cursor));
}
static pet_program_runtime_result_t deny_current(pet_program_host_t *h,uint64_t generation){
  (void)pet_program_runtime_cancel(&h->runtime,generation);
  if(h->bound)release(h);
  return leave(h,PET_RUNTIME_DENIED);
}
pet_program_runtime_result_t pet_program_host_check_current(pet_program_host_t *h,uint64_t generation){
  pet_program_runtime_result_t result=enter(h);
  if(result!=PET_RUNTIME_OK)return result;
  if(!generation||generation!=h->runtime.generation)return leave(h,PET_RUNTIME_STALE);
  if(!h->bound||!h->runtime.active)return leave(h,PET_RUNTIME_STOPPED);
  if(!current(h,generation))return deny_current(h,generation);
  return leave(h,PET_RUNTIME_OK);
}
pet_program_runtime_result_t pet_program_host_project_current(pet_program_host_t *h,
    uint64_t generation,const int32_t base[PET_PROGRAM_UI_VARIABLES],pet_program_ui_snapshot_t *out){
  pet_program_runtime_result_t result=enter(h);
  if(result!=PET_RUNTIME_OK)return result;
  const size_t bytes=sizeof(int32_t)*PET_PROGRAM_UI_VARIABLES;
  if(!base||!out||overlaps(h,base,bytes)||overlaps(h,out,sizeof(*out))
      ||ranges_overlap(base,bytes,out,sizeof(*out)))return leave(h,PET_RUNTIME_INVALID);
  if(!generation||generation!=h->runtime.generation)return leave(h,PET_RUNTIME_STALE);
  if(!h->bound||!h->runtime.active)return leave(h,PET_RUNTIME_STOPPED);
  if(!current(h,generation))return deny_current(h,generation);
  if(!h->projection_bound)return leave(h,PET_RUNTIME_BAD_STATE);
  const pet_program_ui_projection_t *p=&h->projection;
  pet_program_ui_snapshot_t staged={.generation=generation,.schema=p->schema,
    .revision=pet_program_state_read(&h->runtime.state)->revision,.variables=p->variables};
  for(unsigned i=0;i<PET_PROGRAM_UI_VARIABLES;i++){
    if(i<p->variables&&(base[i]<p->minimum[i]||base[i]>p->maximum[i]))return leave(h,PET_RUNTIME_BAD_STATE);
    staged.values[i]=base[i];
  }
  for(unsigned i=0;i<PET_PROGRAM_OUTPUTS;i++){
    unsigned target=p->target[i];
    if(target==PET_PROGRAM_UI_UNBOUND)continue;
    int32_t value=pet_program_state_read(&h->runtime.state)->outputs[i];
    if(value<p->minimum[target]||value>p->maximum[target])return leave(h,PET_RUNTIME_BAD_STATE);
    staged.values[target]=value;
  }
  if(!current(h,generation))return deny_current(h,generation);
  *out=staged;
  return leave(h,PET_RUNTIME_OK);
}
