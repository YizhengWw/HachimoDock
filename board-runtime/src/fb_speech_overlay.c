#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

#include "runtime_common.h"

#define BR_FONT_CANDIDATES 4
#define BR_MAX_GLYPHS 512
#define BR_MAX_LINES 8
#define BR_MAX_LINE_BYTES 1024

typedef struct {
  char root_dir[BR_MAX_PATH];
  char fb_path[BR_MAX_PATH];
  char state_path[BR_MAX_PATH];
  char event_path[BR_MAX_PATH];
  char speech_path[BR_MAX_PATH];
  char debug_speech_path[BR_MAX_PATH];
  char active_path[BR_MAX_PATH];
  char screen_page_path[BR_MAX_PATH];
  char stats_display_path[BR_MAX_PATH];
  char font_candidates[BR_FONT_CANDIDATES][BR_MAX_PATH];
  double poll_seconds;
  double redraw_seconds;
  double hold_seconds;
  double scale;
  int max_lines;
  int padding_x;
  int padding_y;
  int line_gap;
  int tracking;
  int card_margin_x;
  int card_bottom;
  int card_max_width;
  int scrim_top_padding;
  int scrim_bottom_padding;
  int scrim_side_padding;
  int scrim_top_alpha;
  int scrim_bottom_alpha;
} br_overlay_config;

typedef struct {
  uint32_t codepoint;
  int width;
  unsigned char data[32];
  int size;
  bool present;
} br_glyph;

typedef struct {
  br_glyph glyphs[BR_MAX_GLYPHS];
  size_t count;
} br_glyph_cache;

typedef struct {
  unsigned char *data;
  int width;
  int height;
  int stride;
} br_framebuffer;

typedef struct {
  char text[BR_MAX_LINE_BYTES];
  int width;
} br_line;

typedef struct {
  char agent[64];
  char eyebrow[96];
  char lunch[32];
  char headline[160];
  char metric_title[96];
  char metric_value[64];
  char metric_unit[32];
  char alerts[16];
  char completed[16];
  char breakdown[160];
  char sources[160];
} br_stats_dashboard_model;

typedef struct {
  char title[64];        /* maxBytes 60 + 4 safety */
  char eyebrow[96];      /* maxBytes 90 + 6 safety */
  char headline[160];    /* maxBytes 156 + 4 */
  char metric_label[96]; /* maxBytes 90 */
  char metric_value[64]; /* maxBytes 60 */
  char metric_unit[32];  /* maxBytes 30 */
  char badge[16];        /* maxBytes 12 */
  char note[160];        /* maxBytes 156 */
  char footer[160];      /* maxBytes 156 */
  /* progress: "<0-100>:<label>" e.g., "75:今日进度"; empty = no bar */
  char progress[64];
} br_component_dashboard_model;

static void br_overlay_logf(const char *format, ...) {
  va_list args;
  fprintf(stdout, "[fb-speech] ");
  va_start(args, format);
  vfprintf(stdout, format, args);
  va_end(args);
  fputc('\n', stdout);
  fflush(stdout);
}

static void br_overlay_load_config(br_overlay_config *config, const char *root_dir) {
  memset(config, 0, sizeof(*config));
  br_normalize_text(root_dir, ".", config->root_dir, sizeof(config->root_dir));
  br_normalize_text(getenv("PET_CLAW_FB_PATH"), "/dev/fb0", config->fb_path, sizeof(config->fb_path));
  snprintf(config->state_path, sizeof(config->state_path), "%s/.current-state", config->root_dir);
  snprintf(config->event_path, sizeof(config->event_path), "%s/.current-event", config->root_dir);
  snprintf(config->speech_path, sizeof(config->speech_path), "%s/.current-speech", config->root_dir);
  snprintf(config->debug_speech_path, sizeof(config->debug_speech_path), "%s/.current-debug-speech", config->root_dir);
  snprintf(config->active_path, sizeof(config->active_path), "%s/.speech-overlay-active", config->root_dir);
  snprintf(config->screen_page_path, sizeof(config->screen_page_path), "%s/.screen-page", config->root_dir);
  snprintf(config->stats_display_path, sizeof(config->stats_display_path), "%s/.stats-display", config->root_dir);

  br_normalize_text(getenv("PET_CLAW_FB_SPEECH_FONT"), "", config->font_candidates[0], sizeof(config->font_candidates[0]));
  snprintf(config->font_candidates[1], sizeof(config->font_candidates[1]), "%s/unifont-17.0.04.hex.gz", config->root_dir);
  snprintf(config->font_candidates[2], sizeof(config->font_candidates[2]), "%s/unifont.hex.gz", config->root_dir);
  snprintf(config->font_candidates[3], sizeof(config->font_candidates[3]), "%s/unifont.hex", config->root_dir);

  config->poll_seconds = getenv("PET_CLAW_FB_SPEECH_POLL_SECONDS") ? atof(getenv("PET_CLAW_FB_SPEECH_POLL_SECONDS")) : 0.25;
  config->redraw_seconds = getenv("PET_CLAW_FB_SPEECH_REDRAW_SECONDS") ? atof(getenv("PET_CLAW_FB_SPEECH_REDRAW_SECONDS")) : 0.5;
  config->hold_seconds = getenv("PET_CLAW_FB_SPEECH_HOLD_SECONDS") ? atof(getenv("PET_CLAW_FB_SPEECH_HOLD_SECONDS")) : 30.0;
  config->scale = getenv("PET_CLAW_FB_SPEECH_SCALE") ? atof(getenv("PET_CLAW_FB_SPEECH_SCALE")) : 2.0;
  if (config->scale < 1.0) config->scale = 1.0;
  config->max_lines = getenv("PET_CLAW_FB_SPEECH_MAX_LINES") ? atoi(getenv("PET_CLAW_FB_SPEECH_MAX_LINES")) : 3;
  if (config->max_lines < 1) config->max_lines = 1;
  if (config->max_lines > BR_MAX_LINES) config->max_lines = BR_MAX_LINES;
  config->padding_x = getenv("PET_CLAW_FB_SPEECH_PADDING_X") ? atoi(getenv("PET_CLAW_FB_SPEECH_PADDING_X")) : 22;
  config->padding_y = getenv("PET_CLAW_FB_SPEECH_PADDING_Y") ? atoi(getenv("PET_CLAW_FB_SPEECH_PADDING_Y")) : 12;
  config->line_gap = getenv("PET_CLAW_FB_SPEECH_LINE_GAP") ? atoi(getenv("PET_CLAW_FB_SPEECH_LINE_GAP")) : 5;
  config->tracking = getenv("PET_CLAW_FB_SPEECH_TRACKING") ? atoi(getenv("PET_CLAW_FB_SPEECH_TRACKING")) : 3;
  config->card_margin_x = getenv("PET_CLAW_FB_SPEECH_MARGIN_X") ? atoi(getenv("PET_CLAW_FB_SPEECH_MARGIN_X")) : 20;
  config->card_bottom = getenv("PET_CLAW_FB_SPEECH_BOTTOM") ? atoi(getenv("PET_CLAW_FB_SPEECH_BOTTOM")) : 20;
  config->card_max_width = getenv("PET_CLAW_FB_SPEECH_MAX_WIDTH") ? atoi(getenv("PET_CLAW_FB_SPEECH_MAX_WIDTH")) : 760;
  config->scrim_top_padding = getenv("PET_CLAW_FB_SPEECH_SCRIM_TOP_PADDING") ? atoi(getenv("PET_CLAW_FB_SPEECH_SCRIM_TOP_PADDING")) : 84;
  config->scrim_bottom_padding = getenv("PET_CLAW_FB_SPEECH_SCRIM_BOTTOM_PADDING") ? atoi(getenv("PET_CLAW_FB_SPEECH_SCRIM_BOTTOM_PADDING")) : 36;
  config->scrim_side_padding = getenv("PET_CLAW_FB_SPEECH_SCRIM_SIDE_PADDING") ? atoi(getenv("PET_CLAW_FB_SPEECH_SCRIM_SIDE_PADDING")) : 80;
  config->scrim_top_alpha = getenv("PET_CLAW_FB_SPEECH_SCRIM_TOP_ALPHA") ? atoi(getenv("PET_CLAW_FB_SPEECH_SCRIM_TOP_ALPHA")) : 0;
  config->scrim_bottom_alpha = getenv("PET_CLAW_FB_SPEECH_SCRIM_BOTTOM_ALPHA") ? atoi(getenv("PET_CLAW_FB_SPEECH_SCRIM_BOTTOM_ALPHA")) : 108;
}

