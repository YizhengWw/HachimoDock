/*
 * [Input] A bound or current-visible ChatGPT（Codex）/Claude session, a current-visible WorkBuddy composer, or a captured MiMoCode terminal caret, plus staged voice text and an explicit confirm action.
 * [Output] Read-only frontmost-Agent detection, exact desktop-session navigation, bounded composer lookup, per-recording appended draft updates, and explicit-confirm submission without automatic send on ASR finalization.
 * [Pos] Cross-platform foreground input bridge with session, draft, clipboard, stale-focus recovery, and Windows minimized-Claude restoration.
 * [Sync] If this file changes, update pc/.folder.md.
 */

use serde_json::{json, Value};

#[cfg(target_os = "macos")]
#[path = "codex_composer_macos.rs"]
mod macos;

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
    fn new() -> Result<Self, String> {
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
            | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
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
}

#[cfg(any(windows, target_os = "macos"))]
impl CodexComposerBridge {
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
        use std::sync::atomic::Ordering;
        if self.closed.swap(true, Ordering::SeqCst) { return Ok(()); }
        let (tx, rx) = std::sync::mpsc::channel();
        self.sender.send(ComposerCommand {
            payload: json!({ "kind": "release" }), started: None, response: Some(tx),
        }).map_err(|_| "上一段语音输入已关闭".to_string())?;
        rx.recv_timeout(std::time::Duration::from_secs(8))
            .map_err(|_| "等待上一段语音完成写入超时".to_string())??;
        Ok(())
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
            _ => {
                return Err(
                    "Current visible composer requires ChatGPT（Codex）, Claude or WorkBuddy".to_string(),
                )
            }
        };
        Self::start_with_purpose(agent, "", "", "", "", "current_voice", callback)
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
        if !matches!(agent, "codex" | "claude" | "workbuddy") {
            return Err("visible composer agent is invalid".to_string());
        }
        let current_visible = purpose == "current_voice";
        if agent == "workbuddy" && !current_visible {
            return Err("WorkBuddy 仅向当前可见对话输入，不回退到其他 Agent".into());
        }
        if !current_visible
            && agent == "claude"
            && (session_id.is_empty() || !valid_claude_desktop_session_id(deep_link))
        {
            return Err("Claude visible composer requires a bound session ID".to_string());
        }
        if !current_visible && session_title.is_empty() {
            return Err("visible composer requires a non-empty bound session title".to_string());
        }
        if !matches!(purpose, "voice" | "locate" | "current_voice") {
            return Err("Visible Agent composer purpose is invalid".to_string());
        }

        const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
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

  private static void Send(List<INPUT> inputs) {
    if (inputs.Count == 0) return;
    INPUT[] values = inputs.ToArray();
    uint sent = SendInput((uint)values.Length, values, Marshal.SizeOf(typeof(INPUT)));
    if (sent != values.Length) {
      int error = Marshal.GetLastWin32Error();
      throw new Win32Exception(
        error,
        "Windows rejected Agent voice keyboard input (" + sent + "/" + values.Length + ", win32=" + error + ")"
      );
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
}

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
  void FindAll();
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
  void RangeFromChild();
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
  void GetEnclosingElement();
  [return: MarshalAs(UnmanagedType.BStr)]
  string GetText(int maxLength);
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
  public string Value { get { return pattern.GetCurrentValue() ?? string.Empty; } }
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
  public string GetText(int maxLength) { return range.GetText(maxLength) ?? string.Empty; }
  public object GetAttributeValue(object attribute) { return range.GetAttributeValue(WorkBuddyUia.IdOf(attribute)); }
}

public sealed class WorkBuddyUiaTextPattern {
  private readonly IWorkBuddyUiaTextPattern pattern;
  internal WorkBuddyUiaTextPattern(IWorkBuddyUiaTextPattern pattern) { this.pattern = pattern; }
  public WorkBuddyUiaTextRange DocumentRange { get { return new WorkBuddyUiaTextRange(pattern.GetDocumentRange()); } }
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
  public bool HasKeyboardFocus { get { return element.Flag(30008); } }
  public bool IsKeyboardFocusable { get { return element.Flag(30009); } }
  public bool IsEnabled { get { return element.Flag(30010); } }
  public string ClassName { get { return element.Text(30012); } }
  public int NativeWindowHandle { get { return element.Int(30020); } }
  public bool IsOffscreen { get { return element.Flag(30022); } }
}

