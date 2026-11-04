#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Single physical storage job. The command lane grants permission to drop its
 * lock explicitly; merely configuring callbacks never grants that permission.
 * All begin/finish/enter/leave calls require the command lock (or boot before
 * concurrency). Only the owning task may run the out-of-lock prepare phase.
 * Lane and scope storage are native-private, fixed-address and initially zero.
 * A live scope cannot be copied, moved or rebound, even on the same task. */
typedef void (*pet_storage_lock_fn)(void *context);
typedef uintptr_t (*pet_storage_owner_fn)(void *context);
typedef enum {
  PET_STORAGE_CAPTURE=0, PET_STORAGE_PREPARE, PET_STORAGE_DURABLE,
  PET_STORAGE_PUBLISHED, PET_STORAGE_CLEANUP
} pet_storage_phase_t;
struct pet_storage_scope;
typedef struct {
  pet_storage_lock_fn unlock,lock;
  pet_storage_owner_fn owner;
  void *context;
  uintptr_t command_owner;
  uint64_t command_serial,job_serial;
  const struct pet_storage_scope *scope_owner;
  bool command_active,occupied;
} pet_storage_lane_t;
typedef struct pet_storage_scope {
  pet_storage_lane_t *lane;
  uintptr_t owner;
  uint64_t command_serial,job_serial;
  pet_storage_phase_t phase;
  bool active,may_unlock,unlocked;
} pet_storage_scope_t;

bool pet_storage_lane_configure(pet_storage_lane_t *lane,
  pet_storage_lock_fn unlock,pet_storage_lock_fn lock,
  pet_storage_owner_fn owner,void *context);
bool pet_storage_lane_enter(pet_storage_lane_t *lane);
bool pet_storage_lane_leave(pet_storage_lane_t *lane);
/* Fresh zero or fully finished scope only; failure preserves lane and scope.
 * A configured owner callback returning zero grants no work, unlike boot's
 * deliberately unconfigured synchronous lane. Bindings are native addresses,
 * not serializable tokens; copied scope values never own a physical job. */
bool pet_storage_scope_begin(pet_storage_lane_t *lane,pet_storage_scope_t *scope);
bool pet_storage_scope_unlock(pet_storage_scope_t *scope);
bool pet_storage_scope_lock(pet_storage_scope_t *scope);
bool pet_storage_scope_advance(pet_storage_scope_t *scope,pet_storage_phase_t phase);
/* Cancellation is allowed before a catalog commit only. Once durable, the
 * caller must publish/reconcile; never remove committed files as rollback. */
bool pet_storage_scope_cancel_allowed(const pet_storage_scope_t *scope);
bool pet_storage_scope_finish(pet_storage_scope_t *scope);
