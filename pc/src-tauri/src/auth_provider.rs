//! Authenticated PC data providers for `widget/data` sources.
//! Tokens live only on this PC (owner-only file, or the OS vault on Windows) and never enter
//! packages, snapshots, logs or the device. Endpoints are built-in descriptors; a package only
//! subscribes to a source name. Network work uses an injectable transport so the flow is
//! testable without any provider being contacted.
use crate::widget_data::{bounded_text, DataRow, DataSnapshot, MAX_ROWS, PROTOCOL};
use serde_json::{json, Value};
mod response;
use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    sync::{Mutex, OnceLock},
    time::{Duration, Instant},
};

pub struct DeviceFlow {
    pub device_endpoint: &'static str,
    pub token_endpoint: &'static str,
    pub scope: &'static str,
}
pub struct Usage {
    pub endpoint: &'static str,
    pub ttl_ms: u32,
    pub rows: fn(&Value) -> Vec<DataRow>,
}
pub struct Descriptor {
    pub source: &'static str,
    pub title: &'static str,
    /// false: the protocol is not confirmed; the provider refuses to run and says so.
    pub verified: bool,
    pub hosts: &'static [&'static str],
    pub device: Option<DeviceFlow>,
    pub usage: Option<Usage>,
}

fn github_rows(user: &Value) -> Vec<DataRow> {
    let s = |v: &Value| v.as_str().unwrap_or("—").to_string();
    let n = |v: &Value| v.as_i64().map(|n| n.to_string()).unwrap_or("—".into());
    vec![
        DataRow { id: "login".into(), label: "账号".into(), value: bounded_text(&s(&user["login"]), 24), detail: bounded_text(&s(&user["name"]), 24), meta: "GitHub 登录名 / 显示名".into(), tone: 0 },
        DataRow { id: "repos".into(), label: "公开仓库".into(), value: n(&user["public_repos"]), detail: String::new(), meta: "public_repos".into(), tone: 0 },
        DataRow { id: "followers".into(), label: "关注者".into(), value: n(&user["followers"]), detail: format!("关注 {}", n(&user["following"])), meta: "followers / following".into(), tone: 0 },
        DataRow { id: "gists".into(), label: "公开 Gist".into(), value: n(&user["public_gists"]), detail: String::new(), meta: "public_gists".into(), tone: 0 },
    ]
}

/// Built-in descriptors. `codex.usage` stays unverified until its endpoint contract is confirmed
/// by the user; the framework will not guess an API or impersonate another client.
pub fn descriptors() -> &'static [Descriptor] {
    static LIST: OnceLock<Vec<Descriptor>> = OnceLock::new();
    LIST.get_or_init(|| vec![
        Descriptor {
            source: "github.me", title: "GitHub 账号概览", verified: true,
            hosts: &["github.com", "api.github.com"],
            device: Some(DeviceFlow { device_endpoint: "https://github.com/login/device/code", token_endpoint: "https://github.com/login/oauth/access_token", scope: "read:user" }),
            usage: Some(Usage { endpoint: "https://api.github.com/user", ttl_ms: 120_000, rows: github_rows }),
        },
        Descriptor {
            source: "codex.usage", title: "Codex 订阅额度", verified: false,
            hosts: &[], device: None, usage: None,
        },
    ])
}
pub fn descriptor(source: &str) -> Option<&'static Descriptor> {
    descriptors().iter().find(|d| d.source == source)
}
pub fn is_source(source: &str) -> bool {
    descriptor(source).is_some()
}

