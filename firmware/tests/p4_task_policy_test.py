"""Control preemption and inherited-priority handling, without changing audio."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def test_native_animation_priority_policy():
    program = r'''
#include <assert.h>
#include "pet_p4_task_policy.h"
int main(void) {
  assert(PET_P4_PROTOCOL_TASK_PRIORITY == 11);
  assert(PET_P4_TRANSPORT_TASK_PRIORITY == 10);
  assert(PET_P4_INPUT_TASK_PRIORITY == 9);
  assert(PET_P4_STORAGE_IO_TASK_PRIORITY == 1); /* fair main/save-worker base */
  assert(PET_P4_H264_TASK_PRIORITY < PET_P4_INPUT_TASK_PRIORITY);
  assert(PET_P4_H264_TASK_PRIORITY == 8); /* share, never outrank TinyUSB */
  assert(pet_p4_render_can_boost(1, 1));
  for (unsigned effective = 2; effective < 25; ++effective)
    assert(!pet_p4_render_can_boost(effective, 1));
  assert(!pet_p4_render_can_boost(PET_P4_H264_TASK_PRIORITY, PET_P4_H264_TASK_PRIORITY));
}
'''
    with tempfile.TemporaryDirectory() as temporary:
        source = Path(temporary) / "policy.c"
        binary = Path(temporary) / "policy"
        source.write_text(program)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_real_task_creation_and_both_animation_boosts_use_policy():
    main = (ROOT / "main/pet_p4_main.c").read_text()
    renderer = (ROOT / "main/pet_p4_renderer.c").read_text()
    inputs = (ROOT / "main/pet_p4_input.c").read_text()
    assert main.count("PET_P4_PROTOCOL_TASK_PRIORITY,") == 1
    assert main.count("PET_P4_TRANSPORT_TASK_PRIORITY,") == 2
    assert "NULL, PET_P4_INPUT_TASK_PRIORITY, NULL" in inputs
    assert renderer.count("pet_p4_render_can_boost(uxTaskPriorityGet(NULL), ESP_TASK_MAIN_PRIO)") == 2
    assert renderer.count("vTaskPrioritySet(NULL, ESP_TASK_MAIN_PRIO)") == 2
    assert "original_priority" not in renderer
    assert "CONFIG_ESP_H264_DUAL_TASK_PRIORITY == PET_P4_H264_SECONDARY_TASK_PRIORITY" in renderer
    assert "CONFIG_ESP_H264_DUAL_TASK_CORE == 1" in renderer
    assert "PET_P4_H264_PACK_TASK_PRIORITY PET_P4_H264_TASK_PRIORITY" in renderer
    assert "CONFIG_ESP_H264_DUAL_TASK_PRIORITY=17" in (ROOT / "sdkconfig.defaults").read_text()
