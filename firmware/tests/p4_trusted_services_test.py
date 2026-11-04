"""Trusted service native boundary regressions. No hardware/audio/network IO."""
from pathlib import Path
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]


def test_service_catalog_grants_budget_cancellation_and_timing(tmp_path):
    executable = tmp_path / "trusted-core"
    subprocess.run(["cc", *FLAGS, "-I", str(ROOT / "main"),
                    str(ROOT / "main/pet_p4_trusted_services.c"),
                    str(ROOT / "tests/p4_trusted_services_test.c"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


@pytest.mark.parametrize("test_only_capacity", [False, True])
def test_production_device_adapter_uses_real_broker_and_sdk_providers(tmp_path, test_only_capacity):
    executable = tmp_path / "trusted-device"
    resources = ROOT / "main/pet_p4_resources.c"
    if test_only_capacity:
        # Isolated host fault harness only. The shipping broker stays at zero.
        code = resources.read_text()
        marker = "[PET_RESOURCE_PERIPHERAL]={0,0}"
        assert code.count(marker) == 1
        resources = tmp_path / "resources-test-capacity.c"
        resources.write_text(code.replace(marker, "[PET_RESOURCE_PERIPHERAL]={1,1}"))
    subprocess.run(["cc", *FLAGS, "-I", str(ROOT / "main"),
                    "-I", str(ROOT / "tests/program_probe_stubs"),
                    "-I", str(ROOT / "managed_components/espressif__cjson/cJSON"),
                    str(resources),
                    str(ROOT / "main/pet_p4_resources_device.c"),
                    str(ROOT / "main/pet_p4_trusted_services.c"),
                    str(ROOT / "main/pet_p4_trusted_services_device.c"),
                    str(ROOT / "tests/p4_trusted_services_device_test.c"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


def test_readonly_adapter_has_no_raw_peripherals_reconfiguration_or_automatic_retry():
    code = (ROOT / "main/pet_p4_trusted_services_device.c").read_text()
    for forbidden in ("gpio_", "i2c_", "spi_", "dma_", "adc_oneshot", "adc_continuous",
                      "_init(", "capture_start(", "stream_begin(", "music_begin(",
                      "vTaskDelay(", "malloc(", "calloc(", "fopen(", "while ("):
        assert forbidden not in code
    assert "pet_p4_input_get_joystick_snapshot(&snapshot)" in code
    assert "esp_timer_get_time()" in code
