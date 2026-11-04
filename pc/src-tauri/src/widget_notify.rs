//! p4-notify-v1: bounded component notifications on the shared `widget/notify`
//! topic. The PC shows a client toast (Tauri event `component-notify`) and, when
//! the device advertises `widgetNotify`, forwards the same payload so the device
//! shows it in the main-page bubble. One notification per component every 30 s,
//! in both directions, with the same limiter semantics as the firmware header.
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};
use tauri::Emitter;
pub const ENGINE: &str = "p4-notify-v1";
pub const CAPABILITY: &str = "widgetNotify";
pub const TOPIC: &str = "widget/notify";
pub const EVENT: &str = "component-notify";
pub const COOLDOWN_MS: u64 = 30_000;
const SLOTS: usize = 8;
fn bad() -> String { "通知需要 p4-notify-v1：runtime.notify 的 title ≤63 字节、可选整数 badge_var，transition/tick.then 只能写 notify:true".into() }
fn text_ok(s: &str, max: usize, nonempty: bool) -> bool { (!nonempty || !s.is_empty()) && s.len() <= max && !s.chars().any(|c| c.is_control()) }
pub fn component_ok(s: &str) -> bool { (1..=47).contains(&s.len()) && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"_-".contains(&b)) }
/// Exact payload: componentId, title, optional text, optional badge (int).
pub fn valid_payload(p: &Value) -> bool {
    let Some(o) = p.as_object() else { return false; };
    if !o.contains_key("componentId") || !o.contains_key("title") || !o.keys().all(|k| ["componentId","title","text","badge"].contains(&k.as_str())) { return false; }
    o["componentId"].as_str().is_some_and(component_ok) && o["title"].as_str().is_some_and(|s| text_ok(s, 63, true))
        && o.get("text").is_none_or(|t| t.as_str().is_some_and(|s| text_ok(s, 120, false)))
        && o.get("badge").is_none_or(|b| b.as_i64().is_some_and(|n| (-1_000_000_000..=1_000_000_000).contains(&n)))
}
pub fn bubble_title(p: &Value) -> String {
    match p.get("badge").and_then(Value::as_i64) { Some(b) => format!("{} · {b}", p["title"].as_str().unwrap_or("")), None => p["title"].as_str().unwrap_or("").to_string() }
}
/// Package declaration: runtime.notify {title, badge_var?}; rules may carry notify:true.
pub fn declared(runtime: &Value) -> bool {
    runtime.get("notify").is_some() || rules(runtime).iter().any(|r| r.get("notify").is_some())
}
fn rules(runtime: &Value) -> Vec<&Value> {
    runtime["transitions"].as_array().into_iter().flatten().chain(runtime["tick"].as_array().into_iter().flatten().filter_map(|t| t.get("then"))).collect()
}
pub fn validate(runtime: &Value) -> Result<(), String> {
    if !declared(runtime) { return Ok(()); }
    let n = runtime["notify"].as_object().filter(|o| o.contains_key("title") && o.keys().all(|k| ["title","badge_var"].contains(&k.as_str()))).ok_or_else(bad)?;
    if !n["title"].as_str().is_some_and(|s| text_ok(s, 63, true)) { return Err(bad()); }
    if let Some(v) = n.get("badge_var") { if !v.as_str().is_some_and(|name| runtime["vars"][name]["type"] == "int") { return Err(bad()); } }
    if rules(runtime).iter().any(|r| r.get("notify").is_some_and(|v| v != true)) { return Err(bad()); }
    Ok(())
}
pub fn require_capability(runtime: &Value, capabilities: &Value) -> Result<(), String> {
    if declared(runtime) && capabilities[CAPABILITY] != ENGINE { return Err("需要组件通知能力 p4-notify-v1，请先升级固件；未安装不兼容资源".into()); }
    Ok(())
}
#[derive(Default)]
pub struct Limiter { slots: HashMap<String, u64> }
impl Limiter {
    pub fn admit(&mut self, component: &str, now_ms: u64) -> bool {
        if let Some(last) = self.slots.get(component) { if now_ms >= *last && now_ms - last < COOLDOWN_MS { return false; } }
        if !self.slots.contains_key(component) && self.slots.len() >= SLOTS {
            if let Some(oldest) = self.slots.iter().min_by_key(|(_, t)| **t).map(|(k, _)| k.clone()) { self.slots.remove(&oldest); }
        }
        self.slots.insert(component.into(), now_ms); true
    }
}
fn limiter() -> &'static Mutex<Limiter> { static L: OnceLock<Mutex<Limiter>> = OnceLock::new(); L.get_or_init(Default::default) }
fn now_ms() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0) }
pub struct Delivery { pub toast: bool, pub device: Result<(), String> }
/// Post one notification from a PC service or a device event: toast for the client,
/// `widget/notify` to a device that advertises the capability. Rate limited per component.
pub fn post(app: Option<&tauri::AppHandle>, usb: Option<&crate::usb_serial::UsbSerialManager>, payload: &Value, from_device: bool) -> Result<Delivery, String> {
    if !valid_payload(payload) { return Err("通知负载无效".into()); }
    let component = payload["componentId"].as_str().unwrap();
    if !limiter().lock().map_err(|_| "通知限流不可用")?.admit(component, now_ms()) { return Err("通知过于频繁：每个组件 30 秒最多一条".into()); }
    let toast = app.map(|a| a.emit(EVENT, json!({"componentId":component,"title":payload["title"],"text":payload.get("text").cloned().unwrap_or(Value::Null),"badge":payload.get("badge").cloned().unwrap_or(Value::Null),"bubble":bubble_title(payload),"fromDevice":from_device})).is_ok()).unwrap_or(false);
    let device = match usb {
        Some(usb) if !from_device => {
            let status = usb.status();
            if !status.connected { Err("设备未连接".into()) }
            else if status.capabilities[CAPABILITY] != ENGINE { Err("设备未声明 p4-notify-v1，仅显示客户端提示".into()) }
            else { usb.send(TOPIC, payload) }
        }
        _ => Ok(()),
    };
    Ok(Delivery { toast, device })
}
/// Device -> PC: the firmware posted a component notification; show it as a toast.
pub fn device_event(app: &tauri::AppHandle, payload: &Value) { let _ = post(Some(app), None, payload, true); }
#[cfg(test)]
mod tests {
    use super::*;
    fn suite() -> Value { serde_json::from_str(include_str!("../../skills/petui/contracts/notify-v1.json")).unwrap() }
    #[test]
    fn notify_payloads_runtime_and_limiter_follow_the_shared_contract() {
        let s = suite();
        for p in s["payloads"].as_array().unwrap() { assert!(valid_payload(p), "{p}"); }
        for p in s["rejectedPayloads"].as_array().unwrap() { assert!(!valid_payload(p), "{p}"); }
        for c in s["bubble"].as_array().unwrap() { assert_eq!(bubble_title(&c["payload"]), c["title"].as_str().unwrap()); }
        let mut l = Limiter::default();
        for c in s["limiter"].as_array().unwrap() { assert_eq!(l.admit(c["component"].as_str().unwrap(), c["now_ms"].as_u64().unwrap()), c["admit"] == true, "{c}"); }
        let r = &s["runtime"]; validate(r).unwrap();
        for case in s["invalid"].as_array().unwrap() {
            let mut bad = r.clone(); let path = case["path"].as_array().unwrap(); let mut parent = &mut bad;
            for key in &path[..path.len() - 1] { parent = if let Some(k) = key.as_str() { &mut parent[k] } else { &mut parent[key.as_u64().unwrap() as usize] }; }
            let key = path.last().unwrap(); if let Some(k) = key.as_str() { parent[k] = case["value"].clone(); } else { parent[key.as_u64().unwrap() as usize] = case["value"].clone(); }
            assert!(validate(&bad).is_err(), "{case}");
        }
        assert!(require_capability(r, &json!({})).is_err()); require_capability(r, &json!({"widgetNotify":ENGINE})).unwrap();
        assert!(post(None, None, &json!({"componentId":"Bad","title":"x"}), true).is_err());
        let first = post(None, None, &json!({"componentId":"limiter-test-component","title":"x"}), true).unwrap(); assert!(!first.toast && first.device.is_ok());
        assert!(post(None, None, &json!({"componentId":"limiter-test-component","title":"x"}), true).is_err());
    }
}
