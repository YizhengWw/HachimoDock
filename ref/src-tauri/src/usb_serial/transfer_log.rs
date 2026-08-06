/*
 * [Input] Structured USB appearance and firmware transfer lifecycle/ack events.
 * [Output] Bounded JSONL diagnostics persisted under the current app-data directory.
 * [Pos] Release-safe USB transfer observability helper for usb_serial.
 * [Sync] If this file changes, update `ref/.folder.md`.
 */

use serde_json::Value;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};

const TRANSFER_LOG_DIRECTORY: &str = "logs";
const TRANSFER_LOG_FILE: &str = "usb-transfer.jsonl";
const TRANSFER_LOG_BACKUP_FILE: &str = "usb-transfer.previous.jsonl";
const TRANSFER_LOG_MAX_BYTES: u64 = 4 * 1024 * 1024;

struct TransferLogState {
    path: PathBuf,
    write_guard: Mutex<()>,
}

static TRANSFER_LOG_STATE: OnceLock<TransferLogState> = OnceLock::new();

pub fn configure(app_data_dir: &Path) -> Result<PathBuf, String> {
    let log_dir = app_data_dir.join(TRANSFER_LOG_DIRECTORY);
    fs::create_dir_all(&log_dir).map_err(|error| {
        format!(
            "create USB transfer log directory failed {}: {error}",
            log_dir.display()
        )
    })?;
    let path = log_dir.join(TRANSFER_LOG_FILE);
    let configured = TRANSFER_LOG_STATE.get_or_init(|| TransferLogState {
        path: path.clone(),
        write_guard: Mutex::new(()),
    });
    record(
        "logger",
        "configured",
        serde_json::json!({ "path": configured.path }),
    );
    Ok(configured.path.clone())
}

pub fn record(scope: &str, event: &str, details: Value) {
    let Some(state) = TRANSFER_LOG_STATE.get() else {
        return;
    };
    let Ok(_guard) = state.write_guard.lock() else {
        return;
    };
    if let Err(error) = append_record(&state.path, scope, event, details) {
        eprintln!("[usb-transfer-log] append failed: {error}");
    }
}

fn append_record(path: &Path, scope: &str, event: &str, details: Value) -> Result<(), String> {
    rotate_if_needed(path)?;
    let timestamp_ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis();
    let mut line = serde_json::to_vec(&serde_json::json!({
        "timestampMs": timestamp_ms,
        "scope": scope,
        "event": event,
        "details": details,
    }))
    .map_err(|error| format!("encode USB transfer log failed: {error}"))?;
    line.push(b'\n');
    let mut file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .map_err(|error| format!("open USB transfer log failed {}: {error}", path.display()))?;
    file.write_all(&line)
        .map_err(|error| format!("write USB transfer log failed {}: {error}", path.display()))
}

fn rotate_if_needed(path: &Path) -> Result<(), String> {
    let Ok(metadata) = fs::metadata(path) else {
        return Ok(());
    };
    if metadata.len() < TRANSFER_LOG_MAX_BYTES {
        return Ok(());
    }
    let backup = path.with_file_name(TRANSFER_LOG_BACKUP_FILE);
    if backup.exists() {
        fs::remove_file(&backup).map_err(|error| {
            format!(
                "remove previous USB transfer log failed {}: {error}",
                backup.display()
            )
        })?;
    }
    fs::rename(path, &backup).map_err(|error| {
        format!(
            "rotate USB transfer log failed {} -> {}: {error}",
            path.display(),
            backup.display()
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn appends_structured_jsonl_record() {
        let temp = tempfile::tempdir().expect("tempdir");
        let path = temp.path().join("transfer.jsonl");
        append_record(
            &path,
            "appearance",
            "raw_chunk_ack",
            serde_json::json!({ "index": 2, "ok": true }),
        )
        .expect("append record");
        let text = fs::read_to_string(path).expect("read record");
        let value: Value = serde_json::from_str(text.trim()).expect("valid json");
        assert_eq!(value["scope"], "appearance");
        assert_eq!(value["event"], "raw_chunk_ack");
        assert_eq!(value["details"]["index"], 2);
        assert_eq!(value["details"]["ok"], true);
    }
}
