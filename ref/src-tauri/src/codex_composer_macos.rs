/*
 * [Input] A unique or current-visible ChatGPT（Codex）/Claude task, or a captured MiMoCode terminal caret, plus device speech text.
 * [Output] Native consent with activation-ordered Accessibility-pane routing,
 *          activation-gated Chromium AX priming, AX-only stable/rebindable
 *          exact-session visible-composer submission with intentional draft
 *          replacement, plus ID-deeplink-confirmed visible-composer recovery
 *          when a Codex build omits both the active title and sidebar row,
 *          and clipboard-preserving current-caret insertion plus Return.
 * [Pos] macOS foreground-input backend for codex_composer.rs; requests Apple's native consent alert first and routes the activated System Settings window only after the user chooses to open it.
 * [Sync] If this file changes, update ref/.folder.md.
 */

use accessibility::{AXAttribute, AXUIElement, AXUIElementActions, AXUIElementAttributes};
use accessibility_sys::{
    error_string, kAXErrorSuccess, kAXTrustedCheckOptionPrompt, kAXValueTypeCGPoint,
    kAXValueTypeCGSize, AXIsProcessTrusted, AXIsProcessTrustedWithOptions, AXUIElementGetPid,
    AXUIElementPostKeyboardEvent, AXUIElementRef, AXValueGetType, AXValueGetTypeID,
    AXValueGetValue, AXValueRef,
};
#[cfg(debug_assertions)]
use core_foundation::number::CFNumber;
use core_foundation::{
    array::CFArray,
    base::{CFType, TCFType},
    boolean::CFBoolean,
    dictionary::CFDictionary,
    string::CFString,
};
use core_graphics::{
    event::{CGEvent, CGEventFlags, CGEventTapLocation, KeyCode},
    event_source::{CGEventSource, CGEventSourceStateID},
    geometry::{CGPoint, CGSize},
};
use dispatch2::run_on_main;
use objc2::runtime::ProtocolObject;
use objc2_app_kit::{
    NSApplicationActivationOptions, NSPasteboard, NSPasteboardItem, NSPasteboardTypeString,
    NSPasteboardWriting, NSRunningApplication,
};
use objc2_foundation::{NSArray, NSData, NSString};
use std::{
    collections::HashSet,
    ffi::c_void,
    fs,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, Ordering},
        Mutex, OnceLock,
    },
    thread,
    time::{Duration, Instant, SystemTime},
};

const MAX_TREE_ELEMENTS: usize = 12_000;
const MAX_TREE_DEPTH: usize = 64;
const MAX_ANCESTOR_DEPTH: usize = 20;
const SESSION_CONFIRM_TIMEOUT: Duration = Duration::from_millis(2_500);
const COMPOSER_READBACK_TIMEOUT: Duration = Duration::from_millis(500);
const SUBMIT_CONFIRM_TIMEOUT: Duration = Duration::from_millis(3_000);
const DEEPLINK_FOCUS_DELAY: Duration = Duration::from_millis(450);
const KEYBOARD_READBACK_DELAY: Duration = Duration::from_millis(80);
const ACCESSIBILITY_TREE_READY_TIMEOUT: Duration = Duration::from_millis(750);
const ACCESSIBILITY_TREE_RETRY_DELAY: Duration = Duration::from_millis(25);
const AGENT_LAUNCH_WINDOW_TIMEOUT: Duration = Duration::from_millis(4_500);
const AGENT_FRONTMOST_TIMEOUT: Duration = Duration::from_millis(2_500);
const COMPOSER_STABILITY_TIMEOUT: Duration = Duration::from_millis(2_500);
const COMPOSER_STABILITY_DELAY: Duration = Duration::from_millis(120);
const ACCESSIBILITY_SETTINGS_URL: &str =
    "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_Accessibility";
const ACCESSIBILITY_SETTINGS_WATCH_TIMEOUT: Duration = Duration::from_secs(300);
const ACCESSIBILITY_SETTINGS_WATCH_INTERVAL: Duration = Duration::from_millis(100);
const ACCESSIBILITY_SETTINGS_ROUTE_DELAY: Duration = Duration::from_secs(4);
const MAC_KEYCODE_V: u16 = 9;

#[derive(Clone)]
struct CodexWindow {
    app: AXUIElement,
    window: AXUIElement,
}

struct SessionRow {
    app: AXUIElement,
    window: AXUIElement,
    target: AXUIElement,
    workspace_matches: bool,
}

#[derive(Clone)]
struct ComposerTarget {
    agent: MacosAgent,
    app: AXUIElement,
    window: AXUIElement,
    composer: AXUIElement,
    value: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) enum MacosAgent {
    Codex,
    Claude,
}

impl MacosAgent {
    fn label(self) -> &'static str {
        match self {
            Self::Codex => "ChatGPT（Codex）",
            Self::Claude => "Claude",
        }
    }
}

fn agent_bundle_identifiers(agent: MacosAgent) -> &'static [&'static str] {
    match agent {
        MacosAgent::Codex => &["com.openai.codex", "com.openai.chat", "com.openai.chatgpt"],
        MacosAgent::Claude => &["com.anthropic.claudefordesktop", "com.anthropic.claude"],
    }
}

fn agent_application_names(agent: MacosAgent) -> &'static [&'static str] {
    match agent {
        MacosAgent::Codex => &["Codex", "ChatGPT"],
        MacosAgent::Claude => &["Claude"],
    }
}

pub(super) struct MacosComposerState {
    agent: MacosAgent,
    session_id: String,
    session_title: String,
    last_value: String,
    current_visible_target: Option<ComposerTarget>,
}

#[derive(Clone, Debug)]
pub(super) struct FocusedTextTarget {
    pid: i32,
    role: String,
    subrole: String,
    identifier: String,
    window_title: String,
    element_bounds: Option<(i64, i64, i64, i64)>,
    window_bounds: Option<(i64, i64, i64, i64)>,
}

enum PasteboardField {
    Data { data_type: String, bytes: Vec<u8> },
    Text { data_type: String, value: String },
}

impl PasteboardField {
    fn data_type(&self) -> &str {
        match self {
            Self::Data { data_type, .. } | Self::Text { data_type, .. } => data_type,
        }
    }
}

#[derive(Default)]
struct PasteboardSnapshot(Vec<Vec<PasteboardField>>);

fn is_equivalent_plain_text_type(data_type: &str) -> bool {
    matches!(
        data_type,
        "public.utf16-external-plain-text"
            | "public.utf8-plain-text"
            | "public.plain-text"
            | "public.text"
            | "NSStringPboardType"
            | "NeXT plain ascii pasteboard type"
    )
}

fn normalize_text(value: &str) -> String {
    value.split_whitespace().collect::<Vec<_>>().join(" ")
}

pub(super) fn accessibility_permission_granted() -> bool {
    unsafe { AXIsProcessTrusted() }
}

#[derive(Debug)]
struct SettingsActivationGate {
    observed_inactive: bool,
}

impl SettingsActivationGate {
    fn new(is_active: bool) -> Self {
        Self {
            observed_inactive: !is_active,
        }
    }

    fn update(&mut self, is_active: bool) -> bool {
        if !is_active {
            self.observed_inactive = true;
            return false;
        }
        self.observed_inactive
    }
}

fn system_settings_is_active() -> bool {
    NSRunningApplication::runningApplicationsWithBundleIdentifier(&NSString::from_str(
        "com.apple.systempreferences",
    ))
    .iter()
    .any(|application| application.isActive())
}

fn open_accessibility_settings_pane() -> bool {
    crate::command_for_host("open")
        .arg(ACCESSIBILITY_SETTINGS_URL)
        .status()
        .is_ok_and(|status| status.success())
}

fn arm_accessibility_settings_redirect() {
    static WATCHER_ACTIVE: AtomicBool = AtomicBool::new(false);
    if WATCHER_ACTIVE
        .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
        .is_err()
    {
        return;
    }

    thread::spawn(|| {
        let deadline = Instant::now() + ACCESSIBILITY_SETTINGS_WATCH_TIMEOUT;
        let mut activation_gate = SettingsActivationGate::new(system_settings_is_active());
        loop {
            if accessibility_permission_granted() || Instant::now() >= deadline {
                break;
            }
            if activation_gate.update(system_settings_is_active()) {
                // Apple's consent button activates System Settings asynchronously.
                // A cold Privacy & Security extension can rebuild its navigation
                // stack for roughly two seconds after activation. Route only after
                // that initialization settles; otherwise the correct Accessibility
                // pane flashes briefly and the extension replaces it with its root.
                thread::sleep(ACCESSIBILITY_SETTINGS_ROUTE_DELAY);
                if !accessibility_permission_granted() && !open_accessibility_settings_pane() {
                    eprintln!("[accessibility] failed to route System Settings to Accessibility");
                }
                break;
            }
            thread::sleep(ACCESSIBILITY_SETTINGS_WATCH_INTERVAL);
        }
        WATCHER_ACTIVE.store(false, Ordering::SeqCst);
    });
}

