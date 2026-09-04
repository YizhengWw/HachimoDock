/*
 * [Input] ESP32-P4 microphone PCM (16 kHz mono S16LE) and a Volcengine API key from environment, user storage, without embedded credentials.
 * [Output] User-config-first credential resolution, platform-specific persistence, and VAD-segmented streaming partial/final transcripts using the BigModel ASR WebSocket API.
 * [Pos] Tauri-side cloud ASR provider for device push-to-talk.
 */

use flate2::{read::GzDecoder, write::GzEncoder, Compression};
use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::env;
#[cfg(target_os = "macos")]
use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
#[cfg(target_os = "macos")]
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
#[cfg(target_os = "macos")]
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
#[cfg(target_os = "macos")]
use std::sync::OnceLock;
use std::time::{Duration, Instant};
use tokio::net::TcpStream;
use tokio::sync::mpsc;
use tokio::time::{sleep_until, timeout, Instant as TokioInstant};
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::tungstenite::http::HeaderValue;
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::{connect_async, MaybeTlsStream, WebSocketStream};
use uuid::Uuid;

pub const DEFAULT_ENDPOINT: &str = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";
pub const DEFAULT_RESOURCE_ID: &str = "volc.seedasr.sauc.duration";
const INTERNAL_ASR_API_KEY: Option<&str> = None;

#[cfg(windows)]
const KEYRING_SERVICE: &str = "claw-pet-manager";
#[cfg(windows)]
const KEYRING_USER: &str = "volcengine-device-asr";
#[cfg(target_os = "macos")]
const MACOS_CONFIG_FILE_NAME: &str = "volcengine-device-asr.json";
#[cfg(target_os = "macos")]
static MACOS_CONFIG_DIR: OnceLock<PathBuf> = OnceLock::new();
const CONNECT_TIMEOUT: Duration = Duration::from_secs(8);
const FINAL_TIMEOUT: Duration = Duration::from_secs(8);
const PROBE_TIMEOUT: Duration = Duration::from_secs(8);
const FINAL_SILENCE_PCM_BYTES: usize = 640;
const PROBE_SILENCE_PCM_BYTES: usize = 3_200;
const SUCCESS_CODE: i64 = 20_000_000;

const MSG_FULL_CLIENT_REQUEST: u8 = 0x1;
const MSG_AUDIO_ONLY_REQUEST: u8 = 0x2;
const MSG_FULL_SERVER_RESPONSE: u8 = 0x9;
const MSG_SERVER_ERROR: u8 = 0xf;

const FLAG_NO_SEQUENCE: u8 = 0x0;
const FLAG_FINAL_NO_SEQUENCE: u8 = 0x2;
const FLAG_POSITIVE_SEQUENCE: u8 = 0x1;
const FLAG_NEGATIVE_SEQUENCE: u8 = 0x3;

const SERIALIZATION_NONE: u8 = 0x0;
const SERIALIZATION_JSON: u8 = 0x1;
const COMPRESSION_NONE: u8 = 0x0;
const COMPRESSION_GZIP: u8 = 0x1;

type CloudSocket = WebSocketStream<MaybeTlsStream<TcpStream>>;
type SpeechCallback = Arc<dyn Fn(StreamingSpeechEvent) + Send + Sync>;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct StoredAsrConfig {
    version: u32,
    api_key: String,
    resource_id: String,
}

