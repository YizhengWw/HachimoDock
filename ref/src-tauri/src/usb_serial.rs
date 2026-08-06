/*
 * [Input] serialport-enumerated CDC USB ports plus JSON-line OTA/state payloads.
 * [Output] USB serial manager for device handshake, state/speech forwarding,
 *          persistent prebuilt P4 H.264 Annex-B/WAV ready packs plus 4 Mbaud
 *          8KiB raw-chunk
 *          transactional OTA with checksum acks and deterministic pack IDs
 *          with bounded app-data JSONL diagnostics and failure-path aborts
 *          that instantly reactivate either cached P4 A/B slot,
 *          serialized bulk-write transactions and native-USB raw-pack
 *          rejection before any destructive full-sync write plus nonce-bound
 *          boardDeviceId selection across multiple native-USB candidates,
 *          audio-only patch commits, legacy full-sync fallback for boards that
 *          do not support per-file asset acks yet, short-id ACK-gated widget
 *          .clawpkg OTA plus capability-gated, expected-board-id removal with
 *          legacy unsupported-phase NACK correlation, capability-sized,
 *          retry-before-downshift Base64 firmware chunks with recovery,
 *          request-id-matched live device
 *          widget inventory, and persisted input configuration reads;
 *          transaction_waiters owns shared ACK/request matching;
 *          appearance_transaction owns appearance integrity, sync planning,
 *          slot fallback, ACK fallback, and deterministic pack-ID policy;
 *          widget_transaction owns component capability/path/payload policy;
 *          while the manager retains transfer sequencing;
 *          firmware_transaction owns reusable bundled/manual ESP-IDF image
 *          inspection, preflight, and rollback validation
 *          while the manager performs
 *          ESP32-P4 A/B OTA with SHA-256 and per-chunk ACKs; macOS
 *          scans prefer /dev/cu.* callout ports to avoid blocking /dev/tty.*
 *          opens, reconnects cancel stale reader clones before reopening, and
 *          connection_handle owns paired serial resources and keeps Windows
 *          handles non-inheritable by Bridge child processes;
 *          background probes use bounded handshakes and per-adapter retry backoff;
 *          failed heartbeat writes mark stale connections for auto-reconnect;
 *          ignored hardware coverage verifies active-set-driven terminal-card
 *          retention and stable first-seen ordering across working refreshes.
 * [Pos] Tauri USB transport node in ref/src-tauri/src
 * [Sync] If this file changes, update `ref/.folder.md`.
 */

use base64::Engine as _;
use serde::{Deserialize, Serialize};
use serialport::SerialPortType;
use sha2::{Digest, Sha256};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, BufReader, BufWriter, Read, Write};
use std::path::{Path, PathBuf};

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant};

mod appearance_transaction;
mod connection_handle;
mod firmware_transaction;
mod native_usb_protocol;
mod transaction_waiters;
mod transfer_log;
mod widget_transaction;

use appearance_transaction::{
    asset_checksum_hex, build_asset_file_commit_payload, compute_p4_pack_id,
    digest_appearance_assets, p4_pack_id_from_assets, p4_raw_transfer_fallback_slot,
    parse_missing_asset_ack_phase, parse_p4_appearance_slot_state,
    plan_appearance_sync_from_digests, should_retry_appearance_with_legacy_full_sync,
    AppearanceAssetAckPhase, AppearanceAssetDigest, AppearanceAssetEntry, AppearanceFullSyncMode,
    AppearanceSyncPlan, AssetRemoteStat, P4AppearanceSlotState, P4CachedPackActivation,
};
#[cfg(test)]
use appearance_transaction::{
    P4AppearanceSlot, P4_BUILTIN_APPEARANCE_SLOT, P4_RAW_APPEARANCE_SLOT,
};
#[cfg(test)]
use connection_handle::serial_open_error_is_transient;
use connection_handle::{open_serial_pair_with_retry, ProbedSerialPort, UsbConnection};
use firmware_transaction::{
    evaluate_firmware_validation, firmware_corruption_fallback_size, firmware_recovery_chunk_size,
    parse_esp_idf_app_descriptor, preferred_firmware_chunk_size, FirmwareCommandError,
    VerifiedFirmware, P4_FIRMWARE_ACK_TIMEOUT, P4_FIRMWARE_BEGIN_MAX_ATTEMPTS,
    P4_FIRMWARE_CHUNK_ACK_TIMEOUT, P4_FIRMWARE_CHUNK_MAX_ATTEMPTS, P4_FIRMWARE_CHUNK_SIZE,
    P4_FIRMWARE_COMMIT_ACK_TIMEOUT, P4_FIRMWARE_COMMIT_MAX_ATTEMPTS,
    P4_FIRMWARE_CORRUPTION_RETRIES_BEFORE_FALLBACK, P4_FIRMWARE_MAX_IMAGE_SIZE,
    P4_FIRMWARE_RECONNECT_TIMEOUT,
};
pub use firmware_transaction::{inspect_firmware_image, FirmwareImageInfo, FirmwareUpdateResult};
#[cfg(test)]
use firmware_transaction::{
    ESP_APP_DESC_MAGIC, ESP_APP_DESC_SIZE, ESP_IMAGE_HEADER_SIZE, ESP_IMAGE_SEGMENT_HEADER_SIZE,
    P4_FIRMWARE_FALLBACK_CHUNK_SIZE, P4_FIRMWARE_FAST_CHUNK_SIZE, P4_FIRMWARE_PROJECT_NAME,
    P4_FIRMWARE_RECOVERY_SUCCESS_STREAK, P4_FIRMWARE_SAFE_CHUNK_SIZE,
};
use transaction_waiters::{
    resolve_asset_ack, resolve_device_response, resolve_firmware_ack, resolve_widget_ack,
    AssetAckWaiter, DeviceResponseWaiter, FirmwareAckWaiter, WidgetAckWaiter,
};
#[cfg(test)]
use widget_transaction::P4_WIDGET_JSON_MAX_BYTES;
use widget_transaction::{
    build_widget_chunk_payload, ensure_widget_delete_supported, ensure_widget_inventory_supported,
    format_widget_ack_timeout, prepare_p4_widget_file, widget_ota_relative_path,
    widget_ota_should_skip_path, WIDGET_BEGIN_ACK_TIMEOUT, WIDGET_CHUNK_ACK_TIMEOUT,
    WIDGET_CHUNK_MAX_ATTEMPTS, WIDGET_COMMIT_ACK_TIMEOUT, WIDGET_DELETE_ACK_TIMEOUT,
};

use native_usb_protocol::{
    encode_native_usb_frame, next_native_usb_nonce, parse_native_usb_pong,
    select_native_usb_candidate, try_pop_native_usb_frame, NativeUsbCandidateIdentity,
    NativeUsbFrame, NativeUsbIdentity, P4_NATIVE_KIND_COMMIT, P4_NATIVE_KIND_FILE_BEGIN,
    P4_NATIVE_KIND_FILE_DATA, P4_NATIVE_KIND_FILE_END, P4_NATIVE_KIND_JSON, P4_NATIVE_KIND_PING,
    P4_NATIVE_USB_MAX_PAYLOAD,
};

pub fn configure_transfer_logging(app_data_dir: &Path) -> Result<PathBuf, String> {
    transfer_log::configure(app_data_dir)
}

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
    #[serde(default)]
    pub baud_rate: u32,
    pub board_device_id: String,
    pub transport: String,
    pub runtime: String,
    pub device_model: String,
    pub firmware: String,
    #[serde(default)]
    pub build_id: String,
    #[serde(default)]
    pub git_sha: String,
    #[serde(default)]
    pub build_dirty: bool,
    #[serde(default)]
    pub protocol_schema: u32,
    pub wire_protocol: String,
    pub capabilities: serde_json::Value,
}

#[derive(Debug, Clone, Deserialize)]
struct SerialMessage {
    topic: String,
    payload: serde_json::Value,
}

const APPEARANCE_ASSET_CHUNK_SIZE: usize = 49_152;
const DEFAULT_USB_SERIAL_BAUD: u32 = 921_600;
const LEGACY_USB_SERIAL_BAUD: u32 = 115_200;
const P4_USB_UART_BAUD: u32 = 4_000_000;
const P4_USB_UART_LEGACY_BAUD: u32 = 3_000_000;
// Linux board-runtime emits hello every three seconds until acknowledged, and
// opening the CH343 can cold-boot P4 for roughly five seconds before its first
// protocol frame. The first host write can also be lost while the adapter or
// UART0 settles, so keep requesting identity throughout the probe window.
const SERIAL_PROBE_TIMEOUT: Duration = Duration::from_millis(8_000);
// Background discovery must not hold every USB serial candidate for the full
// interactive timeout. A board that is still booting will be retried by the
// auto-connect backoff, while manual connect keeps the longer window above.
#[cfg(windows)]
const SERIAL_AUTO_PROBE_TIMEOUT: Duration = Duration::from_millis(1_500);
// Preserve the established macOS/Linux handshake window. Some USB serial
// drivers can reset the board when DTR/RTS is asserted during open.
#[cfg(not(windows))]
const SERIAL_AUTO_PROBE_TIMEOUT: Duration = SERIAL_PROBE_TIMEOUT;
const SERIAL_PROBE_HANDSHAKE_RETRY_INTERVAL: Duration = Duration::from_millis(750);
const APPEARANCE_ASSET_CHUNK_DELAY_FLOOR_MS: u64 = 35;
const APPEARANCE_ASSET_CHUNK_DELAY_MARGIN_MS: u64 = 25;
const ASSET_ACK_TIMEOUT: Duration = Duration::from_secs(45);
const ASSET_BEGIN_ACK_TIMEOUT: Duration = Duration::from_secs(120);
const ASSET_STAT_TIMEOUT: Duration = Duration::from_secs(2);
const P4_RAW_ASSET_ACK_TIMEOUT: Duration = Duration::from_secs(5);
pub const APPEARANCE_SYNC_CANCELLED_ERROR: &str = "形象素材传输已中断";
const P4_APPEARANCE_WIDTH: u32 = 640;
const P4_APPEARANCE_HEIGHT: u32 = 480;
const P4_APPEARANCE_FPS: u32 = 15;
const P4_APPEARANCE_MAX_FRAMES: u32 = 225;
const P4_APPEARANCE_H264_CRF: u32 = 27;
const P4_READY_DIR_NAME: &str = "p4-ready";
const P4_READY_PROFILE_VERSION: u32 = 9;
const P4_APPEARANCE_AUDIO_MAX_BYTES: usize = 1024 * 1024;
// CH343 -> ESP32-P4 UART0 is reliable at high baud with 8KB raw chunks.
// Larger chunks were faster on paper but dropped bytes without RTS/CTS.
// Keep each raw chunk divisible by three so its Base64 form has no trailing
// padding. Some ESP-IDF/Mbed TLS combinations reject otherwise valid padded
// payloads when JSON arrives in several UART reads.
const P4_APPEARANCE_ASSET_CHUNK_SIZE: usize = 20_478;
const P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE: usize = 8 * 1024;
const P4_RAW_APPEARANCE_CHUNK_MAX_ATTEMPTS: u32 = 4;
const P4_RAW_APPEARANCE_RECOVERY_DELAY: Duration = Duration::from_millis(100);
const P4_APPEARANCE_CHUNK_DECODE_MAX_ATTEMPTS: u32 = 3;
const P4_NATIVE_USB_VID: u16 = 0x303a;
const P4_NATIVE_USB_PID: u16 = 0x4040;
const P4_NATIVE_USB_INTERFACE: u8 = 0;
const P4_NATIVE_USB_EP_OUT: u8 = 0x01;
const P4_NATIVE_USB_EP_IN: u8 = 0x81;
const P4_NATIVE_USB_FILE_CHUNK_SIZE: usize = 32 * 1024;
const P4_NATIVE_USB_IO_TIMEOUT: Duration = Duration::from_secs(5);
const P4_NATIVE_USB_ACK_TIMEOUT: Duration = Duration::from_secs(20);
const P4_NATIVE_USB_IDENTITY_TIMEOUT: Duration = Duration::from_secs(2);
const P4_DEVICE_RESPONSE_TIMEOUT: Duration = Duration::from_secs(5);
// Firmware travels over the board's 4 Mbaud CH343 control link on current
// hardware. Force short, drained bursts so the macOS USB-serial stack cannot
// hand the adapter one multi-kilobyte Base64 line that is silently damaged.
const P4_FIRMWARE_SERIAL_WRITE_SLICE_BYTES: usize = 64;
const P4_FIRMWARE_SERIAL_WRITE_GAP: Duration = Duration::from_micros(100);
const P4_FIRMWARE_CHUNK_RETRY_DELAY: Duration = Duration::from_millis(1);

// native USB bulk is the high-speed ESP32-P4 OTG data plane. UART remains as a fallback.
struct NativeUsbP4Transport {
    handle: rusb::DeviceHandle<rusb::GlobalContext>,
    rx_buffer: Vec<u8>,
    next_seq: u32,
}

fn enumerate_native_usb_devices() -> Result<Vec<rusb::Device<rusb::GlobalContext>>, String> {
    let devices =
        rusb::devices().map_err(|error| format!("Native USB enumeration failed: {error}"))?;
    let mut candidates = Vec::new();
    for device in devices.iter() {
        let Ok(descriptor) = device.device_descriptor() else {
            continue;
        };
        if descriptor.vendor_id() == P4_NATIVE_USB_VID
            && descriptor.product_id() == P4_NATIVE_USB_PID
        {
            candidates.push(device);
        }
    }
    if candidates.is_empty() {
        return Err("Native USB device 303A:4040 not found".to_string());
    }
    Ok(candidates)
}

impl NativeUsbP4Transport {
    fn open_device(device: &rusb::Device<rusb::GlobalContext>) -> Result<Self, String> {
        let handle = device
            .open()
            .map_err(|error| format!("Native USB open failed: {error}"))?;
        if handle.active_configuration().ok() != Some(1) {
            let _ = handle.set_active_configuration(1);
        }
        #[cfg(any(target_os = "linux", target_os = "android"))]
        {
            if handle
                .kernel_driver_active(P4_NATIVE_USB_INTERFACE)
                .unwrap_or(false)
            {
                let _ = handle.detach_kernel_driver(P4_NATIVE_USB_INTERFACE);
            }
        }
        handle
            .claim_interface(P4_NATIVE_USB_INTERFACE)
            .map_err(|e| format!("Native USB open failed: {e}"))?;
        Ok(Self {
            handle,
            rx_buffer: Vec::new(),
            next_seq: 1,
        })
    }

    fn open(expected_board_device_id: &str) -> Result<Self, String> {
        let expected_board_device_id = validate_expected_board_device_id(expected_board_device_id)?;
        let devices = enumerate_native_usb_devices()?;
        let mut identities = Vec::new();
        let mut failures = Vec::new();
        for device in &devices {
            let bus = device.bus_number();
            let address = device.address();
            let result =
                Self::open_device(device).and_then(|mut transport| transport.probe_identity());
            match result {
                Ok(identity) => identities.push(NativeUsbCandidateIdentity {
                    bus,
                    address,
                    identity,
                }),
                Err(error) => failures.push(format!("bus {bus} address {address}: {error}")),
            }
        }
        let selected = select_native_usb_candidate(expected_board_device_id, &identities).map_err(
            |error| {
                if failures.is_empty() {
                    error
                } else {
                    format!("{error}；握手失败：{}", failures.join("；"))
                }
            },
        )?;
        let selected_identity = &identities[selected];
        let selected_device = devices
            .iter()
            .find(|device| {
                device.bus_number() == selected_identity.bus
                    && device.address() == selected_identity.address
            })
            .ok_or("Native USB target disappeared after identity selection")?;
        let mut transport = Self::open_device(selected_device)?;
        let confirmed = transport.probe_identity()?;
        if confirmed.board_device_id != expected_board_device_id {
            return Err(format!(
                "Native USB board changed during selection: expected {expected_board_device_id}, got {}",
                confirmed.board_device_id
            ));
        }
        Ok(transport)
    }

    fn send_frame(&mut self, kind: u8, payload: &[u8]) -> Result<(), String> {
        if payload.len() > P4_NATIVE_USB_MAX_PAYLOAD {
            return Err(format!("Native USB payload too large: {}", payload.len()));
        }
        let frame = encode_native_usb_frame(kind, self.next_seq, payload);
        self.next_seq = self.next_seq.wrapping_add(1);
        let mut offset = 0usize;
        while offset < frame.len() {
            let written = self
                .handle
                .write_bulk(
                    P4_NATIVE_USB_EP_OUT,
                    &frame[offset..],
                    P4_NATIVE_USB_IO_TIMEOUT,
                )
                .map_err(|e| format!("Native USB write failed: {e}"))?;
            if written == 0 {
                return Err("Native USB write returned 0 bytes".to_string());
            }
            offset += written;
        }
        Ok(())
    }

    fn send_json_value(&mut self, value: &serde_json::Value) -> Result<(), String> {
        let payload = serde_json::to_vec(value).map_err(|e| e.to_string())?;
        self.send_frame(P4_NATIVE_KIND_JSON, &payload)
    }

    fn probe_identity(&mut self) -> Result<NativeUsbIdentity, String> {
        let nonce = next_native_usb_nonce();
        let payload = serde_json::to_vec(&serde_json::json!({ "nonce": nonce }))
            .map_err(|error| error.to_string())?;
        self.send_frame(P4_NATIVE_KIND_PING, &payload)?;
        let started = Instant::now();
        loop {
            if started.elapsed() >= P4_NATIVE_USB_IDENTITY_TIMEOUT {
                return Err("Native USB identity handshake timed out".to_string());
            }
            let remaining = P4_NATIVE_USB_IDENTITY_TIMEOUT
                .checked_sub(started.elapsed())
                .unwrap_or_else(|| Duration::from_millis(1));
            let frame = match self.read_frame(remaining.min(Duration::from_millis(200))) {
                Ok(frame) => frame,
                Err(error) if error == "Native USB read timeout" => continue,
                Err(error) => return Err(error),
            };
            if frame.kind != P4_NATIVE_KIND_JSON {
                continue;
            }
            let Ok(message) = serde_json::from_slice::<serde_json::Value>(&frame.payload) else {
                continue;
            };
            if message.get("topic").and_then(|value| value.as_str()) != Some("native/pong") {
                continue;
            }
            return parse_native_usb_pong(&frame.payload, &nonce);
        }
    }

    fn best_effort_asset_abort(&mut self, transfer_id: &str) {
        let _ = self.send_json_value(&serde_json::json!({
            "topic": "asset/abort",
            "payload": { "transferId": transfer_id },
        }));
        let _ = self.send_json_value(&serde_json::json!({
            "topic": "asset/patch-commit",
            "payload": {
                "transferId": transfer_id,
                "fileCount": 0,
                "totalBytes": 0,
            },
        }));
    }

    fn read_frame(&mut self, timeout: Duration) -> Result<NativeUsbFrame, String> {
        let started = std::time::Instant::now();
        let mut buf = [0u8; 4096];
        loop {
            if let Some(frame) = try_pop_native_usb_frame(&mut self.rx_buffer)? {
                return Ok(frame);
            }
            let remaining = timeout
                .checked_sub(started.elapsed())
                .unwrap_or_else(|| Duration::from_millis(1));
            if started.elapsed() >= timeout {
                return Err("Native USB read timeout".to_string());
            }
            match self.handle.read_bulk(
                P4_NATIVE_USB_EP_IN,
                &mut buf,
                remaining.min(P4_NATIVE_USB_IO_TIMEOUT),
            ) {
                Ok(n) if n > 0 => self.rx_buffer.extend_from_slice(&buf[..n]),
                Ok(_) => {}
                Err(rusb::Error::Timeout) => {}
                Err(e) => return Err(format!("Native USB read failed: {e}")),
            }
        }
    }

    fn wait_asset_ack(
        &mut self,
        transfer_id: &str,
        phase: &str,
        path: Option<&str>,
        cancel_requested: &AtomicBool,
    ) -> Result<serde_json::Value, String> {
        self.wait_asset_ack_timeout(
            transfer_id,
            phase,
            path,
            cancel_requested,
            P4_NATIVE_USB_ACK_TIMEOUT,
        )
    }

    fn wait_asset_ack_timeout(
        &mut self,
        transfer_id: &str,
        phase: &str,
        path: Option<&str>,
        cancel_requested: &AtomicBool,
        timeout: Duration,
    ) -> Result<serde_json::Value, String> {
        let started = std::time::Instant::now();
        loop {
            if cancel_requested.load(Ordering::SeqCst) {
                self.best_effort_asset_abort(transfer_id);
                return Err(APPEARANCE_SYNC_CANCELLED_ERROR.to_string());
            }
            let remaining = timeout
                .checked_sub(started.elapsed())
                .unwrap_or_else(|| Duration::from_millis(1));
            if started.elapsed() >= timeout {
                return Err(format!("Native USB ack timeout at phase {phase}"));
            }
            let frame = match self.read_frame(remaining.min(Duration::from_millis(100))) {
                Ok(frame) => frame,
                Err(error) if error == "Native USB read timeout" => continue,
                Err(error) => return Err(error),
            };
            if frame.kind != P4_NATIVE_KIND_JSON {
                continue;
            }
            let message: SerialMessage = serde_json::from_slice(&frame.payload)
                .map_err(|e| format!("Native USB JSON parse failed: {e}"))?;
            if message.topic != "asset/ack" {
                continue;
            }
            let payload = message.payload;
            if payload
                .get("transferId")
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                != transfer_id
            {
                continue;
            }
            if payload
                .get("phase")
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                != phase
            {
                continue;
            }
            if let Some(expected_path) = path {
                if payload
                    .get("path")
                    .and_then(|v| v.as_str())
                    .unwrap_or_default()
                    != expected_path
                {
                    continue;
                }
            }
            let ok = payload.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
            if ok {
                return Ok(payload);
            }
            return Err(payload
                .get("error")
                .and_then(|v| v.as_str())
                .unwrap_or("Native USB asset ack failed")
                .to_string());
        }
    }
}

pub fn p4_native_usb_available() -> bool {
    let Ok(devices) = rusb::devices() else {
        return false;
    };
    devices.iter().any(|device| {
        device
            .device_descriptor()
            .map(|descriptor| {
                descriptor.vendor_id() == P4_NATIVE_USB_VID
                    && descriptor.product_id() == P4_NATIVE_USB_PID
            })
            .unwrap_or(false)
    })
}

fn usb_uart_wire_bytes_per_sec(baud: u32) -> u64 {
    u64::from(baud) / 10
}

fn usb_serial_device_prefers_high_speed(device: &UsbDeviceInfo) -> bool {
    if device.vid == 0x1a86 || device.vid == 0x303a {
        return true;
    }
    let identity = format!(
        "{} {} {}",
        device.port_name, device.manufacturer, device.product
    )
    .to_ascii_lowercase();
    [
        "wchusbserial",
        "usb-enhanced-serial",
        "ch340",
        "ch343",
        "ch344",
        "ch347",
    ]
    .iter()
    .any(|token| identity.contains(token))
}

fn serial_baud_candidates_for_device(device: Option<&UsbDeviceInfo>) -> Vec<u32> {
    let high_speed_first = device.is_some_and(usb_serial_device_prefers_high_speed);
    if high_speed_first {
        vec![
            P4_USB_UART_BAUD,
            P4_USB_UART_LEGACY_BAUD,
            DEFAULT_USB_SERIAL_BAUD,
            LEGACY_USB_SERIAL_BAUD,
        ]
    } else {
        vec![
            DEFAULT_USB_SERIAL_BAUD,
            LEGACY_USB_SERIAL_BAUD,
            P4_USB_UART_BAUD,
            P4_USB_UART_LEGACY_BAUD,
        ]
    }
}

fn canonical_runtime_id(runtime: &str) -> String {
    match runtime.trim().to_ascii_lowercase().as_str() {
        "esp-p4" | "esp32-p4" | "esp32_p4" | "p4" => "esp-p4".to_string(),
        "linux" | "linux-board" | "board-runtime" | "board-server-c" | "pet-screen"
        | "raspberry-pi" | "raspberry" | "radxa" => "linux".to_string(),
        other => other.to_string(),
    }
}

fn runtime_from_hello_payload(payload: &serde_json::Value) -> String {
    let explicit = first_json_string(payload, &["runtime", "runtimeKind", "deviceRuntime"]);
    if !explicit.is_empty() {
        let runtime = canonical_runtime_id(&explicit);
        if matches!(runtime.as_str(), "esp-p4" | "linux") {
            return runtime;
        }
    }

    let model =
        first_json_string(payload, &["deviceModel", "model", "boardModel"]).to_ascii_lowercase();
    if model.contains("esp32-p4") || model.contains("esp-p4") {
        return "esp-p4".to_string();
    }
    if model.contains("linux")
        || model.contains("raspberry")
        || model.contains("radxa")
        || model.contains("pet screen")
    {
        return "linux".to_string();
    }

    let capabilities = payload.get("capabilities");
    let asset_formats = capabilities
        .and_then(|value| value.get("assetFormats"))
        .or_else(|| {
            capabilities
                .and_then(|value| value.get("appearance"))
                .and_then(|value| value.get("formats"))
        });
    let p4_asset_format = asset_formats
        .and_then(|value| value.as_array())
        .is_some_and(|formats| {
            formats.iter().any(|format| {
                format
                    .as_str()
                    .is_some_and(|format| format.to_ascii_lowercase().starts_with("p4-"))
            })
        });
    let native_protocol = capabilities
        .and_then(|value| value.get("nativeProtocol"))
        .or_else(|| {
            capabilities
                .and_then(|value| value.get("transport"))
                .and_then(|value| value.get("nativeProtocol"))
        });
    let p4_native_transport = native_protocol
        .and_then(|value| value.as_str())
        .is_some_and(|protocol| protocol.eq_ignore_ascii_case("pet-usb-native-v1"));
    if p4_asset_format || p4_native_transport {
        return "esp-p4".to_string();
    }

    // Current Linux board-runtime hello frames predate the runtime field. The
    // exact protocol signature is still stronger evidence than adapter or COM
    // metadata: an online Pet Manager board id over the declared USB transport.
    let linux_usb_hello = payload
        .get("online")
        .and_then(|value| value.as_bool())
        .is_some_and(|online| online)
        && first_json_string(payload, &["transport"]).eq_ignore_ascii_case("usb")
        && (payload
            .get("tsMs")
            .and_then(|value| value.as_i64())
            .is_some()
            || !first_json_string(payload, &["ts"]).is_empty());
    if linux_usb_hello {
        return "linux".to_string();
    }

    String::new()
}

fn parse_serial_message(line: &str) -> Option<SerialMessage> {
    let root: serde_json::Value = serde_json::from_str(line).ok()?;
    if let Some(topic) = root.get("topic").and_then(|value| value.as_str()) {
        return Some(SerialMessage {
            topic: topic.to_string(),
            payload: root
                .get("payload")
                .cloned()
                .unwrap_or_else(|| serde_json::json!({})),
        });
    }

    let legacy_type = root.get("type").and_then(|value| value.as_str())?;
    let mut payload = root.clone();
    let topic = if legacy_type == "hello_ack" {
        if let Some(object) = payload.as_object_mut() {
            object.insert("runtime".to_string(), serde_json::json!("linux"));
            object.insert(
                "deviceModel".to_string(),
                serde_json::json!("Linux board runtime"),
            );
            object.insert(
                "wireProtocol".to_string(),
                serde_json::json!("pet-usb-legacy-v1"),
            );
        }
        "hello".to_string()
    } else {
        legacy_type.to_string()
    };
    Some(SerialMessage { topic, payload })
}

fn validate_hello_message(message: &mut SerialMessage) -> Result<(), String> {
    if message.topic != "hello" {
        return Err("device did not return a hello message".to_string());
    }
    let board_device_id = first_json_string(
        &message.payload,
        &["boardDeviceId", "localDeviceId", "deviceId"],
    );
    if board_device_id.is_empty() {
        return Err("hello did not include a board device id".to_string());
    }
    let runtime = runtime_from_hello_payload(&message.payload);
    if !matches!(runtime.as_str(), "esp-p4" | "linux") {
        return Err("hello did not identify a supported device runtime".to_string());
    }
    if let Some(object) = message.payload.as_object_mut() {
        object.insert("runtime".to_string(), serde_json::json!(runtime));
    }
    Ok(())
}

fn serial_probe_handshake_messages(desktop_device_id: &str) -> [serde_json::Value; 2] {
    [
        serde_json::json!({
            // P4 replies to bind immediately. Linux board-runtime ignores it
            // and keeps its short hello heartbeat active; sending ack here
            // would suppress that heartbeat before identity is verified.
            "topic": "bind",
            "payload": { "desktopDeviceId": desktop_device_id },
        }),
        serde_json::json!({
            "v": 1,
            "type": "hello",
            "desktopDeviceId": desktop_device_id,
            "namespace": "desk",
        }),
    ]
}

fn send_serial_probe_handshakes(
    writer: &mut dyn serialport::SerialPort,
    desktop_device_id: &str,
) -> Result<(), String> {
    for message in serial_probe_handshake_messages(desktop_device_id) {
        let mut line = serde_json::to_string(&message).map_err(|error| error.to_string())?;
        line.push('\n');
        writer
            .write_all(line.as_bytes())
            .map_err(|error| format!("serial handshake write failed: {error}"))?;
    }
    writer
        .flush()
        .map_err(|error| format!("serial handshake flush failed: {error}"))
}

fn probe_serial_port(
    port_name: &str,
    baud_candidates: &[u32],
    desktop_device_id: &str,
    probe_timeout: Duration,
) -> Result<ProbedSerialPort, String> {
    let mut failures = Vec::new();
    for baud in baud_candidates.iter().copied() {
        let (mut writer, reader) = match open_serial_pair_with_retry(port_name, baud) {
            Ok(ports) => ports,
            Err(error) => {
                failures.push(error);
                continue;
            }
        };

        let _ = writer.write_data_terminal_ready(true);
        let _ = writer.write_request_to_send(true);
        if let Err(error) = send_serial_probe_handshakes(writer.as_mut(), desktop_device_id) {
            failures.push(format!("{baud} baud: {error}"));
            continue;
        }

        let started = Instant::now();
        let mut next_handshake_at = started + SERIAL_PROBE_HANDSHAKE_RETRY_INTERVAL;
        let mut reader = BufReader::new(reader);
        let mut line_buffer = Vec::with_capacity(32 * 1024);
        while started.elapsed() < probe_timeout {
            if Instant::now() >= next_handshake_at {
                if let Err(error) = send_serial_probe_handshakes(writer.as_mut(), desktop_device_id)
                {
                    failures.push(format!("{baud} baud handshake retry failed: {error}"));
                    break;
                }
                next_handshake_at = Instant::now() + SERIAL_PROBE_HANDSHAKE_RETRY_INTERVAL;
            }
            let line = match read_serial_line_lossy(&mut reader, &mut line_buffer) {
                Ok(Some(line)) => line,
                Ok(None) => break,
                Err(error) if error.kind() == std::io::ErrorKind::TimedOut => continue,
                Err(error) => {
                    failures.push(format!("{baud} baud read failed: {error}"));
                    break;
                }
            };
            let Some(mut message) = parse_serial_message(&line) else {
                log_raw_serial_line(&line);
                continue;
            };
            if message.topic != "hello" {
                continue;
            }
            if let Err(error) = validate_hello_message(&mut message) {
                failures.push(format!("{baud} baud rejected hello: {error}"));
                continue;
            }
            return Ok(ProbedSerialPort {
                writer,
                reader: reader.into_inner(),
                baud,
                hello: message,
            });
        }
        failures.push(format!("{baud} baud: no valid protocol hello"));
    }

    Err(format!(
        "{port_name} did not identify as a supported Pet Manager device ({})",
        failures.join("; ")
    ))
}

