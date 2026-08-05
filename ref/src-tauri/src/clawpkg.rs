/*
 * [Input] .clawpkg zip/directory files, dashboard safe content/visual slots, and per-component buttons.json records.
 * [Output] Shared package validation/preview plus game/tool kind and COMPONENT_DASHBOARD_V1 payload rendering, including bounded visual presets, P4 vars-shape compatibility, four-effect rules, button count, label-byte, allowed-event, and overlap guards.
 * [Pos] .clawpkg contract node in ref/src-tauri/src
 * [Sync] If this file changes, update `ref/.folder.md` and the mirrored frontend contract.
 */

use serde::Serialize;
use std::collections::{HashMap, HashSet};
use std::io::Read;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ClawpkgManifestPreview {
    pub id: String,
    pub name: String,
    pub version: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub kind: Option<String>,
    pub dashboard: HashMap<String, String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ValidateClawpkgResult {
    pub ok: bool,
    pub manifest: Option<ClawpkgManifestPreview>,
    pub errors: Vec<String>,
}

const REQUIRED_FILES: &[&str] = &[
    "component.json",
    "negative-screen.json",
    "buttons.json",
    "runtime/widget.json",
    "share.json",
];
const REQUIRED_DIRS: &[&str] = &["runtime/", "assets/"];
pub(crate) const CLAWPKG_MAX_ARCHIVE_BYTES: usize = 8 * 1024 * 1024;
pub(crate) const CLAWPKG_MAX_ENTRIES: usize = 128;
pub(crate) const CLAWPKG_MAX_ENTRY_BYTES: u64 = 4 * 1024 * 1024;
pub(crate) const CLAWPKG_MAX_EXPANDED_BYTES: u64 = 16 * 1024 * 1024;
const P4_WIDGET_JSON_MAX_BYTES: usize = 4095;
const P4_BUTTONS_JSON_MAX_BYTES: usize = 2047;
const P4_WIDGET_MAX_EFFECTS: usize = 4;
const P4_RUNTIME_ENGINE: &str = "p4-bounded-runtime-v3";
const P4_SCENE_MAX_ENTITIES: usize = 12;
const P4_SCENE_MAX_RULES: usize = 20;
const P4_SCENE_MAX_OPS: usize = 4;
const P4_SCENE_SHAPES: &[&str] = &[
    "rect",
    "player-ship",
    "enemy-ship",
    "bullet",
    "star",
    "paddle",
    "ball",
];
const P4_WIDGET_MAX_VARS: usize = 8;
const P4_WIDGET_VAR_NAME_MAX_BYTES: usize = 31;
const P4_WIDGET_STRING_VAR_MAX_BYTES: usize = 63;
const P4_WIDGET_INT_MIN: i64 = -1_000_000_000;
const P4_WIDGET_INT_MAX: i64 = 1_000_000_000;
const COMPONENT_BUTTON_MAX_BINDINGS: usize = 8;
const COMPONENT_BUTTON_LABEL_MAX_BYTES: usize = 30;
const COMPONENT_BUTTON_EVENTS: &[&str] = &[
    "screen.region.tap",
    "screen.region.long_press",
    "button.sw1.short_press",
    "button.sw2.short_press",
    "button.sw3.short_press",
    "button.encoder.short_press",
    "button.encoder.long_press",
    "knob.rotate_cw",
    "knob.rotate_ccw",
    "joystick.up",
    "joystick.down",
    /* Backward-compatible alias used by older packages. New packages should
    declare the two directions separately so each action stays editable. */
    "knob.rotate_cw / knob.rotate_ccw",
];

fn valid_component_id(id: &str) -> bool {
    let bytes = id.as_bytes();
    !bytes.is_empty()
        && bytes.len() < 48
        && bytes[0].is_ascii_lowercase()
        && bytes.iter().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || *byte == b'-' || *byte == b'_'
        })
}

/// (slot_id, max_utf8_bytes). MUST mirror ref/src/lib/clawpkg-contract.js.
pub const COMPONENT_DASHBOARD_V1_SLOTS: &[(&str, usize)] = &[
    ("title", 60),
    ("eyebrow", 90),
    ("headline", 156),
    ("metricLabel", 90),
    ("metricValue", 60),
    ("metricUnit", 30),
    ("badge", 12),
    ("note", 156),
    ("footer", 156),
    /* progress: serialized as "<0-100>:<label>" by validator (negative-screen.json
    carries it as {value, label} object that gets flattened on read). */
    ("progress", 64),
    ("visualStyle", 16),
    ("visualPalette", 16),
    ("visualLayout", 16),
    ("visualSprite", 16),
];

fn component_visual_slot_values(slot: &str) -> Option<&'static [&'static str]> {
    match slot {
        "visualStyle" => Some(&["classic", "pixel", "clean"]),
        "visualPalette" => Some(&[
            "candy", "sunset", "mint", "arcade", "ocean", "forest", "ember", "mono",
        ]),
        "visualLayout" => Some(&["arcade", "scoreboard", "tool"]),
        "visualSprite" => Some(&[
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
        ]),
        _ => None,
    }
}

fn validate_component_buttons(files: &HashMap<String, Vec<u8>>, errors: &mut Vec<String>) {
    let Some(button_bytes) = files.get("buttons.json") else {
        return;
    };
    let value: serde_json::Value = match serde_json::from_slice(button_bytes) {
        Ok(value) => value,
        Err(error) => {
            errors.push(format!("buttons.json 解析失败: {}", error));
            return;
        }
    };
    let Some(bindings) = value.as_array() else {
        errors.push("buttons.json 必须是数组".to_string());
        return;
    };
    if bindings.len() > COMPONENT_BUTTON_MAX_BINDINGS {
        errors.push(format!(
            "buttons.json 最多允许 {} 个按钮动作",
            COMPONENT_BUTTON_MAX_BINDINGS
        ));
    }
    let mut events = HashSet::new();
    let mut actions = HashSet::new();
    for (index, binding) in bindings.iter().enumerate() {
        let Some(object) = binding.as_object() else {
            errors.push(format!("buttons.json 第 {} 项必须是对象", index + 1));
            continue;
        };
        for field in ["action", "control", "event", "label"] {
            if object
                .get(field)
                .and_then(|item| item.as_str())
                .map(str::trim)
                .unwrap_or("")
                .is_empty()
            {
                errors.push(format!(
                    "buttons.json 第 {} 项缺少非空字段 {}",
                    index + 1,
                    field
                ));
            }
        }
        let event = object
            .get("event")
            .and_then(|item| item.as_str())
            .map(str::trim)
            .unwrap_or("");
        let action = object
            .get("action")
            .and_then(|item| item.as_str())
            .map(str::trim)
            .unwrap_or("");
        if !action.is_empty() && !actions.insert(action.to_string()) {
            errors.push(format!("buttons.json 动作 {} 重复，无法独立换键", action));
        }
        if !event.is_empty() {
            if !COMPONENT_BUTTON_EVENTS.contains(&event) {
                errors.push(format!(
                    "buttons.json 第 {} 项含未知事件 {}",
                    index + 1,
                    event
                ));
            }
            let event_slots = if event == "knob.rotate_cw / knob.rotate_ccw" {
                vec!["knob.rotate_cw", "knob.rotate_ccw"]
            } else {
                vec![event]
            };
            if event_slots.iter().any(|slot| events.contains(*slot)) {
                errors.push(format!("buttons.json 事件 {} 与已有绑定冲突", event));
            }
            events.extend(event_slots.into_iter().map(str::to_string));
        }
        let label = object
            .get("label")
            .and_then(|item| item.as_str())
            .unwrap_or("");
        if label.len() > COMPONENT_BUTTON_LABEL_MAX_BYTES {
            errors.push(format!(
                "buttons.json 第 {} 项标签超出 {} 字节上限",
                index + 1,
                COMPONENT_BUTTON_LABEL_MAX_BYTES
            ));
        }
    }
}

fn validate_p4_compact_json_sizes(files: &HashMap<String, Vec<u8>>, errors: &mut Vec<String>) {
    for (path, max_bytes) in [
        ("runtime/widget.json", P4_WIDGET_JSON_MAX_BYTES),
        ("buttons.json", P4_BUTTONS_JSON_MAX_BYTES),
    ] {
        let Some(bytes) = files.get(path) else {
            continue;
        };
        let Ok(value) = serde_json::from_slice::<serde_json::Value>(bytes) else {
            continue;
        };
        let Ok(compact) = serde_json::to_vec(&value) else {
            continue;
        };
        if compact.len() > max_bytes {
            errors.push(format!(
                "{} 压缩后为 {} 字节，超过 P4 {} 字节上限",
                path,
                compact.len(),
                max_bytes
            ));
        }
    }
}

