/*
 * [Input] A unique or current-visible ChatGPT（Codex）/Claude task, a pinned WorkBuddy composer, or a captured MiMoCode terminal caret, plus staged device speech and a later explicit Confirm action.
 * [Output] Read-only active-Agent detection, native consent with activation-ordered Accessibility-pane routing,
 *          activation-gated Chromium AX priming, AX-only stable/rebindable
 *          exact-session per-recording draft append with original-prefix preservation, including
 *          no-op-safe Claude focus confirmation across CLI/desktop metadata aliases, plus
 *          ID-deeplink-confirmed visible-composer recovery
 *          when a Codex build omits both the active title and sidebar row,
 *          nonempty-current-draft Confirm submission (Codex uses a unique native
 *          Send/Queue/Steer action first, never retries an attempted submission), and clipboard-preserving
 *          current-caret insertion separated from the later Return.
 * [Pos] macOS foreground-input backend for codex_composer.rs; requests Apple's native consent alert first and routes the activated System Settings window only after the user chooses to open it.
 * [Sync] If this file changes, update pc/.folder.md.
 */

#[cfg(test)]
#[path = "codex_voice_receipt.rs"]
mod voice_receipt;

#[path = "composer_visibility.rs"]
mod composer_visibility;

use accessibility::{AXAttribute, AXUIElement, AXUIElementActions, AXUIElementAttributes};
use accessibility_sys::{
    error_string, kAXErrorSuccess, kAXTrustedCheckOptionPrompt, kAXValueTypeCGPoint,
    kAXValueTypeCGSize, kAXValueTypeCFRange, AXValueCreate, AXIsProcessTrusted, AXIsProcessTrustedWithOptions, AXUIElementGetPid,
    AXUIElementPostKeyboardEvent, AXUIElementCopyElementAtPosition, AXUIElementRef, AXValueGetType, AXValueGetTypeID,
    AXValueGetValue, AXValueRef,
};
use core_foundation::number::CFNumber;
use core_foundation::{
    array::CFArray,
    base::{CFHash, CFType, CFRange, TCFType},
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
const MAX_CLAUDE_SESSION_METADATA_DEPTH: usize = 4;
const MAX_CLAUDE_SESSION_METADATA_BYTES: u64 = 2 * 1024 * 1024;
const SESSION_CONFIRM_TIMEOUT: Duration = Duration::from_millis(2_500);
const COMPOSER_READBACK_TIMEOUT: Duration = Duration::from_millis(500);
const SUBMIT_CONFIRM_TIMEOUT: Duration = Duration::from_millis(3_000);
const SUBMIT_FALLBACK_DELAY: Duration = Duration::from_millis(800);
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
    WorkBuddy,
}

impl MacosAgent {
    fn label(self) -> &'static str {
        match self {
            Self::Codex => "ChatGPT（Codex）",
            Self::Claude => "Claude",
            Self::WorkBuddy => "WorkBuddy",
        }
    }
}

pub(super) fn agent_is_frontmost(agent: MacosAgent) -> bool {
    for bundle_identifier in agent_bundle_identifiers(agent) {
        let applications = NSRunningApplication::runningApplicationsWithBundleIdentifier(
            &NSString::from_str(bundle_identifier),
        );
        if applications
            .iter()
            .any(|application| application.isActive())
        {
            return true;
        }
    }
    agent_application_pids(agent).into_iter().any(|pid| {
        NSRunningApplication::runningApplicationWithProcessIdentifier(pid)
            .is_some_and(|application| application.isActive())
    })
}

fn agent_bundle_identifiers(agent: MacosAgent) -> &'static [&'static str] {
    match agent {
        MacosAgent::Codex => &["com.openai.codex", "com.openai.chat", "com.openai.chatgpt"],
        MacosAgent::Claude => &["com.anthropic.claudefordesktop", "com.anthropic.claude"],
        MacosAgent::WorkBuddy => &["com.tencent.workbuddy.mac"],
    }
}

fn agent_application_names(agent: MacosAgent) -> &'static [&'static str] {
    match agent {
        MacosAgent::Codex => &["Codex", "ChatGPT"],
        MacosAgent::Claude => &["Claude"],
        MacosAgent::WorkBuddy => &["WorkBuddy"],
    }
}

