/*
 * [Input] serialport-enumerated CDC USB ports plus JSON-line OTA/state payloads.
 * [Output] USB serial manager for device handshake, state/speech forwarding,
 *          serialized transactional appearance video/WAV cue asset OTA with checksum acks,
 *          audio-only patch commits, legacy full-sync fallback for boards that
 *          do not support per-file asset acks yet, and widget .clawpkg OTA; macOS scans prefer /dev/cu.* callout
 *          ports to avoid blocking /dev/tty.* opens, and reconnects cancel stale
 *          reader clones before reopening the port.
 * [Pos] Tauri USB transport node in ref/src-tauri/src
 * [Sync] If this file changes, update `ref/.folder.md`.
 */

use base64::Engine as _;
use serde::{Deserialize, Serialize};
use serialport::SerialPortType;
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, BufReader, BufWriter, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::thread;
use std::time::Duration;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UsbDeviceInfo {
    pub port_name: String,
    pub vid: u16,
    pub pid: u16,
    pub serial_number: String,
    pub manufacturer: String,
    pub product: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UsbConnectionStatus {
    pub connected: bool,
    pub port_name: String,
    pub board_device_id: String,
    pub transport: String,
}

#[derive(Debug, Deserialize)]
struct SerialMessage {
    topic: String,
    payload: serde_json::Value,
}

struct UsbConnection {
    connection_id: u64,
    port_name: String,
    writer: Box<dyn Write + Send>,
    board_device_id: String,
    connected: bool,
    cancel_reader: Arc<AtomicBool>,
}

const APPEARANCE_ASSET_CHUNK_SIZE: usize = 49_152;
const APPEARANCE_ASSET_SERIAL_BYTES_PER_SEC: u64 = 80_000;
const APPEARANCE_ASSET_CHUNK_DELAY_FLOOR_MS: u64 = 35;
const APPEARANCE_ASSET_CHUNK_DELAY_MARGIN_MS: u64 = 25;
const ASSET_ACK_TIMEOUT: Duration = Duration::from_secs(45);
const ASSET_STAT_TIMEOUT: Duration = Duration::from_secs(2);
const FNV1A64_OFFSET: u64 = 0xcbf29ce484222325;
const FNV1A64_PRIME: u64 = 0x00000100000001b3;

struct AssetAckWaiter {
    transfer_id: String,
    phase: String,
    path: Option<String>,
    sender: mpsc::Sender<serde_json::Value>,
}

struct WidgetAckWaiter {
    transfer_id: String,
    phase: String, // "begin" | "commit"
    sender: mpsc::Sender<serde_json::Value>,
}

/// Widget OTA ack waits. begin should arrive ~50ms after send on a healthy
/// board (mkdir staging); commit ack arrives after base64-decode + rename of
/// the whole staging dir, which on the CPU-saturated Zero 2 W can take a few
/// hundred ms. Both have generous fall-through: on timeout, we log and
/// proceed as if it succeeded, so an older board (which only sent the bare
/// {"type":"widget_install_ack"} form that the host's SerialMessage parser
/// silently dropped) doesn't regress to "stuck spinning forever". A new board
/// will reply within tens-to-hundreds of ms; an old board will look like a
/// 2s + 5s degradation, which the user can fix by redeploying board-server.
const WIDGET_BEGIN_ACK_TIMEOUT: Duration = Duration::from_secs(2);
const WIDGET_COMMIT_ACK_TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Clone)]
pub struct UsbSerialManager {
    connection: Arc<Mutex<Option<UsbConnection>>>,
    desktop_device_id: Arc<Mutex<String>>,
    connect_guard: Arc<Mutex<()>>,
    asset_transfer_guard: Arc<Mutex<()>>,
    next_connection_id: Arc<AtomicU64>,
    asset_ack_waiters: Arc<Mutex<Vec<AssetAckWaiter>>>,
    widget_ack_waiters: Arc<Mutex<Vec<WidgetAckWaiter>>>,
}

impl UsbSerialManager {
    pub fn new() -> Self {
        Self {
            connection: Arc::new(Mutex::new(None)),
            desktop_device_id: Arc::new(Mutex::new(String::new())),
            connect_guard: Arc::new(Mutex::new(())),
            asset_transfer_guard: Arc::new(Mutex::new(())),
            next_connection_id: Arc::new(AtomicU64::new(0)),
            asset_ack_waiters: Arc::new(Mutex::new(Vec::new())),
            widget_ack_waiters: Arc::new(Mutex::new(Vec::new())),
        }
    }

    pub fn set_desktop_device_id(&self, id: &str) {
        if let Ok(mut did) = self.desktop_device_id.lock() {
            *did = id.to_string();
        }
    }

    /// Scan for CDC-ACM USB serial devices
    pub fn scan_devices(&self) -> Vec<UsbDeviceInfo> {
        let ports = match serialport::available_ports() {
            Ok(ports) => ports,
            Err(_) => return Vec::new(),
        };

        let devices = ports
            .into_iter()
            .filter_map(|port| {
                if let SerialPortType::UsbPort(info) = &port.port_type {
                    // Match CDC-ACM / USB gadget and common USB-UART serial devices:
                    // - macOS CDC: port name contains "usbmodem"
                    // - macOS USB-UART: port name contains "usbserial" / "SLAB_USBtoUART"
                    // - Linux: port name contains "ttyACM" / "ttyUSB"
                    // - VID 0x1d6b = Linux Foundation (configfs gadget)
                    // - VID 0x0525 = Netchip/PLX (g_serial default)
                    let port_name = &port.port_name;
                    if is_supported_usb_serial_port(port_name, info.vid) {
                        return Some(UsbDeviceInfo {
                            port_name: port.port_name.clone(),
                            vid: info.vid,
                            pid: info.pid,
                            serial_number: info.serial_number.clone().unwrap_or_default(),
                            manufacturer: info.manufacturer.clone().unwrap_or_default(),
                            product: info.product.clone().unwrap_or_default(),
                        });
                    }
                }
                None
            })
            .collect();
        prefer_callout_ports_for_macos(devices)
    }