#[derive(Debug, Clone)]
struct RuntimeAsrConfig {
    api_key: String,
    resource_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceAsrSettingsInput {
    pub api_key: Option<String>,
    pub resource_id: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceAsrSettingsStatus {
    pub configured: bool,
    pub deferred: bool,
    pub provider: &'static str,
    pub mode: &'static str,
    pub endpoint: &'static str,
    pub resource_id: String,
    pub credential_source: &'static str,
    pub message: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceAsrProbeStatus {
    pub ok: bool,
    pub provider: &'static str,
    pub resource_id: String,
    pub latency_ms: u128,
    pub log_id: String,
    pub message: String,
}

#[derive(Debug, Clone)]
pub enum StreamingSpeechEvent {
    Ready,
    Partial {
        revision: u64,
        text: String,
        confidence: Option<f64>,
    },
    Final {
        revision: u64,
        text: String,
        confidence: Option<f64>,
    },
    Error(String),
}

enum StreamingSpeechCommand {
    Pcm(Vec<u8>),
    Finish,
    Cancel,
}

#[derive(Default)]
struct PendingAudioPacket {
    pcm: Option<Vec<u8>>,
}

impl PendingAudioPacket {
    fn push(&mut self, pcm: Vec<u8>) -> Option<Vec<u8>> {
        self.pcm.replace(pcm)
    }

    fn finish(&mut self) -> Vec<u8> {
        self.pcm
            .take()
            .unwrap_or_else(|| vec![0_u8; FINAL_SILENCE_PCM_BYTES])
    }
}

pub struct StreamingSpeechRecognizer {
    sender: mpsc::UnboundedSender<StreamingSpeechCommand>,
    cancelled: Arc<AtomicBool>,
    finished: Arc<AtomicBool>,
}

impl StreamingSpeechRecognizer {
    pub fn start(
        callback: impl Fn(StreamingSpeechEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        let config = load_runtime_config()?;
        let callback: SpeechCallback = Arc::new(callback);
        let (sender, receiver) = mpsc::unbounded_channel();
        let cancelled = Arc::new(AtomicBool::new(false));
        let worker_cancelled = cancelled.clone();
        let worker_callback = callback.clone();

        tauri::async_runtime::spawn(async move {
            if let Err(error) = run_streaming_session(
                config,
                receiver,
                worker_callback.clone(),
                worker_cancelled.clone(),
            )
            .await
            {
                if !worker_cancelled.load(Ordering::SeqCst) {
                    worker_callback(StreamingSpeechEvent::Error(error));
                }
            }
        });

        Ok(Self {
            sender,
            cancelled,
            finished: Arc::new(AtomicBool::new(false)),
        })
    }

    pub fn push_pcm(&self, pcm: &[u8]) -> Result<(), String> {
        if self.cancelled.load(Ordering::SeqCst) {
            return Err("cloud speech recognition was cancelled".to_string());
        }
        if self.finished.load(Ordering::SeqCst) {
            return Err("cloud speech recognition is already finishing".to_string());
        }
        if pcm.is_empty() {
            return Ok(());
        }
        self.sender
            .send(StreamingSpeechCommand::Pcm(pcm.to_vec()))
            .map_err(|_| "cloud speech recognition input is closed".to_string())
    }

    pub fn finish(&self) -> Result<(), String> {
        if self.finished.swap(true, Ordering::SeqCst) {
            return Ok(());
        }
        self.sender
            .send(StreamingSpeechCommand::Finish)
            .map_err(|_| "cloud speech recognition input is closed".to_string())
    }

    pub fn cancel(&self) {
        if self.cancelled.swap(true, Ordering::SeqCst) {
            return;
        }
        let _ = self.sender.send(StreamingSpeechCommand::Cancel);
    }
}

impl Drop for StreamingSpeechRecognizer {
    fn drop(&mut self) {
        self.cancel();
    }
}

pub fn settings_status() -> Result<DeviceAsrSettingsStatus, String> {
    if let Some(config) = runtime_config_from_env() {
        return Ok(public_status(
            true,
            config.resource_id,
            "environment",
            "火山引擎云端 ASR 已通过环境变量配置",
        ));
    }

    if let Some(config) = read_stored_config()? {
        if !config.api_key.trim().is_empty() {
            return Ok(public_status(
                true,
                normalize_resource_id(&config.resource_id)?,
                credential_source_name(),
                "火山引擎云端 ASR 已配置",
            ));
        }
    }

    match runtime_config_from_internal_build() {
        Some(config) => Ok(public_status(
            true,
            config.resource_id,
            "embedded-internal-build",
            "火山引擎云端 ASR 已由内部版本预配置",
        )),
        None => Ok(public_status(
            false,
            DEFAULT_RESOURCE_ID.to_string(),
            "none",
            "请配置火山引擎语音识别 API Key",
        )),
    }
}

pub fn settings_status_for_automatic_restore() -> Result<DeviceAsrSettingsStatus, String> {
    settings_status()
}

pub fn save_settings(input: DeviceAsrSettingsInput) -> Result<DeviceAsrSettingsStatus, String> {
    let existing = read_stored_config()?;
    let api_key = input
        .api_key
        .as_deref()
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
        .or_else(|| existing.as_ref().map(|value| value.api_key.clone()))
        .or_else(|| runtime_config_from_internal_build().map(|value| value.api_key))
        .ok_or_else(|| "请填写火山引擎语音识别 API Key".to_string())?;
    let resource_id = input
        .resource_id
        .as_deref()
        .or_else(|| existing.as_ref().map(|value| value.resource_id.as_str()))
        .unwrap_or(DEFAULT_RESOURCE_ID);
    let resource_id = normalize_resource_id(resource_id)?;

    write_stored_config(&StoredAsrConfig {
        version: 1,
        api_key,
        resource_id: resource_id.clone(),
    })?;

    Ok(public_status(
        true,
        resource_id,
        credential_source_name(),
        credential_saved_message(),
    ))
}

pub async fn probe_saved_settings() -> Result<DeviceAsrProbeStatus, String> {
    let config = load_runtime_config()?;
    let started = Instant::now();
    let (mut socket, log_id) = connect_cloud_socket(&config).await?;
    socket
        .send(Message::Binary(
            encode_full_client_request(&recognition_payload())?.into(),
        ))
        .await
        .map_err(|error| format!("火山引擎 ASR 请求发送失败: {error}"))?;

    // A short silent PCM packet verifies the selected ASR resource without
    // recording or uploading any real microphone content.
    socket
        .send(Message::Binary(
            encode_audio_request(&vec![0_u8; PROBE_SILENCE_PCM_BYTES], false)?.into(),
        ))
        .await
        .map_err(|error| format!("火山引擎 ASR 测试音频发送失败: {error}"))?;
    socket
        .send(Message::Binary(
            encode_audio_request(&vec![0_u8; FINAL_SILENCE_PCM_BYTES], true)?.into(),
        ))
        .await
        .map_err(|error| format!("火山引擎 ASR 结束帧发送失败: {error}"))?;

    let deadline = TokioInstant::now() + PROBE_TIMEOUT;
    loop {
        let message = timeout_at_message(deadline, &mut socket).await?;
        match message {
            Message::Binary(data) => {
                let frame = parse_server_frame(data.as_ref())?;
                if frame.message_type == MSG_SERVER_ERROR {
                    return Err(format_server_error(&frame, &log_id));
                }
                if frame.message_type == MSG_FULL_SERVER_RESPONSE {
                    validate_response_payload(&frame.payload)?;
                    let _ = socket.close(None).await;
                    return Ok(DeviceAsrProbeStatus {
                        ok: true,
                        provider: "volcengine",
                        resource_id: config.resource_id,
                        latency_ms: started.elapsed().as_millis(),
                        log_id,
                        message: "火山引擎云端 ASR 连接与鉴权成功".to_string(),
                    });
                }
            }
            Message::Close(_) => return Err("火山引擎 ASR 在测试完成前关闭了连接".to_string()),
            Message::Ping(payload) => {
                socket
                    .send(Message::Pong(payload))
                    .await
                    .map_err(|error| format!("火山引擎 ASR 心跳响应失败: {error}"))?;
            }
            _ => {}
        }
    }
}

fn public_status(
    configured: bool,
    resource_id: String,
    credential_source: &'static str,
    message: impl Into<String>,
) -> DeviceAsrSettingsStatus {
    DeviceAsrSettingsStatus {
        configured,
        deferred: false,
        provider: "volcengine",
        mode: "bigmodel-async-device-pcm",
        endpoint: DEFAULT_ENDPOINT,
        resource_id,
        credential_source,
        message: message.into(),
    }
}

fn normalize_resource_id(value: &str) -> Result<String, String> {
    let value = value.trim();
    if value.is_empty() {
        return Ok(DEFAULT_RESOURCE_ID.to_string());
    }
    if value.len() > 128 || !value.starts_with("volc.") || value.chars().any(char::is_whitespace) {
        return Err("火山引擎 ASR Resource ID 格式无效".to_string());
    }
    Ok(value.to_string())
}

fn runtime_config_from_env() -> Option<RuntimeAsrConfig> {
    let api_key = env::var("VOLCENGINE_ASR_API_KEY").ok()?;
    let api_key = api_key.trim();
    if api_key.is_empty() {
        return None;
    }
    let resource_id = env::var("VOLCENGINE_ASR_RESOURCE_ID")
        .ok()
        .and_then(|value| normalize_resource_id(&value).ok())
        .unwrap_or_else(|| DEFAULT_RESOURCE_ID.to_string());
    Some(RuntimeAsrConfig {
        api_key: api_key.to_string(),
        resource_id,
    })
}

fn runtime_config_from_internal_build() -> Option<RuntimeAsrConfig> {
    runtime_config_from_embedded_key(INTERNAL_ASR_API_KEY)
}

fn runtime_config_from_embedded_key(api_key: Option<&str>) -> Option<RuntimeAsrConfig> {
    let api_key = api_key?.trim();
    if api_key.is_empty() {
        return None;
    }
    Some(RuntimeAsrConfig {
        api_key: api_key.to_string(),
        resource_id: DEFAULT_RESOURCE_ID.to_string(),
    })
}

fn load_runtime_config() -> Result<RuntimeAsrConfig, String> {
    if let Some(config) = runtime_config_from_env() {
        return Ok(config);
    }
    if let Some(config) = read_stored_config()? {
        let api_key = config.api_key.trim();
        if !api_key.is_empty() {
            return Ok(RuntimeAsrConfig {
                api_key: api_key.to_string(),
                resource_id: normalize_resource_id(&config.resource_id)?,
            });
        }
    }
    runtime_config_from_internal_build().ok_or_else(|| "未配置火山引擎语音识别 API Key".to_string())
}

#[cfg(target_os = "macos")]
pub fn configure_storage_dir(path: PathBuf) -> Result<(), String> {
    match MACOS_CONFIG_DIR.set(path.clone()) {
        Ok(()) => Ok(()),
        Err(_) if MACOS_CONFIG_DIR.get() == Some(&path) => Ok(()),
        Err(_) => Err("macOS ASR 本地配置目录已初始化为其他路径".to_string()),
    }
}

#[cfg(windows)]
fn credential_entry() -> Result<keyring::Entry, String> {
    keyring::Entry::new(KEYRING_SERVICE, KEYRING_USER)
        .map_err(|error| format!("无法访问{}: {error}", credential_store_label()))
}

#[cfg(windows)]
fn read_stored_config() -> Result<Option<StoredAsrConfig>, String> {
    match credential_entry()?.get_password() {
        Ok(value) => serde_json::from_str(&value)
            .map(Some)
            .map_err(|error| format!("火山引擎 ASR 安全配置损坏: {error}")),
        Err(keyring::Error::NoEntry) => Ok(None),
        Err(error) => Err(format!("无法读取{}: {error}", credential_store_label())),
    }
}

#[cfg(target_os = "macos")]
fn read_stored_config() -> Result<Option<StoredAsrConfig>, String> {
    read_macos_config_file(&macos_config_path()?)
}

#[cfg(not(any(windows, target_os = "macos")))]
fn read_stored_config() -> Result<Option<StoredAsrConfig>, String> {
    Ok(None)
}

#[cfg(windows)]
fn write_stored_config(config: &StoredAsrConfig) -> Result<(), String> {
    let value = serde_json::to_string(config)
        .map_err(|error| format!("无法序列化火山引擎 ASR 配置: {error}"))?;
    credential_entry()?
        .set_password(&value)
        .map_err(|error| format!("无法写入{}: {error}", credential_store_label()))
}

#[cfg(target_os = "macos")]
fn write_stored_config(config: &StoredAsrConfig) -> Result<(), String> {
    write_macos_config_file(&macos_config_path()?, config)
}

#[cfg(not(any(windows, target_os = "macos")))]
fn write_stored_config(_config: &StoredAsrConfig) -> Result<(), String> {
    Err("当前平台仅支持通过环境变量配置火山引擎 ASR".to_string())
}

#[cfg(target_os = "macos")]
fn macos_config_path() -> Result<PathBuf, String> {
    MACOS_CONFIG_DIR
        .get()
        .map(|directory| directory.join(MACOS_CONFIG_FILE_NAME))
        .ok_or_else(|| "macOS ASR 本地配置目录尚未初始化".to_string())
}

#[cfg(target_os = "macos")]
fn prepare_private_directory(directory: &Path) -> Result<(), String> {
    fs::create_dir_all(directory).map_err(|error| {
        format!(
            "无法创建 macOS ASR 本地配置目录 {}: {error}",
            directory.display()
        )
    })?;
    fs::set_permissions(directory, fs::Permissions::from_mode(0o700)).map_err(|error| {
        format!(
            "无法保护 macOS ASR 本地配置目录 {}: {error}",
            directory.display()
        )
    })
}

#[cfg(target_os = "macos")]
fn read_macos_config_file(path: &Path) -> Result<Option<StoredAsrConfig>, String> {
    let directory = path
        .parent()
        .ok_or_else(|| "macOS ASR 本地配置路径缺少父目录".to_string())?;
    prepare_private_directory(directory)?;
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => {
            return Err(format!(
                "无法检查 macOS ASR 本地配置 {}: {error}",
                path.display()
            ))
        }
    };
    if metadata.file_type().is_symlink() || !metadata.is_file() {
        return Err(format!(
            "macOS ASR 本地配置必须是普通文件: {}",
            path.display()
        ));
    }
    fs::set_permissions(path, fs::Permissions::from_mode(0o600))
        .map_err(|error| format!("无法保护 macOS ASR 本地配置 {}: {error}", path.display()))?;
    let value = fs::read_to_string(path)
        .map_err(|error| format!("无法读取 macOS ASR 本地配置 {}: {error}", path.display()))?;
    serde_json::from_str(&value)
        .map(Some)
        .map_err(|error| format!("火山引擎 ASR 本地配置损坏: {error}"))
}

#[cfg(target_os = "macos")]
fn write_macos_config_file(path: &Path, config: &StoredAsrConfig) -> Result<(), String> {
    let directory = path
        .parent()
        .ok_or_else(|| "macOS ASR 本地配置路径缺少父目录".to_string())?;
    prepare_private_directory(directory)?;

    let value = serde_json::to_vec(config)
        .map_err(|error| format!("无法序列化火山引擎 ASR 配置: {error}"))?;
    let temporary_path =
        directory.join(format!(".{MACOS_CONFIG_FILE_NAME}.{}.tmp", Uuid::new_v4()));
    let write_result = (|| -> Result<(), String> {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&temporary_path)
            .map_err(|error| {
                format!(
                    "无法创建 macOS ASR 临时配置 {}: {error}",
                    temporary_path.display()
                )
            })?;
        file.write_all(&value).map_err(|error| {
            format!(
                "无法写入 macOS ASR 临时配置 {}: {error}",
                temporary_path.display()
            )
        })?;
        file.sync_all().map_err(|error| {
            format!(
                "无法同步 macOS ASR 临时配置 {}: {error}",
                temporary_path.display()
            )
        })?;
        fs::rename(&temporary_path, path)
            .map_err(|error| format!("无法提交 macOS ASR 本地配置 {}: {error}", path.display()))?;
        fs::set_permissions(path, fs::Permissions::from_mode(0o600))
            .map_err(|error| format!("无法保护 macOS ASR 本地配置 {}: {error}", path.display()))
    })();
    if write_result.is_err() {
        let _ = fs::remove_file(&temporary_path);
    }
    write_result
}

#[cfg(windows)]
fn credential_store_label() -> &'static str {
    "Windows 凭据管理器"
}

#[cfg(windows)]
fn credential_source_name() -> &'static str {
    "windows-credential-manager"
}

