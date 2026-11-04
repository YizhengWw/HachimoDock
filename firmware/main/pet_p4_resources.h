/* Fixed-storage resource authority. Trusted C only; never decode tokens from JSON.
 * No locks, allocation, callbacks or I/O. The caller serializes every operation
 * on one broker with the same short critical section. Zero-initialize once per
 * boot; never reset a broker while tokens from that boot may still exist.
 * NATIVE_PROBE and PROGRAM/PERIPHERAL physical leases are mutually exclusive;
 * diagnostics cannot silently double an admitted execution budget. Revocation
 * does not remove this conflict until exact physical finish. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PET_RESOURCE_OWNER_SLOTS 8U
#define PET_RESOURCE_LEASE_SLOTS 8U
#define PET_RESOURCE_OWNER_LEASE_MAX 2U
#define PET_RESOURCE_SNAPSHOT_BYTES (640U * 480U * 2U)
#define PET_RESOURCE_REALTIME_SLOTS 8U

typedef enum {
  PET_RESOURCE_HOST_VIEW,
  PET_RESOURCE_SNAPSHOT,
  PET_RESOURCE_DEBUG_BULK,
  PET_RESOURCE_ASSET_BUFFER,
  PET_RESOURCE_ASSET_PREPARE,
  PET_RESOURCE_NATIVE_PROBE,
  /* Reserved, with zero capacity. These are not available package services. */
  PET_RESOURCE_PROGRAM,
  PET_RESOURCE_PERIPHERAL,
  /* One admitted background filesystem job, not an audio file reader. */
  PET_RESOURCE_STORAGE_WORK,
  /* Trusted renderer only: decode an already pinned, immutable PSRAM source.
   * Shares the single physical worker with ASSET_PREPARE; never permits I/O. */
  PET_RESOURCE_CACHED_VIDEO,
  PET_RESOURCE_KIND_COUNT
} pet_resource_kind_t;
#define PET_RESOURCE_MASK(kind) (UINT32_C(1) << (kind))
#define PET_RESOURCE_DEBUG_MASK (PET_RESOURCE_MASK(PET_RESOURCE_SNAPSHOT) | PET_RESOURCE_MASK(PET_RESOURCE_DEBUG_BULK))
#define PET_RESOURCE_ALL_MASK ((UINT32_C(1) << PET_RESOURCE_KIND_COUNT) - 1U)
/* Program/native snapshot work belongs to the same realtime exclusion even
 * while its production quota is zero. Never rely only on delayed audio flags
 * when a future reviewed capability opens those quotas. */
#define PET_RESOURCE_BACKGROUND_MASK (PET_RESOURCE_DEBUG_MASK | PET_RESOURCE_MASK(PET_RESOURCE_ASSET_PREPARE) | PET_RESOURCE_MASK(PET_RESOURCE_NATIVE_PROBE) | PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK) | PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM) | PET_RESOURCE_MASK(PET_RESOURCE_PERIPHERAL))

typedef enum {
  PET_RESOURCE_OK,
  PET_RESOURCE_INVALID,
  PET_RESOURCE_STALE,
  PET_RESOURCE_BUSY,
  PET_RESOURCE_FORBIDDEN,
  PET_RESOURCE_PROTECTED,
  PET_RESOURCE_UNAVAILABLE,
  PET_RESOURCE_EXHAUSTED
} pet_resource_result_t;
typedef enum {
  PET_RESOURCE_FINISH_STALE,
  PET_RESOURCE_FINISH_CURRENT,
  PET_RESOURCE_FINISH_REVOKED
} pet_resource_finish_t;

struct pet_resource_broker;
typedef struct {
  const struct pet_resource_broker *broker;
  uint64_t generation;
  uint8_t slot;
} pet_resource_owner_t;
typedef struct {
  pet_resource_owner_t owner;
  uint64_t serial;
  uint32_t units;
  pet_resource_kind_t kind;
  uint8_t slot;
} pet_resource_lease_t;
/* Dedicated trusted realtime reservation slots cannot be consumed by package
 * owners/leases. A reservation never waits for or releases background I/O. */
