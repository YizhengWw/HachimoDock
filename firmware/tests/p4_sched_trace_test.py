"""Default-off, fixed-ring SDK wrappers; host-only, never opens USB or invokes PIO."""
from pathlib import Path
import os
import subprocess
import pytest

ROOT=Path(__file__).resolve().parents[1]
STUBS=ROOT/"tests/sched_trace_stubs"


def test_enabled_wrappers_window_generation_clock_overflow_and_concurrency(tmp_path):
    binary=tmp_path/"sched-observer"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-pedantic","-pthread",
                    "-fsanitize=address,undefined","-fno-omit-frame-pointer",
                    "-DPET_P4_SCHED_TRACE_ENABLE=1","-DPET_SCHED_TRACE_HOST_TEST=1",
                    "-I",str(STUBS),"-I",str(ROOT/"main"),str(ROOT/"main/pet_p4_sched_trace.c"),
                    str(ROOT/"tests/p4_sched_trace_test.c"),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)


def test_default_disabled_has_no_hooks_or_storage(tmp_path):
    obj=tmp_path/"disabled.o"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-I",str(ROOT/"main"),
                    "-c",str(ROOT/"main/pet_p4_sched_trace.c"),"-o",str(obj)],check=True)
    symbols=subprocess.run(["nm",str(obj)],text=True,capture_output=True,check=True).stdout
    assert "__wrap_" not in symbols and "trace_mux" not in symbols and "events" not in symbols


def test_observer_has_no_output_private_layout_or_kernel_getter():
    source=(ROOT/"main/pet_p4_sched_trace.c").read_text()
    for forbidden in ("printf(","ESP_LOG", "malloc(","fopen(","uxTaskPriorityGet(",
                      "uxTaskPriorityGetFromISR(","pcTaskGetName(","xTaskGetCurrentTaskHandle(",
                      "vTaskPrioritySet(","vTaskCoreAffinitySet(","TCB_t", "tskTCB"):
        assert forbidden not in source
    assert "#if PET_P4_SCHED_TRACE_ENABLE" in source
    assert "ATOMIC_INT_LOCK_FREE==2" in source


def test_opt_in_real_sdk_archive_wrap_relocations(tmp_path):
    """Inspect explicitly supplied artifacts only; never build/download SDKs.

    The same-TU negative control is essential: a passing external reference
    does not prove --wrap covers every call in the final production image.
    """
    build=os.environ.get("PET_SCHED_TRACE_SDK_BUILD")
    tools=os.environ.get("PET_SCHED_TRACE_RISCV_TOOLS")
    if not build or not tools:
        pytest.skip("explicit SDK archive and RISC-V tool paths required for read-only link audit")
    build=Path(build);tools=Path(tools)
    ar=tools/"riscv32-esp-elf-ar";ld=tools/"riscv32-esp-elf-ld";dump=tools/"riscv32-esp-elf-objdump"
    cases=[
        ("freertos","queue.c.o",("xTaskPriorityInherit","xTaskPriorityDisinherit"),True),
        ("freertos","portasm.S.o",("vTaskSwitchContext",),True),
        ("freertos","tasks.c.o",("vTaskSwitchContext",),False),
        ("spiffs","spiffs_hydrogen.c.o",("spiffs_api_lock","spiffs_api_unlock"),True),
    ]
    for component,member,symbols,external in cases:
        archive=build/"esp-idf"/component/f"lib{component}.a"
        # Outputs are private, disposable linker products, not SDK modifications.
        data=subprocess.run([str(ar),"p",str(archive),member],capture_output=True,check=True).stdout
        source=tmp_path/member;source.write_bytes(data)
        out=tmp_path/(member+".wrapped")
        subprocess.run([str(ld),"-r",*["--wrap="+s for s in symbols],str(source),"-o",str(out)],check=True)
        reloc=subprocess.run([str(dump),"-r",str(out)],text=True,capture_output=True,check=True).stdout
        calls=[line for line in reloc.splitlines() if "R_RISCV_CALL_PLT" in line]
        for symbol in symbols:
            expected="__wrap_"+symbol if external else symbol
            assert any(line.split()[-1]==expected for line in calls), (member,expected)
            if not external:
                assert not any(line.split()[-1]=="__wrap_"+symbol for line in calls)
