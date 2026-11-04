/*
 * [Input] A start request from the UI (board id, appearance id, persona system prompt, greeting,
 *         voice spec), global saved volume, targeted saved-persona updates, continuous 16 kHz PCM frames from the board's `audio/chunk` stream while a
 *         session is live, `realtime_chat` button events, and stop requests.
 * [Output] The realtime persona conversation loop: energy VAD → Doubao streaming ASR → persona LLM
 *          (streamed) → Doubao TTS sentences → paced `audio/play_*` playback on the board, plus
 *          `ui/conversation` HUD updates, `realtime-chat-state` events for the UI, and the
 *          `realtime-chat-request` event that lets the UI resolve the active appearance when the
 *          board key is pressed. AEC/VAD-capable boards stay listening during replies;
 *          candidate speech is checked by streaming ASR before barge-in cancels generation,
 *          TTS and queued playback. Empty/noise candidates leave the reply intact. Global gain applies once at the output boundary.
 * [Pos] Tauri-side orchestrator for 实时对话, with current-window Codex delegation,
 *       bounded session-local handoff history and isolated result announcements.
 * [Sync] Update `pc/.folder.md` and `pc/docs/realtime-chat.md`; wire changes also update `firmware/protocol.md`.
 */

use base64::{engine::general_purpose::STANDARD as BASE64, Engine as _};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
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

#[path = "realtime_barge.rs"]
mod barge_policy;
#[path = "realtime_audio_ack.rs"]
mod audio_ack;

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
const BARGE_CONFIRM_TIMEOUT: Duration = Duration::from_secs(3);
const BARGE_CONFIRM_MAX_FRAMES: usize = 150; // bounded even when Windows delivers queued frames in a burst
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
    pub search_state: String,
    pub search_error: String,
    pub device_stop_state: String,
}

fn clear_search(status: &mut RealtimeChatStatus) {
    status.search_state.clear();
    status.search_error.clear();
}

fn update_search(status: &mut RealtimeChatStatus, event: crate::web_search::Event) {
    match event {
        crate::web_search::Event::Started => { clear_search(status); status.search_state="searching".into(); },
        crate::web_search::Event::Finished(_) => {
            // Sources stay inside the read-only tool result, not the PC UI snapshot.
            status.search_state="completed".into(); status.search_error.clear();
        },
        crate::web_search::Event::Failed(error) => {
            clear_search(status); status.search_state="failed".into(); status.search_error=error;
        },
    }
}

enum Input {
    Pcm(Vec<u8>, Option<bool>),
}

struct CaptureTransport {
    usb: UsbSerialManager,
    connection: String,
    boot: String,
}

struct Session {
    id: String,
    board_device_id: String,
    transport: Option<CaptureTransport>,
    capture_stream: Mutex<Option<String>>,
    audio_requests: Arc<Mutex<audio_ack::Requests>>,
    audio_egress: Mutex<()>,
    cancelled: AtomicBool,
    duplex: AtomicBool,
    capture_armed: AtomicBool,
    capture_ready: AtomicBool,
    stop_notify: tokio::sync::Notify,
    input: mpsc::Sender<Input>,
    status: Mutex<RealtimeChatStatus>,
    completed_playback: Mutex<String>,
    pending_persona: Mutex<Option<RealtimeChatStartInput>>,
    playback_volume: AtomicU32,
}

impl Session {
    fn playback_volume(&self) -> f32 {
        f32::from_bits(self.playback_volume.load(Ordering::Acquire))
    }

    fn set_playback_volume(&self, voice: &VoiceSpec) {
        self.playback_volume.store(voice.volume().to_bits(), Ordering::Release);
    }
}

// Apply once, at the last PC playback boundary, after frame assembly and pacing.
// Raw queued TTS can then use a newly saved gain, even during a long sentence.
fn playback_pcm(session: &Session, raw: &[u8]) -> (Vec<u8>, f32) {
    let gain = session.playback_volume();
    let mut pcm = raw.to_vec();
    scale_pcm16(&mut pcm, gain);
    (pcm, gain)
}

fn active_slot() -> &'static Mutex<Option<Arc<Session>>> {
    static SLOT: OnceLock<Mutex<Option<Arc<Session>>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

fn refresh_device_stop_metadata(session:&Session){
    let confirmed=session.audio_requests.lock().map(|r|r.stopped()).unwrap_or(false);
    if confirmed&&session.cancelled.load(Ordering::SeqCst){
        if let Ok(mut status)=session.status.lock(){status.active=false;status.state="ended".into();status.device_stop_state="acknowledged_idle_observed".into();}
    }
}
fn active_session()->Option<Arc<Session>>{
    let session=active_slot().lock().ok().and_then(|slot|slot.clone())?;
    // A late exact stop ACK/status can resolve an earlier unknown without
    // another physical send. No new owner is admitted before this actual proof.
    refresh_device_stop_metadata(&session);
    let active=session.status.lock().map(|s|s.active).unwrap_or(false);
    active.then_some(session)
}

fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}

pub fn status() -> RealtimeChatStatus {
    match active_slot().lock().ok().and_then(|slot| slot.clone()) {
        Some(session) => {refresh_device_stop_metadata(&session);session.status.lock().map(|s|s.clone()).unwrap_or_default()},
        None => RealtimeChatStatus::default(),
    }
}

fn publish(app: &AppHandle, session: &Session, update: impl FnOnce(&mut RealtimeChatStatus)) {
    let Ok(owner) = active_slot().lock() else { return; };
    if !owner.as_ref().is_some_and(|current| std::ptr::eq(current.as_ref(), session)) { return; }
    let snapshot = match session.status.lock() {
        Ok(mut status) => {
            let previous = status.state.clone();
            let mut next = status.clone();
            update(&mut next);
            if session.cancelled.load(Ordering::SeqCst) {
                let ended=!next.active&&next.state=="ended";
                let stopping=next.active&&matches!(next.state.as_str(),"stopping"|"stop_unconfirmed");
                if !ended&&!stopping { return; }
                if stopping&&status.device_stop_state=="acknowledged_idle_observed"{return;}
                next.reason = status.reason.clone();
                next.error = status.error.clone();
            }
            *status = next;
            if status.state != previous {
                diagnostics::record(&session.id, "state", json!({"state": status.state, "turn": status.turns}));
            }
            status.clone()
        }
        Err(_) => return,
    };
    drop(owner); // Never hold the lifecycle mutex across external event handlers.
    if !active_slot().lock().ok().is_some_and(|slot| slot.as_ref()
        .is_some_and(|current| std::ptr::eq(current.as_ref(), session))) { return; }
    let _ = app.emit(STATE_EVENT, &snapshot);
}

fn send_board(usb: &UsbSerialManager, board: &str, topic: &str, payload: Value) {
    let owned = active_session().filter(|s| s.board_device_id == board
        && payload["sessionId"].as_str().is_some_and(|id| id == s.id || id.starts_with(&format!("{}-", s.id))));
    let result = if let Some(session) = owned {
        send_session_board(usb, &session, topic, &payload)
    } else if payload["sessionId"].as_str().is_some_and(|id| id.starts_with("rtc-")) {
        return; // Retired realtime UI/playback callbacks cannot target a successor.
    } else { usb.send_to_board(board, topic, &payload) };
    if let Err(error) = result {
        eprintln!("[realtime-chat] send {topic} failed: {error}");
    }
}

fn transport_current(session: &Session) -> bool {
    session.transport.as_ref().is_none_or(|scope| {
        let status = scope.usb.status();
        status.connected && status.board_device_id == session.board_device_id
            && status.connection_id == scope.connection && status.boot_id == scope.boot
    })
}

fn send_bound_session_payload(usb:&UsbSerialManager,session:&Session,topic:&str,payload:&Value)->Result<(),String>{
    if let Some(scope)=&session.transport{
        if !active_slot().lock().ok().is_some_and(|slot|slot.as_ref().is_some_and(|s|std::ptr::eq(s.as_ref(),session))){
            return Err("实时对话轮次已结束，本次指令未发送".into());
        }
        if !transport_current(session){return Err("实时对话 USB 连接已变化，本次指令未发送".into());}
        let generation=scope.connection.parse::<u64>().map_err(|_|"实时对话连接身份无效")?;
        usb.send_approval_bound(&session.board_device_id,generation,topic,payload)
    }else{usb.send_to_board(&session.board_device_id,topic,payload)}
}

/// One synchronous session ordering point. The request-state guard is gone
/// before invoking the physical writer; RX ACKs never acquire audio_egress.
fn with_ordinary_audio_egress<T>(session:&Session,write:impl FnOnce()->Result<T,String>)->Result<T,String>{
    let _egress=session.audio_egress.lock().map_err(|_|"实时音频发送顺序不可用，未发送")?;
    let closing={session.audio_requests.lock().map_err(|_|"实时音频回执状态不可用")?.closing()};
    if session.cancelled.load(Ordering::SeqCst)||closing{return Err("实时对话已停止接收普通音频指令，未发送".into());}
    write()
}

fn send_session_board(usb:&UsbSerialManager,session:&Session,topic:&str,payload:&Value)->Result<(),String>{
    with_ordinary_audio_egress(session,||{
    if session.transport.is_some()&&(!transport_current(session)
        ||!active_slot().lock().ok().is_some_and(|slot|slot.as_ref().is_some_and(|s|std::ptr::eq(s.as_ref(),session)))){
        return Err("实时对话所有者或连接已变化，未发送".into());
    }
    let mut request=payload.clone();
    if audio_ack::managed_topic(topic){
        let id=session.audio_requests.lock().map_err(|_|"实时音频回执状态不可用")?
            .issue(&session.id,topic,payload["sessionId"].as_str().unwrap_or(""))?;
        request["requestId"]=json!(id);
    }
    send_bound_session_payload(usb,session,topic,&request)
    })
}

fn send_player_board(usb: &UsbSerialManager, session: &Session, topic: &str, payload: Value) {
    if send_session_board(usb, session, topic, &payload).is_err() {
        request_failure(session, "playback_write_failed", Some("设备音频传输失败，请检查 USB 连接后重试"));
    }
}

async fn begin_capture(usb: &UsbSerialManager, session: &Session, duplex: bool) -> Result<(), String> {
    session.capture_ready.store(false, Ordering::SeqCst);
    session.capture_armed.store(true, Ordering::SeqCst);
    send_session_board(usb, session, "audio/conversation",
        &json!({"enabled":true,"halfDuplex": !duplex,"sessionId":session.id}))?;
    // Do not start the greeting speaker/reference clock until the DSP owner has
    // rebuilt its filters. Old firmware confirms through conversation status.
    timeout(Duration::from_secs(3), async {
        loop {
            if session.cancelled.load(Ordering::SeqCst) {
                let error=session.status.lock().map(|s|s.error.clone()).unwrap_or_default();
                return Err(if error.is_empty() {"设备收音启动已停止".into()} else {error});
            }
            if session.capture_ready.load(Ordering::SeqCst) { return Ok(()); }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    }).await.map_err(|_| "设备收音启动超时，请重新进入；若持续出现，请更新固件并导出实时对话日志".to_string())?
}

fn capture_payload_matches(session: &Session, payload: &Value) -> bool {
    session.capture_armed.load(Ordering::SeqCst)
        && payload.get("boardDeviceId").and_then(Value::as_str)
            .map(|id| id == session.board_device_id).unwrap_or(true)
        && payload.get("conversationSessionId").and_then(Value::as_str)
            .map(|id| id == session.id).unwrap_or(session.transport.is_none())
}

fn aec_unavailable(session: &Session, topic: &str, payload: &Value) {
    let failure = diagnostics::aec::failure(topic, payload, diagnostics::aec::Context {
        board_id: &session.board_device_id, timeline_id: &session.id,
        duplex: session.duplex.load(Ordering::SeqCst),
        armed: session.capture_armed.load(Ordering::SeqCst),
        ready: session.capture_ready.load(Ordering::SeqCst),
    });
    diagnostics::record(&session.id, "aec_fallback", failure.metadata);
    let error = format!("设备音频保护已停止双向对话（{}）。请重新进入以恢复；若持续出现，请更新固件并导出实时对话日志", failure.detail);
    // Same fail-closed stop and same ingress predicates: no raw echo to ASR.
    request_failure(session, "aec_unavailable", Some(&error));
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
    if let Some(session) = active_session() {
        if payload.get("boardDeviceId").and_then(Value::as_str)
            .is_some_and(|id| id != session.board_device_id) { return false; }
    }
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
            "bufferedMs": payload.get("bufferedMs"), "uptimeMs": payload.get("uptimeMs"), "seq":payload.get("seq"), "ok":payload.get("ok"), "firmware":payload.get("firmware"),
            "reason":diagnostics::aec::reported_reason(payload), "dspFrames":payload.get("dspFrames"), "dspLastUs":payload.get("dspLastUs"), "dspMaxUs":payload.get("dspMaxUs"),
            "dspAecUs":payload.get("dspAecUs"), "dspNsUs":payload.get("dspNsUs"), "dspVadUs":payload.get("dspVadUs"), "aecReferenceSamples":payload.get("aecReferenceSamples"),
            "internalFreeBytes":payload.get("internalFreeBytes"), "internalLargestBytes":payload.get("internalLargestBytes"),
            "captureTxLastUs":payload.get("captureTxLastUs"), "captureTxMaxUs":payload.get("captureTxMaxUs"), "captureStackMinBytes":payload.get("captureStackMinBytes")}));
        return true;
    }
    let Some(session) = active_session() else { return false };
    route_session_audio(&session, topic, payload)
}

