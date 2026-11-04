"""Read-only native query intervals; no request IDs, tasks, data or grants."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]

def test_query_observer_clock_zero_regression_saturation_and_actual_snapshot(tmp_path):
    getter=function((ROOT/'main/pet_p4_main.c').read_text(),'pet_p4_protocol_query_observation')
    native='\n'.join(function((ROOT/'main/pet_p4_main.c').read_text(),n) for n in ('query_native_begin','query_native_end'))
    code=r'''
#include <assert.h>
#define PET_P4_QUERY_OBSERVATION 1
#define PET_P4_QUERY_REPLY_TRACE 1
#include "pet_p4_query_reply.h"
static pet_p4_query_observation_t g_query_observation;
static unsigned depth,g_query_observation_mux;
static int64_t native_clock;
static int64_t esp_timer_get_time(void){return native_clock;}
#define portENTER_CRITICAL(m) do{(void)(m);assert(!depth);depth++;}while(0)
#define portEXIT_CRITICAL(m) do{(void)(m);assert(depth==1);depth--;}while(0)
'''+getter+native+r'''
int main(void){
 pet_p4_query_observation_t *o=&g_query_observation,s={0};
 assert(!pet_p4_protocol_query_observation(NULL)&&!depth);
 pet_p4_query_observation_begin(o,0);pet_p4_query_observation_unlock(o,0);pet_p4_query_observation_end(o,0);
 assert(!o->invalid_clock&&!o->active&&o->sequence==1);
 pet_p4_query_observation_begin(o,10);pet_p4_query_observation_unlock(o,15);
 assert(pet_p4_protocol_query_observation(&s)&&s.active&&s.unlocked&&s.begin_us==10&&s.unlocked_us==15&&!s.end_us&&!depth);
 pet_p4_query_observation_end(o,20);assert(pet_p4_protocol_query_observation(&s)&&!s.active&&s.end_us==20&&!s.invalid_clock);
 pet_p4_query_observation_begin(o,19);assert(o->invalid_clock);
 memset(o,0,sizeof(*o));pet_p4_query_observation_begin(o,10);pet_p4_query_observation_end(o,9);assert(o->invalid_clock);
 memset(o,0,sizeof(*o));pet_p4_query_observation_begin(o,10);pet_p4_query_observation_begin(o,11);assert(o->invalid_clock);
 memset(o,0,sizeof(*o));o->sequence=UINT64_MAX;pet_p4_query_observation_begin(o,12);assert(o->sequence==UINT64_MAX&&o->invalid_clock);
 assert(sizeof(*o)<=40);
 memset(o,0,sizeof(*o));native_clock=100;query_native_begin(PET_QUERY_DEBUG_NATIVE);
 assert(o->active&&!o->unlocked&&o->begin_us==100&&o->native_kind==PET_QUERY_DEBUG_NATIVE);
 native_clock=110;query_native_end();assert(!o->active&&o->unlocked_us==110&&o->end_us==110&&!o->invalid_clock);
 native_clock=120;query_native_begin(PET_QUERY_HELLO_NATIVE);assert(o->native_kind==PET_QUERY_HELLO_NATIVE);
 native_clock=130;query_native_end();
 pet_p4_query_observation_begin(o,140);assert(o->native_kind==PET_QUERY_ORDINARY);return 0;
}
'''
    src=tmp_path/'observe.c';exe=tmp_path/'observe';src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

def test_native_debug_hello_and_activation_intervals_are_not_missing_from_worker():
    from p4_control_integrity_test import _c_function
    body=_c_function((ROOT/'main/pet_p4_main.c').read_text(),'protocol_worker_task')
    assert body.index('query_native_begin(PET_QUERY_HELLO_NATIVE)')<body.index('run_unsolicited_hello()')
    assert body.index('query_native_begin(PET_QUERY_ACTIVATION_NATIVE)')<body.index('run_component_activation(')
    assert body.index('pet_p4_query_observation_begin(')<body.index('bool handled_debug =')
    assert 'if(handled_debug)query_native_end()' in body
