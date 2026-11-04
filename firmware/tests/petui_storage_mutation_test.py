"""Execute production install/delete with phase fault and concurrent input hooks.

No device, audio, user files or packages are used. Flash adapters assert that
the real mutation handler has released the state lane before calling them.
"""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def test_real_mutations_publish_after_durability_and_restore_all_error_locks(tmp_path):
    _run_real_mutations(tmp_path,False)

def test_optin_unchanged_commit_preserves_all_original_mutation_cases_and_live_state(tmp_path):
    _run_real_mutations(tmp_path,True)

def _mutation_source(unchanged):
    code = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    install = code[code.index('bool pet_p4_miniapp_install_commit('):code.index('static bool commit_builtin_package(')]
    remove = function(code, 'remove_component')
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_storage_lane.h"
#define PET_P4_MINIAPP_CATALOG_MAX 32
#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
#define MINIAPP_DELETE_TARGETS 18
#define PET_FILE_CLEANUP_MAX_TARGETS 24
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define PET_WIDGET_TRACE_SCOPE(name,kind) int name=0
#define pet_widget_trace_scope_end(name) ((void)(name))
static unsigned held=1,critical,unlocks,locks,io_calls,commits,cleanups,forgets;
static bool audio,fail_load,fail_save,fail_schema,forget_fail,requires_touch;
static bool work_current=true,revoke_restore,revoke_commit;
static int fail_io;
static uintptr_t task=1;
#define miniapp_runtime_lock() do{assert(held&&!critical);critical=1;}while(0)
#define miniapp_runtime_unlock() do{assert(critical);critical=0;}while(0)
typedef struct {uint8_t count;} sprites_t;
typedef struct {bool active,repair_resources;char widget_id[48],transfer_id[64],widget_json[512],buttons_json[512];size_t widget_len,buttons_len;sprites_t sprites;} staging_t;
typedef struct {char widget_id[48],title[64];uint8_t slot,package_generation;uint32_t widget_checksum,buttons_checksum,sprites_checksum;uint8_t resource_state;} miniapp_catalog_item_t;
typedef struct {bool active,slicing;char widget_id[48];struct{char title[64];}view;unsigned state_count,var_count,transition_count,tick_count;int counter;} miniapp_runtime_t;
typedef struct {uint8_t generation;uint32_t sequence;} miniapp_catalog_commit_t;
typedef struct {char path[48];} pet_file_cleanup_target_t;
typedef struct {uint32_t sequence;} pet_file_cleanup_plan_t;
static staging_t g_staging;
static miniapp_runtime_t g_runtime;
static miniapp_catalog_item_t g_catalog[32];
static size_t g_catalog_count,g_catalog_selected;
static uint32_t g_catalog_sequence,g_image_revision;
static int g_catalog_file_generation;
static bool g_catalog_recovery_blocked,g_builtin_sync_in_progress,g_catalog_seed_pending;
static char g_storage_failure[128],g_image_id[16];
static size_t g_image_used;static uint32_t g_image_next;
static pet_storage_lane_t g_storage_lane;
static int saved_counter,prepared_counter;
static uintptr_t owner(void *ctx){(void)ctx;return task;}
static void unlock(void *ctx){(void)ctx;assert(held&&!critical);held=0;unlocks++;}
static void lock(void *ctx){(void)ctx;assert(!held&&!critical);held=1;locks++;}
static void storage_scope_finish(pet_storage_scope_t *scope){if(scope->active)assert(pet_storage_scope_finish(scope));}
static bool storage_scope_begin(pet_storage_scope_t *scope,bool activation_job){(void)activation_job;return pet_storage_scope_begin(&g_storage_lane,scope);}
static bool storage_work_current(void){return work_current;}
static void catalog_scratch_free(miniapp_catalog_item_t **p){free(*p);}
static bool storage_audio_busy(void){return audio;}
static bool pet_p4_audio_active(void){return audio;}
static bool pet_p4_audio_playback_active(void){return false;}
static bool pet_p4_audio_conversation_active(void){return false;}
static bool pet_p4_audio_music_active(void){return false;}
static void set_error(char *e,size_t n,const char *s){if(e&&n)snprintf(e,n,"%s",s);}
static void copy_utf8(char *out,size_t n,const char *s){snprintf(out,n,"%s",s);}
static const char *json_string(const cJSON *p,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,k);return cJSON_IsString(v)?v->valuestring:"";}
static bool safe_widget_id(const char *id){return id&&id[0]&&strlen(id)<48;}
static bool transfer_matches(const cJSON *p){return g_staging.active&&!strcmp(json_string(p,"transferId"),g_staging.transfer_id);}
static int catalog_find(const miniapp_catalog_item_t *items,size_t count,const char *id){for(size_t i=0;i<count;i++)if(!strcmp(items[i].widget_id,id))return (int)i;return -1;}
static int catalog_first_free_slot(const miniapp_catalog_item_t *items,size_t count){(void)items;return (int)count;}
static int catalog_visible_preferred_in(const miniapp_catalog_item_t *items,size_t count,int selected){(void)items;return count?(selected>=0&&(size_t)selected<count?selected:0):-1;}
static bool hot_update_save_compatible(miniapp_runtime_t *next){(void)next;assert(!held);return !fail_schema;}
static bool pet_p4_builtin_is_bootstrap(const char *id,uint32_t sum){(void)id;(void)sum;return false;}
static uint32_t miniapp_checksum(const char *s,size_t n){(void)s;return (uint32_t)n;}
static uint32_t sprite_pack_checksum(const sprites_t *p){return p->count?100:0;}
static bool resource_repair_package_matches(const miniapp_catalog_item_t *i,uint32_t w,uint32_t s){return i&&i->widget_checksum==w&&i->sprites_checksum==s;}
static void catalog_slot_path(size_t slot,uint8_t gen,bool buttons,bool tmp,char *out,size_t n){snprintf(out,n,"%zu-%u-%u-%u",slot,gen,buttons,tmp);}
static bool io(void){
 assert(!held&&!critical&&g_storage_lane.occupied);io_calls++;
 pet_storage_scope_t competing={0};assert(!pet_storage_scope_begin(&g_storage_lane,&competing));
 /* The main lane can still render/accept input while flash is in progress. */
 g_runtime.counter++;
 return !fail_io||(unsigned)fail_io!=io_calls;
}
static bool save_app_runtime(void){assert(held&&!critical);saved_counter=g_runtime.counter;return true;}
static bool parse_runtime(miniapp_runtime_t *r,const char *id,const char *w,const char *b,const sprites_t *s,char *e,size_t n){
 (void)w;(void)b;(void)s;(void)e;(void)n;assert(!held);r->active=true;r->slicing=requires_touch;snprintf(r->widget_id,48,"%s",id);snprintf(r->view.title,64,"Title");return true;
}
static bool write_spare_package_json(size_t slot,uint8_t gen,bool buttons,const char *data,size_t n){
 (void)buttons;(void)data;(void)n;
 for(size_t i=0;i<g_catalog_count;i++)assert(g_catalog[i].slot!=slot||g_catalog[i].package_generation!=gen);
 return io();
}
static bool persist_sprite_pack(size_t slot,uint8_t generation,const sprites_t *pack){(void)slot;(void)generation;(void)pack;return io();}
static bool read_json_file(const char *path,char *out,size_t n){(void)path;if(!io())return false;snprintf(out,n,"{}");return true;}
static bool load_catalog_runtime_item(miniapp_catalog_item_t *i,miniapp_runtime_t *r,bool allow,char *error,size_t n){
 (void)allow;(void)error;(void)n;if(!io()||fail_load)return false;r->active=true;strcpy(r->widget_id,i->widget_id);i->resource_state=1;return true;
}
static bool restore_component_runtime(miniapp_runtime_t *r){assert(!held);prepared_counter=saved_counter;r->counter=prepared_counter;if(revoke_restore)work_current=false;return !fail_save;}
static void merge_live_persistent_runtime(miniapp_runtime_t *r){assert(held);r->counter=saved_counter;}
static bool write_catalog_snapshot_at(const miniapp_catalog_item_t *items,size_t count,const char *id,const char *selected,bool seed_pending,int generation,uint32_t sequence,miniapp_catalog_commit_t *out){
 (void)items;(void)count;(void)id;assert(selected||!count);assert(!seed_pending);assert(sequence==g_catalog_sequence);if(!io())return false;
 commits++;out->generation=generation<0?0:1-generation;out->sequence=sequence+1;if(revoke_commit)work_current=false;return true;
}
static void component_cleanup_targets(uint8_t slot,pet_file_cleanup_target_t *out){(void)slot;memset(out,0,sizeof(*out)*MINIAPP_DELETE_TARGETS);}
static uint32_t cleanup_guard(uint32_t *sequence){return *sequence;}
static bool pet_file_cleanup_prepare(pet_file_cleanup_plan_t *plan,const char *root,pet_file_cleanup_target_t *targets,size_t n,uint32_t seq){(void)root;(void)targets;(void)n;assert(!held&&commits);plan->sequence=seq;return true;}
static bool pet_file_cleanup_remove_at(pet_file_cleanup_plan_t *plan,size_t index){(void)index;assert(!held&&g_storage_lane.occupied&&g_catalog_sequence==plan->sequence);cleanups++;return true;}
#define g_storage_resource_lease 1
static bool pet_app_store_forget_with_lease(const char *id,int lease){(void)id;assert(lease==1&&!held&&commits);forgets++;return !forget_fail;}
''' + install + remove + r'''
static void reset(void){
 assert(held&&!g_storage_lane.occupied);memset(g_catalog,0,sizeof(g_catalog));memset(&g_runtime,0,sizeof(g_runtime));memset(&g_staging,0,sizeof(g_staging));
 g_catalog_count=2;g_catalog_selected=0;g_catalog_sequence=20;g_catalog_file_generation=0;
 g_catalog[0]=(miniapp_catalog_item_t){.widget_id="garden",.title="Garden",.slot=3,.package_generation=0};
 g_catalog[1]=(miniapp_catalog_item_t){.widget_id="book",.title="Book",.slot=7,.package_generation=1};
 g_runtime.active=true;strcpy(g_runtime.widget_id,"garden");g_runtime.counter=10;
 g_staging.active=true;strcpy(g_staging.transfer_id,"transfer");strcpy(g_staging.widget_id,"garden");
 strcpy(g_staging.widget_json,"{}");g_staging.widget_len=2;strcpy(g_staging.buttons_json,"{}");g_staging.buttons_len=2;
 io_calls=commits=cleanups=forgets=0;audio=fail_load=fail_save=fail_schema=forget_fail=false;g_catalog_seed_pending=false;fail_io=0;g_catalog_recovery_blocked=false;
 work_current=true;revoke_restore=revoke_commit=false;requires_touch=false;
}
int main(void){
 char error[160];cJSON *p=cJSON_Parse("{\"transferId\":\"transfer\",\"widgetId\":\"garden\"}");assert(p);
 assert(pet_storage_lane_configure(&g_storage_lane,unlock,lock,owner,NULL));
 // Late commit belongs to an old transfer: preserve all accepted staging.
 reset();staging_t before_staging=g_staging;
 cJSON *late=cJSON_Parse("{\"transferId\":\"old-transfer\",\"widgetId\":\"garden\"}");assert(late);
 assert(pet_storage_lane_enter(&g_storage_lane));
 assert(!pet_p4_miniapp_install_commit(late,error,sizeof(error)));
 assert(!memcmp(&before_staging,&g_staging,sizeof(g_staging))&&!io_calls&&!commits&&held);
 assert(pet_storage_lane_leave(&g_storage_lane));cJSON_Delete(late);
 reset();assert(pet_storage_lane_enter(&g_storage_lane));
 assert(pet_p4_miniapp_install_commit(p,error,sizeof(error)));
 assert(held&&!g_storage_lane.occupied&&g_catalog_sequence==21&&commits==1);
 assert(g_runtime.counter==15&&saved_counter==15&&prepared_counter<saved_counter);
 assert(pet_storage_lane_leave(&g_storage_lane));
 // A parsed touch component is installable before hardware readiness. No
 // touch-ready stub exists: reintroducing an install read fails compilation.
 reset();requires_touch=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(pet_p4_miniapp_install_commit(p,error,sizeof(error)));
 assert(commits==1&&g_catalog_sequence==21&&!g_staging.active&&held&&!g_storage_lane.occupied);
 assert(pet_storage_lane_leave(&g_storage_lane));
 // The same touch declaration must still fail a real saved-schema conflict.
 reset();requires_touch=true;fail_schema=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(!pet_p4_miniapp_install_commit(p,error,sizeof(error))&&!io_calls&&!commits&&g_staging.active);
 assert(pet_storage_lane_leave(&g_storage_lane));
 // Schema incompatibility must be rejected before touching even the spare
 // package generation, with the transfer and existing private state retained.
 reset();fail_schema=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(!pet_p4_miniapp_install_commit(p,error,sizeof(error))&&!io_calls&&!commits&&g_staging.active&&g_catalog_sequence==20);
 assert(strstr(error,"saved schema")&&pet_storage_lane_leave(&g_storage_lane));
 // Failure of each file, readback or catalog commit never publishes/deletes.
 for(int stage=1;stage<=5;stage++){
   reset();fail_io=stage;assert(pet_storage_lane_enter(&g_storage_lane));
   assert(!pet_p4_miniapp_install_commit(p,error,sizeof(error)));
   assert(held&&!g_storage_lane.occupied&&g_catalog_sequence==20&&commits==0&&g_catalog_count==2);
   assert(pet_storage_lane_leave(&g_storage_lane));
 }
 reset();audio=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(!pet_p4_miniapp_install_commit(p,error,sizeof(error))&&g_staging.active&&io_calls==0);
 assert(pet_storage_lane_leave(&g_storage_lane));
 reset();fail_save=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(!pet_p4_miniapp_install_commit(p,error,sizeof(error))&&!commits&&g_catalog_sequence==20);
 assert(pet_storage_lane_leave(&g_storage_lane));
 reset();fail_save=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(!remove_component("garden",error,sizeof(error),false)&&!commits&&!forgets&&g_catalog_count==2);
 assert(pet_storage_lane_leave(&g_storage_lane));
 reset();assert(pet_storage_lane_enter(&g_storage_lane));
 assert(remove_component("garden",error,sizeof(error),true));
 assert(held&&!g_storage_lane.occupied&&commits==1&&cleanups==18&&!forgets);
 assert(g_catalog_count==1&&!strcmp(g_runtime.widget_id,"book")&&g_catalog_sequence==21);
 assert(pet_storage_lane_leave(&g_storage_lane));
 reset();assert(pet_storage_lane_enter(&g_storage_lane));
 assert(remove_component("book",error,sizeof(error),false));
 assert(g_catalog_count==1&&forgets==1&&cleanups==18&&!strcmp(g_runtime.widget_id,"garden"));
 assert(pet_storage_lane_leave(&g_storage_lane));
 // A durable catalog deletion with a failed save-clear intent is partial,
 // never reported as successful clear-save. The retained lease still closes.
 reset();forget_fail=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(!remove_component("book",error,sizeof(error),false));
 assert(g_catalog_count==1&&g_catalog_sequence==21&&forgets==1&&strstr(error,"component removed; private save"));
 assert(held&&!g_storage_lane.occupied&&pet_storage_lane_leave(&g_storage_lane));
 for(int stage=1;stage<=2;stage++){
   reset();fail_io=stage;assert(pet_storage_lane_enter(&g_storage_lane));
   assert(!remove_component("garden",error,sizeof(error),false));
   assert(held&&!g_storage_lane.occupied&&g_catalog_count==2&&!cleanups&&!forgets&&!commits);
   assert(pet_storage_lane_leave(&g_storage_lane));
 }
 // Revoked pre-commit jobs abort; successful durable commits still publish.
 reset();revoke_restore=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(!pet_p4_miniapp_install_commit(p,error,sizeof(error))&&!commits&&g_catalog_sequence==20);
 assert(pet_storage_lane_leave(&g_storage_lane));
 reset();revoke_commit=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(pet_p4_miniapp_install_commit(p,error,sizeof(error))&&commits==1&&g_catalog_sequence==21);
 assert(g_runtime.counter==saved_counter&&pet_storage_lane_leave(&g_storage_lane));
 reset();revoke_restore=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(!remove_component("garden",error,sizeof(error),false)&&!commits&&!forgets&&g_catalog_count==2);
 assert(pet_storage_lane_leave(&g_storage_lane));
 reset();revoke_commit=true;assert(pet_storage_lane_enter(&g_storage_lane));
 assert(remove_component("garden",error,sizeof(error),false)&&commits==1&&forgets==1&&!cleanups);
 assert(g_catalog_count==1&&!strcmp(g_runtime.widget_id,"book")&&pet_storage_lane_leave(&g_storage_lane));
 assert(locks==unlocks);cJSON_Delete(p);return 0;
}
'''
    if unchanged:
        # Keep the complete original suite, then exercise the production no-op
        # branch too. State-lane and model-lock are distinct in production;
        # physical verification takes only the latter, never the former.
        source=source.replace('assert(held&&!critical);critical=1;','assert(!critical);critical=1;')
        source=source.replace('bool active,repair_resources;','bool active,repair_resources,preserve_unchanged;')
        source=source.replace('typedef struct {uint8_t count;} sprites_t;',r'''
