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
    if text.starts_with("live serial write failed:") {
        for kind in ["write_progress_unknown","timeout","would_block","interrupted","broken_pipe","not_connected","write_zero","permission_denied","not_found","io_error"] {
            if text.contains(kind) { return kind; }
        }
        return "io_error";
    }
    if text.contains("mismatch") || text.contains("55000000") { "tts_resource_mismatch" }
    else if text.contains("certificate") || text.contains("证书") { "tls_certificate" }
    else if text.contains("401") || text.contains("鉴权") { "authentication" }
    else if text.contains("403") || text.contains("授权") { "resource_permission" }
    else if text.contains("404") { "endpoint_or_model" }
    else if text.contains("持续繁忙") { "device_transfer_busy" }
    else if text.contains("timeout") || text.contains("超时") { "timeout" }
    else if text.contains("配置") { "configuration" }
    else if text.contains("sessionfailed") { "tts_session_rejected" }
    else if text.contains("协议错误") || text.contains("sessionstarted") { "tts_protocol" }
    else { "service_or_transport" }
}

fn metadata(details: Value) -> Value {
    let mut out = serde_json::Map::new();
    // Only the applied numeric gain, never appearance names or voice IDs.
    if let Some(gain) = details.get("volume").and_then(Value::as_f64).filter(|v| v.is_finite() && *v >= 0.0 && *v <= 2.0) {
        out.insert("volume".into(), json!(gain));
    }
    // Call sites supply these diagnostic fields only. Unknown strings (including provider bodies)
    // cannot accidentally become persistent logs.
    for key in ["hits","prepared"]{
        if let Some(v)=details.get(key).and_then(Value::as_u64).filter(|v|*v<=10000){out.insert(key.into(),json!(v));}
    }
    for key in ["bytes", "writtenBytes", "retries", "osError", "frames", "turn", "chars", "elapsedMs", "bufferedMs", "positionMs", "generation", "uptimeMs", "seq", "ok", "count", "steps", "hintFrames", "rejectedFrames", "peakRms", "replyFrames", "argIndex", "expectedArgs", "actualArgs", "dspFrames", "dspLastUs", "dspMaxUs", "dspAecUs", "dspNsUs", "dspVadUs", "aecReferenceSamples", "internalFreeBytes", "internalLargestBytes", "captureTxLastUs", "captureTxMaxUs", "captureStackMinBytes"] {
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
    // Exact enums only: neither raw tool arguments nor arbitrary operation names
    // may leak task titles, dates, identifiers or model-supplied text.
    for (key, allowed) in [
        ("service", &["books","knowledge","news","other"][..]),
        ("taskIdKind", &["absent", "null", "empty", "uuid", "malformed", "wrong_type"][..]),
        ("tool", &["todo_manage", "computer_status", "media_player", "desktop_agent"][..]),
        ("operation", &["list", "add", "update", "complete", "reopen", "delete", "search", "status", "play", "enqueue", "next", "previous", "pause", "resume", "toggle", "stop", "seek", "volume", "mode", "move", "remove", "dispatch", "result", "cancel"][..]),
    ] {
        if let Some(v) = details.get(key).and_then(Value::as_str).filter(|v| allowed.contains(v)) {
            out.insert(key.into(), json!(v));
        }
    }
    if let Some(v)=details["requestId"].as_str().filter(|v|!v.is_empty()&&v.len()<=128&&v.bytes().all(|b|b.is_ascii_alphanumeric()||b==b'-'||b==b'_')){
        out.insert("requestId".into(),json!(v));
    }
    for key in ["trackRef","requestedTrackRef"] {
        if let Some(v)=details[key].as_str().filter(|v|v.len()==16 && v.bytes().all(|b|b.is_ascii_hexdigit())) {
            out.insert(key.into(),json!(v));
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
    #[test]fn news_cache_diagnostics_keep_only_bounded_counts(){
        assert_eq!(metadata(json!({"hits":4,"prepared":16,"componentId":"private","text":"private","key":"private"})),json!({"hits":4,"prepared":16}));
        assert_eq!(metadata(json!({"hits":"private","prepared":10001})),json!({}));
    }
    #[test]
    fn task_and_asr_diagnostics_never_keep_ids_or_payloads() {
        assert_eq!(metadata(json!({"taskIdKind":"malformed","taskId":"private-task", "arguments":"secret", "transcript":"private words", "pcm":"private audio", "reason":"unexpected_task_id", "stage":"final_wait", "elapsedMs":8000,"errorKind":"timeout"})),
            json!({"taskIdKind":"malformed","reason":"unexpected_task_id","stage":"final_wait","elapsedMs":8000,"errorKind":"timeout"}));
        assert!(metadata(json!({"taskIdKind":"private-value"})).as_object().unwrap().is_empty());
    }
    #[test]
    fn serial_recovery_metadata_excludes_payload_and_driver_strings() {
        assert_eq!(metadata(json!({"bytes":4400,"writtenBytes":64,"retries":2,"osError":5,
            "errorKind":"timeout","port":"PRIVATE","data":"PCM","error":"driver path", "session":"PRIVATE"})),
            json!({"bytes":4400,"writtenBytes":64,"retries":2,"osError":5,"errorKind":"timeout"}));
        assert_eq!(error_kind("Live serial write failed: write_progress_unknown after 64 bytes (0 retries)"),"write_progress_unknown");
        assert_eq!(error_kind("Live serial write failed: timeout after 64 bytes (3 retries)"),"timeout");
    }
    #[test]
    fn volume_diagnostics_are_numeric_and_never_include_persona_data() {
        assert_eq!(metadata(json!({"volume":0.3,"appearance":"private", "speaker":"private"})),json!({"volume":0.3}));
        for volume in [json!("private"),json!(-1),json!(200)] {
            assert_eq!(metadata(json!({"volume":volume})),json!({}));
        }
    }
    #[test]
    fn local_tool_diagnostics_include_only_fixed_operation_enums() {
        assert_eq!(metadata(json!({"tool":"todo_manage","operation":"add","count":1,"ok":true,"title":"PRIVATE","dueAt":"PRIVATE","id":"PRIVATE"})),
            json!({"tool":"todo_manage","operation":"add","count":1,"ok":true}));
        assert_eq!(metadata(json!({"tool":"private_title","operation":"private_title"})),json!({}));
        assert_eq!(metadata(json!({"tool":"media_player","operation":"play","generation":3,
            "trackRef":"aabbccddeeff0011","requestedTrackRef":"netease:123","key":"netease:123","title":"PRIVATE"})),
            json!({"tool":"media_player","operation":"play","generation":3,"trackRef":"aabbccddeeff0011"}));
    }
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

    #[test]
    fn dsp_timings_keep_numeric_metadata_not_audio_or_text() {
        let safe = metadata(json!({"dspFrames":3,"dspLastUs":70000,
            "dspMaxUs":71000,"dspAecUs":68000,"dspNsUs":1500,"dspVadUs":500,
            "aecReferenceSamples":640,"internalFreeBytes":64000,"internalLargestBytes":32768,
            "captureTxLastUs":200,"captureTxMaxUs":1000,"captureStackMinBytes":2048,
            "pcm":"private audio","transcript":"private words"}));
        assert_eq!(safe,json!({"dspFrames":3,"dspLastUs":70000,
            "dspMaxUs":71000,"dspAecUs":68000,"dspNsUs":1500,"dspVadUs":500,
            "aecReferenceSamples":640,"internalFreeBytes":64000,"internalLargestBytes":32768,
            "captureTxLastUs":200,"captureTxMaxUs":1000,"captureStackMinBytes":2048}));
        assert_eq!(metadata(json!({"dspAecUs":"private words", "captureTxMaxUs":"secret",
            "internalFreeBytes":"private path", "captureStackMinBytes":"secret"})),json!({}));
    }
}