fn valid_p4_var_name(name: &str) -> bool {
    let bytes = name.as_bytes();
    !bytes.is_empty()
        && bytes.len() <= P4_WIDGET_VAR_NAME_MAX_BYTES
        && bytes.iter().all(|byte| {
            byte.is_ascii_alphanumeric() || *byte == b'_' || *byte == b'-' || *byte == b'.'
        })
}

pub(crate) fn validate_p4_vars_object(
    vars: &serde_json::Map<String, serde_json::Value>,
) -> Result<(), String> {
    if vars.len() > P4_WIDGET_MAX_VARS {
        return Err(format!(
            "runtime/widget.json.vars 最多允许 {} 个变量",
            P4_WIDGET_MAX_VARS
        ));
    }
    for (name, declaration) in vars {
        let label = format!("runtime/widget.json.vars.{}", name);
        if !valid_p4_var_name(name) {
            return Err(format!("{} 的变量名无效", label));
        }
        let declaration = declaration
            .as_object()
            .ok_or_else(|| format!("{} 必须是对象", label))?;
        if let Some(field) = declaration
            .keys()
            .find(|field| !matches!(field.as_str(), "type" | "init"))
        {
            return Err(format!(
                "{} 含固件不支持的字段 {}；只允许 type 和 init",
                label, field
            ));
        }
        let var_type = declaration
            .get("type")
            .and_then(serde_json::Value::as_str)
            .unwrap_or("");
        if !matches!(var_type, "int" | "string") {
            return Err(format!("{}.type 只能是 int 或 string", label));
        }
        let Some(init) = declaration.get("init") else {
            continue;
        };
        if var_type == "int" {
            let Some(value) = init.as_i64() else {
                return Err(format!(
                    "{}.init 必须是 {}..{} 的整数",
                    label, P4_WIDGET_INT_MIN, P4_WIDGET_INT_MAX
                ));
            };
            if !(P4_WIDGET_INT_MIN..=P4_WIDGET_INT_MAX).contains(&value) {
                return Err(format!(
                    "{}.init 必须是 {}..{} 的整数",
                    label, P4_WIDGET_INT_MIN, P4_WIDGET_INT_MAX
                ));
            }
        } else if !init
            .as_str()
            .is_some_and(|value| value.len() <= P4_WIDGET_STRING_VAR_MAX_BYTES)
        {
            return Err(format!(
                "{}.init 必须是最多 {} 个 UTF-8 字节的字符串",
                label, P4_WIDGET_STRING_VAR_MAX_BYTES
            ));
        }
    }
    Ok(())
}

fn validate_p4_widget_vars_shape(files: &HashMap<String, Vec<u8>>, errors: &mut Vec<String>) {
    let Some(widget) = files
        .get("runtime/widget.json")
        .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(bytes).ok())
    else {
        return;
    };
    let Some(object) = widget.as_object() else {
        errors.push("runtime/widget.json 必须是 JSON 对象".to_string());
        return;
    };
    let Some(vars) = object.get("vars") else {
        // The P4 transfer compiler losslessly upgrades legacy omissions to {}.
        return;
    };
    if let Some(vars) = vars.as_object() {
        if let Err(error) = validate_p4_vars_object(vars) {
            errors.push(error);
        }
        return;
    }
    if vars.as_array().is_some_and(Vec::is_empty) {
        return;
    }
    errors.push(
        "runtime/widget.json 的 vars 必须是以变量名为键的 JSON 对象；无变量时请使用 {}".to_string(),
    );
}

fn p4_widget_effect_count(rule: &serde_json::Value) -> usize {
    ["set", "inc"]
        .into_iter()
        .map(|key| {
            rule.get(key)
                .and_then(|value| value.as_object())
                .map(|object| object.len())
                .unwrap_or(0)
        })
        .sum()
}

fn validate_p4_widget_effect_bounds(files: &HashMap<String, Vec<u8>>, errors: &mut Vec<String>) {
    let Some(widget) = files
        .get("runtime/widget.json")
        .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(bytes).ok())
    else {
        return;
    };
    for (field, nested) in [("transitions", false), ("tick", false), ("tick.then", true)] {
        let source_field = if nested { "tick" } else { field };
        let Some(rules) = widget.get(source_field).and_then(|value| value.as_array()) else {
            continue;
        };
        for (index, rule) in rules.iter().enumerate() {
            let target = if nested { rule.get("then") } else { Some(rule) };
            let count = target.map(p4_widget_effect_count).unwrap_or(0);
            if count > P4_WIDGET_MAX_EFFECTS {
                errors.push(format!(
                    "runtime/widget.json {}[{}] 的 set+inc 共 {} 项，超过 P4 {} 项上限",
                    field, index, count, P4_WIDGET_MAX_EFFECTS
                ));
            }
        }
    }
}

fn json_int_between(value: Option<&serde_json::Value>, minimum: i64, maximum: i64) -> bool {
    value
        .and_then(serde_json::Value::as_i64)
        .is_some_and(|number| (minimum..=maximum).contains(&number))
}

fn object_has_only_keys(
    object: &serde_json::Map<String, serde_json::Value>,
    allowed: &[&str],
) -> bool {
    object.keys().all(|key| allowed.contains(&key.as_str()))
}

fn valid_scene_id(value: &str, max_len: usize) -> bool {
    !value.is_empty()
        && value.len() <= max_len
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}

fn valid_scene_coordinate(value: Option<&serde_json::Value>) -> bool {
    let Some(value) = value else {
        return false;
    };
    if value
        .as_i64()
        .is_some_and(|number| (0..=15).contains(&number))
    {
        return true;
    }
    let Some(range) = value.as_array() else {
        return false;
    };
    if range.len() != 2 {
        return false;
    }
    let Some(minimum) = range[0].as_i64() else {
        return false;
    };
    let Some(maximum) = range[1].as_i64() else {
        return false;
    };
    (0..=15).contains(&minimum) && (minimum..=15).contains(&maximum)
}

fn scene_entity_known(
    operation: &serde_json::Map<String, serde_json::Value>,
    field: &str,
    entity_ids: &HashSet<String>,
) -> bool {
    operation
        .get(field)
        .and_then(serde_json::Value::as_str)
        .is_some_and(|id| entity_ids.contains(id))
}

fn validate_scene_op(
    value: &serde_json::Value,
    entity_ids: &HashSet<String>,
    label: &str,
    errors: &mut Vec<String>,
) {
    let Some(operation) = value.as_object() else {
        errors.push(format!("{} 必须是含 op 的对象", label));
        return;
    };
    let name = operation
        .get("op")
        .and_then(serde_json::Value::as_str)
        .unwrap_or("");
    let entity_known = scene_entity_known(operation, "entity", entity_ids);
    match name {
        "move" => {
            if !object_has_only_keys(operation, &["op", "entity", "dx", "dy"])
                || !entity_known
                || !json_int_between(operation.get("dx"), -4, 4)
                || !json_int_between(operation.get("dy"), -4, 4)
                || (operation.get("dx").and_then(|v| v.as_i64()) == Some(0)
                    && operation.get("dy").and_then(|v| v.as_i64()) == Some(0))
            {
                errors.push(format!("{} move 需要实体及 -4..4 的非零 dx/dy", label));
            }
        }
        "velocity" | "accelerate" => {
            if !object_has_only_keys(operation, &["op", "entity", "vx", "vy"])
                || !entity_known
                || !json_int_between(operation.get("vx"), -4, 4)
                || !json_int_between(operation.get("vy"), -4, 4)
            {
                errors.push(format!("{} {} 需要实体及 -4..4 的 vx/vy", label, name));
            }
        }
        "place" => {
            let source_valid = operation.get("source").is_none()
                || scene_entity_known(operation, "source", entity_ids);
            let coordinates_valid = ["x", "y"].into_iter().all(|field| {
                operation.get(field).is_none() || valid_scene_coordinate(operation.get(field))
            });
            let offsets_valid = ["dx", "dy"].into_iter().all(|field| {
                operation.get(field).is_none() || json_int_between(operation.get(field), -4, 4)
            });
            let has_position = ["source", "x", "y"]
                .into_iter()
                .any(|field| operation.contains_key(field));
            if !object_has_only_keys(operation, &["op", "entity", "source", "x", "y", "dx", "dy"])
                || !entity_known
                || !source_valid
                || !coordinates_valid
                || !offsets_valid
                || !has_position
            {
                errors.push(format!("{} place 需要合法实体、坐标范围或 source", label));
            }
        }
        "show" | "hide" => {
            if !object_has_only_keys(operation, &["op", "entity"]) || !entity_known {
                errors.push(format!("{} {} 需要一个已声明实体", label, name));
            }
        }
        "score" => {
            let has_add = operation.contains_key("add");
            let has_set = operation.contains_key("set");
            let score = if has_add {
                operation.get("add")
            } else {
                operation.get("set")
            };
            if !object_has_only_keys(operation, &["op", "add", "set"])
                || has_add == has_set
                || !json_int_between(score, -10000, 10000)
            {
                errors.push(format!(
                    "{} score 必须且只能含一个 -10000..10000 的 add/set",
                    label
                ));
            }
        }
        "run" | "stop" | "restart" => {
            if !object_has_only_keys(operation, &["op"]) {
                errors.push(format!("{} {} 不接受其他字段", label, name));
            }
        }
        "bounce" => {
            let axis = operation
                .get("axis")
                .and_then(serde_json::Value::as_str)
                .unwrap_or("");
            if !object_has_only_keys(operation, &["op", "entity", "axis"])
                || !entity_known
                || !matches!(axis, "x" | "y" | "both")
            {
                errors.push(format!("{} bounce 需要实体及 x/y/both 轴", label));
            }
        }
        "tone" => {
            if !object_has_only_keys(operation, &["op", "entity", "tone"])
                || !entity_known
                || !json_int_between(operation.get("tone"), 1, 4)
            {
                errors.push(format!("{} tone 需要实体及 1..4 色阶", label));
            }
        }
        _ => errors.push(format!("{}.op 不支持 {}", label, name)),
    }
}

