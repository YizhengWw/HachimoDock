/*
 * [Input] A start request from the UI (board id, appearance id, persona system prompt, greeting,
 *         voice spec), targeted saved-persona updates, continuous 16 kHz PCM frames from the board's `audio/chunk` stream while a
 *         session is live, `realtime_chat` button events, and stop requests.
 * [Output] The realtime persona conversation loop: energy VAD → Doubao streaming ASR → persona LLM
 *          (streamed) → Doubao TTS sentences → paced `audio/play_*` playback on the board, plus
 *          `ui/conversation` HUD updates, `realtime-chat-state` events for the UI, and the
 *          `realtime-chat-request` event that lets the UI resolve the active appearance when the
 *          board key is pressed. AEC/VAD-capable boards stay listening during replies;
 *          barge-in cancels generation, TTS and queued playback. Older boards use half-duplex.
 * [Pos] Tauri-side orchestrator for 实时对话; independent from the Agent Session Bus voice path.
 * [Sync] Update `pc/.folder.md` and `pc/docs/realtime-chat.md`; wire changes also update `firmware/protocol.md`.
 */

use base64::{engine::general_purpose::STANDARD as BASE64, Engine as _};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc as std_mpsc, Arc, Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tauri::{AppHandle, Emitter, Manager};
use tokio::sync::mpsc;
use tokio::time::timeout;

use crate::doubao_tts::{scale_pcm16, DoubaoTtsClient};
use crate::persona_llm::{self, ChatTurn, SentenceSplitter};
use crate::usb_serial::UsbSerialManager;
use crate::voice_chat_settings;
use crate::realtime_chat_log as diagnostics;
use crate::volcengine_asr::{StreamingSpeechEvent, StreamingSpeechRecognizer};

pub const STATE_EVENT: &str = "realtime-chat-state";
pub const REQUEST_EVENT: &str = "realtime-chat-request";
pub const ACTION_ID: &str = "realtime_chat";

const FRAME_BYTES: usize = 640; // 20 ms @ 16 kHz mono s16le
const PREROLL_FRAMES: usize = 30; // 600 ms, includes device VAD debounce and PC confirmation
const SPEECH_START_FRAMES: usize = 3; // 60 ms above threshold
const SPEECH_END_FRAMES: usize = 35; // 700 ms of silence
const MAX_UTTERANCE_MS: u64 = 15_000;
const IDLE_TIMEOUT: Duration = Duration::from_secs(60);
const ASR_FINAL_TIMEOUT: Duration = Duration::from_secs(8);
const FINAL_CAPTURE_LIMIT: usize = 576; // > 8 s final wait + existing 128-frame ingress
const POST_SPEAK_GUARD: Duration = Duration::from_millis(250);
const PLAYBACK_LEAD_MS: u64 = 1200;
const PLAY_CHUNK_BYTES: usize = 3200; // 100 ms
const VAD_ABS_MIN: f32 = 380.0;
const VAD_NOISE_RATIO: f32 = 2.6;
// AEC can leave tiny speech-shaped residuals that WebRTC VAD labels as speech.
// This is a signal-quality floor, not a playback mute: near-end speech is still
// evaluated on every frame while the speaker is active.
const PROCESSED_VAD_ABS_MIN: f32 = 160.0;
const PROCESSED_SPEECH_START_FRAMES: usize = 8; // 160 ms; reject brief AEC onset transients
const LISTENING_VAD_ABS_MIN: f32 = 96.0;
const LISTENING_SPEECH_START_FRAMES: usize = 5; // 100 ms; reject isolated low-energy hints
const PREVIEW_MAX_SECONDS: u64 = 20;

#[derive(Debug, Clone, Serialize, Deserialize, Default, PartialEq)]
#[serde(rename_all = "camelCase", default)]
pub struct VoiceSpec {
    pub speaker: String,
    pub clone_speaker_id: String,
    pub speed: f32,
    pub volume: f32,
}

impl VoiceSpec {
    fn speed(&self) -> f32 {
        if self.speed.is_finite() && self.speed > 0.0 { self.speed } else { 1.0 }
    }
    fn volume(&self) -> f32 {
        if self.volume.is_finite() && self.volume > 0.0 { self.volume } else { 1.0 }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RealtimeChatStartInput {
    pub board_device_id: String,
    pub appearance_id: String,
    pub display_name: String,
    pub system_prompt: String,
    #[serde(default)]
    pub greeting: String,
    #[serde(default)]
    pub voice: VoiceSpec,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PersonaVoicePreviewInput {
    pub text: String,
    #[serde(default)]
    pub voice: VoiceSpec,
}

#[derive(Debug, Clone, Serialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct RealtimeChatStatus {
    pub active: bool,
    pub state: String,
    pub session_id: String,
    pub board_device_id: String,
    pub appearance_id: String,
    pub display_name: String,
    pub transcript: String,
    pub reply: String,
    pub error: String,
    pub reason: String,
    pub started_at_ms: u64,
    pub turns: u32,
}

enum Input {
    Pcm(Vec<u8>, Option<bool>),
}

struct Session {
    id: String,
    board_device_id: String,
    cancelled: AtomicBool,
    duplex: AtomicBool,
    stop_notify: tokio::sync::Notify,
    input: mpsc::Sender<Input>,
    status: Mutex<RealtimeChatStatus>,
    completed_playback: Mutex<String>,
    pending_persona: Mutex<Option<RealtimeChatStartInput>>,
}

fn active_slot() -> &'static Mutex<Option<Arc<Session>>> {
    static SLOT: OnceLock<Mutex<Option<Arc<Session>>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

fn active_session() -> Option<Arc<Session>> {
    active_slot().lock().ok().and_then(|slot| slot.clone())
        .filter(|session| session.status.lock().map(|s| s.active).unwrap_or(false))
}

fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}

pub fn status() -> RealtimeChatStatus {
    match active_slot().lock().ok().and_then(|slot| slot.clone()) {
        Some(session) => session.status.lock().map(|s| s.clone()).unwrap_or_default(),
        None => RealtimeChatStatus::default(),
    }
}

fn publish(app: &AppHandle, session: &Session, update: impl FnOnce(&mut RealtimeChatStatus)) {
    let snapshot = match session.status.lock() {
        Ok(mut status) => {
            let previous = status.state.clone();
            update(&mut status);
            if status.state != previous {
                diagnostics::record(&session.id, "state", json!({"state": status.state, "turn": status.turns}));
            }
            status.clone()
        }
        Err(_) => return,
    };
    let _ = app.emit(STATE_EVENT, &snapshot);
}

fn send_board(usb: &UsbSerialManager, board: &str, topic: &str, payload: Value) {
    if let Err(error) = usb.send_to_board(board, topic, &payload) {
        eprintln!("[realtime-chat] send {topic} failed: {error}");
    }
}

fn hud(usb: &UsbSerialManager, board: &str, session_id: &str, state: &str, name: &str, text: &str) {
    send_board(
        usb,
        board,
        "ui/conversation",
        json!({ "sessionId": session_id, "state": state, "name": name, "text": text }),
    );
}

fn user_caption(usb: &UsbSerialManager, board: &str, id: &str, name: &str, text: &str, final_text: bool) {
    send_board(usb, board, "ui/conversation", json!({"sessionId":id,"state":"listening",
        "name":name,"text":text,"role":"user","final":final_text}));
}

/// Called from the USB message dispatcher: consume `audio/*` topics while a session is live.
/// Returns true when the message belongs to realtime chat (so push-to-talk logic must skip it).
pub fn route_device_audio(topic: &str, payload: &Value) -> bool {
    if topic == "audio/diagnostic" {
        if payload.get("event").and_then(Value::as_str) == Some("playback_completed") {
            if let (Some(session), Some(id)) = (active_session(), payload.get("sessionId").and_then(Value::as_str)) {
                if id.starts_with(&format!("{}-", session.id)) {
                    if let Ok(mut completed) = session.completed_playback.lock() { *completed = id.to_owned(); }
                }
            }
        }
        let id = active_session().map(|s| s.id.clone()).unwrap_or_default();
        // Device metadata is allowlisted; never log the full audio payload.
        diagnostics::record(&id, "device", json!({"stage": payload.get("event"), "bytes": payload.get("bytes"),
            "bufferedMs": payload.get("bufferedMs"), "uptimeMs": payload.get("uptimeMs"), "seq":payload.get("seq"), "ok":payload.get("ok"), "firmware":payload.get("firmware")}));
        return true;
    }
    let Some(session) = active_session() else { return false };
    if topic == "protocol/ack" && payload.get("ok").and_then(Value::as_bool) == Some(false)
        && payload.get("requestTopic").and_then(Value::as_str).map(|s| s.starts_with("audio/")).unwrap_or(false) {
        let stage = match payload["requestTopic"].as_str().unwrap_or("") {
            "audio/play_chunk" => "audio_play_chunk", "audio/play_begin" => "audio_play_begin",
            "audio/play_end" => "audio_play_end", "audio/play_subtitle" => "audio_play_subtitle",
            "audio/conversation" => "audio_conversation", _ => "audio_other",
        };
        let reason = match payload["code"].as_str().unwrap_or("") {
            "audio_chunk_rejected" => "audio_chunk_rejected", "audio_chunk_invalid" => "audio_chunk_invalid",
            "audio_playback_failed" => "audio_playback_failed", "invalid_subtitle" => "invalid_subtitle",
            _ => "audio_command_rejected",
        };
        diagnostics::record(&session.id, "protocol_rejected", json!({"stage":stage,"reason":reason,"ok":false}));
        if let Ok(mut s) = session.status.lock() {
            s.error = if reason == "audio_chunk_rejected" || reason == "audio_chunk_invalid" {
                "设备未能接收音频数据，已停止播放；请提供实时对话日志排查 USB 传输".into()
            } else { "设备拒绝音频指令，请检查固件及设备音频状态".into() };
        }
        request_stop(&session, "device_protocol_error");
        return true;
    }
    if !topic.starts_with("audio/") { return false; }
    if session.cancelled.load(Ordering::SeqCst) {
        return topic.starts_with("audio/");
    }
    if topic == "audio/status" && session.duplex.load(Ordering::SeqCst) && payload["audioAec"] == false {
        if let Ok(mut s) = session.status.lock() {s.error="设备回声消除不可用，已停止双向对话，请重新进入或检查固件".into();}
        request_stop(&session,"aec_unavailable");
        return true;
    }
    if topic == "audio/chunk" {
        let processed = payload["aec"] == true && payload["vadSpeech"].is_boolean();
        let duplex = session.duplex.load(Ordering::SeqCst);
        if duplex && !processed {
            // Never route raw speaker echo into ASR/tool calls after an AFE failure.
            diagnostics::record(&session.id, "aec_fallback", json!({"ok":false}));
            if let Ok(mut s) = session.status.lock() {s.error="设备回声消除不可用，已停止双向对话，请重新进入或检查固件".into();}
            request_stop(&session, "aec_unavailable");
            return true;
        }
        if !duplex && !session.status.lock().map(|s| s.state == "listening").unwrap_or(false) { return true; }
        if let Some(data) = payload.get("data").and_then(Value::as_str) {
            if let Ok(pcm) = BASE64.decode(data) {
                if pcm.len() != FRAME_BYTES { return true; }
                if session.input.try_send(Input::Pcm(pcm, if duplex {payload["vadSpeech"].as_bool()} else {None})).is_err() {
                    if let Ok(mut s) = session.status.lock() { s.error = "收音处理队列繁忙，请重新开始对话".into(); }
                    request_stop(&session, "capture_overflow");
                }
            }
        }
        return true;
    }
    if topic == "audio/error" {
        let message = payload.get("error").or_else(|| payload.get("message")).and_then(Value::as_str).unwrap_or("device audio error");
        if let Ok(mut s) = session.status.lock() { s.error = format!("设备音频错误：{message}"); }
        request_stop(&session, "device_audio_error");
        return true;
    }
    // audio/begin, audio/end, audio/status: informational while the conversation runs
    true
}

/// Called from the USB message dispatcher for `input/event`. Handles the `realtime_chat` action:
/// a live session stops; otherwise the UI is asked to start one for the current appearance.
pub fn handle_input_event(app: &AppHandle, payload: &Value) -> bool {
    if let Some(session) = active_session() {
        if payload.get("boardDeviceId").and_then(Value::as_str) == Some(session.board_device_id.as_str()) {
            if payload.get("action").and_then(Value::as_str) == Some(ACTION_ID) { request_stop(&session, "key"); }
            return true; // No Agent/session navigation or submission while realtime owns this board.
        }
    }
    if payload.get("action").and_then(Value::as_str) != Some(ACTION_ID) {
        return false;
    }
    let board = payload.get("boardDeviceId").and_then(Value::as_str).unwrap_or("").to_string();
    let context = payload.get("context").and_then(Value::as_str).unwrap_or("main").to_string();
    if let Some(session) = active_session() {
        request_stop(&session, "key");
        return true;
    }
    if context != "main" {
        // The board already shows a hint when the pet screen is not open; nothing to start.
        return true;
    }
    let _ = app.emit(REQUEST_EVENT, json!({ "boardDeviceId": board, "context": context, "at": now_ms() }));
    true
}

pub fn stop(reason: &str) -> RealtimeChatStatus {
    if let Some(session) = active_session() {
        request_stop(&session, reason);
        return session.status.lock().map(|s| s.clone()).unwrap_or_default();
    }
    RealtimeChatStatus::default()
}

fn request_stop(session: &Session, reason: &str) {
    crate::smart_home::cancel_voice_session(&session.id);
    if let Ok(mut s) = session.status.lock() { s.reason = reason.to_string(); }
    session.cancelled.store(true, Ordering::SeqCst);
    session.stop_notify.notify_one();
}

pub fn start(app: &AppHandle, input: RealtimeChatStartInput) -> Result<RealtimeChatStatus, String> {
    static START_GUARD: OnceLock<Mutex<()>> = OnceLock::new();
    let _guard = START_GUARD.get_or_init(|| Mutex::new(())).lock().map_err(|_| "实时对话启动锁异常")?;
    let board = input.board_device_id.trim().to_string();
    if board.is_empty() {
        return Err("缺少设备 id".to_string());
    }
    if input.system_prompt.trim().is_empty() {
        return Err("这个形象还没有人设，先在画廊里点「人设与声音」设置".to_string());
    }
    let settings = voice_chat_settings::status()?;
    if !settings.llm_configured {
        return Err("对话大模型未配置，请在「API 配置」填写大模型 API Key".to_string());
    }
    if !settings.tts_configured {
        return Err("豆包 TTS 未配置，请在「API 配置」填写语音合成 API Key".to_string());
    }
    if !crate::volcengine_asr::settings_status()?.configured {
        return Err("语音识别未配置，请先在 API 配置填写 ASR Key".into());
    }
    let usb = app.state::<UsbSerialManager>().inner().clone();
    let usb_status = usb.status();
    if !usb_status.connected || usb_status.board_device_id != board {
        return Err("设备未通过 USB 连接，实时对话需要设备在线".to_string());
    }
    if active_session().is_some() { return Err("已有实时对话，请先结束后再开始".into()); }

    let (tx, rx) = mpsc::channel(128);
    let session_id = format!("rtc-{}", now_ms());
    let session = Arc::new(Session {
        id: session_id.clone(),
        board_device_id: board.clone(),
        cancelled: AtomicBool::new(false),
        duplex: AtomicBool::new(["audioAec","audioVad","audioFullDuplex"].iter().all(|key| usb_status.capabilities["features"][*key] == true)),
        stop_notify: tokio::sync::Notify::new(),
        input: tx,
        completed_playback: Mutex::new(String::new()),
        pending_persona: Mutex::new(None),
        status: Mutex::new(RealtimeChatStatus {
            active: true,
            state: "preparing".to_string(),
            session_id: session_id.clone(),
            board_device_id: board.clone(),
            appearance_id: input.appearance_id.clone(),
            display_name: input.display_name.clone(),
            started_at_ms: now_ms(),
            ..RealtimeChatStatus::default()
        }),
    });
    if let Ok(mut slot) = active_slot().lock() {
        *slot = Some(session.clone());
    }
    let snapshot = session.status.lock().map(|s| s.clone()).unwrap_or_default();
    let _ = app.emit(STATE_EVENT, &snapshot);

    let app_clone = app.clone();
    tauri::async_runtime::spawn(async move {
        diagnostics::record(&session.id, "started", json!({}));
        tokio::select! {
            biased;
            _ = session.stop_notify.notified() => {
                let (reason, error) = session.status.lock().map(|s| (s.reason.clone(), s.error.clone())).unwrap_or_default();
                finish(&app_clone, &usb, &session, &reason, &error).await;
            }
            _ = run_session(app_clone.clone(), usb.clone(), session.clone(), input, rx) => {}
        }
    });
    Ok(snapshot)
}

// ---- energy VAD ---------------------------------------------------------------------------

struct Vad {
    noise: f32,
    processed_noise: f32,
    speech_run: usize,
    silence_run: usize,
    in_speech: bool,
    preroll: Vec<Vec<u8>>,
    speech_started: Option<Instant>,
    soft_gap: usize,
    metrics: VadMetrics,
    diagnostic_session: String,
    reply_tail_until: Option<Instant>,
}

#[derive(Default)]
struct VadMetrics {
    frames: usize,
    hints: usize,
    rejected: usize,
    peak: f32,
    starts: usize,
    reply_frames: usize,
}

#[derive(Default)]
struct AsrCaptureStats { frames: usize, peak: f32 }
impl AsrCaptureStats {
    fn observe(&mut self, frame: &[u8]) {
        self.frames += 1;
        self.peak = self.peak.max(Vad::rms(frame));
    }
    fn record(&self, session: &str, turn: u32) {
        diagnostics::record(session, "asr_audio_submitted", json!({"turn":turn,"frames":self.frames,
            "bytes":self.frames*FRAME_BYTES,"peakRms":self.peak.round()}));
    }
}

#[derive(Clone, Copy)]
enum VadContext { Listening, Reply }

enum VadStep {
    Idle,
    Start(Vec<Vec<u8>>),
    Speech,
    End,
}

impl Vad {
    fn new() -> Self {
        Self { noise: 200.0, processed_noise: 16.0, speech_run: 0, silence_run: 0, in_speech: false, preroll: Vec::new(), speech_started: None, soft_gap: 0, metrics: VadMetrics::default(), diagnostic_session: String::new(), reply_tail_until: None }
    }

