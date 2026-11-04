"""Native cooperative policy over the product core/runtime/host; no devices."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def test_cooperative_deadline_and_real_host_boundary(tmp_path):
    binary=tmp_path/'deadline'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-fsanitize=address,undefined',
        '-I',str(ROOT/'main'),*[str(ROOT/'main'/('pet_p4_program_'+n+'.c')) for n in ('core','image','runtime','host')],
        str(ROOT/'tests/p4_program_deadline_test.c'),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
    assert result.returncode==0 and not result.stderr,result.stderr
    assert 'exact continuation' in result.stdout
