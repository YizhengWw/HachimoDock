//! Bounded desktop hotkey emission for the `hotkeys` host service.
//! Chords come from a package-declared whitelist; sending stays off on each PC until
//! the user enables it in the component details. No text injection, macros or files.
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{io::{Read, Write}, path::Path, sync::Mutex};

pub const MODIFIERS: &[&str] = &["ctrl", "shift", "alt", "cmd"];
const LETTERS: &str = "abcdefghijklmnopqrstuvwxyz";
const DIGITS: &str = "0123456789";
const NAMED: &[&str] = &[
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    "enter", "esc", "tab", "space", "backspace", "delete", "up", "down", "left", "right",
    "home", "end", "pageup", "pagedown", "minus", "equal", "comma", "period", "slash",
    "semicolon", "quote", "backquote", "lbracket", "rbracket", "backslash",
];
pub const MAX_PROFILES: usize = 4;
pub const MAX_KEYS: usize = 6;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Chord {
    pub modifiers: Vec<String>,
    pub key: String,
}

pub fn key_allowed(key: &str) -> bool {
    (key.len() == 1 && (LETTERS.contains(key) || DIGITS.contains(key))) || NAMED.contains(&key)
}

/// `["cmd","shift","4"]`: up to three unique modifiers followed by exactly one key.
pub fn parse(parts: &[String]) -> Result<Chord, String> {
    if parts.is_empty() || parts.len() > 4 {
        return Err("组合键需要 1-4 个部分".into());
    }
    let (mods, key) = parts.split_at(parts.len() - 1);
    let key = key[0].to_ascii_lowercase();
    if !key_allowed(&key) {
        return Err(format!("不支持的按键 {key}"));
    }
    let mut modifiers = Vec::new();
    for m in mods {
        let m = m.to_ascii_lowercase();
        if !MODIFIERS.contains(&m.as_str()) || modifiers.contains(&m) {
            return Err(format!("不支持或重复的修饰键 {m}"));
        }
        modifiers.push(m);
    }
    Ok(Chord { modifiers, key })
}

pub fn parse_value(value: &Value) -> Result<Chord, String> {
    let parts: Vec<String> = value
        .as_array()
        .ok_or("组合键必须是字符串数组")?
        .iter()
        .map(|v| v.as_str().map(str::to_string).ok_or("组合键元素必须是字符串".to_string()))
        .collect::<Result<_, _>>()?;
    parse(&parts)
}

pub fn label(chord: &Chord) -> String {
    let mut out = String::new();
    for m in &chord.modifiers {
        out.push_str(match (m.as_str(), cfg!(target_os = "macos")) {
            ("cmd", true) => "⌘",
            ("cmd", false) => "Win+",
            ("ctrl", true) => "⌃",
            ("ctrl", false) => "Ctrl+",
            ("shift", true) => "⇧",
            ("shift", false) => "Shift+",
            ("alt", true) => "⌥",
            _ => "Alt+",
        });
    }
    out.push_str(&chord.key.to_uppercase());
    out
}

/// Per-component opt-in, persisted next to the service settings. Default off.
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Settings {
    schema_version: u8,
    send_enabled: bool,
}

pub fn send_enabled(dir: &Path) -> Result<bool, String> {
    let file = match std::fs::File::open(dir.join("hotkeys-settings.json")) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(false),
        Err(_) => return Err("无法读取快捷键设置".into()),
    };
    let mut bytes = Vec::new();
    file.take(1025).read_to_end(&mut bytes).map_err(|_| "无法读取快捷键设置")?;
    if bytes.len() > 1024 {
        return Err("快捷键设置损坏".into());
    }
    let s: Settings = serde_json::from_slice(&bytes).map_err(|_| "快捷键设置损坏")?;
    if s.schema_version != 1 {
        return Err("快捷键设置版本不受支持".into());
    }
    Ok(s.send_enabled)
}

pub fn set_send_enabled(dir: &Path, send_enabled: bool) -> Result<(), String> {
    std::fs::create_dir_all(dir).map_err(|_| "无法保存快捷键设置")?;
    let mut file = tempfile::NamedTempFile::new_in(dir).map_err(|_| "无法保存快捷键设置")?;
    let bytes = serde_json::to_vec(&Settings { schema_version: 1, send_enabled }).map_err(|_| "快捷键设置无效")?;
    file.write_all(&bytes).and_then(|_| file.as_file().sync_all()).map_err(|_| "无法保存快捷键设置")?;
    file.persist(dir.join("hotkeys-settings.json")).map_err(|_| "无法保存快捷键设置")?;
    Ok(())
}

