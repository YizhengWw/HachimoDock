#include "pet_p4_resources.h"
#include "pet_p4_component_features.h"
#include <string.h>
/* Aggregate PROGRAM fuel units: the v1 total, or the p4-component-program-v2 total. */
#if PET_P4_PROGRAM_V2 && !PET_P4_PUBLIC_PROGRAM_PROFILE
#define PET_PROGRAM_FUEL_UNITS 16384
#else
#define PET_PROGRAM_FUEL_UNITS 4096
#endif

static bool valid_kind(pet_resource_kind_t kind) {
  return kind >= PET_RESOURCE_HOST_VIEW && kind < PET_RESOURCE_KIND_COUNT;
}

pet_resource_limit_t pet_resources_limit(pet_resource_kind_t kind) {
  static const pet_resource_limit_t limits[PET_RESOURCE_KIND_COUNT] = {
    [PET_RESOURCE_HOST_VIEW]={1,1},
    [PET_RESOURCE_SNAPSHOT]={1,PET_RESOURCE_SNAPSHOT_BYTES},
    [PET_RESOURCE_DEBUG_BULK]={1,1},
    [PET_RESOURCE_ASSET_BUFFER]={2,4U*1024U*1024U},
    [PET_RESOURCE_ASSET_PREPARE]={1,1},
    [PET_RESOURCE_NATIVE_PROBE]={1,4096},
#if PET_P4_PUBLIC_PROGRAM_PROFILE
    /* Public admission never inherits the wider private VM-v2 budget. */
    [PET_RESOURCE_PROGRAM]={1,4096},
#elif PET_COMPONENT_PROGRAM_ENABLED && PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
    [PET_RESOURCE_PROGRAM]={2,PET_PROGRAM_FUEL_UNITS},
#else
    [PET_RESOURCE_PROGRAM]={1,PET_PROGRAM_FUEL_UNITS},
#endif
#else
    [PET_RESOURCE_PROGRAM]={0,0},
#endif
    [PET_RESOURCE_PERIPHERAL]={0,0},
    [PET_RESOURCE_STORAGE_WORK]={1,1},
    [PET_RESOURCE_CACHED_VIDEO]={1,1}
  };
  return valid_kind(kind) ? limits[kind] : (pet_resource_limit_t){0};
}

static bool realtime_active(const pet_resource_broker_t *broker) {
  for (unsigned i = 0; i < PET_RESOURCE_REALTIME_SLOTS; i++) if (broker->realtime[i]) return true;
  return false;
}
static bool realtime_blocks_video(const pet_resource_broker_t *broker) {
  for (unsigned i=0;i<PET_RESOURCE_REALTIME_SLOTS;i++)
    if(broker->realtime[i]&&!broker->realtime_video_ready[i])return true;
  return false;
}
pet_resource_result_t pet_resources_realtime_begin(pet_resource_broker_t *broker,
    pet_resource_realtime_t *out) {
  if (!broker || !out || out->serial) return PET_RESOURCE_INVALID;
  if (broker->next_realtime_serial == UINT64_MAX) return PET_RESOURCE_EXHAUSTED;
  unsigned slot = PET_RESOURCE_REALTIME_SLOTS;
  for (unsigned i = 0; i < PET_RESOURCE_REALTIME_SLOTS; i++)
    if (!broker->realtime[i]) { slot = i; break; }
  if (slot == PET_RESOURCE_REALTIME_SLOTS) return PET_RESOURCE_BUSY;
  broker->realtime[slot] = ++broker->next_realtime_serial;
  broker->realtime_video_ready[slot] = false;
  *out = (pet_resource_realtime_t){broker, broker->realtime[slot], (uint8_t)slot};
  for (unsigned i = 0; i < PET_RESOURCE_LEASE_SLOTS; i++) {
    if (broker->leases[i].occupied
        && ((PET_RESOURCE_BACKGROUND_MASK | PET_RESOURCE_MASK(PET_RESOURCE_CACHED_VIDEO))
          & PET_RESOURCE_MASK(broker->leases[i].ticket.kind)))
      (void)pet_resources_owner_revoke(broker, broker->leases[i].ticket.owner);
  }
  return PET_RESOURCE_OK;
}
bool pet_resources_realtime_current(const pet_resource_broker_t *broker, pet_resource_realtime_t ticket) {
  return broker && ticket.broker == broker && ticket.serial && ticket.slot < PET_RESOURCE_REALTIME_SLOTS
    && broker->realtime[ticket.slot] == ticket.serial;
}
bool pet_resources_realtime_finish(pet_resource_broker_t *broker, pet_resource_realtime_t ticket) {
  if (!pet_resources_realtime_current(broker, ticket)) return false;
  broker->realtime[ticket.slot] = 0;
  broker->realtime_video_ready[ticket.slot] = false;
  return true;
}
bool pet_resources_realtime_video_ready(pet_resource_broker_t *broker,
    pet_resource_realtime_t ticket,bool ready) {
  if(!pet_resources_realtime_current(broker,ticket))return false;
  broker->realtime_video_ready[ticket.slot]=ready;
  if(!ready)for(unsigned i=0;i<PET_RESOURCE_LEASE_SLOTS;i++)
    if(broker->leases[i].occupied&&broker->leases[i].ticket.kind==PET_RESOURCE_CACHED_VIDEO)
      (void)pet_resources_owner_revoke(broker,broker->leases[i].ticket.owner);
  return true;
}

