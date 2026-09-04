#include "pet_p4_assets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#define PET_P4_ASSET_DEFAULT_WIDTH 640
#define PET_P4_ASSET_DEFAULT_HEIGHT 480
#define PET_P4_ASSET_DEFAULT_FPS 10
#define PET_P4_ASSET_MAX_JPEG_BYTES (512 * 1024)
#define PET_P4_ASSET_MAX_STREAM_BYTES (4 * 1024 * 1024)

static bool family_is_selftest(const char *family, const char *path) {
  return (family && strstr(family, "selftest")) || (path && strstr(path, "selftest"));
}

typedef struct {
  const char *cursor;
} pet_p4_json_cursor_t;

static void json_skip_space(pet_p4_json_cursor_t *json) {
  while (json && json->cursor && isspace((unsigned char) *json->cursor)) json->cursor += 1;
}

static bool json_take(pet_p4_json_cursor_t *json, char expected) {
  json_skip_space(json);
  if (!json || !json->cursor || *json->cursor != expected) return false;
  json->cursor += 1;
  return true;
}

static int json_hex_digit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static bool json_parse_string(
  pet_p4_json_cursor_t *json,
  char *output,
  size_t output_size
) {
  size_t used = 0;
  json_skip_space(json);
  if (!json || !json->cursor || *json->cursor != '"') return false;
  json->cursor += 1;
  while (*json->cursor && *json->cursor != '"') {
    unsigned char value = (unsigned char) *json->cursor++;
    if (value < 0x20) return false;
    if (value == '\\') {
      char escaped = *json->cursor++;
      if (!escaped) return false;
      if (escaped == '"' || escaped == '\\' || escaped == '/') {
        value = (unsigned char) escaped;
      } else if (escaped == 'b') {
        value = '\b';
      } else if (escaped == 'f') {
        value = '\f';
      } else if (escaped == 'n') {
        value = '\n';
      } else if (escaped == 'r') {
        value = '\r';
      } else if (escaped == 't') {
        value = '\t';
      } else if (escaped == 'u') {
        unsigned int codepoint = 0;
        for (int i = 0; i < 4; i += 1) {
          int digit = json_hex_digit(*json->cursor++);
          if (digit < 0) return false;
          codepoint = (codepoint << 4) | (unsigned int) digit;
        }
        value = codepoint <= 0x7fU ? (unsigned char) codepoint : '?';
      } else {
        return false;
      }
    }
    if (output && output_size > 0 && used + 1 < output_size) output[used] = (char) value;
    used += 1;
  }
  if (*json->cursor != '"') return false;
  json->cursor += 1;
  if (output && output_size > 0) {
    size_t terminator = used < output_size ? used : output_size - 1;
    output[terminator] = '\0';
  }
  return true;
}

static bool json_parse_u32(pet_p4_json_cursor_t *json, uint32_t *output) {
  uint64_t value = 0;
  bool has_digit = false;
  json_skip_space(json);
  if (!json || !json->cursor) return false;
  while (*json->cursor >= '0' && *json->cursor <= '9') {
    has_digit = true;
    value = value * 10U + (uint64_t) (*json->cursor - '0');
    if (value > UINT32_MAX) return false;
    json->cursor += 1;
  }
  if (!has_digit) return false;
  if (output) *output = (uint32_t) value;
  return true;
}

static bool json_skip_value(pet_p4_json_cursor_t *json, unsigned int depth);

static bool json_skip_number(pet_p4_json_cursor_t *json) {
  bool digits = false;
  json_skip_space(json);
  if (*json->cursor == '-') json->cursor += 1;
  while (isdigit((unsigned char) *json->cursor)) {
    digits = true;
    json->cursor += 1;
  }
  if (*json->cursor == '.') {
    json->cursor += 1;
    bool fraction_digits = false;
    while (isdigit((unsigned char) *json->cursor)) {
      fraction_digits = true;
      json->cursor += 1;
    }
    if (!fraction_digits) return false;
  }
  if (*json->cursor == 'e' || *json->cursor == 'E') {
    json->cursor += 1;
    if (*json->cursor == '+' || *json->cursor == '-') json->cursor += 1;
    bool exponent_digits = false;
    while (isdigit((unsigned char) *json->cursor)) {
      exponent_digits = true;
      json->cursor += 1;
    }
    if (!exponent_digits) return false;
  }
  return digits;
}