pub(super) fn request_accessibility_permission() -> bool {
    if accessibility_permission_granted() {
        return true;
    }

    let prompt_key = unsafe { CFString::wrap_under_get_rule(kAXTrustedCheckOptionPrompt) };
    let options = CFDictionary::from_CFType_pairs(&[(prompt_key, CFBoolean::true_value())]);
    let trusted = unsafe { AXIsProcessTrustedWithOptions(options.as_concrete_TypeRef()) };
    if !trusted {
        arm_accessibility_settings_redirect();
    }
    trusted
}

fn ensure_accessibility_permission() -> Result<(), String> {
    if request_accessibility_permission() {
        Ok(())
    } else {
        Err("Pet Manager 需要 macOS 辅助功能权限；请在系统授权弹窗中点击“打开系统设置”，打开 Pet Manager 开关后再重试".to_string())
    }
}

impl PasteboardSnapshot {
    fn capture() -> Result<Self, String> {
        run_on_main(|_| {
            let pasteboard = NSPasteboard::generalPasteboard();
            let Some(items) = pasteboard.pasteboardItems() else {
                return Ok(Self::default());
            };
            let mut snapshot = Vec::new();
            for item in items.iter() {
                let mut fields = Vec::new();
                let mut unreadable_types = Vec::new();
                for data_type in item.types().iter() {
                    let data_type_name = data_type.to_string();
                    if let Some(data) = item.dataForType(&data_type) {
                        fields.push(PasteboardField::Data {
                            data_type: data_type_name,
                            bytes: data.to_vec(),
                        });
                    } else if is_equivalent_plain_text_type(&data_type_name) {
                        if let Some(value) = item.stringForType(&data_type) {
                            fields.push(PasteboardField::Text {
                                data_type: data_type_name,
                                value: value.to_string(),
                            });
                        } else {
                            unreadable_types.push(data_type_name);
                        }
                    } else {
                        unreadable_types.push(data_type_name);
                    }
                }

                let has_equivalent_plain_text = fields
                    .iter()
                    .any(|field| is_equivalent_plain_text_type(field.data_type()));
                if let Some(data_type) = unreadable_types.into_iter().find(|data_type| {
                    !is_equivalent_plain_text_type(data_type) || !has_equivalent_plain_text
                }) {
                    return Err(format!(
                        "无法完整备份剪贴板类型 {}，已停止前台 Agent 输入操作",
                        data_type
                    ));
                }
                snapshot.push(fields);
            }
            Ok(Self(snapshot))
        })
    }

    fn restore(self) {
        run_on_main(move |_| {
            let pasteboard = NSPasteboard::generalPasteboard();
            let items = self
                .0
                .into_iter()
                .map(|fields| {
                    let item = NSPasteboardItem::new();
                    for field in fields {
                        match field {
                            PasteboardField::Data { data_type, bytes } => {
                                let data_type = NSString::from_str(&data_type);
                                let data = NSData::with_bytes(&bytes);
                                let _ = item.setData_forType(&data, &data_type);
                            }
                            PasteboardField::Text { data_type, value } => {
                                let data_type = NSString::from_str(&data_type);
                                let value = NSString::from_str(&value);
                                let _ = item.setString_forType(&value, &data_type);
                            }
                        }
                    }
                    ProtocolObject::<dyn NSPasteboardWriting>::from_retained(item)
                })
                .collect::<Vec<_>>();
            let _ = pasteboard.clearContents();
            if !items.is_empty() {
                let items = NSArray::from_retained_slice(&items);
                let _ = pasteboard.writeObjects(&items);
            }
        });
    }
}

fn set_pasteboard_text(text: &str) -> Result<(), String> {
    run_on_main(|_| {
        let pasteboard = NSPasteboard::generalPasteboard();
        let _ = pasteboard.clearContents();
        if pasteboard
            .setString_forType(&NSString::from_str(text), unsafe { NSPasteboardTypeString })
        {
            Ok(())
        } else {
            Err("无法准备 macOS 前台 Agent 输入框安全检查".to_string())
        }
    })
}

fn post_keyboard_event(keycode: u16, flags: CGEventFlags) -> Result<(), String> {
    let source = CGEventSource::new(CGEventSourceStateID::HIDSystemState)
        .map_err(|_| "无法创建 macOS 键盘事件源".to_string())?;
    let down = CGEvent::new_keyboard_event(source.clone(), keycode, true)
        .map_err(|_| "无法创建 macOS 按键按下事件".to_string())?;
    down.set_flags(flags);
    down.post(CGEventTapLocation::HID);
    let up = CGEvent::new_keyboard_event(source, keycode, false)
        .map_err(|_| "无法创建 macOS 按键释放事件".to_string())?;
    up.set_flags(flags);
    up.post(CGEventTapLocation::HID);
    Ok(())
}

fn post_command_key(keycode: u16) -> Result<(), String> {
    post_keyboard_event(keycode, CGEventFlags::CGEventFlagCommand)
}

fn paste_focused_text(text: &str) -> Result<(), String> {
    set_pasteboard_text(text)?;
    post_command_key(MAC_KEYCODE_V)?;
    thread::sleep(KEYBOARD_READBACK_DELAY);
    Ok(())
}

fn ax_element_attribute(element: &AXUIElement, name: &str) -> Option<AXUIElement> {
    let attribute = AXAttribute::new(&CFString::new(name));
    let value: CFType = element.attribute(&attribute).ok()?;
    if value.type_of() != AXUIElement::type_id() {
        return None;
    }
    Some(unsafe { AXUIElement::wrap_under_get_rule(value.as_CFTypeRef() as AXUIElementRef) })
}

fn ax_string_attribute(element: &AXUIElement, name: &str) -> String {
    let attribute = AXAttribute::new(&CFString::new(name));
    element
        .attribute(&attribute)
        .ok()
        .and_then(|value: CFType| value.downcast::<CFString>())
        .map(|value| value.to_string())
        .unwrap_or_default()
}

fn rounded_bounds(element: &AXUIElement) -> Option<(i64, i64, i64, i64)> {
    let position = ax_point_attribute(element, "AXPosition")?;
    let size = ax_size_attribute(element, "AXSize")?;
    Some((
        position.x.round() as i64,
        position.y.round() as i64,
        size.width.round() as i64,
        size.height.round() as i64,
    ))
}

fn focused_text_role_is_supported(role: &str) -> bool {
    matches!(
        role,
        "AXTextArea" | "AXTextField" | "AXSearchField" | "AXComboBox"
    )
}

fn capture_focused_text_target_unchecked() -> Result<FocusedTextTarget, String> {
    let system = AXUIElement::system_wide();
    let app = ax_element_attribute(&system, "AXFocusedApplication")
        .ok_or_else(|| "无法读取 macOS 当前前台应用".to_string())?;
    let mut pid = 0;
    let result = unsafe { AXUIElementGetPid(app.as_concrete_TypeRef(), &mut pid) };
    if result != kAXErrorSuccess || pid <= 0 {
        return Err(format!(
            "无法确认 macOS 当前前台应用进程: {}",
            error_string(result)
        ));
    }
    let focused = ax_element_attribute(&app, "AXFocusedUIElement")
        .ok_or_else(|| "MiMoCode 当前没有可读取的文本光标；请先点击终端输入位置".to_string())?;
    let role = ax_string_attribute(&focused, "AXRole");
    if !focused_text_role_is_supported(&role) {
        return Err(format!(
            "MiMoCode 当前焦点不是可输入文本的位置（{}）；请先点击终端输入位置",
            if role.is_empty() {
                "未知控件"
            } else {
                &role
            }
        ));
    }
    let window = ax_element_attribute(&focused, "AXWindow")
        .or_else(|| ax_element_attribute(&app, "AXFocusedWindow"));
    let identifier = [
        ax_string_attribute(&focused, "AXIdentifier"),
        ax_string_attribute(&focused, "AXDOMIdentifier"),
        ax_string_attribute(&focused, "AXDocument"),
    ]
    .into_iter()
    .find(|value| !value.trim().is_empty())
    .unwrap_or_default();
    Ok(FocusedTextTarget {
        pid,
        role,
        subrole: ax_string_attribute(&focused, "AXSubrole"),
        identifier,
        window_title: window
            .as_ref()
            .map(|window| ax_string_attribute(window, "AXTitle"))
            .unwrap_or_default(),
        element_bounds: rounded_bounds(&focused),
        window_bounds: window.as_ref().and_then(rounded_bounds),
    })
}

