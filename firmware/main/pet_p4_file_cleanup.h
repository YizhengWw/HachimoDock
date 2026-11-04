#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Exact flat-namespace targets only; never directory/prefix/wildcard deletion.
 * The caller must retain exclusive ownership of the target names until all
 * remove_at calls finish. Other filesystem actors must participate in the
 * idle_epoch callback: writers can move entries through GC, and reader error
 * handling can clear shared SPIFFS errno between readdir and its VFS wrapper. */
#define PET_FILE_CLEANUP_MAX_TARGETS 24U
#define PET_FILE_CLEANUP_PATH_BYTES 48U
#define PET_FILE_CLEANUP_MAX_ENTRIES 2048U
typedef struct { char path[PET_FILE_CLEANUP_PATH_BYTES]; } pet_file_cleanup_target_t;
typedef struct {
  bool (*owner_current)(void *context);
  bool (*idle_epoch)(void *context, uint64_t *epoch);
  void *context;
  /* Optional existing per-path implementation, preserving its error policy. */
  bool (*remove_path)(const char *path);
} pet_file_cleanup_guard_t;
/* Stable numeric diagnostics. These describe eligibility, not a new retry or
 * error policy. UNAVAILABLE cannot distinguish active I/O, disabled observation
 * or counter exhaustion through the existing idle_epoch callback. */
typedef enum {
  PET_CLEANUP_SNAPSHOT=0, PET_CLEANUP_EMPTY=1,
  PET_CLEANUP_INVALID_ARGUMENT=2, PET_CLEANUP_INVALID_TARGET=3,
  PET_CLEANUP_OWNER_BEFORE=4, PET_CLEANUP_OBSERVER_MISSING=5,
  PET_CLEANUP_OBSERVER_BEFORE=6, PET_CLEANUP_OPEN_FAILED=7,
  PET_CLEANUP_OWNER_DURING=8, PET_CLEANUP_OBSERVER_DURING=9,
  PET_CLEANUP_EPOCH_DURING=10, PET_CLEANUP_READ_FAILED=11,
  PET_CLEANUP_ENTRY_LIMIT=12, PET_CLEANUP_NAME_UNTERMINATED=13,
  PET_CLEANUP_DUPLICATE_TARGET=14, PET_CLEANUP_CLOSE_FAILED=15,
  PET_CLEANUP_OWNER_AFTER=16, PET_CLEANUP_OBSERVER_AFTER=17,
  PET_CLEANUP_EPOCH_AFTER=18
} pet_file_cleanup_reason_t;
enum {
  PET_CLEANUP_DIAG_OPENED=1U, PET_CLEANUP_DIAG_READ_NULL=2U,
  PET_CLEANUP_DIAG_COMPLETE=4U, PET_CLEANUP_DIAG_OWNER_LOST=8U,
  PET_CLEANUP_DIAG_CLOSED_OK=16U,
  PET_CLEANUP_ACTIVITY_UNAVAILABLE=1U, PET_CLEANUP_ACTIVITY_EPOCH_CHANGED=2U
};
typedef struct {
  uint64_t epoch_before,epoch_after;
  uint32_t found;
  int32_t open_errno,read_errno,close_errno;
  uint16_t entries;
  uint8_t reason,flags,activity;
  /* 0=not sampled, 1=idle/observable, 2=unavailable. After is the latest
   * post-close or retry-admission sample. The before baseline never resets. */
  uint8_t observer_before,observer_after,targets;
  /* At most two opendir calls. A new scan discards the first bitmap, but keeps
   * these first-attempt diagnostics even if the second scan is accepted.
   * first_reason=255 means no directory open was attempted. */
  int32_t first_read_errno;
  uint16_t first_entries;
  uint8_t attempts,first_reason,first_flags;
} pet_file_cleanup_diagnostic_t;
typedef struct {
  const pet_file_cleanup_target_t *targets;
  pet_file_cleanup_guard_t guard;
  uint32_t present;
  uint8_t count;
  bool valid, snapshot_used;
  pet_file_cleanup_diagnostic_t diagnostic;
} pet_file_cleanup_plan_t;

/* Borrows immutable targets/guard context; consume synchronously, never queue.
 * A first READ_FAILED with successful close and unchanged idle epoch/owner may
 * restart once from a new DIR and empty bitmap. Only that independent complete
 * scan can be accepted; no delete is retried. Other incomplete/error/concurrent
 * scans use original per-target removal. Owner loss stops deletion entirely. */
bool pet_file_cleanup_prepare(pet_file_cleanup_plan_t *plan, const char *directory,
  const pet_file_cleanup_target_t *targets, size_t count, pet_file_cleanup_guard_t guard);
bool pet_file_cleanup_remove_at(const pet_file_cleanup_plan_t *plan, size_t index);