/// Minimal HTTP surface so tests inject responses and production pins hosts.
pub trait Transport: Send {
    fn post_form(&self, url: &str, form: &[(&str, &str)]) -> Result<Value, String>;
    fn get_bearer(&self, url: &str, token: &str) -> Result<Value, String>;
}
struct Https;
impl Https {
    fn client() -> Result<reqwest::blocking::Client, String> {
        reqwest::blocking::Client::builder().timeout(Duration::from_secs(20)).redirect(reqwest::redirect::Policy::none()).build().map_err(|_| "无法创建网络客户端".to_string())
    }
    fn json(r: reqwest::blocking::Response) -> Result<Value, String> {
        if r.content_length().is_some_and(|n| n > response::MAX_RESPONSE_BYTES as u64) {
            return Err("响应过大".into());
        }
        // Bound the actual decoded body too: chunked/compressed responses may
        // omit Content-Length or advertise only their compressed size.
        let bytes = response::read_body(r)?;
        serde_json::from_slice(&bytes).map_err(|_| "响应不是 JSON".into())
    }
}
impl Transport for Https {
    fn post_form(&self, url: &str, form: &[(&str, &str)]) -> Result<Value, String> {
        let r = Self::client()?.post(url).header("Accept", "application/json").form(form).send().map_err(|_| "网络请求失败")?;
        Self::json(r)
    }
    fn get_bearer(&self, url: &str, token: &str) -> Result<Value, String> {
        let r = Self::client()?.get(url).bearer_auth(token).header("Accept", "application/vnd.github+json").header("User-Agent", "pet-manager-auth-provider").send().map_err(|_| "网络请求失败")?;
        if !r.status().is_success() { return Err(format!("上游返回 {}", r.status().as_u16())); }
        Self::json(r)
    }
}

#[derive(Clone)]
struct Pending { device_code: String, user_code: String, verification_uri: String, interval: u64, expires_at: Instant, last_poll: Option<Instant> }

struct Service {
    root: PathBuf,
    transport: Box<dyn Transport>,
    client_ids: HashMap<String, String>,
    pending: HashMap<String, Pending>,
    cache: HashMap<String, (Instant, DataSnapshot)>,
    failures: HashMap<String, (u32, Instant)>,
}
static SERVICE: OnceLock<Mutex<Service>> = OnceLock::new();
fn service() -> &'static Mutex<Service> {
    SERVICE.get_or_init(|| Mutex::new(Service { root: std::env::temp_dir().join("pet-auth-provider-unconfigured"), transport: Box::new(Https), client_ids: HashMap::new(), pending: HashMap::new(), cache: HashMap::new(), failures: HashMap::new() }))
}
pub fn configure(root: PathBuf) {
    if let Ok(mut s) = service().lock() { s.root = root.join("auth-providers"); s.client_ids = load_client_ids(&s.root); }
}
#[cfg(test)]
pub(crate) fn configure_for_test(root: PathBuf, transport: Box<dyn Transport>) {
    let mut s = service().lock().unwrap();
    s.root = root; s.transport = transport; s.client_ids.clear(); s.pending.clear(); s.cache.clear(); s.failures.clear();
}