    /// Connect to a USB serial device
    pub fn connect<F>(&self, port_name: &str, on_message: F) -> Result<(), String>
    where
        F: Fn(String, serde_json::Value) + Send + 'static,
    {
        // Serialize connect/disconnect operations across background auto-connect,
        // UI polling, and manual setup flow to avoid COM-port races.
        let _guard = self.connect_guard.lock().map_err(|e| e.to_string())?;

        {
            let conn = self.connection.lock().map_err(|e| e.to_string())?;
            if let Some(existing) = conn.as_ref() {
                if existing.connected && existing.port_name.eq_ignore_ascii_case(port_name) {
                    return Ok(());
                }
            }
        }

        {
            let mut conn = self.connection.lock().map_err(|e| e.to_string())?;
            if let Some(existing) = conn.as_mut() {
                existing.cancel_reader.store(true, Ordering::SeqCst);
            }
            *conn = None;
        }

        // Use highest baud for CDC-ACM — macOS driver throttles based on baud setting
        let mut last_open_error = String::new();
        let mut maybe_port = None;
        for attempt in 0..6 {
            match serialport::new(port_name, 921_600)
                .timeout(Duration::from_millis(100))
                .open()
            {
                Ok(port) => {
                    maybe_port = Some(port);
                    break;
                }
                Err(e) => {
                    last_open_error = e.to_string();
                    let is_transient_lock = last_open_error.contains("Access is denied")
                        || last_open_error.contains("Unable to acquire exclusive lock")
                        || last_open_error.contains("Resource busy")
                        || last_open_error.contains("Device busy")
                        || last_open_error.contains("拒绝访问");
                    if is_transient_lock && attempt < 5 {
                        thread::sleep(Duration::from_millis(220));
                        continue;
                    }
                    return Err(format!("Failed to open {}: {}", port_name, e));
                }
            }
        }
        let mut port = maybe_port
            .ok_or_else(|| format!("Failed to open {}: {}", port_name, last_open_error))?;

        // Windows CDC serial often needs explicit DTR/RTS assertion to start
        // bidirectional traffic reliably.
        let _ = port.write_data_terminal_ready(true);
        let _ = port.write_request_to_send(true);

        let reader_port = port
            .try_clone()
            .map_err(|e| format!("Clone failed: {}", e))?;
        let connection_id = self.next_connection_id.fetch_add(1, Ordering::SeqCst) + 1;
        let cancel_reader = Arc::new(AtomicBool::new(false));

        let connection = UsbConnection {
            connection_id,
            port_name: port_name.to_string(),
            writer: Box::new(BufWriter::with_capacity(256 * 1024, port)),
            board_device_id: String::new(),
            connected: true,
            cancel_reader: Arc::clone(&cancel_reader),
        };

        {
            let mut conn = self.connection.lock().map_err(|e| e.to_string())?;
            *conn = Some(connection);
        }

        // Send immediate ack so device knows a new host is connected
        // (device may have peer_acked=true from a previous host)
        {
            let desktop_id = self
                .desktop_device_id
                .lock()
                .map(|d| d.clone())
                .unwrap_or_default();
            let ack = format!(
                "{{\"topic\":\"ack\",\"payload\":{{\"desktopDeviceId\":\"{}\"}}}}\n",
                desktop_id
            );
            if let Ok(mut conn) = self.connection.lock() {
                if let Some(ref mut c) = *conn {
                    let _ = c.writer.write_all(ack.as_bytes());
                    let _ = c.writer.flush();
                }
            }
        }

        // Start reader thread
        let conn_ref = Arc::clone(&self.connection);
        let desktop_id_ref = Arc::clone(&self.desktop_device_id);
        let asset_ack_waiters = Arc::clone(&self.asset_ack_waiters);
        let widget_ack_waiters = Arc::clone(&self.widget_ack_waiters);

        thread::spawn(move || {
            let reader = BufReader::new(reader_port);
            for line in reader.lines() {
                if cancel_reader.load(Ordering::SeqCst) {
                    break;
                }
                let line = match line {
                    Ok(l) => l,
                    Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {
                        if cancel_reader.load(Ordering::SeqCst) {
                            break;
                        }
                        continue;
                    }
                    Err(e) => {
                        eprintln!("[usb_serial] read error: {} (kind={:?})", e, e.kind());
                        break;
                    }
                };

                if line.trim().is_empty() {
                    continue;
                }

                let msg: SerialMessage = match serde_json::from_str(&line) {
                    Ok(m) => m,
                    Err(e) => {
                        eprintln!("[usb_serial] invalid JSON: {}", e);
                        continue;
                    }
                };

                resolve_asset_ack(&asset_ack_waiters, &msg.topic, &msg.payload);
                resolve_widget_ack(&widget_ack_waiters, &msg.topic, &msg.payload);

                // Handle hello -> send ack
                if msg.topic == "hello" {
                    if let Some(board_id) =
                        msg.payload.get("boardDeviceId").and_then(|v| v.as_str())
                    {
                        if let Ok(mut conn) = conn_ref.lock() {
                            if let Some(ref mut c) = *conn {
                                if c.connection_id == connection_id {
                                    c.board_device_id = board_id.to_string();
                                }
                            }
                        }
                    }

                    let desktop_id = desktop_id_ref.lock().map(|d| d.clone()).unwrap_or_default();

                    let ack = format!(
                        "{{\"topic\":\"ack\",\"payload\":{{\"desktopDeviceId\":\"{}\"}}}}\n",
                        desktop_id
                    );
                    if let Ok(mut conn) = conn_ref.lock() {
                        if let Some(ref mut c) = *conn {
                            if c.connection_id == connection_id {
                                let _ = c.writer.write_all(ack.as_bytes());
                                let _ = c.writer.flush();
                            }
                        }
                    }
                }

                on_message(msg.topic, msg.payload);
            }

            // Reader thread ended -> mark disconnected
            if let Ok(mut conn) = conn_ref.lock() {
                if let Some(ref mut c) = *conn {
                    if c.connection_id == connection_id {
                        c.connected = false;
                    }
                }
            }
        });

        Ok(())
    }

    /// Disconnect from USB serial device
    pub fn disconnect(&self) {
        if let Ok(_guard) = self.connect_guard.lock() {
            if let Ok(mut conn) = self.connection.lock() {
                if let Some(existing) = conn.as_mut() {
                    existing.cancel_reader.store(true, Ordering::SeqCst);
                }
                *conn = None;
            }
        }
    }

    /// Send a message to the device
    pub fn send(&self, topic: &str, payload: &serde_json::Value) -> Result<(), String> {
        self.send_inner(topic, payload, true)
    }

    /// Send without flush — for streaming bulk data
    fn send_no_flush(&self, topic: &str, payload: &serde_json::Value) -> Result<(), String> {
        self.send_inner(topic, payload, false)
    }

    fn send_inner(
        &self,
        topic: &str,
        payload: &serde_json::Value,
        flush: bool,
    ) -> Result<(), String> {
        let mut conn = self.connection.lock().map_err(|e| e.to_string())?;
        let conn = conn.as_mut().ok_or("Not connected")?;

        if !conn.connected {
            return Err("Connection lost".to_string());
        }

        let msg = serde_json::json!({
            "topic": topic,
            "payload": payload,
        });

        let mut line = serde_json::to_string(&msg).map_err(|e| e.to_string())?;
        line.push('\n');

        conn.writer
            .write_all(line.as_bytes())
            .map_err(|e| format!("Write failed: {}", e))?;
        if flush {
            conn.writer
                .flush()
                .map_err(|e| format!("Flush failed: {}", e))?;
        }

        Ok(())
    }

    /// Flush the writer
    pub fn flush(&self) -> Result<(), String> {
        let mut conn = self.connection.lock().map_err(|e| e.to_string())?;
        let conn = conn.as_mut().ok_or("Not connected")?;
        conn.writer
            .flush()
            .map_err(|e| format!("Flush failed: {}", e))
    }

    /// Send a state update to the device
    pub fn send_state(&self, source: &str, payload: &serde_json::Value) -> Result<(), String> {
        let topic = format!("state/{}", source);
        self.send(&topic, payload)
    }

    /// Send speech text to the device
    pub fn send_speech(&self, text: &str) -> Result<(), String> {
        let payload = serde_json::json!({"text": text});
        self.send("speech/text", &payload)
    }

    /// Send a control command to the device
    pub fn send_command(&self, command: &str) -> Result<(), String> {
        let payload = serde_json::json!({"command": command});
        self.send("control/command", &payload)
    }

    /// Send asset_begin with ack — the unchecked `send_asset_begin` was
    /// removed 2026-06-01; the checked variant is the sole entry point.
    fn send_asset_begin_checked(&self, transfer_id: &str) -> Result<(), String> {
        let payload = serde_json::json!({"transferId": transfer_id});
        self.send_asset_command_and_wait("asset/begin", &payload, transfer_id, "begin", None)?;
        Ok(())
    }

    /// Send asset_chunk: a base64-encoded file chunk
    pub fn send_asset_chunk(
        &self,
        transfer_id: &str,
        path: &str,
        data_base64: &str,
        index: u32,
    ) -> Result<(), String> {
        let payload = serde_json::json!({
            "transferId": transfer_id,
            "path": path,
            "data": data_base64,
            "index": index.to_string()
        });
        self.send_no_flush("asset/chunk", &payload)
    }

