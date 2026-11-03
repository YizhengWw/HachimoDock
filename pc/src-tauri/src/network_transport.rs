/*
 * [Input] Paired-board identities, LAN endpoints, and the TCP JSONL channel exposed by P4 firmware.
 * [Output] Mutually authenticated TCP links, UDP discovery results, and a paired-device registry whose
 *          pairing secrets live in the OS credential store; the business protocol is unchanged.
 * [Pos] Wi-Fi connection source beside usb_serial.rs; it owns identity and framing, never USB transactions.
 * [Sync] If this file changes, update `pc/.folder.md` and `firmware/protocol.md` (Wi-Fi transport).
 */

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream, ToSocketAddrs, UdpSocket};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// TCP service and UDP discovery share one port on the board.
pub const NETWORK_PORT: u16 = 38517;
pub const NETWORK_WIRE_PROTOCOL: &str = "pet-net-jsonl-v1";
/// A paired board emits `net/keepalive` every few seconds; silence beyond this ends the link.
pub const NETWORK_IDLE_TIMEOUT: Duration = Duration::from_secs(12);
pub const NETWORK_WRITE_TIMEOUT: Duration = Duration::from_secs(3);
pub const NETWORK_READ_POLL: Duration = Duration::from_millis(100);
/// Scheme used in `UsbConnection.port_name` to mark a Wi-Fi link.
pub const NETWORK_PORT_PREFIX: &str = "wifi://";

const SECRET_BYTES: usize = 32;
const NONCE_BYTES: usize = 16;
const HANDSHAKE_LINE_MAX: usize = 8192;
const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(8);
const CONNECT_TIMEOUT: Duration = Duration::from_secs(4);
const REGISTRY_FILE: &str = "network-devices.json";
#[cfg(windows)]
const KEYRING_SERVICE: &str = "com.petmanager.desktop.network";

// ---------------------------------------------------------------------------
// Cryptographic helpers (HMAC-SHA256 challenge/response)
// ---------------------------------------------------------------------------

pub fn hmac_sha256(key: &[u8], message: &[u8]) -> [u8; 32] {
    const BLOCK: usize = 64;
    let mut padded = [0u8; BLOCK];
    if key.len() > BLOCK {
        padded[..32].copy_from_slice(&Sha256::digest(key));
    } else {
        padded[..key.len()].copy_from_slice(key);
    }
    let mut inner = Sha256::new();
    inner.update(padded.map(|byte| byte ^ 0x36));
    inner.update(message);
    let inner = inner.finalize();
    let mut outer = Sha256::new();
    outer.update(padded.map(|byte| byte ^ 0x5c));
    outer.update(inner);
    outer.finalize().into()
}

fn hex_encode(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn random_hex(bytes: usize) -> String {
    use rand::RngCore;
    let mut buffer = vec![0u8; bytes];
    rand::rngs::OsRng.fill_bytes(&mut buffer);
    hex_encode(&buffer)
}

/// 256-bit pairing secret, hex encoded. The ASCII form is the HMAC key on both ends.
pub fn generate_secret() -> String {
    random_hex(SECRET_BYTES)
}

fn proof(role: &str, secret: &str, board: &str, device_nonce: &str, client_nonce: &str) -> String {
    let message = format!("pet-net-v1|{role}|{board}|{device_nonce}|{client_nonce}");
    hex_encode(&hmac_sha256(secret.as_bytes(), message.as_bytes()))
}

pub fn client_proof(secret: &str, board: &str, device_nonce: &str, client_nonce: &str) -> String {
    proof("client", secret, board, device_nonce, client_nonce)
}

pub fn server_proof(secret: &str, board: &str, device_nonce: &str, client_nonce: &str) -> String {
    proof("server", secret, board, device_nonce, client_nonce)
}

fn constant_time_eq(left: &str, right: &str) -> bool {
    let (left, right) = (left.as_bytes(), right.as_bytes());
    let mut diff = left.len() ^ right.len();
    for index in 0..left.len().max(right.len()) {
        diff |= usize::from(
            left.get(index).copied().unwrap_or(0) ^ right.get(index).copied().unwrap_or(0),
        );
    }
    diff == 0
}

// ---------------------------------------------------------------------------
// Business topics carried over Wi-Fi
// ---------------------------------------------------------------------------

/// Phase one syncs status, sessions and key events. Everything else (assets, audio, media,
/// widgets, firmware, button mapping) keeps its USB transaction; the firmware enforces the
/// same list and answers other topics with `wifi_unsupported`.
const NETWORK_TOPICS: &[&str] = &[
    "ack",
    "bind",
    "system/heartbeat",
    "speech/text",
    "session/current",
    "control/screen-page",
    "control/command",
    "stats/update",
    "runtime/stats",
];
const NETWORK_TOPIC_PREFIXES: &[&str] = &["state/", "diagnostics/", "network/"];

pub fn topic_allowed_over_network(topic: &str) -> bool {
    NETWORK_TOPICS.contains(&topic)
        || NETWORK_TOPIC_PREFIXES.iter().any(|prefix| topic.starts_with(prefix))
}

// ---------------------------------------------------------------------------
// Authenticated TCP link
// ---------------------------------------------------------------------------

/// Write half of a link. Dropping it shuts the socket down in both directions, which also
/// wakes the reader thread's clone instead of waiting for its next poll.
pub struct LinkWriter(pub TcpStream);

impl Write for LinkWriter {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.0.write(buf)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        self.0.flush()
    }
}

