"""Production frame snapshots, with forced publication/reply interleaving.

No display, USB, audio, device or cloud. Native mutexes model concurrent replies;
the miniapp harness executes the actual capture/resolution code under a guarded
runtime lock and replaces the publication immediately after releasing that lock.
"""
from pathlib import Path
import json
import subprocess
import pytest
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'tests/vendor/cjson'


def compile_run(tmp_path, code, extra=(), flags=()):
    source = tmp_path / 'snapshot.c'
    source.write_text(code)
    exe = tmp_path / 'snapshot'
    subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-Wno-deprecated-declarations', '-I', str(tmp_path), '-I', str(ROOT / 'main'),
                    '-I', str(VENDOR), *flags, str(source), *map(str, extra),
                    str(VENDOR / 'cJSON.c'), '-lm', '-lpthread', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)


def _host_reply_immutable_case(tmp_path, owners, pending_case=False):
    (tmp_path / 'freertos').mkdir()
    (tmp_path / 'freertos/FreeRTOS.h').write_text('''#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(x) pthread_mutex_lock(x)
#define portEXIT_CRITICAL(x) pthread_mutex_unlock(x)
''')
    (tmp_path / 'esp_random.h').write_text('#include <stdint.h>\nstatic uint32_t esp_random(void){return 7;}\n')
    adapter = '''#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"
#define PET_P4_DEVICE_ID_MAX 64
typedef void (*pet_p4_send_line_fn)(const char*,void*);
typedef struct {char widget_id[48],eyebrow[96],headline[160],metric_value[64],metric_label[96],metric_unit[32],badge[16],note[160],footer[160];uint32_t revision;} pet_p4_miniapp_view_t;
bool pet_app_host_enabled(const char*,const char*,uint64_t);
'''
    code = (ROOT / 'main/pet_p4_app_host.c').read_text().replace('#include "pet_p4_app_host.h"', adapter)
    code += r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