pub(super) struct MacosComposerState {
    agent: MacosAgent,
    session_id: String,
    session_title: String,
    draft: crate::voice_draft::VoiceDraft,
    current_visible_target: Option<ComposerTarget>,
    task_identity: Option<Vec<String>>,
    native_voice_text: String,
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

// WorkBuddy must never emit global HID shortcuts: desktop screenshot tools can
// intercept them before the editor sees them. Use a private source and a pinned
// process destination; failure must not fall back to the global event stream.
fn post_workbuddy_key(app: &AXUIElement, keycode: u16, flags: CGEventFlags) -> Result<(), String> {
    let mut pid = 0;
    if unsafe { AXUIElementGetPid(app.as_concrete_TypeRef(), &mut pid) } != kAXErrorSuccess || pid <= 0 {
        return Err("无法确认 WorkBuddy 输入进程，未发送按键".into());
    }
    let source = CGEventSource::new(CGEventSourceStateID::Private)
        .map_err(|_| "无法创建 WorkBuddy 定向输入事件".to_string())?;
    for pressed in [true, false] {
        let event = CGEvent::new_keyboard_event(source.clone(), keycode, pressed)
            .map_err(|_| "无法创建 WorkBuddy 定向按键".to_string())?;
        event.set_flags(flags);
        event.post_to_pid(pid);
    }
    Ok(())
}

fn select_workbuddy_draft(composer: &AXUIElement) -> Result<(), String> {
    // AX ranges count UTF-16 code units, including the editor's leading BOM.
    // Setting selection does not bypass rich-editor input events like AXValue.
    let range = CFRange::init(0, element_value(composer).encode_utf16().count() as isize);
    let value = unsafe { AXValueCreate(kAXValueTypeCFRange, &range as *const CFRange as *const c_void) };
    if value.is_null() { return Err("无法创建 WorkBuddy 输入框选区".into()); }
    let value = unsafe { CFType::wrap_under_create_rule(value as _) };
    composer.set_attribute(&AXAttribute::new(&CFString::new("AXSelectedTextRange")), value)
        .map_err(|_| "WorkBuddy 输入框不支持安全选区，本次语音未写入".into())
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

pub(super) fn insert_at_focused_text_target(
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
    let result: Result<(), String> = (|| {
        paste_focused_text(text)?;
        Ok(())
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

pub(super) fn submit_at_focused_text_target(captured: &FocusedTextTarget) -> Result<(), String> {
    ensure_accessibility_permission()?;
    let current = capture_focused_text_target_unchecked()?;
    if !focused_text_target_matches(captured, &current) {
        return Err("MiMoCode 语音草稿写入后前台窗口或文本光标已变化，未执行确认键".to_string());
    }
    post_keyboard_event(KeyCode::RETURN, CGEventFlags::CGEventFlagNull)
        .map_err(|error| format!("MiMoCode 确认键发送失败: {error}"))
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
    if agent == MacosAgent::WorkBuddy {
        return Err("请在 WorkBuddy 中打开目标对话；不使用未验证的会话深链".into());
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

#[derive(Clone, Debug)]
struct ClaudeDesktopFocusSnapshot {
    target: Option<ClaudeDesktopFocusMarker>,
    target_is_latest: bool,
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

fn claude_metadata_timestamp(value: Option<&serde_json::Value>) -> u64 {
    value
        .and_then(|value| {
            value
                .as_u64()
                .or_else(|| value.as_f64().map(|number| number.max(0.0) as u64))
        })
        .unwrap_or_default()
}

fn claude_desktop_focus_snapshot_from_root(
    root: &std::path::Path,
    session_id: &str,
) -> ClaudeDesktopFocusSnapshot {
    let session_id = session_id.trim();
    if session_id.is_empty() {
        return ClaudeDesktopFocusSnapshot {
            target: None,
            target_is_latest: false,
        };
    }
    let mut target = None;
    let mut newest_last_focused_at = 0_u64;
    let mut pending = vec![(root.to_path_buf(), 0usize)];
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
                if depth < MAX_CLAUDE_SESSION_METADATA_DEPTH {
                    pending.push((path, depth + 1));
                }
                continue;
            }
            let file_name = entry.file_name();
            let file_name = file_name.to_string_lossy();
            if !file_type.is_file()
                || !file_name.starts_with("local_")
                || !file_name.ends_with(".json")
            {
                continue;
            }
            let Ok(metadata) = fs::metadata(&path) else {
                continue;
            };
            if metadata.len() > MAX_CLAUDE_SESSION_METADATA_BYTES {
                continue;
            }
            let Some(value) = fs::read(&path)
                .ok()
                .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(&bytes).ok())
            else {
                continue;
            };
            if value
                .get("isArchived")
                .and_then(serde_json::Value::as_bool)
                .unwrap_or(false)
            {
                continue;
            }
            let last_focused_at = claude_metadata_timestamp(value.get("lastFocusedAt"));
            newest_last_focused_at = newest_last_focused_at.max(last_focused_at);
            let file_session_id = file_name
                .strip_prefix("local_")
                .and_then(|value| value.strip_suffix(".json"))
                .unwrap_or_default();
            let cli_session_id = value
                .get("cliSessionId")
                .or_else(|| value.get("cli_session_id"))
                .and_then(serde_json::Value::as_str)
                .unwrap_or_default()
                .trim();
            let raw_desktop_session_id = value
                .get("sessionId")
                .or_else(|| value.get("session_id"))
                .and_then(serde_json::Value::as_str)
                .unwrap_or_default()
                .trim();
            let desktop_session_id = raw_desktop_session_id
                .strip_prefix("local_")
                .unwrap_or(raw_desktop_session_id);
            if !file_session_id.eq_ignore_ascii_case(session_id)
                && !cli_session_id.eq_ignore_ascii_case(session_id)
                && !desktop_session_id.eq_ignore_ascii_case(session_id)
            {
                continue;
            }
            let candidate = ClaudeDesktopFocusMarker {
                path,
                last_focused_at,
                modified_at: metadata.modified().ok(),
            };
            let replace = target
                .as_ref()
                .is_none_or(|current: &ClaudeDesktopFocusMarker| {
                    candidate.last_focused_at > current.last_focused_at
                        || (candidate.last_focused_at == current.last_focused_at
                            && candidate.modified_at > current.modified_at)
                });
            if replace {
                target = Some(candidate);
            }
        }
    }
    let target_is_latest = target.as_ref().is_some_and(|target| {
        target.last_focused_at > 0 && target.last_focused_at == newest_last_focused_at
    });
    ClaudeDesktopFocusSnapshot {
        target,
        target_is_latest,
    }
}

fn claude_desktop_focus_snapshot(session_id: &str) -> ClaudeDesktopFocusSnapshot {
    claude_desktop_sessions_root()
        .map(|root| claude_desktop_focus_snapshot_from_root(&root, session_id))
        .unwrap_or(ClaudeDesktopFocusSnapshot {
            target: None,
            target_is_latest: false,
        })
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

fn claude_focus_snapshot_confirms_session(
    before: &ClaudeDesktopFocusSnapshot,
    after: &ClaudeDesktopFocusSnapshot,
) -> bool {
    after.target.as_ref().is_some_and(|after_marker| {
        claude_focus_marker_advanced(before.target.as_ref(), after_marker)
    }) || (before.target.is_some() && before.target_is_latest && after.target_is_latest)
}

fn claude_session_is_latest_focused(session_id: &str) -> bool {
    claude_desktop_focus_snapshot(session_id).target_is_latest
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

fn record_agent_accessibility_primed(primed_pids: &mut HashSet<i32>, pid: i32, succeeded: bool) {
    if succeeded {
        primed_pids.insert(pid);
    }
}

fn enable_agent_accessibility(app: &AXUIElement) -> bool {
    let manual_accessibility = AXAttribute::new(&CFString::new("AXManualAccessibility"));
    let manual = app.set_attribute(&manual_accessibility, CFBoolean::true_value().into_CFType());
    let enhanced_accessibility = AXAttribute::new(&CFString::new("AXEnhancedUserInterface"));
    let enhanced = app.set_attribute(
        &enhanced_accessibility,
        CFBoolean::true_value().into_CFType(),
    );
    manual.is_ok() || enhanced.is_ok()
}

fn prime_agent_accessibility_once(app: &AXUIElement, pid: i32) -> bool {
    let mut primed_pids = primed_agent_application_pids()
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    if primed_pids.contains(&pid) {
        return false;
    }
    // A background/cold Electron process can reject the opt-in. Never cache
    // that failure as success for the lifetime of the process.
    let succeeded = enable_agent_accessibility(app);
    record_agent_accessibility_primed(&mut primed_pids, pid, succeeded);
    succeeded
}

fn should_recover_workbuddy_tree(agent: MacosAgent, count: usize, attempted: bool) -> bool {
    agent == MacosAgent::WorkBuddy && count == 0 && !attempted
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
        MacosAgent::WorkBuddy => title == "workbuddy",
    }
}

fn window_area(window: &AXUIElement) -> f64 {
    ax_size_attribute(window, "AXSize")
        .map(|size| size.width.max(0.0) * size.height.max(0.0))
        .unwrap_or_default()
}

fn primary_agent_window(agent: MacosAgent) -> Result<CodexWindow, String> {
    let windows = agent_windows(agent)?;
    if agent == MacosAgent::Codex {
        let metadata = windows.iter().map(|target| (
            cf_string(target.window.title())
                .is_some_and(|title| is_agent_primary_window_title(agent, &title)),
            ax_string_attribute(&target.window, "AXSubrole"),
            task_window_is_blocked(&target.window),
        )).collect::<Vec<_>>();
        let index = codex_primary_window_index(&metadata).map_err(|reason| match reason {
            "DESKTOP_TASK_MODAL_OPEN" => "ChatGPT（Codex）存在模态弹窗，请先处理弹窗后重试".to_string(),
            "DESKTOP_TASK_MULTIPLE_MAIN_WINDOWS" => "检测到多个 ChatGPT（Codex）主窗口，请保留目标主窗口后重试；尚未写入或发送文字".to_string(),
            _ => "ChatGPT（Codex）窗口可访问，但没有可确认的对话主窗口；请打开目标对话后重试".to_string(),
        })?;
        return Ok(windows[index].clone());
    }
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
    if agent == MacosAgent::WorkBuddy && candidates.len() != 1 {
        return Err("请保留一个 WorkBuddy 主窗口并打开目标对话，再重新开始语音输入".into());
    }
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

// The failed 0.1.138 path reached a nonempty AX window list, then discarded
// every window because none had an exact product-name title. A conversation
// title is not a window identity. Prefer a unique standard window, never a pet
// overlay, a modal, an editor in a utility dialog, or the largest arbitrary UI.
fn codex_primary_window_index(windows: &[(bool, String, bool)]) -> Result<usize, &'static str> {
    if windows.iter().any(|(_, _, blocked)| *blocked) {
        return Err("DESKTOP_TASK_MODAL_OPEN");
    }
    let standard = windows.iter().enumerate()
        .filter(|(_, (_, role, _))| role == "AXStandardWindow")
        .map(|(index, _)| index).collect::<Vec<_>>();
    match standard.as_slice() {
        [index] => return Ok(*index),
        [] => (),
        _ => return Err("DESKTOP_TASK_MULTIPLE_MAIN_WINDOWS"),
    }
    // Some older AX implementations omit the subrole. Keep their exact-name
    // compatibility only when unique; explicit AXDialog/utility roles never
    // become conversation windows even if titled "Codex" or "ChatGPT".
    let legacy = windows.iter().enumerate()
        .filter(|(_, (exact, role, _))| *exact && matches!(role.as_str(), "" | "AXUnknown"))
        .map(|(index, _)| index).collect::<Vec<_>>();
    match legacy.as_slice() {
        [index] => Ok(*index),
        [] => Err("DESKTOP_TASK_MAIN_WINDOW_MISSING"),
        _ => Err("DESKTOP_TASK_MULTIPLE_MAIN_WINDOWS"),
    }
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
    // A running app with an unrecognized/temporarily unavailable window is
    // not a missing app. Never relaunch it as a window-selection fallback.
    let launched = agent_application_pids(agent).is_empty();
    if launched { launch_agent_application(agent)?; }

    let deadline = Instant::now() + AGENT_LAUNCH_WINDOW_TIMEOUT;
    loop {
        let error = match primary_agent_window(agent) {
            Ok(target) => return Ok(target),
            Err(error) => error,
        };
        if Instant::now() >= deadline {
            return Err(format!(
                "{} macOS {}: {error}",
                agent.label(),
                if launched { "应用启动后目标窗口尚未就绪" } else { "应用已运行，但目标窗口尚未就绪" },
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
    // Do not repeatedly unminimize/activate/raise an already focused Electron
    // window: ASR updates and readback must not churn its rendering/focus state.
    if ax_element_attribute(&AXUIElement::system_wide(), "AXFocusedApplication")
        .as_ref() == Some(&target.app)
        && ax_element_attribute(&target.app, "AXFocusedWindow").as_ref() == Some(&target.window)
    {
        return Ok(());
    }
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
        let claude_focus_before =
            (agent == MacosAgent::Claude).then(|| claude_desktop_focus_snapshot(session_id));
        focus_session_deeplink(agent, session_id, deep_link)?;
        if agent != MacosAgent::Claude {
            return Ok(());
        }
        let claude_focus_before = claude_focus_before.unwrap_or(ClaudeDesktopFocusSnapshot {
            target: None,
            target_is_latest: false,
        });
        let session_title = normalize_text(session_title);
        let deadline = Instant::now() + SESSION_CONFIRM_TIMEOUT;
        while Instant::now() < deadline {
            let claude_focus_after = claude_desktop_focus_snapshot(session_id);
            if claude_focus_snapshot_confirms_session(&claude_focus_before, &claude_focus_after) {
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

fn released_composer_labels_match(role: &str, labels: &[String]) -> bool {
    if role != "AXTextArea" && role != "AXTextField" {
        return false;
    }
    let labels = labels.iter().map(|label| label.to_lowercase())
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

fn is_composer_element(element: &AXUIElement) -> bool {
    // Draft contents are not control metadata: saying "search" must not make
    // the real composer disappear, nor may prompt words turn another field
    // into an editor. Keep the old policy only in the comparison diagnostic.
    let role = element_role(element);
    if role != "AXTextArea" && role != "AXTextField" { return false; }
    let readonly = element.attribute(&AXAttribute::new(&CFString::new("AXReadOnly")))
        .ok().and_then(|value: CFType| value.downcast::<CFBoolean>())
        .map(bool::from).unwrap_or(false);
    current_composer_metadata_matches(&role, &composer_metadata(element),
        &composer_dom_metadata(element), readonly)
}

fn composer_dom_metadata(element: &AXUIElement) -> Vec<String> {
    let mut result = vec![ax_string_attribute(element,"AXDOMIdentifier"),
        ax_string_attribute(element,"AXSubrole")];
    let value: Result<CFType,_> = element.attribute(&AXAttribute::new(&CFString::new("AXDOMClassList")));
    if let Ok(value) = value { result.extend(string_attribute_values(value)); }
    result
}

fn string_attribute_values(value: CFType) -> Vec<String> {
    if let Some(text) = value.downcast::<CFString>() { return vec![text.to_string()]; }
    let mut result = Vec::new();
    if let Some(values) = value.downcast::<CFArray>() {
        for raw in values.get_all_values().into_iter().take(64).filter(|raw| !raw.is_null()) {
            let value = unsafe { CFType::wrap_under_get_rule(raw) };
            if let Some(text) = value.downcast::<CFString>() { result.push(text.to_string()); }
        }
    }
    result
}

fn current_composer_metadata_matches(role: &str, labels: &[String], dom: &[String], readonly: bool) -> bool {
    if readonly { return false; }
    // Negative metadata wins even if a search control also says "ask anything".
    // Never classify from draft contents. DOM tokens are bounded control hints,
    // not a substring rule that would reject names such as "research-composer".
    let search = labels.iter().any(|label| {
        let label = label.to_lowercase(); label.contains("search") || label.contains("搜索")
    }) || dom.iter().any(|hint| hint.to_lowercase()
        .split(|c: char| !c.is_alphanumeric()).any(|token|
            matches!(token,"search"|"searchbox"|"searchfield"|"searchinput"|"axsearchfield")));
    !search && released_composer_labels_match(role, labels)
}

fn composer_metadata(element: &AXUIElement) -> Vec<String> {
    [cf_string(element.title()), cf_string(element.description()),
        cf_string(element.help()), cf_string(element.placeholder_value())]
        .into_iter().flatten().collect()
}

fn composer_geometry_visible(hidden: bool, bounds: Option<(i64,i64,i64,i64)>, window: Option<(i64,i64,i64,i64)>) -> bool {
    if hidden { return false; }
    let Some((x,y,w,h)) = bounds else { return true; };
    if w <= 0 || h <= 0 { return false; }
    let Some((wx,wy,ww,wh)) = window.filter(|(_,_,w,h)| *w > 0 && *h > 0) else { return true; };
    x < wx.saturating_add(ww) && y < wy.saturating_add(wh)
        && x.saturating_add(w) > wx && y.saturating_add(h) > wy
}

fn composer_is_visible(element: &AXUIElement, window: &CodexWindow) -> bool {
    let hidden = element.attribute(&AXAttribute::new(&CFString::new("AXHidden")))
        .ok().and_then(|value: CFType| value.downcast::<CFBoolean>())
        .map(bool::from).unwrap_or(false);
    // Unsupported visibility metadata stays unknown; never infer hidden just
    // because an AX provider omits a property or uses negative screen origins.
    composer_geometry_visible(hidden, rounded_bounds(element), rounded_bounds(&window.window))
}

fn focused_snapshot_eligible(agent: MacosAgent, owns_window: bool, enabled: bool, metadata_match: bool, visible: bool) -> bool {
    agent == MacosAgent::Codex && owns_window && enabled && metadata_match && visible
}

fn focused_codex_composer(agent: MacosAgent, window: &CodexWindow) -> Option<(AXUIElement, String)> {
    if agent != MacosAgent::Codex { return None; }
    let focus = ax_element_attribute(&window.app, "AXFocusedUIElement")?;
    if !focused_snapshot_eligible(agent,
        ax_element_attribute(&focus, "AXWindow").as_ref() == Some(&window.window),
        element_is_enabled(&focus), is_composer_element(&focus),
        composer_is_visible(&focus, window)) { return None; }
    let value = composer_value(&focus);
    // Pin only an unchanged focus snapshot. A scan of thousands of nodes is
    // not a prerequisite for honoring the user's explicitly selected editor.
    if ax_element_attribute(&window.app, "AXFocusedUIElement").as_ref() != Some(&focus) { return None; }
    Some((focus, value))
}

fn composer_value(element: &AXUIElement) -> String {
    normalize_element_composer_value(element, &element_value(element))
}

fn normalize_element_composer_value(element: &AXUIElement, raw: &str) -> String {
    let placeholder = cf_string(element.placeholder_value());
    let characters = element.attribute(&AXAttribute::new(&CFString::new("AXNumberOfCharacters")))
        .ok().and_then(|value: CFType| value.downcast::<CFNumber>())
        .and_then(|value| value.to_i64());
    composer_value_with_placeholder(raw, placeholder.as_deref(), characters)
}

fn composer_value_with_placeholder(raw: &str, placeholder: Option<&str>, characters: Option<i64>) -> String {
    let value = raw.replace("\r\n", "\n");
    let value = value.trim_start_matches('\u{feff}');
    // Some AX providers expose the empty editor's hint as AXValue. Only
    // accept exact placeholder metadata, never length/suffix heuristics.
    // A positive character count is evidence of actual user text (including
    // text that happens to equal the hint), and must be preserved verbatim.
    if placeholder.is_some_and(|hint| !hint.is_empty()
        && value == hint.replace("\r\n", "\n")) && !characters.is_some_and(|n| n > 0) {
        return String::new();
    }
    if characters.is_some_and(|n| n > 0) { return value.to_string(); }
    normalize_composer_value(value)
}

fn normalize_composer_value(value: &str) -> String {
    let value = value.trim_start_matches('\u{feff}');
    if value.trim() == "今天帮你做些什么？ @ 引用对话文件，/ 调用技能与指令" {
        return String::new();
    }
    if is_placeholder(&value) {
        String::new()
    } else {
        value.to_string()
    }
}

fn workbuddy_selection_matches(selected: Option<&str>, expected: &str) -> bool {
    selected.is_some_and(|value| {
        let selected = normalize_composer_value(&value.replace("\r\n", "\n"));
        // WorkBuddy exposes HTML paragraph breaks in AXValue but omits them
        // from AXSelectedText (verified in its real multiline editor). Keep
        // spaces/characters exact, and still require exact full-value readback
        // after paste; do not treat an unavailable selection as empty.
        selected == expected || (!expected.replace('\n', "").is_empty()
            && selected.replace('\n', "") == expected.replace('\n', ""))
    })
}

fn composer_candidates(window: &CodexWindow) -> Vec<(AXUIElement, String)> {
    let mut composers = Vec::new();
    for composer in walk_elements(&window.window, is_composer_element) {
        if !element_is_enabled(&composer) {
            continue;
        }
        let value = composer_value(&composer);
        composers.push((composer, value));
    }
    composers
}

fn unique_composer(agent: MacosAgent, mut composers: Vec<(AXUIElement, String)>) -> Result<(AXUIElement, String), String> {
    if composers.len() != 1 {
        return Err(format!(
            "需要唯一的 {} macOS 输入框，实际找到 {} 个",
            agent.label(),
            composers.len()
        ));
    }
    Ok(composers.remove(0))
}

fn find_composer(agent: MacosAgent, window: &CodexWindow) -> Result<(AXUIElement, String), String> {
    if let Some(focused) = focused_codex_composer(agent, window) { return Ok(focused); }
    select_window_composer(agent, window, composer_candidates(window))
}

fn focused_composer_index(agent: MacosAgent, count: usize, focused: Option<usize>, owns_window: bool) -> Option<usize> {
    if agent == MacosAgent::Codex && count > 1 && owns_window {
        focused.filter(|index| *index < count)
    } else { None }
}

fn nearest_hit_text_control(window: &CodexWindow, point: (f32,f32)) -> Option<Option<AXUIElement>> {
    let mut raw: AXUIElementRef = std::ptr::null_mut();
    // Read-only hit test against the actually exposed screen, not a guessed
    // coordinate click. A covering window must not expose the editor below it.
    let status = unsafe { AXUIElementCopyElementAtPosition(
        AXUIElement::system_wide().as_concrete_TypeRef(), point.0, point.1, &mut raw) };
    if raw.is_null() { return None; }
    let mut hit = unsafe { AXUIElement::wrap_under_create_rule(raw) };
    if status != kAXErrorSuccess { return None; }
    let mut seen = Vec::new();
    for _ in 0..MAX_TREE_DEPTH {
        if seen.contains(&hit) { return None; }
        seen.push(hit.clone());
        if matches!(element_role(&hit).as_str(), "AXTextArea" | "AXTextField" | "AXSearchField") {
            return Some((ax_element_attribute(&hit,"AXWindow").as_ref() == Some(&window.window)).then_some(hit));
        }
        if hit == window.window { return Some(None); }
        match hit.parent() { Ok(parent) => hit = parent, Err(_) => return None }
    }
    None
}

fn candidate_exposure(window: &CodexWindow, candidate: &AXUIElement) -> composer_visibility::Exposure {
    let hits = composer_visibility::probe_points(rounded_bounds(candidate), rounded_bounds(&window.window))
        .into_iter().map(|point| nearest_hit_text_control(window, point)
            .map(|hit| hit.as_ref() == Some(candidate))).collect::<Vec<_>>();
    composer_visibility::exposure(&hits)
}

fn select_window_composer(agent: MacosAgent, window: &CodexWindow, mut candidates: Vec<(AXUIElement, String)>) -> Result<(AXUIElement, String), String> {
    if agent == MacosAgent::Codex {
        candidates.retain(|(element,_)| composer_is_visible(element, window));
    }
    let focused = ax_element_attribute(&window.app, "AXFocusedUIElement");
    let index = focused.as_ref().and_then(|focus| candidates.iter().position(|(element, _)| element == focus));
    // The user's selected editor can be nested more than twenty groups deep.
    // AXWindow gives direct ownership, without a guessed ancestor-depth limit.
    let owns_window = focused.as_ref().is_some_and(|focus|
        ax_element_attribute(focus, "AXWindow").as_ref() == Some(&window.window));
    if let Some(index) = focused_composer_index(agent, candidates.len(), index, owns_window) {
        return Ok(candidates.remove(index));
    }
    let mut exposures = Vec::new();
    let focus_role = focused.as_ref().map(element_role);
    let foreign_owner = focused.as_ref().and_then(|element| ax_element_attribute(element,"AXWindow"))
        .is_some_and(|owner| owner != window.window);
    let may_auto_focus = composer_visibility::may_auto_focus(
        ax_element_attribute(&AXUIElement::system_wide(),"AXFocusedApplication").as_ref() == Some(&window.app),
        ax_element_attribute(&window.app,"AXFocusedWindow").as_ref() == Some(&window.window),
        foreign_owner, focus_role.as_deref());
    if agent == MacosAgent::Codex && (2..=8).contains(&candidates.len()) && may_auto_focus
    {
        exposures = candidates.iter().map(|(element,_)| candidate_exposure(window, element)).collect();
        let selected=composer_visibility::sole_exposed(&exposures).or_else(||{
            let bounds=candidates.iter().map(|(e,_)|rounded_bounds(e)).collect::<Vec<_>>();
            let rich=candidates.iter().map(|(e,_)|composer_dom_metadata(e).iter().any(|v|
                v.split_whitespace().any(|token|token.eq_ignore_ascii_case("prosemirror")))).collect::<Vec<_>>();
            let chains=candidates.iter().map(|(e,_)|ancestors(e).into_iter().take(3).collect::<Vec<_>>()).collect::<Vec<_>>();
            let related=chains.iter().map(|left|chains.iter().map(|right|left.iter().any(|a|right.contains(a))).collect::<Vec<_>>()).collect::<Vec<_>>();
            composer_visibility::stacked_chat_composer(&bounds,&rich,&exposures,&related,rounded_bounds(&window.window))
        });
        if let Some(index) = selected {
            // Do not steal the user's focus if it changed during the probe.
            if ax_element_attribute(&window.app,"AXFocusedUIElement") == focused
                && ax_element_attribute(&AXUIElement::system_wide(),"AXFocusedApplication").as_ref() == Some(&window.app)
                && ax_element_attribute(&window.app,"AXFocusedWindow").as_ref() == Some(&window.window)
            {
                return Ok(candidates.remove(index));
            }
        }
    }
    if agent == MacosAgent::Codex && candidates.len() > 1 && CAPTURE_AMBIGUOUS_EVIDENCE.with(|enabled| enabled.get()) {
        AMBIGUOUS_COMPOSER_EVIDENCE.with(|slot| {
            let mut slot = slot.borrow_mut();
            if slot.is_none() {
                let elements = candidates.iter().map(|(element,_)| element.clone()).collect::<Vec<_>>();
                *slot = Some(serde_json::json!({"schema":1,"stage":"firstAmbiguousSelection",
                    "candidateCount":candidates.len(),"exposure":format!("{exposures:?}"),
                    "structure":candidate_structure(window, &elements)}));
            }
        });
    }
    unique_composer(agent, candidates).map_err(|error| {
        format!("{error}；定位诊断：焦点角色={}；焦点在候选中={}；焦点属于目标窗口={owns_window}；可见命中={exposures:?}",
            focused.as_ref().map(element_role).unwrap_or_else(|| "不可读".into()), index.is_some())
    })
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
    find_stable_current_target(agent, false)
}

fn legacy_codex_window_index(windows: &[(bool, f64)]) -> Option<usize> {
    // Exact copy of the released 135/138 Codex window-selection policy.
    windows.iter().enumerate().filter(|(_, (exact, _))| *exact)
        .max_by(|(_, (_, a)), (_, (_, b))| a.total_cmp(b)).map(|(index, _)| index)
}

thread_local! {
    // One bounded in-memory snapshot per lookup, on the same voice worker.
    // No file, telemetry, clipboard, draft, title or identity persistence.
    static AMBIGUOUS_COMPOSER_EVIDENCE: std::cell::RefCell<Option<serde_json::Value>> = const { std::cell::RefCell::new(None) };
    static CAPTURE_AMBIGUOUS_EVIDENCE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

struct ComposerEvidenceScope;
impl ComposerEvidenceScope {
    fn begin(enabled: bool) -> Self {
        AMBIGUOUS_COMPOSER_EVIDENCE.with(|slot| slot.borrow_mut().take());
        CAPTURE_AMBIGUOUS_EVIDENCE.with(|flag| flag.set(enabled));
        Self
    }
}
impl Drop for ComposerEvidenceScope {
    fn drop(&mut self) {
        CAPTURE_AMBIGUOUS_EVIDENCE.with(|flag| flag.set(false));
        AMBIGUOUS_COMPOSER_EVIDENCE.with(|slot| slot.borrow_mut().take());
    }
}

fn diagnostic_candidate_structure(window: &CodexWindow) -> serde_json::Value {
    let candidates = composer_candidates(window).into_iter().map(|(element,_)| element).collect::<Vec<_>>();
    candidate_structure(window, &candidates)
}

fn candidate_structure(window: &CodexWindow, all: &[AXUIElement]) -> serde_json::Value {
    // Geometry and relationships from the failed selection's candidates, never
    // labels, identifiers, draft values, clipboard contents or selection text.
    let truncated = all.len() > 8;
    let deadline = Instant::now() + Duration::from_millis(300);
    let candidates = all.iter().take(8).cloned().collect::<Vec<_>>();
    let window_bounds = rounded_bounds(&window.window);
    let chains = candidates.iter().map(|candidate| {
        let mut chain = vec![candidate.clone()];
        let mut current = candidate.clone();
        for _ in 0..MAX_TREE_DEPTH {
            if Instant::now() >= deadline { break; }
            let Ok(parent) = current.parent() else { break; };
            if chain.contains(&parent) { break; }
            chain.push(parent.clone());
            if parent == window.window { break; }
            current = parent;
        }
        chain
    }).collect::<Vec<_>>();
    let controls = candidates.iter().enumerate().map(|(index, candidate)| {
        let bounds = rounded_bounds(candidate);
        let relative = bounds.zip(window_bounds).map(|((x,y,w,h),(wx,wy,_,_))|
            (x.saturating_sub(wx), y.saturating_sub(wy), w, h));
        let hits = composer_visibility::probe_points(bounds, window_bounds).into_iter().map(|point| {
            if Instant::now() >= deadline { return serde_json::json!({"status":"notSampledBudget"}); }
            match nearest_hit_text_control(window, point) {
                None => serde_json::json!({"status":"unknown"}),
                Some(None) => serde_json::json!({"status":"notTextControl"}),
                Some(Some(hit)) => serde_json::json!({"status":"textControl",
                    "candidateIndex":candidates.iter().position(|item| item == &hit)}),
            }
        }).collect::<Vec<_>>();
        let dom = if Instant::now() < deadline { composer_dom_metadata(candidate).join(" ").to_lowercase() } else { String::new() };
        let kinds = ["composer","prompt","message","lexical","prosemirror","monaco","terminal","search"]
            .into_iter().filter(|kind| dom.contains(kind)).collect::<Vec<_>>();
        serde_json::json!({"index":index,"relativeBounds":relative,"role":element_role(candidate),"domKindHints":kinds,
            "ancestorCandidateIndices":candidates.iter().enumerate()
                .filter(|(other, element)| *other != index && chains[index].contains(element))
                .map(|(other,_)| other).collect::<Vec<_>>(),
            "ancestorRoles":chains[index].iter().skip(1).map(element_role).collect::<Vec<_>>(),
            "reachesWindow":chains[index].last() == Some(&window.window),
            "hitTests":hits})
    }).collect::<Vec<_>>();
    serde_json::json!({"truncated":truncated,"budgetExpired":Instant::now() >= deadline,"controls":controls,
        "windowSize":window_bounds.map(|(_,_,w,h)| (w,h)),
        "sharedAncestorDepths":chains.iter().map(|left| chains.iter().map(|right| {
            left.iter().enumerate().find_map(|(i,ancestor)|
                right.iter().position(|other| other == ancestor).map(|j| (i,j)))
        }).collect::<Vec<_>>()).collect::<Vec<_>>()})
}

fn diagnostic_composer_counts(window: &CodexWindow) -> serde_json::Value {
    let (mut nodes, mut web, mut areas, mut fields, mut matches, mut disabled, mut metadata_matches) = (0usize,0usize,0usize,0usize,0usize,0usize,0usize);
    let focused = ax_element_attribute(&window.app, "AXFocusedUIElement");
    let mut focused_in_scan = false;
    let mut text_controls = Vec::new();
    walk_elements(&window.window, |element| {
        nodes += 1;
        if focused.as_ref() == Some(element) { focused_in_scan = true; }
        let role = element_role(element);
        match role.as_str() { "AXWebArea" => web += 1, "AXTextArea" => areas += 1, "AXTextField" => fields += 1, _ => () }
        if matches!(role.as_str(), "AXTextArea" | "AXTextField") {
            let enabled = element_is_enabled(element);
            let original_match = released_composer_labels_match(&role, &element_labels(element));
            if original_match { if enabled { matches += 1; } else { disabled += 1; } }
            let labels = [cf_string(element.title()), cf_string(element.description()), cf_string(element.help()), cf_string(element.placeholder_value())]
                .into_iter().flatten().collect::<Vec<_>>();
            if enabled && is_composer_element(element) { metadata_matches += 1; }
            // Only categorical metadata is returned. Never return labels,
            // DOM identifiers, values, selected text or character counts.
            let mut settable = 0u8;
            let attribute = CFString::new("AXValue");
            let status = unsafe { accessibility_sys::AXUIElementIsAttributeSettable(
                element.as_concrete_TypeRef(), attribute.as_concrete_TypeRef(), &mut settable) };
            let dom = composer_dom_metadata(element).join(" ").to_lowercase();
            let kinds = ["composer", "prompt", "message", "lexical", "prosemirror", "monaco", "terminal", "search"]
                .into_iter().filter(|kind| dom.contains(kind)).collect::<Vec<_>>();
            text_controls.push(serde_json::json!({
                "ordinal":text_controls.len(), "role":role, "enabled":enabled,
                "releasedMatch":original_match,
                "recognizedPromptLabel":released_composer_labels_match("AXTextField", &labels),
                "searchMetadata":labels.iter().any(|s| { let s=s.to_lowercase(); s.contains("search") || s.contains("搜索") }),
                "valueSettable":if status == kAXErrorSuccess { Some(settable != 0) } else { None },
                "isFocusedElement":focused.as_ref() == Some(element),
                "directWindowMatches":ax_element_attribute(element,"AXWindow").as_ref() == Some(&window.window),
                "currentMetadataMatch":is_composer_element(element),
                "currentVisibilityEligible":composer_is_visible(element, window),
                "ancestorRoles":ancestors(element).iter().map(element_role).collect::<Vec<_>>(),
                "domKindHints":kinds
            }));
        }
        false
    });
    let focused_in_window = focused.as_ref().is_some_and(|e| e == &window.window || ancestors(e).iter().any(|a| a == &window.window));
    serde_json::json!({"nodes":nodes,"webAreas":web,"textAreas":areas,"textFields":fields,
        "releasedMatches":matches,"disabledMatches":disabled,"metadataOnlyMatches":metadata_matches,
        "nodeLimitReached":nodes>=MAX_TREE_ELEMENTS,"focusedRole":focused.as_ref().map(element_role),"focusedInWindow":focused_in_window,
        "focusedInScan":focused_in_scan,"textControls":text_controls,
        "candidateStructure":diagnostic_candidate_structure(window)})
}

pub(super) fn diagnose_current_composer(agent: MacosAgent) -> Result<serde_json::Value, String> {
    // Invoked explicitly by the user in the diagnostic UI. No text/labels,
    // titles, paths or IDs leave this function; counts and roles only.
    if agent != MacosAgent::Codex { return Err("此次 135/142 同环境对照仅检查 Codex".into()); }
    if !accessibility_permission_granted() { return Err("辅助功能尚未授权；本次未请求新权限、未读取输入框".into()); }
    let windows = agent_windows(agent)?;
    let metadata = windows.iter().map(|w| (
        cf_string(w.window.title()).is_some_and(|title| is_agent_primary_window_title(agent, &title)),
        ax_string_attribute(&w.window,"AXSubrole"), task_window_is_blocked(&w.window),
    )).collect::<Vec<_>>();
    let legacy = legacy_codex_window_index(&windows.iter().zip(&metadata).map(|(w,(exact,_,_))| (*exact,window_area(&w.window))).collect::<Vec<_>>());
    let current = codex_primary_window_index(&metadata);
    let mut snapshots = Vec::new();
    for index in [legacy, current.as_ref().ok().copied()].into_iter().flatten() {
        if snapshots.iter().any(|value: &serde_json::Value| value["index"].as_u64() == Some(index as u64)) { continue; }
        snapshots.push(serde_json::json!({"index":index,"beforeFocus":diagnostic_composer_counts(&windows[index])}));
    }
    let mut focus_error: Option<String> = None;
    let after_focus = if let Ok(index) = current.as_ref() {
        let target = &windows[*index];
        match focus_window(target) {
            Ok(()) => {
                thread::sleep(COMPOSER_STABILITY_DELAY);
                Some(diagnostic_composer_counts(target))
            },
            Err(error) => { focus_error = Some(error); None },
        }
    } else { None };
    let selection = current.as_ref().ok().map(|index| {
        // Same read-only resolver as actual voice input, not just a count of
        // the legacy classifier's candidates. Never focuses/writes a textbox.
        match find_composer(agent, &windows[*index]) {
            Ok(_) => serde_json::json!({"ok":true}),
            Err(error) => serde_json::json!({"ok":false,"error":error}),
        }
    });
    Ok(serde_json::json!({"schema":4,"baseline":"135/138 released policy","current":"focused-owned-visible composer",
        "windows":metadata.iter().enumerate().map(|(i,(exact,role,blocked))| serde_json::json!({"index":i,"exactProductTitle":exact,"subrole":role,"blocked":blocked})).collect::<Vec<_>>(),
        "legacyWindow":legacy,"currentWindow":current.as_ref().ok(),"currentSelectionError":current.err(),
        "snapshots":snapshots,"afterFocus":after_focus,"focusError":focus_error,"voiceSelection":selection,
        "safety":{"draftWrites":0,"submissions":0,"microphone":false,"modelCalls":0,"forcedRecovery":false,"sessionNavigation":false}}))
}

fn find_stable_current_target(agent: MacosAgent, current_task: bool) -> Result<ComposerTarget, String> {
    let _evidence_scope = ComposerEvidenceScope::begin(agent == MacosAgent::Codex);
    // Tasks share voice input's cold-webview recovery, but must never launch,
    // select another window, or bypass a modal while waiting for the editor.
    let mut target = if current_task {
        current_task_window_for(agent)?
    } else {
        primary_or_launch_agent_window(agent)?
    };
    let pinned_window = target.window.clone();
    // Activation completion is not editor-focus completion. The diagnostic
    // already waited here; voice input must use the same settling interval.
    focus_window(&target)?;
    thread::sleep(COMPOSER_STABILITY_DELAY);
    let deadline = Instant::now() + COMPOSER_STABILITY_TIMEOUT;
    let mut recovery_attempted = false;
    loop {
        if current_task {
            let current = current_task_window_for(agent)?;
            if current.window != pinned_window {
                return Err("DESKTOP_TASK_TARGET_CHANGED".into());
            }
            target = current;
        } else if let Ok(current) = primary_agent_window(agent) {
            if current.window != pinned_window {
                return Err("语音定位期间目标窗口已变化，未写入文字，请在目标对话重新长按".into());
            }
            target = current;
        }
        let result = (|| {
            focus_window(&target)?;
            // Task handoff and push-to-talk choose the same editor: honor the
            // owned visible focus first, otherwise resolve the exposed bottom
            // chat composer. Task window/modal and pinned-target checks remain
            // independent of editor selection and must not be bypassed.
            let focused = focused_codex_composer(agent, &target);
            let (composer, value) = if let Some(focused) = focused {
                focused
            } else {
              let candidates = composer_candidates(&target);
              if should_recover_workbuddy_tree(agent, candidates.len(), recovery_attempted) {
                recovery_attempted = true;
                // One foreground recovery per recording, not every scan: a
                // cached PID does not prove a newly loaded webview is ready.
                // Do not reinitialize a healthy or ambiguous editor, send keys,
                // select another conversation, or write until stability checks pass.
                let enabled = enable_agent_accessibility(&target.app);
                eprintln!("[composer] WorkBuddy empty tree: foreground recovery accepted={enabled}");
                thread::sleep(COMPOSER_STABILITY_DELAY);
                return Err("WorkBuddy 输入框尚未就绪，正在恢复辅助功能树".to_string());
              }
              select_window_composer(agent, &target, candidates)?
            };
            let candidate = ComposerTarget {
                agent,
                app: target.app.clone(),
                window: target.window.clone(),
                composer: composer.clone(),
                value,
            };
            focus_composer(&candidate)?;
            thread::sleep(COMPOSER_STABILITY_DELAY);
            let refreshed = if current_task {
                current_task_window_for(agent)?
            } else {
                primary_agent_window(agent)?
            };
            if refreshed.window != target.window {
                return Err(format!("{} macOS 主窗口仍在恢复", agent.label()));
            }
            let (refreshed_composer, refreshed_value) = find_composer(agent, &refreshed)?;
            if refreshed_composer != composer {
                return Err(format!("{} macOS 输入框仍在恢复", agent.label()));
            }
            if refreshed_value != candidate.value {
                return Err(format!("{} macOS 输入框内容仍在变化", agent.label()));
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
            Ok(target) => {
                AMBIGUOUS_COMPOSER_EVIDENCE.with(|slot| slot.borrow_mut().take());
                return Ok(target);
            },
            Err(error) => error,
        };
        if Instant::now() >= deadline {
            let evidence = AMBIGUOUS_COMPOSER_EVIDENCE.with(|slot| slot.borrow_mut().take());
            let details = evidence.map(|value| format!("\n[COMPOSER_EVIDENCE]{}[/COMPOSER_EVIDENCE]",value))
                .unwrap_or_default();
            return Err(format!(
                "{} macOS 应用已置前，但输入框未稳定: {last_error}；尚未写入或发送文字。失败现场已保留时可展开定位详情。{details}",
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
    if let Some(identity) = &state.task_identity {
        let current = current_task_window_for(state.agent)?;
        if current.window != pinned_window || task_window_identity(&current.window) != *identity {
            return Err("DESKTOP_TASK_TARGET_CHANGED".into());
        }
    }
    let Some(window) = agent_windows(state.agent)?
        .into_iter()
        .find(|candidate| candidate.window == pinned_window)
    else {
        return Err(format!(
            "{} macOS 当前窗口在语音输入期间发生变化",
            state.agent.label()
        ));
    };
    if state.agent == MacosAgent::WorkBuddy {
        if !agent_is_frontmost(state.agent) {
            return Err("WorkBuddy 已不在前台，已停止写入，请回到目标对话重新长按".into());
        }
    } else {
        focus_window(&window)?;
    }
    let (composer, value) = find_composer(state.agent, &window)?;
    if composer != pinned_composer {
        if state.task_identity.is_some() {
            return Err("DESKTOP_TASK_TARGET_CHANGED".into());
        }
        if state.agent == MacosAgent::WorkBuddy {
            return Err("WorkBuddy 输入框已变化，已停止写入并保留草稿，请在目标对话重新长按".into());
        }
        if state.agent == MacosAgent::Codex {
            return Err("Codex 输入框已变化，已停止写入并保留草稿，请点击目标输入框后重新长按".into());
        }
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
    if ax_element_attribute(&target.app, "AXFocusedUIElement").as_ref() == Some(&target.composer) {
        return Ok(());
    }
    target
        .composer
        .set_attribute(&AXAttribute::focused(), CFBoolean::true_value())
        .map_err(|error| format!("{} macOS 输入框无法获得焦点: {error}", target.agent.label()))
}

fn replace_composer_text(target: &ComposerTarget, text: &str) -> Result<(), String> {
    let text = text.replace("\r\n", "\n");
    focus_composer(target)?;
    if composer_value(&target.composer) != target.value {
        return Err("输入框内容在获取焦点后发生变化，已保留原文".into());
    }
    if target.agent == MacosAgent::WorkBuddy {
        // Its rich-text editor can expose writable AXValue while bypassing the
        // editor's input events. Use a guarded native paste, never mutate AXValue.
        let snapshot = PasteboardSnapshot::capture()?;
        let result = (|| {
            let assert_focus = || -> Result<(), String> {
                if !agent_is_frontmost(target.agent)
                    || ax_element_attribute(&target.app, "AXFocusedUIElement").as_ref() != Some(&target.composer)
                    || composer_value(&target.composer) != target.value
                {
                    return Err("WorkBuddy 焦点或草稿已变化，已停止写入".into());
                }
                Ok(())
            };
            assert_focus()?;
            let mut used_targeted_select = select_workbuddy_draft(&target.composer).is_err();
            if used_targeted_select {
                post_workbuddy_key(&target.app, 0, CGEventFlags::CGEventFlagCommand)?;
            }
            // Verify the editor applied its AX selection before native paste.
            let mut selection_deadline = Instant::now() + COMPOSER_READBACK_TIMEOUT;
            let mut selection_stable_since = None;
            loop {
                assert_focus()?;
                let selected = target.composer
                    .attribute(&AXAttribute::new(&CFString::new("AXSelectedText")))
                    .ok()
                    .and_then(|value: CFType| value.downcast::<CFString>())
                    .map(|value| value.to_string());
                if workbuddy_selection_matches(selected.as_deref(), &target.value) {
                    // Give Chromium time to commit its selection before another
                    // shortcut; one optimistic AX snapshot is not sufficient.
                    if selection_stable_since.get_or_insert_with(Instant::now).elapsed()
                        >= KEYBOARD_READBACK_DELAY
                    {
                        break;
                    }
                } else {
                    selection_stable_since = None;
                }
                if Instant::now() >= selection_deadline {
                    if !used_targeted_select {
                        // Slate sometimes ignores AX ranges for multiline/emoji
                        // drafts. Select inside this PID only, never via HID.
                        post_workbuddy_key(&target.app, 0, CGEventFlags::CGEventFlagCommand)?;
                        used_targeted_select = true;
                        selection_stable_since = None;
                        selection_deadline = Instant::now() + COMPOSER_READBACK_TIMEOUT;
                        continue;
                    }
                    return Err("WorkBuddy 未确认选中输入框原文，本次语音未粘贴，请重新长按".into());
                }
                thread::sleep(Duration::from_millis(25));
            }
            assert_focus()?;
            if text.is_empty() {
                post_workbuddy_key(&target.app, 51, CGEventFlags::CGEventFlagNull)?;
                thread::sleep(KEYBOARD_READBACK_DELAY);
            } else {
                set_pasteboard_text(&text)?;
                assert_focus()?;
                post_workbuddy_key(&target.app, MAC_KEYCODE_V, CGEventFlags::CGEventFlagCommand)?;
                thread::sleep(KEYBOARD_READBACK_DELAY);
            }
            let deadline = Instant::now() + Duration::from_secs(2);
            while Instant::now() < deadline {
                if composer_value(&target.composer) == text {
                    return Ok::<(), String>(());
                }
                thread::sleep(Duration::from_millis(25));
            }
            Err("WorkBuddy 未确认语音文字写入；已停止操作，请检查当前草稿".into())
        })();
        snapshot.restore();
        result?;
    } else {
        target
            .composer
            .set_attribute(&AXAttribute::value(), CFString::new(&text).into_CFType())
            .map_err(|error| format!("{} macOS 输入框写入失败: {error}", target.agent.label()))?;
    }
    let deadline = Instant::now() + COMPOSER_READBACK_TIMEOUT;
    while Instant::now() < deadline {
        if composer_value(&target.composer) == text {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(25));
    }
    if target.agent == MacosAgent::Codex {
        let actual=composer_value(&target.composer);
        crate::realtime_chat_log::record("", "task_composer", serde_json::json!({
            "stage":"write_readback","state":if actual.trim_end_matches('\n')==text.trim_end_matches('\n') {
                "trailing_newline_only"
            } else { "different" },"ok":false}));
    }
    Err(format!(
        "{} macOS 输入框没有确认语音文本",
        target.agent.label()
    ))
}

fn voice_draft_at_target(target: &ComposerTarget) -> Result<crate::voice_draft::VoiceDraft, String> {
    if target.agent != MacosAgent::Codex {
        return Ok(crate::voice_draft::VoiceDraft::new(target.value.clone()));
    }
    let attribute = AXAttribute::new(&CFString::new("AXSelectedTextRange"));
    let value: CFType = target.composer.attribute(&attribute)
        .map_err(|_| "无法读取 Codex 光标位置，请点击输入框后重新长按".to_string())?;
    if value.type_of() != unsafe { AXValueGetTypeID() } {
        return Err("Codex 未提供有效光标位置，尚未写入".into());
    }
    let value = value.as_CFTypeRef() as AXValueRef;
    let mut range = CFRange::init(0, 0);
    if unsafe { AXValueGetType(value) } != kAXValueTypeCFRange
        || !unsafe { AXValueGetValue(value, kAXValueTypeCFRange, &mut range as *mut CFRange as *mut c_void) }
        || range.location < 0 || range.length != 0 {
        return Err("请将光标放在要插入语音的位置，取消文字选中后重新长按".into());
    }
    let raw = element_value(&target.composer);
    let units = raw.encode_utf16().collect::<Vec<_>>();
    let prefix = units.get(..range.location as usize)
        .and_then(|units| String::from_utf16(units).ok())
        .ok_or("Codex 光标位置已变化，尚未写入")?;
    if normalize_element_composer_value(&target.composer, &raw) != target.value {
        return Err("输入框内容在读取光标时发生变化，已保留原文".into());
    }
    let offset = if target.value.is_empty() { 0 } else {
        prefix.trim_start_matches('\u{feff}').replace("\r\n", "\n").encode_utf16().count()
    };
    crate::voice_draft::VoiceDraft::at_utf16_caret(target.value.clone(), offset)
}

// Verify only this recording's inserted text at the resulting caret. The
// pre-existing editor value may be a visual hint; it is not a submission gate.
// UTF-16 is used solely for the native caret's coordinate system, not to compare
// expected/actual draft lengths. No full-document reconstruction or overwrite.
fn codex_base_without_inserted_utterance(actual: &str, transcript: &str, caret: Option<usize>) -> Option<String> {
    let text = transcript.trim();
    if text.is_empty() { return None; }
    let end = caret?;
    let actual = actual.encode_utf16().collect::<Vec<_>>();
    let inserted = text.encode_utf16().collect::<Vec<_>>();
    let start = end.checked_sub(inserted.len())?;
    if actual.get(start..end) != Some(inserted.as_slice()) { return None; }
    String::from_utf16(&[&actual[..start], &actual[end..]].concat()).ok()
}

fn codex_inserted_utterance_present(actual: &str, transcript: &str, caret: Option<usize>) -> bool {
    codex_base_without_inserted_utterance(actual, transcript, caret).is_some()
}

#[derive(Debug, PartialEq, Eq)]
struct VoiceEdit { start: usize, replace: String, insert: String }

fn voice_edit(previous: &str, next: &str, origin: usize) -> VoiceEdit {
    match next.strip_prefix(previous) {
        Some(suffix) => VoiceEdit { start: origin + previous.encode_utf16().count(), replace: String::new(), insert: suffix.into() },
        None => VoiceEdit { start: origin, replace: previous.into(), insert: next.into() },
    }
}

fn voice_edit_without_ack(previous: &str, next: &str, origin: usize, actual: &str, caret: Option<usize>) -> VoiceEdit {
    let mut edit = voice_edit(previous, next, origin);
    if !edit.replace.is_empty() {
        if codex_inserted_utterance_present(actual, previous, caret) {
            edit.start = caret.unwrap() - previous.encode_utf16().count();
        } else {
            // Unknown earlier write: insert, never overwrite an unowned range.
            edit.replace.clear();
        }
    }
    edit
}

fn select_codex_voice_revision(target: &ComposerTarget, edit: &VoiceEdit) -> Result<(), String> {
    if edit.replace.is_empty() { return Ok(()); }
    // Select only text owned by this recording. Never select-all or overwrite
    // AXValue. Verify both the exact selected text and native range before paste.
    let raw = element_value(&target.composer);
    let leading = raw.chars().take_while(|c| *c == '\u{feff}').count();
    let range = CFRange::init((edit.start + leading) as isize, edit.replace.encode_utf16().count() as isize);
    let ptr = unsafe { AXValueCreate(kAXValueTypeCFRange, &range as *const CFRange as *const c_void) };
    if ptr.is_null() { return Err("无法定位本次语音的修订片段".into()); }
    let value = unsafe { CFType::wrap_under_create_rule(ptr as _) };
    target.composer.set_attribute(&AXAttribute::new(&CFString::new("AXSelectedTextRange")), value)
        .map_err(|_| "输入框无法选择本次语音片段，已保留现有文字".to_string())?;
    let deadline = Instant::now() + Duration::from_millis(300);
    loop {
        let selection: Option<CFType> = target.composer.attribute(&AXAttribute::new(&CFString::new("AXSelectedTextRange"))).ok();
        let mut read = CFRange::init(-1, -1);
        let range_ok = selection.as_ref().is_some_and(|value| value.type_of() == unsafe { AXValueGetTypeID() }
            && unsafe { AXValueGetValue(value.as_CFTypeRef() as AXValueRef, kAXValueTypeCFRange, &mut read as *mut CFRange as *mut c_void) }
            && read.location == range.location && read.length == range.length);
        let selected = target.composer.attribute(&AXAttribute::new(&CFString::new("AXSelectedText"))).ok()
            .and_then(|value: CFType| value.downcast::<CFString>());
        let text_ok = selected.is_some_and(|text| text.to_string() == edit.replace);
        if range_ok && text_ok { return Ok(()); }
        if Instant::now() >= deadline { return Err("未确认本次语音修订选区，未替换或重复粘贴".into()); }
        thread::sleep(Duration::from_millis(15));
    }
}

fn post_codex_paste(app: &AXUIElement) -> Result<(), String> {
    post_codex_command(app, MAC_KEYCODE_V)
}

fn post_codex_command(app: &AXUIElement, keycode: u16) -> Result<(), String> {
    let mut pid = 0;
    if unsafe { AXUIElementGetPid(app.as_concrete_TypeRef(), &mut pid) } != kAXErrorSuccess || pid <= 0 {
        return Err("无法确认 Codex 输入进程，本次未粘贴".into());
    }
    let source = CGEventSource::new(CGEventSourceStateID::Private).map_err(|_| "无法创建 Codex 输入事件".to_string())?;
    let down = CGEvent::new_keyboard_event(source.clone(), keycode, true).map_err(|_| "无法创建 Codex 输入事件".to_string())?;
    let up = CGEvent::new_keyboard_event(source, keycode, false).map_err(|_| "无法创建 Codex 输入事件".to_string())?;
    down.set_flags(CGEventFlags::CGEventFlagCommand); up.set_flags(CGEventFlags::CGEventFlagCommand);
    down.post_to_pid(pid); up.post_to_pid(pid);
    Ok(())
}

fn insert_codex_voice_once(state: &mut MacosComposerState, transcript: &str) -> Result<String, String> {
    let transcript = transcript.trim();
    // Track dispatched ASR text, not AX readback acknowledgements. An unchanged
    // partial/final event is a no-op; a new hypothesis must never be locked out
    // by an earlier editor acknowledgement that was late or unavailable.
    if transcript.is_empty() || transcript == state.native_voice_text {
        return Ok(state.draft.last.clone());
    }
    let target = find_voice_target(state)?;
    let origin = state.draft.caret_after("").ok_or("语音插入位置不可用")?;
    let caret = voice_draft_at_target(&target).ok().and_then(|draft| draft.caret_after(""));
    let edit = voice_edit_without_ack(&state.native_voice_text, transcript, origin, &target.value, caret);
    let snapshot = PasteboardSnapshot::capture()?;
    let result = (|| {
        if !agent_is_frontmost(MacosAgent::Codex)
            || ax_element_attribute(&target.app, "AXFocusedUIElement").as_ref() != Some(&target.composer) {
            return Err("Codex 焦点已变化，本次未粘贴".into());
        }
        // Only replacing an earlier hypothesis needs an ownership check. A
        // missing/stale AX value must not prevent fresh ASR from being inserted.
        // If ownership cannot be established, append the new hypothesis at the
        // current caret instead of deleting any possibly user-owned text.
        if !edit.replace.is_empty() {
            select_codex_voice_revision(&target, &edit)?;
        }
        set_pasteboard_text(&edit.insert)?;
        if !agent_is_frontmost(MacosAgent::Codex)
            || ax_element_attribute(&target.app,"AXFocusedUIElement").as_ref()!=Some(&target.composer) {
            return Err("语音修订期间焦点已变化，未粘贴".into());
        }
        post_codex_paste(&target.app)?;
        state.native_voice_text = transcript.into();
        state.draft.last = state.draft.desired(transcript);
        // Let the target consume the clipboard before restoring it. This is a
        // fixed transport delay, not a content/caret/receipt confirmation gate.
        thread::sleep(KEYBOARD_READBACK_DELAY);
        Ok(state.draft.last.clone())
    })();
    snapshot.restore();
    result
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
        draft: voice_draft_at_target(&target)?,
        current_visible_target: pin_visible_target.then_some(target),
        task_identity: None,
        native_voice_text: String::new(),
    })
}

pub(super) fn begin_current_voice(agent: MacosAgent) -> Result<MacosComposerState, String> {
    let target = find_current_visible_target(agent)?;
    focus_composer(&target)?;
    Ok(MacosComposerState {
        agent,
        session_id: String::new(),
        session_title: String::new(),
        draft: voice_draft_at_target(&target)?,
        current_visible_target: Some(target),
        task_identity: None,
        native_voice_text: String::new(),
    })
}

fn task_window_identity(window: &AXUIElement) -> Vec<String> {
    let mut labels = vec![cf_string(window.title()).unwrap_or_default()];
    for element in walk_elements(window, |e| element_role(e) == "AXHeading") {
        labels.extend(element_labels(&element));
    }
    labels
}

fn current_task_window_index(windows: &[(String, bool)]) -> Result<usize, &'static str> {
    if windows.iter().any(|(_, blocked)| *blocked) {
        return Err("DESKTOP_TASK_MODAL_OPEN");
    }
    // Codex also exposes nonmodal AXDialog utility windows with editable
    // textareas. They are not conversations, even when larger than the main
    // window. Neither editor count, title, area nor AXMain can disambiguate
    // multiple real conversation windows safely.
    let mut standard = windows.iter().enumerate()
        .filter(|(_, (subrole, _))| subrole == "AXStandardWindow");
    let index = standard.next().map(|(index, _)| index)
        .ok_or("DESKTOP_TASK_MAIN_WINDOW_MISSING")?;
    if standard.next().is_some() {
        return Err("DESKTOP_TASK_MULTIPLE_MAIN_WINDOWS");
    }
    Ok(index)
}

fn task_window_is_blocked(window: &AXUIElement) -> bool {
    let modal: Result<CFType, _> = window.attribute(&AXAttribute::new(&CFString::new("AXModal")));
    let sheets: Result<CFType, _> = window.attribute(&AXAttribute::new(&CFString::new("AXSheets")));
    modal.ok().and_then(|v| v.downcast::<CFBoolean>()).is_some_and(bool::from)
        || sheets.ok().and_then(|v| v.downcast::<CFArray>()).is_some_and(|v| v.len() > 0)
}

fn current_task_window() -> Result<CodexWindow, String> {
    current_task_window_for(MacosAgent::Codex)
}
fn current_task_window_for(agent: MacosAgent) -> Result<CodexWindow, String> {
    // Never launch, navigate, or choose a most-recent thread/window here.
    let windows = agent_windows(agent)?;
    let metadata = windows.iter().map(|w| (
        ax_string_attribute(&w.window, "AXSubrole"), task_window_is_blocked(&w.window),
    )).collect::<Vec<_>>();
    let index = current_task_window_index(&metadata).map_err(str::to_owned)?;
    Ok(windows[index].clone())
}

pub(super) fn begin_current_task() -> Result<MacosComposerState, String> {
    begin_current_task_for(MacosAgent::Codex)
}
pub(super) fn begin_current_task_for(agent: MacosAgent) -> Result<MacosComposerState, String> {
    let target = find_stable_current_target(agent, true)?;
    let identity = task_window_identity(&target.window);
    Ok(MacosComposerState { agent, session_id: String::new(),
        session_title: String::new(), draft: crate::voice_draft::VoiceDraft::new(target.value.clone()),
        current_visible_target: Some(target), task_identity: Some(identity), native_voice_text: String::new() })
}

pub(super) fn update_voice(state: &mut MacosComposerState, text: &str) -> Result<String, String> {
    if state.agent == MacosAgent::Codex && state.draft.caret_after(text).is_some() {
        return insert_codex_voice_once(state, text);
    }
    let text = state.draft.desired(text);
    let mut last_error = String::new();
    // An uncertain native paste must never be repeated: a delayed first paste
    // could otherwise duplicate the utterance on top of the retry.
    let attempts = if state.agent == MacosAgent::WorkBuddy { 1 } else { 2 };
    for _ in 0..attempts {
        match find_voice_target(state).and_then(|target| {
            if !state.draft.can_update(&target.value, &text) {
                return Err("输入框已被手动修改，已停止更新本段语音以保留原文".into());
            }
            if target.value != text { replace_composer_text(&target, &text)?; }
            Ok(target)
        }) {
            Ok(_target) => {
                state.draft.last = text.clone();
                return Ok(text);
            }
            Err(error) => {
                last_error = error;
                thread::sleep(Duration::from_millis(40));
            }
        }
    }
    Err(format!(
        "{} macOS 前台语音更新失败: {last_error}",
        state.agent.label()
    ))
}

fn send_button_label_score(labels: &[String]) -> Option<u8> {
    let labels = labels
        .iter()
        .map(|label| label.to_lowercase())
        .collect::<Vec<_>>();
    if labels
        .iter()
        .any(|label| matches!(label.as_str(), "send" | "submit" | "发送" | "提交"))
    {
        return Some(0);
    }
    // Codex changes the same composer-adjacent submit control from “Send” to
    // “Queue” or “Steer” while a turn is running. These exact labels are safe
    // to accept because geometry and unchanged-draft guards are still applied;
    // deliberately do not accept “Stop” or broad queue substrings.
    if labels.iter().any(|label| {
        matches!(
            label.as_str(),
            "queue" | "steer" | "排队" | "引导" | "加入队列" | "排入队列" | "加入佇列" | "排入佇列"
        )
    }) {
        return Some(1);
    }
    if labels.iter().any(|label| {
        ["send message", "submit message", "发送消息", "提交消息"]
            .iter()
            .any(|needle| label.contains(needle))
    }) {
        return Some(2);
    }
    None
}

fn send_button_score(element: &AXUIElement) -> Option<u8> {
    send_button_label_score(&element_labels(element))
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

fn press_unique_send_button(target: &ComposerTarget, allow_queue: bool) -> Result<bool, String> {
    press_unique_submission(target, allow_queue).map(|mode| mode.is_some())
}

fn submission_kind(labels: &[String]) -> &'static str {
    if labels.iter().any(|label| matches!(label.to_lowercase().as_str(),"steer"|"引导")) { "steer" }
    else if send_button_label_score(labels)==Some(1) { "queue" }
    else { "send" }
}

fn assert_submission_focus(target: &ComposerTarget) -> Result<(), String> {
    if !agent_is_frontmost(target.agent)
        || ax_element_attribute(&target.app, "AXFocusedUIElement").as_ref() != Some(&target.composer)
    {
        return Err("发送前焦点已变化，未执行发送".into());
    }
    Ok(())
}

fn assert_submission_target(target: &ComposerTarget) -> Result<(), String> {
    assert_submission_focus(target)?;
    if composer_value(&target.composer) != target.value {
        return Err("发送前焦点或草稿已变化，未执行发送".into());
    }
    Ok(())
}

fn submit_once(
    button: impl FnOnce() -> Result<Option<&'static str>, String>,
    enter: impl FnOnce() -> Result<(), String>,
) -> Result<&'static str, String> {
    match button()? {
        Some(mode) => Ok(mode),
        None => { enter()?; Ok("enter") }
    }
}

fn press_unique_submission(target: &ComposerTarget, allow_queue: bool) -> Result<Option<&'static str>, String> {
    press_native_submission(target, allow_queue, true)
}

fn press_native_submission(target: &ComposerTarget, allow_queue: bool, check_value: bool) -> Result<Option<&'static str>, String> {
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
    if !allow_queue && buttons.iter().any(|(score, _)| *score == 1) {
        return Err("DESKTOP_TASK_TARGET_BUSY".into());
    }
    let best_score = buttons.first().map(|(score, _)| *score);
    let best = buttons
        .iter()
        .filter(|(score, _)| Some(*score) == best_score)
        .collect::<Vec<_>>();
    if best.len() == 1 {
        if check_value { assert_submission_target(target)?; }
        else { assert_submission_focus(target)?; }
        let mode=submission_kind(&element_labels(&best[0].1));
        best[0]
            .1
            .press()
            .map_err(|error| format!("{} macOS 发送按钮执行失败: {error}", target.agent.label()))?;
        Ok(Some(mode))
    } else if best.is_empty() {
        Ok(None)
    } else {
        Err(format!(
            "{} macOS 主输入框附近存在多个可用发送按钮",
            target.agent.label()
        ))
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum SubmitReadback {
    Confirmed,
    DraftUnchanged,
    ComposerChanged,
}

fn submit_readback(value: &str, submitted_text: &str) -> SubmitReadback {
    if value.is_empty() {
        SubmitReadback::Confirmed
    } else if value == submitted_text {
        SubmitReadback::DraftUnchanged
    } else {
        SubmitReadback::ComposerChanged
    }
}

fn submit_failure_diagnostic(state: &MacosComposerState, submitted_text: &str) -> String {
    let Some(pinned) = state.current_visible_target.as_ref() else {
        return "没有固定输入框".into();
    };
    let window = CodexWindow { app: pinned.app.clone(), window: pinned.window.clone() };
    let candidates = composer_candidates(&window);
    let focus = ax_element_attribute(&pinned.app, "AXFocusedUIElement");
    // A missing AX value must not be mistaken for a cleared editor. This is
    // diagnostic only; it does not rebind a target or attempt submission.
    let raw = pinned.composer.value().ok().and_then(|value| value.downcast::<CFString>())
        .map(|value| value.to_string());
    let value = raw.as_ref().map(|value| normalize_element_composer_value(&pinned.composer, value));
    format!("候选数={}；原框仍在候选={}；焦点仍在原框={}；原框可读={}；原框长度={:?}；原框为空={:?}；原框仅换行={:?}；原文仍在={:?}",
        candidates.len(), candidates.iter().any(|(element, _)| element == &pinned.composer),
        focus.as_ref() == Some(&pinned.composer), value.is_some(),
        value.as_ref().map(|value| value.encode_utf16().count()),
        value.as_ref().map(|value| value.is_empty()), value.as_ref().map(|value| value == "\n"),
        value.as_ref().map(|value| value == submitted_text))
}

fn codex_successor_is_clear(same_identity: bool, old_still_present: bool, focused_owned: bool, value: Option<&str>) -> bool {
    same_identity && !old_still_present && focused_owned
        && value.is_some_and(|value| value.is_empty() || value == "\n")
}

fn cleared_codex_submit_successor(state: &MacosComposerState, identity: &[String]) -> Result<AXUIElement, String> {
    let pinned = state.current_visible_target.as_ref().ok_or("发送前未固定输入框")?;
    let window = agent_windows(state.agent)?.into_iter()
        .find(|window| window.window == pinned.window).ok_or("发送后原窗口已不存在")?;
    let same_identity = task_window_identity(&window.window) == identity;
    let candidates = composer_candidates(&window);
    let old_still_present = candidates.iter().any(|(element, _)| element == &pinned.composer);
    let focused = ax_element_attribute(&window.app, "AXFocusedUIElement");
    let (successor, _) = select_window_composer(state.agent, &window, candidates)?;
    let focused_owned = focused.as_ref() == Some(&successor)
        && ax_element_attribute(&successor, "AXWindow").as_ref() == Some(&window.window);
    let value = successor.value().ok().and_then(|value| value.downcast::<CFString>())
        .map(|value| normalize_element_composer_value(&successor, &value.to_string()));
    if codex_successor_is_clear(same_identity, old_still_present, focused_owned, value.as_deref()) {
        Ok(successor)
    } else {
        Err(format!("发送后新框尚未确认：对话标识一致={same_identity}；原框仍在={old_still_present}；新框焦点归属一致={focused_owned}；新框可读={}；新框长度={:?}",
            value.is_some(), value.as_ref().map(|value| value.encode_utf16().count())))
    }
}

fn wait_for_submit_readback(
    state: &mut MacosComposerState,
    submitted_text: &str,
    timeout: Duration,
    submit_identity: &[String],
) -> Result<SubmitReadback, String> {
    let deadline = Instant::now() + timeout;
    let mut last_error = String::new();
    let mut clear_successor: Option<(AXUIElement, Instant)> = None;
    loop {
        match find_voice_target(state) {
            Ok(target) => {
                clear_successor = None;
                let readback = submit_readback(&target.value, submitted_text);
                if readback != SubmitReadback::DraftUnchanged || Instant::now() >= deadline {
                    return Ok(readback);
                }
            }
            Err(error) => {
                if state.agent == MacosAgent::Codex {
                    // Observation only, after the human's confirmation key.
                    // Never rebind the write target or submit to its successor.
                    match cleared_codex_submit_successor(state, submit_identity) {
                        Ok(successor) => {
                            if let Some((previous, since)) = &clear_successor {
                                if previous == &successor && since.elapsed() >= Duration::from_millis(150) {
                                    return Ok(SubmitReadback::Confirmed);
                                }
                            }
                            if clear_successor.as_ref().is_none_or(|(previous, _)| previous != &successor) {
                                clear_successor = Some((successor, Instant::now()));
                            }
                            last_error = "等待同一新输入框稳定清空".into();
                        }
                        Err(reason) => {
                            clear_successor = None;
                            last_error = format!("{error}；{reason}");
                        }
                    }
                } else {
                    // Sending a first message may replace WorkBuddy's composer.
                    // Observe an empty successor only after submission; never rebind
                    // the write target or send again to that successor.
                    if state.agent == MacosAgent::WorkBuddy {
                        if let Some(pinned) = state.current_visible_target.as_ref() {
                            let window = CodexWindow { app: pinned.app.clone(), window: pinned.window.clone() };
                            if let Ok((_, value)) = find_composer(state.agent, &window) {
                                if value.is_empty() {
                                    return Ok(SubmitReadback::Confirmed);
                                }
                            }
                        }
                    }
                    last_error = error;
                }
            }
        }
        if Instant::now() >= deadline {
            return Err(last_error);
        }
        thread::sleep(Duration::from_millis(40));
    }
}

fn task_draft_snapshot(state: &MacosComposerState, target: &ComposerTarget) -> Result<serde_json::Value, String> {
    if !matches!(state.agent, MacosAgent::Codex | MacosAgent::WorkBuddy) || state.task_identity.is_none() {
        return Err("DESKTOP_TASK_TARGET_CHANGED".into());
    }
    let mut pid = 0;
    if unsafe { AXUIElementGetPid(target.app.as_concrete_TypeRef(), &mut pid) } != kAXErrorSuccess || pid <= 0 {
        return Err("DESKTOP_TASK_TARGET_CHANGED".into());
    }
    // AX equality is checked against the pinned native elements by
    // find_voice_target. These CF hashes only identify the in-memory snapshot.
    Ok(serde_json::json!({
        "target": {
            "agent":state.agent.label(),
            "processId":pid,
            "windowId":unsafe { CFHash(target.window.as_CFTypeRef()) },
            "composerId":unsafe { CFHash(target.composer.as_CFTypeRef()) },
            "taskIdentity":state.task_identity,
        },
        "text":target.value,
    }))
}

pub(super) fn inspect_task_draft(state: &mut MacosComposerState) -> Result<serde_json::Value, String> {
    let target = find_voice_target(state)?;
    task_draft_snapshot(state, &target)
}

fn task_composer_error_kind(error: &str) -> &'static str {
    if error.contains("DESKTOP_TASK_TARGET_CHANGED") { "target_changed" }
    else if error.contains("DESKTOP_TASK_READBACK_CHANGED") || error.contains("内容在获取焦点后发生变化") { "draft_changed" }
    else if error.contains("DESKTOP_TASK_SEND_UNAVAILABLE") { "send_unavailable" }
    else if error.contains("没有确认语音文本") { "write_readback_unconfirmed" }
    else if error.contains("发送前焦点或草稿已变化") { "submit_guard_changed" }
    else if error.contains("多个可用发送按钮") { "multiple_send_buttons" }
    else if error.contains("发送按钮执行失败") { "button_press_failed" }
    else if error.contains("输入框写入失败") { "write_failed" }
    else if error.contains("无法获得焦点") { "focus_failed" }
    else if error.contains("备份剪贴板") { "clipboard_backup_failed" }
    else if error.contains("无法准备 macOS") { "clipboard_write_failed" }
    else if error.contains("无法创建 Codex") { "input_event_failed" }
    else { "other" }
}

// A task replaces the snapshot with a handoff containing that original draft.
// Unlike AXValue assignment, native paste updates the rich editor's input state.
// Native Select All is scoped to the pinned process/editor. AX text selection
// is not an acknowledgement: rich editors expose placeholders/structural text
// inconsistently. Do not gate a new request on selected-text or length equality.
// The original draft remains inside the handoff; paste and submit happen once.
fn replace_codex_task_text(target: &ComposerTarget, text: &str) -> Result<(), String> {
    focus_composer(target)?;
    let assert_unchanged = || -> Result<(), String> {
        if !agent_is_frontmost(MacosAgent::Codex)
            || ax_element_attribute(&target.app, "AXFocusedUIElement").as_ref() != Some(&target.composer)
            || composer_value(&target.composer) != target.value {
            return Err("DESKTOP_TASK_READBACK_CHANGED".into());
        }
        Ok(())
    };
    assert_unchanged()?;
    let snapshot = PasteboardSnapshot::capture()?;
    let result = (|| {
        post_codex_command(&target.app, 0)?; // macOS A: select all in this editor only.
        thread::sleep(KEYBOARD_READBACK_DELAY);
        assert_unchanged()?;
        set_pasteboard_text(text)?;
        assert_unchanged()?;
        post_codex_paste(&target.app)?;
        // Same transport contract as dictation: let the editor consume the
        // clipboard, then submit. AXValue is not a receipt for native input;
        // it may expose a different rich-text representation indefinitely.
        thread::sleep(KEYBOARD_READBACK_DELAY);
        Ok(())
    })();
    snapshot.restore();
    result
}

fn task_composer_stage<T>(stage: &'static str, operation: impl FnOnce() -> Result<T,String>) -> Result<T,String> {
    crate::realtime_chat_log::record("", "task_composer", serde_json::json!({"stage":stage,"state":"started"}));
    let started=Instant::now();
    let result=operation();
    crate::realtime_chat_log::record("", "task_composer", serde_json::json!({"stage":stage,"state":"finished",
        "ok":result.is_ok(),"elapsedMs":started.elapsed().as_millis(),
        "errorKind":result.as_ref().err().map(|e|task_composer_error_kind(e)).unwrap_or("none")}));
    result
}

fn submit_codex_task_after_paste(state: &mut MacosComposerState) -> Result<String, String> {
    // Revalidate the pinned window/editor, not equality with the outgoing
    // payload or its length. Sending must not wait for AX text acknowledgement.
    let target = find_voice_target(state)?;
    focus_composer(&target)?;
    let mode = task_composer_stage("submit_button", || submit_once(
        || press_native_submission(&target, true, false),
        || { assert_submission_focus(&target)?; press_enter(target.agent, &target.app) },
    ))?;
    Ok(mode.into())
}

pub(super) fn dispatch_task_from_draft(
    state: &mut MacosComposerState,
    text: &str,
    expected: &serde_json::Value,
) -> Result<String, String> {
    let target = task_composer_stage("target_refresh", || find_voice_target(state))?;
    let current = task_composer_stage("draft_snapshot", || task_draft_snapshot(state, &target))?;
    if expected.get("target") != current.get("target") {
        return Err("DESKTOP_TASK_TARGET_CHANGED".into());
    }
    if expected.get("text") != current.get("text") {
        return Err("DESKTOP_TASK_READBACK_CHANGED".into());
    }
    // The coordinator has already appended the new request to the live draft.
    // Do not run VoiceDraft::desired: it trims the complete text and adds the
    // old prefix again. Native replacement rechecks focus and current value.
    task_composer_stage("draft_write", || {
        if target.agent == MacosAgent::Codex { replace_codex_task_text(&target, text) }
        else { replace_composer_text(&target, text) }
    }).map_err(|error| format!("DESKTOP_TASK_WRITE_NOT_CONFIRMED: {error}"))?;
    state.draft.last = text.to_string();
    task_composer_stage("confirm", || {
        if state.agent == MacosAgent::Codex { submit_codex_task_after_paste(state) }
        else { confirm_voice_inner(state, Some(text)) }
    })
}

pub(super) fn dispatch_voice_task(state: &mut MacosComposerState, text: &str) -> Result<String, String> {
    let target = find_voice_target(state)?;
    if !matches!(state.agent, MacosAgent::Codex|MacosAgent::WorkBuddy) || !state.draft.base.is_empty() || !target.value.is_empty() {
        return Err("DESKTOP_TASK_DRAFT_NOT_EMPTY".into());
    }
    update_voice(state, text)?;
    confirm_voice_inner(state, Some(text))
}

pub(super) fn confirm_voice(
    state: &mut MacosComposerState,
    _staged_text: &str,
) -> Result<String, String> {
    confirm_voice_inner(state, None)
}

fn confirm_voice_inner(state: &mut MacosComposerState, expected: Option<&str>) -> Result<String, String> {
    let target = find_voice_target(state)?;
    if expected.is_some_and(|text| target.value != text) {
        return Err("DESKTOP_TASK_READBACK_CHANGED".into());
    }
    if target.value.is_empty() {
        return Err(format!(
            "{} macOS 语音草稿已不在输入框中，未重复发送",
            state.agent.label()
        ));
    }
    let text = target.value.clone();
    state.draft.last = text.clone();
    thread::sleep(Duration::from_millis(80));
    focus_composer(&target)?;
    if let Some(expected) = expected {
        if find_voice_target(state)?.value != expected {
            return Err("DESKTOP_TASK_READBACK_CHANGED".into());
        }
    }
    if state.agent == MacosAgent::Codex && expected.is_none() {
        // Plain dictation is keyboard input, not an Agent task. Trigger one
        // native Send/Queue/Steer action and release immediately; no local bus,
        // receipt polling, model-state gate or automatic resend.
        submit_once(
            || press_unique_submission(&target, true),
            || { assert_submission_target(&target)?; press_enter(target.agent, &target.app) },
        ).map_err(|reason| format!("CODEX_SUBMIT_UNCONFIRMED: 提交操作未确认：{reason}；不会自动重发"))?;
        state.draft.last.clear();
        return Ok(String::new());
    }
    let submit_identity = task_window_identity(&target.window);
    if expected.is_some() {
        // Codex tasks share dictation's one-shot submit: native button first,
        // Enter only when no button was attempted. Never retry an uncertain
        // press, press Stop or approve a permission prompt.
        let mode=task_composer_stage("submit_button", || {
            if state.agent == MacosAgent::Codex {
                submit_once(
                    || press_unique_submission(&target, true),
                    || { assert_submission_target(&target)?; press_enter(target.agent, &target.app) },
                )
            } else {
                press_unique_submission(&target, true)?
                    .ok_or_else(|| "DESKTOP_TASK_SEND_UNAVAILABLE".to_string())
            }
        })?;
        // The task coordinator confirms the exact user receipt on the event
        // bus. A successful button press is only an attempt, not a delivery
        // claim. Do not demand that a rerendered editor keep its pre-send ID.
        return Ok(mode.into());
    } else {
        press_enter(target.agent, &target.app)?;
    }

    match wait_for_submit_readback(state, &text, SUBMIT_FALLBACK_DELAY, &submit_identity) {
        Ok(SubmitReadback::Confirmed) => {
            state.draft.last.clear();
            return Ok(String::new());
        }
        Ok(SubmitReadback::ComposerChanged) => {
            state.draft.last.clear();
            return Err(format!(
                "{} macOS 提交后输入框内容发生变化，无法安全确认发送结果",
                state.agent.label()
            ));
        }
        Ok(SubmitReadback::DraftUnchanged) => {
            // AX keyboard delivery is the most reliable path in current Codex
            // builds. Only if the exact speech draft is still present after a
            // bounded delay do we press the unique composer-adjacent button.
            // The equality guard prevents a late fallback after the composer
            // has cleared, been replaced, or received new user text.
            // Explicit task delegation must attempt submission only once.
            // Retain the legacy fallback solely for ordinary key confirmation.
            if expected.is_none() {
                if let Ok(target) = find_voice_target(state) {
                    if target.value == text {
                        let _ = press_unique_send_button(&target, true);
                    }
                }
            }
        }
        Err(_) => {}
    }

    let final_readback_reason = match wait_for_submit_readback(state, &text, SUBMIT_CONFIRM_TIMEOUT, &submit_identity) {
        Ok(SubmitReadback::Confirmed) => {
            state.draft.last.clear();
            return Ok(String::new());
        }
        Ok(SubmitReadback::ComposerChanged) => {
            state.draft.last.clear();
            return Err(format!(
                "{} macOS 提交后输入框内容发生变化，无法安全确认发送结果",
                state.agent.label()
            ));
        }
        Ok(SubmitReadback::DraftUnchanged) => "原草稿仍在输入框".to_string(),
        Err(error) => error,
    };

    Err(format!(
        "{} macOS 提交结果未确认；请检查目标对话，避免重复发送。原因：{}。诊断：{}",
        state.agent.label(), final_readback_reason, submit_failure_diagnostic(state, &text)
    ))
}

pub(super) fn cancel_voice(state: &mut MacosComposerState) {
    if let Ok(target) = find_voice_target(state) {
        if let Some(base) = state.draft.rollback(&target.value) {
            let _ = replace_composer_text(&target, base);
        }
    }
    state.draft.last = state.draft.base.clone();
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
    #[test]
    fn native_submit_attempt_never_falls_back_after_button_success_or_error() {
        for mode in ["send", "queue", "steer"] {
            assert_eq!(super::submit_once(|| Ok(Some(mode)), || panic!("must not double-send")).unwrap(), mode);
        }
        assert!(super::submit_once(|| Err("press outcome unknown".into()), || panic!("must not retry an uncertain press")).is_err());
        let calls = std::cell::Cell::new(0);
        assert_eq!(super::submit_once(|| Ok(None), || { calls.set(calls.get()+1); Ok(()) }).unwrap(), "enter");
        assert_eq!(calls.get(), 1);
        assert!(super::submit_once(|| Ok(None), || Err("key-up failed".into())).is_err());
    }

    use super::*;

    #[test]
    fn streaming_voice_edits_only_the_owned_fragment() {
        assert_eq!(voice_edit("", "你好", 3), VoiceEdit{start:3,replace:"".into(),insert:"你好".into()});
        assert_eq!(voice_edit("你好", "你好呀", 3), VoiceEdit{start:5,replace:"".into(),insert:"呀".into()});
        assert_eq!(voice_edit("你好", "你好", 3), VoiceEdit{start:5,replace:"".into(),insert:"".into()});
        assert_eq!(voice_edit("我想去北京", "我想去南京。", 2), VoiceEdit{start:2,replace:"我想去北京".into(),insert:"我想去南京。".into()});
        assert_eq!(voice_edit("🙂", "🙂好", 3), VoiceEdit{start:5,replace:"".into(),insert:"好".into()});
        let prefix="用户前文🙂";let suffix="原有后文\n";
        let origin=prefix.encode_utf16().count();
        let mut document=format!("{prefix}{suffix}");let mut previous=String::new();
        for text in ["去北", "去北京", "去南京。", "去南京。", "去南京。谢谢"] {
            let edit=voice_edit(&previous,text,origin);
            let mut units=document.encode_utf16().collect::<Vec<_>>();
            let end=edit.start+edit.replace.encode_utf16().count();
            units.splice(edit.start..end,edit.insert.encode_utf16());
            document=String::from_utf16(&units).unwrap();
            assert_eq!(document,format!("{prefix}{text}{suffix}"));
            previous=text.into();
        }
    }

    #[test]
    fn voice_asr_dispatch_does_not_require_previous_readback() {
        for (actual, caret) in [("", None), ("灰色提示", Some(0)), ("其他草稿", Some(4))] {
            let append = voice_edit_without_ack("哈喽", "哈喽你好", 0, actual, caret);
            assert_eq!(append.insert, "你好");
            assert!(append.replace.is_empty());
            let revision = voice_edit_without_ack("哈罗", "哈喽", 0, actual, caret);
            assert_eq!(revision.insert, "哈喽");
            assert!(revision.replace.is_empty());
        }
        let revision = voice_edit_without_ack("哈罗", "哈喽", 0, "前哈罗后", Some(3));
        assert_eq!(revision, VoiceEdit { start: 1, replace: "哈罗".into(), insert: "哈喽".into() });
        let same = voice_edit_without_ack("哈喽", "哈喽", 0, "", None);
        assert!(same.insert.is_empty() && same.replace.is_empty());
    }

    #[test]
    fn voice_native_dispatch_has_no_acknowledgement_latch_or_poll() {
        let source = include_str!("codex_composer_macos.rs");
        let body = source.split("fn insert_codex_voice_once(").nth(1).unwrap()
            .split("pub(super) fn begin_voice(").next().unwrap();
        assert!(!body.contains("native_insert_attempted"));
        assert!(!body.contains("while Instant::now()"));
        assert!(!body.contains("expected_caret"));
        assert!(!body.contains("composer_value(&target.composer)"));
        assert!(body.contains("transcript == state.native_voice_text"));
        assert!(body.contains("post_codex_paste(&target.app)?"));
        assert!(body.contains("state.native_voice_text = transcript.into()"));
    }

    #[test]
    fn plain_dictation_submit_does_not_wait_for_agent_receipts() {
        let source=include_str!("codex_composer_macos.rs");
        let branch=source.split("if state.agent == MacosAgent::Codex && expected.is_none() {").nth(1).unwrap().split("let submit_identity").next().unwrap();
        assert!(branch.contains("submit_once("));
        assert!(!branch.contains("Receipt::capture"));
        assert!(!branch.contains("receipt.poll"));
        assert!(!branch.contains("wait_for_submit_readback"));
    }

    #[test]
    fn dynamic_placeholder_is_not_a_voice_draft_suffix() {
        let hint = "这是动态灰色提示，不是用户草稿。";
        for characters in [None, Some(0)] {
            let base = composer_value_with_placeholder(hint, Some(hint), characters);
            assert!(base.is_empty());
            let draft = crate::voice_draft::VoiceDraft::at_utf16_caret(base, 0).unwrap();
            assert_eq!(draft.desired("识别文本"), "识别文本");
            assert_eq!(draft.caret_after("识别文本"), Some(4));
        }
        assert_eq!(composer_value_with_placeholder(hint, Some(hint), Some(16)), hint);
        assert_eq!(composer_value_with_placeholder(hint, None, None), hint);
        assert_eq!(composer_value_with_placeholder("已有草稿", Some(hint), Some(0)), "已有草稿");
        assert_eq!(composer_value_with_placeholder("Ask anything", Some("Ask anything"), Some(12)), "Ask anything");
        assert_eq!(composer_value_with_placeholder("真实后文\n", Some(hint), None), "真实后文\n");
    }

    #[test]
    fn inserted_utterance_does_not_compare_old_draft_or_hint() {
        let speech = "哈喽哈喽，可以听到我说话吗？";
        let end = speech.encode_utf16().count();
        assert!(codex_inserted_utterance_present(speech,speech,Some(end)));
        assert!(codex_inserted_utterance_present(&format!("{speech}\n"),speech,Some(end)));
        assert!(codex_inserted_utterance_present(&format!("前🙂{speech}真实后文"),speech,Some(end+3)));
        assert!(!codex_inserted_utterance_present("哈喽",speech,Some(2)));
        assert!(!codex_inserted_utterance_present(speech,speech,None));
        assert!(!codex_inserted_utterance_present(speech,speech,Some(end-1)));
        assert!(!codex_inserted_utterance_present(speech,speech,Some(usize::MAX)));
        assert!(!codex_inserted_utterance_present("", " ",Some(0)));
        assert!(!codex_inserted_utterance_present("🙂", "🙂",Some(1)));
        assert!(codex_inserted_utterance_present("🙂", "🙂",Some(2)));
        assert_eq!(codex_base_without_inserted_utterance(speech,speech,Some(end)),Some(String::new()));
        assert_eq!(codex_base_without_inserted_utterance(&format!("前🙂{speech}真实后文"),speech,Some(end+3)),Some("前🙂真实后文".into()));
    }

    #[test]
    fn codex_send_successor_requires_same_identity_owned_focus_and_readable_empty_value() {
        for clear in ["", "\n"] {
            assert!(codex_successor_is_clear(true, false, true, Some(clear)));
            assert!(!codex_successor_is_clear(false, false, true, Some(clear)));
            assert!(!codex_successor_is_clear(true, true, true, Some(clear)));
            assert!(!codex_successor_is_clear(true, false, false, Some(clear)));
        }
        assert!(!codex_successor_is_clear(true, false, true, None));
        for draft in ["原草稿", "新草稿", " ", "\n\n", "正文\n"] {
            assert!(!codex_successor_is_clear(true, false, true, Some(draft)));
        }
    }

    #[test]
    fn repeated_utterances_are_verified_at_their_own_caret_without_document_equality() {
        let first="第一句";let second="第二句";
        assert!(codex_inserted_utterance_present(first, first,Some(3)));
        assert!(codex_inserted_utterance_present("第一句第二句",second,Some(6)));
        assert!(codex_inserted_utterance_present("前第一句第二句后",second,Some(7)));
        assert!(!codex_inserted_utterance_present("第二句第一句",second,Some(6)));
    }

    #[test]
    fn codex_multiple_editors_use_only_user_focus_in_same_window() {
        assert_eq!(focused_composer_index(MacosAgent::Codex, 2, Some(1), true), Some(1));
        assert_eq!(focused_composer_index(MacosAgent::Codex, 2, None, true), None);
        assert_eq!(focused_composer_index(MacosAgent::Codex, 2, Some(0), false), None);
        assert_eq!(focused_composer_index(MacosAgent::Codex, 0, Some(0), true), None);
        assert_eq!(focused_composer_index(MacosAgent::Codex, 2, Some(2), true), None);
        assert_eq!(focused_composer_index(MacosAgent::WorkBuddy, 2, Some(1), true), None);
        assert_eq!(focused_composer_index(MacosAgent::Claude, 2, Some(1), true), None);
    }

    #[test]
    fn codex_user_diagnostic_two_editors_does_not_require_unique_candidate() {
        // User's snapshot: two enabled text areas, no prompt labels; after
        // activation ordinal 0 owns this window and is the focused element.
        assert!(focused_snapshot_eligible(MacosAgent::Codex, true, true,
            released_composer_labels_match("AXTextArea", &[]), true));
        assert_eq!(focused_composer_index(MacosAgent::Codex, 2, Some(0), true), Some(0));
        assert_eq!(focused_composer_index(MacosAgent::Codex, 2, None, false), None);
    }

    #[test]
    fn codex_focused_snapshot_never_crosses_window_or_agent_or_hidden_control() {
        for facts in [(false,true,true,true),(true,false,true,true),
            (true,true,false,true),(true,true,true,false)] {
            assert!(!focused_snapshot_eligible(MacosAgent::Codex, facts.0,facts.1,facts.2,facts.3));
        }
        for agent in [MacosAgent::Claude, MacosAgent::WorkBuddy] {
            assert!(!focused_snapshot_eligible(agent, true,true,true,true));
        }
    }

    #[test]
    fn composer_visibility_rejects_only_explicit_hidden_or_outside_geometry() {
        let window = Some((-640,0,640,480));
        assert!(composer_geometry_visible(false, Some((-600,380,300,70)), window));
        assert!(composer_geometry_visible(false, Some((-20,380,100,70)), window));
        assert!(composer_geometry_visible(false, None, window));
        assert!(composer_geometry_visible(false, Some((0,0,100,70)), None));
        assert!(!composer_geometry_visible(true, None, None));
        assert!(!composer_geometry_visible(false, Some((0,0,0,70)), None));
        assert!(!composer_geometry_visible(false, Some((0,0,100,70)), window));
        assert!(!composer_geometry_visible(false, Some((-600,480,100,70)), window));
    }

    #[test]
    fn composer_matching_never_uses_draft_as_a_control_label() {
        let source = include_str!("codex_composer_macos.rs");
        let matcher = source.split("fn is_composer_element(").nth(1).unwrap()
            .split("fn composer_geometry_visible(").next().unwrap();
        assert!(matcher.contains("composer_metadata(element)"));
        assert!(!matcher.contains("element_value("));
        assert!(!matcher.contains("element_labels("));
        assert!(released_composer_labels_match("AXTextArea", &[]));
        assert!(!released_composer_labels_match("AXTextArea", &["Search".into()]));
    }

    #[test]
    fn composer_search_and_readonly_metadata_override_generic_textarea() {
        assert!(current_composer_metadata_matches("AXTextArea", &[], &[], false));
        assert!(!current_composer_metadata_matches("AXTextArea", &[], &[], true));
        for hint in ["search", "search-input", "search_box", "AXSearchField", "SearchInput"] {
            assert!(!current_composer_metadata_matches("AXTextArea", &[], &[hint.into()], false));
        }
        assert!(!current_composer_metadata_matches("AXTextArea", &["Ask anything search".into()], &[], false));
        for hint in ["research-composer", "prompt-textarea", "message-input"] {
            assert!(current_composer_metadata_matches("AXTextArea", &[], &[hint.into()], false));
        }
        // User's successful snapshot: text area accepted; search text field excluded.
        assert!(!current_composer_metadata_matches("AXTextField", &[], &["search".into()], false));
    }

    #[test]
    fn dom_classes_support_native_string_and_array_without_assuming_element_type() {
        assert_eq!(string_attribute_values(CFString::new("search-input").as_CFType()), vec!["search-input"]);
        let values = CFArray::from_CFTypes(&[CFString::new("prompt").as_CFType(),
            CFBoolean::true_value().as_CFType(),CFString::new("search").as_CFType()]);
        assert_eq!(string_attribute_values(values.as_CFType()),vec!["prompt","search"]);
        assert!(string_attribute_values(CFBoolean::false_value().as_CFType()).is_empty());
    }

    #[test]
    fn failure_evidence_is_scoped_and_cleared_on_success_or_early_exit() {
        {
            let _scope = ComposerEvidenceScope::begin(true);
            assert!(CAPTURE_AMBIGUOUS_EVIDENCE.with(|enabled| enabled.get()));
            AMBIGUOUS_COMPOSER_EVIDENCE.with(|slot| *slot.borrow_mut() = Some(serde_json::json!({"test":true})));
        }
        assert!(!CAPTURE_AMBIGUOUS_EVIDENCE.with(|enabled| enabled.get()));
        assert!(AMBIGUOUS_COMPOSER_EVIDENCE.with(|slot| slot.borrow().is_none()));
        let _scope = ComposerEvidenceScope::begin(false);
        assert!(!CAPTURE_AMBIGUOUS_EVIDENCE.with(|enabled| enabled.get()));
    }

    #[test]
    fn voice_and_task_share_editor_selection_and_keep_task_window_guards() {
        let source = include_str!("codex_composer_macos.rs");
        let finder = source.split("fn find_composer(").nth(1).unwrap()
            .split("fn focused_composer_index(").next().unwrap();
        assert!(finder.find("focused_codex_composer").unwrap() < finder.find("composer_candidates").unwrap());
        let startup = source.split("fn find_stable_current_target(").nth(1).unwrap()
            .split("fn find_voice_target(").next().unwrap();
        assert!(startup.contains("let focused = focused_codex_composer(agent, &target);"));
        assert!(!startup.contains("if current_task { None }"));
        assert!(startup.find("focused_codex_composer").unwrap() < startup.find("composer_candidates").unwrap());
        assert!(startup.find("thread::sleep(COMPOSER_STABILITY_DELAY)").unwrap() < startup.find("let deadline").unwrap());
        assert!(startup.contains("select_window_composer(agent, &target, candidates)?"));
        assert!(!startup.contains("unique_composer(agent, candidates)?"));
        assert!(startup.contains("ComposerEvidenceScope::begin(agent == MacosAgent::Codex)"));
        assert!(startup.contains("current_task_window_for(agent)?"));
        assert!(startup.contains("current.window != pinned_window"));
        assert!(startup.contains("refreshed_composer != composer"));
        assert!(startup.contains("refreshed_value != candidate.value"));
    }

    #[test]
    fn voice_window_regression_non_product_title_is_a_valid_standard_window() {
        let windows = vec![(false, "AXStandardWindow".into(), false)];
        // Models the old title exclusion; a fixture alone does not establish
        // the cause of a user's live failure.
        assert!(!windows.iter().any(|(exact, _, _)| *exact));
        assert_eq!(codex_primary_window_index(&windows), Ok(0));
        assert_eq!(codex_primary_window_index(&[
            (true, "AXDialog".into(), false), windows[0].clone(),
        ]), Ok(1));
    }

    #[test]
    fn voice_window_selection_never_uses_utility_editors_or_guesses_between_threads() {
        let main = (true, "AXStandardWindow".into(), false);
        assert_eq!(codex_primary_window_index(&[main.clone(), main]), Err("DESKTOP_TASK_MULTIPLE_MAIN_WINDOWS"));
        assert_eq!(codex_primary_window_index(&[(true, "AXDialog".into(), false)]), Err("DESKTOP_TASK_MAIN_WINDOW_MISSING"));
        assert_eq!(codex_primary_window_index(&[(false, "AXStandardWindow".into(), true)]), Err("DESKTOP_TASK_MODAL_OPEN"));
        assert_eq!(codex_primary_window_index(&[]), Err("DESKTOP_TASK_MAIN_WINDOW_MISSING"));
        assert_eq!(codex_primary_window_index(&[(false, "AXUnknown".into(), false)]), Err("DESKTOP_TASK_MAIN_WINDOW_MISSING"));
        assert_eq!(codex_primary_window_index(&[(true, "".into(), false)]), Ok(0));
        assert_eq!(codex_primary_window_index(&[(true, "AXUnknown".into(), false), (true, "".into(), false)]), Err("DESKTOP_TASK_MULTIPLE_MAIN_WINDOWS"));
    }

    #[test]
    fn task_stage_diagnostics_preserve_results_and_never_retry() {
        use std::cell::Cell;
        let calls=Cell::new(0);
        let error="ChatGPT macOS 输入框没有确认语音文本 PRIVATE";
        let result:Result<(),String>=task_composer_stage("draft_write",||{
            calls.set(calls.get()+1);Err(error.into())
        });
        assert_eq!(calls.get(),1);
        assert_eq!(result.unwrap_err(),error);
        assert_eq!(task_composer_error_kind(error),"write_readback_unconfirmed");
        assert_eq!(task_composer_error_kind("发送前焦点或草稿已变化，未执行发送"),"submit_guard_changed");
        assert_eq!(task_composer_error_kind("PRIVATE arbitrary provider error"),"other");
        assert_eq!(task_composer_stage("confirm",||Ok::<_,String>("send")).unwrap(),"send");
    }

    #[test]
    fn codex_task_selects_natively_without_ax_text_acknowledgement() {
        let source=include_str!("codex_composer_macos.rs");
        let body=source.split("fn replace_codex_task_text(").nth(1).unwrap().split("fn task_composer_stage").next().unwrap();
        assert!(body.contains("post_codex_command(&target.app, 0)?"));
        assert!(body.contains("assert_unchanged()?"));
        assert!(!body.contains("AXSelectedText"));
        assert!(!body.contains("encode_utf16"));
        assert_eq!(body.matches("post_codex_paste(&target.app)?").count(),1);
        assert!(body.contains("snapshot.restore()"));
    }

    #[test]
    fn current_task_ignores_nonmodal_utility_editors() {
        let utility = ("AXDialog".into(), false);
        let main = ("AXStandardWindow".into(), false);
        assert_eq!(current_task_window_index(&[utility.clone(), main.clone()]), Ok(1));
        assert_eq!(current_task_window_index(&[main.clone(), utility.clone()]), Ok(0));
        assert_eq!(current_task_window_index(&[main.clone(), main]), Err("DESKTOP_TASK_MULTIPLE_MAIN_WINDOWS"));
        assert_eq!(current_task_window_index(&[utility]), Err("DESKTOP_TASK_MAIN_WINDOW_MISSING"));
        assert_eq!(current_task_window_index(&[]), Err("DESKTOP_TASK_MAIN_WINDOW_MISSING"));
    }

    #[test]
    fn current_task_does_not_bypass_modal_dialogs_or_sheets() {
        assert_eq!(current_task_window_index(&[("AXDialog".into(), true), ("AXStandardWindow".into(), false)]), Err("DESKTOP_TASK_MODAL_OPEN"));
        assert_eq!(current_task_window_index(&[("AXStandardWindow".into(), true)]), Err("DESKTOP_TASK_MODAL_OPEN"));
        assert_eq!(current_task_window_index(&[("AXUnknown".into(), false)]), Err("DESKTOP_TASK_MAIN_WINDOW_MISSING"));
    }

    #[test]
    #[ignore = "Opt-in live AX lookup only; requires an already authorized running Codex with one main window"]
    fn current_task_live_window_metadata_probe() {
        assert!(unsafe { AXIsProcessTrusted() });
        let target = current_task_window().expect("unique standard conversation window");
        assert_eq!(ax_string_attribute(&target.window, "AXSubrole"), "AXStandardWindow");
        let composers = composer_candidates(&target);
        assert_eq!(composers.len(), 1, "one enabled composer in selected main window");
        let again = current_task_window().expect("stable current window");
        assert!(again.window == target.window);
        // No focus, text write, keypress, submit, or editor contents in output.
        println!("standard_window=1 enabled_composer=1 stable=true writes=0 sends=0");
    }

    #[test]
    #[ignore = "Opt-in WorkBuddy focus/readiness check; never writes a draft or sends a task"]
    fn workbuddy_current_task_live_readiness_probe() {
        assert!(unsafe { AXIsProcessTrusted() });
        let before = current_task_window_for(MacosAgent::WorkBuddy).expect("unique WorkBuddy window");
        println!("before_composers={}", composer_candidates(&before).len());
        let mut state = begin_current_task_for(MacosAgent::WorkBuddy).expect("stable WorkBuddy editor");
        let target = find_voice_target(&mut state).expect("pinned editor still current");
        assert!(target.window == before.window);
        assert!(state.task_identity.is_some());
        println!("stable=true writes=0 sends=0");
    }

    #[test]
    fn workbuddy_input_never_uses_global_keyboard_delivery() {
        let source = include_str!("codex_composer_macos.rs");
        let target_keys = source.split("fn post_workbuddy_key(").nth(1).unwrap().split("fn select_workbuddy_draft(").next().unwrap();
        assert!(target_keys.contains("CGEventSourceStateID::Private"));
        assert!(target_keys.contains("event.post_to_pid(pid)"));
        assert!(!target_keys.contains(".post(CGEventTapLocation::HID)"));
        let writer = source.split("fn replace_composer_text(").nth(1).unwrap().split("pub(super) fn begin_voice(").next().unwrap();
        assert!(writer.contains("select_workbuddy_draft(&target.composer)"));
        assert!(!writer.contains("post_command_key("));
        assert!(!writer.contains("post_keyboard_event("));
        assert!(!writer.contains("paste_focused_text("));
    }

    #[test]
    fn workbuddy_paste_requires_the_entire_original_draft_selected() {
        assert!(!workbuddy_selection_matches(None, ""));
        assert!(!workbuddy_selection_matches(Some(""), "查一下北京"));
        assert!(!workbuddy_selection_matches(Some("北京"), "查一下北京"));
        assert!(workbuddy_selection_matches(Some("查一下北京"), "查一下北京"));
        assert!(workbuddy_selection_matches(Some("\u{feff}中文🙂\r\n第二行"), "中文🙂\n第二行"));
        assert!(workbuddy_selection_matches(Some("中文🙂第二行"), "中文🙂\n第二行"));
        assert!(!workbuddy_selection_matches(Some("中文🙂"), "中文🙂\n第二行"));
        assert!(!workbuddy_selection_matches(Some(""), "\n"));
        assert!(!workbuddy_selection_matches(Some("原文"), "原文 "));
        assert!(workbuddy_selection_matches(Some("\u{feff}"), ""));
    }

    #[test]
    fn workbuddy_identity_and_placeholder_are_isolated() {
        assert_eq!(agent_bundle_identifiers(MacosAgent::WorkBuddy), &["com.tencent.workbuddy.mac"]);
        assert_eq!(agent_application_names(MacosAgent::WorkBuddy), &["WorkBuddy"]);
        assert!(is_agent_primary_window_title(MacosAgent::WorkBuddy, "WorkBuddy"));
        assert!(!is_agent_primary_window_title(MacosAgent::WorkBuddy, "CodeBuddy"));
        assert!(session_deeplink(MacosAgent::WorkBuddy, "id", "claude://code/id").is_err());
        assert_eq!(normalize_composer_value("\u{feff}\n今天帮你做些什么？ @ 引用对话文件，/ 调用技能与指令"), "");
        assert_eq!(normalize_composer_value("\u{feff}"), "");
        assert_eq!(normalize_composer_value("已有草稿\n下一行"), "已有草稿\n下一行");
    }

    #[test]
    fn text_normalization_matches_windows_composer_contract() {
        assert_eq!(normalize_text("  hello\n world  "), "hello world");
    }

    #[test]
    fn chromium_accessibility_only_caches_successful_initialization() {
        let mut primed_pids = HashSet::new();
        record_agent_accessibility_primed(&mut primed_pids, 101, false);
        assert!(!primed_pids.contains(&101));
        record_agent_accessibility_primed(&mut primed_pids, 101, true);
        record_agent_accessibility_primed(&mut primed_pids, 101, true);
        assert_eq!(primed_pids.len(), 1);
        record_agent_accessibility_primed(&mut primed_pids, 202, true);
        assert_eq!(primed_pids.len(), 2);
    }

    #[test]
    fn workbuddy_tree_recovery_is_bounded_and_never_resolves_ambiguity() {
        assert!(should_recover_workbuddy_tree(MacosAgent::WorkBuddy, 0, false));
        assert!(!should_recover_workbuddy_tree(MacosAgent::WorkBuddy, 0, true));
        assert!(!should_recover_workbuddy_tree(MacosAgent::WorkBuddy, 1, false));
        assert!(!should_recover_workbuddy_tree(MacosAgent::WorkBuddy, 2, false));
        assert!(!should_recover_workbuddy_tree(MacosAgent::Codex, 0, false));
        assert!(!should_recover_workbuddy_tree(MacosAgent::Claude, 0, false));
    }

    #[test]
    fn comparison_keeps_the_released_classifier_including_its_search_word_behavior() {
        assert!(released_composer_labels_match("AXTextArea", &[]));
        assert!(released_composer_labels_match("AXTextField", &["Message Codex".into()]));
        assert!(!released_composer_labels_match("AXTextArea", &["Search chats".into()]));
        assert!(released_composer_labels_match("AXTextArea", &["Search chats".into(), "Message Codex".into()]));
        assert!(!released_composer_labels_match("AXGroup", &["Message Codex".into()]));
    }

    #[test]
    fn diagnostic_window_comparison_exposes_differences_without_changing_policies() {
        assert_eq!(legacy_codex_window_index(&[(false,900.),(false,1000.)]), None);
        assert_eq!(legacy_codex_window_index(&[(true,20.),(true,1000.),(false,2000.)]), Some(1));
        assert_eq!(codex_primary_window_index(&[(false,"AXStandardWindow".into(),false)]), Ok(0));
        assert_eq!(legacy_codex_window_index(&[(false,1000.)]), None);
        assert_eq!(codex_primary_window_index(&[(true,"AXStandardWindow".into(),true)]), Err("DESKTOP_TASK_MODAL_OPEN"));
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
    fn running_task_submit_labels_match_without_accepting_stop_controls() {
        assert_eq!(submission_kind(&["Queue".into()]),"queue");
        assert_eq!(submission_kind(&["加入队列".into()]),"queue");
        assert_eq!(submission_kind(&["Steer".into()]),"steer");
        assert_eq!(submission_kind(&["引导".into()]),"steer");
        assert_eq!(submission_kind(&["Send".into()]),"send");
        for label in [
            "Send",
            "Submit",
            "Queue",
            "Steer",
            "发送",
            "提交",
            "排队",
            "引导",
            "加入队列",
        ] {
            assert!(
                send_button_label_score(&[label.to_string()]).is_some(),
                "expected submit label to match: {label}"
            );
        }
        for label in ["Stop", "停止", "Clear queue", "清空队列", "Resume"] {
            assert_eq!(
                send_button_label_score(&[label.to_string()]),
                None,
                "unsafe non-submit control matched: {label}"
            );
        }
    }

    #[test]
    fn submit_readback_only_retries_an_identical_remaining_draft() {
        assert_eq!(submit_readback("", "hello"), SubmitReadback::Confirmed);
        assert_eq!(
            submit_readback("hello", "hello"),
            SubmitReadback::DraftUnchanged
        );
        assert_eq!(
            submit_readback("new user draft", "hello"),
            SubmitReadback::ComposerChanged
        );
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

    #[test]
    fn claude_focus_snapshot_accepts_an_already_current_target_without_a_new_focus_edge() {
        let marker = ClaudeDesktopFocusMarker {
            path: PathBuf::from("/tmp/local_session.json"),
            last_focused_at: 100,
            modified_at: Some(SystemTime::UNIX_EPOCH + Duration::from_secs(1)),
        };
        let already_current = ClaudeDesktopFocusSnapshot {
            target: Some(marker.clone()),
            target_is_latest: true,
        };
        assert!(claude_focus_snapshot_confirms_session(
            &already_current,
            &already_current
        ));

        let stale = ClaudeDesktopFocusSnapshot {
            target: Some(marker.clone()),
            target_is_latest: false,
        };
        assert!(!claude_focus_snapshot_confirms_session(&stale, &stale));

        let advanced = ClaudeDesktopFocusSnapshot {
            target: Some(ClaudeDesktopFocusMarker {
                last_focused_at: 101,
                ..marker
            }),
            target_is_latest: true,
        };
        assert!(claude_focus_snapshot_confirms_session(&stale, &advanced));
    }

    #[test]
    fn claude_focus_snapshot_resolves_cli_and_desktop_metadata_aliases() {
        let root = tempfile::tempdir().unwrap();
        let nested = root.path().join("account").join("workspace");
        fs::create_dir_all(&nested).unwrap();
        let cli_session_id = "c7755ed6-cf18-45e9-b84b-980fad6c1070";
        let mapped = nested.join("local_a7c981ce-c24d-4619-ae42-2c75e11e1f96.json");
        fs::write(
            &mapped,
            serde_json::to_vec(&serde_json::json!({
                "cliSessionId": cli_session_id,
                "sessionId": "local_a7c981ce-c24d-4619-ae42-2c75e11e1f96",
                "lastFocusedAt": 100,
                "isArchived": false
            }))
            .unwrap(),
        )
        .unwrap();
        let direct = nested.join(format!("local_{cli_session_id}.json"));
        fs::write(
            &direct,
            serde_json::to_vec(&serde_json::json!({
                "cliSessionId": cli_session_id,
                "sessionId": format!("local_{cli_session_id}"),
                "lastFocusedAt": 200,
                "isArchived": false
            }))
            .unwrap(),
        )
        .unwrap();
        fs::write(
            nested.join("local_other.json"),
            serde_json::to_vec(&serde_json::json!({
                "cliSessionId": "other",
                "sessionId": "local_other",
                "lastFocusedAt": 150,
                "isArchived": false
            }))
            .unwrap(),
        )
        .unwrap();
        fs::write(
            nested.join("local_archived.json"),
            serde_json::to_vec(&serde_json::json!({
                "cliSessionId": "archived",
                "sessionId": "local_archived",
                "lastFocusedAt": 300,
                "isArchived": true
            }))
            .unwrap(),
        )
        .unwrap();

        let snapshot = claude_desktop_focus_snapshot_from_root(root.path(), cli_session_id);
        assert_eq!(snapshot.target.unwrap().path, direct);
        assert!(snapshot.target_is_latest);

        let mapped_only = claude_desktop_focus_snapshot_from_root(
            root.path(),
            "a7c981ce-c24d-4619-ae42-2c75e11e1f96",
        );
        assert_eq!(mapped_only.target.unwrap().path, mapped);
        assert!(!mapped_only.target_is_latest);
    }
}