static bool br_overlay_read_text(const char *path, char *output, size_t output_size) {
  if (!br_read_text_file(path, output, output_size)) {
    output[0] = '\0';
    return false;
  }
  br_trim(output);
  return output[0] != '\0';
}

static int br_overlay_read_int(const char *path, int fallback) {
  char buffer[64];
  if (!br_overlay_read_text(path, buffer, sizeof(buffer))) {
    return fallback;
  }
  return atoi(buffer);
}

static void br_overlay_read_display_size(br_framebuffer *fb) {
  char buffer[128];
  fb->width = 800;
  fb->height = 960;
  fb->stride = fb->width * 4;
  if (br_overlay_read_text("/sys/class/graphics/fb0/virtual_size", buffer, sizeof(buffer))) {
    int width = 0;
    int height = 0;
    if (sscanf(buffer, "%d,%d", &width, &height) == 2) {
      if (width > 0) fb->width = width;
      if (height > 0) fb->height = height;
    } else if (sscanf(buffer, "%d", &width) == 1 && width > 0) {
      fb->width = width;
    }
  }
  if (br_overlay_read_text("/sys/class/graphics/fb0/modes", buffer, sizeof(buffer))) {
    int width = 0;
    int height = 0;
    char *x = strchr(buffer, 'x');
    if (x) {
      width = atoi(strrchr(buffer, ':') ? strrchr(buffer, ':') + 1 : buffer);
      height = atoi(x + 1);
      if (width > 0) fb->width = width;
      if (height > 0) fb->height = height;
    }
  }
  fb->stride = br_overlay_read_int("/sys/class/graphics/fb0/stride", fb->width * 4);
}

static bool br_overlay_framebuffer_supported(void) {
  return br_overlay_read_int("/sys/class/graphics/fb0/bits_per_pixel", 0) == 32;
}

static bool br_overlay_find_font(const br_overlay_config *config, char *path, size_t path_size) {
  struct stat st;
  for (int i = 0; i < BR_FONT_CANDIDATES; i += 1) {
    if (!config->font_candidates[i][0]) {
      continue;
    }
    if (stat(config->font_candidates[i], &st) == 0) {
      return br_normalize_text(config->font_candidates[i], "", path, path_size);
    }
  }
  return false;
}

static bool br_overlay_ends_with(const char *text, const char *suffix) {
  size_t text_length = strlen(text);
  size_t suffix_length = strlen(suffix);
  return text_length >= suffix_length && strcmp(text + text_length - suffix_length, suffix) == 0;
}

static bool br_utf8_next(const char *text, size_t length, size_t *index, uint32_t *codepoint, size_t *span) {
  unsigned char ch;
  if (!text || *index >= length) {
    return false;
  }
  ch = (unsigned char) text[*index];
  if (ch < 0x80U) {
    *codepoint = ch;
    *span = 1;
  } else if ((ch & 0xe0U) == 0xc0U && *index + 1 < length) {
    *codepoint = ((uint32_t) (ch & 0x1fU) << 6U) | ((uint32_t) text[*index + 1] & 0x3fU);
    *span = 2;
  } else if ((ch & 0xf0U) == 0xe0U && *index + 2 < length) {
    *codepoint = ((uint32_t) (ch & 0x0fU) << 12U) |
                 (((uint32_t) text[*index + 1] & 0x3fU) << 6U) |
                 ((uint32_t) text[*index + 2] & 0x3fU);
    *span = 3;
  } else if ((ch & 0xf8U) == 0xf0U && *index + 3 < length) {
    *codepoint = ((uint32_t) (ch & 0x07U) << 18U) |
                 (((uint32_t) text[*index + 1] & 0x3fU) << 12U) |
                 (((uint32_t) text[*index + 2] & 0x3fU) << 6U) |
                 ((uint32_t) text[*index + 3] & 0x3fU);
    *span = 4;
  } else {
    *codepoint = '?';
    *span = 1;
  }
  *index += *span;
  return true;
}

static bool br_glyph_cache_has(const br_glyph_cache *cache, uint32_t codepoint) {
  for (size_t i = 0; i < cache->count; i += 1) {
    if (cache->glyphs[i].present && cache->glyphs[i].codepoint == codepoint) {
      return true;
    }
  }
  return false;
}

static bool br_glyph_cache_wants(const br_glyph_cache *cache, uint32_t codepoint) {
  for (size_t i = 0; i < cache->count; i += 1) {
    if (cache->glyphs[i].codepoint == codepoint) {
      return true;
    }
  }
  return false;
}

static br_glyph *br_glyph_cache_find(br_glyph_cache *cache, uint32_t codepoint) {
  for (size_t i = 0; i < cache->count; i += 1) {
    if (cache->glyphs[i].present && cache->glyphs[i].codepoint == codepoint) {
      return &cache->glyphs[i];
    }
  }
  return NULL;
}

static void br_glyph_cache_want_text(br_glyph_cache *cache, const char *text) {
  size_t index = 0;
  size_t length = strlen(text);
  while (index < length && cache->count + 1 < BR_MAX_GLYPHS) {
    uint32_t codepoint;
    size_t span;
    if (!br_utf8_next(text, length, &index, &codepoint, &span)) {
      break;
    }
    if (!br_glyph_cache_wants(cache, codepoint)) {
      cache->glyphs[cache->count].codepoint = codepoint;
      cache->glyphs[cache->count].present = false;
      cache->count += 1;
    }
  }
}

static void br_glyph_cache_add(br_glyph_cache *cache, uint32_t codepoint, const unsigned char *data, int size) {
  for (size_t i = 0; i < cache->count; i += 1) {
    if (cache->glyphs[i].codepoint == codepoint) {
      cache->glyphs[i].width = size == 16 ? 8 : 16;
      cache->glyphs[i].size = size;
      memcpy(cache->glyphs[i].data, data, (size_t) size);
      cache->glyphs[i].present = true;
      return;
    }
  }
  if (cache->count < BR_MAX_GLYPHS) {
    cache->glyphs[cache->count].codepoint = codepoint;
    cache->glyphs[cache->count].width = size == 16 ? 8 : 16;
    cache->glyphs[cache->count].size = size;
    memcpy(cache->glyphs[cache->count].data, data, (size_t) size);
    cache->glyphs[cache->count].present = true;
    cache->count += 1;
  }
}