pub fn platform() -> &'static str {
    if cfg!(target_os = "macos") { "macos" } else if cfg!(windows) { "windows" } else { "unsupported" }
}

#[cfg(target_os = "macos")]
#[link(name = "ApplicationServices", kind = "framework")]
extern "C" {
    fn AXIsProcessTrusted() -> bool;
}

/// macOS needs the Accessibility grant; Windows needs nothing extra; other
/// platforms cannot send at all. Never prompts: the user grants it in Settings.
pub fn accessibility_ready() -> bool {
    #[cfg(target_os = "macos")]
    {
        unsafe { AXIsProcessTrusted() }
    }
    #[cfg(windows)]
    {
        true
    }
    #[cfg(not(any(target_os = "macos", windows)))]
    {
        false
    }
}

pub fn status(dir: &Path) -> Value {
    serde_json::json!({
        "sendEnabled": send_enabled(dir).unwrap_or(false),
        "platform": platform(),
        "accessibility": accessibility_ready(),
    })
}

type Sender = Box<dyn Fn(&Chord) -> Result<(), String> + Send>;
static SENDER: Mutex<Option<Sender>> = Mutex::new(None);
#[cfg(any(windows, test))]
#[path = "windows_input_release.rs"]
mod windows_input_release;
#[cfg(windows)]
static INPUT_QUARANTINED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Tests install a recorder; production uses the OS path below.
#[cfg(test)]
pub(crate) fn install_sender(sender: Sender) {
    *SENDER.lock().unwrap() = Some(sender);
}

pub fn send(chord: &Chord) -> Result<(), String> {
    if let Some(sender) = SENDER.lock().map_err(|_| "快捷键发送器不可用")?.as_ref() {
        return sender(chord);
    }
    native_send(chord)
}

#[cfg(target_os = "macos")]
fn native_send(chord: &Chord) -> Result<(), String> {
    use core_graphics::event::{CGEvent, CGEventFlags, CGEventTapLocation};
    use core_graphics::event_source::{CGEventSource, CGEventSourceStateID};
    if !accessibility_ready() {
        return Err("macOS 未授予辅助功能权限，请在 系统设置 › 隐私与安全性 › 辅助功能 中允许 Pet Manager".into());
    }
    let code = mac_keycode(&chord.key).ok_or("此按键在 macOS 上不可用")?;
    let mut flags = CGEventFlags::CGEventFlagNull;
    for m in &chord.modifiers {
        flags |= match m.as_str() {
            "cmd" => CGEventFlags::CGEventFlagCommand,
            "ctrl" => CGEventFlags::CGEventFlagControl,
            "shift" => CGEventFlags::CGEventFlagShift,
            _ => CGEventFlags::CGEventFlagAlternate,
        };
    }
    let source = CGEventSource::new(CGEventSourceStateID::HIDSystemState).map_err(|_| "无法创建键盘事件源")?;
    for down in [true, false] {
        let event = CGEvent::new_keyboard_event(source.clone(), code, down).map_err(|_| "无法创建键盘事件")?;
        event.set_flags(flags);
        event.post(CGEventTapLocation::HID);
    }
    Ok(())
}