    fn rms(frame: &[u8]) -> f32 {
        let mut acc = 0f64;
        let mut n = 0usize;
        for pair in frame.chunks_exact(2) {
            let v = i16::from_le_bytes([pair[0], pair[1]]) as f64;
            acc += v * v;
            n += 1;
        }
        if n == 0 { 0.0 } else { (acc / n as f64).sqrt() as f32 }
    }

    fn step(&mut self, frame: &[u8]) -> VadStep {
        self.step_with_hint(frame, None)
    }

    fn step_with_hint(&mut self, frame: &[u8], speech: Option<bool>) -> VadStep {
        self.step_in_context(frame, speech, VadContext::Reply)
    }

    fn step_in_context(&mut self, frame: &[u8], speech: Option<bool>, context: VadContext) -> VadStep {
        let context = if self.reply_tail_until.is_some_and(|until| Instant::now() < until) { VadContext::Reply } else { context };
        let level = Self::rms(frame);
        let floor = if matches!(context, VadContext::Listening) { LISTENING_VAD_ABS_MIN } else { PROCESSED_VAD_ABS_MIN };
        if !self.in_speech {
            // slow-tracking noise floor, only while nobody is talking
            self.noise = if level < self.noise { self.noise * 0.9 + level * 0.1 } else { self.noise * 0.995 + level * 0.005 };
        }
        let threshold = (self.noise * VAD_NOISE_RATIO).max(VAD_ABS_MIN);
        let voiced = match speech {
            Some(hint) => {
                if !self.in_speech && !hint {
                    self.processed_noise = if level < self.processed_noise {
                        self.processed_noise * 0.9 + level * 0.1
                    } else { self.processed_noise * 0.995 + level * 0.005 };
                }
                let credible = if self.in_speech {
                    (self.processed_noise * 1.4).max(LISTENING_VAD_ABS_MIN / 2.0)
                } else { (self.processed_noise * VAD_NOISE_RATIO).max(floor) };
                hint && level >= credible
            }
            None => level >= threshold,
        };
        self.metrics.frames += 1;
        self.metrics.reply_frames += usize::from(matches!(context, VadContext::Reply));
        self.metrics.hints += usize::from(speech == Some(true));
        self.metrics.rejected += usize::from(speech == Some(true) && !voiced);
        self.metrics.peak = self.metrics.peak.max(level);
        self.log_metrics();
        if !self.in_speech {
            self.preroll.push(frame.to_vec());
            if self.preroll.len() > PREROLL_FRAMES {
                self.preroll.remove(0);
            }
            if voiced {
                self.speech_run += 1;
                let start_frames = if speech.is_none() { SPEECH_START_FRAMES }
                    else if matches!(context, VadContext::Listening) { LISTENING_SPEECH_START_FRAMES }
                    else { PROCESSED_SPEECH_START_FRAMES };
                if self.speech_run >= start_frames {
                    self.metrics.starts += 1;
                    self.in_speech = true;
                    self.silence_run = 0;
                    self.speech_started = Some(Instant::now());
                    let preroll = std::mem::take(&mut self.preroll);
                    return VadStep::Start(preroll);
                }
            } else {
                // Keep short, speech-labelled energy dips (consonants) without
                // counting them as evidence. A false device VAD immediately
                // breaks the run, preserving onset-echo rejection during playback.
                self.soft_gap += 1;
                if speech != Some(true) || self.soft_gap > 2 {
                    self.speech_run = 0;
                    self.soft_gap = 0;
                }
            }
            return VadStep::Idle;
        }
        if voiced {
            self.silence_run = 0;
        } else {
            self.silence_run += 1;
        }
        let too_long = self
            .speech_started
            .map(|t| t.elapsed() >= Duration::from_millis(MAX_UTTERANCE_MS))
            .unwrap_or(false);
        if self.silence_run >= SPEECH_END_FRAMES || too_long {
            self.in_speech = false;
            self.speech_run = 0;
            self.soft_gap = 0;
            self.silence_run = 0;
            self.speech_started = None;
            return VadStep::End;
        }
        VadStep::Speech
    }

    fn reset(&mut self) {
        self.in_speech = false;
        self.speech_run = 0;
        self.soft_gap = 0;
        self.silence_run = 0;
        self.preroll.clear();
        self.speech_started = None;
    }

