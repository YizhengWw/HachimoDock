"""Production media publication/snapshot/render entrypoint regressions; no device."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]

def run(tmp_path,code,name='entry'):
    src=tmp_path/f'{name}.c';exe=tmp_path/name;src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=15)

def test_production_snapshot_captures_media_generation_before_unlock(tmp_path):
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define PET_P4_CAPABILITY_EXT 1
typedef struct{bool enabled;}data_t;
typedef struct{bool active,app_surface,instrument,slicing,media;unsigned surface_page,revision;char data_source[48];int data_page;data_t data;}view_t;
typedef struct{bool app;unsigned count;}surface_t;
typedef struct{view_t view;surface_t surface;int scene,art;bool surface_active,side_active,images_changed,confirmation_active;char media_source[48],confirmation_prompt[121];uint32_t image_revision,media_generation;}pet_p4_miniapp_render_snapshot_t;
typedef struct{int unused;}pet_p4_miniapp_sprite_pack_t;
static struct{view_t view;bool active,app_host,side_enabled,media;surface_t surface;int side,side_art;char media_source[48],widget_id[48];struct{uint32_t media_generation;}cext;}g_runtime;
static int g_render_host,depth;static uint32_t g_image_revision=17;
static void miniapp_runtime_lock(void){assert(!depth);depth=1;}
static void miniapp_runtime_unlock(void){assert(depth);depth=0;g_runtime.cext.media_generation=99;g_runtime.view.revision=99;}
static uint64_t esp_timer_get_time(void){return 1000000;}
static bool confirmation_locked(char*out,uint64_t now){(void)out;(void)now;assert(depth);return false;}
static bool runtime_host_dashboard(void*r){(void)r;return false;}
static void pet_app_host_render_capture(const char*id,int*host,view_t*view,uint64_t now){(void)id;(void)host;(void)view;(void)now;assert(depth);}
static bool app_surface_locked(surface_t*s,int*host){(void)host;assert(depth);*s=g_runtime.surface;return true;}
static bool surface_images_locked(pet_p4_miniapp_sprite_pack_t*s,uint32_t*r){(void)s;(void)r;assert(depth);return false;}
static void pet_p4_widget_data_view(const char*src,int page,uint64_t now,data_t*out){(void)src;(void)page;(void)now;(void)out;assert(!depth);}
'''+function(text,'pet_p4_miniapp_render_snapshot')+r'''
int main(void){pet_p4_miniapp_render_snapshot_t out={0};pet_p4_miniapp_sprite_pack_t images={0};uint32_t rev=0;
 g_runtime.active=true;g_runtime.view.active=true;g_runtime.view.revision=17;g_runtime.cext.media_generation=17;g_runtime.surface.app=true;
 assert(pet_p4_miniapp_render_snapshot(&out,&images,&rev));assert(out.media_generation==17&&out.view.revision==17&&out.surface_active);
 assert(g_runtime.cext.media_generation==99&&g_runtime.view.revision==99&&!depth);
 assert(!pet_p4_miniapp_render_snapshot(NULL,&images,&rev));return 0;}
'''
    run(tmp_path,code,'snapshot')

def test_production_render_media_gate_polls_until_worker_and_decoder_finish(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text();ext=(ROOT/'main/pet_p4_renderer_ext.inc').read_text()
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define PET_P4_CLIP 1
#define PET_P4_H264_WORKER_PACK 1
#define PET_P4_APP_PPA_COPY 1
#define PET_P4_APP_COLD_FAST 1
#define PET_P4_APP_CLEAR_OPAQUE_CENTER 1
#define PET_P4_APP_DIRTY_CACHE 1
#define PET_P4_H264_PREFETCH 1
#include "pet_p4_surface.h"
#define PET_PREFETCH_EMPTY 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef struct{struct{bool active,app_surface;unsigned surface_page;}view;bool surface_active;uint32_t media_generation;pet_surface_t surface;}pet_p4_miniapp_render_snapshot_t;
static struct{uint64_t token;uint32_t generation;}g_clip_lease;
static struct{unsigned decoder_readers;struct{unsigned generation;}owner;int buffer_lease;const char*bytes;}g_clip_entry;
static struct{unsigned phase;}g_h264_prefetch_lifetime;
static int g_clip,g_asset_prepare_mux;static bool release_busy;static bool worker_finished;static unsigned resets,releases,revokes,finishes,decoder_starts;static uint32_t captured;
static bool h264_prefetch_reset(void){resets++;return worker_finished;}
static void renderer_ext_media_snapshot(uint32_t g){captured=g;}
static bool pet_cmedia_unpin_deferred(uint64_t t){assert(t==7&&!g_clip_entry.decoder_readers&&!g_h264_prefetch_lifetime.phase);if(release_busy)return false;releases++;return true;}
static void pet_resources_device_revoke(__typeof__(g_clip_entry.owner) o){assert(o.generation);revokes++;}
static bool pet_resources_device_finish(int lease){(void)lease;finishes++;return true;}
'''+''.join(function((ROOT/'main/pet_p4_surface.c').read_text(),n)+'\n' for n in ('pet_surface_node_viewport','pet_surface_clamp_offset','pet_surface_project'))+function(ext,'renderer_ext_clip_retire')+'\n'+function(text,'renderer_component_media_frame')+r'''
int main(void){
 g_clip_lease.token=7;g_clip_lease.generation=42;g_clip_entry.decoder_readers=1;g_clip_entry.bytes="live";g_clip_entry.owner.generation=1;g_h264_prefetch_lifetime.phase=3;
 pet_p4_miniapp_render_snapshot_t s={.view={.active=true,.app_surface=true,.surface_page=0},.surface_active=true,.media_generation=42,.surface={.count=1,.nodes={{.type=PET_SURFACE_NODE_CLIP,.page=4}}}};
 assert(renderer_component_media_frame(&s)&&captured==42&&!resets);
 s.surface.nodes[0].hidden=true;assert(!renderer_component_media_frame(&s)&&captured==42&&resets==1&&g_clip_lease.token&&!releases);
 /* Main/exit frames must keep calling retire, never offer another decoder. */
 for(unsigned frame=0;frame<4;frame++){bool idle=renderer_component_media_frame(NULL);if(idle)decoder_starts++;assert(!idle&&captured==0&&!decoder_starts&&!releases);}
 worker_finished=true;assert(!renderer_component_media_frame(NULL)&&!releases);
 g_clip_entry.decoder_readers=0;assert(!renderer_component_media_frame(NULL)&&!releases);
 g_h264_prefetch_lifetime.phase=PET_PREFETCH_EMPTY;release_busy=true;
 assert(!renderer_component_media_frame(NULL)&&g_clip_lease.token&&g_clip_entry.bytes&&!releases&&!revokes);
 release_busy=false;bool idle=renderer_component_media_frame(NULL);if(idle)decoder_starts++;
 assert(idle&&decoder_starts==1&&releases==1&&revokes==1&&finishes==1&&!g_clip_lease.token&&!g_clip_entry.bytes);
 assert(renderer_component_media_frame(NULL)&&releases==1);
 s.surface.nodes[0].hidden=false;s.surface.nodes[0].page=1;assert(renderer_component_media_frame(&s)&&captured==42);return 0;
}
'''
    run(tmp_path,code,'retire')
    entry=function(text,'pet_p4_renderer_render')
    assert entry.index('renderer_component_media_frame(NULL)')<entry.index('component_media_idle&&render_asset_pet_frame(')
    assert 'if(!component_media_idle)g_asset_cache_pending=true;' in entry
    app=function(text,'render_miniapp_page')
    assert app.index('renderer_component_media_frame(')<app.index('render_package_app(')


def test_real_install_delete_abort_before_catalog_and_stop_on_publish_failure(tmp_path,monkeypatch):
    import petui_storage_mutation_test as mutations
    original=Path.write_text
    def write(path,source,*args,**kwargs):
        if path.name=='mutation.c':
            source='#define PET_P4_CAPABILITY_EXT 1\n'+source
            source=source.replace('int counter;} miniapp_runtime_t;','int counter;int cext;} miniapp_runtime_t;')
            source=source.replace('assert(held&&!critical);critical=1;','assert(!critical);critical=1;')
            marker='bool pet_p4_miniapp_install_commit('
            at=source.index(marker)
            adapters=r'''
typedef struct{miniapp_runtime_t *next;bool prepared;}pet_cext_publish_ticket_t;
static bool media_fail_prepare,media_fail_commit;static unsigned media_prepares,media_commits,media_aborts,media_removed;static pet_cext_publish_ticket_t *pending;
static bool pet_cmedia_stage_matches(const int*r,char*error,size_t n){(void)r;(void)error;(void)n;return true;}
static bool pet_cmedia_persist(size_t s,uint8_t g,const int*r){(void)s;(void)g;(void)r;assert(!held);return true;}
static bool miniapp_ext_prepare(miniapp_runtime_t *next,size_t slot,uint8_t generation,pet_cext_publish_ticket_t*t){
 (void)slot;(void)generation;assert(!held&&!critical&&!pending&&!commits);media_prepares++;
 if(media_fail_prepare)return false;
 t->next=next;
 t->prepared=true;
 pending=t;
 return true;
}
static bool miniapp_ext_commit(pet_cext_publish_ticket_t*t){assert(held&&!critical&&commits==1&&pending==t&&t->prepared);if(media_fail_commit)return false;media_commits++;pending=NULL;t->prepared=false;return true;}
static bool miniapp_ext_abort(pet_cext_publish_ticket_t*t){assert(pending==t&&t->prepared&&!critical);media_aborts++;pending=NULL;t->prepared=false;return true;}
static void miniapp_ext_installed(const miniapp_runtime_t*r){(void)r;assert(commits==1&&!pending);}
static void miniapp_ext_removed(const char*id,size_t slot){(void)id;(void)slot;assert(commits==1&&!pending);media_removed++;}
'''
            source=source[:at]+adapters+source[at:]
            source=source.replace('static void reset(void){','static void reset(void){\n assert(!pending);media_fail_prepare=media_fail_commit=false;media_prepares=media_commits=media_aborts=media_removed=0;')
            cases=r'''
 // Prepare failure is before the irreversible catalog write and preserves runtime.
 for(unsigned deletion=0;deletion<2;deletion++){
  reset();miniapp_runtime_t old=g_runtime;media_fail_prepare=true;assert(pet_storage_lane_enter(&g_storage_lane));
  bool ok=deletion?remove_component("garden",error,sizeof(error),false):pet_p4_miniapp_install_commit(p,error,sizeof(error));
  assert(!ok&&!commits&&!media_commits&&!media_removed&&!pending&&media_prepares==1);
  assert(!strcmp(g_runtime.widget_id,old.widget_id)&&g_catalog_sequence==20&&g_catalog_count==2&&!g_catalog_recovery_blocked);
  assert(pet_storage_lane_leave(&g_storage_lane));
 }
 // A failed durable write aborts the exact prepared ticket and keeps old media.
 reset();fail_io=5;assert(pet_storage_lane_enter(&g_storage_lane));assert(!pet_p4_miniapp_install_commit(p,error,sizeof(error)));
 assert(media_prepares==1&&media_aborts==1&&!pending&&!media_commits&&!commits&&g_catalog_sequence==20);assert(pet_storage_lane_leave(&g_storage_lane));
 reset();fail_io=2;assert(pet_storage_lane_enter(&g_storage_lane));assert(!remove_component("garden",error,sizeof(error),false));
 assert(media_prepares==1&&media_aborts==1&&!pending&&!media_commits&&!commits&&g_catalog_count==2);assert(pet_storage_lane_leave(&g_storage_lane));
 // Defensive commit failure must not continue runtime/catalog publication or cleanup.
 for(unsigned deletion=0;deletion<2;deletion++){
  reset();media_fail_commit=true;assert(pet_storage_lane_enter(&g_storage_lane));
  bool ok=deletion?remove_component("garden",error,sizeof(error),false):pet_p4_miniapp_install_commit(p,error,sizeof(error));
  assert(!ok&&commits==1&&!media_commits&&!pending&&media_aborts==1&&g_catalog_recovery_blocked);
  assert(g_catalog_sequence==20&&g_catalog_count==2&&!cleanups&&!forgets&&!media_removed&&!strcmp(g_runtime.widget_id,"garden"));
  assert(pet_storage_lane_leave(&g_storage_lane));
 }
'''
            source=source.replace(' assert(locks==unlocks);cJSON_Delete(p);return 0;',cases+'\n assert(locks==unlocks);cJSON_Delete(p);return 0;',1)
        return original(path,source,*args,**kwargs)
    monkeypatch.setattr(Path,'write_text',write)
    mutations._run_real_mutations(tmp_path,False)


def test_real_memory_activation_does_not_publish_after_media_failure(tmp_path):
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PET_P4_CAPABILITY_EXT 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef struct{int version;}miniapp_runtime_t;
typedef struct{miniapp_runtime_t *next;bool ready;}pet_cext_publish_ticket_t;
static miniapp_runtime_t g_runtime={1};static unsigned g_image_revision=1,g_catalog_selected;
static struct{unsigned slot,package_generation;char widget_id[20];}g_catalog[1];
static bool fail_prepare,fail_commit,g_catalog_recovery_blocked;static unsigned aborted,published;
static bool catalog_entry_visible(size_t i){return !i;}
static bool load_catalog_runtime(size_t i,miniapp_runtime_t*n,char*error,size_t size){(void)i;(void)error;(void)size;n->version=2;return true;}
static bool save_app_runtime(void){return true;}
static bool restore_component_runtime(miniapp_runtime_t*n){(void)n;return true;}
static bool miniapp_ext_prepare(miniapp_runtime_t*n,size_t s,uint8_t g,pet_cext_publish_ticket_t*t){(void)s;(void)g;if(fail_prepare)return false;t->next=n;t->ready=true;return true;}
static bool miniapp_ext_commit(pet_cext_publish_ticket_t*t){assert(t->ready&&g_runtime.version==1);if(fail_commit)return false;t->ready=false;published++;return true;}
static bool miniapp_ext_abort(pet_cext_publish_ticket_t*t){assert(t->ready);t->ready=false;aborted++;return true;}
static void miniapp_runtime_lock(void){}
static void miniapp_runtime_unlock(void){}
'''+function(text,'activate_catalog_index_in_memory')+r'''
int main(void){fail_prepare=true;assert(!activate_catalog_index_in_memory(0)&&g_runtime.version==1&&g_image_revision==1&&!published);
 fail_prepare=false;fail_commit=true;assert(!activate_catalog_index_in_memory(0)&&g_runtime.version==1&&g_image_revision==1&&aborted==1);
 fail_commit=false;assert(activate_catalog_index_in_memory(0)&&g_runtime.version==2&&g_image_revision==2&&published==1);return 0;}
'''
    run(tmp_path,code,'memory')


def test_real_prepared_activation_aborts_ticket_on_cancel_or_failure(tmp_path,monkeypatch):
    import petui_cold_activation_test as cold
    original=Path.write_text
    def write(path,source,*args,**kwargs):
        if path.name=='cold.c':
            source='#define PET_P4_CAPABILITY_EXT 1\n'+source
            at=source.index('static bool resume_catalog_index(')
            adapters=r'''
typedef struct{miniapp_runtime_t *next;bool prepared;}pet_cext_publish_ticket_t;
static bool media_fail_prepare,media_fail_commit;static unsigned media_prepares,media_commits,media_aborts;static pet_cext_publish_ticket_t *pending;
static bool miniapp_ext_prepare(miniapp_runtime_t*n,size_t slot,uint8_t gen,pet_cext_publish_ticket_t*t){
 (void)slot;(void)gen;assert((boot||!held)&&!critical&&!pending);media_prepares++;
 if(media_fail_prepare)return false;
 t->next=n;
 t->prepared=true;
 pending=t;
 return true;
}
static bool miniapp_ext_commit(pet_cext_publish_ticket_t*t){assert(held&&!critical&&pending==t&&t->prepared);if(media_fail_commit)return false;pending=NULL;t->prepared=false;media_commits++;return true;}
static bool miniapp_ext_abort(pet_cext_publish_ticket_t*t){assert((boot||!held)&&!critical&&pending==t&&t->prepared);pending=NULL;t->prepared=false;media_aborts++;return true;}
'''
            source=source[:at]+adapters+source[at:]
            source=source.replace('static void reset(void){','static void reset(void){\n assert(!pending);media_fail_prepare=media_fail_commit=false;media_prepares=media_commits=media_aborts=0;')
            cases=r'''
 reset();queue();media_fail_prepare=true;assert(run()==PET_MINIAPP_ACTIVATION_FAILED);
 assert(media_prepares==1&&!media_commits&&!pending&&g_image_revision==4&&!strcmp(g_runtime.widget_id,"old-app")&&!writes);
 reset();queue();media_fail_commit=true;assert(run()==PET_MINIAPP_ACTIVATION_FAILED);
 assert(media_prepares==1&&!media_commits&&media_aborts==1&&!pending&&g_image_revision==4&&!strcmp(g_runtime.widget_id,"old-app")&&!writes);
'''
            source=source.replace(' puts("cold activation production paths: all cases passed");',cases+'\n puts("cold activation production paths: all cases passed");')
        return original(path,source,*args,**kwargs)
    monkeypatch.setattr(Path,'write_text',write)
    cold.test_cold_activation_queue_prepare_publish_cancel_and_faults(tmp_path)


def test_actual_render_entry_keeps_polling_and_blocks_main_decoder_on_errors(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define TAG 0
#define PET_P4_NATIVE_OUTPUT_BUFFER_COUNT 2
#define BSP_LCD_H_RES 1
#define BSP_LCD_V_RES 1
#define PET_P4_UI_WIDTH 1
#define PET_P4_UI_HEIGHT 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_RETURN_ON_ERROR(e,tag,message) do{int err=(e);if(err)return err;}while(0)
typedef int esp_err_t;
typedef struct{unsigned session_queue_count;}pet_p4_runtime_state_t;
typedef struct{const char *page;bool realtime_conversation,component_loading,show_voice_overlay,show_bubble,compact_bubble;}pet_p4_view_model_t;
typedef struct{int unused;}pet_widget_trace_snapshot_t;
static struct{uint64_t start_us,previous_end_us,gap_us,content_us,common_overlay_us,end_us;int ppa;}g_render_trace;
static uint16_t outputs[2][1],*g_native_framebuffers[2]={outputs[0],outputs[1]},*g_native_framebuffer=outputs[0];
static unsigned g_native_framebuffer_index,g_render_trace_frame,g_lcd_skipped,g_lcd_submitted;
static int64_t g_previous_render_ended_us;
static bool g_main_appearance_ready,g_last_native_valid,g_app_logical_same,g_asset_cache_pending,g_framebuffer_initialized,g_last_realtime_conversation,g_last_approval_visible,g_session_overlay_cache_valid,g_logged_missing_asset;
static unsigned g_last_decode_us,g_last_h264_pack_us,g_last_h264_convert_us;
static char g_last_render_page[32];static bool worker_idle,init_fail,rotate_fail;static unsigned retire_polls,decoder_starts;
static bool renderer_component_media_frame(const void*s){assert(!s);retire_polls++;return worker_idle;}
static int pet_p4_renderer_init(void){return init_fail?7:0;}
static int64_t esp_timer_get_time(void){static int64_t t;return ++t;}
static unsigned elapsed_us_clamped(int64_t a,int64_t b){return b-a;}
#define pet_p4_render_trace_begin(...) ((void)0)
#define ppa_trace_frame_begin(...) ((void)0)
#define pet_widget_trace_snapshot(...) ((void)0)
#define pet_p4_render_trace_observe(...) ((void)0)
#define renderer_cancel_inactive_prepares(...) ((void)0)
#define fill_rect(...) ((void)0)
#define rgb565(...) 0
#define center_art_release(...) ((void)0)
#define instrument_release(...) ((void)0)
#define render_component_center_page(...) ((void)0)
#define render_touch_test_page(...) ((void)0)
#define render_miniapp_page(...) ((void)0)
static bool render_asset_pet_frame(const pet_p4_runtime_state_t*s,const pet_p4_view_model_t*v,uint64_t n){(void)s;(void)v;(void)n;assert(worker_idle);decoder_starts++;return true;}
#define draw_standalone_voice_overlay(...) ((void)0)
#define pet_compact_underlay_forget(...) ((void)0)
#define main_compact_underlay_prepare(...) ((void)0)
#define draw_bubble(...) ((void)0)
#define draw_session_queue(...) ((void)0)
#define pet_p4_miniapp_media_active(...) false
#define pet_p4_miniapp_instrument_config(...) false
#define pet_p4_miniapp_slicing_active(...) false
#define draw_touch_feedback(...) ((void)0)
#define draw_page_indicator(...) ((void)0)
#define draw_connection_banner(...) ((void)0)
#define draw_boot_diagnostic(...) ((void)0)
#define pet_p4_state_approval_pending(...) false
#define draw_approval_overlay(...) ((void)0)
static int rotate_landscape_to_panel(void){return rotate_fail?8:0;}
#define pet_p4_frame_unchanged(...) false
#define pet_p4_lcd_draw_rgb565(...) 0
#define ppa_trace_frame_snapshot(...) ((void)0)
#define record_render_performance(a,b,c,d,e,f) ((void)(a),(void)(b),(void)(c),(void)(d),(void)(e),(void)(f))
'''+function(text,'pet_p4_renderer_render')+r'''
int main(void){pet_p4_runtime_state_t s={0};pet_p4_view_model_t v={.page="main"};
 for(unsigned frame=0;frame<4;frame++){assert(pet_p4_renderer_render(&s,&v,frame)==0);assert(retire_polls==frame+1&&!decoder_starts&&g_asset_cache_pending);}
 rotate_fail=true;assert(pet_p4_renderer_render(&s,&v,10)==8&&retire_polls==5&&!decoder_starts);
 init_fail=true;assert(pet_p4_renderer_render(&s,&v,11)==7&&retire_polls==6&&!decoder_starts);init_fail=rotate_fail=false;
 worker_idle=true;assert(pet_p4_renderer_render(&s,&v,12)==0&&retire_polls==7&&decoder_starts==1&&!g_asset_cache_pending);
 v.page="components";worker_idle=false;assert(pet_p4_renderer_render(&s,&v,13)==0&&retire_polls==8&&decoder_starts==1);return 0;}
'''
    run(tmp_path,code,'render-entry')


def test_frame_and_periodic_media_calls_do_not_use_blocking_store_entries():
    ext = (ROOT/'main/pet_p4_renderer_ext.inc').read_text()
    mini = (ROOT/'main/pet_p4_miniapp_ext.inc').read_text()
    assert 'pet_cmedia_unpin(' not in ext
    assert 'pet_cmedia_big_meta_at(' not in ext
    assert 'pet_cmedia_big_tile_at(' not in ext
    assert 'pet_cmedia_clip_pin_at(' not in ext
    periodic = function(mini, 'miniapp_ext_process')
    assert 'pet_cmedia_collect_try()' in periodic
    assert 'pet_cmedia_collect()' not in periodic
    prepare = function(mini, 'miniapp_ext_prepare')
    # Slow storage preparation cannot hold the event/notification bus mutex.
    io = prepare.index('pet_cmedia_prepare(')
    assert prepare.rfind('ext_unlock()', 0, io) > prepare.rfind('ext_lock()', 0, io)


def test_publication_never_waits_for_media_or_model_under_ext():
    source = (ROOT/'main/pet_p4_miniapp_ext.inc').read_text()
    for name in ('miniapp_ext_prepare', 'miniapp_ext_abort', 'miniapp_ext_commit'):
        body = function(source, name)
        import re
        for call in re.finditer(r'pet_cmedia_(?:prepare|abort|commit|generation|collect)\s*\(|miniapp_runtime_lock\s*\(|pet_p4_audio_cext_set\s*\(', body):
            before = body[:call.start()]
            assert before.rfind('ext_unlock()') > before.rfind('ext_lock()'), (name, call[0])
    prepare = function(source, 'miniapp_ext_prepare')
    assert 'g_ext_pending_publish == ticket && g_ext_publish_phase == EXT_PUBLISH_PREPARING' in prepare
