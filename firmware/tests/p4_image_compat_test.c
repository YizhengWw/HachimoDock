#include <assert.h>
#include <string.h>
#include "pet_p4_image_compat.h"

int main(void) {
  unsigned char image[24] = {0xe9};
  image[12] = 18;
  for (unsigned family = 0; family < 2; ++family) {
    unsigned min = family ? 300 : 1, max = family ? 399 : 199;
    image[15] = min & 255; image[16] = min >> 8;
    image[17] = max & 255; image[18] = max >> 8;
    for (unsigned revision = 0; revision <= 400; ++revision)
      assert(pet_p4_image_header_matches(image, sizeof image, revision)
             == (revision >= min && revision <= max));
    assert(!pet_p4_image_header_matches(image, 23, min));
  }
  image[15] = 0; image[16] = 0; image[17] = 255; image[18] = 255;
  assert(!pet_p4_image_header_matches(image, sizeof image, 301));
  assert(!pet_p4_revision_range_valid(1, 399));
  assert(!pet_p4_revision_range_valid(200, 299));
  return 0;
}
