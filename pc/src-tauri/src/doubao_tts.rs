/*
 * [Input] Doubao Seed TTS 2.0 credentials (API key, resource id), a speaker id, optional speech-rate
 *         hint, and UTF-8 text sentences.
 * [Output] Streaming 16 kHz mono PCM through the Volcengine v3 bidirectional WebSocket protocol
 *          or HTTP unidirectional streaming for catalog-marked Context voices,
 *          (binary frames: 4-byte header, event, session id, payload), with connection reuse across
 *          sentences and a clean reset on provider errors.
 * [Pos] Tauri-side text-to-speech client for realtime persona chat and 人设与声音 preview.
 * [Sync] If the wire protocol or config surface changes, update `pc/docs/realtime-chat.md`.
 */

use futures_util::{SinkExt, StreamExt};
use serde_json::json;
use std::time::Duration;
use tokio::time::timeout;
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::tungstenite::http::HeaderValue;
use tokio_tungstenite::tungstenite::Message as WsMessage;
use uuid::Uuid;

#[path = "doubao_tts_http.rs"]
mod http_stream;

fn voice_catalog() -> &'static serde_json::Value {
    static CATALOG: std::sync::OnceLock<serde_json::Value> = std::sync::OnceLock::new();
    CATALOG.get_or_init(|| serde_json::from_str(include_str!("../../src/lib/tts-voices.json")).expect("bundled voice catalog"))
}

pub const DEFAULT_WS_URL: &str = "wss://openspeech.bytedance.com/api/v3/tts/bidirection";
pub const DEFAULT_RESOURCE_ID: &str = "seed-tts-2.0";
pub const VOICE_CLONE_RESOURCE_ID: &str = "seed-icl-2.0";
pub const DEFAULT_MODEL: &str = "seed-tts-2.0-standard";
pub const SAMPLE_RATE: u32 = 16_000;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
const SESSION_TIMEOUT: Duration = Duration::from_secs(60);
const MAX_PCM_BYTES: usize = 32 * 1024 * 1024;

// ---- protocol constants (mirrors the provider's Python reference) ----
const MSG_FULL_CLIENT_REQUEST: u8 = 0b0001;
const MSG_FULL_SERVER_RESPONSE: u8 = 0b1001;
const MSG_AUDIO_ONLY_SERVER: u8 = 0b1011;
const MSG_ERROR: u8 = 0b1111;
const FLAG_WITH_EVENT: u8 = 0b0100;
const FLAG_POSITIVE_SEQ: u8 = 0b0001;
const FLAG_NEGATIVE_SEQ: u8 = 0b0011;

const EVENT_START_CONNECTION: i32 = 1;
const EVENT_FINISH_CONNECTION: i32 = 2;
const EVENT_CONNECTION_STARTED: i32 = 50;
const EVENT_CONNECTION_FAILED: i32 = 51;
const EVENT_CONNECTION_FINISHED: i32 = 52;
const EVENT_START_SESSION: i32 = 100;
const EVENT_FINISH_SESSION: i32 = 102;
const EVENT_SESSION_STARTED: i32 = 150;
const EVENT_SESSION_FINISHED: i32 = 152;
const EVENT_SESSION_FAILED: i32 = 153;
const EVENT_TASK_REQUEST: i32 = 200;
const EVENT_TTS_RESPONSE: i32 = 352;

#[derive(Debug, Clone)]
pub struct DoubaoTtsConfig {
    pub api_key: String,
    pub resource_id: String,
    pub model: String,
    pub speaker: String,
    /// 1.0 = normal; mapped onto the provider's speech_rate (-50..100) when not 1.0.
    pub speed: f32,
    pub ws_url: String,
}

impl DoubaoTtsConfig {
    pub fn new(api_key: &str, speaker: &str, clone_speaker_id: &str, speed: f32, resource_id: &str) -> Self {
        let cloned = !clone_speaker_id.trim().is_empty();
        let catalog = voice_catalog();
        let legacy = catalog["legacySpeakerIds"].as_array().unwrap().iter().any(|v| v.as_str() == Some(speaker.trim()));
        let speaker = if legacy { catalog["defaultSpeaker"].as_str().unwrap() } else { speaker.trim() };
        Self {
            api_key: api_key.trim().to_string(),
            resource_id: if cloned {
                VOICE_CLONE_RESOURCE_ID.to_string()
            } else if legacy || ["speakers", "compatibilitySpeakers"].iter().any(|key| catalog[key].as_array().unwrap().iter().any(|v| v["id"].as_str() == Some(speaker))) || resource_id.trim().is_empty() {
                DEFAULT_RESOURCE_ID.to_string()
            } else {
                resource_id.trim().to_string()
            },
            model: DEFAULT_MODEL.to_string(),
            speaker: if cloned { clone_speaker_id.trim().to_string() } else { speaker.trim().to_string() },
            speed: if speed.is_finite() && speed > 0.0 { speed } else { 1.0 },
            ws_url: DEFAULT_WS_URL.to_string(),
        }
    }

