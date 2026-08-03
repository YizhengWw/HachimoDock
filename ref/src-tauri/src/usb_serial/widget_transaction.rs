/*
 * [Input] Widget package paths/JSON bytes, device capabilities, and chunk metadata.
 * [Output] P4-safe widget payloads, capability gates, path checks, and OTA timing policy.
 * [Pos] Pure component/widget transaction contract beneath usb_serial.rs.
 * [Sync] If this file changes, update `ref/.folder.md`.
 */

use super::UsbConnectionStatus;
use crate::clawpkg::validate_p4_vars_object;
use serde_json::Value;
use std::path::Path;
use std::time::Duration;

pub(super) const WIDGET_BEGIN_ACK_TIMEOUT: Duration = Duration::from_secs(2);
pub(super) const WIDGET_CHUNK_ACK_TIMEOUT: Duration = Duration::from_secs(2);
// P4 may spend several seconds syncing its flash-backed widget catalog and
// refreshing the active mini-app before the final acknowledgement reaches the host.
pub(super) const WIDGET_COMMIT_ACK_TIMEOUT: Duration = Duration::from_secs(15);
pub(super) const WIDGET_DELETE_ACK_TIMEOUT: Duration = Duration::from_secs(15);
pub(super) const WIDGET_CHUNK_MAX_ATTEMPTS: usize = 3;
pub(super) const P4_WIDGET_JSON_MAX_BYTES: usize = 4095;
const P4_BUTTONS_JSON_MAX_BYTES: usize = 2047;
const WIDGET_DELETE_CAPABILITY: &str = "widgetDelete";
const WIDGET_INVENTORY_CAPABILITY: &str = "widgetInventory";

pub(super) fn build_widget_chunk_payload(
    transfer_id: &str,
    relative_path: &str,
    data_base64: &str,
    index: u32,
    decoded_size: usize,
    checksum: &str,
) -> Value {
    serde_json::json!({
        "transferId": transfer_id,
        "path": relative_path,
        "data": data_base64,
        "index": index.to_string(),
        "decodedSize": decoded_size,
        "checksum": checksum,
    })
}

pub(super) fn format_widget_ack_timeout(transfer_id: &str, phase: &str) -> String {
    format!(
        "未收到板端组件 OTA 确认: transferId={} phase={}",
        transfer_id, phase
    )
}

pub(super) fn ensure_widget_delete_supported(status: &UsbConnectionStatus) -> Result<(), String> {
    ensure_p4_widget_capability(status, WIDGET_DELETE_CAPABILITY, "组件删除")
}

pub(super) fn ensure_widget_inventory_supported(
    status: &UsbConnectionStatus,
) -> Result<(), String> {
    ensure_p4_widget_capability(status, WIDGET_INVENTORY_CAPABILITY, "组件清单")
}

fn ensure_p4_widget_capability(
    status: &UsbConnectionStatus,
    capability: &str,
    capability_label: &str,
) -> Result<(), String> {
    if !status.runtime.eq_ignore_ascii_case("esp-p4") {
        return Ok(());
    }
    let advertised = status
        .capabilities
        .get(capability)
        .and_then(Value::as_bool)
        .or_else(|| {
            status
                .capabilities
                .get("features")
                .and_then(|features| features.get(capability))
                .and_then(Value::as_bool)
        });
    if advertised == Some(true) {
        return Ok(());
    }
    Err(format!(
        "当前 ESP32-P4 板端固件 {} 未声明{}能力；请先在设备操作中升级板端固件并重新连接，再重试。",
        if status.firmware.trim().is_empty() {
            "<未知版本>"
        } else {
            status.firmware.trim()
        },
        capability_label,
    ))
}

pub(super) fn widget_ota_relative_path(root: &Path, path: &Path) -> Result<String, String> {
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

pub(super) fn widget_ota_should_skip_path(path: &str) -> bool {
    path.split('/').next_back() == Some(".keep")
}

pub(super) fn prepare_p4_widget_file(
    widget_id: &str,
    path: &str,
    bytes: &[u8],
) -> Result<Vec<u8>, String> {
    let max_bytes = match path {
        "runtime/widget.json" => P4_WIDGET_JSON_MAX_BYTES,
        "buttons.json" => P4_BUTTONS_JSON_MAX_BYTES,
        _ => return Ok(bytes.to_vec()),
    };
    let mut value: Value = serde_json::from_slice(bytes)
        .map_err(|error| format!("{} is invalid JSON: {}", path, error))?;

    if path == "runtime/widget.json" {
        let object = value
            .as_object_mut()
            .ok_or_else(|| "runtime/widget.json 必须是 JSON 对象，无法下发到 P4".to_string())?;

        // Missing vars and an empty legacy array both mean no variables.
        let legacy_empty_vars = match object.get("vars") {
            None => true,
            Some(vars) if vars.is_object() => false,
            Some(vars) => vars.as_array().is_some_and(Vec::is_empty),
        };
        if legacy_empty_vars {
            object.insert("vars".to_string(), serde_json::json!({}));
        } else if !object.get("vars").is_some_and(Value::is_object) {
            return Err(
                "runtime/widget.json 的 vars 必须是以变量名为键的 JSON 对象；无变量时请使用 {}"
                    .to_string(),
            );
        }
        validate_p4_vars_object(
            object
                .get("vars")
                .and_then(Value::as_object)
                .expect("vars was normalized and checked as an object"),
        )?;

        if let Some(pages) = value.get_mut("pages").and_then(Value::as_array_mut) {
            for page in pages {
                if let Some(page) = page.as_object_mut() {
                    page.remove("label");
                }
            }
        }
    }

    if widget_id == "token-usage" && path == "runtime/widget.json" {
        let object = value
            .as_object_mut()
            .ok_or_else(|| "token-usage runtime/widget.json must be an object".to_string())?;
        object.remove("readers");
        object.remove("fetchers");
    }

    let compact = serde_json::to_vec(&value).map_err(|error| error.to_string())?;
    if compact.len() > max_bytes {
        return Err(format!(
            "{} is {} bytes after JSON compaction; P4 limit is {} bytes",
            path,
            compact.len(),
            max_bytes
        ));
    }
    Ok(compact)
}
