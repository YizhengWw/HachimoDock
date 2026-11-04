/*
 * [Input] ESP32-P4 audio/begin, audio/chunk, audio/end, audio/error, and audio/status messages.
 * [Output] Validated 16 kHz mono PCM for local recognition, optional legacy UDP relay, and UI diagnostics.
 * [Pos] Tauri-side P4 USB microphone transport.
 */

use base64::{engine::general_purpose::STANDARD, Engine as _};
use serde_json::{json, Value};
use std::collections::VecDeque;
use std::net::UdpSocket;

pub const DEFAULT_PCM_RELAY_PORT: u16 = 50_001;

const AUDIO_FORMAT: &str = "pcm_s16le";
const AUDIO_SAMPLE_RATE: u64 = 16_000;
const AUDIO_CHANNELS: u64 = 1;
const AUDIO_BITS_PER_SAMPLE: u64 = 16;
const MAX_CAPTURE_BYTES: usize = AUDIO_SAMPLE_RATE as usize * 2 * 30;
const FNV1A64_OFFSET: u64 = 0xcbf29ce484222325;
const FNV1A64_PRIME: u64 = 0x00000100000001b3;
const MAX_RETIRED_CAPTURES: usize = 8;

#[derive(Clone, Debug, PartialEq, Eq)]
struct TransportScope {
    board: String,
    connection: String,
    boot: String,
}

struct RetiredCapture {
    id: String,
    board: String,
    capture: String,
    scope: Option<TransportScope>,
    event: Value,
}

#[derive(Debug)]
struct UsbAudioSession {
    capture_id: String,
    scope: Option<TransportScope>,
    id: String,
    board_device_id: String,
    next_sequence: u64,
    total_bytes: u64,
    checksum: u64,
    pcm: Vec<u8>,
}

pub struct CompletedUsbAudio {
    pub capture_id: String,
    pub connection_id: String,
    pub boot_id: String,
    pub session_id: String,
    pub board_device_id: String,
    pub pcm: Vec<u8>,
}

pub struct ValidatedUsbAudioChunk {
    pub capture_id: String,
    pub connection_id: String,
    pub boot_id: String,
    pub session_id: String,
    pub board_device_id: String,
    pub sequence: u64,
    pub pcm: Vec<u8>,
}

pub struct UsbAudioRelay {
    enabled: bool,
    forward_udp: bool,
    target_port: u16,
    socket: Option<UdpSocket>,
    session: Option<UsbAudioSession>,
    validated_chunk: Option<ValidatedUsbAudioChunk>,
    completed: Option<CompletedUsbAudio>,
    scope: Option<TransportScope>,
    retired: VecDeque<RetiredCapture>,
    capture_owner: Option<String>,
}

impl Default for UsbAudioRelay {
    fn default() -> Self {
        Self {
            enabled: false,
            forward_udp: false,
            target_port: DEFAULT_PCM_RELAY_PORT,
            socket: None,
            session: None,
            validated_chunk: None,
            completed: None,
            scope: None,
            retired: VecDeque::new(),
            capture_owner: None,
        }
    }
}

impl UsbAudioRelay {
    pub fn configure(&mut self, enabled: bool, target_port: u16, forward_udp: bool) -> Value {
        self.configure_inner(enabled, target_port, forward_udp, self.scope.clone(), None)
    }

    pub fn configure_scoped(&mut self, enabled: bool, target_port: u16, forward_udp: bool,
        board: &str, connection_id: &str, boot_id: &str) -> Value {
        let Some(scope) = transport_scope(board, connection_id, boot_id) else {
            return relay_error("configured", "USB audio control scope is unavailable", "");
        };
        // A delayed stop from an old port/boot must not disable the new owner.
        if !enabled && self.scope.as_ref().is_some_and(|current| current != &scope) {
            return relay_error("configured", "USB audio control scope changed", "");
        }
        self.configure_inner(enabled, target_port, forward_udp, Some(scope), None)
    }

    /// A component relay must explicitly claim its exact owner. The ordinary
    /// PTT relay never inherits an owned Begin merely because notes exited.
    pub(crate) fn configure_owned_scoped(&mut self, enabled: bool, target_port: u16,
        forward_udp: bool, board: &str, connection_id: &str, boot_id: &str, owner: &str) -> Value {
        if owner.is_empty() || owner.len()>64 || !owner.bytes().all(|b|b.is_ascii_alphanumeric() || b"._-".contains(&b)) {
            return relay_error("configured", "USB audio capture owner is invalid", "");
        }
        let Some(scope)=transport_scope(board,connection_id,boot_id) else {
            return relay_error("configured", "USB audio control scope is unavailable", "");
        };
        if !enabled && self.scope.as_ref().is_some_and(|current|current!=&scope) {
            return relay_error("configured", "USB audio control scope changed", "");
        }
        self.configure_inner(enabled,target_port,forward_udp,Some(scope),Some(owner.into()))
    }

