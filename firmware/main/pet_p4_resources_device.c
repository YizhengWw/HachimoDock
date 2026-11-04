#include "pet_p4_resources_device.h"
#include "pet_p4_reset_phase.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
static uint32_t publication_cycle_clock(void){
  __asm__ __volatile__("" ::: "memory");uint32_t value=esp_cpu_get_cycle_count();
  __asm__ __volatile__("" ::: "memory");return value;
}
#endif

static portMUX_TYPE resource_mux = portMUX_INITIALIZER_UNLOCKED;
static pet_resource_broker_t broker;

static bool same_owner(pet_resource_owner_t a, pet_resource_owner_t b) {
  return a.broker == b.broker && a.generation == b.generation && a.slot == b.slot;
}
static bool debug_matches(const pet_resource_debug_session_t *session) {
  return session && session->owner.generation
    && session->snapshot.kind == PET_RESOURCE_SNAPSHOT && session->bulk.kind == PET_RESOURCE_DEBUG_BULK
    && same_owner(session->owner, session->snapshot.owner) && same_owner(session->owner, session->bulk.owner);
}
static bool view_matches(const pet_resource_view_session_t *session) {
  return session && session->owner.generation && session->view.kind == PET_RESOURCE_HOST_VIEW
    && same_owner(session->owner, session->view.owner);
}

pet_resource_result_t pet_resources_device_debug_begin(uint32_t snapshot_bytes,
  bool audio_busy, pet_resource_debug_session_t *out) {
  if (!out || out->owner.generation || out->snapshot.serial || out->bulk.serial)
    return PET_RESOURCE_INVALID;
  pet_resource_debug_session_t next = {0};
  portENTER_CRITICAL(&resource_mux);
  pet_resources_protect(&broker, PET_RESOURCE_DEBUG_MASK, audio_busy);
  pet_resource_result_t result = audio_busy ? PET_RESOURCE_PROTECTED
    : pet_resources_owner_open(&broker, PET_RESOURCE_DEBUG_MASK, &next.owner);
  if (result == PET_RESOURCE_OK)
    result = pet_resources_try_acquire(&broker, next.owner, PET_RESOURCE_SNAPSHOT, snapshot_bytes, &next.snapshot);
  if (result == PET_RESOURCE_OK)
    result = pet_resources_try_acquire(&broker, next.owner, PET_RESOURCE_DEBUG_BULK, 1, &next.bulk);
  if (result == PET_RESOURCE_OK) *out = next;
  else if (next.owner.generation) {
    pet_resources_owner_revoke(&broker, next.owner);
    pet_resources_finish(&broker, next.snapshot);
    pet_resources_finish(&broker, next.bulk);
  }
  portEXIT_CRITICAL(&resource_mux);
  return result;
}

bool pet_resources_device_debug_current(const pet_resource_debug_session_t *session, bool audio_busy) {
  if (!debug_matches(session)) return false;
  portENTER_CRITICAL(&resource_mux);
  pet_resources_protect(&broker, PET_RESOURCE_DEBUG_MASK, audio_busy);
  if (audio_busy) pet_resources_owner_revoke(&broker, session->owner);
  bool current = !audio_busy && pet_resources_current(&broker, session->snapshot)
    && pet_resources_current(&broker, session->bulk);
  portEXIT_CRITICAL(&resource_mux);
  return current;
}

bool pet_resources_device_debug_revoke(const pet_resource_debug_session_t *session) {
  if (!debug_matches(session)) return false;
  portENTER_CRITICAL(&resource_mux);
  bool revoked = pet_resources_owner_revoke(&broker, session->owner);
  portEXIT_CRITICAL(&resource_mux);
  return revoked;
}

bool pet_resources_device_debug_end(pet_resource_debug_session_t *session) {
  if (!debug_matches(session)) return false;
  portENTER_CRITICAL(&resource_mux);
  pet_resource_finish_t snapshot = pet_resources_finish(&broker, session->snapshot);
  pet_resource_finish_t bulk = pet_resources_finish(&broker, session->bulk);
  pet_resources_owner_revoke(&broker, session->owner);
  memset(session, 0, sizeof(*session));
  portEXIT_CRITICAL(&resource_mux);
  return snapshot == PET_RESOURCE_FINISH_CURRENT && bulk == PET_RESOURCE_FINISH_CURRENT;
}

