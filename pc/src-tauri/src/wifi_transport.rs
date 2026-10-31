/*
 * [Input] Device TCP endpoint (default port 3232), the firmware JSONL protocol,
 *         and an optional message handler supplied by the Tauri command layer.
 * [Output] A single Wi-Fi TCP connection with hello-derived status, line reader,
 *         JSON topic send/receive, disconnect handling, and unit-testable behavior.
 * [Pos] Optional PC network transport beneath the existing USB serial manager.
 * [Sync] If this file changes, update pc/.folder.md and wifi integration docs.
 */

use serde::Serialize;
use serde_json::Value;
use std::io::{BufRead, BufReader, ErrorKind, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::thread::JoinHandle;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

pub type WifiMessageHandler = Arc<dyn Fn(&str, &Value) + Send + Sync>;

#[derive(Clone, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct WifiConnectionStatus {
    pub connected: bool,
    pub ip: String,
    pub port: u16,
    pub board_device_id: String,
    pub firmware: String,
    pub runtime: String,
    pub last_error: Option<String>,
    pub last_message_at_ms: u64,
}

struct ActiveConnection {
    cancel: Arc<AtomicBool>,
    writer: Arc<Mutex<TcpStream>>,
    reader: Option<JoinHandle<()>>,
    heartbeat: Option<JoinHandle<()>>,
}

pub struct WifiTransport {
    active: Mutex<Option<ActiveConnection>>,
    status: Arc<Mutex<WifiConnectionStatus>>,
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis() as u64)
        .unwrap_or(0)
}

fn set_error(status: &Mutex<WifiConnectionStatus>, error: impl Into<String>) {
    if let Ok(mut status) = status.lock() {
        status.last_error = Some(error.into());
        status.connected = false;
    }
}

impl WifiTransport {
    pub fn new() -> Self {
        Self {
            active: Mutex::new(None),
            status: Arc::new(Mutex::new(WifiConnectionStatus::default())),
        }
    }

    pub fn status(&self) -> WifiConnectionStatus {
        self.status.lock().map(|status| status.clone()).unwrap_or_default()
    }

    pub fn connect(
        &self,
        ip: &str,
        port: u16,
        handler: WifiMessageHandler,
    ) -> Result<WifiConnectionStatus, String> {
        let ip = ip.trim();
        if ip.is_empty() {
            return Err("Wi-Fi 设备 IP 为空".into());
        }
        if port == 0 {
            return Err("Wi-Fi TCP 端口无效".into());
        }
        self.disconnect();

        let address: SocketAddr = (ip, port)
            .to_socket_addrs()
            .map_err(|error| format!("解析 Wi-Fi 设备地址失败：{error}"))?
            .next()
            .ok_or_else(|| "没有可用的 Wi-Fi 设备地址".to_string())?;
        let stream = TcpStream::connect_timeout(&address, Duration::from_secs(5))
            .map_err(|error| format!("连接 Wi-Fi 设备失败：{error}"))?;
        stream
            .set_read_timeout(Some(Duration::from_millis(600)))
            .map_err(|error| format!("设置 Wi-Fi 读超时失败：{error}"))?;
        stream
            .set_write_timeout(Some(Duration::from_secs(2)))
            .map_err(|error| format!("设置 Wi-Fi 写超时失败：{error}"))?;
        let writer = stream
            .try_clone()
            .map_err(|error| format!("复制 Wi-Fi socket 失败：{error}"))?;
        let cancel = Arc::new(AtomicBool::new(false));

        {
            let mut status = self.status.lock().map_err(|_| "Wi-Fi 状态锁异常")?;
            *status = WifiConnectionStatus {
                connected: true,
                ip: address.ip().to_string(),
                port,
                ..WifiConnectionStatus::default()
            };
        }

        let status = Arc::clone(&self.status);
        let cancel_reader = Arc::clone(&cancel);
        let reader = thread::spawn(move || {
            let mut reader = BufReader::new(stream);
            let mut line = String::new();
            loop {
                if cancel_reader.load(Ordering::SeqCst) {
                    break;
                }
                line.clear();
                match reader.read_line(&mut line) {
                    Ok(0) => break,
                    Ok(_) => {
                        let text = line.trim();
                        if text.is_empty() {
                            continue;
                        }
                        match serde_json::from_str::<Value>(text) {
                            Ok(message) => {
                                let topic = message
                                    .get("topic")
                                    .and_then(Value::as_str)
                                    .unwrap_or_default()
                                    .to_string();
                                let payload = message
                                    .get("payload")
                                    .cloned()
                                    .unwrap_or(Value::Null);
                                if let Ok(mut current) = status.lock() {
                                    current.last_message_at_ms = now_ms();
                                    current.last_error = None;
                                    if topic == "hello" {
                                        current.board_device_id = payload
                                            .get("boardDeviceId")
                                            .and_then(Value::as_str)
                                            .unwrap_or_default()
                                            .to_string();
                                        current.firmware = payload
                                            .get("fw")
                                            .and_then(Value::as_str)
                                            .unwrap_or_default()
                                            .to_string();
                                        current.runtime = payload
                                            .get("runtime")
                                            .and_then(Value::as_str)
                                            .unwrap_or_default()
                                            .to_string();
                                    }
                                }
                                if !topic.is_empty() {
                                    handler(&topic, &payload);
                                }
                            }
                            Err(error) => {
                                if let Ok(mut current) = status.lock() {
                                    current.last_error =
                                        Some(format!("解析 Wi-Fi 消息失败：{error}"));
                                }
                            }
                        }
                    }
                    Err(error)
                        if matches!(error.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) =>
                    {
                        continue;
                    }
                    Err(error) => {
                        set_error(&status, format!("Wi-Fi 读取失败：{error}"));
                        break;
                    }
                }
            }
            if let Ok(mut current) = status.lock() {
                current.connected = false;
            }
        });

        let writer = Arc::new(Mutex::new(writer));
        let heartbeat_writer = Arc::clone(&writer);
        let heartbeat_cancel = Arc::clone(&cancel);
        let heartbeat = thread::spawn(move || {
            // Keep an otherwise idle TCP link alive; firmware also uses this
            // traffic to distinguish an abandoned client from a live PC.
            let mut ticks = 0u32;
            loop {
                if heartbeat_cancel.load(Ordering::SeqCst) {
                    break;
                }
                thread::sleep(Duration::from_millis(250));
                ticks += 1;
                if ticks < 20 {
                    continue;
                }
                ticks = 0;
                let line = "{\"topic\":\"wifi/status\",\"payload\":{}}\n";
                let Ok(mut stream) = heartbeat_writer.lock() else {
                    break;
                };
                if stream
                    .write_all(line.as_bytes())
                    .and_then(|_| stream.flush())
                    .is_err()
                {
                    break;
                }
            }
        });

        *self.active.lock().map_err(|_| "Wi-Fi 连接锁异常")? = Some(ActiveConnection {
            cancel,
            writer,
            reader: Some(reader),
            heartbeat: Some(heartbeat),
        });
        Ok(self.status())
    }

