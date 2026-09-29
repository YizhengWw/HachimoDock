"""Execute production input dispatch with platform stubs; no physical-key claims."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def test_global_realtime_remapping_wins_over_components(tmp_path):
    source = (ROOT / "main/pet_p4_input.c").read_text()
    def function(name):
        start = source.index(name)
        return source[start:source.index("\n}\n", start) + 3]
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PET_P4_MINIAPP_ACTION_MAX 64
#define ESP_OK 0
typedef struct {char event[64],action[64],value[64];} pet_p4_input_binding_t;
typedef struct {char screen_page[24];bool session_voice_active;int session_queue_count;uint64_t last_update_ms;} pet_p4_runtime_state_t;
typedef struct {uint64_t ts_ms;} pet_p4_input_event_t;
typedef void (*pet_p4_send_line_fn)(const char*,void*);
static pet_p4_input_binding_t configured;
static bool conversation, package_claims;
static int package_calls, moved, emitted;
static char emitted_action[64];
static void copy_text(char *out,size_t n,const char *in){snprintf(out,n,"%s",in);}
static const pet_p4_input_binding_t *active_binding(const char *event){return !strcmp(event,configured.event)?&configured:NULL;}
static bool pet_p4_conversation_active(const pet_p4_runtime_state_t *s){(void)s;return conversation;}
static void pet_p4_conversation_move(pet_p4_runtime_state_t *s,int d,uint64_t t){(void)s;(void)t;moved+=d;}
static bool apply_local_action(pet_p4_runtime_state_t *s,const pet_p4_input_binding_t *b,uint64_t t,char *a,size_t n){(void)s;(void)b;(void)t;(void)a;(void)n;return false;}
static void send_input_event(pet_p4_runtime_state_t *s,pet_p4_send_line_fn f,void *c,const pet_p4_input_event_t *e,const char *n,const char *g,const pet_p4_input_binding_t *b,const char *a,bool h){(void)s;(void)f;(void)c;(void)e;(void)n;(void)g;(void)a;(void)h;emitted++;copy_text(emitted_action,sizeof(emitted_action),b?b->action:"");}
static void send_ignored_component_event(pet_p4_runtime_state_t *s,pet_p4_send_line_fn f,void *c,const pet_p4_input_event_t *e,const char *n,const char *g){(void)s;(void)f;(void)c;(void)e;(void)n;(void)g;}
static bool dispatch_component_binding_event(pet_p4_runtime_state_t *s,pet_p4_send_line_fn f,void *c,const pet_p4_input_event_t *e,const char *n,const char *g){(void)s;(void)f;(void)c;(void)e;(void)n;(void)g;package_calls++;return package_claims;}
static int pet_p4_audio_capture_start(bool b){(void)b;return 0;}
static int pet_p4_audio_capture_stop(void){return 0;}
static void pet_p4_media_stop_background(pet_p4_send_line_fn f,void *ctx){(void)f;(void)ctx;assert(false);}
''' + function("static bool component_system_action(") + function("static const pet_p4_input_binding_t *active_global_priority_binding(") + function("static void dispatch_binding_event(") + r'''
int main(void){
  pet_p4_runtime_state_t state={.screen_page="app"};pet_p4_input_event_t event={0};
  package_claims=true;
  const char *keys[]={"button.sw2.long_press","button.sw1.long_press","button.sw1.short_press","joystick.up","knob.rotate_cw"};
  for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++) {
    copy_text(configured.event,sizeof(configured.event),keys[i]);copy_text(configured.action,sizeof(configured.action),"realtime_chat");
    package_calls=emitted=moved=0;conversation=false;
    dispatch_binding_event(&state,NULL,NULL,&event,keys[i],"long_press");
    assert(!package_calls&&emitted==1&&!strcmp(emitted_action,"realtime_chat"));
    conversation=true;dispatch_binding_event(&state,NULL,NULL,&event,keys[i],"long_press");
    assert(emitted==2&&!moved); // Remapped directions also exit, not scroll.
  }
  conversation=false;copy_text(configured.action,sizeof(configured.action),"disabled");
  package_calls=0;dispatch_binding_event(&state,NULL,NULL,&event,configured.event,"short_press");assert(package_calls==1);
  copy_text(configured.event,sizeof(configured.event),"button.sw1.long_press");
  copy_text(configured.action,sizeof(configured.action),"page_back");package_calls=0;
  dispatch_binding_event(&state,NULL,NULL,&event,configured.event,"long_press");assert(!package_calls&&!strcmp(emitted_action,"page_back"));
  copy_text(configured.action,sizeof(configured.action),"realtime_chat");copy_text(state.screen_page,sizeof(state.screen_page),"components");
  assert(active_global_priority_binding(&state,configured.event)==&configured);
  assert(!active_global_priority_binding(NULL,configured.event));
}
'''
    harness = tmp_path / "input.c"; harness.write_text(code)
    exe = tmp_path / "input"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
