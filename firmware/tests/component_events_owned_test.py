"""Production event/alarm ext adapter, with only model/clock/notification owners stubbed."""
import subprocess
from pathlib import Path


def test_hidden_alarm_delivery_cancel_and_delete(tmp_path):
    fw = Path(__file__).resolve().parents[1]
    source = (fw/'main/pet_p4_miniapp_ext.inc').read_text()
    top = source[source.index('#if PET_P4_CAPABILITY_EXT'):source.index('static int ext_find_int_var')] + '\n#endif\n'
    start = source.index('#if PET_P4_EVENTS\n/* Alarm bookkeeping')
    process = source[start:source.index('static pet_cext_publish_ticket_t *g_ext_pending_publish', start)]
    start = source.index('static void miniapp_ext_removed(')
    removed = source[start:source.index('static bool miniapp_ext_install_chunk', start)]
    (tmp_path/'freertos').mkdir()
    (tmp_path/'freertos/semphr.h').write_text('''#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){SemaphoreHandle_t p=malloc(sizeof(*p));if(p)pthread_mutex_init(p,NULL);return p;}
static inline int xSemaphoreTake(SemaphoreHandle_t p,unsigned t){(void)t;return pthread_mutex_lock(p)==0;}
static inline int xSemaphoreGive(SemaphoreHandle_t p){return pthread_mutex_unlock(p)==0;}
static inline void vSemaphoreDelete(SemaphoreHandle_t p){pthread_mutex_destroy(p);free(p);}
#define pdTRUE 1
#define portMAX_DELAY 0
''')
    harness = tmp_path/'events.c'
    harness.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "pet_p4_component_ext.h"