static bool br_overlay_load_glyphs(const char *font_path, const char *text, br_glyph_cache *cache) {
  char line[256];
  bool gzip = br_overlay_ends_with(font_path, ".gz");
  memset(cache, 0, sizeof(*cache));
  br_glyph_cache_want_text(cache, text);
  if (!br_glyph_cache_wants(cache, '?') && cache->count < BR_MAX_GLYPHS) {
    cache->glyphs[cache->count].codepoint = '?';
    cache->count += 1;
  }

  if (gzip) {
    gzFile file = gzopen(font_path, "rb");
    if (!file) {
      return false;
    }
    while (gzgets(file, line, sizeof(line))) {
      char *colon = strchr(line, ':');
      if (!colon) {
        continue;
      }
      *colon = '\0';
      uint32_t codepoint = (uint32_t) strtoul(line, NULL, 16);
      if (!br_glyph_cache_wants(cache, codepoint)) {
        continue;
      }
      char *hex = colon + 1;
      hex[strcspn(hex, "\r\n")] = '\0';
      size_t hex_length = strlen(hex);
      if (hex_length != 32 && hex_length != 64) {
        continue;
      }
      unsigned char data[32];
      size_t byte_count = hex_length / 2;
      for (size_t i = 0; i < byte_count; i += 1) {
        unsigned int value = 0;
        sscanf(hex + i * 2, "%2x", &value);
        data[i] = (unsigned char) value;
      }
      br_glyph_cache_add(cache, codepoint, data, (int) byte_count);
    }
    gzclose(file);
  } else {
    FILE *file = fopen(font_path, "rb");
    if (!file) {
      return false;
    }
    while (fgets(line, sizeof(line), file)) {
      char *colon = strchr(line, ':');
      if (!colon) {
        continue;
      }
      *colon = '\0';
      uint32_t codepoint = (uint32_t) strtoul(line, NULL, 16);
      if (!br_glyph_cache_wants(cache, codepoint)) {
        continue;
      }
      char *hex = colon + 1;
      hex[strcspn(hex, "\r\n")] = '\0';
      size_t hex_length = strlen(hex);
      if (hex_length != 32 && hex_length != 64) {
        continue;
      }
      unsigned char data[32];
      size_t byte_count = hex_length / 2;
      for (size_t i = 0; i < byte_count; i += 1) {
        unsigned int value = 0;
        sscanf(hex + i * 2, "%2x", &value);
        data[i] = (unsigned char) value;
      }
      br_glyph_cache_add(cache, codepoint, data, (int) byte_count);
    }
    fclose(file);
  }
  return true;
}

static br_glyph *br_overlay_glyph_for(br_glyph_cache *cache, uint32_t codepoint) {
  br_glyph *glyph = br_glyph_cache_find(cache, codepoint);
  if (glyph && glyph->present) {
    return glyph;
  }
  glyph = br_glyph_cache_find(cache, '?');
  return (glyph && glyph->present) ? glyph : NULL;
}

static int br_overlay_glyph_width(const br_overlay_config *config, br_glyph_cache *cache, uint32_t codepoint) {
  br_glyph *glyph = br_overlay_glyph_for(cache, codepoint);
  int width = glyph ? glyph->width : 8;
  return (int) lround((double) width * config->scale) + config->tracking;
}

static int br_overlay_wrap_text(
  const br_overlay_config *config,
  br_glyph_cache *cache,
  const char *text,
  int max_width,
  br_line *lines,
  int max_lines
) {
  size_t index = 0;
  size_t length = strlen(text);
  int line_count = 0;
  lines[0].text[0] = '\0';
  lines[0].width = 0;

  while (index < length && line_count < max_lines) {
    size_t start = index;
    uint32_t codepoint;
    size_t span;
    if (!br_utf8_next(text, length, &index, &codepoint, &span)) {
      break;
    }
    if (codepoint == '\r') {
      continue;
    }
    if (codepoint == '\n') {
      line_count += 1;
      if (line_count >= max_lines) {
        break;
      }
      lines[line_count].text[0] = '\0';
      lines[line_count].width = 0;
      continue;
    }
    int glyph_width = br_overlay_glyph_width(config, cache, codepoint);
    if (lines[line_count].text[0] != '\0' && lines[line_count].width + glyph_width > max_width) {
      line_count += 1;
      if (line_count >= max_lines) {
        break;
      }
      lines[line_count].text[0] = '\0';
      lines[line_count].width = 0;
    }
    size_t current_length = strlen(lines[line_count].text);
    if (current_length + span + 1 < sizeof(lines[line_count].text)) {
      memcpy(lines[line_count].text + current_length, text + start, span);
      lines[line_count].text[current_length + span] = '\0';
      lines[line_count].width += glyph_width;
    }
  }
  if (lines[line_count].text[0] != '\0') {
    line_count += 1;
  }
  return line_count;
}

static uint32_t br_pack_pixel(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha) {
  return ((uint32_t) alpha << 24U) | ((uint32_t) red << 16U) | ((uint32_t) green << 8U) | (uint32_t) blue;
}

static void br_set_pixel(br_framebuffer *fb, int x, int y, uint32_t pixel) {
  if (!fb || !fb->data || x < 0 || y < 0 || x >= fb->width || y >= fb->height) {
    return;
  }
  unsigned char *pos = fb->data + (size_t) y * (size_t) fb->stride + (size_t) x * 4U;
  pos[0] = (unsigned char) (pixel & 0xffU);
  pos[1] = (unsigned char) ((pixel >> 8U) & 0xffU);
  pos[2] = (unsigned char) ((pixel >> 16U) & 0xffU);
  pos[3] = (unsigned char) ((pixel >> 24U) & 0xffU);
}

static void br_fill_rect(br_framebuffer *fb, int x0, int y0, int width, int height, uint32_t pixel) {
  int x1 = x0 + width;
  int y1 = y0 + height;
  for (int y = y0; y < y1; y += 1) {
    for (int x = x0; x < x1; x += 1) {
      br_set_pixel(fb, x, y, pixel);
    }
  }
}

static void br_fill_rounded_rect(br_framebuffer *fb, int x, int y, int width, int height, int radius, uint32_t pixel) {
  if (!fb || width <= 0 || height <= 0) return;
  if (radius < 0) radius = 0;
  if (radius * 2 > width) radius = width / 2;
  if (radius * 2 > height) radius = height / 2;
  if (radius == 0) {
    br_fill_rect(fb, x, y, width, height, pixel);
    return;
  }

  br_fill_rect(fb, x, y + radius, width, height - radius * 2, pixel);
  for (int ry = 0; ry < radius; ry += 1) {
    double d = (double) (radius - ry);
    int inset = radius - (int) lround(sqrt((double) (radius * radius) - d * d));
    if (inset < 0) inset = 0;
    br_fill_rect(fb, x + inset, y + ry, width - inset * 2, 1, pixel);
    br_fill_rect(fb, x + inset, y + height - 1 - ry, width - inset * 2, 1, pixel);
  }
}

static void br_fill_circle(br_framebuffer *fb, int cx, int cy, int radius, uint32_t pixel) {
  if (!fb || radius <= 0) return;
  int r2 = radius * radius;
  for (int y = -radius; y <= radius; y += 1) {
    for (int x = -radius; x <= radius; x += 1) {
      if (x * x + y * y <= r2) {
        br_set_pixel(fb, cx + x, cy + y, pixel);
      }
    }
  }
}

static void br_stroke_dashed_hline(br_framebuffer *fb, int x, int y, int width, int dash, int gap, uint32_t pixel) {
  if (!fb || width <= 0 || dash <= 0) return;
  if (gap < 0) gap = 0;
  int end = x + width;
  for (int cursor = x; cursor < end; cursor += dash + gap) {
    int segment = dash;
    if (cursor + segment > end) segment = end - cursor;
    br_fill_rect(fb, cursor, y, segment, 1, pixel);
  }
}

static void br_fill_gradient(
  br_framebuffer *fb,
  int x0,
  int y0,
  int width,
  int height,
  int alpha_top,
  int alpha_bottom
) {
  for (int offset = 0; offset < height; offset += 1) {
    int alpha = height <= 1
      ? alpha_bottom
      : alpha_top + (alpha_bottom - alpha_top) * offset / (height - 1);
    uint32_t pixel = br_pack_pixel(4, 6, 10, (unsigned char) alpha);
    br_fill_rect(fb, x0, y0 + offset, width, 1, pixel);
  }
}

static int br_draw_glyph(
  const br_overlay_config *config,
  br_framebuffer *fb,
  br_glyph_cache *cache,
  uint32_t codepoint,
  int x,
  int y,
  uint32_t pixel
) {
  br_glyph *glyph = br_overlay_glyph_for(cache, codepoint);
  int glyph_width = glyph ? glyph->width : 8;
  int scaled_width = (int) lround((double) glyph_width * config->scale);
  int scaled_height = (int) lround(16.0 * config->scale);

  if (!glyph) {
    return scaled_width + config->tracking;
  }

  for (int out_row = 0; out_row < scaled_height; out_row += 1) {
    int row = (int) floor((double) out_row / config->scale);
    if (row > 15) row = 15;
    uint16_t bits = glyph_width == 8
      ? glyph->data[row]
      : (uint16_t) ((glyph->data[row * 2] << 8U) | glyph->data[row * 2 + 1]);
    for (int out_col = 0; out_col < scaled_width; out_col += 1) {
      int col = (int) floor((double) out_col / config->scale);
      if (col >= glyph_width) col = glyph_width - 1;
      if ((bits & (1U << (glyph_width - 1 - col))) == 0U) {
        continue;
      }
      br_set_pixel(fb, x + out_col, y + out_row, pixel);
    }
  }
  return scaled_width + config->tracking;
}

