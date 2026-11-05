/* p4-data-rows-v1: generic paged/filtered/selectable projection of one bounded
 * PC-fed data list (<=20 rows). Pure functions over the row array held by
 * pet_p4_widget_data.c; the PC preview and validators run the same rules.
 * Filters: up to 4 enum filters, each bound to an int var. Value 0 means
 * "all"; 1..count selects values[value-1]; anything else matches nothing. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_widget_data.h"
#define PET_DATA_FILTERS_MAX 4
#define PET_DATA_FILTER_VALUES_MAX 8
#define PET_DATA_FILTER_VALUE_BYTES 24
enum {PET_DATA_FIELD_LABEL=0,PET_DATA_FIELD_VALUE,PET_DATA_FIELD_DETAIL,PET_DATA_FIELD_META,PET_DATA_FIELD_TONE,PET_DATA_FIELD_COUNT};
typedef struct {
  char var[32];
  uint8_t field;
  uint8_t count;
  char values[PET_DATA_FILTER_VALUES_MAX][PET_DATA_FILTER_VALUE_BYTES+1];
} pet_data_filter_t;
typedef struct pet_data_rows_config {
  bool enabled;
  bool rows_layout;
  uint8_t filter_count;
  pet_data_filter_t filters[PET_DATA_FILTERS_MAX];
  char select_var[32];
  char refresh_action[48];
} pet_data_rows_config_t;
static inline int pet_data_field_index(const char *name){
  static const char *const names[PET_DATA_FIELD_COUNT]={"label","value","detail","meta","tone"};
  if(!name)return -1;
  for(int i=0;i<PET_DATA_FIELD_COUNT;i++)if(!strcmp(name,names[i]))return i;
  return -1;
}
/* Field text of a row; tone is rendered as a decimal string into scratch. */
static inline const char *pet_data_row_field(const pet_p4_data_row_t *row,int field,char scratch[5]){
  switch(field){
    case PET_DATA_FIELD_LABEL:return row->label;
    case PET_DATA_FIELD_VALUE:return row->value;
    case PET_DATA_FIELD_DETAIL:return row->detail;
    case PET_DATA_FIELD_META:return row->meta;
    case PET_DATA_FIELD_TONE:snprintf(scratch,5,"%d",(int)row->tone);return scratch;
    default:return "";
  }
}
static inline bool pet_data_row_matches(const pet_p4_data_row_t *row,const pet_data_filter_t *f,int32_t value){
  if(value<=0)return true;
  if(value>f->count||f->field>=PET_DATA_FIELD_COUNT)return false;
  char scratch[5];return !strcmp(pet_data_row_field(row,f->field,scratch),f->values[value-1]);
}
/* Indexes of the rows that pass every filter, in source order. filter_values
 * holds the current int value of filters[i].var. Returns the visible count. */
static inline unsigned pet_data_rows_filter(const pet_p4_data_row_t *rows,unsigned count,const pet_data_rows_config_t *c,const int32_t *filter_values,uint8_t out[PET_P4_DATA_MAX_ROWS]){
  unsigned n=0;if(count>PET_P4_DATA_MAX_ROWS)count=PET_P4_DATA_MAX_ROWS;
  for(unsigned i=0;i<count;i++){
    bool ok=true;
    if(c&&c->enabled)for(unsigned f=0;f<c->filter_count&&ok;f++)ok=pet_data_row_matches(&rows[i],&c->filters[f],filter_values?filter_values[f]:0);
    if(ok)out[n++]=(uint8_t)i;
  }
  return n;
}
static inline unsigned pet_data_rows_pages(unsigned visible){unsigned p=(visible+PET_P4_DATA_PAGE_ROWS-1)/PET_P4_DATA_PAGE_ROWS;return p?p:1;}
static inline unsigned pet_data_rows_page(unsigned visible,int32_t page){unsigned pages=pet_data_rows_pages(visible);int64_t p=page%(int64_t)pages;if(p<0)p+=pages;return (unsigned)p;}
/* Selected visible index: clamped to the filtered list, -1 when it is empty. */
static inline int pet_data_rows_select(unsigned visible,int32_t select){
  if(!visible)return -1;
  if(select<0)return 0;
  return select>=(int32_t)visible?(int)visible-1:(int)select;
}