#[cfg(target_os = "macos")]
fn credential_source_name() -> &'static str {
    "macos-private-file"
}

#[cfg(not(any(windows, target_os = "macos")))]
fn credential_source_name() -> &'static str {
    "environment"
}

#[cfg(target_os = "macos")]
fn credential_saved_message() -> &'static str {
    "火山引擎云端 ASR 配置已保存到当前用户的应用私有目录"
}

#[cfg(not(target_os = "macos"))]
fn credential_saved_message() -> &'static str {
    "火山引擎云端 ASR 配置已安全保存"
}

async fn run_streaming_session(
    config: RuntimeAsrConfig,
    mut receiver: mpsc::UnboundedReceiver<StreamingSpeechCommand>,
    callback: SpeechCallback,
    cancelled: Arc<AtomicBool>,
) -> Result<(), String> {
    let (mut socket, log_id) = connect_cloud_socket(&config).await?;
    socket
        .send(Message::Binary(
            encode_full_client_request(&recognition_payload())?.into(),
        ))
        .await
        .map_err(|error| format!("火山引擎 ASR 初始化请求发送失败: {error}"))?;
    callback(StreamingSpeechEvent::Ready);

    let mut transcript = TranscriptState::default();
    let mut final_deadline: Option<TokioInstant> = None;
    let mut pending_audio = PendingAudioPacket::default();

    loop {
        if let Some(deadline) = final_deadline {
            tokio::select! {
                message = socket.next() => {
                    if handle_server_message(&mut socket, message, &mut transcript, &callback, &log_id).await? {
                        return Ok(());
                    }
                }
                _ = sleep_until(deadline) => {
                    return Err(format!(
                        "等待火山引擎 ASR 最终结果超时{}",
                        log_id_suffix(&log_id)
                    ));
                }
            }
            continue;
        }

        tokio::select! {
            command = receiver.recv() => {
                match command {
                    Some(StreamingSpeechCommand::Pcm(pcm)) => {
                        if let Some(previous) = pending_audio.push(pcm) {
                            socket
                                .send(Message::Binary(
                                    encode_audio_request(&previous, false)?.into(),
                                ))
                                .await
                                .map_err(|error| format!("火山引擎 ASR 音频发送失败: {error}"))?;
                        }
                    }
                    Some(StreamingSpeechCommand::Finish) => {
                        let final_pcm = pending_audio.finish();
                        socket
                            .send(Message::Binary(encode_audio_request(&final_pcm, true)?.into()))
                            .await
                            .map_err(|error| format!("火山引擎 ASR 结束帧发送失败: {error}"))?;
                        final_deadline = Some(TokioInstant::now() + FINAL_TIMEOUT);
                    }
                    Some(StreamingSpeechCommand::Cancel) | None => {
                        cancelled.store(true, Ordering::SeqCst);
                        let _ = socket.close(None).await;
                        return Ok(());
                    }
                }
            }
            message = socket.next() => {
                if handle_server_message(&mut socket, message, &mut transcript, &callback, &log_id).await? {
                    return Ok(());
                }
            }
        }
    }
}

