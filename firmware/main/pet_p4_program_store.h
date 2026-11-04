/* Portable private program snapshots. NOT an enabled package/storage command.
 * No filesystem, heap, callbacks, authority, replay journal or migration here.
 * Callers own/serialize immutable inputs and output storage for the whole call.
 * CRC detects accidental damage; it does not authenticate a snapshot or grant
 * access to a component, peripheral, user account or program capability.
 */
#pragma once
#include "pet_p4_program_runtime.h"

#define PET_PROGRAM_STORE_HEADER 32U
/* Historical P4S1 constants stay stable for golden files and old callers. */
#define PET_PROGRAM_STORE_CELLS 64U
#define PET_PROGRAM_STORE_BYTES (PET_PROGRAM_STORE_HEADER + PET_PROGRAM_STORE_CELLS * 4U + 8U * 4U)
#define PET_PROGRAM_STORE_TEXT_LENGTH (9U + PET_PROGRAM_STORE_BYTES * 2U + 2U)
#define PET_PROGRAM_STORE_TEXT_CAPACITY (PET_PROGRAM_STORE_TEXT_LENGTH + 1U)

#define PET_PROGRAM_STORE_V2_CELLS 256U
#define PET_PROGRAM_STORE_V2_BYTES (PET_PROGRAM_STORE_HEADER + PET_PROGRAM_STORE_V2_CELLS * 4U + 8U * 4U)
#if PET_P4_PROGRAM_V2
#define PET_PROGRAM_STORE_MAX_BYTES PET_PROGRAM_STORE_V2_BYTES
#else
#define PET_PROGRAM_STORE_MAX_BYTES PET_PROGRAM_STORE_BYTES
#endif
#define PET_PROGRAM_STORE_MAX_TEXT_CAPACITY (12U + PET_PROGRAM_STORE_MAX_BYTES * 2U)
/* Canonical encoding keeps <=64-cell saves byte-identical P4S1. Wider saves
 * use P4S2/version 2, fixed 1088 bytes, LE16 cells at 20..21, outputs=8 at 22,
 * integer bytes=4 at 23. 24..27 remain zero. 256 signed cells start at 32;
 * 8 outputs start at 1056. CRC still skips 28..31. Unused cells MUST be zero.
 * Text wrapper uses the exact {"p4s2":"<2176 lowercase hex digits>"} shape.
 * V1-only builds refuse P4S2. Decoders never infer layout migration.
 */
size_t pet_program_store_size(const pet_program_snapshot_t *snapshot);

/* P4S1 v1, exact 320-byte format, integers little endian (never struct casts):
 *  0..3  "P4S1"                4 version=1       5 flags=0
 *  6..7  header bytes=32        8..11 total bytes=320
 * 12..15 nonzero state schema  16..19 revision (all u32 values)
 * 20     cells (0..64)         21 outputs=8      22 integer bytes=4
 * 23..27 reserved=0           28..31 IEEE CRC32
 * 32..287  64 signed i32 values, unused indices >=cells MUST be zero
 * 288..319  8 signed i32 outputs. All used values/outputs accept the entire
 *           INT32_MIN..INT32_MAX range, encoded as two's-complement bits.
 * CRC covers bytes 0..27 then 32..319, excluding its own four bytes.
 * Layout equality means EXACT schema + cell count. A trusted schema ID must
 * change whenever cell meanings change; equal counts alone do not imply a
 * valid migration. Future versions/flags/reserved bits are rejected.
 * No generation, input queue, receipt/outbox or effect can be restored here.
 * UINT32_MAX is a valid exhausted terminal revision: preserve and restore it,
 * never wrap/reset it. Further runtime events must report revision overflow
 * without changing the committed values or outputs.
 */
typedef enum {
  PET_PROGRAM_STORE_OK,
  PET_PROGRAM_STORE_INVALID_ARGUMENT,
  PET_PROGRAM_STORE_BAD_LENGTH,
  PET_PROGRAM_STORE_BAD_HEADER,
  PET_PROGRAM_STORE_BAD_CHECKSUM,
  PET_PROGRAM_STORE_BAD_STATE,
  PET_PROGRAM_STORE_LAYOUT_MISMATCH,
  PET_PROGRAM_STORE_BAD_ENCODING
} pet_program_store_result_t;

/* Both encode and decode leave ALL output bytes unchanged on every failure.
 * Null, address-range overflow, or overlapping output/input are invalid.
 * Success writes exactly pet_program_store_size(snapshot) bytes; bytes beyond that output remain intact.
 * Decode accepts exact length only and copies to a complete native snapshot.
 * Expected layout comes from the trusted validated image, never from the save
 * being decoded. A missing/invalid/mismatched save is NOT permission to reset.
 */
pet_program_store_result_t pet_program_store_encode(uint8_t *out, size_t capacity,
  const pet_program_snapshot_t *snapshot);
pet_program_store_result_t pet_program_store_decode(pet_program_snapshot_t *out,
  uint32_t expected_schema, pet_program_cells_t expected_cells, const uint8_t *bytes, size_t length);

/* Trusted app-store adapter boundary: a single canonical JSON member
 * {"p4s1":"<640 lowercase hex digits>"}, 651 characters + terminating NUL.
 * V2 builds reserve MAX_TEXT_CAPACITY; the same private A/B store owns durability.
 * Encoder capacity includes the NUL; decoder length excludes it. Exact shape,
 * lower-case hex and length are required; no arbitrary JSON parser is used.
 * Aliasing/failure guarantees are the same as the binary codec.
 *
 * The eventual host adapter must use a collision-free trusted key distinct
 * from legacy component JSON saves; do not overwrite a widget's existing key.
 * It must preserve BUSY/UNAVAILABLE/INVALID load outcomes, never turn them into
 * NEW/EMPTY, and publish a seed only after successful decode and validation.
 * Save update/enqueue success is NOT a durable acknowledgement. Existing A/B
 * transaction/flush policy owns atomic durability, cancellation and unknown
 * outcomes. This codec never calls update/forget, auto-resets, migrates, retries
 * or replays a job; installing/admitting a program remains disabled elsewhere.
 */
pet_program_store_result_t pet_program_store_encode_text(char *out, size_t capacity,
  const pet_program_snapshot_t *snapshot);
pet_program_store_result_t pet_program_store_decode_text(pet_program_snapshot_t *out,
  uint32_t expected_schema, pet_program_cells_t expected_cells, const char *text, size_t length);