static void br_draw_text(
  const br_overlay_config *config,
  br_framebuffer *fb,
  br_glyph_cache *cache,
  const char *text,
  int x,
  int y
) {
  size_t index = 0;
  size_t length = strlen(text);
  int cursor = x;
  while (index < length) {
    uint32_t codepoint;
    size_t span;
    uint32_t pixel;
    if (!br_utf8_next(text, length, &index, &codepoint, &span)) {
      break;
    }
    pixel = br_pack_pixel(255, 255, 255, 240);
    cursor += br_draw_glyph(config, fb, cache, codepoint, cursor, y, pixel);
  }
}

static int br_overlay_measure_text(
  const br_overlay_config *config,
  br_glyph_cache *cache,
  const char *text
) {
  size_t index = 0;
  size_t length = text ? strlen(text) : 0;
  int width = 0;
  while (index < length) {
    uint32_t codepoint;
    size_t span;
    if (!br_utf8_next(text, length, &index, &codepoint, &span)) break;
    width += br_overlay_glyph_width(config, cache, codepoint);
  }
  return width;
}

static void br_draw_text_color(
  const br_overlay_config *config,
  br_framebuffer *fb,
  br_glyph_cache *cache,
  const char *text,
  int x,
  int y,
  uint32_t pixel
) {
  size_t index = 0;
  size_t length = text ? strlen(text) : 0;
  int cursor = x;
  while (index < length) {
    uint32_t codepoint;
    size_t span;
    if (!br_utf8_next(text, length, &index, &codepoint, &span)) {
      break;
    }
    cursor += br_draw_glyph(config, fb, cache, codepoint, cursor, y, pixel);
  }
}

static void br_draw_scaled_text(
  const br_overlay_config *config,
  br_framebuffer *fb,
  br_glyph_cache *cache,
  const char *text,
  int x,
  int y,
  double scale,
  int tracking,
  uint32_t pixel
) {
  br_overlay_config scaled = *config;
  scaled.scale = scale;
  scaled.tracking = tracking;
  br_draw_text_color(&scaled, fb, cache, text, x, y, pixel);
}

static void br_draw_scaled_text_fit(
  const br_overlay_config *config,
  br_framebuffer *fb,
  br_glyph_cache *cache,
  const char *text,
  int x,
  int y,
  int max_width,
  double scale,
  int tracking,
  uint32_t pixel
) {
  br_overlay_config scaled = *config;
  scaled.scale = scale;
  scaled.tracking = tracking;
  while (scale > 1.0 && br_overlay_measure_text(&scaled, cache, text) > max_width) {
    scale -= 0.15;
    scaled.scale = scale;
  }
  br_draw_text_color(&scaled, fb, cache, text, x, y, pixel);
}

static void br_draw_scaled_text_center(
  const br_overlay_config *config,
  br_framebuffer *fb,
  br_glyph_cache *cache,
  const char *text,
  int center_x,
  int y,
  double scale,
  int tracking,
  uint32_t pixel
) {
  br_overlay_config scaled = *config;
  scaled.scale = scale;
  scaled.tracking = tracking;
  int width = br_overlay_measure_text(&scaled, cache, text);
  br_draw_text_color(&scaled, fb, cache, text, center_x - width / 2, y, pixel);
}

static void br_stats_dashboard_defaults(br_stats_dashboard_model *model) {
  memset(model, 0, sizeof(*model));
  snprintf(model->agent, sizeof(model->agent), "Codex");
  snprintf(model->eyebrow, sizeof(model->eyebrow), "等价于购买了");
  snprintf(model->lunch, sizeof(model->lunch), "0.0");
  snprintf(model->headline, sizeof(model->headline), "约 0.0 顿工作午餐");
  snprintf(model->metric_title, sizeof(model->metric_title), "今日累计 Token");
  snprintf(model->metric_value, sizeof(model->metric_value), "0");
  snprintf(model->metric_unit, sizeof(model->metric_unit), "TOKEN");
  snprintf(model->alerts, sizeof(model->alerts), "0");
  snprintf(model->completed, sizeof(model->completed), "0");
}

static void br_stats_dashboard_set_value(br_stats_dashboard_model *model, const char *key, const char *value) {
  if (!key || !value) return;
  if (strcmp(key, "agent") == 0) {
    br_normalize_text(value, "Codex", model->agent, sizeof(model->agent));
  } else if (strcmp(key, "eyebrow") == 0) {
    br_normalize_text(value, "等价于购买了", model->eyebrow, sizeof(model->eyebrow));
  } else if (strcmp(key, "lunch") == 0) {
    br_normalize_text(value, "0.0", model->lunch, sizeof(model->lunch));
  } else if (strcmp(key, "headline") == 0) {
    br_normalize_text(value, "约 0.0 顿工作午餐", model->headline, sizeof(model->headline));
  } else if (strcmp(key, "metricTitle") == 0) {
    br_normalize_text(value, "今日累计 Token", model->metric_title, sizeof(model->metric_title));
  } else if (strcmp(key, "metricValue") == 0) {
    br_normalize_text(value, "0", model->metric_value, sizeof(model->metric_value));
  } else if (strcmp(key, "metricUnit") == 0) {
    br_normalize_text(value, "TOKEN", model->metric_unit, sizeof(model->metric_unit));
  } else if (strcmp(key, "alerts") == 0) {
    br_normalize_text(value, "0", model->alerts, sizeof(model->alerts));
  } else if (strcmp(key, "completed") == 0) {
    br_normalize_text(value, "0", model->completed, sizeof(model->completed));
  } else if (strcmp(key, "breakdown") == 0) {
    br_normalize_text(value, "", model->breakdown, sizeof(model->breakdown));
  } else if (strcmp(key, "sources") == 0) {
    br_normalize_text(value, "", model->sources, sizeof(model->sources));
  }
}

static bool br_stats_dashboard_parse(const char *text, br_stats_dashboard_model *model) {
  if (!text || strncmp(text, "STATS_DASHBOARD_V1", 18) != 0) {
    return false;
  }
  br_stats_dashboard_defaults(model);

  const char *cursor = text;
  while (*cursor) {
    const char *line_end = strchr(cursor, '\n');
    size_t line_len = line_end ? (size_t) (line_end - cursor) : strlen(cursor);
    if (line_len > 0 && line_len < 256) {
      char line[256];
      memcpy(line, cursor, line_len);
      line[line_len] = '\0';
      char *eq = strchr(line, '=');
      if (eq) {
        *eq = '\0';
        br_stats_dashboard_set_value(model, line, eq + 1);
      }
    }
    if (!line_end) break;
    cursor = line_end + 1;
  }
  return true;
}

static void br_component_dashboard_defaults(br_component_dashboard_model *model) {
  memset(model, 0, sizeof(*model));
  snprintf(model->title, sizeof(model->title), "petAgent");
  snprintf(model->eyebrow, sizeof(model->eyebrow), "");
  snprintf(model->headline, sizeof(model->headline), "");
  snprintf(model->metric_label, sizeof(model->metric_label), "");
  snprintf(model->metric_value, sizeof(model->metric_value), "");
  snprintf(model->metric_unit, sizeof(model->metric_unit), "");
  snprintf(model->badge, sizeof(model->badge), "");
  snprintf(model->note, sizeof(model->note), "");
  snprintf(model->footer, sizeof(model->footer), "");
}

