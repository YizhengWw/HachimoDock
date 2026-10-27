//! Allowlisted generation HTTP diagnostics: never retain URL, headers, body, prompt or keys.
use serde::Serialize;
use std::{fs, io::Write, path::{Path, PathBuf}, sync::{Mutex, OnceLock}, time::{Instant, SystemTime, UNIX_EPOCH}};

static LOG_DIR: OnceLock<PathBuf> = OnceLock::new();
pub fn configure(dir: PathBuf) { let _ = LOG_DIR.set(dir); }
pub fn persist_local(record: &Record) { if let Some(dir) = LOG_DIR.get() { persist(dir, record); } }

pub const PREFIX: &str = "PET_HTTP_DIAGNOSTIC:";
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Record {
    pub local_id: String,
    pub timestamp_ms: u64,
    pub stage: String,
    pub phase: String,
    pub elapsed_ms: u64,
    pub timeout_ms: u64,
    pub status: Option<u16>,
    pub request_id: Option<String>,
    pub outcome: String,
}

pub fn stage(value: &str) -> &str {
    match value {
        "image_edit" | "thinking" | "reference_review" | "trial_review" | "video_submit" | "video_poll" | "download" => value,
        _ => "other",
    }
}

pub fn safe_request_id(value: &str) -> Option<String> {
    let value = value.trim();
    if value.is_empty() || value.len() > 128 || value.starts_with("sk-")
        || !value.bytes().all(|b| b.is_ascii_alphanumeric() || b"-_.:".contains(&b)) { return None; }
    Some(value.to_string())
}

impl Record {
    pub fn new(label: &str, timeout_ms: u64) -> Self {
        Self {
            local_id: uuid::Uuid::new_v4().to_string(),
            timestamp_ms: SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis() as u64,
            stage: stage(label).into(), phase: "prepare".into(), elapsed_ms: 0,
            timeout_ms, status: None, request_id: None, outcome: "pending".into(),
        }
    }
    pub fn headers(&mut self, response: &reqwest::Response) {
        self.status = Some(response.status().as_u16());
        self.phase = "response_body".into();
        self.request_id = ["x-request-id", "x-tt-logid", "request-id"].iter()
            .find_map(|key| response.headers().get(*key).and_then(|v| v.to_str().ok()).and_then(safe_request_id));
    }
    pub fn finish(&mut self, started: Instant, outcome: &str) {
        self.elapsed_ms = started.elapsed().as_millis() as u64;
        self.outcome = outcome.into();
    }
    pub fn error_body_id(&mut self, body: &str) {
        if self.request_id.is_some() || body.len() > 64 * 1024 { return; }
        let Ok(json) = serde_json::from_str::<serde_json::Value>(body) else { return };
        let direct = json.get("request_id").or_else(|| json.pointer("/error/request_id")).and_then(|v| v.as_str());
        let message_id = json.pointer("/error/message").and_then(|v| v.as_str())
            .and_then(|m| m.split_once("Request id: ").map(|(_, tail)| tail))
            .and_then(|tail| tail.split_whitespace().next());
        self.request_id = direct.or(message_id).and_then(safe_request_id);
    }
    pub fn transport_error(&mut self, error: &reqwest::Error) -> String {
        self.outcome = if error.is_timeout() { "timeout" } else { "transport_error" }.into();
        if error.is_connect() { self.phase = "connect".into(); }
        let hint = if error.is_timeout() {
            match self.phase.as_str() {
                "connect" => "建立连接超时，尚未取得服务响应；请检查网络或代理。",
                "response_body" => "已收到服务响应，但读取内容超时；不能据此判断本机网络断开。",
                _ => "发送请求或等待响应超时；服务端可能仍在处理，请勿重复提交。",
            }.to_string()
        } else { crate::llm_network::connection_error(error) };
        hint
    }
    pub fn encode_error(&self, message: &str) -> String {
        format!("{PREFIX}{}", serde_json::json!({"message":message,"diagnostic":self}))
    }
}

// Best-effort bounded local journal. A logging failure must never fail generation.
// Only typed/allowlisted records reach disk; 2 x 256 KiB maximum.
pub fn persist(dir: &Path, record: &Record) {
    static LOCK: Mutex<()> = Mutex::new(());
    let Ok(_guard) = LOCK.lock() else { return };
    let _ = persist_inner(dir, record);
}
fn persist_inner(dir: &Path, record: &Record) -> std::io::Result<()> {
    fs::create_dir_all(dir)?;
    let path = dir.join("generation-http.jsonl");
    let old = dir.join("generation-http.previous.jsonl");
    for file in [&path, &old] {
        if let Ok(meta) = fs::symlink_metadata(file) {
            if !meta.is_file() { return Ok(()); }
        }
    }
    if fs::metadata(&path).map(|m| m.len() >= 256 * 1024).unwrap_or(false) {
        if old.exists() { fs::remove_file(&old)?; }
        fs::rename(&path, &old)?;
    }
    let mut options = fs::OpenOptions::new();
    options.create(true).append(true);
    #[cfg(unix)] {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600).custom_flags(libc::O_NOFOLLOW);
    }
    let mut file = options.open(path)?;
    writeln!(file, "{}", serde_json::to_string(record)?)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn metadata_is_allowlisted() {
        let r = Record::new("https://internal/?key=SECRET", 120_000);
        let text = serde_json::to_string(&r).unwrap();
        assert!(text.contains("other"));
        assert!(!text.contains("SECRET"));
        for bad in ["Bearer secret", "https://host/token", "sk-abc", "a\nb", ""] {
            assert_eq!(safe_request_id(bad), None);
        }
        assert_eq!(safe_request_id("req-123_456"), Some("req-123_456".into()));
        let mut r = r;
        r.error_body_id(r#"{"error":{"message":"hidden prompt. Request id: req-987"},"api_key":"PRIVATE"}"#);
        assert_eq!(r.request_id.as_deref(), Some("req-987"));
        assert!(!serde_json::to_string(&r).unwrap().contains("PRIVATE"));
    }
    #[tokio::test]
    async fn distinguishes_wait_and_body_timeout_with_request_id() {
        use tokio::{io::{AsyncReadExt, AsyncWriteExt}, net::TcpListener};
        for headers in [false, true] {
            let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
            let address = listener.local_addr().unwrap();
            let server = tokio::spawn(async move {
                let (mut socket, _) = listener.accept().await.unwrap();
                let mut bytes = [0; 1024]; let _ = socket.read(&mut bytes).await;
                if headers { socket.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\nx-request-id: req-test-42\r\n\r\nx").await.unwrap(); }
                tokio::time::sleep(std::time::Duration::from_millis(300)).await;
            });
            let client = reqwest::Client::builder().no_proxy().timeout(std::time::Duration::from_millis(100)).build().unwrap();
            let mut r = Record::new("thinking", 100);
            r.phase = "request_wait".into();
            let err = match client.get(format!("http://{address}")).send().await {
                Ok(response) => { r.headers(&response); response.text().await.unwrap_err() }
                Err(error) => error,
            };
            let message = r.transport_error(&err);
            assert_eq!(r.outcome, "timeout");
            assert_eq!(r.phase, if headers { "response_body" } else { "request_wait" });
            assert_eq!(r.request_id, if headers { Some("req-test-42".into()) } else { None });
            assert!(!message.contains("公司"));
            server.abort();
        }
    }
}