// ---- token + client id storage (owner-only file on unix; OS vault on windows) ----
fn settings_path(root: &Path) -> PathBuf { root.join("client-ids.json") }
fn load_client_ids(root: &Path) -> HashMap<String, String> {
    std::fs::read(settings_path(root)).ok().and_then(|b| serde_json::from_slice::<HashMap<String, String>>(&b).ok()).unwrap_or_default()
}
fn store_client_ids(root: &Path, ids: &HashMap<String, String>) -> Result<(), String> {
    std::fs::create_dir_all(root).map_err(|_| "无法创建提供器目录")?;
    let mut file = tempfile::NamedTempFile::new_in(root).map_err(|_| "无法保存提供器设置")?;
    std::io::Write::write_all(&mut file, &serde_json::to_vec(ids).map_err(|_| "设置无效")?).map_err(|_| "无法保存提供器设置")?;
    file.persist(settings_path(root)).map_err(|_| "无法保存提供器设置")?;
    Ok(())
}
#[cfg(not(windows))]
fn token_path(root: &Path, source: &str) -> Result<PathBuf, String> {
    use std::fs;
    let dir = root.join("private");
    fs::create_dir_all(&dir).map_err(|_| "无法创建令牌目录")?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o700)).map_err(|_| "无法保护令牌目录")?;
    }
    Ok(dir.join(format!("{source}.token")))
}
#[cfg(not(windows))]
fn load_token(root: &Path, source: &str) -> Result<Option<String>, String> {
    let path = token_path(root, source)?;
    match std::fs::read(&path) {
        Ok(bytes) if bytes.len() <= 8192 => Ok(Some(String::from_utf8(bytes).map_err(|_| "令牌损坏")?)),
        Ok(_) => Err("令牌文件过大".into()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(_) => Err("无法读取令牌".into()),
    }
}
#[cfg(not(windows))]
fn store_token(root: &Path, source: &str, token: Option<&str>) -> Result<(), String> {
    use std::io::Write;
    let path = token_path(root, source)?;
    match token {
        Some(token) => {
            let mut file = tempfile::NamedTempFile::new_in(path.parent().unwrap()).map_err(|_| "无法保存令牌")?;
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                file.as_file().set_permissions(std::fs::Permissions::from_mode(0o600)).map_err(|_| "无法保护令牌")?;
            }
            file.write_all(token.as_bytes()).and_then(|_| file.as_file().sync_all()).map_err(|_| "无法保存令牌")?;
            file.persist(path).map_err(|_| "无法替换令牌")?;
            Ok(())
        }
        None => match std::fs::remove_file(path) { Ok(()) => Ok(()), Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()), Err(_) => Err("无法删除令牌".into()) },
    }
}
#[cfg(windows)]
fn entry(source: &str) -> Result<keyring::Entry, String> {
    keyring::Entry::new("com.petmanager.desktop.auth-provider", source).map_err(|_| "无法打开系统凭据存储".into())
}
#[cfg(windows)]
fn load_token(_: &Path, source: &str) -> Result<Option<String>, String> {
    match entry(source)?.get_password() { Ok(t) => Ok(Some(t)), Err(keyring::Error::NoEntry) => Ok(None), Err(_) => Err("无法读取系统凭据".into()) }
}
#[cfg(windows)]
fn store_token(_: &Path, source: &str, token: Option<&str>) -> Result<(), String> {
    match token {
        Some(t) => entry(source)?.set_password(t).map_err(|_| "令牌保存失败".into()),
        None => match entry(source)?.delete_credential() { Ok(()) | Err(keyring::Error::NoEntry) => Ok(()), Err(_) => Err("无法删除令牌".into()) },
    }
}

fn host_allowed(d: &Descriptor, url: &str) -> bool {
    reqwest::Url::parse(url).ok().is_some_and(|u| u.scheme() == "https" && u.host_str().is_some_and(|h| d.hosts.contains(&h)))
}

/// Public, token-free status for the PC panel: never includes tokens or device codes.
pub fn status() -> Value {
    let s = service().lock().map(|s| (s.root.clone(), s.client_ids.clone(), s.pending.clone())).unwrap_or_else(|p| { let s = p.into_inner(); (s.root.clone(), s.client_ids.clone(), s.pending.clone()) });
    let providers: Vec<Value> = descriptors().iter().map(|d| {
        let authorized = load_token(&s.0, d.source).ok().flatten().is_some();
        let pending = s.2.get(d.source).map(|p| json!({"userCode": p.user_code, "verificationUri": p.verification_uri, "expired": Instant::now() > p.expires_at}));
        json!({"source": d.source, "title": d.title, "verified": d.verified, "authorized": authorized, "clientIdConfigured": s.1.contains_key(d.source), "pending": pending, "hosts": d.hosts})
    }).collect();
    json!({"providers": providers})
}

#[tauri::command]
pub fn auth_provider_status() -> Value { status() }

/// Stores the OAuth client id the user created with the provider; not a secret but still only on this PC.
#[tauri::command]
pub fn auth_provider_configure(source: String, client_id: String) -> Result<Value, String> {
    let d = descriptor(&source).ok_or("未知的数据源")?;
    if !d.verified { return Err("此提供器的接口尚未确认，不能配置".into()); }
    let client_id = client_id.trim().to_string();
    if client_id.is_empty() || client_id.len() > 128 || !client_id.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)) { return Err("client_id 格式无效".into()); }
    let mut s = service().lock().map_err(|_| "提供器不可用")?;
    s.client_ids.insert(source.clone(), client_id);
    let ids = s.client_ids.clone(); store_client_ids(&s.root, &ids)?;
    drop(s); // status() takes the same lock
    Ok(status())
}