fn focused_text_target_matches(captured: &FocusedTextTarget, current: &FocusedTextTarget) -> bool {
    captured.pid == current.pid
        && captured.role == current.role
        && captured.subrole == current.subrole
        && (captured.identifier.is_empty()
            || current.identifier.is_empty()
            || captured.identifier == current.identifier)
        && (captured.window_title.is_empty()
            || current.window_title.is_empty()
            || captured.window_title == current.window_title)
        && (captured.element_bounds.is_none()
            || current.element_bounds.is_none()
            || captured.element_bounds == current.element_bounds)
        && (captured.window_bounds.is_none()
            || current.window_bounds.is_none()
            || captured.window_bounds == current.window_bounds)
}

pub(super) fn capture_focused_text_target() -> Result<FocusedTextTarget, String> {
    ensure_accessibility_permission()?;
    capture_focused_text_target_unchecked()
}

pub(super) fn insert_and_submit_at_focused_text_target(
    captured: &FocusedTextTarget,
    text: &str,
) -> Result<(), String> {
    ensure_accessibility_permission()?;
    let current = capture_focused_text_target_unchecked()?;
    if !focused_text_target_matches(captured, &current) {
        return Err(
            "MiMoCode 录音期间前台窗口或文本光标已变化，本次语音未写入；请将光标停在输入位置后重试"
                .to_string(),
        );
    }
    let snapshot = PasteboardSnapshot::capture()?;
    let current = match capture_focused_text_target_unchecked() {
        Ok(current) => current,
        Err(error) => {
            snapshot.restore();
            return Err(error);
        }
    };
    if !focused_text_target_matches(captured, &current) {
        snapshot.restore();
        return Err(
            "MiMoCode 备份剪贴板期间前台窗口或文本光标已变化，本次语音未写入；请将光标停在输入位置后重试"
                .to_string(),
        );
    }
    let result = (|| {
        paste_focused_text(text)?;
        let current = capture_focused_text_target_unchecked().map_err(|error| {
            format!("MiMoCode 文字已写入，但提交前无法再次确认当前光标；未自动回车: {error}")
        })?;
        if !focused_text_target_matches(captured, &current) {
            return Err(
                "MiMoCode 文字已写入，但提交前焦点发生变化；为避免误操作，未自动回车".to_string(),
            );
        }
        post_keyboard_event(KeyCode::RETURN, CGEventFlags::CGEventFlagNull)
            .map_err(|error| format!("MiMoCode 文字已写入，但自动回车失败: {error}"))
    })();
    snapshot.restore();
    result.map_err(|error| {
        if error.starts_with("MiMoCode ") {
            error
        } else {
            format!("MiMoCode 当前光标输入失败: {error}")
        }
    })
}

fn ax_point_attribute(element: &AXUIElement, name: &str) -> Option<CGPoint> {
    let attribute = AXAttribute::new(&CFString::new(name));
    let value: CFType = element.attribute(&attribute).ok()?;
    if value.type_of() != unsafe { AXValueGetTypeID() } {
        return None;
    }
    let value = value.as_CFTypeRef() as AXValueRef;
    if unsafe { AXValueGetType(value) } != kAXValueTypeCGPoint {
        return None;
    }
    let mut point = CGPoint::default();
    unsafe {
        AXValueGetValue(
            value,
            kAXValueTypeCGPoint,
            &mut point as *mut CGPoint as *mut c_void,
        )
    }
    .then_some(point)
}

fn ax_size_attribute(element: &AXUIElement, name: &str) -> Option<CGSize> {
    let attribute = AXAttribute::new(&CFString::new(name));
    let value: CFType = element.attribute(&attribute).ok()?;
    if value.type_of() != unsafe { AXValueGetTypeID() } {
        return None;
    }
    let value = value.as_CFTypeRef() as AXValueRef;
    if unsafe { AXValueGetType(value) } != kAXValueTypeCGSize {
        return None;
    }
    let mut size = CGSize::default();
    unsafe {
        AXValueGetValue(
            value,
            kAXValueTypeCGSize,
            &mut size as *mut CGSize as *mut c_void,
        )
    }
    .then_some(size)
}

fn thread_deeplink(session_id: &str) -> Result<String, String> {
    let session_id = session_id.trim();
    if session_id.is_empty()
        || session_id.len() > 512
        || !session_id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
    {
        return Err("当前 ChatGPT（Codex）会话没有可用的精确 task ID".to_string());
    }
    Ok(format!("codex://threads/{session_id}"))
}

fn session_deeplink(
    agent: MacosAgent,
    session_id: &str,
    requested_deep_link: &str,
) -> Result<String, String> {
    if agent == MacosAgent::Codex {
        return thread_deeplink(session_id);
    }
    let requested_deep_link = requested_deep_link.trim();
    if requested_deep_link.starts_with("claude://code/")
        || requested_deep_link.starts_with("claude://resume?session=")
    {
        Ok(requested_deep_link.to_string())
    } else {
        Err("当前 Claude 会话没有可用的精确 Code session 深链".to_string())
    }
}

fn focus_session_deeplink(
    agent: MacosAgent,
    session_id: &str,
    requested_deep_link: &str,
) -> Result<(), String> {
    ensure_accessibility_permission()?;
    let deeplink = session_deeplink(agent, session_id, requested_deep_link)?;
    let status = crate::command_for_host("open")
        .arg(&deeplink)
        .status()
        .map_err(|error| format!("无法打开 {} 会话深链: {error}", agent.label()))?;
    if !status.success() {
        return Err(format!("{} 会话深链打开失败: {status}", agent.label()));
    }
    // LaunchServices delivers the route but does not guarantee that the
    // receiving app becomes frontmost. Pet Manager must explicitly activate
    // and raise Codex before posting any global keyboard events, otherwise the
    // safety probe can read Pet Manager's own page and reject a valid composer.
    thread::sleep(Duration::from_millis(150));
    focus_agent_front_window(agent)?;
    thread::sleep(DEEPLINK_FOCUS_DELAY);
    Ok(())
}

#[derive(Clone, Debug)]
struct ClaudeDesktopFocusMarker {
    path: PathBuf,
    last_focused_at: u64,
    modified_at: Option<SystemTime>,
}

fn claude_desktop_sessions_root() -> Option<PathBuf> {
    if let Some(configured) = std::env::var_os("CLAUDE_DESKTOP_SESSIONS_DIR") {
        let configured = PathBuf::from(configured);
        if !configured.as_os_str().is_empty() {
            return Some(configured);
        }
    }
    std::env::var_os("HOME").map(|home| {
        PathBuf::from(home)
            .join("Library")
            .join("Application Support")
            .join("Claude")
            .join("claude-code-sessions")
    })
}

fn claude_desktop_focus_marker(session_id: &str) -> Option<ClaudeDesktopFocusMarker> {
    let root = claude_desktop_sessions_root()?;
    let expected_name = format!("local_{}.json", session_id.trim());
    let mut pending = vec![(root, 0usize)];
    while let Some((directory, depth)) = pending.pop() {
        let Ok(entries) = fs::read_dir(directory) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let Ok(file_type) = entry.file_type() else {
                continue;
            };
            if file_type.is_dir() {
                if depth < 4 {
                    pending.push((path, depth + 1));
                }
                continue;
            }
            if !file_type.is_file() || entry.file_name() != expected_name.as_str() {
                continue;
            }
            let metadata = fs::metadata(&path).ok();
            let last_focused_at = fs::read(&path)
                .ok()
                .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(&bytes).ok())
                .and_then(|value| value.get("lastFocusedAt").and_then(|value| value.as_u64()))
                .unwrap_or_default();
            return Some(ClaudeDesktopFocusMarker {
                path,
                last_focused_at,
                modified_at: metadata.and_then(|metadata| metadata.modified().ok()),
            });
        }
    }
    None
}

fn claude_focus_marker_advanced(
    before: Option<&ClaudeDesktopFocusMarker>,
    after: &ClaudeDesktopFocusMarker,
) -> bool {
    let Some(before) = before else {
        return true;
    };
    before.path != after.path
        || after.last_focused_at > before.last_focused_at
        || after.modified_at > before.modified_at
}

fn claude_session_is_latest_focused(session_id: &str) -> bool {
    let Some(target) = claude_desktop_focus_marker(session_id) else {
        return false;
    };
    let Some(root) = claude_desktop_sessions_root() else {
        return false;
    };
    let mut newest = target.last_focused_at;
    let mut pending = vec![(root, 0usize)];
    while let Some((directory, depth)) = pending.pop() {
        let Ok(entries) = fs::read_dir(directory) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let Ok(file_type) = entry.file_type() else {
                continue;
            };
            if file_type.is_dir() {
                if depth < 4 {
                    pending.push((path, depth + 1));
                }
                continue;
            }
            if !file_type.is_file() || !entry.file_name().to_string_lossy().starts_with("local_") {
                continue;
            }
            if let Some(last_focused_at) = fs::read(&path)
                .ok()
                .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(&bytes).ok())
                .and_then(|value| value.get("lastFocusedAt").and_then(|value| value.as_u64()))
            {
                newest = newest.max(last_focused_at);
            }
        }
    }
    target.last_focused_at > 0 && target.last_focused_at == newest
}