typedef struct {
  const struct pet_resource_broker *broker;
  uint64_t serial;
  uint8_t slot;
} pet_resource_realtime_t;
typedef struct {
  uint16_t leases;
  uint32_t units;
} pet_resource_limit_t;

/* Exposed for static storage, not for caller mutation after initialization. */
typedef struct pet_resource_broker {
  uint64_t next_generation, next_serial;
  uint64_t next_realtime_serial;
  uint64_t realtime[PET_RESOURCE_REALTIME_SLOTS];
  bool realtime_video_ready[PET_RESOURCE_REALTIME_SLOTS];
  uint32_t protected_mask;
  struct {
    uint64_t generation;
    uint32_t allowed_mask;
    uint8_t leases;
    bool live;
  } owners[PET_RESOURCE_OWNER_SLOTS];
  struct {
    pet_resource_lease_t ticket;
    bool occupied;
  } leases[PET_RESOURCE_LEASE_SLOTS];
} pet_resource_broker_t;

typedef struct {
  uint8_t live_owners, revoked_owners;
  uint8_t realtime_reservations;
  uint16_t leases[PET_RESOURCE_KIND_COUNT];
  uint32_t units[PET_RESOURCE_KIND_COUNT], protected_mask;
} pet_resource_usage_t;

pet_resource_limit_t pet_resources_limit(pet_resource_kind_t kind);
/* Outputs must initially be zero. Failure leaves them and occupancy unchanged. */
pet_resource_result_t pet_resources_owner_open(pet_resource_broker_t *broker,
  uint32_t allowed_mask, pet_resource_owner_t *out);
bool pet_resources_owner_current(const pet_resource_broker_t *broker, pet_resource_owner_t owner);
/* Revocation denies future work/delivery, but retains every physical lease. */
bool pet_resources_owner_revoke(pet_resource_broker_t *broker, pet_resource_owner_t owner);
pet_resource_result_t pet_resources_try_acquire(pet_resource_broker_t *broker,
  pet_resource_owner_t owner, pet_resource_kind_t kind, uint32_t units,
  pet_resource_lease_t *out);
bool pet_resources_current(const pet_resource_broker_t *broker, pet_resource_lease_t lease);
/* Call only after the physical work/buffer is finished. The return value says
 * whether completion still has authority; stale finishes release nothing. */
pet_resource_finish_t pet_resources_finish(pet_resource_broker_t *broker, pet_resource_lease_t lease);
/* Admission protection does not free or implicitly revoke an existing lease. */
bool pet_resources_protect(pet_resource_broker_t *broker, uint32_t mask, bool blocked);
void pet_resources_usage(const pet_resource_broker_t *broker, pet_resource_usage_t *out);
/* Atomic with ordinary acquisition on this same broker. Blocks new background
 * work and revokes background job owners, retaining every physical lease until
 * its real worker finishes. Already-entered filesystem calls cannot be aborted. */
pet_resource_result_t pet_resources_realtime_begin(pet_resource_broker_t *broker,
  pet_resource_realtime_t *out);
bool pet_resources_realtime_current(const pet_resource_broker_t *broker, pet_resource_realtime_t ticket);
/* A live audio actor may admit cached animation only after initialization.
 * Every outstanding realtime actor must opt in; a new actor blocks/revokes
 * video atomically before its first codec/allocation operation. */
bool pet_resources_realtime_video_ready(pet_resource_broker_t *broker,
  pet_resource_realtime_t ticket, bool ready);
/* Only after actual realtime work ends; stale/duplicate completion does nothing. */
bool pet_resources_realtime_finish(pet_resource_broker_t *broker, pet_resource_realtime_t ticket);