    fn configure_inner(&mut self, enabled: bool, target_port: u16, forward_udp: bool, scope: Option<TransportScope>, owner: Option<String>) -> Value {
        let unchanged = self.enabled == enabled && self.forward_udp == forward_udp
            && self.target_port == target_port && self.scope == scope && self.capture_owner == owner;
        if unchanged {
            return json!({"phase":"configured","ok":true,"enabled":enabled,"forwardUdp":forward_udp,
                "target":format!("127.0.0.1:{target_port}"),"idempotent":true,
                "capturePreserved":self.session.is_some(),"terminatedCapture":null});
        }
        if enabled && self.enabled && self.scope == scope && self.session.is_some() {
            // Port/mode changes cannot silently destroy an ongoing recording.
            return relay_error("configured", "USB audio configuration cannot change during capture", "");
        }
        let terminated = self.session.take().map(|session| {
            let event = relay_error("error", if enabled { "USB audio transport changed during capture" }
                else { "USB audio capture explicitly stopped" }, &session.id);
            self.retire(session, event)
        });
        self.enabled = enabled;
        self.forward_udp = forward_udp;
        self.target_port = target_port;
        self.socket = None;
        self.session = None;
        self.validated_chunk = None;
        self.completed = None;
        self.scope = scope;
        self.capture_owner = owner;
        json!({
            "phase": "configured",
            "ok": true,
            "enabled": enabled,
            "forwardUdp": forward_udp,
            "target": format!("127.0.0.1:{target_port}"),
            "idempotent": false,
            "capturePreserved": false,
            "terminatedCapture": terminated,
        })
    }

    /// Metadata is supplied by the native reader's admitted connection, never
    /// trusted from a device payload or inferred from a recently used port.
    pub fn handle_scoped(&mut self, topic: &str, payload: &Value,
        board: &str, connection_id: &str, boot_id: &str) -> Option<Value> {
        let scope = transport_scope(board, connection_id, boot_id)?;
        if self.scope.as_ref() != Some(&scope) { return None; }
        if payload.get("boardDeviceId").and_then(Value::as_str)
            .is_some_and(|value| !value.is_empty() && value != board) { return None; }
        self.handle(topic, payload)
    }

    /// Called once by the ASR owner's FIRST terminal failure or explicit stop.
    /// Exact opaque capture UUID prevents an old cloud callback from retiring a
    /// new recording even if firmware session IDs repeat after reboot.
    pub fn terminate_capture(&mut self, capture_id: &str, error: &str) -> Option<Value> {
        if !self.session.as_ref().is_some_and(|s| s.capture_id == capture_id) {
            // A valid transport end is not ASR/delivery success. Its first
            // later recognition failure may still become the terminal cause,
            // without touching any newer active capture.
            let old = self.retired.iter_mut().find(|s| s.capture == capture_id)?;
            if old.event["ok"] != true || old.event["phase"] != "end" { return None; }
            old.event["phase"] = json!("error");
            old.event["ok"] = json!(false);
            old.event["error"] = json!(error);
            return Some(old.event.clone());
        }
        let session = self.session.take()?;
        self.completed = None;
        self.validated_chunk = None;
        let event = relay_error("error", error, &session.id);
        Some(self.retire(session, event))
    }

    fn retire(&mut self, session: UsbAudioSession, event: Value) -> Value {
        let event = session_event(&session, event);
        self.socket = None;
        self.retired.push_front(RetiredCapture { id:session.id, board:session.board_device_id,
            capture:session.capture_id, scope:session.scope, event:event.clone() });
        self.retired.truncate(MAX_RETIRED_CAPTURES);
        event
    }

    fn retired_event(&self, topic: &str, payload: &Value) -> Option<Value> {
        let id = string_field(payload, "sessionId");
        let board = string_field(payload, "boardDeviceId");
        let old = self.retired.iter().find(|s| s.id == id && s.scope == self.scope
            && (board.is_empty() || s.board == board))?;
        let mut event = old.event.clone();
        event["terminalAlreadyReported"] = json!(true);
        event["duplicate"] = json!(true);
        event["lateEvent"] = json!(topic);
        event["captureId"] = json!(old.capture);
        Some(event)
    }

    fn fail_current(&mut self, phase: &str, error: &str, session_id: &str) -> Value {
        self.completed = None;
        let event = relay_error(phase, error, session_id);
        match self.session.take() {
            Some(session) => self.retire(session, event),
            None => event,
        }
    }