static void br_component_dashboard_set_value(br_component_dashboard_model *model, const char *key, const char *value) {
  if (!key || !value) return;
  if (strcmp(key, "title") == 0) {
    br_normalize_text(value, "petAgent", model->title, sizeof(model->title));
  } else if (strcmp(key, "eyebrow") == 0) {
    br_normalize_text(value, "", model->eyebrow, sizeof(model->eyebrow));
  } else if (strcmp(key, "headline") == 0) {
    br_normalize_text(value, "", model->headline, sizeof(model->headline));
  } else if (strcmp(key, "metricLabel") == 0) {
    br_normalize_text(value, "", model->metric_label, sizeof(model->metric_label));
  } else if (strcmp(key, "metricValue") == 0) {
    br_normalize_text(value, "", model->metric_value, sizeof(model->metric_value));
  } else if (strcmp(key, "metricUnit") == 0) {
    br_normalize_text(value, "", model->metric_unit, sizeof(model->metric_unit));
  } else if (strcmp(key, "badge") == 0) {
    br_normalize_text(value, "", model->badge, sizeof(model->badge));
  } else if (strcmp(key, "note") == 0) {
    br_normalize_text(value, "", model->note, sizeof(model->note));
  } else if (strcmp(key, "footer") == 0) {
    br_normalize_text(value, "", model->footer, sizeof(model->footer));
  } else if (strcmp(key, "progress") == 0) {
    br_normalize_text(value, "", model->progress, sizeof(model->progress));
  }
}

static bool br_component_dashboard_parse(const char *text, br_component_dashboard_model *model) {
  if (!text || strncmp(text, "COMPONENT_DASHBOARD_V1", 22) != 0) {
    return false;
  }
  br_component_dashboard_defaults(model);

  const char *cursor = text;
  while (*cursor) {
    const char *line_end = strchr(cursor, '\n');
    size_t line_len = line_end ? (size_t) (line_end - cursor) : strlen(cursor);
    if (line_len > 0 && line_len < 256) {
      char line[256];
      memcpy(line, cursor, line_len);
      line[line_len] = '\0';
      char *eq = strchr(line, '=');
      if (eq) {
        *eq = '\0';
        br_component_dashboard_set_value(model, line, eq + 1);
      }
    }
    if (!line_end) break;
    cursor = line_end + 1;
  }
  return true;
}

static void br_stats_dashboard_combined_text(
  const br_stats_dashboard_model *model,
  const char *debug_text,
  char *out,
  size_t out_size
) {
  snprintf(out, out_size, "%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s",
           debug_text ? debug_text : "",
           model->agent,
           model->eyebrow,
           model->headline,
           model->metric_title,
           model->metric_value,
           model->metric_unit,
           model->breakdown[0] ? model->breakdown : model->sources);
  strncat(out, "\n!\n", out_size > strlen(out) ? out_size - strlen(out) - 1 : 0);
  strncat(out, model->completed, out_size > strlen(out) ? out_size - strlen(out) - 1 : 0);
}

static void br_component_dashboard_combined_text(
  const br_component_dashboard_model *model,
  const char *debug_text,
  char *out,
  size_t out_size
) {
  /* Include EVERY slot the renderer may draw — combined_text is what the glyph
     preloader uses to decide which CJK / emoji codepoints to bake into the cache.
     Omit a slot here and any unique codepoints it carries (e.g. progress label
     "本轮进度") will render as "?" / tofu. */
  snprintf(out, out_size, "%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s",
           debug_text ? debug_text : "",
           model->title,
           model->eyebrow,
           model->headline,
           model->metric_label,
           model->metric_value,
           model->metric_unit,
           model->badge,
           model->note,
           model->footer,
           model->progress);
}

static bool br_overlay_render_stats_dashboard(
  const br_overlay_config *config,
  br_framebuffer *fb,
  br_glyph_cache *cache,
  const br_stats_dashboard_model *model
) {
  bool compact = fb->height <= 540;
  uint32_t bg = br_pack_pixel(5, 7, 6, 255);
  uint32_t scanline = br_pack_pixel(16, 27, 19, 255);
  uint32_t green_wash = br_pack_pixel(10, 31, 22, 255);
  uint32_t amber_wash = br_pack_pixel(36, 25, 10, 255);
  uint32_t panel = br_pack_pixel(11, 14, 13, 255);
  uint32_t panel_green = br_pack_pixel(13, 39, 25, 255);
  uint32_t dim = br_pack_pixel(158, 164, 160, 255);
  uint32_t ivory = br_pack_pixel(250, 241, 204, 255);
  uint32_t orange = br_pack_pixel(255, 163, 31, 255);
  uint32_t green = br_pack_pixel(85, 193, 133, 255);
  uint32_t amber = br_pack_pixel(242, 174, 51, 255);
  uint32_t dark_text = br_pack_pixel(19, 18, 13, 255);

  br_fill_rect(fb, 0, 0, fb->width, fb->height, bg);
  int left_wash = fb->width / 3;
  br_fill_rect(fb, 0, 0, left_wash, fb->height, green_wash);
  br_fill_rect(fb, fb->width - fb->width / 4, 0, fb->width / 4, fb->height, amber_wash);
  for (int y = 0; y < fb->height; y += 4) {
    br_fill_rect(fb, 0, y, fb->width, 1, scanline);
  }
  for (int x = 18; x < fb->width - 18; x += 18) {
    br_fill_circle(fb, x, 14, 3, br_pack_pixel(1, 2, 2, 255));
    br_fill_circle(fb, x, fb->height - 14, 3, br_pack_pixel(1, 2, 2, 255));
  }

  int margin = compact ? (fb->width < 700 ? 30 : 44) : (fb->width < 700 ? 38 : 56);
  int badge_x = margin;
  int badge_y = compact ? 42 : 64;
  int badge_w = compact ? (fb->width < 700 ? 210 : 248) : (fb->width < 700 ? 246 : 304);
  int badge_h = compact ? 78 : 112;
  br_fill_rounded_rect(fb, badge_x, badge_y, badge_w, badge_h, 10, panel_green);
  br_draw_scaled_text_fit(config, fb, cache, model->agent,
                          badge_x + (compact ? 18 : 24),
                          badge_y + (compact ? 18 : 24),
                          badge_w - (compact ? 36 : 48),
                          compact ? 3.0 : 4.2,
                          2,
                          br_pack_pixel(225, 241, 231, 255));

  int circle_radius = compact ? 36 : 45;
  int circle_shadow = compact ? 4 : 5;
  int circle_y = compact ? 82 : 118;
  int done_x = fb->width - margin - circle_radius;
  int alert_x = done_x - (compact ? 78 : 92);
  br_fill_circle(fb, alert_x + circle_shadow, circle_y + circle_shadow,
                 circle_radius + circle_shadow, br_pack_pixel(65, 43, 17, 255));
  br_fill_circle(fb, alert_x, circle_y, circle_radius, amber);
  br_draw_scaled_text_center(config, fb, cache, "!", alert_x,
                             circle_y - (compact ? 19 : 24),
                             compact ? 2.55 : 3.3, 1, dark_text);
  br_fill_circle(fb, done_x + circle_shadow, circle_y + circle_shadow,
                 circle_radius + circle_shadow, br_pack_pixel(20, 57, 39, 255));
  br_fill_circle(fb, done_x, circle_y, circle_radius, green);
  br_draw_scaled_text_center(config, fb, cache, model->completed, done_x,
                             circle_y - (compact ? 19 : 24),
                             compact ? 2.55 : 3.3, 1, dark_text);

  int copy_y = compact ? 168 : 286;
  int headline_y = copy_y + (compact ? 40 : 58);
  int divider_y = copy_y + (compact ? 114 : 146);
  br_draw_scaled_text(config, fb, cache, model->eyebrow, margin, copy_y,
                      compact ? 1.85 : 2.4, 2, dim);
  br_draw_scaled_text_fit(config, fb, cache, model->headline, margin, headline_y,
                          fb->width - margin * 2, compact ? 3.0 : 3.8, 2, orange);
  br_stroke_dashed_hline(fb, margin, divider_y, fb->width - margin * 2, 9, 7,
                         br_pack_pixel(74, 74, 62, 255));

  int token_y = compact ? divider_y + 24 : copy_y + 184;
  int token_w = fb->width - margin * 2;
  int token_h = compact ? 132 : 190;
  br_fill_rounded_rect(fb, margin, token_y, token_w, token_h, 8, panel);
  br_draw_scaled_text(config, fb, cache, model->metric_title,
                      margin + (compact ? 20 : 26),
                      token_y + (compact ? 18 : 28),
                      compact ? 1.65 : 2.5, 2, dim);
  {
    br_overlay_config value_cfg = *config;
    value_cfg.scale = compact ? 3.15 : 4.4;
    value_cfg.tracking = 2;
    int value_max_width = token_w - (compact ? 150 : 190);
    if (value_max_width < 1) value_max_width = 1; /* clamp: narrow fb would underflow → loop never terminates */
    while (value_cfg.scale > 1.0 && br_overlay_measure_text(&value_cfg, cache, model->metric_value) > value_max_width) {
      value_cfg.scale -= 0.15;
    }
    int value_x = margin + (compact ? 20 : 26);
    int value_width = br_overlay_measure_text(&value_cfg, cache, model->metric_value);
    br_draw_text_color(&value_cfg, fb, cache, model->metric_value, value_x,
                       token_y + (compact ? 58 : 86), ivory);
    br_draw_scaled_text(config, fb, cache, model->metric_unit,
                        value_x + value_width + (compact ? 14 : 18),
                        token_y + (compact ? 80 : 116),
                        compact ? 1.15 : 1.7, 1, dim);
  }

  if (!compact && model->breakdown[0]) {
    br_draw_scaled_text_fit(config, fb, cache, model->breakdown, margin, token_y + token_h + 34,
                            fb->width - margin * 2, 1.45, 1, br_pack_pixel(164, 169, 160, 255));
  }
  if (!compact && model->sources[0]) {
    br_draw_scaled_text_fit(config, fb, cache, model->sources, margin, token_y + token_h + 72,
                            fb->width - margin * 2, 1.35, 1, br_pack_pixel(111, 183, 142, 255));
  }

  br_stroke_dashed_hline(fb, margin, fb->height - 52, fb->width - margin * 2, 9, 7,
                         br_pack_pixel(55, 66, 54, 255));
  return true;
}