impl Drop for LinkWriter {
    fn drop(&mut self) {
        let _ = self.0.shutdown(std::net::Shutdown::Both);
    }
}

pub struct AuthenticatedLink {
    pub stream: TcpStream,
    pub hello: Value,
}

fn is_timeout(error: &std::io::Error) -> bool {
    matches!(
        error.kind(),
        std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted
    )
}

/// Reads one line without read-ahead so bytes after the handshake stay in the socket
/// for the connection's buffered reader.
fn read_handshake_line(stream: &mut TcpStream, deadline: Instant) -> Result<Value, String> {
    let mut line = Vec::new();
    let mut byte = [0u8; 1];
    loop {
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .filter(|remaining| !remaining.is_zero())
            .ok_or_else(|| "WiFi 握手超时".to_string())?;
        stream
            .set_read_timeout(Some(remaining))
            .map_err(|error| error.to_string())?;
        match stream.read(&mut byte) {
            Ok(0) => return Err("设备在握手期间关闭了连接".to_string()),
            Ok(_) => match byte[0] {
                b'\n' => {
                    if line.is_empty() {
                        continue;
                    }
                    if let Ok(value) = serde_json::from_slice::<Value>(&line) {
                        return Ok(value);
                    }
                    line.clear();
                }
                b'\r' => {}
                other => {
                    line.push(other);
                    if line.len() > HANDSHAKE_LINE_MAX {
                        return Err("WiFi 握手消息过长".to_string());
                    }
                }
            },
            Err(error) if is_timeout(&error) => continue,
            Err(error) => return Err(format!("WiFi 握手读取失败: {error}")),
        }
    }
}

fn read_topic(stream: &mut TcpStream, deadline: Instant, wanted: &[&str]) -> Result<(String, Value), String> {
    loop {
        let message = read_handshake_line(stream, deadline)?;
        let topic = message.get("topic").and_then(Value::as_str).unwrap_or("");
        if wanted.contains(&topic) {
            let payload = message.get("payload").cloned().unwrap_or_else(|| json!({}));
            return Ok((topic.to_string(), payload));
        }
    }
}

fn write_json_line(stream: &mut TcpStream, topic: &str, payload: Value) -> Result<(), String> {
    let mut line = serde_json::to_string(&json!({"topic": topic, "payload": payload}))
        .map_err(|error| error.to_string())?;
    line.push('\n');
    stream
        .write_all(line.as_bytes())
        .and_then(|_| stream.flush())
        .map_err(|error| format!("WiFi 握手写入失败: {error}"))
}

fn payload_str<'a>(payload: &'a Value, key: &str) -> &'a str {
    payload.get(key).and_then(Value::as_str).unwrap_or("")
}