    fn send_asset_file_commit_checked(
        &self,
        transfer_id: &str,
        path: &str,
        size: u64,
        checksum: &str,
        chunk_count: u64,
    ) -> Result<(), String> {
        let payload =
            build_asset_file_commit_payload(transfer_id, path, size, checksum, chunk_count);
        self.send_asset_command_and_wait("asset/file", &payload, transfer_id, "file", Some(path))?;
        Ok(())
    }

    fn send_asset_commit_checked(
        &self,
        transfer_id: &str,
        file_count: u32,
        byte_count: u64,
    ) -> Result<(), String> {
        let payload = serde_json::json!({
            "transferId": transfer_id,
            "fileCount": file_count,
            "totalBytes": byte_count,
        });
        self.send_asset_command_and_wait("asset/commit", &payload, transfer_id, "commit", None)?;
        Ok(())
    }

    fn send_asset_stat_checked(
        &self,
        transfer_id: &str,
        path: &str,
    ) -> Result<Option<AssetRemoteStat>, String> {
        let payload = serde_json::json!({
            "transferId": transfer_id,
            "path": path,
        });
        let ack = self.send_asset_command_and_wait_timeout(
            "asset/stat",
            &payload,
            transfer_id,
            "stat",
            Some(path),
            ASSET_STAT_TIMEOUT,
        )?;
        let Some(size) = ack.get("size").and_then(|value| value.as_u64()) else {
            return Ok(None);
        };
        let Some(checksum) = ack.get("checksum").and_then(|value| value.as_str()) else {
            return Ok(None);
        };
        Ok(Some(AssetRemoteStat {
            size,
            checksum: checksum.to_string(),
        }))
    }

    fn send_asset_patch_commit_checked(
        &self,
        transfer_id: &str,
        file_count: u32,
        byte_count: u64,
    ) -> Result<(), String> {
        let payload = serde_json::json!({
            "transferId": transfer_id,
            "fileCount": file_count,
            "totalBytes": byte_count,
        });
        self.send_asset_command_and_wait(
            "asset/patch-commit",
            &payload,
            transfer_id,
            "patch",
            None,
        )?;
        Ok(())
    }

    fn send_asset_command_and_wait(
        &self,
        topic: &str,
        payload: &serde_json::Value,
        transfer_id: &str,
        phase: &str,
        path: Option<&str>,
    ) -> Result<serde_json::Value, String> {
        self.send_asset_command_and_wait_timeout(
            topic,
            payload,
            transfer_id,
            phase,
            path,
            ASSET_ACK_TIMEOUT,
        )
    }

    fn send_asset_command_and_wait_timeout(
        &self,
        topic: &str,
        payload: &serde_json::Value,
        transfer_id: &str,
        phase: &str,
        path: Option<&str>,
        timeout: Duration,
    ) -> Result<serde_json::Value, String> {
        let receiver = self.register_asset_ack_waiter(transfer_id, phase, path)?;
        if let Err(error) = self.send(topic, payload) {
            self.remove_asset_ack_waiter(transfer_id, phase, path);
            return Err(error);
        }
        let ack = receiver.recv_timeout(timeout).map_err(|_| {
            self.remove_asset_ack_waiter(transfer_id, phase, path);
            format!(
                "未收到板端素材 OTA 确认: transferId={} phase={}{}",
                transfer_id,
                phase,
                path.map(|value| format!(" path={value}"))
                    .unwrap_or_default()
            )
        })?;
        if ack.get("ok").and_then(|value| value.as_bool()) != Some(true) {
            return Err(ack
                .get("error")
                .or_else(|| ack.get("message"))
                .and_then(|value| value.as_str())
                .unwrap_or("板端素材 OTA 写入失败")
                .to_string());
        }
        Ok(ack)
    }

    fn register_asset_ack_waiter(
        &self,
        transfer_id: &str,
        phase: &str,
        path: Option<&str>,
    ) -> Result<mpsc::Receiver<serde_json::Value>, String> {
        let (sender, receiver) = mpsc::channel();
        let mut waiters = self.asset_ack_waiters.lock().map_err(|e| e.to_string())?;
        waiters.push(AssetAckWaiter {
            transfer_id: transfer_id.to_string(),
            phase: phase.to_string(),
            path: path.map(str::to_string),
            sender,
        });
        Ok(receiver)
    }

    fn remove_asset_ack_waiter(&self, transfer_id: &str, phase: &str, path: Option<&str>) {
        if let Ok(mut waiters) = self.asset_ack_waiters.lock() {
            waiters.retain(|waiter| {
                waiter.transfer_id != transfer_id
                    || waiter.phase != phase
                    || waiter.path.as_deref() != path
            });
        }
    }

    fn register_widget_ack_waiter(
        &self,
        transfer_id: &str,
        phase: &str,
    ) -> Result<mpsc::Receiver<serde_json::Value>, String> {
        let (sender, receiver) = mpsc::channel();
        let mut waiters = self.widget_ack_waiters.lock().map_err(|e| e.to_string())?;
        waiters.push(WidgetAckWaiter {
            transfer_id: transfer_id.to_string(),
            phase: phase.to_string(),
            sender,
        });
        Ok(receiver)
    }

    fn remove_widget_ack_waiter(&self, transfer_id: &str, phase: &str) {
        if let Ok(mut waiters) = self.widget_ack_waiters.lock() {
            waiters.retain(|w| !(w.transfer_id == transfer_id && w.phase == phase));
        }
    }

    /// Widget OTA: begin transfer of a .clawpkg widget directory.
    pub fn send_widget_install_begin(
        &self,
        transfer_id: &str,
        widget_id: &str,
    ) -> Result<(), String> {
        let payload = serde_json::json!({"transferId": transfer_id, "widgetId": widget_id});
        self.send("widget/begin", &payload)
    }

    /// Widget OTA: send one file's content as base64 chunk.
    /// Mirrors send_asset_chunk wire format (path/data/index) — server-side
    /// uses the same b64 staging + decode helpers.
    pub fn send_widget_install_chunk(
        &self,
        transfer_id: &str,
        relative_path: &str,
        data_base64: &str,
        index: u32,
    ) -> Result<(), String> {
        let payload = serde_json::json!({
            "transferId": transfer_id,
            "path": relative_path,
            "data": data_base64,
            "index": index.to_string(),
        });
        self.send_no_flush("widget/chunk", &payload)
    }

    /// Widget OTA: commit — server unpacks staging into widgets/<id>/ and
    /// writes .active-widget so board-widget-runtime reloads.
    pub fn send_widget_install_commit(
        &self,
        transfer_id: &str,
        widget_id: &str,
    ) -> Result<(), String> {
        let payload = serde_json::json!({"transferId": transfer_id, "widgetId": widget_id});
        self.send("widget/commit", &payload)
    }

    /// Push a local .clawpkg directory to the device via USB widget OTA.
    /// Walks all regular files under `widget_dir`, sends each as a base64
    /// chunk, commits. Optional binding_overrides applied to buttons.json
    /// before sending (action → new_control mapping).
    /// Returns (file_count, byte_count_sent_base64).
    pub fn install_widget_clawpkg<F>(
        &self,
        widget_id: &str,
        widget_dir: &std::path::Path,
        binding_overrides: &std::collections::HashMap<String, String>,
        on_progress: F,
    ) -> Result<(u32, u64), String>
    where
        F: Fn(u32, u32, u64),
    {
        use base64::Engine;
        let transfer_id = format!(
            "widget-{}-{}",
            widget_id,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_millis())
                .unwrap_or(0)
        );