fn route_session_audio(session: &Session, topic: &str, payload: &Value) -> bool {
    if let Some(scope) = &session.transport {
        if !transport_current(session) {
            request_failure(session, "capture_connection_changed", Some("设备连接已变化，实时对话已停止，请重新进入"));
            return false;
        }
        if topic.starts_with("audio/") || topic=="protocol/ack" {
            let incoming = &payload["_nativeUsbScope"];
            if !audio_ack::scope_matches((&session.board_device_id,&scope.connection,&scope.boot),
                (incoming["boardDeviceId"].as_str(),incoming["connectionId"].as_str(),incoming["bootId"].as_str(),incoming["runtime"].as_str())) { return false; }
        }
    }
    if payload.get("boardDeviceId").and_then(Value::as_str)
        .is_some_and(|id| id != session.board_device_id) { return false; }
    if topic == "protocol/ack" {
        let request_topic=payload["requestTopic"].as_str().unwrap_or("");
        // RTC never issues audio/control. Let notes/the actual owner consume
        // those ACKs; neither a foreign topic nor an unissued ID can stop RTC.
        if !audio_ack::managed_topic(request_topic)&&request_topic!="audio/query" { return false; }
        let invalid_session_metadata=["sessionId","conversationSessionId"].iter().any(|key|
            payload.get(*key).is_some_and(|value|!value.is_string()));
        // Establish actual issued ID/topic ownership before classifying bad
        // optional metadata. Foreign receipts cannot consume a pending ID;
        // a current malformed receipt is an invalid ACK, never a silent drop.
        let receipt=match session.audio_requests.lock(){
            Ok(mut requests)=>requests.accept(&session.id,request_topic,payload["requestId"].as_str().unwrap_or(""),
                if invalid_session_metadata{None}else{payload.get("sessionId").and_then(Value::as_str)},
                if invalid_session_metadata{None}else{payload.get("conversationSessionId").and_then(Value::as_str)}),
            Err(_)=>{request_failure(session,"audio_ack_state_unavailable",Some("音频回执状态不可用，已停止本轮对话"));return true;}
        };
        match receipt{
            audio_ack::Receipt::Unmatched|audio_ack::Receipt::Retired=>return true,
            audio_ack::Receipt::Terminal(kind)=>{
                if let Ok(mut requests)=session.audio_requests.lock(){
                    requests.terminal_receipt(kind,!invalid_session_metadata&&payload["ok"].is_boolean(),payload["ok"]==true);
                }
                return true;
            },
            audio_ack::Receipt::Probe=>{
                if let Ok(mut requests)=session.audio_requests.lock(){requests.probe_receipt(!invalid_session_metadata&&payload["ok"].is_boolean(),payload["ok"]==true);}
                return true;
            },
            audio_ack::Receipt::Current=>{},
        }
        if session.cancelled.load(Ordering::SeqCst){return true;}
        if invalid_session_metadata {
            request_failure(session,"device_protocol_ack_invalid",Some("设备音频回执会话字段无效，已停止本轮对话"));
            return true;
        }
        if payload["ok"]==true { return true; }
        if !session.capture_armed.load(Ordering::SeqCst) { return true; }
        if !payload["ok"].is_boolean() {
            request_failure(session,"device_protocol_ack_invalid",Some("设备音频回执格式无效，已停止本轮对话"));
            return true;
        }
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
        let error: String = if reason == "audio_chunk_rejected" || reason == "audio_chunk_invalid" {
                "设备未能接收音频数据，已停止播放；请提供实时对话日志排查 USB 传输".into()
            } else { "设备拒绝音频指令，请检查固件及设备音频状态".into() };
        request_failure(session, "device_protocol_error", Some(&error));
        return true;
    }
    if !topic.starts_with("audio/") { return false; }
    if session.cancelled.load(Ordering::SeqCst) {
        // Same admitted native scope was checked above. Only a post-ACK,
        // single-probe idle observation can confirm device quiescence.
        if topic=="audio/status"{
            let idle=["active","conversation","playbackActive","streamActive"].iter().all(|k|payload[*k]==false)
                &&payload["conversationSessionId"].as_str().is_some_and(|id|id.is_empty()||id==session.id);
            if let Ok(mut requests)=session.audio_requests.lock(){requests.observe_status(idle);}
        }
        return true;
    }
    // Ignore pre-entry PTT frames, previous capture timelines and stale status.
    // New firmware echoes the control session independently of capture sessionId.
    if payload.get("conversationSessionId").is_some_and(|id| id.as_str() != Some(session.id.as_str())) { return true; }
    // Device end/error may omit conversationSessionId. Only the capture ID
    // learned from a current-timeline begin can bind such a terminal packet.
    let capture_terminal = matches!(topic, "audio/end" | "audio/error")
        && payload["sessionId"].as_str().is_some_and(|id| !id.is_empty()
            && session.capture_stream.lock().ok().is_some_and(|capture| capture.as_deref() == Some(id)));
    if !capture_payload_matches(&session, payload) && !capture_terminal { return true; }
    if topic == "audio/begin" {
        if let Some(id) = payload["sessionId"].as_str().filter(|id| !id.is_empty()) {
            if let Ok(mut capture) = session.capture_stream.lock() { *capture = Some(id.to_owned()); }
        }
        return true;
    }
    if topic == "audio/status" {
        if payload["conversation"] != true || payload["audioAecPreparing"] == true { return true; }
        if session.duplex.load(Ordering::SeqCst) && payload["audioAec"] == false {
            aec_unavailable(&session, topic, payload);
        } else if !session.duplex.load(Ordering::SeqCst) || payload["audioAec"] == true {
            session.capture_ready.store(true, Ordering::SeqCst);
        }
        return true;
    }
    if topic == "audio/chunk" {
        if let Some(id) = payload["sessionId"].as_str() {
            if session.capture_stream.lock().ok().is_some_and(|capture| capture.as_deref().is_some_and(|current| current != id)) {
                return true;
            }
        }
        let processed = payload["aec"] == true && payload["vadSpeech"].is_boolean();
        let duplex = session.duplex.load(Ordering::SeqCst);
        if duplex && !processed {
            // Legacy firmware lacks timeline IDs: ignore its queued PTT frames
            // until explicit conversation readiness, bounded by begin_capture.
            if !session.capture_ready.load(Ordering::SeqCst) && payload.get("conversationSessionId").is_none() { return true; }
            // Never route raw speaker echo into ASR/tool calls after an AFE failure.
            aec_unavailable(&session, topic, payload);
            return true;
        }
        if processed { session.capture_ready.store(true, Ordering::SeqCst); }
        if !duplex && !session.status.lock().map(|s| s.state == "listening").unwrap_or(false) { return true; }
        if let Some(data) = payload.get("data").and_then(Value::as_str) {
            if let Ok(pcm) = BASE64.decode(data) {
                if pcm.len() != FRAME_BYTES {
                    request_failure(session, "capture_frame_invalid", Some("设备音频帧长度无效，本轮对话已停止"));
                    return true;
                }
                if let Some(id) = payload["sessionId"].as_str().filter(|id| !id.is_empty()) {
                    if let Ok(mut capture) = session.capture_stream.lock() {
                        if capture.is_none() { *capture = Some(id.to_owned()); }
                    }
                } else if session.transport.is_some() {
                    request_failure(session, "capture_frame_invalid", Some("设备音频帧缺少录音身份，本轮对话已停止"));
                    return true;
                }
                if session.input.try_send(Input::Pcm(pcm, if duplex {payload["vadSpeech"].as_bool()} else {None})).is_err() {
                    request_failure(session, "capture_overflow", Some("收音处理队列繁忙，请重新开始对话"));
                }
                return true;
            }
        }
        request_failure(session, "capture_frame_invalid", Some("设备音频帧编码无效，本轮对话已停止"));
        return true;
    }
    if topic == "audio/error" {
        let message = payload.get("error").or_else(|| payload.get("message")).and_then(Value::as_str).unwrap_or("device audio error");
        request_failure(session, "device_audio_error", Some(&format!("设备音频错误：{message}")));
        return true;
    }
    if topic == "audio/end" && capture_terminal {
        session.capture_ready.store(false, Ordering::SeqCst);
        request_failure(session, "device_capture_ended", Some("设备收音已结束，实时对话已停止；请重新进入"));
    }
    true
}

/// Called from the USB message dispatcher for `input/event`. Handles the `realtime_chat` action:
/// a live session stops; otherwise the UI is asked to start one for the current appearance.
fn realtime_entry_context(context: &str) -> bool {
    matches!(context, "main" | "app" | "components")
}

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
    if !realtime_entry_context(&context) {
        // Global realtime input works on all supported device pages. Unknown
        // contexts still fail closed, rather than dispatching an Agent action.
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
    request_failure(session, reason, None);
}

fn request_failure(session: &Session, reason: &str, error: Option<&str>) {
    // Publish the first cause atomically with cancellation. Late protocol/end
    // errors cannot rewrite it; cancellation never waits for more PCM/Ready.
    let Ok(mut status) = session.status.lock() else {
        session.cancelled.store(true, Ordering::SeqCst);
        session.stop_notify.notify_one();
        return;
    };
    if session.cancelled.swap(true, Ordering::SeqCst) { return; }
    status.reason = reason.to_owned();
    // Earlier recoverable-round warnings are not a new user-stop failure.
    status.error = error.unwrap_or_default().to_owned();
    drop(status);
    crate::smart_home::cancel_voice_session(&session.id);
    session.stop_notify.notify_one();
}

fn start_guard() -> &'static Mutex<()> {
    static START_GUARD: OnceLock<Mutex<()>> = OnceLock::new();
    START_GUARD.get_or_init(|| Mutex::new(()))
}

