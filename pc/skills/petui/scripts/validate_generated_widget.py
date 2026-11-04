#!/usr/bin/env python3
"""Validate generated petui packages, including modern scenes, sprites, joystick, and global-exit isolation."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable, Optional
from device_json import strings_valid_text,value_within_depth,DEVICE_JSON_MAX_DEPTH


REQUIRED_FILES = {
    "component.json",
    "negative-screen.json",
    "buttons.json",
    "runtime/widget.json",
    "share.json",
}
FORBIDDEN_SUFFIXES = {
    ".bat",
    ".cmd",
    ".com",
    ".css",
    ".dll",
    ".dylib",
    ".exe",
    ".html",
    ".js",
    ".mjs",
    ".cjs",
    ".ps1",
    ".py",
    ".sh",
    ".so",
    ".svg",
}
ALLOWED_KINDS = {"game", "tool"}
ALLOWED_EVENTS = {
    "button.sw1.short_press",
    "button.sw2.short_press",
    "button.sw3.short_press",
    "button.encoder.short_press",
    "button.encoder.long_press",
    "knob.rotate_cw",
    "knob.rotate_ccw",
    "joystick.up",
    "joystick.down",
    "screen.region.tap",
    "screen.region.long_press",
}
ALLOWED_STYLES = {"pixel", "clean"}
ALLOWED_PALETTES = {
    "candy", "sunset", "mint", "arcade", "ocean", "forest", "ember", "mono"
}
ALLOWED_LAYOUTS = {"arcade", "scoreboard", "tool"}
ALLOWED_SCENE_SHAPES = {
    "rect", "player-ship", "enemy-ship", "bullet", "star", "paddle", "ball",
    "circle", "capsule", "triangle", "diamond", "heart", "cloud", "coin", "character",
}
ALLOWED_SPRITES = {
    "music",
    "target",
    "trophy",
    "star",
    "bolt",
    "coffee",
    "timer",
    "droplet",
    "gauge",
    "blocks",
    "snake",
    "flappy",
    "mole-ready",
    "mole-left",
    "mole-center",
    "mole-right",
}
DASHBOARD_LIMITS = {
    "title": 60,
    "eyebrow": 90,
    "headline": 156,
    "metricLabel": 90,
    "metricValue": 60,
    "metricUnit": 30,
    "badge": 12,
    "note": 156,
    "footer": 156,
    "progress": 64,
    "visualStyle": 16,
    "visualPalette": 16,
    "visualLayout": 16,
    "visualSprite": 16,
}
ALLOWED_GAME_ACTIONS = {
    "blocks": {"start", "left", "right", "rotate", "drop"},
    "snake": {"start", "left", "right"},
    "flappy": {"flap"},
    # gomoku: place doubles as start/restart (flappy convention); undo takes back the last pair.
    "gomoku": {"place", "left", "right", "up", "down", "undo"},
}
RUNTIME_ENGINE_V3 = "p4-bounded-runtime-v3"
RUNTIME_ENGINE = "p4-bounded-runtime-v4"
RUNTIME_ENGINES = {RUNTIME_ENGINE_V3, RUNTIME_ENGINE}
SCENE_ENGINE_V1 = "p4-grid-scene-v1"
SCENE_ENGINE = "p4-grid-scene-v2"
SCENE_ENGINES = {SCENE_ENGINE_V1, SCENE_ENGINE}
SCENE_MAX_ENTITIES = 12
SCENE_MAX_RULES = 20
SCENE_MAX_OPS = 4
SCENE_MAX_SPRITES = 4
SCENE_MAX_SPRITE_FRAMES = 8
SCENE_MAX_SPRITE_PIXELS = 4096
SCENE_SPRITE_MIN_DIMENSION = 8
SCENE_SPRITE_MAX_DIMENSION = 64
SCENE_SPRITE_MIN_FPS = 1
SCENE_SPRITE_MAX_FPS = 20
SCENE_SPRITE_MAX_SOURCE_BYTES = 128 * 1024
CLAWPKG_MAX_ENTRIES = 128
CLAWPKG_MAX_ENTRY_BYTES = 4 * 1024 * 1024
CLAWPKG_MAX_EXPANDED_BYTES = 16 * 1024 * 1024
WIDGET_MAX_VARS = 8
QUOTA_ENGINE = "p4-quota-v2"
QUOTA_V1 = {"vars": 8, "states": 6, "pages": 4, "transitions": 24, "legacyTransitions": 12, "ticks": 8, "choices": 6, "nodes": 32, "localItems": 16, "saveVars": 8, "widgetBytes": 4095, "appWidgetBytes": 8191}
QUOTA_V2 = {"vars": 16, "states": 8, "pages": 6, "transitions": 32, "legacyTransitions": 12, "ticks": 12, "choices": 8, "nodes": 48, "localItems": 24, "saveVars": 16, "widgetBytes": 8191, "appWidgetBytes": 8191}
# p4-quota-v3 (capability parent): larger negotiated ceilings; v1/v2 packages keep their numbers.
QUOTA_ENGINE_V3 = "p4-quota-v3"
QUOTA_V3 = {"vars": 32, "states": 16, "pages": 12, "transitions": 48, "legacyTransitions": 12, "ticks": 16, "choices": 12, "nodes": 64, "localItems": 64, "saveVars": 32, "widgetBytes": 12287, "appWidgetBytes": 12287}
QUOTA_ENGINES = (QUOTA_ENGINE, QUOTA_ENGINE_V3)
# v3 strings grow to 127 bytes (MINIAPP_V3_STRING_MAX 128); v1/v2 keep 63.
STRING_VAR_MAX_BYTES_V3 = 127
PROGRAM_ENGINE = "p4-component-program-v1"
PROGRAM_ENGINE_V2 = "p4-component-program-v2"
PROGRAM_ENGINES = (PROGRAM_ENGINE, PROGRAM_ENGINE_V2)
CELLS_ENGINE = "p4-cells-v1"
TEXTS_ENGINE = "p4-texts-v1"


def string_var_max_bytes(runtime: Any) -> int:
    return STRING_VAR_MAX_BYTES_V3 if isinstance(runtime, dict) and runtime.get("quota") == QUOTA_ENGINE_V3 else STRING_VAR_MAX_BYTES


def superseding_capability(capabilities: dict[str, Any], singular: str, plural: str, required: str, order: tuple[str, ...]) -> bool:
    """A newer single-valued profile satisfies an older package (v3 covers v2); the plural list is exact."""
    advertised = capabilities.get(plural)
    if isinstance(advertised, list) and required in advertised:
        return True
    current = capabilities.get(singular)
    return current in order and required in order and order.index(current) >= order.index(required)


def quota_caps(runtime: Any) -> dict[str, int]:
    """p4-quota-v2 ceilings apply only when the package names the profile."""
    if isinstance(runtime, dict) and runtime.get("quota") == QUOTA_ENGINE_V3:
        return QUOTA_V3
    return QUOTA_V2 if isinstance(runtime, dict) and runtime.get("quota") == QUOTA_ENGINE else QUOTA_V1
VAR_NAME_MAX_BYTES = 31
STRING_VAR_MAX_BYTES = 63
WIDGET_INT_MIN = -1_000_000_000
WIDGET_INT_MAX = 1_000_000_000
DEFAULT_CAPABILITIES = {
    "widgetInstrument": "p4-instrument-v1",
    "widgetMedia": "p4-media-v1",
    "widgetMediaSurface": "p4-media-surface-v1",
    "widgetSurfaceImages": "p4-surface-images-v1",
    "widgetAppSurface": "p4-app-surface-v1",
    "widgetAppGraphics": "p4-app-graphics-v1",
    "widgetGameSurface": "p4-game-surface-v1",
    "widgetDataSurface": "p4-data-surface-v1",
    "widgetSceneSurface": "p4-scene-surface-v1",
    "widgetFlappyTuning": "p4-flappy-tuning-v1",
    "widgetLifecycle": "p4-lifecycle-v1",
    "widgetInput": "p4-input-v1",
    "widgetLocalTools": "p4-local-tools-v1",
    "widgetLocalToolsMasks": "p4-local-masks-v2",
    "widgetLogic": "p4-logic-v1",
    "widgetQuota": "p4-quota-v2",
    "widgetSfx": "p4-sfx-v1",
    "widgetSequence": "p4-sequence-v1",
    "widgetSamples": "p4-samples-v1",
    "widgetClip": "p4-clip-v1",
    "widgetBigImage": "p4-big-image-v1",
    "widgetEvents": "p4-events-v1",
    "widgetMotion": "p4-motion-v1",
    "widgetPlatformTrack": "p4-platform-track-v1",
    "widgetSideScene": "p4-side-scene-v1",
    "widgetMediaTouch": "p4-media-touch-v1",
    "widgetSurfaceAtlas": "p4-wide-atlas-v1",
    "widgetAppTypography": "p4-app-type-v1",
    "widgetGlyphCells": "p4-glyph-cells-v1",
    "widgetAppInteraction": "p4-app-interaction-v1",
    "widgetAppHost": "p4-app-host-v1",
    "widgetVariableBindings": "p4-variable-bindings-v1",
    "widgetHostContext": "p4-host-context-v1",
    "widgetAppViewport": "p4-app-viewport-v1",
    "widgetAppViewports": "p4-app-viewports-v1",
    "widgetAppNavigation": "p4-app-navigation-v1",
    "widgetAppForm": "p4-app-form-v1",
    "widgetState": "p4-app-state-v1",
    "widgetLyrics": "p4-lrc-v1",
    "widgetData": "p4-data-list-v1",
    # The PC host's own adapter capability (pc-fetch-v1 lives in the client, not the firmware).
    "widgetPcFetch": "pc-fetch-v1",
    "widgetRuntime": RUNTIME_ENGINE,
    "widgetRuntimes": [RUNTIME_ENGINE_V3, RUNTIME_ENGINE],
    "widgetScene": SCENE_ENGINE,
    "widgetScenes": [SCENE_ENGINE_V1, SCENE_ENGINE],
    "widgetGamePresets": ["blocks", "snake", "flappy", "gomoku"],
    # Legacy alias retained by firmware for older Pet Manager builds.
    "widgetGames": ["blocks", "snake", "flappy", "gomoku"],
    "touchInput": {"ready": False},
}
ID_PATTERN = re.compile(r"^[a-z][a-z0-9_-]{0,46}$")
PAGE_ID_PATTERN = re.compile(r"^[A-Za-z0-9_.-]{1,23}$")
SCENE_ID_PATTERN = re.compile(r"^[A-Za-z0-9_.-]{1,15}$")
VAR_ID_PATTERN = re.compile(r"^[A-Za-z0-9_.-]{1,31}$")
ACTION_ID_PATTERN = re.compile(r"^[A-Za-z0-9_.-]{1,47}$")
SW_HOLD_PATTERN = re.compile(r"^button\.sw[123]\.(?:long_press|hold)$")
MOVEMENT_CLAIMS = (
    "移动", "飞行", "航行", "下落", "横移", "重力",
    "movement", "moving", "flight", "flying", "falling", "gravity",
)
COLLISION_CLAIMS = ("碰撞", "撞机", "撞墙", "collision", "crash")
SHOOTER_CLAIMS = (
    "射击", "开火", "子弹", "敌机", "战机", "飞机大战",
    "shoot", "firing", "bullet", "enemy fighter", "lane fighter", "shooter",
)
DEFAULT_GLOBAL_EXIT_EVENT = "button.sw3.short_press"
SYSTEM_ACTIONS = {
    "page_toggle",
    "page_enter",
    "page_back",
    "page_main",
    "page_app",
    "component_center",
}


def utf8_size(value: str) -> int:
    return len(value.encode("utf-8"))


def capability_supports(capabilities: dict[str, Any], singular: str, plural: str, value: str) -> bool:
    advertised = capabilities.get(plural)
    if isinstance(advertised, list):
        return value in advertised
    return capabilities.get(singular) == value


def png_dimensions(path: Path) -> tuple[int, int] | None:
    try:
        header = path.read_bytes()[:24]
    except OSError:
        return None
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        return None
    return int.from_bytes(header[16:20], "big"), int.from_bytes(header[20:24], "big")


def load_json(path: Path, errors: list[str]) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        errors.append(f"缺少文件: {path.relative_to(path.parent.parent) if path.parent.name == 'runtime' else path.name}")
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        errors.append(f"{path.name} 不是有效 UTF-8 JSON: {error}")
    return None


def compact_json_size(value: Any) -> int:
    return len(json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))


def string_leaves(value: Any) -> list[str]:
    if isinstance(value, str):
        return [value]
    if isinstance(value, dict):
        leaves: list[str] = []
        for child in value.values():
            leaves.extend(string_leaves(child))
        return leaves
    if isinstance(value, list):
        leaves = []
        for child in value:
            leaves.extend(string_leaves(child))
        return leaves
    return []


def text_claims(*values: Any) -> str:
    return " ".join(
        leaf.casefold()
        for value in values
        for leaf in string_leaves(value)
        if leaf.strip()
    )


def claims_any(text: str, terms: Iterable[str]) -> bool:
    return any(term.casefold() in text for term in terms)


def scene_rules(scene: Any) -> list[dict[str, Any]]:
    if not isinstance(scene, dict) or not isinstance(scene.get("rules"), list):
        return []
    return [rule for rule in scene["rules"] if isinstance(rule, dict)]


def scene_operations(scene: Any) -> list[tuple[dict[str, Any], dict[str, Any]]]:
    operations: list[tuple[dict[str, Any], dict[str, Any]]] = []
    for rule in scene_rules(scene):
        if not isinstance(rule.get("do"), list):
            continue
        operations.extend(
            (rule, operation)
            for operation in rule["do"]
            if isinstance(operation, dict)
        )
    return operations


def validate_claimed_mechanics(
    manifest: Any,
    negative: Any,
    runtime: Any,
    share: Any,
    errors: list[str],
) -> None:
    if not isinstance(runtime, dict):
        return
    text = text_claims(
        manifest,
        negative.get("dashboard") if isinstance(negative, dict) else None,
        runtime.get("dashboard"),
        share,
    )
    scene = runtime.get("scene")
    legacy_game = runtime.get("game")
    entities = scene.get("entities", []) if isinstance(scene, dict) else []
    entities = [entity for entity in entities if isinstance(entity, dict)]
    rules = scene_rules(scene)
    operations = scene_operations(scene)
    has_motion = any(
        entity.get("vx", 0) != 0 or entity.get("vy", 0) != 0
        for entity in entities
    ) or any(
        operation.get("op") in {"move", "velocity", "accelerate"}
        for _, operation in operations
    )
    # Continuous platforms use charge/release plus the shared physics core;
    # static entity velocities are required by that profile, not absent motion.
    # Recognize only a valid declared profile, never an arbitrary motion object.
    motion = scene.get("motion") if isinstance(scene, dict) else None
    if isinstance(motion, dict) and motion.get("engine") == "p4-motion-v1":
        from motion_policy import motion_errors
        has_motion = has_motion or not motion_errors(runtime)
    if isinstance(scene,dict) and 'bodies' in scene:
        from app_bodies import bodies_errors
        if not bodies_errors(runtime):
            by_id={e['id']:e for e in scene['entities']}
            dynamic={b['entity'] for b in scene['bodies'] if not b.get('static',False)}
            has_motion=has_motion or any(by_id[id].get('vx',0) or by_id[id].get('vy',0) for id in dynamic) or bool(dynamic and scene.get('gravity',0)) or any(o.get('op')=='impulse' and o.get('entity') in dynamic for _,o in operations)
    if isinstance(scene,dict) and scene.get('engine')=='p4-side-scene-v1':
        from side_scene import side_scene_errors
        has_motion=has_motion or not side_scene_errors(runtime)
    has_collision = any(rule.get("on") == "collision" for rule in rules)
    if isinstance(scene,dict) and scene.get('engine')=='p4-side-scene-v1':
        from side_scene import side_scene_errors
        has_collision=has_collision or not side_scene_errors(runtime)

    if claims_any(text, MOVEMENT_CLAIMS) and not (has_motion or isinstance(legacy_game, dict)):
        errors.append("组件文案声明了移动/飞行玩法，但 runtime 没有实体速度、移动规则或有效连续运动配置")
    if claims_any(text, COLLISION_CLAIMS) and not (
        has_collision or isinstance(legacy_game, dict)
    ):
        errors.append("组件文案声明了碰撞玩法，但 runtime 没有 collision 规则")

    if not claims_any(text, SHOOTER_CLAIMS):
        return
    if not isinstance(scene, dict):
        errors.append("射击/战机组件必须声明真实 scene，不能只切换文字状态")
        return
    projectile_ids = {
        str(entity.get("id"))
        for entity in entities
        if re.search(r"(?:bullet|shot|laser|missile|rocket)", str(entity.get("id", "")), re.I)
    }
    enemy_ids = {
        str(entity.get("id"))
        for entity in entities
        if re.search(r"(?:enemy|foe|target)", str(entity.get("id", "")), re.I)
    }
    moving_ids = {
        str(entity.get("id"))
        for entity in entities
        if entity.get("vx", 0) != 0 or entity.get("vy", 0) != 0
    }
    collision_pairs = {
        frozenset((str(rule.get("entity")), str(rule.get("with"))))
        for rule in rules
        if rule.get("on") == "collision"
    }
    projectile_hits_enemy = any(
        frozenset((projectile_id, enemy_id)) in collision_pairs
        for projectile_id in projectile_ids
        for enemy_id in enemy_ids
    )
    has_player_steering = any(
        rule.get("on") not in {"tick", "collision", "edge", "blocked"}
        and operation.get("op") == "move"
        and operation.get("dx", 0) != 0
        for rule, operation in operations
    )
    if not projectile_ids or not projectile_ids.intersection(moving_ids):
        errors.append("射击/战机组件需要一个持续移动的 bullet/shot/laser/missile 实体")
    if not enemy_ids or not enemy_ids.intersection(moving_ids):
        errors.append("射击/战机组件需要至少一个持续移动的 enemy/foe/target 实体")
    if not projectile_hits_enemy:
        errors.append("射击/战机组件需要子弹与敌人的 collision 命中规则")
    if not has_player_steering:
        errors.append("射击/战机组件需要由玩家 action 驱动的水平移动规则")


def dashboard_output_strings(value: Any) -> list[str]:
    """Return text that can render, excluding variable and formatter identifiers."""
    if isinstance(value, str):
        return [value]
    if not isinstance(value, dict):
        return []
    if isinstance(value.get("var"), str):
        return []
    if isinstance(value.get("fmt_mmss"), str) or isinstance(value.get("fmt_hms"), str) or isinstance(value.get("text_at"), str):
        return []
    if isinstance(value.get("pct_of"), str):
        label = value.get("label")
        return [label] if isinstance(label, str) else []
    leaves: list[str] = []
    for child in value.values():
        leaves.extend(dashboard_output_strings(child))
    return leaves


DASHBOARD_LEAF_RULE_KEYS = ("var", "fmt_mmss", "fmt_hms", "text_at")
DASHBOARD_SWITCH_MAX_CHOICES = 6
DASHBOARD_RULE_HINT = (
    "设备只接受字符串、{var}、{fmt_mmss}、{fmt_hms}、顶层 switch_state/switch_page"
    "（分支只能是前四种，不能再嵌套），以及 progress 专用 {pct_of,of_max,label}"
)


def validate_dashboard_rule(
    value: Any,
    location: str,
    slot: str,
    *,
    variables: dict[str, Any],
    state_ids: list[str],
    page_ids: list[str],
    errors: list[str],
    max_choices: int = DASHBOARD_SWITCH_MAX_CHOICES,
    texts: Any = None,
) -> None:
    """Mirror firmware parse_dashboard/parse_value. Anything looser previews fine on the PC
    but is rejected by the device at commit with "dashboard value rule is unsupported"."""

    def var_type(name: Any) -> str | None:
        spec = variables.get(name) if isinstance(name, str) else None
        return spec.get("type") if isinstance(spec, dict) else None

    def leaf(item: Any, where: str) -> None:
        if isinstance(item, str):
            return
        if not isinstance(item, dict):
            errors.append(f"{where} 必须是字符串或值规则对象；{DASHBOARD_RULE_HINT}")
            return
        keys = [key for key in DASHBOARD_LEAF_RULE_KEYS if isinstance(item.get(key), str) and item[key]]
        if len(keys) != 1 or set(item) != {keys[0]}:
            errors.append(
                f"{where} 不是设备支持的值规则，固件会以 dashboard value rule is unsupported 拒绝；"
                f"{DASHBOARD_RULE_HINT}"
            )
            return
        key = keys[0]
        name = item[key]
        # p4-texts-v1: {text_at} selects a package text by an int var and needs the table declared.
        if key == "text_at" and not (isinstance(texts, list) and len(texts) >= 1):
            errors.append(f"{where} 的 text_at 需要先声明 runtime/widget.json.texts（{TEXTS_ENGINE}）")
        if var_type(name) is None:
            errors.append(f"{where} 引用了未知变量 {name!r}")
        elif key != "var" and var_type(name) != "int":
            errors.append(f"{where} 的 {key} 需要 int 变量，{name!r} 不是")

    if slot == "progress" and isinstance(value, dict) and "pct_of" in value:
        if (
            set(value) - {"pct_of", "of_max", "label"}
            or not isinstance(value.get("pct_of"), str)
            or not isinstance(value.get("of_max"), str)
            or not isinstance(value.get("label", ""), str)
        ):
            errors.append(f"{location} 的 progress 规则只能包含 pct_of、of_max 和可选 label 字符串")
            return
        for key in ("pct_of", "of_max"):
            if var_type(value[key]) != "int":
                errors.append(f"{location}.{key} 需要 int 变量，{value[key]!r} 不是")
        return
    if isinstance(value, dict) and ("switch_state" in value or "switch_page" in value):
        by_state = "switch_state" in value
        key = "switch_state" if by_state else "switch_page"
        if set(value) != {key} or not isinstance(value[key], dict):
            errors.append(f"{location}.{key} 必须是对象，且不能与其他规则键混用")
            return
        selectors = state_ids if by_state else page_ids
        if len(value[key]) > max_choices:
            errors.append(f"{location}.{key} 最多 {max_choices} 个分支")
        for selector, choice in value[key].items():
            if selector not in selectors:
                errors.append(f"{location}.{key} 引用了未知 {'state' if by_state else 'page'} {selector!r}")
            if isinstance(choice, dict) and (
                "switch_state" in choice or "switch_page" in choice or "pct_of" in choice
            ):
                errors.append(
                    f"{location}.{key}.{selector} 不能再嵌套 switch 或 progress 规则，"
                    "固件会以 dashboard value rule is unsupported 拒绝"
                )
                continue
            leaf(choice, f"{location}.{key}.{selector}")
        return
    leaf(value, location)


def initial_var_values(runtime: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    variables = runtime.get("vars")
    if not isinstance(variables, dict):
        return result
    for name, spec in variables.items():
        if isinstance(spec, dict) and "init" in spec:
            result[name] = spec["init"]
    return result


def evaluate_dashboard_value(
    value: Any,
    *,
    state: str,
    page: str,
    variables: dict[str, Any],
    texts: Any = None,
) -> Any:
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if not isinstance(value, dict):
        return None
    # Firmware switch branches are plain values; a nested switch is rejected on the
    # device, so the preview must not resolve it either.
    if "switch_state" in value and isinstance(value["switch_state"], dict):
        return evaluate_dashboard_leaf(value["switch_state"].get(state), variables, texts)
    if "switch_page" in value and isinstance(value["switch_page"], dict):
        return evaluate_dashboard_leaf(value["switch_page"].get(page), variables, texts)
    if any(isinstance(value.get(key), str) for key in DASHBOARD_LEAF_RULE_KEYS):
        return evaluate_dashboard_leaf(value, variables, texts)
    if isinstance(value.get("pct_of"), str) and isinstance(value.get("of_max"), str):
        current = variables.get(value["pct_of"], 0)
        maximum = variables.get(value["of_max"], 0)
        try:
            pct = 0 if float(maximum) == 0 else round(float(current) * 100 / float(maximum))
        except (TypeError, ValueError, ZeroDivisionError):
            pct = 0
        return {"value": max(0, min(100, pct)), "label": str(value.get("label", ""))}
    if set(value).issubset({"value", "label"}):
        return value
    return None


def evaluate_dashboard_leaf(value: Any, variables: dict[str, Any], texts: Any = None) -> Any:
    """Resolve a literal or var/fmt/text_at rule exactly like firmware parse_value; other shapes are None."""
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if not isinstance(value, dict):
        return None
    if isinstance(value.get("var"), str) and value["var"]:
        return variables.get(value["var"])
    if isinstance(value.get("text_at"), str) and value["text_at"]:
        from texts import text_at
        return text_at(texts, variables.get(value["text_at"], 0))
    for formatter in ("fmt_mmss", "fmt_hms"):
        name = value.get(formatter)
        if isinstance(name, str) and name:
            raw = variables.get(name, 0)
            try:
                seconds = max(0, int(raw))
            except (TypeError, ValueError):
                return None
            if formatter == "fmt_mmss":
                return f"{seconds // 60:02d}:{seconds % 60:02d}"
            return f"{seconds // 3600:02d}:{(seconds // 60) % 60:02d}:{seconds % 60:02d}"
    return None


def normalize_preview_value(slot: str, value: Any) -> Any:
    if slot != "progress":
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            return str(value)
        return value
    if not isinstance(value, dict):
        return value
    try:
        number = max(0, min(100, int(round(float(value.get("value", 0))))))
    except (TypeError, ValueError):
        number = 0
    return {"value": number, "label": str(value.get("label", ""))}


def validate_package_files(widget_dir: Path, errors: list[str]) -> None:
    if not widget_dir.is_dir():
        errors.append(f"组件目录不存在: {widget_dir}")
        return
    entries = list(widget_dir.rglob("*"))
    if len(entries) > CLAWPKG_MAX_ENTRIES:
        errors.append(f"组件包文件数超过 {CLAWPKG_MAX_ENTRIES} 个上限")
    expanded_bytes = 0
    present: set[str] = set()
    for path in entries:
        relative = path.relative_to(widget_dir)
        relative_name = relative.as_posix()
        if path.is_symlink():
            errors.append(f"组件包不允许符号链接: {relative_name}")
            continue
        if not path.is_file():
            continue
        present.add(relative_name)
        try:
            size = path.stat().st_size
        except OSError as error:
            errors.append(f"无法读取组件文件大小: {relative_name}: {error}")
            continue
        expanded_bytes += size
        if size > CLAWPKG_MAX_ENTRY_BYTES:
            errors.append(f"组件文件超过 4 MiB 上限: {relative_name}")
        if path.suffix.lower() in FORBIDDEN_SUFFIXES:
            errors.append(f"组件包不允许可执行或脚本文件: {relative_name}")
        if relative.parts and relative.parts[0] == "assets":
            if len(relative.parts) != 2 or (path.name != ".keep" and path.suffix not in (".png", ".wav", ".mp4")):
                errors.append(f"assets/ 只允许根目录下的 .keep、最终 PNG 以及声明过的 .wav/.mp4: {relative_name}")
    if expanded_bytes > CLAWPKG_MAX_EXPANDED_BYTES:
        errors.append("组件包总大小超过 16 MiB 上限")
    for required in sorted(REQUIRED_FILES - present):
        errors.append(f"缺少文件: {required}")
    if not (widget_dir / "assets").is_dir():
        errors.append("缺少目录: assets/")
    if not (widget_dir / "runtime").is_dir():
        errors.append("缺少目录: runtime/")


def validate_png_asset_inventory(widget_dir: Path, runtime: Any, errors: list[str]) -> None:
    present = {
        path.relative_to(widget_dir).as_posix()
        for path in (widget_dir / "assets").glob("*.png")
        if path.is_file() and not path.is_symlink()
    }
    scene = runtime.get("scene") if isinstance(runtime, dict) else None
    sprites = scene.get("sprites") if isinstance(scene, dict) else None
    referenced = set()
    if isinstance(sprites, list):
        referenced = {
            sprite["asset"]
            for sprite in sprites
            if isinstance(sprite, dict) and isinstance(sprite.get("asset"), str)
        }
    surface=runtime.get("surface",{}) if isinstance(runtime,dict) else {}
    from app_surface import keyframe_declaration, keyframe_asset
    keyframes=keyframe_declaration(surface)
    for index,im in enumerate(surface.get("images",[]) if isinstance(surface,dict) else []):
        if not isinstance(im,dict): continue
        asset=im.get("asset")
        if not isinstance(asset,str): continue
        # p4-keyframe-bg-v1: frames 2..N are sibling PNGs of the declared background.
        frames=range(1,(keyframes[0] if keyframes and index==0 and isinstance(keyframes[0],int) else 1)+1)
        for frame in frames:
            source=keyframe_asset(asset,frame)
            referenced.add(source)
            path=widget_dir/source
            try:
                import struct
                b=path.read_bytes()
                if len(b)>(1024*1024 if im.get('encoding')=='jpeg' else 128*1024) or b[:8]!=b"\x89PNG\r\n\x1a\n" or struct.unpack(">II",b[16:24])!=(im.get("frame_width"),im.get("frame_height")):
                    errors.append("surface PNG 尺寸或格式无效")
            except (OSError,struct.error): errors.append("surface PNG 缺失")
    from capability_ext import media_files
    referenced |= {asset for asset, kind in media_files(runtime).items() if kind == "png"}
    for asset in sorted(present - referenced):
        errors.append(f"PNG 素材未被 runtime/widget.json.scene.sprites 引用: {asset}")


def validate_manifest(manifest: Any, errors: list[str]) -> str | None:
    if not strings_valid_text(manifest):
        errors.append("component.json 的设备字符串不能包含 NUL 或未配对 Unicode 代理项")
        return None
    if not value_within_depth(manifest):
        errors.append(f"component.json 的设备 JSON 嵌套层级超过 {DEVICE_JSON_MAX_DEPTH}");return None
    if not isinstance(manifest, dict):
        if manifest is not None:
            errors.append("component.json 必须是对象")
        return None
    for field in ("id", "name", "version", "description"):
        if not isinstance(manifest.get(field), str) or not manifest[field].strip():
            errors.append(f"component.json.{field} 必须是非空字符串")
    widget_id = manifest.get("id")
    if isinstance(widget_id, str) and not ID_PATTERN.fullmatch(widget_id):
        errors.append("component.json.id 必须匹配 ^[a-z][a-z0-9_-]{0,46}$")
    kind = manifest.get("kind")
    if kind not in ALLOWED_KINDS:
        errors.append('component.json.kind 必须是 "game" 或 "tool"')
        return None
    return kind


def validate_dashboard(
    dashboard: Any,
    location: str,
    kind: str | None,
    *,
    runtime_dashboard: bool,
    errors: list[str],
    native_surface: bool = False,
    variables: dict[str, Any] | None = None,
    state_ids: list[str] | None = None,
    page_ids: list[str] | None = None,
    max_choices: int = DASHBOARD_SWITCH_MAX_CHOICES,
    texts: Any = None,
) -> None:
    if not strings_valid_text(dashboard):
        errors.append(f"{location} 的设备字符串不能包含 NUL 或未配对 Unicode 代理项")
        return
    if not value_within_depth(dashboard):
        errors.append(f"{location} 的设备 JSON 嵌套层级超过 {DEVICE_JSON_MAX_DEPTH}");return
    if not isinstance(dashboard, dict):
        errors.append(f"{location} 必须是对象")
        return
    unknown = sorted(set(dashboard) - set(DASHBOARD_LIMITS))
    if unknown:
        errors.append(f"{location} 含未知槽位: {', '.join(unknown)}")
    for slot, value in dashboard.items():
        if slot not in DASHBOARD_LIMITS:
            continue
        if not runtime_dashboard and slot != "progress" and not isinstance(value, str):
            errors.append(f"{location}.{slot} 初始预览必须是字符串")
            continue
        if slot == "progress" and not runtime_dashboard:
            if not isinstance(value, dict) or set(value) - {"value", "label"}:
                errors.append(f"{location}.progress 必须是 {{value,label}} 对象")
                continue
            number = value.get("value")
            if not isinstance(number, (int, float)) or not 0 <= number <= 100:
                errors.append(f"{location}.progress.value 必须在 0-100")
            if not isinstance(value.get("label", ""), str):
                errors.append(f"{location}.progress.label 必须是字符串")
        if runtime_dashboard and variables is not None:
            validate_dashboard_rule(
                value,
                f"{location}.{slot}",
                slot,
                variables=variables,
                state_ids=state_ids or [],
                page_ids=page_ids or [],
                errors=errors,
                max_choices=max_choices,
                texts=texts,
            )
        for leaf in dashboard_output_strings(value):
            if utf8_size(leaf) > DASHBOARD_LIMITS[slot]:
                errors.append(
                    f"{location}.{slot} 的值超出 {DASHBOARD_LIMITS[slot]} UTF-8 字节: {leaf!r}"
                )
    style_values = string_leaves(dashboard.get("visualStyle"))
    if not style_values or any(value not in ALLOWED_STYLES for value in style_values):
        errors.append(
            f"{location}.visualStyle 必须使用 pixel 或 clean；classic 仅供旧包兼容"
        )
    visual_sets = {
        "visualPalette": ALLOWED_PALETTES,
        "visualLayout": ALLOWED_LAYOUTS,
        "visualSprite": ALLOWED_SPRITES,
    }
    for field, allowed in visual_sets.items():
        values = string_leaves(dashboard.get(field))
        if native_surface and field in {"visualPalette", "visualSprite"} and not values:
            continue
        if not values:
            errors.append(f"{location}.{field} 缺失")
            continue
        invalid = sorted(set(values) - allowed)
        if invalid:
            errors.append(f"{location}.{field} 含不支持的值: {', '.join(invalid)}")
    layouts = string_leaves(dashboard.get("visualLayout"))
    if kind == "tool" and any(value != "tool" for value in layouts):
        errors.append(f'{location}.visualLayout: tool 组件必须使用 "tool"')
    if kind == "game" and any(value == "tool" for value in layouts):
        errors.append(f"{location}.visualLayout: game 组件不能使用 tool")


def validate_initial_preview(
    negative: Any,
    runtime: Any,
    errors: list[str],
) -> None:
    if not isinstance(negative, dict) or not isinstance(runtime, dict):
        return
    preview = negative.get("dashboard")
    dashboard = runtime.get("dashboard")
    if not isinstance(preview, dict) or not isinstance(dashboard, dict):
        return
    state = runtime.get("initial_state", "")
    page = runtime.get("initial_page", "")
    variables = initial_var_values(runtime)
    for slot, preview_value in preview.items():
        if slot not in dashboard:
            errors.append(f"runtime 初始 dashboard 缺少 negative-screen 槽位: {slot}")
            continue
        evaluated = evaluate_dashboard_value(
            dashboard[slot], state=state, page=page, variables=variables, texts=runtime.get("texts")
        )
        if normalize_preview_value(slot, preview_value) != normalize_preview_value(slot, evaluated):
            errors.append(
                f"初始预览不一致: {slot} 在 negative-screen 中为 {preview_value!r}，"
                f"runtime 初始值为 {evaluated!r}"
            )


def effect_count(rule: Any) -> int:
    if not isinstance(rule, dict):
        return 0
    return sum(len(rule.get(key, {})) for key in ("set", "inc") if isinstance(rule.get(key), dict))


def int_between(value: Any, minimum: int, maximum: int) -> bool:
    return type(value) is int and minimum <= value <= maximum


def only_keys(value: Any, allowed: set[str]) -> bool:
    return isinstance(value, dict) and set(value) <= allowed


def valid_scene_coordinate(value: Any) -> bool:
    if int_between(value, 0, 15):
        return True
    return (
        isinstance(value, list)
        and len(value) == 2
        and int_between(value[0], 0, 15)
        and int_between(value[1], value[0], 15)
    )


def validate_scene_op(
    operation: Any,
    entity_ids: set[str],
    location: str,
    errors: list[str],
) -> None:
    if not isinstance(operation, dict) or not isinstance(operation.get("op"), str):
        errors.append(f"{location} 必须是含 op 的对象")
        return
    name = operation["op"]

    def known_entity(field: str = "entity") -> bool:
        if operation.get(field) not in entity_ids:
            errors.append(f"{location}.{field} 引用了未知实体")
            return False
        return True

    if name == "impulse":
        if (set(operation)!={"op","entity","dx","dy"} or not known_entity()
            or not int_between(operation.get("dx"),-64,64) or not int_between(operation.get("dy"),-64,64)
            or not (operation.get("dx") or operation.get("dy"))):
            errors.append(f"{location} impulse 需要已声明刚体和非零 -64..64 向量")
    elif name == "move":
        valid = only_keys(operation, {"op", "entity", "dx", "dy"}) and known_entity()
        valid = valid and int_between(operation.get("dx"), -4, 4)
        valid = valid and int_between(operation.get("dy"), -4, 4)
        valid = valid and (operation.get("dx") != 0 or operation.get("dy") != 0)
        if not valid:
            errors.append(f"{location} move 需要实体及 -4..4 的非零 dx/dy")
    elif name in {"velocity", "accelerate"}:
        valid = only_keys(operation, {"op", "entity", "vx", "vy"}) and known_entity()
        valid = valid and int_between(operation.get("vx"), -4, 4)
        valid = valid and int_between(operation.get("vy"), -4, 4)
        if not valid:
            errors.append(f"{location} {name} 需要实体及 -4..4 的 vx/vy")
    elif name == "place":
        valid = only_keys(operation, {"op", "entity", "source", "x", "y", "dx", "dy"})
        valid = valid and known_entity()
        if "source" in operation:
            valid = known_entity("source") and valid
        valid = valid and all(
            field not in operation or int_between(operation[field], -4, 4)
            for field in ("dx", "dy")
        )
        valid = valid and all(
            field not in operation or valid_scene_coordinate(operation[field])
            for field in ("x", "y")
        )
        valid = valid and any(field in operation for field in ("source", "x", "y"))
        if not valid:
            errors.append(f"{location} place 需要合法实体、坐标范围或 source")
    elif name in {"show", "hide", "charge"}:
        if not only_keys(operation, {"op", "entity"}) or not known_entity():
            errors.append(f"{location} {name} 需要一个已声明实体")
    elif name == "score":
        has_add = "add" in operation
        has_set = "set" in operation
        value = operation.get("add") if has_add else operation.get("set")
        if (
            not only_keys(operation, {"op", "add", "set"})
            or has_add == has_set
            or not int_between(value, -10000, 10000)
        ):
            errors.append(f"{location} score 必须且只能含一个 -10000..10000 的 add/set")
    elif name in {"run", "stop", "restart"}:
        if set(operation) != {"op"}:
            errors.append(f"{location} {name} 不接受其他字段")
    elif name == "bounce":
        if (
            not only_keys(operation, {"op", "entity", "axis"})
            or not known_entity()
            or operation.get("axis") not in {"x", "y", "both"}
        ):
            errors.append(f"{location} bounce 需要实体及 x/y/both 轴")
    elif name == "tone":
        if (
            not only_keys(operation, {"op", "entity", "tone"})
            or not known_entity()
            or not int_between(operation.get("tone"), 1, 4)
        ):
            errors.append(f"{location} tone 需要实体及 1..4 色阶")
    elif name == "sfx":
        if not only_keys(operation, {"op", "cue"}) or not isinstance(operation.get("cue"), str):
            errors.append(f"{location} sfx 需要 cue")
    elif name == "var":
        # p4-logic-v1: scene writes one runtime int var; the validator checks the
        # declaration here and validate_scene checks the logic profile.
        has_set = "set" in operation
        has_inc = "inc" in operation
        value = operation.get("set") if has_set else operation.get("inc")
        if (
            not only_keys(operation, {"op", "name", "set", "inc"})
            or has_set == has_inc
            or not int_between(value, -10000, 10000)
            or not isinstance(operation.get("name"), str)
        ):
            errors.append(f"{location} var 需要 name 与一个 -10000..10000 的 set/inc")
    else:
        errors.append(f"{location}.op 不支持: {name}")


def validate_scene(
    scene: Any,
    runtime: dict[str, Any],
    widget_dir: Path,
    capabilities: dict[str, Any],
    transition_actions: set[str],
    errors: list[str],
) -> None:
    if isinstance(scene,dict) and scene.get('engine')=='p4-side-scene-v1':
        from side_scene import side_scene_errors
        errors.extend(side_scene_errors(runtime,capabilities))
        return
    slicing = isinstance(scene, dict) and scene.get("engine") == "p4-touch-slice-v1"
    required_scene = "p4-touch-slice-v1" if slicing else SCENE_ENGINE if runtime.get("engine") == RUNTIME_ENGINE else SCENE_ENGINE_V1
    if not capability_supports(capabilities, "widgetScene", "widgetScenes", required_scene):
        errors.append(f"目标能力不支持通用场景引擎: {required_scene}")
    if not only_keys(
        scene,
        {"engine", "duration_sec", "lives", "sound_volume", "sprites", "revision"} if slicing else {
            "tick_ms", "active_state", "result_state", "score_var", "auto_start",
            "grid", "sprites", "entities", "rules", "motion", "bodies", "gravity",
        },
    ):
        errors.append("runtime/widget.json.scene 含未知字段或不是对象")
        return
    if not slicing and not int_between(scene.get("tick_ms"), 20 if 'bodies' in scene else 16 if 'motion' in scene else 100, 200 if 'bodies' in scene else 40 if 'motion' in scene else 2000):
        errors.append("runtime/widget.json.scene.tick_ms 必须在 100-2000")
    from app_bodies import bodies_errors
    errors.extend(bodies_errors(runtime))
    states = set(runtime.get("states", [])) if isinstance(runtime.get("states"), list) else set()
    if not slicing and scene.get("active_state") not in states:
        errors.append("runtime/widget.json.scene.active_state 必须引用 states")
    if "result_state" in scene and scene.get("result_state") not in states:
        errors.append("runtime/widget.json.scene.result_state 必须引用 states")
    score_var = scene.get("score_var")
    if score_var is not None:
        score_spec = runtime.get("vars", {}).get(score_var) if isinstance(runtime.get("vars"), dict) else None
        if not isinstance(score_spec, dict) or score_spec.get("type") != "int":
            errors.append("runtime/widget.json.scene.score_var 必须引用 int 变量")
    if "auto_start" in scene and type(scene.get("auto_start")) is not bool:
        errors.append("runtime/widget.json.scene.auto_start 必须是布尔值")
    if scene.get("auto_start") is True and runtime.get("initial_state") != scene.get("active_state"):
        errors.append("runtime/widget.json.scene.auto_start 要求 initial_state 等于 active_state")

    sprites = scene.get("sprites", [])
    sprite_ids: set[str] = set()
    sprite_pixels = 0
    from app_surface import graphics_limits
    max_sprites = graphics_limits(runtime)["sceneSprites"]
    max_sprite_pixels = graphics_limits(runtime)["spritePixels"]
    if not isinstance(sprites, list) or len(sprites) > max_sprites:
        errors.append(f"runtime/widget.json.scene.sprites 最多允许 {max_sprites} 项")
        sprites = []
    if sprites and runtime.get("engine") != RUNTIME_ENGINE:
        errors.append(f"runtime/widget.json.scene.sprites 需要 engine={RUNTIME_ENGINE}")
    for index, sprite in enumerate(sprites):
        location = f"runtime/widget.json.scene.sprites[{index}]"
        if not only_keys(sprite, {"id", "asset", "frame_width", "frame_height", "frames", "fps"}):
            errors.append(f"{location} 含未知字段或不是对象")
            continue
        sprite_id = sprite.get("id")
        if (
            not isinstance(sprite_id, str)
            or SCENE_ID_PATTERN.fullmatch(sprite_id) is None
            or sprite_id in sprite_ids
        ):
            errors.append(f"{location}.id 无效或重复")
        else:
            sprite_ids.add(sprite_id)
        asset = sprite.get("asset")
        if (
            not isinstance(asset, str)
            or re.fullmatch(r"assets/[A-Za-z0-9_.-]+\.png", asset) is None
        ):
            errors.append(f"{location}.asset 必须是 assets/ 下的安全 PNG 路径")
            continue
        frame_width = sprite.get("frame_width")
        frame_height = sprite.get("frame_height")
        frames = sprite.get("frames")
        fps = sprite.get("fps")
        dimensions_valid = (
            int_between(frame_width, SCENE_SPRITE_MIN_DIMENSION, SCENE_SPRITE_MAX_DIMENSION)
            and int_between(frame_height, SCENE_SPRITE_MIN_DIMENSION, SCENE_SPRITE_MAX_DIMENSION)
            and int_between(frames, 1, SCENE_MAX_SPRITE_FRAMES)
            and int_between(fps, SCENE_SPRITE_MIN_FPS, SCENE_SPRITE_MAX_FPS)
        )
        if not dimensions_valid:
            errors.append(f"{location} 的帧尺寸、帧数或 fps 超出 P4 上限")
            continue
        sprite_pixels += frame_width * frame_height * frames
        source = widget_dir / asset
        if source.is_file() and source.stat().st_size > SCENE_SPRITE_MAX_SOURCE_BYTES:
            errors.append(f"{location}.asset PNG 源文件超过 128 KiB 上限")
            continue
        dimensions = png_dimensions(source)
        expected = (frame_width * frames, frame_height)
        if dimensions is None:
            errors.append(f"{location} 引用的素材不是有效 PNG 或不存在: {asset}")
        elif dimensions != expected:
            errors.append(
                f"{location} PNG 尺寸应为 {expected[0]}x{expected[1]}，"
                f"实际为 {dimensions[0]}x{dimensions[1]}"
            )
    if sprite_pixels > max_sprite_pixels:
        errors.append(f"runtime/widget.json.scene.sprites 总像素超过 {max_sprite_pixels}")

    if slicing:
        revision=scene.get("revision",1)
        available=capabilities.get("widgetSliceRevision",1)
        if not int_between(revision,1,2) or not isinstance(available,int) or revision>available:
            errors.append("这个切果版本需要相应的 widgetSliceRevision 能力，请更新固件")
        if capabilities.get("widgetPointer") != "p4-pointer-v1" or capabilities.get("touchInput", {}).get("ready") is not True:
            errors.append("触控切果需要已实测就绪的 touchInput.ready=true 与 p4-pointer-v1")
        expected = [{"on": action, "from": "*"} for action in ("slice.restart", "slice.pause")]
        transitions = runtime.get("transitions")
        if (runtime.get("engine") != RUNTIME_ENGINE or runtime.get("vars") != {}
            or runtime.get("states") != ["idle"] or runtime.get("initial_state") != "idle"
            or runtime.get("tick") != [] or any(k in runtime for k in ("data","media","instrument","game","pages"))
            or not int_between(scene.get("duration_sec"),30,120) or not int_between(scene.get("lives"),1,5)
            or not int_between(scene.get("sound_volume"),0,100)
            or len(sprites)!=4 or any(not isinstance(s,dict) or s.get("frames")!=1 for s in sprites)
            or not isinstance(transitions,list) or len(transitions)!=2 or any(t not in transitions for t in expected)):
            errors.append("触控切果仅接受有界参数、四张单帧水果和开始/暂停动作")
        return

    grid = scene.get("grid")
    grid_valid = only_keys(grid, {"width", "height", "rows", "solid_tones"})
    grid_valid = grid_valid and int_between(grid.get("width"), 4, 16)
    grid_valid = grid_valid and int_between(grid.get("height"), 4, 16)
    if not grid_valid:
        errors.append("runtime/widget.json.scene.grid 需要 4..16 的 width/height")
        grid = {}
    else:
        rows = grid.get("rows")
        if rows is not None and (
            not isinstance(rows, list)
            or len(rows) != grid["height"]
            or any(
                not isinstance(row, str)
                or len(row) != grid["width"]
                or re.fullmatch(r"[0-4]+", row) is None
                for row in rows
            )
        ):
            errors.append("runtime/widget.json.scene.grid.rows 必须按高度/宽度使用 0..4 色阶")
        solid_tones = grid.get("solid_tones")
        if solid_tones is not None and (
            not isinstance(solid_tones, list)
            or len(solid_tones) > 4
            or len(solid_tones) != len(set(solid_tones))
            or any(not int_between(tone, 1, 4) for tone in solid_tones)
        ):
            errors.append("runtime/widget.json.scene.grid.solid_tones 必须是唯一的 1..4 色阶")

    entities = scene.get("entities")
    if not isinstance(entities, list) or not 1 <= len(entities) <= SCENE_MAX_ENTITIES:
        errors.append(f"runtime/widget.json.scene.entities 必须为 1-{SCENE_MAX_ENTITIES} 项")
        entities = []
    entity_ids: set[str] = set()
    for index, entity in enumerate(entities):
        location = f"runtime/widget.json.scene.entities[{index}]"
        if not only_keys(
            entity,
            {
                "id", "x", "y", "width", "height", "tone", "vx", "vy",
                "bounds", "shape", "sprite", "active", "collidable",
            },
        ):
            errors.append(f"{location} 含未知字段或不是对象")
            continue
        entity_id = entity.get("id")
        if (
            not isinstance(entity_id, str)
            or SCENE_ID_PATTERN.fullmatch(entity_id) is None
            or entity_id in entity_ids
        ):
            errors.append(f"{location}.id 无效或重复")
        else:
            entity_ids.add(entity_id)
        width = entity.get("width", 1)
        height = entity.get("height", 1)
        if (
            not int_between(entity.get("x"), 0, 15)
            or not int_between(entity.get("y"), 0, 15)
            or not int_between(width, 1, 8)
            or not int_between(height, 1, 8)
            or (grid.get("width") and entity.get("x", 99) + width > grid["width"])
            or (grid.get("height") and entity.get("y", 99) + height > grid["height"])
            or ("tone" in entity and not int_between(entity["tone"], 1, 4))
            or ("vx" in entity and not int_between(entity["vx"], -4, 4))
            or ("vy" in entity and not int_between(entity["vy"], -4, 4))
        ):
            errors.append(f"{location} 的位置、尺寸、色阶或速度越界")
        if "bounds" in entity and entity["bounds"] not in {"clamp", "wrap", "bounce", "hide", "stop"}:
            errors.append(f"{location}.bounds 不受支持")
        if entity.get("shape", "rect") not in ALLOWED_SCENE_SHAPES:
            errors.append(f"{location}.shape 不受支持")
        if "sprite" in entity and entity.get("sprite") not in sprite_ids:
            errors.append(f"{location}.sprite 引用了未知精灵")
        if any(field in entity and type(entity[field]) is not bool for field in ("active", "collidable")):
            errors.append(f"{location} active/collidable 必须是布尔值")

    rules = scene.get("rules")
    if not isinstance(rules, list) or not 1 <= len(rules) <= SCENE_MAX_RULES:
        errors.append(f"runtime/widget.json.scene.rules 必须为 1-{SCENE_MAX_RULES} 项")
        rules = []
    for index, rule in enumerate(rules):
        location = f"runtime/widget.json.scene.rules[{index}]"
        if not only_keys(rule, {"on", "entity", "with", "edge", "do", "when"}):
            errors.append(f"{location} 含未知字段或不是对象")
            continue
        if "when" in rule:
            if runtime.get("logic") != LOGIC_ENGINE:
                errors.append(f"{location}.when 需要声明 logic={LOGIC_ENGINE}")
            else:
                validate_condition(rule["when"], runtime, f"{location}.when", errors, legacy_only=False, max_items=1)
                when = rule["when"]
                if isinstance(when, dict):
                    slots = list(runtime["vars"])[:8] if isinstance(runtime.get("vars"), dict) else []
                    references = []
                    conditions = when['all'] if isinstance(when.get('all'), list) else [when]
                    for condition in conditions:
                        if not isinstance(condition, dict):
                            continue
                        if 'var' in condition:
                            references.append(condition['var'])
                        if isinstance(condition.get('value'), dict) and 'var' in condition['value']:
                            references.append(condition['value']['var'])
                    if any(name not in slots for name in references):
                        errors.append(f"{location}.when 必须引用前 8 个声明里的 int 变量")
        trigger = rule.get("on")
        if trigger in {"collision", "landed"}:
            if rule.get("entity") not in entity_ids or rule.get("with") not in entity_ids or rule.get("entity") == rule.get("with"):
                errors.append(f"{location} collision 需要两个不同的已声明实体")
        elif trigger in {"edge", "blocked"}:
            if rule.get("entity") not in entity_ids:
                errors.append(f"{location} 需要已声明实体")
            if trigger == "blocked" and "edge" in rule:
                errors.append(f"{location} blocked 不接受 edge")
            elif "edge" in rule and rule["edge"] not in {"any", "left", "right", "top", "bottom"}:
                errors.append(f"{location}.edge 不受支持")
        elif trigger != "tick":
            if (
                not isinstance(trigger, str)
                or ACTION_ID_PATTERN.fullmatch(trigger) is None
                or trigger not in transition_actions
            ):
                errors.append(f"{location}.on 必须匹配 transition 动作")
        operations = rule.get("do")
        if not isinstance(operations, list) or not 1 <= len(operations) <= SCENE_MAX_OPS:
            errors.append(f"{location}.do 必须为 1-{SCENE_MAX_OPS} 项")
        else:
            for op_index, operation in enumerate(operations):
                validate_scene_op(operation, entity_ids, f"{location}.do[{op_index}]", errors)
                if isinstance(operation, dict) and operation.get("op") == "sfx":
                    if operation.get("cue") not in sfx_cue_ids(runtime):
                        errors.append(f"{location}.do[{op_index}] sfx 需要声明 sfx={SFX_ENGINE} 并引用已声明 cue")
                if isinstance(operation, dict) and operation.get("op") == "var":
                    variables = runtime.get("vars") if isinstance(runtime.get("vars"), dict) else {}
                    spec = variables.get(operation.get("name"))
                    if runtime.get("logic") != LOGIC_ENGINE:
                        errors.append(f"{location}.do[{op_index}] var 需要声明 logic={LOGIC_ENGINE}")
                    elif not isinstance(spec, dict) or spec.get("type") != "int" or list(variables).index(operation["name"]) >= 8:
                        errors.append(f"{location}.do[{op_index}] var 必须引用前 8 个声明里的 int 变量")


LOGIC_ENGINE = "p4-logic-v1"
CONDITION_OPS = {"lte", "lt", "gte", "gt", "eq", "ne"}
SFX_ENGINE = "p4-sfx-v1"
SFX_PRESETS = ("tap", "beep", "chirp", "low", "water", "page")


def sfx_cue_ids(runtime: Any) -> list[str]:
    sfx = runtime.get("sfx") if isinstance(runtime, dict) else None
    if not isinstance(sfx, dict) or sfx.get("engine") != SFX_ENGINE or not isinstance(sfx.get("cues"), list):
        return []
    return [cue.get("id") for cue in sfx["cues"] if isinstance(cue, dict) and isinstance(cue.get("id"), str)]


def validate_sfx_profile(runtime: dict[str, Any], capabilities: dict[str, Any], errors: list[str]) -> None:
    """p4-sfx-v1: fixed presets, 1-8 named cues, references from transitions/tick.then/scene ops."""
    cues = sfx_cue_ids(runtime)
    if "sfx" in runtime:
        sfx = runtime["sfx"]
        if (not only_keys(sfx, {"engine", "volume", "cues"}) or sfx.get("engine") != SFX_ENGINE or runtime.get("engine") != RUNTIME_ENGINE
                or not int_between(sfx.get("volume"), 0, 100) or not isinstance(sfx.get("cues"), list) or not 1 <= len(sfx["cues"]) <= 8):
            errors.append(f"runtime/widget.json.sfx 需要 engine={SFX_ENGINE}、volume 0-100 与 1-8 个 cue")
        else:
            for index, cue in enumerate(sfx["cues"]):
                if (not only_keys(cue, {"id", "preset"}) or not isinstance(cue.get("id"), str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,15}", cue["id"])
                        or cue.get("preset") not in SFX_PRESETS or cues.count(cue["id"]) != 1):
                    errors.append(f"sfx.cues[{index}] 需要唯一安全 id 与预设之一: {', '.join(SFX_PRESETS)}")
        if any(key in runtime for key in ("instrument", "media", "local", "program")):
            errors.append("sfx 不与 instrument/media/local/program 混用")
        if capabilities.get("widgetSfx") != SFX_ENGINE:
            errors.append("设备未声明 widgetSfx=p4-sfx-v1，请先升级固件")
    for index, transition in enumerate(runtime.get("transitions") or []):
        if isinstance(transition, dict) and "sfx" in transition and transition["sfx"] not in cues:
            errors.append(f"transitions[{index}].sfx 需要声明 sfx={SFX_ENGINE} 并引用已声明 cue")
    for index, tick in enumerate(runtime.get("tick") or []):
        then = tick.get("then") if isinstance(tick, dict) else None
        if isinstance(then, dict) and "sfx" in then and then["sfx"] not in cues:
            errors.append(f"tick[{index}].then.sfx 需要声明 sfx={SFX_ENGINE} 并引用已声明 cue")


def validate_texts_profile(runtime: dict[str, Any], capabilities: dict[str, Any], errors: list[str]) -> None:
    from texts import ENGINE, schema_errors
    if "texts" in runtime:
        errors.extend(schema_errors(runtime))
        if capabilities.get("widgetTexts") != ENGINE:
            errors.append(f"设备未声明 widgetTexts={ENGINE}")


def validate_cells_profile(runtime: dict[str, Any], capabilities: dict[str, Any], errors: list[str]) -> None:
    from cells import ENGINE, schema_errors, op_errors
    if "cells" in runtime:
        errors.extend(schema_errors(runtime))
        if capabilities.get("widgetCells") != ENGINE:
            errors.append(f"设备未声明 widgetCells={ENGINE}")
    for key in ("transitions", "tick"):
        rules = runtime.get(key)
        if not isinstance(rules, list):
            continue
        for i, rule in enumerate(rules):
            if not isinstance(rule, dict):
                continue
            for suffix, body in (("", rule), (".else", rule.get("else")), (".then", rule.get("then"))):
                if isinstance(body, dict) and "cells" in body:
                    errors.extend(op_errors(body["cells"], runtime, f"{key}[{i}]{suffix}", program_owned="program" in runtime))


def validate_condition(
    when: Any,
    runtime: dict[str, Any],
    location: str,
    errors: list[str],
    *,
    legacy_only: bool,
    max_items: int = 2,
) -> None:
    """Mirror parse_condition: {var,lte} always; op/value/all only under p4-logic-v1."""
    variables = runtime.get("vars") if isinstance(runtime.get("vars"), dict) else {}

    def int_var(name: Any) -> bool:
        spec = variables.get(name) if isinstance(name, str) else None
        return isinstance(spec, dict) and spec.get("type") == "int"

    def item(value: Any, where: str) -> None:
        if isinstance(value, dict) and "cell" in value:
            from cells import grid, arg_ok
            shape = grid(runtime)
            cell = value["cell"]
            if legacy_only or shape is None or set(value) != {"cell", "op", "value"} or not isinstance(cell, dict) or set(cell) != {"x", "y"}:
                errors.append(f"{where} cell 条件需要 cells、logic 与 cell{{x,y}}/op/value")
                return
            cols, rows, _ = shape
            if not arg_ok(cell["x"], 0, cols - 1, variables) or not arg_ok(cell["y"], 0, rows - 1, variables):
                errors.append(f"{where}.cell 坐标必须在网格范围内或引用 int 变量")
            if value["op"] not in CONDITION_OPS or not arg_ok(value["value"], WIDGET_INT_MIN, WIDGET_INT_MAX, variables):
                errors.append(f"{where} cell 条件需要有效 op 和有界整数或 {{var}}")
            return
        if not only_keys(value, {"var", "lte", "op", "value"}):
            errors.append(f"{where} 条件只接受 var/lte 或 var/op/value")
            return
        if not int_var(value.get("var")):
            errors.append(f"{where}.var 必须引用 int 变量")
        if "lte" in value:
            if "op" in value or "value" in value or not int_between(value["lte"], WIDGET_INT_MIN, WIDGET_INT_MAX):
                errors.append(f"{where}.lte 必须是单独的有界整数阈值")
            return
        if legacy_only:
            errors.append(f"{where} 使用 op/value 需要声明 logic={LOGIC_ENGINE}")
            return
        if value.get("op") not in CONDITION_OPS:
            errors.append(f"{where}.op 只接受 lte/lt/gte/gt/eq/ne")
        operand = value.get("value")
        if isinstance(operand, dict):
            if set(operand) != {"var"} or not int_var(operand.get("var")):
                errors.append(f"{where}.value 的 {{var}} 必须引用 int 变量")
        elif not int_between(operand, WIDGET_INT_MIN, WIDGET_INT_MAX):
            errors.append(f"{where}.value 必须是有界整数或 {{var}}")

    if not isinstance(when, dict):
        errors.append(f"{location} 必须是对象")
        return
    if "all" in when:
        if legacy_only or set(when) != {"all"} or not isinstance(when["all"], list) or not 1 <= len(when["all"]) <= max_items:
            errors.append(f"{location}.all 需要 logic={LOGIC_ENGINE} 且 1-{max_items} 个条件")
            return
        for index, child in enumerate(when["all"]):
            item(child, f"{location}.all[{index}]")
        return
    item(when, location)


def validate_logic_profile(runtime: dict[str, Any], capabilities: dict[str, Any], errors: list[str]) -> None:
    """p4-logic-v1 capability negotiation and the schema of guards/else/var sources."""
    logic = runtime.get("logic")
    declared = logic is not None
    if declared:
        if logic != LOGIC_ENGINE or runtime.get("engine") != RUNTIME_ENGINE:
            errors.append(f"runtime/widget.json.logic 只接受 {LOGIC_ENGINE} 且需要 {RUNTIME_ENGINE}")
        # Native game presets may carry logic: their var mirror is inert, guards and tick conditions work.
        if any(key in runtime for key in ("local", "media", "instrument", "host", "program")):
            errors.append("logic 不与 local/media/instrument/host/program 混用")
        if capabilities.get("widgetLogic") != LOGIC_ENGINE:
            errors.append("设备未声明 widgetLogic=p4-logic-v1，请先升级固件")
    active = declared and logic == LOGIC_ENGINE
    variables = runtime.get("vars") if isinstance(runtime.get("vars"), dict) else {}
    app = isinstance(runtime.get("surface"), dict) and runtime["surface"].get("engine") == "p4-app-surface-v1"

    def check_sources(rule: Any, where: str) -> None:
        if not isinstance(rule, dict):
            return
        for op in ("set", "inc"):
            for name, value in (rule.get(op) or {}).items() if isinstance(rule.get(op), dict) else []:
                if isinstance(value, dict):
                    if op == "inc" and not active:
                        errors.append(f"{where}.inc.{name} 使用 {{var}} 需要声明 logic={LOGIC_ENGINE}")
                    elif op == "set" and not active and not app:
                        errors.append(f"{where}.set.{name} 使用 {{var}} 需要应用界面或 logic={LOGIC_ENGINE}")
                    elif set(value) != {"var"} or not isinstance(variables.get(value.get("var")), dict) or variables[value["var"]].get("type") != "int" or not isinstance(variables.get(name), dict) or variables[name].get("type") != "int":
                        errors.append(f"{where}.{op}.{name} 的 {{var}} 必须在整数变量之间复制")

    state_ids = runtime.get("states") if isinstance(runtime.get("states"), list) else []
    page_ids = [page.get("id") for page in runtime.get("pages", []) if isinstance(page, dict)] if isinstance(runtime.get("pages"), list) else []
    for index, transition in enumerate(runtime.get("transitions") or []):
        if not isinstance(transition, dict):
            continue
        where = f"transitions[{index}]"
        check_sources(transition, where)
        if "when" in transition or "else" in transition:
            if not active:
                errors.append(f"{where} 的 when/else 需要声明 logic={LOGIC_ENGINE}")
                continue
        if "when" in transition:
            validate_condition(transition["when"], runtime, f"{where}.when", errors, legacy_only=False)
        if "else" in transition:
            otherwise = transition["else"]
            if "when" not in transition or not only_keys(otherwise, {"to", "page", "set", "inc", "cells"}):
                errors.append(f"{where}.else 需要 when，且只接受 to/page/set/inc")
                continue
            if "to" in otherwise and otherwise["to"] not in state_ids:
                errors.append(f"{where}.else.to 引用了未知 state")
            if "page" in otherwise and otherwise["page"] not in {*page_ids, "next", "prev"}:
                errors.append(f"{where}.else.page 引用了未知 page")
            if not any(key in otherwise for key in ("to", "page", "set", "inc", "cells")) or (effect_count(otherwise) == 0 and "to" not in otherwise and "page" not in otherwise and "cells" not in otherwise):
                errors.append(f"{where}.else 必须改变状态、页面或变量")
            check_sources(otherwise, f"{where}.else")
            if effect_count(transition) + effect_count(otherwise) > 4:
                errors.append(f"{where} 主分支与 else 的 set+inc 合计超过 4 项")
    for index, tick in enumerate(runtime.get("tick") or []):
        if not isinstance(tick, dict):
            continue
        where = f"tick[{index}]"
        check_sources(tick, where)
        check_sources(tick.get("then"), f"{where}.then")
        if "when" in tick:
            validate_condition(tick["when"], runtime, f"{where}.when", errors, legacy_only=not active)


def validate_runtime(
    runtime: Any,
    kind: str | None,
    widget_dir: Path,
    capabilities: dict[str, Any],
    errors: list[str],
) -> set[str]:
    transition_actions: set[str] = set()
    if not strings_valid_text(runtime):
        errors.append("runtime/widget.json 的设备字符串不能包含 NUL 或未配对 Unicode 代理项")
        return transition_actions
    if not value_within_depth(runtime):
        errors.append(f"runtime/widget.json 的设备 JSON 嵌套层级超过 {DEVICE_JSON_MAX_DEPTH}");return transition_actions
    if not isinstance(runtime, dict):
        if runtime is not None:
            errors.append("runtime/widget.json 必须是对象")
        return transition_actions
    if runtime.get("schema_version") != 1:
        errors.append("runtime/widget.json.schema_version 必须为 1")
    runtime_engine = runtime.get("engine")
    if runtime_engine not in RUNTIME_ENGINES:
        errors.append(f"runtime/widget.json.engine 只支持: {', '.join(sorted(RUNTIME_ENGINES))}")
    elif not capability_supports(capabilities, "widgetRuntime", "widgetRuntimes", runtime_engine):
        errors.append(f"目标能力不支持统一组件运行时: {runtime_engine}")
    variables = runtime.get("vars")
    data = runtime.get("data")
    from app_surface import app_runtime_errors
    from capability_ext import strip as strip_capability_ext, validate_capability_ext
    # Track C extensions are validated by their own module; the unchanged validators see the base declaration.
    validate_capability_ext(runtime, capabilities, errors)
    runtime = strip_capability_ext(runtime)
    errors.extend(app_runtime_errors(runtime))
    from app_navigation import errors as navigation_errors
    errors.extend(navigation_errors(runtime))
    from app_form import errors as form_errors
    errors.extend(form_errors(runtime))
    from component_program import errors as program_errors
    errors.extend(program_errors(runtime))
    from variable_owners import errors as ownership_errors
    errors.extend(ownership_errors(runtime))
    if 'ownership' in runtime and capabilities.get('widgetVariableOwners')!='p4-variable-owners-v1':errors.append('设备未开放宿主与设备变量隔离，未安装不兼容资源')
    if 'program' in runtime:
        # p4-component-program-v2 supersedes v1 in the single-valued key; widgetPrograms lists both when present.
        declared_program=runtime['program'].get('engine') if isinstance(runtime.get('program'),dict) else None
        required_program=declared_program if declared_program in PROGRAM_ENGINES else PROGRAM_ENGINE
        if not superseding_capability(capabilities,'widgetProgram','widgetPrograms',required_program,PROGRAM_ENGINES):errors.append(f'设备未开放受限组件程序 widgetProgram={required_program}，未安装不兼容资源')
    if 'form' in runtime and runtime.get('form',{}).get('engine')!='p4-app-form-v3':
        mixed=runtime.get('form',{}).get('engine')=='p4-app-form-v2'
        if capabilities.get('widgetAppFormV2' if mixed else 'widgetAppForm')!=('p4-app-form-v2' if mixed else 'p4-app-form-v1'):errors.append('需要有界表单编辑能力，请先升级固件')
    if 'navigation' in runtime and capabilities.get('widgetAppNavigation')!='p4-app-navigation-v1':errors.append('需要通用页面导航能力，请先升级固件')
    # Track D app-shell capabilities: each is refused unless the device advertises its exact string.
    from app_shell import errors as shell_errors
    errors.extend(shell_errors(runtime))
    from data_rows import errors as data_rows_errors
    errors.extend(data_rows_errors(runtime))
    from notify_policy import errors as notify_errors
    errors.extend(notify_errors(runtime))
    surface_decl=runtime.get('surface',{}) if isinstance(runtime.get('surface'),dict) else {}
    if ('shell' in surface_decl or 'tabs' in surface_decl) and capabilities.get('widgetAppShell')!='p4-app-shell-v1':errors.append('需要应用壳能力 p4-app-shell-v1，请先升级固件')
    data_decl=runtime.get('data') if isinstance(runtime.get('data'),dict) else {}
    if (any(k in data_decl for k in ('select_var','filters','refresh_action')) or (isinstance(surface_decl.get('data_view'),dict) and surface_decl['data_view'].get('layout')=='rows') or any(isinstance(n,dict) and 'row_bind' in n for n in surface_decl.get('elements',[]))) and capabilities.get('widgetDataRows')!='p4-data-rows-v1':errors.append('需要通用数据行能力 p4-data-rows-v1，请先升级固件')
    if ('texts' in surface_decl or any(isinstance(n,dict) and n.get('type') in ('longtext','figure') for n in surface_decl.get('elements',[]))) and capabilities.get('widgetLongText')!='p4-longtext-v1':errors.append('需要长文分页与图文能力 p4-longtext-v1，请先升级固件')
    if ('notify' in runtime or any(isinstance(t,dict) and 'notify' in t for t in runtime.get('transitions',[]))) and capabilities.get('widgetNotify')!='p4-notify-v1':errors.append('需要组件通知能力 p4-notify-v1，请先升级固件')
    if runtime.get('form',{}).get('engine')=='p4-app-form-v3' and capabilities.get('widgetAppFormV3')!='p4-app-form-v3':errors.append('需要多字段文本表单能力 p4-app-form-v3，请先升级固件')
    from local_tools import local_errors, local_capability_errors
    errors.extend(local_errors(runtime))
    errors.extend(local_capability_errors(runtime, capabilities))
    from input_policy import input_errors
    errors.extend(input_errors(runtime))
    from motion_policy import motion_errors
    errors.extend(motion_errors(runtime))
    if isinstance(runtime.get('scene'),dict) and runtime['scene'].get('motion') and capabilities.get('widgetMotion')!='p4-motion-v1':errors.append('连续运动需要 p4-motion-v1，请先升级固件')
    if isinstance(runtime.get('scene'),dict) and isinstance(runtime['scene'].get('motion'),dict) and 'track' in runtime['scene']['motion'] and capabilities.get('widgetPlatformTrack')!='p4-platform-track-v1':errors.append('连续平台与镜头需要 p4-platform-track-v1，请先升级固件')
    if 'input' in runtime and capabilities.get('widgetInput')!='p4-input-v1':errors.append('需要组件输入策略能力，请先升级固件')
    if 'lifecycle' in runtime and capabilities.get('widgetLifecycle')!='p4-lifecycle-v1':errors.append('需要游戏暂停与成绩存档能力，请先升级固件')
    if runtime.get('surface',{}).get('input') and capabilities.get('widgetMediaTouch')!='p4-media-touch-v1':errors.append('需要媒体触屏控制能力，请先升级固件')
    if any(im.get('encoding')!='jpeg' and im.get('frame_width',0)>240 for im in runtime.get('surface',{}).get('images',[])) and capabilities.get('widgetSurfaceAtlas')!='p4-wide-atlas-v1':errors.append('需要高清分格素材能力，请先升级固件')
    if 'host' in runtime and capabilities.get('widgetAppHost')!='p4-app-host-v1':errors.append('需要 PC 应用服务能力，请先升级固件')
    if 'host_context' in runtime:
        if runtime.get('host_context')!='p4-host-context-v1' or runtime.get('host') is not True or runtime.get('input',{}).get('engine')!='p4-input-v1':errors.append('宿主动作上下文声明无效')
        if capabilities.get('widgetHostContext')!='p4-host-context-v1':errors.append('组合 v2 需要宿主动作版本上下文，请先升级固件')
    if runtime.get('surface',{}).get('services') is not None and capabilities.get('widgetServiceBridge')!='p4-service-bridge-v1':errors.append('需要服务桥能力 p4-service-bridge-v1，请先升级固件')
    if runtime.get('surface',{}).get('bindings') and capabilities.get('widgetVariableBindings')!='p4-variable-bindings-v1':errors.append('需要通用变量绑定能力，请先升级固件')
    if 'graphics' in runtime:
        if runtime.get('graphics')!='p4-graphics-v2' or runtime_engine!=RUNTIME_ENGINE:errors.append('graphics 只支持 p4-graphics-v2 且需要 engine=p4-bounded-runtime-v4')
        if capabilities.get('widgetGraphicsV2')!='p4-graphics-v2':errors.append('设备未开放 p4-graphics-v2 图形配额，未安装不兼容资源')
    if isinstance(runtime.get('surface'),dict) and 'particles' in runtime['surface']:
        from app_surface import particle_errors
        errors.extend(particle_errors(runtime['surface'],runtime))
        if capabilities.get('widgetParticles')!='p4-particles-v1':errors.append('设备未开放装饰粒子能力 p4-particles-v1，未安装不兼容资源')
    if isinstance(runtime.get('surface'),dict):
        from app_surface import keyframe_declaration
        if keyframe_declaration(runtime['surface']) is not None:
            if runtime.get('graphics')!='p4-graphics-v2':errors.append('关键帧背景需要 graphics=p4-graphics-v2')
            if capabilities.get('widgetKeyframeBackground')!='p4-keyframe-bg-v1':errors.append('设备未开放关键帧背景能力 p4-keyframe-bg-v1，未安装不兼容资源')
    if isinstance(runtime.get('scene'),dict) and ('bodies' in runtime['scene'] or 'gravity' in runtime['scene']) and capabilities.get('widgetBodies')!='p4-bodies-v1':errors.append('设备未开放刚体场景能力 p4-bodies-v1，未安装不兼容资源')
    if 'canvas' in runtime.get('surface',{}):
        from app_canvas import canvas_capability_accepts
        if not canvas_capability_accepts(runtime['surface']['canvas'].get('engine') if isinstance(runtime['surface']['canvas'],dict) else None,capabilities.get('widgetCanvas')):errors.append('设备未开放有界绘图能力，未安装不兼容资源')
    if runtime.get('surface',{}).get('viewport') and capabilities.get('widgetAppViewport')!='p4-app-viewport-v1':errors.append('需要通用滚动视口能力，请先升级固件')
    if 'viewports' in runtime.get('surface',{}) and capabilities.get('widgetAppViewports')!='p4-app-viewports-v1':errors.append('需要多个独立滚动视口能力，请先升级固件')
    if 'collection_view' in runtime.get('surface',{}) and capabilities.get('widgetHostCollection')!='p4-host-collection-v1':errors.append('设备未开放宿主虚拟列表能力，未安装不兼容资源')
    if 'persist' in runtime:
        if capabilities.get('widgetState')!='p4-app-state-v1':errors.append('需要组件存档能力，请先升级固件')
    if 'surface' in runtime and 'media' not in runtime:
        from app_surface import app_surface_errors
        errors.extend(app_surface_errors(runtime['surface'],runtime))
        if runtime['surface'].get('graphics') and capabilities.get('widgetAppGraphics')!='p4-app-graphics-v1':errors.append('请升级支持高清背景、图标和精细排版的设备固件')
        if runtime['surface'].get('typography') and capabilities.get('widgetAppTypography')!='p4-app-type-v1':errors.append('请升级支持组件字形和动态按键提示的设备固件')
        if any('cell' in f for f in runtime['surface'].get('fonts',[])) and capabilities.get('widgetGlyphCells')!='p4-glyph-cells-v1':errors.append('请升级支持高清字形格的设备固件')
        if runtime['surface'].get('interaction') and capabilities.get('widgetAppInteraction')!='p4-app-interaction-v1':errors.append('请升级支持组件分格素材与条件显示的设备固件')
        game_surface='game_view' in runtime['surface']
        data_surface='data_view' in runtime['surface']
        scene_surface='scene_view' in runtime['surface']
        side_surface=isinstance(runtime.get('scene'),dict) and runtime['scene'].get('engine')=='p4-side-scene-v1'
        if scene_surface and capabilities.get('widgetSceneSurface')!='p4-scene-surface-v1':errors.append('精细场景布局需要升级设备固件')
        if data_surface and capabilities.get('widgetDataSurface')!='p4-data-surface-v1':errors.append('精细数据布局需要升级设备固件')
        if game_surface and capabilities.get('widgetGameSurface')!='p4-game-surface-v1':errors.append('游戏画面层需要升级设备固件')
        if runtime_engine!=RUNTIME_ENGINE or ((game_surface or side_surface) and kind!='game') or (data_surface and kind!='tool') or (not game_surface and not data_surface and not scene_surface and not side_surface and (kind!='tool' or any(k in runtime for k in ('scene','game','instrument','data')))):
            errors.append('app surface 仅支持 v4 tool，不与 scene/game/instrument/data 混用')
        if capabilities.get('widgetAppSurface')!='p4-app-surface-v1':
            errors.append('需要通用应用界面能力，请先升级固件')
        if isinstance(runtime['surface'],dict) and runtime['surface'].get('images') and capabilities.get('widgetSurfaceImages')!='p4-surface-images-v1':
            errors.append('需要受控图片能力，请先升级固件')
    if "instrument" in runtime:
        instrument = runtime["instrument"]
        if (not isinstance(instrument, dict) or set(instrument) != {"source", "effect_volume", "ambience_volume"}
            or instrument.get("source") != "percussion.wooden-fish"
            or any(type(instrument.get(k)) is not int or not 0 <= instrument[k] <= 100 for k in ("effect_volume", "ambience_volume"))
            or runtime_engine != RUNTIME_ENGINE or kind != "tool"
            or variables != {} or runtime.get("tick", []) != []
            or any(k in runtime for k in ("scene", "game", "data", "media", "pages"))
            or runtime.get("transitions") != [{"on": "instrument.strike", "from": "*"}]):
            errors.append("instrument 仅支持受控木鱼 tool、0–100 整数音量和单个 instrument.strike 动作")
        if capabilities.get("widgetInstrument") != "p4-instrument-v1":
            errors.append("目标固件不支持 p4-instrument-v1，请先升级固件")
    if "media" in runtime:
        if variables != {} or runtime.get("tick", []) != [] or "pages" in runtime:
            errors.append("media 不使用自定义变量、tick 或 pages，状态来自真实媒体服务")
        media = runtime.get("media")
        radio = isinstance(media,dict) and set(media)=={"source","mode"} and media.get("mode")=="live" and re.fullmatch(r"pc\.[a-z][a-z0-9_-]{0,43}",str(media.get("source",""))) is not None
        if not (media == {"source":"audio.player"} or radio) or runtime_engine != RUNTIME_ENGINE or kind != "tool" or any(k in runtime for k in ("scene", "game", "data")):
            errors.append("media 需要 v4 tool 的受控 PC 扩展或兼容播放器，不能混用 scene/game/data")
        if radio:
            from pc_extension import surface_errors
            errors.extend(surface_errors(runtime.get("surface")))
            if capabilities.get("widgetMediaSurface") != "p4-media-surface-v1":
                errors.append("目标固件不支持 p4-media-surface-v1，请先升级固件")
            if runtime.get("surface",{}).get("images") and capabilities.get("widgetSurfaceImages")!="p4-surface-images-v1": errors.append("请更新支持图片与旋转的固件")
        if capabilities.get("widgetMedia") != "p4-media-v1":
            errors.append("目标固件不支持 p4-media-v1，请先升级固件")
        media_actions = {"media.toggle", "media.queue", "media.previous", "media.next", "media.up", "media.down", "media.select", "media.lyrics"}
        if radio:
            media_actions.remove("media.lyrics")
        for rule in runtime.get("transitions", []):
            if not isinstance(rule, dict) or set(rule) != {"on", "from"} or rule.get("from") != "*" or rule.get("on") not in media_actions:
                errors.append("media transition 仅接受受控 on 动作，不接受额外效果")
    if "data" in runtime:
        if (
            not only_keys(data, {"source", "page_var", "select_var", "filters", "refresh_action"})
            or not isinstance(data.get("source"), str)
            or re.fullmatch(r"[a-z0-9_.-]{1,47}", data.get("source", "")) is None
            or not isinstance(data.get("page_var"), str)
            or not isinstance(variables, dict)
            or not isinstance(variables.get(data.get("page_var")), dict)
            or variables[data["page_var"]].get("type") != "int"
            or runtime_engine != RUNTIME_ENGINE
            or "scene" in runtime or "game" in runtime
            or kind != "tool"
        ):
            errors.append("runtime/widget.json.data 需要受控 source 和整数 page_var，只用于 v4 列表工具")
        if capabilities.get("widgetData") != "p4-data-list-v1":
            errors.append("目标固件不支持 p4-data-list-v1，请先升级固件")
        if isinstance(data, dict) and isinstance(data.get("source"), str) and data["source"].startswith("fetch.") and capabilities.get("widgetPcFetch") != "pc-fetch-v1":
            errors.append("fetch.* 数据源需要 PC 客户端的 pc-fetch-v1 适配器")
    if not isinstance(variables, dict):
        errors.append("runtime/widget.json.vars 必须是对象；无变量时使用 {}")
        variables = {}
    elif len(variables) > quota_caps(runtime)["vars"]:
        errors.append(
            f"runtime/widget.json.vars 最多 {quota_caps(runtime)['vars']} 个，当前为 {len(variables)}"
        )
    for name, declaration in variables.items():
        location = f"runtime/widget.json.vars.{name}"
        if (
            not isinstance(name, str)
            or VAR_ID_PATTERN.fullmatch(name) is None
            or utf8_size(name) > VAR_NAME_MAX_BYTES
        ):
            errors.append(f"{location} 的变量名无效")
            continue
        if not isinstance(declaration, dict):
            errors.append(f"{location} 必须是对象")
            continue
        unsupported = sorted(set(declaration) - {"type", "init"})
        if unsupported:
            errors.append(
                f"{location} 含固件不支持的字段 {unsupported[0]}；只允许 type 和 init"
            )
            continue
        var_type = declaration.get("type")
        if var_type not in {"int", "string"}:
            errors.append(f"{location}.type 只能是 int 或 string")
            continue
        if "init" not in declaration:
            continue
        init = declaration["init"]
        if var_type == "int":
            if (
                not isinstance(init, int)
                or isinstance(init, bool)
                or not WIDGET_INT_MIN <= init <= WIDGET_INT_MAX
            ):
                errors.append(
                    f"{location}.init 必须是 {WIDGET_INT_MIN}..{WIDGET_INT_MAX} 的整数"
                )
        elif not isinstance(init, str) or utf8_size(init) > string_var_max_bytes(runtime):
            errors.append(
                f"{location}.init 必须是最多 {string_var_max_bytes(runtime)} 个 UTF-8 字节的字符串"
            )
    states = runtime.get("states")
    state_ids: list[str] = []
    if not isinstance(states, list) or not 1 <= len(states) <= quota_caps(runtime)["states"] or not all(
        isinstance(value, str) and value for value in states
    ):
        errors.append(f"runtime/widget.json.states 必须是 1-{quota_caps(runtime)['states']} 个非空字符串")
    else:
        state_ids = states
        if len(state_ids) != len(set(state_ids)):
            errors.append("runtime/widget.json.states 必须唯一")
        if runtime.get("initial_state") not in state_ids:
            errors.append("runtime/widget.json.initial_state 必须引用 states")
    pages = runtime.get("pages")
    page_ids: list[str] = []
    if pages is not None:
        if not isinstance(pages, list) or not 1 <= len(pages) <= quota_caps(runtime)["pages"]:
            errors.append(f"runtime/widget.json.pages 必须是 1-{quota_caps(runtime)['pages']} 个 page 对象")
        else:
            for index, page in enumerate(pages):
                if not isinstance(page, dict) or set(page) != {"id"}:
                    errors.append(f"pages[{index}] 必须且只能包含 id")
                    continue
                page_id = page.get("id")
                if not isinstance(page_id, str) or not PAGE_ID_PATTERN.fullmatch(page_id):
                    errors.append(f"pages[{index}].id 必须是 1-23 个安全 ASCII 字符")
                    continue
                page_ids.append(page_id)
            if len(page_ids) != len(set(page_ids)):
                errors.append("runtime/widget.json.pages id 必须唯一")
            if runtime.get("initial_page") not in page_ids:
                errors.append("runtime/widget.json.initial_page 必须引用 pages")
    elif "initial_page" in runtime:
        errors.append("声明 initial_page 时必须同时声明 pages")
    transitions = runtime.get("transitions")
    if not isinstance(transitions, list):
        errors.append("runtime/widget.json.transitions 必须是数组")
        transitions = []
    elif len(transitions) > (quota_caps(runtime)["transitions"] if runtime.get("input",{}).get("engine")=="p4-input-v1" else quota_caps(runtime)["legacyTransitions"]):
        errors.append(f"runtime/widget.json.transitions 超出预算（传统 12、p4-input-v1 为 {quota_caps(runtime)['transitions']}），当前为 {len(transitions)}")
    for index, transition in enumerate(transitions):
        if not isinstance(transition, dict):
            errors.append(f"transitions[{index}] 必须是对象")
            continue
        action = transition.get("on")
        if not isinstance(action, str) or not action:
            errors.append(f"transitions[{index}].on 必须是非空 action")
        else:
            transition_actions.add(action)
        source = transition.get("from")
        if source not in {"*", *state_ids}:
            errors.append(f"transitions[{index}].from 引用了未知 state")
        target = transition.get("to")
        if target is not None and target not in state_ids:
            errors.append(f"transitions[{index}].to 引用了未知 state")
        page = transition.get("page")
        if page is not None and not isinstance(page,dict) and (not isinstance(page,str) or page not in {*page_ids, "next", "prev"}):
            errors.append(f"transitions[{index}].page 引用了未知 page")
        if effect_count(transition) > 4:
            errors.append(f"transitions[{index}] 的 set+inc 超过 4 项")
    ticks = runtime.get("tick")
    if not isinstance(ticks, list):
        errors.append("runtime/widget.json.tick 必须是数组")
        ticks = []
    elif len(ticks) > quota_caps(runtime)["ticks"]:
        errors.append(f"runtime/widget.json.tick 最多 {quota_caps(runtime)['ticks']} 条，当前为 {len(ticks)}")
    for index, tick in enumerate(ticks):
        if not isinstance(tick, dict):
            errors.append(f"tick[{index}] 必须是对象")
            continue
        if effect_count(tick) > 4:
            errors.append(f"tick[{index}] 的 set+inc 超过 4 项")
        if effect_count(tick.get("then")) > 4:
            errors.append(f"tick[{index}].then 的 set+inc 超过 4 项")
        while_state = tick.get("while_state")
        if while_state is not None and while_state not in state_ids:
            errors.append(f"tick[{index}].while_state 引用了未知 state")
    for field in ("fetchers", "readers"):
        value = runtime.get(field)
        if isinstance(value, dict) and value:
            errors.append(f"P4 第三方组件不支持 runtime/widget.json.{field}")
    validate_logic_profile(runtime, capabilities, errors)
    validate_sfx_profile(runtime, capabilities, errors)
    if "quota" in runtime:
        declared_quota = runtime.get("quota")
        if declared_quota not in (QUOTA_ENGINE, QUOTA_ENGINE_V3) or runtime.get("engine") != RUNTIME_ENGINE or "game" in runtime:
            errors.append(f"runtime/widget.json.quota 只接受 {QUOTA_ENGINE}/{QUOTA_ENGINE_V3} 且需要 {RUNTIME_ENGINE}，不与旧版 game 混用")
        required_quota = declared_quota if declared_quota in QUOTA_ENGINES else QUOTA_ENGINE
        if not superseding_capability(capabilities, "widgetQuota", "widgetQuotas", required_quota, QUOTA_ENGINES):
            errors.append(f"设备未声明 widgetQuota={required_quota}（或 widgetQuotas 列表），请先升级固件")
    validate_texts_profile(runtime, capabilities, errors)
    validate_cells_profile(runtime, capabilities, errors)
    validate_dashboard(
        runtime.get("dashboard"),
        "runtime/widget.json.dashboard",
        kind,
        runtime_dashboard=True,
        errors=errors,
        native_surface=isinstance(runtime.get("instrument"), dict),
        variables=variables,
        state_ids=state_ids,
        page_ids=page_ids,
        max_choices=quota_caps(runtime)["choices"],
        texts=runtime.get("texts"),
    )
    widget_limit=quota_caps(runtime)["appWidgetBytes"] if runtime.get('surface',{}).get('graphics')=='p4-app-graphics-v1' else quota_caps(runtime)["widgetBytes"]
    if compact_json_size(runtime) > widget_limit:
        errors.append(f"runtime/widget.json 紧凑后超过 {widget_limit} 字节: {compact_json_size(runtime)}")
    game = runtime.get("game")
    scene = runtime.get("scene")
    if game is not None and scene is not None:
        errors.append("runtime/widget.json.scene 与旧版 game 不能同时存在")
    if game is not None:
        validate_game(game, runtime, kind, capabilities, transition_actions, errors)
    if scene is not None:
        validate_scene(scene, runtime, widget_dir, capabilities, transition_actions, errors)
        if isinstance(scene,dict) and scene.get("engine")=="p4-touch-slice-v1" and kind!="game":
            errors.append("触控切果必须声明 kind=game")
    return transition_actions


def validate_game(
    game: Any,
    runtime: dict[str, Any],
    kind: str | None,
    capabilities: dict[str, Any],
    transition_actions: set[str],
    errors: list[str],
) -> None:
    if kind != "game":
        errors.append("只有 kind=game 的组件可以声明 runtime.game")
    if not isinstance(game, dict):
        errors.append("runtime/widget.json.game 必须是对象")
        return
    game_type = game.get("type")
    required_keys = ALLOWED_GAME_ACTIONS.get(game_type)
    if required_keys is None:
        errors.append(f"runtime/widget.json.game.type 不支持: {game_type}")
        return
    if capabilities.get("widgetRuntime") != "p4-bounded-v2" and not capability_supports(
        capabilities, "widgetRuntime", "widgetRuntimes", RUNTIME_ENGINE_V3
    ):
        errors.append(f"目标能力不支持旧版原生游戏预设: {capabilities.get('widgetRuntime')!r}")
    widget_games = capabilities.get("widgetGamePresets", capabilities.get("widgetGames"))
    if not isinstance(widget_games, list) or game_type not in widget_games:
        errors.append(f"目标设备能力未声明 widgetGamePresets.{game_type}")
    tick_ms = game.get("tick_ms")
    if "vertical_speed_percent" in game:
        if game_type!="flappy" or not int_between(game["vertical_speed_percent"],25,100):
            errors.append("vertical_speed_percent 仅支持 flappy 的 25-100 整数")
        if capabilities.get("widgetFlappyTuning")!="p4-flappy-tuning-v1":
            errors.append("飞鸟速度参数需要升级设备固件")
    if not isinstance(tick_ms, int) or not 100 <= tick_ms <= 2000:
        errors.append("runtime/widget.json.game.tick_ms 必须在 100-2000")
    states = set(runtime.get("states", [])) if isinstance(runtime.get("states"), list) else set()
    for field in ("playing_state", "result_state"):
        if game.get(field) not in states:
            errors.append(f"runtime/widget.json.game.{field} 必须引用 states")
    score_var = game.get("score_var")
    score_spec = runtime.get("vars", {}).get(score_var) if isinstance(runtime.get("vars"), dict) else None
    if not isinstance(score_spec, dict) or score_spec.get("type") != "int":
        errors.append("runtime/widget.json.game.score_var 必须引用 int 变量")
    actions = game.get("actions")
    required_keys=set(required_keys)
    if "direction_mode" in game:
        if game_type!="snake" or game["direction_mode"]!="absolute" or runtime.get("input",{}).get("engine")!="p4-input-v1":
            errors.append("绝对方向仅支持声明 p4-input-v1 的贪吃蛇")
        required_keys|={"up","down"}
    if isinstance(actions,dict) and "soft_drop" in actions:
        if game_type!="blocks" or runtime.get("input",{}).get("engine")!="p4-input-v1":
            errors.append("软降仅支持声明 p4-input-v1 的方块")
        required_keys.add("soft_drop")
    if not isinstance(actions, dict) or set(actions) != required_keys:
        errors.append(f"{game_type}.actions 必须且只能包含: {', '.join(sorted(required_keys))}")
        return
    for key, action in actions.items():
        if not isinstance(action, str) or not action:
            errors.append(f"runtime/widget.json.game.actions.{key} 必须是非空 action")
        elif action not in transition_actions:
            errors.append(f"game action 未出现在 transitions: {action}")


def event_slots(event: str) -> Iterable[str]:
    yield event


def validate_buttons(
    buttons: Any,
    transition_actions: set[str],
    capabilities: dict[str, Any],
    runtime: Any,
    errors: list[str],
) -> None:
    if not strings_valid_text(buttons):
        errors.append("buttons.json 的设备字符串不能包含 NUL 或未配对 Unicode 代理项")
        return
    if not value_within_depth(buttons):
        errors.append(f"buttons.json 的设备 JSON 嵌套层级超过 {DEVICE_JSON_MAX_DEPTH}");return
    if not isinstance(buttons, list):
        if buttons is not None:
            errors.append("buttons.json 必须是数组")
        return
    if len(buttons) > 8:
        errors.append(f"buttons.json 最多 8 条，当前为 {len(buttons)}")
    actions: list[str] = []
    occupied_events: set[str] = set()
    touch_ready = bool(
        isinstance(capabilities.get("touchInput"), dict)
        and capabilities["touchInput"].get("ready") is True
    )
    for index, binding in enumerate(buttons):
        if not isinstance(binding, dict):
            errors.append(f"buttons.json[{index}] 必须是对象")
            continue
        for field in ("action", "control", "event", "label"):
            if not isinstance(binding.get(field), str) or not binding[field].strip():
                errors.append(f"buttons.json[{index}].{field} 必须是非空字符串")
        action = binding.get("action")
        event = binding.get("event")
        media_lyric_key = (isinstance(runtime, dict) and runtime.get("media") == {"source": "audio.player"}
                           and action == "media.lyrics" and event == "button.sw1.long_press")
        if action == "media.lyrics" and capabilities.get("widgetLyrics") != "p4-lrc-v1":
            errors.append("歌词模式需要 widgetLyrics=p4-lrc-v1，请先升级固件")
        label = binding.get("label")
        if isinstance(action, str) and action:
            actions.append(action)
            if action in SYSTEM_ACTIONS:
                errors.append(f"组件不得定义系统导航 action: {action}；退出由全局设置负责")
        if isinstance(label, str) and utf8_size(label) > 30:
            errors.append(f"buttons.json[{index}].label 超过 30 UTF-8 字节")
        if isinstance(event, str):
            if event == DEFAULT_GLOBAL_EXIT_EVENT:
                errors.append("button.sw3.short_press 是默认全局退出键，组件不得占用")
            if SW_HOLD_PATTERN.fullmatch(event) and not media_lyric_key:
                errors.append(f"SW1/SW2/SW3 不支持长按或 hold: {event}")
            if event not in ALLOWED_EVENTS and not media_lyric_key:
                errors.append(f"buttons.json[{index}].event 不受支持: {event}")
            if event.startswith("screen.") and not touch_ready:
                errors.append(f"目标设备未声明触控可用，不能绑定 {event}")
            for slot in event_slots(event):
                if slot in occupied_events:
                    errors.append(f"buttons.json 物理事件重复: {slot}")
                occupied_events.add(slot)
    if len(actions) != len(set(actions)):
        errors.append("buttons.json.action 必须唯一")
    normal_actions = set(actions)
    missing_transitions = sorted(normal_actions - transition_actions)
    if missing_transitions:
        errors.append(f"按钮 action 未出现在 transitions: {', '.join(missing_transitions)}")
    touch_actions=set()
    if isinstance(runtime,dict) and runtime.get("input",{}).get("engine")=="p4-input-v1":
        touch_actions={n.get("action") for n in runtime.get("surface",{}).get("elements",[]) if isinstance(n,dict) and isinstance(n.get("action"),str)}
        touch_actions.update(runtime["input"].get("swipe",{}).values())
        surface = runtime.get("surface", {})
        if (isinstance(surface, dict) and "collection_view" in surface
                and capabilities.get("widgetHostCollection") == "p4-host-collection-v1"):
            # The real collection renderer expands only these validated row
            # actions into buttons. Never exempt arbitrary host.* transitions.
            from host_collection import config as collection_config
            try:
                collection = collection_config(surface["collection_view"], surface, runtime)
            except (ValueError, TypeError, KeyError, AttributeError):
                pass
            else:
                touch_actions.update(collection["actions"])
    from capability_ext import internal_actions
    unbound = sorted(transition_actions - normal_actions - touch_actions - internal_actions(runtime))
    if unbound:
        errors.append(f"transitions 存在未绑定 action: {', '.join(unbound)}")
    if isinstance(runtime, dict) and isinstance(runtime.get("game"), dict):
        game_actions = runtime["game"].get("actions")
        if isinstance(game_actions, dict):
            for action in game_actions.values():
                if isinstance(action, str) and action not in normal_actions:
                    errors.append(f"game action 未出现在 buttons.json: {action}")
    if compact_json_size(buttons) > 2047:
        errors.append(f"buttons.json 紧凑后超过 2047 字节: {compact_json_size(buttons)}")


def load_capabilities(path: Optional[Path]) -> dict[str, Any]:
    if path is None:
        return json.loads(json.dumps(DEFAULT_CAPABILITIES))
    value = json.loads(path.expanduser().read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("能力文件必须是 JSON 对象")
    return value


def validate_widget(
    widget_dir: Path,
    capabilities: dict[str, Any] | None = None,
) -> list[str]:
    widget_dir = widget_dir.expanduser().resolve()
    # An explicitly empty device report grants no capabilities. Only omission
    # selects the offline authoring defaults; never silently upgrade an old board.
    capabilities = load_capabilities(None) if capabilities is None else capabilities
    errors: list[str] = []
    validate_package_files(widget_dir, errors)
    if not widget_dir.is_dir():
        return errors
    manifest = load_json(widget_dir / "component.json", errors)
    negative = load_json(widget_dir / "negative-screen.json", errors)
    runtime = load_json(widget_dir / "runtime" / "widget.json", errors)
    buttons = load_json(widget_dir / "buttons.json", errors)
    share = load_json(widget_dir / "share.json", errors)
    kind = validate_manifest(manifest, errors)
    application_file=widget_dir/'pc/application.json'
    if application_file.exists() or isinstance(runtime,dict) and 'host' in runtime:
        from app_services import application_errors
        errors.extend(application_errors(load_json(application_file,errors),runtime))
        application=load_json(application_file,errors)
        application_limit=65536 if isinstance(application,dict) and application.get('engine')=='pc-service-bridge-v1' else 32768 if isinstance(application,dict) and application.get('engine') in ('pc-app-composition-v2','pc-app-composition-v3') else 4*1024*1024 if isinstance(application,dict) and (application.get('engine')=='pc-app-services-v3' and application.get('service')=='knowledge' or application.get('engine')=='pc-app-calendar-v1' and application.get('service')=='calendar' or application.get('engine')=='pc-app-books-v1' and application.get('service')=='books') else 16384
        if application_file.exists() and application_file.stat().st_size>application_limit:errors.append('PC 应用声明超过有界容量')
        for f in widget_dir.rglob('*'):
            relative=f.relative_to(widget_dir).as_posix()
            notice=relative in {'LICENSE.txt','THIRD_PARTY_NOTICES.txt'} or bool(re.fullmatch(r'licenses/[A-Za-z0-9_.-]+\.(?:txt|md)',relative))
            if f.is_file() and not notice and relative not in REQUIRED_FILES|{'pc/application.json','assets/.keep'}|{i.get('asset') for i in runtime.get('surface',{}).get('images',[]) if isinstance(i,dict)}:errors.append('PC 应用包含额外文件')
    pc_file=widget_dir / "pc" / "extension.json"
    source=runtime.get("media",{}).get("source","") if isinstance(runtime,dict) else ""
    if pc_file.exists() or source.startswith("pc."):
        from pc_extension import extension_errors
        extension=load_json(pc_file,errors)
        errors.extend(extension_errors(extension))
        if not isinstance(manifest,dict) or source != "pc."+str(manifest.get("id","")):
            errors.append("PC 扩展必须与组件 ID、媒体源绑定")
        if pc_file.exists() and pc_file.stat().st_size>16384: errors.append("PC 扩展超过 16 KiB")
        # A bundled pc/catalog.json is allowed only when search.catalog declares it, and vice versa.
        from pc_extension import catalog_errors
        catalog_file=widget_dir/"pc"/"catalog.json"
        declared=isinstance(extension,dict) and isinstance(extension.get("search"),dict) and extension["search"].get("catalog")=="pc/catalog.json"
        if declared: errors.extend(catalog_errors(catalog_file.read_bytes() if catalog_file.is_file() else None))
        elif catalog_file.exists(): errors.append("pc/catalog.json 必须由 pc/extension.json 的 search.catalog 声明")
        for f in widget_dir.rglob("*"):
            relative = f.relative_to(widget_dir).as_posix()
            legal_notice = relative in {"LICENSE.txt", "THIRD_PARTY_NOTICES.txt"} or bool(re.fullmatch(r"licenses/[A-Za-z0-9_.-]+\.(?:txt|md)", relative))
            if f.is_file() and not legal_notice and relative not in REQUIRED_FILES | {"pc/extension.json", "pc/catalog.json", "assets/.keep"} | {im.get("asset") for im in runtime.get("surface",{}).get("images",[]) if isinstance(im,dict)}:
                errors.append("声明式媒体包不能携带用户数据、凭据或额外文件")
    validate_png_asset_inventory(widget_dir, runtime, errors)
    from capability_ext import media_files, validate_media_files
    validate_media_files(widget_dir, runtime, errors)
    for extra in sorted({p.relative_to(widget_dir).as_posix() for p in (widget_dir / "assets").glob("*") if p.is_file() and p.suffix in (".wav", ".mp4")} - set(media_files(runtime))):
        errors.append(f"素材未被 samples/clip 声明引用: {extra}")
    if not isinstance(negative, dict):
        if negative is not None:
            errors.append("negative-screen.json 必须是对象")
    else:
        validate_dashboard(
            negative.get("dashboard"),
            "negative-screen.json.dashboard",
            kind,
            runtime_dashboard=False,
            errors=errors,
            native_surface=isinstance(runtime, dict) and isinstance(runtime.get("instrument"), dict),
        )
    transition_actions = validate_runtime(runtime, kind, widget_dir, capabilities, errors)
    validate_buttons(buttons, transition_actions, capabilities, runtime, errors)
    if isinstance(runtime, dict) and isinstance(buttons, list):
        from app_form import errors as form_errors
        errors.extend(form_errors(runtime,buttons))
        from input_policy import input_errors
        errors.extend(input_errors(runtime, buttons))
        if isinstance(runtime.get('scene'),dict) and runtime['scene'].get('engine')=='p4-side-scene-v1':
            from side_scene import side_scene_errors
            errors.extend(side_scene_errors(runtime,capabilities,buttons))
    validate_initial_preview(negative, runtime, errors)
    validate_claimed_mechanics(manifest, negative, runtime, share, errors)
    if not isinstance(share, dict):
        if share is not None:
            errors.append("share.json 必须是对象")
    elif not isinstance(share.get("title"), str) or not share["title"].strip():
        errors.append("share.json.title 必须是非空字符串")
    return list(dict.fromkeys(errors))


def main() -> int:
    parser = argparse.ArgumentParser(description="校验 petui P4 .clawpkg 组件目录")
    parser.add_argument("widget_dir", type=Path, help="待校验组件目录")
    parser.add_argument("--capabilities", type=Path, help="可选的设备能力 JSON")
    parser.add_argument("--json", action="store_true", help="以 JSON 输出结果")
    args = parser.parse_args()
    try:
        capabilities = load_capabilities(args.capabilities)
        widget_dir = args.widget_dir.expanduser().resolve()
        errors = validate_widget(widget_dir, capabilities)
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        errors = [f"能力或路径读取失败: {error}"]
        widget_dir = args.widget_dir.expanduser()
    if args.json:
        print(json.dumps({"ok": not errors, "path": str(widget_dir), "errors": errors}, ensure_ascii=False))
    elif errors:
        print(f"FAIL {widget_dir}", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
    else:
        print(f"OK {widget_dir}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
