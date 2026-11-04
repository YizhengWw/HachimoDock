/*
 * [Input] ESP-IDF application images plus post-reboot P4 diagnostics.
 * [Output] Firmware image metadata/preflight, capability-sized adaptive Base64
 *          wire chunks, OTA limits/errors, and validated reboot results.
 * [Pos] Pure firmware transaction contract beneath usb_serial.rs.
 * [Sync] If this file changes, update `pc/.folder.md`.
 */

use serde::Serialize;
use serde_json::Value;
use std::time::Duration;

// Protocol-schema-6 firmware fixes the OTA idle-clock race and advertises a
// decoded 4 KiB receiver with a 32 KiB JSON line buffer. Keep three bytes below
// that decoded ceiling so full Base64 chunks have no padding. Schema-5 boards
// have the same receiver budget but may spuriously clear a live transaction;
// the desktop stops rather than guessing whether that transaction survived.
// Older receivers retain the sub-4-KiB wire line below. Every size is drained
// through short physical serial writes.
pub(super) const P4_FIRMWARE_FAST_CHUNK_SIZE: usize = 4_092;
pub(super) const P4_FIRMWARE_CHUNK_SIZE: usize = 2_046;
pub(super) const P4_FIRMWARE_FALLBACK_CHUNK_SIZE: usize = 1_020;
pub(super) const P4_FIRMWARE_SAFE_CHUNK_SIZE: usize = 510;
pub(super) const P4_FIRMWARE_FAST_PROTOCOL_SCHEMA: u32 = 6;
pub(super) const P4_FIRMWARE_IDLE_CLOCK_BUG_SCHEMA: u32 = 5;
pub(super) const P4_FIRMWARE_CORRUPTION_RETRIES_BEFORE_FALLBACK: usize = 3;
pub(super) const P4_FIRMWARE_RECOVERY_SUCCESS_STREAK: usize = 32;
pub(super) const P4_FIRMWARE_MAX_IMAGE_SIZE: usize = 0x300000;
pub(super) const P4_FIRMWARE_ACK_TIMEOUT: Duration = Duration::from_secs(5);
// A healthy 4 Mbaud board ACK arrives within a few milliseconds. A longer
// timeout only lets a lost/corrupted ACK consume the old firmware's bounded
// whole-transfer window before the duplicate chunk can be retried.
pub(super) const P4_FIRMWARE_CHUNK_ACK_TIMEOUT: Duration = Duration::from_millis(150);
pub(super) const P4_FIRMWARE_COMMIT_ACK_TIMEOUT: Duration = Duration::from_secs(3);
pub(super) const P4_FIRMWARE_RECONNECT_TIMEOUT: Duration = Duration::from_secs(90);
pub(super) const P4_FIRMWARE_CHUNK_MAX_ATTEMPTS: usize = 20;
pub(super) const P4_FIRMWARE_PROJECT_NAME: &str = "pet_manager_p4_runtime";
pub(super) const ESP_IMAGE_HEADER_SIZE: usize = 24;
pub(super) const ESP_IMAGE_SEGMENT_HEADER_SIZE: usize = 8;
pub(super) const ESP_APP_DESC_SIZE: usize = 256;
pub(super) const ESP_APP_DESC_MAGIC: u32 = 0xABCD_5432;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FirmwareUpdateResult {
    pub transfer_id: String,
    pub bytes: u64,
    pub sha256: String,
    pub target_partition: String,
    pub version: String,
    pub project_name: String,
    pub image_state: String,
    pub pending_reboot: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FirmwareImageInfo {
    pub version: String,
    pub project_name: String,
    pub bytes: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct EspIdfAppDescriptor {
    pub(super) version: String,
    pub(super) project_name: String,
}

#[derive(Debug)]
pub(super) enum FirmwareCommandError {
    Send(String),
    Timeout { phase: String },
    Rejected(String),
}

// A desktop image ceiling never enlarges an existing device's physical slot.
// Check its current table before any stale-transfer abort or begin/erase.
pub(super) fn ensure_device_firmware_headroom(status:&Value,image_bytes:usize)->Result<(),String> {
    const MIN_HEADROOM:u64=131_072;
    let size=status.pointer("/nextPartition/size").and_then(Value::as_u64)
        .filter(|v|*v>0 && *v<=P4_FIRMWARE_MAX_IMAGE_SIZE as u64 && *v%4096==0)
        .ok_or("无法确认设备实际 OTA 分区容量；未开始更新，也未清除任何传输")?;
    let next=status.pointer("/nextPartition/label").and_then(Value::as_str);
    let running=status.pointer("/runningPartition/label").and_then(Value::as_str);
    if status["supported"]!=true || !matches!(next,Some("ota_0"|"ota_1")) || !matches!(running,Some("ota_0"|"ota_1")) || next==running {
        return Err("设备未提供独立的 OTA 更新槽，未开始更新".into());
    }
    if image_bytes==0 || (image_bytes as u64).checked_add(MIN_HEADROOM).is_none_or(|need|need>size) {
        return Err("设备当前 OTA 分区余量不足，须保留至少 128 KiB。普通 OTA 不会扩大分区；如需调整，请使用完整 USB 初始化流程（会清空设备资源）。此次未开始更新。".into());
    }
    Ok(())
}
#[cfg(test)]mod capacity_tests {
    use super::*;use serde_json::json;
    fn status(size:u64)->Value {json!({"supported":true,"runningPartition":{"label":"ota_0"},"nextPartition":{"label":"ota_1","size":size}})}
    #[test]fn actual_old_and_new_slots_keep_the_same_headroom_boundary(){
        for size in [0x280000,0x300000] {assert!(ensure_device_firmware_headroom(&status(size),size as usize-131072).is_ok());assert!(ensure_device_firmware_headroom(&status(size),size as usize-131071).is_err());}
        assert!(ensure_device_firmware_headroom(&status(0x280000),0x290000).is_err());
        assert!(ensure_device_firmware_headroom(&status(0x300000),0x290000).is_ok());
    }
    #[test]fn missing_coerced_oversize_and_same_slot_fail_closed(){
        for size in [Value::Null,json!("3145728"),json!(-1),json!(3145728.5),json!(0x400000)] {let mut s=status(0x300000);s["nextPartition"]["size"]=size;assert!(ensure_device_firmware_headroom(&s,1024).is_err());}
        let mut s=status(0x300000);s["nextPartition"]["label"]="ota_0".into();assert!(ensure_device_firmware_headroom(&s,1024).is_err());
        assert!(ensure_device_firmware_headroom(&status(0x300000),0).is_err());
    }
}

