/*
 * [Input] ESP32-P4 microphone PCM (16 kHz mono S16LE) and a Volcengine API key from environment, user storage, without embedded credentials.
 * [Output] Platform-specific shared speech-key persistence and ASR 2.0 streaming partial/final transcripts; legacy resource selections normalize to 2.0 on read/save.
 * [Pos] Tauri-side cloud ASR provider for device push-to-talk.
 */

use flate2::{read::GzDecoder, write::GzEncoder, Compression};
use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::env;
#[cfg(target_os = "macos")]
use std::fs;
use std::io::{Read, Write};
#[cfg(target_os = "macos")]
use std::os::unix::fs::PermissionsExt;
#[cfg(target_os = "macos")]
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
#[cfg(target_os = "macos")]
use std::sync::OnceLock;
use std::time::{Duration, Instant};
use tokio::sync::mpsc;
use tokio::time::{sleep_until, timeout, timeout_at, Instant as TokioInstant};
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::tungstenite::http::HeaderValue;
use tokio_tungstenite::tungstenite::Message;
use uuid::Uuid;

pub const DEFAULT_ENDPOINT: &str = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";
pub const DEFAULT_RESOURCE_ID: &str = "volc.seedasr.sauc.duration";

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
const AUDIO_INPUT_TIMEOUT: Duration = Duration::from_secs(8);
const PCM_SAMPLE_BYTES: usize = 2;
const PROBE_FINAL_SILENCE_PCM_BYTES: usize = 640;
const PROBE_SILENCE_PCM_BYTES: usize = 3_200;
// Admission bounds cover at most 30 seconds of 16 kHz S16LE PCM, even
// while a connection/write is stalled. Finish/Cancel are independent of PCM.
const MAX_QUEUED_PCM_BYTES: usize = 16_000 * 2 * 30;
const MAX_QUEUED_PCM_CHUNKS: usize = 1_536;
const MAX_SERVER_FRAME_BYTES: usize = 1_048_576;
const MAX_SERVER_PAYLOAD_BYTES: usize = 1_048_576;
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

type CloudSocket = crate::llm_network::CloudSocket;
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
    /// Client-side initialization plus the first real PCM socket flush. This
    /// is neither a microphone-start permit nor a server audio acknowledgment.
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
    Pcm(QueuedPcm),
    Finish,
    Cancel,
}

struct QueuedPcm {
    pcm: Vec<u8>,
    budget: Option<Arc<SpeechRound>>,
}

impl QueuedPcm {
    fn release_budget(&mut self) {
        if let Some(round) = self.budget.take() {
            let mut state = round.state();
            state.queued_pcm_chunks = state.queued_pcm_chunks.saturating_sub(1);
            state.queued_pcm_bytes = state.queued_pcm_bytes.saturating_sub(self.pcm.len());
        }
    }

    fn into_pcm(mut self) -> Vec<u8> {
        self.release_budget();
        std::mem::take(&mut self.pcm)
    }
}

impl Drop for QueuedPcm {
    fn drop(&mut self) { self.release_budget(); }
}

#[derive(Default)]
struct PendingAudioPacket {
    pcm: Option<Vec<u8>>,
}

impl PendingAudioPacket {
    fn push(&mut self, pcm: Vec<u8>) -> Option<Vec<u8>> {
        if pcm.is_empty() {
            return None;
        }
        // Keep only one actual S16LE sample for the final frame. Holding an
        // entire packet until the NEXT packet can leave the cloud with no
        // audio at all when USB input pauses immediately after its first chunk.
        let mut ready = match self.pcm.take() {
            Some(mut tail) => {
                tail.extend_from_slice(&pcm);
                tail
            }
            None => pcm,
        };
        if ready.len() <= PCM_SAMPLE_BYTES {
            self.pcm = Some(ready);
            return None;
        }
        self.pcm = Some(ready.split_off(ready.len() - PCM_SAMPLE_BYTES));
        Some(ready)
    }

    fn finish(&mut self) -> Option<Vec<u8>> {
        self.pcm.take()
    }
}

pub struct StreamingSpeechRecognizer {
    sender: mpsc::UnboundedSender<StreamingSpeechCommand>,
    cancelled: Arc<AtomicBool>,
    round: Arc<SpeechRound>,
}

/// Internal terminal state keeps cancellation explicit without changing the
/// existing public event enum used by PTT and component-note consumers.
#[derive(Clone, Debug, PartialEq)]
pub(crate) enum SpeechRoundTerminal {
    Final,
    Error(String),
    Cancelled,
}

#[derive(Default)]
struct SpeechRoundState {
    finishing: bool,
    terminal: Option<SpeechRoundTerminal>,
    terminal_metadata_logged: bool,
    accepted_pcm_chunks: u64,
    accepted_pcm_bytes: u64,
    queued_pcm_chunks: usize,
    queued_pcm_bytes: usize,
    max_queued_pcm_chunks: usize,
    max_queued_pcm_bytes: usize,
    input_failure: Option<String>,
    cloud_connected: bool,
    ws_audio_packets: u64,
    ws_written_pcm_bytes: u64,
    started_at: Option<TokioInstant>,
    first_pcm_at: Option<TokioInstant>,
    last_pcm_at: Option<TokioInstant>,
    first_ws_audio_flush_ms: Option<u64>,
    first_pcm_to_ws_flush_ms: Option<u64>,
    last_ws_audio_flush_at: Option<TokioInstant>,
    max_local_input_gap_ms: Option<u64>,
    max_ws_flush_gap_ms: Option<u64>,
    stage: &'static str,
    failure_stage: &'static str,
    service_code: Option<i64>,
    service_log_id: Option<String>,
}

impl SpeechRoundState {
    fn take_terminal_metadata(&mut self) -> Option<Value> {
        if self.terminal_metadata_logged { return None; }
        let metadata = self.terminal_metadata();
        if metadata.is_some() { self.terminal_metadata_logged = true; }
        metadata
    }

