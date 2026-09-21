#include "pet_p4_system_cues.h"
#include <string.h>

extern const uint8_t done_start[] __asm__("_binary_done_wav_start");
extern const uint8_t done_end[] __asm__("_binary_done_wav_end");
extern const uint8_t error_start[] __asm__("_binary_error_wav_start");
extern const uint8_t error_end[] __asm__("_binary_error_wav_end");
extern const uint8_t waiting_start[] __asm__("_binary_waiting_user_wav_start");
extern const uint8_t waiting_end[] __asm__("_binary_waiting_user_wav_end");

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool pet_p4_system_cue_family(const char *family) {
  return family && (!strcmp(family,"done") || !strcmp(family,"error") || !strcmp(family,"waiting_user"));
}

/* Only known shipped defaults migrate. Unknown WAVs remain appearance overrides. */
bool pet_p4_system_cue_legacy(const char *family, uint64_t checksum) {
  if (!family) return false;
  if (!strcmp(family,"done")) return checksum == UINT64_C(0x3800fd80e9e2461f)
    || checksum == UINT64_C(0x392e208e77b6a2a4);
  if (!strcmp(family,"error") || !strcmp(family,"waiting_user")) {
    return checksum == UINT64_C(0x85ab7ddd599f8277) || checksum == UINT64_C(0x8b0ceaf6989f3c5b);
  }
  return false;
}

bool pet_p4_cue_parse_wav(const uint8_t *wav, size_t size, const uint8_t **pcm, size_t *pcm_size) {
  if (!wav || !pcm || !pcm_size || size < 44 || memcmp(wav,"RIFF",4) || memcmp(wav+8,"WAVE",4)) return false;
  bool format_ok = false;
  for (size_t pos = 12; pos <= size - 8;) {
    uint32_t n = le32(wav + pos + 4);
    size_t start = pos + 8;
    if (n > size - start) return false;
    if (!memcmp(wav + pos,"fmt ",4)) {
      const uint8_t *f = wav + start;
      format_ok = n >= 16 && f[0]==1 && f[1]==0 && f[2]==1 && f[3]==0
        && le32(f+4)==16000 && le32(f+8)==32000 && f[12]==2 && f[13]==0 && f[14]==16 && f[15]==0;
    } else if (!memcmp(wav + pos,"data",4)) {
      if (!format_ok || n==0 || (n&1U)) return false;
      *pcm = wav + start; *pcm_size = n; return true;
    }
    if ((size_t)n + (n&1U) > size - start) return false;
    pos = start + n + (n&1U);
  }
  return false;
}

bool pet_p4_system_cue_pcm(const char *family, const uint8_t **pcm, size_t *size) {
  const uint8_t *start, *end;
  if (!pet_p4_system_cue_family(family)) return false;
  if (!strcmp(family,"done")) { start=done_start; end=done_end; }
  else if (!strcmp(family,"error")) { start=error_start; end=error_end; }
  else { start=waiting_start; end=waiting_end; }
  return pet_p4_cue_parse_wav(start, (size_t)(end-start), pcm, size);
}