async fn connect_cloud_socket(config: &RuntimeAsrConfig) -> Result<(CloudSocket, String), String> {
    let request_id = Uuid::new_v4().to_string();
    let connect_id = Uuid::new_v4().to_string();
    let mut request = DEFAULT_ENDPOINT
        .into_client_request()
        .map_err(|error| format!("火山引擎 ASR 地址无效: {error}"))?;
    insert_header(&mut request, "X-Api-Key", &config.api_key)?;
    insert_header(&mut request, "X-Api-Resource-Id", &config.resource_id)?;
    insert_header(&mut request, "X-Api-Request-Id", &request_id)?;
    insert_header(&mut request, "X-Api-Sequence", "-1")?;
    insert_header(&mut request, "X-Api-Connect-Id", &connect_id)?;

    let connected = timeout(CONNECT_TIMEOUT, connect_async(request))
        .await
        .map_err(|_| "连接火山引擎 ASR 超时".to_string())?
        .map_err(|error| format!("连接火山引擎 ASR 失败: {error}"))?;
    let (socket, response) = connected;
    let log_id = response
        .headers()
        .get("X-Tt-Logid")
        .and_then(|value| value.to_str().ok())
        .unwrap_or_default()
        .to_string();
    Ok((socket, log_id))
}

fn insert_header(
    request: &mut tokio_tungstenite::tungstenite::http::Request<()>,
    name: &'static str,
    value: &str,
) -> Result<(), String> {
    let value = HeaderValue::from_str(value).map_err(|_| format!("{name} 包含无效字符"))?;
    request.headers_mut().insert(name, value);
    Ok(())
}

