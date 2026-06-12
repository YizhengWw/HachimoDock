#!/usr/bin/env python3
"""board-widget-runtime — interpret .clawpkg widgets on-device.

Watches /opt/board-runtime/.active-widget. When set to <id>, loads
widgets/<id>/runtime/widget.json + widgets/<id>/buttons.json, validates,
and runs the declarative state machine. Subscribes to .current-event for
hardware input, schedules ticks/fetchers/readers as asyncio coroutines,
emits .stats-display whenever state/vars/page change so fb-display.sh +
fb-stats-renderer.py pick it up.

Designed for skill-generated widget.json — Turing-incomplete grammar so
the LLM cannot inject arbitrary behavior. See
docs/superpowers/specs/2026-05-25-widget-runtime-design.md.
"""
import argparse
import asyncio
import json
import logging
import os
import re
import shutil
import signal
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Optional

LOG = logging.getLogger("board-widget-runtime")

# Hardcoded read-whitelist prefix. Any reader path is resolved relative to
# RUNTIME_ROOT and must START with one of these prefixes (after resolution).
READER_PATH_WHITELIST = (
    ".stats-display",
    ".token-stats",
    ".current-speech",
    ".screen-page",
    ".active-widget",
    ".debug-",
)

# Fetcher URL allowlist: list of hosts. Loaded from RUNTIME_ROOT/widget-runtime.conf
# (one host per line). Default empty = fetchers disabled.

TICK_MIN_MS = 100
FETCHER_MIN_S = 30
READER_MIN_S = 1


# ───────────────────────── Validation ─────────────────────────

class ValidationError(Exception):
    pass


def _require(cond: bool, msg: str) -> None:
    if not cond:
        raise ValidationError(msg)