// A free-text rejection is not proof that the requested bytes were un-applied.
// The current firmware emits this exact shape before esp_ota_write and before
// advancing its progress. Missing/contradictory fields must remain Unknown.
pub(super) fn firmware_base64_rejection_unapplied(
    ack: &Value, request: &Value, expected_next: u64, expected_bytes: u64,
) -> bool {
    let Some(sequence) = expected_next.checked_sub(1) else { return false; };
    let Some(size) = request["decodedSize"].as_u64().filter(|v| *v > 0) else { return false; };
    let Some(offset) = expected_bytes.checked_sub(size) else { return false; };
    ack["ok"].as_bool() == Some(false)
        && ack["error"].as_str() == Some("firmware chunk base64 mismatch")
        && ack["phase"] == "chunk"
        && request["transferId"].as_str().is_some_and(|id| !id.is_empty() && ack["transferId"].as_str() == Some(id))
        && request["seq"].as_u64() == Some(sequence)
        && ack["seq"].as_u64() == Some(sequence)
        && ack["nextSequence"].as_u64() == Some(sequence)
        && ack["receivedBytes"].as_u64() == Some(offset)
        && ack["chunkBytes"].as_u64() == Some(0)
        && ack["duplicate"].as_bool() == Some(false)
}

pub(super) fn firmware_corruption_fallback_size(
    current_size: usize,
    error: &FirmwareCommandError,
) -> Option<usize> {
    if !matches!(
        error,
        FirmwareCommandError::Rejected(message)
            if message == "firmware chunk base64 mismatch"
    ) {
        return None;
    }
    if current_size > P4_FIRMWARE_CHUNK_SIZE {
        Some(P4_FIRMWARE_CHUNK_SIZE)
    } else if current_size > P4_FIRMWARE_FALLBACK_CHUNK_SIZE {
        Some(P4_FIRMWARE_FALLBACK_CHUNK_SIZE)
    } else if current_size > P4_FIRMWARE_SAFE_CHUNK_SIZE {
        Some(P4_FIRMWARE_SAFE_CHUNK_SIZE)
    } else {
        None
    }
}

pub(super) fn preferred_firmware_chunk_size(protocol_schema: u32, capabilities: &Value) -> usize {
    let advertised_max = capabilities
        .pointer("/firmwareUpdate/chunkBytes")
        .and_then(Value::as_u64)
        .and_then(|value| usize::try_from(value).ok())
        .unwrap_or_default();
    if protocol_schema >= P4_FIRMWARE_FAST_PROTOCOL_SCHEMA
        && advertised_max >= P4_FIRMWARE_FAST_CHUNK_SIZE
    {
        P4_FIRMWARE_FAST_CHUNK_SIZE
    } else if protocol_schema == P4_FIRMWARE_IDLE_CLOCK_BUG_SCHEMA
        && advertised_max >= P4_FIRMWARE_FAST_CHUNK_SIZE
    {
        P4_FIRMWARE_FAST_CHUNK_SIZE
    } else {
        P4_FIRMWARE_CHUNK_SIZE
    }
}

