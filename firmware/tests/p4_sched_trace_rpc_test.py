"""No device, firmware build, playback/capture, credentials or provider IO."""
import subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def test_actual_passive_observer_adapter(tmp_path):
    executable=tmp_path/'probe'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-pthread',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-DPET_P4_SCHED_TRACE_ENABLE=1','-DPET_SCHED_TRACE_HOST_TEST=1',
        '-I',str(ROOT/'tests/sched_trace_stubs'),'-I',str(ROOT/'tests/program_probe_stubs'),'-I',str(ROOT/'main'),
        '-I',str(ROOT/'tests/vendor/cjson'),str(ROOT/'main/pet_p4_sched_trace.c'),str(ROOT/'main/pet_p4_sched_trace_rpc.c'),
        str(ROOT/'tests/p4_sched_trace_rpc_test.c'),str(ROOT/'tests/vendor/cjson/cJSON.c'),'-lm','-o',str(executable)],check=True)
    result=subprocess.run([str(executable)],check=True,text=True,capture_output=True,timeout=20)
    assert result.stdout.strip()=='PASS numeric opt-in scheduler diagnostic boundaries'

def test_ordinary_build_has_no_trace_adapter_or_state(tmp_path):
    obj=tmp_path/'off.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-c',
        str(ROOT/'main/pet_p4_sched_trace_rpc.c'),'-o',str(obj)],check=True)
    symbols=subprocess.run(['nm',str(obj)],check=True,text=True,capture_output=True).stdout
    assert 'pet_p4_sched_trace_rpc_handle' not in symbols and 'reply' not in symbols
    config=(ROOT/'platformio.ini').read_text()
    normal=config.split('[env:esp32_p4_evboard]',1)[1].split('[env:',1)[0]
    ui=config.split('[env:esp32_p4_canvas_candidate]',1)[1].split('[env:',1)[0]
    assert 'SCHED_TRACE_ENABLE' not in normal+ui and '--wrap=' not in normal+ui