#define PET_P4_CAPABILITY_EXT 1
#define PET_P4_EVENTS 1
#define EXT_RAM_BSS_ATTR
#define PET_P4_MINIAPP_ACTION_MAX 48
#define MINIAPP_VAR_INT 1
typedef struct {int type;int32_t int_value;} var_t;
typedef struct {bool active;char widget_id[48];struct {char title[64];} view;pet_cext_runtime_t cext;unsigned var_count;var_t vars[4];} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
static pthread_mutex_t model=PTHREAD_MUTEX_INITIALIZER;
static void miniapp_runtime_lock(void){pthread_mutex_lock(&model);}
static void miniapp_runtime_unlock(void){pthread_mutex_unlock(&model);}
static void copy_utf8(char *out,size_t cap,const char *s){snprintf(out,cap,"%s",s);}
static pet_cev_clock_snapshot_t miniapp_utc_snapshot(uint64_t now){return (pet_cev_clock_snapshot_t){.valid=true,.utc_ms=now,.calibration_generation=1};}
static bool miniapp_ext_hidden_checkpoint(void){return true;}
static unsigned dispatches;
static bool pet_p4_miniapp_dispatch_action(const char *action,uint64_t now){(void)action;(void)now;dispatches++;return true;}
static void pet_cmedia_remove(size_t slot){(void)slot;}
static bool pet_cmedia_collect_try(void){return true;}
''' + top + process + removed + r'''
static void setup(void){
 memset(&g_runtime,0,sizeof(g_runtime));memset(g_ext_alarms,0,sizeof(g_ext_alarms));g_ext_notice_pending=false;
 g_runtime.active=true;strcpy(g_runtime.widget_id,"active");strcpy(g_runtime.view.title,"Active");g_runtime.cext.events=true;
 g_runtime.cext.alarms.count=1;pet_cev_alarm_t *a=&g_runtime.cext.alarms.items[0];
 strcpy(a->id,"timer");strcpy(a->action,"ring");strcpy(a->text,"due");a->at_var=0;a->background=true;
 g_runtime.var_count=1;g_runtime.vars[0].type=MINIAPP_VAR_INT;g_runtime.vars[0].int_value=60000;
}
int main(void){
 setup();pet_p4_miniapp_alarm_policy(false);
 miniapp_ext_process(0,false);miniapp_ext_process(60000,false);
 char title[64],text[PET_CEV_ALARM_TEXT_MAX];assert(!pet_p4_miniapp_alarm_notification(title,text));assert(!dispatches);
 // A due hidden reminder stays pending until per-boot policy permits notification.
 pet_p4_miniapp_alarm_policy(true);miniapp_ext_process(60001,false);
 assert(pet_p4_miniapp_alarm_notification(title,text));assert(!strcmp(text,"due"));assert(!dispatches);
 miniapp_ext_process(60002,false);assert(!pet_p4_miniapp_alarm_notification(title,text));
 // Parked installed component is timed without dispatching its program/action.
 ext_alarm_entry_t *e=ext_alarm_entry("parked",true);strcpy(e->title,"Parked");e->alarms=g_runtime.cext.alarms;
 e->alarms.items[0].armed=true;e->alarms.items[0].pending=false;e->alarms.items[0].deadline_ms=70000;
 miniapp_ext_process(70000,false);assert(pet_p4_miniapp_alarm_notification(title,text));assert(!strcmp(title,"Parked"));assert(!dispatches);
 e->alarms.items[0].armed=true;e->alarms.items[0].deadline_ms=80000;
 assert(pet_p4_miniapp_alarm_cancel("parked","timer"));miniapp_ext_process(90000,false);assert(!pet_p4_miniapp_alarm_notification(title,text));
 // Removal revokes an already queued notice and clears all future table activity.
 e->alarms.items[0].armed=true;e->alarms.items[0].cancelled=false;e->alarms.items[0].utc_deadline=false;e->alarms.items[0].waiting_for_clock=false;e->alarms.items[0].deadline_ms=100000;
 miniapp_ext_process(100000,false);assert(g_ext_notice_pending);
 miniapp_ext_removed("parked",0);assert(!pet_p4_miniapp_alarm_notification(title,text));assert(!ext_alarm_entry("parked",false));
 miniapp_ext_process(100001,true);assert(!dispatches);
 setup();pet_p4_miniapp_alarm_policy(true);miniapp_ext_process(0,false);
 assert(pet_p4_miniapp_alarm_cancel("active","timer"));miniapp_ext_process(70000,false);
 assert(!pet_p4_miniapp_alarm_notification(title,text));assert(!dispatches);
 return 0;
}
''')
    binary = tmp_path/'events'
    subprocess.run(['cc','-std=c11','-pthread','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-I',str(tmp_path),'-I',str(fw/'main'),'-I',str(fw/'tests/vendor/cjson'),str(harness),str(fw/'main/pet_p4_component_events.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)


def test_main_loop_defers_consumption_during_conversation_then_delivers_once(tmp_path):
    from p4_render_trace_test import function
    fw=Path(__file__).resolve().parents[1]
    source=(fw/'main/pet_p4_main.c').read_text()
    start=source.index('char alarm_title[64], alarm_text[41];')
    body=source[start:source.index('\n    }\n#endif',start)]
    notification=function((fw/'main/pet_p4_miniapp_ext.inc').read_text(),'pet_p4_miniapp_alarm_notification')
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define PET_CEV_ALARM_TEXT_MAX 41
static bool g_ext_notice_pending=true,conversation;static char g_ext_notice_id[48]="fixture";
static char g_ext_notice_title[64]="Reminder",g_ext_notice_text[41]="Due";
static struct {char current_title[64],current_speech[41],current_status_text[32];unsigned long long last_update_ms;} g_state;
static void ext_lock(void){}static void ext_unlock(void){}
static void copy_utf8(char*d,size_t n,const char*s){snprintf(d,n,"%s",s);}
static bool pet_p4_conversation_active(const void*s){(void)s;return conversation;}
'''+notification+'\nstatic void turn(unsigned long long now_ms){\n'+body+r'''
}
int main(void){
 conversation=true;turn(10);turn(11);assert(g_ext_notice_pending&&!g_state.last_update_ms&&!g_state.current_speech[0]);
 conversation=false;turn(12);assert(!g_ext_notice_pending&&g_state.last_update_ms==12&&!strcmp(g_state.current_speech,"Due"));
 turn(13);assert(g_state.last_update_ms==12);return 0;
}
'''
    src=tmp_path/'main-alarm.c';src.write_text(code);binary=tmp_path/'main-alarm'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
