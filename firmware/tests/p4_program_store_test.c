/* Offline portable snapshot and real runtime seed tests; no filesystem/audio. */
#include "pet_p4_program_store.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static pet_program_snapshot_t sample, decoded, saved;
static uint8_t wire[PET_PROGRAM_STORE_BYTES + 1U], original[PET_PROGRAM_STORE_BYTES + 1U];
static char text[PET_PROGRAM_STORE_TEXT_CAPACITY + 1U], original_text[PET_PROGRAM_STORE_TEXT_CAPACITY + 1U];
static void put32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8U)); }
static void repair_crc(uint8_t *p) {
  uint32_t crc=UINT32_MAX;
  for(unsigned i=0;i<PET_PROGRAM_STORE_BYTES;i++) {
    if(i>=28U&&i<32U)continue;
    crc^=p[i];for(unsigned b=0;b<8;b++)crc=(crc>>1)^((0U-(crc&1U))&UINT32_C(0xedb88320));
  }
  put32(p+28,~crc);
}
static void fixture(void) {
  memset(&sample,0,sizeof(sample));sample.schema=UINT32_C(0x12345678);sample.cells=3;sample.revision=9;
  sample.values[0]=INT32_MIN;sample.values[1]=INT32_MAX;sample.values[2]=-1;
  for(unsigned i=0;i<PET_PROGRAM_OUTPUTS;i++)sample.outputs[i]=(int32_t)i-4;
}
static void preserve(void) {memset(&decoded,0xa5,sizeof(decoded));memcpy(&saved,&decoded,sizeof(saved));}
static bool unchanged(void) {return !memcmp(&decoded,&saved,sizeof(decoded));}
static int roundtrip(void) {
  fixture();memset(wire,0xa5,sizeof(wire));
  CHECK(pet_program_store_encode(wire,sizeof(wire),&sample)==PET_PROGRAM_STORE_OK);
  CHECK(wire[PET_PROGRAM_STORE_BYTES]==0xa5);
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_OK);
  CHECK(!memcmp(&decoded,&sample,sizeof(sample)));
  uint8_t unaligned[PET_PROGRAM_STORE_BYTES+1U];memcpy(unaligned+1,wire,PET_PROGRAM_STORE_BYTES);
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,unaligned+1,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_OK);
  for(unsigned cells=0;cells<=PET_PROGRAM_CELLS;cells++) {
    memset(&sample,0,sizeof(sample));sample.schema=UINT32_MAX;sample.cells=(uint8_t)cells;sample.revision=UINT32_MAX;
    for(unsigned i=0;i<cells;i++)sample.values[i]=(i&1U)?INT32_MAX:INT32_MIN;
    for(unsigned i=0;i<PET_PROGRAM_OUTPUTS;i++)sample.outputs[i]=(i&1U)?INT32_MIN:INT32_MAX;
    CHECK(pet_program_store_encode(wire,PET_PROGRAM_STORE_BYTES,&sample)==PET_PROGRAM_STORE_OK);
    CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_OK);
    CHECK(!memcmp(&decoded,&sample,sizeof(sample)));
  }
  return 0;
}
static int bad_wire(void) {
  fixture();CHECK(pet_program_store_encode(wire,sizeof(wire),&sample)==PET_PROGRAM_STORE_OK);
  memcpy(original,wire,sizeof(wire));preserve();
  for(size_t length=0;length<PET_PROGRAM_STORE_BYTES;length++) {
    CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,length)==PET_PROGRAM_STORE_BAD_LENGTH&&unchanged());
  }
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES+1U)==PET_PROGRAM_STORE_BAD_LENGTH&&unchanged());
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,SIZE_MAX)==PET_PROGRAM_STORE_BAD_LENGTH&&unchanged());
  for(unsigned i=0;i<PET_PROGRAM_STORE_BYTES;i++) {
    memcpy(wire,original,sizeof(wire));wire[i]^=1U;
    CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES)!=PET_PROGRAM_STORE_OK&&unchanged());
  }
  const unsigned headers[]={0,1,2,3,4,5,6,7,8,9,10,11,21,22,23,24,25,26,27};
  for(unsigned i=0;i<sizeof(headers)/sizeof(*headers);i++) {
    memcpy(wire,original,sizeof(wire));wire[headers[i]]^=128U;repair_crc(wire);
    CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_BAD_HEADER&&unchanged());
  }
  memcpy(wire,original,sizeof(wire));put32(wire+8,UINT32_MAX);repair_crc(wire);
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_BAD_HEADER&&unchanged());
  memcpy(wire,original,sizeof(wire));put32(wire+12,0);repair_crc(wire);
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_BAD_STATE&&unchanged());
  memcpy(wire,original,sizeof(wire));wire[20]=65;repair_crc(wire);
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_BAD_STATE&&unchanged());
  memcpy(wire,original,sizeof(wire));put32(wire+32U+sample.cells*4U,7);repair_crc(wire);
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_BAD_STATE&&unchanged());
  memcpy(wire,original,sizeof(wire));
  CHECK(pet_program_store_decode(&decoded,sample.schema+1U,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_LAYOUT_MISMATCH&&unchanged());
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells+1U,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_LAYOUT_MISMATCH&&unchanged());
  return 0;
}
static int bad_snapshot(void) {
  fixture();memset(wire,0xa5,sizeof(wire));memcpy(original,wire,sizeof(wire));
  for(size_t capacity=0;capacity<PET_PROGRAM_STORE_BYTES;capacity++) {
    CHECK(pet_program_store_encode(wire,capacity,&sample)==PET_PROGRAM_STORE_BAD_LENGTH);
    CHECK(!memcmp(wire,original,sizeof(wire)));
  }
  sample.schema=0;CHECK(pet_program_store_encode(wire,sizeof(wire),&sample)==PET_PROGRAM_STORE_BAD_STATE&&!memcmp(wire,original,sizeof(wire)));
  fixture();sample.cells=65;CHECK(pet_program_store_encode(wire,sizeof(wire),&sample)==PET_PROGRAM_STORE_BAD_STATE&&!memcmp(wire,original,sizeof(wire)));
  fixture();sample.values[3]=1;CHECK(pet_program_store_encode(wire,sizeof(wire),&sample)==PET_PROGRAM_STORE_BAD_STATE&&!memcmp(wire,original,sizeof(wire)));
  fixture();CHECK(pet_program_store_encode(wire,SIZE_MAX,&sample)==PET_PROGRAM_STORE_OK&&wire[PET_PROGRAM_STORE_BYTES]==0xa5);
  return 0;
}
static int arguments(void) {
  fixture();preserve();
  CHECK(pet_program_store_encode(NULL,PET_PROGRAM_STORE_BYTES,&sample)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_encode(wire,sizeof(wire),NULL)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_decode(NULL,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,NULL,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_INVALID_ARGUMENT&&unchanged());
  CHECK(pet_program_store_decode(&decoded,0,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_INVALID_ARGUMENT&&unchanged());
  CHECK(pet_program_store_decode(&decoded,sample.schema,65,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_INVALID_ARGUMENT&&unchanged());
  uint8_t *wrapping=(uint8_t *)(uintptr_t)(UINTPTR_MAX-8U);
  CHECK(pet_program_store_encode(wrapping,PET_PROGRAM_STORE_BYTES,&sample)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_decode(&decoded,sample.schema,sample.cells,wrapping,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_INVALID_ARGUMENT&&unchanged());
  pet_program_snapshot_t *wrapping_snapshot=(pet_program_snapshot_t *)(uintptr_t)(UINTPTR_MAX-3U);
  CHECK(pet_program_store_encode(wire,sizeof(wire),wrapping_snapshot)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_decode(wrapping_snapshot,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_encode_text(NULL,PET_PROGRAM_STORE_TEXT_CAPACITY,&sample)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_encode_text(text,sizeof(text),NULL)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_encode_text((char *)wrapping,PET_PROGRAM_STORE_TEXT_CAPACITY,&sample)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_encode_text(text,sizeof(text),wrapping_snapshot)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_decode_text(NULL,sample.schema,sample.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_decode_text(&decoded,sample.schema,sample.cells,NULL,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_INVALID_ARGUMENT&&unchanged());
  CHECK(pet_program_store_decode_text(&decoded,0,sample.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_INVALID_ARGUMENT&&unchanged());
  CHECK(pet_program_store_decode_text(&decoded,sample.schema,65,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_INVALID_ARGUMENT&&unchanged());
  CHECK(pet_program_store_decode_text(wrapping_snapshot,sample.schema,sample.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_decode_text(&decoded,sample.schema,sample.cells,(char *)wrapping,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_INVALID_ARGUMENT&&unchanged());
  union {pet_program_snapshot_t snapshot;uint8_t bytes[2U*PET_PROGRAM_STORE_TEXT_CAPACITY];} alias;
  uint8_t before[sizeof(alias)];memset(&alias,0xa5,sizeof(alias));alias.snapshot=sample;memcpy(before,&alias,sizeof(alias));
  for(unsigned offset=0;offset<sizeof(sample);offset++) {
    CHECK(pet_program_store_encode(alias.bytes+offset,PET_PROGRAM_STORE_BYTES,&alias.snapshot)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
    CHECK(!memcmp(before,&alias,sizeof(alias)));
    CHECK(pet_program_store_decode(&alias.snapshot,sample.schema,sample.cells,alias.bytes+offset,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
    CHECK(!memcmp(before,&alias,sizeof(alias)));
  }
  CHECK(pet_program_store_encode(alias.bytes+sizeof(sample),PET_PROGRAM_STORE_BYTES,&alias.snapshot)==PET_PROGRAM_STORE_OK);
  CHECK(pet_program_store_decode(&alias.snapshot,sample.schema,sample.cells,alias.bytes+sizeof(sample),PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_OK);
  CHECK(!memcmp(&alias.snapshot,&sample,sizeof(sample)));
  CHECK(pet_program_store_encode_text((char *)alias.bytes,sizeof(alias),&alias.snapshot)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  CHECK(pet_program_store_decode_text(&alias.snapshot,sample.schema,sample.cells,(char *)alias.bytes,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
  return 0;
}
static int text_codec(void) {
  fixture();memset(text,0xa5,sizeof(text));CHECK(pet_program_store_encode_text(text,sizeof(text),&sample)==PET_PROGRAM_STORE_OK);
  CHECK(strlen(text)==PET_PROGRAM_STORE_TEXT_LENGTH&&(uint8_t)text[PET_PROGRAM_STORE_TEXT_CAPACITY]==0xa5);
  CHECK(pet_program_store_decode_text(&decoded,sample.schema,sample.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_OK);
  CHECK(!memcmp(&decoded,&sample,sizeof(sample)));memcpy(original_text,text,sizeof(text));preserve();
  for(size_t n=0;n<PET_PROGRAM_STORE_TEXT_LENGTH;n++) {
    CHECK(pet_program_store_decode_text(&decoded,sample.schema,sample.cells,text,n)==PET_PROGRAM_STORE_BAD_LENGTH&&unchanged());
  }
  CHECK(pet_program_store_decode_text(&decoded,sample.schema,sample.cells,text,PET_PROGRAM_STORE_TEXT_CAPACITY)==PET_PROGRAM_STORE_BAD_LENGTH&&unchanged());
  CHECK(pet_program_store_decode_text(&decoded,sample.schema,sample.cells,text,SIZE_MAX)==PET_PROGRAM_STORE_BAD_LENGTH&&unchanged());
  for(unsigned i=0;i<PET_PROGRAM_STORE_TEXT_LENGTH;i++) {
    memcpy(text,original_text,sizeof(text));text[i]='!';
    CHECK(pet_program_store_decode_text(&decoded,sample.schema,sample.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_BAD_ENCODING&&unchanged());
  }
  memcpy(text,original_text,sizeof(text));text[9]='A';
  CHECK(pet_program_store_decode_text(&decoded,sample.schema,sample.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_BAD_ENCODING&&unchanged());
  memcpy(text,original_text,sizeof(text));text[9U+2U*32U]='1';
  CHECK(pet_program_store_decode_text(&decoded,sample.schema,sample.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_BAD_CHECKSUM&&unchanged());
  memcpy(text,original_text,sizeof(text));
  CHECK(pet_program_store_decode_text(&decoded,sample.schema+1U,sample.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_LAYOUT_MISMATCH&&unchanged());
  for(size_t n=0;n<PET_PROGRAM_STORE_TEXT_CAPACITY;n++) {
    CHECK(pet_program_store_encode_text(text,n,&sample)==PET_PROGRAM_STORE_BAD_LENGTH&&!memcmp(text,original_text,sizeof(text)));
  }
  sample.values[sample.cells]=1;CHECK(pet_program_store_encode_text(text,sizeof(text),&sample)==PET_PROGRAM_STORE_BAD_STATE&&!memcmp(text,original_text,sizeof(text)));
  return 0;
}
static bool current(void *context,uint64_t generation) {return *(uint64_t *)context==generation;}
static pet_program_runtime_result_t complete(pet_program_runtime_t *r,uint64_t generation) {
  for(unsigned i=0;i<100;i++){pet_program_runtime_result_t result=pet_program_runtime_pump(r,generation);if(result!=PET_RUNTIME_YIELDED)return result;}
  return PET_RUNTIME_INVALID;
}
static int runtime_seed(void) {
  pet_program_image_t image={.schema=42,.cells=1,.count=7,.fuel=64,.valid=true,
    .code={{PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
      {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}}};
  pet_program_runtime_t first,second;uint64_t generation=1;pet_program_event_t event={1,{13,0,0,0}};
  pet_program_runtime_init(&first);CHECK(pet_program_runtime_start(&first,&image,1,current,&generation,1,NULL)==PET_RUNTIME_OK);
  CHECK(pet_program_runtime_enqueue(&first,1,9,&event)==PET_RUNTIME_OK&&complete(&first,1)==PET_RUNTIME_COMPLETE);
  CHECK(pet_program_runtime_snapshot(&first,1,&sample)==PET_RUNTIME_OK&&sample.revision==1&&sample.values[0]==13&&sample.outputs[0]==13);
  CHECK(pet_program_store_encode_text(text,sizeof(text),&sample)==PET_PROGRAM_STORE_OK);
  CHECK(pet_program_store_decode_text(&decoded,image.schema,image.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_OK);
  generation=2;pet_program_runtime_init(&second);
  CHECK(pet_program_runtime_start(&second,&image,2,current,&generation,2,&decoded)==PET_RUNTIME_OK);
  CHECK(second.queued==0&&second.inflight==0&&second.last_sequence==0&&!second.report_pending);
  event.words[0]=7;CHECK(pet_program_runtime_enqueue(&second,2,1,&event)==PET_RUNTIME_OK&&complete(&second,2)==PET_RUNTIME_COMPLETE);
  CHECK(pet_program_runtime_snapshot(&second,2,&sample)==PET_RUNTIME_OK&&sample.revision==2&&sample.values[0]==20&&sample.outputs[0]==20);
  /* Neither effect/receipt replay nor implicit schema migration is stored. */
  preserve();CHECK(pet_program_store_decode_text(&decoded,image.schema+1U,image.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_LAYOUT_MISMATCH&&unchanged());
  return 0;
}
static int terminal_revision(void) {
  pet_program_image_t image={.schema=42,.cells=1,.count=7,.fuel=64,.valid=true,
    .code={{PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
      {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}}};
  pet_program_runtime_t first,second;uint64_t generation=1;pet_program_event_t event={1,{13,0,0,0}};
  pet_program_report_t report;memset(&sample,0,sizeof(sample));
  sample.schema=image.schema;sample.cells=image.cells;sample.revision=UINT32_MAX-1U;
  sample.values[0]=5;sample.outputs[0]=5;
  pet_program_runtime_init(&first);
  CHECK(pet_program_runtime_start(&first,&image,1,current,&generation,1,&sample)==PET_RUNTIME_OK);
  CHECK(pet_program_runtime_enqueue(&first,1,9,&event)==PET_RUNTIME_OK&&complete(&first,1)==PET_RUNTIME_COMPLETE);
  CHECK(pet_program_runtime_take_report(&first,1,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_COMMITTED&&report.revision==UINT32_MAX);
  CHECK(pet_program_runtime_snapshot(&first,1,&sample)==PET_RUNTIME_OK&&sample.revision==UINT32_MAX&&sample.values[0]==18&&sample.outputs[0]==18);
  CHECK(pet_program_store_encode(wire,sizeof(wire),&sample)==PET_PROGRAM_STORE_OK);
  CHECK(pet_program_store_decode(&decoded,image.schema,image.cells,wire,PET_PROGRAM_STORE_BYTES)==PET_PROGRAM_STORE_OK&&!memcmp(&decoded,&sample,sizeof(sample)));
  CHECK(pet_program_store_encode_text(text,sizeof(text),&decoded)==PET_PROGRAM_STORE_OK);
  CHECK(pet_program_store_decode_text(&saved,image.schema,image.cells,text,PET_PROGRAM_STORE_TEXT_LENGTH)==PET_PROGRAM_STORE_OK&&!memcmp(&saved,&sample,sizeof(sample)));
  generation=2;pet_program_runtime_init(&second);
  CHECK(pet_program_runtime_start(&second,&image,2,current,&generation,2,&saved)==PET_RUNTIME_OK);
  CHECK(second.queued==0&&second.inflight==0&&second.last_sequence==0&&!second.report_pending);
  CHECK(pet_program_runtime_pump(&second,2)==PET_RUNTIME_IDLE);
  CHECK(pet_program_runtime_snapshot(&second,2,&decoded)==PET_RUNTIME_OK&&!memcmp(&decoded,&saved,sizeof(saved)));
  event.words[0]=7;CHECK(pet_program_runtime_enqueue(&second,2,1,&event)==PET_RUNTIME_OK);
  CHECK(pet_program_runtime_pump(&second,2)==PET_RUNTIME_COMPLETE);
  CHECK(pet_program_runtime_take_report(&second,2,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_OVERFLOW&&report.revision==UINT32_MAX&&report.used_fuel==0);
  CHECK(pet_program_runtime_snapshot(&second,2,&decoded)==PET_RUNTIME_OK&&!memcmp(&decoded,&saved,sizeof(saved)));
  CHECK(pet_program_runtime_enqueue(&second,2,1,&event)==PET_RUNTIME_DUPLICATE);
  CHECK(pet_program_runtime_pump(&second,2)==PET_RUNTIME_IDLE);
  return 0;
}
static uint32_t random_word(uint32_t *s){*s=*s*UINT32_C(1664525)+UINT32_C(1013904223);return *s;}
static int fuzz(void) {
  uint32_t seed=UINT32_C(0x4a11d00d);fixture();CHECK(pet_program_store_encode(wire,sizeof(wire),&sample)==PET_PROGRAM_STORE_OK);memcpy(original,wire,sizeof(wire));
  for(unsigned trial=0;trial<10000;trial++) {
    memcpy(wire,original,sizeof(wire));unsigned index=random_word(&seed)%PET_PROGRAM_STORE_BYTES;wire[index]^=(uint8_t)((random_word(&seed)%255U)+1U);
    if(trial&1U)repair_crc(wire);
    preserve();pet_program_store_result_t result=pet_program_store_decode(&decoded,sample.schema,sample.cells,wire,PET_PROGRAM_STORE_BYTES);
    if(result!=PET_PROGRAM_STORE_OK)CHECK(unchanged());
    else {uint8_t canonical[PET_PROGRAM_STORE_BYTES];CHECK(pet_program_store_encode(canonical,sizeof(canonical),&decoded)==PET_PROGRAM_STORE_OK&&!memcmp(canonical,wire,sizeof(canonical)));}
  }
  return 0;
}
int main(int argc,char **argv) {
  if(argc>1&&!strcmp(argv[1],"dump")){fixture();if(pet_program_store_encode(wire,sizeof(wire),&sample))return 1;for(unsigned i=0;i<PET_PROGRAM_STORE_BYTES;i++)printf("%02x",wire[i]);puts("");return 0;}
  int (*cases[])(void)={roundtrip,bad_wire,bad_snapshot,arguments,text_codec,runtime_seed,fuzz,terminal_revision};
  unsigned which=argc>1?(unsigned)strtoul(argv[1],NULL,10):6U;if(which>=sizeof(cases)/sizeof(*cases))return 2;
  int failed=cases[which]();printf("{\"ok\":%s,\"case\":%u,\"failedLine\":%d,\"wireBytes\":%u,\"textCapacity\":%u}\n",failed?"false":"true",which,failed,PET_PROGRAM_STORE_BYTES,PET_PROGRAM_STORE_TEXT_CAPACITY);
  return failed?1:0;
}
