"""
[Input] Factory image sources, built-in Terrier media, and built-in P4 component packages.
[Output] Contract checks for hashing, H.264, component preload, layout, and export settings.
[Pos] Unit coverage for ESP32-P4 factory provisioning.
[Sync] If this file changes, update esp-p4-runtime/.folder.md.
"""

import importlib.util
import json
import re
from pathlib import Path


RUNTIME = Path(__file__).resolve().parents[1]
REPOSITORY = RUNTIME.parent
TOOL_PATH = RUNTIME / "tools" / "build_factory_image.py"
SPEC = importlib.util.spec_from_file_location("build_factory_image", TOOL_PATH)
factory = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(factory)


def test_factory_hashes_match_firmware_filenames():
    assert factory.slot_file_name(0, "p4/manifest.json") == "s0_a8b42b148f04d84d"
    assert factory.slot_file_name(0, "p4/audio/done.wav") == "s0_add0927412aafb15"
    assert (
        factory.slot_file_name(
            0, "p4/families/sha256-0123456789abcdef01234567.h264"
        )
        == "s0_42234cdda81680ab"
    )


def test_factory_h264_parser_requires_the_proven_minimal_sps():
    sps = factory.p4_minimal_h264_sps(640, 480)
    assert sps.hex() == "6742c01eda0280f440"
    stream = (
        b"\x00\x00\x00\x01"
        + sps
        + b"\x00\x00\x00\x01\x68\xce\x3c\x80"
        + b"\x00\x00\x00\x01\x09\xf0"
        + b"\x00\x00\x00\x01\x65\x88\x84"
    )
    assert factory.parse_p4_h264_stream(stream, 640, 480) == (1, len(stream))
    multi_slice = stream + b"\x00\x00\x00\x01\x65\x88\x84"
    try:
        factory.parse_p4_h264_stream(multi_slice, 640, 480)
    except ValueError as error:
        assert "exactly one slice" in str(error)
    else:
        raise AssertionError("multi-slice H.264 was accepted")
    incompatible = stream.replace(sps, sps[:-1] + b"\x41", 1)
    try:
        factory.parse_p4_h264_stream(incompatible, 640, 480)
    except ValueError as error:
        assert "SPS" in str(error)
    else:
        raise AssertionError("incompatible H.264 SPS was accepted")


def test_factory_config_matches_desktop_export_contract_and_media():
    config = json.loads((RUNTIME / "factory-config.json").read_text(encoding="utf-8"))
    appearance = config["appearance"]
    desktop = (REPOSITORY / "ref" / "src-tauri" / "src" / "usb_serial.rs").read_text(
        encoding="utf-8"
    )
    expected = {
        "width": "P4_APPEARANCE_WIDTH",
        "height": "P4_APPEARANCE_HEIGHT",
        "fps": "P4_APPEARANCE_FPS",
        "maxFrames": "P4_APPEARANCE_MAX_FRAMES",
        "h264Crf": "P4_APPEARANCE_H264_CRF",
    }
    for key, constant in expected.items():
        match = re.search(rf"const {constant}: u32 = (\d+);", desktop)
        assert match, constant
        assert appearance[key] == int(match.group(1))
    version = re.search(r"const P4_READY_PROFILE_VERSION: u32 = (\d+);", desktop)
    assert version
    assert appearance["readyProfile"] == (
        f"v{version.group(1)}-{appearance['width']}x{appearance['height']}-"
        f"{appearance['fps']}fps-{appearance['maxFrames']}f-h264-crf{appearance['h264Crf']}"
    )
    assert re.fullmatch(r"[0-9a-f]{64}", appearance["packId"])

    clips = (RUNTIME / appearance["clipsDir"]).resolve()
    videos = sorted(clips.glob("*.mp4"))
    assert len(videos) == 16
    assert {"welcome", "idle.playing", "working.thinking", "done"} <= {
        path.stem for path in videos
    }
    assert all(path.stat().st_size > 0 for path in videos)


