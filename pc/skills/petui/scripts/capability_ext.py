"""Track C capability expansion validators: p4-sequence-v1, p4-samples-v1, p4-clip-v1,
p4-big-image-v1 and p4-events-v1. Pure declarative checks shared (through the
contracts/*.json vectors) with the firmware parser and the Rust validator.
`strip()` returns the runtime without these extensions so the unchanged surface and
runtime validators keep working on the remaining declaration."""
from __future__ import annotations
import copy
import re
import struct
from pathlib import Path
from typing import Any

SEQUENCE_ENGINE = "p4-sequence-v1"
SAMPLES_ENGINE = "p4-samples-v1"
CLIP_ENGINE = "p4-clip-v1"
BIG_IMAGE_ENGINE = "p4-big-image-v1"
EVENTS_ENGINE = "p4-events-v1"
CAPABILITIES = {"sequence": ("widgetSequence", SEQUENCE_ENGINE), "samples": ("widgetSamples", SAMPLES_ENGINE),
                "clip": ("widgetClip", CLIP_ENGINE), "big": ("widgetBigImage", BIG_IMAGE_ENGINE), "events": ("widgetEvents", EVENTS_ENGINE)}
WAVES = ("square", "triangle", "noise")
SEQ_BPM = (40, 240)
SEQ_MIDI = (36, 96)
SEQ_BEATS = (1, 16)
SEQ_TRACKS = 2
SEQ_NOTES = 64
SAMPLE_ITEMS = 4
SAMPLE_BYTES = 64 * 1024
SAMPLE_BUDGET = 256 * 1024
SAMPLE_RATE = 16000
CLIP_SIZES = ((160, 120), (320, 240))
CLIP_FPS = 15
CLIP_SECONDS = 5
CLIP_BYTES = 512 * 1024
CLIP_SOURCE_BYTES = 4 * 1024 * 1024
BIG_IMAGES = 2
BIG_TILE = 240
BIG_TILES = 16
BIG_MAX = (1280, 960)
BIG_BUDGET = 1024 * 1024
EVENT_NAMES = 8
EVENT_QUEUE = 8
ALARMS = 4
ALARM_MIN_MS = 60000
ALARM_MAX_MS = 7 * 86400000
ALARM_TEXT_BYTES = 40
NAME = re.compile(r"^[A-Za-z0-9_.-]{1,15}$")
AUDIO_OWNERS = ("instrument", "media", "local", "program")


def _int(v: Any, lo: int, hi: int) -> bool:
    return type(v) is int and lo <= v <= hi


def _asset_id(asset: Any, ext: str) -> str | None:
    if not isinstance(asset, str) or not asset.startswith("assets/") or "/" in asset[7:] or not asset.endswith(ext):
        return None
    name = asset[7:-len(ext)]
    return name if NAME.fullmatch(name) else None


def event_action(on: Any) -> bool:
    return isinstance(on, str) and on.startswith("event:") and NAME.fullmatch(on[6:]) is not None