fn validate_component_scene(
    widget: &serde_json::Value,
    files: &HashMap<String, Vec<u8>>,
    errors: &mut Vec<String>,
) {
    let Some(scene_value) = widget.get("scene") else {
        return;
    };
    if widget.get("engine").and_then(|value| value.as_str()) != Some(P4_RUNTIME_ENGINE) {
        errors.push(format!(
            "runtime/widget.json scene 需要 engine={}",
            P4_RUNTIME_ENGINE
        ));
    }
    let Some(scene) = scene_value.as_object() else {
        errors.push("runtime/widget.json scene 必须是对象".to_string());
        return;
    };
    if !object_has_only_keys(
        scene,
        &[
            "tick_ms",
            "active_state",
            "result_state",
            "score_var",
            "auto_start",
            "grid",
            "entities",
            "rules",
        ],
    ) {
        errors.push("runtime/widget.json scene 含未知字段".to_string());
    }
    if !json_int_between(scene.get("tick_ms"), 100, 2000) {
        errors.push("runtime/widget.json scene.tick_ms 必须在 100-2000".to_string());
    }
    let state_ids: HashSet<&str> = widget
        .get("states")
        .and_then(|value| value.as_array())
        .into_iter()
        .flatten()
        .filter_map(|value| value.as_str())
        .collect();
    let active_state = scene
        .get("active_state")
        .and_then(|value| value.as_str())
        .unwrap_or("");
    if !state_ids.contains(active_state) {
        errors.push("runtime/widget.json scene.active_state 必须引用 states".to_string());
    }
    if let Some(result_state) = scene.get("result_state") {
        if !result_state
            .as_str()
            .is_some_and(|state| state_ids.contains(state))
        {
            errors.push("runtime/widget.json scene.result_state 必须引用 states".to_string());
        }
    }
    if let Some(score_var) = scene.get("score_var") {
        let valid = score_var.as_str().is_some_and(|name| {
            widget
                .get("vars")
                .and_then(|vars| vars.get(name))
                .and_then(|spec| spec.get("type"))
                .and_then(|kind| kind.as_str())
                == Some("int")
        });
        if !valid {
            errors.push("runtime/widget.json scene.score_var 必须引用 int 变量".to_string());
        }
    }
    if let Some(auto_start) = scene.get("auto_start") {
        if !auto_start.is_boolean() {
            errors.push("runtime/widget.json scene.auto_start 必须是布尔值".to_string());
        } else if auto_start == &serde_json::Value::Bool(true)
            && widget.get("initial_state").and_then(|value| value.as_str()) != Some(active_state)
        {
            errors.push(
                "runtime/widget.json scene.auto_start 要求 initial_state 等于 active_state"
                    .to_string(),
            );
        }
    }

    let grid = scene.get("grid").and_then(|value| value.as_object());
    let mut grid_width = 0_i64;
    let mut grid_height = 0_i64;
    if let Some(grid) = grid {
        grid_width = grid
            .get("width")
            .and_then(|value| value.as_i64())
            .unwrap_or(0);
        grid_height = grid
            .get("height")
            .and_then(|value| value.as_i64())
            .unwrap_or(0);
        if !object_has_only_keys(grid, &["width", "height", "rows", "solid_tones"])
            || !(4..=16).contains(&grid_width)
            || !(4..=16).contains(&grid_height)
        {
            errors.push("runtime/widget.json scene.grid 需要 4..16 的 width/height".to_string());
        }
        if let Some(rows) = grid.get("rows") {
            let valid = rows.as_array().is_some_and(|rows| {
                rows.len() == grid_height as usize
                    && rows.iter().all(|row| {
                        row.as_str().is_some_and(|row| {
                            row.len() == grid_width as usize
                                && row.bytes().all(|tone| (b'0'..=b'4').contains(&tone))
                        })
                    })
            });
            if !valid {
                errors.push(
                    "runtime/widget.json scene.grid.rows 必须按高度/宽度使用 0..4 色阶".to_string(),
                );
            }
        }
        if let Some(solid_tones) = grid.get("solid_tones") {
            let mut seen = HashSet::new();
            let valid = solid_tones.as_array().is_some_and(|tones| {
                tones.len() <= 4
                    && tones.iter().all(|tone| {
                        tone.as_i64()
                            .is_some_and(|tone| (1..=4).contains(&tone) && seen.insert(tone))
                    })
            });
            if !valid {
                errors.push(
                    "runtime/widget.json scene.grid.solid_tones 必须是唯一的 1..4 色阶".to_string(),
                );
            }
        }
    } else {
        errors.push("runtime/widget.json scene.grid 需要 4..16 的 width/height".to_string());
    }

    let entities = scene.get("entities").and_then(|value| value.as_array());
    let mut entity_ids = HashSet::new();
    if !entities.is_some_and(|items| (1..=P4_SCENE_MAX_ENTITIES).contains(&items.len())) {
        errors.push(format!(
            "runtime/widget.json scene.entities 必须为 1-{} 项",
            P4_SCENE_MAX_ENTITIES
        ));
    }
    for (index, entity) in entities.into_iter().flatten().enumerate() {
        let label = format!("runtime/widget.json scene.entities[{}]", index);
        let Some(entity) = entity.as_object() else {
            errors.push(format!("{} 必须是对象", label));
            continue;
        };
        if !object_has_only_keys(
            entity,
            &[
                "id",
                "x",
                "y",
                "width",
                "height",
                "tone",
                "vx",
                "vy",
                "bounds",
                "shape",
                "active",
                "collidable",
            ],
        ) {
            errors.push(format!("{} 含未知字段", label));
        }
        let id = entity
            .get("id")
            .and_then(|value| value.as_str())
            .unwrap_or("");
        if !valid_scene_id(id, 15) || !entity_ids.insert(id.to_string()) {
            errors.push(format!("{}.id 无效或重复", label));
        }
        let x = entity
            .get("x")
            .and_then(|value| value.as_i64())
            .unwrap_or(-1);
        let y = entity
            .get("y")
            .and_then(|value| value.as_i64())
            .unwrap_or(-1);
        let width = entity
            .get("width")
            .and_then(|value| value.as_i64())
            .unwrap_or(1);
        let height = entity
            .get("height")
            .and_then(|value| value.as_i64())
            .unwrap_or(1);
        if !(0..=15).contains(&x)
            || !(0..=15).contains(&y)
            || !(1..=8).contains(&width)
            || !(1..=8).contains(&height)
            || (grid_width > 0 && x + width > grid_width)
            || (grid_height > 0 && y + height > grid_height)
            || (entity.contains_key("tone") && !json_int_between(entity.get("tone"), 1, 4))
            || (entity.contains_key("vx") && !json_int_between(entity.get("vx"), -4, 4))
            || (entity.contains_key("vy") && !json_int_between(entity.get("vy"), -4, 4))
        {
            errors.push(format!("{} 的位置、尺寸、色阶或速度越界", label));
        }
        if entity.get("bounds").is_some_and(|bounds| {
            !matches!(
                bounds.as_str().unwrap_or(""),
                "clamp" | "wrap" | "bounce" | "hide" | "stop"
            )
        }) {
            errors.push(format!("{}.bounds 不受支持", label));
        }
        if entity.get("shape").is_some_and(|shape| {
            !shape
                .as_str()
                .is_some_and(|shape| P4_SCENE_SHAPES.contains(&shape))
        }) {
            errors.push(format!("{}.shape 不受支持", label));
        }
        if ["active", "collidable"]
            .into_iter()
            .any(|field| entity.get(field).is_some_and(|value| !value.is_boolean()))
        {
            errors.push(format!("{} active/collidable 必须是布尔值", label));
        }
    }

    let transition_actions: HashSet<&str> = widget
        .get("transitions")
        .and_then(|value| value.as_array())
        .into_iter()
        .flatten()
        .filter_map(|value| value.get("on").and_then(|action| action.as_str()))
        .collect();
    let buttons_value = files
        .get("buttons.json")
        .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(bytes).ok())
        .unwrap_or(serde_json::Value::Null);
    let button_actions: HashSet<&str> = buttons_value
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|value| value.get("action").and_then(|action| action.as_str()))
        .collect();
    let rules = scene.get("rules").and_then(|value| value.as_array());
    if !rules.is_some_and(|items| (1..=P4_SCENE_MAX_RULES).contains(&items.len())) {
        errors.push(format!(
            "runtime/widget.json scene.rules 必须为 1-{} 项",
            P4_SCENE_MAX_RULES
        ));
    }
    for (index, rule) in rules.into_iter().flatten().enumerate() {
        let label = format!("runtime/widget.json scene.rules[{}]", index);
        let Some(rule) = rule.as_object() else {
            errors.push(format!("{} 必须是对象", label));
            continue;
        };
        if !object_has_only_keys(rule, &["on", "entity", "with", "edge", "do"]) {
            errors.push(format!("{} 含未知字段", label));
        }
        let trigger = rule
            .get("on")
            .and_then(|value| value.as_str())
            .unwrap_or("");
        if trigger == "collision" {
            let entity = rule
                .get("entity")
                .and_then(|value| value.as_str())
                .unwrap_or("");
            let with = rule
                .get("with")
                .and_then(|value| value.as_str())
                .unwrap_or("");
            if entity == with || !entity_ids.contains(entity) || !entity_ids.contains(with) {
                errors.push(format!("{} collision 需要两个不同的已声明实体", label));
            }
        } else if matches!(trigger, "edge" | "blocked") {
            let entity = rule
                .get("entity")
                .and_then(|value| value.as_str())
                .unwrap_or("");
            if !entity_ids.contains(entity) {
                errors.push(format!("{} 需要已声明实体", label));
            }
            let edge = rule.get("edge").and_then(|value| value.as_str());
            if trigger == "blocked" && edge.is_some() {
                errors.push(format!("{} blocked 不接受 edge", label));
            } else if edge
                .is_some_and(|edge| !matches!(edge, "any" | "left" | "right" | "top" | "bottom"))
            {
                errors.push(format!("{}.edge 不受支持", label));
            }
        } else if trigger != "tick"
            && (!valid_scene_id(trigger, 47)
                || !transition_actions.contains(trigger)
                || !button_actions.contains(trigger))
        {
            errors.push(format!(
                "{}.on 必须同时匹配 transition 与 button 动作",
                label
            ));
        }
        let operations = rule.get("do").and_then(|value| value.as_array());
        if !operations.is_some_and(|items| (1..=P4_SCENE_MAX_OPS).contains(&items.len())) {
            errors.push(format!("{}.do 必须为 1-{} 项", label, P4_SCENE_MAX_OPS));
        }
        for (op_index, operation) in operations.into_iter().flatten().enumerate() {
            validate_scene_op(
                operation,
                &entity_ids,
                &format!("{}.do[{}]", label, op_index),
                errors,
            );
        }
    }
}

