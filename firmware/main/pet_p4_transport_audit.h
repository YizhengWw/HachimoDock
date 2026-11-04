/* Opt-in, RAM-only transport evidence. Caller serializes this fixed state with
 * the existing transport mutex; no driver calls, locks, retries or persistence.
 * Hashes are correlation fingerprints, NOT cryptographic integrity/privacy.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PET_TRANSPORT_AUDIT_RECORDS 16U
#define PET_TRANSPORT_AUDIT_BUDGET 64U
#define PET_TRANSPORT_AUDIT_HASH_MAX_BYTES 32768U
#define PET_TRANSPORT_AUDIT_REPLY_BYTES 8192U
#define PET_TRANSPORT_AUDIT_NOT_OBSERVED INT32_MIN
typedef enum {
  PET_TRANSPORT_AUDIT_UART = 1,
  PET_TRANSPORT_AUDIT_USB_SERIAL_JTAG = 2,
  PET_TRANSPORT_AUDIT_NATIVE_USB = 3
} pet_transport_audit_route_t;
typedef struct {
  size_t accepted;
  int32_t last_return, drain;
  bool invalid_return;
} pet_transport_write_observation_t;
typedef struct {
  uint32_t sequence, body_bytes, fingerprint;
  uint32_t fingerprint_after;
  uint32_t body_accepted, newline_accepted;
  int32_t body_last_return, newline_last_return, body_drain, newline_drain;
  pet_transport_audit_route_t route;
  bool fingerprint_available, fingerprint_after_available, driver_observed, invalid_return;
} pet_transport_audit_record_t;
typedef struct {
  uint32_t session;
  uint16_t seen, overwritten, unused_budget;
  uint8_t count;
  pet_transport_audit_record_t records[PET_TRANSPORT_AUDIT_RECORDS];
} pet_transport_audit_snapshot_t;
typedef struct {
  uint32_t session;
  uint16_t seen, remaining;
  uint8_t head, count;
  bool armed, prepared;
  pet_transport_audit_record_t records[PET_TRANSPORT_AUDIT_RECORDS];
} pet_transport_audit_t;

/* Zero initialization is disabled. Prepare explicitly discards previous RAM
 * evidence but stays disarmed; send the start ACK BEFORE arming this session.
 * Session exhaustion fails closed. Late arms cannot revive a newer capture. */
uint32_t pet_transport_audit_prepare(pet_transport_audit_t *audit);
bool pet_transport_audit_arm(pet_transport_audit_t *audit, uint32_t session);
bool pet_transport_audit_active(const pet_transport_audit_t *audit);
/* Cancel observation without clearing evidence; never automatically resumes. */
void pet_transport_audit_disarm(pet_transport_audit_t *audit);
/* Only called for opted-in ordinary drained frames. No audio/media/logs.
 * Oversized lines are not hashed; absence is explicit, never a fake checksum. */
bool pet_transport_audit_fingerprint(const char *body, size_t bytes, uint32_t *out);
void pet_transport_audit_record(pet_transport_audit_t *audit,
  const pet_transport_audit_record_t *record);
/* Stops first, then copies oldest-to-newest while caller holds transport lock.
 * Encoding/sending the independent snapshot must happen AFTER releasing lock. */
void pet_transport_audit_close(pet_transport_audit_t *audit, pet_transport_audit_snapshot_t *out);
/* Fixed metadata only. Returns byte count excluding NUL, or 0 and empty output
 * on insufficient space. Caller must not send an incomplete JSON response. */
size_t pet_transport_audit_encode(const pet_transport_audit_snapshot_t *snapshot,
  char *out, size_t capacity);
/* Separate last explicitly requested result's TX metadata, never recursively
 * observed or appended to the ordinary audit ring. Same fixed bounds. */
size_t pet_transport_audit_encode_reply(const pet_transport_audit_snapshot_t *snapshot,
  char *out, size_t capacity);
