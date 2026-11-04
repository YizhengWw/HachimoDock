"""R135 gap-peak holder attribution: topic hash, worker locked-span peak, flag gating and call placement."""
from pathlib import Path
import configparser
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]
FLAGS=['-DPET_P4_GAP_PEAK_HOLDER=1','-DPET_P4_GAP_PEAK_CONTEXT=1','-DPET_P4_FRAME_IO_OVERLAP=1','-DPET_P4_RETAIN_PEAK_PHASES=1','-DPET_P4_APP_PHASE_TRACE=1','-DPET_P4_APP_NODE_TRACE=1']


def test_topic_hash_locked_peak_and_gap_context_attribution(tmp_path):
    code=r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_worker_locked.h"
#include "pet_p4_render_lifetime.h"
int main(void){
 uint32_t commit=pet_topic_hash("widget/commit",13);
 assert(commit!=0&&commit==pet_line_topic_hash("{\"topic\":\"widget/commit\",\"payload\":{\"widgetId\":\"x\"}}"));
 assert(pet_line_topic_hash("{\"topic\":\"widget/chunk\",\"payload\":{}}")!=commit);
 assert(pet_line_topic_hash(NULL)==0&&pet_line_topic_hash("{\"payload\":{}}")==0);
 assert(pet_line_topic_hash("{\"topic\":\"widget/commit")==0&&pet_line_topic_hash("{\"topic\":\"\"}")==0);
 /* A topic value never reaches past its closing quote. */
 assert(pet_line_topic_hash("{\"topic\":\"a\",\"x\":\"topic\"}")==pet_topic_hash("a",1));
 pet_worker_locked_peak_t peak;memset(&peak,0,sizeof(peak));
 pet_worker_locked_record(&peak,5000,1,100);pet_worker_locked_record(&peak,52680,commit,200);pet_worker_locked_record(&peak,30000,3,300);
 assert(peak.spans==3&&peak.over_budget_spans==2&&peak.max_us==52680&&peak.max_topic_hash==commit&&peak.max_at_us==200);
 pet_render_lifetime_t life;memset(&life,0,sizeof(life));
 pet_render_gap_context_t c={.frame=10,.wait_max_us=52680,.at_us=12345678,.wait_topic_hash=commit};
 strcpy(c.wait_holder,"protocol");
 pet_render_gap_context_record(&life,84315,&c);pet_render_lifetime_record(&life,12000,84315,10,2,0,12000,66667,100000);
 assert(life.gap_peak.at_us==12345678&&life.gap_peak.wait_topic_hash==commit&&!strcmp(life.gap_peak.wait_holder,"protocol"));
 c.frame=11;c.at_us=1;c.wait_topic_hash=0;c.wait_holder[0]=0;
 pet_render_gap_context_record(&life,40000,&c);
 assert(life.gap_peak.frame==10&&life.gap_peak.at_us==12345678&&!strcmp(life.gap_peak.wait_holder,"protocol"));
 return 0;
}
'''
    src=tmp_path/'holder.c';exe=src.with_suffix('');src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*FLAGS,'-I',str(ROOT/'main'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=60)


def test_holder_instrumentation_is_flag_gated_and_placed_around_the_existing_locks(tmp_path):
    main=(ROOT/'main/pet_p4_main.c').read_text();renderer=(ROOT/'main/pet_p4_renderer.c').read_text();diag=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    loop=main[main.index('  while (true) {\n    bool render_frame = false;'):main.index('pet_p4_renderer_note_loop(')]
    # Holder class and topic are sampled before the main loop's own take; attribution precedes note_loop.
    assert loop.index('loop_holder = xSemaphoreGetMutexHolder(g_state_mutex);')<loop.index('xSemaphoreTake(g_state_mutex, portMAX_DELAY);')
    assert loop.index('loop_wait_us = (uint32_t)')<loop.index('pet_p4_renderer_note_wait_holder(loop_wait_us, loop_holder_class, loop_holder_topic);')
    assert 'pcTaskGetName(' not in loop and loop.count('xSemaphoreTake(g_state_mutex, portMAX_DELAY);')==1 and loop.count('esp_timer_get_time()')==1
    # Worker: topic hash published and span begun right after the take, span ended right before the give, hash cleared after.
    # The retired-ingress free/continue precedes all ordinary lock ownership.
    worker=function(main,'protocol_worker_task')
    worker=worker[worker.index('if (!handled_debug) {'):]
    take=worker.index('xSemaphoreTake(g_state_mutex, portMAX_DELAY);')
    assert worker.index('g_worker_locked_topic_hash=pet_line_topic_hash(request.line);',take)<worker.index('worker_locked_begin();',take)<worker.index('pet_p4_handle_line_with_reply(&g_state, request.line')
    give=worker.index('xSemaphoreGive(g_state_mutex);')
    assert worker.rindex('worker_locked_end();',0,give)>worker.index('pet_p4_protocol_set_deferred_reply(NULL);') and worker.index('g_worker_locked_topic_hash=0;',give)<worker.index('free(request.line);')
    # Unlocked storage IO windows are excluded from the worker's locked spans.
    unlock=main[main.index('static void storage_command_unlock(void *context) {'):main.index('static void storage_command_lock(void *context) {')]
    assert unlock.index('worker_locked_end();')<unlock.index('xSemaphoreGive(g_state_mutex);')
    lock=main[main.index('static void storage_command_lock(void *context) {'):main.index('static void mark_usb_protocol_seen(void);')]
    assert lock.index('xSemaphoreTake(g_state_mutex, portMAX_DELAY);')<lock.index('worker_locked_begin();')
    # Cursor persistence now has its own worker lock span, with the same flag gate.
    cursor=main[main.index('static void run_catalog_cursor('):main.index('static void run_component_activation(')]
    assert cursor.count('#if PET_P4_GAP_PEAK_HOLDER')==2
    assert cursor.index('xSemaphoreTake(g_state_mutex, portMAX_DELAY);')<cursor.index('worker_locked_begin();')<cursor.index('pet_p4_miniapp_catalog_cursor_run(token)')<cursor.index('worker_locked_end();')<cursor.index('xSemaphoreGive(g_state_mutex);')
    # Idle cleanup is now an explicit protocol-worker operation; its two
    # attribution gates must bracket the real cleanup and existing mutex.
    idle=function(main,'asset_idle_worker')
    assert idle.count('#if PET_P4_GAP_PEAK_HOLDER')==2
    assert idle.index('xSemaphoreTake(g_state_mutex,portMAX_DELAY)') < idle.index('worker_locked_begin();') < idle.index('pet_p4_asset_transfer_cleanup_worker(&g_state)') < idle.index('worker_locked_end();') < idle.index('xSemaphoreGive(g_state_mutex)')
    assert 'g_worker_locked_topic_hash=pet_topic_hash("asset/idle-cleanup"' in idle
    assert 'worker_locked_end();g_worker_locked_topic_hash=0;' in idle
    assert main.replace(cursor,'').replace(idle,'').count('#if PET_P4_GAP_PEAK_HOLDER')==8
    assert main.count('#if PET_P4_GAP_PEAK_CONTEXT')==6
    assert renderer.count('void pet_p4_renderer_note_wait_holder(')==1 and renderer.index('context.at_us=(uint64_t)ended_us;')<renderer.index('pet_render_gap_context_record(&g_metrics.lifetime,g_render_trace.gap_us,&context);')
    for key in ('"atUs"','"waitHolder"','"waitTopicHash"','"workerLocked"','"overBudgetSpans"'):assert key in diag
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_gap_holder_parent':
            assert config[section]['extends']=='env:esp32_p4_ack_defer_parent' and config[section]['custom_p4_project_version']=='0.7.103-p4-r135-holder'
            assert 'PET_P4_GAP_PEAK_HOLDER=1' in flags and not any(v in flags for v in ('TASK_PRIORITY','F_CPU','BUDGET_US','WATCHDOG','AFFINITY'))
        else:assert 'PET_P4_GAP_PEAK_HOLDER=1' not in flags
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_render_lifetime.h"\n#include "pet_p4_worker_locked.h"\nint main(void){return PET_P4_GAP_PEAK_HOLDER;}\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_GAP_PEAK_HOLDER=1','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'holder attribution requires' in result.stderr
    assert subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsyntax-only','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True).returncode==0