    fn speech_rate(&self) -> Option<i32> {
        let rate = ((self.speed - 1.0) * 100.0).round() as i32;
        if rate == 0 {
            None
        } else {
            Some(rate.clamp(-50, 100))
        }
    }

    fn uses_http_stream(&self) -> bool {
        voice_catalog()["speakers"].as_array().unwrap().iter().any(|voice|
            voice["id"].as_str() == Some(self.speaker.as_str()) && voice["transport"] == "http")
    }
}

#[derive(Debug, Default)]
struct Frame {
    msg_type: u8,
    flag: u8,
    event: i32,
    session_id: String,
    error_code: u32,
    payload: Vec<u8>,
}

fn encode_client_event(event: i32, session_id: &str, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(16 + session_id.len() + payload.len());
    out.push((1 << 4) | 1); // version 1, header size 4 bytes
    out.push((MSG_FULL_CLIENT_REQUEST << 4) | FLAG_WITH_EVENT);
    out.push((1 << 4) | 0); // JSON serialization, no compression
    out.push(0);
    out.extend_from_slice(&event.to_be_bytes());
    if !matches!(event, EVENT_START_CONNECTION | EVENT_FINISH_CONNECTION) {
        out.extend_from_slice(&(session_id.len() as u32).to_be_bytes());
        out.extend_from_slice(session_id.as_bytes());
    }
    out.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    out.extend_from_slice(payload);
    out
}

fn read_u32(data: &[u8], pos: &mut usize) -> Result<u32, String> {
    if *pos + 4 > data.len() {
        return Err("frame truncated".to_string());
    }
    let value = u32::from_be_bytes([data[*pos], data[*pos + 1], data[*pos + 2], data[*pos + 3]]);
    *pos += 4;
    Ok(value)
}

fn decode_frame(data: &[u8]) -> Result<Frame, String> {
    if data.len() < 4 {
        return Err(format!("frame too short: {} bytes", data.len()));
    }
    let header_size = 4 * (data[0] & 0x0f) as usize;
    let msg_type = data[1] >> 4;
    let flag = data[1] & 0x0f;
    let mut pos = header_size.max(4);
    let mut frame = Frame { msg_type, flag, ..Frame::default() };
    match msg_type {
        MSG_FULL_SERVER_RESPONSE | MSG_AUDIO_ONLY_SERVER | 0b1100 | MSG_FULL_CLIENT_REQUEST | 0b0010 => {
            if flag == FLAG_POSITIVE_SEQ || flag == FLAG_NEGATIVE_SEQ {
                read_u32(data, &mut pos)?; // sequence
            }
        }
        MSG_ERROR => {
            frame.error_code = read_u32(data, &mut pos)?;
        }
        other => return Err(format!("unsupported message type {other}")),
    }
    if flag == FLAG_WITH_EVENT {
        frame.event = read_u32(data, &mut pos)? as i32;
        if !matches!(
            frame.event,
            EVENT_START_CONNECTION
                | EVENT_FINISH_CONNECTION
                | EVENT_CONNECTION_STARTED
                | EVENT_CONNECTION_FAILED
                | EVENT_CONNECTION_FINISHED
        ) {
            let size = read_u32(data, &mut pos)? as usize;
            if size > 0 {
                if pos + size > data.len() {
                    return Err("session id truncated".to_string());
                }
                frame.session_id = String::from_utf8_lossy(&data[pos..pos + size]).to_string();
                pos += size;
            }
        }
        if matches!(frame.event, EVENT_CONNECTION_STARTED | EVENT_CONNECTION_FAILED | EVENT_CONNECTION_FINISHED) {
            let size = read_u32(data, &mut pos)? as usize;
            pos += size.min(data.len().saturating_sub(pos)); // connect id, ignored
        }
    }
    if pos < data.len() {
        let size = read_u32(data, &mut pos)? as usize;
        if pos + size > data.len() {
            return Err("payload truncated".to_string());
        }
        frame.payload = data[pos..pos + size].to_vec();
    }
    Ok(frame)
}

