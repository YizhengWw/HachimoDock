/*
 * [Input] A bound or current-visible ChatGPT（Codex）/Claude session, a current-visible WorkBuddy/DeepSeek Harness composer, or a captured MiMoCode terminal caret, plus staged voice text and an explicit confirm action.
 * [Output] Read-only frontmost-Agent detection, exact desktop-session navigation, bounded composer lookup, per-recording appended draft updates, and explicit-confirm submission without automatic send on ASR finalization.
 * [Pos] Cross-platform foreground input bridge with session, draft, clipboard, stale-focus recovery, Windows minimized-Claude restoration, and runtime-compatible UIA pattern lookup.
 * [Sync] If this file changes, update pc/.folder.md.
 */

use serde_json::{json, Value};

#[cfg(any(windows, target_os = "macos"))]
fn task_submission_mode(reply: &Value) -> Result<String, String> {
    match reply["submission"].as_str() {
        Some(mode @ ("send" | "queue" | "steer" | "enter")) => Ok(mode.into()),
        _ => Err("Codex 提交方式未确认，不会自动重试".into()),
    }
}

#[cfg(target_os = "macos")]
#[path = "codex_composer_macos.rs"]
mod macos;

pub fn diagnose_current_composer(agent_id: &str) -> Result<Value, String> {
    #[cfg(target_os = "macos")]
    {
        let agent = match agent_id {
            "codex" => macos::MacosAgent::Codex,
            "claude-code" => macos::MacosAgent::Claude,
            "workbuddy" => macos::MacosAgent::WorkBuddy,
            "deepseek-harness" => macos::MacosAgent::DeepSeekHarness,
            _ => return Err("此 Agent 不支持 macOS 输入框定位检查".into()),
        };
        macos::diagnose_current_composer(agent)
    }
    #[cfg(not(target_os = "macos"))]
    { let _ = agent_id; Err("此定位检查目前仅用于 macOS".into()) }
}

#[cfg(target_os = "macos")]
#[derive(Clone)]
pub struct FocusedTextTarget(macos::FocusedTextTarget);

#[cfg(target_os = "macos")]
pub fn capture_focused_text_target() -> Result<FocusedTextTarget, String> {
    macos::capture_focused_text_target().map(FocusedTextTarget)
}

#[cfg(target_os = "macos")]
pub fn insert_at_focused_text_target(target: &FocusedTextTarget, text: &str) -> Result<(), String> {
    macos::insert_at_focused_text_target(&target.0, text)
}

#[cfg(target_os = "macos")]
pub fn submit_at_focused_text_target(target: &FocusedTextTarget) -> Result<(), String> {
    macos::submit_at_focused_text_target(&target.0)
}

#[cfg(windows)]
const CODEX_COMPOSER_STARTUP_TIMEOUT_SECS: u64 = 10;

#[cfg(windows)]
const WINDOWS_COMPOSER_PROCESS_MEMORY_LIMIT_BYTES: usize = 512 * 1024 * 1024;

#[cfg(any(windows, test))]
fn composer_startup_timeout_message(agent: &str, stage: &str) -> String {
    let label = match agent {
        "workbuddy" => "WorkBuddy",
        "deepseek-harness" => "DeepSeek Harness",
        "claude" | "claude-code" => "Claude",
        "codex" => "ChatGPT（Codex）",
        _ => "Agent",
    };
    format!("{label} 输入框准备超时（阶段：{stage}）；尚未写入或发送文字")
}

// Progress contains fixed stage identifiers only, never editor text or paths.
// It is not a command acknowledgement and must not complete the ready wait.
#[cfg(any(windows, test))]
fn read_windows_composer_response(
    reader: &mut impl std::io::BufRead,
    mut on_progress: impl FnMut(&'static str),
) -> Result<Value, String> {
    loop {
        let mut line = String::new();
        reader.read_line(&mut line)
            .map_err(|error| format!("failed to read visible Agent composer response: {error}"))?;
        if line.trim().is_empty() {
            return Err("Visible Agent composer bridge closed without a response".into());
        }
        let value = serde_json::from_str::<Value>(&line)
            .map_err(|_| "Invalid visible Agent composer response".to_string())?;
        if value.get("phase").and_then(Value::as_str) == Some("progress") {
            let stage = match value.get("stage").and_then(Value::as_str) {
                Some("load_ui") => "加载 Windows 辅助功能组件",
                Some("compile_native") => "初始化 Windows 输入桥",
                Some("enable_accessibility") => "请求 WorkBuddy 辅助功能界面树",
                Some("ready_runtime") => "等待输入框定位命令",
                Some("resolve_window") => "定位目标应用窗口",
                Some("focused") => "读取当前聚焦输入框",
                Some("native_query") => "查询原生辅助功能输入框",
                Some("control") => "查找可写输入框",
                Some("raw") => "读取 Chromium 辅助功能树",
                Some("probe") => "检查输入区域",
                Some("validate") => "校验输入框归属和内容",
                Some("focus") => "聚焦目标输入框",
                _ => return Err("Unknown visible Agent composer progress stage".into()),
            };
            on_progress(stage);
            continue;
        }
        return match value.get("ok").and_then(Value::as_bool) {
            Some(true) => Ok(value),
            Some(false) => Err(value.get("error").and_then(Value::as_str)
                .unwrap_or("Visible Agent composer update failed").to_string()),
            None => Err("Invalid visible Agent composer acknowledgement".into()),
        };
    }
}

#[cfg(target_os = "macos")]
const CODEX_COMPOSER_STARTUP_TIMEOUT_SECS: u64 = 8;

#[cfg(windows)]
fn hidden_powershell() -> std::process::Command {
    use std::os::windows::process::CommandExt;
    let mut command = std::process::Command::new("powershell.exe");
    command.creation_flags(0x08000000);
    command.args([
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
    ]);
    command
}

#[cfg(windows)]
struct WindowsComposerJob {
    handle: usize,
}

#[cfg(windows)]
impl WindowsComposerJob {
    fn new(contain_descendants: bool) -> Result<Self, String> {
        use windows_sys::Win32::Foundation::CloseHandle;
        use windows_sys::Win32::System::JobObjects::{
            CreateJobObjectW, JobObjectExtendedLimitInformation, SetInformationJobObject,
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
            JOB_OBJECT_LIMIT_PROCESS_MEMORY, JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK,
        };

        let handle = unsafe { CreateJobObjectW(std::ptr::null(), std::ptr::null()) };
        if handle.is_null() {
            return Err(format!(
                "failed to create Windows composer job: {}",
                std::io::Error::last_os_error()
            ));
        }

        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            | JOB_OBJECT_LIMIT_PROCESS_MEMORY
            | if contain_descendants { 0 } else { JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK };
        limits.ProcessMemoryLimit = WINDOWS_COMPOSER_PROCESS_MEMORY_LIMIT_BYTES;
        let configured = unsafe {
            SetInformationJobObject(
                handle,
                JobObjectExtendedLimitInformation,
                (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            )
        };
        if configured == 0 {
            let error = std::io::Error::last_os_error();
            unsafe {
                CloseHandle(handle);
            }
            return Err(format!("failed to configure Windows composer job: {error}"));
        }
        Ok(Self {
            handle: handle as usize,
        })
    }

    fn assign(&self, child: &std::process::Child) -> Result<(), String> {
        use std::os::windows::io::AsRawHandle;
        use windows_sys::Win32::Foundation::HANDLE;
        use windows_sys::Win32::System::JobObjects::AssignProcessToJobObject;

        let assigned = unsafe {
            AssignProcessToJobObject(self.handle as HANDLE, child.as_raw_handle() as HANDLE)
        };
        if assigned == 0 {
            return Err(format!(
                "failed to contain Windows composer process {}: {}",
                child.id(),
                std::io::Error::last_os_error()
            ));
        }
        Ok(())
    }
}

#[cfg(windows)]
impl WindowsComposerJob {
    fn terminate(&self) -> Result<(), String> {
        use windows_sys::Win32::{Foundation::HANDLE, System::JobObjects::TerminateJobObject};
        if unsafe { TerminateJobObject(self.handle as HANDLE, 1) } == 0 {
            return Err("Windows helper job teardown unconfirmed".into());
        }
        Ok(())
    }
    fn active_processes(&self) -> Result<u32, String> {
        use windows_sys::Win32::{Foundation::HANDLE, System::JobObjects::{QueryInformationJobObject, JobObjectBasicAccountingInformation, JOBOBJECT_BASIC_ACCOUNTING_INFORMATION}};
        let mut info=JOBOBJECT_BASIC_ACCOUNTING_INFORMATION::default();
        if unsafe { QueryInformationJobObject(self.handle as HANDLE,JobObjectBasicAccountingInformation,(&mut info as *mut JOBOBJECT_BASIC_ACCOUNTING_INFORMATION).cast(),std::mem::size_of_val(&info) as u32,std::ptr::null_mut()) } == 0 {
            return Err("Windows helper job accounting unavailable".into());
        }
        Ok(info.ActiveProcesses)
    }
}

#[cfg(windows)]
struct WindowsComposerProcess {
    child: std::sync::Mutex<std::process::Child>,
    job: WindowsComposerJob,
    strict: bool,
}
#[cfg(windows)]
impl crate::device_voice_lifecycle::OwnedProcessTree for WindowsComposerProcess {
    fn terminate(&self) -> Result<(), String> {
        // No business command or input replay. Kill failures alone prove nothing;
        // subsequent actual wait + job accounting decide whether it is quiescent.
        if self.strict { let _ = self.job.terminate(); }
        let mut child=self.child.lock().map_err(|_|"Windows helper child owner unavailable")?;
        if child.try_wait().map_err(|_|"Windows helper wait failed")?.is_none() { let _=child.kill(); }
        Ok(())
    }
    fn reaped_and_empty(&self) -> Result<bool, String> {
        if !self.strict { return Ok(false); }
        let mut child=self.child.lock().map_err(|_|"Windows helper child owner unavailable")?;
        if child.try_wait().map_err(|_|"Windows helper wait failed")?.is_none() { return Ok(false); }
        drop(child);
        Ok(self.job.active_processes()? == 0)
    }
}

/// This error is returned only before any native worker/child exists.
#[cfg(any(windows, target_os = "macos"))]
pub struct ComposerNoWriterError(String);
#[cfg(any(windows, target_os = "macos"))]
impl From<String> for ComposerNoWriterError { fn from(error: String) -> Self { Self(error) } }
#[cfg(any(windows, target_os = "macos"))]
impl From<&str> for ComposerNoWriterError { fn from(error: &str) -> Self { Self(error.to_owned()) } }
#[cfg(any(windows, target_os = "macos"))]
impl ComposerNoWriterError { pub fn into_message(self) -> String { self.0 } }

#[cfg(windows)]
impl Drop for WindowsComposerJob {
    fn drop(&mut self) {
        use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
        unsafe {
            CloseHandle(self.handle as HANDLE);
        }
    }
}

#[cfg(windows)]
fn claude_desktop_sessions_root() -> Option<std::path::PathBuf> {
    if let Some(path) = std::env::var_os("CLAUDE_DESKTOP_SESSIONS_DIR") {
        let path = std::path::PathBuf::from(path);
        if !path.as_os_str().is_empty() {
            return Some(path);
        }
    }

    std::env::var_os("APPDATA")
        .map(std::path::PathBuf::from)
        .map(|root| root.join("Claude").join("claude-code-sessions"))
}

#[cfg(windows)]
fn valid_claude_desktop_session_id(session_id: &str) -> bool {
    session_id
        .strip_prefix("local_")
        .and_then(|value| uuid::Uuid::parse_str(value).ok())
        .is_some()
}

#[cfg(windows)]
fn claude_metadata_timestamp(value: &Value) -> u64 {
    value
        .as_u64()
        .or_else(|| value.as_f64().map(|number| number.max(0.0) as u64))
        .unwrap_or_default()
}

#[cfg(windows)]
#[derive(Debug, Clone, PartialEq, Eq)]
struct ClaudeDesktopSessionTarget {
    session_id: String,
    title: String,
}

#[cfg(windows)]
fn claude_desktop_session_target_from_root(
    root: &std::path::Path,
    cli_session_id: &str,
    expected_title: &str,
) -> Option<ClaudeDesktopSessionTarget> {
    let cli_session_id = cli_session_id.trim();
    uuid::Uuid::parse_str(cli_session_id).ok()?;
    let expected_title = expected_title.trim();
    let mut pending = vec![(root.to_path_buf(), 0_u8)];
    let mut best: Option<((bool, bool, u64, u128), ClaudeDesktopSessionTarget)> = None;

    while let Some((directory, depth)) = pending.pop() {
        let Ok(entries) = std::fs::read_dir(directory) else {
            continue;
        };
        for entry in entries.flatten() {
            let Ok(file_type) = entry.file_type() else {
                continue;
            };
            if file_type.is_dir() {
                if depth < 4 {
                    pending.push((entry.path(), depth + 1));
                }
                continue;
            }
            if !file_type.is_file() {
                continue;
            }
            let file_name = entry.file_name();
            let file_name = file_name.to_string_lossy();
            if !file_name.starts_with("local_") || !file_name.ends_with(".json") {
                continue;
            }
            let Ok(metadata) = entry.metadata() else {
                continue;
            };
            if metadata.len() > 2 * 1024 * 1024 {
                continue;
            }
            let Ok(raw) = std::fs::read_to_string(entry.path()) else {
                continue;
            };
            let Ok(value) = serde_json::from_str::<Value>(&raw) else {
                continue;
            };
            let candidate_cli_id = value
                .get("cliSessionId")
                .or_else(|| value.get("cli_session_id"))
                .and_then(Value::as_str)
                .unwrap_or_default()
                .trim();
            if !candidate_cli_id.eq_ignore_ascii_case(cli_session_id)
                || value
                    .get("isArchived")
                    .and_then(Value::as_bool)
                    .unwrap_or(false)
            {
                continue;
            }
            let desktop_session_id = value
                .get("sessionId")
                .or_else(|| value.get("session_id"))
                .and_then(Value::as_str)
                .unwrap_or_default()
                .trim();
            if !valid_claude_desktop_session_id(desktop_session_id) {
                continue;
            }
            let title = value
                .get("title")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .trim();
            let activity = ["lastFocusedAt", "lastActivityAt", "createdAt"]
                .iter()
                .filter_map(|key| value.get(*key))
                .map(claude_metadata_timestamp)
                .max()
                .unwrap_or_default();
            let modified = metadata
                .modified()
                .ok()
                .and_then(|time| time.duration_since(std::time::UNIX_EPOCH).ok())
                .map(|duration| duration.as_millis())
                .unwrap_or_default();
            let score = (
                !expected_title.is_empty() && title.eq_ignore_ascii_case(expected_title),
                !title.is_empty(),
                activity,
                modified,
            );
            if best
                .as_ref()
                .is_none_or(|(best_score, _)| score > *best_score)
            {
                best = Some((
                    score,
                    ClaudeDesktopSessionTarget {
                        session_id: desktop_session_id.to_string(),
                        title: if title.is_empty() {
                            expected_title.to_string()
                        } else {
                            title.to_string()
                        },
                    },
                ));
            }
        }
    }

    best.map(|(_, target)| target)
}

#[cfg(windows)]
fn claude_desktop_session_target(
    session_id: &str,
    session_title: &str,
) -> Result<ClaudeDesktopSessionTarget, String> {
    uuid::Uuid::parse_str(session_id.trim())
        .map_err(|_| "Claude session ID is not a valid UUID".to_string())?;
    let root = claude_desktop_sessions_root()
        .ok_or_else(|| "Claude Desktop session metadata directory is unavailable".to_string())?;
    claude_desktop_session_target_from_root(&root, session_id, session_title).ok_or_else(|| {
            "Claude Desktop has no existing session mapped to this Claude session; refusing to create a new General Coding Session".to_string()
        })
}

#[cfg(target_os = "macos")]
fn claude_cli_transcript_exists(session_id: &str) -> bool {
    let config_root = std::env::var_os("CLAUDE_CONFIG_DIR")
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|home| std::path::PathBuf::from(home).join(".claude"))
        });
    let Some(projects_root) = config_root.map(|root| root.join("projects")) else {
        return false;
    };
    let Ok(projects) = std::fs::read_dir(projects_root) else {
        return false;
    };
    let file_name = format!("{session_id}.jsonl");
    projects
        .flatten()
        .any(|project| project.path().is_dir() && project.path().join(&file_name).is_file())
}

#[cfg(target_os = "macos")]
fn claude_session_deep_link(session_id: &str) -> Result<String, String> {
    let session_id = session_id.trim();
    uuid::Uuid::parse_str(session_id)
        .map_err(|_| "Claude session ID is not a valid UUID".to_string())?;
    if claude_cli_transcript_exists(session_id) {
        Ok(format!("claude://resume?session={session_id}"))
    } else {
        Ok(format!("claude://code/{session_id}"))
    }
}

#[cfg(windows)]
fn codex_session_deep_link(session_id: &str) -> Option<String> {
    let session_id = session_id.trim();
    uuid::Uuid::parse_str(session_id).ok()?;
    Some(format!("codex://threads/{session_id}"))
}

#[derive(Debug, Clone)]
pub struct CodexComposerEvent {
    pub phase: String,
    pub ok: bool,
    pub error: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CodexComposerWaitError {
    StartTimeout,
    StartDisconnected,
    CompletionTimeout,
    CompletionDisconnected,
}

pub struct CodexComposerSubmission {
    started: std::sync::mpsc::Receiver<()>,
    completed: std::sync::mpsc::Receiver<Result<Value, String>>,
}

impl CodexComposerSubmission {
    pub fn wait(
        self,
        start_timeout: std::time::Duration,
        completion_timeout: std::time::Duration,
    ) -> Result<Result<Value, String>, CodexComposerWaitError> {
        self.started
            .recv_timeout(start_timeout)
            .map_err(|error| match error {
                std::sync::mpsc::RecvTimeoutError::Timeout => CodexComposerWaitError::StartTimeout,
                std::sync::mpsc::RecvTimeoutError::Disconnected => {
                    CodexComposerWaitError::StartDisconnected
                }
            })?;
        self.completed
            .recv_timeout(completion_timeout)
            .map_err(|error| match error {
                std::sync::mpsc::RecvTimeoutError::Timeout => {
                    CodexComposerWaitError::CompletionTimeout
                }
                std::sync::mpsc::RecvTimeoutError::Disconnected => {
                    CodexComposerWaitError::CompletionDisconnected
                }
            })
    }
}

#[cfg(any(windows, target_os = "macos"))]
struct ComposerCommand {
    payload: Value,
    started: Option<std::sync::mpsc::Sender<()>>,
    response: Option<std::sync::mpsc::Sender<Result<Value, String>>>,
}

#[cfg(any(windows, target_os = "macos"))]
fn composer_command_kind(command: &ComposerCommand) -> &str {
    command
        .payload
        .get("kind")
        .and_then(Value::as_str)
        .unwrap_or("")
}

#[cfg(any(windows, target_os = "macos"))]
fn receive_latest_composer_command(
    receiver: &std::sync::mpsc::Receiver<ComposerCommand>,
    pending: &mut Option<ComposerCommand>,
) -> Option<ComposerCommand> {
    let mut command = match pending.take() {
        Some(command) => command,
        None => receiver.recv().ok()?,
    };
    if composer_command_kind(&command) != "update" || command.response.is_some() {
        return Some(command);
    }

    while let Ok(next) = receiver.try_recv() {
        if composer_command_kind(&next) == "update" {
            command = next;
            // A readback-acknowledged final update is a barrier, never discard it.
            if command.response.is_some() { break; }
        } else {
            *pending = Some(next);
            break;
        }
    }
    Some(command)
}

#[cfg(any(windows, target_os = "macos"))]
fn composer_command_failure_is_fatal(command: &ComposerCommand) -> bool {
    composer_command_kind(command) == "begin"
}

#[cfg(any(windows, target_os = "macos"))]
pub struct CodexComposerBridge {
    sender: std::sync::mpsc::Sender<ComposerCommand>,
    failed: std::sync::Arc<std::sync::atomic::AtomicBool>,
    closed: std::sync::Arc<std::sync::atomic::AtomicBool>,
    release_result: std::sync::Mutex<Option<Result<(), String>>>,
    #[cfg(any(windows, target_os = "macos"))]
    termination: Option<crate::device_voice_lifecycle::WriterTermination>,
}

#[cfg(any(windows, target_os = "macos"))]
impl CodexComposerBridge {
    pub fn start_current_task() -> Result<Self, String> {
        Self::start_current_task_for("codex")
    }
    pub fn start_current_task_for(agent_id: &str) -> Result<Self, String> {
        if !matches!(agent_id,"codex"|"workbuddy"|"deepseek-harness") {return Err("Unsupported desktop task adapter".into());}
        #[cfg(target_os = "macos")]
        let agent = match agent_id { "workbuddy" => macos::MacosAgent::WorkBuddy, "deepseek-harness" => macos::MacosAgent::DeepSeekHarness, _ => macos::MacosAgent::Codex };
        #[cfg(windows)]
        let agent = agent_id;
        Self::start_with_purpose(agent, "", "", "", "", "current_task", |_| {})
    }

    /// Legacy empty-draft dispatch. Both desktop adapters use the
    /// snapshot-aware path below. A lost acknowledgement never triggers a resend.
    pub fn dispatch_task(&self, text: &str) -> Result<String, String> {
        self.dispatch_task_command(json!({"kind":"dispatch", "text":text}))
    }

    pub fn inspect_task_draft(&self) -> Result<Value, String> {
        let (tx, rx) = std::sync::mpsc::channel();
        self.sender.send(ComposerCommand {
            payload: json!({"kind":"inspect_task_draft"}), started: None, response: Some(tx),
        }).map_err(|_| "Codex 输入桥已关闭".to_string())?;
        let reply = rx.recv_timeout(std::time::Duration::from_secs(10))
            .map_err(|_| "Codex 草稿检查超时，未发送".to_string())??;
        let snapshot = &reply["snapshot"];
        if !snapshot["target"].is_object() || !snapshot["text"].is_string() {
            return Err("Codex 草稿检查未确认，未发送".into());
        }
        Ok(snapshot.clone())
    }

    pub fn dispatch_task_from_draft(&self, text: &str, snapshot: &Value) -> Result<String, String> {
        self.dispatch_task_command(json!({"kind":"dispatch", "text":text, "draftSnapshot":snapshot}))
    }

    fn dispatch_task_command(&self, payload: Value) -> Result<String, String> {
        use std::sync::atomic::Ordering;
        if self.failed.load(Ordering::SeqCst) || self.closed.load(Ordering::SeqCst) {
            return Err("Codex 输入桥不可用".into());
        }
        let (tx, rx) = std::sync::mpsc::channel();
        self.sender.send(ComposerCommand {
            payload,
            started: None, response: Some(tx),
        }).map_err(|_| "Codex 输入桥已关闭".to_string())?;
        let reply = rx.recv_timeout(std::time::Duration::from_secs(15))
            .map_err(|_| "Codex 发送结果未确认，不会自动重试".to_string())??;
        task_submission_mode(&reply)
    }

    /// Success means the editor acknowledged this exact final revision, not just
    /// that it entered the worker queue. No automatic submission or retry.
    pub fn update_confirmed(&self, revision: u64, text: &str) -> Result<(), String> {
        use std::sync::atomic::Ordering;
        if self.failed.load(Ordering::SeqCst) || self.closed.load(Ordering::SeqCst) {
            return Err("Visible Agent composer is unavailable".to_string());
        }
        let (tx, rx) = std::sync::mpsc::channel();
        self.sender.send(ComposerCommand {
            payload: json!({ "kind": "update", "revision": revision, "text": text }),
            started: None,
            response: Some(tx),
        }).map_err(|_| "Visible Agent composer bridge is closed".to_string())?;
        rx.recv_timeout(std::time::Duration::from_secs(8))
            .map_err(|_| "语音草稿写入结果未确认，请检查输入框；不会自动重试或发送".to_string())??;
        Ok(())
    }

    /// Drain this recording's queued writes before a new recording reads its base.
    /// Closing without cancellation must never erase the previous voice segment.
    pub fn preserve_draft(&self) -> Result<(), String> {
        if self.termination.is_some() { return self.preserve_macos_draft(); }
        #[cfg(target_os = "macos")]
        { return Err("native worker owner is unavailable".into()); }
        #[cfg(windows)]
        {
        use std::sync::atomic::Ordering;
        // Only cleanup/startup workers call this waiting operation. Serialize
        // concurrent callers and keep the actual ACK/error, including timeout.
        // `closed` rejects new writes; it is never evidence of release success.
        let mut cached = self.release_result.lock()
            .map_err(|_| "上一段语音释放状态不可用，释放未确认".to_string())?;
        if let Some(result) = cached.as_ref() { return result.clone(); }
        self.closed.store(true, Ordering::SeqCst);
        let result: Result<(), String> = (|| {
            let (tx, rx) = std::sync::mpsc::channel();
            self.sender.send(ComposerCommand {
                payload: json!({ "kind": "release" }), started: None, response: Some(tx),
            }).map_err(|_| "上一段语音输入已关闭，释放未确认".to_string())?;
            rx.recv_timeout(std::time::Duration::from_secs(8))
                .map_err(|_| "等待上一段语音完成写入超时，释放未确认".to_string())??;
            Ok(())
        })();
        *cached = Some(result.clone());
        result
            }
    }

    #[cfg(any(windows, target_os = "macos"))]
    pub(crate) fn writer_termination_proof(&self) -> Result<Option<crate::device_voice_lifecycle::WriterTerminationProof>, String> {
        match self.termination.as_ref() {
            Some(worker) => worker.observe(),
            None => Ok(None),
        }
    }

    #[cfg(windows)]
    pub(crate) fn reap_owned_writer(&self, timeout: std::time::Duration) -> Result<Option<crate::device_voice_lifecycle::WriterTerminationProof>, String> {
        match self.termination.as_ref() {
            Some(worker) => { worker.terminate_process()?; worker.wait(timeout) },
            None => Ok(None),
        }
    }

    #[cfg(any(windows, target_os = "macos"))]
    fn preserve_macos_draft(&self) -> Result<(), String> {
        use std::sync::atomic::Ordering;
        let mut cached = self.release_result.lock()
            .map_err(|_| "上一段语音释放状态不可用，释放未确认".to_string())?;
        self.closed.store(true, Ordering::SeqCst);
        // A cached failure is never retried. Only actual joined termination
        // of this same worker may independently establish ownership release.
        if self.writer_termination_proof()?.is_some() {
            *cached = Some(Ok(()));
            return Ok(());
        }
        if let Some(result) = cached.as_ref() {
            if result.is_err() {
                if let Some(worker) = self.termination.as_ref() {
                    worker.terminate_process()?;
                    if worker.wait(std::time::Duration::from_secs(8))?.is_some() {
                        *cached = Some(Ok(())); return Ok(());
                    }
                }
            }
            return result.clone();
        }
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(8);
        let (tx, rx) = std::sync::mpsc::channel();
        // Exactly one release command; no confirm/cancel/audio replay.
        let sent = self.sender.send(ComposerCommand {
            payload: json!({ "kind": "release" }), started: None, response: Some(tx),
        }).is_ok();
        if sent {
            // This ACK is distinct from terminal quiescence. Once the worker
            // stops, its actual join also covers a lost business/release ACK.
            let grace = deadline.saturating_duration_since(std::time::Instant::now());
            #[cfg(windows)] let grace = grace.min(std::time::Duration::from_secs(4));
            let _ = rx.recv_timeout(grace);
        }
        #[cfg(windows)]
        if let Some(worker) = self.termination.as_ref() { worker.terminate_process()?; }
        let result = loop {
            if self.writer_termination_proof()?.is_some() { break Ok(()); }
            let remaining = deadline.saturating_duration_since(std::time::Instant::now());
            if remaining.is_zero() {
                break Err("上一段语音worker尚未确认退出，未启动新writer".to_string());
            }
            std::thread::sleep(remaining.min(std::time::Duration::from_millis(2)));
        };
        *cached = Some(result.clone());
        result
    }
}

#[cfg(windows)]
impl CodexComposerBridge {
    pub fn is_agent_frontmost(agent_id: &str) -> bool {
        use windows_sys::Win32::Foundation::CloseHandle;
        use windows_sys::Win32::System::Threading::{
            OpenProcess, QueryFullProcessImageNameW, PROCESS_QUERY_LIMITED_INFORMATION,
        };
        use windows_sys::Win32::UI::WindowsAndMessaging::{
            GetForegroundWindow, GetWindowThreadProcessId,
        };

        let expected_names: &[&str] = match agent_id.trim() {
            "codex" => &["chatgpt.exe", "codex.exe"],
            "claude-code" => &["claude.exe"],
            "workbuddy" => &["workbuddy.exe"],
            "deepseek-harness" => &["deepseek harness.exe"],
            _ => return false,
        };
        unsafe {
            let window = GetForegroundWindow();
            if window.is_null() {
                return false;
            }
            let mut process_id = 0;
            GetWindowThreadProcessId(window, &mut process_id);
            if process_id == 0 {
                return false;
            }
            let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, process_id);
            if process.is_null() {
                return false;
            }
            let mut path = vec![0_u16; 32_768];
            let mut path_len = path.len() as u32;
            let queried =
                QueryFullProcessImageNameW(process, 0, path.as_mut_ptr(), &mut path_len) != 0;
            CloseHandle(process);
            if !queried {
                return false;
            }
            let executable = String::from_utf16_lossy(&path[..path_len as usize]);
            let executable_name = std::path::Path::new(&executable)
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or("");
            expected_names
                .iter()
                .any(|expected| executable_name.eq_ignore_ascii_case(expected))
        }
    }

    pub fn start_current(
        agent_id: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        let agent = match agent_id.trim() {
            "codex" => "codex",
            "claude-code" => "claude",
            "workbuddy" => "workbuddy",
            "deepseek-harness" => "deepseek-harness",
            _ => {
                return Err(
                    "Current visible composer requires ChatGPT（Codex）, Claude, WorkBuddy or DeepSeek Harness".to_string(),
                )
            }
        };
        Self::start_with_purpose(agent, "", "", "", "", "current_voice", callback)
    }