static bool br_overlay_render_component_dashboard(
  const br_overlay_config *config,
  br_framebuffer *fb,
  br_glyph_cache *cache,
  const br_component_dashboard_model *model
) {
  bool compact = fb->height <= 540;
  uint32_t bg = br_pack_pixel(5, 7, 6, 255);
  uint32_t scanline = br_pack_pixel(16, 27, 19, 255);
  uint32_t green_wash = br_pack_pixel(10, 31, 22, 255);
  uint32_t amber_wash = br_pack_pixel(36, 25, 10, 255);
  uint32_t panel = br_pack_pixel(11, 14, 13, 255);
  uint32_t dim = br_pack_pixel(158, 164, 160, 255);
  uint32_t ivory = br_pack_pixel(250, 241, 204, 255);
  uint32_t orange = br_pack_pixel(255, 163, 31, 255);
  uint32_t green = br_pack_pixel(85, 193, 133, 255);
  uint32_t dark_text = br_pack_pixel(19, 18, 13, 255);

  br_fill_rect(fb, 0, 0, fb->width, fb->height, bg);
  int left_wash = fb->width / 3;
  br_fill_rect(fb, 0, 0, left_wash, fb->height, green_wash);
  br_fill_rect(fb, fb->width - fb->width / 4, 0, fb->width / 4, fb->height, amber_wash);
  for (int y = 0; y < fb->height; y += 4) {
    br_fill_rect(fb, 0, y, fb->width, 1, scanline);
  }

  int margin = compact ? (fb->width < 700 ? 30 : 44) : (fb->width < 700 ? 38 : 56);
  int top_y = compact ? 44 : 64;
  int title_max_w = fb->width - margin * 2 - (compact ? 250 : 360);
  if (title_max_w < 120) title_max_w = fb->width - margin * 2 - (compact ? 86 : 112);
  if (title_max_w < 1) title_max_w = 1;
  br_draw_scaled_text_fit(config, fb, cache, model->title,
                          margin,
                          top_y,
                          title_max_w,
                          compact ? 2.45 : 3.55,
                          2,
                          br_pack_pixel(225, 241, 231, 255));

  /* badge circle (top-right). Only the green "done" circle — no amber alert
     (COMPONENT_DASHBOARD_V1 has a single badge slot). */
  int circle_radius = compact ? 24 : 36;
  int circle_shadow = compact ? 3 : 4;
  int circle_y = top_y + (compact ? 18 : 26);
  int done_x = fb->width - margin - circle_radius;
  br_fill_circle(fb, done_x + circle_shadow, circle_y + circle_shadow,
                 circle_radius + circle_shadow, br_pack_pixel(20, 57, 39, 255));
  br_fill_circle(fb, done_x, circle_y, circle_radius, green);
  if (model->badge[0]) {
    br_draw_scaled_text_center(config, fb, cache, model->badge, done_x,
                               circle_y - (compact ? 13 : 19),
                               compact ? 1.65 : 2.45, 1, dark_text);
  }

  bool headline_top_drawn = false;
  if (model->headline[0]) {
    br_overlay_config headline_cfg = *config;
    headline_cfg.scale = compact ? 1.65 : 2.35;
    headline_cfg.tracking = 1;
    int callout_right = done_x - circle_radius - (compact ? 16 : 24);
    int callout_x = fb->width / 2;
    int callout_w = callout_right - callout_x;
    while (callout_w > 1 && headline_cfg.scale > 0.85 &&
           br_overlay_measure_text(&headline_cfg, cache, model->headline) > callout_w) {
      headline_cfg.scale -= 0.1;
    }
    if (callout_w > 42) {
      int headline_w = br_overlay_measure_text(&headline_cfg, cache, model->headline);
      int headline_x = callout_right - headline_w;
      if (headline_x < callout_x) headline_x = callout_x;
      br_draw_text_color(&headline_cfg, fb, cache, model->headline, headline_x,
                         top_y + (compact ? 5 : 9), orange);
      headline_top_drawn = true;
    }
  }

  int copy_y = compact ? top_y + 72 : top_y + 126;
  int headline_y = copy_y + (compact ? 34 : 50);
  int divider_y = copy_y + (headline_top_drawn ? (compact ? 38 : 56) : (compact ? 90 : 124));
  if (model->eyebrow[0]) {
    br_draw_scaled_text(config, fb, cache, model->eyebrow, margin, copy_y,
                        compact ? 1.85 : 2.4, 2, dim);
  }
  if (model->headline[0] && !headline_top_drawn) {
    br_draw_scaled_text_fit(config, fb, cache, model->headline, margin, headline_y,
                            fb->width - margin * 2, compact ? 3.0 : 3.8, 2, orange);
  }
  br_stroke_dashed_hline(fb, margin, divider_y, fb->width - margin * 2, 9, 7,
                         br_pack_pixel(74, 74, 62, 255));

  /* metric panel */
  int token_y = compact ? divider_y + 22 : copy_y + 170;
  int token_w = fb->width - margin * 2;
  int token_h = compact ? 178 : 210;
  br_fill_rounded_rect(fb, margin, token_y, token_w, token_h, 8, panel);
  if (model->metric_label[0]) {
    br_draw_scaled_text(config, fb, cache, model->metric_label,
                        margin + (compact ? 20 : 26),
                        token_y + (compact ? 18 : 28),
                        compact ? 1.65 : 2.5, 2, dim);
  }
  if (model->metric_value[0]) {
    br_overlay_config value_cfg = *config;
    value_cfg.scale = compact ? 3.15 : 4.4;
    value_cfg.tracking = 2;
    int value_max_width = token_w - (compact ? 150 : 190);
    if (value_max_width < 1) value_max_width = 1; /* clamp: narrow fb would underflow → loop never terminates */
    while (value_cfg.scale > 1.0 && br_overlay_measure_text(&value_cfg, cache, model->metric_value) > value_max_width) {
      value_cfg.scale -= 0.15;
    }
    int value_x = margin + (compact ? 20 : 26);
    int value_width = br_overlay_measure_text(&value_cfg, cache, model->metric_value);
    br_draw_text_color(&value_cfg, fb, cache, model->metric_value, value_x,
                       token_y + (compact ? 58 : 86), ivory);
    if (model->metric_unit[0]) {
      br_draw_scaled_text(config, fb, cache, model->metric_unit,
                          value_x + value_width + (compact ? 14 : 18),
                          token_y + (compact ? 80 : 116),
                          compact ? 1.15 : 1.7, 1, dim);
    }
  }

  /* IMPORTANT: COMPONENT_DASHBOARD_V1 renders note + footer ALSO in compact mode.
     Note now lives inside the metric panel so small widgets match the client
     preview density and leave the footer readable. */
  int note_y = token_y + (compact ? 120 : 152);
  if (model->note[0]) {
    br_draw_scaled_text_fit(config, fb, cache, model->note,
                            margin + (compact ? 20 : 26),
                            note_y,
                            token_w - (compact ? 40 : 52),
                            compact ? 1.15 : 1.35, 1,
                            br_pack_pixel(164, 169, 160, 255));
  }

  /* progress bar: parse "<pct>:<label>" and draw inside the metric panel near
     its bottom edge. We render INSIDE the panel (overlaying its dark bg) so the
     bar is always visible regardless of compact-mode note/footer crowding.
     Skip silently if no progress field set or value out of range. */
  if (model->progress[0]) {
    char progress_copy[64];
    snprintf(progress_copy, sizeof(progress_copy), "%s", model->progress);
    int pct = -1;
    const char *label = "";
    char *colon = strchr(progress_copy, ':');
    /* parse_pct: use strtol so non-numeric input (e.g. "abc:label") is rejected
       rather than silently rendered as a 0%% bar (atoi returns 0 on any non-numeric
       prefix). end_ptr == start means no digits were consumed → leave pct=-1
       so the bar is skipped entirely. */
    const char *pct_src = progress_copy;
    if (colon) {
      *colon = '\0';
      label = colon + 1;
    }
    {
      char *end_ptr = NULL;
      long parsed = strtol(pct_src, &end_ptr, 10);
      if (end_ptr != pct_src && parsed >= 0) {
        pct = (parsed > 100) ? 100 : (int) parsed;
      }
    }
    if (pct >= 0) {
      /* pct already clamped to [0,100] above. */
      /* bar lives in the metric panel's lower padding strip */
      int bar_x = margin + (compact ? 20 : 26);
      int bar_w = token_w - (compact ? 40 : 52);
      if (bar_w < 1) bar_w = 1; /* clamp: narrow framebuffer with large margins would underflow */
      int bar_h = compact ? 4 : 6;
      int bar_y = token_y + token_h - bar_h - (compact ? 8 : 12);
      /* track: slightly lighter than panel bg */
      br_fill_rect(fb, bar_x, bar_y, bar_w, bar_h, br_pack_pixel(40, 40, 40, 255));
      /* fill (orange accent matches headline color) */
      int fill_w = (bar_w * pct) / 100;
      if (fill_w > 0) br_fill_rect(fb, bar_x, bar_y, fill_w, bar_h, orange);
      /* small label above bar (left) + % (right), kept inside panel */
      int text_y = bar_y - (compact ? 14 : 18);
      if (label && label[0]) {
        br_draw_scaled_text(config, fb, cache, label, bar_x, text_y,
                            compact ? 0.95 : 1.05, 1, dim);
      }
      char pct_text[16];
      snprintf(pct_text, sizeof(pct_text), "%d%%", pct);
      int pct_w = br_overlay_measure_text(config, cache, pct_text);
      br_draw_scaled_text(config, fb, cache, pct_text,
                          bar_x + bar_w - pct_w,
                          text_y, compact ? 0.95 : 1.05, 1, ivory);
    }
  }

  if (model->footer[0]) {
    int footer_y = compact ? fb->height - 40 : fb->height - 60;
    br_draw_scaled_text_fit(config, fb, cache, model->footer, margin, footer_y,
                            fb->width - margin * 2, compact ? 1.15 : 1.35, 1,
                            br_pack_pixel(111, 183, 142, 255));
  }
  return true;
}

