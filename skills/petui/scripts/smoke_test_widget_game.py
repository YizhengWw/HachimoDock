#!/usr/bin/env python3
"""Deterministically exercise a generated PetUI game's complete playable loop.

The simulator mirrors the bounded P4 scene primitives closely enough to reject
packages whose JSON is valid but whose controls, movement, scoring, result, or
restart path cannot be reached through actual play.
"""

from __future__ import annotations

import argparse
import copy
import json
import sys
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from validate_generated_widget import load_capabilities, validate_widget


MAX_SEARCH_DEPTH = 32
MAX_SEARCH_STATES = 12_000
SYSTEM_TRIGGERS = {"tick", "collision", "edge", "blocked"}


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def clamp(value: int, minimum: int, maximum: int) -> int:
    return max(minimum, min(maximum, value))


def stable_widget_seed(widget_id: str) -> int:
    value = 2166136261
    for byte in widget_id.encode("utf-8"):
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def initial_variables(runtime: dict[str, Any]) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for name, declaration in runtime.get("vars", {}).items():
        if isinstance(declaration, dict):
            values[name] = declaration.get("init", 0 if declaration.get("type") == "int" else "")
    return values


def initial_entities(scene: dict[str, Any]) -> dict[str, dict[str, Any]]:
    entities: dict[str, dict[str, Any]] = {}
    for declaration in scene.get("entities", []):
        entity = {
            "id": declaration["id"],
            "x": declaration["x"],
            "y": declaration["y"],
            "width": declaration.get("width", 1),
            "height": declaration.get("height", 1),
            "tone": declaration.get("tone", 1),
            "vx": declaration.get("vx", 0),
            "vy": declaration.get("vy", 0),
            "bounds": declaration.get("bounds", "clamp"),
            "active": declaration.get("active", True),
            "collidable": declaration.get("collidable", True),
        }
        entities[entity["id"]] = entity
    return entities


@dataclass
class PlayState:
    runtime_state: str
    page: str
    variables: dict[str, Any]
    entities: dict[str, dict[str, Any]]
    score: int
    running: bool
    game_over: bool
    rng: int

    def clone(self) -> "PlayState":
        return copy.deepcopy(self)