pub fn start(app: &AppHandle, mut input: RealtimeChatStartInput) -> Result<RealtimeChatStatus, String> {
    let _guard = start_guard().lock().map_err(|_| "实时对话启动锁异常")?;
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
    if usb_status.connection_id.parse::<u64>().ok().filter(|id| *id != 0).is_none()
        || usb_status.boot_id.len() != 16 {
        return Err("设备连接身份尚未确认，请等待 USB 重新连接后进入对话".into());
    }
    if active_session().is_some() { return Err("已有实时对话，请先结束后再开始".into()); }
    crate::component_notes::stop_recording_all();
    crate::component_audio::stop(None);
    // The native global setting is authoritative; ignore legacy per-pet gain
    // and stale frontend snapshots, including on hardware-button entry.
    input.voice.volume = voice_chat_settings::realtime_volume()?;

    let (tx, rx) = mpsc::channel(128);
    let session_id = format!("rtc-{}", uuid::Uuid::new_v4());
    let session = Arc::new(Session {
        id: session_id.clone(),
        board_device_id: board.clone(),
        transport: Some(CaptureTransport { usb:usb.clone(), connection:usb_status.connection_id.clone(), boot:usb_status.boot_id.clone() }),
        capture_stream: Mutex::new(None),
        audio_egress: Mutex::new(()),
        audio_requests: Arc::new(Mutex::new(Default::default())),
        cancelled: AtomicBool::new(false),
        duplex: AtomicBool::new(usb_status.capabilities["features"]["audioAecRecovery"] == true
            || ["audioAec","audioVad","audioFullDuplex"].iter().all(|key| usb_status.capabilities["features"][*key] == true)),
        capture_armed: AtomicBool::new(false),
        capture_ready: AtomicBool::new(false),
        stop_notify: tokio::sync::Notify::new(),
        input: tx,
        completed_playback: Mutex::new(String::new()),
        pending_persona: Mutex::new(None),
        playback_volume: AtomicU32::new(input.voice.volume().to_bits()),
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
        diagnostics::record(&session.id, "started", json!({"volume":session.playback_volume()}));
        tokio::select! {
            biased;
            _ = session.stop_notify.notified() => {
                let (reason, error) = session.status.lock().map(|s| (s.reason.clone(), s.error.clone())).unwrap_or_default();
                finish(&app_clone, &usb, &session, &reason, &error).await;
            }
            _ = async {
                while transport_current(&session) && !session.cancelled.load(Ordering::SeqCst) {
                    // Status snapshot only; no serial reader, probe or command.
                    tokio::time::sleep(Duration::from_millis(100)).await;
                }
            } => {
                request_failure(&session, "capture_connection_changed", Some("设备连接已变化，实时对话已停止，请重新进入"));
                finish(&app_clone, &usb, &session, "capture_connection_changed", "").await;
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
    session: Option<Arc<Session>>,
    previous_user: String,
    turn_started: Option<Instant>,
    awaiting_fresh_silence: bool,
    fresh_silence_run: usize,
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
        Self { noise: 200.0, processed_noise: 16.0, speech_run: 0, silence_run: 0, in_speech: false, preroll: Vec::new(), speech_started: None, soft_gap: 0, metrics: VadMetrics::default(), diagnostic_session: String::new(), reply_tail_until: None, session: None, previous_user: String::new(), turn_started: None, awaiting_fresh_silence:false, fresh_silence_run:0 }
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
        if self.awaiting_fresh_silence {
            // Lifecycle rearm only: use the existing voiced predicate and
            // existing silence duration, never a new acoustic threshold or
            // synthetic PCM. Continuous failed/provisional speech cannot
            // silently open another paid recognition request.
            self.fresh_silence_run = if voiced { 0 } else { self.fresh_silence_run.saturating_add(1) };
            if self.fresh_silence_run >= SPEECH_END_FRAMES { self.reset(); }
            return VadStep::Idle;
        }
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
        self.awaiting_fresh_silence = false;
        self.fresh_silence_run = 0;
    }

    fn await_new_utterance(&mut self) {
        self.reset();
        self.awaiting_fresh_silence = true;
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
    ack_requests:Arc<Mutex<audio_ack::Requests>>,
}
impl Player {
    fn send(&self, cmd: PlayerCmd) -> Result<(), ()> {
        {let mut requests=self.ack_requests.lock().map_err(|_|())?;
            match &cmd{PlayerCmd::Begin(id)=>requests.playback(Some(id)).map_err(|_|())?,
                PlayerCmd::Flush=>requests.playback(None).map_err(|_|())?,_=>{}}
        }
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
    let player = Player { tx, epoch: epoch.clone(), ack_requests:session.audio_requests.clone() };
    thread::spawn(move || {
        #[cfg(target_os = "macos")]
        let _audio_activity = {
            let activity = crate::realtime_audio_macos::AudioActivity::begin_on_player_thread();
            diagnostics::record(&session.id, "playback_scheduling", json!({
                "platform":"macos", "activity":"latency_critical", "qos":"user_initiated",
                "qosApplied":activity.qos_error == 0, "qosError":activity.qos_error
            }));
            activity
        };
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
            let (pcm, gain) = playback_pcm(&session, chunk);
            let result = send_session_board(
                usb, &session,
                "audio/play_chunk",
                &json!({ "sessionId": turn_id, "seq": *seq, "bytes": pcm.len(), "data": BASE64.encode(&pcm) }),
            );
            let elapsed_ms = write_started.elapsed().as_millis();
            // Metadata only: no PCM, transcript, key, port or personal path.
            if *seq < 3 || *seq % 10 == 0 || elapsed_ms >= 80 || result.is_err() {
                diagnostics::record(&session.id, "playback_tx", json!({"seq":*seq,
                    "bytes":chunk.len(),"elapsedMs":elapsed_ms,"ok":result.is_ok(),"volume":gain,
                    "errorKind":result.as_ref().err().map(|error|diagnostics::error_kind(error))}));
            }
            if let Err(error) = result {
                eprintln!("[realtime-chat] device audio transport write failed: {error}");
                request_failure(&session, "playback_write_failed", Some("设备音频传输失败，请检查 USB 连接后重试"));
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
                send_player_board(&usb, &session, "audio/play_flush", json!({"sessionId":turn_id}));
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
                    send_player_board(
                        &usb,
                        &session,
                        "audio/play_begin",
                        json!({ "sessionId": turn_id, "sampleRate": 16000, "channels": 1, "format": "pcm_s16le" }),
                    );
                }
                PlayerCmd::Subtitle(text) => {
                    if subtitles {
                        send_player_board(&usb, &session, "audio/play_subtitle", json!({"sessionId":turn_id,
                            "offsetBytes":sent_bytes + pending.len() as u64,"text":text}));
                    }
                }
                PlayerCmd::Pcm(bytes) => {
                    pending.extend_from_slice(&bytes);
                    while pending.len() >= PLAY_CHUNK_BYTES {
                        let chunk: Vec<u8> = pending.drain(..PLAY_CHUNK_BYTES).collect();
                        flush_chunk(&usb, &turn_id, &mut seq, &chunk, &mut begin_at, &mut sent_bytes, expected);
                        if session.cancelled.load(Ordering::SeqCst) || epoch.load(Ordering::SeqCst) != expected { pending.clear(); break; }
                    }
                }
                PlayerCmd::End(done) => {
                    let chunk = std::mem::take(&mut pending);
                    flush_chunk(&usb, &turn_id, &mut seq, &chunk, &mut begin_at, &mut sent_bytes, expected);
                    if session.cancelled.load(Ordering::SeqCst) || epoch.load(Ordering::SeqCst) != expected { continue; }
                    send_player_board(&usb, &session, "audio/play_end", json!({ "sessionId": turn_id, "bytes": sent_bytes }));
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
                                request_failure(&session, "playback_timeout", Some("设备播放完成确认超时，请检查 USB 连接"));
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
                    send_player_board(&usb, &session, "audio/play_flush", json!({ "sessionId": turn_id }));
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
    let reason: String;
    let mut history: Vec<ChatTurn> = Vec::new();
    let mut desktop_conversation = crate::desktop_agent::Conversation::default();
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
    vad.session = Some(session.clone());
    let mut pending_speech = None;
    let mut deferred_audio = VecDeque::new();
    diagnostics::record(&session.id, "audio_mode", json!({"state":if duplex {"full_duplex"} else {"half_duplex"}}));

    hud(&usb, &board, &session.id, "preparing", &name, "准备中");

    // Validate TTS with the actual greeting before enabling continuous microphone capture.
    let greeting = input.greeting.trim().to_string();
    if !greeting.is_empty() {
        let mut greeting_pcm = Vec::new();
        if let Err(error) = tts.synthesize(&greeting, |pcm| greeting_pcm.extend_from_slice(pcm)).await {
            finish(&app, &usb, &session, "tts", &error).await;
            return;
        }
        if duplex {
            if let Err(error) = begin_capture(&usb, &session, true).await {
                finish(&app, &usb, &session, "audio", &error).await; return;
            }
        }
        diagnostics::record(&session.id, "greeting_ready", json!({"bytes":greeting_pcm.len()}));
        publish(&app, &session, |s| { s.state = "speaking".into(); s.reply = greeting.clone(); });
        hud(&usb, &board, &session.id, "speaking", &name, "");
        let _ = player.send(PlayerCmd::Begin(format!("{}-greeting", session.id)));
        let _ = player.send(PlayerCmd::Subtitle(greeting.clone()));
        let _ = player.send(PlayerCmd::Pcm(greeting_pcm));
        match listen_with_verified_barge_in(async {wait_playback(&player).await; Ok(())}, &mut rx, &mut deferred_audio, &mut vad, duplex).await {
            Ok(frames) => pending_speech = frames,
            Err(error) => {finish(&app, &usb, &session, "audio", &error).await; return;}
        }
        if pending_speech.as_ref().is_some_and(|speech| !speech.reply_completed) { let _ = player.send(PlayerCmd::Flush); }
    }
    if !duplex || greeting.is_empty() {
        if let Err(error) = begin_capture(&usb, &session, duplex).await {
            finish(&app, &usb, &session, "audio", &error).await; return;
        }
    }
    if !duplex { drain_pending(&mut rx); }
    publish(&app, &session, |s| s.state = "listening".into());
    hud(&usb, &board, &session.id, "listening", &name, "");

    let mut recognizer: Option<StreamingSpeechRecognizer> = None;
    let mut capture_stats = AsrCaptureStats::default();
    // Each mailbox belongs to exactly one recognizer generation. Dropping a
    // failed round drops its mailbox; old worker callbacks never target a new one.
    let mut asr_rx: Option<(u64, mpsc::UnboundedReceiver<StreamingSpeechEvent>)> = None;
    let mut asr_generation = 0;
    let mut last_activity = Instant::now();
    let mut turns: u32 = 0;
    let mut latest_asr = String::new();
    let mut displayed_asr = String::new();
    let mut final_asr: Option<String> = None;
    let mut recognizer_terminal = false;
    let mut caption_sent_at = Instant::now();
    let mut desktop_progress = String::new();
    let mut asr_started_at = Instant::now();
    let mut asr_error: Option<(String, &'static str)> = None;
    let mut asr_timeouts: u32 = 0;

    'conversation: loop {
        if session.cancelled.load(Ordering::SeqCst) {
            reason = "cancelled".into();
            break;
        }
        if let Some((error, stage)) = asr_error.take() {
            diagnostics::record(&session.id, "asr_round_failed", json!({
                "generation":asr_generation,"stage":stage,"errorKind":asr_failure_cause(&error),
                "elapsedMs":asr_started_at.elapsed().as_millis(),"count":asr_timeouts
            }));
            if !recoverable_asr_error(&error) {
                finish(&app, &usb, &session, "asr", &error).await;
                return;
            }
            // Abandon this recognizer, including any late results. Do not replay
            // audio or send partial text to the model. USB capture/AEC stay live.
            recognizer.take(); asr_rx.take();
            reset_timed_out_asr(&mut vad, &mut latest_asr, &mut displayed_asr,
                &mut final_asr, &mut recognizer_terminal, &mut deferred_audio);
            // Discard already queued PCM as well as deferred frames. Recovery
            // only admits a fresh user utterance, never historical audio replay.
            drain_pending(&mut rx);
            asr_timeouts = asr_timeouts.saturating_add(1);
            last_activity = Instant::now();
            publish(&app, &session, |s| {
                s.state="listening".into(); s.transcript.clear();
                s.reason="asr_timeout".into();
                s.error="刚才语音识别中断，仍在聆听，请再说一次。".into();
            });
            user_caption(&usb, &board, &session.id, &name, "", false);
            hud(&usb, &board, &session.id, "listening", &name, "识别中断，请再说一次");
            diagnostics::record(&session.id, "asr_recovered", json!({"generation":asr_generation,"reason":asr_failure_cause(&error), "count":asr_timeouts}));
        }
        if let Some(speech) = pending_speech.take() {
            asr_started_at = Instant::now();
            latest_asr = speech.text; displayed_asr.clear();
            recognizer_terminal = speech.terminal;
            final_asr = speech.terminal.then(|| latest_asr.clone());
            last_activity = Instant::now();
            capture_stats = speech.stats;
            asr_generation = speech.recognizer.generation();
            recognizer = Some(speech.recognizer); asr_rx = Some((asr_generation, speech.events));
            publish(&app, &session, |s| {s.state="listening".into(); s.transcript = latest_asr.clone(); s.error.clear(); s.reason.clear();});
            user_caption(&usb,&board,&session.id,&name,&latest_asr,false);
            diagnostics::record(&session.id,"asr_started",json!({"turn":turns+1,"reason":"confirmed_barge_in"}));
        }
        match apply_pending_persona(&session, &mut input, &mut tts).await {
            Ok(true) => {
                name = input.display_name.clone();
                publish(&app, &session, |s| s.display_name = name.clone());
                hud(&usb, &board, &session.id, "listening", &name, &latest_asr);
            }
            Ok(false) => {}
            Err(error) => { finish(&app, &usb, &session, "config", &error).await; return; }
        }
        if recognizer.is_none() && !vad.in_speech && vad.speech_run==0 && pending_speech.is_none() && deferred_audio.is_empty() {
            if let Some(message)=crate::desktop_agent::take_notification(&session.id,&board) {
                diagnostics::record(&session.id,"desktop_receipt",json!({"stage":"playback_started", "chars":message.chars().count()}));
                desktop_conversation.push("desktop_receipt", &message);
                let notification_turn=format!("{}-desktop-{}",session.id,now_ms());
                let _=player.send(PlayerCmd::Begin(notification_turn));
                publish(&app,&session,|s|{s.state="speaking".into();s.reply=message.clone();});
                hud(&usb,&board,&session.id,"speaking",&name,"");
                let announcement=async {
                    speak_one_logged(&mut tts,&player,&message,&session.id).await?;
                    wait_playback(&player).await;Ok(())
                };
                match listen_with_verified_barge_in(announcement,&mut rx,&mut deferred_audio,&mut vad,duplex).await {
                    Ok(speech)=>{
                        diagnostics::record(&session.id,"desktop_receipt",json!({"stage":"playback_finished", "ok":speech.as_ref().is_none_or(|s|s.reply_completed), "reason":if speech.as_ref().is_some_and(|s|!s.reply_completed) {"interrupted"} else {"completed"}}));
                        if speech.as_ref().is_some_and(|s|!s.reply_completed) {let _=player.send(PlayerCmd::Flush);tts.interrupt();}
                        pending_speech=speech;
                    }
                    Err(error)=>{
                        if error.starts_with(ASR_USER_ACTION_PREFIX) { finish(&app,&usb,&session,"asr",&error).await; return; }
                        diagnostics::record(&session.id,"desktop_receipt",json!({"stage":"playback_finished", "ok":false, "errorKind":diagnostics::error_kind(&error)}));let _=player.send(PlayerCmd::Flush);publish(&app,&session,|s|s.error=error);
                    }
                }
                last_activity=Instant::now();desktop_progress.clear();
                publish(&app,&session,|s|s.state="listening".into());
                hud(&usb,&board,&session.id,"listening",&name,"");
                continue;
            }
            let progress=crate::desktop_agent::progress(&session.id,&board).unwrap_or_default();
            if progress!=desktop_progress {
                hud(&usb,&board,&session.id,"listening",&name,&progress);
                desktop_progress=progress;
            }
        }
        if last_activity.elapsed() >= IDLE_TIMEOUT {
            reason = "idle".into();
            break;
        }
        if let Some((generation, events)) = asr_rx.as_mut() {
            while !recognizer_terminal {
                let event = match events.try_recv() {
                    Ok(event) => event,
                    Err(mpsc::error::TryRecvError::Empty) => break,
                    Err(mpsc::error::TryRecvError::Disconnected) => {
                        asr_error = Some(("云端 ASR 连接已关闭，未收到最终结果".into(), "worker_ended"));
                        continue 'conversation;
                    }
                };
                if !asr_generation_matches(asr_generation, *generation) { continue; }
                match event {
                    StreamingSpeechEvent::Partial { text, .. } => { if !text.trim().is_empty() { latest_asr = text; } }
                    StreamingSpeechEvent::Final { text, .. } => {
                        if !text.trim().is_empty() { latest_asr = text; }
                        final_asr = Some(latest_asr.clone());
                        recognizer_terminal = true;
                        // First Final owns this mailbox. Never let a queued
                        // later Error/Partial replace it or reopen the stream.
                        break;
                    }
                    StreamingSpeechEvent::Error(error) => {
                        asr_error=Some((error,"stream"));
                        continue 'conversation;
                    }
                    StreamingSpeechEvent::Ready => {
                        diagnostics::record(&session.id,"asr_ready",json!({"turn":turns+1,"elapsedMs":asr_started_at.elapsed().as_millis()}));
                    }
                }
            }
            if latest_asr != displayed_asr && caption_sent_at.elapsed() >= Duration::from_millis(200) {
                user_caption(&usb, &board, &session.id, &name, &latest_asr, false);
                publish(&app, &session, |s| s.transcript = latest_asr.clone());
                displayed_asr = latest_asr.clone();
                caption_sent_at = Instant::now();
            }
        }
        // A terminal ASR packet closes its worker. Finalize immediately rather
        // than writing more PCM into a closed recognizer or waiting for USB input.
        // Terminal dispatch carries no PCM at all. The single empty control
        // step below is never pushed, counted as audio, padded or sent to ASR.
        let terminal_step = recognizer_terminal && recognizer.is_some();
        let next = if terminal_step {
            None
        } else if let Some(item) = deferred_audio.pop_front() { Some(item) } else { match timeout(Duration::from_millis(500), rx.recv()).await {
            Ok(Some(item)) => Some(item),
            Ok(None) => { reason = "closed".into(); break; }
            Err(_) => continue,
        }};
        let (pcm, speech) = match next { Some(Input::Pcm(pcm, speech)) => (pcm, speech), None => (Vec::new(), None) };
                for frame in pcm.chunks(FRAME_BYTES).chain(terminal_step.then_some(&[][..])) {
                    let step = if terminal_step { vad.reset(); VadStep::End }
                        else { vad.step_in_context(frame, speech, VadContext::Listening) };
                    match step {
                        VadStep::Idle => {}
                        VadStep::Start(preroll) => {
                            asr_started_at = Instant::now();
                            latest_asr.clear();
                            displayed_asr.clear();
                            final_asr = None;
                            recognizer_terminal = false;
                            user_caption(&usb, &board, &session.id, &name, "", false);
                            last_activity = Instant::now();
                            let (tx, rx_events) = mpsc::unbounded_channel();
                            match StreamingSpeechRecognizer::start(move |event| { let _ = tx.send(event); }) {
                                Ok(rec) => {
                                    asr_generation = rec.generation();
                                    capture_stats = AsrCaptureStats::default();
                                    for f in preroll {
                                        if let Err(error) = rec.push_pcm(&f) {
                                            asr_error=Some((error,"preroll")); continue 'conversation;
                                        }
                                        capture_stats.observe(&f);
                                    }
                                    // The trigger frame is already part of preroll.
                                    recognizer = Some(rec);
                                    asr_rx = Some((asr_generation, rx_events));
                                    publish(&app, &session, |s| { s.state = "listening".into(); s.transcript.clear(); s.error.clear(); s.reason.clear(); });
                                    diagnostics::record(&session.id, "asr_started", json!({"turn": turns + 1}));
                                }
                                Err(error) => {
                                    asr_error=Some((error,"start")); continue 'conversation;
                                }
                            }
                        }
                        VadStep::Speech => {
                            if let Some(rec) = recognizer.as_ref() {
                                if let Err(error) = rec.push_pcm(frame) {
                                    asr_error=Some((error,"audio_push")); continue 'conversation;
                                }
                                capture_stats.observe(frame);
                            }
                        }
                        VadStep::End => {
                            let Some(rec) = recognizer.take() else { continue };
                            let Some((generation, mut events)) = asr_rx.take() else { continue };
                            if !asr_generation_matches(asr_generation, generation) { continue; }
                            let asr_finish_at = Instant::now();
                            if !recognizer_terminal {
                                if let Err(error) = rec.push_pcm(frame).and_then(|_| rec.finish()) {
                                    asr_error=Some((error,"finish")); continue 'conversation;
                                }
                                capture_stats.observe(frame);
                            }
                            recognizer_terminal = false;
                            capture_stats.record(&session.id, turns+1);
                            publish(&app, &session, |s| s.state = "thinking".into());
                            hud(&usb, &board, &session.id, "thinking", &name, "正在处理");
                            let final_result = match final_asr.take() {
                                Some(text) => Ok(text),
                                None => await_with_capture(await_final_text(&mut events, latest_asr.clone()), &mut rx, &mut deferred_audio, duplex).await,
                            };
                            let text = match final_result {
                                Ok(text) => text,
                                Err(error) => {
                                    diagnostics::record(&session.id,"asr_final_wait_failed",json!({"turn":turns+1,"elapsedMs":asr_finish_at.elapsed().as_millis(),"errorKind":diagnostics::error_kind(&error),"ok":false}));
                                    asr_error=Some((error,"final_wait")); continue 'conversation;
                                }
                            };
                            drop(rec);
                            asr_timeouts = 0;
                            let text = text.trim().to_string();
                            if text.is_empty() {
                                diagnostics::record(&session.id, "asr_empty", json!({"turn":turns+1,"elapsedMs":asr_finish_at.elapsed().as_millis(),"reason":"no_transcript"}));
                                publish(&app, &session, |s| s.state = "listening".into());
                                // VAD can be triggered by environmental noise. No transcript is
                                // not proof the user spoke; keep listening without blaming them.
                                hud(&usb, &board, &session.id, "listening", &name, "");
                                continue;
                            }
                            if is_explicit_stop(&text) {
                                // Stop means stop speaking, not a new LLM request
                                // that starts another spoken acknowledgement.
                                diagnostics::record(&session.id,"voice_stop",json!({"turn":turns,"chars":1}));
                                vad.reset();
                                publish(&app, &session, |s| {s.state="listening".into(); s.transcript=text.clone();});
                                hud(&usb, &board, &session.id, "listening", &name, "");
                                continue;
                            }
                            turns += 1;
                            if let Err(error) = apply_pending_persona(&session, &mut input, &mut tts).await {
                                finish(&app, &usb, &session, "config", &error).await; return;
                            }
                            name = input.display_name.clone();
                            publish(&app, &session, |s| s.display_name = name.clone());
                            let user_text = text.clone();
                            user_caption(&usb, &board, &session.id, &name, &user_text, true);
                            hud(&usb, &board, &session.id, "thinking", &name, "");
                            publish(&app, &session, |s| { s.transcript = user_text.clone(); s.turns = turns; });
                            diagnostics::record(&session.id, "asr_final", json!({"turn":turns,"chars":user_text.chars().count(),"elapsedMs":asr_finish_at.elapsed().as_millis()}));
                            vad.reset();
                            vad.previous_user = user_text.clone();
                            vad.turn_started = Some(Instant::now());
                            match listen_with_verified_barge_in(answer_turn(&app, &usb, &session, &llm_cfg, &input.system_prompt, &mut history, &mut desktop_conversation, &user_text, &mut tts, &player, &name), &mut rx, &mut deferred_audio, &mut vad, duplex).await {
                                Ok(Some(speech)) => {
                                    if !speech.reply_completed {
                                        // The reply future is now dropped: aborts LLM and TTS callbacks.
                                        publish(&app, &session, clear_search);
                                        let _ = player.send(PlayerCmd::Flush);
                                        tts.interrupt();
                                        crate::smart_home::cancel_voice_session(&session.id);
                                        diagnostics::record(&session.id,"barge_in",json!({"turn":turns,"bytes":speech.stats.frames*FRAME_BYTES,"reason":"asr_confirmed"}));
                                        let partial=session.status.lock().map(|s|s.reply.clone()).unwrap_or_default();
                                        remember_interrupted_reply(&mut history,&mut desktop_conversation,&user_text,&partial);
                                    }
                                    pending_speech = Some(speech);
                                }
                                Ok(None) => {}
                                Err(error) => {
                                    if error.starts_with(ASR_USER_ACTION_PREFIX) {
                                        finish(&app, &usb, &session, "asr", &error).await; return;
                                    }
                                    diagnostics::record(&session.id, "turn_failed", json!({"errorKind":diagnostics::error_kind(&error)}));
                                    crate::smart_home::clear_voice_pending(&session.id).await;
                                    let _ = player.send(PlayerCmd::Flush);
                                    publish(&app, &session, |s| s.error = error.clone());
                                    hud(&usb, &board, &session.id, "listening", &name, "刚才没说成，请再说一遍");
                                }
                            }
                            if pending_speech.is_none() && crate::music_player::take_voice_handoff() {
                                reason = "music_playback".into();
                                break 'conversation;
                            }
                            if !duplex {
                                tokio::time::sleep(POST_SPEAK_GUARD).await;
                                drain_pending(&mut rx);
                                vad.reset();
                            }
                            last_activity = Instant::now();
                            publish(&app, &session, |s| { s.state = "listening".into(); if s.search_state=="searching" {clear_search(s);} });
                            hud(&usb, &board, &session.id, "listening", &name, "");
                        }
                    }
                }
    }
    if let Some(rec) = recognizer.take() { rec.cancel(); }
    let _ = player.send(PlayerCmd::Flush);
    finish(&app, &usb, &session, &reason, "").await;
}

/// Streaming ASR is speculative until it yields at least two meaningful characters
/// or the explicit single-character stop command “停”.
/// The same recognizer is then handed to the conversation loop, preserving the
/// first syllables without replaying PCM or opening a second cloud request.
struct VerifiedSpeech {
    recognizer: StreamingSpeechRecognizer,
    events: mpsc::UnboundedReceiver<StreamingSpeechEvent>,
    stats: AsrCaptureStats,
    text: String,
    reply_completed: bool,
    terminal: bool,
}

fn is_explicit_stop(text: &str) -> bool {
    let mut chars = text.chars().filter(|ch| ch.is_alphanumeric());
    chars.next() == Some('停') && chars.next().is_none()
}

fn has_barge_in_text(text: &str) -> bool {
    let mut chars = text.chars().filter(|ch| ch.is_alphanumeric());
    match (chars.next(), chars.next()) {
        (Some('停'), None) | (Some(_), Some(_)) => true,
        _ => false,
    }
}

fn start_barge_probe(frames: Vec<Vec<u8>>) -> Result<VerifiedSpeech, String> {
    let (tx, events) = mpsc::unbounded_channel();
    let recognizer = StreamingSpeechRecognizer::start(move |event| { let _ = tx.send(event); })?;
    let mut stats = AsrCaptureStats::default();
    for frame in frames {
        recognizer.push_pcm(&frame)?;
        stats.observe(&frame);
    }
    Ok(VerifiedSpeech { recognizer, events, stats, text: String::new(), reply_completed: false, terminal: false })
}

async fn listen_with_verified_barge_in<F>(reply: F, rx: &mut mpsc::Receiver<Input>, deferred: &mut VecDeque<Input>, vad: &mut Vad, duplex: bool)
    -> Result<Option<VerifiedSpeech>, String>
where F: std::future::Future<Output=Result<(), String>> {
    verify_barge_in_with(reply, rx, deferred, vad, duplex, start_barge_probe).await
}

async fn verify_barge_in_with<F, S>(reply: F, rx: &mut mpsc::Receiver<Input>, deferred: &mut VecDeque<Input>, vad: &mut Vad, duplex: bool, mut start: S)
    -> Result<Option<VerifiedSpeech>, String>
where F: std::future::Future<Output=Result<(), String>>,
      S: FnMut(Vec<Vec<u8>>) -> Result<VerifiedSpeech, String> {
    if !duplex { reply.await?; return Ok(None); }
    tokio::pin!(reply);
    loop {
        // Borrow, don't drop the reply future when VAD finds a candidate. Until
        // ASR confirms it, synthesis, queued PCM and the current sentence survive.
        let Some(frames) = listen_during_reply(reply.as_mut(), rx, deferred, vad, true).await? else { return Ok(None); };
        let candidate_phase=vad.session.as_ref().and_then(|s|s.status.lock().ok().map(|s|s.state.clone())).unwrap_or_else(||"thinking".into());
        // Capture provenance now: cloud ASR may return seconds later. Measuring
        // the tail window when the result arrives loses protection against the
        // just-finished utterance and can cancel its pending todo write.
        let previous_user = if barge_policy::protect_previous_tail(vad.turn_started.map(|t| t.elapsed())) {
            vad.previous_user.clone()
        } else { String::new() };
        diagnostics::record(&vad.diagnostic_session, "barge_candidate", json!({"frames":frames.len(),"bufferedMs":deferred.len()*20,"state":candidate_phase}));
        let mut probe = match start(frames) {
            Ok(probe) => probe,
            Err(error) => {
                if asr_failure_cause(&error) == "configuration" { return Err(asr_user_action_error(&error)); }
                diagnostics::record(&vad.diagnostic_session,"barge_rejected",json!({"reason":"asr_start_failed","errorKind":diagnostics::error_kind(&error)}));
                vad.await_new_utterance();
                continue;
            }
        };
        let started = Instant::now();
        let deadline = tokio::time::sleep(BARGE_CONFIRM_TIMEOUT);
        tokio::pin!(deadline);
        let mut added_frames = 0;
        let mut voiced_frames = vad.speech_run;
        let mut confirmation = barge_policy::Confirmation::default();
        let mut input_finished = false;
        let mut rejection = "no_transcript";
        loop {
            // Poll ASR and the reply even when Windows has delivered a burst of
            // retained frames. A backlog must not starve confirmation or playback.
            tokio::select! {
                biased;
                _ = &mut deadline => { rejection = "confirmation_timeout"; break; }
                event = probe.events.recv() => match event {
                    Some(event @ (StreamingSpeechEvent::Partial { .. } | StreamingSpeechEvent::Final { .. })) => {
                        let (text, terminal, confidence) = match event {
                            StreamingSpeechEvent::Partial {text,confidence,..} => (text,false,confidence),
                            StreamingSpeechEvent::Final {text,confidence,..} => (text,true,confidence),
                            _ => unreachable!(),
                        };
                        // Terminal empty/single-character candidates cannot improve.
                        // Reject now instead of leaving a completed stream polling.
                        if terminal && !has_barge_in_text(&text) { break; }
                        if !has_barge_in_text(&text) { continue; }
                        if input_finished && !terminal { continue; }
                        let (phase, reply_text) = vad.session.as_ref().and_then(|s| s.status.lock().ok().map(|s| (s.state.clone(),s.reply.clone())))
                            .unwrap_or_else(|| ("thinking".into(),String::new()));
                        if !confirmation.accept(&text, terminal, confidence, started.elapsed(), voiced_frames, &phase, &reply_text, &previous_user) {
                            if terminal { rejection="unverified_speech"; break; }
                            continue;
                        }
                        probe.text = text;
                        probe.terminal = terminal;
                        // The candidate may have ended while waiting for cloud ASR.
                        // Continue with the SAME stream; ordinary silence detection
                        // sends its final marker once, without losing later speech.
                        vad.in_speech = true;
                        vad.silence_run = 0;
                        vad.speech_started.get_or_insert_with(Instant::now);
                        diagnostics::record(&vad.diagnostic_session,"barge_confirmed",json!({"chars":probe.text.chars().filter(|ch|ch.is_alphanumeric()).count(),"elapsedMs":started.elapsed().as_millis(),"frames":probe.stats.frames,"state":phase,"reason":if terminal {"final_transcript"} else if is_explicit_stop(&probe.text) {"explicit_stop"} else {"stable_transcript"}}));
                        return Ok(Some(probe));
                    }
                    Some(StreamingSpeechEvent::Error(error)) => {
                        if asr_failure_cause(&error) == "configuration" { return Err(asr_user_action_error(&error)); }
                        diagnostics::record(&vad.diagnostic_session,"barge_probe_error",json!({"errorKind":diagnostics::error_kind(&error)}));
                        rejection = "asr_error"; break;
                    }
                    Some(StreamingSpeechEvent::Ready) => {},
                    None => break,
                },
                result = &mut reply, if !probe.reply_completed => {
                    result?;
                    probe.reply_completed = true;
                    vad.reply_tail_until = Some(Instant::now() + POST_SPEAK_GUARD);
                }
                incoming = async { if !input_finished && !deferred.is_empty() { deferred.pop_front() } else { rx.recv().await } } => {
                    let Some(Input::Pcm(pcm, speech)) = incoming else { return Err("设备收音通道关闭".into()); };
                    if speech.is_none() || pcm.len() != FRAME_BYTES { return Err("设备 AEC/VAD 音频帧无效".into()); }
                    if input_finished {
                        if deferred.len() >= FINAL_CAPTURE_LIMIT { return Err("收音缓冲已满".into()); }
                        deferred.push_back(Input::Pcm(pcm,speech));
                        continue;
                    }
                    if let Err(error) = probe.recognizer.push_pcm(&pcm) {
                        if asr_failure_cause(&error) == "configuration" { return Err(asr_user_action_error(&error)); }
                        diagnostics::record(&vad.diagnostic_session,"barge_probe_error",json!({"errorKind":diagnostics::error_kind(&error)}));
                        rejection = "asr_error"; break;
                    }
                    probe.stats.observe(&pcm);
                    added_frames += 1;
                    if speech == Some(true) && Vad::rms(&pcm) >= PROCESSED_VAD_ABS_MIN { voiced_frames += 1; }
                    // Keep feeding silence to streaming ASR; an empty VAD end is
                    // never itself permission to cancel the reply.
                    if matches!(vad.step_with_hint(&pcm, speech),VadStep::End) || added_frames >= BARGE_CONFIRM_MAX_FRAMES {
                        if let Err(error) = probe.recognizer.finish() {
                            if asr_failure_cause(&error) == "configuration" { return Err(asr_user_action_error(&error)); }
                            rejection="asr_error"; break;
                        }
                        input_finished=true;
                    }
                }
            }
        }
        diagnostics::record(&vad.diagnostic_session,"barge_rejected",json!({"reason":rejection,"elapsedMs":started.elapsed().as_millis(),"frames":probe.stats.frames}));
        probe.recognizer.cancel();
        vad.await_new_utterance();
        if probe.reply_completed { return Ok(None); }
    }
}

/// Capture remains polled while the reply is thinking, synthesizing or playing.
/// Only AEC/VAD-tagged device PCM enables this branch. The verification wrapper
/// borrows `reply` here, so finding a candidate does not cancel playback.
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
    let mut pending = session.pending_persona.lock().map_err(|_| "实时对话配置锁异常")?;
    *pending = Some(input);
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

#[tauri::command]
pub fn load_realtime_voice_volume() -> Result<f32, String> {
    voice_chat_settings::realtime_volume()
}

#[tauri::command]
pub fn save_realtime_voice_volume(volume: f32) -> Result<f32, String> {
    // Share the start barrier so a concurrent start cannot publish an old gain
    // after a successful save. Persist first; errors leave live audio unchanged.
    let _guard = start_guard().lock().map_err(|_| "实时对话启动锁异常")?;
    let saved = voice_chat_settings::save_realtime_volume(volume)?;
    if let Some(session) = active_session() {
        session.set_playback_volume(&VoiceSpec { volume:saved, ..Default::default() });
        diagnostics::record(&session.id, "voice_volume_updated", json!({"volume":saved}));
    }
    Ok(saved)
}

fn recoverable_asr_error(error: &str) -> bool {
    // Authentication/configuration errors still need user action; do not loop
    // blindly or retry paid requests. Recovery only permits a NEW utterance.
    matches!(asr_failure_cause(error), "timeout" | "transport" | "worker_ended")
}

const ASR_USER_ACTION_PREFIX: &str = "云端 ASR 需要用户处理：";

fn asr_user_action_error(error: &str) -> String {
    format!("{ASR_USER_ACTION_PREFIX}请检查语音识别配置、授权和额度后重新进入。{error}")
}

fn asr_generation_matches(current: u64, incoming: u64) -> bool {
    current != 0 && current == incoming
}

fn asr_failure_cause(error: &str) -> &'static str {
    let text = error.to_lowercase();
    let code = |raw: &str| -> Option<u32> {
        let end = raw.find(|c: char| !c.is_ascii_digit()).unwrap_or(raw.len());
        if end == 0 || end > 10 || raw[end..].chars().next()
            .is_some_and(|c| !c.is_whitespace() && !matches!(c, ',' | ')' | '）')) { return None; }
        raw[..end].parse().ok()
    };
    // Only the shared ASR formatter's explicit outer code is authoritative.
    // A log ID or provider detail containing "401", "auth" or "quota" is not
    // an authentication result. Keep the original error unchanged for the UI.
    for prefix in ["语音识别服务等待音频超时，本段已结束（provider=volcengine, code=",
        "语音识别服务返回错误（provider=volcengine, code="] {
        if let Some(raw) = text.strip_prefix(prefix) {
            return match code(raw) {
                Some(45_000_081) => "timeout",
                Some(401 | 403 | 429) => "configuration",
                Some(502 | 503 | 504) => "transport",
                _ => "service", // Unknown service codes stay fatal, never retry.
            };
        }
    }
    let without_trace = text.split(", logid=").next().unwrap_or(&text);
    for prefix in ["连接火山引擎 asr 失败: http error: ", "http error: ", "http status: "] {
        if let Some(raw) = without_trace.strip_prefix(prefix) {
            return match code(raw) {
                Some(401 | 403 | 429) => "configuration",
                Some(502 | 503 | 504) => "transport",
                _ => "service",
            };
        }
    }
    // Classify controlled local stages, not arbitrary words/numbers in nested
    // exception bodies, provider messages, URLs, filenames or trace metadata.
    let stage = without_trace.split([':', '：', '（', '(']).next().unwrap_or(without_trace).trim();
    if ["未配置", "请配置", "请填写", "macos asr 本地配置", "火山引擎 asr 安全配置",
        "火山引擎 asr 本地配置", "无法序列化火山引擎 asr 配置", "当前平台仅支持通过环境变量配置",
        "余额不足", "额度不足", "欠费", "限流"].iter().any(|prefix| stage.starts_with(prefix)) {
        return "configuration";
    }
    if stage.starts_with("云端 asr 工作任务在最终结果前结束") { return "worker_ended"; }
    if !["语音识别", "等待语音识别", "本地等待真实麦克风 pcm", "本地真实麦克风 pcm",
        "连接火山引擎 asr", "火山引擎 asr", "云端 asr", "asr connection",
        "cloud speech recognition input is closed"].iter().any(|prefix| stage.starts_with(prefix)) {
        return "service";
    }
    if stage.contains("timeout") || stage.contains("超时") { return "timeout"; }
    if ["connection", "连接", "接收失败", "发送失败", "input is closed", "broken pipe",
        "connection reset", "transport", "websocket"]
        .iter().any(|marker| stage.contains(marker)) { return "transport"; }
    "service"
}

fn reset_timed_out_asr(vad: &mut Vad, latest: &mut String, displayed: &mut String,
    final_text: &mut Option<String>, terminal: &mut bool, deferred: &mut VecDeque<Input>) {
    vad.await_new_utterance();
    latest.clear(); displayed.clear(); *final_text=None; *terminal=false;
    // Never re-feed stale audio into a new paid recognition attempt. Stop and
    // persona updates live outside this PCM queue and are left untouched.
    deferred.retain(|input| !matches!(input, Input::Pcm(..)));
}

async fn await_final_text(events: &mut mpsc::UnboundedReceiver<StreamingSpeechEvent>, latest: String) -> Result<String, String> {
    await_final_text_with_timeout(events, latest, ASR_FINAL_TIMEOUT).await
}

async fn await_final_text_with_timeout(events: &mut mpsc::UnboundedReceiver<StreamingSpeechEvent>, mut latest: String, budget: Duration) -> Result<String, String> {
    let deadline = Instant::now() + budget;
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err("语音识别结果超时".into());
        }
        match timeout(remaining, events.recv()).await {
            Ok(Some(StreamingSpeechEvent::Final { text, .. })) => return Ok(if text.trim().is_empty() { latest } else { text }),
            Ok(Some(StreamingSpeechEvent::Partial { text, .. })) => { if !text.trim().is_empty() { latest = text; } }
            Ok(Some(StreamingSpeechEvent::Error(error))) => {
                return Err(error);
            }
            Ok(Some(StreamingSpeechEvent::Ready)) => {}
            Err(_) => return Err("语音识别结果超时".into()),
            Ok(None) => return Err("云端 ASR 连接已关闭，未收到最终结果，请重新发声".into()),
        }
    }
}

fn remember_interrupted_reply(history:&mut Vec<ChatTurn>, conversation:&mut crate::desktop_agent::Conversation,
    user:&str, displayed:&str) {
    history.push(ChatTurn{role:"user".into(),content:user.into()});
    if !displayed.trim().is_empty() && !persona_llm::is_internal_reply(displayed) {
        let partial=format!("中断前显示的未完整播报内容（仅作上下文，不能据此推断执行结果）：{displayed}");
        history.push(ChatTurn{role:"assistant".into(),content:partial.clone()});
        conversation.push("pet_partial",&partial);
    }
    history.push(persona_llm::interruption_turn());
    conversation.push("interrupted","本轮回复被打断；已发出的操作不会撤回，执行结果须核对，不得自动重复。");
    persona_llm::trim_history(history);
}

#[allow(clippy::too_many_arguments)]
async fn answer_turn(
    app: &AppHandle,
    usb: &UsbSerialManager,
    session: &Arc<Session>,
    llm_cfg: &persona_llm::LlmConfig,
    system_prompt: &str,
    history: &mut Vec<ChatTurn>,
    desktop_conversation: &mut crate::desktop_agent::Conversation,
    user_text: &str,
    tts: &mut DoubaoTtsClient,
    player: &Player,
    name: &str,
) -> Result<(), String> {
    let board = session.board_device_id.clone();
    enum ReplyEvent { Sentence(String), Search(crate::web_search::Event) }
    // This receiver belongs only to this reply. Dropping it on a verified
    // interruption makes late events unreachable by subsequent turns.
    let (sentence_tx, mut sentence_rx) = mpsc::unbounded_channel::<ReplyEvent>();
    let search_tx=sentence_tx.clone();
    publish(app,session,|s| {clear_search(s); s.reply.clear(); s.error.clear();});
    let mut splitter = SentenceSplitter::default();
    let llm_cfg = llm_cfg.clone();
    let prompt = format!("{}{}", system_prompt, crate::shared_location::chat_context());
    let history_snapshot = history.clone();
    let user = user_text.to_string();
    let turn_started = Instant::now();
    let log_session = session.id.clone();
    let desktop_context=crate::desktop_agent::Context::new(&session.id,&board,
        session.status.lock().map(|s|s.turns).unwrap_or(0),user_text)
        .with_conversation(desktop_conversation).with_followed_agent();
    // Retain the user turn even if the reply future is interrupted or fails.
    desktop_conversation.push("user", user_text);
    diagnostics::record(&session.id, "llm_started", json!({}));
    let llm_task = tokio::spawn(async move {
        let mut first_delta = true;
        let mut first_sentence = true;
        let result = persona_llm::stream_chat_with_desktop(&llm_cfg, &prompt, &history_snapshot, &user, Some(&log_session), Some(desktop_context), |delta| {
            if first_delta {
                diagnostics::record(&log_session, "llm_first_delta", json!({"elapsedMs":turn_started.elapsed().as_millis()}));
                first_delta = false;
            }
            for sentence in splitter.push(delta) {
                if first_sentence {
                    diagnostics::record(&log_session, "llm_first_sentence", json!({"elapsedMs":turn_started.elapsed().as_millis()}));
                    first_sentence = false;
                }
                let _ = sentence_tx.send(ReplyEvent::Sentence(sentence));
            }
        }, |event| {let _ = search_tx.send(ReplyEvent::Search(event));})
        .await;
        if let Some(rest) = splitter.flush() {
            if first_sentence {
                diagnostics::record(&log_session, "llm_first_sentence", json!({"elapsedMs":turn_started.elapsed().as_millis()}));
            }
            let _ = sentence_tx.send(ReplyEvent::Sentence(rest));
        }
        result
    });
    struct AbortOnDrop(tokio::task::AbortHandle);
    impl Drop for AbortOnDrop { fn drop(&mut self) { self.0.abort(); } }
    let _abort = AbortOnDrop(llm_task.abort_handle());

    let turn_id = format!("{}-{}", session.id, now_ms());
    let mut spoken = false;
    let mut reply = String::new();
    let slow_hint = tokio::time::sleep(Duration::from_secs(3));
    tokio::pin!(slow_hint);
    let mut hinted = false;
    loop {
        let event = tokio::select! {
            event = sentence_rx.recv() => match event { Some(event) => event, None => break },
            _ = &mut slow_hint, if !spoken && !hinted => {
                hinted = true;
                hud(usb,&board,&session.id,"thinking",name,"已听清，正在处理");
                diagnostics::record(&session.id,"model_wait_hint",json!({"elapsedMs":turn_started.elapsed().as_millis()}));
                continue;
            }
        };
        if session.cancelled.load(Ordering::SeqCst) {
            break;
        }
        let sentence=match event {
            ReplyEvent::Sentence(sentence)=>sentence,
            ReplyEvent::Search(event)=>{
                if matches!(event,crate::web_search::Event::Started) {
                    hinted = true;
                    hud(usb,&board,&session.id,"thinking",name,"正在联网查询");
                }
                publish(app,session,|s|update_search(s,event));
                continue;
            },
        };
        let clean = persona_llm::clean_spoken_text(&sentence);
        if persona_llm::is_internal_reply(&sentence) {
            diagnostics::record(&session.id,"internal_reply_blocked",json!({"turn":session.status.lock().map(|s|s.turns).unwrap_or(0)}));
            return Err("本轮回答异常，已拦截内部状态文案；请重新提问".into());
        }
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
        speak_one_logged(tts, player, &clean, &session.id).await?;
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
        speak_one(tts, player, fallback).await?;
        wait_playback(player).await;
    }
    crate::smart_home::arm_voice_pending(&session.id).await;
    diagnostics::record(&session.id, "turn_completed", json!({"chars": full.chars().count(),"elapsedMs":turn_started.elapsed().as_millis()}));
    history.push(ChatTurn { role: "user".into(), content: user_text.to_string() });
    let final_reply = if full.trim().is_empty() { reply } else { full };
    desktop_conversation.push("pet", &final_reply);
    history.push(ChatTurn { role: "assistant".into(), content: final_reply });
    persona_llm::trim_history(history);
    Ok(())
}

async fn speak_one(tts: &mut DoubaoTtsClient, player: &Player, text: &str) -> Result<(), String> {
    speak_one_logged(tts, player, text, "").await
}

async fn speak_one_logged(tts: &mut DoubaoTtsClient, player: &Player, text: &str, session_id: &str) -> Result<(), String> {
    let sender = player.clone();
    let started = Instant::now();
    let mut first_pcm = true;
    tts.synthesize(text, |pcm| {
        if first_pcm && !pcm.is_empty() {
            let _ = sender.send(PlayerCmd::Subtitle(text.to_string()));
            diagnostics::record(session_id, "tts_first_pcm", json!({"elapsedMs":started.elapsed().as_millis()}));
            first_pcm = false;
        }
        let _ = sender.send(PlayerCmd::Pcm(pcm.to_vec()));
    })
    .await
    .map(|_| ())
    .map_err(|error| {
        diagnostics::record(session_id,"tts_turn_failed",json!({"errorKind":diagnostics::error_kind(&error)}));
        format!("语音合成失败：{error}")
    })
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

/// The closure is the same one production finish uses: two pre-reserved,
/// once-only physical attempts. A writer Ok is NOT a device ACK or idle proof.
fn attempt_terminal_controls(session:&Session,mut send:impl FnMut(audio_ack::Terminal,&str)->Result<(),String>)->Result<bool,String>{
    // The same gate spans reservation and BOTH physical writes. An ordinary
    // sender already in the gate finishes first; one waiting outside is denied
    // by the in-gate cancelled/closing check. No await in this critical section.
    let _egress=session.audio_egress.lock().map_err(|_|"实时音频终止发送顺序不可用")?;
    let batch=session.audio_requests.lock().map_err(|_|"音频终止状态不可用")?.begin_terminal(&session.id)?;
    let Some(batch)=batch else{return Ok(false);};
    for (kind,id) in batch{
        if send(kind,&id).is_err(){
            if let Ok(mut requests)=session.audio_requests.lock(){requests.terminal_write_unknown(kind);}
        }
    }
    Ok(true)
}

async fn finish(app:&AppHandle,usb:&UsbSerialManager,session:&Arc<Session>,reason:&str,error:&str){
    {let Ok(mut status)=session.status.lock() else{return;};
        if !session.cancelled.swap(true,Ordering::SeqCst){
            if status.reason=="asr_timeout"{status.error.clear();}
            status.reason=reason.into();status.error=error.into();
        }
    }
    session.capture_armed.store(false,Ordering::SeqCst);session.capture_ready.store(false,Ordering::SeqCst);
    let already_stopped=session.audio_requests.lock().map(|r|r.stopped()).unwrap_or(false);
    if !already_stopped{publish(app,session,|s|{s.active=true;s.state="stopping".into();s.device_stop_state="awaiting_ack".into();});}
    // No await between reserving the batch and BOTH physical attempts. A
    // cancelling select cannot abandon one command then let another caller retry.
    let _=attempt_terminal_controls(session,|kind,id|{
        let payload=match kind{
            audio_ack::Terminal::Flush=>json!({"sessionId":session.id,"requestId":id}),
            audio_ack::Terminal::Disable=>json!({"enabled":false,"sessionId":session.id,"requestId":id}),
        };
        send_bound_session_payload(usb,session,kind.topic(),&payload)
    });
    let deadline=Instant::now()+Duration::from_secs(3);
    loop{
        let (stopped,failed,probe)=match session.audio_requests.lock(){
            Ok(mut requests)=>(requests.stopped(),requests.terminal_failed(),requests.claim_probe()),
            Err(_)=>(false,true,false),
        };
        if stopped||failed{break;}
        if probe{
            // Separate, bounded, once-only read-only observation; no ordinary
            // ACK-window allocation, no flush/retry and no firmware gate change.
            let result=send_bound_session_payload(usb,session,"audio/query",
                &json!({"requestId":format!("{}-stop-observe",session.id)}));
            if let Ok(mut requests)=session.audio_requests.lock(){requests.probe_written(result.is_ok());}
        }
        if Instant::now()>=deadline{break;}
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    crate::smart_home::clear_voice_pending(&session.id).await;
    let (stopped,stop_state)=session.audio_requests.lock().map(|r|(r.stopped(),r.stop_state())).unwrap_or((false,"state_unavailable"));
    if !stopped{
        if let Ok(mut status)=session.status.lock(){if status.device_stop_state!="acknowledged_idle_observed"&&status.error.is_empty(){status.error="设备停止尚未确认，请只读检查设备；未自动重发终止指令".into();}}
        publish(app,session,|s|{s.active=true;s.state="stop_unconfirmed".into();s.device_stop_state=stop_state.into();});
        diagnostics::record(&session.id,"stop_unconfirmed",json!({"stage":stop_state,"ok":false,"terminalSlots":2}));
        return; // Keep owner admission closed; never label an unknown device idle.
    }
    let (reason,error,name)=session.status.lock().map(|s|(s.reason.clone(),s.error.clone(),s.display_name.clone())).unwrap_or_default();
    let hint=match reason.as_str(){"tts"=>"语音合成失败，请查看 PC 实时对话提示","asr"=>"语音识别失败，请查看 PC 实时对话提示",_=>"实时对话失败，请查看 PC 提示"};
    let _=send_bound_session_payload(usb,session,"ui/conversation",&json!({"sessionId":session.id,
        "state":if error.is_empty(){"ended"}else{"error"},"name":name,"text":if error.is_empty(){""}else{hint}}));
    diagnostics::record(&session.id,"ended",json!({"reason":reason,"ok":error.is_empty()&&reason!="aec_unavailable","errorKind":diagnostics::termination_error_kind(&reason,&error),"deviceStopState":stop_state}));
    publish(app,session,|s|{s.active=false;if s.search_state=="searching"{clear_search(s);}s.state="ended".into();s.device_stop_state=stop_state.into();});
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
pub async fn list_voice_chat_models(input: voice_chat_settings::VoiceChatSettingsInput) -> Result<Vec<String>, String> {
    voice_chat_settings::list_models(input).await
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
    include!("realtime_aec_ingress_tests.rs");
    #[test]
    fn interruption_keeps_displayed_question_without_claiming_it_was_completed() {
        let mut history=Vec::new();
        let mut conversation=crate::desktop_agent::Conversation::default();
        super::remember_interrupted_reply(&mut history,&mut conversation,"安装Chrome","要交给桌面助手安装吗？");
        assert_eq!(history[0].content,"安装Chrome");
        assert!(history[1].content.contains("要交给桌面助手安装吗？"));
        assert!(history[1].content.contains("不能据此推断执行结果"));
        assert_eq!(history[2].role,crate::persona_llm::interruption_turn().role);
        let mut empty=Vec::new();
        super::remember_interrupted_reply(&mut empty,&mut conversation,"安装Chrome","");
        assert_eq!(empty.len(),2);
    }
    #[test]
    fn output_gain_is_global_and_applies_once_even_to_already_queued_pcm() {
        use super::*;
        let (session, _) = routing_session();
        let raw: Vec<u8> = [10000i16, -10000, 0].iter().flat_map(|s| s.to_le_bytes()).collect();
        session.set_playback_volume(&VoiceSpec { volume:0.3, ..Default::default() });
        let (quiet, gain) = playback_pcm(&session, &raw);
        assert_eq!(gain,0.3);
        assert_eq!(quiet, [3000i16,-3000,0].iter().flat_map(|s| s.to_le_bytes()).collect::<Vec<_>>());
        session.set_playback_volume(&VoiceSpec { volume:1.0, ..Default::default() });
        assert_eq!(playback_pcm(&session,&raw).0,raw);
        // An unrelated persona save must not restore its legacy per-pet gain.
        let update = RealtimeChatStartInput { board_device_id:"board".into(),appearance_id:"".into(),
            display_name:"pet".into(),system_prompt:"prompt".into(),greeting:"".into(),
            voice:VoiceSpec {volume:1.5,..Default::default()} };
        queue_persona_update(&session,update).unwrap();
        assert_eq!(session.playback_volume(),1.0);
    }
    fn routing_session() -> (super::Session, tokio::sync::mpsc::Receiver<super::Input>) {
        use super::*;
        let (input, rx) = mpsc::channel(8);
        (Session { id:"rtc-new".into(), board_device_id:"board".into(), transport:None, capture_stream:Mutex::new(None), audio_egress:Mutex::new(()), audio_requests:Arc::new(Mutex::new({let mut r=audio_ack::Requests::default();r.playback(Some("rtc-new-turn")).unwrap();r})),
            cancelled:AtomicBool::new(false), duplex:AtomicBool::new(true),
            capture_armed:AtomicBool::new(false), capture_ready:AtomicBool::new(false),
            stop_notify:tokio::sync::Notify::new(), input,
            status:Mutex::new(RealtimeChatStatus {active:true, ..Default::default()}),
            completed_playback:Mutex::new(String::new()), pending_persona:Mutex::new(None), playback_volume:AtomicU32::new(1.0f32.to_bits()) }, rx)
    }

    #[test]
    fn audio_egress_serializes_admitted_write_before_once_terminal_pair(){
        use super::*;
        let (session,_rx)=routing_session();let session=Arc::new(session);
        let writes=Arc::new(Mutex::new(Vec::<String>::new()));
        let (entered_tx,entered_rx)=std_mpsc::channel();let (resume_tx,resume_rx)=std_mpsc::channel();
        let old=session.clone();let old_writes=writes.clone();
        let ordinary=std::thread::spawn(move||with_ordinary_audio_egress(&old,||{
            assert!(old.audio_requests.try_lock().is_ok());
            entered_tx.send(()).unwrap();resume_rx.recv_timeout(Duration::from_secs(2)).unwrap();
            old_writes.lock().unwrap().push("ordinary".into());Ok(())
        }));
        entered_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        assert!(session.audio_egress.try_lock().is_err());session.cancelled.store(true,Ordering::SeqCst);
        let terminal=session.clone();let terminal_writes=writes.clone();let (starting_tx,starting_rx)=std_mpsc::channel();
        let finisher=std::thread::spawn(move||{
            starting_tx.send(()).unwrap();
            attempt_terminal_controls(&terminal,|kind,id|{
                assert!(terminal.audio_requests.try_lock().is_ok()); // RX may acquire requests during TX.
                terminal_writes.lock().unwrap().push(kind.topic().into());
                route_session_audio(&terminal,"protocol/ack",&json!({"requestTopic":kind.topic(),"requestId":id,"ok":true}));
                Ok(())
            })
        });
        starting_rx.recv_timeout(Duration::from_secs(2)).unwrap();resume_tx.send(()).unwrap();
        ordinary.join().unwrap().unwrap();assert!(finisher.join().unwrap().unwrap());
        assert_eq!(*writes.lock().unwrap(),vec!["ordinary","audio/play_flush","audio/conversation"]);
        assert!(session.audio_requests.lock().unwrap().terminal_acks());
        assert!(!attempt_terminal_controls(&session,|_,_|panic!("terminal pair must stay once")).unwrap());
    }
    #[test]
    fn audio_egress_rejects_pre_gate_old_sender_after_terminal_even_without_cancel_flag(){
        use super::*;
        for cancel in [false,true]{
            let (session,_rx)=routing_session();let session=Arc::new(session);
            let writes=Arc::new(Mutex::new(Vec::<String>::new()));
            let (ready_tx,ready_rx)=std_mpsc::channel();let (resume_tx,resume_rx)=std_mpsc::channel();
            let old=session.clone();let old_writes=writes.clone();
            let ordinary=std::thread::spawn(move||{
                ready_tx.send(()).unwrap();resume_rx.recv_timeout(Duration::from_secs(2)).unwrap();
                with_ordinary_audio_egress(&old,||{old_writes.lock().unwrap().push("stale".into());Ok(())})
            });
            ready_rx.recv_timeout(Duration::from_secs(2)).unwrap();session.cancelled.store(cancel,Ordering::SeqCst);
            assert!(attempt_terminal_controls(&session,|kind,_|{writes.lock().unwrap().push(kind.topic().into());Ok(())}).unwrap());
            resume_tx.send(()).unwrap();assert!(ordinary.join().unwrap().is_err());
            assert_eq!(*writes.lock().unwrap(),vec!["audio/play_flush","audio/conversation"]);
        }
    }
    #[test]
    fn terminal_writes_are_once_and_independent_of_full_normal_window_and_cancelled_ack_is_consumed(){
        use super::*;
        let (session,_rx)=routing_session();
        for _ in 0..audio_ack::MAX_PENDING{session.audio_requests.lock().unwrap().issue(&session.id,"audio/play_chunk","rtc-new-turn").unwrap();}
        session.cancelled.store(true,Ordering::SeqCst);session.status.lock().unwrap().error="first-cause".into();
        let mut writes=Vec::new();
        assert!(attempt_terminal_controls(&session,|kind,id|{
            writes.push(kind);
            // Real route ACK delivery interleaves before writer return, with
            // cancellation already set and the ordinary table completely full.
            route_session_audio(&session,"protocol/ack",&json!({"requestTopic":kind.topic(),"requestId":id,"ok":true}));Ok(())
        }).unwrap());
        assert_eq!(writes,vec![audio_ack::Terminal::Flush,audio_ack::Terminal::Disable]);
        assert!(!attempt_terminal_controls(&session,|_,_|panic!("no second terminal TX")).unwrap());
        assert!(session.audio_requests.lock().unwrap().terminal_acks());assert!(!session.audio_requests.lock().unwrap().stopped());
        assert_eq!(session.status.lock().unwrap().error,"first-cause");
        assert!(session.audio_requests.lock().unwrap().claim_probe());session.audio_requests.lock().unwrap().probe_written(true);
        route_session_audio(&session,"audio/status",&json!({"active":false,"conversation":false,"playbackActive":false,"streamActive":false,"conversationSessionId":""}));
        assert!(!session.audio_requests.lock().unwrap().stopped());
        route_session_audio(&session,"protocol/ack",&json!({"requestTopic":"audio/query","requestId":format!("{}-stop-observe",session.id),"ok":true}));
        assert!(session.audio_requests.lock().unwrap().stopped());
    }
    #[test]
    fn player_begin_and_flush_retire_old_epoch_ack_before_replacement_utterance(){
        use super::*;
        let (session,_rx)=routing_session();session.capture_armed.store(true,Ordering::SeqCst);
        let (tx,_commands)=std_mpsc::channel();let player=Player{tx,epoch:Arc::new(AtomicU64::new(0)),ack_requests:session.audio_requests.clone()};
        player.send(PlayerCmd::Begin("rtc-new-a".into())).unwrap();
        let old=session.audio_requests.lock().unwrap().issue(&session.id,"audio/play_chunk","rtc-new-a").unwrap();
        player.send(PlayerCmd::Flush).unwrap();player.send(PlayerCmd::Begin("rtc-new-b".into())).unwrap();
        let current=session.audio_requests.lock().unwrap().issue(&session.id,"audio/play_chunk","rtc-new-b").unwrap();
        route_session_audio(&session,"protocol/ack",&json!({"requestTopic":"audio/play_chunk","requestId":old,"ok":false,"code":"audio_chunk_rejected"}));
        assert!(!session.cancelled.load(Ordering::SeqCst));
        route_session_audio(&session,"protocol/ack",&json!({"requestTopic":"audio/play_chunk","requestId":current,"ok":false,"code":"audio_chunk_rejected"}));
        assert!(session.cancelled.load(Ordering::SeqCst));assert_eq!(session.status.lock().unwrap().reason,"device_protocol_error");
    }
    #[test]
    fn foreign_notes_stop_ack_cannot_cancel_new_realtime_session() {
        use super::*;
        let (session,_rx)=routing_session();session.capture_armed.store(true,Ordering::SeqCst);
        let payload=json!({"requestTopic":"audio/control","requestId":"notes-old-stop","ok":false,"code":"audio_control_failed"});
        route_session_audio(&session,"protocol/ack",&payload);
        assert!(!session.cancelled.load(Ordering::SeqCst));
        assert!(session.status.lock().unwrap().reason.is_empty());
    }

    #[test]
    fn issued_malformed_optional_audio_ack_fails_closed_but_foreign_receipts_do_not_consume() {
        use super::*;
        for field in ["sessionId","conversationSessionId"] {
            for invalid in [Value::Null,json!(true),json!(7),json!([]),json!({})] {
                // Exact actual current-round request, not inferred from topic
                // or prefix. Current ACKs may omit the fields; present bad
                // types are not absence, for both success and failure ACKs.
                for ok in [true,false] {
                    let (session,_rx)=routing_session();session.capture_armed.store(true,Ordering::SeqCst);
                    let id=session.audio_requests.lock().unwrap().issue(&session.id,"audio/play_chunk","rtc-new-turn").unwrap();
                    let mut receipt=json!({"requestTopic":"audio/play_chunk","requestId":id,"ok":ok});
                    receipt[field]=invalid.clone();
                    assert!(route_session_audio(&session,"protocol/ack",&receipt));
                    assert!(session.cancelled.load(Ordering::SeqCst),"current malformed {field} ok={ok}");
                    assert_eq!(session.status.lock().unwrap().reason,"device_protocol_ack_invalid");
                    assert_eq!(session.audio_requests.lock().unwrap().accept(&session.id,"audio/play_chunk",&id,None,None),audio_ack::Receipt::Unmatched);
                    let saved=session.status.lock().unwrap().error.clone();
                    route_session_audio(&session,"protocol/ack",&json!({"requestTopic":"audio/play_chunk","requestId":id,"ok":false,"code":"audio_chunk_rejected"}));
                    assert_eq!(session.status.lock().unwrap().reason,"device_protocol_ack_invalid");
                    assert_eq!(session.status.lock().unwrap().error,saved);
                }
                // A foreign ID and a mismatched issued topic with the same bad
                // field cannot consume/kill the current round. A subsequent
                // genuine current rejection must still take the fail-closed path.
                let (session,_rx)=routing_session();session.capture_armed.store(true,Ordering::SeqCst);
                let id=session.audio_requests.lock().unwrap().issue(&session.id,"audio/play_chunk","rtc-new-turn").unwrap();
                for (topic,key) in [("audio/play_chunk","rtc-old-a1"),("audio/play_begin",id.as_str()),("audio/control",id.as_str())] {
                    let mut receipt=json!({"requestTopic":topic,"requestId":key,"ok":false});receipt[field]=invalid.clone();
                    route_session_audio(&session,"protocol/ack",&receipt);
                    assert!(!session.cancelled.load(Ordering::SeqCst),"foreign malformed {field} topic={topic}");
                }
                route_session_audio(&session,"protocol/ack",&json!({"requestTopic":"audio/play_chunk","requestId":id,"ok":false,"code":"audio_chunk_rejected"}));
                assert!(session.cancelled.load(Ordering::SeqCst));
                assert_eq!(session.status.lock().unwrap().reason,"device_protocol_error");
            }
        }
    }

    #[test]
    fn only_issued_current_audio_ack_fails_closed_and_success_cannot_be_replayed() {
        use super::*;
        let (session,_rx)=routing_session();session.capture_armed.store(true,Ordering::SeqCst);
        let id=session.audio_requests.lock().unwrap().issue(&session.id,"audio/play_chunk","rtc-new-turn").unwrap();
        for payload in [
            json!({"requestTopic":"audio/play_chunk","requestId":"rtc-old-a1","ok":false}),
            json!({"requestTopic":"audio/play_begin","requestId":id,"ok":false}),
            json!({"requestTopic":"audio/play_chunk","requestId":id,"sessionId":"rtc-old-turn","ok":false}),
        ] {route_session_audio(&session,"protocol/ack",&payload);assert!(!session.cancelled.load(Ordering::SeqCst));}
        route_session_audio(&session,"protocol/ack",&json!({"requestTopic":"audio/play_chunk","requestId":id,"ok":true}));
        route_session_audio(&session,"protocol/ack",&json!({"requestTopic":"audio/play_chunk","requestId":id,"ok":false}));
        assert!(!session.cancelled.load(Ordering::SeqCst));
        let current=session.audio_requests.lock().unwrap().issue(&session.id,"audio/play_chunk","rtc-new-turn").unwrap();
        route_session_audio(&session,"protocol/ack",&json!({"requestTopic":"audio/play_chunk","requestId":current,"ok":false,"code":"audio_chunk_rejected"}));
        assert!(session.cancelled.load(Ordering::SeqCst));
        assert_eq!(session.status.lock().unwrap().reason,"device_protocol_error");
        assert!(!session.status.lock().unwrap().error.is_empty());
    }

    #[test]
    fn capture_routing_ignores_preentry_ptt_other_boards_and_old_timelines() {
        use super::*;
        let (session, mut rx) = routing_session();
        assert!(route_session_audio(&session, "audio/chunk", &json!({"aec":false})));
        assert!(!session.cancelled.load(Ordering::SeqCst));
        session.capture_armed.store(true, Ordering::SeqCst);
        assert!(!route_session_audio(&session, "audio/status", &json!({
            "boardDeviceId":"other", "conversation":true, "audioAec":false})));
        for payload in [json!({"conversationSessionId":"", "aec":false}),
            json!({"conversationSessionId":"rtc-old", "aec":false}),
            json!({"conversationSessionId":"rtc-old", "aec":true, "vadSpeech":true, "data":BASE64.encode(vec![0; FRAME_BYTES])})] {
            assert!(route_session_audio(&session, "audio/chunk", &payload));
        }
        assert!(route_session_audio(&session, "audio/status", &json!({
            "conversationSessionId":"rtc-old", "conversation":true, "audioAec":false})));
        assert!(route_session_audio(&session, "audio/status", &json!({"conversation":false,"audioAec":false})));
        assert!(!session.cancelled.load(Ordering::SeqCst));
        assert!(!session.capture_ready.load(Ordering::SeqCst));
        assert!(rx.try_recv().is_err());
    }

    #[test]
    fn aec_preparation_waits_then_routes_only_processed_current_audio() {
        use super::*;
        let (session, mut rx) = routing_session();
        session.capture_armed.store(true, Ordering::SeqCst);
        assert!(route_session_audio(&session, "audio/status", &json!({
            "conversationSessionId":"rtc-new", "conversation":true,
            "audioAec":false, "audioAecPreparing":true})));
        assert!(!session.cancelled.load(Ordering::SeqCst));
        assert!(!session.capture_ready.load(Ordering::SeqCst));
        assert!(route_session_audio(&session, "audio/status", &json!({
            "conversationSessionId":"rtc-new", "conversation":true,
            "audioAec":true, "audioAecPreparing":false})));
        assert!(session.capture_ready.load(Ordering::SeqCst));
        let pcm=vec![0; FRAME_BYTES];
        assert!(route_session_audio(&session, "audio/chunk", &json!({
            "conversationSessionId":"rtc-new", "aec":true, "vadSpeech":false,"data":BASE64.encode(&pcm)})));
        assert!(matches!(rx.try_recv(),Ok(Input::Pcm(data,Some(false))) if data==pcm));
        assert!(route_session_audio(&session, "audio/chunk", &json!({
            "conversationSessionId":"rtc-new", "aec":false,"data":BASE64.encode(&pcm)})));
        assert!(session.cancelled.load(Ordering::SeqCst));
        assert!(rx.try_recv().is_err());
    }

    #[test]
    fn aec_failures_are_explained_and_legacy_firmware_stays_fail_closed() {
        use super::*;
        for (reason, expected) in [("init_failed","初始化失败"),("stream_reset_failed","初始化失败"),
            ("reference_overflow","参考缓冲溢出"),("processed_queue_overflow","处理缓冲溢出"),("dsp_too_slow","持续超时")] {
            let (session, _) = routing_session();
            session.capture_armed.store(true, Ordering::SeqCst);
            route_session_audio(&session,"audio/status",&json!({"conversationSessionId":"rtc-new",
                "conversation":true,"audioAec":false,"aecReason":reason}));
            assert!(session.cancelled.load(Ordering::SeqCst));
            assert!(session.status.lock().unwrap().error.contains(expected));
        }
        let (session, mut rx) = routing_session();
        session.capture_armed.store(true, Ordering::SeqCst);
        route_session_audio(&session,"audio/chunk",&json!({"aec":false}));
        assert!(!session.cancelled.load(Ordering::SeqCst));
        route_session_audio(&session,"audio/status",&json!({"conversation":true,"audioAec":true}));
        assert!(session.capture_ready.load(Ordering::SeqCst));
        route_session_audio(&session,"audio/chunk",&json!({"aec":true})); // Missing boolean VAD is invalid too.
        assert!(session.cancelled.load(Ordering::SeqCst));
        assert!(rx.try_recv().is_err());
    }

    #[test]
    fn search_status_resets_between_turns_and_on_failure() {
        use super::*;
        let mut status=RealtimeChatStatus::default();
        update_search(&mut status,crate::web_search::Event::Started);
        assert_eq!(status.search_state,"searching");
        update_search(&mut status,crate::web_search::Event::Finished(crate::web_search::SearchResult{
            summary:"not sent to UI".into(), sources:vec![crate::web_search::Source{url:"https://example.com".into(),..Default::default()}],retrieved_at:"2026-09-22".into()
        }));
        assert_eq!(status.search_state,"completed");
        let snapshot=serde_json::to_string(&status).unwrap();
        assert!(!snapshot.contains("not sent to UI") && !snapshot.contains("https://example.com"));
        assert!(!snapshot.contains("searchSources") && !snapshot.contains("searchRetrievedAt"));
        clear_search(&mut status); assert!(status.search_state.is_empty());
        update_search(&mut status,crate::web_search::Event::Failed("未授权".into()));
        assert_eq!(status.search_state,"failed");
        update_search(&mut status,crate::web_search::Event::Started); assert!(status.search_error.is_empty());
    }
    use super::*;

    fn fake_barge_probe(frames: Vec<Vec<u8>>, texts: &[&str], keep_events_open: bool) -> (VerifiedSpeech, impl Sized) {
        let (recognizer, sink) = StreamingSpeechRecognizer::test_sink();
        let (tx, events) = mpsc::unbounded_channel();
        for (revision, text) in texts.iter().enumerate() {
            tx.send(StreamingSpeechEvent::Partial { revision: revision as u64, text: (*text).into(), confidence: None }).unwrap();
        }
        let mut stats = AsrCaptureStats::default();
        for frame in frames { recognizer.push_pcm(&frame).unwrap(); stats.observe(&frame); }
        (VerifiedSpeech { recognizer, events, stats, text: String::new(), reply_completed: false, terminal: false },
            (sink, if keep_events_open { Some(tx) } else { None }))
    }

    #[test]
    fn barge_in_needs_two_real_characters_or_explicit_stop() {
        for text in ["", "  ", "……！", "嗯", "啊，！", "a", "🙂🙂"] { assert!(!has_barge_in_text(text), "{text}"); }
        for text in ["停", " 停！ ", "停下", "等 一 下！", "你好", "OK", "换歌"] { assert!(has_barge_in_text(text), "{text}"); }
        for text in ["停", " 停！ "] { assert!(is_explicit_stop(text)); }
        for text in ["", "停下", "停止音乐", "等一下", "不停", "嗯"] { assert!(!is_explicit_stop(text)); }
    }

    #[test]
    fn delayed_tail_asr_does_not_abort_the_pending_task() {
        tauri::async_runtime::block_on(async {
            let (_tx, mut rx) = mpsc::channel(1);
            let mut deferred = (0..PROCESSED_SPEECH_START_FRAMES).map(|_| Input::Pcm(frame(180),Some(true))).collect();
            let mut vad = Vad::new();
            vad.previous_user = "帮我添加买火车票的待办".into();
            vad.turn_started = Some(Instant::now());
            let mut guards = Vec::new();
            let completed = Arc::new(AtomicBool::new(false)); let flag = completed.clone();
            let reply = async move {
                tokio::time::sleep(Duration::from_millis(1200)).await;
                flag.store(true, Ordering::SeqCst); Ok(())
            };
            let result = verify_barge_in_with(reply,&mut rx,&mut deferred,&mut vad,true,|frames| {
                let (mut probe, guard) = fake_barge_probe(frames,&[],true); guards.push(guard);
                let (tx, events) = mpsc::unbounded_channel(); probe.events=events;
                tokio::spawn(async move {
                    tokio::time::sleep(Duration::from_millis(950)).await;
                    let _ = tx.send(StreamingSpeechEvent::Final {revision:1,text:"的待办".into(),confidence:None});
                });
                Ok(probe)
            }).await.unwrap();
            assert!(result.is_none());
            assert!(completed.load(Ordering::SeqCst));
        });
    }

    #[test]
    fn terminal_confirmation_is_handed_off_without_writing_a_closed_asr_stream() {
        tauri::async_runtime::block_on(async {
            for text in ["停", "换歌", "嗯", ""] {
                let (_tx, mut rx) = mpsc::channel(1);
                let mut deferred = (0..PROCESSED_SPEECH_START_FRAMES).map(|_| Input::Pcm(frame(180),Some(true))).collect();
                let mut vad = Vad::new(); let mut guards = Vec::new();
                let result = verify_barge_in_with(async { Ok(()) },&mut rx,&mut deferred,&mut vad,true,|frames| {
                    let (mut probe, guard) = fake_barge_probe(frames,&[],false); guards.push(guard);
                    let (tx, events) = mpsc::unbounded_channel(); probe.events=events;
                    tx.send(StreamingSpeechEvent::Final {revision:1,text:text.into(),confidence:None}).unwrap();
                    Ok(probe)
                }).await.unwrap();
                if has_barge_in_text(text) {
                    let speech = result.unwrap(); assert!(speech.terminal); assert_eq!(speech.text,text);
                } else { assert!(result.is_none()); }
            }
        });
    }

    #[test]
    fn failed_or_timed_out_probe_keeps_the_original_reply_running() {
        tauri::async_runtime::block_on(async {
            for failure in ["start", "stream", "timeout"] {
                let (_tx, mut rx) = mpsc::channel(1);
                let mut deferred = (0..PROCESSED_SPEECH_START_FRAMES).map(|_| Input::Pcm(frame(180),Some(true))).collect();
                let mut vad = Vad::new(); let mut guards = Vec::new();
                let completed = Arc::new(AtomicBool::new(false)); let flag=completed.clone();
                let reply = async move {flag.store(true,Ordering::SeqCst); Ok(())};
                let result = timeout(BARGE_CONFIRM_TIMEOUT+Duration::from_secs(1), verify_barge_in_with(reply,&mut rx,&mut deferred,&mut vad,true,|frames| {
                    if failure=="start" {return Err("test ASR unavailable".into());}
                    let (mut probe, guard)=fake_barge_probe(frames,&[],true); guards.push(guard);
                    if failure=="stream" {
                        let (tx, events)=mpsc::unbounded_channel(); probe.events=events;
                        tx.send(StreamingSpeechEvent::Error("test ASR closed".into())).unwrap();
                    }
                    Ok(probe)
                })).await.expect("candidate must be bounded").unwrap();
                assert!(result.is_none()); assert!(completed.load(Ordering::SeqCst));
            }
        });
    }

    #[test]
    fn noise_and_one_character_do_not_cancel_the_reply() {
        tauri::async_runtime::block_on(async {
            for texts in [vec![], vec!["嗯", "嗯，！"]] {
                let (_tx, mut rx) = mpsc::channel(32);
                let mut deferred = (0..PROCESSED_SPEECH_START_FRAMES).map(|_| Input::Pcm(frame(2500),Some(true))).collect();
                let mut vad = Vad::new(); let mut guards = Vec::new();
                let completed = Arc::new(AtomicBool::new(false)); let flag = completed.clone();
                let reply = async move { flag.store(true,Ordering::SeqCst); Ok(()) };
                let result = verify_barge_in_with(reply,&mut rx,&mut deferred,&mut vad,true,|frames| {
                    let (probe, guard) = fake_barge_probe(frames,&texts,false); guards.push(guard); Ok(probe)
                }).await.unwrap();
                assert!(result.is_none()); assert!(completed.load(Ordering::SeqCst));
            }
        });
    }

    #[test]
    fn final_two_characters_confirm_and_reuse_the_existing_asr_stream() {
        tauri::async_runtime::block_on(async {
            let (_tx, mut rx) = mpsc::channel(32);
            let mut deferred = (0..PREROLL_FRAMES).map(|_| Input::Pcm(frame(180),Some(true))).collect();
            let mut vad = Vad::new(); let mut guards = Vec::new(); let mut starts = 0;
            let reply = std::future::pending::<Result<(),String>>();
            let speech = verify_barge_in_with(reply,&mut rx,&mut deferred,&mut vad,true,|frames| {
                starts += 1;
                let (mut probe, guard) = fake_barge_probe(frames,&[],true); guards.push(guard);
                let (tx, events)=mpsc::unbounded_channel(); probe.events=events;
                for text in ["等", "等，", "等下"] { tx.send(StreamingSpeechEvent::Partial{revision:1,text:text.into(),confidence:None}).unwrap(); }
                tx.send(StreamingSpeechEvent::Final{revision:2,text:"等下".into(),confidence:None}).unwrap();
                Ok(probe)
            }).await.unwrap().unwrap();
            assert_eq!(starts,1); assert_eq!(speech.text,"等下"); assert!(speech.terminal); assert!(!speech.reply_completed);
            assert_eq!(speech.stats.frames,PROCESSED_SPEECH_START_FRAMES);
            assert_eq!(deferred.len(),PREROLL_FRAMES-PROCESSED_SPEECH_START_FRAMES);
            assert!(vad.in_speech); assert!(speech.recognizer.push_pcm(&frame(180)).is_ok());
        });
    }

    #[test]
    fn provisional_two_words_do_not_abort_waiting_tool_reply() {
        tauri::async_runtime::block_on(async {
            let (_tx, mut rx)=mpsc::channel(1);
            let mut deferred=(0..PROCESSED_SPEECH_START_FRAMES).map(|_|Input::Pcm(frame(200),Some(true))).collect();
            let mut vad=Vad::new(); let mut guards=Vec::new();
            let completed=Arc::new(AtomicBool::new(false)); let flag=completed.clone();
            let reply=async move { tokio::time::sleep(Duration::from_millis(30)).await; flag.store(true,Ordering::SeqCst); Ok(()) };
            let result=verify_barge_in_with(reply,&mut rx,&mut deferred,&mut vad,true,|frames| {
                let (probe,guard)=fake_barge_probe(frames,&["你好","谢谢"],false); guards.push(guard); Ok(probe)
            }).await.unwrap();
            assert!(result.is_none()); assert!(completed.load(Ordering::SeqCst));
        });
    }

    #[test]
    fn windows_style_audio_burst_without_text_does_not_cancel_or_grow_unbounded() {
        tauri::async_runtime::block_on(async {
            let (_tx, mut rx) = mpsc::channel(1);
            let mut deferred = (0..(PROCESSED_SPEECH_START_FRAMES+BARGE_CONFIRM_MAX_FRAMES))
                .map(|_| Input::Pcm(frame(2500),Some(true))).collect();
            let mut vad = Vad::new(); let mut guards = Vec::new(); let mut starts = 0;
            let result = verify_barge_in_with(async { Ok(()) },&mut rx,&mut deferred,&mut vad,true,|frames| {
                starts += 1;
                let (probe, guard) = fake_barge_probe(frames,&[],true); guards.push(guard); Ok(probe)
            }).await.unwrap();
            assert!(result.is_none()); assert_eq!(starts,1); assert!(deferred.is_empty());
        });
    }

    #[test]
    fn single_stop_interrupts_but_late_confirmation_does_not_recancel_a_finished_reply() {
        tauri::async_runtime::block_on(async {
            for finish_first in [false,true] {
                let (_tx, mut rx) = mpsc::channel(1);
                let mut deferred = (0..PROCESSED_SPEECH_START_FRAMES).map(|_| Input::Pcm(frame(180),Some(true))).collect();
                let mut vad = Vad::new(); let mut guards = Vec::new();
                let reply = async move { if !finish_first { std::future::pending::<()>().await; } Ok(()) };
                let result = verify_barge_in_with(reply,&mut rx,&mut deferred,&mut vad,true,|frames| {
                    let (mut probe, guard) = fake_barge_probe(frames,&[],true); guards.push(guard);
                    let (tx, events) = mpsc::unbounded_channel(); probe.events = events;
                    tokio::spawn(async move {
                        tokio::time::sleep(Duration::from_millis(5)).await;
                        tx.send(StreamingSpeechEvent::Partial {revision:1,text:"停！".into(),confidence:None}).unwrap();
                    });
                    Ok(probe)
                }).await.unwrap().unwrap();
                assert_eq!(result.text,"停！"); assert_eq!(result.reply_completed,finish_first);
            }
        });
    }

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
        let player=Player{tx,epoch:Arc::new(AtomicU64::new(0)),ack_requests:Arc::new(Mutex::new(Default::default()))};
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
            let session = Session { id:"rtc-test".into(), board_device_id:"board".into(), transport:None, capture_stream:Mutex::new(None), audio_egress:Mutex::new(()), audio_requests:Arc::new(Mutex::new(Default::default())), cancelled:AtomicBool::new(false), duplex:AtomicBool::new(false),
                capture_armed:AtomicBool::new(false), capture_ready:AtomicBool::new(false),
                stop_notify:tokio::sync::Notify::new(), input:tx,
                status:Mutex::new(RealtimeChatStatus { active:true, appearance_id:"pet-a".into(), ..Default::default() }),
                completed_playback:Mutex::new(String::new()), pending_persona:Mutex::new(None), playback_volume:AtomicU32::new(1.0f32.to_bits()) };
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
    fn closed_stream_requires_terminal_but_empty_final_preserves_displayed_partial() {
        tauri::async_runtime::block_on(async {
            let (tx, mut rx) = mpsc::unbounded_channel();
            drop(tx);
            assert!(await_final_text(&mut rx, "已经显示的语音".into()).await.is_err());
            let (tx, mut rx) = mpsc::unbounded_channel();
            tx.send(StreamingSpeechEvent::Partial { revision: 1, text: " ".into(), confidence: None }).unwrap();
            tx.send(StreamingSpeechEvent::Final { revision: 2, text: "".into(), confidence: None }).unwrap();
            assert_eq!(await_final_text(&mut rx, "停下".into()).await.unwrap(), "停下");
            let (tx, mut rx) = mpsc::unbounded_channel();
            tx.send(StreamingSpeechEvent::Error("service failed".into())).unwrap();
            drop(tx);
            assert!(await_final_text(&mut rx, "partial".into()).await.is_err());
        });
    }

    #[test]
    fn cancellation_survives_microphone_queue_drain() {
        let (tx, mut rx) = mpsc::channel(128);
        let session = Session { id:"rtc-test".into(), board_device_id:"test".into(), transport:None, capture_stream:Mutex::new(None), audio_egress:Mutex::new(()), audio_requests:Arc::new(Mutex::new(Default::default())), cancelled:AtomicBool::new(false), duplex:AtomicBool::new(false),
            capture_armed:AtomicBool::new(false), capture_ready:AtomicBool::new(false),
            stop_notify:tokio::sync::Notify::new(), input:tx, status:Mutex::new(RealtimeChatStatus::default()), completed_playback:Mutex::new(String::new()), pending_persona:Mutex::new(None), playback_volume:AtomicU32::new(1.0f32.to_bits()) };
        session.input.try_send(Input::Pcm(vec![0; FRAME_BYTES], None)).unwrap();
        request_stop(&session, "key");
        drain_pending(&mut rx);
        assert!(session.cancelled.load(Ordering::SeqCst));
        tauri::async_runtime::block_on(async { timeout(Duration::from_millis(100), session.stop_notify.notified()).await.unwrap(); });
    }

    #[test]
    fn asr_timeout_discards_incomplete_round_and_next_utterance_succeeds() {
        tauri::async_runtime::block_on(async {
            let (old_tx, mut old_rx)=mpsc::unbounded_channel();
            old_tx.send(StreamingSpeechEvent::Partial {revision:1,text:"打开".into(),confidence:None}).unwrap();
            let error=await_final_text_with_timeout(&mut old_rx,String::new(),Duration::from_millis(5)).await.unwrap_err();
            assert!(recoverable_asr_error(&error));
            drop(old_rx);
            assert!(old_tx.send(StreamingSpeechEvent::Final {revision:2,text:"过期指令".into(),confidence:None}).is_err());
            let mut vad=Vad::new(); vad.in_speech=true; vad.speech_run=10;
            let mut latest="打开".to_string(); let mut displayed=latest.clone();
            let mut final_text=Some("旧内容".into()); let mut terminal=true;
            let mut pending=VecDeque::from([Input::Pcm(vec![0;FRAME_BYTES],None)]);
            reset_timed_out_asr(&mut vad,&mut latest,&mut displayed,&mut final_text,&mut terminal,&mut pending);
            assert!(!vad.in_speech && vad.speech_run==0 && pending.is_empty());
            assert!(latest.is_empty() && displayed.is_empty() && final_text.is_none() && !terminal);
            let (tx,mut rx)=mpsc::unbounded_channel();
            tx.send(StreamingSpeechEvent::Final {revision:1,text:"帮我打开36kr".into(),confidence:None}).unwrap();
            assert_eq!(await_final_text_with_timeout(&mut rx,String::new(),Duration::from_millis(50)).await.unwrap(),"帮我打开36kr");
        });
    }

    #[test]
    fn asr_recovery_never_masks_configuration_or_authentication_errors() {
        assert!(recoverable_asr_error("语音识别结果超时"));
        assert!(recoverable_asr_error("ASR connection timeout"));
        for error in ["401 鉴权失败", "403 授权失败", "请配置 API Key", "协议错误", "401 request timeout"] {
            assert!(!recoverable_asr_error(error),"{error}");
        }
    }

    #[test]
    fn terminal_lifecycle_transient_rounds_recover_but_user_action_errors_do_not() {
        for error in ["cloud speech recognition input is closed", "ASR connection reset",
            "火山引擎 ASR 接收失败", "云端 ASR 工作任务在最终结果前结束，请重新发声"] {
            assert!(recoverable_asr_error(error), "{error}");
        }
        for error in ["429 quota timeout", "403 connection closed", "余额不足", "额度不足",
            "未配置 API Key", "resource permission timeout", "协议错误"] {
            assert!(!recoverable_asr_error(error), "{error}");
        }
    }

    #[test]
    fn terminal_lifecycle_stale_generation_never_matches_active_round() {
        assert!(asr_generation_matches(2, 2));
        assert!(!asr_generation_matches(2, 1));
        assert!(!asr_generation_matches(0, 0));
    }

    #[test]
    fn terminal_lifecycle_old_error_mailbox_cannot_replace_next_final() {
        tauri::async_runtime::block_on(async {
            let (old_tx, old_rx) = mpsc::unbounded_channel();
            drop(old_rx);
            let (next_tx, mut next_rx) = mpsc::unbounded_channel();
            assert!(old_tx.send(StreamingSpeechEvent::Error("old round".into())).is_err());
            next_tx.send(StreamingSpeechEvent::Final {revision:1,text:"synthetic next".into(),confidence:None}).unwrap();
            assert_eq!(await_final_text(&mut next_rx, String::new()).await.unwrap(), "synthetic next");
        });
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
        let capture_all = std::env::var("P4_NEAR_CAPTURE_ALL").as_deref() == Ok("1");
        let mut short_audio = Vec::new();
        let mut ended = false;
        let mut frames = 0;
        let mut hints = 0;
        let mut peak = 0f32;
        while began.elapsed() < Duration::from_secs(18) {
            if cleanup.2.is_none() && began.elapsed() > Duration::from_secs(2) {
                if capture_all {
                    let tx=asr_tx.clone();
                    rec=Some(StreamingSpeechRecognizer::start(move |e| {let _=tx.send(e);}).unwrap());
                }
                cleanup.2=Some(std::process::Command::new("/usr/bin/afplay").arg(&wav).spawn().unwrap());
            }
            let Ok((topic,p))=events.recv_timeout(Duration::from_millis(30)) else {continue};
            if topic=="audio/error" {panic!("device audio failure");}
            if topic!="audio/chunk" {continue;}
            assert_eq!(p["aec"],true);
            let data=BASE64.decode(p["data"].as_str().unwrap()).unwrap();
            frames+=1; hints+=usize::from(p["vadSpeech"]==true); peak=peak.max(Vad::rms(&data));
            if capture_all {
                if let Some(rec)=rec.as_ref() {
                    rec.push_pcm(&data).unwrap();
                    if began.elapsed()>Duration::from_millis(3000+pcm.len() as u64*1000/32000) {
                        rec.finish().unwrap();ended=true;break;
                    }
                }
                continue;
            }
            match vad.step_in_context(&data,p["vadSpeech"].as_bool(),VadContext::Listening) {
                VadStep::Start(preroll) => {
                    println!("near-end VAD start frame={frames} preroll={} rms={:.1}",preroll.len(),Vad::rms(&data));
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
        let session = Arc::new(Session { id:id.clone(), board_device_id:board.clone(), transport:None, capture_stream:Mutex::new(None), audio_egress:Mutex::new(()), audio_requests:Arc::new(Mutex::new(Default::default())), cancelled:AtomicBool::new(false), duplex:AtomicBool::new(true),
            capture_armed:AtomicBool::new(true), capture_ready:AtomicBool::new(false),
            stop_notify:tokio::sync::Notify::new(), input, status:Mutex::new(RealtimeChatStatus::default()), completed_playback:Mutex::new(String::new()), pending_persona:Mutex::new(None), playback_volume:AtomicU32::new(1.0f32.to_bits()) });
        let player = spawn_player(usb.clone(), board.clone(), session.clone());
        usb.send_to_board(&board,"audio/conversation",&json!({"enabled":true,"halfDuplex":false,"sessionId":id})).unwrap();
        // Match begin_capture: playback must not race DSP handle recreation.
        // Hardware tests written before the readiness handshake need the same
        // gate as production, not a greeting sent immediately after enable.
        let ready_until=Instant::now()+Duration::from_secs(3);
        let mut prepared=false;
        while Instant::now()<ready_until {
            if let Ok((topic,payload))=events_rx.recv_timeout(Duration::from_millis(30)) {
                if topic=="audio/status" && payload["conversationSessionId"]==id
                    && payload["conversation"]==true && payload["audioAec"]==true
                    && payload["audioAecPreparing"]==false {prepared=true;break;}
            }
        }
        assert!(prepared,"DSP did not become ready before test playback");
        tauri::async_runtime::block_on(async {
            let cfg = voice_chat_settings::tts_config("zh_male_naiqimengwa_mars_bigtts", "", 1.05).unwrap();
            let mut tts = DoubaoTtsClient::new(cfg);
            hud(&usb, &board, &id, "preparing", "音频测试", "");
            player.send(PlayerCmd::Begin(id.clone())).unwrap();
            hud(&usb, &board, &id, "speaking", "音频测试", "");
            speak_one(&mut tts, &player, "你好，设备语音播放测试。请稍等。" ).await.unwrap();
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
                    // Match the production reply/barge-in gate explicitly.
                    if matches!(vad.step_in_context(&pcm,payload["vadSpeech"].as_bool(),VadContext::Reply),VadStep::Start(_)) {
                        false_starts+=1;
                        println!("reply echo candidate frame={duplex_frames} rms={:.1}",Vad::rms(&pcm));
                    }
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

    #[test]
    #[ignore = "exclusive physical P4; metadata-only DSP throughput test with silent duplex playback"]
    fn live_device_dsp_timing() {
        let usb=UsbSerialManager::new();
        let (tx,rx)=std_mpsc::channel();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move |topic,payload| {
            if topic=="audio/chunk" {
                let _=tx.send((topic,json!({"aec":payload["aec"],"seq":payload["seq"]})));
            } else if matches!(topic.as_str(),"audio/status"|"audio/diagnostic") { let _=tx.send((topic,payload)); }
        }).unwrap();
        let board=usb.status().board_device_id;
        assert_eq!(board,std::env::var("P4_EXPECTED_BOARD_ID").unwrap());
        struct Cleanup(UsbSerialManager,String);
        impl Drop for Cleanup {fn drop(&mut self) {
            let _=self.0.send_to_board(&self.1,"audio/conversation",&json!({"enabled":false}));
            self.0.disconnect();
        }}
        let _cleanup=Cleanup(usb.clone(),board.clone());
        let id=format!("dsp-probe-{}",now_ms());
        usb.send_to_board(&board,"audio/conversation",&json!({"enabled":true,"halfDuplex":false,"sessionId":id})).unwrap();
        let duration=std::env::var("P4_DSP_TEST_SECONDS").ok().and_then(|s|s.parse::<u64>().ok()).unwrap_or(12).clamp(5,180);
        let started=Instant::now();let mut frames=0;let mut failures=0;let mut ready=false;
        let mut last_query=Instant::now();let mut next_play=Instant::now();let mut sequence=0;
        let playback=std::env::var("P4_DSP_PLAYBACK").as_deref()==Ok("1");
        let play_id=format!("{id}-play");let silence=BASE64.encode(vec![0u8;3200]);
        while started.elapsed()<Duration::from_secs(duration) {
            if let Ok((topic,payload))=rx.recv_timeout(Duration::from_millis(10)) {
                if topic=="audio/chunk" {frames+=1;if payload["aec"]!=true {failures+=1;}}
                else if payload["conversationSessionId"]==id {
                    let available=payload["audioAec"]==true && payload["audioAecPreparing"]!=true;
                    if topic=="audio/status" && available && !ready {
                        ready=true;
                        if playback {usb.send_to_board(&board,"audio/play_begin",&json!({"sessionId":play_id})).unwrap();}
                    }
                    if payload["event"]=="aec_fallback" {failures+=1;}
                    let mut details=json!({"event":payload["event"],"available":available,"reason":payload["aecReason"]});
                    for key in ["dspFrames","dspLastUs","dspMaxUs","dspAecUs","dspNsUs","dspVadUs","aecReferenceSamples","internalFreeBytes","internalLargestBytes","captureTxLastUs","captureTxMaxUs","captureStackMinBytes"] {details[key]=payload[key].clone();}
                    eprintln!("DSP {}",details);
                }
            }
            if ready && playback && Instant::now()>=next_play {
                usb.send_to_board(&board,"audio/play_chunk",&json!({"sessionId":play_id,"seq":sequence,"bytes":3200,"data":silence})).unwrap();
                sequence+=1;next_play+=Duration::from_millis(100);
                if next_play+Duration::from_millis(500)<Instant::now() {next_play=Instant::now();}
            }
            if last_query.elapsed()>=Duration::from_secs(1) {
                usb.send_to_board(&board,"audio/query",&json!({})).unwrap();
                usb.send_to_board(&board,"system/heartbeat",&json!({})).unwrap();
                last_query=Instant::now();
            }
            if failures>0 {break;}
        }
        eprintln!("DSP SUMMARY frames={frames} elapsedMs={} failures={failures} playback={playback}",started.elapsed().as_millis());
        assert_eq!(failures,0,"device DSP failed");
        assert!(frames>duration*40,"capture did not keep up with 20ms cadence");
    }

    #[test]
    #[cfg(target_os = "macos")]
    #[ignore = "exclusive P4 and saved cloud credentials; actual TTS, microphone and production ASR-confirmed interruption path"]
    fn live_device_verified_duplex() {
        let dir=std::path::PathBuf::from(std::env::var("PET_REALTIME_SETTINGS_DIR").unwrap());
        voice_chat_settings::configure_storage_dir(dir.clone()).unwrap();
        crate::llm_network::configure_storage_dir(dir.clone()).unwrap();
        crate::volcengine_asr::configure_storage_dir(dir.clone()).unwrap();
        diagnostics::configure(&dir).unwrap();
        let board=std::env::var("P4_EXPECTED_BOARD_ID").unwrap();
        let id=format!("rtc-verified-hil-{}",now_ms());
        let (input,mut rx)=mpsc::channel(128);
        let session=Arc::new(Session{id:id.clone(),board_device_id:board.clone(), transport:None, capture_stream:Mutex::new(None), audio_egress:Mutex::new(()), audio_requests:Arc::new(Mutex::new(Default::default())),cancelled:AtomicBool::new(false),
            duplex:AtomicBool::new(true),capture_armed:AtomicBool::new(false),capture_ready:AtomicBool::new(false),
            stop_notify:tokio::sync::Notify::new(),input,status:Mutex::new(Default::default()),
            completed_playback:Mutex::new(String::new()),pending_persona:Mutex::new(None), playback_volume:AtomicU32::new(1.0f32.to_bits())});
        let frames=Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let faults=Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let usb=UsbSerialManager::new();
        let routed=session.clone();let counted=frames.clone();let failed=faults.clone();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move |topic,p| {
            if topic=="audio/chunk" {counted.fetch_add(1,Ordering::Relaxed);}
            if topic=="audio/diagnostic" {
                if p["event"]=="aec_fallback" {failed.fetch_add(1,Ordering::Relaxed);}
                if p["event"]=="playback_completed" {
                    *routed.completed_playback.lock().unwrap()=p["sessionId"].as_str().unwrap_or("").into();
                }
            }
            route_session_audio(&routed,&topic,&p);
        }).unwrap();
        assert_eq!(usb.status().board_device_id,board);
        struct Cleanup(UsbSerialManager,Arc<Session>);
        impl Drop for Cleanup {fn drop(&mut self) {
            self.1.cancelled.store(true,Ordering::SeqCst);
            let _=self.0.send_to_board(&self.1.board_device_id,"audio/play_flush",&json!({}));
            let _=self.0.send_to_board(&self.1.board_device_id,"audio/conversation",&json!({"enabled":false}));
            let _=self.0.send_to_board(&self.1.board_device_id,"ui/conversation",&json!({"state":"ended"}));
            self.0.disconnect();
        }}
        let _cleanup=Cleanup(usb.clone(),session.clone());
        let player=spawn_player(usb.clone(),board,session.clone());
        tauri::async_runtime::block_on(async {
            hud(&usb,&session.board_device_id,&id,"preparing","音频测试","");
            begin_capture(&usb,&session,true).await.unwrap();
            let mut tts=DoubaoTtsClient::new(voice_chat_settings::tts_config("zh_male_naiqimengwa_mars_bigtts","",1.05).unwrap());
            let mut vad=Vad::new();vad.session=Some(session.clone());vad.diagnostic_session=id.clone();
            let mut deferred=VecDeque::new();
            let text="你好，现在正在检查双向实时对话。设备会继续播放，同时保持收音。测试结束后就可以正常聊天了。";
            for turn in 1..=3 {
                {let mut status=session.status.lock().unwrap();status.state="speaking".into();status.reply=text.into();}
                hud(&usb,&session.board_device_id,&id,"speaking","音频测试","");
                let reply=async {
                    player.send(PlayerCmd::Begin(format!("{id}-{turn}"))).unwrap();
                    speak_one(&mut tts,&player,text).await?;
                    wait_playback(&player).await;
                    Ok(())
                };
                let interruption=timeout(Duration::from_secs(40),listen_with_verified_barge_in(reply,&mut rx,&mut deferred,&mut vad,true)).await.unwrap().unwrap();
                if let Some(speech)=interruption.as_ref() {
                    let normalized=|s:&str|s.chars().filter(|c|c.is_alphanumeric()).collect::<String>();
                    let candidate=normalized(&speech.text);
                    eprintln!("VERIFIED candidate chars={} containedInSyntheticReply={}",candidate.chars().count(),normalized(text).contains(&candidate));
                }
                assert!(interruption.is_none(),"actual ASR confirmation interrupted test playback");
                assert!(!session.cancelled.load(Ordering::SeqCst),"capture or playback stopped");
                eprintln!("VERIFIED DUPLEX round={turn} frames={} aecFaults={} confirmedInterruptions=0",frames.load(Ordering::Relaxed),faults.load(Ordering::Relaxed));
            }
            tts.reset().await;
        });
        assert_eq!(faults.load(Ordering::Relaxed),0);
        assert!(frames.load(Ordering::Relaxed)>600);
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
        let session=Arc::new(Session{id:id.clone(),board_device_id:board.clone(), transport:None, capture_stream:Mutex::new(None), audio_egress:Mutex::new(()), audio_requests:Arc::new(Mutex::new(Default::default())),cancelled:AtomicBool::new(false),duplex:AtomicBool::new(true),capture_armed:AtomicBool::new(true),capture_ready:AtomicBool::new(false),stop_notify:tokio::sync::Notify::new(),input,status:Mutex::new(Default::default()),completed_playback:Mutex::new(String::new()),pending_persona:Mutex::new(None), playback_volume:AtomicU32::new(1.0f32.to_bits())});
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
        for page in ["main", "app", "components"] { assert!(realtime_entry_context(page)); }
        for page in ["", "unknown", "settings"] { assert!(!realtime_entry_context(page)); }
    }
}