/// Starts the device-code flow: returns the user code and URL to show the user. No browser is opened automatically.
#[tauri::command]
pub fn auth_provider_begin(source: String) -> Result<Value, String> {
    let d = descriptor(&source).ok_or("未知的数据源")?;
    let flow = d.device.as_ref().filter(|_| d.verified).ok_or("此提供器的接口尚未确认，不能授权")?;
    let mut s = service().lock().map_err(|_| "提供器不可用")?;
    let client_id = s.client_ids.get(&source).cloned().ok_or("请先填写该提供器的 OAuth client_id")?;
    if !host_allowed(d, flow.device_endpoint) { return Err("端点不在允许的主机内".into()); }
    let r = s.transport.post_form(flow.device_endpoint, &[("client_id", &client_id), ("scope", flow.scope)])?;
    let text = |k: &str| r[k].as_str().filter(|v| !v.is_empty() && v.len() <= 512).map(str::to_string).ok_or(format!("授权响应缺少 {k}"));
    let pending = Pending { device_code: text("device_code")?, user_code: text("user_code")?, verification_uri: text("verification_uri")?, interval: r["interval"].as_u64().unwrap_or(5).clamp(1, 60), expires_at: Instant::now() + Duration::from_secs(r["expires_in"].as_u64().unwrap_or(900).clamp(60, 3600)), last_poll: None };
    let shown = json!({"userCode": pending.user_code, "verificationUri": pending.verification_uri, "interval": pending.interval});
    s.pending.insert(source, pending);
    Ok(shown)
}

/// One poll of the token endpoint, respecting the provider's interval. Never loops.
#[tauri::command]
pub fn auth_provider_poll(source: String) -> Result<Value, String> {
    let d = descriptor(&source).ok_or("未知的数据源")?;
    let flow = d.device.as_ref().ok_or("此提供器没有设备码流程")?;
    let mut s = service().lock().map_err(|_| "提供器不可用")?;
    let client_id = s.client_ids.get(&source).cloned().ok_or("缺少 client_id")?;
    let Some(p) = s.pending.get(&source).cloned() else { return Ok(json!({"phase": "idle"})); };
    if Instant::now() > p.expires_at { s.pending.remove(&source); return Ok(json!({"phase": "expired"})); }
    if p.last_poll.is_some_and(|t| t.elapsed() < Duration::from_secs(p.interval)) { return Ok(json!({"phase": "pending", "wait": true})); }
    if !host_allowed(d, flow.token_endpoint) { return Err("端点不在允许的主机内".into()); }
    let r = s.transport.post_form(flow.token_endpoint, &[("client_id", &client_id), ("device_code", &p.device_code), ("grant_type", "urn:ietf:params:oauth:grant-type:device_code")])?;
    if let Some(token) = r["access_token"].as_str().filter(|t| !t.is_empty() && t.len() <= 4096) {
        store_token(&s.root, &source, Some(token))?;
        s.pending.remove(&source); s.cache.remove(&source); s.failures.remove(&source);
        return Ok(json!({"phase": "authorized"}));
    }
    let error = r["error"].as_str().unwrap_or("unknown");
    if let Some(p) = s.pending.get_mut(&source) {
        p.last_poll = Some(Instant::now());
        if error == "slow_down" { p.interval = (p.interval + 5).min(60); }
    }
    match error {
        "authorization_pending" | "slow_down" => Ok(json!({"phase": "pending"})),
        "expired_token" => { s.pending.remove(&source); Ok(json!({"phase": "expired"})) }
        "access_denied" => { s.pending.remove(&source); Ok(json!({"phase": "denied"})) }
        other => Ok(json!({"phase": "error", "code": bounded_text(other, 40)})),
    }
}

#[tauri::command]
pub fn auth_provider_forget(source: String) -> Result<Value, String> {
    descriptor(&source).ok_or("未知的数据源")?;
    let mut s = service().lock().map_err(|_| "提供器不可用")?;
    store_token(&s.root, &source, None)?;
    s.pending.remove(&source); s.cache.remove(&source); s.failures.remove(&source);
    drop(s); // status() takes the same lock
    Ok(status())
}

