/*
 * [Input] P4 MJPEG/H.264 transaction chunks plus the `appearance` data partition.
 * [Output] Sequential raw-flash writes and a commit-last 4KiB lookup header for slot 1.
 * [Pos] High-throughput appearance storage backend for ESP32-P4.
 * [Sync] If this file changes, update protocol.md and esp-p4-runtime/.folder.md.
 */

#include "pet_p4_raw_assets.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PET_P4_RAW_SLOT_COUNT 2
#define PET_P4_RAW_HEADER_BYTES 4096U
#define PET_P4_RAW_HEADER_VERSION 1U
#define PET_P4_RAW_HEADER_ENTRY_OFFSET 88U
#define PET_P4_RAW_HEADER_ENTRY_BYTES 24U
#define PET_P4_RAW_MAX_FILES 32U
#define PET_P4_RAW_PACK_ID_BYTES 64U
#define PET_P4_RAW_ERASE_BYTES 4096U
#define PET_P4_RAW_ERASE_SLICE_BYTES (16U * 1024U)

static const unsigned char PET_P4_RAW_MAGIC[8] = {
  'P', '4', 'R', 'A', 'W', '0', '1', '\0'
};

typedef struct {
  unsigned long long path_hash;
  uint32_t offset;
  uint32_t size;
  unsigned long long checksum;
} pet_p4_raw_entry_t;

typedef struct {
  bool prepared;
  uint32_t cursor;
  unsigned int entry_count;
  pet_p4_raw_entry_t entries[PET_P4_RAW_MAX_FILES];
  bool file_active;
  char file_path[128];
  uint32_t file_offset;
  unsigned long long file_size;
  unsigned long long file_checksum;
  uint32_t next_chunk_index;
  bool last_chunk_valid;
  uint32_t last_chunk_index;
  uint32_t last_chunk_size;
  unsigned long long last_chunk_checksum;
} pet_p4_raw_transfer_t;

typedef struct {
  bool attempted;
  bool valid;
  char pack_id[PET_P4_RAW_PACK_ID_BYTES + 1U];
  uint32_t data_end;
  unsigned int entry_count;
  pet_p4_raw_entry_t entries[PET_P4_RAW_MAX_FILES];
} pet_p4_raw_cache_t;

static const esp_partition_t *g_partitions[PET_P4_RAW_SLOT_COUNT];
static pet_p4_raw_transfer_t g_transfer;
static pet_p4_raw_cache_t g_caches[PET_P4_RAW_SLOT_COUNT];

static void set_error(char *error, size_t error_size, const char *message) {
  if (!error || error_size == 0) return;
  snprintf(error, error_size, "%s", message ? message : "raw appearance storage failed");
}

static unsigned long long fnv1a64_update(
  unsigned long long hash,
  const unsigned char *bytes,
  size_t len
) {
  for (size_t i = 0; i < len; i += 1) {
    hash ^= (unsigned long long) bytes[i];
    hash *= 0x00000100000001b3ULL;
  }
  return hash;
}

static unsigned long long path_hash(const char *logical_path) {
  return fnv1a64_update(
    0xcbf29ce484222325ULL,
    (const unsigned char *) logical_path,
    strlen(logical_path)
  );
}

static void put_u32(unsigned char *out, uint32_t value) {
  out[0] = (unsigned char) (value & 0xffU);
  out[1] = (unsigned char) ((value >> 8U) & 0xffU);
  out[2] = (unsigned char) ((value >> 16U) & 0xffU);
  out[3] = (unsigned char) ((value >> 24U) & 0xffU);
}

static uint32_t get_u32(const unsigned char *in) {
  return ((uint32_t) in[0])
       | ((uint32_t) in[1] << 8U)
       | ((uint32_t) in[2] << 16U)
       | ((uint32_t) in[3] << 24U);
}

static void put_u64(unsigned char *out, unsigned long long value) {
  for (unsigned int i = 0; i < 8U; i += 1U) {
    out[i] = (unsigned char) ((value >> (i * 8U)) & 0xffULL);
  }
}

static unsigned long long get_u64(const unsigned char *in) {
  unsigned long long value = 0;
  for (unsigned int i = 0; i < 8U; i += 1U) {
    value |= (unsigned long long) in[i] << (i * 8U);
  }
  return value;
}

