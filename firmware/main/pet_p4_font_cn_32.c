/*******************************************************************************
 * Size: 32 px
 * Bpp: 2
 * Font: MiSans Medium, downloaded from https://hyperos.mi.com/font/zh/
 * MiSans is used under the MiSans Fonts Intellectual Property License Agreement.
 * The source font is intentionally not redistributed with this repository.
 * Charset: built-in component titles, launcher/fixed bubble labels and printable ASCII; 199 glyphs.
 * Rebuild: node firmware/tools/generate_p4_font.cjs --font MiSans-Medium.otf --converter path/to/lv_font_conv
 ******************************************************************************/

/* Imported without rerasterization from PR12 head 85e194d9d970513c6a475a33ce8cabe0c5c72603. */
#include "pet_p4_font_native32.h"
#if PET_FONT_NATIVE_32
#include "esp_attr.h"
#include <string.h>
#include <stddef.h>
_Static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t)==8,"review native32 metric ABI");
static const uint8_t pet_font_native32_packed[] = {
#include "pet_p4_font_native32_packed.inc"
};
_Static_assert(sizeof(pet_font_native32_packed)==PET_FONT_NATIVE32_PACKED_BYTES,"native32 packed size");
static EXT_RAM_BSS_ATTR uint8_t pet_font_native32_bitmap[PET_FONT_NATIVE32_BITMAP_BYTES];
static bool native32_attempted,native32_ready;


