/* Bounded provider-neutral live lists. Volatile data only; no network or flash writes. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#define PET_P4_DATA_MAX_ROWS 20
#define PET_P4_DATA_PAGE_ROWS 5
typedef struct {
  char id[24], label[37], value[25], detail[25], meta[64];
  int8_t tone;
} pet_p4_data_row_t;
typedef struct {
  bool enabled, received, stale;
  uint8_t count, page, pages;
  char status[8], message[91], date[11];
  pet_p4_data_row_t rows[PET_P4_DATA_PAGE_ROWS];
} pet_p4_data_view_t;
bool pet_p4_widget_data_apply(const cJSON *payload, uint64_t now_ms);
void pet_p4_widget_data_view(const char *source, int32_t page, uint64_t now_ms, pet_p4_data_view_t *out);