typedef struct {uint8_t frame_width,frame_height,frames,fps;unsigned data_offset,data_length;} pet_p4_miniapp_sprite_t;
typedef struct {uint8_t count;unsigned data_length;uint8_t pixels[1024];pet_p4_miniapp_sprite_t items[4];} sprites_t;
''')
        helpers=r'''
#define MINIAPP_SPRITE_FILE_HEADER_SIZE 8
typedef struct {bool active;} pet_storage_physical_scope_t;
static bool g_install_unchanged,physical;
static bool pet_storage_physical_begin(pet_storage_physical_scope_t*s){assert(!held&&!critical&&!physical);physical=s->active=true;return true;}
static bool pet_storage_physical_finish(pet_storage_physical_scope_t*s){assert(!held&&!critical&&physical&&s->active);physical=s->active=false;return true;}
static void catalog_sprite_path(size_t s,uint8_t g,unsigned i,bool t,char*out,size_t n){snprintf(out,n,"%zu-%u-%u-%u-s",s,g,i,t);}
'''+function(code,'unchanged_storage_current')+'\n'+function(code,'preserve_unchanged_package')
        source='#define PET_P4_STORAGE_PRESERVE_UNCHANGED 1\n#include "pet_p4_file_exact.h"\n'+source
        source=source.replace(install,helpers+'\n'+install,1)
        cases=r'''
 // Exact active package leaves the complete model/catalog revision untouched.
 reset();g_staging.preserve_unchanged=true;g_catalog[0].resource_state=1;
 strcpy(g_catalog[0].title,"Title");g_catalog[0].widget_checksum=2;g_catalog[0].buttons_checksum=2;
 FILE*f=fopen("3-0-0-0","wb");assert(f&&fwrite("{}",1,2,f)==2&&!fclose(f));
 f=fopen("3-0-1-0","wb");assert(f&&fwrite("{}",1,2,f)==2&&!fclose(f));
 miniapp_runtime_t unchanged_runtime=g_runtime;
 miniapp_catalog_item_t unchanged_catalog[32];memcpy(unchanged_catalog,g_catalog,sizeof(g_catalog));
 uint32_t unchanged_revision=g_image_revision;
 assert(pet_storage_lane_enter(&g_storage_lane));
 assert(pet_p4_miniapp_install_commit(p,error,sizeof(error))&&g_install_unchanged);
 assert(!io_calls&&!commits&&!cleanups&&!physical&&!g_staging.active&&held&&!g_storage_lane.occupied);
 assert(g_catalog_sequence==20&&g_image_revision==unchanged_revision&&g_catalog_selected==0);
 assert(!memcmp(&unchanged_runtime,&g_runtime,sizeof(g_runtime))&&!memcmp(unchanged_catalog,g_catalog,sizeof(g_catalog)));
 assert(pet_storage_lane_leave(&g_storage_lane));
 // A same-length/checksum collision MUST run the original durable transaction.
 reset();g_staging.preserve_unchanged=true;g_catalog[0].resource_state=1;
 strcpy(g_catalog[0].title,"Title");g_catalog[0].widget_checksum=2;g_catalog[0].buttons_checksum=2;
 f=fopen("3-0-0-0","wb");assert(f&&fwrite("[]",1,2,f)==2&&!fclose(f));
 assert(pet_storage_lane_enter(&g_storage_lane));
 assert(pet_p4_miniapp_install_commit(p,error,sizeof(error))&&!g_install_unchanged&&commits==1&&g_catalog_sequence==21);
 assert(!physical&&held&&pet_storage_lane_leave(&g_storage_lane));
'''
        source=source.replace('assert(locks==unlocks);cJSON_Delete(p);return 0;',cases+'\n assert(locks==unlocks);cJSON_Delete(p);return 0;',1)
    return source


def _run_real_mutations(tmp_path,unchanged):
    source = _mutation_source(unchanged)
    src = tmp_path / 'mutation.c'
    src.write_text(source)
    exe = tmp_path / 'mutation'
    vendor = ROOT / 'tests/vendor/cjson'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-error=deprecated-declarations',
                    '-fsanitize=address,undefined', '-I', str(ROOT / 'main'),
                    '-I', str(vendor), str(src), str(vendor / 'cJSON.c'),
                    str(ROOT / 'main/pet_p4_storage_lane.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)],cwd=tmp_path,check=True)