fn recognition_payload() -> Value {
    json!({
        "user": {
            "uid": Uuid::new_v4().to_string(),
        },
        "audio": {
            "format": "pcm",
            "codec": "raw",
            "rate": 16000,
            "bits": 16,
            "channel": 1,
        },
        "request": {
            "model_name": "bigmodel",
            "enable_nonstream": true,
            "enable_itn": true,
            "enable_punc": true,
            "enable_ddc": true,
            "show_utterances": true,
        },
    })
}

fn encode_full_client_request(payload: &Value) -> Result<Vec<u8>, String> {
    let payload = serde_json::to_vec(payload)
        .map_err(|error| format!("无法序列化火山引擎 ASR 请求: {error}"))?;
    let payload = gzip_compress(&payload)?;
    let mut frame = build_header(
        MSG_FULL_CLIENT_REQUEST,
        FLAG_NO_SEQUENCE,
        SERIALIZATION_JSON,
        COMPRESSION_GZIP,
    );
    frame.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    frame.extend_from_slice(&payload);
    Ok(frame)
}

fn encode_audio_request(audio: &[u8], final_packet: bool) -> Result<Vec<u8>, String> {
    let payload = gzip_compress(audio)?;
    let mut frame = build_header(
        MSG_AUDIO_ONLY_REQUEST,
        if final_packet {
            FLAG_FINAL_NO_SEQUENCE
        } else {
            FLAG_NO_SEQUENCE
        },
        SERIALIZATION_NONE,
        COMPRESSION_GZIP,
    );
    frame.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    frame.extend_from_slice(&payload);
    Ok(frame)
}

fn build_header(message_type: u8, flags: u8, serialization: u8, compression: u8) -> Vec<u8> {
    vec![
        0x11,
        (message_type << 4) | flags,
        (serialization << 4) | compression,
        0,
    ]
}

fn gzip_compress(payload: &[u8]) -> Result<Vec<u8>, String> {
    let mut encoder = GzEncoder::new(Vec::new(), Compression::default());
    encoder
        .write_all(payload)
        .map_err(|error| format!("火山引擎 ASR 数据压缩失败: {error}"))?;
    encoder
        .finish()
        .map_err(|error| format!("火山引擎 ASR 数据压缩失败: {error}"))
}

