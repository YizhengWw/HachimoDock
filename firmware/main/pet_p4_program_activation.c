#include "pet_p4_program_activation.h"
#include "pet_p4_resources_device.h"
#include <string.h>

static bool overlaps(const void *a,size_t an,const void *b,size_t bn){
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
  return x<=y?y-x<an:x-y<bn;
}
static bool valid_id(const char *id){
  if(!id||id[0]<'a'||id[0]>'z')return false;
  for(unsigned i=0;i<48;i++){
    unsigned char c=(unsigned char)id[i];if(!c)return true;
    if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;
  }
  return false;
}
static bool storage_current(pet_resource_lease_t storage){
  return storage.kind==PET_RESOURCE_STORAGE_WORK&&storage.units==1
    &&pet_resources_device_current(storage);
}
pet_program_prepare_result_t pet_program_activation_prepare(pet_program_prepared_t *p,
    const char *id,const unsigned char identity[32],uint64_t operation,
    const uint8_t *bytes,size_t length,pet_resource_lease_t storage){
  if(!p||!id||!identity||!bytes||!operation
      ||overlaps(p,sizeof(*p),id,48)||overlaps(p,sizeof(*p),identity,32)
      ||overlaps(p,sizeof(*p),bytes,length))return PET_PROGRAM_PREPARE_INVALID;
  if(p->inside||p->valid)return PET_PROGRAM_PREPARE_BUSY;
  if(!valid_id(id))return PET_PROGRAM_PREPARE_INVALID;
  if(!storage_current(storage))return PET_PROGRAM_PREPARE_CANCELLED;
  memset(p,0,sizeof(*p));p->inside=true;
  pet_program_prepare_result_t result=PET_PROGRAM_PREPARE_BAD_IMAGE;
  if(pet_program_image_decode(&p->image,bytes,length)!=PET_PROGRAM_IMAGE_OK)goto done;
  if(!storage_current(storage)){result=PET_PROGRAM_PREPARE_CANCELLED;goto done;}
  pet_program_save_result_t loaded=pet_program_persistence_load_with_lease(id,&p->image,&p->seed,storage);
  if(loaded==PET_PROGRAM_SAVE_BUSY){result=PET_PROGRAM_PREPARE_BUSY;goto done;}
  if(loaded!=PET_PROGRAM_SAVE_VALUE&&loaded!=PET_PROGRAM_SAVE_EMPTY){result=PET_PROGRAM_PREPARE_SAVE_ERROR;goto done;}
  if(!storage_current(storage)){result=PET_PROGRAM_PREPARE_CANCELLED;goto done;}
  p->has_seed=loaded==PET_PROGRAM_SAVE_VALUE;
  memcpy(p->component_id,id,strlen(id)+1);memcpy(p->package_identity,identity,32);
  p->operation=operation;p->storage=storage;p->valid=true;result=PET_PROGRAM_PREPARED;
done:
  p->inside=false;return result;
}
static pet_program_runtime_result_t publish(pet_program_prepared_t *p,
    pet_program_host_t *h,const char *id,const unsigned char identity[32],
    uint64_t operation,uint16_t quantum,const pet_program_output_contract_t *contract,
    const pet_program_ui_projection_t *projection,uint64_t *generation){
  if(!p||!h||!id||!identity||!generation
      ||overlaps(p,sizeof(*p),h,sizeof(*h))||overlaps(p,sizeof(*p),generation,sizeof(*generation))
      ||overlaps(h,sizeof(*h),generation,sizeof(*generation))
      ||overlaps(p,sizeof(*p),id,48)||overlaps(p,sizeof(*p),identity,32)
      ||overlaps(h,sizeof(*h),id,48)||overlaps(h,sizeof(*h),identity,32)
      ||overlaps(generation,sizeof(*generation),id,48)||overlaps(generation,sizeof(*generation),identity,32)
      ||(contract&&(overlaps(contract,sizeof(*contract),p,sizeof(*p))||overlaps(contract,sizeof(*contract),h,sizeof(*h))
          ||overlaps(contract,sizeof(*contract),generation,sizeof(*generation))))
      ||(projection&&(contract||overlaps(projection,sizeof(*projection),p,sizeof(*p))||overlaps(projection,sizeof(*projection),h,sizeof(*h))
          ||overlaps(projection,sizeof(*projection),generation,sizeof(*generation)))))return PET_RUNTIME_INVALID;
  if(p->inside)return PET_RUNTIME_BUSY;
  if(!p->valid||!valid_id(id))return PET_RUNTIME_INVALID;
  if(p->operation!=operation||strcmp(p->component_id,id)||memcmp(p->package_identity,identity,32))return PET_RUNTIME_STALE;
  if(!storage_current(p->storage))return PET_RUNTIME_DENIED;
  p->inside=true;
  pet_program_runtime_result_t result=projection
    ?pet_program_host_start_image_with_projection(h,&p->image,quantum,p->has_seed?&p->seed:NULL,projection,generation)
    :pet_program_host_start_image_with_contract(h,&p->image,quantum,p->has_seed?&p->seed:NULL,contract,generation);
  if(result==PET_RUNTIME_OK)p->valid=false;
  p->inside=false;return result;
}
pet_program_runtime_result_t pet_program_activation_publish_with_contract(pet_program_prepared_t *p,
    pet_program_host_t *h,const char *id,const unsigned char identity[32],
    uint64_t operation,uint16_t quantum,const pet_program_output_contract_t *contract,uint64_t *generation){
  return publish(p,h,id,identity,operation,quantum,contract,NULL,generation);
}
pet_program_runtime_result_t pet_program_activation_publish_with_projection(pet_program_prepared_t *p,
    pet_program_host_t *h,const char *id,const unsigned char identity[32],
    uint64_t operation,uint16_t quantum,const pet_program_ui_projection_t *projection,uint64_t *generation){
  if(!projection)return PET_RUNTIME_INVALID;
  return publish(p,h,id,identity,operation,quantum,NULL,projection,generation);
}
pet_program_runtime_result_t pet_program_activation_publish(pet_program_prepared_t *p,
    pet_program_host_t *h,const char *id,const unsigned char identity[32],
    uint64_t operation,uint16_t quantum,uint64_t *generation){
  return pet_program_activation_publish_with_contract(p,h,id,identity,operation,quantum,NULL,generation);
}
bool pet_program_activation_discard(pet_program_prepared_t *p){
  if(!p||p->inside)return false;
  memset(p,0,sizeof(*p));return true;
}