fn redact(text: &str, secret: &str) -> String {
    let mut out = text.replace('\n', " ");
    if !secret.is_empty() {
        out = out.replace(secret, "<redacted>");
    }
    out.chars().take(400).collect()
}

type Socket = crate::llm_network::CloudSocket;

/// One provider connection, reused across sentences of the same voice.
pub struct DoubaoTtsClient {
    cfg: DoubaoTtsConfig,
    socket: Option<Socket>,
    /// Set after a SessionFailed that mentioned speech_rate: retry without the hint.
    rate_hint_rejected: bool,
}

impl DoubaoTtsClient {
    pub fn new(cfg: DoubaoTtsConfig) -> Self {
        Self { cfg, socket: None, rate_hint_rejected: false }
    }

    pub fn config(&self) -> &DoubaoTtsConfig {
        &self.cfg
    }

    async fn ensure_ready(&mut self) -> Result<(), String> {
        if self.socket.is_some() {
            return Ok(());
        }
        if self.cfg.api_key.is_empty() {
            return Err("豆包 TTS 未配置 API Key".to_string());
        }
        if self.cfg.speaker.is_empty() {
            return Err("未选择音色".to_string());
        }
        let mut request = self
            .cfg
            .ws_url
            .as_str()
            .into_client_request()
            .map_err(|error| format!("TTS 地址无效: {error}"))?;
        let headers = request.headers_mut();
        headers.insert("X-Api-Key", HeaderValue::from_str(&self.cfg.api_key).map_err(|e| e.to_string())?);
        headers.insert("X-Api-Resource-Id", HeaderValue::from_str(&self.cfg.resource_id).map_err(|e| e.to_string())?);
        headers.insert(
            "X-Api-Connect-Id",
            HeaderValue::from_str(&Uuid::new_v4().simple().to_string()).map_err(|e| e.to_string())?,
        );
        let (mut socket, _) = timeout(CONNECT_TIMEOUT, crate::llm_network::connect_websocket(request))
            .await
            .map_err(|_| "豆包 TTS 连接超时".to_string())?
            .map_err(|error| {
                let text = redact(&error.to_string(), &self.cfg.api_key);
                if text.contains("401") {
                    format!("豆包 TTS 鉴权失败（请核对 TTS API Key）: {text}")
                } else if text.contains("403") {
                    format!("豆包 TTS 资源未授权（请在火山控制台开通 {}）: {text}", self.cfg.resource_id)
                } else {
                    format!("豆包 TTS 连接失败: {text}")
                }
            })?;
        socket
            .send(WsMessage::Binary(encode_client_event(EVENT_START_CONNECTION, "", b"{}").into()))
            .await
            .map_err(|error| format!("豆包 TTS StartConnection 发送失败: {error}"))?;
        let started = timeout(CONNECT_TIMEOUT, Self::wait_for_event(&mut socket, EVENT_CONNECTION_STARTED))
            .await
            .map_err(|_| "豆包 TTS 等待 ConnectionStarted 超时".to_string())?;
        if let Err(error) = started {
            let _ = socket.close(None).await;
            return Err(error);
        }
        self.socket = Some(socket);
        Ok(())
    }

    async fn wait_for_event(socket: &mut Socket, event: i32) -> Result<Frame, String> {
        loop {
            let frame = Self::receive(socket).await?;
            if frame.msg_type == MSG_ERROR {
                return Err(format!(
                    "豆包 TTS 协议错误 ({}): {}",
                    frame.error_code,
                    String::from_utf8_lossy(&frame.payload)
                ));
            }
            if frame.event == EVENT_CONNECTION_FAILED {
                return Err(format!("豆包 TTS ConnectionFailed: {}", String::from_utf8_lossy(&frame.payload)));
            }
            if frame.msg_type == MSG_FULL_SERVER_RESPONSE && frame.event == event {
                return Ok(frame);
            }
        }
    }