def test_factory_reuses_the_desktop_bundled_p4_ready_pack(tmp_path):
    config = json.loads((RUNTIME / "factory-config.json").read_text(encoding="utf-8"))
    appearance = config["appearance"]
    ready_root = (
        RUNTIME / appearance["clipsDir"] / "p4-ready" / appearance["readyProfile"]
    ).resolve()
    ready_manifest = json.loads(
        (ready_root / "p4" / "manifest.json").read_text(encoding="utf-8")
    )
    ready_metadata = json.loads(
        (ready_root / "ready-meta.json").read_text(encoding="utf-8")
    )
    source_durations = {
        item["family"]: item["durationMs"] for item in ready_metadata["sourceAssets"]
    }
    assert ready_metadata["schemaVersion"] == 4
    assert ready_manifest["format"] == "p4-h264-v1"
    assert ready_manifest["codec"] == "h264"
    assert ready_manifest["container"] == "annex-b"
    assert ready_manifest["packId"] == appearance["packId"]
    assert ready_metadata["packId"] == appearance["packId"]
    for family in ready_manifest["families"]:
        assert 0 < family["frames"] <= appearance["maxFrames"]
        assert 0 < family["fps"] <= appearance["fps"]
        assert family["frameDurationMs"] > 0
        playback_ms = family["frames"] * family["frameDurationMs"]
        source_ms = source_durations[family["family"]]
        assert playback_ms >= source_ms
        assert playback_ms - source_ms < family["frames"]
        stream_path = ready_root / family["path"]
        frames, stream_bytes = factory.parse_p4_h264_stream(
            stream_path.read_bytes(), appearance["width"], appearance["height"]
        )
        assert frames == family["frames"]
        assert stream_bytes == family["streamBytes"]

    summary = factory.build_default_appearance_tree(
        RUNTIME,
        tmp_path,
        config,
    )

    assert summary["readyProfile"] == appearance["readyProfile"]
    assert summary["packId"] == ready_manifest["packId"]
    assert summary["families"] == 16
    stored_manifest = (
        summary["tree"]
        / factory.slot_file_name(appearance["slot"], factory.P4_MANIFEST_LOGICAL_PATH)
    )
    assert json.loads(stored_manifest.read_text(encoding="utf-8")) == ready_manifest
    ready_marker = summary["tree"] / factory.SLOT_READY_MARKER_TEMPLATE.format(
        slot=appearance["slot"]
    )
    assert ready_marker.read_text(encoding="ascii") == f"{ready_manifest['packId']}\n"

    mismatched = json.loads(json.dumps(config))
    mismatched["appearance"]["packId"] = "0" * 64
    try:
        factory.build_default_appearance_tree(RUNTIME, tmp_path, mismatched)
    except ValueError as error:
        assert "packId mismatch" in str(error)
    else:
        raise AssertionError("factory accepted a ready pack that differs from its pin")


def test_factory_components_match_current_builtin_catalog_and_device_files(tmp_path):
    config = json.loads((RUNTIME / "factory-config.json").read_text(encoding="utf-8"))
    expected_ids = [
        "two-key-pong",
        "flappy-bird",
        "block-combo",
        "snake-turn",
        "tomato-clock",
        "drink-reminder",
        "token-usage",
    ]
    assert config["components"]["ids"] == expected_ids
    assert "falling-catch" not in config["components"]["ids"]
    assert "slack-off-countdown" not in config["components"]["ids"]
    assert "ten-second-tap" not in config["components"]["ids"]

    spiffs_tree = tmp_path / "factory-spiffs"
    spiffs_tree.mkdir()
    summary = factory.build_builtin_component_tree(RUNTIME, config, spiffs_tree)
    assert summary["count"] == len(expected_ids)
    assert summary["defaultActiveId"] == "two-key-pong"
    assert summary["physicalFiles"] == len(expected_ids) * 2 + 1
    assert not (spiffs_tree / "p4-miniapp-id.txt").exists()

    catalog = json.loads(
        (spiffs_tree / "p4-miniapps-0.json").read_text(encoding="utf-8")
    )
    assert catalog["version"] == 2
    assert catalog["sequence"] == 1
    assert catalog["activeWidgetId"] == "two-key-pong"
    assert [item["widgetId"] for item in catalog["items"]] == expected_ids
    assert [item["slot"] for item in catalog["items"]] == list(range(len(expected_ids)))
    assert [item["packageGeneration"] for item in catalog["items"]] == [0] * len(expected_ids)
    assert all(re.fullmatch(r"[0-9a-f]{8}", item["widgetChecksum"]) for item in catalog["items"])
    assert all(re.fullmatch(r"[0-9a-f]{8}", item["buttonsChecksum"]) for item in catalog["items"])
    for item in catalog["items"]:
        slot = item["slot"]
        assert item["widgetChecksum"] == (
            f"{factory.fnv1a32_bytes((spiffs_tree / f'p4w{slot:02d}.json').read_bytes()):08x}"
        )
        assert item["buttonsChecksum"] == (
            f"{factory.fnv1a32_bytes((spiffs_tree / f'p4b{slot:02d}.json').read_bytes()):08x}"
        )
    assert all(item["title"] for item in catalog["items"])
    assert all(
        (spiffs_tree / f"p4w{slot:02d}.json").is_file()
        for slot in range(len(expected_ids))
    )
    assert all(
        (spiffs_tree / f"p4b{slot:02d}.json").is_file()
        for slot in range(len(expected_ids))
    )

    two_key_widget = json.loads(
        (spiffs_tree / "p4w00.json").read_text(encoding="utf-8")
    )
    two_key_buttons = json.loads(
        (spiffs_tree / "p4b00.json").read_text(encoding="utf-8")
    )
    assert two_key_widget["engine"] == "p4-bounded-runtime-v3"
    assert two_key_widget["scene"]["grid"] == {"width": 16, "height": 16}
    assert [binding["action"] for binding in two_key_buttons] == [
        "shift_left",
        "shift_right",
        "start",
    ]

    token_widget = json.loads(
        (spiffs_tree / "p4w06.json").read_text(encoding="utf-8")
    )
    assert "readers" not in token_widget
    assert "fetchers" not in token_widget