static bool owner_matches(const pet_resource_broker_t *broker, pet_resource_owner_t owner) {
  return broker && owner.broker == broker && owner.generation
    && owner.slot < PET_RESOURCE_OWNER_SLOTS
    && broker->owners[owner.slot].generation == owner.generation;
}

bool pet_resources_owner_current(const pet_resource_broker_t *broker, pet_resource_owner_t owner) {
  return owner_matches(broker, owner) && broker->owners[owner.slot].live;
}

pet_resource_result_t pet_resources_owner_open(pet_resource_broker_t *broker,
  uint32_t allowed_mask, pet_resource_owner_t *out) {
  if (!broker || !out || out->generation || !allowed_mask || (allowed_mask & ~PET_RESOURCE_ALL_MASK))
    return PET_RESOURCE_INVALID;
  if (broker->next_generation == UINT64_MAX) return PET_RESOURCE_EXHAUSTED;
  for (uint8_t i = 0; i < PET_RESOURCE_OWNER_SLOTS; i++) {
    if (broker->owners[i].live || broker->owners[i].leases) continue;
    broker->owners[i].generation = ++broker->next_generation;
    broker->owners[i].allowed_mask = allowed_mask;
    broker->owners[i].live = true;
    *out = (pet_resource_owner_t){broker, broker->next_generation, i};
    return PET_RESOURCE_OK;
  }
  return PET_RESOURCE_BUSY;
}

bool pet_resources_owner_revoke(pet_resource_broker_t *broker, pet_resource_owner_t owner) {
  if (!pet_resources_owner_current(broker, owner)) return false;
  broker->owners[owner.slot].live = false;
  return true;
}

static bool lease_matches(const pet_resource_broker_t *broker, pet_resource_lease_t lease) {
  if (!owner_matches(broker, lease.owner) || !lease.serial || !valid_kind(lease.kind)
      || lease.slot >= PET_RESOURCE_LEASE_SLOTS || !broker->leases[lease.slot].occupied) return false;
  const pet_resource_lease_t *stored = &broker->leases[lease.slot].ticket;
  return stored->serial == lease.serial && stored->owner.generation == lease.owner.generation
    && stored->owner.slot == lease.owner.slot && stored->kind == lease.kind && stored->units == lease.units;
}