/// Connects, proves possession of the pairing secret, verifies the board proved it too,
/// then completes the usual `bind` -> `hello` exchange. `expected_board` must be non-empty:
/// a Wi-Fi link is only ever opened to a board that was paired over USB.
pub fn connect_authenticated(
    addr: SocketAddr,
    secret: &str,
    desktop_device_id: &str,
    expected_board: &str,
) -> Result<AuthenticatedLink, String> {
    if expected_board.trim().is_empty() || secret.is_empty() {
        return Err("缺少配对信息，请先通过 USB 配置 WiFi".to_string());
    }
    let mut stream = TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT)
        .map_err(|error| format!("无法连接 {addr}: {error}"))?;
    let _ = stream.set_nodelay(true);
    let deadline = Instant::now() + HANDSHAKE_TIMEOUT;

    let (_, challenge) = read_topic(&mut stream, deadline, &["net/challenge"])?;
    let board = payload_str(&challenge, "boardDeviceId").to_string();
    let device_nonce = payload_str(&challenge, "nonce").to_string();
    if board != expected_board {
        // Nothing derived from the secret has been sent yet.
        return Err(format!("该地址上的设备是 {board}，不是已配对的 {expected_board}"));
    }
    if device_nonce.len() < 16 {
        return Err("设备返回了无效的挑战".to_string());
    }

    let client_nonce = random_hex(NONCE_BYTES);
    write_json_line(
        &mut stream,
        "net/auth",
        json!({
            "protocol": NETWORK_WIRE_PROTOCOL,
            "desktopDeviceId": desktop_device_id,
            "nonce": client_nonce,
            "proof": client_proof(secret, &board, &device_nonce, &client_nonce),
        }),
    )?;

    let (topic, reply) = read_topic(&mut stream, deadline, &["net/auth_ok", "net/auth_error"])?;
    if topic == "net/auth_error" {
        return Err(match payload_str(&reply, "error") {
            "locked" => "设备暂时拒绝认证，请稍后再试".to_string(),
            _ => "设备拒绝了配对凭据，请通过 USB 重新配置 WiFi".to_string(),
        });
    }
    let expected_server = server_proof(secret, &board, &device_nonce, &client_nonce);
    if !constant_time_eq(payload_str(&reply, "proof"), &expected_server) {
        return Err("设备未能证明其身份，已中止连接".to_string());
    }

    write_json_line(&mut stream, "bind", json!({"desktopDeviceId": desktop_device_id}))?;
    let (_, hello) = read_topic(&mut stream, deadline, &["hello"])?;
    if payload_str(&hello, "boardDeviceId") != board {
        return Err("hello 与认证的设备身份不一致".to_string());
    }

    stream
        .set_read_timeout(Some(NETWORK_READ_POLL))
        .map_err(|error| error.to_string())?;
    stream
        .set_write_timeout(Some(NETWORK_WRITE_TIMEOUT))
        .map_err(|error| error.to_string())?;
    Ok(AuthenticatedLink { stream, hello })
}

/// Parses `host`, `host:port`, or `[v6]:port` into a socket address, preferring IPv4.
pub fn resolve_endpoint(host: &str, port: u16) -> Result<SocketAddr, String> {
    let host = host.trim();
    if host.is_empty() {
        return Err("请输入设备地址".to_string());
    }
    let (host, port) = match host.rsplit_once(':') {
        Some((name, explicit)) if !name.contains(':') && explicit.chars().all(|c| c.is_ascii_digit()) => (
            name,
            explicit.parse::<u16>().map_err(|_| "端口无效".to_string())?,
        ),
        _ => (host.trim_matches(|c| c == '[' || c == ']'), port),
    };
    let mut candidates: Vec<SocketAddr> = (host, port)
        .to_socket_addrs()
        .map_err(|error| format!("无法解析设备地址 {host}: {error}"))?
        .collect();
    candidates.sort_by_key(|addr| !addr.is_ipv4());
    candidates
        .into_iter()
        .next()
        .ok_or_else(|| format!("无法解析设备地址 {host}"))
}

// ---------------------------------------------------------------------------
// UDP discovery
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct DiscoveredDevice {
    pub board_device_id: String,
    pub host: String,
    pub port: u16,
    pub firmware: String,
    /// Whether the board holds a pairing secret (says nothing about *this* desktop).
    pub paired: bool,
}