    fn reject_unstarted(&mut self, phase: &str, error: &str, payload: &Value) -> Value {
        let id = string_field(payload, "sessionId");
        // This is a rejected transport attempt, never an admitted recording.
        // Bound its metadata and retain its first cause so every following
        // chunk/end does not manufacture another missing-session failure.
        if id.is_empty() || id.len() > 128 {
            return relay_error(phase, error, "");
        }
        let board = self.scope.as_ref().map(|scope| scope.board.as_str())
            .unwrap_or_else(|| string_field(payload, "boardDeviceId"));
        if board.len() > 128 { return relay_error(phase, error, ""); }
        let capture = uuid::Uuid::new_v4().to_string();
        let event = json!({
            "phase": phase, "ok": false, "error": error,
            "sessionId": id, "utteranceId": id, "captureId": capture,
            "boardDeviceId": board,
            "connectionId": self.scope.as_ref().map(|scope| scope.connection.as_str()).unwrap_or(""),
            "bootId": self.scope.as_ref().map(|scope| scope.boot.as_str()).unwrap_or(""),
            "admissionRejected": true,
        });
        self.retired.push_front(RetiredCapture { id: id.into(), board: board.into(),
            capture, scope: self.scope.clone(), event: event.clone() });
        self.retired.truncate(MAX_RETIRED_CAPTURES);
        event
    }

    pub fn handle(&mut self, topic: &str, payload: &Value) -> Option<Value> {
        self.validated_chunk = None;
        if self.enabled && matches!(topic, "audio/begin" | "audio/chunk" | "audio/end" | "audio/error") {
            if let Some(event) = self.retired_event(topic, payload) { return Some(event); }
        }
        match topic {
            "audio/status" => Some(json!({
                "phase": "status",
                "ok": true,
                "relayEnabled": self.enabled,
                "target": format!("127.0.0.1:{}", self.target_port),
                "device": payload,
            })),
            "audio/error" => {
                let current = self.session.as_ref()?;
                if !frame_matches(current, payload) { return None; }
                let event = json!({
                    "phase": "error",
                    "ok": false,
                    "sessionId": string_field(payload, "sessionId"),
                    "code": string_field(payload, "code"),
                    "error": string_field(payload, "error"),
                });
                let session = self.session.take()?;
                self.completed = None;
                Some(self.retire(session, event))
            }
            "audio/begin" if self.enabled => Some(self.begin(payload)),
            "audio/chunk" if self.enabled => self.chunk(payload),
            "audio/end" if self.enabled => Some(self.end(payload)),
            _ => None,
        }
    }

    fn begin(&mut self, payload: &Value) -> Value {
        let session_id = string_field(payload, "sessionId");
        if session_id.is_empty() {
            return relay_error("begin", "missing sessionId", "");
        }
        // Only ordinary empty/absent owner belongs to PTT. Malformed or
        // unclaimed owned Begin is retired as an error, never a synthetic End,
        // ASR start, or replacement generic session. Following packets drain
        // through the bounded retired-capture table for this native scope.
        let owner_matches=match &self.capture_owner {
            Some(expected)=>payload.get("captureOwner").and_then(Value::as_str)==Some(expected.as_str()),
            None=>payload.get("captureOwner").is_none_or(|v|v.as_str()==Some("")),
        };
        if !owner_matches {
            if self.session.as_ref().is_some_and(|current|frame_matches(current,payload)) {
                return self.fail_current("begin", "USB audio capture owner conflicts with the admitted session", session_id);
            }
            return self.reject_unstarted("begin", "USB audio capture has no matching owner; not admitted to push-to-talk", payload);
        }
        let supported = string_field(payload, "format") == AUDIO_FORMAT
            && u64_field(payload, "sampleRate") == Some(AUDIO_SAMPLE_RATE)
            && u64_field(payload, "channels") == Some(AUDIO_CHANNELS)
            && u64_field(payload, "bitsPerSample") == Some(AUDIO_BITS_PER_SAMPLE);
        if !supported {
            return if self.session.is_none() {
                self.reject_unstarted("begin", "unsupported PCM format", payload)
            } else { stale_event("begin", session_id) };
        }
        let board_device_id = self.scope.as_ref().map(|s| s.board.clone())
            .unwrap_or_else(|| string_field(payload, "boardDeviceId").to_owned());
        if let Some(session) = self.session.as_ref() {
            if session.id == session_id && session.board_device_id == board_device_id {
                return session_event(session, json!({
                    "phase": "begin",
                    "ok": true,
                    "duplicate": true,
                    "sessionId": session_id,
                    "nextSequence": session.next_sequence,
                    "bytes": session.total_bytes,
                    "format": AUDIO_FORMAT,
                    "sampleRate": AUDIO_SAMPLE_RATE,
                    "channels": AUDIO_CHANNELS,
                    "bitsPerSample": AUDIO_BITS_PER_SAMPLE,
                }));
            }
        }

        if let Some(previous) = self.session.take() {
            let event = relay_error("error", "USB audio capture superseded by a fresh begin", &previous.id);
            self.retire(previous, event);
        }
        self.socket = None;
        self.completed = None;

        if self.forward_udp {
            let socket = match UdpSocket::bind("127.0.0.1:0") {
                Ok(socket) => socket,
                Err(error) => {
                    return relay_error(
                        "begin",
                        &format!("failed to open PCM relay socket: {error}"),
                        session_id,
                    )
                }
            };
            self.socket = Some(socket);
        }
        self.session = Some(UsbAudioSession {
            capture_id: uuid::Uuid::new_v4().to_string(),
            scope: self.scope.clone(),
            id: session_id.to_string(),
            board_device_id: board_device_id.to_string(),
            next_sequence: 0,
            total_bytes: 0,
            checksum: FNV1A64_OFFSET,
            pcm: Vec::with_capacity(64 * 1024),
        });
        session_event(self.session.as_ref().expect("new capture"), json!({
            "phase": "begin",
            "ok": true,
            "sessionId": session_id,
            "forwardUdp": self.forward_udp,
            "target": format!("127.0.0.1:{}", self.target_port),
            "format": AUDIO_FORMAT,
            "sampleRate": AUDIO_SAMPLE_RATE,
            "channels": AUDIO_CHANNELS,
            "bitsPerSample": AUDIO_BITS_PER_SAMPLE,
        }))
    }

