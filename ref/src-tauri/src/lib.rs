/*
 * [Input] Tauri commands invoked by the React Pet Manager client.
 * [Output] Desktop runtime services for device pairing, bridge management,
 *          local agent discovery, Codex pet import, external/community help links,
 *          controlled Codex Pets CLI installs, device follow-source binding,
 *          stale-state-safe USB forwarding with active speech sync, built-in
 *          appearance default/override WAV cue sync, USB desktop identity propagation,
 *          generated component-draft listing/deletion
 *          with manifest descriptions for component-center card summaries,
 *          .clawpkg USB/SSH installs with per-component button-function
 *          overrides, dashboard full-button USB OTA with backend-held
 *          board ack confirmation and stale USB writer reconnect retry,
 *          managed bridge-only voice injection, stale
 *          LaunchAgent/legacy bridge cleanup, and packaged-resource bridge assets.
 * [Pos] Tauri runtime node in ref/src-tauri/src
 * [Sync] If this file changes, update `ref/.folder.md`.
 */

mod clawpkg;
mod codex_import;
mod usb_serial;

use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::env;
use std::fs;
use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream, UdpSocket};
#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;
#[cfg(unix)]
use std::os::unix::process::CommandExt;
#[cfg(windows)]
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{mpsc, Mutex, OnceLock};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tauri::{Emitter, Manager, RunEvent};

const DEVICE_AP_SSID: &str = "claw-pet";
const DEVICE_AP_PASSWORD: &str = "88888888";
const DEVICE_AP_HOST: &str = "192.168.44.1";
const DEVICE_AP_PORT: u16 = 80;
const DESKTOP_DEVICE_ID_FILE_NAME: &str = "desktop-device-id";
const DEVICE_BINDINGS_FILE_NAME: &str = "device-bindings.json";