#[cfg(target_os = "macos")]
fn mac_keycode(key: &str) -> Option<u16> {
    Some(match key {
        "a" => 0, "s" => 1, "d" => 2, "f" => 3, "h" => 4, "g" => 5, "z" => 6, "x" => 7, "c" => 8, "v" => 9,
        "b" => 11, "q" => 12, "w" => 13, "e" => 14, "r" => 15, "y" => 16, "t" => 17, "1" => 18, "2" => 19,
        "3" => 20, "4" => 21, "6" => 22, "5" => 23, "equal" => 24, "9" => 25, "7" => 26, "minus" => 27,
        "8" => 28, "0" => 29, "rbracket" => 30, "o" => 31, "u" => 32, "lbracket" => 33, "i" => 34, "p" => 35,
        "enter" => 36, "l" => 37, "j" => 38, "quote" => 39, "k" => 40, "semicolon" => 41, "backslash" => 42,
        "comma" => 43, "slash" => 44, "n" => 45, "m" => 46, "period" => 47, "tab" => 48, "space" => 49,
        "backquote" => 50, "backspace" => 51, "esc" => 53, "f5" => 96, "f6" => 97, "f7" => 98, "f3" => 99,
        "f8" => 100, "f9" => 101, "f11" => 103, "f10" => 109, "f12" => 111, "home" => 115, "pageup" => 116,
        "delete" => 117, "f4" => 118, "end" => 119, "f2" => 120, "pagedown" => 121, "f1" => 122,
        "left" => 123, "right" => 124, "down" => 125, "up" => 126,
        _ => return None,
    })
}

#[cfg(windows)]
fn native_send(chord: &Chord) -> Result<(), String> {
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{GetAsyncKeyState, SendInput, INPUT, INPUT_KEYBOARD, KEYBDINPUT, KEYEVENTF_KEYUP};
    use windows_input_release::{Event, Transport, Failure};
    use std::sync::atomic::Ordering;
    static INPUT_GATE: Mutex<()> = Mutex::new(());
    let _gate=INPUT_GATE.lock().map_err(|_| "快捷键输入状态不可用")?;
    if INPUT_QUARANTINED.load(Ordering::SeqCst) { return Err("上次键盘输入结果未确认，已停止继续注入；请检查键态后重启客户端".into()); }
    let key = win_vk(&chord.key).ok_or("此按键在 Windows 上不可用")?;
    let mut codes: Vec<u16> = chord.modifiers.iter().map(|m| match m.as_str() { "cmd" => 0x5B, "ctrl" => 0x11, "shift" => 0x10, _ => 0x12 }).collect();
    codes.push(key);
    let events: Vec<Event> = codes.iter().map(|v|Event{key:*v,up:false})
        .chain(codes.iter().rev().map(|v|Event{key:*v,up:true})).collect();
    struct Os;
    impl Transport for Os {
        fn held(&mut self,key:u16)->bool { unsafe{GetAsyncKeyState(i32::from(key))<0} }
        fn insert(&mut self,events:&[Event])->usize {
            let mut inputs=Vec::with_capacity(events.len());
            for e in events {
                let v=e.key;
                inputs.push(INPUT{r#type:INPUT_KEYBOARD,Anonymous:windows_sys::Win32::UI::Input::KeyboardAndMouse::INPUT_0{
                    ki:KEYBDINPUT{wVk:v,wScan:0,dwFlags:if e.up{KEYEVENTF_KEYUP}else{0},time:0,dwExtraInfo:0}}});
            }
            unsafe{SendInput(inputs.len() as u32,inputs.as_ptr(),std::mem::size_of::<INPUT>() as i32) as usize}
        }
    }
    match windows_input_release::dispatch(&events,&mut Os) {
        Ok(())=>Ok(()),
        Err(Failure::Held)=>Err("请先松开正在按住的按键，再触发快捷键；本次未注入".into()),
        Err(Failure::Invalid)=>Err("快捷键事件声明无效，本次未注入".into()),
        Err(Failure::Unknown{accepted,requested,release_needed,release_acked})=>{
            INPUT_QUARANTINED.store(true,Ordering::SeqCst);
            Err(format!("Windows 输入结果未确认，未重发（插入{accepted}/{requested}，松键确认{release_acked}/{release_needed}）；请检查键态后重启客户端"))
        }
    }
}

