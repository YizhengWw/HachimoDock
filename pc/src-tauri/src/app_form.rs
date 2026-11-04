//! Strict bounded local form schema. No executable package code or physical-key
//! policy: six logical actions are resolved through the actual button mappings.
use serde::Deserialize;
use serde_json::Value;
use std::collections::HashSet;
#[path="app_form_v2.rs"]
mod mixed;
#[path="app_form_v3.rs"]
mod text_fields;

pub(crate) const ENGINE: &str = "p4-app-form-v1";
const INTEGER_MIN: i64 = -1_000_000_000;
const INTEGER_MAX: i64 = 1_000_000_000;
const RESERVED: [&str; 6] = [
    "page_main",
    "page_back",
    "page_enter",
    "page_toggle",
    "page_app",
    "component_center",
];
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
const DIRECTIONS: [&str; 4] = [
    "joystick.up",
    "joystick.down",
    "knob.rotate_cw",
    "knob.rotate_ccw",
];

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Form {
    engine: String,
    page: String,
    focus_var: String,
    active_var: String,
    fields: Vec<Field>,
    actions: Actions,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Actions {
    previous: String,
    next: String,
    decrease: String,
    increase: String,
    accept: String,
    cancel: String,
}
impl Actions {
    fn all(&self) -> [&str; 6] {
        [
            &self.previous,
            &self.next,
            &self.decrease,
            &self.increase,
            &self.accept,
            &self.cancel,
        ]
    }
}
#[derive(Deserialize)]
#[serde(tag = "kind", deny_unknown_fields)]
enum Field {
    #[serde(rename = "choice")]
    Choice {
        id: String,
        var: String,
        draft_var: String,
        options: Vec<Choice>,
    },
    #[serde(rename = "integer")]
    Integer {
        id: String,
        var: String,
        draft_var: String,
        min: i64,
        max: i64,
        step: i64,
    },
}
impl Field {
    fn names(&self) -> (&str, &str, &str) {
        match self {
            Self::Choice {
                id, var, draft_var, ..
            }
            | Self::Integer {
                id, var, draft_var, ..
            } => (id, var, draft_var),
        }
    }
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Choice {
    value: i64,
    label: String,
}

fn bad() -> String {
    "表单需要 p4-app-form-v1、有界本地字段、独立草稿及可达逻辑动作".into()
}
fn identifier(value: &str, max: usize) -> bool {
    !value.is_empty()
        && value.len() <= max
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
}
fn parse(runtime: &Value) -> Result<Form, String> {
    serde_json::from_value(runtime["form"].clone()).map_err(|_| bad())
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
        return Err(bad());
    }
    runtime["vars"][name]
        .get("init")
        .map_or(Ok(0), |v| v.as_i64().ok_or_else(bad))
        .and_then(|value| {
            if (INTEGER_MIN..=INTEGER_MAX).contains(&value) {
                Ok(value)
            } else {
                Err(bad())
            }
        })
}
fn limit_covers(runtime: &Value, name: &str, lo: i64, hi: i64) -> Result<(), String> {
    if let Some(limit) = runtime["limits"].get(name) {
        let limits = limit.as_array().filter(|a| a.len() == 2).ok_or_else(bad)?;
        if !limits[0]
            .as_i64()
            .is_some_and(|n| n >= INTEGER_MIN && n <= lo)
            || !limits[1]
                .as_i64()
                .is_some_and(|n| n >= hi && n <= INTEGER_MAX)
        {
            return Err(bad());
        }
    }
    Ok(())
}
fn cannot_write(rule: &Value, protected: &HashSet<&str>) -> Result<(), String> {
    for op in ["set", "inc"] {
        if let Some(values) = rule.get(op) {
            if values
                .as_object()
                .ok_or_else(bad)?
                .keys()
                .any(|name| protected.contains(name.as_str()))
            {
                return Err(bad());
            }
        }
    }
    Ok(())
}

pub(crate) fn validate_runtime(runtime: &Value) -> Result<(), String> {
    if text_fields::declared(runtime) { return text_fields::validate_runtime(runtime); }
    let mixed_host=crate::app_variable_owners::mixed(runtime)?;
    if runtime["form"]["engine"]==mixed::ENGINE{return validate_runtime(&mixed::normalized(runtime)?);}
    let nodes = runtime["surface"]["elements"]
        .as_array()
        .map(Vec::as_slice)
        .unwrap_or(&[]);
    if runtime.get("form").is_none() {
        return if nodes.iter().any(|node| node.get("field_bind").is_some()) {
            Err(bad())
        } else {
            Ok(())
        };
    }
    // cJSON strings are NUL-terminated. Match the firmware's raw JSON guard so
    // a field/key cannot mean something different after native parsing.
    if has_nul(runtime) {
        return Err(bad());
    }
    let form = parse(runtime)?;
    if form.engine != ENGINE
        || runtime["engine"] != "p4-bounded-runtime-v4"
        || runtime["surface"]["engine"] != "p4-app-surface-v1"
        || runtime["surface"]["graphics"] != "p4-app-graphics-v1"
        || runtime["surface"]["bindings"] != "p4-variable-bindings-v1"
        || runtime["input"]["engine"] != "p4-input-v1"
        || [
            "local",
            "scene",
            "game",
            "data",
            "media",
            "instrument",
        ]
        .iter()
        .any(|key| runtime.get(*key).is_some())
        ||runtime.get("host").is_some()&&!mixed_host
        || !(1..=3).contains(&form.fields.len())
    {
        return Err(bad());
    }
    let vars = runtime["vars"].as_object().filter(|v|v.len()<=8).ok_or_else(bad)?;
    crate::clawpkg::validate_p4_vars_object(vars)?;
    let pages = runtime["pages"]
        .as_array()
        .filter(|a| (1..=4).contains(&a.len()))
        .ok_or_else(bad)?;
    let mut page_ids = HashSet::new();
    for page in pages {
        let id = page["id"]
            .as_str()
            .filter(|id| identifier(id, 23))
            .ok_or_else(bad)?;
        if page.as_object().is_none_or(|o| o.len() != 1) || !page_ids.insert(id) {
            return Err(bad());
        }
    }
    if !page_ids.contains(form.page.as_str()) {
        return Err(bad());
    }
    let mut all_vars = HashSet::new();
    let mut temporary_vars = HashSet::new();
    for name in [&form.focus_var, &form.active_var] {
        if !all_vars.insert(name.as_str()) || int_init(runtime, name)? != 0 {
            return Err(bad());
        }
        temporary_vars.insert(name.as_str());
    }
    limit_covers(runtime, &form.focus_var, 0, form.fields.len() as i64 - 1)?;
    limit_covers(runtime, &form.active_var, 0, 1)?;
    let mut field_ids = HashSet::new();
    for field in &form.fields {
        let (id, committed, draft) = field.names();
        if !identifier(id, 15)
            || !field_ids.insert(id)
            || !all_vars.insert(committed)
            || !all_vars.insert(draft)
        {
            return Err(bad());
        }
        temporary_vars.insert(draft);
        let initial = int_init(runtime, committed)?;
        if int_init(runtime, draft)? != initial {
            return Err(bad());
        }
        let (lo, hi) = match field {
            Field::Choice { options, .. } => {
                if !(2..=8).contains(&options.len()) {
                    return Err(bad());
                }
                let mut values = HashSet::new();
                for option in options {
                    if !(INTEGER_MIN..=INTEGER_MAX).contains(&option.value)
                        || !values.insert(option.value)
                        || option.label.is_empty()
                        || option.label.len() > 63
                        || option.label.chars().any(|c| c <= '\u{1f}' || c == '\u{7f}')
                    {
                        return Err(bad());
                    }
                }
                if !values.contains(&initial) {
                    return Err(bad());
                }
                (
                    *values.iter().min().ok_or_else(bad)?,
                    *values.iter().max().ok_or_else(bad)?,
                )
            }
            Field::Integer { min, max, step, .. } => {
                if *min < INTEGER_MIN
                    || *max > INTEGER_MAX
                    || min >= max
                    || !(1..=INTEGER_MAX).contains(step)
                    || (max - min) % step != 0
                    || initial < *min
                    || initial > *max
                    || (initial - min) % step != 0
                {
                    return Err(bad());
                }
                (*min, *max)
            }
        };
        for name in [committed, draft] {
            limit_covers(runtime, name, lo, hi)?;
        }
    }
    if runtime["persist"]["vars"].as_array().is_some_and(|vars| {
        vars.iter()
            .any(|v| v.as_str().is_some_and(|name| temporary_vars.contains(name)))
    }) {
        return Err(bad());
    }
    let actions = form.actions.all();
    let mut unique_actions = HashSet::new();
    let transitions = runtime["transitions"].as_array().ok_or_else(bad)?;
    for action in actions {
        if !identifier(action, 47)
            || action.starts_with("host.")
            || action.starts_with("input.")
            || RESERVED.contains(&action)
            || !unique_actions.insert(action)
        {
            return Err(bad());
        }
        let rules: Vec<_> = transitions.iter().filter(|t| t["on"] == action).collect();
        if rules.len() != 1
            || rules[0]["from"] != "*"
            || rules[0]
                .as_object()
                .is_none_or(|o| o.len() != 2 || !o.contains_key("on") || !o.contains_key("from"))
        {
            return Err(bad());
        }
        if runtime["input"]["hold"] == action
            || runtime["input"]["press"]
                .as_array()
                .is_some_and(|a| a.iter().any(|v| v == action))
            || ["confirm", "cooldown"]
                .iter()
                .any(|key| runtime["input"][key].get(action).is_some())
        {
            return Err(bad());
        }
    }
    if runtime["input"]["repeat"].as_array().is_some_and(|a| {
        a.iter()
            .any(|v| v == &form.actions.accept || v == &form.actions.cancel)
    }) {
        return Err(bad());
    }
    for transition in transitions {
        cannot_write(transition, &temporary_vars)?;
        if let Some(otherwise) = transition.get("else") { cannot_write(otherwise, &temporary_vars)?; }
    }
    if let Some(ticks) = runtime.get("tick") {
        for tick in ticks.as_array().ok_or_else(bad)? {
            cannot_write(tick, &all_vars)?;
            if let Some(then) = tick.get("then") {
                cannot_write(then, &all_vars)?;
            }
        }
    }
    for node in nodes {
        validate_field_node(node, Some(runtime))?;
    }
    Ok(())
}

pub(crate) fn validate_field_node(node: &Value, runtime: Option<&Value>) -> Result<(), String> {
    if let Some(r)=runtime.filter(|r|text_fields::declared(r)) { return text_fields::validate_field_node(node,r); }
    if let Some(r)=runtime.filter(|r|r["form"]["engine"]==mixed::ENGINE){return validate_field_node(node,Some(&mixed::normalized(r)?));}
    let Some(binding) = node.get("field_bind") else {
        return Ok(());
    };
    let id = binding
        .as_str()
        .filter(|id| identifier(id, 15))
        .ok_or_else(bad)?;
    let runtime = runtime.ok_or_else(bad)?;
    let form = parse(runtime)?;
    if form.engine != ENGINE
        || !matches!(node["type"].as_str(), Some("text" | "button"))
        || ["text", "bind", "var_bind", "key_for"]
            .iter()
            .any(|key| node.get(*key).is_some())
        || !form.fields.iter().any(|field| field.names().0 == id)
    {
        return Err(bad());
    }
    Ok(())
}

/// Run both at import and after user remapping, before starting a USB transaction.
pub(crate) fn validate_bindings(runtime: &Value, buttons: &Value) -> Result<(), String> {
    if text_fields::declared(runtime) {
        if has_nul(buttons) { return Err(bad()); }
        super::validate_app_runtime(runtime)?;
        super::validate_app_surface(&runtime["surface"], Some(runtime))?;
        return text_fields::bindings(runtime, buttons);
    }
    if runtime["form"]["engine"]==mixed::ENGINE{
        validate_runtime(runtime)?;mixed::bindings(runtime,buttons)?;
        return validate_bindings(&mixed::normalized(runtime)?,buttons);
    }
    if runtime.get("form").is_none() {
        return Ok(());
    }
    if has_nul(buttons) {
        return Err(bad());
    }
    // The transfer path can be called directly with an installed directory.
    // Recheck the whole bounded runtime/surface even when it has no PNG assets.
    super::validate_app_runtime(runtime)?;
    super::validate_app_surface(&runtime["surface"], Some(runtime))?;
    let form = parse(runtime)?;
    let bindings = buttons
        .as_array()
        .filter(|a| (6..=8).contains(&a.len()))
        .ok_or_else(bad)?;
    let mut events = HashSet::new();
    let mut actions = HashSet::new();
    for binding in bindings {
        let action = binding["action"]
            .as_str()
            .filter(|s| identifier(s, 47))
            .ok_or_else(bad)?;
        let event = binding["event"].as_str().ok_or_else(bad)?;
        if RESERVED.contains(&action)
            || !actions.insert(action)
            || !(PHYSICAL.contains(&event)
                || ["screen.region.tap", "screen.region.long_press"].contains(&event))
            || !binding["control"]
                .as_str()
                .is_some_and(|s| !s.trim().is_empty())
            || !binding["label"]
                .as_str()
                .is_some_and(|s| !s.trim().is_empty() && s.len() <= 30)
        {
            return Err(bad());
        }
        if !events.insert(event) {
            return Err(bad());
        }
    }
    for action in form.actions.all() {
        let binding = bindings
            .iter()
            .find(|b| b["action"] == action)
            .ok_or_else(bad)?;
        let event = binding["event"].as_str().ok_or_else(bad)?;
        if !PHYSICAL.contains(&event) {
            return Err(bad());
        }
        if runtime["input"]["repeat"]
            .as_array()
            .is_some_and(|a| a.iter().any(|v| v == action))
            && !DIRECTIONS.contains(&event)
        {
            return Err(bad());
        }
    }
    Ok(())
}

pub(crate) fn require_capability(runtime: &Value, capabilities: &Value) -> Result<(), String> {
    if text_fields::declared(runtime) { return text_fields::require_capability(runtime, capabilities); }
    if runtime["form"]["engine"]==mixed::ENGINE{
        return if capabilities["widgetAppFormV2"]==mixed::ENGINE{Ok(())}else{Err("需要混合文本表单能力；未安装不兼容资源".into())};
    }
    if runtime.get("form").is_some() && capabilities["widgetAppForm"] != ENGINE {
        return Err("需要有界表单能力，请先升级固件；未安装不兼容资源".into());
    }
    Ok(())
}

#[cfg(test)]
pub(crate) fn fixture() -> (Value, Value) {
    use serde_json::json;
    let mut runtime = json!({"schema_version":1,"engine":"p4-bounded-runtime-v4",
        "states":["ready"],"initial_state":"ready","pages":[{"id":"home"},{"id":"settings"}],"initial_page":"home",
        "vars":{"focus":{"type":"int","init":0},"editing":{"type":"int","init":0},
            "choice":{"type":"int","init":2},"choice_draft":{"type":"int","init":2},
            "number":{"type":"int","init":10},"number_draft":{"type":"int","init":10}},
        "input":{"engine":"p4-input-v1","repeat":["edit.increase"]},"tick":[],"dashboard":{"title":"Form"},
        "form":{"engine":ENGINE,"page":"settings","focus_var":"focus","active_var":"editing",
            "fields":[{"id":"selection","kind":"choice","var":"choice","draft_var":"choice_draft",
                "options":[{"value":2,"label":"A"},{"value":5,"label":"B"}]},
                {"id":"quantity","kind":"integer","var":"number","draft_var":"number_draft","min":0,"max":20,"step":5}],
            "actions":{"previous":"edit.previous","next":"edit.next","decrease":"edit.decrease",
                "increase":"edit.increase","accept":"edit.accept","cancel":"edit.cancel"}},
        "surface":{"engine":"p4-app-surface-v1","graphics":"p4-app-graphics-v1","bindings":"p4-variable-bindings-v1",
            "background":0,"elements":[{"type":"text","x":20,"y":20,"w":200,"h":40,"color":65535,"field_bind":"selection"},
                {"type":"button","x":20,"y":70,"w":200,"h":44,"color":200,"field_bind":"quantity","action":"edit.accept"}]}});
    let actions = [
        "edit.previous",
        "edit.next",
        "edit.decrease",
        "edit.increase",
        "edit.accept",
        "edit.cancel",
    ];
    runtime["transitions"] = json!(actions
        .iter()
        .map(|action| json!({"from":"*","on":action}))
        .collect::<Vec<_>>());
    let events = [
        "joystick.up",
        "joystick.down",
        "knob.rotate_ccw",
        "knob.rotate_cw",
        "button.sw1.short_press",
        "button.sw2.short_press",
    ];
    let buttons = json!(actions.iter().zip(events).map(|(action,event)| json!({"action":action,"control":"Control","event":event,"label":"Edit"})).collect::<Vec<_>>());
    (runtime, buttons)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn valid(runtime: &Value) -> bool {
        super::super::validate_app_runtime(runtime).is_ok()
            && super::super::validate_app_surface(&runtime["surface"], Some(runtime)).is_ok()
    }

    #[test]
    fn app_form_shared_schema_vectors() {
        let vectors: Value = serde_json::from_str(include_str!(
            "../../skills/petui/contracts/app-form-v1.json"
        ))
        .unwrap();
        let runtime = &vectors["runtime"];
        assert!(valid(runtime));
        assert!(validate_bindings(runtime, &vectors["buttons"]).is_ok());
        for case in vectors["invalid"].as_array().unwrap() {
            let mut invalid = runtime.clone();
            let path = case["path"].as_array().unwrap();
            let mut parent = &mut invalid;
            for segment in &path[..path.len() - 1] {
                parent = if let Some(key) = segment.as_str() {
                    &mut parent[key]
                } else {
                    &mut parent[segment.as_u64().unwrap() as usize]
                };
            }
            let last = path.last().unwrap();
            if let Some(key) = last.as_str() {
                parent[key] = case["value"].clone();
            } else {
                parent[last.as_u64().unwrap() as usize] = case["value"].clone();
            }
            assert!(!valid(&invalid), "accepted shared case {}", case["name"]);
        }
        let mut invalid = runtime.clone();
        invalid["dashboard"]["title"] = json!("bad\0text");
        assert!(!valid(&invalid));
        let mut invalid = runtime.clone();
        invalid["dashboard"]["bad\0key"] = json!("text");
        assert!(!valid(&invalid));
        let mut literal = runtime.clone();
        literal["dashboard"]["title"] = json!(r"literal \u0000");
        assert!(valid(&literal));
        for key in ["label", "control", "extra"] {
            let mut invalid = vectors["buttons"].clone();
            invalid[0][key] = json!("bad\0text");
            assert!(validate_bindings(runtime, &invalid).is_err());
        }
        let mut invalid = vectors["buttons"].clone();
        invalid[0]["bad\0key"] = json!("text");
        assert!(validate_bindings(runtime, &invalid).is_err());
        let mut literal = vectors["buttons"].clone();
        literal[0]["label"] = json!(r"literal \u0000");
        assert!(validate_bindings(runtime, &literal).is_ok());
    }

    #[test]
    fn app_form_schema_types_budgets_and_transient_ownership_are_strict() {
        let (runtime, buttons) = fixture();
        assert!(valid(&runtime));
        assert!(validate_bindings(&runtime, &buttons).is_ok());
        for (pointer, value) in [
            ("/form/engine", json!("future")),
            ("/form/page", json!("missing")),
            ("/form/focus_var", json!("editing")),
            ("/form/fields/0/id", json!("bad id")),
            ("/form/fields/1/id", json!("selection")),
            ("/form/fields/0/var", json!("focus")),
            ("/form/fields/0/draft_var", json!("choice")),
            ("/vars/focus/init", json!(1)),
            ("/vars/editing/init", json!(true)),
            ("/vars/choice_draft/init", json!(5)),
            ("/vars/choice/init", json!(3)),
            ("/form/fields/1/step", json!(0)),
            ("/form/fields/1/max", json!(19)),
            ("/form/fields/1/min", json!(21)),
            ("/form/fields/1/min", json!(i64::MIN)),
            ("/form/fields/1/max", json!(i64::MAX)),
            ("/form/fields/1/step", json!(1.0)),
            ("/form/fields/0/options/1/value", json!(2)),
            ("/form/fields/0/options/1/value", json!(true)),
            ("/form/fields/0/options/0/label", json!("")),
            ("/form/fields/0/options/0/label", json!("字".repeat(22))),
            ("/form/fields/0/options/0/label", json!("bad\nline")),
            ("/form/actions/accept", json!("edit.cancel")),
            ("/form/actions/previous", json!("host.next")),
            ("/form/actions/previous", json!("input.next")),
            ("/form/actions/previous", json!("page_back")),
            ("/transitions/0/from", json!("ready")),
            ("/form/fields", json!([])),
            ("/surface/elements/0/field_bind", json!("unknown")),
            ("/surface/elements/0/type", json!("meter")),
            ("/input/repeat", json!(["edit.accept"])),
            ("/surface/bindings", json!("future")),
        ] {
            let mut bad = runtime.clone();
            *bad.pointer_mut(pointer).unwrap() = value;
            assert!(!valid(&bad), "accepted {pointer}: {bad}");
        }
        for key in [
            "host",
            "local",
            "scene",
            "game",
            "data",
            "media",
            "instrument",
        ] {
            let mut bad = runtime.clone();
            bad[key] = Value::Null;
            assert!(!valid(&bad), "accepted profile {key}");
        }
        for pointer in [
            "/form",
            "/form/actions",
            "/form/fields/0",
            "/form/fields/0/options/0",
        ] {
            let mut bad = runtime.clone();
            bad.pointer_mut(pointer).unwrap()["unknown"] = json!(true);
            assert!(!valid(&bad), "accepted extra key {pointer}");
        }
        for key in ["text", "bind", "var_bind", "key_for"] {
            let mut bad = runtime.clone();
            bad["surface"]["elements"][0][key] = json!("choice");
            assert!(!valid(&bad), "accepted binding conflict {key}");
        }
        for key in ["confirm", "cooldown"] {
            let mut bad = runtime.clone();
            bad["input"][key] = json!({"edit.accept":500});
            assert!(!valid(&bad));
        }
        let mut bad = runtime.clone();
        bad["input"]["hold"] = json!("edit.accept");
        assert!(!valid(&bad));
        let mut bad = runtime.clone();
        bad["input"]["press"] = json!(["edit.accept"]);
        assert!(!valid(&bad));
        let mut bad = runtime.clone();
        bad["transitions"][0]["set"] = json!({"number":15});
        assert!(!valid(&bad));
        let mut bad = runtime.clone();
        bad.as_object_mut().unwrap().remove("form");
        assert!(!valid(&bad));
        let mut bad = runtime.clone();
        let t = bad["transitions"][0].clone();
        bad["transitions"].as_array_mut().unwrap().push(t);
        assert!(!valid(&bad));
        let mut good = runtime;
        good["form"]["fields"][0]["options"][0]["label"] = json!("\u{0080}");
        assert!(valid(&good));
        good["form"]["fields"][0]["options"][0]["label"] = json!("字".repeat(21));
        assert!(valid(&good));
        good["surface"]["elements"][0]["page"] = json!(4);
        assert!(valid(&good));
    }

    #[test]
    fn app_form_limits_persistence_and_external_writes_preserve_draft_ownership() {
        let (runtime, _) = fixture();
        let mut good = runtime.clone();
        good["limits"] = json!({"focus":[0,1],"editing":[0,1],"choice":[2,5],"choice_draft":[2,5],"number":[0,20],"number_draft":[0,20]});
        good["persist"] =
            json!({"version":1,"vars":["choice","number"],"states":{"ready":"ready"}});
        assert!(valid(&good));
        for name in ["focus", "editing", "choice_draft", "number_draft"] {
            let mut bad = good.clone();
            bad["persist"]["vars"]
                .as_array_mut()
                .unwrap()
                .push(json!(name));
            assert!(!valid(&bad));
            let mut bad = runtime.clone();
            bad["transitions"]
                .as_array_mut()
                .unwrap()
                .push(json!({"from":"*","on":"other","set":{name:0}}));
            assert!(!valid(&bad));
        }
        for name in [
            "focus",
            "editing",
            "choice",
            "choice_draft",
            "number",
            "number_draft",
        ] {
            let mut bad = runtime.clone();
            bad["tick"] = json!([{"every_ms":1000,"inc":{name:1}}]);
            assert!(!valid(&bad));
            let mut bad = runtime.clone();
            bad["tick"] = json!([{"every_ms":1000,"then":{"set":{name:0}}}]);
            assert!(!valid(&bad));
        }
        for name in [
            "focus",
            "editing",
            "choice",
            "choice_draft",
            "number",
            "number_draft",
        ] {
            let mut bad = good.clone();
            bad["limits"][name][1] = bad["vars"][name]["init"].clone();
            assert!(!valid(&bad), "narrow {name}");
        }
        // Non-form actions can change committed data; entering the editor will
        // normalize only its draft, never silently replace external stored data.
        let mut external = runtime.clone();
        external["transitions"]
            .as_array_mut()
            .unwrap()
            .push(json!({"from":"*","on":"external","set":{"choice":99,"number":3}}));
        assert!(valid(&external));
        let mut too_many = runtime;
        too_many["vars"]["third"] = json!({"type":"int","init":0});
        too_many["vars"]["third_draft"] = json!({"type":"int","init":0});
        too_many["form"]["fields"].as_array_mut().unwrap().push(json!({"id":"third","kind":"integer","var":"third","draft_var":"third_draft","min":0,"max":1,"step":1}));
        assert!(valid(&too_many));
        too_many["vars"]["ninth"] = json!({"type":"int","init":0});
        assert!(!valid(&too_many));
    }

    #[test]
    fn app_form_physical_remapping_and_old_capability_rejection() {
        let (runtime, buttons) = fixture();
        let mut malformed = runtime.clone();
        malformed["limits"] = json!([]);
        assert!(validate_bindings(&malformed, &buttons).is_err());
        let mut malformed = runtime.clone();
        malformed["surface"]["elements"][0]["x"] = json!(640);
        assert!(validate_bindings(&malformed, &buttons).is_err());
        let mut forbidden = buttons.clone();
        forbidden.as_array_mut().unwrap().push(json!({"action":"page_main","event":"button.encoder.short_press","control":"Control","label":"Global"}));
        assert!(validate_bindings(&runtime, &forbidden).is_err());
        for caps in [
            json!({}),
            json!({"widgetAppForm":true}),
            json!({"widgetAppForm":"p4-app-form-v0"}),
        ] {
            assert!(require_capability(&runtime, &caps).is_err());
            assert!(require_capability(&json!({}), &caps).is_ok());
        }
        assert!(require_capability(&runtime, &json!({"widgetAppForm":ENGINE})).is_ok());
        let mut swapped = buttons.clone();
        swapped[4]["event"] = json!("button.sw2.short_press");
        swapped[5]["event"] = json!("button.sw1.short_press");
        assert!(validate_bindings(&runtime, &swapped).is_ok());
        swapped[4]["event"] = json!("button.encoder.long_press");
        assert!(validate_bindings(&runtime, &swapped).is_ok());
        for event in [
            "button.sw3.short_press",
            "screen.region.tap",
            "screen.region.long_press",
            "unknown",
            "knob.rotate_cw / knob.rotate_ccw",
        ] {
            let mut bad = buttons.clone();
            bad[4]["event"] = json!(event);
            assert!(
                validate_bindings(&runtime, &bad).is_err(),
                "accepted {event}"
            );
        }
        let mut bad = buttons.clone();
        bad.as_array_mut().unwrap().pop();
        assert!(validate_bindings(&runtime, &bad).is_err());
        let mut bad = buttons.clone();
        bad[4]["event"] = bad[5]["event"].clone();
        assert!(validate_bindings(&runtime, &bad).is_err());
        let mut bad = buttons.clone();
        bad[3]["event"] = json!("button.sw1.short_press");
        bad[4]["event"] = json!("knob.rotate_cw");
        assert!(validate_bindings(&runtime, &bad).is_err());
        let mut without_repeat = runtime;
        without_repeat["input"]
            .as_object_mut()
            .unwrap()
            .remove("repeat");
        assert!(validate_bindings(&without_repeat, &bad).is_ok());
    }
}

#[cfg(test)]
mod legacy_quota_boundary_tests {
    use super::*;
    #[test]
    fn quota_v3_does_not_expand_v1_v2_editor_slot_capacity() {
        for text in [include_str!("../../skills/petui/contracts/app-form-v1.json"),include_str!("../../skills/petui/contracts/app-form-v2.json")] {
            let suite:Value=serde_json::from_str(text).unwrap();let base=&suite["runtime"];
            for count in [8usize,9,16,17,32] {
                let mut r=base.clone();r["quota"]="p4-quota-v3".into();
                while r["vars"].as_object().unwrap().len()<count {let n=r["vars"].as_object().unwrap().len();r["vars"][format!("extra{n}")]=serde_json::json!({"type":"int","init":0});}
                assert_eq!(validate_runtime(&r).is_ok(),count==8,"engine={} count={count}",r["form"]["engine"]);
            }
        }
    }
}
