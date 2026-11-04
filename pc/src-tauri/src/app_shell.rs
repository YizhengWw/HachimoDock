//! p4-app-shell-v1: pages 0..11 behind quota v3, a bounded tab bar expanded into
//! standard button nodes and the reserved tab.next / tab.prev actions. Data only;
//! no component code, no renderer branch.
use serde_json::Value;
pub const ENGINE: &str = "p4-app-shell-v1";
pub const CAPABILITY: &str = "widgetAppShell";
pub const PAGES: usize = 12;
pub const RESERVED: [&str; 2] = ["tab.next", "tab.prev"];
const ICONS: [&str; 20] = ["home","leaf","desk","agent","compass","sun","mic","book","sound","spark","heart","pause","play","check","plus","list","arrow","cloud","clock","stretch"];
fn bad() -> String { "应用壳需要 p4-app-shell-v1、quota v3、通用绑定、2–5 个标签页与 tab.next/tab.prev 保留动作".into() }
fn ident(s: &str, max: usize) -> bool { !s.is_empty() && s.len() <= max && s.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)) }
fn u(v: &Value, lo: u64, hi: u64) -> Option<u64> { v.as_u64().filter(|n| (lo..=hi).contains(n)) }
pub fn declared(runtime: &Value) -> bool { runtime["surface"].get("shell").is_some() || runtime["surface"].get("tabs").is_some() }
pub fn require_capability(runtime: &Value, capabilities: &Value) -> Result<(), String> {
    if declared(runtime) && capabilities[CAPABILITY] != ENGINE { return Err("需要应用壳能力 p4-app-shell-v1，请先升级固件；未安装不兼容资源".into()); }
    Ok(())
}
/// Strict tab bar: 2–5 items on distinct declared pages, int var, bar inside 640×480,
/// no overlap with touch targets, reserved actions declared as effect-free wildcards.
pub fn validate_tabs(tabs: &Value, surface: &Value, runtime: Option<&Value>) -> Result<(), String> {
    let o = tabs.as_object().filter(|o| o.len() == 7 && ["items","var","x","y","w","h","colors"].iter().all(|k| o.contains_key(*k))).ok_or_else(bad)?;
    let items = o["items"].as_array().filter(|a| (2..=5).contains(&a.len())).ok_or_else(bad)?;
    let var = o["var"].as_str().filter(|s| ident(s, 31)).ok_or_else(bad)?;
    let x = u(&o["x"], 0, 639).ok_or_else(bad)?; let y = u(&o["y"], 0, 479).ok_or_else(bad)?;
    let w = u(&o["w"], 64 * items.len() as u64, 640 - x).ok_or_else(bad)?; let h = u(&o["h"], 32, 96).ok_or_else(bad)?;
    if y + h > 480 { return Err(bad()); }
    let colors = o["colors"].as_array().filter(|a| a.len() == 4 && a.iter().all(|c| u(c, 0, 65535).is_some())).ok_or_else(bad)?; let _ = colors;
    let mut pages = std::collections::HashSet::new();
    for item in items {
        let io = item.as_object().filter(|m| m.contains_key("label") && m.contains_key("page") && m.keys().all(|k| ["label","page","icon"].contains(&k.as_str()))).ok_or_else(bad)?;
        let label = io["label"].as_str().filter(|s| !s.is_empty() && s.len() <= 24 && !s.chars().any(char::is_control)).ok_or_else(bad)?; let _ = label;
        if let Some(icon) = io.get("icon") { if !icon.as_str().is_some_and(|i| ICONS.contains(&i)) { return Err(bad()); } }
        let page = u(&io["page"], 0, PAGES as u64 - 1).ok_or_else(bad)?;
        if !pages.insert(page) { return Err(bad()); }
    }
    if surface["shell"] != ENGINE || surface["bindings"] != "p4-variable-bindings-v1" || ["game_view","scene_view","input"].iter().any(|k| surface.get(*k).is_some()) { return Err(bad()); }
    for n in surface["elements"].as_array().into_iter().flatten() {
        if n.get("action").is_none() || n.get("view").is_some() || n.get("in_view").is_some() { continue; }
        let (nx, ny, nw, nh) = (n["x"].as_u64().unwrap_or(0), n["y"].as_u64().unwrap_or(0), n["w"].as_u64().unwrap_or(0), n["h"].as_u64().unwrap_or(0));
        if x < nx + nw && x + w > nx && y < ny + nh && y + h > ny { return Err(bad()); }
    }
    if let Some(r) = runtime {
        let count = r["pages"].as_array().map_or(1, |p| p.len().max(1)) as u64;
        if pages.iter().any(|p| *p >= count) || r["vars"][var]["type"] != "int" { return Err(bad()); }
        let transitions = r["transitions"].as_array().ok_or_else(bad)?;
        for action in RESERVED {
            let matching: Vec<_> = transitions.iter().filter(|t| t["on"] == action).collect();
            if matching.is_empty() || matching.iter().any(|t| t.as_object().is_none_or(|o| o.len() != 2) || t["from"] != "*") { return Err(bad()); }
        }
    }
    Ok(())
}
/// Shell-wide rules for a declared package (surface.shell and/or surface.tabs).
pub fn validate(runtime: &Value) -> Result<(), String> {
    if !declared(runtime) { return Ok(()); }
    let s = &runtime["surface"];
    if s["shell"] != ENGINE || s["graphics"] != "p4-app-graphics-v1" || s["bindings"] != "p4-variable-bindings-v1" || runtime["quota"] != "p4-quota-v3"
        || s.get("canvas").is_some()
        || !runtime["pages"].as_array().is_some_and(|p| (1..=PAGES).contains(&p.len()))
        || ["media","instrument","game","scene"].iter().any(|k| runtime.get(*k).is_some()) { return Err(bad()); }
    if let Some(tabs) = s.get("tabs") { validate_tabs(tabs, s, Some(runtime))?; }
    Ok(())
}
/// Reserved action -> Some(next?) ; None for ordinary actions.
pub fn tab_operation(action: &str) -> Option<bool> { match action { "tab.next" => Some(true), "tab.prev" => Some(false), _ => None } }
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn suite() -> Value { serde_json::from_str(include_str!("../../skills/petui/contracts/app-shell-v1.json")).unwrap() }
    fn patched(value: &Value, case: &Value) -> Value {
        let mut out = value.clone(); let path = case["path"].as_array().unwrap(); let mut parent = &mut out;
        for key in &path[..path.len() - 1] { parent = if let Some(s) = key.as_str() { &mut parent[s] } else { &mut parent[key.as_u64().unwrap() as usize] }; }
        let key = path.last().unwrap(); if let Some(s) = key.as_str() { parent[s] = case["value"].clone(); } else { parent[key.as_u64().unwrap() as usize] = case["value"].clone(); } out
    }
    fn whole(r: &Value) -> Result<(), String> {
        validate(r)?; crate::component_extension::validate_app_runtime(r)?; crate::component_extension::validate_app_surface(&r["surface"], Some(r))
    }
    #[test]
    fn shared_shell_contract_accepts_runtime_and_rejects_every_patch() {
        let s = suite(); let r = &s["runtime"];
        whole(r).unwrap();
        for case in s["invalid"].as_array().unwrap() { assert!(whole(&patched(r, case)).is_err(), "{case}"); }
        assert!(require_capability(r, &json!({})).is_err());
        assert!(require_capability(r, &json!({"widgetAppShell":"p4-app-shell-v2"})).is_err());
        require_capability(r, &json!({"widgetAppShell":ENGINE})).unwrap();
        require_capability(&json!({"surface":{}}), &json!({})).unwrap();
        assert_eq!(tab_operation("tab.next"), Some(true)); assert_eq!(tab_operation("hit"), None);
    }
}