fn cf_string(result: Result<CFString, accessibility::Error>) -> Option<String> {
    result.ok().map(|value| value.to_string())
}

fn element_value(element: &AXUIElement) -> String {
    element
        .value()
        .ok()
        .and_then(|value| value.downcast::<CFString>())
        .map(|value| value.to_string())
        .unwrap_or_default()
}

fn element_role(element: &AXUIElement) -> String {
    cf_string(element.role()).unwrap_or_default()
}

fn element_labels(element: &AXUIElement) -> Vec<String> {
    let mut labels = Vec::new();
    for value in [
        cf_string(element.title()),
        cf_string(element.description()),
        cf_string(element.help()),
        cf_string(element.placeholder_value()),
        Some(element_value(element)),
    ]
    .into_iter()
    .flatten()
    {
        let normalized = normalize_text(&value);
        if !normalized.is_empty() && !labels.iter().any(|label| label == &normalized) {
            labels.push(normalized);
        }
    }
    labels
}

fn element_has_exact_label(element: &AXUIElement, expected: &str) -> bool {
    let expected = normalize_text(expected);
    !expected.is_empty()
        && element_labels(element)
            .iter()
            .any(|label| label.eq_ignore_ascii_case(&expected))
}

fn element_label_contains(element: &AXUIElement, needles: &[&str]) -> bool {
    element_labels(element).iter().any(|label| {
        let label = label.to_lowercase();
        needles.iter().any(|needle| label.contains(needle))
    })
}

fn element_is_enabled(element: &AXUIElement) -> bool {
    element.enabled().map(bool::from).unwrap_or(true)
}

fn element_supports_action(element: &AXUIElement, action: &str) -> bool {
    element
        .action_names()
        .map(|actions| {
            actions
                .into_iter()
                .any(|candidate| candidate.to_string() == action)
        })
        .unwrap_or(false)
}

fn element_supports_press(element: &AXUIElement) -> bool {
    element_supports_action(element, "AXPress")
}

fn element_array_attribute(element: &AXUIElement, name: &str) -> Vec<AXUIElement> {
    let attribute = AXAttribute::new(&CFString::new(name));
    element
        .attribute(&attribute)
        .ok()
        .and_then(|value: CFType| value.downcast::<CFArray>())
        .map(|values| {
            values
                .get_all_values()
                .into_iter()
                .filter(|value| !value.is_null())
                .map(|value| unsafe { AXUIElement::wrap_under_get_rule(value as AXUIElementRef) })
                .collect()
        })
        .unwrap_or_default()
}

fn walk_elements(
    root: &AXUIElement,
    mut visit: impl FnMut(&AXUIElement) -> bool,
) -> Vec<AXUIElement> {
    let mut matches = Vec::new();
    let mut stack = vec![(root.clone(), 0usize)];
    let mut seen = Vec::<AXUIElement>::new();
    let mut visited = 0usize;
    while let Some((element, depth)) = stack.pop() {
        if seen.iter().any(|candidate| candidate == &element) {
            continue;
        }
        seen.push(element.clone());
        visited += 1;
        if visit(&element) {
            matches.push(element.clone());
        }
        if visited >= MAX_TREE_ELEMENTS || depth >= MAX_TREE_DEPTH {
            continue;
        }
        let mut children = Vec::new();
        for candidates in [
            element.children().ok(),
            element.visible_children().ok(),
            element.selected_children().ok(),
        ]
        .into_iter()
        .flatten()
        {
            for child in candidates.iter().map(|child| child.clone()) {
                if !children.iter().any(|candidate| candidate == &child) {
                    children.push(child);
                }
            }
        }
        for attribute in ["AXChildrenInNavigationOrder", "AXRows"] {
            for child in element_array_attribute(&element, attribute) {
                if !children.iter().any(|candidate| candidate == &child) {
                    children.push(child);
                }
            }
        }
        if let Ok(contents) = element.contents() {
            if !children.iter().any(|candidate| candidate == &contents) {
                children.push(contents);
            }
        }
        children.reverse();
        stack.extend(children.into_iter().map(|child| (child, depth + 1)));
    }
    matches
}

fn ancestors(element: &AXUIElement) -> Vec<AXUIElement> {
    let mut result = Vec::new();
    let mut current = element.clone();
    for _ in 0..MAX_ANCESTOR_DEPTH {
        let Ok(parent) = current.parent() else {
            break;
        };
        if result.iter().any(|existing| existing == &parent) {
            break;
        }
        result.push(parent.clone());
        current = parent;
    }
    result
}

fn nearest_pressable(element: &AXUIElement) -> Option<AXUIElement> {
    if element_supports_press(element) {
        return Some(element.clone());
    }
    ancestors(element).into_iter().find(element_supports_press)
}

fn ancestor_has_exact_label(element: &AXUIElement, expected: &str) -> bool {
    ancestors(element)
        .iter()
        .any(|ancestor| element_has_exact_label(ancestor, expected))
}

fn pgrep_exact(process_name: &str) -> Vec<i32> {
    let output = crate::command_for_host("pgrep")
        .args(["-x", process_name])
        .output();
    let Ok(output) = output else {
        return Vec::new();
    };
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter_map(|line| line.trim().parse::<i32>().ok())
        .collect()
}

fn agent_application_pids(agent: MacosAgent) -> Vec<i32> {
    let mut pids = Vec::new();
    for bundle_identifier in agent_bundle_identifiers(agent) {
        let applications = NSRunningApplication::runningApplicationsWithBundleIdentifier(
            &NSString::from_str(bundle_identifier),
        );
        for application in applications.iter() {
            let pid = application.processIdentifier();
            if pid > 0 && !pids.contains(&pid) {
                pids.push(pid);
            }
        }
    }
    if pids.is_empty() {
        for process_name in agent_application_names(agent) {
            for pid in pgrep_exact(process_name) {
                if !pids.contains(&pid) {
                    pids.push(pid);
                }
            }
        }
    }
    pids
}

fn primed_agent_application_pids() -> &'static Mutex<HashSet<i32>> {
    static PRIMED_PIDS: OnceLock<Mutex<HashSet<i32>>> = OnceLock::new();
    PRIMED_PIDS.get_or_init(|| Mutex::new(HashSet::new()))
}

fn mark_agent_accessibility_primed(primed_pids: &mut HashSet<i32>, pid: i32) -> bool {
    primed_pids.insert(pid)
}

fn prime_agent_accessibility_once(app: &AXUIElement, pid: i32) -> bool {
    let should_prime = {
        let mut primed_pids = primed_agent_application_pids()
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        mark_agent_accessibility_primed(&mut primed_pids, pid)
    };
    if !should_prime {
        return false;
    }

    // Chromium-backed macOS apps keep their web accessibility tree lazy
    // until an assistive client opts in. Rewriting these flags on every
    // lookup can rebuild the tree between two consecutive window scans, so
    // each running process is primed only once.
    let manual_accessibility = AXAttribute::new(&CFString::new("AXManualAccessibility"));
    let _ = app.set_attribute(&manual_accessibility, CFBoolean::true_value().into_CFType());
    let enhanced_accessibility = AXAttribute::new(&CFString::new("AXEnhancedUserInterface"));
    let _ = app.set_attribute(
        &enhanced_accessibility,
        CFBoolean::true_value().into_CFType(),
    );
    true
}

fn wait_for_agent_windows(app: &AXUIElement) -> bool {
    let deadline = Instant::now() + ACCESSIBILITY_TREE_READY_TIMEOUT;
    loop {
        if matches!(app.windows(), Ok(windows) if windows.iter().next().is_some()) {
            return true;
        }
        if Instant::now() >= deadline {
            return false;
        }
        thread::sleep(ACCESSIBILITY_TREE_RETRY_DELAY);
    }
}

fn agent_applications(agent: MacosAgent) -> Result<Vec<AXUIElement>, String> {
    ensure_accessibility_permission()?;
    let pids = agent_application_pids(agent);
    if pids.is_empty() {
        return Err(format!("未找到正在运行的 {} macOS 应用", agent.label()));
    }
    let mut apps = Vec::new();
    for pid in pids {
        let app = AXUIElement::application(pid);
        let _ = app.set_messaging_timeout(1.5);
        prime_agent_accessibility_once(&app, pid);
        if wait_for_agent_windows(&app) {
            apps.push(app);
        }
    }
    if apps.is_empty() {
        Err(format!(
            "{} 已运行，但 macOS 辅助功能无法读取其窗口",
            agent.label()
        ))
    } else {
        Ok(apps)
    }
}

