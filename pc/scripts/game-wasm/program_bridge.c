/* Product interpreter ABI. Never instantiate component-supplied WASM.
 * One module instance owns one pure, bounded program; no imports/heap/IO.
 * configure/start are single-use, cancellation is latched, and snapshots never
 * include queued events. This preview grants no device execution authority. */
#include "pet_p4_program_runtime.h"
#include "pet_p4_program_timers.h"
#include <stddef.h>
static uint8_t image_bytes[PET_PROGRAM_IMAGE_MAX_BYTES];
static pet_program_image_t image;
static pet_program_runtime_t runtime;
static pet_program_snapshot_t seed,snapshot;
static pet_program_report_t report;
static pet_program_output_contract_t limits;
static unsigned limit_mask;
static bool limit_invalid;
static bool configured,started,live,seed_ready;
static pet_program_timers_t timers;
static pet_program_timer_spec_t timer_specs[PET_PROGRAM_TIMERS];
static pet_program_timer_diagnostic_t timer_diagnostic;
static unsigned timer_mask;
static bool timer_invalid;
static bool current(void *context,uint64_t generation){(void)context;return live&&generation==1;}
uint8_t *pp_buffer(void){return image_bytes;}
int pp_configure(unsigned length){
  if(configured||started||length>PET_PROGRAM_IMAGE_MAX_BYTES)return PET_PROGRAM_IMAGE_INVALID_ARGUMENT;
  pet_program_image_result_t result=pet_program_image_decode(&image,image_bytes,length);
  if(result==PET_PROGRAM_IMAGE_OK){pet_program_runtime_init(&runtime);configured=true;}
  return result;
}
unsigned pp_schema(void){return image.valid?image.schema:0;}
unsigned pp_cells(void){return image.valid?image.cells:0;}
unsigned pp_fuel(void){return image.valid?image.fuel:0;}
unsigned pp_count(void){return image.valid?image.count:0;}
#if PET_P4_PROGRAM_V2
/* v2 preview only: package-level grid/text table bound before start (never wire data). */
unsigned pp_version(void){return 2;}
int pp_grid(unsigned cols,unsigned rows){
  if(!configured||started||cols>16||rows>16||(cols&&!rows)||(!cols&&rows)||cols*rows>image.cells)return 0;
  image.grid_cols=(uint8_t)cols;image.grid_rows=(uint8_t)rows;return 1;
}
int pp_texts(unsigned count){if(!configured||started||count>64)return 0;image.texts=(uint8_t)count;return 1;}
#else
unsigned pp_version(void){return 1;}
#endif
void pp_seed(unsigned schema,unsigned revision,unsigned cells){
  seed_ready=false;
  if(!configured||started||schema!=image.schema||cells!=image.cells||cells>PET_PROGRAM_CELLS)return;
  seed=(pet_program_snapshot_t){.schema=schema,.revision=revision,.cells=cells};
  seed_ready=true;
}
void pp_seed_cell(unsigned index,int32_t value){if(index<PET_PROGRAM_CELLS)seed.values[index]=value;}
void pp_seed_output(unsigned index,int32_t value){if(index<PET_PROGRAM_OUTPUTS)seed.outputs[index]=value;}
int pp_limit(unsigned index,int32_t minimum,int32_t maximum){
  if(started)return 0;
  if(!configured||index>=PET_PROGRAM_OUTPUTS||minimum>maximum){limit_invalid=true;return 0;}
  limits.minimum[index]=minimum;limits.maximum[index]=maximum;limit_mask|=1U<<index;return 1;
}
int pp_start(unsigned quantum,int seeded){
  if(!configured||started||!quantum||quantum>PET_PROGRAM_QUANTUM||seeded<0||seeded>1)return PET_RUNTIME_INVALID;
  if(seeded&&!seed_ready)return PET_RUNTIME_BAD_STATE;
  if(limit_invalid||(limit_mask&&limit_mask!=255))return PET_RUNTIME_BAD_STATE;
  started=true;live=true;
  pet_program_runtime_result_t result=pet_program_runtime_start_with_contract(&runtime,&image,1,current,NULL,quantum,seeded?&seed:NULL,limit_mask?&limits:NULL);
  if(result!=PET_RUNTIME_OK)live=false;
  return result;
}
int pp_timer(unsigned index,unsigned id,unsigned delay_ms,unsigned interval_ms,unsigned count,
    int32_t a,int32_t b,int32_t c,int32_t d){
  if(timers.marker)return 0;
  if(!configured||index>=PET_PROGRAM_TIMERS||!id||id>UINT16_MAX||count>PET_PROGRAM_EVENT_WORDS){timer_invalid=true;return 0;}
  timer_specs[index]=(pet_program_timer_spec_t){.id=(uint16_t)id,.delay_ms=delay_ms,.interval_ms=interval_ms,.event={.count=count,.words={a,b,c,d}}};
  timer_mask|=1U<<index;return 1;
}
int pp_clock_start(unsigned low,unsigned high,unsigned count){
  if(!started||!runtime.active||timer_invalid||!count||count>PET_PROGRAM_TIMERS||timer_mask!=((1U<<count)-1U))return PET_TIMERS_INVALID;
  return pet_program_timers_start(&timers,1,((uint64_t)high<<32)|low,timer_specs,count);
}
static pet_program_runtime_result_t timer_enqueue(void *context,uint64_t generation,uint64_t sequence,const pet_program_event_t *event){
  (void)context;return pet_program_runtime_enqueue(&runtime,generation,sequence,event);
}
int pp_tick(unsigned low,unsigned high,unsigned sequence_low,unsigned sequence_high){
  timer_diagnostic=(pet_program_timer_diagnostic_t){0};
  pet_program_timers_result_t result=pet_program_timers_tick(&timers,1,((uint64_t)high<<32)|low,
    ((uint64_t)sequence_high<<32)|sequence_low,timer_enqueue,NULL,&timer_diagnostic);
  if(result==PET_TIMERS_CLOCK_ERROR||result==PET_TIMERS_SEQUENCE_ERROR||result==PET_TIMERS_STOPPED){
    live=false;(void)pet_program_runtime_cancel(&runtime,1);
  }
  return result;
}
unsigned pp_tick_id(void){return timer_diagnostic.timer_id;}
unsigned pp_tick_coalesced(void){return timer_diagnostic.coalesced;}
unsigned pp_tick_accepted(void){return timer_diagnostic.accepted;}
unsigned pp_tick_exhausted(void){return timer_diagnostic.deadline_exhausted;}
int pp_enqueue(unsigned low,unsigned high,unsigned count,int32_t a,int32_t b,int32_t c,int32_t d){
  if(!started||count>PET_PROGRAM_EVENT_WORDS)return PET_RUNTIME_INVALID;
  pet_program_event_t event={.count=count,.words={a,b,c,d}};
  return pet_program_runtime_enqueue(&runtime,1,((uint64_t)high<<32)|low,&event);
}
int pp_pump(void){return started?pet_program_runtime_pump(&runtime,1):PET_RUNTIME_INVALID;}
unsigned pp_queued(void){return started?runtime.queued:0;}
int pp_cancel(void){
  if(!started)return PET_RUNTIME_INVALID;
  if(timers.marker)(void)pet_program_timers_cancel(&timers,1);
  live=false;return pet_program_runtime_cancel(&runtime,1);
}
int pp_report(void){return started?pet_program_runtime_take_report(&runtime,1,&report):PET_RUNTIME_INVALID;}
unsigned pp_report_result(void){return report.result;}
unsigned pp_report_revision(void){return report.revision;}
unsigned pp_report_fuel(void){return report.used_fuel;}
unsigned pp_report_low(void){return (uint32_t)report.sequence;}
unsigned pp_report_high(void){return (uint32_t)(report.sequence>>32);}
int pp_snapshot(void){return started?pet_program_runtime_snapshot(&runtime,1,&snapshot):PET_RUNTIME_INVALID;}
unsigned pp_revision(void){return snapshot.revision;}
int32_t pp_cell(unsigned index){return index<PET_PROGRAM_CELLS?snapshot.values[index]:0;}
int32_t pp_output(unsigned index){return index<PET_PROGRAM_OUTPUTS?snapshot.outputs[index]:0;}
