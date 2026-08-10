/*
 * [Input] Tauri commands invoked by the React Pet Manager client.
 * [Output] Desktop runtime services for device pairing, single-instance bridge management,
 *          local agent discovery, atomic all-Agent petui Skill replacement,
 *          Codex pet import, external/community help links,
 *          controlled Codex Pets CLI installs, exact-board USB-only device
 *          follow-source binding,
 *          stale-state-safe, bounded local-file USB-first forwarding with current-followed-Agent
 *          daily Token preference, SSH state fallback, and
 *          active speech sync plus immediate reconnect replay, a P4 host
 *          heartbeat, and verified USB binding refresh, built-in
 *          appearance default/override WAV cue sync plus P4 cached-slot reuse,
 *          persistent USB transfer diagnostics, and serialized, exact-board
 *          native-only appearance attempts,
 *          USB desktop identity propagation,
 *          foreground-Agent-first / device-bubble-fallback visible-composer
 *          voice drafts consumed only by an explicit same-board global Confirm action,
 *          formal local component latest-version listing/deletion with game/tool kind
 *          and manifest descriptions for component-center card summaries, without a manual import command,
 *          .clawpkg USB/SSH installs with per-component button-function
 *          overrides plus explicit target-bound transactional removal and
 *          live USB/SSH installed-component inventory,
 *          runtime-aware Linux/P4 input configuration with
 *          transfer-serialized reads/writes, backend-held ACK confirmation,
 *          client-authoritative reconciliation, and configurable P4
 *          two-page/enter/back navigation,
 *          P4 hardware Agent-event injection,
 *          Agent-switch-isolated selected-Session title/position/display-enable
 *          and active-ID sync to the P4 display plus
 *          explicit-action-only Codex/Claude Desktop task navigation,
 *          and stale USB writer reconnect retry,
 *          ACK-gated ESP32-P4 A/B firmware OTA with SHA-256 verification,
 *          a version-guarded bundled-image update path, and desktop progress events,
 *          validated P4 device-microphone PCM with capture-start frozen
 *          Agent/Session routing, utterance-correlated delivery events,
 *          single-claim final recognition, cloud speech recognition, and
 *          prompt-free owner-only macOS ASR credential-file initialization,
 *          activation-gated, draft-replacing, AX-node-rebindable live/final Codex/Claude visible-composer synchronization
 *          plus macOS MiMoCode current-caret draft insertion, with Return/send reserved for the device Confirm action,
 *          with non-prompting macOS Accessibility diagnostics and native system-consent requests at startup and protected operations,
 *          without background fallback, managed bridge-only non-visible-agent voice injection, stale
 *          LaunchAgent/legacy bridge cleanup with install-relative Node resources
 *          and user PATH propagation for CLI shims, environment-relative coding-agent
 *          discovery, credential-preserving partial bridge-profile updates,
 *          selected-agent adapter health self-restart, creation-time-sorted component
 *          draft discovery, immutable app-local component sync snapshots, and
 *          packaged bridge assets.
 * [Pos] Tauri runtime node in ref/src-tauri/src
 * [Sync] If this file changes, update `ref/.folder.md`.
 */

mod clawpkg;
mod codex_composer;
mod codex_import;
mod component_library;
mod pc_audio;
mod usb_audio;
mod usb_serial;
mod volcengine_asr;

use serde::{Deserialize, Serialize};
use std::cmp::Ordering as VersionOrdering;
use std::collections::{HashMap, HashSet};
use std::env;
use std::fs;
use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream, UdpSocket};
#[cfg(unix)]
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
#[cfg(unix)]
use std::os::unix::process::CommandExt;
#[cfg(windows)]
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Condvar, Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tauri::{Emitter, Manager, RunEvent};

const DEVICE_AP_SSID: &str = "claw-pet";
const DEVICE_AP_PASSWORD: &str = "88888888";
const DEVICE_AP_HOST: &str = "192.168.44.1";
const DEVICE_AP_PORT: u16 = 80;
const DESKTOP_DEVICE_ID_FILE_NAME: &str = "desktop-device-id";
const DEVICE_BINDINGS_FILE_NAME: &str = "device-bindings.json";

const BRIDGE_PROFILE_FILE_NAME: &str = "pet-bridge.json";
const PET_SCREENS_FILE_NAME: &str = "pet-screens.json";
const DEFAULT_NAMESPACE: &str = "desk";
const DEFAULT_DESKTOP_DEVICE_ID: &str = "linux-pet-01";
const DEFAULT_MQTT_URL: &str = "mqtt://broker.openclaw.example:1883";
const DEFAULT_MQTT_USERNAME: &str = "device";
const BUNDLED_MQTT_URL: Option<&str> = option_env!("PET_MANAGER_BUNDLED_MQTT_URL");
const BUNDLED_MQTT_USERNAME: Option<&str> = option_env!("PET_MANAGER_BUNDLED_MQTT_USERNAME");
const BUNDLED_MQTT_PASSWORD: Option<&str> = option_env!("PET_MANAGER_BUNDLED_MQTT_PASSWORD");
const DEFAULT_PET_CHANNEL_ID: &str = "openclaw";
const DEFAULT_BRIDGE_PORT: u16 = 23333;
const DEFAULT_AGENT_BUS_PORT: u16 = 8181;
const CLAW_PET_DIR_NAME: &str = ".claw-pet";
const LEGACY_OPENCLAW_DIR_NAME: &str = ".openclaw";
const COMPONENT_SYNC_CACHE_DIR_NAME: &str = "component-sync-cache";
const LEGACY_BRIDGE_PORT: u16 = 23334;
const BUTTON_CONFIG_ACK_TIMEOUT_SECS: u64 = 12;
const BUTTON_CONFIG_ACK_TIMEOUT_MESSAGE: &str =
    "未收到板端按钮配置确认；设备端可能还没更新到支持 button-config-ack 的运行时，或板端未写入 .button-config。";
const DEFAULT_BOARD_RUNTIME_ROOT: &str = "/opt/board-runtime";
const SSH_STATE_FALLBACK_ERROR_LOG_MS: u64 = 10_000;
const SSH_STATE_FALLBACK_RETRY_MS: u64 = 30_000;
const USB_AUTO_RETRY_MIN_SECS: u64 = 5;
const USB_AUTO_RETRY_MAX_SECS: u64 = 60;
const P4_SESSION_TERMINAL_HOLD_MS: u64 = 60_000;
const JSON_SAFE_INTEGER_MAX: u64 = 9_007_199_254_740_991;
const BUNDLED_P4_FIRMWARE_RESOURCE: &str = "firmware/esp32-p4/firmware.bin";

fn desktop_build_info() -> serde_json::Value {
    serde_json::json!({
        "version": env!("CARGO_PKG_VERSION"),
        "buildId": env!("PET_MANAGER_BUILD_ID"),
        "gitSha": env!("PET_MANAGER_BUILD_GIT_SHA"),
        "dirty": env!("PET_MANAGER_BUILD_DIRTY") == "1",
        "protocolSchema": env!("PET_MANAGER_PROTOCOL_SCHEMA")
            .parse::<u32>()
            .unwrap_or(0),
    })
}

fn bundled_value(value: Option<&'static str>) -> Option<String> {
    value
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
}

fn default_mqtt_url() -> String {
    bundled_value(BUNDLED_MQTT_URL).unwrap_or_else(|| DEFAULT_MQTT_URL.to_string())
}

fn default_mqtt_username() -> String {
    bundled_value(BUNDLED_MQTT_USERNAME).unwrap_or_else(|| DEFAULT_MQTT_USERNAME.to_string())
}

fn default_mqtt_password() -> String {
    bundled_value(BUNDLED_MQTT_PASSWORD)
        .or_else(|| env::var("PET_CLAW_MQTT_PASSWORD").ok())
        .or_else(|| env::var("MQTT_PASSWORD").ok())
        .unwrap_or_default()
}

fn default_appearance_audio_cue_name(family: &str) -> Option<&'static str> {
    match family {
        "done" => Some("done.wav"),
        "error" => Some("error.wav"),
        "waiting_user" => Some("waiting_user.wav"),
        _ => None,
    }
}

fn ensure_default_appearance_audio_cues(
    appearance_dir: &Path,
    clips_dir: &Path,
) -> Result<(), String> {
    if !clips_dir.is_dir() {
        return Ok(());
    }
    let manifest_path = appearance_dir.join("manifest.json");
    if !manifest_path.is_file() {
        return Ok(());
    }
    let raw = fs::read_to_string(&manifest_path)
        .map_err(|e| format!("读取形象 manifest 失败 {}: {}", manifest_path.display(), e))?;
    let manifest: serde_json::Value = serde_json::from_str(&raw)
        .map_err(|e| format!("解析形象 manifest 失败 {}: {}", manifest_path.display(), e))?;
    let Some(families) = manifest.get("families").and_then(|v| v.as_array()) else {
        return Ok(());
    };
    let videos_dir = appearance_dir.join("videos");
    let _ = fs::create_dir_all(&videos_dir);
    for family in families {
        let ok = family.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
        if !ok || family.get("audioPath").and_then(|v| v.as_str()).is_some() {
            continue;
        }
        let Some(family_name) = family.get("family").and_then(|v| v.as_str()) else {
            continue;
        };
        let Some(cue_name) = default_appearance_audio_cue_name(family_name) else {
            continue;
        };
        let source = clips_dir.join(cue_name);
        if !source.is_file() {
            continue;
        }
        let dest = videos_dir.join(format!("{}.wav", family_name));
        if !dest.is_file() {
            let _ = fs::copy(source, dest);
        }
    }
    Ok(())
}

static BUTTON_CONFIG_ACK_WAITERS: OnceLock<
    Mutex<HashMap<String, mpsc::Sender<serde_json::Value>>>,
> = OnceLock::new();

fn button_config_ack_waiters() -> &'static Mutex<HashMap<String, mpsc::Sender<serde_json::Value>>> {
    BUTTON_CONFIG_ACK_WAITERS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn register_button_config_ack_waiter(
    request_id: &str,
) -> Result<mpsc::Receiver<serde_json::Value>, String> {
    let (sender, receiver) = mpsc::channel();
    let mut waiters = button_config_ack_waiters()
        .lock()
        .map_err(|_| "按钮配置确认等待队列已损坏".to_string())?;
    waiters.insert(request_id.to_string(), sender);
    Ok(receiver)
}

fn remove_button_config_ack_waiter(request_id: &str) {
    if let Ok(mut waiters) = button_config_ack_waiters().lock() {
        waiters.remove(request_id);
    }
}

fn resolve_button_config_ack(topic: &str, payload: &serde_json::Value) {
    if topic != "button-config-ack" && topic != "input/config-ack" {
        return;
    }

    let Some(request_id) = payload
        .get("requestId")
        .or_else(|| payload.get("request_id"))
        .and_then(|v| v.as_str())
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
    else {
        return;
    };

    let sender = button_config_ack_waiters()
        .lock()
        .ok()
        .and_then(|mut waiters| waiters.remove(&request_id));
    eprintln!(
        "[button-config] ack received requestId={} matched={}",
        request_id,
        sender.is_some()
    );
    if let Some(sender) = sender {
        let _ = sender.send(payload.clone());
    }
}

fn reconnect_usb_serial_for_command(
    app_handle: &tauri::AppHandle,
    usb_manager: &usb_serial::UsbSerialManager,
) -> Result<usb_serial::UsbConnectionStatus, String> {
    usb_manager.disconnect();
    thread::sleep(Duration::from_millis(250));
    let devices = usb_manager.scan_devices();
    if devices.is_empty() {
        return Err("USB 重新连接失败：未找到可用串口".to_string());
    }
    let mut failures = Vec::new();
    for device in devices {
        let port_name = device.port_name.clone();
        eprintln!(
            "[button-config] probing stale USB writer candidate {}",
            port_name
        );
        let emitter = app_handle.clone();
        match usb_manager.connect(&port_name, move |topic, payload| {
            handle_incoming_usb_message(&emitter, topic, payload);
        }) {
            Ok(()) => return Ok(usb_manager.status()),
            Err(error) => {
                failures.push(format!("{port_name}: {error}"));
                usb_manager.disconnect();
            }
        }
    }
    Err(format!(
        "USB 重新连接失败：没有串口通过 Pet Manager 协议握手（{}）",
        failures.join("；")
    ))
}

fn reconnect_usb_serial_to_expected_board(
    app_handle: &tauri::AppHandle,
    usb_manager: &usb_serial::UsbSerialManager,
    expected_board_device_id: &str,
) -> Result<(), String> {
    let expected_board_device_id = expected_board_device_id.trim();
    if expected_board_device_id.is_empty() {
        return Err("expectedBoardDeviceId is required".to_string());
    }

    usb_manager.disconnect();
    thread::sleep(Duration::from_millis(250));
    let devices = usb_manager.scan_devices();
    if devices.is_empty() {
        return Err(format!(
            "waiting for board {expected_board_device_id}: no USB serial device found"
        ));
    }

    let mut last_error = String::new();
    for device in devices {
        let emitter = app_handle.clone();
        if let Err(error) = usb_manager.connect(&device.port_name, move |topic, payload| {
            handle_incoming_usb_message(&emitter, topic, payload);
        }) {
            last_error = format!("{}: {error}", device.port_name);
            continue;
        }

        let hello_deadline = std::time::Instant::now() + Duration::from_secs(3);
        while std::time::Instant::now() < hello_deadline {
            let status = usb_manager.status();
            if !status.connected {
                break;
            }
            if !status.board_device_id.is_empty() {
                if status.board_device_id == expected_board_device_id {
                    return Ok(());
                }
                last_error = format!(
                    "{} identified as {}, expected {}",
                    device.port_name, status.board_device_id, expected_board_device_id
                );
                break;
            }
            thread::sleep(Duration::from_millis(100));
        }
        usb_manager.disconnect();
    }

    Err(if last_error.is_empty() {
        format!("waiting for board {expected_board_device_id} to reconnect")
    } else {
        format!("waiting for board {expected_board_device_id}: {last_error}")
    })
}

/// Build a reqwest blocking client that is *immune* to system / shell HTTP
/// proxy env vars (`HTTP_PROXY`, `HTTPS_PROXY`, `ALL_PROXY`, …).
///
/// All HTTP that flows through this binary today is loopback or LAN
/// (`127.0.0.1:23333` to the bridge sidecar, `192.168.44.1:80` to the
/// board's AP-mode HTTP server). `reqwest`'s default behaviour is to
/// honour `ALL_PROXY` regardless, and its `NO_PROXY` parser is stricter
/// than curl's — so a developer who runs a SOCKS/HTTP proxy in their
/// shell (e.g. `ALL_PROXY=http://127.0.0.1:63762`) ends up routing
/// loopback-to-loopback traffic through the proxy, which then refuses
/// to relay it back into 127.0.0.1 and we surface "error sending
/// request for url …". The user-visible symptom looked like the bridge
/// was down, when in fact only the HTTP transport was misrouted.
///
/// We never want to proxy these calls; force `.no_proxy()` everywhere.
fn lan_http_client(timeout: Duration) -> Result<reqwest::blocking::Client, String> {
    reqwest::blocking::Client::builder()
        .no_proxy()
        .timeout(timeout)
        .build()
        .map_err(|e| e.to_string())
}

fn resolve_usb_inject_agent_id() -> String {
    let profile = get_bridge_profile_path()
        .ok()
        .and_then(|path| read_bridge_profile(&path).ok().flatten())
        .map(|profile| apply_bridge_profile_defaults(normalize_bridge_profile(profile)))
        .unwrap_or_default();

    normalize_agent_id(&profile.selected_agent_id)
        .or_else(|| {
            profile
                .enabled_agents
                .iter()
                .find_map(|agent| normalize_agent_id(agent))
        })
        .unwrap_or_else(|| "codex".to_string())
}

fn extract_usb_voice_input_text(payload: &serde_json::Value) -> Option<String> {
    let view = payload.get("view").and_then(|v| v.as_str())?;
    if !view.trim().eq_ignore_ascii_case("voice_input") {
        return None;
    }
    let text = payload
        .get("state")
        .and_then(|v| v.as_str())
        .map(|v| v.trim().to_string())
        .unwrap_or_default();
    if text.is_empty() {
        None
    } else {
        Some(text)
    }
}

fn usb_audio_relay() -> &'static Mutex<usb_audio::UsbAudioRelay> {
    static RELAY: OnceLock<Mutex<usb_audio::UsbAudioRelay>> = OnceLock::new();
    RELAY.get_or_init(|| Mutex::new(usb_audio::UsbAudioRelay::default()))
}

fn pc_audio_capture() -> &'static Mutex<pc_audio::PcAudioCapture> {
    static CAPTURE: OnceLock<Mutex<pc_audio::PcAudioCapture>> = OnceLock::new();
    CAPTURE.get_or_init(|| Mutex::new(pc_audio::PcAudioCapture::default()))
}

#[derive(Debug, Clone, Default)]
struct PcAudioBoardBinding {
    board_device_id: String,
    generation: u64,
    active_capture_id: Option<u64>,
}

#[derive(Debug, Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
struct P4SessionBinding {
    board_device_id: String,
    agent_id: String,
    session_id: String,
    auto_follow: bool,
    session_title: String,
    session_cwd: String,
    session_title_unique: bool,
    desktop_location: String,
    desktop_location_error: String,
    generation: u64,
}

fn p4_session_agent_switch_required(
    current: &P4SessionBinding,
    board_device_id: &str,
    agent_id: &str,
) -> bool {
    current.board_device_id != board_device_id || current.agent_id != agent_id
}

fn reset_p4_session_binding_for_agent(
    current: &mut P4SessionBinding,
    board_device_id: &str,
    agent_id: &str,
) -> bool {
    if !p4_session_agent_switch_required(current, board_device_id, agent_id) {
        return false;
    }
    current.board_device_id = board_device_id.to_string();
    current.agent_id = agent_id.to_string();
    current.session_id.clear();
    current.auto_follow = false;
    current.session_title.clear();
    current.session_cwd.clear();
    current.session_title_unique = false;
    current.desktop_location = if agent_uses_visible_composer(agent_id) {
        "not_requested"
    } else {
        "not_applicable"
    }
    .to_string();
    current.desktop_location_error.clear();
    current.generation = current.generation.wrapping_add(1).max(1);
    true
}

fn default_true() -> bool {
    true
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SetP4SessionBindingInput {
    board_device_id: String,
    agent_id: String,
    session_id: String,
    #[serde(default)]
    auto_follow: bool,
    #[serde(default)]
    session_title: String,
    #[serde(default)]
    device_title: Option<String>,
    #[serde(default)]
    session_cwd: String,
    #[serde(default)]
    session_title_unique: bool,
    #[serde(default)]
    locate_desktop: bool,
    #[serde(default)]
    session_index: u32,
    #[serde(default)]
    session_count: u32,
    #[serde(default)]
    sessions: Vec<P4SessionQueueInput>,
    #[serde(default)]
    active_session_ids: Vec<String>,
    #[serde(default = "default_true")]
    display_enabled: bool,
    #[serde(default)]
    notice: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct P4SessionQueueInput {
    id: String,
    title: String,
    #[serde(default)]
    cwd: String,
    #[serde(default)]
    content: String,
    #[serde(default)]
    state: String,
    #[serde(default)]
    transition_revision: u64,
    #[serde(default)]
    terminal_remaining_ms: u64,
}

fn validate_p4_session_transition_metadata(
    state: &str,
    transition_revision: u64,
    terminal_remaining_ms: u64,
) -> Result<(), String> {
    if transition_revision > JSON_SAFE_INTEGER_MAX {
        return Err("设备会话转换版本号超出 JSON 安全整数范围".to_string());
    }
    if terminal_remaining_ms > P4_SESSION_TERMINAL_HOLD_MS {
        return Err("设备会话终态保留时间不能超过 60 秒".to_string());
    }
    if terminal_remaining_ms > 0 && transition_revision == 0 {
        return Err("设备会话终态保留时间缺少转换版本号".to_string());
    }
    let terminal = matches!(
        state,
        "done" | "error" | "failed" | "complete" | "completed"
    );
    if terminal_remaining_ms > 0 && !terminal {
        return Err("非终态设备会话不能携带终态保留时间".to_string());
    }
    Ok(())
}

fn p4_session_target_is_unique(
    session_title: &str,
    session_cwd: &str,
    sessions: &[P4SessionQueueInput],
) -> bool {
    if session_title.is_empty() {
        return false;
    }
    let title_matches = sessions
        .iter()
        .filter(|session| session.title.trim() == session_title)
        .collect::<Vec<_>>();
    if title_matches.len() <= 1 {
        return true;
    }
    !session_cwd.is_empty()
        && title_matches
            .iter()
            .filter(|session| session.cwd.trim() == session_cwd)
            .count()
            == 1
}

fn agent_uses_visible_composer(agent_id: &str) -> bool {
    matches!(agent_id, "codex" | "claude-code")
}

fn visible_composer_agent_label(agent_id: &str) -> &'static str {
    if agent_id == "claude-code" {
        "Claude"
    } else {
        "ChatGPT（Codex）"
    }
}

fn should_locate_desktop_session(locate_desktop: bool, agent_id: &str) -> bool {
    locate_desktop && agent_uses_visible_composer(agent_id)
}

#[tauri::command]
fn check_codex_accessibility_permission() -> serde_json::Value {
    #[cfg(target_os = "macos")]
    let trusted = codex_composer::CodexComposerBridge::accessibility_permission_granted();
    #[cfg(not(target_os = "macos"))]
    let trusted = true;

    serde_json::json!({
        "platform": if cfg!(target_os = "macos") {
            "macos"
        } else if cfg!(windows) {
            "windows"
        } else {
            "other"
        },
        "trusted": trusted,
    })
}

#[tauri::command]
fn request_codex_accessibility_permission() -> serde_json::Value {
    #[cfg(target_os = "macos")]
    let trusted = codex_composer::CodexComposerBridge::request_accessibility_permission();
    #[cfg(not(target_os = "macos"))]
    let trusted = true;

    serde_json::json!({
        "platform": if cfg!(target_os = "macos") {
            "macos"
        } else if cfg!(windows) {
            "windows"
        } else {
            "other"
        },
        "trusted": trusted,
    })
}

struct PcAudioRecognitionJob {
    emitter: tauri::AppHandle,
    pcm: Vec<u8>,
    board_device_id: String,
    generation: u64,
    capture_id: u64,
}

struct DeviceVoiceContext {
    emitter: tauri::AppHandle,
    utterance_id: String,
    board_device_id: String,
    target: P4SessionBinding,
    session_queue_empty_at_start: bool,
    use_current_visible_session: AtomicBool,
    final_requested: AtomicBool,
    final_handled: AtomicBool,
    draft_ready: AtomicBool,
    draft_submit_requested: AtomicBool,
    cancelled: AtomicBool,
    latest_revision: AtomicU64,
    latest_text: Mutex<String>,
    latest_confidence: Mutex<Option<f64>>,
    streaming_error: Mutex<String>,
    composer_error: Mutex<String>,
    composer_visible: AtomicBool,
    recognizer: Mutex<Option<pc_audio::StreamingSpeechRecognizer>>,
    composer: Mutex<Option<codex_composer::CodexComposerBridge>>,
    composer_startup_complete: Mutex<bool>,
    composer_startup_ready: Condvar,
    #[cfg(target_os = "macos")]
    focused_text_target: Mutex<Option<codex_composer::FocusedTextTarget>>,
}

const VISIBLE_COMPOSER_SUBMIT_TIMEOUT: Duration = Duration::from_secs(8);
const VISIBLE_COMPOSER_START_TIMEOUT: Duration = Duration::from_secs(3);
const VISIBLE_COMPOSER_PREPARE_WAIT: Duration = Duration::from_secs(8);

#[derive(Debug, PartialEq, Eq)]
enum VisibleComposerSubmitOutcome {
    Submitted,
    ExplicitFailure(String),
    Unconfirmed(String),
}

fn classify_visible_composer_submit(
    result: Result<Result<serde_json::Value, String>, codex_composer::CodexComposerWaitError>,
) -> VisibleComposerSubmitOutcome {
    match result {
        Ok(Ok(_)) => VisibleComposerSubmitOutcome::Submitted,
        Ok(Err(error))
            if error.contains("提交结果未确认") || error.contains("无法安全确认发送结果") =>
        {
            VisibleComposerSubmitOutcome::Unconfirmed(error)
        }
        Ok(Err(error)) => VisibleComposerSubmitOutcome::ExplicitFailure(error),
        Err(codex_composer::CodexComposerWaitError::StartTimeout) => {
            VisibleComposerSubmitOutcome::Unconfirmed(
                "Visible composer waited too long to start submission".to_string(),
            )
        }
        Err(codex_composer::CodexComposerWaitError::StartDisconnected) => {
            VisibleComposerSubmitOutcome::Unconfirmed(
                "Visible composer closed before submission started".to_string(),
            )
        }
        Err(codex_composer::CodexComposerWaitError::CompletionTimeout) => {
            VisibleComposerSubmitOutcome::Unconfirmed(
                "Visible composer submission timed out".to_string(),
            )
        }
        Err(codex_composer::CodexComposerWaitError::CompletionDisconnected) => {
            VisibleComposerSubmitOutcome::Unconfirmed(
                "Visible composer closed without confirming submission".to_string(),
            )
        }
    }
}

fn pc_audio_board_binding() -> &'static Mutex<PcAudioBoardBinding> {
    static BINDING: OnceLock<Mutex<PcAudioBoardBinding>> = OnceLock::new();
    BINDING.get_or_init(|| Mutex::new(PcAudioBoardBinding::default()))
}

fn p4_session_binding() -> &'static Mutex<P4SessionBinding> {
    static BINDING: OnceLock<Mutex<P4SessionBinding>> = OnceLock::new();
    BINDING.get_or_init(|| Mutex::new(P4SessionBinding::default()))
}

fn active_device_voice_context() -> &'static Mutex<Option<Arc<DeviceVoiceContext>>> {
    static CONTEXT: OnceLock<Mutex<Option<Arc<DeviceVoiceContext>>>> = OnceLock::new();
    CONTEXT.get_or_init(|| Mutex::new(None))
}

fn device_voice_target_snapshot(board_device_id: &str) -> P4SessionBinding {
    let board_device_id = board_device_id.trim();
    if let Ok(binding) = p4_session_binding().lock() {
        if !board_device_id.is_empty()
            && binding.board_device_id == board_device_id
            && !binding.agent_id.is_empty()
        {
            return binding.clone();
        }
    }
    let (agent_id, session_id) = resolve_usb_inject_target(board_device_id);
    P4SessionBinding {
        board_device_id: board_device_id.to_string(),
        agent_id,
        session_id: if session_id == "auto" {
            String::new()
        } else {
            session_id
        },
        session_title: String::new(),
        ..P4SessionBinding::default()
    }
}

fn audio_begin_session_queue_empty(payload: &serde_json::Value) -> bool {
    payload
        .get("sessionQueueEmpty")
        .and_then(serde_json::Value::as_bool)
        .unwrap_or(false)
}

fn should_use_current_visible_session(
    agent_is_frontmost: bool,
    session_queue_empty: bool,
    agent_id: &str,
) -> bool {
    agent_uses_visible_composer(agent_id) && (agent_is_frontmost || session_queue_empty)
}

fn device_voice_uses_current_visible_session(context: &DeviceVoiceContext) -> bool {
    context.use_current_visible_session.load(Ordering::SeqCst)
}

fn device_voice_bound_target_is_addressable(
    session_queue_empty: bool,
    target: &P4SessionBinding,
) -> bool {
    !session_queue_empty
        && !target.session_id.is_empty()
        && !target.session_title.is_empty()
        && (target.agent_id == "claude-code"
            || cfg!(target_os = "macos")
            || target.session_title_unique)
}

fn device_voice_binding_matches(target: &P4SessionBinding, binding: &P4SessionBinding) -> bool {
    binding.board_device_id == target.board_device_id
        && binding.agent_id == target.agent_id
        // Auto mode may follow a newer display session while an utterance is
        // active. Keep the exact composer target captured at audio/begin until
        // that utterance completes; an explicit session change still cancels.
        && ((target.auto_follow && binding.auto_follow)
            || (binding.session_id == target.session_id
                && binding.generation == target.generation))
}

fn should_preserve_exact_auto_binding(
    current: &P4SessionBinding,
    board_device_id: &str,
    agent_id: &str,
    session_id: &str,
    auto_follow: bool,
) -> bool {
    auto_follow
        && agent_uses_visible_composer(agent_id)
        && session_id.is_empty()
        && current.board_device_id == board_device_id
        && current.agent_id == agent_id
        && !current.session_id.is_empty()
}

fn device_voice_target_is_current(context: &DeviceVoiceContext) -> bool {
    if context.cancelled.load(Ordering::SeqCst) {
        return false;
    }
    if device_voice_uses_current_visible_session(context) {
        if let Ok(binding) = p4_session_binding().lock() {
            if binding.board_device_id == context.target.board_device_id
                && binding.agent_id == context.target.agent_id
            {
                return true;
            }
        }
        let (agent_id, _) = resolve_usb_inject_target(&context.board_device_id);
        return agent_id == context.target.agent_id;
    }
    #[cfg(target_os = "macos")]
    if context.target.agent_id == "mimocode" {
        if let Ok(binding) = p4_session_binding().lock() {
            if binding.board_device_id == context.target.board_device_id
                && binding.agent_id == context.target.agent_id
            {
                return true;
            }
        }
        if context.target.generation != 0 {
            return false;
        }
        let (agent_id, _) = resolve_usb_inject_target(&context.board_device_id);
        return agent_id == context.target.agent_id;
    }
    if let Ok(binding) = p4_session_binding().lock() {
        if device_voice_binding_matches(&context.target, &binding) {
            return true;
        }
    }
    if context.target.generation != 0 {
        return false;
    }
    let (agent_id, session_id) = resolve_usb_inject_target(&context.board_device_id);
    agent_id == context.target.agent_id
        && session_id
            == if context.target.session_id.is_empty() {
                "auto"
            } else {
                context.target.session_id.as_str()
            }
}

fn p4_session_binding_inject_session_id(target: &P4SessionBinding) -> &str {
    if target.session_id.is_empty() {
        "auto"
    } else {
        target.session_id.as_str()
    }
}

fn device_voice_session_id(context: &DeviceVoiceContext) -> &str {
    if device_voice_uses_current_visible_session(context) {
        "current"
    } else {
        p4_session_binding_inject_session_id(&context.target)
    }
}

fn frozen_device_voice_inject_target(
    voice_utterance_id: &str,
    board_device_id: &str,
    active_utterance_id: &str,
    active_board_device_id: &str,
    target: &P4SessionBinding,
    cancelled: bool,
) -> Option<(String, String)> {
    if voice_utterance_id.is_empty()
        || cancelled
        || voice_utterance_id != active_utterance_id
        || board_device_id != active_board_device_id
    {
        return None;
    }
    Some((
        target.agent_id.clone(),
        p4_session_binding_inject_session_id(target).to_string(),
    ))
}

fn claim_device_voice_final(final_handled: &AtomicBool) -> bool {
    !final_handled.swap(true, Ordering::SeqCst)
}

fn device_voice_composer_mode(context: &DeviceVoiceContext) -> &'static str {
    #[cfg(target_os = "macos")]
    if context.target.agent_id == "mimocode" {
        return if context
            .focused_text_target
            .lock()
            .map(|target| target.is_some())
            .unwrap_or(false)
        {
            "focused-input"
        } else {
            "unavailable"
        };
    }
    if !agent_uses_visible_composer(&context.target.agent_id) {
        "agent-bus"
    } else if context.composer_visible.load(Ordering::SeqCst) {
        "visible"
    } else {
        "unavailable"
    }
}

fn emit_device_voice_transcript(
    context: &DeviceVoiceContext,
    phase: &str,
    revision: u64,
    text: &str,
    is_final: bool,
    ok: bool,
    error: &str,
) {
    let composer_error = context
        .composer_error
        .lock()
        .map(|value| value.clone())
        .unwrap_or_default();
    let confidence = context
        .latest_confidence
        .lock()
        .map(|value| *value)
        .unwrap_or(None);
    let event = serde_json::json!({
        "utteranceId": context.utterance_id,
        "boardDeviceId": context.board_device_id,
        "agentId": context.target.agent_id,
        "sessionId": device_voice_session_id(context),
        "sessionQueueEmpty": context.session_queue_empty_at_start,
        "revision": revision,
        "text": text,
        "isFinal": is_final,
        "phase": phase,
        "ok": ok,
        "confidence": confidence,
        "composerMode": device_voice_composer_mode(context),
        "composerError": composer_error,
        "error": error,
    });
    let _ = context.emitter.emit("voice-transcript", event.clone());
    let _ = context.emitter.emit("usb-audio-stream", event);
}

fn complete_device_voice_context(context: &Arc<DeviceVoiceContext>) {
    if let Ok(mut active) = active_device_voice_context().lock() {
        if active
            .as_ref()
            .is_some_and(|current| Arc::ptr_eq(current, context))
        {
            *active = None;
        }
    }
}

fn mark_visible_composer_startup_complete(context: &DeviceVoiceContext) {
    if let Ok(mut complete) = context.composer_startup_complete.lock() {
        *complete = true;
    }
    context.composer_startup_ready.notify_all();
}

fn wait_for_visible_composer_startup(context: &DeviceVoiceContext) {
    let Ok(complete) = context.composer_startup_complete.lock() else {
        return;
    };
    if *complete {
        return;
    }
    drop(
        context
            .composer_startup_ready
            .wait_timeout(complete, VISIBLE_COMPOSER_PREPARE_WAIT),
    );
}

fn stop_device_voice_context(
    context: &Arc<DeviceVoiceContext>,
    reason: &str,
    emit_cancelled: bool,
) {
    if context.cancelled.swap(true, Ordering::SeqCst) {
        return;
    }
    context.composer_startup_ready.notify_all();
    if let Ok(recognizer) = context.recognizer.lock() {
        if let Some(recognizer) = recognizer.as_ref() {
            recognizer.cancel();
        }
    }
    if let Ok(composer) = context.composer.lock() {
        if let Some(composer) = composer.as_ref() {
            composer.cancel();
        }
    }
    if emit_cancelled {
        let revision = context.latest_revision.load(Ordering::SeqCst);
        let text = context
            .latest_text
            .lock()
            .map(|value| value.clone())
            .unwrap_or_default();
        emit_device_voice_transcript(context, "cancelled", revision, &text, false, false, reason);
    }
    complete_device_voice_context(context);
}

fn cancel_device_voice_context(context: &Arc<DeviceVoiceContext>, reason: &str) {
    stop_device_voice_context(context, reason, true);
}

fn supersede_device_voice_context(context: &Arc<DeviceVoiceContext>) {
    stop_device_voice_context(context, "superseded by a new device recording", false);
}

fn submit_device_voice_via_agent_bus(context: Arc<DeviceVoiceContext>, text: String) {
    if !device_voice_target_is_current(&context) {
        cancel_device_voice_context(&context, "voice target changed before final submission");
        return;
    }
    let revision = context.latest_revision.load(Ordering::SeqCst);
    emit_device_voice_transcript(&context, "submitting", revision, &text, true, true, "");
    handle_incoming_usb_message(
        &context.emitter,
        "input/action".to_string(),
        serde_json::json!({
            "view": "voice_input",
            "state": text,
            "type": "voice_ptt_streaming_stt",
            "event": "device.microphone.hold",
            "boardDeviceId": context.board_device_id,
            "voiceUtteranceId": context.utterance_id,
            "requestId": format!("p4-device-ptt-{}", context.utterance_id),
        }),
    );
    complete_device_voice_context(&context);
}

fn report_device_voice_delivery_failure(
    context: &Arc<DeviceVoiceContext>,
    revision: u64,
    text: &str,
    error: &str,
    message: &str,
    code: &str,
) {
    if let Ok(mut composer_error) = context.composer_error.lock() {
        *composer_error = error.to_string();
    }
    emit_device_voice_transcript(context, "error", revision, text, true, false, error);
    let _ = context.emitter.emit(
        "usb-input-action-result",
        serde_json::json!({
            "ok": false,
            "pending": false,
            "view": "voice_input",
            "utteranceId": context.utterance_id,
            "text": text,
            "agentId": context.target.agent_id,
            "sessionId": device_voice_session_id(context),
            "message": message,
            "error": error,
            "code": code,
            "composerMode": device_voice_composer_mode(context),
            "composerError": error,
        }),
    );
    complete_device_voice_context(context);
}

fn stage_device_voice_final(
    context: Arc<DeviceVoiceContext>,
    revision: u64,
    text: String,
    confidence: Option<f64>,
) {
    let text = text.trim().to_string();
    if text.is_empty() || !context.final_requested.load(Ordering::SeqCst) {
        return;
    }
    if !device_voice_target_is_current(&context) {
        cancel_device_voice_context(&context, "voice target changed during recognition");
        return;
    }
    if !claim_device_voice_final(&context.final_handled) {
        return;
    }
    context.latest_revision.store(revision, Ordering::SeqCst);
    if let Ok(mut latest_text) = context.latest_text.lock() {
        *latest_text = text.clone();
    }
    if let Ok(mut latest_confidence) = context.latest_confidence.lock() {
        *latest_confidence = confidence;
    }
    let event = serde_json::json!({
        "utteranceId": context.utterance_id,
        "boardDeviceId": context.board_device_id,
        "agentId": context.target.agent_id,
        "sessionId": device_voice_session_id(&context),
        "revision": revision,
        "text": text,
        "isFinal": true,
        "phase": "finalizing",
        "ok": true,
        "confidence": confidence,
        "composerMode": device_voice_composer_mode(&context),
        "composerError": context.composer_error.lock().map(|v| v.clone()).unwrap_or_default(),
    });
    let _ = context.emitter.emit("voice-transcript", event.clone());
    let _ = context.emitter.emit("usb-audio-stream", event);

    #[cfg(target_os = "macos")]
    if context.target.agent_id == "mimocode" {
        let target = context
            .focused_text_target
            .lock()
            .ok()
            .and_then(|target| target.as_ref().cloned());
        let Some(target) = target else {
            let error = context
                .composer_error
                .lock()
                .map(|value| value.clone())
                .unwrap_or_default();
            let unavailable =
                "MiMoCode 当前光标未定位，本次语音未写入；请先点击终端输入位置".to_string();
            report_device_voice_delivery_failure(
                &context,
                revision,
                &text,
                if error.trim().is_empty() {
                    unavailable.as_str()
                } else {
                    error.as_str()
                },
                &unavailable,
                "FOCUSED_TEXT_INPUT_UNAVAILABLE",
            );
            return;
        };
        thread::spawn(move || {
            if !device_voice_target_is_current(&context) {
                cancel_device_voice_context(
                    &context,
                    "voice target changed before MiMoCode draft insertion",
                );
                return;
            }
            match codex_composer::insert_at_focused_text_target(&target, &text) {
                Ok(()) => {
                    if let Ok(mut composer_error) = context.composer_error.lock() {
                        composer_error.clear();
                    }
                    context.draft_ready.store(true, Ordering::SeqCst);
                    emit_device_voice_transcript(
                        &context,
                        "draft_ready",
                        revision,
                        &text,
                        true,
                        true,
                        "",
                    );
                }
                Err(error) => report_device_voice_delivery_failure(
                    &context,
                    revision,
                    &text,
                    &error,
                    "MiMoCode 当前光标草稿写入失败",
                    "FOCUSED_TEXT_INPUT_FAILED",
                ),
            }
        });
        return;
    }

    if agent_uses_visible_composer(&context.target.agent_id) {
        wait_for_visible_composer_startup(&context);
        if !device_voice_target_is_current(&context) {
            cancel_device_voice_context(
                &context,
                "voice target changed while preparing the visible composer",
            );
            return;
        }
    }

    let composer_update = context.composer.lock().ok().and_then(|composer| {
        composer
            .as_ref()
            .map(|bridge| bridge.update(revision, &text))
    });
    let Some(update_result) = composer_update else {
        if agent_uses_visible_composer(&context.target.agent_id) {
            let agent_label = visible_composer_agent_label(&context.target.agent_id);
            let error = context
                .composer_error
                .lock()
                .map(|value| value.clone())
                .unwrap_or_default();
            let unavailable = format!("{agent_label} 前台会话未定位，语音草稿未写入");
            report_device_voice_delivery_failure(
                &context,
                revision,
                &text,
                if error.trim().is_empty() {
                    unavailable.as_str()
                } else {
                    error.as_str()
                },
                &unavailable,
                "VISIBLE_COMPOSER_UNAVAILABLE",
            );
        } else {
            context.draft_ready.store(true, Ordering::SeqCst);
            emit_device_voice_transcript(&context, "draft_ready", revision, &text, true, true, "");
        }
        return;
    };
    if let Err(error) = update_result {
        let agent_label = visible_composer_agent_label(&context.target.agent_id);
        report_device_voice_delivery_failure(
            &context,
            revision,
            &text,
            &error,
            &format!("{agent_label} 语音草稿写入失败"),
            "VISIBLE_COMPOSER_DRAFT_FAILED",
        );
        return;
    }
    context.draft_ready.store(true, Ordering::SeqCst);
    emit_device_voice_transcript(&context, "draft_ready", revision, &text, true, true, "");
}

fn input_event_matches_device_voice_confirm(
    topic: &str,
    payload: &serde_json::Value,
    board_device_id: &str,
    draft_ready: bool,
    cancelled: bool,
) -> bool {
    if topic != "input/event"
        || payload.get("action").and_then(serde_json::Value::as_str) != Some("page_enter")
        || payload
            .get("handledLocally")
            .and_then(serde_json::Value::as_bool)
            == Some(true)
        || !draft_ready
        || cancelled
    {
        return false;
    }
    payload
        .get("boardDeviceId")
        .and_then(serde_json::Value::as_str)
        .is_some_and(|payload_board_device_id| payload_board_device_id == board_device_id)
}

fn input_event_confirms_device_voice_draft(
    context: &DeviceVoiceContext,
    topic: &str,
    payload: &serde_json::Value,
) -> bool {
    input_event_matches_device_voice_confirm(
        topic,
        payload,
        &context.board_device_id,
        context.draft_ready.load(Ordering::SeqCst),
        context.cancelled.load(Ordering::SeqCst),
    )
}

fn confirm_pending_device_voice_draft(topic: &str, payload: &serde_json::Value) -> bool {
    let context = active_device_voice_context()
        .lock()
        .ok()
        .and_then(|active| active.clone());
    let Some(context) = context else {
        return false;
    };
    if !input_event_confirms_device_voice_draft(&context, topic, payload) {
        return false;
    }
    if context.draft_submit_requested.swap(true, Ordering::SeqCst) {
        return true;
    }
    if !device_voice_target_is_current(&context) {
        cancel_device_voice_context(&context, "voice target changed before draft confirmation");
        return true;
    }
    let revision = context.latest_revision.load(Ordering::SeqCst);
    let text = context
        .latest_text
        .lock()
        .map(|value| value.trim().to_string())
        .unwrap_or_default();
    if text.is_empty() {
        report_device_voice_delivery_failure(
            &context,
            revision,
            "",
            "语音草稿为空",
            "语音草稿为空，未执行确认发送",
            "VOICE_DRAFT_EMPTY",
        );
        return true;
    }
    emit_device_voice_transcript(&context, "submitting", revision, &text, true, true, "");

    #[cfg(target_os = "macos")]
    if context.target.agent_id == "mimocode" {
        let target = context
            .focused_text_target
            .lock()
            .ok()
            .and_then(|target| target.as_ref().cloned());
        let Some(target) = target else {
            report_device_voice_delivery_failure(
                &context,
                revision,
                &text,
                "MiMoCode 语音草稿的光标定位已丢失",
                "MiMoCode 当前光标未定位，未执行确认发送",
                "FOCUSED_TEXT_INPUT_UNAVAILABLE",
            );
            return true;
        };
        thread::spawn(
            move || match codex_composer::submit_at_focused_text_target(&target) {
                Ok(()) if device_voice_target_is_current(&context) => {
                    emit_device_voice_transcript(
                        &context,
                        "submitted",
                        revision,
                        &text,
                        true,
                        true,
                        "",
                    );
                    let _ = context.emitter.emit(
                        "usb-input-action-result",
                        serde_json::json!({
                            "ok": true,
                            "pending": false,
                            "view": "voice_input",
                            "utteranceId": context.utterance_id,
                            "text": text,
                            "agentId": context.target.agent_id,
                            "sessionId": device_voice_session_id(&context),
                            "message": "已通过设备确认键发送 MiMoCode 语音草稿",
                            "composerMode": "focused-input",
                            "composerError": "",
                        }),
                    );
                    complete_device_voice_context(&context);
                }
                Ok(()) => cancel_device_voice_context(
                    &context,
                    "voice target changed during MiMoCode draft confirmation",
                ),
                Err(error) => report_device_voice_delivery_failure(
                    &context,
                    revision,
                    &text,
                    &error,
                    "MiMoCode 确认键发送失败，语音草稿未自动重发",
                    "FOCUSED_TEXT_SUBMIT_FAILED",
                ),
            },
        );
        return true;
    }

    if !agent_uses_visible_composer(&context.target.agent_id) {
        submit_device_voice_via_agent_bus(context, text);
        return true;
    }

    let composer_submission = context.composer.lock().ok().and_then(|composer| {
        composer
            .as_ref()
            .map(|bridge| bridge.confirm(revision, &text))
    });
    let Some(submission) = composer_submission else {
        let agent_label = visible_composer_agent_label(&context.target.agent_id);
        report_device_voice_delivery_failure(
            &context,
            revision,
            &text,
            &format!("{agent_label} 语音草稿桥接已关闭"),
            &format!("{agent_label} 输入框已无法确认，本次未发送"),
            "VISIBLE_COMPOSER_UNAVAILABLE",
        );
        return true;
    };

    thread::spawn(move || {
        match classify_visible_composer_submit(submission.wait(
            VISIBLE_COMPOSER_START_TIMEOUT,
            VISIBLE_COMPOSER_SUBMIT_TIMEOUT,
        )) {
            VisibleComposerSubmitOutcome::Submitted if device_voice_target_is_current(&context) => {
                let agent_label = visible_composer_agent_label(&context.target.agent_id);
                context.composer_visible.store(true, Ordering::SeqCst);
                if let Ok(mut composer_error) = context.composer_error.lock() {
                    composer_error.clear();
                }
                emit_device_voice_transcript(
                    &context,
                    "submitted",
                    revision,
                    &text,
                    true,
                    true,
                    "",
                );
                let _ = context.emitter.emit(
                    "usb-input-action-result",
                    serde_json::json!({
                        "ok": true,
                        "pending": false,
                        "view": "voice_input",
                        "utteranceId": context.utterance_id,
                        "text": text,
                        "agentId": context.target.agent_id,
                        "sessionId": device_voice_session_id(&context),
                        "message": format!("已通过设备确认键发送到 {agent_label} 当前会话"),
                        "composerMode": "visible",
                        "composerError": "",
                    }),
                );
                complete_device_voice_context(&context);
            }
            VisibleComposerSubmitOutcome::Submitted => {
                let agent_label = visible_composer_agent_label(&context.target.agent_id);
                cancel_device_voice_context(
                    &context,
                    &format!("voice target changed before {agent_label} draft confirmation"),
                );
            }
            VisibleComposerSubmitOutcome::ExplicitFailure(error) => {
                let agent_label = visible_composer_agent_label(&context.target.agent_id);
                report_device_voice_delivery_failure(
                    &context,
                    revision,
                    &text,
                    &error,
                    &format!("{agent_label} 确认键发送失败，语音草稿未自动重发"),
                    "VISIBLE_COMPOSER_FAILED",
                );
            }
            VisibleComposerSubmitOutcome::Unconfirmed(error) => {
                let agent_label = visible_composer_agent_label(&context.target.agent_id);
                report_device_voice_delivery_failure(
                    &context,
                    revision,
                    &text,
                    &error,
                    &format!("{agent_label} 确认键发送结果未确认，语音草稿不会重复发送"),
                    "VISIBLE_COMPOSER_UNCONFIRMED",
                );
            }
        }
    });
    true
}

fn fail_device_voice_context(context: &Arc<DeviceVoiceContext>, error: &str) {
    if !claim_device_voice_final(&context.final_handled) {
        return;
    }
    if let Ok(composer) = context.composer.lock() {
        if let Some(composer) = composer.as_ref() {
            composer.cancel();
        }
    }
    let revision = context.latest_revision.fetch_add(1, Ordering::SeqCst) + 1;
    let text = context
        .latest_text
        .lock()
        .map(|value| value.clone())
        .unwrap_or_default();
    emit_device_voice_transcript(context, "error", revision, &text, false, false, error);
    complete_device_voice_context(context);
}

fn start_device_voice_context(
    emitter: &tauri::AppHandle,
    board_device_id: &str,
    utterance_id: &str,
    session_queue_empty_at_start: bool,
) {
    let target = device_voice_target_snapshot(board_device_id);
    let agent_is_frontmost =
        codex_composer::CodexComposerBridge::is_agent_frontmost(&target.agent_id);
    let use_current_visible_session = should_use_current_visible_session(
        agent_is_frontmost,
        session_queue_empty_at_start,
        &target.agent_id,
    );
    let context = Arc::new(DeviceVoiceContext {
        emitter: emitter.clone(),
        utterance_id: utterance_id.to_string(),
        board_device_id: board_device_id.to_string(),
        target,
        session_queue_empty_at_start,
        use_current_visible_session: AtomicBool::new(use_current_visible_session),
        final_requested: AtomicBool::new(false),
        final_handled: AtomicBool::new(false),
        draft_ready: AtomicBool::new(false),
        draft_submit_requested: AtomicBool::new(false),
        cancelled: AtomicBool::new(false),
        latest_revision: AtomicU64::new(0),
        latest_text: Mutex::new(String::new()),
        latest_confidence: Mutex::new(None),
        streaming_error: Mutex::new(String::new()),
        composer_error: Mutex::new(String::new()),
        composer_visible: AtomicBool::new(false),
        recognizer: Mutex::new(None),
        composer: Mutex::new(None),
        composer_startup_complete: Mutex::new(false),
        composer_startup_ready: Condvar::new(),
        #[cfg(target_os = "macos")]
        focused_text_target: Mutex::new(None),
    });

    let previous = active_device_voice_context()
        .lock()
        .ok()
        .and_then(|mut active| active.replace(context.clone()));
    if let Some(previous) = previous {
        eprintln!(
            "[device-voice] superseding utterance={} with utterance={}",
            previous.utterance_id, context.utterance_id
        );
        supersede_device_voice_context(&previous);
    }

    emit_device_voice_transcript(&context, "listening", 0, "", false, true, "");

    #[cfg(target_os = "macos")]
    if context.target.agent_id == "mimocode" {
        match codex_composer::capture_focused_text_target() {
            Ok(target) => {
                if let Ok(mut slot) = context.focused_text_target.lock() {
                    *slot = Some(target);
                }
                if let Ok(mut composer_error) = context.composer_error.lock() {
                    composer_error.clear();
                }
            }
            Err(error) => {
                if let Ok(mut composer_error) = context.composer_error.lock() {
                    *composer_error = error;
                }
            }
        }
        emit_device_voice_transcript(&context, "listening", 0, "", false, true, "");
    }

    if device_voice_uses_current_visible_session(&context) {
        eprintln!(
            "[device-voice] current visible {} session selected (frontmost={}, empty_queue={})",
            context.target.agent_id, agent_is_frontmost, session_queue_empty_at_start,
        );
    }

    let bound_target_is_addressable =
        device_voice_bound_target_is_addressable(session_queue_empty_at_start, &context.target);
    let visible_target_is_addressable =
        device_voice_uses_current_visible_session(&context) || bound_target_is_addressable;
    if agent_uses_visible_composer(&context.target.agent_id) && visible_target_is_addressable {
        let startup_context = context.clone();
        if let Err(error) = thread::Builder::new()
            .name("pet-visible-composer-startup".to_string())
            .spawn(move || {
                let composer_context = Arc::downgrade(&startup_context);
                let callback_agent_id = startup_context.target.agent_id.clone();
                let callback = Arc::new(move |event: codex_composer::CodexComposerEvent| {
                    let Some(context) = composer_context.upgrade() else {
                        return;
                    };
                    if event.ok && matches!(event.phase.as_str(), "ready" | "updated" | "submitted")
                    {
                        context.composer_visible.store(true, Ordering::SeqCst);
                        if let Ok(mut composer_error) = context.composer_error.lock() {
                            composer_error.clear();
                        }
                    } else if !event.ok {
                        eprintln!(
                            "[device-voice] {} visible composer {} failed: {}",
                            callback_agent_id, event.phase, event.error
                        );
                        context.composer_visible.store(false, Ordering::SeqCst);
                        if let Ok(mut composer_error) = context.composer_error.lock() {
                            *composer_error = event.error.clone();
                        }
                    }
                    let revision = context.latest_revision.load(Ordering::SeqCst);
                    let text = context
                        .latest_text
                        .lock()
                        .map(|value| value.clone())
                        .unwrap_or_default();
                    emit_device_voice_transcript(
                        &context,
                        if event.ok { "listening" } else { "partial" },
                        revision,
                        &text,
                        false,
                        true,
                        "",
                    );
                });
                let start_bound = |callback: Arc<
                    dyn Fn(codex_composer::CodexComposerEvent) + Send + Sync,
                >| {
                    if startup_context.target.agent_id == "claude-code" {
                        codex_composer::CodexComposerBridge::start_claude(
                            &startup_context.target.session_id,
                            &startup_context.target.session_title,
                            &startup_context.target.session_cwd,
                            move |event| callback(event),
                        )
                    } else {
                        codex_composer::CodexComposerBridge::start(
                            &startup_context.target.session_id,
                            &startup_context.target.session_title,
                            &startup_context.target.session_cwd,
                            move |event| callback(event),
                        )
                    }
                };
                let composer_result = if device_voice_uses_current_visible_session(&startup_context)
                {
                    let current_callback = callback.clone();
                    match codex_composer::CodexComposerBridge::start_current(
                        &startup_context.target.agent_id,
                        move |event| current_callback(event),
                    ) {
                        Ok(composer) => Ok(composer),
                        Err(current_error) if bound_target_is_addressable => {
                            eprintln!(
                                "[device-voice] current visible composer unavailable, falling back to device session: {current_error}"
                            );
                            startup_context
                                .use_current_visible_session
                                .store(false, Ordering::SeqCst);
                            start_bound(callback.clone()).map_err(|bound_error| {
                                format!(
                                    "当前会话输入框不可用: {current_error}; 设备气泡会话定位也失败: {bound_error}"
                                )
                            })
                        }
                        Err(error) => Err(error),
                    }
                } else {
                    start_bound(callback.clone())
                };
                match composer_result {
                    Ok(composer) if startup_context.cancelled.load(Ordering::SeqCst) => {
                        composer.cancel();
                    }
                    Ok(composer) => {
                        if let Ok(mut slot) = startup_context.composer.lock() {
                            *slot = Some(composer);
                        }
                    }
                    Err(error) => {
                        if let Ok(mut composer_error) = startup_context.composer_error.lock() {
                            *composer_error = error;
                        }
                    }
                }
                mark_visible_composer_startup_complete(&startup_context);
            })
        {
            if let Ok(mut composer_error) = context.composer_error.lock() {
                *composer_error = format!("无法启动前台输入准备线程: {error}");
            }
            mark_visible_composer_startup_complete(&context);
        }
    } else if agent_uses_visible_composer(&context.target.agent_id) {
        if let Ok(mut composer_error) = context.composer_error.lock() {
            let agent_label = visible_composer_agent_label(&context.target.agent_id);
            *composer_error = if context.target.session_id.is_empty() {
                format!("当前 {agent_label} 会话没有可定位的 session ID")
            } else if context.target.session_title.is_empty() {
                format!("当前 {agent_label} 会话没有可校验的标题")
            } else {
                format!("当前 {agent_label} 会话标题不唯一，无法安全定位可见输入框")
            };
        }
        mark_visible_composer_startup_complete(&context);
    } else {
        mark_visible_composer_startup_complete(&context);
    }

    let recognizer_context = Arc::downgrade(&context);
    match pc_audio::StreamingSpeechRecognizer::start(move |event| {
        let Some(context) = recognizer_context.upgrade() else {
            return;
        };
        if !device_voice_target_is_current(&context) {
            cancel_device_voice_context(&context, "voice target changed during recording");
            return;
        }
        match event {
            pc_audio::StreamingSpeechEvent::Ready => {
                emit_device_voice_transcript(&context, "listening", 0, "", false, true, "")
            }
            pc_audio::StreamingSpeechEvent::Partial {
                revision,
                text,
                confidence,
            } => {
                context.latest_revision.store(revision, Ordering::SeqCst);
                if let Ok(mut latest_text) = context.latest_text.lock() {
                    *latest_text = text.clone();
                }
                if let Ok(mut latest_confidence) = context.latest_confidence.lock() {
                    *latest_confidence = confidence;
                }
                if let Ok(composer) = context.composer.lock() {
                    if let Some(composer) = composer.as_ref() {
                        if let Err(error) = composer.update(revision, &text) {
                            context.composer_visible.store(false, Ordering::SeqCst);
                            if let Ok(mut composer_error) = context.composer_error.lock() {
                                *composer_error = error;
                            }
                        }
                    }
                }
                emit_device_voice_transcript(&context, "partial", revision, &text, false, true, "");
            }
            pc_audio::StreamingSpeechEvent::Final {
                revision,
                text,
                confidence,
            } => {
                if text.trim().is_empty() {
                    fail_device_voice_context(&context, "火山引擎云端识别未检测到有效语音");
                } else {
                    stage_device_voice_final(context, revision, text, confidence);
                }
            }
            pc_audio::StreamingSpeechEvent::Error(error) => {
                if let Ok(mut streaming_error) = context.streaming_error.lock() {
                    *streaming_error = error.clone();
                }
                fail_device_voice_context(&context, &error);
            }
        }
    }) {
        Ok(recognizer) => {
            if let Ok(mut slot) = context.recognizer.lock() {
                *slot = Some(recognizer);
            }
        }
        Err(error) => {
            if let Ok(mut streaming_error) = context.streaming_error.lock() {
                *streaming_error = error.clone();
            }
            fail_device_voice_context(&context, &error);
        }
    }
}

fn push_device_voice_chunk(chunk: usb_audio::ValidatedUsbAudioChunk) {
    let context = active_device_voice_context()
        .lock()
        .ok()
        .and_then(|active| active.clone());
    let Some(context) = context else {
        return;
    };
    if context.utterance_id != chunk.session_id
        || context.board_device_id != chunk.board_device_id
        || context.cancelled.load(Ordering::SeqCst)
    {
        return;
    }
    let push_error = context.recognizer.lock().ok().and_then(|recognizer| {
        recognizer
            .as_ref()
            .and_then(|recognizer| recognizer.push_pcm(&chunk.pcm).err())
    });
    if let Some(error) = push_error {
        let error = format!("云端音频分片 {} 发送失败: {error}", chunk.sequence);
        if let Ok(mut streaming_error) = context.streaming_error.lock() {
            *streaming_error = error.clone();
        }
        fail_device_voice_context(&context, &error);
    }
}

fn finish_device_voice_context(completed: usb_audio::CompletedUsbAudio) {
    let context = active_device_voice_context()
        .lock()
        .ok()
        .and_then(|active| active.clone());
    let Some(context) = context else {
        return;
    };
    if context.utterance_id != completed.session_id
        || context.board_device_id != completed.board_device_id
    {
        cancel_device_voice_context(&context, "completed audio did not match active utterance");
        return;
    }
    if completed.pcm.is_empty() {
        fail_device_voice_context(&context, "设备没有返回可识别的麦克风音频");
        return;
    }
    context.final_requested.store(true, Ordering::SeqCst);
    let finish_result = context
        .recognizer
        .lock()
        .ok()
        .and_then(|recognizer| recognizer.as_ref().map(|recognizer| recognizer.finish()));
    match finish_result {
        Some(Ok(())) => {}
        Some(Err(error)) => {
            fail_device_voice_context(&context, &error);
            return;
        }
        None => {
            fail_device_voice_context(&context, "火山引擎云端识别器未启动");
            return;
        }
    }
    let timeout_context = context.clone();
    thread::spawn(move || {
        thread::sleep(Duration::from_secs(9));
        if !timeout_context.final_handled.load(Ordering::SeqCst)
            && !timeout_context.cancelled.load(Ordering::SeqCst)
        {
            fail_device_voice_context(&timeout_context, "等待火山引擎云端识别最终文本超时");
        }
    });
}

fn p4_session_binding_for_board(board_device_id: &str) -> Option<P4SessionBinding> {
    let board_device_id = board_device_id.trim();
    let binding = p4_session_binding().lock().ok()?.clone();
    if board_device_id.is_empty()
        || binding.board_device_id != board_device_id
        || binding.agent_id.is_empty()
        || binding.session_id.is_empty()
    {
        return None;
    }
    Some(binding)
}

fn usb_session_binding_allows(
    board_device_id: &str,
    source: &str,
    payload: &serde_json::Value,
) -> bool {
    let Some(binding) = p4_session_binding_for_board(board_device_id) else {
        return true;
    };
    let source = normalize_agent_id(source).unwrap_or_else(|| source.trim().to_ascii_lowercase());
    let session_id = payload
        .get("sessionId")
        .and_then(|value| value.as_str())
        .map(str::trim)
        .unwrap_or_default();
    source == binding.agent_id && session_id == binding.session_id
}

fn resolve_usb_inject_target(board_device_id: &str) -> (String, String) {
    if let Some(binding) = p4_session_binding_for_board(board_device_id) {
        return (binding.agent_id, binding.session_id);
    }
    (resolve_usb_inject_agent_id(), "auto".to_string())
}

fn pc_audio_binding_matches(board_device_id: &str, generation: u64) -> bool {
    pc_audio_board_binding().lock().is_ok_and(|binding| {
        !binding.board_device_id.is_empty()
            && binding.board_device_id == board_device_id
            && binding.generation == generation
    })
}

fn take_pc_audio_binding_for_capture(capture_id: u64) -> Option<PcAudioBoardBinding> {
    let mut binding = pc_audio_board_binding().lock().ok()?;
    if binding.active_capture_id != Some(capture_id) || binding.board_device_id.is_empty() {
        return None;
    }
    binding.active_capture_id = None;
    Some(binding.clone())
}

fn pc_audio_payload_is_current(payload: &serde_json::Value) -> bool {
    let Some(generation) = payload
        .get("pcAudioGeneration")
        .and_then(|value| value.as_u64())
    else {
        return true;
    };
    let board_device_id = payload
        .get("boardDeviceId")
        .and_then(|value| value.as_str())
        .unwrap_or_default();
    pc_audio_binding_matches(board_device_id, generation)
}

fn pc_audio_recognition_sender() -> &'static mpsc::SyncSender<PcAudioRecognitionJob> {
    static SENDER: OnceLock<mpsc::SyncSender<PcAudioRecognitionJob>> = OnceLock::new();
    SENDER.get_or_init(|| {
        let (sender, receiver) = mpsc::sync_channel::<PcAudioRecognitionJob>(2);
        thread::Builder::new()
            .name("pet-pc-speech-recognition".to_string())
            .spawn(move || {
                while let Ok(job) = receiver.recv() {
                    if !pc_audio_binding_matches(&job.board_device_id, job.generation) {
                        continue;
                    }
                    let _ = job.emitter.emit(
                        "usb-audio-stream",
                        serde_json::json!({
                            "phase": "recognizing",
                            "ok": true,
                            "source": "windows-speech",
                            "bytes": job.pcm.len(),
                            "captureId": job.capture_id,
                        }),
                    );
                    match pc_audio::transcribe_pcm_s16le(&job.pcm) {
                        Ok(text)
                            if pc_audio_binding_matches(&job.board_device_id, job.generation) =>
                        {
                            let _ = job.emitter.emit(
                                "usb-audio-result",
                                serde_json::json!({
                                    "phase": "recognized",
                                    "ok": true,
                                    "source": "windows-speech",
                                    "text": text,
                                    "captureId": job.capture_id,
                                }),
                            );
                            handle_incoming_usb_message(
                                &job.emitter,
                                "input/action".to_string(),
                                serde_json::json!({
                                    "view": "voice_input",
                                    "state": text,
                                    "type": "voice_ptt_stt",
                                    "event": "button.sw1.hold",
                                    "boardDeviceId": job.board_device_id,
                                    "pcAudioGeneration": job.generation,
                                    "requestId": format!(
                                        "p4-ptt-{}-{}",
                                        job.generation, job.capture_id
                                    ),
                                }),
                            );
                        }
                        Ok(_) => {
                            let _ = job.emitter.emit(
                                "usb-audio-result",
                                serde_json::json!({
                                    "phase": "cancelled",
                                    "ok": false,
                                    "source": "windows-speech",
                                    "captureId": job.capture_id,
                                    "error": "voice target changed before recognition completed",
                                }),
                            );
                        }
                        Err(error) => {
                            let _ = job.emitter.emit(
                                "usb-audio-result",
                                serde_json::json!({
                                    "phase": "error",
                                    "ok": false,
                                    "source": "windows-speech",
                                    "captureId": job.capture_id,
                                    "error": error,
                                }),
                            );
                        }
                    }
                }
            })
            .expect("failed to start PC speech recognition worker");
        sender
    })
}

fn process_pc_audio_capture_result(
    emitter: &tauri::AppHandle,
    result: pc_audio::PcAudioCaptureResult,
    binding: PcAudioBoardBinding,
) {
    let event = result.event.clone();
    let _ = emitter.emit("usb-audio-stream", event.clone());
    let _ = emitter.emit("usb-audio-result", event);
    if result.pcm.is_empty() || probe_voice_service_running(DEFAULT_VOICE_SERVICE_PORT) {
        return;
    }
    let job = PcAudioRecognitionJob {
        emitter: emitter.clone(),
        pcm: result.pcm,
        board_device_id: binding.board_device_id,
        generation: binding.generation,
        capture_id: result.capture_id,
    };
    if let Err(error) = pc_audio_recognition_sender().try_send(job) {
        let _ = emitter.emit(
            "usb-audio-result",
            serde_json::json!({
                "phase": "error",
                "ok": false,
                "source": "windows-speech",
                "error": format!("speech recognition queue is busy: {error}"),
            }),
        );
    }
}

fn ensure_pc_audio_completion_monitor(emitter: &tauri::AppHandle) {
    static STARTED: OnceLock<()> = OnceLock::new();
    let emitter = emitter.clone();
    STARTED.get_or_init(|| {
        thread::Builder::new()
            .name("pet-pc-microphone-timeout".to_string())
            .spawn(move || loop {
                let completed = pc_audio_capture()
                    .lock()
                    .ok()
                    .and_then(|mut capture| capture.take_completed());
                if let Some(result) = completed {
                    if let Some(binding) = take_pc_audio_binding_for_capture(result.capture_id) {
                        process_pc_audio_capture_result(&emitter, result, binding);
                    }
                }
                thread::sleep(Duration::from_millis(100));
            })
            .expect("failed to start PC microphone timeout monitor");
    });
}

#[derive(Debug, Clone)]
struct UsbAgentInput {
    text: String,
    view: &'static str,
    button_event: String,
    source: &'static str,
    input_type: &'static str,
    action_type: String,
}

fn extract_usb_agent_input(topic: &str, payload: &serde_json::Value) -> Option<UsbAgentInput> {
    if topic == "input/action" {
        return Some(UsbAgentInput {
            text: extract_usb_voice_input_text(payload)?,
            view: "voice_input",
            button_event: payload
                .get("event")
                .and_then(|v| v.as_str())
                .unwrap_or("button.encoder.long_press")
                .to_string(),
            source: "usb-input-action",
            input_type: "voice-text",
            action_type: payload
                .get("type")
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                .to_string(),
        });
    }
    if topic != "input/event"
        || payload
            .get("handledLocally")
            .and_then(|value| value.as_bool())
            == Some(true)
    {
        return None;
    }

    let action = payload.get("action").and_then(|v| v.as_str())?.trim();
    let text = match action {
        "agent_enter" => "继续当前任务。".to_string(),
        "agent_prompt" => payload
            .get("value")
            .and_then(|v| v.as_str())
            .map(str::trim)
            .filter(|value| !value.is_empty())?
            .to_string(),
        _ => return None,
    };
    Some(UsbAgentInput {
        text,
        view: "hardware_input",
        button_event: payload
            .get("event")
            .and_then(|v| v.as_str())
            .unwrap_or("device.input")
            .to_string(),
        source: "usb-input-event",
        input_type: "hardware-control",
        action_type: action.to_string(),
    })
}

fn resolve_usb_input_route_snapshot(payload: &serde_json::Value) -> (String, String, String) {
    let board_device_id = payload
        .get("boardDeviceId")
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .trim();
    let voice_utterance_id = payload
        .get("voiceUtteranceId")
        .or_else(|| payload.get("utteranceId"))
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .trim()
        .to_string();

    if !voice_utterance_id.is_empty() {
        let active = active_device_voice_context()
            .lock()
            .ok()
            .and_then(|active| active.clone());
        if let Some(context) = active {
            if let Some((agent_id, session_id)) = frozen_device_voice_inject_target(
                &voice_utterance_id,
                board_device_id,
                &context.utterance_id,
                &context.board_device_id,
                &context.target,
                context.cancelled.load(Ordering::SeqCst),
            ) {
                return (agent_id, session_id, voice_utterance_id);
            }
        }
    }

    let (agent_id, session_id) = resolve_usb_inject_target(board_device_id);
    (agent_id, session_id, voice_utterance_id)
}

fn forward_usb_agent_input_to_bridge(
    payload: &serde_json::Value,
    input: &UsbAgentInput,
    agent_id: &str,
    session_id: &str,
) -> Result<serde_json::Value, String> {
    if input.text.trim().is_empty() {
        return Ok(serde_json::json!({
            "ok": true,
            "skipped": true,
            "reason": "empty device input",
        }));
    }

    let board_device_id = payload
        .get("boardDeviceId")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_string();
    let agent_id = agent_id.trim().to_string();
    let session_id = if session_id.trim().is_empty() {
        "auto".to_string()
    } else {
        session_id.trim().to_string()
    };
    let local_device_id = payload
        .get("localDeviceId")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_string();
    let request_id = payload
        .get("requestId")
        .and_then(|value| value.as_str())
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| format!("usb-input-{}", uuid::Uuid::new_v4()));

    let body = serde_json::json!({
        "requestId": request_id.clone(),
        "agentId": agent_id,
        "sessionId": session_id,
        "text": input.text,
        "buttonEvent": input.button_event,
        "metadata": {
            "source": input.source,
            "inputType": input.input_type,
            "trigger": "device-button",
            "transport": "usb",
            "boardDeviceId": board_device_id,
            "localDeviceId": local_device_id,
            "actionType": input.action_type,
            "requestId": request_id,
        }
    });

    // Bridge may be in a short restart window during dev hot-reload.
    // Only target the managed bridge; a stale legacy bridge cannot share the
    // agent-session-bus port and would report misleading injection failures.
    for attempt in 1..=8 {
        let url = format!(
            "http://127.0.0.1:{}/mock-button-inject",
            DEFAULT_BRIDGE_PORT
        );
        let client = lan_http_client(Duration::from_secs(150))?;
        let response = match client.post(&url).json(&body).send() {
            Ok(resp) => resp,
            Err(error) if error.is_connect() && attempt < 8 => {
                thread::sleep(Duration::from_millis(250));
                continue;
            }
            Err(error) => {
                return Err(format!(
                    "usb device input inject request failed on {}: {}",
                    DEFAULT_BRIDGE_PORT, error
                ));
            }
        };
        let status = response.status();
        let response_text = response.text().unwrap_or_default();
        if !status.is_success() {
            return Err(format!(
                "usb device input inject http {} on {}: {}",
                status, DEFAULT_BRIDGE_PORT, response_text
            ));
        }
        println!("[usb-device-input] bridge response {}", response_text);

        let parsed =
            serde_json::from_str::<serde_json::Value>(&response_text).unwrap_or_else(|_| {
                serde_json::json!({
                    "ok": true,
                    "raw": response_text,
                })
            });
        if parsed
            .get("ok")
            .and_then(|v| v.as_bool())
            .is_some_and(|ok| !ok)
        {
            return Err(parsed
                .get("error")
                .and_then(|v| v.as_str())
                .unwrap_or("mock-button-inject returned ok=false")
                .to_string());
        }
        return Ok(parsed);
    }

    Err("usb device input inject failed while bridge was unavailable".to_string())
}

fn handle_incoming_usb_message(
    emitter: &tauri::AppHandle,
    topic: String,
    payload: serde_json::Value,
) {
    resolve_button_config_ack(&topic, &payload);

    if topic.starts_with("audio/") {
        let (relay_event, validated_chunk, completed_audio) = match usb_audio_relay().lock() {
            Ok(mut relay) => {
                let event = relay.handle(&topic, &payload);
                let chunk = relay.take_validated_chunk();
                let completed = relay.take_completed();
                (event, chunk, completed)
            }
            Err(error) => (
                Some(serde_json::json!({
                    "phase": "error",
                    "ok": false,
                    "error": format!("USB audio relay lock failed: {error}"),
                })),
                None,
                None,
            ),
        };
        if topic != "audio/chunk" {
            let _ = emitter.emit(
                "usb-message",
                serde_json::json!({"topic": topic, "payload": payload}),
            );
        }
        if let Some(event) = relay_event {
            if topic == "audio/begin"
                && event.get("ok").and_then(|value| value.as_bool()) == Some(true)
                && event.get("duplicate").and_then(|value| value.as_bool()) != Some(true)
            {
                eprintln!("[device-voice] audio/begin payload={payload}");
                start_device_voice_context(
                    emitter,
                    payload
                        .get("boardDeviceId")
                        .and_then(|value| value.as_str())
                        .unwrap_or_default(),
                    payload
                        .get("sessionId")
                        .and_then(|value| value.as_str())
                        .unwrap_or_default(),
                    audio_begin_session_queue_empty(&payload),
                );
            } else if topic == "audio/error" {
                let active = active_device_voice_context()
                    .lock()
                    .ok()
                    .and_then(|active| active.clone());
                if let Some(active) = active {
                    cancel_device_voice_context(
                        &active,
                        payload
                            .get("error")
                            .and_then(|value| value.as_str())
                            .unwrap_or("device microphone reported an error"),
                    );
                }
            }
            let terminal = matches!(
                event.get("phase").and_then(|value| value.as_str()),
                Some("end") | Some("error")
            ) || event.get("ok").and_then(|value| value.as_bool()) == Some(false);
            let _ = emitter.emit("usb-audio-stream", event.clone());
            if terminal {
                let _ = emitter.emit("usb-audio-result", event);
            }
        }
        if let Some(chunk) = validated_chunk {
            push_device_voice_chunk(chunk);
        }
        if let Some(completed) = completed_audio {
            finish_device_voice_context(completed);
        }
        return;
    }

    let pc_audio_context = pc_audio_board_binding().lock().ok().and_then(|binding| {
        pc_audio::fallback_gesture_for_board(&topic, &payload, &binding.board_device_id)
            .map(|action| (action, binding.clone()))
    });
    if let Some((action, binding_snapshot)) = pc_audio_context {
        if action == "start" {
            ensure_pc_audio_completion_monitor(emitter);
            let event = match pc_audio_capture().lock() {
                Ok(mut capture) => capture.start(),
                Err(error) => serde_json::json!({
                    "phase": "begin",
                    "ok": false,
                    "source": "pc-microphone",
                    "error": format!("PC microphone fallback lock failed: {error}"),
                }),
            };
            if event.get("ok").and_then(|value| value.as_bool()) == Some(true) {
                if let Some(capture_id) = event.get("captureId").and_then(|value| value.as_u64()) {
                    if let Ok(mut binding) = pc_audio_board_binding().lock() {
                        if binding.board_device_id == binding_snapshot.board_device_id
                            && binding.generation == binding_snapshot.generation
                        {
                            binding.active_capture_id = Some(capture_id);
                        }
                    }
                }
            }
            let terminal = event.get("ok").and_then(|value| value.as_bool()) == Some(false);
            let _ = emitter.emit("usb-audio-stream", event.clone());
            if terminal {
                let _ = emitter.emit("usb-audio-result", event);
            }
        } else {
            let result = pc_audio_capture()
                .lock()
                .ok()
                .and_then(|mut capture| capture.stop_with_pcm());
            if let Some(result) = result {
                if let Some(binding) = take_pc_audio_binding_for_capture(result.capture_id) {
                    process_pc_audio_capture_result(emitter, result, binding);
                } else {
                    let _ = emitter.emit("usb-audio-stream", result.event.clone());
                    let _ = emitter.emit("usb-audio-result", result.event);
                }
            }
        }
    }

    let _ = emitter.emit(
        "usb-message",
        serde_json::json!({"topic": topic, "payload": payload}),
    );

    if confirm_pending_device_voice_draft(&topic, &payload) {
        return;
    }

    if topic == "availability" {
        if let Some(online) = payload.get("online").and_then(|v| v.as_bool()) {
            if !online {
                let disconnected_board_id = payload
                    .get("boardDeviceId")
                    .and_then(|value| value.as_str())
                    .unwrap_or_default();
                let should_disable = pc_audio_board_binding().lock().is_ok_and(|binding| {
                    !binding.board_device_id.is_empty()
                        && (disconnected_board_id.is_empty()
                            || binding.board_device_id == disconnected_board_id)
                });
                if should_disable {
                    if let Ok(mut capture) = pc_audio_capture().lock() {
                        let _ = capture.configure(false, usb_audio::DEFAULT_PCM_RELAY_PORT);
                    }
                    if let Ok(mut binding) = pc_audio_board_binding().lock() {
                        binding.generation = binding.generation.wrapping_add(1).max(1);
                        binding.board_device_id.clear();
                        binding.active_capture_id = None;
                    }
                }
                let _ = emitter.emit("usb-disconnected", ());
            }
        }
        return;
    }

    if topic == "input/action" || topic == "input/event" {
        let Some(agent_input) = extract_usb_agent_input(&topic, &payload) else {
            return;
        };
        let (route_agent_id, route_session_id, voice_utterance_id) =
            resolve_usb_input_route_snapshot(&payload);
        println!(
            "[usb-device-input] received topic={} payload={}",
            topic, payload
        );
        let emitter = emitter.clone();
        thread::spawn(move || {
            if !pc_audio_payload_is_current(&payload) {
                let _ = emitter.emit(
                    "usb-input-action-result",
                    serde_json::json!({
                        "ok": false,
                        "cancelled": true,
                        "view": agent_input.view,
                        "utteranceId": voice_utterance_id,
                        "message": "voice target changed before Agent injection",
                    }),
                );
                return;
            }
            let input_text = agent_input.text.clone();
            let input_view = agent_input.view;
            let pending_agent_id = route_agent_id.clone();
            let pending_session_id = route_session_id.clone();
            let _ = emitter.emit(
                "usb-input-action-result",
                serde_json::json!({
                    "ok": true,
                    "pending": true,
                    "view": input_view,
                    "utteranceId": voice_utterance_id.clone(),
                    "text": input_text.clone(),
                    "agentId": pending_agent_id.clone(),
                    "sessionId": pending_session_id,
                    "message": format!("已发送到 {}，等待模型回复...", pending_agent_id),
                }),
            );

            match forward_usb_agent_input_to_bridge(
                &payload,
                &agent_input,
                &route_agent_id,
                &route_session_id,
            ) {
                Ok(response) => {
                    let request = response.get("request").unwrap_or(&serde_json::Value::Null);
                    let mut agent_id = request
                        .get("agentId")
                        .and_then(|v| v.as_str())
                        .unwrap_or_default()
                        .trim()
                        .to_string();
                    if agent_id.is_empty() {
                        agent_id = route_agent_id.clone();
                    }

                    let mut session_id = response
                        .get("done")
                        .and_then(|v| v.get("sessionId"))
                        .and_then(|v| v.as_str())
                        .or_else(|| {
                            response
                                .get("ready")
                                .and_then(|v| v.get("sessionId"))
                                .and_then(|v| v.as_str())
                        })
                        .or_else(|| request.get("sessionId").and_then(|v| v.as_str()))
                        .unwrap_or("auto")
                        .trim()
                        .to_string();
                    if session_id.is_empty() {
                        session_id = "auto".to_string();
                    }

                    let reply_preview = response
                        .get("tokenPreview")
                        .and_then(|v| v.as_str())
                        .unwrap_or_default()
                        .to_string();
                    let queued = response
                        .get("queued")
                        .and_then(|value| value.as_bool())
                        .unwrap_or(false);
                    let message = if queued {
                        response
                            .get("message")
                            .and_then(|value| value.as_str())
                            .unwrap_or("当前任务仍在运行，设备语音已排队。")
                            .to_string()
                    } else {
                        format!("已发送到 {} · 会话 {}", agent_id, session_id)
                    };
                    let _ = emitter.emit(
                        "usb-input-action-result",
                        serde_json::json!({
                            "ok": true,
                            "pending": queued,
                            "queued": queued,
                            "view": input_view,
                            "utteranceId": voice_utterance_id.clone(),
                            "text": input_text,
                            "agentId": agent_id,
                            "sessionId": session_id,
                            "message": message,
                            "tokenPreview": reply_preview,
                            "replyPreview": reply_preview,
                            "response": response,
                        }),
                    );
                    println!("[usb-device-input] forward ok");
                }
                Err(error) => {
                    let error_message = error.to_string();
                    let lower = error_message.to_ascii_lowercase();
                    let transient = lower.contains("error sending request for url")
                        || lower.contains("connection refused")
                        || lower.contains("timed out")
                        || lower.contains("failed on 23333");
                    let _ = emitter.emit(
                        "usb-input-action-result",
                        serde_json::json!({
                            "ok": false,
                            "view": input_view,
                            "utteranceId": voice_utterance_id,
                            "text": input_text,
                            "agentId": route_agent_id,
                            "sessionId": route_session_id,
                            "message": error_message,
                            "error": error_message,
                            "transient": transient,
                        }),
                    );
                    eprintln!("[usb-device-input] forward failed: {}", error);
                }
            }
        });
    }
}
const DEFAULT_VOICE_SERVICE_HOST: &str = "127.0.0.1";
const DEFAULT_VOICE_SERVICE_PORT: u16 = 8080;
const VOICE_SERVICE_RESOURCE_ROOT: &str = "voice-service";
const VOICE_SERVICE_ENTRY_RELATIVE_PATH: &str = "src/index.mjs";
const VOICE_SERVICE_LOG_FILE_NAME: &str = "voice-service.log";
const VOICE_SERVICE_PID_FILE_NAME: &str = "voice-service.pid";
const VOICE_SERVICE_AGENT_ID_FILE_NAME: &str = "voice-service.agent-id";
const VOICE_SERVICE_LAUNCH_SCRIPT_FILE_NAME: &str = "run-voice-service.sh";
const BRIDGE_RESOURCE_ROOT: &str = "bridge";
const BRIDGE_WORKSPACE_RELATIVE_PATH: &str = "packages/clawd-backend-service";
const BRIDGE_ENTRY_RELATIVE_PATH: &str = "packages/clawd-backend-service/src/headless-mqtt.js";
const BRIDGE_LOG_FILE_NAME: &str = "status-bridge.log";
const BRIDGE_PID_FILE_NAME: &str = "status-bridge.pid";
const BRIDGE_LAUNCH_SCRIPT_FILE_NAME: &str = "run-status-bridge.sh";
const BRIDGE_WINDOWS_LAUNCH_SCRIPT_FILE_NAME: &str = "run-status-bridge.ps1";
#[cfg(target_os = "macos")]
const BRIDGE_LAUNCH_AGENT_LABEL: &str = "com.petmanager.status-bridge";
const BRIDGE_WINDOWS_STARTUP_SCRIPT_NAME: &str = "Pet Manager Status Bridge.cmd";
const USB_STATE_MAX_AGE_MS: u64 = 10 * 60 * 1000;
const USB_BRIDGE_SCAN_MAX_FILES: usize = 64;
const KNOWN_USB_STATE_SOURCES: [&str; 4] = ["claude-code", "codex", "openclaw", "mimocode"];
#[cfg(windows)]
const CREATE_NO_WINDOW_FLAG: u32 = 0x08000000;

fn command_for_host<S: AsRef<std::ffi::OsStr>>(program: S) -> Command {
    #[cfg(windows)]
    {
        let mut command = Command::new(program);
        command.creation_flags(CREATE_NO_WINDOW_FLAG);
        command
    }
    #[cfg(not(windows))]
    {
        Command::new(program)
    }
}

#[cfg(any(unix, test))]
fn is_managed_claw_pet_directory(path: &Path) -> bool {
    path.components()
        .any(|component| component.as_os_str() == std::ffi::OsStr::new(".claw-pet"))
}

fn ensure_private_directory(path: &Path) -> Result<(), String> {
    #[cfg(unix)]
    let should_harden = !path.is_dir() || is_managed_claw_pet_directory(path);
    fs::create_dir_all(path).map_err(|error| error.to_string())?;
    #[cfg(unix)]
    if should_harden {
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))
            .map_err(|error| error.to_string())?;
    }
    Ok(())
}

fn write_private_file(path: &Path, bytes: &[u8]) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        ensure_private_directory(parent)?;
    }
    let mut options = OpenOptions::new();
    options.create(true).write(true).truncate(true);
    #[cfg(unix)]
    options.mode(0o600);
    let mut file = options.open(path).map_err(|error| error.to_string())?;
    file.write_all(bytes).map_err(|error| error.to_string())?;
    #[cfg(unix)]
    fs::set_permissions(path, fs::Permissions::from_mode(0o600))
        .map_err(|error| error.to_string())?;
    Ok(())
}

fn write_private_executable(path: &Path, bytes: &[u8]) -> Result<(), String> {
    write_private_file(path, bytes)?;
    #[cfg(unix)]
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))
        .map_err(|error| error.to_string())?;
    Ok(())
}

fn open_private_append_file(path: &Path) -> Result<std::fs::File, String> {
    if let Some(parent) = path.parent() {
        ensure_private_directory(parent)?;
    }
    let mut options = OpenOptions::new();
    options.create(true).append(true);
    #[cfg(unix)]
    options.mode(0o600);
    let file = options.open(path).map_err(|error| error.to_string())?;
    #[cfg(unix)]
    fs::set_permissions(path, fs::Permissions::from_mode(0o600))
        .map_err(|error| error.to_string())?;
    Ok(file)
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct BridgeProfileFile {
    version: u8,
    updated_at: u64,
    desktop_device_id: String,
    mqtt_url: String,
    mqtt_namespace: String,
    mqtt_username: String,
    mqtt_password: String,
    pet_channel_id: String,
    enabled_agents: Vec<String>,
    selected_agent_id: String,
}

#[derive(Debug, Clone, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct PetScreensStoreFile {
    screens: Vec<PetScreenStateFallbackConfig>,
    active_board_device_id: String,
}

#[derive(Debug, Clone, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct PetScreenStateFallbackConfig {
    board_device_id: String,
    host: String,
    ssh_host: String,
    ssh_user: String,
    ssh_port: Option<u16>,
    ssh_password: String,
    ssh_root_dir: String,
}

#[derive(Debug, Clone)]
struct SshStateFallbackTarget {
    host: String,
    port: Option<u16>,
    password: String,
    root_dir: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct BridgeProfileInput {
    desktop_device_id: String,
    mqtt_url: String,
    mqtt_namespace: Option<String>,
    mqtt_username: Option<String>,
    mqtt_password: Option<String>,
    pet_channel_id: Option<String>,
    enabled_agents: Option<Vec<String>>,
    selected_agent_id: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct BridgeProfileResponse {
    version: u8,
    updated_at: u64,
    desktop_device_id: String,
    mqtt_url: String,
    mqtt_namespace: String,
    mqtt_username: String,
    mqtt_password: String,
    pet_channel_id: String,
    enabled_agents: Vec<String>,
    selected_agent_id: String,
    config_path: String,
    topic_base: String,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
struct EnsureBridgeRuntimeInput {
    force_restart: bool,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
struct EnsureVoiceRuntimeInput {
    force_restart: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct VoiceRuntimeStatusResponse {
    configured: bool,
    running: bool,
    pid: Option<u32>,
    host: String,
    port: u16,
    selected_agent_id: String,
    enabled_agents: Vec<String>,
    log_path: String,
    pid_path: String,
    launch_script_path: String,
    executable_path: String,
    resource_root: String,
    message: String,
    mode: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct BridgeRuntimeStatusResponse {
    configured: bool,
    running: bool,
    pid: Option<u32>,
    topic_base: String,
    log_path: String,
    pid_path: String,
    launch_script_path: String,
    launch_agent_path: String,
    auto_start_installed: bool,
    node_path: String,
    bridge_workspace_root: String,
    bridge_entry_path: String,
    message: String,
    mode: String,
}

#[derive(Debug, Clone)]
struct BridgeRuntimePaths {
    config_dir: PathBuf,
    log_path: PathBuf,
    pid_path: PathBuf,
    launch_script_path: PathBuf,
    launch_agent_path: Option<PathBuf>,
}

#[derive(Debug, Clone)]
struct ResolvedBridgeAssets {
    resource_root: PathBuf,
    workspace_root: PathBuf,
    entry_path: PathBuf,
}

#[derive(Debug, Clone)]
struct VoiceRuntimePaths {
    log_path: PathBuf,
    pid_path: PathBuf,
    agent_id_path: PathBuf,
    launch_script_path: PathBuf,
}

#[derive(Debug, Clone)]
struct ResolvedVoiceServiceAssets {
    resource_root: PathBuf,
    executable_path: PathBuf,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct AgentSelectionInput {
    enabled_agents: Vec<String>,
    selected_agent_id: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct AgentSelectionResponse {
    enabled_agents: Vec<String>,
    selected_agent_id: String,
    has_saved_selection: bool,
    config_path: String,
}

// ── Device setup data structures ──

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct WifiStatusResponse {
    interface: String,
    current_ssid: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct WifiConnectResult {
    ok: bool,
    ssid: String,
    message: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct PairingStateResponse {
    board_device_id: String,
    pairing_state: String,
    pairing_mode: String,
    ap_ip: String,
    ap_ssid: String,
    hint: String,
    desktop_device_id: String,
    mqtt_namespace: String,
    last_attempt: Option<serde_json::Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct WifiNetwork {
    ssid: String,
    signal: i32,
    secure: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct WifiScanResponse {
    networks: Vec<WifiNetwork>,
    updated_at: u64,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ApplyConfigInput {
    ssid: String,
    password: String,
    desktop_device_id: Option<String>,
    mqtt_namespace: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct ApplyConfigResponse {
    ok: bool,
    pairing_state: String,
    error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DeviceBinding {
    board_device_id: String,
    desktop_device_id: String,
    wifi_ssid: String,
    bound_at: u64,
}

// ── WiFi operation commands ──

#[tauri::command]
async fn wifi_get_status() -> Result<WifiStatusResponse, String> {
    tauri::async_runtime::spawn_blocking(|| {
        let interface = detect_wifi_interface()?;
        let current_ssid = get_current_ssid(&interface)?;
        Ok(WifiStatusResponse {
            interface,
            current_ssid,
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn wifi_connect_ap() -> Result<WifiConnectResult, String> {
    let result = tauri::async_runtime::spawn_blocking(|| {
        let interface = detect_wifi_interface()?;

        // Check if already connected to the device AP
        if let Ok(Some(ref ssid)) = get_current_ssid(&interface) {
            if ssid == DEVICE_AP_SSID {
                return Ok::<WifiConnectResult, String>(WifiConnectResult {
                    ok: true,
                    ssid: DEVICE_AP_SSID.to_string(),
                    message: format!("已连接到 {DEVICE_AP_SSID}"),
                });
            }
        }

        connect_wifi(&interface, DEVICE_AP_SSID, DEVICE_AP_PASSWORD)?;
        // connect_wifi returns Ok only when networksetup reports success,
        // so we trust it and just wait briefly for the link to stabilize.
        thread::sleep(Duration::from_secs(2));

        Ok(WifiConnectResult {
            ok: true,
            ssid: DEVICE_AP_SSID.to_string(),
            message: format!("已连接到 {DEVICE_AP_SSID}"),
        })
    })
    .await
    .map_err(|e| e.to_string())?;
    result
}

#[tauri::command]
async fn wifi_restore(ssid: String, password: String) -> Result<WifiConnectResult, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let interface = detect_wifi_interface()?;
        connect_wifi(&interface, &ssid, &password)?;
        thread::sleep(Duration::from_secs(2));
        let connected_ssid = get_current_ssid(&interface)?;
        let ok = connected_ssid.as_deref() == Some(ssid.as_str());
        Ok(WifiConnectResult {
            ok,
            ssid: ssid.clone(),
            message: if ok {
                format!("已恢复到 {ssid}")
            } else {
                "恢复网络失败，请手动切换 WiFi".to_string()
            },
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

// ── Device API proxy commands ──

/// HTTP GET with retries. Uses `lan_http_client` so it stays immune to
/// system HTTP/SOCKS proxy env vars (the device's AP is loopback-adjacent).
fn device_http_get(path: &str, timeout_secs: u64, max_retries: u32) -> Result<String, String> {
    let url = format!("http://{}:{}{}", DEVICE_AP_HOST, DEVICE_AP_PORT, path);
    let client = lan_http_client(Duration::from_secs(timeout_secs))?;

    let mut last_err = String::new();
    for attempt in 0..=max_retries {
        if attempt > 0 {
            thread::sleep(Duration::from_secs(2));
        }
        match client.get(&url).send() {
            Ok(resp) => match resp.text() {
                Ok(body) => return Ok(body),
                Err(e) => last_err = e.to_string(),
            },
            Err(e) => last_err = e.to_string(),
        }
    }
    Err(format!(
        "无法连接设备（已重试 {max_retries} 次）: {last_err}"
    ))
}

#[tauri::command]
async fn device_get_pairing_state() -> Result<PairingStateResponse, String> {
    tauri::async_runtime::spawn_blocking(|| {
        let body = device_http_get("/pairing/state", 5, 2)?;
        serde_json::from_str(&body).map_err(|e| format!("解析设备响应失败: {e}"))
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn device_get_wifi_scan() -> Result<WifiScanResponse, String> {
    tauri::async_runtime::spawn_blocking(|| {
        let body = device_http_get("/wifi/scan", 5, 2)?;
        serde_json::from_str(&body).map_err(|e| format!("解析 WiFi 扫描结果失败: {e}"))
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn device_apply_config(input: ApplyConfigInput) -> Result<ApplyConfigResponse, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let desktop_id = match &input.desktop_device_id {
            Some(id) if !id.is_empty() => id.clone(),
            _ => get_or_create_desktop_device_id_inner()?,
        };
        let namespace = input
            .mqtt_namespace
            .unwrap_or_else(|| DEFAULT_NAMESPACE.to_string());

        let payload = serde_json::json!({
            "ssid": input.ssid,
            "password": input.password,
            "desktopDeviceId": desktop_id,
            "mqttNamespace": namespace,
        });

        let url = format!(
            "http://{}:{}/pairing/apply-config",
            DEVICE_AP_HOST, DEVICE_AP_PORT
        );
        let client = lan_http_client(Duration::from_secs(10))?;

        let mut last_err = String::new();
        for attempt in 0..=2u32 {
            if attempt > 0 {
                thread::sleep(Duration::from_secs(2));
            }
            match client.post(&url).json(&payload).send() {
                Ok(resp) => match resp.text() {
                    Ok(body) => {
                        return serde_json::from_str(&body)
                            .map_err(|e| format!("解析配置响应失败: {e}"));
                    }
                    Err(e) => last_err = e.to_string(),
                },
                Err(e) => last_err = e.to_string(),
            }
        }
        Err(format!("无法连接设备（已重试 2 次）: {last_err}"))
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn device_poll_pairing_result() -> Result<PairingStateResponse, String> {
    device_get_pairing_state().await
}

// ── Device availability via bridge ──

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct DeviceAvailabilityEntry {
    online: bool,
    ts: String,
    received_at: String,
    board_device_id: String,
    desktop_device_id: String,
    target_device_id: String,
    target_source: String,
    mqtt_namespace: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct DeviceAvailabilityResponse {
    ok: bool,
    devices: std::collections::HashMap<String, DeviceAvailabilityEntry>,
}

#[tauri::command]
async fn check_device_availability() -> Result<DeviceAvailabilityResponse, String> {
    tauri::async_runtime::spawn_blocking(|| {
        let url = format!(
            "http://127.0.0.1:{}/device-availability",
            DEFAULT_BRIDGE_PORT
        );
        let client = lan_http_client(Duration::from_secs(3))?;
        let resp = client
            .get(&url)
            .send()
            .map_err(|e| format!("Bridge 未运行或无法连接: {e}"))?;
        let body = resp.text().map_err(|e| e.to_string())?;
        serde_json::from_str(&body).map_err(|e| format!("解析设备可用性数据失败: {e}"))
    })
    .await
    .map_err(|e| e.to_string())?
}

// ── Test message via bridge ──

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct PublishTestResponse {
    ok: bool,
    #[serde(default)]
    topic: String,
    #[serde(default)]
    text: String,
    #[serde(default)]
    error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct DispatchRemoteCliBindingInput {
    #[serde(default)]
    board_device_id: String,
    #[serde(default)]
    target_device_id: String,
    #[serde(default)]
    target_source: String,
    #[serde(default)]
    previous_source: String,
    #[serde(default)]
    mqtt_namespace: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct DispatchRemoteCliBindingResponse {
    ok: bool,
    board_device_id: String,
    target_device_id: String,
    target_source: String,
    usb_sent: bool,
    sessions_reset: bool,
    error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct HttpTextResponse {
    status: u16,
    ok: bool,
    body: String,
}

#[tauri::command]
async fn send_test_message(
    desktop_device_id: String,
    namespace: Option<String>,
    text: Option<String>,
) -> Result<PublishTestResponse, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let url = format!("http://127.0.0.1:{}/publish-test", DEFAULT_BRIDGE_PORT);
        let ns = namespace.unwrap_or_else(|| DEFAULT_NAMESPACE.to_string());
        // The device subscribes to {namespace}/{desktopDeviceId}/speech/text
        // so we publish to the desktop's topic, not the board's.
        let payload = serde_json::json!({
            "namespace": ns,
            "deviceId": desktop_device_id,
            "text": text.unwrap_or_default(),
        });
        let client = lan_http_client(Duration::from_secs(5))?;
        let resp = client
            .post(&url)
            .json(&payload)
            .send()
            .map_err(|e| format!("Bridge 未运行或无法连接: {e}"))?;
        let status = resp.status();
        let body = resp.text().map_err(|e| e.to_string())?;
        let parsed: PublishTestResponse =
            serde_json::from_str(&body).map_err(|e| format!("解析响应失败: {e}"))?;
        if !parsed.ok {
            return Err(parsed
                .error
                .unwrap_or_else(|| format!("Bridge 返回错误 (HTTP {})", status)));
        }
        Ok(parsed)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn dispatch_remote_cli_binding(
    input: DispatchRemoteCliBindingInput,
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> Result<DispatchRemoteCliBindingResponse, String> {
    let usb_manager = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let namespace = normalize_topic_segment(
            input
                .mqtt_namespace
                .unwrap_or_else(|| DEFAULT_NAMESPACE.to_string()),
            DEFAULT_NAMESPACE,
        );
        let target_device_id = normalize_topic_segment(input.target_device_id, "");
        let target_source_raw = input.target_source;
        let target_source = normalize_agent_id(&target_source_raw)
            .unwrap_or_else(|| normalize_topic_segment(target_source_raw, ""));
        let previous_source_raw = input.previous_source;
        let previous_source = normalize_agent_id(&previous_source_raw)
            .unwrap_or_else(|| normalize_topic_segment(previous_source_raw, ""));
        let requested_board_device_id = normalize_topic_segment(input.board_device_id, "");
        if target_device_id.is_empty() || target_source.is_empty() {
            return Err("缺少目标桌面设备或渠道。".to_string());
        }

        let usb_status = usb_manager.status();
        if !usb_status.connected {
            return Err("切换跟随需要 USB 连接，请连接设备后重试。".to_string());
        }
        let board_device_id = normalize_topic_segment(usb_status.board_device_id, "");
        if board_device_id.is_empty() {
            return Err("当前 USB 设备身份尚未确认，请稍后重试。".to_string());
        }
        if !requested_board_device_id.is_empty()
            && requested_board_device_id != board_device_id
        {
            return Err(format!(
                "USB 设备已变化：当前连接的是 {board_device_id}，不是目标设备 {requested_board_device_id}。"
            ));
        }

        let payload = serde_json::json!({
            "command": "remote_cli_binding.update",
            "enabled": true,
            "targetDeviceId": target_device_id.clone(),
            "targetSource": target_source.clone(),
            "mqttNamespace": namespace.clone(),
            "updatedBy": "pet-manager",
            "tsMs": current_timestamp_ms(),
        });

        let mut warning = None;
        if !previous_source.is_empty() && previous_source != target_source {
            let disabled_payload = build_disabled_usb_state_payload(&previous_source);
            let disabled_topic = format!("state/{previous_source}");
            if let Err(error) =
                usb_manager.send_to_board(&board_device_id, &disabled_topic, &disabled_payload)
            {
                warning = Some(format!("USB 清理旧渠道失败: {error}"));
            }
        }
        usb_manager
            .send_to_board(
                &board_device_id,
                "control/remote-cli-binding",
                &payload,
            )
            .map_err(|error| format!("USB 下发渠道切换失败: {error}"))?;

        let mut sessions_reset = false;
        if usb_status.runtime.eq_ignore_ascii_case("esp-p4") {
            let mut session_binding = p4_session_binding()
                .lock()
                .map_err(|error| error.to_string())?;
            if p4_session_agent_switch_required(
                &session_binding,
                &board_device_id,
                &target_source,
            ) {
                usb_manager
                    .send_to_board(
                        &board_device_id,
                        "session/current",
                        &serde_json::json!({
                            "sessionId": "auto",
                            "title": "",
                            "index": 0,
                            "count": 0,
                            "sessions": [],
                            "agentId": &target_source,
                            "activeSessionIds": [],
                            "displayEnabled": true,
                            "notice": "",
                        }),
                    )
                    .map_err(|error| format!("USB 清理旧 Agent 会话失败: {error}"))?;
                sessions_reset = reset_p4_session_binding_for_agent(
                    &mut session_binding,
                    &board_device_id,
                    &target_source,
                );
                if sessions_reset {
                    if let Ok(mut audio_binding) = pc_audio_board_binding().lock() {
                        if audio_binding.board_device_id == board_device_id {
                            audio_binding.generation =
                                audio_binding.generation.wrapping_add(1).max(1);
                            audio_binding.active_capture_id = None;
                        }
                    }
                }
            }
        }

        Ok(DispatchRemoteCliBindingResponse {
            ok: true,
            board_device_id,
            target_device_id,
            target_source,
            usb_sent: true,
            sessions_reset,
            error: warning,
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

// ── Desktop device ID ──

#[tauri::command]
fn get_or_create_desktop_device_id(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> Result<String, String> {
    sync_usb_desktop_device_id(usb_manager.inner())
}

fn get_or_create_desktop_device_id_inner() -> Result<String, String> {
    let config_dir = get_home_dir()?.join(".claw-pet");
    ensure_private_directory(&config_dir)?;
    let id_path = config_dir.join(DESKTOP_DEVICE_ID_FILE_NAME);

    if id_path.exists() {
        let id = fs::read_to_string(&id_path)
            .map_err(|e| e.to_string())?
            .trim()
            .to_string();
        if !id.is_empty() {
            return Ok(id);
        }
    }

    let id = format!("desktop-{}", uuid::Uuid::new_v4());
    write_private_file(&id_path, id.as_bytes())?;
    Ok(id)
}

fn sync_usb_desktop_device_id(
    usb_manager: &usb_serial::UsbSerialManager,
) -> Result<String, String> {
    let id = get_or_create_desktop_device_id_inner()?;
    usb_manager.set_desktop_device_id(&id);
    Ok(id)
}

fn is_preview_board_device_id(board_device_id: &str) -> bool {
    let normalized = board_device_id.trim().to_ascii_lowercase();
    normalized.contains("preview")
        || normalized == "board-ethernet-preview-001"
        || normalized == "board-preview-001"
}

// ── Device binding persistence ──

fn upsert_device_binding(bindings: &mut Vec<DeviceBinding>, binding: DeviceBinding) {
    // Drop legacy mock/preview bindings whenever a stable board id is saved.
    if !is_preview_board_device_id(&binding.board_device_id) {
        bindings.retain(|item| !is_preview_board_device_id(&item.board_device_id));
    }

    // App.jsx treats the last binding as the most recently active device.
    // Move an existing binding to the end as well as replacing its contents.
    bindings.retain(|item| item.board_device_id != binding.board_device_id);
    bindings.push(binding);
}

#[tauri::command]
fn save_device_binding(binding: DeviceBinding) -> Result<Vec<DeviceBinding>, String> {
    let bindings_path = get_home_dir()?
        .join(".claw-pet")
        .join(DEVICE_BINDINGS_FILE_NAME);
    let mut bindings = load_device_bindings_inner(&bindings_path)?;

    let desktop_device_id = binding.desktop_device_id.clone();
    upsert_device_binding(&mut bindings, binding);

    let payload = serde_json::to_vec_pretty(&bindings).map_err(|e| e.to_string())?;
    write_private_file(&bindings_path, &payload)?;

    // Auto-create bridge profile if it doesn't exist, so ensure_bridge_runtime
    // can start the bridge without manual setup.
    let config_path = get_bridge_profile_path()?;
    if !config_path.exists() {
        let profile = BridgeProfileFile {
            version: 1,
            updated_at: current_timestamp_ms(),
            desktop_device_id,
            mqtt_url: default_mqtt_url(),
            mqtt_namespace: DEFAULT_NAMESPACE.to_string(),
            mqtt_username: default_mqtt_username(),
            mqtt_password: default_mqtt_password(),
            pet_channel_id: DEFAULT_PET_CHANNEL_ID.to_string(),
            enabled_agents: Vec::new(),
            selected_agent_id: String::new(),
        };
        let profile_payload = serde_json::to_vec_pretty(&profile).map_err(|e| e.to_string())?;
        write_private_file(&config_path, &profile_payload)?;
    }

    Ok(bindings)
}

fn persist_connected_usb_binding(port_name: &str, board_device_id: &str) -> Result<(), String> {
    let board_device_id = board_device_id.trim();
    if board_device_id.is_empty() {
        return Err("USB handshake did not provide a board device id".to_string());
    }
    let desktop_device_id = get_or_create_desktop_device_id_inner()?;
    save_device_binding(DeviceBinding {
        board_device_id: board_device_id.to_string(),
        desktop_device_id,
        wifi_ssid: format!("USB({})", port_name.trim()),
        bound_at: current_timestamp_ms(),
    })?;
    Ok(())
}

#[tauri::command]
#[allow(non_snake_case)]
fn remove_device_binding(boardDeviceId: String) -> Result<Vec<DeviceBinding>, String> {
    let board_device_id = boardDeviceId;

    // Send factory_reset command to the device via MQTT before tearing down the bridge.
    let cmd_url = format!("http://127.0.0.1:{}/publish-command", DEFAULT_BRIDGE_PORT);
    let cmd_body = serde_json::json!({
        "boardDeviceId": &board_device_id,
        "command": "factory_reset"
    });
    if let Ok(client) = lan_http_client(std::time::Duration::from_secs(3)) {
        let _ = client.post(&cmd_url).json(&cmd_body).send();
        // Give the MQTT message time to reach the device.
        std::thread::sleep(std::time::Duration::from_millis(500));
    }

    let bindings_path = get_home_dir()?
        .join(".claw-pet")
        .join(DEVICE_BINDINGS_FILE_NAME);
    let mut bindings = load_device_bindings_inner(&bindings_path)?;
    bindings.retain(|b| b.board_device_id != board_device_id);

    let payload = serde_json::to_vec_pretty(&bindings).map_err(|e| e.to_string())?;
    write_private_file(&bindings_path, &payload)?;

    // Stop bridge and clear its profile since it was bound to the old device.
    let _ = clear_bridge_profile();

    Ok(bindings)
}

/// Resolve the local LAN IP that the OS would use to reach the given peer.
///
/// Uses the well-known UDP-connect trick: opening a UDP socket and "connecting"
/// it to a remote address forces the kernel to pick a source IP, but no packets
/// are actually sent. Falls back to a public-DNS dummy peer when no peer hint
/// is provided so we still get a routable interface IP rather than 127.0.0.1.
fn detect_local_outgoing_ip(peer_hint: Option<&str>) -> Option<String> {
    let probes: [&str; 3] = [
        peer_hint.unwrap_or("8.8.8.8:53"),
        "8.8.8.8:53",
        "223.5.5.5:53",
    ];
    for probe in probes.iter() {
        let target = if probe.contains(':') {
            (*probe).to_string()
        } else {
            format!("{}:53", probe)
        };
        let socket = match UdpSocket::bind("0.0.0.0:0") {
            Ok(s) => s,
            Err(_) => continue,
        };
        if socket.connect(&target).is_err() {
            continue;
        }
        if let Ok(addr) = socket.local_addr() {
            let ip = addr.ip().to_string();
            if !ip.starts_with("127.") && ip != "0.0.0.0" {
                return Some(ip);
            }
        }
    }
    None
}

fn resolve_audio_bridge_pc_ip(
    action: &str,
    pc_ip: Option<String>,
    requires_lan_fields: bool,
) -> Result<Option<String>, String> {
    if action != "start" || !requires_lan_fields {
        return Ok(None);
    }
    pc_ip
        .filter(|value| !value.trim().is_empty())
        .or_else(|| detect_local_outgoing_ip(None))
        .map(Some)
        .ok_or_else(|| "无法自动获取本机 LAN IP, 请显式传 pcIp".to_string())
}

fn normalize_voice_button(input: Option<String>) -> Result<String, String> {
    let value = input.unwrap_or_else(|| "encoder_button.hold".to_string());
    let normalized = value.trim().replace('-', "_").to_lowercase();
    match normalized.as_str() {
        "" => Ok("encoder_button.hold".to_string()),
        "encoder_button"
        | "encoder_button.hold"
        | "rotary_button"
        | "rotary_button.hold"
        | "knob_button.hold" => Ok("encoder_button.hold".to_string()),
        "sw1" | "sw1.hold" | "button.sw1.hold" => Ok("sw1.hold".to_string()),
        "sw2" | "sw2.hold" | "button.sw2.hold" => Ok("sw2.hold".to_string()),
        "sw3" | "sw3.hold" | "button.sw3.hold" => Ok("sw3.hold".to_string()),
        _ => Err(format!(
            "invalid voiceButton '{value}', expected encoder_button.hold or sw1/sw2/sw3.hold"
        )),
    }
}

#[tauri::command]
#[allow(non_snake_case, clippy::too_many_arguments)]
fn audio_bridge_signal(
    boardDeviceId: String,
    action: String,
    pcIp: Option<String>,
    pcPort: Option<u16>,
    listenPort: Option<u16>,
    captureDev: Option<String>,
    playDev: Option<String>,
    voiceButton: Option<String>,
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> Result<serde_json::Value, String> {
    let action = action.trim().to_lowercase();
    if action != "start" && action != "stop" {
        return Err(format!("invalid action '{action}', expected start|stop"));
    }
    let voice_button = normalize_voice_button(voiceButton)?;
    let pcm_relay_port = pcPort.unwrap_or(usb_audio::DEFAULT_PCM_RELAY_PORT);
    if pcm_relay_port == 0 {
        return Err("pcPort must be between 1 and 65535".to_string());
    }
    let usb_status = usb_manager.status();
    let p4_usb_connected =
        usb_status.connected && usb_status.runtime.eq_ignore_ascii_case("esp-p4");

    // Build the JSON object the board runtime expects on
    // `claw-pet/board/<id>/control/command`. Keep field names in sync with
    // board-runtime/src/board_server.c and board-audio-bridge.sh.
    let mut obj = serde_json::Map::new();
    obj.insert(
        "type".to_string(),
        serde_json::Value::String("audio_bridge".to_string()),
    );
    obj.insert(
        "action".to_string(),
        serde_json::Value::String(action.clone()),
    );

    if action == "start" {
        let resolved_ip = resolve_audio_bridge_pc_ip(&action, pcIp.clone(), !p4_usb_connected)?;
        if let Some(resolved_ip) = resolved_ip {
            obj.insert("pc_ip".to_string(), serde_json::Value::String(resolved_ip));
        }
        obj.insert(
            "pc_port".to_string(),
            serde_json::Value::Number(pcm_relay_port.into()),
        );
        if !p4_usb_connected {
            obj.insert(
                "listen_port".to_string(),
                serde_json::Value::Number(listenPort.unwrap_or(50002).into()),
            );
            if let Some(dev) = captureDev.filter(|s| !s.is_empty()) {
                obj.insert("capture_dev".to_string(), serde_json::Value::String(dev));
            }
            if let Some(dev) = playDev.filter(|s| !s.is_empty()) {
                obj.insert("play_dev".to_string(), serde_json::Value::String(dev));
            }
        }
        obj.insert(
            "voice_button".to_string(),
            serde_json::Value::String(voice_button),
        );
    }

    let command_payload = serde_json::Value::Object(obj.clone());
    let mut usb_sent = false;
    let mut usb_error: Option<String> = None;
    if usb_status.connected {
        match usb_manager.send_to_board(&boardDeviceId, "control/command", &command_payload) {
            Ok(()) => usb_sent = true,
            Err(error) => usb_error = Some(error),
        }
    }
    let mut usb_audio_relay_status = serde_json::Value::Null;
    let mut pc_audio_capture_status = serde_json::Value::Null;
    if action == "stop" || usb_sent {
        match usb_audio_relay().lock() {
            Ok(mut relay) => {
                usb_audio_relay_status =
                    relay.configure(action == "start", pcm_relay_port, !p4_usb_connected);
            }
            Err(error) => {
                usb_error = Some(format!("USB audio relay lock failed: {error}"));
            }
        }
        match pc_audio_capture().lock() {
            Ok(mut capture) => {
                let pc_microphone_enabled = action == "start" && !p4_usb_connected;
                pc_audio_capture_status = capture.configure(pc_microphone_enabled, pcm_relay_port);
                if let Ok(mut binding) = pc_audio_board_binding().lock() {
                    binding.generation = binding.generation.wrapping_add(1).max(1);
                    binding.active_capture_id = None;
                    binding.board_device_id.clear();
                }
            }
            Err(error) => {
                usb_error = Some(format!("PC microphone fallback lock failed: {error}"));
            }
        }
    }

    let mut mqtt_sent = false;
    let mut mqtt_error: Option<String> = None;
    let mut bridge_response = serde_json::Value::Null;
    if !p4_usb_connected {
        let cmd_url = format!("http://127.0.0.1:{}/publish-command", DEFAULT_BRIDGE_PORT);
        let body = serde_json::json!({
            "boardDeviceId": &boardDeviceId,
            "payload": command_payload,
        });
        match lan_http_client(std::time::Duration::from_secs(3)).and_then(|client| {
            client
                .post(&cmd_url)
                .json(&body)
                .send()
                .map_err(|e| format!("调用 bridge /publish-command 失败: {e}"))
        }) {
            Ok(resp) => {
                let status = resp.status();
                let text = resp.text().unwrap_or_default();
                if status.is_success() {
                    mqtt_sent = true;
                    bridge_response = serde_json::from_str::<serde_json::Value>(&text)
                        .unwrap_or(serde_json::Value::String(text));
                } else {
                    mqtt_error = Some(format!("bridge 返回 {status}: {text}"));
                }
            }
            Err(error) => mqtt_error = Some(error),
        }
    }

    if !usb_sent && !mqtt_sent {
        return Err(usb_error
            .or(mqtt_error)
            .unwrap_or_else(|| "USB 未连接，且无法通过 MQTT 下发板端音频信令。".to_string()));
    }

    Ok(serde_json::json!({
        "ok": true,
        "boardDeviceId": boardDeviceId,
        "sent": serde_json::Value::Object(obj),
        "usbSent": usb_sent,
        "mqttSent": mqtt_sent,
        "usbError": usb_error,
        "usbAudioRelay": usb_audio_relay_status,
        "pcAudioCapture": pc_audio_capture_status,
        "mqttError": mqtt_error,
        "bridgeResponse": bridge_response,
    }))
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ButtonConfigBinding {
    event: String,
    action: String,
    value: Option<String>,
}

fn is_allowed_button_config_event(event: &str) -> bool {
    matches!(
        event,
        "button.encoder.short_press"
            | "button.encoder.long_press"
            | "knob.rotate_cw / knob.rotate_ccw"
            | "screen.region.tap"
            | "screen.region.long_press"
            | "button.sw1.short_press"
            | "button.sw1.long_press"
            | "button.sw1.hold"
            | "button.sw2.short_press"
            | "button.sw2.long_press"
            | "button.sw2.hold"
            | "button.sw3.short_press"
            | "button.sw3.long_press"
            | "button.sw3.hold"
            | "button.encoder.hold"
            | "knob.rotate_cw"
            | "knob.rotate_ccw"
            | "joystick.up"
            | "joystick.down"
    )
}

fn is_allowed_button_config_action(action: &str) -> bool {
    matches!(
        action,
        "voice_ptt"
            | "system_page"
            | "system_reset"
            | "volume_adjust"
            | "agent_enter"
            | "agent_prompt"
            | "session_next"
            | "session_previous"
            | "session_clear"
            | "miniapp_screen_tap"
            | "miniapp_screen_long_press"
            | "component_center"
            | "miniapp_action"
            | "page_toggle"
            | "page_enter"
            | "page_back"
            | "page_main"
            | "page_app"
            | "disabled"
    )
}

#[tauri::command]
fn set_p4_session_binding(
    input: SetP4SessionBindingInput,
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> Result<P4SessionBinding, String> {
    let board_device_id = input.board_device_id.trim().to_string();
    let agent_id = normalize_agent_id(&input.agent_id)
        .ok_or_else(|| "请选择一个受支持的 Agent".to_string())?;
    let raw_session_id = input.session_id.trim();
    let session_id = if raw_session_id.is_empty() || raw_session_id.eq_ignore_ascii_case("auto") {
        String::new()
    } else {
        if board_device_id.is_empty() {
            return Err("请选择已连接的 P4 设备后再绑定会话".to_string());
        }
        if raw_session_id.len() > 512 {
            return Err("会话 ID 过长".to_string());
        }
        raw_session_id.to_string()
    };
    let session_title = input.session_title.trim();
    if session_title.len() > 1024 {
        return Err("会话名称过长".to_string());
    }
    let device_title = input
        .device_title
        .as_deref()
        .unwrap_or(session_title)
        .trim()
        .to_string();
    if device_title.len() > 1024 {
        return Err("设备会话名称过长".to_string());
    }
    let session_cwd = input.session_cwd.trim();
    if session_cwd.len() > 2048 {
        return Err("会话工作目录过长".to_string());
    }
    let notice = input.notice.trim();
    if notice.len() > 256 {
        return Err("会话切换提示过长".to_string());
    }
    let session_count = input.session_count;
    let session_index = if session_count > 0 {
        input.session_index.min(session_count)
    } else {
        0
    };
    if input.sessions.len() > 8 {
        return Err("设备会话队列最多支持 8 项".to_string());
    }
    if input.active_session_ids.len() > 8 {
        return Err("设备活跃会话 ID 最多支持 8 项".to_string());
    }
    let mut active_session_ids = Vec::with_capacity(input.active_session_ids.len());
    for session_id in &input.active_session_ids {
        let session_id = session_id.trim();
        if session_id.is_empty() || session_id.len() > 128 {
            return Err("设备活跃会话 ID 无效".to_string());
        }
        active_session_ids.push(session_id.to_string());
    }
    let session_title_unique = input.session_title_unique
        && p4_session_target_is_unique(session_title, session_cwd, &input.sessions);
    let mut device_sessions = Vec::with_capacity(input.sessions.len());
    for session in &input.sessions {
        let id = session.id.trim();
        let title = session.title.trim();
        let cwd = session.cwd.trim();
        let content = session.content.trim();
        let state = session.state.trim().to_ascii_lowercase();
        if id.is_empty() || id.len() > 512 {
            return Err("设备会话 ID 无效".to_string());
        }
        if title.len() > 1024 {
            return Err("设备会话名称过长".to_string());
        }
        if cwd.len() > 2048 {
            return Err("设备会话工作目录过长".to_string());
        }
        if content.len() > 2048 {
            return Err("设备会话进展内容过长".to_string());
        }
        if state.len() > 32 {
            return Err("设备会话状态过长".to_string());
        }
        validate_p4_session_transition_metadata(
            &state,
            session.transition_revision,
            session.terminal_remaining_ms,
        )?;
        device_sessions.push(serde_json::json!({
            "id": id,
            "title": title,
            "content": content,
            "state": if state.is_empty() { "idle" } else { state.as_str() },
            "transitionRevision": session.transition_revision,
            "terminalRemainingMs": session.terminal_remaining_ms,
        }));
    }

    let mut binding = p4_session_binding()
        .lock()
        .map_err(|error| error.to_string())?;
    if should_preserve_exact_auto_binding(
        &binding,
        &board_device_id,
        &agent_id,
        &session_id,
        input.auto_follow,
    ) {
        return Ok(binding.clone());
    }
    let target_changed = binding.board_device_id != board_device_id
        || binding.agent_id != agent_id
        || binding.session_id != session_id
        || binding.auto_follow != input.auto_follow
        || binding.session_title != session_title
        || binding.session_cwd != session_cwd
        || binding.session_title_unique != session_title_unique;
    if target_changed {
        binding.generation = binding.generation.wrapping_add(1).max(1);
    }
    binding.board_device_id = board_device_id.clone();
    binding.agent_id = agent_id;
    binding.session_id = session_id;
    binding.auto_follow = input.auto_follow;
    binding.session_title = session_title.to_string();
    binding.session_cwd = session_cwd.to_string();
    binding.session_title_unique = session_title_unique;
    let navigation_requested =
        should_locate_desktop_session(input.locate_desktop, &binding.agent_id);
    if target_changed || navigation_requested {
        binding.desktop_location = if navigation_requested {
            "pending"
        } else if agent_uses_visible_composer(&binding.agent_id) {
            "not_requested"
        } else {
            "not_applicable"
        }
        .to_string();
        binding.desktop_location_error.clear();
    }

    // A changed target must not let an already-recorded PC microphone capture
    // inject into the previously selected session.
    if target_changed {
        if let Ok(mut audio_binding) = pc_audio_board_binding().lock() {
            if !board_device_id.is_empty() && audio_binding.board_device_id == board_device_id {
                audio_binding.generation = audio_binding.generation.wrapping_add(1).max(1);
                audio_binding.active_capture_id = None;
            }
        }
    }
    let navigation_generation = binding.generation;
    let navigation_agent = binding.agent_id.clone();
    let navigation_id = binding.session_id.clone();
    let navigation_title = binding.session_title.clone();
    let navigation_cwd = binding.session_cwd.clone();
    #[cfg(not(target_os = "macos"))]
    let navigation_title_unique = binding.session_title_unique;
    drop(binding);

    if navigation_requested {
        let navigation_is_claude = navigation_agent == "claude-code";
        #[cfg(target_os = "macos")]
        let navigation_result = if navigation_id.is_empty() {
            Err("当前会话没有可定位的 session ID".to_string())
        } else if navigation_is_claude {
            codex_composer::CodexComposerBridge::focus_claude_session(
                &navigation_id,
                &navigation_title,
                &navigation_cwd,
            )
        } else {
            codex_composer::CodexComposerBridge::focus_session(
                &navigation_id,
                &navigation_title,
                &navigation_cwd,
            )
        };
        #[cfg(not(target_os = "macos"))]
        let navigation_result = if navigation_title.is_empty() {
            Err("当前会话没有可定位的标题".to_string())
        } else if !navigation_is_claude && !navigation_title_unique {
            Err("当前 ChatGPT（Codex）会话标题不唯一，已停止自动定位".to_string())
        } else if navigation_is_claude {
            codex_composer::CodexComposerBridge::focus_claude_session(
                &navigation_id,
                &navigation_title,
                &navigation_cwd,
            )
        } else {
            codex_composer::CodexComposerBridge::focus_session(
                &navigation_id,
                &navigation_title,
                &navigation_cwd,
            )
        };
        if let Ok(mut current) = p4_session_binding().lock() {
            if current.generation == navigation_generation {
                match navigation_result {
                    Ok(()) => {
                        current.desktop_location = "located".to_string();
                        current.desktop_location_error.clear();
                    }
                    Err(error) => {
                        eprintln!("[p4-session] desktop location failed: {error}");
                        current.desktop_location = "failed".to_string();
                        current.desktop_location_error = error;
                    }
                }
            }
        }
    }

    let result = p4_session_binding()
        .lock()
        .map_err(|error| error.to_string())?
        .clone();

    if navigation_requested && result.generation != navigation_generation {
        return Ok(result);
    }

    let status = usb_manager.status();
    if status.connected && status.board_device_id == board_device_id {
        let displayed_session_id = if result.session_id.is_empty() {
            "auto"
        } else {
            result.session_id.as_str()
        };
        let device_notice = if notice.is_empty() || !navigation_requested {
            notice.to_string()
        } else if result.desktop_location == "located" {
            "已切换并定位会话".to_string()
        } else {
            "会话已切换，客户端定位失败".to_string()
        };
        usb_manager.send_to_board(
            &board_device_id,
            "session/current",
            &serde_json::json!({
                "sessionId": displayed_session_id,
                "title": device_title,
                "index": session_index,
                "count": session_count,
                "sessions": device_sessions,
                "agentId": &result.agent_id,
                "activeSessionIds": active_session_ids,
                "displayEnabled": input.display_enabled,
                "notice": device_notice,
                "desktopLocation": &result.desktop_location,
                "desktopLocationError": &result.desktop_location_error,
            }),
        )?;
    }

    Ok(result)
}

fn send_button_config_and_wait_for_ack(
    usb_manager: &usb_serial::UsbSerialManager,
    expected_board_device_id: &str,
    topic: &str,
    request_id: &str,
    command_payload: &serde_json::Value,
    fallback_binding_count: usize,
) -> Result<(serde_json::Value, u64), String> {
    eprintln!(
        "[button-config] sending topic={} requestId={}",
        topic, request_id
    );
    let ack_receiver = register_button_config_ack_waiter(request_id)?;
    if let Err(error) = usb_manager.send_to_board(expected_board_device_id, topic, command_payload)
    {
        remove_button_config_ack_waiter(request_id);
        return Err(format!("USB OTA 下发按钮配置失败: {error}"));
    }

    let ack_payload =
        match ack_receiver.recv_timeout(Duration::from_secs(BUTTON_CONFIG_ACK_TIMEOUT_SECS)) {
            Ok(payload) => payload,
            Err(_) => {
                remove_button_config_ack_waiter(request_id);
                eprintln!("[button-config] ack timeout requestId={}", request_id);
                return Err(BUTTON_CONFIG_ACK_TIMEOUT_MESSAGE.to_string());
            }
        };

    if ack_payload.get("ok").and_then(|v| v.as_bool()) != Some(true) {
        return Err(ack_payload
            .get("message")
            .and_then(|v| v.as_str())
            .unwrap_or("板端写入按钮配置失败")
            .to_string());
    }
    let binding_count = ack_payload
        .get("bindingCount")
        .and_then(|v| v.as_u64())
        .unwrap_or(fallback_binding_count as u64);
    Ok((ack_payload, binding_count))
}

#[tauri::command]
#[allow(non_snake_case)]
async fn button_config_signal(
    app_handle: tauri::AppHandle,
    boardDeviceId: String,
    bindings: Vec<ButtonConfigBinding>,
    requestId: Option<String>,
    voiceButton: Option<String>,
    voiceEnabled: Option<bool>,
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> Result<serde_json::Value, String> {
    let usb_status = usb_manager.status();
    if !usb_status.connected {
        return Err("USB 未连接,无法通过 USB OTA 下发按钮配置".to_string());
    }
    if bindings.is_empty() {
        return Err("按钮配置为空,无法下发".to_string());
    }
    let voice_button = normalize_voice_button(voiceButton)?;
    let command_topic = if usb_status.runtime.eq_ignore_ascii_case("esp-p4") {
        "input/config"
    } else {
        "control/command"
    };
    let mut normalized_bindings = Vec::with_capacity(bindings.len());
    for binding in bindings {
        let event = binding.event.trim();
        let action = binding.action.trim();
        if !is_allowed_button_config_event(event) {
            return Err(format!("invalid button event '{event}'"));
        }
        if !is_allowed_button_config_action(action) {
            return Err(format!("invalid button action '{action}'"));
        }
        let value = binding.value.as_deref().unwrap_or("").trim();
        if value.len() >= 160 {
            return Err("button action value exceeds 159 UTF-8 bytes".to_string());
        }
        normalized_bindings.push(serde_json::json!({
            "event": event,
            "action": action,
            "value": value,
        }));
    }
    let request_id = requestId
        .as_deref()
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| format!("button-config-{}", current_timestamp_ms()));

    let command_payload = serde_json::json!({
        "type": "button_config",
        "version": 1,
        "request_id": request_id.clone(),
        "requestId": request_id.clone(),
        "voice_button": voice_button,
        "voice_enabled": voiceEnabled.unwrap_or(false),
        "bindings": normalized_bindings,
    });

    let manager = usb_manager.inner().clone();
    let wait_app_handle = app_handle.clone();
    let wait_board_device_id = boardDeviceId.clone();
    let wait_request_id = request_id.clone();
    let wait_command_payload = command_payload.clone();
    let fallback_binding_count = normalized_bindings.len();
    let (ack_payload, binding_count) = tauri::async_runtime::spawn_blocking(move || {
        manager.with_asset_transfer_guard(|| {
            match send_button_config_and_wait_for_ack(
                &manager,
                &wait_board_device_id,
                command_topic,
                &wait_request_id,
                &wait_command_payload,
                fallback_binding_count,
            ) {
                Ok(result) => Ok(result),
                Err(error) if error == BUTTON_CONFIG_ACK_TIMEOUT_MESSAGE => {
                    reconnect_usb_serial_for_command(&wait_app_handle, &manager)?;
                    send_button_config_and_wait_for_ack(
                        &manager,
                        &wait_board_device_id,
                        command_topic,
                        &wait_request_id,
                        &wait_command_payload,
                        fallback_binding_count,
                    )
                }
                Err(error) => Err(error),
            }
        })
    })
    .await
    .map_err(|error| error.to_string())??;

    Ok(serde_json::json!({
        "ok": true,
        "boardDeviceId": boardDeviceId,
        "requestId": request_id,
        "sent": command_payload,
        "usbSent": true,
        "bindingCount": binding_count,
        "message": ack_payload.get("message").cloned().unwrap_or_else(|| serde_json::json!("button config written")),
        "ack": ack_payload,
    }))
}

#[tauri::command]
fn load_device_bindings() -> Result<Vec<DeviceBinding>, String> {
    let bindings_path = get_home_dir()?
        .join(".claw-pet")
        .join(DEVICE_BINDINGS_FILE_NAME);
    let bindings = load_device_bindings_inner(&bindings_path)?;
    Ok(bindings
        .into_iter()
        .filter(|item| !is_preview_board_device_id(&item.board_device_id))
        .collect())
}

fn load_device_bindings_inner(path: &Path) -> Result<Vec<DeviceBinding>, String> {
    if !path.exists() {
        return Ok(Vec::new());
    }
    let content = fs::read_to_string(path).map_err(|e| e.to_string())?;
    serde_json::from_str(&content).map_err(|e| format!("解析绑定数据失败: {e}"))
}

// ── WiFi helper functions (cross-platform) ──

struct WifiConnectAttemptOutcome {
    success: bool,
    retryable: bool,
    error: String,
}

impl WifiConnectAttemptOutcome {
    fn success() -> Self {
        Self {
            success: true,
            retryable: false,
            error: String::new(),
        }
    }

    fn failure(retryable: bool, error: String) -> Self {
        Self {
            success: false,
            retryable,
            error,
        }
    }
}

fn run_wifi_connect_with_retry<F>(max_attempts: usize, mut attempt_once: F) -> Result<(), String>
where
    F: FnMut(usize) -> Result<WifiConnectAttemptOutcome, String>,
{
    let mut last_err = "WiFi 连接失败".to_string();
    for attempt in 0..max_attempts {
        if attempt > 0 {
            thread::sleep(Duration::from_secs(2 + attempt as u64));
        }

        let outcome = attempt_once(attempt)?;
        if outcome.success {
            return Ok(());
        }

        if !outcome.error.trim().is_empty() {
            last_err = outcome.error;
        }
        if !outcome.retryable {
            return Err(last_err);
        }
    }

    Err(format!(
        "{last_err}\n(已重试 {max_attempts} 次，请确认设备已开机并进入配网模式)"
    ))
}

#[cfg(target_os = "macos")]
fn detect_wifi_interface() -> Result<String, String> {
    let output = command_for_host("networksetup")
        .arg("-listallhardwareports")
        .output()
        .map_err(|e| format!("执行 networksetup 失败: {e}"))?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let mut found_wifi = false;
    for line in stdout.lines() {
        if line.contains("Wi-Fi") || line.contains("AirPort") {
            found_wifi = true;
            continue;
        }
        if found_wifi && line.starts_with("Device:") {
            return Ok(line.trim_start_matches("Device:").trim().to_string());
        }
    }
    Err("未找到 Wi-Fi 网络接口".to_string())
}

#[cfg(target_os = "windows")]
fn parse_line_value_after_colon(line: &str) -> Option<String> {
    line.split_once(':')
        .map(|(_, value)| value.trim().to_string())
        .filter(|value| !value.is_empty())
}

#[cfg(target_os = "windows")]
fn detect_wifi_interface() -> Result<String, String> {
    let output = command_for_host("netsh")
        .args(["wlan", "show", "interfaces"])
        .output()
        .map_err(|e| format!("执行 netsh 失败: {e}"))?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    for line in stdout.lines() {
        let trimmed = line.trim();
        // Match both English "Name" and Chinese "名称"
        if trimmed.starts_with("Name") || trimmed.starts_with("名称") {
            if let Some(name) = parse_line_value_after_colon(trimmed) {
                return Ok(name);
            }
        }
    }
    Err("未找到 Wi-Fi 网络接口".to_string())
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
fn detect_wifi_interface() -> Result<String, String> {
    Ok("wlan0".to_string())
}

#[cfg(target_os = "macos")]
fn get_current_ssid(interface: &str) -> Result<Option<String>, String> {
    let output = command_for_host("networksetup")
        .args(["-getairportnetwork", interface])
        .output()
        .map_err(|e| format!("获取当前 WiFi 失败: {e}"))?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let text = stdout.trim();
    if text.contains("not associated") || text.contains("未关联") || text.is_empty() {
        return Ok(None);
    }
    if let Some(pos) = text.rfind(": ") {
        let ssid = text[pos + 2..].trim();
        if !ssid.is_empty() {
            return Ok(Some(ssid.to_string()));
        }
    }
    Ok(None)
}

#[cfg(target_os = "windows")]
fn get_current_ssid(_interface: &str) -> Result<Option<String>, String> {
    let output = command_for_host("netsh")
        .args(["wlan", "show", "interfaces"])
        .output()
        .map_err(|e| format!("获取当前 WiFi 失败: {e}"))?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    for line in stdout.lines() {
        let trimmed = line.trim();
        // Match "SSID" but not "BSSID"
        if trimmed.starts_with("SSID") && !trimmed.starts_with("BSSID") {
            if let Some(ssid) = parse_line_value_after_colon(trimmed) {
                return Ok(Some(ssid));
            }
        }
    }
    Ok(None)
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
fn get_current_ssid(_interface: &str) -> Result<Option<String>, String> {
    Ok(None)
}

#[cfg(target_os = "macos")]
fn trigger_wifi_scan(_interface: &str) {
    let airport =
        "/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport";
    if Path::new(airport).exists() {
        if let Ok(mut child) = command_for_host(airport)
            .args(["-s"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
        {
            let start = std::time::Instant::now();
            loop {
                match child.try_wait() {
                    Ok(Some(_)) => break,
                    Ok(None) => {
                        if start.elapsed() > Duration::from_secs(6) {
                            let _ = child.kill();
                            break;
                        }
                        thread::sleep(Duration::from_millis(200));
                    }
                    Err(_) => break,
                }
            }
        }
    }
}

#[cfg(target_os = "macos")]
fn connect_wifi(interface: &str, ssid: &str, password: &str) -> Result<(), String> {
    run_wifi_connect_with_retry(10, |_| {
        trigger_wifi_scan(interface);
        thread::sleep(Duration::from_millis(1500));
        let output = command_for_host("networksetup")
            .args(["-setairportnetwork", interface, ssid, password])
            .output()
            .map_err(|e| format!("执行 WiFi 连接失败: {e}"))?;
        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);
        let text = stdout.trim();
        if text.is_empty() && stderr.trim().is_empty() {
            return Ok(WifiConnectAttemptOutcome::success());
        }
        let error = if !text.is_empty() {
            text.to_string()
        } else {
            stderr.trim().to_string()
        };
        Ok(WifiConnectAttemptOutcome::failure(
            error.contains("Could not find network"),
            error,
        ))
    })
}

#[cfg(target_os = "windows")]
fn connect_wifi(_interface: &str, ssid: &str, password: &str) -> Result<(), String> {
    // On Windows, netsh requires a saved profile to connect. Create a temporary
    // profile XML, add it, then connect.
    let profile_xml = format!(
        r#"<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
    <name>{ssid}</name>
    <SSIDConfig>
        <SSID><name>{ssid}</name></SSID>
    </SSIDConfig>
    <connectionType>ESS</connectionType>
    <connectionMode>manual</connectionMode>
    <MSM>
        <security>
            <authEncryption>
                <authentication>WPA2PSK</authentication>
                <encryption>AES</encryption>
                <useOneX>false</useOneX>
            </authEncryption>
            <sharedKey>
                <keyType>passPhrase</keyType>
                <protected>false</protected>
                <keyMaterial>{password}</keyMaterial>
            </sharedKey>
        </security>
    </MSM>
</WLANProfile>"#
    );

    // Write profile to a temp file
    let temp_dir = env::temp_dir();
    let profile_path = temp_dir.join(format!("claw-pet-wifi-{}.xml", ssid));
    fs::write(&profile_path, &profile_xml).map_err(|e| format!("写入 WiFi 配置文件失败: {e}"))?;

    // Add profile
    let output = command_for_host("netsh")
        .args(["wlan", "add", "profile"])
        .arg(format!("filename={}", profile_path.display()))
        .output()
        .map_err(|e| format!("添加 WiFi 配置失败: {e}"))?;
    let _ = fs::remove_file(&profile_path);

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        return Err(format!(
            "添加 WiFi 配置失败: {} {}",
            stdout.trim(),
            stderr.trim()
        ));
    }

    run_wifi_connect_with_retry(10, |_| {
        let output = command_for_host("netsh")
            .args(["wlan", "connect", &format!("name={ssid}")])
            .output()
            .map_err(|e| format!("执行 WiFi 连接失败: {e}"))?;
        let stdout = String::from_utf8_lossy(&output.stdout);

        // Wait for connection to establish
        thread::sleep(Duration::from_secs(3));

        // Verify we actually connected
        if let Ok(Some(ref current)) = get_current_ssid("") {
            if current == ssid {
                return Ok(WifiConnectAttemptOutcome::success());
            }
        }
        let error = {
            let raw = stdout.trim().to_string();
            if raw.is_empty() {
                format!("连接到 {ssid} 失败")
            } else {
                raw
            }
        };
        Ok(WifiConnectAttemptOutcome::failure(true, error))
    })
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
fn connect_wifi(_interface: &str, _ssid: &str, _password: &str) -> Result<(), String> {
    Err("当前平台不支持自动切换 WiFi，请手动连接".to_string())
}

// ── Existing bridge commands ──

#[tauri::command]
fn load_bridge_profile() -> Result<BridgeProfileResponse, String> {
    let config_path = get_bridge_profile_path()?;
    let profile = read_bridge_profile(&config_path)?.unwrap_or_default();
    Ok(build_bridge_profile_response(
        &config_path,
        apply_bridge_profile_defaults(normalize_bridge_profile(profile)),
    ))
}

fn merge_bridge_profile_input(
    existing: BridgeProfileFile,
    input: BridgeProfileInput,
) -> BridgeProfileFile {
    let mqtt_namespace = input.mqtt_namespace.unwrap_or_else(|| {
        if existing.mqtt_namespace.is_empty() {
            DEFAULT_NAMESPACE.to_string()
        } else {
            existing.mqtt_namespace.clone()
        }
    });
    let pet_channel_id = input.pet_channel_id.unwrap_or_else(|| {
        if existing.pet_channel_id.is_empty() {
            DEFAULT_PET_CHANNEL_ID.to_string()
        } else {
            existing.pet_channel_id.clone()
        }
    });
    normalize_bridge_profile(BridgeProfileFile {
        version: 1,
        updated_at: current_timestamp_ms(),
        desktop_device_id: input.desktop_device_id,
        mqtt_url: input.mqtt_url,
        mqtt_namespace,
        mqtt_username: input.mqtt_username.unwrap_or(existing.mqtt_username),
        mqtt_password: input.mqtt_password.unwrap_or(existing.mqtt_password),
        pet_channel_id,
        enabled_agents: input.enabled_agents.unwrap_or(existing.enabled_agents),
        selected_agent_id: input
            .selected_agent_id
            .unwrap_or(existing.selected_agent_id),
    })
}

#[tauri::command]
fn save_bridge_profile(input: BridgeProfileInput) -> Result<BridgeProfileResponse, String> {
    let config_path = get_bridge_profile_path()?;
    let existing = read_bridge_profile(&config_path)?.unwrap_or_default();
    let profile = merge_bridge_profile_input(existing, input);

    if profile.desktop_device_id.is_empty() {
        return Err("Desktop ID 不能为空。".to_string());
    }

    if profile.mqtt_url.is_empty() {
        return Err("MQTT URL 不能为空。".to_string());
    }

    if let Some(parent_dir) = config_path.parent() {
        ensure_private_directory(parent_dir)?;
    }

    let payload = serde_json::to_vec_pretty(&profile).map_err(|error| error.to_string())?;
    write_private_file(&config_path, &payload)?;

    Ok(build_bridge_profile_response(&config_path, profile))
}

#[tauri::command]
fn clear_bridge_profile() -> Result<BridgeProfileResponse, String> {
    let config_path = get_bridge_profile_path()?;
    let runtime_paths = resolve_bridge_runtime_paths(&config_path)?;

    stop_managed_bridge(&runtime_paths.pid_path);
    thread::sleep(Duration::from_millis(180));

    if config_path.exists() {
        fs::remove_file(&config_path).map_err(|error| error.to_string())?;
    }

    Ok(build_bridge_profile_response(
        &config_path,
        apply_bridge_profile_defaults(normalize_bridge_profile(BridgeProfileFile::default())),
    ))
}

#[tauri::command]
fn load_agent_selection() -> Result<AgentSelectionResponse, String> {
    let config_path = get_bridge_profile_path()?;
    let raw_profile = read_bridge_profile(&config_path)?.unwrap_or_default();
    let has_saved_selection =
        !raw_profile.selected_agent_id.trim().is_empty() || !raw_profile.enabled_agents.is_empty();
    let profile = normalize_bridge_profile(raw_profile);
    Ok(AgentSelectionResponse {
        enabled_agents: profile.enabled_agents,
        selected_agent_id: profile.selected_agent_id,
        has_saved_selection,
        config_path: config_path.display().to_string(),
    })
}

#[tauri::command]
fn save_agent_selection(input: AgentSelectionInput) -> Result<AgentSelectionResponse, String> {
    let config_path = get_bridge_profile_path()?;
    let mut profile = read_bridge_profile(&config_path)?.unwrap_or_default();
    profile.version = 1;
    profile.updated_at = current_timestamp_ms();
    profile.enabled_agents = input.enabled_agents;
    profile.selected_agent_id = input.selected_agent_id.unwrap_or_default();
    profile = normalize_bridge_profile(profile);

    if let Some(parent_dir) = config_path.parent() {
        ensure_private_directory(parent_dir)?;
    }
    let payload = serde_json::to_vec_pretty(&profile).map_err(|error| error.to_string())?;
    write_private_file(&config_path, &payload)?;

    Ok(AgentSelectionResponse {
        enabled_agents: profile.enabled_agents,
        selected_agent_id: profile.selected_agent_id,
        has_saved_selection: true,
        config_path: config_path.display().to_string(),
    })
}

fn bridge_runtime_lifecycle_lock() -> &'static Mutex<()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
}

fn voice_runtime_lifecycle_lock() -> &'static Mutex<()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
}

#[tauri::command]
fn ensure_bridge_runtime(
    app_handle: tauri::AppHandle,
    input: Option<EnsureBridgeRuntimeInput>,
) -> Result<BridgeRuntimeStatusResponse, String> {
    let _lifecycle_guard = bridge_runtime_lifecycle_lock()
        .lock()
        .map_err(|_| "Bridge 生命周期锁异常，请重启 Pet Manager 后重试。".to_string())?;
    let force_restart = input.unwrap_or_default().force_restart;
    let config_path = get_bridge_profile_path()?;
    let raw_profile = read_bridge_profile(&config_path)?.unwrap_or_default();
    // Check if the profile was explicitly saved (has a real desktop_device_id),
    // not just filled in by defaults.
    let has_saved_profile = !raw_profile.desktop_device_id.is_empty();
    let mut profile = apply_bridge_profile_defaults(normalize_bridge_profile(raw_profile));
    let runtime_paths = resolve_bridge_runtime_paths(&config_path)?;

    if !has_saved_profile {
        // Try to recover profile from device bindings so the bridge can start
        // even if pet-bridge.json was deleted (e.g. after unbind + re-pair).
        let bindings_path = get_home_dir()?
            .join(".claw-pet")
            .join(DEVICE_BINDINGS_FILE_NAME);
        let bindings = load_device_bindings_inner(&bindings_path)?;
        if let Some(binding) = bindings
            .iter()
            .find(|item| !is_preview_board_device_id(&item.board_device_id))
        {
            profile = apply_bridge_profile_defaults(normalize_bridge_profile(BridgeProfileFile {
                version: 1,
                updated_at: current_timestamp_ms(),
                desktop_device_id: binding.desktop_device_id.clone(),
                mqtt_url: default_mqtt_url(),
                mqtt_namespace: DEFAULT_NAMESPACE.to_string(),
                mqtt_username: default_mqtt_username(),
                mqtt_password: default_mqtt_password(),
                pet_channel_id: DEFAULT_PET_CHANNEL_ID.to_string(),
                enabled_agents: Vec::new(),
                selected_agent_id: String::new(),
            }));
            let payload = serde_json::to_vec_pretty(&profile).map_err(|e| e.to_string())?;
            let _ = write_private_file(&config_path, &payload);
        } else {
            return Ok(build_bridge_runtime_status(
                &profile,
                &runtime_paths,
                None,
                None,
                false,
                "inactive",
                "填写 Desktop ID 并保存后，Pet Manager 会自动拉起本地 bridge。".to_string(),
            ));
        }
    }

    let bridge_assets = resolve_bridge_assets(&app_handle)?;
    let node_path = resolve_node_path(&app_handle)?;

    // Best-effort: write the launch script for manual debugging; don't block
    // bridge startup if this fails (e.g. macOS permission issues).
    let _ = write_launch_script(
        &runtime_paths.launch_script_path,
        &runtime_paths.log_path,
        &profile,
        &bridge_assets,
        &node_path,
    );
    let auto_start_installed =
        install_bridge_autostart(&runtime_paths, &profile, &bridge_assets, &node_path)
            .unwrap_or(false);

    stop_bridge_launch_agent(&runtime_paths);

    if force_restart {
        stop_managed_bridge(&runtime_paths.pid_path);
        stop_legacy_bridge_runtime();
        thread::sleep(Duration::from_millis(180));
    }

    stop_legacy_bridge_runtime();

    let bridge_http_running = probe_bridge_running(DEFAULT_BRIDGE_PORT);
    let agent_bus_running = probe_agent_bus_running(DEFAULT_AGENT_BUS_PORT);
    let mut running = bridge_http_running && agent_bus_running;
    let mut pid = read_live_managed_pid(&runtime_paths.pid_path);

    // A busy but live managed Bridge can miss the short health timeout while
    // it refreshes an Agent session list. Never launch a second process on the
    // same strict port: give the existing child a bounded recovery window, and
    // stop it before replacement if it remains unhealthy.
    if !running {
        if let Some(existing_pid) = pid.filter(|candidate| process_exists(*candidate)) {
            running = wait_for_bridge_ready(DEFAULT_BRIDGE_PORT, 6, 250);
            if !running {
                eprintln!(
                    "[bridge-runtime] managed bridge {existing_pid} is alive but unresponsive; replacing it"
                );
                stop_managed_bridge(&runtime_paths.pid_path);
                thread::sleep(Duration::from_millis(180));
                pid = None;
            }
        }
    }

    // If a bridge is already running but it's an old external process (not ours),
    // kill it so we can start the bundled version with all endpoints.
    if pid.is_none() && (bridge_http_running || agent_bus_running) {
        // A recognized Bridge endpoint without our PID file is an external/old
        // runtime. Replace the complete stack so 23333 and 8181 stay in sync.
        if bridge_http_running {
            stop_process_on_port(DEFAULT_BRIDGE_PORT);
        }
        if agent_bus_running {
            stop_process_on_port(DEFAULT_AGENT_BUS_PORT);
        }
        thread::sleep(Duration::from_millis(300));
        running = false;
    }

    let mut mode = if running { "ready" } else { "launching" };
    let mut message = if running {
        format!("bridge 已连接，正在发布到 {}。", build_topic_base(&profile))
    } else {
        format!("正在拉起 bridge，并连接到 {}。", build_topic_base(&profile))
    };

    if !running {
        (running, pid) =
            launch_bridge_runtime(&node_path, &bridge_assets, &profile, &runtime_paths)?;
        mode = if running { "ready" } else { "error" };
        message = if running {
            format!("bridge 已启动，正在发布到 {}。", build_topic_base(&profile))
        } else {
            format!(
                "bridge 已尝试启动，但 Bridge 或 Agent Bus 健康检查未通过。日志：{}",
                runtime_paths.log_path.display()
            )
        };
    }

    if running {
        if let Ok(Some(status)) = fetch_bridge_agent_status() {
            if bridge_agent_status_needs_restart(&profile, &status) {
                eprintln!(
                    "[bridge-runtime] selected agent adapter is recoverably unhealthy; restarting bridge"
                );
                stop_managed_bridge(&runtime_paths.pid_path);
                stop_process_on_port(DEFAULT_BRIDGE_PORT);
                thread::sleep(Duration::from_millis(250));
                (running, pid) =
                    launch_bridge_runtime(&node_path, &bridge_assets, &profile, &runtime_paths)?;
                mode = if running { "ready" } else { "error" };
                message = if running {
                    format!(
                        "bridge 已自愈重启，正在发布到 {}。",
                        build_topic_base(&profile)
                    )
                } else {
                    format!(
                        "bridge 已尝试自愈重启，但进程未能连接本地状态端口。日志：{}",
                        runtime_paths.log_path.display()
                    )
                };
            }
        }
    }

    Ok(build_bridge_runtime_status(
        &profile,
        &runtime_paths,
        Some(&bridge_assets),
        Some(&node_path),
        auto_start_installed,
        mode,
        message,
    )
    .with_runtime(running, pid))
}

#[tauri::command]
fn stop_bridge_runtime(
    app_handle: tauri::AppHandle,
) -> Result<BridgeRuntimeStatusResponse, String> {
    let config_path = get_bridge_profile_path()?;
    let profile = apply_bridge_profile_defaults(normalize_bridge_profile(
        read_bridge_profile(&config_path)?.unwrap_or_default(),
    ));
    let runtime_paths = resolve_bridge_runtime_paths(&config_path)?;
    let bridge_assets = resolve_bridge_assets(&app_handle).ok();
    let node_path = resolve_node_path(&app_handle).ok();
    let auto_start_installed = runtime_paths
        .launch_agent_path
        .as_ref()
        .map(|path| path.exists())
        .unwrap_or(false);
    let pid_before = read_pid(&runtime_paths.pid_path);
    let running_before = probe_bridge_running(DEFAULT_BRIDGE_PORT);

    stop_bridge_launch_agent(&runtime_paths);

    if pid_before.is_some() {
        stop_managed_bridge(&runtime_paths.pid_path);
        thread::sleep(Duration::from_millis(180));
    }
    stop_legacy_bridge_runtime();

    let running_after = probe_bridge_running(DEFAULT_BRIDGE_PORT);
    let pid_after = read_pid(&runtime_paths.pid_path);
    let (mode, message) = if running_after {
        if pid_before.is_none() {
            (
                "ready",
                "检测到 bridge 仍在运行，但当前没有 Pet Manager 的 pid 记录，无法直接断开。"
                    .to_string(),
            )
        } else {
            (
                "error",
                "已发送断开请求，但 bridge 仍在运行。请检查日志或手动结束进程。".to_string(),
            )
        }
    } else if pid_before.is_some() || running_before {
        (
            "inactive",
            format!("已断开 {} 的 MQTT bridge。", build_topic_base(&profile)),
        )
    } else {
        (
            "inactive",
            "当前没有正在运行的本地 MQTT bridge。".to_string(),
        )
    };

    Ok(build_bridge_runtime_status(
        &profile,
        &runtime_paths,
        bridge_assets.as_ref(),
        node_path.as_ref(),
        auto_start_installed,
        mode,
        message,
    )
    .with_runtime(running_after, pid_after))
}

#[tauri::command]
fn load_device_asr_settings() -> Result<volcengine_asr::DeviceAsrSettingsStatus, String> {
    volcengine_asr::settings_status()
}

#[tauri::command]
fn save_device_asr_settings(
    input: volcengine_asr::DeviceAsrSettingsInput,
) -> Result<volcengine_asr::DeviceAsrSettingsStatus, String> {
    volcengine_asr::save_settings(input)
}

#[tauri::command]
async fn test_device_asr_settings() -> Result<volcengine_asr::DeviceAsrProbeStatus, String> {
    volcengine_asr::probe_saved_settings().await
}

#[tauri::command]
fn ensure_device_voice_runtime(
    input: Option<EnsureDeviceVoiceRuntimeInput>,
) -> Result<serde_json::Value, String> {
    let interactive = input.map(|value| value.interactive).unwrap_or(true);
    let status = if interactive {
        volcengine_asr::settings_status()?
    } else {
        volcengine_asr::settings_status_for_automatic_restore()?
    };
    Ok(serde_json::json!({
        "configured": status.configured,
        "running": status.configured && !status.deferred,
        "deferred": status.deferred,
        "provider": status.provider,
        "mode": status.mode,
        "endpoint": status.endpoint,
        "resourceId": status.resource_id,
        "credentialSource": status.credential_source,
        "port": 0,
        "message": status.message,
    }))
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct EnsureDeviceVoiceRuntimeInput {
    interactive: bool,
}

#[tauri::command]
fn ensure_voice_runtime(
    app_handle: tauri::AppHandle,
    input: Option<EnsureVoiceRuntimeInput>,
) -> Result<VoiceRuntimeStatusResponse, String> {
    let _lifecycle_guard = voice_runtime_lifecycle_lock()
        .lock()
        .map_err(|_| "语音服务生命周期锁异常，请重启 Pet Manager 后重试。".to_string())?;
    let force_restart = input.unwrap_or_default().force_restart;
    let config_path = get_bridge_profile_path()?;
    let profile = normalize_bridge_profile(read_bridge_profile(&config_path)?.unwrap_or_default());
    let runtime_paths = resolve_voice_runtime_paths(&config_path)?;
    let voice_assets_result = resolve_voice_service_assets(&app_handle);
    let fallback_assets = ResolvedVoiceServiceAssets {
        resource_root: PathBuf::new(),
        executable_path: PathBuf::new(),
    };

    // Defensive guard: voice-service-node bakes VOICE_AGENT_ID into the
    // worker child's env at spawn time, so starting it without a
    // resolved selection produces a worker that throws ConfigError on
    // every job dispatch. Just report inactive and let the front-end
    // try again once the user has picked an agent.
    if profile.selected_agent_id.trim().is_empty() {
        let pid = read_pid(&runtime_paths.pid_path);
        if pid.is_some() {
            stop_managed_process(&runtime_paths.pid_path);
        }
        if probe_voice_service_running(DEFAULT_VOICE_SERVICE_PORT) {
            stop_process_on_port(DEFAULT_VOICE_SERVICE_PORT);
        }
        let _ = fs::remove_file(&runtime_paths.agent_id_path);
        return Ok(build_voice_runtime_status(
            &profile,
            &runtime_paths,
            voice_assets_result.as_ref().unwrap_or(&fallback_assets),
            "inactive",
            "暂未选择编程工具（agent），voice-service 不会启动。请在仪表盘选一个 agent 后重试。"
                .to_string(),
        )
        .with_runtime(false, None));
    }

    if voice_assets_result.is_err() {
        if let Ok(recognizer) = pc_audio::built_in_stt_status() {
            let mut status = build_voice_runtime_status(
                &profile,
                &runtime_paths,
                &fallback_assets,
                "windows-stt",
                format!(
                    "P4 语音已使用 Windows 本地识别器，识别文本将交给当前 Agent：{}（{}）",
                    profile.selected_agent_id, recognizer
                ),
            );
            status.configured = true;
            status.running = true;
            status.port = 0;
            return Ok(status);
        }
    }

    let voice_assets = voice_assets_result?;
    let node_path = resolve_node_path(&app_handle)?;

    // Best-effort: write the launch script for manual debugging even if
    // we end up not spawning anything below. A future user-initiated
    // selection will pick up the existing script.
    let _ = write_voice_launch_script(
        &runtime_paths.launch_script_path,
        &runtime_paths.log_path,
        &voice_assets,
        &node_path,
        &profile,
    );

    let mut running = probe_voice_service_running(DEFAULT_VOICE_SERVICE_PORT);
    let had_pid_file = read_pid(&runtime_paths.pid_path).is_some();
    let mut pid = read_live_managed_pid(&runtime_paths.pid_path);
    if had_pid_file && pid.is_none() {
        let _ = fs::remove_file(&runtime_paths.agent_id_path);
    }
    let launched_agent_id = fs::read_to_string(&runtime_paths.agent_id_path)
        .unwrap_or_default()
        .trim()
        .to_string();
    let agent_changed =
        (running || pid.is_some()) && launched_agent_id != profile.selected_agent_id;

    if force_restart || agent_changed {
        if pid.is_some() {
            stop_managed_process(&runtime_paths.pid_path);
        }
        if probe_voice_service_running(DEFAULT_VOICE_SERVICE_PORT) {
            stop_process_on_port(DEFAULT_VOICE_SERVICE_PORT);
        }
        let _ = fs::remove_file(&runtime_paths.agent_id_path);
        thread::sleep(Duration::from_millis(180));
        running = false;
        pid = None;
    }

    if !running {
        if let Some(existing_pid) = pid.filter(|candidate| process_exists(*candidate)) {
            running = wait_for_voice_service_ready(DEFAULT_VOICE_SERVICE_PORT, 6, 250);
            if !running {
                eprintln!(
                    "[voice-runtime] managed voice-service {existing_pid} is alive but unresponsive; replacing it"
                );
                stop_managed_process(&runtime_paths.pid_path);
                let _ = fs::remove_file(&runtime_paths.agent_id_path);
                thread::sleep(Duration::from_millis(180));
                pid = None;
            }
        }
    }

    if running && pid.is_none() {
        stop_process_on_port(DEFAULT_VOICE_SERVICE_PORT);
        thread::sleep(Duration::from_millis(180));
        running = false;
    }

    if !running {
        let started_pid = start_voice_service_direct(
            &node_path,
            &voice_assets,
            &profile,
            &runtime_paths.log_path,
            &runtime_paths.pid_path,
        )?;
        if let Err(error) = write_private_file(
            &runtime_paths.agent_id_path,
            profile.selected_agent_id.as_bytes(),
        ) {
            stop_managed_process(&runtime_paths.pid_path);
            return Err(error);
        }
        pid = Some(started_pid);
        running = wait_for_voice_service_ready(DEFAULT_VOICE_SERVICE_PORT, 50, 200);
        if !running {
            stop_managed_process(&runtime_paths.pid_path);
            let _ = fs::remove_file(&runtime_paths.agent_id_path);
            pid = None;
        }
    }

    let mode = if running { "ready" } else { "error" };
    let message = if running {
        format!(
            "voice-service 已启动，当前 agent 为 {}。",
            profile.selected_agent_id
        )
    } else {
        "voice-service 已尝试启动，但当前还没有连上本地 8080 端口。请检查日志。".to_string()
    };

    Ok(
        build_voice_runtime_status(&profile, &runtime_paths, &voice_assets, mode, message)
            .with_runtime(running, pid),
    )
}

#[tauri::command]
fn stop_voice_runtime(app_handle: tauri::AppHandle) -> Result<VoiceRuntimeStatusResponse, String> {
    let _lifecycle_guard = voice_runtime_lifecycle_lock()
        .lock()
        .map_err(|_| "语音服务生命周期锁异常，请重启 Pet Manager 后重试。".to_string())?;
    let config_path = get_bridge_profile_path()?;
    let profile = normalize_bridge_profile(read_bridge_profile(&config_path)?.unwrap_or_default());
    let runtime_paths = resolve_voice_runtime_paths(&config_path)?;
    let voice_assets = resolve_voice_service_assets(&app_handle).ok();
    let pid_before = read_pid(&runtime_paths.pid_path);
    let running_before = probe_voice_service_running(DEFAULT_VOICE_SERVICE_PORT);

    if pid_before.is_some() {
        stop_managed_process(&runtime_paths.pid_path);
        thread::sleep(Duration::from_millis(180));
    }
    if probe_voice_service_running(DEFAULT_VOICE_SERVICE_PORT) {
        stop_process_on_port(DEFAULT_VOICE_SERVICE_PORT);
        thread::sleep(Duration::from_millis(180));
    }
    let _ = fs::remove_file(&runtime_paths.agent_id_path);

    let running_after = probe_voice_service_running(DEFAULT_VOICE_SERVICE_PORT);
    let pid_after = read_pid(&runtime_paths.pid_path);
    let (mode, message) = if running_after {
        (
            "error",
            "已发送断开请求，但 voice-service 仍在运行。请检查日志或手动结束进程。".to_string(),
        )
    } else if pid_before.is_some() || running_before {
        ("inactive", "已断开本地 voice-service。".to_string())
    } else {
        (
            "inactive",
            "当前没有正在运行的本地 voice-service。".to_string(),
        )
    };

    let fallback_assets = ResolvedVoiceServiceAssets {
        resource_root: PathBuf::new(),
        executable_path: PathBuf::new(),
    };
    Ok(build_voice_runtime_status(
        &profile,
        &runtime_paths,
        voice_assets.as_ref().unwrap_or(&fallback_assets),
        mode,
        message,
    )
    .with_runtime(running_after, pid_after))
}

// ── Local agent detection ──

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DetectedAgent {
    id: String,
    label: String,
    detected: bool,
    ready: bool,
    status: String,
    detail: String,
    command_path: String,
    config_path: String,
    activity_path: String,
    can_sync_hook: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct AgentDiscoveryResponse {
    scanned_at: u64,
    agents: Vec<DetectedAgent>,
}

/// Resolve the user's full login-shell PATH.  GUI apps on macOS/Linux inherit
/// a minimal PATH that misses ~/.npm-global/bin, nvm shims, cargo bin, etc.
/// We run the user's default shell in login mode to get the real PATH.
fn get_full_shell_path() -> Option<String> {
    #[cfg(unix)]
    {
        let shell = env::var("SHELL").unwrap_or_else(|_| "sh".to_string());
        let output = std::process::Command::new(&shell)
            .args(["-l", "-c", "echo $PATH"])
            .output()
            .ok()?;
        let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
        if path.is_empty() {
            None
        } else {
            Some(path)
        }
    }
    #[cfg(windows)]
    {
        // GUI apps on Windows may inherit a minimal PATH.  Read the full
        // user + system PATH from the registry and merge them so we can
        // find executables installed via nvm-windows, npm global, etc.
        get_full_path_from_registry()
            .or_else(|| {
                // Fallback: ask PowerShell for the merged PATH
                command_for_host("powershell")
                    .args(["-NoProfile", "-Command", "[Environment]::GetEnvironmentVariable('PATH','Machine') + ';' + [Environment]::GetEnvironmentVariable('PATH','User')"])
                    .output()
                    .ok()
                    .and_then(|o| {
                        let p = String::from_utf8_lossy(&o.stdout).trim().to_string();
                        if p.is_empty() { None } else { Some(p) }
                    })
            })
            .or_else(|| env::var("PATH").ok())
    }
}

#[cfg(windows)]
fn get_full_path_from_registry() -> Option<String> {
    use winreg::enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    use winreg::RegKey;

    let system_path = RegKey::predef(HKEY_LOCAL_MACHINE)
        .open_subkey(r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment")
        .ok()
        .and_then(|key| key.get_value::<String, _>("Path").ok())
        .unwrap_or_default();

    let user_path = RegKey::predef(HKEY_CURRENT_USER)
        .open_subkey(r"Environment")
        .ok()
        .and_then(|key| key.get_value::<String, _>("Path").ok())
        .unwrap_or_default();

    let merged = format!("{};{}", system_path, user_path);
    let merged = merged.trim_matches(';').to_string();
    if merged.is_empty() {
        None
    } else {
        Some(merged)
    }
}

/// Discover npm's global bin directory by running `npm config get prefix`.
/// Returns `<prefix>/bin` on unix, `<prefix>` on Windows (npm puts .cmd there directly).
fn get_npm_global_bin() -> Option<PathBuf> {
    let npm_name = if cfg!(windows) { "npm.cmd" } else { "npm" };
    let output = command_for_host(npm_name)
        .args(["config", "get", "prefix"])
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    let prefix = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if prefix.is_empty() {
        return None;
    }
    let bin_dir = if cfg!(windows) {
        PathBuf::from(&prefix)
    } else {
        PathBuf::from(&prefix).join("bin")
    };
    if bin_dir.is_dir() {
        Some(bin_dir)
    } else {
        None
    }
}

fn find_executable(name: &str, extra_paths: &[&str]) -> Option<String> {
    // 1. Check explicit extra paths first
    for path in extra_paths {
        let full = PathBuf::from(path);
        if full.is_file() {
            return Some(full.to_string_lossy().to_string());
        }
    }

    let executable_names = {
        #[cfg(windows)]
        {
            let mut names = vec![name.to_string()];
            let lower = name.to_ascii_lowercase();
            if !(lower.ends_with(".exe")
                || lower.ends_with(".cmd")
                || lower.ends_with(".bat")
                || lower.ends_with(".com"))
            {
                names.push(format!("{name}.exe"));
                names.push(format!("{name}.cmd"));
                names.push(format!("{name}.bat"));
            }
            names
        }
        #[cfg(not(windows))]
        {
            vec![name.to_string()]
        }
    };

    // 2. Check npm global bin (handles custom prefix like ~/.npm-global)
    if let Some(npm_bin) = get_npm_global_bin() {
        for executable_name in &executable_names {
            let candidate = npm_bin.join(executable_name);
            if candidate.is_file() {
                return Some(candidate.to_string_lossy().to_string());
            }
        }
    }

    // 3. Search using the user's full shell PATH (not just the GUI app's minimal PATH)
    let search_path = get_full_shell_path()
        .or_else(|| env::var("PATH").ok())
        .unwrap_or_default();
    for dir in env::split_paths(&std::ffi::OsString::from(&search_path)) {
        for executable_name in &executable_names {
            let candidate = dir.join(executable_name);
            if candidate.is_file() {
                return Some(candidate.to_string_lossy().to_string());
            }
        }
    }

    None
}

fn require_host_command(name: &str, purpose: &str) -> Result<String, String> {
    find_executable(name, &[]).ok_or_else(|| {
        if cfg!(windows) {
            format!(
                "{purpose}，但未找到本机命令 `{name}`。Windows 上请启用/安装 OpenSSH 客户端和 tar，或改用 USB 下发。"
            )
        } else {
            format!("{purpose}，但未找到本机命令 `{name}`。请先安装后重试，或改用 USB 下发。")
        }
    })
}

fn find_agent_executable(
    name: &str,
    home: Option<&Path>,
    home_relative_candidates: &[&str],
    windows_apps_name: &str,
) -> Option<String> {
    let mut extra = Vec::new();

    if let Some(home_dir) = home {
        for relative in home_relative_candidates {
            extra.push(home_dir.join(relative).to_string_lossy().to_string());
        }
    }

    #[cfg(windows)]
    {
        if let Some(local_app_data) = env::var_os("LOCALAPPDATA") {
            if name.eq_ignore_ascii_case("codex") {
                let local_app_data_path = PathBuf::from(&local_app_data);
                extra.push(
                    local_app_data_path
                        .join("OpenAI")
                        .join("Codex")
                        .join("bin")
                        .join("codex.exe")
                        .to_string_lossy()
                        .to_string(),
                );

                let packages_root = local_app_data_path.join("Packages");
                if let Ok(entries) = fs::read_dir(&packages_root) {
                    for entry in entries.flatten() {
                        let file_name = entry.file_name().to_string_lossy().to_string();
                        if !file_name.starts_with("OpenAI.Codex_") {
                            continue;
                        }
                        let package_bin_dir = entry
                            .path()
                            .join("LocalCache")
                            .join("Local")
                            .join("OpenAI")
                            .join("Codex")
                            .join("bin");
                        extra.push(
                            package_bin_dir
                                .join("codex.exe")
                                .to_string_lossy()
                                .to_string(),
                        );
                        if let Ok(bin_children) = fs::read_dir(&package_bin_dir) {
                            for child in bin_children.flatten() {
                                if child.path().is_dir() {
                                    extra.push(
                                        child
                                            .path()
                                            .join("codex.exe")
                                            .to_string_lossy()
                                            .to_string(),
                                    );
                                }
                            }
                        }
                    }
                }
            }
            extra.push(
                PathBuf::from(local_app_data)
                    .join("Microsoft")
                    .join("WindowsApps")
                    .join(windows_apps_name)
                    .to_string_lossy()
                    .to_string(),
            );
        }
    }
    #[cfg(not(windows))]
    {
        let _ = windows_apps_name;
    }

    let extra_refs: Vec<&str> = extra.iter().map(|s| s.as_str()).collect();
    find_executable(name, &extra_refs)
}

#[cfg(target_os = "macos")]
fn macos_app_bundle_candidates(
    home: Option<&Path>,
    app_names: &[&str],
    bundle_identifiers: &[&str],
) -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(home_dir) = home {
        for app_name in app_names {
            push_unique_path(
                &mut candidates,
                home_dir.join("Applications").join(app_name),
            );
        }
    }

    if let Some(mdfind) = find_executable("mdfind", &[]) {
        for bundle_identifier in bundle_identifiers {
            let output = command_for_host(&mdfind)
                .arg(format!(
                    "kMDItemCFBundleIdentifier == '{bundle_identifier}'"
                ))
                .output();
            let Ok(output) = output else {
                continue;
            };
            for line in String::from_utf8_lossy(&output.stdout).lines() {
                let candidate = PathBuf::from(line.trim());
                if candidate.is_dir() {
                    push_unique_path(&mut candidates, candidate);
                }
            }
        }
    }

    candidates
}

#[cfg(target_os = "macos")]
fn macos_codex_app_cli_candidates(home: Option<&Path>) -> Vec<PathBuf> {
    macos_app_bundle_candidates(
        home,
        &["ChatGPT.app", "Codex.app"],
        &["com.openai.chatgpt", "com.openai.codex"],
    )
    .into_iter()
    .map(|app| app.join("Contents").join("Resources").join("codex"))
    .collect()
}

fn find_codex_executable(home: Option<&Path>) -> Option<String> {
    // The desktop app and its bundled CLI are released together. Prefer that
    // compatible CLI over an older global npm installation, while still
    // letting Codex inherit the user's active profile and model configuration.
    #[cfg(target_os = "macos")]
    {
        for candidate in macos_codex_app_cli_candidates(home) {
            if candidate.is_file() {
                return Some(candidate.to_string_lossy().to_string());
            }
        }
    }

    find_agent_executable("codex", home, &[".local/bin/codex"], "codex.exe")
}

fn find_claude_desktop_install() -> Option<String> {
    #[cfg(target_os = "windows")]
    {
        let packages_root = env::var_os("LOCALAPPDATA")
            .map(PathBuf::from)?
            .join("Packages");
        if let Ok(entries) = fs::read_dir(packages_root) {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().to_string();
                if entry.path().is_dir() && name.starts_with("Claude_") {
                    return Some(entry.path().to_string_lossy().to_string());
                }
            }
        }
    }

    #[cfg(target_os = "macos")]
    {
        let home = get_home_dir().ok();
        for path in macos_app_bundle_candidates(
            home.as_deref(),
            &["Claude.app", "Claude Desktop.app"],
            &["com.anthropic.claudefordesktop", "com.anthropic.claude"],
        ) {
            if path.is_dir() {
                return Some(path.to_string_lossy().to_string());
            }
        }
    }

    None
}

fn detect_claude_code() -> DetectedAgent {
    let home = get_home_dir().ok();
    let cli = find_agent_executable(
        "claude",
        home.as_deref(),
        &[".local/bin/claude", ".claude/local/claude"],
        "claude.exe",
    );
    let desktop = find_claude_desktop_install();
    let settings_path = home
        .as_ref()
        .map(|path| {
            path.join(".claude")
                .join("settings.json")
                .to_string_lossy()
                .to_string()
        })
        .unwrap_or_default();
    let has_hooks = !settings_path.is_empty() && Path::new(&settings_path).exists();

    let detected = cli.is_some() || desktop.is_some();
    let status = if !detected {
        "not_found"
    } else if has_hooks {
        "ready"
    } else {
        "needs_hook"
    }
    .to_string();
    let installed = match (&desktop, &cli) {
        (Some(_), Some(path)) => format!("Claude Desktop + CLI: {path}"),
        (Some(path), None) => format!("Claude Desktop: {path}"),
        (None, Some(path)) => format!("Claude CLI: {path}"),
        (None, None) => "未检测到 Claude 客户端或 CLI".to_string(),
    };
    let detail = if status == "needs_hook" {
        format!("{installed}；需要配置 hooks")
    } else {
        installed
    };

    DetectedAgent {
        id: "claude-code".to_string(),
        label: "Claude".to_string(),
        detected,
        ready: status == "ready",
        status,
        detail,
        command_path: cli.unwrap_or_default(),
        config_path: settings_path,
        activity_path: desktop.unwrap_or_default(),
        can_sync_hook: detected,
    }
}

fn detect_codex() -> DetectedAgent {
    let home = get_home_dir().ok();
    let cli_path = find_codex_executable(home.as_deref());
    let sessions_dir = home
        .as_ref()
        .map(|path| {
            path.join(".codex")
                .join("sessions")
                .to_string_lossy()
                .to_string()
        })
        .unwrap_or_default();
    let logs_sqlite_path = home
        .as_ref()
        .map(|path| {
            path.join(".codex")
                .join("logs_2.sqlite")
                .to_string_lossy()
                .to_string()
        })
        .unwrap_or_default();
    let has_sessions = !sessions_dir.is_empty() && Path::new(&sessions_dir).exists();
    let has_logs_sqlite = !logs_sqlite_path.is_empty() && Path::new(&logs_sqlite_path).exists();

    let mut desktop_markers: Vec<String> = Vec::new();
    #[cfg(windows)]
    {
        if let Some(app_data) = env::var_os("APPDATA") {
            desktop_markers.push(
                PathBuf::from(app_data)
                    .join("Codex")
                    .to_string_lossy()
                    .to_string(),
            );
        }
    }
    #[cfg(target_os = "macos")]
    {
        if let Some(home_dir) = home.as_ref() {
            desktop_markers.push(
                home_dir
                    .join("Library")
                    .join("Application Support")
                    .join("Codex")
                    .to_string_lossy()
                    .to_string(),
            );
        }
    }
    #[cfg(target_os = "linux")]
    {
        if let Some(home_dir) = home.as_ref() {
            desktop_markers.push(
                home_dir
                    .join(".config")
                    .join("Codex")
                    .to_string_lossy()
                    .to_string(),
            );
        }
    }
    let desktop_path = desktop_markers
        .iter()
        .find(|candidate| Path::new(candidate.as_str()).exists())
        .cloned();

    let has_activity_data = has_sessions || has_logs_sqlite;
    let (detected, status, detail) = match &cli_path {
        Some(path) => {
            if has_activity_data {
                (
                    true,
                    "ready".to_string(),
                    format!("已检测到 Codex CLI 与本地会话数据: {}", path),
                )
            } else {
                (
                    true,
                    "ready".to_string(),
                    format!("已检测到 Codex CLI（等待会话数据生成）: {}", path),
                )
            }
        }
        None => {
            if has_activity_data {
                (
                    true,
                    "ready".to_string(),
                    "已检测到 ChatGPT（Codex）客户端本地会话数据（未发现 CLI 入口）".to_string(),
                )
            } else if let Some(client_path) = desktop_path.as_ref() {
                (
                    true,
                    "ready".to_string(),
                    format!("已检测到 ChatGPT（Codex）桌面客户端: {}", client_path),
                )
            } else {
                (
                    false,
                    "not_found".to_string(),
                    "未检测到 ChatGPT（Codex）桌面客户端或 Codex CLI".to_string(),
                )
            }
        }
    };

    let activity_path = if has_sessions {
        sessions_dir.clone()
    } else if has_logs_sqlite {
        logs_sqlite_path.clone()
    } else {
        String::new()
    };

    DetectedAgent {
        id: "codex".to_string(),
        label: "ChatGPT（Codex）".to_string(),
        detected,
        ready: status == "ready",
        status,
        detail,
        command_path: cli_path.unwrap_or_default(),
        config_path: String::new(),
        activity_path,
        can_sync_hook: false,
    }
}

fn detect_openclaw() -> DetectedAgent {
    let home = get_home_dir().ok();
    let cli_path = find_agent_executable(
        "openclaw",
        home.as_deref(),
        &[".local/bin/openclaw", ".npm-global/bin/openclaw"],
        "openclaw.exe",
    );
    let config_path = home
        .as_ref()
        .map(|path| {
            path.join(".status-bridge")
                .join("openclaw-device.json")
                .to_string_lossy()
                .to_string()
        })
        .unwrap_or_default();
    let has_config = !config_path.is_empty() && Path::new(&config_path).exists();

    let (detected, status, detail) = match (&cli_path, has_config) {
        (Some(path), true) => (
            true,
            "ready".to_string(),
            format!("已检测到 OpenClaw CLI 与 Gateway: {}", path),
        ),
        (Some(path), false) => (
            true,
            "ready".to_string(),
            format!("已检测到 OpenClaw CLI（等待 Gateway 配置）: {}", path),
        ),
        (None, true) => (
            true,
            "ready".to_string(),
            "OpenClaw Gateway 已配置（未发现 CLI 入口）".to_string(),
        ),
        (None, false) => (
            false,
            "not_found".to_string(),
            "未检测到 OpenClaw CLI 或 Gateway".to_string(),
        ),
    };

    DetectedAgent {
        id: "openclaw".to_string(),
        label: "OpenClaw".to_string(),
        detected,
        ready: status == "ready",
        status,
        detail,
        command_path: cli_path.unwrap_or_default(),
        config_path,
        activity_path: String::new(),
        can_sync_hook: false,
    }
}

fn detect_mimocode() -> DetectedAgent {
    let home = get_home_dir().ok();
    let cli_path = find_agent_executable(
        "mimo",
        home.as_deref(),
        &[
            ".mimocode/bin/mimo",
            ".mimocode/bin/mimo.exe",
            ".local/bin/mimo",
        ],
        "mimo.exe",
    );
    let config_path = home
        .as_ref()
        .map(|path| {
            path.join(".config")
                .join("mimocode")
                .join("plugin")
                .join("pet-manager.js")
                .to_string_lossy()
                .to_string()
        })
        .unwrap_or_default();
    let activity_path = home
        .as_ref()
        .map(|path| {
            path.join(".local")
                .join("share")
                .join("mimocode")
                .join("mimocode.db")
                .to_string_lossy()
                .to_string()
        })
        .unwrap_or_default();
    let plugin_ready = !config_path.is_empty() && Path::new(&config_path).is_file();

    let (detected, status, detail) = match &cli_path {
        Some(path) if plugin_ready => (
            true,
            "ready".to_string(),
            format!("已检测到 MiMoCode CLI，Pet Manager 状态插件已就绪: {path}"),
        ),
        Some(path) => (
            true,
            "ready".to_string(),
            format!("已检测到 MiMoCode CLI，启动 Bridge 后自动同步状态插件: {path}"),
        ),
        None => (
            false,
            "not_found".to_string(),
            "未检测到 MiMoCode CLI".to_string(),
        ),
    };

    DetectedAgent {
        id: "mimocode".to_string(),
        label: "MiMoCode".to_string(),
        detected,
        ready: status == "ready",
        status,
        detail,
        command_path: cli_path.unwrap_or_default(),
        config_path,
        activity_path,
        can_sync_hook: detected,
    }
}

#[tauri::command]
fn detect_local_agents() -> Result<AgentDiscoveryResponse, String> {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);

    let agents = vec![
        detect_claude_code(),
        detect_codex(),
        detect_openclaw(),
        detect_mimocode(),
    ];

    Ok(AgentDiscoveryResponse {
        scanned_at: now,
        agents,
    })
}

/// Download raw bytes from a URL on the Rust side, bypassing plugin-http's
/// `new Headers(responseHeaders)` path which throws `TypeError` when the
/// server returns header values containing bytes > 0xFF (e.g. non-ASCII
/// `Content-Disposition` filenames from Volcano TOS CDN).
#[tauri::command]
async fn download_bytes(url: String) -> Result<Vec<u8>, String> {
    eprintln!("[download_bytes] GET {url}");
    let started = std::time::Instant::now();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let client = reqwest::blocking::Client::builder()
            .timeout(Duration::from_secs(120))
            .build()
            .map_err(|e| format!("build client: {e}"))?;
        let url_for_err = url.clone();
        let response = client
            .get(&url)
            .send()
            .map_err(|e| format!("GET {url_for_err}: {e}"))?;
        let status = response.status();
        if !status.is_success() {
            return Err(format!(
                "HTTP {} downloading {url_for_err}",
                status.as_u16()
            ));
        }
        let bytes = response.bytes().map_err(|e| format!("read body: {e}"))?;
        Ok(bytes.to_vec())
    })
    .await
    .map_err(|e| format!("join blocking task: {e}"))?;
    match &result {
        Ok(bytes) => eprintln!(
            "[download_bytes] OK {} bytes in {} ms",
            bytes.len(),
            started.elapsed().as_millis()
        ),
        Err(e) => eprintln!(
            "[download_bytes] ERR in {} ms: {e}",
            started.elapsed().as_millis()
        ),
    }
    result
}

/// Run a text-based HTTP request on the Rust side so the desktop app does not
/// depend on WebView CORS or JS-side plugin probing for avatar-generation APIs.
#[tauri::command]
async fn http_request_text(
    url: String,
    method: Option<String>,
    headers_json: Option<String>,
    body: Option<String>,
    timeout_ms: Option<u64>,
) -> Result<HttpTextResponse, String> {
    let method = if method.as_deref().unwrap_or("").trim().is_empty() {
        "GET".to_string()
    } else {
        method.clone().unwrap().trim().to_uppercase()
    };
    let url = url.trim().to_string();
    if url.is_empty() {
        return Err("http_request_text: url is required".to_string());
    }

    let headers: HashMap<String, String> = match headers_json {
        Some(raw) if !raw.trim().is_empty() => {
            serde_json::from_str(&raw).map_err(|e| format!("invalid headersJson: {e}"))?
        }
        _ => HashMap::new(),
    };

    eprintln!("[http_request_text] {} {}", method, url);
    let started = std::time::Instant::now();
    let log_method = method.clone();
    let log_url = url.clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let timeout_ms = timeout_ms.unwrap_or(120_000).max(1);
        let client = reqwest::blocking::Client::builder()
            .timeout(Duration::from_millis(timeout_ms))
            .build()
            .map_err(|e| format!("build client: {e}"))?;

        let req_method = reqwest::Method::from_bytes(method.as_bytes())
            .map_err(|e| format!("invalid method {method}: {e}"))?;
        let mut request = client.request(req_method.clone(), &url);

        for (name, value) in headers {
            request = request.header(&name, value);
        }
        if let Some(body) = body {
            request = request.body(body);
        }

        let response = request.send().map_err(|e| format!("{method} {url}: {e}"))?;
        let status = response.status();
        let body = response
            .text()
            .map_err(|e| format!("read {method} {url} body: {e}"))?;

        Ok(HttpTextResponse {
            status: status.as_u16(),
            ok: status.is_success(),
            body,
        })
    })
    .await
    .map_err(|e| format!("join blocking task: {e}"))?;

    match &result {
        Ok(response) => eprintln!(
            "[http_request_text] {} {} -> {} in {} ms",
            log_method,
            log_url,
            response.status,
            started.elapsed().as_millis()
        ),
        Err(e) => eprintln!(
            "[http_request_text] {} {} !! {} ms: {e}",
            log_method,
            log_url,
            started.elapsed().as_millis()
        ),
    }

    result
}

// ── Codex pet importer (M13.1) ──

#[tauri::command]
async fn check_ffmpeg_available() -> Result<bool, String> {
    tauri::async_runtime::spawn_blocking(|| Ok(codex_import::check_ffmpeg_available()))
        .await
        .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn list_codex_pets() -> Result<Vec<codex_import::CodexPetSummary>, String> {
    tauri::async_runtime::spawn_blocking(codex_import::list_codex_pets)
        .await
        .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn import_codex_pet(
    app_handle: tauri::AppHandle,
    pet_id: String,
) -> Result<codex_import::CodexImportResult, String> {
    let app_local_data_dir = app_handle
        .path()
        .app_local_data_dir()
        .map_err(|e| format!("无法解析 AppLocalData 目录: {}", e))?;
    tauri::async_runtime::spawn_blocking(move || {
        let imported = codex_import::import_codex_pet(&pet_id, &app_local_data_dir)?;
        usb_serial::prepare_p4_appearance(
            Path::new(&imported.appearance_dir),
            &app_local_data_dir,
        )?;
        Ok(imported)
    })
    .await
    .map_err(|e| e.to_string())?
}

// --- USB serial Tauri commands ---

#[tauri::command]
async fn usb_scan_devices(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> Result<Vec<usb_serial::UsbDeviceInfo>, String> {
    Ok(usb_manager.scan_devices())
}

#[tauri::command]
async fn usb_connect(
    app_handle: tauri::AppHandle,
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    port_name: String,
) -> Result<usb_serial::UsbConnectionStatus, String> {
    let emitter = app_handle.clone();
    usb_manager.connect(&port_name, move |topic, payload| {
        handle_incoming_usb_message(&emitter, topic, payload);
    })?;
    Ok(usb_manager.status())
}

#[tauri::command]
async fn usb_disconnect(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> Result<(), String> {
    usb_manager.disconnect();
    Ok(())
}

#[tauri::command]
async fn usb_send_state(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    source: String,
    payload: serde_json::Value,
) -> Result<(), String> {
    usb_manager.send_state(&source, &payload)
}

#[tauri::command]
async fn usb_send_speech(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    text: String,
) -> Result<(), String> {
    // Snapshot the latest known active state before injecting transient test speech.
    let board_device_id = usb_manager.status().board_device_id;
    if let Some((source, payload)) = load_last_usb_active_state()
        .or_else(|| pick_best_usb_bridge_state(false, &board_device_id))
        .or_else(|| pick_best_usb_bridge_state(true, &board_device_id))
    {
        cache_last_usb_active_state(&source, &payload);
    }

    usb_manager.send_speech(&text)?;

    // Test speech is transient feedback; replay latest active state multiple times
    // so the pet reliably returns to the previous lifecycle even on transient USB jitter.
    let manager = usb_manager.inner().clone();
    thread::spawn(move || {
        let mut elapsed_ms = 0u64;

        let sleep_to = |elapsed_ms: &mut u64, target_ms: u64| {
            if target_ms > *elapsed_ms {
                thread::sleep(Duration::from_millis(target_ms - *elapsed_ms));
                *elapsed_ms = target_ms;
            }
        };

        for target_ms in [1400u64, 3200u64, 5400u64] {
            sleep_to(&mut elapsed_ms, target_ms);
            if let Err(error) = replay_usb_active_state(&manager) {
                eprintln!(
                    "[usb-forwarder] replay active after speech failed(at {} ms): {}",
                    target_ms, error
                );
            }
        }
    });

    Ok(())
}

#[tauri::command]
async fn usb_send_command(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    command: String,
) -> Result<(), String> {
    usb_manager.send_command(&command)
}

#[tauri::command]
#[allow(non_snake_case)]
async fn usb_audio_capture_control(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    boardDeviceId: String,
    action: String,
) -> Result<(), String> {
    let action = action.trim().to_ascii_lowercase();
    if action != "start" && action != "stop" {
        return Err(format!(
            "invalid audio capture action '{action}', expected start|stop"
        ));
    }
    usb_manager.send_to_board(
        &boardDeviceId,
        "audio/control",
        &serde_json::json!({ "action": action }),
    )
}

#[tauri::command]
async fn usb_get_status(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> Result<usb_serial::UsbConnectionStatus, String> {
    Ok(usb_manager.status())
}

/// Remote-set the device's screen page (main | app). Forwards via USB serial
/// as topic "control/screen-page" with {"page": "<value>"}. Used as both a
/// recovery hatch (when widget OTA leaves the device stuck on app) and a
/// diagnostic tool (success implies file-write + renderer alive; failure
/// implies the renderer layer is stuck).
#[tauri::command]
async fn usb_set_screen_page(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    page: String,
) -> Result<(), String> {
    let payload = serde_json::json!({ "page": page });
    usb_manager.send("control/screen-page", &payload)
}

#[tauri::command]
async fn usb_apply_wifi(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    ssid: String,
    psk: String,
) -> Result<(), String> {
    let payload = serde_json::json!({ "ssid": ssid, "password": psk });
    usb_manager.send("control/apply-wifi", &payload)
}

#[tauri::command]
async fn usb_get_diagnostics(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    expected_board_device_id: String,
) -> Result<serde_json::Value, String> {
    let manager = usb_manager.inner().clone();
    let mut report = tauri::async_runtime::spawn_blocking(move || {
        manager.query_diagnostics(&expected_board_device_id)
    })
    .await
    .map_err(|error| error.to_string())??;
    if let Some(object) = report.as_object_mut() {
        object.insert("desktopBuild".to_string(), desktop_build_info());
    }
    Ok(report)
}

#[tauri::command]
async fn usb_get_button_config(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    expected_board_device_id: String,
) -> Result<serde_json::Value, String> {
    let manager = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        manager.query_button_config(&expected_board_device_id)
    })
    .await
    .map_err(|error| error.to_string())?
}

#[tauri::command]
async fn usb_reset_input_config(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    expected_board_device_id: String,
) -> Result<serde_json::Value, String> {
    let manager = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        manager.reset_input_config(&expected_board_device_id)
    })
    .await
    .map_err(|error| error.to_string())?
}

#[tauri::command]
async fn usb_reboot_device(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    expected_board_device_id: String,
) -> Result<serde_json::Value, String> {
    let manager = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || manager.reboot_device(&expected_board_device_id))
        .await
        .map_err(|error| error.to_string())?
}

fn parse_firmware_version(value: &str) -> Option<Vec<u64>> {
    let normalized = value.trim().trim_start_matches(['v', 'V']);
    let core = normalized
        .split_once('-')
        .map_or(normalized, |(core, _)| core);
    let parts = core
        .split('.')
        .map(str::parse::<u64>)
        .collect::<Result<Vec<_>, _>>()
        .ok()?;
    if parts.is_empty() || parts.len() > 4 {
        return None;
    }
    Some(parts)
}

fn compare_firmware_versions(left: &str, right: &str) -> Result<VersionOrdering, String> {
    let mut left = parse_firmware_version(left)
        .ok_or_else(|| format!("invalid device firmware version: {left}"))?;
    let mut right = parse_firmware_version(right)
        .ok_or_else(|| format!("invalid bundled firmware version: {right}"))?;
    let width = left.len().max(right.len());
    left.resize(width, 0);
    right.resize(width, 0);
    Ok(left.cmp(&right))
}

fn bundled_p4_firmware_path(app_handle: &tauri::AppHandle) -> Result<PathBuf, String> {
    let mut candidates = Vec::new();
    if let Ok(resource_dir) = app_handle.path().resource_dir() {
        candidates.push(resource_dir.join(BUNDLED_P4_FIRMWARE_RESOURCE));
    }
    candidates.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(BUNDLED_P4_FIRMWARE_RESOURCE));
    candidates
        .into_iter()
        .find(|path| path.is_file())
        .ok_or_else(|| {
            format!("bundled P4 firmware resource was not found: {BUNDLED_P4_FIRMWARE_RESOURCE}")
        })
}

#[tauri::command]
fn usb_get_bundled_firmware_info(
    app_handle: tauri::AppHandle,
) -> Result<usb_serial::FirmwareImageInfo, String> {
    let path = bundled_p4_firmware_path(&app_handle)?;
    usb_serial::inspect_firmware_image(&path)
}

async fn run_usb_firmware_update(
    app_handle: tauri::AppHandle,
    manager: usb_serial::UsbSerialManager,
    firmware_path: PathBuf,
    expected_board_device_id: String,
) -> Result<usb_serial::FirmwareUpdateResult, String> {
    let emitter = app_handle.clone();
    let reconnect_manager = manager.clone();
    let reconnect_app = app_handle.clone();
    let reconnect_board_device_id = expected_board_device_id.clone();
    tauri::async_runtime::spawn_blocking(move || {
        manager.update_firmware(
            &firmware_path,
            &expected_board_device_id,
            |bytes_sent, bytes_total, stage| {
                let percent = if bytes_total == 0 {
                    0
                } else {
                    ((bytes_sent.saturating_mul(100)) / bytes_total).min(100)
                };
                let _ = emitter.emit(
                    "usb-firmware-update-progress",
                    serde_json::json!({
                        "stage": stage,
                        "bytesSent": bytes_sent,
                        "bytesTotal": bytes_total,
                        "percent": percent,
                    }),
                );
            },
            || {
                reconnect_usb_serial_to_expected_board(
                    &reconnect_app,
                    &reconnect_manager,
                    &reconnect_board_device_id,
                )
            },
        )
    })
    .await
    .map_err(|error| error.to_string())?
}

#[tauri::command]
async fn usb_update_firmware(
    app_handle: tauri::AppHandle,
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    firmware_path: String,
    expected_board_device_id: String,
) -> Result<usb_serial::FirmwareUpdateResult, String> {
    let path = PathBuf::from(firmware_path.trim());
    if !path.is_file() {
        return Err(format!("firmware image does not exist: {}", path.display()));
    }
    if !path
        .extension()
        .and_then(|value| value.to_str())
        .is_some_and(|value| value.eq_ignore_ascii_case("bin"))
    {
        return Err("firmware image must use the .bin extension".to_string());
    }

    run_usb_firmware_update(
        app_handle,
        usb_manager.inner().clone(),
        path,
        expected_board_device_id,
    )
    .await
}

#[tauri::command]
async fn usb_update_bundled_firmware(
    app_handle: tauri::AppHandle,
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    expected_board_device_id: String,
) -> Result<usb_serial::FirmwareUpdateResult, String> {
    let firmware_path = bundled_p4_firmware_path(&app_handle)?;
    let firmware_info = usb_serial::inspect_firmware_image(&firmware_path)?;
    let manager = usb_manager.inner().clone();
    let status = manager.status();
    if !status.connected || status.board_device_id != expected_board_device_id {
        return Err("connected USB board changed before bundled firmware update".to_string());
    }
    if !status.runtime.eq_ignore_ascii_case("esp-p4") {
        return Err("bundled firmware update is only supported by ESP32-P4".to_string());
    }
    if compare_firmware_versions(&status.firmware, &firmware_info.version)? != VersionOrdering::Less
    {
        return Err(format!(
            "device firmware {} is already newer than or equal to bundled firmware {}",
            status.firmware, firmware_info.version
        ));
    }
    run_usb_firmware_update(app_handle, manager, firmware_path, expected_board_device_id).await
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct UsbSyncAppearanceResult {
    ok: bool,
    file_count: u32,
    byte_count: u64,
    reused_slot: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct UsbCancelAppearanceSyncResult {
    requested: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum UsbAppearanceSyncRuntime {
    Linux,
    EspP4,
}

fn usb_status_supports_p4_assets(status: &usb_serial::UsbConnectionStatus) -> bool {
    status.runtime.eq_ignore_ascii_case("esp-p4")
        || status
            .capabilities
            .get("assetFormats")
            .and_then(|value| value.as_array())
            .is_some_and(|formats| {
                formats.iter().any(|format| {
                    matches!(
                        format.as_str(),
                        Some("p4-h264-v1") | Some("p4-mjpeg-v1") | Some("p4-frames-v1")
                    )
                })
            })
}

fn usb_appearance_sync_runtime(
    status: &usb_serial::UsbConnectionStatus,
) -> Result<UsbAppearanceSyncRuntime, String> {
    if !status.connected {
        return Err("USB 未连接，请先通过 USB 连接设备".to_string());
    }
    if usb_status_supports_p4_assets(status) {
        Ok(UsbAppearanceSyncRuntime::EspP4)
    } else {
        Ok(UsbAppearanceSyncRuntime::Linux)
    }
}

fn validate_appearance_id(appearance_id: &str) -> Result<&str, String> {
    let appearance_id = appearance_id.trim();
    if appearance_id.is_empty()
        || appearance_id == "."
        || appearance_id == ".."
        || appearance_id.contains('/')
        || appearance_id.contains('\\')
    {
        return Err("invalid appearance id".to_string());
    }
    Ok(appearance_id)
}

fn ensure_builtin_terrier_source(
    app_local_data_dir: &Path,
    clips_dir: &Path,
) -> Result<PathBuf, String> {
    if !clips_dir.is_dir() {
        return Err(format!(
            "built-in appearance resources were not found: {}",
            clips_dir.display()
        ));
    }
    let appearance_dir = app_local_data_dir
        .join("custom-appearances")
        .join("builtin-terrier");
    let videos_dir = appearance_dir.join("videos");
    fs::create_dir_all(&videos_dir)
        .map_err(|error| format!("create built-in appearance directory failed: {error}"))?;
    let audio_overrides: HashMap<String, String> =
        fs::read_to_string(appearance_dir.join("audio-overrides.json"))
            .ok()
            .and_then(|raw| serde_json::from_str(&raw).ok())
            .unwrap_or_default();
    let mut entries = fs::read_dir(clips_dir)
        .map_err(|error| format!("read built-in appearance resources failed: {error}"))?
        .filter_map(Result::ok)
        .filter(|entry| entry.path().extension().and_then(|value| value.to_str()) == Some("mp4"))
        .collect::<Vec<_>>();
    entries.sort_by_key(|entry| entry.file_name());

    let mut families = Vec::new();
    for entry in entries {
        let source = entry.path();
        let name = entry.file_name().to_string_lossy().to_string();
        let family = source
            .file_stem()
            .and_then(|value| value.to_str())
            .unwrap_or_default()
            .to_string();
        if family.is_empty() {
            continue;
        }
        let video_dest = videos_dir.join(&name);
        if !video_dest.is_file()
            || fs::metadata(&video_dest)
                .map(|meta| meta.len())
                .unwrap_or(0)
                != fs::metadata(&source).map(|meta| meta.len()).unwrap_or(1)
        {
            fs::copy(&source, &video_dest)
                .map_err(|error| format!("copy built-in video {name} failed: {error}"))?;
        }
        let mut family_entry = serde_json::json!({
            "family": family.clone(),
            "ok": true,
            "videoPath": format!("custom-appearances/builtin-terrier/videos/{name}"),
        });
        let audio_override = audio_overrides
            .get(&family)
            .map(|relative| app_local_data_dir.join(relative))
            .filter(|path| path.is_file());
        let audio_source = audio_override.or_else(|| {
            default_appearance_audio_cue_name(&family)
                .map(|cue_name| clips_dir.join(cue_name))
                .filter(|path| path.is_file())
        });
        if let Some(audio_source) = audio_source {
            let audio_name = format!("{family}.wav");
            let audio_dest = videos_dir.join(&audio_name);
            fs::copy(&audio_source, &audio_dest)
                .map_err(|error| format!("copy built-in audio {audio_name} failed: {error}"))?;
            family_entry["audioPath"] = serde_json::json!(format!(
                "custom-appearances/builtin-terrier/videos/{audio_name}"
            ));
        }
        families.push(family_entry);
    }
    if families.is_empty() {
        return Err("built-in appearance contains no MP4 videos".to_string());
    }
    fs::write(
        appearance_dir.join("manifest.json"),
        serde_json::to_vec_pretty(&serde_json::json!({
            "schema_version": 1,
            "id": "builtin-terrier",
            "type": "builtin",
            "families": families,
        }))
        .map_err(|error| error.to_string())?,
    )
    .map_err(|error| format!("write built-in appearance manifest failed: {error}"))?;
    Ok(appearance_dir)
}

fn prepare_p4_appearance_by_id(
    app_handle: &tauri::AppHandle,
    appearance_id: &str,
) -> Result<usb_serial::PreparedP4Appearance, String> {
    let appearance_id = validate_appearance_id(appearance_id)?;
    let app_local_data_dir = app_handle
        .path()
        .app_local_data_dir()
        .map_err(|error| format!("resolve AppLocalData failed: {error}"))?;
    let resource_dir = app_handle
        .path()
        .resource_dir()
        .map_err(|error| format!("resolve resource directory failed: {error}"))?;
    let clips_dir = resource_dir.join("terrier-clips");
    let appearance_dir = if appearance_id == "builtin-terrier" {
        ensure_builtin_terrier_source(&app_local_data_dir, &clips_dir)?
    } else {
        let dir = app_local_data_dir
            .join("custom-appearances")
            .join(appearance_id);
        ensure_default_appearance_audio_cues(&dir, &clips_dir)?;
        dir
    };
    usb_serial::prepare_p4_appearance(&appearance_dir, &app_local_data_dir)
}

#[tauri::command]
async fn prepare_p4_appearance(
    app_handle: tauri::AppHandle,
    appearance_id: String,
) -> Result<usb_serial::PreparedP4Appearance, String> {
    tauri::async_runtime::spawn_blocking(move || {
        prepare_p4_appearance_by_id(&app_handle, &appearance_id)
    })
    .await
    .map_err(|error| error.to_string())?
}

fn start_p4_ready_migration(app_handle: tauri::AppHandle) {
    thread::spawn(move || {
        let Ok(app_local_data_dir) = app_handle.path().app_local_data_dir() else {
            return;
        };
        let Ok(resource_dir) = app_handle.path().resource_dir() else {
            return;
        };
        let clips_dir = resource_dir.join("terrier-clips");
        let appearances_root = app_local_data_dir.join("custom-appearances");
        let Ok(entries) = fs::read_dir(&appearances_root) else {
            return;
        };
        for entry in entries.filter_map(Result::ok) {
            let appearance_dir = entry.path();
            if !appearance_dir.is_dir()
                || entry.file_name().to_string_lossy() == "builtin-terrier"
                || !appearance_dir.join("manifest.json").is_file()
                || usb_serial::inspect_prepared_p4_appearance(&appearance_dir).is_ok()
            {
                continue;
            }
            if let Err(error) = ensure_default_appearance_audio_cues(&appearance_dir, &clips_dir)
                .and_then(|_| {
                    usb_serial::prepare_p4_appearance(&appearance_dir, &app_local_data_dir)
                        .map(|_| ())
                })
            {
                eprintln!(
                    "[p4-ready] background migration failed source={}: {}",
                    appearance_dir.display(),
                    error
                );
            }
        }
    });
}

#[tauri::command]
fn usb_cancel_appearance_sync(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> UsbCancelAppearanceSyncResult {
    UsbCancelAppearanceSyncResult {
        requested: usb_manager.cancel_appearance_sync(),
    }
}

fn resolve_appearance_sync_board_device_id(
    serial_connected: bool,
    connected_board_device_id: &str,
    requested_board_device_id: &str,
) -> Result<String, String> {
    let connected_board_device_id = connected_board_device_id.trim();
    let requested_board_device_id = requested_board_device_id.trim();
    if serial_connected {
        if connected_board_device_id.is_empty() {
            return Err("当前 USB 连接没有可校验的 boardDeviceId，已拒绝下发".to_string());
        }
        if !requested_board_device_id.is_empty()
            && requested_board_device_id != connected_board_device_id
        {
            return Err(format!(
                "目标设备已变化：请求 {requested_board_device_id}，当前连接 {connected_board_device_id}。已拒绝下发"
            ));
        }
        return Ok(connected_board_device_id.to_string());
    }
    if requested_board_device_id.is_empty() {
        return Err(
            "原生 USB 模式需要明确的 boardDeviceId，无法从断开的串口连接推断目标设备".to_string(),
        );
    }
    Ok(requested_board_device_id.to_string())
}

#[tauri::command]
async fn usb_sync_appearance(
    app_handle: tauri::AppHandle,
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    appearance_id: String,
    board_device_id: Option<String>,
) -> Result<UsbSyncAppearanceResult, String> {
    let status = usb_manager.status();
    let native_usb_available = usb_serial::p4_native_usb_available();
    if !status.connected && !native_usb_available {
        return Err("USB 未连接，请先通过 USB 连接设备".to_string());
    }
    let requested_board_device_id = board_device_id.unwrap_or_default().trim().to_string();
    let expected_board_device_id = resolve_appearance_sync_board_device_id(
        status.connected,
        &status.board_device_id,
        &requested_board_device_id,
    )?;

    let app_local_data_dir = app_handle
        .path()
        .app_local_data_dir()
        .map_err(|e| format!("无法解析 AppLocalData 目录: {}", e))?;

    let serial_connected = status.connected;
    let sync_runtime = if serial_connected {
        usb_appearance_sync_runtime(&status)?
    } else {
        UsbAppearanceSyncRuntime::EspP4
    };

    let appearance_id = validate_appearance_id(&appearance_id)?.to_string();
    let resource_dir = app_handle
        .path()
        .resource_dir()
        .map_err(|error| format!("无法解析 resource 目录: {error}"))?;
    let clips_dir = resource_dir.join("terrier-clips");
    let appearance_dir = if appearance_id == "builtin-terrier" {
        let local_dir = app_local_data_dir
            .join("custom-appearances")
            .join("builtin-terrier");
        match sync_runtime {
            UsbAppearanceSyncRuntime::EspP4
                if usb_serial::inspect_prepared_p4_appearance(&local_dir).is_ok() =>
            {
                local_dir
            }
            UsbAppearanceSyncRuntime::EspP4 => clips_dir,
            UsbAppearanceSyncRuntime::Linux => {
                ensure_builtin_terrier_source(&app_local_data_dir, &clips_dir)?
            }
        }
    } else {
        let dir = app_local_data_dir
            .join("custom-appearances")
            .join(&appearance_id);
        ensure_default_appearance_audio_cues(&dir, &clips_dir)?;
        dir
    };

    match sync_runtime {
        UsbAppearanceSyncRuntime::EspP4 => {
            usb_serial::inspect_prepared_p4_appearance(&appearance_dir)?;
        }
        UsbAppearanceSyncRuntime::Linux if !appearance_dir.join("manifest.json").is_file() => {
            return Err(format!("未找到形象素材: {}", appearance_dir.display()));
        }
        UsbAppearanceSyncRuntime::Linux => {}
    }

    usb_manager.begin_appearance_sync()?;
    let mgr = usb_manager.inner().clone();
    let worker_mgr = mgr.clone();
    let dir = appearance_dir.clone();
    let data_dir = app_local_data_dir.clone();
    let emitter = app_handle.clone();
    let worker_result = tauri::async_runtime::spawn_blocking(move || {
        let sync_result = match sync_runtime {
            UsbAppearanceSyncRuntime::EspP4 => {
                if serial_connected {
                    worker_mgr.sync_appearance_p4(
                        &dir,
                        &data_dir,
                        &expected_board_device_id,
                        |current, total, bytes_sent, bytes_total| {
                            let _ = emitter.emit(
                                "usb-sync-progress",
                                serde_json::json!({
                                    "currentFile": current,
                                    "totalFiles": total,
                                    "bytesSent": bytes_sent,
                                    "bytesTotal": bytes_total,
                                }),
                            );
                        },
                    )
                } else {
                    eprintln!(
                        "[usb-p4-native-ota] Native USB mode detected without serial COM port"
                    );
                    worker_mgr.sync_appearance_p4_native_only(
                        &dir,
                        &data_dir,
                        &expected_board_device_id,
                        |current, total, bytes_sent, bytes_total| {
                            let _ = emitter.emit(
                                "usb-sync-progress",
                                serde_json::json!({
                                    "currentFile": current,
                                    "totalFiles": total,
                                    "bytesSent": bytes_sent,
                                    "bytesTotal": bytes_total,
                                }),
                            );
                        },
                    )
                }
            }
            UsbAppearanceSyncRuntime::Linux => worker_mgr
                .sync_appearance(
                    &dir,
                    &data_dir,
                    |current, total, bytes_sent, bytes_total| {
                        let _ = emitter.emit(
                            "usb-sync-progress",
                            serde_json::json!({
                                "currentFile": current,
                                "totalFiles": total,
                                "bytesSent": bytes_sent,
                                "bytesTotal": bytes_total,
                            }),
                        );
                    },
                )
                .map(|(file_count, byte_count)| (file_count, byte_count, false)),
        };
        match sync_result {
            Ok((file_count, byte_count, reused_slot)) => Ok(UsbSyncAppearanceResult {
                ok: true,
                file_count,
                byte_count,
                reused_slot,
                error: None,
            }),
            Err(e) => Ok(UsbSyncAppearanceResult {
                ok: false,
                file_count: 0,
                byte_count: 0,
                reused_slot: false,
                error: Some(e),
            }),
        }
    })
    .await;
    mgr.finish_appearance_sync();
    worker_result.map_err(|e| e.to_string())?
}

// ---------------------------------------------------------------------------
// install_widget_skill — copy bundled skill to every detected coding-agent CLI
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct SkillInstallEntry {
    agent: String,
    home_dir: String,
    target_path: String,
    file_count: u32,
    overwrote: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct SkillSkipEntry {
    agent: String,
    home_dir: String,
    reason: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct InstallWidgetSkillResult {
    ok: bool,
    installed: Vec<SkillInstallEntry>,
    skipped: Vec<SkillSkipEntry>,
    skill_source_path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

struct SkillTarget {
    agent: &'static str,
    home_dir: &'static str,
}

const SKILL_TARGETS: &[SkillTarget] = &[
    SkillTarget {
        agent: "ChatGPT（Codex）",
        home_dir: ".codex",
    },
    SkillTarget {
        agent: "Claude",
        home_dir: ".claude",
    },
    SkillTarget {
        agent: "OpenClaw",
        home_dir: ".openclaw",
    },
    SkillTarget {
        agent: "MiMoCode / Agent Skills",
        home_dir: ".agents",
    },
    SkillTarget {
        agent: "Gemini CLI",
        home_dir: ".gemini",
    },
    SkillTarget {
        agent: "Cursor",
        home_dir: ".cursor",
    },
];

const SKILL_NAME: &str = "petui";
const LEGACY_SKILL_NAMES: &[&str] = &[
    "petAgent-ui-generator",
    "petagent-ui-generator",
    "petui-agent",
];

/// Locate the on-disk `skills/petui` directory the
/// install_widget_skill command copies into each coding agent's home.
///
/// Paths, in order:
///   1. **Production bundle** — looks under `app.path().resource_dir()`
///      where tauri.conf.json's `bundle.resources` placed
///      `skills/petui` at build time.
///   2. **Debug fallback** — `CARGO_MANIFEST_DIR/../../skills/petui`,
///      i.e. resolved relative to the source tree. Only meaningful when
///      running `npm run dev` / `cargo run` from the repo.
///
/// If neither exists, surface a single error listing both attempted paths.
fn resolve_skill_source_dir(app: &tauri::AppHandle) -> Result<std::path::PathBuf, String> {
    let mut tried: Vec<String> = Vec::new();

    // 1. Bundled resource (production)
    if let Ok(res_dir) = app.path().resource_dir() {
        let bundled = res_dir.join("skills").join(SKILL_NAME);
        if bundled.exists() {
            return Ok(bundled);
        }
        tried.push(format!("bundle: {}", bundled.display()));
    }

    // 2. Debug fallback — source-tree path
    #[cfg(debug_assertions)]
    {
        let manifest_dir = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        if let Some(dev_src) = manifest_dir
            .parent() // ref/
            .and_then(|p| p.parent()) // claw-pet-manager/
            .map(|p| p.join("skills").join(SKILL_NAME))
        {
            if dev_src.exists() {
                return Ok(dev_src);
            }
            tried.push(format!("debug:  {}", dev_src.display()));
        }
    }

    Err(format!(
        "skill 源目录不存在；尝试过:\n  {}\n(production: tauri.conf.json bundle.resources 必须包含 skills/petui; debug: 确认从 claw-pet-manager 根目录运行)",
        tried.join("\n  ")
    ))
}

fn copy_dir_recursive(src: &std::path::Path, dst: &std::path::Path) -> std::io::Result<u32> {
    std::fs::create_dir_all(dst)?;
    let mut count: u32 = 0;
    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let file_type = entry.file_type()?;
        let dst_path = dst.join(entry.file_name());
        if file_type.is_dir() {
            count += copy_dir_recursive(&entry.path(), &dst_path)?;
        } else if file_type.is_file() {
            std::fs::copy(entry.path(), dst_path)?;
            count += 1;
        }
    }
    Ok(count)
}

fn install_skill_into_agent(
    src: &std::path::Path,
    agent_home: &std::path::Path,
    agent_label: &str,
) -> Result<SkillInstallEntry, String> {
    let skills_root = agent_home.join("skills");
    std::fs::create_dir_all(&skills_root).map_err(|e| format!("create skills root: {}", e))?;
    let dst = skills_root.join(SKILL_NAME);
    let overwrote = dst.exists()
        || LEGACY_SKILL_NAMES
            .iter()
            .any(|legacy_name| skills_root.join(legacy_name).exists());
    let operation_id = uuid::Uuid::new_v4();
    let staging = skills_root.join(format!(".{SKILL_NAME}.install-{operation_id}"));
    let backup = skills_root.join(format!(".{SKILL_NAME}.backup-{operation_id}"));
    let file_count = match copy_dir_recursive(src, &staging) {
        Ok(count) => count,
        Err(error) => {
            let _ = std::fs::remove_dir_all(&staging);
            return Err(format!("拷贝到临时目录失败: {error}"));
        }
    };
    if dst.exists() {
        std::fs::rename(&dst, &backup).map_err(|error| {
            let _ = std::fs::remove_dir_all(&staging);
            format!("备份旧 skill 目录失败: {error}")
        })?;
    }
    if let Err(error) = std::fs::rename(&staging, &dst) {
        let restore_error = if backup.exists() {
            std::fs::rename(&backup, &dst).err()
        } else {
            None
        };
        let _ = std::fs::remove_dir_all(&staging);
        return Err(match restore_error {
            Some(restore_error) => {
                format!("切换新版 skill 失败: {error}；恢复旧版也失败: {restore_error}")
            }
            None => format!("切换新版 skill 失败，已保留旧版: {error}"),
        });
    }
    if backup.exists() {
        std::fs::remove_dir_all(&backup)
            .map_err(|error| format!("新版已安装，但清理旧 skill 备份失败: {error}"))?;
    }
    for legacy_name in LEGACY_SKILL_NAMES {
        let legacy = skills_root.join(legacy_name);
        if legacy.exists() {
            std::fs::remove_dir_all(&legacy)
                .map_err(|e| format!("清理旧 skill 目录 {} 失败: {}", legacy.display(), e))?;
        }
    }
    Ok(SkillInstallEntry {
        agent: agent_label.to_string(),
        home_dir: agent_home
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("?")
            .to_string(),
        target_path: dst.display().to_string(),
        file_count,
        overwrote,
    })
}

#[tauri::command]
async fn install_widget_skill(
    app_handle: tauri::AppHandle,
) -> Result<InstallWidgetSkillResult, String> {
    let src = resolve_skill_source_dir(&app_handle)?;
    let home = get_home_dir().map_err(|e| e.to_string())?;

    let mut installed: Vec<SkillInstallEntry> = Vec::new();
    let mut skipped: Vec<SkillSkipEntry> = Vec::new();

    for target in SKILL_TARGETS {
        let home_subdir = home.join(target.home_dir);
        if !home_subdir.exists() {
            skipped.push(SkillSkipEntry {
                agent: target.agent.to_string(),
                home_dir: target.home_dir.to_string(),
                reason: "未检测到该 agent (config dir 不存在)".to_string(),
            });
            continue;
        }
        match install_skill_into_agent(&src, &home_subdir, target.agent) {
            Ok(entry) => installed.push(entry),
            Err(e) => skipped.push(SkillSkipEntry {
                agent: target.agent.to_string(),
                home_dir: target.home_dir.to_string(),
                reason: format!("拷贝失败: {}", e),
            }),
        }
    }

    // Fallback: nothing detected -> force-install to Claude's standard home.
    if installed.is_empty() {
        let fallback_home = home.join(".claude");
        match install_skill_into_agent(&src, &fallback_home, "Claude (fallback)") {
            Ok(entry) => installed.push(entry),
            Err(e) => return Err(format!("fallback 安装也失败: {}", e)),
        }
    }

    Ok(InstallWidgetSkillResult {
        ok: !installed.is_empty(),
        installed,
        skipped,
        skill_source_path: src.display().to_string(),
        error: None,
    })
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct InstallClawpkgInput {
    clawpkg_path: String,
    /// Optional footer slot override — generated client-side from the user's
    /// (possibly customized) button bindings so config changes ride along with
    /// the install payload. Only used for legacy zip-based static dashboards.
    #[serde(default)]
    footer_override: Option<String>,
    /// Action-id → new-control map from ComponentCenter's "按钮功能" UI.
    /// Used by the new widget OTA path (directory clawpkg) to rewrite
    /// buttons.json before pushing. Empty for legacy zip flow.
    #[serde(default)]
    binding_overrides: std::collections::HashMap<String, String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct InstallClawpkgResult {
    ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    manifest: Option<crate::clawpkg::ClawpkgManifestPreview>,
    errors: Vec<String>,
    transferred_bytes: usize,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct RemoveWidgetInput {
    component_id: String,
    transport: String,
    #[serde(default)]
    board_device_id: String,
    #[serde(default)]
    ssh_host: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct RemoveWidgetResult {
    ok: bool,
    component_id: String,
    transport: String,
    input_config_reset: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    warning: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ListDeviceWidgetsInput {
    transport: String,
    #[serde(default)]
    board_device_id: String,
    #[serde(default)]
    ssh_host: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct DeviceWidgetInventoryItem {
    id: String,
    name: Option<String>,
    kind: Option<String>,
    version: Option<String>,
    active: bool,
    manifest_state: String,
    removable: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct DeviceWidgetInventory {
    ok: bool,
    freshness: String,
    runtime: String,
    transport: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    board_device_id: Option<String>,
    queried_at_ms: u64,
    active_widget_id: Option<String>,
    supports_multiple: bool,
    max_installed: Option<u32>,
    items: Vec<DeviceWidgetInventoryItem>,
    warnings: Vec<String>,
}

const DEVICE_WIDGET_INVENTORY_MAX_ITEMS: usize = 128;
const SSH_WIDGET_INVENTORY_SCRIPT: &str = r#"
import json
import os
import re
import stat
import sys

root = sys.argv[1]
safe_id = re.compile(r"^[a-z][a-z0-9_-]{0,46}$")
warnings = []
active = None
board_id = None

try:
    with open(os.path.join(root, ".active-widget"), "r", encoding="utf-8") as handle:
        candidate = handle.read(128).strip()
    if candidate:
        if safe_id.fullmatch(candidate):
            active = candidate
        else:
            warnings.append("invalid-active-widget-marker")
except FileNotFoundError:
    pass
except (OSError, UnicodeError):
    warnings.append("unreadable-active-widget-marker")

try:
    with open(os.path.join(root, "device-config.json"), "r", encoding="utf-8") as handle:
        config = json.load(handle)
    if isinstance(config, dict):
        candidate = config.get("boardDeviceId")
        if isinstance(candidate, str) and candidate and len(candidate) <= 128:
            board_id = candidate
except (FileNotFoundError, OSError, UnicodeError, json.JSONDecodeError):
    pass

entries = []
widgets_root = os.path.join(root, "widgets")
try:
    with os.scandir(widgets_root) as scan:
        entries = sorted(
            (
                entry for entry in scan
                if safe_id.fullmatch(entry.name)
                and entry.is_dir(follow_symlinks=False)
            ),
            key=lambda entry: entry.name,
        )
except FileNotFoundError:
    pass

if len(entries) > 128:
    entries = entries[:128]
    warnings.append("inventory-limit-reached")

items = []
for entry in entries:
    item = {
        "id": entry.name,
        "name": entry.name,
        "kind": None,
        "version": None,
        "active": entry.name == active,
        "manifestState": "missing",
        "removable": True,
    }
    manifest_path = os.path.join(entry.path, "component.json")
    try:
        metadata = os.lstat(manifest_path)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > 4095:
            item["manifestState"] = "invalid"
        else:
            with open(manifest_path, "r", encoding="utf-8") as handle:
                manifest = json.load(handle)
            manifest_id = manifest.get("id") if isinstance(manifest, dict) else None
            name = manifest.get("name") if isinstance(manifest, dict) else None
            version = manifest.get("version") if isinstance(manifest, dict) else None
            kind = manifest.get("kind") if isinstance(manifest, dict) else None
            if manifest_id != entry.name:
                item["manifestState"] = "id-mismatch"
            elif (
                not isinstance(name, str)
                or not name
                or not isinstance(version, str)
                or not version
                or (kind is not None and kind not in ("game", "tool"))
            ):
                item["manifestState"] = "invalid"
            else:
                item["name"] = name
                item["version"] = version
                item["kind"] = kind
                item["manifestState"] = "valid"
    except FileNotFoundError:
        pass
    except (OSError, UnicodeError, json.JSONDecodeError):
        item["manifestState"] = "invalid"
    items.append(item)

if active is not None and not any(item["id"] == active for item in items):
    warnings.append("active-package-missing")

print(json.dumps({
    "ok": True,
    "boardDeviceId": board_id,
    "activeWidgetId": active,
    "supportsMultiple": True,
    "maxInstalled": None,
    "items": items,
    "warnings": warnings,
}, ensure_ascii=False, separators=(",", ":")))
"#;

fn is_safe_builtin_clawpkg_id(id: &str) -> bool {
    !id.is_empty()
        && id
            .chars()
            .all(|ch| ch.is_ascii_alphanumeric() || ch == '-' || ch == '_')
}

fn is_safe_widget_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() < 48
        && id.as_bytes().first().is_some_and(u8::is_ascii_lowercase)
        && id
            .chars()
            .all(|ch| ch.is_ascii_lowercase() || ch.is_ascii_digit() || ch == '-' || ch == '_')
}

fn normalize_widget_ssh_host(value: &str) -> Result<Option<String>, String> {
    let host = value.trim();
    if host.is_empty() {
        return Ok(None);
    }
    if host.len() > 255
        || host.starts_with('-')
        || !host.chars().all(|ch| {
            ch.is_ascii_alphanumeric()
                || matches!(ch, '@' | '.' | '_' | '-' | ':' | '[' | ']' | '%')
        })
    {
        return Err("SSH 目标格式非法；请使用 user@host 或 user@IP。".to_string());
    }
    Ok(Some(host.to_string()))
}

fn normalized_optional_inventory_text(
    value: Option<&serde_json::Value>,
    max_bytes: usize,
) -> Option<String> {
    value
        .and_then(serde_json::Value::as_str)
        .map(str::trim)
        .filter(|value| !value.is_empty() && value.len() <= max_bytes)
        .filter(|value| !value.chars().any(char::is_control))
        .map(str::to_string)
}

fn normalize_device_widget_inventory(
    raw: &serde_json::Value,
    transport: &str,
    runtime: &str,
    expected_board_device_id: Option<&str>,
) -> Result<DeviceWidgetInventory, String> {
    if raw.get("ok").and_then(serde_json::Value::as_bool) == Some(false) {
        return Err(raw
            .get("error")
            .or_else(|| raw.get("message"))
            .and_then(serde_json::Value::as_str)
            .unwrap_or("设备组件清单查询失败")
            .to_string());
    }
    let mut warnings = raw
        .get("warnings")
        .and_then(serde_json::Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(serde_json::Value::as_str)
        .filter(|warning| !warning.is_empty() && warning.len() <= 160)
        .map(str::to_string)
        .collect::<Vec<_>>();
    if raw.get("complete").and_then(serde_json::Value::as_bool) == Some(false) {
        warnings.push("inventory-incomplete".to_string());
    }

    let active_widget_id = normalized_optional_inventory_text(raw.get("activeWidgetId"), 47)
        .filter(|id| is_safe_widget_id(id))
        .or_else(|| {
            if raw
                .get("activeWidgetId")
                .is_some_and(|value| !value.is_null())
            {
                warnings.push("invalid-active-widget-id".to_string());
            }
            None
        });
    let mut seen = HashSet::new();
    let mut items = Vec::new();
    for item in raw
        .get("items")
        .and_then(serde_json::Value::as_array)
        .into_iter()
        .flatten()
    {
        let Some(id) = normalized_optional_inventory_text(item.get("id"), 47)
            .filter(|id| is_safe_widget_id(id))
        else {
            warnings.push("invalid-inventory-item-id".to_string());
            continue;
        };
        if !seen.insert(id.clone()) {
            warnings.push(format!("duplicate-inventory-item:{id}"));
            continue;
        }
        if items.len() >= DEVICE_WIDGET_INVENTORY_MAX_ITEMS {
            warnings.push("inventory-limit-reached".to_string());
            break;
        }
        let kind = normalized_optional_inventory_text(item.get("kind"), 16)
            .filter(|kind| matches!(kind.as_str(), "game" | "tool"));
        items.push(DeviceWidgetInventoryItem {
            name: normalized_optional_inventory_text(item.get("name"), 160),
            kind,
            version: normalized_optional_inventory_text(item.get("version"), 64),
            active: item
                .get("active")
                .and_then(serde_json::Value::as_bool)
                .unwrap_or_else(|| active_widget_id.as_deref() == Some(id.as_str())),
            manifest_state: normalized_optional_inventory_text(item.get("manifestState"), 32)
                .unwrap_or_else(|| "unknown".to_string()),
            removable: item
                .get("removable")
                .and_then(serde_json::Value::as_bool)
                .unwrap_or(true),
            id,
        });
    }
    items.sort_by(|left, right| left.id.cmp(&right.id));

    let supports_multiple = raw
        .get("supportsMultiple")
        .and_then(serde_json::Value::as_bool)
        .unwrap_or_else(|| !runtime.eq_ignore_ascii_case("esp-p4"));
    let max_installed = raw
        .get("maxInstalled")
        .and_then(serde_json::Value::as_u64)
        .and_then(|value| u32::try_from(value).ok())
        .or((!supports_multiple).then_some(1));
    let raw_board_device_id = normalized_optional_inventory_text(raw.get("boardDeviceId"), 128);
    let expected_board_device_id = expected_board_device_id
        .map(str::trim)
        .filter(|value| !value.is_empty());
    if let (Some(expected), Some(actual)) =
        (expected_board_device_id, raw_board_device_id.as_deref())
    {
        if expected != actual {
            return Err(format!(
                "设备组件清单身份不匹配：期望 {expected}，实际 {actual}"
            ));
        }
    }
    warnings.sort();
    warnings.dedup();

    Ok(DeviceWidgetInventory {
        ok: true,
        freshness: "live".to_string(),
        runtime: runtime.to_string(),
        transport: transport.to_string(),
        board_device_id: expected_board_device_id
            .map(str::to_string)
            .or(raw_board_device_id),
        queried_at_ms: current_timestamp_ms(),
        active_widget_id,
        supports_multiple,
        max_installed,
        items,
        warnings,
    })
}

fn run_widget_inventory_python(mut command: Command) -> Result<serde_json::Value, String> {
    let mut child = command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("启动设备组件清单查询失败: {error}"))?;
    let mut stdin = child
        .stdin
        .take()
        .ok_or_else(|| "设备组件清单查询 stdin 不可用".to_string())?;
    stdin
        .write_all(SSH_WIDGET_INVENTORY_SCRIPT.as_bytes())
        .map_err(|error| format!("发送设备组件清单查询脚本失败: {error}"))?;
    drop(stdin);
    let output = child
        .wait_with_output()
        .map_err(|error| format!("等待设备组件清单查询失败: {error}"))?;
    if !output.status.success() {
        let detail = String::from_utf8_lossy(&output.stderr).trim().to_string();
        return Err(if detail.is_empty() {
            format!("设备组件清单查询退出 {}", output.status)
        } else {
            format!("设备组件清单查询失败: {detail}")
        });
    }
    let stdout = String::from_utf8(output.stdout)
        .map_err(|error| format!("设备组件清单响应不是 UTF-8: {error}"))?;
    let payload = stdout
        .lines()
        .rev()
        .find(|line| !line.trim().is_empty())
        .ok_or_else(|| "设备组件清单响应为空".to_string())?;
    serde_json::from_str(payload).map_err(|error| format!("设备组件清单响应不是有效 JSON: {error}"))
}

fn query_widgets_over_ssh(ssh_bin: &str, ssh_host: &str) -> Result<serde_json::Value, String> {
    let mut command = Command::new(ssh_bin);
    command
        .arg("-o")
        .arg("StrictHostKeyChecking=accept-new")
        .arg("-o")
        .arg("BatchMode=yes")
        .arg(ssh_host)
        .arg(
            "sudo flock -s /opt/board-runtime/.widget-transaction.lock \
             python3 - /opt/board-runtime",
        );
    run_widget_inventory_python(command)
}

fn ssh_widget_delete_inner_script(widget_id: &str) -> String {
    format!(
        "set -eu; root=\"$1\"; active=\"\"; was_active=0; \
         if [ -f \"$root/.active-widget\" ]; then \
           active=$(tr -d \"[:space:]\" < \"$root/.active-widget\"); \
         fi; \
         if [ \"$active\" = \"{id}\" ]; then was_active=1; fi; \
         target=\"$root/widgets/{id}\"; \
         previous=\"$target.previous\"; \
         deleting=\"$root/widgets/.deleting-{id}\"; \
         deleting_previous=\"$deleting.previous\"; \
         rm -rf -- \"$root/.incoming-widget\" \"$deleting\" \"$deleting_previous\"; \
         rm -f -- \"$root/.incoming-widget-transfer\" \"$root/.incoming-widget-id\"; \
         if [ -e \"$target\" ]; then mv \"$target\" \"$deleting\"; fi; \
         if [ -e \"$previous\" ]; then \
           if ! mv \"$previous\" \"$deleting_previous\"; then \
             if [ -e \"$deleting\" ]; then mv \"$deleting\" \"$target\"; fi; \
             exit 43; \
           fi; \
         fi; \
         if [ \"$was_active\" -eq 1 ]; then \
           if ! printf \"\" > \"$root/.active-widget\"; then \
             if [ -e \"$deleting_previous\" ]; then \
               mv \"$deleting_previous\" \"$previous\"; \
             fi; \
             if [ -e \"$deleting\" ]; then mv \"$deleting\" \"$target\"; fi; \
             exit 44; \
           fi; \
           if ! printf main > \"$root/.screen-page\"; then \
             echo \"widget removed; screen switch pending\"; \
           fi; \
         fi; \
         rm -rf -- \"$deleting\" \"$deleting_previous\" \
           || echo \"widget removed; orphan cleanup pending\"; \
         rm -f -- \"$root/.widget-state-{id}.json\" \
           || echo \"widget removed; state cleanup pending\"",
        id = widget_id,
    )
}

fn builtin_clawpkg_candidates(app_handle: &tauri::AppHandle, id: &str) -> Vec<PathBuf> {
    let mut candidates = Vec::new();

    if let Ok(resource_dir) = app_handle.path().resource_dir() {
        push_unique_path(
            &mut candidates,
            resource_dir.join("builtin-clawpkgs").join(id),
        );
    }

    #[cfg(debug_assertions)]
    {
        let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        push_unique_path(
            &mut candidates,
            manifest_dir.join("../builtin-clawpkgs").join(id),
        );

        if let Ok(current_dir) = env::current_dir() {
            push_unique_path(
                &mut candidates,
                current_dir.join("builtin-clawpkgs").join(id),
            );
            push_unique_path(
                &mut candidates,
                current_dir.join("../builtin-clawpkgs").join(id),
            );
            push_unique_path(
                &mut candidates,
                current_dir.join("ref/builtin-clawpkgs").join(id),
            );
        }

        if let Ok(home) = get_home_dir() {
            push_unique_path(
                &mut candidates,
                home.join(".openclaw").join("builtin-clawpkgs").join(id),
            );
        }
    }

    candidates
}

#[tauri::command]
async fn resolve_builtin_clawpkg_path(
    app_handle: tauri::AppHandle,
    id: String,
) -> Result<String, String> {
    let id = id.trim();
    if !is_safe_builtin_clawpkg_id(id) {
        return Err("内置组件 ID 非法".to_string());
    }

    let candidates = builtin_clawpkg_candidates(&app_handle, id);
    let attempted = candidates
        .iter()
        .map(|path| format!("  {}", path.display()))
        .collect::<Vec<_>>()
        .join("\n");

    for candidate in candidates {
        if !candidate.exists() {
            continue;
        }
        let validation = crate::clawpkg::validate_clawpkg_at_path(&candidate)?;
        if validation.ok {
            return Ok(candidate.to_string_lossy().to_string());
        }
        return Err(format!(
            "内置组件 {} 校验失败: {}",
            id,
            validation.errors.join("; ")
        ));
    }

    Err(format!("找不到内置组件 {}；尝试过:\n{}", id, attempted))
}

#[tauri::command]
async fn install_clawpkg_over_usb(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    input: InstallClawpkgInput,
) -> Result<InstallClawpkgResult, String> {
    /* connection precondition */
    let status = usb_manager.status();
    if !status.connected {
        return Err("USB 未连接,请先通过 USB 连接设备".to_string());
    }

    /* validate */
    let path = std::path::PathBuf::from(&input.clawpkg_path);
    let validation = crate::clawpkg::validate_clawpkg_at_path(&path)?;
    if !validation.ok {
        return Ok(InstallClawpkgResult {
            ok: false,
            manifest: validation.manifest,
            errors: validation.errors,
            transferred_bytes: 0,
        });
    }
    let manifest = validation
        .manifest
        .clone()
        .ok_or_else(|| "manifest 缺失".to_string())?;
    let widget_id = manifest.id.clone();

    /* Dispatch on clawpkg shape:
    - DIRECTORY (skill-generated widget with runtime/widget.json) → full
      widget OTA via widget/begin+chunk+commit. Device-side board-widget-runtime
      picks up .active-widget change and starts the state machine.
    - ZIP file (legacy static .clawpkg) → just push the rendered
      COMPONENT_DASHBOARD_V1 payload via payload_write (single string,
      no state machine, no buttons functional). Kept for backward compat.
    The directory path is what the v2 skill emits and is what users will
    hit going forward. */
    if path.is_dir() {
        let mgr = usb_manager.inner().clone();
        let src = path.clone();
        let overrides = input.binding_overrides.clone();
        let wid = widget_id.clone();
        let (files, bytes) =
            tauri::async_runtime::spawn_blocking(move || -> Result<(u32, u64), String> {
                mgr.install_widget_clawpkg(&wid, &src, &overrides, |_cur, _total, _sent| {})
            })
            .await
            .map_err(|e| e.to_string())??;
        return Ok(InstallClawpkgResult {
            ok: true,
            manifest: Some(manifest),
            errors: vec![format!(
                "widget OTA done: {} files, {} bytes (base64)",
                files, bytes
            )],
            transferred_bytes: bytes as usize,
        });
    }

    /* Legacy zip path: render dashboard string + send payload_write */
    let mut dashboard = manifest.dashboard.clone();
    if let Some(footer) = input
        .footer_override
        .as_deref()
        .map(str::trim)
        .filter(|s| !s.is_empty())
    {
        dashboard.insert("footer".to_string(), footer.to_string());
    }
    let payload = crate::clawpkg::render_component_dashboard_payload(&dashboard);
    let payload_bytes = payload.len();

    let write_msg = serde_json::json!({
        "v": 1,
        "type": "payload_write",
        "path": ".stats-display",
        "content": payload,
    });
    let mut write_line =
        serde_json::to_string(&write_msg).map_err(|e| format!("serialize payload_write: {}", e))?;
    write_line.push('\n');
    usb_manager.send_command(&write_line)?;

    let switch_msg = serde_json::json!({
        "v": 1,
        "type": "payload_write",
        "path": ".screen-page",
        "content": "stats\n",
    });
    let mut switch_line = serde_json::to_string(&switch_msg)
        .map_err(|e| format!("serialize screen switch: {}", e))?;
    switch_line.push('\n');
    let _ = usb_manager.send_command(&switch_line);

    Ok(InstallClawpkgResult {
        ok: true,
        manifest: Some(manifest),
        errors: vec![],
        transferred_bytes: payload_bytes,
    })
}

/// Push a .clawpkg (directory OR zip) to a LAN-attached device over SSH.
///
/// Replaces the USB-serial transport for devices that aren't physically connected
/// (RPi via WiFi). Steps:
///   1. Validate the clawpkg locally (same `validate_clawpkg_at_path`)
///   2. Apply client-side `bindingOverrides` to `buttons.json` in-memory:
///      for each (action, new_control), find the matching entry in buttons.json
///      and swap its `control` + canonical `event`. widget.json transitions
///      reference action names so they stay untouched.
///   3. tar the (possibly mutated) widget dir into a temp file, scp to
///      `petagent@<ssh_host>:/tmp/`, ssh-extract under `/opt/board-runtime/widgets/<id>/`
///   4. SSH-write `<id>` to `/opt/board-runtime/.active-widget` — the device-side
///      `board-widget-runtime` daemon notices via inotify and re-loads.
///
/// Returns the same `InstallClawpkgResult` shape as the USB transport so the
/// frontend can use one render path. Errors surface as Err(String).
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct InstallClawpkgSshInput {
    /// Local filesystem path to .clawpkg directory or .zip file.
    clawpkg_path: String,
    /// SSH target like "petagent@<DEVICE_IP>". Reuses the user's existing SSH key /
    /// agent — we don't accept passwords here (privacy / one-off PAT semantics
    /// don't apply to long-lived SSH sessions).
    ssh_host: String,
    /// Action-id → new-control map from ComponentCenter's "按钮功能" UI.
    /// Empty map = no overrides, use buttons.json as-shipped.
    #[serde(default)]
    binding_overrides: std::collections::HashMap<String, String>,
}

#[tauri::command]
async fn install_clawpkg_over_ssh(
    input: InstallClawpkgSshInput,
) -> Result<InstallClawpkgResult, String> {
    let src_path = std::path::PathBuf::from(&input.clawpkg_path);
    let validation = crate::clawpkg::validate_clawpkg_at_path(&src_path)?;
    if !validation.ok {
        return Ok(InstallClawpkgResult {
            ok: false,
            manifest: validation.manifest,
            errors: validation.errors,
            transferred_bytes: 0,
        });
    }
    let manifest = validation
        .manifest
        .clone()
        .ok_or_else(|| "manifest 缺失".to_string())?;
    let widget_id = manifest.id.clone();

    let ssh_host = input.ssh_host.clone();
    let overrides = input.binding_overrides.clone();
    let src = src_path.clone();
    let tar_bin = require_host_command("tar", "SSH 下发组件需要本机 tar 命令")?;
    let ssh_bin = require_host_command("ssh", "SSH 下发组件需要本机 OpenSSH ssh 命令")?;

    tauri::async_runtime::spawn_blocking(move || -> Result<InstallClawpkgResult, String> {
        // Step 1: stage widget into a tmp dir we can mutate safely (avoid touching user's draft)
        let stage = tempfile::tempdir().map_err(|e| format!("tempdir: {}", e))?;
        let stage_widget = stage.path().join(&widget_id);
        if src.is_dir() {
            copy_dir_recursive(&src, &stage_widget).map_err(|e| format!("copy_dir: {}", e))?;
        } else {
            // zip path — unzip into stage_widget
            std::fs::create_dir_all(&stage_widget).map_err(|e| e.to_string())?;
            let f = std::fs::File::open(&src).map_err(|e| format!("open zip: {}", e))?;
            let mut archive = zip::ZipArchive::new(f).map_err(|e| format!("read zip: {}", e))?;
            if archive.len() > crate::clawpkg::CLAWPKG_MAX_ENTRIES {
                return Err("clawpkg 文件数超过安全上限".to_string());
            }
            let mut expanded_bytes = 0u64;
            for i in 0..archive.len() {
                let entry = archive.by_index(i).map_err(|e| e.to_string())?;
                let relative_path = entry
                    .enclosed_name()
                    .ok_or_else(|| format!("clawpkg 含不安全路径: {}", entry.name()))?
                    .to_path_buf();
                if entry
                    .unix_mode()
                    .is_some_and(|mode| mode & 0o170000 == 0o120000)
                {
                    return Err(format!("clawpkg 不允许符号链接: {}", entry.name()));
                }
                if entry.size() > crate::clawpkg::CLAWPKG_MAX_ENTRY_BYTES {
                    return Err(format!("clawpkg 文件过大: {}", entry.name()));
                }
                expanded_bytes = expanded_bytes
                    .checked_add(entry.size())
                    .ok_or_else(|| "clawpkg 解压大小溢出".to_string())?;
                if expanded_bytes > crate::clawpkg::CLAWPKG_MAX_EXPANDED_BYTES {
                    return Err("clawpkg 解压后总大小超过安全上限".to_string());
                }
                let outpath = stage_widget.join(relative_path);
                if entry.is_dir() {
                    std::fs::create_dir_all(&outpath).map_err(|e| e.to_string())?;
                } else {
                    if let Some(parent) = outpath.parent() {
                        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
                    }
                    let mut outfile = std::fs::File::create(&outpath).map_err(|e| e.to_string())?;
                    let mut limited = entry.take(crate::clawpkg::CLAWPKG_MAX_ENTRY_BYTES + 1);
                    let copied =
                        std::io::copy(&mut limited, &mut outfile).map_err(|e| e.to_string())?;
                    if copied > crate::clawpkg::CLAWPKG_MAX_ENTRY_BYTES {
                        return Err("clawpkg 文件解压后超过安全上限".to_string());
                    }
                }
            }
        }

        // Step 2: apply binding overrides to staged buttons.json
        if !overrides.is_empty() {
            let buttons_path = stage_widget.join("buttons.json");
            if buttons_path.exists() {
                let bytes = std::fs::read(&buttons_path).map_err(|e| e.to_string())?;
                if let Ok(mut arr) = serde_json::from_slice::<Vec<serde_json::Value>>(&bytes) {
                    for entry in arr.iter_mut() {
                        if let Some(obj) = entry.as_object_mut() {
                            if let Some(action) = obj.get("action").and_then(|v| v.as_str()) {
                                if let Some(new_control) = overrides.get(action) {
                                    if let Some((canonical_control, new_event)) =
                                        canonical_binding_for_control(new_control)
                                    {
                                        obj.insert(
                                            "control".to_string(),
                                            serde_json::Value::String(
                                                canonical_control.to_string(),
                                            ),
                                        );
                                        obj.insert(
                                            "event".to_string(),
                                            serde_json::Value::String(new_event.to_string()),
                                        );
                                    } else {
                                        obj.insert(
                                            "control".to_string(),
                                            serde_json::Value::String(new_control.clone()),
                                        );
                                    }
                                }
                            }
                        }
                    }
                    std::fs::write(
                        &buttons_path,
                        serde_json::to_vec_pretty(&arr).map_err(|e| e.to_string())?,
                    )
                    .map_err(|e| e.to_string())?;
                }
            }
        }

        // Step 3+4+5 (merged): stream `tar c | ssh 'tar x; activate'` in a
        // single SSH handshake. The old 3-stage flow (local tar → scp → ssh
        // exec) cost ~1.8s in fixed handshake overhead for a ~3KB widget —
        // 100% overhead. Now: one ssh connection, tarball never lands on disk
        // (host-side OR device-side), staging dir is mktemp under the user
        // (no sudo for tar extract), then a single atomic `mv` swaps the
        // widget into place — slightly more atomic than the old in-place
        // `sudo tar -xzf -C {dir}` which exposed a partial-extract window.
        let remote_widgets_dir = "/opt/board-runtime/widgets";
        let remote_script = format!(
            "set -e; \
             sudo mkdir -p {dir}; \
             stage=$(mktemp -d); \
             trap 'rm -rf \"$stage\"' EXIT; \
             tar -xzf - -C \"$stage\"; \
             sudo flock -x /opt/board-runtime/.widget-transaction.lock sh -c '\
               target={dir}/{id}; previous=\"$target.previous\"; \
               rm -rf -- \"$previous\"; \
               if [ -e \"$target\" ]; then mv \"$target\" \"$previous\"; fi; \
               if ! mv \"$1\" \"$target\"; then \
                 if [ -e \"$previous\" ]; then mv \"$previous\" \"$target\"; fi; \
                 exit 41; \
               fi; \
               if ! printf \"{id}\" > /opt/board-runtime/.active-widget; then \
                 rm -rf -- \"$target\"; \
                 if [ -e \"$previous\" ]; then mv \"$previous\" \"$target\"; fi; \
                 exit 42; \
               fi; \
               if ! printf stats > /opt/board-runtime/.screen-page; then \
                 echo \"widget installed; screen switch pending\" >&2; \
               fi' sh \"$stage/{id}\"; \
             rmdir \"$stage\" 2>/dev/null || true; \
             trap - EXIT",
            dir = remote_widgets_dir,
            id = widget_id,
        );
        let mut tar = Command::new(&tar_bin)
            .arg("-czf")
            .arg("-")
            .arg("-C")
            .arg(stage.path())
            .arg(&widget_id)
            .stdout(std::process::Stdio::piped())
            .spawn()
            .map_err(|e| format!("tar spawn: {}", e))?;
        let tar_stdout = tar
            .stdout
            .take()
            .ok_or_else(|| "tar stdout pipe missing".to_string())?;
        let ssh_status = Command::new(&ssh_bin)
            .arg("-o")
            .arg("StrictHostKeyChecking=accept-new")
            .arg("-o")
            .arg("BatchMode=yes")
            .arg(&ssh_host)
            .arg(&remote_script)
            .stdin(std::process::Stdio::from(tar_stdout))
            .status()
            .map_err(|e| format!("ssh: {}", e))?;
        let tar_status = tar.wait().map_err(|e| format!("tar wait: {}", e))?;
        if !tar_status.success() {
            return Err(format!("tar exited {}", tar_status));
        }
        if !ssh_status.success() {
            return Err(format!("ssh exited {}", ssh_status));
        }
        // best-effort transferred-bytes accounting: walk the staged dir for
        // uncompressed payload size (more meaningful to the UI than the gzip
        // wire bytes which we no longer materialize).
        let transferred_bytes: usize = {
            fn dir_bytes(p: &std::path::Path) -> std::io::Result<u64> {
                let mut acc = 0u64;
                for ent in std::fs::read_dir(p)? {
                    let ent = ent?;
                    let t = ent.file_type()?;
                    if t.is_dir() {
                        acc += dir_bytes(&ent.path()).unwrap_or(0);
                    } else if t.is_file() {
                        acc += ent.metadata().map(|m| m.len()).unwrap_or(0);
                    }
                }
                Ok(acc)
            }
            dir_bytes(&stage_widget).unwrap_or(0) as usize
        };

        Ok(InstallClawpkgResult {
            ok: true,
            manifest: Some(manifest),
            errors: vec![],
            transferred_bytes,
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Query one explicit device target for its live installed-component inventory.
/// USB uses the request-id-correlated board protocol; SSH evaluates one fixed
/// read-only Python scanner while holding the same shared widget lock as the
/// Linux runtime's native inventory handler.
#[tauri::command]
async fn list_device_widgets(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    input: ListDeviceWidgetsInput,
) -> Result<DeviceWidgetInventory, String> {
    let transport = input.transport.trim().to_ascii_lowercase();
    if transport == "ssh" {
        if !input.board_device_id.trim().is_empty() {
            return Err("SSH 组件清单不能同时指定 USB boardDeviceId。".to_string());
        }
        let ssh_host = normalize_widget_ssh_host(&input.ssh_host)?
            .ok_or_else(|| "SSH 组件清单必须指定目标主机。".to_string())?;
        let ssh_bin =
            require_host_command("ssh", "通过 SSH 查询组件清单需要本机 OpenSSH ssh 命令")?;
        let raw = tauri::async_runtime::spawn_blocking(move || {
            query_widgets_over_ssh(&ssh_bin, &ssh_host)
        })
        .await
        .map_err(|error| error.to_string())??;
        return normalize_device_widget_inventory(&raw, "ssh", "linux", None);
    }

    if transport != "usb" {
        return Err("组件清单传输方式必须明确为 usb 或 ssh。".to_string());
    }
    if normalize_widget_ssh_host(&input.ssh_host)?.is_some() {
        return Err("USB 组件清单不能同时指定 SSH 主机。".to_string());
    }
    let expected_board_device_id = input.board_device_id.trim().to_string();
    if expected_board_device_id.is_empty() {
        return Err("USB 组件清单必须指定当前 boardDeviceId。".to_string());
    }
    let status = usb_manager.status();
    if !status.connected {
        return Err("USB 未连接，请先连接设备后再查询组件清单。".to_string());
    }
    if status.board_device_id != expected_board_device_id {
        return Err(format!(
            "当前 USB 设备与查询目标不一致：期望 {}，实际 {}。",
            expected_board_device_id,
            if status.board_device_id.is_empty() {
                "<未识别>"
            } else {
                &status.board_device_id
            }
        ));
    }
    let manager = usb_manager.inner().clone();
    let query_board_device_id = expected_board_device_id.clone();
    let raw = tauri::async_runtime::spawn_blocking(move || {
        manager.query_widget_inventory(&query_board_device_id)
    })
    .await
    .map_err(|error| error.to_string())??;
    normalize_device_widget_inventory(
        &raw,
        "usb",
        &status.runtime,
        Some(&expected_board_device_id),
    )
}

/// Remove a component from one explicit install target. USB validates the
/// expected board id before the ACK-gated widget/delete protocol; SSH freezes
/// the recorded host and uses the shared widget transaction lock plus
/// tombstones so package cleanup cannot race an install.
#[tauri::command]
async fn remove_widget_from_device(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    input: RemoveWidgetInput,
) -> Result<RemoveWidgetResult, String> {
    let component_id = input.component_id.trim().to_string();
    if !is_safe_widget_id(&component_id) {
        return Err("组件 ID 非法；只允许小写字母、数字、- 和 _。".to_string());
    }

    let transport = input.transport.trim().to_ascii_lowercase();
    if transport == "ssh" {
        let ssh_host = normalize_widget_ssh_host(&input.ssh_host)?
            .ok_or_else(|| "SSH 删除必须指定安装时的目标主机。".to_string())?;
        if !input.board_device_id.trim().is_empty() {
            return Err("SSH 删除不能同时指定 USB boardDeviceId。".to_string());
        }
        let ssh_bin = require_host_command("ssh", "通过 SSH 删除组件需要本机 OpenSSH ssh 命令")?;
        let widget_id = component_id.clone();
        let warning =
            tauri::async_runtime::spawn_blocking(move || -> Result<Option<String>, String> {
                let delete_inner = ssh_widget_delete_inner_script(&widget_id);
                let remote_script = format!(
                    "set -eu; root=/opt/board-runtime; \
                     sudo mkdir -p \"$root\"; \
                     sudo flock -x \"$root/.widget-transaction.lock\" \
                       sh -c '{delete_inner}' sh \"$root\"",
                );
                let output = Command::new(&ssh_bin)
                    .arg("-o")
                    .arg("StrictHostKeyChecking=accept-new")
                    .arg("-o")
                    .arg("BatchMode=yes")
                    .arg(&ssh_host)
                    .arg(remote_script)
                    .output()
                    .map_err(|error| format!("ssh: {}", error))?;
                if output.status.success() {
                    let detail = String::from_utf8_lossy(&output.stdout).trim().to_string();
                    Ok((!detail.is_empty()).then_some(detail))
                } else {
                    let detail = String::from_utf8_lossy(&output.stderr).trim().to_string();
                    Err(if detail.is_empty() {
                        format!("ssh exited {}", output.status)
                    } else {
                        format!("SSH 删除组件失败: {}", detail)
                    })
                }
            })
            .await
            .map_err(|error| error.to_string())??;
        return Ok(RemoveWidgetResult {
            ok: true,
            component_id,
            transport: "ssh".to_string(),
            input_config_reset: false,
            warning,
        });
    }

    if transport != "usb" {
        return Err("删除传输方式必须明确为 usb 或 ssh。".to_string());
    }
    if normalize_widget_ssh_host(&input.ssh_host)?.is_some() {
        return Err("USB 删除不能同时指定 SSH 主机。".to_string());
    }
    let expected_board_device_id = input.board_device_id.trim().to_string();
    if expected_board_device_id.is_empty() {
        return Err("USB 删除必须指定安装时的 boardDeviceId。".to_string());
    }
    let status = usb_manager.status();
    if !status.connected {
        return Err("USB 未连接，请先连接设备后再移除组件。".to_string());
    }
    if status.board_device_id != expected_board_device_id {
        return Err(format!(
            "当前 USB 设备与组件安装目标不一致：期望 {}，实际 {}。",
            expected_board_device_id,
            if status.board_device_id.is_empty() {
                "<未识别>"
            } else {
                &status.board_device_id
            }
        ));
    }
    let manager = usb_manager.inner().clone();
    let widget_id = component_id.clone();
    let board_device_id = expected_board_device_id;
    tauri::async_runtime::spawn_blocking(move || {
        manager.remove_widget(&board_device_id, &widget_id)
    })
    .await
    .map_err(|error| error.to_string())??;
    Ok(RemoveWidgetResult {
        ok: true,
        component_id,
        transport: "usb".to_string(),
        input_config_reset: false,
        warning: None,
    })
}

/// For a ComponentCenter option label, return the canonical control + event
/// pair written into buttons.json when the user remaps an action.
fn canonical_binding_for_control(control: &str) -> Option<(&'static str, &'static str)> {
    let binding = match control {
        "屏幕点击" => ("屏幕区域", "screen.region.tap"),
        "屏幕长按" => ("屏幕区域", "screen.region.long_press"),
        "SW1 短按" => ("SW1", "button.sw1.short_press"),
        "SW2 短按" => ("SW2", "button.sw2.short_press"),
        "SW3 短按" => ("SW3", "button.sw3.short_press"),
        "摇杆中按短按" => ("前方摇杆", "button.encoder.short_press"),
        "摇杆中按长按" => ("前方摇杆", "button.encoder.long_press"),
        "摇杆向上" => ("前方摇杆", "joystick.up"),
        "摇杆向下" => ("前方摇杆", "joystick.down"),
        "摇杆向左" => ("前方摇杆", "knob.rotate_ccw"),
        "摇杆向右" => ("前方摇杆", "knob.rotate_cw"),
        "摇杆左右方向" => ("前方摇杆", "knob.rotate_cw / knob.rotate_ccw"),
        // Backward-compatible labels and event names from encoder hardware.
        "旋钮短按" => ("前方旋钮", "button.encoder.short_press"),
        "旋钮长按" => ("前方旋钮", "button.encoder.long_press"),
        "旋钮顺时针" => ("前方旋钮", "knob.rotate_cw"),
        "旋钮逆时针" => ("前方旋钮", "knob.rotate_ccw"),
        "旋钮双向旋转" => ("前方旋钮", "knob.rotate_cw / knob.rotate_ccw"),
        "旋钮旋转" => ("前方旋钮", "knob.rotate_cw / knob.rotate_ccw"),
        // Backward-compatible labels from older component-center builds.
        "屏幕区域" => ("屏幕区域", "screen.region.tap"),
        _ => return None,
    };
    Some(binding)
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PrepareClawpkgForSyncInput {
    path: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct PreparedClawpkgForSyncResult {
    path: String,
    id: String,
    name: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ReleaseClawpkgSyncSnapshotInput {
    path: String,
}

fn component_sync_cache_root(app_handle: &tauri::AppHandle) -> Result<PathBuf, String> {
    app_handle
        .path()
        .app_data_dir()
        .map(|path| path.join(COMPONENT_SYNC_CACHE_DIR_NAME))
        .map_err(|error| format!("无法定位组件同步缓存目录: {}", error))
}

fn copy_clawpkg_sync_snapshot(source: &Path, cache_root: &Path) -> Result<PathBuf, String> {
    if !source.exists() {
        return Err(format!("组件安装源已不存在: {}", source.display()));
    }
    let validation = crate::clawpkg::validate_clawpkg_at_path(source)?;
    if !validation.ok {
        return Err(format!(
            "组件安装源校验失败: {}",
            validation.errors.join("; ")
        ));
    }

    fs::create_dir_all(cache_root)
        .map_err(|error| format!("创建组件同步缓存失败 {}: {}", cache_root.display(), error))?;
    let snapshot_root = cache_root.join(uuid::Uuid::new_v4().to_string());
    let snapshot_path = if source.is_dir() {
        snapshot_root.join("package")
    } else {
        let extension = source
            .extension()
            .and_then(|value| value.to_str())
            .filter(|value| !value.is_empty())
            .unwrap_or("clawpkg");
        snapshot_root.join(format!("package.{}", extension))
    };

    let copied = if source.is_dir() {
        copy_dir_recursive(source, &snapshot_path)
            .map(|_| ())
            .map_err(|error| format!("冻结组件目录失败 {}: {}", source.display(), error))
    } else {
        fs::create_dir_all(&snapshot_root)
            .and_then(|_| fs::copy(source, &snapshot_path).map(|_| ()))
            .map_err(|error| format!("冻结组件文件失败 {}: {}", source.display(), error))
    };
    if let Err(error) = copied {
        let _ = fs::remove_dir_all(&snapshot_root);
        return Err(error);
    }

    match crate::clawpkg::validate_clawpkg_at_path(&snapshot_path) {
        Ok(result) if result.ok => Ok(snapshot_path),
        Ok(result) => {
            let _ = fs::remove_dir_all(&snapshot_root);
            Err(format!(
                "冻结后的组件包校验失败: {}",
                result.errors.join("; ")
            ))
        }
        Err(error) => {
            let _ = fs::remove_dir_all(&snapshot_root);
            Err(error)
        }
    }
}

fn release_clawpkg_sync_snapshot_at_path(
    snapshot_path: &Path,
    cache_root: &Path,
) -> Result<bool, String> {
    if !snapshot_path.exists() {
        return Ok(false);
    }
    let canonical_root = cache_root
        .canonicalize()
        .map_err(|error| format!("无法解析组件同步缓存目录: {}", error))?;
    let canonical_snapshot = snapshot_path
        .canonicalize()
        .map_err(|error| format!("无法解析组件同步快照: {}", error))?;
    let relative = canonical_snapshot
        .strip_prefix(&canonical_root)
        .map_err(|_| "拒绝清理组件同步缓存目录之外的路径".to_string())?;
    let snapshot_id = relative
        .components()
        .next()
        .ok_or_else(|| "组件同步快照路径无效".to_string())?;
    let snapshot_root = canonical_root.join(snapshot_id.as_os_str());
    fs::remove_dir_all(&snapshot_root).map_err(|error| {
        format!(
            "清理组件同步快照失败 {}: {}",
            snapshot_root.display(),
            error
        )
    })?;
    Ok(true)
}

#[tauri::command]
async fn prepare_clawpkg_for_sync(
    app_handle: tauri::AppHandle,
    input: PrepareClawpkgForSyncInput,
) -> Result<PreparedClawpkgForSyncResult, String> {
    let source = PathBuf::from(&input.path);
    let cache_root = component_sync_cache_root(&app_handle)?;
    let snapshot_path = copy_clawpkg_sync_snapshot(&source, &cache_root)?;
    let validation = crate::clawpkg::validate_clawpkg_at_path(&snapshot_path)?;
    let manifest = validation
        .manifest
        .ok_or_else(|| "冻结后的组件包缺少 manifest".to_string())?;
    Ok(PreparedClawpkgForSyncResult {
        path: snapshot_path.display().to_string(),
        id: manifest.id,
        name: manifest.name,
    })
}

#[tauri::command]
async fn release_clawpkg_sync_snapshot(
    app_handle: tauri::AppHandle,
    input: ReleaseClawpkgSyncSnapshotInput,
) -> Result<bool, String> {
    let cache_root = component_sync_cache_root(&app_handle)?;
    release_clawpkg_sync_snapshot_at_path(Path::new(&input.path), &cache_root)
}

#[tauri::command]
async fn purge_clawpkg_sync_cache(app_handle: tauri::AppHandle) -> Result<bool, String> {
    let cache_root = component_sync_cache_root(&app_handle)?;
    if !cache_root.exists() {
        return Ok(false);
    }
    fs::remove_dir_all(&cache_root)
        .map_err(|error| format!("清理组件同步缓存失败 {}: {}", cache_root.display(), error))?;
    Ok(true)
}

/// Return the latest published version of each formal local component.
#[tauri::command]
async fn list_component_library() -> Result<component_library::ComponentLibrarySnapshot, String> {
    let home = get_home_dir()?;
    tauri::async_runtime::spawn_blocking(move || component_library::list(&home))
        .await
        .map_err(|error| error.to_string())?
}

/// Inspect an arbitrary package for sync preflight without publishing it.
#[tauri::command]
async fn inspect_clawpkg(path: String) -> Result<component_library::ComponentLibraryEntry, String> {
    let path = PathBuf::from(path);
    tauri::async_runtime::spawn_blocking(move || component_library::inspect(&path))
        .await
        .map_err(|error| error.to_string())?
}

#[tauri::command]
async fn delete_component_from_library(
    input: component_library::DeleteLibraryComponentInput,
) -> Result<component_library::DeleteLibraryComponentResult, String> {
    let home = get_home_dir()?;
    tauri::async_runtime::spawn_blocking(move || component_library::delete(&home, input))
        .await
        .map_err(|error| error.to_string())?
}

#[tauri::command]
async fn install_codex_community_pet(
    pet_id: String,
) -> Result<codex_import::CodexCommunityInstallResult, String> {
    tauri::async_runtime::spawn_blocking(move || codex_import::install_codex_community_pet(&pet_id))
        .await
        .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn open_external_url(url: String) -> Result<(), String> {
    let target = resolve_open_target(&url)?;
    tauri::async_runtime::spawn_blocking(move || -> Result<(), String> {
        #[cfg(target_os = "macos")]
        let status = command_for_host("open").arg(&target).status();
        #[cfg(target_os = "linux")]
        let status = command_for_host("xdg-open").arg(&target).status();
        #[cfg(target_os = "windows")]
        let status = command_for_host("cmd")
            .args(["/C", "start", "", &target])
            .status();
        match status {
            Ok(s) if s.success() => Ok(()),
            Ok(s) => Err(format!("打开外部资源失败 (exit {:?})", s.code())),
            Err(e) => Err(format!("打开外部资源失败: {}", e)),
        }
    })
    .await
    .map_err(|e| e.to_string())?
}

fn resolve_open_target(input: &str) -> Result<String, String> {
    let trimmed = input.trim();
    if trimmed.starts_with("http://") || trimmed.starts_with("https://") {
        return Ok(trimmed.to_string());
    }
    Err("仅支持 http(s) 外部链接".to_string())
}

fn normalize_agent_id(value: &str) -> Option<String> {
    let normalized = value.trim().to_ascii_lowercase();
    match normalized.as_str() {
        "codex" => Some("codex".to_string()),
        "claude" | "claude-code" => Some("claude-code".to_string()),
        "openclaw" => Some("openclaw".to_string()),
        "mimo" | "mimo-code" | "mimocode" => Some("mimocode".to_string()),
        "copilot" | "copilot-cli" => Some("copilot-cli".to_string()),
        "gemini" | "gemini-cli" => Some("gemini-cli".to_string()),
        "cursor" => Some("cursor".to_string()),
        _ => {
            if normalized.is_empty() {
                None
            } else {
                Some(normalized)
            }
        }
    }
}

fn compact_usb_state_payload(source: &str, payload: &serde_json::Value) -> serde_json::Value {
    let mut out = serde_json::Map::new();
    out.insert(
        "source".to_string(),
        serde_json::Value::String(source.to_string()),
    );

    for key in [
        "state",
        "rawState",
        "reason",
        "event",
        "channel",
        "sessionId",
        "ts",
    ] {
        if let Some(value) = payload.get(key) {
            out.insert(key.to_string(), value.clone());
        }
    }

    if let Some(value) = payload.get("sessionTitle").and_then(|value| value.as_str()) {
        out.insert("sessionTitle".to_string(), serde_json::json!(value));
    }
    if let Some(display) = payload.get("display").and_then(|value| value.as_object()) {
        if let Some(title) = display.get("title").and_then(|value| value.as_str()) {
            out.insert("displayTitle".to_string(), serde_json::json!(title));
        }
        if let Some(content) = display.get("content").and_then(|value| value.as_str()) {
            out.insert("displayContent".to_string(), serde_json::json!(content));
        }
        if let Some(status) = display.get("status").and_then(|value| value.as_str()) {
            out.insert("statusText".to_string(), serde_json::json!(status));
        }
    }

    if let Some(ts_ms) = payload.get("tsMs").and_then(|value| value.as_u64()) {
        out.insert("tsMs".to_string(), serde_json::json!(ts_ms));
    } else {
        out.insert(
            "tsMs".to_string(),
            serde_json::json!(current_timestamp_ms()),
        );
    }

    for key in ["dailyTokenUsage", "tokenUsage", "token_usage", "usage"] {
        if let Some(value) = payload.get(key).and_then(|value| value.as_object()) {
            out.insert(
                "tokenUsage".to_string(),
                serde_json::Value::Object(value.clone()),
            );
            if key == "dailyTokenUsage" {
                out.insert("tokenUsagePeriod".to_string(), serde_json::json!("today"));
            }
            break;
        }
    }

    if let Some(value) = payload.get("metrics").and_then(|value| value.as_object()) {
        out.insert(
            "metrics".to_string(),
            serde_json::Value::Object(value.clone()),
        );
    }

    if !out.contains_key("state") {
        if let Some(raw_state) = payload.get("rawState").and_then(|value| value.as_str()) {
            out.insert("state".to_string(), serde_json::json!(raw_state));
        }
    }

    serde_json::Value::Object(out)
}

fn build_disabled_usb_state_payload(source: &str) -> serde_json::Value {
    serde_json::json!({
        "source": source,
        "state": "idle",
        "reason": "source.disabled",
        "event": "source.disabled",
        "tsMs": current_timestamp_ms(),
    })
}

fn non_empty_env(name: &str) -> Option<String> {
    env::var(name)
        .ok()
        .map(|value| value.trim().to_string())
        .filter(|value| !value.is_empty())
}

fn parse_u16_env(name: &str) -> Option<u16> {
    non_empty_env(name).and_then(|value| value.parse::<u16>().ok())
}

fn normalize_state_for_board(value: &str) -> String {
    match value.trim().to_ascii_lowercase().replace('-', "_").as_str() {
        "thinking" | "tool_running" | "working" => "working".to_string(),
        "speaking" => "speaking".to_string(),
        "waiting_user" | "waiting" | "needs_user" => "waiting_user".to_string(),
        "done" | "complete" | "completed" => "done".to_string(),
        "error" | "failed" => "error".to_string(),
        "idle" | "" => "idle".to_string(),
        _ => "idle".to_string(),
    }
}

fn fallback_speech_text_for_state(state: &str) -> &'static str {
    match state {
        "working" => "努力工作中",
        "speaking" => "正在回复",
        "waiting_user" => "等你确认",
        "done" => "任务完成",
        "error" => "出错了",
        _ => "休息中",
    }
}

fn speech_text_for_state_payload(payload: &serde_json::Value, state: &str) -> String {
    first_non_empty_string_field(
        payload,
        &[
            "displayContent",
            "display_content",
            "speechText",
            "speech_text",
            "content",
            "text",
            "message",
        ],
    )
    .unwrap_or_else(|| fallback_speech_text_for_state(state).to_string())
}

fn pet_screens_config_paths() -> Result<Vec<PathBuf>, String> {
    let home = get_home_dir()?;
    Ok(vec![
        home.join(CLAW_PET_DIR_NAME).join(PET_SCREENS_FILE_NAME),
        home.join(LEGACY_OPENCLAW_DIR_NAME)
            .join(PET_SCREENS_FILE_NAME),
    ])
}

fn read_pet_screen_ssh_fallback_from_file(path: &Path) -> Option<SshStateFallbackTarget> {
    let raw = fs::read_to_string(path).ok()?;
    let store: PetScreensStoreFile = serde_json::from_str(&raw).ok()?;
    let active = store.active_board_device_id.trim();
    let screen = store
        .screens
        .iter()
        .find(|screen| !active.is_empty() && screen.board_device_id.trim() == active)
        .or_else(|| store.screens.first())?;
    let host = if !screen.ssh_host.trim().is_empty() {
        screen.ssh_host.trim().to_string()
    } else if !screen.ssh_user.trim().is_empty() && !screen.host.trim().is_empty() {
        format!("{}@{}", screen.ssh_user.trim(), screen.host.trim())
    } else {
        String::new()
    };
    if host.is_empty() {
        return None;
    }
    Some(SshStateFallbackTarget {
        host,
        port: screen.ssh_port,
        password: screen.ssh_password.trim().to_string(),
        root_dir: if screen.ssh_root_dir.trim().is_empty() {
            DEFAULT_BOARD_RUNTIME_ROOT.to_string()
        } else {
            screen.ssh_root_dir.trim().to_string()
        },
    })
}

fn resolve_ssh_state_fallback_target() -> Option<SshStateFallbackTarget> {
    if let Some(host) = non_empty_env("PET_MANAGER_STATE_FALLBACK_SSH_HOST")
        .or_else(|| non_empty_env("PET_CLAW_STATE_FALLBACK_SSH_HOST"))
    {
        return Some(SshStateFallbackTarget {
            host,
            port: parse_u16_env("PET_MANAGER_STATE_FALLBACK_SSH_PORT")
                .or_else(|| parse_u16_env("PET_CLAW_STATE_FALLBACK_SSH_PORT")),
            password: non_empty_env("PET_MANAGER_STATE_FALLBACK_SSH_PASSWORD")
                .or_else(|| non_empty_env("PET_CLAW_STATE_FALLBACK_SSH_PASSWORD"))
                .unwrap_or_default(),
            root_dir: non_empty_env("PET_MANAGER_STATE_FALLBACK_ROOT")
                .or_else(|| non_empty_env("PET_CLAW_STATE_FALLBACK_ROOT"))
                .unwrap_or_else(|| DEFAULT_BOARD_RUNTIME_ROOT.to_string()),
        });
    }

    let paths = pet_screens_config_paths().ok()?;
    paths
        .iter()
        .find_map(|path| read_pet_screen_ssh_fallback_from_file(path))
}

fn build_ssh_fallback_debug_json(
    source: &str,
    payload: &serde_json::Value,
    state: &str,
    event: &str,
    reason: &str,
    now_ms: u64,
) -> String {
    let session_id = first_non_empty_string_field(payload, &["sessionId", "session_id"]);
    let session_key = first_non_empty_string_field(payload, &["sessionKey", "session_key"]);
    let active_key = session_id
        .as_ref()
        .map(|id| format!("{source}:session:{id}"))
        .or_else(|| {
            session_key
                .as_ref()
                .map(|id| format!("{source}:session:{id}"))
        })
        .unwrap_or_else(|| format!("{source}:source:{source}"));
    serde_json::json!({
        "resolvedState": state,
        "resolvedEvent": event,
        "activeSessionKey": active_key,
        "lastReason": reason,
        "updatedAtMs": now_ms,
        "records": [{
            "sessionKey": active_key,
            "source": source,
            "state": state,
            "event": event,
            "seq": 0,
            "updatedAtMs": now_ms,
            "displayUntilMs": 0,
            "candidate": true
        }]
    })
    .to_string()
}

fn send_state_via_ssh_fallback(source: &str, payload: &serde_json::Value) -> Result<(), String> {
    let target = resolve_ssh_state_fallback_target()
        .ok_or_else(|| "未配置 SSH 状态 fallback 目标。".to_string())?;
    let state = normalize_state_for_board(
        payload
            .get("state")
            .and_then(|value| value.as_str())
            .unwrap_or("idle"),
    );
    let event = first_non_empty_string_field(payload, &["event"]).unwrap_or_default();
    let reason = first_non_empty_string_field(payload, &["reason"]).unwrap_or_default();
    let speech = speech_text_for_state_payload(payload, &state);
    let now_ms = current_timestamp_ms();
    let debug_json =
        build_ssh_fallback_debug_json(source, payload, &state, &event, &reason, now_ms);
    let script = format!(
        "set -eu\nroot={root}\nstate={state}\nevent={event}\nspeech={speech}\ndebug_json={debug_json}\nsudo mkdir -p \"$root\"\nprintf '%s' \"$state\" | sudo tee \"$root/.current-state\" >/dev/null\nprintf '%s' \"$event\" | sudo tee \"$root/.current-event\" >/dev/null\nprintf '%s' \"$speech\" | sudo tee \"$root/.current-speech\" >/dev/null\nprintf '%s' \"$debug_json\" | sudo tee \"$root/.debug-session-state.json\" >/dev/null\n",
        root = shell_quote(&target.root_dir),
        state = shell_quote(&state),
        event = shell_quote(&event),
        speech = shell_quote(&speech),
        debug_json = shell_quote(&debug_json),
    );

    let mut command = if target.password.is_empty() {
        let mut command = command_for_host("ssh");
        command.arg("-o").arg("BatchMode=yes");
        command
    } else {
        let mut command = command_for_host("sshpass");
        command.arg("-e").arg("ssh");
        command.env("SSHPASS", &target.password);
        command
    };
    command
        .arg("-o")
        .arg("StrictHostKeyChecking=no")
        .arg("-o")
        .arg("UserKnownHostsFile=/dev/null")
        .arg("-o")
        .arg("ConnectTimeout=2");
    if let Some(port) = target.port {
        command.arg("-p").arg(port.to_string());
    }
    let output = command
        .arg(&target.host)
        .arg(script)
        .output()
        .map_err(|error| format!("SSH fallback 启动失败: {error}"))?;
    if output.status.success() {
        Ok(())
    } else {
        let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
        Err(format!(
            "SSH fallback 写入失败 ({}): {}",
            output.status, stderr
        ))
    }
}

fn enabled_usb_filter_signature(enabled_agents: &HashSet<String>) -> String {
    let mut sources: Vec<&str> = enabled_agents.iter().map(String::as_str).collect();
    sources.sort_unstable();
    sources.join(",")
}

fn disabled_usb_sources_for_filter(
    previous_enabled_agents: &HashSet<String>,
    next_enabled_agents: &HashSet<String>,
) -> HashSet<String> {
    let mut disabled_sources = HashSet::new();
    if next_enabled_agents.is_empty() {
        return disabled_sources;
    }

    for source in previous_enabled_agents.difference(next_enabled_agents) {
        disabled_sources.insert(source.clone());
    }
    for source in KNOWN_USB_STATE_SOURCES {
        if !next_enabled_agents.contains(source) {
            disabled_sources.insert(source.to_string());
        }
    }

    disabled_sources
}

fn recent_bridge_json_paths(directory: &Path, now_ms: u64) -> Vec<PathBuf> {
    let entries = match fs::read_dir(directory) {
        Ok(entries) => entries,
        Err(_) => return Vec::new(),
    };
    let mut candidates = Vec::new();

    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|extension| extension.to_str()) != Some("json") {
            continue;
        }

        let modified_ms = entry
            .metadata()
            .ok()
            .and_then(|metadata| metadata.modified().ok())
            .and_then(|modified| modified.duration_since(UNIX_EPOCH).ok())
            .map(|duration| duration.as_millis() as u64);
        if modified_ms
            .is_some_and(|modified| now_ms.saturating_sub(modified) > USB_STATE_MAX_AGE_MS)
        {
            let _ = fs::remove_file(&path);
            continue;
        }
        candidates.push((modified_ms.unwrap_or(0), path));
    }

    candidates.sort_by(|left, right| right.0.cmp(&left.0).then_with(|| left.1.cmp(&right.1)));
    candidates.truncate(USB_BRIDGE_SCAN_MAX_FILES);
    candidates.into_iter().map(|(_, path)| path).collect()
}

fn usb_state_payload_is_fresh(path: &Path, payload: &serde_json::Value, now_ms: u64) -> bool {
    if let Some(ts_ms) = payload
        .get("tsMs")
        .and_then(|value| value.as_u64())
        .filter(|value| *value > 0)
    {
        return now_ms.saturating_sub(ts_ms) <= USB_STATE_MAX_AGE_MS;
    }

    let modified = match fs::metadata(path).and_then(|metadata| metadata.modified()) {
        Ok(value) => value,
        Err(_) => return false,
    };

    match SystemTime::now().duration_since(modified) {
        Ok(age) => age.as_millis() <= u128::from(USB_STATE_MAX_AGE_MS),
        Err(_) => true,
    }
}

fn retain_fresh_usb_state_payload(path: &Path, payload: &serde_json::Value, now_ms: u64) -> bool {
    if usb_state_payload_is_fresh(path, payload, now_ms) {
        return true;
    }
    let _ = fs::remove_file(path);
    false
}

fn usb_state_source_from_file_stem(raw_source: &str) -> String {
    let source = raw_source.split("--session-").next().unwrap_or(raw_source);
    normalize_agent_id(source).unwrap_or_else(|| source.to_string())
}

fn load_enabled_agents_filter_for_usb() -> std::collections::HashSet<String> {
    if let Ok(config_path) = get_bridge_profile_path() {
        if let Ok(Some(profile)) = read_bridge_profile(&config_path) {
            let normalized = apply_bridge_profile_defaults(normalize_bridge_profile(profile));
            return normalized
                .enabled_agents
                .into_iter()
                .filter_map(|id| normalize_agent_id(&id))
                .collect();
        }
    }
    std::collections::HashSet::new()
}

fn load_selected_agent_for_usb() -> Option<String> {
    get_bridge_profile_path()
        .ok()
        .and_then(|path| read_bridge_profile(&path).ok().flatten())
        .map(|profile| apply_bridge_profile_defaults(normalize_bridge_profile(profile)))
        .and_then(|profile| normalize_agent_id(&profile.selected_agent_id))
}

fn usb_source_allowed_by_follow(
    source: &str,
    selected_agent: &Option<String>,
    enabled_agents: &HashSet<String>,
) -> bool {
    if let Some(selected) = selected_agent {
        return source == selected;
    }
    enabled_agents.is_empty() || enabled_agents.contains(source)
}

fn score_usb_state(state: &str) -> i32 {
    match state {
        "error" => 60,
        "working" | "tool_running" | "thinking" => 50,
        "speaking" => 40,
        "waiting_user" => 30,
        "done" => 15,
        "idle" => 10,
        _ => 5,
    }
}

fn score_usb_source(source: &str) -> i32 {
    match source {
        "codex" => 30,
        "claude-code" => 20,
        "mimocode" => 15,
        "openclaw" => 10,
        _ => 0,
    }
}

fn should_replace_usb_source_state(
    existing: Option<(i32, u64)>,
    candidate_state_score: i32,
    candidate_ts_ms: u64,
) -> bool {
    match existing {
        Some((state_score, ts_ms)) => {
            candidate_state_score > state_score
                || (candidate_state_score == state_score && candidate_ts_ms > ts_ms)
        }
        None => true,
    }
}

fn usb_active_state_cache() -> &'static Mutex<Option<(String, serde_json::Value)>> {
    static CACHE: OnceLock<Mutex<Option<(String, serde_json::Value)>>> = OnceLock::new();
    CACHE.get_or_init(|| Mutex::new(None))
}

fn cache_last_usb_active_state(source: &str, payload: &serde_json::Value) {
    if let Ok(mut slot) = usb_active_state_cache().lock() {
        *slot = Some((source.to_string(), payload.clone()));
    }
}

fn load_last_usb_active_state() -> Option<(String, serde_json::Value)> {
    usb_active_state_cache()
        .lock()
        .ok()
        .and_then(|slot| slot.as_ref().cloned())
}

fn first_non_empty_string_field(payload: &serde_json::Value, keys: &[&str]) -> Option<String> {
    keys.iter()
        .find_map(|key| payload.get(key).and_then(|value| value.as_str()))
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(|value| value.to_string())
}

fn usb_source_display_name(source: &str) -> String {
    match source.trim().to_ascii_lowercase().as_str() {
        "codex" => "ChatGPT（Codex）".to_string(),
        "claude" | "claude-code" => "Claude".to_string(),
        "openclaw" => "OpenClaw".to_string(),
        "mimo" | "mimo-code" | "mimocode" => "MiMoCode".to_string(),
        other if !other.is_empty() => other.to_string(),
        _ => "桌宠".to_string(),
    }
}

fn usb_state_display_text(state: &str) -> Option<&'static str> {
    match state {
        "idle" => Some("待机中"),
        "working" | "thinking" | "tool_running" => Some("工作中"),
        "speaking" => Some("回复中"),
        "waiting_user" => Some("等待操作"),
        "done" => Some("已完成"),
        "error" => Some("出错了"),
        _ => None,
    }
}

fn build_usb_restore_speech_text(source: &str, payload: &serde_json::Value) -> Option<String> {
    if let Some(text) = first_non_empty_string_field(
        payload,
        &[
            "displayText",
            "display_text",
            "speechText",
            "speech_text",
            "displayContent",
            "display_content",
            "statusText",
            "status_text",
            "content",
            "text",
            "message",
        ],
    ) {
        return Some(text);
    }

    // "openclaw" is gateway-level fallback state; avoid overriding device text with
    // generic idle text when restoring from test speech.
    if source.eq_ignore_ascii_case("openclaw") {
        return None;
    }

    let state = payload
        .get("state")
        .and_then(|value| value.as_str())
        .unwrap_or("")
        .trim()
        .to_ascii_lowercase();
    let state_text = usb_state_display_text(&state)?;
    let source_name = usb_source_display_name(source);
    Some(format!("{} {}", source_name, state_text))
}

fn build_usb_speech_payload_from_state(
    _source: &str,
    _payload: &serde_json::Value,
) -> Option<serde_json::Value> {
    None
}

fn build_usb_active_speech_text(_source: &str, _payload: &serde_json::Value) -> Option<String> {
    None
}

fn pick_best_usb_bridge_state(
    exclude_speaking: bool,
    board_device_id: &str,
) -> Option<(String, serde_json::Value)> {
    let tmp = env::var("TMPDIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| env::temp_dir());
    let state_dir = tmp.join("pet-manager-bridge-state");
    let enabled_agents = load_enabled_agents_filter_for_usb();
    let selected_agent = load_selected_agent_for_usb();
    let now_ms = current_timestamp_ms();
    let paths = recent_bridge_json_paths(&state_dir, now_ms);

    let mut best_source = String::new();
    let mut best_payload: Option<serde_json::Value> = None;
    let mut best_state_score = i32::MIN;
    let mut best_source_score = i32::MIN;
    let mut best_ts_ms = 0u64;

    for path in paths {
        let raw_source = match path.file_stem().and_then(|s| s.to_str()) {
            Some(s) => s,
            None => continue,
        };
        let source = usb_state_source_from_file_stem(raw_source);
        if !usb_source_allowed_by_follow(&source, &selected_agent, &enabled_agents) {
            continue;
        }

        let content = match fs::read_to_string(&path) {
            Ok(c) => c.trim().to_string(),
            Err(_) => continue,
        };
        let payload = match serde_json::from_str::<serde_json::Value>(&content) {
            Ok(payload) => payload,
            Err(_) => continue,
        };
        if !retain_fresh_usb_state_payload(&path, &payload, now_ms) {
            continue;
        }
        if !usb_session_binding_allows(board_device_id, &source, &payload) {
            continue;
        }

        let state = payload
            .get("state")
            .and_then(|value| value.as_str())
            .unwrap_or("")
            .to_ascii_lowercase();
        if exclude_speaking && state == "speaking" {
            continue;
        }
        let ts_ms = payload
            .get("tsMs")
            .and_then(|value| value.as_u64())
            .unwrap_or(0);
        let current_state_score = score_usb_state(&state);
        let current_source_score = score_usb_source(&source);

        let is_better = current_state_score > best_state_score
            || (current_state_score == best_state_score && ts_ms > best_ts_ms)
            || (current_state_score == best_state_score
                && ts_ms == best_ts_ms
                && current_source_score > best_source_score);
        if is_better {
            best_source = source.clone();
            best_payload = Some(compact_usb_state_payload(&source, &payload));
            best_state_score = current_state_score;
            best_source_score = current_source_score;
            best_ts_ms = ts_ms;
        }
    }

    best_payload.map(|payload| (best_source, payload))
}

fn pick_usb_bridge_state_for_source(
    expected_source: &str,
    exclude_speaking: bool,
    board_device_id: &str,
) -> Option<(String, serde_json::Value)> {
    let expected = normalize_agent_id(expected_source)
        .unwrap_or_else(|| expected_source.trim().to_ascii_lowercase());
    if expected.is_empty() {
        return None;
    }

    let tmp = env::var("TMPDIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| env::temp_dir());
    let state_dir = tmp.join("pet-manager-bridge-state");
    let now_ms = current_timestamp_ms();
    let paths = recent_bridge_json_paths(&state_dir, now_ms);

    let mut best_payload: Option<serde_json::Value> = None;
    let mut best_ts_ms = 0u64;

    for path in paths {
        let raw_source = match path.file_stem().and_then(|s| s.to_str()) {
            Some(s) => s,
            None => continue,
        };
        let normalized_source = usb_state_source_from_file_stem(raw_source);
        if normalized_source != expected {
            continue;
        }

        let content = match fs::read_to_string(&path) {
            Ok(c) => c.trim().to_string(),
            Err(_) => continue,
        };
        let payload = match serde_json::from_str::<serde_json::Value>(&content) {
            Ok(payload) => payload,
            Err(_) => continue,
        };
        if !retain_fresh_usb_state_payload(&path, &payload, now_ms) {
            continue;
        }
        if !usb_session_binding_allows(board_device_id, &normalized_source, &payload) {
            continue;
        }
        let state = payload
            .get("state")
            .and_then(|value| value.as_str())
            .unwrap_or("")
            .to_ascii_lowercase();
        if exclude_speaking && state == "speaking" {
            continue;
        }
        let ts_ms = payload
            .get("tsMs")
            .and_then(|value| value.as_u64())
            .unwrap_or(0);
        if best_payload.is_none() || ts_ms >= best_ts_ms {
            best_ts_ms = ts_ms;
            best_payload = Some(compact_usb_state_payload(&normalized_source, &payload));
        }
    }

    best_payload.map(|payload| (expected, payload))
}

fn replay_usb_active_state(usb_manager: &usb_serial::UsbSerialManager) -> Result<(), String> {
    if !usb_manager.status().connected {
        return Ok(());
    }
    let board_device_id = usb_manager.status().board_device_id;
    let cached = load_last_usb_active_state();
    let same_source_non_speaking = cached
        .as_ref()
        .and_then(|(source, _)| pick_usb_bridge_state_for_source(source, true, &board_device_id));
    let same_source_any = cached
        .as_ref()
        .and_then(|(source, _)| pick_usb_bridge_state_for_source(source, false, &board_device_id));

    let (source, payload) = match same_source_non_speaking
        .or(same_source_any)
        .or(cached)
        .or_else(|| pick_best_usb_bridge_state(true, &board_device_id))
        .or_else(|| pick_best_usb_bridge_state(false, &board_device_id))
    {
        Some(pair) => pair,
        None => return Ok(()),
    };

    let _ = usb_manager.send_state(&source, &payload);

    let mut active_payload = payload.clone();
    if let Some(object) = active_payload.as_object_mut() {
        object.insert("activeTopic".to_string(), serde_json::json!(true));
        object.insert("source".to_string(), serde_json::json!(source.clone()));
    }
    usb_manager.send_state("active", &active_payload)?;
    if let Some(restore_text) = build_usb_restore_speech_text(&source, &payload) {
        if let Err(error) = usb_manager.send_speech(&restore_text) {
            eprintln!(
                "[usb-forwarder] restore speech after test failed(source={}): {}",
                source, error
            );
        } else {
            eprintln!(
                "[usb-forwarder] restored speech after test(source={}): {}",
                source, restore_text
            );
        }
    }
    cache_last_usb_active_state(&source, &payload);
    eprintln!(
        "[usb-forwarder] replayed state/active(source={}) after speech",
        source
    );
    Ok(())
}

fn forward_current_state_after_usb_connect(
    usb_manager: &usb_serial::UsbSerialManager,
) -> Result<(), String> {
    let status = usb_manager.status();
    if !status.connected || status.board_device_id.trim().is_empty() {
        return Ok(());
    }
    let Some((source, payload)) = pick_best_usb_bridge_state(true, &status.board_device_id)
        .or_else(|| pick_best_usb_bridge_state(false, &status.board_device_id))
    else {
        return Ok(());
    };

    usb_manager.send_state(&source, &payload)?;
    let mut active_payload = payload.clone();
    if let Some(object) = active_payload.as_object_mut() {
        object.insert("activeTopic".to_string(), serde_json::json!(true));
        object.insert("source".to_string(), serde_json::json!(source.clone()));
    }
    usb_manager.send_state("active", &active_payload)?;
    cache_last_usb_active_state(&source, &payload);
    eprintln!(
        "[usb-auto] replayed current state/{} and state/active after connect",
        source
    );
    Ok(())
}

fn forward_usb_speech_updates(
    usb_manager: &usb_serial::UsbSerialManager,
    speech_dir: &Path,
    board_device_id: &str,
    selected_agent: &Option<String>,
    enabled_agents: &std::collections::HashSet<String>,
    last_speech_signatures: &mut std::collections::HashMap<String, String>,
    now_ms: u64,
) {
    let mut seen_sources: std::collections::HashSet<String> = std::collections::HashSet::new();
    for path in recent_bridge_json_paths(speech_dir, now_ms) {
        let source = match path.file_stem().and_then(|s| s.to_str()) {
            Some(stem) => usb_state_source_from_file_stem(stem),
            None => continue,
        };
        if !usb_source_allowed_by_follow(&source, selected_agent, enabled_agents) {
            continue;
        }
        seen_sources.insert(source.clone());

        let content = match fs::read_to_string(&path) {
            Ok(content) => content.trim().to_string(),
            Err(_) => continue,
        };
        let payload = match serde_json::from_str::<serde_json::Value>(&content) {
            Ok(payload) => payload,
            Err(_) => continue,
        };
        if !usb_session_binding_allows(board_device_id, &source, &payload) {
            continue;
        }

        if let Some(expires_at_ms) = payload.get("expiresAtMs").and_then(|value| value.as_u64()) {
            if expires_at_ms > 0 && expires_at_ms < now_ms {
                let _ = fs::remove_file(&path);
                continue;
            }
        }

        let signature = serde_json::to_string(&payload).unwrap_or_else(|_| content.clone());
        if last_speech_signatures.get(&source).map(|s| s.as_str()) == Some(signature.as_str()) {
            continue;
        }

        match usb_manager.send("speech/text", &payload) {
            Ok(_) => {
                last_speech_signatures.insert(source.clone(), signature);
                eprintln!(
                    "[usb-forwarder] sent speech/text(source={}) -> {:?}",
                    source,
                    payload
                        .get("displayContent")
                        .or_else(|| payload.get("content"))
                        .or_else(|| payload.get("text"))
                );
            }
            Err(e) => eprintln!("[usb-forwarder] send_speech error: {}", e),
        }
    }

    last_speech_signatures.retain(|source, _| seen_sources.contains(source));
}

/// Keep the board's host lease alive independently from state and speech scans.
fn start_usb_host_heartbeat(usb_manager: usb_serial::UsbSerialManager) {
    thread::spawn(move || loop {
        thread::sleep(Duration::from_secs(2));
        if !usb_manager.status().connected {
            continue;
        }
        if let Err(error) = usb_manager.send(
            "system/heartbeat",
            &serde_json::json!({ "tsMs": current_timestamp_ms() }),
        ) {
            eprintln!("[usb-heartbeat] send failed: {}", error);
        }
    });
}

/// Background thread: poll bridge state files and forward to device via USB serial.
fn start_usb_state_forwarder(usb_manager: usb_serial::UsbSerialManager) {
    thread::spawn(move || {
        // macOS: os.tmpdir() in Node uses $TMPDIR (user-specific), not /tmp
        let tmp = env::var("TMPDIR")
            .map(PathBuf::from)
            .unwrap_or_else(|_| env::temp_dir());
        let state_dir = tmp.join("pet-manager-bridge-state");
        let speech_dir = tmp.join("pet-manager-bridge-speech");
        let mut last_enabled_refresh_ms: u64 = 0;
        let mut enabled_agents: std::collections::HashSet<String> =
            std::collections::HashSet::new();
        let mut selected_agent: Option<String> = None;
        let mut last_source_signatures: std::collections::HashMap<String, String> =
            std::collections::HashMap::new();
        let mut last_speech_signatures: std::collections::HashMap<String, String> =
            std::collections::HashMap::new();
        let mut last_active_signature = String::new();
        let mut last_active_speech_text = String::new();
        let mut last_disabled_filter_signature = String::new();
        let mut last_ssh_fallback_signature = String::new();
        let mut last_ssh_fallback_attempt_ms: u64 = 0;
        let mut last_ssh_fallback_error_ms: u64 = 0;
        let mut was_usb_connected = false;
        loop {
            thread::sleep(Duration::from_millis(800));

            let now_ms = current_timestamp_ms();
            let status = usb_manager.status();
            if !status.connected {
                if was_usb_connected {
                    last_source_signatures.clear();
                    last_speech_signatures.clear();
                    last_active_signature.clear();
                    last_active_speech_text.clear();
                    last_disabled_filter_signature.clear();
                    last_ssh_fallback_signature.clear();
                    last_ssh_fallback_attempt_ms = 0;
                }
                was_usb_connected = false;
                if let Some((source, payload)) = pick_best_usb_bridge_state(true, "")
                    .or_else(|| pick_best_usb_bridge_state(false, ""))
                {
                    let signature = format!(
                        "{}|{}",
                        source,
                        serde_json::to_string(&payload).unwrap_or_default()
                    );
                    let retry_due = now_ms.saturating_sub(last_ssh_fallback_attempt_ms)
                        >= SSH_STATE_FALLBACK_RETRY_MS;
                    if signature != last_ssh_fallback_signature || retry_due {
                        last_ssh_fallback_signature = signature;
                        last_ssh_fallback_attempt_ms = now_ms;
                        match send_state_via_ssh_fallback(&source, &payload) {
                            Ok(()) => {
                                eprintln!(
                                    "[state-forwarder] ssh fallback sent source={} -> {:?}",
                                    source,
                                    payload.get("state")
                                );
                            }
                            Err(error) => {
                                if now_ms.saturating_sub(last_ssh_fallback_error_ms)
                                    > SSH_STATE_FALLBACK_ERROR_LOG_MS
                                {
                                    eprintln!("[state-forwarder] ssh fallback skipped: {}", error);
                                    last_ssh_fallback_error_ms = now_ms;
                                }
                            }
                        }
                    }
                }
                continue;
            }
            if !was_usb_connected {
                last_source_signatures.clear();
                last_speech_signatures.clear();
                last_active_signature.clear();
                last_active_speech_text.clear();
                last_disabled_filter_signature.clear();
                last_ssh_fallback_signature.clear();
                last_ssh_fallback_attempt_ms = 0;
                was_usb_connected = true;
            }

            if now_ms.saturating_sub(last_enabled_refresh_ms) > 2500 {
                let previous_enabled_agents = enabled_agents.clone();
                let mut next_enabled_agents: std::collections::HashSet<String> =
                    std::collections::HashSet::new();
                let mut next_selected_agent: Option<String> = None;
                if let Ok(config_path) = get_bridge_profile_path() {
                    if let Ok(Some(profile)) = read_bridge_profile(&config_path) {
                        let normalized =
                            apply_bridge_profile_defaults(normalize_bridge_profile(profile));
                        next_selected_agent = normalize_agent_id(&normalized.selected_agent_id);
                        next_enabled_agents = normalized
                            .enabled_agents
                            .into_iter()
                            .filter_map(|id| normalize_agent_id(&id))
                            .collect();
                    }
                }
                let next_filter_signature = enabled_usb_filter_signature(&next_enabled_agents);
                if next_filter_signature != last_disabled_filter_signature {
                    last_source_signatures.clear();
                    last_speech_signatures.clear();
                    last_active_signature.clear();
                    last_active_speech_text.clear();
                }
                if !next_enabled_agents.is_empty()
                    && next_filter_signature != last_disabled_filter_signature
                {
                    for source in disabled_usb_sources_for_filter(
                        &previous_enabled_agents,
                        &next_enabled_agents,
                    ) {
                        let disabled_payload = build_disabled_usb_state_payload(&source);
                        match usb_manager.send_state(&source, &disabled_payload) {
                            Ok(_) => {
                                last_source_signatures.remove(&source);
                                last_speech_signatures.remove(&source);
                                eprintln!("[usb-forwarder] cleared disabled state/{}", source);
                            }
                            Err(e) => {
                                eprintln!("[usb-forwarder] clear disabled state error: {}", e)
                            }
                        }
                    }
                    last_disabled_filter_signature = next_filter_signature;
                } else if next_enabled_agents.is_empty() {
                    last_disabled_filter_signature.clear();
                }
                enabled_agents = next_enabled_agents;
                selected_agent = next_selected_agent;
                last_enabled_refresh_ms = now_ms;
            }

            forward_usb_speech_updates(
                &usb_manager,
                &speech_dir,
                &status.board_device_id,
                &selected_agent,
                &enabled_agents,
                &mut last_speech_signatures,
                now_ms,
            );

            let mut seen_sources: std::collections::HashSet<String> =
                std::collections::HashSet::new();
            let mut best_payload_by_source: std::collections::HashMap<
                String,
                (serde_json::Value, String, i32, u64),
            > = std::collections::HashMap::new();
            let mut best_state_score = i32::MIN;
            let mut best_source_score = i32::MIN;
            let mut best_ts_ms = 0u64;
            let mut best_source = String::new();
            let mut best_payload: Option<serde_json::Value> = None;
            let mut best_signature = String::new();

            for path in recent_bridge_json_paths(&state_dir, now_ms) {
                let raw_source = match path.file_stem().and_then(|s| s.to_str()) {
                    Some(s) => s,
                    None => continue,
                };
                let source = usb_state_source_from_file_stem(raw_source);
                if !usb_source_allowed_by_follow(&source, &selected_agent, &enabled_agents) {
                    continue;
                }
                let content = match fs::read_to_string(&path) {
                    Ok(c) => c.trim().to_string(),
                    Err(_) => continue,
                };

                // Parse and forward via USB — device-side does its own state normalization
                if let Ok(payload) = serde_json::from_str::<serde_json::Value>(&content) {
                    if !retain_fresh_usb_state_payload(&path, &payload, now_ms) {
                        continue;
                    }
                    if !usb_session_binding_allows(&status.board_device_id, &source, &payload) {
                        continue;
                    }

                    let ts_ms = payload
                        .get("tsMs")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0);
                    let compact_payload = compact_usb_state_payload(&source, &payload);
                    let compact_signature =
                        serde_json::to_string(&compact_payload).unwrap_or_else(|_| content.clone());

                    let state = payload
                        .get("state")
                        .and_then(|value| value.as_str())
                        .unwrap_or("")
                        .to_ascii_lowercase();

                    let current_state_score = score_usb_state(&state);
                    let current_source_score = score_usb_source(&source);
                    let existing_source_score = best_payload_by_source
                        .get(&source)
                        .map(|(_, _, state_score, state_ts_ms)| (*state_score, *state_ts_ms));
                    let replace_source_payload = should_replace_usb_source_state(
                        existing_source_score,
                        current_state_score,
                        ts_ms,
                    );
                    if replace_source_payload {
                        best_payload_by_source.insert(
                            source.clone(),
                            (
                                compact_payload.clone(),
                                compact_signature.clone(),
                                current_state_score,
                                ts_ms,
                            ),
                        );
                    }

                    let is_better = current_state_score > best_state_score
                        || (current_state_score == best_state_score && ts_ms > best_ts_ms)
                        || (current_state_score == best_state_score
                            && ts_ms == best_ts_ms
                            && current_source_score > best_source_score);
                    if is_better {
                        best_source = source.clone();
                        best_payload = Some(compact_payload);
                        best_state_score = current_state_score;
                        best_source_score = current_source_score;
                        best_ts_ms = ts_ms;
                        best_signature = compact_signature;
                    }
                }
            }

            for (source, (compact_payload, compact_signature, _, _)) in best_payload_by_source {
                seen_sources.insert(source.clone());
                if last_source_signatures.get(&source).map(|s| s.as_str())
                    == Some(compact_signature.as_str())
                {
                    continue;
                }
                match usb_manager.send_state(&source, &compact_payload) {
                    Ok(_) => {
                        last_source_signatures.insert(source.clone(), compact_signature);
                        eprintln!(
                            "[usb-forwarder] sent state/{} -> {:?}",
                            source,
                            compact_payload.get("state")
                        );
                        if let Some(speech_payload) =
                            build_usb_speech_payload_from_state(&source, &compact_payload)
                        {
                            let speech_signature =
                                serde_json::to_string(&speech_payload).unwrap_or_default();
                            if last_speech_signatures.get(&source).map(|s| s.as_str())
                                != Some(speech_signature.as_str())
                            {
                                match usb_manager.send("speech/text", &speech_payload) {
                                    Ok(_) => {
                                        last_speech_signatures
                                            .insert(source.clone(), speech_signature);
                                        eprintln!(
                                            "[usb-forwarder] sent speech/text(source={}) from state -> {:?}",
                                            source,
                                            speech_payload
                                                .get("displayContent")
                                                .or_else(|| speech_payload.get("content"))
                                                .or_else(|| speech_payload.get("text"))
                                        );
                                    }
                                    Err(e) => {
                                        eprintln!("[usb-forwarder] send_speech error: {}", e)
                                    }
                                }
                            }
                        }
                    }
                    Err(e) => eprintln!("[usb-forwarder] send_state error: {}", e),
                }
            }

            last_source_signatures.retain(|source, _| seen_sources.contains(source));

            let base_payload = match best_payload {
                Some(payload) => payload,
                None => continue,
            };
            let mut active_payload = base_payload.clone();
            if let Some(object) = active_payload.as_object_mut() {
                object.insert("activeTopic".to_string(), serde_json::json!(true));
                object.insert("source".to_string(), serde_json::json!(best_source.clone()));
            }

            let active_signature = format!("{}|{}", best_source, best_signature);
            if active_signature == last_active_signature {
                continue;
            }

            match usb_manager.send_state("active", &active_payload) {
                Ok(_) => {
                    last_active_signature = active_signature;
                    cache_last_usb_active_state(&best_source, &base_payload);
                    if let Some(speech_text) =
                        build_usb_active_speech_text(&best_source, &base_payload)
                    {
                        if speech_text != last_active_speech_text {
                            match usb_manager.send_speech(&speech_text) {
                                Ok(_) => {
                                    last_active_speech_text = speech_text.clone();
                                    eprintln!(
                                        "[usb-forwarder] sent speech(active source={}): {}",
                                        best_source, speech_text
                                    );
                                }
                                Err(error) => {
                                    eprintln!("[usb-forwarder] send active speech error: {}", error)
                                }
                            }
                        }
                    }
                    eprintln!(
                        "[usb-forwarder] sent state/active(source={}) -> {:?}",
                        best_source,
                        active_payload.get("state")
                    );
                }
                Err(e) => eprintln!("[usb-forwarder] send_state error: {}", e),
            }
        }
    });
}

fn usb_auto_probe_key(device: &usb_serial::UsbDeviceInfo) -> String {
    format!(
        "{}|{:04x}|{:04x}|{}",
        device.port_name.to_ascii_lowercase(),
        device.vid,
        device.pid,
        device.serial_number.trim().to_ascii_lowercase()
    )
}

fn usb_auto_retry_delay(failure_count: u32) -> Duration {
    let exponent = failure_count.saturating_sub(1).min(6);
    let seconds = USB_AUTO_RETRY_MIN_SECS
        .saturating_mul(1u64 << exponent)
        .min(USB_AUTO_RETRY_MAX_SECS);
    Duration::from_secs(seconds)
}

/// Background thread: auto-connect USB serial on startup and reconnect on disconnect.
fn start_usb_auto_connect(usb_manager: usb_serial::UsbSerialManager, app_handle: tauri::AppHandle) {
    thread::spawn(move || {
        let mut failed_probes: HashMap<String, (u32, Instant)> = HashMap::new();
        // Wait for app to initialize
        thread::sleep(Duration::from_secs(2));
        loop {
            let status = usb_manager.status();
            if !status.connected {
                let devices = usb_manager.scan_devices();
                let present_keys: HashSet<String> =
                    devices.iter().map(usb_auto_probe_key).collect();
                failed_probes.retain(|key, _| present_keys.contains(key));
                for dev in devices {
                    let probe_key = usb_auto_probe_key(&dev);
                    if failed_probes
                        .get(&probe_key)
                        .is_some_and(|(_, retry_at)| Instant::now() < *retry_at)
                    {
                        continue;
                    }
                    let port_name = dev.port_name.clone();
                    eprintln!("[usb-auto] probing {}", port_name);
                    let emitter = app_handle.clone();
                    let result = usb_manager.connect_for_auto(&port_name, move |topic, payload| {
                        handle_incoming_usb_message(&emitter, topic, payload);
                    });
                    match result {
                        Ok(_) => {
                            failed_probes.remove(&probe_key);
                            let verified = usb_manager.status();
                            eprintln!(
                                "[usb-auto] connected to {} runtime={} board={}",
                                port_name, verified.runtime, verified.board_device_id
                            );
                            if let Err(error) =
                                persist_connected_usb_binding(&port_name, &verified.board_device_id)
                            {
                                eprintln!(
                                    "[usb-auto] failed to refresh binding for {}: {}",
                                    verified.board_device_id, error
                                );
                            }
                            if let Err(error) =
                                forward_current_state_after_usb_connect(&usb_manager)
                            {
                                eprintln!(
                                    "[usb-auto] failed to replay bridge state after connect: {}",
                                    error
                                );
                            }
                            break;
                        }
                        Err(e) => {
                            let failure_count = failed_probes
                                .get(&probe_key)
                                .map(|(count, _)| count.saturating_add(1))
                                .unwrap_or(1);
                            let retry_delay = usb_auto_retry_delay(failure_count);
                            failed_probes
                                .insert(probe_key, (failure_count, Instant::now() + retry_delay));
                            eprintln!("[usb-auto] rejected {}: {}", port_name, e);
                            usb_manager.disconnect();
                        }
                    }
                }
            }
            thread::sleep(Duration::from_secs(3));
        }
    });
}

pub fn run() {
    let usb_manager = usb_serial::UsbSerialManager::new();
    if let Err(error) = sync_usb_desktop_device_id(&usb_manager) {
        eprintln!("[usb-identity] desktop id unavailable: {}", error);
    }
    start_usb_host_heartbeat(usb_manager.clone());
    start_usb_state_forwarder(usb_manager.clone());

    let usb_for_auto = usb_manager.clone();

    let app = tauri::Builder::default()
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_http::init())
        .plugin(tauri_plugin_dialog::init())
        .setup(move |app| {
            let usb_transfer_log =
                usb_serial::configure_transfer_logging(&app.path().app_local_data_dir()?)
                    .map_err(std::io::Error::other)?;
            eprintln!(
                "[usb-transfer-log] persistent diagnostics={}",
                usb_transfer_log.display()
            );
            #[cfg(target_os = "macos")]
            volcengine_asr::configure_storage_dir(app.path().app_data_dir()?)
                .map_err(std::io::Error::other)?;
            let handle = app.handle().clone();
            start_usb_auto_connect(usb_for_auto, handle.clone());
            start_p4_ready_migration(handle.clone());
            // Pull up the MQTT bridge so the moment a board comes online we
            // can capture its `hello` and write the device binding without
            // forcing the user through SetupWizard. ensure_bridge_runtime
            // is a no-op when pet-bridge.json doesn't have a saved desktop
            // id.
            thread::spawn(move || {
                thread::sleep(Duration::from_secs(3));
                if let Err(error) = ensure_bridge_runtime(
                    handle,
                    Some(EnsureBridgeRuntimeInput {
                        force_restart: false,
                    }),
                ) {
                    eprintln!("[bridge-auto] start skipped: {}", error);
                }
            });
            // NOTE: voice-service is intentionally NOT auto-started here.
            // The voice worker bakes VOICE_AGENT_ID (the user's selected
            // coding agent) into its env at spawn time, so starting it
            // before the front-end has resolved selectedAgentId would
            // crash the worker on the very first dispatch with
            // "VOICE_AGENT_ID is required". The front-end calls
            // `ensure_voice_runtime` itself once detect_local_agents has
            // settled and a non-empty selection is persisted in
            // pet-bridge.json — see DeviceDashboard.jsx.
            Ok(())
        })
        .manage(usb_manager)
        .invoke_handler(tauri::generate_handler![
            wifi_get_status,
            wifi_connect_ap,
            wifi_restore,
            device_get_pairing_state,
            device_get_wifi_scan,
            device_apply_config,
            device_poll_pairing_result,
            get_or_create_desktop_device_id,
            save_device_binding,
            load_device_bindings,
            remove_device_binding,
            audio_bridge_signal,
            button_config_signal,
            check_codex_accessibility_permission,
            request_codex_accessibility_permission,
            set_p4_session_binding,
            check_device_availability,
            send_test_message,
            dispatch_remote_cli_binding,
            load_bridge_profile,
            save_bridge_profile,
            clear_bridge_profile,
            load_agent_selection,
            save_agent_selection,
            ensure_bridge_runtime,
            stop_bridge_runtime,
            load_device_asr_settings,
            save_device_asr_settings,
            test_device_asr_settings,
            ensure_device_voice_runtime,
            ensure_voice_runtime,
            stop_voice_runtime,
            detect_local_agents,
            download_bytes,
            http_request_text,
            check_ffmpeg_available,
            list_codex_pets,
            import_codex_pet,
            install_codex_community_pet,
            open_external_url,
            usb_scan_devices,
            usb_connect,
            usb_disconnect,
            usb_send_state,
            usb_send_speech,
            usb_send_command,
            usb_audio_capture_control,
            usb_get_status,
            usb_set_screen_page,
            usb_apply_wifi,
            usb_get_diagnostics,
            usb_get_button_config,
            usb_reset_input_config,
            usb_reboot_device,
            usb_get_bundled_firmware_info,
            usb_update_firmware,
            usb_update_bundled_firmware,
            prepare_p4_appearance,
            usb_cancel_appearance_sync,
            usb_sync_appearance,
            resolve_builtin_clawpkg_path,
            install_clawpkg_over_usb,
            install_clawpkg_over_ssh,
            list_device_widgets,
            remove_widget_from_device,
            install_widget_skill,
            list_component_library,
            inspect_clawpkg,
            delete_component_from_library,
            prepare_clawpkg_for_sync,
            release_clawpkg_sync_snapshot,
            purge_clawpkg_sync_cache
        ])
        .build(tauri::generate_context!())
        .expect("error while building pet-manager tauri application");

    app.run(|_app_handle, event| {
        if let RunEvent::Exit | RunEvent::ExitRequested { .. } = event {
            stop_background_runtimes_on_exit();
        }
    });
}

#[cfg(all(target_os = "macos", debug_assertions))]
pub fn run_codex_accessibility_probe(
    session_id: &str,
    session_title: &str,
    session_cwd: &str,
) -> Result<(), String> {
    codex_composer::CodexComposerBridge::debug_probe_visible_composer(
        session_id,
        session_title,
        session_cwd,
    )
}

#[cfg(all(target_os = "macos", debug_assertions))]
pub fn run_claude_accessibility_probe(
    session_id: &str,
    session_title: &str,
    session_cwd: &str,
) -> Result<(), String> {
    codex_composer::CodexComposerBridge::focus_claude_session(
        session_id,
        session_title,
        session_cwd,
    )
}

#[cfg(all(target_os = "macos", debug_assertions))]
pub fn dump_codex_accessibility_tree() -> Result<Vec<String>, String> {
    codex_composer::CodexComposerBridge::debug_dump_accessibility_tree()
}

fn stop_background_runtimes_on_exit() {
    static STOPPED: AtomicBool = AtomicBool::new(false);
    if STOPPED.swap(true, Ordering::SeqCst) {
        return;
    }

    #[cfg(windows)]
    {
        // The job is the fast path; PID cleanup below covers assignment
        // failures and children inherited from an older desktop process.
        terminate_windows_background_job();
    }

    let Ok(config_path) = get_bridge_profile_path() else {
        return;
    };

    if let Ok(runtime_paths) = resolve_bridge_runtime_paths(&config_path) {
        stop_bridge_launch_agent(&runtime_paths);
        if read_pid(&runtime_paths.pid_path).is_some() {
            stop_managed_bridge(&runtime_paths.pid_path);
        } else if probe_bridge_running(DEFAULT_BRIDGE_PORT) {
            stop_process_on_port(DEFAULT_BRIDGE_PORT);
        }
        stop_legacy_bridge_runtime();
    }

    if let Ok(runtime_paths) = resolve_voice_runtime_paths(&config_path) {
        stop_managed_process(&runtime_paths.pid_path);
    }
}

impl BridgeRuntimeStatusResponse {
    fn with_runtime(mut self, running: bool, pid: Option<u32>) -> Self {
        self.running = running;
        self.pid = pid;
        self
    }
}

impl VoiceRuntimeStatusResponse {
    fn with_runtime(mut self, running: bool, pid: Option<u32>) -> Self {
        self.running = running;
        self.pid = pid;
        self
    }
}

fn get_bridge_profile_path() -> Result<PathBuf, String> {
    if let Some(override_path) = env::var_os("PET_CLAW_SHARED_CONFIG_PATH") {
        return Ok(PathBuf::from(override_path));
    }

    Ok(get_home_dir()?
        .join(".claw-pet")
        .join(BRIDGE_PROFILE_FILE_NAME))
}

fn get_home_dir() -> Result<PathBuf, String> {
    env::var_os("HOME")
        .or_else(|| env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .ok_or_else(|| "无法解析当前用户目录，无法写入共享 bridge 配置。".to_string())
}

fn resolve_bridge_runtime_paths(config_path: &Path) -> Result<BridgeRuntimePaths, String> {
    let config_dir = config_path
        .parent()
        .ok_or_else(|| "无法解析共享配置目录。".to_string())?
        .to_path_buf();
    let logs_dir = config_dir.join("logs");
    ensure_private_directory(&config_dir)?;
    ensure_private_directory(&logs_dir)?;

    Ok(BridgeRuntimePaths {
        config_dir: config_dir.clone(),
        log_path: logs_dir.join(BRIDGE_LOG_FILE_NAME),
        pid_path: config_dir.join(BRIDGE_PID_FILE_NAME),
        launch_script_path: config_dir.join(BRIDGE_LAUNCH_SCRIPT_FILE_NAME),
        launch_agent_path: resolve_launch_agent_path()?,
    })
}

fn read_bridge_profile(config_path: &PathBuf) -> Result<Option<BridgeProfileFile>, String> {
    if !config_path.exists() {
        return Ok(None);
    }

    let content = fs::read_to_string(config_path).map_err(|error| error.to_string())?;
    let content = content.trim_start_matches('\u{feff}');
    let profile =
        serde_json::from_str::<BridgeProfileFile>(content).map_err(|error| error.to_string())?;
    Ok(Some(profile))
}

fn normalize_bridge_profile(mut profile: BridgeProfileFile) -> BridgeProfileFile {
    profile.version = 1;
    profile.updated_at = if profile.updated_at > 0 {
        profile.updated_at
    } else if bridge_profile_has_saved_values(&profile) {
        current_timestamp_ms()
    } else {
        0
    };
    profile.desktop_device_id = normalize_topic_segment(profile.desktop_device_id, "");
    profile.mqtt_url = profile.mqtt_url.trim().to_string();
    profile.mqtt_namespace = normalize_topic_segment(profile.mqtt_namespace, DEFAULT_NAMESPACE);
    profile.mqtt_username = profile.mqtt_username.trim().to_string();
    profile.mqtt_password = profile.mqtt_password.trim().to_string();
    profile.pet_channel_id = normalize_pet_channel_id(profile.pet_channel_id);
    // Step 1 (from main): normalize + dedup the enabled_agents list.
    // filter_map drops empties; sort+dedup collapses duplicates.
    profile.enabled_agents = profile
        .enabled_agents
        .into_iter()
        .filter_map(|id| normalize_agent_id(&id))
        .collect();
    profile.enabled_agents.sort();
    profile.enabled_agents.dedup();
    // Step 2 (from feat/agent-session-bus): voice agent promotion logic.
    // We deliberately do NOT derive selected_agent_id from pet_channel_id
    // anymore. pet_channel_id is the legacy board-side MQTT routing slot, and
    // its default ("openclaw") used to silently end up as the user's voice
    // agent — which gives bad first-run UX once the agent-session-bus is in
    // play (the user gets "openclaw 未安装" before they even pick anything).
    // Empty selected_agent_id means "user hasn't explicitly chosen"; the
    // frontend's auto-pick path takes it from there based on what's actually
    // detected on this machine.
    profile.selected_agent_id = normalize_agent_id(&profile.selected_agent_id).unwrap_or_default();
    if !profile.selected_agent_id.is_empty()
        && !profile.enabled_agents.contains(&profile.selected_agent_id)
    {
        profile
            .enabled_agents
            .insert(0, profile.selected_agent_id.clone());
    }
    if profile.selected_agent_id.is_empty() && !profile.enabled_agents.is_empty() {
        profile.selected_agent_id = profile.enabled_agents[0].clone();
    }
    if !profile.selected_agent_id.is_empty() {
        profile.pet_channel_id = selected_agent_to_channel_id(&profile.selected_agent_id);
    }
    profile
}

fn apply_bridge_profile_defaults(mut profile: BridgeProfileFile) -> BridgeProfileFile {
    if profile.desktop_device_id.is_empty() {
        profile.desktop_device_id = DEFAULT_DESKTOP_DEVICE_ID.to_string();
    }

    if profile.mqtt_url.is_empty() {
        profile.mqtt_url = default_mqtt_url();
    }

    if profile.mqtt_namespace.is_empty() {
        profile.mqtt_namespace = DEFAULT_NAMESPACE.to_string();
    }

    if profile.mqtt_username.is_empty() {
        profile.mqtt_username = default_mqtt_username();
    }

    if profile.mqtt_password.is_empty() {
        profile.mqtt_password = default_mqtt_password();
    }

    if profile.pet_channel_id.is_empty() {
        profile.pet_channel_id = DEFAULT_PET_CHANNEL_ID.to_string();
    }

    if profile.enabled_agents.is_empty() && !profile.selected_agent_id.is_empty() {
        profile
            .enabled_agents
            .push(profile.selected_agent_id.clone());
    }

    profile
}

fn bridge_profile_has_saved_values(profile: &BridgeProfileFile) -> bool {
    !profile.desktop_device_id.trim().is_empty()
        || !profile.mqtt_url.trim().is_empty()
        || !profile.mqtt_namespace.trim().is_empty()
        || !profile.mqtt_username.trim().is_empty()
        || !profile.mqtt_password.trim().is_empty()
}

fn build_bridge_profile_response(
    config_path: &Path,
    profile: BridgeProfileFile,
) -> BridgeProfileResponse {
    let topic_base = build_topic_base(&profile);

    BridgeProfileResponse {
        version: profile.version,
        updated_at: profile.updated_at,
        desktop_device_id: profile.desktop_device_id,
        mqtt_url: profile.mqtt_url,
        mqtt_namespace: profile.mqtt_namespace,
        mqtt_username: profile.mqtt_username,
        mqtt_password: profile.mqtt_password,
        pet_channel_id: profile.pet_channel_id,
        enabled_agents: profile.enabled_agents,
        selected_agent_id: profile.selected_agent_id,
        config_path: config_path.display().to_string(),
        topic_base,
    }
}

// NOTE: `normalize_agent_id(&str) -> Option<String>` is defined once at the top
// of this file (used by both voice agent promotion + general agent dispatch).
// HEAD originally redeclared a `String -> String` version + a plural
// `normalize_agent_ids` helper here; both were removed during merge to avoid
// duplicate symbols, with `filter_map(normalize_agent_id)` + sort + dedup
// inlined at the only call site (`normalize_bridge_profile`).

fn selected_agent_to_channel_id(value: &str) -> String {
    match value {
        "claude-code" => "claude".to_string(),
        "codex" => "codex".to_string(),
        "openclaw" => "openclaw".to_string(),
        "mimocode" => "mimocode".to_string(),
        _ => DEFAULT_PET_CHANNEL_ID.to_string(),
    }
}

fn normalize_pet_channel_id(value: String) -> String {
    match value.trim().to_lowercase().as_str() {
        "codex" => "codex".to_string(),
        "claude" => "claude".to_string(),
        "openclaw" => "openclaw".to_string(),
        "mimocode" => "mimocode".to_string(),
        "cursor" => "cursor".to_string(),
        _ => DEFAULT_PET_CHANNEL_ID.to_string(),
    }
}

fn build_bridge_runtime_status(
    profile: &BridgeProfileFile,
    runtime_paths: &BridgeRuntimePaths,
    bridge_assets: Option<&ResolvedBridgeAssets>,
    node_path: Option<&PathBuf>,
    auto_start_installed: bool,
    mode: &str,
    message: String,
) -> BridgeRuntimeStatusResponse {
    BridgeRuntimeStatusResponse {
        configured: !profile.desktop_device_id.is_empty() && !profile.mqtt_url.is_empty(),
        running: false,
        pid: None,
        topic_base: build_topic_base(profile),
        log_path: runtime_paths.log_path.display().to_string(),
        pid_path: runtime_paths.pid_path.display().to_string(),
        launch_script_path: runtime_paths.launch_script_path.display().to_string(),
        launch_agent_path: runtime_paths
            .launch_agent_path
            .as_ref()
            .map(|path| path.display().to_string())
            .unwrap_or_default(),
        auto_start_installed,
        node_path: node_path
            .map(|path| path.display().to_string())
            .unwrap_or_default(),
        bridge_workspace_root: bridge_assets
            .map(|assets| assets.workspace_root.display().to_string())
            .unwrap_or_default(),
        bridge_entry_path: bridge_assets
            .map(|assets| assets.entry_path.display().to_string())
            .unwrap_or_default(),
        message,
        mode: mode.to_string(),
    }
}

fn build_voice_runtime_status(
    profile: &BridgeProfileFile,
    runtime_paths: &VoiceRuntimePaths,
    voice_assets: &ResolvedVoiceServiceAssets,
    mode: &str,
    message: String,
) -> VoiceRuntimeStatusResponse {
    VoiceRuntimeStatusResponse {
        configured: voice_assets.executable_path.exists(),
        running: false,
        pid: None,
        host: DEFAULT_VOICE_SERVICE_HOST.to_string(),
        port: DEFAULT_VOICE_SERVICE_PORT,
        selected_agent_id: profile.selected_agent_id.clone(),
        enabled_agents: profile.enabled_agents.clone(),
        log_path: runtime_paths.log_path.display().to_string(),
        pid_path: runtime_paths.pid_path.display().to_string(),
        launch_script_path: runtime_paths.launch_script_path.display().to_string(),
        executable_path: voice_assets.executable_path.display().to_string(),
        resource_root: voice_assets.resource_root.display().to_string(),
        message,
        mode: mode.to_string(),
    }
}

fn build_topic_base(profile: &BridgeProfileFile) -> String {
    if profile.desktop_device_id.is_empty() {
        String::new()
    } else {
        format!("{}/{}", profile.mqtt_namespace, profile.desktop_device_id)
    }
}

fn resolve_bridge_assets(app_handle: &tauri::AppHandle) -> Result<ResolvedBridgeAssets, String> {
    let mut candidates = Vec::new();

    if let Some(override_root) = env::var_os("PET_MANAGER_BRIDGE_ROOT") {
        candidates.push(PathBuf::from(override_root));
    }

    #[cfg(debug_assertions)]
    {
        let dev_bridge_root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("bridge");
        candidates.push(dev_bridge_root);
    }

    if let Ok(resource_dir) = app_handle.path().resource_dir() {
        candidates.push(resource_dir.join(BRIDGE_RESOURCE_ROOT));
    }

    for root in candidates {
        let workspace_root = root.join(BRIDGE_WORKSPACE_RELATIVE_PATH);
        let entry_path = root.join(BRIDGE_ENTRY_RELATIVE_PATH);
        if workspace_root.exists() && entry_path.exists() {
            return Ok(ResolvedBridgeAssets {
                resource_root: root,
                workspace_root,
                entry_path,
            });
        }
    }

    Err("未找到 bridge 运行资源，Pet Manager 当前无法自动拉起本地 bridge。".to_string())
}

fn resolve_voice_runtime_paths(config_path: &Path) -> Result<VoiceRuntimePaths, String> {
    let config_dir = config_path
        .parent()
        .ok_or_else(|| "无法解析共享配置目录。".to_string())?
        .to_path_buf();
    let logs_dir = config_dir.join("logs");
    ensure_private_directory(&config_dir)?;
    ensure_private_directory(&logs_dir)?;

    Ok(VoiceRuntimePaths {
        log_path: logs_dir.join(VOICE_SERVICE_LOG_FILE_NAME),
        pid_path: config_dir.join(VOICE_SERVICE_PID_FILE_NAME),
        agent_id_path: config_dir.join(VOICE_SERVICE_AGENT_ID_FILE_NAME),
        launch_script_path: config_dir.join(VOICE_SERVICE_LAUNCH_SCRIPT_FILE_NAME),
    })
}

fn resolve_voice_service_assets(
    app_handle: &tauri::AppHandle,
) -> Result<ResolvedVoiceServiceAssets, String> {
    let mut candidates = Vec::new();
    if let Some(override_root) = env::var_os("PET_MANAGER_VOICE_SERVICE_ROOT") {
        candidates.push(PathBuf::from(override_root));
    }
    if let Ok(resource_dir) = app_handle.path().resource_dir() {
        candidates.push(resource_dir.join(VOICE_SERVICE_RESOURCE_ROOT));
    }
    // Debug fallback: use the sibling voice-service-node checkout when present.
    #[cfg(debug_assertions)]
    {
        candidates.push(
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("../../../openclaw-pet/voice-service-node"),
        );
    }

    for root in candidates {
        let entry_path = root.join(VOICE_SERVICE_ENTRY_RELATIVE_PATH);
        if entry_path.is_file() {
            return Ok(ResolvedVoiceServiceAssets {
                resource_root: root,
                executable_path: entry_path,
            });
        }
    }

    Err("未找到 voice-service-node 运行资源，Pet Manager 当前无法自动拉起语音服务。".to_string())
}

fn resolve_node_path(app_handle: &tauri::AppHandle) -> Result<PathBuf, String> {
    let node_name = if cfg!(windows) { "node.exe" } else { "node" };

    // Explicit environment override for a target-compatible Node runtime.
    if let Some(override_path) = env::var_os("PET_MANAGER_NODE_BIN") {
        let path = PathBuf::from(override_path);
        if path.is_file() {
            return Ok(path);
        }
    }

    // Debug builds can use the developer's PATH without copying a runtime.
    #[cfg(debug_assertions)]
    if let Some(system_node) = resolve_path_program(node_name) {
        return Ok(system_node);
    }

    // Bundled node inside app resources.
    if let Ok(resource_dir) = app_handle.path().resource_dir() {
        let bundled = resource_dir.join("bridge/runtime").join(node_name);
        if bundled.is_file() {
            return Ok(bundled);
        }
    }

    // Last-resort user PATH lookup keeps unpackaged developer sessions useful.
    if let Some(path) = find_executable(node_name, &[]) {
        return Ok(PathBuf::from(path));
    }

    Err("未找到可用的 Node.js（安装包 bridge/runtime 或用户 PATH）。".to_string())
}

fn write_voice_launch_script(
    script_path: &Path,
    log_path: &Path,
    voice_assets: &ResolvedVoiceServiceAssets,
    node_path: &Path,
    profile: &BridgeProfileFile,
) -> Result<(), String> {
    let env_exports = build_voice_agent_env_exports(profile);
    let node_modules = voice_assets.resource_root.join("node_modules");
    let script = format!(
        "#!/bin/sh\nset -eu\nunset NODE_OPTIONS\nmkdir -p {logs_dir}\ncd {resource_root}\nexport NODE_PATH={node_modules}${{NODE_PATH:+:$NODE_PATH}}\nexport VOICE_SERVICE_HOST={host}\nexport VOICE_SERVICE_PORT={port}\nexport VOICE_SERVICE_CORS_ORIGINS='*'\n{env_exports}\nexec {node_path} {entry_path} >> {log_path} 2>&1\n",
        logs_dir = shell_quote(
            log_path
                .parent()
                .and_then(|path| path.to_str())
                .unwrap_or("")
        ),
        resource_root = shell_quote(voice_assets.resource_root.to_string_lossy().as_ref()),
        node_modules = shell_quote(node_modules.to_string_lossy().as_ref()),
        host = shell_quote(DEFAULT_VOICE_SERVICE_HOST),
        port = shell_quote(&DEFAULT_VOICE_SERVICE_PORT.to_string()),
        env_exports = env_exports,
        node_path = shell_quote(node_path.to_string_lossy().as_ref()),
        entry_path = shell_quote(voice_assets.executable_path.to_string_lossy().as_ref()),
        log_path = shell_quote(log_path.to_string_lossy().as_ref()),
    );

    write_private_executable(script_path, script.as_bytes())
}

#[cfg(debug_assertions)]
fn resolve_path_program(program: &str) -> Option<PathBuf> {
    env::var_os("PATH").and_then(|paths| {
        env::split_paths(&paths)
            .map(|path| path.join(program))
            .find(|candidate| candidate.is_file())
    })
}

fn enabled_agents_csv(profile: &BridgeProfileFile) -> String {
    profile.enabled_agents.join(",")
}

fn agent_enabled_env(profile: &BridgeProfileFile, id: &str) -> &'static str {
    if profile.enabled_agents.is_empty() || profile.enabled_agents.iter().any(|agent| agent == id) {
        "true"
    } else {
        "false"
    }
}

fn node_dir_for_path(node_path: &Path) -> Option<&Path> {
    node_path
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
}

fn merged_host_path(preferred_dir: Option<&Path>) -> Option<std::ffi::OsString> {
    let mut directories = Vec::<PathBuf>::new();
    if let Some(path) = preferred_dir {
        push_unique_path(&mut directories, path.to_path_buf());
    }

    if let Some(paths) = get_full_shell_path().map(std::ffi::OsString::from) {
        for path in env::split_paths(&paths) {
            push_unique_path(&mut directories, path);
        }
    }
    if let Some(paths) = env::var_os("PATH") {
        for path in env::split_paths(&paths) {
            push_unique_path(&mut directories, path);
        }
    }

    if directories.is_empty() {
        None
    } else {
        env::join_paths(directories).ok()
    }
}

fn write_launch_script(
    script_path: &Path,
    log_path: &Path,
    profile: &BridgeProfileFile,
    bridge_assets: &ResolvedBridgeAssets,
    node_path: &Path,
) -> Result<(), String> {
    let bridge_root_candidates = build_bridge_root_candidates(&bridge_assets.resource_root)
        .into_iter()
        .map(|path| shell_quote(path.to_string_lossy().as_ref()))
        .collect::<Vec<_>>()
        .join(" ");
    let search_path = merged_host_path(node_dir_for_path(node_path))
        .map(|path| shell_quote(path.to_string_lossy().as_ref()))
        .unwrap_or_else(|| "''".to_string());
    let codex_cli_export = find_codex_executable(get_home_dir().ok().as_deref())
        .map(|path| format!("export CODEX_CLI_PATH={}\n", shell_quote(&path)))
        .unwrap_or_default();
    let bridge_port_export = format!(
        "{}\nexport CLAWD_BRIDGE_STRICT_PORT=1",
        shell_quote(&DEFAULT_BRIDGE_PORT.to_string())
    );
    let script = format!(
        "#!/bin/sh\nset -eu\nunset NODE_OPTIONS\nmkdir -p {logs_dir}\nBRIDGE_ROOT=''\nfor candidate in {bridge_root_candidates}; do\n  if [ -f \"$candidate/{entry_relative_path}\" ]; then\n    BRIDGE_ROOT=\"$candidate\"\n    break\n  fi\ndone\nif [ -z \"$BRIDGE_ROOT\" ]; then\n  printf '%s\\n' 'bridge resources not found in any detected local path' >> {log_path}\n  exit 1\nfi\ncd \"$BRIDGE_ROOT/{workspace_relative_path}\"\nexport PATH={search_path}\nexport NODE_PATH=\"$BRIDGE_ROOT/{workspace_relative_path}/node_modules${{NODE_PATH:+:$NODE_PATH}}\"\n{codex_cli_export}export MQTT_URL={mqtt_url}\nexport MQTT_USERNAME={mqtt_username}\nexport MQTT_PASSWORD={mqtt_password}\nexport STATUS_NAMESPACE={namespace}\nexport STATUS_DEVICE_ID={device_id}\nexport STATUS_BRIDGE_LOCAL_STATE_DIR={local_state_dir}\nexport CLAWD_BRIDGE_PORT={bridge_port}\nexport AGENT_BUS_PORT={agent_bus_port}\nexport CLAWD_ENABLED_AGENTS={enabled_agents}\nexport CLAWD_SELECTED_AGENT_ID={selected_agent_id}\nexport CLAWD_ENABLE_CLAUDE_LOG_MONITOR={claude_enabled}\nexport CLAWD_SYNC_HOOKS={claude_enabled}\nexport CLAWD_ENABLE_CODEX_MONITOR={codex_enabled}\nexport CLAWD_CODEX_SESSION_DIR={codex_session_dir}\nexport OPENCLAW_ENABLE={openclaw_enabled}\nexport CLAWD_ENABLE_MIMOCODE={mimocode_enabled}\nexec {node_path} \"$BRIDGE_ROOT/{entry_relative_path}\" >> {log_path} 2>&1\n",
        logs_dir = shell_quote(
            log_path
                .parent()
                .and_then(|path| path.to_str())
                .unwrap_or("")
        ),
        bridge_root_candidates = bridge_root_candidates,
        workspace_relative_path = BRIDGE_WORKSPACE_RELATIVE_PATH,
        entry_relative_path = BRIDGE_ENTRY_RELATIVE_PATH,
        search_path = search_path,
        codex_cli_export = codex_cli_export,
        mqtt_url = shell_quote(&profile.mqtt_url),
        mqtt_username = shell_quote(&profile.mqtt_username),
        mqtt_password = shell_quote(&profile.mqtt_password),
        namespace = shell_quote(&profile.mqtt_namespace),
        device_id = shell_quote(&profile.desktop_device_id),
        local_state_dir = shell_quote(
            env::var("STATUS_BRIDGE_LOCAL_STATE_DIR")
                .unwrap_or_default()
                .as_str()
        ),
        bridge_port = bridge_port_export,
        agent_bus_port = shell_quote(&DEFAULT_AGENT_BUS_PORT.to_string()),
        enabled_agents = shell_quote(&enabled_agents_csv(profile)),
        selected_agent_id = shell_quote(&profile.selected_agent_id),
        claude_enabled = shell_quote(agent_enabled_env(profile, "claude-code")),
        codex_enabled = shell_quote(agent_enabled_env(profile, "codex")),
        codex_session_dir = shell_quote(
            env::var("CLAWD_CODEX_SESSION_DIR")
                .unwrap_or_default()
                .as_str()
        ),
        openclaw_enabled = shell_quote(agent_enabled_env(profile, "openclaw")),
        mimocode_enabled = shell_quote(agent_enabled_env(profile, "mimocode")),
        node_path = shell_quote(node_path.to_string_lossy().as_ref()),
        log_path = shell_quote(log_path.to_string_lossy().as_ref()),
    );

    write_private_executable(script_path, script.as_bytes())
}

fn install_bridge_autostart(
    runtime_paths: &BridgeRuntimePaths,
    profile: &BridgeProfileFile,
    bridge_assets: &ResolvedBridgeAssets,
    node_path: &Path,
) -> Result<bool, String> {
    #[cfg(target_os = "macos")]
    {
        if let Some(agent_path) = runtime_paths.launch_agent_path.as_ref() {
            if let Some(parent) = agent_path.parent() {
                fs::create_dir_all(parent).map_err(|error| error.to_string())?;
            }

            let plist = format!(
                r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>{label}</string>
  <key>ProgramArguments</key>
  <array>
    <string>{script_path}</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>WorkingDirectory</key>
  <string>{working_dir}</string>
  <key>StandardOutPath</key>
  <string>{log_path}</string>
  <key>StandardErrorPath</key>
  <string>{log_path}</string>
</dict>
</plist>
"#,
                label = BRIDGE_LAUNCH_AGENT_LABEL,
                script_path = runtime_paths.launch_script_path.display(),
                working_dir = runtime_paths.config_dir.display(),
                log_path = runtime_paths.log_path.display(),
            );

            fs::write(agent_path, plist).map_err(|error| error.to_string())?;
            return Ok(true);
        }
    }

    #[cfg(target_os = "windows")]
    {
        if let Some(agent_path) = runtime_paths.launch_agent_path.as_ref() {
            if let Some(parent) = agent_path.parent() {
                fs::create_dir_all(parent).map_err(|error| error.to_string())?;
            }

            let launcher_path = windows_bridge_launch_script_path(runtime_paths);
            let launcher = build_windows_bridge_launcher_script(
                runtime_paths,
                profile,
                bridge_assets,
                node_path,
            );
            write_powershell_script_utf8_bom(&launcher_path, &launcher)?;

            let startup_script = build_windows_bridge_startup_script(&launcher_path);
            fs::write(agent_path, startup_script).map_err(|error| error.to_string())?;
            return Ok(true);
        }
    }

    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    {
        let _ = (runtime_paths, profile, bridge_assets, node_path);
    }

    Ok(false)
}

#[cfg(target_os = "windows")]
fn windows_bridge_launch_script_path(runtime_paths: &BridgeRuntimePaths) -> PathBuf {
    runtime_paths
        .config_dir
        .join(BRIDGE_WINDOWS_LAUNCH_SCRIPT_FILE_NAME)
}

#[cfg(target_os = "windows")]
fn build_windows_bridge_startup_script(launcher_path: &Path) -> String {
    format!(
        "@echo off\r\n\
start \"\" /min powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File {}\r\n",
        cmd_quote_path(launcher_path),
    )
}

#[cfg(target_os = "windows")]
fn build_windows_bridge_launcher_script(
    runtime_paths: &BridgeRuntimePaths,
    profile: &BridgeProfileFile,
    bridge_assets: &ResolvedBridgeAssets,
    node_path: &Path,
) -> String {
    let node_path = child_process_path(node_path);
    let workspace_root = child_process_path(&bridge_assets.workspace_root);
    let node_modules = workspace_root.join("node_modules");
    let search_path = merged_host_path(node_dir_for_path(&node_path)).unwrap_or_default();
    let error_log_path = runtime_paths.log_path.with_extension("error.log");
    format!(
        "$ErrorActionPreference = 'Stop'\r\n\
New-Item -ItemType Directory -Force -Path {logs_dir} | Out-Null\r\n\
$env:NODE_OPTIONS = $null\r\n\
$env:PATH = {search_path}\r\n\
$env:NODE_PATH = {node_modules}\r\n\
$env:MQTT_URL = {mqtt_url}\r\n\
$env:MQTT_USERNAME = {mqtt_username}\r\n\
$env:MQTT_PASSWORD = {mqtt_password}\r\n\
$env:STATUS_NAMESPACE = {namespace}\r\n\
$env:STATUS_DEVICE_ID = {device_id}\r\n\
$env:STATUS_BRIDGE_LOCAL_STATE_DIR = {local_state_dir}\r\n\
$env:CLAWD_BRIDGE_PORT = {bridge_port}\r\n\
$env:CLAWD_BRIDGE_STRICT_PORT = '1'\r\n\
$env:AGENT_BUS_PORT = {agent_bus_port}\r\n\
$env:CLAWD_ENABLED_AGENTS = {enabled_agents}\r\n\
$env:CLAWD_SELECTED_AGENT_ID = {selected_agent_id}\r\n\
$env:CLAWD_ENABLE_CLAUDE_LOG_MONITOR = {claude_enabled}\r\n\
$env:CLAWD_SYNC_HOOKS = {claude_enabled}\r\n\
$env:CLAWD_ENABLE_CODEX_MONITOR = {codex_enabled}\r\n\
$env:CLAWD_CODEX_SESSION_DIR = {codex_session_dir}\r\n\
$env:OPENCLAW_ENABLE = {openclaw_enabled}\r\n\
$env:CLAWD_ENABLE_MIMOCODE = {mimocode_enabled}\r\n\
Set-Location -LiteralPath {working_dir}\r\n\
$nodePath = {node_path}\r\n\
$entryPath = {entry_path}\r\n\
$entryArg = '\"' + $entryPath + '\"'\r\n\
$process = Start-Process -PassThru -WindowStyle Hidden -FilePath $nodePath -ArgumentList $entryArg -WorkingDirectory {working_dir} -RedirectStandardOutput {log_path} -RedirectStandardError {error_log_path}\r\n\
Set-Content -LiteralPath {pid_path} -Value $process.Id -Encoding ascii\r\n",
        logs_dir = powershell_quote(
            runtime_paths
                .log_path
                .parent()
                .unwrap_or(runtime_paths.config_dir.as_path())
                .to_string_lossy()
                .as_ref()
        ),
        search_path = powershell_quote(search_path.to_string_lossy().as_ref()),
        node_modules = powershell_path_quote(&node_modules),
        mqtt_url = powershell_quote(&profile.mqtt_url),
        mqtt_username = powershell_quote(&profile.mqtt_username),
        mqtt_password = powershell_quote(&profile.mqtt_password),
        namespace = powershell_quote(&profile.mqtt_namespace),
        device_id = powershell_quote(&profile.desktop_device_id),
        local_state_dir = powershell_quote(
            env::var("STATUS_BRIDGE_LOCAL_STATE_DIR")
                .unwrap_or_default()
                .as_str(),
        ),
        bridge_port = powershell_quote(&DEFAULT_BRIDGE_PORT.to_string()),
        agent_bus_port = powershell_quote(&DEFAULT_AGENT_BUS_PORT.to_string()),
        enabled_agents = powershell_quote(&enabled_agents_csv(profile)),
        selected_agent_id = powershell_quote(&profile.selected_agent_id),
        claude_enabled = powershell_quote(agent_enabled_env(profile, "claude-code")),
        codex_enabled = powershell_quote(agent_enabled_env(profile, "codex")),
        codex_session_dir = powershell_quote(
            env::var("CLAWD_CODEX_SESSION_DIR")
                .unwrap_or_default()
                .as_str(),
        ),
        openclaw_enabled = powershell_quote(agent_enabled_env(profile, "openclaw")),
        mimocode_enabled = powershell_quote(agent_enabled_env(profile, "mimocode")),
        working_dir = powershell_path_quote(&bridge_assets.workspace_root),
        node_path = powershell_path_quote(&node_path),
        entry_path = powershell_path_quote(&bridge_assets.entry_path),
        log_path = powershell_path_quote(&runtime_paths.log_path),
        error_log_path = powershell_path_quote(&error_log_path),
        pid_path = powershell_path_quote(&runtime_paths.pid_path),
    )
}

#[cfg(windows)]
static WINDOWS_BACKGROUND_JOB: OnceLock<Result<usize, String>> = OnceLock::new();

#[cfg(windows)]
fn windows_background_job_handle() -> Result<usize, String> {
    WINDOWS_BACKGROUND_JOB
        .get_or_init(|| unsafe {
            use windows_sys::Win32::Foundation::CloseHandle;
            use windows_sys::Win32::System::JobObjects::{
                CreateJobObjectW, JobObjectExtendedLimitInformation, SetInformationJobObject,
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
            };

            let handle = CreateJobObjectW(std::ptr::null(), std::ptr::null());
            if handle.is_null() {
                return Err(format!(
                    "failed to create Windows background job: {}",
                    std::io::Error::last_os_error()
                ));
            }

            let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            let configured = SetInformationJobObject(
                handle,
                JobObjectExtendedLimitInformation,
                (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            );
            if configured == 0 {
                let error = std::io::Error::last_os_error();
                CloseHandle(handle);
                return Err(format!(
                    "failed to configure Windows background job: {error}"
                ));
            }

            Ok(handle as usize)
        })
        .clone()
}

#[cfg(windows)]
fn assign_child_to_windows_background_job(child: &std::process::Child) -> Result<(), String> {
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::Foundation::HANDLE;
    use windows_sys::Win32::System::JobObjects::AssignProcessToJobObject;

    let job = windows_background_job_handle()? as HANDLE;
    let process = child.as_raw_handle() as HANDLE;
    if unsafe { AssignProcessToJobObject(job, process) } == 0 {
        return Err(format!(
            "failed to assign child {} to Windows background job: {}",
            child.id(),
            std::io::Error::last_os_error()
        ));
    }
    Ok(())
}

#[cfg(windows)]
fn assign_pid_to_windows_background_job(pid: u32) -> Result<(), String> {
    use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
    use windows_sys::Win32::System::JobObjects::AssignProcessToJobObject;
    use windows_sys::Win32::System::Threading::{
        OpenProcess, PROCESS_SET_QUOTA, PROCESS_TERMINATE,
    };

    let job = windows_background_job_handle()? as HANDLE;
    let process = unsafe { OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, 0, pid) };
    if process.is_null() {
        return Err(format!(
            "failed to open child {pid} for Windows background job: {}",
            std::io::Error::last_os_error()
        ));
    }
    let assigned = unsafe { AssignProcessToJobObject(job, process) };
    let error = if assigned == 0 {
        Some(std::io::Error::last_os_error())
    } else {
        None
    };
    unsafe {
        CloseHandle(process);
    }
    match error {
        Some(error) => Err(format!(
            "failed to assign child {pid} to Windows background job: {error}"
        )),
        None => Ok(()),
    }
}

#[cfg(windows)]
fn terminate_windows_background_job() {
    use windows_sys::Win32::Foundation::HANDLE;
    use windows_sys::Win32::System::JobObjects::TerminateJobObject;

    let Some(Ok(handle)) = WINDOWS_BACKGROUND_JOB.get() else {
        return;
    };
    if unsafe { TerminateJobObject(*handle as HANDLE, 0) } == 0 {
        eprintln!(
            "[runtime-exit] failed to terminate Windows background job: {}",
            std::io::Error::last_os_error()
        );
    }
}

fn start_bridge_process(
    script_path: &Path,
    log_path: &Path,
    pid_path: &Path,
) -> Result<u32, String> {
    #[cfg(unix)]
    {
        start_bridge_via_sh(script_path, log_path, pid_path)
    }
    #[cfg(windows)]
    {
        let _ = log_path;
        let _ = fs::remove_file(pid_path);
        let status = command_for_host("powershell.exe")
            .env("PET_MANAGER_PARENT_PID", std::process::id().to_string())
            .args([
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-WindowStyle",
                "Hidden",
                "-File",
            ])
            .arg(child_process_path(script_path))
            .status()
            .map_err(|error| format!("PowerShell bridge 备用启动失败: {error}"))?;
        if !status.success() {
            return Err(format!(
                "PowerShell bridge 备用启动退出码异常: {:?}",
                status.code()
            ));
        }
        for _ in 0..20 {
            if let Some(pid) = read_pid(pid_path) {
                if let Err(error) = assign_pid_to_windows_background_job(pid) {
                    eprintln!("[bridge-runtime] {error}");
                }
                return Ok(pid);
            }
            thread::sleep(Duration::from_millis(50));
        }
        Err("PowerShell bridge 备用启动未写入进程号".to_string())
    }
}

fn start_voice_service_direct(
    node_path: &Path,
    voice_assets: &ResolvedVoiceServiceAssets,
    profile: &BridgeProfileFile,
    log_path: &Path,
    pid_path: &Path,
) -> Result<u32, String> {
    if let Some(parent) = log_path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }

    let stdout = open_private_append_file(log_path)?;
    let stderr = stdout.try_clone().map_err(|error| error.to_string())?;

    // executable_path here is the entry .mjs (resolve_voice_service_assets
    // populated it that way). We invoke the bundled node against it.
    let node_path = child_process_path(node_path);
    let resource_root = child_process_path(&voice_assets.resource_root);
    let executable_path = child_process_path(&voice_assets.executable_path);
    let node_modules = resource_root.join("node_modules");
    let path_separator = if cfg!(windows) { ";" } else { ":" };
    let node_path_env = match env::var_os("NODE_PATH") {
        Some(existing) => {
            let mut combined = node_modules.as_os_str().to_owned();
            combined.push(path_separator);
            combined.push(&existing);
            combined
        }
        None => node_modules.into_os_string(),
    };

    let mut command = command_for_host(&node_path);
    command.arg(&executable_path);
    command.current_dir(&resource_root);
    command.env_remove("NODE_OPTIONS");
    command.env("NODE_PATH", node_path_env);
    command.env("VOICE_SERVICE_HOST", DEFAULT_VOICE_SERVICE_HOST);
    command.env("VOICE_SERVICE_PORT", DEFAULT_VOICE_SERVICE_PORT.to_string());
    command.env("VOICE_SERVICE_CORS_ORIGINS", "*");
    for (key, value) in build_voice_agent_env_pairs(profile) {
        command.env(key, value);
    }
    command.stdin(Stdio::null());
    command.stdout(Stdio::from(stdout));
    command.stderr(Stdio::from(stderr));
    #[cfg(unix)]
    {
        command.process_group(0);
    }

    let child = command.spawn().map_err(|error| error.to_string())?;
    #[cfg(windows)]
    if let Err(error) = assign_child_to_windows_background_job(&child) {
        eprintln!("[voice-runtime] {error}");
    }
    let pid = child.id();
    write_private_file(pid_path, format!("{pid}\n").as_bytes())?;
    Ok(pid)
}

/// Spawn node directly with the correct env vars, bypassing the shell script.
/// This avoids macOS Permission denied errors when the Tauri app tries to
/// execute a user-owned external launch script.
fn start_bridge_direct(
    node_path: &Path,
    bridge_assets: &ResolvedBridgeAssets,
    profile: &BridgeProfileFile,
    log_path: &Path,
    pid_path: &Path,
) -> Result<u32, String> {
    if let Some(parent) = log_path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }

    let stdout = open_private_append_file(log_path)?;
    let stderr = stdout.try_clone().map_err(|error| error.to_string())?;

    let node_path = child_process_path(node_path);
    let workspace_root = child_process_path(&bridge_assets.workspace_root);
    let entry_path = child_process_path(&bridge_assets.entry_path);
    let node_modules = workspace_root.join("node_modules");
    let path_separator = if cfg!(windows) { ";" } else { ":" };
    let node_path_env = match env::var_os("NODE_PATH") {
        Some(existing) => {
            let mut combined = node_modules.as_os_str().to_owned();
            combined.push(path_separator);
            combined.push(&existing);
            combined
        }
        None => node_modules.into_os_string(),
    };

    let mut command = command_for_host(&node_path);
    command.arg(&entry_path);
    command.current_dir(&workspace_root);
    command.env("PET_MANAGER_PARENT_PID", std::process::id().to_string());
    command.env_remove("NODE_OPTIONS");
    if let Some(path_env) = merged_host_path(node_dir_for_path(&node_path)) {
        command.env("PATH", path_env);
    }
    command.env("NODE_PATH", node_path_env);
    if let Some(codex_cli_path) = find_codex_executable(get_home_dir().ok().as_deref()) {
        command.env("CODEX_CLI_PATH", codex_cli_path);
    }
    command.env("MQTT_URL", &profile.mqtt_url);
    command.env("MQTT_USERNAME", &profile.mqtt_username);
    command.env("MQTT_PASSWORD", &profile.mqtt_password);
    command.env("STATUS_NAMESPACE", &profile.mqtt_namespace);
    command.env("STATUS_DEVICE_ID", &profile.desktop_device_id);
    command.env("CLAWD_BRIDGE_PORT", DEFAULT_BRIDGE_PORT.to_string());
    command.env("CLAWD_BRIDGE_STRICT_PORT", "1");
    command.env("AGENT_BUS_PORT", DEFAULT_AGENT_BUS_PORT.to_string());
    command.env("CLAWD_ENABLED_AGENTS", enabled_agents_csv(profile));
    command.env("CLAWD_SELECTED_AGENT_ID", &profile.selected_agent_id);

    // Map enabled_agents to per-tool env vars.  Empty vec = all enabled
    // (backward compat with profiles saved before this field existed).
    let has_filter = !profile.enabled_agents.is_empty();
    let agent_on = |id: &str| !has_filter || profile.enabled_agents.iter().any(|a| a == id);
    command.env(
        "CLAWD_ENABLE_CLAUDE_LOG_MONITOR",
        if agent_on("claude-code") {
            "true"
        } else {
            "false"
        },
    );
    command.env(
        "CLAWD_SYNC_HOOKS",
        if agent_on("claude-code") {
            "true"
        } else {
            "false"
        },
    );
    command.env(
        "CLAWD_ENABLE_CODEX_MONITOR",
        if agent_on("codex") { "true" } else { "false" },
    );
    command.env(
        "OPENCLAW_ENABLE",
        if agent_on("openclaw") {
            "true"
        } else {
            "false"
        },
    );
    command.env(
        "CLAWD_ENABLE_MIMOCODE",
        if agent_on("mimocode") {
            "true"
        } else {
            "false"
        },
    );
    command.stdin(Stdio::null());
    command.stdout(Stdio::from(stdout));
    command.stderr(Stdio::from(stderr));
    #[cfg(unix)]
    {
        command.process_group(0);
    }
    let child = command.spawn().map_err(|error| error.to_string())?;
    #[cfg(windows)]
    if let Err(error) = assign_child_to_windows_background_job(&child) {
        eprintln!("[bridge-runtime] {error}");
    }
    let pid = child.id();
    write_private_file(pid_path, format!("{pid}\n").as_bytes())?;
    Ok(pid)
}

fn launch_bridge_runtime(
    node_path: &Path,
    bridge_assets: &ResolvedBridgeAssets,
    profile: &BridgeProfileFile,
    runtime_paths: &BridgeRuntimePaths,
) -> Result<(bool, Option<u32>), String> {
    let direct_failure = match start_bridge_direct(
        node_path,
        bridge_assets,
        profile,
        &runtime_paths.log_path,
        &runtime_paths.pid_path,
    ) {
        Ok(pid) => {
            if wait_for_bridge_ready(DEFAULT_BRIDGE_PORT, 36, 200) {
                return Ok((true, Some(pid)));
            }
            let process_state = if process_exists(pid) {
                "仍在运行但端口未就绪"
            } else {
                "已提前退出"
            };
            let error = format!("直接启动进程 {pid} {process_state}");
            eprintln!("[bridge-runtime] {error}; trying managed launcher");
            stop_managed_bridge(&runtime_paths.pid_path);
            error
        }
        Err(error) => {
            let error = format!("直接启动失败: {error}");
            eprintln!("[bridge-runtime] {error}; trying managed launcher");
            error
        }
    };

    #[cfg(target_os = "windows")]
    let fallback_script_path = windows_bridge_launch_script_path(runtime_paths);
    #[cfg(not(target_os = "windows"))]
    let fallback_script_path = runtime_paths.launch_script_path.clone();

    let fallback_pid = start_bridge_process(
        &fallback_script_path,
        &runtime_paths.log_path,
        &runtime_paths.pid_path,
    )
    .map_err(|error| format!("{direct_failure}；备用启动失败: {error}"))?;
    if wait_for_bridge_ready(DEFAULT_BRIDGE_PORT, 36, 200) {
        return Ok((true, Some(fallback_pid)));
    }

    let fallback_state = if process_exists(fallback_pid) {
        "仍在运行但端口未就绪"
    } else {
        "已提前退出"
    };
    stop_managed_bridge(&runtime_paths.pid_path);
    eprintln!(
        "[bridge-runtime] fallback process {} {}; log={}",
        fallback_pid,
        fallback_state,
        runtime_paths.log_path.display()
    );
    Ok((false, None))
}

#[cfg(unix)]
fn start_bridge_via_sh(
    script_path: &Path,
    log_path: &Path,
    pid_path: &Path,
) -> Result<u32, String> {
    if let Some(parent) = log_path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }

    let stdout = open_private_append_file(log_path)?;
    let stderr = stdout.try_clone().map_err(|error| error.to_string())?;

    let mut command = command_for_host("sh");
    command.arg(script_path);
    command.env("PET_MANAGER_PARENT_PID", std::process::id().to_string());
    command.env_remove("NODE_OPTIONS");
    command.stdin(Stdio::null());
    command.stdout(Stdio::from(stdout));
    command.stderr(Stdio::from(stderr));
    #[cfg(unix)]
    {
        command.process_group(0);
    }

    let child = command.spawn().map_err(|error| error.to_string())?;
    let pid = child.id();
    write_private_file(pid_path, format!("{pid}\n").as_bytes())?;
    Ok(pid)
}

fn build_voice_agent_env_pairs(profile: &BridgeProfileFile) -> Vec<(&'static str, String)> {
    // The voice service no longer hosts an LLM provider — turns are routed
    // through agent-session-bus (started by the bridge sidecar on
    // VOICE_BUS_URL, default http://127.0.0.1:8181) which dispatches them
    // into the user's currently selected coding agent. The legacy
    // LOCAL_AGENT_* env vars (BACKEND / BASE_URL / MODEL) are intentionally
    // gone — see docs/voice-architecture.md.

    let mut pairs = vec![
        // Forwarded so the board (over MQTT) and the voice worker can each
        // tell which agent the user picked. Voice consumes this as
        // VOICE_AGENT_ID; the original name is kept for back-compat with
        // existing pet-manager bridge consumers.
        (
            "LOCAL_AGENT_SELECTED_AGENT_ID",
            profile.selected_agent_id.clone(),
        ),
        ("VOICE_AGENT_ID", profile.selected_agent_id.clone()),
        (
            "PET_MANAGER_ENABLED_AGENT_IDS",
            profile.enabled_agents.join(","),
        ),
        // The bus speaks HTTP+SSE on the bridge sidecar's port. The bridge
        // sidecar binds AGENT_BUS_PORT (default 8181) on 127.0.0.1 from the
        // same Node process that already holds the MQTT relay, so this URL
        // is always loopback-only and shares the bridge's lifecycle.
        (
            "VOICE_BUS_URL",
            std::env::var("VOICE_BUS_URL").unwrap_or_else(|_| "http://127.0.0.1:8181".to_string()),
        ),
        // Empty / "auto" means the bus picks the user's most recent session
        // for the selected agent (see resolveActive() — 永远续最近). Pet-
        // manager UI may override this to a specific session id when the
        // user manually picks one from the session dropdown.
        (
            "VOICE_SESSION_ID",
            std::env::var("VOICE_SESSION_ID").unwrap_or_else(|_| "auto".to_string()),
        ),
    ];

    // Audio relay defaults — keep the relay subprocess running alongside the
    // token API so the moment a board ships its first mic UDP packet we already
    // have a livekit participant ready to publish. board_addr=auto means the
    // relay learns the board IP from that first packet.
    pairs.push(("VOICE_SERVICE_AUDIO_RELAY_ENABLED", "1".to_string()));
    pairs.push(("VOICE_SERVICE_AUDIO_RELAY_BOARD_ADDR", "auto".to_string()));
    pairs.push(("VOICE_SERVICE_AUDIO_RELAY_MIC_PORT", "50001".to_string()));

    pairs
}

fn build_voice_agent_env_exports(profile: &BridgeProfileFile) -> String {
    build_voice_agent_env_pairs(profile)
        .into_iter()
        .map(|(key, value)| format!("export {key}={}\n", shell_quote(&value)))
        .collect::<Vec<_>>()
        .join("")
}

/// Kill whatever process is listening on the given port (used to reclaim the
/// bridge port from stale/external processes we don't have a PID file for).
fn stop_process_on_port(port: u16) {
    for pid in find_listening_pids_on_port(port) {
        let _ = stop_process(pid);
    }
}

fn stop_bridge_launch_agent(runtime_paths: &BridgeRuntimePaths) {
    #[cfg(target_os = "macos")]
    {
        let Some(agent_path) = runtime_paths.launch_agent_path.as_ref() else {
            return;
        };
        let uid = command_for_host("id")
            .arg("-u")
            .output()
            .ok()
            .and_then(|output| {
                if output.status.success() {
                    String::from_utf8(output.stdout).ok()
                } else {
                    None
                }
            })
            .map(|value| value.trim().to_string())
            .filter(|value| !value.is_empty());
        let Some(uid) = uid else {
            return;
        };

        let gui_domain = format!("gui/{uid}");
        let gui_service = format!("{gui_domain}/{BRIDGE_LAUNCH_AGENT_LABEL}");
        let _ = command_for_host("launchctl")
            .args(["bootout", gui_service.as_str()])
            .status();
        if let Some(path) = agent_path.to_str() {
            let _ = command_for_host("launchctl")
                .args(["bootout", gui_domain.as_str(), path])
                .status();
        }
    }

    #[cfg(not(target_os = "macos"))]
    {
        let _ = runtime_paths;
    }
}

fn stop_legacy_bridge_runtime() {
    if !probe_bridge_running(LEGACY_BRIDGE_PORT) {
        return;
    }

    let default_pids: HashSet<u32> = find_listening_pids_on_port(DEFAULT_BRIDGE_PORT)
        .into_iter()
        .collect();
    for pid in find_listening_pids_on_port(LEGACY_BRIDGE_PORT) {
        if !default_pids.contains(&pid) {
            let _ = stop_process(pid);
        }
    }
}

fn find_listening_pids_on_port(port: u16) -> Vec<u32> {
    #[cfg(unix)]
    {
        let mut pids = HashSet::new();
        if let Ok(output) = command_for_host("lsof")
            .args(["-ti", &format!(":{port}")])
            .output()
        {
            let stdout = String::from_utf8_lossy(&output.stdout);
            for pid_str in stdout.split_whitespace() {
                if let Ok(pid) = pid_str.parse::<u32>() {
                    pids.insert(pid);
                }
            }
        }
        return pids.into_iter().collect();
    }

    #[cfg(windows)]
    {
        let mut pids = HashSet::new();
        if let Ok(output) = command_for_host("netstat")
            .args(["-ano", "-p", "tcp"])
            .output()
        {
            let stdout = String::from_utf8_lossy(&output.stdout);
            for line in stdout.lines() {
                let columns: Vec<&str> = line.split_whitespace().collect();
                if columns.len() < 5 {
                    continue;
                }
                if !columns[0].eq_ignore_ascii_case("TCP") {
                    continue;
                }
                if !address_matches_port(columns[1], port) {
                    continue;
                }
                let state = columns[3];
                let state_lower = state.to_ascii_lowercase();
                let is_listening = state_lower == "listening" || state == "侦听";
                if !is_listening {
                    continue;
                }
                if let Ok(pid) = columns[4].parse::<u32>() {
                    pids.insert(pid);
                }
            }
        }
        return pids.into_iter().collect();
    }

    #[allow(unreachable_code)]
    Vec::new()
}

#[cfg(windows)]
fn address_matches_port(address: &str, port: u16) -> bool {
    let Some((_, port_text)) = address.rsplit_once(':') else {
        return false;
    };
    port_text.parse::<u16>().ok() == Some(port)
}

fn stop_managed_bridge(pid_path: &Path) {
    if let Some(pid) = read_pid(pid_path) {
        let _ = stop_process(pid);
    }
    let _ = fs::remove_file(pid_path);
}

fn stop_managed_process(pid_path: &Path) {
    if let Some(pid) = read_pid(pid_path) {
        let _ = stop_process(pid);
    }
    let _ = fs::remove_file(pid_path);
}

fn stop_process(pid: u32) -> bool {
    terminate_process_soft(pid);
    if wait_for_process_exit(pid, 12, 120) {
        return true;
    }

    terminate_process_force(pid);
    wait_for_process_exit(pid, 6, 120)
}

fn wait_for_process_exit(pid: u32, attempts: usize, sleep_ms: u64) -> bool {
    for _ in 0..attempts {
        if !process_exists(pid) {
            return true;
        }
        thread::sleep(Duration::from_millis(sleep_ms));
    }
    !process_exists(pid)
}

#[cfg(unix)]
fn terminate_process_soft(pid: u32) {
    let _ = command_for_host("kill").arg(pid.to_string()).status();
}

#[cfg(unix)]
fn terminate_process_force(pid: u32) {
    let _ = command_for_host("kill")
        .arg("-9")
        .arg(pid.to_string())
        .status();
}

#[cfg(windows)]
fn terminate_process_soft(pid: u32) {
    let _ = command_for_host("taskkill")
        .args(["/PID", &pid.to_string(), "/T"])
        .status();
}

#[cfg(windows)]
fn terminate_process_force(pid: u32) {
    let _ = command_for_host("taskkill")
        .args(["/PID", &pid.to_string(), "/T", "/F"])
        .status();
}

fn process_exists(pid: u32) -> bool {
    process_exists_platform(pid)
}

#[cfg(unix)]
fn process_exists_platform(pid: u32) -> bool {
    command_for_host("kill")
        .arg("-0")
        .arg(pid.to_string())
        .status()
        .map(|status| status.success())
        .unwrap_or(false)
}

#[cfg(windows)]
fn process_exists_platform(pid: u32) -> bool {
    let pid_text = pid.to_string();
    command_for_host("tasklist")
        .args(["/FO", "CSV", "/NH", "/FI", &format!("PID eq {pid}")])
        .output()
        .map(|output| {
            let stdout = String::from_utf8_lossy(&output.stdout);
            stdout.lines().any(|line| {
                let trimmed = line.trim();
                if !trimmed.starts_with('\"') {
                    return false;
                }
                let columns: Vec<&str> = trimmed.trim_matches('\"').split("\",\"").collect();
                columns
                    .get(1)
                    .map(|value| value.trim() == pid_text.as_str())
                    .unwrap_or(false)
            })
        })
        .unwrap_or(false)
}

#[cfg(not(any(unix, windows)))]
fn process_exists_platform(_pid: u32) -> bool {
    false
}

#[cfg(not(any(unix, windows)))]
fn terminate_process_soft(_pid: u32) {}

#[cfg(not(any(unix, windows)))]
fn terminate_process_force(_pid: u32) {}

fn read_pid(pid_path: &Path) -> Option<u32> {
    let raw = fs::read_to_string(pid_path).ok()?;
    raw.trim().parse::<u32>().ok()
}

fn read_live_managed_pid(pid_path: &Path) -> Option<u32> {
    let pid = read_pid(pid_path)?;
    if process_exists(pid) {
        Some(pid)
    } else {
        let _ = fs::remove_file(pid_path);
        None
    }
}

fn fetch_bridge_agent_status() -> Result<Option<serde_json::Value>, String> {
    let url = format!("http://127.0.0.1:{DEFAULT_AGENT_BUS_PORT}/agent/status");
    let response = lan_http_client(Duration::from_millis(1200))?
        .get(url)
        .send()
        .map_err(|error| error.to_string())?;
    if !response.status().is_success() {
        return Ok(None);
    }
    let text = response.text().map_err(|error| error.to_string())?;
    serde_json::from_str::<serde_json::Value>(&text)
        .map(Some)
        .map_err(|error| error.to_string())
}

fn bridge_agent_status_needs_restart(
    profile: &BridgeProfileFile,
    status: &serde_json::Value,
) -> bool {
    let selected = normalize_agent_id(&profile.selected_agent_id)
        .or_else(|| {
            profile
                .enabled_agents
                .iter()
                .find_map(|agent| normalize_agent_id(agent))
        })
        .unwrap_or_default();
    if selected.is_empty() {
        return false;
    }

    let adapters = status
        .get("adapters")
        .or_else(|| status.get("agents"))
        .and_then(|value| value.as_array());
    let Some(adapters) = adapters else {
        return false;
    };

    adapters.iter().any(|adapter| {
        let agent_id = adapter
            .get("agentId")
            .and_then(|value| value.as_str())
            .and_then(normalize_agent_id)
            .unwrap_or_default();
        if agent_id != selected {
            return false;
        }
        if adapter
            .get("ready")
            .and_then(|value| value.as_bool())
            .unwrap_or(false)
        {
            return false;
        }
        let reason = adapter
            .get("reason")
            .and_then(|value| value.as_str())
            .unwrap_or_default();
        bridge_adapter_reason_is_recoverable(&agent_id, reason)
    })
}

fn bridge_adapter_reason_is_recoverable(agent_id: &str, reason: &str) -> bool {
    let reason = reason.to_ascii_lowercase();
    match agent_id {
        "codex" => {
            (reason.contains("codex cli")
                && (reason.contains("未找到") || reason.contains("not found")))
                || reason.contains("codex --version")
                || (reason.contains("node") && reason.contains("no such file"))
                || reason.contains("env: node")
        }
        _ => false,
    }
}

fn probe_bridge_running(port: u16) -> bool {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let mut stream = match TcpStream::connect_timeout(&address, Duration::from_millis(500)) {
        Ok(stream) => stream,
        Err(_) => return false,
    };

    let _ = stream.set_read_timeout(Some(Duration::from_millis(500)));
    let _ = stream.set_write_timeout(Some(Duration::from_millis(500)));
    let request =
        format!("GET /state HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n");
    if stream.write_all(request.as_bytes()).is_err() {
        return false;
    }

    let mut buffer = [0u8; 4096];
    let mut response = String::new();

    loop {
        match stream.read(&mut buffer) {
            Ok(0) => break,
            Ok(read) => {
                response.push_str(&String::from_utf8_lossy(&buffer[..read]));
                if response.contains("clawd-status-bridge") {
                    return true;
                }
            }
            Err(_) => break,
        }
    }

    false
}

fn probe_agent_bus_running(port: u16) -> bool {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let mut stream = match TcpStream::connect_timeout(&address, Duration::from_millis(500)) {
        Ok(stream) => stream,
        Err(_) => return false,
    };

    let _ = stream.set_read_timeout(Some(Duration::from_millis(500)));
    let _ = stream.set_write_timeout(Some(Duration::from_millis(500)));
    let request = format!(
        "GET /agent/health HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n"
    );
    if stream.write_all(request.as_bytes()).is_err() {
        return false;
    }

    let mut buffer = [0u8; 2048];
    let mut response = String::new();
    loop {
        match stream.read(&mut buffer) {
            Ok(0) => break,
            Ok(read) => {
                response.push_str(&String::from_utf8_lossy(&buffer[..read]));
                if response.contains("200 OK")
                    && response.contains("\"ok\":true")
                    && response.contains("\"adapters\"")
                {
                    return true;
                }
            }
            Err(_) => break,
        }
    }

    false
}

fn probe_voice_service_running(port: u16) -> bool {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let mut stream = match TcpStream::connect_timeout(&address, Duration::from_millis(500)) {
        Ok(stream) => stream,
        Err(_) => return false,
    };

    let _ = stream.set_read_timeout(Some(Duration::from_millis(500)));
    let _ = stream.set_write_timeout(Some(Duration::from_millis(500)));
    // PetAgent LiveKit Agent SDK exposes a tiny HTTP API: GET /healthz returns
    // `{"status":"ok"}`, POST /rtc/token mints a participant token. We probe
    // /healthz because it's free of side effects and we can match on the body.
    let request =
        format!("GET /healthz HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n");
    if stream.write_all(request.as_bytes()).is_err() {
        return false;
    }

    let mut buffer = [0u8; 2048];
    let mut response = String::new();
    loop {
        match stream.read(&mut buffer) {
            Ok(0) => break,
            Ok(read) => {
                response.push_str(&String::from_utf8_lossy(&buffer[..read]));
                if response.contains("200 OK") && response.contains("\"status\":\"ok\"") {
                    return true;
                }
            }
            Err(_) => break,
        }
    }

    false
}

fn wait_for_bridge_ready(port: u16, attempts: usize, sleep_ms: u64) -> bool {
    for _ in 0..attempts {
        if probe_bridge_running(port) && probe_agent_bus_running(DEFAULT_AGENT_BUS_PORT) {
            return true;
        }
        thread::sleep(Duration::from_millis(sleep_ms));
    }
    false
}

fn wait_for_voice_service_ready(port: u16, attempts: usize, sleep_ms: u64) -> bool {
    for _ in 0..attempts {
        if probe_voice_service_running(port) {
            return true;
        }
        thread::sleep(Duration::from_millis(sleep_ms));
    }
    false
}

fn build_bridge_root_candidates(current_root: &Path) -> Vec<PathBuf> {
    let mut candidates = Vec::new();

    if let Some(override_root) = env::var_os("PET_MANAGER_BRIDGE_ROOT") {
        push_unique_path(&mut candidates, PathBuf::from(override_root));
    }

    push_unique_path(&mut candidates, current_root.to_path_buf());

    candidates
}

fn push_unique_path(paths: &mut Vec<PathBuf>, candidate: PathBuf) {
    if !paths.iter().any(|path| path == &candidate) {
        paths.push(candidate);
    }
}

fn resolve_launch_agent_path() -> Result<Option<PathBuf>, String> {
    #[cfg(target_os = "macos")]
    {
        Ok(Some(
            get_home_dir()?
                .join("Library")
                .join("LaunchAgents")
                .join(format!("{BRIDGE_LAUNCH_AGENT_LABEL}.plist")),
        ))
    }

    #[cfg(target_os = "windows")]
    {
        let startup_dir = env::var_os("APPDATA")
            .map(PathBuf::from)
            .ok_or_else(|| "无法解析 APPDATA，无法安装 Windows 登录自启动。".to_string())?
            .join("Microsoft")
            .join("Windows")
            .join("Start Menu")
            .join("Programs")
            .join("Startup");
        Ok(Some(startup_dir.join(BRIDGE_WINDOWS_STARTUP_SCRIPT_NAME)))
    }

    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    {
        Ok(None)
    }
}

fn normalize_topic_segment(value: String, fallback: &str) -> String {
    let mut normalized = String::new();
    let mut last_was_dash = false;

    for character in value.trim().chars() {
        let lowered = character.to_ascii_lowercase();
        let is_allowed =
            lowered.is_ascii_alphanumeric() || lowered == '.' || lowered == '_' || lowered == '-';

        if is_allowed {
            normalized.push(lowered);
            last_was_dash = false;
            continue;
        }

        if !last_was_dash {
            normalized.push('-');
            last_was_dash = true;
        }
    }

    let normalized = normalized.trim_matches('-').to_string();
    if normalized.is_empty() {
        fallback.to_string()
    } else {
        normalized
    }
}

fn shell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\"'\"'"))
}

#[cfg(target_os = "windows")]
fn powershell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "''"))
}

#[cfg(target_os = "windows")]
fn windows_powershell_path(value: &str) -> String {
    if let Some(rest) = value.strip_prefix(r"\\?\UNC\") {
        format!(r"\\{}", rest)
    } else if let Some(rest) = value.strip_prefix(r"\\?\") {
        rest.to_string()
    } else {
        value.to_string()
    }
}

#[cfg(target_os = "windows")]
fn child_process_path(path: &Path) -> PathBuf {
    PathBuf::from(windows_powershell_path(path.to_string_lossy().as_ref()))
}

#[cfg(not(target_os = "windows"))]
fn child_process_path(path: &Path) -> PathBuf {
    path.to_path_buf()
}

#[cfg(target_os = "windows")]
fn powershell_path_quote(path: &Path) -> String {
    powershell_quote(&windows_powershell_path(path.to_string_lossy().as_ref()))
}

#[cfg(target_os = "windows")]
fn cmd_quote_path(path: &Path) -> String {
    format!("\"{}\"", path.display().to_string().replace('"', "\"\""))
}

#[cfg(target_os = "windows")]
fn write_powershell_script_utf8_bom(path: &Path, script: &str) -> Result<(), String> {
    let mut bytes = Vec::with_capacity(3 + script.len());
    bytes.extend_from_slice(&[0xEF, 0xBB, 0xBF]);
    bytes.extend_from_slice(script.as_bytes());
    fs::write(path, bytes).map_err(|e| e.to_string())
}

fn current_timestamp_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis() as u64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bundled_firmware_version_comparison_is_numeric_and_suffix_tolerant() {
        assert_eq!(
            compare_firmware_versions("0.7.28-p4", "0.7.29-p4").unwrap(),
            VersionOrdering::Less
        );
        assert_eq!(
            compare_firmware_versions("v0.7.29", "0.7.29-p4").unwrap(),
            VersionOrdering::Equal
        );
        assert_eq!(
            compare_firmware_versions("0.8.0-p4", "0.7.29-p4").unwrap(),
            VersionOrdering::Greater
        );
        assert!(compare_firmware_versions("unknown", "0.7.29-p4").is_err());
    }

    fn write_test_clawpkg_dir(path: &Path) {
        fs::create_dir_all(path.join("runtime")).unwrap();
        fs::create_dir_all(path.join("assets")).unwrap();
        fs::write(
            path.join("component.json"),
            br#"{"id":"sync-snapshot","name":"Sync Snapshot","version":"1.0.0"}"#,
        )
        .unwrap();
        fs::write(
            path.join("negative-screen.json"),
            br#"{"dashboard":{"title":"Sync Snapshot"}}"#,
        )
        .unwrap();
        fs::write(path.join("buttons.json"), b"[]").unwrap();
        fs::write(path.join("runtime/widget.json"), br#"{"schema_version":1}"#).unwrap();
        fs::write(path.join("share.json"), br#"{"title":"Sync Snapshot"}"#).unwrap();
        fs::write(path.join("assets/.keep"), b"").unwrap();
    }

    #[test]
    fn audio_begin_queue_state_is_backward_compatible_and_boolean_only() {
        assert!(!audio_begin_session_queue_empty(&serde_json::json!({})));
        assert!(!audio_begin_session_queue_empty(&serde_json::json!({
            "sessionQueueEmpty": "true"
        })));
        assert!(audio_begin_session_queue_empty(&serde_json::json!({
            "sessionQueueEmpty": true
        })));
    }

    #[test]
    fn voice_prefers_the_foreground_agent_and_keeps_empty_queue_fallback() {
        assert!(should_use_current_visible_session(true, false, "codex"));
        assert!(should_use_current_visible_session(
            true,
            false,
            "claude-code"
        ));
        assert!(should_use_current_visible_session(false, true, "codex"));
        assert!(should_use_current_visible_session(
            false,
            true,
            "claude-code"
        ));
        assert!(!should_use_current_visible_session(false, false, "codex"));
        assert!(!should_use_current_visible_session(true, true, "openclaw"));

        let stale_target = P4SessionBinding {
            agent_id: "codex".to_string(),
            session_id: "stale-session".to_string(),
            session_title: "stale title".to_string(),
            session_title_unique: true,
            ..P4SessionBinding::default()
        };
        assert!(!device_voice_bound_target_is_addressable(
            true,
            &stale_target
        ));
        assert!(device_voice_bound_target_is_addressable(
            false,
            &stale_target
        ));

        let source = include_str!("lib.rs");
        assert!(source.contains("CodexComposerBridge::is_agent_frontmost"));
        assert!(source.contains("falling back to device session"));
        assert!(source.contains("use_current_visible_session"));
    }

    #[test]
    fn ssh_component_override_accepts_combined_encoder_rotation() {
        assert_eq!(
            canonical_binding_for_control("旋钮双向旋转"),
            Some(("前方旋钮", "knob.rotate_cw / knob.rotate_ccw"))
        );
        assert_eq!(
            canonical_binding_for_control("摇杆向上"),
            Some(("前方摇杆", "joystick.up"))
        );
    }

    #[test]
    fn widget_delete_targets_reject_path_and_ssh_option_injection() {
        assert!(is_safe_widget_id("token-usage"));
        assert!(!is_safe_widget_id("1-token-usage"));
        assert!(!is_safe_widget_id("-token-usage"));
        assert!(!is_safe_widget_id("../token-usage"));
        assert!(!is_safe_widget_id("Token Usage"));
        assert_eq!(
            normalize_widget_ssh_host("petagent@192.168.1.20").unwrap(),
            Some("petagent@192.168.1.20".to_string())
        );
        assert!(normalize_widget_ssh_host("-oProxyCommand=bad").is_err());
        assert!(normalize_widget_ssh_host("petagent@host;reboot").is_err());
    }

    #[test]
    fn device_widget_inventory_normalizes_the_stable_frontend_contract() {
        let raw = serde_json::json!({
            "ok": true,
            "boardDeviceId": "p4-board-a",
            "runtime": "esp-p4",
            "queriedAtMs": 17,
            "activeWidgetId": "tomato-clock",
            "supportsMultiple": false,
            "maxInstalled": 1,
            "items": [{
                "id": "tomato-clock",
                "name": null,
                "kind": null,
                "version": null,
                "active": true,
                "manifestState": "valid",
                "removable": true
            }],
            "warnings": []
        });

        let inventory =
            normalize_device_widget_inventory(&raw, "usb", "esp-p4", Some("p4-board-a")).unwrap();
        assert!(inventory.ok);
        assert_eq!(inventory.freshness, "live");
        assert_eq!(inventory.transport, "usb");
        assert_eq!(inventory.runtime, "esp-p4");
        assert_eq!(inventory.board_device_id.as_deref(), Some("p4-board-a"));
        assert_eq!(inventory.active_widget_id.as_deref(), Some("tomato-clock"));
        assert!(!inventory.supports_multiple);
        assert_eq!(inventory.max_installed, Some(1));
        assert_eq!(inventory.items.len(), 1);
        assert_eq!(inventory.items[0].name, None);
        assert!(inventory.items[0].active);
    }

    #[test]
    fn device_widget_inventory_rejects_a_different_usb_board_identity() {
        let error = normalize_device_widget_inventory(
            &serde_json::json!({
                "ok": true,
                "boardDeviceId": "board-b",
                "items": []
            }),
            "usb",
            "linux",
            Some("board-a"),
        )
        .unwrap_err();
        assert!(error.contains("身份不匹配"));
    }

    #[cfg(unix)]
    #[test]
    fn ssh_inventory_scanner_excludes_previous_and_tombstone_directories() {
        let root = tempfile::tempdir().unwrap();
        let widgets = root.path().join("widgets");
        fs::create_dir_all(widgets.join("active-widget")).unwrap();
        fs::create_dir_all(widgets.join("other-widget")).unwrap();
        fs::create_dir_all(widgets.join("old-widget.previous")).unwrap();
        fs::create_dir_all(widgets.join(".deleting-active-widget")).unwrap();
        fs::write(root.path().join(".active-widget"), "active-widget\n").unwrap();
        fs::write(
            root.path().join("device-config.json"),
            r#"{"boardDeviceId":"board-ssh"}"#,
        )
        .unwrap();
        fs::write(
            widgets.join("active-widget").join("component.json"),
            r#"{"id":"active-widget","name":"Active Widget","kind":"tool","version":"1.2.3"}"#,
        )
        .unwrap();

        let mut command = Command::new("python3");
        command.arg("-").arg(root.path());
        let raw = run_widget_inventory_python(command).unwrap();
        let inventory = normalize_device_widget_inventory(&raw, "ssh", "linux", None).unwrap();

        assert_eq!(inventory.board_device_id.as_deref(), Some("board-ssh"));
        assert_eq!(inventory.active_widget_id.as_deref(), Some("active-widget"));
        assert_eq!(
            inventory
                .items
                .iter()
                .map(|item| item.id.as_str())
                .collect::<Vec<_>>(),
            vec!["active-widget", "other-widget"]
        );
        assert_eq!(inventory.items[0].name.as_deref(), Some("Active Widget"));
        assert_eq!(inventory.items[0].kind.as_deref(), Some("tool"));
        assert_eq!(inventory.items[0].version.as_deref(), Some("1.2.3"));
        assert_eq!(inventory.items[1].manifest_state, "missing");
    }

    #[cfg(unix)]
    #[test]
    fn ssh_delete_of_an_inactive_widget_preserves_active_marker_and_page() {
        let root = tempfile::tempdir().unwrap();
        let widgets = root.path().join("widgets");
        fs::create_dir_all(widgets.join("active-widget")).unwrap();
        fs::create_dir_all(widgets.join("inactive-widget")).unwrap();
        fs::write(root.path().join(".active-widget"), "active-widget\n").unwrap();
        fs::write(root.path().join(".screen-page"), "stats").unwrap();

        let inactive_script = ssh_widget_delete_inner_script("inactive-widget");
        let status = Command::new("sh")
            .arg("-c")
            .arg(inactive_script)
            .arg("sh")
            .arg(root.path())
            .status()
            .unwrap();
        assert!(status.success());
        assert!(!widgets.join("inactive-widget").exists());
        assert_eq!(
            fs::read_to_string(root.path().join(".active-widget")).unwrap(),
            "active-widget\n"
        );
        assert_eq!(
            fs::read_to_string(root.path().join(".screen-page")).unwrap(),
            "stats"
        );

        let active_script = ssh_widget_delete_inner_script("active-widget");
        let status = Command::new("sh")
            .arg("-c")
            .arg(active_script)
            .arg("sh")
            .arg(root.path())
            .status()
            .unwrap();
        assert!(status.success());
        assert!(!widgets.join("active-widget").exists());
        assert_eq!(
            fs::read_to_string(root.path().join(".active-widget")).unwrap(),
            ""
        );
        assert_eq!(
            fs::read_to_string(root.path().join(".screen-page")).unwrap(),
            "main"
        );
    }

    #[test]
    fn visible_composer_timeout_never_becomes_an_explicit_background_failure() {
        let outcome = classify_visible_composer_submit(Err(
            codex_composer::CodexComposerWaitError::CompletionTimeout,
        ));

        assert!(matches!(
            outcome,
            VisibleComposerSubmitOutcome::Unconfirmed(_)
        ));

        let start_outcome = classify_visible_composer_submit(Err(
            codex_composer::CodexComposerWaitError::StartTimeout,
        ));
        assert!(matches!(
            start_outcome,
            VisibleComposerSubmitOutcome::Unconfirmed(_)
        ));
    }

    #[test]
    fn explicit_composer_failure_stays_on_the_foreground_delivery_path() {
        let outcome = classify_visible_composer_submit(Ok(Err("composer rejected".to_string())));

        assert_eq!(
            outcome,
            VisibleComposerSubmitOutcome::ExplicitFailure("composer rejected".to_string())
        );
    }

    #[test]
    fn composer_readback_uncertainty_stays_unconfirmed() {
        let outcome = classify_visible_composer_submit(Ok(Err(
            "ChatGPT（Codex）macOS 提交结果未确认；为避免重复发送".to_string(),
        )));

        assert!(matches!(
            outcome,
            VisibleComposerSubmitOutcome::Unconfirmed(_)
        ));
    }

    #[test]
    fn p4_session_display_defaults_on_and_accepts_explicit_off() {
        let base = serde_json::json!({
            "boardDeviceId": "p4-board-a",
            "agentId": "codex",
            "sessionId": "session-a"
        });
        let default_input: SetP4SessionBindingInput = serde_json::from_value(base.clone()).unwrap();
        assert!(default_input.display_enabled);

        let mut disabled = base;
        disabled["displayEnabled"] = serde_json::json!(false);
        let disabled_input: SetP4SessionBindingInput = serde_json::from_value(disabled).unwrap();
        assert!(!disabled_input.display_enabled);
    }

    #[test]
    fn p4_session_transition_metadata_is_backward_compatible_and_bounded() {
        let legacy: P4SessionQueueInput = serde_json::from_value(serde_json::json!({
            "id": "legacy",
            "title": "Legacy",
            "state": "working"
        }))
        .unwrap();
        assert_eq!(legacy.transition_revision, 0);
        assert_eq!(legacy.terminal_remaining_ms, 0);
        assert!(validate_p4_session_transition_metadata("working", 0, 0).is_ok());
        assert!(validate_p4_session_transition_metadata("done", 42, 60_000).is_ok());
        assert!(validate_p4_session_transition_metadata("error", 42, 60_001).is_err());
        assert!(validate_p4_session_transition_metadata("working", 42, 1).is_err());
        assert!(validate_p4_session_transition_metadata("done", 0, 1).is_err());
        assert!(
            validate_p4_session_transition_metadata("done", JSON_SAFE_INTEGER_MAX + 1, 1,).is_err()
        );
    }

    #[test]
    fn desktop_build_identity_includes_version_git_state_and_protocol_schema() {
        let build = desktop_build_info();
        assert_eq!(build["version"], env!("CARGO_PKG_VERSION"));
        assert!(build["buildId"]
            .as_str()
            .is_some_and(|value| value.starts_with(env!("CARGO_PKG_VERSION"))));
        assert!(build["gitSha"]
            .as_str()
            .is_some_and(|value| !value.is_empty()));
        assert!(build["dirty"].is_boolean());
        assert_eq!(build["protocolSchema"], 7);
    }

    #[test]
    fn p4_agent_switch_resets_exact_session_and_voice_target() {
        let mut current = P4SessionBinding {
            board_device_id: "p4-board-a".to_string(),
            agent_id: "codex".to_string(),
            session_id: "codex-session-a".to_string(),
            auto_follow: true,
            session_title: "Codex task".to_string(),
            session_cwd: "/tmp/codex".to_string(),
            session_title_unique: true,
            desktop_location: "located".to_string(),
            desktop_location_error: "stale".to_string(),
            generation: 7,
        };

        assert!(reset_p4_session_binding_for_agent(
            &mut current,
            "p4-board-a",
            "claude-code",
        ));
        assert_eq!(current.agent_id, "claude-code");
        assert!(current.session_id.is_empty());
        assert!(!current.auto_follow);
        assert!(current.session_title.is_empty());
        assert!(current.session_cwd.is_empty());
        assert!(!current.session_title_unique);
        assert_eq!(current.desktop_location, "not_requested");
        assert!(current.desktop_location_error.is_empty());
        assert_eq!(current.generation, 8);

        assert!(!reset_p4_session_binding_for_agent(
            &mut current,
            "p4-board-a",
            "claude-code",
        ));
        assert_eq!(current.generation, 8);
    }

    #[test]
    fn p4_session_title_uses_workspace_to_disambiguate_codex_tasks() {
        let sessions = vec![
            P4SessionQueueInput {
                id: "one".to_string(),
                title: "修复语音".to_string(),
                cwd: r"D:\code\one".to_string(),
                content: String::new(),
                state: "working".to_string(),
                transition_revision: 0,
                terminal_remaining_ms: 0,
            },
            P4SessionQueueInput {
                id: "two".to_string(),
                title: "修复语音".to_string(),
                cwd: r"D:\code\two".to_string(),
                content: String::new(),
                state: "idle".to_string(),
                transition_revision: 0,
                terminal_remaining_ms: 0,
            },
        ];

        assert!(p4_session_target_is_unique(
            "修复语音",
            r"D:\code\one",
            &sessions
        ));
        assert!(!p4_session_target_is_unique("修复语音", "", &sessions));
    }

    #[test]
    fn duplicate_codex_titles_in_one_workspace_are_not_safe_to_locate() {
        let sessions = vec![
            P4SessionQueueInput {
                id: "one".to_string(),
                title: "修复语音".to_string(),
                cwd: r"D:\code\one".to_string(),
                content: String::new(),
                state: "working".to_string(),
                transition_revision: 0,
                terminal_remaining_ms: 0,
            },
            P4SessionQueueInput {
                id: "two".to_string(),
                title: "修复语音".to_string(),
                cwd: r"D:\code\one".to_string(),
                content: String::new(),
                state: "idle".to_string(),
                transition_revision: 0,
                terminal_remaining_ms: 0,
            },
        ];

        assert!(!p4_session_target_is_unique(
            "修复语音",
            r"D:\code\one",
            &sessions
        ));
    }

    #[test]
    fn only_explicit_desktop_session_actions_request_navigation() {
        assert!(!should_locate_desktop_session(false, "codex"));
        assert!(!should_locate_desktop_session(false, "claude-code"));
        assert!(should_locate_desktop_session(true, "codex"));
        assert!(should_locate_desktop_session(true, "claude-code"));
        assert!(!should_locate_desktop_session(true, "openclaw"));
    }

    #[test]
    fn auto_voice_keeps_its_starting_target_during_queue_refresh() {
        let target = P4SessionBinding {
            board_device_id: "board-p4".to_string(),
            agent_id: "codex".to_string(),
            session_id: "session-a".to_string(),
            auto_follow: true,
            session_title: "会话 A".to_string(),
            generation: 7,
            ..P4SessionBinding::default()
        };
        let refreshed_auto = P4SessionBinding {
            session_id: "session-b".to_string(),
            session_title: "会话 B".to_string(),
            generation: 8,
            ..target.clone()
        };
        assert!(device_voice_binding_matches(&target, &refreshed_auto));

        let explicit_target = P4SessionBinding {
            session_id: "session-a".to_string(),
            auto_follow: false,
            ..target
        };
        let changed_explicit = P4SessionBinding {
            generation: 8,
            ..explicit_target.clone()
        };
        assert!(!device_voice_binding_matches(
            &explicit_target,
            &changed_explicit
        ));
    }

    #[test]
    fn device_voice_agent_bus_route_uses_the_audio_begin_snapshot() {
        let target = P4SessionBinding {
            board_device_id: "board-p4".to_string(),
            agent_id: "codex".to_string(),
            session_id: "session-at-begin".to_string(),
            auto_follow: true,
            ..P4SessionBinding::default()
        };
        assert_eq!(
            frozen_device_voice_inject_target(
                "utterance-a",
                "board-p4",
                "utterance-a",
                "board-p4",
                &target,
                false,
            ),
            Some(("codex".to_string(), "session-at-begin".to_string()))
        );
        assert_eq!(
            frozen_device_voice_inject_target(
                "utterance-old",
                "board-p4",
                "utterance-a",
                "board-p4",
                &target,
                false,
            ),
            None
        );
        assert_eq!(
            frozen_device_voice_inject_target(
                "utterance-a",
                "board-other",
                "utterance-a",
                "board-p4",
                &target,
                false,
            ),
            None
        );
    }

    #[test]
    fn device_voice_final_recognition_can_only_be_claimed_once() {
        let final_handled = AtomicBool::new(false);
        assert!(claim_device_voice_final(&final_handled));
        assert!(!claim_device_voice_final(&final_handled));
    }

    #[test]
    fn only_unhandled_confirm_for_the_same_board_submits_a_ready_voice_draft() {
        let confirm = serde_json::json!({
            "action": "page_enter",
            "handledLocally": false,
            "boardDeviceId": "board-p4",
        });
        assert!(input_event_matches_device_voice_confirm(
            "input/event",
            &confirm,
            "board-p4",
            true,
            false,
        ));
        assert!(!input_event_matches_device_voice_confirm(
            "input/event",
            &serde_json::json!({ "action": "page_enter", "handledLocally": true, "boardDeviceId": "board-p4" }),
            "board-p4",
            true,
            false,
        ));
        assert!(!input_event_matches_device_voice_confirm(
            "input/event",
            &serde_json::json!({ "action": "page_enter", "handledLocally": false, "boardDeviceId": "other" }),
            "board-p4",
            true,
            false,
        ));
        assert!(!input_event_matches_device_voice_confirm(
            "input/event",
            &serde_json::json!({ "action": "page_enter", "handledLocally": false, "boardDeviceId": "board-p4" }),
            "board-p4",
            false,
            false,
        ));
    }

    #[test]
    fn asr_final_stages_a_draft_and_does_not_call_the_submit_bridge() {
        let source = include_str!("lib.rs");
        let composer = include_str!("codex_composer.rs");
        assert!(source.contains("fn stage_device_voice_final("));
        assert!(source.contains("\"draft_ready\""));
        assert!(source.contains("bridge.confirm(revision, &text)"));
        assert!(!composer.contains("pub fn submit(&self"));
    }

    #[test]
    fn device_voice_starts_asr_without_waiting_for_visible_composer_setup() {
        let source = include_str!("lib.rs");
        let startup_thread = source
            .find("pet-visible-composer-startup")
            .expect("visible composer startup thread");
        let recognizer_start = source
            .find("pc_audio::StreamingSpeechRecognizer::start")
            .expect("streaming recognizer startup");
        assert!(startup_thread < recognizer_start);
        assert!(source.contains("wait_for_visible_composer_startup(&context)"));
        assert!(source.contains("composer_startup_ready: Condvar"));
    }

    #[test]
    fn macos_current_voice_activates_stabilizes_and_rebinds_the_composer() {
        let source = include_str!("codex_composer_macos.rs");
        assert!(source.contains("NSApplicationActivationOptions::ActivateAllWindows"));
        assert!(source.contains("AGENT_FRONTMOST_TIMEOUT"));
        assert!(source.contains("COMPOSER_STABILITY_TIMEOUT"));
        assert!(source.contains("COMPOSER_STABILITY_DELAY"));
        assert!(source.contains("application == target.app"));
        assert!(source.contains("pinned.composer = composer.clone()"));
        assert!(source.contains("let target = find_current_visible_target(agent)"));
        assert!(source.contains("set_attribute(&AXAttribute::value()"));
        assert!(!source.contains("keyboard_fallback"));
        assert!(!source.contains("PMF{}"));
        assert!(!source.contains("verify_empty_composer_probe"));
        assert!(!source.contains("输入框已有用户草稿，已拒绝覆盖"));
        assert!(!source.contains("当前会话或输入框在语音输入期间发生变化"));
    }

    #[test]
    fn unresolved_auto_refresh_preserves_the_last_exact_codex_task() {
        let current = P4SessionBinding {
            board_device_id: "board-p4".to_string(),
            agent_id: "codex".to_string(),
            session_id: "session-a".to_string(),
            auto_follow: true,
            ..P4SessionBinding::default()
        };
        assert!(should_preserve_exact_auto_binding(
            &current, "board-p4", "codex", "", true
        ));
        assert!(!should_preserve_exact_auto_binding(
            &current,
            "board-p4",
            "codex",
            "session-b",
            true
        ));
        assert!(!should_preserve_exact_auto_binding(
            &current, "board-p4", "codex", "", false
        ));
    }

    #[test]
    fn p4_voice_button_accepts_all_three_switches() {
        assert_eq!(
            normalize_voice_button(Some("sw1.hold".into())).unwrap(),
            "sw1.hold"
        );
        assert_eq!(
            normalize_voice_button(Some("button.sw2.hold".into())).unwrap(),
            "sw2.hold"
        );
        assert_eq!(
            normalize_voice_button(Some("sw3".into())).unwrap(),
            "sw3.hold"
        );
    }

    #[test]
    fn normalize_empty_bridge_profile_keeps_unbound_timestamp_zero() {
        let profile = normalize_bridge_profile(BridgeProfileFile::default());

        assert_eq!(profile.updated_at, 0);
    }

    #[test]
    fn normalize_saved_bridge_profile_backfills_timestamp() {
        let profile = normalize_bridge_profile(BridgeProfileFile {
            desktop_device_id: "linux-pet-01".to_string(),
            mqtt_url: "mqtt://broker.openclaw.example:1883".to_string(),
            ..BridgeProfileFile::default()
        });

        assert!(profile.updated_at > 0);
    }

    #[test]
    fn build_usb_restore_speech_text_prefers_explicit_text_fields() {
        let payload = serde_json::json!({
            "state": "tool_running",
            "displayText": "正在执行上一条任务",
        });
        let text = build_usb_restore_speech_text("codex", &payload).unwrap_or_default();
        assert_eq!(text, "正在执行上一条任务");
    }

    #[test]
    fn build_usb_restore_speech_text_falls_back_to_state_label() {
        let payload = serde_json::json!({
            "state": "working",
        });
        let text = build_usb_restore_speech_text("codex", &payload).unwrap_or_default();
        assert_eq!(text, "ChatGPT（Codex） 工作中");
    }

    #[test]
    fn usb_active_speech_text_uses_current_follow_source() {
        let payload = serde_json::json!({
            "source": "codex",
            "state": "working",
        });
        assert!(build_usb_active_speech_text("codex", &payload).is_none());
    }

    #[test]
    fn usb_source_allowed_by_follow_prefers_selected_agent() {
        let enabled = HashSet::from(["codex".to_string(), "claude-code".to_string()]);
        let selected = Some("claude-code".to_string());

        assert!(usb_source_allowed_by_follow(
            "claude-code",
            &selected,
            &enabled
        ));
        assert!(!usb_source_allowed_by_follow("codex", &selected, &enabled));
    }

    #[test]
    fn usb_source_state_selection_sends_only_the_strongest_candidate() {
        assert!(should_replace_usb_source_state(None, 10, 100));
        assert!(should_replace_usb_source_state(Some((10, 100)), 15, 90));
        assert!(should_replace_usb_source_state(Some((10, 100)), 10, 101));
        assert!(!should_replace_usb_source_state(Some((15, 100)), 10, 200));
        assert!(!should_replace_usb_source_state(Some((10, 100)), 10, 99));
    }

    #[test]
    fn usb_desktop_identity_is_synced_before_auto_connect() {
        let source = include_str!("lib.rs");

        assert!(
            source.contains("sync_usb_desktop_device_id(&usb_manager)"),
            "USB manager should receive the persisted desktop id before auto-connect starts"
        );
        assert!(
            source.contains("usb_manager.set_desktop_device_id(&id);"),
            "USB ack should carry the persisted desktop id instead of an empty string"
        );
    }

    #[test]
    fn replacing_device_recording_is_silent_and_duplicate_begin_is_ignored() {
        let source = include_str!("lib.rs");

        assert!(source.contains("fn supersede_device_voice_context("));
        assert!(source.contains(
            "stop_device_voice_context(context, \"superseded by a new device recording\", false)"
        ));
        assert!(source
            .contains("event.get(\"duplicate\").and_then(|value| value.as_bool()) != Some(true)"));
    }

    #[test]
    fn verified_usb_binding_becomes_the_most_recent_device() {
        let mut bindings = vec![
            DeviceBinding {
                board_device_id: "board-current".to_string(),
                desktop_device_id: "desktop-a".to_string(),
                wifi_ssid: "USB(/dev/old)".to_string(),
                bound_at: 1,
            },
            DeviceBinding {
                board_device_id: "board-other".to_string(),
                desktop_device_id: "desktop-a".to_string(),
                wifi_ssid: "USB(/dev/other)".to_string(),
                bound_at: 2,
            },
        ];

        upsert_device_binding(
            &mut bindings,
            DeviceBinding {
                board_device_id: "board-current".to_string(),
                desktop_device_id: "desktop-a".to_string(),
                wifi_ssid: "USB(/dev/current)".to_string(),
                bound_at: 3,
            },
        );

        assert_eq!(bindings.len(), 2);
        let latest = bindings.last().expect("latest binding");
        assert_eq!(latest.board_device_id, "board-current");
        assert_eq!(latest.wifi_ssid, "USB(/dev/current)");
        assert_eq!(latest.bound_at, 3);
    }

    #[test]
    fn usb_auto_connect_refreshes_binding_and_replays_bridge_state() {
        let source = include_str!("lib.rs");
        let auto_connect = &source[source
            .find("fn start_usb_auto_connect(")
            .expect("USB auto-connect")
            ..source.find("pub fn run()").expect("Tauri run entry")];

        assert!(auto_connect.contains("persist_connected_usb_binding("));
        assert!(auto_connect.contains("forward_current_state_after_usb_connect("));
    }

    #[test]
    fn usb_host_heartbeat_runs_independently_from_state_forwarding() {
        let source = include_str!("lib.rs");
        let state_forwarder = &source[source
            .find("fn start_usb_state_forwarder(")
            .expect("state forwarder")
            ..source
                .find("fn start_usb_auto_connect(")
                .expect("USB auto-connect")];

        assert!(source.contains("fn start_usb_host_heartbeat("));
        assert!(source.contains("start_usb_host_heartbeat(usb_manager.clone())"));
        assert!(!state_forwarder.contains("system/heartbeat"));
    }

    #[test]
    fn build_usb_restore_speech_text_returns_none_without_state_or_text() {
        let payload = serde_json::json!({
            "reason": "heartbeat",
        });
        assert!(build_usb_restore_speech_text("codex", &payload).is_none());
    }

    #[test]
    fn build_usb_restore_speech_text_skips_openclaw_fallback_text() {
        let payload = serde_json::json!({
            "state": "idle",
        });
        assert!(build_usb_restore_speech_text("openclaw", &payload).is_none());
    }

    #[test]
    fn p4_agent_enter_event_becomes_safe_continue_injection() {
        let payload = serde_json::json!({
            "event": "button.sw2.short_press",
            "action": "agent_enter",
            "handledLocally": false,
        });
        let input = extract_usb_agent_input("input/event", &payload).unwrap();

        assert_eq!(input.text, "继续当前任务。");
        assert_eq!(input.button_event, "button.sw2.short_press");
        assert_eq!(input.input_type, "hardware-control");
    }

    #[test]
    fn p4_custom_prompt_event_preserves_bounded_value() {
        let payload = serde_json::json!({
            "event": "button.sw3.short_press",
            "action": "agent_prompt",
            "value": "总结当前进度并继续。",
            "handledLocally": false,
        });
        let input = extract_usb_agent_input("input/event", &payload).unwrap();

        assert_eq!(input.text, "总结当前进度并继续。");
        assert_eq!(input.action_type, "agent_prompt");
    }

    #[test]
    fn p4_local_page_events_are_not_forwarded_to_agent() {
        let payload = serde_json::json!({
            "event": "knob.rotate_cw",
            "action": "page_app",
            "handledLocally": true,
        });

        assert!(extract_usb_agent_input("input/event", &payload).is_none());
    }

    #[test]
    fn p4_usb_audio_start_does_not_require_a_lan_address() {
        assert_eq!(
            resolve_audio_bridge_pc_ip("start", None, false).unwrap(),
            None
        );
    }

    #[test]
    fn linux_audio_start_keeps_an_explicit_lan_address() {
        assert_eq!(
            resolve_audio_bridge_pc_ip("start", Some("192.168.1.25".to_string()), true).unwrap(),
            Some("192.168.1.25".to_string())
        );
    }

    #[test]
    fn compact_usb_state_preserves_stats_inputs_for_p4() {
        let payload = serde_json::json!({
            "state": "working",
            "sessionTitle": "pet dev",
            "tokenUsage": {
                "totalTokens": 18432,
                "modelContextWindow": 128000,
            },
            "dailyTokenUsage": {
                "totalTokens": 42000,
                "inputTokens": 30000,
                "outputTokens": 12000,
            },
            "metrics": {
                "latency": {"turnMs": 1234, "firstTokenMs": 400},
                "toolCalls": 3,
                "contextUsagePct": 14.4,
            },
            "tsMs": 1780000000000u64,
        });

        let compact = compact_usb_state_payload("codex", &payload);
        assert_eq!(compact["source"], "codex");
        assert_eq!(compact["tokenUsage"]["totalTokens"], 42000);
        assert_eq!(compact["tokenUsage"]["inputTokens"], 30000);
        assert_eq!(compact["tokenUsagePeriod"], "today");
        assert_eq!(compact["metrics"]["latency"]["turnMs"], 1234);
        assert_eq!(compact["metrics"]["toolCalls"], 3);
    }

    #[test]
    fn canonical_input_config_ack_resolves_waiter() {
        let request_id = "test-input-config-ack";
        let receiver = register_button_config_ack_waiter(request_id).unwrap();
        resolve_button_config_ack(
            "input/config-ack",
            &serde_json::json!({"requestId": request_id, "ok": true}),
        );

        assert_eq!(
            receiver.recv_timeout(Duration::from_millis(50)).unwrap()["ok"],
            true
        );
    }

    #[test]
    fn button_config_allows_joystick_session_and_miniapp_proxy_actions() {
        assert!(is_allowed_button_config_event("joystick.up"));
        assert!(is_allowed_button_config_event("joystick.down"));
        assert!(is_allowed_button_config_action("session_next"));
        assert!(is_allowed_button_config_action("session_previous"));
        assert!(is_allowed_button_config_action("session_clear"));
        assert!(is_allowed_button_config_action("miniapp_screen_tap"));
        assert!(is_allowed_button_config_action("miniapp_screen_long_press"));
        assert!(is_allowed_button_config_action("component_center"));
        assert!(is_allowed_button_config_action("page_toggle"));
        assert!(is_allowed_button_config_action("page_enter"));
        assert!(is_allowed_button_config_action("page_back"));
        assert!(is_allowed_button_config_action("page_app"));
        assert!(!is_allowed_button_config_action("page_stats"));
    }

    #[test]
    fn usb_state_payload_freshness_rejects_stale_payload_timestamps() {
        let payload = serde_json::json!({
            "source": "codex",
            "state": "done",
            "tsMs": 10_000,
        });

        assert!(!usb_state_payload_is_fresh(
            Path::new("/tmp/codex.json"),
            &payload,
            USB_STATE_MAX_AGE_MS + 10_001
        ));
    }

    #[test]
    fn bridge_state_scan_is_bounded_and_ignores_non_json_files() {
        let directory = tempfile::tempdir().unwrap();
        for index in 0..(USB_BRIDGE_SCAN_MAX_FILES + 8) {
            fs::write(
                directory.path().join(format!("codex-{index:03}.json")),
                b"{}",
            )
            .unwrap();
        }
        fs::write(directory.path().join("ignored.tmp"), b"{}").unwrap();

        let paths = recent_bridge_json_paths(directory.path(), current_timestamp_ms());

        assert_eq!(paths.len(), USB_BRIDGE_SCAN_MAX_FILES);
        assert!(paths.iter().all(|path| {
            path.extension().and_then(|extension| extension.to_str()) == Some("json")
        }));
    }

    #[test]
    fn stale_bridge_state_payload_is_removed_after_rejection() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("codex--session-stale.json");
        fs::write(&path, b"{}").unwrap();
        let payload = serde_json::json!({
            "source": "codex",
            "state": "done",
            "tsMs": 10_000,
        });

        assert!(!retain_fresh_usb_state_payload(
            &path,
            &payload,
            USB_STATE_MAX_AGE_MS + 10_001
        ));
        assert!(!path.exists());
    }

    #[test]
    fn disabled_usb_sources_include_known_non_enabled_on_startup() {
        let previous = std::collections::HashSet::new();
        let next = std::collections::HashSet::from(["claude-code".to_string()]);
        let mut disabled: Vec<String> = disabled_usb_sources_for_filter(&previous, &next)
            .into_iter()
            .collect();
        disabled.sort();

        assert_eq!(
            disabled,
            vec![
                "codex".to_string(),
                "mimocode".to_string(),
                "openclaw".to_string()
            ]
        );
    }

    #[test]
    fn p4_runtime_routes_to_p4_appearance_sync() {
        let p4_status = usb_serial::UsbConnectionStatus {
            connected: true,
            port_name: "COM42".to_string(),
            baud_rate: 4_000_000,
            board_device_id: "board-p4".to_string(),
            transport: "usb".to_string(),
            runtime: "esp-p4".to_string(),
            device_model: "ESP32-P4 + ESP32-C6".to_string(),
            firmware: "0.1.0-p4".to_string(),
            build_id: "0.1.0-p4+test".to_string(),
            git_sha: "test".to_string(),
            build_dirty: false,
            protocol_schema: 4,
            wire_protocol: "pet-usb-jsonl-v2".to_string(),
            capabilities: serde_json::json!({ "assetFormats": ["p4-mjpeg-v1"] }),
        };
        let p4_asset_format_status = usb_serial::UsbConnectionStatus {
            runtime: String::new(),
            capabilities: serde_json::json!({ "assetFormats": ["p4-mjpeg-v1"] }),
            ..p4_status.clone()
        };
        let linux_status = usb_serial::UsbConnectionStatus {
            runtime: "linux".to_string(),
            capabilities: serde_json::Value::Null,
            ..p4_status.clone()
        };

        assert_eq!(
            usb_appearance_sync_runtime(&p4_status).unwrap(),
            UsbAppearanceSyncRuntime::EspP4
        );
        assert_eq!(
            usb_appearance_sync_runtime(&p4_asset_format_status).unwrap(),
            UsbAppearanceSyncRuntime::EspP4
        );
        assert_eq!(
            usb_appearance_sync_runtime(&linux_status).unwrap(),
            UsbAppearanceSyncRuntime::Linux
        );
    }

    #[test]
    fn appearance_sync_target_is_exact_for_serial_and_required_for_native_only() {
        assert_eq!(
            resolve_appearance_sync_board_device_id(true, "board-a", "").unwrap(),
            "board-a"
        );
        assert_eq!(
            resolve_appearance_sync_board_device_id(true, "board-a", "board-a").unwrap(),
            "board-a"
        );
        assert!(
            resolve_appearance_sync_board_device_id(true, "board-a", "board-b")
                .unwrap_err()
                .contains("目标设备已变化")
        );
        assert!(resolve_appearance_sync_board_device_id(false, "", "")
            .unwrap_err()
            .contains("需要明确的 boardDeviceId"));
        assert_eq!(
            resolve_appearance_sync_board_device_id(false, "", "board-native").unwrap(),
            "board-native"
        );
    }

    #[test]
    fn copy_dir_recursive_counts_files_only() {
        let tmp = tempfile::tempdir().unwrap();
        let src = tmp.path().join("src");
        let dst = tmp.path().join("dst");
        std::fs::create_dir_all(src.join("subdir")).unwrap();
        std::fs::write(src.join("a.txt"), "alpha").unwrap();
        std::fs::write(src.join("subdir/b.txt"), "beta").unwrap();
        std::fs::write(src.join("subdir/c.txt"), "gamma").unwrap();
        let count = copy_dir_recursive(&src, &dst).unwrap();
        assert_eq!(count, 3);
        assert!(dst.join("a.txt").exists());
        assert!(dst.join("subdir/b.txt").exists());
        assert!(dst.join("subdir/c.txt").exists());
    }

    #[test]
    fn component_sync_snapshot_survives_source_removal_and_cleans_its_own_root() {
        let tmp = tempfile::tempdir().unwrap();
        let source = tmp.path().join("source-widget");
        let cache_root = tmp.path().join("component-sync-cache");
        write_test_clawpkg_dir(&source);

        let snapshot = copy_clawpkg_sync_snapshot(&source, &cache_root).unwrap();
        fs::remove_dir_all(&source).unwrap();

        assert!(snapshot.join("component.json").is_file());
        assert!(
            crate::clawpkg::validate_clawpkg_at_path(&snapshot)
                .unwrap()
                .ok
        );
        assert!(release_clawpkg_sync_snapshot_at_path(&snapshot, &cache_root).unwrap());
        assert!(!snapshot.exists());
    }

    #[test]
    fn component_sync_snapshot_rejects_missing_sources_and_outside_cleanup() {
        let tmp = tempfile::tempdir().unwrap();
        let cache_root = tmp.path().join("component-sync-cache");
        let missing = tmp.path().join("missing-widget");
        let error = copy_clawpkg_sync_snapshot(&missing, &cache_root).unwrap_err();
        assert!(error.contains("组件安装源已不存在"));

        fs::create_dir_all(&cache_root).unwrap();
        let outside = tmp.path().join("outside");
        fs::create_dir_all(&outside).unwrap();
        let error = release_clawpkg_sync_snapshot_at_path(&outside, &cache_root).unwrap_err();
        assert!(error.contains("拒绝清理组件同步缓存目录之外"));
        assert!(outside.exists());
    }

    #[test]
    fn install_skill_into_agent_creates_skills_subdir() {
        let tmp = tempfile::tempdir().unwrap();
        let src = tmp.path().join("source");
        std::fs::create_dir_all(src.join("references")).unwrap();
        std::fs::write(src.join("SKILL.md"), "---\nname: test\n---\nbody").unwrap();
        std::fs::write(src.join("references/notes.md"), "ref").unwrap();

        let agent_home = tmp.path().join(".fake-agent");
        std::fs::create_dir_all(&agent_home).unwrap();

        let entry = install_skill_into_agent(&src, &agent_home, "Fake Agent").unwrap();
        assert_eq!(entry.agent, "Fake Agent");
        assert_eq!(entry.file_count, 2);
        assert!(!entry.overwrote);
        assert!(agent_home.join("skills/petui/SKILL.md").exists());
        assert!(agent_home.join("skills/petui/references/notes.md").exists());
    }

    #[test]
    fn install_skill_into_agent_overwrites_existing() {
        let tmp = tempfile::tempdir().unwrap();
        let src = tmp.path().join("source");
        std::fs::create_dir_all(&src).unwrap();
        std::fs::write(src.join("SKILL.md"), "new content").unwrap();

        let agent_home = tmp.path().join(".fake-agent");
        let existing_skill = agent_home.join("skills/petui");
        std::fs::create_dir_all(&existing_skill).unwrap();
        std::fs::write(existing_skill.join("stale.md"), "stale").unwrap();
        let legacy_skill = agent_home.join("skills/petAgent-ui-generator");
        std::fs::create_dir_all(&legacy_skill).unwrap();
        std::fs::write(legacy_skill.join("obsolete.md"), "obsolete").unwrap();

        let entry = install_skill_into_agent(&src, &agent_home, "Fake Agent").unwrap();
        assert!(entry.overwrote);
        assert!(
            !existing_skill.join("stale.md").exists(),
            "stale file removed"
        );
        assert!(existing_skill.join("SKILL.md").exists(), "new file present");
        assert!(!legacy_skill.exists(), "legacy skill directory removed");
        let hidden_install_artifacts = std::fs::read_dir(agent_home.join("skills"))
            .unwrap()
            .filter_map(Result::ok)
            .filter_map(|entry| entry.file_name().into_string().ok())
            .filter(|name| {
                name.starts_with(".petui.install-") || name.starts_with(".petui.backup-")
            })
            .collect::<Vec<_>>();
        assert!(hidden_install_artifacts.is_empty());
    }

    #[test]
    fn partial_bridge_profile_update_preserves_saved_mqtt_credentials() {
        let existing = BridgeProfileFile {
            mqtt_url: "mqtt://broker.example:1883".to_string(),
            mqtt_namespace: "desk".to_string(),
            mqtt_username: "device".to_string(),
            mqtt_password: "saved-secret".to_string(),
            desktop_device_id: "desktop-old".to_string(),
            pet_channel_id: "codex".to_string(),
            enabled_agents: vec!["codex".to_string()],
            selected_agent_id: "codex".to_string(),
            ..BridgeProfileFile::default()
        };
        let merged = merge_bridge_profile_input(
            existing,
            BridgeProfileInput {
                desktop_device_id: "desktop-new".to_string(),
                mqtt_url: "mqtt://broker.example:1883".to_string(),
                mqtt_namespace: None,
                mqtt_username: None,
                mqtt_password: None,
                pet_channel_id: None,
                enabled_agents: Some(vec!["codex".to_string(), "claude-code".to_string()]),
                selected_agent_id: Some("claude-code".to_string()),
            },
        );

        assert_eq!(merged.mqtt_username, "device");
        assert_eq!(merged.mqtt_password, "saved-secret");
        assert_eq!(merged.mqtt_namespace, "desk");
        assert_eq!(merged.pet_channel_id, "claude");
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn powershell_runner_is_written_with_utf8_bom() {
        let tmp = tempfile::tempdir().unwrap();
        let runner = tmp.path().join("run.ps1");

        write_powershell_script_utf8_bom(
            &runner,
            "Set-Location -LiteralPath 'C:\\Users\\TestUser\\.claw-pet'\n",
        )
        .unwrap();

        let bytes = std::fs::read(&runner).unwrap();
        assert_eq!(&bytes[0..3], &[0xEF, 0xBB, 0xBF]);
        assert!(String::from_utf8(bytes[3..].to_vec())
            .unwrap()
            .contains("TestUser"));
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn windows_child_process_paths_remove_verbatim_prefixes_without_losing_unicode() {
        assert_eq!(
            child_process_path(Path::new(
                r"\\?\C:\Users\王文超\AppData\Local\Pet Manager\bridge\runtime\node.exe"
            )),
            PathBuf::from(r"C:\Users\王文超\AppData\Local\Pet Manager\bridge\runtime\node.exe")
        );
        assert_eq!(
            child_process_path(Path::new(r"\\?\UNC\server\share\Pet Manager\node.exe")),
            PathBuf::from(r"\\server\share\Pet Manager\node.exe")
        );
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn windows_bridge_launcher_quotes_paths_with_spaces_without_model_override() {
        let runtime_paths = BridgeRuntimePaths {
            config_dir: PathBuf::from(r"C:\Users\TestUser\.claw-pet"),
            log_path: PathBuf::from(r"C:\Users\TestUser\.claw-pet\logs\status-bridge.log"),
            pid_path: PathBuf::from(r"C:\Users\TestUser\.claw-pet\status-bridge.pid"),
            launch_script_path: PathBuf::from(r"C:\Users\TestUser\.claw-pet\run-status-bridge.ps1"),
            launch_agent_path: None,
        };
        let profile = BridgeProfileFile {
            mqtt_url: "mqtt://example.invalid:1883".to_string(),
            mqtt_namespace: "desk".to_string(),
            mqtt_username: "device".to_string(),
            mqtt_password: "secret".to_string(),
            desktop_device_id: "desktop-test".to_string(),
            enabled_agents: vec!["codex".to_string()],
            selected_agent_id: "codex".to_string(),
            ..BridgeProfileFile::default()
        };
        let bridge_assets = ResolvedBridgeAssets {
            resource_root: PathBuf::from(r"\\?\C:\Users\TestUser\AppData\Local\Pet Manager\bridge"),
            workspace_root: PathBuf::from(
                r"\\?\C:\Users\TestUser\AppData\Local\Pet Manager\bridge\packages\clawd-backend-service",
            ),
            entry_path: PathBuf::from(
                r"\\?\C:\Users\TestUser\AppData\Local\Pet Manager\bridge\packages\clawd-backend-service\src\headless-mqtt.js",
            ),
        };
        let script = build_windows_bridge_launcher_script(
            &runtime_paths,
            &profile,
            &bridge_assets,
            Path::new(r"\\?\C:\Users\TestUser\AppData\Local\Pet Manager\bridge\runtime\node.exe"),
        );

        assert!(script.contains(r#"$entryArg = '"' + $entryPath + '"'"#));
        assert!(script.contains(
            "$process = Start-Process -PassThru -WindowStyle Hidden -FilePath $nodePath -ArgumentList $entryArg"
        ));
        assert!(script.contains(
            "Set-Content -LiteralPath 'C:\\Users\\TestUser\\.claw-pet\\status-bridge.pid' -Value $process.Id -Encoding ascii"
        ));
        assert!(script.contains(r"C:\Users\TestUser\AppData\Local\Pet Manager\bridge"));
        assert!(script.contains("$env:CLAWD_BRIDGE_PORT = '23333'"));
        assert!(script.contains("$env:CLAWD_BRIDGE_STRICT_PORT = '1'"));
        assert!(!script.contains(r"\\?\C:\Users"));
        assert!(!script.contains("CLAWD_CODEX_MODEL"));
        assert!(!script.contains("--model"));
    }

    #[test]
    fn bridge_launch_script_exports_node_dir_on_path_for_cli_shims() {
        let tmp = tempfile::tempdir().unwrap();
        let script_path = tmp.path().join("run-status-bridge.sh");
        let log_path = tmp.path().join("logs").join("status-bridge.log");
        let bridge_root = tmp.path().join("bridge");
        let workspace_root = bridge_root.join(BRIDGE_WORKSPACE_RELATIVE_PATH);
        let entry_path = bridge_root.join(BRIDGE_ENTRY_RELATIVE_PATH);
        std::fs::create_dir_all(entry_path.parent().unwrap()).unwrap();
        std::fs::write(&entry_path, "console.log('bridge');").unwrap();

        let profile = BridgeProfileFile {
            mqtt_url: "mqtt://example.invalid:1883".to_string(),
            mqtt_namespace: "desk".to_string(),
            mqtt_username: "device".to_string(),
            mqtt_password: "secret".to_string(),
            desktop_device_id: "desktop-test".to_string(),
            enabled_agents: vec!["codex".to_string()],
            selected_agent_id: "codex".to_string(),
            ..BridgeProfileFile::default()
        };
        let bridge_assets = ResolvedBridgeAssets {
            resource_root: bridge_root,
            workspace_root,
            entry_path,
        };

        write_launch_script(
            &script_path,
            &log_path,
            &profile,
            &bridge_assets,
            Path::new("/opt/pet-manager/runtime/node"),
        )
        .unwrap();

        let script = std::fs::read_to_string(script_path).unwrap();
        assert!(script.contains("/opt/pet-manager/runtime"));
        assert!(script.contains(
            "export NODE_PATH=\"$BRIDGE_ROOT/packages/clawd-backend-service/node_modules"
        ));
        assert!(!script.contains("${PATH:+:$PATH}"));
        assert!(script.contains("export CLAWD_BRIDGE_PORT='23333'"));
        assert!(script.contains("export CLAWD_BRIDGE_STRICT_PORT=1"));
    }

    #[test]
    fn bridge_agent_status_requests_restart_when_selected_codex_cli_shim_lacks_node() {
        let profile = BridgeProfileFile {
            selected_agent_id: "codex".to_string(),
            enabled_agents: vec!["codex".to_string()],
            ..BridgeProfileFile::default()
        };
        let status = serde_json::json!({
            "ok": true,
            "adapters": [
                { "agentId": "codex", "ready": false, "reason": "codex --version 调用失败 (/Users/me/.npm-global/bin/codex)" },
                { "agentId": "claude-code", "ready": true, "reason": null }
            ]
        });

        assert!(bridge_agent_status_needs_restart(&profile, &status));
    }

    #[test]
    fn bridge_agent_status_requests_restart_when_selected_codex_cli_is_missing() {
        let profile = BridgeProfileFile {
            selected_agent_id: "codex".to_string(),
            enabled_agents: vec!["codex".to_string()],
            ..BridgeProfileFile::default()
        };
        let status = serde_json::json!({
            "ok": true,
            "adapters": [
                { "agentId": "codex", "ready": false, "reason": "codex CLI 未找到（请设置 CODEX_CLI_PATH）" }
            ]
        });

        assert!(bridge_agent_status_needs_restart(&profile, &status));
    }

    #[test]
    fn usb_auto_probe_failures_back_off_and_cap() {
        assert_eq!(usb_auto_retry_delay(1), Duration::from_secs(5));
        assert_eq!(usb_auto_retry_delay(2), Duration::from_secs(10));
        assert_eq!(usb_auto_retry_delay(4), Duration::from_secs(40));
        assert_eq!(usb_auto_retry_delay(5), Duration::from_secs(60));
        assert_eq!(usb_auto_retry_delay(20), Duration::from_secs(60));
    }

    #[test]
    fn usb_auto_probe_key_uses_adapter_identity_not_only_com_number() {
        let first = usb_serial::UsbDeviceInfo {
            port_name: "COM5".to_string(),
            vid: 0x1a86,
            pid: 0x55d3,
            serial_number: "board-a".to_string(),
            manufacturer: String::new(),
            product: String::new(),
        };
        let mut second = first.clone();
        second.serial_number = "board-b".to_string();

        assert_ne!(usb_auto_probe_key(&first), usb_auto_probe_key(&second));
        assert!(usb_auto_probe_key(&first).starts_with("com5|1a86|55d3|"));
    }

    #[test]
    fn private_directory_hardening_is_limited_to_claw_pet_trees() {
        assert!(is_managed_claw_pet_directory(Path::new(".claw-pet/logs")));
        assert!(is_managed_claw_pet_directory(Path::new(
            "/home/user/.claw-pet/logs"
        )));
        assert!(!is_managed_claw_pet_directory(Path::new("/tmp")));
        assert!(!is_managed_claw_pet_directory(Path::new(
            "/tmp/shared-config"
        )));
    }

    #[test]
    fn stale_managed_pid_files_are_discarded_before_runtime_reuse() {
        let tmp = tempfile::tempdir().unwrap();
        let pid_path = tmp.path().join("managed.pid");
        std::fs::write(&pid_path, u32::MAX.to_string()).unwrap();

        assert_eq!(read_live_managed_pid(&pid_path), None);
        assert!(!pid_path.exists());
    }
}
