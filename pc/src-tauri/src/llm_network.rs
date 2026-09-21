//! Shared cloud HTTP/download/speech networking. Never embeds a proxy or credential.
//! The legacy settings filename is retained for upgrade compatibility; LAN stays separate.
//! Internal builds may add an explicitly approved public CA chain to native trust.
//! System mode honors reqwest's OS/environment proxy discovery and native TLS trust.
use serde::{Deserialize, Serialize};
use std::{fs, io::{Read, Write}, path::{Path, PathBuf}, sync::{Mutex, OnceLock}, time::{Duration, Instant}};

const MAX_SETTINGS_BYTES: u64 = 256 * 1024;
const FILE_NAME: &str = "llm-network.json";
#[cfg(feature = "internal-network")]
const BUNDLED_CA: &str = include_str!(concat!(env!("OUT_DIR"), "/approved-internal-ca.pem"));
#[cfg(not(feature = "internal-network"))]
const BUNDLED_CA: &str = "";
static STORAGE_DIR: OnceLock<PathBuf> = OnceLock::new();

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum ProxyMode { #[default] System, Direct, Manual }

#[derive(Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
struct Settings {
    mode: ProxyMode,
    proxy_url: String,
    ca_pem: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NetworkInput {
    mode: ProxyMode,
    #[serde(default)] proxy_url: String,
    ca_file: Option<String>,
    #[serde(default)] clear_certificate: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NetworkStatus {
    mode: ProxyMode,
    proxy_url: String,
    certificate_configured: bool,
    bundled_certificate_configured: bool,
}

impl Settings {
    fn status(&self) -> NetworkStatus {
        NetworkStatus { mode: self.mode.clone(), proxy_url: self.proxy_url.clone(), certificate_configured: !self.ca_pem.is_empty(), bundled_certificate_configured: !BUNDLED_CA.is_empty() }
    }
}

pub fn configure_storage_dir(dir: PathBuf) -> Result<(), String> {
    match STORAGE_DIR.set(dir.clone()) {
        Ok(()) => Ok(()),
        Err(_) if STORAGE_DIR.get() == Some(&dir) => Ok(()),
        Err(_) => Err("网络配置目录已经初始化".into()),
    }
}

fn settings_path() -> Result<PathBuf, String> {
    STORAGE_DIR.get().map(|dir| dir.join(FILE_NAME)).ok_or_else(|| "网络配置尚未初始化".into())
}

fn read_bounded(path: &Path) -> Result<String, String> {
    let mut options = fs::OpenOptions::new();
    options.read(true);
    #[cfg(unix)] {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NOFOLLOW);
    }
    let file = options.open(path).map_err(|_| "无法读取网络配置或证书文件，请确认文件存在且可读".to_string())?;
    if !file.metadata().map_err(|_| "无法检查文件类型")?.is_file() { return Err("请选择普通证书文件".into()); }
    let mut raw = String::new();
    file.take(MAX_SETTINGS_BYTES + 1).read_to_string(&mut raw).map_err(|_| "文件必须为 UTF-8 文本格式")?;
    if raw.len() as u64 > MAX_SETTINGS_BYTES { return Err("网络配置或证书文件超过 256 KiB".into()); }
    Ok(raw)
}

fn load_path(path: &Path) -> Result<Settings, String> {
    match fs::symlink_metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Settings::default()),
        Err(_) => return Err("无法读取网络配置".into()),
        Ok(meta) if meta.file_type().is_symlink() => return Err("网络配置不能是符号链接".into()),
        _ => {}
    }
    let settings: Settings = serde_json::from_str(&read_bounded(path)?).map_err(|_| "网络配置损坏，请重新保存网络设置")?;
    validate(&settings)?;
    Ok(settings)
}

fn load() -> Result<Settings, String> {
    // Headless cloud tests can use standard OS/environment networking without app setup.
    match STORAGE_DIR.get() { Some(dir) => load_path(&dir.join(FILE_NAME)), None => Ok(Settings::default()) }
}

