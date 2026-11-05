#include "pet_p4_reset_cost.h"
#if PET_P4_RESET_PHASE_BENCHMARK
#include <stdatomic.h>
#include <limits.h>
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pet_p4_reset_phase.h"
#if !PET_P4_RESET_PHASE_TRACE
#error "Native recorder measurement needs the real bounded actor recorder"
#endif
static DRAM_ATTR pet_reset_cost_t reset_cost;
static DRAM_ATTR _Atomic bool cost_complete;
static DRAM_ATTR bool cost_attempted;
void pet_reset_cost_boot(void){
  if(cost_attempted)return;
  cost_attempted=true;
  pet_reset_cost_t next={.min_cycles=UINT32_MAX,.min_wall_us=UINT32_MAX};
  next.clock_hz=esp_clk_cpu_freq();
  bool good=next.clock_hz>0&&next.clock_hz<=UINT32_C(1000000000);
  for(unsigned i=0;good&&i<32;i++){
    int64_t begin_us=esp_timer_get_time();uint32_t begin=esp_cpu_get_cycle_count();
    pet_reset_phase_scope_t scope=pet_reset_phase_enter(PET_PHASE_BOOT);
    if(!scope.active){good=false;break;}
    pet_reset_phase_leave(&scope);
    uint32_t end=esp_cpu_get_cycle_count();int64_t end_us=esp_timer_get_time();
    /* Includes preemption; NOT CPU-time/WCET. Bound wall time to exclude an
     * ambiguous full 32-bit cycle wrap or a debugger-paused observation. */
    if(scope.active||begin_us<0||end_us<begin_us||end_us-begin_us>1000000){good=false;break;}
    uint32_t cycles=end-begin,wall=(uint32_t)(end_us-begin_us);
    next.samples++;next.total_cycles+=cycles;next.total_wall_us+=wall;
    if(cycles<next.min_cycles)next.min_cycles=cycles;
    if(cycles>next.max_cycles)next.max_cycles=cycles;
    if(wall<next.min_wall_us)next.min_wall_us=wall;
    if(wall>next.max_wall_us)next.max_wall_us=wall;
    if((i&3U)==3U&&i+1<32)vTaskDelay(pdMS_TO_TICKS(1));
  }
  next.valid=good&&next.samples==32&&esp_clk_cpu_freq()==next.clock_hz;
  reset_cost=next;atomic_store_explicit(&cost_complete,true,memory_order_release);
}
bool pet_reset_cost_observe(pet_reset_cost_t *out){
  if(!out||!atomic_load_explicit(&cost_complete,memory_order_acquire))return false;
  *out=reset_cost;return true;
}
#endif