pet_resource_result_t pet_resources_device_view_begin(pet_resource_view_session_t *out) {
  if (!out || out->owner.generation || out->view.serial) return PET_RESOURCE_INVALID;
  pet_resource_view_session_t next = {0};
  portENTER_CRITICAL(&resource_mux);
  pet_resource_result_t result = pet_resources_owner_open(&broker, PET_RESOURCE_MASK(PET_RESOURCE_HOST_VIEW), &next.owner);
  if (result == PET_RESOURCE_OK)
    result = pet_resources_try_acquire(&broker, next.owner, PET_RESOURCE_HOST_VIEW, 1, &next.view);
  if (result == PET_RESOURCE_OK) *out = next;
  else if (next.owner.generation) pet_resources_owner_revoke(&broker, next.owner);
  portEXIT_CRITICAL(&resource_mux);
  return result;
}

bool pet_resources_device_view_current(const pet_resource_view_session_t *session) {
  if (!view_matches(session)) return false;
  portENTER_CRITICAL(&resource_mux);
  bool current = pet_resources_current(&broker, session->view);
  portEXIT_CRITICAL(&resource_mux);
  return current;
}

void pet_resources_device_view_end(pet_resource_view_session_t *session) {
  if (!view_matches(session)) return;
  portENTER_CRITICAL(&resource_mux);
  pet_resources_owner_revoke(&broker, session->owner);
  pet_resources_finish(&broker, session->view);
  memset(session, 0, sizeof(*session));
  portEXIT_CRITICAL(&resource_mux);
}

void pet_resources_device_audio_observed(bool busy) {
  portENTER_CRITICAL(&resource_mux);
  pet_resources_protect(&broker, PET_RESOURCE_DEBUG_MASK, busy);
  portEXIT_CRITICAL(&resource_mux);
}

void pet_resources_device_usage(pet_resource_usage_t *out) {
  portENTER_CRITICAL(&resource_mux);
  pet_resources_usage(&broker, out);
  portEXIT_CRITICAL(&resource_mux);
}

pet_resource_result_t pet_resources_device_realtime_begin(pet_resource_realtime_t *out) {
  portENTER_CRITICAL(&resource_mux);
  pet_resource_result_t result = pet_resources_realtime_begin(&broker, out);
  portEXIT_CRITICAL(&resource_mux);
  return result;
}
bool pet_resources_device_realtime_current(pet_resource_realtime_t ticket) {
  portENTER_CRITICAL(&resource_mux);
  bool current = pet_resources_realtime_current(&broker, ticket);
  portEXIT_CRITICAL(&resource_mux);
  return current;
}
bool pet_resources_device_realtime_video_ready(pet_resource_realtime_t ticket,bool ready){
  portENTER_CRITICAL(&resource_mux);
  bool current=pet_resources_realtime_video_ready(&broker,ticket,ready);
  portEXIT_CRITICAL(&resource_mux);
  return current;
}
void pet_resources_device_realtime_end(pet_resource_realtime_t *ticket) {
  if (!ticket || !ticket->serial) return;
  portENTER_CRITICAL(&resource_mux);
  (void)pet_resources_realtime_finish(&broker, *ticket);
  memset(ticket, 0, sizeof(*ticket));
  portEXIT_CRITICAL(&resource_mux);
}