fn validate_component_game(files: &HashMap<String, Vec<u8>>, errors: &mut Vec<String>) {
    let Some(widget_bytes) = files.get("runtime/widget.json") else {
        return;
    };
    let widget: serde_json::Value = match serde_json::from_slice(widget_bytes) {
        Ok(value) => value,
        Err(error) => {
            errors.push(format!("runtime/widget.json 解析失败: {}", error));
            return;
        }
    };
    if widget.get("engine").is_some()
        && widget.get("engine").and_then(|value| value.as_str()) != Some(P4_RUNTIME_ENGINE)
    {
        errors.push(format!(
            "runtime/widget.json engine 只支持 {}",
            P4_RUNTIME_ENGINE
        ));
    }
    if widget.get("scene").is_some() && widget.get("game").is_some() {
        errors.push("runtime/widget.json scene 与旧版 game 不能同时存在".to_string());
    }
    validate_component_scene(&widget, files, errors);
    let Some(game) = widget.get("game") else {
        return;
    };
    let Some(game) = game.as_object() else {
        errors.push("runtime/widget.json game 必须是对象".to_string());
        return;
    };
    let game_type = game
        .get("type")
        .and_then(|value| value.as_str())
        .unwrap_or("");
    if !matches!(game_type, "blocks" | "snake" | "flappy") {
        errors.push(format!(
            "runtime/widget.json game.type 不支持 {}",
            game_type
        ));
        return;
    }
    let allowed_game_keys: HashSet<&str> = [
        "type",
        "tick_ms",
        "playing_state",
        "result_state",
        "score_var",
        "actions",
    ]
    .into_iter()
    .collect();
    if game
        .keys()
        .any(|key| !allowed_game_keys.contains(key.as_str()))
    {
        errors.push("runtime/widget.json game 含未知字段".to_string());
    }
    let tick_ms = game
        .get("tick_ms")
        .and_then(|value| value.as_u64())
        .unwrap_or(0);
    if !(100..=2000).contains(&tick_ms) {
        errors.push("runtime/widget.json game.tick_ms 必须在 100-2000".to_string());
    }
    let state_ids: HashSet<&str> = widget
        .get("states")
        .and_then(|value| value.as_array())
        .into_iter()
        .flatten()
        .filter_map(|value| value.as_str())
        .collect();
    let playing_state = game
        .get("playing_state")
        .and_then(|value| value.as_str())
        .unwrap_or("");
    let result_state = game
        .get("result_state")
        .and_then(|value| value.as_str())
        .unwrap_or("");
    if !state_ids.contains(playing_state) || !state_ids.contains(result_state) {
        errors.push("runtime/widget.json game 引用了未知 playing/result 状态".to_string());
    }
    let score_var = game
        .get("score_var")
        .and_then(|value| value.as_str())
        .unwrap_or("");
    let score_var_type = widget
        .get("vars")
        .and_then(|value| value.get(score_var))
        .and_then(|value| value.get("type"))
        .and_then(|value| value.as_str());
    if score_var_type != Some("int") {
        errors.push("runtime/widget.json game.score_var 必须引用 int 变量".to_string());
    }
    let transition_actions: HashSet<&str> = widget
        .get("transitions")
        .and_then(|value| value.as_array())
        .into_iter()
        .flatten()
        .filter_map(|value| value.get("on").and_then(|action| action.as_str()))
        .collect();
    let buttons_value = files
        .get("buttons.json")
        .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(bytes).ok())
        .unwrap_or(serde_json::Value::Null);
    let button_actions: HashSet<&str> = buttons_value
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|value| value.get("action").and_then(|action| action.as_str()))
        .collect();
    let required_actions: &[&str] = if game_type == "blocks" {
        &["start", "left", "right", "rotate", "drop"]
    } else if game_type == "snake" {
        &["start", "left", "right"]
    } else {
        &["flap"]
    };
    let action_keys: HashSet<&str> = game
        .get("actions")
        .and_then(|value| value.as_object())
        .into_iter()
        .flat_map(|object| object.keys().map(String::as_str))
        .collect();
    if action_keys.len() != required_actions.len()
        || action_keys
            .iter()
            .any(|key| !required_actions.contains(key))
    {
        errors.push(format!(
            "runtime/widget.json {} game.actions 字段不完整或含未知动作",
            game_type
        ));
    }
    for key in required_actions {
        let action = game
            .get("actions")
            .and_then(|value| value.get(*key))
            .and_then(|value| value.as_str())
            .unwrap_or("");
        if action.is_empty()
            || !transition_actions.contains(action)
            || !button_actions.contains(action)
        {
            errors.push(format!(
                "runtime/widget.json game.actions.{} 必须同时匹配 transition 与 button",
                key
            ));
        }
    }
}

