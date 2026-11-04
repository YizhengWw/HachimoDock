#include "pet_p4_transport_audit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  pet_transport_audit_t audit = {0};
  pet_transport_audit_snapshot_t snapshot;
  pet_transport_audit_record_t record = {.route = PET_TRANSPORT_AUDIT_UART,
    .body_bytes = 3, .driver_observed = true, .fingerprint_available = true};
  assert(!pet_transport_audit_active(&audit));
  pet_transport_audit_record(&audit, &record); assert(!audit.seen);
  uint32_t first = pet_transport_audit_prepare(&audit);
  assert(first == 1 && !pet_transport_audit_active(&audit));
  pet_transport_audit_record(&audit, &record); assert(!audit.seen);
  assert(!pet_transport_audit_arm(&audit, 2));
  assert(pet_transport_audit_arm(&audit, first));
  assert(!pet_transport_audit_arm(&audit, first));
  for (unsigned i = 0; i < 100; i++) {
    record.body_bytes = i;
    pet_transport_audit_record(&audit, &record);
  }
  assert(!pet_transport_audit_active(&audit) && audit.seen == 64);
  pet_transport_audit_close(&audit, &snapshot);
  assert(snapshot.count == 16 && snapshot.overwritten == 48 && snapshot.unused_budget == 0);
  for (unsigned i = 0; i < 16; i++) {
    assert(snapshot.records[i].sequence == 49 + i);
    assert(snapshot.records[i].body_bytes == 48 + i);
  }
  uint32_t next = pet_transport_audit_prepare(&audit);
  assert(next == 2 && !audit.seen && !audit.count);
  assert(!pet_transport_audit_arm(&audit, first));
  pet_transport_audit_close(&audit, &snapshot);
  assert(!pet_transport_audit_arm(&audit, next));
  assert(snapshot.count == 0 && snapshot.unused_budget == 0);
  next = pet_transport_audit_prepare(&audit);
  assert(pet_transport_audit_arm(&audit, next));
  pet_transport_audit_close(&audit, &snapshot);
  assert(!pet_transport_audit_arm(&audit, next) && snapshot.unused_budget == 64);
  audit.session = UINT32_MAX;
  assert(!pet_transport_audit_prepare(&audit) && !pet_transport_audit_arm(&audit, UINT32_MAX));

  uint32_t hash = 0;
  assert(pet_transport_audit_fingerprint("hello", 5, &hash) && hash == UINT32_C(0x4f9f2cab));
  assert(pet_transport_audit_fingerprint("", 0, &hash) && hash == UINT32_C(2166136261));
  hash = 123;
  assert(!pet_transport_audit_fingerprint("", PET_TRANSPORT_AUDIT_HASH_MAX_BYTES + 1, &hash));
  assert(hash == 123); /* Does not read out of bounds or silently report a hash. */
  assert(!pet_transport_audit_fingerprint(NULL, 0, &hash));
  char encoded[PET_TRANSPORT_AUDIT_REPLY_BYTES], tiny[10];
  memset(&snapshot, 0, sizeof(snapshot)); snapshot.count = 16; snapshot.session = UINT32_MAX;
  snapshot.seen = UINT16_MAX; snapshot.overwritten = UINT16_MAX; snapshot.unused_budget = UINT16_MAX;
  for (unsigned i = 0; i < 16; i++) snapshot.records[i] = (pet_transport_audit_record_t){
    .sequence = UINT32_MAX, .body_bytes = UINT32_MAX, .fingerprint = UINT32_MAX,
    .body_accepted = UINT32_MAX, .newline_accepted = UINT32_MAX,
    .body_last_return = INT32_MIN, .newline_last_return = INT32_MIN,
    .body_drain = INT32_MIN, .newline_drain = INT32_MIN,
    .route = PET_TRANSPORT_AUDIT_NATIVE_USB, .invalid_return = false,
    .fingerprint_available = false, .driver_observed = false};
  size_t bytes = pet_transport_audit_encode(&snapshot, encoded, sizeof(encoded));
  assert(bytes && bytes == strlen(encoded) && bytes < sizeof(encoded));
  assert(!pet_transport_audit_encode(&snapshot, tiny, sizeof(tiny)) && !tiny[0]);
  puts(encoded);
  return 0;
}
