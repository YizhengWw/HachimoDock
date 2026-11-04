"""Actual C compact pet status; synthetic only, no SDK/board/provider calls."""
from pathlib import Path
import os
import subprocess
ROOT=Path(__file__).resolve().parents[1]
PRODUCTION=Path(os.environ.get("PET_FW_STATUS_PRODUCTION_ROOT",ROOT))
def test_compact_pet_status_never_uses_agent_identity(tmp_path):
    binary=tmp_path/"pet-status-only"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror",
        "-I",str(PRODUCTION/"main"),str(ROOT/"main/pet_p4_view.c"),
        str(PRODUCTION/"main/pet_p4_conversation.c"),str(ROOT/"tests/p4_pet_status_test.c"),
        "-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