pub(super) fn initial_firmware_chunk_size(protocol_schema: u32, preferred_size: usize) -> usize {
    if protocol_schema == P4_FIRMWARE_IDLE_CLOCK_BUG_SCHEMA {
        preferred_size.min(P4_FIRMWARE_SAFE_CHUNK_SIZE)
    } else {
        preferred_size
    }
}

pub(super) fn firmware_chunk_error_is_retryable(error: &FirmwareCommandError) -> bool {
    match error {
        // Timeout is an unknown write outcome, never permission to replay.
        FirmwareCommandError::Timeout { .. } => false,
        FirmwareCommandError::Rejected(message) => {
            message == "firmware chunk base64 mismatch"
        }
        FirmwareCommandError::Send(_) => false,
    }
}

pub(super) fn firmware_recovery_chunk_size(
    current_size: usize,
    preferred_size: usize,
    successful_chunks: usize,
) -> Option<usize> {
    if current_size >= preferred_size || successful_chunks < P4_FIRMWARE_RECOVERY_SUCCESS_STREAK {
        return None;
    }
    let next_size = if current_size < P4_FIRMWARE_FALLBACK_CHUNK_SIZE {
        P4_FIRMWARE_FALLBACK_CHUNK_SIZE
    } else if current_size < P4_FIRMWARE_CHUNK_SIZE {
        P4_FIRMWARE_CHUNK_SIZE
    } else {
        P4_FIRMWARE_FAST_CHUNK_SIZE
    };
    Some(next_size.min(preferred_size))
}

impl std::fmt::Display for FirmwareCommandError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Send(error) | Self::Rejected(error) => formatter.write_str(error),
            Self::Timeout { phase } => {
                write!(
                    formatter,
                    "firmware OTA acknowledgement timed out at {phase}"
                )
            }
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct VerifiedFirmware {
    pub(super) partition: String,
    pub(super) image_state: String,
}

fn parse_descriptor_string(bytes: &[u8], field_name: &str) -> Result<String, String> {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(bytes.len());
    let value = std::str::from_utf8(&bytes[..end])
        .map_err(|_| format!("ESP-IDF app descriptor {field_name} is not valid UTF-8"))?
        .trim();
    if value.is_empty() {
        return Err(format!(
            "ESP-IDF app descriptor {field_name} must not be empty"
        ));
    }
    Ok(value.to_string())
}

pub(super) fn parse_esp_idf_app_descriptor(firmware: &[u8]) -> Result<EspIdfAppDescriptor, String> {
    let descriptor_offset = ESP_IMAGE_HEADER_SIZE + ESP_IMAGE_SEGMENT_HEADER_SIZE;
    let descriptor_end = descriptor_offset + ESP_APP_DESC_SIZE;
    if firmware.len() < descriptor_end {
        return Err("selected file is too small to contain an ESP-IDF app descriptor".to_string());
    }
    if firmware[0] != 0xE9 {
        return Err("selected file is not an ESP-IDF application image".to_string());
    }

    let first_segment_size = u32::from_le_bytes(
        firmware[ESP_IMAGE_HEADER_SIZE + 4..ESP_IMAGE_HEADER_SIZE + 8]
            .try_into()
            .map_err(|_| "ESP-IDF first segment header is truncated".to_string())?,
    ) as usize;
    if first_segment_size < ESP_APP_DESC_SIZE
        || descriptor_offset
            .checked_add(first_segment_size)
            .is_none_or(|end| end > firmware.len())
    {
        return Err("ESP-IDF first segment does not contain a complete app descriptor".to_string());
    }

    let magic = u32::from_le_bytes(
        firmware[descriptor_offset..descriptor_offset + 4]
            .try_into()
            .map_err(|_| "ESP-IDF app descriptor is truncated".to_string())?,
    );
    if magic != ESP_APP_DESC_MAGIC {
        return Err("ESP-IDF app descriptor magic is invalid".to_string());
    }

    let version = parse_descriptor_string(
        &firmware[descriptor_offset + 16..descriptor_offset + 48],
        "version",
    )?;
    let project_name = parse_descriptor_string(
        &firmware[descriptor_offset + 48..descriptor_offset + 80],
        "projectName",
    )?;
    if project_name != P4_FIRMWARE_PROJECT_NAME {
        return Err(format!(
            "firmware projectName must be {P4_FIRMWARE_PROJECT_NAME}, got {project_name}"
        ));
    }

    Ok(EspIdfAppDescriptor {
        version,
        project_name,
    })
}

