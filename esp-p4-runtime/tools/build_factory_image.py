#!/usr/bin/env python3
"""
[Input] PlatformIO artifacts, factory-config.json, desktop-built Terrier P4 ready pack, and P4 components.
[Output] A preloaded SPIFFS image and one flashable ESP32-P4 factory binary.
[Pos] Factory provisioning tool for esp-p4-runtime.
[Sync] If this file changes, update esp-p4-runtime/.folder.md and README.md.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path
from typing import Iterable, Sequence


FACTORY_CONFIG_NAME = "factory-config.json"
FACTORY_IMAGE_NAME = "pet-manager-p4-factory.bin"
FACTORY_MANIFEST_NAME = "pet-manager-p4-factory.json"
FACTORY_FORMAT = "pet-manager-p4-factory-v1"
SPIFFS_IMAGE_NAME = "default-terrier.spiffs.bin"
RAW_APPEARANCE_IMAGE_NAME = "default-terrier.appearance0.bin"
SPIFFS_TREE_NAME = "factory-spiffs"
P4_MANIFEST_LOGICAL_PATH = "p4/manifest.json"
ACTIVE_SLOT_MARKER = "p4slot.txt"
SLOT_READY_MARKER_TEMPLATE = "s{slot}_ready"
MINIAPP_CATALOG_FILE = "p4-miniapps-0.json"
BUILTIN_APPEARANCE_PARTITION_LABEL = "appearance0"
CUSTOM_APPEARANCE_PARTITION_LABEL = "appearance1"
RAW_APPEARANCE_HEADER_BYTES = 4096
RAW_APPEARANCE_HEADER_ENTRY_OFFSET = 88
RAW_APPEARANCE_HEADER_ENTRY_BYTES = 24
RAW_APPEARANCE_MAX_FILES = 32
FNV1A64_OFFSET = 0xCBF29CE484222325
FNV1A64_PRIME = 0x100000001B3
FNV1A32_OFFSET = 0x811C9DC5
FNV1A32_PRIME = 0x01000193


def parse_int(value: object) -> int:
    if isinstance(value, int):
        return value
    text = str(value).strip()
    if not text:
        raise ValueError("empty integer value")
    return int(text, 0)


def parse_flash_size(value: str) -> int:
    match = re.fullmatch(r"\s*(\d+)\s*(KB|MB)?\s*", value, re.IGNORECASE)
    if not match:
        raise ValueError(f"unsupported flash size: {value}")
    amount = int(match.group(1))
    unit = (match.group(2) or "").upper()
    return amount * (1024 if unit == "KB" else 1024 * 1024 if unit == "MB" else 1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_region(path: Path, offset: int, size: int) -> str:
    digest = hashlib.sha256()
    remaining = size
    with path.open("rb") as handle:
        handle.seek(offset)
        while remaining:
            chunk = handle.read(min(1024 * 1024, remaining))
            if not chunk:
                raise RuntimeError(f"factory image ended before 0x{offset + size:x}")
            digest.update(chunk)
            remaining -= len(chunk)
    return digest.hexdigest()


def fnv1a64(text: str) -> int:
    value = FNV1A64_OFFSET
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * FNV1A64_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def fnv1a64_bytes(data: bytes) -> int:
    value = FNV1A64_OFFSET
    for byte in data:
        value ^= byte
        value = (value * FNV1A64_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def fnv1a32_bytes(data: bytes) -> int:
    value = FNV1A32_OFFSET
    for byte in data:
        value ^= byte
        value = (value * FNV1A32_PRIME) & 0xFFFFFFFF
    return value


def slot_file_name(slot: int, logical_path: str) -> str:
    return f"s{slot}_{fnv1a64(logical_path):016x}"


def parse_mjpeg_frame_sizes(data: bytes) -> list[int]:
    sizes: list[int] = []
    cursor = 0
    while cursor + 1 < len(data):
        start = data.find(b"\xff\xd8", cursor)
        if start < 0:
            break
        end = data.find(b"\xff\xd9", start + 2)
        if end < 0:
            raise ValueError("MJPEG frame is missing its EOI marker")
        sizes.append(end + 2 - start)
        cursor = end + 2
    if not sizes:
        raise ValueError("MJPEG stream contains no complete JPEG frames")
    if sum(sizes) != len(data):
        raise ValueError("MJPEG stream contains bytes outside complete JPEG frames")
    return sizes


def annex_b_nals(data: bytes) -> list[tuple[int, int, int]]:
    starts: list[tuple[int, int]] = []
    cursor = 0
    while cursor + 3 <= len(data):
        if data[cursor : cursor + 4] == b"\x00\x00\x00\x01":
            starts.append((cursor, 4))
            cursor += 4
        elif data[cursor : cursor + 3] == b"\x00\x00\x01":
            starts.append((cursor, 3))
            cursor += 3
        else:
            cursor += 1
    return [
        (start, prefix, starts[index + 1][0] if index + 1 < len(starts) else len(data))
        for index, (start, prefix) in enumerate(starts)
    ]


def _ue_bits(value: int) -> list[int]:
    code_num = value + 1
    leading_zeroes = code_num.bit_length() - 1
    return [0] * leading_zeroes + [
        (code_num >> shift) & 1 for shift in range(leading_zeroes, -1, -1)
    ]


def _rbsp_bytes(bits: list[int]) -> bytes:
    bits = [*bits, 1]
    bits.extend([0] * ((8 - len(bits) % 8) % 8))
    return bytes(
        sum(bit << (7 - index) for index, bit in enumerate(bits[offset : offset + 8]))
        for offset in range(0, len(bits), 8)
    )


def _add_emulation_prevention(data: bytes) -> bytes:
    output = bytearray()
    zero_count = 0
    for value in data:
        if zero_count >= 2 and value <= 3:
            output.append(3)
            zero_count = 0
        output.append(value)
        zero_count = zero_count + 1 if value == 0 else 0
    return bytes(output)


def p4_minimal_h264_sps(width: int, height: int) -> bytes:
    if width <= 0 or height <= 0 or width % 2 or height % 2:
        raise ValueError("P4 H.264 dimensions must be positive even values")
    encoded_width = (width + 15) & ~15
    encoded_height = (height + 15) & ~15
    crop_right = (encoded_width - width) // 2
    crop_bottom = (encoded_height - height) // 2
    has_crop = crop_right != 0 or crop_bottom != 0
    bits = [
        *_ue_bits(0),  # seq_parameter_set_id
        *_ue_bits(0),  # log2_max_frame_num_minus4
        *_ue_bits(2),  # pic_order_cnt_type
        *_ue_bits(1),  # max_num_ref_frames
        0,  # gaps_in_frame_num_value_allowed_flag
        *_ue_bits(encoded_width // 16 - 1),
        *_ue_bits(encoded_height // 16 - 1),
        1,  # frame_mbs_only_flag
        0,  # direct_8x8_inference_flag
        int(has_crop),
    ]
    if has_crop:
        bits.extend(
            [
                *_ue_bits(0),
                *_ue_bits(crop_right),
                *_ue_bits(0),
                *_ue_bits(crop_bottom),
            ]
        )
    bits.append(0)  # vui_parameters_present_flag
    rbsp = bytes((66, 0xC0, 30)) + _rbsp_bytes(bits)
    return b"\x67" + _add_emulation_prevention(rbsp)


def parse_p4_h264_stream(data: bytes, width: int, height: int) -> tuple[int, int]:
    expected_sps = p4_minimal_h264_sps(width, height)
    access_units = 0
    slices_in_access_unit = 0
    has_sps = False
    has_pps = False
    for start, prefix, end in annex_b_nals(data):
        payload_start = start + prefix
        if payload_start >= end:
            continue
        nal_type = data[payload_start] & 0x1F
        if nal_type in (1, 5):
            if access_units == 0:
                raise ValueError("P4 H.264 stream is missing access unit delimiters")
            slices_in_access_unit += 1
            if slices_in_access_unit > 1:
                raise ValueError(
                    "P4 H.264 stream must contain exactly one slice per access unit"
                )
        elif nal_type == 7:
            if data[payload_start:end] != expected_sps:
                raise ValueError("P4 H.264 stream uses an incompatible SPS")
            has_sps = True
        elif nal_type == 8:
            has_pps = True
        elif nal_type == 9:
            if access_units > 0 and slices_in_access_unit != 1:
                raise ValueError(
                    "P4 H.264 stream must contain exactly one slice per access unit"
                )
            access_units += 1
            slices_in_access_unit = 0
    if access_units <= 0 or slices_in_access_unit != 1:
        raise ValueError(
            "P4 H.264 stream must contain exactly one slice per access unit"
        )
    if not has_sps or not has_pps:
        raise ValueError("P4 H.264 stream is missing SPS, PPS, or video frames")
    return access_units, len(data)


def command_for_tool(path: Path) -> list[str]:
    return [sys.executable, str(path)] if path.suffix.lower() == ".py" else [str(path)]


def first_existing(candidates: Iterable[Path]) -> Path | None:
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate.resolve()
    return None


def resolve_spiffsgen(explicit: str | None) -> Path:
    configured = explicit or os.environ.get("SPIFFSGEN", "")
    home = Path.home()
    idf_path = Path(os.environ.get("IDF_PATH", "")).expanduser()
    candidates = []
    if configured:
        candidates.append(Path(configured).expanduser())
    if str(idf_path):
        candidates.append(idf_path / "components" / "spiffs" / "spiffsgen.py")
    candidates.append(
        home
        / ".platformio"
        / "packages"
        / "framework-espidf"
        / "components"
        / "spiffs"
        / "spiffsgen.py"
    )
    result = first_existing(candidates)
    if not result:
        raise FileNotFoundError(
            "ESP-IDF spiffsgen.py was not found. Install the PlatformIO ESP-IDF platform "
            "or set SPIFFSGEN."
        )
    return result


def resolve_esptool(explicit: str | None) -> Path:
    configured = explicit or os.environ.get("ESPTOOL", "")
    home = Path.home()
    candidates = []
    if configured:
        candidates.append(Path(configured).expanduser())
    for variable in ("PLATFORMIO_CORE_DIR", "HACHIMO_PLATFORMIO_CORE_DIR"):
        configured_core = os.environ.get(variable, "").strip()
        if not configured_core:
            continue
        core = Path(configured_core).expanduser()
        candidates.extend(
            [
                core / "penv" / "Scripts" / "esptool.exe",
                core / "penv" / "bin" / "esptool",
                core / "packages" / "tool-esptoolpy" / "esptool.py",
            ]
        )
    candidates.extend(
        [
            home / ".platformio" / "penv" / "Scripts" / "esptool.exe",
            home / ".platformio" / "penv" / "bin" / "esptool",
            home / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py",
        ]
    )
    for name in ("esptool", "esptool.py"):
        discovered = shutil.which(name)
        if discovered:
            candidates.append(Path(discovered))
    result = first_existing(candidates)
    if not result:
        raise FileNotFoundError(
            "esptool was not found. Install PlatformIO's tool-esptoolpy package or set ESPTOOL."
        )
    return result


def run_checked(command: Sequence[str], *, cwd: Path | None = None) -> None:
    env = os.environ.copy()
    env.setdefault("PYTHONUTF8", "1")
    env.setdefault("PYTHONIOENCODING", "utf-8")
    subprocess.run(list(command), cwd=cwd, env=env, check=True)


def find_project_dir(build_dir: Path, explicit: str | None) -> Path:
    if explicit:
        project_dir = Path(explicit).expanduser().resolve()
        if not (project_dir / "platformio.ini").is_file():
            raise FileNotFoundError(f"platformio.ini not found under {project_dir}")
        return project_dir
    for candidate in [build_dir.resolve(), *build_dir.resolve().parents]:
        if (candidate / "platformio.ini").is_file():
            return candidate
    raise FileNotFoundError("could not locate esp-p4-runtime from the build directory")


def reset_generated_directory(path: Path, build_dir: Path) -> None:
    resolved = path.resolve()
    root = build_dir.resolve()
    if root not in resolved.parents:
        raise RuntimeError(f"refusing to clear generated directory outside {root}: {resolved}")
    if resolved.exists():
        shutil.rmtree(resolved)
    resolved.mkdir(parents=True, exist_ok=True)


def load_factory_config(project_dir: Path, explicit: str | None) -> dict:
    path = Path(explicit).expanduser().resolve() if explicit else project_dir / FACTORY_CONFIG_NAME
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schemaVersion") != 1:
        raise ValueError(f"unsupported factory config schema in {path}")
    return data


def partition_entry(partitions_path: Path, label: str) -> tuple[int, int]:
    with partitions_path.open("r", encoding="utf-8", newline="") as handle:
        rows = csv.reader(line for line in handle if not line.lstrip().startswith("#"))
        for row in rows:
            values = [value.strip() for value in row]
            if values and values[0] == label and len(values) >= 5:
                return parse_int(values[3]), parse_int(values[4])
    raise ValueError(f"partition {label!r} not found in {partitions_path}")


def wav_matches_device_contract(path: Path, sample_rate: int) -> bool:
    try:
        with wave.open(str(path), "rb") as handle:
            return (
                handle.getnchannels() == 1
                and handle.getsampwidth() == 2
                and handle.getframerate() == sample_rate
                and handle.getcomptype() == "NONE"
                and handle.getnframes() > 0
            )
    except (wave.Error, EOFError):
        return False


def write_slot_file(
    spiffs_tree: Path,
    slot: int,
    logical_path: str,
    source: Path | None = None,
    data: bytes | None = None,
) -> Path:
    if (source is None) == (data is None):
        raise ValueError("provide exactly one of source or data")
    output = spiffs_tree / slot_file_name(slot, logical_path)
    if output.exists():
        raise RuntimeError(f"duplicate SPIFFS logical path: {logical_path}")
    if source is not None:
        shutil.copyfile(source, output)
    else:
        output.write_bytes(data or b"")
    return output


def compact_p4_component_json(
    widget_id: str,
    relative_path: str,
    source: Path,
    max_bytes: int,
) -> tuple[bytes, dict | list]:
    value = json.loads(source.read_text(encoding="utf-8"))
    if relative_path == "runtime/widget.json":
        if not isinstance(value, dict):
            raise ValueError(f"{source} must contain a JSON object")
        pages = value.get("pages")
        if isinstance(pages, list):
            for page in pages:
                if isinstance(page, dict):
                    page.pop("label", None)
        if widget_id == "token-usage":
            value.pop("readers", None)
            value.pop("fetchers", None)
    elif relative_path == "buttons.json" and not isinstance(value, list):
        raise ValueError(f"{source} must contain a JSON array")

    compact = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(compact) > max_bytes:
        raise ValueError(
            f"{source} is {len(compact)} bytes after compaction; P4 limit is {max_bytes}"
        )
    return compact, value


def write_spiffs_root_file(spiffs_tree: Path, name: str, data: bytes) -> Path:
    if not name or Path(name).name != name:
        raise ValueError(f"unsafe SPIFFS root file name: {name}")
    output = spiffs_tree / name
    if output.exists():
        raise RuntimeError(f"duplicate SPIFFS root file: {name}")
    output.write_bytes(data)
    return output


def build_builtin_component_tree(
    project_dir: Path,
    config: dict,
    spiffs_tree: Path,
) -> dict:
    component_config = config["components"]
    packages_dir = (project_dir / component_config["packagesDir"]).resolve()
    widget_max = parse_int(component_config["widgetMaxBytes"])
    buttons_max = parse_int(component_config["buttonsMaxBytes"])
    max_installed = parse_int(component_config["maxInstalled"])
    component_ids = component_config["ids"]
    active_id = str(component_config["defaultActiveId"])
    if not isinstance(component_ids, list) or not component_ids:
        raise ValueError("factory components.ids must be a non-empty array")
    if len(component_ids) > max_installed:
        raise ValueError(
            f"factory config has {len(component_ids)} components; device limit is {max_installed}"
        )
    if len(set(component_ids)) != len(component_ids):
        raise ValueError("factory components.ids contains duplicates")
    if active_id not in component_ids:
        raise ValueError("factory components.defaultActiveId must be present in ids")

    catalog_items = []
    package_bytes = 0
    for slot, widget_id_value in enumerate(component_ids):
        widget_id = str(widget_id_value)
        if not re.fullmatch(r"[a-z][a-z0-9_-]{0,46}", widget_id):
            raise ValueError(f"invalid P4 component id: {widget_id}")
        package_dir = packages_dir / widget_id
        component_path = package_dir / "component.json"
        widget_path = package_dir / "runtime" / "widget.json"
        buttons_path = package_dir / "buttons.json"
        for source in (component_path, widget_path, buttons_path):
            if not source.is_file():
                raise FileNotFoundError(f"built-in component file is missing: {source}")

        component = json.loads(component_path.read_text(encoding="utf-8"))
        if not isinstance(component, dict) or component.get("id") != widget_id:
            raise ValueError(f"{component_path} id does not match {widget_id}")
        widget_bytes, widget = compact_p4_component_json(
            widget_id, "runtime/widget.json", widget_path, widget_max
        )
        buttons_bytes, _ = compact_p4_component_json(
            widget_id, "buttons.json", buttons_path, buttons_max
        )
        dashboard = widget.get("dashboard")
        title = dashboard.get("title") if isinstance(dashboard, dict) else None
        if not isinstance(title, str) or not title.strip():
            title = component.get("name") or widget_id
        title = str(title).strip()
        if len(title.encode("utf-8")) >= 64:
            raise ValueError(f"component title exceeds the firmware catalog limit: {widget_id}")

        write_spiffs_root_file(spiffs_tree, f"p4w{slot:02d}.json", widget_bytes)
        write_spiffs_root_file(spiffs_tree, f"p4b{slot:02d}.json", buttons_bytes)
        package_bytes += len(widget_bytes) + len(buttons_bytes)
        catalog_items.append(
            {
                "widgetId": widget_id,
                "title": title,
                "slot": slot,
                "packageGeneration": 0,
                "widgetChecksum": f"{fnv1a32_bytes(widget_bytes):08x}",
                "buttonsChecksum": f"{fnv1a32_bytes(buttons_bytes):08x}",
            }
        )

    catalog_bytes = json.dumps(
        {
            "version": 2,
            "sequence": 1,
            "activeWidgetId": active_id,
            "items": catalog_items,
        },
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")
    if len(catalog_bytes) >= 4096:
        raise ValueError("generated P4 component catalog exceeds the firmware buffer")
    write_spiffs_root_file(spiffs_tree, MINIAPP_CATALOG_FILE, catalog_bytes)
    package_bytes += len(catalog_bytes)
    return {
        "count": len(catalog_items),
        "ids": component_ids,
        "defaultActiveId": active_id,
        "physicalFiles": len(catalog_items) * 2 + 1,
        "payloadBytes": package_bytes,
    }


def build_default_appearance_tree(
    project_dir: Path,
    build_dir: Path,
    config: dict,
) -> dict:
    appearance = config["appearance"]
    slot = parse_int(appearance["slot"])
    if slot not in (0, 1):
        raise ValueError("factory appearance slot must be 0 or 1")
    clips_dir = (project_dir / appearance["clipsDir"]).resolve()
    ready_profile = str(appearance.get("readyProfile", "")).strip()
    if not ready_profile:
        raise ValueError("factory appearance readyProfile is required")
    ready_root = clips_dir / "p4-ready" / ready_profile
    ready_manifest_path = ready_root / P4_MANIFEST_LOGICAL_PATH
    if not ready_manifest_path.is_file():
        raise FileNotFoundError(
            f"prebuilt P4 ready pack is missing: {ready_manifest_path}. "
            "Run npm run prepare:p4-assets before building the factory image."
        )
    ready_manifest_bytes = ready_manifest_path.read_bytes()
    ready_manifest = json.loads(ready_manifest_bytes.decode("utf-8"))
    expected_contract = {
        "format": "p4-h264-v1",
        "codec": "h264",
        "container": "annex-b",
        "width": parse_int(appearance["width"]),
        "height": parse_int(appearance["height"]),
        "fps": parse_int(appearance["fps"]),
    }
    for key, expected in expected_contract.items():
        if ready_manifest.get(key) != expected:
            raise ValueError(
                f"P4 ready manifest {key} mismatch: "
                f"{ready_manifest.get(key)!r} != {expected!r}"
            )
    expected_pack_id = str(appearance.get("packId", "")).strip()
    if not re.fullmatch(r"[0-9a-f]{64}", expected_pack_id):
        raise ValueError(
            "factory appearance packId must be 64 lowercase hexadecimal characters"
        )
    actual_pack_id = str(ready_manifest.get("packId", "")).strip()
    if actual_pack_id != expected_pack_id:
        raise ValueError(
            f"P4 ready manifest packId mismatch: {actual_pack_id!r} != {expected_pack_id!r}"
        )

    spiffs_tree = build_dir / SPIFFS_TREE_NAME
    reset_generated_directory(spiffs_tree, build_dir)
    (spiffs_tree / ACTIVE_SLOT_MARKER).write_text(f"{slot}\n", encoding="ascii")
    (spiffs_tree / SLOT_READY_MARKER_TEMPLATE.format(slot=slot)).write_text(
        f"{ready_manifest['packId']}\n", encoding="ascii"
    )
    logical_files = []
    raw_files: list[tuple[str, Path]] = []
    raw_payload_bytes = 0
    families = ready_manifest.get("families")
    if not isinstance(families, list) or not families:
        raise ValueError("P4 ready manifest contains no families")
    for family in families:
        logical_video = str(family.get("path", ""))
        if not logical_video.startswith("p4/") or ".." in logical_video or "\\" in logical_video:
            raise ValueError(f"invalid P4 ready video path: {logical_video}")
        ready_video = ready_root / logical_video
        ready_video_bytes = ready_video.read_bytes()
        frames, stream_bytes = parse_p4_h264_stream(
            ready_video_bytes,
            parse_int(appearance["width"]),
            parse_int(appearance["height"]),
        )
        if frames != parse_int(family.get("frames", 0)):
            raise ValueError(f"P4 ready frame index mismatch: {logical_video}")
        if stream_bytes != parse_int(family.get("streamBytes", 0)):
            raise ValueError(f"P4 ready stream size mismatch: {logical_video}")
        raw_files.append((logical_video, ready_video))
        logical_files.append(logical_video)
        raw_payload_bytes += len(ready_video_bytes)

        logical_audio = str(family.get("audioPath", "")).strip()
        if logical_audio:
            if not logical_audio.startswith("p4/") or ".." in logical_audio or "\\" in logical_audio:
                raise ValueError(f"invalid P4 ready audio path: {logical_audio}")
            ready_audio = ready_root / logical_audio
            if not wav_matches_device_contract(
                ready_audio, parse_int(appearance["audioSampleRate"])
            ):
                raise ValueError(f"invalid P4 ready audio: {logical_audio}")
            write_slot_file(spiffs_tree, slot, logical_audio, source=ready_audio)
            logical_files.append(logical_audio)

    write_slot_file(
        spiffs_tree,
        slot,
        P4_MANIFEST_LOGICAL_PATH,
        data=ready_manifest_bytes,
    )
    logical_files.append(P4_MANIFEST_LOGICAL_PATH)
    partition_path = project_dir / "partitions.csv"
    _, builtin_partition_bytes = partition_entry(
        partition_path, BUILTIN_APPEARANCE_PARTITION_LABEL
    )
    _, custom_partition_bytes = partition_entry(
        partition_path, CUSTOM_APPEARANCE_PARTITION_LABEL
    )
    builtin_capacity_bytes = builtin_partition_bytes - RAW_APPEARANCE_HEADER_BYTES
    custom_capacity_bytes = custom_partition_bytes - RAW_APPEARANCE_HEADER_BYTES
    required_raw_bytes = raw_payload_bytes + max(0, len(raw_files) - 1) * 3
    for label, capacity in (
        ("built-in", builtin_capacity_bytes),
        ("custom", custom_capacity_bytes),
    ):
        if required_raw_bytes > capacity:
            raise ValueError(
                f"P4 ready video payload exceeds the {label} appearance slot: "
                f"{required_raw_bytes} > {capacity}"
            )
    spiffs_payload_bytes = sum(
        path.stat().st_size for path in spiffs_tree.iterdir() if path.is_file()
    )
    return {
        "appearanceId": appearance["id"],
        "slot": slot,
        "families": len(families),
        "packId": ready_manifest["packId"],
        "readyProfile": ready_profile,
        "logicalFiles": logical_files,
        "physicalFiles": len(list(spiffs_tree.iterdir())) + len(raw_files),
        "payloadBytes": spiffs_payload_bytes + raw_payload_bytes,
        "spiffsPayloadBytes": spiffs_payload_bytes,
        "rawPayloadBytes": raw_payload_bytes,
        "rawCapacityBytes": min(builtin_capacity_bytes, custom_capacity_bytes),
        "rawFiles": raw_files,
        "tree": spiffs_tree,
    }


def build_raw_appearance_image(
    project_dir: Path,
    build_dir: Path,
    appearance_summary: dict,
) -> tuple[Path, int, int]:
    raw_files = appearance_summary["rawFiles"]
    if not raw_files or len(raw_files) > RAW_APPEARANCE_MAX_FILES:
        raise ValueError(
            f"factory raw appearance requires 1-{RAW_APPEARANCE_MAX_FILES} video files"
        )
    offset, size = partition_entry(
        project_dir / "partitions.csv", BUILTIN_APPEARANCE_PARTITION_LABEL
    )
    output = build_dir / RAW_APPEARANCE_IMAGE_NAME
    image = bytearray(b"\xff" * size)
    entries: list[tuple[int, int, int, int]] = []
    cursor = RAW_APPEARANCE_HEADER_BYTES
    for logical_path, source in raw_files:
        cursor = (cursor + 3) & ~3
        data = source.read_bytes()
        if cursor + len(data) > size:
            raise ValueError(
                f"factory raw appearance exceeds partition at {logical_path}"
            )
        image[cursor : cursor + len(data)] = data
        entries.append(
            (fnv1a64(logical_path), cursor, len(data), fnv1a64_bytes(data))
        )
        cursor += len(data)

    header = memoryview(image)[:RAW_APPEARANCE_HEADER_BYTES]
    header[:8] = b"P4RAW01\0"
    struct.pack_into("<IIII", header, 8, 1, len(entries), cursor, 0)
    pack_id = str(appearance_summary["packId"]).encode("ascii")
    if len(pack_id) != 64:
        raise ValueError("factory raw appearance packId must be 64 lowercase hex bytes")
    header[24:88] = pack_id
    for index, entry in enumerate(entries):
        entry_offset = (
            RAW_APPEARANCE_HEADER_ENTRY_OFFSET
            + index * RAW_APPEARANCE_HEADER_ENTRY_BYTES
        )
        struct.pack_into("<QIIQ", header, entry_offset, *entry)

    output.write_bytes(image)
    return output, offset, size


def build_spiffs_image(
    project_dir: Path,
    build_dir: Path,
    config: dict,
    tree: Path,
    spiffsgen: Path,
) -> tuple[Path, int, int]:
    spiffs = config["spiffs"]
    offset, size = partition_entry(
        project_dir / "partitions.csv", str(spiffs["partitionLabel"])
    )
    output = build_dir / SPIFFS_IMAGE_NAME
    if output.exists():
        output.unlink()
    run_checked(
        [
            *command_for_tool(spiffsgen),
            "--page-size",
            str(parse_int(spiffs["pageSize"])),
            "--block-size",
            str(parse_int(spiffs["blockSize"])),
            "--obj-name-len",
            str(parse_int(spiffs["objectNameLength"])),
            "--meta-len",
            str(parse_int(spiffs["metadataLength"])),
            str(size),
            str(tree),
            str(output),
        ]
    )
    if output.stat().st_size != size:
        raise RuntimeError(
            f"SPIFFS image size mismatch: expected {size}, got {output.stat().st_size}"
        )
    return output, offset, size


def resolve_flash_segments(build_dir: Path) -> tuple[dict, list[tuple[int, Path]]]:
    flasher_path = build_dir / "flasher_args.json"
    flasher = json.loads(flasher_path.read_text(encoding="utf-8"))
    aliases = {
        "bootloader": "bootloader.bin",
        "partition-table": "partitions.bin",
        "otadata": "ota_data_initial.bin",
        "app": "firmware.bin",
    }
    segments = []
    for role in ("bootloader", "partition-table", "app", "otadata"):
        descriptor = flasher.get(role) or {}
        if "offset" not in descriptor:
            raise ValueError(f"{role} is missing from {flasher_path}")
        source = build_dir / str(descriptor.get("file", ""))
        if not source.is_file():
            source = build_dir / aliases[role]
        if not source.is_file():
            raise FileNotFoundError(f"missing {role} image under {build_dir}")
        segments.append((parse_int(descriptor["offset"]), source.resolve()))
    return flasher, segments


def validate_segments(segments: list[tuple[int, Path]], flash_size: int) -> None:
    cursor = 0
    for offset, source in sorted(segments):
        if offset < cursor:
            raise ValueError(f"flash segment overlaps before 0x{offset:x}: {source}")
        cursor = offset + source.stat().st_size
    if cursor > flash_size:
        raise ValueError(
            f"factory image ends at 0x{cursor:x}, beyond flash size 0x{flash_size:x}"
        )


def project_version(project_dir: Path) -> str:
    cmake = (project_dir / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r'set\s*\(\s*PROJECT_VER\s+"([^"]+)"', cmake)
    return match.group(1) if match else "unknown"


def build_factory_image(
    project_dir: Path,
    build_dir: Path,
    config: dict,
    appearance_summary: dict,
    component_summary: dict,
    spiffs_image: Path,
    spiffs_offset: int,
    raw_appearance_image: Path,
    raw_appearance_offset: int,
    esptool: Path,
    output: Path,
) -> dict:
    flasher, segments = resolve_flash_segments(build_dir)
    settings = flasher["flash_settings"]
    flash_size = parse_flash_size(str(settings["flash_size"]))
    segments.append((spiffs_offset, spiffs_image.resolve()))
    segments.append((raw_appearance_offset, raw_appearance_image.resolve()))
    segments.sort(key=lambda item: item[0])
    validate_segments(segments, flash_size)
    if output.exists():
        output.unlink()

    command = [
        *command_for_tool(esptool),
        "--chip",
        str(flasher.get("extra_esptool_args", {}).get("chip", "esp32p4")),
        "merge-bin",
        "--flash-mode",
        str(settings["flash_mode"]),
        "--flash-freq",
        str(settings["flash_freq"]),
        "--flash-size",
        str(settings["flash_size"]),
        "--output",
        str(output),
    ]
    for offset, source in segments:
        command.extend([f"0x{offset:x}", str(source)])
    run_checked(command, cwd=project_dir)

    if sha256_region(output, spiffs_offset, spiffs_image.stat().st_size) != sha256_file(
        spiffs_image
    ):
        raise RuntimeError("merged factory image does not contain the expected SPIFFS image")
    if sha256_region(
        output,
        raw_appearance_offset,
        raw_appearance_image.stat().st_size,
    ) != sha256_file(raw_appearance_image):
        raise RuntimeError(
            "merged factory image does not contain the expected built-in appearance image"
        )

    segment_records = [
        {
            "offset": f"0x{offset:x}",
            "path": source.name,
            "bytes": source.stat().st_size,
            "sha256": sha256_file(source),
        }
        for offset, source in segments
    ]
    summary = {
        "schemaVersion": 1,
        "format": FACTORY_FORMAT,
        "completeInstall": True,
        "project": "pet_manager_p4_runtime",
        "version": project_version(project_dir),
        "chip": str(flasher.get("extra_esptool_args", {}).get("chip", "esp32p4")),
        "flashOffset": "0x0",
        "flashSize": str(settings["flash_size"]),
        "image": output.name,
        "bytes": output.stat().st_size,
        "sha256": sha256_file(output),
        "appearance": {
            key: value
            for key, value in appearance_summary.items()
            if key not in ("tree", "logicalFiles", "rawFiles")
        },
        "components": component_summary,
        "spiffsPayloadBytes": (
            appearance_summary["spiffsPayloadBytes"]
            + component_summary["payloadBytes"]
        ),
        "segments": segment_records,
    }
    manifest_path = output.with_name(FACTORY_MANIFEST_NAME)
    manifest_path.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return summary


def flash_factory_image(
    esptool: Path,
    image: Path,
    chip: str,
    port: str,
    baud: int,
) -> None:
    if not port.strip():
        raise ValueError("factory upload requires an explicit serial port")
    run_checked(
        [
            *command_for_tool(esptool),
            "--chip",
            chip,
            "--port",
            port,
            "--baud",
            str(baud),
            "--connect-attempts",
            "5",
            "--before",
            "default-reset",
            "--after",
            "no-reset",
            "erase-flash",
        ]
    )
    run_checked(
        [
            *command_for_tool(esptool),
            "--chip",
            chip,
            "--port",
            port,
            "--baud",
            str(baud),
            "--connect-attempts",
            "5",
            "--before",
            "default-reset",
            "--after",
            "hard-reset",
            "write-flash",
            "-z",
            "0x0",
            str(image),
        ]
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build one ESP32-P4 factory image with the built-in Terrier appearance "
            "and P4 component catalog."
        )
    )
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--project-dir")
    parser.add_argument("--config")
    parser.add_argument("--spiffsgen")
    parser.add_argument("--esptool")
    parser.add_argument("--output")
    parser.add_argument("--flash-port")
    parser.add_argument("--flash-baud", type=int, default=921600)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    build_dir = Path(args.build_dir).expanduser().resolve()
    project_dir = find_project_dir(build_dir, args.project_dir)
    config = load_factory_config(project_dir, args.config)
    spiffsgen = resolve_spiffsgen(args.spiffsgen)
    esptool = resolve_esptool(args.esptool)
    output = (
        Path(args.output).expanduser().resolve()
        if args.output
        else build_dir / FACTORY_IMAGE_NAME
    )

    print(f"[factory] project={project_dir}")
    print(f"[factory] build={build_dir}")
    appearance = build_default_appearance_tree(project_dir, build_dir, config)
    print(
        "[factory] appearance "
        f"families={appearance['families']} files={appearance['physicalFiles']} "
        f"payload={appearance['payloadBytes']}"
    )
    components = build_builtin_component_tree(
        project_dir, config, appearance["tree"]
    )
    print(
        "[factory] components "
        f"count={components['count']} files={components['physicalFiles']} "
        f"payload={components['payloadBytes']}"
    )
    spiffs_image, spiffs_offset, _ = build_spiffs_image(
        project_dir,
        build_dir,
        config,
        appearance["tree"],
        spiffsgen,
    )
    raw_appearance_image, raw_appearance_offset, _ = build_raw_appearance_image(
        project_dir,
        build_dir,
        appearance,
    )
    summary = build_factory_image(
        project_dir,
        build_dir,
        config,
        appearance,
        components,
        spiffs_image,
        spiffs_offset,
        raw_appearance_image,
        raw_appearance_offset,
        esptool,
        output,
    )
    print(f"[factory] image={output}")
    print(f"[factory] bytes={summary['bytes']}")
    print(f"[factory] sha256={summary['sha256']}")
    if args.flash_port:
        flash_factory_image(
            esptool,
            output,
            str(summary["chip"]),
            args.flash_port,
            args.flash_baud,
        )
        print(f"[factory] flashed={args.flash_port}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
