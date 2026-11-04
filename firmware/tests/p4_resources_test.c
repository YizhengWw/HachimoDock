#include "pet_p4_resources.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static pet_resource_owner_t open_owner(pet_resource_broker_t *broker, uint32_t mask) {
  pet_resource_owner_t owner = {0};
  assert(pet_resources_owner_open(broker, mask, &owner) == PET_RESOURCE_OK);
  return owner;
}
static pet_resource_lease_t acquire(pet_resource_broker_t *broker, pet_resource_owner_t owner,
  pet_resource_kind_t kind, uint32_t units) {
  pet_resource_lease_t lease = {0};
  assert(pet_resources_try_acquire(broker, owner, kind, units, &lease) == PET_RESOURCE_OK);
  return lease;
}

static void revoked_physical_occupancy_and_exact_completion(void) {
  pet_resource_broker_t broker = {0};
  pet_resource_owner_t old = open_owner(&broker, PET_RESOURCE_DEBUG_MASK);
  pet_resource_lease_t snapshot = acquire(&broker, old, PET_RESOURCE_SNAPSHOT, PET_RESOURCE_SNAPSHOT_BYTES);
  pet_resource_lease_t bulk = acquire(&broker, old, PET_RESOURCE_DEBUG_BULK, 1);
  assert(pet_resources_owner_revoke(&broker, old));
  assert(!pet_resources_owner_revoke(&broker, old));
  assert(!pet_resources_current(&broker, snapshot));
  pet_resource_usage_t used;pet_resources_usage(&broker, &used);
  assert(used.live_owners == 0 && used.revoked_owners == 1);
  assert(used.leases[PET_RESOURCE_SNAPSHOT] == 1 && used.units[PET_RESOURCE_SNAPSHOT] == PET_RESOURCE_SNAPSHOT_BYTES);
  pet_resource_owner_t next = open_owner(&broker, PET_RESOURCE_DEBUG_MASK);
  pet_resource_lease_t waiting = {0};
  assert(pet_resources_try_acquire(&broker, next, PET_RESOURCE_SNAPSHOT, 1, &waiting) == PET_RESOURCE_BUSY);
  assert(pet_resources_try_acquire(&broker, old, PET_RESOURCE_DEBUG_BULK, 1, &waiting) == PET_RESOURCE_STALE);
  assert(pet_resources_finish(&broker, snapshot) == PET_RESOURCE_FINISH_REVOKED);
  pet_resource_lease_t replacement = acquire(&broker, next, PET_RESOURCE_SNAPSHOT, 153600);
  assert(replacement.slot == snapshot.slot && replacement.serial != snapshot.serial);
  assert(pet_resources_finish(&broker, snapshot) == PET_RESOURCE_FINISH_STALE);
  assert(!pet_resources_owner_revoke(&broker, old));
  assert(pet_resources_current(&broker, replacement));
  assert(pet_resources_try_acquire(&broker, next, PET_RESOURCE_DEBUG_BULK, 1, &waiting) == PET_RESOURCE_BUSY);
  assert(pet_resources_finish(&broker, bulk) == PET_RESOURCE_FINISH_REVOKED);
  assert(pet_resources_finish(&broker, replacement) == PET_RESOURCE_FINISH_CURRENT);
  assert(pet_resources_owner_revoke(&broker, next));
  pet_resources_usage(&broker, &used);
  assert(used.live_owners == 0 && used.revoked_owners == 0);
  for (unsigned i = 0; i < PET_RESOURCE_KIND_COUNT; i++) assert(!used.leases[i] && !used.units[i]);
}