class SceneSimulator:
    def __init__(
        self,
        runtime: dict[str, Any],
        buttons: list[dict[str, Any]],
        widget_id: str,
    ) -> None:
        self.runtime = runtime
        self.scene = runtime["scene"]
        self.buttons = buttons
        self.actions = [binding["action"] for binding in buttons]
        self.grid = self.scene["grid"]
        self.width = self.grid["width"]
        self.height = self.grid["height"]
        rows = self.grid.get("rows") or ["0" * self.width for _ in range(self.height)]
        self.cells = [[int(cell) for cell in row] for row in rows]
        self.solid_tones = set(self.grid.get("solid_tones", []))
        self.base_entities = initial_entities(self.scene)
        self.rules = [rule for rule in self.scene.get("rules", []) if isinstance(rule, dict)]
        self.transitions = [
            transition
            for transition in runtime.get("transitions", [])
            if isinstance(transition, dict)
        ]
        self.result_state = self.scene.get("result_state")
        self.score_var = self.scene.get("score_var")
        self.active_state = self.scene["active_state"]
        self.seed = stable_widget_seed(widget_id)
        self.start_actions = {
            rule.get("on")
            for rule in self.rules
            if any(
                operation.get("op") in {"run", "restart"}
                for operation in rule.get("do", [])
                if isinstance(operation, dict)
            )
        }

    def potentially_immediate_actions(self) -> set[str]:
        visible_operations = {
            "move",
            "place",
            "show",
            "hide",
            "score",
            "run",
            "stop",
            "restart",
            "tone",
        }
        visible: set[str] = set()
        for action in self.actions:
            if any(
                transition.get("on") == action
                and any(key in transition for key in ("to", "page", "set", "inc"))
                for transition in self.transitions
            ):
                visible.add(action)
                continue
            if any(
                rule.get("on") == action
                and any(
                    operation.get("op") in visible_operations
                    for operation in rule.get("do", [])
                    if isinstance(operation, dict)
                )
                for rule in self.rules
            ):
                visible.add(action)
        return visible

    def initial(self) -> PlayState:
        return PlayState(
            runtime_state=self.runtime["initial_state"],
            page=self.runtime.get("initial_page", ""),
            variables=initial_variables(self.runtime),
            entities=copy.deepcopy(self.base_entities),
            score=0,
            running=bool(self.scene.get("auto_start", False)),
            game_over=False,
            rng=self.seed,
        )

    def state_key(self, state: PlayState) -> tuple[Any, ...]:
        entity_key = tuple(
            (
                entity_id,
                entity["x"],
                entity["y"],
                entity["vx"],
                entity["vy"],
                entity["tone"],
                entity["active"],
            )
            for entity_id, entity in sorted(state.entities.items())
        )
        return (
            state.runtime_state,
            state.page,
            tuple(sorted(state.variables.items())),
            entity_key,
            state.score,
            state.running,
            state.game_over,
            state.rng,
        )

    def visible_key(self, state: PlayState) -> tuple[Any, ...]:
        entity_key = tuple(
            (entity_id, entity["x"], entity["y"], entity["tone"], entity["active"])
            for entity_id, entity in sorted(state.entities.items())
        )
        return (
            state.runtime_state,
            state.page,
            tuple(sorted(state.variables.items())),
            entity_key,
            state.score,
            state.running,
            state.game_over,
        )

    def transition_matches(self, transition: dict[str, Any], state: PlayState, action: str) -> bool:
        return transition.get("on") == action and transition.get("from") in {
            "*",
            state.runtime_state,
        }

    def apply_effects(self, state: PlayState, effect: dict[str, Any]) -> None:
        for name, value in effect.get("set", {}).items():
            state.variables[name] = value
        for name, value in effect.get("inc", {}).items():
            state.variables[name] = int(state.variables.get(name, 0)) + int(value)

    def apply_transition(self, state: PlayState, action: str) -> bool:
        for transition in self.transitions:
            if not self.transition_matches(transition, state, action):
                continue
            if isinstance(transition.get("to"), str):
                state.runtime_state = transition["to"]
            page = transition.get("page")
            pages = [item["id"] for item in self.runtime.get("pages", [])]
            if page in pages:
                state.page = page
            elif page == "next" and pages:
                state.page = pages[(pages.index(state.page) + 1) % len(pages)]
            elif page == "prev" and pages:
                state.page = pages[(pages.index(state.page) - 1) % len(pages)]
            self.apply_effects(state, transition)
            return True
        return False

    def rule_matches(
        self,
        rule: dict[str, Any],
        trigger: str,
        *,
        action: str = "",
        entity: str = "",
        other: str = "",
        edge: str = "",
    ) -> bool:
        if rule.get("on") != trigger:
            return False
        if trigger not in SYSTEM_TRIGGERS:
            return trigger == action
        if trigger == "collision":
            pair = {rule.get("entity"), rule.get("with")}
            return pair == {entity, other}
        if trigger in {"edge", "blocked"}:
            if rule.get("entity") != entity:
                return False
            return trigger == "blocked" or rule.get("edge", "any") in {"any", edge}
        return True

    def execute_rules(
        self,
        state: PlayState,
        trigger: str,
        *,
        action: str = "",
        entity: str = "",
        other: str = "",
        edge: str = "",
    ) -> None:
        for rule in self.rules:
            if self.rule_matches(
                rule,
                trigger,
                action=action,
                entity=entity,
                other=other,
                edge=edge,
            ):
                for operation in rule.get("do", []):
                    self.apply_operation(state, operation)

    def solid_hit(self, entity: dict[str, Any], x: int, y: int) -> bool:
        if not self.solid_tones:
            return False
        for dy in range(entity["height"]):
            for dx in range(entity["width"]):
                cell_x, cell_y = x + dx, y + dy
                if 0 <= cell_x < self.width and 0 <= cell_y < self.height:
                    if self.cells[cell_y][cell_x] in self.solid_tones:
                        return True
        return False

    def move_entity(self, state: PlayState, entity_id: str, dx: int, dy: int) -> None:
        entity = state.entities[entity_id]
        if not entity["active"] or (dx == 0 and dy == 0):
            return
        old_x, old_y = entity["x"], entity["y"]
        next_x, next_y = old_x + dx, old_y + dy
        max_x = self.width - entity["width"]
        max_y = self.height - entity["height"]
        edges: list[str] = []
        if next_x < 0:
            edges.append("left")
        elif next_x > max_x:
            edges.append("right")
        if next_y < 0:
            edges.append("top")
        elif next_y > max_y:
            edges.append("bottom")
        if edges:
            if entity["bounds"] == "wrap":
                next_x = max_x if next_x < 0 else 0 if next_x > max_x else next_x
                next_y = max_y if next_y < 0 else 0 if next_y > max_y else next_y
            else:
                next_x, next_y = clamp(next_x, 0, max_x), clamp(next_y, 0, max_y)
                if entity["bounds"] == "bounce":
                    if any(edge in {"left", "right"} for edge in edges):
                        entity["vx"] = -entity["vx"]
                    if any(edge in {"top", "bottom"} for edge in edges):
                        entity["vy"] = -entity["vy"]
                elif entity["bounds"] == "hide":
                    entity["active"] = False
                elif entity["bounds"] == "stop":
                    state.running = False
                    state.game_over = True
            for edge in edges:
                self.execute_rules(state, "edge", entity=entity_id, edge=edge)
        if entity["active"] and self.solid_hit(entity, next_x, next_y):
            next_x, next_y = old_x, old_y
            if entity["bounds"] == "bounce":
                if dx:
                    entity["vx"] = -entity["vx"]
                if dy:
                    entity["vy"] = -entity["vy"]
            self.execute_rules(state, "blocked", entity=entity_id)
        entity["x"], entity["y"] = next_x, next_y

    def apply_operation(self, state: PlayState, operation: dict[str, Any]) -> None:
        name = operation["op"]
        entity = state.entities.get(operation.get("entity"))
        if name == "move" and entity:
            self.move_entity(state, entity["id"], operation["dx"], operation["dy"])
        elif name == "velocity" and entity:
            entity["vx"], entity["vy"] = operation["vx"], operation["vy"]
        elif name == "accelerate" and entity:
            entity["vx"] = clamp(entity["vx"] + operation["vx"], -4, 4)
            entity["vy"] = clamp(entity["vy"] + operation["vy"], -4, 4)
        elif name == "place" and entity:
            source = state.entities.get(operation.get("source"))
            x = source["x"] + operation.get("dx", 0) if source else entity["x"]
            y = source["y"] + operation.get("dy", 0) if source else entity["y"]
            if "x" in operation:
                if isinstance(operation["x"], list):
                    state.rng = (state.rng * 1664525 + 1013904223) & 0xFFFFFFFF
                    x = operation["x"][0] + state.rng % (
                        operation["x"][1] - operation["x"][0] + 1
                    )
                else:
                    x = operation["x"]
            if "y" in operation:
                if isinstance(operation["y"], list):
                    state.rng = (state.rng * 1664525 + 1013904223) & 0xFFFFFFFF
                    y = operation["y"][0] + state.rng % (
                        operation["y"][1] - operation["y"][0] + 1
                    )
                else:
                    y = operation["y"]
            entity["x"] = clamp(x, 0, self.width - entity["width"])
            entity["y"] = clamp(y, 0, self.height - entity["height"])
            entity["active"] = True
        elif name == "show" and entity:
            entity["active"] = True
        elif name == "hide" and entity:
            entity["active"] = False
        elif name == "score":
            state.score = int(operation.get("set", state.score + operation.get("add", 0)))
            state.score = clamp(state.score, -1_000_000_000, 1_000_000_000)
            if self.score_var:
                state.variables[self.score_var] = state.score
        elif name == "run":
            state.running, state.game_over = True, False
        elif name == "stop":
            state.running, state.game_over = False, True
        elif name == "restart":
            state.entities = copy.deepcopy(self.base_entities)
            state.score = 0
            if self.score_var:
                state.variables[self.score_var] = 0
            state.running, state.game_over = True, False
        elif name == "bounce" and entity:
            if operation["axis"] in {"x", "both"}:
                entity["vx"] = -entity["vx"]
            if operation["axis"] in {"y", "both"}:
                entity["vy"] = -entity["vy"]
        elif name == "tone" and entity:
            entity["tone"] = operation["tone"]

    @staticmethod
    def overlap(left: dict[str, Any], right: dict[str, Any]) -> bool:
        return (
            left["active"]
            and right["active"]
            and left["collidable"]
            and right["collidable"]
            and left["x"] < right["x"] + right["width"]
            and left["x"] + left["width"] > right["x"]
            and left["y"] < right["y"] + right["height"]
            and left["y"] + left["height"] > right["y"]
        )

    def collisions(self, state: PlayState) -> None:
        identifiers = list(state.entities)
        for left_index, left_id in enumerate(identifiers):
            for right_id in identifiers[left_index + 1 :]:
                if self.overlap(state.entities[left_id], state.entities[right_id]):
                    self.execute_rules(
                        state,
                        "collision",
                        entity=left_id,
                        other=right_id,
                    )

    def sync_result(self, state: PlayState) -> None:
        if state.game_over and self.result_state:
            state.runtime_state = self.result_state
        if self.score_var:
            state.variables[self.score_var] = state.score

    def action(self, source: PlayState, action: str) -> PlayState:
        state = source.clone()
        if not self.apply_transition(state, action):
            return state
        self.execute_rules(state, action, action=action)
        self.collisions(state)
        self.sync_result(state)
        return state

    def tick(self, source: PlayState) -> PlayState:
        state = source.clone()
        if not state.running or state.game_over or state.runtime_state != self.active_state:
            return state
        self.execute_rules(state, "tick")
        for entity_id in list(state.entities):
            entity = state.entities[entity_id]
            if entity["active"] and (entity["vx"] or entity["vy"]):
                self.move_entity(state, entity_id, entity["vx"], entity["vy"])
        self.collisions(state)
        self.sync_result(state)
        return state