        // 1) walk widget_dir to collect (rel_path, bytes) — small files (each a
        // JSON or empty .keep), so reading fully into memory is fine.
        let mut entries: Vec<(String, Vec<u8>)> = Vec::new();
        fn walk(
            root: &std::path::Path,
            cur: &std::path::Path,
            out: &mut Vec<(String, Vec<u8>)>,
        ) -> Result<(), String> {
            for ent in
                std::fs::read_dir(cur).map_err(|e| format!("read_dir {}: {}", cur.display(), e))?
            {
                let ent = ent.map_err(|e| e.to_string())?;
                let p = ent.path();
                if p.is_dir() {
                    walk(root, &p, out)?;
                } else if p.is_file() {
                    let rel = widget_ota_relative_path(root, &p)?;
                    let bytes =
                        std::fs::read(&p).map_err(|e| format!("read {}: {}", p.display(), e))?;
                    out.push((rel, bytes));
                }
            }
            Ok(())
        }
        walk(widget_dir, widget_dir, &mut entries)?;

        // 2) apply binding overrides to buttons.json in-memory
        if !binding_overrides.is_empty() {
            for (rel, bytes) in entries.iter_mut() {
                if rel == "buttons.json" {
                    if let Ok(mut arr) = serde_json::from_slice::<Vec<serde_json::Value>>(bytes) {
                        for ent in arr.iter_mut() {
                            if let Some(obj) = ent.as_object_mut() {
                                if let Some(action) = obj.get("action").and_then(|v| v.as_str()) {
                                    if let Some(new_control) = binding_overrides.get(action) {
                                        if let Some((canonical_control, new_event)) =
                                            canonical_binding_for_control(new_control)
                                        {
                                            obj.insert(
                                                "control".into(),
                                                serde_json::Value::String(
                                                    canonical_control.to_string(),
                                                ),
                                            );
                                            obj.insert(
                                                "event".into(),
                                                serde_json::Value::String(new_event.to_string()),
                                            );
                                        } else {
                                            obj.insert(
                                                "control".into(),
                                                serde_json::Value::String(new_control.clone()),
                                            );
                                        }
                                    }
                                }
                            }
                        }
                        *bytes = serde_json::to_vec_pretty(&arr).map_err(|e| e.to_string())?;
                    }
                    break;
                }
            }
        }

        // 3) begin — register ack waiter BEFORE sending so we don't miss
        //    a very-fast reply on a healthy board (~50ms).
        let begin_rx = self.register_widget_ack_waiter(&transfer_id, "begin")?;
        self.send_widget_install_begin(&transfer_id, widget_id)?;
        match begin_rx.recv_timeout(WIDGET_BEGIN_ACK_TIMEOUT) {
            Ok(ack) => {
                if ack.get("ok").and_then(|v| v.as_bool()) != Some(true) {
                    self.remove_widget_ack_waiter(&transfer_id, "begin");
                    let msg = ack
                        .get("msg")
                        .and_then(|v| v.as_str())
                        .unwrap_or("板端拒绝 widget begin");
                    return Err(format!("widget begin rejected: {}", msg));
                }
            }
            Err(_) => {
                self.remove_widget_ack_waiter(&transfer_id, "begin");
                return Err(format_widget_ack_timeout(&transfer_id, "begin"));
            }
        }

        // 4) chunks. Skip zero-byte files (.keep markers) — device-side chunk
        // handler rejects empty `data` and the runtime doesn't need these.
        // Successful chunks are NOT acked by board (only failures are), so we
        // stream without waiting between chunks.
        let b64 = base64::engine::general_purpose::STANDARD;
        let total = entries.len() as u32;
        let mut sent_bytes: u64 = 0;
        for (i, (rel, bytes)) in entries.iter().enumerate() {
            if bytes.is_empty() {
                continue;
            }
            let encoded = b64.encode(bytes);
            self.send_widget_install_chunk(&transfer_id, rel, &encoded, 0)?;
            sent_bytes += encoded.len() as u64;
            on_progress(i as u32 + 1, total, sent_bytes);
        }

