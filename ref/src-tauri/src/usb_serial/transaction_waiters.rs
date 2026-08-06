/*
 * [Input] Device ACK/response topics and their request-correlated JSON payloads.
 * [Output] One-shot waiter matching for appearance, widget, firmware, and device requests.
 * [Pos] Shared transaction-response routing boundary beneath usb_serial.rs.
 * [Sync] If this file changes, update `ref/.folder.md`.
 */

use serde_json::Value;
use std::sync::{mpsc, Arc, Mutex};

pub(super) struct AssetAckWaiter {
    pub(super) transfer_id: String,
    pub(super) phase: String,
    pub(super) path: Option<String>,
    pub(super) index: Option<u32>,
    pub(super) sender: mpsc::Sender<Value>,
}

pub(super) struct WidgetAckWaiter {
    pub(super) transfer_id: String,
    pub(super) phase: String,
    pub(super) path: Option<String>,
    pub(super) index: Option<u32>,
    pub(super) sender: mpsc::Sender<Value>,
}

pub(super) struct FirmwareAckWaiter {
    pub(super) transfer_id: String,
    pub(super) phase: String,
    pub(super) expected_next_sequence: u64,
    pub(super) expected_received_bytes: u64,
    pub(super) sender: mpsc::Sender<Value>,
}

pub(super) struct DeviceResponseWaiter {
    pub(super) request_id: String,
    pub(super) response_topic: String,
    pub(super) sender: mpsc::Sender<Value>,
}

pub(super) fn resolve_asset_ack(
    waiters: &Arc<Mutex<Vec<AssetAckWaiter>>>,
    topic: &str,
    payload: &Value,
) {
    if topic != "asset/ack" {
        return;
    }
    let transfer_id = payload
        .get("transferId")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let phase = payload
        .get("phase")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let path = payload.get("path").and_then(Value::as_str);
    let index = payload.get("index").and_then(|value| {
        value
            .as_u64()
            .and_then(|number| u32::try_from(number).ok())
            .or_else(|| value.as_str().and_then(|text| text.parse::<u32>().ok()))
    });
    if transfer_id.is_empty() || phase.is_empty() {
        return;
    }

    let sender = waiters.lock().ok().and_then(|mut waiters| {
        let matched_index = waiters.iter().position(|waiter| {
            waiter.transfer_id == transfer_id
                && waiter.phase == phase
                && waiter.path.as_deref() == path
                && (waiter.index == index || index.is_none())
        })?;
        Some(waiters.remove(matched_index).sender)
    });
    eprintln!(
        "[usb-appearance-ota] ack transfer_id={} phase={} path={} index={:?} matched={}",
        transfer_id,
        phase,
        path.unwrap_or(""),
        index,
        sender.is_some()
    );
    if let Some(sender) = sender {
        let _ = sender.send(payload.clone());
    }
}

pub(super) fn resolve_widget_ack(
    waiters: &Arc<Mutex<Vec<WidgetAckWaiter>>>,
    topic: &str,
    payload: &Value,
) {
    if topic != "widget-install-ack" {
        return;
    }
    let transfer_id = payload
        .get("transferId")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let phase = payload
        .get("phase")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let path = payload.get("path").and_then(Value::as_str);
    let index = payload.get("index").and_then(|value| {
        value
            .as_u64()
            .and_then(|number| u32::try_from(number).ok())
            .or_else(|| value.as_str().and_then(|text| text.parse::<u32>().ok()))
    });
    if transfer_id.is_empty() || phase.is_empty() {
        return;
    }
    let sender = waiters.lock().ok().and_then(|mut waiters| {
        let exact = waiters.iter().position(|waiter| {
            waiter.transfer_id == transfer_id
                && waiter.phase == phase
                && waiter.path.as_deref() == path
                && waiter.index == index
        });
        // Older firmware routes unsupported widget/delete through the widget
        // dispatcher and replies with a correlated phase="unknown" NACK.
        let legacy_delete_nack = (exact.is_none()
            && phase == "unknown"
            && payload.get("ok").and_then(Value::as_bool) == Some(false))
        .then(|| {
            waiters.iter().position(|waiter| {
                waiter.transfer_id == transfer_id
                    && waiter.phase == "delete"
                    && waiter.path.is_none()
                    && waiter.index.is_none()
            })
        })
        .flatten();
        let index = exact.or(legacy_delete_nack)?;
        Some(waiters.remove(index).sender)
    });
    eprintln!(
        "[widget-ota] ack transfer_id={} phase={} path={} index={:?} matched={}",
        transfer_id,
        phase,
        path.unwrap_or(""),
        index,
        sender.is_some()
    );
    if let Some(sender) = sender {
        let _ = sender.send(payload.clone());
    }
}

pub(super) fn resolve_firmware_ack(
    waiters: &Arc<Mutex<Vec<FirmwareAckWaiter>>>,
    topic: &str,
    payload: &Value,
) {
    if topic != "firmware/ack" {
        return;
    }
    let transfer_id = payload
        .get("transferId")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let phase = payload
        .get("phase")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let ok = payload.get("ok").and_then(Value::as_bool) == Some(true);
    let next_sequence = payload.get("nextSequence").and_then(Value::as_u64);
    let received_bytes = payload.get("receivedBytes").and_then(Value::as_u64);
    let chunk_sequence = payload.get("seq").and_then(Value::as_u64);
    if transfer_id.is_empty() || phase.is_empty() {
        return;
    }
    let sender = waiters.lock().ok().and_then(|mut waiters| {
        let index = waiters.iter().position(|waiter| {
            if waiter.transfer_id != transfer_id || waiter.phase != phase {
                return false;
            }
            if ok {
                return next_sequence == Some(waiter.expected_next_sequence)
                    && received_bytes == Some(waiter.expected_received_bytes);
            }
            phase != "chunk"
                || chunk_sequence == Some(waiter.expected_next_sequence.saturating_sub(1))
        })?;
        Some(waiters.remove(index).sender)
    });
    if phase != "chunk" || !ok || sender.is_none() {
        eprintln!(
            "[usb-firmware-ota] ack transfer_id={} phase={} next_sequence={} received_bytes={} matched={}",
            transfer_id,
            phase,
            next_sequence.unwrap_or_default(),
            received_bytes.unwrap_or_default(),
            sender.is_some()
        );
    }
    if let Some(sender) = sender {
        let _ = sender.send(payload.clone());
    }
}

pub(super) fn resolve_device_response(
    waiters: &Arc<Mutex<Vec<DeviceResponseWaiter>>>,
    topic: &str,
    payload: &Value,
) {
    let request_id = payload
        .get("requestId")
        .and_then(Value::as_str)
        .unwrap_or_default();
    if request_id.is_empty() {
        return;
    }
    let sender = waiters.lock().ok().and_then(|mut waiters| {
        let index = waiters
            .iter()
            .position(|waiter| waiter.request_id == request_id && waiter.response_topic == topic)?;
        Some(waiters.remove(index).sender)
    });
    if let Some(sender) = sender {
        let _ = sender.send(payload.clone());
    }
}