    fn log_metrics(&mut self) {
        if self.metrics.frames < 250 { return; }
        let m = std::mem::take(&mut self.metrics);
        if self.diagnostic_session.is_empty() { return; }
        diagnostics::record(&self.diagnostic_session, "vad_window", json!({"frames":m.frames,"hintFrames":m.hints,
            "rejectedFrames":m.rejected,"peakRms":m.peak.round(),"count":m.starts,"replyFrames":m.reply_frames}));
    }
}

// ---- board playback sender ------------------------------------------------------------------

enum PlayerCmd {
    Begin(String),
    Subtitle(String),
    Pcm(Vec<u8>),
    End(std_mpsc::Sender<()>),
    Flush,
}

#[derive(Clone)]
struct Player {
    tx: std_mpsc::Sender<(u64, PlayerCmd)>,
    epoch: Arc<AtomicU64>,
}
impl Player {
    fn send(&self, cmd: PlayerCmd) -> Result<(), ()> {
        let epoch = if matches!(&cmd, PlayerCmd::Flush) {
            self.epoch.fetch_add(1, Ordering::SeqCst) + 1
        } else { self.epoch.load(Ordering::SeqCst) };
        self.tx.send((epoch, cmd)).map_err(|_| ())
    }
}

/// Dedicated thread: packs PCM into 100 ms `audio/play_chunk` frames and paces them so the board
/// never holds more than ~1.2 s of audio ahead of real time.
fn spawn_player(usb: UsbSerialManager, board: String, session: Arc<Session>) -> Player {
    let subtitles = usb.status().capabilities.pointer("/features/conversationSubtitles").and_then(Value::as_bool) == Some(true);
    let await_device = subtitles && active_session().map(|s| s.id == session.id).unwrap_or(false);
    let (tx, rx) = std_mpsc::channel::<(u64, PlayerCmd)>();
    let epoch = Arc::new(AtomicU64::new(0));
    let player = Player { tx, epoch: epoch.clone() };
    thread::spawn(move || {
        let mut current_epoch = 0;
        let mut turn_id = String::new();
        let mut pending: Vec<u8> = Vec::new();
        let mut seq: u32 = 0;
        let mut begin_at: Option<Instant> = None;
        let mut sent_bytes: u64 = 0;
        let flush_chunk = |usb: &UsbSerialManager, turn_id: &str, seq: &mut u32, chunk: &[u8], begin_at: &mut Option<Instant>, sent_bytes: &mut u64, expected: u64| {
            if chunk.is_empty() {
                return;
            }
            let started = *begin_at.get_or_insert_with(Instant::now);
            let lead_ms = (*sent_bytes * 1000 / 32_000) as i64 - started.elapsed().as_millis() as i64;
            let wait = Duration::from_millis(lead_ms.saturating_sub(PLAYBACK_LEAD_MS as i64).max(0) as u64);
            let until = Instant::now() + wait;
            while Instant::now() < until && !session.cancelled.load(Ordering::SeqCst) && epoch.load(Ordering::SeqCst) == expected { thread::sleep(Duration::from_millis(10)); }
            if session.cancelled.load(Ordering::SeqCst) || epoch.load(Ordering::SeqCst) != expected { return; }
            let write_started = Instant::now();
            let result = usb.send_to_board(
                &board,
                "audio/play_chunk",
                &json!({ "sessionId": turn_id, "seq": *seq, "bytes": chunk.len(), "data": BASE64.encode(chunk) }),
            );
            let elapsed_ms = write_started.elapsed().as_millis();
            // Metadata only: no PCM, transcript, key, port or personal path.
            if *seq < 3 || *seq % 10 == 0 || elapsed_ms >= 80 || result.is_err() {
                diagnostics::record(&session.id, "playback_tx", json!({"seq":*seq,
                    "bytes":chunk.len(),"elapsedMs":elapsed_ms,"ok":result.is_ok()}));
            }
            if result.is_err() {
                if let Ok(mut status) = session.status.lock() { status.error = "设备音频传输失败，请检查 USB 连接后重试".into(); }
                request_stop(&session, "playback_write_failed");
                return;
            }
            *seq += 1;
            *sent_bytes += chunk.len() as u64;
        };
        while let Ok((expected, cmd)) = rx.recv() {
            if session.cancelled.load(Ordering::SeqCst) { break; }
            if expected != epoch.load(Ordering::SeqCst) { continue; }
            if current_epoch != expected {
                pending.clear(); begin_at = None; sent_bytes = 0;
                send_board(&usb, &board, "audio/play_flush", json!({"sessionId":turn_id}));
                current_epoch = expected;
            }
            match cmd {
                PlayerCmd::Begin(id) => {
                    if let Ok(mut completed) = session.completed_playback.lock() { completed.clear(); }
                    turn_id = id;
                    pending.clear();
                    seq = 0;
                    begin_at = None;
                    sent_bytes = 0;
                    send_board(
                        &usb,
                        &board,
                        "audio/play_begin",
                        json!({ "sessionId": turn_id, "sampleRate": 16000, "channels": 1, "format": "pcm_s16le" }),
                    );
                }
                PlayerCmd::Subtitle(text) => {
                    if subtitles {
                        send_board(&usb, &board, "audio/play_subtitle", json!({"sessionId":turn_id,
                            "offsetBytes":sent_bytes + pending.len() as u64,"text":text}));
                    }
                }
                PlayerCmd::Pcm(bytes) => {
                    pending.extend_from_slice(&bytes);
                    while pending.len() >= PLAY_CHUNK_BYTES {
                        let chunk: Vec<u8> = pending.drain(..PLAY_CHUNK_BYTES).collect();
                        flush_chunk(&usb, &turn_id, &mut seq, &chunk, &mut begin_at, &mut sent_bytes, expected);
                        if epoch.load(Ordering::SeqCst) != expected { pending.clear(); break; }
                    }
                }
                PlayerCmd::End(done) => {
                    let chunk = std::mem::take(&mut pending);
                    flush_chunk(&usb, &turn_id, &mut seq, &chunk, &mut begin_at, &mut sent_bytes, expected);
                    if epoch.load(Ordering::SeqCst) != expected { continue; }
                    send_board(&usb, &board, "audio/play_end", json!({ "sessionId": turn_id, "bytes": sent_bytes }));
                    if let Some(started) = begin_at {
                        let total = Duration::from_millis(sent_bytes * 1000 / 32_000);
                        let elapsed = started.elapsed();
                        if total > elapsed {
                            let until = Instant::now() + total - elapsed;
                            while Instant::now() < until && !session.cancelled.load(Ordering::SeqCst) && epoch.load(Ordering::SeqCst) == expected { thread::sleep(Duration::from_millis(10)); }
                        }
                    }
                    if await_device && sent_bytes > 0 {
                        // Do not clear the spoken caption or resume ASR while buffered audio is still playing.
                        let deadline = Instant::now() + Duration::from_secs(5);
                        while !session.cancelled.load(Ordering::SeqCst)
                            && epoch.load(Ordering::SeqCst) == expected
                            && !session.completed_playback.lock().map(|id| *id == turn_id).unwrap_or(false) {
                            if Instant::now() >= deadline {
                                if let Ok(mut status) = session.status.lock() { status.error = "设备播放完成确认超时，请检查 USB 连接".into(); }
                                request_stop(&session, "playback_timeout");
                                break;
                            }
                            thread::sleep(Duration::from_millis(10));
                        }
                    }
                    let _ = done.send(());
                    begin_at = None;
                    sent_bytes = 0;
                }
                PlayerCmd::Flush => {
                    pending.clear();
                    send_board(&usb, &board, "audio/play_flush", json!({ "sessionId": turn_id }));
                    begin_at = None;
                    sent_bytes = 0;
                }
            }
        }
    });
    player
}

// ---- session loop ---------------------------------------------------------------------------

async fn run_session(
    app: AppHandle,
    usb: UsbSerialManager,
    session: Arc<Session>,
    mut input: RealtimeChatStartInput,
    mut rx: mpsc::Receiver<Input>,
) {
    let board = session.board_device_id.clone();
    let mut name = input.display_name.clone();
    let voice = input.voice.clone();
    let mut volume = voice.volume();
    let reason: String;
    let mut history: Vec<ChatTurn> = Vec::new();
    crate::smart_home::prepare_agent_capabilities();

    let llm_cfg = voice_chat_settings::llm_config();
    let tts_cfg = voice_chat_settings::tts_config(&voice.speaker, &voice.clone_speaker_id, voice.speed());
    let (llm_cfg, tts_cfg) = match (llm_cfg, tts_cfg) {
        (Ok(l), Ok(t)) => (l, t),
        (Err(error), _) | (_, Err(error)) => {
            finish(&app, &usb, &session, "config", &error).await;
            return;
        }
    };
    let mut tts = DoubaoTtsClient::new(tts_cfg);
    let player = spawn_player(usb.clone(), board.clone(), session.clone());
    let duplex = session.duplex.load(Ordering::SeqCst);
    let mut vad = Vad::new();
    vad.diagnostic_session = session.id.clone();
    let mut pending_speech = None;
    let mut deferred_audio = VecDeque::new();
    diagnostics::record(&session.id, "audio_mode", json!({"state":if duplex {"full_duplex"} else {"half_duplex"}}));

    hud(&usb, &board, &session.id, "preparing", &name, "准备中…");

    // Validate TTS with the actual greeting before enabling continuous microphone capture.
    let greeting = input.greeting.trim().to_string();
    if !greeting.is_empty() {
        let mut greeting_pcm = Vec::new();
        if let Err(error) = tts.synthesize(&greeting, |pcm| greeting_pcm.extend_from_slice(pcm)).await {
            finish(&app, &usb, &session, "tts", &error).await;
            return;
        }
        scale_pcm16(&mut greeting_pcm, volume);
        if duplex {
            send_board(&usb, &board, "audio/conversation", json!({"enabled":true,"halfDuplex":false,"sessionId":session.id}));
        }
        diagnostics::record(&session.id, "greeting_ready", json!({"bytes":greeting_pcm.len()}));
        publish(&app, &session, |s| { s.state = "speaking".into(); s.reply = greeting.clone(); });
        hud(&usb, &board, &session.id, "speaking", &name, "");
        let _ = player.send(PlayerCmd::Begin(format!("{}-greeting", session.id)));
        let _ = player.send(PlayerCmd::Subtitle(greeting.clone()));
        let _ = player.send(PlayerCmd::Pcm(greeting_pcm));
        match listen_during_reply(async {wait_playback(&player).await; Ok(())}, &mut rx, &mut deferred_audio, &mut vad, duplex).await {
            Ok(frames) => pending_speech = frames,
            Err(error) => {finish(&app, &usb, &session, "audio", &error).await; return;}
        }
        if pending_speech.is_some() { let _ = player.send(PlayerCmd::Flush); }
    }
    if !duplex || greeting.is_empty() {
        send_board(&usb, &board, "audio/conversation", json!({ "enabled": true, "halfDuplex": !duplex, "sessionId": session.id }));
    }
    if !duplex { drain_pending(&mut rx); }
    publish(&app, &session, |s| s.state = "listening".into());
    hud(&usb, &board, &session.id, "listening", &name, "");

    let mut recognizer: Option<StreamingSpeechRecognizer> = None;
    let mut capture_stats = AsrCaptureStats::default();
    let mut asr_rx: Option<mpsc::UnboundedReceiver<StreamingSpeechEvent>> = None;
    let mut last_activity = Instant::now();
    let mut turns: u32 = 0;
    let mut latest_asr = String::new();
    let mut displayed_asr = String::new();
    let mut final_asr: Option<String> = None;
    let mut caption_sent_at = Instant::now();

    loop {
        if session.cancelled.load(Ordering::SeqCst) {
            reason = "cancelled".into();
            break;
        }
        if let Some(frames) = pending_speech.take() {
            latest_asr.clear(); displayed_asr.clear(); final_asr = None;
            last_activity = Instant::now();
            let (tx, events) = mpsc::unbounded_channel();
            match StreamingSpeechRecognizer::start(move |event| {let _ = tx.send(event);}) {
                Ok(rec) => {
                    capture_stats = AsrCaptureStats::default();
                    for frame in frames {
                        if let Err(error) = rec.push_pcm(&frame) {finish(&app,&usb,&session,"asr",&error).await; return;}
                        capture_stats.observe(&frame);
                    }
                    recognizer = Some(rec); asr_rx = Some(events);
                    publish(&app, &session, |s| {s.state="listening".into(); s.transcript.clear();});
                    user_caption(&usb,&board,&session.id,&name,"",false);
                    diagnostics::record(&session.id,"asr_started",json!({"turn":turns+1}));
                }
                Err(error) => {finish(&app,&usb,&session,"asr",&error).await; return;}
            }
        }
        match apply_pending_persona(&session, &mut input, &mut tts).await {
            Ok(true) => {
                name = input.display_name.clone();
                volume = input.voice.volume();
                publish(&app, &session, |s| s.display_name = name.clone());
                hud(&usb, &board, &session.id, "listening", &name, &latest_asr);
            }
            Ok(false) => {}
            Err(error) => { finish(&app, &usb, &session, "config", &error).await; return; }
        }
        if last_activity.elapsed() >= IDLE_TIMEOUT {
            reason = "idle".into();
            break;
        }
        if let Some(events) = asr_rx.as_mut() {
            while let Ok(event) = events.try_recv() {
                match event {
                    StreamingSpeechEvent::Partial { text, .. } => latest_asr = text,
                    StreamingSpeechEvent::Final { text, .. } => { latest_asr = text.clone(); final_asr = Some(text); }
                    StreamingSpeechEvent::Error(error) => {
                        finish(&app, &usb, &session, "asr", &error).await;
                        return;
                    }
                    StreamingSpeechEvent::Ready => {}
                }
            }
            if latest_asr != displayed_asr && caption_sent_at.elapsed() >= Duration::from_millis(200) {
                user_caption(&usb, &board, &session.id, &name, &latest_asr, false);
                publish(&app, &session, |s| s.transcript = latest_asr.clone());
                displayed_asr = latest_asr.clone();
                caption_sent_at = Instant::now();
            }
        }
        let next = if let Some(item) = deferred_audio.pop_front() { item } else { match timeout(Duration::from_millis(500), rx.recv()).await {
            Ok(Some(item)) => item,
            Ok(None) => { reason = "closed".into(); break; }
            Err(_) => continue,
        }};
        match next {
            Input::Pcm(pcm, speech) => {
                for frame in pcm.chunks(FRAME_BYTES) {
                    match vad.step_in_context(frame, speech, VadContext::Listening) {
                        VadStep::Idle => {}
                        VadStep::Start(preroll) => {
                            latest_asr.clear();
                            displayed_asr.clear();
                            final_asr = None;
                            user_caption(&usb, &board, &session.id, &name, "", false);
                            last_activity = Instant::now();
                            let (tx, rx_events) = mpsc::unbounded_channel();
                            match StreamingSpeechRecognizer::start(move |event| { let _ = tx.send(event); }) {
                                Ok(rec) => {
                                    capture_stats = AsrCaptureStats::default();
                                    for f in preroll {
                                        if let Err(error) = rec.push_pcm(&f) {
                                            finish(&app, &usb, &session, "asr", &error).await; return;
                                        }
                                        capture_stats.observe(&f);
                                    }
                                    // The trigger frame is already part of preroll.
                                    recognizer = Some(rec);
                                    asr_rx = Some(rx_events);
                                    publish(&app, &session, |s| { s.state = "listening".into(); s.transcript.clear(); });
                                    diagnostics::record(&session.id, "asr_started", json!({"turn": turns + 1}));
                                }
                                Err(error) => {
                                    finish(&app, &usb, &session, "asr", &error).await;
                                    return;
                                }
                            }
                        }
                        VadStep::Speech => {
                            if let Some(rec) = recognizer.as_ref() {
                                if let Err(error) = rec.push_pcm(frame) {
                                    finish(&app, &usb, &session, "asr", &error).await; return;
                                }
                                capture_stats.observe(frame);
                            }
                        }
                        VadStep::End => {
                            let Some(rec) = recognizer.take() else { continue };
                            let Some(mut events) = asr_rx.take() else { continue };
                            let asr_finish_at = Instant::now();
                            if let Err(error) = rec.push_pcm(frame).and_then(|_| rec.finish()) {
                                finish(&app, &usb, &session, "asr", &error).await; return;
                            }
                            capture_stats.observe(frame);
                            capture_stats.record(&session.id, turns+1);
                            publish(&app, &session, |s| s.state = "thinking".into());
                            hud(&usb, &board, &session.id, "thinking", &name, "正在处理…");
                            let final_result = match final_asr.take() {
                                Some(text) => Ok(text),
                                None => await_with_capture(await_final_text(&mut events, latest_asr.clone()), &mut rx, &mut deferred_audio, duplex).await,
                            };
                            let text = match final_result {
                                Ok(text) => text,
                                Err(error) => {
                                    finish(&app, &usb, &session, "asr", &error).await;
                                    return;
                                }
                            };
                            drop(rec);
                            let text = text.trim().to_string();
                            if text.is_empty() {
                                diagnostics::record(&session.id, "asr_empty", json!({"turn":turns+1,"elapsedMs":asr_finish_at.elapsed().as_millis(),"reason":"no_transcript"}));
                                publish(&app, &session, |s| s.state = "listening".into());
                                hud(&usb, &board, &session.id, "listening", &name, "没听清，请再说一次");
                                continue;
                            }
                            turns += 1;
                            if let Err(error) = apply_pending_persona(&session, &mut input, &mut tts).await {
                                finish(&app, &usb, &session, "config", &error).await; return;
                            }
                            name = input.display_name.clone();
                            volume = input.voice.volume();
                            publish(&app, &session, |s| s.display_name = name.clone());
                            let user_text = text.clone();
                            user_caption(&usb, &board, &session.id, &name, &user_text, true);
                            hud(&usb, &board, &session.id, "thinking", &name, "");
                            publish(&app, &session, |s| { s.transcript = user_text.clone(); s.turns = turns; });
                            diagnostics::record(&session.id, "asr_final", json!({"turn":turns,"chars":user_text.chars().count(),"elapsedMs":asr_finish_at.elapsed().as_millis()}));
                            vad.reset();
                            match listen_during_reply(answer_turn(&app, &usb, &session, &llm_cfg, &input.system_prompt, &mut history, &user_text, &mut tts, &player, volume, &name), &mut rx, &mut deferred_audio, &mut vad, duplex).await {
                                Ok(Some(frames)) => {
                                    // The reply future is now dropped: aborts LLM and TTS callbacks.
                                    let _ = player.send(PlayerCmd::Flush);
                                    tts.interrupt();
                                    crate::smart_home::cancel_voice_session(&session.id);
                                    diagnostics::record(&session.id,"barge_in",json!({"turn":turns,"bytes":frames.iter().map(Vec::len).sum::<usize>()}));
                                    history.push(ChatTurn{role:"user".into(),content:user_text.clone()});
                                    history.push(ChatTurn{role:"assistant".into(),content:"[回复被用户打断；已发出的设备操作不会撤回，也不可自动重复。]".into()});
                                    persona_llm::trim_history(&mut history);
                                    pending_speech = Some(frames);
                                }
                                Ok(None) => {}
                                Err(error) => {
                                    diagnostics::record(&session.id, "turn_failed", json!({"errorKind":diagnostics::error_kind(&error)}));
                                    crate::smart_home::clear_voice_pending(&session.id).await;
                                    let _ = player.send(PlayerCmd::Flush);
                                    publish(&app, &session, |s| s.error = error.clone());
                                    hud(&usb, &board, &session.id, "listening", &name, "刚才没说成，再说一遍？");
                                }
                            }
                            if !duplex {
                                tokio::time::sleep(POST_SPEAK_GUARD).await;
                                drain_pending(&mut rx);
                                vad.reset();
                            }
                            last_activity = Instant::now();
                            publish(&app, &session, |s| s.state = "listening".into());
                            hud(&usb, &board, &session.id, "listening", &name, "");
                        }
                    }
                }
            }
        }
    }
    if let Some(rec) = recognizer.take() { rec.cancel(); }
    let _ = player.send(PlayerCmd::Flush);
    finish(&app, &usb, &session, &reason, "").await;
}

/// Capture remains polled while the reply is thinking, synthesizing or playing.
/// Only AEC/VAD-tagged device PCM enables this branch. Dropping `reply` aborts
/// all its child work; the caller then fences queued playback before starting ASR.
async fn listen_during_reply<F>(reply: F, rx: &mut mpsc::Receiver<Input>, deferred: &mut VecDeque<Input>, vad: &mut Vad, duplex: bool)
    -> Result<Option<Vec<Vec<u8>>>, String>
where F: std::future::Future<Output=Result<(), String>> {
    if !duplex {reply.await?; return Ok(None);}
    tokio::pin!(reply);
    loop {
        // Process audio retained during the ASR final wait before starting a
        // reply. Any resumed speech can cancel the reply without losing its tail.
        let incoming = if let Some(input) = deferred.pop_front() { Some(input) } else { tokio::select! {
            result = &mut reply => {
                // Continue listening, but retain playback-level echo rejection
                // for the brief acoustic/DMA tail. Never discard these frames.
                vad.reply_tail_until = Some(Instant::now() + POST_SPEAK_GUARD);
                result?; return Ok(None);
            }
            incoming = rx.recv() => incoming,
        }};
        let Some(Input::Pcm(pcm, speech)) = incoming else {return Err("设备收音通道关闭".into());};
        if speech.is_none() {return Err("设备 AEC/VAD 不可用，请重新进入对话".into());}
        for frame in pcm.chunks(FRAME_BYTES) {
            if let VadStep::Start(preroll) = vad.step_with_hint(frame, speech) {return Ok(Some(preroll));}
        }
    }
}

/// ASR can take up to eight seconds to finalize; the live ingress queue only
/// holds 2.56 seconds. Keep draining it without discarding a user's next words.
/// The bounded backlog is replayed through the same VAD before any new reply.
async fn await_with_capture<F, T>(result: F, rx: &mut mpsc::Receiver<Input>, deferred: &mut VecDeque<Input>, duplex: bool) -> Result<T, String>
where F: std::future::Future<Output=Result<T, String>> {
    if !duplex { return result.await; }
    tokio::pin!(result);
    loop {
        tokio::select! {
            value = &mut result => return value,
            incoming = rx.recv() => {
                let Some(Input::Pcm(pcm, speech)) = incoming else {return Err("设备收音通道关闭".into());};
                if speech.is_none() || pcm.len() != FRAME_BYTES { return Err("设备 AEC/VAD 音频帧无效".into()); }
                if deferred.len() >= FINAL_CAPTURE_LIMIT { return Err("语音识别等待过久，收音缓冲已满，请重新开始对话".into()); }
                deferred.push_back(Input::Pcm(pcm, speech));
            }
        }
    }
}

fn drain_pending(rx: &mut mpsc::Receiver<Input>) {
    // Control cancellation has its own Notify and cannot be dropped with old microphone frames.
    while rx.try_recv().is_ok() {}
}

fn queue_persona_update(session: &Session, input: RealtimeChatStartInput) -> Result<bool, String> {
    let status = session.status.lock().map_err(|_| "实时对话状态锁异常")?;
    if !status.active || session.cancelled.load(Ordering::SeqCst) || status.appearance_id != input.appearance_id { return Ok(false); }
    if input.system_prompt.trim().is_empty() || input.display_name.trim().is_empty() { return Err("人设名称或内容不能为空".into()); }
    *session.pending_persona.lock().map_err(|_| "实时对话配置锁异常")? = Some(input);
    Ok(true)
}

async fn apply_pending_persona(session: &Session, current: &mut RealtimeChatStartInput, tts: &mut DoubaoTtsClient) -> Result<bool, String> {
    apply_pending_persona_with(session, current, tts, |voice| {
        voice_chat_settings::tts_config(&voice.speaker, &voice.clone_speaker_id, voice.speed())
    }).await
}

async fn apply_pending_persona_with(session: &Session, current: &mut RealtimeChatStartInput, tts: &mut DoubaoTtsClient,
    config_for: impl FnOnce(&VoiceSpec) -> Result<crate::doubao_tts::DoubaoTtsConfig, String>) -> Result<bool, String> {
    // Separate from the PCM queue: playback cleanup must never discard a saved update.
    let pending = session.pending_persona.lock().map_err(|_| "实时对话配置锁异常")?.take();
    let Some(next) = pending else { return Ok(false) };
    if next.voice.speaker != current.voice.speaker || next.voice.clone_speaker_id != current.voice.clone_speaker_id
        || next.voice.speed() != current.voice.speed() {
        let cfg = config_for(&next.voice)?;
        tts.reset().await;
        *tts = DoubaoTtsClient::new(cfg);
    }
    current.display_name = next.display_name;
    current.system_prompt = next.system_prompt;
    current.greeting = next.greeting;
    current.voice = next.voice;
    Ok(true)
}

#[tauri::command]
pub fn realtime_chat_update_persona(input: RealtimeChatStartInput) -> Result<bool, String> {
    match active_session() { Some(session) => queue_persona_update(&session, input), None => Ok(false) }
}

async fn await_final_text(events: &mut mpsc::UnboundedReceiver<StreamingSpeechEvent>, mut latest: String) -> Result<String, String> {
    let deadline = Instant::now() + ASR_FINAL_TIMEOUT;
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return if latest.is_empty() { Err("语音识别结果超时".into()) } else { Ok(latest) };
        }
        match timeout(remaining, events.recv()).await {
            Ok(Some(StreamingSpeechEvent::Final { text, .. })) => return Ok(text),
            Ok(Some(StreamingSpeechEvent::Partial { text, .. })) => latest = text,
            Ok(Some(StreamingSpeechEvent::Error(error))) => {
                return Err(error);
            }
            Ok(Some(StreamingSpeechEvent::Ready)) => {}
            Ok(None) | Err(_) => return if latest.is_empty() { Err("语音识别没有返回结果，请检查服务连接".into()) } else { Ok(latest) },
        }
    }
}