        // 5) commit — wait for board to finish decode+rename+activate so the
        // caller's "success" toast lines up with the device actually showing
        // the new widget.
        let commit_rx = self.register_widget_ack_waiter(&transfer_id, "commit")?;
        self.send_widget_install_commit(&transfer_id, widget_id)?;
        match commit_rx.recv_timeout(WIDGET_COMMIT_ACK_TIMEOUT) {
            Ok(ack) => {
                if ack.get("ok").and_then(|v| v.as_bool()) != Some(true) {
                    let msg = ack
                        .get("msg")
                        .and_then(|v| v.as_str())
                        .unwrap_or("板端 widget commit 失败")
                        .to_string();
                    return Err(msg);
                }
            }
            Err(_) => {
                self.remove_widget_ack_waiter(&transfer_id, "commit");
                return Err(format_widget_ack_timeout(&transfer_id, "commit"));
            }
        }
        Ok((total, sent_bytes))
    }

    /// Transfer appearance video files and optional WAV cues to device via asset protocol.
    /// Reads manifest.json, sends each ok family's assets as base64 chunks.
    /// `app_data_dir` is the app's local data root (videoPath is relative to it).
    /// `on_progress` is called with (current_file, total_files, bytes_sent, total_bytes).
    /// Returns (file_count, byte_count).
    pub fn sync_appearance<F>(
        &self,
        appearance_dir: &std::path::Path,
        app_data_dir: &std::path::Path,
        on_progress: F,
    ) -> Result<(u32, u64), String>
    where
        F: Fn(u32, u32, u64, u64),
    {
        let _asset_transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|e| e.to_string())?;
        let manifest_path = appearance_dir.join("manifest.json");
        let manifest_str = std::fs::read_to_string(&manifest_path)
            .map_err(|e| format!("读取 manifest 失败: {}", e))?;
        let manifest: serde_json::Value = serde_json::from_str(&manifest_str)
            .map_err(|e| format!("解析 manifest 失败: {}", e))?;

        let families = manifest
            .get("families")
            .and_then(|v| v.as_array())
            .ok_or("manifest 中没有 families 数组")?;

        // Pre-calculate total files and bytes for progress reporting.
        let assets = collect_appearance_assets(families, appearance_dir, app_data_dir);
        let audio_device_paths =
            collect_appearance_audio_device_paths(families, appearance_dir, app_data_dir);
        let total_files: u32 = assets.len() as u32;
        let total_bytes: u64 = assets
            .iter()
            .filter_map(|asset| std::fs::metadata(&asset.source_path).ok())
            .map(|meta| meta.len())
            .sum();

        let digests = digest_appearance_assets(&assets)?;
        match self.plan_incremental_appearance_sync(&digests, &audio_device_paths) {
            Ok(AppearanceSyncPlan::Skip) => {
                eprintln!("[usb-appearance-ota] skip transfer: board assets already match");
                on_progress(0, total_files, 0, total_bytes);
                return Ok((0, 0));
            }
            Ok(AppearanceSyncPlan::AudioPatch(paths)) => {
                let changed_audio = assets
                    .iter()
                    .filter(|asset| paths.iter().any(|path| path == &asset.device_path))
                    .cloned()
                    .collect::<Vec<_>>();
                match self.sync_appearance_audio_patch(changed_audio, &on_progress) {
                    Ok(result) => return Ok(result),
                    Err(error)
                        if parse_missing_asset_ack_phase(&error)
                            == Some(AppearanceAssetAckPhase::Patch) =>
                    {
                        eprintln!(
                            "[usb-appearance-ota] patch commit unsupported; retrying full sync: {}",
                            error
                        );
                    }
                    Err(error) => return Err(error),
                }
            }
            Ok(AppearanceSyncPlan::Full) => {}
            Err(error) => {
                eprintln!(
                    "[usb-appearance-ota] remote stat unavailable; falling back to full sync: {}",
                    error
                );
            }
        }

        self.sync_appearance_full_with_legacy_fallback(
            &assets,
            total_files,
            total_bytes,
            &on_progress,
        )
    }

    fn sync_appearance_full_with_legacy_fallback<F>(
        &self,
        assets: &[AppearanceAssetEntry],
        total_files: u32,
        total_bytes: u64,
        on_progress: &F,
    ) -> Result<(u32, u64), String>
    where
        F: Fn(u32, u32, u64, u64),
    {
        match self.sync_appearance_full(
            assets,
            total_files,
            total_bytes,
            on_progress,
            AppearanceFullSyncMode::Verified,
        ) {
            Ok(result) => Ok(result),
            Err(error) if should_retry_appearance_with_legacy_full_sync(&error) => {
                eprintln!(
                    "[usb-appearance-ota] falling back to legacy full sync after protocol timeout: {}",
                    error
                );
                self.sync_appearance_full(
                    assets,
                    total_files,
                    total_bytes,
                    on_progress,
                    AppearanceFullSyncMode::LegacyCommitOnly,
                )
            }
            Err(error) => Err(error),
        }
    }

    fn sync_appearance_full<F>(
        &self,
        assets: &[AppearanceAssetEntry],
        total_files: u32,
        total_bytes: u64,
        on_progress: &F,
        mode: AppearanceFullSyncMode,
    ) -> Result<(u32, u64), String>
    where
        F: Fn(u32, u32, u64, u64),
    {
        let transfer_prefix = match mode {
            AppearanceFullSyncMode::Verified => "sync",
            AppearanceFullSyncMode::LegacyCommitOnly => "legacy-sync",
        };
        let transfer_id = format!(
            "{}-{}",
            transfer_prefix,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_millis()
        );

        eprintln!(
            "[usb-appearance-ota] begin transfer_id={} mode={:?} files={} bytes={}",
            transfer_id, mode, total_files, total_bytes
        );
        self.send_asset_begin_checked(&transfer_id)?;

        let mut file_count: u32 = 0;
        let mut byte_count: u64 = 0;
        on_progress(0, total_files, 0, total_bytes);

        for asset in assets {
            eprintln!(
                "[usb-appearance-ota] file family={} kind={} path={} mode={:?}",
                asset.family_name,
                asset.kind,
                asset.source_path.display(),
                mode
            );

            let open_label = if asset.kind == "audio" {
                "打开音效文件失败"
            } else {
                "打开视频文件失败"
            };
            let read_label = if asset.kind == "audio" {
                "读取音效失败"
            } else {
                "读取视频失败"
            };
            let mut file = std::fs::File::open(&asset.source_path)
                .map_err(|e| format!("{} {}: {}", open_label, asset.family_name, e))?;
            let mut buf = Vec::new();
            file.read_to_end(&mut buf)
                .map_err(|e| format!("{} {}: {}", read_label, asset.family_name, e))?;

            let file_size = buf.len() as u64;
            let checksum = asset_checksum_hex(&buf);
            let mut chunk_count = 0u64;
            for (i, chunk) in buf.chunks(APPEARANCE_ASSET_CHUNK_SIZE).enumerate() {
                let b64 = base64::engine::general_purpose::STANDARD.encode(chunk);
                self.send_asset_chunk(&transfer_id, &asset.device_path, &b64, i as u32)?;
                chunk_count += 1;
                let _ = self.flush();
                std::thread::sleep(appearance_asset_chunk_delay(b64.len()));
                let chunk_bytes_sent =
                    std::cmp::min(((i + 1) * APPEARANCE_ASSET_CHUNK_SIZE) as u64, file_size);
                on_progress(
                    file_count,
                    total_files,
                    byte_count + chunk_bytes_sent,
                    total_bytes,
                );
            }
            if mode == AppearanceFullSyncMode::Verified {
                self.send_asset_file_commit_checked(
                    &transfer_id,
                    &asset.device_path,
                    file_size,
                    &checksum,
                    chunk_count,
                )?;
            }

            file_count += 1;
            byte_count += file_size;
            on_progress(file_count, total_files, byte_count, total_bytes);
        }

        self.send_asset_commit_checked(&transfer_id, file_count, byte_count)?;
        eprintln!(
            "[usb-appearance-ota] commit transfer_id={} mode={:?} sent_files={} sent_bytes={}",
            transfer_id, mode, file_count, byte_count
        );

        Ok((file_count, byte_count))
    }

    fn plan_incremental_appearance_sync(
        &self,
        local: &[AppearanceAssetDigest],
        audio_device_paths: &[String],
    ) -> Result<AppearanceSyncPlan, String> {
        let transfer_id = format!(
            "stat-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_millis()
        );
        let mut remote = HashMap::new();
        for asset in local {
            if let Some(stat) = self.send_asset_stat_checked(&transfer_id, &asset.device_path)? {
                remote.insert(asset.device_path.clone(), stat);
            }
        }
        let local_audio_paths = local
            .iter()
            .filter(|asset| asset.kind == "audio")
            .map(|asset| asset.device_path.as_str())
            .collect::<HashSet<_>>();
        let mut remote_has_removed_audio = false;
        for path in audio_device_paths {
            if local_audio_paths.contains(path.as_str()) {
                continue;
            }
            if let Some(stat) = self.send_asset_stat_checked(&transfer_id, path)? {
                remote.insert(path.clone(), stat);
                remote_has_removed_audio = true;
            }
        }
        Ok(plan_appearance_sync_from_digests(
            local,
            &remote,
            remote_has_removed_audio,
        ))
    }

    fn sync_appearance_audio_patch<F>(
        &self,
        assets: Vec<AppearanceAssetEntry>,
        on_progress: &F,
    ) -> Result<(u32, u64), String>
    where
        F: Fn(u32, u32, u64, u64),
    {
        if assets.iter().any(|asset| asset.kind != "audio") {
            return Err("音效增量 OTA 只能下发 WAV 文件".to_string());
        }
        let transfer_id = format!(
            "audio-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_millis()
        );
        let total_files: u32 = assets.len() as u32;
        let total_bytes: u64 = assets
            .iter()
            .filter_map(|asset| std::fs::metadata(&asset.source_path).ok())
            .map(|meta| meta.len())
            .sum();

        eprintln!(
            "[usb-appearance-ota] begin audio patch transfer_id={} files={} bytes={}",
            transfer_id, total_files, total_bytes
        );
        self.send_asset_begin_checked(&transfer_id)?;

        let mut file_count: u32 = 0;
        let mut byte_count: u64 = 0;
        on_progress(0, total_files, 0, total_bytes);

        for asset in assets {
            let mut file = std::fs::File::open(&asset.source_path)
                .map_err(|e| format!("打开音效文件失败 {}: {}", asset.family_name, e))?;
            let mut buf = Vec::new();
            file.read_to_end(&mut buf)
                .map_err(|e| format!("读取音效失败 {}: {}", asset.family_name, e))?;

            let file_size = buf.len() as u64;
            let checksum = asset_checksum_hex(&buf);
            let mut chunk_count = 0u64;
            for (i, chunk) in buf.chunks(APPEARANCE_ASSET_CHUNK_SIZE).enumerate() {
                let b64 = base64::engine::general_purpose::STANDARD.encode(chunk);
                self.send_asset_chunk(&transfer_id, &asset.device_path, &b64, i as u32)?;
                chunk_count += 1;
                let _ = self.flush();
                std::thread::sleep(appearance_asset_chunk_delay(b64.len()));
                let chunk_bytes_sent =
                    std::cmp::min(((i + 1) * APPEARANCE_ASSET_CHUNK_SIZE) as u64, file_size);
                on_progress(
                    file_count,
                    total_files,
                    byte_count + chunk_bytes_sent,
                    total_bytes,
                );
            }
            self.send_asset_file_commit_checked(
                &transfer_id,
                &asset.device_path,
                file_size,
                &checksum,
                chunk_count,
            )?;

            file_count += 1;
            byte_count += file_size;
            on_progress(file_count, total_files, byte_count, total_bytes);
        }

        self.send_asset_patch_commit_checked(&transfer_id, file_count, byte_count)?;
        eprintln!(
            "[usb-appearance-ota] audio patch commit transfer_id={} sent_files={} sent_bytes={}",
            transfer_id, file_count, byte_count
        );
        Ok((file_count, byte_count))
    }

    /// Get current connection status
    pub fn status(&self) -> UsbConnectionStatus {
        let conn = self.connection.lock().ok();
        match conn.as_ref().and_then(|c| c.as_ref()) {
            Some(c) => UsbConnectionStatus {
                connected: c.connected,
                port_name: c.port_name.clone(),
                board_device_id: c.board_device_id.clone(),
                transport: "usb".to_string(),
            },
            None => UsbConnectionStatus {
                connected: false,
                port_name: String::new(),
                board_device_id: String::new(),
                transport: "mqtt".to_string(),
            },
        }
    }
}