static bool br_overlay_write_frame(const br_overlay_config *config, const br_framebuffer *fb) {
  FILE *file = fopen(config->fb_path, "r+b");
  if (!file) {
    return false;
  }
  size_t total = (size_t) fb->stride * (size_t) fb->height;
  bool ok = fwrite(fb->data, 1, total, file) == total;
  fclose(file);
  return ok;
}

static void br_overlay_set_blank(bool visible) {
  FILE *file = fopen("/sys/class/graphics/fb0/blank", "wb");
  if (!file) {
    return;
  }
  fprintf(file, "%d", visible ? 0 : 4);
  fclose(file);
}

static void br_overlay_clear_flag(const br_overlay_config *config) {
  unlink(config->active_path);
}

static bool br_overlay_render_card(
  const br_overlay_config *config,
  br_framebuffer *fb,
  br_glyph_cache *cache,
  const char *text,
  bool top_aligned
) {
  br_line lines[BR_MAX_LINES];
  int line_count;
  int line_height;
  int card_width;
  int card_x;
  int max_text_width;
  int bar_height;
  int bar_top;
  int scrim_x;
  int scrim_width;
  int scrim_top;
  int scrim_height;

  memset(lines, 0, sizeof(lines));
  if (!text || !*text) {
    return true;
  }

  card_width = fb->width - config->card_margin_x * 2;
  if (card_width > config->card_max_width) {
    card_width = config->card_max_width;
  }
  if (card_width < 64) {
    card_width = 64;
  }
  card_x = (fb->width - card_width) / 2;
  max_text_width = card_width - config->padding_x * 2;
  if (max_text_width < 64) {
    max_text_width = 64;
  }
  line_count = br_overlay_wrap_text(config, cache, text, max_text_width, lines, config->max_lines);
  if (line_count <= 0) {
    return true;
  }

  line_height = (int) lround(16.0 * config->scale);
  bar_height = config->padding_y * 2 + line_count * line_height + (line_count - 1) * config->line_gap;
  if (top_aligned) {
    bar_top = config->card_bottom;
  } else {
    bar_top = fb->height - config->card_bottom - bar_height;
  }
  if (bar_top < 0) {
    bar_top = 0;
  }
  scrim_x = card_x - config->scrim_side_padding;
  if (scrim_x < 0) {
    scrim_x = 0;
  }
  scrim_width = card_width + config->scrim_side_padding * 2;
  if (scrim_x + scrim_width > fb->width) {
    scrim_width = fb->width - scrim_x;
  }
  if (top_aligned) {
    scrim_top = bar_top;
    scrim_height = bar_height + config->scrim_bottom_padding;
  } else {
    scrim_top = bar_top - config->scrim_top_padding;
    if (scrim_top < 0) {
      scrim_top = 0;
    }
    scrim_height = bar_height + config->scrim_top_padding + config->scrim_bottom_padding;
    if (scrim_top + scrim_height > fb->height) {
      scrim_height = fb->height - scrim_top;
    }
  }

  if (top_aligned) {
    br_fill_gradient(fb, scrim_x, scrim_top, scrim_width, scrim_height, config->scrim_bottom_alpha, config->scrim_top_alpha);
  } else {
    br_fill_gradient(fb, scrim_x, scrim_top, scrim_width, scrim_height, config->scrim_top_alpha, config->scrim_bottom_alpha);
  }
  /* Card background with rounded corners (radius 8px) */
  {
    int r = 8;
    /* Fill main body (excluding top/bottom radius rows) */
    br_fill_rect(fb, card_x, bar_top + r, card_width, bar_height - r * 2, br_pack_pixel(8, 10, 15, 200));
    /* Fill top and bottom radius bands row-by-row with inset */
    for (int ry = 0; ry < r; ry++) {
      /* Approximate circle: inset = r - sqrt(r^2 - (r-ry)^2) */
      double d = (double)(r - ry);
      int inset = r - (int)lround(sqrt((double)(r * r) - d * d));
      if (inset < 0) inset = 0;
      /* Top row */
      br_fill_rect(fb, card_x + inset, bar_top + ry, card_width - inset * 2, 1, br_pack_pixel(8, 10, 15, 200));
      /* Bottom row */
      br_fill_rect(fb, card_x + inset, bar_top + bar_height - 1 - ry, card_width - inset * 2, 1, br_pack_pixel(8, 10, 15, 200));
    }
  }

  int y = bar_top + config->padding_y;
  for (int i = 0; i < line_count; i += 1) {
    int text_x = card_x + (card_width - lines[i].width) / 2;
    br_draw_text(config, fb, cache, lines[i].text, text_x, y);
    y += line_height + config->line_gap;
  }
  return true;
}

