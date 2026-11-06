//! OpenAI-compatible TTS client (e.g. GPUStack /v1/audio/speech).
//! Synthesizes text to 16 kHz mono 16-bit PCM and feeds it through the same
//! on_pcm callback pipeline as the built-in provider.
use crate::doubao_tts::DoubaoTtsConfig;

const TARGET_RATE: u32 = 16_000;

pub async fn synthesize<F>(cfg: &DoubaoTtsConfig, text: &str, mut on_pcm: F) -> Result<usize, String>
where
    F: FnMut(&[u8]),
{
    let base = cfg.tts_base_url.trim().trim_end_matches('/');
    if base.is_empty() {
        return Err("自定义语音合成端点未配置（需要 base URL）".into());
    }
    let url = format!("{}/v1/audio/speech", base);
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(60))
        .build()
        .map_err(|e| format!("无法创建语音合成客户端: {e}"))?;
    let body = serde_json::json!({
        "model": cfg.tts_model,
        "input": text,
        "voice": cfg.tts_voice,
        "response_format": "wav",
    });
    let resp = client
        .post(&url)
        .header("Authorization", format!("Bearer {}", cfg.api_key.trim()))
        .json(&body)
        .send()
        .await
        .map_err(|e| format!("语音合成请求失败: {e}"))?;
    let status = resp.status();
    let bytes = resp
        .bytes()
        .await
        .map_err(|e| format!("读取语音合成响应失败: {e}"))?;
    if !status.is_success() {
        let snippet = String::from_utf8_lossy(&bytes[..bytes.len().min(300)]).to_string();
        return Err(format!("语音合成服务返回错误 (HTTP {status}): {snippet}"));
    }
    let pcm = parse_wav_to_pcm16mono16k(&bytes).map_err(|e| format!("解析语音合成音频失败: {e}"))?;
    let mut emitted = 0usize;
    for chunk in pcm.chunks(8000) {
        on_pcm(chunk);
        emitted += chunk.len();
    }
    Ok(emitted)
}

/// Parse an RIFF/WAV byte stream into 16 kHz mono 16-bit little-endian PCM.
fn parse_wav_to_pcm16mono16k(bytes: &[u8]) -> Result<Vec<u8>, String> {
    if bytes.len() < 44 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return Err("响应不是 WAV 音频".into());
    }
    let mut channels: u16 = 1;
    let mut sample_rate: u32 = TARGET_RATE;
    let mut bits: u16 = 16;
    let mut offset = 12usize;
    let mut data: Option<&[u8]> = None;
    while offset + 8 <= bytes.len() {
        let id = &bytes[offset..offset + 4];
        let size = u32::from_le_bytes([bytes[offset + 4], bytes[offset + 5], bytes[offset + 6], bytes[offset + 7]]) as usize;
        let payload = offset + 8;
        if payload.saturating_add(size) > bytes.len() {
            break;
        }
        let chunk = &bytes[payload..payload.saturating_add(size)];
        if id == b"fmt " {
            if chunk.len() >= 16 {
                channels = u16::from_le_bytes([chunk[2], chunk[3]]);
                sample_rate = u32::from_le_bytes([chunk[4], chunk[5], chunk[6], chunk[7]]);
                bits = u16::from_le_bytes([chunk[14], chunk[15]]);
            }
        } else if id == b"data" {
            data = Some(chunk);
        }
        offset = payload.saturating_add(size).saturating_add(size % 2);
    }
    let data = data.ok_or("WAV 缺少 data 块")?;
    if bits != 16 {
        return Err(format!("不支持的位深: {bits}"));
    }
    if sample_rate == 0 {
        return Err("WAV 采样率异常".into());
    }
    // Mono-ize (average channels), then linear-resample to 16 kHz.
    let samples: Vec<i16> = if channels == 1 {
        data.chunks_exact(2).map(|b| i16::from_le_bytes([b[0], b[1]])).collect()
    } else {
        let ch = channels as usize;
        let frames = data.len() / (2 * ch);
        (0..frames)
            .map(|i| {
                let sum: i32 = (0..ch).map(|c| i16::from_le_bytes([data[(i * ch + c) * 2], data[(i * ch + c) * 2 + 1]]) as i32).sum();
                (sum / ch as i32) as i16
            })
            .collect()
    };
    if samples.is_empty() {
        return Err("音频内容为空".into());
    }
    let out = resample_linear(&samples, sample_rate, TARGET_RATE);
    let mut v = Vec::with_capacity(out.len() * 2);
    for s in out {
        v.extend_from_slice(&s.to_le_bytes());
    }
    Ok(v)
}

fn resample_linear(src: &[i16], from: u32, to: u32) -> Vec<i16> {
    if from == to {
        return src.to_vec();
    }
    let ratio = from as f64 / to as f64;
    let n = (src.len() as f64 / ratio) as usize;
    (0..n)
        .map(|i| {
            let pos = i as f64 * ratio;
            let lo = pos.floor() as usize;
            let hi = (lo + 1).min(src.len() - 1);
            let frac = (pos - lo as f64) as i32;
            let a = src[lo] as i32;
            let b = src[hi] as i32;
            (a + (b - a) * frac / 1024) as i16
        })
        .collect()
}