    fn terminal_metadata(&self) -> Option<Value> {
        self.terminal.as_ref().map(|terminal| json!({
                "terminal": match terminal {
                    SpeechRoundTerminal::Final => "final",
                    SpeechRoundTerminal::Error(_) => "error",
                    SpeechRoundTerminal::Cancelled => "cancelled",
                },
                "provider": "volcengine",
                "acceptedPcmChunks": self.accepted_pcm_chunks,
                "acceptedPcmBytes": self.accepted_pcm_bytes,
                "finishRequested": self.finishing,
                "cloudConnected": self.cloud_connected,
                "wsAudioPackets": self.ws_audio_packets,
                "wsWrittenPcmBytes": self.ws_written_pcm_bytes,
                "queuedPcmChunks": self.queued_pcm_chunks,
                "queuedPcmBytes": self.queued_pcm_bytes,
                "maxQueuedPcmChunks": self.max_queued_pcm_chunks,
                "maxQueuedPcmBytes": self.max_queued_pcm_bytes,
                "firstPcmArrivalMs": self.first_pcm_at.zip(self.started_at)
                    .map(|(first, start)| elapsed_ms(first, start)),
                "firstWsAudioFlushMs": self.first_ws_audio_flush_ms,
                "firstPcmToWsFlushMs": self.first_pcm_to_ws_flush_ms,
                "maxLocalInputGapMs": self.max_local_input_gap_ms,
                "maxWsFlushGapMs": self.max_ws_flush_gap_ms,
                "localInputIdleMs": self.last_pcm_at.map(|last| elapsed_ms(TokioInstant::now(), last)),
                "wsAudioIdleMs": self.last_ws_audio_flush_at.map(|last| elapsed_ms(TokioInstant::now(), last)),
                "stage": self.stage,
                "failureStage": if matches!(terminal, SpeechRoundTerminal::Error(_)) {
                    if self.failure_stage.is_empty() { self.stage } else { self.failure_stage }
                } else { "none" },
                "serviceCode": self.service_code,
                "serviceLogId": self.service_log_id,
                "socketFlushIsServerAck": false,
            }))
    }
}

struct SpeechRound {
    generation: u64,
    state: Mutex<SpeechRoundState>,
    stop: tokio::sync::Notify,
}

impl SpeechRound {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        Self { generation: NEXT.fetch_add(1, Ordering::Relaxed),
            state: Mutex::new(SpeechRoundState {
                started_at: Some(TokioInstant::now()), stage: "created",
                ..SpeechRoundState::default()
            }), stop: tokio::sync::Notify::new() }
    }

    fn state(&self) -> std::sync::MutexGuard<'_, SpeechRoundState> {
        self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn emit(&self, mut event: StreamingSpeechEvent, callback: &SpeechCallback) {
        let terminal_metadata = {
            let mut state = self.state();
            if state.terminal.is_some() { return; }
            // A known input loss must not later be reported as a successful
            // Final or replaced by a generic receiver-close fallback.
            if let Some(error) = &state.input_failure {
                event = StreamingSpeechEvent::Error(error.clone());
            }
            state.terminal = match &event {
                StreamingSpeechEvent::Final { .. } => Some(SpeechRoundTerminal::Final),
                StreamingSpeechEvent::Error(error) => Some(SpeechRoundTerminal::Error(error.clone())),
                _ => None,
            };
            state.take_terminal_metadata()
        };
        // Publish the terminal before invoking user code or dropping the worker
        // receiver, so late PCM/finish cannot replace it with a channel error.
        if let Some(metadata) = terminal_metadata {
            // Fixed stages/counters/timings and the bounded provider trace ID
            // only; no audio, transcript, error detail, keys, host or user path.
            eprintln!("[speech-round] {metadata}");
        }
        callback(event);
    }

    fn cancel(&self) {
        let mut state = self.state();
        if state.terminal.is_none() && state.input_failure.is_none() {
            state.terminal = Some(SpeechRoundTerminal::Cancelled);
        }
        drop(state);
        // Fast caller path: no network, callback or stderr write on USB ingress.
        self.stop.notify_one();
    }

    fn log_terminal_metadata(&self) {
        let metadata = self.state().take_terminal_metadata();
        if let Some(metadata) = metadata { eprintln!("[speech-round] {metadata}"); }
    }

    fn input_result(&self) -> Result<(), String> {
        let state = self.state();
        match &state.terminal {
            Some(SpeechRoundTerminal::Error(error)) => Err(error.clone()),
            Some(SpeechRoundTerminal::Cancelled) => Err("cloud speech recognition was cancelled".into()),
            // Final/finishing PCM is deliberately ignored. A closed receiver
            // while still open defers to the worker's guaranteed exit event.
            None => state.input_failure.clone().map_or(Ok(()), Err),
            _ => Ok(()),
        }
    }

    fn note_audio_write(&self, bytes: usize) {
        let mut state = self.state();
        let now = TokioInstant::now();
        if state.first_ws_audio_flush_ms.is_none() {
            state.first_ws_audio_flush_ms = state.started_at.map(|start| elapsed_ms(now, start));
            state.first_pcm_to_ws_flush_ms = state.first_pcm_at.map(|first| elapsed_ms(now, first));
        }
        if let Some(previous) = state.last_ws_audio_flush_at {
            let gap = elapsed_ms(now, previous);
            state.max_ws_flush_gap_ms = Some(state.max_ws_flush_gap_ms.unwrap_or(0).max(gap));
        }
        state.last_ws_audio_flush_at = Some(now);
        state.ws_audio_packets = state.ws_audio_packets.saturating_add(1);
        state.ws_written_pcm_bytes = state.ws_written_pcm_bytes.saturating_add(bytes as u64);
    }

    fn set_stage(&self, stage: &'static str) {
        let mut state = self.state();
        if state.input_failure.is_none() && state.terminal.is_none() { state.stage = stage; }
    }

    fn fail_at(&self, stage: &'static str, error: String) -> String {
        let mut state = self.state();
        if state.failure_stage.is_empty() { state.failure_stage = stage; }
        error
    }

    fn note_log_id(&self, log_id: &str) {
        // A malformed trace header is not safe diagnostic metadata. The error
        // returned to the caller still carries the original provider detail.
        if !log_id.is_empty() && log_id.len() <= 128
            && log_id.bytes().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'-' | b'_'))
        {
            self.state().service_log_id = Some(log_id.to_string());
        }
    }

    fn note_provider_error(&self, code: i64, log_id: &str) {
        self.note_log_id(log_id);
        let mut state = self.state();
        state.service_code.get_or_insert(code);
        if state.failure_stage.is_empty() { state.failure_stage = "provider_response"; }
    }
}

fn elapsed_ms(now: TokioInstant, then: TokioInstant) -> u64 {
    now.saturating_duration_since(then).as_millis().min(u128::from(u64::MAX)) as u64
}