#[allow(clippy::too_many_arguments)]
async fn answer_turn(
    app: &AppHandle,
    usb: &UsbSerialManager,
    session: &Arc<Session>,
    llm_cfg: &persona_llm::LlmConfig,
    system_prompt: &str,
    history: &mut Vec<ChatTurn>,
    user_text: &str,
    tts: &mut DoubaoTtsClient,
    player: &Player,
    volume: f32,
    name: &str,
) -> Result<(), String> {
    let board = session.board_device_id.clone();
    let (sentence_tx, mut sentence_rx) = mpsc::unbounded_channel::<String>();
    let mut splitter = SentenceSplitter::default();
    let llm_cfg = llm_cfg.clone();
    let prompt = system_prompt.to_string();
    let history_snapshot = history.clone();
    let user = user_text.to_string();
    let turn_started = Instant::now();
    let log_session = session.id.clone();
    diagnostics::record(&session.id, "llm_started", json!({}));
    let llm_task = tokio::spawn(async move {
        let mut first_delta = true;
        let mut first_sentence = true;
        let result = persona_llm::stream_chat_in_session(&llm_cfg, &prompt, &history_snapshot, &user, Some(&log_session), |delta| {
            if first_delta {
                diagnostics::record(&log_session, "llm_first_delta", json!({"elapsedMs":turn_started.elapsed().as_millis()}));
                first_delta = false;
            }
            for sentence in splitter.push(delta) {
                if first_sentence {
                    diagnostics::record(&log_session, "llm_first_sentence", json!({"elapsedMs":turn_started.elapsed().as_millis()}));
                    first_sentence = false;
                }
                let _ = sentence_tx.send(sentence);
            }
        })
        .await;
        if let Some(rest) = splitter.flush() {
            if first_sentence {
                diagnostics::record(&log_session, "llm_first_sentence", json!({"elapsedMs":turn_started.elapsed().as_millis()}));
            }
            let _ = sentence_tx.send(rest);
        }
        result
    });
    struct AbortOnDrop(tokio::task::AbortHandle);
    impl Drop for AbortOnDrop { fn drop(&mut self) { self.0.abort(); } }
    let _abort = AbortOnDrop(llm_task.abort_handle());

    let turn_id = format!("{}-{}", session.id, now_ms());
    let mut spoken = false;
    let mut reply = String::new();
    while let Some(sentence) = sentence_rx.recv().await {
        if session.cancelled.load(Ordering::SeqCst) {
            break;
        }
        let clean = persona_llm::clean_spoken_text(&sentence);
        if clean.is_empty() {
            continue;
        }
        if !spoken {
            spoken = true;
            let _ = player.send(PlayerCmd::Begin(turn_id.clone()));
            publish(app, session, |s| s.state = "speaking".into());
            hud(usb, &board, &session.id, "speaking", name, "");
        }
        reply.push_str(&clean);
        let shown = reply.clone();
        publish(app, session, |s| s.reply = shown.clone());
        speak_one_logged(tts, player, volume, &clean, &session.id).await?;
    }
    let full = match llm_task.await {
        Ok(Ok(text)) => text,
        Ok(Err(error)) => {
            if spoken {
                wait_playback(player).await;
            }
            return Err(error);
        }
        Err(error) => return Err(format!("对话任务异常: {error}")),
    };
    if spoken {
        wait_playback(player).await;
    } else {
        let fallback = "嗯……我刚才走神了，你再说一遍？";
        let _ = player.send(PlayerCmd::Begin(turn_id.clone()));
        speak_one(tts, player, volume, fallback).await?;
        wait_playback(player).await;
    }
    crate::smart_home::arm_voice_pending(&session.id).await;
    diagnostics::record(&session.id, "turn_completed", json!({"chars": full.chars().count(),"elapsedMs":turn_started.elapsed().as_millis()}));
    history.push(ChatTurn { role: "user".into(), content: user_text.to_string() });
    history.push(ChatTurn { role: "assistant".into(), content: if full.trim().is_empty() { reply } else { full } });
    persona_llm::trim_history(history);
    Ok(())
}