static bool json_skip_value(pet_p4_json_cursor_t *json, unsigned int depth) {
  if (!json || !json->cursor || depth > 8U) return false;
  json_skip_space(json);
  if (*json->cursor == '"') return json_parse_string(json, NULL, 0);
  if (*json->cursor == '{') {
    json->cursor += 1;
    json_skip_space(json);
    if (*json->cursor == '}') {
      json->cursor += 1;
      return true;
    }
    while (true) {
      if (!json_parse_string(json, NULL, 0) || !json_take(json, ':')
          || !json_skip_value(json, depth + 1U)) return false;
      json_skip_space(json);
      if (*json->cursor == '}') {
        json->cursor += 1;
        return true;
      }
      if (!json_take(json, ',')) return false;
    }
  }
  if (*json->cursor == '[') {
    json->cursor += 1;
    json_skip_space(json);
    if (*json->cursor == ']') {
      json->cursor += 1;
      return true;
    }
    while (true) {
      if (!json_skip_value(json, depth + 1U)) return false;
      json_skip_space(json);
      if (*json->cursor == ']') {
        json->cursor += 1;
        return true;
      }
      if (!json_take(json, ',')) return false;
    }
  }
  if (strncmp(json->cursor, "true", 4) == 0) {
    json->cursor += 4;
    return true;
  }
  if (strncmp(json->cursor, "false", 5) == 0) {
    json->cursor += 5;
    return true;
  }
  if (strncmp(json->cursor, "null", 4) == 0) {
    json->cursor += 4;
    return true;
  }
  return json_skip_number(json);
}

static bool json_parse_frame_sizes(
  pet_p4_json_cursor_t *json,
  uint32_t frame_sizes[PET_P4_ASSET_CATALOG_MAX_FRAMES],
  uint16_t *frame_count
) {
  uint16_t count = 0;
  if (!json_take(json, '[')) return false;
  json_skip_space(json);
  if (*json->cursor == ']') {
    json->cursor += 1;
    *frame_count = 0;
    return true;
  }
  while (true) {
    uint32_t size = 0;
    if (!json_parse_u32(json, &size) || size == 0 || size > PET_P4_ASSET_MAX_JPEG_BYTES
        || count >= PET_P4_ASSET_CATALOG_MAX_FRAMES) return false;
    frame_sizes[count++] = size;
    json_skip_space(json);
    if (*json->cursor == ']') {
      json->cursor += 1;
      *frame_count = count;
      return true;
    }
    if (!json_take(json, ',')) return false;
  }
}