static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 148, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 140, .box_w = 5, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 30, .adv_w = 182, .box_w = 9, .box_h = 9, .ofs_x = 1, .ofs_y = 19},
    {.bitmap_index = 51, .adv_w = 366, .box_w = 20, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 171, .adv_w = 300, .box_w = 18, .box_h = 31, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 311, .adv_w = 428, .box_w = 27, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 473, .adv_w = 370, .box_w = 22, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 605, .adv_w = 100, .box_w = 4, .box_h = 9, .ofs_x = 1, .ofs_y = 19},
    {.bitmap_index = 614, .adv_w = 164, .box_w = 8, .box_h = 30, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 674, .adv_w = 164, .box_w = 8, .box_h = 30, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 734, .adv_w = 230, .box_w = 11, .box_h = 11, .ofs_x = 2, .ofs_y = 13},
    {.bitmap_index = 765, .adv_w = 316, .box_w = 16, .box_h = 16, .ofs_x = 2, .ofs_y = 5},
    {.bitmap_index = 829, .adv_w = 136, .box_w = 5, .box_h = 10, .ofs_x = 2, .ofs_y = -6},
    {.bitmap_index = 842, .adv_w = 236, .box_w = 13, .box_h = 3, .ofs_x = 1, .ofs_y = 11},
    {.bitmap_index = 852, .adv_w = 130, .box_w = 4, .box_h = 4, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 856, .adv_w = 210, .box_w = 11, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 922, .adv_w = 328, .box_w = 18, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1030, .adv_w = 216, .box_w = 9, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1084, .adv_w = 294, .box_w = 16, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1180, .adv_w = 306, .box_w = 17, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1282, .adv_w = 308, .box_w = 19, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1396, .adv_w = 308, .box_w = 17, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1498, .adv_w = 308, .box_w = 17, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1600, .adv_w = 274, .box_w = 16, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1696, .adv_w = 326, .box_w = 18, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1804, .adv_w = 312, .box_w = 17, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1906, .adv_w = 130, .box_w = 4, .box_h = 18, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 1924, .adv_w = 136, .box_w = 5, .box_h = 24, .ofs_x = 2, .ofs_y = -6},
    {.bitmap_index = 1954, .adv_w = 316, .box_w = 16, .box_h = 20, .ofs_x = 2, .ofs_y = 2},
    {.bitmap_index = 2034, .adv_w = 316, .box_w = 16, .box_h = 10, .ofs_x = 2, .ofs_y = 8},
    {.bitmap_index = 2074, .adv_w = 316, .box_w = 16, .box_h = 20, .ofs_x = 2, .ofs_y = 2},
    {.bitmap_index = 2154, .adv_w = 256, .box_w = 15, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 2244, .adv_w = 428, .box_w = 25, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2394, .adv_w = 350, .box_w = 22, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 2526, .adv_w = 344, .box_w = 18, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 2634, .adv_w = 358, .box_w = 21, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2760, .adv_w = 384, .box_w = 21, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 2886, .adv_w = 304, .box_w = 16, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 2982, .adv_w = 290, .box_w = 16, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3078, .adv_w = 380, .box_w = 21, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 3204, .adv_w = 376, .box_w = 19, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3318, .adv_w = 130, .box_w = 4, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3342, .adv_w = 250, .box_w = 14, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 3426, .adv_w = 336, .box_w = 20, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3546, .adv_w = 294, .box_w = 16, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3642, .adv_w = 456, .box_w = 25, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3792, .adv_w = 380, .box_w = 20, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3912, .adv_w = 400, .box_w = 23, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 4050, .adv_w = 324, .box_w = 18, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 4158, .adv_w = 402, .box_w = 23, .box_h = 27, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 4314, .adv_w = 330, .box_w = 19, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 4428, .adv_w = 310, .box_w = 19, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 4542, .adv_w = 314, .box_w = 19, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 4656, .adv_w = 366, .box_w = 19, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 4770, .adv_w = 352, .box_w = 22, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 4902, .adv_w = 510, .box_w = 32, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 5094, .adv_w = 338, .box_w = 21, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 5220, .adv_w = 326, .box_w = 21, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 5346, .adv_w = 312, .box_w = 18, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 5454, .adv_w = 182, .box_w = 8, .box_h = 30, .ofs_x = 3, .ofs_y = -2},
    {.bitmap_index = 5514, .adv_w = 178, .box_w = 11, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 5580, .adv_w = 182, .box_w = 8, .box_h = 30, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 5640, .adv_w = 232, .box_w = 14, .box_h = 9, .ofs_x = 0, .ofs_y = 18},
    {.bitmap_index = 5672, .adv_w = 236, .box_w = 15, .box_h = 3, .ofs_x = 0, .ofs_y = -5},
    {.bitmap_index = 5684, .adv_w = 138, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 19},
    {.bitmap_index = 5697, .adv_w = 280, .box_w = 15, .box_h = 18, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 5765, .adv_w = 308, .box_w = 16, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 5861, .adv_w = 274, .box_w = 15, .box_h = 18, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 5929, .adv_w = 310, .box_w = 17, .box_h = 24, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 6031, .adv_w = 288, .box_w = 16, .box_h = 18, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 6103, .adv_w = 180, .box_w = 11, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 6169, .adv_w = 308, .box_w = 17, .box_h = 25, .ofs_x = 1, .ofs_y = -7},
    {.bitmap_index = 6276, .adv_w = 302, .box_w = 15, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 6366, .adv_w = 130, .box_w = 5, .box_h = 25, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 6398, .adv_w = 130, .box_w = 9, .box_h = 32, .ofs_x = -2, .ofs_y = -7},
    {.bitmap_index = 6470, .adv_w = 268, .box_w = 15, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 6560, .adv_w = 134, .box_w = 6, .box_h = 24, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 6596, .adv_w = 458, .box_w = 25, .box_h = 18, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 6709, .adv_w = 302, .box_w = 15, .box_h = 18, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 6777, .adv_w = 298, .box_w = 17, .box_h = 18, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 6854, .adv_w = 308, .box_w = 16, .box_h = 25, .ofs_x = 2, .ofs_y = -7},
    {.bitmap_index = 6954, .adv_w = 310, .box_w = 17, .box_h = 25, .ofs_x = 1, .ofs_y = -7},
    {.bitmap_index = 7061, .adv_w = 200, .box_w = 11, .box_h = 18, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 7111, .adv_w = 246, .box_w = 15, .box_h = 18, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 7179, .adv_w = 188, .box_w = 12, .box_h = 24, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 7251, .adv_w = 302, .box_w = 15, .box_h = 18, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 7319, .adv_w = 266, .box_w = 17, .box_h = 18, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 7396, .adv_w = 414, .box_w = 26, .box_h = 18, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 7513, .adv_w = 258, .box_w = 17, .box_h = 18, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 7590, .adv_w = 270, .box_w = 17, .box_h = 25, .ofs_x = 0, .ofs_y = -7},
    {.bitmap_index = 7697, .adv_w = 248, .box_w = 15, .box_h = 18, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 7765, .adv_w = 184, .box_w = 11, .box_h = 29, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 7845, .adv_w = 118, .box_w = 4, .box_h = 30, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 7875, .adv_w = 184, .box_w = 11, .box_h = 29, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 7955, .adv_w = 276, .box_w = 15, .box_h = 6, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 7978, .adv_w = 512, .box_w = 30, .box_h = 4, .ofs_x = 1, .ofs_y = 11},
    {.bitmap_index = 8008, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 8241, .adv_w = 512, .box_w = 26, .box_h = 30, .ofs_x = 3, .ofs_y = -2},
    {.bitmap_index = 8436, .adv_w = 512, .box_w = 29, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 8654, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 8879, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 9104, .adv_w = 512, .box_w = 28, .box_h = 29, .ofs_x = 2, .ofs_y = -3},
    {.bitmap_index = 9307, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 9540, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 9773, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 10006, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 10231, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 10456, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 10681, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 10914, .adv_w = 512, .box_w = 27, .box_h = 28, .ofs_x = 2, .ofs_y = -3},
    {.bitmap_index = 11103, .adv_w = 512, .box_w = 28, .box_h = 30, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 11313, .adv_w = 512, .box_w = 28, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 11530, .adv_w = 512, .box_w = 30, .box_h = 32, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 11770, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 11988, .adv_w = 512, .box_w = 30, .box_h = 28, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 12198, .adv_w = 512, .box_w = 28, .box_h = 30, .ofs_x = 3, .ofs_y = -2},
    {.bitmap_index = 12408, .adv_w = 512, .box_w = 28, .box_h = 30, .ofs_x = 3, .ofs_y = -2},
    {.bitmap_index = 12618, .adv_w = 512, .box_w = 26, .box_h = 28, .ofs_x = 3, .ofs_y = -2},
    {.bitmap_index = 12800, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 13033, .adv_w = 512, .box_w = 26, .box_h = 28, .ofs_x = 3, .ofs_y = -2},
    {.bitmap_index = 13215, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 13448, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 13673, .adv_w = 512, .box_w = 29, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 13898, .adv_w = 512, .box_w = 29, .box_h = 29, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 14109, .adv_w = 512, .box_w = 28, .box_h = 30, .ofs_x = 2, .ofs_y = -3},
    {.bitmap_index = 14319, .adv_w = 512, .box_w = 29, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 14544, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 14762, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 14980, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 15205, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 15430, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 15648, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 15873, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 16098, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 16323, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 16556, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 16774, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 16999, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 17224, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 17457, .adv_w = 512, .box_w = 28, .box_h = 31, .ofs_x = 2, .ofs_y = -3},
    {.bitmap_index = 17674, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 17907, .adv_w = 512, .box_w = 22, .box_h = 28, .ofs_x = 5, .ofs_y = -2},
    {.bitmap_index = 18061, .adv_w = 512, .box_w = 30, .box_h = 28, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 18271, .adv_w = 512, .box_w = 28, .box_h = 30, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 18481, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 18706, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 18931, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 19164, .adv_w = 512, .box_w = 30, .box_h = 28, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 19374, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 19599, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 19832, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 20057, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 20282, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 20500, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 20733, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 20951, .adv_w = 512, .box_w = 29, .box_h = 29, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 21162, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 21395, .adv_w = 512, .box_w = 29, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 21613, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 21846, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 22064, .adv_w = 512, .box_w = 28, .box_h = 29, .ofs_x = 3, .ofs_y = -2},
    {.bitmap_index = 22267, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 22485, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 22710, .adv_w = 512, .box_w = 28, .box_h = 31, .ofs_x = 3, .ofs_y = -3},
    {.bitmap_index = 22927, .adv_w = 512, .box_w = 28, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 23144, .adv_w = 512, .box_w = 29, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 23369, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 23594, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 23819, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 24044, .adv_w = 512, .box_w = 27, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 24247, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 24465, .adv_w = 512, .box_w = 29, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 24683, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 24901, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 25134, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 25367, .adv_w = 512, .box_w = 24, .box_h = 30, .ofs_x = 4, .ofs_y = -2},
    {.bitmap_index = 25547, .adv_w = 512, .box_w = 29, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 25765, .adv_w = 512, .box_w = 29, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 25990, .adv_w = 512, .box_w = 29, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 26208, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 26426, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 26644, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 26862, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 27095, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 27320, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 27553, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 27778, .adv_w = 512, .box_w = 28, .box_h = 30, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 27988, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 28206, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 28431, .adv_w = 512, .box_w = 30, .box_h = 28, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 28641, .adv_w = 512, .box_w = 29, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 28859, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 29092, .adv_w = 512, .box_w = 26, .box_h = 30, .ofs_x = 3, .ofs_y = -2},
    {.bitmap_index = 29287, .adv_w = 512, .box_w = 30, .box_h = 31, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 29520, .adv_w = 512, .box_w = 29, .box_h = 27, .ofs_x = 2, .ofs_y = -2},
    {.bitmap_index = 29716, .adv_w = 512, .box_w = 30, .box_h = 29, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 29934, .adv_w = 512, .box_w = 30, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 30159, .adv_w = 512, .box_w = 28, .box_h = 30, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 30369, .adv_w = 512, .box_w = 28, .box_h = 30, .ofs_x = 2, .ofs_y = -3}
};