async fn speak_one(tts: &mut DoubaoTtsClient, player: &Player, volume: f32, text: &str) -> Result<(), String> {
    speak_one_logged(tts, player, volume, text, "").await
}

async fn speak_one_logged(tts: &mut DoubaoTtsClient, player: &Player, volume: f32, text: &str, session_id: &str) -> Result<(), String> {
    let sender = player.clone();
    let started = Instant::now();
    let mut first_pcm = true;
    tts.synthesize(text, |pcm| {
        if first_pcm && !pcm.is_empty() {
            let _ = sender.send(PlayerCmd::Subtitle(text.to_string()));
            diagnostics::record(session_id, "tts_first_pcm", json!({"elapsedMs":started.elapsed().as_millis()}));
            first_pcm = false;
        }
        let mut chunk = pcm.to_vec();
        scale_pcm16(&mut chunk, volume);
        let _ = sender.send(PlayerCmd::Pcm(chunk));
    })
    .await
    .map(|_| ())
}

async fn wait_playback(player: &Player) {
    let (done_tx, done_rx) = std_mpsc::channel::<()>();
    if player.send(PlayerCmd::End(done_tx)).is_err() {
        return;
    }
    let _ = tauri::async_runtime::spawn_blocking(move || {
        let _ = done_rx.recv_timeout(Duration::from_secs(120));
    })
    .await;
}

async fn finish(app: &AppHandle, usb: &UsbSerialManager, session: &Arc<Session>, reason: &str, error: &str) {
    crate::smart_home::clear_voice_pending(&session.id).await;
    session.cancelled.store(true, Ordering::SeqCst);
    let board = session.board_device_id.clone();
    send_board(usb, &board, "audio/play_flush", json!({ "sessionId": session.id }));
    send_board(usb, &board, "audio/conversation", json!({ "enabled": false, "sessionId": session.id }));
    let name = session.status.lock().map(|s| s.display_name.clone()).unwrap_or_default();
    let hint = match reason {
        "tts" => "语音合成失败，请查看 PC 实时对话提示",
        "asr" => "语音识别失败，请查看 PC 实时对话提示",
        _ => "实时对话失败，请查看 PC 提示",
    };
    hud(usb, &board, &session.id, if error.is_empty() { "ended" } else { "error" }, &name, if error.is_empty() { "" } else { hint });
    diagnostics::record(&session.id, "ended", json!({"reason": reason, "ok":error.is_empty(),"errorKind": if error.is_empty() { "none" } else { diagnostics::error_kind(error) }}));
    // Keep the final snapshot so reconnecting/mounting UI can still retrieve the failure.
    let reason = reason.to_string();
    let error = error.to_string();
    publish(app, session, |s| {
        s.active = false;
        s.state = "ended".into();
        s.reason = reason.clone();
        if !error.is_empty() {
            s.error = error.clone();
        }
    });
}

// ---- preview -----------------------------------------------------------------------------------

pub async fn preview(input: PersonaVoicePreviewInput) -> Result<(), String> {
    let text = input.text.trim().to_string();
    if text.is_empty() {
        return Err("没有可试听的文本".to_string());
    }
    let cfg = voice_chat_settings::tts_config(&input.voice.speaker, &input.voice.clone_speaker_id, input.voice.speed())?;
    let mut client = DoubaoTtsClient::new(cfg);
    let mut pcm: Vec<u8> = Vec::new();
    client.synthesize(&text, |chunk| pcm.extend_from_slice(chunk)).await?;
    client.reset().await;
    scale_pcm16(&mut pcm, input.voice.volume());
    tauri::async_runtime::spawn_blocking(move || {
        crate::pc_playback::play_pcm16_mono_16k(&pcm, Duration::from_secs(PREVIEW_MAX_SECONDS))
    })
    .await
    .map_err(|error| format!("试听播放线程异常: {error}"))?
}

// ---- tauri commands --------------------------------------------------------------------------

#[tauri::command]
pub async fn realtime_chat_start(app: AppHandle, input: RealtimeChatStartInput) -> Result<RealtimeChatStatus, String> {
    start(&app, input)
}

#[tauri::command]
pub async fn realtime_chat_stop(reason: Option<String>) -> Result<RealtimeChatStatus, String> {
    Ok(stop(reason.as_deref().unwrap_or("ui")))
}

#[tauri::command]
pub async fn realtime_chat_status() -> Result<RealtimeChatStatus, String> {
    Ok(status())
}

#[tauri::command]
pub async fn persona_voice_preview(input: PersonaVoicePreviewInput) -> Result<(), String> {
    preview(input).await
}

#[tauri::command]
pub async fn load_voice_chat_settings() -> Result<voice_chat_settings::VoiceChatSettingsStatus, String> {
    voice_chat_settings::status()
}

#[tauri::command]
pub async fn save_voice_chat_settings(
    input: voice_chat_settings::VoiceChatSettingsInput,
) -> Result<voice_chat_settings::VoiceChatSettingsStatus, String> {
    voice_chat_settings::save(input)
}