static const esp_partition_t *raw_partition_for_slot(int slot) {
  static const char *labels[PET_P4_RAW_SLOT_COUNT] = {
    "appearance0",
    "appearance1",
  };
  if (slot < 0 || slot >= PET_P4_RAW_SLOT_COUNT) return NULL;
  if (!g_partitions[slot]) {
    g_partitions[slot] = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_ANY,
      labels[slot]
    );
  }
  return g_partitions[slot];
}

static const esp_partition_t *raw_partition(void) {
  return raw_partition_for_slot(PET_P4_RAW_APPEARANCE_SLOT);
}

static uint32_t align_up(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1U) & ~(alignment - 1U);
}

static bool parse_chunk_index(const char *text, uint32_t *out) {
  char *end = NULL;
  unsigned long value;
  if (!text || !text[0] || !out) return false;
  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno != 0 || !end || *end != '\0' || value > UINT32_MAX) return false;
  *out = (uint32_t) value;
  return true;
}

static bool erase_range_yielding(
  const esp_partition_t *partition,
  uint32_t offset,
  uint32_t size
) {
  uint32_t erased = 0;
  while (erased < size) {
    uint32_t remaining = size - erased;
    uint32_t slice = remaining < PET_P4_RAW_ERASE_SLICE_BYTES
      ? remaining
      : PET_P4_RAW_ERASE_SLICE_BYTES;
    if (esp_partition_erase_range(partition, offset + erased, slice) != ESP_OK) {
      return false;
    }
    erased += slice;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return true;
}

static bool pack_id_is_valid(const char *pack_id) {
  if (!pack_id || strlen(pack_id) != PET_P4_RAW_PACK_ID_BYTES) return false;
  for (size_t i = 0; i < PET_P4_RAW_PACK_ID_BYTES; i += 1) {
    char ch = pack_id[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  }
  return true;
}

static void invalidate_cache(int slot) {
  if (slot < 0 || slot >= PET_P4_RAW_SLOT_COUNT) return;
  memset(&g_caches[slot], 0, sizeof(g_caches[slot]));
}

void pet_p4_raw_assets_reset_transfer(void) {
  memset(&g_transfer, 0, sizeof(g_transfer));
}

bool pet_p4_raw_assets_available(void) {
  return pet_p4_raw_assets_slot_available(PET_P4_RAW_APPEARANCE_SLOT);
}

unsigned long long pet_p4_raw_assets_capacity_bytes(void) {
  return pet_p4_raw_assets_slot_capacity_bytes(PET_P4_RAW_APPEARANCE_SLOT);
}

bool pet_p4_raw_assets_slot_available(int slot) {
  return raw_partition_for_slot(slot) != NULL;
}

unsigned long long pet_p4_raw_assets_slot_capacity_bytes(int slot) {
  const esp_partition_t *partition = raw_partition_for_slot(slot);
  if (!partition || partition->size <= PET_P4_RAW_HEADER_BYTES) return 0;
  return (unsigned long long) partition->size - PET_P4_RAW_HEADER_BYTES;
}

bool pet_p4_raw_assets_supports_path(const char *logical_path) {
  static const char mjpeg_suffix[] = ".mjpg";
  static const char h264_suffix[] = ".h264";
  size_t path_len;
  if (!logical_path) return false;
  path_len = strlen(logical_path);
  return (path_len > sizeof(mjpeg_suffix) - 1U
          && strcmp(
            logical_path + path_len - (sizeof(mjpeg_suffix) - 1U),
            mjpeg_suffix
          ) == 0)
      || (path_len > sizeof(h264_suffix) - 1U
          && strcmp(
            logical_path + path_len - (sizeof(h264_suffix) - 1U),
            h264_suffix
          ) == 0);
}

bool pet_p4_raw_assets_prepare(
  unsigned long long total_bytes,
  char *error,
  size_t error_size
) {
  const esp_partition_t *partition = raw_partition();
  unsigned long long requested;
  uint32_t erase_bytes;
  if (!partition) {
    set_error(error, error_size, "appearance partition not found");
    return false;
  }
  requested = total_bytes > 0
    ? total_bytes + PET_P4_RAW_HEADER_BYTES
    : (unsigned long long) partition->size;
  if (requested > partition->size) {
    set_error(error, error_size, "appearance pack exceeds raw partition capacity");
    return false;
  }
  erase_bytes = align_up((uint32_t) requested, PET_P4_RAW_ERASE_BYTES);
  if (erase_bytes > partition->size) erase_bytes = partition->size;
  pet_p4_raw_assets_reset_transfer();
  invalidate_cache(PET_P4_RAW_APPEARANCE_SLOT);
  if (!erase_range_yielding(partition, 0, erase_bytes)) {
    set_error(error, error_size, "appearance partition erase failed");
    return false;
  }
  g_transfer.prepared = true;
  g_transfer.cursor = PET_P4_RAW_HEADER_BYTES;
  return true;
}

bool pet_p4_raw_assets_invalidate(char *error, size_t error_size) {
  const esp_partition_t *partition = raw_partition();
  pet_p4_raw_assets_reset_transfer();
  invalidate_cache(PET_P4_RAW_APPEARANCE_SLOT);
  if (!partition) return true;
  if (!erase_range_yielding(partition, 0, PET_P4_RAW_ERASE_BYTES)) {
    set_error(error, error_size, "appearance partition header erase failed");
    return false;
  }
  return true;
}

bool pet_p4_raw_assets_write_chunk(
  const char *logical_path,
  const char *index,
  const unsigned char *data,
  size_t data_len,
  char *error,
  size_t error_size
) {
  const esp_partition_t *partition = raw_partition();
  uint32_t chunk_index;
  unsigned long long chunk_checksum;
  bool starts_file;
  if (!partition || !g_transfer.prepared || !pet_p4_raw_assets_supports_path(logical_path)
      || !index || !data || data_len == 0) {
    set_error(error, error_size, "raw appearance transfer is not prepared");
    return false;
  }
  if (!parse_chunk_index(index, &chunk_index) || chunk_index == UINT32_MAX) {
    set_error(error, error_size, "raw appearance chunk index is invalid");
    return false;
  }
  chunk_checksum = fnv1a64_update(0xcbf29ce484222325ULL, data, data_len);
  if (g_transfer.file_active
      && strcmp(g_transfer.file_path, logical_path) == 0
      && g_transfer.last_chunk_valid
      && chunk_index == g_transfer.last_chunk_index) {
    if (data_len == g_transfer.last_chunk_size
        && chunk_checksum == g_transfer.last_chunk_checksum) {
      return true;
    }
    set_error(error, error_size, "raw appearance duplicate chunk does not match");
    return false;
  }
  starts_file = chunk_index == 0;
  if (starts_file) {
    if (g_transfer.file_active || g_transfer.entry_count >= PET_P4_RAW_MAX_FILES) {
      set_error(error, error_size, "raw appearance file sequence is invalid");
      return false;
    }
    g_transfer.cursor = align_up(g_transfer.cursor, 4U);
    g_transfer.file_active = true;
    snprintf(g_transfer.file_path, sizeof(g_transfer.file_path), "%s", logical_path);
    g_transfer.file_offset = g_transfer.cursor;
    g_transfer.file_size = 0;
    g_transfer.file_checksum = 0xcbf29ce484222325ULL;
    g_transfer.next_chunk_index = 0;
    g_transfer.last_chunk_valid = false;
  } else if (!g_transfer.file_active || strcmp(g_transfer.file_path, logical_path) != 0) {
    set_error(error, error_size, "raw appearance chunk path is out of sequence");
    return false;
  }
  if (chunk_index != g_transfer.next_chunk_index) {
    snprintf(
      error,
      error_size,
      "raw appearance chunk out of sequence expected=%u actual=%u",
      (unsigned int) g_transfer.next_chunk_index,
      (unsigned int) chunk_index
    );
    return false;
  }
  if ((unsigned long long) g_transfer.cursor + data_len > partition->size) {
    set_error(error, error_size, "raw appearance partition is full");
    return false;
  }
  if (esp_partition_write(partition, g_transfer.cursor, data, data_len) != ESP_OK) {
    set_error(error, error_size, "raw appearance partition write failed");
    return false;
  }
  g_transfer.file_checksum = fnv1a64_update(g_transfer.file_checksum, data, data_len);
  g_transfer.file_size += (unsigned long long) data_len;
  g_transfer.cursor += (uint32_t) data_len;
  g_transfer.last_chunk_valid = true;
  g_transfer.last_chunk_index = chunk_index;
  g_transfer.last_chunk_size = (uint32_t) data_len;
  g_transfer.last_chunk_checksum = chunk_checksum;
  g_transfer.next_chunk_index = chunk_index + 1U;
  return true;
}

bool pet_p4_raw_assets_finish_file(
  const char *logical_path,
  unsigned long long expected_size,
  const char *expected_checksum,
  char *error,
  size_t error_size
) {
  char actual_checksum[17];
  pet_p4_raw_entry_t *entry;
  if (!g_transfer.prepared || !g_transfer.file_active || !logical_path
      || strcmp(g_transfer.file_path, logical_path) != 0) {
    set_error(error, error_size, "missing raw appearance file");
    return false;
  }
  snprintf(actual_checksum, sizeof(actual_checksum), "%016llx", g_transfer.file_checksum);
  if (expected_size != g_transfer.file_size
      || (expected_checksum && expected_checksum[0]
          && strcmp(expected_checksum, actual_checksum) != 0)) {
    snprintf(
      error,
      error_size,
      "checksum mismatch expected_size=%llu actual_size=%llu expected=%s actual=%s",
      expected_size,
      g_transfer.file_size,
      expected_checksum ? expected_checksum : "",
      actual_checksum
    );
    return false;
  }
  entry = &g_transfer.entries[g_transfer.entry_count++];
  entry->path_hash = path_hash(logical_path);
  entry->offset = g_transfer.file_offset;
  entry->size = (uint32_t) g_transfer.file_size;
  entry->checksum = g_transfer.file_checksum;
  g_transfer.file_active = false;
  g_transfer.file_path[0] = '\0';
  g_transfer.file_offset = 0;
  g_transfer.file_size = 0;
  g_transfer.file_checksum = 0xcbf29ce484222325ULL;
  g_transfer.next_chunk_index = 0;
  g_transfer.last_chunk_valid = false;
  g_transfer.last_chunk_index = 0;
  g_transfer.last_chunk_size = 0;
  g_transfer.last_chunk_checksum = 0;
  return true;
}

bool pet_p4_raw_assets_commit(
  const char *pack_id,
  char *error,
  size_t error_size
) {
  const esp_partition_t *partition = raw_partition();
  unsigned char *header;
  if (!partition || !g_transfer.prepared || g_transfer.file_active
      || g_transfer.entry_count == 0 || !pack_id_is_valid(pack_id)) {
    set_error(error, error_size, "raw appearance commit state is invalid");
    return false;
  }
  header = (unsigned char *) malloc(PET_P4_RAW_HEADER_BYTES);
  if (!header) {
    set_error(error, error_size, "raw appearance header allocation failed");
    return false;
  }
  memset(header, 0xff, PET_P4_RAW_HEADER_BYTES);
  memcpy(header, PET_P4_RAW_MAGIC, sizeof(PET_P4_RAW_MAGIC));
  put_u32(header + 8U, PET_P4_RAW_HEADER_VERSION);
  put_u32(header + 12U, g_transfer.entry_count);
  put_u32(header + 16U, g_transfer.cursor);
  put_u32(header + 20U, 0U);
  memcpy(header + 24U, pack_id, PET_P4_RAW_PACK_ID_BYTES);
  for (unsigned int i = 0; i < g_transfer.entry_count; i += 1U) {
    unsigned char *encoded = header + PET_P4_RAW_HEADER_ENTRY_OFFSET
                           + i * PET_P4_RAW_HEADER_ENTRY_BYTES;
    put_u64(encoded, g_transfer.entries[i].path_hash);
    put_u32(encoded + 8U, g_transfer.entries[i].offset);
    put_u32(encoded + 12U, g_transfer.entries[i].size);
    put_u64(encoded + 16U, g_transfer.entries[i].checksum);
  }
  if (esp_partition_write(partition, 0, header, PET_P4_RAW_HEADER_BYTES) != ESP_OK) {
    free(header);
    set_error(error, error_size, "raw appearance header commit failed");
    return false;
  }
  free(header);
  pet_p4_raw_assets_reset_transfer();
  invalidate_cache(PET_P4_RAW_APPEARANCE_SLOT);
  return true;
}

static bool load_cache(int slot) {
  const esp_partition_t *partition = raw_partition_for_slot(slot);
  pet_p4_raw_cache_t *cache;
  unsigned char *header;
  uint32_t count;
  uint32_t data_end;
  if (slot < 0 || slot >= PET_P4_RAW_SLOT_COUNT) return false;
  cache = &g_caches[slot];
  if (cache->attempted) return cache->valid;
  cache->attempted = true;
  if (!partition) return false;
  header = (unsigned char *) malloc(PET_P4_RAW_HEADER_BYTES);
  if (!header) return false;
  if (esp_partition_read(partition, 0, header, PET_P4_RAW_HEADER_BYTES) != ESP_OK
      || memcmp(header, PET_P4_RAW_MAGIC, sizeof(PET_P4_RAW_MAGIC)) != 0
      || get_u32(header + 8U) != PET_P4_RAW_HEADER_VERSION) {
    free(header);
    return false;
  }
  count = get_u32(header + 12U);
  data_end = get_u32(header + 16U);
  memcpy(cache->pack_id, header + 24U, PET_P4_RAW_PACK_ID_BYTES);
  cache->pack_id[PET_P4_RAW_PACK_ID_BYTES] = '\0';
  if (count == 0 || count > PET_P4_RAW_MAX_FILES
      || data_end < PET_P4_RAW_HEADER_BYTES || data_end > partition->size
      || !pack_id_is_valid(cache->pack_id)) {
    free(header);
    return false;
  }
  for (unsigned int i = 0; i < count; i += 1U) {
    const unsigned char *encoded = header + PET_P4_RAW_HEADER_ENTRY_OFFSET
                                 + i * PET_P4_RAW_HEADER_ENTRY_BYTES;
    pet_p4_raw_entry_t *entry = &cache->entries[i];
    entry->path_hash = get_u64(encoded);
    entry->offset = get_u32(encoded + 8U);
    entry->size = get_u32(encoded + 12U);
    entry->checksum = get_u64(encoded + 16U);
    if (entry->offset < PET_P4_RAW_HEADER_BYTES || entry->size == 0
        || (unsigned long long) entry->offset + entry->size > data_end) {
      free(header);
      memset(cache, 0, sizeof(*cache));
      cache->attempted = true;
      return false;
    }
  }
  free(header);
  cache->entry_count = count;
  cache->data_end = data_end;
  cache->valid = true;
  return true;
}

bool pet_p4_raw_assets_committed_pack_id(char *out, size_t out_size) {
  return pet_p4_raw_assets_committed_pack_id_for_slot(
    PET_P4_RAW_APPEARANCE_SLOT,
    out,
    out_size
  );
}

bool pet_p4_raw_assets_committed_pack_id_for_slot(
  int slot,
  char *out,
  size_t out_size
) {
  if (!out || out_size < PET_P4_RAW_PACK_ID_BYTES + 1U || !load_cache(slot)) return false;
  snprintf(out, out_size, "%s", g_caches[slot].pack_id);
  return true;
}

bool pet_p4_raw_assets_read_all(
  const char *logical_path,
  unsigned char *out,
  size_t expected_size
) {
  return pet_p4_raw_assets_read_all_from_slot(
    PET_P4_RAW_APPEARANCE_SLOT,
    logical_path,
    out,
    expected_size
  );
}

bool pet_p4_raw_assets_read_all_from_slot(
  int slot,
  const char *logical_path,
  unsigned char *out,
  size_t expected_size
) {
  const esp_partition_t *partition = raw_partition_for_slot(slot);
  pet_p4_raw_cache_t *cache;
  unsigned long long hash;
  if (slot < 0 || slot >= PET_P4_RAW_SLOT_COUNT
      || !partition || !logical_path || !out || expected_size == 0
      || !load_cache(slot)) {
    return false;
  }
  cache = &g_caches[slot];
  hash = path_hash(logical_path);
  for (unsigned int i = 0; i < cache->entry_count; i += 1U) {
    const pet_p4_raw_entry_t *entry = &cache->entries[i];
    if (entry->path_hash != hash || entry->size != expected_size) continue;
    return esp_partition_read(partition, entry->offset, out, expected_size) == ESP_OK;
  }
  return false;
}