def scene_rule_operations(scene: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        operation
        for rule in scene.get("rules", [])
        if isinstance(rule, dict)
        for operation in rule.get("do", [])
        if isinstance(operation, dict)
    ]


def smoke_test_game(widget_dir: Path) -> dict[str, Any]:
    runtime = load_json(widget_dir / "runtime" / "widget.json")
    manifest = load_json(widget_dir / "component.json")
    buttons = load_json(widget_dir / "buttons.json")
    if manifest.get("kind") != "game":
        return {"ok": True, "skipped": True, "reason": "component is not a game"}
    if not isinstance(runtime.get("scene"), dict):
        if runtime.get("engine") != "p4-bounded-runtime-v4":
            return {
                "ok": True,
                "skipped": True,
                "reason": "legacy preset game is validated by its compatibility contract",
            }
        return {"ok": False, "errors": ["玩法自测仅接受使用通用 scene 的新游戏"]}

    simulator = SceneSimulator(runtime, buttons, str(manifest.get("id", "")))
    potentially_immediate = simulator.potentially_immediate_actions()
    initial = simulator.initial()
    operations = scene_rule_operations(simulator.scene)
    requires_ticks = any(
        entity.get("vx", 0) or entity.get("vy", 0)
        for entity in simulator.scene.get("entities", [])
    ) or any(rule.get("on") == "tick" for rule in simulator.rules)
    requires_score = any(operation.get("op") == "score" for operation in operations)
    requires_result = bool(simulator.result_state) or any(
        operation.get("op") == "stop" for operation in operations
    )
    if not requires_score and not requires_result:
        return {"ok": False, "errors": ["游戏没有可验证的得分、成功或失败结算"]}

    queue: deque[tuple[PlayState, tuple[str, ...]]] = deque([(initial, ())])
    visited = {simulator.state_key(initial)}
    effective_actions: set[str] = set()
    immediate_actions: set[str] = set()
    tick_advanced = False
    score_reached = False
    result_reached: tuple[PlayState, tuple[str, ...]] | None = None
    start_reached = False
    outcome_path: tuple[str, ...] = ()
    score_path: tuple[str, ...] = ()
    result_path: tuple[str, ...] = ()

    while queue and len(visited) <= MAX_SEARCH_STATES:
        state, path = queue.popleft()
        if len(path) >= MAX_SEARCH_DEPTH:
            continue
        if state.running and state.runtime_state == simulator.active_state:
            start_reached = True
        if state.score != 0:
            score_reached = True
            outcome_path = outcome_path or path
            score_path = score_path or path
        if state.game_over or (
            simulator.result_state and state.runtime_state == simulator.result_state
        ):
            result_reached = result_reached or (state, path)
            outcome_path = outcome_path or path
            result_path = result_path or path

        ticked = simulator.tick(state)
        if simulator.visible_key(ticked) != simulator.visible_key(state):
            tick_advanced = True
        tick_key = simulator.state_key(ticked)
        if tick_key not in visited:
            visited.add(tick_key)
            queue.append((ticked, (*path, "tick")))

        for action in simulator.actions:
            acted = simulator.action(state, action)
            baseline = simulator.tick(state)
            acted_then_tick = simulator.tick(acted)
            if state.running and state.runtime_state == simulator.active_state:
                if simulator.visible_key(acted) != simulator.visible_key(state):
                    immediate_actions.add(action)
                if simulator.visible_key(acted_then_tick) != simulator.visible_key(baseline):
                    effective_actions.add(action)
            elif action in simulator.start_actions and (
                acted.running and acted.runtime_state == simulator.active_state
            ):
                immediate_actions.add(action)
                effective_actions.add(action)
            action_key = simulator.state_key(acted)
            if action_key not in visited:
                visited.add(action_key)
                queue.append((acted, (*path, action)))

        if (
            start_reached
            and effective_actions == set(simulator.actions)
            and immediate_actions == potentially_immediate
            and (tick_advanced or not requires_ticks)
            and (score_reached or not requires_score)
            and (result_reached is not None or not requires_result)
        ):
            break

    errors: list[str] = []
    if not start_reached:
        errors.append("开始动作无法让游戏进入运行状态")
    missing_actions = sorted(set(simulator.actions) - effective_actions)
    if missing_actions:
        errors.append(f"以下输入在可达局面中没有产生独立可见效果: {', '.join(missing_actions)}")
    delayed_actions = sorted(set(simulator.actions) - immediate_actions)
    if delayed_actions:
        errors.append(
            "以下输入只改变隐藏速度/内部状态或仅在未开始时生效，按下后没有即时可见反馈: "
            + ", ".join(delayed_actions)
        )
    if requires_ticks and not tick_advanced:
        errors.append("游戏声明了自动运动或 tick 规则，但连续运行时画面没有推进")
    if requires_score and not score_reached:
        errors.append("游戏声明了得分规则，但自动探索无法通过真实玩法改变得分")
    if requires_result and result_reached is None:
        errors.append("游戏声明了结束条件，但自动探索无法到达成功/失败结算")

    restart_path: tuple[str, ...] = ()
    if result_reached is not None:
        result_state, result_path = result_reached
        for action in simulator.actions:
            restarted = simulator.action(result_state, action)
            if restarted.running and not restarted.game_over and restarted.runtime_state == simulator.active_state:
                restart_path = (*result_path, action)
                break
        if not restart_path:
            errors.append("游戏结束后没有可用的重新开始闭环")

    return {
        "ok": not errors,
        "componentId": manifest.get("id"),
        "visitedStates": len(visited),
        "effectiveActions": sorted(effective_actions),
        "immediateActions": sorted(immediate_actions),
        "tickAdvanced": tick_advanced,
        "scoreReached": score_reached,
        "resultReached": result_reached is not None,
        "outcomePath": list(outcome_path),
        "scorePath": list(score_path),
        "resultPath": list(result_path),
        "restartPath": list(restart_path),
        "errors": errors,
    }


def run_smoke_test(
    widget_dir: Path,
    capabilities: dict[str, Any] | None = None,
) -> dict[str, Any]:
    widget_dir = widget_dir.expanduser().resolve()
    validation_errors = validate_widget(widget_dir, capabilities)
    if validation_errors:
        return {"ok": False, "path": str(widget_dir), "errors": validation_errors}
    try:
        result = smoke_test_game(widget_dir)
    except (KeyError, TypeError, ValueError) as error:
        result = {"ok": False, "errors": [f"玩法模拟失败: {error}"]}
    return {"path": str(widget_dir), **result}


def main() -> int:
    parser = argparse.ArgumentParser(description="模拟并验证 PetUI 游戏完整玩法闭环")
    parser.add_argument("widget_dir", type=Path)
    parser.add_argument("--capabilities", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    result = run_smoke_test(args.widget_dir, load_capabilities(args.capabilities))
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    elif result.get("ok"):
        print(
            f"PLAYABLE {result['path']} "
            f"states={result.get('visitedStates', 0)} "
            f"outcome={result.get('outcomePath', [])}"
        )
    else:
        print(f"UNPLAYABLE {result['path']}", file=sys.stderr)
        for error in result.get("errors", []):
            print(f"- {error}", file=sys.stderr)
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
