/* Same code on native sanitizers and fixed-memory WASM, no providers/IO. */
#include "pet_p4_program_runtime.h"
#include <limits.h>
#ifndef PET_PROGRAM_WASM
#include <stdio.h>
#endif
#define CHECK(c) do{if(!(c))return __LINE__;}while(0)
static bool live=true;
static unsigned observations;
static bool current(void *context,uint64_t generation){(void)context;observations++;return live&&generation>0;}
static void copy(void *out,const void *in,size_t n){unsigned char *a=out;const unsigned char *b=in;for(size_t i=0;i<n;i++)a[i]=b[i];}
static bool equal(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return false;return true;}
static pet_program_image_t image(void){
  pet_program_image_t p={.schema=9,.count=5,.fuel=32,.cells=1,.valid=true};
  p.code[0]=(pet_program_insn_t){PET_OP_CONST,0,0,0,0};
  p.code[1]=(pet_program_insn_t){PET_OP_EVENT,1,0,0,0};
  p.code[2]=(pet_program_insn_t){PET_OP_STORE,0,1,0,0};
  p.code[3]=(pet_program_insn_t){PET_OP_OUTPUT,0,1,0,0};
  p.code[4]=(pet_program_insn_t){PET_OP_HALT,0,0,0,0};return p;
}
static pet_program_output_contract_t contract(int32_t lo,int32_t hi){
  pet_program_output_contract_t c;
  for(unsigned i=0;i<8;i++){c.minimum[i]=INT32_MIN;c.maximum[i]=INT32_MAX;}
  c.minimum[0]=lo;c.maximum[0]=hi;return c;
}
static int pump(pet_program_runtime_t *r,uint64_t generation,pet_program_report_t *out){
  for(unsigned i=0;i<40;i++){
    pet_program_runtime_result_t result=pet_program_runtime_pump(r,generation);
    if(result==PET_RUNTIME_COMPLETE){CHECK(pet_program_runtime_take_report(r,generation,out)==PET_RUNTIME_OK);return 0;}
    CHECK(result==PET_RUNTIME_YIELDED);
  }
  return __LINE__;
}
static int core_atomic(void){
  pet_program_image_t im=image();pet_program_t p;CHECK(pet_program_image_program(&im,&p));
  pet_program_state_t state,before;pet_program_workspace_t w,work_before;
  pet_program_authority_t authority={1,current,NULL,NULL,0};CHECK(pet_program_init(&state,1));
  state.cells[0]=55;state.outputs[0]=4;copy(&before,&state,sizeof(state));
  pet_program_event_t event={1,{101}};pet_program_output_contract_t c=contract(-100,100);
  CHECK(pet_program_run(&p,&event,&state,&authority,&w)==PET_PROGRAM_READY);
  CHECK(w.candidate.cells[0]==101&&state.cells[0]==55);
  CHECK(pet_program_commit_with_contract(&state,&w,&authority,&c)==PET_PROGRAM_BOUNDS);
  CHECK(equal(&before,&state,sizeof(state))&&!w.ready&&w.used_fuel==5);
  CHECK(pet_program_commit(&state,&w,&authority)==PET_PROGRAM_NOT_READY);
  event.words[0]=-100;CHECK(pet_program_run(&p,&event,&state,&authority,&w)==PET_PROGRAM_READY);
  copy(&work_before,&w,sizeof(w));
  CHECK(pet_program_commit_with_contract(&state,&w,&authority,(const void*)&state.cells[0])==PET_PROGRAM_INVALID);
  CHECK(equal(&before,&state,sizeof(state))&&equal(&work_before,&w,sizeof(w)));
  CHECK(pet_program_commit_with_contract(&state,&w,&authority,&c)==PET_PROGRAM_COMMITTED);
  CHECK(state.cells[0]==-100&&state.outputs[0]==-100&&state.revision==1);
  copy(&before,&state,sizeof(state));event.words[0]=100;
  CHECK(pet_program_run(&p,&event,&state,&authority,&w)==PET_PROGRAM_READY);
  c.minimum[1]=1;c.maximum[1]=0;
  CHECK(pet_program_commit_with_contract(&state,&w,&authority,&c)==PET_PROGRAM_INVALID);
  CHECK(equal(&before,&state,sizeof(state)));
  CHECK(!pet_program_output_contract_valid(&c)&&!pet_program_outputs_fit(state.outputs,&c));
  CHECK(pet_program_outputs_fit(state.outputs,NULL)&&!pet_program_outputs_fit(NULL,NULL));
  return 0;
}
static int runtime_copy_restore(void){
  pet_program_runtime_t r,before;pet_program_runtime_init(&r);pet_program_image_t p=image();
  pet_program_output_contract_t c=contract(-10,10);
  CHECK(pet_program_runtime_start_with_contract(&r,&p,1,current,NULL,1,NULL,&c)==PET_RUNTIME_OK);
  c=contract(INT32_MIN,INT32_MAX);pet_program_event_t event={1,{11}};pet_program_report_t report;
  CHECK(pet_program_runtime_enqueue(&r,1,1,&event)==PET_RUNTIME_OK);CHECK(!pump(&r,1,&report));
  CHECK(report.result==PET_PROGRAM_BOUNDS&&report.revision==0&&r.state.cells[0]==0&&r.state.outputs[0]==0);
  event.words[0]=10;CHECK(pet_program_runtime_enqueue(&r,1,2,&event)==PET_RUNTIME_OK);CHECK(!pump(&r,1,&report));
  CHECK(report.result==PET_PROGRAM_COMMITTED&&report.revision==1&&r.state.outputs[0]==10);
  pet_program_snapshot_t seed;CHECK(pet_program_runtime_snapshot(&r,1,&seed)==PET_RUNTIME_OK);
  CHECK(pet_program_runtime_cancel(&r,1)==PET_RUNTIME_OK);copy(&before,&r,sizeof(r));unsigned checks=observations;
  c=contract(-9,9);
  CHECK(pet_program_runtime_start_with_contract(&r,&p,2,current,NULL,1,&seed,&c)==PET_RUNTIME_BAD_STATE);
  CHECK(equal(&before,&r,sizeof(r))&&observations==checks&&seed.outputs[0]==10);
  CHECK(pet_program_runtime_start_with_contract(&r,&p,2,current,NULL,1,&seed,&r.output_contract)==PET_RUNTIME_INVALID);
  CHECK(equal(&before,&r,sizeof(r)));
  CHECK(pet_program_runtime_start(&r,&p,2,current,NULL,1,&seed)==PET_RUNTIME_OK&&!r.output_limited);
  event.words[0]=INT32_MAX;CHECK(pet_program_runtime_enqueue(&r,2,1,&event)==PET_RUNTIME_OK);CHECK(!pump(&r,2,&report));
  CHECK(report.result==PET_PROGRAM_COMMITTED&&r.state.outputs[0]==INT32_MAX&&r.state.revision==2);
  CHECK(pet_program_runtime_cancel(&r,2)==PET_RUNTIME_OK);copy(&before,&r,sizeof(r));
  c=contract(1,100);
  CHECK(pet_program_runtime_start_with_contract(&r,&p,3,current,NULL,64,NULL,&c)==PET_RUNTIME_BAD_STATE);
  CHECK(equal(&before,&r,sizeof(r)));
  seed=(pet_program_snapshot_t){.schema=9,.revision=UINT32_MAX,.cells=1,.outputs={1}};
  CHECK(pet_program_runtime_start_with_contract(&r,&p,3,current,NULL,64,&seed,&c)==PET_RUNTIME_OK);
  CHECK(pet_program_runtime_enqueue(&r,3,1,&event)==PET_RUNTIME_OK);CHECK(!pump(&r,3,&report));
  CHECK(report.result==PET_PROGRAM_OVERFLOW&&r.state.revision==UINT32_MAX&&r.state.outputs[0]==1);
  return 0;
}
static uint32_t rng=7;
static uint32_t next(void){rng=rng*1664525U+1013904223U;return rng;}
static int mutations(void){
  pet_program_image_t p=image();
  for(unsigned trial=0;trial<5000;trial++){
    pet_program_runtime_t r;pet_program_runtime_init(&r);
    int32_t lo=(int32_t)(next()>>8)%10000-5000,hi=lo+(int32_t)(next()>>16)%1000;
    pet_program_output_contract_t c=contract(lo,hi);
    pet_program_snapshot_t seed={.schema=9,.cells=1,.values={55}};seed.outputs[0]=lo;
    CHECK(pet_program_runtime_start_with_contract(&r,&p,trial+1,current,NULL,1+trial%32,&seed,&c)==PET_RUNTIME_OK);
    int32_t value=(int32_t)(next()>>8)%12000-6000;
    c=contract(INT32_MIN,INT32_MAX); /* mutating the caller cannot broaden it */
    pet_program_event_t event={1,{value}};pet_program_report_t report;
    CHECK(pet_program_runtime_enqueue(&r,trial+1,1,&event)==PET_RUNTIME_OK);CHECK(!pump(&r,trial+1,&report));
    bool accepted=value>=lo&&value<=hi;
    CHECK(report.result==(accepted?PET_PROGRAM_COMMITTED:PET_PROGRAM_BOUNDS));
    CHECK(r.state.revision==(accepted?1U:0U)&&r.state.cells[0]==(accepted?value:55)&&r.state.outputs[0]==(accepted?value:lo));
    CHECK(pet_program_runtime_enqueue(&r,trial+1,1,&event)==PET_RUNTIME_DUPLICATE);
  }
  return 0;
}
int pet_program_output_contract_selftest(void){
  live=true;observations=0;int r=core_atomic();if(r)return r;
  r=runtime_copy_restore();if(r)return r;return mutations();
}
#ifndef PET_PROGRAM_WASM
int main(void){int result=pet_program_output_contract_selftest();printf("{\"result\":%d,\"trials\":5000,\"runtimeBytes\":%zu}\n",result,sizeof(pet_program_runtime_t));return result?1:0;}
#endif