fn asset_checksum_hex(bytes: &[u8]) -> String {
    let checksum = fnv1a64_update(FNV1A64_OFFSET, bytes);
    format!("{checksum:016x}")
}

fn digest_appearance_asset(asset: &AppearanceAssetEntry) -> Result<AppearanceAssetDigest, String> {
    let bytes = std::fs::read(&asset.source_path).map_err(|e| {
        let label = if asset.kind == "audio" {
            "读取音效失败"
        } else {
            "读取视频失败"
        };
        format!("{} {}: {}", label, asset.family_name, e)
    })?;
    Ok(AppearanceAssetDigest {
        kind: asset.kind,
        device_path: asset.device_path.clone(),
        size: bytes.len() as u64,
        checksum: asset_checksum_hex(&bytes),
    })
}

fn digest_appearance_assets(
    assets: &[AppearanceAssetEntry],
) -> Result<Vec<AppearanceAssetDigest>, String> {
    assets.iter().map(digest_appearance_asset).collect()
}

fn plan_appearance_sync_from_digests(
    local: &[AppearanceAssetDigest],
    remote: &HashMap<String, AssetRemoteStat>,
    remote_has_removed_audio: bool,
) -> AppearanceSyncPlan {
    for asset in local.iter().filter(|asset| asset.kind == "video") {
        let Some(stat) = remote.get(&asset.device_path) else {
            return AppearanceSyncPlan::Full;
        };
        if stat.size != asset.size || stat.checksum != asset.checksum {
            return AppearanceSyncPlan::Full;
        }
    }

    if remote_has_removed_audio {
        return AppearanceSyncPlan::Full;
    }

    let changed_audio = local
        .iter()
        .filter(|asset| asset.kind == "audio")
        .filter_map(|asset| {
            let stat = remote.get(&asset.device_path)?;
            if stat.size == asset.size && stat.checksum == asset.checksum {
                None
            } else {
                Some(asset.device_path.clone())
            }
        })
        .chain(
            local
                .iter()
                .filter(|asset| asset.kind == "audio" && !remote.contains_key(&asset.device_path))
                .map(|asset| asset.device_path.clone()),
        )
        .collect::<Vec<_>>();

    if changed_audio.is_empty() {
        AppearanceSyncPlan::Skip
    } else {
        AppearanceSyncPlan::AudioPatch(changed_audio)
    }
}

fn fnv1a64_update(mut checksum: u64, bytes: &[u8]) -> u64 {
    for byte in bytes {
        checksum ^= u64::from(*byte);
        checksum = checksum.wrapping_mul(FNV1A64_PRIME);
    }
    checksum
}

fn build_asset_file_commit_payload(
    transfer_id: &str,
    path: &str,
    size: u64,
    checksum: &str,
    chunk_count: u64,
) -> serde_json::Value {
    serde_json::json!({
        "transferId": transfer_id,
        "path": path,
        "size": size,
        "checksum": checksum,
        "chunkCount": chunk_count,
    })
}

fn format_widget_ack_timeout(transfer_id: &str, phase: &str) -> String {
    format!(
        "未收到板端组件 OTA 确认: transferId={} phase={}",
        transfer_id, phase
    )
}

fn widget_ota_relative_path(root: &Path, path: &Path) -> Result<String, String> {
    let rel = path.strip_prefix(root).map_err(|error| error.to_string())?;
    let parts = rel
        .components()
        .map(|component| match component {
            std::path::Component::Normal(value) => value
                .to_str()
                .map(str::to_string)
                .ok_or_else(|| format!("widget path is not UTF-8: {}", path.display())),
            _ => Err(format!("unsafe widget path: {}", path.display())),
        })
        .collect::<Result<Vec<_>, _>>()?;
    Ok(parts.join("/"))
}

fn resolve_asset_ack(
    waiters: &Arc<Mutex<Vec<AssetAckWaiter>>>,
    topic: &str,
    payload: &serde_json::Value,
) {
    if topic != "asset/ack" {
        return;
    }
    let transfer_id = payload
        .get("transferId")
        .and_then(|value| value.as_str())
        .unwrap_or_default();
    let phase = payload
        .get("phase")
        .and_then(|value| value.as_str())
        .unwrap_or_default();
    let path = payload.get("path").and_then(|value| value.as_str());
    if transfer_id.is_empty() || phase.is_empty() {
        return;
    }

    let sender = waiters.lock().ok().and_then(|mut waiters| {
        let index = waiters.iter().position(|waiter| {
            waiter.transfer_id == transfer_id
                && waiter.phase == phase
                && waiter.path.as_deref() == path
        })?;
        Some(waiters.remove(index).sender)
    });
    eprintln!(
        "[usb-appearance-ota] ack transfer_id={} phase={} path={} matched={}",
        transfer_id,
        phase,
        path.unwrap_or(""),
        sender.is_some()
    );
    if let Some(sender) = sender {
        let _ = sender.send(payload.clone());
    }
}

fn resolve_widget_ack(
    waiters: &Arc<Mutex<Vec<WidgetAckWaiter>>>,
    topic: &str,
    payload: &serde_json::Value,
) {
    if topic != "widget-install-ack" {
        return;
    }
    let transfer_id = payload
        .get("transferId")
        .and_then(|v| v.as_str())
        .unwrap_or_default();
    let phase = payload
        .get("phase")
        .and_then(|v| v.as_str())
        .unwrap_or_default();
    if transfer_id.is_empty() || phase.is_empty() {
        return;
    }
    let sender = waiters.lock().ok().and_then(|mut waiters| {
        let idx = waiters
            .iter()
            .position(|w| w.transfer_id == transfer_id && w.phase == phase)?;
        Some(waiters.remove(idx).sender)
    });
    eprintln!(
        "[widget-ota] ack transfer_id={} phase={} matched={}",
        transfer_id,
        phase,
        sender.is_some()
    );
    if let Some(sender) = sender {
        let _ = sender.send(payload.clone());
    }
}

