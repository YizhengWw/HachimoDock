//! p4-app-form-v3: up to four bounded UTF-8 text fields with a per-field keyboard
//! mode (numeric / latin / pinyin) and five keyboard vars. Validation only; the
//! editor runs in the shared C core on the device and in the preview mirror.
use serde::Deserialize;
use serde_json::Value;
use std::collections::HashSet;
pub(crate) const ENGINE: &str = "p4-app-form-v3";
pub(crate) const CAPABILITY: &str = "widgetAppFormV3";
fn identifier(value: &str, max: usize) -> bool {
    !value.is_empty()
        && value.len() <= max
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
}
fn has_nul(value: &Value) -> bool {
    match value {
        Value::String(text) => text.contains('\0'),
        Value::Array(items) => items.iter().any(has_nul),
        Value::Object(fields) => fields
            .iter()
            .any(|(key, value)| key.contains('\0') || has_nul(value)),
        _ => false,
    }
}
fn int_init(runtime: &Value, name: &str) -> Result<i64, String> {
    if !identifier(name, 31) || runtime["vars"][name]["type"] != "int" {
        return Err(v3bad());
    }
    runtime["vars"][name]
        .get("init")
        .map_or(Ok(0), |v| v.as_i64().ok_or_else(v3bad))
        .and_then(|value| {
            if (-1_000_000_000..=1_000_000_000).contains(&value) {
                Ok(value)
            } else {
                Err(v3bad())
            }
        })
}
fn limit_covers(runtime: &Value, name: &str, lo: i64, hi: i64) -> Result<(), String> {
    if let Some(limit) = runtime["limits"].get(name) {
        let limits = limit
            .as_array()
            .filter(|a| a.len() == 2)
            .ok_or_else(v3bad)?;
        if !limits[0]
            .as_i64()
            .is_some_and(|n| n >= -1_000_000_000 && n <= lo)
            || !limits[1]
                .as_i64()
                .is_some_and(|n| n >= hi && n <= 1_000_000_000)
        {
            return Err(v3bad());
        }
    }
    Ok(())
}
fn cannot_write(rule: &Value, protected: &HashSet<&str>) -> Result<(), String> {
    for op in ["set", "inc"] {
        if let Some(values) = rule.get(op) {
            if values
                .as_object()
                .ok_or_else(v3bad)?
                .keys()
                .any(|name| protected.contains(name.as_str()))
            {
                return Err(v3bad());
            }
        }
    }
    Ok(())
}