def big_tiles(width: int, height: int) -> int:
    return -(-width // BIG_TILE) * -(-height // BIG_TILE)


def _runtime_v4(runtime: dict) -> bool:
    return runtime.get("engine") == "p4-bounded-runtime-v4"


def _ops(runtime: dict, location: str, obj: Any, errors: list[str], seq: bool, samples: list[str], emits: list[str]) -> None:
    if not isinstance(obj, dict):
        return
    if "sequence" in obj and (not seq or obj["sequence"] not in ("play", "stop")):
        errors.append(f"{location}.sequence 需要声明 sequence={SEQUENCE_ENGINE} 且只接受 play/stop")
    if "sample" in obj and obj["sample"] not in samples:
        errors.append(f"{location}.sample 需要声明 samples={SAMPLES_ENGINE} 并引用已声明 id")
    if "emit" in obj and obj["emit"] not in emits:
        errors.append(f"{location}.emit 需要声明 events={EVENTS_ENGINE} 并引用已声明名称")


def validate_capability_ext(runtime: Any, capabilities: dict[str, Any], errors: list[str]) -> None:
    if not isinstance(runtime, dict):
        return
    v4 = _runtime_v4(runtime)
    exclusive = any(k in runtime for k in AUDIO_OWNERS)
    seq = False
    sample_ids: list[str] = []
    emit_names: list[str] = []
    used: set[str] = set()
    if "sequence" in runtime:
        used.add("sequence")
        s = runtime["sequence"]
        ok = isinstance(s, dict) and set(s) <= {"engine", "bpm", "volume", "tracks", "loop"} and s.get("engine") == SEQUENCE_ENGINE and v4 and not exclusive
        ok = ok and _int(s.get("bpm"), *SEQ_BPM) and _int(s.get("volume"), 0, 100) and isinstance(s.get("loop", False), bool)
        tracks = s.get("tracks") if isinstance(s, dict) else None
        ok = ok and isinstance(tracks, list) and 1 <= len(tracks) <= SEQ_TRACKS
        if ok:
            for t in tracks:
                notes = t.get("notes") if isinstance(t, dict) else None
                if (not isinstance(t, dict) or set(t) != {"wave", "volume", "notes"} or t["wave"] not in WAVES or not _int(t["volume"], 0, 100)
                        or not isinstance(notes, list) or not 1 <= len(notes) <= SEQ_NOTES
                        or any(not isinstance(n, list) or len(n) != 2 or not (n[0] == 0 or _int(n[0], *SEQ_MIDI)) or type(n[0]) is not int or not _int(n[1], *SEQ_BEATS) for n in notes)):
                    ok = False
        if not ok:
            errors.append(f"runtime/widget.json.sequence 需要 engine={SEQUENCE_ENGINE}、bpm 40-240、volume 0-100、1-2 条轨道（wave square/triangle/noise，1-64 个 [midi 36-96|0, beats 1-16]），且不与 instrument/media/local/program 混用")
        else:
            seq = True
        if capabilities.get("widgetSequence") != SEQUENCE_ENGINE:
            errors.append("设备未声明 widgetSequence=p4-sequence-v1，请先升级固件")
    if "samples" in runtime:
        used.add("samples")
        s = runtime["samples"]
        ok = isinstance(s, dict) and set(s) <= {"engine", "volume", "items"} and s.get("engine") == SAMPLES_ENGINE and v4 and not exclusive and _int(s.get("volume"), 0, 100)
        items = s.get("items") if isinstance(s, dict) else None
        ok = ok and isinstance(items, list) and 1 <= len(items) <= SAMPLE_ITEMS
        if ok:
            for it in items:
                if not isinstance(it, dict) or set(it) != {"id", "asset"} or not isinstance(it["id"], str) or NAME.fullmatch(it["id"]) is None or _asset_id(it["asset"], ".wav") != it["id"] or it["id"] in sample_ids:
                    ok = False
                    break
                sample_ids.append(it["id"])
        if not ok:
            sample_ids = []
            errors.append(f"runtime/widget.json.samples 需要 engine={SAMPLES_ENGINE}、volume 0-100 与 1-4 个唯一 {{id, asset: assets/<id>.wav}}，且不与 instrument/media/local/program 混用")
        if capabilities.get("widgetSamples") != SAMPLES_ENGINE:
            errors.append("设备未声明 widgetSamples=p4-samples-v1，请先升级固件")
    events = False
    if "events" in runtime:
        used.add("events")
        e = runtime["events"]
        emit = e.get("emit", []) if isinstance(e, dict) else None
        ok = isinstance(e, dict) and set(e) <= {"engine", "emit"} and e.get("engine") == EVENTS_ENGINE and v4 and isinstance(emit, list) and len(emit) <= EVENT_NAMES
        ok = ok and all(isinstance(n, str) and NAME.fullmatch(n) for n in emit) and len(set(emit)) == len(emit)
        if not ok:
            errors.append(f"runtime/widget.json.events 需要 engine={EVENTS_ENGINE} 与最多 8 个唯一安全事件名（1-15 位 A-Za-z0-9_.-）")
        else:
            events = True
            emit_names = list(emit)
        if capabilities.get("widgetEvents") != EVENTS_ENGINE:
            errors.append("设备未声明 widgetEvents=p4-events-v1，请先升级固件")
    transitions = runtime.get("transitions") if isinstance(runtime.get("transitions"), list) else []
    triggers = {t["on"] for t in transitions if isinstance(t, dict) and isinstance(t.get("on"), str)}
    subscriptions = [t["on"][6:] for t in transitions if isinstance(t, dict) and isinstance(t.get("on"), str) and t["on"].startswith("event:")]
    if subscriptions and (not events or len(set(subscriptions)) > EVENT_NAMES or any(NAME.fullmatch(n) is None for n in subscriptions)):
        errors.append(f"transitions.on 使用 event:<name> 需要声明 events={EVENTS_ENGINE}，最多 8 个不同名称")
    for i, t in enumerate(transitions):
        _ops(runtime, f"transitions[{i}]", t, errors, seq, sample_ids, emit_names)
        if isinstance(t, dict) and isinstance(t.get("else"), dict) and any(k in t["else"] for k in ("sequence", "sample", "emit")):
            errors.append(f"transitions[{i}].else 不接受 sequence/sample/emit")
    for i, t in enumerate(runtime.get("tick") if isinstance(runtime.get("tick"), list) else []):
        if isinstance(t, dict):
            _ops(runtime, f"tick[{i}].then", t.get("then"), errors, seq, sample_ids, emit_names)
            if any(k in t for k in ("sequence", "sample", "emit")):
                errors.append(f"tick[{i}] 的 sequence/sample/emit 只能放在 then 中")
    if "alarms" in runtime:
        used.add("alarms")
        alarms = runtime["alarms"]
        variables = runtime.get("vars") if isinstance(runtime.get("vars"), dict) else {}
        ok = events and isinstance(runtime.get("persist"), dict) and isinstance(alarms, list) and 1 <= len(alarms) <= ALARMS
        ids: list[str] = []
        if ok:
            for a in alarms:
                if not isinstance(a, dict) or not set(a) <= {"id", "every_ms", "at_ms_var", "action", "background", "text"} or not isinstance(a.get("id"), str) or NAME.fullmatch(a["id"]) is None or a["id"] in ids:
                    ok = False
                    break
                ids.append(a["id"])
                periodic = "every_ms" in a
                if periodic == ("at_ms_var" in a) or not isinstance(a.get("action"), str) or not (re.fullmatch(r"[A-Za-z0-9_.-]{1,47}", a["action"]) or event_action(a["action"])) or a["action"] not in triggers or not isinstance(a.get("background", False), bool):
                    ok = False
                    break
                if periodic and not _int(a["every_ms"], ALARM_MIN_MS, ALARM_MAX_MS):
                    ok = False
                    break
                if not periodic and (not isinstance(a["at_ms_var"], str) or not isinstance(variables.get(a["at_ms_var"]), dict) or variables[a["at_ms_var"]].get("type") != "int"):
                    ok = False
                    break
                text = a.get("text", "")
                if not isinstance(text, str) or len(text.encode()) > ALARM_TEXT_BYTES:
                    ok = False
                    break
        if not ok:
            errors.append(f"runtime/widget.json.alarms 需要 events={EVENTS_ENGINE} 与 persist，1-4 个 {{id, every_ms>=60000 | at_ms_var:<int var>, action:<已声明 on>, background?, text<=40 字节}}")
    surface = runtime.get("surface")
    if isinstance(surface, dict):
        app = surface.get("engine") == "p4-app-surface-v1" and surface.get("graphics") == "p4-app-graphics-v1"
        bindings = surface.get("bindings") == "p4-variable-bindings-v1"
        elements = surface.get("elements") if isinstance(surface.get("elements"), list) else []
        clips = [n for n in elements if isinstance(n, dict) and n.get("type") == "clip"]
        if clips or any(isinstance(n, dict) and "asset" in n for n in elements):
            ok = app and bindings and len(clips) == 1 and all("asset" not in n or n.get("type") == "clip" for n in elements if isinstance(n, dict))
            for n in clips:
                ok = ok and set(n) <= {"type", "x", "y", "w", "h", "color", "asset", "page", "state_mask", "host_mask", "show"} and (n.get("w"), n.get("h")) in CLIP_SIZES and _asset_id(n.get("asset"), ".mp4") is not None
            if not ok:
                errors.append(f"surface clip 节点需要 p4-app-graphics-v1 + p4-variable-bindings-v1、恰好 1 个 {{type:clip, w/h 160x120|320x240, asset: assets/<id>.mp4}}，不接受 action/motion/tile")
            if capabilities.get("widgetClip") != CLIP_ENGINE:
                errors.append("设备未声明 widgetClip=p4-clip-v1，请先升级固件")
        bigs = surface.get("big_images")
        big_nodes = [n for n in elements if isinstance(n, dict) and ("big" in n or "offset_x_var" in n or "offset_y_var" in n)]
        if bigs is not None or big_nodes:
            variables = runtime.get("vars") if isinstance(runtime.get("vars"), dict) else {}
            declared: dict[str, tuple[int, int]] = {}
            ok = app and bindings and isinstance(bigs, list) and 1 <= len(bigs) <= BIG_IMAGES
            if ok:
                for b in bigs:
                    if (not isinstance(b, dict) or set(b) != {"id", "asset", "width", "height"} or not isinstance(b["id"], str) or NAME.fullmatch(b["id"]) is None
                            or _asset_id(b["asset"], ".png") != b["id"] or b["id"] in declared or not _int(b["width"], BIG_TILE + 1, BIG_MAX[0]) or not _int(b["height"], 1, BIG_MAX[1])
                            or big_tiles(b["width"], b["height"]) > BIG_TILES):
                        ok = False
                        break
                    declared[b["id"]] = (b["width"], b["height"])
            if ok:
                for n in big_nodes:
                    if (n.get("type") != "image" or "image" in n or not {"big", "offset_x_var", "offset_y_var"} <= set(n) or n.get("big") not in declared
                            or any(k in n for k in ("tile", "rotate_ms", "motion", "action", "in_view", "view"))
                            or not all(isinstance(n[k], str) and isinstance(variables.get(n[k]), dict) and variables[n[k]].get("type") == "int" for k in ("offset_x_var", "offset_y_var"))
                            or not _int(n.get("w"), 1, declared[n["big"]][0]) or not _int(n.get("h"), 1, declared[n["big"]][1])):
                        ok = False
                        break
            if not ok:
                errors.append("surface.big_images 需要 p4-app-graphics-v1 + p4-variable-bindings-v1、1-2 个 {id, asset: assets/<id>.png, width 241-1280, height 1-960} 且 ceil(w/240)*ceil(h/240)<=16；image 节点用 big + 两个 int offset 变量引用，窗口不大于图片")
            if capabilities.get("widgetBigImage") != BIG_IMAGE_ENGINE:
                errors.append("设备未声明 widgetBigImage=p4-big-image-v1，请先升级固件")


def strip(runtime: Any) -> Any:
    """Runtime with Track C extensions removed so the unchanged validators see the base declaration."""
    if not isinstance(runtime, dict):
        return runtime
    out = copy.deepcopy(runtime)
    for key in ("sequence", "samples", "events", "alarms"):
        out.pop(key, None)
    for t in out.get("transitions", []) if isinstance(out.get("transitions"), list) else []:
        if isinstance(t, dict):
            for k in ("sequence", "sample", "emit"):
                t.pop(k, None)
    for t in out.get("tick", []) if isinstance(out.get("tick"), list) else []:
        if isinstance(t, dict) and isinstance(t.get("then"), dict):
            for k in ("sequence", "sample", "emit"):
                t["then"].pop(k, None)
    surface = out.get("surface")
    if isinstance(surface, dict):
        surface.pop("big_images", None)
        elements = surface.get("elements")
        if isinstance(elements, list):
            for n in elements:
                if not isinstance(n, dict):
                    continue
                if n.get("type") == "clip" or "big" in n:
                    keep = {k: n[k] for k in ("x", "y", "w", "h", "color", "page", "state_mask", "host_mask", "show") if k in n}
                    n.clear()
                    n.update({"type": "panel", **keep})
    return out


def media_files(runtime: Any) -> dict[str, str]:
    """asset path -> kind (wav|mp4|png) for every declared Track C media file."""
    files: dict[str, str] = {}
    if not isinstance(runtime, dict):
        return files
    for it in runtime.get("samples", {}).get("items", []) if isinstance(runtime.get("samples"), dict) and isinstance(runtime["samples"].get("items"), list) else []:
        if isinstance(it, dict) and isinstance(it.get("asset"), str):
            files[it["asset"]] = "wav"
    surface = runtime.get("surface") if isinstance(runtime.get("surface"), dict) else {}
    for n in surface.get("elements", []) if isinstance(surface.get("elements"), list) else []:
        if isinstance(n, dict) and n.get("type") == "clip" and isinstance(n.get("asset"), str):
            files[n["asset"]] = "mp4"
    for b in surface.get("big_images", []) if isinstance(surface.get("big_images"), list) else []:
        if isinstance(b, dict) and isinstance(b.get("asset"), str):
            files[b["asset"]] = "png"
    return files


def wav_info(data: bytes) -> tuple[int, int, int, int] | None:
    """(sample_rate, channels, bits, frames) from a RIFF/WAVE header, or None."""
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    at, fmt, frames = 12, None, None
    while at + 8 <= len(data):
        cid, size = data[at:at + 4], struct.unpack("<I", data[at + 4:at + 8])[0]
        if size > len(data) - at - 8:
            return None
        body = data[at + 8:at + 8 + size]
        if cid == b"fmt " and len(body) >= 16:
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag not in (1, 3, 0xFFFE) or channels < 1 or rate < 8000 or bits not in (8, 16, 24, 32):
                return None
            fmt = (rate, channels, bits)
        elif cid == b"data" and fmt:
            frames = len(body) // (fmt[1] * fmt[2] // 8)
        at += 8 + size + (size & 1)
    return (*fmt, frames) if fmt and frames else None


def validate_media_files(widget_dir: Path, runtime: Any, errors: list[str]) -> None:
    """Offline file checks; the PC compiles samples/clips with ffmpeg at install time."""
    for asset, kind in media_files(runtime).items():
        path = widget_dir / asset
        limit = CLIP_SOURCE_BYTES
        try:
            if _asset_id(asset, {"wav": ".wav", "mp4": ".mp4", "png": ".png"}[kind]) is None or not path.resolve().is_relative_to(widget_dir.resolve()):
                errors.append(f"非法素材路径: {asset}")
                continue
            with path.open("rb") as source:
                data = source.read(limit + 1)
            if len(data) > limit:
                errors.append(f"{asset} 源素材超过 4 MiB")
                continue
        except OSError:
            errors.append(f"缺少素材: {asset}")
            continue
        if kind == "wav":
            info = wav_info(data)
            if not info:
                errors.append(f"{asset} 必须是 RIFF/WAVE PCM 文件（PC 会重采样为 16 kHz 单声道 16 位）")
            elif info[3] * SAMPLE_RATE // info[0] * 2 > SAMPLE_BYTES:
                errors.append(f"{asset} 重采样后超过 64 KiB（最长约 2.0 s）")
        elif kind == "mp4":
            if len(data) > CLIP_SOURCE_BYTES or data[4:8] != b"ftyp":
                errors.append(f"{asset} 必须是不超过 4 MiB 的 MP4 容器（PC 压制为 15 fps、不超过 5 s、512 KiB 的 H.264）")
        elif kind == "png":
            if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
                errors.append(f"{asset} 必须是 PNG")
            else:
                width, height = struct.unpack(">II", data[16:24])
                declared = next((b for b in runtime["surface"]["big_images"] if isinstance(b, dict) and b.get("asset") == asset), {})
                if (width, height) != (declared.get("width"), declared.get("height")):
                    errors.append(f"{asset} 的 PNG 尺寸与 big_images 声明不一致")


def internal_actions(runtime: Any) -> set[str]:
    """Only validated subscribers and exact declared alarm targets are internal.

    event.emit is an emission allowlist, not a subscriber grant. Subscriptions
    are the valid event:<name> transition declarations in this exact runtime.
    Invalid profiles never gain a physical-binding exemption.
    """
    if not isinstance(runtime, dict):
        return set()
    errors: list[str] = []
    validate_capability_ext(runtime, {v[0]: v[1] for v in CAPABILITIES.values()}, errors)
    if errors or runtime.get("engine") != "p4-bounded-runtime-v4" or runtime.get("events", {}).get("engine") != EVENTS_ENGINE:
        return set()
    transitions = runtime.get("transitions", [])
    names = {t["on"] for t in transitions if isinstance(t, dict) and isinstance(t.get("on"), str)} if isinstance(transitions, list) else set()
    out = {name for name in names if event_action(name)}
    alarms = runtime.get("alarms", [])
    if isinstance(alarms, list):
        out.update(a["action"] for a in alarms if isinstance(a, dict) and isinstance(a.get("action"), str) and a["action"] in names)
    return out
