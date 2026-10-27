#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

/* Bounded offline primitives. No component IDs, network, scripts or file access. */
#define PET_LOCAL_ENGINE "p4-local-tools-v1"
#define PET_LOCAL_VARS 7
#define PET_LOCAL_GROUPS 3
#define PET_LOCAL_ITEMS 16
typedef struct { char title[61], note[145]; } pet_local_item_t;
typedef struct {
  char title[31]; uint8_t count, kind;
  pet_local_item_t items[PET_LOCAL_ITEMS];
} pet_local_group_t;
typedef struct {
  bool enabled, countdown;
  uint8_t count, action_count;
  uint16_t reveal_ms;
  char actions[8][48]; uint8_t operations[8];
  pet_local_group_t groups[PET_LOCAL_GROUPS];
  char idle[145], busy[145], hint[145];
} pet_local_config_t;
typedef struct {
  uint8_t phase, field;
  int32_t pending, draft_day, draft_minute;
  uint64_t reveal_at;
  int64_t last_second;
} pet_local_state_t;
typedef struct {
  uint8_t phase;
  char eyebrow[91],headline[157],note[157],metric[61],label[91],unit[31],badge[13];
  char primary[40],secondary[40];
  int progress;
} pet_local_view_t;
enum { PET_LOCAL_DRAW=1, PET_LOCAL_EXCLUDE, PET_LOCAL_RESET, PET_LOCAL_CLOSE,
       PET_LOCAL_PREVIOUS, PET_LOCAL_NEXT, PET_LOCAL_EDIT, PET_LOCAL_CANCEL,
       PET_LOCAL_UP, PET_LOCAL_DOWN };
bool pet_local_parse(const cJSON *json, pet_local_config_t *out);
void pet_local_init(pet_local_state_t *s);
bool pet_local_enabled(const pet_local_config_t *c,const pet_local_state_t *s,const char *action);
bool pet_local_action(const pet_local_config_t *c,pet_local_state_t *s,int32_t v[7],const char *action,uint64_t ms,int64_t local_seconds,uint32_t random);
bool pet_local_tick(const pet_local_config_t *c,pet_local_state_t *s,int32_t v[7],uint64_t ms,int64_t local_seconds);
void pet_local_view(const pet_local_config_t *c,const pet_local_state_t *s,const int32_t v[7],uint64_t ms,int64_t local_seconds,pet_local_view_t *out);
int32_t pet_local_day(int y,int m,int d);
void pet_local_date(int32_t day,int *y,int *m,int *d);
int64_t pet_local_target(uint8_t kind,int32_t day,int minute,int64_t local_seconds);
