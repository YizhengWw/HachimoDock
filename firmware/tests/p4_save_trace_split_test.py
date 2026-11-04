"""Real save algorithm suite with detached numeric-trace completion adapters."""
from pathlib import Path
import petui_app_store_test as store
ROOT=Path(__file__).resolve().parents[1]

def test_actual_detached_save_retains_full_generation_revoke_fault_and_nested_flush_suite(tmp_path,monkeypatch):
    original=Path.read_text
    def source(path,*args,**kwargs):
        text=original(path,*args,**kwargs)
        if path!=ROOT/'main/pet_p4_app_store.c':return text
        text=text.replace('#include "pet_p4_reset_phase.h"',
            '#define PET_RESET_PHASE_SCOPE(name,phase)\n#define pet_reset_phase_bind(actor) true\n')
        text=text.replace('flush_one(', 'flush_one_native(')
        text='#define PET_P4_SAVE_TRACE_DETACHED 1\n#define PET_P4_RESET_PHASE_TRACE 1\n#define PET_WIDGET_TRACE_TEST 1\n'+text
        return text+r'''
static unsigned trace_started,trace_ended;
pet_widget_trace_operation_t pet_widget_trace_begin(pet_widget_trace_kind_t kind){
 assert(kind==PET_WTRACE_SAVE_FLUSH);return(pet_widget_trace_operation_t){.seq=++trace_started,.lane=1,.owned=true};
}
void pet_widget_trace_end(pet_widget_trace_operation_t *op,bool ok){
 (void)ok;if(!op||!op->owned)return;
 /* This must be AFTER physical/cache/owned-pending cleanup, not merely after
  * a successful fwrite or a guessed acknowledgement. No extra stack granted. */
 assert(!save_pending_scope&&!save_cache_epoch&&!store_locked);op->owned=false;trace_ended++;
}
void pet_widget_trace_operation_cleanup(pet_widget_trace_operation_t *op){pet_widget_trace_end(op,false);}
pet_widget_trace_scope_t pet_widget_trace_scope_begin(pet_widget_trace_stage_t stage){(void)stage;return(pet_widget_trace_scope_t){0};}
void pet_widget_trace_scope_end(pet_widget_trace_scope_t *scope){(void)scope;}
void pet_widget_trace_snapshot(pet_widget_trace_snapshot_t *out){if(out)*out=(pet_widget_trace_snapshot_t){0};}
/* Same two physical/completion statements as the actual worker; the old full
 * algorithm harness calls this instead of entering an endless RTOS loop. */
static void flush_one(void){
 unsigned outstanding=trace_started-trace_ended;
 pet_widget_trace_operation_t op __attribute__((cleanup(pet_widget_trace_operation_cleanup)))={0};bool ok=false;
 flush_one_native(&op,&ok);pet_widget_trace_end(&op,ok);
 /* A refused nested call leaves the OUTER trace outstanding, not completed. */
 assert(trace_started-trace_ended==outstanding);
}
'''
    monkeypatch.setattr(Path,'read_text',source)
    store._run_private_save_generations(tmp_path,True,owned_pending=True)
    worker=original(ROOT/'main/pet_p4_app_store.c')
    worker=worker[worker.index('static void save_worker('):worker.index('bool pet_app_store_init(')]
    assert worker.index('flush_one(&completed,&completed_ok);')<worker.index('pet_widget_trace_end(&completed,completed_ok);')<worker.index('save_stack_sample();')
    assert 'PET_APP_SAVE_TASK_STACK_BYTES,NULL,1,&worker' in original(ROOT/'main/pet_p4_app_store.c')