fn gzip_decompress(payload: &[u8]) -> Result<Vec<u8>, String> {
    let mut decoder = GzDecoder::new(payload);
    let mut decoded = Vec::new();
    decoder
        .read_to_end(&mut decoded)
        .map_err(|error| format!("火山引擎 ASR 响应解压失败: {error}"))?;
    Ok(decoded)
}

#[derive(Debug)]
struct ServerFrame {
    message_type: u8,
    flags: u8,
    sequence: Option<i32>,
    error_code: u32,
    payload: Vec<u8>,
}

impl ServerFrame {
    fn is_final(&self) -> bool {
        matches!(self.flags, FLAG_FINAL_NO_SEQUENCE | FLAG_NEGATIVE_SEQUENCE)
            || self.sequence.is_some_and(|sequence| sequence < 0)
    }
}

fn parse_server_frame(data: &[u8]) -> Result<ServerFrame, String> {
    if data.len() < 4 {
        return Err("火山引擎 ASR 响应帧过短".to_string());
    }
    let header_size = usize::from(data[0] & 0x0f) * 4;
    if header_size < 4 || data.len() < header_size {
        return Err("火山引擎 ASR 响应帧头无效".to_string());
    }

    let message_type = data[1] >> 4;
    let flags = data[1] & 0x0f;
    let compression = data[2] & 0x0f;
    let mut offset = header_size;
    let mut sequence = None;

    if matches!(flags, FLAG_POSITIVE_SEQUENCE | FLAG_NEGATIVE_SEQUENCE) {
        sequence = Some(read_i32(data, &mut offset, "sequence")?);
    }

    let (error_code, payload_size) = if message_type == MSG_SERVER_ERROR {
        let error_code = read_u32(data, &mut offset, "error code")?;
        let payload_size = read_u32(data, &mut offset, "error payload size")?;
        (error_code, payload_size)
    } else {
        (0, read_u32(data, &mut offset, "payload size")?)
    };
    let payload_size = payload_size as usize;
    let end = offset
        .checked_add(payload_size)
        .ok_or_else(|| "火山引擎 ASR 响应长度溢出".to_string())?;
    if end > data.len() {
        return Err("火山引擎 ASR 响应内容不完整".to_string());
    }
    let payload = match compression {
        COMPRESSION_NONE => data[offset..end].to_vec(),
        COMPRESSION_GZIP => gzip_decompress(&data[offset..end])?,
        other => return Err(format!("不支持的火山引擎 ASR 压缩格式: {other}")),
    };

    Ok(ServerFrame {
        message_type,
        flags,
        sequence,
        error_code,
        payload,
    })
}

fn read_u32(data: &[u8], offset: &mut usize, field: &str) -> Result<u32, String> {
    let end = offset
        .checked_add(4)
        .ok_or_else(|| format!("火山引擎 ASR {field} 偏移溢出"))?;
    let bytes: [u8; 4] = data
        .get(*offset..end)
        .ok_or_else(|| format!("火山引擎 ASR 响应缺少 {field}"))?
        .try_into()
        .map_err(|_| format!("火山引擎 ASR {field} 长度无效"))?;
    *offset = end;
    Ok(u32::from_be_bytes(bytes))
}

fn read_i32(data: &[u8], offset: &mut usize, field: &str) -> Result<i32, String> {
    Ok(read_u32(data, offset, field)? as i32)
}

async fn handle_server_message(
    socket: &mut CloudSocket,
    message: Option<Result<Message, tokio_tungstenite::tungstenite::Error>>,
    transcript: &mut TranscriptState,
    callback: &SpeechCallback,
    log_id: &str,
) -> Result<bool, String> {
    let message = message
        .ok_or_else(|| format!("火山引擎 ASR 连接已关闭{}", log_id_suffix(log_id)))?
        .map_err(|error| format!("火山引擎 ASR 接收失败: {error}{}", log_id_suffix(log_id)))?;

    match message {
        Message::Binary(data) => {
            let frame = parse_server_frame(data.as_ref())?;
            if frame.message_type == MSG_SERVER_ERROR {
                return Err(format_server_error(&frame, log_id));
            }
            if frame.message_type != MSG_FULL_SERVER_RESPONSE {
                return Ok(false);
            }
            let payload = validate_response_payload(&frame.payload)?;
            let text = extract_transcript(&payload);
            let is_final = frame.is_final();
            transcript.emit(text, is_final, callback);
            Ok(is_final)
        }
        Message::Ping(payload) => {
            socket
                .send(Message::Pong(payload))
                .await
                .map_err(|error| format!("火山引擎 ASR 心跳响应失败: {error}"))?;
            Ok(false)
        }
        Message::Close(_) => Err(format!(
            "火山引擎 ASR 在返回最终文本前关闭连接{}",
            log_id_suffix(log_id)
        )),
        _ => Ok(false),
    }
}

fn validate_response_payload(payload: &[u8]) -> Result<Value, String> {
    if payload.is_empty() {
        return Ok(Value::Null);
    }
    let value: Value = serde_json::from_slice(payload)
        .map_err(|error| format!("火山引擎 ASR 响应 JSON 无效: {error}"))?;
    let code = value
        .get("code")
        .or_else(|| value.get("status_code"))
        .and_then(|value| {
            value
                .as_i64()
                .or_else(|| value.as_str().and_then(|value| value.parse().ok()))
        })
        .unwrap_or(0);
    if !matches!(code, 0 | 1000 | SUCCESS_CODE) {
        let message = value
            .get("message")
            .or_else(|| value.get("error"))
            .and_then(Value::as_str)
            .unwrap_or("云端识别请求失败");
        return Err(format!("火山引擎 ASR 返回错误 {code}: {message}"));
    }
    Ok(value)
}