/// Also runs on task abort/panic. There is exactly one terminal per generation;
/// a real provider error or Final always wins over this fallback.
struct SpeechWorkerExit {
    round: Arc<SpeechRound>,
    callback: SpeechCallback,
    cancelled: Arc<AtomicBool>,
}

impl SpeechWorkerExit {
    fn complete(&self, result: Result<(), String>) {
        let input_failure = self.round.state().input_failure.clone();
        if let Some(error) = input_failure {
            self.round.emit(StreamingSpeechEvent::Error(error), &self.callback);
        } else if self.cancelled.load(Ordering::SeqCst) { self.round.cancel(); }
        else {
            self.round.emit(StreamingSpeechEvent::Error(result.err().unwrap_or_else(||
                "云端 ASR 工作任务在最终结果前结束，请重新发声".into())), &self.callback);
        }
        self.round.log_terminal_metadata();
    }
}

impl Drop for SpeechWorkerExit {
    fn drop(&mut self) { self.complete(Ok(())); }
}

impl StreamingSpeechRecognizer {
    /// Offline orchestration tests retain the receiver; no keys, socket or device.
    #[cfg(test)]
    pub(crate) fn test_sink() -> (Self, impl Sized) {
        let (sender, receiver) = mpsc::unbounded_channel();
        (Self { sender, cancelled: Arc::new(AtomicBool::new(false)),
            round: Arc::new(SpeechRound::new()) }, receiver)
    }

    pub fn start(
        callback: impl Fn(StreamingSpeechEvent) + Send + Sync + 'static,
    ) -> Result<Self, String> {
        let started_at = TokioInstant::now();
        let config = load_runtime_config()?;
        let callback: SpeechCallback = Arc::new(callback);
        let (sender, receiver) = mpsc::unbounded_channel();
        let cancelled = Arc::new(AtomicBool::new(false));
        let worker_cancelled = cancelled.clone();
        let round = Arc::new(SpeechRound::new());
        // Include synchronous credential preparation in ASR API-entry latency.
        // Earlier device/capture setup remains the caller's separate timeline.
        round.state().started_at = Some(started_at);
        let worker_round = round.clone();
        let transport_round = round.clone();
        let exit = SpeechWorkerExit { round: round.clone(), callback: callback.clone(), cancelled: cancelled.clone() };
        let worker_callback: SpeechCallback = Arc::new(move |event| worker_round.emit(event, &callback));

        tauri::async_runtime::spawn(async move {
            let exit = exit;
            let stopped = async {
                loop {
                    let notified = exit.round.stop.notified();
                    if worker_cancelled.load(Ordering::SeqCst)
                        || exit.round.state().input_failure.is_some() { return; }
                    notified.await;
                }
            };
            let result = tokio::select! {
                biased;
                _ = stopped => Ok(()),
                result = run_streaming_session(
                config,
                receiver,
                worker_callback,
                worker_cancelled.clone(),
                transport_round,
                ) => result,
            };
            exit.complete(result);
        });

        Ok(Self {
            sender,
            cancelled,
            round,
        })
    }

    pub(crate) fn generation(&self) -> u64 { self.round.generation }

    pub(crate) fn terminal(&self) -> Option<SpeechRoundTerminal> { self.round.state().terminal.clone() }

    pub fn push_pcm(&self, pcm: &[u8]) -> Result<(), String> {
        let mut state = self.round.state();
        if state.terminal.is_some() || state.input_failure.is_some() || state.finishing || pcm.is_empty() {
            drop(state);
            return self.round.input_result();
        }
        if pcm.len() % PCM_SAMPLE_BYTES != 0 {
            let error = "本地 ASR PCM 必须为完整的 16-bit 采样".to_string();
            state.input_failure = Some(error.clone());
            state.stage = "input_validation";
            if state.failure_stage.is_empty() { state.failure_stage = "input_validation"; }
            drop(state);
            self.round.stop.notify_one();
            return Err(error);
        }
        if state.queued_pcm_chunks >= MAX_QUEUED_PCM_CHUNKS
            || pcm.len() > MAX_QUEUED_PCM_BYTES.saturating_sub(state.queued_pcm_bytes)
        {
            let error = "本地语音 PCM 输入缓冲已满，本段已停止；未丢包续传或重连（ASR_INPUT_QUEUE_OVERFLOW）".to_string();
            state.input_failure = Some(error.clone());
            state.stage = "input_queue_overflow";
            if state.failure_stage.is_empty() { state.failure_stage = "input_queue_overflow"; }
            drop(state);
            self.round.stop.notify_one();
            return Err(error);
        }
        state.queued_pcm_chunks += 1;
        state.queued_pcm_bytes += pcm.len();
        let sent = self.sender.send(StreamingSpeechCommand::Pcm(QueuedPcm {
            pcm: pcm.to_vec(), budget: Some(self.round.clone()),
        }));
        if sent.is_ok() {
            state.max_queued_pcm_chunks = state.max_queued_pcm_chunks.max(state.queued_pcm_chunks);
            state.max_queued_pcm_bytes = state.max_queued_pcm_bytes.max(state.queued_pcm_bytes);
            let now = TokioInstant::now();
            if let Some(previous) = state.last_pcm_at {
                let gap = elapsed_ms(now, previous);
                state.max_local_input_gap_ms = Some(state.max_local_input_gap_ms.unwrap_or(0).max(gap));
            }
            state.first_pcm_at.get_or_insert(now);
            state.last_pcm_at = Some(now);
            state.accepted_pcm_chunks = state.accepted_pcm_chunks.saturating_add(1);
            state.accepted_pcm_bytes = state.accepted_pcm_bytes.saturating_add(pcm.len() as u64);
        }
        drop(state);
        // Failed sends return the packet; drop its budget only AFTER unlocking
        // the round, otherwise QueuedPcm::drop would re-lock the same mutex.
        drop(sent);
        self.round.input_result()
    }

    pub fn finish(&self) -> Result<(), String> {
        let mut state = self.round.state();
        if !state.finishing && state.terminal.is_none() && state.input_failure.is_none() {
            state.finishing = true;
            let _ = self.sender.send(StreamingSpeechCommand::Finish);
        }
        let error = match &state.terminal {
            Some(SpeechRoundTerminal::Error(error)) => Some(error.clone()),
            None => state.input_failure.clone(),
            _ => None,
        };
        error.map_or(Ok(()), Err)
    }

