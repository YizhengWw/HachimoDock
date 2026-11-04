#pragma once
#include <stdint.h>

/* Private lookup-start experiment, NOT a directory/content/existence cache.
 * Ordinary targets compile no wrappers or RAM. A hint is never an answer:
 * every request still runs the unmodified SDK full-name/flags/header visitor,
 * including wraparound and its exact I/O errors. Stale/hash-colliding hints
 * can cost time, but cannot authorize success, skip readback or hide failure.
 * GNU --wrap covers audited cross-object calls only; IDF ABI/link audit required.
 */
#ifndef PET_P4_SPIFFS_HINT_ENABLE
#define PET_P4_SPIFFS_HINT_ENABLE 0
#endif
#ifndef PET_P4_SPIFFS_HINT_APPLY
#define PET_P4_SPIFFS_HINT_APPLY 0
#endif
#if PET_P4_SPIFFS_HINT_APPLY && !PET_P4_SPIFFS_HINT_ENABLE
#error "lookup-start application requires the private observer"
#endif
typedef struct {
    uint64_t positive_us, negative_us, error_us;
    uint32_t calls, candidates, applied, positives, negatives, errors, maximum_us;
} pet_spiffs_hint_stats_t;
#if PET_P4_SPIFFS_HINT_ENABLE
/* Numeric snapshot only; no paths, hashes, pointers or file data exported. */
void pet_spiffs_hint_stats(pet_spiffs_hint_stats_t *out);
#endif