static bool br_overlay_build_frame(
  const br_overlay_config *config,
  const char *font_path,
  const char *speech_text,
  const char *debug_text,
  br_framebuffer *fb
) {
  br_glyph_cache cache;
  char combined_text[4096];
  br_stats_dashboard_model dashboard;
  bool is_dashboard = false;
  br_component_dashboard_model component;
  bool is_component_dashboard = false;

  br_overlay_read_display_size(fb);
  size_t total = (size_t) fb->stride * (size_t) fb->height;
  fb->data = (unsigned char *) calloc(total, 1);
  if (!fb->data) {
    return false;
  }

  if ((!speech_text || !*speech_text) && (!debug_text || !*debug_text)) {
    return true;
  }

  is_component_dashboard = br_component_dashboard_parse(speech_text, &component);
  if (is_component_dashboard) {
    br_component_dashboard_combined_text(&component, debug_text, combined_text, sizeof(combined_text));
  } else {
    is_dashboard = br_stats_dashboard_parse(speech_text, &dashboard);
    if (is_dashboard) {
      br_stats_dashboard_combined_text(&dashboard, debug_text, combined_text, sizeof(combined_text));
    } else {
      snprintf(combined_text, sizeof(combined_text), "%s\n%s", debug_text ? debug_text : "", speech_text ? speech_text : "");
    }
  }
  if (!br_overlay_load_glyphs(font_path, combined_text, &cache)) {
    return false;
  }
  if (is_component_dashboard) {
    if (!br_overlay_render_component_dashboard(config, fb, &cache, &component)) {
      return false;
    }
    return br_overlay_render_card(config, fb, &cache, debug_text, true);
  }
  if (is_dashboard) {
    if (!br_overlay_render_stats_dashboard(config, fb, &cache, &dashboard)) {
      return false;
    }
    return br_overlay_render_card(config, fb, &cache, debug_text, true);
  }
  if (!br_overlay_render_card(config, fb, &cache, debug_text, true)) {
    return false;
  }
  return br_overlay_render_card(config, fb, &cache, speech_text, false);
}

int main(int argc, char **argv) {
  br_overlay_config config;
  char font_path[BR_MAX_PATH];
  char last_text[2048] = "";
  char last_debug_text[2048] = "";
  time_t last_mtime = 0;
  double visible_until = 0.0;
  bool is_visible = false;
  double last_render_at = 0.0;

  if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    printf("usage: %s [runtime-root]\n", argv[0]);
    return 0;
  }

  if (getenv("PET_CLAW_FB_SPEECH_OVERLAY") && strcmp(getenv("PET_CLAW_FB_SPEECH_OVERLAY"), "0") == 0) {
    br_overlay_logf("disabled");
    return 0;
  }
  if (!br_overlay_framebuffer_supported()) {
    br_overlay_logf("disabled: fb0 is not 32bpp");
    return 0;
  }

  br_overlay_load_config(&config, argc > 1 ? argv[1] : ".");
  while (!br_overlay_find_font(&config, font_path, sizeof(font_path))) {
    br_overlay_logf("waiting for font");
    br_sleep_ms(5000);
  }
  br_overlay_logf("font %s", font_path);
  br_overlay_clear_flag(&config);

  while (true) {
    struct stat st;
    char state[128];
    char event[128];
    char text[2048];
    char debug_text[2048];
    bool speech_changed;
    bool debug_changed;
    double now = br_now_ms() / 1000.0;

    if (!br_overlay_read_text(config.state_path, state, sizeof(state))) {
      state[0] = '\0';
    }
    if (!br_overlay_read_text(config.event_path, event, sizeof(event))) {
      event[0] = '\0';
    }

    /* 文本来源：默认从 .current-speech 读；当 .screen-page=stats 时改读 .stats-display。
     * stats 文件由 runtime_stats 模块预格式化为 STATS_DASHBOARD_V1 payload，
     * overlay 只负责解析展示字段和绘制 framebuffer，不重新计算 token。 */
    char screen_page[16];
    bool is_stats_page = false;
    if (br_overlay_read_text(config.screen_page_path, screen_page, sizeof(screen_page))) {
      is_stats_page = strcmp(screen_page, "stats") == 0;
    }
    const char *active_text_path = is_stats_page ? config.stats_display_path : config.speech_path;

    if (!br_overlay_read_text(active_text_path, text, sizeof(text))) {
      text[0] = '\0';
    }
    if (!br_overlay_read_text(config.debug_speech_path, debug_text, sizeof(debug_text))) {
      debug_text[0] = '\0';
    }

    bool is_pairing = strncmp(event, "Pairing", 7) == 0 ||
                      strcmp(state, "waiting_user") == 0 ||
                      strcmp(state, "waiting_config") == 0;
    /* 统计页文本不应在 hold 后消失，让用户能持续阅读，直到切回 main 页。 */
    double effective_hold = is_stats_page ? 86400.0 : config.hold_seconds;

    time_t mtime = 0;
    if (stat(active_text_path, &st) == 0) {
      mtime = st.st_mtime;
    }
    speech_changed = mtime != last_mtime || strcmp(text, last_text) != 0;
    debug_changed = strcmp(debug_text, last_debug_text) != 0;

    if ((text[0] != '\0' || debug_text[0] != '\0') && (speech_changed || debug_changed)) {
      br_framebuffer fb;
      memset(&fb, 0, sizeof(fb));
      if (br_overlay_build_frame(&config, font_path, text, debug_text, &fb)) {
        br_overlay_write_frame(&config, &fb);
        br_overlay_set_blank(true);
        is_visible = true;
        visible_until = debug_text[0] != '\0' ? now + 3600.0 : now + effective_hold;
        last_render_at = now;
        br_overlay_logf("render speech=%zu debug=%zu state=%s event=%s pairing=%d",
                        strlen(text), strlen(debug_text), state, event, is_pairing ? 1 : 0);
      }
      free(fb.data);
      br_normalize_text(text, "", last_text, sizeof(last_text));
      br_normalize_text(debug_text, "", last_debug_text, sizeof(last_debug_text));
      last_mtime = mtime;
    } else if (is_visible && (debug_text[0] != '\0' || now < visible_until) && (now - last_render_at) >= config.redraw_seconds) {
      br_framebuffer fb;
      memset(&fb, 0, sizeof(fb));
      if (br_overlay_build_frame(&config, font_path, last_text, last_debug_text, &fb)) {
        br_overlay_write_frame(&config, &fb);
        br_overlay_set_blank(true);
      }
      free(fb.data);
      last_render_at = now;
    } else if (is_visible && debug_text[0] == '\0' && (text[0] == '\0' || now >= visible_until)) {
      br_framebuffer fb;
      memset(&fb, 0, sizeof(fb));
      if (br_overlay_build_frame(&config, font_path, "", "", &fb)) {
        br_overlay_write_frame(&config, &fb);
        br_overlay_set_blank(false);
      }
      free(fb.data);
      is_visible = false;
      last_debug_text[0] = '\0';
      br_overlay_clear_flag(&config);
      br_overlay_logf("clear");
    }

    br_sleep_ms((int) lround(config.poll_seconds * 1000.0));
  }
}
