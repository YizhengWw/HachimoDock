/* Versioned data-only writer plan. Execution still requires native admission.
 * No owner handles, service execution, IO, callbacks, tasks or allocation. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "cJSON.h"
#ifndef PET_VARIABLE_OWNERS_ENABLED
#define PET_VARIABLE_OWNERS_ENABLED 0
#endif
#define PET_VARIABLE_OWNERS_ENGINE "p4-variable-owners-v1"
typedef struct {uint8_t host_mask;bool mixed;} pet_variable_owners_t;
static inline bool pet_variable_owners_host(pet_variable_owners_t plan,int at){
  return at>=0&&at<8&&(plan.host_mask&(1U<<(unsigned)at));
}
/* One trusted parser translation unit avoids per-consumer code duplication. */
#if PET_VARIABLE_OWNERS_ENABLED
bool pet_variable_owners_parse(const cJSON *root,pet_variable_owners_t *out);
#else
static inline bool pet_variable_owners_parse(const cJSON *root,pet_variable_owners_t *out){
  if(!root||!out)return false;
  const cJSON *vars=cJSON_GetObjectItemCaseSensitive(root,"vars");
  int n=cJSON_GetArraySize(vars);
  if(!cJSON_IsObject(vars)||n>8||cJSON_HasObjectItem(root,"ownership"))return false;
  bool hosted=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"host"));
  *out=(pet_variable_owners_t){.host_mask=hosted?(uint8_t)((1U<<(unsigned)n)-1):0};return true;
}
#endif
