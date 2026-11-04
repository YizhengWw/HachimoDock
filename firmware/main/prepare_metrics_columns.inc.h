/* Private integration fragment, not a replacement pet_p4_font_bitmap.c.
 * Include after the existing bitmap/metrics arrays and shared inflater.
 * Parent must call this BEFORE inflating the bitmap and before any tasks.
 * Existing attempted/ready latch and full startup failure clearing stay owned
 * by the parent. No allocation, task, filesystem or extra PSRAM buffer here.
 */
#include "pet_p4_font_metrics_columns_config.h"
#if PET_FONT_NATIVE_32
#include "pet_p4_font_metrics_columns_size.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
extern const unsigned char metrics_columns_packed[] asm("_binary_pet_cn_metrics_columns_deflate_start");
extern const unsigned char metrics_columns_packed_end[] asm("_binary_pet_cn_metrics_columns_deflate_end");
_Static_assert(PET_FONT_METRIC_COLUMNS_FORMAT == 1U, "Unknown metric column format");
_Static_assert(PET_FONT_METRIC_COLUMNS_STRIDE == 8U, "Unknown metric record stride");
_Static_assert(PET_FONT_METRIC_COLUMNS_RECORDS == PET_FONT_METRIC_RECORDS, "Metric record count mismatch");
_Static_assert(PET_FONT_METRIC_COLUMNS_BYTES == PET_FONT_METRIC_COLUMNS_RECORDS * 8U, "Metric column length mismatch");
_Static_assert(PET_FONT_METRIC_COLUMNS_BYTES == sizeof(pet_font_metrics), "Metric output ABI mismatch");
_Static_assert(PET_FONT_METRIC_COLUMNS_BYTES <= sizeof(pet_font_bitmap), "Bitmap scratch too small");
_Static_assert(PET_FONT_METRIC_COLUMNS_ORIGINAL_ADLER == PET_FONT_METRIC_ADLER, "Original metrics identity mismatch");

static uint32_t metrics_columns_crc_step(uint32_t crc, uint8_t byte) {
  /* Reflected IEEE CRC32; 64 bytes of const flash, not a RAM lookup table. */
  static const uint32_t table[16] = {
    UINT32_C(0x00000000), UINT32_C(0x1db71064), UINT32_C(0x3b6e20c8), UINT32_C(0x26d930ac),
    UINT32_C(0x76dc4190), UINT32_C(0x6b6b51f4), UINT32_C(0x4db26158), UINT32_C(0x5005713c),
    UINT32_C(0xedb88320), UINT32_C(0xf00f9344), UINT32_C(0xd6d6a3e8), UINT32_C(0xcb61b38c),
    UINT32_C(0x9b64c2b0), UINT32_C(0x86d3d2d4), UINT32_C(0xa00ae278), UINT32_C(0xbdbdf21c)
  };
  crc ^= byte;
  crc = table[crc & 15U] ^ (crc >> 4);
  return table[crc & 15U] ^ (crc >> 4);
}
static uint32_t metrics_columns_crc(const uint8_t *bytes, size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; ++i) crc = metrics_columns_crc_step(crc, bytes[i]);
  return crc ^ UINT32_MAX;
}
static bool prepare_metrics_columns(void) {
  size_t consumed = (size_t)(metrics_columns_packed_end - metrics_columns_packed);
  size_t decoded = PET_FONT_METRIC_COLUMNS_BYTES;
  uint8_t *scratch = pet_font_bitmap;
  uint32_t canonical_crc = UINT32_MAX, adler_a = 1, adler_b = 0;
  if (consumed != PET_FONT_METRIC_COLUMNS_PACKED_BYTES
      || metrics_columns_crc(metrics_columns_packed, consumed) != PET_FONT_METRIC_COLUMNS_PACKED_CRC) goto failed;
  tinfl_init(&inflater);
  tinfl_status status = tinfl_decompress(&inflater, metrics_columns_packed, &consumed,
    scratch, scratch, &decoded,
    TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (status != TINFL_STATUS_DONE || consumed != PET_FONT_METRIC_COLUMNS_PACKED_BYTES
      || decoded != PET_FONT_METRIC_COLUMNS_BYTES
      || tinfl_get_adler32(&inflater) != PET_FONT_METRIC_COLUMNS_ADLER
      || metrics_columns_crc(scratch, decoded) != PET_FONT_METRIC_COLUMNS_CRC) goto failed;
  for (size_t i = 0; i < PET_FONT_METRIC_COLUMNS_RECORDS; ++i) {
    uint8_t record[8];
    for (size_t col = 0; col < 8; ++col) {
      record[col] = scratch[col * PET_FONT_METRIC_COLUMNS_RECORDS + i];
      canonical_crc = metrics_columns_crc_step(canonical_crc, record[col]);
      adler_a += record[col];
      if (adler_a >= 65521U) adler_a -= 65521U;
      adler_b += adler_a;
      if (adler_b >= 65521U) adler_b -= 65521U;
    }
    uint32_t word = (uint32_t)record[0] | ((uint32_t)record[1] << 8)
      | ((uint32_t)record[2] << 16) | ((uint32_t)record[3] << 24);
    uint32_t index = word & UINT32_C(0xfffff);
    uint8_t width = record[4], height = record[5];
    uint32_t length = ((uint32_t)width * height + 7U) / 8U;
    int8_t ox, oy;
    memcpy(&ox, record + 6, 1); memcpy(&oy, record + 7, 1);
    if (index >= PET_FONT_BITMAP_BYTES || length > PET_FONT_BITMAP_BYTES - index) goto failed;
    pet_font_metrics[i] = (lv_font_fmt_txt_glyph_dsc_t){
      .bitmap_index=index, .adv_w=word >> 20, .box_w=width, .box_h=height, .ofs_x=ox, .ofs_y=oy};
  }
  if ((canonical_crc ^ UINT32_MAX) != PET_FONT_METRIC_COLUMNS_ORIGINAL_CRC
      || ((adler_b << 16) | adler_a) != PET_FONT_METRIC_COLUMNS_ORIGINAL_ADLER) goto failed;
  return true;
failed:
  memset(pet_font_metrics, 0, sizeof(pet_font_metrics));
  memset(scratch, 0, PET_FONT_METRIC_COLUMNS_BYTES);
  return false;
}
#endif
