/* Bounded provider-neutral live lists. Volatile data only; no network or flash writes. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#define PET_P4_DATA_MAX_SOURCES 8
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
typedef struct {uint8_t page,pages;} pet_p4_data_paging_t;
/* Optional JSON bool clearOnExpire defaults false. When true, stale views
 * expose an empty list and a generic expiry message, without prior row/date text. */
bool pet_p4_widget_data_apply(const cJSON *payload, uint64_t now_ms);
void pet_p4_widget_data_view(const char *source, int32_t page, uint64_t now_ms, pet_p4_data_view_t *out);
/* Same source/count/modulo snapshot under the provider mux. Eligibility needs
 * no row strings, heap allocation, freshness renewal or full view on stack. */
pet_p4_data_paging_t pet_p4_widget_data_paging(const char *source,int32_t page);
#include "pet_p4_component_features.h"
#if PET_P4_DATA_ROWS
struct pet_data_rows_config;
/* p4-data-rows-v1: the active filter for one source (copied under the data lock;
 * values are the current int values of filters[i].var, 4 entries). Paging, view
 * rows and selection then follow the filtered list; a NULL config clears it. */
void pet_p4_widget_data_set_filter(const char *source,const struct pet_data_rows_config *config,const int32_t *values);
/* Selected visible row (clamped) of the filtered list; false when empty or
 * clearOnExpire has expired at now_ms, even if no view has been requested. */
bool pet_p4_widget_data_select(const char *source,int32_t select,uint64_t now_ms,pet_p4_data_row_t *out);
#endif