pub fn sync_appearance_p4_native<F>(
    appearance_dir: &std::path::Path,
    _app_data_dir: &std::path::Path,
    expected_board_device_id: &str,
    cancel_requested: &AtomicBool,
    on_progress: F,
) -> Result<(u32, u64, bool), String>
where
    F: Fn(u32, u32, u64, u64),
{
    if cancel_requested.load(Ordering::SeqCst) {
        return Err(APPEARANCE_SYNC_CANCELLED_ERROR.to_string());
    }
    let mut transport = NativeUsbP4Transport::open(expected_board_device_id)?;
    let assets = load_prepared_p4_appearance_pack(appearance_dir)?;
    if cancel_requested.load(Ordering::SeqCst) {
        return Err(APPEARANCE_SYNC_CANCELLED_ERROR.to_string());
    }

    let total_files: u32 = assets.len() as u32;
    let total_bytes: u64 = assets
        .iter()
        .filter_map(|asset| std::fs::metadata(&asset.source_path).ok())
        .map(|meta| meta.len())
        .sum();
    let pack_id = p4_pack_id_from_assets(&assets)?;
    let transfer_id = format!(
        "p4-native-{}",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis()
    );
    transport.send_json_value(&serde_json::json!({
        "topic": "asset/slot-query",
        "payload": {"transferId": transfer_id}
    }))?;
    match transport
        .wait_asset_ack_timeout(
            &transfer_id,
            "slot-query",
            None,
            cancel_requested,
            ASSET_STAT_TIMEOUT,
        )
        .and_then(|ack| parse_p4_appearance_slot_state(&ack))
    {
        Ok(state) => {
            if let Some(cached) = state.slots.iter().find(|slot| slot.pack_id == pack_id) {
                if state.active_slot == Some(cached.slot) {
                    eprintln!(
                        "[usb-p4-native-ota] skip transfer: active slot already has pack_id={}",
                        pack_id
                    );
                    on_progress(0, total_files, 0, total_bytes);
                    return Ok((0, 0, false));
                }
                transport.send_json_value(&serde_json::json!({
                    "topic": "asset/activate",
                    "payload": {
                        "transferId": transfer_id,
                        "slot": cached.slot,
                        "packId": pack_id,
                    }
                }))?;
                transport.wait_asset_ack_timeout(
                    &transfer_id,
                    "activate",
                    None,
                    cancel_requested,
                    ASSET_STAT_TIMEOUT,
                )?;
                eprintln!(
                    "[usb-p4-native-ota] reactivated cached slot={} pack_id={}",
                    cached.slot, pack_id
                );
                on_progress(0, total_files, 0, total_bytes);
                return Ok((0, 0, true));
            }
        }
        Err(error) => {
            eprintln!(
                "[usb-p4-native-ota] slot reuse unavailable; falling back to full sync: {}",
                error
            );
        }
    }
    ensure_p4_native_full_pack_supported(&assets)?;
    eprintln!(
        "[usb-p4-native-ota] begin native USB bulk transfer_id={} files={} bytes={}",
        transfer_id, total_files, total_bytes
    );
    transfer_log::record(
        "appearance",
        "native_transaction_started",
        serde_json::json!({
            "transferId": transfer_id.as_str(),
            "boardDeviceId": expected_board_device_id,
            "files": total_files,
            "bytes": total_bytes,
            "transport": "native-usb-v1",
            "packId": pack_id.as_str(),
        }),
    );

    let transfer_result = (|| {
        let mut file_count = 0u32;
        let mut byte_count = 0u64;
        on_progress(0, total_files, 0, total_bytes);
        for asset in assets.iter() {
            if cancel_requested.load(Ordering::SeqCst) {
                return Err(APPEARANCE_SYNC_CANCELLED_ERROR.to_string());
            }
            let bytes = std::fs::read(&asset.source_path)
                .map_err(|e| format!("读取 P4 资源失败 {}: {}", asset.source_path.display(), e))?;
            let file_size = bytes.len() as u64;
            let checksum = asset_checksum_hex(&bytes);
            transfer_log::record(
                "appearance",
                "native_file_started",
                serde_json::json!({
                    "transferId": transfer_id.as_str(),
                    "devicePath": asset.device_path.as_str(),
                    "sourcePath": &asset.source_path,
                    "size": file_size,
                    "checksum": checksum.as_str(),
                }),
            );
            let metadata = serde_json::to_vec(&serde_json::json!({
                "transferId": transfer_id.as_str(),
                "path": asset.device_path.as_str(),
                "size": file_size,
                "checksum": checksum.as_str(),
            }))
            .map_err(|e| e.to_string())?;
            transport.send_frame(P4_NATIVE_KIND_FILE_BEGIN, &metadata)?;
            transport.wait_asset_ack(
                &transfer_id,
                "file-begin",
                Some(&asset.device_path),
                cancel_requested,
            )?;

            for (index, chunk) in bytes.chunks(P4_NATIVE_USB_FILE_CHUNK_SIZE).enumerate() {
                if cancel_requested.load(Ordering::SeqCst) {
                    return Err(APPEARANCE_SYNC_CANCELLED_ERROR.to_string());
                }
                transport.send_frame(P4_NATIVE_KIND_FILE_DATA, chunk)?;
                let chunk_bytes_sent = std::cmp::min(
                    ((index + 1) * P4_NATIVE_USB_FILE_CHUNK_SIZE) as u64,
                    file_size,
                );
                on_progress(
                    file_count,
                    total_files,
                    byte_count + chunk_bytes_sent,
                    total_bytes,
                );
            }

            let end_metadata = serde_json::to_vec(&serde_json::json!({
                "transferId": transfer_id.as_str(),
                "path": asset.device_path.as_str(),
            }))
            .map_err(|e| e.to_string())?;
            transport.send_frame(P4_NATIVE_KIND_FILE_END, &end_metadata)?;
            transport.wait_asset_ack(
                &transfer_id,
                "file",
                Some(&asset.device_path),
                cancel_requested,
            )?;

            file_count += 1;
            byte_count += file_size;
            on_progress(file_count, total_files, byte_count, total_bytes);
        }

        let commit_metadata = serde_json::to_vec(&serde_json::json!({
            "transferId": transfer_id.as_str(),
            "fileCount": file_count,
            "totalBytes": byte_count,
        }))
        .map_err(|e| e.to_string())?;
        transport.send_frame(P4_NATIVE_KIND_COMMIT, &commit_metadata)?;
        transport.wait_asset_ack(&transfer_id, "commit", None, cancel_requested)?;
        eprintln!(
            "[usb-p4-native-ota] commit transfer_id={} sent_files={} sent_bytes={}",
            transfer_id, file_count, byte_count
        );
        transfer_log::record(
            "appearance",
            "native_transaction_committed",
            serde_json::json!({
                "transferId": transfer_id.as_str(),
                "files": file_count,
                "bytes": byte_count,
            }),
        );

        Ok((file_count, byte_count, false))
    })();
    if let Err(error) = &transfer_result {
        transfer_log::record(
            "appearance",
            "native_transaction_failed",
            serde_json::json!({
                "transferId": transfer_id.as_str(),
                "error": error,
            }),
        );
        transport.best_effort_asset_abort(&transfer_id);
    }
    transfer_result
}

fn ensure_p4_native_full_pack_supported(assets: &[AppearanceAssetEntry]) -> Result<(), String> {
    let raw_video_count = assets
        .iter()
        .filter(|asset| {
            asset.device_path.ends_with(".h264") || asset.device_path.ends_with(".mjpg")
        })
        .count();
    if raw_video_count == 0 {
        return Ok(());
    }

    Err(format!(
        "原生 USB 当前只能安全激活设备上的缓存形象，不能完整写入包含 {raw_video_count} 个 raw 视频的形象包；请将 Type-C 切回 USB-UART 模式后重试"
    ))
}

#[derive(Clone)]
pub struct UsbSerialManager {
    connection: Arc<Mutex<Option<UsbConnection>>>,
    desktop_device_id: Arc<Mutex<String>>,
    connect_guard: Arc<Mutex<()>>,
    asset_transfer_guard: Arc<Mutex<()>>,
    appearance_sync_active: Arc<AtomicBool>,
    appearance_sync_cancel_requested: Arc<AtomicBool>,
    next_connection_id: Arc<AtomicU64>,
    asset_ack_waiters: Arc<Mutex<Vec<AssetAckWaiter>>>,
    widget_ack_waiters: Arc<Mutex<Vec<WidgetAckWaiter>>>,
    firmware_ack_waiters: Arc<Mutex<Vec<FirmwareAckWaiter>>>,
    device_response_waiters: Arc<Mutex<Vec<DeviceResponseWaiter>>>,
}

fn read_serial_line_lossy<R: BufRead>(
    reader: &mut R,
    buffer: &mut Vec<u8>,
) -> std::io::Result<Option<String>> {
    buffer.clear();
    if reader.read_until(b'\n', buffer)? == 0 {
        return Ok(None);
    }
    while matches!(buffer.last(), Some(b'\n' | b'\r')) {
        buffer.pop();
    }
    Ok(Some(String::from_utf8_lossy(buffer).into_owned()))
}

fn log_raw_serial_line(line: &str) {
    if std::env::var("P4_SERIAL_LOG_RAW").as_deref() != Ok("1") {
        return;
    }
    let line = line.trim_matches('\0').trim();
    if !line.is_empty() {
        eprintln!("[usb_serial/raw] {line}");
    }
}

fn validate_expected_board_device_id(expected_board_device_id: &str) -> Result<&str, String> {
    let expected_board_device_id = expected_board_device_id.trim();
    if expected_board_device_id.is_empty() {
        return Err("expectedBoardDeviceId is required".to_string());
    }
    Ok(expected_board_device_id)
}

impl UsbSerialManager {
    pub fn new() -> Self {
        Self {
            connection: Arc::new(Mutex::new(None)),
            desktop_device_id: Arc::new(Mutex::new(String::new())),
            connect_guard: Arc::new(Mutex::new(())),
            asset_transfer_guard: Arc::new(Mutex::new(())),
            appearance_sync_active: Arc::new(AtomicBool::new(false)),
            appearance_sync_cancel_requested: Arc::new(AtomicBool::new(false)),
            next_connection_id: Arc::new(AtomicU64::new(0)),
            asset_ack_waiters: Arc::new(Mutex::new(Vec::new())),
            widget_ack_waiters: Arc::new(Mutex::new(Vec::new())),
            firmware_ack_waiters: Arc::new(Mutex::new(Vec::new())),
            device_response_waiters: Arc::new(Mutex::new(Vec::new())),
        }
    }

    pub fn set_desktop_device_id(&self, id: &str) {
        if let Ok(mut did) = self.desktop_device_id.lock() {
            *did = id.to_string();
        }
    }

    pub fn begin_appearance_sync(&self) -> Result<(), String> {
        self.appearance_sync_active
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .map_err(|_| "已有形象素材正在传输，请等待完成或先中断当前传输".to_string())?;
        self.appearance_sync_cancel_requested
            .store(false, Ordering::SeqCst);
        Ok(())
    }

    pub fn cancel_appearance_sync(&self) -> bool {
        if !self.appearance_sync_active.load(Ordering::SeqCst) {
            return false;
        }
        self.appearance_sync_cancel_requested
            .store(true, Ordering::SeqCst);
        true
    }

    pub fn finish_appearance_sync(&self) {
        self.appearance_sync_active.store(false, Ordering::SeqCst);
        self.appearance_sync_cancel_requested
            .store(false, Ordering::SeqCst);
    }

    fn ensure_appearance_sync_not_cancelled(
        &self,
        transfer_id: Option<&str>,
    ) -> Result<(), String> {
        if !self.appearance_sync_cancel_requested.load(Ordering::SeqCst) {
            return Ok(());
        }
        if let Some(transfer_id) = transfer_id {
            self.best_effort_asset_abort(transfer_id);
        }
        Err(APPEARANCE_SYNC_CANCELLED_ERROR.to_string())
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
        prioritize_usb_serial_devices(devices)
    }

    /// Connect to a USB serial device
    pub fn connect<F>(&self, port_name: &str, on_message: F) -> Result<(), String>
    where
        F: Fn(String, serde_json::Value) + Send + 'static,
    {
        self.connect_with_probe_timeout(port_name, SERIAL_PROBE_TIMEOUT, on_message)
    }

    pub fn connect_for_auto<F>(&self, port_name: &str, on_message: F) -> Result<(), String>
    where
        F: Fn(String, serde_json::Value) + Send + 'static,
    {
        self.connect_with_probe_timeout(port_name, SERIAL_AUTO_PROBE_TIMEOUT, on_message)
    }

    fn connect_with_probe_timeout<F>(
        &self,
        port_name: &str,
        probe_timeout: Duration,
        on_message: F,
    ) -> Result<(), String>
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

        let detected_device = self
            .scan_devices()
            .into_iter()
            .find(|device| device.port_name.eq_ignore_ascii_case(port_name));
        let baud_candidates = serial_baud_candidates_for_device(detected_device.as_ref());
        let desktop_id = self
            .desktop_device_id
            .lock()
            .map(|value| value.clone())
            .unwrap_or_default();
        let probed = probe_serial_port(port_name, &baud_candidates, &desktop_id, probe_timeout)?;
        eprintln!(
            "[usb_serial] verified {} at {} baud as runtime={} board={} firmware={}",
            port_name,
            probed.baud,
            runtime_from_hello_payload(&probed.hello.payload),
            first_json_string(
                &probed.hello.payload,
                &["boardDeviceId", "localDeviceId", "deviceId"]
            ),
            first_json_string(&probed.hello.payload, &["fw", "firmware", "version"])
        );

        let connection_id = self.next_connection_id.fetch_add(1, Ordering::SeqCst) + 1;
        let cancel_reader = Arc::new(AtomicBool::new(false));