#[derive(Debug, Clone)]
struct AppearanceAssetEntry {
    family_name: String,
    kind: &'static str,
    source_path: PathBuf,
    device_path: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct AppearanceAssetDigest {
    kind: &'static str,
    device_path: String,
    size: u64,
    checksum: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct AssetRemoteStat {
    size: u64,
    checksum: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum AppearanceSyncPlan {
    Full,
    AudioPatch(Vec<String>),
    Skip,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum AppearanceFullSyncMode {
    Verified,
    LegacyCommitOnly,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum AppearanceAssetAckPhase {
    Begin,
    Stat,
    File,
    Patch,
    Commit,
}

fn parse_missing_asset_ack_phase(error: &str) -> Option<AppearanceAssetAckPhase> {
    if !error.contains("未收到板端素材 OTA 确认:") {
        return None;
    }
    if error.contains("phase=begin") {
        return Some(AppearanceAssetAckPhase::Begin);
    }
    if error.contains("phase=stat") {
        return Some(AppearanceAssetAckPhase::Stat);
    }
    if error.contains("phase=file") {
        return Some(AppearanceAssetAckPhase::File);
    }
    if error.contains("phase=patch") {
        return Some(AppearanceAssetAckPhase::Patch);
    }
    if error.contains("phase=commit") {
        return Some(AppearanceAssetAckPhase::Commit);
    }
    None
}

fn should_retry_appearance_with_legacy_full_sync(error: &str) -> bool {
    matches!(
        parse_missing_asset_ack_phase(error),
        Some(
            AppearanceAssetAckPhase::Stat
                | AppearanceAssetAckPhase::File
                | AppearanceAssetAckPhase::Patch
        )
    )
}

fn manifest_asset_path(
    family: &serde_json::Value,
    key: &str,
    app_data_dir: &Path,
) -> Option<PathBuf> {
    family
        .get(key)
        .and_then(|v| v.as_str())
        .filter(|p| !p.is_empty())
        .map(|p| {
            let path = Path::new(p);
            if path.is_absolute() {
                path.to_path_buf()
            } else {
                app_data_dir.join(path)
            }
        })
}

fn family_video_path(
    family: &serde_json::Value,
    family_name: &str,
    appearance_dir: &Path,
    app_data_dir: &Path,
) -> Option<PathBuf> {
    manifest_asset_path(family, "videoPath", app_data_dir).or_else(|| {
        let candidate = appearance_dir.join(format!("{}.mp4", family_name));
        candidate.exists().then_some(candidate)
    })
}

fn family_audio_path(
    family: &serde_json::Value,
    family_name: &str,
    video_path: &Path,
    appearance_dir: &Path,
    app_data_dir: &Path,
) -> Option<PathBuf> {
    manifest_asset_path(family, "audioPath", app_data_dir)
        .or_else(|| {
            let candidate = video_path.with_extension("wav");
            candidate.exists().then_some(candidate)
        })
        .or_else(|| {
            let candidate = appearance_dir.join(format!("{}.wav", family_name));
            candidate.exists().then_some(candidate)
        })
}

fn collect_appearance_assets(
    families: &[serde_json::Value],
    appearance_dir: &Path,
    app_data_dir: &Path,
) -> Vec<AppearanceAssetEntry> {
    let mut assets = Vec::new();
    for family in families {
        let ok = family.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
        if !ok {
            continue;
        }

        let family_name = family
            .get("family")
            .and_then(|v| v.as_str())
            .unwrap_or("unknown")
            .to_string();
        let Some(video_path) =
            family_video_path(family, &family_name, appearance_dir, app_data_dir)
        else {
            continue;
        };
        if !video_path.exists() {
            continue;
        }

        // Device activates files under terrier-clips after commit. Keep both
        // single and dotted family names intact, e.g. videos/working.mp4 and
        // videos/working.thinking.mp4. WAV cues live beside their matching MP4s.
        assets.push(AppearanceAssetEntry {
            family_name: family_name.clone(),
            kind: "video",
            source_path: video_path.clone(),
            device_path: format!("videos/{}.mp4", family_name),
        });

        if let Some(audio_path) = family_audio_path(
            family,
            &family_name,
            &video_path,
            appearance_dir,
            app_data_dir,
        ) {
            if audio_path.exists() {
                assets.push(AppearanceAssetEntry {
                    family_name: family_name.clone(),
                    kind: "audio",
                    source_path: audio_path,
                    device_path: format!("videos/{}.wav", family_name),
                });
            }
        }
    }
    assets
}

fn collect_appearance_audio_device_paths(
    families: &[serde_json::Value],
    appearance_dir: &Path,
    app_data_dir: &Path,
) -> Vec<String> {
    let mut paths = Vec::new();
    for family in families {
        let ok = family.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
        if !ok {
            continue;
        }

        let family_name = family
            .get("family")
            .and_then(|v| v.as_str())
            .unwrap_or("unknown")
            .to_string();
        let Some(video_path) =
            family_video_path(family, &family_name, appearance_dir, app_data_dir)
        else {
            continue;
        };
        if !video_path.exists() {
            continue;
        }

        paths.push(format!("videos/{}.wav", family_name));
    }
    paths
}

fn appearance_asset_chunk_delay(encoded_len: usize) -> Duration {
    let transfer_ms = ((encoded_len as u64) * 1000).div_ceil(APPEARANCE_ASSET_SERIAL_BYTES_PER_SEC);
    let delay_ms = transfer_ms
        .saturating_add(APPEARANCE_ASSET_CHUNK_DELAY_MARGIN_MS)
        .max(APPEARANCE_ASSET_CHUNK_DELAY_FLOOR_MS);
    Duration::from_millis(delay_ms)
}

fn prefer_callout_ports_for_macos(mut devices: Vec<UsbDeviceInfo>) -> Vec<UsbDeviceInfo> {
    let callout_suffixes: HashSet<String> = devices
        .iter()
        .filter_map(|device| {
            device
                .port_name
                .strip_prefix("/dev/cu.")
                .map(|suffix| suffix.to_string())
        })
        .collect();

    devices.retain(|device| {
        device
            .port_name
            .strip_prefix("/dev/tty.")
            .map(|suffix| !callout_suffixes.contains(suffix))
            .unwrap_or(true)
    });
    devices.sort_by(|left, right| {
        serial_port_priority(&left.port_name)
            .cmp(&serial_port_priority(&right.port_name))
            .then_with(|| left.port_name.cmp(&right.port_name))
    });
    devices
}

fn serial_port_priority(port_name: &str) -> u8 {
    if port_name.starts_with("/dev/cu.") {
        0
    } else if port_name.contains("usbmodem") || port_name.contains("ttyACM") {
        1
    } else {
        2
    }
}

fn is_supported_usb_serial_port(port_name: &str, vid: u16) -> bool {
    port_name.contains("ttyACM")
        || port_name.contains("ttyUSB")
        || port_name.contains("usbmodem")
        || port_name.contains("usbserial")
        || port_name.contains("SLAB_USBtoUART")
        || vid == 0x1d6b
        || vid == 0x0525
        || vid == 0x1a86
        || vid == 0x10c4
        || vid == 0x0403
}

fn canonical_binding_for_control(control: &str) -> Option<(&'static str, &'static str)> {
    let binding = match control {
        "屏幕点击" => ("屏幕区域", "screen.region.tap"),
        "屏幕长按" => ("屏幕区域", "screen.region.long_press"),
        "旋钮旋转" => ("旋钮", "knob.rotate_cw / knob.rotate_ccw"),
        "屏幕区域" => ("屏幕区域", "screen.region.tap"),
        _ => return None,
    };
    Some(binding)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn prefers_macos_callout_port_over_blocking_tty_pair() {
        let devices = vec![
            UsbDeviceInfo {
                port_name: "/dev/tty.usbmodem11201".to_string(),
                vid: 0x0525,
                pid: 0xa4a7,
                serial_number: String::new(),
                manufacturer: String::new(),
                product: "Gadget Serial v2.4".to_string(),
            },
            UsbDeviceInfo {
                port_name: "/dev/cu.usbmodem11201".to_string(),
                vid: 0x0525,
                pid: 0xa4a7,
                serial_number: String::new(),
                manufacturer: String::new(),
                product: "Gadget Serial v2.4".to_string(),
            },
        ];

        let normalized = prefer_callout_ports_for_macos(devices);

        assert_eq!(normalized.len(), 1);
        assert_eq!(normalized[0].port_name, "/dev/cu.usbmodem11201");
    }

    #[test]
    fn accepts_documented_usbserial_adapter_ports() {
        assert!(is_supported_usb_serial_port(
            "/dev/cu.usbserial-210",
            0x1a86
        ));
    }

    #[test]
    fn asset_chunk_delay_tracks_serial_wire_time() {
        let delay = appearance_asset_chunk_delay(65_536);

        assert!(delay >= Duration::from_millis(844));
    }

    #[test]
    fn asset_file_checksum_is_stable_for_transfer_integrity() {
        assert_eq!(asset_checksum_hex(b"hello"), "a430d84680aabd0b");
    }

    #[test]
    fn asset_file_commit_payload_carries_integrity_fields() {
        let payload = build_asset_file_commit_payload(
            "transfer-1",
            "videos/idle.mp4",
            5,
            "a430d84680aabd0b",
            2,
        );

        assert_eq!(payload["transferId"], "transfer-1");
        assert_eq!(payload["path"], "videos/idle.mp4");
        assert_eq!(payload["size"], 5);
        assert_eq!(payload["checksum"], "a430d84680aabd0b");
        assert_eq!(payload["chunkCount"], 2);
    }

    #[test]
    fn widget_ota_relative_path_uses_device_safe_forward_slashes() {
        let root = Path::new("meeting-timer");
        let path = root.join("runtime").join("widget.json");

        assert_eq!(
            widget_ota_relative_path(root, &path).unwrap(),
            "runtime/widget.json"
        );
    }

    #[test]
    fn appearance_sync_plan_patches_only_changed_audio_when_videos_match() {
        let local = vec![
            AppearanceAssetDigest {
                kind: "video",
                device_path: "videos/done.mp4".to_string(),
                size: 100,
                checksum: "video-same".to_string(),
            },
            AppearanceAssetDigest {
                kind: "audio",
                device_path: "videos/done.wav".to_string(),
                size: 6,
                checksum: "audio-new".to_string(),
            },
            AppearanceAssetDigest {
                kind: "audio",
                device_path: "videos/error.wav".to_string(),
                size: 5,
                checksum: "audio-same".to_string(),
            },
        ];
        let remote = std::collections::HashMap::from([
            (
                "videos/done.mp4".to_string(),
                AssetRemoteStat {
                    size: 100,
                    checksum: "video-same".to_string(),
                },
            ),
            (
                "videos/done.wav".to_string(),
                AssetRemoteStat {
                    size: 4,
                    checksum: "audio-old".to_string(),
                },
            ),
            (
                "videos/error.wav".to_string(),
                AssetRemoteStat {
                    size: 5,
                    checksum: "audio-same".to_string(),
                },
            ),
        ]);

        assert_eq!(
            plan_appearance_sync_from_digests(&local, &remote, false),
            AppearanceSyncPlan::AudioPatch(vec!["videos/done.wav".to_string()])
        );
    }

    #[test]
    fn appearance_sync_plan_uses_full_sync_when_video_differs() {
        let local = vec![
            AppearanceAssetDigest {
                kind: "video",
                device_path: "videos/done.mp4".to_string(),
                size: 100,
                checksum: "video-new".to_string(),
            },
            AppearanceAssetDigest {
                kind: "audio",
                device_path: "videos/done.wav".to_string(),
                size: 6,
                checksum: "audio-new".to_string(),
            },
        ];
        let remote = std::collections::HashMap::from([(
            "videos/done.mp4".to_string(),
            AssetRemoteStat {
                size: 99,
                checksum: "video-old".to_string(),
            },
        )]);

        assert_eq!(
            plan_appearance_sync_from_digests(&local, &remote, false),
            AppearanceSyncPlan::Full
        );
    }

    #[test]
    fn appearance_sync_plan_uses_full_sync_when_remote_has_removed_audio() {
        let local = vec![AppearanceAssetDigest {
            kind: "video",
            device_path: "videos/done.mp4".to_string(),
            size: 100,
            checksum: "video-same".to_string(),
        }];
        let remote = std::collections::HashMap::from([
            (
                "videos/done.mp4".to_string(),
                AssetRemoteStat {
                    size: 100,
                    checksum: "video-same".to_string(),
                },
            ),
            (
                "videos/done.wav".to_string(),
                AssetRemoteStat {
                    size: 6,
                    checksum: "audio-removed".to_string(),
                },
            ),
        ]);

        assert_eq!(
            plan_appearance_sync_from_digests(&local, &remote, true),
            AppearanceSyncPlan::Full
        );
    }

    #[test]
    fn appearance_sync_plan_skips_when_video_and_audio_match() {
        let local = vec![
            AppearanceAssetDigest {
                kind: "video",
                device_path: "videos/done.mp4".to_string(),
                size: 100,
                checksum: "video-same".to_string(),
            },
            AppearanceAssetDigest {
                kind: "audio",
                device_path: "videos/done.wav".to_string(),
                size: 6,
                checksum: "audio-same".to_string(),
            },
        ];
        let remote = std::collections::HashMap::from([
            (
                "videos/done.mp4".to_string(),
                AssetRemoteStat {
                    size: 100,
                    checksum: "video-same".to_string(),
                },
            ),
            (
                "videos/done.wav".to_string(),
                AssetRemoteStat {
                    size: 6,
                    checksum: "audio-same".to_string(),
                },
            ),
        ]);

        assert_eq!(
            plan_appearance_sync_from_digests(&local, &remote, false),
            AppearanceSyncPlan::Skip
        );
    }

    #[test]
    fn appearance_sync_timeout_phase_detection_identifies_protocol_gated_steps() {
        assert_eq!(
            parse_missing_asset_ack_phase("未收到板端素材 OTA 确认: transferId=t phase=stat path=videos/done.mp4"),
            Some(AppearanceAssetAckPhase::Stat)
        );
        assert_eq!(
            parse_missing_asset_ack_phase("未收到板端素材 OTA 确认: transferId=t phase=file path=videos/done.mp4"),
            Some(AppearanceAssetAckPhase::File)
        );
        assert_eq!(
            parse_missing_asset_ack_phase("未收到板端素材 OTA 确认: transferId=t phase=patch"),
            Some(AppearanceAssetAckPhase::Patch)
        );
        assert_eq!(
            parse_missing_asset_ack_phase("未收到板端素材 OTA 确认: transferId=t phase=commit"),
            Some(AppearanceAssetAckPhase::Commit)
        );
        assert_eq!(parse_missing_asset_ack_phase("板端素材 OTA 写入失败"), None);
    }

    #[test]
    fn appearance_sync_legacy_retry_is_limited_to_newer_protocol_phases() {
        assert!(should_retry_appearance_with_legacy_full_sync(
            "未收到板端素材 OTA 确认: transferId=t phase=stat path=videos/done.mp4"
        ));
        assert!(should_retry_appearance_with_legacy_full_sync(
            "未收到板端素材 OTA 确认: transferId=t phase=file path=videos/done.mp4"
        ));
        assert!(should_retry_appearance_with_legacy_full_sync(
            "未收到板端素材 OTA 确认: transferId=t phase=patch"
        ));
        assert!(!should_retry_appearance_with_legacy_full_sync(
            "未收到板端素材 OTA 确认: transferId=t phase=begin"
        ));
        assert!(!should_retry_appearance_with_legacy_full_sync(
            "未收到板端素材 OTA 确认: transferId=t phase=commit"
        ));
    }
}