static atomic_bool done;
static unsigned sends;
static char wire[2048];
static void send(const char *s,void *ctx){(void)ctx;sends++;snprintf(wire,sizeof(wire),"%s",s);}
static cJSON *reply;
static void publish(int n){
  char number[24];snprintf(number,sizeof(number),"%d",n);
  cJSON *v=cJSON_GetObjectItemCaseSensitive(reply,"values");
  cJSON_ReplaceItemInObjectCaseSensitive(v,"headline",cJSON_CreateString(number));
  cJSON *vars=cJSON_GetObjectItemCaseSensitive(reply,"variables");
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(vars,"a"),n);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(vars,"b"),n*2);
  cJSON *ui=cJSON_GetObjectItemCaseSensitive(reply,"ui");
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(ui,"mode"),(n%2));
  cJSON *a=cJSON_GetObjectItemCaseSensitive(ui,"actions");
  cJSON_ReplaceItemInArray(a,0,cJSON_CreateString(n%2?"host.odd":"host.even"));
  portENTER_CRITICAL(&mux);unsigned next=++sequence;portEXIT_CRITICAL(&mux);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(reply,"sequence"),next);
  assert(pet_app_host_apply(reply,2500));
}
static void *writer(void *unused){(void)unused;for(int i=1;i<=10000;i++)publish(i);atomic_store(&done,true);return NULL;}
int main(void){
  _Static_assert(sizeof(pet_host_render_t)<=1640,"bounded render-only copy");
  pet_app_host_transport(send,NULL,"board");pet_app_host_tick("app",1,0,true,2100);
  cJSON *event=cJSON_Parse(wire),*p=cJSON_GetObjectItemCaseSensitive(event,"payload");
  reply=cJSON_CreateObject();cJSON_AddStringToObject(reply,"componentId","app");
  cJSON_AddStringToObject(reply,"instance",str(p,"instance"));cJSON_AddNumberToObject(reply,"sequence",1);cJSON_Delete(event);
  cJSON *values=cJSON_AddObjectToObject(reply,"values");cJSON_AddStringToObject(values,"headline","0");
  cJSON *vars=cJSON_AddObjectToObject(reply,"variables");cJSON_AddNumberToObject(vars,"a",0);cJSON_AddNumberToObject(vars,"b",0);
  cJSON *ui=cJSON_AddObjectToObject(reply,"ui");cJSON_AddNumberToObject(ui,"mode",0);
  cJSON *actions=cJSON_AddArrayToObject(ui,"actions");cJSON_AddItemToArray(actions,cJSON_CreateString("host.even"));
  assert(pet_app_host_apply(reply,2500));
  pthread_t thread;assert(!pthread_create(&thread,NULL,writer,NULL));unsigned captures=0;
  do {
    pet_host_render_t s;pet_p4_miniapp_view_t view={0};strcpy(view.widget_id,"app");
    pet_app_host_render_capture("app",&s,&view,2501);
    assert(s.current&&s.fresh&&!s.pending&&!s.legacy);
    int32_t a,b;char text[64];
    assert(pet_host_render_variable(&s,"a",true,text,&a));
    assert(pet_host_render_variable(&s,"b",true,NULL,&b));
    assert(b==a*2&&atoi(view.headline)==a&&s.mode==a%2);
    assert(pet_host_render_enabled(&s,a%2?"host.odd":"host.even"));
    assert(!pet_host_render_enabled(&s,a%2?"host.even":"host.odd"));
    assert(!pet_host_render_variable(&s,"a",false,text,NULL));captures++;
#if PET_VARIABLE_OWNERS_ENABLED
    pet_host_render_t frame;pet_host_observation_t observation;
    pet_app_host_render_capture_observed("app",&frame,&observation,2501);
    int32_t fa,fb;assert(pet_host_render_variable(&frame,"a",true,NULL,&fa)&&pet_host_render_variable(&frame,"b",true,NULL,&fb));
    const char keys[2][32]={"a","b"};int32_t words[8]={-7,-9};
    if(pet_app_host_integer_snapshot_observed("app",keys,2,words,&observation,2501))assert(words[0]==fa&&words[1]==fb&&fb==fa*2);
    else assert(words[0]==-7&&words[1]==-9);
#endif
  }while(!atomic_load(&done)||captures<1000);
  assert(!pthread_join(thread,NULL));
#if PET_VARIABLE_OWNERS_ENABLED
  pet_host_render_t stable;pet_host_observation_t observation;
  pet_app_host_render_capture_observed("app",&stable,&observation,2501);
  const char keys[2][32]={"a","b"};int32_t words[8]={0};
  assert(pet_app_host_integer_snapshot_observed("app",keys,2,words,&observation,2501)&&words[0]==10000&&words[1]==20000);
  publish(10002);words[0]=-7;words[1]=-9;
  assert(!pet_app_host_integer_snapshot_observed("app",keys,2,words,&observation,2501)&&words[0]==-7&&words[1]==-9);
  pet_app_host_render_capture_observed("app",&stable,&observation,2501);
  assert(pet_app_host_integer_snapshot_observed("app",keys,2,words,&observation,2501)&&words[0]==10002&&words[1]==20004);
  assert(!pet_app_host_integer_snapshot_observed("app",keys,2,words,&observation,2499));
  assert(!pet_app_host_integer_snapshot_observed("app",keys,2,words,&observation,12500));
#endif
  pet_host_render_t old;pet_p4_miniapp_view_t view={0};strcpy(view.widget_id,"app");
  pet_app_host_render_capture("app",&old,&view,2501);assert(pet_host_render_enabled(&old,"host.even"));
  pet_host_observation_t observed;assert(pet_app_host_observe("app",&observed));
  pet_app_host_tick("",0,0,false,2600);pet_app_host_tick("app",1,0,true,2700);
  unsigned before=sends;assert(pet_app_host_observed_action("app",1,0,"host.even",&observed,2800));assert(sends==before);
  /* Old pixels may finish drawing; they never refresh current action rights. */
  assert(pet_host_render_enabled(&old,"host.even"));assert(!pet_app_host_observation_current("app",&observed));
  pet_host_render_t stale;pet_app_host_render_capture("app",&stale,&view,14000);assert(!stale.fresh&&stale.mode==7);
  pet_app_host_render_capture("other",&stale,NULL,2501);assert(!stale.current&&!stale.fresh&&stale.count==0);
  cJSON_Delete(reply);return 0;
}
'''
    if pending_case:
        assert owners==1 and code.count('  cJSON_Delete(reply);return 0;')==1
        block=r'''
  pet_app_host_transport(NULL,NULL,"board");pet_app_host_transport(send,NULL,"board");
  pet_app_host_tick("app",1,0,true,20000);
  event=cJSON_Parse(wire);p=cJSON_GetObjectItemCaseSensitive(event,"payload");
  cJSON_ReplaceItemInObjectCaseSensitive(reply,"instance",cJSON_CreateString(str(p,"instance")));cJSON_Delete(event);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(reply,"sequence"),1);
  assert(pet_app_host_apply(reply,20010));
  const char owned_keys[2][32]={"a","b"};int32_t owned_words[8]={-7,-9};
  assert(pet_app_host_owned_integer_snapshot("app",owned_keys,2,owned_words,20011)&&owned_words[0]==10002);
  pet_host_observation_t original;assert(pet_app_host_observe("app",&original));
  assert(pet_app_host_action("app",1,0,"host.even",20012));
  assert(!pet_app_host_observation_current("app",&original));
  owned_words[0]=-7;owned_words[1]=-9;
  assert(!pet_app_host_owned_integer_snapshot("app",owned_keys,2,owned_words,20013));
  assert(owned_words[0]==-7&&owned_words[1]==-9);
  pet_host_render_t pending_frame;pet_host_observation_t pending_observation;
  pet_app_host_render_capture_observed("app",&pending_frame,&pending_observation,20013);
  assert(pending_frame.current&&!pending_frame.fresh&&!pending_observation.valid);
  // Legacy status age and API remain unchanged, but a pending command is not
  // an actual data publication. It cannot revive data for native consumers.
  assert(pet_app_host_integer_snapshot("app",owned_keys,2,owned_words,30011));
  assert(!pet_app_host_owned_integer_snapshot("app",owned_keys,2,owned_words,30011));
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(reply,"sequence"),sequence);
  assert(pet_app_host_apply(reply,30012));
  assert(pet_app_host_owned_integer_snapshot("app",owned_keys,2,owned_words,30013));
  assert(!pet_app_host_apply(reply,30014)); // Duplicate cannot renew freshness.
  assert(!pet_app_host_owned_integer_snapshot("app",owned_keys,2,owned_words,40012));