fn extract_transcript(payload: &Value) -> String {
    let result = payload.get("result").unwrap_or(payload);
    if let Some(utterances) = result.get("utterances").and_then(Value::as_array) {
        let text = utterances
            .iter()
            .filter_map(|utterance| utterance.get("text").and_then(Value::as_str))
            .collect::<String>();
        if !text.trim().is_empty() {
            return text.trim().to_string();
        }
    }
    result
        .get("text")
        .or_else(|| payload.get("text"))
        .and_then(Value::as_str)
        .unwrap_or_default()
        .trim()
        .to_string()
}

#[derive(Default)]
struct TranscriptState {
    revision: u64,
    text: String,
}

impl TranscriptState {
    fn emit(&mut self, text: String, is_final: bool, callback: &SpeechCallback) {
        if !text.is_empty() && text != self.text {
            self.text = text;
            self.revision = self.revision.saturating_add(1);
            if !is_final {
                callback(StreamingSpeechEvent::Partial {
                    revision: self.revision,
                    text: self.text.clone(),
                    confidence: None,
                });
            }
        }
        if is_final {
            self.revision = self.revision.saturating_add(1);
            callback(StreamingSpeechEvent::Final {
                revision: self.revision,
                text: self.text.clone(),
                confidence: None,
            });
        }
    }
}

fn format_server_error(frame: &ServerFrame, log_id: &str) -> String {
    let detail = serde_json::from_slice::<Value>(&frame.payload)
        .ok()
        .and_then(|value| {
            value
                .get("message")
                .or_else(|| value.get("error"))
                .and_then(Value::as_str)
                .map(str::to_string)
        })
        .unwrap_or_else(|| String::from_utf8_lossy(&frame.payload).trim().to_string());
    format!(
        "火山引擎 ASR 拒绝请求（code={}{}）{}",
        frame.error_code,
        log_id_suffix(log_id),
        if detail.is_empty() {
            String::new()
        } else {
            format!(": {detail}")
        }
    )
}

fn log_id_suffix(log_id: &str) -> String {
    if log_id.is_empty() {
        String::new()
    } else {
        format!(", logid={log_id}")
    }
}

