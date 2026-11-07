"""Compile both production pause predicates; no audio worker or device access."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_protocol_and_main_keep_music_buffering_and_underrun_protected(tmp_path):
    predicates = []
    for file in ("pet_p4_protocol.c", "pet_p4_main.c"):
        source = (ROOT / "main" / file).read_text()
        calls = re.findall(
            r"pet_bubble_pixels_maintain\([^;]*?,\s*(pet_p4_audio_music_active\(\)[\s\S]*?)\);",
            source,
        )
        assert len(calls) == 1, file
        predicates.append(calls[0])
    harness = r'''
#include <assert.h>
#include <stdbool.h>
static unsigned mask;
typedef struct {bool asset_transfer_active;} pause_state_t;
static pause_state_t g_state;
static const pause_state_t *state=&g_state;
static bool pet_p4_audio_music_active(void) {return (mask&1U)!=0;}
static bool pet_p4_ota_active(void) {return (mask&2U)!=0;}
'''
    for name, predicate in zip(("protocol_paused", "main_paused"), predicates):
        harness += "static bool " + name + "(void) {return " + predicate + ";}\n"
    harness += r'''
int main(void) {
  /* Music startup buffering/underrun has no playback/conversation flag set.
   * Music's retained realtime ownership must still block pixel transfers. */
  mask=1U;g_state.asset_transfer_active=false;
  assert(protocol_paused() && main_paused());
  /* OTA still owns the write channel. */
  mask=2U;assert(protocol_paused() && main_paused());
  /* Asset transfers still own the write channel. */
  mask=0U;g_state.asset_transfer_active=true;assert(protocol_paused() && main_paused());
  /* Conversation/voice must NOT block pixels: blocking it forced the local
   * render fallback and the opacity flicker this pipeline change fixes. */
  mask=0U;g_state.asset_transfer_active=false;
  assert(!protocol_paused() && !main_paused());
}
'''
    source = tmp_path / "pause.c"
    source.write_text(harness)
    binary = tmp_path / "pause"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