fn agent_windows(agent: MacosAgent) -> Result<Vec<CodexWindow>, String> {
    let mut result = Vec::new();
    for app in agent_applications(agent)? {
        let windows = app
            .windows()
            .map_err(|error| format!("读取 {} macOS 窗口失败: {error}", agent.label()))?;
        for window in windows.iter().map(|window| window.clone()) {
            result.push(CodexWindow {
                app: app.clone(),
                window,
            });
        }
    }
    if result.is_empty() {
        Err(format!("未找到可见的 {} macOS 窗口", agent.label()))
    } else {
        Ok(result)
    }
}

fn is_agent_primary_window_title(agent: MacosAgent, title: &str) -> bool {
    let title = normalize_text(title).to_lowercase();
    match agent {
        MacosAgent::Codex => matches!(title.as_str(), "codex" | "chatgpt"),
        MacosAgent::Claude => title == "claude",
    }
}

fn window_area(window: &AXUIElement) -> f64 {
    ax_size_attribute(window, "AXSize")
        .map(|size| size.width.max(0.0) * size.height.max(0.0))
        .unwrap_or_default()
}

fn primary_agent_window(agent: MacosAgent) -> Result<CodexWindow, String> {
    let windows = agent_windows(agent)?;
    let preferred = windows
        .iter()
        .filter(|target| {
            cf_string(target.window.title())
                .is_some_and(|title| is_agent_primary_window_title(agent, &title))
        })
        .cloned()
        .collect::<Vec<_>>();
    let candidates = if preferred.is_empty() && agent == MacosAgent::Claude {
        windows
    } else {
        preferred
    };
    candidates
        .into_iter()
        // Recent Codex builds can expose a small utility surface titled
        // "Codex" alongside the full "ChatGPT" primary window. Selecting the
        // first title match raises the utility surface and makes every
        // composer candidate invalid, so prefer the largest exact-title
        // window instead.
        .max_by(|left, right| window_area(&left.window).total_cmp(&window_area(&right.window)))
        .ok_or_else(|| format!("未找到 {} macOS 主窗口", agent.label()))
}

fn launch_agent_application(agent: MacosAgent) -> Result<(), String> {
    let mut errors = Vec::new();
    for bundle_identifier in agent_bundle_identifiers(agent) {
        match crate::command_for_host("open")
            .args(["-b", *bundle_identifier])
            .status()
        {
            Ok(status) if status.success() => return Ok(()),
            Ok(status) => errors.push(format!("{bundle_identifier}: {status}")),
            Err(error) => errors.push(format!("{bundle_identifier}: {error}")),
        }
    }
    for application_name in agent_application_names(agent) {
        match crate::command_for_host("open")
            .args(["-a", *application_name])
            .status()
        {
            Ok(status) if status.success() => return Ok(()),
            Ok(status) => errors.push(format!("{application_name}: {status}")),
            Err(error) => errors.push(format!("{application_name}: {error}")),
        }
    }
    Err(format!(
        "无法启动 {} macOS 应用: {}",
        agent.label(),
        errors.join("; ")
    ))
}

fn primary_or_launch_agent_window(agent: MacosAgent) -> Result<CodexWindow, String> {
    ensure_accessibility_permission()?;
    if let Ok(target) = primary_agent_window(agent) {
        return Ok(target);
    }
    launch_agent_application(agent)?;

    let deadline = Instant::now() + AGENT_LAUNCH_WINDOW_TIMEOUT;
    loop {
        let error = match primary_agent_window(agent) {
            Ok(target) => return Ok(target),
            Err(error) => error,
        };
        if Instant::now() >= deadline {
            return Err(format!(
                "{} macOS 应用启动后没有出现可访问窗口: {error}",
                agent.label(),
            ));
        }
        thread::sleep(Duration::from_millis(100));
    }
}

fn focus_agent_front_window(agent: MacosAgent) -> Result<(), String> {
    let target = primary_agent_window(agent)?;
    focus_window(&target)
}

fn active_title_matches(window: &AXUIElement, expected_title: &str) -> bool {
    if cf_string(window.title())
        .is_some_and(|title| normalize_text(&title).eq_ignore_ascii_case(expected_title))
    {
        return true;
    }
    walk_elements(window, |element| {
        if !element_has_exact_label(element, expected_title) {
            return false;
        }
        let role = element_role(element);
        role == "AXHeading"
            || ancestors(element)
                .iter()
                .any(|ancestor| element_role(ancestor) == "AXHeading")
            || nearest_pressable(element).is_none()
    })
    .into_iter()
    .next()
    .is_some()
}

fn focus_window(target: &CodexWindow) -> Result<(), String> {
    let minimized = AXAttribute::new(&CFString::new("AXMinimized"));
    let _ = target
        .window
        .set_attribute(&minimized, CFBoolean::false_value().into_CFType());
    let mut pid = 0;
    let pid_result = unsafe { AXUIElementGetPid(target.app.as_concrete_TypeRef(), &mut pid) };
    let running_application = (pid_result == kAXErrorSuccess && pid > 0)
        .then(|| NSRunningApplication::runningApplicationWithProcessIdentifier(pid))
        .flatten();
    if let Some(application) = running_application.as_ref() {
        if application.isHidden() {
            let _ = application.unhide();
        }
        let _ = application.activateWithOptions(NSApplicationActivationOptions::ActivateAllWindows);
    }
    target
        .app
        .set_frontmost(CFBoolean::true_value())
        .map_err(|error| format!("Agent macOS 应用无法切到前台: {error}"))?;
    let _ = target.window.raise();
    let deadline = Instant::now() + AGENT_FRONTMOST_TIMEOUT;
    loop {
        let ax_frontmost =
            ax_element_attribute(&AXUIElement::system_wide(), "AXFocusedApplication")
                .is_some_and(|application| application == target.app);
        let appkit_frontmost = running_application
            .as_ref()
            .is_some_and(|application| application.isActive());
        if ax_frontmost || appkit_frontmost {
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err(format!(
                "{} macOS 应用已启动，但无法确认已切到前台",
                cf_string(target.window.title())
                    .filter(|title| !title.trim().is_empty())
                    .unwrap_or_else(|| "Agent".to_string())
            ));
        }
        if let Some(application) = running_application.as_ref() {
            let _ =
                application.activateWithOptions(NSApplicationActivationOptions::ActivateAllWindows);
        }
        let _ = target.app.set_frontmost(CFBoolean::true_value());
        let _ = target.window.raise();
        thread::sleep(ACCESSIBILITY_TREE_RETRY_DELAY);
    }
}

fn show_sidebar(target: &CodexWindow) {
    let buttons = walk_elements(&target.window, |element| {
        element_role(element) == "AXButton"
            && element_is_enabled(element)
            && element_label_contains(
                element,
                &["show sidebar", "open sidebar", "显示边栏", "展开边栏"],
            )
            && element_supports_press(element)
    });
    if let Some(button) = buttons.first() {
        let _ = focus_window(target);
        let _ = button.press();
        thread::sleep(Duration::from_millis(120));
    }
}

fn find_session_rows(
    windows: &[CodexWindow],
    session_title: &str,
    workspace_label: &str,
) -> Vec<SessionRow> {
    let mut rows = Vec::new();
    for target in windows {
        for label in walk_elements(&target.window, |element| {
            element_has_exact_label(element, session_title)
        }) {
            let Some(pressable) = nearest_pressable(&label) else {
                continue;
            };
            if rows
                .iter()
                .any(|existing: &SessionRow| existing.target == pressable)
            {
                continue;
            }
            rows.push(SessionRow {
                app: target.app.clone(),
                window: target.window.clone(),
                target: pressable,
                workspace_matches: !workspace_label.is_empty()
                    && ancestor_has_exact_label(&label, workspace_label),
            });
        }
    }
    rows
}

fn press_unique_session_row(
    agent: MacosAgent,
    session_title: &str,
    workspace_label: &str,
) -> Result<(), String> {
    let session_title = normalize_text(session_title);
    if session_title.is_empty() {
        return Err(format!("当前 {} 会话没有可定位的标题", agent.label()));
    }
    let mut windows = agent_windows(agent)?;
    for target in &windows {
        show_sidebar(target);
    }
    windows = agent_windows(agent)?;
    let mut rows = find_session_rows(&windows, &session_title, workspace_label);
    if rows.len() > 1 && !workspace_label.is_empty() {
        rows.retain(|row| row.workspace_matches);
    }
    if rows.is_empty() {
        return Err(format!(
            "在 {} macOS 侧边栏中没有找到绑定会话",
            agent.label()
        ));
    }
    if rows.len() > 1 {
        return Err(format!(
            "{} macOS 侧边栏中有多个同名会话，无法安全定位",
            agent.label()
        ));
    }
    let row = rows.remove(0);
    let target = CodexWindow {
        app: row.app,
        window: row.window,
    };
    focus_window(&target)?;
    let scroll_to_visible = CFString::new("AXScrollToVisible");
    if element_supports_action(&row.target, "AXScrollToVisible") {
        let _ = row.target.perform_action(&scroll_to_visible);
        thread::sleep(Duration::from_millis(80));
    }
    row.target
        .press()
        .map_err(|error| format!("无法打开 {} macOS 会话: {error}", agent.label()))
}