    async fn receive(socket: &mut Socket) -> Result<Frame, String> {
        loop {
            match socket.next().await {
                Some(Ok(WsMessage::Binary(data))) => return decode_frame(&data),
                Some(Ok(WsMessage::Ping(_))) | Some(Ok(WsMessage::Pong(_))) => continue,
                Some(Ok(WsMessage::Text(text))) => return Err(format!("豆包 TTS 返回了意外的文本帧: {text}")),
                Some(Ok(WsMessage::Close(frame))) => {
                    return Err(format!("豆包 TTS 连接被关闭: {:?}", frame.map(|f| f.reason.to_string())))
                }
                Some(Ok(_)) => continue,
                Some(Err(error)) => return Err(format!("豆包 TTS 接收失败: {error}")),
                None => return Err("豆包 TTS 连接已结束".to_string()),
            }
        }
    }

    /// Barge-in must not wait for the provider's close handshake. Dropping the
    /// stream fences all old response frames; the next sentence reconnects.
    pub fn interrupt(&mut self) { self.socket.take(); }

    pub async fn reset(&mut self) {
        if let Some(mut socket) = self.socket.take() {
            let _ = timeout(
                Duration::from_secs(2),
                socket.send(WsMessage::Binary(encode_client_event(EVENT_FINISH_CONNECTION, "", b"{}").into())),
            )
            .await;
            let _ = timeout(Duration::from_secs(2), socket.close(None)).await;
        }
    }

    fn start_session_payload(&self, with_rate_hint: bool) -> Vec<u8> {
        let mut audio_params = json!({ "format": "pcm", "sample_rate": SAMPLE_RATE });
        if with_rate_hint {
            if let Some(rate) = self.cfg.speech_rate() {
                audio_params["speech_rate"] = json!(rate);
            }
        }
        let body = json!({
            "user": { "uid": Uuid::new_v4().simple().to_string() },
            "event": EVENT_START_SESSION,
            "namespace": "BidirectionalTTS",
            "req_params": {
                "speaker": self.cfg.speaker,
                "model": self.cfg.model,
                "audio_params": audio_params,
            },
        });
        serde_json::to_vec(&body).unwrap_or_default()
    }

    /// Synthesize one sentence, streaming PCM chunks to `on_pcm` as they arrive.
    /// Returns the total number of PCM bytes emitted.
    pub async fn synthesize<F>(&mut self, text: &str, mut on_pcm: F) -> Result<usize, String>
    where
        F: FnMut(&[u8]),
    {
        let clean = text.trim();
        if clean.is_empty() {
            return Ok(0);
        }
        if self.cfg.uses_http_stream() {
            // Context voices explicitly reject the bidirectional protocol. Never retry
            // after audio has been emitted: that would repeat speech on the device.
            return timeout(SESSION_TIMEOUT, http_stream::synthesize(&self.cfg, clean, &mut on_pcm))
                .await.map_err(|_| "豆包 TTS 会话超时".to_string())?;
        }
        let mut last_error = String::new();
        for _attempt in 0..3 {
            if let Err(error) = self.ensure_ready().await {
                last_error = error;
                continue;
            }
            let with_rate_hint = !self.rate_hint_rejected;
            match timeout(SESSION_TIMEOUT, self.synthesize_once(clean, with_rate_hint, &mut on_pcm)).await {
                Ok(Ok(total)) => return Ok(total),
                Ok(Err(error)) => {
                    let lowered = error.to_lowercase();
                    if with_rate_hint && lowered.contains("speech_rate") {
                        self.rate_hint_rejected = true;
                    }
                    crate::realtime_chat_log::record("", "tts_failed", json!({"errorKind":crate::realtime_chat_log::error_kind(&error)}));
                    last_error = error;
                    self.reset().await;
                    // Configuration/permission errors cannot be fixed by repeating the same request.
                    if last_error.contains("mismatched") {
                        return Err("音色与语音资源不匹配，请在人设与声音中选择 TTS 2.0 音色（55000000）".into());
                    }
                }
                Err(_) => {
                    last_error = "豆包 TTS 会话超时".to_string();
                    self.reset().await;
                }
            }
        }
        Err(last_error)
    }

