"""Execute production storage scope ownership; no filesystem/device/audio IO."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def scope_binary(tmp_path_factory):
    path = tmp_path_factory.mktemp("storage-scope-owner") / "scope-owner"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
        str(ROOT / "tests/petui_storage_scope_owner_test.c"),
        str(ROOT / "main/pet_p4_storage_lane.c"), "-o", str(path),
    ], check=True)
    return path


@pytest.mark.parametrize("case", range(9), ids=[
    "copy-locked", "copy-unlocked", "no-rebind", "no-copied-publish",
    "original-reuse-10000", "task-isolation", "retired-copy", "no-lane-alias", "unidentified-task",
])
def test_production_scope_fixed_address_ownership(scope_binary, case):
    subprocess.run([str(scope_binary), str(case)], check=True)
