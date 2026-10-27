from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def test_fruit_runtime(tmp_path):
    exe=tmp_path/'fruit'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-I',str(ROOT/'main'),str(ROOT/'main/pet_p4_fruit_core.c'),str(ROOT/'tests/p4_fruit_test.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