fn certificates(pem: &str) -> Result<Vec<reqwest::Certificate>, String> {
    if pem.is_empty() { return Ok(vec![]); }
    if pem.contains("PRIVATE KEY") { return Err("只允许导入 CA 公共证书，不能导入私钥".into()); }
    let certs = reqwest::Certificate::from_pem_bundle(pem.as_bytes()).map_err(|_| "CA 证书无效，请使用 PEM 格式公共证书")?;
    if certs.is_empty() || certs.len() > 64 { return Err("证书文件需包含 1 至 64 张 PEM 公共证书".into()); }
    Ok(certs)
}

fn validate(settings: &Settings) -> Result<(), String> {
    if settings.mode == ProxyMode::Manual {
        let url = reqwest::Url::parse(&settings.proxy_url).map_err(|_| "代理地址无效，请填写完整的 http:// 或 https:// 地址")?;
        if !matches!(url.scheme(), "http" | "https") || url.host_str().is_none()
            || !url.username().is_empty() || url.password().is_some()
            || url.query().is_some() || url.fragment().is_some() || !matches!(url.path(), "" | "/") {
            return Err("代理仅支持 HTTP/HTTPS 主机和端口，不接受 URL 内嵌密码、路径或查询参数".into());
        }
    }
    certificates(&settings.ca_pem)?;
    Ok(())
}

fn updated(current: Settings, input: NetworkInput) -> Result<Settings, String> {
    let mut settings = Settings { mode: input.mode, proxy_url: input.proxy_url.trim().to_string(), ca_pem: current.ca_pem };
    if settings.mode != ProxyMode::Manual { settings.proxy_url.clear(); }
    if input.clear_certificate { settings.ca_pem.clear(); }
    if let Some(path) = input.ca_file.filter(|p| !p.trim().is_empty()) { settings.ca_pem = read_bounded(Path::new(&path))?; }
    validate(&settings)?;
    Ok(settings)
}

fn write_path(path: &Path, settings: &Settings) -> Result<(), String> {
    let parent = path.parent().ok_or("网络配置目录无效")?;
    fs::create_dir_all(parent).map_err(|_| "无法创建网络配置目录")?;
    let mut file = tempfile::NamedTempFile::new_in(parent).map_err(|_| "无法创建网络配置文件")?;
    // NamedTempFile is owner-only (0600) on Unix; atomic replacement avoids partial files.
    let raw = serde_json::to_vec_pretty(settings).map_err(|_| "网络配置序列化失败")?;
    if raw.len() as u64 > MAX_SETTINGS_BYTES { return Err("网络配置超过 256 KiB，请减少导入的证书数量".into()); }
    file.write_all(&raw).and_then(|_| file.as_file().sync_all()).map_err(|_| "无法写入网络配置")?;
    file.persist(path).map_err(|_| "无法保存网络配置")?;
    Ok(())
}

fn build_client(settings: &Settings, timeout: Duration) -> Result<reqwest::Client, String> {
    build_cloud_client(settings, Some(timeout), CloudTransport::Api)
}

#[derive(Clone, Copy)]
enum CloudTransport { Api, Download, WebSocket }

