"""Private timing reference cannot become a guest program execution profile."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_reference_is_explicit_and_does_not_change_default_policy(tmp_path):
    source = tmp_path / "reference.c"
    source.write_text('''
#include <assert.h>
#include "pet_p4_task_policy.h"
int main(void) {
#if PET_P4_RENDER_SCHEDULING_REFERENCE
  assert(PET_P4_H264_TASK_PRIORITY == PET_P4_RENDER_REFERENCE_PRIORITY);
#else
  assert(PET_P4_H264_TASK_PRIORITY == 8);
#endif
  assert(PET_P4_H264_SECONDARY_TASK_PRIORITY == 17);
  assert(PET_P4_PROTOCOL_TASK_PRIORITY == 11);
  assert(PET_P4_TRANSPORT_TASK_PRIORITY == 10);
  assert(PET_P4_INPUT_TASK_PRIORITY == 9);
  assert(pet_p4_render_can_boost(1, 1));
  assert(!pet_p4_render_can_boost(11, 1));
}
''')
    for enabled, priority in ((0, 16), (1, 16), (1, 9)):
        binary = tmp_path / f"reference-{enabled}-{priority}"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                        f"-DPET_P4_RENDER_SCHEDULING_REFERENCE={enabled}",
                        f"-DPET_P4_RENDER_REFERENCE_PRIORITY={priority}",
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    config = (ROOT / "platformio.ini").read_text()
    reference = config.split("[env:esp32_p4_render_reference]", 1)[1].split("\n[env:", 1)[0]
    assert "extends = env:esp32_p4_canvas_candidate" in reference
    assert "-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=0" in reference
    assert "-DPET_P4_RENDER_SCHEDULING_REFERENCE=1" in reference
    assert "-DPET_P4_RENDER_REFERENCE_PRIORITY=9" in reference
    assert "-DPET_P4_RENDER_SCHEDULING_REFERENCE" not in config.split("[env:esp32_p4_render_reference]", 1)[0]


def test_reference_compile_refuses_component_execution(tmp_path):
    source = tmp_path / "forbidden.c"
    source.write_text('#include "pet_p4_task_policy.h"\nint main(void){return 0;}\n')
    result = subprocess.run(["cc", "-std=c11", "-I", str(ROOT / "main"),
                             "-DPET_P4_RENDER_SCHEDULING_REFERENCE=1",
                             "-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1",
                             str(source), "-o", str(tmp_path / "forbidden")],
                            capture_output=True, text=True)
    assert result.returncode != 0
    assert "reference cannot enable component program execution" in result.stderr