'''
        code=code.replace('  cJSON_Delete(reply);return 0;',block+'\n  cJSON_Delete(reply);return 0;')
    compile_run(tmp_path, code, (ROOT / 'main/pet_p4_resources.c', ROOT / 'main/pet_p4_resources_device.c'),flags=(f'-DPET_VARIABLE_OWNERS_ENABLED={owners}',))


def test_one_host_reply_is_immutable_but_never_authorizes_an_action(tmp_path):
    _host_reply_immutable_case(tmp_path,0)


def test_mixed_pointer_and_program_words_use_one_observed_host_publication(tmp_path):
    _host_reply_immutable_case(tmp_path,1)


def test_pending_host_command_never_renews_native_data_or_stale_gesture(tmp_path):
    _host_reply_immutable_case(tmp_path,1,True)


@pytest.mark.parametrize('canvas,form2,collection', [(0,0,0), (1,0,0), (1,1,0), (1,1,1)])
def test_app_frame_uses_one_publication_and_no_live_host_reads_after_capture(tmp_path, canvas, form2, collection, phase_trace=0):
    text = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    resolver = text.split('static int32_t render_integer(', 1)[1].split('bool pet_p4_miniapp_media_config(', 1)[0]
    resolver = 'static int32_t render_integer(' + resolver
    resolver = '\n'.join(function(text,name) for name in ('runtime_host_variable','runtime_host_action','runtime_host_dashboard'))+'\n'+resolver
    snapshot = 'static bool surface_images_locked(' + text.split('static bool surface_images_locked(', 1)[1].split('bool pet_p4_miniapp_side_snapshot(', 1)[0]
    (tmp_path / 'esp_err.h').write_text('typedef int esp_err_t;\n')
    suite=json.loads((ROOT.parent/'pc/skills/petui/contracts/host-collection-v1.json').read_text())
    collection_surface={**suite['runtimeBase']['surface'],'collection_view':suite['config']}
    fixtures='#if PET_HOST_COLLECTION_ENABLED\nstatic const char *collection_fixture='+json.dumps(json.dumps(suite['snapshot'],ensure_ascii=True))+';\nstatic const char *collection_surface_fixture='+json.dumps(json.dumps(collection_surface,ensure_ascii=True))+';\n#endif\n'
    code = fixtures+r'''