static void typed_quotas_and_unavailable_services(void) {
  pet_resource_broker_t broker = {0};
  pet_resource_owner_t owner = open_owner(&broker, PET_RESOURCE_ALL_MASK);
  pet_resource_lease_t empty = {0};
  assert(pet_resources_try_acquire(&broker, owner, PET_RESOURCE_PROGRAM, 1, &empty) == PET_RESOURCE_UNAVAILABLE);
  assert(pet_resources_try_acquire(&broker, owner, PET_RESOURCE_PERIPHERAL, 1, &empty) == PET_RESOURCE_UNAVAILABLE);
  assert(pet_resources_try_acquire(&broker, owner, PET_RESOURCE_SNAPSHOT, 0, &empty) == PET_RESOURCE_INVALID);
  assert(pet_resources_try_acquire(&broker, owner, PET_RESOURCE_SNAPSHOT, PET_RESOURCE_SNAPSHOT_BYTES + 1, &empty) == PET_RESOURCE_INVALID);
  assert(pet_resources_try_acquire(&broker, owner, (pet_resource_kind_t)-1, 1, &empty) == PET_RESOURCE_INVALID);
  assert(pet_resources_try_acquire(&broker, owner, PET_RESOURCE_KIND_COUNT, 1, &empty) == PET_RESOURCE_INVALID);
  pet_resource_lease_t first = acquire(&broker, owner, PET_RESOURCE_SNAPSHOT, 1);
  /* The snapshot count is one, even if its byte quota is not exhausted. */
  assert(pet_resources_try_acquire(&broker, owner, PET_RESOURCE_SNAPSHOT, 1, &empty) == PET_RESOURCE_BUSY);
  pet_resource_lease_t second = acquire(&broker, owner, PET_RESOURCE_DEBUG_BULK, 1);
  /* A trusted owner still cannot exceed its two-lease budget. */
  assert(pet_resources_try_acquire(&broker, owner, PET_RESOURCE_HOST_VIEW, 1, &empty) == PET_RESOURCE_BUSY);
  pet_resource_owner_t host = open_owner(&broker, PET_RESOURCE_MASK(PET_RESOURCE_HOST_VIEW));
  assert(pet_resources_try_acquire(&broker, host, PET_RESOURCE_SNAPSHOT, 1, &empty) == PET_RESOURCE_FORBIDDEN);
  pet_resource_lease_t view = acquire(&broker, host, PET_RESOURCE_HOST_VIEW, 1);
  assert(pet_resources_finish(&broker, first) == PET_RESOURCE_FINISH_CURRENT);
  assert(pet_resources_finish(&broker, second) == PET_RESOURCE_FINISH_CURRENT);
  assert(pet_resources_finish(&broker, view) == PET_RESOURCE_FINISH_CURRENT);
}

static void bounded_owner_slots_and_generation_reuse(void) {
  pet_resource_broker_t broker = {0};pet_resource_owner_t owners[PET_RESOURCE_OWNER_SLOTS];
  for (unsigned i = 0; i < PET_RESOURCE_OWNER_SLOTS; i++) owners[i] = open_owner(&broker, PET_RESOURCE_DEBUG_MASK);
  pet_resource_owner_t failed = {0};
  assert(pet_resources_owner_open(&broker, PET_RESOURCE_DEBUG_MASK, &failed) == PET_RESOURCE_BUSY);
  pet_resource_lease_t held = acquire(&broker, owners[0], PET_RESOURCE_DEBUG_BULK, 1);
  assert(pet_resources_owner_revoke(&broker, owners[0]));
  assert(pet_resources_owner_open(&broker, PET_RESOURCE_DEBUG_MASK, &failed) == PET_RESOURCE_BUSY);
  assert(pet_resources_finish(&broker, held) == PET_RESOURCE_FINISH_REVOKED);
  pet_resource_owner_t replacement = open_owner(&broker, PET_RESOURCE_DEBUG_MASK);
  assert(replacement.slot == owners[0].slot && replacement.generation != owners[0].generation);
  assert(!pet_resources_owner_revoke(&broker, owners[0]));
  assert(pet_resources_owner_current(&broker, replacement));
  assert(!pet_resources_current(&broker, held));
  assert(pet_resources_finish(&broker, held) == PET_RESOURCE_FINISH_STALE);
}

