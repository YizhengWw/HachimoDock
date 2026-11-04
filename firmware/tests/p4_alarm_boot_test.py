"""Extract production boot grant + periodic delivery; real storage-lane/UTC core.

Disk/package APIs are injected at their boundary to force load/checksum/OOM and
concurrent lifecycle failures. They assert IO never holds state/model/ext locks.
"""
from p4_alarm_utc_test import ROOT, _run_utc_test


def test_coldboot_hidden_alarm_grant_atomicity_and_no_hidden_execution(tmp_path):
    prefix = r'''
#include "pet_p4_storage_lane.h"
#define PET_P4_MINIAPP_CATALOG_MAX 32
typedef struct {char widget_id[48];char title[64];uint8_t resource_state;uint32_t checksum;} miniapp_catalog_item_t;
typedef enum {PET_APP_STORE_EMPTY,PET_APP_STORE_VALUE,PET_APP_STORE_BUSY,PET_APP_STORE_UNAVAILABLE} pet_app_store_status_t;
static miniapp_catalog_item_t g_catalog[32];static size_t g_catalog_count;
static uint32_t g_catalog_sequence;static bool g_catalog_recovery_blocked;
static pet_storage_lane_t lane;
static unsigned g_storage_resource_lease=1,package_reads,disk_reads;
static unsigned bad_package,bad_save,load_error,change_catalog,disable_during_load,create_live,fail_later;
static bool storage_audio_busy(void){return false;}
static bool storage_work_current(void){return true;}
static bool storage_scope_begin(pet_storage_scope_t*s,bool activation){(void)activation;return pet_storage_scope_begin(&lane,s);}
static void storage_scope_finish(pet_storage_scope_t*s){if(s->active)assert(pet_storage_scope_finish(s));}
static bool safe_widget_id(const char*s){return s&&s[0];}
static pet_app_store_status_t pet_app_store_load_with_lease(const char*,char*,unsigned);
static bool load_catalog_runtime_item(miniapp_catalog_item_t*,miniapp_runtime_t*,bool,char*,size_t);
static bool restore_app_runtime_buffer(miniapp_runtime_t*,bool,char*);
'''
    body = r'''
static void unlocked(void){
 assert(!state_depth&&!model_depth);
 SemaphoreHandle_t m=atomic_load(&g_ext_lock);if(m){assert(pthread_mutex_trylock(m)==0);pthread_mutex_unlock(m);}
}
static pet_cev_alarm_set_t boot_config(void){
 pet_cev_alarm_set_t a={0};a.count=1;strcpy(a.items[0].id,"timer");strcpy(a.items[0].text,"wake");strcpy(a.items[0].action,"NEVER_HIDDEN_ACTION");a.items[0].at_var=0;a.items[0].background=true;return a;
}
static pet_app_store_status_t pet_app_store_load_with_lease(const char*id,char*out,unsigned lease){
 unlocked();assert(lease==1);disk_reads++;
 if(load_error)return PET_APP_STORE_UNAVAILABLE;
 if(!strcmp(id,"no-save"))return PET_APP_STORE_EMPTY;
 if(!strcmp(id,"no-alarms")){strcpy(out,"{\"version\":1,\"vars\":{}}");return PET_APP_STORE_VALUE;}
 snprintf(out,PET_APP_SAVE_BYTES,"{\"version\":1,\"vars\":{},\"alarms\":\"timer:%s\"}",!strcmp(id,"cancelled")?"0:v60000:c":"1700000060000:u:v60000");
 if(bad_save)strcpy(out,"{bad");
 if(change_catalog)g_catalog_sequence++;
 if(disable_during_load)pet_p4_miniapp_alarm_policy(false);
 if(create_live){ext_lock();ext_alarm_entry_t*e=ext_alarm_entry(id,true);e->alarms=boot_config();pet_cev_alarms_cancel(&e->alarms,0);ext_unlock();}
 return PET_APP_STORE_VALUE;
}
static bool load_catalog_runtime_item(miniapp_catalog_item_t*i,miniapp_runtime_t*r,bool missing,char*err,size_t size){
 unlocked();assert(!missing&&err&&size);package_reads++;if(bad_package||(fail_later&&package_reads==2))return false;
 assert(i->checksum==123);strcpy(r->widget_id,i->widget_id);strcpy(r->view.title,"Saved hidden component");r->cext.events=true;r->cext.alarms=boot_config();r->save_version=1;return true;
}
static bool restore_app_runtime_buffer(miniapp_runtime_t*r,bool cached,char*json){
 unlocked();assert(cached);cJSON*j=cJSON_Parse(json);bool ok=j&&miniapp_ext_restore_text(r,cJSON_GetObjectItemCaseSensitive(j,"alarms"));cJSON_Delete(j);return ok;
}
#include "pet_p4_miniapp_alarm_boot.inc"
static void state_unlock(void*ctx){(void)ctx;assert(state_depth==1);state_depth=0;}
static void state_lock(void*ctx){(void)ctx;assert(!state_depth);state_depth=1;}
static uintptr_t owner(void*ctx){(void)ctx;return 1;}
static bool grant(bool value){
 state_depth=1;assert(pet_storage_lane_enter(&lane));bool ok=pet_p4_miniapp_alarm_policy_prepare(value);
 assert(state_depth==1&&pet_storage_lane_leave(&lane));state_depth=0;assert(!live_alloc);return ok;
}
static void boot_reset(void){
 memset(g_ext_alarms,0,sizeof g_ext_alarms);memset(&g_runtime,0,sizeof g_runtime);g_runtime.active=true;strcpy(g_runtime.widget_id,"other-active");
 memset(g_catalog,0,sizeof g_catalog);g_catalog_count=5;
 const char*ids[]={"other-active","hidden","cancelled","no-save","no-alarms"};
 for(unsigned i=0;i<5;i++){strcpy(g_catalog[i].widget_id,ids[i]);g_catalog[i].resource_state=1;g_catalog[i].checksum=123;}
 g_wall_valid=false;g_clock_generation=0;mono=0;g_ext_notice_pending=false;g_ext_background_notify=false;g_ext_alarm_policy_revision=0;g_ext_alarm_serial=0;
 bad_package=bad_save=load_error=change_catalog=disable_during_load=create_live=fail_later=package_reads=disk_reads=0;
}
static void boot_lifecycle(void){
 assert(pet_storage_lane_configure(&lane,state_unlock,state_lock,owner,NULL));
 boot_reset();assert(grant(true)&&g_ext_background_notify&&package_reads==2);
 assert(ext_alarm_entry("hidden",false)&&ext_alarm_entry("cancelled",false));
 assert(!ext_alarm_entry("no-save",false)&&!ext_alarm_entry("no-alarms",false)&&!ext_alarm_entry("other-active",false));
 assert(ext_alarm_entry("hidden",false)->alarms.items[0].waiting_for_clock);
 char title[64],notice[PET_CEV_ALARM_TEXT_MAX];mono=900000;miniapp_ext_process(mono,false);
 assert(!pet_p4_miniapp_alarm_notification(title,notice)&&!dispatches);
 cJSON*c=cJSON_Parse("{\"unixSeconds\":1700000060,\"utcOffsetMinutes\":480}");assert(pet_p4_miniapp_clock_sync(c,mono));cJSON_Delete(c);
 miniapp_ext_process(mono,false);assert(pet_p4_miniapp_alarm_notification(title,notice)&&!strcmp(notice,"wake")&&!dispatches);
 assert(!strcmp(g_runtime.widget_id,"other-active")&&ext_alarm_entry("cancelled",false)->alarms.items[0].cancelled);
 unsigned read_count=disk_reads;assert(grant(true)&&disk_reads==read_count+2); /* only absent/no-alarm records revisited */
 assert(grant(false)&&!g_ext_background_notify&&!g_ext_notice_pending);
 boot_reset();oom=true;assert(!grant(true)&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false));oom=false;
 for(unsigned failure=0;failure<4;failure++){
  boot_reset();bad_package=failure==0;bad_save=failure==1;load_error=failure==2;change_catalog=failure==3;
  assert(!grant(true)&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false));
 }
 boot_reset();fail_later=1;assert(!grant(true)&&package_reads==2&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false)); /* no prefix commit */
 boot_reset();pet_p4_miniapp_alarm_policy(true);bad_save=1;assert(!grant(true)&&g_ext_background_notify); /* preserve old grant */
 boot_reset();disable_during_load=1;assert(!grant(true)&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false));
 boot_reset();create_live=1;assert(grant(true)&&ext_alarm_entry("hidden",false)->alarms.items[0].cancelled); /* fresh RAM wins */
 boot_reset();g_catalog_count=1;strcpy(g_catalog[0].widget_id,"hidden");g_catalog[0].resource_state=3;assert(grant(true)&&!disk_reads&&!ext_alarm_entry("hidden",false)); /* disabled/damaged skipped */
 boot_reset();g_ext_alarm_serial=UINT64_MAX;assert(!grant(true)&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false));
 boot_reset();for(unsigned i=0;i<PET_CEV_COMPONENTS;i++){char id[48];snprintf(id,sizeof id,"retained-%u",i);assert(ext_alarm_entry(id,true));}
 assert(!grant(true)&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false)&&ext_alarm_entry("retained-31",false));
 assert(!updates&&!dispatches); /* boot prepare never writes a default/save, dispatches, or changes active selection */
 boot_reset();memset(g_ext_alarms,0,sizeof g_ext_alarms);g_catalog_count=0;memset(&g_runtime,0,sizeof g_runtime);
}
'''
    _run_utc_test(tmp_path, prefix, body, 'boot_lifecycle();',
                  (ROOT/'main/pet_p4_storage_lane.c',))


def test_policy_protocol_waits_for_worker_prepare_before_success_ack():
    source=(ROOT/'main/pet_p4_protocol.c').read_text()
    policy=source[source.index('strcmp(topic, "component/alarm_policy")'):source.index('strcmp(topic, "ui/conversation")',source.index('strcmp(topic, "component/alarm_policy")'))]
    assert 'pet_p4_miniapp_alarm_policy_prepare(cJSON_IsTrue(background))' in policy
    assert policy.index('"alarm_restore_unavailable"') < policy.index('send_protocol_ack_if_requested')
    helper=(ROOT/'main/pet_p4_miniapp_alarm_boot.inc').read_text()
    for forbidden in ('pet_p4_miniapp_dispatch_action(', 'miniapp_ext_publish(',
                      'pet_program_registry_start(', 'pet_app_store_update('):
        assert forbidden not in helper
    assert 'storage.may_unlock' in helper