    async fn synthesize_once<F>(&mut self, text: &str, with_rate_hint: bool, on_pcm: &mut F) -> Result<usize, String>
    where
        F: FnMut(&[u8]),
    {
        let payload = self.start_session_payload(with_rate_hint);
        let api_key = self.cfg.api_key.clone();
        let socket = self.socket.as_mut().ok_or_else(|| "豆包 TTS 未连接".to_string())?;
        let session_id = Uuid::new_v4().simple().to_string();
        socket
            .send(WsMessage::Binary(encode_client_event(EVENT_START_SESSION, &session_id, &payload).into()))
            .await
            .map_err(|error| format!("StartSession 发送失败: {error}"))?;
        let started = Self::receive(socket).await?;
        if started.event == EVENT_SESSION_FAILED {
            return Err(format!("SessionFailed: {}", redact(&String::from_utf8_lossy(&started.payload), &api_key)));
        }
        if started.event != EVENT_SESSION_STARTED {
            return Err(format!("期望 SessionStarted，收到 event={}", started.event));
        }
        let task = serde_json::to_vec(&json!({ "event": EVENT_TASK_REQUEST, "req_params": { "text": text } }))
            .unwrap_or_default();
        socket
            .send(WsMessage::Binary(encode_client_event(EVENT_TASK_REQUEST, &session_id, &task).into()))
            .await
            .map_err(|error| format!("TaskRequest 发送失败: {error}"))?;
        socket
            .send(WsMessage::Binary(encode_client_event(EVENT_FINISH_SESSION, &session_id, b"{}").into()))
            .await
            .map_err(|error| format!("FinishSession 发送失败: {error}"))?;
        let mut total = 0usize;
        loop {
            let frame = Self::receive(socket).await?;
            if frame.msg_type == MSG_ERROR {
                return Err(format!(
                    "协议错误 ({}): {}",
                    frame.error_code,
                    redact(&String::from_utf8_lossy(&frame.payload), &api_key)
                ));
            }
            if frame.msg_type == MSG_AUDIO_ONLY_SERVER && frame.event == EVENT_TTS_RESPONSE {
                if !frame.payload.is_empty() {
                    total += frame.payload.len();
                    if total > MAX_PCM_BYTES {
                        return Err("豆包 TTS 音频超过安全上限".to_string());
                    }
                    on_pcm(&frame.payload);
                }
                continue;
            }
            if frame.event == EVENT_SESSION_FINISHED {
                break;
            }
            if frame.event == EVENT_SESSION_FAILED {
                return Err(format!("SessionFailed: {}", redact(&String::from_utf8_lossy(&frame.payload), &api_key)));
            }
        }
        if total == 0 {
            return Err("豆包 TTS 会话结束但未返回音频".to_string());
        }
        Ok(total)
    }
}

