"""Production raw metadata parser: both read/mutation orders, writer exclusion,
and failure exits preserve bounded ownership without publishing stale IO.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def test_real_raw_metadata_epoch_and_partition_publication():
    source = (ROOT / "main/pet_p4_raw_assets.c").read_text()
    for include in ('#include "esp_partition.h"', '#include "freertos/FreeRTOS.h"', '#include "freertos/task.h"'):
        source = source.replace(include, "")
    harness = (ROOT / "tests/p4_raw_asset_cache_test.c").read_text()
    with tempfile.TemporaryDirectory() as temp:
        src, binary = Path(temp) / "raw.c", Path(temp) / "raw"
        src.write_text(harness.replace("/* RAW_PRODUCTION */", source))
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
            str(src), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)