async fn timeout_at_message(
    deadline: TokioInstant,
    socket: &mut CloudSocket,
) -> Result<Message, String> {
    timeout(
        deadline.saturating_duration_since(TokioInstant::now()),
        socket.next(),
    )
    .await
    .map_err(|_| "等待火山引擎 ASR 测试响应超时".to_string())?
    .ok_or_else(|| "火山引擎 ASR 测试连接已关闭".to_string())?
    .map_err(|error| format!("火山引擎 ASR 测试接收失败: {error}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn server_frame(payload: &Value, final_packet: bool) -> Vec<u8> {
        let encoded = gzip_compress(&serde_json::to_vec(payload).unwrap()).unwrap();
        let mut frame = build_header(
            MSG_FULL_SERVER_RESPONSE,
            if final_packet {
                FLAG_FINAL_NO_SEQUENCE
            } else {
                FLAG_NO_SEQUENCE
            },
            SERIALIZATION_JSON,
            COMPRESSION_GZIP,
        );
        frame.extend_from_slice(&(encoded.len() as u32).to_be_bytes());
        frame.extend_from_slice(&encoded);
        frame
    }

    #[test]
    fn audio_frames_use_raw_serialization_and_final_flag() {
        let regular = encode_audio_request(&[1, 2, 3, 4], false).unwrap();
        let final_pcm = [5, 6, 7, 8];
        let final_frame = encode_audio_request(&final_pcm, true).unwrap();
        assert_eq!(regular[0], 0x11);
        assert_eq!(regular[1], 0x20);
        assert_eq!(regular[2], 0x01);
        assert_eq!(final_frame[1], 0x22);
        assert_eq!(gzip_decompress(&final_frame[8..]).unwrap(), final_pcm);
    }

    #[test]
    fn streaming_audio_holds_the_real_last_chunk_for_the_final_packet() {
        let mut pending = PendingAudioPacket::default();
        assert_eq!(pending.push(vec![1, 2, 3]), None);
        assert_eq!(pending.push(vec![4, 5, 6]), Some(vec![1, 2, 3]));
        assert_eq!(pending.finish(), vec![4, 5, 6]);

        let mut empty = PendingAudioPacket::default();
        assert_eq!(empty.finish(), vec![0_u8; FINAL_SILENCE_PCM_BYTES]);
    }

    #[test]
    fn recognition_uses_service_default_vad_window() {
        assert!(recognition_payload()["request"]
            .get("end_window_size")
            .is_none());
    }

    #[test]
    fn parser_decodes_gzip_server_payload() {
        let payload = json!({
            "code": 20000000,
            "result": {
                "utterances": [
                    {"text": "你好，", "definite": true},
                    {"text": "继续任务。", "definite": false}
                ]
            }
        });
        let frame = parse_server_frame(&server_frame(&payload, true)).unwrap();
        assert!(frame.is_final());
        let decoded = validate_response_payload(&frame.payload).unwrap();
        assert_eq!(extract_transcript(&decoded), "你好，继续任务。");
    }

    #[test]
    fn resource_id_rejects_header_injection() {
        assert!(normalize_resource_id("volc.seedasr.sauc.duration\r\nX-Test: bad").is_err());
        assert_eq!(
            normalize_resource_id("").unwrap(),
            DEFAULT_RESOURCE_ID.to_string()
        );
    }

    #[test]
    fn embedded_asr_fallback_rejects_blank_values_and_uses_the_default_resource() {
        assert!(runtime_config_from_embedded_key(None).is_none());
        assert!(runtime_config_from_embedded_key(Some("  ")).is_none());
        let config = runtime_config_from_embedded_key(Some("internal-test-key")).unwrap();
        assert_eq!(config.api_key, "internal-test-key");
        assert_eq!(config.resource_id, DEFAULT_RESOURCE_ID);
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn macos_private_file_round_trip_is_owner_only() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("app-data").join(MACOS_CONFIG_FILE_NAME);
        let expected = StoredAsrConfig {
            version: 1,
            api_key: "test-secret".to_string(),
            resource_id: DEFAULT_RESOURCE_ID.to_string(),
        };

        write_macos_config_file(&path, &expected).unwrap();

        let directory_mode = fs::metadata(path.parent().unwrap())
            .unwrap()
            .permissions()
            .mode()
            & 0o777;
        let file_mode = fs::metadata(&path).unwrap().permissions().mode() & 0o777;
        assert_eq!(directory_mode, 0o700);
        assert_eq!(file_mode, 0o600);

        let actual = read_macos_config_file(&path).unwrap().unwrap();
        assert_eq!(actual.version, expected.version);
        assert_eq!(actual.api_key, expected.api_key);
        assert_eq!(actual.resource_id, expected.resource_id);
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn macos_private_file_does_not_follow_symlinks() {
        use std::os::unix::fs::symlink;

        let temp = tempfile::tempdir().unwrap();
        let target = temp.path().join("target.json");
        fs::write(&target, "{}").unwrap();
        let link = temp.path().join(MACOS_CONFIG_FILE_NAME);
        symlink(&target, &link).unwrap();

        let error = read_macos_config_file(&link).unwrap_err();
        assert!(error.contains("必须是普通文件"));
    }

    #[test]
    fn transcript_state_emits_cumulative_partial_then_final() {
        let events = Arc::new(std::sync::Mutex::new(Vec::new()));
        let captured = events.clone();
        let callback: SpeechCallback = Arc::new(move |event| {
            captured.lock().unwrap().push(event);
        });
        let mut state = TranscriptState::default();
        state.emit("你好".to_string(), false, &callback);
        state.emit("你好，继续任务".to_string(), false, &callback);
        state.emit("你好，继续任务".to_string(), true, &callback);

        let events = events.lock().unwrap();
        assert_eq!(events.len(), 3);
        assert!(matches!(
            &events[2],
            StreamingSpeechEvent::Final { revision: 3, text, .. }
                if text == "你好，继续任务"
        ));
    }

    #[tokio::test]
    #[ignore = "requires saved Volcengine credentials and cloud access"]
    async fn saved_credentials_probe_accepts_real_final_silence_packet() {
        let status = probe_saved_settings()
            .await
            .expect("saved credentials should accept a real PCM final packet");
        assert!(status.ok);
        assert_eq!(status.provider, "volcengine");
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    #[ignore = "requires saved Volcengine credentials and PET_MANAGER_ASR_PCM_FILE"]
    async fn saved_credentials_stream_real_pcm_to_final_text() {
        let pcm_path = env::var("PET_MANAGER_ASR_PCM_FILE")
            .expect("PET_MANAGER_ASR_PCM_FILE must point to 16 kHz mono S16LE PCM");
        let pcm = std::fs::read(&pcm_path).expect("PCM fixture must be readable");
        assert!(!pcm.is_empty(), "PCM fixture must not be empty");

        let config = load_runtime_config().expect("saved ASR credentials must be readable");
        let (commands, receiver) = mpsc::unbounded_channel();
        let (events, mut observed) = mpsc::unbounded_channel();
        let callback: SpeechCallback = Arc::new(move |event| {
            let _ = events.send(event);
        });
        let cancelled = Arc::new(AtomicBool::new(false));
        let worker = tokio::spawn(run_streaming_session(config, receiver, callback, cancelled));

        for chunk in pcm.chunks(640) {
            commands
                .send(StreamingSpeechCommand::Pcm(chunk.to_vec()))
                .expect("streaming ASR worker must accept PCM");
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
        commands
            .send(StreamingSpeechCommand::Finish)
            .expect("streaming ASR worker must accept the final marker");

        let transcript = timeout(Duration::from_secs(18), async {
            loop {
                match observed.recv().await {
                    Some(StreamingSpeechEvent::Final { text, .. }) if !text.trim().is_empty() => {
                        break text;
                    }
                    Some(StreamingSpeechEvent::Error(error)) => {
                        panic!("streaming ASR returned an error: {error}");
                    }
                    Some(_) => {}
                    None => panic!("streaming ASR event channel closed before final text"),
                }
            }
        })
        .await
        .expect("streaming ASR did not return final text before the test timeout");

        assert!(!transcript.trim().is_empty());
        timeout(Duration::from_secs(2), worker)
            .await
            .expect("streaming ASR worker did not exit")
            .expect("streaming ASR worker panicked")
            .expect("streaming ASR session failed");
        println!("recognized transcript: {transcript}");
    }
}