    fn chunk(&mut self, payload: &Value) -> Option<Value> {
        let session_id = string_field(payload, "sessionId").to_string();
        let Some(current) = self.session.as_ref() else {
            return Some(self.reject_unstarted(
                "chunk",
                "本段录音缺少有效开始帧，未送往识别服务；请重新按住说话",
                payload,
            ));
        };
        if !frame_matches(current, payload) {
            return Some(stale_event("chunk", &session_id));
        }

        let Some(sequence) = u64_field(payload, "seq") else {
            return Some(self.fail_current("chunk", "missing audio sequence", &session_id));
        };
        if sequence != current.next_sequence {
            let expected = current.next_sequence;
            return Some(self.fail_current(
                "chunk",
                &format!("audio sequence mismatch: expected {expected}, got {sequence}"),
                &session_id,
            ));
        }

        let decoded = match STANDARD.decode(string_field(payload, "data")) {
            Ok(decoded) => decoded,
            Err(error) => {
                    return Some(self.fail_current(
                    "chunk",
                    &format!("invalid audio base64: {error}"),
                    &session_id,
                ));
            }
        };
        let declared_bytes = u64_field(payload, "bytes");
        if declared_bytes != Some(decoded.len() as u64) {
            let (error_kind, error) = match payload.get("bytes") {
                None => ("audio_bytes_missing", "audio byte count field is missing"),
                Some(_) if declared_bytes.is_none() => (
                    "audio_bytes_invalid",
                    "audio byte count field must be a non-negative integer",
                ),
                Some(_) => ("audio_byte_count_mismatch", "audio byte count mismatch"),
            };
            // Fixed types and counts only: never log data, PCM, checksum, IDs,
            // raw JSON, or arbitrary JSON values. This is the first terminal
            // rejection; later packets drain through the existing retired table.
            let metadata = json!({
                "stage": "usb_audio_chunk_rejected", "errorKind": error_kind,
                "seq": sequence, "expectedSeq": current.next_sequence,
                "bytesFieldType": audio_field_type(payload.get("bytes")),
                "declaredBytes": declared_bytes, "decodedBytes": decoded.len(),
                "dataFieldType": audio_field_type(payload.get("data")),
                "dataFieldPresent": payload.get("data").is_some(),
                "base64CharCount": string_field(payload, "data").len(),
                "priorChunks": current.next_sequence, "priorBytes": current.total_bytes,
            });
            self.completed = None;
            let mut event = relay_error("chunk", error, &session_id);
            event["errorKind"] = json!(error_kind);
            let event = match self.session.take() {
                Some(session) => self.retire(session, event),
                None => event,
            };
            crate::realtime_chat_log::record("", "usb_audio_chunk_rejected", metadata);
            return Some(event);
        }
        let frame_checksum = fnv1a64_update(FNV1A64_OFFSET, &decoded);
        if string_field(payload, "checksum") != format!("{frame_checksum:016x}") {
            return Some(self.fail_current(
                "chunk",
                "audio frame checksum mismatch",
                &session_id,
            ));
        }
        if current.pcm.len().saturating_add(decoded.len()) > MAX_CAPTURE_BYTES {
            return Some(self.fail_current(
                "chunk",
                "device microphone recording exceeds 30 seconds",
                &session_id,
            ));
        }

        if self.forward_udp {
            let Some(socket) = self.socket.as_ref() else {
                    return Some(self.fail_current(
                    "chunk",
                    "audio relay socket is closed",
                    &session_id,
                ));
            };
            if let Err(error) = socket.send_to(&decoded, ("127.0.0.1", self.target_port)) {
                    return Some(self.fail_current(
                    "chunk",
                    &format!("failed to relay PCM: {error}"),
                    &session_id,
                ));
            }
        }

        let session = self.session.as_mut().expect("session checked above");
        session.next_sequence += 1;
        session.total_bytes += decoded.len() as u64;
        session.checksum = fnv1a64_update(session.checksum, &decoded);
        session.pcm.extend_from_slice(&decoded);
        self.validated_chunk = Some(ValidatedUsbAudioChunk {
            capture_id: session.capture_id.clone(),
            connection_id: session.scope.as_ref().map(|s| s.connection.clone()).unwrap_or_default(),
            boot_id: session.scope.as_ref().map(|s| s.boot.clone()).unwrap_or_default(),
            session_id: session_id.clone(),
            board_device_id: session.board_device_id.clone(),
            sequence,
            pcm: decoded,
        });

        if sequence % 25 == 0 {
            Some(session_event(session, json!({
                "phase": "streaming",
                "ok": true,
                "sessionId": session_id,
                "chunks": session.next_sequence,
                "bytes": session.total_bytes,
            })))
        } else {
            None
        }
    }

