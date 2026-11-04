/*
 * [Input] Base64-decoded install chunks for runtime/samples|clips|big files, the parsed
 *         pet_cext_runtime_t of the staged/active component and catalog slot/generation.
 * [Output] Per-slot SPIFFS files (/spiffs/p4m<slot>-<gen>-<kind><index>.bin), PSRAM copies of
 *          the active component's samples and clip, and bounded tile reads for big images.
 * [Pos] Track C component media store (device only). Budgets: samples <= 256 KiB, clip <= 512 KiB,
 *       big images <= 1 MiB per package; staging arena and caches live in PSRAM. Three fixed banks and eight generation leases
 *       retain borrowed sample/clip memory across unload; audio releases use bounded
 *       lock-free retirement. Store mutex protects IO/publication; no new task.
 * [Sync] If this file changes, update firmware/protocol.md and firmware/.folder.md.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pet_p4_component_ext.h"
#define PET_CMEDIA_STAGE_BYTES (PET_SAMPLE_BUDGET_BYTES + 4u * PET_CEXT_SAMPLE_HEADER + PET_CEXT_CLIP_MAX_BYTES + PET_CEXT_BIG_BUDGET_BYTES)
#define PET_CMEDIA_DEVICE_BUDGET_BYTES (4u * 1024u * 1024u) /* all installed component media together */
/* ---- install staging (protocol worker, storage lane held; never an audio lock) ---- */
void pet_cmedia_stage_reset(void);
/* Appends one decoded chunk (index must be sequential per file). */
bool pet_cmedia_stage_chunk(pet_cext_media_kind_t kind, const char *id, uint32_t index, const uint8_t *data, size_t len, char *error, size_t error_size);
/* Every declared sample/clip/big image is staged with a valid header and inside budget. */
bool pet_cmedia_stage_matches(const pet_cext_runtime_t *r, char *error, size_t error_size);
/* Writes the staged files for slot/generation (tmp + rename). */
bool pet_cmedia_persist(size_t slot, uint8_t generation, const pet_cext_runtime_t *r);
void pet_cmedia_remove(size_t slot);
/* ---- active component (no model/audio lock held during file IO) ---- */
/* ONE pending transaction, caller-owned fixed ticket. Prepare performs bounded
 * IO but leaves visible generation and existing borrows unchanged. Commit of
 * the reserved ticket does no IO/allocation; abort releases only prepared data.
 * Catalog/storage owner serializes the entire prepare -> commit/abort interval.
 * Direct load/persist/remove/unload cannot replace a reserved transaction. */
typedef struct { uint64_t serial; uint32_t generation; } pet_cmedia_ticket_t;
bool pet_cmedia_prepare(size_t slot, uint8_t generation, const pet_cext_runtime_t *r, pet_cmedia_ticket_t *ticket);
bool pet_cmedia_commit(pet_cmedia_ticket_t *ticket);
bool pet_cmedia_abort(pet_cmedia_ticket_t *ticket);
bool pet_cmedia_load(size_t slot, uint8_t generation, const pet_cext_runtime_t *r);
void pet_cmedia_unload(void);
/* Three banks bound memory during swap; eight tokens bound simultaneous users.
 * Pin output must be fresh (do not overwrite a live lease). A successful pin owns
 * exactly one token; copy of a token does not acquire ownership. Release once,
 * after the final sample/decoder access. Failed pins produce a zeroed output.
 * Unload hides active media; buffers remain alive until their last unpin.
 * Load is transactional and fails preserving current media when banks are busy.
 * Legacy raw-pointer APIs fail closed (NULL); callers must migrate to pins. */
#define PET_CMEDIA_BANKS 3u
#define PET_CMEDIA_LEASES 8u
typedef struct { uint64_t token; uint32_t generation; const int16_t *pcm; uint32_t frames; } pet_cmedia_sample_lease_t;
typedef struct { uint64_t token; uint32_t generation; const uint8_t *stream; size_t bytes; pet_cext_clip_t meta; } pet_cmedia_clip_lease_t;
bool pet_cmedia_sample_pin(unsigned index, pet_cmedia_sample_lease_t *out);
bool pet_cmedia_clip_pin(unsigned index, pet_cmedia_clip_lease_t *out);
bool pet_cmedia_unpin(uint64_t token);
/* Audio-safe bounded release: eight lock-free 32-bit CAS attempts, no mutex,
 * free or IO. Store calls collect under its mutex; retirement uses fixed banks. */
bool pet_cmedia_unpin_deferred(uint64_t token);
void pet_cmedia_collect(void);
/* expected must match the snapshot generation; zero never admits a snapshot. */
bool pet_cmedia_sample_pin_at(unsigned index, uint32_t expected, pet_cmedia_sample_lease_t *out);
bool pet_cmedia_clip_pin_at(unsigned index, uint32_t expected, pet_cmedia_clip_lease_t *out);
const int16_t *pet_cmedia_sample_pcm(unsigned index, uint32_t *frames);
const uint8_t *pet_cmedia_clip_stream(unsigned index, size_t *bytes, pet_cext_clip_t *meta);
/* Reads one tile (<= 64 KiB) from the big image file into the caller buffer. */
bool pet_cmedia_big_tile(unsigned big, unsigned tile, uint8_t *out, size_t capacity, size_t *bytes);
bool pet_cmedia_big_meta(unsigned big, pet_cext_big_t *meta);
bool pet_cmedia_big_meta_at(unsigned big, uint32_t expected, pet_cext_big_t *meta);
bool pet_cmedia_big_tile_at(unsigned big, unsigned tile, uint32_t expected, uint8_t *out, size_t capacity, size_t *bytes);
/* Visibility generation; refuses new loads at UINT32_MAX. Terminal unload
 * returns invalid generation zero and never reuses a previous generation. */
uint32_t pet_cmedia_generation(void);

/* Main-lane diagnostics; taking this snapshot also collects deferred releases.
 * No wire endpoint is introduced here. Alloc bytes describe store capacities,
 * not physical decoder, renderer, staging or global heap usage. */
typedef struct {
  uint32_t generation, tokens_issued, tokens_reclaimed, deferred_reclaimed;
  uint32_t sample_alloc_bytes, clip_alloc_bytes, table_alloc_bytes;
  unsigned live_banks, pinned_banks, live_tokens, bank_pins;
} pet_cmedia_stats_t;
bool pet_cmedia_stats(pet_cmedia_stats_t *out);

/* Frame/main lane: zero-timeout probes, no mutex creation or implicit reaping.
 * Busy/uninitialized means retry next frame. expected=0 always rejects.
 * collect_try is the only probe that reaps (fixed eight leases/three banks).
 * Tile try additionally requires nonblocking native STORAGE_WORK admission.
 * Worker transaction APIs above retain their original blocking semantics. */
bool pet_cmedia_generation_try(uint32_t expected);
bool pet_cmedia_collect_try(void);
bool pet_cmedia_sample_pin_try_at(unsigned index, uint32_t expected, pet_cmedia_sample_lease_t *out);
bool pet_cmedia_clip_pin_try_at(unsigned index, uint32_t expected, pet_cmedia_clip_lease_t *out);
bool pet_cmedia_big_meta_try_at(unsigned big, uint32_t expected, pet_cext_big_t *meta);
bool pet_cmedia_big_tile_try_at(unsigned big, unsigned tile, uint32_t expected, uint8_t *out, size_t capacity, size_t *bytes);