const BRIDGE_PROFILE_FILE_NAME: &str = "pet-bridge.json";
const DEFAULT_NAMESPACE: &str = "desk";
const DEFAULT_DESKTOP_DEVICE_ID: &str = "linux-pet-01";
const DEFAULT_MQTT_URL: &str = "mqtt://broker.openclaw.example:1883";
const DEFAULT_MQTT_USERNAME: &str = "device";
const BUNDLED_MQTT_URL: Option<&str> = option_env!("PET_MANAGER_BUNDLED_MQTT_URL");
const BUNDLED_MQTT_USERNAME: Option<&str> = option_env!("PET_MANAGER_BUNDLED_MQTT_USERNAME");
const BUNDLED_MQTT_PASSWORD: Option<&str> = option_env!("PET_MANAGER_BUNDLED_MQTT_PASSWORD");
const DEFAULT_PET_CHANNEL_ID: &str = "openclaw";
const DEFAULT_BRIDGE_PORT: u16 = 23333;
const CLAW_PET_DIR_NAME: &str = ".claw-pet";
const LEGACY_OPENCLAW_DIR_NAME: &str = ".openclaw";
const COMPONENT_DRAFTS_DIR_NAME: &str = "component-drafts";
const LEGACY_BRIDGE_PORT: u16 = 23334;
const BUTTON_CONFIG_ACK_TIMEOUT_SECS: u64 = 12;
const BUTTON_CONFIG_ACK_TIMEOUT_MESSAGE: &str =
    "未收到板端按钮配置确认；设备端可能还没更新到支持 button-config-ack 的运行时，或板端未写入 .button-config。";

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
    if topic != "button-config-ack" {
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
    let device = devices
        .first()
        .ok_or_else(|| "USB 重新连接失败：未找到可用串口".to_string())?;
    let port_name = device.port_name.clone();
    eprintln!(
        "[button-config] reconnecting stale USB writer via {}",
        port_name
    );
    let emitter = app_handle.clone();
    usb_manager.connect(&port_name, move |topic, payload| {
        handle_incoming_usb_message(&emitter, topic, payload);
    })?;
    Ok(usb_manager.status())
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
    if view.trim().to_ascii_lowercase() != "voice_input" {
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

fn forward_usb_input_action_to_bridge(
    payload: &serde_json::Value,
) -> Result<serde_json::Value, String> {
    let Some(text) = extract_usb_voice_input_text(payload) else {
        return Ok(serde_json::json!({
            "ok": true,
            "skipped": true,
            "reason": "not voice_input or empty state",
        }));
    };

    let agent_id = resolve_usb_inject_agent_id();
    let board_device_id = payload
        .get("boardDeviceId")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_string();
    let local_device_id = payload
        .get("localDeviceId")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_string();
    let action_type = payload
        .get("type")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_string();

    let body = serde_json::json!({
        "agentId": agent_id,
        "sessionId": "auto",
        "text": text,
        "buttonEvent": "button.encoder.long_press",
        "metadata": {
            "source": "usb-input-action",
            "inputType": "voice-text",
            "trigger": "device-button",
            "transport": "usb",
            "boardDeviceId": board_device_id,
            "localDeviceId": local_device_id,
            "actionType": action_type,
        }
    });

    // Bridge may be in a short restart window during dev hot-reload.
    // Only target the managed bridge; a stale legacy bridge cannot share the
    // agent-session-bus port and would report misleading injection failures.
    let ports = [DEFAULT_BRIDGE_PORT];
    let mut last_error = String::new();

    for attempt in 1..=8 {
        for port in ports {
            let url = format!("http://127.0.0.1:{}/mock-button-inject", port);
            let client = lan_http_client(Duration::from_secs(150))?;
            let response = match client.post(&url).json(&body).send() {
                Ok(resp) => resp,
                Err(error) => {
                    last_error = format!(
                        "usb input/action inject request failed on {} (attempt {}/8): {}",
                        port, attempt, error
                    );
                    continue;
                }
            };
            let status = response.status();
            let response_text = response.text().unwrap_or_default();
            if !status.is_success() {
                last_error = format!(
                    "usb input/action inject http {} on {} (attempt {}/8): {}",
                    status, port, attempt, response_text
                );
                continue;
            }
            println!("[usb-input-action] bridge response {}", response_text);

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
                last_error = parsed
                    .get("error")
                    .and_then(|v| v.as_str())
                    .unwrap_or("mock-button-inject returned ok=false")
                    .to_string();
                continue;
            }
            return Ok(parsed);
        }
        thread::sleep(Duration::from_millis(250));
    }

    Err(if last_error.is_empty() {
        "usb input/action inject failed after retries".to_string()
    } else {
        last_error
    })
}

fn handle_incoming_usb_message(
    emitter: &tauri::AppHandle,
    topic: String,
    payload: serde_json::Value,
) {
    resolve_button_config_ack(&topic, &payload);

    let _ = emitter.emit(
        "usb-message",
        serde_json::json!({"topic": topic, "payload": payload}),
    );

    if topic == "availability" {
        if let Some(online) = payload.get("online").and_then(|v| v.as_bool()) {
            if !online {
                let _ = emitter.emit("usb-disconnected", ());
            }
        }
        return;
    }

    if topic == "input/action" {
        println!("[usb-input-action] received payload {}", payload);
        let emitter = emitter.clone();
        thread::spawn(move || {
            let Some(voice_text) = extract_usb_voice_input_text(&payload) else {
                return;
            };
            let pending_agent_id = resolve_usb_inject_agent_id();
            let _ = emitter.emit(
                "usb-input-action-result",
                serde_json::json!({
                    "ok": true,
                    "pending": true,
                    "view": "voice_input",
                    "text": voice_text.clone(),
                    "agentId": pending_agent_id.clone(),
                    "sessionId": "auto",
                    "message": format!("已发送到 {}，等待模型回复...", pending_agent_id),
                }),
            );

            match forward_usb_input_action_to_bridge(&payload) {
                Ok(response) => {
                    let request = response.get("request").unwrap_or(&serde_json::Value::Null);
                    let mut agent_id = request
                        .get("agentId")
                        .and_then(|v| v.as_str())
                        .unwrap_or_default()
                        .trim()
                        .to_string();
                    if agent_id.is_empty() {
                        agent_id = resolve_usb_inject_agent_id();
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
                    let message = format!("已发送到 {} · 会话 {}", agent_id, session_id);
                    let _ = emitter.emit(
                        "usb-input-action-result",
                        serde_json::json!({
                            "ok": true,
                            "view": "voice_input",
                            "text": voice_text,
                            "agentId": agent_id,
                            "sessionId": session_id,
                            "message": message,
                            "tokenPreview": reply_preview,
                            "replyPreview": reply_preview,
                            "response": response,
                        }),
                    );
                    println!("[usb-input-action] forward ok");
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
                            "view": "voice_input",
                            "text": voice_text,
                            "message": error_message,
                            "error": error_message,
                            "transient": transient,
                        }),
                    );
                    eprintln!("[usb-input-action] forward failed: {}", error);
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
const VOICE_SERVICE_LAUNCH_SCRIPT_FILE_NAME: &str = "run-voice-service.sh";
const BRIDGE_RESOURCE_ROOT: &str = "bridge";
const BRIDGE_WORKSPACE_RELATIVE_PATH: &str = "packages/clawd-backend-service";
const BRIDGE_ENTRY_RELATIVE_PATH: &str = "packages/clawd-backend-service/src/headless-mqtt.js";
const BRIDGE_LOG_FILE_NAME: &str = "status-bridge.log";
const BRIDGE_PID_FILE_NAME: &str = "status-bridge.pid";
const BRIDGE_LAUNCH_SCRIPT_FILE_NAME: &str = "run-status-bridge.sh";
const BRIDGE_WINDOWS_LAUNCH_SCRIPT_FILE_NAME: &str = "run-status-bridge.ps1";
const BRIDGE_LAUNCH_AGENT_LABEL: &str = "com.petmanager.status-bridge";
const BRIDGE_WINDOWS_STARTUP_SCRIPT_NAME: &str = "Pet Manager Status Bridge.cmd";
const USB_STATE_MAX_AGE_MS: u64 = 10 * 60 * 1000;
const KNOWN_USB_STATE_SOURCES: [&str; 3] = ["claude-code", "codex", "openclaw"];
const PET_MANAGER_APP_BUNDLE_NAME: &str = "Pet Manager.app";
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
    topic: String,
    target_device_id: String,
    target_source: String,
    usb_sent: bool,
    mqtt_sent: bool,
    topics: Vec<String>,
    board_device_ids: Vec<String>,
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
        let board_device_id = normalize_topic_segment(input.board_device_id, "");
        if target_device_id.is_empty() || target_source.is_empty() {
            return Err("缺少目标桌面设备或渠道。".to_string());
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

        let mut usb_sent = false;
        let mut usb_error: Option<String> = None;
        if usb_manager.status().connected {
            if !previous_source.is_empty() && previous_source != target_source {
                let disabled_payload = build_disabled_usb_state_payload(&previous_source);
                if let Err(error) = usb_manager.send_state(&previous_source, &disabled_payload) {
                    usb_error = Some(format!("USB 清理旧渠道失败: {error}"));
                }
            }
            match usb_manager.send("control/remote-cli-binding", &payload) {
                Ok(()) => {
                    usb_sent = true;
                }
                Err(error) => {
                    let message = format!("USB 下发渠道切换失败: {error}");
                    usb_error = Some(match usb_error {
                        Some(previous) => format!("{previous}; {message}"),
                        None => message,
                    });
                }
            }
        }

        let mut topic = if board_device_id.is_empty() {
            String::new()
        } else {
            format!(
                "claw-pet/board/{}/control/remote-cli-binding",
                board_device_id
            )
        };
        let mut mqtt_sent = false;
        let mut mqtt_error: Option<String> = None;
        let mut topics: Vec<String> = Vec::new();
        let mut board_device_ids: Vec<String> = Vec::new();

        {
            let url = format!(
                "http://127.0.0.1:{}/publish-remote-binding",
                DEFAULT_BRIDGE_PORT
            );
            let request_payload = serde_json::json!({
                "boardDeviceId": board_device_id,
                "mqttNamespace": namespace.clone(),
                "binding": {
                    "command": "remote_cli_binding.update",
                    "enabled": true,
                    "targetDeviceId": target_device_id.clone(),
                    "targetSource": target_source.clone(),
                    "previousSource": previous_source.clone(),
                    "mqttNamespace": namespace.clone(),
                    "updatedBy": "pet-manager",
                    "tsMs": current_timestamp_ms(),
                },
            });
            match reqwest::blocking::Client::builder()
                .timeout(Duration::from_secs(5))
                .build()
                .map_err(|e| e.to_string())
                .and_then(|client| {
                    client
                        .post(&url)
                        .json(&request_payload)
                        .send()
                        .map_err(|e| format!("Bridge 未运行或无法连接: {e}"))
                }) {
                Ok(resp) => {
                    let status = resp.status();
                    let body = resp.text().map_err(|e| e.to_string())?;
                    let parsed: DispatchRemoteCliBindingResponse =
                        serde_json::from_str(&body).map_err(|e| format!("解析响应失败: {e}"))?;
                    if parsed.ok {
                        mqtt_sent = true;
                        if !parsed.topic.is_empty() {
                            topic = parsed.topic.clone();
                        }
                        topics = parsed.topics.clone();
                        board_device_ids = parsed.board_device_ids.clone();
                    } else {
                        mqtt_error = Some(
                            parsed
                                .error
                                .unwrap_or_else(|| format!("Bridge 返回错误 (HTTP {})", status)),
                        );
                    }
                }
                Err(error) => {
                    mqtt_error = Some(error);
                }
            }
        }

        let dispatch_error = mqtt_error.clone().or_else(|| usb_error.clone());
        if !usb_sent && !mqtt_sent {
            return Err(dispatch_error.unwrap_or_else(|| {
                "设备未通过 USB 连接，且无法通过 MQTT 发送渠道切换命令。".to_string()
            }));
        }

        Ok(DispatchRemoteCliBindingResponse {
            ok: true,
            topic,
            target_device_id,
            target_source,
            usb_sent,
            mqtt_sent,
            topics,
            board_device_ids,
            error: dispatch_error,
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
    fs::create_dir_all(&config_dir).map_err(|e| e.to_string())?;
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
    fs::write(&id_path, &id).map_err(|e| e.to_string())?;
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

#[tauri::command]
fn save_device_binding(binding: DeviceBinding) -> Result<Vec<DeviceBinding>, String> {
    let bindings_path = get_home_dir()?
        .join(".claw-pet")
        .join(DEVICE_BINDINGS_FILE_NAME);
    let mut bindings = load_device_bindings_inner(&bindings_path)?;

    // Drop legacy mock/preview bindings whenever a stable board id is saved.
    if !is_preview_board_device_id(&binding.board_device_id) {
        bindings.retain(|item| !is_preview_board_device_id(&item.board_device_id));
    }

    // Update existing or append
    let desktop_device_id = binding.desktop_device_id.clone();
    if let Some(existing) = bindings
        .iter_mut()
        .find(|b| b.board_device_id == binding.board_device_id)
    {
        *existing = binding;
    } else {
        bindings.push(binding);
    }

    let payload = serde_json::to_vec_pretty(&bindings).map_err(|e| e.to_string())?;
    fs::write(&bindings_path, payload).map_err(|e| e.to_string())?;

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
        fs::write(&config_path, profile_payload).map_err(|e| e.to_string())?;
    }

    Ok(bindings)
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
    fs::write(&bindings_path, payload).map_err(|e| e.to_string())?;

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
        _ => Err(format!(
            "invalid voiceButton '{value}', expected encoder_button.hold"
        )),
    }
}

#[tauri::command]
#[allow(non_snake_case)]
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
        let resolved_ip = pcIp
            .clone()
            .filter(|s| !s.trim().is_empty())
            .or_else(|| detect_local_outgoing_ip(None))
            .ok_or_else(|| "无法自动获取本机 LAN IP, 请显式传 pcIp".to_string())?;
        obj.insert(
            "pc_ip".to_string(),
            serde_json::Value::String(resolved_ip.clone()),
        );
        obj.insert(
            "pc_port".to_string(),
            serde_json::Value::Number(pcPort.unwrap_or(50001).into()),
        );
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
        obj.insert(
            "voice_button".to_string(),
            serde_json::Value::String(voice_button),
        );
    }

    let command_payload = serde_json::Value::Object(obj.clone());
    let mut usb_sent = false;
    let mut usb_error: Option<String> = None;
    if usb_manager.status().connected {
        match usb_manager.send("control/command", &command_payload) {
            Ok(()) => usb_sent = true,
            Err(error) => usb_error = Some(error),
        }
    }

    let mut mqtt_sent = false;
    let mut mqtt_error: Option<String> = None;
    let mut bridge_response = serde_json::Value::Null;
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
        "mqttError": mqtt_error,
        "bridgeResponse": bridge_response,
    }))
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ButtonConfigBinding {
    event: String,
    action: String,
}

fn is_allowed_button_config_event(event: &str) -> bool {
    matches!(
        event,
        "button.encoder.short_press"
            | "button.encoder.long_press"
            | "knob.rotate_cw / knob.rotate_ccw"
            | "screen.region.tap"
            | "screen.region.long_press"
    )
}

fn is_allowed_button_config_action(action: &str) -> bool {
    matches!(
        action,
        "voice_ptt" | "system_page" | "system_reset" | "volume_adjust" | "disabled"
    )
}

fn send_button_config_and_wait_for_ack(
    usb_manager: &usb_serial::UsbSerialManager,
    request_id: &str,
    command_payload: &serde_json::Value,
    fallback_binding_count: usize,
) -> Result<(serde_json::Value, u64), String> {
    eprintln!("[button-config] sending requestId={}", request_id);
    let ack_receiver = register_button_config_ack_waiter(request_id)?;
    if let Err(error) = usb_manager.send("control/command", command_payload) {
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
fn button_config_signal(
    app_handle: tauri::AppHandle,
    boardDeviceId: String,
    bindings: Vec<ButtonConfigBinding>,
    requestId: Option<String>,
    voiceButton: Option<String>,
    voiceEnabled: Option<bool>,
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> Result<serde_json::Value, String> {
    if !usb_manager.status().connected {
        return Err("USB 未连接,无法通过 USB OTA 下发按钮配置".to_string());
    }
    if bindings.is_empty() {
        return Err("按钮配置为空,无法下发".to_string());
    }
    let voice_button = normalize_voice_button(voiceButton)?;
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
        normalized_bindings.push(serde_json::json!({
            "event": event,
            "action": action,
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

    let (ack_payload, binding_count) = match send_button_config_and_wait_for_ack(
        usb_manager.inner(),
        &request_id,
        &command_payload,
        normalized_bindings.len(),
    ) {
        Ok(result) => result,
        Err(error) if error == BUTTON_CONFIG_ACK_TIMEOUT_MESSAGE => {
            reconnect_usb_serial_for_command(&app_handle, usb_manager.inner())?;
            send_button_config_and_wait_for_ack(
                usb_manager.inner(),
                &request_id,
                &command_payload,
                normalized_bindings.len(),
            )?
        }
        Err(error) => return Err(error),
    };

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

#[cfg(not(target_os = "macos"))]
fn trigger_wifi_scan(_interface: &str) {
    // On Windows, netsh wlan connect triggers an implicit scan.
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

#[tauri::command]
fn save_bridge_profile(input: BridgeProfileInput) -> Result<BridgeProfileResponse, String> {
    let config_path = get_bridge_profile_path()?;
    let existing = read_bridge_profile(&config_path)?.unwrap_or_default();
    let profile = normalize_bridge_profile(BridgeProfileFile {
        version: 1,
        updated_at: current_timestamp_ms(),
        desktop_device_id: input.desktop_device_id,
        mqtt_url: input.mqtt_url,
        mqtt_namespace: input
            .mqtt_namespace
            .unwrap_or_else(|| DEFAULT_NAMESPACE.to_string()),
        mqtt_username: input.mqtt_username.unwrap_or_default(),
        mqtt_password: input.mqtt_password.unwrap_or_default(),
        pet_channel_id: input
            .pet_channel_id
            .unwrap_or_else(|| DEFAULT_PET_CHANNEL_ID.to_string()),
        enabled_agents: input.enabled_agents.unwrap_or(existing.enabled_agents),
        selected_agent_id: input
            .selected_agent_id
            .unwrap_or(existing.selected_agent_id),
    });

    if profile.desktop_device_id.is_empty() {
        return Err("Desktop ID 不能为空。".to_string());
    }

    if profile.mqtt_url.is_empty() {
        return Err("MQTT URL 不能为空。".to_string());
    }

    if let Some(parent_dir) = config_path.parent() {
        fs::create_dir_all(parent_dir).map_err(|error| error.to_string())?;
    }

    let payload = serde_json::to_vec_pretty(&profile).map_err(|error| error.to_string())?;
    fs::write(&config_path, payload).map_err(|error| error.to_string())?;

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
        fs::create_dir_all(parent_dir).map_err(|error| error.to_string())?;
    }
    let payload = serde_json::to_vec_pretty(&profile).map_err(|error| error.to_string())?;
    fs::write(&config_path, payload).map_err(|error| error.to_string())?;

    Ok(AgentSelectionResponse {
        enabled_agents: profile.enabled_agents,
        selected_agent_id: profile.selected_agent_id,
        has_saved_selection: true,
        config_path: config_path.display().to_string(),
    })
}

#[tauri::command]
fn ensure_bridge_runtime(
    app_handle: tauri::AppHandle,
    input: Option<EnsureBridgeRuntimeInput>,
) -> Result<BridgeRuntimeStatusResponse, String> {
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
            let _ = fs::write(&config_path, payload);
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

    let mut running = probe_bridge_running(DEFAULT_BRIDGE_PORT);
    let mut pid = read_pid(&runtime_paths.pid_path);

    // If a bridge is already running but it's an old external process (not ours),
    // kill it so we can start the bundled version with all endpoints.
    if running && pid.is_none() {
        // Bridge on port but no PID file → external/old process. Force restart.
        stop_process_on_port(DEFAULT_BRIDGE_PORT);
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
        // Try spawning node directly first (avoids macOS permission issues with
        // /bin/sh from inside a .app bundle), fall back to the shell script.
        pid = Some(
            start_bridge_direct(
                &node_path,
                &bridge_assets,
                &profile,
                &runtime_paths.log_path,
                &runtime_paths.pid_path,
            )
            .or_else(|_| {
                start_bridge_process(
                    &runtime_paths.launch_script_path,
                    &runtime_paths.log_path,
                    &runtime_paths.pid_path,
                )
            })?,
        );
        running = wait_for_bridge_ready(DEFAULT_BRIDGE_PORT, 36, 200);
        mode = if running { "ready" } else { "error" };
        message = if running {
            format!("bridge 已启动，正在发布到 {}。", build_topic_base(&profile))
        } else {
            "bridge 已尝试启动，但当前还没有连上本地状态端口。请检查日志。".to_string()
        };
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
fn ensure_voice_runtime(
    app_handle: tauri::AppHandle,
    input: Option<EnsureVoiceRuntimeInput>,
) -> Result<VoiceRuntimeStatusResponse, String> {
    let force_restart = input.unwrap_or_default().force_restart;
    let config_path = get_bridge_profile_path()?;
    let profile = normalize_bridge_profile(read_bridge_profile(&config_path)?.unwrap_or_default());
    let runtime_paths = resolve_voice_runtime_paths(&config_path)?;
    let voice_assets = resolve_voice_service_assets(&app_handle)?;
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

    // Defensive guard: voice-service-node bakes VOICE_AGENT_ID into the
    // worker child's env at spawn time, so starting it without a
    // resolved selection produces a worker that throws ConfigError on
    // every job dispatch. Just report inactive and let the front-end
    // try again once the user has picked an agent.
    if profile.selected_agent_id.trim().is_empty() {
        let running = probe_voice_service_running(DEFAULT_VOICE_SERVICE_PORT);
        let pid = read_pid(&runtime_paths.pid_path);
        return Ok(build_voice_runtime_status(
            &profile,
            &runtime_paths,
            &voice_assets,
            "inactive",
            "暂未选择编程工具（agent），voice-service 不会启动。请在仪表盘选一个 agent 后重试。"
                .to_string(),
        )
        .with_runtime(running, pid));
    }

    if force_restart {
        stop_managed_process(&runtime_paths.pid_path);
        thread::sleep(Duration::from_millis(180));
    }

    let mut running = probe_voice_service_running(DEFAULT_VOICE_SERVICE_PORT);
    let mut pid = read_pid(&runtime_paths.pid_path);
    if !running {
        pid = Some(start_voice_service_direct(
            &node_path,
            &voice_assets,
            &profile,
            &runtime_paths.log_path,
            &runtime_paths.pid_path,
        )?);
        running = wait_for_voice_service_ready(DEFAULT_VOICE_SERVICE_PORT, 50, 200);
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
        let shell = env::var("SHELL").unwrap_or_else(|_| "/bin/sh".to_string());
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
    let mut extra = vec![
        format!("/opt/homebrew/bin/{name}"),
        format!("/usr/local/bin/{name}"),
    ];

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

fn detect_claude_code() -> DetectedAgent {
    let home = get_home_dir().ok();
    let found = find_agent_executable(
        "claude",
        home.as_deref(),
        &[".local/bin/claude", ".claude/local/claude"],
        "claude.exe",
    );
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

    let (detected, status, detail) = match &found {
        Some(path) => {
            if has_hooks {
                (true, "ready".to_string(), format!("已安装: {}", path))
            } else {
                (
                    true,
                    "needs_hook".to_string(),
                    format!("已安装但需要配置 hooks: {}", path),
                )
            }
        }
        None => (
            false,
            "not_found".to_string(),
            "未检测到 Claude Code CLI".to_string(),
        ),
    };

    DetectedAgent {
        id: "claude-code".to_string(),
        label: "Claude Code".to_string(),
        detected,
        ready: status == "ready",
        status,
        detail,
        command_path: found.unwrap_or_default(),
        config_path: settings_path,
        activity_path: String::new(),
        can_sync_hook: detected,
    }
}

fn detect_codex() -> DetectedAgent {
    let home = get_home_dir().ok();
    let cli_path =
        find_agent_executable("codex", home.as_deref(), &[".local/bin/codex"], "codex.exe");
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
                    "已检测到 Codex 客户端本地会话数据（未发现 CLI 入口）".to_string(),
                )
            } else if let Some(client_path) = desktop_path.as_ref() {
                (
                    true,
                    "ready".to_string(),
                    format!("已检测到 Codex 桌面客户端: {}", client_path),
                )
            } else {
                (
                    false,
                    "not_found".to_string(),
                    "未检测到 Codex CLI 或桌面客户端".to_string(),
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
        label: "Codex".to_string(),
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

#[tauri::command]
fn detect_local_agents() -> Result<AgentDiscoveryResponse, String> {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);

    let agents = vec![detect_claude_code(), detect_codex(), detect_openclaw()];

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
        codex_import::import_codex_pet(&pet_id, &app_local_data_dir)
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
    if let Some((source, payload)) = load_last_usb_active_state()
        .or_else(|| pick_best_usb_bridge_state(false))
        .or_else(|| pick_best_usb_bridge_state(true))
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
async fn usb_get_status(
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
) -> Result<usb_serial::UsbConnectionStatus, String> {
    Ok(usb_manager.status())
}

/// Remote-set the device's screen page (main | stats). Forwards via USB serial
/// as topic "control/screen-page" with {"page": "<value>"}. Used as both a
/// recovery hatch (when widget OTA leaves the device stuck on stats) and a
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

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct UsbSyncAppearanceResult {
    ok: bool,
    file_count: u32,
    byte_count: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

#[tauri::command]
async fn usb_sync_appearance(
    app_handle: tauri::AppHandle,
    usb_manager: tauri::State<'_, usb_serial::UsbSerialManager>,
    appearance_id: String,
) -> Result<UsbSyncAppearanceResult, String> {
    let status = usb_manager.status();
    if !status.connected {
        return Err("USB 未连接，请先通过 USB 连接设备".to_string());
    }

    let app_local_data_dir = app_handle
        .path()
        .app_local_data_dir()
        .map_err(|e| format!("无法解析 AppLocalData 目录: {}", e))?;

    // Built-in terrier: use videos bundled in app resources
    let appearance_dir = if appearance_id == "builtin-terrier" {
        let resource_dir = app_handle
            .path()
            .resource_dir()
            .map_err(|e| format!("无法解析 resource 目录: {}", e))?;
        let clips_dir = resource_dir.join("terrier-clips");
        if !clips_dir.is_dir() {
            return Err(format!("内置形象资源未找到: {}", clips_dir.display()));
        }
        // Generate a temporary manifest for sync_appearance
        let manifest_dir = app_local_data_dir
            .join("custom-appearances")
            .join("builtin-terrier");
        let _ = std::fs::create_dir_all(manifest_dir.join("videos"));
        let audio_overrides: HashMap<String, String> =
            std::fs::read_to_string(manifest_dir.join("audio-overrides.json"))
                .ok()
                .and_then(|raw| serde_json::from_str(&raw).ok())
                .unwrap_or_default();
        // Build families from mp4 files in clips_dir and attach optional WAV cues.
        let mut families = Vec::new();
        if let Ok(entries) = std::fs::read_dir(&clips_dir) {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().to_string();
                if name.ends_with(".mp4") {
                    let family = name.trim_end_matches(".mp4").to_string();
                    let dest = manifest_dir.join("videos").join(&name);
                    let _ = std::fs::copy(entry.path(), &dest);
                    let mut family_entry = serde_json::json!({
                        "family": family.clone(),
                        "ok": true,
                        "videoPath": format!("custom-appearances/builtin-terrier/videos/{}", name),
                    });
                    let audio_override = audio_overrides
                        .get(&family)
                        .map(|rel| app_local_data_dir.join(rel))
                        .filter(|path| path.is_file());
                    let audio_name = format!("{}.wav", family);
                    let audio_source = audio_override.or_else(|| {
                        default_appearance_audio_cue_name(family.as_str())
                            .map(|cue_name| clips_dir.join(cue_name))
                            .filter(|path| path.is_file())
                    });
                    if let Some(audio_source) = audio_source {
                        let audio_dest = manifest_dir.join("videos").join(&audio_name);
                        let _ = std::fs::copy(&audio_source, &audio_dest);
                        family_entry["audioPath"] = serde_json::json!(format!(
                            "custom-appearances/builtin-terrier/videos/{}",
                            audio_name
                        ));
                    }
                    families.push(family_entry);
                }
            }
        }
        let manifest = serde_json::json!({ "families": families });
        let _ = std::fs::write(
            manifest_dir.join("manifest.json"),
            serde_json::to_string_pretty(&manifest).unwrap_or_default(),
        );
        manifest_dir
    } else {
        let dir = app_local_data_dir
            .join("custom-appearances")
            .join(&appearance_id);
        if let Ok(resource_dir) = app_handle.path().resource_dir() {
            let clips_dir = resource_dir.join("terrier-clips");
            ensure_default_appearance_audio_cues(&dir, &clips_dir)?;
        }
        dir
    };

    if !appearance_dir.join("manifest.json").is_file() {
        return Err(format!("未找到形象素材: {}", appearance_dir.display()));
    }

    let mgr = usb_manager.inner().clone();
    let dir = appearance_dir.clone();
    let data_dir = app_local_data_dir.clone();
    let emitter = app_handle.clone();
    tauri::async_runtime::spawn_blocking(move || {
        match mgr.sync_appearance(
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
        ) {
            Ok((file_count, byte_count)) => Ok(UsbSyncAppearanceResult {
                ok: true,
                file_count,
                byte_count,
                error: None,
            }),
            Err(e) => Ok(UsbSyncAppearanceResult {
                ok: false,
                file_count: 0,
                byte_count: 0,
                error: Some(e),
            }),
        }
    })
    .await
    .map_err(|e| e.to_string())?
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
        agent: "Claude Code",
        home_dir: ".claude",
    },
    SkillTarget {
        agent: "Codex CLI",
        home_dir: ".codex",
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

const SKILL_NAME: &str = "petAgent-ui-generator";

/// Locate the on-disk `skills/petAgent-ui-generator` directory the
/// install_widget_skill command copies into each coding agent's home.
///
/// Paths, in order:
///   1. **Production bundle** — looks under `app.path().resource_dir()`
///      where tauri.conf.json's `bundle.resources` placed
///      `skills/petAgent-ui-generator` at build time.
///   2. **Debug fallback** — `CARGO_MANIFEST_DIR/../../skills/petAgent-ui-generator`,
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
        "skill 源目录不存在；尝试过:\n  {}\n(production: tauri.conf.json bundle.resources 必须包含 skills/petAgent-ui-generator; debug: 确认从 claw-pet-manager 根目录运行)",
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
    let overwrote = dst.exists();
    if overwrote {
        std::fs::remove_dir_all(&dst).map_err(|e| format!("清理旧 skill 目录失败: {}", e))?;
    }
    let file_count = copy_dir_recursive(src, &dst).map_err(|e| format!("拷贝失败: {}", e))?;
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

    // Fallback: nothing detected -> force-install to Claude Code
    if installed.is_empty() {
        let fallback_home = home.join(".claude");
        match install_skill_into_agent(&src, &fallback_home, "Claude Code (fallback)") {
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

fn is_safe_builtin_clawpkg_id(id: &str) -> bool {
    !id.is_empty()
        && id
            .chars()
            .all(|ch| ch.is_ascii_alphanumeric() || ch == '-' || ch == '_')
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
    let payload_bytes = payload.as_bytes().len();

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
            for i in 0..archive.len() {
                let mut entry = archive.by_index(i).map_err(|e| e.to_string())?;
                let outpath = stage_widget.join(entry.name());
                if entry.is_dir() {
                    std::fs::create_dir_all(&outpath).map_err(|e| e.to_string())?;
                } else {
                    if let Some(parent) = outpath.parent() {
                        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
                    }
                    let mut outfile = std::fs::File::create(&outpath).map_err(|e| e.to_string())?;
                    std::io::copy(&mut entry, &mut outfile).map_err(|e| e.to_string())?;
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
             tar -xzf - -C \"$stage\"; \
             sudo rm -rf {dir}/{id}; \
             sudo mv \"$stage/{id}\" {dir}/{id}; \
             rmdir \"$stage\" 2>/dev/null || true; \
             echo {id} | sudo tee /opt/board-runtime/.active-widget > /dev/null; \
             echo stats | sudo tee /opt/board-runtime/.screen-page > /dev/null",
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

/// For a ComponentCenter option label, return the canonical control + event
/// pair written into buttons.json when the user remaps an action.
fn canonical_binding_for_control(control: &str) -> Option<(&'static str, &'static str)> {
    let binding = match control {
        "屏幕点击" => ("屏幕区域", "screen.region.tap"),
        "屏幕长按" => ("屏幕区域", "screen.region.long_press"),
        "旋钮旋转" => ("旋钮", "knob.rotate_cw / knob.rotate_ccw"),
        // Backward-compatible labels from older component-center builds.
        "屏幕区域" => ("屏幕区域", "screen.region.tap"),
        _ => return None,
    };
    Some(binding)
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct ComponentDraftEntry {
    id: String,
    name: String,
    description: String,
    path: String,
    is_clawpkg_zip: bool,
    mtime_ms: u64,
    /// COMPONENT_DASHBOARD_V1 slot values (flat string map, `progress` flattened
    /// to "value:label"). Empty if the draft's negative-screen.json couldn't be
    /// parsed — frontend then shows the device preview with empty slots. Sent so
    /// the gallery's right-hand preview panel can render draft content without a
    /// separate fetch round-trip.
    dashboard: std::collections::HashMap<String, String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DeleteComponentDraftInput {
    path: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct DeleteComponentDraftResult {
    ok: bool,
    deleted_path: String,
}

/// Scan generated component draft folders/zips.
/// Returns entries sorted newest-first. Each entry has a manifest (component.json
/// inside dir, or zip first-level component.json). For one-click install in the UI.
#[tauri::command]
async fn list_component_drafts() -> Result<Vec<ComponentDraftEntry>, String> {
    let mut entries: Vec<ComponentDraftEntry> = Vec::new();
    let mut seen_paths: HashSet<PathBuf> = HashSet::new();

    for drafts_root in component_draft_roots()? {
        if !drafts_root.exists() {
            continue;
        }
        scan_component_drafts_root(&drafts_root, &mut entries, &mut seen_paths)?;
    }

    /* newest first */
    entries.sort_by(|a, b| b.mtime_ms.cmp(&a.mtime_ms));
    Ok(entries)
}

#[tauri::command]
async fn delete_component_draft(
    input: DeleteComponentDraftInput,
) -> Result<DeleteComponentDraftResult, String> {
    let draft_roots = component_draft_roots()?;
    let target = canonicalize_component_draft_path(&input.path, &draft_roots)?;
    if target.is_dir() {
        fs::remove_dir_all(&target)
            .map_err(|error| format!("删除组件目录失败 {}: {}", target.display(), error))?;
    } else if target.is_file() {
        let is_clawpkg_file = target
            .extension()
            .and_then(|value| value.to_str())
            .map(|value| value.eq_ignore_ascii_case("clawpkg") || value.eq_ignore_ascii_case("zip"))
            .unwrap_or(false);
        if !is_clawpkg_file {
            return Err("只能删除组件草稿目录或 .clawpkg/.zip 文件。".to_string());
        }
        fs::remove_file(&target)
            .map_err(|error| format!("删除组件文件失败 {}: {}", target.display(), error))?;
    } else {
        return Err("只能删除组件草稿目录或 .clawpkg/.zip 文件。".to_string());
    }
    Ok(DeleteComponentDraftResult {
        ok: true,
        deleted_path: target.display().to_string(),
    })
}

fn component_drafts_root() -> Result<PathBuf, String> {
    Ok(get_home_dir()?
        .join(CLAW_PET_DIR_NAME)
        .join(COMPONENT_DRAFTS_DIR_NAME))
}

fn legacy_component_drafts_root() -> Result<PathBuf, String> {
    Ok(get_home_dir()?
        .join(LEGACY_OPENCLAW_DIR_NAME)
        .join(COMPONENT_DRAFTS_DIR_NAME))
}

fn component_draft_roots() -> Result<Vec<PathBuf>, String> {
    let primary = component_drafts_root()?;
    let legacy = legacy_component_drafts_root()?;
    if primary == legacy {
        Ok(vec![primary])
    } else {
        Ok(vec![primary, legacy])
    }
}

fn scan_component_drafts_root(
    drafts_root: &Path,
    entries: &mut Vec<ComponentDraftEntry>,
    seen_paths: &mut HashSet<PathBuf>,
) -> Result<(), String> {
    let read = match std::fs::read_dir(drafts_root) {
        Ok(rd) => rd,
        Err(e) => return Err(format!("read {}: {}", drafts_root.display(), e)),
    };
    for sub in read.flatten() {
        let path = sub.path();
        let meta = match sub.metadata() {
            Ok(m) => m,
            Err(_) => continue,
        };
        let mtime_ms = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_millis() as u64)
            .unwrap_or(0);

        if meta.is_dir() {
            let direct_manifest = path.join("component.json");
            if direct_manifest.exists() {
                push_draft_entry_once(entries, seen_paths, &path, false, mtime_ms);
            } else if let Ok(inner) = std::fs::read_dir(&path) {
                for child in inner.flatten() {
                    let cpath = child.path();
                    if cpath.is_dir() && cpath.join("component.json").exists() {
                        push_draft_entry_once(entries, seen_paths, &cpath, false, mtime_ms);
                    }
                }
            }
        } else if path
            .extension()
            .and_then(|s| s.to_str())
            .map(|s| s.eq_ignore_ascii_case("clawpkg") || s.eq_ignore_ascii_case("zip"))
            .unwrap_or(false)
        {
            push_zip_draft_entry_once(entries, seen_paths, &path, mtime_ms);
        }
    }
    Ok(())
}

fn push_draft_entry_once(
    entries: &mut Vec<ComponentDraftEntry>,
    seen_paths: &mut HashSet<PathBuf>,
    path: &Path,
    is_zip: bool,
    mtime_ms: u64,
) {
    let key = path.canonicalize().unwrap_or_else(|_| path.to_path_buf());
    if !seen_paths.insert(key) {
        return;
    }
    if let Some(entry) = read_draft_entry(path, is_zip, mtime_ms) {
        entries.push(entry);
    }
}

fn push_zip_draft_entry_once(
    entries: &mut Vec<ComponentDraftEntry>,
    seen_paths: &mut HashSet<PathBuf>,
    path: &Path,
    mtime_ms: u64,
) {
    let key = path.canonicalize().unwrap_or_else(|_| path.to_path_buf());
    if !seen_paths.insert(key) {
        return;
    }
    let filename = path
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("(unnamed.clawpkg)")
        .to_string();
    match crate::clawpkg::validate_clawpkg_at_path(path) {
        Ok(v) => {
            if let Some(manifest) = v.manifest {
                entries.push(ComponentDraftEntry {
                    id: manifest.id,
                    name: manifest.name,
                    description: read_component_description(path),
                    path: path.display().to_string(),
                    is_clawpkg_zip: true,
                    mtime_ms,
                    dashboard: manifest.dashboard,
                });
            } else {
                eprintln!(
                    "[list_component_drafts] zip {} 校验失败,缺少 manifest: {}",
                    path.display(),
                    v.errors.join("; ")
                );
                entries.push(ComponentDraftEntry {
                    id: filename.clone(),
                    name: format!("{} (校验失败)", filename),
                    description: String::new(),
                    path: path.display().to_string(),
                    is_clawpkg_zip: true,
                    mtime_ms,
                    dashboard: std::collections::HashMap::new(),
                });
            }
        }
        Err(err) => {
            eprintln!(
                "[list_component_drafts] zip {} 校验异常: {}",
                path.display(),
                err
            );
            entries.push(ComponentDraftEntry {
                id: filename.clone(),
                name: format!("{} (无法读取)", filename),
                description: String::new(),
                path: path.display().to_string(),
                is_clawpkg_zip: true,
                mtime_ms,
                dashboard: std::collections::HashMap::new(),
            });
        }
    }
}

fn canonicalize_component_draft_path(
    path: &str,
    draft_roots: &[PathBuf],
) -> Result<PathBuf, String> {
    let target = PathBuf::from(path);
    if !target.is_absolute() {
        return Err("组件草稿路径必须是绝对路径。".to_string());
    }
    let target = target
        .canonicalize()
        .map_err(|error| format!("无法解析组件草稿路径 {}: {}", path, error))?;
    for drafts_root in draft_roots {
        let Ok(drafts_root) = drafts_root.canonicalize() else {
            continue;
        };
        if target != drafts_root && target.starts_with(&drafts_root) {
            return Ok(target);
        }
    }
    Err("只能删除 Pet Manager component-drafts 目录下的组件草稿。".to_string())
}

fn read_draft_entry(
    path: &std::path::Path,
    is_zip: bool,
    mtime_ms: u64,
) -> Option<ComponentDraftEntry> {
    /* Try the full validator first — gives us id+name+dashboard in one pass
    AND ensures the draft is install-shaped. If it fails (missing files,
    byte overflow), fall back to id+name from component.json so the user
    still sees their draft in the grid with an empty preview, rather than
    it silently disappearing. */
    let mut dashboard = std::collections::HashMap::new();
    let (id, name) = match crate::clawpkg::validate_clawpkg_at_path(path) {
        Ok(v) if v.ok => {
            let manifest = v.manifest?;
            dashboard = manifest.dashboard;
            (manifest.id, manifest.name)
        }
        _ => {
            let mp = path.join("component.json");
            let bytes = std::fs::read(&mp).ok()?;
            let v: serde_json::Value = serde_json::from_slice(&bytes).ok()?;
            let id = v
                .get("id")
                .and_then(|x| x.as_str())
                .unwrap_or("")
                .to_string();
            if id.is_empty() {
                return None;
            }
            let name = v
                .get("name")
                .and_then(|x| x.as_str())
                .unwrap_or(&id)
                .to_string();
            (id, name)
        }
    };
    Some(ComponentDraftEntry {
        id,
        name,
        description: read_component_description(path),
        path: path.display().to_string(),
        is_clawpkg_zip: is_zip,
        mtime_ms,
        dashboard,
    })
}

fn read_component_description(path: &std::path::Path) -> String {
    let manifest_path = if path.is_dir() {
        path.join("component.json")
    } else {
        return String::new();
    };
    let bytes = match std::fs::read(manifest_path) {
        Ok(bytes) => bytes,
        Err(_) => return String::new(),
    };
    let value: serde_json::Value = match serde_json::from_slice(&bytes) {
        Ok(value) => value,
        Err(_) => return String::new(),
    };
    value
        .get("description")
        .and_then(|description| description.as_str())
        .map(str::trim)
        .filter(|description| !description.is_empty())
        .unwrap_or("")
        .to_string()
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

    for key in ["tokenUsage", "token_usage", "usage"] {
        if let Some(value) = payload.get(key).and_then(|value| value.as_object()) {
            out.insert(
                "tokenUsage".to_string(),
                serde_json::Value::Object(value.clone()),
            );
            break;
        }
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
        "openclaw" => 10,
        _ => 0,
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
        "codex" => "Codex".to_string(),
        "claude" | "claude-code" => "Claude".to_string(),
        "openclaw" => "OpenClaw".to_string(),
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

fn pick_best_usb_bridge_state(exclude_speaking: bool) -> Option<(String, serde_json::Value)> {
    let tmp = env::var("TMPDIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| env::temp_dir());
    let state_dir = tmp.join("pet-manager-bridge-state");
    let enabled_agents = load_enabled_agents_filter_for_usb();
    let selected_agent = load_selected_agent_for_usb();
    let entries = fs::read_dir(&state_dir).ok()?;

    let mut best_source = String::new();
    let mut best_payload: Option<serde_json::Value> = None;
    let mut best_state_score = i32::MIN;
    let mut best_source_score = i32::MIN;
    let mut best_ts_ms = 0u64;

    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let raw_source = match path.file_stem().and_then(|s| s.to_str()) {
            Some(s) => s,
            None => continue,
        };
        let source = normalize_agent_id(raw_source).unwrap_or_else(|| raw_source.to_string());
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
    let entries = fs::read_dir(&state_dir).ok()?;

    let mut best_payload: Option<serde_json::Value> = None;
    let mut best_ts_ms = 0u64;

    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let raw_source = match path.file_stem().and_then(|s| s.to_str()) {
            Some(s) => s,
            None => continue,
        };
        let normalized_source =
            normalize_agent_id(raw_source).unwrap_or_else(|| raw_source.to_string());
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
    let cached = load_last_usb_active_state();
    let same_source_non_speaking = cached
        .as_ref()
        .and_then(|(source, _)| pick_usb_bridge_state_for_source(source, true));
    let same_source_any = cached
        .as_ref()
        .and_then(|(source, _)| pick_usb_bridge_state_for_source(source, false));

    let (source, payload) = match same_source_non_speaking
        .or(same_source_any)
        .or(cached)
        .or_else(|| pick_best_usb_bridge_state(true))
        .or_else(|| pick_best_usb_bridge_state(false))
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

fn forward_usb_speech_updates(
    usb_manager: &usb_serial::UsbSerialManager,
    speech_dir: &Path,
    selected_agent: &Option<String>,
    enabled_agents: &std::collections::HashSet<String>,
    last_speech_signatures: &mut std::collections::HashMap<String, String>,
    now_ms: u64,
) {
    let entries = match fs::read_dir(speech_dir) {
        Ok(entries) => entries,
        Err(_) => return,
    };

    let mut seen_sources: std::collections::HashSet<String> = std::collections::HashSet::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let source = match path.file_stem().and_then(|s| s.to_str()) {
            Some(stem) => normalize_agent_id(stem).unwrap_or_else(|| stem.to_string()),
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

        if let Some(expires_at_ms) = payload.get("expiresAtMs").and_then(|value| value.as_u64()) {
            if expires_at_ms > 0 && expires_at_ms < now_ms {
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
        let mut was_usb_connected = false;

        loop {
            thread::sleep(Duration::from_millis(800));

            // Only forward when USB is connected
            let status = usb_manager.status();
            if !status.connected {
                if was_usb_connected {
                    last_source_signatures.clear();
                    last_speech_signatures.clear();
                    last_active_signature.clear();
                    last_active_speech_text.clear();
                    last_disabled_filter_signature.clear();
                }
                was_usb_connected = false;
                continue;
            }
            if !was_usb_connected {
                last_source_signatures.clear();
                last_speech_signatures.clear();
                last_active_signature.clear();
                last_active_speech_text.clear();
                last_disabled_filter_signature.clear();
                was_usb_connected = true;
            }

            let now_ms = current_timestamp_ms();
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
                &selected_agent,
                &enabled_agents,
                &mut last_speech_signatures,
                now_ms,
            );

            // Read all *.json files in state_dir
            let entries = match fs::read_dir(&state_dir) {
                Ok(e) => e,
                Err(_) => continue,
            };

            let mut seen_sources: std::collections::HashSet<String> =
                std::collections::HashSet::new();
            let mut best_state_score = i32::MIN;
            let mut best_source_score = i32::MIN;
            let mut best_ts_ms = 0u64;
            let mut best_source = String::new();
            let mut best_payload: Option<serde_json::Value> = None;
            let mut best_signature = String::new();

            for entry in entries.flatten() {
                let path = entry.path();
                if path.extension().and_then(|e| e.to_str()) != Some("json") {
                    continue;
                }
                let raw_source = match path.file_stem().and_then(|s| s.to_str()) {
                    Some(s) => s,
                    None => continue,
                };
                let source =
                    normalize_agent_id(raw_source).unwrap_or_else(|| raw_source.to_string());
                if !usb_source_allowed_by_follow(&source, &selected_agent, &enabled_agents) {
                    continue;
                }
                let content = match fs::read_to_string(&path) {
                    Ok(c) => c.trim().to_string(),
                    Err(_) => continue,
                };

                // Parse and forward via USB — device-side does its own state normalization
                if let Ok(payload) = serde_json::from_str::<serde_json::Value>(&content) {
                    if !usb_state_payload_is_fresh(&path, &payload, now_ms) {
                        continue;
                    }

                    let ts_ms = payload
                        .get("tsMs")
                        .and_then(|value| value.as_u64())
                        .unwrap_or(0);
                    let compact_payload = compact_usb_state_payload(&source, &payload);
                    let compact_signature =
                        serde_json::to_string(&compact_payload).unwrap_or_else(|_| content.clone());
                    seen_sources.insert(source.clone());

                    if last_source_signatures.get(&source).map(|s| s.as_str())
                        != Some(compact_signature.as_str())
                    {
                        match usb_manager.send_state(&source, &compact_payload) {
                            Ok(_) => {
                                last_source_signatures
                                    .insert(source.clone(), compact_signature.clone());
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
                                            Err(e) => eprintln!(
                                                "[usb-forwarder] send_speech error: {}",
                                                e
                                            ),
                                        }
                                    }
                                }
                            }
                            Err(e) => eprintln!("[usb-forwarder] send_state error: {}", e),
                        }
                    }

                    let state = payload
                        .get("state")
                        .and_then(|value| value.as_str())
                        .unwrap_or("")
                        .to_ascii_lowercase();

                    let current_state_score = score_usb_state(&state);
                    let current_source_score = score_usb_source(&source);

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

/// Background thread: auto-connect USB serial on startup and reconnect on disconnect.
fn start_usb_auto_connect(usb_manager: usb_serial::UsbSerialManager, app_handle: tauri::AppHandle) {
    thread::spawn(move || {
        // Wait for app to initialize
        thread::sleep(Duration::from_secs(2));
        loop {
            let status = usb_manager.status();
            if !status.connected {
                let devices = usb_manager.scan_devices();
                if let Some(dev) = devices.first() {
                    let port_name = dev.port_name.clone();
                    eprintln!("[usb-auto] connecting to {}", port_name);
                    let emitter = app_handle.clone();
                    let result = usb_manager.connect(&port_name, move |topic, payload| {
                        handle_incoming_usb_message(&emitter, topic, payload);
                    });
                    match result {
                        Ok(_) => eprintln!("[usb-auto] connected to {}", port_name),
                        Err(e) => eprintln!("[usb-auto] connect failed: {}", e),
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
    start_usb_state_forwarder(usb_manager.clone());

    let usb_for_auto = usb_manager.clone();

    let app = tauri::Builder::default()
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_http::init())
        .setup(move |app| {
            let handle = app.handle().clone();
            start_usb_auto_connect(usb_for_auto, handle.clone());
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
            usb_get_status,
            usb_set_screen_page,
            usb_apply_wifi,
            usb_sync_appearance,
            resolve_builtin_clawpkg_path,
            install_clawpkg_over_usb,
            install_clawpkg_over_ssh,
            install_widget_skill,
            list_component_drafts,
            delete_component_draft,
            launch_agent_with_prompt
        ])
        .build(tauri::generate_context!())
        .expect("error while building pet-manager tauri application");

    app.run(|_app_handle, event| {
        if let RunEvent::Exit | RunEvent::ExitRequested { .. } = event {
            stop_background_runtimes_on_exit();
        }
    });
}

fn stop_background_runtimes_on_exit() {
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
    fs::create_dir_all(&logs_dir).map_err(|error| error.to_string())?;

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
    config_path: &PathBuf,
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
        _ => DEFAULT_PET_CHANNEL_ID.to_string(),
    }
}

fn normalize_pet_channel_id(value: String) -> String {
    match value.trim().to_lowercase().as_str() {
        "codex" => "codex".to_string(),
        "claude" => "claude".to_string(),
        "openclaw" => "openclaw".to_string(),
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
    fs::create_dir_all(&logs_dir).map_err(|error| error.to_string())?;

    Ok(VoiceRuntimePaths {
        log_path: logs_dir.join(VOICE_SERVICE_LOG_FILE_NAME),
        pid_path: config_dir.join(VOICE_SERVICE_PID_FILE_NAME),
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
    if let Some(resource_dir) = app_handle.path().resource_dir().ok() {
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

    // Production escape hatch (HEAD/voice): explicit env-var override for
    // pointing at a custom Node toolchain. The bundled bridge/runtime/node is
    // Node v22 LTS, which the native bindings used by voice-service-node
    // (@discordjs/opus, @livekit/rtc-node) are pre-built for. Override is
    // mainly useful for debugging against a different Node build.
    if let Some(override_path) = env::var_os("PET_MANAGER_NODE_BIN") {
        let path = PathBuf::from(override_path);
        if path.is_file() {
            return Ok(path);
        }
    }

    // Debug-only fallback (main): try $PATH for `node` so devs can iterate
    // without re-bundling. In release builds we ALWAYS prefer the pinned
    // bundled Node to keep ABI compat with native voice bindings.
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

    // Debug fallback: relative to CARGO_MANIFEST_DIR.
    #[cfg(debug_assertions)]
    {
        let dev_bundled = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("bridge/runtime")
            .join(node_name);
        if dev_bundled.is_file() {
            return Ok(dev_bundled);
        }
    }

    // Last-resort PATH lookup so dev sessions without the bundled runtime
    // (e.g. fresh checkout that hasn't downloaded bridge/runtime/node) can
    // still launch voice-service-node and the bridge against any system node.
    // Use our cross-platform resolver instead of `which`, which is not
    // available on a stock Windows install.
    if let Some(path) = find_executable(node_name, &[]) {
        return Ok(PathBuf::from(path));
    }

    Err("未找到可用的 Node.js（bridge/runtime/node 或 PATH 中的 node）。".to_string())
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
        "#!/bin/sh\nset -eu\nmkdir -p {logs_dir}\ncd {resource_root}\nexport NODE_PATH={node_modules}${{NODE_PATH:+:$NODE_PATH}}\nexport VOICE_SERVICE_HOST={host}\nexport VOICE_SERVICE_PORT={port}\nexport VOICE_SERVICE_CORS_ORIGINS='*'\n{env_exports}\nexec {node_path} {entry_path} >> {log_path} 2>&1\n",
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

    fs::write(script_path, script).map_err(|error| error.to_string())?;
    #[cfg(unix)]
    {
        let permissions = fs::Permissions::from_mode(0o755);
        fs::set_permissions(script_path, permissions).map_err(|error| error.to_string())?;
    }
    Ok(())
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
    let script = format!(
        "#!/bin/sh\nset -eu\nmkdir -p {logs_dir}\nBRIDGE_ROOT=''\nfor candidate in {bridge_root_candidates}; do\n  if [ -f \"$candidate/{entry_relative_path}\" ]; then\n    BRIDGE_ROOT=\"$candidate\"\n    break\n  fi\ndone\nif [ -z \"$BRIDGE_ROOT\" ]; then\n  printf '%s\\n' 'bridge resources not found in any detected local path' >> {log_path}\n  exit 1\nfi\ncd \"$BRIDGE_ROOT/{workspace_relative_path}\"\nexport NODE_PATH=\"$BRIDGE_ROOT/node_modules${{NODE_PATH:+:$NODE_PATH}}\"\nexport MQTT_URL={mqtt_url}\nexport MQTT_USERNAME={mqtt_username}\nexport MQTT_PASSWORD={mqtt_password}\nexport STATUS_NAMESPACE={namespace}\nexport STATUS_DEVICE_ID={device_id}\nexport STATUS_BRIDGE_LOCAL_STATE_DIR={local_state_dir}\nexport CLAWD_BRIDGE_PORT={bridge_port}\nexport CLAWD_ENABLED_AGENTS={enabled_agents}\nexport CLAWD_SELECTED_AGENT_ID={selected_agent_id}\nexport CLAWD_ENABLE_CLAUDE_LOG_MONITOR={claude_enabled}\nexport CLAWD_SYNC_HOOKS={claude_enabled}\nexport CLAWD_ENABLE_CODEX_MONITOR={codex_enabled}\nexport CLAWD_CODEX_SESSION_DIR={codex_session_dir}\nexport OPENCLAW_ENABLE={openclaw_enabled}\nexec {node_path} \"$BRIDGE_ROOT/{entry_relative_path}\" >> {log_path} 2>&1\n",
        logs_dir = shell_quote(
            log_path
                .parent()
                .and_then(|path| path.to_str())
                .unwrap_or("")
        ),
        bridge_root_candidates = bridge_root_candidates,
        workspace_relative_path = BRIDGE_WORKSPACE_RELATIVE_PATH,
        entry_relative_path = BRIDGE_ENTRY_RELATIVE_PATH,
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
        bridge_port = shell_quote(&DEFAULT_BRIDGE_PORT.to_string()),
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
        node_path = shell_quote(node_path.to_string_lossy().as_ref()),
        log_path = shell_quote(log_path.to_string_lossy().as_ref()),
    );

    fs::write(script_path, script).map_err(|error| error.to_string())?;
    #[cfg(unix)]
    {
        let permissions = fs::Permissions::from_mode(0o755);
        fs::set_permissions(script_path, permissions).map_err(|error| error.to_string())?;
    }
    Ok(())
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
    <string>/bin/sh</string>
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
    let node_modules = bridge_assets.resource_root.join("node_modules");
    let error_log_path = runtime_paths.log_path.with_extension("error.log");
    format!(
        "$ErrorActionPreference = 'Stop'\r\n\
New-Item -ItemType Directory -Force -Path {logs_dir} | Out-Null\r\n\
$env:NODE_PATH = {node_modules}\r\n\
$env:MQTT_URL = {mqtt_url}\r\n\
$env:MQTT_USERNAME = {mqtt_username}\r\n\
$env:MQTT_PASSWORD = {mqtt_password}\r\n\
$env:STATUS_NAMESPACE = {namespace}\r\n\
$env:STATUS_DEVICE_ID = {device_id}\r\n\
$env:STATUS_BRIDGE_LOCAL_STATE_DIR = {local_state_dir}\r\n\
$env:CLAWD_BRIDGE_PORT = {bridge_port}\r\n\
$env:CLAWD_ENABLED_AGENTS = {enabled_agents}\r\n\
$env:CLAWD_SELECTED_AGENT_ID = {selected_agent_id}\r\n\
$env:CLAWD_ENABLE_CLAUDE_LOG_MONITOR = {claude_enabled}\r\n\
$env:CLAWD_SYNC_HOOKS = {claude_enabled}\r\n\
$env:CLAWD_ENABLE_CODEX_MONITOR = {codex_enabled}\r\n\
$env:CLAWD_CODEX_SESSION_DIR = {codex_session_dir}\r\n\
$env:OPENCLAW_ENABLE = {openclaw_enabled}\r\n\
Set-Location -LiteralPath {working_dir}\r\n\
$nodePath = {node_path}\r\n\
$entryPath = {entry_path}\r\n\
$entryArg = '\"' + $entryPath + '\"'\r\n\
Start-Process -WindowStyle Hidden -FilePath $nodePath -ArgumentList $entryArg -WorkingDirectory {working_dir} -RedirectStandardOutput {log_path} -RedirectStandardError {error_log_path}\r\n",
        logs_dir = powershell_quote(
            runtime_paths
                .log_path
                .parent()
                .unwrap_or(runtime_paths.config_dir.as_path())
                .to_string_lossy()
                .as_ref()
        ),
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
        working_dir = powershell_path_quote(&bridge_assets.workspace_root),
        node_path = powershell_path_quote(node_path),
        entry_path = powershell_path_quote(&bridge_assets.entry_path),
        log_path = powershell_path_quote(&runtime_paths.log_path),
        error_log_path = powershell_path_quote(&error_log_path),
    )
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
        let _ = (script_path, log_path, pid_path);
        Err("Windows 不支持 shell 脚本启动方式，请使用 start_bridge_direct".to_string())
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

    let stdout = OpenOptions::new()
        .create(true)
        .append(true)
        .open(log_path)
        .map_err(|error| error.to_string())?;
    let stderr = stdout.try_clone().map_err(|error| error.to_string())?;

    // executable_path here is the entry .mjs (resolve_voice_service_assets
    // populated it that way). We invoke the bundled node against it.
    let node_modules = voice_assets.resource_root.join("node_modules");
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

    let mut command = command_for_host(node_path);
    command.arg(&voice_assets.executable_path);
    command.current_dir(&voice_assets.resource_root);
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
    let pid = child.id();
    fs::write(pid_path, format!("{pid}\n")).map_err(|error| error.to_string())?;
    Ok(pid)
}

/// Spawn node directly with the correct env vars, bypassing the shell script.
/// This avoids macOS Permission denied errors when the Tauri app tries to
/// execute /bin/sh with an external script.
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

    let stdout = OpenOptions::new()
        .create(true)
        .append(true)
        .open(log_path)
        .map_err(|error| error.to_string())?;
    let stderr = stdout.try_clone().map_err(|error| error.to_string())?;

    let node_modules = bridge_assets.resource_root.join("node_modules");
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

    let mut command = command_for_host(node_path);
    command.arg(&bridge_assets.entry_path);
    command.current_dir(&bridge_assets.workspace_root);
    command.env("NODE_PATH", node_path_env);
    command.env("MQTT_URL", &profile.mqtt_url);
    command.env("MQTT_USERNAME", &profile.mqtt_username);
    command.env("MQTT_PASSWORD", &profile.mqtt_password);
    command.env("STATUS_NAMESPACE", &profile.mqtt_namespace);
    command.env("STATUS_DEVICE_ID", &profile.desktop_device_id);
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
    command.stdin(Stdio::null());
    command.stdout(Stdio::from(stdout));
    command.stderr(Stdio::from(stderr));
    #[cfg(unix)]
    {
        command.process_group(0);
    }
    let child = command.spawn().map_err(|error| error.to_string())?;
    let pid = child.id();
    fs::write(pid_path, format!("{pid}\n")).map_err(|error| error.to_string())?;
    Ok(pid)
}

fn start_bridge_via_sh(
    script_path: &Path,
    log_path: &Path,
    pid_path: &Path,
) -> Result<u32, String> {
    if let Some(parent) = log_path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }

    let stdout = OpenOptions::new()
        .create(true)
        .append(true)
        .open(log_path)
        .map_err(|error| error.to_string())?;
    let stderr = stdout.try_clone().map_err(|error| error.to_string())?;

    let mut command = command_for_host("/bin/sh");
    command.arg(script_path);
    command.stdin(Stdio::null());
    command.stdout(Stdio::from(stdout));
    command.stderr(Stdio::from(stderr));
    #[cfg(unix)]
    {
        command.process_group(0);
    }

    let child = command.spawn().map_err(|error| error.to_string())?;
    let pid = child.id();
    fs::write(pid_path, format!("{pid}\n")).map_err(|error| error.to_string())?;
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
        if probe_bridge_running(port) {
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

    #[cfg(target_os = "macos")]
    {
        if let Ok(home_dir) = get_home_dir() {
            push_unique_path(
                &mut candidates,
                home_dir
                    .join("Applications")
                    .join(PET_MANAGER_APP_BUNDLE_NAME)
                    .join("Contents")
                    .join("Resources")
                    .join(BRIDGE_RESOURCE_ROOT),
            );
        }

        push_unique_path(
            &mut candidates,
            PathBuf::from("/Applications")
                .join(PET_MANAGER_APP_BUNDLE_NAME)
                .join("Contents")
                .join("Resources")
                .join(BRIDGE_RESOURCE_ROOT),
        );
    }

    #[cfg(target_os = "windows")]
    {
        // Windows installed app paths (NSIS/MSI default install locations)
        if let Some(local_app_data) = env::var_os("LOCALAPPDATA") {
            push_unique_path(
                &mut candidates,
                PathBuf::from(&local_app_data)
                    .join("Pet Manager")
                    .join(BRIDGE_RESOURCE_ROOT),
            );
        }
        if let Some(program_files) = env::var_os("PROGRAMFILES") {
            push_unique_path(
                &mut candidates,
                PathBuf::from(&program_files)
                    .join("Pet Manager")
                    .join(BRIDGE_RESOURCE_ROOT),
            );
        }
    }

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
        return Ok(Some(
            get_home_dir()?
                .join("Library")
                .join("LaunchAgents")
                .join(format!("{BRIDGE_LAUNCH_AGENT_LABEL}.plist")),
        ));
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
        return Ok(Some(startup_dir.join(BRIDGE_WINDOWS_STARTUP_SCRIPT_NAME)));
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
fn powershell_path_quote(path: &Path) -> String {
    powershell_quote(&windows_powershell_path(path.to_string_lossy().as_ref()))
}

#[cfg(target_os = "windows")]
fn cmd_quote_path(path: &Path) -> String {
    format!("\"{}\"", path.display().to_string().replace('"', "\"\""))
}

fn agent_cli_binary(agent_id: &str) -> Option<&'static str> {
    match normalize_agent_id(agent_id)?.as_str() {
        "codex" => Some("codex"),
        "claude-code" => Some("claude"),
        "openclaw" => Some("openclaw"),
        _ => None,
    }
}

fn find_agent_cli_executable(agent_id: &str) -> Option<String> {
    let home = get_home_dir().ok();
    match normalize_agent_id(agent_id)?.as_str() {
        "codex" => find_executable(if cfg!(windows) { "codex.cmd" } else { "codex" }, &[]),
        "claude-code" => find_agent_executable(
            "claude",
            home.as_deref(),
            &[".local/bin/claude", ".claude/local/claude"],
            "claude.exe",
        ),
        "openclaw" => find_agent_executable(
            "openclaw",
            home.as_deref(),
            &[".local/bin/openclaw", ".npm-global/bin/openclaw"],
            "openclaw.exe",
        ),
        _ => None,
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct LaunchAgentPromptInput {
    agent_id: String,
    prompt: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct LaunchAgentPromptResult {
    ok: bool,
    work_dir: String,
    prompt_file: String,
}

#[tauri::command]
async fn launch_agent_with_prompt(
    input: LaunchAgentPromptInput,
) -> Result<LaunchAgentPromptResult, String> {
    let bin_name = agent_cli_binary(&input.agent_id)
        .ok_or_else(|| format!("暂不支持的 agent: {}", input.agent_id))?;
    let agent_label = match normalize_agent_id(&input.agent_id).as_deref() {
        Some("codex") => "Codex",
        Some("claude-code") => "Claude Code",
        Some("openclaw") => "OpenClaw",
        _ => &input.agent_id,
    };
    let bin = find_agent_cli_executable(&input.agent_id).ok_or_else(|| {
        format!(
            "当前跟随的是 {agent_label}，但没有检测到可从终端启动的 `{bin_name}` CLI。组件生成需要命令行版 agent；请安装 CLI，或切换到已安装 CLI 的 Claude Code / OpenClaw 后重试。"
        )
    })?;
    tauri::async_runtime::spawn_blocking(move || -> Result<LaunchAgentPromptResult, String> {
        let ts = current_timestamp_ms();
        let work_dir = component_drafts_root()?.join(ts.to_string());
        fs::create_dir_all(&work_dir).map_err(|e| e.to_string())?;
        let prompt_file = work_dir.join("PROMPT.md");
        fs::write(&prompt_file, &input.prompt).map_err(|e| e.to_string())?;
        launch_agent_prompt_terminal(&input.agent_id, &work_dir, &prompt_file, &bin)?;
        Ok(LaunchAgentPromptResult {
            ok: true,
            work_dir: work_dir.display().to_string(),
            prompt_file: prompt_file.display().to_string(),
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[cfg(target_os = "macos")]
fn launch_agent_prompt_terminal(
    agent_id: &str,
    work_dir: &Path,
    _prompt_file: &Path,
    bin: &str,
) -> Result<(), String> {
    /* Build a sh script that (a) cd's into the workdir and (b) execs the
    agent CLI with the user-prompt as a single argv element. We deliberately
    avoid embedding the prompt body inline (which is untrusted free-text from
    the user) and instead read it from PROMPT.md into a shell variable, then
    pass the variable as a quoted argument. The shell variable read uses one-
    pass command substitution — the prompt body is NEVER re-evaluated by sh.

    Both `work_dir` and `bin` are escaped to defend against any future case
    where HOME or an executable path contains a `"`, `$`, `\` or backtick. */
    let runner = work_dir.join("run.command");
    let escaped_work_dir = shell_double_quote_escape(&work_dir.display().to_string());
    let escaped_bin = shell_double_quote_escape(bin);
    let script = if normalize_agent_id(agent_id).as_deref() == Some("codex") {
        format!(
            "#!/bin/sh\ncd \"{}\"\nRUST_LOG=error exec \"{}\" exec --skip-git-repo-check --dangerously-bypass-approvals-and-sandbox - < PROMPT.md\n",
            escaped_work_dir, escaped_bin
        )
    } else {
        format!(
            "#!/bin/sh\ncd \"{}\"\nPROMPT_TEXT=\"$(cat PROMPT.md)\"\nexec \"{}\" \"$PROMPT_TEXT\"\n",
            escaped_work_dir, escaped_bin
        )
    };
    fs::write(&runner, script).map_err(|e| e.to_string())?;
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(&runner, fs::Permissions::from_mode(0o755)).map_err(|e| e.to_string())?;
    let status = command_for_host("open")
        .arg(&runner)
        .status()
        .map_err(|e| e.to_string())?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("打开 macOS 终端失败 (exit {:?})", status.code()))
    }
}

#[cfg(target_os = "windows")]
fn launch_agent_prompt_terminal(
    agent_id: &str,
    work_dir: &Path,
    prompt_file: &Path,
    bin: &str,
) -> Result<(), String> {
    let runner = work_dir.join("run.ps1");
    let escaped_work_dir = powershell_single_quote_escape(&work_dir.display().to_string());
    let escaped_prompt_file = powershell_single_quote_escape(&prompt_file.display().to_string());
    let escaped_bin = powershell_single_quote_escape(bin);
    let script = if normalize_agent_id(agent_id).as_deref() == Some("codex") {
        let cmd_bin = cmd_double_quote_escape(bin);
        let cmd_prompt_file = cmd_double_quote_escape(&prompt_file.display().to_string());
        format!(
            "$ErrorActionPreference = 'Stop'\n\
[Console]::InputEncoding = [System.Text.Encoding]::UTF8\n\
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8\n\
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n\
$env:RUST_LOG = 'error'\n\
Set-Location -LiteralPath {escaped_work_dir}\n\
& $env:ComSpec /D /C \"chcp 65001 >NUL && \"\"{cmd_bin}\"\" exec --skip-git-repo-check --dangerously-bypass-approvals-and-sandbox - < \"\"{cmd_prompt_file}\"\"\"\n\
if ($LASTEXITCODE -ne $null -and $LASTEXITCODE -ne 0) {{ Write-Host \"`nAgent exited with code $LASTEXITCODE\" }}\n"
        )
    } else {
        format!(
        "$ErrorActionPreference = 'Stop'\n\
Set-Location -LiteralPath {escaped_work_dir}\n\
$PromptText = Get-Content -LiteralPath {escaped_prompt_file} -Raw -Encoding UTF8\n\
& {escaped_bin} $PromptText\n\
if ($LASTEXITCODE -ne $null -and $LASTEXITCODE -ne 0) {{ Write-Host \"`nAgent exited with code $LASTEXITCODE\" }}\n"
        )
    };
    write_powershell_script_utf8_bom(&runner, &script)?;
    let status = command_for_host("cmd")
        .arg("/C")
        .arg("start")
        .arg("")
        .arg("powershell.exe")
        .arg("-NoProfile")
        .arg("-ExecutionPolicy")
        .arg("Bypass")
        .arg("-NoExit")
        .arg("-File")
        .arg(&runner)
        .status()
        .map_err(|e| format!("启动 Windows PowerShell 失败: {e}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!(
            "启动 Windows PowerShell 失败 (exit {:?})",
            status.code()
        ))
    }
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
fn launch_agent_prompt_terminal(
    _agent_id: &str,
    _work_dir: &Path,
    _prompt_file: &Path,
    _bin: &str,
) -> Result<(), String> {
    Err("当前仅实现 macOS/Windows 终端启动".to_string())
}

/// Escape a string for safe insertion inside a sh `"..."` double-quoted context.
/// Inside `"..."`, sh treats `\`, `$`, `` ` `` and `"` specially. This escapes all
/// four so the inserted text becomes a literal.
fn shell_double_quote_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 4);
    for ch in s.chars() {
        match ch {
            '\\' | '"' | '$' | '`' => {
                out.push('\\');
                out.push(ch);
            }
            _ => out.push(ch),
        }
    }
    out
}

#[cfg(target_os = "windows")]
fn powershell_single_quote_escape(s: &str) -> String {
    format!("'{}'", s.replace('\'', "''"))
}

#[cfg(target_os = "windows")]
fn cmd_double_quote_escape(s: &str) -> String {
    s.replace('"', "\"\"")
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
        assert_eq!(text, "Codex 工作中");
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
    fn disabled_usb_sources_include_known_non_enabled_on_startup() {
        let previous = std::collections::HashSet::new();
        let next = std::collections::HashSet::from(["claude-code".to_string()]);
        let mut disabled: Vec<String> = disabled_usb_sources_for_filter(&previous, &next)
            .into_iter()
            .collect();
        disabled.sort();

        assert_eq!(disabled, vec!["codex".to_string(), "openclaw".to_string()]);
    }

    #[test]
    fn agent_binary_maps_known_agents() {
        assert_eq!(agent_cli_binary("codex"), Some("codex"));
        assert_eq!(agent_cli_binary("claude-code"), Some("claude"));
        assert_eq!(agent_cli_binary("openclaw"), Some("openclaw"));
        assert_eq!(agent_cli_binary("unknown"), None);
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
        assert!(agent_home
            .join("skills/petAgent-ui-generator/SKILL.md")
            .exists());
        assert!(agent_home
            .join("skills/petAgent-ui-generator/references/notes.md")
            .exists());
    }

    #[test]
    fn install_skill_into_agent_overwrites_existing() {
        let tmp = tempfile::tempdir().unwrap();
        let src = tmp.path().join("source");
        std::fs::create_dir_all(&src).unwrap();
        std::fs::write(src.join("SKILL.md"), "new content").unwrap();

        let agent_home = tmp.path().join(".fake-agent");
        let existing_skill = agent_home.join("skills/petAgent-ui-generator");
        std::fs::create_dir_all(&existing_skill).unwrap();
        std::fs::write(existing_skill.join("stale.md"), "stale").unwrap();

        let entry = install_skill_into_agent(&src, &agent_home, "Fake Agent").unwrap();
        assert!(entry.overwrote);
        assert!(
            !existing_skill.join("stale.md").exists(),
            "stale file removed"
        );
        assert!(existing_skill.join("SKILL.md").exists(), "new file present");
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
        assert!(script.contains("Start-Process -WindowStyle Hidden -FilePath $nodePath -ArgumentList $entryArg"));
        assert!(script.contains(r"C:\Users\TestUser\AppData\Local\Pet Manager\bridge"));
        assert!(!script.contains(r"\\?\C:\Users"));
        assert!(!script.contains("CLAWD_CODEX_MODEL"));
        assert!(!script.contains("--model"));
    }

    #[test]
    fn component_draft_path_guard_accepts_nested_draft() {
        let tmp = tempfile::tempdir().unwrap();
        let drafts_root = tmp.path().join("component-drafts");
        let draft_roots = vec![drafts_root.clone()];
        let draft = drafts_root.join("run-1").join("timer-widget");
        std::fs::create_dir_all(&draft).unwrap();
        std::fs::write(draft.join("component.json"), "{}").unwrap();

        let resolved =
            canonicalize_component_draft_path(draft.to_str().unwrap(), &draft_roots).unwrap();

        assert_eq!(resolved, draft.canonicalize().unwrap());
    }

    #[test]
    fn component_draft_path_guard_rejects_outside_path() {
        let tmp = tempfile::tempdir().unwrap();
        let drafts_root = tmp.path().join("component-drafts");
        let draft_roots = vec![drafts_root.clone()];
        std::fs::create_dir_all(&drafts_root).unwrap();
        let outside = tmp.path().join("outside.clawpkg");
        std::fs::write(&outside, "not a draft").unwrap();

        let err =
            canonicalize_component_draft_path(outside.to_str().unwrap(), &draft_roots).unwrap_err();

        assert!(err.contains("只能删除 Pet Manager component-drafts"));
    }

    #[test]
    fn component_draft_path_guard_rejects_root_path() {
        let tmp = tempfile::tempdir().unwrap();
        let drafts_root = tmp.path().join("component-drafts");
        let draft_roots = vec![drafts_root.clone()];
        std::fs::create_dir_all(&drafts_root).unwrap();

        let err = canonicalize_component_draft_path(drafts_root.to_str().unwrap(), &draft_roots)
            .unwrap_err();

        assert!(err.contains("只能删除 Pet Manager component-drafts"));
    }
}
