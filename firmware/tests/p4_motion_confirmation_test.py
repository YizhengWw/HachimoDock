"""Compile production modal clock and resume selection branches on the host."""
from pathlib import Path
import subprocess


def test_motion_modal_clock_and_restart_is_not_resume(tmp_path):
    source = (Path(__file__).resolve().parents[1] / "main/pet_p4_miniapp.c").read_text()
    resume = source[source.index("    bool resuming=false;"):]
    resume = resume[:resume.index("    if (transition->to_state >= 0")]
    clock = source[source.index("  bool motion_confirm="):]
    clock = clock[:clock.index("  if (changed) refresh_view")]
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct {char action[48];int from_state,to_state;} miniapp_transition_t;
typedef struct {struct {bool motion;}bounded;uint64_t last_tick_ms;} engine_t;
static struct {struct {bool active;int playing_state;engine_t engine;}game;
 int current_state,current_page,transition_count;char pause_action[48];miniapp_transition_t transitions[2];}g_runtime;
static char g_confirm_action[48];static uint64_t g_confirm_until,g_confirm_context;
static unsigned g_image_revision=1,steps;
static bool pet_p4_game_process(engine_t *e,uint64_t now){assert(now-e->last_tick_ms<=20);e->last_tick_ms=now;steps++;return true;}
static bool sync_game_runtime(void *p){(void)p;return false;}
static bool resume(const char *action,const miniapp_transition_t *transition){
''' + resume + r'''
 return resuming;
}
static void tick(uint64_t now_ms){bool visible=true,changed=false;
''' + clock + r'''
 (void)changed;
}
int main(void){
 g_runtime.game.active=true;g_runtime.game.playing_state=1;g_runtime.current_state=2;
 strcpy(g_runtime.pause_action,"pause");g_runtime.transition_count=1;
 g_runtime.transitions[0]=(miniapp_transition_t){.action="pause",.from_state=2,.to_state=1};
 miniapp_transition_t restart={.action="restart",.from_state=-1,.to_state=1};
 assert(resume("pause",&g_runtime.transitions[0]));assert(!resume("restart",&restart));
 g_runtime.current_state=1;assert(!resume("pause",&g_runtime.transitions[0]));
 g_runtime.game.engine.bounded.motion=true;strcpy(g_confirm_action,"restart");
 g_confirm_until=30000;g_confirm_context=(1u<<16)|(1u<<8);
 for(unsigned t=20;t<30000;t+=20)tick(t);
 assert(steps==0);assert(g_runtime.game.engine.last_tick_ms==29980);
 tick(30000);assert(steps==1); /* timeout resumes exactly one frame */
 strcpy(g_confirm_action,"restart");g_confirm_until=60000;tick(30020);assert(steps==1);
 g_confirm_action[0]=0;tick(30040);assert(steps==2); /* cancel also has no catch-up */
}
'''
    c = tmp_path / "confirmation.c"
    c.write_text(code)
    binary = tmp_path / "confirmation"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
