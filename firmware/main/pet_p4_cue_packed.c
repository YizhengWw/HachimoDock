/* Lossless fixed factory PCM startup. No allocation, task, playback-time decode,
 * shared scratch or DSP. All writable payload/inflater storage is static PSRAM.
 * After release publication PCM is immutable for the entire boot lifetime. */
#include "pet_p4_cue_packed.h"
#include "pet_p4_cue_packed_size.h"
#include "sdkconfig.h"
#include "esp_attr.h"
#include "miniz.h"
#include <stdatomic.h>
#include <string.h>
#if !defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) || !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "Packed factory cues require static PSRAM BSS; internal RAM fallback is forbidden"
#endif

typedef struct {
  uint32_t packed_offset, packed_size, pcm_offset, pcm_size, planes_adler, pcm_fnv;
} cue_descriptor_t;
_Static_assert(sizeof(cue_descriptor_t) == 24, "fixed cue descriptor ABI");
#include "pet_p4_cue_packed_data.inc"

static EXT_RAM_BSS_ATTR uint8_t cue_pcm[PET_CUE_PCM_BYTES];
static EXT_RAM_BSS_ATTR uint8_t cue_temp[PET_CUE_TEMP_BYTES];
static EXT_RAM_BSS_ATTR tinfl_decompressor cue_inflater;
enum { CUE_EMPTY, CUE_INITIALIZING, CUE_READY, CUE_FAILED };
static atomic_uint cue_state;

static uint32_t restore_pcm(uint8_t *pcm, const uint8_t *planes, size_t bytes) {
  uint16_t history[PET_CUE_DELTA_ORDER] = {0};
  size_t samples = bytes / 2U;
  uint32_t checksum = UINT32_C(2166136261);
  for (size_t i = 0; i < samples; ++i) {
    uint16_t zigzag = (uint16_t)((uint16_t)planes[i]
      | (uint16_t)((uint16_t)planes[samples + i] << 8));
    /* Unsigned arithmetic makes both zigzag sign restoration and all six
     * integrators explicit modulo65536; no negative shifts or signed overflow. */
    uint16_t value = (uint16_t)((zigzag >> 1) ^ (UINT32_C(0) - (zigzag & 1U)));
    for (unsigned order = 0; order < PET_CUE_DELTA_ORDER; ++order) {
      history[order] = (uint16_t)((uint32_t)history[order] + value);
      value = history[order];
    }
    pcm[2U*i] = (uint8_t)value;
    pcm[2U*i+1U] = (uint8_t)(value >> 8);
    checksum = (checksum ^ pcm[2U*i]) * UINT32_C(16777619);
    checksum = (checksum ^ pcm[2U*i+1U]) * UINT32_C(16777619);
  }
  return checksum;
}

static bool decode_cue(const cue_descriptor_t *d) {
  if (!d->pcm_size || (d->pcm_size & 1U) || d->pcm_size > sizeof(cue_temp)
      || d->pcm_offset > sizeof(cue_pcm) || d->pcm_size > sizeof(cue_pcm) - d->pcm_offset
      || !d->packed_size || d->packed_offset > sizeof(packed_bytes)
      || d->packed_size > sizeof(packed_bytes) - d->packed_offset) return false;
  size_t consumed = d->packed_size, produced = d->pcm_size;
  tinfl_init(&cue_inflater);
  tinfl_status status = tinfl_decompress(&cue_inflater,
    packed_bytes + d->packed_offset, &consumed, cue_temp, cue_temp, &produced,
    TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (status != TINFL_STATUS_DONE || consumed != d->packed_size
      || produced != d->pcm_size || tinfl_get_adler32(&cue_inflater) != d->planes_adler)
    return false;

  return restore_pcm(cue_pcm + d->pcm_offset, cue_temp, d->pcm_size) == d->pcm_fnv;
}

bool pet_p4_cue_packed_init(void) {
  unsigned expected = CUE_EMPTY;
  if (!atomic_compare_exchange_strong_explicit(&cue_state, &expected, CUE_INITIALIZING,
        memory_order_acq_rel, memory_order_acquire)) return expected == CUE_READY;
  bool ok = true;
  for (unsigned i = 0; i < PET_CUE_PACKED_COUNT; ++i) {
    if (!decode_cue(&descriptors[i])) { ok = false; break; }
  }
  if (!ok) memset(cue_pcm, 0, sizeof(cue_pcm));
  memset(cue_temp, 0, sizeof(cue_temp));
  memset(&cue_inflater, 0, sizeof(cue_inflater));
  atomic_store_explicit(&cue_state, ok ? CUE_READY : CUE_FAILED, memory_order_release);
  return ok;
}

bool pet_p4_cue_packed_pcm(unsigned index, const uint8_t **pcm, size_t *size) {
  if (pcm) *pcm = NULL;
  if (size) *size = 0;
  if (!pcm || !size || index >= PET_CUE_PACKED_COUNT
      || atomic_load_explicit(&cue_state, memory_order_acquire) != CUE_READY) return false;
  *pcm = cue_pcm + descriptors[index].pcm_offset;
  *size = descriptors[index].pcm_size;
  return true;
}