/// Sends one probe to each target and collects announcements until `timeout` elapses.
/// Announcements carry no secrets; identity is only trusted after `connect_authenticated`.
pub fn discover_via(targets: &[SocketAddr], timeout: Duration) -> Result<Vec<DiscoveredDevice>, String> {
    let socket = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).map_err(|error| format!("无法创建发现套接字: {error}"))?;
    socket.set_broadcast(true).map_err(|error| error.to_string())?;
    let nonce = random_hex(8);
    let probe = serde_json::to_vec(&json!({
        "topic": "net/discover",
        "payload": {"protocol": NETWORK_WIRE_PROTOCOL, "nonce": nonce},
    }))
    .map_err(|error| error.to_string())?;
    for target in targets {
        let _ = socket.send_to(&probe, target);
    }
    let deadline = Instant::now() + timeout;
    let mut found: Vec<DiscoveredDevice> = Vec::new();
    let mut buffer = [0u8; 1024];
    while let Some(remaining) = deadline.checked_duration_since(Instant::now()).filter(|d| !d.is_zero()) {
        socket
            .set_read_timeout(Some(remaining))
            .map_err(|error| error.to_string())?;
        let (len, from) = match socket.recv_from(&mut buffer) {
            Ok(received) => received,
            Err(error) if is_timeout(&error) => continue,
            Err(error) => return Err(format!("发现失败: {error}")),
        };
        let Ok(message) = serde_json::from_slice::<Value>(&buffer[..len]) else { continue };
        if message.get("topic").and_then(Value::as_str) != Some("net/announce") {
            continue;
        }
        let payload = message.get("payload").cloned().unwrap_or_else(|| json!({}));
        if payload_str(&payload, "nonce") != nonce || payload_str(&payload, "boardDeviceId").is_empty() {
            continue;
        }
        let device = DiscoveredDevice {
            board_device_id: payload_str(&payload, "boardDeviceId").to_string(),
            host: from.ip().to_string(),
            port: payload
                .get("port")
                .and_then(Value::as_u64)
                .and_then(|port| u16::try_from(port).ok())
                .filter(|port| *port != 0)
                .unwrap_or(NETWORK_PORT),
            firmware: payload_str(&payload, "fw").to_string(),
            paired: payload.get("paired").and_then(Value::as_bool).unwrap_or(false),
        };
        if !found.iter().any(|known| known.board_device_id == device.board_device_id) {
            found.push(device);
        }
    }
    Ok(found)
}

pub fn discover(timeout: Duration) -> Result<Vec<DiscoveredDevice>, String> {
    discover_via(
        &[SocketAddr::new(IpAddr::V4(Ipv4Addr::BROADCAST), NETWORK_PORT)],
        timeout,
    )
}

// ---------------------------------------------------------------------------
// Paired-device registry
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct PairedNetworkDevice {
    pub board_device_id: String,
    pub host: String,
    pub port: u16,
    #[serde(default)]
    pub updated_at_ms: u64,
}

pub trait SecretStore: Send + Sync {
    fn get(&self, board: &str) -> Result<Option<String>, String>;
    fn set(&self, board: &str, secret: &str) -> Result<(), String>;
    fn delete(&self, board: &str) -> Result<(), String>;
}

fn valid_board_id(board: &str) -> Result<&str, String> {
    let ok = !board.is_empty()
        && board.len() <= 64
        && board.bytes().all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_');
    if ok { Ok(board) } else { Err("设备标识无效".to_string()) }
}

/// Windows keeps pairing secrets in Credential Manager.
#[cfg(windows)]
struct KeyringSecrets;

#[cfg(windows)]
impl KeyringSecrets {
    fn entry(board: &str) -> Result<keyring::Entry, String> {
        keyring::Entry::new(KEYRING_SERVICE, valid_board_id(board)?)
            .map_err(|error| format!("系统凭据库不可用: {error}"))
    }
}

#[cfg(windows)]
impl SecretStore for KeyringSecrets {
    fn get(&self, board: &str) -> Result<Option<String>, String> {
        match Self::entry(board)?.get_password() {
            Ok(secret) => Ok(Some(secret)),
            Err(keyring::Error::NoEntry) => Ok(None),
            Err(error) => Err(format!("读取配对凭据失败: {error}")),
        }
    }
    fn set(&self, board: &str, secret: &str) -> Result<(), String> {
        Self::entry(board)?
            .set_password(secret)
            .map_err(|error| format!("保存配对凭据失败: {error}"))
    }
    fn delete(&self, board: &str) -> Result<(), String> {
        match Self::entry(board)?.delete_credential() {
            Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
            Err(error) => Err(format!("删除配对凭据失败: {error}")),
        }
    }
}

/// Other platforms follow the community-identity convention: a private 0700 directory with
/// one owner-only 0600 file per board, never inside the endpoint registry.
#[cfg(not(windows))]
struct FileSecrets {
    dir: PathBuf,
}

#[cfg(not(windows))]
impl FileSecrets {
    fn path(&self, board: &str) -> Result<PathBuf, String> {
        Ok(self.dir.join(format!("network-{}.secret", valid_board_id(board)?)))
    }

    fn ensure_dir(&self) -> Result<(), String> {
        std::fs::create_dir_all(&self.dir).map_err(|error| format!("无法创建凭据目录: {error}"))?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&self.dir, std::fs::Permissions::from_mode(0o700))
                .map_err(|error| format!("无法保护凭据目录: {error}"))?;
        }
        Ok(())
    }
}