def validate_widget(spec: dict) -> None:
    """Strict structural validation of widget.json. Run on every load.

    Anything not in the grammar (extra keys, unknown var refs, malformed
    transitions) raises ValidationError — the runtime then refuses to
    activate the widget rather than silently degrading."""
    _require(isinstance(spec, dict), "widget spec must be an object")
    _require(spec.get("schema_version") == 1, "schema_version must be 1")

    states = spec.get("states")
    _require(isinstance(states, list) and states, "states must be a non-empty list")
    state_set = set(states)
    _require(all(isinstance(s, str) for s in states), "state ids must be strings")
    _require(spec.get("initial_state") in state_set,
             f"initial_state must be one of {sorted(state_set)}")

    vars_decl = spec.get("vars", {})
    _require(isinstance(vars_decl, dict), "vars must be an object")
    for name, decl in vars_decl.items():
        _require(isinstance(name, str), "var name must be string")
        _require(isinstance(decl, dict), f"var {name} decl must be object")
        _require(decl.get("type") in ("int", "string"),
                 f"var {name}.type must be int or string")

    pages_decl = spec.get("pages")
    page_ids: set = set()
    if pages_decl is not None:
        _require(isinstance(pages_decl, list) and pages_decl, "pages must be non-empty list")
        for p in pages_decl:
            _require(isinstance(p, dict) and "id" in p, "page must have id")
            page_ids.add(p["id"])
        _require(spec.get("initial_page") in page_ids,
                 f"initial_page must be one of {sorted(page_ids)}")

    transitions = spec.get("transitions", [])
    _require(isinstance(transitions, list), "transitions must be a list")
    for i, t in enumerate(transitions):
        _require(isinstance(t, dict), f"transitions[{i}] must be object")
        f = t.get("from")
        _require(f == "*" or f in state_set,
                 f"transitions[{i}].from must be '*' or known state")
        _require(isinstance(t.get("on"), str), f"transitions[{i}].on must be string action name")
        to = t.get("to")
        if to is not None:
            _require(to in state_set, f"transitions[{i}].to must be known state")
        for op in ("set", "inc"):
            if op in t:
                _require(isinstance(t[op], dict), f"transitions[{i}].{op} must be object")
                for k in t[op]:
                    _require(k in vars_decl, f"transitions[{i}].{op} references unknown var {k}")
        if "page" in t:
            pg = t["page"]
            _require(pg in ("next", "prev") or pg in page_ids,
                     f"transitions[{i}].page must be next/prev or known page id")

    tick = spec.get("tick", [])
    _require(isinstance(tick, list), "tick must be a list")
    for i, tk in enumerate(tick):
        _require(isinstance(tk, dict), f"tick[{i}] must be object")
        every = tk.get("every_ms")
        _require(isinstance(every, int) and every >= TICK_MIN_MS,
                 f"tick[{i}].every_ms must be int >= {TICK_MIN_MS}")
        if "while_state" in tk:
            _require(tk["while_state"] in state_set,
                     f"tick[{i}].while_state must be known state")
        for op in ("set", "inc"):
            if op in tk:
                _require(isinstance(tk[op], dict), f"tick[{i}].{op} must be object")
                for k in tk[op]:
                    _require(k in vars_decl, f"tick[{i}].{op} references unknown var {k}")

    fetchers = spec.get("fetchers", {})
    _require(isinstance(fetchers, dict), "fetchers must be an object")
    for fid, fdef in fetchers.items():
        _require(isinstance(fdef, dict), f"fetchers.{fid} must be object")
        url = fdef.get("url", "")
        _require(isinstance(url, str) and url.startswith("https://"),
                 f"fetchers.{fid}.url must be https:// string")
        every = fdef.get("every_s")
        _require(isinstance(every, int) and every >= FETCHER_MIN_S,
                 f"fetchers.{fid}.every_s must be int >= {FETCHER_MIN_S}")
        _require(fdef.get("parse") in ("json", "text"),
                 f"fetchers.{fid}.parse must be json or text")
        _require(fdef.get("into") in vars_decl,
                 f"fetchers.{fid}.into must be a declared var")

    readers = spec.get("readers", {})
    _require(isinstance(readers, dict), "readers must be an object")
    for rid, rdef in readers.items():
        _require(isinstance(rdef, dict), f"readers.{rid} must be object")
        path = rdef.get("path", "")
        _require(isinstance(path, str), f"readers.{rid}.path must be string")
        _require(any(path.startswith(p) for p in READER_PATH_WHITELIST),
                 f"readers.{rid}.path {path!r} not in whitelist {READER_PATH_WHITELIST}")
        every = rdef.get("every_s")
        _require(isinstance(every, int) and every >= READER_MIN_S,
                 f"readers.{rid}.every_s must be int >= {READER_MIN_S}")
        _require(rdef.get("into") in vars_decl,
                 f"readers.{rid}.into must be a declared var")

    dashboard = spec.get("dashboard", {})
    _require(isinstance(dashboard, dict), "dashboard must be an object")
    allowed_slots = {"title", "eyebrow", "headline", "metricLabel", "metricValue",
                     "metricUnit", "badge", "note", "footer", "progress"}
    for slot, rule in dashboard.items():
        _require(slot in allowed_slots,
                 f"dashboard slot {slot!r} not in {sorted(allowed_slots)}")
        if isinstance(rule, str):
            continue  # literal
        _require(isinstance(rule, dict), f"dashboard.{slot} rule must be string or object")
        if "switch_state" in rule:
            for k in rule["switch_state"]:
                _require(k in state_set,
                         f"dashboard.{slot}.switch_state has unknown state {k}")
        elif "switch_page" in rule:
            for k in rule["switch_page"]:
                _require(k in page_ids,
                         f"dashboard.{slot}.switch_page has unknown page {k}")
        elif "fmt_mmss" in rule or "fmt_hms" in rule:
            ref = rule.get("fmt_mmss") or rule.get("fmt_hms")
            _require(ref in vars_decl,
                     f"dashboard.{slot} formatter references unknown var {ref}")
        elif "var" in rule:
            _require(rule["var"] in vars_decl,
                     f"dashboard.{slot}.var references unknown var {rule['var']}")
        elif "pct_of" in rule:
            _require(rule["pct_of"] in vars_decl,
                     f"dashboard.{slot}.pct_of references unknown var {rule['pct_of']}")
            of_max = rule.get("of_max")
            _require(of_max is not None and of_max in vars_decl,
                     f"dashboard.{slot}.pct_of requires of_max var, got {of_max!r}")
        else:
            raise ValidationError(f"dashboard.{slot} has no recognized rule key")