        let mut connection = UsbConnection {
            connection_id,
            port_name: port_name.to_string(),
            baud_rate: probed.baud,
            writer: Box::new(BufWriter::with_capacity(256 * 1024, probed.writer)),
            board_device_id: String::new(),
            runtime: String::new(),
            device_model: String::new(),
            firmware: String::new(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 0,
            wire_protocol: String::new(),
            capabilities: serde_json::Value::Null,
            connected: true,
            cancel_reader: Arc::clone(&cancel_reader),
        };
        apply_hello_payload_to_connection(&mut connection, &probed.hello.payload);
        let initial_hello = probed.hello.clone();
        let reader_port = probed.reader;

        {
            let mut conn = self.connection.lock().map_err(|e| e.to_string())?;
            *conn = Some(connection);
        }
        on_message(initial_hello.topic, initial_hello.payload);

        // Start reader thread
        let conn_ref = Arc::clone(&self.connection);
        let desktop_id_ref = Arc::clone(&self.desktop_device_id);
        let asset_ack_waiters = Arc::clone(&self.asset_ack_waiters);
        let widget_ack_waiters = Arc::clone(&self.widget_ack_waiters);
        let firmware_ack_waiters = Arc::clone(&self.firmware_ack_waiters);
        let device_response_waiters = Arc::clone(&self.device_response_waiters);

        thread::spawn(move || {
            let mut reader = BufReader::new(reader_port);
            let mut line_buffer = Vec::with_capacity(32 * 1024);
            loop {
                if cancel_reader.load(Ordering::SeqCst) {
                    break;
                }
                let line = match read_serial_line_lossy(&mut reader, &mut line_buffer) {
                    Ok(Some(line)) => line,
                    Ok(None) => break,
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

                let mut msg = match parse_serial_message(&line) {
                    Some(message) => message,
                    None => {
                        log_raw_serial_line(&line);
                        continue;
                    }
                };

                resolve_asset_ack(&asset_ack_waiters, &msg.topic, &msg.payload);
                resolve_widget_ack(&widget_ack_waiters, &msg.topic, &msg.payload);
                resolve_firmware_ack(&firmware_ack_waiters, &msg.topic, &msg.payload);
                resolve_device_response(&device_response_waiters, &msg.topic, &msg.payload);

                // Handle hello -> refresh identity and send ack. Never block the
                // reader behind an in-flight bulk write: send_inner holds this
                // mutex while flushing, and the board cannot finish consuming
                // that write if its TX path is back-pressured by an unread
                // hello/asset ACK. A later hello will retry this best-effort
                // refresh after the writer releases the connection.
                if msg.topic == "hello" {
                    if let Err(error) = validate_hello_message(&mut msg) {
                        eprintln!("[usb_serial] ignored invalid hello: {error}");
                        continue;
                    }
                    let desktop_id = desktop_id_ref.lock().map(|d| d.clone()).unwrap_or_default();
                    let ack = format!(
                        "{{\"topic\":\"ack\",\"payload\":{{\"desktopDeviceId\":\"{}\"}}}}\n",
                        desktop_id
                    );
                    if let Ok(mut conn) = conn_ref.try_lock() {
                        if let Some(ref mut c) = *conn {
                            if c.connection_id == connection_id {
                                apply_hello_payload_to_connection(c, &msg.payload);
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
        self.send_inner(topic, payload, true, None, None)
    }

    /// Send without flush - for streaming bulk data.
    fn send_no_flush(&self, topic: &str, payload: &serde_json::Value) -> Result<(), String> {
        self.send_inner(topic, payload, false, None, None)
    }

    pub(crate) fn send_to_board(
        &self,
        expected_board_device_id: &str,
        topic: &str,
        payload: &serde_json::Value,
    ) -> Result<(), String> {
        self.send_inner(topic, payload, true, Some(expected_board_device_id), None)
    }

    fn send_firmware_to_board(
        &self,
        expected_board_device_id: &str,
        topic: &str,
        payload: &serde_json::Value,
    ) -> Result<(), String> {
        self.send_inner(
            topic,
            payload,
            true,
            Some(expected_board_device_id),
            Some(P4_FIRMWARE_SERIAL_WRITE_SLICE_BYTES),
        )
    }

    fn send_inner(
        &self,
        topic: &str,
        payload: &serde_json::Value,
        flush: bool,
        expected_board_device_id: Option<&str>,
        write_slice_bytes: Option<usize>,
    ) -> Result<(), String> {
        let mut conn = self.connection.lock().map_err(|e| e.to_string())?;
        let conn = conn.as_mut().ok_or("Not connected")?;

        if !conn.connected {
            return Err("Connection lost".to_string());
        }
        if let Some(expected_board_device_id) = expected_board_device_id {
            let expected_board_device_id =
                validate_expected_board_device_id(expected_board_device_id)?;
            if conn.board_device_id.is_empty() {
                return Err("connected USB board identity is not available yet".to_string());
            }
            if conn.board_device_id != expected_board_device_id {
                return Err(format!(
                    "connected USB board changed: expected {expected_board_device_id}, got {}",
                    conn.board_device_id
                ));
            }
        }

        let msg = serde_json::json!({
            "topic": topic,
            "payload": payload,
        });

        let mut line = serde_json::to_string(&msg).map_err(|e| e.to_string())?;
        line.push('\n');

        let bytes = line.as_bytes();
        if let Some(write_slice_bytes) = write_slice_bytes.filter(|size| *size > 0) {
            let chunks = bytes.chunks(write_slice_bytes);
            let chunk_count = chunks.len();
            for (index, chunk) in chunks.enumerate() {
                if let Err(error) = conn.writer.write_all(chunk) {
                    conn.connected = false;
                    return Err(format!("Write failed: {}", error));
                }
                // The writer is buffered. Flush every slice, not only the full
                // JSON line, otherwise the intended UART pacing is lost.
                if let Err(error) = conn.writer.flush() {
                    conn.connected = false;
                    return Err(format!("Flush failed: {}", error));
                }
                if index + 1 < chunk_count {
                    thread::sleep(P4_FIRMWARE_SERIAL_WRITE_GAP);
                }
            }
        } else {
            if let Err(error) = conn.writer.write_all(bytes) {
                conn.connected = false;
                return Err(format!("Write failed: {}", error));
            }
            if flush {
                if let Err(error) = conn.writer.flush() {
                    conn.connected = false;
                    return Err(format!("Flush failed: {}", error));
                }
            }
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

    pub fn query_diagnostics(
        &self,
        expected_board_device_id: &str,
    ) -> Result<serde_json::Value, String> {
        self.send_device_request_and_wait(
            expected_board_device_id,
            "diagnostics/query",
            "diagnostics/status",
        )
    }

    pub fn query_button_config(
        &self,
        expected_board_device_id: &str,
    ) -> Result<serde_json::Value, String> {
        self.send_device_request_and_wait(
            expected_board_device_id,
            "input/config-query",
            "input/config-state",
        )
    }

    pub fn reset_input_config(
        &self,
        expected_board_device_id: &str,
    ) -> Result<serde_json::Value, String> {
        self.send_device_request_and_wait(
            expected_board_device_id,
            "system/reset-inputs",
            "diagnostics/action",
        )
    }

    pub fn reboot_device(
        &self,
        expected_board_device_id: &str,
    ) -> Result<serde_json::Value, String> {
        self.send_device_request_and_wait(
            expected_board_device_id,
            "system/reboot",
            "diagnostics/action",
        )
    }

    pub fn query_widget_inventory(
        &self,
        expected_board_device_id: &str,
    ) -> Result<serde_json::Value, String> {
        let _transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        let status = self.connected_board_status(expected_board_device_id)?;
        ensure_widget_inventory_supported(&status)?;
        self.send_device_request_and_wait(
            expected_board_device_id,
            "widget/list",
            "widget/inventory",
        )
    }

    fn send_device_request_and_wait(
        &self,
        expected_board_device_id: &str,
        request_topic: &str,
        response_topic: &str,
    ) -> Result<serde_json::Value, String> {
        let expected_board_device_id = validate_expected_board_device_id(expected_board_device_id)?;
        let request_id = format!("device-request-{}", uuid::Uuid::new_v4());
        let (sender, receiver) = mpsc::channel();
        self.device_response_waiters
            .lock()
            .map_err(|error| error.to_string())?
            .push(DeviceResponseWaiter {
                request_id: request_id.clone(),
                response_topic: response_topic.to_string(),
                sender,
            });
        if let Err(error) = self.send_to_board(
            expected_board_device_id,
            request_topic,
            &serde_json::json!({ "requestId": request_id }),
        ) {
            self.remove_device_response_waiter(&request_id, response_topic);
            return Err(error);
        }
        let response = receiver
            .recv_timeout(P4_DEVICE_RESPONSE_TIMEOUT)
            .map_err(|_| {
                self.remove_device_response_waiter(&request_id, response_topic);
                format!("device response timed out for {request_topic}")
            })?;
        let response_board_device_id = response
            .get("boardDeviceId")
            .and_then(|value| value.as_str())
            .unwrap_or_default();
        if !response_board_device_id.is_empty()
            && response_board_device_id != expected_board_device_id
        {
            return Err(format!(
                "device response board mismatch: expected {expected_board_device_id}, got {response_board_device_id}"
            ));
        }
        if response.get("ok").and_then(|value| value.as_bool()) == Some(false) {
            return Err(response
                .get("message")
                .or_else(|| response.get("error"))
                .and_then(|value| value.as_str())
                .unwrap_or("device rejected the request")
                .to_string());
        }
        Ok(response)
    }

    fn remove_device_response_waiter(&self, request_id: &str, response_topic: &str) {
        if let Ok(mut waiters) = self.device_response_waiters.lock() {
            waiters.retain(|waiter| {
                waiter.request_id != request_id || waiter.response_topic != response_topic
            });
        }
    }

    pub fn update_firmware<F, R>(
        &self,
        firmware_path: &Path,
        expected_board_device_id: &str,
        on_progress: F,
        mut reconnect: R,
    ) -> Result<FirmwareUpdateResult, String>
    where
        F: Fn(u64, u64, &str),
        R: FnMut() -> Result<(), String>,
    {
        let expected_board_device_id =
            validate_expected_board_device_id(expected_board_device_id)?.to_string();
        let _transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        let status = self.connected_board_status(&expected_board_device_id)?;
        if !status.runtime.eq_ignore_ascii_case("esp-p4") {
            return Err(format!(
                "firmware OTA is only supported by ESP32-P4, connected runtime is {}",
                status.runtime
            ));
        }
        let preferred_chunk_size =
            preferred_firmware_chunk_size(status.protocol_schema, &status.capabilities);
        // A desktop crash or a force-stopped hardware test can leave the
        // board's transactional appearance receiver active. Clear that stale
        // state before firmware/begin so a valid A/B firmware update does not
        // remain permanently blocked until the board is power-cycled.
        let appearance_abort_id = format!("firmware-preflight-{}", uuid::Uuid::new_v4());
        if let Err(error) = self.send_asset_command_and_wait_timeout(
            "asset/abort",
            &serde_json::json!({ "transferId": appearance_abort_id }),
            &appearance_abort_id,
            "abort",
            None,
            None,
            ASSET_STAT_TIMEOUT,
        ) {
            eprintln!(
                "[usb-firmware-ota] appearance preflight abort was not acknowledged: {}",
                error
            );
            self.best_effort_asset_abort(&appearance_abort_id);
        }

        let firmware = std::fs::read(firmware_path)
            .map_err(|error| format!("read firmware {}: {error}", firmware_path.display()))?;
        if firmware.is_empty() || firmware.len() > P4_FIRMWARE_MAX_IMAGE_SIZE {
            return Err(format!(
                "firmware image must be 1..={} bytes, got {}",
                P4_FIRMWARE_MAX_IMAGE_SIZE,
                firmware.len()
            ));
        }
        let descriptor = parse_esp_idf_app_descriptor(&firmware)?;

        let initial_diagnostics = self.query_diagnostics(&expected_board_device_id)?;
        let baseline_boot_count = initial_diagnostics
            .get("bootCount")
            .and_then(|value| value.as_u64())
            .ok_or("device diagnostics did not include bootCount")?;
        let initial_runtime = initial_diagnostics
            .get("runtime")
            .and_then(|value| value.as_object())
            .ok_or("device diagnostics did not include runtime")?;
        let original_version = initial_runtime
            .get("firmware")
            .and_then(|value| value.as_str())
            .unwrap_or(&status.firmware)
            .to_string();
        let original_partition = initial_runtime
            .get("runningPartition")
            .and_then(|value| value.as_str())
            .unwrap_or_default()
            .to_string();

        let sha256 = format!("{:x}", Sha256::digest(&firmware));
        let transfer_id = format!("firmware-{}", uuid::Uuid::new_v4());
        let total_bytes = firmware.len() as u64;
        on_progress(0, total_bytes, "begin");

        let begin_payload = serde_json::json!({
            "transferId": transfer_id,
            "size": total_bytes,
            "sha256": sha256,
        });
        let begin_ack =
            self.begin_firmware_transfer(&expected_board_device_id, &begin_payload, &transfer_id)?;
        let target_partition = begin_ack
            .get("targetPartition")
            .and_then(|value| value.as_str())
            .filter(|value| !value.is_empty())
            .ok_or_else(|| {
                self.best_effort_firmware_abort(&expected_board_device_id, &transfer_id);
                "firmware begin acknowledgement did not include targetPartition".to_string()
            })?
            .to_string();

        let upload_started = Instant::now();
        let mut upload_attempts = 0usize;
        let mut upload_corruption_rejections = 0usize;
        let mut upload_fallbacks = 0usize;
        let transfer_result = (|| {
            let mut offset = 0usize;
            let mut sequence = 0u64;
            let mut chunk_size = preferred_chunk_size;
            let mut successful_chunks_at_size = 0usize;
            while offset < firmware.len() {
                let chunk_end = offset.saturating_add(chunk_size).min(firmware.len());
                let chunk = &firmware[offset..chunk_end];
                let received_bytes = chunk_end as u64;
                let payload = serde_json::json!({
                    "transferId": transfer_id,
                    "seq": sequence,
                    "decodedSize": chunk.len(),
                    "data": base64::engine::general_purpose::STANDARD.encode(chunk),
                });
                let mut chunk_result = Err(FirmwareCommandError::Send(
                    "firmware chunk was not attempted".to_string(),
                ));
                let mut fallback_size = None;
                let mut corruption_rejections = 0usize;
                for attempt in 1..=P4_FIRMWARE_CHUNK_MAX_ATTEMPTS {
                    upload_attempts += 1;
                    chunk_result = self.send_firmware_command_and_wait(
                        &expected_board_device_id,
                        "firmware/chunk",
                        &payload,
                        &transfer_id,
                        "chunk",
                        sequence + 1,
                        received_bytes,
                        P4_FIRMWARE_CHUNK_ACK_TIMEOUT,
                    );
                    if chunk_result.is_ok() {
                        break;
                    }
                    if matches!(
                        chunk_result.as_ref().unwrap_err(),
                        FirmwareCommandError::Rejected(message)
                            if message.contains("firmware chunk base64 mismatch")
                    ) {
                        upload_corruption_rejections += 1;
                        corruption_rejections += 1;
                        if corruption_rejections >= P4_FIRMWARE_CORRUPTION_RETRIES_BEFORE_FALLBACK {
                            if let Some(smaller_size) = firmware_corruption_fallback_size(
                                chunk_size,
                                chunk_result.as_ref().unwrap_err(),
                            ) {
                                fallback_size = Some(smaller_size);
                                upload_fallbacks += 1;
                                eprintln!(
                                    "[usb-firmware-ota] repeated payload corruption at seq={sequence}; reducing decoded chunks from {chunk_size} to {smaller_size} bytes"
                                );
                                break;
                            }
                        }
                    }
                    eprintln!(
                        "[usb-firmware-ota] retry seq={} attempt={}/{} error={}",
                        sequence,
                        attempt,
                        P4_FIRMWARE_CHUNK_MAX_ATTEMPTS,
                        chunk_result.as_ref().unwrap_err()
                    );
                    if attempt < P4_FIRMWARE_CHUNK_MAX_ATTEMPTS {
                        thread::sleep(P4_FIRMWARE_CHUNK_RETRY_DELAY);
                    }
                }
                if let Some(smaller_size) = fallback_size {
                    chunk_size = smaller_size;
                    successful_chunks_at_size = 0;
                    continue;
                }
                chunk_result.map_err(|error| {
                    format!(
                        "firmware upload failed at seq {sequence}, offset {offset}, chunk {chunk_size}: {error}"
                    )
                })?;
                on_progress(received_bytes, total_bytes, "upload");
                offset = chunk_end;
                sequence += 1;
                successful_chunks_at_size += 1;
                if let Some(larger_size) = firmware_recovery_chunk_size(
                    chunk_size,
                    preferred_chunk_size,
                    successful_chunks_at_size,
                ) {
                    eprintln!(
                        "[usb-firmware-ota] serial link stable for {successful_chunks_at_size} chunks; restoring decoded chunks from {chunk_size} to {larger_size} bytes"
                    );
                    chunk_size = larger_size;
                    successful_chunks_at_size = 0;
                }
            }
            Ok::<u64, String>(sequence)
        })();

        let expected_next_sequence = match transfer_result {
            Ok(sequence) => {
                eprintln!(
                    "[usb-firmware-ota] upload complete bytes={} chunks={} attempts={} corruption_rejections={} fallbacks={} elapsed_ms={}",
                    total_bytes,
                    sequence,
                    upload_attempts,
                    upload_corruption_rejections,
                    upload_fallbacks,
                    upload_started.elapsed().as_millis()
                );
                sequence
            }
            Err(error) => {
                self.best_effort_firmware_abort(&expected_board_device_id, &transfer_id);
                return Err(error);
            }
        };

        on_progress(total_bytes, total_bytes, "verify");
        let commit_payload = serde_json::json!({ "transferId": transfer_id });
        let mut commit_ack = None;
        let mut last_commit_delivery_error = String::new();
        for attempt in 1..=P4_FIRMWARE_COMMIT_MAX_ATTEMPTS {
            match self.send_firmware_command_and_wait(
                &expected_board_device_id,
                "firmware/commit",
                &commit_payload,
                &transfer_id,
                "commit",
                expected_next_sequence,
                total_bytes,
                P4_FIRMWARE_COMMIT_ACK_TIMEOUT,
            ) {
                Ok(ack) => {
                    commit_ack = Some(ack);
                    break;
                }
                Err(FirmwareCommandError::Rejected(error)) => {
                    self.best_effort_firmware_abort(&expected_board_device_id, &transfer_id);
                    return Err(error);
                }
                Err(error) => {
                    last_commit_delivery_error = error.to_string();
                    eprintln!(
                        "[usb-firmware-ota] commit delivery attempt={}/{} error={}",
                        attempt, P4_FIRMWARE_COMMIT_MAX_ATTEMPTS, last_commit_delivery_error
                    );
                    if attempt < P4_FIRMWARE_COMMIT_MAX_ATTEMPTS {
                        thread::sleep(Duration::from_millis(150));
                    }
                }
            }
        }
        if commit_ack.is_none() {
            eprintln!(
                "[usb-firmware-ota] commit ACK unavailable; validating after reconnect: {}",
                last_commit_delivery_error
            );
        }
        on_progress(total_bytes, total_bytes, "reboot");

        let verified = self.wait_for_firmware_validation(
            &expected_board_device_id,
            &descriptor.version,
            &target_partition,
            &original_version,
            &original_partition,
            baseline_boot_count,
            &mut reconnect,
            &on_progress,
            total_bytes,
        )?;

        Ok(FirmwareUpdateResult {
            transfer_id,
            bytes: total_bytes,
            sha256,
            target_partition: verified.partition,
            version: descriptor.version,
            project_name: descriptor.project_name,
            image_state: verified.image_state,
            pending_reboot: false,
        })
    }

    fn connected_board_status(
        &self,
        expected_board_device_id: &str,
    ) -> Result<UsbConnectionStatus, String> {
        let expected_board_device_id = validate_expected_board_device_id(expected_board_device_id)?;
        let conn = self.connection.lock().map_err(|error| error.to_string())?;
        let conn = conn.as_ref().ok_or("USB is not connected")?;
        if !conn.connected {
            return Err("USB connection is not active".to_string());
        }
        if conn.board_device_id != expected_board_device_id {
            return Err(format!(
                "connected USB board changed: expected {expected_board_device_id}, got {}",
                if conn.board_device_id.is_empty() {
                    "<missing>"
                } else {
                    &conn.board_device_id
                }
            ));
        }
        Ok(UsbConnectionStatus {
            connected: true,
            port_name: conn.port_name.clone(),
            baud_rate: conn.baud_rate,
            board_device_id: conn.board_device_id.clone(),
            transport: "usb".to_string(),
            runtime: conn.runtime.clone(),
            device_model: conn.device_model.clone(),
            firmware: conn.firmware.clone(),
            build_id: conn.build_id.clone(),
            git_sha: conn.git_sha.clone(),
            build_dirty: conn.build_dirty,
            protocol_schema: conn.protocol_schema,
            wire_protocol: conn.wire_protocol.clone(),
            capabilities: conn.capabilities.clone(),
        })
    }

    #[allow(clippy::too_many_arguments)]
    fn wait_for_firmware_validation<F, R>(
        &self,
        expected_board_device_id: &str,
        expected_version: &str,
        expected_partition: &str,
        original_version: &str,
        original_partition: &str,
        baseline_boot_count: u64,
        reconnect: &mut R,
        on_progress: &F,
        total_bytes: u64,
    ) -> Result<VerifiedFirmware, String>
    where
        F: Fn(u64, u64, &str),
        R: FnMut() -> Result<(), String>,
    {
        thread::sleep(Duration::from_secs(1));
        let started = Instant::now();
        let mut force_reconnect = true;
        let mut last_observation = "waiting for the device to reboot".to_string();

        while started.elapsed() < P4_FIRMWARE_RECONNECT_TIMEOUT {
            let status = self.status();
            if force_reconnect
                || !status.connected
                || status.board_device_id != expected_board_device_id
            {
                force_reconnect = false;
                if let Err(error) = reconnect() {
                    last_observation = error;
                    thread::sleep(Duration::from_millis(400));
                    continue;
                }
            }

            let status = self.status();
            if !status.connected || status.board_device_id != expected_board_device_id {
                last_observation =
                    format!("reconnected board identity did not match {expected_board_device_id}");
                thread::sleep(Duration::from_millis(400));
                continue;
            }

            on_progress(total_bytes, total_bytes, "validate");
            match self.query_diagnostics(expected_board_device_id) {
                Ok(diagnostics) => match evaluate_firmware_validation(
                    &diagnostics,
                    expected_version,
                    expected_partition,
                    original_version,
                    original_partition,
                    baseline_boot_count,
                ) {
                    Ok(Some(verified)) => return Ok(verified),
                    Ok(None) => {
                        last_observation = "new firmware is still pending validation".to_string();
                    }
                    Err(error) => return Err(error),
                },
                Err(error) => {
                    last_observation = error;
                }
            }
            thread::sleep(Duration::from_millis(500));
        }

        Err(format!(
            "timed out waiting for board {expected_board_device_id} to reconnect with firmware {expected_version} in imageState=valid: {last_observation}"
        ))
    }

    fn best_effort_firmware_abort(&self, expected_board_device_id: &str, transfer_id: &str) {
        let _ = self.send_to_board(
            expected_board_device_id,
            "firmware/abort",
            &serde_json::json!({ "transferId": transfer_id }),
        );
    }

    fn begin_firmware_transfer(
        &self,
        expected_board_device_id: &str,
        begin_payload: &serde_json::Value,
        transfer_id: &str,
    ) -> Result<serde_json::Value, String> {
        let mut last_error = "firmware begin was not attempted".to_string();
        for attempt in 1..=P4_FIRMWARE_BEGIN_MAX_ATTEMPTS {
            match self.send_firmware_command_and_wait(
                expected_board_device_id,
                "firmware/begin",
                begin_payload,
                transfer_id,
                "begin",
                0,
                0,
                P4_FIRMWARE_ACK_TIMEOUT,
            ) {
                Ok(ack) => return Ok(ack),
                Err(FirmwareCommandError::Rejected(error)) => {
                    self.best_effort_firmware_abort(expected_board_device_id, transfer_id);
                    return Err(error);
                }
                Err(error) => {
                    last_error = error.to_string();
                    eprintln!(
                        "[usb-firmware-ota] begin delivery attempt={}/{} error={}",
                        attempt, P4_FIRMWARE_BEGIN_MAX_ATTEMPTS, last_error
                    );
                    self.best_effort_firmware_abort(expected_board_device_id, transfer_id);
                    if attempt < P4_FIRMWARE_BEGIN_MAX_ATTEMPTS {
                        thread::sleep(Duration::from_millis(150));
                    }
                }
            }
        }
        Err(format!(
            "firmware begin failed after {P4_FIRMWARE_BEGIN_MAX_ATTEMPTS} attempts: {last_error}"
        ))
    }

    #[allow(clippy::too_many_arguments)]
    fn send_firmware_command_and_wait(
        &self,
        expected_board_device_id: &str,
        topic: &str,
        payload: &serde_json::Value,
        transfer_id: &str,
        phase: &str,
        expected_next_sequence: u64,
        expected_received_bytes: u64,
        timeout: Duration,
    ) -> Result<serde_json::Value, FirmwareCommandError> {
        let receiver = self
            .register_firmware_ack_waiter(
                transfer_id,
                phase,
                expected_next_sequence,
                expected_received_bytes,
            )
            .map_err(FirmwareCommandError::Send)?;
        if let Err(error) = self.send_firmware_to_board(expected_board_device_id, topic, payload) {
            self.remove_firmware_ack_waiter(
                transfer_id,
                phase,
                expected_next_sequence,
                expected_received_bytes,
            );
            return Err(FirmwareCommandError::Send(error));
        }
        let ack = receiver.recv_timeout(timeout).map_err(|_| {
            self.remove_firmware_ack_waiter(
                transfer_id,
                phase,
                expected_next_sequence,
                expected_received_bytes,
            );
            FirmwareCommandError::Timeout {
                phase: phase.to_string(),
            }
        })?;
        if ack.get("ok").and_then(|value| value.as_bool()) != Some(true) {
            return Err(FirmwareCommandError::Rejected(
                ack.get("error")
                    .or_else(|| ack.get("message"))
                    .and_then(|value| value.as_str())
                    .unwrap_or("device rejected firmware OTA command")
                    .to_string(),
            ));
        }
        Ok(ack)
    }

    fn register_firmware_ack_waiter(
        &self,
        transfer_id: &str,
        phase: &str,
        expected_next_sequence: u64,
        expected_received_bytes: u64,
    ) -> Result<mpsc::Receiver<serde_json::Value>, String> {
        let (sender, receiver) = mpsc::channel();
        let mut waiters = self
            .firmware_ack_waiters
            .lock()
            .map_err(|error| error.to_string())?;
        waiters.push(FirmwareAckWaiter {
            transfer_id: transfer_id.to_string(),
            phase: phase.to_string(),
            expected_next_sequence,
            expected_received_bytes,
            sender,
        });
        Ok(receiver)
    }

    fn remove_firmware_ack_waiter(
        &self,
        transfer_id: &str,
        phase: &str,
        expected_next_sequence: u64,
        expected_received_bytes: u64,
    ) {
        if let Ok(mut waiters) = self.firmware_ack_waiters.lock() {
            waiters.retain(|waiter| {
                waiter.transfer_id != transfer_id
                    || waiter.phase != phase
                    || waiter.expected_next_sequence != expected_next_sequence
                    || waiter.expected_received_bytes != expected_received_bytes
            });
        }
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

    /// Send asset_begin with ack - the unchecked `send_asset_begin` was
    /// removed 2026-06-01; the checked variant is the sole entry point.
    fn send_asset_begin_checked(&self, transfer_id: &str, total_bytes: u64) -> Result<(), String> {
        self.send_asset_begin_checked_with_raw_bytes(transfer_id, total_bytes, None)
    }

    fn send_asset_begin_checked_with_raw_bytes(
        &self,
        transfer_id: &str,
        total_bytes: u64,
        raw_bytes: Option<u64>,
    ) -> Result<(), String> {
        self.ensure_appearance_sync_not_cancelled(None)?;
        let mut payload = serde_json::json!({
            "transferId": transfer_id,
            "totalBytes": total_bytes,
        });
        if let Some(raw_bytes) = raw_bytes {
            payload["rawBytes"] = serde_json::Value::from(raw_bytes);
        }
        self.send_asset_command_and_wait_timeout(
            "asset/begin",
            &payload,
            transfer_id,
            "begin",
            None,
            None,
            ASSET_BEGIN_ACK_TIMEOUT,
        )?;
        Ok(())
    }

    fn best_effort_asset_abort(&self, transfer_id: &str) {
        transfer_log::record(
            "appearance",
            "abort_requested",
            serde_json::json!({ "transferId": transfer_id }),
        );
        let _ = self.send(
            "asset/abort",
            &serde_json::json!({ "transferId": transfer_id }),
        );
        // Older P4 firmware predates asset/abort. Its zero-file patch commit
        // safely releases asset_transfer_active without switching the active
        // slot; never use this compatibility release on Linux boards because
        // they could apply a partially staged audio patch.
        if self.status().runtime.eq_ignore_ascii_case("esp-p4") {
            let _ = self.send(
                "asset/patch-commit",
                &serde_json::json!({
                    "transferId": transfer_id,
                    "fileCount": 0,
                    "totalBytes": 0,
                }),
            );
        }
    }

    fn run_serial_asset_transaction<T, F>(
        &self,
        transfer_id: &str,
        operation: F,
    ) -> Result<T, String>
    where
        F: FnOnce() -> Result<T, String>,
    {
        let result = operation();
        if let Err(error) = &result {
            transfer_log::record(
                "appearance",
                "transaction_failed",
                serde_json::json!({
                    "transferId": transfer_id,
                    "error": error,
                }),
            );
            self.best_effort_asset_abort(transfer_id);
        }
        result
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

    fn send_asset_chunk_checked(
        &self,
        transfer_id: &str,
        path: &str,
        data_base64: &str,
        decoded_size: usize,
        index: u32,
    ) -> Result<(), String> {
        let payload = serde_json::json!({
            "transferId": transfer_id,
            "path": path,
            "data": data_base64,
            "size": decoded_size,
            "index": index.to_string()
        });
        self.send_asset_command_and_wait(
            "asset/chunk",
            &payload,
            transfer_id,
            "chunk",
            Some(path),
            Some(index),
        )?;
        Ok(())
    }

    fn send_asset_raw_chunk_once(
        &self,
        transfer_id: &str,
        path: &str,
        chunk: &[u8],
        index: u32,
    ) -> Result<(), String> {
        let phase = "raw-chunk";
        let ready_phase = "raw-ready";
        let wait_for_ready = self.supports_p4_raw_ready_ack();
        let receiver =
            self.register_asset_ack_waiter(transfer_id, phase, Some(path), Some(index))?;
        let ready_receiver = if wait_for_ready {
            Some(self.register_asset_ack_waiter(
                transfer_id,
                ready_phase,
                Some(path),
                Some(index),
            )?)
        } else {
            None
        };
        let chunk_checksum = asset_checksum_hex(chunk);
        let header = serde_json::json!({
            "topic": "asset/raw-chunk",
            "payload": {
                "transferId": transfer_id,
                "path": path,
                "size": chunk.len(),
                "checksum": chunk_checksum.as_str(),
                "index": index.to_string(),
            },
        });
        let mut line = serde_json::to_vec(&header).map_err(|error| error.to_string())?;
        line.push(b'\n');
        let send_started = Instant::now();
        let send_result = (|| {
            let mut connection = self.connection.lock().map_err(|error| error.to_string())?;
            let connection = connection.as_mut().ok_or("Not connected")?;
            if !connection.connected {
                return Err("Connection lost".to_string());
            }
            if let Err(error) = connection.writer.write_all(&line) {
                connection.connected = false;
                return Err(format!("Raw asset header write failed: {error}"));
            }
            if let Err(error) = connection.writer.flush() {
                connection.connected = false;
                return Err(format!("Raw asset header flush failed: {error}"));
            }
            if let Some(ready_receiver) = ready_receiver.as_ref() {
                let ready = ready_receiver
                    .recv_timeout(P4_RAW_ASSET_ACK_TIMEOUT)
                    .map_err(|error| match error {
                        mpsc::RecvTimeoutError::Timeout => format!(
                            "timed out waiting for raw P4 asset chunk readiness: transferId={transfer_id} path={path} index={index}"
                        ),
                        mpsc::RecvTimeoutError::Disconnected => {
                            "P4 raw asset readiness channel closed".to_string()
                        }
                    })?;
                transfer_log::record(
                    "appearance",
                    "raw_ready_ack",
                    serde_json::json!({
                        "transferId": transfer_id,
                        "path": path,
                        "index": index,
                        "size": chunk.len(),
                        "checksum": chunk_checksum.as_str(),
                        "response": &ready,
                    }),
                );
                if ready.get("ok").and_then(|value| value.as_bool()) != Some(true) {
                    return Err(ready
                        .get("error")
                        .or_else(|| ready.get("message"))
                        .and_then(|value| value.as_str())
                        .unwrap_or("device rejected raw P4 asset chunk header")
                        .to_string());
                }
            }
            if let Err(error) = connection.writer.write_all(chunk) {
                connection.connected = false;
                return Err(format!("Raw asset data write failed: {error}"));
            }
            if let Err(error) = connection.writer.flush() {
                connection.connected = false;
                return Err(format!("Raw asset flush failed: {error}"));
            }
            Ok(())
        })();
        if let Err(error) = send_result {
            transfer_log::record(
                "appearance",
                "raw_chunk_write_failed",
                serde_json::json!({
                    "transferId": transfer_id,
                    "path": path,
                    "index": index,
                    "size": chunk.len(),
                    "checksum": chunk_checksum.as_str(),
                    "error": error.as_str(),
                }),
            );
            self.remove_asset_ack_waiter(transfer_id, phase, Some(path), Some(index));
            if wait_for_ready {
                self.remove_asset_ack_waiter(transfer_id, ready_phase, Some(path), Some(index));
            }
            return Err(error);
        }

        let wait_started = Instant::now();
        let ack = loop {
            if let Err(error) = self.ensure_appearance_sync_not_cancelled(Some(transfer_id)) {
                self.remove_asset_ack_waiter(transfer_id, phase, Some(path), Some(index));
                return Err(error);
            }
            let Some(remaining) = P4_RAW_ASSET_ACK_TIMEOUT.checked_sub(wait_started.elapsed())
            else {
                self.remove_asset_ack_waiter(transfer_id, phase, Some(path), Some(index));
                return Err(format!(
                    "timed out waiting for raw P4 asset chunk: transferId={transfer_id} path={path} index={index}"
                ));
            };
            match receiver.recv_timeout(remaining.min(Duration::from_millis(100))) {
                Ok(ack) => break ack,
                Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    self.remove_asset_ack_waiter(transfer_id, phase, Some(path), Some(index));
                    return Err("P4 raw asset acknowledgement channel closed".to_string());
                }
            }
        };
        transfer_log::record(
            "appearance",
            "raw_chunk_ack",
            serde_json::json!({
                "transferId": transfer_id,
                "path": path,
                "index": index,
                "size": chunk.len(),
                "checksum": chunk_checksum.as_str(),
                "elapsedMs": send_started.elapsed().as_millis(),
                "response": &ack,
            }),
        );
        if ack.get("ok").and_then(|value| value.as_bool()) != Some(true) {
            return Err(ack
                .get("error")
                .or_else(|| ack.get("message"))
                .and_then(|value| value.as_str())
                .unwrap_or("device rejected raw P4 asset chunk")
                .to_string());
        }
        let elapsed = send_started.elapsed();
        if elapsed >= Duration::from_millis(300) {
            eprintln!(
                "[usb-p4-ota] slow raw chunk elapsed_ms={} bytes={} path={} index={}",
                elapsed.as_millis(),
                chunk.len(),
                path,
                index
            );
        }
        Ok(())
    }

    fn recover_raw_asset_stream(&self, pending_size: usize) -> Result<(), String> {
        {
            let mut connection_guard = self.connection.lock().map_err(|error| error.to_string())?;
            let connection = connection_guard.as_mut().ok_or("Not connected")?;
            if !connection.connected {
                return Err("Connection lost".to_string());
            }
            let padding = vec![0u8; pending_size];
            if let Err(error) = connection.writer.write_all(&padding) {
                connection.connected = false;
                return Err(format!("Raw asset recovery write failed: {error}"));
            }
            if let Err(error) = connection.writer.write_all(b"\n") {
                connection.connected = false;
                return Err(format!("Raw asset recovery delimiter failed: {error}"));
            }
            if let Err(error) = connection.writer.flush() {
                connection.connected = false;
                return Err(format!("Raw asset recovery flush failed: {error}"));
            }
        }
        thread::sleep(P4_RAW_APPEARANCE_RECOVERY_DELAY);
        Ok(())
    }

    fn send_asset_raw_chunk_checked(
        &self,
        transfer_id: &str,
        path: &str,
        chunk: &[u8],
        index: u32,
    ) -> Result<(), String> {
        let mut last_error = String::new();
        for attempt in 1..=P4_RAW_APPEARANCE_CHUNK_MAX_ATTEMPTS {
            transfer_log::record(
                "appearance",
                "raw_chunk_attempt",
                serde_json::json!({
                    "transferId": transfer_id,
                    "path": path,
                    "index": index,
                    "size": chunk.len(),
                    "checksum": asset_checksum_hex(chunk),
                    "attempt": attempt,
                    "maxAttempts": P4_RAW_APPEARANCE_CHUNK_MAX_ATTEMPTS,
                    "baud": self.status().baud_rate,
                }),
            );
            match self.send_asset_raw_chunk_once(transfer_id, path, chunk, index) {
                Ok(()) => return Ok(()),
                Err(error) => {
                    transfer_log::record(
                        "appearance",
                        "raw_chunk_attempt_failed",
                        serde_json::json!({
                            "transferId": transfer_id,
                            "path": path,
                            "index": index,
                            "size": chunk.len(),
                            "checksum": asset_checksum_hex(chunk),
                            "attempt": attempt,
                            "error": error.as_str(),
                        }),
                    );
                    let timed_out = error.contains("timed out waiting for raw P4 asset chunk");
                    let checksum_failed = error.contains("raw chunk checksum mismatch");
                    if attempt == P4_RAW_APPEARANCE_CHUNK_MAX_ATTEMPTS
                        || (!timed_out && !checksum_failed)
                    {
                        return Err(error);
                    }
                    eprintln!(
                        "[usb-p4-ota] retry raw chunk path={} index={} attempt={} error={}",
                        path, index, attempt, error
                    );
                    if timed_out {
                        self.recover_raw_asset_stream(chunk.len())?;
                    }
                    last_error = error;
                }
            }
        }
        Err(last_error)
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
        self.send_asset_command_and_wait(
            "asset/file",
            &payload,
            transfer_id,
            "file",
            Some(path),
            None,
        )?;
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
        self.send_asset_command_and_wait(
            "asset/commit",
            &payload,
            transfer_id,
            "commit",
            None,
            None,
        )?;
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
            None,
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

    fn supports_p4_appearance_slot_reuse(&self) -> bool {
        let capabilities = self.status().capabilities;
        capabilities
            .get("appearanceSlotReuse")
            .and_then(|value| value.as_bool())
            .unwrap_or(false)
            || capabilities
                .get("appearance")
                .and_then(|value| value.get("slotReuse"))
                .and_then(|value| value.as_bool())
                .unwrap_or(false)
    }

    fn supports_p4_raw_asset_chunks(&self) -> bool {
        let capabilities = self.status().capabilities;
        capabilities
            .get("transport")
            .and_then(|value| value.get("rawAssetChunks"))
            .and_then(|value| value.as_bool())
            .unwrap_or(false)
            && capabilities
                .get("transport")
                .and_then(|value| value.get("rawAssetChunkBytes"))
                .and_then(|value| value.as_u64())
                .is_some_and(|size| size >= P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE as u64)
    }

    fn supports_p4_raw_ready_ack(&self) -> bool {
        self.status()
            .capabilities
            .get("transport")
            .and_then(|value| value.get("rawAssetReadyAck"))
            .and_then(|value| value.as_bool())
            .unwrap_or(false)
    }

    fn supports_p4_raw_appearance_slot(&self) -> bool {
        let capabilities = self.status().capabilities;
        capabilities
            .get("rawAppearanceSlot1")
            .and_then(|value| value.as_bool())
            .unwrap_or(false)
            || capabilities
                .get("appearance")
                .and_then(|value| value.get("rawSlot1"))
                .and_then(|value| value.as_bool())
                .unwrap_or(false)
    }

    fn query_p4_appearance_slots(
        &self,
        transfer_id: &str,
    ) -> Result<P4AppearanceSlotState, String> {
        let payload = serde_json::json!({ "transferId": transfer_id });
        let ack = self.send_asset_command_and_wait_timeout(
            "asset/slot-query",
            &payload,
            transfer_id,
            "slot-query",
            None,
            None,
            ASSET_STAT_TIMEOUT,
        )?;
        parse_p4_appearance_slot_state(&ack)
    }

    fn activate_p4_appearance_slot(
        &self,
        transfer_id: &str,
        slot: u32,
        pack_id: &str,
    ) -> Result<(), String> {
        let payload = serde_json::json!({
            "transferId": transfer_id,
            "slot": slot,
            "packId": pack_id,
        });
        self.send_asset_command_and_wait_timeout(
            "asset/activate",
            &payload,
            transfer_id,
            "activate",
            None,
            None,
            ASSET_STAT_TIMEOUT,
        )?;
        Ok(())
    }

    fn prepare_p4_raw_transfer_slot(&self, transfer_id: &str) -> Result<bool, String> {
        let state = self.query_p4_appearance_slots(transfer_id)?;
        let Some(fallback) = p4_raw_transfer_fallback_slot(&state) else {
            return Ok(false);
        };
        self.activate_p4_appearance_slot(transfer_id, fallback.slot, &fallback.pack_id)?;
        Ok(true)
    }

    fn try_activate_cached_p4_pack(
        &self,
        transfer_id: &str,
        pack_id: &str,
    ) -> Result<P4CachedPackActivation, String> {
        let state = self.query_p4_appearance_slots(transfer_id)?;
        let Some(cached) = state.slots.iter().find(|slot| slot.pack_id == pack_id) else {
            return Ok(P4CachedPackActivation::NotFound);
        };
        if state.active_slot == Some(cached.slot) {
            return Ok(P4CachedPackActivation::AlreadyActive);
        }
        self.activate_p4_appearance_slot(transfer_id, cached.slot, pack_id)?;
        Ok(P4CachedPackActivation::Activated)
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
        index: Option<u32>,
    ) -> Result<serde_json::Value, String> {
        self.send_asset_command_and_wait_timeout(
            topic,
            payload,
            transfer_id,
            phase,
            path,
            index,
            ASSET_ACK_TIMEOUT,
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn send_asset_command_and_wait_timeout(
        &self,
        topic: &str,
        payload: &serde_json::Value,
        transfer_id: &str,
        phase: &str,
        path: Option<&str>,
        index: Option<u32>,
        timeout: Duration,
    ) -> Result<serde_json::Value, String> {
        let receiver = self.register_asset_ack_waiter(transfer_id, phase, path, index)?;
        let command_started = Instant::now();
        if let Err(error) = self.send(topic, payload) {
            self.remove_asset_ack_waiter(transfer_id, phase, path, index);
            return Err(error);
        }
        let send_elapsed = command_started.elapsed();
        let started = Instant::now();
        let ack = loop {
            if let Err(error) = self.ensure_appearance_sync_not_cancelled(Some(transfer_id)) {
                self.remove_asset_ack_waiter(transfer_id, phase, path, index);
                return Err(error);
            }
            let Some(remaining) = timeout.checked_sub(started.elapsed()) else {
                self.remove_asset_ack_waiter(transfer_id, phase, path, index);
                return Err(format!(
                    "未收到板端素材 OTA 确认: transferId={} phase={}{}",
                    transfer_id,
                    phase,
                    path.map(|value| format!(" path={value}"))
                        .unwrap_or_default()
                ));
            };
            match receiver.recv_timeout(remaining.min(Duration::from_millis(100))) {
                Ok(ack) => break ack,
                Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    self.remove_asset_ack_waiter(transfer_id, phase, path, index);
                    return Err("板端素材 OTA 确认通道已断开".to_string());
                }
            }
        };
        let ack_elapsed = started.elapsed();
        if phase == "chunk"
            && (send_elapsed >= Duration::from_millis(250)
                || ack_elapsed >= Duration::from_millis(250))
        {
            eprintln!(
                "[usb-p4-ota] slow chunk send_ms={} ack_ms={} path={}",
                send_elapsed.as_millis(),
                ack_elapsed.as_millis(),
                path.unwrap_or_default()
            );
        }
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
        index: Option<u32>,
    ) -> Result<mpsc::Receiver<serde_json::Value>, String> {
        let (sender, receiver) = mpsc::channel();
        let mut waiters = self.asset_ack_waiters.lock().map_err(|e| e.to_string())?;
        waiters.push(AssetAckWaiter {
            transfer_id: transfer_id.to_string(),
            phase: phase.to_string(),
            path: path.map(str::to_string),
            index,
            sender,
        });
        Ok(receiver)
    }

    fn remove_asset_ack_waiter(
        &self,
        transfer_id: &str,
        phase: &str,
        path: Option<&str>,
        index: Option<u32>,
    ) {
        if let Ok(mut waiters) = self.asset_ack_waiters.lock() {
            waiters.retain(|waiter| {
                waiter.transfer_id != transfer_id
                    || waiter.phase != phase
                    || waiter.path.as_deref() != path
                    || waiter.index != index
            });
        }
    }

    fn register_widget_ack_waiter(
        &self,
        transfer_id: &str,
        phase: &str,
        path: Option<&str>,
        index: Option<u32>,
    ) -> Result<mpsc::Receiver<serde_json::Value>, String> {
        let (sender, receiver) = mpsc::channel();
        let mut waiters = self.widget_ack_waiters.lock().map_err(|e| e.to_string())?;
        waiters.push(WidgetAckWaiter {
            transfer_id: transfer_id.to_string(),
            phase: phase.to_string(),
            path: path.map(str::to_string),
            index,
            sender,
        });
        Ok(receiver)
    }

    fn remove_widget_ack_waiter(
        &self,
        transfer_id: &str,
        phase: &str,
        path: Option<&str>,
        index: Option<u32>,
    ) {
        if let Ok(mut waiters) = self.widget_ack_waiters.lock() {
            waiters.retain(|waiter| {
                waiter.transfer_id != transfer_id
                    || waiter.phase != phase
                    || waiter.path.as_deref() != path
                    || waiter.index != index
            });
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
    /// Mirrors send_asset_chunk wire format (path/data/index) - server-side
    /// uses the same b64 staging + decode helpers.
    pub fn send_widget_install_chunk(
        &self,
        transfer_id: &str,
        relative_path: &str,
        data_base64: &str,
        index: u32,
        decoded_size: usize,
        checksum: &str,
    ) -> Result<(), String> {
        let payload = build_widget_chunk_payload(
            transfer_id,
            relative_path,
            data_base64,
            index,
            decoded_size,
            checksum,
        );
        // Each widget file is ACK-gated. Flush it now so the board can reply
        // before the per-file timeout instead of waiting for a later write.
        self.send("widget/chunk", &payload)
    }

    /// Widget OTA: commit - server unpacks staging into widgets/<id>/ and
    /// writes .active-widget so board-widget-runtime reloads.
    pub fn send_widget_install_commit(
        &self,
        transfer_id: &str,
        widget_id: &str,
    ) -> Result<(), String> {
        let payload = serde_json::json!({"transferId": transfer_id, "widgetId": widget_id});
        self.send("widget/commit", &payload)
    }

    /// Remove an installed widget and wait until the board confirms both
    /// package cleanup and the return to its main screen.
    pub fn remove_widget(
        &self,
        expected_board_device_id: &str,
        widget_id: &str,
    ) -> Result<(), String> {
        let _transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        let status = self.connected_board_status(expected_board_device_id)?;
        ensure_widget_delete_supported(&status)?;
        let transfer_id = format!("widget-delete-{}", uuid::Uuid::new_v4());
        let receiver = self.register_widget_ack_waiter(&transfer_id, "delete", None, None)?;
        let payload = serde_json::json!({
            "transferId": &transfer_id,
            "widgetId": widget_id,
        });
        if let Err(error) = self.send_to_board(expected_board_device_id, "widget/delete", &payload)
        {
            self.remove_widget_ack_waiter(&transfer_id, "delete", None, None);
            return Err(error);
        }
        match receiver.recv_timeout(WIDGET_DELETE_ACK_TIMEOUT) {
            Ok(ack) if ack.get("ok").and_then(|value| value.as_bool()) == Some(true) => Ok(()),
            Ok(ack) => Err(ack
                .get("msg")
                .or_else(|| ack.get("error"))
                .and_then(|value| value.as_str())
                .unwrap_or("板端拒绝删除组件")
                .to_string()),
            Err(_) => {
                self.remove_widget_ack_waiter(&transfer_id, "delete", None, None);
                Err(format_widget_ack_timeout(&transfer_id, "delete"))
            }
        }
    }

    /// Push a local .clawpkg directory to the device via USB widget OTA.
    /// Walks all regular files under `widget_dir`, sends each as a base64
    /// chunk, commits. Optional binding_overrides applied to buttons.json
    /// before sending (action -> new_control mapping).
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
        let _transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        // Stay below the P4 runtime's 64-byte transaction buffer even when
        // the manifest uses the maximum 47-byte component id.
        let transfer_id = format!("widget-install-{}", uuid::Uuid::new_v4());

        // 1) walk widget_dir to collect (rel_path, bytes) - small files (each a
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
                    if widget_ota_should_skip_path(&rel) {
                        continue;
                    }
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
                        *bytes = serde_json::to_vec(&arr).map_err(|e| e.to_string())?;
                    }
                    break;
                }
            }
        }
        // Normalize the two JSON files consumed by the bounded P4 mini-app
        // runtime. Whitespace must not spend the device's fixed receive buffer.
        for (rel, bytes) in entries.iter_mut() {
            *bytes = prepare_p4_widget_file(widget_id, rel, bytes)?;
        }

        // 3) begin - register ack waiter BEFORE sending so we don't miss
        //    a very-fast reply on a healthy board (~50ms).
        let begin_rx = self.register_widget_ack_waiter(&transfer_id, "begin", None, None)?;
        self.send_widget_install_begin(&transfer_id, widget_id)?;
        match begin_rx.recv_timeout(WIDGET_BEGIN_ACK_TIMEOUT) {
            Ok(ack) => {
                if ack.get("ok").and_then(|v| v.as_bool()) != Some(true) {
                    self.remove_widget_ack_waiter(&transfer_id, "begin", None, None);
                    let msg = ack
                        .get("msg")
                        .and_then(|v| v.as_str())
                        .unwrap_or("板端拒绝组件传输开始");
                    return Err(format!("widget begin rejected: {}", msg));
                }
            }
            Err(_) => {
                self.remove_widget_ack_waiter(&transfer_id, "begin", None, None);
                return Err(format_widget_ack_timeout(&transfer_id, "begin"));
            }
        }

        // 4) chunks. Skip zero-byte files (.keep markers). P4 acknowledges each
        // file with decoded size + checksum validation, so retry transient UART
        // corruption before the transaction reaches commit.
        let b64 = base64::engine::general_purpose::STANDARD;
        let total = entries.len() as u32;
        let mut sent_bytes: u64 = 0;
        for (i, (rel, bytes)) in entries.iter().enumerate() {
            if bytes.is_empty() {
                continue;
            }
            let encoded = b64.encode(bytes);
            let checksum = asset_checksum_hex(bytes);
            let mut accepted = false;
            let mut last_error = String::new();
            for attempt in 1..=WIDGET_CHUNK_MAX_ATTEMPTS {
                let chunk_rx =
                    self.register_widget_ack_waiter(&transfer_id, "chunk", Some(rel), Some(0))?;
                if let Err(error) = self.send_widget_install_chunk(
                    &transfer_id,
                    rel,
                    &encoded,
                    0,
                    bytes.len(),
                    &checksum,
                ) {
                    self.remove_widget_ack_waiter(&transfer_id, "chunk", Some(rel), Some(0));
                    return Err(error);
                }
                match chunk_rx.recv_timeout(WIDGET_CHUNK_ACK_TIMEOUT) {
                    Ok(ack) if ack.get("ok").and_then(|value| value.as_bool()) == Some(true) => {
                        accepted = true;
                        break;
                    }
                    Ok(ack) => {
                        last_error = ack
                            .get("msg")
                            .and_then(|value| value.as_str())
                            .unwrap_or("widget chunk rejected")
                            .to_string();
                    }
                    Err(_) => {
                        self.remove_widget_ack_waiter(&transfer_id, "chunk", Some(rel), Some(0));
                        last_error = "chunk acknowledgement timed out".to_string();
                    }
                }
                eprintln!(
                    "[widget-ota] retry path={} attempt={} error={}",
                    rel, attempt, last_error
                );
            }
            if !accepted {
                return Err(format!(
                    "widget chunk failed after {} attempts: path={} error={}",
                    WIDGET_CHUNK_MAX_ATTEMPTS, rel, last_error
                ));
            }
            sent_bytes += encoded.len() as u64;
            on_progress(i as u32 + 1, total, sent_bytes);
        }

        // 5) commit - wait for board to finish decode+rename+activate so the
        // caller's "success" toast lines up with the device actually showing
        // the new widget.
        let commit_rx = self.register_widget_ack_waiter(&transfer_id, "commit", None, None)?;
        self.send_widget_install_commit(&transfer_id, widget_id)?;
        match commit_rx.recv_timeout(WIDGET_COMMIT_ACK_TIMEOUT) {
            Ok(ack) => {
                if ack.get("ok").and_then(|v| v.as_bool()) != Some(true) {
                    let msg = ack
                        .get("msg")
                        .and_then(|v| v.as_str())
                        .unwrap_or("板端组件提交失败")
                        .to_string();
                    return Err(msg);
                }
            }
            Err(_) => {
                self.remove_widget_ack_waiter(&transfer_id, "commit", None, None);
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
        self.ensure_appearance_sync_not_cancelled(None)?;
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
        self.send_asset_begin_checked(&transfer_id, total_bytes)?;

        let mut file_count: u32 = 0;
        let mut byte_count: u64 = 0;
        on_progress(0, total_files, 0, total_bytes);

        for asset in assets {
            self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
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
                self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
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

        self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
        self.send_asset_commit_checked(&transfer_id, file_count, byte_count)?;
        eprintln!(
            "[usb-appearance-ota] commit transfer_id={} mode={:?} sent_files={} sent_bytes={}",
            transfer_id, mode, file_count, byte_count
        );

        Ok((file_count, byte_count))
    }

    pub fn sync_appearance_p4<F>(
        &self,
        appearance_dir: &std::path::Path,
        app_data_dir: &std::path::Path,
        expected_board_device_id: &str,
        on_progress: F,
    ) -> Result<(u32, u64, bool), String>
    where
        F: Fn(u32, u32, u64, u64),
    {
        let _asset_transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        self.connected_board_status(expected_board_device_id)?;
        self.ensure_appearance_sync_not_cancelled(None)?;
        if p4_native_usb_available() {
            match sync_appearance_p4_native(
                appearance_dir,
                app_data_dir,
                expected_board_device_id,
                &self.appearance_sync_cancel_requested,
                &on_progress,
            ) {
                Ok(result) => return Ok(result),
                Err(error) if error == APPEARANCE_SYNC_CANCELLED_ERROR => return Err(error),
                Err(error) => {
                    eprintln!(
                        "[usb-p4-native-ota] native USB bulk failed, falling back to serial: {}",
                        error
                    );
                }
            }
        }

        let _ = app_data_dir;
        let assets = load_prepared_p4_appearance_pack(appearance_dir)?;
        self.ensure_appearance_sync_not_cancelled(None)?;

        let total_files: u32 = assets.len() as u32;
        let total_bytes: u64 = assets
            .iter()
            .filter_map(|asset| std::fs::metadata(&asset.source_path).ok())
            .map(|meta| meta.len())
            .sum();
        let raw_bytes: u64 = assets
            .iter()
            .filter(|asset| {
                asset.device_path.ends_with(".h264") || asset.device_path.ends_with(".mjpg")
            })
            .filter_map(|asset| std::fs::metadata(&asset.source_path).ok())
            .map(|meta| meta.len())
            .sum();
        let pack_id = p4_pack_id_from_assets(&assets)?;
        if self.supports_p4_appearance_slot_reuse() {
            let cache_transfer_id = format!(
                "p4-slot-{}",
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_millis()
            );
            match self.try_activate_cached_p4_pack(&cache_transfer_id, &pack_id) {
                Ok(P4CachedPackActivation::Activated) => {
                    eprintln!(
                        "[usb-p4-ota] reactivated cached appearance pack_id={}",
                        pack_id
                    );
                    on_progress(0, total_files, 0, total_bytes);
                    return Ok((0, 0, true));
                }
                Ok(P4CachedPackActivation::AlreadyActive) => {
                    eprintln!(
                        "[usb-p4-ota] skip transfer: active slot already has pack_id={}",
                        pack_id
                    );
                    on_progress(0, total_files, 0, total_bytes);
                    return Ok((0, 0, false));
                }
                Ok(P4CachedPackActivation::NotFound) => {}
                Err(error) => {
                    eprintln!(
                        "[usb-p4-ota] cached slot query failed; falling back to current sync planner: {}",
                        error
                    );
                }
            }
        }
        let digests = digest_appearance_assets(&assets)?;
        match self.plan_incremental_appearance_sync(&digests, &[]) {
            Ok(AppearanceSyncPlan::Skip) => {
                eprintln!("[usb-p4-ota] skip transfer: board p4 assets already match");
                on_progress(0, total_files, 0, total_bytes);
                return Ok((0, 0, false));
            }
            Ok(AppearanceSyncPlan::Full) => {}
            Ok(AppearanceSyncPlan::AudioPatch(_)) => {}
            Err(error) => {
                eprintln!(
                    "[usb-p4-ota] remote stat unavailable; falling back to full sync: {}",
                    error
                );
            }
        }

        let transfer_id = format!(
            "p4-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_millis()
        );
        let use_raw_chunks = self.supports_p4_raw_asset_chunks();
        let use_raw_slot = use_raw_chunks && self.supports_p4_raw_appearance_slot();
        if use_raw_slot {
            let capabilities = self.status().capabilities;
            let raw_capacity = capabilities
                .get("rawAppearanceCapacityBytes")
                .and_then(|value| value.as_u64())
                .or_else(|| {
                    capabilities
                        .get("appearance")
                        .and_then(|value| value.get("customCapacityBytes"))
                        .and_then(|value| value.as_u64())
                });
            if raw_capacity.is_some_and(|capacity| raw_bytes > capacity) {
                return Err(format!(
                    "P4 appearance videos require {raw_bytes} bytes but the custom slot only has {} bytes",
                    raw_capacity.unwrap_or_default()
                ));
            }
        }
        let negotiated_baud = self.status().baud_rate;
        if use_raw_slot && self.prepare_p4_raw_transfer_slot(&transfer_id)? {
            eprintln!(
                "[usb-p4-ota] activated slot0 before full sync so raw slot1 remains the transfer target"
            );
        }
        let chunk_size = if use_raw_chunks {
            P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE
        } else {
            P4_APPEARANCE_ASSET_CHUNK_SIZE
        };
        eprintln!(
            "[usb-p4-ota] begin transfer_id={} files={} bytes={} transport={} baud={} chunk_bytes={}",
            transfer_id,
            total_files,
            total_bytes,
            if use_raw_chunks {
                "uart-raw-v1"
            } else {
                "json-base64-v1"
            },
            negotiated_baud,
            chunk_size
        );
        transfer_log::record(
            "appearance",
            "transaction_started",
            serde_json::json!({
                "transferId": transfer_id,
                "boardDeviceId": expected_board_device_id,
                "files": total_files,
                "bytes": total_bytes,
                "rawBytes": raw_bytes,
                "transport": if use_raw_chunks { "uart-raw-v1" } else { "json-base64-v1" },
                "baud": negotiated_baud,
                "chunkBytes": chunk_size,
                "packId": pack_id.as_str(),
            }),
        );
        let transfer_started = Instant::now();
        self.run_serial_asset_transaction(&transfer_id, || {
            self.send_asset_begin_checked_with_raw_bytes(
                &transfer_id,
                total_bytes,
                use_raw_slot.then_some(raw_bytes),
            )?;

            let mut file_count: u32 = 0;
            let mut byte_count: u64 = 0;
            on_progress(0, total_files, 0, total_bytes);

            for asset in assets {
                self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
                eprintln!(
                    "[usb-p4-ota] file family={} kind={} path={}",
                    asset.family_name,
                    asset.kind,
                    asset.source_path.display()
                );
                let mut file = std::fs::File::open(&asset.source_path)
                    .map_err(|e| format!("打开 P4 资源失败 {}: {}", asset.family_name, e))?;
                let mut buf = Vec::new();
                file.read_to_end(&mut buf)
                    .map_err(|e| format!("读取 P4 资源失败 {}: {}", asset.family_name, e))?;

                let file_size = buf.len() as u64;
                let checksum = asset_checksum_hex(&buf);
                transfer_log::record(
                    "appearance",
                    "file_started",
                    serde_json::json!({
                        "transferId": transfer_id.as_str(),
                        "family": asset.family_name.as_str(),
                        "kind": asset.kind,
                        "sourcePath": &asset.source_path,
                        "devicePath": asset.device_path.as_str(),
                        "size": file_size,
                        "checksum": checksum.as_str(),
                    }),
                );
                let mut chunk_count = 0u64;
                for (i, chunk) in buf.chunks(chunk_size).enumerate() {
                    self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
                    if use_raw_chunks {
                        self.send_asset_raw_chunk_checked(
                            &transfer_id,
                            &asset.device_path,
                            chunk,
                            i as u32,
                        )?;
                    } else {
                        let b64 = base64::engine::general_purpose::STANDARD.encode(chunk);
                        let mut accepted = false;
                        let mut last_error = String::new();
                        for attempt in 1..=P4_APPEARANCE_CHUNK_DECODE_MAX_ATTEMPTS {
                            self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
                            match self.send_asset_chunk_checked(
                                &transfer_id,
                                &asset.device_path,
                                &b64,
                                chunk.len(),
                                i as u32,
                            ) {
                                Ok(()) => {
                                    accepted = true;
                                    break;
                                }
                                Err(error) if error.contains("base64 decode failed") => {
                                    last_error = error;
                                    eprintln!(
                                        "[usb-p4-ota] retry decoded chunk path={} index={} attempt={}",
                                        asset.device_path, i, attempt
                                    );
                                    thread::sleep(Duration::from_millis(10));
                                }
                                Err(error) => return Err(error),
                            }
                        }
                        if !accepted {
                            return Err(format!(
                                "P4 asset chunk failed after {} decode attempts: path={} index={} error={}",
                                P4_APPEARANCE_CHUNK_DECODE_MAX_ATTEMPTS,
                                asset.device_path,
                                i,
                                last_error
                            ));
                        }
                    }
                    chunk_count += 1;
                    let chunk_bytes_sent = std::cmp::min(((i + 1) * chunk_size) as u64, file_size);
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
                transfer_log::record(
                    "appearance",
                    "file_committed",
                    serde_json::json!({
                        "transferId": transfer_id.as_str(),
                        "devicePath": asset.device_path.as_str(),
                        "size": file_size,
                        "checksum": checksum.as_str(),
                        "chunks": chunk_count,
                    }),
                );

                file_count += 1;
                byte_count += file_size;
                on_progress(file_count, total_files, byte_count, total_bytes);
            }

            self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
            self.send_asset_commit_checked(&transfer_id, file_count, byte_count)?;
            let elapsed = transfer_started.elapsed();
            let effective_bytes_per_sec = if elapsed.is_zero() {
                0
            } else {
                (byte_count as u128 * 1_000 / elapsed.as_millis().max(1)) as u64
            };
            eprintln!(
                "[usb-p4-ota] commit transfer_id={} sent_files={} sent_bytes={} transport={} baud={} elapsed_ms={} effective_kib_s={:.1}",
                transfer_id,
                file_count,
                byte_count,
                if use_raw_chunks {
                    "uart-raw-v1"
                } else {
                    "json-base64-v1"
                },
                negotiated_baud,
                elapsed.as_millis(),
                effective_bytes_per_sec as f64 / 1024.0
            );
            transfer_log::record(
                "appearance",
                "transaction_committed",
                serde_json::json!({
                    "transferId": transfer_id.as_str(),
                    "files": file_count,
                    "bytes": byte_count,
                    "baud": negotiated_baud,
                    "elapsedMs": elapsed.as_millis(),
                    "effectiveBytesPerSecond": effective_bytes_per_sec,
                }),
            );

            Ok((file_count, byte_count, false))
        })
    }

    pub fn sync_appearance_p4_native_only<F>(
        &self,
        appearance_dir: &std::path::Path,
        app_data_dir: &std::path::Path,
        expected_board_device_id: &str,
        on_progress: F,
    ) -> Result<(u32, u64, bool), String>
    where
        F: Fn(u32, u32, u64, u64),
    {
        let _asset_transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        sync_appearance_p4_native(
            appearance_dir,
            app_data_dir,
            expected_board_device_id,
            self.appearance_sync_cancel_requested.as_ref(),
            on_progress,
        )
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
        self.send_asset_begin_checked(&transfer_id, total_bytes)?;

        let mut file_count: u32 = 0;
        let mut byte_count: u64 = 0;
        on_progress(0, total_files, 0, total_bytes);

        for asset in assets {
            self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
            let mut file = std::fs::File::open(&asset.source_path)
                .map_err(|e| format!("打开音效文件失败 {}: {}", asset.family_name, e))?;
            let mut buf = Vec::new();
            file.read_to_end(&mut buf)
                .map_err(|e| format!("读取音效失败 {}: {}", asset.family_name, e))?;

            let file_size = buf.len() as u64;
            let checksum = asset_checksum_hex(&buf);
            let mut chunk_count = 0u64;
            for (i, chunk) in buf.chunks(APPEARANCE_ASSET_CHUNK_SIZE).enumerate() {
                self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
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

        self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
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
                baud_rate: c.baud_rate,
                board_device_id: c.board_device_id.clone(),
                transport: "usb".to_string(),
                runtime: c.runtime.clone(),
                device_model: c.device_model.clone(),
                firmware: c.firmware.clone(),
                build_id: c.build_id.clone(),
                git_sha: c.git_sha.clone(),
                build_dirty: c.build_dirty,
                protocol_schema: c.protocol_schema,
                wire_protocol: c.wire_protocol.clone(),
                capabilities: c.capabilities.clone(),
            },
            None => UsbConnectionStatus {
                connected: false,
                port_name: String::new(),
                baud_rate: 0,
                board_device_id: String::new(),
                transport: "mqtt".to_string(),
                runtime: String::new(),
                device_model: String::new(),
                firmware: String::new(),
                build_id: String::new(),
                git_sha: String::new(),
                build_dirty: false,
                protocol_schema: 0,
                wire_protocol: String::new(),
                capabilities: serde_json::Value::Null,
            },
        }
    }
}

fn first_json_string(payload: &serde_json::Value, keys: &[&str]) -> String {
    keys.iter()
        .find_map(|key| payload.get(*key).and_then(|value| value.as_str()))
        .unwrap_or_default()
        .trim()
        .to_string()
}

fn apply_hello_payload_to_connection(conn: &mut UsbConnection, payload: &serde_json::Value) {
    let board_device_id =
        first_json_string(payload, &["boardDeviceId", "localDeviceId", "deviceId"]);
    if !board_device_id.is_empty() {
        conn.board_device_id = board_device_id;
    }
    let runtime = runtime_from_hello_payload(payload);
    if !runtime.is_empty() {
        conn.runtime = runtime;
    }
    let device_model = first_json_string(payload, &["deviceModel", "model", "boardModel"]);
    if !device_model.is_empty() {
        conn.device_model = device_model;
    }
    let firmware = first_json_string(payload, &["fw", "firmware", "firmwareVersion"]);
    if !firmware.is_empty() {
        conn.firmware = firmware;
    }
    let build_id = first_json_string(payload, &["buildId"]);
    if !build_id.is_empty() {
        conn.build_id = build_id;
    }
    let git_sha = first_json_string(payload, &["gitSha"]);
    if !git_sha.is_empty() {
        conn.git_sha = git_sha;
    }
    if let Some(build_dirty) = payload.get("buildDirty").and_then(|value| value.as_bool()) {
        conn.build_dirty = build_dirty;
    }
    if let Some(protocol_schema) = payload
        .get("protocolSchema")
        .and_then(|value| value.as_u64())
        .and_then(|value| u32::try_from(value).ok())
    {
        conn.protocol_schema = protocol_schema;
    }
    let wire_protocol = first_json_string(payload, &["wireProtocol", "protocol"]);
    if !wire_protocol.is_empty() {
        conn.wire_protocol = wire_protocol;
    }
    if let Some(capabilities) = payload.get("capabilities") {
        if !capabilities.is_null() {
            conn.capabilities = capabilities.clone();
        }
    }
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PreparedP4Appearance {
    pub profile: String,
    pub pack_id: String,
    pub file_count: u32,
    pub byte_count: u64,
}

fn p4_ready_profile_id() -> &'static str {
    static PROFILE: OnceLock<String> = OnceLock::new();
    PROFILE
        .get_or_init(|| {
            format!(
                "v{}-{}x{}-{}fps-{}f-h264-crf{}",
                P4_READY_PROFILE_VERSION,
                P4_APPEARANCE_WIDTH,
                P4_APPEARANCE_HEIGHT,
                P4_APPEARANCE_FPS,
                P4_APPEARANCE_MAX_FRAMES,
                P4_APPEARANCE_H264_CRF
            )
        })
        .as_str()
}

fn p4_ready_prepare_lock() -> &'static Mutex<()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
}

fn p4_ready_profile_root(appearance_dir: &Path) -> PathBuf {
    appearance_dir
        .join(P4_READY_DIR_NAME)
        .join(p4_ready_profile_id())
}

fn p4_ready_asset_path(profile_root: &Path, device_path: &str) -> Result<PathBuf, String> {
    let relative = Path::new(device_path);
    if !device_path.starts_with("p4/")
        || device_path.contains('\\')
        || relative.is_absolute()
        || relative
            .components()
            .any(|component| !matches!(component, std::path::Component::Normal(_)))
    {
        return Err(format!("invalid P4 ready asset path: {device_path}"));
    }
    Ok(profile_root.join(relative))
}

fn prepared_p4_summary(
    assets: &[AppearanceAssetEntry],
    pack_id: String,
) -> Result<PreparedP4Appearance, String> {
    let byte_count = assets.iter().try_fold(0u64, |total, asset| {
        let size = std::fs::metadata(&asset.source_path)
            .map_err(|error| {
                format!(
                    "stat P4 ready asset failed {}: {}",
                    asset.source_path.display(),
                    error
                )
            })?
            .len();
        Ok::<u64, String>(total.saturating_add(size))
    })?;
    Ok(PreparedP4Appearance {
        profile: p4_ready_profile_id().to_string(),
        pack_id,
        file_count: u32::try_from(assets.len())
            .map_err(|_| "P4 ready pack contains too many files".to_string())?,
        byte_count,
    })
}

fn load_p4_pack_from_profile_root(
    profile_root: &Path,
) -> Result<(Vec<AppearanceAssetEntry>, PreparedP4Appearance), String> {
    let manifest_path = profile_root.join("p4").join("manifest.json");
    let manifest_bytes = std::fs::read(&manifest_path).map_err(|error| {
        format!(
            "P4 ready pack is missing for profile {}: {} ({})",
            p4_ready_profile_id(),
            manifest_path.display(),
            error
        )
    })?;
    let manifest: serde_json::Value = serde_json::from_slice(&manifest_bytes)
        .map_err(|error| format!("parse P4 ready manifest failed: {error}"))?;
    if manifest.get("format").and_then(|value| value.as_str()) != Some("p4-h264-v1")
        || manifest.get("codec").and_then(|value| value.as_str()) != Some("h264")
        || manifest.get("container").and_then(|value| value.as_str()) != Some("annex-b")
        || manifest.get("width").and_then(|value| value.as_u64())
            != Some(u64::from(P4_APPEARANCE_WIDTH))
        || manifest.get("height").and_then(|value| value.as_u64())
            != Some(u64::from(P4_APPEARANCE_HEIGHT))
        || manifest.get("fps").and_then(|value| value.as_u64())
            != Some(u64::from(P4_APPEARANCE_FPS))
    {
        return Err(format!(
            "P4 ready manifest does not match profile {}",
            p4_ready_profile_id()
        ));
    }

    let families = manifest
        .get("families")
        .and_then(|value| value.as_array())
        .filter(|families| !families.is_empty())
        .ok_or_else(|| "P4 ready manifest contains no families".to_string())?;
    let mut assets = vec![AppearanceAssetEntry {
        family_name: "manifest".to_string(),
        kind: "p4-manifest",
        source_path: manifest_path,
        device_path: "p4/manifest.json".to_string(),
    }];
    let mut seen_paths = HashSet::new();
    seen_paths.insert("p4/manifest.json".to_string());

    for family in families {
        let family_name = family
            .get("family")
            .and_then(|value| value.as_str())
            .filter(|value| !value.is_empty())
            .ok_or_else(|| "P4 ready family is missing its name".to_string())?
            .to_string();
        let device_path = family
            .get("path")
            .and_then(|value| value.as_str())
            .filter(|value| !value.is_empty())
            .ok_or_else(|| format!("P4 ready family {family_name} is missing its path"))?
            .to_string();
        let frame_count = family
            .get("frames")
            .and_then(|value| value.as_u64())
            .filter(|value| *value > 0 && *value <= u64::from(P4_APPEARANCE_MAX_FRAMES))
            .ok_or_else(|| format!("P4 ready family {family_name} has an invalid frame count"))?;
        let expected_stream_bytes = family
            .get("streamBytes")
            .and_then(|value| value.as_u64())
            .filter(|value| *value > 0 && *value <= u64::from(u32::MAX))
            .ok_or_else(|| format!("P4 ready family {family_name} has invalid streamBytes"))?;
        let family_fps = family
            .get("fps")
            .and_then(|value| value.as_u64())
            .filter(|value| *value > 0 && *value <= u64::from(P4_APPEARANCE_FPS))
            .ok_or_else(|| format!("P4 ready family {family_name} has an invalid fps"))?;
        let frame_duration_ms = family
            .get("frameDurationMs")
            .and_then(|value| value.as_u64())
            .filter(|value| *value > 0 && *value <= u64::from(u32::MAX))
            .ok_or_else(|| {
                format!("P4 ready family {family_name} has an invalid frameDurationMs")
            })?;
        let duration_ms = family
            .get("durationMs")
            .and_then(|value| value.as_u64())
            .filter(|value| *value > 0 && *value <= u64::from(u32::MAX))
            .ok_or_else(|| format!("P4 ready family {family_name} has an invalid durationMs"))?;
        let maximum_duration_ms = frame_duration_ms
            .checked_mul(frame_count)
            .ok_or_else(|| format!("P4 ready family {family_name} duration overflows"))?;
        let minimum_duration_ms = frame_duration_ms
            .saturating_sub(1)
            .saturating_mul(frame_count);
        if duration_ms <= minimum_duration_ms || duration_ms > maximum_duration_ms {
            return Err(format!(
                "P4 ready family {family_name} durationMs does not match its frame timing"
            ));
        }
        let _ = family_fps;

        let source_path = p4_ready_asset_path(profile_root, &device_path)?;
        let stream_bytes = std::fs::read(&source_path).map_err(|error| {
            format!(
                "read P4 ready video failed {}: {}",
                source_path.display(),
                error
            )
        })?;
        let actual =
            parse_p4_h264_stream(&stream_bytes, P4_APPEARANCE_WIDTH, P4_APPEARANCE_HEIGHT)?;
        if u64::from(actual.frames) != frame_count
            || u64::from(actual.stream_bytes) != expected_stream_bytes
        {
            return Err(format!(
                "P4 ready family {family_name} frame index does not match {}",
                source_path.display()
            ));
        }
        if seen_paths.insert(device_path.clone()) {
            assets.push(AppearanceAssetEntry {
                family_name: family_name.clone(),
                kind: "p4-h264",
                source_path,
                device_path,
            });
        }

        if let Some(audio_device_path) = family
            .get("audioPath")
            .and_then(|value| value.as_str())
            .filter(|value| !value.is_empty())
        {
            let audio_source_path = p4_ready_asset_path(profile_root, audio_device_path)?;
            validate_p4_audio_wav(&std::fs::read(&audio_source_path).map_err(|error| {
                format!(
                    "read P4 ready audio failed {}: {}",
                    audio_source_path.display(),
                    error
                )
            })?)?;
            if seen_paths.insert(audio_device_path.to_string()) {
                assets.push(AppearanceAssetEntry {
                    family_name: family_name.clone(),
                    kind: "p4-audio",
                    source_path: audio_source_path,
                    device_path: audio_device_path.to_string(),
                });
            }
        }
    }

    let pack_id = p4_pack_id_from_assets(&assets)?;
    let mut identity_manifest = manifest.clone();
    identity_manifest["packId"] = serde_json::Value::String(String::new());
    let manifest_identity =
        serde_json::to_vec(&identity_manifest).map_err(|error| error.to_string())?;
    let computed_pack_id = compute_p4_pack_id(&assets[1..], &manifest_identity)?;
    if computed_pack_id != pack_id {
        return Err(format!(
            "P4 ready pack checksum mismatch: manifest={pack_id} computed={computed_pack_id}"
        ));
    }
    let summary = prepared_p4_summary(&assets, pack_id)?;
    Ok((assets, summary))
}

fn load_prepared_p4_appearance_pack(
    appearance_dir: &Path,
) -> Result<Vec<AppearanceAssetEntry>, String> {
    load_p4_pack_from_profile_root(&p4_ready_profile_root(appearance_dir))
        .map(|(assets, _)| assets)
        .map_err(|error| {
            format!(
                "{error}. Re-save or re-import this appearance so Pet Manager can prepare it before device transfer"
            )
        })
}

pub fn inspect_prepared_p4_appearance(
    appearance_dir: &Path,
) -> Result<PreparedP4Appearance, String> {
    load_p4_pack_from_profile_root(&p4_ready_profile_root(appearance_dir))
        .map(|(_, prepared)| prepared)
}

pub fn prepare_p4_appearance(
    appearance_dir: &Path,
    app_data_dir: &Path,
) -> Result<PreparedP4Appearance, String> {
    let _guard = p4_ready_prepare_lock()
        .lock()
        .map_err(|error| format!("lock P4 ready preparation failed: {error}"))?;
    let manifest_path = appearance_dir.join("manifest.json");
    let manifest: serde_json::Value =
        serde_json::from_slice(&std::fs::read(&manifest_path).map_err(|error| {
            format!(
                "read appearance manifest failed {}: {}",
                manifest_path.display(),
                error
            )
        })?)
        .map_err(|error| format!("parse appearance manifest failed: {error}"))?;
    let families = manifest
        .get("families")
        .and_then(|value| value.as_array())
        .ok_or_else(|| "appearance manifest contains no families array".to_string())?;

    let ready_base = appearance_dir.join(P4_READY_DIR_NAME);
    std::fs::create_dir_all(&ready_base)
        .map_err(|error| format!("create P4 ready directory failed: {error}"))?;
    let final_root = p4_ready_profile_root(appearance_dir);
    let existing_is_valid = load_p4_pack_from_profile_root(&final_root).is_ok();
    let staging = tempfile::Builder::new()
        .prefix(".prepare-")
        .tempdir_in(&ready_base)
        .map_err(|error| format!("create P4 ready staging directory failed: {error}"))?;
    let ffmpeg = crate::codex_import::resolve_ffmpeg()?;
    stage_p4_appearance_pack_with_exporter(
        families,
        appearance_dir,
        app_data_dir,
        staging.path(),
        |input, output| {
            if existing_is_valid {
                if let Ok(relative) = output.strip_prefix(staging.path()) {
                    let cached = final_root.join(relative);
                    if cached.is_file() {
                        if let Some(parent) = output.parent() {
                            std::fs::create_dir_all(parent).map_err(|error| {
                                format!("create cached P4 output directory failed: {error}")
                            })?;
                        }
                        if std::fs::copy(&cached, output).is_ok() {
                            if let Ok(bytes) = std::fs::read(output) {
                                if let (Ok(stream), Ok(duration_ms)) = (
                                    parse_p4_h264_stream(
                                        &bytes,
                                        P4_APPEARANCE_WIDTH,
                                        P4_APPEARANCE_HEIGHT,
                                    ),
                                    probe_video_duration_ms(&ffmpeg, input),
                                ) {
                                    return p4_exported_stream(
                                        duration_ms,
                                        stream.frames,
                                        stream.stream_bytes,
                                    );
                                }
                            }
                            let _ = std::fs::remove_file(output);
                        }
                    }
                }
            }
            export_p4_h264_stream(&ffmpeg, input, output)
        },
    )?;
    let (_, prepared) = load_p4_pack_from_profile_root(staging.path())?;

    if let Ok((_, existing)) = load_p4_pack_from_profile_root(&final_root) {
        if existing.pack_id == prepared.pack_id {
            return Ok(existing);
        }
    }

    let staging_path = staging.keep();
    let backup = ready_base.join(format!(".replace-{}", uuid::Uuid::new_v4().simple()));
    if final_root.exists() {
        std::fs::rename(&final_root, &backup)
            .map_err(|error| format!("stage previous P4 ready pack failed: {error}"))?;
    }
    if let Err(error) = std::fs::rename(&staging_path, &final_root) {
        if backup.exists() {
            let _ = std::fs::rename(&backup, &final_root);
        }
        let _ = std::fs::remove_dir_all(&staging_path);
        return Err(format!("activate prepared P4 ready pack failed: {error}"));
    }
    if backup.exists() {
        let _ = std::fs::remove_dir_all(&backup);
    }
    eprintln!(
        "[p4-ready] prepared profile={} pack_id={} files={} bytes={} source={}",
        prepared.profile,
        prepared.pack_id,
        prepared.file_count,
        prepared.byte_count,
        appearance_dir.display()
    );
    Ok(prepared)
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct P4ExportedStream {
    frames: u32,
    stream_bytes: u32,
    fps: u32,
    frame_duration_ms: u32,
    duration_ms: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct P4FrameSpec {
    family_name: String,
    device_path: String,
    frames: u32,
    stream_bytes: u32,
    fps: u32,
    frame_duration_ms: u32,
    duration_ms: u32,
    audio_device_path: Option<String>,
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

fn p4_safe_family_file_stem(family_name: &str) -> String {
    let stem = family_name
        .chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || ch == '.' || ch == '-' || ch == '_' {
                ch
            } else {
                '_'
            }
        })
        .collect::<String>();
    if stem.is_empty() {
        "unknown".to_string()
    } else {
        stem
    }
}

fn p4_file_sha256_hex(path: &Path) -> Result<String, String> {
    let mut file = std::fs::File::open(path)
        .map_err(|error| format!("open P4 source asset failed {}: {}", path.display(), error))?;
    let mut digest = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let read = file.read(&mut buffer).map_err(|error| {
            format!("read P4 source asset failed {}: {}", path.display(), error)
        })?;
        if read == 0 {
            break;
        }
        digest.update(&buffer[..read]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

fn p4_content_addressed_video_path(sha256: &str) -> Result<String, String> {
    let prefix = sha256
        .get(..24)
        .ok_or_else(|| "P4 video SHA-256 is too short".to_string())?;
    Ok(format!("p4/families/sha256-{prefix}.h264"))
}

fn build_p4_h264_manifest(
    specs: &[P4FrameSpec],
    width: u32,
    height: u32,
    fps: u32,
    pack_id: &str,
) -> serde_json::Value {
    let p4_families = specs
        .iter()
        .filter(|spec| spec.frames > 0 && spec.stream_bytes > 0)
        .map(|spec| {
            let mut family = serde_json::json!({
                "family": spec.family_name,
                "path": spec.device_path,
                "frames": spec.frames,
                "streamBytes": spec.stream_bytes,
                "fps": spec.fps,
                "frameDurationMs": spec.frame_duration_ms,
                "durationMs": spec.duration_ms,
            });
            if let Some(audio_path) = spec.audio_device_path.as_deref() {
                family["audioPath"] = serde_json::json!(audio_path);
            }
            family
        })
        .collect::<Vec<_>>();

    serde_json::json!({
        "format": "p4-h264-v1",
        "packId": pack_id,
        "codec": "h264",
        "container": "annex-b",
        "width": width,
        "height": height,
        "fps": fps,
        "families": p4_families,
    })
}

fn ffmpeg_container_duration_ms(stderr: &[u8]) -> Option<u64> {
    let text = String::from_utf8_lossy(stderr);
    let marker = "Duration: ";
    for line in text.lines() {
        let Some((_, remainder)) = line.split_once(marker) else {
            continue;
        };
        let Some(value) = remainder.split(',').next().map(str::trim) else {
            continue;
        };
        let mut parts = value.split(':');
        let (Some(hours), Some(minutes), Some(seconds)) = (
            parts.next().and_then(|part| part.parse::<u64>().ok()),
            parts.next().and_then(|part| part.parse::<u64>().ok()),
            parts.next().and_then(|part| part.parse::<f64>().ok()),
        ) else {
            continue;
        };
        if parts.next().is_some() || !seconds.is_finite() || seconds < 0.0 {
            continue;
        }
        let duration_ms =
            ((hours * 3600 + minutes * 60) as f64 * 1000.0 + seconds * 1000.0).round();
        if duration_ms > 0.0 && duration_ms <= u64::MAX as f64 {
            return Some(duration_ms as u64);
        }
    }
    None
}

fn probe_video_duration_ms(ffmpeg: &str, input: &Path) -> Result<u64, String> {
    let output = crate::command_for_host(ffmpeg)
        .args(["-hide_banner", "-nostdin", "-i"])
        .arg(input)
        .args([
            "-map",
            "0:v:0",
            "-c",
            "copy",
            "-f",
            "null",
            "-",
            "-progress",
            "pipe:1",
            "-nostats",
        ])
        .output()
        .map_err(|error| format!("start ffmpeg duration probe failed: {error}"))?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
        return Err(format!(
            "ffmpeg duration probe failed for {}: {}",
            input.display(),
            if stderr.is_empty() {
                format!("exit {:?}", output.status.code())
            } else {
                stderr
            }
        ));
    }
    if let Some(duration_ms) = ffmpeg_container_duration_ms(&output.stderr) {
        return Ok(duration_ms);
    }
    let duration_us = String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter_map(|line| {
            line.strip_prefix("out_time_us=")
                .or_else(|| line.strip_prefix("out_time_ms="))
                .and_then(|value| value.trim().parse::<u64>().ok())
        })
        .max()
        .unwrap_or(0);
    if duration_us == 0 {
        return Err(format!(
            "ffmpeg did not report a video duration for {}",
            input.display()
        ));
    }
    Ok(duration_us.div_ceil(1000))
}

fn p4_sampling_fps(duration_ms: u64) -> f64 {
    if duration_ms == 0 {
        return f64::from(P4_APPEARANCE_FPS);
    }
    (f64::from(P4_APPEARANCE_MAX_FRAMES) * 1000.0 / duration_ms as f64)
        .min(f64::from(P4_APPEARANCE_FPS))
        .max(0.01)
}

fn p4_exported_stream(
    duration_ms: u64,
    frames: u32,
    stream_bytes: u32,
) -> Result<P4ExportedStream, String> {
    if frames == 0 || stream_bytes == 0 {
        return Err("P4 H.264 stream contains no frames".to_string());
    }
    let frame_count = u64::from(frames);
    let frame_duration_ms = duration_ms.max(1).div_ceil(frame_count);
    let fallback_fps =
        ((1000 + frame_duration_ms / 2) / frame_duration_ms).clamp(1, u64::from(P4_APPEARANCE_FPS));
    Ok(P4ExportedStream {
        frames,
        stream_bytes,
        fps: fallback_fps as u32,
        frame_duration_ms: u32::try_from(frame_duration_ms)
            .map_err(|_| "P4 video frame duration exceeds u32".to_string())?,
        duration_ms: u32::try_from(duration_ms.max(1))
            .map_err(|_| "P4 video duration exceeds u32".to_string())?,
    })
}

fn build_p4_h264_ffmpeg_args(
    input: &Path,
    output: &Path,
    width: u32,
    height: u32,
    sampling_fps: f64,
    max_frames: u32,
) -> Vec<String> {
    vec![
        "-y".to_string(),
        "-v".to_string(),
        "error".to_string(),
        "-nostdin".to_string(),
        "-i".to_string(),
        input.display().to_string(),
        "-vf".to_string(),
        format!(
            "fps={sampling_fps:.6},scale={width}:{height}:force_original_aspect_ratio=decrease,pad={width}:{height}:(ow-iw)/2:(oh-ih)/2:black"
        ),
        "-frames:v".to_string(),
        max_frames.to_string(),
        "-an".to_string(),
        "-c:v".to_string(),
        "libx264".to_string(),
        "-preset".to_string(),
        "medium".to_string(),
        "-tune".to_string(),
        "zerolatency".to_string(),
        "-profile:v".to_string(),
        "baseline".to_string(),
        "-level:v".to_string(),
        "3.0".to_string(),
        "-crf".to_string(),
        P4_APPEARANCE_H264_CRF.to_string(),
        "-pix_fmt".to_string(),
        "yuv420p".to_string(),
        "-x264-params".to_string(),
        format!(
            "cabac=0:bframes=0:ref=1:weightp=0:scenecut=0:keyint={max_frames}:min-keyint={max_frames}:repeat-headers=1:aud=1:threads=1:sliced-threads=0"
        ),
        "-f".to_string(),
        "h264".to_string(),
        output.display().to_string(),
    ]
}

fn build_p4_audio_ffmpeg_args(input: &Path, output: &Path) -> Vec<String> {
    vec![
        "-y".to_string(),
        "-v".to_string(),
        "error".to_string(),
        "-i".to_string(),
        input.display().to_string(),
        "-map".to_string(),
        "0:a:0".to_string(),
        "-vn".to_string(),
        "-ac".to_string(),
        "1".to_string(),
        "-ar".to_string(),
        "16000".to_string(),
        "-c:a".to_string(),
        "pcm_s16le".to_string(),
        "-f".to_string(),
        "wav".to_string(),
        output.display().to_string(),
    ]
}

#[derive(Debug, Clone, Copy)]
struct P4AnnexBNal {
    start: usize,
    prefix_size: usize,
    end: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct P4H264Stream {
    frames: u32,
    stream_bytes: u32,
}

fn p4_annex_b_nals(bytes: &[u8]) -> Vec<P4AnnexBNal> {
    let mut starts = Vec::new();
    let mut cursor = 0usize;
    while cursor + 3 <= bytes.len() {
        if cursor + 4 <= bytes.len() && bytes[cursor..cursor + 4] == [0, 0, 0, 1] {
            starts.push((cursor, 4usize));
            cursor += 4;
        } else if bytes[cursor..cursor + 3] == [0, 0, 1] {
            starts.push((cursor, 3usize));
            cursor += 3;
        } else {
            cursor += 1;
        }
    }
    starts
        .iter()
        .enumerate()
        .map(|(index, (start, prefix_size))| P4AnnexBNal {
            start: *start,
            prefix_size: *prefix_size,
            end: starts
                .get(index + 1)
                .map(|(next, _)| *next)
                .unwrap_or(bytes.len()),
        })
        .collect()
}

fn p4_push_bit(bits: &mut Vec<u8>, value: u32) {
    bits.push((value & 1) as u8);
}

fn p4_push_bits(bits: &mut Vec<u8>, value: u32, count: u32) {
    for shift in (0..count).rev() {
        p4_push_bit(bits, value >> shift);
    }
}

fn p4_push_ue(bits: &mut Vec<u8>, value: u32) {
    let code_num = value + 1;
    let leading_zeroes = 31 - code_num.leading_zeros();
    bits.extend(std::iter::repeat_n(0, leading_zeroes as usize));
    p4_push_bits(bits, code_num, leading_zeroes + 1);
}

fn p4_finish_rbsp(mut bits: Vec<u8>) -> Vec<u8> {
    bits.push(1);
    while bits.len() & 7 != 0 {
        bits.push(0);
    }
    bits.chunks(8)
        .map(|chunk| chunk.iter().fold(0u8, |value, bit| (value << 1) | *bit))
        .collect()
}

fn p4_add_emulation_prevention(rbsp: &[u8]) -> Vec<u8> {
    let mut output = Vec::with_capacity(rbsp.len() + 4);
    let mut zero_count = 0usize;
    for value in rbsp {
        if zero_count >= 2 && *value <= 3 {
            output.push(3);
            zero_count = 0;
        }
        output.push(*value);
        zero_count = if *value == 0 { zero_count + 1 } else { 0 };
    }
    output
}

fn build_p4_minimal_h264_sps(width: u32, height: u32) -> Result<Vec<u8>, String> {
    if width == 0 || height == 0 || width & 1 != 0 || height & 1 != 0 {
        return Err("P4 H.264 width and height must be positive even values".to_string());
    }
    let encoded_width = width.div_ceil(16) * 16;
    let encoded_height = height.div_ceil(16) * 16;
    let crop_right = (encoded_width - width) / 2;
    let crop_bottom = (encoded_height - height) / 2;
    let has_crop = crop_right != 0 || crop_bottom != 0;
    let mut bits = Vec::new();
    p4_push_ue(&mut bits, 0); // seq_parameter_set_id
    p4_push_ue(&mut bits, 0); // log2_max_frame_num_minus4
    p4_push_ue(&mut bits, 2); // pic_order_cnt_type
    p4_push_ue(&mut bits, 1); // max_num_ref_frames
    p4_push_bit(&mut bits, 0); // gaps_in_frame_num_value_allowed_flag
    p4_push_ue(&mut bits, encoded_width / 16 - 1);
    p4_push_ue(&mut bits, encoded_height / 16 - 1);
    p4_push_bit(&mut bits, 1); // frame_mbs_only_flag
    p4_push_bit(&mut bits, 0); // direct_8x8_inference_flag
    p4_push_bit(&mut bits, u32::from(has_crop));
    if has_crop {
        p4_push_ue(&mut bits, 0);
        p4_push_ue(&mut bits, crop_right);
        p4_push_ue(&mut bits, 0);
        p4_push_ue(&mut bits, crop_bottom);
    }
    p4_push_bit(&mut bits, 0); // vui_parameters_present_flag

    let mut rbsp = vec![66, 0xc0, 30];
    rbsp.extend(p4_finish_rbsp(bits));
    let mut nal = vec![0x67];
    nal.extend(p4_add_emulation_prevention(&rbsp));
    Ok(nal)
}

fn rewrite_p4_h264_sps(bytes: &[u8], width: u32, height: u32) -> Result<Vec<u8>, String> {
    let replacement = build_p4_minimal_h264_sps(width, height)?;
    let mut output = Vec::with_capacity(bytes.len());
    let mut cursor = 0usize;
    let mut replaced = 0usize;
    for nal in p4_annex_b_nals(bytes) {
        let payload_start = nal.start + nal.prefix_size;
        if payload_start >= nal.end || bytes[payload_start] & 0x1f != 7 {
            continue;
        }
        output.extend_from_slice(&bytes[cursor..payload_start]);
        output.extend_from_slice(&replacement);
        cursor = nal.end;
        replaced += 1;
    }
    if replaced == 0 {
        return Err("P4 H.264 stream contains no SPS".to_string());
    }
    output.extend_from_slice(&bytes[cursor..]);
    Ok(output)
}

fn parse_p4_h264_stream(bytes: &[u8], width: u32, height: u32) -> Result<P4H264Stream, String> {
    let expected_sps = build_p4_minimal_h264_sps(width, height)?;
    let mut access_units = 0u32;
    let mut slices_in_access_unit = 0u32;
    let mut has_sps = false;
    let mut has_pps = false;
    for nal in p4_annex_b_nals(bytes) {
        let payload_start = nal.start + nal.prefix_size;
        if payload_start >= nal.end {
            continue;
        }
        match bytes[payload_start] & 0x1f {
            1 | 5 => {
                if access_units == 0 {
                    return Err("P4 H.264 stream is missing access unit delimiters".to_string());
                }
                slices_in_access_unit = slices_in_access_unit.saturating_add(1);
                if slices_in_access_unit > 1 {
                    return Err(
                        "P4 H.264 stream must contain exactly one slice per access unit"
                            .to_string(),
                    );
                }
            }
            7 => {
                if bytes[payload_start..nal.end] != expected_sps {
                    return Err("P4 H.264 stream uses an incompatible SPS".to_string());
                }
                has_sps = true;
            }
            8 => has_pps = true,
            9 => {
                if access_units > 0 && slices_in_access_unit != 1 {
                    return Err(
                        "P4 H.264 stream must contain exactly one slice per access unit"
                            .to_string(),
                    );
                }
                access_units = access_units.saturating_add(1);
                slices_in_access_unit = 0;
            }
            _ => {}
        }
    }
    if access_units == 0 || slices_in_access_unit != 1 {
        return Err("P4 H.264 stream must contain exactly one slice per access unit".to_string());
    }
    if !has_sps || !has_pps {
        return Err("P4 H.264 stream is missing SPS, PPS, or video frames".to_string());
    }
    Ok(P4H264Stream {
        frames: access_units,
        stream_bytes: u32::try_from(bytes.len())
            .map_err(|_| "P4 H.264 stream is too large".to_string())?,
    })
}

fn validate_p4_audio_wav(bytes: &[u8]) -> Result<(), String> {
    if bytes.len() < 12 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return Err("P4 audio cue is not a RIFF/WAVE file".to_string());
    }
    if bytes.len() > P4_APPEARANCE_AUDIO_MAX_BYTES {
        return Err(format!(
            "P4 audio cue exceeds {} bytes",
            P4_APPEARANCE_AUDIO_MAX_BYTES
        ));
    }

    let mut offset = 12usize;
    let mut format_ok = false;
    let mut data_ok = false;
    while offset + 8 <= bytes.len() {
        let chunk_size = u32::from_le_bytes(
            bytes[offset + 4..offset + 8]
                .try_into()
                .map_err(|_| "P4 WAV chunk size is invalid")?,
        ) as usize;
        let data_offset = offset + 8;
        let data_end = data_offset
            .checked_add(chunk_size)
            .ok_or("P4 WAV chunk size overflow")?;
        if data_end > bytes.len() {
            return Err("P4 WAV chunk exceeds file size".to_string());
        }
        match &bytes[offset..offset + 4] {
            b"fmt " if chunk_size >= 16 => {
                let audio_format =
                    u16::from_le_bytes(bytes[data_offset..data_offset + 2].try_into().unwrap());
                let channels =
                    u16::from_le_bytes(bytes[data_offset + 2..data_offset + 4].try_into().unwrap());
                let sample_rate =
                    u32::from_le_bytes(bytes[data_offset + 4..data_offset + 8].try_into().unwrap());
                let bits = u16::from_le_bytes(
                    bytes[data_offset + 14..data_offset + 16]
                        .try_into()
                        .unwrap(),
                );
                format_ok =
                    audio_format == 1 && channels == 1 && sample_rate == 16_000 && bits == 16;
            }
            b"data" => data_ok = chunk_size > 0 && chunk_size & 1 == 0,
            _ => {}
        }
        offset = data_end + (chunk_size & 1);
    }
    if !format_ok {
        return Err("P4 audio cue must be PCM 16 kHz mono 16-bit WAV".to_string());
    }
    if !data_ok {
        return Err("P4 audio cue has no aligned PCM data".to_string());
    }
    Ok(())
}

fn stage_p4_audio_cue(source: &Path, output: &Path) -> Result<(), String> {
    let bytes = std::fs::read(source)
        .map_err(|error| format!("read P4 audio cue failed {}: {}", source.display(), error))?;
    if let Some(parent) = output.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|error| format!("create P4 audio directory failed: {}", error))?;
    }
    match validate_p4_audio_wav(&bytes) {
        Ok(()) => std::fs::write(output, bytes)
            .map_err(|error| format!("write P4 audio cue failed {}: {}", output.display(), error)),
        Err(source_error) => {
            let ffmpeg = crate::codex_import::resolve_ffmpeg().map_err(|error| {
                format!(
                    "{}; automatic P4 audio conversion is unavailable: {}",
                    source_error, error
                )
            })?;
            let result = crate::command_for_host(&ffmpeg)
                .args(build_p4_audio_ffmpeg_args(source, output))
                .output()
                .map_err(|error| {
                    format!(
                        "{}; start ffmpeg P4 audio conversion failed for {}: {}",
                        source_error,
                        source.display(),
                        error
                    )
                })?;
            if !result.status.success() {
                let _ = std::fs::remove_file(output);
                let stderr = String::from_utf8_lossy(&result.stderr).trim().to_string();
                return Err(format!(
                    "{}; ffmpeg P4 audio conversion failed for {}: {}",
                    source_error,
                    source.display(),
                    if stderr.is_empty() {
                        format!("exit {:?}", result.status.code())
                    } else {
                        stderr
                    }
                ));
            }
            let normalized = std::fs::read(output).map_err(|error| {
                format!(
                    "read normalized P4 audio cue failed {}: {}",
                    output.display(),
                    error
                )
            })?;
            if let Err(normalized_error) = validate_p4_audio_wav(&normalized) {
                let _ = std::fs::remove_file(output);
                return Err(format!(
                    "normalized P4 audio cue is invalid for {}: {}",
                    source.display(),
                    normalized_error
                ));
            }
            eprintln!(
                "[usb-p4-ota] normalized legacy audio cue to PCM 16 kHz mono 16-bit: {}",
                source.display()
            );
            Ok(())
        }
    }
}

fn collect_p4_asset_pack(
    staging_dir: &Path,
    specs: &[P4FrameSpec],
    width: u32,
    height: u32,
    fps: u32,
) -> Result<Vec<AppearanceAssetEntry>, String> {
    let mut payload_assets = Vec::new();
    let mut seen_device_paths = HashSet::new();
    for spec in specs
        .iter()
        .filter(|spec| spec.frames > 0 && spec.stream_bytes > 0)
    {
        if seen_device_paths.insert(spec.device_path.clone()) {
            let source_path = staging_dir.join(&spec.device_path);
            if !source_path.is_file() {
                return Err(format!("P4 帧文件不存在: {}", source_path.display()));
            }
            payload_assets.push(AppearanceAssetEntry {
                family_name: spec.family_name.clone(),
                kind: "p4-h264",
                source_path,
                device_path: spec.device_path.clone(),
            });
        }
        if let Some(device_path) = spec.audio_device_path.as_deref() {
            if !seen_device_paths.insert(device_path.to_string()) {
                continue;
            }
            let source_path = staging_dir.join(device_path);
            if !source_path.is_file() {
                return Err(format!(
                    "P4 audio cue is missing: {}",
                    source_path.display()
                ));
            }
            payload_assets.push(AppearanceAssetEntry {
                family_name: spec.family_name.clone(),
                kind: "p4-audio",
                source_path,
                device_path: device_path.to_string(),
            });
        }
    }

    let identity_manifest = build_p4_h264_manifest(specs, width, height, fps, "");
    let manifest_identity =
        serde_json::to_vec(&identity_manifest).map_err(|error| error.to_string())?;
    let pack_id = compute_p4_pack_id(&payload_assets, &manifest_identity)?;
    let manifest = build_p4_h264_manifest(specs, width, height, fps, &pack_id);
    let manifest_path = staging_dir.join("p4").join("manifest.json");
    if let Some(parent) = manifest_path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("创建 P4 manifest 目录失败: {}", e))?;
    }
    std::fs::write(
        &manifest_path,
        serde_json::to_vec_pretty(&manifest).map_err(|e| e.to_string())?,
    )
    .map_err(|e| format!("写入 P4 manifest 失败: {}", e))?;

    let mut assets = vec![AppearanceAssetEntry {
        family_name: "manifest".to_string(),
        kind: "p4-manifest",
        source_path: manifest_path,
        device_path: "p4/manifest.json".to_string(),
    }];

    assets.extend(payload_assets);

    Ok(assets)
}

fn stage_p4_appearance_pack_with_exporter<F>(
    families: &[serde_json::Value],
    appearance_dir: &Path,
    app_data_dir: &Path,
    staging_dir: &Path,
    exporter: F,
) -> Result<Vec<AppearanceAssetEntry>, String>
where
    F: Fn(&Path, &Path) -> Result<P4ExportedStream, String>,
{
    let frames_dir = staging_dir.join("p4").join("families");
    std::fs::create_dir_all(&frames_dir).map_err(|e| format!("创建 P4 帧目录失败: {}", e))?;

    let mut specs = Vec::new();
    let mut shared_videos: HashMap<String, (String, P4ExportedStream)> = HashMap::new();
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

        let source_sha256 = p4_file_sha256_hex(&video_path)?;
        let (device_path, exported) =
            if let Some(shared) = shared_videos.get(&source_sha256).cloned() {
                shared
            } else {
                let device_path = p4_content_addressed_video_path(&source_sha256)?;
                let output_path = staging_dir.join(&device_path);
                let exported = exporter(&video_path, &output_path)?;
                if exported.frames == 0 || exported.stream_bytes == 0 {
                    continue;
                }
                if !output_path.is_file() {
                    return Err(format!(
                        "P4 pre-conversion did not create output: {}",
                        output_path.display()
                    ));
                }
                shared_videos.insert(source_sha256, (device_path.clone(), exported.clone()));
                (device_path, exported)
            };
        let audio_device_path = if let Some(audio_path) = family_audio_path(
            family,
            &family_name,
            &video_path,
            appearance_dir,
            app_data_dir,
        )
        .filter(|path| path.is_file())
        {
            let device_path = format!("p4/audio/{}.wav", p4_safe_family_file_stem(&family_name));
            stage_p4_audio_cue(&audio_path, &staging_dir.join(&device_path))?;
            Some(device_path)
        } else {
            None
        };
        specs.push(P4FrameSpec {
            family_name,
            device_path,
            frames: exported.frames,
            stream_bytes: exported.stream_bytes,
            fps: exported.fps,
            frame_duration_ms: exported.frame_duration_ms,
            duration_ms: exported.duration_ms,
            audio_device_path,
        });
    }

    if specs.is_empty() {
        return Err("没有可同步到 ESP-P4 的形象帧".to_string());
    }

    collect_p4_asset_pack(
        staging_dir,
        &specs,
        P4_APPEARANCE_WIDTH,
        P4_APPEARANCE_HEIGHT,
        P4_APPEARANCE_FPS,
    )
}

fn export_p4_h264_stream(
    ffmpeg: &str,
    input: &Path,
    output: &Path,
) -> Result<P4ExportedStream, String> {
    if let Some(parent) = output.parent() {
        std::fs::create_dir_all(parent).map_err(|e| {
            format!(
                "create P4 video output directory failed {}: {}",
                parent.display(),
                e
            )
        })?;
    }
    let duration_ms = probe_video_duration_ms(ffmpeg, input)?;
    let args = build_p4_h264_ffmpeg_args(
        input,
        output,
        P4_APPEARANCE_WIDTH,
        P4_APPEARANCE_HEIGHT,
        p4_sampling_fps(duration_ms),
        P4_APPEARANCE_MAX_FRAMES,
    );
    let result = crate::command_for_host(ffmpeg)
        .args(&args)
        .output()
        .map_err(|e| format!("start ffmpeg P4 export failed: {}", e))?;
    if !result.status.success() {
        let stderr = String::from_utf8_lossy(&result.stderr).trim().to_string();
        return Err(format!(
            "ffmpeg P4 export failed for {}: {}",
            input.display(),
            if stderr.is_empty() {
                format!("exit {:?}", result.status.code())
            } else {
                stderr
            }
        ));
    }
    let bytes = std::fs::read(output)
        .map_err(|e| format!("read P4 H.264 stream failed {}: {}", output.display(), e))?;
    let compatible = rewrite_p4_h264_sps(&bytes, P4_APPEARANCE_WIDTH, P4_APPEARANCE_HEIGHT)?;
    std::fs::write(output, &compatible)
        .map_err(|e| format!("write compatible P4 H.264 stream failed: {e}"))?;
    let stream = parse_p4_h264_stream(&compatible, P4_APPEARANCE_WIDTH, P4_APPEARANCE_HEIGHT)?;
    p4_exported_stream(duration_ms, stream.frames, stream.stream_bytes)
}
fn appearance_asset_chunk_delay(encoded_len: usize) -> Duration {
    let transfer_ms = ((encoded_len as u64) * 1000)
        .div_ceil(usb_uart_wire_bytes_per_sec(DEFAULT_USB_SERIAL_BAUD));
    let delay_ms = transfer_ms
        .saturating_add(APPEARANCE_ASSET_CHUNK_DELAY_MARGIN_MS)
        .max(APPEARANCE_ASSET_CHUNK_DELAY_FLOOR_MS);
    Duration::from_millis(delay_ms)
}

#[cfg(test)]
fn p4_appearance_asset_chunk_wire_time(encoded_len: usize) -> Duration {
    Duration::from_millis(
        ((encoded_len as u64) * 1000).div_ceil(usb_uart_wire_bytes_per_sec(P4_USB_UART_BAUD)),
    )
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

fn prioritize_usb_serial_devices(devices: Vec<UsbDeviceInfo>) -> Vec<UsbDeviceInfo> {
    let mut devices = prefer_callout_ports_for_macos(devices);
    devices.sort_by(|left, right| {
        usb_device_auto_connect_priority(left)
            .cmp(&usb_device_auto_connect_priority(right))
            .then_with(|| {
                serial_port_priority(&left.port_name).cmp(&serial_port_priority(&right.port_name))
            })
            .then_with(|| left.port_name.cmp(&right.port_name))
    });
    devices
}

fn usb_device_auto_connect_priority(device: &UsbDeviceInfo) -> u8 {
    match (device.vid, device.pid) {
        (0x303a, _) => 0,
        (0x1a86, 0x55d3) => 1,
        (0x1d6b, _) | (0x0525, _) => 2,
        (0x10c4, _) | (0x0403, _) => 3,
        (0x1a86, _) => 4,
        _ => 5,
    }
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
        || vid == 0x303a
        || vid == 0x1a86
        || vid == 0x10c4
        || vid == 0x0403
}

fn canonical_binding_for_control(control: &str) -> Option<(&'static str, &'static str)> {
    let binding = match control {
        "屏幕点击" => ("屏幕区域", "screen.region.tap"),
        "屏幕长按" => ("屏幕区域", "screen.region.long_press"),
        "SW1 短按" => ("SW1", "button.sw1.short_press"),
        "SW2 短按" => ("SW2", "button.sw2.short_press"),
        "SW3 短按" => ("SW3", "button.sw3.short_press"),
        "摇杆中按短按" => ("前方摇杆", "button.encoder.short_press"),
        "摇杆中按长按" => ("前方摇杆", "button.encoder.long_press"),
        "摇杆向上" => ("前方摇杆", "joystick.up"),
        "摇杆向下" => ("前方摇杆", "joystick.down"),
        "摇杆向左" => ("前方摇杆", "knob.rotate_ccw"),
        "摇杆向右" => ("前方摇杆", "knob.rotate_cw"),
        "摇杆左右方向" => ("前方摇杆", "knob.rotate_cw / knob.rotate_ccw"),
        // Backward-compatible labels and event names from encoder hardware.
        "旋钮短按" => ("前方旋钮", "button.encoder.short_press"),
        "旋钮长按" => ("前方旋钮", "button.encoder.long_press"),
        "旋钮顺时针" => ("前方旋钮", "knob.rotate_cw"),
        "旋钮逆时针" => ("前方旋钮", "knob.rotate_ccw"),
        "旋钮双向旋转" => ("前方旋钮", "knob.rotate_cw / knob.rotate_ccw"),
        "旋钮旋转" => ("前方旋钮", "knob.rotate_cw / knob.rotate_ccw"),
        "屏幕区域" => ("屏幕区域", "screen.region.tap"),
        _ => return None,
    };
    Some(binding)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn appearance_sync_cancellation_is_scoped_to_an_active_transfer() {
        let manager = UsbSerialManager::new();
        assert!(!manager.cancel_appearance_sync());
        manager
            .begin_appearance_sync()
            .expect("begin appearance sync");
        assert!(manager.begin_appearance_sync().is_err());
        assert!(manager.cancel_appearance_sync());
        assert!(manager.ensure_appearance_sync_not_cancelled(None).is_err());
        manager.finish_appearance_sync();
        assert!(!manager.cancel_appearance_sync());
        manager
            .ensure_appearance_sync_not_cancelled(None)
            .expect("cancel flag resets after completion");
    }

    #[test]
    fn usb_component_override_accepts_combined_encoder_rotation() {
        assert_eq!(
            canonical_binding_for_control("旋钮双向旋转"),
            Some(("前方旋钮", "knob.rotate_cw / knob.rotate_ccw"))
        );
        assert_eq!(
            canonical_binding_for_control("摇杆向下"),
            Some(("前方摇杆", "joystick.down"))
        );
    }

    #[test]
    fn usb_serial_source_contains_no_known_mojibake_codepoints() {
        let sources = [
            ("usb_serial.rs", include_str!("usb_serial.rs")),
            (
                "usb_serial/connection_handle.rs",
                include_str!("usb_serial/connection_handle.rs"),
            ),
            (
                "usb_serial/transaction_waiters.rs",
                include_str!("usb_serial/transaction_waiters.rs"),
            ),
            (
                "usb_serial/firmware_transaction.rs",
                include_str!("usb_serial/firmware_transaction.rs"),
            ),
            (
                "usb_serial/widget_transaction.rs",
                include_str!("usb_serial/widget_transaction.rs"),
            ),
            (
                "usb_serial/appearance_transaction.rs",
                include_str!("usb_serial/appearance_transaction.rs"),
            ),
        ];
        let forbidden = [
            '\u{922b}', '\u{93c9}', '\u{705e}', '\u{93c3}', '\u{9352}', '\u{93b5}', '\u{7487}',
            '\u{6fb6}', '\u{752f}', '\u{5a0c}', '\u{fffd}',
        ];
        for (path, source) in sources {
            for character in forbidden {
                assert!(
                    !source.contains(character),
                    "{path} contains mojibake sentinel U+{:04X}",
                    character as u32
                );
            }
        }
    }

    #[derive(Clone)]
    struct SharedWriter(Arc<Mutex<Vec<u8>>>);

    #[derive(Default)]
    struct PacedWriterState {
        writes: Vec<Vec<u8>>,
        flushes: usize,
    }

    struct PacedWriter(Arc<Mutex<PacedWriterState>>);

    fn p4_test_wav() -> Vec<u8> {
        let pcm = [0x00, 0x00, 0xff, 0x7f];
        let mut wav = Vec::with_capacity(44 + pcm.len());
        wav.extend_from_slice(b"RIFF");
        wav.extend_from_slice(&(36u32 + pcm.len() as u32).to_le_bytes());
        wav.extend_from_slice(b"WAVEfmt ");
        wav.extend_from_slice(&16u32.to_le_bytes());
        wav.extend_from_slice(&1u16.to_le_bytes());
        wav.extend_from_slice(&1u16.to_le_bytes());
        wav.extend_from_slice(&16_000u32.to_le_bytes());
        wav.extend_from_slice(&32_000u32.to_le_bytes());
        wav.extend_from_slice(&2u16.to_le_bytes());
        wav.extend_from_slice(&16u16.to_le_bytes());
        wav.extend_from_slice(b"data");
        wav.extend_from_slice(&(pcm.len() as u32).to_le_bytes());
        wav.extend_from_slice(&pcm);
        wav
    }

    impl Write for SharedWriter {
        fn write(&mut self, buffer: &[u8]) -> std::io::Result<usize> {
            self.0.lock().unwrap().extend_from_slice(buffer);
            Ok(buffer.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    impl Write for PacedWriter {
        fn write(&mut self, buffer: &[u8]) -> std::io::Result<usize> {
            self.0.lock().unwrap().writes.push(buffer.to_vec());
            Ok(buffer.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            self.0.lock().unwrap().flushes += 1;
            Ok(())
        }
    }

    #[test]
    fn non_utf8_boot_output_does_not_block_following_json_message() {
        let input = b"\xff\xfeESP-ROM boot output\r\n{\"topic\":\"hello\",\"payload\":{\"fw\":\"0.4.0-p4\"}}\r\n";
        let mut reader = BufReader::new(std::io::Cursor::new(input));
        let mut buffer = Vec::new();

        let boot_line = read_serial_line_lossy(&mut reader, &mut buffer)
            .unwrap()
            .unwrap();
        assert!(serde_json::from_str::<SerialMessage>(&boot_line).is_err());

        let json_line = read_serial_line_lossy(&mut reader, &mut buffer)
            .unwrap()
            .unwrap();
        let message: SerialMessage = serde_json::from_str(&json_line).unwrap();

        assert_eq!(message.topic, "hello");
        assert_eq!(message.payload["fw"], "0.4.0-p4");
    }

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
    fn accepts_espressif_native_usb_cdc_ports() {
        assert!(is_supported_usb_serial_port("COM15", 0x303a));
    }

    #[test]
    fn wch_adapters_probe_protocol_bauds_without_using_the_com_number() {
        for (pid, port_name) in [
            (0x55d3, "COM5"),
            (0x55d4, "/dev/cu.wchusbserial1420"),
            (0x7523, "COM3"),
        ] {
            let device = UsbDeviceInfo {
                port_name: port_name.to_string(),
                vid: 0x1a86,
                pid,
                serial_number: String::new(),
                manufacturer: "wch.cn".to_string(),
                product: "WCH USB serial".to_string(),
            };
            assert_eq!(
                serial_baud_candidates_for_device(Some(&device)),
                vec![
                    P4_USB_UART_BAUD,
                    P4_USB_UART_LEGACY_BAUD,
                    DEFAULT_USB_SERIAL_BAUD,
                    LEGACY_USB_SERIAL_BAUD
                ]
            );
        }

        let macos_driver_name_only = UsbDeviceInfo {
            port_name: "/dev/cu.wchusbserial110".to_string(),
            vid: 0,
            pid: 0,
            serial_number: String::new(),
            manufacturer: String::new(),
            product: String::new(),
        };
        assert!(usb_serial_device_prefers_high_speed(
            &macos_driver_name_only
        ));
    }

    #[test]
    fn probe_requests_identity_without_acknowledging_an_unverified_device() {
        let messages = serial_probe_handshake_messages("desktop-1");

        assert!(SERIAL_PROBE_TIMEOUT >= Duration::from_secs(8));
        if cfg!(windows) {
            assert!(SERIAL_AUTO_PROBE_TIMEOUT < SERIAL_PROBE_TIMEOUT);
            assert!(SERIAL_AUTO_PROBE_TIMEOUT >= Duration::from_secs(1));
        } else {
            assert_eq!(SERIAL_AUTO_PROBE_TIMEOUT, SERIAL_PROBE_TIMEOUT);
        }
        assert!(SERIAL_PROBE_HANDSHAKE_RETRY_INTERVAL <= Duration::from_secs(1));
        assert_eq!(messages[0]["topic"], "bind");
        assert_eq!(messages[0]["payload"]["desktopDeviceId"], "desktop-1");
        assert_eq!(messages[1]["type"], "hello");
        assert!(messages.iter().all(|message| message["topic"] != "ack"));
    }

    #[test]
    fn protocol_hello_not_port_metadata_decides_the_runtime() {
        let p4 = serde_json::json!({
            "boardDeviceId": "board-a",
            "runtime": "ESP32-P4",
        });
        let nested_p4 = serde_json::json!({
            "boardDeviceId": "board-b",
            "runtime": "future-runtime-name",
            "capabilities": {
                "appearance": { "formats": ["p4-mjpeg-v1"] },
                "transport": { "nativeProtocol": "pet-usb-native-v1" },
            },
        });
        let unknown = serde_json::json!({ "boardDeviceId": "board-a" });

        assert_eq!(runtime_from_hello_payload(&p4), "esp-p4");
        assert_eq!(runtime_from_hello_payload(&nested_p4), "esp-p4");
        assert_eq!(runtime_from_hello_payload(&unknown), "");
    }

    #[test]
    fn linux_runtime_is_verified_from_protocol_metadata_not_adapter_metadata() {
        let explicit = serde_json::json!({
            "boardDeviceId": "linux-board-1",
            "runtime": "board-server-c",
        });
        let compatible_usb_hello = serde_json::json!({
            "boardDeviceId": "linux-board-2",
            "online": true,
            "transport": "usb",
            "ts": "2026-07-20T00:00:00Z",
            "tsMs": 1784505600000_i64,
        });
        let arbitrary_serial_json = serde_json::json!({
            "boardDeviceId": "not-enough-evidence",
            "transport": "usb",
        });

        assert_eq!(runtime_from_hello_payload(&explicit), "linux");
        assert_eq!(runtime_from_hello_payload(&compatible_usb_hello), "linux");
        assert_eq!(runtime_from_hello_payload(&arbitrary_serial_json), "");
    }

    #[test]
    fn legacy_type_hello_is_normalized_to_a_verified_linux_runtime() {
        let message = parse_serial_message(
            r#"{"v":1,"type":"hello_ack","boardDeviceId":"linux-board-1","fw":"0.1.0"}"#,
        )
        .unwrap();

        assert_eq!(message.topic, "hello");
        assert_eq!(message.payload["runtime"], "linux");
        assert_eq!(message.payload["wireProtocol"], "pet-usb-legacy-v1");
    }

    #[test]
    fn windows_scan_uses_stable_adapter_priority_without_assigning_runtime() {
        let devices = vec![
            UsbDeviceInfo {
                port_name: "COM3".to_string(),
                vid: 0x1a86,
                pid: 0x7523,
                serial_number: "ch340-debug".to_string(),
                manufacturer: "wch.cn".to_string(),
                product: "USB-SERIAL CH340".to_string(),
            },
            UsbDeviceInfo {
                port_name: "COM5".to_string(),
                vid: 0x1a86,
                pid: 0x55d3,
                serial_number: "p4-ch343".to_string(),
                manufacturer: "wch.cn".to_string(),
                product: "USB-Enhanced-SERIAL CH343".to_string(),
            },
        ];

        let prioritized = prioritize_usb_serial_devices(devices);

        assert_eq!(prioritized[0].port_name, "COM5");
        assert_eq!(prioritized[1].port_name, "COM3");
    }

    #[test]
    fn serial_open_retries_documented_windows_lock_errors() {
        assert!(serial_open_error_is_transient("Access is denied"));
        assert!(serial_open_error_is_transient("拒绝访问。"));
        assert!(serial_open_error_is_transient(
            "Unable to acquire exclusive lock"
        ));
        assert!(!serial_open_error_is_transient(
            "The device is not connected"
        ));
    }

    #[test]
    fn checked_asset_chunk_waits_for_matching_board_ack() {
        let manager = UsbSerialManager::new();
        {
            let mut conn = manager.connection.lock().unwrap();
            *conn = Some(UsbConnection {
                connection_id: 1,
                port_name: "COM5".to_string(),
                baud_rate: P4_USB_UART_BAUD,
                writer: Box::new(Vec::<u8>::new()),
                board_device_id: String::new(),
                runtime: String::new(),
                device_model: String::new(),
                firmware: String::new(),
                build_id: String::new(),
                git_sha: String::new(),
                build_dirty: false,
                protocol_schema: 0,
                wire_protocol: String::new(),
                capabilities: serde_json::Value::Null,
                connected: true,
                cancel_reader: Arc::new(AtomicBool::new(false)),
            });
        }
        let waiters = Arc::clone(&manager.asset_ack_waiters);
        thread::spawn(move || {
            thread::sleep(Duration::from_millis(50));
            resolve_asset_ack(
                &waiters,
                "asset/ack",
                &serde_json::json!({
                    "transferId": "chunk-test",
                    "phase": "chunk",
                    "path": "p4/families/idle.default.h264",
                    "ok": true,
                }),
            );
        });

        let started = std::time::Instant::now();
        manager
            .send_asset_chunk_checked(
                "chunk-test",
                "p4/families/idle.default.h264",
                "aGVsbG8=",
                5,
                0,
            )
            .unwrap();

        assert!(started.elapsed() >= Duration::from_millis(40));
    }

    #[test]
    fn raw_asset_chunk_writes_json_header_then_unencoded_bytes() {
        let manager = UsbSerialManager::new();
        let written = Arc::new(Mutex::new(Vec::<u8>::new()));
        {
            let mut conn = manager.connection.lock().unwrap();
            *conn = Some(UsbConnection {
                connection_id: 1,
                port_name: "COM15".to_string(),
                baud_rate: P4_USB_UART_BAUD,
                writer: Box::new(SharedWriter(Arc::clone(&written))),
                board_device_id: "p4-test".to_string(),
                runtime: "esp-p4".to_string(),
                device_model: "ESP32-P4".to_string(),
                firmware: "0.7.11-p4".to_string(),
                build_id: String::new(),
                git_sha: String::new(),
                build_dirty: false,
                protocol_schema: 0,
                wire_protocol: "pet-usb-jsonl-v3".to_string(),
                capabilities: serde_json::Value::Null,
                connected: true,
                cancel_reader: Arc::new(AtomicBool::new(false)),
            });
        }
        let waiters = Arc::clone(&manager.asset_ack_waiters);
        thread::spawn(move || {
            thread::sleep(Duration::from_millis(20));
            resolve_asset_ack(
                &waiters,
                "asset/ack",
                &serde_json::json!({
                    "transferId": "raw-test",
                    "phase": "raw-chunk",
                    "path": "p4/families/idle.default.h264",
                    "index": "7",
                    "ok": true,
                }),
            );
        });

        let chunk = [0x00, b'\n', 0xff, 0xd8, b'{', b'}', 0xff, 0xd9];
        manager
            .send_asset_raw_chunk_checked("raw-test", "p4/families/idle.default.h264", &chunk, 7)
            .unwrap();

        let bytes = written.lock().unwrap().clone();
        let header_end = bytes
            .iter()
            .position(|byte| *byte == b'\n')
            .expect("raw chunk header terminator");
        let header: serde_json::Value =
            serde_json::from_slice(&bytes[..header_end]).expect("raw chunk JSON header");

        assert_eq!(header["topic"], "asset/raw-chunk");
        assert_eq!(header["payload"]["transferId"], "raw-test");
        assert_eq!(header["payload"]["path"], "p4/families/idle.default.h264");
        assert_eq!(header["payload"]["size"], chunk.len());
        assert_eq!(header["payload"]["index"], "7");
        assert_eq!(header["payload"]["checksum"], asset_checksum_hex(&chunk));
        assert!(header["payload"].get("data").is_none());
        assert_eq!(&bytes[(header_end + 1)..], chunk.as_slice());
    }

    #[test]
    fn asset_chunk_ack_requires_the_current_index_when_firmware_provides_one() {
        let manager = UsbSerialManager::new();
        let receiver = manager
            .register_asset_ack_waiter(
                "indexed-asset-test",
                "raw-chunk",
                Some("p4/families/working.typing.h264"),
                Some(2),
            )
            .unwrap();
        let waiters = Arc::clone(&manager.asset_ack_waiters);

        resolve_asset_ack(
            &waiters,
            "asset/ack",
            &serde_json::json!({
                "transferId": "indexed-asset-test",
                "phase": "raw-chunk",
                "path": "p4/families/working.typing.h264",
                "index": "1",
                "ok": true,
            }),
        );
        assert!(matches!(
            receiver.try_recv(),
            Err(mpsc::TryRecvError::Empty)
        ));

        resolve_asset_ack(
            &waiters,
            "asset/ack",
            &serde_json::json!({
                "transferId": "indexed-asset-test",
                "phase": "raw-chunk",
                "path": "p4/families/working.typing.h264",
                "index": "2",
                "ok": true,
            }),
        );
        assert_eq!(
            receiver.recv_timeout(Duration::from_millis(50)).unwrap()["index"],
            "2"
        );
    }

    #[test]
    fn hello_payload_records_p4_runtime_capabilities() {
        let mut conn = UsbConnection {
            connection_id: 1,
            port_name: "COM15".to_string(),
            baud_rate: P4_USB_UART_BAUD,
            writer: Box::new(Vec::<u8>::new()),
            board_device_id: String::new(),
            runtime: String::new(),
            device_model: String::new(),
            firmware: String::new(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 0,
            wire_protocol: String::new(),
            capabilities: serde_json::Value::Null,
            connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)),
        };
        let payload = serde_json::json!({
            "boardDeviceId": "p4-devkit-001",
            "runtime": "esp-p4",
            "deviceModel": "ESP32-P4 + ESP32-C6",
            "fw": "0.1.0-p4",
            "buildId": "0.1.0-p4+abc123-dirty",
            "gitSha": "abc123",
            "buildDirty": true,
            "protocolSchema": 4,
            "wireProtocol": "pet-usb-jsonl-v2",
            "capabilities": {
                "usbOnly": true,
                "display": {"width": 640, "height": 480},
                "assetFormats": ["p4-mjpeg-v1"]
            }
        });

        apply_hello_payload_to_connection(&mut conn, &payload);

        assert_eq!(conn.board_device_id, "p4-devkit-001");
        assert_eq!(conn.runtime, "esp-p4");
        assert_eq!(conn.device_model, "ESP32-P4 + ESP32-C6");
        assert_eq!(conn.firmware, "0.1.0-p4");
        assert_eq!(conn.build_id, "0.1.0-p4+abc123-dirty");
        assert_eq!(conn.git_sha, "abc123");
        assert!(conn.build_dirty);
        assert_eq!(conn.protocol_schema, 4);
        assert_eq!(conn.wire_protocol, "pet-usb-jsonl-v2");
        assert_eq!(conn.capabilities["usbOnly"], true);
        assert_eq!(conn.capabilities["display"]["width"], 640);
    }

    #[test]
    fn partial_hello_does_not_erase_verified_runtime_metadata() {
        let mut conn = UsbConnection {
            connection_id: 1,
            port_name: "COM3".to_string(),
            baud_rate: P4_USB_UART_BAUD,
            writer: Box::new(Vec::<u8>::new()),
            board_device_id: "old-board".to_string(),
            runtime: "esp-p4".to_string(),
            device_model: "ESP32-P4".to_string(),
            firmware: "0.5.0-p4".to_string(),
            build_id: "0.5.0-p4+old".to_string(),
            git_sha: "old".to_string(),
            build_dirty: false,
            protocol_schema: 3,
            wire_protocol: "pet-usb-jsonl-v2".to_string(),
            capabilities: serde_json::json!({ "usbOnly": true }),
            connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)),
        };

        apply_hello_payload_to_connection(
            &mut conn,
            &serde_json::json!({ "boardDeviceId": "new-board" }),
        );

        assert_eq!(conn.board_device_id, "new-board");
        assert_eq!(conn.runtime, "esp-p4");
        assert_eq!(conn.device_model, "ESP32-P4");
        assert_eq!(conn.firmware, "0.5.0-p4");
        assert_eq!(conn.build_id, "0.5.0-p4+old");
        assert_eq!(conn.git_sha, "old");
        assert!(!conn.build_dirty);
        assert_eq!(conn.protocol_schema, 3);
        assert_eq!(conn.wire_protocol, "pet-usb-jsonl-v2");
        assert_eq!(conn.capabilities["usbOnly"], true);
    }

    #[test]
    fn native_full_pack_preflight_rejects_raw_video_but_allows_spiffs_metadata() {
        let manifest = AppearanceAssetEntry {
            family_name: "manifest".to_string(),
            kind: "p4-manifest",
            source_path: PathBuf::from("manifest.json"),
            device_path: "p4/manifest.json".to_string(),
        };
        assert!(ensure_p4_native_full_pack_supported(&[manifest.clone()]).is_ok());

        let video = AppearanceAssetEntry {
            family_name: "idle.default".to_string(),
            kind: "p4-h264",
            source_path: PathBuf::from("idle.default.h264"),
            device_path: "p4/families/idle.default.h264".to_string(),
        };
        let error = ensure_p4_native_full_pack_supported(&[manifest, video]).unwrap_err();
        assert!(error.contains("不能完整写入"));
        assert!(error.contains("USB-UART"));
    }

    #[test]
    fn p4_manifest_maps_ok_families_to_h264_streams() {
        let specs = vec![P4FrameSpec {
            family_name: "idle.default".to_string(),
            device_path: "p4/families/idle.default.h264".to_string(),
            frames: 2,
            stream_bytes: 6912,
            fps: 2,
            frame_duration_ms: 480,
            duration_ms: 960,
            audio_device_path: Some("p4/audio/idle.default.wav".to_string()),
        }];

        let manifest = build_p4_h264_manifest(&specs, 640, 480, 10, "test-pack");

        assert_eq!(manifest["format"], "p4-h264-v1");
        assert_eq!(manifest["packId"], "test-pack");
        assert_eq!(manifest["codec"], "h264");
        assert_eq!(manifest["container"], "annex-b");
        assert_eq!(manifest["width"], 640);
        assert_eq!(manifest["height"], 480);
        assert_eq!(manifest["fps"], 10);
        assert_eq!(manifest["families"].as_array().unwrap().len(), 1);
        assert_eq!(manifest["families"][0]["family"], "idle.default");
        assert_eq!(manifest["families"][0]["frames"], 2);
        assert_eq!(manifest["families"][0]["fps"], 2);
        assert_eq!(manifest["families"][0]["frameDurationMs"], 480);
        assert_eq!(manifest["families"][0]["durationMs"], 960);
        assert_eq!(manifest["families"][0]["streamBytes"], 6912);
        assert_eq!(
            manifest["families"][0]["path"],
            "p4/families/idle.default.h264"
        );
        assert_eq!(
            manifest["families"][0]["audioPath"],
            "p4/audio/idle.default.wav"
        );
    }

    #[test]
    fn p4_family_file_stem_replaces_path_unsafe_chars() {
        assert_eq!(
            p4_safe_family_file_stem("working/../../bad name"),
            "working_.._.._bad_name"
        );
    }

    #[test]
    fn p4_ffmpeg_args_export_h264_stream_with_letterbox() {
        let args = build_p4_h264_ffmpeg_args(
            Path::new("input.mp4"),
            Path::new("out.h264"),
            P4_APPEARANCE_WIDTH,
            P4_APPEARANCE_HEIGHT,
            f64::from(P4_APPEARANCE_FPS),
            P4_APPEARANCE_MAX_FRAMES,
        );
        let joined = args.join(" ");

        assert!(joined.contains("fps=15"));
        assert!(joined.contains("scale=640:480:force_original_aspect_ratio=decrease"));
        assert!(joined.contains("pad=640:480:(ow-iw)/2:(oh-ih)/2:black"));
        assert!(joined.contains("-frames:v 225"));
        assert!(joined.contains("-c:v libx264"));
        assert!(joined.contains("-profile:v baseline"));
        assert!(joined.contains("-crf 27"));
        assert!(joined.contains("-pix_fmt yuv420p"));
        assert!(joined.contains("repeat-headers=1:aud=1"));
        assert!(joined.contains("threads=1:sliced-threads=0"));
        assert!(joined.contains("-f h264"));
    }

    #[test]
    fn p4_duration_probe_prefers_container_duration_over_last_packet_timestamp() {
        let stderr = b"Input #0\n  Duration: 00:00:15.13, start: 0.000000, bitrate: 269 kb/s\n";
        assert_eq!(ffmpeg_container_duration_ms(stderr), Some(15_130));
        assert_eq!(ffmpeg_container_duration_ms(b"Duration: N/A"), None);
    }

    #[test]
    fn p4_long_clips_sample_the_full_timeline_without_growing_the_pack() {
        assert_eq!(p4_sampling_fps(1_000), f64::from(P4_APPEARANCE_FPS));
        let full_speed_sampling_fps = p4_sampling_fps(15_000);
        assert_eq!(full_speed_sampling_fps, f64::from(P4_APPEARANCE_FPS));
        let capped_sampling_fps = p4_sampling_fps(20_000);
        assert!((capped_sampling_fps - 11.25).abs() < 0.000_001);

        let exported = p4_exported_stream(15_000, 225, 750_000).unwrap();
        assert_eq!(exported.frames, P4_APPEARANCE_MAX_FRAMES);
        assert_eq!(exported.stream_bytes, 750_000);
        assert_eq!(exported.fps, 15);
        assert_eq!(exported.frame_duration_ms, 67);
        assert_eq!(exported.duration_ms, 15_000);
    }

    #[test]
    fn bundled_terrier_p4_ready_pack_matches_desktop_profile() {
        let appearance_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("public")
            .join("terrier-clips");
        let prepared = inspect_prepared_p4_appearance(&appearance_dir).unwrap();
        assert_eq!(prepared.profile, "v9-640x480-15fps-225f-h264-crf27");
        assert_eq!(prepared.pack_id.len(), 64);
        assert_eq!(prepared.file_count, 20);
        assert!(prepared.byte_count > 5_000_000);
    }

    #[test]
    fn p4_h264_sps_matches_the_proven_esp_decoder_contract() {
        let sps = build_p4_minimal_h264_sps(640, 480).unwrap();
        assert_eq!(
            sps,
            vec![0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02, 0x80, 0xf4, 0x40]
        );

        let original = vec![
            0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02, 0x80, 0xbf, 0xe5, 0x84, 0, 0, 3, 0, 4,
            0, 0, 3, 0, 0x7a, 0x3c, 0x58, 0xba, 0x80, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80, 0, 0, 0,
            1, 0x09, 0xf0, 0, 0, 0, 1, 0x65, 0x88, 0x84,
        ];
        let compatible = rewrite_p4_h264_sps(&original, 640, 480).unwrap();
        let parsed = parse_p4_h264_stream(&compatible, 640, 480).unwrap();
        assert_eq!(parsed.frames, 1);
        assert!(parse_p4_h264_stream(&original, 640, 480).is_err());
        let mut multi_slice = compatible;
        multi_slice.extend_from_slice(&[0, 0, 0, 1, 0x65, 0x88, 0x84]);
        assert!(parse_p4_h264_stream(&multi_slice, 640, 480)
            .unwrap_err()
            .contains("exactly one slice"));
    }

    #[test]
    fn p4_audio_cue_requires_board_pcm_format() {
        assert!(validate_p4_audio_wav(&p4_test_wav()).is_ok());
        let mut stereo = p4_test_wav();
        stereo[22..24].copy_from_slice(&2u16.to_le_bytes());
        assert!(validate_p4_audio_wav(&stereo).is_err());
    }

    #[test]
    fn p4_audio_ffmpeg_args_normalize_legacy_cues() {
        let args = build_p4_audio_ffmpeg_args(
            Path::new("legacy-48k-stereo.wav"),
            Path::new("p4-16k-mono.wav"),
        );
        let joined = args.join(" ");

        assert!(joined.contains("-map 0:a:0"));
        assert!(joined.contains("-vn"));
        assert!(joined.contains("-ac 1"));
        assert!(joined.contains("-ar 16000"));
        assert!(joined.contains("-c:a pcm_s16le"));
        assert!(joined.contains("-f wav"));
    }

    #[test]
    fn p4_asset_chunk_size_fits_firmware_json_line_buffer() {
        let encoded_len = P4_APPEARANCE_ASSET_CHUNK_SIZE.div_ceil(3) * 4;

        assert_eq!(P4_APPEARANCE_ASSET_CHUNK_SIZE % 3, 0);
        assert!(
            encoded_len + 1024 < 32 * 1024,
            "P4 CDC JSON line buffer is 32KB; keep encoded asset chunks below it"
        );
        assert_eq!(usb_uart_wire_bytes_per_sec(P4_USB_UART_BAUD), 400_000);
        assert!(p4_appearance_asset_chunk_wire_time(encoded_len) < Duration::from_millis(100));
        assert_eq!(P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE, 8_192);
        assert!(
            Duration::from_millis(
                (P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE as u64 * 1000)
                    .div_ceil(usb_uart_wire_bytes_per_sec(P4_USB_UART_BAUD))
            ) < Duration::from_millis(25)
        );
    }

    #[test]
    fn p4_asset_pack_includes_manifest_and_h264_streams() {
        let tmp = tempfile::tempdir().unwrap();
        let staging = tmp.path().join("p4-stage");
        std::fs::create_dir_all(staging.join("p4/families")).unwrap();
        let frame_file = staging.join("p4/families/idle.default.h264");
        std::fs::write(&frame_file, b"fake-h264").unwrap();

        let specs = vec![P4FrameSpec {
            family_name: "idle.default".to_string(),
            device_path: "p4/families/idle.default.h264".to_string(),
            frames: 2,
            stream_bytes: 9,
            fps: 4,
            frame_duration_ms: 250,
            duration_ms: 500,
            audio_device_path: None,
        }];

        let assets = collect_p4_asset_pack(&staging, &specs, 640, 480, 10).unwrap();

        assert_eq!(assets.len(), 2);
        assert_eq!(assets[0].kind, "p4-manifest");
        assert_eq!(assets[0].device_path, "p4/manifest.json");
        assert!(assets[0].source_path.exists());
        assert_eq!(assets[1].kind, "p4-h264");
        assert_eq!(assets[1].family_name, "idle.default");
        assert_eq!(assets[1].device_path, "p4/families/idle.default.h264");

        let manifest: serde_json::Value =
            serde_json::from_str(&std::fs::read_to_string(&assets[0].source_path).unwrap())
                .unwrap();
        assert_eq!(manifest["format"], "p4-h264-v1");
        assert_eq!(manifest["packId"].as_str().unwrap().len(), 64);
        assert_eq!(
            manifest["packId"].as_str().unwrap(),
            p4_pack_id_from_assets(&assets).unwrap()
        );
        assert_eq!(manifest["families"][0]["frames"], 2);
    }

    #[test]
    fn p4_pack_id_is_stable_and_changes_with_payload_bytes() {
        let tmp = tempfile::tempdir().unwrap();
        let first_path = tmp.path().join("first.h264");
        let second_path = tmp.path().join("second.wav");
        std::fs::write(&first_path, b"first").unwrap();
        std::fs::write(&second_path, b"second").unwrap();
        let assets = vec![
            AppearanceAssetEntry {
                family_name: "audio".to_string(),
                kind: "p4-audio",
                source_path: second_path.clone(),
                device_path: "p4/audio/done.wav".to_string(),
            },
            AppearanceAssetEntry {
                family_name: "video".to_string(),
                kind: "p4-h264",
                source_path: first_path.clone(),
                device_path: "p4/families/idle.h264".to_string(),
            },
        ];
        let first = compute_p4_pack_id(&assets, b"manifest-a").unwrap();
        let reversed = compute_p4_pack_id(
            &assets.iter().cloned().rev().collect::<Vec<_>>(),
            b"manifest-a",
        )
        .unwrap();
        assert_eq!(first, reversed);
        assert_eq!(first.len(), 64);
        assert_ne!(first, compute_p4_pack_id(&assets, b"manifest-b").unwrap());

        std::fs::write(&first_path, b"changed").unwrap();
        assert_ne!(first, compute_p4_pack_id(&assets, b"manifest-a").unwrap());
    }

    #[test]
    fn p4_slot_query_parses_only_valid_pack_ids() {
        let state = parse_p4_appearance_slot_state(&serde_json::json!({
            "activeSlot": 1,
            "slots": [
                {"slot": 0, "valid": true, "packId": "pack-a"},
                {"slot": 1, "valid": true, "packId": "pack-b"},
                {"slot": 2, "valid": false}
            ]
        }))
        .unwrap();
        assert_eq!(state.active_slot, Some(1));
        assert_eq!(
            state.slots,
            vec![
                P4AppearanceSlot {
                    slot: 0,
                    pack_id: "pack-a".to_string(),
                },
                P4AppearanceSlot {
                    slot: 1,
                    pack_id: "pack-b".to_string(),
                },
            ]
        );
    }

    #[test]
    fn p4_raw_transfer_uses_valid_slot_zero_only_when_slot_one_is_active() {
        let raw_active = P4AppearanceSlotState {
            active_slot: Some(P4_RAW_APPEARANCE_SLOT),
            slots: vec![
                P4AppearanceSlot {
                    slot: P4_BUILTIN_APPEARANCE_SLOT,
                    pack_id: "builtin-pack".to_string(),
                },
                P4AppearanceSlot {
                    slot: P4_RAW_APPEARANCE_SLOT,
                    pack_id: "current-pack".to_string(),
                },
            ],
        };
        let fallback = p4_raw_transfer_fallback_slot(&raw_active).unwrap();
        assert_eq!(fallback.slot, P4_BUILTIN_APPEARANCE_SLOT);
        assert_eq!(fallback.pack_id, "builtin-pack");

        let builtin_active = P4AppearanceSlotState {
            active_slot: Some(P4_BUILTIN_APPEARANCE_SLOT),
            slots: raw_active.slots.clone(),
        };
        assert!(p4_raw_transfer_fallback_slot(&builtin_active).is_none());

        let missing_builtin = P4AppearanceSlotState {
            active_slot: Some(P4_RAW_APPEARANCE_SLOT),
            slots: vec![raw_active.slots[1].clone()],
        };
        assert!(p4_raw_transfer_fallback_slot(&missing_builtin).is_none());
    }

    #[test]
    fn p4_stage_exports_ok_families_into_asset_pack() {
        let tmp = tempfile::tempdir().unwrap();
        let appearance_dir = tmp.path().join("appearance");
        let app_data_dir = tmp.path().join("app-data");
        let staging = tmp.path().join("stage");
        std::fs::create_dir_all(&appearance_dir).unwrap();
        std::fs::create_dir_all(&app_data_dir).unwrap();
        std::fs::write(appearance_dir.join("idle.default.mp4"), b"fake mp4").unwrap();
        std::fs::write(appearance_dir.join("idle.default.wav"), p4_test_wav()).unwrap();

        let families = vec![
            serde_json::json!({"family": "idle.default", "ok": true}),
            serde_json::json!({"family": "broken", "ok": false}),
        ];

        let assets = stage_p4_appearance_pack_with_exporter(
            &families,
            &appearance_dir,
            &app_data_dir,
            &staging,
            |input, output| {
                assert_eq!(input, appearance_dir.join("idle.default.mp4"));
                std::fs::write(output, b"h264-stream").unwrap();
                Ok(P4ExportedStream {
                    frames: 3,
                    stream_bytes: 11,
                    fps: 6,
                    frame_duration_ms: 167,
                    duration_ms: 500,
                })
            },
        )
        .unwrap();

        assert_eq!(assets.len(), 3);
        assert_eq!(assets[0].device_path, "p4/manifest.json");
        assert!(assets[1].device_path.starts_with("p4/families/sha256-"));
        assert_eq!(assets[2].kind, "p4-audio");
        assert_eq!(assets[2].device_path, "p4/audio/idle.default.wav");
        let manifest: serde_json::Value =
            serde_json::from_str(&std::fs::read_to_string(&assets[0].source_path).unwrap())
                .unwrap();
        assert_eq!(manifest["fps"], P4_APPEARANCE_FPS);
        assert_eq!(manifest["families"].as_array().unwrap().len(), 1);
        assert_eq!(manifest["families"][0]["family"], "idle.default");
        assert_eq!(manifest["families"][0]["frames"], 3);
        assert_eq!(manifest["families"][0]["path"], assets[1].device_path);
        assert_eq!(
            manifest["families"][0]["audioPath"],
            "p4/audio/idle.default.wav"
        );
    }

    #[test]
    fn p4_stage_deduplicates_identical_video_content() {
        let tmp = tempfile::tempdir().unwrap();
        let appearance_dir = tmp.path().join("appearance");
        let app_data_dir = tmp.path().join("app-data");
        let staging = tmp.path().join("stage");
        std::fs::create_dir_all(&appearance_dir).unwrap();
        std::fs::create_dir_all(&app_data_dir).unwrap();
        std::fs::write(appearance_dir.join("idle.default.mp4"), b"same fake mp4").unwrap();
        std::fs::write(appearance_dir.join("working.mp4"), b"same fake mp4").unwrap();

        let families = vec![
            serde_json::json!({"family": "idle.default", "ok": true}),
            serde_json::json!({"family": "working", "ok": true}),
        ];
        let export_count = std::cell::Cell::new(0u32);

        let assets = stage_p4_appearance_pack_with_exporter(
            &families,
            &appearance_dir,
            &app_data_dir,
            &staging,
            |_, output| {
                export_count.set(export_count.get() + 1);
                std::fs::write(output, b"h264").unwrap();
                Ok(P4ExportedStream {
                    frames: 1,
                    stream_bytes: 4,
                    fps: 5,
                    frame_duration_ms: 200,
                    duration_ms: 200,
                })
            },
        )
        .unwrap();

        assert_eq!(export_count.get(), 1);
        assert_eq!(assets.len(), 2);
        assert_eq!(assets[0].device_path, "p4/manifest.json");
        assert!(assets[1].device_path.starts_with("p4/families/sha256-"));

        let manifest: serde_json::Value =
            serde_json::from_str(&std::fs::read_to_string(&assets[0].source_path).unwrap())
                .unwrap();
        assert_eq!(manifest["families"].as_array().unwrap().len(), 2);
        assert_eq!(
            manifest["families"][0]["path"],
            manifest["families"][1]["path"]
        );
        assert_eq!(
            manifest["families"][0]["streamBytes"],
            manifest["families"][1]["streamBytes"]
        );
    }

    #[test]
    fn asset_chunk_delay_tracks_serial_wire_time() {
        let delay = appearance_asset_chunk_delay(65_536);
        let expected_ms = (65_536u64 * 1000)
            .div_ceil(usb_uart_wire_bytes_per_sec(DEFAULT_USB_SERIAL_BAUD))
            + APPEARANCE_ASSET_CHUNK_DELAY_MARGIN_MS;

        assert_eq!(delay, Duration::from_millis(expected_ms));
    }

    #[test]
    fn asset_file_checksum_is_stable_for_transfer_integrity() {
        assert_eq!(asset_checksum_hex(b"hello"), "a430d84680aabd0b");
    }

    #[test]
    fn widget_chunk_ack_requires_the_exact_file_and_index() {
        let waiters = Arc::new(Mutex::new(Vec::new()));
        let (sender, receiver) = mpsc::channel();
        waiters.lock().unwrap().push(WidgetAckWaiter {
            transfer_id: "widget-1".to_string(),
            phase: "chunk".to_string(),
            path: Some("runtime/widget.json".to_string()),
            index: Some(0),
            sender,
        });

        resolve_widget_ack(
            &waiters,
            "widget-install-ack",
            &serde_json::json!({
                "transferId": "widget-1",
                "phase": "chunk",
                "path": "buttons.json",
                "index": "0",
                "ok": true,
            }),
        );
        assert!(receiver.try_recv().is_err());

        resolve_widget_ack(
            &waiters,
            "widget-install-ack",
            &serde_json::json!({
                "transferId": "widget-1",
                "phase": "chunk",
                "path": "runtime/widget.json",
                "index": "0",
                "ok": true,
            }),
        );
        assert_eq!(receiver.recv().unwrap()["ok"], true);
    }

    #[test]
    fn legacy_unknown_phase_widget_nack_reaches_the_delete_waiter() {
        let waiters = Arc::new(Mutex::new(Vec::new()));
        let (sender, receiver) = mpsc::channel();
        waiters.lock().unwrap().push(WidgetAckWaiter {
            transfer_id: "widget-delete-legacy".to_string(),
            phase: "delete".to_string(),
            path: None,
            index: None,
            sender,
        });

        resolve_widget_ack(
            &waiters,
            "widget-install-ack",
            &serde_json::json!({
                "transferId": "widget-delete-legacy",
                "phase": "unknown",
                "ok": false,
                "msg": "unsupported widget phase",
            }),
        );

        let ack = receiver.recv().unwrap();
        assert_eq!(ack["ok"], false);
        assert_eq!(ack["msg"], "unsupported widget phase");
        assert!(waiters.lock().unwrap().is_empty());
    }

    #[test]
    fn p4_widget_delete_requires_the_advertised_capability() {
        let status = |runtime: &str, capabilities: serde_json::Value| UsbConnectionStatus {
            connected: true,
            port_name: "test-port".to_string(),
            baud_rate: P4_USB_UART_BAUD,
            board_device_id: "board-test".to_string(),
            transport: "usb".to_string(),
            runtime: runtime.to_string(),
            device_model: "test-device".to_string(),
            firmware: "0.6.1-p4".to_string(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 0,
            wire_protocol: "pet-usb-jsonl-v2".to_string(),
            capabilities,
        };

        let old_p4 = status("esp-p4", serde_json::Value::Null);
        let error = ensure_widget_delete_supported(&old_p4).unwrap_err();
        assert!(error.contains("0.6.1-p4"));
        assert!(error.contains("升级板端固件"));

        assert!(ensure_widget_delete_supported(&status(
            "esp-p4",
            serde_json::json!({ "widgetDelete": true }),
        ))
        .is_ok());
        assert!(ensure_widget_delete_supported(&status(
            "esp-p4",
            serde_json::json!({ "features": { "widgetDelete": true } }),
        ))
        .is_ok());
        assert!(ensure_widget_delete_supported(&status("linux", serde_json::Value::Null)).is_ok());
    }

    #[test]
    fn p4_widget_inventory_requires_the_advertised_capability() {
        let status = |runtime: &str, capabilities: serde_json::Value| UsbConnectionStatus {
            connected: true,
            port_name: "test-port".to_string(),
            baud_rate: P4_USB_UART_BAUD,
            board_device_id: "board-test".to_string(),
            transport: "usb".to_string(),
            runtime: runtime.to_string(),
            device_model: "test-device".to_string(),
            firmware: "0.6.1-p4".to_string(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 0,
            wire_protocol: "pet-usb-jsonl-v2".to_string(),
            capabilities,
        };

        let error = ensure_widget_inventory_supported(&status("esp-p4", serde_json::Value::Null))
            .unwrap_err();
        assert!(error.contains("组件清单能力"));
        assert!(error.contains("升级板端固件"));
        assert!(ensure_widget_inventory_supported(&status(
            "esp-p4",
            serde_json::json!({ "widgetInventory": true }),
        ))
        .is_ok());
        assert!(ensure_widget_inventory_supported(&status(
            "esp-p4",
            serde_json::json!({ "features": { "widgetInventory": true } }),
        ))
        .is_ok());
        assert!(
            ensure_widget_inventory_supported(&status("linux", serde_json::Value::Null)).is_ok()
        );
    }

    #[test]
    fn firmware_ack_requires_transfer_phase_and_exact_progress() {
        let waiters = Arc::new(Mutex::new(Vec::new()));
        let (sender, receiver) = mpsc::channel();
        waiters.lock().unwrap().push(FirmwareAckWaiter {
            transfer_id: "firmware-1".to_string(),
            phase: "chunk".to_string(),
            expected_next_sequence: 2,
            expected_received_bytes: 8192,
            sender,
        });

        resolve_firmware_ack(
            &waiters,
            "firmware/ack",
            &serde_json::json!({
                "transferId": "firmware-1",
                "phase": "begin",
                "ok": true,
                "nextSequence": 2,
                "receivedBytes": 8192,
            }),
        );
        assert!(receiver.try_recv().is_err());

        // A duplicate ACK for the previous block must not satisfy block 2.
        resolve_firmware_ack(
            &waiters,
            "firmware/ack",
            &serde_json::json!({
                "transferId": "firmware-1",
                "phase": "chunk",
                "ok": true,
                "nextSequence": 1,
                "receivedBytes": 4096,
            }),
        );
        assert!(receiver.try_recv().is_err());

        // Both progress counters are part of the waiter identity.
        resolve_firmware_ack(
            &waiters,
            "firmware/ack",
            &serde_json::json!({
                "transferId": "firmware-1",
                "phase": "chunk",
                "ok": true,
                "nextSequence": 2,
                "receivedBytes": 4096,
            }),
        );
        assert!(receiver.try_recv().is_err());

        resolve_firmware_ack(
            &waiters,
            "firmware/ack",
            &serde_json::json!({
                "transferId": "firmware-1",
                "phase": "chunk",
                "ok": true,
                "nextSequence": 2,
                "receivedBytes": 8192,
            }),
        );

        let ack = receiver.recv_timeout(Duration::from_millis(50)).unwrap();
        assert_eq!(ack["ok"], true);
        assert_eq!(ack["nextSequence"], 2);
        assert_eq!(ack["receivedBytes"], 8192);
    }

    #[test]
    fn firmware_chunk_nack_matches_the_requested_sequence_without_advanced_progress() {
        let waiters = Arc::new(Mutex::new(Vec::new()));
        let (sender, receiver) = mpsc::channel();
        waiters.lock().unwrap().push(FirmwareAckWaiter {
            transfer_id: "firmware-2".to_string(),
            phase: "chunk".to_string(),
            expected_next_sequence: 3,
            expected_received_bytes: 12_288,
            sender,
        });

        resolve_firmware_ack(
            &waiters,
            "firmware/ack",
            &serde_json::json!({
                "transferId": "firmware-2",
                "phase": "chunk",
                "ok": false,
                "seq": 2,
                "nextSequence": 2,
                "receivedBytes": 8192,
                "error": "invalid firmware chunk size",
            }),
        );

        let ack = receiver.recv_timeout(Duration::from_millis(50)).unwrap();
        assert_eq!(ack["ok"], false);
        assert_eq!(ack["seq"], 2);
    }

    fn app_image_fixture(project_name: &str, version: &str) -> Vec<u8> {
        let descriptor_offset = ESP_IMAGE_HEADER_SIZE + ESP_IMAGE_SEGMENT_HEADER_SIZE;
        let mut image = vec![0u8; descriptor_offset + ESP_APP_DESC_SIZE];
        image[0] = 0xE9;
        image[ESP_IMAGE_HEADER_SIZE + 4..ESP_IMAGE_HEADER_SIZE + 8]
            .copy_from_slice(&(ESP_APP_DESC_SIZE as u32).to_le_bytes());
        image[descriptor_offset..descriptor_offset + 4]
            .copy_from_slice(&ESP_APP_DESC_MAGIC.to_le_bytes());
        image[descriptor_offset + 16..descriptor_offset + 16 + version.len()]
            .copy_from_slice(version.as_bytes());
        image[descriptor_offset + 48..descriptor_offset + 48 + project_name.len()]
            .copy_from_slice(project_name.as_bytes());
        image
    }

    #[test]
    fn esp_idf_descriptor_preflight_extracts_expected_runtime_version() {
        let image = app_image_fixture(P4_FIRMWARE_PROJECT_NAME, "1.7.3-p4");

        let descriptor = parse_esp_idf_app_descriptor(&image).unwrap();

        assert_eq!(descriptor.project_name, P4_FIRMWARE_PROJECT_NAME);
        assert_eq!(descriptor.version, "1.7.3-p4");
    }

    #[test]
    fn esp_idf_descriptor_preflight_rejects_another_project() {
        let image = app_image_fixture("unrelated_firmware", "9.9.9");

        let error = parse_esp_idf_app_descriptor(&image).unwrap_err();

        assert!(error.contains("projectName must be pet_manager_p4_runtime"));
    }

    #[test]
    fn board_targeted_send_rejects_blank_or_changed_identity_under_lock() {
        let manager = UsbSerialManager::new();
        *manager.connection.lock().unwrap() = Some(UsbConnection {
            connection_id: 1,
            port_name: "COM15".to_string(),
            baud_rate: P4_USB_UART_BAUD,
            writer: Box::new(Vec::<u8>::new()),
            board_device_id: "p4-board-a".to_string(),
            runtime: "esp-p4".to_string(),
            device_model: "ESP32-P4".to_string(),
            firmware: "1.0.0".to_string(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 0,
            wire_protocol: "pet-usb-jsonl-v2".to_string(),
            capabilities: serde_json::Value::Null,
            connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)),
        });

        assert!(manager
            .send_to_board("", "diagnostics/query", &serde_json::json!({}))
            .unwrap_err()
            .contains("expectedBoardDeviceId is required"));
        assert!(manager
            .send_to_board("p4-board-b", "diagnostics/query", &serde_json::json!({}))
            .unwrap_err()
            .contains("connected USB board changed"));
        manager
            .send_to_board("p4-board-a", "diagnostics/query", &serde_json::json!({}))
            .unwrap();
    }

    #[test]
    fn firmware_validation_waits_for_new_boot_and_valid_image() {
        let pending_old_boot = serde_json::json!({
            "bootCount": 4,
            "runtime": {
                "firmware": "2.0.0",
                "runningPartition": "ota_1",
                "imageState": "valid",
            }
        });
        assert_eq!(
            evaluate_firmware_validation(&pending_old_boot, "2.0.0", "ota_1", "1.0.0", "ota_0", 4,)
                .unwrap(),
            None
        );

        let pending_verify = serde_json::json!({
            "bootCount": 5,
            "runtime": {
                "firmware": "2.0.0",
                "runningPartition": "ota_1",
                "imageState": "pending_verify",
            }
        });
        assert_eq!(
            evaluate_firmware_validation(&pending_verify, "2.0.0", "ota_1", "1.0.0", "ota_0", 4,)
                .unwrap(),
            None
        );

        let valid = serde_json::json!({
            "bootCount": 5,
            "runtime": {
                "firmware": "2.0.0",
                "runningPartition": "ota_1",
                "imageState": "valid",
            }
        });
        assert_eq!(
            evaluate_firmware_validation(&valid, "2.0.0", "ota_1", "1.0.0", "ota_0", 4,).unwrap(),
            Some(VerifiedFirmware {
                partition: "ota_1".to_string(),
                image_state: "valid".to_string(),
            })
        );
    }

    #[test]
    fn firmware_validation_reports_rollback_to_old_version() {
        let rolled_back = serde_json::json!({
            "bootCount": 6,
            "runtime": {
                "firmware": "1.0.0",
                "runningPartition": "ota_0",
                "imageState": "valid",
            }
        });

        let error =
            evaluate_firmware_validation(&rolled_back, "2.0.0", "ota_1", "1.0.0", "ota_0", 4)
                .unwrap_err();

        assert!(error.contains("rolled back"));
        assert!(error.contains("old version 1.0.0"));
    }

    #[test]
    fn firmware_commit_ack_retry_is_short_and_bounded() {
        assert_eq!(P4_FIRMWARE_COMMIT_ACK_TIMEOUT, Duration::from_secs(3));
        assert_eq!(P4_FIRMWARE_COMMIT_MAX_ATTEMPTS, 3);
        assert!(P4_FIRMWARE_COMMIT_ACK_TIMEOUT < P4_FIRMWARE_RECONNECT_TIMEOUT);
    }

    #[test]
    fn firmware_image_limit_matches_the_current_ota_partition() {
        assert_eq!(P4_FIRMWARE_MAX_IMAGE_SIZE, 0x280000);
    }

    #[test]
    fn legacy_firmware_chunk_json_stays_below_four_kibibytes() {
        let encoded_len = P4_FIRMWARE_CHUNK_SIZE.div_ceil(3) * 4;
        let payload = serde_json::json!({
            "topic": "firmware/chunk",
            "payload": {
                "transferId": "firmware-00000000-0000-0000-0000-000000000000",
                "seq": 9999,
                "decodedSize": P4_FIRMWARE_CHUNK_SIZE,
                "data": "A".repeat(encoded_len),
            },
        });
        let line = serde_json::to_string(&payload).unwrap();

        assert_eq!(P4_FIRMWARE_CHUNK_SIZE % 3, 0);
        assert!(
            line.len() < 4 * 1024,
            "firmware Base64 JSON must fit the legacy P4 line buffer"
        );
    }

    #[test]
    fn advertised_schema_five_receiver_uses_the_full_decoded_chunk_budget() {
        let capabilities = serde_json::json!({
            "firmwareUpdate": { "chunkBytes": 4096 }
        });
        assert_eq!(
            preferred_firmware_chunk_size(5, &capabilities),
            P4_FIRMWARE_FAST_CHUNK_SIZE
        );
        assert_eq!(
            preferred_firmware_chunk_size(4, &capabilities),
            P4_FIRMWARE_CHUNK_SIZE
        );
        assert_eq!(
            preferred_firmware_chunk_size(
                5,
                &serde_json::json!({ "firmwareUpdate": { "chunkBytes": 2048 } }),
            ),
            P4_FIRMWARE_CHUNK_SIZE
        );

        let encoded_len = P4_FIRMWARE_FAST_CHUNK_SIZE.div_ceil(3) * 4;
        let payload = serde_json::json!({
            "topic": "firmware/chunk",
            "payload": {
                "transferId": "firmware-00000000-0000-0000-0000-000000000000",
                "seq": 9999,
                "decodedSize": P4_FIRMWARE_FAST_CHUNK_SIZE,
                "data": "A".repeat(encoded_len),
            },
        });
        let line = serde_json::to_string(&payload).unwrap();
        assert_eq!(P4_FIRMWARE_FAST_CHUNK_SIZE % 3, 0);
        assert!(line.len() < 8 * 1024);
    }

    #[test]
    fn firmware_base64_corruption_downshifts_without_restarting_the_transfer() {
        let corruption =
            FirmwareCommandError::Rejected("firmware chunk base64 mismatch".to_string());
        assert_eq!(
            firmware_corruption_fallback_size(P4_FIRMWARE_FAST_CHUNK_SIZE, &corruption),
            Some(P4_FIRMWARE_CHUNK_SIZE)
        );
        assert_eq!(
            firmware_corruption_fallback_size(P4_FIRMWARE_CHUNK_SIZE, &corruption),
            Some(P4_FIRMWARE_FALLBACK_CHUNK_SIZE)
        );
        assert_eq!(
            firmware_corruption_fallback_size(P4_FIRMWARE_FALLBACK_CHUNK_SIZE, &corruption),
            Some(P4_FIRMWARE_SAFE_CHUNK_SIZE)
        );
        assert_eq!(
            firmware_corruption_fallback_size(P4_FIRMWARE_SAFE_CHUNK_SIZE, &corruption),
            None
        );
        assert_eq!(
            firmware_corruption_fallback_size(
                P4_FIRMWARE_CHUNK_SIZE,
                &FirmwareCommandError::Rejected("firmware transferId mismatch".to_string()),
            ),
            None
        );
        assert_eq!(P4_FIRMWARE_FAST_CHUNK_SIZE % 3, 0);
        assert_eq!(P4_FIRMWARE_CHUNK_SIZE % 3, 0);
        assert_eq!(P4_FIRMWARE_FALLBACK_CHUNK_SIZE % 3, 0);
        assert_eq!(P4_FIRMWARE_SAFE_CHUNK_SIZE % 3, 0);
    }

    #[test]
    fn firmware_chunk_size_recovers_only_after_a_stable_success_streak() {
        assert_eq!(
            firmware_recovery_chunk_size(
                P4_FIRMWARE_SAFE_CHUNK_SIZE,
                P4_FIRMWARE_FAST_CHUNK_SIZE,
                P4_FIRMWARE_RECOVERY_SUCCESS_STREAK - 1,
            ),
            None
        );
        assert_eq!(
            firmware_recovery_chunk_size(
                P4_FIRMWARE_SAFE_CHUNK_SIZE,
                P4_FIRMWARE_FAST_CHUNK_SIZE,
                P4_FIRMWARE_RECOVERY_SUCCESS_STREAK,
            ),
            Some(P4_FIRMWARE_FALLBACK_CHUNK_SIZE)
        );
        assert_eq!(
            firmware_recovery_chunk_size(
                P4_FIRMWARE_FALLBACK_CHUNK_SIZE,
                P4_FIRMWARE_FAST_CHUNK_SIZE,
                P4_FIRMWARE_RECOVERY_SUCCESS_STREAK,
            ),
            Some(P4_FIRMWARE_CHUNK_SIZE)
        );
        assert_eq!(
            firmware_recovery_chunk_size(
                P4_FIRMWARE_CHUNK_SIZE,
                P4_FIRMWARE_FAST_CHUNK_SIZE,
                P4_FIRMWARE_RECOVERY_SUCCESS_STREAK,
            ),
            Some(P4_FIRMWARE_FAST_CHUNK_SIZE)
        );
    }

    #[test]
    fn firmware_commands_are_flushed_in_short_serial_slices() {
        let manager = UsbSerialManager::new();
        let recorded = Arc::new(Mutex::new(PacedWriterState::default()));
        *manager.connection.lock().unwrap() = Some(UsbConnection {
            connection_id: 1,
            port_name: "COM15".to_string(),
            baud_rate: P4_USB_UART_BAUD,
            writer: Box::new(PacedWriter(Arc::clone(&recorded))),
            board_device_id: "p4-board-a".to_string(),
            runtime: "esp-p4".to_string(),
            device_model: "ESP32-P4".to_string(),
            firmware: "1.0.0".to_string(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 0,
            wire_protocol: "pet-usb-jsonl-v2".to_string(),
            capabilities: serde_json::Value::Null,
            connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)),
        });
        let payload = serde_json::json!({
            "transferId": "firmware-1",
            "seq": 0,
            "decodedSize": P4_FIRMWARE_CHUNK_SIZE,
            "data": "A".repeat(P4_FIRMWARE_CHUNK_SIZE.div_ceil(3) * 4),
        });

        manager
            .send_firmware_to_board("p4-board-a", "firmware/chunk", &payload)
            .unwrap();

        let recorded = recorded.lock().unwrap();
        assert!(recorded.writes.len() > 1);
        assert!(recorded
            .writes
            .iter()
            .all(|write| write.len() <= P4_FIRMWARE_SERIAL_WRITE_SLICE_BYTES));
        assert_eq!(recorded.flushes, recorded.writes.len());
        let line = recorded.writes.concat();
        assert!(line.ends_with(b"\n"));
        let message: serde_json::Value = serde_json::from_slice(&line[..line.len() - 1]).unwrap();
        assert_eq!(message["topic"], "firmware/chunk");
        assert_eq!(message["payload"]["decodedSize"], P4_FIRMWARE_CHUNK_SIZE);
    }

    #[test]
    fn rejected_firmware_begin_attempts_idempotent_abort_on_same_board() {
        let manager = UsbSerialManager::new();
        let written = Arc::new(Mutex::new(Vec::new()));
        *manager.connection.lock().unwrap() = Some(UsbConnection {
            connection_id: 1,
            port_name: "COM15".to_string(),
            baud_rate: P4_USB_UART_BAUD,
            writer: Box::new(SharedWriter(Arc::clone(&written))),
            board_device_id: "p4-board-a".to_string(),
            runtime: "esp-p4".to_string(),
            device_model: "ESP32-P4".to_string(),
            firmware: "1.0.0".to_string(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 0,
            wire_protocol: "pet-usb-jsonl-v2".to_string(),
            capabilities: serde_json::Value::Null,
            connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)),
        });

        let waiters = Arc::clone(&manager.firmware_ack_waiters);
        let resolver = thread::spawn(move || {
            for _ in 0..100 {
                if waiters.lock().unwrap().len() == 1 {
                    resolve_firmware_ack(
                        &waiters,
                        "firmware/ack",
                        &serde_json::json!({
                            "transferId": "firmware-test",
                            "phase": "begin",
                            "ok": false,
                            "error": "begin rejected",
                            "nextSequence": 0,
                            "receivedBytes": 0,
                        }),
                    );
                    return;
                }
                thread::sleep(Duration::from_millis(1));
            }
            panic!("firmware begin waiter was not registered");
        });

        let error = manager
            .begin_firmware_transfer(
                "p4-board-a",
                &serde_json::json!({
                    "transferId": "firmware-test",
                    "size": 4096,
                    "sha256": "00".repeat(32),
                }),
                "firmware-test",
            )
            .unwrap_err();
        resolver.join().unwrap();

        let output = String::from_utf8(written.lock().unwrap().clone()).unwrap();
        assert!(error.contains("begin rejected"));
        assert!(output.contains("\"topic\":\"firmware/begin\""));
        assert!(output.contains("\"topic\":\"firmware/abort\""));
        assert!(output.find("firmware/begin") < output.find("firmware/abort"));
    }

    #[test]
    fn device_response_resolves_only_matching_topic_and_request_id() {
        let waiters = Arc::new(Mutex::new(Vec::new()));
        let (sender, receiver) = mpsc::channel();
        waiters.lock().unwrap().push(DeviceResponseWaiter {
            request_id: "diagnostics-1".to_string(),
            response_topic: "diagnostics/status".to_string(),
            sender,
        });

        resolve_device_response(
            &waiters,
            "diagnostics/action",
            &serde_json::json!({"requestId": "diagnostics-1", "ok": true}),
        );
        resolve_device_response(
            &waiters,
            "diagnostics/status",
            &serde_json::json!({"requestId": "diagnostics-2", "ok": true}),
        );
        assert!(receiver.try_recv().is_err());

        resolve_device_response(
            &waiters,
            "diagnostics/status",
            &serde_json::json!({
                "requestId": "diagnostics-1",
                "ok": true,
                "lastResetReason": "software",
            }),
        );
        let response = receiver.recv_timeout(Duration::from_millis(50)).unwrap();
        assert_eq!(response["lastResetReason"], "software");
    }

    #[test]
    #[ignore = "requires a connected ESP32-P4 and P4_FIRMWARE_BIN"]
    fn p4_firmware_ota_hardware_reboots_into_valid_inactive_slot() {
        let port_name = std::env::var("P4_SERIAL_PORT").unwrap_or_else(|_| "COM5".to_string());
        let firmware_path = std::env::var("P4_FIRMWARE_BIN")
            .map(PathBuf::from)
            .expect("P4_FIRMWARE_BIN is required");
        let manager = UsbSerialManager::new();
        manager
            .connect(&port_name, |_topic, _payload| {})
            .expect("connect P4 serial");

        let hello_deadline = std::time::Instant::now() + Duration::from_secs(15);
        while std::time::Instant::now() < hello_deadline {
            if manager.status().runtime.eq_ignore_ascii_case("esp-p4") {
                break;
            }
            thread::sleep(Duration::from_millis(100));
        }
        assert_eq!(manager.status().runtime, "esp-p4");
        let expected_board_device_id = manager.status().board_device_id;
        assert!(!expected_board_device_id.is_empty());
        let reconnect_manager = manager.clone();
        let reconnect_port_name = port_name.clone();
        let reconnect_board_device_id = expected_board_device_id.clone();

        let result = manager
            .update_firmware(
                &firmware_path,
                &expected_board_device_id,
                |sent, total, stage| {
                    if stage != "upload" || sent == total || sent % (64 * 1024) == 0 {
                        eprintln!("[p4-firmware-hil] stage={stage} bytes={sent}/{total}");
                    }
                },
                move || {
                    reconnect_manager.disconnect();
                    let deadline = Instant::now() + Duration::from_secs(20);
                    loop {
                        match reconnect_manager.connect(&reconnect_port_name, |_topic, _payload| {})
                        {
                            Ok(()) => {
                                let hello_deadline = Instant::now() + Duration::from_secs(5);
                                while Instant::now() < hello_deadline {
                                    let status = reconnect_manager.status();
                                    if status.connected
                                        && status.board_device_id == reconnect_board_device_id
                                    {
                                        return Ok(());
                                    }
                                    thread::sleep(Duration::from_millis(100));
                                }
                                reconnect_manager.disconnect();
                            }
                            Err(error) if Instant::now() >= deadline => return Err(error),
                            Err(_) => {}
                        }
                        if Instant::now() >= deadline {
                            return Err("timed out reconnecting HIL board".to_string());
                        }
                        thread::sleep(Duration::from_millis(500));
                    }
                },
            )
            .expect("firmware OTA transfer");
        assert!(!result.pending_reboot);
        assert_eq!(result.image_state, "valid");
        assert!(!result.target_partition.is_empty());
        assert_eq!(
            result.bytes,
            std::fs::metadata(&firmware_path).unwrap().len()
        );
        manager.disconnect();
    }

    #[test]
    #[ignore = "requires a connected ESP32-P4 with appearance slot reuse firmware"]
    fn p4_appearance_hardware_queries_slot_cache() {
        let port_name = std::env::var("P4_SERIAL_PORT").unwrap_or_else(|_| "COM5".to_string());
        let manager = UsbSerialManager::new();
        manager
            .connect(&port_name, |_topic, _payload| {})
            .expect("connect P4 serial");
        assert_eq!(manager.status().runtime, "esp-p4");
        assert!(manager.supports_p4_appearance_slot_reuse());
        manager.best_effort_asset_abort("p4-slot-hil-preflight");
        thread::sleep(Duration::from_millis(250));

        let state = manager
            .query_p4_appearance_slots("p4-slot-hil-query")
            .expect("query P4 appearance slots");
        eprintln!("[p4-slot-hil] state={state:?}");
        let diagnostics = manager
            .query_diagnostics(&manager.status().board_device_id)
            .expect("query P4 storage diagnostics");
        eprintln!("[p4-slot-hil] storage={}", diagnostics["storage"]);
        assert!(state.active_slot.is_none() || state.active_slot.is_some_and(|slot| slot <= 1));
        assert!(state.slots.iter().all(|slot| slot.slot <= 1));
        manager.disconnect();
    }

    #[test]
    #[ignore = "requires a connected ESP32-P4 and P4_APPEARANCE_DIR"]
    fn p4_appearance_hardware_syncs_production_pack_and_samples_performance() {
        let port_name = std::env::var("P4_SERIAL_PORT").unwrap_or_else(|_| "COM5".to_string());
        let appearance_dir = std::env::var("P4_APPEARANCE_DIR")
            .map(PathBuf::from)
            .expect("P4_APPEARANCE_DIR is required");
        let app_data_dir = std::env::var("P4_APP_DATA_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|_| {
                appearance_dir
                    .parent()
                    .and_then(Path::parent)
                    .expect("appearance must live below the app data directory")
                    .to_path_buf()
            });
        let manager = UsbSerialManager::new();
        manager
            .connect(&port_name, |_topic, _payload| {})
            .expect("connect P4 serial");

        let hello_deadline = std::time::Instant::now() + Duration::from_secs(15);
        while std::time::Instant::now() < hello_deadline {
            if manager.status().runtime.eq_ignore_ascii_case("esp-p4") {
                break;
            }
            thread::sleep(Duration::from_millis(100));
        }
        assert_eq!(manager.status().runtime, "esp-p4");
        let board_device_id = manager.status().board_device_id;
        assert!(!board_device_id.is_empty());

        let (file_count, byte_count, reused_slot) = manager
            .sync_appearance_p4(
                &appearance_dir,
                &app_data_dir,
                &board_device_id,
                |files, total, bytes, total_bytes| {
                    if files == total || bytes == total_bytes || bytes % (256 * 1024) < 16 * 1024 {
                        eprintln!(
                            "[p4-appearance-hil] files={files}/{total} bytes={bytes}/{total_bytes}"
                        );
                    }
                },
            )
            .expect("sync production P4 appearance");
        if std::env::var("P4_EXPECT_REUSED_SLOT").as_deref() == Ok("1") {
            assert_eq!(file_count, 0, "cached pack should not resend files");
            assert_eq!(byte_count, 0, "cached pack should not resend bytes");
            eprintln!(
                "[p4-appearance-hil] exact pack skipped; inactive_slot_reactivated={reused_slot}"
            );
        } else {
            assert!(
                file_count > 0,
                "12 FPS pack should differ from the active pack"
            );
            assert!(byte_count > 0);
        }

        let sample_seconds = std::env::var("P4_PERF_SAMPLE_SECS")
            .ok()
            .and_then(|value| value.parse::<u64>().ok())
            .unwrap_or(8);
        eprintln!("[p4-appearance-hil] sampling renderer for {sample_seconds}s");
        thread::sleep(Duration::from_secs(sample_seconds));
        manager.disconnect();
    }

    #[test]
    #[ignore = "requires a connected ESP32-P4 with the production appearance pack"]
    fn p4_render_hardware_samples_working_session_queue() {
        let port_name = std::env::var("P4_SERIAL_PORT").unwrap_or_else(|_| "COM5".to_string());
        let manager = UsbSerialManager::new();
        manager
            .connect(&port_name, |_topic, _payload| {})
            .expect("connect P4 serial");

        let hello_deadline = std::time::Instant::now() + Duration::from_secs(15);
        while std::time::Instant::now() < hello_deadline {
            if manager.status().runtime.eq_ignore_ascii_case("esp-p4") {
                break;
            }
            thread::sleep(Duration::from_millis(100));
        }
        assert_eq!(manager.status().runtime, "esp-p4");
        let board_device_id = manager.status().board_device_id;
        assert!(!board_device_id.is_empty());

        manager
            .send_to_board(
                &board_device_id,
                "session/current",
                &serde_json::json!({
                    "sessionId": "session-b",
                    "title": "优化桌宠播放性能",
                    "index": 2,
                    "count": 3,
                    "sessions": [
                        {
                            "id": "session-a",
                            "title": "同步资源",
                            "content": "准备动画资源",
                            "state": "done"
                        },
                        {
                            "id": "session-b",
                            "title": "优化桌宠播放性能",
                            "content": "硬件 JPEG 与缓存渲染",
                            "state": "working"
                        },
                        {
                            "id": "session-c",
                            "title": "真机验证",
                            "content": "采集帧率与阶段耗时",
                            "state": "idle"
                        }
                    ]
                }),
            )
            .expect("send P4 session queue");
        manager
            .send_to_board(
                &board_device_id,
                "state/openclaw",
                &serde_json::json!({
                    "state": "working",
                    "status": "working",
                    "statusText": "正在优化播放性能",
                    "sessionTitle": "优化桌宠播放性能",
                    "tsMs": 1
                }),
            )
            .expect("send P4 working state");

        let sample_seconds = std::env::var("P4_PERF_SAMPLE_SECS")
            .ok()
            .and_then(|value| value.parse::<u64>().ok())
            .unwrap_or(12);
        eprintln!("[p4-render-hil] sampling working session queue for {sample_seconds}s");
        thread::sleep(Duration::from_secs(sample_seconds));
        manager.disconnect();
    }

    #[test]
    #[ignore = "requires a connected ESP32-P4 with active-ID retention firmware"]
    fn p4_session_retention_hardware_survives_omitted_active_snapshot() {
        let port_name = std::env::var("P4_SERIAL_PORT").unwrap_or_else(|_| "COM5".to_string());
        let manager = UsbSerialManager::new();
        manager
            .connect(&port_name, |_topic, _payload| {})
            .expect("connect P4 serial");
        let board_device_id = manager.status().board_device_id;
        assert!(!board_device_id.is_empty());

        manager
            .send_to_board(
                &board_device_id,
                "session/current",
                &serde_json::json!({
                    "agentId": "codex",
                    "sessionId": "retention-hil",
                    "title": "活动卡片保活验证",
                    "index": 1,
                    "count": 1,
                    "sessions": [{
                        "id": "retention-hil",
                        "title": "活动卡片保活验证",
                        "content": "模拟一次漏采样",
                        "state": "working"
                    }],
                    "activeSessionIds": ["retention-hil"],
                    "displayEnabled": true
                }),
            )
            .expect("send active retention session");
        manager
            .send_to_board(
                &board_device_id,
                "session/current",
                &serde_json::json!({
                    "agentId": "codex",
                    "sessionId": "auto",
                    "title": "",
                    "index": 0,
                    "count": 0,
                    "sessions": [],
                    "activeSessionIds": ["retention-hil"],
                    "displayEnabled": true
                }),
            )
            .expect("omit active session from one queue snapshot");

        thread::sleep(Duration::from_secs(13));

        let retained = manager
            .query_diagnostics(&board_device_id)
            .expect("query active-ID retained session diagnostics");
        assert_eq!(retained["runtime"]["sessionQueueCount"], 1);
        assert_eq!(retained["runtime"]["retainedSessionCount"], 0);
        assert_eq!(retained["runtime"]["currentSessionId"], "auto");

        thread::sleep(Duration::from_secs(19));
        let expired = manager
            .query_diagnostics(&board_device_id)
            .expect("query expired orphan session diagnostics");
        assert_eq!(expired["runtime"]["sessionQueueCount"], 0);
        assert_eq!(expired["runtime"]["retainedSessionCount"], 0);

        manager
            .send_to_board(
                &board_device_id,
                "session/current",
                &serde_json::json!({
                    "agentId": "codex",
                    "sessionId": "auto",
                    "title": "",
                    "index": 0,
                    "count": 0,
                    "sessions": [],
                    "activeSessionIds": [],
                    "displayEnabled": false
                }),
            )
            .expect("clear session display after retention test");
        thread::sleep(Duration::from_millis(150));
        let cleared = manager
            .query_diagnostics(&board_device_id)
            .expect("query cleared session diagnostics");
        assert_eq!(cleared["runtime"]["sessionQueueCount"], 0);
        assert_eq!(cleared["runtime"]["retainedSessionCount"], 0);
        manager.disconnect();
    }

    #[test]
    #[ignore = "requires a connected ESP32-P4 with stable session-order diagnostics"]
    fn p4_session_queue_hardware_preserves_first_seen_order() {
        let port_name = std::env::var("P4_SERIAL_PORT").unwrap_or_else(|_| "COM5".to_string());
        let manager = UsbSerialManager::new();
        manager
            .connect(&port_name, |_topic, _payload| {})
            .expect("connect P4 serial");
        let board_device_id = manager.status().board_device_id;
        assert!(!board_device_id.is_empty());

        manager
            .send_to_board(
                &board_device_id,
                "session/current",
                &serde_json::json!({
                    "agentId": "codex",
                    "sessionId": "order-a",
                    "title": "顺序 A",
                    "index": 1,
                    "count": 3,
                    "sessions": [
                        {"id": "order-a", "title": "顺序 A", "content": "开始", "state": "working"},
                        {"id": "order-b", "title": "顺序 B", "content": "开始", "state": "thinking"},
                        {"id": "order-c", "title": "顺序 C", "content": "开始", "state": "tool_running"}
                    ],
                    "activeSessionIds": ["order-a", "order-b", "order-c"],
                    "displayEnabled": true
                }),
            )
            .expect("send first-seen session order");
        manager
            .send_to_board(
                &board_device_id,
                "session/current",
                &serde_json::json!({
                    "agentId": "codex",
                    "sessionId": "order-c",
                    "title": "顺序 C",
                    "index": 1,
                    "count": 3,
                    "sessions": [
                        {"id": "order-c", "title": "顺序 C", "content": "最新输出", "state": "working"},
                        {"id": "order-a", "title": "顺序 A", "content": "继续工作", "state": "thinking"},
                        {"id": "order-b", "title": "顺序 B", "content": "调用工具", "state": "tool_running"}
                    ],
                    "activeSessionIds": ["order-c", "order-a", "order-b"],
                    "displayEnabled": true
                }),
            )
            .expect("refresh sessions in changing activity order");
        thread::sleep(Duration::from_millis(150));
        let active = manager
            .query_diagnostics(&board_device_id)
            .expect("query stable active session order");
        assert_eq!(
            active["runtime"]["sessionQueueIds"],
            serde_json::json!(["order-a", "order-b", "order-c"])
        );
        assert_eq!(active["runtime"]["currentSessionId"], "order-c");

        manager
            .send_to_board(
                &board_device_id,
                "session/current",
                &serde_json::json!({
                    "agentId": "codex",
                    "sessionId": "order-c",
                    "title": "顺序 C",
                    "index": 1,
                    "count": 2,
                    "sessions": [
                        {"id": "order-c", "title": "顺序 C", "content": "继续", "state": "working"},
                        {"id": "order-a", "title": "顺序 A", "content": "继续", "state": "thinking"}
                    ],
                    "activeSessionIds": ["order-c", "order-a"],
                    "displayEnabled": true
                }),
            )
            .expect("retain completed middle session in place");
        thread::sleep(Duration::from_millis(150));
        let retained = manager
            .query_diagnostics(&board_device_id)
            .expect("query stable retained session order");
        assert_eq!(
            retained["runtime"]["sessionQueueIds"],
            serde_json::json!(["order-a", "order-b", "order-c"])
        );

        manager
            .send_to_board(
                &board_device_id,
                "session/current",
                &serde_json::json!({
                    "agentId": "codex",
                    "sessionId": "auto",
                    "title": "",
                    "index": 0,
                    "count": 0,
                    "sessions": [],
                    "activeSessionIds": [],
                    "displayEnabled": false
                }),
            )
            .expect("clear stable-order test sessions");
        manager.disconnect();
    }

    #[test]
    #[ignore = "requires a connected ESP32-P4 with diagnostics firmware"]
    fn p4_diagnostics_hardware_preserves_assets_across_reset_and_reboot() {
        let port_name = std::env::var("P4_SERIAL_PORT").unwrap_or_else(|_| "COM5".to_string());
        let manager = UsbSerialManager::new();
        manager
            .connect(&port_name, |_topic, _payload| {})
            .expect("connect P4 serial");

        let hello_deadline = std::time::Instant::now() + Duration::from_secs(15);
        while std::time::Instant::now() < hello_deadline {
            if manager.status().runtime.eq_ignore_ascii_case("esp-p4") {
                break;
            }
            thread::sleep(Duration::from_millis(100));
        }
        assert_eq!(manager.status().runtime, "esp-p4");
        let expected_board_device_id = manager.status().board_device_id;
        assert!(!expected_board_device_id.is_empty());

        let before = manager
            .query_diagnostics(&expected_board_device_id)
            .expect("query diagnostics");
        let boot_count = before["bootCount"].as_u64().expect("boot count");
        let storage_used = before["storage"]["usedBytes"]
            .as_u64()
            .expect("SPIFFS used bytes");
        let appearance_slot = before["storage"]["activeAppearanceSlot"].clone();
        assert!(storage_used > 0, "appearance storage should not be empty");
        assert!(before["memory"]["freeHeapBytes"].as_u64().unwrap_or(0) > 0);
        assert!(before["memory"]["freePsramBytes"].as_u64().unwrap_or(0) > 0);

        let reset = manager
            .reset_input_config(&expected_board_device_id)
            .expect("reset input config");
        assert_eq!(reset["ok"], true);
        assert_eq!(reset["preservedAppearanceAssets"], true);

        let reboot = manager
            .reboot_device(&expected_board_device_id)
            .expect("schedule reboot");
        assert_eq!(reboot["ok"], true);
        assert_eq!(reboot["preservedAppearanceAssets"], true);
        thread::sleep(Duration::from_secs(2));
        manager.disconnect();

        let reopen_deadline = std::time::Instant::now() + Duration::from_secs(20);
        loop {
            match manager.connect(&port_name, |_topic, _payload| {}) {
                Ok(()) => break,
                Err(_) if std::time::Instant::now() < reopen_deadline => {
                    thread::sleep(Duration::from_millis(500));
                }
                Err(error) => panic!("reconnect after diagnostic reboot: {error}"),
            }
        }
        let hello_deadline = std::time::Instant::now() + Duration::from_secs(15);
        while std::time::Instant::now() < hello_deadline {
            if manager.status().runtime.eq_ignore_ascii_case("esp-p4") {
                break;
            }
            thread::sleep(Duration::from_millis(100));
        }
        let after = manager
            .query_diagnostics(&expected_board_device_id)
            .expect("query after reboot");
        assert!(after["bootCount"].as_u64().unwrap_or(0) > boot_count);
        assert_eq!(after["lastResetReason"], "software");
        assert_eq!(after["lastResetWasFault"], false);
        assert_eq!(after["storage"]["usedBytes"].as_u64(), Some(storage_used));
        assert_eq!(after["storage"]["activeAppearanceSlot"], appearance_slot);
        manager.disconnect();
    }

    #[test]
    fn widget_chunk_payload_carries_integrity_fields() {
        let payload = build_widget_chunk_payload(
            "widget-1",
            "runtime/widget.json",
            "aGVsbG8=",
            0,
            5,
            "a430d84680aabd0b",
        );

        assert_eq!(payload["decodedSize"], 5);
        assert_eq!(payload["checksum"], "a430d84680aabd0b");
        assert_eq!(payload["index"], "0");
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
    fn widget_ota_skips_directory_placeholder_files() {
        assert!(widget_ota_should_skip_path("assets/.keep"));
        assert!(widget_ota_should_skip_path("runtime/.keep"));
        assert!(!widget_ota_should_skip_path("runtime/widget.json"));
        assert!(!widget_ota_should_skip_path("assets/icon.png"));
    }

    #[test]
    fn token_usage_widget_is_compiled_for_the_bounded_p4_runtime() {
        let source = br#"{
            "schema_version": 1,
            "readers": {"stats": {"path": ".stats-display"}},
            "fetchers": {"remote": {"url": "https://example.invalid"}},
            "dashboard": {"metricValue": {"var": "tokens_display"}}
        }"#;

        let prepared =
            prepare_p4_widget_file("token-usage", "runtime/widget.json", source).unwrap();
        let runtime: serde_json::Value = serde_json::from_slice(&prepared).unwrap();

        assert!(runtime.get("readers").is_none());
        assert!(runtime.get("fetchers").is_none());
        assert_eq!(runtime["dashboard"]["metricValue"]["var"], "tokens_display");
    }

    #[test]
    fn p4_widget_pages_drop_legacy_display_labels_before_transfer() {
        let source = br#"{
            "schema_version": 1,
            "pages": [
                {"id": "overview", "label": "Overview"},
                {"id": "details"}
            ]
        }"#;
        let prepared =
            prepare_p4_widget_file("third-party", "runtime/widget.json", source).unwrap();
        let runtime: serde_json::Value = serde_json::from_slice(&prepared).unwrap();

        assert_eq!(runtime["pages"][0], serde_json::json!({"id": "overview"}));
        assert_eq!(runtime["pages"][1], serde_json::json!({"id": "details"}));
    }

    #[test]
    fn p4_widget_normalizes_legacy_missing_or_empty_array_vars() {
        for source in [
            br#"{"schema_version":1,"states":["idle"]}"#.as_slice(),
            br#"{"schema_version":1,"vars":[],"states":["idle"]}"#.as_slice(),
        ] {
            let prepared =
                prepare_p4_widget_file("third-party", "runtime/widget.json", source).unwrap();
            let runtime: serde_json::Value = serde_json::from_slice(&prepared).unwrap();

            assert_eq!(runtime["vars"], serde_json::json!({}));
        }
    }

    #[test]
    fn p4_widget_rejects_nonempty_array_vars_before_transfer() {
        let source = br#"{
            "schema_version":1,
            "vars":[{"name":"count","type":"int","init":0}],
            "states":["idle"]
        }"#;
        let error =
            prepare_p4_widget_file("third-party", "runtime/widget.json", source).unwrap_err();

        assert!(error.contains("vars 必须是以变量名为键的 JSON 对象"));
        assert!(error.contains("{}"));
    }

    #[test]
    fn p4_widget_rejects_unsupported_var_fields_before_transfer() {
        let source = br#"{
            "schema_version":1,
            "vars":{"score":{"type":"int","init":0,"max":99}},
            "states":["idle"]
        }"#;
        let error =
            prepare_p4_widget_file("third-party", "runtime/widget.json", source).unwrap_err();

        assert!(error.contains("runtime/widget.json.vars.score"));
        assert!(error.contains("max"));
        assert!(error.contains("type"));
        assert!(error.contains("init"));
    }

    #[test]
    fn arbitrary_widget_readers_are_not_silently_enabled_for_p4() {
        let source = br#"{"schema_version":1,"readers":{"local":{}}}"#;
        let prepared =
            prepare_p4_widget_file("third-party", "runtime/widget.json", source).unwrap();
        let runtime: serde_json::Value = serde_json::from_slice(&prepared).unwrap();

        assert_eq!(runtime["readers"], serde_json::json!({"local": {}}));
        assert_eq!(runtime["vars"], serde_json::json!({}));
    }

    #[test]
    fn p4_widget_json_is_compacted_before_bounded_ota() {
        let source = br#"{
          "schema_version": 1,
          "states": ["ready", "playing"],
          "dashboard": {"title": "Whack A Mole"}
        }"#;
        let prepared =
            prepare_p4_widget_file("whack-a-mole", "runtime/widget.json", source).unwrap();

        assert!(prepared.len() < source.len());
        assert!(!prepared.contains(&b'\n'));
        assert_eq!(
            serde_json::from_slice::<serde_json::Value>(&prepared).unwrap()["states"][1],
            "playing"
        );
    }

    #[test]
    fn p4_widget_buttons_drop_legacy_component_navigation_actions() {
        let source = br#"[
          {"action":"game.start","control":"SW3","event":"button.sw3.short_press","label":"start"},
          {"action":"page_main","control":"SW1","event":"button.sw1.short_press","label":"exit"},
          {"action":"page_back","control":"SW2","event":"button.sw2.short_press","label":"back"}
        ]"#;
        let prepared = prepare_p4_widget_file("legacy-game", "buttons.json", source).unwrap();
        let buttons: serde_json::Value = serde_json::from_slice(&prepared).unwrap();

        assert_eq!(
            buttons,
            serde_json::json!([{
                "action": "game.start",
                "control": "SW3",
                "event": "button.sw3.short_press",
                "label": "start"
            }])
        );
    }

    #[test]
    fn p4_widget_json_rejects_compacted_content_beyond_device_buffer() {
        let source = serde_json::to_vec(&serde_json::json!({
            "schema_version": 1,
            "oversized": "x".repeat(P4_WIDGET_JSON_MAX_BYTES),
        }))
        .unwrap();
        let error =
            prepare_p4_widget_file("oversized-widget", "runtime/widget.json", &source).unwrap_err();

        assert!(error.contains("after JSON compaction"));
        assert!(error.contains("4095"));
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
    fn appearance_sync_plan_uses_full_sync_when_p4_h264_stream_differs() {
        let local = vec![AppearanceAssetDigest {
            kind: "p4-h264",
            device_path: "p4/families/idle.default.h264".to_string(),
            size: 100,
            checksum: "h264-new".to_string(),
        }];
        let remote = std::collections::HashMap::from([(
            "p4/families/idle.default.h264".to_string(),
            AssetRemoteStat {
                size: 100,
                checksum: "h264-old".to_string(),
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
            parse_missing_asset_ack_phase(
                "未收到板端素材 OTA 确认: transferId=t phase=stat path=videos/done.mp4"
            ),
            Some(AppearanceAssetAckPhase::Stat)
        );
        assert_eq!(
            parse_missing_asset_ack_phase(
                "未收到板端素材 OTA 确认: transferId=t phase=file path=videos/done.mp4"
            ),
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
