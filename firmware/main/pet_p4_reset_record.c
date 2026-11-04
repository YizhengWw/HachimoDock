#include "pet_p4_reset_record.h"
#if PET_P4_RESET_PHASE_TRACE || defined(PET_RESET_PHASE_HOST_TEST)
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define PET_RESET_RECORD_IRAM IRAM_ATTR
#else
#define PET_RESET_RECORD_IRAM
#endif

#define RECORD_MAGIC UINT32_C(0x50545231)
#define COMMIT_SALT UINT32_C(0xa6935c4f)
static bool overlap(const void *a,size_t an,const void *b,size_t bn){
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
static uint32_t PET_RESET_RECORD_IRAM mix(uint32_t h,uint32_t word){
  for(unsigned j=0;j<4;j++)h=(h^(uint8_t)(word>>(8*j)))*UINT32_C(16777619);
  return h;
}
static uint32_t PET_RESET_RECORD_IRAM header_sum(const pet_reset_record_header_t *h){
  uint32_t v=mix(mix(UINT32_C(2166136261),h->magic),h->schema);
  for(unsigned i=0;i<8;i++)v=mix(v,h->build[i]);
  return mix(v,h->boot_nonce);
}
static uint32_t PET_RESET_RECORD_IRAM record_sum(const pet_reset_phase_record_t *r){
  return mix(mix(mix(mix(UINT32_C(2166136261),r->sequence),r->boot_nonce),r->phase_core),r->cycles);
}
static bool PET_RESET_RECORD_IRAM store(volatile uint32_t *p,uint32_t value){
#ifdef PET_RESET_PHASE_STORE_TEST
  extern bool pet_reset_test_store(volatile uint32_t*,uint32_t);
  return pet_reset_test_store(p,value);
#else
  *p=value;return true;
#endif
}
static void PET_RESET_RECORD_IRAM read_header(const volatile pet_reset_record_header_t *s,pet_reset_record_header_t *d){
  d->magic=s->magic;d->schema=s->schema;for(unsigned i=0;i<8;i++)d->build[i]=s->build[i];
  d->boot_nonce=s->boot_nonce;d->checksum=s->checksum;d->commit=s->commit;
  atomic_thread_fence(memory_order_acquire);
}
static void PET_RESET_RECORD_IRAM read_record(const volatile pet_reset_phase_record_t *s,pet_reset_phase_record_t *d){
  d->sequence=s->sequence;d->boot_nonce=s->boot_nonce;d->phase_core=s->phase_core;
  d->cycles=s->cycles;d->checksum=s->checksum;d->commit=s->commit;
  atomic_thread_fence(memory_order_acquire);
}
static bool PET_RESET_RECORD_IRAM aligned(const void *p){return ((uintptr_t)p&3U)==0;}
static bool PET_RESET_RECORD_IRAM header_valid(const pet_reset_record_header_t *h){
  return h->magic==RECORD_MAGIC&&h->schema==PET_RESET_RECORD_SCHEMA&&h->boot_nonce
    &&h->commit==(h->boot_nonce^COMMIT_SALT)
    &&h->checksum==header_sum(h);
}
static bool PET_RESET_RECORD_IRAM record_valid(const pet_reset_phase_record_t *r,uint32_t actor,uint32_t nonce){
  return r->sequence&&r->boot_nonce==nonce&&(r->phase_core>>24)==0xc3
    &&((r->phase_core>>16)&255)==actor&&((r->phase_core>>8)&255)<2
    &&r->commit==(r->sequence^COMMIT_SALT)
    &&r->checksum==record_sum(r);
}
bool pet_reset_record_observe(const volatile pet_reset_record_storage_t *storage,
  const uint32_t build[8],pet_reset_record_snapshot_t *out){
  if(!storage||!build||!out||!aligned((const void*)storage)||!aligned(build)||!aligned(out)
      ||overlap((const void*)storage,sizeof(*storage),out,sizeof(*out))
      ||overlap((const void*)storage,sizeof(*storage),build,32)
      ||overlap(build,32,out,sizeof(*out)))return false;
  pet_reset_record_snapshot_t next={0};pet_reset_record_header_t h;
  read_header(&storage->header,&h);next.header_valid=header_valid(&h);
  next.same_build=next.header_valid&&!memcmp(h.build,build,32);
  if(next.same_build)for(unsigned actor=0;actor<PET_RESET_ACTORS;actor++){
    pet_reset_phase_record_t a,b;read_record(&storage->records[actor][0],&a);read_record(&storage->records[actor][1],&b);
    bool av=record_valid(&a,actor,h.boot_nonce),bv=record_valid(&b,actor,h.boot_nonce);
    const pet_reset_phase_record_t *r=bv&&(!av||b.sequence>a.sequence)?&b:&a;
    if(av||bv){next.records[actor]=*r;next.valid_mask|=(uint8_t)(1U<<actor);if(r->sequence==UINT32_MAX)next.saturated_mask|=(uint8_t)(1U<<actor);}
  }
  *out=next;return true;
}
bool pet_reset_record_initialize(volatile pet_reset_record_storage_t *storage,
  const uint32_t build[8],uint32_t boot_nonce){
  if(!storage||!build||!boot_nonce||!aligned((const void*)storage)||!aligned(build)
      ||overlap((const void*)storage,sizeof(*storage),build,32))return false;
  pet_reset_record_header_t h={.magic=RECORD_MAGIC,.schema=PET_RESET_RECORD_SCHEMA,.boot_nonce=boot_nonce};
  memcpy(h.build,build,32);h.checksum=header_sum(&h);
  h.commit=boot_nonce^COMMIT_SALT;
  /* invalidate-before-clear: no old valid header may authorize a partial boot */
  if(!store(&storage->header.magic,0))return false;
  atomic_thread_fence(memory_order_seq_cst);
  for(unsigned actor=0;actor<PET_RESET_ACTORS;actor++)for(unsigned bank=0;bank<2;bank++){
    volatile pet_reset_phase_record_t *r=&storage->records[actor][bank];
    if(!store(&r->commit,0)||!store(&r->sequence,0)||!store(&r->boot_nonce,0)
        ||!store(&r->phase_core,0)||!store(&r->cycles,0)||!store(&r->checksum,0))return false;
  }
  if(!store(&storage->header.schema,h.schema))return false;
  for(unsigned i=0;i<8;i++)if(!store(&storage->header.build[i],h.build[i]))return false;
  if(!store(&storage->header.boot_nonce,h.boot_nonce)||!store(&storage->header.checksum,h.checksum)
      ||!store(&storage->header.commit,h.commit))return false;
  atomic_thread_fence(memory_order_seq_cst);
  return store(&storage->header.magic,RECORD_MAGIC);
}
bool PET_RESET_RECORD_IRAM pet_reset_record_write(volatile pet_reset_record_storage_t *storage,
  uint32_t actor,uint32_t boot_nonce,uint32_t sequence,uint32_t phase,uint32_t core,uint32_t cycles){
  if(!storage||!aligned((const void*)storage)||actor>=PET_RESET_ACTORS||!boot_nonce||!sequence||phase>255||core>1)return false;
  pet_reset_record_header_t header;read_header(&storage->header,&header);
  if(!header_valid(&header)||header.boot_nonce!=boot_nonce)return false;
  for(unsigned bank=0;bank<2;bank++){
    pet_reset_phase_record_t prior;read_record(&storage->records[actor][bank],&prior);
    if(record_valid(&prior,actor,boot_nonce)&&sequence<=prior.sequence)return false;
  }
  pet_reset_phase_record_t r={.sequence=sequence,.boot_nonce=boot_nonce,
    .phase_core=UINT32_C(0xc3000000)|(actor<<16)|(core<<8)|phase,.cycles=cycles};
  r.checksum=record_sum(&r);
  r.commit=sequence^COMMIT_SALT;
  volatile pet_reset_phase_record_t *target=&storage->records[actor][sequence&1U];
  if(!store(&target->commit,0))return false;
  atomic_thread_fence(memory_order_seq_cst);
  if(!store(&target->sequence,r.sequence)||!store(&target->boot_nonce,r.boot_nonce)
      ||!store(&target->phase_core,r.phase_core)||!store(&target->cycles,r.cycles)
      ||!store(&target->checksum,r.checksum))return false;
  atomic_thread_fence(memory_order_seq_cst);
  return store(&target->commit,r.commit);
}
#endif
