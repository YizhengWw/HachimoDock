#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_ASSET_CATALOG_MAX_FAMILIES 24
#define PET_P4_ASSET_CATALOG_MAX_FRAMES 320
#define PET_P4_ASSET_FAMILY_MAX 48
#define PET_P4_ASSET_PATH_MAX 128

typedef enum {
  PET_P4_ASSET_CODEC_MJPEG = 0,
  PET_P4_ASSET_CODEC_H264 = 1,
} pet_p4_asset_codec_t;

typedef struct {
  char family[PET_P4_ASSET_FAMILY_MAX];
  char path[PET_P4_ASSET_PATH_MAX];
  char audio_path[PET_P4_ASSET_PATH_MAX];
  uint32_t frame_sizes[PET_P4_ASSET_CATALOG_MAX_FRAMES];
  uint32_t stream_bytes;
  uint16_t frames;
  uint16_t fps;
  uint32_t frame_duration_ms;
  uint32_t duration_ms;
  uint16_t width;
  uint16_t height;
  pet_p4_asset_codec_t codec;
} pet_p4_asset_entry_t;

typedef struct {
  pet_p4_asset_entry_t entries[PET_P4_ASSET_CATALOG_MAX_FAMILIES];
  uint16_t count;
  uint16_t width;
  uint16_t height;
  uint16_t fps;
  pet_p4_asset_codec_t codec;
} pet_p4_asset_catalog_t;

void pet_p4_asset_catalog_init(pet_p4_asset_catalog_t *catalog);
bool pet_p4_asset_catalog_parse(pet_p4_asset_catalog_t *catalog, const char *manifest_json);
int pet_p4_asset_catalog_find_exact(const pet_p4_asset_catalog_t *catalog, const char *family);
int pet_p4_asset_catalog_count_prefix(const pet_p4_asset_catalog_t *catalog, const char *prefix);
int pet_p4_asset_catalog_nth_prefix(const pet_p4_asset_catalog_t *catalog, const char *prefix, int nth);

#ifdef __cplusplus
}
#endif