    pub fn start_current_retained(
        agent_id: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<RetainedComposerStartup, ComposerNoWriterError> {
        let agent=match agent_id.trim() {
            "codex"=>"codex", "claude-code"=>"claude", "workbuddy"=>"workbuddy", "deepseek-harness"=>"deepseek-harness",
            _=>return Err("Unsupported current visible composer".to_string().into()),
        };
        Self::start_with_purpose_retained(agent,"","","","","current_voice",callback)
    }
    fn retained_windows_failure(process: std::sync::Arc<WindowsComposerProcess>, error: String) -> RetainedComposerStartup {
        use crate::device_voice_lifecycle::OwnedProcessTree;
        let _=process.terminate();
        let(sender,receiver)=std::sync::mpsc::channel(); drop(receiver);
        RetainedComposerStartup {
            bridge: Self { sender, failed: std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true)), closed: std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true)), release_result: std::sync::Mutex::new(None), termination: Some(crate::device_voice_lifecycle::WriterTermination::owned_process(None,process)) },
            readiness: Err(error),
        }
    }

    pub fn start(
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        let deep_link = codex_session_deep_link(session_id).unwrap_or_default();
        Self::start_with_purpose(
            "codex",
            session_id,
            &deep_link,
            session_title,
            session_cwd,
            "voice",
            callback,
        )
    }

    pub fn start_claude(
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        let target = claude_desktop_session_target(session_id, session_title)?;
        Self::start_with_purpose(
            "claude",
            session_id,
            &target.session_id,
            &target.title,
            session_cwd,
            "voice",
            callback,
        )
    }

    pub fn focus_session(
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
    ) -> Result<(), String> {
        static NAVIGATION_LOCK: std::sync::OnceLock<std::sync::Mutex<()>> =
            std::sync::OnceLock::new();
        let _guard = NAVIGATION_LOCK
            .get_or_init(|| std::sync::Mutex::new(()))
            .lock()
            .map_err(|error| format!("ChatGPT（Codex） session navigation lock failed: {error}"))?;
        let deep_link = codex_session_deep_link(session_id).unwrap_or_default();
        let bridge = Self::start_with_purpose(
            "codex",
            session_id,
            &deep_link,
            session_title,
            session_cwd,
            "locate",
            |_| {},
        )?;
        drop(bridge);
        Ok(())
    }

    pub fn focus_claude_session(
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
    ) -> Result<(), String> {
        static NAVIGATION_LOCK: std::sync::OnceLock<std::sync::Mutex<()>> =
            std::sync::OnceLock::new();
        let _guard = NAVIGATION_LOCK
            .get_or_init(|| std::sync::Mutex::new(()))
            .lock()
            .map_err(|error| format!("Claude session navigation lock failed: {error}"))?;
        let target = claude_desktop_session_target(session_id, session_title)?;
        let bridge = Self::start_with_purpose(
            "claude",
            session_id,
            &target.session_id,
            &target.title,
            session_cwd,
            "locate",
            |_| {},
        )?;
        drop(bridge);
        Ok(())
    }

    fn start_with_purpose(
        agent: &str,
        session_id: &str,
        deep_link: &str,
        session_title: &str,
        session_cwd: &str,
        purpose: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        let startup=Self::start_with_purpose_retained(agent,session_id,deep_link,session_title,session_cwd,purpose,callback)
            .map_err(ComposerNoWriterError::into_message)?;
        startup.readiness?;
        Ok(startup.bridge)
    }

    fn start_with_purpose_retained(
        agent: &str,
        session_id: &str,
        deep_link: &str,
        session_title: &str,
        session_cwd: &str,
        purpose: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<RetainedComposerStartup, ComposerNoWriterError> {
        use std::fs;
        use std::io::{BufReader, Write};
        use std::process::Stdio;
        use std::sync::atomic::{AtomicBool, Ordering};
        use std::sync::{mpsc, Arc, Mutex};
        use std::thread;

        let agent = agent.trim();
        let session_id = session_id.trim();
        let deep_link = deep_link.trim();
        let session_title = session_title.trim();
        if !matches!(agent, "codex" | "claude" | "workbuddy" | "deepseek-harness") {
            return Err("visible composer agent is invalid".to_string().into());
        }
        let current_visible = matches!(purpose, "current_voice" | "current_task");
        if matches!(agent, "workbuddy" | "deepseek-harness") && !current_visible {
            return Err("WorkBuddy 仅向当前可见对话输入，不回退到其他 Agent".into());
        }
        if !current_visible
            && agent == "claude"
            && (session_id.is_empty() || !valid_claude_desktop_session_id(deep_link))
        {
            return Err("Claude visible composer requires a bound session ID".to_string().into());
        }
        if !current_visible && session_title.is_empty() {
            return Err("visible composer requires a non-empty bound session title".to_string().into());
        }
        if !matches!(purpose, "voice" | "locate" | "current_voice" | "current_task") {
            return Err("Visible Agent composer purpose is invalid".to_string().into());
        }

        const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
# Native parent assigns the process to its containment job before opening this gate.
if ([Console]::In.ReadLine() -cne 'HACHIMODOCK_CONTAINED_HELPER_V1') { exit 1 }
function Write-ComposerProgress([string]$stage) {
  [Console]::Out.WriteLine((@{ phase = 'progress'; stage = $stage } | ConvertTo-Json -Compress))
  [Console]::Out.Flush()
}
Write-ComposerProgress 'load_ui'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Write-ComposerProgress 'compile_native'
Add-Type @"
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class CodexVoiceNative {
  private const uint INPUT_KEYBOARD = 1;
  private const uint INPUT_MOUSE = 0;
  private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
  private const uint MOUSEEVENTF_LEFTUP = 0x0004;
  private const uint KEYEVENTF_KEYUP = 0x0002;
  private const uint KEYEVENTF_UNICODE = 0x0004;
  private const ushort VK_CONTROL = 0x11;
  private const ushort VK_A = 0x41;
  private const ushort VK_BACK = 0x08;
  private const ushort VK_RETURN = 0x0D;
  private const int SW_RESTORE = 9;

  [StructLayout(LayoutKind.Sequential)]
  private struct INPUT {
    public uint type;
    public INPUTUNION data;
  }

  [StructLayout(LayoutKind.Explicit)]
  private struct INPUTUNION {
    [FieldOffset(0)] public MOUSEINPUT mouse;
    [FieldOffset(0)] public KEYBDINPUT keyboard;
    [FieldOffset(0)] public HARDWAREINPUT hardware;
  }

  [StructLayout(LayoutKind.Sequential)]
  private struct MOUSEINPUT {
    public int x;
    public int y;
    public uint mouseData;
    public uint flags;
    public uint time;
    public UIntPtr extraInfo;
  }

  [StructLayout(LayoutKind.Sequential)]
  private struct KEYBDINPUT {
    public ushort virtualKey;
    public ushort scanCode;
    public uint flags;
    public uint time;
    public UIntPtr extraInfo;
  }

  [StructLayout(LayoutKind.Sequential)]
  private struct HARDWAREINPUT {
    public uint message;
    public ushort parameterLow;
    public ushort parameterHigh;
  }

  [DllImport("user32.dll", SetLastError = true)]
  private static extern uint SendInput(uint count, INPUT[] inputs, int size);

  [DllImport("user32.dll")]
  private static extern short GetAsyncKeyState(int virtualKey);
  private static readonly object InputGate = new object();
  private static bool InputUnconfirmed;
  private static bool InputNotInserted;
  // Survives every command in this helper, including Enter/click/release.
  public static bool InputOutcomeUnknown { get { lock (InputGate) { return InputUnconfirmed; } } }
  public static bool LastInputRejectedBeforeSend { get { lock (InputGate) { return InputNotInserted && !InputUnconfirmed; } } }

  [DllImport("user32.dll")]
  private static extern bool SetForegroundWindow(IntPtr window);

  [DllImport("user32.dll")]
  private static extern bool ShowWindowAsync(IntPtr window, int command);

  [DllImport("user32.dll")]
  private static extern bool IsIconic(IntPtr window);

  [DllImport("user32.dll")]
  private static extern IntPtr GetForegroundWindow();

  [DllImport("user32.dll")]
  private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

  [DllImport("kernel32.dll")]
  private static extern uint GetCurrentThreadId();

  [DllImport("user32.dll")]
  private static extern bool AttachThreadInput(uint fromThread, uint toThread, bool attach);

  [DllImport("user32.dll")]
  private static extern bool BringWindowToTop(IntPtr window);

  [DllImport("user32.dll")]
  public static extern bool IsWindow(IntPtr window);

  [DllImport("user32.dll")]
  private static extern bool IsChild(IntPtr parent, IntPtr child);

  public static bool IsOwnedWindow(IntPtr parent, IntPtr child) {
    return parent != IntPtr.Zero && child != IntPtr.Zero &&
      (parent == child || IsChild(parent, child));
  }

  public static bool IsForeground(IntPtr window) {
    return GetForegroundWindow() == window;
  }

  [DllImport("oleacc.dll", ExactSpelling = true)]
  private static extern int AccessibleObjectFromWindow(
    IntPtr window, uint objectId, ref Guid interfaceId, out IntPtr accessible);

  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  private static extern IntPtr SendMessageTimeout(
    IntPtr window, uint message, UIntPtr wParam, IntPtr lParam,
    uint flags, uint timeout, out UIntPtr result);

  public static int RequestClientAccessibility(IntPtr parent, IntPtr window, int expectedProcessId) {
    uint processId;
    GetWindowThreadProcessId(parent, out processId);
    if (processId != (uint)expectedProcessId || !IsForeground(parent) ||
        !IsWindow(window) || !IsOwnedWindow(parent, window)) { return 0; }
    int responses = 0;
    var className = new StringBuilder(256);
    GetClassName(window, className, className.Capacity);
    if (className.ToString() == "Chrome_RenderWidgetHostHWND") {
      // Chromium's documented MSAA client-detection object (id 1). It
      // returns no object: the request asks Chromium to expose web content.
      // This is WM_GETOBJECT, not a keyboard/mouse event or global setting.
      UIntPtr result;
      if (SendMessageTimeout(window, 0x003D, UIntPtr.Zero, new IntPtr(1),
          0x0001 | 0x0002 | 0x0020, 250, out result) != IntPtr.Zero) {
        responses |= 2;
      }
    }
    if (!IsForeground(parent) || !IsOwnedWindow(parent, window)) { return responses; }
    var iid = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71"); // IID_IAccessible
    IntPtr accessible = IntPtr.Zero;
    try {
      // Standard OBJID_CLIENT request also serves Windows versions using
      // MSAA-to-UIA bridging. A returned interface is NOT a writable editor.
      int hr = AccessibleObjectFromWindow(window, 0xFFFFFFFC, ref iid, out accessible);
      if (hr == 0 && accessible != IntPtr.Zero) { responses |= 1; }
    } finally {
      if (accessible != IntPtr.Zero) { Marshal.Release(accessible); }
    }
    return responses;
  }

  [DllImport("user32.dll", SetLastError = true)]
  private static extern bool SetCursorPos(int x, int y);

  private delegate bool EnumChildProc(IntPtr window, IntPtr parameter);

  [DllImport("user32.dll")]
  private static extern bool EnumChildWindows(IntPtr parent, EnumChildProc callback, IntPtr parameter);

  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  private static extern int GetClassName(IntPtr window, StringBuilder className, int maxCount);

  public static IntPtr[] FindChildWindowsByClass(IntPtr parent, string expectedClass) {
    var matches = new List<IntPtr>();
    EnumChildWindows(parent, delegate(IntPtr window, IntPtr parameter) {
      var className = new StringBuilder(256);
      GetClassName(window, className, className.Capacity);
      if (string.Equals(className.ToString(), expectedClass, StringComparison.Ordinal)) {
        matches.Add(window);
      }
      return true;
    }, IntPtr.Zero);
    return matches.ToArray();
  }

  private static INPUT Key(ushort virtualKey, bool keyUp) {
    return new INPUT {
      type = INPUT_KEYBOARD,
      data = new INPUTUNION {
        keyboard = new KEYBDINPUT {
          virtualKey = virtualKey,
          flags = keyUp ? KEYEVENTF_KEYUP : 0
        }
      }
    };
  }

  private static INPUT Unicode(char value, bool keyUp) {
    return new INPUT {
      type = INPUT_KEYBOARD,
      data = new INPUTUNION {
        keyboard = new KEYBDINPUT {
          scanCode = value,
          flags = KEYEVENTF_UNICODE | (keyUp ? KEYEVENTF_KEYUP : 0)
        }
      }
    };
  }

  // A token describes only an event identity, never the user text or target.
  private static ulong InputToken(INPUT input) {
    if (input.type == INPUT_MOUSE) {
      if (input.data.mouse.flags != MOUSEEVENTF_LEFTDOWN && input.data.mouse.flags != MOUSEEVENTF_LEFTUP)
        throw new InvalidOperationException("Unsupported mouse input; nothing inserted");
      return 2UL << 32;
    }
    if (input.type != INPUT_KEYBOARD || (input.data.keyboard.flags & ~(KEYEVENTF_KEYUP | KEYEVENTF_UNICODE | 0x0001U)) != 0)
      throw new InvalidOperationException("Unsupported keyboard input; nothing inserted");
    if ((input.data.keyboard.flags & KEYEVENTF_UNICODE) != 0) {
      if (input.data.keyboard.virtualKey != 0) throw new InvalidOperationException("Invalid Unicode input");
      return (1UL << 32) | input.data.keyboard.scanCode;
    }
    if (input.data.keyboard.virtualKey == 0 || input.data.keyboard.virtualKey > 255 || input.data.keyboard.scanCode != 0)
      throw new InvalidOperationException("Invalid virtual-key input");
    return ((ulong)(input.data.keyboard.flags & 0x0001U) << 24) | input.data.keyboard.virtualKey;
  }

  private static bool InputIsUp(INPUT input) {
    return input.type == INPUT_MOUSE ? input.data.mouse.flags == MOUSEEVENTF_LEFTUP
      : (input.data.keyboard.flags & KEYEVENTF_KEYUP) != 0;
  }

  private static INPUT InputRelease(INPUT input) {
    if (input.type == INPUT_MOUSE) input.data.mouse.flags = MOUSEEVENTF_LEFTUP;
    else input.data.keyboard.flags |= KEYEVENTF_KEYUP; // retains Unicode scan/extended identity
    return input;
  }

  private static void Send(List<INPUT> inputs) {
    if (inputs.Count == 0) return;
    lock (InputGate) {
      if (InputUnconfirmed) throw new InvalidOperationException("Previous native input is unknown; no further injection");
      InputNotInserted = true;
      if (inputs.Count > 131080) throw new InvalidOperationException("Native input budget exceeded");
      var pressed = new HashSet<ulong>();
      var watched = new HashSet<ulong>();
      foreach (INPUT input in inputs) {
        ulong token = InputToken(input); watched.Add(token);
        if (InputIsUp(input)) {
          if (!pressed.Remove(token)) throw new InvalidOperationException("Unowned key release; nothing inserted");
        } else if (!pressed.Add(token)) throw new InvalidOperationException("Duplicate key down; nothing inserted");
      }
      if (pressed.Count != 0) throw new InvalidOperationException("Unbalanced key input; nothing inserted");
      foreach (int modifier in new int[] { 0x10, 0x11, 0x12, 0x5b, 0x5c }) {
        if (GetAsyncKeyState(modifier) < 0) throw new InvalidOperationException("Release held modifiers before native input; nothing inserted");
      }
      foreach (ulong token in watched) {
        if ((token >> 32) == 1) continue; // Unicode packets have no physical key to snapshot.
        int vk = (token >> 32) == 2 ? 1 : (int)(token & 0xffff);
        if (GetAsyncKeyState(vk) < 0) throw new InvalidOperationException("Release held input before native input; nothing inserted");
      }
      INPUT[] values = inputs.ToArray();
      uint sent;
      InputNotInserted = false; // From here, exceptions do not establish zero insertion.
      try { sent = SendInput((uint)values.Length, values, Marshal.SizeOf(typeof(INPUT))); }
      catch { InputUnconfirmed = true; throw; }
      if (sent == values.Length) return; // insertion only, never target action completion
      int error = Marshal.GetLastWin32Error();
      InputUnconfirmed = true;
      var outstanding = new List<INPUT>();
      if (sent <= values.Length) {
        for (int i = 0; i < (int)sent; i++) {
          INPUT input = values[i]; ulong token = InputToken(input);
          if (InputIsUp(input)) {
            for (int j = outstanding.Count - 1; j >= 0; j--) {
              if (InputToken(outstanding[j]) == token) { outstanding.RemoveAt(j); break; }
            }
          } else outstanding.Add(input);
        }
      }
      int released = 0;
      // Only release this accepted prefix's outstanding downs. Each release
      // gets one call; even rejection must not prevent releasing other keys.
      for (int i = outstanding.Count - 1; i >= 0; i--) {
        try {
          INPUT[] up = new INPUT[] { InputRelease(outstanding[i]) };
          if (SendInput(1, up, Marshal.SizeOf(typeof(INPUT))) == 1) released++;
        } catch { /* retain Unknown, continue bounded releases; never replay action */ }
      }
      throw new Win32Exception(error,
        "Windows input outcome unknown (inserted " + sent + "/" + values.Length
        + ", releases " + released + "/" + outstanding.Count + "); stopped without replay");
    }
  }

  public static bool ActivateWindow(IntPtr window) {
    if (!IsWindow(window)) return false;
    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
    if (GetForegroundWindow() == window) return true;

    IntPtr foregroundWindow = GetForegroundWindow();
    uint ignored;
    uint foregroundThread = foregroundWindow == IntPtr.Zero
      ? 0
      : GetWindowThreadProcessId(foregroundWindow, out ignored);
    uint targetThread = GetWindowThreadProcessId(window, out ignored);
    uint currentThread = GetCurrentThreadId();
    bool attachedForeground = foregroundThread != 0 &&
      foregroundThread != currentThread &&
      AttachThreadInput(currentThread, foregroundThread, true);
    bool attachedTarget = targetThread != 0 &&
      targetThread != currentThread &&
      targetThread != foregroundThread &&
      AttachThreadInput(currentThread, targetThread, true);
    try {
      BringWindowToTop(window);
      SetForegroundWindow(window);
      return GetForegroundWindow() == window;
    } finally {
      if (attachedTarget) AttachThreadInput(currentThread, targetThread, false);
      if (attachedForeground) AttachThreadInput(currentThread, foregroundThread, false);
    }
  }

  public static bool RestoreWindow(IntPtr window) {
    return IsWindow(window) && ShowWindowAsync(window, SW_RESTORE);
  }

  public static void AppendFocusedText(IntPtr window, int expectedProcessId, string text) {
    lock (InputGate) {
      if (InputUnconfirmed) throw new InvalidOperationException("Previous native input is unknown; no further injection");
      InputNotInserted = true;
      uint actualProcessId;
      GetWindowThreadProcessId(window, out actualProcessId);
      if (actualProcessId != (uint)expectedProcessId || !IsForeground(window))
        throw new InvalidOperationException("DESKTOP_TASK_TARGET_CHANGED");
      if (text == null || text.Length == 0 || text.Length > 65536)
        throw new InvalidOperationException("Invalid native append text; nothing inserted");
      var inputs = new List<INPUT>(text.Length * 2 + 4);
      inputs.Add(Key(VK_CONTROL, false));
      inputs.Add(Key(0x23, false)); // End: collapse selection at document end.
      inputs.Add(Key(0x23, true));
      inputs.Add(Key(VK_CONTROL, true));
      foreach (char value in text) { inputs.Add(Unicode(value, false)); inputs.Add(Unicode(value, true)); }
      Send(inputs); // existing balanced-input/partial-release/Unknown contract
      if (!IsForeground(window)) {
        InputUnconfirmed = true;
        throw new InvalidOperationException("Native append target changed; result unknown, no replay");
      }
    }
  }

  public static void ReplaceFocusedText(string text) {
    var inputs = new List<INPUT>(Math.Max(8, (text == null ? 0 : text.Length * 2) + 6));
    inputs.Add(Key(VK_CONTROL, false));
    inputs.Add(Key(VK_A, false));
    inputs.Add(Key(VK_A, true));
    inputs.Add(Key(VK_CONTROL, true));
    inputs.Add(Key(VK_BACK, false));
    inputs.Add(Key(VK_BACK, true));
    if (text != null) {
      foreach (char value in text) {
        inputs.Add(Unicode(value, false));
        inputs.Add(Unicode(value, true));
      }
    }
    Send(inputs);
  }

  public static void PressEnter() {
    Send(new List<INPUT> { Key(VK_RETURN, false), Key(VK_RETURN, true) });
  }

  private static INPUT Mouse(uint flags) {
    return new INPUT {
      type = INPUT_MOUSE,
      data = new INPUTUNION { mouse = new MOUSEINPUT { flags = flags } }
    };
  }

  public static void Click(int x, int y) {
    if (!SetCursorPos(x, y)) {
      throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not position the ChatGPT (Codex) session click");
    }
    Send(new List<INPUT> { Mouse(MOUSEEVENTF_LEFTDOWN), Mouse(MOUSEEVENTF_LEFTUP) });
  }

  [StructLayout(LayoutKind.Sequential)]
  private struct ScreenPoint { public int X; public int Y; }

  [DllImport("user32.dll")]
  private static extern IntPtr WindowFromPoint(ScreenPoint point);

  public static void ClickSubmit(IntPtr window, int x, int y) {
    // Fail before any mouse input if another window covers the validated button.
    var point = new ScreenPoint { X = x, Y = y };
    if (GetForegroundWindow() != window || !IsOwnedWindow(window, WindowFromPoint(point))) {
      throw new InvalidOperationException("DESKTOP_TASK_TARGET_CHANGED");
    }
    Click(x, y);
  }
}
// WorkBuddy (Electron 37) exposes its web content only through Chromium's
// native UIA provider on the owned Chrome_RenderWidgetHostHWND. The managed
// System.Windows.Automation client cannot read that provider, and the first
// managed AutomationElement created in a process registers client-side
// proxies that hide it from every later UIA request in the same process.
// WorkBuddy therefore uses UIAutomationCore's COM client only. The wrappers
// mirror the small AutomationElement surface used by the composer helpers.
[StructLayout(LayoutKind.Sequential)]
public struct WorkBuddyUiaPoint {
  public int X;
  public int Y;
}

[ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IWorkBuddyUia {
  void CompareElements();
  void CompareRuntimeIds();
  void GetRootElement();
  IWorkBuddyUiaElement ElementFromHandle(IntPtr window);
  IWorkBuddyUiaElement ElementFromPoint(WorkBuddyUiaPoint point);
  IWorkBuddyUiaElement GetFocusedElement();
  void GetRootElementBuildCache();
  void ElementFromHandleBuildCache();
  void ElementFromPointBuildCache();
  void GetFocusedElementBuildCache();
  void CreateTreeWalker();
  IWorkBuddyUiaTreeWalker GetControlViewWalker();
  IWorkBuddyUiaTreeWalker GetContentViewWalker();
  IWorkBuddyUiaTreeWalker GetRawViewWalker();
  IWorkBuddyUiaCondition GetRawViewCondition();
  void GetControlViewCondition();
  void GetContentViewCondition();
  void CreateCacheRequest();
  void CreateTrueCondition();
  void CreateFalseCondition();
  IWorkBuddyUiaCondition CreatePropertyCondition(int propertyId, [MarshalAs(UnmanagedType.Struct)] object value);
  IWorkBuddyUiaCondition CreatePropertyConditionEx(int propertyId, [MarshalAs(UnmanagedType.Struct)] object value, int flags);
  void CreateAndCondition();
  void CreateAndConditionFromArray();
  void CreateAndConditionFromNativeArray();
  IWorkBuddyUiaCondition CreateOrCondition(IWorkBuddyUiaCondition left, IWorkBuddyUiaCondition right);
}

[ComImport, Guid("352ffba8-0973-437c-a61f-f64cafd81df9"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IWorkBuddyUiaCondition {}

[ComImport, Guid("4042c624-389c-4afc-a630-9df854a541fc"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IWorkBuddyUiaTreeWalker {
  IWorkBuddyUiaElement GetParentElement(IWorkBuddyUiaElement element);
  IWorkBuddyUiaElement GetFirstChildElement(IWorkBuddyUiaElement element);
  IWorkBuddyUiaElement GetLastChildElement(IWorkBuddyUiaElement element);
  IWorkBuddyUiaElement GetNextSiblingElement(IWorkBuddyUiaElement element);
}

[ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IWorkBuddyUiaElement {
  void SetFocus();
  [return: MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_I4)]
  int[] GetRuntimeId();
  void FindFirst();
  IWorkBuddyUiaElementArray FindAll(int scope, IWorkBuddyUiaCondition condition);
  void FindFirstBuildCache();
  void FindAllBuildCache();
  void BuildUpdatedCache();
  [return: MarshalAs(UnmanagedType.Struct)]
  object GetCurrentPropertyValue(int propertyId);
  void GetCurrentPropertyValueEx();
  void GetCachedPropertyValue();
  void GetCachedPropertyValueEx();
  void GetCurrentPatternAs();
  void GetCachedPatternAs();
  [return: MarshalAs(UnmanagedType.IUnknown)]
  object GetCurrentPattern(int patternId);
}

[ComImport, Guid("a94cd8b1-0844-4cd6-9d2d-640537ab39e9"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IWorkBuddyUiaValuePattern {
  void SetValue([MarshalAs(UnmanagedType.BStr)] string value);
  [return: MarshalAs(UnmanagedType.BStr)]
  string GetCurrentValue();
  [return: MarshalAs(UnmanagedType.Bool)]
  bool GetCurrentIsReadOnly();
}

[ComImport, Guid("32eba289-3583-42c9-9c59-3b6d9a1e9b6a"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IWorkBuddyUiaTextPattern {
  void RangeFromPoint();
  IWorkBuddyUiaTextRange RangeFromChild(IWorkBuddyUiaElement child);
  void GetSelection();
  void GetVisibleRanges();
  IWorkBuddyUiaTextRange GetDocumentRange();
}

[ComImport, Guid("a543cc6a-f4ae-494b-8239-c814481187a8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IWorkBuddyUiaTextRange {
  void Clone();
  void Compare();
  void CompareEndpoints();
  void ExpandToEnclosingUnit();
  void FindAttribute();
  void FindText();
  [return: MarshalAs(UnmanagedType.Struct)]
  object GetAttributeValue(int attributeId);
  void GetBoundingRectangles();
  IWorkBuddyUiaElement GetEnclosingElement();
  [return: MarshalAs(UnmanagedType.BStr)]
  string GetText(int maxLength);
  void Move();
  void MoveEndpointByUnit();
  void MoveEndpointByRange();
  void Select();
  void AddToSelection();
  void RemoveFromSelection();
  void ScrollIntoView();
  IWorkBuddyUiaElementArray GetChildren();
}

[ComImport, Guid("14314595-b4bc-4055-95f2-58f2e42c9855"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IWorkBuddyUiaElementArray {
  int GetLength();
  IWorkBuddyUiaElement GetElement(int index);
}

[ComImport, Guid("828055ad-355b-4435-86d5-3b51c14a9b1b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IWorkBuddyUiaLegacyPattern {
  void Select(int flags);
  void DoDefaultAction();
  void SetValue();
  int GetCurrentChildId();
  void GetCurrentName();
  [return: MarshalAs(UnmanagedType.BStr)]
  string GetCurrentValue();
  void GetCurrentDescription();
  uint GetCurrentRole();
  uint GetCurrentState();
}

[ComImport, Guid("fb377fbe-8ea6-46d5-9c73-6499642d3059"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IWorkBuddyUiaInvokePattern {
  void Invoke();
}

public sealed class WorkBuddyUiaControlType {
  public WorkBuddyUiaControlType(int id) { Id = id; }
  public int Id { get; private set; }
  public string ProgrammaticName { get { return "ControlType." + Id; } }
}

public sealed class WorkBuddyUiaRect {
  public WorkBuddyUiaRect(double left, double top, double width, double height) {
    Left = left; Top = top; Width = width; Height = height;
  }
  public double Left { get; private set; }
  public double Top { get; private set; }
  public double Width { get; private set; }
  public double Height { get; private set; }
  public double Right { get { return Left + Width; } }
  public double Bottom { get { return Top + Height; } }
  public bool IsEmpty { get { return !(Width > 0 && Height > 0); } }
}

public sealed class WorkBuddyUiaValueState {
  private readonly IWorkBuddyUiaValuePattern pattern;
  internal WorkBuddyUiaValueState(IWorkBuddyUiaValuePattern pattern) { this.pattern = pattern; }
  public string Value { get { return pattern.GetCurrentValue(); } }
  public bool IsReadOnly { get { return pattern.GetCurrentIsReadOnly(); } }
}

public sealed class WorkBuddyUiaValuePattern {
  private readonly IWorkBuddyUiaValuePattern pattern;
  internal WorkBuddyUiaValuePattern(IWorkBuddyUiaValuePattern pattern) { this.pattern = pattern; }
  public WorkBuddyUiaValueState Current { get { return new WorkBuddyUiaValueState(pattern); } }
}

public sealed class WorkBuddyUiaTextRange {
  private readonly IWorkBuddyUiaTextRange range;
  internal WorkBuddyUiaTextRange(IWorkBuddyUiaTextRange range) { this.range = range; }
  public string GetText(int maxLength) { return range.GetText(maxLength); }
  public object GetAttributeValue(object attribute) { return range.GetAttributeValue(WorkBuddyUia.IdOf(attribute)); }
  public WorkBuddyUiaElement GetEnclosingElement() { return new WorkBuddyUiaElement(range.GetEnclosingElement()); }
  public WorkBuddyUiaElement[] GetChildren() {
    IWorkBuddyUiaElementArray children = range.GetChildren();
    if (children == null) { throw new InvalidOperationException("Composer text children unavailable"); }
    int count = children.GetLength();
    if (count < 0 || count > 64) { throw new InvalidOperationException("Composer text child budget exceeded"); }
    WorkBuddyUiaElement[] result = new WorkBuddyUiaElement[count];
    for (int i = 0; i < count; i++) { result[i] = new WorkBuddyUiaElement(children.GetElement(i)); }
    return result;
  }
}

public sealed class WorkBuddyUiaTextPattern {
  private readonly IWorkBuddyUiaTextPattern pattern;
  internal WorkBuddyUiaTextPattern(IWorkBuddyUiaTextPattern pattern) { this.pattern = pattern; }
  public WorkBuddyUiaTextRange DocumentRange { get { return new WorkBuddyUiaTextRange(pattern.GetDocumentRange()); } }
  public WorkBuddyUiaTextRange RangeFromChild(WorkBuddyUiaElement child) { return new WorkBuddyUiaTextRange(pattern.RangeFromChild(child.Native)); }
}

public sealed class WorkBuddyUiaLegacyState {
  private readonly IWorkBuddyUiaLegacyPattern pattern;
  internal WorkBuddyUiaLegacyState(IWorkBuddyUiaLegacyPattern pattern) { this.pattern = pattern; }
  public string Value { get { return pattern.GetCurrentValue(); } }
  public uint Role { get { return pattern.GetCurrentRole(); } }
  public uint State { get { return pattern.GetCurrentState(); } }
}

public sealed class WorkBuddyUiaLegacyPattern {
  private readonly IWorkBuddyUiaLegacyPattern pattern;
  internal WorkBuddyUiaLegacyPattern(IWorkBuddyUiaLegacyPattern pattern) { this.pattern = pattern; }
  public WorkBuddyUiaLegacyState Current { get { return new WorkBuddyUiaLegacyState(pattern); } }
}

public sealed class WorkBuddyUiaInvokePattern {
  private readonly IWorkBuddyUiaInvokePattern pattern;
  internal WorkBuddyUiaInvokePattern(IWorkBuddyUiaInvokePattern pattern) { this.pattern = pattern; }
  public void Invoke() { pattern.Invoke(); }
}

public sealed class WorkBuddyUiaCurrent {
  private readonly WorkBuddyUiaElement element;
  internal WorkBuddyUiaCurrent(WorkBuddyUiaElement element) { this.element = element; }
  public WorkBuddyUiaControlType ControlType { get { return new WorkBuddyUiaControlType(element.Int(30003)); } }
  public WorkBuddyUiaRect BoundingRectangle {
    get {
      double[] rect = element.Property(30001) as double[];
      if (rect == null || rect.Length != 4) { return new WorkBuddyUiaRect(0, 0, 0, 0); }
      return new WorkBuddyUiaRect(rect[0], rect[1], rect[2], rect[3]);
    }
  }
  public int ProcessId { get { return element.Int(30002); } }
  public string Name { get { return element.Text(30005); } }
  public bool HasKeyboardFocus { get { return element.FocusFlag(); } }
  public bool IsKeyboardFocusable { get { return element.KeyboardReachable(); } }
  public bool IsEnabled { get { return element.EnabledFlag(); } }
  public string ClassName { get { return element.Text(30012); } }
  public int NativeWindowHandle { get { return element.Int(30020); } }
  public bool IsOffscreen { get { return element.OffscreenFlag(); } }
}

public sealed class WorkBuddyUiaElement {
  private readonly IWorkBuddyUiaElement element;
  internal WorkBuddyUiaElement(IWorkBuddyUiaElement element) { this.element = element; }
  internal IWorkBuddyUiaElement Native { get { return element; } }
  public WorkBuddyUiaCurrent Current { get { return new WorkBuddyUiaCurrent(this); } }
  public int[] GetRuntimeId() {
    try { return element.GetRuntimeId() ?? new int[0]; } catch { return new int[0]; }
  }
  public void SetFocus() {
    try { element.SetFocus(); }
    catch {
      uint state;
      if(!TryLegacyTextState(out state) || (state & 0x100000U)==0) throw;
      IWorkBuddyUiaLegacyPattern legacy=element.GetCurrentPattern(10018) as IWorkBuddyUiaLegacyPattern;
      if(legacy==null) throw;
      legacy.Select(1); // SELFLAG_TAKEFOCUS, never SetValue/keyboard/clipboard
    }
  }
  public object GetCurrentPropertyValue(object property) { return Property(WorkBuddyUia.IdOf(property)); }
  public bool TryGetCurrentPattern(object pattern, out object patternObject) {
    patternObject = null;
    int patternId = WorkBuddyUia.IdOf(pattern);
    object raw;
    try { raw = element.GetCurrentPattern(patternId); }
    catch (Exception e) { WorkBuddyUia.PatternFailure(Marshal.GetHRForException(e)); return false; }
    if (raw == null) { return false; }
    if (patternId == 10002) {
      IWorkBuddyUiaValuePattern value = raw as IWorkBuddyUiaValuePattern;
      if (value != null) { patternObject = new WorkBuddyUiaValuePattern(value); }
    } else if (patternId == 10014) {
      IWorkBuddyUiaTextPattern text = raw as IWorkBuddyUiaTextPattern;
      if (text != null) { patternObject = new WorkBuddyUiaTextPattern(text); }
    } else if (patternId == 10018) {
      IWorkBuddyUiaLegacyPattern legacy = raw as IWorkBuddyUiaLegacyPattern;
      if (legacy != null) { patternObject = new WorkBuddyUiaLegacyPattern(legacy); }
    } else if (patternId == 10000) {
      IWorkBuddyUiaInvokePattern invoke = raw as IWorkBuddyUiaInvokePattern;
      if (invoke != null) { patternObject = new WorkBuddyUiaInvokePattern(invoke); }
    }
    if(patternObject==null) WorkBuddyUia.PatternInterfaceFailure();
    return patternObject != null;
  }
  internal object Property(int propertyId) {
    try { return element.GetCurrentPropertyValue(propertyId); }
    catch (Exception e) { WorkBuddyUia.PropertyFailure(Marshal.GetHRForException(e)); return null; }
  }
  public bool TryLegacyTextState(out uint state) {
    state=0;
    object available=Property(30090), role=Property(30095), raw=Property(30096);
    if (!(available is bool) || !(bool)available ||
        (!(role is int) && !(role is uint)) || (role is int ? (int)role!=42 : (uint)role!=42) ||
        (!(raw is int) && !(raw is uint))) return false;
    try {
      state=Convert.ToUInt32(raw);
      // Unavailable/read-only/invisible/offscreen/protected never authorizes input.
      return (state & 0x20018041U)==0;
    } catch { return false; }
  }
  public bool HasWritableLegacyText() { uint state; return TryLegacyTextState(out state); }
  internal bool FocusFlag() {
    if (Flag(30008)) return true;
    uint state; return TryLegacyTextState(out state) && (state & 4U)!=0;
  }
  internal bool KeyboardReachable() {
    if (Flag(30009) || FocusFlag()) return true;
    uint state; return TryLegacyTextState(out state) && (state & 0x100000U)!=0;
  }
  internal bool EnabledFlag() {
    object value=Property(30010);
    if(value is bool) return (bool)value; // explicit disabled cannot be overridden
    uint state; return TryLegacyTextState(out state);
  }
  internal bool OffscreenFlag() {
    object value=Property(30022);
    if(value is bool) return (bool)value;
    uint state; return !TryLegacyTextState(out state); // unknown is not visible
  }
  internal int Int(int propertyId) {
    object value = Property(propertyId);
    return value is int ? (int)value : 0;
  }
  internal bool Flag(int propertyId) {
    object value = Property(propertyId);
    if (!(value is bool)) WorkBuddyUia.UnknownFlag();
    return value is bool && (bool)value;
  }
  internal string Text(int propertyId) {
    return Property(propertyId) as string ?? string.Empty;
  }
}

public static class WorkBuddyUia {
  private static IWorkBuddyUia automation;
  public static int Visited, Discovered, TimeTruncated, NodeTruncated, DepthTruncated;
  public static int ProviderQueries, ProviderMatches, ProviderTruncated, ProviderFailures;
  public static int PropertyFailures, PatternFailures, PatternInterfaceFailures, UnknownFlags, LastHResult;
  public static void ResetDiagnostics() {
    Visited=Discovered=TimeTruncated=NodeTruncated=DepthTruncated=0;
    ProviderQueries=ProviderMatches=ProviderTruncated=ProviderFailures=0;
    PropertyFailures=PatternFailures=PatternInterfaceFailures=UnknownFlags=LastHResult=0;
  }
  internal static void PropertyFailure(int code){PropertyFailures++;LastHResult=code;}
  internal static void PatternFailure(int code){PatternFailures++;LastHResult=code;}
  internal static void PatternInterfaceFailure(){PatternInterfaceFailures++;}
  internal static void UnknownFlag(){UnknownFlags++;}
  public static string DiagnosticSummary() {
    return "visited="+Visited+",discovered="+Discovered+",timeCut="+TimeTruncated+
      ",nodeCut="+NodeTruncated+",depthCut="+DepthTruncated+",queries="+ProviderQueries+
      ",queryMatches="+ProviderMatches+",queryCut="+ProviderTruncated+",queryFailures="+ProviderFailures+
      ",propertyFailures="+PropertyFailures+",patternFailures="+PatternFailures+
      ",patternInterfaces="+PatternInterfaceFailures+",unknownFlags="+UnknownFlags+",hr="+LastHResult;
  }

  // Provider-side query on THIS app window only. Never scan desktop descendants.
  // Matching alone grants nothing: caller revalidates semantics/owner/writability.
  public static WorkBuddyUiaElement[] FindEditable(IntPtr window,int budgetMs,int maxMatches) {
    ProviderQueries++;
    var clock=System.Diagnostics.Stopwatch.StartNew();
    try {
      IWorkBuddyUiaElement root=Automation.ElementFromHandle(window);
      if(root==null) return new WorkBuddyUiaElement[0];
      IWorkBuddyUiaCondition edit=Automation.CreatePropertyCondition(30003,50004);
      IWorkBuddyUiaCondition textbox=Automation.CreatePropertyCondition(30101,"textbox");
      IWorkBuddyUiaCondition legacy=Automation.CreatePropertyCondition(30095,42);
      IWorkBuddyUiaCondition prose=Automation.CreatePropertyConditionEx(30012,"ProseMirror",2);
      IWorkBuddyUiaCondition tiptap=Automation.CreatePropertyConditionEx(30012,"tiptap",2);
      IWorkBuddyUiaCondition condition=Automation.CreateOrCondition(
        Automation.CreateOrCondition(Automation.CreateOrCondition(edit,textbox),legacy),
        Automation.CreateOrCondition(prose,tiptap));
      IWorkBuddyUiaElementArray found=root.FindAll(4,condition); // TreeScope_Descendants, not desktop
      int count=found==null?0:found.GetLength();
      ProviderMatches+=Math.Max(0,count);
      if(count<0 || count>maxMatches || clock.ElapsedMilliseconds>budgetMs) {
        ProviderTruncated++;return new WorkBuddyUiaElement[0];
      }
      var result=new List<WorkBuddyUiaElement>();
      for(int i=0;i<count;i++) {
        if(clock.ElapsedMilliseconds>=budgetMs){ProviderTruncated++;return new WorkBuddyUiaElement[0];}
        IWorkBuddyUiaElement element=found.GetElement(i);
        if(element!=null)result.Add(new WorkBuddyUiaElement(element));
      }
      if(clock.ElapsedMilliseconds>budgetMs){ProviderTruncated++;return new WorkBuddyUiaElement[0];}
      return result.ToArray();
    } catch(Exception e){ProviderFailures++;LastHResult=Marshal.GetHRForException(e);return new WorkBuddyUiaElement[0];}
  }

  public static WorkBuddyUiaElement[] FocusedAncestors(IntPtr window) {
    var result=new List<WorkBuddyUiaElement>();
    try {
      IWorkBuddyUiaElement current=Automation.GetFocusedElement();
      IWorkBuddyUiaTreeWalker walker=Automation.GetRawViewWalker();
      for(int depth=0;depth<12&&current!=null;depth++) {
        var wrapped=new WorkBuddyUiaElement(current);
        if(!IsOwnedBy(window,wrapped)) break;
        result.Add(wrapped);
        if((long)HandleOf(current)==window.ToInt64())break;
        current=walker.GetParentElement(current);
      }
    } catch(Exception e){PropertyFailure(Marshal.GetHRForException(e));}
    return result.ToArray();
  }

  [DllImport("user32.dll")]
  private static extern bool IsChild(IntPtr parent, IntPtr child);

  private static IWorkBuddyUia Automation {
    get {
      if (automation != null) { return automation; }
      // CUIAutomation8 first, then the Windows 7 CUIAutomation class.
      foreach (string clsid in new[] { "e22ad333-b25f-460c-83d0-0581107395c9", "ff48dba4-60ef-4201-aa87-54103eef594e" }) {
        try {
          automation = (IWorkBuddyUia)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid(clsid), true));
          return automation;
        } catch {}
      }
      throw new InvalidOperationException("Windows UI Automation COM client is unavailable");
    }
  }

  // Accepts managed AutomationPattern/AutomationProperty/AutomationTextAttribute
  // identifiers (only their numeric Id is read) or a raw UIA identifier.
  public static int IdOf(object identifier) {
    if (identifier == null) { return 0; }
    if (identifier is int) { return (int)identifier; }
    System.Reflection.PropertyInfo id = identifier.GetType().GetProperty("Id");
    if (id == null) { return 0; }
    object value = id.GetValue(identifier, null);
    return value is int ? (int)value : 0;
  }

  private static WorkBuddyUiaElement Wrap(IWorkBuddyUiaElement element) {
    return element == null ? null : new WorkBuddyUiaElement(element);
  }

  private static int HandleOf(IWorkBuddyUiaElement element) {
    object value = element.GetCurrentPropertyValue(30020);
    return value is int ? (int)value : 0;
  }

  public static WorkBuddyUiaElement FromHandle(IntPtr window) {
    try { return Wrap(Automation.ElementFromHandle(window)); } catch { return null; }
  }

  public static WorkBuddyUiaElement FocusedElement() {
    try { return Wrap(Automation.GetFocusedElement()); } catch { return null; }
  }

  public static bool IsOwnedBy(IntPtr window, WorkBuddyUiaElement element) {
    if (window == IntPtr.Zero || element == null) { return false; }
    try {
      IWorkBuddyUiaTreeWalker walker = Automation.GetRawViewWalker();
      IWorkBuddyUiaElement current = element.Native;
      for (int depth = 0; depth < 64 && current != null; depth += 1) {
        IntPtr handle = new IntPtr(HandleOf(current));
        if (handle != IntPtr.Zero && (handle == window || IsChild(window, handle))) { return true; }
        current = walker.GetParentElement(current);
      }
    } catch {}
    return false;
  }

  // Same bounded breadth-first contract as Find-BoundedComposerElements.
  public static WorkBuddyUiaElement[] FindBounded(IntPtr window, bool rawView, int budgetMs, int maxNodes) {
    List<WorkBuddyUiaElement> matches = new List<WorkBuddyUiaElement>();
    System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();
    Queue<KeyValuePair<IWorkBuddyUiaElement, int>> pending = new Queue<KeyValuePair<IWorkBuddyUiaElement, int>>();
    IWorkBuddyUiaTreeWalker walker;
    try {
      walker = rawView ? Automation.GetRawViewWalker() : Automation.GetControlViewWalker();
      IWorkBuddyUiaElement root = Automation.ElementFromHandle(window);
      if (root == null) { return matches.ToArray(); }
      pending.Enqueue(new KeyValuePair<IWorkBuddyUiaElement, int>(root, 0));
    } catch { return matches.ToArray(); }
    int discovered = 1;
    while (pending.Count > 0 && clock.ElapsedMilliseconds < budgetMs) {
      KeyValuePair<IWorkBuddyUiaElement, int> node = pending.Dequeue();
      Visited++;
      if (node.Value > 0) {
        int typeId = 0;
        try {
          object type = node.Key.GetCurrentPropertyValue(30003);
          if (type is int) { typeId = (int)type; }
        } catch {}
        // Edit, Button, Group, Custom, Document, Pane, Text (conversation header).
        if (typeId == 50004 || typeId == 50000 || typeId == 50026 ||
            typeId == 50025 || typeId == 50030 || typeId == 50033 || typeId == 50020) {
          matches.Add(new WorkBuddyUiaElement(node.Key));
        }
      }
      if(node.Value>=48){DepthTruncated++;continue;}
      if(discovered>=maxNodes){NodeTruncated++;continue;}
      try {
        IWorkBuddyUiaElement child = walker.GetFirstChildElement(node.Key);
        while (child != null && discovered < maxNodes && clock.ElapsedMilliseconds < budgetMs) {
          pending.Enqueue(new KeyValuePair<IWorkBuddyUiaElement, int>(child, node.Value + 1));
          discovered += 1;
          child = walker.GetNextSiblingElement(child);
        }
        if(child!=null) {
          if(discovered>=maxNodes)NodeTruncated++;
          if(clock.ElapsedMilliseconds>=budgetMs)TimeTruncated++;
        }
      } catch {}
    }
    Discovered+=discovered;
    if(pending.Count>0 && clock.ElapsedMilliseconds>=budgetMs)TimeTruncated++;
    return matches.ToArray();
  }

  // Same lower-surface point probe as Find-PointComposerElements: each hit and
  // its ancestors up to the target window.
  public static WorkBuddyUiaElement[] FindAtPoints(IntPtr window, double[] xRatios, double[] yRatios, int budgetMs) {
    List<WorkBuddyUiaElement> matches = new List<WorkBuddyUiaElement>();
    System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();
    try {
      IWorkBuddyUiaElement root = Automation.ElementFromHandle(window);
      if (root == null) { return matches.ToArray(); }
      object value = root.GetCurrentPropertyValue(30001);
      double[] rect = value as double[];
      if (rect == null || rect.Length != 4 || !(rect[2] > 0 && rect[3] > 0)) { return matches.ToArray(); }
      IWorkBuddyUiaTreeWalker walker = Automation.GetControlViewWalker();
      foreach (double yRatio in yRatios) {
        foreach (double xRatio in xRatios) {
          if (clock.ElapsedMilliseconds >= budgetMs) { return matches.ToArray(); }
          WorkBuddyUiaPoint point = new WorkBuddyUiaPoint();
          point.X = (int)Math.Round(rect[0] + rect[2] * xRatio);
          point.Y = (int)Math.Round(rect[1] + rect[3] * yRatio);
          try {
            IWorkBuddyUiaElement element = Automation.ElementFromPoint(point);
            for (int depth = 0; depth < 16 && element != null && clock.ElapsedMilliseconds < budgetMs; depth += 1) {
              if ((long)HandleOf(element) == window.ToInt64()) { break; }
              matches.Add(new WorkBuddyUiaElement(element));
              element = walker.GetParentElement(element);
            }
          } catch {}
        }
      }
    } catch {}
    return matches.ToArray();
  }
}
"@
Write-ComposerProgress 'ready_runtime'

function Write-ComposerReply([hashtable]$reply) {
  [Console]::Out.WriteLine(($reply | ConvertTo-Json -Compress))
  [Console]::Out.Flush()
}

function Normalize-Label([string]$value) {
  if ([string]::IsNullOrWhiteSpace($value)) { return '' }
  return (($value -replace '\s+', ' ').Trim())
}

function Get-MonotonicMilliseconds {
  $ticks = [System.Diagnostics.Stopwatch]::GetTimestamp()
  $frequency = [System.Diagnostics.Stopwatch]::Frequency
  return [int64](($ticks * 1000) / $frequency)
}

function Find-DescendantsByControlTypes($root, [array]$controlTypes) {
  $conditions = @()
  foreach ($controlType in @($controlTypes)) {
    $conditions += [System.Windows.Automation.PropertyCondition]::new(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
      $controlType
    )
  }
  if ($conditions.Count -eq 0) { return @() }
  $condition = if ($conditions.Count -eq 1) {
    $conditions[0]
  } else {
    [System.Windows.Automation.OrCondition]::new(
      [System.Windows.Automation.Condition[]]$conditions
    )
  }
  return $root.FindAll(
    [System.Windows.Automation.TreeScope]::Descendants,
    $condition
  )
}

function Test-LabelMatch([string]$actualValue, [string]$expectedValue) {
  $actual = Normalize-Label $actualValue
  $expected = Normalize-Label $expectedValue
  if (-not $actual -or $expected.Length -lt 2) { return $false }
  if ($actual.Equals($expected, [StringComparison]::OrdinalIgnoreCase)) { return $true }
  if ($expected.Length -lt 12) { return $false }
  $prefix = $expected.Substring(0, [Math]::Min($expected.Length, 24))
  return $actual.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Test-SelectedSessionTitle($root, [string]$expectedTitle) {
  $expected = Normalize-Label $expectedTitle
  if ($expected.Length -lt 2) { return $false }
  $rootName = Normalize-Label $root.Current.Name
  if ($rootName -and $rootName.Contains($expected)) { return $true }

  $rootRect = $root.Current.BoundingRectangle
  if (-not (Test-FiniteWindowRectangle $rootRect)) { return $false }
  $headerBottom = $rootRect.Top + [Math]::Min(
    [double]160,
    [Math]::Max([double]80, [double]($rootRect.Height * 0.18))
  )
  $contentLeft = $rootRect.Left + [Math]::Min(
    [double]280,
    [Math]::Max([double]100, [double]($rootRect.Width * 0.16))
  )
  $all = Find-DescendantsByControlTypes $root @(
    [System.Windows.Automation.ControlType]::Text
  )
  for ($index = 0; $index -lt $all.Count; $index += 1) {
    $element = $all.Item($index)
    if ($element.Current.IsOffscreen -or
        $element.Current.ControlType.Id -ne [System.Windows.Automation.ControlType]::Text.Id -or
        -not (Test-LabelMatch ([string]$element.Current.Name) $expected)) { continue }
    $rect = $element.Current.BoundingRectangle
    if ($rect.Top -le $headerBottom -and $rect.Left -ge $contentLeft) { return $true }
  }
  return $false
}

function Test-FiniteWindowRectangle($rect) {
  if ($null -eq $rect) { return $false }
  foreach ($value in @($rect.Left, $rect.Top, $rect.Width, $rect.Height)) {
    $number = [double]$value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) {
      return $false
    }
  }
  return $rect.Width -gt 0 -and $rect.Height -gt 0
}

function Get-CodexWindows {
  $processes = @(Get-Process -Name ChatGPT -ErrorAction SilentlyContinue)
  $processIds = @($processes | ForEach-Object { [int]$_.Id })
  if ($processIds.Count -eq 0) { throw 'No running ChatGPT（Codex） process was found' }

  $desktop = [System.Windows.Automation.AutomationElement]::RootElement
  $topLevel = $desktop.FindAll(
    [System.Windows.Automation.TreeScope]::Children,
    [System.Windows.Automation.Condition]::TrueCondition
  )
  $windows = @()
  $seenHandles = @{}
  for ($index = 0; $index -lt $topLevel.Count; $index += 1) {
    $root = $topLevel.Item($index)
    $handle = [int64]$root.Current.NativeWindowHandle
    if ($processIds -notcontains [int]$root.Current.ProcessId -or $handle -eq 0) { continue }
    $rect = $root.Current.BoundingRectangle
    $hasFiniteRectangle = Test-FiniteWindowRectangle $rect
    if ($hasFiniteRectangle -and -not $root.Current.IsOffscreen -and
        ($rect.Width -lt 420 -or $rect.Height -lt 320)) { continue }
    # A minimized Store-packaged ChatGPT window can stay in UIA with a valid
    # handle while every BoundingRectangle field is +/-Infinity. Keep it as a
    # restore candidate; ActivateWindow applies SW_RESTORE before foregrounding.
    $area = if ($hasFiniteRectangle) {
      [Math]::Max([double]1, [double]($rect.Width * $rect.Height))
    } else {
      [double]1
    }
    $windows += [pscustomobject]@{ Root = $root; Area = $area }
    $seenHandles[$handle] = $true
  }

  # Minimized Chromium windows disappear from RootElement's children. The
  # process main-window handle remains valid and can be restored by ActivateWindow.
  foreach ($process in $processes) {
    $handle = [int64]$process.MainWindowHandle
    if ($handle -eq 0 -or $seenHandles.ContainsKey($handle)) { continue }
    try {
      $root = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$handle)
      if ($null -eq $root) { continue }
      $windows += [pscustomobject]@{ Root = $root; Area = [double]1 }
      $seenHandles[$handle] = $true
    } catch {}
  }
  if ($windows.Count -eq 0) { throw 'No ChatGPT（Codex） window was found' }
  return @($windows | Sort-Object Area -Descending)
}

function Get-OrLaunchCodexWindows {
  try {
    $windows = @(Get-CodexWindows)
    if ($windows.Count -gt 0) { return $windows }
  } catch {}

  try {
    Start-Process -FilePath 'explorer.exe' `
      -ArgumentList 'shell:AppsFolder\OpenAI.Codex_2p2nqsd0c76g0!App'
  } catch {
    throw "ChatGPT（Codex） has no visible window and could not be launched: $($_.Exception.Message)"
  }

  $deadline = (Get-MonotonicMilliseconds) + 4500
  do {
    Start-Sleep -Milliseconds 100
    try {
      $windows = @(Get-CodexWindows)
      if ($windows.Count -gt 0) { return $windows }
    } catch {}
  } while ((Get-MonotonicMilliseconds) -lt $deadline)
  throw 'ChatGPT（Codex） did not expose a visible window after launch'
}

function Wait-CodexWindowRoot($root, [IntPtr]$handle, [int]$timeoutMs) {
  $deadline = (Get-MonotonicMilliseconds) + [Math]::Max(200, $timeoutMs)
  do {
    try {
      if (Test-FiniteWindowRectangle $root.Current.BoundingRectangle) { return $root }
    } catch {}
    try {
      $freshRoot = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
      if ($null -ne $freshRoot -and
          (Test-FiniteWindowRectangle $freshRoot.Current.BoundingRectangle)) {
        return $freshRoot
      }
    } catch {}
    if ((Get-MonotonicMilliseconds) -ge $deadline) { break }
    Start-Sleep -Milliseconds 40
  } while ($true)
  throw 'ChatGPT（Codex）窗口已激活，但可访问性窗口坐标尚未就绪'
}

function Ensure-CodexForeground($root, [string]$context) {
  $handle = [IntPtr]$root.Current.NativeWindowHandle
  if (-not (Test-FiniteWindowRectangle $root.Current.BoundingRectangle)) {
    [void][CodexVoiceNative]::RestoreWindow($handle)
    Start-Sleep -Milliseconds 120
  }
  $activated = [CodexVoiceNative]::ActivateWindow($handle)
  if (-not $activated) {
    try {
      $shell = New-Object -ComObject WScript.Shell
      [void]$shell.AppActivate([int]$root.Current.ProcessId)
      Start-Sleep -Milliseconds 80
      $activated = [CodexVoiceNative]::ActivateWindow($handle)
    } catch {}
  }
  if (-not $activated) {
    throw "ChatGPT（Codex）主窗口无法切换到前台 $context"
  }
  # Chromium can expose the previous/offscreen accessibility tree briefly
  # after Windows brings an occluded window to the foreground.
  Start-Sleep -Milliseconds 120
  return Wait-CodexWindowRoot $root $handle 1200
}

function Show-CodexSidebar($root) {
  $all = Find-DescendantsByControlTypes $root @(
    [System.Windows.Automation.ControlType]::Button
  )
  for ($index = 0; $index -lt $all.Count; $index += 1) {
    $element = $all.Item($index)
    $name = Normalize-Label ([string]$element.Current.Name)
    if ($element.Current.ControlType.Id -ne [System.Windows.Automation.ControlType]::Button.Id -or
        $element.Current.IsOffscreen -or
        $name -notmatch '^(?i:显示边栏|show sidebar)$') { continue }
    $invoke = $null
    if ($element.TryGetCurrentPattern(
        [System.Windows.Automation.InvokePattern]::Pattern,
        [ref]$invoke
    )) {
      $invoke.Invoke()
      Start-Sleep -Milliseconds 120
    }
    return
  }
}

function Get-ClaudeWindows {
  $processes = @(Get-Process -Name claude -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 })
  if ($processes.Count -eq 0) { return @() }

  $windows = @()
  foreach ($process in $processes) {
    $mainHandle = [IntPtr]$process.MainWindowHandle
    try {
      $mainRoot = [System.Windows.Automation.AutomationElement]::FromHandle($mainHandle)
      $rect = $mainRoot.Current.BoundingRectangle
      if ((Test-FiniteWindowRectangle $rect) -and -not $mainRoot.Current.IsOffscreen -and
          $rect.Width -ge 420 -and $rect.Height -ge 320) {
        $windows += [pscustomobject]@{
          Root = $mainRoot
          RootHandle = [int64]$mainHandle
          WindowHandle = [int64]$mainHandle
          ProcessId = [int]$process.Id
          Area = [double]($rect.Width * $rect.Height)
        }
        continue
      }
    } catch {}

    $rendererHandles = @([CodexVoiceNative]::FindChildWindowsByClass(
      $mainHandle,
      'Chrome_RenderWidgetHostHWND'
    ))
    foreach ($rendererHandle in $rendererHandles) {
      try {
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($rendererHandle)
        if ($null -eq $root -or $root.Current.IsOffscreen) { continue }
        $rect = $root.Current.BoundingRectangle
        if (-not (Test-FiniteWindowRectangle $rect) -or
            $rect.Width -lt 420 -or $rect.Height -lt 320) { continue }
        $windows += [pscustomobject]@{
          Root = $root
          RootHandle = [int64]$rendererHandle
          WindowHandle = [int64]$mainHandle
          ProcessId = [int]$process.Id
          Area = [double]($rect.Width * $rect.Height)
        }
      } catch {}
    }
  }
  return @($windows | Sort-Object Area -Descending)
}

function Get-RestoredClaudeWindows {
  $windows = @(Get-ClaudeWindows)
  if ($windows.Count -gt 0) { return $windows }

  $desktopProcesses = @(Get-Process -Name claude -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 })
  if ($desktopProcesses.Count -eq 0) { return @() }
  # Minimized Chromium windows can expose an empty UIA rectangle. Restore the
  # existing window and reacquire its UIA tree before checking the session.
  foreach ($process in $desktopProcesses) {
    [void][CodexVoiceNative]::RestoreWindow([IntPtr]$process.MainWindowHandle)
  }
  $deadline = (Get-MonotonicMilliseconds) + 2000
  do {
    Start-Sleep -Milliseconds 100
    $windows = @(Get-ClaudeWindows)
    if ($windows.Count -gt 0) { return $windows }
  } while ((Get-MonotonicMilliseconds) -lt $deadline)
  throw 'Claude Desktop is running, but its window could not be restored; open Claude and retry'
}

function Get-OrLaunchClaudeWindows {
  $windows = @(Get-RestoredClaudeWindows)
  if ($windows.Count -gt 0) { return $windows }

  $appIds = @()
  try {
    $appIds = @(Get-StartApps -ErrorAction Stop |
      Where-Object { $_.Name -eq 'Claude' -or $_.AppID -match '(?i)Claude|Anthropic' } |
      Sort-Object @{ Expression = {
        if ($_.AppID -eq 'com.squirrel.AnthropicClaude.claude') { 0 } else { 1 }
      }} |
      ForEach-Object { [string]$_.AppID } |
      Select-Object -Unique)
  } catch {}
  if ($appIds.Count -eq 0) {
    $appIds = @(
      'com.squirrel.AnthropicClaude.claude',
      'Claude_pzs8sxrjxfjjc!Claude'
    )
  }

  $deadline = (Get-MonotonicMilliseconds) + 4500
  for ($appIndex = 0; $appIndex -lt $appIds.Count; $appIndex += 1) {
    if ((Get-MonotonicMilliseconds) -ge $deadline) { break }
    $appId = [string]$appIds[$appIndex]
    try {
      Start-Process -FilePath 'explorer.exe' `
        -ArgumentList "shell:AppsFolder\$appId"
    } catch { continue }
    $attemptDeadline = if ($appIndex -eq ($appIds.Count - 1)) {
      $deadline
    } else {
      [Math]::Min(
        [int64]$deadline,
        [int64]((Get-MonotonicMilliseconds) + 3200)
      )
    }
    do {
      Start-Sleep -Milliseconds 100
      $windows = @(Get-ClaudeWindows)
      if ($windows.Count -gt 0) { return $windows }
    } while ((Get-MonotonicMilliseconds) -lt $attemptDeadline)
  }
  throw 'Claude Desktop did not expose a visible window after restore or launch'
}

function Test-ClaudeSelectedSession($root, [string]$expectedTitle) {
  $expected = Normalize-Label $expectedTitle
  if ($expected.Length -lt 2) { return $false }
  $documentName = Normalize-Label ([string]$root.Current.Name)
  if ($documentName -and $documentName.Contains($expected)) { return $true }

  $rootRect = $root.Current.BoundingRectangle
  if (-not (Test-FiniteWindowRectangle $rootRect)) { return $false }
  $contentLeft = $rootRect.Left + [Math]::Min(
    [double]420,
    [Math]::Max([double]300, [double]($rootRect.Width * 0.22))
  )
  $headerBottom = $rootRect.Top + [Math]::Min(
    [double]260,
    [Math]::Max([double]160, [double]($rootRect.Height * 0.28))
  )
  $all = Find-DescendantsByControlTypes $root @(
    [System.Windows.Automation.ControlType]::Document,
    [System.Windows.Automation.ControlType]::Text,
    [System.Windows.Automation.ControlType]::Header
  )
  for ($index = 0; $index -lt $all.Count; $index += 1) {
    $element = $all.Item($index)
    $typeId = $element.Current.ControlType.Id
    if ($typeId -eq [System.Windows.Automation.ControlType]::Document.Id -and
        (Test-LabelMatch ([string]$element.Current.Name) $expected)) { return $true }
    if ($element.Current.IsOffscreen -or
        ($typeId -ne [System.Windows.Automation.ControlType]::Text.Id -and
         $typeId -ne [System.Windows.Automation.ControlType]::Header.Id) -or
        -not (Test-LabelMatch ([string]$element.Current.Name) $expected)) { continue }
    $rect = $element.Current.BoundingRectangle
    if ($rect.Left -ge $contentLeft -and $rect.Top -le $headerBottom) { return $true }
  }
  return $false
}

$script:ClaudeFocusSnapshot = $null
$script:ClaudeFocusSnapshotAt = [int64]0

function Get-ClaudeDesktopSessionsRoot {
  $override = Normalize-Label ([Environment]::GetEnvironmentVariable('CLAUDE_DESKTOP_SESSIONS_DIR'))
  if ($override) { return $override }
  if ([string]::IsNullOrWhiteSpace($env:APPDATA)) { return '' }
  return Join-Path (Join-Path $env:APPDATA 'Claude') 'claude-code-sessions'
}

function Reset-ClaudeFocusSnapshot {
  $script:ClaudeFocusSnapshot = $null
  $script:ClaudeFocusSnapshotAt = [int64]0
}

function Get-ClaudeFocusSnapshot {
  $now = Get-MonotonicMilliseconds
  if ($null -ne $script:ClaudeFocusSnapshot -and
      ($now - $script:ClaudeFocusSnapshotAt) -lt 500) {
    return $script:ClaudeFocusSnapshot
  }

  $root = Get-ClaudeDesktopSessionsRoot
  $records = @()
  if ($root -and (Test-Path -LiteralPath $root)) {
    $files = @(Get-ChildItem -LiteralPath $root -Recurse -File -Filter 'local_*.json' `
      -ErrorAction SilentlyContinue)
    foreach ($file in $files) {
      try {
        $metadata = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        $desktopSessionId = Normalize-Label ([string]$metadata.sessionId)
        $cliSessionId = Normalize-Label ([string]$metadata.cliSessionId)
        $title = Normalize-Label ([string]$metadata.title)
        if (-not $desktopSessionId -or -not $cliSessionId -or $metadata.isArchived -eq $true) {
          continue
        }
        $lastFocusedAt = [double]0
        try { $lastFocusedAt = [double]$metadata.lastFocusedAt } catch {}
        $records += [pscustomobject]@{
          DesktopSessionId = $desktopSessionId
          CliSessionId = $cliSessionId
          Title = $title
          LastFocusedAt = $lastFocusedAt
        }
      } catch {}
    }
  }
  $focused = @($records | Sort-Object LastFocusedAt -Descending | Select-Object -First 1)
  $script:ClaudeFocusSnapshot = [pscustomobject]@{
    Records = @($records)
    FocusedDesktopSessionId = if ($focused.Count -gt 0) {
      [string]$focused[0].DesktopSessionId
    } else { '' }
  }
  $script:ClaudeFocusSnapshotAt = $now
  return $script:ClaudeFocusSnapshot
}

function Get-ClaudeDesktopSessionState(
  [string]$sessionId,
  [string]$desktopSessionId,
  [string]$sessionTitle
) {
  $expectedCli = Normalize-Label $sessionId
  $expectedDesktop = Normalize-Label $desktopSessionId
  $expectedTitle = Normalize-Label $sessionTitle
  if (-not $expectedCli -or -not $expectedDesktop) { return 'unavailable' }
  $snapshot = Get-ClaudeFocusSnapshot
  $record = @($snapshot.Records | Where-Object {
    ([string]$_.DesktopSessionId).Equals(
      $expectedDesktop,
      [StringComparison]::OrdinalIgnoreCase
    )
  } | Select-Object -First 1)
  if ($record.Count -eq 0) { return 'unavailable' }
  if (-not ([string]$record[0].CliSessionId).Equals(
      $expectedCli,
      [StringComparison]::OrdinalIgnoreCase
  )) { return 'mismatch' }
  if ($expectedTitle -and -not ([string]$record[0].Title).Equals(
      $expectedTitle,
      [StringComparison]::OrdinalIgnoreCase
  )) { return 'mismatch' }
  return $(if (([string]$snapshot.FocusedDesktopSessionId).Equals(
    $expectedDesktop,
    [StringComparison]::OrdinalIgnoreCase
  )) { 'matched' } else { 'mismatch' })
}

function Test-ClaudeWindowSession(
  $root,
  [string]$sessionId,
  [string]$desktopSessionId,
  [string]$sessionTitle
) {
  $metadataState = Get-ClaudeDesktopSessionState $sessionId $desktopSessionId $sessionTitle
  if ($metadataState -eq 'matched') { return $true }
  if ($metadataState -eq 'unavailable') { return Test-ClaudeSelectedSession $root $sessionTitle }
  return $false
}

function Activate-ClaudeWindow($window) {
  $activated = [CodexVoiceNative]::ActivateWindow([IntPtr]$window.WindowHandle)
  if (-not $activated) {
    try {
      $shell = New-Object -ComObject WScript.Shell
      [void]$shell.AppActivate([int]$window.ProcessId)
      Start-Sleep -Milliseconds 60
      $activated = [CodexVoiceNative]::ActivateWindow([IntPtr]$window.WindowHandle)
    } catch {}
  }
  return $activated
}

function Show-ClaudeSidebar($root) {
  $buttons = Find-DescendantsByControlTypes $root @(
    [System.Windows.Automation.ControlType]::Button
  )
  for ($index = 0; $index -lt $buttons.Count; $index += 1) {
    $button = $buttons.Item($index)
    if ($button.Current.IsOffscreen -or
        -not (Normalize-Label ([string]$button.Current.Name)).Equals(
          'Expand sidebar',
          [StringComparison]::OrdinalIgnoreCase
        )) { continue }
    $invoke = $null
    if ($button.TryGetCurrentPattern(
        [System.Windows.Automation.InvokePattern]::Pattern,
        [ref]$invoke
    )) {
      $invoke.Invoke()
      Start-Sleep -Milliseconds 180
    }
    return
  }
}

function Test-ClaudeSessionButtonName([string]$actualValue, [string]$expectedValue) {
  $actual = Normalize-Label $actualValue
  $expected = Normalize-Label $expectedValue
  if (-not $actual -or -not $expected) { return $false }
  if ($actual.Equals($expected, [StringComparison]::OrdinalIgnoreCase)) { return $true }
  $suffix = " $expected"
  if (-not $actual.EndsWith($suffix, [StringComparison]::OrdinalIgnoreCase)) { return $false }
  $prefix = $actual.Substring(0, $actual.Length - $suffix.Length)
  return -not ($prefix.Equals('More options for', [StringComparison]::OrdinalIgnoreCase) -or
    $prefix.Equals('New session in', [StringComparison]::OrdinalIgnoreCase))
}

function Find-ClaudeSessionRows([array]$windows, [string]$sessionTitle) {
  $expected = Normalize-Label $sessionTitle
  if (-not $expected) { return @() }
  $optionsName = "More options for $expected"
  $rows = @()
  foreach ($window in $windows) {
    if (-not (Activate-ClaudeWindow $window)) { continue }
    Show-ClaudeSidebar $window.Root
    $buttons = Find-DescendantsByControlTypes $window.Root @(
      [System.Windows.Automation.ControlType]::Button
    )
    $rootRect = $window.Root.Current.BoundingRectangle
    if (-not (Test-FiniteWindowRectangle $rootRect)) { continue }
    $sidebarRight = $rootRect.Left + [Math]::Min(
      [double]560,
      [Math]::Max([double]260, [double]($rootRect.Width * 0.24))
    )
    $options = @()
    for ($index = 0; $index -lt $buttons.Count; $index += 1) {
      $button = $buttons.Item($index)
      $name = Normalize-Label ([string]$button.Current.Name)
      $rect = $button.Current.BoundingRectangle
      if ((Test-FiniteWindowRectangle $rect) -and
          -not $button.Current.IsOffscreen -and
          $name.Equals($optionsName, [StringComparison]::OrdinalIgnoreCase) -and
          $rect.Width -ge 12 -and $rect.Height -ge 12 -and
          $rect.Left -lt $sidebarRight) {
        $options += [pscustomobject]@{ Item = $button; Rect = $rect }
      }
    }
    for ($index = 0; $index -lt $buttons.Count; $index += 1) {
      $button = $buttons.Item($index)
      $name = Normalize-Label ([string]$button.Current.Name)
      $rect = $button.Current.BoundingRectangle
      if (-not (Test-FiniteWindowRectangle $rect) -or
          $button.Current.IsOffscreen -or
          -not (Test-ClaudeSessionButtonName $name $expected) -or
          $rect.Width -lt 80 -or $rect.Height -lt 20 -or
          $rect.Left -ge $sidebarRight) { continue }
      $rowCenter = $rect.Top + ($rect.Height / 2)
      $pairedOptions = @($options | Where-Object {
        $optionCenter = $_.Rect.Top + ($_.Rect.Height / 2)
        [Math]::Abs($optionCenter - $rowCenter) -le [Math]::Max(
          [double]6,
          [double]($rect.Height * 0.45)
        ) -and
          $_.Rect.Left -ge $rect.Left
      })
      if ($pairedOptions.Count -eq 1) {
        $rows += [pscustomobject]@{
          Window = $window
          Root = $window.Root
          Item = $button
        }
      }
    }
  }
  return @($rows)
}

function Test-ClaudeSessionPoint([int]$x, [int]$y, [string]$sessionTitle) {
  $element = [System.Windows.Automation.AutomationElement]::FromPoint(
    [System.Windows.Point]::new($x, $y)
  )
  $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
  while ($null -ne $element) {
    if ($element.Current.ControlType.Id -eq [System.Windows.Automation.ControlType]::Button.Id -and
        (Test-ClaudeSessionButtonName ([string]$element.Current.Name) $sessionTitle)) {
      return $true
    }
    $element = $walker.GetParent($element)
  }
  return $false
}

function Invoke-ClaudeSessionRow($match, [string]$sessionTitle) {
  if (-not (Activate-ClaudeWindow $match.Window)) {
    throw 'Claude Desktop could not become foreground for session navigation'
  }
  $scroll = $null
  if ($match.Item.TryGetCurrentPattern(
      [System.Windows.Automation.ScrollItemPattern]::Pattern,
      [ref]$scroll
  )) {
    $scroll.ScrollIntoView()
    Start-Sleep -Milliseconds 160
  }

  $freshRows = @(Find-ClaudeSessionRows @($match.Window) $sessionTitle)
  if ($freshRows.Count -ne 1) {
    throw 'Claude sidebar session changed while preparing navigation'
  }
  $item = $freshRows[0].Item
  $invoke = $null
  if ($item.TryGetCurrentPattern(
      [System.Windows.Automation.InvokePattern]::Pattern,
      [ref]$invoke
  )) {
    $invoke.Invoke()
    return
  }

  $rect = $item.Current.BoundingRectangle
  if (-not (Test-FiniteWindowRectangle $rect)) {
    throw 'Claude session row did not expose a finite clickable area'
  }
  $clickX = [int]($rect.Left + [Math]::Min([double]48, [double]($rect.Width * 0.2)))
  $clickY = [int]($rect.Top + ($rect.Height / 2))
  if (-not (Test-ClaudeSessionPoint $clickX $clickY $sessionTitle)) {
    throw 'Claude session click point no longer belongs to the requested session'
  }
  [CodexVoiceNative]::Click($clickX, $clickY)
}

function Open-ClaudeSession(
  [string]$sessionId,
  [string]$desktopSessionId,
  [string]$sessionTitle
) {
  if ([string]::IsNullOrWhiteSpace($sessionId) -or
      [string]::IsNullOrWhiteSpace($desktopSessionId) -or
      [string]::IsNullOrWhiteSpace($sessionTitle)) {
    throw 'Claude Desktop session navigation requires an exact session ID'
  }
  $existingWindows = @(Get-RestoredClaudeWindows)
  if ($existingWindows.Count -eq 0) {
    throw 'No running Claude Desktop window was found'
  }
  foreach ($window in $existingWindows) {
    if (Test-ClaudeWindowSession $window.Root $sessionId $desktopSessionId $sessionTitle) {
      if (-not (Activate-ClaudeWindow $window)) {
        throw 'Claude Desktop could not activate the already selected session'
      }
      return $window
    }
  }

  $sessionRows = @(Find-ClaudeSessionRows $existingWindows $sessionTitle)
  if ($sessionRows.Count -eq 0) {
    throw 'The bound Claude session was not found in the visible sidebar'
  }
  if ($sessionRows.Count -gt 1) {
    throw 'Multiple Claude sidebar sessions matched the bound title; refusing an ambiguous switch'
  }
  Reset-ClaudeFocusSnapshot
  Invoke-ClaudeSessionRow $sessionRows[0] $sessionTitle
  $deadline = (Get-MonotonicMilliseconds) + 4000
  do {
    Start-Sleep -Milliseconds 80
    Reset-ClaudeFocusSnapshot
    $windows = @(Get-ClaudeWindows)
    foreach ($window in $windows) {
      if (Test-ClaudeWindowSession $window.Root $sessionId $desktopSessionId $sessionTitle) {
        if (-not (Activate-ClaudeWindow $window)) {
          throw 'Claude Desktop could not become foreground after session navigation'
        }
        return $window
      }
    }
  } while ((Get-MonotonicMilliseconds) -lt $deadline)
  throw 'Claude Desktop did not confirm the requested existing session after sidebar navigation'
}

function Test-WorkspaceAncestor($element, [string]$workspaceLabel) {
  $expected = Normalize-Label $workspaceLabel
  if (-not $expected) { return $false }
  $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
  $parent = $walker.GetParent($element)
  while ($null -ne $parent) {
    $typeId = $parent.Current.ControlType.Id
    if (($typeId -eq [System.Windows.Automation.ControlType]::ListItem.Id -or
         $typeId -eq [System.Windows.Automation.ControlType]::Group.Id) -and
        (Normalize-Label ([string]$parent.Current.Name)).Equals(
          $expected,
          [StringComparison]::OrdinalIgnoreCase
        )) { return $true }
    $parent = $walker.GetParent($parent)
  }
  return $false
}

function Find-CodexSessionRows([array]$windows, [string]$sessionTitle) {
  $expected = Normalize-Label $sessionTitle
  $sessionRows = @()
  foreach ($window in $windows) {
    try {
      $window.Root = Ensure-CodexForeground $window.Root 'for task lookup'
      Show-CodexSidebar $window.Root
      $all = Find-DescendantsByControlTypes $window.Root @(
        [System.Windows.Automation.ControlType]::ListItem
      )
      $rootRect = $window.Root.Current.BoundingRectangle
      if (-not (Test-FiniteWindowRectangle $rootRect)) { continue }
      $sidebarRight = $rootRect.Left + [Math]::Min(
        [double]480,
        [Math]::Max([double]240, [double]($rootRect.Width * 0.38))
      )
      for ($index = 0; $index -lt $all.Count; $index += 1) {
        $element = $all.Item($index)
        if ($element.Current.ControlType.Id -ne [System.Windows.Automation.ControlType]::ListItem.Id) { continue }
        $name = Normalize-Label ([string]$element.Current.Name)
        $rect = $element.Current.BoundingRectangle
        if (-not (Test-FiniteWindowRectangle $rect) -or
            -not $name.Equals($expected, [StringComparison]::OrdinalIgnoreCase) -or
            $element.Current.IsOffscreen -or
            $rect.Width -lt 80 -or
            $rect.Height -lt 20 -or
            $rect.Left -ge $sidebarRight) { continue }
        $sessionRows += [pscustomobject]@{ Root = $window.Root; Item = $element }
      }
    } catch { continue }
  }
  return @($sessionRows)
}

function Test-CodexSessionPoint([int]$x, [int]$y, [string]$sessionTitle) {
  $expected = Normalize-Label $sessionTitle
  $element = [System.Windows.Automation.AutomationElement]::FromPoint(
    [System.Windows.Point]::new($x, $y)
  )
  $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
  while ($null -ne $element) {
    if ($element.Current.ControlType.Id -eq [System.Windows.Automation.ControlType]::ListItem.Id -and
        (Normalize-Label ([string]$element.Current.Name)).Equals(
          $expected,
          [StringComparison]::OrdinalIgnoreCase
        )) { return $true }
    $element = $walker.GetParent($element)
  }
  return $false
}

function Invoke-CodexSessionRow(
  $match,
  [string]$sessionTitle,
  [string]$workspaceLabel
) {
  $match.Root = Ensure-CodexForeground $match.Root 'for session navigation'
  $scroll = $null
  if ($match.Item.TryGetCurrentPattern(
      [System.Windows.Automation.ScrollItemPattern]::Pattern,
      [ref]$scroll
  )) {
    $scroll.ScrollIntoView()
    Start-Sleep -Milliseconds 180
  }

  # Scrolling virtualized task lists can recycle the original AutomationElement
  # or move the row again while Chromium refreshes its accessibility tree.
  $clickReady = $false
  $clickError = 'ChatGPT（Codex） session row changed while preparing navigation'
  $clickDeadline = (Get-MonotonicMilliseconds) + 1200
  do {
    $freshRows = @(Find-CodexSessionRows @(
      [pscustomobject]@{ Root = $match.Root; Area = 1 }
    ) $sessionTitle)
    if ($freshRows.Count -gt 1 -and (Normalize-Label $workspaceLabel)) {
      $freshRows = @($freshRows | Where-Object {
        Test-WorkspaceAncestor $_.Item $workspaceLabel
      })
    }
    if ($freshRows.Count -eq 1) {
      $rect = $freshRows[0].Item.Current.BoundingRectangle
      if ((Test-FiniteWindowRectangle $rect) -and
          $rect.Width -ge 80 -and $rect.Height -ge 20) {
        $clickX = [int]($rect.Left + [Math]::Min([double]48, [double]($rect.Width * 0.2)))
        $clickY = [int]($rect.Top + ($rect.Height / 2))
        if (Test-CodexSessionPoint $clickX $clickY $sessionTitle) {
          $clickReady = $true
          break
        }
        $clickError = 'ChatGPT（Codex） session click point no longer belongs to the requested task'
      } else {
        $clickError = 'ChatGPT（Codex） session row did not expose a clickable area'
      }
    }
    if ((Get-MonotonicMilliseconds) -ge $clickDeadline) { break }
    Start-Sleep -Milliseconds 80
  } while ($true)
  if (-not $clickReady) { throw $clickError }
  [CodexVoiceNative]::Click($clickX, $clickY)
}

function Open-CodexSession([string]$sessionTitle, [string]$workspaceLabel) {
  $windows = @(Get-OrLaunchCodexWindows)
  foreach ($window in $windows) {
    try {
      $window.Root = Ensure-CodexForeground $window.Root 'for session selection'
      if (Test-SelectedSessionTitle $window.Root $sessionTitle) { return $window.Root }
    } catch { continue }
  }

  $sessionRows = @(Find-CodexSessionRows $windows $sessionTitle)
  if ($sessionRows.Count -gt 1 -and (Normalize-Label $workspaceLabel)) {
    $sessionRows = @($sessionRows | Where-Object { Test-WorkspaceAncestor $_.Item $workspaceLabel })
  }
  if ($sessionRows.Count -eq 0) {
    throw 'The bound ChatGPT（Codex） session was not found in the visible sidebar'
  }
  if ($sessionRows.Count -gt 1) {
    throw 'Multiple ChatGPT（Codex） sidebar sessions matched the bound title'
  }

  Invoke-CodexSessionRow $sessionRows[0] $sessionTitle $workspaceLabel
  $deadline = (Get-MonotonicMilliseconds) + 2500
  do {
    if (Test-SelectedSessionTitle $sessionRows[0].Root $sessionTitle) {
      $sessionRows[0].Root = Ensure-CodexForeground `
        $sessionRows[0].Root 'after session navigation'
      return $sessionRows[0].Root
    }
    Start-Sleep -Milliseconds 50
  } while ((Get-MonotonicMilliseconds) -lt $deadline)
  throw 'ChatGPT（Codex） did not confirm the requested session after navigation'
}

function Open-CodexSessionById(
  [string]$sessionId,
  [string]$sessionTitle,
  [string]$deepLink,
  [string]$workspaceLabel
) {
  if (-not (Normalize-Label $sessionId) -or -not (Normalize-Label $deepLink)) {
    return Open-CodexSession $sessionTitle $workspaceLabel
  }

  $windows = @()
  try { $windows = @(Get-CodexWindows) } catch {}
  foreach ($window in $windows) {
    try {
      $window.Root = Ensure-CodexForeground `
        $window.Root 'for already selected session voice input'
    } catch { continue }
    if (-not (Test-SelectedSessionTitle $window.Root $sessionTitle)) { continue }
    return $window.Root
  }

  try {
    Start-Process $deepLink
    $deadline = (Get-MonotonicMilliseconds) + 5000
    do {
      Start-Sleep -Milliseconds 100
      try { $windows = @(Get-CodexWindows) } catch { $windows = @() }
      foreach ($window in $windows) {
        try {
          $window.Root = Ensure-CodexForeground `
            $window.Root 'after session deep-link navigation'
        } catch { continue }
        if (-not (Test-SelectedSessionTitle $window.Root $sessionTitle)) { continue }
        return $window.Root
      }
    } while ((Get-MonotonicMilliseconds) -lt $deadline)
  } catch {}

  # Older Codex builds may register the protocol without accepting thread
  # routes. Keep the title/sidebar path as a compatibility fallback.
  return Open-CodexSession $sessionTitle $workspaceLabel
}

function Normalize-ComposerText([string]$text) {
  return $text.Replace("`r`n", "`n").Replace("`r", "`n")
}

function Join-VoiceDraft([string]$base, [string]$transcript) {
  $text = (Normalize-ComposerText $transcript).Trim()
  if ([string]::IsNullOrEmpty($text)) { return $base }
  if ([string]::IsNullOrEmpty($base) -or $base -match '\s$') { return ($base + $text) }
  return ($base + ' ' + $text)
}

function Get-ComposerProperty($element, [int]$id) {
  # Native COM accepts a real numeric property ID. Managed LookupById can be
  # null for identifiers not registered by that managed assembly/provider.
  if ($element -is [WorkBuddyUiaElement]) { return $element.GetCurrentPropertyValue($id) }
  $property = [System.Windows.Automation.AutomationProperty]::LookupById($id)
  if ($null -eq $property) { return $null }
  return $element.GetCurrentPropertyValue($property)
}

# UIA3 LegacyIAccessible has no managed type in Windows PowerShell 5.1.
# Native Chromium elements accept the SDK ID directly; managed clients may
# only use identifiers actually registered by their loaded UIA assembly.
function Get-ComposerPattern($element, [int]$id) {
  $identifier = if ($element -is [WorkBuddyUiaElement]) { $id }
    else { [System.Windows.Automation.AutomationPattern]::LookupById($id) }
  if ($null -eq $identifier) { return $null }
  $pattern = $null
  if ($element.TryGetCurrentPattern($identifier, [ref]$pattern)) { return $pattern }
  return $null
}

function Test-ComposerEditor($element) {
  # Names/help/labels describe controls; none is evidence of an editor value.
  # In particular, a readable Document/Group ancestor of a textbox is not the
  # textbox. Require native Edit semantics or an explicit rich-textbox surface.
  $current = $element.Current
  # Password surfaces must never be admitted as a voice composer.
  $password = (Get-ComposerProperty $element 30019)
  if ($password -is [bool] -and $password) { return $false }
  if (-not $current.IsEnabled -or $current.IsOffscreen -or
      -not $current.IsKeyboardFocusable) { return $false }
  $type = $current.ControlType.Id
  if ($element -is [WorkBuddyUiaElement] -and $element.HasWritableLegacyText()) { return $true }
  if ($type -eq [System.Windows.Automation.ControlType]::Edit.Id) { return $true }
  if ($type -ne [System.Windows.Automation.ControlType]::Document.Id -and
      $type -ne [System.Windows.Automation.ControlType]::Group.Id -and
      $type -ne [System.Windows.Automation.ControlType]::Custom.Id) { return $false }
  $role = ''
  try { $role = [string](Get-ComposerProperty $element 30101) } catch {}
  return $role -ceq 'textbox' -or
    ([string]$current.ClassName) -match '(?i)(^|\s)(ProseMirror|tiptap)(\s|$)'
}

function Assert-ComposerEditableRange($range) {
  $readOnly = $range.GetAttributeValue([System.Windows.Automation.TextPattern]::IsReadOnlyAttribute)
  $hidden = $range.GetAttributeValue([System.Windows.Automation.TextPattern]::IsHiddenAttribute)
  # MixedAttributeValue/NotSupported/null are not false. Do not reconstruct a
  # draft by deleting arbitrary read-only spans (which may be user attachments,
  # mentions or code). Without an authoritative value such a draft is unknown.
  if ($readOnly -isnot [bool] -or $readOnly -or $hidden -isnot [bool] -or $hidden) {
    throw 'Visible composer text includes read-only, hidden or unknown content; no draft was changed'
  }
}

function Get-ComposerValuePropertySnapshot($element) {
  # Some UIA bridges expose the typed Value properties even when the managed
  # pattern object cannot be marshalled. Never fall back to displayed text in
  # that case. This is a read snapshot, not an alternate injection mechanism.
  $availableProperty = 30043
  $valueProperty = 30045
  $readOnlyProperty = 30046
  $available = (Get-ComposerProperty $element $availableProperty)
  if ($available -isnot [bool] -or -not $available) { return $null }
  $readOnly = (Get-ComposerProperty $element $readOnlyProperty)
  $value = (Get-ComposerProperty $element $valueProperty)
  if ($readOnly -isnot [bool] -or $readOnly -or $value -isnot [string] -or $value.Length -gt 65536) {
    throw 'Visible composer Value properties are not writable or are unknown'
  }
  $againReadOnly = (Get-ComposerProperty $element $readOnlyProperty)
  $againAvailable = (Get-ComposerProperty $element $availableProperty)
  if ($againReadOnly -isnot [bool] -or $againReadOnly -or
      $againAvailable -isnot [bool] -or -not $againAvailable -or
      (Get-ComposerProperty $element $valueProperty) -cne $value) {
    throw 'Visible composer Value properties changed while its draft was read'
  }
  return [pscustomobject]@{
    PSTypeName = 'Hachimi.ComposerValueSnapshot'
    Current = [pscustomobject]@{ IsReadOnly = $false; Value = $value }
  }
}

function Assert-ComposerFallbackText($element, [string]$text) {
  if ([string]::IsNullOrEmpty($text)) { return }
  # UIA HelpText is Chromium's placeholder channel. Name can also be derived
  # from a placeholder/title. Neither is an editable value. A fallback that
  # equals either is ambiguous: refuse it, do not delete a guessed hint or
  # assume an empty draft. Real ValuePattern/Value properties bypass this test.
  # Ignore only provider-added terminal newlines for this *comparison*; never
  # trim or remove a prefix/suffix from a real user's value.
  $comparison = (Normalize-ComposerText $text).TrimEnd([char[]]"`n")
  if ([string]::IsNullOrEmpty($comparison)) { return }
  foreach ($id in @(30005, 30013)) {
    $hint = $element.GetCurrentPropertyValue([System.Windows.Automation.AutomationProperty]::LookupById($id))
    if ($hint -is [string] -and -not [string]::IsNullOrEmpty($hint) -and
        (Normalize-ComposerText $hint).TrimEnd([char[]]"`n") -ceq $comparison) {
      throw 'COMPOSER_DRAFT_SEMANTICS_UNKNOWN: displayed prompt metadata is not an editable draft; no input was sent'
    }
  }
}

function Read-ComposerTextPattern($element, $pattern) {
  $range = $pattern.DocumentRange
  $enclosing = $range.GetEnclosingElement()
  $editorId = @($element.GetRuntimeId()) -join '.'
  $rangeId = @($enclosing.GetRuntimeId()) -join '.'
  if (-not $editorId -or $rangeId -cne $editorId) {
    throw 'Visible composer text range belongs to another container'
  }
  Assert-ComposerEditableRange $range
  $children = @($range.GetChildren())
  if ($children.Count -gt 64) { throw 'Visible composer text child budget exceeded' }
  foreach ($child in $children) {
    # GetText includes embedded-object text. Check its actual child range,
    # never the Name/HelpText, foreground color, or a list of prompt strings.
    if ($child.Current.ControlType.Id -eq [System.Windows.Automation.ControlType]::Button.Id) {
      throw 'Visible composer text includes an accessory control; no draft was changed'
    }
    Assert-ComposerEditableRange ($pattern.RangeFromChild($child))
  }
  $text = $range.GetText(65537)
  if ($text -isnot [string] -or $text.Length -gt 65536) {
    throw 'Visible composer text is unavailable or exceeds the read budget'
  }
  Assert-ComposerEditableRange $range
  if ($range.GetText(65537) -cne $text) { throw 'Visible composer changed while its draft was read' }
  Assert-ComposerFallbackText $element $text
  return Normalize-ComposerText $text
}

function Get-ComposerText($element) {
  if (-not (Test-ComposerEditor $element)) { throw 'Visible target is not an editable composer' }
  $pattern = $null
  if ($element.TryGetCurrentPattern(
      [System.Windows.Automation.ValuePattern]::Pattern,
      [ref]$pattern
  )) {
    $current = $pattern.Current
    if ($current.IsReadOnly -isnot [bool] -or $current.IsReadOnly -or $current.Value -isnot [string]) {
      throw 'Visible composer value is not writable or is unknown'
    }
    if ($current.Value.Length -gt 65536) { throw 'Visible composer value exceeds the read budget' }
    $again = $pattern.Current
    if ($again.IsReadOnly -isnot [bool] -or $again.IsReadOnly -or
        $again.Value -isnot [string] -or $again.Value -cne $current.Value) {
      throw 'Visible composer value changed while its draft was read'
    }
    # A real value wins even when empty, equal to a prompt, or beginning with
    # U+FEFF. Never replace it with a display label or trim real user content.
    return @($pattern, (Normalize-ComposerText $current.Value))
  }
  $valueSnapshot = Get-ComposerValuePropertySnapshot $element
  if ($null -ne $valueSnapshot) {
    return @($valueSnapshot, (Normalize-ComposerText $valueSnapshot.Current.Value))
  }
  $pattern = Get-ComposerPattern $element 10018 # UIA_LegacyIAccessiblePatternId
  if ($null -ne $pattern) {
    $current = $pattern.Current
    # MSAA ROLE_SYSTEM_TEXT + no READONLY/UNAVAILABLE state. Legacy Name/Help
    # and Value on generic document/accessory roles are not draft authorities.
    if ($current.Role -eq 42) {
      if (($current.State -isnot [int] -and $current.State -isnot [uint32]) -or
          ($current.State -band 0x41) -ne 0 -or $current.Value -isnot [string]) {
        throw 'Visible composer legacy editor value is not writable or is unknown'
      }
      if ($current.Value.Length -gt 65536) { throw 'Visible composer legacy value exceeds the read budget' }
      Assert-ComposerFallbackText $element $current.Value
      # A legacy value may include CSS-generated/read-only hint children. When
      # TextPattern exists, require its actual editable range to agree instead
      # of trusting the legacy role/state alone. Never reconstruct mixed text.
      $textPattern = $null
      if ($element.TryGetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern, [ref]$textPattern)) {
        $editable = Read-ComposerTextPattern $element $textPattern
        if ($editable -cne (Normalize-ComposerText $current.Value)) {
          throw 'Visible composer legacy and editable text disagree; no draft was changed'
        }
      }
      return @($pattern, (Normalize-ComposerText $current.Value))
    }
  }
  if ($element.TryGetCurrentPattern(
      [System.Windows.Automation.TextPattern]::Pattern,
      [ref]$pattern
  )) {
    return @($pattern, (Read-ComposerTextPattern $element $pattern))
  }
  $legacySnapshot = Get-NativeLegacyComposerSnapshot $element $true
  if ($null -ne $legacySnapshot) { return @($legacySnapshot, (Normalize-ComposerText $legacySnapshot.Current.Value)) }
  throw 'Visible composer exposes no unambiguous editable value'
}

function Get-NativeLegacyComposerSnapshot($element, [bool]$readValue = $false) {
  if ($element -isnot [WorkBuddyUiaElement] -or -not $element.HasWritableLegacyText()) { return $null }
  $role = $element.GetCurrentPropertyValue(30095)
  $state = $element.GetCurrentPropertyValue(30096)
  $current = @{ Role=$role; State=$state }
  if ($readValue) {
    $value = $element.GetCurrentPropertyValue(30093)
    if ($value -isnot [string] -or $value.Length -gt 65536 -or
        $element.GetCurrentPropertyValue(30093) -cne $value -or -not $element.HasWritableLegacyText()) { return $null }
    $current.Value = $value
  }
  return [pscustomobject]@{ PSTypeName='Hachimi.ComposerLegacySnapshot'; Current=[pscustomobject]$current }
}

function Get-NativeComposerInputPattern($element) {
  if (-not (Test-ComposerEditor $element)) { throw 'Visible target is not an editable composer' }
  foreach ($kind in @(10002, 10018, 10014)) { # Value, LegacyIAccessible, Text
    $pattern = Get-ComposerPattern $element $kind
    if ($null -ne $pattern -and (Test-WorkBuddyWritablePattern $pattern)) { return $pattern }
  }
  # A provider can expose typed Value capability properties without a
  # marshalable pattern. Read only capability/read-only flags, never Value.
  $availableProperty = 30043
  $readOnlyProperty = 30046
  $available = (Get-ComposerProperty $element $availableProperty)
  $readOnly = (Get-ComposerProperty $element $readOnlyProperty)
  $againAvailable = (Get-ComposerProperty $element $availableProperty)
  $againReadOnly = (Get-ComposerProperty $element $readOnlyProperty)
  if ($available -is [bool] -and $available -and $readOnly -is [bool] -and -not $readOnly -and
      $againAvailable -is [bool] -and $againAvailable -and $againReadOnly -is [bool] -and -not $againReadOnly) {
    return [pscustomobject]@{
      PSTypeName = 'Hachimi.ComposerInputCapability'
      Current = [pscustomobject]@{ IsReadOnly = $false }
    }
  }
  $legacyCapability = Get-NativeLegacyComposerSnapshot $element
  if ($null -ne $legacyCapability) { return $legacyCapability }
  throw 'Visible composer has no confirmed writable native input surface'
}

function Get-NativeVoiceConversationAnchor($root, [string]$agent) {
  # Optional witness only: a real selected navigation item paired with the
  # current content title. Incidental header/status texts are never identity.
  $titles = @((Get-TaskWindowIdentity $root $agent).Split([char]31) | ForEach-Object { Normalize-Label $_ })
  $elements = if ($root -is [WorkBuddyUiaElement]) {
    @([WorkBuddyUia]::FindBounded([IntPtr]$root.Current.NativeWindowHandle, $true, 400, 3500))
  } else { @(Find-BoundedComposerElements $root $true 400) }
  $selected = @{}; $rect = $root.Current.BoundingRectangle
  foreach ($element in $elements) {
    try {
      $type = $element.Current.ControlType.Id
      if ($type -ne [System.Windows.Automation.ControlType]::ListItem.Id -and
          $type -ne [System.Windows.Automation.ControlType]::TreeItem.Id) { continue }
      if ($element.Current.IsOffscreen -or -not $element.Current.IsEnabled -or
          -not (Test-ComposerElementOwner $root $element)) { continue }
      $r = $element.Current.BoundingRectangle
      if (-not (Test-FiniteWindowRectangle $r) -or $r.Left -lt $rect.Left -or
          $r.Right -gt ($rect.Left + [Math]::Min(420, $rect.Width * 0.45))) { continue }
      $available = (Get-ComposerProperty $element 30036)
      $isSelected = (Get-ComposerProperty $element 30079)
      if ($available -isnot [bool] -or -not $available -or $isSelected -isnot [bool] -or -not $isSelected) { continue }
      $name = Normalize-Label ([string]$element.Current.Name)
      if ($name.Length -lt 2 -or @('chatgpt','codex','workbuddy','deepseek harness','claude') -contains $name.ToLowerInvariant()) { continue }
      if (-not @($titles | Where-Object { $_ -ceq $name }).Count) { continue }
      $id = Get-ElementRuntimeId $element
      if ($id) { $selected[$id] = @{ Element=$element; RuntimeId=$id; Name=$name } }
    } catch {}
  }
  if ($selected.Count -ne 1) { return $null }
  return @($selected.Values)[0]
}

function Assert-NativeVoiceConversation($state, $root) {
  if (-not $state.CurrentVisible) { return } # Get-BoundComposerRoot already checks the bound session.
  $anchor = $state.NativeVoiceAnchor
  if ($null -eq $anchor) { throw 'Visible conversation identity is unavailable; no submit or editor replacement was attempted; draft preserved' }
  $element = $anchor.Element
  $selected = (Get-ComposerProperty $element 30079)
  if ($selected -isnot [bool] -or -not $selected -or $element.Current.IsOffscreen -or
      -not $element.Current.IsEnabled -or -not (Test-ComposerElementOwner $root $element) -or
      (Get-ElementRuntimeId $element) -cne $anchor.RuntimeId -or
      (Normalize-Label ([string]$element.Current.Name)) -cne $anchor.Name) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
}

function Test-NativeVoiceEditorIdentity($state) {
  try {
    $element = $state.Composer
    $id = Get-ElementRuntimeId $element
    return -not [string]::IsNullOrEmpty($id) -and $id -ceq $state.ComposerRuntimeId -and
      (Test-ComposerElementOwner $state.Root $element) -and (Test-ComposerEditor $element)
  } catch { return $false }
}

function Assert-NativeVoiceComposer($state) {
  $root = Get-BoundComposerRoot $state
  if (-not (Test-NativeVoiceEditorIdentity $state)) { throw 'Native voice editor needs reacquisition' }
  if ($null -ne $state.NativeVoiceAnchor) { Assert-NativeVoiceConversation $state $root }
  $state.Pattern = Get-NativeComposerInputPattern $state.Composer
}

function Get-NativeVoiceValueHash([string]$value) {
  $hasher = [System.Security.Cryptography.SHA256]::Create()
  try { return [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($value))).Replace('-', '').ToLowerInvariant() }
  finally { $hasher.Dispose() }
}

function Read-NativeVoiceOwnedDraft($state, [int]$budgetMs = 900) {
  $deadline = (Get-MonotonicMilliseconds) + $budgetMs
  $previous = $null; $previousEditor = ''
  do {
    try {
      [void](Get-BoundComposerRoot $state)
      if (-not (Test-NativeVoiceEditorIdentity $state)) { Reacquire-BoundComposer $state $null 150 }
      Assert-NativeVoiceComposer $state
      if (-not $state.Composer.Current.HasKeyboardFocus) { return $false }
      $value = [string](Get-ComposerText $state.Composer)[1]
      # Corroborate this segment in the actual provider readback only. A
      # provider-added terminal paragraph is allowed for comparison, not removed
      # from the stored full snapshot. This value is NEVER pasted or rewritten.
      $matches = $value.EndsWith($state.NativeVoiceText, [StringComparison]::Ordinal) -or
        $value.EndsWith(($state.NativeVoiceText + "`n"), [StringComparison]::Ordinal)
      $editor = Get-ElementRuntimeId $state.Composer
      if ($matches -and $null -ne $previous -and $value -ceq $previous -and $editor -ceq $previousEditor) {
        [void](Get-BoundComposerRoot $state)
        $state.NativeVoiceOwnedValue = $value
        $state.NativeVoiceOwnedHash = Get-NativeVoiceValueHash $value
        $state.NativeVoiceOwnedRuntimeId = $editor
        return $true
      }
      $previous = if ($matches) { $value } else { $null }; $previousEditor = $editor
    } catch { $previous = $null }
    Start-Sleep -Milliseconds 40
  } while ((Get-MonotonicMilliseconds) -lt $deadline)
  return $false
}

function Assert-NativeVoiceOwnedDraft($state, [bool]$allowReacquire = $false) {
  $root = Get-BoundComposerRoot $state
  if (-not (Test-NativeVoiceEditorIdentity $state)) {
    if (-not $allowReacquire) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
    Reacquire-BoundComposer $state $null 150 # positive conversation witness required
  }
  Assert-NativeVoiceComposer $state
  if ($state.ComposerRuntimeId -cne $state.NativeVoiceOwnedRuntimeId) {
    if (-not $allowReacquire) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
    Assert-NativeVoiceConversation $state $root
  }
  if (-not $state.Composer.Current.HasKeyboardFocus) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
  $focused = if ($state.NativeUia) { [WorkBuddyUia]::FocusedElement() }
    else { [System.Windows.Automation.AutomationElement]::FocusedElement }
  if ($null -eq $focused -or (Get-ElementRuntimeId $focused) -cne $state.ComposerRuntimeId -or
      -not (Test-ComposerElementOwner $state.Root $focused)) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
  $actual = [string](Get-ComposerText $state.Composer)[1]
  if ([string]::IsNullOrEmpty($state.NativeVoiceOwnedHash) -or
      $actual -cne $state.NativeVoiceOwnedValue -or
      (Get-NativeVoiceValueHash $actual) -cne $state.NativeVoiceOwnedHash) {
    throw 'Owned voice draft changed or is unreadable; no submit was attempted; current draft preserved'
  }
  [void](Get-BoundComposerRoot $state)
  $state.NativeVoiceOwnedRuntimeId = $state.ComposerRuntimeId
}

function Append-ComposerVoiceText($state, [string]$text) {
  $text = Normalize-ComposerText $text
  if ([string]::IsNullOrEmpty($text)) { throw 'No final voice text to append' }
  if ($state.NativeVoiceWritten) {
    if ($text -ceq $state.NativeVoiceText) { return } # exact duplicate: zero wire/input
    throw 'COMPOSER_WRITE_ACK_UNKNOWN: A different final segment followed an appended segment; no rewrite or replay'
  }
  if ($state.NativeVoiceAttempted) { throw 'COMPOSER_WRITE_ACK_UNKNOWN: This segment was already attempted; no replay' }
  # Retain the exact admitted editor while it remains valid. No dynamic header
  # comparison or unconditional whole-tree rebind can reject a first utterance.
  if (-not (Test-NativeVoiceEditorIdentity $state)) { Reacquire-BoundComposer $state $null 400 }
  Assert-NativeVoiceComposer $state
  if ($state.CurrentVisible -and $null -eq $state.NativeVoiceAnchor -and -not $state.Composer.Current.HasKeyboardFocus) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
  Focus-Composer $state
  Assert-NativeVoiceComposer $state
  [void](Get-BoundComposerRoot $state)
  $state.NativeVoiceText = $text
  $state.NativeVoiceAttempted = $true
  $state.WriteAttempted = $true
  try {
    [CodexVoiceNative]::AppendFocusedText([IntPtr]$state.WindowHandle, [int]$state.ProcessId, $text)
    if (-not (Read-NativeVoiceOwnedDraft $state)) {
      throw 'COMPOSER_WRITE_ACK_UNKNOWN: Native append was accepted but its owned readback could not be proven; no replay'
    }
    $state.NativeVoiceWritten = $true
    $state.WriteAttempted = $false
  } catch {
    if ([CodexVoiceNative]::LastInputRejectedBeforeSend) { $state.WriteAttempted = $false; throw }
    throw 'COMPOSER_WRITE_ACK_UNKNOWN: Native append may be partial; no replay. Existing draft was not replaced.'
  }
}

function Test-AllowedComposerValue([string]$value, [string[]]$allowedValues) {
  # A null allow-list is used only for the initial semantic bind. Product
  # bind snapshots the existing draft as an immutable prefix; later rebinds
  # still require the exact last voice-controlled value (including formatting).
  if ($null -eq $allowedValues) { return $true }
  $normalizedValue = Normalize-ComposerText $value
  foreach ($allowedValue in @($allowedValues)) {
    if ($normalizedValue -ceq (Normalize-ComposerText ([string]$allowedValue))) { return $true }
  }
  return $false
}

function Add-UniqueComposerElement($element, $elements, $seen) {
  if ($null -eq $element) { return }
  try {
    $runtimeId = @($element.GetRuntimeId()) -join '.'
    $rect = $element.Current.BoundingRectangle
    $key = if ($runtimeId) {
      "runtime:$runtimeId"
    } elseif (Test-FiniteWindowRectangle $rect) {
      "bounds:$([int]$rect.Left),$([int]$rect.Top),$([int]$rect.Width),$([int]$rect.Height):$([int]$element.Current.ControlType.Id)"
    } else {
      ''
    }
    if (-not $key -or $seen.ContainsKey($key)) { return }
    $seen[$key] = $true
    [void]$elements.Add($element)
  } catch {}
}

function Find-PointComposerElements($root, [int]$budgetMs = 1800) {
  $deadline = (Get-MonotonicMilliseconds) + $budgetMs
  $rootRect = $root.Current.BoundingRectangle
  if (-not (Test-FiniteWindowRectangle $rootRect)) { return @() }
  $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
  $elements = [System.Collections.Generic.List[object]]::new()
  $seen = @{}
  # The active Codex transcript can expose thousands of streaming UIA nodes.
  # Probe the stable lower composer surface first so a running turn never needs
  # an unbounded descendant materialization before voice can stage its draft.
  foreach ($yRatio in @(0.76, 0.83, 0.89, 0.94)) {
    foreach ($xRatio in @(0.42, 0.60, 0.76, 0.89)) {
      if ((Get-MonotonicMilliseconds) -ge $deadline) { return @($elements.ToArray()) }
      try {
        $x = [double]($rootRect.Left + ($rootRect.Width * $xRatio))
        $y = [double]($rootRect.Top + ($rootRect.Height * $yRatio))
        $element = [System.Windows.Automation.AutomationElement]::FromPoint(
          [System.Windows.Point]::new($x, $y)
        )
        for ($depth = 0; $depth -lt 16 -and $null -ne $element; $depth += 1) {
          if ((Get-MonotonicMilliseconds) -ge $deadline) { break }
          if ($element.Current.NativeWindowHandle -eq $root.Current.NativeWindowHandle) { break }
          Add-UniqueComposerElement $element $elements $seen
          $element = $walker.GetParent($element)
        }
      } catch {}
    }
  }
  return @($elements.ToArray())
}

function Find-BoundedComposerElements($root, [bool]$rawView = $false, [int]$budgetMs = 1800) {
  $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
  if ($rawView) { $walker = [System.Windows.Automation.TreeWalker]::RawViewWalker }
  $pending = [System.Collections.Generic.Queue[object]]::new()
  $elements = [System.Collections.Generic.List[object]]::new()
  $seen = @{}
  $deadline = (Get-MonotonicMilliseconds) + $budgetMs
  $visited = 0
  $discovered = 0
  try {
    $child = $walker.GetFirstChild($root)
    while ($null -ne $child -and $discovered -lt 3500 -and
           (Get-MonotonicMilliseconds) -lt $deadline) {
      $pending.Enqueue([pscustomobject]@{ Element = $child; Depth = 1 })
      $discovered += 1
      $child = $walker.GetNextSibling($child)
    }
  } catch {}

  while ($pending.Count -gt 0 -and
         $visited -lt 3500 -and
         (Get-MonotonicMilliseconds) -lt $deadline) {
    $node = $pending.Dequeue()
    $element = $node.Element
    $depth = [int]$node.Depth
    $visited += 1
    try {
      $typeId = $element.Current.ControlType.Id
      if ($typeId -eq [System.Windows.Automation.ControlType]::Edit.Id -or
          $typeId -eq [System.Windows.Automation.ControlType]::Button.Id -or
          $typeId -eq [System.Windows.Automation.ControlType]::Group.Id -or
          $typeId -eq [System.Windows.Automation.ControlType]::Custom.Id -or
          $typeId -eq [System.Windows.Automation.ControlType]::Document.Id -or
          $typeId -eq [System.Windows.Automation.ControlType]::Text.Id -or
          $typeId -eq [System.Windows.Automation.ControlType]::Pane.Id) {
        Add-UniqueComposerElement $element $elements $seen
      }
    } catch {}

    if ($depth -ge 48 -or $discovered -ge 3500) { continue }
    try {
      $child = $walker.GetFirstChild($element)
      while ($null -ne $child -and
             $discovered -lt 3500 -and
             (Get-MonotonicMilliseconds) -lt $deadline) {
        $pending.Enqueue([pscustomobject]@{ Element = $child; Depth = $depth + 1 })
        $discovered += 1
        $child = $walker.GetNextSibling($child)
      }
    } catch {}
  }
  return @($elements.ToArray())
}

function Test-WorkBuddyWritablePattern($pattern) {
  try {
    if ($pattern.PSObject.TypeNames -contains 'Hachimi.ComposerLegacySnapshot') {
      return $pattern.Current.Role -eq 42 -and
        ($pattern.Current.State -is [int] -or $pattern.Current.State -is [uint32]) -and
        ($pattern.Current.State -band 0x20018041) -eq 0
    }
    if ($pattern.PSObject.TypeNames -contains 'Hachimi.ComposerInputCapability') {
      return $pattern.Current.IsReadOnly -is [bool] -and -not $pattern.Current.IsReadOnly
    }
    if ($pattern.PSObject.TypeNames -contains 'Hachimi.ComposerValueSnapshot') {
      return $pattern.Current.IsReadOnly -is [bool] -and -not $pattern.Current.IsReadOnly -and
        $pattern.Current.Value -is [string]
    }
    if ($pattern -is [System.Windows.Automation.ValuePattern] -or
        $pattern -is [WorkBuddyUiaValuePattern]) {
      return $pattern.Current.IsReadOnly -is [bool] -and -not $pattern.Current.IsReadOnly
    }
    if ($pattern -is [WorkBuddyUiaLegacyPattern]) {
      $current = $pattern.Current
      return $current.Role -eq 42 -and
        ($current.State -is [int] -or $current.State -is [uint32]) -and ($current.State -band 0x41) -eq 0
    }
    if ($pattern -is [System.Windows.Automation.TextPattern] -or
        $pattern -is [WorkBuddyUiaTextPattern]) {
      $readOnly = $pattern.DocumentRange.GetAttributeValue(
        [System.Windows.Automation.TextPattern]::IsReadOnlyAttribute
      )
      # Mixed/NotSupported is not evidence of an editable document.
      return ($readOnly -is [bool] -and -not $readOnly)
    }
  } catch {}
  return $false
}

function Test-WorkBuddyElementOwner($root, $element) {
  # Native raw-view parents only; a managed TreeWalker would hide Chromium's
  # provider for the rest of this helper process (see WorkBuddyUia).
  return [WorkBuddyUia]::IsOwnedBy([IntPtr]$root.Current.NativeWindowHandle, $element)
}

function Test-ComposerElementOwner($root, $element) {
  if ($element -is [WorkBuddyUiaElement]) { return Test-WorkBuddyElementOwner $root $element }
  $rootId = Get-ElementRuntimeId $root
  if (-not $rootId) { return $false }
  $walker = [System.Windows.Automation.TreeWalker]::RawViewWalker
  try {
    for ($depth = 0; $depth -lt 64 -and $null -ne $element; $depth += 1) {
      if ((Get-ElementRuntimeId $element) -ceq $rootId) { return $true }
      $element = $walker.GetParent($element)
    }
  } catch {}
  return $false
}

function Record-ComposerReject([string]$stage) { $script:ComposerRejects[$stage] += 1 }

function Get-ComposerCandidates($root, $elements, [string[]]$allowedValues, [bool]$workbuddy = $false) {
  $rootRect = $root.Current.BoundingRectangle
  if (-not (Test-FiniteWindowRectangle $rootRect)) {
    throw 'Visible Agent window geometry is unavailable'
  }
  $candidates = @()
  $unexpectedCandidates = @()
  $readableCount = 0
  $writableCount = 0
  foreach ($element in @($elements)) {
    try { $current = $element.Current } catch { continue }
    if (-not $current.IsEnabled) { Record-ComposerReject 'disabled';continue }
    if ($current.IsOffscreen) { Record-ComposerReject 'offscreen';continue }
    if (-not $current.IsKeyboardFocusable) { Record-ComposerReject 'focusable';continue }
    if (-not (Test-ComposerElementOwner $root $element)) { Record-ComposerReject 'owner';continue }
    $rect = $current.BoundingRectangle
    if (-not (Test-FiniteWindowRectangle $rect) -or $rect.Width -lt 240 -or $rect.Height -lt 22 -or
        $rect.Bottom -lt ($rootRect.Top + ($rootRect.Height * 0.40))) { Record-ComposerReject 'geometry';continue }

    $className = [string]$current.ClassName
    $name = Normalize-Label ([string]$current.Name)
    $typeId = $current.ControlType.Id
    $classHint = $className -match '(?i)(^|\s)(ProseMirror|tiptap)(\s|$)'
    $nameHint = $name -match '(?i)prompt|message|ask|write|输入|消息|提问'
    $ariaRole = ''
    try { $ariaRole = [string](Get-ComposerProperty $element 30101) } catch {}
    if ($ariaRole -match '^(?i:searchbox)$') { Record-ComposerReject 'search';continue }
    $legacyText = $element -is [WorkBuddyUiaElement] -and $element.HasWritableLegacyText()
    $documentHint = $typeId -eq [System.Windows.Automation.ControlType]::Document.Id -and
      ($classHint -or $ariaRole -ceq 'textbox' -or $legacyText)
    if ($typeId -eq [System.Windows.Automation.ControlType]::Document.Id -and -not $documentHint) { Record-ComposerReject 'semantic';continue }
    if ($workbuddy) {
      if ($name -match '(?i)search|搜索') { Record-ComposerReject 'search';continue }
      # Rich contenteditables may appear as Document rather than Edit. Require
      # explicit textbox/editor semantics, not merely a readable chat document.
    }
    $typeHint = $typeId -eq [System.Windows.Automation.ControlType]::Edit.Id -or
      $typeId -eq [System.Windows.Automation.ControlType]::Group.Id -or
      $typeId -eq [System.Windows.Automation.ControlType]::Custom.Id
    if (-not $classHint -and -not $nameHint -and -not $typeHint -and -not $documentHint -and -not $legacyText) { Record-ComposerReject 'semantic';continue }

    $valueInfo = $null
    try {
      $valueInfo = if ($script:NativeVoiceAppendOnly) { @((Get-NativeComposerInputPattern $element), '') }
        else { Get-ComposerText $element }
    } catch { Record-ComposerReject 'capability';continue }
    $readableCount += 1
    if (-not (Test-WorkBuddyWritablePattern $valueInfo[0])) { Record-ComposerReject 'writable';continue }
    $writableCount += 1
    $score = 0
    if ($classHint) { $score += 8 }
    if ($nameHint) { $score += 6 }
    if ($typeId -eq [System.Windows.Automation.ControlType]::Edit.Id -or $documentHint) { $score += 5 }
    elseif ($typeId -eq [System.Windows.Automation.ControlType]::Group.Id -or
            $typeId -eq [System.Windows.Automation.ControlType]::Custom.Id) { $score += 2 }
    if ($current.HasKeyboardFocus) { $score += 3 }
    if ($rect.Bottom -ge ($rootRect.Top + ($rootRect.Height * 0.72))) { $score += 2 }
    if ($rect.Width -ge ($rootRect.Width * 0.45)) { $score += 1 }
    if ($legacyText) { $score += 5 }
    if ($score -lt 5) { Record-ComposerReject 'score';continue }

    $candidate = [pscustomobject]@{
      Element = $element
      Pattern = $valueInfo[0]
      Value = [string]$valueInfo[1]
      Score = [int]$score
      Bottom = [double]$rect.Bottom
      Width = [double]$rect.Width
      BoundsKey = "$([int]$rect.Left),$([int]$rect.Top),$([int]$rect.Width),$([int]$rect.Height)"
      SemanticKey = "$typeId`:$className`:$ariaRole"
    }
    if (Test-AllowedComposerValue $candidate.Value $allowedValues) {
      $candidates += $candidate
    } else {
      $unexpectedCandidates += $candidate
    }
  }
  return [pscustomobject]@{
    Candidates = @($candidates)
    UnexpectedCandidates = @($unexpectedCandidates)
    ReadableCount = $readableCount
    WritableCount = $writableCount
  }
}

function Find-Composer($root, [string[]]$allowedValues) {
  # Codex rich textboxes can be Document nodes and can exist only in raw view.
  # A focused *owned* editor is stronger evidence than a screen point probe.
  try {
    $focused = [System.Windows.Automation.AutomationElement]::FocusedElement
    if ($null -ne $focused -and $focused.Current.HasKeyboardFocus) {
      $focusedSet = Get-ComposerCandidates $root @($focused) $allowedValues
      if (@($focusedSet.UnexpectedCandidates).Count -gt 0) { throw 'COMPOSER_DRAFT_CHANGED' }
      if (@($focusedSet.Candidates).Count -eq 1) { return $focusedSet.Candidates[0] }
    }
  } catch { if ($_.Exception.Message -eq 'COMPOSER_DRAFT_CHANGED') { throw } }
  $pointElements = @(Find-PointComposerElements $root)
  $candidateSet = Get-ComposerCandidates $root $pointElements $allowedValues
  $candidates = @($candidateSet.Candidates)
  $unexpectedCandidates = @($candidateSet.UnexpectedCandidates)
  if ($candidates.Count -eq 0 -and $unexpectedCandidates.Count -eq 0) {
    $boundedElements = @(Find-BoundedComposerElements $root)
    $candidateSet = Get-ComposerCandidates $root $boundedElements $allowedValues
    $candidates = @($candidateSet.Candidates)
    $unexpectedCandidates = @($candidateSet.UnexpectedCandidates)
  }
  if ($candidates.Count -eq 0 -and $unexpectedCandidates.Count -eq 0) {
    $rawElements = @(Find-BoundedComposerElements $root $true)
    $candidateSet = Get-ComposerCandidates $root $rawElements $allowedValues
    $candidates = @($candidateSet.Candidates)
    $unexpectedCandidates = @($candidateSet.UnexpectedCandidates)
  }
  if ($unexpectedCandidates.Count -gt 0) {
    throw 'Visible composer already contains a user draft; voice input was refused'
  }
  if ($candidates.Count -eq 0) { throw 'No writable visible semantic composer was found' }

  $ordered = @($candidates | Sort-Object `
    @{ Expression = 'Score'; Descending = $true }, `
    @{ Expression = 'Bottom'; Descending = $true }, `
    @{ Expression = 'Width'; Descending = $true })
  $distinct = @()
  $seenBounds = @{}
  foreach ($candidate in $ordered) {
    if ($seenBounds.ContainsKey($candidate.BoundsKey)) { continue }
    $seenBounds[$candidate.BoundsKey] = $true
    $distinct += $candidate
  }
  if ($distinct.Count -gt 1 -and $distinct[0].Score -eq $distinct[1].Score) {
    throw 'Multiple equally likely visible composers were found; voice input was refused'
  }
  return $distinct[0]
}

function Get-CodexTarget([string]$sessionTitle, [string[]]$allowedValues) {
  $windows = @(Get-CodexWindows)
  $matchedTitle = $false
  $composerErrors = @()
  foreach ($window in $windows) {
    try {
      $window.Root = Ensure-CodexForeground $window.Root 'for bound voice input'
      if (-not (Test-SelectedSessionTitle $window.Root $sessionTitle)) { continue }
      $matchedTitle = $true
      $composer = Find-Composer $window.Root $allowedValues
      return [pscustomobject]@{ Root = $window.Root; Composer = $composer }
    } catch {
      $composerErrors += $_.Exception.Message
    }
  }
  if (-not $matchedTitle) {
    throw 'ChatGPT（Codex） main window was found, but its active task title did not match the bound session'
  }
  throw "ChatGPT（Codex） task matched, but its composer was unavailable: $($composerErrors -join '; ')"
}

function Get-ClaudeTarget(
  [string]$sessionId,
  [string]$desktopSessionId,
  [string]$sessionTitle,
  [string[]]$allowedValues
) {
  $windows = @(Get-ClaudeWindows)
  $matchedSession = $false
  $composerErrors = @()
  foreach ($window in $windows) {
    if (-not (Test-ClaudeWindowSession $window.Root $sessionId $desktopSessionId $sessionTitle)) {
      continue
    }
    $matchedSession = $true
    try {
      $composer = Find-Composer $window.Root $allowedValues
      return [pscustomobject]@{
        Root = $window.Root
        RootHandle = $window.RootHandle
        WindowHandle = $window.WindowHandle
        ProcessId = $window.ProcessId
        Composer = $composer
      }
    } catch {
      $composerErrors += $_.Exception.Message
    }
  }
  if (-not $matchedSession) {
    throw 'Claude Desktop was found, but its active Code session did not match the bound session'
  }
  throw "Claude session matched, but its composer was unavailable: $($composerErrors -join '; ')"
}

function Get-ElementRuntimeId($element) {
  try { return @($element.GetRuntimeId()) -join '.' } catch { return '' }
}

function Select-WorkBuddyComposer($candidateSet) {
  if (@($candidateSet.UnexpectedCandidates).Count -gt 0) {
    throw 'WorkBuddy 草稿已被其他操作修改，已停止语音写入'
  }
  $distinct = @{}
  foreach ($candidate in @($candidateSet.Candidates)) {
    $identity = Get-ElementRuntimeId $candidate.Element
    if (-not $identity) { throw 'WorkBuddy 输入框没有稳定标识，已停止语音写入' }
    $distinct[$identity] = $candidate
  }
  if ($distinct.Count -gt 1) { throw 'WorkBuddy 存在多个可写输入框，请关闭搜索或弹窗后重试' }
  if ($distinct.Count -eq 1) { return @($distinct.Values)[0] }
  return $null
}

function Find-WorkBuddyFocusedComposer($root, [string[]]$allowedValues) {
  # Explicit keyboard focus is a stronger target than a guessed screen region.
  # Still apply every ownership, semantics, writability and draft guard.
  try {
    $focused = [WorkBuddyUia]::FocusedElement()
    if ($null -eq $focused -or -not $focused.Current.HasKeyboardFocus) { return $null }
  } catch { return $null }
  $set = Get-ComposerCandidates $root @([WorkBuddyUia]::FocusedAncestors([IntPtr]$root.Current.NativeWindowHandle)) $allowedValues $true
  return Select-WorkBuddyComposer $set
}

function Initialize-WorkBuddyAccessibility([IntPtr]$handle, [int]$ownerProcessId, [long]$deadline) {
  $key = "${ownerProcessId}:$handle"
  if ($script:WorkBuddyAccessibilityRequests.ContainsKey($key)) {
    return $script:WorkBuddyAccessibilityRequests[$key]
  }
  $info = [pscustomobject]@{ Requested = 0; Msaa = 0; Renderers = 0; Detected = 0 }
  # At most once per window per recording, even if activation fails. The next
  # recording uses a fresh helper and can retry; no permanent success cache.
  $script:WorkBuddyAccessibilityRequests[$key] = $info
  Write-ComposerProgress 'enable_accessibility'
  $targets = [System.Collections.Generic.List[IntPtr]]::new()
  $targets.Add($handle)
  foreach ($renderer in @([CodexVoiceNative]::FindChildWindowsByClass($handle, 'Chrome_RenderWidgetHostHWND'))) {
    if ($info.Renderers -ge 4) { break }
    if (-not [CodexVoiceNative]::IsOwnedWindow($handle, $renderer)) { continue }
    if ($targets.Contains($renderer)) { continue }
    $targets.Add($renderer)
    $info.Renderers += 1
  }
  foreach ($target in $targets) {
    if ((Get-MonotonicMilliseconds) -ge $deadline -or
        -not [CodexVoiceNative]::IsForeground($handle)) { break }
    $info.Requested += 1
    try {
      $responses = [CodexVoiceNative]::RequestClientAccessibility($handle, $target, $ownerProcessId)
      if (($responses -band 1) -ne 0) { $info.Msaa += 1 }
      if (($responses -band 2) -ne 0) { $info.Detected += 1 }
    } catch {
      # Re-read UIA after a rejected MSAA request as well. Never treat a
      # handshake response or exception as proof that editing is safe.
    }
  }
  return $info
}

function Test-CurrentElectronAgent([string]$agent) {
  return @('workbuddy','deepseek-harness') -contains $agent -or ($agent -eq 'codex' -and $script:NativeVoiceAppendOnly)
}

function Get-WorkBuddyTarget([string[]]$allowedValues, [string]$agent = 'workbuddy') {
  Write-ComposerProgress 'resolve_window'
  $processName = if ($agent -eq 'deepseek-harness') { 'DeepSeek Harness' } elseif ($agent -eq 'codex') { 'ChatGPT' } else { 'WorkBuddy' }
  $processNames = if ($agent -eq 'codex') { @('ChatGPT','Codex') } else { @($processName) }
  $windows = @(Get-Process -Name $processNames -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 })
  if ($agent -eq 'codex' -and $windows.Count -gt 1) { $windows = @($windows | Where-Object { [CodexVoiceNative]::IsForeground([IntPtr]$_.MainWindowHandle) }) }
  if ($windows.Count -ne 1) { throw '请保留唯一目标 Agent 主窗口并打开目标对话，再重新开始语音输入' }
  $process = $windows[0]
  $handle = [IntPtr]$process.MainWindowHandle
  if (-not [CodexVoiceNative]::ActivateWindow($handle)) { throw '无法激活 WorkBuddy 目标窗口' }
  # Same activation settling as Codex/Claude. Refresh roots on each attempt;
  # a valid main HWND does not mean Chromium has populated its editable tree.
  Start-Sleep -Milliseconds 120
  $deadline = (Get-MonotonicMilliseconds) + 4500
  $lastCount = 0
  $readableCount = 0
  $writableCount = 0
  $accessibilityInfo = $null
  do {
    if (-not [CodexVoiceNative]::IsForeground($handle)) { throw 'WorkBuddy 已失去前台焦点，未写入文字' }
    $process.Refresh()
    if ([IntPtr]$process.MainWindowHandle -ne $handle) { throw 'WorkBuddy 主窗口已变化，请重新开始录音' }
    # Native UIA only. Chromium serves this tree from the owned
    # Chrome_RenderWidgetHostHWND, which the native walkers cross from the
    # main window; managed AutomationElement would permanently hide it.
    $root = [WorkBuddyUia]::FromHandle($handle)
    if ($null -ne $root -and (Test-FiniteWindowRectangle $root.Current.BoundingRectangle)) {
      $elements = [System.Collections.Generic.List[object]]::new()
      $seen = @{}
      Write-ComposerProgress 'focused'
      $composer = Find-WorkBuddyFocusedComposer $root $allowedValues
      if ($null -eq $composer) {
        Write-ComposerProgress 'native_query'
        foreach ($element in @([WorkBuddyUia]::FindEditable($handle, 1000, 256))) { Add-UniqueComposerElement $element $elements $seen }
        $candidateSet = Get-ComposerCandidates $root @($elements.ToArray()) $allowedValues $true
        $composer = Select-WorkBuddyComposer $candidateSet
      }
      if ($null -eq $composer) {
        Write-ComposerProgress 'control'
        foreach ($element in @([WorkBuddyUia]::FindBounded($handle, $false, 1500, 3500))) { Add-UniqueComposerElement $element $elements $seen }
        $candidateSet = Get-ComposerCandidates $root @($elements.ToArray()) $allowedValues $true
        $composer = Select-WorkBuddyComposer $candidateSet
      }
      if ($null -eq $composer) {
        if ($null -eq $accessibilityInfo -and (Get-MonotonicMilliseconds) -lt $deadline) {
          $accessibilityInfo = Initialize-WorkBuddyAccessibility $handle ([int]$process.Id) $deadline
          Start-Sleep -Milliseconds 150
          # Enabling Chromium accessibility replaces proxy/stub elements.
          # Discard every pre-activation root, candidate and runtime identity.
          continue
        }
        Write-ComposerProgress 'raw'
        # Raw view also includes non-control containers and the owned
        # Chrome_RenderWidgetHostHWND subtree.
        foreach ($element in @([WorkBuddyUia]::FindBounded($handle, $true, 1500, 3500))) { Add-UniqueComposerElement $element $elements $seen }
        $candidateSet = Get-ComposerCandidates $root @($elements.ToArray()) $allowedValues $true
        $composer = Select-WorkBuddyComposer $candidateSet
      }
      if ($null -eq $composer -and (Get-MonotonicMilliseconds) -lt $deadline) {
        Write-ComposerProgress 'probe'
        $points = [WorkBuddyUia]::FindAtPoints($handle, [double[]]@(0.42, 0.60, 0.76, 0.89), [double[]]@(0.76, 0.83, 0.89, 0.94), 250)
        foreach ($element in @($points)) { Add-UniqueComposerElement $element $elements $seen }
        $candidateSet = Get-ComposerCandidates $root @($elements.ToArray()) $allowedValues $true
        $composer = Select-WorkBuddyComposer $candidateSet
      }
      if ($null -ne $composer) {
        Write-ComposerProgress 'validate'
        Start-Sleep -Milliseconds 120
        $current = if ($script:NativeVoiceAppendOnly) { @((Get-NativeComposerInputPattern $composer.Element), '') }
          else { Get-ComposerText $composer.Element }
        if ([CodexVoiceNative]::IsForeground($handle) -and
            (Test-WorkBuddyElementOwner $root $composer.Element) -and
            $composer.Element.Current.IsEnabled -and -not $composer.Element.Current.IsOffscreen -and
            [string]$current[1] -ceq $composer.Value) {
          return [pscustomobject]@{ Root = $root; RootHandle = [int64]$handle; WindowHandle = [int64]$handle; ProcessId = [int]$process.Id; Composer = $composer }
        }
      } else {
        $lastCount = $elements.Count
        $readableCount = $candidateSet.ReadableCount
        $writableCount = $candidateSet.WritableCount
      }
    }
    Start-Sleep -Milliseconds 80
  } while ((Get-MonotonicMilliseconds) -lt $deadline)
  # No window names, composer contents, local paths, or account data in errors.
  $activation = if ($null -ne $accessibilityInfo) {
    "，辅助功能请求=$($accessibilityInfo.Requested)，MSAA应答=$($accessibilityInfo.Msaa)，渲染窗口=$($accessibilityInfo.Renderers)，激活应答=$($accessibilityInfo.Detected)"
  } else { '' }
  $stages = @($script:ComposerRejects.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ','
  $native = [WorkBuddyUia]::DiagnosticSummary()
  throw "Visible Agent 输入框未就绪（节点=$lastCount，可读=$readableCount，可写=$writableCount$activation；$native；$stages）；未取得唯一可写编辑器，尚未写入或发送文字"
}

function Get-CurrentVisibleTarget([string]$agent, [string[]]$allowedValues) {
  if (Test-CurrentElectronAgent $agent) {
    return Get-WorkBuddyTarget $allowedValues $agent
  }
  if ($agent -eq 'codex') {
    $windows = @(Get-CodexWindows)
    if ($windows.Count -eq 0) { throw 'Open the intended Codex window before current-visible voice input; no application was launched' }
    if ($windows.Count -gt 1) {
      $windows = @($windows | Where-Object { [CodexVoiceNative]::IsForeground([IntPtr]$_.Root.Current.NativeWindowHandle) })
      if ($windows.Count -ne 1) { throw 'Multiple Codex windows are open; keep the intended conversation foreground' }
    }
    $errors = @()
    foreach ($window in $windows) {
      try {
        $window.Root = Ensure-CodexForeground $window.Root 'for current voice input'
        $composer = Find-Composer $window.Root $allowedValues
        return [pscustomobject]@{
          Root = $window.Root
          RootHandle = [int64]$window.Root.Current.NativeWindowHandle
          WindowHandle = [int64]$window.Root.Current.NativeWindowHandle
          ProcessId = [int]$window.Root.Current.ProcessId
          Composer = $composer
        }
      } catch {
        $errors += $_.Exception.Message
      }
    }
    throw "ChatGPT（Codex）窗口已找到，但前台输入框无法定位: $($errors -join '; ')"
  }
  if ($agent -eq 'claude') {
    $windows = @(Get-ClaudeWindows)
    if ($windows.Count -eq 0) { throw 'Open the intended Claude window before current-visible voice input; no application was launched' }
    $window = $windows[0]
    $activated = [CodexVoiceNative]::ActivateWindow([IntPtr]$window.WindowHandle)
    if (-not $activated) {
      try {
        $shell = New-Object -ComObject WScript.Shell
        [void]$shell.AppActivate([int]$window.ProcessId)
        Start-Sleep -Milliseconds 80
        $activated = [CodexVoiceNative]::ActivateWindow([IntPtr]$window.WindowHandle)
      } catch {}
    }
    if (-not $activated) {
      throw 'Claude main window could not become foreground for current voice input'
    }
    Start-Sleep -Milliseconds 120
    $composer = Find-Composer $window.Root $allowedValues
    return [pscustomobject]@{
      Root = $window.Root
      RootHandle = [int64]$window.RootHandle
      WindowHandle = [int64]$window.WindowHandle
      ProcessId = [int]$window.ProcessId
      Composer = $composer
    }
  }
  throw "Unsupported current visible composer agent: $agent"
}

function Assert-WorkBuddyComposerCurrent($state) {
  # Re-verify the composer bound at begin instead of re-running the whole
  # window lookup (two settle sleeps plus tree scans) before and after every
  # write. The same element must still exist with the recorded runtime ID,
  # stay owned, enabled, visible and writable; the caller then requires the
  # exact last voice-controlled text.
  $handle = [IntPtr]$state.WindowHandle
  if ($state.Agent -eq 'deepseek-harness' -and
      (-not [CodexVoiceNative]::IsForeground($handle) -or
       (Get-Process -Id $state.ProcessId -ErrorAction Stop).ProcessName -ine 'DeepSeek Harness')) {
    throw 'DESKTOP_TASK_TARGET_CHANGED'
  }
  if (-not [CodexVoiceNative]::IsForeground($handle) -and
      -not [CodexVoiceNative]::ActivateWindow($handle)) {
    throw 'WorkBuddy 已失去前台焦点，未写入文字'
  }
  $element = $state.Composer
  $runtimeId = Get-ElementRuntimeId $element
  if (-not $runtimeId -or $runtimeId -ne $state.ComposerRuntimeId -or
      -not (Test-WorkBuddyElementOwner $state.Root $element) -or
      -not $element.Current.IsEnabled -or $element.Current.IsOffscreen -or
      -not (Test-WorkBuddyWritablePattern (Get-ComposerText $element)[0])) {
    throw 'The visible Agent session or composer changed during voice input'
  }
}

function Get-TaskWindowIdentity($root, [string]$agent = 'codex') {
  $rect = $root.Current.BoundingRectangle
  $labels = @([string]$root.Current.Name)
  $all = if (Test-CurrentElectronAgent $agent) {
    @([WorkBuddyUia]::FindBounded([IntPtr]$root.Current.NativeWindowHandle, $false, 150, 256) | Where-Object { $_.Current.ControlType.Id -eq 50020 })
  } else {
    @(Find-BoundedComposerElements $root $false 150 | Where-Object { $_.Current.ControlType.Id -eq 50020 })
  }
  foreach ($element in $all) {
    $r = $element.Current.BoundingRectangle
    if (-not $element.Current.IsOffscreen -and (Test-FiniteWindowRectangle $r) -and
        $r.Top -ge $rect.Top -and $r.Bottom -le ($rect.Top + 100) -and
        $r.Left -ge ($rect.Left + [Math]::Min(280, $rect.Width * 0.25))) {
      $labels += [string]$element.Current.Name
    }
  }
  return ($labels -join [char]31)
}

function Test-SpecificComposerIdentity([string]$identity) {
  $parts = @($identity.Split([char]31) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
  if ($parts.Count -gt 1) { return $true }
  if ($parts.Count -ne 1) { return $false }
  # A process/application title alone cannot authorize a different UIA editor.
  return @('chatgpt','codex','workbuddy','deepseek harness','claude') -notcontains $parts[0].Trim().ToLowerInvariant()
}

function Get-BoundComposerRoot($state, [bool]$forReacquire = $false) {
  [void](Get-Process -Id $state.ProcessId -ErrorAction Stop)
  $handle = [IntPtr]$state.WindowHandle
  if (-not [CodexVoiceNative]::IsWindow($handle) -or -not [CodexVoiceNative]::IsForeground($handle)) {
    throw 'DESKTOP_TASK_TARGET_CHANGED'
  }
  $root = if ($state.NativeUia) { [WorkBuddyUia]::FromHandle([IntPtr]$state.RootHandle) }
    else { [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$state.RootHandle) }
  if ($null -eq $root -or $root.Current.NativeWindowHandle -ne $state.RootHandle -or
      $root.Current.ProcessId -ne $state.ProcessId) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
  if ($state.NativeVoiceAppendOnly) {
    $process = Get-Process -Id $state.ProcessId -ErrorAction Stop
    if ($process.StartTime.ToUniversalTime().Ticks -ne $state.ProcessStartTicks) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
    if (-not $state.CurrentVisible) {
      $matches = if ($state.Agent -eq 'claude') {
        Test-ClaudeWindowSession $root $state.SessionId $state.DesktopSessionId $state.SessionTitle
      } else { Test-SelectedSessionTitle $root $state.SessionTitle }
      if (-not $matches) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
    } elseif ($forReacquire) { Assert-NativeVoiceConversation $state $root }
  } elseif ($state.CurrentVisible) {
    if (-not $state.TaskIdentity -or (Get-TaskWindowIdentity $root $state.Agent) -cne $state.TaskIdentity) {
      throw 'DESKTOP_TASK_TARGET_CHANGED'
    }
    if ($forReacquire -and -not (Test-SpecificComposerIdentity $state.TaskIdentity)) {
      throw 'Visible conversation identity is insufficient to reacquire its editor'
    }
  } else {
    $matches = if ($state.Agent -eq 'claude') {
      Test-ClaudeWindowSession $root $state.SessionId $state.DesktopSessionId $state.SessionTitle
    } else { Test-SelectedSessionTitle $root $state.SessionTitle }
    if (-not $matches) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
  }
  return $root
}

function Reacquire-BoundComposer($state, [string]$expectedValue, [int]$budgetMs = 150) {
  # Read-only recovery: never enumerate/activate another application window,
  # navigate to a conversation, click, type, or choose a latest session.
  $root = Get-BoundComposerRoot $state $true
  $elements = [System.Collections.Generic.List[object]]::new()
  $seen = @{}
  if ($state.NativeUia) {
    $focused = [WorkBuddyUia]::FocusedElement()
    if ($null -ne $focused) { Add-UniqueComposerElement $focused $elements $seen }
    foreach ($e in @([WorkBuddyUia]::FindEditable([IntPtr]$state.RootHandle, $budgetMs, 256))) { Add-UniqueComposerElement $e $elements $seen }
    foreach ($e in @([WorkBuddyUia]::FindBounded([IntPtr]$state.RootHandle, $true, $budgetMs, 256))) {
      Add-UniqueComposerElement $e $elements $seen
    }
  } else {
    $focused = [System.Windows.Automation.AutomationElement]::FocusedElement
    if ($null -ne $focused) { Add-UniqueComposerElement $focused $elements $seen }
    foreach ($e in @(Find-BoundedComposerElements $root $true $budgetMs)) {
      Add-UniqueComposerElement $e $elements $seen
    }
  }
  $allowed = if ($state.NativeVoiceAppendOnly) { $null } else { @($expectedValue) }
  $set = Get-ComposerCandidates $root @($elements.ToArray()) $allowed ([bool]$state.NativeUia)
  $matching = @($set.Candidates | Where-Object { $_.SemanticKey -ceq $state.ComposerSemanticKey })
  $unexpected = @($set.UnexpectedCandidates | Where-Object { $_.SemanticKey -ceq $state.ComposerSemanticKey })
  if (($state.NativeVoiceAppendOnly -and @($set.Candidates).Count -ne 1) -or $matching.Count -ne 1 -or $unexpected.Count -gt 0) {
    throw 'The bound conversation did not expose one matching semantic editor'
  }
  $candidate = $matching[0]
  $runtimeId = Get-ElementRuntimeId $candidate.Element
  if (-not $runtimeId) { throw 'The replacement editor has no stable runtime identity' }
  # Recheck conversation and actual draft after tree traversal, before adopting.
  [void](Get-BoundComposerRoot $state $true)
  $value = if ($state.NativeVoiceAppendOnly) { @((Get-NativeComposerInputPattern $candidate.Element), '') }
    else { Get-ComposerText $candidate.Element }
  if (-not $state.NativeVoiceAppendOnly -and [string]$value[1] -cne $expectedValue) { throw 'Visible draft changed during editor reacquisition' }
  $state.Root = $root
  $state.Composer = $candidate.Element
  $state.ComposerRuntimeId = $runtimeId
  $state.Pattern = $value[0]
}

function Get-CurrentTaskTarget([string[]]$allowedValues, [string]$agent = 'codex') {
  if (Test-CurrentElectronAgent $agent) { return Get-WorkBuddyTarget $allowedValues $agent }
  $windows = @(Get-CodexWindows)
  if ($windows.Count -ne 1) { throw 'Please open the target Codex conversation in one main window; nothing was sent' }
  $root = Ensure-CodexForeground $windows[0].Root 'for current task'
  $composer = Find-Composer $root $allowedValues
  return [pscustomobject]@{
    Root = $root; RootHandle = [int64]$root.Current.NativeWindowHandle
    WindowHandle = [int64]$root.Current.NativeWindowHandle
    ProcessId = [int]$root.Current.ProcessId; Composer = $composer
  }
}

function Assert-TargetCurrent($state, [bool]$checkSession, [bool]$checkDraft = $true) {
  if ($state.NativeVoiceAppendOnly) {
    if (-not (Test-NativeVoiceEditorIdentity $state)) { Reacquire-BoundComposer $state $null }
    Assert-NativeVoiceComposer $state
    return
  }
  [void](Get-Process -Id $state.ProcessId -ErrorAction Stop)
  if (-not [CodexVoiceNative]::IsWindow([IntPtr]$state.WindowHandle) -or
      $state.Root.Current.NativeWindowHandle -ne $state.RootHandle) {
    throw 'Visible Agent window changed during voice input'
  }
  if ($state.CurrentVisible -and (Test-CurrentElectronAgent $state.Agent)) {
    Assert-WorkBuddyComposerCurrent $state
    if (($state.CurrentTask -or $state.Agent -eq 'deepseek-harness') -and (Get-TaskWindowIdentity $state.Root $state.Agent) -cne $state.TaskIdentity) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
  } elseif ($state.CurrentVisible) {
    if ($checkSession) {
      $allowedValues = if ($checkDraft) { @($state.LastValue) } else { @('', $state.LastValue) }
      $current = if ($state.CurrentTask) { Get-CurrentTaskTarget $allowedValues } else { Get-CurrentVisibleTarget $state.Agent $allowedValues }
      $runtimeId = Get-ElementRuntimeId $current.Composer.Element
      if ($current.ProcessId -ne $state.ProcessId -or
          $current.WindowHandle -ne $state.WindowHandle -or
          $current.RootHandle -ne $state.RootHandle -or
          -not $runtimeId -or $runtimeId -ne $state.ComposerRuntimeId) {
        throw 'The visible Agent session or composer changed during voice input'
      }
      if ($state.CurrentTask -and (Get-TaskWindowIdentity $current.Root) -cne $state.TaskIdentity) {
        throw 'DESKTOP_TASK_TARGET_CHANGED'
      }
    }
  } else {
    $sessionMatches = if ($state.Agent -eq 'claude') {
      Test-ClaudeWindowSession `
        $state.Root $state.SessionId $state.DesktopSessionId $state.SessionTitle
    } else {
      Test-SelectedSessionTitle $state.Root $state.SessionTitle
    }
    if ($checkSession -and -not $sessionMatches) {
      throw 'The visible Agent session no longer matches the bound session'
    }
  }
  $currentInfo = Get-ComposerText $state.Composer
  $currentValue = [string]$currentInfo[1]
  if ($checkDraft -and $currentValue -cne $state.LastValue) {
    throw 'Visible composer was edited outside voice input; voice updates were cancelled'
  }
  $state.Pattern = $currentInfo[0]
}

function Focus-Composer($state) {
  Write-ComposerProgress 'focus'
  if (($state.NativeVoiceAppendOnly -or $state.Agent -eq 'deepseek-harness') -and -not [CodexVoiceNative]::IsForeground([IntPtr]$state.WindowHandle)) {
    throw 'DESKTOP_TASK_TARGET_CHANGED'
  }
  $activated = [CodexVoiceNative]::ActivateWindow([IntPtr]$state.WindowHandle)
  if (-not $activated) {
    try {
      $shell = New-Object -ComObject WScript.Shell
      [void]$shell.AppActivate([int]$state.ProcessId)
      Start-Sleep -Milliseconds 60
      $activated = [CodexVoiceNative]::ActivateWindow([IntPtr]$state.WindowHandle)
    } catch {}
  }
  if (-not $activated) {
    throw 'Agent window could not become foreground; unlock Windows and keep the client visible'
  }
  for ($attempt = 0; $attempt -lt 2; $attempt += 1) {
    try {
      if (-not $state.Composer.Current.HasKeyboardFocus) { $state.Composer.SetFocus() }
      Start-Sleep -Milliseconds 30
      if (-not $state.Composer.Current.HasKeyboardFocus) { throw 'Composer focus was not confirmed' }
      $focused = if ($state.NativeUia) { [WorkBuddyUia]::FocusedElement() }
        else { [System.Windows.Automation.AutomationElement]::FocusedElement }
      if ($null -eq $focused -or (Get-ElementRuntimeId $focused) -cne $state.ComposerRuntimeId -or
          -not (Test-ComposerElementOwner $state.Root $focused)) {
        throw 'Composer focus no longer belongs to the bound editor'
      }
      return
    } catch {
      if ($attempt -ne 0) { throw }
      # Focusing a contenteditable may replace its accessibility node. Recover
      # only before input, with the exact original draft in the same conversation.
      Reacquire-BoundComposer $state $state.LastValue
    }
  }
}

function Rebind-CodexTarget($state, [string[]]$allowedValues) {
  if ($state.CurrentVisible) {
    Assert-TargetCurrent $state $true
    return
  }
  $target = if ($state.Agent -eq 'claude') {
    Get-ClaudeTarget `
      $state.SessionId $state.DesktopSessionId $state.SessionTitle $allowedValues
  } else {
    Get-CodexTarget $state.SessionTitle $allowedValues
  }
  $root = $target.Root
  $composer = $target.Composer
  $state.ProcessId = if ($state.Agent -eq 'claude') { [int]$target.ProcessId } else { [int]$root.Current.ProcessId }
  $state.WindowHandle = if ($state.Agent -eq 'claude') { [int64]$target.WindowHandle } else { [int64]$root.Current.NativeWindowHandle }
  $state.RootHandle = if ($state.Agent -eq 'claude') { [int64]$target.RootHandle } else { [int64]$root.Current.NativeWindowHandle }
  $state.Root = $root
  $state.Composer = $composer.Element
  $state.Pattern = $composer.Pattern
  $state.LastValue = [string]$composer.Value
  $state.LastSessionCheck = Get-MonotonicMilliseconds
}

function Wait-ComposerText($state, [string]$expectedText, [int]$timeoutMs) {
  $deadline = (Get-MonotonicMilliseconds) + $timeoutMs
  do {
    try {
      [void](Get-BoundComposerRoot $state)
      $updatedInfo = Get-ComposerText $state.Composer
      $updatedValue = [string]$updatedInfo[1]
      if ($updatedValue -ceq $expectedText) {
        $state.Pattern = $updatedInfo[0]
        return $true
      }
    } catch {}
    # A successful key injection may rerender the old UIA node. Only re-read a
    # replacement; this function never repeats the write or changes LastValue.
    $remaining = $deadline - (Get-MonotonicMilliseconds)
    if ($remaining -gt 0) {
      try {
        Reacquire-BoundComposer $state $expectedText ([Math]::Min(150, $remaining))
        return $true
      } catch {}
    }
    if ((Get-MonotonicMilliseconds) -ge $deadline) { break }
    Start-Sleep -Milliseconds 25
  } while ($true)
  return $false
}

function Set-ComposerText($state, [string]$text, [bool]$forceSessionCheck, [bool]$exactText = $false) {
  $normalizedText = if ($exactText) { Normalize-ComposerText $text } else { Join-VoiceDraft $state.BaseValue $text }
  if ($normalizedText -ceq $state.LastValue) {
    Assert-TargetCurrent $state $forceSessionCheck
    return
  }
  $now = Get-MonotonicMilliseconds
  $checkSession = $forceSessionCheck -or (($now - $state.LastSessionCheck) -ge 900)
  try { Assert-TargetCurrent $state $checkSession }
  catch {
    Reacquire-BoundComposer $state $state.LastValue
    Assert-TargetCurrent $state $true
  }
  if ($checkSession) { $state.LastSessionCheck = $now }
  Focus-Composer $state
  Assert-TargetCurrent $state $false
  [void](Get-BoundComposerRoot $state)
  # Exactly one write attempt for every Windows agent. Any partial SendInput or
  # unreadable ACK is uncertain, even when the API throws before returning.
  $state.WriteAttempted = $true
  try {
      [CodexVoiceNative]::ReplaceFocusedText($normalizedText)
      # Chromium can block a synchronous TextPattern read while committing the
      # just-injected ProseMirror update. Let the accessibility tree settle.
      Start-Sleep -Milliseconds 120
      # WorkBuddy receives the whole final transcript as one burst of typed
      # characters; allow for its length. Readback returns on first match.
      $confirmMs = if ((Test-CurrentElectronAgent $state.Agent)) {
        [Math]::Min(3000, 350 + (10 * $normalizedText.Length))
      } else { 350 }
      if (-not (Wait-ComposerText $state $normalizedText $confirmMs)) {
        throw 'Composer readback was not confirmed'
      }
      $state.LastValue = $normalizedText
      $state.WriteAttempted = $false
  } catch {
    if ([CodexVoiceNative]::LastInputRejectedBeforeSend) {
      # Validated Held/Invalid before SendInput is known not to have written.
      $state.WriteAttempted = $false
      throw
    }
    throw 'COMPOSER_WRITE_ACK_UNKNOWN: Windows may have inserted part or all of this segment; stopped without repeating input. Keep the local transcript and inspect the target draft.'
  }
}

function Clear-ComposerVoiceText($state) {
  if ($state.NativeVoiceAppendOnly) { return } # release preserves both old and newly appended draft
  if ($state.LastValue -ceq $state.BaseValue) { return }
  Assert-TargetCurrent $state (-not [bool]$state.CurrentVisible)
  Focus-Composer $state
  Assert-TargetCurrent $state $false
  [CodexVoiceNative]::ReplaceFocusedText($state.BaseValue)
  $state.LastValue = $state.BaseValue
}

function Assert-ComposerHasText($state) {
  if ($state.NativeVoiceAppendOnly) {
    if (-not $state.NativeVoiceWritten -or [string]::IsNullOrEmpty($state.NativeVoiceText)) { throw 'No acknowledged native voice append to submit' }
    # No title is required for an unchanged exact editor. Full post-append
    # owned value+hash corroborates the same draft before this explicit confirm.
    Assert-NativeVoiceOwnedDraft $state $true
    return
  }
  Assert-TargetCurrent $state (-not [bool]$state.CurrentVisible)
  $currentInfo = Get-ComposerText $state.Composer
  $currentValue = Normalize-ComposerText ([string]$currentInfo[1])
  if ([string]::IsNullOrEmpty($currentValue)) {
    throw 'Visible composer voice draft is no longer present; it was not sent again'
  }
  $state.Pattern = $currentInfo[0]
  $state.LastValue = $currentValue
}

function Get-ComposerSubmitLabelScore([string]$value) {
  $label = (Normalize-Label $value).ToLowerInvariant()
  if (@('send', 'submit', '发送', '提交') -contains $label) { return 0 }
  # A running Codex task relabels the same composer-adjacent button to Queue or
  # Steer. Match only these exact labels; never accept Stop or queue-management
  # controls through a broad substring rule.
  if (@(
      'queue', 'steer', '排队', '引导',
      '加入队列', '排入队列', '加入佇列', '排入佇列'
    ) -contains $label) { return 1 }
  foreach ($phrase in @('send message', 'submit message', '发送消息', '提交消息')) {
    if ($label.Contains($phrase)) { return 2 }
  }
  return -1
}

function Get-TaskDraftSnapshot($state) {
  if ($null -eq $state -or -not $state.CurrentTask -or @('codex','workbuddy','deepseek-harness') -notcontains $state.Agent) {
    throw 'DESKTOP_TASK_TARGET_CHANGED'
  }
  # Edits made between voice requests are intentional. Pin the current value
  # now; changes during this write transaction must still fail closed.
  Assert-TargetCurrent $state $true
  return @{
    target = @{
      agent = $state.Agent
      processId = $state.ProcessId; windowHandle = $state.WindowHandle
      rootHandle = $state.RootHandle; composerId = $state.ComposerRuntimeId
      taskIdentity = $state.TaskIdentity
    }
    text = [string](Get-ComposerText $state.Composer)[1]
  }
}

function Assert-TaskDraftSnapshot($state, $expected) {
  $current = Get-TaskDraftSnapshot $state
  foreach ($key in @('agent','processId','windowHandle','rootHandle','composerId','taskIdentity')) {
    if ($null -eq $expected.target.$key -or
        [string]$current.target.$key -cne [string]$expected.target.$key) {
      throw 'DESKTOP_TASK_TARGET_CHANGED'
    }
  }
  if ([string]$current.text -cne [string]$expected.text) { throw 'DESKTOP_TASK_READBACK_CHANGED' }
}

function Test-CodexSubmitPoint($state, $button, [int]$x, [int]$y) {
  $buttonId = Get-ElementRuntimeId $button
  $rootId = Get-ElementRuntimeId $state.Root
  if (-not $buttonId -or -not $rootId) { return $false }
  $element = [System.Windows.Automation.AutomationElement]::FromPoint(
    [System.Windows.Point]::new($x, $y)
  )
  $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
  $found = $false
  for ($depth = 0; $null -ne $element -and $depth -lt 32; $depth += 1) {
    $id = Get-ElementRuntimeId $element
    if ($id -eq $buttonId) { $found = $true }
    if ($id -eq $rootId) { return $found }
    $element = $walker.GetParent($element)
  }
  return $false
}

function Invoke-CodexTaskSubmit($state, $candidate) {
  # InvokePattern can acknowledge a Chromium control without submitting its
  # composer. Use one hit-tested native click instead, never Invoke then retry.
  Assert-TargetCurrent $state $true
  $button = $candidate.Element
  $rect = $button.Current.BoundingRectangle
  $rootRect = $state.Root.Current.BoundingRectangle
  $composerRect = $state.Composer.Current.BoundingRectangle
  if (-not $button.Current.IsEnabled -or $button.Current.IsOffscreen -or
      $button.Current.ControlType.Id -ne [System.Windows.Automation.ControlType]::Button.Id -or
      [string]$button.Current.Name -cne $candidate.Label -or
      -not (Test-FiniteWindowRectangle $rect) -or
      -not (Test-FiniteWindowRectangle $rootRect) -or
      -not (Test-FiniteWindowRectangle $composerRect) -or
      $rect.Left -lt $rootRect.Left -or $rect.Right -gt $rootRect.Right -or
      $rect.Top -lt $rootRect.Top -or $rect.Bottom -gt $rootRect.Bottom -or
      $rect.Bottom -lt ($composerRect.Top - 12) -or $rect.Top -gt ($composerRect.Bottom + 80) -or
      $rect.Right -lt ($composerRect.Left - 12) -or $rect.Left -gt ($composerRect.Right + 120)) {
    throw 'DESKTOP_TASK_SEND_UNAVAILABLE'
  }
  $x = [int][Math]::Floor($rect.Left + $rect.Width / 2)
  $y = [int][Math]::Floor($rect.Top + $rect.Height / 2)
  if (-not (Test-CodexSubmitPoint $state $button $x $y)) { throw 'DESKTOP_TASK_SEND_UNAVAILABLE' }
  if (-not [CodexVoiceNative]::IsForeground([IntPtr]$state.WindowHandle)) { throw 'DESKTOP_TASK_TARGET_CHANGED' }
  if ([string](Get-ComposerText $state.Composer)[1] -cne $state.LastValue) { throw 'DESKTOP_TASK_READBACK_CHANGED' }
  [CodexVoiceNative]::ClickSubmit([IntPtr]$state.WindowHandle, $x, $y)
}

function Wait-NativeVoiceSubmitClear($state, [int]$budgetMs = 1800) {
  $deadline = (Get-MonotonicMilliseconds) + $budgetMs
  $stableSince = $null; $stableEditor = ''
  do {
    try {
      $root = Get-BoundComposerRoot $state
      if (-not (Test-NativeVoiceEditorIdentity $state)) { Reacquire-BoundComposer $state $null 150 }
      Assert-NativeVoiceComposer $state
      $value = Get-ComposerText $state.Composer
      if ([string]$value[1] -ceq $state.NativeVoiceOwnedValue) {
        $stableSince = $null
        Start-Sleep -Milliseconds 40
        continue # old draft still pending; do not mistake API ACK for delivery
      }
      if ([string]$value[1] -cne '') { return $false } # preserve a new draft; unknown, no write
      $r = $state.Composer.Current.BoundingRectangle
      $all = if ($state.NativeUia) {
        @([WorkBuddyUia]::FindBounded([IntPtr]$state.RootHandle, $false, 150, 3500))
      } else { @(Find-BoundedComposerElements $root $false 150) }
      $buttons = @($all | Where-Object {
        $_.Current.ControlType.Id -eq [System.Windows.Automation.ControlType]::Button.Id -and
        -not $_.Current.IsOffscreen -and (Get-ComposerSubmitLabelScore ([string]$_.Current.Name)) -eq 0 -and
        (Test-ComposerElementOwner $root $_) -and (Test-FiniteWindowRectangle $_.Current.BoundingRectangle) -and
        $_.Current.BoundingRectangle.Top -le ($r.Bottom + 80) -and $_.Current.BoundingRectangle.Bottom -ge ($r.Top - 12) -and
        $_.Current.BoundingRectangle.Left -le ($r.Right + 120) -and $_.Current.BoundingRectangle.Right -ge ($r.Left - 12)
      })
      $editor = Get-ElementRuntimeId $state.Composer
      $disabled = $false
      if ($buttons.Count -eq 1) {
        # Native Current.Flag maps missing properties to false. Missing is NOT
        # evidence of disabled/visible: inspect the actual typed properties.
        $enabled = (Get-ComposerProperty $buttons[0] 30010)
        $offscreen = (Get-ComposerProperty $buttons[0] 30022)
        $disabled = $enabled -is [bool] -and -not $enabled -and $offscreen -is [bool] -and -not $offscreen
      }
      if ($disabled) {
        if ($stableSince -ne $null -and $stableEditor -ceq $editor -and ((Get-MonotonicMilliseconds) - $stableSince) -ge 150) {
          Assert-NativeVoiceComposer $state
          [void](Get-BoundComposerRoot $state)
          if ([string](Get-ComposerText $state.Composer)[1] -cne '' -or
              (Get-ElementRuntimeId $state.Composer) -cne $editor) { return $false }
          $enabledAgain = (Get-ComposerProperty $buttons[0] 30010)
          if ($enabledAgain -isnot [bool] -or $enabledAgain) { return $false }
          return $true
        }
        if ($stableSince -eq $null -or $stableEditor -cne $editor) { $stableSince = Get-MonotonicMilliseconds; $stableEditor = $editor }
      } else { $stableSince = $null }
    } catch { return $false }
    Start-Sleep -Milliseconds 40
  } while ((Get-MonotonicMilliseconds) -lt $deadline)
  return $false
}

function Invoke-SendButton($state, [bool]$allowQueue = $true, [bool]$strictButton = $false) {
  $nativeTaskClick = $strictButton -and $state.CurrentTask -and $state.Agent -eq 'codex'
  $composerRect = $state.Composer.Current.BoundingRectangle
  if (-not (Test-FiniteWindowRectangle $composerRect)) {
    throw 'Visible composer geometry is unavailable for submission'
  }
  # Reuse the bounded breadth-first surface walk. A live task transcript can
  # make an unbounded descendant Button query stall just like the old Edit
  # query did during startup; Enter remains the verified fallback.
  $all = if ((Test-CurrentElectronAgent $state.Agent)) {
    @([WorkBuddyUia]::FindBounded([IntPtr]$state.WindowHandle, $false, 1800, 3500))
  } else {
    @(Find-BoundedComposerElements $state.Root)
  }
  $buttons = @()
  for ($index = 0; $index -lt $all.Count; $index += 1) {
    $element = $all[$index]
    if ($element.Current.ControlType.Id -ne [System.Windows.Automation.ControlType]::Button.Id -or
        -not $element.Current.IsEnabled -or $element.Current.IsOffscreen) { continue }
    $labelScore = Get-ComposerSubmitLabelScore ([string]$element.Current.Name)
    if ($labelScore -lt 0) { continue }
    $rect = $element.Current.BoundingRectangle
    if (-not (Test-FiniteWindowRectangle $rect)) { continue }
    if ($rect.Bottom -lt ($composerRect.Top - 12) -or $rect.Top -gt ($composerRect.Bottom + 80)) { continue }
    if ($rect.Right -lt ($composerRect.Left - 12) -or $rect.Left -gt ($composerRect.Right + 120)) { continue }
    if (-not $allowQueue -and $labelScore -eq 1) {
      throw 'DESKTOP_TASK_TARGET_BUSY'
    }
    $invoke = $null
    if (-not $nativeTaskClick -and -not $element.TryGetCurrentPattern(
        [System.Windows.Automation.InvokePattern]::Pattern,
        [ref]$invoke
    )) { continue }
    $buttons += [pscustomobject]@{
      Element = $element
      Label = [string]$element.Current.Name
      Pattern = $invoke
      Score = [int]$labelScore
      Submission = if ($labelScore -eq 1) {
        if (@('steer', '引导') -contains (Normalize-Label ([string]$element.Current.Name)).ToLowerInvariant()) { 'steer' } else { 'queue' }
      } else { 'send' }
      Left = [double]$rect.Left
    }
  }
  $buttons = @($buttons | Sort-Object `
    @{ Expression = 'Score'; Ascending = $true }, `
    @{ Expression = 'Left'; Descending = $true })
  if ($buttons.Count -eq 1 -or
      ($buttons.Count -gt 1 -and $buttons[0].Score -lt $buttons[1].Score)) {
    if ($nativeTaskClick) {
      Invoke-CodexTaskSubmit $state $buttons[0]
    } else {
      if ($state.NativeVoiceAppendOnly) {
        # Enumeration can outlive a redraw. Do not adopt another editor while
        # holding a button from the old tree, or submit in a changed window.
        Assert-NativeVoiceOwnedDraft $state
        if (-not (Test-ComposerElementOwner $state.Root $buttons[0].Element) -or
            -not $buttons[0].Element.Current.IsEnabled -or $buttons[0].Element.Current.IsOffscreen) {
          throw 'DESKTOP_TASK_TARGET_CHANGED'
        }
        $state.NativeVoiceSubmitAttempted = $true
      }
      $buttons[0].Pattern.Invoke()
    }
    return $buttons[0].Submission
  }
  if ($buttons.Count -gt 1) {
    throw "Expected one enabled send button near the composer; found $($buttons.Count)"
  }

  if ($state.Agent -eq 'deepseek-harness' -or $strictButton -or -not $allowQueue) { throw 'DESKTOP_TASK_SEND_UNAVAILABLE' }

  Focus-Composer $state
  if ($state.NativeVoiceAppendOnly) {
    Assert-NativeVoiceOwnedDraft $state
    $state.NativeVoiceSubmitAttempted = $true
  }
  [CodexVoiceNative]::PressEnter()
  if ($state.NativeVoiceAppendOnly) { return 'enter' } # native action ACK, not a UIA value/delivery assertion
  Start-Sleep -Milliseconds 180
  $afterSubmit = Get-ComposerText $state.Composer
  if (-not [string]::IsNullOrEmpty([string]$afterSubmit[1])) {
    throw 'Visible composer did not accept Enter submission; the voice draft remains in the composer'
  }
}
$state = $null
$script:WorkBuddyAccessibilityRequests = @{}
$script:CurrentComposerAgent = ''
while ($null -ne ($line = [Console]::In.ReadLine())) {
  if ([string]::IsNullOrWhiteSpace($line)) { continue }
  $command = $null
  try {
    $command = $line | ConvertFrom-Json
    switch ([string]$command.kind) {
      'begin' {
        Write-ComposerProgress 'resolve_window'
        $agent = Normalize-Label ([string]$command.agent)
        if (-not $agent) { $agent = 'codex' }
        $script:CurrentComposerAgent = $agent
        [WorkBuddyUia]::ResetDiagnostics()
        $script:ComposerRejects = @{disabled=0;offscreen=0;focusable=0;owner=0;geometry=0;semantic=0;search=0;capability=0;writable=0;score=0}
        $sessionId = Normalize-Label ([string]$command.sessionId)
        $desktopSessionId = Normalize-Label ([string]$command.desktopSessionId)
        $title = Normalize-Label ([string]$command.sessionTitle)
        $purpose = Normalize-Label ([string]$command.purpose)
        $workspaceLabel = Normalize-Label ([string]$command.workspaceLabel)
        $currentVisible = $purpose -eq 'current_voice' -or $purpose -eq 'current_task'
        $script:NativeVoiceAppendOnly = $purpose -eq 'voice' -or $purpose -eq 'current_voice'
        $locatedCodexRoot = $null
        if (-not $currentVisible) {
          if ($agent -eq 'claude') {
            [void](Open-ClaudeSession `
              $sessionId $desktopSessionId $title)
          } elseif ($agent -eq 'codex') {
            $locatedCodexRoot = Open-CodexSessionById `
              $sessionId $title ([string]$command.deepLink) $workspaceLabel
          } else {
            throw "Unsupported visible composer agent: $agent"
          }
        }
        if ($purpose -eq 'locate') {
          Write-ComposerReply @{ ok = $true; phase = 'located'; mode = 'visible' }
          break
        }
        $target = if ($purpose -eq 'current_task') {
          Get-CurrentTaskTarget $null $agent
        } elseif ($currentVisible) {
          Get-CurrentVisibleTarget $agent $null
        } elseif ($agent -eq 'claude') {
          Get-ClaudeTarget $sessionId $desktopSessionId $title $null
        } elseif ($null -ne $locatedCodexRoot) {
          [pscustomobject]@{
            Root = $locatedCodexRoot
            Composer = Find-Composer $locatedCodexRoot $null
          }
        } else {
          Get-CodexTarget $title $null
        }
        $root = $target.Root
        $composer = $target.Composer
        $composerRuntimeId = Get-ElementRuntimeId $composer.Element
        if ($currentVisible -and -not $composerRuntimeId) {
          throw 'Current visible composer did not expose a stable runtime identity'
        }
        $state = @{
          Agent = $agent
          ProcessId = if ($agent -eq 'claude') { [int]$target.ProcessId } else { [int]$root.Current.ProcessId }
          WindowHandle = if ($agent -eq 'claude') { [int64]$target.WindowHandle } else { [int64]$root.Current.NativeWindowHandle }
          RootHandle = if ($agent -eq 'claude') { [int64]$target.RootHandle } else { [int64]$root.Current.NativeWindowHandle }
          Root = $root
          SessionId = $sessionId
          DesktopSessionId = $desktopSessionId
          SessionTitle = $title
          CurrentVisible = $currentVisible
          CurrentTask = $purpose -eq 'current_task'
          TaskIdentity = if ($currentVisible -and -not $script:NativeVoiceAppendOnly) { Get-TaskWindowIdentity $root $agent } else { '' }
          NativeUia = $composer.Element -is [WorkBuddyUiaElement]
          ComposerSemanticKey = [string]$composer.SemanticKey
          WriteAttempted = $false
          NativeVoiceAppendOnly = [bool]$script:NativeVoiceAppendOnly
          NativeVoiceAnchor = if ($script:NativeVoiceAppendOnly -and $currentVisible) {
            try { Get-NativeVoiceConversationAnchor $root $agent } catch { $null }
          } else { $null }
          ProcessStartTicks = if ($script:NativeVoiceAppendOnly) { (Get-Process -Id $(if ($agent -eq 'claude') { [int]$target.ProcessId } else { [int]$root.Current.ProcessId }) -ErrorAction Stop).StartTime.ToUniversalTime().Ticks } else { 0 }
          NativeVoiceAttempted = $false
          NativeVoiceWritten = $false
          NativeVoiceText = ''
          NativeVoiceOwnedValue = $null
          NativeVoiceOwnedHash = ''
          NativeVoiceOwnedRuntimeId = ''
          NativeVoiceSubmitAttempted = $false
          Composer = $composer.Element
          ComposerRuntimeId = $composerRuntimeId
          Pattern = $composer.Pattern
          BaseValue = [string]$composer.Value
          LastValue = [string]$composer.Value
          LastSessionCheck = Get-MonotonicMilliseconds
        }
        Focus-Composer $state
        Write-ComposerReply @{ ok = $true; phase = 'ready'; mode = 'visible' }
      }
      'update' {
        if ($null -eq $state) { throw 'Visible composer is not ready' }
        if ($state.NativeVoiceAppendOnly) { Append-ComposerVoiceText $state ([string]$command.text) }
        else { Set-ComposerText $state ([string]$command.text) $false }
        Write-ComposerReply @{ ok = $true; phase = 'updated'; revision = $command.revision; mode = 'visible'; inputEvidence = if ($state.NativeVoiceAppendOnly) { 'native-append-owned-readback' } else { 'editor-readback' } }
      }
      'confirm' {
        if ($null -eq $state) { throw 'Visible composer is not ready' }
        if ($state.NativeVoiceAppendOnly -and $state.NativeVoiceSubmitAttempted) { throw 'COMPOSER_SUBMIT_UNCONFIRMED: Submit was already attempted; no replay' }
        Assert-ComposerHasText $state
        $null = Invoke-SendButton $state
        if ($state.NativeVoiceAppendOnly -and -not (Wait-NativeVoiceSubmitClear $state)) {
          throw 'COMPOSER_SUBMIT_UNCONFIRMED: Native submit was attempted but application readback was not confirmed; no replay; draft preserved'
        }
        Write-ComposerReply @{ ok = $true; phase = 'submitted'; revision = $command.revision; mode = 'visible' }
        break
      }
      'inspect_task_draft' {
        $snapshot = Get-TaskDraftSnapshot $state
        Write-ComposerReply @{ ok = $true; phase = 'inspected'; snapshot = $snapshot }
        break
      }
      'dispatch' {
        if ($null -eq $state -or @('codex','workbuddy','deepseek-harness') -notcontains $state.Agent) { throw 'Desktop composer is not ready' }
        Assert-TargetCurrent $state $true
        if ($null -ne $command.draftSnapshot) {
          Assert-TaskDraftSnapshot $state $command.draftSnapshot
          # The coordinator supplies the complete current draft plus the new
          # request. Do not prepend BaseValue a second time in Join-VoiceDraft.
          $state.BaseValue = ''
        } elseif (-not [string]::IsNullOrEmpty($state.BaseValue) -or
            -not [string]::IsNullOrEmpty([string](Get-ComposerText $state.Composer)[1])) {
          throw 'DESKTOP_TASK_DRAFT_NOT_EMPTY'
        }
        Set-ComposerText $state ([string]$command.text) $true ($null -ne $command.draftSnapshot)
        Assert-TargetCurrent $state $true
        if ([string](Get-ComposerText $state.Composer)[1] -cne [string]$command.text) {
          throw 'DESKTOP_TASK_READBACK_CHANGED'
        }
        Assert-ComposerHasText $state
        $submission = Invoke-SendButton $state $true $true
        # Like macOS, report the native button attempt, not task acceptance.
        # Queue/Steer can replace the editor immediately. Do not lose the queue
        # mode or repeat the click because of a post-submit UI redraw; the
        # coordinator requires a fresh matching user receipt from the event bus.
        Write-ComposerReply @{ ok = $true; phase = 'submitted'; mode = 'visible'; submission = $submission }
        break
      }
      'release' {
        if ([CodexVoiceNative]::InputOutcomeUnknown) {
          throw 'COMPOSER_WRITE_ACK_UNKNOWN: Native input remains unconfirmed; writer release was not acknowledged.'
        }
        Write-ComposerReply @{ ok = $true; phase = 'released'; mode = 'visible' }
        break
      }
      'cancel' {
        if ($null -ne $state) {
          Clear-ComposerVoiceText $state
        }
        Write-ComposerReply @{ ok = $true; phase = 'cancelled'; mode = 'visible' }
        break
      }
      default { throw "Unknown composer command: $($command.kind)" }
    }
  } catch {
    $errorMessage = $_.Exception.Message
    # SendInput uncertainty applies to EVERY native input path, not just text.
    # The Rust worker treats this prefix as fatal, drains no queued release,
    # and preserve_draft caches Err, retaining the parent's old-writer barrier.
    if ($null -ne $state -and $state.NativeVoiceAppendOnly -and $state.NativeVoiceSubmitAttempted -and [string]$command.kind -eq 'confirm' -and
        -not [CodexVoiceNative]::LastInputRejectedBeforeSend) {
      $errorMessage = 'COMPOSER_SUBMIT_UNCONFIRMED: Native submit result is unknown; no replay and the current draft is preserved.'
    } elseif ([CodexVoiceNative]::InputOutcomeUnknown) {
      $errorMessage = 'COMPOSER_WRITE_ACK_UNKNOWN: Native input may be partial; no injection replay or confirmed writer release.'
    }
    Write-ComposerReply @{
      ok = $false
      phase = if ($null -ne $command) { [string]$command.kind } else { 'error' }
      mode = 'fallback'
      error = $errorMessage
    }
    $state = $null
  }
}
"#;

        let script_path = std::env::temp_dir().join(format!(
            "pet-codex-visible-composer-{}.ps1",
            uuid::Uuid::new_v4()
        ));
        let mut script_bytes = Vec::with_capacity(SCRIPT.len() + 3);
        script_bytes.extend_from_slice(&[0xEF, 0xBB, 0xBF]);
        script_bytes.extend_from_slice(SCRIPT.as_bytes());
        fs::write(&script_path, script_bytes)
            .map_err(|error| format!("failed to stage visible Agent composer script: {error}"))?;

        let composer_job = match WindowsComposerJob::new(current_visible) {
            Ok(job) => job,
            Err(error) => {
                let _ = fs::remove_file(&script_path);
                return Err(error.into());
            }
        };

        let mut child = match hidden_powershell()
            .arg("-Mta")
            .arg("-File")
            .arg(&script_path)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
        {
            Ok(child) => child,
            Err(error) => {
                let _ = fs::remove_file(&script_path);
                return Err(format!(
                    "failed to start visible Agent composer bridge: {error}"
                ).into());
            }
        };
        let assignment = composer_job.assign(&child);
        let pipes = child.stdin.take().zip(child.stdout.take());
        let process = Arc::new(WindowsComposerProcess { child: Mutex::new(child), job: composer_job, strict: current_visible });
        if let Err(error)=assignment {
            // Gate was not opened: retain actual child owner for administrative reap.
            return Ok(Self::retained_windows_failure(process,error));
        }
        let Some((mut stdin,stdout))=pipes else {
            return Ok(Self::retained_windows_failure(process,"Visible helper pipes unavailable".into()));
        };
        if stdin.write_all(b"HACHIMODOCK_CONTAINED_HELPER_V1\n").and_then(|_|stdin.flush()).is_err() {
            return Ok(Self::retained_windows_failure(process,"Visible helper containment startup failed".into()));
        }

        let callback: Arc<dyn Fn(CodexComposerEvent) + Send + Sync> = Arc::new(callback);
        let failed = Arc::new(AtomicBool::new(false));
        let closed = Arc::new(AtomicBool::new(false));
        let (sender, receiver) = mpsc::channel::<ComposerCommand>();
        let worker_callback = callback.clone();
        let worker_failed = failed.clone();
        let worker_closed = closed.clone();
        let worker_process = process.clone();
        let startup_stage = Arc::new(Mutex::new("启动 Windows 输入桥"));
        let worker_startup_stage = startup_stage.clone();
        let voice_append_only = matches!(purpose, "voice" | "current_voice");
        let worker = match thread::Builder::new()
            .name("pet-codex-visible-composer".to_string())
            .spawn(move || {

                let mut stdout = BufReader::new(stdout);
                let mut pending_command = None;
                while let Some(mut command) =
                    receive_latest_composer_command(&receiver, &mut pending_command)
                {
                    if voice_append_only && composer_command_kind(&command)=="update" && command.response.is_none() {
                        continue; // nonfinal recognition: never claim an editor write
                    }
                    if let Some(started) = command.started.take() {
                        let _ = started.send(());
                    }
                    let serialized = match serde_json::to_string(&command.payload) {
                        Ok(serialized) => serialized,
                        Err(error) => {
                            let message =
                                format!("failed to encode visible Agent composer command: {error}");
                            worker_failed.store(true, Ordering::SeqCst);
                            if let Some(response) = command.response {
                                let _ = response.send(Err(message.clone()));
                            }
                            worker_callback(CodexComposerEvent {
                                phase: "error".to_string(),
                                ok: false,
                                error: message,
                            });
                            continue;
                        }
                    };
                    let result = (|| -> Result<Value, String> {
                        stdin
                            .write_all(serialized.as_bytes())
                            .and_then(|_| stdin.write_all(b"\n"))
                            .and_then(|_| stdin.flush())
                            .map_err(|error| {
                                format!("failed to write visible Agent composer command: {error}")
                            })?;
                        read_windows_composer_response(&mut stdout, |stage| {
                            if let Ok(mut current) = worker_startup_stage.lock() {
                                *current = stage;
                            }
                        })
                    })();

                    let phase = command
                        .payload
                        .get("kind")
                        .and_then(Value::as_str)
                        .unwrap_or("error")
                        .to_string();
                    let fatal_begin = result.is_err() && composer_command_failure_is_fatal(&command);
                    let write_ack_unknown = result.as_ref().err()
                        .is_some_and(|error| error.starts_with("COMPOSER_WRITE_ACK_UNKNOWN:"));
                    match &result {
                        Ok(value) => worker_callback(CodexComposerEvent {
                            phase: value
                                .get("phase")
                                .and_then(Value::as_str)
                                .unwrap_or(&phase)
                                .to_string(),
                            ok: true,
                            error: String::new(),
                        }),
                        Err(error) => {
                            if composer_command_failure_is_fatal(&command) || write_ack_unknown {
                                worker_failed.store(true, Ordering::SeqCst);
                            }
                            worker_callback(CodexComposerEvent {
                                phase,
                                ok: false,
                                error: error.clone(),
                            });
                        }
                    }
                    if let Some(response) = command.response {
                        let _ = response.send(result);
                    }
                    // A partial/unknown Windows write terminates this helper;
                    // queued updates cannot re-paste the same utterance. The
                    // caller retains its transcript and starts no replay here.
                    if write_ack_unknown || fatal_begin { break; }
                    if command.payload.get("kind").and_then(Value::as_str) == Some("release") { break; }
                }
                worker_closed.store(true, Ordering::SeqCst);
                use crate::device_voice_lifecycle::OwnedProcessTree;
                let _=worker_process.terminate();
                let _ = fs::remove_file(&script_path);
            }) {
                Ok(worker)=>worker,
                Err(error)=>return Ok(Self::retained_windows_failure(process,format!("failed to start visible composer worker: {error}"))),
            };

        let bridge = Self {
            sender,
            failed,
            closed,
            release_result: std::sync::Mutex::new(None),
            #[cfg(any(windows, target_os = "macos"))]
            // Historical bound-navigation helpers may launch desktop applications
            // outside their job. Preserve their existing release-ACK contract;
            // never manufacture a descendant-termination proof for that mode.
            termination: if current_visible { Some(crate::device_voice_lifecycle::WriterTermination::owned_process(Some(worker),process)) } else { drop(worker); None },
        };
        let desktop_session_id = if agent == "claude" { deep_link } else { "" };
        let navigation_deep_link = if agent == "codex" { deep_link } else { "" };
        let (ready_sender, ready_receiver) = mpsc::channel();
        let begin_send = bridge
            .sender
            .send(ComposerCommand {
                payload: json!({
                    "kind": "begin",
                    "agent": agent,
                    "sessionId": session_id,
                    "desktopSessionId": desktop_session_id,
                    "deepLink": navigation_deep_link,
                    "sessionTitle": session_title,
                    "workspaceLabel": workspace_label_from_cwd(session_cwd),
                    "purpose": purpose,
                }),
                started: None,
                response: Some(ready_sender),
            })
            ;
        if begin_send.is_err() {
            bridge.failed.store(true,Ordering::SeqCst);
            return Ok(RetainedComposerStartup { bridge, readiness: Err("Visible Agent composer bridge closed during startup".into()) });
        }
        let readiness=match ready_receiver.recv_timeout(std::time::Duration::from_secs(
            CODEX_COMPOSER_STARTUP_TIMEOUT_SECS,
        )) {
            Ok(Ok(_))=>Ok(()),
            Ok(Err(error))=>Err(error),
            Err(mpsc::RecvTimeoutError::Timeout)=>{
                let stage=startup_stage.lock().map(|stage|*stage).unwrap_or("读取输入框状态");
                Err(composer_startup_timeout_message(agent,stage))
            },
            Err(mpsc::RecvTimeoutError::Disconnected)=>Err("Visible Agent composer closed during startup".into()),
        };
        if readiness.is_err() {
            bridge.failed.store(true,Ordering::SeqCst);
            // Readiness/business unknown is preserved, but retained ownership
            // permits independent actual helper/job teardown on cleanup.
        }
        Ok(RetainedComposerStartup { bridge, readiness })
    }

    fn send(&self, payload: Value) -> Result<(), String> {
        use std::sync::atomic::Ordering;
        if self.closed.load(Ordering::SeqCst) {
            return Err("Visible Agent composer bridge is closed".to_string());
        }
        self.sender
            .send(ComposerCommand {
                payload,
                started: None,
                response: None,
            })
            .map_err(|_| "Visible Agent composer bridge is closed".to_string())
    }

    pub fn update(&self, revision: u64, text: &str) -> Result<(), String> {
        use std::sync::atomic::Ordering;
        if self.failed.load(Ordering::SeqCst) {
            return Err("Visible Agent composer is unavailable".to_string());
        }
        self.send(json!({ "kind": "update", "revision": revision, "text": text }))
    }

    pub fn confirm(&self, revision: u64, text: &str) -> CodexComposerSubmission {
        use std::sync::atomic::Ordering;
        let (started_sender, started_receiver) = std::sync::mpsc::channel();
        let (completed_sender, completed_receiver) = std::sync::mpsc::channel();
        if self.failed.load(Ordering::SeqCst) || self.closed.load(Ordering::SeqCst) {
            let _ = started_sender.send(());
            let _ = completed_sender.send(Err("Visible Agent composer is unavailable".to_string()));
            return CodexComposerSubmission {
                started: started_receiver,
                completed: completed_receiver,
            };
        }
        if self
            .sender
            .send(ComposerCommand {
                payload: json!({ "kind": "confirm", "revision": revision, "text": text }),
                started: Some(started_sender.clone()),
                response: Some(completed_sender.clone()),
            })
            .is_err()
        {
            let _ = started_sender.send(());
            let _ =
                completed_sender.send(Err("Visible Agent composer bridge is closed".to_string()));
        }
        CodexComposerSubmission {
            started: started_receiver,
            completed: completed_receiver,
        }
    }

    pub fn cancel(&self) {
        let _ = self.send(json!({ "kind": "cancel" }));
    }
}

#[cfg(windows)]
impl Drop for CodexComposerBridge {
    fn drop(&mut self) {
        // Dropping an owned helper is administrative teardown, never a draft
        // rollback/cancel replay after an unknown business result.
        self.closed.store(true,std::sync::atomic::Ordering::SeqCst);
        if let Some(worker)=self.termination.as_ref() { let _=worker.terminate_process(); }
    }
}

/// Own the already spawned native worker even when its read-only lookup failed.
/// Readiness is not a release ACK; callers must store `bridge` in the existing
/// writer barrier, close write admission on error, then obtain a real release.
#[cfg(any(windows, target_os = "macos"))]
pub struct RetainedComposerStartup {
    pub bridge: CodexComposerBridge,
    pub readiness: Result<(), String>,
}

#[cfg(target_os = "macos")]
impl CodexComposerBridge {
    pub fn is_agent_frontmost(agent_id: &str) -> bool {
        let agent = match agent_id.trim() {
            "codex" => macos::MacosAgent::Codex,
            "claude-code" => macos::MacosAgent::Claude,
            "workbuddy" => macos::MacosAgent::WorkBuddy,
            "deepseek-harness" => macos::MacosAgent::DeepSeekHarness,
            _ => return false,
        };
        macos::agent_is_frontmost(agent)
    }

    pub fn accessibility_permission_granted() -> bool {
        macos::accessibility_permission_granted()
    }

    pub fn request_accessibility_permission() -> bool {
        macos::request_accessibility_permission()
    }

    pub fn start_current(
        agent_id: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        let agent = match agent_id.trim() {
            "codex" => macos::MacosAgent::Codex,
            "claude-code" => macos::MacosAgent::Claude,
            "workbuddy" => macos::MacosAgent::WorkBuddy,
            "deepseek-harness" => macos::MacosAgent::DeepSeekHarness,
            _ => {
                return Err(
                    "Current visible composer requires ChatGPT（Codex）, Claude, WorkBuddy or DeepSeek Harness".to_string(),
                )
            }
        };
        Self::start_with_purpose(agent, "", "", "", "", "current_voice", callback)
    }

    pub fn start_current_retained(
        agent_id: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<RetainedComposerStartup, ComposerNoWriterError> {
        let agent = match agent_id.trim() {
            "codex" => macos::MacosAgent::Codex,
            "claude-code" => macos::MacosAgent::Claude,
            "workbuddy" => macos::MacosAgent::WorkBuddy,
            "deepseek-harness" => macos::MacosAgent::DeepSeekHarness,
            _ => return Err("Unsupported current visible composer".into()),
        };
        Self::start_with_purpose_retained(agent, "", "", "", "", "current_voice", callback)
    }

    #[cfg(debug_assertions)]
    pub fn debug_dump_accessibility_tree() -> Result<Vec<String>, String> {
        macos::debug_dump_codex_tree()
    }

    #[cfg(debug_assertions)]
    pub fn debug_probe_visible_composer(
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
    ) -> Result<(), String> {
        let mut state = macos::begin_voice(
            macos::MacosAgent::Codex,
            "",
            session_id,
            session_title,
            &workspace_label_from_cwd(session_cwd),
        )?;
        let result = macos::debug_probe_final_focus(&state);
        macos::cancel_voice(&mut state);
        result
    }

    pub fn start(
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        Self::start_with_purpose(
            macos::MacosAgent::Codex,
            "",
            session_id,
            session_title,
            session_cwd,
            "voice",
            callback,
        )
    }

    pub fn start_claude(
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        let deep_link = claude_session_deep_link(session_id)?;
        Self::start_with_purpose(
            macos::MacosAgent::Claude,
            &deep_link,
            session_id,
            session_title,
            session_cwd,
            "voice",
            callback,
        )
    }

    pub fn focus_session(
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
    ) -> Result<(), String> {
        static NAVIGATION_LOCK: std::sync::OnceLock<std::sync::Mutex<()>> =
            std::sync::OnceLock::new();
        let _guard = NAVIGATION_LOCK
            .get_or_init(|| std::sync::Mutex::new(()))
            .lock()
            .map_err(|error| format!("ChatGPT（Codex） session navigation lock failed: {error}"))?;
        let bridge = Self::start_with_purpose(
            macos::MacosAgent::Codex,
            "",
            session_id,
            session_title,
            session_cwd,
            "locate",
            |_| {},
        )?;
        drop(bridge);
        Ok(())
    }

    pub fn focus_claude_session(
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
    ) -> Result<(), String> {
        static NAVIGATION_LOCK: std::sync::OnceLock<std::sync::Mutex<()>> =
            std::sync::OnceLock::new();
        let _guard = NAVIGATION_LOCK
            .get_or_init(|| std::sync::Mutex::new(()))
            .lock()
            .map_err(|error| format!("Claude session navigation lock failed: {error}"))?;
        let deep_link = claude_session_deep_link(session_id)?;
        let bridge = Self::start_with_purpose(
            macos::MacosAgent::Claude,
            &deep_link,
            session_id,
            session_title,
            session_cwd,
            "locate",
            |_| {},
        )?;
        drop(bridge);
        Ok(())
    }

    fn start_with_purpose(
        agent: macos::MacosAgent,
        deep_link: &str,
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
        purpose: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        let startup = Self::start_with_purpose_retained(
            agent, deep_link, session_id, session_title, session_cwd, purpose, callback).map_err(ComposerNoWriterError::into_message)?;
        startup.readiness?;
        Ok(startup.bridge)
    }

    fn start_with_purpose_retained(
        agent: macos::MacosAgent,
        deep_link: &str,
        session_id: &str,
        session_title: &str,
        session_cwd: &str,
        purpose: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<RetainedComposerStartup, ComposerNoWriterError> {
        use std::sync::atomic::{AtomicBool, Ordering};
        use std::sync::{mpsc, Arc};
        use std::thread;

        let session_id = session_id.trim();
        let session_title = session_title.trim();
        let current_visible = matches!(purpose, "current_voice" | "current_task");
        if agent == macos::MacosAgent::DeepSeekHarness && !current_visible {
            return Err("DeepSeek Harness 仅支持当前可见输入框".into());
        }
        if !current_visible && session_id.is_empty() && session_title.is_empty() {
            return Err("Visible composer requires a bound task ID or title".into());
        }
        if !matches!(purpose, "voice" | "locate" | "current_voice" | "current_task") {
            return Err("Visible composer purpose is invalid".into());
        }

        let deep_link = deep_link.to_string();
        let session_id = session_id.to_string();
        let session_title = session_title.to_string();
        let workspace_label = workspace_label_from_cwd(session_cwd);
        let purpose = purpose.to_string();
        let callback: Arc<dyn Fn(CodexComposerEvent) + Send + Sync> = Arc::new(callback);
        let failed = Arc::new(AtomicBool::new(false));
        let closed = Arc::new(AtomicBool::new(false));
        let (sender, receiver) = mpsc::channel::<ComposerCommand>();
        let worker_callback = callback.clone();
        let worker_failed = failed.clone();
        let worker_closed = closed.clone();
        let worker = thread::Builder::new()
            .name("pet-visible-agent-composer".to_string())
            .spawn(move || {
                let mut pending_command = None;
                let mut state: Option<macos::MacosComposerState> = None;
                while let Some(mut command) =
                    receive_latest_composer_command(&receiver, &mut pending_command)
                {
                    if let Some(started) = command.started.take() {
                        let _ = started.send(());
                    }
                    let kind = composer_command_kind(&command).to_string();
                    let text = command
                        .payload
                        .get("text")
                        .and_then(Value::as_str)
                        .unwrap_or("")
                        .to_string();
                    let revision = command
                        .payload
                        .get("revision")
                        .and_then(Value::as_u64)
                        .unwrap_or(0);
                    let result = match kind.as_str() {
                        "begin" if purpose == "locate" => macos::focus_session(
                            agent,
                            &deep_link,
                            &session_id,
                            &session_title,
                            &workspace_label,
                        )
                        .map(|_| json!({ "ok": true, "phase": "located", "mode": "visible" })),
                        "begin" if purpose == "current_task" => macos::begin_current_task_for(agent)
                            .map(|new_state| {
                                state = Some(new_state);
                                json!({ "ok": true, "phase": "ready", "mode": "visible" })
                            }),
                        "begin" if purpose == "current_voice" => macos::begin_current_voice(agent)
                            .map(|new_state| {
                                state = Some(new_state);
                                json!({ "ok": true, "phase": "ready", "mode": "visible" })
                            }),
                        "begin" => macos::begin_voice(
                            agent,
                            &deep_link,
                            &session_id,
                            &session_title,
                            &workspace_label,
                        )
                        .map(|new_state| {
                            state = Some(new_state);
                            json!({ "ok": true, "phase": "ready", "mode": "visible" })
                        }),
                        "update" => state
                            .as_mut()
                            .ok_or_else(|| "Visible composer is not ready".to_string())
                            .and_then(|state| macos::update_voice(state, &text))
                            .map(|composer_value| {
                                json!({
                                    "ok": true,
                                    "phase": "updated",
                                    "revision": revision,
                                    "mode": "visible",
                                    "composerValue": composer_value,
                                })
                            }),
                        "inspect_task_draft" => state
                            .as_mut()
                            .ok_or_else(|| "Codex composer is not ready".to_string())
                            .and_then(macos::inspect_task_draft)
                            .map(|snapshot| json!({"ok":true,"phase":"inspected","snapshot":snapshot})),
                        "dispatch" => state
                            .as_mut()
                            .ok_or_else(|| "Codex composer is not ready".to_string())
                            .and_then(|state| match command.payload.get("draftSnapshot") {
                                Some(snapshot) => macos::dispatch_task_from_draft(state, &text, snapshot),
                                None => macos::dispatch_voice_task(state, &text),
                            })
                            .map(|submission| json!({"ok":true,"phase":"submitted","mode":"visible","submission":submission})),
                        "confirm" => state
                            .as_mut()
                            .ok_or_else(|| "Visible composer is not ready".to_string())
                            .and_then(|state| macos::confirm_voice(state, &text))
                            .map(|composer_value| {
                                json!({
                                    "ok": true,
                                    "phase": "submitted",
                                    "revision": revision,
                                    "mode": "visible",
                                    "composerValue": composer_value,
                                })
                            }),
                        "release" => Ok(json!({ "ok": true, "phase": "released", "mode": "visible" })),
                        "cancel" => {
                            if let Some(state) = state.as_mut() {
                                macos::cancel_voice(state);
                            }
                            Ok(json!({ "ok": true, "phase": "cancelled", "mode": "visible" }))
                        }
                        _ => Err(format!("Unknown composer command: {kind}")),
                    };

                    match &result {
                        Ok(value) => worker_callback(CodexComposerEvent {
                            phase: value
                                .get("phase")
                                .and_then(Value::as_str)
                                .unwrap_or(&kind)
                                .to_string(),
                            ok: true,
                            error: String::new(),
                        }),
                        Err(error) => {
                            if composer_command_failure_is_fatal(&command) {
                                worker_failed.store(true, Ordering::SeqCst);
                            }
                            worker_callback(CodexComposerEvent {
                                phase: kind.clone(),
                                ok: false,
                                error: error.clone(),
                            });
                        }
                    }
                    if let Some(response) = command.response {
                        let _ = response.send(result);
                    }
                    if matches!(kind.as_str(), "confirm" | "cancel" | "release")
                        || (kind == "begin" && purpose == "locate")
                    {
                        break;
                    }
                }
                worker_closed.store(true, Ordering::SeqCst);
            })
            .map_err(|error| format!("failed to start visible composer worker: {error}"))?;

        let bridge = Self {
            sender,
            failed,
            closed,
            release_result: std::sync::Mutex::new(None),
            #[cfg(any(windows, target_os = "macos"))]
            termination: Some(crate::device_voice_lifecycle::WriterTermination::new(worker)),
        };
        let (ready_sender, ready_receiver) = mpsc::channel();
        let begin_send = bridge
            .sender
            .send(ComposerCommand {
                payload: json!({ "kind": "begin" }),
                started: None,
                response: Some(ready_sender),
            })
            ;
        if begin_send.is_err() {
            bridge.failed.store(true, Ordering::SeqCst);
            return Ok(RetainedComposerStartup { bridge, readiness: Err("Visible Agent composer bridge closed during startup".into()) });
        }
        let readiness = match ready_receiver.recv_timeout(std::time::Duration::from_secs(
            CODEX_COMPOSER_STARTUP_TIMEOUT_SECS,
        )) {
            Ok(Ok(_)) => Ok(()),
            Ok(Err(error)) => Err(error),
            Err(mpsc::RecvTimeoutError::Timeout) => {
                Err("Visible Agent composer startup timed out".to_string())
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                Err("Visible Agent composer closed during startup".to_string())
            }
        };
        // Close ordinary writes BEFORE the retained Bridge becomes observable.
        // A finalizer may already have passed the context admission check.
        // Failed is sticky; late begin success cannot reopen update/confirm.
        // preserve_draft still bypasses this gate to obtain the real release ACK.
        if readiness.is_err() {
            bridge.failed.store(true, Ordering::SeqCst);
        }
        Ok(RetainedComposerStartup { bridge, readiness })
    }

    fn send(&self, payload: Value) -> Result<(), String> {
        use std::sync::atomic::Ordering;
        if self.closed.load(Ordering::SeqCst) {
            return Err("Visible Agent composer bridge is closed".to_string());
        }
        self.sender
            .send(ComposerCommand {
                payload,
                started: None,
                response: None,
            })
            .map_err(|_| "Visible Agent composer bridge is closed".to_string())
    }

    pub fn update(&self, revision: u64, text: &str) -> Result<(), String> {
        use std::sync::atomic::Ordering;
        if self.failed.load(Ordering::SeqCst) {
            return Err("Visible Agent composer is unavailable".to_string());
        }
        self.send(json!({ "kind": "update", "revision": revision, "text": text }))
    }

    pub fn confirm(&self, revision: u64, text: &str) -> CodexComposerSubmission {
        use std::sync::atomic::Ordering;
        let (started_sender, started_receiver) = std::sync::mpsc::channel();
        let (completed_sender, completed_receiver) = std::sync::mpsc::channel();
        if self.failed.load(Ordering::SeqCst) || self.closed.load(Ordering::SeqCst) {
            let _ = started_sender.send(());
            let _ = completed_sender.send(Err("Visible Agent composer is unavailable".to_string()));
            return CodexComposerSubmission {
                started: started_receiver,
                completed: completed_receiver,
            };
        }
        if self
            .sender
            .send(ComposerCommand {
                payload: json!({ "kind": "confirm", "revision": revision, "text": text }),
                started: Some(started_sender.clone()),
                response: Some(completed_sender.clone()),
            })
            .is_err()
        {
            let _ = started_sender.send(());
            let _ =
                completed_sender.send(Err("Visible Agent composer bridge is closed".to_string()));
        }
        CodexComposerSubmission {
            started: started_receiver,
            completed: completed_receiver,
        }
    }

    pub fn cancel(&self) {
        let _ = self.send(json!({ "kind": "cancel" }));
    }
}

#[cfg(target_os = "macos")]
impl Drop for CodexComposerBridge {
    fn drop(&mut self) {
        self.cancel();
    }
}

#[cfg(not(any(windows, target_os = "macos")))]
pub struct CodexComposerBridge;

#[cfg(not(any(windows, target_os = "macos")))]
impl CodexComposerBridge {
    pub fn start_current_task() -> Result<Self, String> {
        Err("当前系统不支持桌面任务派发".into())
    }
    pub fn start_current_task_for(_agent_id: &str) -> Result<Self, String> {
        Err("当前系统不支持桌面任务派发".into())
    }
    pub fn is_agent_frontmost(_agent_id: &str) -> bool {
        false
    }

    pub fn dispatch_task(&self, _text: &str) -> Result<String, String> {
        Err("当前系统不支持桌面任务派发".into())
    }

    pub fn inspect_task_draft(&self) -> Result<Value, String> {
        Err("当前系统不支持桌面任务派发".into())
    }

    pub fn dispatch_task_from_draft(&self, _text: &str, _snapshot: &Value) -> Result<String, String> {
        Err("当前系统不支持桌面任务派发".into())
    }

    pub fn preserve_draft(&self) -> Result<(), String> { Ok(()) }

    pub fn update_confirmed(&self, _revision: u64, _text: &str) -> Result<(), String> {
        Err("当前系统不支持前台语音输入".into())
    }

    pub fn start_current(
        _agent_id: &str,
        _callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        Err("Current visible composer is currently available on Windows and macOS only".to_string())
    }

    pub fn start(
        _session_id: &str,
        _session_title: &str,
        _session_cwd: &str,
        _callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        Err("Visible Agent composer is currently available on Windows and macOS only".to_string())
    }

    pub fn start_claude(
        _session_id: &str,
        _session_title: &str,
        _session_cwd: &str,
        _callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        Err(
            "Claude Desktop visible composer is currently available on Windows and macOS only"
                .to_string(),
        )
    }

    pub fn update(&self, _revision: u64, _text: &str) -> Result<(), String> {
        Err("Visible Agent composer is currently available on Windows and macOS only".to_string())
    }

    pub fn confirm(&self, _revision: u64, _text: &str) -> CodexComposerSubmission {
        let (started_sender, started_receiver) = std::sync::mpsc::channel();
        let (completed_sender, completed_receiver) = std::sync::mpsc::channel();
        let _ = started_sender.send(());
        let _ = completed_sender.send(Err(
            "Visible Agent composer is currently available on Windows and macOS only".to_string(),
        ));
        CodexComposerSubmission {
            started: started_receiver,
            completed: completed_receiver,
        }
    }

    pub fn cancel(&self) {}

    pub fn focus_session(
        _session_id: &str,
        _session_title: &str,
        _session_cwd: &str,
    ) -> Result<(), String> {
        Err(
            "ChatGPT（Codex） session navigation is currently available on Windows and macOS only"
                .to_string(),
        )
    }

    pub fn focus_claude_session(
        _session_id: &str,
        _session_title: &str,
        _session_cwd: &str,
    ) -> Result<(), String> {
        Err(
            "Claude Desktop session navigation is currently available on Windows and macOS only"
                .to_string(),
        )
    }
}

fn workspace_label_from_cwd(cwd: &str) -> String {
    cwd.trim_end_matches(['/', '\\'])
        .rsplit(['/', '\\'])
        .next()
        .unwrap_or_default()
        .trim()
        .to_string()
}

#[cfg(all(test, any(windows, target_os = "macos")))]
mod tests {
    use super::*;

    #[test]
    fn task_submission_accepts_native_enter_without_claiming_delivery() {
        for mode in ["send", "queue", "steer", "enter"] {
            assert_eq!(task_submission_mode(&json!({"submission":mode})).unwrap(), mode);
        }
        for reply in [json!({}), json!({"submission":""}), json!({"submission":"unknown"}), json!({"submission":true})] {
            assert!(task_submission_mode(&reply).is_err());
        }
    }

    #[test]
    fn windows_startup_timeout_names_the_selected_agent_and_stage() {
        let message = composer_startup_timeout_message("workbuddy", "读取当前聚焦输入框");
        assert!(message.starts_with("WorkBuddy "));
        assert!(!message.contains("Codex"));
        assert!(message.contains("读取当前聚焦输入框"));
        assert!(composer_startup_timeout_message("claude", "test").starts_with("Claude "));
        assert!(composer_startup_timeout_message("codex", "test").starts_with("ChatGPT（Codex）"));
        assert!(composer_startup_timeout_message("unknown", "test").starts_with("Agent "));
    }

    #[test]
    fn windows_progress_does_not_acknowledge_startup_or_consume_next_reply() {
        let mut reader = std::io::Cursor::new(concat!(
            "{\"phase\":\"progress\",\"stage\":\"compile_native\"}\n",
            "{\"phase\":\"progress\",\"stage\":\"enable_accessibility\"}\n",
            "{\"phase\":\"progress\",\"stage\":\"focused\"}\n",
            "{\"ok\":true,\"phase\":\"ready\"}\n",
            "{\"ok\":true,\"phase\":\"updated\",\"revision\":1}\n",
        ));
        let mut stages = Vec::new();
        let ready = read_windows_composer_response(&mut reader, |stage| stages.push(stage)).unwrap();
        assert_eq!(ready["phase"], "ready");
        assert_eq!(stages, ["初始化 Windows 输入桥", "请求 WorkBuddy 辅助功能界面树", "读取当前聚焦输入框"]);
        let updated = read_windows_composer_response(&mut reader, |_| {}).unwrap();
        assert_eq!(updated["revision"], 1);
    }

    #[test]
    fn windows_all_emitted_progress_stages_match_parser_and_replay_to_real_ack() {
        use std::collections::BTreeSet;
        let source = include_str!("codex_composer.rs");
        let script = source.split("        const SCRIPT: &str = r#\"").nth(1)
            .expect("embedded Windows helper").split("\"#;").next().unwrap();
        let emitted: Vec<&str> = script.lines().filter_map(|line| {
            let argument = line.trim().strip_prefix("Write-ComposerProgress ")?;
            assert!(argument.starts_with('\'') && argument.ends_with('\''),
                "progress must remain a literal stage, never dynamic data");
            let stage = &argument[1..argument.len()-1];
            assert!(stage.bytes().all(|b| b.is_ascii_lowercase() || b == b'_'));
            Some(stage)
        }).collect();
        assert!(!emitted.is_empty());
        let parser = source.split("fn read_windows_composer_response(").nth(1).unwrap()
            .split("\n#[cfg(").next().unwrap();
        let accepted: BTreeSet<&str> = parser.lines().filter_map(|line|
            line.trim().strip_prefix("Some(\"").and_then(|tail| tail.split_once("\") =>").map(|v| v.0))
        ).collect();
        assert_eq!(emitted.iter().copied().collect::<BTreeSet<_>>(), accepted);
        let mut wire = String::new();
        for stage in &emitted {
            wire.push_str(&json!({"phase":"progress", "stage":stage}).to_string());
            wire.push('\n');
        }
        wire.push_str("{\"ok\":true,\"phase\":\"ready\"}\n");
        wire.push_str("{\"ok\":true,\"phase\":\"updated\",\"revision\":7}\n");
        let mut reader = std::io::Cursor::new(wire);
        let mut callbacks = Vec::new();
        let ready = read_windows_composer_response(&mut reader, |v| callbacks.push(v)).unwrap();
        assert_eq!(ready["phase"], "ready");
        assert_eq!(callbacks.len(), emitted.len());
        assert!(callbacks.iter().all(|v| !v.is_empty()));
        let updated = read_windows_composer_response(&mut reader, |_| panic!("no progress after ready")).unwrap();
        assert_eq!(updated["revision"], 7);
    }

    #[test]
    fn windows_progress_retains_errors_and_rejects_incomplete_or_unknown_replies() {
        let mut reader = std::io::Cursor::new(concat!(
            "{\"phase\":\"progress\",\"stage\":\"control\"}\n",
            "{\"ok\":false,\"error\":\"输入框不可写\"}\n",
        ));
        assert_eq!(read_windows_composer_response(&mut reader, |_| {}).unwrap_err(), "输入框不可写");
        for input in [
            "{\"phase\":\"progress\",\"stage\":\"focus\"}\n",
            "{\"phase\":\"progress\",\"stage\":\"untrusted details\"}\n",
            "{}\n", "invalid\n", "",
        ] {
            assert!(read_windows_composer_response(&mut std::io::Cursor::new(input), |_| {}).is_err());
        }
    }
    use std::time::Duration;

    #[test]
    fn releasing_a_recording_drains_updates_without_cancel_or_send() {
        use std::sync::{mpsc, Arc, Mutex, atomic::AtomicBool};
        let (sender, receiver) = mpsc::channel();
        let mut bridge = CodexComposerBridge {
            sender, failed: Arc::new(AtomicBool::new(false)), closed: Arc::new(AtomicBool::new(false)),
            release_result: std::sync::Mutex::new(None),
            #[cfg(any(windows, target_os = "macos"))]
            termination: None,
        };
        let value = Arc::new(Mutex::new("已经输入\n".to_string()));
        let worker_value = value.clone();
        let worker = std::thread::spawn(move || {
            let mut draft = crate::voice_draft::VoiceDraft::new(worker_value.lock().unwrap().clone());
            let mut pending = None;
            while let Some(command) = receive_latest_composer_command(&receiver, &mut pending) {
                match composer_command_kind(&command) {
                    "update" => {
                        draft.last = draft.desired(command.payload["text"].as_str().unwrap());
                        *worker_value.lock().unwrap() = draft.last.clone();
                    }
                    "release" => {
                        command.response.unwrap().send(Ok(json!({"ok":true}))).unwrap();
                        return;
                    }
                    _ => panic!("release must not cancel or send the draft"),
                }
            }
        });
        #[cfg(target_os = "macos")]
        { bridge.termination = Some(crate::device_voice_lifecycle::WriterTermination::new(worker)); }
        bridge.update(1, "部分").unwrap();
        bridge.update(2, "完整的一句话").unwrap();
        bridge.preserve_draft().unwrap();
        assert!(bridge.update(3, "迟到的旧结果").is_err());
        drop(bridge);
        #[cfg(windows)]
        worker.join().unwrap();
        let next = crate::voice_draft::VoiceDraft::new(value.lock().unwrap().clone());
        assert_eq!(next.desired("第二句话"), "已经输入\n完整的一句话 第二句话");
    }

    #[test]
    fn windows_append_contract_preserves_base_and_formatting() {
        let source = include_str!("codex_composer.rs");
        let begin = source.find("function Normalize-ComposerText").unwrap();
        let end = source[begin..].find("function Add-UniqueComposerElement").unwrap() + begin;
        let functions = &source[begin..end];
        assert!(functions.contains("function Join-VoiceDraft"));
        assert!(!functions.contains("Normalize-Label"));
        assert!(source.contains("BaseValue = [string]$composer.Value"));
        assert!(source.contains("Join-VoiceDraft $state.BaseValue $text"));
        assert!(source.contains("ReplaceFocusedText($state.BaseValue)"));
        assert!(source.contains("$currentValue -cne $state.LastValue"));
    }

    #[cfg(any(windows, target_os="macos"))]
    #[test]
    fn realtime_delegation_submits_one_exact_atomic_command_and_waits_for_ack() {
        use std::sync::{Arc,atomic::AtomicBool,mpsc};
        let (sender,receiver)=mpsc::channel::<ComposerCommand>();
        let bridge=CodexComposerBridge {sender,failed:Arc::new(AtomicBool::new(false)),closed:Arc::new(AtomicBool::new(false)),release_result:std::sync::Mutex::new(None),termination:None};
        let text=format!("语音对话数据（JSON）：\n{}",serde_json::json!({"conversation":[{"speaker":"user","text":"然后搜索AI硬件这个关键词"},{"speaker":"pet","text":"在抖音搜索吗？"}],"current_user_request":"是的"}));
        let expected=text.clone();
        let worker=std::thread::spawn(move || {
            let command=receiver.recv().unwrap();
            assert_eq!(command.payload["kind"],"dispatch");
            assert_eq!(command.payload["text"],expected);
            command.response.unwrap().send(Ok(json!({"ok":true,"phase":"submitted","submission":"queue"}))).unwrap();
            let release=receiver.recv().unwrap();assert_eq!(release.payload["kind"],"release");
            release.response.unwrap().send(Ok(json!({"ok":true}))).unwrap();
        });
        assert_eq!(bridge.dispatch_task(&text).unwrap(),"queue");
        bridge.preserve_draft().unwrap();drop(bridge);worker.join().unwrap();
    }

    #[cfg(any(windows, target_os="macos"))]
    #[test]
    fn task_draft_snapshot_roundtrip_preserves_exact_text_and_native_mode() {
        use std::sync::{Arc, atomic::AtomicBool, mpsc};
        let (sender, receiver) = mpsc::channel::<ComposerCommand>();
        let bridge = CodexComposerBridge { sender, failed:Arc::new(AtomicBool::new(false)), closed:Arc::new(AtomicBool::new(false)), release_result:std::sync::Mutex::new(None), termination:None };
        let snapshot = json!({"target":{"composerId":"pinned"},"text":"  manually edited\n    prefix"});
        let expected = snapshot.clone();
        let text = "  manually edited\n    prefix\n\nnew request";
        let worker = std::thread::spawn(move || {
            let inspect = receiver.recv().unwrap();
            assert_eq!(inspect.payload["kind"], "inspect_task_draft");
            inspect.response.unwrap().send(Ok(json!({"ok":true,"snapshot":expected}))).unwrap();
            let dispatch = receiver.recv().unwrap();
            assert_eq!(dispatch.payload["draftSnapshot"], expected);
            assert_eq!(dispatch.payload["text"], text);
            dispatch.response.unwrap().send(Ok(json!({"ok":true,"submission":"steer"}))).unwrap();
            let release = receiver.recv().unwrap();
            assert_eq!(release.payload["kind"], "release");
            release.response.unwrap().send(Ok(json!({"ok":true}))).unwrap();
        });
        assert_eq!(bridge.inspect_task_draft().unwrap(), snapshot);
        assert_eq!(bridge.dispatch_task_from_draft(text, &snapshot).unwrap(), "steer");
        bridge.preserve_draft().unwrap();
        worker.join().unwrap();
    }

    #[test]
    fn realtime_delegation_keeps_legacy_empty_draft_and_snapshot_readback_guards() {
        let windows=include_str!("codex_composer.rs");
        let dispatch=windows.split("'dispatch' {").nth(1).unwrap().split("'release' {").next().unwrap();
        assert!(dispatch.contains("DESKTOP_TASK_DRAFT_NOT_EMPTY"));
        assert!(dispatch.contains("DESKTOP_TASK_READBACK_CHANGED"));
        assert!(dispatch.contains("Assert-TargetCurrent $state $true"));
        assert!(dispatch.contains("Invoke-SendButton $state $true $true"));
        assert!(windows.contains("if ($state.Agent -eq 'deepseek-harness' -or $strictButton -or -not $allowQueue)"));
        let mac=include_str!("codex_composer_macos.rs");
        assert!(mac.contains("!state.draft.base.is_empty() || !target.value.is_empty()"));
        assert!(mac.contains("confirm_voice_inner(state, Some(text))"));
        assert!(mac.contains("expected.is_some_and(|text| target.value != text)"));
        assert!(mac.contains("expected.get(\"text\") != current.get(\"text\")"));
        assert!(dispatch.contains("Assert-TaskDraftSnapshot $state $command.draftSnapshot"));
        let confirm=mac.split("fn confirm_voice_inner(").nth(1).unwrap()
            .split("\npub(super) fn cancel_voice(").next().unwrap();
        let ordinary=confirm.rsplit_once("\n    } else {\n").unwrap().1
            .split("\n    match wait_for_submit_readback(").next().unwrap();
        assert_eq!(ordinary.matches("submit_once(").count(),1);
        assert!(ordinary.contains("press_unique_submission(&target, true)"));
        assert!(ordinary.contains("assert_submission_target(&target)?; press_enter(target.agent, &target.app)"));
        let policy=mac.split("fn submit_once(").nth(1).unwrap()
            .split("\nfn press_unique_submission(").next().unwrap();
        assert!(policy.contains("match button()?"));
        assert!(policy.contains("Some(mode) => Ok(mode)"));
        assert!(policy.contains("None => { enter()?; Ok(\"enter\") }"));
        let unchanged=confirm.split("Ok(SubmitReadback::DraftUnchanged) => {").nth(1).unwrap()
            .split("\n        }").next().unwrap();
        // The old ordinary-confirm resend body must stay empty apart from
        // comments: an unchanged draft never authorizes a second submission.
        assert!(unchanged.lines().filter(|line| !line.trim().starts_with("//"))
            .all(|line| line.trim().is_empty()));
    }

    #[cfg(windows)]
    #[test]
    fn windows_powershell_append_preserves_previous_segments() {
        let source = include_str!("codex_composer.rs");
        let start = source.find("function Normalize-ComposerText").unwrap();
        let end = start + source[start..].find("function Get-ComposerText").unwrap();
        let script = format!("{}\nif ((Join-VoiceDraft 'first' 'second') -cne 'first second') {{ exit 1 }}\nif ((Join-VoiceDraft \"line`n  \" 'next') -cne \"line`n  next\") {{ exit 2 }}\nif ((Join-VoiceDraft 'old' '') -cne 'old') {{ exit 3 }}", &source[start..end]);
        assert!(hidden_powershell().arg("-Command").arg(script).status().unwrap().success());
    }

    fn command(kind: &str, revision: u64) -> ComposerCommand {
        ComposerCommand {
            payload: json!({ "kind": kind, "revision": revision }),
            started: None,
            response: None,
        }
    }

    #[test]
    fn live_update_failure_does_not_disable_explicit_confirmation() {
        assert!(!composer_command_failure_is_fatal(&command("update", 1)));
        assert!(!composer_command_failure_is_fatal(&command("confirm", 2)));
        assert!(composer_command_failure_is_fatal(&command("begin", 0)));
    }

    #[test]
    fn workspace_label_uses_the_last_path_component() {
        assert_eq!(workspace_label_from_cwd("D:\\code\\claw-pet\\"), "claw-pet");
        assert_eq!(workspace_label_from_cwd("/tmp/demo/"), "demo");
        assert_eq!(workspace_label_from_cwd(""), "");
    }

    #[cfg(windows)]
    #[test]
    fn claude_desktop_mapping_prefers_the_exact_titled_non_archived_session() {
        let root = tempfile::tempdir().unwrap();
        let account = root.path().join("account").join("org");
        std::fs::create_dir_all(&account).unwrap();
        let cli_session_id = "11111111-2222-4333-8444-555555555555";
        let generic_id = "local_aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
        let titled_id = "local_bbbbbbbb-cccc-4ddd-8eee-ffffffffffff";
        let archived_id = "local_cccccccc-dddd-4eee-8fff-000000000000";
        std::fs::write(
            account.join(format!("{generic_id}.json")),
            serde_json::to_vec(&json!({
                "sessionId": generic_id,
                "cliSessionId": cli_session_id,
                "title": "",
                "lastActivityAt": 9999
            }))
            .unwrap(),
        )
        .unwrap();
        std::fs::write(
            account.join(format!("{titled_id}.json")),
            serde_json::to_vec(&json!({
                "sessionId": titled_id,
                "cliSessionId": cli_session_id,
                "title": "Existing task",
                "lastActivityAt": 100
            }))
            .unwrap(),
        )
        .unwrap();
        std::fs::write(
            account.join(format!("{archived_id}.json")),
            serde_json::to_vec(&json!({
                "sessionId": archived_id,
                "cliSessionId": cli_session_id,
                "title": "Existing task",
                "isArchived": true,
                "lastActivityAt": 99999
            }))
            .unwrap(),
        )
        .unwrap();

        let target =
            claude_desktop_session_target_from_root(root.path(), cli_session_id, "Existing task")
                .unwrap();
        assert_eq!(target.session_id, titled_id);
        assert_eq!(target.title, "Existing task");
        assert!(claude_desktop_session_target_from_root(
            root.path(),
            "22222222-3333-4444-8555-666666666666",
            "Missing"
        )
        .is_none());
    }

    #[cfg(windows)]
    #[test]
    fn windows_codex_deep_link_targets_the_exact_thread_id() {
        let session_id = "019fbd0f-7a5a-73a3-9f53-af7c03d0ac9e";
        assert_eq!(
            codex_session_deep_link(session_id).as_deref(),
            Some("codex://threads/019fbd0f-7a5a-73a3-9f53-af7c03d0ac9e")
        );
        assert!(codex_session_deep_link("not-a-session").is_none());

        let source = include_str!("codex_composer.rs");
        assert!(source.contains("function Open-CodexSessionById"));
        assert!(source.contains("Start-Process $deepLink"));
        assert!(source.contains("for already selected session voice input"));
        assert!(source.contains("return Open-CodexSession $sessionTitle $workspaceLabel"));
    }

    #[cfg(windows)]
    #[test]
    fn windows_codex_voice_filters_candidates_and_reuses_the_located_root() {
        let source = include_str!("codex_composer.rs");
        assert!(source.contains("function Find-DescendantsByControlTypes"));
        assert!(source.contains("$locatedCodexRoot = Open-CodexSessionById"));
        assert!(source.contains("Composer = Find-Composer $locatedCodexRoot $null"));
        assert!(source.contains("[System.Windows.Automation.ControlType]::Edit"));
        assert!(source.contains("[System.Windows.Automation.ControlType]::ListItem"));
        assert!(source.contains("[System.Windows.Automation.ControlType]::Button"));
        assert!(source.contains("let worker_process = process.clone();"));
        assert!(source.contains("WriterTermination::owned_process(Some(worker),process)"));
    }

    #[cfg(windows)]
    #[test]
    fn windows_draft_reader_uses_semantic_values_instead_of_prompt_strings() {
        let source = include_str!("codex_composer.rs");
        let reader = source.split("function Get-ComposerText($element) {").nth(1).unwrap()
            .split("function Get-NativeLegacyComposerSnapshot").next().unwrap();
        assert!(reader.contains("UIA_LegacyIAccessiblePatternId"));
        assert!(reader.contains("Read-ComposerTextPattern $element $pattern"));
        assert!(!reader.contains("Current.Name"));
        assert!(!reader.contains("HelpText"));
        assert!(!reader.contains("TrimStart"));
        assert!(!reader.contains("type / for commands"));
        assert!(source.contains("$rect.Right -lt ($composerRect.Left - 12)"));
    }

    #[cfg(windows)]
    #[test]
    fn windows_claude_binding_uses_an_exact_active_session_signal() {
        let source = include_str!("codex_composer.rs");
        assert!(source.contains("function Get-ClaudeDesktopSessionState"));
        assert!(source.contains("FocusedDesktopSessionId"));
        assert!(source.contains("LastFocusedAt"));
        assert!(source.contains("if ($metadataState -eq 'unavailable')"));
        assert!(source.contains("SessionId = $sessionId"));
        assert!(source.contains("DesktopSessionId = $desktopSessionId"));
        assert!(source.contains("$state.DesktopSessionId $state.SessionTitle $allowedValues"));
        let removed_process_probe = ["function Get-ClaudeProcess", "SessionState"].concat();
        assert!(!source.contains(&removed_process_probe));
        let unsupported_clock = ["TickCount", "64"].concat();
        assert!(!source.contains(&unsupported_clock));
        assert!(source.contains("Claude Desktop has no existing session mapped"));
        assert!(source.contains("$existingWindows = @(Get-RestoredClaudeWindows)"));
        assert!(source.contains("function Find-ClaudeSessionRows"));
        assert!(source.contains("function Invoke-ClaudeSessionRow"));
        let open_start = source.find("function Open-ClaudeSession(").unwrap();
        let open_end = source[open_start..]
            .find("function Test-WorkspaceAncestor")
            .map(|offset| open_start + offset)
            .unwrap();
        let open_claude = &source[open_start..open_end];
        assert!(open_claude.contains("Invoke-ClaudeSessionRow"));
        assert!(!open_claude.contains("Start-Process"));
        let removed_document_scan =
            ["$mainDocuments = Find-Descendants", "ByControlTypes"].concat();
        assert!(!source.contains(&removed_document_scan));
    }

    #[cfg(windows)]
    #[test]
    fn windows_composer_locator_uses_semantics_and_fails_closed_on_ambiguity() {
        let source = include_str!("codex_composer.rs");
        assert!(source.contains("$current.IsKeyboardFocusable"));
        assert!(source.contains("$classHint = $className -match"));
        assert!(source.contains("$nameHint = $name -match"));
        assert!(source.contains("Multiple equally likely visible composers were found"));
        assert!(source.contains("Start-Sleep -Milliseconds 120"));
        assert!(source.contains("Wait-ComposerText $state $normalizedText 350"));
    }

    #[cfg(windows)]
    #[test]
    fn windows_composer_helper_is_bounded_and_owned_by_the_desktop_process() {
        assert_eq!(CODEX_COMPOSER_STARTUP_TIMEOUT_SECS, 10);
        assert_eq!(
            WINDOWS_COMPOSER_PROCESS_MEMORY_LIMIT_BYTES,
            512 * 1024 * 1024
        );

        let source = include_str!("codex_composer.rs");
        assert!(source.contains("function Get-OrLaunchCodexWindows"));
        assert!(source.contains("OpenAI.Codex_2p2nqsd0c76g0!App"));
        assert!(source.contains("function Find-PointComposerElements"));
        assert!(source.contains("AutomationElement]::FromPoint"));
        assert!(source.contains("function Find-BoundedComposerElements"));
        assert!(source.contains("$visited -lt 3500"));
        assert!(source.contains("[int]$budgetMs = 1800"));
        assert!(source.contains("(Get-MonotonicMilliseconds) + $budgetMs"));
        assert!(source.contains("$depth -ge 48"));
        assert!(source.contains("JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE"));
        assert!(source.contains("JOB_OBJECT_LIMIT_PROCESS_MEMORY"));
        assert!(source.contains("JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK"));
        assert!(source.contains("WindowsComposerJob::new(current_visible)"));
        assert!(source.contains("if contain_descendants { 0 } else { JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK }"));
        assert!(source.contains("self.job.active_processes()? == 0"));
        let gate=source.find("if ([Console]::In.ReadLine() -cne 'HACHIMODOCK_CONTAINED_HELPER_V1')").unwrap();
        assert!(source.find("[Console]::InputEncoding =").unwrap() < gate);
        assert!(gate < source.find("Add-Type -AssemblyName UIAutomationClient").unwrap());

        let fallback_start = source
            .find("function Find-BoundedComposerElements")
            .unwrap();
        let fallback_end = source[fallback_start..]
            .find("function Get-ComposerCandidates")
            .map(|offset| fallback_start + offset)
            .unwrap();
        let fallback = &source[fallback_start..fallback_end];
        assert!(fallback.contains("Queue[object]"));
        assert!(fallback.contains("ControlType]::Edit.Id"));
        assert!(fallback.contains("ControlType]::Button.Id"));
        assert!(!fallback.contains("Stack[object]"));

        let locator_start = source.find("function Find-Composer(").unwrap();
        let locator_end = source[locator_start..]
            .find("function Get-CodexTarget")
            .map(|offset| locator_start + offset)
            .unwrap();
        let locator = &source[locator_start..locator_end];
        assert!(locator.contains("Find-PointComposerElements"));
        assert!(locator.contains("Find-BoundedComposerElements"));
        assert!(!locator.contains("TreeScope]::Descendants"));
        assert!(!locator.contains("Find-DescendantsByControlTypes"));
    }

    #[cfg(windows)]
    #[test]
    fn windows_running_task_confirm_accepts_queue_and_steer_but_not_stop() {
        let source = include_str!("codex_composer.rs");
        let labels_start = source
            .find("function Get-ComposerSubmitLabelScore")
            .unwrap();
        let labels_end = source[labels_start..]
            .find("function Invoke-SendButton")
            .map(|offset| labels_start + offset)
            .unwrap();
        let labels = &source[labels_start..labels_end];
        assert!(labels.contains("'queue', 'steer'"));
        assert!(!labels.contains("'stop'"));
        assert!(!labels.contains("'停止'"));
        assert!(!labels.contains("clear queue"));
        assert!(!labels.contains("清空队列"));
    }

    #[cfg(windows)]
    #[test]
    fn windows_session_navigation_uses_sidebar_geometry_not_volatile_css_classes() {
        let source = include_str!("codex_composer.rs");
        assert!(source.contains("$sidebarRight = $rootRect.Left"));
        assert!(source.contains("$rect.Left -ge $sidebarRight"));
        let removed_css_selector = ["after:block.*after:", "h-px"].concat();
        assert!(!source.contains(&removed_css_selector));
    }

    #[cfg(windows)]
    #[test]
    fn windows_current_visible_mode_skips_bound_session_navigation_and_pins_the_composer() {
        let source = include_str!("codex_composer.rs");
        assert!(source.contains("$currentVisible = $purpose -eq 'current_voice'"));
        assert!(source.contains("if (-not $currentVisible)"));
        assert!(source.contains("Get-CurrentVisibleTarget $agent $null"));
        assert!(source.contains("function Get-OrLaunchCodexWindows"));
        assert!(source.contains("function Get-OrLaunchClaudeWindows"));
        assert!(source.contains("com.squirrel.AnthropicClaude.claude"));
        assert!(source.contains("Claude_pzs8sxrjxfjjc!Claude"));
        assert!(source.contains("function Clear-ComposerVoiceText"));
        assert!(source.contains("Assert-TargetCurrent $state (-not [bool]$state.CurrentVisible)"));
        assert!(source.contains("Clear-ComposerVoiceText $state"));
        assert!(source.contains("ComposerRuntimeId = $composerRuntimeId"));
        assert!(source.contains("The visible Agent session or composer changed during voice input"));
    }

    #[cfg(windows)]
    #[test]
    fn windows_codex_window_discovery_keeps_minimized_uia_handles_with_invalid_geometry() {
        let source = include_str!("codex_composer.rs");
        assert!(source.contains("function Test-FiniteWindowRectangle"));
        assert!(source.contains("[double]::IsInfinity($number)"));
        assert!(source.contains("[Math]::Max([double]1, [double]($rect.Width * $rect.Height))"));
        assert!(source.contains("$hasFiniteRectangle = Test-FiniteWindowRectangle $rect"));
        assert!(source.contains("if ($hasFiniteRectangle -and -not $root.Current.IsOffscreen"));
        assert!(source.contains("$area = if ($hasFiniteRectangle)"));
        assert!(source.contains("[CodexVoiceNative]::RestoreWindow($handle)"));
        assert!(source.contains("function Wait-CodexWindowRoot"));
        assert!(source.contains("$window.Root = Ensure-CodexForeground"));
        assert!(source.contains("catch { continue }"));
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires a running, visible Codex window"]
    fn windows_current_visible_composer_smoke_test() {
        let bridge = CodexComposerBridge::start_current("codex", |_| {})
            .expect("current visible Codex composer should be addressable");
        bridge.cancel();
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires an installed Claude Desktop client with a current composer"]
    fn windows_current_visible_claude_composer_smoke_test() {
        let bridge = CodexComposerBridge::start_current("claude-code", |_| {})
            .expect("current visible Claude composer should be addressable");
        bridge.cancel();
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires CODEX_TEST_SESSION_ID/TITLE and a running Codex window"]
    fn windows_bound_codex_composer_smoke_test() {
        let session_id =
            std::env::var("CODEX_TEST_SESSION_ID").expect("CODEX_TEST_SESSION_ID is required");
        let session_title = std::env::var("CODEX_TEST_SESSION_TITLE")
            .expect("CODEX_TEST_SESSION_TITLE is required");
        let session_cwd = std::env::var("CODEX_TEST_SESSION_CWD").unwrap_or_default();
        let bridge = CodexComposerBridge::start(&session_id, &session_title, &session_cwd, |_| {})
            .expect("bound Codex composer should start without a duplicate transcript-tree scan");
        bridge.cancel();
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires CLAUDE_TEST_SESSION_ID/TITLE and a running Claude Desktop session"]
    fn windows_bound_claude_navigation_smoke_test() {
        let session_id =
            std::env::var("CLAUDE_TEST_SESSION_ID").expect("CLAUDE_TEST_SESSION_ID is required");
        let session_title = std::env::var("CLAUDE_TEST_SESSION_TITLE")
            .expect("CLAUDE_TEST_SESSION_TITLE is required");
        let session_cwd = std::env::var("CLAUDE_TEST_SESSION_CWD").unwrap_or_default();
        CodexComposerBridge::focus_claude_session(&session_id, &session_title, &session_cwd)
            .expect(
                "bound Claude Desktop session should be focused without importing a new session",
            );
    }

    #[cfg(windows)]
    fn run_current_visible_composer_reversible_write_smoke(agent_id: &str) {
        fn wait_for_phase(
            receiver: &std::sync::mpsc::Receiver<CodexComposerEvent>,
            expected: &str,
        ) -> CodexComposerEvent {
            let deadline = std::time::Instant::now() + Duration::from_secs(10);
            loop {
                let remaining = deadline.saturating_duration_since(std::time::Instant::now());
                let event = receiver
                    .recv_timeout(remaining)
                    .unwrap_or_else(|error| panic!("timed out waiting for {expected}: {error}"));
                if event.phase == expected {
                    return event;
                }
            }
        }

        let (event_sender, event_receiver) = std::sync::mpsc::channel();
        let bridge = CodexComposerBridge::start_current(agent_id, move |event| {
            let _ = event_sender.send(event);
        })
        .expect("current visible Agent composer should be addressable");
        bridge
            .update(1, "Pet Manager voice input probe")
            .expect("probe update should be queued");

        let updated = wait_for_phase(&event_receiver, "updated");
        assert!(updated.ok, "probe update failed: {}", updated.error);
        bridge.cancel();
        let cancelled = wait_for_phase(&event_receiver, "cancelled");
        assert!(
            cancelled.ok,
            "probe cancellation failed: {}",
            cancelled.error
        );
        drop(bridge);

        let verification = CodexComposerBridge::start_current(agent_id, |_| {})
            .expect("the composer should be empty after cancelling the probe");
        verification.cancel();
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "temporarily writes a probe into the current Codex composer, then removes it"]
    fn windows_current_visible_composer_reversible_write_smoke_test() {
        run_current_visible_composer_reversible_write_smoke("codex");
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "temporarily writes a probe into the current Claude composer, then removes it"]
    fn windows_current_visible_claude_reversible_write_smoke_test() {
        run_current_visible_composer_reversible_write_smoke("claude-code");
    }

    #[test]
    fn live_updates_are_coalesced_before_the_pending_confirm() {
        let (sender, receiver) = std::sync::mpsc::channel();
        sender.send(command("update", 1)).unwrap();
        sender.send(command("update", 2)).unwrap();
        sender.send(command("update", 3)).unwrap();
        sender.send(command("confirm", 4)).unwrap();
        let mut pending = None;

        let latest = receive_latest_composer_command(&receiver, &mut pending).unwrap();
        assert_eq!(composer_command_kind(&latest), "update");
        assert_eq!(latest.payload["revision"], 3);

        let confirm = receive_latest_composer_command(&receiver, &mut pending).unwrap();
        assert_eq!(composer_command_kind(&confirm), "confirm");
        assert_eq!(confirm.payload["revision"], 4);
    }

    #[test]
    fn final_readback_is_a_queue_barrier_even_with_later_updates() {
        for has_partial in [false, true] {
            let (sender, receiver) = std::sync::mpsc::channel();
            if has_partial { sender.send(command("update", 1)).unwrap(); }
            let (response, ack) = std::sync::mpsc::channel();
            let mut final_update = command("update", 2);
            final_update.response = Some(response);
            sender.send(final_update).unwrap();
            sender.send(command("update", 3)).unwrap();
            sender.send(command("confirm", 4)).unwrap();
            let mut pending = None;
            let final_update = receive_latest_composer_command(&receiver, &mut pending).unwrap();
            assert_eq!(final_update.payload["revision"], 2);
            final_update.response.unwrap().send(Ok(json!({"ok":true}))).unwrap();
            assert!(ack.recv().unwrap().is_ok());
            assert_eq!(receive_latest_composer_command(&receiver, &mut pending).unwrap().payload["revision"], 3);
            assert_eq!(receive_latest_composer_command(&receiver, &mut pending).unwrap().payload["revision"], 4);
        }
    }

    #[test]
    fn final_draft_waits_for_readback_and_propagates_editor_rejection() {
        use std::sync::{Arc, atomic::AtomicBool};
        for rejected in [false, true] {
            let (sender, receiver) = std::sync::mpsc::channel::<ComposerCommand>();
            let bridge = CodexComposerBridge {
                sender,
                failed: Arc::new(AtomicBool::new(false)),
                closed: Arc::new(AtomicBool::new(false)),
                release_result: std::sync::Mutex::new(None),
            #[cfg(any(windows, target_os = "macos"))]
            termination: None,
            };
            let worker = std::thread::spawn(move || {
                let command = receiver.recv().unwrap();
                assert_eq!(command.payload["text"], "查一下北京今天的天气怎么样？");
                assert_eq!(command.payload["revision"], 9);
                command.response.unwrap().send(if rejected {
                    Err("selection not confirmed".to_string())
                } else {
                    Ok(json!({"ok":true,"phase":"updated"}))
                }).unwrap();
            });
            let result = bridge.update_confirmed(9, "查一下北京今天的天气怎么样？");
            assert_eq!(result.is_err(), rejected);
            if rejected { assert_eq!(result.unwrap_err(), "selection not confirmed"); }
            worker.join().unwrap();
        }
    }

    #[test]
    fn submission_does_not_consume_completion_before_worker_start() {
        let (_started_sender, started) = std::sync::mpsc::channel();
        let (completed_sender, completed) = std::sync::mpsc::channel();
        completed_sender.send(Ok(json!({ "ok": true }))).unwrap();
        let submission = CodexComposerSubmission { started, completed };

        assert_eq!(
            submission.wait(Duration::from_millis(1), Duration::ZERO),
            Err(CodexComposerWaitError::StartTimeout)
        );
    }

    #[test]
    fn submission_completion_timer_begins_after_worker_start() {
        let (started_sender, started) = std::sync::mpsc::channel();
        let (completed_sender, completed) = std::sync::mpsc::channel();
        completed_sender.send(Ok(json!({ "ok": true }))).unwrap();
        started_sender.send(()).unwrap();
        let submission = CodexComposerSubmission { started, completed };

        assert!(submission
            .wait(Duration::ZERO, Duration::ZERO)
            .unwrap()
            .is_ok());
    }
}