#[tauri::command]
pub async fn clear_voice_chat_secret(which: String) -> Result<voice_chat_settings::VoiceChatSettingsStatus, String> {
    voice_chat_settings::clear_secret(&which)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn listening_accepts_short_soft_speech_without_weakening_playback_gate() {
        let mut listening = Vad::new();
        let mut replying = Vad::new();
        for _ in 0..60 {
            listening.step_in_context(&frame(15), Some(false), VadContext::Listening);
            replying.step_with_hint(&frame(15), Some(false));
        }
        for _ in 0..(LISTENING_SPEECH_START_FRAMES-1) {
            assert!(matches!(listening.step_in_context(&frame(100), Some(true), VadContext::Listening), VadStep::Idle));
        }
        let VadStep::Start(preroll) = listening.step_in_context(&frame(100), Some(true), VadContext::Listening) else { panic!("soft short words must trigger while listening") };
        assert_eq!(preroll.len(), PREROLL_FRAMES);
        for _ in 0..50 { assert!(matches!(replying.step_with_hint(&frame(100), Some(true)), VadStep::Idle)); }
    }

    #[test]
    fn processed_vad_preserves_onset_and_tolerates_brief_consonant_dip() {
        let mut vad = Vad::new();
        for _ in 0..25 { vad.step_with_hint(&frame(20), Some(false)); }
        for _ in 0..4 { assert!(matches!(vad.step_with_hint(&frame(200), Some(true)), VadStep::Idle)); }
        for _ in 0..2 { assert!(matches!(vad.step_with_hint(&frame(65), Some(true)), VadStep::Idle)); }
        for _ in 0..3 { assert!(matches!(vad.step_with_hint(&frame(200), Some(true)), VadStep::Idle)); }
        let VadStep::Start(preroll) = vad.step_with_hint(&frame(200), Some(true)) else { panic!("speech-labelled energy dip must not reset the whole word") };
        assert_eq!(preroll.len(), PREROLL_FRAMES);
        assert_eq!(preroll[preroll.len()-10], frame(200));
    }

    #[test]
    fn sparse_speech_hints_and_playback_tail_do_not_trigger_soft_gate() {
        let mut vad = Vad::new();
        for _ in 0..20 {
            assert!(matches!(vad.step_with_hint(&frame(200), Some(true)), VadStep::Idle));
            for _ in 0..2 { assert!(matches!(vad.step_with_hint(&frame(20), Some(true)), VadStep::Idle)); }
        }
        vad.reset();
        vad.reply_tail_until = Some(Instant::now() + POST_SPEAK_GUARD);
        for _ in 0..20 {
            assert!(matches!(vad.step_in_context(&frame(100), Some(true), VadContext::Listening), VadStep::Idle));
        }
    }

    #[test]
    fn listening_rejects_low_level_hints_and_short_post_playback_noise() {
        let mut vad=Vad::new();
        for _ in 0..250 {
            assert!(matches!(vad.step_in_context(&frame(85),Some(true),VadContext::Listening),VadStep::Idle));
        }
        for _ in 0..3 {
            assert!(matches!(vad.step_in_context(&frame(114),Some(true),VadContext::Listening),VadStep::Idle));
        }
        assert!(matches!(vad.step_in_context(&frame(20),Some(false),VadContext::Listening),VadStep::Idle));
        assert!(!vad.in_speech);
    }

    #[test]
    fn processed_vad_rejects_quiet_speech_shaped_echo_but_accepts_near_speech() {
        let mut vad = Vad::new();
        for _ in 0..150 {assert!(matches!(vad.step_with_hint(&frame(15),Some(false)),VadStep::Idle));}
        for _ in 0..50 {assert!(matches!(vad.step_with_hint(&frame(40),Some(true)),VadStep::Idle));}
        let mut started = false;
        for _ in 0..PROCESSED_SPEECH_START_FRAMES {
            if matches!(vad.step_with_hint(&frame(180),Some(true)),VadStep::Start(_)) {started=true;}
        }
        assert!(started, "speech must remain interruptible, even at modest processed level");
        assert!(matches!(vad.step_with_hint(&frame(45),Some(true)),VadStep::Speech));
    }

    #[test]
    fn processed_vad_ignores_short_onset_bursts_without_muting_sustained_speech() {
        let mut vad=Vad::new();
        for _ in 0..3 {
            for _ in 0..(PROCESSED_SPEECH_START_FRAMES-1) {
                assert!(matches!(vad.step_with_hint(&frame(2500),Some(true)),VadStep::Idle));
            }
            assert!(matches!(vad.step_with_hint(&frame(20),Some(false)),VadStep::Idle));
        }
        for _ in 0..(PROCESSED_SPEECH_START_FRAMES-1) {
            assert!(matches!(vad.step_with_hint(&frame(180),Some(true)),VadStep::Idle));
        }
        assert!(matches!(vad.step_with_hint(&frame(180),Some(true)),VadStep::Start(_)));
    }

    #[test]
    fn duplex_uses_processed_vad_and_preserves_interruption_preroll() {
        tauri::async_runtime::block_on(async {
            let (tx, mut rx) = mpsc::channel(32);
            let frame=vec![20; FRAME_BYTES];
            for _ in 0..10 {tx.send(Input::Pcm(frame.clone(),Some(false))).await.unwrap();}
            for _ in 0..(PROCESSED_SPEECH_START_FRAMES+1) {tx.send(Input::Pcm(frame.clone(),Some(true))).await.unwrap();}
            let dropped=Arc::new(AtomicBool::new(false));
            struct MarkDrop(Arc<AtomicBool>);
            impl Drop for MarkDrop {fn drop(&mut self){self.0.store(true,Ordering::SeqCst);}}
            let marker=MarkDrop(dropped.clone());
            let response=async move {let _marker=marker; std::future::pending::<Result<(),String>>().await};
            let mut vad=Vad::new();
            let frames=listen_during_reply(response,&mut rx,&mut VecDeque::new(),&mut vad,true).await.unwrap().unwrap();
            assert!(dropped.load(Ordering::SeqCst),"old generation must be cancelled");
            assert_eq!(frames.len(),10+PROCESSED_SPEECH_START_FRAMES); assert!(vad.in_speech);
            assert!(matches!(rx.try_recv(),Ok(Input::Pcm(_,Some(true)))),"new speech must not be drained");
            assert!(matches!(vad.step_with_hint(&frame,Some(true)),VadStep::Speech));
        });
    }

    #[test]
    fn duplex_rejects_raw_pcm_and_keeps_preroll_when_reply_finishes() {
        tauri::async_runtime::block_on(async {
            let (tx,mut rx)=mpsc::channel(8); let mut vad=Vad::new();
            tx.send(Input::Pcm(vec![0;FRAME_BYTES],None)).await.unwrap();
            assert!(listen_during_reply(std::future::pending(),&mut rx,&mut VecDeque::new(),&mut vad,true).await.is_err());
            vad.step_with_hint(&vec![0;FRAME_BYTES],Some(false));
            assert!(listen_during_reply(async{Ok(())},&mut rx,&mut VecDeque::new(),&mut vad,true).await.unwrap().is_none());
            assert_eq!(vad.preroll.len(),1);
        });
    }

    #[test]
    fn slow_asr_final_drains_live_queue_and_preserves_followup_speech() {
        tauri::async_runtime::block_on(async {
            let (tx, mut rx) = mpsc::channel(128);
            let (done, finished) = tokio::sync::oneshot::channel();
            let sender = tokio::spawn(async move {
                // More than the ingress capacity; simulates a slow final reply.
                for _ in 0..300 { tx.send(Input::Pcm(frame(15), Some(false))).await.unwrap(); }
                for _ in 0..10 { tx.send(Input::Pcm(frame(800), Some(true))).await.unwrap(); }
                // Wait for the consumer to collect every frame, without closing ingress.
                while tx.capacity() != 128 { tokio::task::yield_now().await; }
                let _ = done.send(());
                tx
            });
            let mut deferred = VecDeque::new();
            let text = await_with_capture(async {finished.await.unwrap(); Ok("first sentence")}, &mut rx, &mut deferred, true).await.unwrap();
            let _tx = sender.await.unwrap();
            assert_eq!(text, "first sentence"); assert_eq!(deferred.len(),310);
            let mut vad = Vad::new();
            let frames = listen_during_reply(async {panic!("resumed speech must be evaluated before generating a reply")}, &mut rx, &mut deferred, &mut vad, true).await.unwrap().unwrap();
            assert_eq!(frames.len(),PREROLL_FRAMES);
            assert_eq!(deferred.len(),10-PROCESSED_SPEECH_START_FRAMES);
        });
    }

    #[test]
    fn final_capture_backlog_is_bounded_and_rejects_raw_audio() {
        tauri::async_runtime::block_on(async {
            let (tx,mut rx)=mpsc::channel(2);
            let mut deferred=VecDeque::new();
            tx.send(Input::Pcm(frame(0),None)).await.unwrap();
            assert!(await_with_capture(std::future::pending::<Result<(),String>>(),&mut rx,&mut deferred,true).await.is_err());
            for _ in 0..FINAL_CAPTURE_LIMIT {deferred.push_back(Input::Pcm(frame(0),Some(false)));}
            tx.send(Input::Pcm(frame(0),Some(false))).await.unwrap();
            assert!(await_with_capture(std::future::pending::<Result<(),String>>(),&mut rx,&mut deferred,true).await.is_err());
            assert_eq!(deferred.len(),FINAL_CAPTURE_LIMIT);
        });
    }

    #[test]
    fn playback_flush_invalidates_already_queued_pcm_and_subtitles() {
        let (tx,rx)=std_mpsc::channel();
        let player=Player{tx,epoch:Arc::new(AtomicU64::new(0))};
        player.send(PlayerCmd::Begin("old".into())).unwrap();
        player.send(PlayerCmd::Pcm(vec![1;640])).unwrap();
        player.send(PlayerCmd::Subtitle("old".into())).unwrap();
        player.send(PlayerCmd::Flush).unwrap();
        player.send(PlayerCmd::Begin("new".into())).unwrap();
        let epoch=player.epoch.load(Ordering::SeqCst);
        for _ in 0..3 {assert_ne!(rx.recv().unwrap().0,epoch);}
        assert!(matches!(rx.recv().unwrap(),(1,PlayerCmd::Flush)));
        assert!(matches!(rx.recv().unwrap(),(1,PlayerCmd::Begin(id)) if id=="new"));
    }

    #[test]
    fn saved_persona_is_targeted_latest_wins_and_survives_audio_drain() {
        tauri::async_runtime::block_on(async {
            let (tx, mut rx) = mpsc::channel(128);
            let session = Session { id:"rtc-test".into(), board_device_id:"board".into(), cancelled:AtomicBool::new(false), duplex:AtomicBool::new(false),
                stop_notify:tokio::sync::Notify::new(), input:tx,
                status:Mutex::new(RealtimeChatStatus { active:true, appearance_id:"pet-a".into(), ..Default::default() }),
                completed_playback:Mutex::new(String::new()), pending_persona:Mutex::new(None) };
            let mut current = RealtimeChatStartInput { board_device_id:"board".into(), appearance_id:"pet-a".into(),
                display_name:"old".into(), system_prompt:"old prompt".into(), greeting:"hello".into(), voice:VoiceSpec::default() };
            let mut next = current.clone(); next.appearance_id = "pet-b".into();
            assert!(!queue_persona_update(&session, next.clone()).unwrap());
            next.appearance_id = "pet-a".into(); next.display_name = "new".into();
            next.voice.speaker = "new-speaker".into(); next.voice.speed = 1.2; next.voice.volume = 0.8;
            assert!(queue_persona_update(&session, next.clone()).unwrap());
            next.system_prompt = "latest prompt".into(); next.board_device_id.clear();
            assert!(queue_persona_update(&session, next).unwrap());
            session.input.try_send(Input::Pcm(vec![0; FRAME_BYTES], None)).unwrap(); drain_pending(&mut rx);
            let mut tts = DoubaoTtsClient::new(crate::doubao_tts::DoubaoTtsConfig::new("test", "old", "", 1.0, ""));
            assert!(apply_pending_persona_with(&session, &mut current, &mut tts, |voice| {
                assert_eq!(voice.speaker, "new-speaker"); assert_eq!(voice.speed, 1.2); assert_eq!(voice.volume, 0.8);
                Ok(crate::doubao_tts::DoubaoTtsConfig::new("test", &voice.speaker, "", voice.speed(), ""))
            }).await.unwrap());
            assert_eq!(current.system_prompt, "latest prompt"); assert_eq!(current.display_name, "new");
            assert_eq!(current.board_device_id, "board"); assert_eq!(current.appearance_id, "pet-a");
            let mut prompt_only = current.clone(); prompt_only.system_prompt = "next prompt".into(); prompt_only.voice.volume = 0.6;
            queue_persona_update(&session, prompt_only).unwrap();
            assert!(apply_pending_persona_with(&session, &mut current, &mut tts, |_| panic!("unchanged voice must reuse TTS")).await.unwrap());
            assert_eq!(current.voice.volume, 0.6);
            assert!(!apply_pending_persona_with(&session, &mut current, &mut tts, |_| panic!("no pending update")).await.unwrap());
            session.cancelled.store(true, Ordering::SeqCst);
            assert!(!queue_persona_update(&session, current).unwrap());
        });
    }

    #[test]
    fn displayed_asr_partial_is_preserved_when_final_stream_closes() {
        tauri::async_runtime::block_on(async {
            let (tx, mut rx) = mpsc::unbounded_channel();
            drop(tx);
            assert_eq!(await_final_text(&mut rx, "已经显示的语音".into()).await.unwrap(), "已经显示的语音");
            let (tx, mut rx) = mpsc::unbounded_channel();
            tx.send(StreamingSpeechEvent::Error("service failed".into())).unwrap();
            drop(tx);
            assert!(await_final_text(&mut rx, "partial".into()).await.is_err());
        });
    }

    #[test]
    fn cancellation_survives_microphone_queue_drain() {
        let (tx, mut rx) = mpsc::channel(128);
        let session = Session { id:"rtc-test".into(), board_device_id:"test".into(), cancelled:AtomicBool::new(false), duplex:AtomicBool::new(false),
            stop_notify:tokio::sync::Notify::new(), input:tx, status:Mutex::new(RealtimeChatStatus::default()), completed_playback:Mutex::new(String::new()), pending_persona:Mutex::new(None) };
        session.input.try_send(Input::Pcm(vec![0; FRAME_BYTES], None)).unwrap();
        request_stop(&session, "key");
        drain_pending(&mut rx);
        assert!(session.cancelled.load(Ordering::SeqCst));
        tauri::async_runtime::block_on(async { timeout(Duration::from_millis(100), session.stop_notify.notified()).await.unwrap(); });
    }

    #[test]
    #[cfg(target_os = "macos")]
    #[ignore = "explicit cloud integration test; requires PET_REALTIME_SETTINGS_DIR"]
    fn live_voice_pipeline() {
        let dir = std::path::PathBuf::from(std::env::var("PET_REALTIME_SETTINGS_DIR").unwrap());
        voice_chat_settings::configure_storage_dir(dir.clone()).unwrap();
        crate::llm_network::configure_storage_dir(dir.clone()).unwrap();
        crate::volcengine_asr::configure_storage_dir(dir).unwrap();
        tauri::async_runtime::block_on(async {
            let cfg = voice_chat_settings::tts_config("zh_female_vv_uranus_bigtts", "", 1.0).unwrap();
            let mut tts = DoubaoTtsClient::new(cfg);
            let mut history=Vec::new();
            for turn in 0..3 {
            let mut question = Vec::new();
            tts.synthesize("你好，请告诉我你是谁。", |pcm| question.extend_from_slice(pcm)).await.unwrap();
            let (tx, mut rx) = mpsc::unbounded_channel();
            let rec = StreamingSpeechRecognizer::start(move |e| { let _ = tx.send(e); }).unwrap();
            for chunk in question.chunks(FRAME_BYTES) {
                rec.push_pcm(chunk).unwrap();
                tokio::time::sleep(Duration::from_millis(20)).await;
            }
            rec.finish().unwrap();
            let text = await_final_text(&mut rx, String::new()).await.unwrap();
            assert!(text.contains("你") && text.contains("谁"), "ASR did not recognize the synthetic test sentence");
            let reply = persona_llm::stream_chat(&voice_chat_settings::llm_config().unwrap(), "你是桌面宠物小西。用中文一句话回答，不超过二十字。", &history, &text, |_| {}).await.unwrap();
            assert!(!reply.trim().is_empty());
            let mut bytes = 0;
            tts.synthesize(&reply, |pcm| bytes += pcm.len()).await.unwrap();
            assert!(bytes > 6400);
            history.push(ChatTurn{role:"user".into(),content:text.clone()});
            history.push(ChatTurn{role:"assistant".into(),content:reply.clone()});
            // Cancel an actual streaming TTS response after the first PCM arrives.
            // Synthetic near-end PCM exercises the production VAD/cancellation
            // boundary; the separate hardware test covers actual acoustic input.
            let (tx,mut rx)=mpsc::channel(32);
            let ready=Arc::new(tokio::sync::Notify::new());
            let started=ready.clone();
            let injection=tokio::spawn(async move {
                started.notified().await;
                for _ in 0..PROCESSED_SPEECH_START_FRAMES {
                    tx.send(Input::Pcm(frame(1000),Some(true))).await.unwrap();
                    tokio::time::sleep(Duration::from_millis(20)).await;
                }
                tx
            });
            let response=async {
                tts.synthesize("这是一段用于检验打断的较长回复。用户可以在我说话的时候提出新的问题。我会停止旧的回复，继续倾听新的内容。", |_|ready.notify_one()).await?;
                std::future::pending::<Result<(),String>>().await
            };
            let mut vad=Vad::new();
            let interruption=timeout(Duration::from_secs(30),listen_during_reply(response,&mut rx,&mut VecDeque::new(),&mut vad,true)).await.unwrap().unwrap();
            assert!(interruption.is_some());
            tts.interrupt();
            injection.await.unwrap();
            // Reuse the client after interruption; stale audio must not poison it.
            let mut resumed=0;
            tts.synthesize("好的，请继续。", |pcm|resumed+=pcm.len()).await.unwrap();
            assert!(resumed>6400);
            println!("PASS cloud turn={}: TTS → ASR → LLM → TTS; interruption and next TTS recovered; ASR chars={}, reply chars={}, PCM bytes={bytes}",turn+1,text.chars().count(),reply.chars().count());
            }
            tts.reset().await;
        });
    }

    #[test]
    #[cfg(target_os = "macos")]
    #[ignore = "exclusive P4 port; Mac speaker emits synthetic speech into the real device microphone; cloud ASR only"]
    fn live_device_near_end_asr() {
        let dir = std::path::PathBuf::from(std::env::var("PET_REALTIME_SETTINGS_DIR").unwrap());
        voice_chat_settings::configure_storage_dir(dir.clone()).unwrap();
        crate::llm_network::configure_storage_dir(dir.clone()).unwrap();
        crate::volcengine_asr::configure_storage_dir(dir).unwrap();
        // Generated stimulus only. Microphone PCM is never written to disk.
        let pcm = tauri::async_runtime::block_on(async {
            let cfg = voice_chat_settings::tts_config("zh_female_vv_uranus_bigtts", "", 1.0).unwrap();
            let mut tts = DoubaoTtsClient::new(cfg);
            let mut pcm = Vec::new();
            tts.synthesize("你好，请告诉我你是谁。", |data| pcm.extend_from_slice(data)).await.unwrap();
            tts.reset().await;
            pcm
        });
        let temp = tempfile::tempdir().unwrap();
        let wav = temp.path().join("synthetic-near-end.wav");
        let mut bytes = Vec::new();
        bytes.extend_from_slice(b"RIFF"); bytes.extend_from_slice(&(36u32 + pcm.len() as u32).to_le_bytes());
        bytes.extend_from_slice(b"WAVEfmt "); bytes.extend_from_slice(&16u32.to_le_bytes());
        bytes.extend_from_slice(&1u16.to_le_bytes()); bytes.extend_from_slice(&1u16.to_le_bytes());
        bytes.extend_from_slice(&16000u32.to_le_bytes()); bytes.extend_from_slice(&32000u32.to_le_bytes());
        bytes.extend_from_slice(&2u16.to_le_bytes()); bytes.extend_from_slice(&16u16.to_le_bytes());
        bytes.extend_from_slice(b"data"); bytes.extend_from_slice(&(pcm.len() as u32).to_le_bytes());
        bytes.extend_from_slice(&pcm); std::fs::write(&wav, bytes).unwrap();
        let usb = UsbSerialManager::new();
        let (tx, events) = std_mpsc::channel();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(), move |topic,payload| {let _=tx.send((topic,payload));}).unwrap();
        let board = usb.status().board_device_id;
        assert_eq!(board, std::env::var("P4_EXPECTED_BOARD_ID").unwrap());
        assert_eq!(usb.status().capabilities["features"]["audioFullDuplex"], true);
        struct Cleanup(UsbSerialManager, String, Option<std::process::Child>);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                if let Some(child)=self.2.as_mut() {let _=child.kill(); let _=child.wait();}
                let _=self.0.send_to_board(&self.1,"audio/conversation",&json!({"enabled":false}));
                self.0.disconnect();
            }
        }
        let mut cleanup = Cleanup(usb.clone(), board.clone(), None);
        usb.send_to_board(&board,"audio/conversation",&json!({"enabled":true,"halfDuplex":false,"sessionId":"rtc-near-test"})).unwrap();
        let began = Instant::now();
        let mut vad = Vad::new();
        let mut rec: Option<StreamingSpeechRecognizer> = None;
        let (asr_tx, mut asr_events) = mpsc::unbounded_channel();
        let compare_cache = std::env::var("P4_COMPARE_CACHE").as_deref() == Ok("1");
        let mut short_audio = Vec::new();
        let mut ended = false;
        let mut frames = 0;
        let mut hints = 0;
        let mut peak = 0f32;
        while began.elapsed() < Duration::from_secs(18) {
            if cleanup.2.is_none() && began.elapsed() > Duration::from_secs(2) {
                cleanup.2=Some(std::process::Command::new("/usr/bin/afplay").arg(&wav).spawn().unwrap());
            }
            let Ok((topic,p))=events.recv_timeout(Duration::from_millis(30)) else {continue};
            if topic=="audio/error" {panic!("device audio failure");}
            if topic!="audio/chunk" {continue;}
            assert_eq!(p["aec"],true);
            let data=BASE64.decode(p["data"].as_str().unwrap()).unwrap();
            frames+=1; hints+=usize::from(p["vadSpeech"]==true); peak=peak.max(Vad::rms(&data));
            match vad.step_in_context(&data,p["vadSpeech"].as_bool(),VadContext::Listening) {
                VadStep::Start(preroll) => {
                    if compare_cache {
                        for frame in preroll.iter().skip(preroll.len().saturating_sub(15)) {short_audio.extend_from_slice(frame);}
                    }
                    let asr_tx=asr_tx.clone();
                    let next=StreamingSpeechRecognizer::start(move |e| {let _=asr_tx.send(e);}).unwrap();
                    for frame in preroll {next.push_pcm(&frame).unwrap();}
                    rec=Some(next);
                }
                VadStep::Speech => {
                    if compare_cache {short_audio.extend_from_slice(&data);}
                    if let Some(rec)=rec.as_ref() {rec.push_pcm(&data).unwrap();}
                }
                VadStep::End => {if let Some(rec)=rec.as_ref() {
                    if compare_cache {short_audio.extend_from_slice(&data);}
                    rec.push_pcm(&data).unwrap(); rec.finish().unwrap(); ended=true; break;
                }}
                VadStep::Idle => {}
            }
        }
        // Stop capture before waiting for the cloud; this test never executes tools/LLM.
        usb.send_to_board(&board,"audio/conversation",&json!({"enabled":false})).unwrap();
        println!("near-end physical capture: frames={frames} hint_frames={hints} peak_rms={peak:.1} ended={ended}");
        assert!(ended,"external test speech did not produce a complete utterance; inspect physical output route/level and VAD metrics");
        let text=tauri::async_runtime::block_on(await_final_text(&mut asr_events,String::new())).unwrap();
        println!("near-end ASR chars={} expected_phrase={}",text.chars().count(),text.contains("你是谁"));
        if compare_cache {
            let short = tauri::async_runtime::block_on(async {
                let (tx,mut rx)=mpsc::unbounded_channel();
                let rec=StreamingSpeechRecognizer::start(move |e| {let _=tx.send(e);}).unwrap();
                for frame in short_audio.chunks(FRAME_BYTES) {
                    rec.push_pcm(frame).unwrap();
                    tokio::time::sleep(Duration::from_millis(20)).await;
                }
                rec.finish().unwrap();
                await_final_text(&mut rx,String::new()).await.unwrap()
            });
            let normalized = |s: &str| s.chars().filter(|c| c.is_alphanumeric()).collect::<String>();
            let expected = "你好请告诉我你是谁";
            println!("cache paired same capture: ms=300 chars={} exact={} keyword={}; ms=600 chars={} exact={} keyword={}",
                short.chars().count(),normalized(&short)==expected,short.contains("你是谁"),
                text.chars().count(),normalized(&text)==expected,text.contains("你是谁"));
        }
        assert!(text.contains("你是谁"),"captured speech did not retain the expected synthetic phrase");
        drop(rec);
    }

    #[test]
    #[cfg(target_os = "macos")]
    #[ignore = "requires exclusive P4_SERIAL_PORT, saved credentials; plays a short test sentence"]
    fn live_device_audio() {
        let dir = std::path::PathBuf::from(std::env::var("PET_REALTIME_SETTINGS_DIR").unwrap());
        voice_chat_settings::configure_storage_dir(dir.clone()).unwrap();
        crate::llm_network::configure_storage_dir(dir.clone()).unwrap();
        crate::volcengine_asr::configure_storage_dir(dir.clone()).unwrap();
        diagnostics::configure(&dir).unwrap();
        let port = std::env::var("P4_SERIAL_PORT").unwrap();
        let usb = UsbSerialManager::new();
        let (events_tx, events_rx) = std_mpsc::channel();
        usb.connect(&port, move |topic, payload| { let _ = events_tx.send((topic, payload)); }).unwrap();
        let board = usb.status().board_device_id;
        assert!(!board.is_empty());
        assert_eq!(board,std::env::var("P4_EXPECTED_BOARD_ID").unwrap());
        assert_eq!(usb.status().runtime, "esp-p4");
        assert!(["audioAec","audioVad","audioFullDuplex"].iter().all(|key| usb.status().capabilities["features"][*key]==true));
        struct Cleanup(UsbSerialManager, String);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = self.0.send_to_board(&self.1, "audio/conversation", &json!({"enabled":false}));
                let _ = self.0.send_to_board(&self.1, "ui/conversation", &json!({"state":"ended"}));
                self.0.disconnect();
            }
        }
        let _cleanup = Cleanup(usb.clone(), board.clone());
        let diag = usb.query_diagnostics(&board).unwrap();
        assert_eq!(diag["runtime"]["audioReady"], true);
        assert_eq!(diag["runtime"]["audioPlaybackReady"], true);
        let id = format!("rtc-hil-{}", now_ms());
        let (input, _) = mpsc::channel(128);
        let session = Arc::new(Session { id:id.clone(), board_device_id:board.clone(), cancelled:AtomicBool::new(false), duplex:AtomicBool::new(true),
            stop_notify:tokio::sync::Notify::new(), input, status:Mutex::new(RealtimeChatStatus::default()), completed_playback:Mutex::new(String::new()), pending_persona:Mutex::new(None) });
        let player = spawn_player(usb.clone(), board.clone(), session.clone());
        usb.send_to_board(&board,"audio/conversation",&json!({"enabled":true,"halfDuplex":false,"sessionId":id})).unwrap();
        tauri::async_runtime::block_on(async {
            let cfg = voice_chat_settings::tts_config("zh_male_naiqimengwa_mars_bigtts", "", 1.05).unwrap();
            let mut tts = DoubaoTtsClient::new(cfg);
            hud(&usb, &board, &id, "preparing", "音频测试", "");
            player.send(PlayerCmd::Begin(id.clone())).unwrap();
            hud(&usb, &board, &id, "speaking", "音频测试", "");
            speak_one(&mut tts, &player, 1.0, "你好，设备语音播放测试。请稍等。" ).await.unwrap();
            wait_playback(&player).await;
            tts.reset().await;
        });
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut played = false;
        let mut vad=Vad::new(); let mut false_starts=0; let mut duplex_frames=0;
        while Instant::now() < deadline {
            if let Ok((topic, payload)) = events_rx.recv_timeout(Duration::from_millis(100)) {
                if topic=="audio/chunk" {
                    assert_eq!(payload["aec"],true);
                    let pcm=BASE64.decode(payload["data"].as_str().unwrap()).unwrap();
                    duplex_frames+=1;
                    if matches!(vad.step_with_hint(&pcm,payload["vadSpeech"].as_bool()),VadStep::Start(_)) {false_starts+=1;}
                }
                if topic == "audio/diagnostic" {
                    diagnostics::record(&id, "device", json!({"stage":payload["event"],"bytes":payload["bytes"],"ok":payload["ok"]}));
                    if payload["event"] == "playback_completed" { played = true; break; }
                }
            }
        }
        assert!(played, "device did not confirm playback completion");
        assert!(duplex_frames>50,"full-duplex capture missing during actual cloud TTS playback");
        assert_eq!(false_starts,0,"actual cloud TTS echoed into production VAD");
        usb.send_to_board(&board, "audio/query", &json!({})).unwrap();
        hud(&usb, &board, &id, "listening", "收音测试", "");
        let deadline = Instant::now() + Duration::from_secs(4);
        let mut bytes = 0;
        let mut volume_confirmed = false;
        let mut peak_rms = 0f32;
        while Instant::now() < deadline {
            if let Ok((topic, payload)) = events_rx.recv_timeout(Duration::from_millis(100)) {
                if topic == "audio/chunk" {
                    let pcm = BASE64.decode(payload["data"].as_str().unwrap()).unwrap();
                    bytes += pcm.len();
                    peak_rms = peak_rms.max(Vad::rms(&pcm));
                } else if topic == "audio/diagnostic" || topic == "audio/status" || topic == "audio/error" || topic == "protocol/ack" {
                    if topic == "audio/status" && payload["playbackVolume"] == 100 { volume_confirmed = true; }
                    diagnostics::record(&id, "device", json!({"stage":payload["event"],"bytes":payload["bytes"],"ok":payload["ok"]}));
                    println!("capture topic={topic} event={} active={} enabled={} conversation={} code={}", payload["event"], payload["active"], payload["enabled"], payload["conversation"], payload["code"]);
                }
            }
            let _ = usb.send_to_board(&board, "system/heartbeat", &json!({}));
        }
        session.cancelled.store(true, Ordering::SeqCst);
        assert!(volume_confirmed, "device codec output volume is not confirmed at 100");
        assert!(bytes > 32_000, "device microphone did not provide sustained PCM: bytes={bytes}");
        diagnostics::record(&id, "hardware_passed", json!({"bytes":bytes,"ok":true}));
        println!("PASS actual cloud TTS device playback completed with {duplex_frames} duplex capture frames and zero echo-triggered starts; microphone bytes={bytes}, peak RMS={peak_rms:.1}");
    }

    #[test]
    #[cfg(target_os = "macos")]
    #[ignore = "exclusive P4 port and internal cloud defaults; repeats actual duplex device playback three times"]
    fn live_duplex_three_hardware_turns() {
        for turn in 1..=3 {
            live_device_audio();
            println!("PASS actual duplex device round={turn}");
        }
    }

    fn frame(amplitude: i16) -> Vec<u8> {
        let mut out = Vec::with_capacity(FRAME_BYTES);
        for i in 0..(FRAME_BYTES / 2) {
            let v = if i % 2 == 0 { amplitude } else { -amplitude };
            out.extend_from_slice(&v.to_le_bytes());
        }
        out
    }

    #[test]
    #[ignore = "exclusive P4 port; plays supplied test WAV and measures processed microphone metadata only"]
    fn live_device_duplex_echo_probe() {
        let wav=std::fs::read(std::env::var("P4_TEST_WAV").unwrap()).unwrap();
        assert_eq!(&wav[..4], b"RIFF"); assert_eq!(&wav[8..12], b"WAVE");
        let mut at=12; let mut pcm=None; let mut valid=false;
        while at+8<=wav.len() {
            let n=u32::from_le_bytes(wav[at+4..at+8].try_into().unwrap()) as usize;
            assert!(at+8+n<=wav.len()); let chunk=&wav[at+8..at+8+n];
            if &wav[at..at+4]==b"fmt " {
                valid=chunk.len()>=16 && chunk[..4]==[1,0,1,0] && chunk[4..8]==16000u32.to_le_bytes() && chunk[14..16]==[16,0];
            }
            if &wav[at..at+4]==b"data" {pcm=Some(chunk.to_vec());}
            at+=8+n+(n%2);
        }
        assert!(valid); let pcm=pcm.unwrap(); assert!(pcm.len()<=32_000*40 && pcm.len()>32_000);
        let repeats=std::env::var("P4_ECHO_REPEAT").ok().map(|v|v.parse::<usize>().unwrap()).unwrap_or(1);
        assert!((1..=2).contains(&repeats));
        let pcm=pcm.repeat(repeats); assert!(pcm.len()<=32_000*40);
        let usb=UsbSerialManager::new(); let (tx,events)=std_mpsc::channel();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move |topic,payload|{let _=tx.send((topic,payload));}).unwrap();
        let board=usb.status().board_device_id;
        assert_eq!(board,std::env::var("P4_EXPECTED_BOARD_ID").unwrap());
        let caps=usb.status().capabilities;
        println!("duplex capabilities aec={} vad={} duplex={}",caps["features"]["audioAec"],caps["features"]["audioVad"],caps["features"]["audioFullDuplex"]);
        assert!(["audioAec","audioVad","audioFullDuplex"].iter().all(|k|caps["features"][*k]==true));
        struct Cleanup(UsbSerialManager,String);
        impl Drop for Cleanup {fn drop(&mut self){let _=self.0.send_to_board(&self.1,"audio/play_flush",&json!({})); let _=self.0.send_to_board(&self.1,"audio/conversation",&json!({"enabled":false}));self.0.disconnect();}}
        let _cleanup=Cleanup(usb.clone(),board.clone());
        let id=format!("rtc-echo-{}",now_ms()); let (input,_)=mpsc::channel(128);
        let session=Arc::new(Session{id:id.clone(),board_device_id:board.clone(),cancelled:AtomicBool::new(false),duplex:AtomicBool::new(true),stop_notify:tokio::sync::Notify::new(),input,status:Mutex::new(Default::default()),completed_playback:Mutex::new(String::new()),pending_persona:Mutex::new(None)});
        let player=spawn_player(usb.clone(),board.clone(),session.clone());
        let raw_calibration=std::env::var("P4_ECHO_RAW").as_deref()==Ok("1");
        if raw_calibration {
            usb.send_to_board(&board,"audio/control",&json!({"action":"start"})).unwrap();
        } else {
            usb.send_to_board(&board,"audio/conversation",&json!({"enabled":true,"halfDuplex":false,"sessionId":id})).unwrap();
        }
        let began=Instant::now(); let mut started=false; let mut playing=false; let mut completed=false;
        let mut frames=[0usize;3]; let mut speech=[0usize;3]; let mut energy=[0f64;3];
        let mut max_run=0; let mut run=0; let mut faults=0;
        let mut production_vad=Vad::new(); let mut false_starts=0;
        let barge_test=std::env::var("P4_ECHO_BARGE").as_deref()==Ok("1");
        let mut barge_at: Option<Instant>=None;
        let mut flush_latency=None;
        let mut prompted=false;
        let mut credible_run=0; let mut max_credible_run=0;
        let mut voiced_levels: [Vec<f32>;3] = Default::default();
        let mut raw_mic=Vec::<i16>::new(); let mut playback_start_sample=0usize;
        let capture_only=std::env::var("P4_ECHO_CAPTURE_ONLY").as_deref()==Ok("1");
        let duration=Duration::from_millis(pcm.len() as u64*1000/32000);
        let deadline=Instant::now()+if capture_only {Duration::from_secs(5)} else {duration+Duration::from_secs(10)};
        while Instant::now()<deadline {
            if barge_test && !prompted && began.elapsed()>Duration::from_secs(7) {
                hud(&usb,&board,&id,"speaking","打断测试","现在请说：停一下，我有一个新问题。");
                println!("barge prompt: speak now while device continues playing"); prompted=true;
            }
            if flush_latency.is_some() && barge_at.map(|at|at.elapsed()>Duration::from_secs(3)).unwrap_or(false) {break;}
            if !capture_only && !started && began.elapsed()>Duration::from_secs(3) {
                println!("echo baseline frames={} (expected about 150 in 3 s)", frames[0]);
                if frames[0]<110 || faults>0 {break;}
                player.send(PlayerCmd::Begin(format!("{id}-play"))).unwrap();
                player.send(PlayerCmd::Pcm(pcm.clone())).unwrap();
                let (done,_)=std_mpsc::channel();player.send(PlayerCmd::End(done)).unwrap();started=true;
            }
            if let Ok((topic,p))=events.recv_timeout(Duration::from_millis(30)) {
                if topic=="audio/diagnostic" {
                    if p["event"]=="playback_started" {playing=true;playback_start_sample=raw_mic.len();}
                    if p["event"]=="playback_completed" {
                        playing=false;completed=true;
                        production_vad.reply_tail_until=Some(Instant::now()+POST_SPEAK_GUARD);
                    }
                    if p["event"]=="playback_flushed" {
                        if let Some(at)=barge_at {
                            if flush_latency.is_none() {flush_latency=Some(at.elapsed());playing=false;}
                        }
                    }
                    if p["event"]=="aec_fallback" || p["event"]=="playback_overflow" {faults+=1;}
                    if p["ok"]==false {println!("device diagnostic event={}",p["event"]);}
                }
                if topic=="audio/error" {faults+=1;println!("audio error code={}",p["code"]);}
                if topic=="audio/chunk" {
                    if !raw_calibration && p["aec"]!=true {faults+=1;continue;}
                    let data=BASE64.decode(p["data"].as_str().unwrap()).unwrap();
                    if raw_calibration {raw_mic.extend(data.chunks_exact(2).map(|b|i16::from_le_bytes([b[0],b[1]])));}
                    let phase=if playing{1}else if completed{2}else{0};
                    frames[phase]+=1;energy[phase]+=Vad::rms(&data) as f64;
                    if playing && p["vadSpeech"]==true && Vad::rms(&data)>=PROCESSED_VAD_ABS_MIN {
                        credible_run+=1;max_credible_run=max_credible_run.max(credible_run);
                    } else {credible_run=0;}
                    let context = if started && !completed && flush_latency.is_none() { VadContext::Reply } else { VadContext::Listening };
                    if let VadStep::Start(_) = production_vad.step_in_context(&data,p["vadSpeech"].as_bool(),context) {
                        if barge_test && prompted && barge_at.is_none() {
                            barge_at=Some(Instant::now());
                            player.send(PlayerCmd::Flush).unwrap();
                        } else if !barge_test || !prompted {false_starts+=1;}
                        println!("echo vad_start phase={phase} at_ms={} rms={:.1}",began.elapsed().as_millis(),Vad::rms(&data));
                    }
                    if p["vadSpeech"]==true {voiced_levels[phase].push(Vad::rms(&data));}
                    if p["vadSpeech"]==true {speech[phase]+=1;if playing {run+=1;max_run=max_run.max(run);}} else {run=0;}
                }
            }
        }
        session.cancelled.store(true,Ordering::SeqCst);
        if barge_test {hud(&usb,&board,&id,"ended","","");}
        for i in 0..3 {
            voiced_levels[i].sort_by(|a,b|a.total_cmp(b));
            let levels=&voiced_levels[i];
            println!("echo phase={i} frames={} voiced={} mean_rms={:.1} voiced_p95={:.1} voiced_max={:.1}",frames[i],speech[i],energy[i]/frames[i].max(1) as f64,
                levels.get(levels.len()*95/100).copied().unwrap_or(0.0),levels.last().copied().unwrap_or(0.0));
        }
        println!("echo production_vad_starts={false_starts} max_credible_ms={}",max_credible_run*20);
        println!("echo longest_voiced_ms={} playback_completed={completed} faults={faults}",max_run*20);
        if barge_test {
            assert_eq!(false_starts,0,"speaker echo interrupted before the speak prompt");
            let latency=flush_latency.expect("no near-end speech / device flush acknowledgement");
            println!("barge VAD-to-device-flush={} ms (codec DMA tail up to 30 ms; not mouth-to-stop latency)",latency.as_millis());
            assert!(latency<Duration::from_millis(300) && faults==0,"barge-in transport latency/fault");
            return;
        }
        if raw_calibration {
            let reference:Vec<f64>=pcm.chunks_exact(2).map(|b|i16::from_le_bytes([b[0],b[1]]) as f64).collect();
            let mut best=(0isize,0f64);
            for lag in (-4096isize..8192).step_by(8) {
                let mut dot=0f64;let mut x2=0f64;let mut y2=0f64;
                for i in (16000..reference.len().min(64000)).step_by(8) {
                    let j=playback_start_sample as isize+i as isize+lag;
                    if j<0 || j as usize>=raw_mic.len(){continue;}
                    let x=reference[i];let y=raw_mic[j as usize] as f64;
                    dot+=x*y;x2+=x*x;y2+=y*y;
                }
                let score=dot.abs()/(x2*y2).sqrt().max(1.0);
                if score>best.1 {best=(lag,score);}
            }
            println!("acoustic alignment mic_lag_samples={} mic_lag_ms={:.2} correlation={:.3} clipped={}/{}",best.0,best.0 as f64/16.0,best.1,raw_mic.iter().filter(|s|s.unsigned_abs()>32000).count(),raw_mic.len());
            assert!(completed && frames[1]>50 && faults==0,"raw calibration stream failed");
            return;
        }
        if capture_only {
            assert!(frames[0]>=200 && faults==0,"capture alone cannot keep real-time cadence");
            return;
        }
        assert!(completed && frames[1]>50 && faults==0,"full-duplex stream/codec failed");
        assert_eq!(false_starts,0,"speaker-only playback would trigger the production VAD; needs acoustic tuning");
    }

    #[test]
    fn vad_starts_on_sustained_speech_and_ends_after_silence() {
        let mut vad = Vad::new();
        for _ in 0..20 {
            assert!(matches!(vad.step(&frame(50)), VadStep::Idle));
        }
        let mut started = false;
        for _ in 0..SPEECH_START_FRAMES {
            if let VadStep::Start(preroll) = vad.step(&frame(6000)) {
                started = true;
                assert!(!preroll.is_empty());
            }
        }
        assert!(started);
        for _ in 0..5 {
            assert!(matches!(vad.step(&frame(6000)), VadStep::Speech));
        }
        let mut ended = false;
        for _ in 0..SPEECH_END_FRAMES {
            if let VadStep::End = vad.step(&frame(40)) {
                ended = true;
            }
        }
        assert!(ended);
    }

    #[test]
    fn realtime_key_events_are_recognized_by_action_id() {
        let payload = json!({ "action": "realtime_chat", "context": "main", "boardDeviceId": "b1" });
        assert_eq!(payload.get("action").and_then(Value::as_str), Some(ACTION_ID));
    }
}
