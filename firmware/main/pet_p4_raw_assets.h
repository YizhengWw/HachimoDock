/*
 * [Input] P4 MJPEG/H.264 logical paths and transaction chunks for the dedicated raw flash partition.
 * [Output] Bounded append-only slot-1 writes, commit-last index headers, and direct reads.
 * [Pos] High-throughput appearance storage backend for ESP32-P4.
 * [Sync] If this file changes, update protocol.md and firmware/.folder.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_BUILTIN_APPEARANCE_SLOT 0
#define PET_P4_RAW_APPEARANCE_SLOT 1

bool pet_p4_raw_assets_available(void);
unsigned long long pet_p4_raw_assets_capacity_bytes(void);
bool pet_p4_raw_assets_slot_available(int slot);
unsigned long long pet_p4_raw_assets_slot_capacity_bytes(int slot);
bool pet_p4_raw_assets_supports_path(const char *logical_path);
/* Track C component media kinds (p4-samples-v1 / p4-clip-v1 / p4-big-image-v1):
 * runtime/samples/<id>.pcm, runtime/clips/<id>.h264, runtime/big/<id>.p4t. They travel
 * inside the component install transaction and are stored by pet_p4_component_media in
 * SPIFFS, never in the appearance partitions; this classifier keeps the two asset
 * directions from ever sharing a path. */
bool pet_p4_raw_assets_component_media_path(const char *logical_path);
bool pet_p4_raw_assets_prepare(
  unsigned long long total_bytes,
  char *error,
  size_t error_size
);
bool pet_p4_raw_assets_write_chunk(
  const char *logical_path,
  const char *index,
  const unsigned char *data,
  size_t data_len,
  char *error,
  size_t error_size
);
bool pet_p4_raw_assets_finish_file(
  const char *logical_path,
  unsigned long long expected_size,
  const char *expected_checksum,
  char *error,
  size_t error_size
);
/* False with may_have_changed=false is a pre-write rejection/OOM. The flag is
 * set BEFORE the partition driver write; any later error must not be replayed
 * or cleaned up as an untouched header. Original bool API remains for callers
 * that do not make transaction-outcome decisions. */
bool pet_p4_raw_assets_commit_observed(const char *pack_id,char *error,
  size_t error_size,bool *may_have_changed);
bool pet_p4_raw_assets_commit(
  const char *pack_id,
  char *error,
  size_t error_size
);
bool pet_p4_raw_assets_invalidate(char *error, size_t error_size);
void pet_p4_raw_assets_reset_transfer(void);
bool pet_p4_raw_assets_committed_pack_id(char *out, size_t out_size);
bool pet_p4_raw_assets_committed_pack_id_for_slot(
  int slot,
  char *out,
  size_t out_size
);
bool pet_p4_raw_assets_read_all(
  const char *logical_path,
  unsigned char *out,
  size_t expected_size
);
bool pet_p4_raw_assets_read_all_from_slot(
  int slot,
  const char *logical_path,
  unsigned char *out,
  size_t expected_size
);
/* Trusted native reader descriptor. Resolve metadata on the caller's existing
 * storage lane, then a low-priority preparer can read fixed bounded ranges
 * without consulting mutable global cache/active-slot state. No wire command. */
/* Read-only committed header proof; caller owns the existing storage lane.
 * No video bytes are read. Mutating/ambiguous/wrong-pack entries fail closed. */
bool pet_p4_raw_assets_stat_for_slot(int slot,const char *pack,const char *path,
  size_t expected_size,uint32_t *size,unsigned long long *checksum);
bool pet_p4_raw_assets_locate(int slot,const char *logical_path,size_t expected_size,uint32_t *offset);
bool pet_p4_raw_assets_read_range(int slot,uint32_t base,size_t total,size_t offset,unsigned char *out,size_t len);

#ifdef __cplusplus
}
#endif
