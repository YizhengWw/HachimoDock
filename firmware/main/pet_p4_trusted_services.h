/* Trusted, read-only service boundary. NOT a package-level peripheral grant.
 * Only the trusted runtime constructs authorities/backends; wire programs cannot
 * supply pointers, owners, GPIO pins, bus addresses, DMA channels or callbacks.
 * The caller serializes one authority and checks its owning lifecycle again
 * before delivering a returned snapshot to a view/program. No heap or retries.
 */
#pragma once
#include "pet_p4_resources.h"
#include <stdbool.h>
#include <stdint.h>

#define PET_TRUSTED_SERVICE_VERSION 1U
#define PET_TRUSTED_SERVICE_READ_LIMIT 16U
#define PET_TRUSTED_SERVICE_MAX_US UINT64_C(2000)
typedef enum {
  PET_TRUSTED_CLOCK_MONOTONIC = 1,
  PET_TRUSTED_INPUT_JOYSTICK = 2
} pet_trusted_service_id_t;
#define PET_TRUSTED_CLOCK_MASK UINT32_C(1)
#define PET_TRUSTED_JOYSTICK_MASK UINT32_C(2)
#define PET_TRUSTED_SERVICE_ALL_MASK (PET_TRUSTED_CLOCK_MASK | PET_TRUSTED_JOYSTICK_MASK)

typedef struct {
  pet_trusted_service_id_t id;
  const char *name;
  uint16_t version;
  uint32_t grant_mask;
} pet_trusted_service_descriptor_t;
/* Compiled providers, not a promise that the resource quota or a grant is open.
 * Unknown IDs return NULL. There are deliberately no raw bus/pin services. */
const pet_trusted_service_descriptor_t *pet_trusted_service_descriptor(uint32_t id);

typedef struct {
  bool ready;
  int32_t center_x, center_y, current_x, current_y;
  int32_t minimum_x, maximum_x, minimum_y, maximum_y;
} pet_trusted_joystick_t;
typedef struct {
  uint32_t service;
  uint16_t version;
  uint64_t sampled_us;
  union {
    uint64_t monotonic_us;
    pet_trusted_joystick_t joystick;
  } value;
} pet_trusted_service_snapshot_t;
typedef struct {
  /* Fixed private native instance, never a serialized/wire authority. */
  const void *instance;
  pet_resource_owner_t owner;
  uint32_t granted_mask;
  uint16_t remaining_reads;
  bool cancelled, in_flight;
} pet_trusted_service_authority_t;
typedef enum {
  PET_TRUSTED_OK, PET_TRUSTED_INVALID, PET_TRUSTED_UNKNOWN_SERVICE,
  PET_TRUSTED_DENIED, PET_TRUSTED_CALL_LIMIT, PET_TRUSTED_UNAVAILABLE,
  PET_TRUSTED_BUSY, PET_TRUSTED_CANCELLED, PET_TRUSTED_AUDIO_PROTECTED,
  PET_TRUSTED_TIMEOUT, PET_TRUSTED_CLOCK_ERROR, PET_TRUSTED_NOT_READY,
  PET_TRUSTED_PROVIDER_ERROR
} pet_trusted_service_result_t;
typedef enum {
  PET_TRUSTED_SAMPLE_READY, PET_TRUSTED_SAMPLE_NOT_READY,
  PET_TRUSTED_SAMPLE_ERROR
} pet_trusted_sample_result_t;
typedef struct {
  pet_trusted_service_result_t result;
  pet_resource_result_t resource_result;
  pet_resource_finish_t finish_result;
  uint64_t elapsed_us;
  bool provider_started, provider_completed, snapshot_published;
} pet_trusted_service_diagnostic_t;

/* These are approved native functions, not package-supplied implementations.
 * now_us/input snapshots must be nonblocking, with no driver init/sample/event.
 * acquire/current/finish serialize the SAME device resource broker. They must
 * not hold that lock while calling providers. audio_busy only observes flags.
 * Callbacks cannot mutate the authority, backend or caller output objects;
 * same-authority reentry is rejected without a callback or budget consumption.
 * The opaque context's internal storage is the native backend's responsibility.
 * A timeout is observed after a native call returns, not hard preemption: the
 * exact physical lease is never released while a provider is still executing.
 */
typedef struct {
  void *context;
  int64_t (*now_us)(void *context);
  pet_trusted_sample_result_t (*joystick)(void *context, pet_trusted_joystick_t *out);
  bool (*audio_busy)(void *context);
  pet_resource_result_t (*acquire)(void *context, pet_resource_owner_t owner,
    pet_resource_kind_t kind, uint32_t units, pet_resource_lease_t *out);
  bool (*current)(void *context, pet_resource_lease_t lease);
  void (*revoke)(void *context, pet_resource_owner_t owner);
  pet_resource_finish_t (*finish)(void *context, pet_resource_lease_t lease);
} pet_trusted_service_backend_t;

/* Trusted invocation boundary only. out MUST be zero-initialized before its
 * first init. Init rejects any already initialized/cancelled/in-flight object;
 * Do not copy or move a live authority. Copies cannot consume a read budget,
 * invoke a provider or revoke the original owner. Do not clear or reuse one to reset an event's budget. A fresh event requires
 * runtime admission and a fresh private authority, not a retry's re-init. */
bool pet_trusted_service_authority_init(pet_trusted_service_authority_t *out,
  pet_resource_owner_t owner, uint32_t granted_mask, uint16_t read_limit);
/* authority/backend/out/diagnostic must be distinct nonoverlapping objects.
 * Overlap is rejected before any write, including diagnostic initialization.
 * The caller serializes the authority; in_flight is a reentry guard, NOT a lock.
 * Output remains byte-for-byte unchanged on every non-OK result. Reads are
 * observational: no submission/unknown-side-effect outcome and no auto retry.
 * Cancellation/audio/clock failure/overrun revokes this owner permanently;
 * releasing occupancy happens only after the approved provider has returned.
 */
pet_trusted_service_result_t pet_trusted_service_read(
  pet_trusted_service_authority_t *authority, uint32_t service,
  const pet_trusted_service_backend_t *backend,
  pet_trusted_service_snapshot_t *out, pet_trusted_service_diagnostic_t *diagnostic);
