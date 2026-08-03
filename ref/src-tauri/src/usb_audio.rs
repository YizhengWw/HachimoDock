/*
 * [Input] ESP32-P4 audio/begin, audio/chunk, audio/end, audio/error, and audio/status messages.
 * [Output] Validated 16 kHz mono PCM for local recognition, optional legacy UDP relay, and UI diagnostics.
 * [Pos] Tauri-side P4 USB microphone transport.
 */

use base64::{engine::general_purpose::STANDARD, Engine as _};
use serde_json::{json, Value};
use std::net::UdpSocket;

pub const DEFAULT_PCM_RELAY_PORT: u16 = 50_001;

const AUDIO_FORMAT: &str = "pcm_s16le";
const AUDIO_SAMPLE_RATE: u64 = 16_000;
const AUDIO_CHANNELS: u64 = 1;
const AUDIO_BITS_PER_SAMPLE: u64 = 16;
const MAX_CAPTURE_BYTES: usize = AUDIO_SAMPLE_RATE as usize * 2 * 30;
const FNV1A64_OFFSET: u64 = 0xcbf29ce484222325;
const FNV1A64_PRIME: u64 = 0x00000100000001b3;

#[derive(Debug)]
struct UsbAudioSession {
    id: String,
    board_device_id: String,
    next_sequence: u64,
    total_bytes: u64,
    checksum: u64,
    pcm: Vec<u8>,
}

pub struct CompletedUsbAudio {
    pub session_id: String,
    pub board_device_id: String,
    pub pcm: Vec<u8>,
}

pub struct ValidatedUsbAudioChunk {
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
        }
    }
}

impl UsbAudioRelay {
    pub fn configure(&mut self, enabled: bool, target_port: u16, forward_udp: bool) -> Value {
        self.enabled = enabled;
        self.forward_udp = forward_udp;
        self.target_port = target_port;
        self.socket = None;
        self.session = None;
        self.validated_chunk = None;
        self.completed = None;
        json!({
            "phase": "configured",
            "ok": true,
            "enabled": enabled,
            "forwardUdp": forward_udp,
            "target": format!("127.0.0.1:{target_port}"),
        })
    }

    pub fn handle(&mut self, topic: &str, payload: &Value) -> Option<Value> {
        self.validated_chunk = None;
        match topic {
            "audio/status" => Some(json!({
                "phase": "status",
                "ok": true,
                "relayEnabled": self.enabled,
                "target": format!("127.0.0.1:{}", self.target_port),
                "device": payload,
            })),
            "audio/error" => {
                self.session = None;
                self.completed = None;
                Some(json!({
                    "phase": "error",
                    "ok": false,
                    "sessionId": string_field(payload, "sessionId"),
                    "code": string_field(payload, "code"),
                    "error": string_field(payload, "error"),
                }))
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
        let supported = string_field(payload, "format") == AUDIO_FORMAT
            && u64_field(payload, "sampleRate") == Some(AUDIO_SAMPLE_RATE)
            && u64_field(payload, "channels") == Some(AUDIO_CHANNELS)
            && u64_field(payload, "bitsPerSample") == Some(AUDIO_BITS_PER_SAMPLE);
        if !supported {
            return relay_error("begin", "unsupported PCM format", session_id);
        }
        let board_device_id = string_field(payload, "boardDeviceId");
        if let Some(session) = self.session.as_ref() {
            if session.id == session_id && session.board_device_id == board_device_id {
                return json!({
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
                });
            }
        }

        self.session = None;
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
            id: session_id.to_string(),
            board_device_id: board_device_id.to_string(),
            next_sequence: 0,
            total_bytes: 0,
            checksum: FNV1A64_OFFSET,
            pcm: Vec::with_capacity(64 * 1024),
        });
        json!({
            "phase": "begin",
            "ok": true,
            "sessionId": session_id,
            "forwardUdp": self.forward_udp,
            "target": format!("127.0.0.1:{}", self.target_port),
            "format": AUDIO_FORMAT,
            "sampleRate": AUDIO_SAMPLE_RATE,
            "channels": AUDIO_CHANNELS,
            "bitsPerSample": AUDIO_BITS_PER_SAMPLE,
        })
    }

