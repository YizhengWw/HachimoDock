#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
enum {
PET_BUBBLE_TITLE = 1, PET_BUBBLE_BODY = 2, PET_BUBBLE_COMPACT = 3, PET_BUBBLE_BODY_CLIPPED = 4};
typedef struct {
  char            title[192], body[512];
  uint64_t        since_ms;
  bool            pixels;
  unsigned        style;
  uint32_t        epoch;
  int             title_pin, body_pin;
} pet_bubble_text_view_t;
typedef struct {
  char            id[17], boot[17], client[33];
  unsigned        style, page;
  const void     *route;
} pet_bubble_need_t;
bool            pet_bubble_pixels_next_need(uint64_t now_ms, pet_bubble_need_t * out);
bool            pet_bubble_pixels_init(void);
const char     *pet_bubble_pixels_boot(void);
void            pet_bubble_pixels_maintain(uint64_t now_ms, bool pause);
void            pet_bubble_pixels_desire(const cJSON * payload, const void *route, const char *scope, uint64_t now_ms);
bool            pet_bubble_pixels_handle_owned(const cJSON * payload, const void *route, uint64_t now_ms, cJSON * reply);
uint32_t        pet_bubble_pixels_revision(void);
void            pet_bubble_pixels_select(const char *owner, unsigned style, const char *title, const char *body, uint64_t now_ms, pet_bubble_text_view_t * out);
void            pet_bubble_pixels_pin_page(pet_bubble_text_view_t * view, unsigned page);
void            pet_bubble_pixels_release(pet_bubble_text_view_t * view);
unsigned        pet_bubble_pixels_pages(unsigned style, const char *text);
bool            pet_bubble_pixels_draw_pinned(const pet_bubble_text_view_t * view, unsigned style, unsigned page, uint16_t * framebuffer, int x, int y, int clip_width, uint16_t color);
