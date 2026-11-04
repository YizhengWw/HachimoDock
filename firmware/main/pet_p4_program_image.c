#include "pet_p4_program_image.h"
#include <limits.h>

static void zero_bytes(void *target, size_t count) {
  unsigned char *out = target;
  for (size_t i = 0; i < count; i++) out[i] = 0;
}
static bool overlaps(const void *a, size_t an, const void *b, size_t bn) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
  return x <= y ? y - x < an : x - y < bn;
}
static uint16_t le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
    | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t signed32(uint32_t value) {
  return value <= INT32_MAX ? (int32_t)value : -1 - (int32_t)(UINT32_MAX - value);
}
static uint32_t crc32(const uint8_t *bytes, size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; i++) {
    if (i >= 20 && i < 24) continue;
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
  }
  return ~crc;
}
bool pet_program_image_program(const pet_program_image_t *image, pet_program_t *out) {
  if (!image || !out || !image->valid || !image->schema
      || !image->count || image->count > PET_PROGRAM_CODE) return false;
  pet_program_t p = {image->code, image->count, image->fuel, image->cells, 0
#if PET_P4_PROGRAM_V2
    , image->grid_cols, image->grid_rows, image->texts
#endif
  };
  if (pet_program_validate(&p) != PET_PROGRAM_READY) return false;
  for (unsigned i = 0; i < p.count; i++) if (p.code[i].op == PET_OP_INTENT) return false;
  *out = p;
  return true;
}
pet_program_image_result_t pet_program_image_decode(pet_program_image_t *out,
    const uint8_t *bytes, size_t length) {
  if (!out || !bytes || overlaps(out, sizeof(*out), bytes, length))
    return PET_PROGRAM_IMAGE_INVALID_ARGUMENT;
  zero_bytes(out, sizeof(*out));
  if (length < PET_PROGRAM_IMAGE_HEADER || length > PET_PROGRAM_IMAGE_MAX_BYTES)
    return PET_PROGRAM_IMAGE_BAD_LENGTH;
  bool v2 = PET_P4_PROGRAM_V2 && bytes[4] == 2;
  if (bytes[0] != 'P' || bytes[1] != '4' || bytes[2] != 'P' || bytes[3] != '1'
      || (bytes[4] != 1 && !v2) || bytes[5] || le16(bytes + 6) != PET_PROGRAM_IMAGE_HEADER
      || (!v2 && bytes[13]) || bytes[14] || bytes[15] || !le32(bytes + 16))
    return PET_PROGRAM_IMAGE_BAD_HEADER;
  uint16_t count = le16(bytes + 8);
  if (!count || count > PET_PROGRAM_CODE || length != PET_PROGRAM_IMAGE_HEADER + (size_t)count * 8U)
    return PET_PROGRAM_IMAGE_BAD_LENGTH;
  if (!v2 && (count > PET_PROGRAM_V1_CODE || le16(bytes + 10) > PET_PROGRAM_V1_FUEL || bytes[12] > PET_PROGRAM_V1_CELLS))
    return PET_PROGRAM_IMAGE_BAD_CODE;
  if (crc32(bytes, length) != le32(bytes + 20)) return PET_PROGRAM_IMAGE_BAD_CHECKSUM;
  for (unsigned i = 0; i < count; i++) {
    const uint8_t *p = bytes + PET_PROGRAM_IMAGE_HEADER + (size_t)i * 8U;
    out->code[i] = (pet_program_insn_t){p[0], p[1], p[2], p[3], signed32(le32(p + 4))};
#if PET_P4_PROGRAM_V2
    if (!v2 && p[0] >= PET_OP_TEXT) { zero_bytes(out, sizeof(*out)); return PET_PROGRAM_IMAGE_BAD_CODE; }
#endif
    if (p[0] == PET_OP_INTENT) {
      zero_bytes(out, sizeof(*out));
      return PET_PROGRAM_IMAGE_EFFECT_FORBIDDEN;
    }
  }
  out->count = count; out->fuel = le16(bytes + 10);
  unsigned cells = v2 ? le16(bytes + 12) : bytes[12];
  /* v1 keeps its historical BAD_CODE for an oversized byte; only the wider v2 field is clipped here. */
  if (v2 && cells > PET_PROGRAM_CELLS) { zero_bytes(out, sizeof(*out)); return PET_PROGRAM_IMAGE_BAD_HEADER; }
  out->cells = (pet_program_cells_t)(cells > PET_PROGRAM_CELLS ? PET_PROGRAM_CELLS + 1U : cells); out->schema = le32(bytes + 16); out->valid = true;
  pet_program_t p;
#if PET_P4_PROGRAM_V2
  /* Decode validates instruction shape before package metadata is available.
   * Activation still requires the real grid via image_program revalidation. */
  if (v2 && cells) out->grid_cols = out->grid_rows = 1;
#endif
  if (!pet_program_image_program(out, &p)) {
    zero_bytes(out, sizeof(*out));
    return PET_PROGRAM_IMAGE_BAD_CODE;
  }
#if PET_P4_PROGRAM_V2
  out->grid_cols = out->grid_rows = 0;
#endif
  return PET_PROGRAM_IMAGE_OK;
}