def validate_buttons(buttons: list, transitions: list) -> None:
    """buttons.json shape check + cross-check that every action referenced by
    transitions has at least one binding entry (otherwise the transition is
    unreachable and very likely a skill mistake)."""
    _require(isinstance(buttons, list), "buttons.json must be a list")
    actions_in_buttons: set = set()
    for i, b in enumerate(buttons):
        _require(isinstance(b, dict), f"buttons[{i}] must be object")
        for key in ("action", "control", "event", "label"):
            _require(isinstance(b.get(key), str) and b[key],
                     f"buttons[{i}].{key} must be non-empty string")
        actions_in_buttons.add(b["action"])
    actions_in_transitions = {t["on"] for t in transitions}
    missing = actions_in_transitions - actions_in_buttons
    if missing:
        # not a hard failure — those transitions are simply unreachable
        LOG.warning("transitions reference actions with no button binding: %s", missing)


# ───────────────────────── Interpreter ─────────────────────────

def _fmt_mmss(total_seconds: int) -> str:
    s = max(0, int(total_seconds))
    return f"{s // 60:02d}:{s % 60:02d}"


def _fmt_hms(total_seconds: int) -> str:
    s = max(0, int(total_seconds))
    return f"{s // 3600}:{(s // 60) % 60:02d}:{s % 60:02d}"