pub fn validate_clawpkg_bytes(bytes: &[u8]) -> Result<ValidateClawpkgResult, String> {
    if bytes.len() > CLAWPKG_MAX_ARCHIVE_BYTES {
        return Err(format!(
            "clawpkg 压缩包超过 {} 字节上限",
            CLAWPKG_MAX_ARCHIVE_BYTES
        ));
    }
    let mut files: HashMap<String, Vec<u8>> = HashMap::new();
    let mut dir_seen: HashMap<String, bool> = HashMap::new();
    let mut expanded_bytes = 0u64;

    let mut archive = zip::ZipArchive::new(std::io::Cursor::new(bytes))
        .map_err(|e| format!("not a valid zip: {}", e))?;
    if archive.len() > CLAWPKG_MAX_ENTRIES {
        return Err(format!(
            "clawpkg 文件数超过 {} 个上限",
            CLAWPKG_MAX_ENTRIES
        ));
    }

    for i in 0..archive.len() {
        let entry = archive.by_index(i).map_err(|e| e.to_string())?;
        let enclosed = entry
            .enclosed_name()
            .ok_or_else(|| format!("clawpkg 含不安全路径: {}", entry.name()))?;
        let name = enclosed
            .to_str()
            .ok_or_else(|| "clawpkg 路径必须是 UTF-8".to_string())?
            .replace('\\', "/");
        if entry
            .unix_mode()
            .is_some_and(|mode| mode & 0o170000 == 0o120000)
        {
            return Err(format!("clawpkg 不允许符号链接: {}", name));
        }
        if entry.size() > CLAWPKG_MAX_ENTRY_BYTES {
            return Err(format!("clawpkg 文件 {} 超过单文件大小上限", name));
        }
        expanded_bytes = expanded_bytes
            .checked_add(entry.size())
            .ok_or_else(|| "clawpkg 解压大小溢出".to_string())?;
        if expanded_bytes > CLAWPKG_MAX_EXPANDED_BYTES {
            return Err("clawpkg 解压后总大小超过上限".to_string());
        }
        for d in REQUIRED_DIRS {
            if name.starts_with(d) {
                dir_seen.insert((*d).to_string(), true);
            }
        }
        if entry.is_file() {
            let mut buf = Vec::new();
            entry
                .take(CLAWPKG_MAX_ENTRY_BYTES + 1)
                .read_to_end(&mut buf)
                .map_err(|e| e.to_string())?;
            if buf.len() as u64 > CLAWPKG_MAX_ENTRY_BYTES {
                return Err(format!("clawpkg 文件 {} 超过单文件大小上限", name));
            }
            if files.contains_key(&name) {
                return Err(format!("clawpkg 含重复路径: {}", name));
            }
            files.insert(name, buf);
        }
    }

    validate_clawpkg_collected(files, |d| dir_seen.get(d).copied().unwrap_or(false))
}

/// Build the manifest preview from the collected file map. Mutates `errors` with
/// any structural problems (missing fields, unknown slots, byte overflow). Returns
/// `Some(preview)` only when nothing was pushed to `errors` during this call.
fn build_manifest_preview(
    files: &HashMap<String, Vec<u8>>,
    errors: &mut Vec<String>,
) -> Option<ClawpkgManifestPreview> {
    let meta_bytes = files.get("component.json")?;
    let v: serde_json::Value = match serde_json::from_slice(meta_bytes) {
        Ok(v) => v,
        Err(e) => {
            errors.push(format!("component.json 解析失败: {}", e));
            return None;
        }
    };
    let id = v.get("id").and_then(|x| x.as_str()).unwrap_or("");
    let name = v.get("name").and_then(|x| x.as_str()).unwrap_or("");
    let version = v.get("version").and_then(|x| x.as_str()).unwrap_or("");
    let kind = v.get("kind").and_then(|x| x.as_str());
    let had_errors_before = errors.len();
    if id.is_empty() || name.is_empty() || version.is_empty() {
        errors.push("component.json 必须含 id、name、version".to_string());
    }
    if !id.is_empty() && !valid_component_id(id) {
        errors.push(
            "component.json id 必须为 1-47 位小写 ASCII 标识（字母开头，仅 a-z、0-9、-、_）"
                .to_string(),
        );
    }
    if v.get("kind").is_some() && !matches!(kind, Some("game" | "tool")) {
        errors.push("component.json kind 只支持 game/tool".to_string());
    }
    validate_component_buttons(files, errors);
    validate_p4_compact_json_sizes(files, errors);
    validate_p4_widget_vars_shape(files, errors);
    validate_p4_widget_effect_bounds(files, errors);
    validate_component_game(files, errors);
    /* Read dashboard. Most slots are flat strings; `progress` may be an object
    {value, label} which we flatten to "<value>:<label>" so it serializes as
    a normal slot for the device parser. */
    let mut dashboard_map: HashMap<String, String> = HashMap::new();
    if let Some(dash_val) = files
        .get("negative-screen.json")
        .and_then(|d| serde_json::from_slice::<serde_json::Value>(d).ok())
        .and_then(|v| v.get("dashboard").cloned())
    {
        if let Some(obj) = dash_val.as_object() {
            for (k, v) in obj {
                if k == "progress" && v.is_object() {
                    let value = v
                        .get("value")
                        .and_then(|x| x.as_i64().or_else(|| x.as_f64().map(|f| f as i64)))
                        .unwrap_or(0);
                    let label = v.get("label").and_then(|x| x.as_str()).unwrap_or("");
                    dashboard_map.insert("progress".to_string(), format!("{}:{}", value, label));
                } else if let Some(s) = v.as_str() {
                    dashboard_map.insert(k.clone(), s.to_string());
                }
            }
        }
    }
    for (slot, value) in &dashboard_map {
        let known = COMPONENT_DASHBOARD_V1_SLOTS.iter().find(|(k, _)| k == slot);
        match known {
            None => errors.push(format!("negative-screen.json 含未知槽位 {}", slot)),
            Some((_, max_bytes)) => {
                if value.len() > *max_bytes {
                    errors.push(format!("槽位 {} 超出 {} 字节上限", slot, max_bytes));
                } else if component_visual_slot_values(slot)
                    .is_some_and(|allowed| !allowed.contains(&value.as_str()))
                {
                    errors.push(format!("槽位 {} 含未知视觉预置 {}", slot, value));
                }
            }
        }
    }
    if errors.len() == had_errors_before {
        Some(ClawpkgManifestPreview {
            id: id.to_string(),
            name: name.to_string(),
            version: version.to_string(),
            kind: kind.map(str::to_string),
            dashboard: dashboard_map,
        })
    } else {
        None
    }
}

pub fn validate_clawpkg_at_path(path: &std::path::Path) -> Result<ValidateClawpkgResult, String> {
    /* Skill-generated drafts land as directories (the agent's working copy);
    distributed clawpkgs land as .zip / .clawpkg files. Dispatch on metadata
    so both shapes feed the same validator. */
    let meta = std::fs::metadata(path).map_err(|e| format!("stat {}: {}", path.display(), e))?;
    if meta.is_dir() {
        let mut files: HashMap<String, Vec<u8>> = HashMap::new();
        for f in REQUIRED_FILES {
            let fp = path.join(f);
            if fp.exists() {
                let bytes =
                    std::fs::read(&fp).map_err(|e| format!("read {}: {}", fp.display(), e))?;
                files.insert((*f).to_string(), bytes);
            }
        }
        validate_clawpkg_collected(files, |dir| path.join(dir).is_dir())
    } else {
        if meta.len() > CLAWPKG_MAX_ARCHIVE_BYTES as u64 {
            return Err(format!(
                "clawpkg 压缩包超过 {} 字节上限",
                CLAWPKG_MAX_ARCHIVE_BYTES
            ));
        }
        let bytes = std::fs::read(path).map_err(|e| format!("read {}: {}", path.display(), e))?;
        validate_clawpkg_bytes(&bytes)
    }
}

/// Shared core: validate a manifest after files are collected (from zip or fs walk).
/// `dir_exists` reports whether REQUIRED_DIRS are present in the source.
fn validate_clawpkg_collected(
    files: HashMap<String, Vec<u8>>,
    dir_exists: impl Fn(&str) -> bool,
) -> Result<ValidateClawpkgResult, String> {
    let mut errors: Vec<String> = Vec::new();
    for f in REQUIRED_FILES {
        if !files.contains_key(*f) {
            errors.push(format!("缺少 {}", f));
        }
    }
    for d in REQUIRED_DIRS {
        if !dir_exists(d) {
            errors.push(format!("缺少 {}", d));
        }
    }
    let preview = build_manifest_preview(&files, &mut errors);
    Ok(ValidateClawpkgResult {
        ok: errors.is_empty(),
        manifest: preview,
        errors,
    })
}