fn empty(source: &str, ttl_ms: u32, status: &str, message: &str) -> DataSnapshot {
    DataSnapshot { schema: 1, date: chrono::Local::now().format("%Y-%m-%d").to_string(), source: source.into(), ttl_ms, status: status.into(), message: message.into(), rows: vec![] }
}

/// Snapshot for a registered source. Unauthorized or unverified providers return honest empty
/// states; upstream failures keep the last rows and mark the snapshot stale with backoff.
pub fn snapshot(source: &str) -> Result<DataSnapshot, String> {
    let d = descriptor(source).ok_or("不支持的数据源")?;
    let Some(usage) = d.usage.as_ref().filter(|_| d.verified) else {
        return Ok(empty(source, 300_000, "error", "此数据源的接口契约尚未确认，提供器未启用"));
    };
    let mut s = service().lock().map_err(|_| "提供器不可用")?;
    if let Some((at, cached)) = s.cache.get(source) {
        if at.elapsed() < Duration::from_millis(u64::from(usage.ttl_ms) / 2) { return Ok(cached.clone()); }
    }
    let Some(token) = load_token(&s.root, source)? else { return Ok(empty(source, 60_000, "empty", "尚未授权：请在 PC 组件详情完成设备码授权")); };
    if let Some((count, at)) = s.failures.get(source) {
        if at.elapsed() < Duration::from_secs(15u64 << count.min(&5)) {
            if let Some((_, cached)) = s.cache.get(source) { let mut c = cached.clone(); c.status = "error".into(); c.message = "上游暂不可用，显示上次结果".into(); return Ok(c); }
            return Ok(empty(source, 60_000, "error", "上游暂不可用，稍后重试"));
        }
    }
    if !host_allowed(d, usage.endpoint) { return Err("端点不在允许的主机内".into()); }
    match s.transport.get_bearer(usage.endpoint, &token) {
        Ok(v) => {
            let mut rows = (usage.rows)(&v); rows.truncate(MAX_ROWS);
            let snap = DataSnapshot { schema: 1, date: chrono::Local::now().format("%Y-%m-%d").to_string(), source: source.into(), ttl_ms: usage.ttl_ms, status: "ok".into(), message: String::new(), rows };
            s.cache.insert(source.into(), (Instant::now(), snap.clone())); s.failures.remove(source);
            Ok(snap)
        }
        Err(e) => {
            let count = s.failures.get(source).map(|f| f.0 + 1).unwrap_or(1);
            s.failures.insert(source.into(), (count, Instant::now()));
            if let Some((_, cached)) = s.cache.get(source) { let mut c = cached.clone(); c.status = "error".into(); c.message = bounded_text(&e, 63); return Ok(c); }
            Ok(empty(source, 60_000, "error", &bounded_text(&e, 63)))
        }
    }
}

/// Sources worth pushing right now: verified, authorized providers only.
pub fn push_sources() -> Vec<&'static str> {
    let root = service().lock().map(|s| s.root.clone()).unwrap_or_default();
    descriptors().iter().filter(|d| d.verified && load_token(&root, d.source).ok().flatten().is_some()).map(|d| d.source).collect()
}

