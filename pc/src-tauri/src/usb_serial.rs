/*
 * [Input] serialport-enumerated CDC USB ports plus JSON-line OTA/state payloads.
 * [Output] USB serial manager for device handshake, state/speech forwarding,
 *          timeout-preserving bounded frames, boot-aware Windows probing,
 *          reusable port handles, verified-rate reconnect caching and
 *          lifecycle-updated status snapshots during bulk writes;
 *          persistent prebuilt P4 H.264 Annex-B/WAV ready packs plus
 *          LGPL-safe VideoToolbox/Media Foundation custom-media encoding with
 *          native-SPS structural and full-decode validation, plus paced
 *          4 Mbaud JSON control frames plus the hardware-proven 8KiB raw
 *          chunks and 64-byte macOS CH343 drain pacing,
 *          transactional OTA with checksum acks and deterministic pack IDs
 *          with bounded app-data JSONL diagnostics and durable unknown-result latches
 *          that instantly reactivate either cached P4 A/B slot,
 *          serialized bulk-write transactions and native-USB raw-pack
 *          rejection before any destructive full-sync write plus nonce-bound
 *          boardDeviceId selection across multiple native-USB candidates,
 *          audio-only patch commits, no post-submission transport fallback, short-id ACK-gated widget
 *          .clawpkg OTA with frame-contiguous PNG-to-RGB565-alpha sprite compilation,
 *          plus capability-gated, expected-board-id removal with
 *          legacy unsupported-phase NACK correlation, capability-sized,
 *          retry-before-downshift Base64 firmware chunks with stale-transfer
 *          discovery, exact abort recovery, and connection-generation pinning,
 *          request-id-matched live device widget inventory, and persisted
 *          input configuration reads/writes serialized behind bulk transfers;
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
 *          handles non-inheritable by Bridge child processes; windows_serial
 *          opens overlapped RX/TX with separate completion events;
 *          background probes use bounded handshakes and per-adapter retry backoff;
 *          failed heartbeat writes mark stale connections for auto-reconnect;
 *          ignored hardware coverage verifies active-set-driven terminal-card
 *          retention and stable first-seen ordering across working refreshes.
 * [Output] Fixed-category media rejection diagnostics without device message text.
 * [Pos] Tauri USB transport node in pc/src-tauri/src
 * [Sync] If this file changes, update `pc/.folder.md`.
 */

use base64::Engine as _;
use serde::{Deserialize, Serialize};
use serialport::SerialPortType;
use sha2::{Digest, Sha256};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};

use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant};

mod appearance_transaction;
mod appearance_recovery;
mod connection_handle;
mod live_audio_pacing;
mod media_arbitration;
mod master_volume;
use live_audio_pacing::write_serial_live_frame;
#[cfg(windows)]
mod windows_serial;
#[cfg(any(windows,test))]
mod windows_configuration;
mod firmware_transaction;
mod firmware_begin_wait;
mod firmware_diagnostics;
mod firmware_chip;
pub(crate) use firmware_chip::{bundled_resource_for_chip, chip_target_from_device, P4ChipTarget};
use firmware_chip::{firmware_chip_range, validate_firmware_chip};
mod native_usb_protocol;
mod transaction_waiters;
mod transfer_log;
mod receive_diagnostics;
mod control_integrity;
#[cfg(test)]
mod control_integrity_capture;
#[cfg(test)]
mod control_integrity_transport_tests;
#[cfg(test)]
mod control_integrity_connected_transport_tests;
#[cfg(test)]
mod query_duplex_tests;
#[cfg(test)]
mod appearance_commit_acceptance;
mod appearance_safety;
mod appearance_activation_evidence;
mod inbound_owner;
#[cfg(test)]
mod lifecycle_safety_tests;
mod widget_transaction;
pub(crate) mod catalog;
pub(crate) mod program_autostart_host;
#[cfg(test)]
mod component_sync_safety_tests;
mod bubble_pixels;
pub(crate) use bubble_pixels::BubbleFrame;

use appearance_transaction::{
    asset_checksum_hex, build_asset_file_commit_payload, compute_p4_pack_id,
    digest_appearance_assets, p4_pack_id_from_assets, p4_raw_transfer_fallback_slot, p4_builtin_slot,
    parse_p4_appearance_slot_state,
    plan_appearance_sync_from_digests,
    AppearanceAssetDigest, AppearanceAssetEntry, AppearanceFullSyncMode,
    AppearanceSyncPlan, AssetRemoteStat, P4AppearanceSlotState, P4CachedPackActivation,
};
#[cfg(test)]
use appearance_transaction::{
    P4AppearanceSlot, P4_BUILTIN_APPEARANCE_SLOT, P4_RAW_APPEARANCE_SLOT,
    AppearanceAssetAckPhase, parse_missing_asset_ack_phase, should_retry_appearance_with_legacy_full_sync,
};
#[cfg(test)]
use connection_handle::serial_open_error_is_transient;
use connection_handle::{open_serial_pair_with_retry, ProbedSerialPort, UsbConnection};
use firmware_transaction::{
    evaluate_firmware_validation, firmware_chunk_error_is_retryable, firmware_base64_rejection_unapplied,
    firmware_corruption_fallback_size, firmware_recovery_chunk_size, initial_firmware_chunk_size,
    parse_esp_idf_app_descriptor, preferred_firmware_chunk_size,
    FirmwareCommandError, VerifiedFirmware,
    P4_FIRMWARE_CHUNK_ACK_TIMEOUT,
    P4_FIRMWARE_CHUNK_MAX_ATTEMPTS, P4_FIRMWARE_COMMIT_ACK_TIMEOUT,
    P4_FIRMWARE_CORRUPTION_RETRIES_BEFORE_FALLBACK,
    P4_FIRMWARE_MAX_IMAGE_SIZE, P4_FIRMWARE_RECONNECT_TIMEOUT,
};
pub use firmware_transaction::{inspect_firmware_image, FirmwareImageInfo, FirmwareUpdateResult};
#[cfg(test)]
use firmware_transaction::{
    ESP_APP_DESC_MAGIC, ESP_APP_DESC_SIZE, ESP_IMAGE_HEADER_SIZE, ESP_IMAGE_SEGMENT_HEADER_SIZE,
    P4_FIRMWARE_CHUNK_SIZE, P4_FIRMWARE_FALLBACK_CHUNK_SIZE, P4_FIRMWARE_FAST_CHUNK_SIZE,
    P4_FIRMWARE_PROJECT_NAME, P4_FIRMWARE_RECOVERY_SUCCESS_STREAK, P4_FIRMWARE_SAFE_CHUNK_SIZE,
};
use transaction_waiters::{
    resolve_asset_ack, resolve_device_response, resolve_firmware_ack, resolve_firmware_status,
    resolve_widget_ack, AssetAckWaiter, DeviceResponseWaiter, FirmwareAckWaiter,
    FirmwareStatusWaiter, WidgetAckWaiter,
};
#[cfg(test)]
use widget_transaction::P4_WIDGET_JSON_MAX_BYTES;
use widget_transaction::{
    build_widget_chunk_payload, ensure_widget_delete_supported, ensure_widget_inventory_supported,
    format_widget_ack_timeout, prepare_p4_widget_file, prepare_p4_widget_sprite_files,
    widget_ota_relative_path, widget_ota_should_skip_path, WIDGET_BEGIN_ACK_TIMEOUT,
    WIDGET_CHUNK_ACK_TIMEOUT, WIDGET_CHUNK_MAX_ATTEMPTS, WIDGET_COMMIT_ACK_TIMEOUT,
    WIDGET_DELETE_ACK_TIMEOUT,
};

use native_usb_protocol::{
    encode_native_usb_frame, next_native_usb_nonce, parse_native_usb_pong,
    select_native_usb_candidate, try_pop_native_usb_frame, NativeUsbCandidateIdentity,
    NativeUsbFrame, NativeUsbIdentity, P4_NATIVE_KIND_COMMIT, P4_NATIVE_KIND_FILE_BEGIN,
    P4_NATIVE_KIND_FILE_DATA, P4_NATIVE_KIND_FILE_END, P4_NATIVE_KIND_JSON, P4_NATIVE_KIND_PING,
    P4_NATIVE_USB_MAX_PAYLOAD,
};

pub fn configure_transfer_logging(app_data_dir: &Path) -> Result<PathBuf, String> {
    let path=transfer_log::configure(app_data_dir)?;
    appearance_safety::configure(app_data_dir)?;
    Ok(path)
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
    #[serde(default)]
    pub connection_id: String,
    #[serde(default)]
    pub boot_id: String,
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
const P4_UNIFIED_UART_BAUD: u32 = 4_000_000;
const P4_COMPAT_UART_BAUD: u32 = 2_000_000;
// These platform-specific budgets retain the proven transmission pacing;
// discovery and firmware UART configuration are independent of this budget.
#[cfg(windows)]
const P4_USB_UART_BAUD: u32 = 2_000_000;
#[cfg(not(windows))]
const P4_USB_UART_BAUD: u32 = 4_000_000;
const P4_USB_UART_TRANSITION_BAUD: u32 = 3_000_000;
// Linux board-runtime emits hello every three seconds until acknowledged, and
// opening the CH343 can cold-boot P4 for roughly five seconds before its first
// protocol frame. The first host write can also be lost while the adapter or
// UART0 settles, so keep requesting identity throughout the probe window.
const SERIAL_PROBE_TIMEOUT: Duration = Duration::from_millis(8_000);
// Background fallback baud rates share a short window on every platform.
// serial_probe_attempt_timeout retains eight seconds for the first P4 probe
// and every first Unix probe (including cold-boot Linux board runtimes).
// Manual connect still passes SERIAL_PROBE_TIMEOUT for every baud.
const SERIAL_AUTO_PROBE_TIMEOUT: Duration = Duration::from_millis(1_500);
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
// Keep the logical raw chunk bounded so each checksum/ACK retry stays cheap.
// The physical CH343 write must still be paced below because this link has no
// RTS/CTS and silently drops bytes when macOS queues one large write.
// Keep each raw chunk divisible by three so its Base64 form has no trailing
// padding. Some ESP-IDF/Mbed TLS combinations reject otherwise valid padded
// payloads when JSON arrives in several UART reads.
const P4_APPEARANCE_ASSET_CHUNK_SIZE: usize = 20_478;
#[cfg(windows)]
const P4_RAW_APPEARANCE_SAFE_CHUNK_SIZE: usize = 2 * 1024;
#[cfg(not(windows))]
const P4_RAW_APPEARANCE_SAFE_CHUNK_SIZE: usize = 8 * 1024;
#[cfg(windows)]
const P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE: usize = 64 * 1024;
#[cfg(not(windows))]
const P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE: usize = P4_RAW_APPEARANCE_SAFE_CHUNK_SIZE;
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
const P4_FIRMWARE_STATUS_TIMEOUT: Duration = Duration::from_secs(2);
// Control frames keep the proven 64-byte packetization on both desktop OSes.
// Raw bodies use wider Windows WriteFile calls to reduce syscall overhead;
// macOS retains the hardware-proven drained 64-byte write for every slice.
const P4_CH343_SERIAL_WRITE_SLICE_BYTES: usize = 64;
#[cfg(windows)]
const P4_CH343_SERIAL_WRITE_GAP: Duration = Duration::from_micros(400);
#[cfg(not(windows))]
const P4_CH343_SERIAL_WRITE_GAP: Duration = Duration::from_micros(250);
#[cfg(windows)]
const P4_CH343_RAW_WRITE_SLICE_BYTES: usize = 128;
#[cfg(not(windows))]
const P4_CH343_RAW_WRITE_SLICE_BYTES: usize = 64;
#[cfg(windows)]
const P4_CH343_RAW_WRITE_GAP: Duration = Duration::from_micros(500);
#[cfg(not(windows))]
const P4_CH343_RAW_WRITE_GAP: Duration = Duration::from_micros(250);
const P4_FIRMWARE_CHUNK_RETRY_DELAY: Duration = Duration::from_millis(1);

// native USB bulk is the high-speed ESP32-P4 OTG data plane. UART remains as a fallback.
struct NativeUsbP4Transport {
    handle: rusb::DeviceHandle<rusb::GlobalContext>,
    rx_buffer: Vec<u8>,
    next_seq: u32,
    appearance_owner: Option<UsbSerialManager>,
    asset_transfer_id: Option<String>,
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
            appearance_owner: None,
            asset_transfer_id: None,
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
        if payload.len()>P4_NATIVE_USB_MAX_PAYLOAD {return Err("Native USB payload too large".into());}
        let native_owner=self.appearance_owner.clone();
        let _admission=match native_owner.as_ref(){
            Some(owner)=>{let attempt=owner.active_appearance_attempt()?.ok_or("appearance owner missing")?;
                let admission=owner.appearance_admission(&attempt)?;
                let c=admission.1.as_ref().ok_or("appearance connection missing")?;
                owner.ensure_wire_available(&c.port_name,Some(&c.board_device_id))?;
                Some(admission)},
            None=>None,
        };
        if let Some(owner)=&native_owner {
            let command=if kind==P4_NATIVE_KIND_FILE_DATA||kind==P4_NATIVE_KIND_PING {None}
                else{Some(serde_json::from_slice::<serde_json::Value>(payload).map_err(|_|"invalid native asset metadata")?)};
            let mutation=kind!=P4_NATIVE_KIND_PING&&(kind!=P4_NATIVE_KIND_JSON
                ||command.as_ref().is_some_and(|v|appearance_safety::mutation(v["topic"].as_str().unwrap_or(""))));
            let transfer_id=match kind {
                P4_NATIVE_KIND_JSON=>Some(appearance_safety::command_id(command.as_ref().unwrap()["topic"].as_str().unwrap_or(""),&command.as_ref().unwrap()["payload"] )?.to_owned()),
                P4_NATIVE_KIND_FILE_BEGIN|P4_NATIVE_KIND_FILE_END|P4_NATIVE_KIND_COMMIT=>Some(appearance_safety::command_id("asset/native",command.as_ref().unwrap())?.to_owned()),
                P4_NATIVE_KIND_FILE_DATA=>Some(self.asset_transfer_id.clone().ok_or("native asset owner missing")?),
                P4_NATIVE_KIND_PING=>None,
                _=>return Err("unsupported native asset frame".into()),
            };
            if kind==P4_NATIVE_KIND_FILE_BEGIN {
                if self.asset_transfer_id.as_ref().is_some_and(|old|Some(old)!=transfer_id.as_ref()){return Err("native asset transferId changed".into());}
                self.asset_transfer_id=transfer_id.clone();
            }else if matches!(kind,P4_NATIVE_KIND_FILE_END|P4_NATIVE_KIND_COMMIT)
                &&self.asset_transfer_id.as_ref()!=transfer_id.as_ref(){return Err("native asset transferId changed".into());}
            if mutation {owner.active_appearance_attempt()?.ok_or("appearance owner missing")?.before_mutation(
                if kind==P4_NATIVE_KIND_COMMIT {"asset/native-commit"}else{"asset/native-frame"},transfer_id.as_deref().ok_or("native transferId missing")?)?;}
        }
        let frame = encode_native_usb_frame(kind, self.next_seq, payload);
        self.next_seq = self.next_seq.wrapping_add(1);
        let activating=kind==P4_NATIVE_KIND_JSON && serde_json::from_slice::<serde_json::Value>(payload).ok().is_some_and(|v|v["topic"]=="asset/activate");
        if activating {if let Some(owner)=&native_owner {owner.activation_update(|t|t.write_started());}}
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
        if activating {if let Some(owner)=&native_owner {owner.activation_update(|t|t.write_completed());}}
        Ok(())
    }

    fn send_json_value(&mut self, value: &serde_json::Value) -> Result<(), String> {
        if value["topic"]=="asset/activate" {if let Some(owner)=&self.appearance_owner {owner.activation_start();}}
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
        let diagnostic_owner=self.appearance_owner.clone();
        let mark=|kind|{if phase=="activate" {if let Some(owner)=&diagnostic_owner {owner.activation_update(|t|t.fail(kind));}}};
        if phase=="activate" {if let Some(owner)=&diagnostic_owner {owner.activation_update(|t|t.ack_wait_started());}}
        let started = std::time::Instant::now();
        loop {
            if cancel_requested.load(Ordering::SeqCst) {
                mark(appearance_activation_evidence::Kind::Cancelled);
                return Err(APPEARANCE_SYNC_CANCELLED_ERROR.to_string());
            }
            let remaining = timeout
                .checked_sub(started.elapsed())
                .unwrap_or_else(|| Duration::from_millis(1));
            if started.elapsed() >= timeout {
                mark(appearance_activation_evidence::Kind::AckTimeout);
                return Err(format!("Native USB ack timeout at phase {phase}"));
            }
            let frame = match self.read_frame(remaining.min(Duration::from_millis(100))) {
                Ok(frame) => frame,
                Err(error) if error == "Native USB read timeout" => continue,
                Err(error) => {mark(appearance_activation_evidence::Kind::AckReadFailed);return Err(error)},
            };
            if frame.kind != P4_NATIVE_KIND_JSON {
                continue;
            }
            let message: SerialMessage = serde_json::from_slice(&frame.payload)
                .map_err(|e| {mark(appearance_activation_evidence::Kind::MalformedResponse);format!("Native USB JSON parse failed: {e}")})?;
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
            if phase=="activate" {if let Some(owner)=&diagnostic_owner {owner.activation_update(|t|t.response(payload.get("ok").and_then(|v|v.as_bool())));}}
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

fn p4_force_full_sync_requested() -> bool {
    std::env::var("P4_FORCE_FULL_SYNC").as_deref() == Ok("1")
}

fn p4_raw_asset_chunk_size_for_limit(advertised_limit: Option<u64>) -> usize {
    advertised_limit
        .and_then(|size| usize::try_from(size).ok())
        .map(|size| size.min(P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE))
        .filter(|size| *size >= P4_RAW_APPEARANCE_SAFE_CHUNK_SIZE)
        .unwrap_or(P4_RAW_APPEARANCE_SAFE_CHUNK_SIZE)
}

fn should_downgrade_p4_raw_chunk_size(error: &str) -> bool {
    error.contains("raw chunk checksum mismatch")
        || error.contains("timed out waiting for raw P4 asset chunk")
}

fn record_firmware_failure_snapshot(row: firmware_diagnostics::Snapshot) {
    transfer_log::record("firmware", "failure_metadata", serde_json::json!({
        "scope": if row.command { "command" } else { "transaction" },
        "phase": row.phase.label(), "stage": row.stage.label(), "outcome": row.outcome.label(),
        "sequence": row.sequence, "offset": row.offset, "expectedBytes": row.expected_bytes,
        "totalBytes": row.total_bytes, "attempt": row.attempt,
        "lastAckPhase": row.last_ack.map(|ack| ack.phase.label()),
        "lastAckNextSequence": row.last_ack.map(|ack| ack.next_sequence),
        "lastAckReceivedBytes": row.last_ack.map(|ack| ack.received_bytes),
        "elapsedMs": row.elapsed_ms,
    }));
}

struct SerialReaderPause<'a>(&'a AtomicBool);

impl<'a> SerialReaderPause<'a> {
    fn new(flag: &'a AtomicBool) -> Self {
        flag.store(true, Ordering::Release);
        Self(flag)
    }
}

impl Drop for SerialReaderPause<'_> {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

// Only this exact diagnostic topic is added: firmware handles it as a status
// read. Do not generalize this to */query or action-bearing control topics.
fn serial_frame_is_bounded(topic: &str, live_frame: bool) -> bool {
    live_frame || matches!(topic, "system/heartbeat" | "diagnostics/query")
}

fn flush_serial_writer<W: Write + ?Sized>(writer: &mut W) -> std::io::Result<()> {
    // Serial writes are unbuffered in user space. macOS tcdrain has no bounded
    // timeout; use the peer's protocol ACK as the delivery barrier instead.
    #[cfg(target_os = "macos")]
    { let _ = writer; Ok(()) }
    #[cfg(not(target_os = "macos"))]
    { writer.flush() }
}

fn write_serial_bytes_paced(
    writer: &mut dyn Write,
    bytes: &[u8],
    slice_bytes: usize,
    gap: Duration,
) -> std::io::Result<()> {
    #[cfg(target_os = "macos")]
    {
        let slice_bytes = slice_bytes.max(1);
        let chunks = bytes.chunks(slice_bytes);
        let chunk_count = chunks.len();
        for (index, chunk) in chunks.enumerate() {
            writer.write_all(chunk)?;
            // TTYPort::flush calls tcdrain on macOS. WCH drivers can block
            // there indefinitely, ignoring the configured write timeout and
            // wedging the port even after disconnect. Pace bounded writes;
            // protocol acknowledgements, not a kernel drain, confirm delivery.
            if index + 1 < chunk_count && !gap.is_zero() {
                thread::sleep(gap);
            }
        }
        return Ok(());
    }

    #[cfg(windows)]
    {
        // WriteFile boundaries are enough to keep the CH343 stream packetized
        // on Windows. FlushFileBuffers after every physical write takes hundreds
        // of milliseconds on some WCH drivers, so drain only once per logical
        // protocol frame or raw chunk.
        let chunks = bytes.chunks(slice_bytes.max(1));
        let chunk_count = chunks.len();
        for (index, chunk) in chunks.enumerate() {
            writer.write_all(chunk)?;
            if index + 1 < chunk_count && !gap.is_zero() {
                let deadline = Instant::now() + gap;
                while Instant::now() < deadline {
                    std::hint::spin_loop();
                }
            }
        }
        writer.flush()?;
        return Ok(());
    }

    #[cfg(all(not(target_os = "macos"), not(windows)))]
    {
        let _ = (slice_bytes, gap);
        writer.write_all(bytes)?;
        writer.flush()?;
        Ok(())
    }
}

fn observe_serial_capture(active: &AtomicBool, topic: &str, payload: &serde_json::Value) {
    match topic {
        "audio/begin" | "audio/chunk" => active.store(true, Ordering::Release),
        "audio/end" => active.store(false, Ordering::Release),
        "audio/status" => {
            if let Some(value) = payload.get("active").and_then(serde_json::Value::as_bool) {
                active.store(value || payload.get("conversation").and_then(serde_json::Value::as_bool) == Some(true), Ordering::Release);
            }
        }
        _ => {}
    }
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
            P4_UNIFIED_UART_BAUD,
            P4_COMPAT_UART_BAUD,
            P4_USB_UART_TRANSITION_BAUD,
            DEFAULT_USB_SERIAL_BAUD,
            LEGACY_USB_SERIAL_BAUD,
        ]
    } else {
        vec![
            DEFAULT_USB_SERIAL_BAUD,
            LEGACY_USB_SERIAL_BAUD,
            P4_UNIFIED_UART_BAUD,
            P4_COMPAT_UART_BAUD,
            P4_USB_UART_TRANSITION_BAUD,
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

fn hello_ack_due(last_sent: Option<Instant>, now: Instant, desktop_device_id: &str) -> bool {
    // P4 acknowledges bind/ack with hello. Echoing every hello creates an
    // unbounded hello -> ack -> hello loop. Linux/P4 periodic hello still lets
    // a lost ACK retry; an empty desktop identity cannot establish a binding.
    !desktop_device_id.trim().is_empty()
        && last_sent.is_none_or(|last| now.saturating_duration_since(last) >= Duration::from_secs(2))
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
    nonce: &str,
) -> Result<(), String> {
    if !control_integrity::valid_nonce(nonce) { return Err("invalid control handshake nonce".to_string()); }
    let mut messages=serial_probe_handshake_messages(desktop_device_id);
    messages[0]["payload"]=control_integrity::ack_payload(desktop_device_id,Some(nonce))
        .ok_or_else(||"invalid control handshake nonce".to_string())?;
    for message in messages {
        let mut line = serde_json::to_string(&message).map_err(|error| error.to_string())?;
        line.push('\n');
        writer
            .write_all(line.as_bytes())
            .map_err(|error| format!("serial handshake write failed: {error}"))?;
    }
    flush_serial_writer(writer)
        .map_err(|error| format!("serial handshake flush failed: {error}"))
}

fn serial_probe_attempt_timeout(index: usize, baud: u32, timeout: Duration) -> Duration {
    if index == 0 && (matches!(baud, P4_UNIFIED_UART_BAUD | P4_COMPAT_UART_BAUD) || !cfg!(windows)) {
        timeout.max(SERIAL_PROBE_TIMEOUT)
    } else {
        timeout
    }
}

// A temporary read error does not invalidate a partial physical frame. Use
// the same policy while probing and while connected; callers retain their
// original deadline/cancellation checks. This never retries a write.
fn serial_read_recovery_delay(error: &std::io::Error) -> Option<Duration> {
    match error.kind() {
        std::io::ErrorKind::TimedOut => Some(Duration::ZERO),
        std::io::ErrorKind::Interrupted | std::io::ErrorKind::WouldBlock => {
            Some(Duration::from_millis(2))
        }
        _ => None,
    }
}

fn probe_serial_port(
    port_name: &str,
    baud_candidates: &[u32],
    desktop_device_id: &str,
    probe_timeout: Duration,
) -> Result<ProbedSerialPort, String> {
    let mut failures = Vec::new();
    let mut reusable_pair: Option<(
        Box<dyn serialport::SerialPort>,
        Box<dyn serialport::SerialPort>,
    )> = None;
    for (index, baud) in baud_candidates.iter().copied().enumerate() {
        let pair = if let Some((mut writer, reader)) = reusable_pair.take() {
            match writer.set_baud_rate(baud) {
                Ok(()) => {
                    let _ = writer.clear(serialport::ClearBuffer::Input);
                    Ok((writer, reader))
                }
                Err(error) => Err(error.to_string()),
            }
        } else {
            open_serial_pair_with_retry(port_name, baud)
        };
        let (mut writer, reader) = match pair {
            Ok(ports) => ports,
            Err(error) => {
                failures.push(error);
                continue;
            }
        };

        // Runtime connections must not assert boot/reset control lines on
        // any platform. Firmware flashing keeps its separate esptool path.
        let _ = writer.write_data_terminal_ready(false);
        let _ = writer.write_request_to_send(false);
        let attempt_timeout = serial_probe_attempt_timeout(index, baud, probe_timeout);
        transfer_log::record(
            "connection",
            "probe",
            serde_json::json!({
                "port": port_name, "baud": baud, "timeoutMs": attempt_timeout.as_millis(),
            }),
        );
        let control_nonce=uuid::Uuid::new_v4().simple().to_string();
        if let Err(error) = send_serial_probe_handshakes(writer.as_mut(), desktop_device_id,&control_nonce) {
            failures.push(format!("{baud} baud: {error}"));
            continue;
        }

        let started = Instant::now();
        let mut next_handshake_at = started + SERIAL_PROBE_HANDSHAKE_RETRY_INTERVAL;
        let mut reader = BufReader::new(reader);
        let mut line_buffer = Vec::with_capacity(32 * 1024);
        let mut rejections = receive_diagnostics::Rejections::default();
        let mut integrity_rejections=[0u64;6];
        while started.elapsed() < attempt_timeout {
            if Instant::now() >= next_handshake_at {
                if let Err(error) = send_serial_probe_handshakes(writer.as_mut(), desktop_device_id,&control_nonce)
                {
                    failures.push(format!("{baud} baud handshake retry failed: {error}"));
                    break;
                }
                next_handshake_at = Instant::now() + SERIAL_PROBE_HANDSHAKE_RETRY_INTERVAL;
            }
            let frame = match read_serial_frame(&mut reader, &mut line_buffer) {
                Ok(Some(line)) => line,
                Ok(None) => break,
                Err(error) if serial_read_recovery_delay(&error).is_some() => {
                    let delay = serial_read_recovery_delay(&error).unwrap();
                    if !delay.is_zero() { thread::sleep(delay); }
                    continue;
                }
                Err(error) => {
                    failures.push(format!("{baud} baud read failed: {error}"));
                    break;
                }
            };
            let line = &frame.text;
            let Some(mut message) = parse_serial_frame(&frame) else {
                if let Some(reason)=frame.integrity.failure.or_else(||(frame.integrity.framed&&!frame.valid_utf8).then_some(control_integrity::Failure::Utf8)) {
                    log_serial_integrity_failure(frame.integrity,reason,&mut integrity_rejections,"probe",0,baud);
                }
                log_rejected_serial_line(&line, &mut rejections, "probe", 0, baud);
                continue;
            };
            if message.topic != "hello" {
                continue;
            }
            if !control_integrity::hello_matches_probe(&message.payload,frame.integrity.framed,&control_nonce) {
                log_serial_integrity_failure(frame.integrity,control_integrity::Failure::Negotiation,&mut integrity_rejections,"probe",0,baud);
                continue;
            }
            if let Err(error) = validate_hello_message(&mut message) {
                failures.push(format!("{baud} baud rejected hello: {error}"));
                continue;
            }
            return Ok(ProbedSerialPort::from_verified_reader(writer, reader, baud, message));
        }
        failures.push(format!("{baud} baud: no valid protocol hello"));
        if cfg!(windows) {
            reusable_pair = Some((writer, reader.into_inner()));
        }
    }

    Err(format!(
        "{port_name} did not identify as a supported Pet Manager device ({})",
        failures.join("; ")
    ))
}

const P4_AUDIO_PATCH_CONTRACT: &str = "p4-audio-patch-v1";

#[derive(Clone)]
struct P4AudioPatchFile {
    family: String,
    path: String,
    bytes: Vec<u8>,
}

fn validate_p4_audio_patch_wav(bytes: &[u8]) -> Result<(), String> {
    // Keep the existing profile/size limit, then match the overlay firmware's
    // exact RIFF framing before admission. This never edits/repairs WAV bytes.
    validate_p4_audio_wav(bytes)?;
    let u32_at = |at: usize| u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap()) as usize;
    if u32_at(4) != bytes.len() - 8 { return Err("audio-only WAV RIFF size mismatch".into()); }
    let mut offset = 12usize;
    let mut format = false;
    let mut data = false;
    while offset + 8 <= bytes.len() {
        let tag = &bytes[offset..offset + 4];
        let size = u32_at(offset + 4);
        offset += 8;
        let end = offset.checked_add(size).filter(|end| *end <= bytes.len()).ok_or("audio-only WAV chunk overflow")?;
        if tag == b"fmt " {
            if size < 16 { return Err("audio-only WAV format truncated".into()); }
            format = bytes[offset..offset + 4] == [1, 0, 1, 0]
                && u32_at(offset + 4) == 16_000 && u32_at(offset + 8) == 32_000
                && bytes[offset + 12..offset + 16] == [2, 0, 16, 0];
        } else if tag == b"data" {
            if !format || size == 0 || size & 1 != 0 || size > 1024 * 1024 {
                return Err("audio-only WAV data/profile mismatch".into());
            }
            data = true;
        }
        offset = end.checked_add(size & 1).filter(|end| *end <= bytes.len()).ok_or("audio-only WAV padding overflow")?;
    }
    if !format || !data || offset != bytes.len() { return Err("audio-only WAV framing mismatch".into()); }
    Ok(())
}

fn p4_audio_patch_byte_count(files: &[P4AudioPatchFile]) -> Result<u64, String> {
    if files.len() > 24 { return Err("audio-only file limit exceeded".into()); }
    let mut paths = HashSet::new();
    let mut families = HashSet::new();
    let mut total = 0u64;
    for file in files {
        if file.family.is_empty() || file.family.len() > 32
            || !file.family.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_' || c == b'.')
            || file.path != format!("p4/audio/{}.wav", file.family)
            || !paths.insert(file.path.as_str()) || !families.insert(file.family.as_str()) {
            return Err("audio-only requires unique bounded family WAV paths".into());
        }
        validate_p4_audio_patch_wav(&file.bytes)?;
        total = total.checked_add(file.bytes.len() as u64).ok_or("audio-only size overflow")?;
    }
    if total > 2 * 1024 * 1024 { return Err("audio-only total exceeds 2 MiB".into()); }
    Ok(total)
}

fn p4_audio_patch_send_files<F, P>(
    files: &[P4AudioPatchFile], mut send_chunk: F, on_progress: &P,
) -> Result<(), String>
where F: FnMut(&str, u32, u64, &[u8]) -> Result<(), String>,
      P: Fn(u32, u32, u64, u64) {
    let total = p4_audio_patch_byte_count(files)?;
    let mut sent = 0u64;
    on_progress(0, files.len() as u32, 0, total);
    for (file_index, file) in files.iter().enumerate() {
        let mut offset = 0u64;
        for (index, chunk) in file.bytes.chunks(4096).enumerate() {
            send_chunk(&file.path, index as u32, offset, chunk)?;
            offset += chunk.len() as u64;
            sent += chunk.len() as u64;
            on_progress(file_index as u32, files.len() as u32, sent, total);
        }
        on_progress(file_index as u32 + 1, files.len() as u32, sent, total);
    }
    Ok(())
}

fn p4_audio_patch_target(
    builtin: bool, builtin_protected: bool, active_slot: Option<u32>,
    active_pack: &str, capability: Option<&str>,
) -> Result<u32, String> {
    if capability != Some(P4_AUDIO_PATCH_CONTRACT) {
        return Err("设备固件不支持安全的独立音效更新，请升级固件；未下发视频或切换形象。".into());
    }
    if active_pack.len() != 64 || !active_pack.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {
        return Err("当前形象身份未确认，未下发音效。".into());
    }
    let target = if builtin { 0 } else { 1 };
    if active_slot != Some(target) || (builtin && !builtin_protected) {
        return Err("设备当前形象与音效目标不一致，请先确认当前形象；未自动切换或完整上传。".into());
    }
    Ok(target)
}

fn p4_audio_patch_transfer_assets(assets: &[AppearanceAssetEntry]) -> Result<Vec<&AppearanceAssetEntry>, String> {
    let mut selected = Vec::new();
    for asset in assets {
        match asset.kind {
            "p4-audio" if asset.device_path.starts_with("p4/audio/") && asset.device_path.ends_with(".wav") => selected.push(asset),
            "p4-manifest" | "p4-h264" => {},
            _ => return Err("audio-only invalid prepared asset kind/path".into()),
        }
    }
    Ok(selected)
}

fn load_p4_audio_patch_files(
    appearance_dir: &Path, app_data_dir: &Path, builtin: bool,
) -> Result<(Vec<P4AudioPatchFile>, Vec<AppearanceAssetDigest>), String> {
    if builtin {
        // Do not inspect/prepare the default video pack. Only the explicit WAV
        // override map is input; firmware supplies the protected default identity.
        let map_path = appearance_dir.join("audio-overrides.json");
        if map_path.is_symlink() || appearance_dir.ancestors().any(|p| p.is_symlink()) {
            return Err("audio-only override path must be regular local source".into());
        }
        let overrides: std::collections::BTreeMap<String, String> = match std::fs::File::open(&map_path) {
            Ok(file) => {
                let mut bytes=Vec::new();
                file.take(65_537).read_to_end(&mut bytes).map_err(|_| "无法读取内置形象音效设置")?;
                if bytes.len()>65_536 { return Err("audio-only override map is too large".into()); }
                serde_json::from_slice(&bytes).map_err(|_| "内置形象音效设置损坏")?
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Default::default(),
            Err(_) => return Err("无法读取内置形象音效设置".into()),
        };
        if overrides.len() > 24 { return Err("audio-only file limit exceeded".into()); }
        let mut files = Vec::new();
        let temporary = tempfile::tempdir().map_err(|_| "audio-only staging unavailable")?;
        if !overrides.is_empty() {
            let directory = appearance_dir.join("audio-overrides");
            if directory.is_symlink() { return Err("audio-only override directory must not be a symlink".into()); }
            let root = directory.canonicalize().map_err(|_| "audio-only override directory unavailable")?;
            if !root.starts_with(app_data_dir.canonicalize().map_err(|_| "audio-only local source root unavailable")?) {
                return Err("audio-only override directory escaped local source".into());
            }
            for (family, relative) in overrides {
                if family.is_empty() || family.len() > 32
                    || !family.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_' || c == b'.') {
                    return Err("audio-only invalid family".into());
                }
                let rel = Path::new(&relative);
                if rel.is_absolute() || rel.components().any(|c| !matches!(c, std::path::Component::Normal(_))) {
                    return Err("audio-only invalid source path".into());
                }
                let source = app_data_dir.join(rel);
                if source.ancestors().any(|p|p.is_symlink()) || source.extension().and_then(|v| v.to_str()) != Some("wav")
                    || source.canonicalize().map_err(|_| "audio-only source unavailable")? != root.join(format!("{family}.wav")) {
                    return Err("audio-only source is outside the selected override directory".into());
                }
                let output = temporary.path().join(format!("{family}.wav"));
                stage_p4_audio_cue(&source, &output)?;
                let bytes = std::fs::read(output).map_err(|_| "audio-only normalized WAV unavailable")?;
                files.push(P4AudioPatchFile { path: format!("p4/audio/{family}.wav"), family, bytes });
            }
        }
        p4_audio_patch_byte_count(&files)?;
        return Ok((files, Vec::new()));
    }
    // A custom appearance still requires its validated ready profile/video
    // identity. Only WAV bytes enter the transfer set, never manifest/video.
    let assets = load_prepared_p4_appearance_pack(appearance_dir)?;
    let mut files = Vec::new();
    let mut videos = Vec::new();
    for asset in p4_audio_patch_transfer_assets(&assets)? {
        let bytes = std::fs::read(&asset.source_path).map_err(|_| "audio-only WAV unavailable")?;
        files.push(P4AudioPatchFile { family: asset.family_name.clone(), path: asset.device_path.clone(), bytes });
    }
    for asset in assets.into_iter().filter(|asset| asset.kind == "p4-h264") {
        videos.extend(digest_appearance_assets(&[asset])?);
    }
    p4_audio_patch_byte_count(&files)?;
    Ok((files, videos))
}

impl UsbSerialManager {
    pub fn sync_appearance_audio_only_p4<F>(
        &self, _appearance_dir: &Path, app_data_dir: &Path,
        expected_board_device_id: &str, _builtin: bool, on_progress: F,
    ) -> Result<(u32, u64, bool), String>
    where F: Fn(u32, u32, u64, u64) {
        let guard = self.asset_transfer_guard.lock().map_err(|_| "audio-only owner unavailable")?;
        self.with_appearance_user_intent(expected_board_device_id, &guard, || {
            let (status, generation) = self.connected_board_snapshot(expected_board_device_id)?;
            if status.capabilities["globalAudioCues"].as_str() != Some("p4-global-audio-cues-v1") {
                return Err("设备固件不支持安全的独立音效更新，请升级固件；未下发视频或切换形象。".into());
            }
            let boot = status.boot_id.clone();
            if boot.is_empty() { return Err("audio-only boot identity unavailable".into()); }
            let current = || -> Result<(), String> {
                self.ensure_appearance_sync_not_cancelled(None)?;
                self.check_appearance_connection()?;
                let (now, id) = self.connected_board_snapshot(expected_board_device_id)?;
                if id != generation || now.boot_id != boot { return Err("audio-only connection changed; no replay".into()); }
                Ok(())
            };
            let id = format!("audio-only-{}", uuid::Uuid::new_v4());
            current()?;
            // Fresh, nonce-bound readonly proof; no cached activation or Full.
            let query = serde_json::json!({"transferId":id,"queryNonce":id,
                "observationSchema":appearance_recovery::SCHEMA,
                "expectedBoardDeviceId":expected_board_device_id,"expectedBootId":boot});
            let state = self.send_asset_command_and_wait_timeout("asset/slot-query", &query, &id, "slot-query", None, None, ASSET_STAT_TIMEOUT)?;
            current()?;
            if state["queryNonce"].as_str()!=Some(id.as_str()) || state["boardDeviceId"].as_str()!=Some(expected_board_device_id)
                || state["bootId"].as_str()!=Some(boot.as_str()) || state["serialized"]!=true || state["quiescent"]!=true
                || state["observationSchema"].as_str()!=Some(appearance_recovery::SCHEMA)
                || state.get("errorCode").is_some() || state.get("proof").is_some()
                || !state["nativeIoEpoch"].as_u64().is_some_and(|n|n<u64::from(u32::MAX)) {
                return Err("audio-only current identity proof unavailable".into());
            }
            for key in ["snapshotChanged","assetTransferActive","rawReceiving","appearanceOwnerActive",
                "assetFileOpen","assetSlotPending","nativeTransferActive","nativeReceiving",
                "nativeWorkerPending","protocolWorkerPending","storageWorkerPending","firmwareOtaActive","rebootPending"] {
                if state[key].as_bool()!=Some(false) { return Err("audio-only device is not quiescent".into()); }
            }
            let rows=state["slots"].as_array().filter(|rows|rows.len()==2).ok_or("audio-only slot proof unavailable")?;
            let mut seen=HashSet::new();
            for row in rows {
                let n=row["slot"].as_u64().filter(|n|*n<=1).ok_or("audio-only invalid slot proof")?;
                if !seen.insert(n)||row["valid"].as_bool().is_none() {return Err("audio-only invalid slot proof".into());}
            }
            let slots = parse_p4_appearance_slot_state(&state)?;
            let active = slots.active_slot.and_then(|slot| slots.slots.iter().find(|s| s.slot==slot)).ok_or("audio-only current slot unavailable")?;
            // The active slot is readonly transport authority, not cue storage identity.
            let slot=slots.active_slot.filter(|n| *n<=1).ok_or("audio-only current slot unavailable")?;
            if active.pack_id.len()!=64 || !active.pack_id.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)) {
                return Err("audio-only current identity unavailable".into());
            }
            let global_dir=app_data_dir.join("global-audio-cues");
            let (files, _) = load_p4_audio_patch_files(&global_dir, app_data_dir, true)?;
            let total = p4_audio_patch_byte_count(&files)?;
            let metadata = files.iter().map(|f| serde_json::json!({"family":f.family,"path":f.path,
                "size":f.bytes.len(),"checksum":asset_checksum_hex(&f.bytes)})).collect::<Vec<_>>();
            let binding = || serde_json::json!({"transferId":id,
                "boardDeviceId":expected_board_device_id,"bootId":boot,"slot":slot,
                "packId":active.pack_id,"scope":"global"});
            let mut begin = binding();
            begin["files"] = serde_json::json!(metadata);
            begin["totalBytes"] = serde_json::json!(total);
            current()?;
            // New protocol only: original manifest/raw/builtin files remain
            // protected. Unknown results never reach a replay, Full or abort.
            let admitted = self.send_asset_command_and_wait("asset/audio-begin", &begin, &id, "audio-begin", None, None)?;
            let exact = |ack: &serde_json::Value, phase: &str| ack["ok"].as_bool()==Some(true)
                &&ack["transferId"].as_str()==Some(id.as_str())&&ack["phase"].as_str()==Some(phase)
                &&ack["boardDeviceId"].as_str()==Some(expected_board_device_id)&&ack["bootId"].as_str()==Some(boot.as_str())
                &&ack["slot"].as_u64()==Some(u64::from(slot))&&ack["packId"].as_str()==Some(active.pack_id.as_str());
            if !exact(&admitted,"audio-begin") { return Err("audio-only admission outcome unknown; no replay".into()); }
            p4_audio_patch_send_files(&files, |path, index, offset, chunk| {
                current()?;
                let mut payload=binding();
                payload["path"]=serde_json::json!(path);
                payload["index"]=serde_json::json!(index);
                payload["offset"]=serde_json::json!(offset);
                payload["data"]=serde_json::json!(base64::engine::general_purpose::STANDARD.encode(chunk));
                let ack=self.send_asset_command_and_wait("asset/audio-chunk",&payload,&id,"audio-chunk",Some(path),Some(index))?;
                let next=offset+chunk.len() as u64;
                if !exact(&ack,"audio-chunk")||ack["path"].as_str()!=Some(path)
                    ||ack["index"].as_u64()!=Some(u64::from(index))||ack["nextIndex"].as_u64()!=Some(u64::from(index)+1)
                    ||ack["receivedBytes"].as_u64()!=Some(next) {return Err("audio-only chunk outcome unknown; no replay".into());}
                Ok(())
            }, &on_progress)?;
            current()?;
            let mut commit=binding();
            commit["files"]=serde_json::json!(files.len());commit["totalBytes"]=serde_json::json!(total);
            let committed=self.send_asset_command_and_wait("asset/audio-commit",&commit,&id,"audio-commit",None,None)?;
            if !exact(&committed,"audio-commit")||committed["files"].as_u64()!=Some(files.len() as u64)
                ||committed["totalBytes"].as_u64()!=Some(total)
                ||!committed["audioGeneration"].as_u64().is_some_and(|n|n>0) {return Err("audio-only commit outcome unknown; no replay".into());}
            current()?;
            Ok((files.len() as u32,total,false))
        })
    }
}

fn sync_appearance_p4_native<F>(
    owner: &UsbSerialManager,
    appearance_dir: &std::path::Path,
    _app_data_dir: &std::path::Path,
    expected_board_device_id: &str,
    use_device_builtin: bool,
    cancel_requested: &AtomicBool,
    on_progress: F,
) -> Result<(u32, u64, bool), String>
where
    F: Fn(u32, u32, u64, u64),
{
    if cancel_requested.load(Ordering::SeqCst) {
        return Err(APPEARANCE_SYNC_CANCELLED_ERROR.to_string());
    }
    owner.check_appearance_connection()?;
    let mut transport = NativeUsbP4Transport::open(expected_board_device_id)?;
    transport.appearance_owner=Some(owner.clone());
    if use_device_builtin {
        let id = format!("builtin-{}", uuid::Uuid::new_v4());
        transport.send_json_value(&serde_json::json!({"topic":"asset/slot-query","payload":{"transferId":id}}))?;
        let ack = transport.wait_asset_ack_timeout(&id,"slot-query",None,cancel_requested,ASSET_STAT_TIMEOUT)?;
        if ack["builtinProtected"] != true {
            return Err("设备尚未声明受保护的内置形象，请先升级固件；未自动重传。".into());
        }
        let state = parse_p4_appearance_slot_state(&ack)?;
        let slot = p4_builtin_slot(&state)?;
        if state.active_slot != Some(slot.slot) {
            transport.send_json_value(&serde_json::json!({"topic":"asset/activate","payload":{"transferId":id,"slot":slot.slot,"packId":slot.pack_id}}))?;
            transport.wait_asset_ack_timeout(&id,"activate",None,cancel_requested,ASSET_ACK_TIMEOUT)?;
        }
        transfer_log::record("appearance","builtin_activated",serde_json::json!({"files":0,"bytes":0,"transport":"native-usb"}));
        on_progress(0,0,0,0);
        return Ok((0,0,true));
    }
    let assets = load_prepared_p4_appearance_pack(appearance_dir)?;
    if prepared_p4_requires_system_cues(appearance_dir)? {
        let id = format!("cue-check-{}", uuid::Uuid::new_v4());
        transport.send_json_value(&serde_json::json!({"topic":"asset/slot-query","payload":{"transferId":id}}))?;
        let ack = transport.wait_asset_ack_timeout(&id,"slot-query",None,cancel_requested,ASSET_STAT_TIMEOUT)?;
        if ack["systemCues"] != true {
            return Err("此形象使用公共提示音，请先升级设备固件至 0.7.63 或更新版本。".into());
        }
    }
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
    let native_raw_bytes=assets.iter().filter(|a|a.device_path.ends_with(".h264")||a.device_path.ends_with(".mjpg"))
        .try_fold(0u64,|sum,a|sum.checked_add(std::fs::metadata(&a.source_path).map_err(|_|"native raw metadata unavailable")?.len()).ok_or("native raw size overflow"))?;
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
                    ASSET_ACK_TIMEOUT,
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
                "rawBytes": native_raw_bytes,
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
        // Preserve unknown native state; no implicit abort/patch commit.
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
    bubble_renderer: Arc<Mutex<bubble_pixels::RenderBridge>>,
    bubble_worker: Arc<AtomicBool>,
    last_serial_baud: Arc<Mutex<Option<(String, u32)>>>,
    connection: Arc<Mutex<Option<UsbConnection>>>,
    status_snapshot: Arc<Mutex<UsbConnectionStatus>>,
    // Exact-generation validated HELLO retained across a busy writer lock.
    pending_hello: Arc<Mutex<Option<(u64, serde_json::Value)>>>,
    follow_scope: Arc<Mutex<Option<UsbFollowScope>>>,
    desktop_device_id: Arc<Mutex<String>>,
    connect_guard: Arc<Mutex<()>>,
    // Logical physical-frame reservation. Raw-ready may release connection/gate
    // for RX while retaining this lease against interleaved UART writes.
    wire_guard: Arc<Mutex<()>>,
    inbound_owner: Arc<inbound_owner::Owner>,
    appearance_attempt: Arc<Mutex<Option<Arc<appearance_safety::Attempt>>>>,
    #[cfg(test)]
    appearance_journal: Arc<appearance_safety::Journal>,
    asset_transfer_guard: Arc<Mutex<()>>,
    media_requests_pending: Arc<AtomicUsize>,
    appearance_sync_active: Arc<AtomicBool>,
    appearance_sync_cancel_requested: Arc<AtomicBool>,
    serial_body_write_active: Arc<AtomicBool>,
    next_connection_id: Arc<AtomicU64>,
    asset_ack_waiters: Arc<Mutex<Vec<AssetAckWaiter>>>,
    widget_ack_waiters: Arc<Mutex<Vec<WidgetAckWaiter>>>,
    firmware_ack_waiters: Arc<Mutex<Vec<FirmwareAckWaiter>>>,
    firmware_status_waiters: Arc<Mutex<Vec<FirmwareStatusWaiter>>>,
    device_response_waiters: Arc<Mutex<Vec<DeviceResponseWaiter>>>,
}
fn component_sync_media_blocks(state:&str)->bool{
    matches!(state,"playing"|"buffering"|"preparing"|"paused"|"ended")
}

// Physical-send admission, independent of the forwarder's cached profile.
// A queued old agent/session cannot overwrite a newly submitted follow.
struct UsbFollowScope {
    generation: u64,
    board: String,
    agent: String,
    session: Option<String>,
    submitted: bool,
}
impl UsbFollowScope {
    fn allows(&self, topic: &str, payload: &serde_json::Value) -> bool {
        let source = if let Some(source) = topic.strip_prefix("state/") {
            if source == "active" { payload["source"].as_str() } else { Some(source) }
        } else if topic == "session/current" {
            payload["agentId"].as_str()
        } else if topic == "speech/text" {
            payload["source"].as_str().or_else(|| payload["agentId"].as_str())
        } else { return true; };
        if !self.submitted || source.and_then(crate::normalize_agent_id).as_deref() != Some(self.agent.as_str()) {
            return false;
        }
        if topic == "session/current" { return true; }
        if let Some(session) = self.session.as_deref() {
            return payload["sessionId"].as_str() == Some(session);
        }
        true
    }
}

// Text is retained for bounded diagnostics, but lossy decoding must NEVER turn
// corrupted bytes inside a JSON string into an accepted protocol payload.
struct SerialFrame {
    text: String,
    valid_utf8: bool,
    integrity: control_integrity::Observation,
    #[cfg(test)]
    diagnostic_failure: Option<control_integrity_capture::Snapshot>,
}

fn parse_serial_frame(frame: &SerialFrame) -> Option<SerialMessage> {
    if !frame.valid_utf8 || frame.integrity.failure.is_some() { return None; }
    parse_serial_message(&frame.text)
}

fn read_serial_frame<R: BufRead>(
    reader: &mut R,
    buffer: &mut Vec<u8>,
) -> std::io::Result<Option<SerialFrame>> {
    // Keep incomplete frames across I/O timeouts, including during probing.
    // Bound both memory and work per call so noise cannot stall the probe loop.
    const MAX_SERIAL_LINE_BYTES: usize = 256 * 1024;
    const READ_SLICE: Duration = Duration::from_millis(100);
    let started = Instant::now();
    loop {
        let bytes = reader.fill_buf()?;
        if bytes.is_empty() {
            buffer.clear();
            return Ok(None);
        }
        let newline = bytes.iter().position(|byte| *byte == b'\n');
        let count = newline.map_or(bytes.len(), |index| index + 1);
        if buffer.len().saturating_add(count) > MAX_SERIAL_LINE_BYTES {
            buffer.clear();
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "serial frame exceeds size limit",
            ));
        }
        buffer.extend_from_slice(&bytes[..count]);
        reader.consume(count);
        if newline.is_some() {
            break;
        }
        if started.elapsed() >= READ_SLICE {
            return Err(std::io::ErrorKind::TimedOut.into());
        }
    }
    // Test-only evidence observes the complete physical line BEFORE trimming,
    // decoding or clearing it. It cannot supply bytes to the protocol parser.
    #[cfg(test)]
    let diagnostic_failure = control_integrity_capture::capture_candidate(buffer);
    while matches!(buffer.last(), Some(b'\n' | b'\r')) {
        buffer.pop();
    }
    let (body,integrity)=control_integrity::decode(buffer);
    let (text, valid_utf8) = match std::str::from_utf8(body) {
        Ok(text) => (text.to_owned(), true),
        Err(_) => (String::from_utf8_lossy(body).into_owned(), false),
    };
    buffer.clear();
    Ok(Some(SerialFrame { text, valid_utf8, integrity,
        #[cfg(test)] diagnostic_failure,
    }))
}

// Existing byte-fragment/timeout tests inspect the diagnostic text too. Real
// readers above/below always keep the original encoding verdict through parse.
#[cfg(test)]
fn read_serial_line_lossy<R: BufRead>(reader: &mut R, buffer: &mut Vec<u8>)
    -> std::io::Result<Option<String>> {
    read_serial_frame(reader, buffer).map(|frame| frame.map(|frame| frame.text))
}

fn log_raw_serial_line(line: &str) {
    #[cfg(test)]
    crate::acoustic_acceptance::observe_rejected_line(line);
    if std::env::var("P4_SERIAL_LOG_RAW").as_deref() != Ok("1") {
        return;
    }
    let line = line.trim_matches('\0').trim();
    if !line.is_empty() {
        eprintln!("[usb_serial/raw] {line}");
    }
}

// Only the firmware's fixed numeric widget timing report may cross the raw
// text boundary. Parse into numbers; never persist the source line or payload.
fn widget_numeric_trace(line: &str) -> Option<serde_json::Value> {
    if line.len() > 4096 { return None; }
    let mut text = line;
    if let Some(color) = text.strip_prefix("\x1b[") {
        let end = color.find('m')?;
        if end == 0 || end > 16 || !color[..end].bytes().all(|b| b.is_ascii_digit() || b == b';') {
            return None;
        }
        text = &color[end + 1..];
        text = text.strip_suffix("\x1b[0m").unwrap_or(text);
    }
    if let Some(log) = text.strip_prefix("I (") {
        let (stamp, body) = log.split_once(") pet-widget-trace: ")?;
        if stamp.is_empty() || stamp.len() > 20 || !stamp.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        text = body;
    }
    fn number(text: &str) -> Option<u64> {
        if text.is_empty() || text.len() > 20 || !text.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        text.parse().ok()
    }
    let mut fields = text.strip_prefix("P4 widget trace ")?.split(' ');
    let part = number(fields.next()?.strip_prefix("part=")?)?;
    let (first, last) = match part { 1 => (1, 10), 2 => (11, 20), 3 => (21, 22), _ => return None };
    let mut values = serde_json::Map::new();
    values.insert("part".into(), serde_json::json!(part));
    for name in ["lane", "seq", "kind", "start_us", "end_us", "ok", "bad", "rejected"] {
        let (key, value) = fields.next()?.split_once('=')?;
        if key != name { return None; }
        let value = number(value)?;
        if name == "ok" && value > 1 { return None; }
        values.insert(name.into(), serde_json::json!(value));
    }
    for index in first..=last {
        let (key, value) = fields.next()?.split_once('=')?;
        if key != format!("v{index}") { return None; }
        let mut triple = value.split(',');
        let count = number(triple.next()?)?;
        let sum = number(triple.next()?)?;
        let max = number(triple.next()?)?;
        if triple.next().is_some() || count > u64::from(u32::MAX) { return None; }
        values.insert(key.into(), serde_json::json!([count, sum, max]));
    }
    if fields.next().is_some() { return None; }
    Some(serde_json::Value::Object(values))
}

fn log_rejected_serial_line(line: &str, stats: &mut receive_diagnostics::Rejections,
    phase: &'static str, generation: u64, baud: u32) {
    if let Some(trace) = widget_numeric_trace(line) {
        transfer_log::record("widget", "device_numeric_trace", serde_json::json!({
            "phase": phase, "generation": generation, "baud": baud,
            "authenticated": false, "trace": trace,
        }));
        return;
    }
    if let Some(report) = stats.observe(line) {
        transfer_log::record("connection", "unparsed_line", serde_json::json!({
            "phase":phase,"generation":generation,"baud":baud,"kind":report.kind.label(),
            "occurrences":report.occurrences,"totalLines":report.total_lines,
            "decodedBytes":report.decoded_bytes,"nulChars":report.nul_chars,
            "replacementChars":report.replacement_chars
        }));
        if let Some(hint)=report.fault_hint {
            transfer_log::record("connection","device_fault_hint",serde_json::json!({
                "phase":phase,"generation":generation,"baud":baud,"kind":hint.kind.label(),
                "occurrences":hint.occurrences,"authenticated":false,"resetProven":false
            }));
        }
        if let Some(site)=report.abort_site {
            transfer_log::record("connection","device_abort_site",serde_json::json!({
                "phase":phase,"generation":generation,"baud":baud,"pc":site.pc,"core":site.core,
                "authenticated":false,"resetProven":false,"scope":"native_abort_site_only"
            }));
        }
    }
    // Existing explicit local raw-debug opt-in only; default logs contain none.
    log_raw_serial_line(line);
}

fn log_serial_integrity_failure(observation:control_integrity::Observation,reason:control_integrity::Failure,
    counts:&mut [u64;6],phase:&'static str,generation:u64,baud:u32){
    let count=&mut counts[reason.index()];*count=count.saturating_add(1);
    if count.is_power_of_two(){
        transfer_log::record("connection","control_integrity_rejected",serde_json::json!({
            "phase":phase,"generation":generation,"baud":baud,"kind":reason.label(),"occurrences":*count,
            "expectedBytes":observation.expected,"actualBytes":observation.actual,"framed":observation.framed,
            "byteScope":if observation.expected.is_some(){"body"}else{"physical_line"}
        }));
    }
}

fn validate_expected_board_device_id(expected_board_device_id: &str) -> Result<&str, String> {
    let expected_board_device_id = expected_board_device_id.trim();
    if expected_board_device_id.is_empty() {
        return Err("expectedBoardDeviceId is required".to_string());
    }
    Ok(expected_board_device_id)
}

// Scoped to this test thread. Release and ordinary tests retain the production
// retry policy. The capability harness uses this guard around ONE install call.
#[cfg(test)]
pub(crate) mod installer_acceptance {
    use serde::Serialize;
    use std::{cell::RefCell, collections::BTreeMap};
    #[derive(Default, Serialize)]
    #[serde(rename_all = "camelCase")]
    pub(crate) struct Chunk {
        pub attempts: u32,
        pub outcome: &'static str,
    }
    #[derive(Default, Serialize)]
    #[serde(rename_all = "camelCase")]
    pub(crate) struct Receipt {
        pub configured_max_attempts: usize,
        pub chunks: BTreeMap<String, Chunk>,
    }
    impl Receipt {
        pub(crate) fn single_attempts_only(&self) -> bool {
            self.configured_max_attempts == 1 && self.chunks.values().all(|c| c.attempts == 1)
        }
    }
    thread_local! {static STATE:RefCell<Option<Receipt>>=const{RefCell::new(None)};}
    pub(crate) fn limit(default: usize) -> usize {
        STATE.with(|s| if s.borrow().is_some() { 1 } else { default })
    }
    pub(crate) fn attempt(transfer: &str, path: &str, index: u32) {
        STATE.with(|s| {
            if let Some(state) = s.borrow_mut().as_mut() {
                let chunk = state
                    .chunks
                    .entry(format!("{transfer}:{path}:{index}"))
                    .or_default();
                chunk.attempts += 1;
                chunk.outcome = "submitted_unknown";
            }
        });
    }
    pub(crate) fn outcome(transfer: &str, path: &str, index: u32, value: &'static str) {
        STATE.with(|s| {
            if let Some(state) = s.borrow_mut().as_mut() {
                if let Some(chunk) = state.chunks.get_mut(&format!("{transfer}:{path}:{index}")) {
                    chunk.outcome = value;
                }
            }
        });
    }
    pub(crate) fn single_attempt<T>(operation: impl FnOnce() -> T) -> (T, Receipt) {
        struct Reset;
        impl Drop for Reset {
            fn drop(&mut self) {
                STATE.with(|s| {
                    s.replace(None);
                });
            }
        }
        STATE.with(|s| {
            assert!(s.borrow().is_none(), "nested installer probe");
            s.replace(Some(Receipt {
                configured_max_attempts: 1,
                chunks: BTreeMap::new(),
            }));
        });
        let reset = Reset;
        let result = operation();
        let receipt = STATE.with(|s| s.take().expect("installer probe missing"));
        drop(reset);
        (result, receipt)
    }
    pub(crate) fn missing_chunk_ack_probe() -> (Result<(u32, u64), String>, Receipt, Vec<String>) {
        use super::*;
        struct Writer {
            pending: Vec<u8>,
            waiters: Arc<Mutex<Vec<WidgetAckWaiter>>>,
            topics: Arc<Mutex<Vec<String>>>,
        }
        impl Write for Writer {
            fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
                self.pending.extend_from_slice(bytes);
                while let Some(end) = self.pending.iter().position(|b| *b == b'\n') {
                    let row = self.pending.drain(..=end).collect::<Vec<_>>();
                    let request: serde_json::Value = serde_json::from_slice(&row).unwrap();
                    let topic = request["topic"].as_str().unwrap();
                    self.topics.lock().unwrap().push(topic.into());
                    if topic == "widget/begin" {
                        resolve_widget_ack(
                            &self.waiters,
                            "widget-install-ack",
                            &serde_json::json!({"transferId":request["payload"]["transferId"],"phase":"begin","ok":true}),
                        );
                    } else if topic == "widget/chunk" {
                        // Unknown delivery: the matching waiter is disconnected,
                        // not ACKed. Same production error branch as timeout.
                        self.waiters.lock().unwrap().clear();
                    } else {
                        panic!("unexpected post-unknown mutation {topic}");
                    }
                }
                Ok(bytes.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
        let manager = UsbSerialManager::new();
        let topics = Arc::new(Mutex::new(Vec::new()));
        *manager.connection.lock().unwrap() = Some(UsbConnection {
            connection_id: 1,
            port_name: "test-only-fake-writer-no-port".into(),
            baud_rate: 115200,
            writer: Box::new(Writer {
                pending: Vec::new(),
                waiters: manager.widget_ack_waiters.clone(),
                topics: topics.clone(),
            }),
            board_device_id: "test-board".into(),
            runtime: "esp-p4".into(),
            device_model: String::new(),
            firmware: String::new(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 7,
            wire_protocol: String::new(),
            capabilities: serde_json::json!({}),
            connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)),
            capture_active: Arc::new(AtomicBool::new(false)),
        });
manager.activate_memory_test_connection();
        let tmp = tempfile::tempdir().unwrap();
        std::fs::create_dir(tmp.path().join("runtime")).unwrap();
        std::fs::write(tmp.path().join("runtime/widget.json"), b"{}").unwrap();
        let (result, receipt) = single_attempt(|| {
            manager.install_widget_clawpkg_on_board(
                "test-board",
                "test-fixture",
                tmp.path(),
                &std::collections::HashMap::new(),
                |_, _, _| {},
            )
        });
        let sent = topics.lock().unwrap().clone();
        (result, receipt, sent)
    }
}


#[cfg(test)]
pub(crate) fn configure_acceptance_transfer_log(directory: &std::path::Path) {
    transfer_log::configure(directory).expect("configure private acceptance metadata log");
}

impl UsbSerialManager {
    // Memory transports must publish the same dispatch generation as connect().
    // This does not register fake replies or bypass send/retirement checks.
    #[cfg(test)]
    fn activate_memory_test_connection(&self) {
        let (_dispatch, connection) = self.inbound_owner.connection(&self.connection).unwrap();
        let conn = connection.as_ref().expect("memory test connection");
        assert!(conn.connected && conn.connection_id != 0);
        assert!(!conn.cancel_reader.load(Ordering::Acquire));
        self.inbound_owner.publish(conn.connection_id);
        cache_usb_status(&self.status_snapshot, connection.as_ref());
    }

    pub fn new() -> Self {
        Self {
            bubble_renderer: Arc::new(Mutex::new(bubble_pixels::RenderBridge::default())),
            bubble_worker: Arc::new(AtomicBool::new(false)),
            last_serial_baud: Arc::new(Mutex::new(None)),
            connection: Arc::new(Mutex::new(None)),
            status_snapshot: Arc::new(Mutex::new(disconnected_usb_status())),
            pending_hello: Arc::new(Mutex::new(None)),
            follow_scope: Arc::new(Mutex::new(None)),
            desktop_device_id: Arc::new(Mutex::new(String::new())),
            connect_guard: Arc::new(Mutex::new(())),
            wire_guard: Arc::new(Mutex::new(())),
            inbound_owner: Arc::new(inbound_owner::Owner::default()),
            appearance_attempt: Arc::new(Mutex::new(None)),
            #[cfg(test)]
            appearance_journal: Arc::new(appearance_safety::Journal::memory()),
            asset_transfer_guard: Arc::new(Mutex::new(())),
            media_requests_pending: Arc::new(AtomicUsize::new(0)),
            appearance_sync_active: Arc::new(AtomicBool::new(false)),
            appearance_sync_cancel_requested: Arc::new(AtomicBool::new(false)),
            serial_body_write_active: Arc::new(AtomicBool::new(false)),
            next_connection_id: Arc::new(AtomicU64::new(0)),
            asset_ack_waiters: Arc::new(Mutex::new(Vec::new())),
            widget_ack_waiters: Arc::new(Mutex::new(Vec::new())),
            firmware_ack_waiters: Arc::new(Mutex::new(Vec::new())),
            firmware_status_waiters: Arc::new(Mutex::new(Vec::new())),
            device_response_waiters: Arc::new(Mutex::new(Vec::new())),
        }
    }

    // Caller owns connection; enqueue never holds pending_hello while acquiring
    // connection. Never block the RX reader behind a bulk writer.
    fn refresh_pending_hello(&self, c: &mut UsbConnection) -> Option<serde_json::Value> {
        let mut pending = self.pending_hello.lock().ok()?;
        let (generation, payload) = pending.as_ref()?;
        if *generation != c.connection_id || !self.inbound_owner.is_current(*generation) {
            if !self.inbound_owner.is_current(*generation) { *pending = None; }
            return None;
        }
        let payload = payload.clone();
        *pending = None;
        drop(pending);
        let next_board = first_json_string(&payload, &["boardDeviceId", "localDeviceId", "deviceId"]);
        let old_boot = c.capabilities["debugBootId"].as_str().unwrap_or("");
        let next_boot = payload["capabilities"]["debugBootId"].as_str().unwrap_or("");
        if (!next_board.is_empty() && next_board != c.board_device_id)
            || (!old_boot.is_empty() && old_boot != next_boot) {
            // A reboot is a new probe/connection generation, never a refresh of
            // an old mutation owner. No pending journal is cleared or replayed.
            self.inbound_owner.retire(c.connection_id);
            c.cancel_reader.store(true, Ordering::Release);
            c.connected = false;
            c.capabilities = serde_json::Value::Null;
            cache_usb_status(&self.status_snapshot, Some(c));
            return None;
        }
        apply_hello_payload_to_connection(c, &payload);
        cache_usb_status(&self.status_snapshot, Some(c));
        Some(payload)
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
        let _ = transfer_id;
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
        // No bind/probe/open against a possibly raw physical stream, including
        // aliases/newly enumerated ports after process restart. No TTL bypass.
        self.ensure_wire_available(port_name,None)?;
        // Serialize connect/disconnect operations across background auto-connect,
        // UI polling, and manual setup flow to avoid COM-port races.
        let _guard = self.connect_guard.lock().map_err(|e| e.to_string())?;

        {
            let conn = self.connection.lock().map_err(|e| e.to_string())?;
            if let Some(existing) = conn.as_ref() {
                if existing.connected && existing.port_name.eq_ignore_ascii_case(port_name)
                    && self.inbound_owner.is_current(existing.connection_id) {
                    return Ok(());
                }
            }
        }

        {
            let (_dispatch,mut conn) = self.inbound_owner.connection(&self.connection)?;
            if let Some(existing) = conn.as_mut() {
                existing.cancel_reader.store(true, Ordering::SeqCst);
                self.inbound_owner.retire(existing.connection_id);
            }
            *conn = None;
            cache_usb_status(&self.status_snapshot, None);
        }

        let probe_wire=self.wire_guard.lock().map_err(|_|"USB wire owner unavailable")?;
        self.ensure_wire_available(port_name,None)?;
        let detected_device = self
            .scan_devices()
            .into_iter()
            .find(|device| device.port_name.eq_ignore_ascii_case(port_name));
        self.ensure_probe_candidate(port_name,detected_device.as_ref())?;
        let probe_key = detected_device
            .as_ref()
            .map(|device| {
                format!(
                    "{}|{:04x}|{:04x}|{}",
                    device.port_name, device.vid, device.pid, device.serial_number
                )
            })
            .unwrap_or_else(|| port_name.to_string());
        let mut baud_candidates = serial_baud_candidates_for_device(detected_device.as_ref());
        if let Ok(cached) = self.last_serial_baud.lock() {
            if let Some((key, baud)) = cached.as_ref().filter(|(key, _)| key == &probe_key) {
                let _ = key;
                baud_candidates.retain(|candidate| candidate != baud);
                baud_candidates.insert(0, *baud);
            }
        }
        let desktop_id = self
            .desktop_device_id
            .lock()
            .map(|value| value.clone())
            .unwrap_or_default();
        let probed =
            match probe_serial_port(port_name, &baud_candidates, &desktop_id, probe_timeout) {
                Ok(probed) => probed,
                Err(error) => {
                    transfer_log::record(
                        "connection",
                        "failed",
                        serde_json::json!({"port": port_name, "error": error}),
                    );
                    return Err(error);
                }
            };
        if let Ok(mut cached) = self.last_serial_baud.lock() {
            *cached = Some((probe_key, probed.baud));
        }
        transfer_log::record(
            "connection",
            "verified",
            serde_json::json!({
                "port": port_name, "baud": probed.baud,
                "firmware": first_json_string(&probed.hello.payload, &["fw", "firmware", "version"]),
            }),
        );
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
        let capture_active = Arc::new(AtomicBool::new(false));

        let mut connection = UsbConnection {
            connection_id,
            port_name: port_name.to_string(),
            baud_rate: probed.baud,
            writer: probed.writer,
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
            capture_active: Arc::clone(&capture_active),
        };
        apply_hello_payload_to_connection(&mut connection, &probed.hello.payload);
        let initial_hello = probed.hello.clone();
        let negotiated_control_integrity=control_integrity::required_from_hello(&initial_hello.payload);
        let negotiated_control_nonce=control_integrity::confirmed_nonce(&initial_hello.payload).map(str::to_owned);
        let reader_port = probed.reader;
        let reader_baud = probed.baud;

        {
            let (_dispatch,mut conn) = self.inbound_owner.connection(&self.connection)?;
            *conn = Some(connection);
            self.inbound_owner.publish(connection_id);
            cache_usb_status(&self.status_snapshot, conn.as_ref());
            drop(conn);
            drop(probe_wire);
            // No connection/waiter/physical-wire mutex is held during callback.
            on_message(initial_hello.topic, initial_hello.payload);
        }

        // The generic reader is also exercised with memory transports; no alternate parser.
        let reader_owner=self.clone();
        thread::spawn(move || reader_owner.read_connected(reader_port,connection_id,reader_baud,
            cancel_reader,capture_active,negotiated_control_integrity,negotiated_control_nonce,on_message));

        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn read_connected<R:BufRead,F:Fn(String,serde_json::Value)>(&self,mut reader:R,connection_id:u64,reader_baud:u32,
        cancel_reader:Arc<AtomicBool>,capture_active:Arc<AtomicBool>,negotiated_control_integrity:bool,
        negotiated_control_nonce:Option<String>,on_message:F) {
        let conn_ref=&self.connection;
        let inbound_owner=&self.inbound_owner;
        let status_snapshot=&self.status_snapshot;
        let desktop_id_ref=&self.desktop_device_id;
        let asset_ack_waiters=&self.asset_ack_waiters;
        let widget_ack_waiters=&self.widget_ack_waiters;
        let firmware_ack_waiters=&self.firmware_ack_waiters;
        let firmware_status_waiters=&self.firmware_status_waiters;
        let device_response_waiters=&self.device_response_waiters;
        let serial_body_write_active=&self.serial_body_write_active;
        let bubble_ref=&self.bubble_renderer;
            let mut rejections = receive_diagnostics::Rejections::default();
            let mut integrity_rejections=[0u64;6];
            // Sticky for this exact connection generation; later unframed
            // HELLO/state can never downgrade it before resolving any waiter.
            let mut require_control_integrity=negotiated_control_integrity;
            let mut line_buffer = Vec::with_capacity(32 * 1024);
            let mut read_failure=None;
            let mut last_hello_ack = None;
            loop {
                if cancel_reader.load(Ordering::SeqCst) {
                    break;
                }
                // Retry publication even if the board never sends another
                // HELLO. This is metadata only; never resend a business frame.
                {
                    let Ok(_lease) = inbound_owner.enter(&cancel_reader, connection_id) else { break; };
                    let published = conn_ref.try_lock().ok().and_then(|mut conn|
                        conn.as_mut().filter(|c| c.connection_id == connection_id)
                            .and_then(|c| self.refresh_pending_hello(c)));
                    if let Some(payload) = published { on_message("hello".into(), payload); }
                }
                if serial_body_write_active.load(Ordering::Acquire) {
                    thread::sleep(Duration::from_millis(1));
                    continue;
                }
                let frame = match read_serial_frame(&mut reader, &mut line_buffer) {
                    Ok(Some(line)) => line,
                    Ok(None) => { read_failure=Some(("read_eof","end_of_stream",None)); break; },
                    Err(e) if serial_read_recovery_delay(&e).is_some() => {
                        if cancel_reader.load(Ordering::SeqCst) {
                            break;
                        }
                        let delay = serial_read_recovery_delay(&e).unwrap();
                        if !delay.is_zero() { thread::sleep(delay); }
                        continue;
                    }
                    Err(e) => {
                        read_failure=Some(("read_error",live_audio_pacing::io_kind(&e),e.raw_os_error()));
                        eprintln!("[usb_serial] read error: {} (kind={:?})", e, e.kind());
                        break;
                    }
                };

                let _dispatch = match inbound_owner.enter(&cancel_reader,connection_id) {
                    Ok(guard)=>guard,Err(_)=>break,
                };
                let line = &frame.text;
                if line.trim().is_empty() {
                    continue;
                }

                let mut msg = match parse_serial_frame(&frame) {
                    Some(message) => message,
                    None => {
                        #[cfg(test)]
                        if let Some(snapshot) = &frame.diagnostic_failure {
                            let outcome = control_integrity_capture::save(snapshot, connection_id, reader_baud);
                            transfer_log::record("connection", "diagnostic_failed_frame_capture",
                                serde_json::json!({"outcome":outcome,"generation":connection_id,
                                    "mode":"diagnostics-structure-v1","originalPayloadRetained":false}));
                        }
                        if let Some(reason)=frame.integrity.failure.or_else(||(frame.integrity.framed&&!frame.valid_utf8).then_some(control_integrity::Failure::Utf8)) {
                            log_serial_integrity_failure(frame.integrity,reason,&mut integrity_rejections,"connected",connection_id,reader_baud);
                        }
                        log_rejected_serial_line(&line, &mut rejections, "connected", connection_id, reader_baud);
                        continue;
                    }
                };

                if msg.topic=="hello" {
                    let claims=control_integrity::required_from_hello(&msg.payload);
                    let nonce_matches=match negotiated_control_nonce.as_deref(){
                        Some(nonce)=>control_integrity::hello_matches_probe(&msg.payload,frame.integrity.framed,nonce)
                            &&control_integrity::confirmed_nonce(&msg.payload)==Some(nonce),
                        None=>msg.payload.get("capabilities").and_then(|v|v.get("serialControlOffer")).is_none(),
                    };
                    if !nonce_matches || claims!=frame.integrity.framed || (require_control_integrity&&!claims){
                        log_serial_integrity_failure(frame.integrity,control_integrity::Failure::Negotiation,&mut integrity_rejections,"connected",connection_id,reader_baud);
                        if inbound_owner::requires_reprobe(&msg.payload, frame.integrity.framed, negotiated_control_nonce.as_deref()) {
                            // Never accept this HELLO as identity/ACK or downgrade.
                            // Retire only this generation and let the connection owner
                            // open a fresh probe nonce; do not replay business requests.
                            inbound_owner.retire(connection_id);
                            cancel_reader.store(true,Ordering::Release);
                            if let Ok(mut conn)=conn_ref.try_lock() {
                                if let Some(c)=conn.as_mut().filter(|c|c.connection_id==connection_id) {
                                    c.connected=false;cache_usb_status(&status_snapshot,Some(c));
                                }
                            }
                            break;
                        }
                        continue;
                    }
                    require_control_integrity|=claims;
                }
                if !control_integrity::permitted(frame.integrity,require_control_integrity,&msg.topic){
                    log_serial_integrity_failure(frame.integrity,control_integrity::Failure::Missing,&mut integrity_rejections,"connected",connection_id,reader_baud);
                    continue;
                }

                resolve_asset_ack(&asset_ack_waiters, &msg.topic, &msg.payload);
                observe_serial_capture(&capture_active, &msg.topic, &msg.payload);
                if msg.topic=="audio/begin" || (msg.topic=="media/event"&&matches!(msg.payload["operation"].as_str(),Some("play"|"resume"|"select"|"stop"))){
                    let board=status_snapshot.try_lock().ok().map(|s|s.board_device_id.clone()).unwrap_or_default();
                    crate::program_autostart::native::preempt_rx(&board,connection_id);
                }
                resolve_widget_ack(&widget_ack_waiters, &msg.topic, &msg.payload);
                resolve_firmware_ack(&firmware_ack_waiters, &msg.topic, &msg.payload);
                resolve_firmware_status(&firmware_status_waiters, &msg.topic, &msg.payload);
                resolve_device_response(&device_response_waiters, &msg.topic, &msg.payload);

                // Handle hello -> retain validated identity and send ack. Never block the
                // reader behind an in-flight bulk write: send_inner holds this
                // mutex while flushing, and the board cannot finish consuming
                // that write if its TX path is back-pressured by an unread
                // hello/asset ACK. Pending publication survives this frame.
                if msg.topic == "hello" {
                    if let Err(error) = validate_hello_message(&mut msg) {
                        eprintln!("[usb_serial] ignored invalid hello: {error}");
                        continue;
                    }
                    let scope_changed = status_snapshot.lock().map(|old| {
                        let board = first_json_string(&msg.payload, &["boardDeviceId", "localDeviceId", "deviceId"]);
                        let boot = msg.payload["capabilities"]["debugBootId"].as_str().unwrap_or("");
                        old.connection_id == connection_id.to_string()
                            && ((!board.is_empty() && old.board_device_id != board)
                                || (!old.boot_id.is_empty() && old.boot_id != boot))
                    }).unwrap_or(true);
                    if scope_changed {
                        // Retire before resolving any later buffered ACK, even
                        // if a writer currently owns the connection mutex.
                        inbound_owner.retire(connection_id);
                        cancel_reader.store(true, Ordering::Release);
                        if let Ok(mut cached) = status_snapshot.lock() { *cached = disconnected_usb_status(); }
                        break;
                    }
                    if let Ok(mut pending) = self.pending_hello.lock() {
                        *pending = Some((connection_id, msg.payload.clone()));
                    } else { continue; }
                    let desktop_id = desktop_id_ref.lock().map(|d| d.clone()).unwrap_or_default();
                    let Some(ack_payload)=control_integrity::ack_payload(&desktop_id,negotiated_control_nonce.as_deref()) else { continue; };
                    let ack = format!("{}\n", serde_json::json!({"topic":"ack", "payload":ack_payload}));
                    let mut published = false;
                    if let Ok(mut conn) = conn_ref.try_lock() {
                        if let Some(ref mut c) = *conn {
                            if c.connection_id == connection_id {
                                published = self.refresh_pending_hello(c).is_some();
                                if !c.connected { break; }
                                if let Ok(status)=status_snapshot.try_lock(){crate::program_autostart::native::connected_status(&status,connection_id);}
                                let now = Instant::now();
                                if hello_ack_due(last_hello_ack, now, &desktop_id) {
                                    if let Ok(_wire)=self.wire_guard.try_lock(){
                                        if self.ensure_wire_available(&c.port_name,Some(&c.board_device_id)).is_ok()&&c.writer.write_all(ack.as_bytes()).is_ok(){
                                            last_hello_ack=Some(now);
                                            if !capture_active.load(Ordering::Acquire){let _=flush_serial_writer(c.writer.as_mut());}
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // UI must not see a new firmware/HELLO while native still
                    // holds old capabilities. Reader/status will publish later.
                    if !published { continue; }
                }

                if msg.topic=="ui/text-pixels/need" {
                    let delivery=conn_ref.try_lock().ok().and_then(|conn|conn.as_ref().and_then(|c|{
                        if c.connection_id!=connection_id||c.capabilities["bubblePixelsBoot"]!=msg.payload["boot"]{return None;}
                        let mut bridge=bubble_ref.lock().ok()?;
                        Some((bridge.sink(),bridge.need(&msg.payload,connection_id,&c.board_device_id)))
                    }));
                    if let Some((Some(sink),events))=delivery{for event in events{sink(event);}}
                }
                if msg.topic.starts_with("audio/") || msg.topic=="protocol/ack" {
                    // Replace any device-provided copy. Only the admitted
                    // reader may attach physical connection provenance.
                    let scope = status_snapshot.lock().ok().and_then(|status| {
                        (status.connected && status.connection_id == connection_id.to_string())
                            .then(|| serde_json::json!({
                                "boardDeviceId": status.board_device_id,
                                "connectionId": status.connection_id,
                                "bootId": status.boot_id,
                                "runtime": status.runtime,
                            }))
                    });
                    let Some(scope) = scope else { continue; };
                    let Some(payload) = msg.payload.as_object_mut() else { continue; };
                    payload.insert("_nativeUsbScope".into(), scope);
                }
                on_message(msg.topic, msg.payload);
            }

            // Reader thread ended -> mark only its own generation disconnected.
            inbound_owner.retire(connection_id);
            #[cfg(test)] lifecycle_safety_tests::pause_before_disconnect();
            let mut current_connection=false;
            if let Ok(mut conn) = conn_ref.lock() {
                if let Some(ref mut c) = *conn {
                    if c.connection_id == connection_id {
                        current_connection=true;
                        c.connected = false;
                        cache_usb_status(&status_snapshot, Some(c));
                    }
                }
            }
            if current_connection && !cancel_reader.load(Ordering::SeqCst) {
                if let Some((reason,error_kind,os_error))=read_failure {
                    let details=serde_json::json!({"reason":reason,"errorKind":error_kind,"osError":os_error,
                        "generation":connection_id,"ok":false});
                    transfer_log::record("connection","serial_read_ended",details.clone());
                    crate::realtime_chat_log::record("","serial_read_ended",details);
                }
            }
    }

    /// Disconnect from USB serial device
    pub fn disconnect(&self) {
        if let Ok(_guard) = self.connect_guard.lock() {
            let Ok((_dispatch,mut conn)) = self.inbound_owner.connection(&self.connection) else { return; };
            if let Some(existing) = conn.as_mut() {
                existing.cancel_reader.store(true, Ordering::SeqCst);
                self.inbound_owner.retire(existing.connection_id);
            }
            *conn = None;
            if let Ok(mut bridge)=self.bubble_renderer.lock(){bridge.clear_peer();}
            cache_usb_status(&self.status_snapshot, None);
        }
    }

    /// Send a message to the device
    pub fn send(&self, topic: &str, payload: &serde_json::Value) -> Result<(), String> {
        self.send_inner(topic, payload, true, None, None, None)
    }

    pub(crate) fn set_bubble_renderer(
        &self,
        instance: &str,
        sink: Option<bubble_pixels::RenderSink>,
    ) -> bool {
        self.bubble_renderer
            .lock()
            .map(|mut bridge| bridge.set_sink(instance, sink))
            .unwrap_or(false)
    }
    pub(crate) fn reserve_bubble_worker(
        &self,
        frame: &BubbleFrame,
    ) -> Result<bubble_pixels::WorkerLease, String> {
        if frame.data.len() > 43692 {
            return Err("BUBBLE_FALLBACK: encoded limit".into());
        }
        if !self
            .bubble_renderer
            .lock()
            .map_err(|_| "BUBBLE_FALLBACK: lifecycle")?
            .current(frame)
        {
            return Err("BUBBLE_FALLBACK: stale content".into());
        }
        bubble_pixels::WorkerLease::acquire(self.bubble_worker.clone())
    }
    pub(crate) fn send_bubble_pixels(&self, frame: BubbleFrame) -> Result<(), String> {
        let bytes = base64::engine::general_purpose::STANDARD
            .decode(&frame.data)
            .map_err(|_| "BUBBLE_FALLBACK: base64")?;
        bubble_pixels::validate_frame(&frame, &bytes)?;
        let status = self.status();
        if status.capabilities["bubblePixels"] != bubble_pixels::SCHEMA {
            return Err("BUBBLE_FALLBACK: unsupported peer".into());
        }
        let connection_id = frame
            .connection_id
            .parse::<u64>()
            .map_err(|_| "BUBBLE_FALLBACK: connection")?;
        let rate = bubble_pixels::wire_rate(status.baud_rate);
        if bubble_pixels::estimated_wire_bytes(bytes.len())
            > rate * bubble_pixels::DEADLINE_MS as usize / 1000
        {
            return Err("BUBBLE_FALLBACK: asset cannot fit wire budget".into());
        }
        let deadline = Instant::now() + Duration::from_millis(bubble_pixels::DEADLINE_MS);
        let transfer = uuid::Uuid::new_v4().simple().to_string();
        let mut next_write = Instant::now();
        let mut phase = |name: &str,
                         extra: serde_json::Value|
         -> Result<serde_json::Value, String> {
            while Instant::now() < next_write {
                if Instant::now() >= deadline
                    || !self
                        .bubble_renderer
                        .lock()
                        .map_err(|_| "BUBBLE_FALLBACK: lifecycle")?
                        .current(&frame)
                {
                    return Err("BUBBLE_FALLBACK: retired".into());
                }
                thread::sleep(
                    next_write
                        .saturating_duration_since(Instant::now())
                        .min(Duration::from_millis(10)),
                );
            }
            if Instant::now() >= deadline {
                return Err("BUBBLE_FALLBACK: deadline".into());
            }
            let request_id = format!("bubble-{}", uuid::Uuid::new_v4());
            let mut payload = serde_json::json!({"phase":name,"requestId":request_id,"boot":frame.boot_id,
                "client":frame.renderer_id,"owner":frame.owner,"version":frame.version,"transfer":transfer,
                "id":frame.id,"style":frame.style});
            if let Some(extra) = extra.as_object() {
                for (k, v) in extra {
                    payload[k] = v.clone();
                }
            }
            let cost = serde_json::to_vec(
                &serde_json::json!({"topic":"ui/text-pixels","payload":payload}),
            )
            .map_err(|_| "BUBBLE_FALLBACK: JSON")?
            .len()
                + 1;
            if cost > 1536 {
                return Err("BUBBLE_FALLBACK: frame budget".into());
            }
            let (sender, receiver) = mpsc::channel();
            self.device_response_waiters
                .lock()
                .map_err(|_| "BUBBLE_FALLBACK: waiter")?
                .push(DeviceResponseWaiter {
                    request_id: request_id.clone(),
                    response_topic: "ui/text-pixels/status".into(),
                    sender,
                });
            let result = (|| {
                // One FRAME owns the low-priority guard. It is released before
                // token sleep or ACK wait, allowing media/OTA between frames.
                {
                    if self.media_requests_pending.load(Ordering::SeqCst) != 0 {
                        return Err("BUBBLE_FALLBACK: media pending".into());
                    }
                    let _guard = self
                        .asset_transfer_guard
                        .try_lock()
                        .map_err(|_| "BUBBLE_FALLBACK: transfer contention")?;
                    self.send_inner(
                        "ui/text-pixels",
                        &payload,
                        false,
                        Some(&frame.board_device_id),
                        Some(connection_id),
                        None,
                    )?;
                }
                next_write = Instant::now() + Duration::from_secs_f64(cost as f64 / rate as f64);
                let wait = deadline
                    .saturating_duration_since(Instant::now())
                    .min(Duration::from_millis(1000));
                let reply = receiver
                    .recv_timeout(wait)
                    .map_err(|_| "BUBBLE_FALLBACK: unknown ACK; no replay")?;
                for key in [
                    "phase", "boot", "client", "owner", "version", "transfer", "id",
                ] {
                    if reply[key] != payload[key] {
                        return Err("BUBBLE_FALLBACK: stale ACK".into());
                    }
                }
                if reply["ok"] != true {
                    return Err("BUBBLE_FALLBACK: device declined".into());
                }
                Ok(reply)
            })();
            self.remove_device_response_waiter(&request_id, "ui/text-pixels/status");
            result
        };
        let result = (|| {
            let begin = phase(
                "begin",
                serde_json::json!({"pageSizes":frame.page_sizes,"pageIndex":frame.page_index,
                "pageCount":frame.page_count,"checksum":frame.checksum,"totalBytes":bytes.len()}),
            )?;
            if begin["cached"] == true {
                return Ok(());
            }
            if begin["offset"] != 0 {
                return Err("BUBBLE_FALLBACK: invalid READY".into());
            }
            for (index, chunk) in bytes.chunks(bubble_pixels::CHUNK_BYTES).enumerate() {
                let reply = phase(
                    "chunk",
                    serde_json::json!({"offset":index*bubble_pixels::CHUNK_BYTES,
                    "data":base64::engine::general_purpose::STANDARD.encode(chunk)}),
                )?;
                if reply["offset"] != index * bubble_pixels::CHUNK_BYTES + chunk.len() {
                    return Err("BUBBLE_FALLBACK: offset ACK".into());
                }
            }
            phase("end", serde_json::json!({}))?;
            Ok(())
        })();
        // No uncertain image/frame replay. Superseding text invalidates its
        // device stage; otherwise the bounded idle/absolute timeout retires it.
        result
    }

    /// Low-priority snapshots never interleave with firmware/widget/appearance transfers.
    pub(crate) fn send_widget_data(&self, board: &str, payload: &serde_json::Value) -> Result<(), String> {
        if self.media_requests_pending.load(Ordering::SeqCst) != 0 {
            return Err("音乐正在传输，稍后更新组件数据".into());
        }
        let _guard = self.asset_transfer_guard.try_lock().map_err(|_| "设备正在传输，稍后更新行情")?;
        if self.media_requests_pending.load(Ordering::SeqCst) != 0 {
            return Err("音乐正在传输，稍后更新组件数据".into());
        }
        self.send_to_board(board, "widget/data", payload)
    }
    pub(crate) fn send_app_snapshot(&self,board:&str,payload:&serde_json::Value)->Result<(),String>{
        // App controls need feedback while their own audio is playing. Wait for
        // a chunk boundary, never for an entire stream; do not replay the action.
        // This is called only by a host worker, never by the serial reader.
        if serde_json::to_vec(payload).map_err(|e|e.to_string())?.len()>2048{return Err("应用快照过大".into());}
        let _guard=media_arbitration::acquire(&self.asset_transfer_guard,Duration::from_millis(250)).map_err(|_|"设备传输繁忙，等待下一次状态更新")?;
        self.send_to_board(board,"app/snapshot",payload)
    }
    pub(crate) fn try_send_app_snapshot(&self,board:&str,payload:&serde_json::Value)->Result<(),String>{
        // Reader callbacks must not wait for media ACKs delivered by that reader.
        if serde_json::to_vec(payload).map_err(|e|e.to_string())?.len()>2048{return Err("应用快照过大".into());}
        let _guard=self.asset_transfer_guard.try_lock().map_err(|_|"设备资源同步中")?;
        self.send_to_board(board,"app/snapshot",payload)
    }

    /// Send without flush - for streaming bulk data.
    fn send_no_flush(&self, topic: &str, payload: &serde_json::Value) -> Result<(), String> {
        self.send_inner(topic, payload, false, None, None, None)
    }

    pub(crate) fn send_owned_notes_control(&self,board:&str,generation:u64,owner:&str,action:&str)->Result<(),String>{
        if generation==0||owner.is_empty()||owner.len()>64||!owner.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))
            ||!matches!(action,"start"|"stop"){return Err("invalid notes control owner".into());}
        self.send_inner("audio/control",&serde_json::json!({"action":action,"captureOwner":owner,
            "requestId":format!("notes-{owner}-{action}")}),true,Some(board),Some(generation),None)
    }
    // Approval ownership is bound to a physical connection generation; no
    // decoder, scheduling or generic transport behavior changes here.
    pub(crate) fn approval_connection(&self) -> Option<(String, u64)> {
        let conn = self.connection.lock().ok()?;
        let c = conn.as_ref()?;
        if !c.connected || c.board_device_id.is_empty() { return None; }
        Some((c.board_device_id.clone(), c.connection_id))
    }
    pub(crate) fn send_approval_bound(&self, board: &str, generation: u64, topic: &str, payload: &serde_json::Value) -> Result<(), String> {
        self.send_inner(topic, payload, true, Some(board), Some(generation), None)
    }

    pub(crate) fn send_audio_bridge_command_for_connection(
        &self,
        expected_board_device_id: &str,
        expected_connection_id: u64,
        payload: &serde_json::Value,
    ) -> Result<(), String> {
        if expected_connection_id == 0 || payload["type"] != "audio_bridge"
            || !matches!(payload["action"].as_str(), Some("start" | "stop"))
        {
            return Err("invalid connection-bound audio bridge command".into());
        }
        self.send_inner("control/command", payload, true,
            Some(expected_board_device_id), Some(expected_connection_id), None)
    }

    // Callback-safe metadata only: never wait for a writer/connection mutex.
    pub(crate) fn session_follow_target_is_current(&self, board: &str, generation: &str, agent: &str) -> bool {
        // Every holder only inspects/updates metadata; none performs IO or UI
        // work under this mutex. Contention alone must not retire valid speech.
        let Ok(scope) = self.follow_scope.lock() else { return false; };
        scope.as_ref().filter(|scope| scope.generation.to_string() == generation && scope.board == board)
            .is_none_or(|scope| scope.allows("session/current", &serde_json::json!({"agentId":agent})))
    }

    pub(crate) fn check_session_follow_target(
        &self, board: &str, agent: &str, expected_connection: &str, expected_boot: &str,
    ) -> Result<u64, String> {
        let mut connection = self.connection.lock().map_err(|_| "session connection unavailable")?;
        let conn = connection.as_mut().ok_or("Not connected")?;
        self.refresh_pending_hello(conn);
        if !conn.connected || conn.cancel_reader.load(Ordering::Acquire)
            || !self.inbound_owner.is_current(conn.connection_id)
            || conn.board_device_id != board
            || (!expected_connection.is_empty() && expected_connection != conn.connection_id.to_string())
            || (!expected_boot.is_empty() && conn.capabilities["debugBootId"].as_str() != Some(expected_boot))
        { return Err("USB session scope changed; binding not committed".into()); }
        let scope = self.follow_scope.lock().map_err(|_| "follow scope unavailable")?;
        if scope.as_ref().filter(|scope| scope.generation == conn.connection_id && scope.board == board)
            .is_some_and(|scope| !scope.allows("session/current", &serde_json::json!({"agentId":agent})))
        { return Err("stale follow agent/session; binding not committed".into()); }
        Ok(conn.connection_id)
    }

    pub(crate) fn send_follow_change_for_connection(
        &self, board: &str, generation: u64, topic: &str, payload: &serde_json::Value,
    ) -> Result<(), String> {
        if generation == 0 || !(topic == "control/remote-cli-binding" || topic.starts_with("state/")) {
            return Err("invalid follow change scope".into());
        }
        self.send_inner(topic, payload, true, Some(board), Some(generation), None)
    }

    pub(crate) fn send_session_current_for_connection(
        &self, board: &str, generation: u64, payload: &serde_json::Value,
    ) -> Result<(), String> {
        if generation == 0 { return Err("invalid session connection".into()); }
        self.send_inner("session/current", payload, true, Some(board), Some(generation), None)
    }

    pub(crate) fn send_to_board(
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
            None,
            None,
        )
    }

    fn send_firmware_to_board(
        &self,
        expected_board_device_id: &str,
        expected_connection_id: u64,
        topic: &str,
        payload: &serde_json::Value,
    ) -> Result<(), String> {
        self.send_inner(
            topic,
            payload,
            true,
            Some(expected_board_device_id),
            Some(expected_connection_id),
            Some(P4_CH343_SERIAL_WRITE_SLICE_BYTES),
        )
    }

    fn send_inner(
        &self,
        topic: &str,
        payload: &serde_json::Value,
        flush: bool,
        expected_board_device_id: Option<&str>,
        expected_connection_id: Option<u64>,
        write_slice_bytes: Option<usize>,
    ) -> Result<(), String> {
        self.send_inner_admitted(topic,payload,flush,expected_board_device_id,expected_connection_id,write_slice_bytes,None)
    }
    fn send_inner_admitted(
        &self,topic:&str,payload:&serde_json::Value,flush:bool,
        expected_board_device_id:Option<&str>,expected_connection_id:Option<u64>,write_slice_bytes:Option<usize>,program_epoch:Option<u64>,
    )->Result<(),String>{
        let owned_notes=topic=="audio/control"&&expected_connection_id.is_some()&&payload.get("captureOwner").is_some();
        let preempt_on_send = !owned_notes&&((topic=="audio/conversation"&&payload["enabled"]==true)
            ||(topic=="audio/control"&&payload["action"]=="start")
            ||topic=="audio/play_begin"||topic=="media/begin"||topic=="media/play"||topic=="media/resume"||topic=="widget/program-stop");
        if topic.starts_with("asset/") {appearance_safety::command_id(topic,payload)?;}
        let appearance = if topic.starts_with("asset/") { self.active_appearance_attempt()? } else { None };
        // Only an exact-board, generation-bound volume command may retain the
        // inbound dispatch lease across its write, just like owned notes.
        let owned_volume=topic=="audio/volume"&&expected_board_device_id.is_some()&&expected_connection_id.is_some();
        let owned_control=owned_notes||owned_volume;
        let (_admission,_wire,mut connection_guard)=if appearance.is_some()||owned_control {
            // RX callbacks can submit an ordinary bounded notice. Do not own
            // their wire while waiting for the dispatch lease they still hold.
            let (lease,wire,connection)=self.inbound_owner.wire_connection(&self.wire_guard,&self.connection)?;
            (Some(lease),wire,connection)
        } else {
            let wire=self.wire_guard.lock().map_err(|_|"USB wire owner unavailable")?;
            (None,wire,self.connection.lock().map_err(|e|e.to_string())?)
        };
        let conn = connection_guard.as_mut().ok_or("Not connected")?;
        self.refresh_pending_hello(conn);
        if conn.cancel_reader.load(Ordering::Acquire) || !self.inbound_owner.is_current(conn.connection_id) {
            return Err("USB connection retired; request not submitted".into());
        }
        self.ensure_wire_available(&conn.port_name,Some(&conn.board_device_id))?;
        if owned_control&&(conn.cancel_reader.load(Ordering::Acquire)||!self.inbound_owner.is_current(conn.connection_id)){
            return Err("owned connection retired; control not submitted".into());
        }
        if let Some(attempt)=&appearance {
            self.check_appearance_locked(attempt,conn)?;
        } else if appearance_safety::mutation(topic) {
            self.ensure_unowned_asset_write(&conn.board_device_id)?;
        }
        if let Some(epoch)=program_epoch{
            if topic!="widget/program-start"||crate::program_autostart::native::admission()!=Some(epoch)||conn.capture_active.load(Ordering::Acquire)
                ||self.media_requests_pending.load(Ordering::Acquire)!=0{return Err("autostart admission changed".into());}
        }

        if !conn.connected {
            return Err("Connection lost".to_string());
        }
        if let Some(expected_connection_id) = expected_connection_id {
            if conn.connection_id != expected_connection_id {
                return Err(format!(
                    "USB connection changed during firmware update: expected session {expected_connection_id}, got {}",
                    conn.connection_id
                ));
            }
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

        let next_follow = if topic == "control/remote-cli-binding" && payload["enabled"] == true {
            Some(crate::normalize_agent_id(payload["targetSource"].as_str().unwrap_or(""))
                .ok_or("follow source is not recognized")?)
        } else { None };
        {
            let mut scope = self.follow_scope.lock().map_err(|_| "follow scope unavailable")?;
            if scope.as_ref().is_some_and(|s| s.generation != conn.connection_id || s.board != conn.board_device_id) {
                *scope = None;
            }
            if let Some(agent) = &next_follow {
                // A failed/unknown binding write leaves status forwarding closed;
                // only a new explicit binding submission can reopen it.
                *scope = Some(UsbFollowScope { generation: conn.connection_id, board: conn.board_device_id.clone(),
                    agent: agent.clone(), session: None, submitted: false });
            } else if scope.as_ref().is_some_and(|s| !s.allows(topic, payload)) {
                return Err("stale follow agent/session; state not submitted".into());
            }
        }

        if topic=="audio/volume" {
            master_volume::validate_write(conn,expected_board_device_id,expected_connection_id,payload)?;
        }
        // Suppress only the connection admitted above. A rejected stale bound
        // request must not suppress the current board/boot's autostart policy.
        // This RX-safe callback never reacquires the held connection mutex.
        if preempt_on_send||(owned_notes&&payload["action"]=="start") {
            crate::program_autostart::native::preempt_rx(&conn.board_device_id,conn.connection_id);
        }
        // Background widgets must never delay duplex audio on the serial writer.
        // Their providers retain the newest snapshot and retry after capture ends.
        if topic == "widget/data" && conn.capture_active.load(Ordering::Acquire) {
            return Err("语音通道使用中，稍后更新组件数据".into());
        }
        let mut payload = payload.clone();
        let pixel_wire_guard=if topic=="ui/text-pixels" {
            if conn.capture_active.load(Ordering::Acquire)||self.media_requests_pending.load(Ordering::SeqCst)!=0
                ||self.appearance_sync_active.load(Ordering::Acquire)||conn.cancel_reader.load(Ordering::Acquire){
                return Err("BUBBLE_FALLBACK: protected transport".into());
            }
            let bridge=self.bubble_renderer.lock().map_err(|_|"BUBBLE_FALLBACK: lifecycle")?;
            let boot=conn.capabilities["bubblePixelsBoot"].as_str().unwrap_or("");
            if conn.capabilities["bubblePixels"]!=bubble_pixels::SCHEMA||!bridge.current_wire(&payload,conn.connection_id,&conn.board_device_id,boot){
                return Err("BUBBLE_FALLBACK: stale peer/content".into());
            }
            Some(bridge) // Linearize invalidation with this final physical write.
        }else{None};
        let mut bubble_events=Vec::new();let mut bubble_sink=None;
        if conn.capabilities["bubblePixels"]==bubble_pixels::SCHEMA&&bubble_pixels::is_text_topic(topic)&&payload.is_object(){
            let mut bridge=self.bubble_renderer.lock().map_err(|_|"BUBBLE_FALLBACK: lifecycle")?;
            let boot=conn.capabilities["bubblePixelsBoot"].as_str().unwrap_or("");
            bubble_events=bridge.observe(topic,&payload,conn.connection_id,&conn.board_device_id,boot);
            payload["textPixels"]=bridge.metadata(topic,boot);bubble_sink=bridge.sink();
        }
        if topic == "system/heartbeat" && conn.capabilities["widgetLocalTools"] == "p4-local-tools-v1" && payload.is_object() {
            let now=chrono::Local::now();
            payload["clock"]=serde_json::json!({"unixSeconds":now.timestamp(),"utcOffsetMinutes":now.offset().local_minus_utc()/60});
        }
        let msg = serde_json::json!({
            "topic": topic,
            "payload": payload,
        });

        let mut line = serde_json::to_string(&msg).map_err(|e| e.to_string())?;
        line.push('\n');

        let bytes = line.as_bytes();
        if let Some(attempt)=&appearance {
            if appearance_safety::mutation(topic) { attempt.before_mutation(topic,appearance_safety::command_id(topic,&payload)?)?; }
        }
        // The P4 CH343 link has no hardware flow control. Pace every outbound
        // frame at high baud, not only firmware/raw bodies: button-config JSON
        // and asset transaction headers can otherwise be truncated before the
        // board parser sees a newline, which presents to the UI as a missing
        // ACK and makes an identical second attempt appear to fix itself.
        let paced_slice_bytes = write_slice_bytes.filter(|size| *size > 0).or_else(|| {
            (conn.baud_rate >= P4_USB_UART_BAUD && conn.runtime.eq_ignore_ascii_case("esp-p4"))
                .then_some(P4_CH343_SERIAL_WRITE_SLICE_BYTES)
        });
        let audio_frame = topic.starts_with("audio/") || topic.starts_with("media/") || topic == "ui/conversation";
        if (topic == "audio/conversation" && payload.get("enabled").and_then(serde_json::Value::as_bool) == Some(true))
            || (topic == "audio/control" && payload.get("action").and_then(serde_json::Value::as_str) == Some("start")) {
            conn.capture_active.store(true, Ordering::Release);
        }
        let bulk_frame = topic.starts_with("asset/") || topic.starts_with("firmware/");
        // Replaceable widget snapshots share the audio-safe paced path. A
        // driver's blocking drain can otherwise hold the mutex indefinitely
        // even after media itself stopped using flush()/tcdrain().
        let live_frame = audio_frame || topic == "widget/data" || topic == "app/snapshot" || topic == "ui/text-pixels"
            || (!bulk_frame && conn.capture_active.load(Ordering::Acquire));
        // Heartbeats must keep RX running and recover only the unwritten
        // suffix, just like live frames. A busy device is not a dead handle.
        // Read-only diagnostics must also leave the physical RX reader running.
        let bounded_frame = serial_frame_is_bounded(topic, live_frame);
        let write_started = Instant::now();
        let mut failure_progress = None;
        if topic=="asset/activate" { self.activation_update(|t|t.write_started()); }
        let write_result = {
            let _pause = (!bounded_frame).then(|| SerialReaderPause::new(&self.serial_body_write_active));
            (|| {
                if bounded_frame {
                    let started=Instant::now();
                    let result=write_serial_live_frame(conn.writer.as_mut(), bytes,
                        paced_slice_bytes.unwrap_or(bytes.len()), live_audio_pacing::serial_frame_gap(topic, P4_CH343_SERIAL_WRITE_GAP));
                    let report=match &result {Ok(report)=>report,Err(error)=>&error.report};
                    if result.is_err() {
                        failure_progress = Some((report.bytes_written, report.retries, report.error_kind, report.os_error));
                    }
                    if result.is_err() || report.retries > 0 {
                        let stage=match topic {
                            "audio/play_chunk"=>"audio_chunk", "audio/play_begin"=>"audio_begin",
                            "audio/play_end"=>"audio_end", "audio/play_flush"=>"audio_flush",
                            "audio/conversation"=>"conversation", "ui/conversation"=>"conversation_ui",
                            "widget/data"=>"widget", "app/snapshot"=>"app_snapshot", "system/heartbeat"=>"heartbeat",
                            "diagnostics/query"=>"diagnostics_query", _=>"other_live",
                        };
                        let event=if result.is_ok() {"live_write_recovered"}else{"live_write_failed"};
                        // Never log JSON bodies, PCM, board IDs or driver text.
                        let details=serde_json::json!({"stage":stage,"ok":result.is_ok(),
                            "bytes":bytes.len(),"writtenBytes":report.bytes_written,"retries":report.retries,
                            "elapsedMs":started.elapsed().as_millis(),"errorKind":report.error_kind,
                            "osError":report.os_error,"generation":conn.connection_id});
                        transfer_log::record("connection",event,details.clone());
                        crate::realtime_chat_log::record(payload.get("sessionId").and_then(serde_json::Value::as_str).unwrap_or(""),event,details);
                    }
                    return result.map(|_|()).map_err(|error| format!("Live serial write failed: {error}"));
                }
                if let Some(write_slice_bytes) = paced_slice_bytes {
                    write_serial_bytes_paced(
                        conn.writer.as_mut(),
                        bytes,
                        write_slice_bytes,
                        P4_CH343_SERIAL_WRITE_GAP,
                    )
                    .map_err(|error| format!("Paced serial write failed: {error}"))
                } else {
                    conn.writer
                        .write_all(bytes)
                        .map_err(|error| format!("Write failed: {error}"))?;
                    if flush {
                        flush_serial_writer(conn.writer.as_mut())
                            .map_err(|error| format!("Flush failed: {error}"))?;
                    }
                    Ok(())
                }
            })()
        };
        if let Err(error) = write_result {
            if let Some(attempt)=&appearance { attempt.uncertain(); }
            // Only a classified zero-progress transient leaves framing intact.
            // Partial/unknown writes and hard IO errors still invalidate the
            // connection; never replay a failed logical frame or fake an ACK.
            let keep_connection = failure_progress.is_some_and(|(written, _, kind, _)|
                written == 0 && matches!(kind, Some("timeout" | "would_block" | "interrupted")));
            let details = serde_json::json!({
                "stage": if topic == "system/heartbeat" {"heartbeat"} else if live_frame {"live"} else if bulk_frame {"bulk"} else {"control"},
                "generation": conn.connection_id, "bytes": bytes.len(),
                "writtenBytes": failure_progress.map(|p| p.0),
                "retries": failure_progress.map(|p| p.1),
                "errorKind": failure_progress.and_then(|p| p.2).unwrap_or("write_progress_unknown"),
                "osError": failure_progress.and_then(|p| p.3),
                "elapsedMs": write_started.elapsed().as_millis(),
                "connectedAfter": keep_connection,
                "reason": if keep_connection {"zero_progress_transient"} else {"hard_or_partial_or_unknown_write"}
            });
            transfer_log::record("connection", "serial_write_failed", details.clone());
            crate::realtime_chat_log::record("", "serial_write_failed", details);
            if !keep_connection {
                conn.connected = false;
                conn.cancel_reader.store(true,Ordering::Release);
            }
            cache_usb_status(&self.status_snapshot, Some(conn));
            return Err(error);
        }

        if topic=="asset/activate" { self.activation_update(|t|t.write_completed()); }
        {
            let mut scope = self.follow_scope.lock().map_err(|_| "follow scope unavailable")?;
            if let Some(scope) = scope.as_mut().filter(|s| s.generation == conn.connection_id && s.board == conn.board_device_id) {
                if next_follow.is_some() { scope.submitted = true; }
                if topic == "session/current" {
                    scope.session = payload["sessionId"].as_str().filter(|id| !id.is_empty() && *id != "auto").map(str::to_owned);
                }
            }
        }
        drop(pixel_wire_guard);
        drop(connection_guard);
        drop(_admission);
        drop(_wire);
        if let Some(sink)=bubble_sink {for event in bubble_events {sink(event);}}

        Ok(())
    }

    /// Flush the writer
    pub fn flush(&self) -> Result<(), String> {
        let mut conn = self.connection.lock().map_err(|e| e.to_string())?;
        let conn = conn.as_mut().ok_or("Not connected")?;
        flush_serial_writer(conn.writer.as_mut())
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

    /// Read-only preflight: never stops capture/playback or retries an install.
    pub(crate) fn component_sync_audio_status(&self, board: &str) -> Result<serde_json::Value, String> {
        let (status, connection_id) = self.connected_board_snapshot(board)?;
        let capture_active = self.connection.lock().map_err(|e| e.to_string())?
            .as_ref().ok_or("USB 未连接")?.capture_active.load(Ordering::Acquire);
        if capture_active {
            return Ok(serde_json::json!({"blocked":true,"reason":"voice","message":"设备正在录音或实时语音中。请先结束语音，再重新检查同步；不会自动中断语音。"}));
        }
        let diagnostic = self.query_diagnostics(board)
            .map_err(|_| "无法确认设备音频状态，尚未开始同步；请稍后重新检查。".to_string())?;
        let playback = diagnostic["runtime"]["audioPlaybackActive"].as_bool()
            .ok_or("设备未返回可确认的播放状态；尚未开始同步，请更新固件或稍后重新检查。")?;
        let media = if status.capabilities["widgetMedia"] == "p4-media-v1" {
            self.media_request(board,"media/query",serde_json::json!({}))
                .map_err(|_| "无法确认后台播放状态，尚未开始同步；请停止播放后重新检查。".to_string())?
        } else { serde_json::json!({"state":"idle"}) };
        let state = media["state"].as_str().ok_or("设备播放状态不完整；尚未开始同步。")?;
        let blocked = playback || component_sync_media_blocks(state);
        let capture_now = self.connection.lock().map_err(|e|e.to_string())?
            .as_ref().ok_or("USB 未连接")?.capture_active.load(Ordering::Acquire);
        if capture_now { return Ok(serde_json::json!({"blocked":true,"reason":"voice","message":"设备正在录音或实时语音中。请先结束语音，再重新检查同步；不会自动中断语音。"})); }
        let (_, current_id) = self.connected_board_snapshot(board)?;
        if current_id != connection_id { return Err("设备连接已变化，请重新检查同步目标；尚未开始同步。".into()); }
        Ok(serde_json::json!({"blocked":blocked,"reason":if blocked {"playback"} else {"ready"},
            "message":if blocked {"设备仍在播放或准备音频。请先停止播放，再重新检查并同步；返回主屏不等于停止播放。"} else {"音频空闲"}}))
    }

    pub fn query_button_config(
        &self,
        expected_board_device_id: &str,
    ) -> Result<serde_json::Value, String> {
        self.with_asset_transfer_guard(|| {
            self.send_device_request_and_wait(
                expected_board_device_id,
                "input/config-query",
                "input/config-state",
            )
        })
    }

    pub(crate) fn transfer_in_progress(&self) -> bool {
        self.asset_transfer_guard.try_lock().is_err()
    }

    pub(crate) fn with_asset_transfer_guard<T>(
        &self,
        operation: impl FnOnce() -> Result<T, String>,
    ) -> Result<T, String> {
        let _transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        operation()
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

    fn background_program_supported(&self,board:&str)->Result<(),String>{
        let status=self.connected_board_status(board)?;
        if status.runtime!="esp-p4"||!crate::app_program_background::supported(&status.capabilities){
            return Err("当前设备未开放后台程序管理，未更改程序状态".into());
        }
        Ok(())
    }

    pub fn query_background_programs(&self,board:&str)->Result<crate::app_program_background::Snapshot,String>{
        let _guard=self.asset_transfer_guard.try_lock().map_err(|_|"设备传输忙，未查询后台程序")?;
        self.background_program_supported(board)?;
        let reply=self.send_device_request_and_wait(board,"widget/program-query","widget/program-status")?;
        crate::app_program_background::snapshot(&reply,board,None)
    }

    pub fn start_background_program(&self,board:&str,id:&str)->Result<crate::app_program_background::Outcome,String>{
        if !crate::app_program_background::valid_widget_id(id){return Err("组件 ID 无效，未启动程序".into());}
        let _guard=self.asset_transfer_guard.try_lock().map_err(|_|"设备传输忙，未启动后台程序")?;
        self.background_program_supported(board)?;
        // Inventory verifies installed resources. Never derive native checksums
        // from a local preview/version or a differently formatted JSON file.
        let inventory=self.send_device_request_and_wait(board,"widget/list","widget/inventory")?;
        if inventory["boardDeviceId"]!=board||inventory["ok"]!=true||inventory["complete"]!=true||inventory["recoveryBlocked"]!=false
            ||!inventory["items"].as_array().is_some_and(|rows|rows.len()<=32&&rows.iter().any(|row|row["id"]==id)){
            return Err("设备组件清单尚未确认，未启动程序，已有数据保留".into());
        }
        let inspected=self.send_device_payload_and_wait(board,"widget/program-inspect","widget/program-status",serde_json::json!({"widgetId":id}))?;
        let content=crate::app_program_background::installed(&inspected,board,id)?;
        let payload=crate::app_program_background::start_payload(&content);
        // Exactly one mutation submission. A lost/invalid/negative reply is
        // conservative unknown, never a second start or rollback/reset.
        Ok(match self.send_device_payload_and_wait(board,"widget/program-start","widget/program-status",payload){
            Ok(reply)=>crate::app_program_background::Outcome::started(&reply,board,&content),
            Err(_)=>crate::app_program_background::Outcome::unknown(),
        })
    }

    pub fn stop_background_program(&self,board:&str,id:&str,boot_id:&str,registration:&str)->Result<crate::app_program_background::Outcome,String>{
        if !crate::app_program_background::valid_widget_id(id){return Err("组件 ID 无效，未停止程序".into());}
        let payload=crate::app_program_background::stop_payload(id,boot_id,registration)?;
        crate::program_autostart::native::suppress_now(board,boot_id);
        let _guard=self.asset_transfer_guard.try_lock().map_err(|_|"设备传输忙，未停止后台程序")?;
        self.background_program_supported(board)?;
        // Never replace the user's observed owner with a newly queried serial.
        Ok(match self.send_device_payload_and_wait(board,"widget/program-stop","widget/program-status",payload){
            Ok(reply)=>crate::app_program_background::Outcome::stopped(&reply,board,id,boot_id,registration),
            Err(_)=>crate::app_program_background::Outcome::unknown(),
        })
    }

    fn send_device_request_and_wait(
        &self,
        expected_board_device_id: &str,
        request_topic: &str,
        response_topic: &str,
    ) -> Result<serde_json::Value, String> {
        self.send_device_payload_and_wait(expected_board_device_id,request_topic,response_topic,serde_json::json!({}))
    }

    pub fn activate_installed_widget(&self,board:&str,widget_id:&str,show:bool)->Result<serde_json::Value,String>{
        if !crate::component_extension::valid_id(widget_id){return Err("组件 ID 无效，未打开".into());}
        let _guard=self.asset_transfer_guard.try_lock().map_err(|_|"设备传输忙，未切换组件")?;
        let status=self.connected_board_status(board)?;
        if status.capabilities["widgetActivation"]!="p4-widget-activation-v1"{return Err("设备不支持直接打开已安装组件，请先升级固件；未覆盖安装".into());}
        self.send_device_payload_and_wait(board,"widget/activate","protocol/ack",serde_json::json!({"widgetId":widget_id,"show":show}))
    }

    fn send_device_payload_and_wait(&self,expected_board_device_id:&str,request_topic:&str,response_topic:&str,mut payload:serde_json::Value)->Result<serde_json::Value,String>{
        let expected_board_device_id = validate_expected_board_device_id(expected_board_device_id)?;
        let request_id = format!("device-request-{}", uuid::Uuid::new_v4());
        payload["requestId"]=request_id.clone().into();
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
            &payload,
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

    pub(crate) fn service_image_request(&self,board:&str,payload:serde_json::Value)->Result<serde_json::Value,String>{
        if self.status().capabilities["widgetServiceBridge"]!="p4-service-bridge-v1"{return Err("设备未声明服务桥图片能力".into());}
        if self.media_requests_pending.load(Ordering::Acquire)>0{return Err("音频传输优先，图片稍后更新".into());}
        let _guard=self.asset_transfer_guard.try_lock().map_err(|_|"资源传输繁忙，图片稍后更新")?;
        self.send_device_payload_and_wait(board,"app/image","protocol/ack",payload)
    }

    pub(crate) fn media_request(&self, board: &str, topic: &str, payload: serde_json::Value) -> Result<serde_json::Value,String> {
        self.media_request_scoped(board,None,topic,payload,false)
    }
    pub(crate) fn media_request_for_connection(&self, board:&str, generation:u64, topic:&str, payload:serde_json::Value)->Result<serde_json::Value,String> {
        if generation==0 {return Err("invalid radio connection scope".into());}
        self.media_request_scoped(board,Some(generation),topic,payload,false)
    }
    /// Return a correlated status even when the device rejected the command.
    /// Callers must validate typed terminal evidence; this is never a success ACK.
    pub(crate) fn media_status_for_connection(&self,board:&str,generation:u64,topic:&str,payload:serde_json::Value)->Result<serde_json::Value,String>{
        if generation==0{return Err("invalid radio connection scope".into());}
        self.media_request_scoped(board,Some(generation),topic,payload,true)
    }
    fn media_request_scoped(&self, board:&str, generation:Option<u64>, topic:&str, mut payload:serde_json::Value, return_rejection:bool)->Result<serde_json::Value,String> {
        let _priority = media_arbitration::PendingMedia::new(&self.media_requests_pending);
        let started = Instant::now();
        let guard = media_arbitration::acquire(&self.asset_transfer_guard, media_arbitration::WAIT_BUDGET);
        if started.elapsed() >= Duration::from_millis(2) || guard.is_err() {
            crate::realtime_chat_log::record("", "music_transport", serde_json::json!({
                "stage":"lock_wait", "elapsedMs":started.elapsed().as_millis(), "ok":guard.is_ok()
            }));
        }
        let _guard = guard?;
        let request_id=format!("media-{}",uuid::Uuid::new_v4());
        let (sender,receiver)=mpsc::channel();
        payload["requestId"]=serde_json::json!(request_id);
        self.device_response_waiters.lock().map_err(|_|"音乐回执队列不可用")?.push(DeviceResponseWaiter{request_id:request_id.clone(),response_topic:"media/status".into(),sender});
        if let Err(e)=self.send_inner(topic,&payload,true,Some(board),generation,None) {self.remove_device_response_waiter(&request_id,"media/status");return Err(e);}
        #[cfg(test)] let write_ms=started.elapsed().as_millis();
        let result=receiver.recv_timeout(Duration::from_secs(3));
        if started.elapsed() >= Duration::from_millis(150) || result.is_err() {
            crate::realtime_chat_log::record("", "music_transport", serde_json::json!({
                "stage":"response", "elapsedMs":started.elapsed().as_millis(), "ok":result.is_ok()
            }));
        }
        #[cfg(test)] if started.elapsed() >= Duration::from_millis(150) || result.is_err() || topic=="media/chunk" && payload["seq"].as_u64().is_some_and(|n|n<8) {eprintln!("media timing: topic={topic}, writeMs={write_ms}, totalMs={}, ok={}",started.elapsed().as_millis(),result.is_ok());}
        self.remove_device_response_waiter(&request_id,"media/status");
        let response=result.map_err(|_|"设备音乐响应超时，请检查连接及固件")?;
        if response["ok"]!=true {
            let message=response["message"].as_str().unwrap_or("设备拒绝音乐指令");
            let operation=match topic {"media/begin"=>"begin","media/chunk"=>"chunk","media/query"=>"query","media/stop"=>"stop","media/library"=>"library",_=>"other"};
            crate::realtime_chat_log::record("", "media_device_rejected", serde_json::json!({
                "stage":operation,"reason":media_arbitration::rejection_category(message),
                "seq":payload["seq"].as_u64(),"bufferedMs":response["bufferedMs"].as_u64(),
                "state":match response["state"].as_str(){Some(s @ ("idle"|"ended"|"error"|"interrupted"|"playing"|"buffering"|"paused"))=>s,_=>"unknown"},
                "count":match response["sessionId"].as_str(){Some("")=>0,Some(_)=>1,None=>2}
            }));
            if !return_rejection { return Err(message.into()); }
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

    fn query_firmware_status(
        &self,
        expected_board_device_id: &str,
        expected_connection_id: u64,
    ) -> Result<serde_json::Value, String> {
        let request_id = format!("firmware-status-{}", uuid::Uuid::new_v4());
        let (sender, receiver) = mpsc::channel();
        self.firmware_status_waiters
            .lock()
            .map_err(|error| error.to_string())?
            .push(FirmwareStatusWaiter {
                request_id: request_id.clone(),
                sender,
            });
        if let Err(error) = self.send_firmware_to_board(
            expected_board_device_id,
            expected_connection_id,
            "firmware/query",
            &serde_json::json!({ "requestId": request_id }),
        ) {
            self.remove_firmware_status_waiter(&request_id);
            return Err(error);
        }
        receiver
            .recv_timeout(P4_FIRMWARE_STATUS_TIMEOUT)
            .map_err(|_| {
                self.remove_firmware_status_waiter(&request_id);
                "firmware status query timed out".to_string()
            })
    }

    fn remove_firmware_status_waiter(&self, request_id: &str) {
        if let Ok(mut waiters) = self.firmware_status_waiters.lock() {
            waiters.retain(|waiter| waiter.request_id != request_id);
        }
    }

    fn ensure_firmware_transfer_idle(
        &self, expected_board_device_id: &str, expected_connection_id: u64,
    ) -> Result<(), String> {
        let status = self.query_firmware_status(expected_board_device_id, expected_connection_id)?;
        match status.get("active").and_then(serde_json::Value::as_bool) {
            Some(false) => Ok(()),
            Some(true) => Err("firmware transfer already active; no automatic abort or restart; read-only reconciliation required".into()),
            None => Err("firmware transfer state unknown; no mutation submitted".into()),
        }
    }

    pub(crate) fn firmware_chip_target(&self, expected_board_device_id: &str) -> Result<P4ChipTarget, String> {
        let status = self.connected_board_status(expected_board_device_id)?;
        if status.capabilities.pointer("/firmwareUpdate/chipRevision").is_some() {
            return chip_target_from_device(&status.capabilities, None);
        }
        let diagnostics = self.query_diagnostics(expected_board_device_id)?;
        self.connected_board_status(expected_board_device_id)?;
        chip_target_from_device(&status.capabilities, Some(&diagnostics))
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
        let mut ota_diagnostic = firmware_diagnostics::Diagnostic::new(false, record_firmware_failure_snapshot);
        let expected_board_device_id =
            validate_expected_board_device_id(expected_board_device_id)?.to_string();
        let _transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        let (status, firmware_connection_id) =
            self.connected_board_snapshot(&expected_board_device_id)?;
        if !status.runtime.eq_ignore_ascii_case("esp-p4") {
            return Err(format!(
                "firmware OTA is only supported by ESP32-P4, connected runtime is {}",
                status.runtime
            ));
        }
        // Validate the image before any abort, erase or upload side effect.
        let firmware = std::fs::read(firmware_path)
            .map_err(|error| format!("read firmware {}: {error}", firmware_path.display()))?;
        if firmware.is_empty() || firmware.len() > P4_FIRMWARE_MAX_IMAGE_SIZE {
            return Err(format!("firmware image must be 1..={} bytes, got {}",
                P4_FIRMWARE_MAX_IMAGE_SIZE, firmware.len()));
        }
        ota_diagnostic.snapshot.total_bytes = firmware.len() as u64;
        let descriptor = parse_esp_idf_app_descriptor(&firmware)?;
        let chip_range = firmware_chip_range(&firmware)?;
        let chip_target = self.firmware_chip_target(&expected_board_device_id)?;
        validate_firmware_chip(chip_range, chip_target)?;
        let actual_ota_status=self.query_firmware_status(&expected_board_device_id,firmware_connection_id)?;
        firmware_transaction::ensure_device_firmware_headroom(&actual_ota_status,firmware.len())?;
        self.ensure_firmware_transfer_idle(&expected_board_device_id, firmware_connection_id)?;
        let preferred_chunk_size =
            preferred_firmware_chunk_size(status.protocol_schema, &status.capabilities);
        let initial_chunk_size =
            initial_firmware_chunk_size(status.protocol_schema, preferred_chunk_size);
        // Never clear an unknown appearance transaction with a guessed ID.
        // firmware/begin remains authoritative about device resource ownership.
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
        transfer_log::record(
            "firmware",
            "transaction_started",
            serde_json::json!({
                "transferId": transfer_id.as_str(),
                "boardDeviceId": expected_board_device_id.as_str(),
                "sourceFirmware": status.firmware.as_str(),
                "targetFirmware": descriptor.version.as_str(),
                "protocolSchema": status.protocol_schema,
                "initialChunkBytes": initial_chunk_size,
                "chunkBytes": preferred_chunk_size,
                "bytes": total_bytes,
            }),
        );
        on_progress(0, total_bytes, "begin");

        let begin_payload = serde_json::json!({
            "transferId": transfer_id,
            "size": total_bytes,
            "sha256": sha256,
            "chipRevisionMin": chip_range.min,
            "chipRevisionMax": chip_range.max,
        });
        ota_diagnostic.request(firmware_diagnostics::Phase::Begin, Some(0), 0, 0, 1);
        let begin_ack = self.begin_firmware_transfer(
            &expected_board_device_id,
            firmware_connection_id,
            &begin_payload,
            &transfer_id,
        )?;
        ota_diagnostic.ack(firmware_diagnostics::Phase::Begin, 0, 0);
        let target_partition = begin_ack
            .get("targetPartition")
            .and_then(|value| value.as_str())
            .filter(|value| !value.is_empty())
            .ok_or_else(|| {
                "firmware begin acknowledgement did not include targetPartition".to_string()
            })?
            .to_string();

        let upload_started = Instant::now();
        let mut upload_attempts = 0usize;
        let mut upload_corruption_rejections = 0usize;
        let mut upload_fallbacks = 0usize;
        let transaction_attempt = 1usize;
        let transfer_result = (|| {
                let mut offset = 0usize;
                let mut sequence = 0u64;
                let mut chunk_size = initial_chunk_size;
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
                        ota_diagnostic.request(firmware_diagnostics::Phase::Chunk, Some(sequence), offset as u64, received_bytes, attempt as u64);
                        chunk_result = self.send_firmware_command_and_wait(
                            &expected_board_device_id,
                            firmware_connection_id,
                            "firmware/chunk",
                            &payload,
                            &transfer_id,
                            "chunk",
                            sequence + 1,
                            received_bytes,
                            P4_FIRMWARE_CHUNK_ACK_TIMEOUT,
                        );
                        if chunk_result.is_ok() {
                            ota_diagnostic.ack(firmware_diagnostics::Phase::Chunk, sequence + 1, received_bytes);
                            break;
                        }
                        if matches!(
                            chunk_result.as_ref().unwrap_err(),
                            FirmwareCommandError::Rejected(message)
                                if message == "firmware chunk base64 mismatch"
                        ) {
                            upload_corruption_rejections += 1;
                            corruption_rejections += 1;
                            if corruption_rejections
                                >= P4_FIRMWARE_CORRUPTION_RETRIES_BEFORE_FALLBACK
                            {
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
                            "[usb-firmware-ota] failed chunk seq={} attempt={}/{} error={}",
                            sequence,
                            attempt,
                            P4_FIRMWARE_CHUNK_MAX_ATTEMPTS,
                            chunk_result.as_ref().unwrap_err()
                        );
                        transfer_log::record(
                            "firmware",
                            "chunk_attempt_failed",
                            serde_json::json!({
                                "transferId": transfer_id.as_str(),
                                "transactionAttempt": transaction_attempt,
                                "seq": sequence,
                                "offset": offset,
                                "chunkBytes": chunk_size,
                                "attempt": attempt,
                                "error": chunk_result.as_ref().unwrap_err().to_string(),
                            }),
                        );
                        if !firmware_chunk_error_is_retryable(chunk_result.as_ref().unwrap_err()) {
                            break;
                        }
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
                    "[usb-firmware-ota] upload complete transaction_attempt={} bytes={} chunks={} attempts={} corruption_rejections={} fallbacks={} elapsed_ms={}",
                    transaction_attempt,
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
                transfer_log::record(
                    "firmware",
                    "transaction_failed",
                    serde_json::json!({
                        "transferId": transfer_id.as_str(),
                        "error": error.as_str(),
                    }),
                );
                return Err(error);
            }
        };

        on_progress(total_bytes, total_bytes, "verify");
        let commit_payload = serde_json::json!({ "transferId": transfer_id });
        ota_diagnostic.request(firmware_diagnostics::Phase::Commit, Some(expected_next_sequence), total_bytes, total_bytes, 1);
        let commit_ack = self.send_firmware_command_and_wait(
            &expected_board_device_id, firmware_connection_id, "firmware/commit",
            &commit_payload, &transfer_id, "commit", expected_next_sequence,
            total_bytes, P4_FIRMWARE_COMMIT_ACK_TIMEOUT,
        ).map_err(|error| {
            transfer_log::record("firmware", "transaction_stopped_no_replay",
                serde_json::json!({"transferId": transfer_id.as_str(), "phase":"commit", "error":error.to_string()}));
            format!("firmware commit failed: {error}; no replay or automatic abort; read-only reconciliation required")
        })?;
        ota_diagnostic.ack(firmware_diagnostics::Phase::Commit, expected_next_sequence, total_bytes);
        transfer_log::record("firmware", "commit_acknowledged",
            serde_json::json!({"transferId":transfer_id.as_str(),"attempt":1,"ack":commit_ack}));
        on_progress(total_bytes, total_bytes, "reboot");

        ota_diagnostic.snapshot.phase = firmware_diagnostics::Phase::Validate;
        ota_diagnostic.snapshot.stage = firmware_diagnostics::Stage::ValidateBoot;
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
        )
        .map_err(|error| {
            transfer_log::record(
                "firmware",
                "transaction_failed",
                serde_json::json!({
                    "transferId": transfer_id.as_str(),
                    "phase": "validate",
                    "error": error.as_str(),
                }),
            );
            error
        })?;

        transfer_log::record(
            "firmware",
            "transaction_committed",
            serde_json::json!({
                "transferId": transfer_id.as_str(),
                "version": descriptor.version.as_str(),
                "partition": verified.partition.as_str(),
                "bytes": total_bytes,
                "attempts": upload_attempts,
                "elapsedMs": upload_started.elapsed().as_millis(),
            }),
        );

        ota_diagnostic.finish(firmware_diagnostics::Outcome::Success);
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
        self.connected_board_snapshot(expected_board_device_id)
            .map(|(status, _)| status)
    }

    fn connected_board_snapshot(
        &self,
        expected_board_device_id: &str,
    ) -> Result<(UsbConnectionStatus, u64), String> {
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
        let connection_id = conn.connection_id;
        Ok((usb_connection_status(conn),connection_id))
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
        // Native USB often keeps its serial handle across an application reboot.
        // Closing/reopening it immediately after commit toggles DTR/RTS and can
        // interrupt the new image before its 8-second health check completes.
        // Keep the working reader; only reopen after a real disconnect/error,
        // with time for delayed reboot, initialization and health validation.
        let mut force_reconnect = false;
        let mut last_observation = "waiting for the device to reboot".to_string();

        while started.elapsed() < P4_FIRMWARE_RECONNECT_TIMEOUT {
            // Leave the boot self-test a quiet window, while the reader keeps
            // draining boot output. Diagnostics are multi-kilobyte replies;
            // polling them during validation can delay render/RX health samples.
            if started.elapsed() < Duration::from_secs(20) {
                on_progress(total_bytes, total_bytes, "reboot");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
            let status = self.status();
            if force_reconnect
                || !status.connected
                || status.board_device_id != expected_board_device_id
            {
                force_reconnect = false;
                if let Err(error) = reconnect() {
                    transfer_log::record(
                        "firmware",
                        "validation_reconnect_failed",
                        serde_json::json!({ "boardDeviceId": expected_board_device_id, "error": error.as_str() }),
                    );
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

                        transfer_log::record(

                            "firmware",

                            "validation_pending",

                            serde_json::json!({

                                "boardDeviceId": expected_board_device_id,

                                "expectedVersion": expected_version,

                                "baselineBootCount": baseline_boot_count,

                                "diagnostics": diagnostics.clone(),

                            }),

                        );
                    }
                    Err(error) => {
                        transfer_log::record(
                            "firmware",
                            "validation_rejected",
                            serde_json::json!({
                                "boardDeviceId": expected_board_device_id,
                                "error": error.as_str(),
                                "diagnostics": diagnostics.clone(),
                            }),
                        );
                        return Err(error);
                    }
                },
                Err(error) => {
                    transfer_log::record(
                        "firmware",
                        "validation_query_failed",
                        serde_json::json!({ "boardDeviceId": expected_board_device_id, "error": error.as_str() }),
                    );
                    last_observation = error;
                    force_reconnect = true;
                }
            }
            thread::sleep(Duration::from_millis(500));
        }

        let error = format!(
            "timed out waiting for board {expected_board_device_id} to reconnect with firmware {expected_version} in imageState=valid: {last_observation}"
        );
        transfer_log::record(
            "firmware",
            "validation_timeout",
            serde_json::json!({
                "boardDeviceId": expected_board_device_id,
                "expectedVersion": expected_version,
                "lastObservation": last_observation.as_str(),
            }),
        );
        Err(error)
    }

    fn begin_firmware_transfer(
        &self, expected_board_device_id: &str, expected_connection_id: u64,
        begin_payload: &serde_json::Value, transfer_id: &str,
    ) -> Result<serde_json::Value, String> {
        self.begin_firmware_transfer_once(expected_board_device_id, expected_connection_id,
            begin_payload, transfer_id, firmware_begin_wait::BEGIN_ACK_TIMEOUT)
    }

    // Per-call timeout is a test seam, not a global override or retry policy.
    fn begin_firmware_transfer_once(
        &self, expected_board_device_id: &str, expected_connection_id: u64,
        begin_payload: &serde_json::Value, transfer_id: &str, timeout: Duration,
    ) -> Result<serde_json::Value, String> {
        self.send_firmware_command_and_wait(expected_board_device_id, expected_connection_id,
            "firmware/begin", begin_payload, transfer_id, "begin", 0, 0, timeout)
            .map_err(|error| format!("firmware begin failed: {error}; no replay or automatic abort; read-only reconciliation required"))
    }

    #[allow(clippy::too_many_arguments)]
    fn send_firmware_command_and_wait(
        &self,
        expected_board_device_id: &str,
        expected_connection_id: u64,
        topic: &str,
        payload: &serde_json::Value,
        transfer_id: &str,
        phase: &str,
        expected_next_sequence: u64,
        expected_received_bytes: u64,
        timeout: Duration,
    ) -> Result<serde_json::Value, FirmwareCommandError> {
        let mut diagnostic = firmware_diagnostics::Diagnostic::new(true, record_firmware_failure_snapshot);
        let diagnostic_phase = firmware_diagnostics::Phase::from_command(phase);
        let diagnostic_size = if diagnostic_phase == firmware_diagnostics::Phase::Chunk {
            payload.get("decodedSize").and_then(serde_json::Value::as_u64).unwrap_or(0)
        } else { 0 };
        diagnostic.request(diagnostic_phase,
            if diagnostic_phase == firmware_diagnostics::Phase::Chunk { expected_next_sequence.checked_sub(1) } else { Some(expected_next_sequence) },
            expected_received_bytes.saturating_sub(diagnostic_size), expected_received_bytes, 0);
        diagnostic.snapshot.stage = firmware_diagnostics::Stage::RegisterWaiter;
        let result = (|| {
        let receiver = self
            .register_firmware_ack_waiter(
                transfer_id,
                phase,
                expected_next_sequence,
                expected_received_bytes,
            )
            .map_err(FirmwareCommandError::Send)?;
        diagnostic.snapshot.stage = firmware_diagnostics::Stage::Write;
        if let Err(error) = self.send_firmware_to_board(
            expected_board_device_id,
            expected_connection_id,
            topic,
            payload,
        ) {
            self.remove_firmware_ack_waiter(
                transfer_id,
                phase,
                expected_next_sequence,
                expected_received_bytes,
            );
            return Err(FirmwareCommandError::Send(error));
        }
        diagnostic.snapshot.stage = firmware_diagnostics::Stage::WaitAck;
        let ack_result = if phase == "begin" {
            firmware_begin_wait::wait(&receiver, timeout,
                || self.inbound_owner.is_current(expected_connection_id))
                .map_err(|error| match error {
                    firmware_begin_wait::WaitError::Timeout => FirmwareCommandError::Timeout { phase: phase.to_string() },
                    firmware_begin_wait::WaitError::ConnectionChanged => FirmwareCommandError::Send(
                        "firmware begin connection retired before ACK; result unknown; no replay".into()),
                    firmware_begin_wait::WaitError::ChannelClosed => FirmwareCommandError::Send(
                        "firmware begin ACK channel closed; result unknown; no replay".into()),
                })
        } else {
            receiver.recv_timeout(timeout).map_err(|_| FirmwareCommandError::Timeout { phase: phase.to_string() })
        };
        let ack = ack_result.map_err(|error| {
            self.remove_firmware_ack_waiter(
                transfer_id, phase, expected_next_sequence, expected_received_bytes,
            );
            error
        })?;
        diagnostic.snapshot.stage = firmware_diagnostics::Stage::ValidateAck;
        match ack.get("ok").and_then(serde_json::Value::as_bool) {
            Some(true) => Ok(ack),
            Some(false) => {
                let message = ack.get("error").or_else(|| ack.get("message"))
                    .and_then(serde_json::Value::as_str).unwrap_or("device rejected firmware OTA command");
                if message == "firmware chunk base64 mismatch"
                    && (phase != "chunk" || !firmware_base64_rejection_unapplied(
                        &ack, payload, expected_next_sequence, expected_received_bytes))
                {
                    return Err(FirmwareCommandError::Send(
                        "firmware rejection does not prove unchanged progress; result unknown; no replay".into()));
                }
                Err(FirmwareCommandError::Rejected(message.to_string()))
            },
            // A malformed result is not a known NACK. In particular its free
            // text must never authorize a chunk replay or base64 downshift.
            None => Err(FirmwareCommandError::Send(
                format!("firmware {phase} acknowledgement has missing or invalid ok; result unknown; no replay"),
            )),
        }
        })();
        diagnostic.finish(match &result {
            Ok(_) => firmware_diagnostics::Outcome::Acknowledged,
            Err(FirmwareCommandError::Send(_)) => firmware_diagnostics::Outcome::SendError,
            Err(FirmwareCommandError::Timeout { .. }) => firmware_diagnostics::Outcome::AckTimeout,
            Err(FirmwareCommandError::Rejected(_)) => firmware_diagnostics::Outcome::Rejected,
        });
        result
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

    #[cfg(test)]
    fn best_effort_asset_abort(&self, transfer_id: &str) {
        let status = self.status();
        let needs_legacy_release = status.runtime.eq_ignore_ascii_case("esp-p4")
            && status
                .capabilities
                .get("transport")
                .and_then(|value| value.get("rawAssetChunks"))
                .and_then(|value| value.as_bool())
                != Some(true);
        transfer_log::record(
            "appearance",
            "abort_requested",
            serde_json::json!({
                "transferId": transfer_id,
                "legacyRelease": needs_legacy_release,
            }),
        );
        let _ = self.send(
            "asset/abort",
            &serde_json::json!({ "transferId": transfer_id }),
        );
        // Older P4 firmware predates asset/abort. Its zero-file patch commit
        // safely releases asset_transfer_active without switching the active
        // slot; never use this compatibility release on Linux boards because
        // they could apply a partially staged audio patch.
        if needs_legacy_release {
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
            // A failed operation is not evidence that its last mutation failed.
            self.appearance_uncertain();
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
        if !appearance_safety::valid_transfer_id(transfer_id){return Err("invalid appearance transferId".into());}
        let appearance=self.active_appearance_attempt()?.ok_or("appearance transaction owner required")?;
        let send_started=Instant::now();
        let mut ready_ack=None;
        let mut ready_elapsed_ms=0u128;
        let mut raw_write_elapsed_ms=0u128;
        let mut physical_header_submitted=false;
        let mut terminal_raw_proven=false;
        // Reserve only the physical stream across raw-ready. RX, lifecycle and
        // state queries remain free; no dispatch/connection lock waits for ACK.
        let wire=self.wire_guard.lock().map_err(|_|"USB wire owner unavailable")?;
        let send_result=(||{
            {
                let (_lease,mut slot)=self.appearance_admission(&appearance)?;
                let connection=slot.as_mut().ok_or("Not connected")?;
                self.ensure_wire_available(&connection.port_name,Some(&connection.board_device_id))?;
                appearance.before_mutation("asset/raw-chunk",transfer_id)?;
                appearance.raw_intent(&connection.port_name,self.raw_usb_identity(&connection.port_name))?;
                physical_header_submitted=true;
                let _pause=SerialReaderPause::new(&self.serial_body_write_active);
                if let Err(error)=write_serial_bytes_paced(connection.writer.as_mut(),&line,
                    P4_CH343_SERIAL_WRITE_SLICE_BYTES,P4_CH343_SERIAL_WRITE_GAP){
                    connection.connected=false;connection.cancel_reader.store(true,Ordering::Release);
                    cache_usb_status(&self.status_snapshot,Some(connection));
                    return Err(format!("Raw asset paced header write failed: {error}"));
                }
            }
            if let Some(receiver)=ready_receiver.as_ref(){
                let ready=receiver.recv_timeout(P4_RAW_ASSET_ACK_TIMEOUT).map_err(|error|match error{
                    mpsc::RecvTimeoutError::Timeout=>format!("timed out waiting for raw P4 asset chunk readiness: transferId={transfer_id} path={path} index={index}"),
                    mpsc::RecvTimeoutError::Disconnected=>"P4 raw asset readiness channel closed".to_string(),
                })?;
                let ok=ready.get("ok").and_then(|v|v.as_bool())==Some(true);
                ready_ack=Some(ready);ready_elapsed_ms=send_started.elapsed().as_millis();
                if !ok{
                    let ready=ready_ack.as_ref().ok_or("raw-ready receipt missing")?;
                    {
                        // This branch precedes every raw body write. Only an
                        // exact current typed idle proof can release raw-intent.
                        let (_lease,slot)=self.appearance_admission(&appearance)?;
                        let connection=slot.as_ref().ok_or("appearance connection missing")?;
                        let boot=connection.capabilities["debugBootId"].as_str().unwrap_or("");
                        if appearance_safety::raw_ready_rejected_idle(ready,transfer_id,path,index,&connection.board_device_id,boot){
                            appearance.raw_confirmed()?;
                            terminal_raw_proven=true; // Parser boundary only; NOT operation success.
                        }
                    }
                    transfer_log::record("appearance","raw_ready_rejected",serde_json::json!({
                        "errorCode":appearance_safety::raw_ready_error_code(ready),
                        "rawBoundaryProven":terminal_raw_proven,"rawBodyWritten":false,"index":index,
                    }));
                    return Err(format!("device rejected raw P4 asset chunk header: {}",appearance_safety::raw_ready_error_code(ready)));
                }
            }
            // The body may have been prepared before reboot/cancellation. Its
            // own physical submission must linearize AFTER a fresh admission.
            let raw_started=Instant::now();
            {
                let (_lease,mut slot)=self.appearance_admission(&appearance)?;
                let connection=slot.as_mut().ok_or("Not connected")?;
                let _pause=SerialReaderPause::new(&self.serial_body_write_active);
                if let Err(error)=write_serial_bytes_paced(connection.writer.as_mut(),chunk,
                    P4_CH343_RAW_WRITE_SLICE_BYTES,P4_CH343_RAW_WRITE_GAP){
                    connection.connected=false;connection.cancel_reader.store(true,Ordering::Release);
                    cache_usb_status(&self.status_snapshot,Some(connection));
                    return Err(format!("Raw asset paced write failed: {error}"));
                }
            }
            raw_write_elapsed_ms=raw_started.elapsed().as_millis();
            Ok(())
        })();
        let raw_result=(||{
        if let Some(ready) = ready_ack.as_ref() {
            // Persist diagnostics only after the binary payload has left the
            // critical raw-ready window. Opening a log file here previously
            // delayed the board while it was already waiting for raw bytes.
            transfer_log::record(
                "appearance",
                "raw_ready_ack",
                serde_json::json!({
                    "index": index,
                    "size": chunk.len(),
                    "readyElapsedMs": ready_elapsed_ms,
                    "rawWriteElapsedMs": raw_write_elapsed_ms,
                    "response": appearance_safety::raw_ready_diagnostics(ready),
                }),
            );
        }
        if let Err(error) = send_result {
            transfer_log::record(
                "appearance",
                "raw_chunk_send_failed",
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
        let ack_index=ack["index"].as_u64().or_else(||ack["index"].as_str().and_then(|s|s.parse::<u64>().ok()));
        if ack["transferId"]!=transfer_id||ack["phase"]!="raw-chunk"||ack["path"]!=path||ack_index!=Some(u64::from(index))
            ||ack["ok"].as_bool().is_none(){return Err("raw terminal receipt not exact".into());}
        {
            let _admission=self.appearance_admission(&appearance)?;
            appearance.raw_confirmed()?;terminal_raw_proven=true;
        }
        // An exact terminal NACK also proves the binary buffer was consumed;
        // it does NOT prove the enclosing appearance operation committed.
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
        })();
        if raw_result.is_err()&&physical_header_submitted&&!terminal_raw_proven{self.retire_uncertain_raw(&appearance);}
        drop(wire);
        raw_result
    }

    fn send_asset_raw_chunk_checked(
        &self,
        transfer_id: &str,
        path: &str,
        chunk: &[u8],
        index: u32,
    ) -> Result<(), String> {
        let result=self.send_asset_raw_chunk_once(transfer_id,path,chunk,index);
        if result.is_err() { self.appearance_uncertain(); }
        result
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
                .is_some_and(|size| size >= P4_RAW_APPEARANCE_SAFE_CHUNK_SIZE as u64)
    }

    fn p4_raw_asset_chunk_size(&self) -> usize {
        let advertised_limit = self
            .status()
            .capabilities
            .get("transport")
            .and_then(|value| value.get("rawAssetChunkBytes"))
            .and_then(|value| value.as_u64());
        p4_raw_asset_chunk_size_for_limit(advertised_limit)
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
        self.activation_start();
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
            ASSET_ACK_TIMEOUT,
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
        let mark=|kind|{if phase=="activate" {self.activation_update(|t|t.fail(kind));}};
        let mut explicit_rejection=false;
        let result=(|| {
        let receiver = self.register_asset_ack_waiter(transfer_id, phase, path, index)?;
        let command_started = Instant::now();
        if let Err(error) = self.send(topic, payload) {
            self.remove_asset_ack_waiter(transfer_id, phase, path, index);
            return Err(error);
        }
        if phase=="activate" {self.activation_update(|t|t.ack_wait_started());}
        let send_elapsed = command_started.elapsed();
        let started = Instant::now();
        let ack = loop {
            if let Err(error) = self.ensure_appearance_sync_not_cancelled(Some(transfer_id)) {
                mark(appearance_activation_evidence::Kind::Cancelled);
                self.remove_asset_ack_waiter(transfer_id, phase, path, index);
                return Err(error);
            }
            let Some(remaining) = timeout.checked_sub(started.elapsed()) else {
                self.remove_asset_ack_waiter(transfer_id, phase, path, index);
                mark(appearance_activation_evidence::Kind::AckTimeout);
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
                    mark(appearance_activation_evidence::Kind::AckChannelClosed);
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
        if phase=="activate" {self.activation_update(|t|t.response(ack.get("ok").and_then(|v|v.as_bool())));}
        if ack.get("ok").and_then(|value| value.as_bool()) != Some(true) {
            explicit_rejection=ack.get("ok").and_then(|value|value.as_bool())==Some(false);
            return Err(ack
                .get("error")
                .or_else(|| ack.get("message"))
                .and_then(|value| value.as_str())
                .unwrap_or("板端素材 OTA 写入失败")
                .to_string());
        }
        Ok(ack)

        })();
        if result.is_err()&&!explicit_rejection { self.appearance_uncertain(); }
        result
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
        self.remove_widget_with_save_policy(expected_board_device_id,widget_id,true)
    }

    pub(crate) fn remove_widget_with_save_policy(&self, expected_board_device_id:&str, widget_id:&str, preserve_save:bool)->Result<(),String>{
        crate::program_autostart::native::component_changed(widget_id,true);
        let _transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        let (status,connection_id) = self.connected_board_snapshot(expected_board_device_id)?;
        ensure_widget_delete_supported(&status)?;
        if preserve_save&&status.capabilities["widgetDeletePreserveSave"]!=true{return Err("设备不支持保留存档卸载；未删除组件".into());}
        let transfer_id = format!("widget-delete-{}", uuid::Uuid::new_v4());
        let receiver = self.register_widget_ack_waiter(&transfer_id, "delete", None, None)?;
        let payload = serde_json::json!({
            "transferId": &transfer_id,
            "widgetId": widget_id,
            "preserveSave": preserve_save,
        });
        if let Err(error) = self.send_inner("widget/delete", &payload, true, Some(expected_board_device_id), Some(connection_id), None)
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
        let target=self.status().board_device_id;
        self.install_widget_clawpkg_on_board(&target,widget_id,widget_dir,binding_overrides,on_progress)
    }

    pub fn install_widget_clawpkg_on_board<F>(
        &self,
        expected_board_device_id:&str,
        widget_id:&str,
        widget_dir:&std::path::Path,
        binding_overrides:&std::collections::HashMap<String,String>,
        on_progress:F,
    )->Result<(u32,u64),String>
    where F:Fn(u32,u32,u64),
    {
        self.install_widget_clawpkg_mode(expected_board_device_id,widget_id,widget_dir,binding_overrides,on_progress,false)
    }

    pub(crate) fn repair_widget_resources(&self,board:&str,id:&str,dir:&std::path::Path)->Result<(u32,u64),String>{
        if self.status().capabilities["widgetResourceIntegrity"]!="p4-resource-integrity-v1"{
            return Err("设备尚不支持保留配置的资源恢复".into());
        }
        self.install_widget_clawpkg_mode(board,id,dir,&std::collections::HashMap::new(),|_,_,_|{},true)
    }

    fn install_widget_clawpkg_mode<F>(
        &self,expected_board_device_id:&str,widget_id:&str,widget_dir:&std::path::Path,
        binding_overrides:&std::collections::HashMap<String,String>,on_progress:F,repair_resources:bool,
    )->Result<(u32,u64),String> where F:Fn(u32,u32,u64) {
        use base64::Engine;
        crate::program_autostart::native::component_changed(widget_id,false);
        let _transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        let (install_status,connection_id)=self.connected_board_snapshot(expected_board_device_id)?;
        let extension_path=widget_dir.join("pc/extension.json");
        if extension_path.is_file() {
            if extension_path.metadata().map_err(|_|"无法读取媒体扩展")?.len()>16384 {return Err("媒体扩展超过安全上限".into());}
            // Validate the declaration, not the peer's optional feature adverts.
            crate::component_extension::parse(&std::fs::read(extension_path).map_err(|_|"无法读取媒体扩展")?)?;
        }
        let send_bound=|topic:&str,payload:&serde_json::Value|self.send_inner(topic,payload,true,Some(expected_board_device_id),Some(connection_id),None);
        // Stay below the P4 runtime's 64-byte transaction buffer even when
        // the manifest uses the maximum 47-byte component id.
        let transfer_id = format!("widget-install-{}", uuid::Uuid::new_v4());
        let transaction_started = std::time::Instant::now();
        // Only generated transaction IDs, phase timing and numeric/boolean
        // outcomes: never persist package paths, definitions or device messages.
        let record_phase = |event: &str, phase: &str, started: std::time::Instant,
                            metadata: serde_json::Value| {
            transfer_log::record("widget", event, serde_json::json!({
                "transferId": &transfer_id,
                "phase": phase,
                "elapsedMs": started.elapsed().as_millis(),
                "transactionElapsedMs": transaction_started.elapsed().as_millis(),
                "metadata": metadata,
            }));
        };

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
                if ent.file_type().map_err(|e|e.to_string())?.is_symlink(){return Err("组件资源不允许符号链接".into());}
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

        // The package keeps author-friendly PNG sheets for validation and the
        // desktop preview. P4 receives compact RGB565+alpha runtime files
        // generated from the exact same source images. Raw assets are not part
        // of the bounded firmware file contract.
        let widget_source = entries
            .iter()
            .find(|(relative_path, _)| relative_path == "runtime/widget.json")
            .map(|(_, bytes)| bytes.as_slice())
            .ok_or_else(|| "component package is missing runtime/widget.json".to_string())?;
        let widget_definition: serde_json::Value = serde_json::from_slice(widget_source).map_err(|e| e.to_string())?;
        // Optional advertised features/hardware do not prohibit installation.
        // Keep executable/package/resource validation; firmware remains the
        // authority for parsing/accepting the actual uploaded component.
        crate::app_component_program::validate(&widget_definition)?;
        // This is a proven legacy memory boundary, not an optional feature:
        // old peers advertised 24/64 items while their draw buffer held only16.
        let extended_local_deck=widget_definition["local"]["kind"]=="deck"
            &&widget_definition["local"]["groups"].as_array().is_some_and(|groups|groups.iter().any(|group|
                group["items"].as_array().is_some_and(|items|items.len()>16)));
        if extended_local_deck&&install_status.capabilities["widgetLocalToolsMasks"]!="p4-local-masks-v2" {
            return Err("离线内容池超过旧固件安全的 16 项接收边界；未写入组件".into());
        }
        let compiled_sprites = prepare_p4_widget_sprite_files(widget_dir, widget_source)?;
        let compiled_media=crate::component_capability_ext::prepare_p4_widget_media_files(widget_dir,&widget_definition)?;
        entries.retain(|(relative_path, _)| !relative_path.starts_with("assets/"));
        entries.extend(compiled_sprites);
        entries.extend(compiled_media);

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

        // Validate actual user-remapped form controls, never assume SW1/SW2 roles.
        // This is still before widget/begin, so a bad override writes no device state.
        if widget_definition.get("form").is_some(){
            let buttons=entries.iter().find(|(path,_)|path=="buttons.json")
                .ok_or("表单缺少物理动作映射")?;
            let buttons:serde_json::Value=serde_json::from_slice(&buttons.1).map_err(|e|e.to_string())?;
            crate::component_extension::app_form::validate_bindings(&widget_definition,&buttons)?;
        }

        // 3) begin - register ack waiter BEFORE sending so we don't miss
        //    a very-fast reply on a healthy board (~50ms).
        // The same connection snapshot that binds every send negotiates this
        // optimization. Old peers retain their exact legacy begin envelope;
        // repairs always take the original physical repair transaction.
        let begin_payload=widget_transaction::build_widget_begin_payload(&transfer_id,widget_id,repair_resources,&install_status.capabilities);
        let begin_rx = self.register_widget_ack_waiter(&transfer_id, "begin", None, None)?;
        let begin_started = std::time::Instant::now();
        record_phase("phase_started", "begin", begin_started,
            serde_json::json!({"timeoutMs": WIDGET_BEGIN_ACK_TIMEOUT.as_millis()}));
        if let Err(error)=send_bound("widget/begin",&begin_payload){
            record_phase("phase_send_failed", "begin", begin_started, serde_json::json!({}));
            self.remove_widget_ack_waiter(&transfer_id,"begin",None,None);return Err(error);
        }
        match begin_rx.recv_timeout(WIDGET_BEGIN_ACK_TIMEOUT) {
            Ok(ack) => {
                record_phase("phase_ack", "begin", begin_started,
                    serde_json::json!({"ok": ack.get("ok").and_then(|v| v.as_bool())}));
                if ack.get("ok").and_then(serde_json::Value::as_bool).is_none() {
                    self.remove_widget_ack_waiter(&transfer_id,"begin",None,None);
                    return Err("组件开始回执格式未确认，结果未知；不会自动重发或取消传输。".into());
                }
                if ack.get("ok").and_then(|v| v.as_bool()) == Some(false) {
                    self.remove_widget_ack_waiter(&transfer_id, "begin", None, None);
                    let msg = ack
                        .get("msg")
                        .and_then(|v| v.as_str())
                        .unwrap_or("板端拒绝组件传输开始");
                    return Err(format!("widget begin rejected: {}", msg));
                }
            }
            Err(error) => {
                record_phase(if matches!(error, std::sync::mpsc::RecvTimeoutError::Timeout) {
                    "phase_timed_out"
                } else { "phase_wait_disconnected" }, "begin", begin_started, serde_json::json!({}));
                self.remove_widget_ack_waiter(&transfer_id, "begin", None, None);
                return Err(format_widget_ack_timeout(&transfer_id, "begin"));
            }
        }

        // 4) chunks. Skip zero-byte files (.keep markers). P4 acknowledges each
        // file with decoded size + checksum validation. Unknown or rejected
        // chunks stop this intent; metadata conflict never authorizes a replay.
        let b64 = base64::engine::general_purpose::STANDARD;
        let total = entries.len() as u32;
        let mut sent_bytes: u64 = 0;
        for (i, (rel, bytes)) in entries.iter().enumerate() {
            if bytes.is_empty() {
                continue;
            }
            for (chunk_index,bytes) in bytes.chunks(if rel.starts_with("runtime/images/"){4096}else{bytes.len()}).enumerate(){
            let chunk_index=chunk_index as u32;
            let encoded = b64.encode(bytes);
            let checksum = asset_checksum_hex(bytes);
            {
                let attempt = 1usize;
                let chunk_rx =
                    self.register_widget_ack_waiter(&transfer_id, "chunk", Some(rel), Some(chunk_index))?;
                #[cfg(test)] installer_acceptance::attempt(&transfer_id,rel,chunk_index);
                let chunk_started = std::time::Instant::now();
                let chunk_metadata = |ok: Option<bool>| serde_json::json!({
                    "fileIndex": i, "chunkIndex": chunk_index, "attempt": attempt,
                    "decodedSize": bytes.len(), "ok": ok,
                    "timeoutMs": WIDGET_CHUNK_ACK_TIMEOUT.as_millis(),
                });
                record_phase("phase_started", "chunk", chunk_started, chunk_metadata(None));
                if let Err(error) = send_bound("widget/chunk",&build_widget_chunk_payload(&transfer_id,rel,&encoded,chunk_index,bytes.len(),&checksum)) {
                    record_phase("phase_send_failed", "chunk", chunk_started, chunk_metadata(None));
                    #[cfg(test)] installer_acceptance::outcome(&transfer_id,rel,chunk_index,"send_failed_unknown");
                    self.remove_widget_ack_waiter(&transfer_id, "chunk", Some(rel), Some(chunk_index));
                    return Err(error);
                }
                match chunk_rx.recv_timeout(WIDGET_CHUNK_ACK_TIMEOUT) {
                    Ok(ack) if ack.get("ok").and_then(|value| value.as_bool()) == Some(true) => {
                        record_phase("phase_ack", "chunk", chunk_started, chunk_metadata(Some(true)));
                        #[cfg(test)] installer_acceptance::outcome(&transfer_id,rel,chunk_index,"acknowledged");
                    }
                    Ok(ack) if ack.get("ok").and_then(serde_json::Value::as_bool) == Some(false) => {
                        record_phase("phase_ack", "chunk", chunk_started, chunk_metadata(Some(false)));
                        #[cfg(test)] installer_acceptance::outcome(&transfer_id,rel,chunk_index,"rejected_no_replay");
                        // Even a real metadata conflict is not permission to
                        // overwrite/re-send that chunk under the old intent.
                        let message = ack.get("msg").and_then(serde_json::Value::as_str).unwrap_or("widget chunk rejected");
                        return Err(format!("widget chunk rejected; no automatic retry: {message}"));
                    }
                    Ok(_) => {
                        record_phase("phase_ack_invalid", "chunk", chunk_started, chunk_metadata(None));
                        #[cfg(test)] installer_acceptance::outcome(&transfer_id,rel,chunk_index,"ack_unknown_no_replay");
                        self.remove_widget_ack_waiter(&transfer_id,"chunk",Some(rel),Some(chunk_index));
                        return Err("组件数据回执格式未确认，已停止发送且不会自动重发。请先刷新设备清单核对结果。".into());
                    }
                    Err(error) => {
                        record_phase(if matches!(error, std::sync::mpsc::RecvTimeoutError::Timeout) {
                            "phase_timed_out"
                        } else { "phase_wait_disconnected" }, "chunk", chunk_started, chunk_metadata(None));
                        #[cfg(test)] installer_acceptance::outcome(&transfer_id,rel,chunk_index,"ack_unknown_no_replay");
                        self.remove_widget_ack_waiter(&transfer_id, "chunk", Some(rel), Some(chunk_index));
                        // Unknown outcome: never replay this chunk, commit or abort.
                        return Err("组件数据回执未确认，已停止发送且不会自动重发。请先刷新设备清单核对结果，再决定后续处理。".into());
                    }
                }
            }
            sent_bytes += encoded.len() as u64;
            }
            on_progress(i as u32 + 1, total, sent_bytes);
        }

        // 5) commit - wait for board to finish decode+rename+activate so the
        // caller's "success" toast lines up with the device actually showing
        // the new widget.
        let commit_rx = self.register_widget_ack_waiter(&transfer_id, "commit", None, None)?;
        let commit_started = std::time::Instant::now();
        record_phase("phase_started", "commit", commit_started,
            serde_json::json!({"timeoutMs": WIDGET_COMMIT_ACK_TIMEOUT.as_millis(),
                "files": total, "encodedBytes": sent_bytes}));
        if let Err(error)=send_bound("widget/commit",&serde_json::json!({"transferId":transfer_id,"widgetId":widget_id})){
            record_phase("phase_send_failed", "commit", commit_started, serde_json::json!({}));
            self.remove_widget_ack_waiter(&transfer_id,"commit",None,None);return Err(error);
        }
        match commit_rx.recv_timeout(WIDGET_COMMIT_ACK_TIMEOUT) {
            Ok(ack) => {
                record_phase("phase_ack", "commit", commit_started,
                    serde_json::json!({"ok": ack.get("ok").and_then(|v| v.as_bool())}));
                if ack.get("ok").and_then(serde_json::Value::as_bool).is_none() {
                    return Err("组件提交回执格式未确认，结果未知；不会自动重发或取消传输。".into());
                }
                if ack.get("ok").and_then(|v| v.as_bool()) == Some(false) {
                    let msg = ack
                        .get("msg")
                        .and_then(|v| v.as_str())
                        .unwrap_or("板端组件提交失败")
                        .to_string();
                    return Err(msg);
                }
            }
            Err(error) => {
                record_phase(if matches!(error, std::sync::mpsc::RecvTimeoutError::Timeout) {
                    "phase_timed_out"
                } else { "phase_wait_disconnected" }, "commit", commit_started, serde_json::json!({}));
                self.remove_widget_ack_waiter(&transfer_id, "commit", None, None);
                return Err(format_widget_ack_timeout(&transfer_id, "commit"));
            }
        }
        record_phase("transaction_completed", "commit", commit_started,
            serde_json::json!({"files": total, "encodedBytes": sent_bytes}));
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
        self.with_appearance_user_intent(&self.status().board_device_id, &_asset_transfer_guard, || {
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
                return self.sync_appearance_audio_patch(changed_audio, &on_progress);
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
        })
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
        self.sync_appearance_full(assets,total_files,total_bytes,on_progress,AppearanceFullSyncMode::Verified)
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
        use_device_builtin: bool,
        on_progress: F,
    ) -> Result<(u32, u64, bool), String>
    where
        F: Fn(u32, u32, u64, u64),
    {
        self.sync_appearance_p4_with_stages(
            appearance_dir,
            app_data_dir,
            expected_board_device_id,
            use_device_builtin,
            on_progress,
            |_| {},
        )
    }

    /// Same as `sync_appearance_p4`, and also names each pre-transfer step
    /// (cache query, remote comparison, slot preparation, device flash
    /// erase) so the UI is not a silent 0% before the first file byte.
    pub fn sync_appearance_p4_with_stages<F, S>(
        &self,
        appearance_dir: &std::path::Path,
        app_data_dir: &std::path::Path,
        expected_board_device_id: &str,
        use_device_builtin: bool,
        on_progress: F,
        on_stage: S,
    ) -> Result<(u32, u64, bool), String>
    where
        F: Fn(u32, u32, u64, u64),
        S: Fn(&'static str),
    {
        let sync_started = Instant::now();
        let stage = |name: &'static str| {
            transfer_log::record(
                "appearance",
                "stage",
                serde_json::json!({
                    "stage": name,
                    "elapsedMs": sync_started.elapsed().as_millis() as u64,
                }),
            );
            on_stage(name);
        };
        let _asset_transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        self.with_appearance_user_intent(expected_board_device_id, &_asset_transfer_guard, || {
        self.connected_board_status(expected_board_device_id)?;
        self.ensure_appearance_sync_not_cancelled(None)?;
        if p4_native_usb_available() {
            if let Some(result)=self.native_appearance_or_fallback(||sync_appearance_p4_native(
                self,appearance_dir,app_data_dir,expected_board_device_id,use_device_builtin,
                &self.appearance_sync_cancel_requested,&on_progress,
            ))? {return Ok(result);}

        }

        if use_device_builtin {
            let started = Instant::now();
            let id = format!("builtin-{}", uuid::Uuid::new_v4());
            let state = self.query_p4_appearance_slots(&id)?;
            let slot = p4_builtin_slot(&state)?;
            self.ensure_appearance_sync_not_cancelled(None)?;
            if state.active_slot != Some(slot.slot) {
                self.activate_p4_appearance_slot(&id, slot.slot, &slot.pack_id)?;
            }
            transfer_log::record("appearance","builtin_activated",serde_json::json!({"files":0,"bytes":0,"elapsedMs":started.elapsed().as_millis()}));
            on_progress(0,0,0,0);
            return Ok((0,0,true));
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
        let force_full_sync = p4_force_full_sync_requested();
        if force_full_sync {
            eprintln!("[usb-p4-ota] forcing full transfer for hardware validation");
        }
        if !force_full_sync && self.supports_p4_appearance_slot_reuse() {
            stage("checking_cache");
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
                Err(error) if self.appearance_submitted()=>return Err(error),
                Err(error) => {
                    eprintln!(
                        "[usb-p4-ota] cached slot query failed; falling back to current sync planner: {}",
                        error
                    );
                }
            }
        }
        if !force_full_sync {
            stage("comparing_assets");
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
        }

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
        let preferred_chunk_size = if use_raw_chunks {
            self.p4_raw_asset_chunk_size()
        } else {
            P4_APPEARANCE_ASSET_CHUNK_SIZE
        };
        let run_transfer = |chunk_size: usize| -> Result<(u32, u64, bool), String> {
            let transfer_id = format!(
                "p4-{}",
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_millis()
            );
            if use_raw_slot {
                stage("preparing_slot");
            }
            if use_raw_slot && self.prepare_p4_raw_transfer_slot(&transfer_id)? {
                eprintln!(
                    "[usb-p4-ota] activated slot0 before full sync so raw slot1 remains the transfer target"
                );
            }
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
                // The board erases the target flash range and compacts SPIFFS
                // before acknowledging begin; this is the longest silent step.
                stage("preparing_storage");
                let begin_started = Instant::now();
                self.send_asset_begin_checked_with_raw_bytes(
                    &transfer_id,
                    total_bytes,
                    use_raw_slot.then_some(raw_bytes),
                )?;
                transfer_log::record(
                    "appearance",
                    "begin_acknowledged",
                    serde_json::json!({
                        "transferId": transfer_id.as_str(),
                        "rawBytes": raw_bytes,
                        "elapsedMs": begin_started.elapsed().as_millis() as u64,
                    }),
                );
                stage("transferring");

                let mut file_count: u32 = 0;
                let mut byte_count: u64 = 0;
                on_progress(0, total_files, 0, total_bytes);

                for asset in &assets {
                    self.ensure_appearance_sync_not_cancelled(Some(&transfer_id))?;
                    eprintln!("[usb-p4-ota] transferring prepared asset kind={}", asset.kind);
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
        };

        run_transfer(preferred_chunk_size)
        })
    }

    pub fn sync_appearance_p4_native_only<F>(
        &self,
        appearance_dir: &std::path::Path,
        app_data_dir: &std::path::Path,
        expected_board_device_id: &str,
        use_device_builtin: bool,
        on_progress: F,
    ) -> Result<(u32, u64, bool), String>
    where
        F: Fn(u32, u32, u64, u64),
    {
        let _asset_transfer_guard = self
            .asset_transfer_guard
            .lock()
            .map_err(|error| error.to_string())?;
        self.with_appearance_user_intent(expected_board_device_id, &_asset_transfer_guard, || {
        sync_appearance_p4_native(
            self,
            appearance_dir,
            app_data_dir,
            expected_board_device_id,
            use_device_builtin,
            self.appearance_sync_cancel_requested.as_ref(),
            on_progress,
        )
        })
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
        if let Ok(mut connection) = self.connection.try_lock() {
            if let Some(c) = connection.as_mut() { self.refresh_pending_hello(c); }
            let snapshot = connection
                .as_ref()
                .map(usb_connection_status)
                .unwrap_or_else(disconnected_usb_status);
            if let Ok(mut cached) = self.status_snapshot.lock() {
                *cached = snapshot.clone();
            }
            return snapshot;
        }

        self.status_snapshot
            .lock()
            .map(|snapshot| snapshot.clone())
            .unwrap_or_else(|_| disconnected_usb_status())
    }
}

fn usb_connection_status(connection: &UsbConnection) -> UsbConnectionStatus {
    let connected = connection.connected && !connection.cancel_reader.load(Ordering::Acquire);
    UsbConnectionStatus {
        connected,
        connection_id: if connected&&connection.connection_id!=0{connection.connection_id.to_string()}else{String::new()},
        boot_id: if connected{connection.capabilities["debugBootId"].as_str()
            .filter(|s|s.len()==16&&s.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b)))
            .unwrap_or_default().to_owned()}else{String::new()},
        port_name: connection.port_name.clone(),
        baud_rate: connection.baud_rate,
        board_device_id: connection.board_device_id.clone(),
        transport: "usb".to_string(),
        runtime: connection.runtime.clone(),
        device_model: connection.device_model.clone(),
        firmware: connection.firmware.clone(),
        build_id: connection.build_id.clone(),
        git_sha: connection.git_sha.clone(),
        build_dirty: connection.build_dirty,
        protocol_schema: connection.protocol_schema,
        wire_protocol: connection.wire_protocol.clone(),
        capabilities: if connected {
            connection.capabilities.clone()
        } else { serde_json::Value::Null },
    }
}

// Call while holding the connection lock, before releasing it or doing I/O.
// Every lifecycle transition must replace the snapshot, not just status polls.
fn cache_usb_status(cache: &Mutex<UsbConnectionStatus>, connection: Option<&UsbConnection>) {
    if let Ok(mut cached) = cache.lock() {
        *cached = connection.map(usb_connection_status).unwrap_or_else(disconnected_usb_status);
    }
}

fn disconnected_usb_status() -> UsbConnectionStatus {
    UsbConnectionStatus {
        connected: false,
        connection_id: String::new(),
        boot_id: String::new(),
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
    // HELLO capabilities are a snapshot, not a patch. Missing/new boot data
    // cannot inherit capabilities from a previous firmware generation.
    conn.capabilities = payload.get("capabilities").cloned().unwrap_or(serde_json::Value::Null);
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
        crate::json_canonical::canonical_vec(&identity_manifest).map_err(|error| error.to_string())?;
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

pub fn prepared_p4_requires_system_cues(appearance_dir: &Path) -> Result<bool, String> {
    let bytes = std::fs::read(p4_ready_profile_root(appearance_dir).join("p4/manifest.json"))
        .map_err(|e| e.to_string())?;
    let manifest: serde_json::Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    Ok(manifest["systemCues"] == true)
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
                family["audioSource"] = serde_json::json!("custom");
            }
            family
        })
        .collect::<Vec<_>>();

    serde_json::json!({
        "format": "p4-h264-v1",
        "systemCues": true,
        "packId": pack_id,
        "codec": "h264",
        "container": "annex-b",
        "width": width,
        "height": height,
        "fps": fps,
        "families": p4_families,
    })
}

pub(crate) fn ffmpeg_container_duration_ms(stderr: &[u8]) -> Option<u64> {
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
    encoder: crate::codex_import::PlatformH264Encoder,
) -> Vec<String> {
    let mut args = vec![
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
        encoder.codec_name().to_string(),
        "-profile:v".to_string(),
        encoder.profile_value().to_string(),
        "-level:v".to_string(),
        encoder.level_arg().to_string(),
        "-pix_fmt".to_string(),
        "nv12".to_string(),
        "-g".to_string(),
        max_frames.to_string(),
        "-bf".to_string(),
        "0".to_string(),
        "-refs".to_string(),
        "1".to_string(),
        "-b:v".to_string(),
        "900k".to_string(),
        "-maxrate".to_string(),
        "1200k".to_string(),
        "-bufsize".to_string(),
        "2400k".to_string(),
    ];
    encoder.append_quality_args(&mut args);
    // Do not insert an additional AUD into Media Foundation output. The P4
    // parser below still rejects missing/invalid frame boundaries. Retain the
    // existing AUD insertion for VideoToolbox.
    if encoder == crate::codex_import::PlatformH264Encoder::VideoToolbox {
        args.extend(["-bsf:v".to_string(), "h264_metadata=aud=insert".to_string()]);
    }
    args.extend([
        "-f".to_string(),
        "h264".to_string(),
        output.display().to_string(),
    ]);
    args
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

fn parse_p4_h264_stream(bytes: &[u8], width: u32, height: u32) -> Result<P4H264Stream, String> {
    if width == 0 || height == 0 || width & 1 != 0 || height & 1 != 0 {
        return Err("P4 H.264 width and height must be positive even values".to_string());
    }
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
                let sps = &bytes[payload_start..nal.end];
                if sps.len() < 4 || sps[1] != 66 || sps[3] > 30 {
                    return Err(
                        "P4 H.264 stream must use Baseline profile at level 3.0 or lower"
                            .to_string(),
                    );
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
        crate::json_canonical::canonical_vec(&identity_manifest).map_err(|error| error.to_string())?;
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
        let audio_device_path = if let Some(audio_path) = p4_custom_audio_path(
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

fn p4_custom_audio_path(
    family: &serde_json::Value, family_name: &str, video_path: &Path,
    appearance_dir: &Path, app_data_dir: &Path,
) -> Option<PathBuf> {
    // Implicit sibling WAVs were injected defaults. Only explicit user audio
    // travels with a new P4 pack; Linux's legacy asset path stays unchanged.
    if family["audioSource"] == "default" || family["audioPath"].as_str().is_none() { return None; }
    family_audio_path(family, family_name, video_path, appearance_dir, app_data_dir)
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
    let encoder = crate::codex_import::platform_h264_encoder(ffmpeg)?;
    let duration_ms = probe_video_duration_ms(ffmpeg, input)?;
    let args = build_p4_h264_ffmpeg_args(
        input,
        output,
        P4_APPEARANCE_WIDTH,
        P4_APPEARANCE_HEIGHT,
        p4_sampling_fps(duration_ms),
        P4_APPEARANCE_MAX_FRAMES,
        encoder,
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
    let stream = parse_p4_h264_stream(&bytes, P4_APPEARANCE_WIDTH, P4_APPEARANCE_HEIGHT)?;
    let decode = crate::command_for_host(ffmpeg)
        .args(["-v", "error", "-nostdin", "-f", "h264", "-i"])
        .arg(output)
        .args(["-an", "-f", "null", "-"])
        .output()
        .map_err(|error| format!("start P4 H.264 decode validation failed: {error}"))?;
    if !decode.status.success() {
        return Err(format!(
            "P4 H.264 stream failed post-encode decode validation: {}",
            String::from_utf8_lossy(&decode.stderr).trim()
        ));
    }
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
    if port_name.trim().is_empty() {
        return false;
    }
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
        "SW1 长按" => ("SW1", "button.sw1.long_press"),
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
    #[test]
    fn component_sync_ended_music_retains_lease_until_owner_releases_it(){
        for state in ["playing","buffering","preparing","paused","ended"]{assert!(super::component_sync_media_blocks(state),"{state}");}
        for state in ["idle","stopped"]{assert!(!super::component_sync_media_blocks(state),"{state}");}
    }
    include!("usb_serial/silent_acceptance.rs");
    include!("usb_serial/new_components_acceptance.rs");
    include!("usb_serial/framework_install_acceptance.rs");
    #[test]
    #[cfg(target_os = "macos")]
    fn paced_macos_serial_writes_do_not_drain_driver() {
        struct Writer(Vec<u8>, Vec<usize>);
        impl std::io::Write for Writer {
            fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
                self.0.extend_from_slice(bytes);
                self.1.push(bytes.len());
                Ok(bytes.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                panic!("macOS serial drain must not be called");
            }
        }
        let mut writer = Writer(Vec::new(), Vec::new());
        super::write_serial_bytes_paced(&mut writer, b"abcdefg", 3, std::time::Duration::ZERO).unwrap();
        super::flush_serial_writer(&mut writer).unwrap();
        assert_eq!(writer.0, b"abcdefg");
        assert_eq!(writer.1, vec![3, 3, 1]);
    }
    #[test]
    fn protected_builtin_selection_ignores_desktop_hash_but_never_uses_custom_slot() {
        use super::*;
        let state = P4AppearanceSlotState { active_slot:Some(1), slots:vec![
            P4AppearanceSlot {slot:0,pack_id:"old-factory-hash".into()},
            P4AppearanceSlot {slot:1,pack_id:"new-desktop-hash".into()}] };
        assert_eq!(p4_builtin_slot(&state).unwrap().slot, 0);
        assert_eq!(p4_builtin_slot(&state).unwrap().pack_id,"old-factory-hash");
        let missing = P4AppearanceSlotState {active_slot:Some(1),slots:vec![state.slots[1].clone()]};
        assert!(p4_builtin_slot(&missing).is_err());
    }

    #[test]
    fn new_p4_packs_exclude_implicit_defaults_and_keep_explicit_overrides() {
        use super::*;
        let tmp=tempfile::tempdir().unwrap();
        let video=tmp.path().join("done.mp4");
        std::fs::write(&video,b"video").unwrap();
        std::fs::write(video.with_extension("wav"),p4_test_wav()).unwrap();
        let implicit=serde_json::json!({"family":"done","ok":true});
        assert!(p4_custom_audio_path(&implicit,"done",&video,tmp.path(),tmp.path()).is_none());
        let default=serde_json::json!({"audioPath":"done.wav","audioSource":"default"});
        assert!(p4_custom_audio_path(&default,"done",&video,tmp.path(),tmp.path()).is_none());
        for source in [serde_json::json!({"audioPath":"done.wav"}),serde_json::json!({"audioPath":"done.wav","audioSource":"custom"})] {
            assert_eq!(p4_custom_audio_path(&source,"done",&video,tmp.path(),tmp.path()),Some(video.with_extension("wav")));
        }
    }

    #[test]
    fn changing_default_wav_does_not_change_p4_pack_identity() {
        use super::*;
        let tmp=tempfile::tempdir().unwrap();
        std::fs::write(tmp.path().join("done.mp4"),b"video").unwrap();
        let build=|family:serde_json::Value, name:&str| {
            let assets=stage_p4_appearance_pack_with_exporter(&[family],tmp.path(),tmp.path(),&tmp.path().join(name),|_, output| {
                std::fs::write(output,b"stream").unwrap();
                Ok(P4ExportedStream{frames:1,stream_bytes:6,fps:15,frame_duration_ms:67,duration_ms:67})
            }).unwrap();
            p4_pack_id_from_assets(&assets).unwrap()
        };
        let family=serde_json::json!({"family":"done","ok":true});
        std::fs::write(tmp.path().join("done.wav"),p4_test_wav()).unwrap();
        let first=build(family.clone(),"first");
        let mut other=p4_test_wav(); *other.last_mut().unwrap() ^= 1;
        std::fs::write(tmp.path().join("done.wav"),&other).unwrap();
        assert_eq!(first,build(family,"second"));
        let custom=serde_json::json!({"family":"done","ok":true,"audioPath":"done.wav"});
        let third=build(custom.clone(),"third");
        assert_ne!(first,third);
        std::fs::write(tmp.path().join("done.wav"),p4_test_wav()).unwrap();
        assert_ne!(third,build(custom,"fourth"));
    }
    use super::*;

    fn background_test_manager(mode:usize)->(UsbSerialManager,Arc<Mutex<Vec<String>>>){
        struct ReplyWriter{pending:Vec<u8>,waiters:Arc<Mutex<Vec<DeviceResponseWaiter>>>,topics:Arc<Mutex<Vec<String>>>,mode:usize,running:bool}
        impl Write for ReplyWriter{
            fn write(&mut self,bytes:&[u8])->std::io::Result<usize>{
                self.pending.extend_from_slice(bytes);assert!(self.pending.len()<8192);
                while let Some(end)=self.pending.iter().position(|b|*b==b'\n'){
                    let row=self.pending.drain(..=end).collect::<Vec<_>>();let request:serde_json::Value=serde_json::from_slice(&row).unwrap();
                    let topic=request["topic"].as_str().unwrap();self.topics.lock().unwrap().push(topic.into());
                    let mut reply=serde_json::json!({"requestId":request["payload"]["requestId"],"ok":true,"boardDeviceId":"p4-test","engine":crate::app_program_background::ENGINE,"bootId":"0123456789abcdef","items":[]});
                    let response=if topic=="widget/list"{
                        reply["complete"]=serde_json::json!(self.mode!=3);reply["recoveryBlocked"]=serde_json::json!(false);reply["items"]=serde_json::json!([{"id":"focus"}]);"widget/inventory"
                    }else{
                        match topic{
                            "widget/program-query"=>{},
                            "widget/program-inspect"=>{reply["installedContent"]=serde_json::json!({"widgetId":"focus","widgetChecksum":"ffffffff","buttonsChecksum":"00000001","spritesChecksum":"00000000"});},
                            "widget/program-start"=>{
                                assert_eq!(request["payload"]["widgetChecksum"],"ffffffff");assert_eq!(request["payload"]["bootId"],"0123456789abcdef");self.running=true;
                                if self.mode==1{reply["bootId"]=serde_json::json!("1123456789abcdef");}
                                if self.mode==2{reply["ok"]=serde_json::json!(false);reply["error"]=serde_json::json!("must-not-reflect");}
                            },
                            "widget/program-stop"=>{
                                assert_eq!(request["payload"]["registration"],"1");assert_eq!(request["payload"]["bootId"],"0123456789abcdef");self.running=false;
                            },_=>panic!("unexpected background topic {topic}"),
                        }
                        if self.running||topic=="widget/program-stop"{
                            reply["items"]=serde_json::json!([{"widgetId":"focus","registration":if self.mode==4{"2"}else{"1"},"live":self.running,"stopped":!self.running,"checkpointPending":false,"revision":0,"phase":1,"lastStepUs":10,"maxStepUs":20}]);
                        }
                        "widget/program-status"
                    };
                    resolve_device_response(&self.waiters,response,&reply);
                }
                Ok(bytes.len())
            }
            fn flush(&mut self)->std::io::Result<()>{Ok(())}
        }
        let manager=UsbSerialManager::new();let topics=Arc::new(Mutex::new(Vec::new()));
        *manager.connection.lock().unwrap()=Some(UsbConnection{
            connection_id:1,port_name:"synthetic-background".into(),baud_rate:P4_USB_UART_BAUD,
            writer:Box::new(ReplyWriter{pending:Vec::new(),waiters:manager.device_response_waiters.clone(),topics:topics.clone(),mode,running:false}),
            board_device_id:"p4-test".into(),runtime:"esp-p4".into(),device_model:String::new(),firmware:String::new(),build_id:String::new(),git_sha:String::new(),build_dirty:false,
            protocol_schema:7,wire_protocol:String::new(),capabilities:serde_json::json!({"widgetProgram":"p4-component-program-v1","widgetProgramBackground":crate::app_program_background::ENGINE,"widgetProgramInspect":crate::app_program_background::INSPECT,"widgetProgramBackgroundMaxActive":1}),
            connected:true,cancel_reader:Arc::new(AtomicBool::new(false)),capture_active:Arc::new(AtomicBool::new(false)),
        });
manager.activate_memory_test_connection();(manager,topics)
    }
    #[test]fn background_controller_exact_readonly_preflight_then_one_start_and_one_stop(){
        let(manager,topics)=background_test_manager(0);
        assert!(manager.query_background_programs("p4-test").unwrap().items.is_empty());
        assert!(manager.start_background_program("p4-test","focus").unwrap().confirmed);
        assert!(manager.stop_background_program("p4-test","focus","0123456789abcdef","1").unwrap().confirmed);
        assert_eq!(*topics.lock().unwrap(),["widget/program-query","widget/list","widget/program-inspect","widget/program-start","widget/program-stop"]);
        assert!(manager.device_response_waiters.lock().unwrap().is_empty());
    }
    #[test]fn background_controller_unknown_never_replays_or_replaces_observed_owner(){
        for mode in [1,2]{let(manager,topics)=background_test_manager(mode);let out=manager.start_background_program("p4-test","focus").unwrap();assert!(!out.confirmed&&out.code=="result_unknown_no_replay");assert_eq!(topics.lock().unwrap().iter().filter(|s|s.as_str()=="widget/program-start").count(),1);assert!(!topics.lock().unwrap().iter().any(|s|s=="widget/program-query"));}
        let(manager,topics)=background_test_manager(4);assert!(!manager.stop_background_program("p4-test","focus","0123456789abcdef","1").unwrap().confirmed);assert_eq!(*topics.lock().unwrap(),["widget/program-stop"]);
    }
    #[test]fn background_controller_legacy_busy_or_incomplete_inventory_emits_no_mutation(){
        let(manager,topics)=background_test_manager(3);assert!(manager.start_background_program("p4-test","focus").is_err());assert_eq!(*topics.lock().unwrap(),["widget/list"]);
        let(manager,topics)=background_test_manager(0);
        {let _guard=manager.asset_transfer_guard.lock().unwrap();assert!(manager.start_background_program("p4-test","focus").is_err());}assert!(topics.lock().unwrap().is_empty());
        manager.connection.lock().unwrap().as_mut().unwrap().capabilities["widgetProgramInspect"]=serde_json::Value::Null;
        assert!(manager.query_background_programs("p4-test").is_err());assert!(topics.lock().unwrap().is_empty());
    }

    #[test]
    fn live_audio_keeps_reader_running_and_never_drains_but_bulk_is_unchanged() {
        struct CheckedWriter { pause: Arc<AtomicBool>, expected: Arc<AtomicBool>, stats: Arc<Mutex<PacedWriterState>>, fail_next: Arc<AtomicBool> }
        impl Write for CheckedWriter {
            fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
                assert_eq!(self.pause.load(Ordering::Acquire), self.expected.load(Ordering::Acquire));
                if self.fail_next.swap(false,Ordering::SeqCst) {
                    return Err(std::io::Error::new(std::io::ErrorKind::TimedOut,"synthetic zero-byte timeout"));
                }
                self.stats.lock().unwrap().writes.push(bytes.to_vec()); Ok(bytes.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                assert!(self.expected.load(Ordering::Acquire), "live traffic must not drain");
                self.stats.lock().unwrap().flushes += 1; Ok(())
            }
        }
        let manager = UsbSerialManager::new();
        let stats = Arc::new(Mutex::new(PacedWriterState::default()));
        let expected = Arc::new(AtomicBool::new(false));
        let active = Arc::new(AtomicBool::new(false));
        let fail_next=Arc::new(AtomicBool::new(false));
        *manager.connection.lock().unwrap() = Some(UsbConnection {
            connection_id: 1, port_name: "test".into(), baud_rate: P4_USB_UART_BAUD,
            writer: Box::new(CheckedWriter {pause: manager.serial_body_write_active.clone(), expected: expected.clone(), stats: stats.clone(), fail_next: fail_next.clone()}),
            board_device_id: "p4-test".into(), runtime: "esp-p4".into(), device_model: String::new(),
            firmware: String::new(), build_id: String::new(), git_sha: String::new(), build_dirty: false,
            protocol_schema: 7, wire_protocol: String::new(), capabilities: serde_json::Value::Null,
            connected: true, cancel_reader: Arc::new(AtomicBool::new(false)), capture_active: active.clone(),
        });
manager.activate_memory_test_connection();
        manager.send("audio/conversation", &serde_json::json!({"enabled":true})).unwrap();
        assert!(active.load(Ordering::Acquire));
        fail_next.store(true,Ordering::SeqCst);
        manager.send("audio/play_chunk", &serde_json::json!({"data":"a".repeat(4000)})).unwrap();
        assert!(manager.connection.lock().unwrap().as_ref().unwrap().connected,"recovered write must not disconnect");
        let transmitted=stats.lock().unwrap().writes.concat();
        let lines=std::str::from_utf8(&transmitted).unwrap().lines().map(|line|serde_json::from_str::<serde_json::Value>(line).unwrap()).collect::<Vec<_>>();
        assert_eq!(lines.len(),2,"each frame is sent exactly once");
        assert_eq!(lines[1]["payload"]["data"],"a".repeat(4000));
        let previous_writes=stats.lock().unwrap().writes.len();
        assert!(manager.send_widget_data("p4-test",&serde_json::json!({"source":"computer.status"})).is_err());
        assert_eq!(stats.lock().unwrap().writes.len(),previous_writes,"widget traffic must yield to audio");
        manager.send("system/heartbeat", &serde_json::json!({})).unwrap();
        manager.send("ui/conversation", &serde_json::json!({"state":"speaking"})).unwrap();
        assert_eq!(stats.lock().unwrap().flushes, 0);
        expected.store(true, Ordering::Release);
        manager.send("firmware/query", &serde_json::json!({})).unwrap();
        #[cfg(target_os = "macos")]
        assert_eq!(stats.lock().unwrap().flushes, 0, "macOS uses peer ACKs, never unbounded tcdrain");
        #[cfg(not(target_os = "macos"))]
        assert!(stats.lock().unwrap().flushes > 0);
        assert!(!manager.serial_body_write_active.load(Ordering::Acquire));
        observe_serial_capture(&active, "audio/end", &serde_json::json!({}));
        assert!(!active.load(Ordering::Acquire));
        expected.store(false, Ordering::Release);
        let before_widget_flushes=stats.lock().unwrap().flushes;
        manager.send_widget_data("p4-test",&serde_json::json!({"source":"computer.status"})).unwrap();
        assert_eq!(stats.lock().unwrap().flushes,before_widget_flushes,"background widgets must not block in driver drain");
    }

    #[test]
    fn session_binding_preflight_rejects_retired_agent_or_generation_without_a_write() {
        let manager = UsbSerialManager::new();
        let recorded = Arc::new(Mutex::new(PacedWriterState::default()));
        *manager.connection.lock().unwrap() = Some(UsbConnection {
            connection_id: 1, port_name: "COM21".into(), baud_rate: P4_USB_UART_BAUD,
            writer: Box::new(PacedWriter(recorded.clone())), board_device_id: "p4-test".into(),
            runtime: "esp-p4".into(), device_model: "ESP32-P4".into(), firmware: "test".into(),
            build_id: String::new(), git_sha: String::new(), build_dirty: false, protocol_schema: 7,
            wire_protocol: "pet-usb-jsonl-v3".into(),
            capabilities: serde_json::json!({"debugBootId":"0123456789abcdef"}), connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)), capture_active: Arc::new(AtomicBool::new(false)),
        });
manager.activate_memory_test_connection();
        { let _lease = manager.inbound_owner.lock().unwrap(); manager.inbound_owner.publish(1); }
        *manager.follow_scope.lock().unwrap() = Some(UsbFollowScope {
            generation: 1, board: "p4-test".into(), agent: "codex".into(), session: None, submitted: true,
        });
        assert!(manager.check_session_follow_target("p4-test", "workbuddy", "1", "0123456789abcdef").is_err());
        assert!(!manager.session_follow_target_is_current("p4-test", "1", "workbuddy"));
        assert!(manager.session_follow_target_is_current("p4-test", "1", "codex"));
        assert!(manager.check_session_follow_target("p4-test", "codex", "2", "0123456789abcdef").is_err());
        assert!(manager.check_session_follow_target("p4-test", "codex", "1", "ffffffffffffffff").is_err());
        assert_eq!(manager.check_session_follow_target("p4-test", "codex", "1", "0123456789abcdef"), Ok(1));
        manager.follow_scope.lock().unwrap().as_mut().unwrap().submitted = false;
        assert!(manager.check_session_follow_target("p4-test", "codex", "1", "0123456789abcdef").is_err());
        assert!(!manager.session_follow_target_is_current("p4-test", "1", "codex"));
        assert!(recorded.lock().unwrap().writes.is_empty());
    }

    #[test]
    fn capture_state_follows_device_without_cross_connection_leakage() {
        let active = AtomicBool::new(false);
        observe_serial_capture(&active, "audio/begin", &serde_json::json!({}));
        assert!(active.load(Ordering::Acquire));
        observe_serial_capture(&active, "audio/status", &serde_json::json!({"active":false,"conversation":true}));
        assert!(active.load(Ordering::Acquire));
        observe_serial_capture(&active, "audio/status", &serde_json::json!({"active":false,"conversation":false}));
        assert!(!active.load(Ordering::Acquire));
        observe_serial_capture(&active, "audio/status", &serde_json::json!({}));
        assert!(!active.load(Ordering::Acquire));
    }

    #[test]
    fn status_returns_the_last_snapshot_while_the_writer_lock_is_busy() {
        let manager = UsbSerialManager::new();
        let recorded = Arc::new(Mutex::new(PacedWriterState::default()));
        *manager.connection.lock().unwrap() = Some(UsbConnection {
            connection_id: u64::MAX,
            port_name: "COM21".into(),
            baud_rate: P4_USB_UART_BAUD,
            writer: Box::new(PacedWriter(recorded)),
            board_device_id: "p4-test".into(),
            runtime: "esp-p4".into(),
            device_model: "ESP32-P4".into(),
            firmware: "0.7.51-p4".into(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 7,
            wire_protocol: "pet-usb-jsonl-v3".into(),
            capabilities: serde_json::json!({"debugBootId":"0123456789abcdef"}),
            connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)),
            capture_active: Arc::new(AtomicBool::new(false)),
        });
manager.activate_memory_test_connection();

        assert_eq!(manager.status().port_name, "COM21");
        assert_eq!(manager.status().connection_id,"18446744073709551615");
        assert_eq!(manager.status().boot_id,"0123456789abcdef");
        let mut legacy=serde_json::to_value(manager.status()).unwrap();
        legacy.as_object_mut().unwrap().remove("connectionId");legacy.as_object_mut().unwrap().remove("bootId");
        let legacy:UsbConnectionStatus=serde_json::from_value(legacy).unwrap();assert!(legacy.connection_id.is_empty()&&legacy.boot_id.is_empty());
        let _writer_guard = manager.connection.lock().unwrap();
        let cached = manager.status();
        assert!(cached.connected);
        assert_eq!(cached.board_device_id, "p4-test");
        assert_eq!(cached.connection_id,"18446744073709551615");assert_eq!(cached.boot_id,"0123456789abcdef");
        drop(_writer_guard);

        // A new board must be visible even before the first successful poll.
        {
            let mut connection = manager.connection.lock().unwrap();
            connection.as_mut().unwrap().capabilities["debugBootId"]="0123456789abcdeF".into();
            assert!(usb_connection_status(connection.as_ref().unwrap()).boot_id.is_empty());
            connection.as_mut().unwrap().board_device_id = "p4-replacement".into();
            cache_usb_status(&manager.status_snapshot, connection.as_ref());
            assert_eq!(manager.status().board_device_id, "p4-replacement");
            connection.as_mut().unwrap().connected = false;
            cache_usb_status(&manager.status_snapshot, connection.as_ref());
            assert!(!manager.status().connected);
            assert!(manager.status().connection_id.is_empty()&&manager.status().boot_id.is_empty());
        }
        manager.disconnect();
        let _guard = manager.connection.lock().unwrap();
        let disconnected = manager.status();
        assert!(!disconnected.connected);
        assert!(disconnected.board_device_id.is_empty());
        assert!(disconnected.connection_id.is_empty()&&disconnected.boot_id.is_empty());
    }

    #[test]
    fn cross_chip_ota_rejects_before_any_serial_write_or_progress() {
        for (revision, name) in [(301, "firmware.bin"), (100, "firmware-v3.bin")] {
            let manager = UsbSerialManager::new();
            let recorded = Arc::new(Mutex::new(PacedWriterState::default()));
            *manager.connection.lock().unwrap() = Some(UsbConnection {
                connection_id: 1,
                port_name: "test-only".into(),
                baud_rate: P4_USB_UART_BAUD,
                writer: Box::new(PacedWriter(Arc::clone(&recorded))),
                board_device_id: "p4-test".into(),
                runtime: "esp-p4".into(),
                device_model: "ESP32-P4".into(),
                firmware: "0.7.51-p4".into(),
                build_id: String::new(), git_sha: String::new(), build_dirty: false,
                protocol_schema: 7, wire_protocol: "pet-usb-jsonl-v3".into(),
                capabilities: serde_json::json!({"firmwareUpdate":{"chipRevision":revision}}),
                connected: true, cancel_reader: Arc::new(AtomicBool::new(false)),
                capture_active: Arc::new(AtomicBool::new(false)),
            });
manager.activate_memory_test_connection();
            let image = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("firmware/esp32-p4").join(name);
            let result = manager.update_firmware(&image, "p4-test",
                |_,_,_| panic!("incompatible firmware must not start transfer"),
                || panic!("incompatible firmware must not reconnect"));
            assert!(result.unwrap_err().contains("芯片版本不匹配"));
            assert!(recorded.lock().unwrap().writes.is_empty());
        }
    }

    #[test]
    fn control_transactions_wait_for_an_active_asset_transfer() {
        let manager = UsbSerialManager::new();
        let transfer_manager = manager.clone();
        let (transfer_started_tx, transfer_started_rx) = mpsc::channel();
        let (release_transfer_tx, release_transfer_rx) = mpsc::channel();
        let transfer_thread = thread::spawn(move || {
            transfer_manager.with_asset_transfer_guard(|| {
                transfer_started_tx.send(()).unwrap();
                release_transfer_rx.recv().unwrap();
                Ok(())
            })
        });
        transfer_started_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("asset transfer should acquire the guard");

        let control_manager = manager.clone();
        let (control_started_tx, control_started_rx) = mpsc::channel();
        let control_thread = thread::spawn(move || {
            control_manager.with_asset_transfer_guard(|| {
                control_started_tx.send(()).unwrap();
                Ok(())
            })
        });
        assert!(control_started_rx
            .recv_timeout(Duration::from_millis(50))
            .is_err());

        release_transfer_tx.send(()).unwrap();
        transfer_thread.join().unwrap().unwrap();
        control_started_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("control transaction should resume after the asset transfer");
        control_thread.join().unwrap().unwrap();
    }

    #[test]
    fn app_snapshots_share_media_chunk_boundaries_without_blocking_reader() {
        let manager=UsbSerialManager::new();
        let _priority=media_arbitration::PendingMedia::new(&manager.media_requests_pending);
        // A pending stream isn't a reason to suppress all control feedback.
        assert_eq!(manager.send_app_snapshot("test",&serde_json::json!({})).unwrap_err(),"Not connected");
        let copy=manager.clone();let (tx,rx)=mpsc::channel();
        let writer=thread::spawn(move||{let _guard=copy.asset_transfer_guard.lock().unwrap();tx.send(()).unwrap();thread::sleep(Duration::from_millis(70));});
        rx.recv().unwrap();let at=Instant::now();
        assert!(manager.try_send_app_snapshot("test",&serde_json::json!({})).unwrap_err().contains("资源同步"));
        assert!(at.elapsed()<Duration::from_millis(50));
        assert_eq!(manager.send_app_snapshot("test",&serde_json::json!({})).unwrap_err(),"Not connected");
        writer.join().unwrap();
        assert!(manager.send_app_snapshot("test",&serde_json::json!({"values":"x".repeat(2049)})).unwrap_err().contains("过大"));
        let _bulk=manager.asset_transfer_guard.lock().unwrap();let at=Instant::now();
        assert!(manager.send_app_snapshot("test",&serde_json::json!({})).unwrap_err().contains("下一次状态更新"));
        assert!(at.elapsed()<Duration::from_secs(1));
    }

    #[test]
    fn widget_snapshots_yield_to_media_and_priority_cleans_up_on_send_error() {
        let manager = UsbSerialManager::new();
        let priority = media_arbitration::PendingMedia::new(&manager.media_requests_pending);
        assert!(manager.send_widget_data("test", &serde_json::json!({})).unwrap_err().contains("音乐正在传输"));
        drop(priority);
        assert!(manager.media_request("test", "media/query", serde_json::json!({})).is_err());
        assert_eq!(manager.media_requests_pending.load(Ordering::SeqCst), 0);
        assert!(manager.device_response_waiters.lock().unwrap().is_empty());
    }

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
    fn cancelled_current_p4_asset_transaction_never_implicitly_aborts() {
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
            firmware: "0.7.35-p4".to_string(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 5,
            wire_protocol: "pet-usb-jsonl-v3".to_string(),
            capabilities: serde_json::json!({
                "transport": {
                    "rawAssetChunks": true,
                    "rawAssetChunkBytes": P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE,
                },
            }),
            connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)),
            capture_active: Arc::new(AtomicBool::new(false)),
        });
manager.activate_memory_test_connection();
        manager.begin_appearance_sync().unwrap();
        assert!(manager.cancel_appearance_sync());

        let result = manager.run_serial_asset_transaction("cancel-test", || {
            manager.ensure_appearance_sync_not_cancelled(Some("cancel-test"))
        });

        assert_eq!(result.unwrap_err(), APPEARANCE_SYNC_CANCELLED_ERROR);
        let output = String::from_utf8(written.lock().unwrap().clone()).unwrap();
        let messages = output
            .lines()
            .map(|line| serde_json::from_str::<serde_json::Value>(line).unwrap())
            .collect::<Vec<_>>();
        assert!(messages.is_empty(),"cancel cannot prove whether the prior mutation committed");
        manager.finish_appearance_sync();
    }

    #[test]
    fn usb_component_override_accepts_combined_encoder_rotation() {
        assert_eq!(canonical_binding_for_control("SW1 长按"), Some(("SW1", "button.sw1.long_press")));
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

    #[test]
    fn local_extended_deck_rejects_historical_quota_before_transport_or_repair() {
        use serde_json::json;
        struct NoWrites;
        impl Write for NoWrites {
            fn write(&mut self,_:&[u8])->std::io::Result<usize>{panic!("local capability rejection must precede transport");}
            fn flush(&mut self)->std::io::Result<()>{panic!("local capability rejection must not flush");}
        }
        for count in [17,24,25,32,33,64] {
            let dir=tempfile::tempdir().unwrap();
            std::fs::create_dir(dir.path().join("runtime")).unwrap();
            let runtime=json!({"local":{"kind":"deck","groups":[{"items":vec![json!({});count]}]}});
            std::fs::write(dir.path().join("runtime/widget.json"),serde_json::to_vec(&runtime).unwrap()).unwrap();
            for repair in [false,true] {
                let manager=UsbSerialManager::new();
                *manager.connection.lock().unwrap()=Some(UsbConnection{
                    connection_id:1,port_name:"synthetic-only".into(),baud_rate:P4_USB_UART_BAUD,
                    writer:Box::new(NoWrites),board_device_id:"p4-mask-test".into(),runtime:"esp-p4".into(),
                    device_model:String::new(),firmware:String::new(),build_id:String::new(),git_sha:String::new(),build_dirty:false,
                    protocol_schema:7,wire_protocol:String::new(),
                    capabilities:json!({"widgetLocalTools":"p4-local-tools-v1","widgetQuota":"p4-quota-v3","maxLocalItems":64}),connected:true,
                    cancel_reader:Arc::new(AtomicBool::new(false)),capture_active:Arc::new(AtomicBool::new(false)),
                });
                manager.activate_memory_test_connection();
                let error=manager.install_widget_clawpkg_mode("p4-mask-test","synthetic-mask",dir.path(),&HashMap::new(),|_,_,_|{},repair).unwrap_err();
                assert!(error.contains("p4-local-masks-v2"),"count={count} repair={repair}: {error}");
            }
        }
    }

    #[test]
    fn app_form_transfer_rejects_old_capability_and_bad_overrides_before_any_write() {
        use serde_json::json;
        struct NoWrites;
        impl Write for NoWrites {
            fn write(&mut self,_:&[u8])->std::io::Result<usize>{panic!("form validation must precede widget/begin");}
            fn flush(&mut self)->std::io::Result<()>{panic!("no transport operations during rejection");}
        }
        for with_png in [false,true] {
            let dir=tempfile::tempdir().unwrap();
            std::fs::create_dir(dir.path().join("runtime")).unwrap();
            std::fs::create_dir(dir.path().join("assets")).unwrap();
            let (mut runtime,buttons)=crate::component_extension::app_form::fixture();
            if with_png {
                runtime["surface"]["images"]=json!([{"id":"tiny","asset":"assets/tiny.png","frame_width":8,"frame_height":8,"frames":1,"fps":1}]);
                runtime["surface"]["elements"].as_array_mut().unwrap().push(json!({"type":"image","x":250,"y":20,"w":8,"h":8,"color":65535,"image":"tiny"}));
                image::RgbaImage::from_pixel(8,8,image::Rgba([20,40,60,255])).save(dir.path().join("assets/tiny.png")).unwrap();
            }
            std::fs::write(dir.path().join("runtime/widget.json"),serde_json::to_vec(&runtime).unwrap()).unwrap();
            std::fs::write(dir.path().join("buttons.json"),serde_json::to_vec(&buttons).unwrap()).unwrap();
            for bad_override in [None,Some("SW3 短按"),Some("SW2 短按")] {
                let manager=UsbSerialManager::new();
                let mut capabilities=json!({"widgetAppSurface":"p4-app-surface-v1","widgetAppGraphics":"p4-app-graphics-v1","widgetVariableBindings":"p4-variable-bindings-v1","widgetInput":"p4-input-v1","widgetSurfaceImages":"p4-surface-images-v1"});
                if bad_override.is_some(){capabilities["widgetAppForm"]=json!(crate::component_extension::app_form::ENGINE);}
                *manager.connection.lock().unwrap()=Some(UsbConnection{
                    connection_id:1,port_name:"synthetic-only".into(),baud_rate:P4_USB_UART_BAUD,
                    writer:Box::new(NoWrites),board_device_id:"p4-form-test".into(),runtime:"esp-p4".into(),
                    device_model:String::new(),firmware:String::new(),build_id:String::new(),git_sha:String::new(),build_dirty:false,
                    protocol_schema:7,wire_protocol:String::new(),capabilities,connected:true,
                    cancel_reader:Arc::new(AtomicBool::new(false)),capture_active:Arc::new(AtomicBool::new(false)),
                });
manager.activate_memory_test_connection();
                let overrides=bad_override.map(|control|HashMap::from([("edit.accept".to_string(),control.to_string())])).unwrap_or_default();
                let error=manager.install_widget_clawpkg_on_board("p4-form-test","synthetic-form",dir.path(),&overrides,|_,_,_|{}).unwrap_err();
                assert!(error.contains(if bad_override.is_none(){"有界表单能力"}else{"表单需要"}),"{with_png}: {error}");
            }
        }
    }

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
    fn actual_reader_checks_control_bytes_before_json_and_retains_fragmented_tail() {
        use std::collections::VecDeque;
        struct Pieces { chunks:VecDeque<Result<Vec<u8>,std::io::ErrorKind>> }
        impl Read for Pieces {
            fn read(&mut self,out:&mut [u8])->std::io::Result<usize>{
                match self.chunks.pop_front(){
                    None=>Ok(0),Some(Err(kind))=>Err(kind.into()),Some(Ok(bytes))=>{
                        let n=out.len().min(bytes.len());out[..n].copy_from_slice(&bytes[..n]);
                        if n<bytes.len(){self.chunks.push_front(Ok(bytes[n..].to_vec()));}Ok(n)
                    }
                }
            }
        }
        let body="{\"topic\":\"protocol/ack\",\"payload\":{\"value\":\"中文😀\"}}";
        let mut guarded=format!("!P4C1:{:05}:{:08x}:",body.len(),control_integrity::fingerprint(body.as_bytes())).into_bytes();
        guarded.extend_from_slice(body.as_bytes());guarded.push(b'\n');
        let following=b"{\"topic\":\"next\",\"payload\":{}}\n";
        for cut in 1..guarded.len(){
            let input=Pieces { chunks:VecDeque::from([Ok(guarded[..cut].to_vec()),Err(std::io::ErrorKind::TimedOut),
                Ok(guarded[cut..].to_vec()),Ok(following.to_vec())]) };
            let mut reader=BufReader::with_capacity(7,input);let mut pending=Vec::new();
            assert!(matches!(read_serial_frame(&mut reader,&mut pending),Err(ref e) if e.kind()==std::io::ErrorKind::TimedOut));
            let frame=read_serial_frame(&mut reader,&mut pending).unwrap().unwrap();
            assert!(frame.integrity.framed&&frame.integrity.failure.is_none()&&frame.valid_utf8);
            assert_eq!(parse_serial_frame(&frame).unwrap().payload["value"],"中文😀");
            let tail=read_serial_frame(&mut reader,&mut pending).unwrap().unwrap();
            assert_eq!(parse_serial_frame(&tail).unwrap().topic,"next");assert!(!tail.integrity.framed);
            assert!(!control_integrity::permitted(tail.integrity,true,"protocol/ack"));
        }
        let mut damaged=guarded.clone();let at=damaged.iter().rposition(|b|*b==b'v').unwrap();damaged[at]=b'w';
        assert!(serde_json::from_slice::<serde_json::Value>(&damaged[21..damaged.len()-1]).is_ok());
        let mut reader=BufReader::new(std::io::Cursor::new([damaged,following.to_vec()].concat()));let mut pending=Vec::new();
        let bad=read_serial_frame(&mut reader,&mut pending).unwrap().unwrap();
        assert_eq!(bad.integrity.failure,Some(control_integrity::Failure::Hash));assert!(parse_serial_frame(&bad).is_none());
        assert_eq!(parse_serial_frame(&read_serial_frame(&mut reader,&mut pending).unwrap().unwrap()).unwrap().topic,"next");
        // A checksum must NEVER make invalid UTF-8 acceptable by replacement.
        let raw=b"{\"topic\":\"reply\",\"payload\":{\"text\":\"\xff\"}}";
        let mut invalid=format!("!P4C1:{:05}:{:08x}:",raw.len(),control_integrity::fingerprint(raw)).into_bytes();invalid.extend_from_slice(raw);invalid.push(b'\n');
        let mut reader=BufReader::new(std::io::Cursor::new(invalid));let bad=read_serial_frame(&mut reader,&mut pending).unwrap().unwrap();
        assert!(bad.integrity.failure.is_none()&&!bad.valid_utf8&&parse_serial_frame(&bad).is_none());
    }

    #[test]
    fn invalid_utf8_json_is_never_repaired_into_a_protocol_message() {
        let damaged = b"{\"topic\":\"hello\",\"payload\":{\"deviceId\":\"bo\xffard\"}}\n";
        let valid = "{\"topic\":\"next\",\"payload\":{\"text\":\"中文😀\u{fffd}\"}}\r\n";
        let input = [damaged.as_slice(), valid.as_bytes()].concat();
        for capacity in [1, 7, 64, 8192] {
            let mut reader = BufReader::with_capacity(capacity, std::io::Cursor::new(&input));
            let mut buffer = Vec::new();
            let frame = read_serial_frame(&mut reader, &mut buffer).unwrap().unwrap();
            assert!(!frame.valid_utf8);
            // This is precisely the former bug: syntactically valid JSON after
            // replacement could have resolved an ACK or changed device identity.
            assert!(parse_serial_message(&frame.text).is_some());
            assert!(parse_serial_frame(&frame).is_none());
            assert!(buffer.is_empty());
            let following = read_serial_frame(&mut reader, &mut buffer).unwrap().unwrap();
            assert!(following.valid_utf8);
            let message = parse_serial_frame(&following).unwrap();
            assert_eq!(message.topic, "next");
            // A deliberately encoded U+FFFD is valid data, not evidence of loss.
            assert_eq!(message.payload["text"], "中文😀\u{fffd}");
            assert!(read_serial_frame(&mut reader, &mut buffer).unwrap().is_none());
        }
    }

    #[test]
    fn non_utf8_boot_frame_is_discarded_whole_before_next_valid_frame() {
        let input = b"\xff\xfe boot {\"topic\":\"not_salvaged\"}\n{\"topic\":\"hello\",\"payload\":{}}\n";
        let mut reader = BufReader::with_capacity(7, std::io::Cursor::new(input));
        let mut buffer = Vec::new();
        let boot = read_serial_frame(&mut reader, &mut buffer).unwrap().unwrap();
        assert!(!boot.valid_utf8 && parse_serial_frame(&boot).is_none());
        let next = read_serial_frame(&mut reader, &mut buffer).unwrap().unwrap();
        assert!(next.valid_utf8);
        assert_eq!(parse_serial_frame(&next).unwrap().topic, "hello");
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
    fn serial_probe_handoff_retains_prefetched_frames() {
        let hello = b"{\"topic\":\"hello\",\"payload\":{}}\n";
        let next = b"{\"topic\":\"debug/identity\",\"payload\":{\"requestId\":\"local-proof\"}}\n";
        let final_line = b"{\"topic\":\"audio/status\",\"payload\":{\"active\":false}}\n";
        let input = [hello.as_slice(), next.as_slice(), final_line.as_slice()].concat();
        // Reproduce the old handoff with exactly the same production line reader.
        let mut old = BufReader::new(std::io::Cursor::new(&input));
        let mut line = Vec::new();
        assert_eq!(parse_serial_message(&read_serial_line_lossy(&mut old, &mut line).unwrap().unwrap()).unwrap().topic, "hello");
        assert!(!old.buffer().is_empty());
        let mut discarded = BufReader::new(old.into_inner());
        assert!(read_serial_line_lossy(&mut discarded, &mut line).unwrap().is_none());
        // Includes a partially prefetched next frame, several complete frames,
        // and buffers too small to prefetch. No scanning/salvaging/replay added.
        for capacity in [1, 7, 32, 64, 512, 8192] {
            let mut reader = BufReader::with_capacity(capacity, std::io::Cursor::new(&input));
            let first = read_serial_line_lossy(&mut reader, &mut line).unwrap().unwrap();
            let message = parse_serial_message(&first).unwrap();
            let prefetched = reader.buffer().to_vec();
            let mut connected = ProbedSerialPort::from_verified_reader(Vec::<u8>::new(), reader, 4_000_000, message);
            assert_eq!(connected.reader.buffer(), prefetched);
            assert_eq!(connected.hello.topic, "hello");
            assert_eq!(connected.baud, 4_000_000);
            for expected in [next.as_slice(), final_line.as_slice()] {
                assert_eq!(read_serial_line_lossy(&mut connected.reader, &mut line).unwrap().unwrap().as_bytes(), &expected[..expected.len()-1]);
            }
            assert!(read_serial_line_lossy(&mut connected.reader, &mut line).unwrap().is_none());
        }
    }

    #[test]
    fn probe_and_connected_reads_share_bounded_transient_policy_without_losing_prefix() {
        use std::collections::VecDeque;
        struct Fragments(VecDeque<Result<Vec<u8>, std::io::ErrorKind>>);
        impl std::io::Read for Fragments {
            fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
                match self.0.pop_front() {
                    Some(Err(kind)) => Err(kind.into()),
                    Some(Ok(mut bytes)) => {
                        let count=bytes.len().min(out.len());
                        out[..count].copy_from_slice(&bytes[..count]);
                        if count<bytes.len() { self.0.push_front(Ok(bytes.split_off(count))); }
                        Ok(count)
                    }
                    None => Ok(0),
                }
            }
        }
        let line=b"{\"topic\":\"hello\",\"payload\":{\"ok\":true}}\n";
        for kind in [std::io::ErrorKind::TimedOut,std::io::ErrorKind::Interrupted,std::io::ErrorKind::WouldBlock] {
            for cut in 1..line.len() {
                let mut reader=BufReader::new(Fragments(VecDeque::from([
                    Ok(line[..cut].to_vec()),Err(kind),Err(kind),Ok(line[cut..].to_vec())])));
                let mut pending=Vec::new();
                for _ in 0..2 {
                    let error=match read_serial_frame(&mut reader,&mut pending) {
                        Err(error) => error,
                        Ok(_) => panic!("transient read must preserve the unfinished frame"),
                    };
                    assert_eq!(error.kind(),kind);
                    let delay=serial_read_recovery_delay(&error).unwrap();
                    assert_eq!(delay,if kind==std::io::ErrorKind::TimedOut {Duration::ZERO}else{Duration::from_millis(2)});
                    assert_eq!(pending,&line[..cut]);
                }
                let frame=read_serial_frame(&mut reader,&mut pending).unwrap().unwrap();
                assert_eq!(parse_serial_frame(&frame).unwrap().topic,"hello");
                assert!(pending.is_empty());
                assert!(read_serial_frame(&mut reader,&mut pending).unwrap().is_none());
            }
        }
        for kind in [std::io::ErrorKind::BrokenPipe,std::io::ErrorKind::PermissionDenied,
            std::io::ErrorKind::InvalidData,std::io::ErrorKind::UnexpectedEof] {
            assert_eq!(serial_read_recovery_delay(&kind.into()),None);
        }
    }

    #[test]
    fn serial_frames_survive_every_timeout_split_and_repeated_timeouts() {
        use std::collections::VecDeque;
        struct Fragmented(VecDeque<Option<Vec<u8>>>,std::io::ErrorKind);
        impl std::io::Read for Fragmented {
            fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
                match self.0.pop_front() {
                    Some(None) => Err(self.1.into()),
                    Some(Some(bytes)) => {
                        out[..bytes.len()].copy_from_slice(&bytes);
                        Ok(bytes.len())
                    }
                    None => Ok(0),
                }
            }
        }
        for (kind, topic) in [std::io::ErrorKind::TimedOut, std::io::ErrorKind::WouldBlock, std::io::ErrorKind::Interrupted]
            .into_iter().flat_map(|kind| ["hello", "firmware/chunk_ack"].map(|topic| (kind, topic))) {
            let line = format!("{{\"topic\":\"{topic}\",\"payload\":{{\"ok\":true}}}}\r\n");
            for split in 1..line.len() - 1 {
                let port = Fragmented(VecDeque::from([
                    Some(line.as_bytes()[..split].to_vec()),
                    None,
                    None,
                    Some(line.as_bytes()[split..].to_vec()),
                    Some(b"{\"topic\":\"next\",\"payload\":{}}\n".to_vec()),
                ]),kind);
                let mut reader = BufReader::new(port);
                let mut buffer = Vec::new();
                for _ in 0..2 {
                    assert_eq!(
                        read_serial_line_lossy(&mut reader, &mut buffer)
                            .unwrap_err()
                            .kind(),
                        kind
                    );
                }
                let frame = read_serial_line_lossy(&mut reader, &mut buffer)
                    .unwrap()
                    .unwrap();
                assert_eq!(frame, line.trim_end());
                assert_eq!(parse_serial_message(&frame).unwrap().topic, topic);
                assert!(buffer.is_empty());
                assert_eq!(
                    parse_serial_message(
                        &read_serial_line_lossy(&mut reader, &mut buffer)
                            .unwrap()
                            .unwrap()
                    )
                    .unwrap()
                    .topic,
                    "next"
                );
            }
        }
    }

    #[test]
    fn screenshot_serial_frames_preserve_base64_across_fragments_and_timeouts() {
        use base64::{engine::general_purpose::STANDARD, Engine as _};
        use std::collections::VecDeque;

        struct Fragmented {
            reads: VecDeque<Result<Vec<u8>, std::io::ErrorKind>>,
        }
        impl std::io::Read for Fragmented {
            fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
                match self.reads.pop_front() {
                    Some(Ok(bytes)) => {
                        let count = out.len().min(bytes.len());
                        out[..count].copy_from_slice(&bytes[..count]);
                        if count < bytes.len() {
                            self.reads.push_front(Ok(bytes[count..].to_vec()));
                        }
                        Ok(count)
                    }
                    Some(Err(kind)) => Err(kind.into()),
                    None => Ok(0),
                }
            }
        }

        let raw = (0..3072).map(|i| ((i * 73 + i / 7) % 256) as u8).collect::<Vec<_>>();
        let encoded = STANDARD.encode(&raw);
        assert_eq!(encoded.len(), 4096);
        let checksum = raw.iter().fold(2166136261u32, |sum, byte|
            (sum ^ u32::from(*byte)).wrapping_mul(16777619));
        let payload = serde_json::json!({"id":"offline-frame","index":23,"rawSize":3072,"checksum":format!("{checksum:08x}"),"data":encoded});
        let line = format!("{}\r\n", serde_json::json!({"topic":"debug/screenshot_chunk","payload":payload}));
        let following = "{\"topic\":\"debug/screenshot_end\",\"payload\":{\"id\":\"offline-frame\",\"chunks\":200}}\n";
        let verify_chunk = |frame: &str| {
            assert_eq!(frame, line.trim_end_matches(['\r', '\n']));
            let message = parse_serial_message(frame).unwrap();
            assert_eq!(message.topic, "debug/screenshot_chunk");
            assert_eq!(message.payload, payload);
            let data = message.payload["data"].as_str().unwrap();
            assert_eq!(data.len(), 4096);
            assert_eq!(STANDARD.decode(data).unwrap(), raw);
        };
        let verify_following = |reader: &mut BufReader<Fragmented>, buffer: &mut Vec<u8>| {
            let next = read_serial_line_lossy(reader, buffer).unwrap().unwrap();
            assert_eq!(next, following.trim_end());
            assert_eq!(parse_serial_message(&next).unwrap().topic, "debug/screenshot_end");
            assert!(buffer.is_empty());
        };

        // Exercise every boundary, including JSON syntax, all base64 bytes,
        // and CR/LF. Temporary errors never mean that a prefix may be dropped.
        for kind in [std::io::ErrorKind::TimedOut, std::io::ErrorKind::WouldBlock, std::io::ErrorKind::Interrupted] {
            for split in 1..line.len() {
                let mut rest = line.as_bytes()[split..].to_vec();
                rest.extend_from_slice(following.as_bytes());
                let mut reader = BufReader::new(Fragmented { reads: VecDeque::from([
                    Ok(line.as_bytes()[..split].to_vec()), Err(kind), Err(kind), Ok(rest),
                ]) });
                let mut buffer = Vec::new();
                for _ in 0..2 {
                    assert_eq!(read_serial_line_lossy(&mut reader, &mut buffer).unwrap_err().kind(), kind,
                               "split={split}, kind={kind:?}");
                    assert_eq!(buffer, line.as_bytes()[..split], "split={split}, kind={kind:?}");
                }
                verify_chunk(&read_serial_line_lossy(&mut reader, &mut buffer).unwrap().unwrap());
                verify_following(&mut reader, &mut buffer);
            }
        }

        // A single syscall can also end inside either line or return both;
        // packet-sized fragmentation must not add, omit or consume ahead bytes.
        let stream = [line.as_bytes(), following.as_bytes()].concat();
        for size in [1, 63, 64, 127, 128, 129, 255, 1023, 4095, 4096, 8192] {
            let reads = stream.chunks(size).map(|chunk| Ok(chunk.to_vec())).collect();
            let mut reader = BufReader::new(Fragmented { reads });
            let mut buffer = Vec::new();
            verify_chunk(&read_serial_line_lossy(&mut reader, &mut buffer).unwrap().unwrap());
            verify_following(&mut reader, &mut buffer);
        }
    }

    #[test]
    fn serial_noise_is_bounded_and_eof_discards_an_incomplete_frame() {
        let mut reader = BufReader::new(std::io::Cursor::new(vec![b'x'; 256 * 1024 + 1]));
        let mut buffer = Vec::new();
        assert_eq!(
            read_serial_line_lossy(&mut reader, &mut buffer)
                .unwrap_err()
                .kind(),
            std::io::ErrorKind::InvalidData
        );
        assert!(buffer.is_empty());
        let mut reader = BufReader::new(std::io::Cursor::new(b"incomplete"));
        assert!(read_serial_line_lossy(&mut reader, &mut buffer)
            .unwrap()
            .is_none());
        assert!(buffer.is_empty());
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
    fn rejects_usb_serial_devices_without_an_assigned_port_name() {
        for vid in [0x1d6b, 0x0525, 0x303a, 0x1a86, 0x10c4, 0x0403] {
            assert!(!is_supported_usb_serial_port("", vid));
            assert!(!is_supported_usb_serial_port("   ", vid));
        }
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
                    P4_UNIFIED_UART_BAUD,
                    P4_COMPAT_UART_BAUD,
                    P4_USB_UART_TRANSITION_BAUD,
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
        assert_eq!(SERIAL_AUTO_PROBE_TIMEOUT, Duration::from_millis(1500));
        assert!(SERIAL_AUTO_PROBE_TIMEOUT < SERIAL_PROBE_TIMEOUT);
        assert_eq!(serial_probe_attempt_timeout(0, P4_UNIFIED_UART_BAUD,
            SERIAL_AUTO_PROBE_TIMEOUT), SERIAL_PROBE_TIMEOUT);
        assert!(SERIAL_PROBE_HANDSHAKE_RETRY_INTERVAL <= Duration::from_secs(1));
        assert_eq!(messages[0]["topic"], "bind");
        assert_eq!(messages[0]["payload"]["desktopDeviceId"], "desktop-1");
        assert_eq!(messages[1]["type"], "hello");
        assert!(messages.iter().all(|message| message["topic"] != "ack"));
    }

    #[test]
    fn first_p4_probe_covers_boot_without_lengthening_every_unknown_port() {
        let short = Duration::from_millis(1500);
        for baud in [P4_UNIFIED_UART_BAUD, P4_COMPAT_UART_BAUD] {
            assert!(serial_probe_attempt_timeout(0, baud, short) >= Duration::from_secs(8));
            assert_eq!(serial_probe_attempt_timeout(1, baud, short), short);
            assert_eq!(serial_probe_attempt_timeout(0, baud, Duration::from_secs(12)), Duration::from_secs(12));
        }
        let first_unknown = if cfg!(windows) { short } else { SERIAL_PROBE_TIMEOUT };
        for baud in [DEFAULT_USB_SERIAL_BAUD, LEGACY_USB_SERIAL_BAUD] {
            assert_eq!(serial_probe_attempt_timeout(0, baud, short), first_unknown);
            assert_eq!(serial_probe_attempt_timeout(1, baud, short), short);
        }
    }

    #[test]
    fn auto_probe_retains_boot_window_and_bounds_fallback_bauds_on_all_platforms() {
        for first in [P4_UNIFIED_UART_BAUD, P4_COMPAT_UART_BAUD] {
            let candidates = [first, P4_USB_UART_TRANSITION_BAUD,
                DEFAULT_USB_SERIAL_BAUD, LEGACY_USB_SERIAL_BAUD, first];
            let windows: Vec<_> = candidates.iter().enumerate().map(|(index, baud)|
                serial_probe_attempt_timeout(index, *baud, SERIAL_AUTO_PROBE_TIMEOUT)).collect();
            assert_eq!(windows[0], Duration::from_secs(8));
            assert!(windows[1..].iter().all(|window| *window == Duration::from_millis(1500)));
            assert_eq!(windows.into_iter().sum::<Duration>(), Duration::from_secs(14));
        }
    }

    #[test]
    fn manual_probe_preserves_full_window_for_every_legacy_fallback() {
        for (index, baud) in [P4_UNIFIED_UART_BAUD, P4_COMPAT_UART_BAUD,
            P4_USB_UART_TRANSITION_BAUD, DEFAULT_USB_SERIAL_BAUD,
            LEGACY_USB_SERIAL_BAUD].iter().enumerate() {
            assert_eq!(serial_probe_attempt_timeout(index, *baud, SERIAL_PROBE_TIMEOUT),
                Duration::from_secs(8));
        }
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
                board_device_id: "p4-test".into(),
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
                capture_active: Arc::new(AtomicBool::new(false)),
            });
        }
        {let _lease=manager.inbound_owner.lock().unwrap();manager.inbound_owner.publish(1);}
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
        manager.with_appearance_attempt("p4-test",||manager.send_asset_chunk_checked(
            "chunk-test","p4/families/idle.default.h264","aGVsbG8=",5,0,
        )).unwrap();

        assert!(started.elapsed() >= Duration::from_millis(40));
    }

    #[test]
    fn raw_asset_chunk_writes_json_header_then_unencoded_bytes() {
        let manager = UsbSerialManager::new();
        let recorded = Arc::new(Mutex::new(PacedWriterState::default()));
        {
            let mut conn = manager.connection.lock().unwrap();
            *conn = Some(UsbConnection {
                connection_id: 1,
                port_name: "COM15".to_string(),
                baud_rate: P4_USB_UART_BAUD,
                writer: Box::new(PacedWriter(Arc::clone(&recorded))),
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
                capture_active: Arc::new(AtomicBool::new(false)),
            });
        }
        {let _lease=manager.inbound_owner.lock().unwrap();manager.inbound_owner.publish(1);}
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

        let mut chunk = vec![0u8; P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE];
        for (index, byte) in chunk.iter_mut().enumerate() {
            *byte = (index % 251) as u8;
        }
        manager.with_appearance_attempt("p4-test",||manager.send_asset_raw_chunk_checked(
            "raw-test","p4/families/idle.default.h264",&chunk,7,
        )).unwrap();

        let recorded = recorded.lock().unwrap();
        assert!(recorded.writes.len() >= 2);
        #[cfg(any(target_os = "macos", windows))]
        {
            let mut header_complete = false;
            for write in &recorded.writes {
                let slice_limit = if header_complete {
                    P4_CH343_RAW_WRITE_SLICE_BYTES
                } else {
                    P4_CH343_SERIAL_WRITE_SLICE_BYTES
                };
                assert!(write.len() <= slice_limit);
                if !header_complete && write.contains(&b'\n') {
                    header_complete = true;
                }
            }
            assert!(header_complete);
        }
        #[cfg(target_os = "macos")]
        assert_eq!(recorded.flushes, 0);
        #[cfg(windows)]
        {
            assert!(recorded.writes.len() > 2);
            assert_eq!(recorded.flushes, 2);
        }
        #[cfg(all(not(target_os = "macos"), not(windows)))]
        {
            assert_eq!(recorded.writes.len(), 2);
            assert_eq!(recorded.flushes, 2);
        }
        let bytes = recorded.writes.concat();
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

    // Raw unknown-result behavior is covered by lifecycle_safety_tests using
    // the actual header/body sender; no padding-recovery API remains.

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
            capture_active: Arc::new(AtomicBool::new(false)),
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
    fn partial_hello_preserves_display_metadata_but_invalidates_capability_identity() {
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
            capture_active: Arc::new(AtomicBool::new(false)),
        };

        // First establish a complete HELLO; later partial HELLOs cannot inherit
        // the old boot's capability/nonce/identity proof, even on the same port.
        apply_hello_payload_to_connection(&mut conn, &serde_json::json!({
            "boardDeviceId":"old-board", "runtime":"esp-p4", "firmware":"0.5.0-p4",
            "capabilities":{"usbOnly":true,"debugBootId":"0123456789abcdef"}
        }));
        assert_eq!(conn.capabilities["usbOnly"], true);
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
        assert!(conn.capabilities.is_null());
        apply_hello_payload_to_connection(&mut conn, &serde_json::json!({
            "boardDeviceId":"new-board", "runtime":"esp-p4", "firmware":"0.6.0-p4",
            "capabilities":{"usbOnly":false,"debugBootId":"1123456789abcdef"}
        }));
        assert_eq!(conn.firmware, "0.6.0-p4");
        assert_eq!(conn.capabilities["debugBootId"], "1123456789abcdef");
        apply_hello_payload_to_connection(&mut conn, &serde_json::json!({"boardDeviceId":"new-board"}));
        assert!(conn.capabilities.is_null(), "same-board partial HELLO is not a capability patch");
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
    fn p4_ffmpeg_args_use_only_platform_h264_encoders() {
        for encoder in [
            crate::codex_import::PlatformH264Encoder::VideoToolbox,
            crate::codex_import::PlatformH264Encoder::MediaFoundation,
        ] {
            let args = build_p4_h264_ffmpeg_args(
                Path::new("input.mp4"),
                Path::new("out.h264"),
                P4_APPEARANCE_WIDTH,
                P4_APPEARANCE_HEIGHT,
                f64::from(P4_APPEARANCE_FPS),
                P4_APPEARANCE_MAX_FRAMES,
                encoder,
            );
            let joined = args.join(" ");

            assert!(joined.contains("fps=15"));
            assert!(joined.contains("scale=640:480:force_original_aspect_ratio=decrease"));
            assert!(joined.contains("pad=640:480:(ow-iw)/2:(oh-ih)/2:black"));
            assert!(joined.contains("-frames:v 225"));
            assert!(joined.contains(&format!("-c:v {}", encoder.codec_name())));
            // Assert the external encoder contract, not the value returned by
            // the implementation under test (which hid the Windows regression).
            match encoder {
                crate::codex_import::PlatformH264Encoder::MediaFoundation => {
                    assert!(args.windows(2).any(|pair| pair == ["-profile:v", "66"]));
                    assert!(args.windows(2).any(|pair| pair == ["-level:v", "30"]));
                    assert!(args.windows(2).any(|pair| pair == ["-slices", "1"]));
                    assert!(!args.iter().any(|arg| arg == "h264_metadata=aud=insert"));
                    assert!(!args.iter().any(|arg| arg == "baseline"));
                }
                crate::codex_import::PlatformH264Encoder::VideoToolbox => {
                    assert!(args
                        .windows(2)
                        .any(|pair| pair == ["-profile:v", "constrained_baseline"]));
                    assert!(args.windows(2).any(|pair| pair == ["-level:v", "3.0"]));
                    assert!(args.windows(2).any(|pair| pair == ["-bsf:v", "h264_metadata=aud=insert"]));
                    assert!(!args.iter().any(|arg| arg == "-slices"));
                }
            }
            assert!(joined.contains("-pix_fmt nv12"));
            assert!(!joined.contains("libx264"));
            assert!(!joined.contains("libopenh264"));
            assert!(!joined.contains("-crf"));
            assert!(joined.contains("-f h264"));
        }
    }

    #[test]
    #[ignore = "requires PET_MANAGER_FFMPEG_SMOKE_INPUT and a host LGPL FFmpeg runtime"]
    fn p4_platform_h264_encoder_validates_and_decodes_a_real_clip() {
        let ffmpeg = std::env::var("PET_MANAGER_FFMPEG_SMOKE_BIN")
            .expect("PET_MANAGER_FFMPEG_SMOKE_BIN is required");
        let input = PathBuf::from(
            std::env::var("PET_MANAGER_FFMPEG_SMOKE_INPUT")
                .expect("PET_MANAGER_FFMPEG_SMOKE_INPUT is required"),
        );
        let temp = tempfile::tempdir().expect("create smoke output directory");
        let output = temp.path().join("platform.h264");
        let exported = export_p4_h264_stream(&ffmpeg, &input, &output)
            .expect("platform H.264 output must survive P4 structural and decode validation");
        assert!(exported.frames > 0);
        assert!(exported.stream_bytes > 0);
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
        assert_eq!(prepared.file_count, 17);
        assert!(prepared_p4_requires_system_cues(&appearance_dir).unwrap());
        assert!(load_prepared_p4_appearance_pack(&appearance_dir).unwrap().iter().all(|a| a.kind != "p4-audio"));
        assert!(prepared.byte_count > 5_000_000);
    }

    #[test]
    fn p4_h264_accepts_native_baseline_sps_and_rejects_unsupported_profiles() {
        let baseline = vec![
            0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02, 0x80, 0xbf, 0xe5, 0x84, 0, 0, 3, 0, 4,
            0, 0, 3, 0, 0x7a, 0x3c, 0x58, 0xba, 0x80, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80, 0, 0, 0,
            1, 0x09, 0xf0, 0, 0, 0, 1, 0x65, 0x88, 0x84,
        ];
        let parsed = parse_p4_h264_stream(&baseline, 640, 480).unwrap();
        assert_eq!(parsed.frames, 1);
        let mut high_profile = baseline.clone();
        high_profile[5] = 100;
        assert!(parse_p4_h264_stream(&high_profile, 640, 480)
            .unwrap_err()
            .contains("Baseline profile"));
        let mut multi_slice = baseline;
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
        #[cfg(windows)]
        assert_eq!(usb_uart_wire_bytes_per_sec(P4_USB_UART_BAUD), 200_000);
        #[cfg(not(windows))]
        assert_eq!(usb_uart_wire_bytes_per_sec(P4_USB_UART_BAUD), 400_000);
        assert!(p4_appearance_asset_chunk_wire_time(encoded_len) < Duration::from_millis(150));
        #[cfg(windows)]
        {
            assert_eq!(P4_RAW_APPEARANCE_SAFE_CHUNK_SIZE, 2_048);
            assert_eq!(P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE, 65_536);
        }
        #[cfg(not(windows))]
        {
            assert_eq!(P4_RAW_APPEARANCE_SAFE_CHUNK_SIZE, 8_192);
            assert_eq!(P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE, 8_192);
        }
        assert!(
            Duration::from_millis(
                (P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE as u64 * 1000)
                    .div_ceil(usb_uart_wire_bytes_per_sec(P4_USB_UART_BAUD))
            ) < Duration::from_millis(350)
        );
    }

    #[test]
    fn p4_raw_chunk_policy_uses_platform_limit_and_safe_integrity_fallback() {
        assert_eq!(
            p4_raw_asset_chunk_size_for_limit(Some(4 * 1024)),
            P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE
                .min(4 * 1024)
                .max(P4_RAW_APPEARANCE_SAFE_CHUNK_SIZE)
        );
        assert_eq!(
            p4_raw_asset_chunk_size_for_limit(Some(8 * 1024)),
            P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE.min(8 * 1024)
        );
        assert_eq!(
            p4_raw_asset_chunk_size_for_limit(Some(64 * 1024)),
            P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE
        );
        assert!(should_downgrade_p4_raw_chunk_size(
            "raw chunk checksum mismatch expected=aa actual=bb"
        ));
        assert!(should_downgrade_p4_raw_chunk_size(
            "timed out waiting for raw P4 asset chunk: transferId=test"
        ));
        assert!(!should_downgrade_p4_raw_chunk_size(
            "raw appearance partition write failed"
        ));
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
        std::fs::write(app_data_dir.join("custom.wav"), p4_test_wav()).unwrap();

        let families = vec![
            serde_json::json!({"family": "idle.default", "ok": true, "audioPath":"custom.wav"}),
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
            connection_id: String::new(),boot_id:String::new(),
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
            connection_id: String::new(),boot_id:String::new(),
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
            capture_active: Arc::new(AtomicBool::new(false)),
        });
manager.activate_memory_test_connection();

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
    fn high_speed_p4_control_frames_use_the_platform_serial_slice_size() {
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
            wire_protocol: "pet-usb-jsonl-v3".to_string(),
            capabilities: serde_json::Value::Null,
            connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)),
            capture_active: Arc::new(AtomicBool::new(false)),
        });
manager.activate_memory_test_connection();
        let bindings = (0..16)
            .map(|index| {
                serde_json::json!({
                    "event": format!("button.test.{index}"),
                    "action": "disabled",
                    "value": "",
                })
            })
            .collect::<Vec<_>>();

        manager
            .send_to_board(
                "p4-board-a",
                "input/config",
                &serde_json::json!({ "requestId": "config-1", "bindings": bindings }),
            )
            .unwrap();

        let recorded = recorded.lock().unwrap();
        assert!(!recorded.writes.is_empty());
        #[cfg(any(target_os = "macos", windows))]
        assert!(recorded
            .writes
            .iter()
            .all(|write| write.len() <= P4_CH343_SERIAL_WRITE_SLICE_BYTES));
        #[cfg(target_os = "macos")]
        assert_eq!(recorded.flushes, 0);
        #[cfg(windows)]
        {
            assert!(recorded.writes.len() > 1);
            assert_eq!(recorded.flushes, 1);
        }
        #[cfg(all(not(target_os = "macos"), not(windows)))]
        {
            assert_eq!(recorded.writes.len(), 1);
            assert_eq!(recorded.flushes, 1);
        }
        let line = recorded.writes.concat();
        assert!(line.ends_with(b"\n"));
        let message: serde_json::Value = serde_json::from_slice(&line[..line.len() - 1]).unwrap();
        assert_eq!(message["topic"], "input/config");
        assert_eq!(message["payload"]["bindings"].as_array().unwrap().len(), 16);
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
    fn firmware_commit_ack_wait_is_bounded_without_extending_validation_timeout() {
        assert_eq!(P4_FIRMWARE_COMMIT_ACK_TIMEOUT, Duration::from_secs(3));
        assert!(P4_FIRMWARE_COMMIT_ACK_TIMEOUT < P4_FIRMWARE_RECONNECT_TIMEOUT);
    }

    #[test]
    fn firmware_image_limit_matches_the_current_ota_partition() {
        assert_eq!(P4_FIRMWARE_MAX_IMAGE_SIZE, 0x300000);
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
    fn schema_five_starts_safely_then_recovers_to_fast_chunks() {
        let capabilities = serde_json::json!({
            "firmwareUpdate": { "chunkBytes": 4096 }
        });
        assert_eq!(
            preferred_firmware_chunk_size(5, &capabilities),
            P4_FIRMWARE_FAST_CHUNK_SIZE
        );
        assert_eq!(
            initial_firmware_chunk_size(5, P4_FIRMWARE_FAST_CHUNK_SIZE),
            P4_FIRMWARE_SAFE_CHUNK_SIZE
        );
        assert_eq!(
            preferred_firmware_chunk_size(6, &capabilities),
            P4_FIRMWARE_FAST_CHUNK_SIZE
        );
        assert_eq!(
            initial_firmware_chunk_size(6, P4_FIRMWARE_FAST_CHUNK_SIZE),
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
    fn firmware_chunk_only_retries_explicit_not_applied_base64_rejection() {
        assert!(!firmware_chunk_error_is_retryable(
            &FirmwareCommandError::Timeout {
                phase: "chunk".to_string(),
            }
        ));
        assert!(firmware_chunk_error_is_retryable(
            &FirmwareCommandError::Rejected("firmware chunk base64 mismatch".to_string())
        ));
        assert!(!firmware_chunk_error_is_retryable(
            &FirmwareCommandError::Rejected("firmware transferId mismatch".to_string())
        ));
        assert!(!firmware_chunk_error_is_retryable(
            &FirmwareCommandError::Rejected("ESP_ERR_FLASH_OP_FAIL".to_string())
        ));
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
    fn firmware_commands_use_the_platform_serial_slice_size() {
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
            capture_active: Arc::new(AtomicBool::new(false)),
        });
manager.activate_memory_test_connection();
        let payload = serde_json::json!({
            "transferId": "firmware-1",
            "seq": 0,
            "decodedSize": P4_FIRMWARE_CHUNK_SIZE,
            "data": "A".repeat(P4_FIRMWARE_CHUNK_SIZE.div_ceil(3) * 4),
        });

        manager
            .send_firmware_to_board("p4-board-a", 1, "firmware/chunk", &payload)
            .unwrap();

        let recorded = recorded.lock().unwrap();
        assert!(!recorded.writes.is_empty());
        #[cfg(any(target_os = "macos", windows))]
        assert!(recorded
            .writes
            .iter()
            .all(|write| write.len() <= P4_CH343_SERIAL_WRITE_SLICE_BYTES));
        #[cfg(target_os = "macos")]
        assert_eq!(recorded.flushes, 0);
        #[cfg(windows)]
        {
            assert!(recorded.writes.len() > 1);
            assert_eq!(recorded.flushes, 1);
        }
        #[cfg(all(not(target_os = "macos"), not(windows)))]
        {
            assert_eq!(recorded.writes.len(), 1);
            assert_eq!(recorded.flushes, 1);
        }
        let line = recorded.writes.concat();
        assert!(line.ends_with(b"\n"));
        let message: serde_json::Value = serde_json::from_slice(&line[..line.len() - 1]).unwrap();
        assert_eq!(message["topic"], "firmware/chunk");
        assert_eq!(message["payload"]["decodedSize"], P4_FIRMWARE_CHUNK_SIZE);
    }

    #[test]
    fn firmware_command_refuses_to_cross_a_usb_reconnect() {
        let manager = UsbSerialManager::new();
        let written = Arc::new(Mutex::new(Vec::new()));
        *manager.connection.lock().unwrap() = Some(UsbConnection {
            connection_id: 2,
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
            protocol_schema: 5,
            wire_protocol: "pet-usb-jsonl-v3".to_string(),
            capabilities: serde_json::Value::Null,
            connected: true,
            cancel_reader: Arc::new(AtomicBool::new(false)),
            capture_active: Arc::new(AtomicBool::new(false)),
        });
manager.activate_memory_test_connection();

        let error = manager
            .send_firmware_to_board(
                "p4-board-a",
                1,
                "firmware/chunk",
                &serde_json::json!({ "transferId": "old-transfer" }),
            )
            .unwrap_err();

        assert!(error.contains("USB connection changed"));
        assert!(written.lock().unwrap().is_empty());
    }

    // A per-instance scripted wire resolves the production waiters. Any extra
    // abort/replay/restart is an unexpected real frame, not a source-text pin.
    fn firmware_once_test_manager(script: Vec<(&'static str, Option<serde_json::Value>)>)
        -> (UsbSerialManager, Arc<Mutex<Vec<serde_json::Value>>>) {
        struct Writer {
            pending: Vec<u8>,
            script: std::collections::VecDeque<(&'static str, Option<serde_json::Value>)>,
            trace: Arc<Mutex<Vec<serde_json::Value>>>,
            acks: Arc<Mutex<Vec<FirmwareAckWaiter>>>,
            statuses: Arc<Mutex<Vec<FirmwareStatusWaiter>>>,
            replies: Arc<Mutex<Vec<DeviceResponseWaiter>>>,
            received: u64,
        }
        impl Write for Writer {
            fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
                self.pending.extend_from_slice(bytes);
                assert!(self.pending.len() < 16384);
                while let Some(at) = self.pending.iter().position(|b| *b == b'\n') {
                    let line = self.pending.drain(..=at).collect::<Vec<_>>();
                    let request: serde_json::Value = serde_json::from_slice(&line).unwrap();
                    self.trace.lock().unwrap().push(request.clone());
                    let topic = request["topic"].as_str().unwrap();
                    let (expected, reply) = self.script.pop_front().expect("unexpected replay/abort/restart frame");
                    assert_eq!(topic, expected);
                    if let Some(mut reply) = reply {
                        let p = &request["payload"];
                        if topic == "firmware/query" {
                            reply["requestId"] = p["requestId"].clone();
                            resolve_firmware_status(&self.statuses, "firmware/status", &reply);
                        } else if topic == "diagnostics/query" {
                            reply["requestId"] = p["requestId"].clone();
                            reply["boardDeviceId"] = "p4-board-a".into();
                            resolve_device_response(&self.replies, "diagnostics/status", &reply);
                        } else {
                            let phase = topic.strip_prefix("firmware/").expect("firmware-only script");
                            reply["transferId"] = p["transferId"].clone();
                            reply["phase"] = phase.into();
                            let positive = reply["ok"].as_bool() == Some(true);
                            if phase == "chunk" {
                                if positive { self.received += p["decodedSize"].as_u64().unwrap(); }
                                if reply.get("seq").is_none() { reply["seq"] = p["seq"].clone(); }
                                if reply.get("chunkBytes").is_none() { reply["chunkBytes"] = if positive {p["decodedSize"].clone()} else {0.into()}; }
                                if reply.get("duplicate").is_none() {reply["duplicate"]=false.into();}
                            }
                            if reply.get("nextSequence").is_none() {
                                reply["nextSequence"] = if phase == "begin" || (phase == "chunk" && !positive) {0.into()} else {1.into()};
                            }
                            if reply.get("receivedBytes").is_none() {reply["receivedBytes"] = self.received.into();}
                            resolve_firmware_ack(&self.acks, "firmware/ack", &reply);
                        }
                    }
                }
                Ok(bytes.len())
            }
            fn flush(&mut self) -> std::io::Result<()> { Ok(()) }
        }
        let manager = UsbSerialManager::new();
        let trace = Arc::new(Mutex::new(Vec::new()));
        *manager.connection.lock().unwrap() = Some(UsbConnection {
            connection_id: 1, port_name: "memory-firmware-once".into(), baud_rate: P4_USB_UART_BAUD,
            writer: Box::new(Writer { pending: Vec::new(), script: script.into(), trace: trace.clone(),
                acks: manager.firmware_ack_waiters.clone(), statuses: manager.firmware_status_waiters.clone(),
                replies: manager.device_response_waiters.clone(), received: 0 }),
            board_device_id: "p4-board-a".into(), runtime: "esp-p4".into(), device_model: "test-only".into(),
            firmware: "0.7.103-p4-old".into(), build_id: String::new(), git_sha: String::new(), build_dirty: false,
            protocol_schema: 5, wire_protocol: "pet-usb-jsonl-v3".into(),
            capabilities: serde_json::json!({"firmwareUpdate":{"chipRevision":103}}),
            connected: true, cancel_reader: Arc::new(AtomicBool::new(false)), capture_active: Arc::new(AtomicBool::new(false)),
        });
        manager.activate_memory_test_connection();
        (manager, trace)
    }

    #[test]
    fn firmware_preflight_keeps_an_interrupted_or_unknown_transfer_read_only() {
        for (status, allowed) in [(serde_json::json!({"active":true,"transferId":"interrupted"}),false),
            (serde_json::json!({}),false), (serde_json::json!({"active":false}),true)] {
            let (manager, trace) = firmware_once_test_manager(vec![("firmware/query", Some(status))]);
            assert_eq!(manager.ensure_firmware_transfer_idle("p4-board-a",1).is_ok(), allowed);
            let trace = trace.lock().unwrap();
            assert_eq!(trace.len(),1);
            assert_eq!(trace[0]["topic"],"firmware/query");
            assert!(manager.firmware_status_waiters.lock().unwrap().is_empty());
            assert!(manager.firmware_ack_waiters.lock().unwrap().is_empty());
        }
    }

    #[test]
    fn rejected_firmware_begin_is_sent_once_without_abort_or_stale_recovery() {
        for error in ["begin rejected", "another firmware transfer is active"] {
            let (manager, trace) = firmware_once_test_manager(vec![("firmware/begin",
                Some(serde_json::json!({"ok":false,"error":error})))]);
            let result = manager.begin_firmware_transfer("p4-board-a",1,
                &serde_json::json!({"transferId":"firmware-test","size":4096,"sha256":"00".repeat(32)}),"firmware-test");
            assert!(result.unwrap_err().contains(error));
            let trace = trace.lock().unwrap();assert_eq!(trace.len(),1);assert_eq!(trace[0]["topic"],"firmware/begin");
            assert!(manager.firmware_ack_waiters.lock().unwrap().is_empty());
        }
    }

    #[test]
    fn malformed_firmware_ack_is_unknown_even_with_retryable_error_text() {
        for (topic,phase,next,received) in [("firmware/begin","begin",0,0),
            ("firmware/chunk","chunk",1,16), ("firmware/commit","commit",1,0)] {
            for ok in [None,Some(serde_json::Value::Null),Some(serde_json::json!("false")),
                Some(serde_json::json!("true")),Some(serde_json::json!(0))] {
                let mut ack=serde_json::json!({"error":"firmware chunk base64 mismatch"});
                if let Some(ok)=ok {ack["ok"]=ok;}
                let (manager,trace)=firmware_once_test_manager(vec![(topic,Some(ack))]);
                let error=manager.send_firmware_command_and_wait("p4-board-a",1,topic,
                    &serde_json::json!({"transferId":"malformed-test","seq":0,"decodedSize":16}),
                    "malformed-test",phase,next,received,Duration::from_millis(25)).unwrap_err();
                assert!(matches!(&error,FirmwareCommandError::Send(message) if message.contains("result unknown")),"{phase}: {error}");
                assert!(!firmware_chunk_error_is_retryable(&error));
                assert!(firmware_corruption_fallback_size(P4_FIRMWARE_FAST_CHUNK_SIZE,&error).is_none());
                assert_eq!(trace.lock().unwrap().len(),1);
                assert!(manager.firmware_ack_waiters.lock().unwrap().is_empty());
            }
            for ok in [false,true] {
                let (manager,trace)=firmware_once_test_manager(vec![(topic,
                    Some(serde_json::json!({"ok":ok,"error":if phase=="chunk" {"firmware chunk base64 mismatch"} else {"explicit rejection"}})))]);
                let result=manager.send_firmware_command_and_wait("p4-board-a",1,topic,
                    &serde_json::json!({"transferId":"explicit-test","seq":0,"decodedSize":16}),
                    "explicit-test",phase,next,received,Duration::from_millis(25));
                if ok {assert!(result.is_ok());}
                else {assert!(matches!(result,Err(FirmwareCommandError::Rejected(_))));}
                assert_eq!(trace.lock().unwrap().len(),1);
            }
        }
    }

    #[test]
    fn missing_firmware_begin_ack_is_unknown_without_replay_or_abort() {
        let (manager, trace) = firmware_once_test_manager(vec![("firmware/begin", None)]);
        let timeout = Duration::from_millis(25);let started = Instant::now();
        let result = manager.begin_firmware_transfer_once("p4-board-a",1,
            &serde_json::json!({"transferId":"firmware-test","size":4096,"sha256":"00".repeat(32)}),"firmware-test",timeout);
        assert!(started.elapsed() >= timeout);
        assert!(result.unwrap_err().contains("timed out"));
        let trace = trace.lock().unwrap();assert_eq!(trace.len(),1);assert_eq!(trace[0]["topic"],"firmware/begin");
        assert!(manager.firmware_ack_waiters.lock().unwrap().is_empty());
        assert!(manager.inbound_owner.is_current(1));
        assert!(manager.connection.lock().unwrap().as_ref().unwrap().connected,
            "a still-live transport must not cause mutation replay");
    }

    #[test]
    fn real_firmware_update_never_restarts_aborts_or_replays_unknown_steps() {
        for phase in ["begin-metadata", "begin-rejected", "chunk-mismatch", "chunk-timeout",
            "chunk-ok-missing", "chunk-ok-null", "chunk-ok-string-false", "chunk-ok-string-true",
            "chunk-ok-number", "chunk-progress-next", "chunk-progress-bytes", "chunk-progress-duplicate",
            "chunk-progress-size", "chunk-progress-null", "chunk-base64-known", "commit-rejected", "commit-timeout"] {
            let status = serde_json::json!({"supported":true,"active":false,
                "runningPartition":{"label":"ota_0"},"nextPartition":{"label":"ota_1","size":2621440}});
            let mut script = vec![("firmware/query",Some(status.clone())),("firmware/query",Some(status)),
                ("diagnostics/query",Some(serde_json::json!({"bootCount":1,"runtime":{"firmware":"0.7.103-p4-old","runningPartition":"ota_0"}})))];
            script.push(("firmware/begin",Some(match phase {
                "begin-metadata" => serde_json::json!({"ok":true}),
                "begin-rejected" => serde_json::json!({"ok":false,"error":"another firmware transfer is active"}),
                _ => serde_json::json!({"ok":true,"targetPartition":"ota_1"}),
            })));
            if !phase.starts_with("begin-") {
                script.push(("firmware/chunk",match phase {
                    "chunk-mismatch" => Some(serde_json::json!({"ok":false,"error":"firmware transferId mismatch"})),
                    "chunk-timeout" => None,
                    // Correlated frames deliberately claim the only retryable
                    // error while omitting/mistyping its actual result. The real
                    // waiter must deliver them; the production classifier stops.
                    "chunk-ok-missing" => Some(serde_json::json!({"error":"firmware chunk base64 mismatch"})),
                    "chunk-ok-null" => Some(serde_json::json!({"ok":null,"error":"firmware chunk base64 mismatch"})),
                    "chunk-ok-string-false" => Some(serde_json::json!({"ok":"false","error":"firmware chunk base64 mismatch"})),
                    "chunk-ok-string-true" => Some(serde_json::json!({"ok":"true","error":"firmware chunk base64 mismatch"})),
                    "chunk-ok-number" => Some(serde_json::json!({"ok":0,"error":"firmware chunk base64 mismatch"})),
                    "chunk-base64-known" => Some(serde_json::json!({"ok":false,"error":"firmware chunk base64 mismatch"})),
                    "chunk-progress-next" => Some(serde_json::json!({"ok":false,"error":"firmware chunk base64 mismatch","nextSequence":1})),
                    "chunk-progress-bytes" => Some(serde_json::json!({"ok":false,"error":"firmware chunk base64 mismatch","receivedBytes":288})),
                    "chunk-progress-duplicate" => Some(serde_json::json!({"ok":false,"error":"firmware chunk base64 mismatch","duplicate":true})),
                    "chunk-progress-size" => Some(serde_json::json!({"ok":false,"error":"firmware chunk base64 mismatch","chunkBytes":1})),
                    "chunk-progress-null" => Some(serde_json::json!({"ok":false,"error":"firmware chunk base64 mismatch","receivedBytes":null})),
                    _ => Some(serde_json::json!({"ok":true})),
                }));
            }
            if phase=="chunk-base64-known" {script.push(("firmware/chunk",Some(serde_json::json!({"ok":true}))));}
            if phase.starts_with("commit-") || phase=="chunk-base64-known" {
                script.push(("firmware/commit",if phase=="commit-timeout" {None}
                    else {Some(serde_json::json!({"ok":false,"error":"commit rejected"}))}));
            }
            let expected = script.iter().map(|(topic,_)|*topic).collect::<Vec<_>>();
            let (manager, trace) = firmware_once_test_manager(script);
            let dir = tempfile::tempdir().unwrap();let image_path = dir.path().join("firmware.bin");
            let mut image = app_image_fixture(P4_FIRMWARE_PROJECT_NAME,"0.7.103-p4-test");
            image[12..14].copy_from_slice(&18u16.to_le_bytes());
            image[15..17].copy_from_slice(&103u16.to_le_bytes());
            image[17..19].copy_from_slice(&103u16.to_le_bytes());
            std::fs::write(&image_path,image).unwrap();
            let started = Instant::now();
            let error = manager.update_firmware(&image_path,"p4-board-a",|_,_,_|{},
                ||panic!("unknown/rejected transaction must not reconnect or validate")).unwrap_err();
            let reason = match phase {"begin-metadata"=>"targetPartition", "begin-rejected"=>"another firmware transfer is active",
                "chunk-mismatch"=>"firmware transferId mismatch", "commit-rejected"|"chunk-base64-known"=>"commit rejected",
                p if p.starts_with("chunk-ok-") || p.starts_with("chunk-progress-") => "result unknown", _=>"timed out"};
            assert!(error.contains(reason),"{phase}: {error}");
            if phase=="chunk-timeout" {assert!(started.elapsed()>=P4_FIRMWARE_CHUNK_ACK_TIMEOUT);}
            if phase=="commit-timeout" {assert!(started.elapsed()>=P4_FIRMWARE_COMMIT_ACK_TIMEOUT);}
            let trace=trace.lock().unwrap();
            assert_eq!(trace.iter().map(|r|r["topic"].as_str().unwrap()).collect::<Vec<_>>(),expected,"{phase}");
            assert!(manager.firmware_ack_waiters.lock().unwrap().is_empty());
            assert!(manager.firmware_status_waiters.lock().unwrap().is_empty());
            assert!(manager.device_response_waiters.lock().unwrap().is_empty());
            assert!(manager.inbound_owner.is_current(1));
        }
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
    fn hello_ack_breaks_echo_loops_but_allows_lost_ack_retry() {
        let now = Instant::now();
        assert!(!hello_ack_due(None, now, ""));
        assert!(!hello_ack_due(None, now, "  "));
        assert!(hello_ack_due(None, now, "desktop"));
        for ms in [0, 1, 100, 1999] {
            assert!(!hello_ack_due(Some(now), now + Duration::from_millis(ms), "desktop"));
        }
        assert!(hello_ack_due(Some(now), now + Duration::from_secs(2), "desktop"));
        assert!(hello_ack_due(Some(now), now + Duration::from_secs(3), "desktop"));
    }

    #[test]
    #[ignore = "Requires explicit permission and exclusive USB: updates only the selected existing media package"]
    fn p4_media_surface_hardware_sync_and_reenter() {
        let port = std::env::var("P4_SERIAL_PORT").expect("explicit port");
        let expected_board = std::env::var("P4_EXPECTED_BOARD_ID").expect("explicit board");
        let expected_version = std::env::var("P4_EXPECTED_VERSION").expect("explicit firmware");
        let package = PathBuf::from(std::env::var("P4_WIDGET_DIR").expect("explicit package"));
        let entry = crate::component_library::inspect(&package).expect("validate package");
        assert!(entry.valid, "{:?}", entry.validation_errors);
        let expected_id = std::env::var("P4_EXPECTED_WIDGET_ID").expect("explicit existing component ID");
        assert_eq!(entry.id, expected_id);
        assert!(entry.pc_extension.is_some(), "a declarative media package is required");
        assert_eq!(entry.version, std::env::var("P4_EXPECTED_WIDGET_VERSION").expect("explicit component version"));
        // Pass the current component-specific controls explicitly; never infer
        // that an empty override map is safe for another user's installation.
        let bindings: HashMap<String, String> = serde_json::from_str(
            &std::env::var("P4_WIDGET_BINDINGS_JSON").expect("explicit preserved bindings")
        ).unwrap();
        let usb = UsbSerialManager::new();
        usb.set_desktop_device_id("radio-surface-hil");
        usb.connect(&port, |_, _| {}).expect("exclusive USB");
        struct Close(UsbSerialManager);
        impl Drop for Close { fn drop(&mut self) { self.0.disconnect(); } }
        let _close = Close(usb.clone());
        assert_eq!(usb.status().board_device_id, expected_board);
        let command = |topic: &str, mut payload: serde_json::Value| {
            let request_id = format!("radio-hil-{}", uuid::Uuid::new_v4());
            payload["requestId"] = serde_json::json!(request_id);
            let (sender, receiver) = mpsc::channel();
            usb.device_response_waiters.lock().unwrap().push(DeviceResponseWaiter {
                request_id: request_id.clone(), response_topic: "protocol/ack".into(), sender,
            });
            let sent = usb.send_to_board(&expected_board, topic, &payload);
            let reply = receiver.recv_timeout(Duration::from_secs(3));
            usb.remove_device_response_waiter(&request_id, "protocol/ack");
            sent.expect("send display command");
            let reply = reply.expect("correlated display ACK");
            assert_eq!(reply["ok"], true, "{reply}");
        };
        let before = usb.query_diagnostics(&expected_board).unwrap();
        assert_eq!(before["runtime"]["firmware"].as_str(), Some(expected_version.as_str()));
        assert_eq!(before["runtime"]["imageState"], "valid");
        let media = usb.media_request(&expected_board, "media/query", serde_json::json!({})).unwrap();
        assert!(!matches!(media["state"].as_str(), Some("playing" | "buffering" | "paused")), "existing playback must remain untouched");
        let config = usb.query_button_config(&expected_board).unwrap();
        let inventory = usb.query_widget_inventory(&expected_board).unwrap();
        let ids = |v: &serde_json::Value| -> HashSet<String> {
            assert_eq!(v["ok"], true); assert_eq!(v["complete"], true);
            v["items"].as_array().unwrap().iter().map(|i| i["id"].as_str().unwrap().to_owned()).collect()
        };
        let prior_ids = ids(&inventory);
        assert!(prior_ids.contains(&expected_id), "only update the authorized existing component");
        let result = usb.install_widget_clawpkg(&expected_id, &package, &bindings, |file, total, bytes| {
            eprintln!("radio sync {file}/{total}, transferred={bytes}");
        }).expect("atomic component sync");
        let after_inventory = usb.query_widget_inventory(&expected_board).unwrap();
        assert_eq!(ids(&after_inventory), prior_ids);
        assert_eq!(after_inventory["activeWidgetId"], expected_id);
        for cycle in 0..10 {
            for page in ["main", "app"] {
                command("control/screen-page", serde_json::json!({"page":page}));
                thread::sleep(Duration::from_millis(400));
                let d = usb.query_diagnostics(&expected_board).unwrap();
                assert_eq!(d["runtime"]["screenPage"], page);
                assert_eq!(d["bootCount"], before["bootCount"]);
                assert_eq!(d["faultResetCount"], before["faultResetCount"]);
            }
            for _ in 0..2 {
                command("miniapp/event", serde_json::json!({"action":"media.queue"}));
                thread::sleep(Duration::from_millis(400));
                usb.media_request(&expected_board, "media/query", serde_json::json!({})).unwrap();
            }
            eprintln!("radio render cycle {} passed", cycle + 1);
        }
        let final_state = usb.query_diagnostics(&expected_board).unwrap();
        assert_eq!(final_state["bootCount"], before["bootCount"]);
        assert_eq!(final_state["faultResetCount"], before["faultResetCount"]);
        assert_eq!(final_state["storage"]["activeAppearanceSlot"], before["storage"]["activeAppearanceSlot"]);
        let final_config = usb.query_button_config(&expected_board).unwrap();
        assert_eq!(final_config["config"], config["config"]);
        eprintln!("RADIO SURFACE PASS: files={}, catalogPreserved=true, globalButtonsPreserved=true, cycles=10, firmware={}, boot={}, faults={}, page={}", result.0, final_state["runtime"]["firmware"], final_state["bootCount"], final_state["faultResetCount"], final_state["runtime"]["screenPage"]);
    }

    #[test]
    #[ignore = "Explicit approval: install three classic game surfaces; no firmware/audio/microphone"]
    fn p4_classic_games_surface_hardware() {
        use serde_json::{json,Value};
        use base64::Engine;
        assert_eq!(std::env::var("PET_CLASSIC_GAMES_LIVE_TEST").as_deref(),Ok("1"));
        let root=PathBuf::from(std::env::var("P4_APP_PACKAGES").unwrap());
        let output=PathBuf::from(std::env::var("PET_CLASSIC_GAMES_REPORT_DIR").unwrap());
        std::fs::create_dir_all(&output).unwrap();
        let selected=std::env::var("P4_CLASSIC_GAME_IDS").unwrap_or("snake-turn,block-combo,flappy-bird".into());
        let ids:Vec<&str>=selected.split(',').collect();
        assert!(ids.iter().all(|id|matches!(*id,"snake-turn"|"block-combo"|"flappy-bird"|"bloomfrog_companion"|"two-key-pong"|"upcoming-todos"|"computer-status")));
        if ids.iter().any(|id| matches!(*id,"bloomfrog_companion"|"two-key-pong"|"upcoming-todos"|"computer-status")) {
            assert_eq!(std::env::var("PET_COMPANION_SURFACES_LIVE_TEST").as_deref(),Ok("1"));
        }
        for id in &ids {let e=crate::component_library::inspect(&root.join(id)).unwrap();assert!(e.valid,"{id}: {:?}",e.validation_errors);}
        let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|topic,p|{
            if topic=="miniapp/state"||topic.starts_with("debug/"){let _=tx.send((topic,p));}
        }).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}let _close=Close(usb.clone());
        let board=usb.status().board_device_id;
        assert_eq!(board,std::env::var("P4_EXPECTED_BOARD_ID").unwrap());
        assert_eq!(usb.status().capabilities["widgetGameSurface"],"p4-game-surface-v1");
        if std::env::var("PET_COMPANION_SURFACES_LIVE_TEST").as_deref()==Ok("1") {
            assert_eq!(usb.status().capabilities["widgetSceneSurface"],"p4-scene-surface-v1");
            assert_eq!(usb.status().capabilities["widgetDataSurface"],"p4-data-surface-v1");
        }
        let before=usb.query_diagnostics(&board).unwrap();let keys=usb.query_button_config(&board).unwrap();
        assert_eq!(before["runtime"]["firmware"],std::env::var("P4_EXPECTED_FW").unwrap_or("0.7.88-p4".into()));
        let inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["complete"],true);
        let media=usb.media_request(&board,"media/query",json!({})).unwrap();
        assert!(!matches!(media["state"].as_str(),Some("playing"|"buffering"|"paused")));
        std::fs::write(output.join("before.json"),serde_json::to_vec_pretty(&json!({"diagnostics":before,"inventory":inventory,"buttons":keys})).unwrap()).unwrap();
        let shown=|id:&str|{
            while rx.try_recv().is_ok(){}
            usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();
            let until=Instant::now()+Duration::from_secs(5);
            while Instant::now()<until{if let Ok((topic,v))=rx.recv_timeout(Duration::from_millis(100)){
                if topic=="miniapp/state"&&v["widgetId"]==id{return v;}
            }}panic!("No state for {id}");
        };
        let capture=|id:&str|{
            while rx.try_recv().is_ok(){}
            usb.send_to_board(&board,"debug/screenshot",&json!({"fullResolution":true})).unwrap();
            let until=Instant::now()+Duration::from_secs(30);
            let mut identity=Value::Null;let mut data=Vec::new();let mut index=0;
            while Instant::now()<until{
                let Ok((topic,p))=rx.recv_timeout(Duration::from_millis(100))else{continue;};
                assert_ne!(topic,"debug/screenshot_error","{p}");
                if topic=="debug/screenshot_begin"{assert_eq!(p["width"],640);assert_eq!(p["height"],480);assert_eq!(p["format"],"rgb565le");identity=p["id"].clone();}
                else if topic=="debug/screenshot_chunk"&&p["id"]==identity{
                    assert_eq!(p["index"],index);index+=1;
                    data.extend(base64::engine::general_purpose::STANDARD.decode(p["data"].as_str().unwrap()).unwrap());
                }else if topic=="debug/screenshot_end"&&p["id"]==identity{
                    assert_eq!(data.len(),640*480*2);assert_eq!(p["chunks"],index);
                    let mut hash=2166136261u32;let mut image=image::RgbImage::new(640,480);
                    for (at,raw) in data.chunks_exact(2).enumerate(){
                        let px=u16::from_le_bytes([raw[0],raw[1]]);hash=(hash^px as u32).wrapping_mul(16777619);
                        let (r,g,b)=((px>>11)as u8,((px>>5)&63)as u8,(px&31)as u8);
                        image.put_pixel(at as u32%640,at as u32/640,image::Rgb([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)]));
                    }
                    assert_eq!(p["checksum"],format!("{hash:08x}"));image.save(output.join(format!("{id}.png"))).unwrap();return;
                }
            }panic!("Screenshot timeout");
        };
        for (id,start,moves) in [
            ("snake-turn","snake.start",vec!["snake.turn_left","snake.turn_right"]),
            ("block-combo","blocks.start",vec!["blocks.left","blocks.right","blocks.rotate","blocks.drop"]),
            ("flappy-bird","flappy.flap",vec!["flappy.flap"]),
            ("bloomfrog_companion","pet_pet",vec!["move_left","move_right","feed_pet"]),
            ("two-key-pong","start",vec!["shift_left","shift_right"]),
            ("upcoming-todos","first_page",vec!["next_page","previous_page"]),
            ("computer-status","first_page",vec!["next_page","previous_page"]),
        ] {
            if !ids.contains(&id){continue;}
            usb.install_widget_clawpkg(id,&root.join(id),&HashMap::new(),|f,n,b|eprintln!("{id}: file {f}/{n}, {b} bytes")).unwrap();
            usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();
            thread::sleep(Duration::from_millis(800));capture(id);
            let event=|action|usb.send_to_board(&board,"miniapp/event",&json!({"action":action})).unwrap();
            let classic=matches!(id,"snake-turn"|"block-combo"|"flappy-bird"|"two-key-pong");
            if classic {assert_eq!(shown(id)["gameRunning"],false);}
            event(start);
            if classic {assert_eq!(shown(id)["gameRunning"],true);}
            if id=="bloomfrog_companion" {assert!(shown(id)["headline"].as_str().unwrap().contains("舒服"));}
            let first=shown(id);
            for action in moves{event(action);thread::sleep(Duration::from_millis(30));}
            thread::sleep(Duration::from_millis(400));let next=shown(id);
            if classic||id=="bloomfrog_companion" {
                assert!(next["gameRevision"].as_u64().unwrap()>first["gameRevision"].as_u64().unwrap());
            } else {
                // Exercise the actual live-data protocol using explicitly marked test
                // rows in RAM only; never create user todos or publish dummy covers.
                let source=if id=="upcoming-todos"{"todos.upcoming"}else{"computer.status"};
                let rows:Vec<_>=(0..6).map(|i|json!({"id":format!("qa-{i}"),"label":format!("验收测试 {}",i+1),"value":"10-20 12:30","detail":if i%2==0{"未完成"}else{"已完成"},"meta":"","tone":if i%2==0{1}else{-1}})).collect();
                usb.send_widget_data(&board,&json!({"schema":1,"source":source,"date":"2026-10-20","ttlMs":10000,"status":"ok","message":"验收用临时数据，不写入用户记录","rows":rows})).unwrap();
                event("first_page");thread::sleep(Duration::from_millis(200));capture(&format!("{id}-test-rows"));
                let page1=shown(id);event("next_page");let page2=shown(id);
                assert_ne!(page1,page2,"data paging must change device state");
                event("previous_page");
                // Restore the real provider snapshot, never leave test data visible.
                let real=if id=="upcoming-todos"{crate::todos::snapshot()}else{crate::local_widget_data::computer_status()};
                usb.send_widget_data(&board,&serde_json::to_value(real).unwrap()).unwrap();
            }
            let d=usb.query_diagnostics(&board).unwrap();assert_eq!(d["bootCount"],before["bootCount"]);assert_eq!(d["faultResetCount"],before["faultResetCount"]);
            usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();
            assert_eq!(usb.query_diagnostics(&board).unwrap()["runtime"]["screenPage"],"main");
            eprintln!("PASS {id}: native display screenshot, start/actions/tick, exit and no reset");
        }
        let after=usb.query_diagnostics(&board).unwrap();let next=usb.query_widget_inventory(&board).unwrap();assert_eq!(next["complete"],true);
        let installed=|v:&Value|v["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().to_string()).collect::<HashSet<_>>();
        let mut expected=installed(&inventory);for id in &ids{expected.insert((*id).into());}
        assert_eq!(installed(&next),expected);assert_eq!(keys["config"],usb.query_button_config(&board).unwrap()["config"]);
        assert_eq!(before["storage"]["activeAppearanceSlot"],after["storage"]["activeAppearanceSlot"]);
        std::fs::write(output.join("after.json"),serde_json::to_vec_pretty(&json!({"diagnostics":after,"inventory":next})).unwrap()).unwrap();
        eprintln!("PASS selected game surfaces {selected}; physical button/touch feel still requires manual verification.");
    }

    #[test]
    #[ignore = "Explicit approval: refresh three classic tools only, no firmware/audio/microphone"]
    fn p4_classic_tools_surface_hardware() {
        use serde_json::{json,Value};
        assert_eq!(std::env::var("PET_CLASSIC_TOOLS_LIVE_TEST").as_deref(),Ok("1"));
        let root=PathBuf::from(std::env::var("P4_APP_PACKAGES").unwrap());
        let ids=["drink-reminder","tomato-clock","token-usage"];
        for id in ids { let e=crate::component_library::inspect(&root.join(id)).unwrap();assert!(e.valid,"{id}: {:?}",e.validation_errors);assert_eq!(e.id,id); }
        let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|topic,payload|{if topic=="miniapp/state"{let _=tx.send(payload);}}).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}let _close=Close(usb.clone());
        let board=usb.status().board_device_id;assert!(!board.is_empty());
        assert_eq!(usb.status().capabilities["widgetAppGraphics"],"p4-app-graphics-v1");
        assert_eq!(usb.status().capabilities["widgetAppTypography"],"p4-app-type-v1");
        let before=usb.query_diagnostics(&board).unwrap();let keys=usb.query_button_config(&board).unwrap();
        let inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["complete"],true);
        let media=usb.media_request(&board,"media/query",json!({})).unwrap();
        assert!(!matches!(media["state"].as_str(),Some("playing"|"buffering"|"paused")),"Do not interrupt audio");
        let installed=|v:&Value|v["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().to_string()).collect::<HashSet<_>>();
        let mut expected=installed(&inventory);for id in ids {expected.insert(id.into());}
        assert!(expected.len()<=inventory["maxInstalled"].as_u64().unwrap() as usize);
        if let Ok(dir)=std::env::var("PET_CLASSIC_TOOLS_REPORT_DIR"){
            std::fs::create_dir_all(&dir).unwrap();
            std::fs::write(Path::new(&dir).join("before.json"),serde_json::to_vec_pretty(&json!({"diagnostics":before,"inventory":inventory,"buttons":keys})).unwrap()).unwrap();
        }
        let shown=|id:&str|{
            while rx.try_recv().is_ok(){}
            usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();
            let until=Instant::now()+Duration::from_secs(3);
            while Instant::now()<until {
                if let Ok(v)=rx.recv_timeout(Duration::from_millis(100)){if v["widgetId"]==id{return v;}}
            }panic!("No current state for {id}");
        };
        let event=|a|usb.send_to_board(&board,"miniapp/event",&json!({"action":a})).unwrap();
        for id in ids {
            usb.install_widget_clawpkg(id,&root.join(id),&HashMap::new(),|f,n,b|eprintln!("{id}: file {f}/{n}, {b} bytes")).unwrap();
            usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();
            thread::sleep(Duration::from_millis(700));
            assert!(!shown(id)["headline"].as_str().unwrap_or("").is_empty());
            match id {
                "drink-reminder"=>{
                    event("reminder.pause_resume");assert_eq!(shown(id)["metricUnit"],"继续提醒");
                    event("reminder.pause_resume");assert_eq!(shown(id)["metricUnit"],"暂停提醒");
                    // Don't fabricate a drink acknowledgement in the user's counters.
                    event("reminder.switch_view");assert_eq!(shown(id)["metricLabel"],"本次使用已记录");
                    event("reminder.switch_view");assert_eq!(shown(id)["metricLabel"],"距离下次提醒");
                },
                "tomato-clock"=>{
                    event("tomato.start_pause");assert_eq!(shown(id)["metricUnit"],"继续专注");
                    event("tomato.skip_phase");let v=shown(id);assert_eq!(v["badge"],"0");assert_eq!(v["metricUnit"],"暂停休息");
                    event("tomato.skip_phase");assert_eq!(shown(id)["metricUnit"],"暂停专注");
                },
                _=>{
                    event("stats.show_input");assert_eq!(shown(id)["metricLabel"],"输入 Token");
                    event("stats.show_output");assert_eq!(shown(id)["metricLabel"],"输出 Token");
                    event("stats.show_total");assert_eq!(shown(id)["metricLabel"],"总 Token");
                }
            }
            let d=usb.query_diagnostics(&board).unwrap();
            assert_eq!(d["bootCount"],before["bootCount"]);assert_eq!(d["faultResetCount"],before["faultResetCount"]);
            assert_eq!(d["runtime"]["screenPage"],"app");eprintln!("PASS {id}: installed/rendered/action state confirmed");
        }
        let after=usb.query_diagnostics(&board).unwrap();let next=usb.query_widget_inventory(&board).unwrap();assert_eq!(next["complete"],true);
        assert_eq!(installed(&next),expected);assert_eq!(keys["config"],usb.query_button_config(&board).unwrap()["config"]);
        assert_eq!(before["storage"]["activeAppearanceSlot"],after["storage"]["activeAppearanceSlot"]);
        if let Ok(dir)=std::env::var("PET_CLASSIC_TOOLS_REPORT_DIR"){
            std::fs::write(Path::new(&dir).join("after.json"),serde_json::to_vec_pretty(&json!({"diagnostics":after,"inventory":next})).unwrap()).unwrap();
        }
        eprintln!("PASS three classic tools; other inventory, appearance, global controls and boot count preserved. Physical touch and perceived image quality require user verification.");
    }

    #[test]
    #[ignore = "Explicit device approval: sync one authored Hachimi sample, preserve other components and global controls"]
    fn p4_app_graphics_hardware_sample() {
        let port=std::env::var("P4_SERIAL_PORT").expect("explicit port");
        let board=std::env::var("P4_EXPECTED_BOARD_ID").expect("explicit board");
        let dir=PathBuf::from(std::env::var("P4_WIDGET_DIR").expect("explicit package"));
        let entry=crate::component_library::inspect(&dir).unwrap();assert!(entry.valid,"{:?}",entry.validation_errors);
        assert!(matches!(entry.id.as_str(),"hachimi-home"|"hachimi-garden"));
        let usb=UsbSerialManager::new();usb.set_desktop_device_id("app-graphics-hil");usb.connect(&port,|_,_|{}).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}let _close=Close(usb.clone());
        assert_eq!(usb.status().board_device_id,board);assert_eq!(usb.status().capabilities["widgetAppGraphics"],"p4-app-graphics-v1");
        let before=usb.query_diagnostics(&board).unwrap();let config=usb.query_button_config(&board).unwrap();let inventory=usb.query_widget_inventory(&board).unwrap();
        assert_eq!(inventory["complete"],true);let ids:HashSet<String>=inventory["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().into()).collect();
        let media=usb.media_request(&board,"media/query",serde_json::json!({})).unwrap();assert!(!matches!(media["state"].as_str(),Some("playing"|"buffering"|"paused")));
        usb.install_widget_clawpkg(&entry.id,&dir,&HashMap::new(),|file,total,bytes|{eprintln!("app sync {file}/{total}: {bytes} bytes");}).unwrap();
        usb.send_to_board(&board,"control/screen-page",&serde_json::json!({"page":"app"})).unwrap();
        thread::sleep(Duration::from_secs(3));
        let after=usb.query_diagnostics(&board).unwrap();let next=usb.query_widget_inventory(&board).unwrap();
        assert_eq!(next["complete"],true);let next_ids:HashSet<String>=next["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().into()).collect();
        assert_eq!(next_ids,ids.union(&HashSet::from([entry.id.clone()])).cloned().collect());
        assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
        assert_eq!(before["storage"]["activeAppearanceSlot"],after["storage"]["activeAppearanceSlot"]);
        assert_eq!(config["config"],usb.query_button_config(&board).unwrap()["config"]);
        eprintln!("APP GRAPHICS SAMPLE PASS; original catalog/appearance/global controls preserved; no reset");
    }

    #[test]
    #[ignore = "Explicit approval: sync weather only, real forecast, isolated city settings, no firmware/audio/microphone"]
    fn p4_weather_resource_package_hardware(){
        use serde_json::{json,Value};
        assert_eq!(std::env::var("PET_WEATHER_LIVE_TEST").as_deref(),Ok("1"));
        let port=std::env::var("P4_SERIAL_PORT").expect("explicit connected port");
        let package=PathBuf::from(std::env::var("P4_WIDGET_DIR").expect("explicit weather package"));
        let entry=crate::component_library::inspect(&package).unwrap();
        assert!(entry.valid,"{:?}",entry.validation_errors);assert_eq!(entry.id,"hachimi-weather");
        let storage=tempfile::tempdir().unwrap();let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();
        usb.connect(&port,move|topic,payload|{if matches!(topic.as_str(),"app/event"|"miniapp/state"|"protocol/ack"){let _=tx.send((topic,payload));}}).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}let _close=Close(usb.clone());
        let board=usb.status().board_device_id;assert!(!board.is_empty());
        assert_eq!(usb.status().capabilities["widgetAppHost"],"p4-app-host-v1");
        let before=usb.query_diagnostics(&board).unwrap();let buttons=usb.query_button_config(&board).unwrap();
        let inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["complete"],true);
        let ids=|v:&Value|v["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().to_string()).collect::<HashSet<_>>();
        let media=usb.media_request(&board,"media/query",json!({})).unwrap();
        assert!(!matches!(media["state"].as_str(),Some("playing"|"buffering"|"paused")),"do not interrupt active audio");
        if std::env::var("P4_WEATHER_ALREADY_INSTALLED").as_deref()!=Ok("1"){
            usb.install_widget_clawpkg("hachimi-weather",&package,&HashMap::new(),|file,total,bytes|eprintln!("weather sync {file}/{total}: {bytes} bytes")).unwrap();
        }else{
            assert!(inventory["items"].as_array().unwrap().iter().any(|i|i["id"]=="hachimi-weather"&&i["active"]==true),"reuse only the previously confirmed active weather install");
        }
        let request=|input|crate::component_app::test_request(&package,storage.path(),input,&usb).unwrap();
        let mut latest=Value::Null;let mut seen=HashSet::new();
        let mut pump=|seconds:u64|{
            usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();
            let until=Instant::now()+Duration::from_secs(seconds);let mut queried=Instant::now();
            while Instant::now()<until{
                if queried.elapsed()>Duration::from_millis(400){usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();queried=Instant::now();}
                let Ok((topic,p))=rx.recv_timeout(Duration::from_millis(40))else{continue;};
                if topic=="miniapp/state"&&p["widgetId"]=="hachimi-weather"{latest=p;continue;}
                // A new input intentionally invalidates an in-flight older status
                // reply. Assert final displayed state below, not every raced ACK.
                if topic=="protocol/ack"&&p["requestTopic"]=="app/snapshot"&&p["ok"]==false{
                    eprintln!("weather snapshot ACK rejected (checking converged state): {p}");
                }
                if topic!="app/event"||p["componentId"]!="hachimi-weather"||p["visible"]!=true{continue;}
                assert_eq!(p["boardDeviceId"],board);
                if !seen.insert(format!("{}:{}",p["instance"],p["sequence"])){continue;}
                // Only this weather package's declared actions; never microphone or task dispatch.
                let action=p["action"].as_str().unwrap_or("status");
                assert!(matches!(action,"status"|"host.choose"|"host.previous"|"host.next"|"host.primary"|"host.secondary"|"host.details"));
                let result=request(if action=="status"{json!({"operation":"status"})}else{json!({"operation":"action","action":action})});
                usb.send_app_snapshot(&board,&json!({"componentId":"hachimi-weather","instance":p["instance"],"sequence":p["sequence"],"values":result["values"]})).unwrap();
            }latest.clone()
        };
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();
        for (city,lat,lon) in [("北京",39.904,116.407),("上海",31.230,121.474),("北京",39.904,116.407)]{
            request(json!({"operation":"configure","city":{"city":city,"lat":lat,"lon":lon}}));
            let state=pump(30);
            assert!(state["eyebrow"].as_str().unwrap_or("").contains(city),"device did not receive selected city: {}",state["eyebrow"]);
            assert!(state["metricValue"].as_str().unwrap_or("").contains("°C"),"real forecast not displayed: {}",state["headline"]);
            eprintln!("PASS selected {city}: real forecast + correlated device snapshot");
        }
        for (action,headline) in [("host.choose","北京"),("host.next","上海")]{
            usb.send_to_board(&board,"miniapp/event",&json!({"action":action})).unwrap();
            let shown=pump(2);assert_eq!(shown["headline"],headline);
            assert_eq!(shown["eyebrow"],"北京 · 切换");
        }
        usb.send_to_board(&board,"miniapp/event",&json!({"action":"host.secondary"})).unwrap();pump(2);
        assert_eq!(request(json!({"operation":"status"}))["state"]["city"]["city"],"北京");
        for action in ["host.choose","host.next","host.primary"]{
            usb.send_to_board(&board,"miniapp/event",&json!({"action":action})).unwrap();pump(2);
        }
        assert_eq!(request(json!({"operation":"status"}))["state"]["city"]["city"],"上海");
        assert_eq!(pump(2)["eyebrow"],"上海 · 切换");
        request(json!({"operation":"configure","city":{"city":"北京","lat":39.904,"lon":116.407}}));pump(2);
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();pump(1);
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();
        assert!(pump(4)["eyebrow"].as_str().unwrap_or("").contains("北京"));
        let after=usb.query_diagnostics(&board).unwrap();let next=usb.query_widget_inventory(&board).unwrap();assert_eq!(next["complete"],true);
        let mut expected=ids(&inventory);expected.insert("hachimi-weather".into());assert_eq!(ids(&next),expected);
        assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
        assert_eq!(before["storage"]["activeAppearanceSlot"],after["storage"]["activeAppearanceSlot"]);
        assert_eq!(buttons["config"],usb.query_button_config(&board).unwrap()["config"]);
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();
        eprintln!("PASS weather-only sync; Beijing/Shanghai/Beijing; device picker cancel/confirm; exit/reentry; original catalog/appearance/buttons preserved; no user city files changed; physical touch still requires user verification");
    }

    #[test]
    #[ignore = "Explicit approval: sync knowledge only; no firmware, microphone, speech or other components"]
    fn p4_knowledge_library_hardware(){
        use serde_json::{json,Value};
        assert_eq!(std::env::var("PET_KNOWLEDGE_LIVE_TEST").as_deref(),Ok("1"));
        let port=std::env::var("P4_SERIAL_PORT").expect("explicit connected port");
        let package=PathBuf::from(std::env::var("PET_KNOWLEDGE_PACKAGE").expect("knowledge package"));
        let entry=crate::component_library::inspect(&package).unwrap();assert!(entry.valid,"{:?}",entry.validation_errors);assert_eq!(entry.id,"hachimi-learn");
        let storage=tempfile::tempdir().unwrap();let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();
        usb.connect(&port,move|topic,payload|{if matches!(topic.as_str(),"app/event"|"miniapp/state"|"protocol/ack"){let _=tx.send((topic,payload));}}).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){let b=self.0.status().board_device_id;let _=self.0.send_to_board(&b,"control/screen-page",&json!({"page":"main"}));self.0.disconnect();}}let _close=Close(usb.clone());
        let board=usb.status().board_device_id;assert!(!board.is_empty());assert_eq!(usb.status().capabilities["widgetAppHost"],"p4-app-host-v1");
        let before=usb.query_diagnostics(&board).unwrap();let buttons=usb.query_button_config(&board).unwrap();
        let inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["complete"],true);
        let ids=|v:&Value|v["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().to_string()).collect::<HashSet<_>>();
        let media=usb.media_request(&board,"media/query",json!({})).unwrap();assert!(!matches!(media["state"].as_str(),Some("playing"|"buffering"|"paused")),"do not interrupt active audio");
        usb.install_widget_clawpkg("hachimi-learn",&package,&HashMap::new(),|file,total,bytes|eprintln!("knowledge sync {file}/{total}: {bytes} bytes")).unwrap();
        let request=|input|crate::component_app::test_request(&package,storage.path(),input,&usb).unwrap();
        let mut latest=Value::Null;let mut seen=HashSet::new();
        let mut pump=|seconds:u64|{
            let until=Instant::now()+Duration::from_secs(seconds);let mut queried=Instant::now()-Duration::from_secs(1);
            while Instant::now()<until{
                if queried.elapsed()>Duration::from_millis(400){usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();queried=Instant::now();}
                let Ok((topic,p))=rx.recv_timeout(Duration::from_millis(40))else{continue;};
                if topic=="miniapp/state"&&p["widgetId"]=="hachimi-learn"{latest=p;continue;}
                if topic!="app/event"||p["componentId"]!="hachimi-learn"||p["visible"]!=true{continue;}
                assert_eq!(p["boardDeviceId"],board);if !seen.insert(format!("{}:{}",p["instance"],p["sequence"])){continue;}
                let action=p["action"].as_str().unwrap_or("status");
                // Speech is not authorized in this acceptance; never call it.
                assert!(matches!(action,"status"|"host.primary"|"host.secondary"|"host.previous"|"host.next"|"host.favorite"),"speech excluded");
                let result=request(if action=="status"{json!({"operation":"status"})}else{json!({"operation":"action","action":action})});
                usb.send_app_snapshot(&board,&json!({"componentId":"hachimi-learn","instance":p["instance"],"sequence":p["sequence"],"values":result["values"]})).unwrap();
            }latest.clone()
        };
        let event=|action:&str|usb.send_to_board(&board,"miniapp/event",&json!({"action":action})).unwrap();
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();pump(4);
        let initial=request(json!({"operation":"status"}));
        let pages=initial["knowledge"]["pages"].as_u64().unwrap();let title=initial["values"]["headline"].clone();
        for page in 0..pages{
            if page>0{event("host.next");}
            let shown=pump(2);let expected=request(json!({"operation":"status"}));
            assert_eq!(shown["headline"],title);assert_eq!(shown["note"],expected["values"]["note"]);assert_eq!(shown["footer"],expected["values"]["footer"]);assert_eq!(shown["badge"],expected["values"]["badge"]);
        }
        event("host.next");pump(2);assert_eq!(request(json!({"operation":"status"}))["knowledge"]["page"],pages-1);
        for _ in 0..10{
            let prior=request(json!({"operation":"status"}));event("host.secondary");pump(2);event("host.next");pump(2);event("host.primary");
            let shown=pump(2);let selected=request(json!({"operation":"status"}));
            assert_ne!(selected["knowledge"]["category"],prior["knowledge"]["category"]);assert_eq!(selected["knowledge"]["page"],0);
            assert_eq!(shown["eyebrow"],selected["values"]["eyebrow"]);assert_eq!(shown["headline"],selected["values"]["headline"]);
            eprintln!("PASS device category {}, article {}, pages {}",selected["knowledge"]["category"],selected["knowledge"]["current"]["id"],selected["knowledge"]["pages"]);
        }
        let before_cancel=request(json!({"operation":"status"}));
        event("host.secondary");pump(2);event("host.next");pump(2);event("host.secondary");pump(2);
        assert_eq!(request(json!({"operation":"status"}))["knowledge"]["current"]["id"],before_cancel["knowledge"]["current"]["id"]);
        event("host.primary");pump(2);let final_state=request(json!({"operation":"status"}));assert_ne!(final_state["knowledge"]["current"]["id"],before_cancel["knowledge"]["current"]["id"]);
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();pump(1);
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();let reentry=pump(3);assert_eq!(reentry["headline"],final_state["values"]["headline"]);
        let after=usb.query_diagnostics(&board).unwrap();let next=usb.query_widget_inventory(&board).unwrap();assert_eq!(next["complete"],true);
        let mut expected=ids(&inventory);expected.insert("hachimi-learn".into());assert_eq!(ids(&next),expected);
        assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
        assert_eq!(before["storage"]["activeAppearanceSlot"],after["storage"]["activeAppearanceSlot"]);assert_eq!(buttons["config"],usb.query_button_config(&board).unwrap()["config"]);
        assert!(!crate::component_audio::playing("hachimi-learn"));
        eprintln!("PASS knowledge-only sync, ten category loop, random/confirm/cancel, complete article pages, exit/reentry; no resets, firmware, speech or user-save changes. Physical touch/visual quality still require user verification.");
    }

    #[test]
    #[ignore = "Explicit approval: journey-only device selection; isolated progress, no audio/firmware"]
    fn p4_journey_device_selection_hardware(){
        use serde_json::{json,Value};
        assert_eq!(std::env::var("PET_JOURNEY_LIVE_TEST").as_deref(),Ok("1"));
        let package=PathBuf::from(std::env::var("PET_JOURNEY_PACKAGE").unwrap());
        let entry=crate::component_library::inspect(&package).unwrap();assert!(entry.valid);assert_eq!(entry.id,"hachimi-travel");
        let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();
        let (frame_tx,frame_rx)=mpsc::channel();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|topic,payload|{
            if topic.starts_with("debug/"){let _=frame_tx.send((topic.clone(),payload.clone()));}
            if matches!(topic.as_str(),"app/event"|"miniapp/state"){let _=tx.send((topic,payload));}
        }).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){let b=self.0.status().board_device_id;let _=self.0.send_to_board(&b,"control/screen-page",&json!({"page":"main"}));self.0.disconnect();}}let _close=Close(usb.clone());
        let board=usb.status().board_device_id;let before=usb.query_diagnostics(&board).unwrap();let buttons=usb.query_button_config(&board).unwrap();let inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["complete"],true);
        let media=usb.media_request(&board,"media/query",json!({})).unwrap();assert!(!matches!(media["state"].as_str(),Some("playing"|"buffering"|"paused")));
        usb.install_widget_clawpkg("hachimi-travel",&package,&HashMap::new(),|_,_,_|{}).unwrap();
        let storage=tempfile::tempdir().unwrap();let mut seen=HashSet::new();let mut shown=Value::Null;
        let request=|input|crate::component_app::test_request(&package,storage.path(),input,&usb).unwrap();
        let mut pump=||{
            let until=Instant::now()+Duration::from_millis(1600);let mut queried=Instant::now()-Duration::from_secs(1);
            while Instant::now()<until{
                if queried.elapsed()>Duration::from_millis(300){usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();queried=Instant::now();}
                let Ok((topic,p))=rx.recv_timeout(Duration::from_millis(40))else{continue;};
                if topic=="miniapp/state"&&p["widgetId"]=="hachimi-travel"{shown=p;continue;}
                if topic!="app/event"||p["componentId"]!="hachimi-travel"||p["visible"]!=true{continue;}
                if !seen.insert(format!("{}:{}",p["instance"],p["sequence"])){continue;}
                let action=p["action"].as_str().unwrap_or("status");assert!(matches!(action,"status"|"host.primary"|"host.secondary"|"host.previous"|"host.next"));
                let result=request(if action=="status"{json!({"operation":"status"})}else{json!({"operation":"action","action":action})});
                usb.send_app_snapshot(&board,&json!({"componentId":"hachimi-travel","instance":p["instance"],"sequence":p["sequence"],"values":result["values"]})).unwrap();
            }shown.clone()
        };
        let event=|action|usb.send_to_board(&board,"miniapp/event",&json!({"action":action})).unwrap();
        let capture=|index:usize|{
            use base64::Engine;
            let Ok(folder)=std::env::var("PET_JOURNEY_REPORT_DIR")else{return;};
            std::fs::create_dir_all(&folder).unwrap();
            while frame_rx.try_recv().is_ok(){}
            usb.send_to_board(&board,"debug/screenshot",&json!({"fullResolution":true})).unwrap();
            let until=Instant::now()+Duration::from_secs(30);
            let mut identity=Value::Null;let mut bytes=Vec::new();let mut sequence=0;
            while Instant::now()<until{
                let Ok((topic,p))=frame_rx.recv_timeout(Duration::from_millis(100))else{continue;};
                assert_ne!(topic,"debug/screenshot_error","{p}");
                if topic=="debug/screenshot_begin"{
                    assert_eq!(p["width"],640);assert_eq!(p["height"],480);
                    assert_eq!(p["format"],"rgb565le");identity=p["id"].clone();
                }else if topic=="debug/screenshot_chunk"&&p["id"]==identity{
                    assert_eq!(p["index"],sequence);sequence+=1;
                    bytes.extend(base64::engine::general_purpose::STANDARD.decode(p["data"].as_str().unwrap()).unwrap());
                }else if topic=="debug/screenshot_end"&&p["id"]==identity{
                    assert_eq!(bytes.len(),640*480*2);assert_eq!(p["chunks"],sequence);
                    let mut hash=2166136261u32;let mut image=image::RgbImage::new(640,480);
                    for(at,raw)in bytes.chunks_exact(2).enumerate(){
                        let px=u16::from_le_bytes([raw[0],raw[1]]);hash=(hash^px as u32).wrapping_mul(16777619);
                        let(r,g,b)=((px>>11)as u8,((px>>5)&63)as u8,(px&31)as u8);
                        image.put_pixel(at as u32%640,at as u32/640,image::Rgb([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)]));
                    }
                    assert_eq!(p["checksum"],format!("{hash:08x}"));
                    image.save(Path::new(&folder).join(format!("postcard-{index}.png"))).unwrap();return;
                }
            }panic!("Travel screenshot timed out");
        };
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();pump();
        assert_eq!(pump()["headline"],"风铃海岸");
        event("host.secondary");
        for (index,title) in ["风铃海岸","苔藓森林","云朵山丘","星光小镇"].into_iter().enumerate() {
            let card=pump();assert_eq!(card["headline"],title);
            assert_eq!(card["eyebrow"],"默认明信片 · 目的地预览");
            assert!(card["note"].as_str().unwrap().chars().count()>20);
            assert_eq!(card["metricValue"],"已带回 0/4");
            capture(index);
            event("host.next");
        }
        pump();event("host.secondary");pump();
        let empty=request(json!({"operation":"status"}));
        assert_eq!(empty["state"]["visits"],0);assert_eq!(empty["state"]["favorites"],json!([]));
        event("host.next");assert_eq!(pump()["headline"],"苔藓森林");
        event("host.primary");pump();let trip=request(json!({"operation":"status"}))["state"]["trip"].clone();assert_eq!(trip["id"],"forest");
        event("host.next");assert_eq!(pump()["headline"],"云朵山丘");
        assert_eq!(request(json!({"operation":"status"}))["state"]["selected"],1);
        event("host.secondary");assert_eq!(pump()["headline"],"苔藓森林");assert_eq!(request(json!({"operation":"status"}))["state"]["selected"],1);
        event("host.next");pump();event("host.primary");let current=pump();assert_eq!(current["headline"],"苔藓森林");assert!(current["note"].as_str().unwrap().contains("云朵山丘"));
        let saved=request(json!({"operation":"status"}));assert_eq!(saved["state"]["trip"],trip);assert_eq!(saved["state"]["selected"],2);
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();pump();
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();assert_eq!(pump()["headline"],"苔藓森林");assert_eq!(request(json!({"operation":"status"}))["state"]["trip"],trip);
        let after=usb.query_diagnostics(&board).unwrap();let next=usb.query_widget_inventory(&board).unwrap();assert_eq!(next["complete"],true);
        let ids=|v:&Value|v["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().to_string()).collect::<HashSet<_>>();
        let mut expected=ids(&inventory);expected.insert("hachimi-travel".into());assert_eq!(ids(&next),expected);
        assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);assert_eq!(buttons["config"],usb.query_button_config(&board).unwrap()["config"]);
        eprintln!("PASS device destination selection, departure, active trip candidate cancel/confirm, no timer restart, exit/reentry, other widget inventory and keys preserved; isolated progress, no firmware/audio. Physical touch still requires manual verification.");
    }

    #[test]
    #[ignore = "Explicit approval and prior notice: knowledge automatic speech; no microphone or firmware"]
    fn p4_knowledge_autoread_hardware(){
        use serde_json::{json,Value};
        assert_eq!(std::env::var("PET_KNOWLEDGE_AUDIO_TEST").as_deref(),Ok("1"));
        let package=PathBuf::from(std::env::var("PET_KNOWLEDGE_PACKAGE").unwrap());
        let entry=crate::component_library::inspect(&package).unwrap();assert!(entry.valid);assert_eq!(entry.id,"hachimi-learn");
        let config=PathBuf::from(std::env::var("P4_SERVICE_CONFIG").unwrap());
        crate::volcengine_asr::configure_storage_dir(config.clone()).unwrap();
        crate::voice_chat_settings::configure_storage_dir(config).unwrap();
        let volume=(crate::voice_chat_settings::realtime_volume().unwrap().clamp(0.0,0.35)*100.0)as u32;
        let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|topic,payload|{
            crate::component_audio::observe(&topic,&payload);
            if topic=="app/event"{if let Some(id)=payload["componentId"].as_str(){crate::component_audio::heartbeat(id,payload["visible"]==true);}let _=tx.send(payload);}
        }).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){crate::component_audio::stop(Some("hachimi-learn"));let b=self.0.status().board_device_id;let _=self.0.send_to_board(&b,"control/screen-page",&json!({"page":"main"}));thread::sleep(Duration::from_millis(1500));self.0.disconnect();}}
        let _close=Close(usb.clone());let board=usb.status().board_device_id;
        let prior=usb.media_request(&board,"media/query",json!({})).unwrap();assert!(!matches!(prior["state"].as_str(),Some("playing"|"buffering"|"paused")));
        let before=usb.query_diagnostics(&board).unwrap();let buttons=usb.query_button_config(&board).unwrap();
        usb.install_widget_clawpkg("hachimi-learn",&package,&HashMap::new(),|_,_,_|{}).unwrap();
        let storage=tempfile::tempdir().unwrap();let mut narration=crate::knowledge_narration::Narration::default();
        let mut seen=HashSet::new();let mut starts=0;let mut visible=false;let mut epoch=0;let mut article=Value::Null;
        let mut pump=|ms:u64|{
            let until=Instant::now()+Duration::from_millis(ms);
            while Instant::now()<until{
                let Ok(p)=rx.recv_timeout(Duration::from_millis(40))else{continue;};
                if p["componentId"]!="hachimi-learn"{continue;}
                let now=p["visible"]==true;if now!=visible{epoch+=1;visible=now;}if !visible{continue;}
                let action=p["action"].as_str().unwrap_or("status");
                if action!="status"&&!seen.insert(format!("{}:{}",p["instance"],p["sequence"])){continue;}
                assert!(matches!(action,"status"|"host.primary"),"only approved test actions");
                let result=crate::component_app::test_request(&package,storage.path(),if action=="status"{json!({"operation":"status"})}else{json!({"operation":"action","action":action})},&usb).unwrap();
                article=result["knowledge"]["current"]["id"].clone();
                let audio=crate::component_audio::status("hachimi-learn");assert_ne!(audio["phase"],"error","{}",audio["error"]);
                if let Some(text)=narration.next(epoch,&result,audio["phase"].as_str().unwrap_or("idle")){
                    crate::component_audio::start("hachimi-learn",&usb,crate::component_audio::Source::Speech(text),volume).unwrap();
                    starts+=1;eprintln!("AUTO NARRATION segment start {starts}");
                }
                usb.send_app_snapshot(&board,&json!({"componentId":"hachimi-learn","instance":p["instance"],"sequence":p["sequence"],"values":result["values"]})).unwrap();
            }
            (starts,article.clone(),crate::component_audio::status("hachimi-learn")["phase"].clone())
        };
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();
        let deadline=Instant::now()+Duration::from_secs(110);
        let first=loop{let s=pump(250);assert!(Instant::now()<deadline,"no automatic sequential playback");if s.0>=2&&s.2=="playing"{break s;}};
        assert_eq!(first.0,2,"polling must not duplicate active speech");eprintln!("PASS automatic entry and natural completion advanced to second segment");
        usb.send_to_board(&board,"miniapp/event",&json!({"action":"host.primary"})).unwrap();
        let deadline=Instant::now()+Duration::from_secs(45);
        loop{let s=pump(250);assert!(Instant::now()<deadline,"new article did not play");if s.1!=first.1&&s.2=="playing"{assert_eq!(s.0,3);break;}}
        pump(1500);usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();
        pump(2500);assert!(!crate::component_audio::playing("hachimi-learn"));
        let stopped=usb.media_request(&board,"media/query",json!({})).unwrap();assert!(!matches!(stopped["state"].as_str(),Some("playing"|"buffering"|"paused")));
        assert_eq!(pump(3000).0,3,"exit must not restart speech");
        let after=usb.query_diagnostics(&board).unwrap();assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
        assert_eq!(buttons["config"],usb.query_button_config(&board).unwrap()["config"]);
        eprintln!("PASS device entry auto-read, bounded sequential speech, random changes article, exact-session exit stop without replay; no microphone/firmware/user-save changes. Uses production host requests + narration/audio modules, not desktop UI.");
    }

    #[test]
    #[ignore = "Explicit approval: install eight retained Hachimi components without removing existing data"]
    fn p4_hachimi_all_packages_hardware() {
        let root=PathBuf::from(std::env::var("P4_APP_PACKAGES").expect("explicit package root"));
        let port=std::env::var("P4_SERIAL_PORT").expect("explicit port");
        let board=std::env::var("P4_EXPECTED_BOARD_ID").expect("explicit board");
        let ids=["hachimi-garden","hachimi-travel","hachimi-weather","hachimi-notes","hachimi-learn","hachimi-sounds","hachimi-break","hachimi-home"];
        // Preflight every package before any write.
        let entries:Vec<_>=ids.iter().map(|id|{let e=crate::component_library::inspect(&root.join(id)).unwrap();assert!(e.valid,"{id}: {:?}",e.validation_errors);assert_eq!(&e.id,id);e}).collect();
        let usb=UsbSerialManager::new();usb.set_desktop_device_id("hachimi-hil");usb.connect(&port,|_,_|{}).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}let _close=Close(usb.clone());
        assert_eq!(usb.status().board_device_id,board);
        assert_eq!(usb.status().capabilities["widgetAppInteraction"],"p4-app-interaction-v1");
        let before=usb.query_diagnostics(&board).unwrap();let config=usb.query_button_config(&board).unwrap();
        let inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["complete"],true);
        let mut expected:HashSet<String>=inventory["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().into()).collect();
        for id in ids{expected.insert(id.into());}
        assert!(expected.len()<=inventory["maxInstalled"].as_u64().unwrap() as usize);
        for e in entries{
            eprintln!("INSTALL {}",e.id);
            usb.install_widget_clawpkg(&e.id,&root.join(&e.id),&HashMap::new(),|_,_,_|{}).unwrap();
            usb.send_to_board(&board,"control/screen-page",&serde_json::json!({"page":"app"})).unwrap();
            thread::sleep(Duration::from_secs(2));
            let after=usb.query_diagnostics(&board).unwrap();assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
            assert_eq!(after["runtime"]["screenPage"],"app");
        }
        let after=usb.query_diagnostics(&board).unwrap();let inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["complete"],true);
        let actual:HashSet<String>=inventory["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().into()).collect();assert_eq!(actual,expected);
        assert_eq!(before["storage"]["activeAppearanceSlot"],after["storage"]["activeAppearanceSlot"]);
        assert_eq!(config["config"],usb.query_button_config(&board).unwrap()["config"]);
        eprintln!("PASS: ten packages installed/rendered without resets; {} total entries, original appearance and global buttons preserved. Host services NOT verified by this test.",actual.len());
    }

    #[test]
    #[ignore = "Explicit device acceptance: eight retained components, short speaker test, NO microphone or external task dispatch"]
    fn p4_hachimi_acceptance_without_microphone() {
        use serde_json::{json,Value};
        let port=std::env::var("P4_SERIAL_PORT").expect("explicit port");
        let board=std::env::var("P4_EXPECTED_BOARD_ID").expect("explicit board");
        let root=PathBuf::from(std::env::var("P4_APP_PACKAGES").expect("explicit packages"));
        let storage=tempfile::tempdir().unwrap();let usb=UsbSerialManager::new();
        let (tx,rx)=mpsc::channel();
        usb.connect(&port,move|topic,payload|{
            crate::component_audio::observe(&topic,&payload);
            if topic=="app/event"{if let Some(id)=payload["componentId"].as_str(){crate::component_audio::heartbeat(id,payload["visible"]==true);}}
            if matches!(topic.as_str(),"app/event"|"miniapp/state"|"protocol/ack"){let _=tx.send((topic,payload));}
        }).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){crate::component_audio::stop(None);thread::sleep(Duration::from_millis(1500));self.0.disconnect();}}let _close=Close(usb.clone());
        assert_eq!(usb.status().board_device_id,board);let before=usb.query_diagnostics(&board).unwrap();
        let config=usb.query_button_config(&board).unwrap();let inventory=usb.query_widget_inventory(&board).unwrap();
        let prior=usb.media_request(&board,"media/query",json!({})).unwrap();assert!(!matches!(prior["state"].as_str(),Some("playing"|"buffering"|"paused")));
        let mut latest=Value::Null;let mut seen=HashSet::new();
        // Hardware requests pass through production declaration/authorization/storage/application logic.
        // Only the desktop UI notification is deliberately unavailable; recording is never invoked.
        let mut pump=|id:&str,ms:u64,respond:bool|{
            let deadline=Instant::now()+Duration::from_millis(ms);
            while Instant::now()<deadline{
                let Ok((topic,p))=rx.recv_timeout(Duration::from_millis(40))else{continue;};
                if topic=="miniapp/state"&&p["widgetId"]==id{latest=p;continue;}
                if topic=="protocol/ack"&&p["requestTopic"]=="app/snapshot"{assert_ne!(p["ok"],false,"snapshot rejected: {p}");}
                if topic!="app/event"||p["componentId"]!=id||p["visible"]!=true||!respond{continue;}
                assert_eq!(p["boardDeviceId"],board);
                let action=p["action"].as_str().unwrap();let key=format!("{}:{}",p["instance"],p["sequence"]);
                if !seen.insert(key){continue;}
                assert!(!(id=="hachimi-notes"&&action=="host.primary"),"recording intentionally deferred");
                assert!(!(id=="hachimi-agent"&&action=="host.primary"),"real-time conversation intentionally deferred");
                let input=if action=="status"{json!({"operation":"status"})}else{json!({"operation":"action","action":action})};
                let result=crate::component_app::test_request(&root.join(id),storage.path(),input,&usb).expect("production host request");
                usb.send_app_snapshot(&board,&json!({"componentId":id,"instance":p["instance"],"sequence":p["sequence"],"values":result["values"]})).unwrap();
            }
            latest.clone()
        };
        for id in ["hachimi-home","hachimi-garden","hachimi-break","hachimi-travel","hachimi-weather","hachimi-notes","hachimi-learn","hachimi-sounds"]{
            let package=root.join(id);usb.install_widget_clawpkg(id,&package,&HashMap::new(),|_,_,_|{}).unwrap();
            usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();pump(id,2400,true);
            usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();let initial=pump(id,300,true);assert_eq!(initial["widgetId"],id);
            let actions:Vec<&str>=match id{
                "hachimi-home"=>vec!["previous","primary","primary","primary","secondary","primary","primary","primary","secondary","primary","primary","primary","secondary"],
                "hachimi-garden"=>if initial["state"]=="run"{vec!["primary","secondary","secondary"]}else{vec!["primary","primary","secondary","secondary"]},
                "hachimi-desk"=>vec!["more","less"],
                "hachimi-break"=>vec!["next","previous","primary","primary"],
                "hachimi-travel"=>vec!["host.next","host.primary","host.secondary","host.previous","host.secondary"],
                "hachimi-weather"=>vec!["host.details","host.details"],
                "hachimi-agent"=>vec!["host.secondary","host.next","host.previous","host.secondary"],
                "hachimi-notes"=>vec!["host.next","host.previous"],
                "hachimi-learn"=>vec!["host.next","host.secondary","host.secondary","host.previous"],
                _=>vec!["host.next","host.up","host.down","host.secondary","host.primary"],
            };
            for action in actions{usb.send_to_board(&board,"miniapp/event",&json!({"action":action})).unwrap();pump(id,350,true);}
            if id=="hachimi-sounds"{
                for _ in 0..30{pump(id,250,true);if crate::component_audio::status(id)["phase"]=="playing"{break;}}
                assert_eq!(crate::component_audio::status(id)["phase"],"playing");pump(id,2500,true);
            }
            usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();let after=pump(id,300,true);
            assert!(!matches!(after["headline"].as_str(),Some("正在处理"|"等待电脑连接"|"操作未完成")),"{id} host loop did not settle");
            if id=="hachimi-garden"{assert_eq!(after["state"],"paused");}
            if id=="hachimi-travel"{assert!(after["metricValue"].as_str().unwrap().contains("分钟"));}
            if id=="hachimi-weather"{assert_eq!(after["headline"],"先选一座城市");}
            usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();pump(id,1200,true);
            if id=="hachimi-sounds"{
                for _ in 0..20{if crate::component_audio::status(id)["phase"]=="idle"{break;}pump(id,150,true);}
                assert_eq!(crate::component_audio::status(id)["phase"],"idle");
                assert!(!matches!(usb.media_request(&board,"media/query",json!({})).unwrap()["state"].as_str(),Some("playing"|"buffering"|"paused")));
            }
            usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();pump(id,2400,true);
            if package.join("pc/application.json").exists(){
                pump(id,11000,false);usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();assert_eq!(pump(id,300,false)["headline"],"等待电脑连接");
                pump(id,2400,true);usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();assert_ne!(pump(id,300,true)["headline"],"等待电脑连接");
            }
            let d=usb.query_diagnostics(&board).unwrap();assert_eq!(d["bootCount"],before["bootCount"]);assert_eq!(d["faultResetCount"],before["faultResetCount"]);
            eprintln!("ACCEPTED {id}: real device action/state + host snapshots where applicable, exit/reentry, host timeout/recovery; recording/voice excluded");
        }
        let after=usb.query_widget_inventory(&board).unwrap();assert_eq!(after["items"].as_array().unwrap().len(),inventory["items"].as_array().unwrap().len());
        assert_eq!(usb.query_button_config(&board).unwrap()["config"],config["config"]);
        assert_eq!(crate::component_notes::diagnostics("hachimi-notes"),Value::Null);assert!(!storage.path().join("hachimi-notes/notes.json").exists());
        eprintln!("PASS: all ten device loops; private service state isolated; no microphone, no notes/todos, no external task dispatched");
    }

    #[test]
    #[ignore = "Explicit device acceptance: preserve/reboot the paused garden; no audio or microphone"]
    fn p4_hachimi_saved_garden_survives_reboot() {
        use serde_json::{json,Value};
        let port=std::env::var("P4_SERIAL_PORT").expect("explicit port");
        let board=std::env::var("P4_EXPECTED_BOARD_ID").expect("explicit board");
        let root=PathBuf::from(std::env::var("P4_APP_PACKAGES").expect("explicit packages"));
        let usb=UsbSerialManager::new();let (tx,rx)=mpsc::channel();
        let sender=tx.clone();usb.connect(&port,move|topic,payload|{if topic=="miniapp/state"{let _=sender.send(payload);}}).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}let _close=Close(usb.clone());
        assert_eq!(usb.status().board_device_id,board);
        let audio=usb.media_request(&board,"media/query",json!({})).unwrap();assert!(!matches!(audio["state"].as_str(),Some("playing"|"buffering"|"paused")));
        let before=usb.query_diagnostics(&board).unwrap();let buttons=usb.query_button_config(&board).unwrap();
        let inventory=usb.query_widget_inventory(&board).unwrap();
        if inventory["activeWidgetId"]!="hachimi-garden" {
            usb.install_widget_clawpkg("hachimi-garden",&root.join("hachimi-garden"),&HashMap::new(),|_,_,_|{}).unwrap();
        }
        let query=||->Value{while rx.try_recv().is_ok(){}usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();rx.recv_timeout(Duration::from_secs(5)).unwrap()};
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();thread::sleep(Duration::from_millis(500));
        let saved=query();assert_eq!(saved["widgetId"],"hachimi-garden");assert_eq!(saved["state"],"paused","run the non-recording acceptance first; do not disturb an active timer");
        // Save worker coalesces writes for 30s. Keep the real production policy.
        thread::sleep(Duration::from_secs(35));
        let ack=usb.reboot_device(&board).unwrap();assert_eq!(ack["preservedAppearanceAssets"],true);
        thread::sleep(Duration::from_secs(3));usb.disconnect();
        let deadline=Instant::now()+Duration::from_secs(30);
        loop {let sender=tx.clone();if usb.connect(&port,move|topic,payload|{if topic=="miniapp/state"{let _=sender.send(payload);}}).is_ok(){break;}assert!(Instant::now()<deadline);thread::sleep(Duration::from_millis(500));}
        assert_eq!(usb.status().board_device_id,board);
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();thread::sleep(Duration::from_millis(600));
        let restored=query();for key in ["widgetId","state","page","metricValue","metricLabel","metricUnit","progressPercent"]{assert_eq!(restored[key],saved[key],"saved field {key}");}
        let after=usb.query_diagnostics(&board).unwrap();assert_eq!(after["faultResetCount"],before["faultResetCount"]);
        assert!(after["bootCount"].as_u64().unwrap()>before["bootCount"].as_u64().unwrap());
        assert_eq!(after["storage"]["activeAppearanceSlot"],before["storage"]["activeAppearanceSlot"]);
        assert_eq!(buttons["config"],usb.query_button_config(&board).unwrap()["config"]);
        eprintln!("PASS: paused garden timer/page/progress restored after real reboot; appearance/global buttons preserved");
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();
    }

    #[test]
    #[ignore = "Explicit approval and notice required: real component microphone or speaker; never saves notes/todos"]
    fn p4_hachimi_service_hardware() {
        let mode=std::env::var("P4_SERVICE_TEST").expect("notes, sound, or speech");
        assert!(matches!(mode.as_str(),"notes"|"sound"|"speech"));
        let port=std::env::var("P4_SERIAL_PORT").expect("explicit port");
        let board=std::env::var("P4_EXPECTED_BOARD_ID").expect("explicit board");
        let root=PathBuf::from(std::env::var("P4_APP_PACKAGES").expect("explicit package root"));
        let id=match mode.as_str(){"notes"=>"hachimi-notes","sound"=>"hachimi-sounds",_=>"hachimi-learn"};
        let package=root.join(id);let entry=crate::component_library::inspect(&package).unwrap();assert!(entry.valid);
        #[cfg(target_os="macos")]
        if mode!="sound" {
            let config=PathBuf::from(std::env::var("P4_SERVICE_CONFIG").expect("existing app config directory"));
            crate::volcengine_asr::configure_storage_dir(config.clone()).unwrap();
            crate::voice_chat_settings::configure_storage_dir(config).unwrap();
        }
        if mode=="notes"{assert!(crate::volcengine_asr::settings_status().unwrap().configured,"ASR not configured; microphone not started");}
        let usb=UsbSerialManager::new();usb.set_desktop_device_id("hachimi-service-hil");
        let event=std::sync::Arc::new(std::sync::Mutex::new(serde_json::Value::Null));let last_event=event.clone();
        usb.connect(&port,move|topic,payload|{crate::component_notes::route(&topic,&payload);crate::component_audio::observe(&topic,&payload);if topic=="app/event"{if let Some(id)=payload["componentId"].as_str(){crate::component_audio::heartbeat(id,payload["visible"]==true);}*last_event.lock().unwrap()=payload;}}).unwrap();
        struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){crate::component_notes::cancel(None);crate::component_audio::stop(None);thread::sleep(Duration::from_millis(1500));self.0.disconnect();}}let _close=Close(usb.clone());
        assert_eq!(usb.status().board_device_id,board);
        let before=usb.query_diagnostics(&board).unwrap();let prior=usb.media_request(&board,"media/query",serde_json::json!({})).unwrap();
        assert!(!matches!(prior["state"].as_str(),Some("playing"|"buffering"|"paused")),"do not interrupt existing audio");
        usb.install_widget_clawpkg(id,&package,&HashMap::new(),|_,_,_|{}).unwrap();
        usb.send_to_board(&board,"control/screen-page",&serde_json::json!({"page":"app"})).unwrap();thread::sleep(Duration::from_secs(1));
        for _ in 0..20{if event.lock().unwrap()["componentId"]==id{break;}thread::sleep(Duration::from_millis(200));}
        assert_eq!(event.lock().unwrap()["boardDeviceId"],board,"host events must carry the real device identity");
        if mode=="notes"{
            let dir=tempfile::tempdir().unwrap();
            let show=|mut values:serde_json::Value|{
                if let Some(note)=values["note"].as_str(){values["note"]=crate::widget_data::bounded_text(note,156).into();}
                let event=event.lock().unwrap().clone();
                if event["componentId"]==id{usb.send_app_snapshot(&board,&serde_json::json!({"componentId":id,"instance":event["instance"],"sequence":event["sequence"],"values":values})).unwrap();}
            };
            eprintln!("MICROPHONE PREPARING: device countdown, then 20 seconds; nothing will be saved");
            for seconds in (1..=3).rev(){show(serde_json::json!({"headline":"准备录音测试","metricValue":seconds.to_string(),"note":"看到正在录音后，请说：明天下午记得买水果"}));thread::sleep(Duration::from_secs(1));}
            crate::component_notes::execute(id,"record",&serde_json::json!({}),dir.path(),&usb).unwrap();
            let mut recording_notified=false;
            for _ in 0..80{
                let mut s=crate::component_notes::execute(id,"status",&serde_json::json!({}),dir.path(),&usb).unwrap();
                if s["headline"]=="正在录音"&&!recording_notified{eprintln!("MICROPHONE RECORDING CONFIRMED: speak now");recording_notified=true;}
                if s["headline"]=="正在录音"&&s["note"].as_str().unwrap_or("").is_empty(){s["note"]="请说：明天下午记得买水果；测试不会保存笔记".into();}
                show(s);
                thread::sleep(Duration::from_millis(250));
            }
            crate::component_notes::stop_recording(id);
            let mut draft=false;let mut final_phase=String::new();
            for _ in 0..80{
                let s=crate::component_notes::execute(id,"status",&serde_json::json!({}),dir.path(),&usb).unwrap();
                show(s.clone());
                final_phase=s["headline"].as_str().unwrap_or("").into();
                if s["headline"]=="转写完成 · 待确认"{assert!(!s["note"].as_str().unwrap_or("").trim().is_empty());draft=true;break;}
                if s["headline"]=="录音未完成"{panic!("ASR failed: {}; diagnostic={}",s["note"],crate::component_notes::diagnostics(id));}
                thread::sleep(Duration::from_millis(250));
            }
            eprintln!("Capture diagnostic: {}",crate::component_notes::diagnostics(id));
            crate::component_notes::cancel(Some(id));assert!(draft,"no final nonempty draft; phase={final_phase}");assert!(!dir.path().join("notes.json").exists());
            eprintln!("PASS: real device capture -> configured ASR -> nonempty final draft; text not logged; no note/todo saved");
        }else{
            let source=if mode=="sound"{crate::component_audio::Source::Sound{kind:0,minutes:1}}else{crate::component_audio::Source::Speech("这是哈基米知识卡的朗读测试，测试结束后会自动停止。".into())};
            crate::component_audio::start(id,&usb,source,35).unwrap();let mut heard=false;
            for _ in 0..180{
                crate::component_audio::heartbeat(id,true);let s=crate::component_audio::status(id);
                assert_ne!(s["phase"],"error","{}",s["error"]);
                if s["phase"]=="playing"{heard=true;break;}thread::sleep(Duration::from_millis(250));
            }
            assert!(heard,"device never reported playing");
            for _ in 0..32{crate::component_audio::heartbeat(id,true);thread::sleep(Duration::from_millis(250));}
            crate::component_audio::heartbeat(id,false);
            for _ in 0..40{if crate::component_audio::status(id)["phase"]=="idle"{break;}thread::sleep(Duration::from_millis(100));}
            assert_eq!(crate::component_audio::status(id)["phase"],"idle");
            let stopped=usb.media_request(&board,"media/query",serde_json::json!({})).unwrap();assert!(!matches!(stopped["state"].as_str(),Some("playing"|"buffering"|"paused")));
            eprintln!("PASS: {mode} reached device playing, then exact-session stop; listening quality requires user confirmation");
        }
        let after=usb.query_diagnostics(&board).unwrap();assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
    }

    #[test]
    #[ignore = "requires a connected ESP32-P4 and P4_FIRMWARE_BIN"]
    fn p4_firmware_ota_hardware_reboots_into_valid_inactive_slot() {
        let port_name = std::env::var("P4_SERIAL_PORT").unwrap_or_else(|_| "COM5".to_string());
        let firmware_path = std::env::var("P4_FIRMWARE_BIN")
            .map(PathBuf::from)
            .expect("P4_FIRMWARE_BIN is required");
        let manager = UsbSerialManager::new();
        manager.set_desktop_device_id("radio-ota-hil");
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
        if let Ok(expected) = std::env::var("P4_EXPECTED_BOARD_ID") {
            assert_eq!(expected_board_device_id, expected, "refusing OTA to a different board");
        }
        manager.send_to_board(&expected_board_device_id, "audio/conversation", &serde_json::json!({"enabled":false})).expect("stop test audio before OTA");
        let before = manager.query_diagnostics(&expected_board_device_id).unwrap();
        let inventory_before = manager.query_widget_inventory(&expected_board_device_id).unwrap();
        assert_eq!(inventory_before["complete"], true);
        let buttons_before = manager.query_button_config(&expected_board_device_id).unwrap();
        eprintln!("[p4-firmware-hil] before chip={} firmware={} partition={}",
            before["runtime"]["chipRevision"], before["runtime"]["firmware"], before["runtime"]["runningPartition"]);
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
        let after = manager.query_diagnostics(&expected_board_device_id).unwrap();
        eprintln!("[p4-firmware-hil] after chip={} firmware={} partition={} state={}",
            after["runtime"]["chipRevision"], after["runtime"]["firmware"], after["runtime"]["runningPartition"], after["runtime"]["imageState"]);
        // SPIFFS also contains build-owned bootstrap packages and migration
        // markers. Their legitimate update can change total usage; compare
        // identities/configuration instead of treating byte equality as proof
        // of user-data preservation. This does not prove every saved value.
        let inventory_after = manager.query_widget_inventory(&expected_board_device_id).unwrap();
        assert_eq!(inventory_after["complete"], true);
        let ids = |v: &serde_json::Value| v["items"].as_array().unwrap().iter()
            .map(|i| i["id"].as_str().unwrap().to_owned()).collect::<HashSet<_>>();
        let before_ids=ids(&inventory_before);let after_ids=ids(&inventory_after);
        assert!(before_ids.is_subset(&after_ids),"OTA must preserve every installed component");
        let bundle:serde_json::Value=serde_json::from_str(include_str!("../../../firmware/factory-config.json")).unwrap();
        let bundled:HashSet<String>=bundle["components"]["ids"].as_array().unwrap().iter().map(|v|v.as_str().unwrap().to_owned()).collect();
        assert!(after_ids.difference(&before_ids).all(|id|bundled.contains(id)),"OTA may only add declared builtins");
        assert_eq!(buttons_before["config"], manager.query_button_config(&expected_board_device_id).unwrap()["config"]);
        assert_eq!(before["storage"]["activeAppearanceSlot"], after["storage"]["activeAppearanceSlot"]);
        manager.disconnect();
    }

    #[test]
    #[ignore = "Requires explicit permission: verify a committed OTA; reboot without erasing if still on old firmware"]
    fn p4_committed_ota_hardware_verify_or_reboot() {
        let port = std::env::var("P4_SERIAL_PORT").expect("explicit port");
        let expected = std::env::var("P4_EXPECTED_VERSION").expect("expected version");
        let board = std::env::var("P4_EXPECTED_BOARD_ID").expect("expected board");
        let manager = UsbSerialManager::new();
        manager.connect(&port, |_, _| {}).unwrap();
        assert_eq!(manager.status().board_device_id, board);
        let before = manager.query_diagnostics(&board).unwrap();
        eprintln!("OTA inspection: version={} partition={} state={} boot={}", before["runtime"]["firmware"], before["runtime"]["runningPartition"], before["runtime"]["imageState"], before["bootCount"]);
        if before["runtime"]["firmware"].as_str() != Some(expected.as_str()) {
            let reply = manager.reboot_device(&board).expect("preserving reboot");
            assert_eq!(reply["ok"], true);
            assert_eq!(reply["preservedAppearanceAssets"], true);
            std::thread::sleep(Duration::from_secs(3));
            manager.disconnect();
            let until = Instant::now() + Duration::from_secs(30);
            loop {
                if manager.connect(&port, |_, _| {}).is_ok() { break; }
                assert!(Instant::now() < until, "reconnect timeout");
                std::thread::sleep(Duration::from_millis(500));
            }
        }
        let until = Instant::now() + Duration::from_secs(40);
        loop {
            let after = manager.query_diagnostics(&board).unwrap();
            assert_eq!(after["runtime"]["firmware"].as_str(), Some(expected.as_str()), "expected committed firmware after reboot");
            if after["runtime"]["imageState"] == "valid" {
                assert_eq!(before["storage"]["usedBytes"], after["storage"]["usedBytes"]);
                assert_eq!(before["storage"]["activeAppearanceSlot"], after["storage"]["activeAppearanceSlot"]);
                eprintln!("OTA verified: version={} state=valid storagePreserved=true", expected);
                break;
            }
            assert!(Instant::now() < until, "validation timeout");
            std::thread::sleep(Duration::from_millis(500));
        }
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
        configure_transfer_logging(&app_data_dir).expect("configure hardware transfer log");
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
                false,
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
        assert!(widget_ota_should_skip_path("pc/extension.json"));
        assert!(widget_ota_should_skip_path("runtime/.keep"));
        assert!(!widget_ota_should_skip_path("runtime/widget.json"));
        assert!(!widget_ota_should_skip_path("assets/icon.png"));
    }

    #[test]
    fn p4_widget_sprite_sheet_compiles_to_rgb565_alpha_frames() {
        let temporary = tempfile::tempdir().unwrap();
        let assets = temporary.path().join("assets");
        std::fs::create_dir_all(&assets).unwrap();
        let mut sheet = image::RgbaImage::new(16, 8);
        for (x, _, pixel) in sheet.enumerate_pixels_mut() {
            *pixel = if x < 8 {
                image::Rgba([255, 0, 0, 128])
            } else {
                image::Rgba([0, 255, 0, 255])
            };
        }
        sheet.save(assets.join("hero.png")).unwrap();
        let widget = serde_json::to_vec(&serde_json::json!({
            "scene": {
                "sprites": [{
                    "id": "hero",
                    "asset": "assets/hero.png",
                    "frame_width": 8,
                    "frame_height": 8,
                    "frames": 2,
                    "fps": 6
                }]
            }
        }))
        .unwrap();

        let compiled = prepare_p4_widget_sprite_files(temporary.path(), &widget).unwrap();

        assert_eq!(compiled.len(), 1);
        assert_eq!(compiled[0].0, "runtime/sprites/hero.p4s");
        assert_eq!(&compiled[0].1[..8], b"P4S1\x08\x08\x02\x06");
        assert_eq!(compiled[0].1.len(), 8 + 16 * 8 * 3);
        assert_eq!(&compiled[0].1[8..11], &[0x00, 0xf8, 128]);
        let frame_bytes = 8 * 8 * 3;
        assert_eq!(
            &compiled[0].1[8 + frame_bytes - 3..8 + frame_bytes],
            &[0x00, 0xf8, 128],
        );
        assert_eq!(
            &compiled[0].1[8 + frame_bytes..8 + frame_bytes + 3],
            &[0xe0, 0x07, 255],
        );
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

/// The v3 profile covers v2 declarations; plural advertisements also explicitly list support.
fn widget_quota_supported(required: &str, capabilities: &serde_json::Value) -> bool {
    if !matches!(required, "p4-quota-v2" | "p4-quota-v3") { return false; }
    matches!((required, capabilities["widgetQuota"].as_str()),
        ("p4-quota-v2", Some("p4-quota-v2" | "p4-quota-v3")) |
        ("p4-quota-v3", Some("p4-quota-v3"))) ||
        capabilities["widgetQuotas"].as_array().is_some_and(|profiles|
            profiles.iter().any(|profile| profile.as_str() == Some(required)))
}

#[cfg(test)]
mod quota_negotiation_tests {
    use super::widget_quota_supported;
    use serde_json::json;
    #[test]
    fn installation_quota_negotiation_preserves_older_packages_and_refuses_unavailable_profiles() {
        let v2 = json!({"widgetQuota":"p4-quota-v2"});
        let v3 = json!({"widgetQuota":"p4-quota-v3"});
        assert!(widget_quota_supported("p4-quota-v2", &v2));
        assert!(widget_quota_supported("p4-quota-v2", &v3));
        assert!(widget_quota_supported("p4-quota-v3", &v3));
        assert!(!widget_quota_supported("p4-quota-v3", &v2));
        assert!(!widget_quota_supported("p4-quota-v2", &json!({})));
        assert!(!widget_quota_supported("arbitrary", &json!({"widgetQuotas":["arbitrary"]})));
        assert!(widget_quota_supported("p4-quota-v2", &json!({"widgetQuotas":["p4-quota-v2"]})));
    }
}

#[cfg(test)]mod appearance_audio_only_tests {
use super::*;
fn wav()->Vec<u8>{let mut b=b"RIFF".to_vec();b.extend_from_slice(&38u32.to_le_bytes());b.extend_from_slice(b"WAVEfmt ");b.extend_from_slice(&16u32.to_le_bytes());b.extend_from_slice(&1u16.to_le_bytes());b.extend_from_slice(&1u16.to_le_bytes());b.extend_from_slice(&16000u32.to_le_bytes());b.extend_from_slice(&32000u32.to_le_bytes());b.extend_from_slice(&2u16.to_le_bytes());b.extend_from_slice(&16u16.to_le_bytes());b.extend_from_slice(b"data");b.extend_from_slice(&2u32.to_le_bytes());b.extend_from_slice(&[0,0]);b}
fn audio(f:&str)->P4AudioPatchFile{P4AudioPatchFile{family:f.into(),path:format!("p4/audio/{f}.wav"),bytes:wav()}}
fn pack()->String{"a".repeat(64)}
#[test]fn builtin_uses_protected_current_default_identity(){assert_eq!(p4_audio_patch_target(true,true,Some(0),&pack(),Some(P4_AUDIO_PATCH_CONTRACT)).unwrap(),0);}
#[test]fn builtin_never_switches_or_writes_custom(){assert!(p4_audio_patch_target(true,true,Some(1),&pack(),Some(P4_AUDIO_PATCH_CONTRACT)).is_err());}
#[test]fn builtin_requires_protection(){assert!(p4_audio_patch_target(true,false,Some(0),&pack(),Some(P4_AUDIO_PATCH_CONTRACT)).is_err());}
#[test]fn custom_requires_current_custom_slot(){assert_eq!(p4_audio_patch_target(false,true,Some(1),&pack(),Some(P4_AUDIO_PATCH_CONTRACT)).unwrap(),1);assert!(p4_audio_patch_target(false,true,Some(0),&pack(),Some(P4_AUDIO_PATCH_CONTRACT)).is_err());}
#[test]fn missing_capability_fails_before_admission(){assert!(p4_audio_patch_target(true,true,Some(0),&pack(),None).unwrap_err().contains("升级固件"));}
#[test]fn invalid_pack_identity_refused(){for p in ["", "not-a-pack",&"A".repeat(64)]{assert!(p4_audio_patch_target(true,true,Some(0),p,Some(P4_AUDIO_PATCH_CONTRACT)).is_err());}}
#[test]fn actual_byte_set_is_wav_only(){let a=vec![audio("welcome"),audio("done")];assert_eq!(p4_audio_patch_byte_count(&a).unwrap(),92);}
#[test]fn video_and_manifest_never_enter_transfer_set(){let mut a=Vec::new();for(k,p)in[("p4-manifest","p4/manifest.json"),("p4-h264","p4/video/large.h264"),("p4-audio","p4/audio/welcome.wav")]{a.push(AppearanceAssetEntry{kind:k,device_path:p.into(),source_path:PathBuf::from("synthetic-only"),family_name:"welcome".into()});}let x=p4_audio_patch_transfer_assets(&a).unwrap();assert_eq!(x.len(),1);assert_eq!(x[0].device_path,"p4/audio/welcome.wav");}
#[test]fn disguised_video_path_and_duplicate_rejected(){let mut a=audio("done");a.path="p4/video/x.h264".into();assert!(p4_audio_patch_byte_count(&[a]).is_err());assert!(p4_audio_patch_byte_count(&[audio("done"),audio("done")]).is_err());}
#[test]fn malformed_or_wrong_profile_wav_rejected(){let mut a=audio("done");a.bytes[24..28].copy_from_slice(&48000u32.to_le_bytes());assert!(p4_audio_patch_byte_count(&[a]).is_err());let mut a=audio("done");a.bytes=vec![0;46];assert!(p4_audio_patch_byte_count(&[a]).is_err());}
#[test]fn empty_audio_set_is_explicit_default_restore_not_full(){assert_eq!(p4_audio_patch_byte_count(&[]).unwrap(),0);}
fn large_wav(data:usize)->Vec<u8>{let mut b=wav();b.truncate(44);b[4..8].copy_from_slice(&((data+36)as u32).to_le_bytes());b[40..44].copy_from_slice(&(data as u32).to_le_bytes());b.resize(44+data,0);b}
#[test]fn actual_upload_loop_stops_unknown_without_second_send(){let f=P4AudioPatchFile{family:"done".into(),path:"p4/audio/done.wav".into(),bytes:large_wav(9000)};let mut calls=0;let r=p4_audio_patch_send_files(&[f],|_,_,_,_|{calls+=1;if calls==1{Err("lost ACK unknown".into())}else{Ok(())}},&|_,_,_,_|{});assert!(r.is_err());assert_eq!(calls,1);}
#[test]fn actual_upload_loop_offsets_and_boundaries(){let f=P4AudioPatchFile{family:"done".into(),path:"p4/audio/done.wav".into(),bytes:large_wav(9000)};let mut seen=Vec::new();p4_audio_patch_send_files(&[f],|path,index,offset,chunk|{seen.push((path.to_owned(),index,offset,chunk.len()));Ok(())},&|_,_,_,_|{}).unwrap();assert_eq!(seen.iter().map(|(_,i,o,n)|(*i,*o,*n)).collect::<Vec<_>>(),vec![(0,0,4096),(1,4096,4096),(2,8192,852)]);assert!(seen.iter().all(|x|x.0=="p4/audio/done.wav"));}
#[test]fn file_and_total_budget_enforced_before_send(){let many=(0..25).map(|i|audio(&format!("f{i}"))).collect::<Vec<_>>();assert!(p4_audio_patch_byte_count(&many).is_err());let big=(0..3).map(|i|P4AudioPatchFile{family:format!("f{i}"),path:format!("p4/audio/f{i}.wav"),bytes:large_wav(800000)}).collect::<Vec<_>>();let mut called=false;assert!(p4_audio_patch_send_files(&big,|_,_,_,_|{called=true;Ok(())},&|_,_,_,_|{}).is_err());assert!(!called);}
#[test]fn malformed_riff_and_pcm_metadata_are_rejected_before_admission(){
    let mut bad=wav();bad[4..8].copy_from_slice(&0u32.to_le_bytes());
    assert!(validate_p4_audio_wav(&bad).is_ok(),"old validator counterexample must execute");
    assert!(validate_p4_audio_patch_wav(&bad).is_err());
    for (offset,value) in [(28,1234u32),(32,0x00100004u32)] {
        let mut bad=wav();bad[offset..offset+4].copy_from_slice(&value.to_le_bytes());
        assert!(validate_p4_audio_wav(&bad).is_ok());assert!(validate_p4_audio_patch_wav(&bad).is_err());
    }
    let good=wav();let before=good.clone();validate_p4_audio_patch_wav(&good).unwrap();assert_eq!(good,before);
}
#[test]fn trailing_bytes_and_data_before_format_are_rejected(){
    let mut bad=wav();bad.push(0);bad[4..8].copy_from_slice(&39u32.to_le_bytes());
    assert!(validate_p4_audio_wav(&bad).is_ok());assert!(validate_p4_audio_patch_wav(&bad).is_err());
    let good=wav();let mut bad=good[..12].to_vec();bad.extend_from_slice(&good[36..]);bad.extend_from_slice(&good[12..36]);
    assert!(validate_p4_audio_wav(&bad).is_ok());assert!(validate_p4_audio_patch_wav(&bad).is_err());
}
#[test]fn unknown_prepared_asset_is_not_silently_transferred(){let a=AppearanceAssetEntry{kind:"video",device_path:"foo.mp4".into(),family_name:"x".into(),source_path:PathBuf::new()};assert!(p4_audio_patch_transfer_assets(&[a]).is_err());}
}
