#include "pet_p4_transport_audit.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

uint32_t pet_transport_audit_prepare(pet_transport_audit_t *audit) {
  if (!audit) return 0;
  audit->armed = false;
  audit->prepared = false;
  if (audit->session == UINT32_MAX) return 0;
  uint32_t session = audit->session + 1;
  memset(audit, 0, sizeof(*audit));
  audit->session = session;
  audit->prepared = true;
  return session;
}
bool pet_transport_audit_arm(pet_transport_audit_t *audit, uint32_t session) {
  if (!audit || !session || session != audit->session || !audit->prepared || audit->armed)
    return false;
  audit->remaining = PET_TRANSPORT_AUDIT_BUDGET;
  audit->prepared = false;
  audit->armed = true;
  return true;
}
bool pet_transport_audit_active(const pet_transport_audit_t *audit) {
  return audit && audit->armed && audit->remaining;
}
void pet_transport_audit_disarm(pet_transport_audit_t *audit) {
  if (audit) { audit->armed = false; audit->prepared = false; }
}
bool pet_transport_audit_fingerprint(const char *body, size_t bytes, uint32_t *out) {
  if (!body || !out || bytes > PET_TRANSPORT_AUDIT_HASH_MAX_BYTES) return false;
  uint32_t hash = UINT32_C(2166136261);
  for (size_t i = 0; i < bytes; i++) { hash ^= (uint8_t)body[i]; hash *= UINT32_C(16777619); }
  *out = hash;
  return true;
}
void pet_transport_audit_record(pet_transport_audit_t *audit, const pet_transport_audit_record_t *record) {
  if (!pet_transport_audit_active(audit) || !record) return;
  pet_transport_audit_record_t next = *record;
  next.sequence = ++audit->seen;
  audit->records[audit->head] = next;
  audit->head = (uint8_t)((audit->head + 1) % PET_TRANSPORT_AUDIT_RECORDS);
  if (audit->count < PET_TRANSPORT_AUDIT_RECORDS) audit->count++;
  if (!--audit->remaining) audit->armed = false;
}
void pet_transport_audit_close(pet_transport_audit_t *audit, pet_transport_audit_snapshot_t *out) {
  if (!audit || !out) return;
  pet_transport_audit_disarm(audit);
  memset(out, 0, sizeof(*out));
  out->session = audit->session; out->seen = audit->seen; out->count = audit->count;
  out->unused_budget = audit->remaining;
  out->overwritten = audit->seen > audit->count ? (uint16_t)(audit->seen - audit->count) : 0;
  for (unsigned i = 0; i < audit->count; i++) {
    unsigned index = (audit->head + PET_TRANSPORT_AUDIT_RECORDS - audit->count + i) % PET_TRANSPORT_AUDIT_RECORDS;
    out->records[i] = audit->records[index];
  }
}
static bool append(char *out, size_t capacity, size_t *used, const char *format, ...) {
  if (*used >= capacity) return false;
  va_list args; va_start(args, format);
  int n = vsnprintf(out + *used, capacity - *used, format, args);
  va_end(args);
  if (n < 0 || (size_t)n >= capacity - *used) return false;
  *used += (size_t)n;
  return true;
}
static size_t encode(const pet_transport_audit_snapshot_t *snapshot,
    char *out, size_t capacity, bool reply) {
  if (!out || !capacity) return 0;
  out[0] = '\0';
  if (!snapshot || snapshot->count > PET_TRANSPORT_AUDIT_RECORDS) return 0;
  size_t used = 0;
  if (!append(out, capacity, &used,
      "{\"topic\":\"debug/transport_audit_%sresult\",\"payload\":{\"ok\":true,\"active\":false,\"session\":%" PRIu32
      ",\"seen\":%u,\"overwritten\":%u,\"unusedBudget\":%u,\"records\":[",
      reply ? "reply_" : "", snapshot->session, snapshot->seen, snapshot->overwritten, snapshot->unused_budget)) goto failed;
  for (unsigned i = 0; i < snapshot->count; i++) {
    const pet_transport_audit_record_t *r = &snapshot->records[i];
    if (!append(out, capacity, &used,
        "%s{\"seq\":%" PRIu32 ",\"route\":%u,\"bodyBytes\":%" PRIu32 ",\"fnv\":\"%08" PRIx32
        "\",\"hashAvailable\":%s,\"fnvAfter\":\"%08" PRIx32 "\",\"hashAfterAvailable\":%s,\"driverObserved\":%s,\"bodyAccepted\":%" PRIu32
        ",\"newlineAccepted\":%" PRIu32 ",\"bodyLast\":%" PRId32 ",\"newlineLast\":%" PRId32
        ",\"bodyDrain\":%" PRId32 ",\"newlineDrain\":%" PRId32 ",\"invalidReturn\":%s}",
        i ? "," : "", r->sequence, (unsigned)r->route, r->body_bytes, r->fingerprint,
        r->fingerprint_available ? "true" : "false", r->fingerprint_after,
        r->fingerprint_after_available ? "true" : "false",r->driver_observed ? "true" : "false",
        r->body_accepted, r->newline_accepted, r->body_last_return, r->newline_last_return,
        r->body_drain, r->newline_drain, r->invalid_return ? "true" : "false")) goto failed;
  }
  if (!append(out, capacity, &used, "]}}")) goto failed;
  return used;
failed:
  out[0] = '\0';
  return 0;
}
size_t pet_transport_audit_encode(const pet_transport_audit_snapshot_t *snapshot,char *out,size_t capacity){
  return encode(snapshot,out,capacity,false);
}
size_t pet_transport_audit_encode_reply(const pet_transport_audit_snapshot_t *snapshot,char *out,size_t capacity){
  return encode(snapshot,out,capacity,true);
}