const PHYSICAL: [&str; 8] = [
    "button.sw1.short_press",
    "button.sw2.short_press",
    "button.encoder.short_press",
    "button.encoder.long_press",
    "joystick.up",
    "joystick.down",
    "knob.rotate_cw",
    "knob.rotate_ccw",
];
const MODES: [&str; 3] = ["numeric", "latin", "pinyin"];
const RESERVED_V3: [&str; 9] = [
    "page_toggle",
    "page_enter",
    "page_back",
    "page_main",
    "page_app",
    "component_center",
    "tab.next",
    "tab.prev",
    "data.refresh",
];
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Form {
    engine: String,
    page: String,
    focus_var: String,
    active_var: String,
    fields: Vec<Field>,
    actions: MixedActions,
    keyboard: Keyboard,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Field {
    id: String,
    var: String,
    draft_var: String,
    kind: String,
    max_bytes: u64,
    keyboard: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct MixedActions {
    previous: String,
    next: String,
    decrease: String,
    increase: String,
    accept: String,
    cancel: String,
    append: String,
    erase: String,
}
impl MixedActions {
    fn all(&self) -> [&str; 8] {
        [
            &self.previous,
            &self.next,
            &self.decrease,
            &self.increase,
            &self.accept,
            &self.cancel,
            &self.append,
            &self.erase,
        ]
    }
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Keyboard {
    key_var: String,
    cursor_var: String,
    buffer_var: String,
    label_var: String,
    candidates_var: String,
}
fn v3bad() -> String {
    "多字段文本表单需要 p4-app-form-v3：1–4 个 ≤63 字节文本字段、键盘模式、五个键盘变量、八个可达动作与仅提交值的存档".into()
}
fn printable(s: &str, max: usize) -> bool {
    s.len() <= max
        && !s
            .chars()
            .any(|c| c <= '\u{1f}' || ('\u{7f}'..='\u{9f}').contains(&c))
}
fn string_init<'a>(r: &'a Value, name: &str) -> Result<&'a str, String> {
    if !identifier(name, 31)
        || r["vars"][name]["type"] != "string"
        || r["limits"].get(name).is_some()
    {
        return Err(v3bad());
    }
    let s = r["vars"][name]
        .get("init")
        .map_or(Some(""), Value::as_str)
        .ok_or_else(v3bad)?;
    if printable(s, 63) {
        Ok(s)
    } else {
        Err(v3bad())
    }
}
fn int_zero(r: &Value, name: &str, lo: i64, hi: i64) -> Result<(), String> {
    if int_init(r, name).map_err(|_| v3bad())? != 0 {
        return Err(v3bad());
    }
    limit_covers(r, name, lo, hi).map_err(|_| v3bad())
}
pub(crate) fn declared(r: &Value) -> bool {
    r["form"]["engine"] == ENGINE
}
pub(crate) fn require_capability(r: &Value, capabilities: &Value) -> Result<(), String> {
    if declared(r) && capabilities[CAPABILITY] != ENGINE {
        return Err("需要 p4-app-form-v3 能力".into());
    }
    Ok(())
}
pub(crate) fn validate_runtime(r: &Value) -> Result<(), String> {
    let mixed_host = crate::app_variable_owners::mixed(r)?;
    if has_nul(r) {
        return Err(v3bad());
    }
    let f: Form = serde_json::from_value(r["form"].clone()).map_err(|_| v3bad())?;
    if f.engine != ENGINE
        || r["engine"] != "p4-bounded-runtime-v4"
        || r["surface"]["engine"] != "p4-app-surface-v1"
        || r["surface"]["graphics"] != "p4-app-graphics-v1"
        || r["surface"]["bindings"] != "p4-variable-bindings-v1"
        || r["input"]["engine"] != "p4-input-v1"
        || !(1..=4).contains(&f.fields.len())
        || ["local", "scene", "game", "data", "media", "instrument"]
            .iter()
            .any(|k| r.get(*k).is_some())
        || r.get("host").is_some() && !mixed_host
    {
        return Err(v3bad());
    }
    let (max_vars, max_string, max_pages) = match r["quota"].as_str() {
        Some("p4-quota-v3") => (32, 127, 12),
        Some("p4-quota-v2") => (16, 63, 6),
        _ => (8, 63, 4),
    };
    let vars = r["vars"]
        .as_object()
        .filter(|v| (1..=max_vars).contains(&v.len()))
        .ok_or_else(v3bad)?;
    if vars.len() > 8 && !matches!(r["quota"].as_str(), Some("p4-quota-v2" | "p4-quota-v3")) {
        return Err(v3bad());
    }
    for d in vars.values() {
        if d["type"] == "string" {
            if !d
                .get("init")
                .map_or(Some(""), Value::as_str)
                .is_some_and(|s| printable(s, max_string))
            {
                return Err(v3bad());
            }
        } else if d["type"] != "int"
            || !d
                .get("init")
                .map_or(Some(0), Value::as_i64)
                .is_some_and(|n| (-1_000_000_000..=1_000_000_000).contains(&n))
        {
            return Err(v3bad());
        }
    }
    let pages = r["pages"]
        .as_array()
        .filter(|a| (1..=max_pages).contains(&a.len()))
        .ok_or_else(v3bad)?;
    if !pages.iter().any(|p| p["id"] == f.page) {
        return Err(v3bad());
    }
    let mut roles = HashSet::new();
    let mut ephemeral = HashSet::new();
    for name in [&f.focus_var, &f.active_var] {
        if !roles.insert(name.as_str()) {
            return Err(v3bad());
        }
        ephemeral.insert(name.as_str());
    }
    int_zero(r, &f.focus_var, 0, f.fields.len() as i64 - 1)?;
    int_zero(r, &f.active_var, 0, 1)?;
    let mut ids = HashSet::new();
    for q in &f.fields {
        if !identifier(&q.id, 15)
            || !ids.insert(q.id.as_str())
            || q.kind != "text"
            || !MODES.contains(&q.keyboard.as_str())
            || !(1..=63).contains(&q.max_bytes)
        {
            return Err(v3bad());
        }
        if !roles.insert(q.var.as_str()) || !roles.insert(q.draft_var.as_str()) {
            return Err(v3bad());
        }
        ephemeral.insert(q.draft_var.as_str());
        let initial = string_init(r, &q.var)?;
        if initial.len() > q.max_bytes as usize || string_init(r, &q.draft_var)? != initial {
            return Err(v3bad());
        }
    }
    let k = &f.keyboard;
    for name in [
        &k.key_var,
        &k.cursor_var,
        &k.buffer_var,
        &k.label_var,
        &k.candidates_var,
    ] {
        if !roles.insert(name.as_str()) {
            return Err(v3bad());
        }
        ephemeral.insert(name.as_str());
    }
    int_zero(r, &k.key_var, 0, 11)?;
    int_zero(r, &k.cursor_var, 0, 63)?;
    for name in [&k.buffer_var, &k.label_var, &k.candidates_var] {
        if !string_init(r, name)?.is_empty() {
            return Err(v3bad());
        }
    }
    let mut actions = HashSet::new();
    let transitions = r["transitions"].as_array().ok_or_else(v3bad)?;
    for a in f.actions.all() {
        if !identifier(a, 47)
            || a.starts_with("host.")
            || a.starts_with("input.")
            || RESERVED_V3.contains(&a)
            || !actions.insert(a)
        {
            return Err(v3bad());
        }
        let found: Vec<_> = transitions.iter().filter(|t| t["on"] == a).collect();
        if found.len() != 1
            || *found[0] != serde_json::json!({"from":"*","on":a})
            || r["input"]["hold"] == a
            || r["input"]["press"]
                .as_array()
                .is_some_and(|v| v.iter().any(|x| x == a))
            || ["confirm", "cooldown"]
                .iter()
                .any(|key| r["input"][key].get(a).is_some())
        {
            return Err(v3bad());
        }
    }
    if r["input"]["repeat"].as_array().is_some_and(|v| {
        v.iter().any(|x| {
            [
                &f.actions.accept,
                &f.actions.cancel,
                &f.actions.append,
                &f.actions.erase,
            ]
            .iter()
            .any(|a| x == *a)
        })
    }) {
        return Err(v3bad());
    }
    for t in transitions {
        cannot_write(t, &roles).map_err(|_| v3bad())?;
        if let Some(otherwise) = t.get("else") { cannot_write(otherwise, &roles).map_err(|_| v3bad())?; }
    }
    if let Some(ticks) = r.get("tick") {
        for tick in ticks.as_array().ok_or_else(v3bad)? {
            cannot_write(tick, &roles).map_err(|_| v3bad())?;
            if let Some(then) = tick.get("then") {
                cannot_write(then, &roles).map_err(|_| v3bad())?;
            }
        }
    }
    if r["persist"]["vars"].as_array().is_some_and(|v| {
        v.iter()
            .any(|n| n.as_str().is_some_and(|n| ephemeral.contains(n)))
    }) {
        return Err(v3bad());
    }
    for n in r["surface"]["elements"].as_array().into_iter().flatten() {
        if let Some(field) = n.get("field_bind") {
            if !field.as_str().is_some_and(|id| ids.contains(id))
                || !matches!(n["type"].as_str(), Some("text" | "button"))
                || ["text", "bind", "var_bind", "key_for"]
                    .iter()
                    .any(|k| n.get(*k).is_some())
            {
                return Err(v3bad());
            }
        }
    }
    Ok(())
}
pub(crate) fn validate_field_node(n: &Value, r: &Value) -> Result<(), String> {
    let Some(id) = n.get("field_bind") else {
        return Ok(());
    };
    let f: Form = serde_json::from_value(r["form"].clone()).map_err(|_| v3bad())?;
    if !id
        .as_str()
        .is_some_and(|id| f.fields.iter().any(|f| f.id == id))
        || !matches!(n["type"].as_str(), Some("text" | "button"))
        || ["text", "bind", "var_bind", "key_for", "row_bind"]
            .iter()
            .any(|k| n.get(*k).is_some())
    {
        return Err(v3bad());
    }
    Ok(())
}
pub(crate) fn bindings(r: &Value, b: &Value) -> Result<(), String> {
    let f: Form = serde_json::from_value(r["form"].clone()).map_err(|_| v3bad())?;
    let bindings = b.as_array().filter(|a| a.len() == 8).ok_or_else(v3bad)?;
    let mut events = HashSet::new();
    for b in bindings {
        let event = b["event"]
            .as_str()
            .filter(|e| PHYSICAL.contains(e))
            .ok_or_else(v3bad)?;
        if !events.insert(event)
            || !b["control"].as_str().is_some_and(|s| !s.trim().is_empty())
            || !b["label"]
                .as_str()
                .is_some_and(|s| !s.trim().is_empty() && s.len() <= 30)
            || r["input"]["repeat"]
                .as_array()
                .is_some_and(|a| a.contains(&b["action"]))
                && ![
                    "joystick.up",
                    "joystick.down",
                    "knob.rotate_cw",
                    "knob.rotate_ccw",
                ]
                .contains(&event)
        {
            return Err(v3bad());
        }
    }
    for a in f.actions.all() {
        if !bindings
            .iter()
            .any(|b| b["action"] == a && b["event"].as_str().is_some_and(|e| PHYSICAL.contains(&e)))
        {
            return Err(v3bad());
        }
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn quota_v3_allows_32_vars_and_preserves_unowned_127_byte_strings() {
        let suite: Value = serde_json::from_str(include_str!(
            "../../skills/petui/contracts/app-form-v3.json"
        ))
        .unwrap();
        let mut r = suite["runtime"].clone();
        r["quota"] = "p4-quota-v3".into();
        while r["vars"].as_object().unwrap().len() < 32 {
            let i = r["vars"].as_object().unwrap().len();
            r["vars"][format!("extra{i}")] =
                serde_json::json!({"type":"string","init":"x".repeat(127)});
        }
        validate_runtime(&r).unwrap();
        let name = r["form"]["fields"][0]["var"].as_str().unwrap().to_owned();
        r["vars"][name]["init"] = "x".repeat(64).into();
        assert!(validate_runtime(&r).is_err());
    }
    #[test]
    fn shared_text_form_vectors() {
        let suite: Value = serde_json::from_str(include_str!(
            "../../skills/petui/contracts/app-form-v3.json"
        ))
        .unwrap();
        let r = &suite["runtime"];
        validate_runtime(r).unwrap();
        bindings(r, &suite["buttons"]).unwrap();
        for c in suite["invalid"].as_array().unwrap() {
            let mut bad = r.clone();
            let path = c["path"].as_array().unwrap();
            let mut parent = &mut bad;
            for key in &path[..path.len() - 1] {
                parent = if let Some(s) = key.as_str() {
                    &mut parent[s]
                } else {
                    &mut parent[key.as_u64().unwrap() as usize]
                };
            }
            let key = path.last().unwrap();
            if let Some(s) = key.as_str() {
                parent[s] = c["value"].clone();
            } else {
                parent[key.as_u64().unwrap() as usize] = c["value"].clone();
            }
            assert!(validate_runtime(&bad).is_err(), "{c}");
        }
        let mut no_quota = r.clone();
        no_quota.as_object_mut().unwrap().remove("quota");
        assert!(validate_runtime(&no_quota).is_err());
        assert!(
            require_capability(r, &serde_json::json!({"widgetAppFormV2":"p4-app-form-v2"}))
                .is_err()
        );
        require_capability(r, &serde_json::json!({"widgetAppFormV3":ENGINE})).unwrap();
        assert!(bindings(r, &serde_json::json!([])).is_err());
    }
}