#include "pet_p4_miniapp.h"
#include "pet_p4_host_render.h"
#include "pet_p4_app_form.h"
#include "pet_p4_form_v2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EXT_RAM_BSS_ATTR
#define MINIAPP_VAR_INT 1
typedef struct {char name[32],string_value[64];int type;int32_t int_value,int_min,int_max;} miniapp_var_t;
typedef struct {
 bool active,app_host,side_enabled,media;pet_surface_t surface;pet_form_t form;
 miniapp_var_t vars[8];int var_count,current_state,current_page,button_count;
#if PET_FORM_V2_ENABLED
 pet_form_text_config_t form_text;
#endif
 struct {char action[48],event[48];} buttons[8];
 pet_p4_miniapp_view_t view;pet_side_scene_t side;pet_side_art_t side_art;
 pet_p4_miniapp_sprite_pack_t sprites;char media_source[48];
} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
static uint32_t g_image_revision=4;static int g_pressed_node=-1;
static int32_t g_pressed_scroll[PET_SURFACE_VIEWPORTS+1];
static uint64_t g_pressed_context,g_viewport_epoch;
#if PET_HOST_COLLECTION_ENABLED
static uint64_t g_pressed_collection_epoch;
#endif
static int locks,unlocks,depth,captures;static bool replace_on_unlock;
static void miniapp_runtime_lock(void){assert(depth==0);depth=1;locks++;}
static void miniapp_runtime_unlock(void){assert(depth==1);depth=0;unlocks++;
 if(replace_on_unlock){g_runtime.surface.background=999;g_runtime.view.revision=999;g_runtime.side.score=999;
  g_runtime.sprites.pixels[0]=99;g_image_revision++;replace_on_unlock=false;}}