    pub fn cancel(&self) {
        if self.cancelled.swap(true, Ordering::SeqCst) {
            return;
        }
        self.round.cancel();
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

    Ok(public_status(
        false,
        DEFAULT_RESOURCE_ID.to_string(),
        "none",
        "请配置火山引擎语音识别 API Key",
    ))
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
    let deadline = TokioInstant::now() + PROBE_TIMEOUT;
    // One original 8 s budget covers writes, reads, Ping/Pong and best-effort
    // close. An initialization acknowledgment alone does not prove completion.
    let status = timeout_at(deadline, async {
        send_stream_message(&mut socket, Message::Binary(
            encode_full_client_request(&recognition_payload())?.into()
        ), "探测初始化", &log_id).await?;
        // Explicit settings probe only, never production-stream keepalive.
        send_stream_message(&mut socket, Message::Binary(
            encode_audio_request(&vec![0_u8; PROBE_SILENCE_PCM_BYTES], false)?.into()
        ), "探测音频", &log_id).await?;
        send_stream_message(&mut socket, Message::Binary(
            encode_audio_request(&vec![0_u8; PROBE_FINAL_SILENCE_PCM_BYTES], true)?.into()
        ), "探测结束帧", &log_id).await?;
        loop {
            let message = timeout_at_message(deadline, &mut socket).await?;
            match message {
                Message::Binary(data) => {
                    let frame = parse_server_frame(data.as_ref())?;
                    if frame.message_type == MSG_SERVER_ERROR {
                        return Err(format_server_error(&frame, &log_id));
                    }
                    if frame.message_type == MSG_FULL_SERVER_RESPONSE {
                        validate_response_payload(&frame.payload, &log_id, None)?;
                        if !frame.is_final() { continue; }
                        return Ok(DeviceAsrProbeStatus {
                            ok: true, provider: "volcengine", resource_id: config.resource_id,
                            latency_ms: started.elapsed().as_millis(), log_id,
                            message: "语音识别探测已收到服务端最终响应".to_string(),
                        });
                    }
                }
                Message::Close(_) => return Err("语音识别在探测最终响应前关闭连接".to_string()),
                Message::Ping(payload) => {
                    send_stream_message(&mut socket, Message::Pong(payload), "探测心跳响应", &log_id).await?;
                }
                _ => {}
            }
        }
    }).await.map_err(|_| "语音识别探测超时，未收到最终响应".to_string())??;
    // Cleanup cannot invalidate an already verified final response or obtain
    // a new timeout budget; an exhausted deadline simply drops the transport.
    let _ = timeout_at(deadline, socket.close(None)).await;
    Ok(status)
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
    // All supported speech input uses ASR 2.0; migrate stored 1.0/resource overrides on read.
    Ok(DEFAULT_RESOURCE_ID.to_string())
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

/// The Doubao speech API key the ASR module resolved (stored user configuration or runtime environment).
/// Realtime chat reuses it for Seed TTS when no separate TTS key is configured.
pub fn stored_api_key() -> Option<String> {
    load_runtime_config().ok().map(|config| config.api_key).filter(|key| !key.trim().is_empty())
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
    Err("未配置火山引擎语音识别 API Key".to_string())
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
    let value = crate::secure_config::read(path,"speech-asr")?.ok_or("语音配置未找到")?;
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
    crate::secure_config::write(path,"speech-asr",&value)
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
    round: Arc<SpeechRound>,
) -> Result<(), String> {
    // A local capture can be admitted before any USB PCM arrives. Do not start
    // the provider's audio-gap clock until real input exists. Cancellation is
    // still selected by the outer worker and Finish never fabricates silence.
    round.set_stage("first_pcm_wait");
    let mut pending_audio = PendingAudioPacket::default();
    let first_audio = timeout(AUDIO_INPUT_TIMEOUT, async {
        loop {
            match receiver.recv().await {
                Some(StreamingSpeechCommand::Pcm(pcm)) => {
                    let pcm = pcm.into_pcm();
                    if pcm.len() % PCM_SAMPLE_BYTES != 0 {
                        return Err(round.fail_at("input_validation", "本地 ASR PCM 必须为完整的 16-bit 采样".to_string()));
                    }
                    if let Some(audio) = pending_audio.push(pcm) {
                        return Ok(Some(audio));
                    }
                }
                Some(StreamingSpeechCommand::Finish) => {
                    return Err(round.fail_at("first_pcm_empty", "未收到足够的真实麦克风 PCM，未建立云端识别会话".to_string()));
                }
                Some(StreamingSpeechCommand::Cancel) | None => {
                    cancelled.store(true, Ordering::SeqCst);
                    return Ok(None);
                }
            }
        }
    })
    .await
    .map_err(|_| round.fail_at("first_pcm_timeout", "本地等待真实麦克风 PCM 超时，未建立云端识别会话".to_string()))??;
    let Some(first_audio) = first_audio else { return Ok(()); };
    round.input_result()?;
    round.set_stage("connect");
    let (mut socket, log_id) = connect_cloud_socket(&config).await
        .map_err(|error| round.fail_at("connect", error))?;
    round.state().cloud_connected = true;
    round.note_log_id(&log_id);
    round.input_result()?;
    round.set_stage("init_write");
    send_stream_message(
        &mut socket,
        Message::Binary(
            encode_full_client_request(&recognition_payload())?.into(),
        ),
        "初始化请求",
        &log_id,
    ).await.map_err(|error| round.fail_at("ws_init_write", error))?;
    round.input_result()?;
    round.set_stage("first_audio_write");
    send_stream_message(
        &mut socket,
        Message::Binary(encode_audio_request(&first_audio, false)?.into()),
        "首个音频包",
        &log_id,
    ).await.map_err(|error| round.fail_at("ws_first_audio_write", error))?;
    round.note_audio_write(first_audio.len());
    callback(StreamingSpeechEvent::Ready);

    let mut transcript = TranscriptState::default();
    let mut final_deadline: Option<TokioInstant> = None;
    let mut audio_input_deadline = TokioInstant::now() + AUDIO_INPUT_TIMEOUT;

    loop {
        // Do not rely only on the outer select being re-polled: a ready PCM
        // backlog can otherwise keep this future making progress after input
        // overflow/cancellation. Already-in-flight I/O is never replayed.
        round.input_result()?;
        if let Some(deadline) = final_deadline {
            round.set_stage("final_wait");
            // One absolute deadline covers read AND control-frame handling.
            // A Ping at 7.9 s must not buy another 8 s for a blocked Pong write.
            let completed = timeout_at(deadline, async {
                let message = socket.next().await;
                handle_server_message(&mut socket, message, &mut transcript, &callback, &log_id, &round).await
            }).await.map_err(|_| round.fail_at("final_wait_deadline", format!(
                "等待语音识别最终结果超时{}", log_id_suffix(&log_id)
            )))??;
            if completed {
                return Ok(());
            }
            continue;
        }

        if let Some(accepted_at) = round.state().last_pcm_at {
            // Receiving new local PCM while an earlier write is blocked is
            // backpressure, not proof that the microphone stopped producing.
            audio_input_deadline = audio_input_deadline.max(accepted_at + AUDIO_INPUT_TIMEOUT);
        }
        round.set_stage("stream");
        tokio::select! {
            command = receiver.recv() => {
                match command {
                    Some(StreamingSpeechCommand::Pcm(pcm)) => {
                        round.input_result()?;
                        let pcm = pcm.into_pcm();
                        if pcm.is_empty() { continue; }
                        if pcm.len() % PCM_SAMPLE_BYTES != 0 {
                            return Err(round.fail_at("input_validation", "本地 ASR PCM 必须为完整的 16-bit 采样".to_string()));
                        }
                        audio_input_deadline = TokioInstant::now() + AUDIO_INPUT_TIMEOUT;
                        if let Some(audio) = pending_audio.push(pcm) {
                            round.set_stage("audio_write");
                            send_stream_message(
                                &mut socket,
                                Message::Binary(encode_audio_request(&audio, false)?.into()),
                                "音频包",
                                &log_id,
                            ).await.map_err(|error| round.fail_at("ws_audio_write", error))?;
                            round.note_audio_write(audio.len());
                        }
                    }
                    Some(StreamingSpeechCommand::Finish) => {
                        let final_pcm = pending_audio.finish()
                            .ok_or_else(|| round.fail_at("final_pcm_missing", "本地 ASR 缺少真实音频尾帧".to_string()))?;
                        round.set_stage("final_audio_write");
                        send_stream_message(
                            &mut socket,
                            Message::Binary(encode_audio_request(&final_pcm, true)?.into()),
                            "结束帧",
                            &log_id,
                        ).await.map_err(|error| round.fail_at("ws_final_audio_write", error))?;
                        round.note_audio_write(final_pcm.len());
                        final_deadline = Some(TokioInstant::now() + FINAL_TIMEOUT);
                    }
                    Some(StreamingSpeechCommand::Cancel) | None => {
                        cancelled.store(true, Ordering::SeqCst);
                        let _ = timeout(CONNECT_TIMEOUT, socket.close(None)).await;
                        return Ok(());
                    }
                }
            }
            message = socket.next() => {
                let completed = timeout_at(audio_input_deadline,
                    handle_server_message(&mut socket, message, &mut transcript, &callback, &log_id, &round)
                ).await.map_err(|_| round.fail_at("audio_window_control", format!(
                    "语音识别音频处理窗口超时（WebSocket 控制帧处理未完成），本段已停止{}",
                    log_id_suffix(&log_id)
                )))??;
                if completed {
                    return Ok(());
                }
            }
            _ = sleep_until(audio_input_deadline) => {
                if let Some(accepted_at) = round.state().last_pcm_at {
                    let current = accepted_at + AUDIO_INPUT_TIMEOUT;
                    if current > TokioInstant::now() {
                        audio_input_deadline = current;
                        continue;
                    }
                }
                return Err(round.fail_at("input_gap", format!(
                    "本地真实麦克风 PCM 输入中断超时，本段识别已停止，未补静音或重连{}",
                    log_id_suffix(&log_id)
                )));
            }
        }
    }
}

async fn send_stream_message(
    socket: &mut CloudSocket,
    message: Message,
    phase: &str,
    log_id: &str,
) -> Result<(), String> {
    timeout(CONNECT_TIMEOUT, socket.send(message))
        .await
        .map_err(|_| format!("语音识别 WebSocket {phase}写入超时{}", log_id_suffix(log_id)))?
        .map_err(|error| format!("语音识别 WebSocket {phase}写入失败: {error}{}", log_id_suffix(log_id)))
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

    let connected = timeout(CONNECT_TIMEOUT, crate::llm_network::connect_websocket(request))
        .await
        .map_err(|_| "连接火山引擎 ASR 超时".to_string())?
        .map_err(|error| format!("连接火山引擎 ASR 失败: {error}"))?;
    let (socket, response) = connected;
    let log_id = response
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
    let decoder = GzDecoder::new(payload);
    let mut decoded = Vec::new();
    decoder
        .take(MAX_SERVER_PAYLOAD_BYTES as u64 + 1)
        .read_to_end(&mut decoded)
        .map_err(|error| format!("火山引擎 ASR 响应解压失败: {error}"))?;
    if decoded.len() > MAX_SERVER_PAYLOAD_BYTES {
        return Err("语音识别服务响应解压后超过安全上限".to_string());
    }
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
    if data.len() > MAX_SERVER_FRAME_BYTES {
        return Err("语音识别服务响应帧超过安全上限".to_string());
    }
    if data[0] >> 4 != 1 {
        return Err("语音识别服务响应协议版本无效".to_string());
    }
    let header_size = usize::from(data[0] & 0x0f) * 4;
    if header_size < 4 || data.len() < header_size {
        return Err("火山引擎 ASR 响应帧头无效".to_string());
    }

    let message_type = data[1] >> 4;
    let flags = data[1] & 0x0f;
    let compression = data[2] & 0x0f;
    if !matches!(message_type, MSG_FULL_SERVER_RESPONSE | MSG_SERVER_ERROR)
        || !matches!(flags, FLAG_NO_SEQUENCE | FLAG_POSITIVE_SEQUENCE | FLAG_FINAL_NO_SEQUENCE | FLAG_NEGATIVE_SEQUENCE)
        || !matches!(data[2] >> 4, SERIALIZATION_NONE | SERIALIZATION_JSON)
    {
        return Err("语音识别服务响应协议类型或标志无效".to_string());
    }
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
    if end != data.len() || payload_size > MAX_SERVER_PAYLOAD_BYTES {
        return Err("语音识别服务响应长度不匹配或超过安全上限".to_string());
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
    round: &SpeechRound,
) -> Result<bool, String> {
    round.set_stage("ws_read");
    let message = message
        .ok_or_else(|| round.fail_at("ws_read_close", format!("语音识别 WebSocket 连接已关闭{}", log_id_suffix(log_id))))?
        .map_err(|error| round.fail_at("ws_read", format!("语音识别 WebSocket 接收失败: {error}{}", log_id_suffix(log_id))))?;

    match message {
        Message::Binary(data) => {
            round.set_stage("response_decode");
            let frame = parse_server_frame(data.as_ref())
                .map_err(|error| round.fail_at("response_decode", error))?;
            if frame.message_type == MSG_SERVER_ERROR {
                round.note_provider_error(i64::from(frame.error_code), log_id);
                return Err(format_server_error(&frame, log_id));
            }
            if frame.message_type != MSG_FULL_SERVER_RESPONSE {
                return Ok(false);
            }
            let payload = validate_response_payload(&frame.payload, log_id, Some(round))
                .map_err(|error| round.fail_at("response_decode", error))?;
            let text = extract_transcript(&payload);
            let is_final = frame.is_final();
            round.set_stage(if is_final { "final_received" } else { "transcript" });
            transcript.emit(text, is_final, callback);
            Ok(is_final)
        }
        Message::Ping(payload) => {
            round.set_stage("pong_write");
            send_stream_message(socket, Message::Pong(payload), "心跳响应", log_id).await
                .map_err(|error| round.fail_at("ws_pong_write", error))?;
            Ok(false)
        }
        Message::Close(_) => Err(round.fail_at("ws_close_before_final", format!(
            "语音识别 WebSocket 在返回最终文本前关闭连接{}",
            log_id_suffix(log_id)
        ))),
        _ => Ok(false),
    }
}

fn validate_response_payload(payload: &[u8], log_id: &str, round: Option<&SpeechRound>) -> Result<Value, String> {
    if payload.is_empty() {
        return Ok(Value::Null);
    }
    let value: Value = serde_json::from_slice(payload)
        .map_err(|error| format!("火山引擎 ASR 响应 JSON 无效: {error}"))?;
    if !value.is_object() && !value.is_null() {
        return Err("语音识别服务响应 JSON 顶层类型无效".into());
    }
    let code = match value.get("code").or_else(|| value.get("status_code")) {
        None => 0, // Result-only responses are allowed by the current protocol.
        Some(code) => code.as_i64()
            .or_else(|| code.as_str().and_then(|raw| raw.parse().ok()))
            .filter(|code| *code >= 0)
            .ok_or_else(|| "语音识别服务响应错误码格式无效".to_string())?,
    };
    if !matches!(code, 0 | 1000 | SUCCESS_CODE) {
        if let Some(round) = round { round.note_provider_error(code, log_id); }
        // Preserve the decoded provider body for the caller, not diagnostic logs.
        let detail = String::from_utf8_lossy(payload);
        return Err(format_provider_error(code, detail.trim(), log_id));
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
    // No raw body is logged: the caller gets the unchanged decoded service
    // detail while the terminal diagnostic contains only code and trace metadata.
    let detail = String::from_utf8_lossy(&frame.payload);
    format_provider_error(i64::from(frame.error_code), detail.trim(), log_id)
}

fn format_provider_error(code: i64, detail: &str, log_id: &str) -> String {
    let category = if code == 45_000_081 {
        "语音识别服务等待音频超时，本段已结束"
    } else {
        "语音识别服务返回错误"
    };
    format!(
        "{category}（provider=volcengine, code={code}{}）{}",
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
mod terminal_lifecycle_tests {
    use super::*;

    fn fixture() -> (StreamingSpeechRecognizer, mpsc::UnboundedReceiver<StreamingSpeechCommand>,
        SpeechCallback, Arc<Mutex<Vec<StreamingSpeechEvent>>>) {
        let (sender, receiver) = mpsc::unbounded_channel();
        let events = Arc::new(Mutex::new(Vec::new()));
        let output = events.clone();
        let callback: SpeechCallback = Arc::new(move |event| output.lock().unwrap().push(event));
        (StreamingSpeechRecognizer { sender, cancelled: Arc::new(AtomicBool::new(false)),
            round: Arc::new(SpeechRound::new()) }, receiver, callback, events)
    }

    fn final_event() -> StreamingSpeechEvent {
        StreamingSpeechEvent::Final { revision: 1, text: "synthetic".into(), confidence: None }
    }

    #[test]
    fn final_then_late_pcm_finish_and_error_preserves_one_final() {
        let (rec, receiver, callback, events) = fixture();
        rec.round.emit(final_event(), &callback);
        drop(receiver);
        assert!(rec.push_pcm(&[0; 640]).is_ok());
        assert!(rec.finish().is_ok()); assert!(rec.finish().is_ok());
        rec.round.emit(StreamingSpeechEvent::Error("late closed".into()), &callback);
        rec.round.emit(final_event(), &callback);
        rec.cancel();
        assert_eq!(rec.terminal(), Some(SpeechRoundTerminal::Final));
        assert_eq!(events.lock().unwrap().len(), 1);
        assert!(matches!(events.lock().unwrap()[0], StreamingSpeechEvent::Final { .. }));
    }

    #[test]
    fn receiver_close_defers_to_real_worker_first_error() {
        let (rec, receiver, callback, events) = fixture();
        drop(receiver);
        // Simulate the receiver-drop / worker-result-publication scheduling gap.
        assert!(rec.push_pcm(&[0; 640]).is_ok()); assert!(rec.finish().is_ok());
        let exit = SpeechWorkerExit { round: rec.round.clone(), callback,
            cancelled: rec.cancelled.clone() };
        exit.complete(Err("401 synthetic authentication failure".into()));
        drop(exit);
        assert_eq!(rec.push_pcm(&[0; 640]).unwrap_err(), "401 synthetic authentication failure");
        assert_eq!(rec.finish().unwrap_err(), "401 synthetic authentication failure");
        assert_eq!(events.lock().unwrap().len(), 1);
    }

    #[test]
    fn finish_once_ignores_inflight_late_audio() {
        let (rec, mut receiver, _, _) = fixture();
        rec.finish().unwrap(); rec.finish().unwrap(); rec.push_pcm(&[0; 640]).unwrap();
        assert!(matches!(receiver.try_recv().unwrap(), StreamingSpeechCommand::Finish));
        assert!(receiver.try_recv().is_err());
    }

    #[test]
    fn worker_exit_without_final_publishes_one_error() {
        let (rec, receiver, callback, events) = fixture();
        let exit = SpeechWorkerExit { round: rec.round.clone(), callback,
            cancelled: rec.cancelled.clone() };
        drop(receiver); drop(exit);
        assert!(matches!(rec.terminal(), Some(SpeechRoundTerminal::Error(_))));
        assert!(rec.push_pcm(&[0; 640]).is_err());
        assert_eq!(events.lock().unwrap().len(), 1);
    }

    #[test]
    fn cancelled_generation_never_delivers_into_next_round() {
        let (old, old_receiver, old_callback, old_events) = fixture();
        let (next, _, next_callback, next_events) = fixture();
        assert_ne!(old.generation(), next.generation());
        old.cancel(); drop(old_receiver);
        old.round.emit(final_event(), &old_callback);
        old.round.emit(StreamingSpeechEvent::Error("stale".into()), &old_callback);
        next.round.emit(final_event(), &next_callback);
        assert_eq!(old.terminal(), Some(SpeechRoundTerminal::Cancelled));
        assert!(old_events.lock().unwrap().is_empty());
        assert_eq!(next_events.lock().unwrap().len(), 1);
        assert_eq!(next.terminal(), Some(SpeechRoundTerminal::Final));
    }

    #[test]
    fn first_provider_error_survives_cancellation_and_late_errors() {
        let (rec, _, callback, events) = fixture();
        rec.round.emit(StreamingSpeechEvent::Error("quota synthetic".into()), &callback);
        rec.cancel(); rec.round.emit(StreamingSpeechEvent::Error("generic closed".into()), &callback);
        assert_eq!(rec.finish().unwrap_err(), "quota synthetic");
        assert_eq!(events.lock().unwrap().len(), 1);
    }

    #[test]
    fn racing_final_and_cancel_choose_one_terminal() {
        for _ in 0..32 {
            let (rec, _, callback, events) = fixture();
            let round = rec.round.clone();
            let worker = std::thread::spawn(move || round.emit(final_event(), &callback));
            rec.cancel(); worker.join().unwrap();
            match rec.terminal().unwrap() {
                SpeechRoundTerminal::Final => assert_eq!(events.lock().unwrap().len(), 1),
                SpeechRoundTerminal::Cancelled => assert!(events.lock().unwrap().is_empty()),
                _ => panic!("unexpected terminal"),
            }
        }
    }

    #[test]
    fn byte_budget_overflow_preserves_first_failure_and_never_reports_final() {
        let (rec, receiver, callback, events) = fixture();
        rec.push_pcm(&vec![1; MAX_QUEUED_PCM_BYTES]).unwrap();
        let error = rec.push_pcm(&[1, 2]).unwrap_err();
        assert!(error.contains("ASR_INPUT_QUEUE_OVERFLOW"));
        assert_eq!(rec.finish().unwrap_err(), error);
        rec.cancel();
        let exit = SpeechWorkerExit { round: rec.round.clone(), callback,
            cancelled: rec.cancelled.clone() };
        exit.complete(Err("later transport close".into()));
        drop(exit);
        assert_eq!(rec.terminal(), Some(SpeechRoundTerminal::Error(error)));
        assert_eq!(events.lock().unwrap().len(), 1);
        assert!(matches!(events.lock().unwrap()[0], StreamingSpeechEvent::Error(_)));
        assert_eq!(rec.round.state().accepted_pcm_bytes, MAX_QUEUED_PCM_BYTES as u64);
        drop(receiver);
        assert_eq!(rec.round.state().queued_pcm_bytes, 0);
        assert_eq!(rec.round.state().queued_pcm_chunks, 0);
    }

    #[test]
    fn chunk_budget_is_bounded_and_receiver_drop_releases_every_permit() {
        let (rec, receiver, _, _) = fixture();
        for _ in 0..MAX_QUEUED_PCM_CHUNKS { rec.push_pcm(&[1, 2]).unwrap(); }
        assert!(rec.push_pcm(&[3, 4]).is_err());
        assert_eq!(rec.round.state().queued_pcm_chunks, MAX_QUEUED_PCM_CHUNKS);
        drop(receiver);
        assert_eq!(rec.round.state().queued_pcm_chunks, 0);
    }

    #[test]
    fn dequeue_releases_budget_and_socket_counts_do_not_claim_server_receipt() {
        let (rec, mut receiver, _, _) = fixture();
        rec.push_pcm(&[1, 2, 3, 4]).unwrap();
        let StreamingSpeechCommand::Pcm(packet) = receiver.try_recv().unwrap() else {
            panic!("expected real PCM");
        };
        assert_eq!(packet.into_pcm(), vec![1, 2, 3, 4]);
        assert_eq!(rec.round.state().queued_pcm_bytes, 0);
        assert_eq!(rec.round.state().ws_written_pcm_bytes, 0);
        rec.round.note_audio_write(2);
        let state = rec.round.state();
        assert_eq!(state.accepted_pcm_bytes, 4);
        assert_eq!(state.ws_written_pcm_bytes, 2);
        assert_eq!(state.ws_audio_packets, 1);
        assert!(state.first_pcm_to_ws_flush_ms.is_some());
    }

    #[test]
    fn terminal_metadata_excludes_error_body_and_rejects_non_trace_header_content() {
        let round = SpeechRound::new();
        round.note_log_id("unsafe/host:path");
        assert!(round.state().service_log_id.is_none());
        round.note_provider_error(45_000_081, "synthetic_trace");
        let mut state = round.state();
        state.terminal = Some(SpeechRoundTerminal::Error("synthetic_provider_body_only".into()));
        let metadata = state.terminal_metadata().unwrap();
        assert_eq!(metadata["serviceCode"], 45_000_081);
        assert_eq!(metadata["serviceLogId"], "synthetic_trace");
        assert_eq!(metadata["socketFlushIsServerAck"], false);
        assert!(metadata["firstWsAudioFlushMs"].is_null());
        assert!(!metadata.to_string().contains("synthetic_provider_body_only"));
        assert!(!metadata.to_string().contains("unsafe/host:path"));
    }
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
    fn provider_timeout_and_unknown_error_keep_code_trace_and_detail_without_blame() {
        let timeout = format_provider_error(45_000_081, "Timed out waiting for next audio packet", "synthetic_trace");
        assert!(timeout.starts_with("语音识别服务等待音频超时，本段已结束"));
        assert!(timeout.contains("provider=volcengine"));
        assert!(timeout.contains("code=45000081"));
        assert!(timeout.contains("logid=synthetic_trace"));
        assert!(timeout.contains("Timed out waiting for next audio packet"));
        assert!(!timeout.contains("拒绝"));
        let unknown = format_provider_error(55_099_999, "synthetic detail", "trace");
        assert!(unknown.starts_with("语音识别服务返回错误"));
        assert!(!unknown.contains("拒绝"));
    }

    #[test]
    fn invalid_status_code_never_falls_back_to_success() {
        for payload in [br#"{"code":"invalid"}"#.as_slice(), br#"{"status_code":null}"#.as_slice(), b"[]".as_slice()] {
            assert!(validate_response_payload(payload, "", None).is_err());
        }
        let round = SpeechRound::new();
        let error = validate_response_payload(br#"{"code":45000081,"message":"synthetic detail"}"#, "trace", Some(&round)).unwrap_err();
        assert!(error.contains("code=45000081"));
        assert_eq!(round.state().service_code, Some(45_000_081));
        assert_eq!(round.state().failure_stage, "provider_response");
    }

    #[test]
    fn response_limits_reject_gzip_expansion_and_trailing_frame_data() {
        let compressed = gzip_compress(&vec![b'x'; MAX_SERVER_PAYLOAD_BYTES + 1]).unwrap();
        assert!(gzip_decompress(&compressed).is_err());
        let mut frame = server_frame(&json!({"result":{"text":"synthetic"}}), true);
        assert!(parse_server_frame(&frame).is_ok());
        frame.push(0);
        assert!(parse_server_frame(&frame).is_err());
        frame[0] = 0x21;
        assert!(parse_server_frame(&frame).is_err());
    }

    #[test]
    fn streaming_audio_sends_first_chunk_without_waiting_for_another_packet() {
        let mut pending = PendingAudioPacket::default();
        assert_eq!(pending.push(vec![1, 2, 3, 4, 5, 6]), Some(vec![1, 2, 3, 4]));
        assert_eq!(pending.push(vec![7, 8, 9, 10]), Some(vec![5, 6, 7, 8]));
        assert_eq!(pending.finish(), Some(vec![9, 10]));
        assert_eq!(pending.finish(), None);

        let mut empty = PendingAudioPacket::default();
        assert_eq!(empty.finish(), None);
    }

    #[test]
    fn single_pcm_packet_has_a_real_final_tail_and_exact_wire_bytes() {
        let pcm: Vec<u8> = (0..640).map(|n| (n % 251) as u8).collect();
        let mut pending = PendingAudioPacket::default();
        let first = pending.push(pcm.clone()).expect("first PCM must be sent immediately");
        assert_eq!(first.len(), pcm.len() - PCM_SAMPLE_BYTES);
        let tail = pending.finish().expect("final frame must contain a real sample");
        assert_eq!(tail, pcm[pcm.len() - PCM_SAMPLE_BYTES..]);
        let regular = encode_audio_request(&first, false).unwrap();
        let final_frame = encode_audio_request(&tail, true).unwrap();
        assert_eq!(regular[1], 0x20);
        assert_eq!(final_frame[1], 0x22);
        let mut delivered = gzip_decompress(&regular[8..]).unwrap();
        delivered.extend(gzip_decompress(&final_frame[8..]).unwrap());
        assert_eq!(delivered, pcm);
    }

    #[test]
    fn one_sample_fragments_preserve_order_without_silence_or_replay() {
        let mut pending = PendingAudioPacket::default();
        let mut delivered = Vec::new();
        let pcm: Vec<u8> = (0..80).collect();
        for sample in pcm.chunks(PCM_SAMPLE_BYTES) {
            if let Some(audio) = pending.push(sample.to_vec()) {
                assert_eq!(audio.len() % PCM_SAMPLE_BYTES, 0);
                delivered.extend(audio);
            }
        }
        delivered.extend(pending.finish().unwrap());
        assert_eq!(delivered, pcm);
        assert_eq!(pending.push(Vec::new()), None);
        assert_eq!(pending.finish(), None);
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
        let decoded = validate_response_payload(&frame.payload, "", None).unwrap();
        assert_eq!(extract_transcript(&decoded), "你好，继续任务。");
    }

    #[test]
    fn resource_id_rejects_header_injection() {
        assert_eq!(normalize_resource_id("volc.bigasr.sauc.duration").unwrap(), DEFAULT_RESOURCE_ID);
        assert!(normalize_resource_id("volc.seedasr.sauc.duration\r\nX-Test: bad").is_err());
        assert_eq!(
            normalize_resource_id("").unwrap(),
            DEFAULT_RESOURCE_ID.to_string()
        );
    }

    #[test]
    fn unconfigured_asr_status_requests_a_user_key() {
        let status = public_status(false, DEFAULT_RESOURCE_ID.to_string(), "none", "请配置火山引擎语音识别 API Key");
        assert!(!status.configured);
        assert_eq!(status.credential_source, "none");
        assert_eq!(status.resource_id, DEFAULT_RESOURCE_ID);
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

    #[test]
    fn empty_terminal_packet_keeps_the_last_nonempty_partial() {
        let events = Arc::new(std::sync::Mutex::new(Vec::new()));
        let captured = events.clone();
        let callback: SpeechCallback = Arc::new(move |event| captured.lock().unwrap().push(event));
        let mut state = TranscriptState::default();
        state.emit("请继续".into(), false, &callback);
        state.emit(String::new(), false, &callback);
        state.emit(String::new(), true, &callback);
        assert!(matches!(events.lock().unwrap().last(), Some(StreamingSpeechEvent::Final {text, ..}) if text == "请继续"));
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
        let worker = tokio::spawn(run_streaming_session(config, receiver, callback, cancelled, Arc::new(SpeechRound::new())));

        for chunk in pcm.chunks(640) {
            commands
                .send(StreamingSpeechCommand::Pcm(QueuedPcm { pcm: chunk.to_vec(), budget: None }))
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