public sealed class WorkBuddyUiaElement {
  private readonly IWorkBuddyUiaElement element;
  internal WorkBuddyUiaElement(IWorkBuddyUiaElement element) { this.element = element; }
  internal IWorkBuddyUiaElement Native { get { return element; } }
  public WorkBuddyUiaCurrent Current { get { return new WorkBuddyUiaCurrent(this); } }
  public int[] GetRuntimeId() {
    try { return element.GetRuntimeId() ?? new int[0]; } catch { return new int[0]; }
  }
  public void SetFocus() { element.SetFocus(); }
  public object GetCurrentPropertyValue(object property) { return Property(WorkBuddyUia.IdOf(property)); }
  public bool TryGetCurrentPattern(object pattern, out object patternObject) {
    patternObject = null;
    int patternId = WorkBuddyUia.IdOf(pattern);
    object raw;
    try { raw = element.GetCurrentPattern(patternId); } catch { return false; }
    if (raw == null) { return false; }
    if (patternId == 10002) {
      IWorkBuddyUiaValuePattern value = raw as IWorkBuddyUiaValuePattern;
      if (value != null) { patternObject = new WorkBuddyUiaValuePattern(value); }
    } else if (patternId == 10014) {
      IWorkBuddyUiaTextPattern text = raw as IWorkBuddyUiaTextPattern;
      if (text != null) { patternObject = new WorkBuddyUiaTextPattern(text); }
    } else if (patternId == 10000) {
      IWorkBuddyUiaInvokePattern invoke = raw as IWorkBuddyUiaInvokePattern;
      if (invoke != null) { patternObject = new WorkBuddyUiaInvokePattern(invoke); }
    }
    return patternObject != null;
  }
  internal object Property(int propertyId) {
    try { return element.GetCurrentPropertyValue(propertyId); } catch { return null; }
  }
  internal int Int(int propertyId) {
    object value = Property(propertyId);
    return value is int ? (int)value : 0;
  }
  internal bool Flag(int propertyId) {
    object value = Property(propertyId);
    return value is bool && (bool)value;
  }
  internal string Text(int propertyId) {
    return Property(propertyId) as string ?? string.Empty;
  }
}

public static class WorkBuddyUia {
  private static IWorkBuddyUia automation;

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
      if (node.Value > 0) {
        int typeId = 0;
        try {
          object type = node.Key.GetCurrentPropertyValue(30003);
          if (type is int) { typeId = (int)type; }
        } catch {}
        // Edit, Button, Group, Custom, Document, Pane.
        if (typeId == 50004 || typeId == 50000 || typeId == 50026 ||
            typeId == 50025 || typeId == 50030 || typeId == 50033) {
          matches.Add(new WorkBuddyUiaElement(node.Key));
        }
      }
      if (node.Value >= 48 || discovered >= maxNodes) { continue; }
      try {
        IWorkBuddyUiaElement child = walker.GetFirstChildElement(node.Key);
        while (child != null && discovered < maxNodes && clock.ElapsedMilliseconds < budgetMs) {
          pending.Enqueue(new KeyValuePair<IWorkBuddyUiaElement, int>(child, node.Value + 1));
          discovered += 1;
          child = walker.GetNextSiblingElement(child);
        }
      } catch {}
    }
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
  return $text.Replace("`r`n", "`n")
}

function Join-VoiceDraft([string]$base, [string]$transcript) {
  $text = (Normalize-ComposerText $transcript).Trim()
  if ([string]::IsNullOrEmpty($text)) { return $base }
  if ([string]::IsNullOrEmpty($base) -or $base -match '\s$') { return ($base + $text) }
  return ($base + ' ' + $text)
}