pet_resource_result_t pet_resources_device_owner_open(uint32_t mask,pet_resource_owner_t *out){
  portENTER_CRITICAL(&resource_mux);
  pet_resource_result_t result=pet_resources_owner_open(&broker,mask,out);
  portEXIT_CRITICAL(&resource_mux);return result;
}
pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t owner,pet_resource_kind_t kind,uint32_t units,pet_resource_lease_t *out){
  portENTER_CRITICAL(&resource_mux);
  pet_resource_result_t result=pet_resources_try_acquire(&broker,owner,kind,units,out);
  portEXIT_CRITICAL(&resource_mux);return result;
}
bool pet_resources_device_current(pet_resource_lease_t lease){
  portENTER_CRITICAL(&resource_mux);bool result=pet_resources_current(&broker,lease);
  portEXIT_CRITICAL(&resource_mux);return result;
}
void pet_resources_device_revoke(pet_resource_owner_t owner){
  portENTER_CRITICAL(&resource_mux);pet_resources_owner_revoke(&broker,owner);portEXIT_CRITICAL(&resource_mux);
}
pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t lease){
  portENTER_CRITICAL(&resource_mux);pet_resource_finish_t result=pet_resources_finish(&broker,lease);portEXIT_CRITICAL(&resource_mux);return result;
}
static bool publication_overlap(const void *a,size_t an,const void *b,size_t bn){
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
#if PET_P4_PROGRAM_V2
/* Caller holds its model lock through staging and every snapshot. The broker
 * mux only reserves a spare and later flips a small selector. Revoke never
 * modifies payloads; a rejected spare remains private and is safe to reuse. */
static bool program_state_publish_v2(pet_resource_kind_t expected,pet_resource_lease_t lease,
    pet_program_state_t *target,const pet_program_state_t *candidate,pet_program_publication_observation_t *observation){
  _Static_assert(sizeof(pet_program_publication_t)<=512,"program selector publication must stay a short fixed commit");
  if(!target||!candidate||lease.kind!=expected||!target->publication||candidate->publication
      ||publication_overlap(target,sizeof(*target),candidate,sizeof(*candidate))
      ||publication_overlap(target,sizeof(*target),&broker,sizeof(broker))
      ||publication_overlap(candidate,sizeof(*candidate),&broker,sizeof(broker))
      ||publication_overlap(target,sizeof(*target),&resource_mux,sizeof(resource_mux))
      ||publication_overlap(candidate,sizeof(*candidate),&resource_mux,sizeof(resource_mux)))return false;
  pet_program_publication_t *p=target->publication;
  if(p->self!=p||p->target!=target||!p->slots[0]||!p->slots[1]||p->slots[0]==p->slots[1]
      ||publication_overlap(p,sizeof(*p),candidate,sizeof(*candidate))
      ||publication_overlap(p,sizeof(*p),target,sizeof(*target))
      ||publication_overlap(p,sizeof(*p),&broker,sizeof(broker))
      ||publication_overlap(p,sizeof(*p),&resource_mux,sizeof(resource_mux)))return false;
  for(unsigned i=0;i<2;i++){
    if(publication_overlap(p->slots[i],sizeof(*target),target,sizeof(*target))
        ||publication_overlap(p->slots[i],sizeof(*target),candidate,sizeof(*candidate))
        ||publication_overlap(p->slots[i],sizeof(*target),p,sizeof(*p))
        ||publication_overlap(p->slots[i],sizeof(*target),&broker,sizeof(broker))
        ||publication_overlap(p->slots[i],sizeof(*target),&resource_mux,sizeof(resource_mux)))return false;
  }
  if(publication_overlap(p->slots[0],sizeof(*target),p->slots[1],sizeof(*target)))return false;
  if(observation&&(publication_overlap(observation,sizeof(*observation),p,sizeof(*p))
      ||publication_overlap(observation,sizeof(*observation),target,sizeof(*target))
      ||publication_overlap(observation,sizeof(*observation),candidate,sizeof(*candidate))
      ||publication_overlap(observation,sizeof(*observation),p->slots[0],sizeof(*target))
      ||publication_overlap(observation,sizeof(*observation),p->slots[1],sizeof(*target))
      ||publication_overlap(observation,sizeof(*observation),&broker,sizeof(broker))
      ||publication_overlap(observation,sizeof(*observation),&resource_mux,sizeof(resource_mux))))return false;
  pet_program_state_t *before=NULL,*spare=NULL;uint32_t revision=0;
  portENTER_CRITICAL(&resource_mux);
  bool admitted=!p->preparing&&(p->live==p->slots[0]||p->live==p->slots[1])
    &&pet_resources_current(&broker,lease);
  if(admitted){
    before=p->live;revision=before->revision;
    admitted=before->generation==lease.owner.generation&&candidate->generation==before->generation
      &&revision!=UINT32_MAX&&candidate->revision==revision+1&&!before->outbox_count&&!candidate->outbox_count;
    if(admitted){p->preparing=true;spare=p->slots[before==p->slots[0]?1:0];}
  }
  portEXIT_CRITICAL(&resource_mux);
  if(!admitted)return false;
  /* This is the ONLY large copy: never under resource_mux, never into live. */
  memcpy(spare,candidate,sizeof(*spare));
  portENTER_CRITICAL(&resource_mux);
#ifdef ESP_PLATFORM
  uint32_t started_cycles=observation?publication_cycle_clock():0;
#endif
  bool committed=p->preparing&&p->live==before&&before->revision==revision
    &&before->generation==lease.owner.generation&&pet_resources_current(&broker,lease)
    &&spare->generation==before->generation&&spare->revision==revision+1&&!spare->outbox_count;
  if(committed)p->live=spare;
  p->preparing=false;
#ifdef ESP_PLATFORM
  uint32_t cycles=observation?publication_cycle_clock()-started_cycles:0;
#endif
  portEXIT_CRITICAL(&resource_mux);
  if(observation){
#ifdef ESP_PLATFORM
    *observation=(pet_program_publication_observation_t){cycles,true};
#else
    *observation=(pet_program_publication_observation_t){0,false};
#endif
  }
  return committed;
}
#endif
static bool program_state_publish(pet_resource_kind_t expected,pet_resource_lease_t lease,
    pet_program_state_t *target,const pet_program_state_t *candidate,pet_program_publication_observation_t *observation){
#if PET_P4_RESET_PHASE_TRACE
  /* Record OUTSIDE the original broker critical section. Never put RTC,
   * task lookup, CRC/hash, SDK calls or extra PSRAM work inside that lock. */
  PET_RESET_PHASE_SCOPE(publication_phase,PET_PHASE_PROGRAM_PUBLICATION);
#endif
#if PET_P4_PROGRAM_V2
  return program_state_publish_v2(expected,lease,target,candidate,observation);
#else
  _Static_assert(sizeof(pet_program_state_t)<=512,"program publication must stay a short fixed copy");
  if(!target||!candidate||lease.kind!=expected
      ||publication_overlap(target,sizeof(*target),candidate,sizeof(*candidate))
      ||publication_overlap(target,sizeof(*target),&broker,sizeof(broker))
      ||publication_overlap(candidate,sizeof(*candidate),&broker,sizeof(broker))
      ||publication_overlap(target,sizeof(*target),&resource_mux,sizeof(resource_mux))
      ||publication_overlap(candidate,sizeof(*candidate),&resource_mux,sizeof(resource_mux))
      ||(observation&&(publication_overlap(observation,sizeof(*observation),target,sizeof(*target))
        ||publication_overlap(observation,sizeof(*observation),candidate,sizeof(*candidate))
        ||publication_overlap(observation,sizeof(*observation),&broker,sizeof(broker))
        ||publication_overlap(observation,sizeof(*observation),&resource_mux,sizeof(resource_mux)))))return false;
  portENTER_CRITICAL(&resource_mux);
#ifdef ESP_PLATFORM
  uint32_t started_cycles=observation?publication_cycle_clock():0;
#endif
  bool current=pet_resources_current(&broker,lease)
    &&target->generation==lease.owner.generation&&candidate->generation==target->generation
    &&target->revision!=UINT32_MAX&&candidate->revision==target->revision+1
    &&!target->outbox_count&&!candidate->outbox_count;
  if(current)memcpy(target,candidate,sizeof(*target));
#ifdef ESP_PLATFORM
  uint32_t cycles=observation?publication_cycle_clock()-started_cycles:0;
#endif
  portEXIT_CRITICAL(&resource_mux);
  if(observation){
#ifdef ESP_PLATFORM
    *observation=(pet_program_publication_observation_t){cycles,true};
#else
    *observation=(pet_program_publication_observation_t){0,false};
#endif
  }return current;
#endif
}
bool pet_resources_device_program_publish(pet_resource_lease_t lease,pet_program_state_t *target,const pet_program_state_t *candidate){
  return program_state_publish(PET_RESOURCE_PROGRAM,lease,target,candidate,NULL);
}
bool pet_resources_device_probe_program_publish(pet_resource_lease_t lease,pet_program_state_t *target,const pet_program_state_t *candidate){
  return program_state_publish(PET_RESOURCE_NATIVE_PROBE,lease,target,candidate,NULL);
}
bool pet_resources_device_probe_program_publish_observed(pet_resource_lease_t lease,pet_program_state_t *target,const pet_program_state_t *candidate,pet_program_publication_observation_t *observation){
  return observation&&program_state_publish(PET_RESOURCE_NATIVE_PROBE,lease,target,candidate,observation);
}