    fn end(&mut self, payload: &Value) -> Value {
        let session_id = string_field(payload, "sessionId").to_string();
        let Some(current) = self.session.as_ref() else {
            return self.reject_unstarted("end",
                "本段录音缺少有效开始帧，未送往识别服务；请重新按住说话", payload);
        };
        // Check BEFORE take: an old release must never consume a newer press.
        if !frame_matches(current, payload) {
            return stale_event("end", &session_id);
        }
        let mut session = self.session.take().expect("matching capture");
        self.completed = None;
        let error = if u64_field(payload, "chunks") != Some(session.next_sequence) {
            Some("audio chunk total mismatch")
        } else if u64_field(payload, "bytes") != Some(session.total_bytes) {
            Some("audio stream byte total mismatch")
        } else if string_field(payload, "checksum") != format!("{:016x}", session.checksum) {
            Some("audio stream checksum mismatch")
        } else { None };
        if let Some(error) = error {
            let event = relay_error("end", error, &session_id);
            return self.retire(session, event);
        }

        self.completed = Some(CompletedUsbAudio {
            capture_id: session.capture_id.clone(),
            connection_id: session.scope.as_ref().map(|s| s.connection.clone()).unwrap_or_default(),
            boot_id: session.scope.as_ref().map(|s| s.boot.clone()).unwrap_or_default(),
            session_id: session.id.clone(),
            board_device_id: session.board_device_id.clone(),
            pcm: std::mem::take(&mut session.pcm),
        });
        let event = json!({
            "phase": "end", "ok": true, "sessionId": session_id,
            "reason": string_field(payload, "reason"),
            "chunks": session.next_sequence, "bytes": session.total_bytes,
            "durationMs": payload.get("durationMs").cloned().unwrap_or(Value::Null),
            "checksum": format!("{:016x}", session.checksum),
        });
        self.retire(session, event)
    }

    pub fn take_completed(&mut self) -> Option<CompletedUsbAudio> {
        self.completed.take()
    }

    pub fn take_validated_chunk(&mut self) -> Option<ValidatedUsbAudioChunk> {
        self.validated_chunk.take()
    }
}

fn transport_scope(board: &str, connection: &str, boot: &str) -> Option<TransportScope> {
    if board.is_empty() || board.len() > 128 || board.chars().any(char::is_control)
        || connection.parse::<u64>().ok().filter(|n| *n != 0).is_none()
        || connection.starts_with('0') || connection.chars().any(|c| !c.is_ascii_digit())
        || boot.len() != 16 || !boot.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    { return None; }
    Some(TransportScope { board:board.into(), connection:connection.into(), boot:boot.into() })
}

fn frame_matches(session: &UsbAudioSession, payload: &Value) -> bool {
    let board = string_field(payload, "boardDeviceId");
    session.id == string_field(payload, "sessionId")
        && (board.is_empty() || board == session.board_device_id)
}

fn session_event(session: &UsbAudioSession, mut event: Value) -> Value {
    event["captureId"] = json!(session.capture_id);
    event["utteranceId"] = json!(session.id);
    event["boardDeviceId"] = json!(session.board_device_id);
    event["connectionId"] = json!(session.scope.as_ref().map(|s| s.connection.as_str()).unwrap_or(""));
    event["bootId"] = json!(session.scope.as_ref().map(|s| s.boot.as_str()).unwrap_or(""));
    event
}