pub fn inspect_firmware_image(path: &std::path::Path) -> Result<FirmwareImageInfo, String> {
    let firmware = std::fs::read(path)
        .map_err(|error| format!("read firmware {}: {error}", path.display()))?;
    if firmware.is_empty() || firmware.len() > P4_FIRMWARE_MAX_IMAGE_SIZE {
        return Err(format!(
            "firmware image must be 1..={} bytes, got {}",
            P4_FIRMWARE_MAX_IMAGE_SIZE,
            firmware.len()
        ));
    }
    let descriptor = parse_esp_idf_app_descriptor(&firmware)?;
    Ok(FirmwareImageInfo {
        version: descriptor.version,
        project_name: descriptor.project_name,
        bytes: firmware.len() as u64,
    })
}

pub(super) fn evaluate_firmware_validation(
    diagnostics: &Value,
    expected_version: &str,
    expected_partition: &str,
    original_version: &str,
    original_partition: &str,
    baseline_boot_count: u64,
) -> Result<Option<VerifiedFirmware>, String> {
    let boot_count = diagnostics
        .get("bootCount")
        .and_then(Value::as_u64)
        .ok_or("reconnected diagnostics did not include bootCount")?;
    if boot_count <= baseline_boot_count {
        return Ok(None);
    }

    let runtime = diagnostics
        .get("runtime")
        .and_then(Value::as_object)
        .ok_or("reconnected diagnostics did not include runtime")?;
    let version = runtime
        .get("firmware")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let partition = runtime
        .get("runningPartition")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let image_state = runtime
        .get("imageState")
        .and_then(Value::as_str)
        .unwrap_or_default();

    if version != expected_version {
        if version == original_version {
            return Err(format!(
                "firmware update rolled back: expected version {expected_version}, device restarted on old version {version}"
            ));
        }
        return Err(format!(
            "firmware update restarted into unexpected version {version}; expected {expected_version}"
        ));
    }
    if partition != expected_partition {
        if partition == original_partition {
            return Err(format!(
                "firmware update rolled back: expected partition {expected_partition}, device restarted on previous partition {partition}"
            ));
        }
        return Err(format!(
            "firmware update restarted into unexpected partition {partition}; expected {expected_partition}"
        ));
    }

    match image_state {
        "valid" => Ok(Some(VerifiedFirmware {
            partition: partition.to_string(),
            image_state: image_state.to_string(),
        })),
        "pending_verify" | "new" => Ok(None),
        "" => Err("reconnected diagnostics did not include runtime.imageState".to_string()),
        other => Err(format!(
            "firmware {expected_version} restarted with invalid imageState={other}"
        )),
    }
}

#[cfg(test)]
mod ack_rejection_tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn only_complete_unadvanced_base64_rejection_proves_not_applied() {
        let request=json!({"transferId":"tx","seq":2,"decodedSize":16});
        let ack=json!({"transferId":"tx","phase":"chunk","ok":false,"error":"firmware chunk base64 mismatch",
            "seq":2,"nextSequence":2,"receivedBytes":32,"chunkBytes":0,"duplicate":false});
        assert!(firmware_base64_rejection_unapplied(&ack,&request,3,48));
        for (key,value) in [("ok",json!(null)),("ok",json!("false")),("seq",json!(1)),
            ("nextSequence",json!(3)),("receivedBytes",json!(48)),("chunkBytes",json!(16)),
            ("duplicate",json!(true)),("duplicate",json!(null)),("transferId",json!("other")),
            ("error",json!("prefix firmware chunk base64 mismatch")),("phase",json!("commit"))] {
            let mut bad=ack.clone();bad[key]=value;
            assert!(!firmware_base64_rejection_unapplied(&bad,&request,3,48),"{key}: {bad}");
        }
        for key in ["ok","seq","nextSequence","receivedBytes","chunkBytes","duplicate","transferId","phase","error"] {
            let mut bad=ack.clone();bad.as_object_mut().unwrap().remove(key);
            assert!(!firmware_base64_rejection_unapplied(&bad,&request,3,48),"missing {key}");
        }
        assert!(!firmware_base64_rejection_unapplied(&ack,&request,0,48));
        assert!(!firmware_base64_rejection_unapplied(&ack,&request,3,15));
        let near=FirmwareCommandError::Rejected("prefix firmware chunk base64 mismatch".into());
        assert!(!firmware_chunk_error_is_retryable(&near));
        assert!(firmware_corruption_fallback_size(P4_FIRMWARE_FAST_CHUNK_SIZE,&near).is_none());
    }
}
