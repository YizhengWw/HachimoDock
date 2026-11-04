/* Experimental install-image staging; NOT an advertised package capability.
 * P4P1 v1 is pure integer computation: it has no intents, IO or native pointers.
 * The trusted installer decodes into INACTIVE staging storage, then validates
 * the complete package and durable transaction before publishing that storage.
 * Never decode over an image currently used by a runtime. Failure invalidates
 * staging only; it must not overwrite the previous package or its user data.
 * CRC detects accidental damage, not authenticity or permission to execute.
 */
#pragma once
#include "pet_p4_program_core.h"

#define PET_PROGRAM_IMAGE_HEADER 24U
#define PET_PROGRAM_IMAGE_MAX_BYTES (PET_PROGRAM_IMAGE_HEADER + PET_PROGRAM_CODE * 8U)

/* Wire header, all integers little endian (no native struct casts):
 * 0..3 "P4P1", 4 version=1, 5 flags=0, 6..7 header size=24,
 * 8..9 instruction count, 10..11 total fuel, 12 cells, 13..15 reserved=0,
 * 16..19 nonzero state schema, 20..23 IEEE CRC32. CRC covers bytes 0..19
 * followed by 24..end. Exactly count*8 instruction bytes follow the header:
 * opcode,a,b,c,immediate-i32-LE. INTENT is forbidden even if unreachable.
 * File size is exact; trailing bytes, future flags and zero schema are errors.
 * A PET_P4_PROGRAM_V2 build also accepts version=2 where 12..13 is the LE16
 * cell count (<=256) and 14..15 stay reserved=0; version 1 keeps byte 12.
 */
typedef struct {
  pet_program_insn_t code[PET_PROGRAM_CODE];
  uint32_t schema;
  uint16_t count, fuel;
  pet_program_cells_t cells;
  bool valid;
#if PET_P4_PROGRAM_V2
  /* Package-derived, host-set after decode (not in the wire header): the
   * p4-cells-v1 grid mirrored into cells[0, cols*rows) and the text count. */
  uint8_t grid_cols, grid_rows, texts;
#endif
} pet_program_image_t;

typedef enum {
  PET_PROGRAM_IMAGE_OK, PET_PROGRAM_IMAGE_INVALID_ARGUMENT,
  PET_PROGRAM_IMAGE_BAD_HEADER, PET_PROGRAM_IMAGE_BAD_LENGTH,
  PET_PROGRAM_IMAGE_BAD_CHECKSUM, PET_PROGRAM_IMAGE_BAD_CODE,
  PET_PROGRAM_IMAGE_EFFECT_FORBIDDEN
} pet_program_image_result_t;

/* Input bytes remain caller-owned; success copies code to staging. No retained
 * wire pointer, heap, filesystem or host calls. Output/input must not overlap;
 * null/overlap arguments leave output unchanged; any other failure clears it.
 * The caller serializes staging and makes it immutable after publication.
 */
pet_program_image_result_t pet_program_image_decode(pet_program_image_t *staging,
  const uint8_t *bytes, size_t length);
/* Full revalidation of trusted staging before runtime activation. Output is
 * unchanged on failure. Descriptor code points into image, not the wire file.
 */
bool pet_program_image_program(const pet_program_image_t *image, pet_program_t *out);