function Get-ComposerText($element) {
  $pattern = $null
  $value = ''
  if ($element.TryGetCurrentPattern(
      [System.Windows.Automation.ValuePattern]::Pattern,
      [ref]$pattern
  )) {
    $value = Normalize-ComposerText ([string]$pattern.Current.Value)
  } elseif ($element.TryGetCurrentPattern(
      [System.Windows.Automation.TextPattern]::Pattern,
      [ref]$pattern
  )) {
    $value = Normalize-ComposerText ([string]$pattern.DocumentRange.GetText(-1))
  } else {
    throw 'Visible composer exposes neither ValuePattern nor TextPattern'
  }
  $value = $value.TrimStart([char]0xFEFF)
  if ($value.Trim() -eq '今天帮你做些什么？ @ 引用对话文件，/ 调用技能与指令') { $value = '' }
  if ($value -match '^(?i:随心输入|输入消息|message codex|ask anything|write your prompt to claude|type / for commands|write a message\W*|send a message\W*)$') { $value = '' }
  return @($pattern, $value)
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
    if ($pattern -is [System.Windows.Automation.ValuePattern] -or
        $pattern -is [WorkBuddyUiaValuePattern]) {
      return -not $pattern.Current.IsReadOnly
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
    if (-not $current.IsEnabled -or $current.IsOffscreen -or
        -not $current.IsKeyboardFocusable) { continue }
    $rect = $current.BoundingRectangle
    if (-not (Test-FiniteWindowRectangle $rect)) { continue }
    if ($rect.Width -lt 240 -or $rect.Height -lt 22) { continue }
    if ($rect.Bottom -lt ($rootRect.Top + ($rootRect.Height * 0.40))) { continue }

    $className = [string]$current.ClassName
    $name = Normalize-Label ([string]$current.Name)
    $typeId = $current.ControlType.Id
    $classHint = $className -match '(?i)(^|\s)(ProseMirror|tiptap)(\s|$)'
    $nameHint = $name -match '(?i)prompt|message|ask|write|输入|消息|提问'
    $documentHint = $false
    if ($workbuddy) {
      if ($name -match '(?i)search|搜索') { continue }
      if (-not (Test-WorkBuddyElementOwner $root $element)) { continue }
      # Rich contenteditables may appear as Document rather than Edit. Require
      # explicit textbox/editor semantics, not merely a readable chat document.
      $ariaRole = ''
      try {
        $ariaRoleProperty = [System.Windows.Automation.AutomationProperty]::LookupById(30101)
        if ($null -ne $ariaRoleProperty) { $ariaRole = [string]$element.GetCurrentPropertyValue($ariaRoleProperty) }
      } catch {}
      if ($ariaRole -match '^(?i:searchbox)$') { continue }
      $documentHint = $typeId -eq [System.Windows.Automation.ControlType]::Document.Id -and
        ($classHint -or $ariaRole -match '^(?i:textbox)$')
      if ($typeId -eq [System.Windows.Automation.ControlType]::Document.Id -and -not $documentHint) { continue }
    }
    $typeHint = $typeId -eq [System.Windows.Automation.ControlType]::Edit.Id -or
      $typeId -eq [System.Windows.Automation.ControlType]::Group.Id -or
      $typeId -eq [System.Windows.Automation.ControlType]::Custom.Id
    if (-not $classHint -and -not $nameHint -and -not $typeHint -and -not $documentHint) { continue }

    $valueInfo = $null
    try { $valueInfo = Get-ComposerText $element } catch { continue }
    $readableCount += 1
    if ($workbuddy -and -not (Test-WorkBuddyWritablePattern $valueInfo[0])) { continue }
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
    if ($score -lt 5) { continue }

    $candidate = [pscustomobject]@{
      Element = $element
      Pattern = $valueInfo[0]
      Value = [string]$valueInfo[1]
      Score = [int]$score
      Bottom = [double]$rect.Bottom
      Width = [double]$rect.Width
      BoundsKey = "$([int]$rect.Left),$([int]$rect.Top),$([int]$rect.Width),$([int]$rect.Height)"
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
  $set = Get-ComposerCandidates $root @($focused) $allowedValues $true
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

function Get-WorkBuddyTarget([string[]]$allowedValues) {
  Write-ComposerProgress 'resolve_window'
  $windows = @(Get-Process -Name WorkBuddy -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 })
  if ($windows.Count -ne 1) { throw '请保留一个 WorkBuddy 主窗口并打开目标对话，再重新开始语音输入' }
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
        $current = Get-ComposerText $composer.Element
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
  throw "WorkBuddy 输入框未就绪（节点=$lastCount，可读=$readableCount，可写=$writableCount$activation）；未取得可写编辑器，已停止写入。请保持目标对话可见。尚未写入或发送文字"
}

function Get-CurrentVisibleTarget([string]$agent, [string[]]$allowedValues) {
  if ($agent -eq 'workbuddy') {
    return Get-WorkBuddyTarget $allowedValues
  }
  if ($agent -eq 'codex') {
    $windows = @(Get-OrLaunchCodexWindows)
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
    $windows = @(Get-OrLaunchClaudeWindows)
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

function Assert-TargetCurrent($state, [bool]$checkSession) {
  [void](Get-Process -Id $state.ProcessId -ErrorAction Stop)
  if (-not [CodexVoiceNative]::IsWindow([IntPtr]$state.WindowHandle) -or
      $state.Root.Current.NativeWindowHandle -ne $state.RootHandle) {
    throw 'Visible Agent window changed during voice input'
  }
  if ($state.CurrentVisible -and $state.Agent -eq 'workbuddy') {
    Assert-WorkBuddyComposerCurrent $state
  } elseif ($state.CurrentVisible) {
    if ($checkSession) {
      $current = Get-CurrentVisibleTarget $state.Agent @($state.LastValue)
      $runtimeId = Get-ElementRuntimeId $current.Composer.Element
      if ($current.ProcessId -ne $state.ProcessId -or
          $current.WindowHandle -ne $state.WindowHandle -or
          $current.RootHandle -ne $state.RootHandle -or
          -not $runtimeId -or $runtimeId -ne $state.ComposerRuntimeId) {
        throw 'The visible Agent session or composer changed during voice input'
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
  if ($currentValue -cne $state.LastValue) {
    throw 'Visible composer was edited outside voice input; voice updates were cancelled'
  }
  $state.Pattern = $currentInfo[0]
}

function Focus-Composer($state) {
  Write-ComposerProgress 'focus'
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
  $state.Composer.SetFocus()
  Start-Sleep -Milliseconds 30
  if (-not $state.Composer.Current.HasKeyboardFocus) {
    throw 'Visible composer could not receive keyboard focus'
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
      $updatedInfo = Get-ComposerText $state.Composer
      $updatedValue = [string]$updatedInfo[1]
      if ($updatedValue -ceq $expectedText) {
        $state.Pattern = $updatedInfo[0]
        return $true
      }
    } catch {}
    if ((Get-MonotonicMilliseconds) -ge $deadline) { break }
    Start-Sleep -Milliseconds 25
  } while ($true)
  return $false
}

function Set-ComposerText($state, [string]$text, [bool]$forceSessionCheck) {
  $normalizedText = Join-VoiceDraft $state.BaseValue $text
  if ($normalizedText -ceq $state.LastValue) {
    Assert-TargetCurrent $state $forceSessionCheck
    return
  }
  $lastError = ''
  # Never replay an uncertain WorkBuddy rich-editor insertion.
  $attempts = if ($state.Agent -eq 'workbuddy') { 1 } else { 2 }
  for ($attempt = 0; $attempt -lt $attempts; $attempt += 1) {
    try {
      if ($attempt -gt 0) {
        Rebind-CodexTarget $state @($state.LastValue, $normalizedText)
        if ($state.LastValue -ceq $normalizedText) { return }
      }
      $now = Get-MonotonicMilliseconds
      $checkSession = $forceSessionCheck -or (($now - $state.LastSessionCheck) -ge 900)
      Assert-TargetCurrent $state $checkSession
      if ($checkSession) { $state.LastSessionCheck = $now }
      Focus-Composer $state
      Assert-TargetCurrent $state $false
      [CodexVoiceNative]::ReplaceFocusedText($normalizedText)
      # Chromium can block a synchronous TextPattern read while committing the
      # just-injected ProseMirror update. Let the accessibility tree settle.
      Start-Sleep -Milliseconds 120
      # WorkBuddy receives the whole final transcript as one burst of typed
      # characters; allow for its length. Readback returns on first match.
      $confirmMs = if ($state.Agent -eq 'workbuddy') {
        [Math]::Min(3000, 350 + (10 * $normalizedText.Length))
      } else { 350 }
      if (-not (Wait-ComposerText $state $normalizedText $confirmMs)) {
        throw 'Visible composer did not confirm the voice transcript in time'
      }
      $state.LastValue = $normalizedText
      return
    } catch {
      $lastError = $_.Exception.Message
      if ($attempt + 1 -lt $attempts) { Start-Sleep -Milliseconds 40 }
    }
  }
  throw "Visible composer update failed: $lastError"
}

function Clear-ComposerVoiceText($state) {
  if ($state.LastValue -ceq $state.BaseValue) { return }
  Assert-TargetCurrent $state (-not [bool]$state.CurrentVisible)
  Focus-Composer $state
  Assert-TargetCurrent $state $false
  [CodexVoiceNative]::ReplaceFocusedText($state.BaseValue)
  $state.LastValue = $state.BaseValue
}

function Assert-ComposerHasText($state) {
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

function Invoke-SendButton($state) {
  $composerRect = $state.Composer.Current.BoundingRectangle
  if (-not (Test-FiniteWindowRectangle $composerRect)) {
    throw 'Visible composer geometry is unavailable for submission'
  }
  # Reuse the bounded breadth-first surface walk. A live task transcript can
  # make an unbounded descendant Button query stall just like the old Edit
  # query did during startup; Enter remains the verified fallback.
  $all = if ($state.Agent -eq 'workbuddy') {
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
    $invoke = $null
    if (-not $element.TryGetCurrentPattern(
        [System.Windows.Automation.InvokePattern]::Pattern,
        [ref]$invoke
    )) { continue }
    $buttons += [pscustomobject]@{
      Pattern = $invoke
      Score = [int]$labelScore
      Left = [double]$rect.Left
    }
  }
  $buttons = @($buttons | Sort-Object `
    @{ Expression = 'Score'; Ascending = $true }, `
    @{ Expression = 'Left'; Descending = $true })
  if ($buttons.Count -eq 1 -or
      ($buttons.Count -gt 1 -and $buttons[0].Score -lt $buttons[1].Score)) {
    $buttons[0].Pattern.Invoke()
    return
  }
  if ($buttons.Count -gt 1) {
    throw "Expected one enabled send button near the composer; found $($buttons.Count)"
  }

  Focus-Composer $state
  [CodexVoiceNative]::PressEnter()
  Start-Sleep -Milliseconds 180
  $afterSubmit = Get-ComposerText $state.Composer
  if (-not [string]::IsNullOrEmpty([string]$afterSubmit[1])) {
    throw 'Visible composer did not accept Enter submission; the voice draft remains in the composer'
  }
}
$state = $null
$script:WorkBuddyAccessibilityRequests = @{}
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
        $sessionId = Normalize-Label ([string]$command.sessionId)
        $desktopSessionId = Normalize-Label ([string]$command.desktopSessionId)
        $title = Normalize-Label ([string]$command.sessionTitle)
        $purpose = Normalize-Label ([string]$command.purpose)
        $workspaceLabel = Normalize-Label ([string]$command.workspaceLabel)
        $currentVisible = $purpose -eq 'current_voice'
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
        $target = if ($currentVisible) {
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
        Set-ComposerText $state ([string]$command.text) $false
        Write-ComposerReply @{ ok = $true; phase = 'updated'; revision = $command.revision; mode = 'visible' }
      }
      'confirm' {
        if ($null -eq $state) { throw 'Visible composer is not ready' }
        Assert-ComposerHasText $state
        Invoke-SendButton $state
        Write-ComposerReply @{ ok = $true; phase = 'submitted'; revision = $command.revision; mode = 'visible' }
        break
      }
      'release' {
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
    Write-ComposerReply @{
      ok = $false
      phase = if ($null -ne $command) { [string]$command.kind } else { 'error' }
      mode = 'fallback'
      error = $_.Exception.Message
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

        let composer_job = match WindowsComposerJob::new() {
            Ok(job) => job,
            Err(error) => {
                let _ = fs::remove_file(&script_path);
                return Err(error);
            }
        };

        let mut child = match hidden_powershell()
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
                ));
            }
        };
        if let Err(error) = composer_job.assign(&child) {
            let _ = child.kill();
            let _ = child.wait();
            let _ = fs::remove_file(&script_path);
            return Err(error);
        }
        let mut stdin = child
            .stdin
            .take()
            .ok_or_else(|| "Visible Agent composer bridge has no stdin".to_string())?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| "Visible Agent composer bridge has no stdout".to_string())?;
        let child = Arc::new(Mutex::new(child));

        let callback: Arc<dyn Fn(CodexComposerEvent) + Send + Sync> = Arc::new(callback);
        let failed = Arc::new(AtomicBool::new(false));
        let closed = Arc::new(AtomicBool::new(false));
        let (sender, receiver) = mpsc::channel::<ComposerCommand>();
        let worker_callback = callback.clone();
        let worker_failed = failed.clone();
        let worker_closed = closed.clone();
        let worker_child = child.clone();
        let startup_stage = Arc::new(Mutex::new("启动 Windows 输入桥"));
        let worker_startup_stage = startup_stage.clone();
        let worker_job = composer_job;
        thread::Builder::new()
            .name("pet-codex-visible-composer".to_string())
            .spawn(move || {
                let _worker_job = worker_job;
                let mut stdout = BufReader::new(stdout);
                let mut pending_command = None;
                while let Some(mut command) =
                    receive_latest_composer_command(&receiver, &mut pending_command)
                {
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
                            if composer_command_failure_is_fatal(&command) {
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
                    if command.payload.get("kind").and_then(Value::as_str) == Some("release") { break; }
                }
                worker_closed.store(true, Ordering::SeqCst);
                if let Ok(mut child) = worker_child.lock() {
                    let _ = child.kill();
                    let _ = child.wait();
                }
                let _ = fs::remove_file(&script_path);
            })
            .map_err(|error| {
                if let Ok(mut child) = child.lock() {
                    let _ = child.kill();
                    let _ = child.wait();
                }
                format!("failed to start visible Agent composer worker: {error}")
            })?;

        let bridge = Self {
            sender,
            failed,
            closed,
        };
        let desktop_session_id = if agent == "claude" { deep_link } else { "" };
        let navigation_deep_link = if agent == "codex" { deep_link } else { "" };
        let (ready_sender, ready_receiver) = mpsc::channel();
        bridge
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
            .map_err(|_| "Visible Agent composer bridge closed during startup".to_string())?;
        match ready_receiver.recv_timeout(std::time::Duration::from_secs(
            CODEX_COMPOSER_STARTUP_TIMEOUT_SECS,
        )) {
            Ok(Ok(_)) => Ok(bridge),
            Ok(Err(error)) => Err(error),
            Err(mpsc::RecvTimeoutError::Timeout) => {
                if let Ok(mut child) = child.lock() {
                    let _ = child.kill();
                    let _ = child.wait();
                }
                let stage = startup_stage.lock().map(|stage| *stage)
                    .unwrap_or("读取输入框状态");
                Err(composer_startup_timeout_message(agent, stage))
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                Err("Visible Agent composer closed during startup".to_string())
            }
        }
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
        self.cancel();
    }
}

#[cfg(target_os = "macos")]
impl CodexComposerBridge {
    pub fn is_agent_frontmost(agent_id: &str) -> bool {
        let agent = match agent_id.trim() {
            "codex" => macos::MacosAgent::Codex,
            "claude-code" => macos::MacosAgent::Claude,
            "workbuddy" => macos::MacosAgent::WorkBuddy,
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
            _ => {
                return Err(
                    "Current visible composer requires ChatGPT（Codex）, Claude or WorkBuddy".to_string(),
                )
            }
        };
        Self::start_with_purpose(agent, "", "", "", "", "current_voice", callback)
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
        use std::sync::atomic::{AtomicBool, Ordering};
        use std::sync::{mpsc, Arc};
        use std::thread;

        let session_id = session_id.trim();
        let session_title = session_title.trim();
        let current_visible = purpose == "current_voice";
        if !current_visible && session_id.is_empty() && session_title.is_empty() {
            return Err("Visible composer requires a bound task ID or title".to_string());
        }
        if !matches!(purpose, "voice" | "locate" | "current_voice") {
            return Err("Visible composer purpose is invalid".to_string());
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
        thread::Builder::new()
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
        };
        let (ready_sender, ready_receiver) = mpsc::channel();
        bridge
            .sender
            .send(ComposerCommand {
                payload: json!({ "kind": "begin" }),
                started: None,
                response: Some(ready_sender),
            })
            .map_err(|_| "Visible Agent composer bridge closed during startup".to_string())?;
        match ready_receiver.recv_timeout(std::time::Duration::from_secs(
            CODEX_COMPOSER_STARTUP_TIMEOUT_SECS,
        )) {
            Ok(Ok(_)) => Ok(bridge),
            Ok(Err(error)) => Err(error),
            Err(mpsc::RecvTimeoutError::Timeout) => {
                Err("Visible Agent composer startup timed out".to_string())
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                Err("Visible Agent composer closed during startup".to_string())
            }
        }
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
    pub fn is_agent_frontmost(_agent_id: &str) -> bool {
        false
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
        let bridge = CodexComposerBridge {
            sender, failed: Arc::new(AtomicBool::new(false)), closed: Arc::new(AtomicBool::new(false)),
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
        bridge.update(1, "部分").unwrap();
        bridge.update(2, "完整的一句话").unwrap();
        bridge.preserve_draft().unwrap();
        assert!(bridge.update(3, "迟到的旧结果").is_err());
        drop(bridge);
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
        assert!(source.contains("let worker_child = child.clone();"));
    }

    #[cfg(windows)]
    #[test]
    fn windows_claude_command_hint_is_treated_as_an_empty_composer() {
        let source = include_str!("codex_composer.rs");
        assert!(source.contains("type / for commands"));
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
        assert!(source.contains("let _worker_job = worker_job;"));

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