    fn chunk(&mut self, payload: &Value) -> Option<Value> {
        let session_id = string_field(payload, "sessionId").to_string();
        let Some(current) = self.session.as_ref() else {
            return Some(relay_error(
                "chunk",
                "audio chunk without active session",
                &session_id,
            ));
        };
        if current.id != session_id {
            self.session = None;
            return Some(relay_error(
                "chunk",
                "audio sessionId mismatch",
                &session_id,
            ));
        }

        let Some(sequence) = u64_field(payload, "seq") else {
            self.session = None;
            return Some(relay_error("chunk", "missing audio sequence", &session_id));
        };
        if sequence != current.next_sequence {
            let expected = current.next_sequence;
            self.session = None;
            return Some(relay_error(
                "chunk",
                &format!("audio sequence mismatch: expected {expected}, got {sequence}"),
                &session_id,
            ));
        }

        let decoded = match STANDARD.decode(string_field(payload, "data")) {
            Ok(decoded) => decoded,
            Err(error) => {
                self.session = None;
                return Some(relay_error(
                    "chunk",
                    &format!("invalid audio base64: {error}"),
                    &session_id,
                ));
            }
        };
        if u64_field(payload, "bytes") != Some(decoded.len() as u64) {
            self.session = None;
            return Some(relay_error(
                "chunk",
                "audio byte count mismatch",
                &session_id,
            ));
        }
        let frame_checksum = fnv1a64_update(FNV1A64_OFFSET, &decoded);
        if string_field(payload, "checksum") != format!("{frame_checksum:016x}") {
            self.session = None;
            return Some(relay_error(
                "chunk",
                "audio frame checksum mismatch",
                &session_id,
            ));
        }
        if current.pcm.len().saturating_add(decoded.len()) > MAX_CAPTURE_BYTES {
            self.session = None;
            return Some(relay_error(
                "chunk",
                "device microphone recording exceeds 30 seconds",
                &session_id,
            ));
        }

        if self.forward_udp {
            let Some(socket) = self.socket.as_ref() else {
                self.session = None;
                return Some(relay_error(
                    "chunk",
                    "audio relay socket is closed",
                    &session_id,
                ));
            };
            if let Err(error) = socket.send_to(&decoded, ("127.0.0.1", self.target_port)) {
                self.session = None;
                return Some(relay_error(
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
            session_id: session_id.clone(),
            board_device_id: session.board_device_id.clone(),
            sequence,
            pcm: decoded,
        });

        if sequence % 25 == 0 {
            Some(json!({
                "phase": "streaming",
                "ok": true,
                "sessionId": session_id,
                "chunks": session.next_sequence,
                "bytes": session.total_bytes,
            }))
        } else {
            None
        }
    }

    fn end(&mut self, payload: &Value) -> Value {
        let session_id = string_field(payload, "sessionId").to_string();
        let Some(session) = self.session.take() else {
            return relay_error("end", "audio end without active session", &session_id);
        };
        self.socket = None;

        if session.id != session_id {
            return relay_error("end", "audio sessionId mismatch", &session_id);
        }
        if u64_field(payload, "chunks") != Some(session.next_sequence) {
            return relay_error("end", "audio chunk total mismatch", &session_id);
        }
        if u64_field(payload, "bytes") != Some(session.total_bytes) {
            return relay_error("end", "audio stream byte total mismatch", &session_id);
        }
        if string_field(payload, "checksum") != format!("{:016x}", session.checksum) {
            return relay_error("end", "audio stream checksum mismatch", &session_id);
        }

        self.completed = Some(CompletedUsbAudio {
            session_id: session.id.clone(),
            board_device_id: session.board_device_id.clone(),
            pcm: session.pcm,
        });

        json!({
            "phase": "end",
            "ok": true,
            "sessionId": session_id,
            "reason": string_field(payload, "reason"),
            "chunks": session.next_sequence,
            "bytes": session.total_bytes,
            "durationMs": payload.get("durationMs").cloned().unwrap_or(Value::Null),
            "checksum": format!("{:016x}", session.checksum),
        })
    }

    pub fn take_completed(&mut self) -> Option<CompletedUsbAudio> {
        self.completed.take()
    }

    pub fn take_validated_chunk(&mut self) -> Option<ValidatedUsbAudioChunk> {
        self.validated_chunk.take()
    }
}

fn string_field<'a>(payload: &'a Value, key: &str) -> &'a str {
    payload.get(key).and_then(Value::as_str).unwrap_or_default()
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
