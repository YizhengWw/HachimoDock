//! Bounded, metadata-only realtime diagnostics. Never persist keys, prompts, transcripts or PCM.
use serde_json::{json, Value};
use std::{fs::{self, OpenOptions}, io::Write, path::{Path, PathBuf}, sync::{Mutex, OnceLock}, time::{SystemTime, UNIX_EPOCH}};

static LOG: OnceLock<Mutex<PathBuf>> = OnceLock::new();
const MAX_BYTES: u64 = 1024 * 1024;

pub fn configure(dir: &Path) -> Result<(), String> {
    let dir = dir.join("logs");
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let _ = LOG.set(Mutex::new(dir.join("realtime-chat.jsonl")));
    record("", "logger_ready", json!({"version":env!("CARGO_PKG_VERSION")}));
    Ok(())
}

pub fn error_kind(error: &str) -> &'static str {
    let text = error.to_lowercase();
    if text.contains("mismatch") || text.contains("55000000") { "tts_resource_mismatch" }
    else if text.contains("certificate") || text.contains("证书") { "tls_certificate" }
    else if text.contains("401") || text.contains("鉴权") { "authentication" }
    else if text.contains("403") || text.contains("授权") { "resource_permission" }
    else if text.contains("404") { "endpoint_or_model" }
    else if text.contains("timeout") || text.contains("超时") { "timeout" }
    else if text.contains("配置") { "configuration" }
    else { "service_or_transport" }
}

fn metadata(details: Value) -> Value {
    let mut out = serde_json::Map::new();
    // Call sites supply these diagnostic fields only. Unknown strings (including provider bodies)
    // cannot accidentally become persistent logs.
    for key in ["bytes", "frames", "turn", "chars", "elapsedMs", "bufferedMs", "uptimeMs", "seq", "ok", "count", "steps", "hintFrames", "rejectedFrames", "peakRms", "replyFrames", "argIndex", "expectedArgs", "actualArgs"] {
        if let Some(v) = details.get(key).filter(|v| v.is_number() || v.is_boolean()) { out.insert(key.into(), v.clone()); }
    }
    for key in ["state", "reason", "errorKind", "stage"] {
        if let Some(v) = details.get(key).and_then(Value::as_str) {
            if v.len() <= 64 && v.bytes().all(|b| b.is_ascii_lowercase() || b == b'_') { out.insert(key.into(), json!(v)); }
        }
    }
    for key in ["version", "firmware"] {
        if let Some(v) = details.get(key).and_then(Value::as_str) {
            if v.len() <= 48 && v.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-') { out.insert(key.into(), json!(v)); }
        }
    }
    Value::Object(out)
}

pub fn record(session: &str, event: &str, details: Value) {
    let Some(lock) = LOG.get() else { return };
    let Ok(path) = lock.lock() else { return };
    let _ = append(&path, session, event, details);
}

fn append(path: &Path, session: &str, event: &str, details: Value) -> std::io::Result<()> {
    if fs::metadata(path).map(|m| m.len() >= MAX_BYTES).unwrap_or(false) {
        let previous = path.with_file_name("realtime-chat.previous.jsonl");
        if previous.exists() { fs::remove_file(&previous)?; }
        fs::rename(path, previous)?;
    }
    let mut options = OpenOptions::new();
    options.create(true).append(true);
    #[cfg(unix)] { use std::os::unix::fs::OpenOptionsExt; options.mode(0o600); }
    let mut file = options.open(path)?;
    let safe_session: String = session.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-').take(80).collect();
    let entry = json!({"timestampMs": SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis(),
        "sessionId": safe_session, "event": event, "details": metadata(details)});
    writeln!(file, "{entry}")
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn argument_diagnostics_never_include_values_or_device_ids() {
        assert_eq!(metadata(json!({"argIndex":1,"expectedArgs":2,"actualArgs":1,
            "errorKind":"argument_type","did":"PRIVATE_DEVICE","args":["PRIVATE_WORDS",false],
            "error":"PRIVATE_PROVIDER_RESPONSE"})),
            json!({"argIndex":1,"expectedArgs":2,"actualArgs":1,"errorKind":"argument_type"}));
    }
    #[test]
    fn vad_diagnostics_allow_counts_but_never_audio_or_transcripts() {
        let safe = metadata(json!({"frames":250,"hintFrames":20,"rejectedFrames":8,"replyFrames":120,
            "peakRms":160,"pcm":"private audio","transcript":"private words","peakRmsText":"secret"}));
        assert_eq!(safe, json!({"frames":250,"hintFrames":20,"rejectedFrames":8,"replyFrames":120,"peakRms":160}));
    }
    #[test]
    fn logs_are_bounded_private_metadata_only() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("realtime-chat.jsonl");
        append(&path, "rtc-1", "ended", json!({"bytes":640,"apiKey":"SECRET", "text":"PRIVATE", "errorKind":"tts_resource_mismatch"})).unwrap();
        let raw = fs::read_to_string(&path).unwrap();
        assert!(!raw.contains("SECRET") && !raw.contains("PRIVATE"));
        assert!(raw.contains("tts_resource_mismatch"));
        #[cfg(unix)] { use std::os::unix::fs::PermissionsExt; assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600); }
        OpenOptions::new().write(true).open(&path).unwrap().set_len(MAX_BYTES).unwrap();
        append(&path, "rtc-2", "started", json!({})).unwrap();
        assert!(path.with_file_name("realtime-chat.previous.jsonl").exists());
        assert!(fs::metadata(path).unwrap().len() < 1024);
    }
}