fn build_cloud_client(settings: &Settings, timeout: Option<Duration>, transport: CloudTransport) -> Result<reqwest::Client, String> {
    validate(settings)?;
    let mut builder = reqwest::Client::builder()
        .use_native_tls() // OS trust store, including IT-managed enterprise roots.
        .connect_timeout(Duration::from_secs(10))
        .redirect(reqwest::redirect::Policy::none());
    if let Some(timeout) = timeout { builder = builder.timeout(timeout); }
    match transport {
        CloudTransport::Api => {}, // Never forward API credentials through redirects.
        CloudTransport::WebSocket => builder = builder.http1_only(),
        CloudTransport::Download => builder = builder.redirect(reqwest::redirect::Policy::custom(|attempt| {
            if attempt.previous().len() >= 5 {
                attempt.error("下载重定向次数过多")
            } else if !matches!(attempt.url().scheme(), "http" | "https")
                || !attempt.url().username().is_empty() || attempt.url().password().is_some()
                || (attempt.url().scheme() == "http" && attempt.previous().iter().any(|url| url.scheme() == "https")) {
                attempt.error("拒绝不安全的下载重定向")
            } else { attempt.follow() }
        })),
    }
    builder = match settings.mode {
        ProxyMode::System => builder, // Environment first, then this machine's OS proxy; no fixed fallback.
        ProxyMode::Direct => builder.no_proxy(),
        ProxyMode::Manual => builder.no_proxy().proxy(reqwest::Proxy::https(&settings.proxy_url).map_err(|_| "代理配置无效")?),
    };
    for cert in certificates(&settings.ca_pem)? { builder = builder.add_root_certificate(cert); }
    for cert in certificates(BUNDLED_CA)? { builder = builder.add_root_certificate(cert); }
    builder.build().map_err(|_| "网络客户端初始化失败，请检查代理和证书配置".into())
}

pub fn download_client(timeout: Duration) -> Result<reqwest::Client, String> {
    build_cloud_client(&load()?, Some(timeout), CloudTransport::Download)
}

/// Extend trust only for the optional Node community installer invocation.
/// Keep this file alive until npx exits; no keys or proxy overrides are exported.
pub fn community_ca_file() -> Result<Option<tempfile::NamedTempFile>, String> {
    let settings = load()?;
    if settings.ca_pem.is_empty() && BUNDLED_CA.is_empty() { return Ok(None); }
    let mut pem = format!("{}\n{}\n", BUNDLED_CA, settings.ca_pem);
    if let Some(path) = std::env::var_os("NODE_EXTRA_CA_CERTS").filter(|p| !p.is_empty()) {
        let existing = read_bounded(Path::new(&path))?;
        certificates(&existing)?;
        pem.push_str(&existing);
    }
    let mut file = tempfile::NamedTempFile::new().map_err(|_| "无法创建社区导入证书文件")?;
    file.write_all(pem.as_bytes()).and_then(|_| file.flush()).map_err(|_| "无法准备社区导入证书")?;
    Ok(Some(file))
}

pub type CloudSocket = tokio_tungstenite::WebSocketStream<reqwest::Upgraded>;

/// HTTP Upgrade uses the same native trust and proxy policy as cloud APIs.
/// Callers bound the handshake with their existing timeout; upgraded audio streams
/// have no whole-response timeout. Validate the RFC 6455 handshake before framing.
pub async fn connect_websocket(
    request: tokio_tungstenite::tungstenite::http::Request<()>,
) -> Result<(CloudSocket, reqwest::header::HeaderMap), String> {
    let client = build_cloud_client(&load()?, None, CloudTransport::WebSocket)?;
    upgrade_websocket(&client, request).await
}

async fn upgrade_websocket(
    client: &reqwest::Client,
    request: tokio_tungstenite::tungstenite::http::Request<()>,
) -> Result<(CloudSocket, reqwest::header::HeaderMap), String> {
    use tokio_tungstenite::tungstenite::{handshake::derive_accept_key, protocol::Role};
    let mut url = reqwest::Url::parse(&request.uri().to_string()).map_err(|_| "语音服务地址无效")?;
    let scheme = match url.scheme() { "wss" => "https", "ws" => "http", _ => return Err("语音服务需使用 WebSocket 地址".into()) };
    if !url.username().is_empty() || url.password().is_some() { return Err("语音服务地址不能包含凭据".into()); }
    url.set_scheme(scheme).map_err(|_| "语音服务地址无效")?;
    let key = request.headers().get("sec-websocket-key").ok_or("语音握手缺少校验信息")?;
    let expected = derive_accept_key(key.as_bytes());
    let response = client.get(url).headers(request.headers().clone()).send().await.map_err(|e| connection_error(&e))?;
    if response.status().as_u16() != 101 { return Err(format!("语音服务握手失败（HTTP {}）", response.status().as_u16())); }
    let headers = response.headers().clone();
    let has_token = |name: &str, expected: &str| headers.get_all(name).iter().any(|v| v.to_str().unwrap_or("").split(',').any(|s| s.trim().eq_ignore_ascii_case(expected)));
    if !has_token("connection", "upgrade") || !has_token("upgrade", "websocket")
        || headers.get("sec-websocket-accept").and_then(|v| v.to_str().ok()) != Some(expected.as_str())
        || headers.contains_key("sec-websocket-extensions") || headers.contains_key("sec-websocket-protocol") {
        return Err("语音服务 WebSocket 握手校验失败".into());
    }
    let stream = response.upgrade().await.map_err(|e| connection_error(&e))?;
    Ok((tokio_tungstenite::WebSocketStream::from_raw_socket(stream, Role::Client, None).await, headers))
}