pet_resource_result_t pet_resources_try_acquire(pet_resource_broker_t *broker,
  pet_resource_owner_t owner, pet_resource_kind_t kind, uint32_t units,
  pet_resource_lease_t *out) {
  if (!broker || !out || out->serial || !valid_kind(kind) || !units) return PET_RESOURCE_INVALID;
  if (!pet_resources_owner_current(broker, owner)) return PET_RESOURCE_STALE;
  const uint32_t bit = PET_RESOURCE_MASK(kind);
  if (!(broker->owners[owner.slot].allowed_mask & bit)) return PET_RESOURCE_FORBIDDEN;
  pet_resource_limit_t limit = pet_resources_limit(kind);
  if (!limit.leases || !limit.units) return PET_RESOURCE_UNAVAILABLE;
  if (units > limit.units) return PET_RESOURCE_INVALID;
  if ((PET_RESOURCE_BACKGROUND_MASK & bit) && realtime_active(broker)) return PET_RESOURCE_PROTECTED;
  if(kind==PET_RESOURCE_CACHED_VIDEO&&realtime_blocks_video(broker))return PET_RESOURCE_PROTECTED;
  if (broker->protected_mask & bit) return PET_RESOURCE_PROTECTED;
  if (broker->next_serial == UINT64_MAX) return PET_RESOURCE_EXHAUSTED;
  if (broker->owners[owner.slot].leases >= PET_RESOURCE_OWNER_LEASE_MAX) return PET_RESOURCE_BUSY;
  unsigned used = 0, free_slot = PET_RESOURCE_LEASE_SLOTS;
  uint32_t used_units = 0;
  for (unsigned i = 0; i < PET_RESOURCE_LEASE_SLOTS; i++) {
    if (!broker->leases[i].occupied) {
      if (free_slot == PET_RESOURCE_LEASE_SLOTS) free_slot = i;
    } else {
      pet_resource_kind_t held=broker->leases[i].ticket.kind;
      if((kind==PET_RESOURCE_CACHED_VIDEO&&held==PET_RESOURCE_ASSET_PREPARE)
          ||(kind==PET_RESOURCE_ASSET_PREPARE&&held==PET_RESOURCE_CACHED_VIDEO))return PET_RESOURCE_BUSY;
      /* Fixed execution diagnostics must not add another runtime quantum to
       * an admitted component/provider. Physical occupancy, including revoked
       * work not yet returned, wins in BOTH admission directions. */
      if((kind==PET_RESOURCE_NATIVE_PROBE&&(held==PET_RESOURCE_PROGRAM||held==PET_RESOURCE_PERIPHERAL))
          ||(held==PET_RESOURCE_NATIVE_PROBE&&(kind==PET_RESOURCE_PROGRAM||kind==PET_RESOURCE_PERIPHERAL)))
        return PET_RESOURCE_BUSY;
      if(held==kind){used++;used_units+=broker->leases[i].ticket.units;}
    }
  }
  if (free_slot == PET_RESOURCE_LEASE_SLOTS || used >= limit.leases || units > limit.units - used_units)
    return PET_RESOURCE_BUSY;
  pet_resource_lease_t lease = {owner, ++broker->next_serial, units, kind, (uint8_t)free_slot};
  broker->leases[free_slot].ticket = lease;
  broker->leases[free_slot].occupied = true;
  broker->owners[owner.slot].leases++;
  *out = lease;
  return PET_RESOURCE_OK;
}

bool pet_resources_current(const pet_resource_broker_t *broker, pet_resource_lease_t lease) {
  return lease_matches(broker, lease) && pet_resources_owner_current(broker, lease.owner);
}

pet_resource_finish_t pet_resources_finish(pet_resource_broker_t *broker, pet_resource_lease_t lease) {
  if (!lease_matches(broker, lease)) return PET_RESOURCE_FINISH_STALE;
  bool current = pet_resources_owner_current(broker, lease.owner);
  broker->owners[lease.owner.slot].leases--;
  memset(&broker->leases[lease.slot], 0, sizeof(broker->leases[lease.slot]));
  return current ? PET_RESOURCE_FINISH_CURRENT : PET_RESOURCE_FINISH_REVOKED;
}

bool pet_resources_protect(pet_resource_broker_t *broker, uint32_t mask, bool blocked) {
  if (!broker || (mask & ~PET_RESOURCE_ALL_MASK)) return false;
  if (blocked) broker->protected_mask |= mask;
  else broker->protected_mask &= ~mask;
  return true;
}

void pet_resources_usage(const pet_resource_broker_t *broker, pet_resource_usage_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!broker) return;
  out->protected_mask = broker->protected_mask;
  for (unsigned i = 0; i < PET_RESOURCE_REALTIME_SLOTS; i++)
    if (broker->realtime[i]) out->realtime_reservations++;
  if (out->realtime_reservations) out->protected_mask |= PET_RESOURCE_BACKGROUND_MASK;
  if(realtime_blocks_video(broker))out->protected_mask|=PET_RESOURCE_MASK(PET_RESOURCE_CACHED_VIDEO);
  for (unsigned i = 0; i < PET_RESOURCE_OWNER_SLOTS; i++) {
    if (broker->owners[i].live) out->live_owners++;
    else if (broker->owners[i].leases) out->revoked_owners++;
  }
  for (unsigned i = 0; i < PET_RESOURCE_LEASE_SLOTS; i++) {
    if (!broker->leases[i].occupied) continue;
    const pet_resource_lease_t *lease = &broker->leases[i].ticket;
    out->leases[lease->kind]++;
    out->units[lease->kind] += lease->units;
  }
}