class WidgetRuntime:
    """Holds the live widget state and writes .stats-display on every change."""

    def __init__(self, root: Path):
        self.root = root
        # Widget output goes to .widget-display, NOT .stats-display. board-server
        # owns .stats-display and writes Codex/Claude token-usage content there
        # periodically — writing widget content to the same file would get
        # overwritten on the next board-server tick. fb-stats-renderer.py reads
        # .widget-display when a widget is active and falls back to .stats-display
        # otherwise.
        self.stats_display = root / ".widget-display"
        self.active_widget_file = root / ".active-widget"
        # .widget-events: append-only JSON-line file fed by either the C
        # input drivers (board_rotary_input / board_touch_input, planned for
        # P2) or by SSH-injected test events. Each line:
        #   {"control": "屏幕区域", "event": "screen.region.tap"}
        # The runtime tails it via byte-offset polling (200ms).
        self.widget_events_file = root / ".widget-events"
        self.spec: Optional[dict] = None
        self.buttons: Optional[list] = None
        self.state: Optional[str] = None
        self.page: Optional[str] = None
        self.vars: dict = {}
        self.tasks: list[asyncio.Task] = []
        self.widget_id: Optional[str] = None

    # ── load / unload ──

    def load(self, widget_id: str) -> None:
        widget_dir = self.root / "widgets" / widget_id
        spec_path = widget_dir / "runtime" / "widget.json"
        buttons_path = widget_dir / "buttons.json"
        if not spec_path.exists():
            raise FileNotFoundError(f"widget.json missing: {spec_path}")
        spec = json.loads(spec_path.read_text(encoding="utf-8"))
        validate_widget(spec)
        buttons = []
        if buttons_path.exists():
            buttons = json.loads(buttons_path.read_text(encoding="utf-8"))
            validate_buttons(buttons, spec.get("transitions", []))
        self.spec = spec
        self.buttons = buttons
        self.widget_id = widget_id
        self.state = spec["initial_state"]
        self.page = spec.get("initial_page")
        self.vars = {}
        for name, decl in spec.get("vars", {}).items():
            self.vars[name] = decl.get("init", 0 if decl["type"] == "int" else "")
        # Restore persisted state if file exists AND was written by the same
        # widget_id (per-widget file, so switching widget always resets).
        # Vars get overlaid from snapshot; unknown vars in snapshot are ignored
        # (handles widget schema changes between sessions).
        self._restore_state()
        LOG.info("loaded widget %s (state=%s, page=%s, vars=%s)",
                 widget_id, self.state, self.page, self.vars)

    async def unload(self) -> None:
        for t in self.tasks:
            t.cancel()
        if self.tasks:
            await asyncio.gather(*self.tasks, return_exceptions=True)
        self.tasks = []
        self.spec = None
        self.buttons = None
        self.state = None
        self.page = None
        self.vars = {}
        self.widget_id = None

    # ── input dispatch ──

    def _lookup_action(self, control: str, event: str) -> Optional[str]:
        if not self.buttons:
            return None
        for b in self.buttons:
            if b.get("control") == control and b.get("event") == event:
                return b.get("action")
        return None

    def _apply_transition(self, t: dict) -> bool:
        """Apply one transition's effects. Returns True if anything changed."""
        changed = False
        if "to" in t and t["to"] != self.state:
            self.state = t["to"]; changed = True
        for k, v in t.get("set", {}).items():
            if self.vars.get(k) != v:
                self.vars[k] = v; changed = True
        for k, d in t.get("inc", {}).items():
            self.vars[k] = self.vars.get(k, 0) + d; changed = True
        if "page" in t:
            pages = [p["id"] for p in self.spec.get("pages", [])]
            if pages:
                idx = pages.index(self.page) if self.page in pages else 0
                if t["page"] == "next":
                    self.page = pages[(idx + 1) % len(pages)]
                elif t["page"] == "prev":
                    self.page = pages[(idx - 1) % len(pages)]
                else:
                    self.page = t["page"]
                changed = True
        return changed

    def dispatch_action(self, action: str) -> bool:
        """Apply the first matching transition. Returns True if anything changed."""
        for t in self.spec.get("transitions", []):
            if t["on"] != action:
                continue
            if t["from"] != "*" and t["from"] != self.state:
                continue
            if self._apply_transition(t):
                LOG.debug("transition fired: %s in %s", action, t["from"])
                return True
            return False
        LOG.debug("no transition matched action %s in state %s", action, self.state)
        return False

    # ── rendering ──

    def _resolve_slot(self, rule) -> str:
        if rule is None:
            return ""
        if isinstance(rule, str):
            return rule
        if "switch_state" in rule:
            return self._resolve_slot(rule["switch_state"].get(self.state, ""))
        if "switch_page" in rule:
            return self._resolve_slot(rule["switch_page"].get(self.page, ""))
        if "fmt_mmss" in rule:
            return _fmt_mmss(self.vars.get(rule["fmt_mmss"], 0))
        if "fmt_hms" in rule:
            return _fmt_hms(self.vars.get(rule["fmt_hms"], 0))
        if "var" in rule:
            return str(self.vars.get(rule["var"], ""))
        if "pct_of" in rule:
            # Compute progress percentage from two vars and emit "<pct>:<label>"
            # consumable by fb-stats-renderer.py's progress-bar branch.
            #   { "pct_of": "since_last_min", "of_max": "interval_min",
            #     "label": "本次间隔" }
            val = self.vars.get(rule["pct_of"], 0)
            mx = self.vars.get(rule.get("of_max", ""), 0)
            try:
                num = int(val); denom = int(mx)
                pct = 0 if denom <= 0 else max(0, min(100, (num * 100) // denom))
            except (ValueError, TypeError):
                pct = 0
            label = str(rule.get("label", ""))
            return f"{pct}:{label}"
        return ""

    def render_payload(self) -> str:
        out = ["COMPONENT_DASHBOARD_V1"]
        slot_order = ["title", "eyebrow", "headline", "metricLabel", "metricValue",
                      "metricUnit", "badge", "note", "footer", "progress"]
        for slot in slot_order:
            rule = self.spec.get("dashboard", {}).get(slot)
            if rule is None:
                continue
            value = self._resolve_slot(rule)
            if value:
                out.append(f"{slot}={value}")
        return "\n".join(out) + "\n"

    def write_payload(self) -> None:
        payload = self.render_payload()
        tmp = self.stats_display.with_suffix(".stats-display.tmp")
        tmp.write_text(payload, encoding="utf-8")
        tmp.replace(self.stats_display)
        LOG.debug("wrote .stats-display (%d bytes)", len(payload))
        self._persist_state()

    def _state_file(self) -> Path:
        # per-widget so changing widget never resurrects another widget's vars
        return self.root / f".widget-state-{self.widget_id}.json"

    def _persist_state(self) -> None:
        """Snapshot {state, page, vars} to disk so a board-widget-runtime
        restart resumes where we left off. Fire-and-forget — disk write
        failures get logged but don't break the live widget."""
        if not self.widget_id:
            return
        try:
            snap = {
                "widget_id": self.widget_id,
                "state": self.state,
                "page": self.page,
                "vars": self.vars,
            }
            tmp = self._state_file().with_suffix(".tmp")
            tmp.write_text(json.dumps(snap, ensure_ascii=False), encoding="utf-8")
            tmp.replace(self._state_file())
        except Exception as e:
            LOG.warning("persist state failed: %s", e)

    def _restore_state(self) -> None:
        """If the persisted snapshot exists and matches the loaded widget_id,
        overlay it onto fresh init values. Mismatched widget id (e.g. a
        leftover from a previously-active widget) is ignored."""
        try:
            sf = self._state_file()
            if not sf.exists():
                return
            snap = json.loads(sf.read_text(encoding="utf-8"))
            if snap.get("widget_id") != self.widget_id:
                return
            if snap.get("state") in (self.spec.get("states") or []):
                self.state = snap["state"]
            pages = [p.get("id") for p in (self.spec.get("pages") or [])]
            if snap.get("page") and snap["page"] in pages:
                self.page = snap["page"]
            vars_decl = self.spec.get("vars", {})
            for k, v in (snap.get("vars") or {}).items():
                if k in vars_decl:
                    self.vars[k] = v
            LOG.info("restored persisted state for %s: state=%s vars=%s",
                     self.widget_id, self.state, self.vars)
        except Exception as e:
            LOG.warning("restore state failed: %s", e)

    # ── async tasks ──

    def _start_per_widget_tasks(self) -> None:
        for tk in self.spec.get("tick", []):
            self.tasks.append(asyncio.create_task(self._tick_loop(tk)))
        for rid, rdef in self.spec.get("readers", {}).items():
            self.tasks.append(asyncio.create_task(self._reader_loop(rid, rdef)))
        # Fetchers gated by allowlist — check before scheduling
        allow = _load_fetcher_allowlist(self.root)
        for fid, fdef in self.spec.get("fetchers", {}).items():
            host = _url_host(fdef["url"])
            if host not in allow:
                LOG.warning("fetcher %s host %s not in allowlist, skipped", fid, host)
                continue
            self.tasks.append(asyncio.create_task(self._fetcher_loop(fid, fdef)))

    async def _tick_loop(self, tk: dict) -> None:
        every_s = tk["every_ms"] / 1000.0
        try:
            while True:
                await asyncio.sleep(every_s)
                if "while_state" in tk and tk["while_state"] != self.state:
                    continue
                changed = False
                for k, v in tk.get("set", {}).items():
                    if self.vars.get(k) != v:
                        self.vars[k] = v; changed = True
                for k, d in tk.get("inc", {}).items():
                    self.vars[k] = self.vars.get(k, 0) + d; changed = True
                if changed:
                    self.write_payload()
        except asyncio.CancelledError:
            raise

    async def _reader_loop(self, rid: str, rdef: dict) -> None:
        path = self.root / rdef["path"]
        pattern = re.compile(rdef["field_pattern"]) if rdef.get("field_pattern") else None
        try:
            while True:
                try:
                    text = path.read_text(encoding="utf-8")
                    value: Any = text.strip()
                    if pattern is not None:
                        m = pattern.search(text)
                        if m:
                            value = m.group(1)
                    decl = self.spec["vars"][rdef["into"]]
                    if decl["type"] == "int":
                        value = int(value)
                    if self.vars.get(rdef["into"]) != value:
                        self.vars[rdef["into"]] = value
                        self.write_payload()
                except FileNotFoundError:
                    pass
                except Exception as e:
                    LOG.warning("reader %s error: %s", rid, e)
                await asyncio.sleep(rdef["every_s"])
        except asyncio.CancelledError:
            raise

    async def _fetcher_loop(self, fid: str, fdef: dict) -> None:
        import urllib.request
        try:
            while True:
                try:
                    req = urllib.request.Request(fdef["url"], headers={"User-Agent": "board-widget-runtime/1"})
                    with urllib.request.urlopen(req, timeout=10) as resp:
                        body = resp.read().decode("utf-8")
                    value: Any = body
                    if fdef["parse"] == "json":
                        data = json.loads(body)
                        value = _resolve_jsonpath(data, fdef.get("json_path", "$"))
                    decl = self.spec["vars"][fdef["into"]]
                    if decl["type"] == "int":
                        value = int(value) if value is not None else 0
                    if self.vars.get(fdef["into"]) != value:
                        self.vars[fdef["into"]] = value
                        self.write_payload()
                except Exception as e:
                    LOG.warning("fetcher %s error: %s", fid, e)
                await asyncio.sleep(fdef["every_s"])
        except asyncio.CancelledError:
            raise

    # ── top-level orchestration ──

    async def watch_active_widget(self) -> None:
        last_id: Optional[str] = None
        while True:
            try:
                desired = self.active_widget_file.read_text(encoding="utf-8").strip()
            except FileNotFoundError:
                desired = ""
            if desired != last_id:
                LOG.info("active-widget changed: %r -> %r", last_id, desired)
                await self.unload()
                if desired:
                    try:
                        self.load(desired)
                        self.write_payload()
                        self._start_per_widget_tasks()
                    except (FileNotFoundError, ValidationError, json.JSONDecodeError) as e:
                        LOG.error("failed to activate widget %s: %s", desired, e)
                last_id = desired
            # 200ms poll keeps OTA-to-activate latency under ~200ms (was 1s).
            # File is a tiny atomic-write; stat+read overhead is negligible.
            await asyncio.sleep(0.2)

    async def watch_input_events(self) -> None:
        # poll-based tail of .widget-events: read file, track byte position,
        # parse new JSON lines. P1: file fed by SSH-injected test events.
        # P2: board_rotary_input.c / board_touch_input.c will append to this
        # file when a widget is active (instead of just toggling .screen-page).
        pos = 0
        if self.widget_events_file.exists():
            pos = self.widget_events_file.stat().st_size
        while True:
            try:
                if self.widget_events_file.exists():
                    sz = self.widget_events_file.stat().st_size
                    if sz < pos:  # file truncated / rotated
                        pos = 0
                    if sz > pos:
                        with self.widget_events_file.open("r", encoding="utf-8") as f:
                            f.seek(pos)
                            new_lines = f.read()
                            pos = f.tell()
                        for line in new_lines.splitlines():
                            line = line.strip()
                            if not line:
                                continue
                            try:
                                evt = json.loads(line)
                            except json.JSONDecodeError:
                                continue
                            control = evt.get("control", "")
                            event = evt.get("event", "")
                            if not self.spec:
                                continue
                            action = self._lookup_action(control, event)
                            if not action:
                                LOG.debug("no binding for (%s, %s)", control, event)
                                continue
                            if self.dispatch_action(action):
                                self.write_payload()
            except Exception as e:
                LOG.warning("input event watcher error: %s", e)
            await asyncio.sleep(0.2)


# ───────────────────────── Helpers ─────────────────────────

def _url_host(url: str) -> str:
    from urllib.parse import urlparse
    return urlparse(url).hostname or ""


def _load_fetcher_allowlist(root: Path) -> set:
    conf = root / "widget-runtime.conf"
    if not conf.exists():
        return set()
    hosts: set = set()
    for line in conf.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        hosts.add(line)
    return hosts


def _resolve_jsonpath(data: Any, path: str) -> Any:
    """Tiny JSONPath subset: $, $.field, $.field.length, $.field.subfield, etc.
    Length is special: takes len() of preceding array."""
    if path in ("$", "$."):
        return data
    parts = path.lstrip("$").lstrip(".").split(".")
    cur = data
    for p in parts:
        if p == "length":
            cur = len(cur) if hasattr(cur, "__len__") else 0
        elif isinstance(cur, dict):
            cur = cur.get(p)
        elif isinstance(cur, list):
            try:
                cur = cur[int(p)]
            except (ValueError, IndexError):
                return None
        else:
            return None
    return cur


# ───────────────────────── Main ─────────────────────────

async def amain(root: Path) -> int:
    runtime = WidgetRuntime(root)

    def shutdown(*_):
        for task in asyncio.all_tasks():
            task.cancel()

    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            asyncio.get_event_loop().add_signal_handler(sig, shutdown)
        except NotImplementedError:
            pass

    try:
        await asyncio.gather(
            runtime.watch_active_widget(),
            runtime.watch_input_events(),
        )
    except asyncio.CancelledError:
        pass
    finally:
        await runtime.unload()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Interpret .clawpkg widgets on-device")
    parser.add_argument("root", nargs="?", default="/opt/board-runtime",
                        help="runtime root (default: /opt/board-runtime)")
    parser.add_argument("--log-level", default=os.environ.get("BOARD_WIDGET_LOG", "INFO"))
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(message)s",
    )
    return asyncio.run(amain(Path(args.root)))


if __name__ == "__main__":
    raise SystemExit(main())
