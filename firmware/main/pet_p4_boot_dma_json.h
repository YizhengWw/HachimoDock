/* Explicit diagnostic-only serialization of the immutable pre-heap snapshot.
 * No MMIO, mutation, driver creation, persistence or fault-counter adjustment.
 * Build an unattached object first; allocation failure never publishes partial
 * metadata. Ordinary diagnostics omit this object unless includeBootDma=true. */
#pragma once
#include "cJSON.h"
#include "pet_p4_boot_dma.h"
static inline bool pet_boot_dma_state_json(cJSON *out,const pet_boot_dma_state_t *s){
  if(!cJSON_AddNumberToObject(out,"parkedMask",s->parked_mask))return false;
  cJSON *rows=cJSON_AddArrayToObject(out,"channels");if(!rows)return false;
  for(unsigned i=0;i<PET_BOOT_DMA_CHANNELS;i++){
    const pet_boot_dma_channel_t *v=&s->channels[i];cJSON *row=cJSON_CreateObject();
    if(!row)return false;
    if(!cJSON_AddItemToArray(rows,row)){cJSON_Delete(row);return false;}
    const char *const names[]={"rxLinkAddress","rxLink","rxState","rxSuccessEofAddress","rxPrefetchedAddress","rxPeripheral","rxDw0ValidMask","txLinkAddress","txLink","txState","txEofAddress","txPrefetchedAddress"};
    const uint32_t values[]={v->rx_link_addr,v->rx_link,v->rx_state,v->rx_eof,v->rx_prefetched,v->rx_peripheral,v->rx_dw0_valid,v->tx_link_addr,v->tx_link,v->tx_state,v->tx_eof,v->tx_prefetched};
    if(!cJSON_AddNumberToObject(row,"channel",i))return false;
    for(unsigned j=0;j<sizeof(values)/sizeof(*values);j++)if(!cJSON_AddNumberToObject(row,names[j],values[j]))return false;
    cJSON *words=cJSON_AddArrayToObject(row,"rxDw0");if(!words)return false;
    for(unsigned j=0;j<3;j++){
      cJSON *word=cJSON_CreateObject();if(!word)return false;
      if(!cJSON_AddItemToArray(words,word)){cJSON_Delete(word);return false;}
      uint32_t raw=v->rx_dw0[j];
      if(!cJSON_AddNumberToObject(word,"candidate",j)
          ||!cJSON_AddBoolToObject(word,"readable",(v->rx_dw0_valid&(1U<<j))!=0)
          ||!cJSON_AddNumberToObject(word,"raw",raw)
          ||!cJSON_AddNumberToObject(word,"size",raw&4095U)
          ||!cJSON_AddNumberToObject(word,"length",(raw>>12)&4095U)
          ||!cJSON_AddNumberToObject(word,"successEof",(raw>>30)&1U)
          ||!cJSON_AddNumberToObject(word,"owner",raw>>31))return false;
    }
  }
  return true;
}
static inline bool pet_boot_dma_add_json(cJSON *root,const pet_boot_dma_snapshot_t *s,bool available){
  if(!root||!s||cJSON_HasObjectItem(root,"bootDma"))return false;
  cJSON *value=cJSON_CreateObject();if(!value)return false;
  bool ok=cJSON_AddBoolToObject(value,"available",available)
    &&cJSON_AddNumberToObject(value,"version",s->version)
    &&cJSON_AddBoolToObject(value,"clockWasEnabled",s->clock_was_enabled!=0)
    &&cJSON_AddBoolToObject(value,"resetApplied",s->reset_applied!=0)
    &&cJSON_AddNumberToObject(value,"stopPolls",s->stop_polls)
    &&cJSON_AddNumberToObject(value,"stoppedMask",s->stopped_mask)
    &&cJSON_AddBoolToObject(value,"complete",s->complete!=0);
  cJSON *before=ok?cJSON_AddObjectToObject(value,"before"):NULL;
  cJSON *after=before?cJSON_AddObjectToObject(value,"after"):NULL;
  if(!after||!pet_boot_dma_state_json(before,&s->before)||!pet_boot_dma_state_json(after,&s->after)
      ||!cJSON_AddItemToObject(root,"bootDma",value)){cJSON_Delete(value);return false;}
  return true;
}