static bool json_parse_family(
  pet_p4_json_cursor_t *json,
  pet_p4_asset_catalog_t *parsed
) {
  pet_p4_asset_entry_t candidate = {0};
  bool have_family = false;
  bool have_path = false;
  bool have_sizes = false;
  bool have_frames = false;
  bool have_stream_bytes = false;
  if (!json_take(json, '{')) return false;
  json_skip_space(json);
  while (*json->cursor != '}') {
    char key[24];
    if (!json_parse_string(json, key, sizeof(key)) || !json_take(json, ':')) return false;
    if (strcmp(key, "family") == 0) {
      have_family = json_parse_string(json, candidate.family, sizeof(candidate.family));
      if (!have_family) return false;
    } else if (strcmp(key, "path") == 0) {
      have_path = json_parse_string(json, candidate.path, sizeof(candidate.path));
      if (!have_path) return false;
    } else if (strcmp(key, "audioPath") == 0) {
      if (!json_parse_string(json, candidate.audio_path, sizeof(candidate.audio_path))) return false;
    } else if (strcmp(key, "fps") == 0) {
      uint32_t fps = 0;
      if (!json_parse_u32(json, &fps) || fps > UINT16_MAX) return false;
      candidate.fps = (uint16_t) fps;
    } else if (strcmp(key, "frameDurationMs") == 0) {
      uint32_t frame_duration_ms = 0;
      if (!json_parse_u32(json, &frame_duration_ms) || frame_duration_ms == 0) return false;
      candidate.frame_duration_ms = frame_duration_ms;
    } else if (strcmp(key, "durationMs") == 0) {
      uint32_t duration_ms = 0;
      if (!json_parse_u32(json, &duration_ms) || duration_ms == 0) return false;
      candidate.duration_ms = duration_ms;
    } else if (strcmp(key, "frames") == 0) {
      uint32_t frames = 0;
      if (!json_parse_u32(json, &frames) || frames == 0
          || frames > PET_P4_ASSET_CATALOG_MAX_FRAMES) return false;
      candidate.frames = (uint16_t) frames;
      have_frames = true;
    } else if (strcmp(key, "streamBytes") == 0) {
      uint32_t stream_bytes = 0;
      if (!json_parse_u32(json, &stream_bytes) || stream_bytes == 0
          || stream_bytes > PET_P4_ASSET_MAX_STREAM_BYTES) return false;
      candidate.stream_bytes = stream_bytes;
      have_stream_bytes = true;
    } else if (strcmp(key, "frameSizes") == 0) {
      uint16_t sized_frames = 0;
      have_sizes = json_parse_frame_sizes(json, candidate.frame_sizes, &sized_frames);
      if (!have_sizes) return false;
      if (have_frames && candidate.frames != sized_frames) return false;
      candidate.frames = sized_frames;
      have_frames = true;
    } else if (!json_skip_value(json, 1U)) {
      return false;
    }
    json_skip_space(json);
    if (*json->cursor == '}') break;
    if (!json_take(json, ',')) return false;
  }
  if (!json_take(json, '}')) return false;
  if (!have_family || !have_path || !candidate.family[0] || !candidate.path[0]
      || !have_frames || candidate.frames == 0
      || (!have_sizes && !have_stream_bytes)
      || family_is_selftest(candidate.family, candidate.path)) {
    return true;
  }
  if (candidate.audio_path[0]
      && (strncmp(candidate.audio_path, "p4/", 3) != 0
          || strstr(candidate.audio_path, "..")
          || strchr(candidate.audio_path, '\\'))) {
    candidate.audio_path[0] = '\0';
  }
  if (parsed->count < PET_P4_ASSET_CATALOG_MAX_FAMILIES) {
    parsed->entries[parsed->count++] = candidate;
  }
  return true;
}

static bool json_parse_families(
  pet_p4_json_cursor_t *json,
  pet_p4_asset_catalog_t *parsed
) {
  if (!json_take(json, '[')) return false;
  json_skip_space(json);
  if (*json->cursor == ']') {
    json->cursor += 1;
    return true;
  }
  while (true) {
    if (!json_parse_family(json, parsed)) return false;
    json_skip_space(json);
    if (*json->cursor == ']') {
      json->cursor += 1;
      return true;
    }
    if (!json_take(json, ',')) return false;
  }
}

void pet_p4_asset_catalog_init(pet_p4_asset_catalog_t *catalog) {
  if (!catalog) return;
  memset(catalog, 0, sizeof(*catalog));
  catalog->width = PET_P4_ASSET_DEFAULT_WIDTH;
  catalog->height = PET_P4_ASSET_DEFAULT_HEIGHT;
  catalog->fps = PET_P4_ASSET_DEFAULT_FPS;
  catalog->codec = PET_P4_ASSET_CODEC_MJPEG;
}

