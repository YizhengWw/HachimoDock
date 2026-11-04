"""Compile production merge boundaries with draw/audio spies; no board access."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^(?:static )?[^\n]*\b" + name + r"\(", source, re.M)
    assert match, name
    return source[match.start():source.index("\n}", match.start()) + 2]


def compile_run(tmp_path, source, *extra):
    path = tmp_path / "test.c"
    path.write_text(source)
    binary = tmp_path / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "main"),
                    "-I", str(ROOT / "tests/vendor/cjson"),
                    str(path), *map(str, extra), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_approval_uses_local_text_without_host_bubble_pin(tmp_path):
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    # Use the actual six-argument prototype, rather than duplicating the old
    # signature in a stub (which would conceal the automatic-merge error).
    signature = function(source, "draw_card_body_lines").split("{")[0]
    prelude = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "pet_p4_protocol.h"
#include "pet_p4_bubble_pixels.h"
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static int draws;
static bool pet_p4_state_approval_pending_stub(const pet_p4_runtime_state_t *s, unsigned long long t) {(void)t;return s->approval_active;}
#define pet_p4_state_approval_pending pet_p4_state_approval_pending_stub
static uint16_t rgb565(int r,int g,int b) {return (uint16_t)(r+g+b);}
static void fill_round_rect(int x,int y,int w,int h,int r,uint16_t c) {(void)x;(void)y;(void)w;(void)h;(void)r;(void)c;}
static void fill_round_rect_outline(int x,int y,int w,int h,int r,uint16_t a,uint16_t b) {(void)a;fill_round_rect(x,y,w,h,r,b);}
static void blend_rect(int x,int y,int w,int h,int r,int g,int b,int a) {(void)x;(void)y;(void)w;(void)h;(void)r;(void)g;(void)b;(void)a;}
static void fill_rect(int x,int y,int w,int h,uint16_t c) {fill_round_rect(x,y,w,h,0,c);}
static void draw_text_line(const char *t,int x,int y,int w,uint16_t c,int s,bool e) {(void)t;(void)x;(void)y;(void)w;(void)c;(void)s;(void)e;}
static void draw_text_line_vcenter(const char *t,int x,int y,int h,int w,uint16_t c,int s,bool e) {(void)h;draw_text_line(t,x,y,w,c,s,e);}
static void draw_text_right(const char *t,int x,int y,int w,uint16_t c,int s) {draw_text_line(t,x,y,w,c,s,false);}
static void draw_text_center(const char *t,int x,int y,int w,uint16_t c,int s) {draw_text_line(t,x,y,w,c,s,false);}
static int text_y_in_box(const char *t,int s,int y,int h) {(void)t;(void)s;(void)h;return y;}
'''
    spy = signature + r'''{
  (void)x;(void)first_line_y;(void)max_px;(void)color;
  assert(body && body[0]); assert(host_pixels == NULL); draws++;
}
'''
    main = r'''
int main(void) {
  pet_p4_runtime_state_t state={0};
  draw_approval_overlay(&state,1000); assert(draws==0);
  state.approval_active=true;
  draw_approval_overlay(&state,1000); assert(draws==1);
  snprintf(state.approval_reason,sizeof(state.approval_reason),"local approval");
  state.approval_expires_ms=2000;
  draw_approval_overlay(&state,1000); assert(draws==2);
}
'''
    constants = "\n".join(line for line in source.splitlines()
                          if line.startswith("#define PET_P4_APPROVAL_CARD_")) + "\n"
    compile_run(tmp_path, prelude + constants + spy + function(source, "draw_approval_button")
                + function(source, "draw_approval_overlay") + main)


def test_stopped_cue_only_after_admission_and_once_per_revision(tmp_path):
    source = (ROOT / "main/pet_p4_protocol.c").read_text()
    names = ["copy_text", "lifecycle_is", "lifecycle_is_active", "lifecycle_is_terminal",
             "session_ids_match", "find_session_queue_item", "json_string", "json_u64",
             "json_bounded_u64", "json_session_id_array_contains"]
    prelude = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_protocol.h"
#define PET_P4_JSON_SAFE_INTEGER_MAX 9007199254740991ULL
#define PET_P4_SESSION_NOTICE_MS 2200ULL
static int cues;
static long long now_us=1000000;
static long long esp_timer_get_time(void) {return now_us;}
static int pet_p4_audio_play_event_cue(const char *name) {assert(!strcmp(name,"session_terminated"));return ++cues;}
static void restore_idle_session_view(pet_p4_runtime_state_t *s,const char *event) {(void)s;(void)event;}
static char g_last_stopped_cue_session[PET_P4_SESSION_QUEUE_ID_MAX];
static unsigned long long g_last_stopped_cue_revision;
'''
    main = r'''
static void send(pet_p4_runtime_state_t *s,const char *body) {
  cJSON *j=cJSON_Parse(body); assert(j); handle_session_current(s,j); cJSON_Delete(j);
}
int main(void) {
  pet_p4_runtime_state_t s={0};
  const char *stop="{\"agentId\":\"dsh\",\"sessionId\":\"a\",\"sessions\":[{\"id\":\"a\",\"title\":\"A\",\"state\":\"stopped\",\"transitionRevision\":2,\"terminalRemainingMs\":1000}]}";
  /* Historical/expired rows and legacy rows without a previous card are silent. */
  send(&s,"{\"agentId\":\"dsh\",\"sessions\":[{\"id\":\"a\",\"title\":\"A\",\"state\":\"stopped\",\"transitionRevision\":2}]}");
  assert(cues==0 && s.session_queue_count==0);
  send(&s,"{\"agentId\":\"dsh\",\"sessions\":[{\"id\":\"a\",\"title\":\"A\",\"state\":\"stopped\"}]}");
  assert(cues==0 && s.session_queue_count==0);
  send(&s,stop); assert(cues==1 && s.session_queue_count==1);
  send(&s,stop); assert(cues==1);
  send(&s,"{\"agentId\":\"dsh\",\"sessionId\":\"a\",\"sessions\":[{\"id\":\"a\",\"title\":\"A\",\"state\":\"working\",\"transitionRevision\":3}]}");
  send(&s,stop); assert(cues==1 && !strcmp(s.session_queue[0].state,"working"));
  send(&s,"{\"agentId\":\"dsh\",\"sessionId\":\"a\",\"sessions\":[{\"id\":\"a\",\"title\":\"A\",\"state\":\"stopped\",\"transitionRevision\":4,\"terminalRemainingMs\":1000}]}");
  assert(cues==2 && !strcmp(s.session_queue[0].state,"stopped"));
  /* A reconnect/cold snapshot of an already announced revision stays silent. */
  memset(&s,0,sizeof(s)); send(&s,stop); assert(cues==3);
  memset(&s,0,sizeof(s)); send(&s,stop); assert(cues==3);
}
'''
    compile_run(tmp_path, prelude + "\n".join(function(source,n) for n in names)
                + function(source, "handle_session_current") + main,
                ROOT / "tests/vendor/cjson/cJSON.c")


def test_approval_cancel_releases_parsed_message_even_on_stale_id(tmp_path):
    source = (ROOT / "main/pet_p4_protocol.c").read_text()
    begin = source.index('  } else if (strcmp(topic, "approval/cancel") == 0)')
    end = source.index('  } else if (strncmp(topic, "state/", 6) == 0)', begin)
    branch = source[begin:end]
    handler = function(source, "pet_p4_handle_line_with_reply")
    cleanup = handler[handler.rindex("  cJSON_Delete(root);"):]
    prelude = r'''
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_protocol.h"
static int allocated, cues;
static void *allocate(size_t size) {void *p=malloc(size);if(p)allocated++;return p;}
static void release(void *p) {if(p)allocated--;free(p);}
static int pet_p4_audio_play_event_cue(const char *name) {(void)name;return ++cues;}
'''
    main = r'''
int main(void) {
  cJSON_Hooks hooks={allocate,release};cJSON_InitHooks(&hooks);
  pet_p4_runtime_state_t s={0};s.approval_active=true;
  strcpy(s.approval_request_id,"request-a");
  assert(dispatch(&s,"{\"requestId\":\"stale\",\"outcome\":\"cancelled\"}"));
  assert(allocated==0 && cues==0 && s.approval_active);
  assert(dispatch(&s,"{\"requestId\":\"request-a\",\"outcome\":\"cancelled\"}"));
  assert(allocated==0 && cues==1 && !s.approval_active);
}
'''
    dispatch = r'''
static bool dispatch(pet_p4_runtime_state_t *state,const char *line) {
  cJSON *root=cJSON_Parse(line);assert(root);
  cJSON *payload=root;
  const char *topic="approval/cancel";
  if (false) {
'''
    compile_run(tmp_path, prelude + function(source, "json_string")
                + function(source, "pet_p4_state_clear_approval").replace("static ", "", 1)
                + dispatch + branch + "\n  }\n" + cleanup + main,
                ROOT / "tests/vendor/cjson/cJSON.c")


def test_approval_invalidates_actual_private_final_frame_retention(tmp_path):
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    begin = source.index("  const bool approval_visible =")
    end = source.index('  if (strcmp(page, "components") == 0)', begin)
    gate = source[begin:end]
    begin = source.index("  pet_logical_retention_finish(", end)
    finish = source[begin:source.index(");", begin) + 2]
    harness = r'''
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "pet_p4_logical_retention.h"
#include "pet_p4_compact_underlay.h"
#define PET_P4_APP_RETAIN_LOGICAL 1
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
#define ESP_OK 0
typedef struct {bool active;} state_t;
static bool pet_p4_state_approval_pending(const state_t *s,unsigned long long now) {(void)now;return s->active;}
static unsigned clears;
static uint16_t rgb565(int r,int g,int b) {return (uint16_t)(r+g+b);}
static void fill_rect(int x,int y,int w,int h,uint16_t c) {(void)x;(void)y;(void)w;(void)h;(void)c;clears++;}
static bool g_last_approval_visible,g_app_may_retain,g_main_appearance_ready=true;
static bool g_session_overlay_cache_valid=true,g_app_final_eligible=true;
static pet_logical_retention_t g_app_final_retention;
static pet_compact_underlay_t g_main_compact_underlay={.valid=true,.painted=true};
static pet_logical_common_plan_t g_app_common_plan;
static void frame(bool active,bool expected_reuse,bool expected_publish) {
  state_t actual={active};const state_t *state=&actual;
  const unsigned long long now_ms=1000;
  const char *page="app";int lcd_err=ESP_OK;
  g_app_may_retain=pet_logical_retention_begin(&g_app_final_retention,g_app_common_plan,true,true,true);
'''
    main = r'''
  assert(g_app_may_retain==expected_reuse);
''' + finish + r'''
  assert(g_app_final_retention.valid==expected_publish);
}
int main(void) {
  frame(false,false,true);frame(false,true,true);
  frame(true,false,false);assert(clears==1 && !g_main_appearance_ready && !g_session_overlay_cache_valid);
  assert(!g_main_compact_underlay.valid && !g_main_compact_underlay.painted);
  frame(true,false,false);assert(clears==1);
  g_main_compact_underlay.valid=true;g_main_compact_underlay.painted=true;
  frame(false,false,true);assert(clears==2);
  assert(!g_main_compact_underlay.valid && !g_main_compact_underlay.painted);
  frame(false,true,true);
}
'''
    compile_run(tmp_path, harness + gate + main)
