"""Run the production exact-frame gate; check alternate-buffer reuse/failures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def test_exact_gate():
    source = r'''
#include "pet_p4_frame_gate.h"
#include <assert.h>
#include <stdint.h>
int main(void) {
  uint16_t a[640 * 480] = {0}, b[640 * 480] = {0};
  assert(!pet_p4_frame_unchanged(a, b, sizeof(a), false));
  assert(pet_p4_frame_unchanged(a, b, sizeof(a), true));
  assert(pet_p4_frame_unchanged(b, a, sizeof(a), true));
  b[640 * 480 - 1] = 1;
  assert(!pet_p4_frame_unchanged(b, a, sizeof(a), true));
  assert(!pet_p4_frame_unchanged(a, a, sizeof(a), true));
  assert(!pet_p4_frame_unchanged(a, NULL, sizeof(a), true));
  assert(!pet_p4_frame_unchanged(a, b, 0, true));
  memcpy(a, b, sizeof(a));
  assert(!pet_p4_frame_unchanged(a, b, sizeof(a), false));
  assert(pet_p4_frame_unchanged(a, b, sizeof(a), true));
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "frame-gate"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
                        "-I", str(ROOT / "main"), "-o", str(exe)], input=source, text=True, check=True)
        subprocess.run([str(exe)], check=True)


def test_renderer_invalidates_on_error_reset_and_transfer():
    renderer = (ROOT / "main/pet_p4_renderer.c").read_text()
    assert 'g_last_native_valid = lcd_err == ESP_OK;' in renderer
    for name in ('pet_p4_renderer_reset_playback', 'pet_p4_renderer_render_transfer_status'):
        assert 'g_last_native_valid = false;' in renderer.split(name + '(')[1].split('\n}')[0]