bool pet_p4_asset_catalog_parse(pet_p4_asset_catalog_t *catalog, const char *manifest_json) {
  pet_p4_asset_catalog_t *parsed;
  pet_p4_json_cursor_t json = {.cursor = manifest_json};
  bool have_families = false;
  bool have_format = false;
  if (!catalog || !manifest_json || !manifest_json[0]) return false;
  parsed = (pet_p4_asset_catalog_t *) heap_caps_calloc(
    1,
    sizeof(*parsed),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
  if (!parsed) return false;
  pet_p4_asset_catalog_init(parsed);
  if (!json_take(&json, '{')) goto parse_failed;
  json_skip_space(&json);
  while (*json.cursor != '}') {
    char key[24];
    if (!json_parse_string(&json, key, sizeof(key)) || !json_take(&json, ':')) goto parse_failed;
    if (strcmp(key, "format") == 0) {
      char format[32];
      if (!json_parse_string(&json, format, sizeof(format))) goto parse_failed;
      if (strcmp(format, "p4-mjpeg-v1") == 0) {
        parsed->codec = PET_P4_ASSET_CODEC_MJPEG;
      } else if (strcmp(format, "p4-h264-v1") == 0) {
        parsed->codec = PET_P4_ASSET_CODEC_H264;
      } else {
        goto parse_failed;
      }
      have_format = true;
    } else if (strcmp(key, "width") == 0 || strcmp(key, "height") == 0 || strcmp(key, "fps") == 0) {
      uint32_t value = 0;
      if (!json_parse_u32(&json, &value) || value == 0 || value > UINT16_MAX) goto parse_failed;
      if (strcmp(key, "width") == 0) parsed->width = (uint16_t) value;
      else if (strcmp(key, "height") == 0) parsed->height = (uint16_t) value;
      else parsed->fps = (uint16_t) value;
    } else if (strcmp(key, "families") == 0) {
      if (!json_parse_families(&json, parsed)) goto parse_failed;
      have_families = true;
    } else if (!json_skip_value(&json, 1U)) {
      goto parse_failed;
    }
    json_skip_space(&json);
    if (*json.cursor == '}') break;
    if (!json_take(&json, ',')) goto parse_failed;
  }
  if (!json_take(&json, '}')) goto parse_failed;
  json_skip_space(&json);
  if (*json.cursor != '\0' || !have_format || !have_families || parsed->count == 0) {
    goto parse_failed;
  }
  for (uint16_t i = 0; i < parsed->count; i += 1) {
    parsed->entries[i].width = parsed->width;
    parsed->entries[i].height = parsed->height;
    pet_p4_asset_entry_t *entry = &parsed->entries[i];
    uint64_t fallback_duration_ms;
    entry->codec = parsed->codec;
    if (entry->codec == PET_P4_ASSET_CODEC_H264) {
      if (entry->stream_bytes == 0) goto parse_failed;
    } else {
      uint64_t stream_bytes = 0;
      if (entry->frame_sizes[0] == 0) goto parse_failed;
      for (uint16_t frame = 0; frame < entry->frames; frame += 1) {
        stream_bytes += entry->frame_sizes[frame];
      }
      if (stream_bytes == 0 || stream_bytes > UINT32_MAX) goto parse_failed;
      entry->stream_bytes = (uint32_t) stream_bytes;
    }
    if (entry->fps == 0) entry->fps = parsed->fps;
    if (entry->duration_ms == 0) {
      fallback_duration_ms = entry->frame_duration_ms > 0
        ? (uint64_t) entry->frames * entry->frame_duration_ms
        : ((uint64_t) entry->frames * 1000ULL) / entry->fps;
      if (fallback_duration_ms == 0 || fallback_duration_ms > UINT32_MAX) goto parse_failed;
      entry->duration_ms = (uint32_t) fallback_duration_ms;
    }
    if (entry->frame_duration_ms == 0) {
      entry->frame_duration_ms = (uint32_t) (
        ((uint64_t) entry->duration_ms + entry->frames - 1U) / entry->frames
      );
    }
  }
  *catalog = *parsed;
  heap_caps_free(parsed);
  return true;

parse_failed:
  if (parsed) {
    heap_caps_free(parsed);
  }
  return false;
}

int pet_p4_asset_catalog_find_exact(const pet_p4_asset_catalog_t *catalog, const char *family) {
  if (!catalog || !family || !family[0]) return -1;
  for (int i = 0; i < catalog->count; i += 1) {
    if (strcmp(catalog->entries[i].family, family) == 0) return i;
  }
  return -1;
}

int pet_p4_asset_catalog_count_prefix(const pet_p4_asset_catalog_t *catalog, const char *prefix) {
  int count = 0;
  size_t prefix_len;
  if (!catalog || !prefix || !prefix[0]) return 0;
  prefix_len = strlen(prefix);
  for (int i = 0; i < catalog->count; i += 1) {
    if (strncmp(catalog->entries[i].family, prefix, prefix_len) == 0) count += 1;
  }
  return count;
}

int pet_p4_asset_catalog_nth_prefix(const pet_p4_asset_catalog_t *catalog, const char *prefix, int nth) {
  size_t prefix_len;
  if (!catalog || !prefix || !prefix[0] || nth < 0) return -1;
  prefix_len = strlen(prefix);
  for (int i = 0; i < catalog->count; i += 1) {
    if (strncmp(catalog->entries[i].family, prefix, prefix_len) != 0) continue;
    if (nth == 0) return i;
    nth -= 1;
  }
  return -1;
}