/// Scale 16-bit PCM in place (used for the per-appearance volume hint).
pub fn scale_pcm16(pcm: &mut [u8], gain: f32) {
    if !(gain.is_finite()) || (gain - 1.0).abs() < 0.001 {
        return;
    }
    for sample in pcm.chunks_exact_mut(2) {
        let value = i16::from_le_bytes([sample[0], sample[1]]) as f32 * gain;
        let clamped = value.clamp(i16::MIN as f32, i16::MAX as f32) as i16;
        sample.copy_from_slice(&clamped.to_le_bytes());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn entire_catalog_preserves_speakers_and_routes_context_voices() {
        let voices = voice_catalog()["speakers"].as_array().unwrap();
        assert_eq!(voices.len(), 445);
        let mut http_count = 0;
        for voice in voices {
            let speaker = voice["id"].as_str().unwrap();
            let cfg = DoubaoTtsConfig::new("k", speaker, "", 1.0, "seed-tts-1.0");
            assert_eq!(cfg.speaker, speaker);
            assert_eq!(cfg.resource_id, DEFAULT_RESOURCE_ID);
            assert_eq!(cfg.uses_http_stream(), voice["transport"] == "http");
            http_count += usize::from(cfg.uses_http_stream());
            let clone = DoubaoTtsConfig::new("k", speaker, "S_abc", 1.0, "");
            assert!(!clone.uses_http_stream());
            assert_eq!(clone.resource_id, VOICE_CLONE_RESOURCE_ID);
        }
        assert_eq!(http_count, 15);
    }

    /// Explicit opt-in only: reads local credentials without printing or embedding them.
    #[test]
    #[cfg(target_os = "macos")]
    #[ignore = "requires PET_REALTIME_SETTINGS_DIR and uses the configured cloud service"]
    fn live_seed_tts_2_catalog() {
        let dir = std::path::PathBuf::from(std::env::var("PET_REALTIME_SETTINGS_DIR").expect("settings directory"));
        crate::voice_chat_settings::configure_storage_dir(dir.clone()).unwrap();
        crate::volcengine_asr::configure_storage_dir(dir).unwrap();
        tauri::async_runtime::block_on(async {
            for speaker in ["zh_female_vv_uranus_bigtts", "zh_male_shaonianzixin_uranus_bigtts", "zh_male_ruyayichen_uranus_bigtts", "ICL_uranus_en_female_charlie_tob", "en_male_bill_jones_corey_uranus_bigtts"] {
                if let Ok(filter) = std::env::var("PET_TTS_TEST_SPEAKER") {
                    if speaker != filter { continue; }
                }
                let cfg = crate::voice_chat_settings::tts_config(speaker, "", 1.0).unwrap();
                let mut client = DoubaoTtsClient::new(cfg);
                let mut bytes = 0;
                let text = if speaker.contains("en_") { "Hello, it is nice to meet you." } else { "你好，我们来聊聊天吧。" };
                let result = client.synthesize(text, |pcm| bytes += pcm.len()).await;
                client.reset().await;
                assert!(result.is_ok(), "speaker={speaker}: {result:?}");
                assert!(bytes > 6400, "speaker={speaker}: no playable audio");
                println!("speaker={speaker} PCM bytes={bytes}");
            }
        });
    }

    #[test]
    fn client_event_frames_round_trip_through_the_decoder() {
        let frame = encode_client_event(EVENT_START_SESSION, "abc", b"{\"x\":1}");
        assert_eq!(frame[0], 0x11);
        assert_eq!(frame[1], (MSG_FULL_CLIENT_REQUEST << 4) | FLAG_WITH_EVENT);
        let decoded = decode_frame(&frame).unwrap();
        assert_eq!(decoded.event, EVENT_START_SESSION);
        assert_eq!(decoded.session_id, "abc");
        assert_eq!(decoded.payload, b"{\"x\":1}");
        let conn = encode_client_event(EVENT_START_CONNECTION, "ignored", b"{}");
        let decoded = decode_frame(&conn).unwrap();
        assert_eq!(decoded.event, EVENT_START_CONNECTION);
        assert_eq!(decoded.session_id, "");
    }

    #[test]
    fn audio_server_frames_carry_raw_pcm() {
        let mut data = vec![0x11, (MSG_AUDIO_ONLY_SERVER << 4) | FLAG_WITH_EVENT, 0x10, 0x00];
        data.extend_from_slice(&EVENT_TTS_RESPONSE.to_be_bytes());
        data.extend_from_slice(&2u32.to_be_bytes());
        data.extend_from_slice(b"id");
        data.extend_from_slice(&4u32.to_be_bytes());
        data.extend_from_slice(&[1, 2, 3, 4]);
        let frame = decode_frame(&data).unwrap();
        assert_eq!(frame.msg_type, MSG_AUDIO_ONLY_SERVER);
        assert_eq!(frame.event, EVENT_TTS_RESPONSE);
        assert_eq!(frame.payload, vec![1, 2, 3, 4]);
    }

    #[test]
    fn speed_maps_onto_provider_speech_rate_and_clone_ids_switch_resource() {
        let cfg = DoubaoTtsConfig::new("k", "spk", "", 1.25, "");
        assert_eq!(cfg.speech_rate(), Some(25));
        assert_eq!(cfg.resource_id, DEFAULT_RESOURCE_ID);
        let cloned = DoubaoTtsConfig::new("k", "spk", "S_abc", 0.5, "");
        assert_eq!(cloned.speaker, "S_abc");
        assert_eq!(cloned.resource_id, VOICE_CLONE_RESOURCE_ID);
        assert_eq!(cloned.speech_rate(), Some(-50));
        assert_eq!(DoubaoTtsConfig::new("k", "spk", "", 1.0, "").speech_rate(), None);
    }

    #[test]
    fn pcm_scaling_clamps() {
        let mut pcm = 30000i16.to_le_bytes().to_vec();
        scale_pcm16(&mut pcm, 2.0);
        assert_eq!(i16::from_le_bytes([pcm[0], pcm[1]]), i16::MAX);
    }

    #[test]
    fn legacy_default_migrates_and_utf8_error_redaction_is_safe() {
        let cfg = DoubaoTtsConfig::new("k", "zh_male_naiqimengwa_mars_bigtts", "", 1.0, "seed-tts-1.0");
        assert_eq!(cfg.speaker, "zh_female_vv_uranus_bigtts");
        assert_eq!(cfg.resource_id, "seed-tts-2.0");
        assert_eq!(redact(&"中文".repeat(500), "").chars().count(), 400);
    }
}