    pub fn disconnect(&self) {
        let active = self
            .active
            .lock()
            .ok()
            .and_then(|mut active| active.take());
        if let Some(mut active) = active {
            active.cancel.store(true, Ordering::SeqCst);
            if let Ok(writer) = active.writer.lock() {
                let _ = writer.shutdown(std::net::Shutdown::Both);
            }
            if let Some(reader) = active.reader.take() {
                let _ = reader.join();
            }
            if let Some(heartbeat) = active.heartbeat.take() {
                let _ = heartbeat.join();
            }
        }
        if let Ok(mut status) = self.status.lock() {
            status.connected = false;
        }
    }

    pub fn send(&self, topic: &str, payload: &Value) -> Result<(), String> {
        if topic.trim().is_empty() {
            return Err("Wi-Fi topic 为空".into());
        }
        let writer = {
            let active = self.active.lock().map_err(|_| "Wi-Fi 连接锁异常")?;
            active
                .as_ref()
                .map(|connection| Arc::clone(&connection.writer))
                .ok_or_else(|| "Wi-Fi TCP 未连接".to_string())?
        };
        let line = serde_json::json!({"topic": topic, "payload": payload}).to_string();
        let mut writer = writer.lock().map_err(|_| "Wi-Fi 写锁异常")?;
        writer
            .write_all(line.as_bytes())
            .and_then(|_| writer.write_all(b"\n"))
            .and_then(|_| writer.flush())
            .map_err(|error| format!("Wi-Fi 发送失败：{error}"))
    }
}

impl Default for WifiTransport {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;
    use std::net::TcpListener;
    use std::sync::mpsc;

    #[test]
    fn connects_reads_hello_and_sends_jsonl() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).expect("bind");
        let port = listener.local_addr().expect("addr").port();
        let (tx, rx) = mpsc::channel();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept");
            stream
                .write_all(b"{\"topic\":\"hello\",\"payload\":{\"boardDeviceId\":\"p4-test\",\"fw\":\"1.2.3\",\"runtime\":\"esp-p4\"}}\n")
                .expect("hello");
            let mut buf = [0u8; 512];
            let n = stream.read(&mut buf).expect("read");
            tx.send(String::from_utf8_lossy(&buf[..n]).to_string())
                .expect("send");
        });

        let transport = WifiTransport::new();
        let (message_tx, message_rx) = mpsc::channel();
        let handler: WifiMessageHandler = Arc::new(move |topic: &str, payload: &Value| {
            let _ = message_tx.send((
                topic.to_string(),
                payload.get("boardDeviceId").and_then(Value::as_str).unwrap_or_default().to_string(),
            ));
        });
        let status = transport
            .connect("127.0.0.1", port, handler)
            .expect("connect");
        assert!(status.connected);
        assert_eq!(status.port, port);

        let (topic, board) = message_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("hello message");
        assert_eq!(topic, "hello");
        assert_eq!(board, "p4-test");
        let status = transport.status();
        assert_eq!(status.board_device_id, "p4-test");
        assert_eq!(status.firmware, "1.2.3");

        transport
            .send("wifi/status", &serde_json::json!({}))
            .expect("send");
        let received = rx.recv_timeout(Duration::from_secs(2)).expect("server recv");
        assert!(received.contains("\"topic\":\"wifi/status\""));
        transport.disconnect();
        assert!(!transport.status().connected);
        server.join().expect("join");
    }
}