static uint64_t esp_timer_get_time(void){return 2500000;}
static int find_var(const miniapp_runtime_t *r,const char *name){for(int i=0;i<r->var_count;i++)if(!strcmp(name,r->vars[i].name))return i;return -1;}
static void copy_utf8(char *out,size_t n,const char *s){snprintf(out,n,"%s",s);}
static bool runtime_action_enabled(const miniapp_runtime_t *r,const char *a){(void)r;(void)a;assert(depth==1);return true;}
static void form_values(const miniapp_runtime_t *r,int32_t v[PET_FORM_VARS]){(void)r;memset(v,0,PET_FORM_VARS*sizeof(*v));}
#if PET_FORM_V2_ENABLED
static void form2_values(const miniapp_runtime_t *r,pet_form_value_t values[PET_FORM_VARS]){
 memset(values,0,PET_FORM_VARS*sizeof(*values));
 for(int i=0;i<r->var_count;i++){values[i].number=r->vars[i].int_value;memcpy(values[i].text,r->vars[i].string_value,64);}
}
#endif
static bool confirmation_locked(char prompt[121],uint64_t now){assert(depth==1&&now==2500);strcpy(prompt,"snapshot-confirm");return true;}
static void pet_app_host_render_capture(const char *id,pet_host_render_t *s,pet_p4_miniapp_view_t *view,uint64_t now){
 assert(depth==1&&!strcmp(id,"app")&&now==2500);captures++;memset(s,0,sizeof(*s));s->count=1;s->current=s->fresh=true;
 s->mode=3;s->action_count=1;strcpy(s->actions[0],"host.next");strcpy(s->variables[0].name,"value");
 s->variables[0].integer=true;s->variables[0].number=23;strcpy(s->variables[0].text,"23");
 if(view){view->revision=23;strcpy(view->headline,"host-23");}
#if PET_HOST_COLLECTION_ENABLED
 cJSON *model=cJSON_Parse(collection_fixture);assert(pet_host_collection_parse_snapshot(model,&s->collection));cJSON_Delete(model);
 s->action_count=3;strcpy(s->actions[1],"host.pick_a");strcpy(s->actions[2],"host.pick_b");
 s->collection_epoch=1;
#endif
}
void pet_p4_widget_data_view(const char *source,int32_t page,uint64_t now,pet_p4_data_view_t *out){
 (void)out;assert(depth==0&&now==2500&&page==2&&!strcmp(source,"snapshot.source"));
}
'''+resolver+snapshot+r'''
int main(void){
 static pet_p4_miniapp_render_snapshot_t s;static pet_p4_miniapp_sprite_pack_t images;
 g_runtime.active=g_runtime.app_host=g_runtime.surface.app=g_runtime.side_enabled=true;
 strcpy(g_runtime.widget_id,"app");
 return 0;
}
'''
    # The harness runtime names are native-owned and only the used production
    # fields are modeled; no parser/renderer is reimplemented.
    code = code.replace(' bool active,app_host,side_enabled,media;pet_surface_t surface;', ' char widget_id[48];bool active,app_host,side_enabled,media;pet_surface_t surface;')
    code = code.replace(' strcpy(g_runtime.widget_id,"app");\n return 0;', r'''
 strcpy(g_runtime.widget_id,"app");strcpy(g_runtime.view.widget_id,"app");g_runtime.view.active=true;
 strcpy(g_runtime.view.data_source,"snapshot.source");g_runtime.view.data_page=2;
 g_runtime.surface.background=77;g_runtime.side.score=88;g_runtime.sprites.count=1;g_runtime.sprites.data_length=1;g_runtime.sprites.pixels[0]=42;
 g_runtime.var_count=1;strcpy(g_runtime.vars[0].name,"value");g_runtime.vars[0].type=MINIAPP_VAR_INT;g_runtime.vars[0].int_min=0;g_runtime.vars[0].int_max=100;
 g_runtime.surface.count=2;pet_surface_node_t *n=&g_runtime.surface.nodes[0];strcpy(n->var_bind,"value");strcpy(n->action,"host.next");n->host_mask=1u<<3;
#if PET_HOST_COLLECTION_ENABLED
 cJSON *definition=cJSON_Parse(collection_surface_fixture);assert(pet_surface_parse(definition,&g_runtime.surface));cJSON_Delete(definition);
 g_runtime.surface.background=77;
 n=&g_runtime.surface.nodes[0];strcpy(n->var_bind,"value");strcpy(n->action,"host.next");n->host_mask=1u<<3;
#endif
 n=&g_runtime.surface.nodes[1];strcpy(n->tile_var,"value");n->tile_cols=4;strcpy(n->show_var,"value");n->show_min=20;n->show_max=30;
 g_runtime.surface.viewport.enabled=true;g_runtime.surface.viewport.h=100;g_runtime.surface.viewport.content_height=200;strcpy(g_runtime.surface.viewport.offset_var,"value");
#if PET_CANVAS_ENABLED
 g_runtime.surface.canvas.variable_count=1;strcpy(g_runtime.surface.canvas.names[0],"value");
#endif
 uint32_t revision=0;replace_on_unlock=true;
 assert(pet_p4_miniapp_render_snapshot(&s,&images,&revision));assert(locks==1&&unlocks==1&&captures==1);
 assert(s.surface_active&&s.side_active&&s.images_changed&&s.image_revision==4&&revision==4);
 assert(s.surface.background==77&&s.scene.score==88&&images.pixels[0]==42&&images.data_length==1);
 assert(s.view.revision==23&&!strcmp(s.view.headline,"host-23")&&s.confirmation_active&&!strcmp(s.confirmation_prompt,"snapshot-confirm"));
 assert(!strcmp(s.surface.nodes[0].text,"23")&&s.surface.nodes[0].bound_value==23&&!s.surface.nodes[0].disabled&&!s.surface.nodes[0].hidden);
 assert(!s.surface.nodes[1].hidden&&s.surface.nodes[1].tile_index==3&&s.surface.viewport.offset==23);