pub(super) fn focus_session(
    agent: MacosAgent,
    deep_link: &str,
    session_id: &str,
    session_title: &str,
    workspace_label: &str,
) -> Result<(), String> {
    if !session_id.trim().is_empty() {
        let claude_marker_before = (agent == MacosAgent::Claude)
            .then(|| claude_desktop_focus_marker(session_id))
            .flatten();
        focus_session_deeplink(agent, session_id, deep_link)?;
        if agent != MacosAgent::Claude {
            return Ok(());
        }
        let session_title = normalize_text(session_title);
        let deadline = Instant::now() + SESSION_CONFIRM_TIMEOUT;
        while Instant::now() < deadline {
            if claude_desktop_focus_marker(session_id).is_some_and(|marker| {
                claude_focus_marker_advanced(claude_marker_before.as_ref(), &marker)
            }) {
                return Ok(());
            }
            if !session_title.is_empty()
                && agent_windows(agent)?
                    .iter()
                    .any(|target| active_title_matches(&target.window, &session_title))
            {
                return Ok(());
            }
            thread::sleep(Duration::from_millis(50));
        }
        return Err("Claude macOS 深链已打开，但客户端没有确认目标 Code 会话".to_string());
    }
    let session_title = normalize_text(session_title);
    if session_title.is_empty() {
        return Err(format!("当前 {} 会话没有可定位的标题", agent.label()));
    }
    let windows = agent_windows(agent)?;
    if windows
        .iter()
        .any(|target| active_title_matches(&target.window, &session_title))
    {
        return Ok(());
    }
    press_unique_session_row(agent, &session_title, workspace_label)?;

    let deadline = Instant::now() + SESSION_CONFIRM_TIMEOUT;
    while Instant::now() < deadline {
        if agent_windows(agent)?
            .iter()
            .any(|candidate| active_title_matches(&candidate.window, &session_title))
        {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(50));
    }
    Err(format!("{} macOS 在切换后没有确认目标会话", agent.label()))
}

fn is_placeholder(value: &str) -> bool {
    let value = normalize_text(value).to_lowercase();
    matches!(
        value.as_str(),
        "随心输入"
            | "输入消息"
            | "message codex"
            | "ask anything"
            | "do anything"
            | "write your prompt to claude"
            | "type / for commands"
            | "write a message"
            | "write a message..."
    )
}

fn is_composer_element(element: &AXUIElement) -> bool {
    let role = element_role(element);
    if role != "AXTextArea" && role != "AXTextField" {
        return false;
    }
    let labels = element_labels(element)
        .into_iter()
        .map(|label| label.to_lowercase())
        .collect::<Vec<_>>();
    let recognized = labels.iter().any(|label| {
        [
            "message codex",
            "ask anything",
            "do anything",
            "write your prompt to claude",
            "type / for commands",
            "write a message",
            "随心输入",
            "输入消息",
        ]
        .iter()
        .any(|needle| label.contains(needle))
    });
    let search_field = labels
        .iter()
        .any(|label| label.contains("search") || label.contains("搜索"));
    recognized || (role == "AXTextArea" && !search_field)
}

fn composer_value(element: &AXUIElement) -> String {
    let value = normalize_text(&element_value(element));
    if is_placeholder(&value) {
        String::new()
    } else {
        value
    }
}

fn find_composer(agent: MacosAgent, window: &CodexWindow) -> Result<(AXUIElement, String), String> {
    let mut composers = Vec::new();
    for composer in walk_elements(&window.window, is_composer_element) {
        if !element_is_enabled(&composer) {
            continue;
        }
        let value = composer_value(&composer);
        composers.push((composer, value));
    }
    if composers.len() != 1 {
        return Err(format!(
            "需要唯一的 {} macOS 输入框，实际找到 {} 个",
            agent.label(),
            composers.len()
        ));
    }
    Ok(composers.remove(0))
}

fn find_target(
    agent: MacosAgent,
    session_id: &str,
    session_title: &str,
) -> Result<ComposerTarget, String> {
    if agent == MacosAgent::Claude && !claude_session_is_latest_focused(session_id) {
        return Err("Claude macOS 当前会话与设备选中的会话不一致".to_string());
    }
    let mut matched_title = false;
    let mut errors = Vec::new();
    for target in agent_windows(agent)? {
        if agent != MacosAgent::Claude && !active_title_matches(&target.window, session_title) {
            continue;
        }
        matched_title = true;
        match find_composer(agent, &target) {
            Ok((composer, value)) => {
                return Ok(ComposerTarget {
                    agent,
                    app: target.app,
                    window: target.window,
                    composer,
                    value,
                });
            }
            Err(error) => errors.push(error),
        }
    }
    if !matched_title {
        Err(format!(
            "{} macOS 当前会话与设备选中的会话不一致",
            agent.label()
        ))
    } else {
        Err(format!(
            "{} macOS 会话已匹配，但输入框不可用: {}",
            agent.label(),
            errors.join("; ")
        ))
    }
}

fn find_current_visible_target(agent: MacosAgent) -> Result<ComposerTarget, String> {
    let mut target = primary_or_launch_agent_window(agent)?;
    let deadline = Instant::now() + COMPOSER_STABILITY_TIMEOUT;
    loop {
        if let Ok(current) = primary_agent_window(agent) {
            target = current;
        }
        let result = (|| {
            focus_window(&target)?;
            let (composer, value) = find_composer(agent, &target)?;
            let candidate = ComposerTarget {
                agent,
                app: target.app.clone(),
                window: target.window.clone(),
                composer: composer.clone(),
                value,
            };
            focus_composer(&candidate)?;
            thread::sleep(COMPOSER_STABILITY_DELAY);
            let refreshed = primary_agent_window(agent)?;
            if refreshed.window != target.window {
                return Err(format!("{} macOS 主窗口仍在恢复", agent.label()));
            }
            let (refreshed_composer, refreshed_value) = find_composer(agent, &refreshed)?;
            if refreshed_composer != composer {
                return Err(format!("{} macOS 输入框仍在恢复", agent.label()));
            }
            Ok(ComposerTarget {
                agent,
                app: refreshed.app,
                window: refreshed.window,
                composer: refreshed_composer,
                value: refreshed_value,
            })
        })();
        let last_error = match result {
            Ok(target) => return Ok(target),
            Err(error) => error,
        };
        if Instant::now() >= deadline {
            return Err(format!(
                "{} macOS 应用已置前，但输入框未稳定: {last_error}",
                agent.label()
            ));
        }
        thread::sleep(ACCESSIBILITY_TREE_RETRY_DELAY);
    }
}

fn find_voice_target(state: &mut MacosComposerState) -> Result<ComposerTarget, String> {
    let Some(pinned) = state.current_visible_target.as_ref() else {
        return find_target(state.agent, &state.session_id, &state.session_title);
    };
    let pinned_window = pinned.window.clone();
    let pinned_composer = pinned.composer.clone();
    let Some(window) = agent_windows(state.agent)?
        .into_iter()
        .find(|candidate| candidate.window == pinned_window)
    else {
        return Err(format!(
            "{} macOS 当前窗口在语音输入期间发生变化",
            state.agent.label()
        ));
    };
    focus_window(&window)?;
    let (composer, value) = find_composer(state.agent, &window)?;
    if composer != pinned_composer {
        if let Some(pinned) = state.current_visible_target.as_mut() {
            pinned.app = window.app.clone();
            pinned.window = window.window.clone();
            pinned.composer = composer.clone();
            pinned.value = value.clone();
        }
    }
    Ok(ComposerTarget {
        agent: state.agent,
        app: window.app,
        window: window.window,
        composer,
        value,
    })
}

fn focus_composer(target: &ComposerTarget) -> Result<(), String> {
    focus_window(&CodexWindow {
        app: target.app.clone(),
        window: target.window.clone(),
    })?;
    target
        .composer
        .set_attribute(&AXAttribute::focused(), CFBoolean::true_value())
        .map_err(|error| format!("{} macOS 输入框无法获得焦点: {error}", target.agent.label()))
}

fn replace_composer_text(target: &ComposerTarget, text: &str) -> Result<(), String> {
    let text = normalize_text(text);
    focus_composer(target)?;
    target
        .composer
        .set_attribute(&AXAttribute::value(), CFString::new(&text).into_CFType())
        .map_err(|error| format!("{} macOS 输入框写入失败: {error}", target.agent.label()))?;
    let deadline = Instant::now() + COMPOSER_READBACK_TIMEOUT;
    while Instant::now() < deadline {
        if composer_value(&target.composer) == text {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(25));
    }
    Err(format!(
        "{} macOS 输入框没有确认语音文本",
        target.agent.label()
    ))
}

