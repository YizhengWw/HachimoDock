#include "pet_p4_storage_lane.h"
#include <string.h>

static bool overlap(const void *a,size_t an,const void *b,size_t bn){
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
  return x<=y?y-x<an:x-y<bn;
}
static uintptr_t current_owner(const pet_storage_lane_t *lane){
  return lane->owner?lane->owner(lane->context):0;
}
static bool current_scope(const pet_storage_scope_t *scope){
  return scope&&scope->active&&scope->lane&&scope->lane->occupied
    &&scope->lane->scope_owner==scope
    &&scope->job_serial==scope->lane->job_serial
    &&scope->owner==current_owner(scope->lane);
}
bool pet_storage_lane_configure(pet_storage_lane_t *lane,
  pet_storage_lock_fn unlock,pet_storage_lock_fn lock,
  pet_storage_owner_fn owner,void *context){
  if(!lane||lane->occupied||lane->scope_owner||lane->command_active||!unlock||!lock||!owner)return false;
  lane->unlock=unlock;lane->lock=lock;lane->owner=owner;lane->context=context;return true;
}
bool pet_storage_lane_enter(pet_storage_lane_t *lane){
  if(!lane||!lane->owner||lane->occupied||lane->scope_owner||lane->command_active
      ||lane->command_serial==UINT64_MAX)return false;
  uintptr_t owner=current_owner(lane);if(!owner)return false;
  lane->command_owner=owner;lane->command_serial++;lane->command_active=true;return true;
}
bool pet_storage_lane_leave(pet_storage_lane_t *lane){
  if(!lane||!lane->command_active||lane->occupied||lane->scope_owner
      ||lane->command_owner!=current_owner(lane))return false;
  lane->command_active=false;lane->command_owner=0;return true;
}
bool pet_storage_scope_begin(pet_storage_lane_t *lane,pet_storage_scope_t *scope){
  if(!lane||!scope||overlap(lane,sizeof(*lane),scope,sizeof(*scope))
      ||scope->active||lane->occupied||lane->scope_owner||lane->job_serial==UINT64_MAX)return false;
  uintptr_t owner=current_owner(lane);
  if(lane->owner&&!owner)return false;
  /* A direct native fallback cannot start a mutation beside another task's
   * active protocol command, even when that command has not started I/O yet. */
  if(lane->command_active&&lane->command_owner!=owner)return false;
  memset(scope,0,sizeof(*scope));scope->lane=lane;scope->owner=owner;
  scope->command_serial=lane->command_serial;scope->job_serial=++lane->job_serial;
  scope->may_unlock=lane->command_active&&scope->owner&&scope->owner==lane->command_owner;
  scope->active=true;lane->scope_owner=scope;lane->occupied=true;return true;
}
bool pet_storage_scope_unlock(pet_storage_scope_t *scope){
  if(!current_scope(scope)||scope->unlocked)return false;
  if(scope->may_unlock){
    pet_storage_lane_t *lane=scope->lane;
    if(!lane->command_active||lane->command_serial!=scope->command_serial
        ||lane->command_owner!=scope->owner)return false;
    scope->unlocked=true;lane->unlock(lane->context);
  }
  return true;
}
bool pet_storage_scope_lock(pet_storage_scope_t *scope){
  if(!current_scope(scope))return false;
  if(scope->unlocked){scope->lane->lock(scope->lane->context);scope->unlocked=false;}
  return true;
}
bool pet_storage_scope_advance(pet_storage_scope_t *scope,pet_storage_phase_t phase){
  if(!current_scope(scope)||phase!=(pet_storage_phase_t)(scope->phase+1)
      ||phase>PET_STORAGE_CLEANUP)return false;
  if(phase==PET_STORAGE_PUBLISHED&&scope->unlocked)return false;
  scope->phase=phase;return true;
}
bool pet_storage_scope_cancel_allowed(const pet_storage_scope_t *scope){
  return current_scope(scope)&&scope->phase<PET_STORAGE_DURABLE;
}
bool pet_storage_scope_finish(pet_storage_scope_t *scope){
  if(!pet_storage_scope_lock(scope))return false;
  /* A durable, unpublished result cannot release its slot. This is a bug in
   * the caller, not a transient error that a second command may overwrite. */
  if(scope->phase==PET_STORAGE_DURABLE)return false;
  scope->lane->scope_owner=NULL;scope->lane->occupied=false;scope->active=false;return true;
}
