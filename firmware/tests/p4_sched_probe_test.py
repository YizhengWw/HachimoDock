"""One-shot diagnostic adapter with the real trace and resource broker. No SDK/USB."""
from pathlib import Path
import subprocess
import pytest

ROOT=Path(__file__).resolve().parents[1]
STUBS=ROOT/"tests/sched_probe_stubs"

@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    tmp=tmp_path_factory.mktemp("sched-probe")
    flags=["-std=c11","-Wall","-Wextra","-Werror","-pedantic","-pthread",
           "-fsanitize=address,undefined","-fno-omit-frame-pointer",
           "-DPET_P4_SCHED_TRACE_ENABLE=1","-DPET_P4_SCHED_PROBE_ENABLE=1",
           "-DPET_SCHED_TRACE_HOST_TEST=1","-I",str(STUBS),"-I",str(ROOT/"main")]
    objects=[]
    for name,defines in (("pet_p4_resources_device.c",["-Dpet_resources_device_acquire=probe_real_acquire","-Dpet_resources_device_finish=probe_real_finish"]),
                         ("pet_p4_sched_trace.c",["-Dpet_sched_trace_begin=probe_real_trace_begin"])):
        obj=tmp/(name+".o");objects.append(str(obj))
        subprocess.run(["cc",*flags,*defines,"-c",str(ROOT/"main"/name),"-o",str(obj)],check=True)
    exe=tmp/"probe"
    subprocess.run(["cc",*flags,str(ROOT/"main/pet_p4_resources.c"),
                    str(ROOT/"tests/p4_sched_probe_test.c"),*objects,"-o",str(exe)],check=True)
    return exe

@pytest.mark.parametrize("scenario",["basic","cancel_ttl","arm_ttl","negative_clock","backwards_clock",
                                    "finish_failure","owner_loss","revoke_during_arm","end_during_claim","marker_drain",
                                    "earlier_completion","complete_then_audio","owner_exhaustion",
                                    "revision_exhaustion_idle","revision_exhaustion_inflight"])
def test_lifecycle_and_interleavings(probe,scenario):
    subprocess.run([str(probe),scenario],check=True,timeout=10)

@pytest.mark.parametrize("scenario",["audio_first","audio_second","save_audio_first","save_audio_second"])
@pytest.mark.parametrize("flag",range(5))
def test_every_fresh_audio_gate(probe,scenario,flag):
    subprocess.run([str(probe),scenario,str(flag)],check=True,timeout=10)

def test_default_off_has_no_hooks_storage_or_sdk_dependency(tmp_path):
    obj=tmp_path/"disabled.o"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-I",str(ROOT/"main"),
                    "-c",str(ROOT/"main/pet_p4_sched_probe.c"),"-o",str(obj)],check=True)
    symbols=subprocess.run(["nm",str(obj)],text=True,capture_output=True,check=True).stdout
    for name in ("probe_mux","stored","pet_resources_","pet_p4_audio_","published_trace"):
        assert name not in symbols

def test_enabled_probe_requires_enabled_trace(tmp_path):
    result=subprocess.run(["cc","-std=c11","-DPET_P4_SCHED_PROBE_ENABLE=1","-I",str(STUBS),
                           "-I",str(ROOT/"main"),"-c",str(ROOT/"main/pet_p4_sched_probe.c"),"-o",str(tmp_path/"bad.o")],
                          text=True,capture_output=True)
    assert result.returncode and "requires explicitly enabled scheduler trace" in result.stderr