static void protection_is_admission_not_early_release(void) {
  pet_resource_broker_t broker = {0};pet_resource_lease_t empty = {0};
  pet_resource_owner_t owner = open_owner(&broker, PET_RESOURCE_DEBUG_MASK);
  pet_resource_lease_t held = acquire(&broker, owner, PET_RESOURCE_SNAPSHOT, 32);
  assert(pet_resources_protect(&broker, PET_RESOURCE_DEBUG_MASK, true));
  assert(pet_resources_current(&broker, held));
  assert(pet_resources_try_acquire(&broker, owner, PET_RESOURCE_DEBUG_BULK, 1, &empty) == PET_RESOURCE_PROTECTED);
  pet_resource_usage_t used;pet_resources_usage(&broker, &used);
  assert(used.leases[PET_RESOURCE_SNAPSHOT] == 1 && used.units[PET_RESOURCE_SNAPSHOT] == 32);
  assert(pet_resources_protect(&broker, PET_RESOURCE_DEBUG_MASK, false));
  pet_resource_lease_t bulk = acquire(&broker, owner, PET_RESOURCE_DEBUG_BULK, 1);
  assert(pet_resources_finish(&broker, held) == PET_RESOURCE_FINISH_CURRENT);
  assert(pet_resources_finish(&broker, bulk) == PET_RESOURCE_FINISH_CURRENT);
}

static void fail_closed_invalid_foreign_and_exhausted_tokens(void) {
  pet_resource_broker_t broker = {0}, foreign = {0};pet_resource_owner_t empty = {0};
  assert(pet_resources_owner_open(&broker, 0, &empty) == PET_RESOURCE_INVALID);
  assert(pet_resources_owner_open(&broker, PET_RESOURCE_ALL_MASK + 1, &empty) == PET_RESOURCE_INVALID);
  pet_resource_owner_t owner = open_owner(&broker, PET_RESOURCE_DEBUG_MASK);
  pet_resource_owner_t other = open_owner(&foreign, PET_RESOURCE_DEBUG_MASK);
  pet_resource_lease_t lease = acquire(&broker, owner, PET_RESOURCE_DEBUG_BULK, 1), invalid = {0};
  assert(pet_resources_try_acquire(&broker, other, PET_RESOURCE_SNAPSHOT, 1, &invalid) == PET_RESOURCE_STALE);
  assert(!pet_resources_owner_revoke(&foreign, owner));
  assert(pet_resources_finish(&foreign, lease) == PET_RESOURCE_FINISH_STALE);
  pet_resource_lease_t forged = lease;forged.kind = PET_RESOURCE_SNAPSHOT;
  assert(pet_resources_finish(&broker, forged) == PET_RESOURCE_FINISH_STALE);
  forged = lease;forged.units++;
  assert(pet_resources_finish(&broker, forged) == PET_RESOURCE_FINISH_STALE);
  forged = lease;forged.slot = PET_RESOURCE_LEASE_SLOTS;
  assert(pet_resources_finish(&broker, forged) == PET_RESOURCE_FINISH_STALE);
  assert(pet_resources_current(&broker, lease));
  broker.next_serial = UINT64_MAX;
  assert(pet_resources_try_acquire(&broker, owner, PET_RESOURCE_SNAPSHOT, 1, &invalid) == PET_RESOURCE_EXHAUSTED);
  assert(!invalid.serial);
  assert(pet_resources_finish(&broker, lease) == PET_RESOURCE_FINISH_CURRENT);
  broker.next_generation = UINT64_MAX;
  assert(pet_resources_owner_open(&broker, PET_RESOURCE_DEBUG_MASK, &empty) == PET_RESOURCE_EXHAUSTED);
  assert(!empty.generation);
  assert(pet_resources_finish(&broker, (pet_resource_lease_t){0}) == PET_RESOURCE_FINISH_STALE);
  assert(!pet_resources_protect(&broker, PET_RESOURCE_ALL_MASK + 1, true));
}

int main(void) {
  revoked_physical_occupancy_and_exact_completion();
  typed_quotas_and_unavailable_services();
  bounded_owner_slots_and_generation_reuse();
  protection_is_admission_not_early_release();
  fail_closed_invalid_foreign_and_exhausted_tokens();
  puts("PASS resource core: typed quotas, held cancellation, stale/foreign tokens, overflow");
  return 0;
}