/// Render slot map to COMPONENT_DASHBOARD_V1 text payload understood by device.
/// Format: first line = magic, subsequent lines = "key=value" for non-empty values
/// in canonical slot order. Empty slots are omitted.
pub fn render_component_dashboard_payload(dashboard: &HashMap<String, String>) -> String {
    let mut out = String::from("COMPONENT_DASHBOARD_V1\n");
    for (slot, _max_bytes) in COMPONENT_DASHBOARD_V1_SLOTS {
        if let Some(value) = dashboard.get(*slot) {
            if !value.is_empty() {
                out.push_str(slot);
                out.push('=');
                out.push_str(value);
                out.push('\n');
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn make_zip(files: &[(&str, &[u8])]) -> Vec<u8> {
        let mut buf = Vec::new();
        {
            let mut zw = zip::ZipWriter::new(std::io::Cursor::new(&mut buf));
            for (name, data) in files {
                zw.start_file(*name, zip::write::FileOptions::default())
                    .unwrap();
                zw.write_all(data).unwrap();
            }
            zw.finish().unwrap();
        }
        buf
    }

    fn valid_files() -> Vec<(&'static str, Vec<u8>)> {
        vec![
            (
                "component.json",
                br#"{"id":"x","name":"X","version":"1.0.0"}"#.to_vec(),
            ),
            (
                "negative-screen.json",
                b"{\"dashboard\":{\"title\":\"X\",\"headline\":\"\xe4\xbd\xa0\xe5\xa5\xbd\"}}"
                    .to_vec(),
            ),
            ("buttons.json", b"[]".to_vec()),
            ("runtime/widget.json", br#"{"schema_version":1}"#.to_vec()),
            ("assets/.keep", b"".to_vec()),
            ("share.json", br#"{"title":"X"}"#.to_vec()),
        ]
    }

    #[test]
    fn validates_complete_clawpkg() {
        let files_owned = valid_files();
        let files: Vec<(&str, &[u8])> = files_owned
            .iter()
            .map(|(n, d)| (*n, d.as_slice()))
            .collect();
        let zip_bytes = make_zip(&files);
        let result = validate_clawpkg_bytes(&zip_bytes).expect("validate should run");
        assert!(result.ok, "expected valid; errors={:?}", result.errors);
        assert_eq!(result.manifest.as_ref().unwrap().id, "x");
        assert_eq!(result.manifest.as_ref().unwrap().kind, None);
    }

    #[test]
    fn rejects_parent_traversal_paths() {
        let zip_bytes = make_zip(&[("../escape.txt", b"escape")]);
        let error = validate_clawpkg_bytes(&zip_bytes).unwrap_err();
        assert!(error.contains("不安全路径"), "unexpected error: {error}");
    }

    #[test]
    fn rejects_duplicate_paths() {
        let zip_bytes = make_zip(&[("component.json", b"{}"), ("component.json", b"{}")]);
        let error = validate_clawpkg_bytes(&zip_bytes).unwrap_err();
        assert!(error.contains("重复路径"), "unexpected error: {error}");
    }

    #[test]
    fn rejects_excessive_entry_count() {
        let names = (0..=CLAWPKG_MAX_ENTRIES)
            .map(|index| format!("assets/{index}.txt"))
            .collect::<Vec<_>>();
        let files = names
            .iter()
            .map(|name| (name.as_str(), b"x".as_slice()))
            .collect::<Vec<_>>();
        let zip_bytes = make_zip(&files);
        let error = validate_clawpkg_bytes(&zip_bytes).unwrap_err();
        assert!(error.contains("文件数超过"), "unexpected error: {error}");
    }

    #[test]
    fn rejects_oversized_expanded_entry() {
        let payload = vec![0u8; CLAWPKG_MAX_ENTRY_BYTES as usize + 1];
        let zip_bytes = make_zip(&[("assets/large.bin", payload.as_slice())]);
        let error = validate_clawpkg_bytes(&zip_bytes).unwrap_err();
        assert!(error.contains("单文件大小上限"), "unexpected error: {error}");
    }

    #[test]
    fn rejects_component_ids_that_cannot_be_removed_cross_runtime() {
        let invalid_ids = vec![
            "Uppercase".to_string(),
            "has.dot".to_string(),
            format!("a{}", "b".repeat(47)),
        ];
        for invalid_id in invalid_ids {
            let mut files_owned = valid_files();
            files_owned.retain(|(name, _)| *name != "component.json");
            files_owned.push((
                "component.json",
                format!(r#"{{"id":"{}","name":"X","version":"1.0.0"}}"#, invalid_id).into_bytes(),
            ));
            let files = files_owned
                .iter()
                .map(|(name, data)| (*name, data.as_slice()))
                .collect::<Vec<_>>();
            let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
            assert!(!result.ok);
            assert!(result
                .errors
                .iter()
                .any(|error| error.contains("1-47 位小写 ASCII 标识")));
        }
    }

    #[test]
    fn validates_optional_component_kind() {
        for kind in ["game", "tool"] {
            let mut files_owned = valid_files();
            files_owned.retain(|(name, _)| *name != "component.json");
            files_owned.push((
                "component.json",
                format!(
                    r#"{{"id":"x","name":"X","version":"1.0.0","kind":"{}"}}"#,
                    kind
                )
                .into_bytes(),
            ));
            let files = files_owned
                .iter()
                .map(|(name, data)| (*name, data.as_slice()))
                .collect::<Vec<_>>();
            let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
            assert!(
                result.ok,
                "expected {kind} to validate: {:?}",
                result.errors
            );
            assert_eq!(result.manifest.unwrap().kind.as_deref(), Some(kind));
        }

        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "component.json");
        files_owned.push((
            "component.json",
            br#"{"id":"x","name":"X","version":"1.0.0","kind":"dashboard"}"#.to_vec(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();
        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|error| error.contains("game/tool")));
    }

    #[test]
    fn validates_p4_json_size_after_whitespace_compaction() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "runtime/widget.json");
        files_owned.push((
            "runtime/widget.json",
            format!(
                "{{{}\"schema_version\": 1}}",
                "\n ".repeat(P4_WIDGET_JSON_MAX_BYTES)
            )
            .into_bytes(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(
            result.ok,
            "whitespace should compact away: {:?}",
            result.errors
        );
    }

    #[test]
    fn rejects_widget_content_beyond_p4_compacted_size_bound() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "runtime/widget.json");
        files_owned.push((
            "runtime/widget.json",
            serde_json::to_vec(&serde_json::json!({
                "schema_version": 1,
                "oversized": "x".repeat(P4_WIDGET_JSON_MAX_BYTES),
            }))
            .unwrap(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|error| error.contains("压缩后") && error.contains("4095")));
    }

    #[test]
    fn rejects_nonempty_array_widget_vars_before_staging() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "runtime/widget.json");
        files_owned.push((
            "runtime/widget.json",
            br#"{
              "schema_version":1,
              "vars":[{"name":"count","type":"int","init":0}]
            }"#
            .to_vec(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|error| error.contains("vars 必须是以变量名为键的 JSON 对象")));
    }

    #[test]
    fn rejects_unsupported_var_declaration_fields_before_staging() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "runtime/widget.json");
        files_owned.push((
            "runtime/widget.json",
            br#"{
              "schema_version":1,
              "vars":{"score":{"type":"int","init":0,"min":0}}
            }"#
            .to_vec(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result.errors.iter().any(|error| {
            error.contains("runtime/widget.json.vars.score")
                && error.contains("min")
                && error.contains("type")
                && error.contains("init")
        }));
    }

    #[test]
    fn accepts_legacy_empty_array_widget_vars_for_lossless_p4_normalization() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "runtime/widget.json");
        files_owned.push((
            "runtime/widget.json",
            br#"{"schema_version":1,"vars":[]}"#.to_vec(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(
            result.ok,
            "expected compatible legacy vars; {:?}",
            result.errors
        );
    }

    #[test]
    fn rejects_transition_and_tick_rules_beyond_p4_effect_bound() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "runtime/widget.json");
        files_owned.push((
            "runtime/widget.json",
            br#"{
              "schema_version":1,
              "transitions":[{
                "from":"*",
                "on":"game.restart",
                "set":{"a":0,"b":0,"c":0},
                "inc":{"d":1,"e":1}
              }],
              "tick":[{
                "every_ms":1000,
                "set":{"a":0,"b":0,"c":0,"d":0,"e":0},
                "then":{"set":{"a":0,"b":0,"c":0},"inc":{"d":1,"e":1}}
              }]
            }"#
            .to_vec(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|error| error.contains("transitions[0]") && error.contains("P4 4")));
        assert!(result
            .errors
            .iter()
            .any(|error| error.contains("tick[0]") && error.contains("P4 4")));
        assert!(result
            .errors
            .iter()
            .any(|error| error.contains("tick.then[0]") && error.contains("P4 4")));
    }

    #[test]
    fn rejects_missing_component_json() {
        let files: Vec<(&str, &[u8])> = vec![
            ("negative-screen.json", br#"{"dashboard":{}}"#.as_slice()),
            ("buttons.json", b"[]".as_slice()),
            ("runtime/widget.json", br#"{"schema_version":1}"#.as_slice()),
        ];
        let zip_bytes = make_zip(&files);
        let result = validate_clawpkg_bytes(&zip_bytes).unwrap();
        assert!(!result.ok);
        assert!(result.errors.iter().any(|e| e.contains("component.json")));
    }

    #[test]
    fn rejects_missing_runtime_widget_json() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "runtime/widget.json");
        files_owned.push(("runtime/.keep", b"".to_vec()));
        let files: Vec<(&str, &[u8])> = files_owned
            .iter()
            .map(|(n, d)| (*n, d.as_slice()))
            .collect();
        let zip_bytes = make_zip(&files);
        let result = validate_clawpkg_bytes(&zip_bytes).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|e| e.contains("runtime/widget.json")));
    }

    #[test]
    fn renders_component_dashboard_v1_payload() {
        let mut dashboard = HashMap::new();
        dashboard.insert("title".to_string(), "摸鱼倒计时".to_string());
        dashboard.insert("eyebrow".to_string(), "距离今天下班".to_string());
        dashboard.insert("headline".to_string(), "还有 2 小时 13 分".to_string());
        dashboard.insert("metricLabel".to_string(), "下班时间".to_string());
        dashboard.insert("metricValue".to_string(), "18:00".to_string());
        dashboard.insert("badge".to_string(), "5".to_string());
        dashboard.insert("note".to_string(), "本周已坚持 5 天".to_string());
        dashboard.insert("footer".to_string(), "红钮 切显示".to_string());
        let payload = render_component_dashboard_payload(&dashboard);
        let lines: Vec<&str> = payload.lines().collect();
        assert_eq!(lines[0], "COMPONENT_DASHBOARD_V1");
        assert!(lines.iter().any(|l| *l == "title=摸鱼倒计时"));
        assert!(lines.iter().any(|l| *l == "headline=还有 2 小时 13 分"));
        assert!(lines.iter().any(|l| *l == "footer=红钮 切显示"));
        // metricUnit not present in input - must be omitted from output
        assert!(!lines.iter().any(|l| l.starts_with("metricUnit=")));
    }

    #[test]
    fn payload_omits_empty_slots() {
        let dashboard = HashMap::new();
        let payload = render_component_dashboard_payload(&dashboard);
        assert_eq!(payload, "COMPONENT_DASHBOARD_V1\n");
    }

    #[test]
    fn validate_at_path_handles_directory_drafts() {
        /* Skill-generated drafts arrive as directories (no zip). The validator
        must walk the 6-file contract from disk and produce the same manifest
        preview as the zip path. */
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path().join("meeting-timer");
        std::fs::create_dir_all(dir.join("runtime")).unwrap();
        std::fs::create_dir_all(dir.join("assets")).unwrap();
        std::fs::write(
            dir.join("component.json"),
            r#"{"id":"meeting-timer","name":"会议计时","version":"1.0.0"}"#.as_bytes(),
        )
        .unwrap();
        std::fs::write(
            dir.join("negative-screen.json"),
            r#"{"dashboard":{"title":"会议","headline":"还有 12 分"}}"#.as_bytes(),
        )
        .unwrap();
        std::fs::write(dir.join("buttons.json"), b"[]").unwrap();
        std::fs::write(dir.join("share.json"), r#"{"title":"会议计时"}"#.as_bytes()).unwrap();
        std::fs::write(dir.join("runtime/widget.json"), br#"{"schema_version":1}"#).unwrap();
        std::fs::write(dir.join("assets/.keep"), b"").unwrap();

        let result = validate_clawpkg_at_path(&dir).expect("validate should run");
        assert!(result.ok, "expected valid; errors={:?}", result.errors);
        let manifest = result.manifest.expect("manifest should be built");
        assert_eq!(manifest.id, "meeting-timer");
        assert_eq!(manifest.name, "会议计时");
        assert_eq!(
            manifest.dashboard.get("title").map(String::as_str),
            Some("会议")
        );
    }

    #[test]
    fn validate_at_path_directory_missing_required_file_errors() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path().join("incomplete");
        std::fs::create_dir_all(dir.join("runtime")).unwrap();
        std::fs::create_dir_all(dir.join("assets")).unwrap();
        std::fs::write(
            dir.join("component.json"),
            br#"{"id":"x","name":"X","version":"1.0.0"}"#,
        )
        .unwrap();
        // missing negative-screen.json, buttons.json, share.json

        let result = validate_clawpkg_at_path(&dir).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|e| e.contains("negative-screen.json")));
        assert!(result.errors.iter().any(|e| e.contains("buttons.json")));
        assert!(result.errors.iter().any(|e| e.contains("share.json")));
    }

    #[test]
    fn rejects_slot_over_max_bytes() {
        let big_badge: String = "A".repeat(20); /* badge maxBytes 12 */
        let neg_screen = format!(r#"{{"dashboard":{{"badge":"{}"}}}}"#, big_badge);
        let files: Vec<(&str, &[u8])> = vec![
            (
                "component.json",
                br#"{"id":"x","name":"X","version":"1.0.0"}"#.as_slice(),
            ),
            ("negative-screen.json", neg_screen.as_bytes()),
            ("buttons.json", b"[]".as_slice()),
            ("runtime/widget.json", br#"{"schema_version":1}"#.as_slice()),
            ("assets/.keep", b"".as_slice()),
            ("share.json", br#"{"title":"X"}"#.as_slice()),
        ];
        let zip_bytes = make_zip(&files);
        let result = validate_clawpkg_bytes(&zip_bytes).unwrap();
        assert!(!result.ok);
        assert!(result.errors.iter().any(|e| e.contains("badge")));
    }

    #[test]
    fn accepts_bounded_pixel_game_visual_presets() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "negative-screen.json");
        files_owned.push((
            "negative-screen.json",
            br#"{"dashboard":{"title":"Whack A Mole","visualStyle":"pixel","visualPalette":"candy","visualLayout":"arcade","visualSprite":"mole-center"}}"#.to_vec(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(result.ok, "expected valid; errors={:?}", result.errors);
        let dashboard = &result.manifest.unwrap().dashboard;
        assert_eq!(
            dashboard.get("visualStyle").map(String::as_str),
            Some("pixel")
        );
        assert_eq!(
            dashboard.get("visualSprite").map(String::as_str),
            Some("mole-center")
        );
    }

    #[test]
    fn validates_bounded_native_game_shape_and_action_alignment() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "buttons.json" && *name != "runtime/widget.json");
        files_owned.push((
            "buttons.json",
            br#"[
              {"action":"snake.start","control":"\u524d\u65b9\u65cb\u94ae","event":"button.encoder.short_press","label":"\u5f00\u59cb"},
              {"action":"snake.left","control":"\u524d\u65b9\u65cb\u94ae","event":"knob.rotate_ccw","label":"\u5de6\u8f6c"},
              {"action":"snake.right","control":"\u524d\u65b9\u65cb\u94ae","event":"knob.rotate_cw","label":"\u53f3\u8f6c"}
            ]"#
            .to_vec(),
        ));
        files_owned.push((
            "runtime/widget.json",
            br#"{
              "schema_version":1,
              "vars":{"score":{"type":"int","init":0}},
              "states":["ready","playing","result"],
              "initial_state":"ready",
              "transitions":[
                {"from":"ready","on":"snake.start","to":"playing"},
                {"from":"playing","on":"snake.left"},
                {"from":"playing","on":"snake.right"}
              ],
              "game":{
                "type":"snake",
                "tick_ms":220,
                "playing_state":"playing",
                "result_state":"result",
                "score_var":"score",
                "actions":{"start":"snake.start","left":"snake.left","right":"snake.right"}
              },
              "dashboard":{}
            }"#
            .to_vec(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();
        let valid = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(valid.ok, "expected valid game; errors={:?}", valid.errors);

        files_owned.retain(|(name, _)| *name != "runtime/widget.json");
        files_owned.push((
            "runtime/widget.json",
            br#"{
              "schema_version":1,
              "vars":{"score":{"type":"int","init":0}},
              "states":["ready","playing","result"],
              "initial_state":"ready",
              "transitions":[
                {"from":"ready","on":"snake.start","to":"playing"},
                {"from":"playing","on":"snake.left"},
                {"from":"playing","on":"snake.right"}
              ],
              "game":{
                "type":"snake",
                "tick_ms":20,
                "playing_state":"playing",
                "result_state":"result",
                "score_var":"score",
                "actions":{"start":"snake.start","left":"snake.left","right":"snake.missing"}
              },
              "dashboard":{}
            }"#
            .to_vec(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();
        let invalid = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!invalid.ok);
        assert!(invalid.errors.iter().any(|error| error.contains("tick_ms")));
        assert!(invalid
            .errors
            .iter()
            .any(|error| error.contains("actions.right")));
    }

    #[test]
    fn validates_native_flappy_game_shape() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "buttons.json" && *name != "runtime/widget.json");
        files_owned.push((
            "buttons.json",
            br#"[{"action":"flappy.flap","control":"SW1","event":"button.sw1.short_press","label":"\u62cd\u7fc5"}]"#
                .to_vec(),
        ));
        files_owned.push((
            "runtime/widget.json",
            br#"{
              "schema_version":1,
              "vars":{"score":{"type":"int","init":0}},
              "states":["ready","playing","result"],
              "initial_state":"ready",
              "transitions":[{"from":"*","on":"flappy.flap","to":"playing"}],
              "game":{
                "type":"flappy",
                "tick_ms":100,
                "playing_state":"playing",
                "result_state":"result",
                "score_var":"score",
                "actions":{"flap":"flappy.flap"}
              },
              "dashboard":{}
            }"#
            .to_vec(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();
        let valid = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(
            valid.ok,
            "expected valid flappy game; errors={:?}",
            valid.errors
        );
    }

    #[test]
    fn rejects_package_authored_visual_modes() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "negative-screen.json");
        files_owned.push((
            "negative-screen.json",
            br#"{"dashboard":{"title":"Tap Challenge","visualStyle":"custom-css"}}"#.to_vec(),
        ));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result.errors.iter().any(|e| e.contains("未知视觉预置")));
    }

    #[test]
    fn rejects_more_than_eight_component_button_bindings() {
        let mut files_owned = valid_files();
        let buttons = (0..9)
            .map(|index| {
                serde_json::json!({
                    "action": format!("game.action_{}", index),
                    "control": "SW1",
                    "event": "button.sw1.short_press",
                    "label": format!("动作{}", index),
                })
            })
            .collect::<Vec<_>>();
        let encoded = serde_json::to_vec(&buttons).unwrap();
        files_owned.retain(|(name, _)| *name != "buttons.json");
        files_owned.push(("buttons.json", encoded));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|error| error.contains("最多允许 8")));
    }

    #[test]
    fn rejects_duplicate_or_unknown_component_button_events() {
        let mut files_owned = valid_files();
        let buttons = br#"[
          {"action":"game.start","control":"SW1","event":"button.sw1.short_press","label":"\u5f00\u59cb"},
          {"action":"game.retry","control":"SW1","event":"button.sw1.short_press","label":"\u91cd\u8bd5"},
          {"action":"game.cheat","control":"\u79d8\u5bc6\u952e","event":"button.secret","label":"\u4f5c\u5f0a"}
        ]"#;
        files_owned.retain(|(name, _)| *name != "buttons.json");
        files_owned.push(("buttons.json", buttons.to_vec()));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|error| error.contains("与已有绑定冲突")));
        assert!(result.errors.iter().any(|error| error.contains("未知事件")));
    }

    #[test]
    fn accepts_four_direction_joystick_component_events() {
        let mut files_owned = valid_files();
        let buttons = br#"[
          {"action":"game.up","control":"joystick","event":"joystick.up","label":"up"},
          {"action":"game.down","control":"joystick","event":"joystick.down","label":"down"},
          {"action":"game.left","control":"joystick","event":"knob.rotate_ccw","label":"left"},
          {"action":"game.right","control":"joystick","event":"knob.rotate_cw","label":"right"}
        ]"#;
        files_owned.retain(|(name, _)| *name != "buttons.json");
        files_owned.push(("buttons.json", buttons.to_vec()));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(result.ok, "unexpected errors: {:?}", result.errors);
    }

    #[test]
    fn rejects_duplicate_component_button_actions_that_cannot_remap_independently() {
        let mut files_owned = valid_files();
        let buttons = br#"[
          {"action":"game.score","control":"SW1","event":"button.sw1.short_press","label":"left"},
          {"action":"game.score","control":"SW2","event":"button.sw2.short_press","label":"middle"}
        ]"#;
        files_owned.retain(|(name, _)| *name != "buttons.json");
        files_owned.push(("buttons.json", buttons.to_vec()));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|error| error.contains("无法独立换键")));
    }

    #[test]
    fn rejects_overlapping_combined_knob_binding() {
        let mut files_owned = valid_files();
        let buttons = br#"[
          {"action":"game.adjust","control":"encoder","event":"knob.rotate_cw / knob.rotate_ccw","label":"adjust"},
          {"action":"game.next","control":"encoder","event":"knob.rotate_cw","label":"next"}
        ]"#;
        files_owned.retain(|(name, _)| *name != "buttons.json");
        files_owned.push(("buttons.json", buttons.to_vec()));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|error| error.contains("与已有绑定冲突")));
    }

    #[test]
    fn rejects_crowded_component_button_labels() {
        let mut files_owned = valid_files();
        let buttons = br#"[
          {"action":"game.start","control":"SW1","event":"button.sw1.short_press","label":"\u8fd9\u662f\u4e00\u4e2a\u660e\u663e\u8d85\u8fc7\u5341\u4e2a\u6c49\u5b57\u7684\u6309\u94ae\u63d0\u793a"}
        ]"#;
        files_owned.retain(|(name, _)| *name != "buttons.json");
        files_owned.push(("buttons.json", buttons.to_vec()));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(!result.ok);
        assert!(result.errors.iter().any(|error| error.contains("30 字节")));
    }

    #[test]
    fn validates_generic_scene_without_a_legacy_game_preset() {
        let mut files_owned = valid_files();
        let buttons = br#"[
          {"action":"catch.start","control":"encoder","event":"button.encoder.short_press","label":"start"},
          {"action":"catch.left","control":"encoder","event":"knob.rotate_ccw","label":"left"},
          {"action":"catch.right","control":"encoder","event":"knob.rotate_cw","label":"right"}
        ]"#;
        let widget = br#"{
          "schema_version":1,
          "engine":"p4-bounded-runtime-v3",
          "vars":{"score":{"type":"int","init":0}},
          "states":["ready","playing","result"],
          "initial_state":"ready",
          "transitions":[
            {"from":"*","on":"catch.start","to":"playing"},
            {"from":"playing","on":"catch.left"},
            {"from":"playing","on":"catch.right"}
          ],
          "tick":[],
          "scene":{
            "tick_ms":140,
            "active_state":"playing",
            "result_state":"result",
            "score_var":"score",
            "grid":{"width":12,"height":8},
            "entities":[
              {"id":"player","x":5,"y":7,"width":2,"tone":3,"shape":"paddle"},
              {"id":"star","x":5,"y":0,"tone":2,"vy":1,"bounds":"hide","shape":"star"}
            ],
            "rules":[
              {"on":"catch.start","do":[{"op":"restart"}]},
              {"on":"catch.left","do":[{"op":"move","entity":"player","dx":-1,"dy":0}]},
              {"on":"catch.right","do":[{"op":"move","entity":"player","dx":1,"dy":0}]},
              {"on":"collision","entity":"player","with":"star","do":[
                {"op":"score","add":1},
                {"op":"place","entity":"star","x":[0,11],"y":0}
              ]}
            ]
          },
          "dashboard":{}
        }"#;
        files_owned.retain(|(name, _)| !matches!(*name, "buttons.json" | "runtime/widget.json"));
        files_owned.push(("buttons.json", buttons.to_vec()));
        files_owned.push(("runtime/widget.json", widget.to_vec()));
        let files = files_owned
            .iter()
            .map(|(name, data)| (*name, data.as_slice()))
            .collect::<Vec<_>>();

        let result = validate_clawpkg_bytes(&make_zip(&files)).unwrap();
        assert!(result.ok, "generic scene errors: {:?}", result.errors);
    }
}