struct CachedClient { settings: Settings, timeout: Duration, client: reqwest::Client, created: Instant }
fn cache() -> &'static Mutex<Vec<CachedClient>> {
    static CACHE: OnceLock<Mutex<Vec<CachedClient>>> = OnceLock::new();
    CACHE.get_or_init(|| Mutex::new(Vec::new()))
}

pub fn client(timeout: Duration) -> Result<reqwest::Client, String> {
    let settings = load()?;
    let mut cached = cache().lock().map_err(|_| "网络客户端暂时忙碌")?;
    // Cloud services use different timeout classes. Keep separate pools instead of
    // evicting the LLM connection whenever MIOT or ASR/TTS makes a request.
    cached.retain(|entry|entry.settings==settings && entry.created.elapsed()<Duration::from_secs(60));
    if let Some(entry)=cached.iter().find(|entry|entry.timeout==timeout) {return Ok(entry.client.clone());}
    let client = build_client(&settings, timeout)?;
    if cached.len()>=8 {cached.remove(0);}
    cached.push(CachedClient { settings, timeout, client: client.clone(), created: Instant::now() });
    Ok(client)
}

pub fn connection_error(error: &reqwest::Error) -> String {
    use std::error::Error;
    let mut chain = error.to_string();
    let mut cause = error.source();
    while let Some(e) = cause { chain.push_str(&e.to_string()); cause = e.source(); }
    let chain = chain.to_lowercase();
    let hint = if chain.contains("certificate") || chain.contains("cert") || chain.contains("证书") {
        "HTTPS 证书校验失败：请确认系统时间及公司根证书，或在 API 配置 → 云服务网络中导入 IT 提供的 CA 证书"
    } else if chain.contains("refused") || chain.contains("tunnel") || chain.contains("proxy") {
        "代理或服务器连接失败：请检查这台电脑的系统代理，或在 API 配置 → 云服务网络中填写实际代理地址和端口"
    } else if error.is_timeout() {
        "网络连接超时：请确认公司网络允许访问该服务，并检查代理设置"
    } else {
        "网络连接失败：请在 API 配置 → 云服务网络中测试火山与 DeepSeek 的连接"
    };
    // Do not expose proxy credentials, internal addresses or certificate details in logs/UI.
    hint.into()
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProbeResult { provider: String, reachable: bool, http_status: Option<u16>, message: String }

async fn probe(client: &reqwest::Client, provider: &str, url: &str) -> ProbeResult {
    // Fixed public service probe URLs only; no API keys, prompts or private data are sent.
    match client.get(url).send().await {
        Ok(response) => {
            let code = response.status().as_u16();
            let reachable = response.status().is_success() || matches!(code, 401 | 404 | 405 | 429);
            let message = match code {
                401 => "已收到 HTTPS 响应；测试未发送 Key，正式对话仍需有效 Key 和服务权限".into(),
                403 => "已收到 HTTPS 响应，但访问被拒绝；请核对公司网络策略或服务权限".into(),
                _ => format!("HTTPS 返回 HTTP {code}；这仅检查连通性，不验证模型、额度或 Key"),
            };
            ProbeResult { provider: provider.into(), reachable, http_status: Some(code), message }
        }
        Err(error) => ProbeResult { provider: provider.into(), reachable: false, http_status: None, message: connection_error(&error) },
    }
}

async fn probe_services(settings: &Settings) -> Result<Vec<ProbeResult>, String> {
    let client = build_client(settings, Duration::from_secs(15))?;
    let (ark, deepseek) = tokio::join!(
        probe(&client, "火山方舟（豆包）", "https://ark.cn-beijing.volces.com/api/v3/models"),
        probe(&client, "DeepSeek", "https://api.deepseek.com/models"));
    Ok(vec![ark, deepseek])
}

#[tauri::command]
pub fn load_llm_network_settings() -> Result<NetworkStatus, String> { Ok(load()?.status()) }

#[tauri::command]
pub fn save_llm_network_settings(input: NetworkInput) -> Result<NetworkStatus, String> {
    let settings = updated(load()?, input)?;
    write_path(&settings_path()?, &settings)?;
    if let Ok(mut cached) = cache().lock() { cached.clear(); }
    Ok(settings.status())
}

#[tauri::command]
pub async fn test_llm_network_settings() -> Result<Vec<ProbeResult>, String> {
    if let Ok(mut cached) = cache().lock() { cached.clear(); }
    probe_services(&load()?).await
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fresh_machine_has_no_proxy_or_certificate() {
        let dir = tempfile::tempdir().unwrap();
        let s = load_path(&dir.path().join(FILE_NAME)).unwrap();
        assert_eq!(s.mode, ProxyMode::System);
        assert!(s.proxy_url.is_empty() && s.ca_pem.is_empty());
        assert!(build_client(&s, Duration::from_secs(1)).is_ok());
    }
    #[test]
    fn validates_proxy_and_never_keeps_hidden_manual_override() {
        for proxy in ["localhost:1", "socks5://host:1", "http://user:password@host", "https://host/path", "https://host/?token=x"] {
            assert!(validate(&Settings { mode: ProxyMode::Manual, proxy_url:proxy.into(), ..Default::default() }).is_err());
        }
        let s = Settings { mode: ProxyMode::Manual, proxy_url:"http://company-proxy.example:8080".into(), ..Default::default() };
        assert!(validate(&s).is_ok());
        let s = updated(s, NetworkInput { mode:ProxyMode::System, proxy_url:"http://unused.example".into(), ca_file:None, clear_certificate:false }).unwrap();
        assert!(s.proxy_url.is_empty());
    }
    #[test]
    fn rejects_private_keys_and_invalid_certificates() {
        let private_key_fixture = ["-----BEGIN ", "PRIVATE KEY-----\nsecret"].concat();
        assert!(certificates(&private_key_fixture).is_err());
        assert!(certificates("not a certificate").is_err());
    }
    #[test]
    fn bundled_ca_is_internal_only_and_independent_from_user_imports() {
        let status = Settings::default().status();
        assert!(!status.certificate_configured);
        assert_eq!(status.bundled_certificate_configured, cfg!(feature = "internal-network"));
        if cfg!(feature = "internal-network") {
            assert!(!certificates(BUNDLED_CA).unwrap().is_empty());
        } else {
            assert!(BUNDLED_CA.is_empty());
        }
        let cleared = updated(Settings::default(), NetworkInput {
            mode: ProxyMode::System, proxy_url: String::new(), ca_file: None, clear_certificate: true,
        }).unwrap();
        assert_eq!(cleared.status().bundled_certificate_configured, status.bundled_certificate_configured);
    }
    #[test]
    fn settings_are_atomic_owner_only_and_fail_closed_when_invalid() {
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().join(FILE_NAME);
        let s = Settings { mode:ProxyMode::Direct, ..Default::default() };
        write_path(&path, &s).unwrap();
        assert_eq!(load_path(&path).unwrap().mode, ProxyMode::Direct);
        #[cfg(unix)] { use std::os::unix::fs::PermissionsExt; assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600); }
        fs::write(&path, "invalid").unwrap(); assert!(load_path(&path).is_err());
        fs::write(&path, vec![b'a'; MAX_SETTINGS_BYTES as usize + 1]).unwrap(); assert!(read_bounded(&path).is_err());
    }
    #[test]
    #[cfg(unix)]
    fn rejects_settings_symlinks() {
        let dir = tempfile::tempdir().unwrap(); let target = dir.path().join("target"); let link = dir.path().join(FILE_NAME);
        fs::write(&target, "{}").unwrap(); std::os::unix::fs::symlink(&target, &link).unwrap();
        assert!(load_path(&link).is_err() && read_bounded(&link).is_err());
    }
    #[test]
    fn each_computer_uses_its_own_proxy_and_errors_hide_addresses() {
        use std::net::TcpListener;
        // Two independent local proxy fixtures: neither address is a product default.
        for _ in 0..2 {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let address = listener.local_addr().unwrap();
            listener.set_nonblocking(true).unwrap();
            let worker = std::thread::spawn(move || {
                let deadline = Instant::now() + Duration::from_secs(5);
                loop {
                    if let Ok((mut stream, _)) = listener.accept() {
                        stream.set_nonblocking(false).unwrap();
                        stream.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
                        let mut request = [0u8; 4096];
                        let count = stream.read(&mut request).unwrap();
                        stream.write_all(b"HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").unwrap();
                        return String::from_utf8_lossy(&request[..count]).to_string();
                    }
                    assert!(Instant::now() < deadline, "configured proxy was not used");
                    std::thread::sleep(Duration::from_millis(10));
                }
            });
            let settings = Settings { mode: ProxyMode::Manual, proxy_url: format!("http://{address}"), ..Default::default() };
            let client = build_client(&settings, Duration::from_secs(3)).unwrap();
            let result = tauri::async_runtime::block_on(probe(&client, "test", "https://service.example.invalid/models"));
            assert!(!result.reachable);
            assert!(!result.message.contains(&address.to_string()));
            assert!(!result.message.contains("service.example.invalid"));
            assert!(worker.join().unwrap().starts_with("CONNECT service.example.invalid:443 "));
        }
    }
    #[test]
    fn service_denials_are_not_reported_as_success() {
        use std::net::TcpListener;
        for (code, expected) in [(401, true), (403, false), (500, false)] {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            listener.set_nonblocking(true).unwrap();
            let url = format!("http://{}/models", listener.local_addr().unwrap());
            let worker = std::thread::spawn(move || {
                let deadline = Instant::now() + Duration::from_secs(5);
                loop {
                    if let Ok((mut stream, _)) = listener.accept() {
                        stream.set_nonblocking(false).unwrap();
                        stream.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
                        let mut buf = [0u8; 4096]; stream.read(&mut buf).unwrap();
                        write!(stream, "HTTP/1.1 {code} Test\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").unwrap();
                        return;
                    }
                    assert!(Instant::now() < deadline);
                    std::thread::sleep(Duration::from_millis(10));
                }
            });
            let client = build_client(&Settings { mode: ProxyMode::Direct, ..Default::default() }, Duration::from_secs(3)).unwrap();
            let result = tauri::async_runtime::block_on(probe(&client, "test", &url));
            assert_eq!(result.reachable, expected);
            assert_eq!(result.http_status, Some(code));
            worker.join().unwrap();
        }
    }
    #[test]
    #[ignore = "live HTTPS probes; no credentials sent; optional PET_LLM_TEST_PROXY is runtime-only"]
    fn live_provider_connectivity() {
        let mut settings = Settings::default();
        if let Ok(proxy) = std::env::var("PET_LLM_TEST_PROXY") { settings.mode = ProxyMode::Manual; settings.proxy_url = proxy; }
        let results = tauri::async_runtime::block_on(probe_services(&settings)).unwrap();
        println!("{}", serde_json::to_string(&results).unwrap());
        assert!(results.iter().all(|r| r.reachable), "one or more service network probes failed");
    }
}

#[cfg(test)]
#[path = "cloud_network_tests.rs"]
mod cloud_network_tests;