pub const _PROTOCOL_CHECK: &str = PROTOCOL;

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex as StdMutex};
    static SERIAL: StdMutex<()> = StdMutex::new(());
    struct Fake { calls: Arc<StdMutex<Vec<String>>>, token_replies: Arc<StdMutex<Vec<Value>>>, usage_reply: Value }
    impl Transport for Fake {
        fn post_form(&self, url: &str, form: &[(&str, &str)]) -> Result<Value, String> {
            self.calls.lock().unwrap().push(format!("POST {url} {}", form.iter().map(|(k, _)| *k).collect::<Vec<_>>().join(",")));
            if url.ends_with("/device/code") { return Ok(json!({"device_code":"dev-secret","user_code":"ABCD-1234","verification_uri":"https://github.com/login/device","expires_in":900,"interval":1})); }
            Ok(self.token_replies.lock().unwrap().remove(0))
        }
        fn get_bearer(&self, url: &str, token: &str) -> Result<Value, String> {
            self.calls.lock().unwrap().push(format!("GET {url} bearer={}", token.len()));
            if self.usage_reply.is_null() { Err("模拟上游失败".into()) } else { Ok(self.usage_reply.clone()) }
        }
    }
    #[test]
    fn device_flow_stores_token_privately_and_snapshots_rows_without_leaking_secrets() {
        let _serial = SERIAL.lock().unwrap_or_else(|p| p.into_inner());
        let dir = tempfile::tempdir().unwrap();
        let calls = Arc::new(StdMutex::new(vec![]));
        let replies = Arc::new(StdMutex::new(vec![json!({"error":"authorization_pending"}), json!({"access_token":"gho_secret_token_value","token_type":"bearer"})]));
        configure_for_test(dir.path().to_path_buf(), Box::new(Fake { calls: calls.clone(), token_replies: replies, usage_reply: json!({"login":"octocat","name":"The Octocat","public_repos":8,"followers":3938,"following":9,"public_gists":8}) }));
        assert_eq!(snapshot("github.me").unwrap().status, "empty");
        assert!(auth_provider_begin("github.me".into()).is_err(), "client id is required first");
        auth_provider_configure("github.me".into(), "Iv1.testclient".into()).unwrap();
        let shown = auth_provider_begin("github.me".into()).unwrap();
        assert_eq!(shown["userCode"], "ABCD-1234");
        assert!(serde_json::to_string(&status()).unwrap().contains("ABCD-1234"));
        assert!(!serde_json::to_string(&status()).unwrap().contains("dev-secret"));
        assert_eq!(auth_provider_poll("github.me".into()).unwrap()["phase"], "pending");
        assert_eq!(auth_provider_poll("github.me".into()).unwrap()["wait"], true, "interval is respected");
        std::thread::sleep(Duration::from_millis(1100));
        assert_eq!(auth_provider_poll("github.me".into()).unwrap()["phase"], "authorized");
        let token_file = dir.path().join("private").join("github.me.token");
        assert_eq!(std::fs::read_to_string(&token_file).unwrap(), "gho_secret_token_value");
        #[cfg(unix)]
        { use std::os::unix::fs::PermissionsExt; assert_eq!(std::fs::metadata(&token_file).unwrap().permissions().mode() & 0o777, 0o600); }
        let snap = snapshot("github.me").unwrap();
        assert_eq!(snap.status, "ok"); assert_eq!(snap.rows[0].value, "octocat"); assert_eq!(snap.rows.len(), 4);
        let text = serde_json::to_string(&snap).unwrap(); assert!(!text.contains("gho_secret"));
        assert!(calls.lock().unwrap().iter().all(|c| !c.contains("gho_secret")));
        assert_eq!(push_sources(), vec!["github.me"]);
        auth_provider_forget("github.me".into()).unwrap();
        assert!(!token_file.exists()); assert_eq!(snapshot("github.me").unwrap().status, "empty");
    }
    #[test]
    fn unverified_provider_refuses_to_run_and_failures_keep_last_rows() {
        let _serial = SERIAL.lock().unwrap_or_else(|p| p.into_inner());
        let dir = tempfile::tempdir().unwrap();
        configure_for_test(dir.path().to_path_buf(), Box::new(Fake { calls: Default::default(), token_replies: Default::default(), usage_reply: Value::Null }));
        assert!(auth_provider_begin("codex.usage".into()).is_err());
        assert!(auth_provider_configure("codex.usage".into(), "x".into()).is_err());
        let snap = snapshot("codex.usage").unwrap(); assert_eq!(snap.status, "error"); assert!(snap.message.contains("尚未确认"));
        store_token(dir.path(), "github.me", Some("tok")).unwrap();
        let snap = snapshot("github.me").unwrap(); assert_eq!(snap.status, "error"); assert!(snap.rows.is_empty());
        assert!(snapshot("nope").is_err());
    }
}
