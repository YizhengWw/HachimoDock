/* Versioned PC-fed visible collection. No records, IO, tasks or permissions.
 * Parser output is private candidate scratch, never a live published buffer.
 * The parent still checks component/instance/sequence and holds its host lock.
 * Default OFF; only the UI development target opts in pending physical gates. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
struct cJSON;
#ifndef PET_HOST_COLLECTION_ENABLED
#define PET_HOST_COLLECTION_ENABLED 0
#endif
#define PET_HOST_COLLECTION_ENGINE "p4-host-collection-v1"
#define PET_HOST_COLLECTION_VISIBLE 8U
#if PET_HOST_COLLECTION_ENABLED
typedef struct {
  char source[17],actions[8][48];
  uint16_t x,y,w,row_height,colors[6];
  uint8_t gap,visible,page;
  bool enabled;
} pet_host_collection_config_t;
typedef struct {
  char key[64],label[64],detail[64];
  int16_t index;
  bool present,selected,truncated;
} pet_host_collection_row_t;
typedef struct {
  char source[17],selection[64];
  pet_host_collection_row_t rows[8];
  uint16_t count,offset;
  uint8_t visible;
  bool valid;
} pet_host_collection_snapshot_t;
bool pet_host_collection_parse_config(const struct cJSON *value,pet_host_collection_config_t *out);
bool pet_host_collection_parse_snapshot(const struct cJSON *value,pet_host_collection_snapshot_t *out);
/* Config and snapshot are already validated native-private copies. */
bool pet_host_collection_matches(const pet_host_collection_config_t *config,const pet_host_collection_snapshot_t *snapshot);
#endif
