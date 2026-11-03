"""Compile both production pause predicates; no audio worker or device access."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_protocol_and_main_keep_music_buffering_and_underrun_protected(tmp_path):
    predicates = []
    for file in ("pet_p4_protocol.c", "pet_p4_main.c"):
        source = (ROOT / "main" / file).read_text()
        calls = re.findall(r"pet_bubble_pixels_maintain\([^;]*?,\s*(pet_p4_audio_active\(\)[\s\S]*?)\);", source)
        assert len(calls) == 1, file
        predicates.append(calls[0])
    harness = r'''
#include <assert.h>
#include <stdbool.h>
static unsigned mask;
typedef struct {bool asset_transfer_active;} pause_state_t;
static pause_state_t g_state;
static const pause_state_t *state=&g_state;
static bool pet_p4_audio_active(void) {return (mask&1U)!=0;}
static bool pet_p4_audio_playback_active(void) {return (mask&2U)!=0;}
static bool pet_p4_audio_conversation_active(void) {return (mask&4U)!=0;}
static bool pet_p4_ota_active(void) {return (mask&8U)!=0;}
static bool pet_p4_audio_music_active(void) {return (mask&32U)!=0;}
'''
    for name, predicate in zip(("protocol_paused", "main_paused"), predicates):
        harness += "static bool " + name + "(void) {return " + predicate + ";}\n"
    harness += r'''
int main(void) {
  /* In music startup buffering/underrun all three old audio flags are false.
   * Music's retained realtime ownership must still block pixel transfers. */
  mask=32U;g_state.asset_transfer_active=false;
  assert(!pet_p4_audio_active() && !pet_p4_audio_playback_active()
    && !pet_p4_audio_conversation_active());
  assert(protocol_paused() && main_paused());
  /* Preserve idle admission plus every existing protection and combination. */
  for(mask=0;mask<64;mask++) {
    g_state.asset_transfer_active=(mask&16U)!=0;
    assert(protocol_paused()==(mask!=0));
    assert(main_paused()==(mask!=0));
  }
}
'''
    source = tmp_path / "pause.c"
    source.write_text(harness)
    binary = tmp_path / "pause"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