static const uint16_t unicode_list_0[] = {
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
    0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x4de0,
    0x4df3, 0x4e0d, 0x4e2b, 0x4e46, 0x4e6b, 0x4e71, 0x4eaa, 0x4eca,
    0x4ed6, 0x4ef1, 0x4f40, 0x50af, 0x515b, 0x51e0, 0x51f7, 0x527b,
    0x527e, 0x5366, 0x53ac, 0x53e3, 0x54a8, 0x557d, 0x56aa, 0x56cd,
    0x5737, 0x57da, 0x58d0, 0x5909, 0x5914, 0x5b80, 0x5bef, 0x5c2b,
    0x5f65, 0x5fa3, 0x5fe1, 0x604f, 0x60a5, 0x60ff, 0x61f0, 0x624d,
    0x6385, 0x63b0, 0x6590, 0x6599, 0x65a5, 0x65c5, 0x664f, 0x66ff,
    0x6708, 0x670c, 0x671a, 0x677c, 0x6828, 0x6b27, 0x6baf, 0x6bf4,
    0x6c14, 0x6cc8, 0x6d1b, 0x7055, 0x7116, 0x7249, 0x7296, 0x73e3,
    0x7515, 0x754a, 0x76b8, 0x76dc, 0x77c5, 0x7acf, 0x7b34, 0x7c53,
    0x7d00, 0x7e80, 0x7ea4, 0x7eb3, 0x8081, 0x80d6, 0x80f1, 0x81ca,
    0x8291, 0x82e4, 0x86a7, 0x86ab, 0x86b9, 0x882c, 0x8848, 0x8c41,
    0x8d0a, 0x8dd3, 0x8f46, 0x8fb1, 0x8fe9, 0x9172, 0x947f, 0x950e,
    0x95d4, 0x9739, 0x98be, 0x9c5c, 0x9c7c, 0x9dff, 0x9ea4
};

