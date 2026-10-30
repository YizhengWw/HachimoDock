#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

enum { PET_BUBBLE_TITLE = 1, PET_BUBBLE_BODY = 2, PET_BUBBLE_COMPACT = 3, PET_BUBBLE_BODY_CLIPPED = 4 };
typedef struct {
  char title[192], body[512];
  uint64_t since_ms;
  bool pixels;
} pet_bubble_text_view_t;
bool pet_bubble_pixels_init(void);
void pet_bubble_pixels_expect(bool enabled, uint64_t now_ms);
void pet_bubble_pixels_select(const char *owner, unsigned style, const char *title,
    const char *body, uint64_t now_ms, pet_bubble_text_view_t *out);
bool pet_bubble_pixels_handle(const cJSON *payload, char *error, size_t error_size);
uint32_t pet_bubble_pixels_revision(void);
unsigned pet_bubble_pixels_pages(unsigned style, const char *text);
bool pet_bubble_pixels_draw(unsigned style, const char *text, unsigned page,
    uint16_t *framebuffer, int x, int y, int clip_width, uint16_t color);
