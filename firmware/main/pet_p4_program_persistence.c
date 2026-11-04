#include "pet_p4_program_persistence.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static char *scratch_acquire(void){return heap_caps_malloc(PET_APP_SAVE_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
static void scratch_release(void *p){heap_caps_free(p);}
#else
/* Host adapter only. Pure store/core codecs use neither heap nor global state. */
#include <stdlib.h>
static char *scratch_acquire(void){return malloc(PET_APP_SAVE_BYTES);}
static void scratch_release(void *p){free(p);}
#endif
_Static_assert(PET_APP_SAVE_BYTES >= PET_PROGRAM_STORE_MAX_TEXT_CAPACITY, "private app store must fit the complete program snapshot");

static bool overlaps(const void *a,size_t an,const void *b,size_t bn){
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
  return x<=y?y-x<an:x-y<bn;
}
static pet_program_save_result_t load_buffer(const char *id,
    const pet_program_image_t *image,pet_program_snapshot_t *seed,const pet_resource_lease_t *borrowed,bool cached,char *text){
  pet_program_t program;
  if(!id||!image||!seed||overlaps(image,sizeof(*image),seed,sizeof(*seed))
      ||!pet_program_image_program(image,&program))return PET_PROGRAM_SAVE_INVALID;
  pet_app_store_status_t status=cached?pet_app_store_read_scoped_cached(PET_PROGRAM_SAVE_DOMAIN,id,text)
    :borrowed?pet_app_store_load_scoped_with_lease(PET_PROGRAM_SAVE_DOMAIN,id,text,*borrowed)
    :pet_app_store_load_scoped(PET_PROGRAM_SAVE_DOMAIN,id,text);
  switch(status){
    case PET_APP_STORE_EMPTY:return PET_PROGRAM_SAVE_EMPTY;
    case PET_APP_STORE_BUSY:return PET_PROGRAM_SAVE_BUSY;
    case PET_APP_STORE_UNAVAILABLE:return PET_PROGRAM_SAVE_UNAVAILABLE;
    case PET_APP_STORE_INVALID:return PET_PROGRAM_SAVE_INVALID;
    case PET_APP_STORE_VALUE:break;
    default:return PET_PROGRAM_SAVE_UNAVAILABLE;
  }
  size_t length=0;while(length<PET_APP_SAVE_BYTES&&text[length])length++;
  pet_program_store_result_t decoded=pet_program_store_decode_text(seed,image->schema,
    image->cells,text,length);
  if(decoded==PET_PROGRAM_STORE_OK)return PET_PROGRAM_SAVE_VALUE;
  if(decoded==PET_PROGRAM_STORE_LAYOUT_MISMATCH)return PET_PROGRAM_SAVE_INCOMPATIBLE;
  return PET_PROGRAM_SAVE_CORRUPT;
}
static pet_program_save_result_t load(const char *id,const pet_program_image_t *image,
    pet_program_snapshot_t *seed,const pet_resource_lease_t *borrowed,bool cached){
  char *text=scratch_acquire();if(!text)return PET_PROGRAM_SAVE_UNAVAILABLE;
  pet_program_save_result_t result=load_buffer(id,image,seed,borrowed,cached,text);
  scratch_release(text);return result;
}
pet_program_save_result_t pet_program_persistence_load(const char *id,const pet_program_image_t *image,pet_program_snapshot_t *seed){
  return load(id,image,seed,NULL,false);
}
pet_program_save_result_t pet_program_persistence_load_with_lease(const char *id,const pet_program_image_t *image,pet_program_snapshot_t *seed,pet_resource_lease_t borrowed){
  return load(id,image,seed,&borrowed,false);
}
pet_program_save_result_t pet_program_persistence_load_cached(const char *id,const pet_program_image_t *image,pet_program_snapshot_t *seed){return load(id,image,seed,NULL,true);}
bool pet_program_persistence_enqueue(const char *id,const pet_program_snapshot_t *snapshot){
  char *text=scratch_acquire();if(!text)return false;
  bool accepted=pet_program_store_encode_text(text,PET_APP_SAVE_BYTES,snapshot)==PET_PROGRAM_STORE_OK
    &&pet_app_store_update_scoped(PET_PROGRAM_SAVE_DOMAIN,id,text);
  scratch_release(text);return accepted;
}
pet_app_checkpoint_status_t pet_program_persistence_checkpoint(const char *id,const pet_program_snapshot_t *snapshot){
  char *text=scratch_acquire();if(!text)return PET_APP_CHECKPOINT_BLOCKED;
  pet_app_checkpoint_status_t result=pet_program_store_encode_text(text,PET_APP_SAVE_BYTES,snapshot)==PET_PROGRAM_STORE_OK
    ?pet_app_store_checkpoint_scoped(PET_PROGRAM_SAVE_DOMAIN,id,text):PET_APP_CHECKPOINT_INVALID;
  scratch_release(text);return result;
}
