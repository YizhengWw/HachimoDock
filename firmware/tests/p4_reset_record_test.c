/* Actual retained-word codec, every physical store cut point. No device IO. */
#include "pet_p4_reset_record.h"
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
static unsigned writes,cut=UINT_MAX;
bool pet_reset_test_store(volatile uint32_t *p,uint32_t value){
  if(writes++>=cut)return false;
  *p=value;
  return true;
}
static const uint32_t build[8]={1,2,3,4,5,6,7,8};
static pet_reset_record_snapshot_t observe(const pet_reset_record_storage_t *s){
  pet_reset_record_snapshot_t out={0};assert(pet_reset_record_observe(s,build,&out));return out;
}
static void enabled(void){writes=0;cut=UINT_MAX;}
int main(void){
  pet_reset_record_storage_t s={0};pet_reset_record_snapshot_t out=observe(&s);
  assert(!out.header_valid&&!out.same_build&&!out.valid_mask);
  enabled();assert(pet_reset_record_initialize(&s,build,17));assert(writes==62);
  out=observe(&s);assert(out.header_valid&&out.same_build&&!out.valid_mask);
  for(unsigned actor=0;actor<PET_RESET_ACTORS;actor++){
    assert(pet_reset_record_write(&s,actor,17,1,3,actor&1,11));
    assert(pet_reset_record_write(&s,actor,17,2,4,actor&1,22));
  }
  out=observe(&s);assert(out.valid_mask==15&&!out.saturated_mask);
  pet_reset_record_storage_t baseline=s;
  for(unsigned actor=0;actor<4;actor++)for(unsigned n=0;n<=7;n++){
    s=baseline;writes=0;cut=n;
    bool ok=pet_reset_record_write(&s,actor,17,3,5,actor&1,33);
    out=observe(&s);assert(ok==(n==7)&&out.valid_mask==15);
    assert(out.records[actor].sequence==(n==7?3:2));
    for(unsigned other=0;other<4;other++)if(other!=actor)assert(out.records[other].sequence==2);
  }
  for(unsigned n=0;n<=62;n++){
    s=baseline;writes=0;cut=n;
    bool ok=pet_reset_record_initialize(&s,build,18);out=observe(&s);
    assert(ok==(n==62));
    if(!n)assert(out.header_valid&&out.valid_mask==15);
    else if(n<62)assert(!out.header_valid&&!out.valid_mask);
    else assert(out.header_valid&&out.same_build&&!out.valid_mask);
  }
  enabled();s=baseline;
  assert(!pet_reset_record_write(&s,4,17,3,1,0,0));
  assert(!pet_reset_record_write(&s,0,17,0,1,0,0));
  assert(!pet_reset_record_write(&s,0,18,3,1,0,0));
  assert(!pet_reset_record_write(&s,0,17,3,256,0,0));
  assert(!pet_reset_record_write(&s,0,17,3,1,2,0));
  assert(!pet_reset_record_write(&s,0,17,1,1,0,0));
  assert(!pet_reset_record_write(&s,0,17,2,1,0,0));
  assert(!memcmp(&s,&baseline,sizeof(s)));
  assert(pet_reset_record_write(&s,0,17,UINT32_MAX,5,0,33));
  out=observe(&s);assert(out.records[0].sequence==UINT32_MAX&&out.saturated_mask==1);
  assert(!pet_reset_record_write(&s,0,17,1,1,0,0));
  uint32_t different[8];memcpy(different,build,sizeof(build));different[7]^=1;
  assert(pet_reset_record_observe(&s,different,&out)&&out.header_valid&&!out.same_build&&!out.valid_mask);
  s=baseline;
  assert(!pet_reset_record_observe(&s,build,(pet_reset_record_snapshot_t*)&s));
  assert(!pet_reset_record_observe(&s,s.header.build,&out));
  assert(!pet_reset_record_initialize(&s,s.header.build,99));
  assert(!pet_reset_record_initialize(&s,build,0));
  assert(!pet_reset_record_observe(NULL,build,&out));
  assert(!pet_reset_record_observe(&s,NULL,&out));
  assert(!pet_reset_record_observe(&s,build,NULL));
  assert(!memcmp(&s,&baseline,sizeof(s)));
  _Alignas(4) unsigned char unaligned[sizeof(s)+4];memcpy(unaligned+1,&s,sizeof(s));
  assert(!pet_reset_record_observe((void*)(unaligned+1),build,&out));
  assert(!pet_reset_record_initialize((void*)(unaligned+1),build,99));
  assert(!pet_reset_record_write((void*)(unaligned+1),0,17,3,1,0,0));
  /* Alter each header/record word, via the byte representation (no aliasing
   * struct-array casts). A bad latest record falls back to its intact peer. */
  for(size_t word=0;word<sizeof(s.header)/4;word++){
    s=baseline;((unsigned char*)&s.header)[word*4]^=1;out=observe(&s);assert(!out.header_valid&&!out.valid_mask);
  }
  for(unsigned actor=0;actor<4;actor++)for(size_t word=0;word<sizeof(s.records[0][0])/4;word++){
    s=baseline;((unsigned char*)&s.records[actor][0])[word*4]^=1;out=observe(&s);
    assert(out.valid_mask==15&&out.records[actor].sequence==1);
  }
  s=baseline;s.records[1][0]=s.records[0][0];out=observe(&s);assert(out.records[1].sequence==1);
  unsigned seed=7;
  for(unsigned i=0;i<10000;i++){
    unsigned char *p=(unsigned char*)&s;
    for(size_t j=0;j<sizeof(s);j++){seed=seed*1664525U+1013904223U;p[j]=(unsigned char)(seed>>24);}
    out=observe(&s);assert(!out.header_valid&&!out.valid_mask);
  }
  puts("retained record bounds/build/boot/actor/torn-store cases passed");return 0;
}