#if PET_HOST_COLLECTION_ENABLED
 assert(s.surface.count==8&&!s.surface.nodes[2].hidden&&!s.surface.nodes[2].disabled);
 assert(!strcmp(s.surface.nodes[3].text,"第一项")&&!strcmp(s.surface.nodes[6].text,"第二项"));
 assert(s.surface.nodes[5].color==65504); // selected style uses the same captured reply
#endif
#if PET_CANVAS_ENABLED
 assert(s.surface.canvas_bindings_ready&&s.surface.canvas.values[0]==23);
#endif
 /* A same-id replacement is copied once, not mistaken for the same lifetime. */
 assert(pet_p4_miniapp_render_snapshot(&s,&images,&revision));assert(s.images_changed&&s.image_revision==5&&s.surface.background==999&&s.scene.score==999&&images.pixels[0]==99);
 images.pixels[0]=17;assert(pet_p4_miniapp_render_snapshot(&s,&images,&revision));assert(!s.images_changed&&images.pixels[0]==17); /* unchanged image bytes are not copied */
 g_runtime.active=false;images.pixels[0]=13;assert(pet_p4_miniapp_render_snapshot(&s,&images,&revision));assert(!s.surface_active&&!s.side_active&&!s.images_changed&&images.pixels[0]==13);
 assert(!pet_p4_miniapp_render_snapshot(NULL,&images,&revision)&&!pet_p4_miniapp_render_snapshot(&s,NULL,&revision)&&!pet_p4_miniapp_render_snapshot(&s,&images,NULL));
 assert(locks==4&&unlocks==4&&depth==0);return 0;''')
    if phase_trace:
        old='static uint64_t esp_timer_get_time(void){return 2500000;}'
        assert code.count(old)==1
        code=code.replace(old,'static uint64_t esp_timer_get_time(void){static unsigned n;const uint64_t stamps[]={2500000,2500123,2500200,2500500};return stamps[n++%4];}')
        code=code.replace('assert(locks==4&&unlocks==4&&depth==0);return 0;', 'assert(locks==4&&unlocks==4&&depth==0);assert(s.capture_wait_us==123&&s.capture_total_us==500);return 0;')
    compile_run(tmp_path, code, (ROOT / 'main/pet_p4_surface.c', ROOT / 'main/pet_p4_canvas.c', ROOT / 'main/pet_p4_form_v2.c',
                ROOT/'main/pet_p4_host_collection.c',ROOT/'main/pet_p4_collection.c'),
                (f'-DPET_CANVAS_ENABLED={canvas}', f'-DPET_FORM_V2_ENABLED={form2}',f'-DPET_HOST_COLLECTION_ENABLED={collection}',f'-DPET_P4_APP_PHASE_TRACE={phase_trace}'))


def test_actual_application_capture_measures_wait_and_total_without_extra_lock_or_copy(tmp_path):
    test_app_frame_uses_one_publication_and_no_live_host_reads_after_capture(tmp_path,1,1,1,phase_trace=1)


def test_renderer_never_refetches_app_or_media_surface_or_side_after_capture():
    text = (ROOT / 'main/pet_p4_renderer.c').read_text()
    assert 'pet_p4_miniapp_render_snapshot(&g_miniapp_render_snapshot,&g_surface_images,&g_surface_revision)' in text
    for name in ('pet_p4_miniapp_app_surface(', 'pet_p4_miniapp_get_surface_images(',
                 'pet_p4_miniapp_side_snapshot(', 'pet_p4_miniapp_media_config(', 'pet_p4_miniapp_confirmation('):
        assert name not in text
    source = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    resolver = source.split('static bool app_surface_locked(', 1)[1].split('bool pet_p4_miniapp_media_config(', 1)[0]
    # The compatibility wrapper captures once too; no per-node live host reads.
    assert resolver.count('pet_app_host_render_capture(') == 1
    for name in ('pet_app_host_variable(', 'pet_app_host_enabled(', 'pet_app_host_mode(', 'pet_app_host_integer_snapshot('):
        assert name not in resolver
