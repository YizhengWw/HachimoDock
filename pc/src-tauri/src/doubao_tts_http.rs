//! HTTP unidirectional streaming for TTS 2.0 Context voices. Uses the same
//! trusted-CA/network client as other cloud calls; never logs keys or spoken text.
//! Protocol: https://docs.volcengine.com/docs/DoubaoVoice/unidirectional-streaming-text-to-speech-http
use super::{DoubaoTtsConfig, MAX_PCM_BYTES, SAMPLE_RATE, SESSION_TIMEOUT};
use base64::Engine;
use serde_json::{json, Value};

const URL: &str = "https://openspeech.bytedance.com/api/v3/tts/unidirectional";
const MAX_JSON_BYTES: usize = 4 * 1024 * 1024;

#[derive(Default)]
struct Decoder {
    pending: Vec<u8>,
    odd_sample_byte: Option<u8>,
    total: usize,
    finished: bool,
}

impl Decoder {
    fn push(&mut self, bytes: &[u8], on_pcm: &mut impl FnMut(&[u8])) -> Result<(), String> {
        if self.finished { return Ok(()); }
        // Process bounded windows even if the HTTP stack coalesces many JSON packets.
        for part in bytes.chunks(64 * 1024) {
            if self.finished { break; }
            self.pending.extend_from_slice(part);
            if self.pending.len() > MAX_JSON_BYTES { return Err("豆包 TTS 响应数据过大".into()); }
            loop {
                let mut stream = serde_json::Deserializer::from_slice(&self.pending).into_iter::<Value>();
                let packet = match stream.next() {
                    Some(Ok(packet)) => packet,
                    Some(Err(error)) if error.is_eof() => break,
                    Some(Err(_)) => return Err("豆包 TTS 返回了无效的流式数据".into()),
                    None => { self.pending.clear(); break; }
                };
                let consumed = stream.byte_offset();
                self.pending.drain(..consumed);
                self.packet(packet, on_pcm)?;
                if self.finished { self.pending.clear(); break; }
            }
        }
        Ok(())
    }

    fn packet(&mut self, packet: Value, on_pcm: &mut impl FnMut(&[u8])) -> Result<(), String> {
        let code = packet["code"].as_i64().ok_or("豆包 TTS 响应缺少状态码")?;
        if code == 20_000_000 {
            self.finished = true;
            return Ok(());
        }
        if code != 0 {
            // The provider message may echo input/credentials; only expose the code.
            return Err(format!("豆包 TTS 合成失败（{code}），请检查音色及语音服务权限"));
        }
        if let Some(data) = packet["data"].as_str().filter(|data| !data.is_empty()) {
            let mut pcm = base64::engine::general_purpose::STANDARD.decode(data)
                .map_err(|_| "豆包 TTS 音频编码无效")?;
            if let Some(byte) = self.odd_sample_byte.take() { pcm.insert(0, byte); }
            if self.total + pcm.len() > MAX_PCM_BYTES { return Err("豆包 TTS 音频超过长度限制".into()); }
            if pcm.len() % 2 != 0 { self.odd_sample_byte = pcm.pop(); }
            if !pcm.is_empty() { self.total += pcm.len(); on_pcm(&pcm); }
        }
        Ok(())
    }

    fn complete(self) -> Result<usize, String> {
        if !self.finished || self.odd_sample_byte.is_some() {
            return Err("豆包 TTS 音频传输未完整结束，请重试".into());
        }
        if self.total == 0 { return Err("豆包 TTS 未返回音频，请使用该音色对应语言的文本重试".into()); }
        Ok(self.total)
    }
}

pub(super) async fn synthesize(cfg: &DoubaoTtsConfig, text: &str, on_pcm: &mut impl FnMut(&[u8])) -> Result<usize, String> {
    if cfg.api_key.is_empty() { return Err("豆包 TTS 未配置 API Key".into()); }
    let mut audio = json!({ "format": "pcm", "sample_rate": SAMPLE_RATE });
    if let Some(rate) = cfg.speech_rate() { audio["speech_rate"] = json!(rate); }
    let mut response = crate::llm_network::client(SESSION_TIMEOUT)?
        .post(URL)
        .header("X-Api-Key", &cfg.api_key)
        .header("X-Api-Resource-Id", &cfg.resource_id)
        .header("X-Api-Request-Id", uuid::Uuid::new_v4().to_string())
        .json(&json!({ "user": { "uid": "pet-manager" }, "req_params": {
            "text": text, "speaker": cfg.speaker, "audio_params": audio
        }}))
        .send().await.map_err(|error| crate::llm_network::connection_error(&error))?;
    if !response.status().is_success() {
        return Err(format!("豆包 TTS 请求失败（HTTP {}）", response.status().as_u16()));
    }
    let mut decoder = Decoder::default();
    while let Some(chunk) = response.chunk().await.map_err(|error| crate::llm_network::connection_error(&error))? {
        decoder.push(&chunk, on_pcm)?;
        if decoder.finished { break; }
    }
    decoder.complete()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_fragmented_json_and_carries_split_samples() {
        let wire = b"{\"code\":0,\"data\":\"AQ==\"}\n{\"code\":0,\"data\":\"AgME\"}\n{\"code\":20000000}";
        for size in 1..=wire.len() {
            let mut decoder = Decoder::default();
            let mut pcm = Vec::new();
            for part in wire.chunks(size) { decoder.push(part, &mut |chunk| pcm.extend_from_slice(chunk)).unwrap(); }
            assert_eq!(decoder.complete().unwrap(), 4);
            assert_eq!(pcm, [1, 2, 3, 4]);
        }
    }

    #[test]
    fn rejects_errors_truncation_missing_audio_and_malformed_data() {
        for wire in [r#"{"code":55000000,"message":"secret"}"#, r#"{"code":0,"data":"???"}"#, r#"{"data":"AQI="}"#, "not json"] {
            let error = Decoder::default().push(wire.as_bytes(), &mut |_| {}).unwrap_err();
            assert!(!error.contains("secret"));
        }
        for wire in [r#"{"code":0,"data":"AQI="}"#, r#"{"code":20000000}"#, r#"{"code":0,"data":"AQ=="}{"code":20000000}"#] {
            let mut decoder = Decoder::default();
            decoder.push(wire.as_bytes(), &mut |_| {}).unwrap();
            assert!(decoder.complete().is_err());
        }
    }

    #[test]
    fn bounds_pending_json_and_audio() {
        let mut decoder = Decoder::default();
        let mut wire = vec![b'a'; MAX_JSON_BYTES + 1];
        wire[0] = b'"';
        assert!(decoder.push(&wire, &mut |_| {}).is_err());
        let mut decoder = Decoder { total: MAX_PCM_BYTES, ..Default::default() };
        assert!(decoder.push(br#"{"code":0,"data":"AQI="}"#, &mut |_| {}).is_err());
    }
}
