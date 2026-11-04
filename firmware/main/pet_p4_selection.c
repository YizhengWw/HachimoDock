#include "pet_p4_selection.h"
#include "pet_p4_widget_trace.h"
#include "nvs.h"
#include <string.h>
typedef struct {uint32_t magic,sequence;char id[48];} selection_t;
#define SELECTION_MAGIC 0x50534c31u
static bool safe_id(const char *id){
  if(!id||!id[0]||strlen(id)>47||id[0]<'a'||id[0]>'z')return false;
  for(const char*p=id;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'))return false;
  return true;
}
bool pet_selection_read(uint32_t sequence,char out[48],bool *record_exists){
  if(!out||!record_exists)return false;
  PET_WIDGET_TRACE_SCOPE(selection_read,PET_WTRACE_NVS);
  out[0]=0;*record_exists=false;
  nvs_handle_t handle;esp_err_t err=nvs_open("p4_widgets",NVS_READONLY,&handle);
  if(err!=ESP_OK){*record_exists=err!=ESP_ERR_NVS_NOT_FOUND;return false;}
  selection_t saved={0};size_t size=sizeof(saved);err=nvs_get_blob(handle,"selection",&saved,&size);nvs_close(handle);
  *record_exists=err!=ESP_ERR_NVS_NOT_FOUND;
  if(err!=ESP_OK||size!=sizeof(saved)||saved.magic!=SELECTION_MAGIC||saved.sequence!=sequence||!memchr(saved.id,0,sizeof(saved.id))||!safe_id(saved.id))return false;
  memcpy(out,saved.id,48);return true;
}
bool pet_selection_write(uint32_t sequence,const char *id){
  if(!safe_id(id))return false;
  char previous[48];bool exists;
  if(pet_selection_read(sequence,previous,&exists)&&!strcmp(previous,id))return true;
  /* The read scope has ended: count the NVS write without overlapping it. */
  PET_WIDGET_TRACE_SCOPE(selection_write,PET_WTRACE_NVS);
  nvs_handle_t handle;if(nvs_open("p4_widgets",NVS_READWRITE,&handle)!=ESP_OK)return false;
  selection_t saved={.magic=SELECTION_MAGIC,.sequence=sequence};memcpy(saved.id,id,strlen(id));
  esp_err_t err=nvs_set_blob(handle,"selection",&saved,sizeof(saved));
  if(err==ESP_OK)err=nvs_commit(handle);
  nvs_close(handle);return err==ESP_OK;
}