pub(super) fn begin_voice(
    agent: MacosAgent,
    deep_link: &str,
    session_id: &str,
    session_title: &str,
    workspace_label: &str,
) -> Result<MacosComposerState, String> {
    focus_session(agent, deep_link, session_id, session_title, workspace_label)?;
    let session_title = normalize_text(session_title);
    let (target, pin_visible_target) = match find_target(agent, session_id, &session_title) {
        Ok(target) => (target, false),
        Err(session_match_error) if agent == MacosAgent::Codex && !session_id.trim().is_empty() => {
            // A successful exact ID deep link has already selected the requested
            // Codex task. Some Codex builds expose neither the active title nor
            // the matching sidebar row through AX; in that case bind the one
            // stable composer now visible in the foreground window. Pinning the
            // element keeps later updates from falling back to title matching.
            let target = find_current_visible_target(agent)
                .map_err(|focus_error| format!("{session_match_error}; {focus_error}"))?;
            (target, true)
        }
        Err(error) => return Err(error),
    };
    focus_composer(&target)?;
    Ok(MacosComposerState {
        agent,
        session_id: session_id.to_string(),
        session_title,
        last_value: String::new(),
        current_visible_target: pin_visible_target.then_some(target),
    })
}

pub(super) fn begin_current_voice(agent: MacosAgent) -> Result<MacosComposerState, String> {
    let target = find_current_visible_target(agent)?;
    focus_composer(&target)?;
    Ok(MacosComposerState {
        agent,
        session_id: String::new(),
        session_title: String::new(),
        last_value: String::new(),
        current_visible_target: Some(target),
    })
}

pub(super) fn update_voice(state: &mut MacosComposerState, text: &str) -> Result<String, String> {
    let text = normalize_text(text);
    if text == state.last_value {
        return Ok(text);
    }
    let mut last_error = String::new();
    for _ in 0..2 {
        match find_voice_target(state).and_then(|target| {
            if target.value == text {
                Ok(())
            } else {
                replace_composer_text(&target, &text)
            }
        }) {
            Ok(()) => {
                state.last_value = text.clone();
                return Ok(text);
            }
            Err(error) => {
                last_error = error;
                thread::sleep(Duration::from_millis(40));
            }
        }
    }
    Err(format!(
        "{} macOS 前台语音更新重试后失败: {last_error}",
        state.agent.label()
    ))
}

fn send_button_score(element: &AXUIElement) -> Option<u8> {
    let labels = element_labels(element)
        .into_iter()
        .map(|label| label.to_lowercase())
        .collect::<Vec<_>>();
    if labels
        .iter()
        .any(|label| matches!(label.as_str(), "send" | "submit" | "发送" | "提交"))
    {
        return Some(0);
    }
    if labels.iter().any(|label| {
        ["send message", "submit message", "发送消息", "提交消息"]
            .iter()
            .any(|needle| label.contains(needle))
    }) {
        return Some(1);
    }
    None
}

fn send_button_bounds_match_composer(
    composer: (i64, i64, i64, i64),
    button: (i64, i64, i64, i64),
) -> bool {
    let (composer_x, composer_y, composer_width, composer_height) = composer;
    let (button_x, button_y, button_width, button_height) = button;
    if composer_width <= 0 || composer_height <= 0 || button_width <= 0 || button_height <= 0 {
        return false;
    }

    let composer_right = composer_x.saturating_add(composer_width);
    let composer_bottom = composer_y.saturating_add(composer_height);
    let button_right = button_x.saturating_add(button_width);
    let button_center_x = button_x.saturating_add(button_width / 2);
    let button_center_y = button_y.saturating_add(button_height / 2);
    let vertical_margin = composer_height.clamp(24, 56);

    // Codex and Claude place the send control inside, or immediately to the
    // right of, the bottom composer. Image-result cards can also expose a
    // generic “Send” action, but it is vertically separated from the composer.
    button_center_y >= composer_y.saturating_sub(vertical_margin)
        && button_center_y <= composer_bottom.saturating_add(vertical_margin)
        && button_right >= composer_x.saturating_sub(24)
        && button_x <= composer_right.saturating_add(120)
        && button_center_x >= composer_x.saturating_add(composer_width / 3)
}

fn elements_share_near_ancestor(left: &AXUIElement, right: &AXUIElement) -> bool {
    let left_ancestors = ancestors(left);
    let right_ancestors = ancestors(right);
    left_ancestors.iter().take(5).any(|left_ancestor| {
        right_ancestors
            .iter()
            .take(5)
            .any(|right_ancestor| right_ancestor == left_ancestor)
    })
}

fn send_button_matches_composer(button: &AXUIElement, composer: &AXUIElement) -> bool {
    match (rounded_bounds(composer), rounded_bounds(button)) {
        (Some(composer_bounds), Some(button_bounds)) => {
            send_button_bounds_match_composer(composer_bounds, button_bounds)
        }
        _ => elements_share_near_ancestor(button, composer),
    }
}

fn press_enter(agent: MacosAgent, app: &AXUIElement) -> Result<(), String> {
    let result = unsafe { AXUIElementPostKeyboardEvent(app.as_concrete_TypeRef(), 13, 36, true) };
    if result != kAXErrorSuccess {
        return Err(format!(
            "{} macOS Enter 按键按下失败: {}",
            agent.label(),
            error_string(result)
        ));
    }
    let result = unsafe { AXUIElementPostKeyboardEvent(app.as_concrete_TypeRef(), 13, 36, false) };
    if result == kAXErrorSuccess {
        Ok(())
    } else {
        Err(format!(
            "{} macOS Enter 按键释放失败: {}",
            agent.label(),
            error_string(result)
        ))
    }
}

fn submit_target(target: &ComposerTarget) -> Result<(), String> {
    focus_composer(target)?;
    let mut buttons = walk_elements(&target.window, |element| {
        element_role(element) == "AXButton"
            && element_is_enabled(element)
            && element_supports_press(element)
            && send_button_score(element).is_some()
            && send_button_matches_composer(element, &target.composer)
    })
    .into_iter()
    .filter_map(|button| send_button_score(&button).map(|score| (score, button)))
    .collect::<Vec<_>>();
    buttons.sort_by_key(|(score, _)| *score);
    let best_score = buttons.first().map(|(score, _)| *score);
    let best = buttons
        .iter()
        .filter(|(score, _)| Some(*score) == best_score)
        .collect::<Vec<_>>();
    if best.len() == 1 {
        best[0]
            .1
            .press()
            .map_err(|error| format!("{} macOS 发送按钮执行失败: {error}", target.agent.label()))?;
    } else if best.is_empty() {
        focus_composer(target)?;
        press_enter(target.agent, &target.app)?;
    } else {
        return Err(format!(
            "{} macOS 主输入框附近存在多个可用发送按钮",
            target.agent.label()
        ));
    }
    Ok(())
}

pub(super) fn submit_voice(state: &mut MacosComposerState, text: &str) -> Result<String, String> {
    update_voice(state, text)?;
    thread::sleep(Duration::from_millis(80));
    let target = find_voice_target(state)?;
    submit_target(&target)?;
    let deadline = Instant::now() + SUBMIT_CONFIRM_TIMEOUT;
    while Instant::now() < deadline {
        if let Ok(target) = find_voice_target(state) {
            if target.value.is_empty() {
                state.last_value.clear();
                return Ok(String::new());
            }
        }
        thread::sleep(Duration::from_millis(40));
    }
    let cleanup = find_voice_target(state).and_then(|target| replace_composer_text(&target, ""));
    state.last_value.clear();
    if let Err(error) = cleanup {
        return Err(format!(
            "ChatGPT（Codex）macOS 提交结果未确认，且无法清理语音草稿: {error}"
        ));
    }
    Err(format!(
        "{} macOS 提交结果未确认；为避免重复发送，本次不再尝试其他通道",
        state.agent.label()
    ))
}

pub(super) fn cancel_voice(state: &mut MacosComposerState) {
    if state.last_value.is_empty() {
        return;
    }
    if let Ok(target) = find_voice_target(state) {
        if target.value == state.last_value {
            let _ = replace_composer_text(&target, "");
        }
    }
    state.last_value.clear();
}

#[cfg(debug_assertions)]
pub(super) fn debug_probe_final_focus(state: &MacosComposerState) -> Result<(), String> {
    find_current_visible_target(state.agent).and_then(|target| focus_composer(&target))
}