#[cfg(not(windows))]
impl SecretStore for FileSecrets {
    fn get(&self, board: &str) -> Result<Option<String>, String> {
        match std::fs::read_to_string(self.path(board)?) {
            Ok(text) => {
                let secret = text.trim().to_string();
                if secret.len() == SECRET_BYTES * 2 && secret.bytes().all(|byte| byte.is_ascii_hexdigit()) {
                    Ok(Some(secret))
                } else {
                    Err("配对凭据已损坏，请通过 USB 重新配置 WiFi".to_string())
                }
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(error) => Err(format!("读取配对凭据失败: {error}")),
        }
    }

    fn set(&self, board: &str, secret: &str) -> Result<(), String> {
        let path = self.path(board)?;
        self.ensure_dir()?;
        let mut options = std::fs::OpenOptions::new();
        options.write(true).create(true).truncate(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        options
            .open(&path)
            .and_then(|mut file| file.write_all(secret.as_bytes()))
            .map_err(|error| format!("保存配对凭据失败: {error}"))
    }

    fn delete(&self, board: &str) -> Result<(), String> {
        match std::fs::remove_file(self.path(board)?) {
            Ok(()) => Ok(()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(format!("删除配对凭据失败: {error}")),
        }
    }
}

pub struct NetworkRegistry {
    path: PathBuf,
    secrets: Box<dyn SecretStore>,
}

impl NetworkRegistry {
    pub fn new(dir: &Path, secrets: Box<dyn SecretStore>) -> Self {
        Self { path: dir.join(REGISTRY_FILE), secrets }
    }

    pub fn list(&self) -> Vec<PairedNetworkDevice> {
        std::fs::read(&self.path)
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default()
    }

    fn save(&self, devices: &[PairedNetworkDevice]) -> Result<(), String> {
        if let Some(parent) = self.path.parent() {
            std::fs::create_dir_all(parent).map_err(|error| error.to_string())?;
        }
        let bytes = serde_json::to_vec_pretty(devices).map_err(|error| error.to_string())?;
        std::fs::write(&self.path, bytes).map_err(|error| format!("保存设备列表失败: {error}"))
    }

    /// Returns the board's pairing secret, creating one on first use. The secret never touches
    /// the registry file; it is sent to the board once, over USB, by the provisioning command.
    pub fn ensure_secret(&self, board: &str) -> Result<String, String> {
        if let Some(existing) = self.secrets.get(board)? {
            return Ok(existing);
        }
        let secret = generate_secret();
        self.secrets.set(board, &secret)?;
        Ok(secret)
    }

    pub fn secret(&self, board: &str) -> Result<Option<String>, String> {
        self.secrets.get(board)
    }

    pub fn remember_endpoint(&self, board: &str, host: &str, port: u16) -> Result<(), String> {
        let mut devices = self.list();
        let entry = PairedNetworkDevice {
            board_device_id: board.to_string(),
            host: host.to_string(),
            port,
            updated_at_ms: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|elapsed| elapsed.as_millis() as u64)
                .unwrap_or(0),
        };
        match devices.iter_mut().find(|known| known.board_device_id == board) {
            Some(known) => *known = entry,
            None => devices.push(entry),
        }
        self.save(&devices)
    }

    pub fn forget(&self, board: &str) -> Result<(), String> {
        self.secrets.delete(board)?;
        let mut devices = self.list();
        devices.retain(|known| known.board_device_id != board);
        self.save(&devices)
    }

    /// Boards that can be reconnected: an endpoint on file *and* a secret in the credential store.
    pub fn connectable(&self) -> Vec<(PairedNetworkDevice, String)> {
        self.list()
            .into_iter()
            .filter_map(|device| {
                let secret = self.secrets.get(&device.board_device_id).ok().flatten()?;
                Some((device, secret))
            })
            .collect()
    }
}

static REGISTRY: OnceLock<NetworkRegistry> = OnceLock::new();
static REGISTRY_LOCK: Mutex<()> = Mutex::new(());

pub fn configure(app_data_dir: &Path) {
    #[cfg(windows)]
    let secrets: Box<dyn SecretStore> = Box::new(KeyringSecrets);
    #[cfg(not(windows))]
    let secrets: Box<dyn SecretStore> = Box::new(FileSecrets { dir: app_data_dir.join("private") });
    let _ = REGISTRY.set(NetworkRegistry::new(app_data_dir, secrets));
}

pub fn registry() -> Result<&'static NetworkRegistry, String> {
    REGISTRY.get().ok_or_else(|| "WiFi 配对存储尚未初始化".to_string())
}

/// Serializes read-modify-write cycles on the registry file across commands and auto-connect.
pub fn with_registry<T>(action: impl FnOnce(&NetworkRegistry) -> Result<T, String>) -> Result<T, String> {
    let registry = registry()?;
    let _guard = REGISTRY_LOCK.lock().map_err(|_| "WiFi 配对存储已损坏".to_string())?;
    action(registry)
}

pub fn is_network_port_name(port_name: &str) -> bool {
    port_name.starts_with(NETWORK_PORT_PREFIX)
}

pub fn network_port_name(addr: &SocketAddr) -> String {
    format!("{NETWORK_PORT_PREFIX}{addr}")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::net::TcpListener;
    use std::sync::Arc;
    use std::thread;

    #[derive(Default)]
    struct MemorySecrets(Mutex<HashMap<String, String>>);

    impl SecretStore for MemorySecrets {
        fn get(&self, board: &str) -> Result<Option<String>, String> {
            Ok(self.0.lock().unwrap().get(board).cloned())
        }
        fn set(&self, board: &str, secret: &str) -> Result<(), String> {
            self.0.lock().unwrap().insert(board.to_string(), secret.to_string());
            Ok(())
        }
        fn delete(&self, board: &str) -> Result<(), String> {
            self.0.lock().unwrap().remove(board);
            Ok(())
        }
    }

    fn to_hex(bytes: &[u8]) -> String {
        hex_encode(bytes)
    }

    #[test]
    fn hmac_matches_rfc4231_vectors() {
        assert_eq!(
            to_hex(&hmac_sha256(&[0x0b; 20], b"Hi There")),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
        );
        assert_eq!(
            to_hex(&hmac_sha256(b"Jefe", b"what do ya want for nothing?")),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
        );
        // Keys longer than the block size are hashed first (RFC 4231 case 6).
        assert_eq!(
            to_hex(&hmac_sha256(
                &[0xaa; 131],
                b"Test Using Larger Than Block-Size Key - Hash Key First"
            )),
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"
        );
    }

    #[test]
    fn proofs_are_role_and_input_bound() {
        let secret = generate_secret();
        assert_eq!(secret.len(), SECRET_BYTES * 2);
        let client = client_proof(&secret, "p4-a", "dn", "cn");
        assert_ne!(client, server_proof(&secret, "p4-a", "dn", "cn"));
        assert_ne!(client, client_proof(&secret, "p4-b", "dn", "cn"));
        assert_ne!(client, client_proof(&secret, "p4-a", "dn2", "cn"));
        assert_ne!(client, client_proof(&generate_secret(), "p4-a", "dn", "cn"));
        assert!(constant_time_eq(&client, &client.clone()));
        assert!(!constant_time_eq(&client, &client[..client.len() - 1]));
    }

    #[test]
    fn endpoint_parsing_accepts_host_port_and_defaults() {
        assert_eq!(resolve_endpoint("127.0.0.1", 38517).unwrap(), "127.0.0.1:38517".parse().unwrap());
        assert_eq!(resolve_endpoint(" 127.0.0.1:4000 ", 38517).unwrap(), "127.0.0.1:4000".parse().unwrap());
        assert!(resolve_endpoint("", 38517).is_err());
        assert!(resolve_endpoint("127.0.0.1:99999", 38517).is_err());
    }

    /// Minimal board: the same HMAC contract the firmware implements.
    fn spawn_board(board: &'static str, secret: String, hello_board: &'static str) -> (SocketAddr, thread::JoinHandle<Vec<String>>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let handle = thread::spawn(move || {
            let mut seen = Vec::new();
            let (mut stream, _) = listener.accept().unwrap();
            let deadline = Instant::now() + Duration::from_secs(5);
            let device_nonce = "0123456789abcdef0123456789abcdef";
            write_json_line(&mut stream, "net/challenge", json!({"boardDeviceId": board, "nonce": device_nonce})).unwrap();
            let Ok((_, auth)) = read_topic(&mut stream, deadline, &["net/auth"]) else { return seen };
            seen.push("net/auth".to_string());
            let client_nonce = payload_str(&auth, "nonce").to_string();
            if !constant_time_eq(payload_str(&auth, "proof"), &client_proof(&secret, board, device_nonce, &client_nonce)) {
                let _ = write_json_line(&mut stream, "net/auth_error", json!({"error": "auth_failed"}));
                return seen;
            }
            write_json_line(&mut stream, "net/auth_ok", json!({"proof": server_proof(&secret, board, device_nonce, &client_nonce)})).unwrap();
            let Ok((_, bind)) = read_topic(&mut stream, deadline, &["bind"]) else { return seen };
            seen.push(format!("bind:{}", payload_str(&bind, "desktopDeviceId")));
            write_json_line(&mut stream, "hello", json!({"boardDeviceId": hello_board, "runtime": "esp-p4"})).unwrap();
            // Bytes sent right after the handshake must survive for the buffered reader.
            stream.write_all(b"{\"topic\":\"input/event\",\"payload\":{}}\n").unwrap();
            thread::sleep(Duration::from_millis(200));
            seen
        });
        (addr, handle)
    }

    #[test]
    fn handshake_authenticates_both_sides_and_preserves_following_bytes() {
        let secret = generate_secret();
        let (addr, board) = spawn_board("p4-aa", secret.clone(), "p4-aa");
        let mut link = connect_authenticated(addr, &secret, "desk-1", "p4-aa").unwrap();
        assert_eq!(link.hello["boardDeviceId"], "p4-aa");
        let mut rest = String::new();
        let mut buf = [0u8; 64];
        let deadline = Instant::now() + Duration::from_secs(2);
        while !rest.ends_with('\n') && Instant::now() < deadline {
            if let Ok(n) = link.stream.read(&mut buf) { rest.push_str(&String::from_utf8_lossy(&buf[..n])); }
        }
        assert!(rest.contains("input/event"));
        assert_eq!(board.join().unwrap(), vec!["net/auth", "bind:desk-1"]);
    }

    #[test]
    fn handshake_rejects_wrong_secret_without_hello() {
        let (addr, board) = spawn_board("p4-aa", generate_secret(), "p4-aa");
        let error = connect_authenticated(addr, &generate_secret(), "desk-1", "p4-aa").err().unwrap();
        assert!(error.contains("拒绝"), "{error}");
        assert_eq!(board.join().unwrap(), vec!["net/auth"]);
    }

    #[test]
    fn handshake_refuses_a_board_that_is_not_the_paired_one_before_sending_a_proof() {
        let secret = generate_secret();
        let (addr, board) = spawn_board("p4-other", secret.clone(), "p4-other");
        let error = connect_authenticated(addr, &secret, "desk-1", "p4-aa").err().unwrap();
        assert!(error.contains("p4-other"), "{error}");
        assert!(board.join().unwrap().is_empty(), "no proof may be sent to an unexpected board");
    }

    #[test]
    fn handshake_rejects_a_board_that_cannot_prove_its_identity() {
        let secret = generate_secret();
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let impostor = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            write_json_line(&mut stream, "net/challenge", json!({"boardDeviceId": "p4-aa", "nonce": "0123456789abcdef0123456789abcdef"})).unwrap();
            let _ = read_topic(&mut stream, Instant::now() + Duration::from_secs(5), &["net/auth"]);
            write_json_line(&mut stream, "net/auth_ok", json!({"proof": "00"})).unwrap();
            thread::sleep(Duration::from_millis(200));
        });
        let error = connect_authenticated(addr, &secret, "desk-1", "p4-aa").err().unwrap();
        assert!(error.contains("证明"), "{error}");
        impostor.join().unwrap();
    }

    #[test]
    fn handshake_rejects_hello_for_a_different_board() {
        let secret = generate_secret();
        let (addr, board) = spawn_board("p4-aa", secret.clone(), "p4-zz");
        let error = connect_authenticated(addr, &secret, "desk-1", "p4-aa").err().unwrap();
        assert!(error.contains("hello"), "{error}");
        let _ = board.join();
    }

    #[test]
    fn connect_requires_pairing_material() {
        let addr: SocketAddr = "127.0.0.1:9".parse().unwrap();
        assert!(connect_authenticated(addr, "", "desk", "p4-aa").is_err());
        assert!(connect_authenticated(addr, "secret", "desk", "").is_err());
    }

    #[test]
    fn discovery_collects_matching_announcements_only() {
        let responder = UdpSocket::bind("127.0.0.1:0").unwrap();
        let target = responder.local_addr().unwrap();
        let board = thread::spawn(move || {
            let mut buf = [0u8; 512];
            let (len, from) = responder.recv_from(&mut buf).unwrap();
            let probe: Value = serde_json::from_slice(&buf[..len]).unwrap();
            let nonce = probe["payload"]["nonce"].as_str().unwrap().to_string();
            let reply = |nonce: &str, board: &str| serde_json::to_vec(&json!({"topic": "net/announce", "payload": {
                "boardDeviceId": board, "nonce": nonce, "port": 38517, "fw": "0.7.104-p4", "paired": true}})).unwrap();
            responder.send_to(&reply("stale", "p4-ghost"), from).unwrap();
            responder.send_to(&reply(&nonce, "p4-aa"), from).unwrap();
            responder.send_to(&reply(&nonce, "p4-aa"), from).unwrap();
        });
        let found = discover_via(&[target], Duration::from_millis(600)).unwrap();
        board.join().unwrap();
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].board_device_id, "p4-aa");
        assert_eq!(found[0].host, "127.0.0.1");
        assert!(found[0].paired);
    }

    #[test]
    fn registry_keeps_secrets_out_of_the_endpoint_file() {
        let dir = std::env::temp_dir().join(format!("pet-net-registry-{}", random_hex(6)));
        let secrets = Arc::new(MemorySecrets::default());
        struct Shared(Arc<MemorySecrets>);
        impl SecretStore for Shared {
            fn get(&self, b: &str) -> Result<Option<String>, String> { self.0.get(b) }
            fn set(&self, b: &str, s: &str) -> Result<(), String> { self.0.set(b, s) }
            fn delete(&self, b: &str) -> Result<(), String> { self.0.delete(b) }
        }
        let registry = NetworkRegistry::new(&dir, Box::new(Shared(secrets.clone())));
        let secret = registry.ensure_secret("p4-aa").unwrap();
        assert_eq!(registry.ensure_secret("p4-aa").unwrap(), secret, "secret is stable per board");
        assert!(registry.connectable().is_empty(), "no endpoint yet");
        registry.remember_endpoint("p4-aa", "192.168.1.9", 38517).unwrap();
        registry.remember_endpoint("p4-aa", "192.168.1.10", 38517).unwrap();
        let connectable = registry.connectable();
        assert_eq!(connectable.len(), 1);
        assert_eq!(connectable[0].0.host, "192.168.1.10", "address change replaces the entry");
        assert_eq!(connectable[0].1, secret);
        let on_disk = std::fs::read_to_string(dir.join(REGISTRY_FILE)).unwrap();
        assert!(!on_disk.contains(&secret));
        registry.forget("p4-aa").unwrap();
        assert!(registry.list().is_empty());
        assert!(secrets.get("p4-aa").unwrap().is_none());
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn only_status_session_and_key_traffic_is_allowed_over_wifi() {
        for topic in ["state/active", "session/current", "speech/text", "system/heartbeat", "bind",
                      "control/screen-page", "stats/update", "diagnostics/query", "network/status"] {
            assert!(topic_allowed_over_network(topic), "{topic}");
        }
        for topic in ["asset/chunk", "asset/raw-chunk", "firmware/begin", "audio/conversation",
                      "media/begin", "widget/data", "widget/chunk", "app/snapshot",
                      "ui/conversation", "input/config", "debug/screenshot", "miniapp/event"] {
            assert!(!topic_allowed_over_network(topic), "{topic}");
        }
    }

    #[test]
    fn dropping_the_writer_closes_the_peer() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let writer = LinkWriter(TcpStream::connect(addr).unwrap());
        let (mut peer, _) = listener.accept().unwrap();
        drop(writer);
        peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let mut buf = [0u8; 1];
        assert_eq!(peer.read(&mut buf).unwrap(), 0, "peer sees EOF once the writer is dropped");
    }

    #[cfg(unix)]
    #[test]
    fn file_secrets_are_owner_only_and_reject_unsafe_board_ids() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("pet-net-secrets-{}", random_hex(6)));
        let store = FileSecrets { dir: dir.join("private") };
        let secret = generate_secret();
        assert_eq!(store.get("p4-aa").unwrap(), None);
        store.set("p4-aa", &secret).unwrap();
        assert_eq!(store.get("p4-aa").unwrap().as_deref(), Some(secret.as_str()));
        let file = dir.join("private/network-p4-aa.secret");
        assert_eq!(std::fs::metadata(&file).unwrap().permissions().mode() & 0o777, 0o600);
        assert_eq!(std::fs::metadata(dir.join("private")).unwrap().permissions().mode() & 0o777, 0o700);
        assert!(store.set("../escape", &secret).is_err());
        assert!(store.get("a/b").is_err());
        std::fs::write(&file, "not-a-secret").unwrap();
        assert!(store.get("p4-aa").is_err(), "a corrupted secret is reported, not silently replaced");
        store.delete("p4-aa").unwrap();
        assert_eq!(store.get("p4-aa").unwrap(), None);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn network_port_names_round_trip() {
        let addr: SocketAddr = "192.168.1.9:38517".parse().unwrap();
        let name = network_port_name(&addr);
        assert_eq!(name, "wifi://192.168.1.9:38517");
        assert!(is_network_port_name(&name));
        assert!(!is_network_port_name("/dev/cu.usbserial-1"));
    }
}