def test_factory_platformio_targets_and_partition_contract():
    platformio = (RUNTIME / "platformio.ini").read_text(encoding="utf-8")
    integration = (
        RUNTIME / "scripts" / "platformio_factory_image.py"
    ).read_text(encoding="utf-8")
    assert "scripts/platformio_factory_image.py" in platformio
    assert 'name="factory"' in integration
    assert 'name="factory_upload"' in integration
    assert factory.FACTORY_FORMAT == "pet-manager-p4-factory-v1"

    config = json.loads((RUNTIME / "factory-config.json").read_text(encoding="utf-8"))
    offset, size = factory.partition_entry(
        RUNTIME / "partitions.csv", config["spiffs"]["partitionLabel"]
    )
    assert offset == 0x520000
    assert size == 0x6E0000
    builtin_offset, builtin_size = factory.partition_entry(
        RUNTIME / "partitions.csv", factory.BUILTIN_APPEARANCE_PARTITION_LABEL
    )
    custom_offset, custom_size = factory.partition_entry(
        RUNTIME / "partitions.csv", factory.CUSTOM_APPEARANCE_PARTITION_LABEL
    )
    assert (builtin_offset, builtin_size) == (0xC00000, 0xA00000)
    assert (custom_offset, custom_size) == (0x1600000, 0xA00000)


def test_factory_esptool_prefers_the_active_platformio_core(tmp_path, monkeypatch):
    core = tmp_path / "pio-core"
    esptool = core / "penv" / "Scripts" / "esptool.exe"
    esptool.parent.mkdir(parents=True)
    esptool.write_bytes(b"launcher")
    monkeypatch.delenv("ESPTOOL", raising=False)
    monkeypatch.delenv("HACHIMO_PLATFORMIO_CORE_DIR", raising=False)
    monkeypatch.setenv("PLATFORMIO_CORE_DIR", str(core))

    assert factory.resolve_esptool(None) == esptool


def test_factory_spiffsgen_prefers_the_active_platformio_core(tmp_path, monkeypatch):
    core = tmp_path / "pio-core"
    spiffsgen = (
        core
        / "packages"
        / "framework-espidf"
        / "components"
        / "spiffs"
        / "spiffsgen.py"
    )
    spiffsgen.parent.mkdir(parents=True)
    spiffsgen.write_text("# factory fixture\n", encoding="utf-8")
    monkeypatch.delenv("SPIFFSGEN", raising=False)
    monkeypatch.delenv("IDF_PATH", raising=False)
    monkeypatch.delenv("HACHIMO_PLATFORMIO_CORE_DIR", raising=False)
    monkeypatch.setenv("PLATFORMIO_CORE_DIR", str(core))

    assert factory.resolve_spiffsgen(None) == spiffsgen
