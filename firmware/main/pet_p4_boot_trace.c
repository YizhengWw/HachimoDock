#include "pet_p4_boot_trace.h"
#if PET_P4_BOOT_TRACE_ENABLE
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>
static DRAM_ATTR portMUX_TYPE boot_mux=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR struct {
  int64_t start_us,last_us;
  pet_boot_trace_t value;
  bool attempted;
} boot_trace;
_Static_assert(sizeof(boot_trace)<=64,"bounded native boot metadata only");
_Static_assert(PET_BOOT_MARK_COUNT==8,"update wire mask contract before adding milestones");
static bool boot_overlap(const void *a,size_t an,const void *b,size_t bn){
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
bool pet_boot_trace_begin(void){
  int64_t now=esp_timer_get_time();bool ok=false;
  portENTER_CRITICAL(&boot_mux);
  if(!boot_trace.attempted){
    boot_trace.attempted=true;
    boot_trace.value.available=now>=0;boot_trace.value.invalid_clock=now<0;
    if(now>=0){boot_trace.start_us=boot_trace.last_us=now;ok=true;}
  }
  portEXIT_CRITICAL(&boot_mux);return ok;
}
bool pet_boot_trace_mark(pet_boot_mark_t mark){
  if((unsigned)mark>=PET_BOOT_MARK_COUNT)return false;
  int64_t now=esp_timer_get_time();bool ok=false;
  portENTER_CRITICAL(&boot_mux);
  uint8_t bit=(uint8_t)(1U<<(unsigned)mark);
  if(boot_trace.value.available&&!boot_trace.value.invalid_clock&&!(boot_trace.value.mask&bit)){
    if(now<boot_trace.last_us||now<boot_trace.start_us
        ||(uint64_t)(now-boot_trace.start_us)>UINT32_MAX)boot_trace.value.invalid_clock=true;
    else{
      boot_trace.value.elapsed_us[mark]=(uint32_t)(now-boot_trace.start_us);
      boot_trace.value.mask|=bit;boot_trace.last_us=now;ok=true;
    }
  }
  portEXIT_CRITICAL(&boot_mux);return ok;
}
bool pet_boot_trace_observe(pet_boot_trace_t *out){
  if(!out||boot_overlap(out,sizeof(*out),&boot_trace,sizeof(boot_trace))
      ||boot_overlap(out,sizeof(*out),&boot_mux,sizeof(boot_mux)))return false;
  pet_boot_trace_t copy;
  portENTER_CRITICAL(&boot_mux);copy=boot_trace.value;portEXIT_CRITICAL(&boot_mux);
  *out=copy;return true;
}
#endif
