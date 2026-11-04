#pragma once
#include "pet_p4_save_policy.h"
#include "cJSON.h"
#if PET_APP_SAVE_OWNED_PENDING_ENABLED
/* All-or-nothing fixed numeric evidence, no current-task scan in diagnostics. */
static inline cJSON *pet_app_save_stack_json(void){
  pet_app_save_stack_t sample={0};if(!pet_app_store_worker_stack(&sample))return NULL;
  cJSON *out=cJSON_CreateObject();if(!out)return NULL;
  if(!cJSON_AddBoolToObject(out,"available",sample.available)
      ||!cJSON_AddNumberToObject(out,"taskStackBytes",PET_APP_SAVE_TASK_STACK_BYTES)
      ||!cJSON_AddNumberToObject(out,"observations",sample.observations)
      ||!cJSON_AddNumberToObject(out,"invalidSamples",sample.invalid_samples)
      ||(sample.available&&!cJSON_AddNumberToObject(out,"minimumFreeBytes",sample.minimum_free_bytes))){
    cJSON_Delete(out);return NULL;
  }
  return out;
}
#endif
