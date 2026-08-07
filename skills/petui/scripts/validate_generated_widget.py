#!/usr/bin/env python3
"""Validate generated petui packages, including joystick and global-exit isolation."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable, Optional


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
    "rect", "player-ship", "enemy-ship", "bullet", "star", "paddle", "ball"
}
ALLOWED_SPRITES = {
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
}
RUNTIME_ENGINE = "p4-bounded-runtime-v3"
SCENE_ENGINE = "p4-grid-scene-v1"
SCENE_MAX_ENTITIES = 12
SCENE_MAX_RULES = 20
SCENE_MAX_OPS = 4
WIDGET_MAX_VARS = 8
VAR_NAME_MAX_BYTES = 31
STRING_VAR_MAX_BYTES = 63
WIDGET_INT_MIN = -1_000_000_000
WIDGET_INT_MAX = 1_000_000_000
DEFAULT_CAPABILITIES = {
    "widgetRuntime": RUNTIME_ENGINE,
    "widgetScene": SCENE_ENGINE,
    "widgetGamePresets": ["blocks", "snake", "flappy"],
    # Legacy alias retained by firmware for older Pet Manager builds.
    "widgetGames": ["blocks", "snake", "flappy"],
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
    has_collision = any(rule.get("on") == "collision" for rule in rules)

    if claims_any(text, MOVEMENT_CLAIMS) and not (has_motion or isinstance(legacy_game, dict)):
        errors.append("组件文案声明了移动/飞行玩法，但 runtime 没有实体速度或 move/velocity/accelerate 规则")
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
    if isinstance(value.get("fmt_mmss"), str) or isinstance(value.get("fmt_hms"), str):
        return []
    if isinstance(value.get("pct_of"), str):
        label = value.get("label")
        return [label] if isinstance(label, str) else []
    leaves: list[str] = []
    for child in value.values():
        leaves.extend(dashboard_output_strings(child))
    return leaves


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
) -> Any:
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if not isinstance(value, dict):
        return None
    if "switch_state" in value and isinstance(value["switch_state"], dict):
        return evaluate_dashboard_value(
            value["switch_state"].get(state), state=state, page=page, variables=variables
        )
    if "switch_page" in value and isinstance(value["switch_page"], dict):
        return evaluate_dashboard_value(
            value["switch_page"].get(page), state=state, page=page, variables=variables
        )
    if isinstance(value.get("var"), str):
        return variables.get(value["var"])
    for formatter in ("fmt_mmss", "fmt_hms"):
        name = value.get(formatter)
        if isinstance(name, str):
            raw = variables.get(name, 0)
            try:
                seconds = max(0, int(raw))
            except (TypeError, ValueError):
                return None
            if formatter == "fmt_mmss":
                return f"{seconds // 60:02d}:{seconds % 60:02d}"
            return f"{seconds // 3600:02d}:{(seconds // 60) % 60:02d}:{seconds % 60:02d}"
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
    for path in widget_dir.rglob("*"):
        if path.is_symlink():
            errors.append(f"组件包不允许符号链接: {path.relative_to(widget_dir).as_posix()}")
            continue
        if path.is_file() and path.suffix.lower() in FORBIDDEN_SUFFIXES:
            errors.append(f"组件包不允许可执行或脚本文件: {path.relative_to(widget_dir).as_posix()}")
    present = {
        path.relative_to(widget_dir).as_posix()
        for path in widget_dir.rglob("*")
        if path.is_file()
    }
    for required in sorted(REQUIRED_FILES - present):
        errors.append(f"缺少文件: {required}")
    if not (widget_dir / "assets").is_dir():
        errors.append("缺少目录: assets/")
    if not (widget_dir / "runtime").is_dir():
        errors.append("缺少目录: runtime/")


def validate_manifest(manifest: Any, errors: list[str]) -> str | None:
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
) -> None:
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
            dashboard[slot], state=state, page=page, variables=variables
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

    if name == "move":
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
    elif name in {"show", "hide"}:
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
    else:
        errors.append(f"{location}.op 不支持: {name}")


def validate_scene(
    scene: Any,
    runtime: dict[str, Any],
    capabilities: dict[str, Any],
    transition_actions: set[str],
    errors: list[str],
) -> None:
    if capabilities.get("widgetScene") != SCENE_ENGINE:
        errors.append(f"目标能力不支持通用场景引擎: {capabilities.get('widgetScene')!r}")
    if not only_keys(
        scene,
        {
            "tick_ms", "active_state", "result_state", "score_var", "auto_start",
            "grid", "entities", "rules",
        },
    ):
        errors.append("runtime/widget.json.scene 含未知字段或不是对象")
        return
    if not int_between(scene.get("tick_ms"), 100, 2000):
        errors.append("runtime/widget.json.scene.tick_ms 必须在 100-2000")
    states = set(runtime.get("states", [])) if isinstance(runtime.get("states"), list) else set()
    if scene.get("active_state") not in states:
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
                "bounds", "shape", "active", "collidable",
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
        if any(field in entity and type(entity[field]) is not bool for field in ("active", "collidable")):
            errors.append(f"{location} active/collidable 必须是布尔值")

    rules = scene.get("rules")
    if not isinstance(rules, list) or not 1 <= len(rules) <= SCENE_MAX_RULES:
        errors.append(f"runtime/widget.json.scene.rules 必须为 1-{SCENE_MAX_RULES} 项")
        rules = []
    for index, rule in enumerate(rules):
        location = f"runtime/widget.json.scene.rules[{index}]"
        if not only_keys(rule, {"on", "entity", "with", "edge", "do"}):
            errors.append(f"{location} 含未知字段或不是对象")
            continue
        trigger = rule.get("on")
        if trigger == "collision":
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


def validate_runtime(
    runtime: Any,
    kind: str | None,
    capabilities: dict[str, Any],
    errors: list[str],
) -> set[str]:
    transition_actions: set[str] = set()
    if not isinstance(runtime, dict):
        if runtime is not None:
            errors.append("runtime/widget.json 必须是对象")
        return transition_actions
    if runtime.get("schema_version") != 1:
        errors.append("runtime/widget.json.schema_version 必须为 1")
    if runtime.get("engine") != RUNTIME_ENGINE:
        errors.append(f'runtime/widget.json.engine 必须固定为 "{RUNTIME_ENGINE}"')
    if capabilities.get("widgetRuntime") != RUNTIME_ENGINE:
        errors.append(f"目标能力不支持统一组件运行时: {capabilities.get('widgetRuntime')!r}")
    variables = runtime.get("vars")
    if not isinstance(variables, dict):
        errors.append("runtime/widget.json.vars 必须是对象；无变量时使用 {}")
        variables = {}
    elif len(variables) > WIDGET_MAX_VARS:
        errors.append(
            f"runtime/widget.json.vars 最多 {WIDGET_MAX_VARS} 个，当前为 {len(variables)}"
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
        elif not isinstance(init, str) or utf8_size(init) > STRING_VAR_MAX_BYTES:
            errors.append(
                f"{location}.init 必须是最多 {STRING_VAR_MAX_BYTES} 个 UTF-8 字节的字符串"
            )
    states = runtime.get("states")
    state_ids: list[str] = []
    if not isinstance(states, list) or not 1 <= len(states) <= 6 or not all(
        isinstance(value, str) and value for value in states
    ):
        errors.append("runtime/widget.json.states 必须是 1-6 个非空字符串")
    else:
        state_ids = states
        if len(state_ids) != len(set(state_ids)):
            errors.append("runtime/widget.json.states 必须唯一")
        if runtime.get("initial_state") not in state_ids:
            errors.append("runtime/widget.json.initial_state 必须引用 states")
    pages = runtime.get("pages")
    page_ids: list[str] = []
    if pages is not None:
        if not isinstance(pages, list) or not 1 <= len(pages) <= 4:
            errors.append("runtime/widget.json.pages 必须是 1-4 个 page 对象")
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
    elif len(transitions) > 12:
        errors.append(f"runtime/widget.json.transitions 最多 12 条，当前为 {len(transitions)}")
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
        if page is not None and page not in {*page_ids, "next", "prev"}:
            errors.append(f"transitions[{index}].page 引用了未知 page")
        if effect_count(transition) > 4:
            errors.append(f"transitions[{index}] 的 set+inc 超过 4 项")
    ticks = runtime.get("tick")
    if not isinstance(ticks, list):
        errors.append("runtime/widget.json.tick 必须是数组")
        ticks = []
    elif len(ticks) > 8:
        errors.append(f"runtime/widget.json.tick 最多 8 条，当前为 {len(ticks)}")
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
    validate_dashboard(
        runtime.get("dashboard"),
        "runtime/widget.json.dashboard",
        kind,
        runtime_dashboard=True,
        errors=errors,
    )
    if compact_json_size(runtime) > 4095:
        errors.append(f"runtime/widget.json 紧凑后超过 4095 字节: {compact_json_size(runtime)}")
    game = runtime.get("game")
    scene = runtime.get("scene")
    if game is not None and scene is not None:
        errors.append("runtime/widget.json.scene 与旧版 game 不能同时存在")
    if game is not None:
        validate_game(game, runtime, kind, capabilities, transition_actions, errors)
    if scene is not None:
        validate_scene(scene, runtime, capabilities, transition_actions, errors)
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
    if capabilities.get("widgetRuntime") not in {"p4-bounded-v2", RUNTIME_ENGINE}:
        errors.append(f"目标能力不支持旧版原生游戏预设: {capabilities.get('widgetRuntime')!r}")
    widget_games = capabilities.get("widgetGamePresets", capabilities.get("widgetGames"))
    if not isinstance(widget_games, list) or game_type not in widget_games:
        errors.append(f"目标设备能力未声明 widgetGamePresets.{game_type}")
    tick_ms = game.get("tick_ms")
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
            if SW_HOLD_PATTERN.fullmatch(event):
                errors.append(f"SW1/SW2/SW3 不支持长按或 hold: {event}")
            if event not in ALLOWED_EVENTS:
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
    unbound = sorted(transition_actions - normal_actions)
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
    capabilities = capabilities or load_capabilities(None)
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
        )
    transition_actions = validate_runtime(runtime, kind, capabilities, errors)
    validate_buttons(buttons, transition_actions, capabilities, runtime, errors)
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
