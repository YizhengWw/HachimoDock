/*
 * [Input] Prepared appearance assets, remote stats/slots, and asset OTA errors.
 * [Output] Integrity digests, sync plans, slot fallback, ACK policy, and stable pack IDs.
 * [Pos] Pure appearance transaction contract beneath usb_serial.rs.
 * [Sync] If this file changes, update `ref/.folder.md`.
 */

use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::io::Read;
use std::path::PathBuf;

const FNV1A64_OFFSET: u64 = 0xcbf29ce484222325;
const FNV1A64_PRIME: u64 = 0x00000100000001b3;
pub(super) const P4_BUILTIN_APPEARANCE_SLOT: u32 = 0;
pub(super) const P4_RAW_APPEARANCE_SLOT: u32 = 1;

#[derive(Debug, Clone)]
pub(super) struct AppearanceAssetEntry {
    pub(super) family_name: String,
    pub(super) kind: &'static str,
    pub(super) source_path: PathBuf,
    pub(super) device_path: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct AppearanceAssetDigest {
    pub(super) kind: &'static str,
    pub(super) device_path: String,
    pub(super) size: u64,
    pub(super) checksum: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct AssetRemoteStat {
    pub(super) size: u64,
    pub(super) checksum: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct P4AppearanceSlot {
    pub(super) slot: u32,
    pub(super) pack_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct P4AppearanceSlotState {
    pub(super) active_slot: Option<u32>,
    pub(super) slots: Vec<P4AppearanceSlot>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum P4CachedPackActivation {
    NotFound,
    AlreadyActive,
    Activated,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum AppearanceSyncPlan {
    Full,
    AudioPatch(Vec<String>),
    Skip,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum AppearanceFullSyncMode {
    Verified,
    LegacyCommitOnly,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum AppearanceAssetAckPhase {
    Begin,
    Stat,
    File,
    Patch,
    Commit,
}

pub(super) fn asset_checksum_hex(bytes: &[u8]) -> String {
    let checksum = fnv1a64_update(FNV1A64_OFFSET, bytes);
    format!("{checksum:016x}")
}

fn fnv1a64_update(mut checksum: u64, bytes: &[u8]) -> u64 {
    for byte in bytes {
        checksum ^= u64::from(*byte);
        checksum = checksum.wrapping_mul(FNV1A64_PRIME);
    }
    checksum
}

pub(super) fn digest_appearance_assets(
    assets: &[AppearanceAssetEntry],
) -> Result<Vec<AppearanceAssetDigest>, String> {
    assets.iter().map(digest_appearance_asset).collect()
}

fn digest_appearance_asset(asset: &AppearanceAssetEntry) -> Result<AppearanceAssetDigest, String> {
    let bytes = std::fs::read(&asset.source_path).map_err(|error| {
        let label = if asset.kind == "audio" {
            "读取音效失败"
        } else {
            "读取视频失败"
        };
        format!("{} {}: {}", label, asset.family_name, error)
    })?;
    Ok(AppearanceAssetDigest {
        kind: asset.kind,
        device_path: asset.device_path.clone(),
        size: bytes.len() as u64,
        checksum: asset_checksum_hex(&bytes),
    })
}

pub(super) fn plan_appearance_sync_from_digests(
    local: &[AppearanceAssetDigest],
    remote: &HashMap<String, AssetRemoteStat>,
    remote_has_removed_audio: bool,
) -> AppearanceSyncPlan {
    for asset in local.iter().filter(|asset| asset.kind != "audio") {
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

pub(super) fn build_asset_file_commit_payload(
    transfer_id: &str,
    path: &str,
    size: u64,
    checksum: &str,
    chunk_count: u64,
) -> Value {
    serde_json::json!({
        "transferId": transfer_id,
        "path": path,
        "size": size,
        "checksum": checksum,
        "chunkCount": chunk_count,
    })
}

pub(super) fn parse_p4_appearance_slot_state(
    payload: &Value,
) -> Result<P4AppearanceSlotState, String> {
    let active_slot = payload
        .get("activeSlot")
        .and_then(Value::as_u64)
        .and_then(|value| u32::try_from(value).ok());
    let slots = payload
        .get("slots")
        .and_then(Value::as_array)
        .ok_or_else(|| "P4 slot query response does not contain slots".to_string())?
        .iter()
        .filter(|slot| slot.get("valid").and_then(Value::as_bool).unwrap_or(true))
        .filter_map(|slot| {
            let slot_index = slot
                .get("slot")
                .and_then(Value::as_u64)
                .and_then(|value| u32::try_from(value).ok())?;
            let pack_id = slot
                .get("packId")
                .and_then(Value::as_str)
                .filter(|value| !value.is_empty())?;
            Some(P4AppearanceSlot {
                slot: slot_index,
                pack_id: pack_id.to_string(),
            })
        })
        .collect();
    Ok(P4AppearanceSlotState { active_slot, slots })
}

pub(super) fn p4_raw_transfer_fallback_slot(
    state: &P4AppearanceSlotState,
) -> Option<&P4AppearanceSlot> {
    if state.active_slot != Some(P4_RAW_APPEARANCE_SLOT) {
        return None;
    }
    state
        .slots
        .iter()
        .find(|slot| slot.slot == P4_BUILTIN_APPEARANCE_SLOT)
}

pub(super) fn parse_missing_asset_ack_phase(error: &str) -> Option<AppearanceAssetAckPhase> {
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

pub(super) fn should_retry_appearance_with_legacy_full_sync(error: &str) -> bool {
    matches!(
        parse_missing_asset_ack_phase(error),
        Some(
            AppearanceAssetAckPhase::Stat
                | AppearanceAssetAckPhase::File
                | AppearanceAssetAckPhase::Patch
        )
    )
}

pub(super) fn compute_p4_pack_id(
    assets: &[AppearanceAssetEntry],
    manifest_identity: &[u8],
) -> Result<String, String> {
    let mut ordered = assets.iter().collect::<Vec<_>>();
    ordered.sort_by(|left, right| left.device_path.cmp(&right.device_path));
    let mut digest = Sha256::new();
    digest.update(b"pet-manager-p4-pack-v1\0");
    digest.update((manifest_identity.len() as u64).to_le_bytes());
    digest.update(manifest_identity);
    for asset in ordered {
        let path = asset.device_path.as_bytes();
        digest.update((path.len() as u64).to_le_bytes());
        digest.update(path);
        let mut file = std::fs::File::open(&asset.source_path).map_err(|error| {
            format!(
                "open P4 pack asset failed {}: {}",
                asset.source_path.display(),
                error
            )
        })?;
        let size = file
            .metadata()
            .map_err(|error| {
                format!(
                    "stat P4 pack asset failed {}: {}",
                    asset.source_path.display(),
                    error
                )
            })?
            .len();
        digest.update(size.to_le_bytes());
        let mut buffer = [0u8; 64 * 1024];
        loop {
            let read = file.read(&mut buffer).map_err(|error| {
                format!(
                    "read P4 pack asset failed {}: {}",
                    asset.source_path.display(),
                    error
                )
            })?;
            if read == 0 {
                break;
            }
            digest.update(&buffer[..read]);
        }
    }
    Ok(format!("{:x}", digest.finalize()))
}

pub(super) fn p4_pack_id_from_assets(assets: &[AppearanceAssetEntry]) -> Result<String, String> {
    let manifest = assets
        .iter()
        .find(|asset| asset.device_path == "p4/manifest.json")
        .ok_or_else(|| "P4 pack manifest is missing".to_string())?;
    let value: Value = serde_json::from_slice(
        &std::fs::read(&manifest.source_path)
            .map_err(|error| format!("read P4 pack manifest failed: {error}"))?,
    )
    .map_err(|error| format!("parse P4 pack manifest failed: {error}"))?;
    value
        .get("packId")
        .and_then(Value::as_str)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
        .ok_or_else(|| "P4 pack manifest does not contain packId".to_string())
}
