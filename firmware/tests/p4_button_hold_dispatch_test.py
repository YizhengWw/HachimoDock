"""Exercise production debounce -> queue ordering -> SW1 dispatch, not UI actions."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_physical_hold_owns_depression_without_legacy_ptt_or_release_replay(tmp_path):
    source = (ROOT / "main/pet_p4_input.c").read_text()
    controls = source[source.index("typedef enum {\n  PET_P4_INPUT_CONTROL_SW1"):]
    controls = controls[:controls.index("typedef struct {\n  gpio_num_t")]
    priority = source[source.index("static const pet_p4_input_binding_t *active_global_priority_binding("):]
    priority = priority[:priority.index("static bool dispatch_component_binding_event(")]
    process = source[source.index("static void process_button_event("):]
    process = process[:process.index("void pet_p4_input_process(")]
    queue = source[source.index("static void queue_button_flags("):]
    queue = queue[:queue.index("static bool read_joystick_axes(")]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_input_core.h"
#define PET_P4_INPUT_EVENT_MAX 64
typedef struct { char screen_page[24]; } pet_p4_runtime_state_t;
typedef struct { char action[40]; } pet_p4_input_binding_t;
typedef void (*pet_p4_send_line_fn)(const char *,void *);
''' + controls + r'''
static pet_p4_runtime_state_t state={"app"};
static pet_p4_input_binding_t hold={"voice_ptt"}, short_binding={"disabled"}, long_binding={"disabled"};
static bool conversation, motion=true, press, accept=true;
static int starts, releases, cancels, dispatched, voice_events;
static unsigned long long now;
static const char *button_event_prefix(pet_p4_input_control_t c){return c==PET_P4_INPUT_CONTROL_SW1?"button.sw1":"button.sw2";}
static const pet_p4_input_binding_t *active_binding(const char *e){
  if(!strcmp(e,"button.sw1.hold"))return &hold;
  if(!strcmp(e,"button.sw1.short_press"))return &short_binding;
  if(!strcmp(e,"button.sw1.long_press"))return &long_binding;
  return NULL;
}
static bool pet_p4_conversation_active(const pet_p4_runtime_state_t *s){(void)s;return conversation;}
static bool pet_p4_miniapp_hold_input(const char *e){return motion&&!strcmp(e,"button.sw1.short_press");}
static bool pet_p4_miniapp_input_policy(const char *e,bool down){(void)e;(void)down;return press;}
static bool pet_p4_miniapp_has_input(const char *e){(void)e;return false;}
static bool pet_p4_miniapp_resolve_input(const char *e,char *a,size_t n){(void)e;snprintf(a,n,"jump.charge");return true;}
static bool pet_p4_miniapp_hold_event(const char *a,int phase,unsigned owner,unsigned long long t){
  (void)a;(void)owner;(void)t;
  if(!phase){if(accept)starts++;return accept;}
  if(phase==1)releases++;else cancels++;
  return true;
}
static void dispatch_binding_event(pet_p4_runtime_state_t *s,pet_p4_send_line_fn f,void *c,const pet_p4_input_event_t *e,const char *n,const char *g){
  (void)s;(void)f;(void)c;(void)e;(void)g;dispatched++;
  const pet_p4_input_binding_t *b=active_binding(n);
  if(b&&!strcmp(b->action,"voice_ptt"))voice_events++;
}
''' + priority + process + r'''
static void queue_event(pet_p4_input_control_t c,pet_p4_input_gesture_t g,int d){
  pet_p4_input_event_t e={c,g,d,now};process_button_event(&state,NULL,NULL,&e);
}
''' + queue + r'''
static void samples(pet_p4_button_decoder_t *d,bool down,int ms){
  for(int i=0;i<ms;i+=5){now+=5;queue_button_flags(PET_P4_INPUT_CONTROL_SW1,pet_p4_button_decoder_update(d,down,5));}
}
static void stroke(int ms){
  pet_p4_button_decoder_t d;pet_p4_button_decoder_init(&d,false,25,700);
  samples(&d,true,ms);samples(&d,false,50);
}
static void reset(void){starts=releases=cancels=dispatched=voice_events=0;}
int main(void){
  // Default persisted legacy PTT must not block an app's explicit hold action.
  stroke(500);assert(starts==1&&releases==1&&dispatched==0&&voice_events==0);
  reset();stroke(1400);assert(starts==1&&releases==1&&dispatched==0&&voice_events==0);
  // An airborne/rejected charge must consume this depression, not charge on release.
  reset();accept=false;stroke(500);assert(starts==0&&releases==0&&dispatched==0);accept=true;
  // Rapid repeated presses each have exactly one complete lifecycle.
  reset();for(int i=0;i<12;i++)stroke(60);assert(starts==12&&releases==12&&dispatched==0);
  // Navigation while held cancels rather than launching into another page.
  reset();pet_p4_button_decoder_t d;pet_p4_button_decoder_init(&d,false,25,700);
  samples(&d,true,500);strcpy(state.screen_page,"main");samples(&d,false,50);
  assert(starts==1&&cancels==1&&releases==0);strcpy(state.screen_page,"app");
  // Explicit global exit/realtime overrides continue to win.
  reset();strcpy(short_binding.action,"page_back");stroke(500);assert(starts==0&&dispatched==1);strcpy(short_binding.action,"disabled");
  reset();strcpy(long_binding.action,"realtime_chat");stroke(900);assert(starts==0&&dispatched>0);strcpy(long_binding.action,"disabled");
  reset();strcpy(hold.action,"realtime_chat");stroke(900);assert(starts==0&&dispatched>0);
  // Other explicit hold bindings are not silently stolen.
  reset();strcpy(hold.action,"custom_action");stroke(900);assert(starts==0&&dispatched>0);
  // The legacy PTT still works on the pet page and during conversations.
  strcpy(hold.action,"voice_ptt");strcpy(state.screen_page,"main");reset();stroke(900);assert(starts==0&&voice_events==2);
  strcpy(state.screen_page,"app");conversation=true;reset();stroke(900);assert(starts==0&&voice_events==2);conversation=false;
  // Existing immediate-press policy must still respect hold bindings.
  motion=false;press=true;reset();stroke(500);assert(starts==0&&dispatched==1);
  strcpy(hold.action,"disabled");reset();stroke(500);assert(dispatched==1&&starts==0);
  motion=true;press=false;reset();stroke(500);assert(starts==1&&releases==1&&dispatched==0);
}
'''
    file = tmp_path / "button-hold.c"
    file.write_text(harness)
    binary = tmp_path / "button-hold"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"), str(file), str(ROOT / "main/pet_p4_input_core.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
