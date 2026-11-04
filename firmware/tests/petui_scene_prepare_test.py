"""Execute the production scene preparation lifetime, including allocation failure."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_scene_scratch_is_bounded_and_freed_on_every_prepare_exit(tmp_path):
    code = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    cleanup = code[code.index('static void free_bounded_scene_config('):code.index('static bool parse_bounded_scene(')]
    parser = code[code.rindex('static bool parse_bounded_scene('):code.index('static bool load_legacy_catalog(')]
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_game.h"
#include "pet_p4_bodies_json.h"
enum {MINIAPP_VAR_INT};
typedef struct {int from_state,to_state,effect_count,page_op;} miniapp_transition_t;
typedef struct {int count;struct {int frames;} items[4];} sprites_t;
typedef struct {
 uint8_t var_count,tick_count,page_count,state_count,current_state,transition_count,button_count;
 bool media,instrument,slicing;char data_source[48],states[4][24],widget_id[48],hold_action[48];
 struct {int type;} vars[4];miniapp_transition_t transitions[4];sprites_t sprites;int fruit;
 struct {int charge_var,score_var,playing_state,result_state;bool active;pet_p4_game_engine_t engine;} game;
} miniapp_runtime_t;
static unsigned allocations,releases,live,phase;
static int fail_phase;
static bool oom,bad_engine;
static void *scratch_calloc(size_t n,size_t bytes){assert(n==1&&bytes==sizeof(pet_p4_bounded_game_config_t));allocations++;if(oom)return NULL;void *p=calloc(n,bytes);if(p)live++;return p;}
static void scratch_free(void *p){if(p){assert(live==1);live--;releases++;free(p);}}
static void set_error(char *out,size_t n,const char *s){snprintf(out,n,"%s",s);}
static const char *json_string(const cJSON *o,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:"";}
static bool object_keys_allowed(const cJSON *o,const char *const *keys,size_t count){if(!cJSON_IsObject(o))return false;const cJSON *v;cJSON_ArrayForEach(v,o){bool found=false;for(size_t i=0;i<count;i++)if(!strcmp(v->string,keys[i]))found=true;if(!found)return false;}return true;}
static bool json_int_range(const cJSON *v,int low,int high,int *out){if(!cJSON_IsNumber(v)||v->valuedouble!=v->valueint||v->valueint<low||v->valueint>high)return false;if(out)*out=v->valueint;return true;}
static int find_name(char states[][24],int count,const char *name){for(int i=0;i<count;i++)if(!strcmp(states[i],name))return i;return -1;}
static int find_var(miniapp_runtime_t *r,const char *name){(void)r;return !strcmp(name,"score")?0:!strcmp(name,"charge")?1:-1;}
static bool runtime_has_transition_action(miniapp_runtime_t *r,const char *s){(void)r;(void)s;return true;}
static bool runtime_has_button_action(miniapp_runtime_t *r,const char *s){(void)r;(void)s;return true;}
static void pet_fruit_init(int *fruit,int seconds,int lives,int volume){*fruit=seconds+lives+volume;}
static uint32_t stable_widget_seed(const char *id){(void)id;return 123;}
static int bounded_find_entity(const pet_p4_bounded_game_config_t *c,const char *id){for(int i=0;i<c->entity_count;i++)if(!strcmp(c->entities[i].id,id))return i;return -1;}
/* Isolate scratch ownership at the real parser's phase boundaries; the bounded
 * engine below is the actual production implementation, not a mock result. */
static bool parse_bounded_sprites(miniapp_runtime_t *r,const cJSON *v,bool v4,char *e,size_t n){(void)r;(void)v;(void)v4;(void)e;(void)n;return ++phase!=(unsigned)fail_phase;}
static bool parse_bounded_grid(pet_p4_bounded_game_config_t *c,const cJSON *v,char *e,size_t n){(void)v;(void)e;(void)n;c->width=bad_engine?0:8;c->height=8;return ++phase!=(unsigned)fail_phase;}
static bool parse_bounded_entities(pet_p4_bounded_game_config_t *c,sprites_t *s,const cJSON *v,char *e,size_t n){(void)s;(void)v;(void)e;(void)n;c->entity_count=1;c->entities[0]=(pet_p4_game_entity_t){.id="hero",.width=1,.height=1,.tone=1,.sprite_index=-1,.active=true};return ++phase!=(unsigned)fail_phase;}
static bool parse_bounded_rules(miniapp_runtime_t *r,pet_p4_bounded_game_config_t *c,const cJSON *v,char *e,size_t n){(void)r;(void)v;(void)e;(void)n;c->rule_count=1;c->rules[0]=(pet_p4_game_rule_t){.trigger=PET_P4_GAME_TRIGGER_ACTION,.action="go",.op_count=1};return ++phase!=(unsigned)fail_phase;}
#define calloc scratch_calloc
#define free scratch_free
''' + cleanup + parser + r'''
#undef calloc
#undef free
static miniapp_runtime_t runtime;
static void reset(void){memset(&runtime,0,sizeof(runtime));runtime.state_count=1;strcpy(runtime.states[0],"play");strcpy(runtime.widget_id,"test");runtime.game.engine.rng=456;phase=0;}
static bool run(const char *text){reset();cJSON *s=cJSON_Parse(text);assert(s);char error[128]={0};bool ok=parse_bounded_scene(&runtime,s,true,error,sizeof(error));cJSON_Delete(s);assert(live==0);if(!ok)assert(runtime.game.engine.rng==456&&!runtime.game.active);return ok;}
int main(void){
 const char *valid="{\"active_state\":\"play\",\"tick_ms\":100,\"auto_start\":true}";
 char error[128]={0};reset();assert(parse_bounded_scene(&runtime,NULL,true,error,sizeof(error)));assert(allocations==0);
 assert(!run("{\"unsupported\":1}"));assert(allocations==0);
 oom=true;assert(!run(valid));assert(allocations==1&&releases==0);oom=false;
 assert(run(valid));assert(allocations==2&&releases==1&&runtime.game.active&&runtime.game.engine.running&&runtime.game.engine.rng==123);
 for(fail_phase=1;fail_phase<=4;fail_phase++){unsigned freed=releases;assert(!run(valid));assert(releases==freed+1);}
 fail_phase=0;
 const char *invalid[]={"{\"active_state\":\"missing\",\"tick_ms\":100}",
 "{\"active_state\":\"play\",\"score_var\":\"missing\",\"tick_ms\":100}",
 "{\"active_state\":\"play\",\"result_state\":\"missing\",\"tick_ms\":100}",
 "{\"active_state\":\"play\",\"tick_ms\":0}",
 "{\"active_state\":\"play\",\"tick_ms\":100,\"auto_start\":12}",
 "{\"active_state\":\"play\",\"tick_ms\":20,\"motion\":{}}"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){unsigned freed=releases;assert(!run(invalid[i]));assert(releases==freed+1);}
 bad_engine=true;assert(!run(valid));bad_engine=false;assert(run(valid));
 assert(allocations==releases+1&&live==0);return 0;
}
'''
    src = tmp_path / 'scene.c'
    src.write_text(source)
    obj = tmp_path / 'scene.o'
    exe = tmp_path / 'scene'
    vendor = ROOT / 'tests/vendor/cjson'
    common = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1',
              '-fsanitize=address,undefined', '-I', str(ROOT / 'main'), '-I', str(vendor)]
    subprocess.run(common + ['-Wframe-larger-than=1024', '-c', str(src), '-o', str(obj)], check=True)
    subprocess.run(common + ['-Wno-error=deprecated-declarations', str(obj), str(ROOT / 'main/pet_p4_game.c'),
                             str(vendor / 'cJSON.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