#[cfg(debug_assertions)]
fn debug_attribute_value(element: &AXUIElement, name: &str) -> String {
    let attribute = AXAttribute::new(&CFString::new(name));
    let Ok(value): Result<CFType, _> = element.attribute(&attribute) else {
        return String::new();
    };
    if let Some(value) = value.downcast::<CFString>() {
        return value.to_string();
    }
    if let Some(value) = value.downcast::<CFBoolean>() {
        return bool::from(value).to_string();
    }
    if let Some(value) = value.downcast::<CFNumber>() {
        return value
            .to_i64()
            .map(|value| value.to_string())
            .unwrap_or_default();
    }
    if let Some(values) = value.downcast::<CFArray>() {
        return values
            .iter()
            .filter_map(|value| {
                let value = unsafe { CFType::wrap_under_get_rule(*value as _) };
                value.downcast::<CFString>().map(|value| value.to_string())
            })
            .collect::<Vec<_>>()
            .join(",");
    }
    String::new()
}

#[cfg(debug_assertions)]
pub(super) fn debug_dump_codex_tree() -> Result<Vec<String>, String> {
    let mut lines = Vec::new();
    for target in agent_windows(MacosAgent::Codex)? {
        let window_title = cf_string(target.window.title()).unwrap_or_default();
        let origin = ax_point_attribute(&target.window, "AXPosition").unwrap_or_default();
        let size = ax_size_attribute(&target.window, "AXSize").unwrap_or_default();
        lines.push(format!(
            "AXWindow | {} | x={} y={} width={} height={}",
            normalize_text(&window_title),
            origin.x,
            origin.y,
            size.width,
            size.height
        ));
        walk_elements(&target.window, |element| {
            if lines.len() >= 5_000 {
                return false;
            }
            let role = element_role(element);
            let labels = element_labels(element)
                .into_iter()
                .map(|label| label.chars().take(200).collect::<String>())
                .collect::<Vec<_>>();
            let attributes = element
                .attribute_names()
                .map(|names| {
                    names
                        .iter()
                        .map(|name| name.to_string())
                        .collect::<Vec<_>>()
                        .join(",")
                })
                .unwrap_or_default();
            lines.push(format!(
                "{} | {} | dom={} | classes={} | focused={} | chars={} | attrs={}",
                role,
                labels.join(" || "),
                debug_attribute_value(element, "AXDOMIdentifier"),
                debug_attribute_value(element, "AXDOMClassList"),
                debug_attribute_value(element, "AXFocused"),
                debug_attribute_value(element, "AXNumberOfCharacters"),
                attributes
            ));
            false
        });
    }
    Ok(lines)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn text_normalization_matches_windows_composer_contract() {
        assert_eq!(normalize_text("  hello\n world  "), "hello world");
    }

    #[test]
    fn chromium_accessibility_is_primed_once_per_running_process() {
        let mut primed_pids = HashSet::new();
        assert!(mark_agent_accessibility_primed(&mut primed_pids, 101));
        assert!(!mark_agent_accessibility_primed(&mut primed_pids, 101));
        assert!(mark_agent_accessibility_primed(&mut primed_pids, 202));
    }

    #[test]
    fn accessibility_settings_redirect_waits_for_an_activation_edge() {
        let mut initially_inactive = SettingsActivationGate::new(false);
        assert!(!initially_inactive.update(false));
        assert!(initially_inactive.update(true));

        let mut initially_active = SettingsActivationGate::new(true);
        assert!(!initially_active.update(true));
        assert!(!initially_active.update(false));
        assert!(initially_active.update(true));
    }

    #[test]
    fn pasteboard_plain_text_aliases_allow_equivalent_backup() {
        assert!(is_equivalent_plain_text_type(
            "public.utf16-external-plain-text"
        ));
        assert!(is_equivalent_plain_text_type("public.utf8-plain-text"));
        assert!(!is_equivalent_plain_text_type("public.rtf"));
        assert!(!is_equivalent_plain_text_type("public.file-url"));
    }

    #[test]
    fn focused_text_input_only_accepts_editable_terminal_roles() {
        assert!(focused_text_role_is_supported("AXTextArea"));
        assert!(focused_text_role_is_supported("AXTextField"));
        assert!(!focused_text_role_is_supported("AXStaticText"));
        assert!(!focused_text_role_is_supported("AXButton"));
    }

    #[test]
    fn focused_text_input_rejects_changed_process_or_control() {
        let captured = FocusedTextTarget {
            pid: 101,
            role: "AXTextArea".to_string(),
            subrole: String::new(),
            identifier: "terminal-input".to_string(),
            window_title: "MiMoCode".to_string(),
            element_bounds: Some((10, 20, 800, 500)),
            window_bounds: Some((0, 0, 900, 600)),
        };
        assert!(focused_text_target_matches(&captured, &captured));
        assert!(!focused_text_target_matches(
            &captured,
            &FocusedTextTarget {
                pid: 202,
                ..captured.clone()
            }
        ));
        assert!(!focused_text_target_matches(
            &captured,
            &FocusedTextTarget {
                identifier: "other-input".to_string(),
                ..captured.clone()
            }
        ));
        assert!(!focused_text_target_matches(
            &captured,
            &FocusedTextTarget {
                element_bounds: Some((20, 20, 800, 500)),
                ..captured.clone()
            }
        ));
    }

    #[test]
    fn primary_codex_window_excludes_pet_overlay_surfaces() {
        assert!(is_agent_primary_window_title(MacosAgent::Codex, "Codex"));
        assert!(is_agent_primary_window_title(MacosAgent::Codex, "ChatGPT"));
        assert!(!is_agent_primary_window_title(
            MacosAgent::Codex,
            "Codex Pet Composition Surface"
        ));
        assert!(!is_agent_primary_window_title(
            MacosAgent::Codex,
            "Codex Pet Mascot Effect"
        ));
        assert!(is_agent_primary_window_title(MacosAgent::Claude, "Claude"));
    }

    #[test]
    fn current_voice_launch_ids_cover_latest_agent_clients() {
        assert_eq!(
            agent_bundle_identifiers(MacosAgent::Codex),
            &["com.openai.codex", "com.openai.chat", "com.openai.chatgpt"]
        );
        assert_eq!(
            agent_bundle_identifiers(MacosAgent::Claude),
            &["com.anthropic.claudefordesktop", "com.anthropic.claude"]
        );
        assert_eq!(
            agent_application_names(MacosAgent::Codex),
            &["Codex", "ChatGPT"]
        );
        assert_eq!(agent_application_names(MacosAgent::Claude), &["Claude"]);
    }

    #[test]
    fn placeholder_detection_matches_codex_copy() {
        assert_eq!(normalize_text("Send"), "Send");
        assert!(is_placeholder("Ask anything"));
        assert!(is_placeholder("Do anything"));
        assert!(is_placeholder("Write your prompt to Claude"));
        assert!(is_placeholder("Type / for commands"));
        assert!(is_placeholder("Write a message..."));
        assert!(!is_placeholder("existing draft"));
    }

    #[test]
    fn send_button_geometry_accepts_only_composer_adjacent_controls() {
        let composer = (300, 800, 720, 96);
        assert!(send_button_bounds_match_composer(
            composer,
            (944, 824, 48, 48)
        ));
        assert!(send_button_bounds_match_composer(
            composer,
            (1008, 824, 48, 48)
        ));
        assert!(!send_button_bounds_match_composer(
            composer,
            (944, 360, 48, 48)
        ));
        assert!(!send_button_bounds_match_composer(
            composer,
            (80, 824, 48, 48)
        ));
    }

    #[test]
    fn codex_thread_deeplink_accepts_only_bounded_task_ids() {
        assert_eq!(
            thread_deeplink("019f83c9-2faa-7eb3-bb5c-5938538b93f0").unwrap(),
            "codex://threads/019f83c9-2faa-7eb3-bb5c-5938538b93f0"
        );
        assert!(thread_deeplink("").is_err());
        assert!(thread_deeplink("../../settings").is_err());
        assert!(thread_deeplink("thread?id=other").is_err());
    }

    #[test]
    fn claude_session_deeplink_accepts_only_claude_routes() {
        let code = "claude://code/11111111-2222-4333-8444-555555555555";
        let resume = "claude://resume?session=11111111-2222-4333-8444-555555555555";
        assert_eq!(
            session_deeplink(MacosAgent::Claude, "ignored", code).unwrap(),
            code
        );
        assert_eq!(
            session_deeplink(MacosAgent::Claude, "ignored", resume).unwrap(),
            resume
        );
        assert!(session_deeplink(MacosAgent::Claude, "ignored", "https://example.com").is_err());
    }

    #[test]
    fn claude_desktop_focus_marker_requires_forward_progress() {
        let before = ClaudeDesktopFocusMarker {
            path: PathBuf::from("/tmp/local_session.json"),
            last_focused_at: 100,
            modified_at: Some(SystemTime::UNIX_EPOCH + Duration::from_secs(1)),
        };
        let unchanged = before.clone();
        let advanced = ClaudeDesktopFocusMarker {
            last_focused_at: 101,
            ..before.clone()
        };
        assert!(!claude_focus_marker_advanced(Some(&before), &unchanged));
        assert!(claude_focus_marker_advanced(Some(&before), &advanced));
        assert!(claude_focus_marker_advanced(None, &before));
    }
}