#[cfg(windows)]
fn win_vk(key: &str) -> Option<u16> {
    if key.len() == 1 {
        let c = key.as_bytes()[0];
        return Some(if c.is_ascii_digit() { c as u16 } else { c.to_ascii_uppercase() as u16 });
    }
    Some(match key {
        "f1" => 0x70, "f2" => 0x71, "f3" => 0x72, "f4" => 0x73, "f5" => 0x74, "f6" => 0x75, "f7" => 0x76, "f8" => 0x77,
        "f9" => 0x78, "f10" => 0x79, "f11" => 0x7A, "f12" => 0x7B, "enter" => 0x0D, "esc" => 0x1B, "tab" => 0x09,
        "space" => 0x20, "backspace" => 0x08, "delete" => 0x2E, "up" => 0x26, "down" => 0x28, "left" => 0x25,
        "right" => 0x27, "home" => 0x24, "end" => 0x23, "pageup" => 0x21, "pagedown" => 0x22, "minus" => 0xBD,
        "equal" => 0xBB, "comma" => 0xBC, "period" => 0xBE, "slash" => 0xBF, "semicolon" => 0xBA, "quote" => 0xDE,
        "backquote" => 0xC0, "lbracket" => 0xDB, "rbracket" => 0xDD, "backslash" => 0xDC,
        _ => return None,
    })
}

#[cfg(not(any(target_os = "macos", windows)))]
fn native_send(_: &Chord) -> Result<(), String> {
    Err("此平台尚不支持发送快捷键".into())
}

/// Package content for the `hotkeys` service: 1–4 profiles, each 1–6 chords.
pub fn validate_profiles(content: &[Value]) -> Result<(), String> {
    let bad = || "快捷键场景声明无效".to_string();
    if content.is_empty() || content.len() > MAX_PROFILES { return Err(bad()); }
    let mut ids = std::collections::HashSet::new();
    let text = |v: &Value, n: usize| v.as_str().is_some_and(|s| !s.trim().is_empty() && s.len() <= n && !s.chars().any(char::is_control));
    for p in content {
        let o = p.as_object().ok_or_else(bad)?;
        if o.len() != 3 || !o.contains_key("id") || !o.contains_key("title") || !o.contains_key("keys") { return Err(bad()); }
        let id = p["id"].as_str().filter(|s| !s.is_empty() && s.len() <= 24 && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')).ok_or_else(bad)?;
        if !ids.insert(id) || !text(&p["title"], 30) { return Err(bad()); }
        let keys = p["keys"].as_array().ok_or_else(bad)?;
        if keys.is_empty() || keys.len() > MAX_KEYS { return Err(bad()); }
        for k in keys {
            let o = k.as_object().ok_or_else(bad)?;
            if o.len() != 2 || !text(&k["label"], 30) { return Err(bad()); }
            parse_value(&k["chord"])?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn chords_follow_the_whitelist() {
        assert_eq!(parse(&["cmd".into(), "c".into()]).unwrap(), Chord { modifiers: vec!["cmd".into()], key: "c".into() });
        assert!(parse(&["cmd".into(), "cmd".into(), "c".into()]).is_err());
        assert!(parse(&["ctrl".into(), "alt".into(), "delete".into()]).is_ok());
        assert!(parse(&["super".into(), "c".into()]).is_err());
        assert!(parse(&["ctrl".into(), "ß".into()]).is_err());
        assert!(parse(&["ctrl".into(), "shift".into(), "alt".into(), "cmd".into(), "c".into()]).is_err());
        assert!(parse(&[]).is_err());
    }
    #[test]
    fn profiles_are_bounded_and_unique() {
        let ok = vec![json!({"id":"common","title":"常用","keys":[{"label":"复制","chord":["cmd","c"]}]})];
        assert!(validate_profiles(&ok).is_ok());
        let mut dup = ok.clone(); dup.push(ok[0].clone()); assert!(validate_profiles(&dup).is_err());
        let bad = vec![json!({"id":"common","title":"常用","keys":[{"label":"宏","chord":["cmd","c","v"]}]})];
        assert!(validate_profiles(&bad).is_err());
        let many = vec![json!({"id":"x","title":"x","keys":(0..7).map(|_|json!({"label":"k","chord":["a"]})).collect::<Vec<_>>()})];
        assert!(validate_profiles(&many).is_err());
    }
    #[test]
    fn sending_is_off_until_enabled_and_settings_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        assert!(!send_enabled(dir.path()).unwrap());
        set_send_enabled(dir.path(), true).unwrap();
        assert!(send_enabled(dir.path()).unwrap());
        assert_eq!(status(dir.path())["sendEnabled"], true);
        std::fs::write(dir.path().join("hotkeys-settings.json"), b"{\"schemaVersion\":2,\"sendEnabled\":true}").unwrap();
        assert!(send_enabled(dir.path()).is_err());
    }
}