static const lv_font_fmt_txt_cmap_t cmaps[] = {
    {
        .range_start = 32, .range_length = 40613, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 199, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};

static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = pet_font_native32_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .cmap_num = 1,
    .bpp = 2,
    .bitmap_format = 0,
};

const lv_font_t pet_p4_font_cn_32 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 36,
    .base_line = 6,
    .dsc = &font_dsc,
};

void pet_p4_font_native32_clear(void){
  memset(pet_font_native32_bitmap,0,sizeof(pet_font_native32_bitmap));
  native32_attempted=true;native32_ready=false;
}
bool pet_p4_font_native32_prepare(tinfl_decompressor *shared){
  if(native32_attempted)return native32_ready;
  native32_attempted=true;
  if(!shared){pet_p4_font_native32_clear();return false;}
  size_t consumed=sizeof(pet_font_native32_packed),decoded=sizeof(pet_font_native32_bitmap);
  tinfl_init(shared);
  tinfl_status status=tinfl_decompress(shared,pet_font_native32_packed,&consumed,
    pet_font_native32_bitmap,pet_font_native32_bitmap,&decoded,
    TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  native32_ready=status==TINFL_STATUS_DONE&&consumed==sizeof(pet_font_native32_packed)
    &&decoded==sizeof(pet_font_native32_bitmap)&&tinfl_get_adler32(shared)==PET_FONT_NATIVE32_BITMAP_ADLER;
  if(!native32_ready)pet_p4_font_native32_clear();
  return native32_ready;
}
bool pet_p4_font_native32_has_glyph(uint32_t codepoint){
  if(!native32_ready||codepoint<32||codepoint-32>=40613)return false;
  uint32_t key=codepoint-32;size_t lo=0,hi=PET_FONT_NATIVE32_GLYPHS;
  while(lo<hi){size_t mid=lo+(hi-lo)/2;if(unicode_list_0[mid]<key)lo=mid+1;else hi=mid;}
  return lo<PET_FONT_NATIVE32_GLYPHS&&unicode_list_0[lo]==key;
}
#endif /* PET_FONT_NATIVE_32: no font data or helpers when disabled. */
