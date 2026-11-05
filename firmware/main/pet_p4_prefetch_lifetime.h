/* Trusted single-job lifetime. Caller serializes every operation with ONE mux.
 * Cancellation revokes delivery, never proves physical native work returned.
 * Keep this fixed storage alive; stale tickets cannot retire a newer job. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum {PET_PREFETCH_EMPTY,PET_PREFETCH_QUEUED,PET_PREFETCH_WORKING,
  PET_PREFETCH_READY,PET_PREFETCH_CONSUMING,PET_PREFETCH_DISCARDED} pet_prefetch_phase_t;
typedef struct pet_prefetch_lifetime {
  const struct pet_prefetch_lifetime *self;
  uint64_t serial,epoch;
  pet_prefetch_phase_t phase;
  bool cancelled;
} pet_prefetch_lifetime_t;
typedef struct {pet_prefetch_lifetime_t *parent;uint64_t serial;} pet_prefetch_ticket_t;
static inline bool pet_prefetch_init(pet_prefetch_lifetime_t *p){
  if(!p||p->self||p->serial||p->epoch||p->phase||p->cancelled)return false;
  p->self=p;return true;
}
static inline bool pet_prefetch_owned(const pet_prefetch_lifetime_t *p){return p&&p->self==p;}
static inline bool pet_prefetch_exact(pet_prefetch_ticket_t t){return pet_prefetch_owned(t.parent)&&t.serial&&t.parent->serial==t.serial;}
static inline bool pet_prefetch_offer(pet_prefetch_lifetime_t *p,uint64_t epoch,pet_prefetch_ticket_t *out){
  if(!pet_prefetch_owned(p)||!epoch||p->phase!=PET_PREFETCH_EMPTY||p->cancelled||p->serial==UINT64_MAX||!out||out->parent||out->serial)return false;
  uintptr_t a=(uintptr_t)p,b=(uintptr_t)out;
  if(a<=b?b-a<sizeof(*p):a-b<sizeof(*out))return false;
  p->serial++;p->epoch=epoch;p->phase=PET_PREFETCH_QUEUED;*out=(pet_prefetch_ticket_t){p,p->serial};return true;
}
/* Even cancelled queued work must be claimed and physically finished. */
static inline bool pet_prefetch_claim(pet_prefetch_ticket_t t,bool *execute){
  if(!pet_prefetch_exact(t)||!execute||t.parent->phase!=PET_PREFETCH_QUEUED)return false;
  uintptr_t a=(uintptr_t)t.parent,b=(uintptr_t)execute;
  if(a<=b?b-a<sizeof(*t.parent):a-b<sizeof(*execute))return false;
  t.parent->phase=PET_PREFETCH_WORKING;*execute=!t.parent->cancelled;return true;
}
/* Metadata first; only after all native calls and exact lease finish. */
static inline bool pet_prefetch_complete(pet_prefetch_ticket_t t,bool success){
  if(!pet_prefetch_exact(t)||t.parent->phase!=PET_PREFETCH_WORKING)return false;
  t.parent->phase=success&&!t.parent->cancelled?PET_PREFETCH_READY:PET_PREFETCH_DISCARDED;return true;
}
static inline bool pet_prefetch_take(pet_prefetch_ticket_t t,uint64_t epoch){
  if(!pet_prefetch_exact(t)||!epoch||t.parent->epoch!=epoch||t.parent->cancelled||t.parent->phase!=PET_PREFETCH_READY)return false;
  t.parent->phase=PET_PREFETCH_CONSUMING;return true;
}
/* Every decoder-output reader, including pack workers, must have returned. */
static inline bool pet_prefetch_release(pet_prefetch_ticket_t t){
  if(!pet_prefetch_exact(t)||t.parent->phase!=PET_PREFETCH_CONSUMING)return false;
  t.parent->phase=t.parent->cancelled?PET_PREFETCH_DISCARDED:PET_PREFETCH_EMPTY;
  if(!t.parent->cancelled)t.parent->epoch=0;
  return true;
}
static inline bool pet_prefetch_cancel(pet_prefetch_ticket_t t){
  if(!pet_prefetch_exact(t)||t.parent->phase==PET_PREFETCH_EMPTY)return false;
  t.parent->cancelled=true;if(t.parent->phase==PET_PREFETCH_READY)t.parent->phase=PET_PREFETCH_DISCARDED;return true;
}
static inline bool pet_prefetch_retire(pet_prefetch_lifetime_t *p){
  if(!pet_prefetch_owned(p)||(p->phase!=PET_PREFETCH_EMPTY&&p->phase!=PET_PREFETCH_DISCARDED))return false;
  p->phase=PET_PREFETCH_EMPTY;p->epoch=0;p->cancelled=false;return true;
}
_Static_assert(sizeof(pet_prefetch_lifetime_t)<=40,"bounded native single-job lifetime");
