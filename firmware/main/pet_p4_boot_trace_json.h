/* Atomic assembly: allocation failure never publishes a partial boot record. */
#pragma once
#include "pet_p4_boot_trace.h"
#if PET_P4_BOOT_TRACE_ENABLE
#include "cJSON.h"
static inline bool pet_boot_trace_add_json(cJSON *payload,const pet_boot_trace_t *boot,bool observed){
  if(!cJSON_IsObject(payload)||!boot||cJSON_HasObjectItem(payload,"bootTrace"))return false;
  cJSON *object=cJSON_CreateObject(),*times=cJSON_CreateArray();
  bool ok=object&&times;
  if(ok)ok=cJSON_AddBoolToObject(object,"available",observed&&boot->available)
    &&cJSON_AddBoolToObject(object,"invalidClock",boot->invalid_clock)
    &&cJSON_AddNumberToObject(object,"observedMask",boot->mask)
    &&cJSON_AddBoolToObject(object,"complete",observed&&boot->available&&!boot->invalid_clock&&boot->mask==255)
    &&cJSON_AddBoolToObject(object,"includesPreemption",true);
  for(unsigned i=0;ok&&i<PET_BOOT_MARK_COUNT;i++){
    cJSON *number=cJSON_CreateNumber(boot->elapsed_us[i]);
    if(!number||!cJSON_AddItemToArray(times,number)){cJSON_Delete(number);ok=false;}
  }
  if(ok){if(cJSON_AddItemToObject(object,"elapsedUs",times))times=NULL;else ok=false;}
  if(ok){if(cJSON_AddItemToObject(payload,"bootTrace",object))object=NULL;else ok=false;}
  cJSON_Delete(times);cJSON_Delete(object);return ok;
}
#endif
