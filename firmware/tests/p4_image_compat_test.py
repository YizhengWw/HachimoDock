"""Host-only revision checks; no serial hardware or cloud access."""
from pathlib import Path
import subprocess
import sys
import types

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from p4_image_compat import image_revision_range, factory_revision_range
from flash_checked import flash


def image(minimum, maximum):
    data = bytearray(24)
    data[0] = 0xe9
    data[12] = 18
    data[15:17] = minimum.to_bytes(2, "little")
    data[17:19] = maximum.to_bytes(2, "little")
    return data


def factory(minimum, maximum):
    data = bytearray(0x10000 + 24)
    for offset in [0x2000, 0x10000]:
        data[offset:offset + 24] = image(minimum, maximum)
    return data


def test_device_header_matrix(tmp_path):
    binary = tmp_path / "chip-test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "main"), str(ROOT / "tests/p4_image_compat_test.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_v3_build_is_separate_without_changing_transport_or_ui():
    platform = (ROOT / "platformio.ini").read_text()
    v3 = platform.split("[env:esp32_p4_evboard_v3]", 1)[1]
    assert "55.03.38-1" in v3
    assert "board_build.f_cpu = 400000000L" in v3
    assert "custom_p4_uart_baud" not in v3
    assert "monitor_speed" not in v3
    assert "custom_p4_uart_baud = 4000000" in platform
    overlay = (ROOT / "sdkconfig.v3.defaults").read_text()
    assert "CONFIG_ESP32P4_REV_MIN_300=y" in overlay
    assert "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400=y" in overlay
    assert "CONFIG_ESP32P4_SELECTS_REV_LESS_V3 is not set" in overlay
    cmake = (ROOT / "CMakeLists.txt").read_text()
    assert "dependencies.v3.lock" in cmake
    for name, version in [("dependencies.lock", "5.5.1"), ("dependencies.v3.lock", "5.5.4")]:
        assert "version: " + version in (ROOT / name).read_text()


def test_factory_rejects_unbounded_and_mixed_images():
    for lo, hi in [(1,199), (300,399)]:
        assert factory_revision_range(factory(lo,hi)) == (lo,hi)
    for lo, hi in [(0,65535), (1,399), (300,199), (200,299)]:
        with pytest.raises(ValueError):
            image_revision_range(image(lo,hi))
    mixed = factory(1,199)
    mixed[0x10000:0x10018] = image(300,399)
    with pytest.raises(ValueError):
        factory_revision_range(mixed)


@pytest.mark.parametrize("revision", [1,100,199,200,300,301,302,399,400])
def test_flash_checks_revision_before_any_erase_or_write(tmp_path, monkeypatch, revision):
    events = []
    class Device:
        CHIP_NAME = "ESP32-P4"
        def __enter__(self): return self
        def __exit__(self, *_): events.append("closed")
        def get_chip_revision(self): events.append("revision"); return revision
        def change_baud(self, baud): events.append("baud")
        def hard_reset(self): events.append("reset")
    device = Device()
    monkeypatch.setitem(sys.modules, "esptool", types.SimpleNamespace(__version__="5.4.0", detect_chip=lambda *a,**k: device))
    def write(esp, data, **kwargs):
        assert esp is device and kwargs == {"erase_all": True, "compress": True}
        events.append("write")
    monkeypatch.setitem(sys.modules, "esptool.cmds", types.SimpleNamespace(run_stub=lambda d:d, write_flash=write))
    path = tmp_path / "factory.bin"
    path.write_bytes(factory(300,399))
    if 300 <= revision <= 399:
        flash(path, "test-port")
        assert events == ["revision", "baud", "write", "reset", "closed"]
    else:
        with pytest.raises(ValueError): flash(path, "test-port")
        assert events == ["revision", "closed"]


def test_flash_rejects_old_tool_before_opening_device(tmp_path, monkeypatch):
    def unexpected(*args, **kwargs):
        pytest.fail("old tool must not open the device or write flash")
    monkeypatch.setitem(sys.modules, "esptool", types.SimpleNamespace(__version__="5.2.0", detect_chip=unexpected))
    monkeypatch.setitem(sys.modules, "esptool.cmds", types.SimpleNamespace(run_stub=unexpected, write_flash=unexpected))
    with pytest.raises(ValueError, match="5.4.0"):
        flash(tmp_path / "not-read.bin", "test-port")


def test_packaged_tools_upgrade_existing_environments():
    source = (ROOT.parent / "pc/scripts/package-factory-release.mjs").read_text()
    assert source.count('esptool==5.4.0') == 2
    assert source.count('__import__("esptool").__version__ == "5.4.0"') == 2
    assert 'esptool==5.2.0' not in source
