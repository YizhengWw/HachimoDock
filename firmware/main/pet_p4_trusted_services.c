#include "pet_p4_trusted_services.h"
#include <string.h>

static const pet_trusted_service_descriptor_t services[] = {
  {PET_TRUSTED_CLOCK_MONOTONIC, "clock.monotonic", PET_TRUSTED_SERVICE_VERSION, PET_TRUSTED_CLOCK_MASK},
  {PET_TRUSTED_INPUT_JOYSTICK, "input.joystick", PET_TRUSTED_SERVICE_VERSION, PET_TRUSTED_JOYSTICK_MASK}
};
const pet_trusted_service_descriptor_t *pet_trusted_service_descriptor(uint32_t id) {
  for (unsigned i = 0; i < sizeof(services) / sizeof(services[0]); i++)
    if ((uint32_t)services[i].id == id) return &services[i];
  return NULL;
}
bool pet_trusted_service_authority_init(pet_trusted_service_authority_t *out,
    pet_resource_owner_t owner, uint32_t granted_mask, uint16_t read_limit) {
  if (!out || !owner.broker || !owner.generation || owner.slot >= PET_RESOURCE_OWNER_SLOTS
      || !granted_mask || (granted_mask & ~PET_TRUSTED_SERVICE_ALL_MASK)
      || !read_limit || read_limit > PET_TRUSTED_SERVICE_READ_LIMIT) return false;
  /* This API accepts initialized C storage ({0}), never uninitialized bytes. */
  if (out->instance || out->owner.broker || out->owner.generation || out->owner.slot || out->granted_mask
      || out->remaining_reads || out->cancelled || out->in_flight) return false;
  *out = (pet_trusted_service_authority_t){.instance = out, .owner = owner,
    .granted_mask = granted_mask, .remaining_reads = read_limit};
  return true;
}
static pet_trusted_service_result_t record(pet_trusted_service_diagnostic_t *out,
    pet_trusted_service_diagnostic_t *d, pet_trusted_service_result_t result) {
  d->result = result;
  if (out) *out = *d;
  return result;
}
static bool overlaps(const void *a, size_t a_size, const void *b, size_t b_size) {
  if (!a || !b) return false;
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
  /* Subtraction avoids an overflowing end address near UINTPTR_MAX. */
  return x <= y ? y - x < a_size : x - y < b_size;
}
static bool aliases(pet_trusted_service_authority_t *a,
    const pet_trusted_service_backend_t *b, pet_trusted_service_snapshot_t *out,
    pet_trusted_service_diagnostic_t *d) {
  return overlaps(a, sizeof(*a), b, sizeof(*b))
    || overlaps(a, sizeof(*a), out, sizeof(*out))
    || overlaps(a, sizeof(*a), d, sizeof(*d))
    || overlaps(b, sizeof(*b), out, sizeof(*out))
    || overlaps(b, sizeof(*b), d, sizeof(*d))
    || overlaps(out, sizeof(*out), d, sizeof(*d));
}
static bool valid_backend(const pet_trusted_service_backend_t *b) {
  return b && b->now_us && b->joystick && b->audio_busy && b->acquire
    && b->current && b->revoke && b->finish;
}
typedef struct {
  pet_trusted_service_authority_t *authority;
  pet_trusted_service_backend_t backend;
  pet_resource_owner_t owner;
  uint32_t granted_mask;
  uint16_t remaining_reads;
} invocation_t;
static bool same_owner(pet_resource_owner_t a, pet_resource_owner_t b) {
  return a.broker == b.broker && a.generation == b.generation && a.slot == b.slot;
}
static bool authority_current(const invocation_t *call) {
  const pet_trusted_service_authority_t *a = call->authority;
  return a->instance == a && a->in_flight && !a->cancelled && same_owner(a->owner, call->owner)
    && a->granted_mask == call->granted_mask && a->remaining_reads == call->remaining_reads;
}
static void cancel(invocation_t *call) {
  call->authority->cancelled = true;
  call->backend.revoke(call->backend.context, call->owner);
  /* A reentrant callback cannot undo the terminal state. */
  call->authority->cancelled = true;
}
static pet_trusted_service_result_t check(invocation_t *call, pet_resource_lease_t lease) {
  const pet_trusted_service_backend_t *b = &call->backend;
  if (!authority_current(call)) { cancel(call); return PET_TRUSTED_CANCELLED; }
  bool busy = b->audio_busy(b->context);
  if (!authority_current(call)) { cancel(call); return PET_TRUSTED_CANCELLED; }
  if (busy) {
    cancel(call);
    return PET_TRUSTED_AUDIO_PROTECTED;
  }
  bool current = b->current(b->context, lease);
  if (!current || !authority_current(call)) {
    cancel(call);
    return PET_TRUSTED_CANCELLED;
  }
  return PET_TRUSTED_OK;
}
static pet_trusted_service_result_t admission_result(pet_resource_result_t result) {
  switch (result) {
    case PET_RESOURCE_STALE: return PET_TRUSTED_CANCELLED;
    case PET_RESOURCE_FORBIDDEN: return PET_TRUSTED_DENIED;
    case PET_RESOURCE_PROTECTED: return PET_TRUSTED_AUDIO_PROTECTED;
    case PET_RESOURCE_UNAVAILABLE: return PET_TRUSTED_UNAVAILABLE;
    case PET_RESOURCE_BUSY: case PET_RESOURCE_EXHAUSTED: return PET_TRUSTED_BUSY;
    default: return PET_TRUSTED_INVALID;
  }
}
pet_trusted_service_result_t pet_trusted_service_read(pet_trusted_service_authority_t *a,
    uint32_t service, const pet_trusted_service_backend_t *b,
    pet_trusted_service_snapshot_t *out, pet_trusted_service_diagnostic_t *diagnostic) {
  /* No external store before overlap rejection, even for invalid arguments. */
  if (aliases(a, b, out, diagnostic)) return PET_TRUSTED_INVALID;
  /* Reject moved/copied native authority before ANY output/callback. In
   * particular a copy's timeout must not revoke the original live owner. */
  if (a && a->instance && a->instance != a) return PET_TRUSTED_INVALID;
  pet_trusted_service_diagnostic_t local = {.resource_result = PET_RESOURCE_INVALID,
    .finish_result = PET_RESOURCE_FINISH_STALE};
  pet_trusted_service_diagnostic_t *d = &local;
  if (!a || a->instance != a || !out || !valid_backend(b) || !a->owner.broker || !a->owner.generation
      || a->owner.slot >= PET_RESOURCE_OWNER_SLOTS || !a->granted_mask
      || (a->granted_mask & ~PET_TRUSTED_SERVICE_ALL_MASK)
      || a->remaining_reads > PET_TRUSTED_SERVICE_READ_LIMIT)
    return record(diagnostic, d, PET_TRUSTED_INVALID);
  const pet_trusted_service_descriptor_t *descriptor = pet_trusted_service_descriptor(service);
  if (!descriptor) return record(diagnostic, d, PET_TRUSTED_UNKNOWN_SERVICE);
  if (!(a->granted_mask & descriptor->grant_mask)) return record(diagnostic, d, PET_TRUSTED_DENIED);
  if (a->cancelled) return record(diagnostic, d, PET_TRUSTED_CANCELLED);
  if (a->in_flight) return record(diagnostic, d, PET_TRUSTED_BUSY);
  if (!a->remaining_reads) return record(diagnostic, d, PET_TRUSTED_CALL_LIMIT);
  a->in_flight = true;
  a->remaining_reads--;
  invocation_t call = {a, *b, a->owner, a->granted_mask, a->remaining_reads};
  b = &call.backend;
  pet_trusted_service_result_t result = PET_TRUSTED_OK;
  pet_resource_lease_t lease = {0};
  pet_trusted_service_snapshot_t next = {0};
  bool busy = b->audio_busy(b->context);
  if (!authority_current(&call)) { cancel(&call); result = PET_TRUSTED_CANCELLED; goto done; }
  if (busy) { cancel(&call); result = PET_TRUSTED_AUDIO_PROTECTED; goto done; }
  d->resource_result = b->acquire(b->context, call.owner, PET_RESOURCE_PERIPHERAL, 1, &lease);
  if (d->resource_result != PET_RESOURCE_OK) {
    result = admission_result(d->resource_result);
    if (!authority_current(&call)) result = PET_TRUSTED_CANCELLED;
    if (result == PET_TRUSTED_CANCELLED || result == PET_TRUSTED_AUDIO_PROTECTED) cancel(&call);
    goto done;
  }
  if (!same_owner(lease.owner, call.owner) || !lease.serial || lease.slot >= PET_RESOURCE_LEASE_SLOTS
      || lease.kind != PET_RESOURCE_PERIPHERAL || lease.units != 1) {
    /* Never finish an unrelated or malformed lease returned by a broken native
     * backend. Revoke only this invocation; unknown occupancy stays retained. */
    cancel(&call); result = PET_TRUSTED_INVALID; goto done;
  }
  result = check(&call, lease);
  int64_t started = -1, ended = -1;
  if (result == PET_TRUSTED_OK) {
    started = b->now_us(b->context);
    if (started < 0) result = PET_TRUSTED_CLOCK_ERROR;
    else result = check(&call, lease);
  }
  if (result == PET_TRUSTED_OK) {
    next.service = service;
    next.version = descriptor->version;
    next.sampled_us = (uint64_t)started;
    d->provider_started = true;
    if (service == PET_TRUSTED_CLOCK_MONOTONIC) next.value.monotonic_us = (uint64_t)started;
    else {
      pet_trusted_sample_result_t sample = b->joystick(b->context, &next.value.joystick);
      if (sample == PET_TRUSTED_SAMPLE_NOT_READY || (sample == PET_TRUSTED_SAMPLE_READY && !next.value.joystick.ready))
        result = PET_TRUSTED_NOT_READY;
      else if (sample != PET_TRUSTED_SAMPLE_READY) result = PET_TRUSTED_PROVIDER_ERROR;
    }
    d->provider_completed = true;
    pet_trusted_service_result_t current = check(&call, lease);
    if (current != PET_TRUSTED_OK) result = current;
    else {
      ended = b->now_us(b->context);
      if (ended < started) result = PET_TRUSTED_CLOCK_ERROR;
      else {
        d->elapsed_us = (uint64_t)(ended - started);
        if (d->elapsed_us > PET_TRUSTED_SERVICE_MAX_US) result = PET_TRUSTED_TIMEOUT;
      }
      current = check(&call, lease);
      if (current != PET_TRUSTED_OK) result = current;
    }
  }
  if (result == PET_TRUSTED_TIMEOUT || result == PET_TRUSTED_CLOCK_ERROR) cancel(&call);
  /* The provider has physically returned before this exact ticket is released. */
  d->finish_result = b->finish(b->context, lease);
  if (d->finish_result != PET_RESOURCE_FINISH_CURRENT || !authority_current(&call)) {
    if (result == PET_TRUSTED_OK || result == PET_TRUSTED_NOT_READY || result == PET_TRUSTED_PROVIDER_ERROR)
      result = PET_TRUSTED_CANCELLED;
    cancel(&call);
  }
done:
  a->in_flight = false;
  if (result == PET_TRUSTED_OK) { *out = next; d->snapshot_published = true; }
  return record(diagnostic, d, result);
}
