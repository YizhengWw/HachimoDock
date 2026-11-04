/* Private DEFAULT-OFF downstream serial CONTROL checksum, NOT authentication.
 * Prefix original immutable JSON; never audio/media/native USB/screenshot data.
 * Pure bounded helper, no allocation, queue, wait, task, lock or raw log. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifndef PET_P4_SERIAL_CONTROL_INTEGRITY
#define PET_P4_SERIAL_CONTROL_INTEGRITY 0
#endif
#define PET_CONTROL_INTEGRITY_ENGINE "p4-control-fnv1a32-v1"
#define PET_CONTROL_INTEGRITY_MAX 32768U
#define PET_CONTROL_PREFIX_BYTES 21U
static inline bool pet_control_prefix(char out[PET_CONTROL_PREFIX_BYTES+1],const char *json,size_t *bytes){
  if(!out||!json||!bytes)return false;
  out[0]=0;size_t length=0;
  while(length<=PET_CONTROL_INTEGRITY_MAX&&json[length])length++;
  if(!length||length>PET_CONTROL_INTEGRITY_MAX)return false;
  uint32_t hash=2166136261U;
  for(size_t i=0;i<length;i++){hash^=(uint8_t)json[i];hash*=16777619U;}
  int n=snprintf(out,PET_CONTROL_PREFIX_BYTES+1,"!P4C1:%05u:%08x:",(unsigned)length,(unsigned)hash);
  if(n!=(int)PET_CONTROL_PREFIX_BYTES){out[0]=0;return false;}
  *bytes=length;return true;
}
