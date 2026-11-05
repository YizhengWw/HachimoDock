"""Run production cold activation with interleaved main-lane input and I/O faults.

The extracted production functions execute with the real storage-lane module.
Only RTOS, storage adapters and the large runtime layout are substituted; every
adapter asserts whether the actual state lock is held. No USB or audio is used.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_cold_activation_queue_prepare_publish_cancel_and_faults(tmp_path):
    source = (ROOT / "main/pet_p4_miniapp.c").read_text()
    header = (ROOT / "main/pet_p4_miniapp.h").read_text()
    api = header[header.index("typedef bool (*pet_p4_miniapp_activation_schedule_fn)"):
                 header.index("bool pet_p4_miniapp_sync_stats(")]
    code = source[source.index("static bool resume_catalog_index("):
                  source.index("static bool remove_component(const char *widget_id, char *error, size_t error_size, bool preserve_save);")]
    def function(signature):
        start = source.index(signature)
        end = source.index("\n}\n", start) + 3
        return source[start:end]
    code = "\n".join(function(signature) for signature in (
        "static bool catalog_item_visible(", "static bool catalog_entry_visible(",
        "static int catalog_visible_index(", "bool pet_p4_miniapp_catalog_select(")) + code
    slot = source[source.index("typedef struct {\n  uint64_t serial;"):
                  source.index("bool pet_p4_miniapp_storage_lane_configure(")]
    wrappers = source[source.index("static bool storage_work_current("):
                      source.index("static bool storage_audio_busy(")]
    prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_storage_lane.h"
#include "pet_p4_resources.h"
#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
static bool held=true,critical,audio,fail_alloc,fail_load,fail_restore,fail_write,fail_queue;
static unsigned saves,fail_save_at,loads,restores,writes,unlocks,locks,allocations;
static int saved_counter;
static uintptr_t task=1;
static uint64_t queued_token;
static int hook;
static bool boot;
#define miniapp_runtime_lock() do{assert(held&&!critical);critical=true;}while(0)
#define miniapp_runtime_unlock() do{assert(critical);critical=false;}while(0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(tag,...) ((void)printf(__VA_ARGS__))
#define PET_WIDGET_TRACE_SCOPE(name,kind) int name=0
#define pet_widget_trace_scope_end(p) ((void)(p))
typedef struct {bool active,media;char widget_id[48];unsigned save_version;int counter;} miniapp_runtime_t;
typedef struct {char widget_id[48];unsigned slot,package_generation,resource_state,checksum;} miniapp_catalog_item_t;
static miniapp_runtime_t g_runtime;
static miniapp_catalog_item_t g_catalog[3];
static size_t g_catalog_selected,g_catalog_count=3;
static uint32_t g_catalog_sequence,g_image_revision;
static struct {bool active;} g_staging;
static pet_storage_lane_t g_storage_lane;
static pet_resource_broker_t broker;
static pet_resource_owner_t g_storage_resource_owner;
static pet_resource_lease_t g_storage_resource_lease;
static pet_resource_realtime_t realtime;
static bool g_catalog_recovery_blocked;
static pet_resource_result_t pet_resources_device_owner_open(uint32_t mask,pet_resource_owner_t *out){return pet_resources_owner_open(&broker,mask,out);}
static pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t owner,pet_resource_kind_t kind,uint32_t units,pet_resource_lease_t *out){return pet_resources_try_acquire(&broker,owner,kind,units,out);}
static bool pet_resources_device_current(pet_resource_lease_t lease){return pet_resources_current(&broker,lease);}
static void pet_resources_device_revoke(pet_resource_owner_t owner){pet_resources_owner_revoke(&broker,owner);}
static pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t lease){return pet_resources_finish(&broker,lease);}
static void enter_realtime(void){assert(pet_resources_realtime_begin(&broker,&realtime)==PET_RESOURCE_OK);}
static uintptr_t owner(void *ctx){(void)ctx;return task;}
static void unlock(void *ctx){(void)ctx;assert(held&&!critical);held=false;unlocks++;if(hook==10&&unlocks==2)enter_realtime();}
static void lock(void *ctx){(void)ctx;assert(!held&&!critical);held=true;locks++;}
static bool storage_audio_busy(void){return audio;}
static int64_t esp_timer_get_time(void){return 10;}
static int catalog_find(const miniapp_catalog_item_t *items,size_t n,const char *id){
 for(size_t i=0;i<n;i++)if(!strcmp(items[i].widget_id,id))return (int)i;
 return -1;
}
static void copy_utf8(char *out,size_t n,const char *id){snprintf(out,n,"%s",id);}
/* Cursor scheduling is an independent worker boundary: selection never does IO. */
static bool catalog_select_id_queued(const char *id){
 assert(held&&!critical);int at=catalog_find(g_catalog,g_catalog_count,id);
 if(at<0)return false;
 g_catalog_selected=(size_t)at;return true;
}
static bool save_app_runtime(void){assert(held&&!critical);saves++;if(saves==fail_save_at)return false;saved_counter=g_runtime.counter;return true;}
static void merge_live_persistent_runtime(miniapp_runtime_t *next){
 assert(held&&!critical);if(!strcmp(next->widget_id,g_runtime.widget_id))next->counter=g_runtime.counter;
}
static void *activation_calloc(size_t n,size_t size){assert(boot||!held);if(fail_alloc)return NULL;allocations++;return calloc(n,size);}
static void activation_free(void *p){assert(boot||!held);if(p){assert(allocations);allocations--;}free(p);}
static void io_hook(unsigned phase);
static bool load_catalog_runtime_item(miniapp_catalog_item_t *item,miniapp_runtime_t *next,bool allow,char *error,size_t n){
 (void)allow;(void)error;(void)n;assert(boot||(!held&&g_storage_lane.occupied));loads++;
 assert(item!=&g_catalog[0]&&item!=&g_catalog[1]);
 item->resource_state=7;io_hook(1);if(fail_load)return false;
 next->active=true;strcpy(next->widget_id,item->widget_id);next->media=item->slot<2;next->counter=50;return true;
}
static bool restore_component_runtime(miniapp_runtime_t *next){
 (void)next;assert(boot||!held);restores++;io_hook(2);return !fail_restore;
}
static bool pet_selection_write(uint32_t sequence,const char *id){
 assert(boot||!held);assert(sequence==g_catalog_sequence);assert(!strcmp(g_runtime.widget_id,id));
 writes++;io_hook(3);return !fail_write;
}
static bool catalog_cursor_write(uint32_t sequence,const char *id){
 assert(boot||!held);assert(sequence==g_catalog_sequence);assert(!strcmp(g_runtime.widget_id,id));
 return !fail_write;
}
'''
    suffix = r'''
static void io_hook(unsigned phase){
 if(boot)return;
 assert(!held&&!critical);
 /* Model the real main loop running while the worker has released its lock. */
 held=true;
 if(phase<3)g_runtime.counter++;
 assert(pet_p4_miniapp_storage_busy());
 assert(!pet_p4_miniapp_activation_reject_not_started(queued_token));
 assert(pet_p4_miniapp_activation_request_selected()==PET_MINIAPP_ACTIVATION_REJECTED);
 if(hook==1&&phase==1)pet_p4_miniapp_activation_cancel();
 if(hook==2&&phase==1){pet_p4_miniapp_activation_guard(false);pet_p4_miniapp_activation_guard(true);}
 if(hook==3&&phase==1){g_catalog_selected=2;pet_p4_miniapp_activation_guard(true);g_catalog_selected=1;}
 if(hook==4&&phase==1)g_catalog_sequence++;
 if(hook==5&&phase==1)g_catalog[1].package_generation++;
 if(hook==6&&phase==1)g_image_revision++;
 if(hook==7&&phase==1)audio=true;
 if(hook==8&&phase==3){g_runtime.counter=91;pet_p4_miniapp_activation_guard(false);}
 if((hook==9||hook==11)&&phase==1){
  enter_realtime();pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);
  assert(usage.leases[PET_RESOURCE_STORAGE_WORK]==1);assert(!storage_work_current());
  if(hook==11){assert(pet_resources_realtime_finish(&broker,realtime));memset(&realtime,0,sizeof(realtime));assert(!storage_work_current());}
 }
 held=false;
}
static bool schedule(uint64_t token,void *context){
 (void)context;assert(held&&!critical&&loads==0&&writes==0);queued_token=token;return !fail_queue;
}
static void reset(void){
 assert(held&&!critical&&!allocations);
 if(realtime.serial){assert(pet_resources_realtime_finish(&broker,realtime));memset(&realtime,0,sizeof(realtime));}
 pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);assert(!usage.live_owners&&!usage.revoked_owners&&!usage.leases[PET_RESOURCE_STORAGE_WORK]);
 memset(&broker,0,sizeof(broker));
 memset(&g_storage_lane,0,sizeof(g_storage_lane));memset(&g_activation,0,sizeof(g_activation));
 memset(&g_staging,0,sizeof(g_staging));memset(g_catalog,0,sizeof(g_catalog));
 strcpy(g_catalog[0].widget_id,"old-app");strcpy(g_catalog[1].widget_id,"next-app");strcpy(g_catalog[2].widget_id,"other-app");
 for(unsigned i=0;i<3;i++){g_catalog[i].slot=i;g_catalog[i].resource_state=1;g_catalog[i].checksum=10+i;}
 g_runtime=(miniapp_runtime_t){.active=true,.media=true,.counter=10};strcpy(g_runtime.widget_id,"old-app");
 g_catalog_selected=1;g_catalog_count=3;g_catalog_sequence=20;g_image_revision=4;
 loads=restores=writes=saves=unlocks=locks=fail_save_at=0;saved_counter=0;task=1;queued_token=0;
 audio=fail_alloc=fail_load=fail_restore=fail_write=fail_queue=boot=false;hook=0;
 assert(pet_storage_lane_configure(&g_storage_lane,unlock,lock,owner,NULL));
 assert(pet_p4_miniapp_activation_configure(schedule,NULL));
}
static pet_p4_miniapp_activation_result_t run(void){
 task=2;assert(pet_storage_lane_enter(&g_storage_lane));
 pet_p4_miniapp_activation_result_t result=pet_p4_miniapp_activation_run(queued_token);
 assert(held&&!critical&&!g_storage_lane.occupied&&!g_activation.active&&!allocations);
 assert(unlocks==locks);assert(pet_storage_lane_leave(&g_storage_lane));task=1;return result;
}
static void queue(void){
 assert(pet_p4_miniapp_activation_request_selected()==PET_MINIAPP_ACTIVATION_QUEUED);
 assert(!strcmp(g_runtime.widget_id,"old-app")&&g_image_revision==4&&loads==0&&writes==0&&saves==0);
 assert(pet_p4_miniapp_activation_request_selected()==PET_MINIAPP_ACTIVATION_REJECTED);
}
int main(void){
 reset();queue();assert(pet_p4_miniapp_activation_run(queued_token+1)==PET_MINIAPP_ACTIVATION_CANCELLED&&g_activation.active);
 assert(run()==PET_MINIAPP_ACTIVATION_OPENED);assert(!strcmp(g_runtime.widget_id,"next-app")&&g_image_revision==5);
 assert(saved_counter==12&&saves==2&&loads==1&&writes==1&&g_catalog[1].resource_state==1);
 assert(pet_p4_miniapp_activation_navigation_current(queued_token));
 assert(pet_p4_miniapp_activation_run(queued_token)==PET_MINIAPP_ACTIVATION_CANCELLED&&loads==1);
 pet_p4_miniapp_activation_guard(false);pet_p4_miniapp_activation_guard(true);
 assert(!pet_p4_miniapp_activation_navigation_current(queued_token)&&!strcmp(g_runtime.widget_id,"next-app"));
 // Visible UI indices map to physical catalog IDs even with hidden entries.
 reset();strcpy(g_catalog[0].widget_id,"music-player");
 assert(pet_p4_miniapp_catalog_select(0)&&g_catalog_selected==1);queue();
 assert(run()==PET_MINIAPP_ACTIVATION_OPENED&&!strcmp(g_runtime.widget_id,"next-app"));
 assert(g_catalog_count==3&&!strcmp(g_catalog[0].widget_id,"music-player"));
 reset();strcpy(g_catalog[1].widget_id,"music-player");
 assert(pet_p4_miniapp_catalog_select(1)&&g_catalog_selected==2);queue();
 assert(run()==PET_MINIAPP_ACTIVATION_OPENED&&!strcmp(g_runtime.widget_id,"other-app"));
 reset();queue();uint64_t older=queued_token;g_catalog_selected=2;
 assert(pet_p4_miniapp_activation_request_selected()==PET_MINIAPP_ACTIVATION_QUEUED&&queued_token!=older);
 assert(pet_p4_miniapp_activation_run(older)==PET_MINIAPP_ACTIVATION_CANCELLED&&pet_p4_miniapp_activation_pending());
 assert(run()==PET_MINIAPP_ACTIVATION_OPENED&&!strcmp(g_runtime.widget_id,"other-app"));
 reset();queue();assert(pet_p4_miniapp_activation_run(queued_token)==PET_MINIAPP_ACTIVATION_FAILED&&!pet_p4_miniapp_activation_pending());
 assert(!pet_p4_miniapp_storage_busy()&&loads==0);
 reset();queue();assert(!pet_p4_miniapp_activation_reject_not_started(queued_token+1));
 assert(pet_p4_miniapp_activation_reject_not_started(queued_token)&&!pet_p4_miniapp_activation_pending());
 assert(!pet_p4_miniapp_activation_reject_not_started(queued_token));
 // Existing warm media/saved apps are pure navigation, with no queue or I/O.
 reset();g_catalog_selected=0;assert(pet_p4_miniapp_activation_request_selected()==PET_MINIAPP_ACTIVATION_READY);
 assert(loads==0&&writes==0&&saves==0&&queued_token==0);
 audio=true;assert(pet_p4_miniapp_activation_request_selected()==PET_MINIAPP_ACTIVATION_READY&&loads==0);
 assert(activate_catalog_index(0)&&loads==0);audio=false;
 g_runtime.media=false;g_runtime.save_version=1;assert(pet_p4_miniapp_activation_request_selected()==PET_MINIAPP_ACTIVATION_READY);
 // A configured direct main/fallback call cannot smuggle flash work onto its lane.
 reset();assert(!activate_catalog_index(1)&&loads==0&&writes==0);
 // The synchronous protocol path uses the same unlocked preparation.
 task=2;assert(pet_storage_lane_enter(&g_storage_lane));assert(activate_catalog_index(1));
 assert(pet_storage_lane_leave(&g_storage_lane)&&held&&saved_counter==12&&writes==1);
 // Boot callers without lock callbacks remain synchronous and retain legacy ABI.
 reset();memset(&g_storage_lane,0,sizeof(g_storage_lane));boot=true;assert(activate_catalog_index(1)&&held&&unlocks==0);
 // No failure before publication changes active identity, image generation or NVS.
 for(int fault=1;fault<=5;fault++){
  reset();if(fault==1)fail_alloc=true;if(fault==2)fail_load=true;if(fault==3)fail_restore=true;
  if(fault==4)fail_save_at=1;
  if(fault==5)fail_save_at=2;
  queue();assert(run()==PET_MINIAPP_ACTIVATION_FAILED);
  assert(!strcmp(g_runtime.widget_id,"old-app")&&g_image_revision==4&&writes==0);
 }
 // Cancellation before worker, during package I/O, navigation and identity changes.
 reset();queue();pet_p4_miniapp_activation_cancel();assert(run()==PET_MINIAPP_ACTIVATION_CANCELLED&&loads==0);
 for(hook=1;hook<=6;){int h=hook;reset();hook=h;queue();assert(run()==PET_MINIAPP_ACTIVATION_CANCELLED);
  assert(!strcmp(g_runtime.widget_id,"old-app")&&writes==0&&!pet_p4_miniapp_activation_navigation_current(queued_token));hook=h+1;}
 reset();hook=7;queue();assert(run()==PET_MINIAPP_ACTIVATION_FAILED&&writes==0);
 // Selection persistence failure does not roll back or misreport activation.
 reset();fail_write=true;queue();assert(run()==PET_MINIAPP_ACTIVATION_OPENED_UNSAVED);
 assert(!strcmp(g_runtime.widget_id,"next-app")&&writes==1&&g_image_revision==5);
 // After publication, new input is retained and navigation cancellation is sticky.
 reset();hook=8;queue();assert(run()==PET_MINIAPP_ACTIVATION_OPENED);
 assert(g_runtime.counter==91&&!pet_p4_miniapp_activation_navigation_current(queued_token));
 // Real broker revocation preserves occupancy through I/O and prevents later phases.
 reset();hook=9;queue();assert(run()==PET_MINIAPP_ACTIVATION_FAILED&&restores==0&&writes==0);
 reset();hook=11;queue();assert(run()==PET_MINIAPP_ACTIVATION_FAILED&&restores==0&&writes==0);
 reset();hook=10;queue();assert(run()==PET_MINIAPP_ACTIVATION_OPENED_UNSAVED&&writes==0);
 assert(!strcmp(g_runtime.widget_id,"next-app"));
 reset();queue();enter_realtime();assert(run()==PET_MINIAPP_ACTIVATION_FAILED&&loads==0);
 reset();g_catalog[1].resource_state=2;queue();assert(run()==PET_MINIAPP_ACTIVATION_FAILED&&loads==0&&writes==0);
 // Queue failure, staging and token exhaustion cannot cause inline I/O/reuse.
 reset();fail_queue=true;assert(pet_p4_miniapp_activation_request_selected()==PET_MINIAPP_ACTIVATION_REJECTED);
 assert(!g_activation.active&&g_activation.serial==1&&loads==0);
 reset();g_staging.active=true;assert(pet_p4_miniapp_activation_request_selected()==PET_MINIAPP_ACTIVATION_REJECTED);
 reset();g_activation.serial=UINT64_MAX;assert(pet_p4_miniapp_activation_request_selected()==PET_MINIAPP_ACTIVATION_REJECTED);
 puts("cold activation production paths: all cases passed");
}
'''
    harness = tmp_path / "cold.c"
    harness.write_text(prelude + api + slot +
        "static bool pet_p4_miniapp_storage_busy(void){return g_storage_lane.occupied||g_activation.active;}\n"
        + wrappers + "#define calloc activation_calloc\n#define free activation_free\n" + code + suffix)
    exe = tmp_path / "cold"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I", str(ROOT / "main"), str(harness), str(ROOT / "main/pet_p4_storage_lane.c"),
        str(ROOT / "main/pet_p4_resources.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
