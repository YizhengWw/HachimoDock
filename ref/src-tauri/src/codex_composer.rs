/*
 * [Input] A bound or current-visible ChatGPT（Codex）/Claude session, or a captured MiMoCode terminal caret, plus voice text.
 * [Output] Exact desktop-session navigation, pinned current-visible delivery, and macOS current-caret insertion plus Return.
 * [Pos] Cross-platform foreground input bridge with session, draft, clipboard, and stale-focus recovery.
 * [Sync] If this file changes, update ref/.folder.md.
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
pub fn insert_and_submit_at_focused_text_target(
    target: &FocusedTextTarget,
    text: &str,
) -> Result<(), String> {
    macos::insert_and_submit_at_focused_text_target(&target.0, text)
}

#[cfg(windows)]
const CODEX_COMPOSER_STARTUP_TIMEOUT_SECS: u64 = 7;

#[cfg(windows)]
const WINDOWS_COMPOSER_PROCESS_MEMORY_LIMIT_BYTES: usize = 512 * 1024 * 1024;

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
    if composer_command_kind(&command) != "update" {
        return Some(command);
    }

    while let Ok(next) = receiver.try_recv() {
        if composer_command_kind(&next) == "update" {
            command = next;
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

#[cfg(windows)]
impl CodexComposerBridge {
    pub fn start_current(
        agent_id: &str,
        callback: impl Fn(CodexComposerEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        let agent = match agent_id.trim() {
            "codex" => "codex",
            "claude-code" => "claude",
            _ => {
                return Err(
                    "Current visible composer requires ChatGPT（Codex） or Claude".to_string(),
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
        use std::io::{BufRead, BufReader, Write};
        use std::process::Stdio;
        use std::sync::atomic::{AtomicBool, Ordering};
        use std::sync::{mpsc, Arc, Mutex};
        use std::thread;

        let agent = agent.trim();
        let session_id = session_id.trim();
        let deep_link = deep_link.trim();
        let session_title = session_title.trim();
        if !matches!(agent, "codex" | "claude") {
            return Err("visible composer agent is invalid".to_string());
        }
        let current_visible = purpose == "current_voice";
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
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
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
"@

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
      if (-not $mainRoot.Current.IsOffscreen -and
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
        if ($rect.Width -lt 420 -or $rect.Height -lt 320) { continue }
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

function Get-OrLaunchClaudeWindows {
  $windows = @(Get-ClaudeWindows)
  if ($windows.Count -gt 0) { return $windows }

  $desktopProcesses = @(Get-Process -Name claude -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 })
  foreach ($process in $desktopProcesses) {
    [void][CodexVoiceNative]::ActivateWindow([IntPtr]$process.MainWindowHandle)
  }
  if ($desktopProcesses.Count -gt 0) {
    Start-Sleep -Milliseconds 180
    $windows = @(Get-ClaudeWindows)
    if ($windows.Count -gt 0) { return $windows }
  }

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
  $existingWindows = @(Get-ClaudeWindows)
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

function Get-ComposerText($element) {
  $pattern = $null
  $value = ''
  if ($element.TryGetCurrentPattern(
      [System.Windows.Automation.ValuePattern]::Pattern,
      [ref]$pattern
  )) {
    $value = Normalize-Label ([string]$pattern.Current.Value)
  } elseif ($element.TryGetCurrentPattern(
      [System.Windows.Automation.TextPattern]::Pattern,
      [ref]$pattern
  )) {
    $value = Normalize-Label ([string]$pattern.DocumentRange.GetText(-1))
  } else {
    throw 'Visible composer exposes neither ValuePattern nor TextPattern'
  }
  if ($value -match '^(?i:随心输入|输入消息|message codex|ask anything|write your prompt to claude|type / for commands|write a message\W*|send a message\W*)$') { $value = '' }
  return @($pattern, $value)
}

function Test-AllowedComposerValue([string]$value, [string[]]$allowedValues) {
  $normalizedValue = Normalize-Label $value
  foreach ($allowedValue in @($allowedValues)) {
    if ($normalizedValue -eq (Normalize-Label ([string]$allowedValue))) { return $true }
  }
  return $false
}

function Find-BoundedComposerFallbackElements($root) {
  $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
  $pending = [System.Collections.Generic.Stack[object]]::new()
  $elements = [System.Collections.Generic.List[object]]::new()
  $deadline = (Get-MonotonicMilliseconds) + 1800
  $visited = 0
  try {
    $first = $walker.GetFirstChild($root)
    if ($null -ne $first) {
      $pending.Push([pscustomobject]@{ Element = $first; Depth = 1 })
    }
  } catch {}

  while ($pending.Count -gt 0 -and
         $visited -lt 3500 -and
         (Get-MonotonicMilliseconds) -lt $deadline) {
    $node = $pending.Pop()
    $element = $node.Element
    $depth = [int]$node.Depth
    $visited += 1
    try {
      $typeId = $element.Current.ControlType.Id
      if ($typeId -eq [System.Windows.Automation.ControlType]::Group.Id -or
          $typeId -eq [System.Windows.Automation.ControlType]::Custom.Id -or
          $typeId -eq [System.Windows.Automation.ControlType]::Document.Id -or
          $typeId -eq [System.Windows.Automation.ControlType]::Pane.Id) {
        [void]$elements.Add($element)
      }
    } catch {}

    try {
      $next = $walker.GetNextSibling($element)
      if ($null -ne $next) {
        $pending.Push([pscustomobject]@{ Element = $next; Depth = $depth })
      }
    } catch {}
    if ($depth -ge 48) { continue }
    try {
      $child = $walker.GetFirstChild($element)
      if ($null -ne $child) {
        $pending.Push([pscustomobject]@{ Element = $child; Depth = $depth + 1 })
      }
    } catch {}
  }
  return @($elements.ToArray())
}

function Get-ComposerCandidates($root, $elements, [string[]]$allowedValues) {
  $rootRect = $root.Current.BoundingRectangle
  if (-not (Test-FiniteWindowRectangle $rootRect)) {
    throw 'Visible Agent window geometry is unavailable'
  }
  $candidates = @()
  $unexpectedCandidates = @()
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
    $typeHint = $typeId -eq [System.Windows.Automation.ControlType]::Edit.Id -or
      $typeId -eq [System.Windows.Automation.ControlType]::Group.Id -or
      $typeId -eq [System.Windows.Automation.ControlType]::Custom.Id
    if (-not $classHint -and -not $nameHint -and -not $typeHint) { continue }

    $valueInfo = $null
    try { $valueInfo = Get-ComposerText $element } catch { continue }
    $score = 0
    if ($classHint) { $score += 8 }
    if ($nameHint) { $score += 6 }
    if ($typeId -eq [System.Windows.Automation.ControlType]::Edit.Id) { $score += 5 }
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
  }
}

function Find-Composer($root, [string[]]$allowedValues) {
  $editCondition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Edit
  )
  $editElements = $root.FindAll(
    [System.Windows.Automation.TreeScope]::Descendants,
    $editCondition
  )
  $candidateSet = Get-ComposerCandidates $root $editElements $allowedValues
  $candidates = @($candidateSet.Candidates)
  $unexpectedCandidates = @($candidateSet.UnexpectedCandidates)
  if ($candidates.Count -eq 0 -and $unexpectedCandidates.Count -eq 0) {
    $fallbackElements = @(Find-BoundedComposerFallbackElements $root)
    $candidateSet = Get-ComposerCandidates $root $fallbackElements $allowedValues
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

function Get-CurrentVisibleTarget([string]$agent, [string[]]$allowedValues) {
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

function Assert-TargetCurrent($state, [bool]$checkSession) {
  [void](Get-Process -Id $state.ProcessId -ErrorAction Stop)
  if (-not [CodexVoiceNative]::IsWindow([IntPtr]$state.WindowHandle) -or
      $state.Root.Current.NativeWindowHandle -ne $state.RootHandle) {
    throw 'Visible Agent window changed during voice input'
  }
  if ($state.CurrentVisible) {
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
  if ($currentValue -ne $state.LastValue) {
    throw 'Visible composer was edited outside voice input; voice updates were cancelled'
  }
  $state.Pattern = $currentInfo[0]
}

function Focus-Composer($state) {
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
      if ($updatedValue -eq $expectedText) {
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
  $normalizedText = Normalize-Label $text
  if ($normalizedText -eq $state.LastValue) { return }
  $lastError = ''
  for ($attempt = 0; $attempt -lt 2; $attempt += 1) {
    try {
      if ($attempt -gt 0) {
        Rebind-CodexTarget $state @($state.LastValue, $normalizedText)
        if ($state.LastValue -eq $normalizedText) { return }
      }
      $now = Get-MonotonicMilliseconds
      $checkSession = $forceSessionCheck -or (($now - $state.LastSessionCheck) -ge 900)
      Assert-TargetCurrent $state $checkSession
      if ($checkSession) { $state.LastSessionCheck = $now }
      Focus-Composer $state
      [CodexVoiceNative]::ReplaceFocusedText($normalizedText)
      # Chromium can block a synchronous TextPattern read while committing the
      # just-injected ProseMirror update. Let the accessibility tree settle.
      Start-Sleep -Milliseconds 120
      if (-not (Wait-ComposerText $state $normalizedText 350)) {
        throw 'Visible composer did not confirm the voice transcript in time'
      }
      $state.LastValue = $normalizedText
      return
    } catch {
      $lastError = $_.Exception.Message
      if ($attempt -lt 1) { Start-Sleep -Milliseconds 40 }
    }
  }
  throw "Visible composer update failed after retry: $lastError"
}

function Clear-ComposerVoiceText($state) {
  if ([string]::IsNullOrEmpty($state.LastValue)) { return }
  Assert-TargetCurrent $state (-not [bool]$state.CurrentVisible)
  Focus-Composer $state
  [CodexVoiceNative]::ReplaceFocusedText('')
  $state.LastValue = ''
}

function Invoke-SendButton($state) {
  $composerRect = $state.Composer.Current.BoundingRectangle
  if (-not (Test-FiniteWindowRectangle $composerRect)) {
    throw 'Visible composer geometry is unavailable for submission'
  }
  $all = Find-DescendantsByControlTypes $state.Root @(
    [System.Windows.Automation.ControlType]::Button
  )
  $buttons = @()
  for ($index = 0; $index -lt $all.Count; $index += 1) {
    $element = $all.Item($index)
    if ($element.Current.ControlType.Id -ne [System.Windows.Automation.ControlType]::Button.Id -or
        -not $element.Current.IsEnabled -or $element.Current.IsOffscreen) { continue }
    $name = Normalize-Label $element.Current.Name
    if ($name -notmatch '(?i)send|submit|发送|提交') { continue }
    $rect = $element.Current.BoundingRectangle
    if (-not (Test-FiniteWindowRectangle $rect)) { continue }
    if ($rect.Bottom -lt ($composerRect.Top - 12) -or $rect.Top -gt ($composerRect.Bottom + 80)) { continue }
    if ($rect.Right -lt ($composerRect.Left - 12) -or $rect.Left -gt ($composerRect.Right + 120)) { continue }
    $invoke = $null
    if (-not $element.TryGetCurrentPattern(
        [System.Windows.Automation.InvokePattern]::Pattern,
        [ref]$invoke
    )) { continue }
    $buttons += [pscustomobject]@{ Pattern = $invoke; Left = [double]$rect.Left }
  }
  if ($buttons.Count -eq 1) {
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
    [CodexVoiceNative]::ReplaceFocusedText('')
    throw 'Visible composer did not accept Enter submission while the send button was unavailable'
  }
}
$state = $null
while ($null -ne ($line = [Console]::In.ReadLine())) {
  if ([string]::IsNullOrWhiteSpace($line)) { continue }
  $command = $null
  try {
    $command = $line | ConvertFrom-Json
    switch ([string]$command.kind) {
      'begin' {
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
          Get-CurrentVisibleTarget $agent @('')
        } elseif ($agent -eq 'claude') {
          Get-ClaudeTarget $sessionId $desktopSessionId $title @('')
        } elseif ($null -ne $locatedCodexRoot) {
          [pscustomobject]@{
            Root = $locatedCodexRoot
            Composer = Find-Composer $locatedCodexRoot @('')
          }
        } else {
          Get-CodexTarget $title @('')
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
      'submit' {
        if ($null -eq $state) { throw 'Visible composer is not ready' }
        Set-ComposerText $state ([string]$command.text) $true
        Start-Sleep -Milliseconds 80
        Invoke-SendButton $state
        Write-ComposerReply @{ ok = $true; phase = 'submitted'; revision = $command.revision; mode = 'visible' }
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
                        let mut line = String::new();
                        stdout.read_line(&mut line).map_err(|error| {
                            format!("failed to read visible Agent composer response: {error}")
                        })?;
                        if line.trim().is_empty() {
                            return Err("Visible Agent composer bridge closed without a response"
                                .to_string());
                        }
                        let value = serde_json::from_str::<Value>(&line).map_err(|error| {
                            format!("invalid visible Agent composer response: {error}")
                        })?;
                        if value.get("ok").and_then(Value::as_bool) == Some(false) {
                            return Err(value
                                .get("error")
                                .and_then(Value::as_str)
                                .unwrap_or("Visible Agent composer update failed")
                                .to_string());
                        }
                        Ok(value)
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
                Err(format!(
                    "{} visible composer startup timed out",
                    if agent == "claude" {
                        "Claude"
                    } else {
                        "ChatGPT（Codex）"
                    }
                ))
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

    pub fn submit(&self, revision: u64, text: &str) -> CodexComposerSubmission {
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
                payload: json!({ "kind": "submit", "revision": revision, "text": text }),
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
            _ => {
                return Err(
                    "Current visible composer requires ChatGPT（Codex） or Claude".to_string(),
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
                        "submit" => state
                            .as_mut()
                            .ok_or_else(|| "Visible composer is not ready".to_string())
                            .and_then(|state| macos::submit_voice(state, &text))
                            .map(|composer_value| {
                                json!({
                                    "ok": true,
                                    "phase": "submitted",
                                    "revision": revision,
                                    "mode": "visible",
                                    "composerValue": composer_value,
                                })
                            }),
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
                    if matches!(kind.as_str(), "submit" | "cancel")
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

    pub fn submit(&self, revision: u64, text: &str) -> CodexComposerSubmission {
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
                payload: json!({ "kind": "submit", "revision": revision, "text": text }),
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

    pub fn submit(&self, _revision: u64, _text: &str) -> CodexComposerSubmission {
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
    use std::time::Duration;

    fn command(kind: &str, revision: u64) -> ComposerCommand {
        ComposerCommand {
            payload: json!({ "kind": kind, "revision": revision }),
            started: None,
            response: None,
        }
    }

    #[test]
    fn live_update_failure_does_not_disable_final_submission() {
        assert!(!composer_command_failure_is_fatal(&command("update", 1)));
        assert!(!composer_command_failure_is_fatal(&command("submit", 2)));
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
        assert!(source.contains("Composer = Find-Composer $locatedCodexRoot @('')"));
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
        assert!(source.contains("$existingWindows = @(Get-ClaudeWindows)"));
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
        assert_eq!(CODEX_COMPOSER_STARTUP_TIMEOUT_SECS, 7);
        assert_eq!(
            WINDOWS_COMPOSER_PROCESS_MEMORY_LIMIT_BYTES,
            512 * 1024 * 1024
        );

        let source = include_str!("codex_composer.rs");
        assert!(source.contains("function Get-OrLaunchCodexWindows"));
        assert!(source.contains("OpenAI.Codex_2p2nqsd0c76g0!App"));
        assert!(source.contains("function Find-BoundedComposerFallbackElements"));
        assert!(source.contains("$visited -lt 3500"));
        assert!(source.contains("(Get-MonotonicMilliseconds) + 1800"));
        assert!(source.contains("$depth -ge 48"));
        assert!(source.contains("JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE"));
        assert!(source.contains("JOB_OBJECT_LIMIT_PROCESS_MEMORY"));
        assert!(source.contains("JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK"));
        assert!(source.contains("let _worker_job = worker_job;"));

        let fallback_start = source
            .find("function Find-BoundedComposerFallbackElements")
            .unwrap();
        let fallback_end = source[fallback_start..]
            .find("function Get-ComposerCandidates")
            .map(|offset| fallback_start + offset)
            .unwrap();
        let fallback = &source[fallback_start..fallback_end];
        assert!(fallback.find("GetNextSibling").unwrap() < fallback.find("$depth -ge 48").unwrap());

        let locator_start = source.find("function Find-Composer(").unwrap();
        let locator_end = source[locator_start..]
            .find("function Get-CodexTarget")
            .map(|offset| locator_start + offset)
            .unwrap();
        let locator = &source[locator_start..locator_end];
        assert!(locator.contains("[System.Windows.Automation.ControlType]::Edit"));
        assert!(!locator.contains("Find-DescendantsByControlTypes"));
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
        assert!(source.contains("Get-CurrentVisibleTarget $agent @('')"));
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
    #[ignore = "requires a running, visible Codex window with an empty composer"]
    fn windows_current_visible_composer_smoke_test() {
        let bridge = CodexComposerBridge::start_current("codex", |_| {})
            .expect("current visible Codex composer should be addressable");
        bridge.cancel();
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires an installed Claude Desktop client with an empty current composer"]
    fn windows_current_visible_claude_composer_smoke_test() {
        let bridge = CodexComposerBridge::start_current("claude-code", |_| {})
            .expect("current visible Claude composer should be addressable");
        bridge.cancel();
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires CODEX_TEST_SESSION_ID/TITLE and a running Codex window with an empty composer"]
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
    fn live_updates_are_coalesced_before_the_pending_submit() {
        let (sender, receiver) = std::sync::mpsc::channel();
        sender.send(command("update", 1)).unwrap();
        sender.send(command("update", 2)).unwrap();
        sender.send(command("update", 3)).unwrap();
        sender.send(command("submit", 4)).unwrap();
        let mut pending = None;

        let latest = receive_latest_composer_command(&receiver, &mut pending).unwrap();
        assert_eq!(composer_command_kind(&latest), "update");
        assert_eq!(latest.payload["revision"], 3);

        let submit = receive_latest_composer_command(&receiver, &mut pending).unwrap();
        assert_eq!(composer_command_kind(&submit), "submit");
        assert_eq!(submit.payload["revision"], 4);
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
