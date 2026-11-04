"""Production UTC accessor, alarm core, parked cancellation/checkpoint lifecycle."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def _run_utc_test(tmp_path, extra_prefix='', extra_body='', extra_main='', extra_sources=()):
    source=(ROOT/'main/pet_p4_miniapp_ext.inc').read_text()
    top=source[source.index('#if PET_P4_CAPABILITY_EXT'):source.index('static int ext_find_int_var')]+'\n#endif\n'
    a=source.index('#if PET_P4_EVENTS\n/* Alarm bookkeeping');process=source[a:source.index('static pet_cext_publish_ticket_t *g_ext_pending_publish',a)]
    a=source.index('#if PET_P4_EVENTS\n/* Storage-owner checkpoint');save=source[a:source.index('#if PET_P4_EVENTS\n#include "pet_p4_miniapp_alarm_boot.inc"',a)]
    main=(ROOT/'main/pet_p4_miniapp.c').read_text();a=main.index('static bool g_wall_valid;');clock=main[a:main.index('static bool g_input_visible;',a)]
    (tmp_path/'freertos').mkdir()
    (tmp_path/'freertos/semphr.h').write_text('''#pragma once
#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){SemaphoreHandle_t p=malloc(sizeof(*p));if(p)pthread_mutex_init(p,NULL);return p;}
static inline int xSemaphoreTake(SemaphoreHandle_t p,unsigned t){(void)t;return pthread_mutex_lock(p)==0;}
static inline int xSemaphoreGive(SemaphoreHandle_t p){return pthread_mutex_unlock(p)==0;}
static inline void vSemaphoreDelete(SemaphoreHandle_t p){pthread_mutex_destroy(p);free(p);}
#define pdTRUE 1
#define portMAX_DELAY 0
''')
    harness=r'''
#include <assert.h>
#include <math.h>
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
#define PET_APP_SAVE_BYTES 2304
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(m) pthread_mutex_lock(m)
#define portEXIT_CRITICAL(m) pthread_mutex_unlock(m)
typedef struct {int type;int32_t int_value;} var_t;
typedef struct {bool active;char widget_id[48];struct{char title[64];}view;pet_cext_runtime_t cext;unsigned var_count,save_version;var_t vars[4];} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
static pthread_mutex_t model=PTHREAD_MUTEX_INITIALIZER;
static unsigned model_depth,state_depth,dispatches,live_alloc,updates;static bool oom,fail_update;
static char saved[PET_APP_SAVE_BYTES],durable[PET_APP_SAVE_BYTES];
static uint64_t mono;
static int64_t esp_timer_get_time(void){return (int64_t)mono*1000;}
static void miniapp_runtime_lock(void){pthread_mutex_lock(&model);model_depth++;}
static void miniapp_runtime_unlock(void){assert(model_depth);model_depth--;pthread_mutex_unlock(&model);}
static void copy_utf8(char*out,size_t n,const char*s){snprintf(out,n,"%s",s);}
static void *heap_caps_calloc(size_t n,size_t s,int caps){assert(!model_depth&&!state_depth&&caps==3);if(oom)return NULL;void*p=calloc(n,s);if(p)live_alloc++;return p;}
static void heap_caps_free(void*p){assert(!model_depth&&!state_depth);if(p){assert(live_alloc);live_alloc--;}free(p);}
static bool pet_form_source_valid(const char*s){return s&&s[0]=='{';}
static bool pet_app_store_read_cached(const char*id,char*out){assert(!model_depth&&!strcmp(id,"parked"));strcpy(out,saved);return saved[0];}
static bool pet_app_store_update(const char*id,const char*json){assert(!model_depth&&!strcmp(id,"parked"));updates++;if(fail_update)return false;assert(strlen(json)<sizeof saved);strcpy(saved,json);return true;}
static bool pet_p4_miniapp_dispatch_action(const char*a,uint64_t now){(void)a;(void)now;dispatches++;return true;}
static bool pet_cmedia_collect_try(void){return true;}
''' +extra_prefix+clock+top+process+save+extra_body+r'''
static void sync_clock(uint64_t utc,int offset,uint64_t now){
 char json[128];snprintf(json,sizeof json,"{\"unixSeconds\":%llu,\"utcOffsetMinutes\":%d}",(unsigned long long)utc,offset);
 cJSON*j=cJSON_Parse(json);assert(j&&pet_p4_miniapp_clock_sync(j,now));cJSON_Delete(j);mono=now;
}
static pet_cev_alarm_set_t configuration(void){pet_cev_alarm_set_t s={0};s.count=1;strcpy(s.items[0].id,"timer");strcpy(s.items[0].action,"ring");strcpy(s.items[0].text,"wake");s.items[0].at_var=0;s.items[0].background=true;return s;}
static void core_deadlines(void){
 pet_cev_clock_snapshot_t c={.valid=true,.utc_ms=1700000000000ULL,.calibration_generation=1},unknown={0};
 pet_cev_alarm_set_t s=configuration();pet_cev_alarms_observe_var_clock(&s,0,60000,100,&c);
 assert(s.items[0].utc_deadline_ms==1700000060000ULL&&!s.items[0].waiting_for_clock);
 char text[PET_CEV_SAVE_TEXT_MAX];assert(pet_cev_alarms_serialize_clock(&s,100,&c,text,sizeof text));assert(strstr(text,":u:v60000"));
 pet_cev_alarm_set_t restored=configuration();assert(pet_cev_alarms_restore_clock(&restored,text,0,&unknown));
 assert(restored.items[0].waiting_for_clock&&!pet_cev_alarms_tick_clock(&restored,900000,&unknown));assert(!pet_cev_alarms_take_pending(&restored));
 c.utc_ms=1700000059999ULL;c.calibration_generation=2;assert(!pet_cev_alarms_tick_clock(&restored,900000,&c));
 c.utc_ms=1700000060000ULL;assert(pet_cev_alarms_tick_clock(&restored,900001,&c)==1);assert(pet_cev_alarms_take_pending(&restored)==1);assert(!pet_cev_alarms_tick_clock(&restored,900002,&c));
 /* Saving already-due before tick preserves pending, but does not deliver until calibrated. */
 assert(pet_cev_alarms_serialize_clock(&s,100,&c,text,sizeof text));assert(strstr(text,":p"));restored=configuration();assert(pet_cev_alarms_restore_clock(&restored,text,0,&unknown));assert(restored.items[0].pending&&!pet_cev_alarms_take_pending(&restored));pet_cev_alarms_tick_clock(&restored,1,&c);assert(pet_cev_alarms_take_pending(&restored)==1);
 /* Bad suffix/duplicate id is transactional, including clock metadata. */
 pet_cev_alarm_set_t kept=restored;assert(!pet_cev_alarms_restore_clock(&restored,"timer:1700000060000:u:v60000;timer:1",0,&unknown));assert(!memcmp(&kept,&restored,sizeof kept));assert(!pet_cev_alarms_restore_clock(&restored,"timer:1:u:u",0,&unknown));assert(!memcmp(&kept,&restored,sizeof kept));
 /* Legacy saves keep a relative duration; new unknown-clock arms wait explicitly. */
 restored=configuration();assert(pet_cev_alarms_restore_clock(&restored,"timer:60000:v60000",0,&unknown));assert(restored.items[0].waiting_for_clock);c.utc_ms=1700001000000ULL;assert(!pet_cev_alarms_tick_clock(&restored,500000,&c));assert(restored.items[0].utc_deadline_ms==c.utc_ms+60000);
 s=configuration();pet_cev_alarms_observe_var_clock(&s,0,60000,0,&unknown);assert(s.items[0].waiting_for_clock);assert(pet_cev_alarms_serialize_clock(&s,999999,&unknown,text,sizeof text)&&strstr(text,":w"));
 pet_cev_alarms_cancel(&s,0);assert(pet_cev_alarms_serialize_clock(&s,0,&unknown,text,sizeof text)&&strstr(text,":c"));restored=configuration();assert(pet_cev_alarms_restore_clock(&restored,text,0,&c));pet_cev_alarms_observe_var_clock(&restored,0,60000,0,&c);assert(!pet_cev_alarms_tick_clock(&restored,999999,&c));
 /* Periodic catch-up remains O(1), preserves its UTC phase and only one pending delivery. */
 s=configuration();s.items[0].at_var=-1;s.items[0].every_ms=60000;pet_cev_alarms_start_clock(&s,0,&c);c.utc_ms+=60000ULL*1000000+5;assert(pet_cev_alarms_tick_clock(&s,1,&c)==1);assert(s.items[0].utc_deadline_ms==c.utc_ms+59995);
}
static void maximum_save_budget(void){
 pet_cev_alarm_set_t s={0};s.count=4;pet_cev_clock_snapshot_t c={0};
 for(unsigned i=0;i<4;i++){pet_cev_alarm_t*a=&s.items[i];snprintf(a->id,sizeof a->id,"alarm_name_%04u",i);a->at_var=(int8_t)i;a->last_var=INT32_MIN;a->armed=true;a->pending=true;a->utc_deadline=true;a->utc_deadline_ms=4103049599000ULL;}
 char text[PET_CEV_SAVE_TEXT_MAX];size_t n=pet_cev_alarms_serialize_clock(&s,0,&c,text,sizeof text);assert(n&&n<256&&n+12<=268);
 for(size_t cap=2;cap<=n;cap++){char tiny[PET_CEV_SAVE_TEXT_MAX];memset(tiny,0xa5,sizeof tiny);assert(!pet_cev_alarms_serialize_clock(&s,0,&c,tiny,cap)&&!tiny[0]);}
 pet_cev_alarm_set_t old=s;assert(!pet_cev_alarms_restore_clock(&s,"alarm_name_0000:18446744073709551616:u",0,&c)&&!memcmp(&s,&old,sizeof s));
}
static void lifecycle(void){
 sync_clock(1700000000,480,1000);pet_cev_clock_snapshot_t c=miniapp_utc_snapshot(2000);assert(c.valid&&c.utc_ms==1700000001000ULL&&c.monotonic_at_sync==1000&&c.calibration_generation==1);assert(local_wall_seconds(2000)==1700028801);
 sync_clock(1700000001,-300,2000);pet_cev_clock_snapshot_t same=miniapp_utc_snapshot(2000);assert(same.utc_ms==c.utc_ms&&same.calibration_generation==2);assert(!miniapp_utc_snapshot(1999).valid);
 ext_alarm_entry_t*e=ext_alarm_entry("parked",true);strcpy(e->title,"Parked");e->alarms=configuration();pet_cev_alarms_observe_var_clock(&e->alarms,0,60000,mono,&same);
 char alarms[PET_CEV_SAVE_TEXT_MAX];assert(pet_cev_alarms_serialize_clock(&e->alarms,mono,&same,alarms,sizeof alarms));
 snprintf(saved,sizeof saved,"{\"version\":1,\"state\":\"ready\",\"vars\":{\"keep\":42},\"alarms\":\"%s\"}",alarms);strcpy(durable,saved);
 oom=true;assert(pet_p4_miniapp_alarm_cancel("parked","timer"));assert(e->dirty&&e->alarms.items[0].cancelled&&!strcmp(saved,durable)&&!live_alloc);oom=false;
 fail_update=true;assert(!miniapp_ext_hidden_checkpoint()&&e->dirty&&!live_alloc);fail_update=false;assert(miniapp_ext_hidden_checkpoint()&&!e->dirty&&!live_alloc);
 cJSON*j=cJSON_Parse(saved);assert(j&&cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j,"vars"),"keep")->valueint==42);assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"state")->valuestring,"ready"));assert(strstr(cJSON_GetObjectItemCaseSensitive(j,"alarms")->valuestring,":c"));cJSON_Delete(j);
 strcpy(durable,saved); /* existing A/B worker's accepted immutable checkpoint */
 miniapp_runtime_t next={0};next.cext.alarms=configuration();j=cJSON_Parse(durable);assert(miniapp_ext_restore_text(&next,cJSON_GetObjectItemCaseSensitive(j,"alarms")));cJSON_Delete(j);assert(next.cext.alarms.items[0].cancelled);
 pet_p4_miniapp_alarm_policy(true);mono+=1000000;miniapp_ext_process(mono,false);char title[64],notice[PET_CEV_ALARM_TEXT_MAX];assert(!pet_p4_miniapp_alarm_notification(title,notice)&&!dispatches);
 assert(updates==2);pthread_mutex_destroy(atomic_load(&g_ext_lock));free(atomic_load(&g_ext_lock));
}
int main(void){char unused[256];(void)miniapp_ext_save_text(unused,sizeof unused);core_deadlines();maximum_save_budget();EXTRA_MAIN lifecycle();puts("UTC, timezone, reboot waiting, cancellation checkpoint and no hidden dispatch OK");}
'''.replace('EXTRA_MAIN',extra_main)
    (tmp_path/'utc.c').write_text(harness)
    binary=tmp_path/'utc'
    subprocess.run(['cc','-std=c11','-pthread','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-Wno-error=deprecated-declarations','-fsanitize=address,undefined','-I',str(tmp_path),
                    '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(tmp_path/'utc.c'),
                    str(ROOT/'main/pet_p4_component_events.c'),str(ROOT/'tests/vendor/cjson/cJSON.c'),
                    *map(str,extra_sources),'-lm','-o',str(binary)],check=True)
    run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
    assert run.returncode==0 and not run.stderr,(run.returncode,run.stdout,run.stderr)
    assert 'no hidden dispatch OK' in run.stdout

def test_utc_deadline_reboot_unknown_clock_and_hidden_cancel_checkpoint(tmp_path):
    _run_utc_test(tmp_path)