fn stale_event(phase: &str, id: &str) -> Value {
    let mut event = relay_error(phase, "audio packet belongs to another capture", id);
    event["staleCapture"] = json!(true);
    event
}

fn string_field<'a>(payload: &'a Value, key: &str) -> &'a str {
    payload.get(key).and_then(Value::as_str).unwrap_or_default()
}

// Classify presence/type without serializing even a malformed field value.
fn audio_field_type(value: Option<&Value>) -> &'static str {
    match value {
        None => "missing",
        Some(Value::Null) => "null",
        Some(Value::Bool(_)) => "boolean",
        Some(Value::Number(_)) => "number",
        Some(Value::String(_)) => "string",
        Some(Value::Array(_)) => "array",
        Some(Value::Object(_)) => "object",
    }
}

fn u64_field(payload: &Value, key: &str) -> Option<u64> {
    payload.get(key).and_then(Value::as_u64)
}

fn fnv1a64_update(mut checksum: u64, bytes: &[u8]) -> u64 {
    for byte in bytes {
        checksum ^= u64::from(*byte);
        checksum = checksum.wrapping_mul(FNV1A64_PRIME);
    }
    checksum
}

fn relay_error(phase: &str, error: &str, session_id: &str) -> Value {
    json!({
        "phase": phase,
        "ok": false,
        "sessionId": session_id,
        "error": error,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    fn begin_payload(session_id: &str) -> Value {
        json!({
            "sessionId": session_id,
            "boardDeviceId": "board-p4",
            "format": AUDIO_FORMAT,
            "sampleRate": AUDIO_SAMPLE_RATE,
            "channels": AUDIO_CHANNELS,
            "bitsPerSample": AUDIO_BITS_PER_SAMPLE,
        })
    }

    #[test]
    fn orphan_owned_begin_never_admits_generic_ptt_or_replaces_new_capture() {
        let mut relay=UsbAudioRelay::default();
        assert_eq!(relay.configure_scoped(true,DEFAULT_PCM_RELAY_PORT,false,"board-p4","42","0123456789abcdef")["ok"],true);
        let mut owned=begin_payload("notes-cancelled");owned["captureOwner"]=json!("cancelled-owner");
        let event=relay.handle_scoped("audio/begin",&owned,"board-p4","42","0123456789abcdef").unwrap();
        assert_eq!(event["ok"],false);assert_eq!(event["admissionRejected"],true);
        assert!(relay.session.is_none());assert!(relay.take_validated_chunk().is_none());assert!(relay.take_completed().is_none());
        for topic in ["audio/chunk","audio/end","audio/begin"] {
            let event=relay.handle_scoped(topic,&owned,"board-p4","42","0123456789abcdef").unwrap();
            assert_eq!(event["terminalAlreadyReported"],true);assert_ne!(event["phase"],"end");
            assert!(relay.take_validated_chunk().is_none());assert!(relay.take_completed().is_none());
        }
        let current=begin_payload("ptt-new");
        assert_eq!(relay.handle_scoped("audio/begin",&current,"board-p4","42","0123456789abcdef").unwrap()["ok"],true);
        for i in 0..MAX_RETIRED_CAPTURES+2 {
            let mut foreign=owned.clone();foreign["sessionId"]=json!(format!("foreign-{i}"));
            assert_eq!(relay.handle_scoped("audio/begin",&foreign,"board-p4","42","0123456789abcdef").unwrap()["ok"],false);
            assert_eq!(relay.session.as_ref().unwrap().id,"ptt-new");
        }
        assert_eq!(relay.handle_scoped("audio/begin",&owned,"board-p4","42","0123456789abcdef").unwrap()["ok"],false);
        assert_eq!(relay.session.as_ref().unwrap().id,"ptt-new");
        assert!(relay.handle_scoped("audio/begin",&owned,"board-p4","41","0123456789abcdef").is_none());
        assert!(relay.handle_scoped("audio/begin",&owned,"board-p4","42","fedcba9876543210").is_none());
        assert_eq!(relay.session.as_ref().unwrap().id,"ptt-new");
    }

    #[test]
    fn explicit_owner_is_required_only_for_its_notes_relay_and_conflicts_fail_closed() {
        let mut relay=UsbAudioRelay::default();
        assert_eq!(relay.configure_owned_scoped(true,DEFAULT_PCM_RELAY_PORT,false,"board-p4","42","0123456789abcdef","notes-current")["ok"],true);
        let mut current=begin_payload("owned-new");current["captureOwner"]=json!("notes-current");
        assert_eq!(relay.handle_scoped("audio/begin",&current,"board-p4","42","0123456789abcdef").unwrap()["ok"],true);
        let capture=relay.session.as_ref().unwrap().capture_id.clone();
        let mut old=begin_payload("owned-old");old["captureOwner"]=json!("notes-old");
        assert_eq!(relay.handle_scoped("audio/begin",&old,"board-p4","42","0123456789abcdef").unwrap()["ok"],false);
        assert_eq!(relay.session.as_ref().unwrap().capture_id,capture);
        let mut conflict=current.clone();conflict["captureOwner"]=json!("notes-other");
        assert_eq!(relay.handle_scoped("audio/begin",&conflict,"board-p4","42","0123456789abcdef").unwrap()["ok"],false);
        assert!(relay.session.is_none());assert!(relay.take_completed().is_none());
    }

    #[test]
    fn chunk_length_failures_classify_bytes_and_never_forward_rejected_pcm() {
        for (bytes, kind) in [
            (None, "audio_bytes_missing"),
            (Some(Value::Null), "audio_bytes_invalid"),
            (Some(json!("640")), "audio_bytes_invalid"),
            (Some(json!(-1)), "audio_bytes_invalid"),
            (Some(json!(1.5)), "audio_bytes_invalid"),
            (Some(json!(8)), "audio_byte_count_mismatch"),
        ] {
            let mut relay = UsbAudioRelay::default();
            relay.configure(true, DEFAULT_PCM_RELAY_PORT, false);
            relay.handle("audio/begin", &begin_payload("length-case"));
            let pcm = b"good";
            relay.handle("audio/chunk", &json!({"sessionId":"length-case", "seq":0,
                "bytes":pcm.len(), "data":STANDARD.encode(pcm),
                "checksum":format!("{:016x}", fnv1a64_update(FNV1A64_OFFSET, pcm))}));
            assert!(relay.take_validated_chunk().is_some());
            let mut bad = json!({"sessionId":"length-case", "seq":1,
                "data":STANDARD.encode(b"bad!"), "checksum":"not-checked"});
            if let Some(value) = bytes { bad["bytes"] = value; }
            let event = relay.handle("audio/chunk", &bad).unwrap();
            assert_eq!(event["phase"], "chunk");
            assert_eq!(event["ok"], false);
            assert_eq!(event["errorKind"], kind);
            assert!(relay.session.is_none());
            assert!(relay.take_validated_chunk().is_none());
            assert!(relay.take_completed().is_none());
            let late = relay.handle("audio/chunk", &bad).unwrap();
            assert_eq!(late["terminalAlreadyReported"], true);
            assert_eq!(late["errorKind"], kind);
        }
    }

    #[test]
    fn missing_or_nonstring_data_stays_a_length_failure_not_synthetic_pcm() {
        for data in [None, Some(Value::Null), Some(json!(true)), Some(json!({"x":1})), Some(json!(""))] {
            let mut relay = UsbAudioRelay::default();
            relay.configure(true, DEFAULT_PCM_RELAY_PORT, false);
            relay.handle("audio/begin", &begin_payload("data-case"));
            let mut bad = json!({"sessionId":"data-case", "seq":0, "bytes":640});
            if let Some(value) = data { bad["data"] = value; }
            let event = relay.handle("audio/chunk", &bad).unwrap();
            assert_eq!(event["errorKind"], "audio_byte_count_mismatch");
            assert!(relay.take_validated_chunk().is_none());
            assert!(relay.take_completed().is_none());
        }
    }

    #[test]
    fn validates_and_relays_complete_pcm_stream() {
        let receiver = UdpSocket::bind("127.0.0.1:0").unwrap();
        receiver
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        let port = receiver.local_addr().unwrap().port();
        let mut relay = UsbAudioRelay::default();
        relay.configure(true, port, true);

        assert_eq!(
            relay.handle("audio/begin", &begin_payload("s1")).unwrap()["ok"],
            true
        );
        let pcm = b"test-pcm-frame";
        let checksum = fnv1a64_update(FNV1A64_OFFSET, pcm);
        let progress = relay
            .handle(
                "audio/chunk",
                &json!({
                    "sessionId": "s1",
                    "seq": 0,
                    "bytes": pcm.len(),
                    "checksum": format!("{checksum:016x}"),
                    "data": STANDARD.encode(pcm),
                }),
            )
            .unwrap();
        assert_eq!(progress["ok"], true);
        let validated = relay.take_validated_chunk().unwrap();
        assert_eq!(validated.session_id, "s1");
        assert_eq!(validated.board_device_id, "board-p4");
        assert_eq!(validated.sequence, 0);
        assert_eq!(validated.pcm, pcm);

        let mut received = [0u8; 64];
        let (size, _) = receiver.recv_from(&mut received).unwrap();
        assert_eq!(&received[..size], pcm);

        let end = relay
            .handle(
                "audio/end",
                &json!({
                    "sessionId": "s1",
                    "reason": "released",
                    "chunks": 1,
                    "bytes": pcm.len(),
                    "durationMs": 20,
                    "checksum": format!("{checksum:016x}"),
                }),
            )
            .unwrap();
        assert_eq!(end["ok"], true);
        assert_eq!(end["bytes"], pcm.len());
        let completed = relay.take_completed().unwrap();
        assert_eq!(completed.board_device_id, "board-p4");
        assert_eq!(completed.pcm, pcm);
    }

    #[test]
    fn rejects_sequence_gaps_and_drops_the_session() {
        let mut relay = UsbAudioRelay::default();
        relay.configure(true, DEFAULT_PCM_RELAY_PORT, false);
        relay.handle("audio/begin", &begin_payload("s2"));
        let pcm = b"frame";
        let checksum = fnv1a64_update(FNV1A64_OFFSET, pcm);
        let result = relay
            .handle(
                "audio/chunk",
                &json!({
                    "sessionId": "s2",
                    "seq": 2,
                    "bytes": pcm.len(),
                    "checksum": format!("{checksum:016x}"),
                    "data": STANDARD.encode(pcm),
                }),
            )
            .unwrap();

        assert_eq!(result["ok"], false);
        assert!(result["error"]
            .as_str()
            .unwrap()
            .contains("sequence mismatch"));
        assert!(relay.session.is_none());
    }

    #[test]
    fn duplicate_begin_preserves_the_active_stream_sequence() {
        let mut relay = UsbAudioRelay::default();
        relay.configure(true, DEFAULT_PCM_RELAY_PORT, false);
        relay.handle("audio/begin", &begin_payload("same-session"));

        let first = b"first-frame";
        let first_checksum = fnv1a64_update(FNV1A64_OFFSET, first);
        relay.handle(
            "audio/chunk",
            &json!({
                "sessionId": "same-session",
                "seq": 0,
                "bytes": first.len(),
                "checksum": format!("{first_checksum:016x}"),
                "data": STANDARD.encode(first),
            }),
        );
        relay.take_validated_chunk().unwrap();

        let duplicate = relay
            .handle("audio/begin", &begin_payload("same-session"))
            .unwrap();
        assert_eq!(duplicate["ok"], true);
        assert_eq!(duplicate["duplicate"], true);
        assert_eq!(duplicate["nextSequence"], 1);

        let second = b"second-frame";
        let second_checksum = fnv1a64_update(FNV1A64_OFFSET, second);
        assert!(relay
            .handle(
                "audio/chunk",
                &json!({
                    "sessionId": "same-session",
                    "seq": 1,
                    "bytes": second.len(),
                    "checksum": format!("{second_checksum:016x}"),
                    "data": STANDARD.encode(second),
                }),
            )
            .is_none());
        assert_eq!(relay.take_validated_chunk().unwrap().sequence, 1);

        let stream_checksum = fnv1a64_update(first_checksum, second);
        let end = relay
            .handle(
                "audio/end",
                &json!({
                    "sessionId": "same-session",
                    "reason": "released",
                    "chunks": 2,
                    "bytes": first.len() + second.len(),
                    "durationMs": 40,
                    "checksum": format!("{stream_checksum:016x}"),
                }),
            )
            .unwrap();
        assert_eq!(end["ok"], true);
        assert_eq!(
            relay.take_completed().unwrap().pcm,
            [first.as_slice(), second.as_slice()].concat()
        );
    }

    #[test]
    fn retains_device_pcm_without_opening_the_legacy_udp_relay() {
        let mut relay = UsbAudioRelay::default();
        let configured = relay.configure(true, DEFAULT_PCM_RELAY_PORT, false);
        assert_eq!(configured["forwardUdp"], false);
        assert_eq!(
            relay
                .handle("audio/begin", &begin_payload("local-only"))
                .unwrap()["ok"],
            true
        );
        assert!(relay.socket.is_none());

        let pcm = b"device-microphone-pcm";
        let checksum = fnv1a64_update(FNV1A64_OFFSET, pcm);
        relay.handle(
            "audio/chunk",
            &json!({
                "sessionId": "local-only",
                "seq": 0,
                "bytes": pcm.len(),
                "checksum": format!("{checksum:016x}"),
                "data": STANDARD.encode(pcm),
            }),
        );
        let end = relay.handle(
            "audio/end",
            &json!({
                "sessionId": "local-only",
                "reason": "released",
                "chunks": 1,
                "bytes": pcm.len(),
                "durationMs": 20,
                "checksum": format!("{checksum:016x}"),
            }),
        );
        assert_eq!(end.unwrap()["ok"], true);
        assert_eq!(relay.take_completed().unwrap().pcm, pcm);
    }
}
