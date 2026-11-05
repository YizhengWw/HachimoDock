/*
 * [Input] Structured USB appearance and firmware transfer lifecycle/ack events.
 * [Output] Bounded JSONL diagnostics persisted under the current app-data directory.
 * [Pos] Release-safe USB transfer observability helper for usb_serial.
 * [Sync] If this file changes, update `pc/.folder.md`.
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
    record("logger", "configured", serde_json::json!({}));
    Ok(configured.path.clone())
}

pub fn record(scope: &str, event: &str, details: Value) {
    let Some(state) = TRANSFER_LOG_STATE.get() else {
        return;
    };
    let Ok(_guard) = state.write_guard.lock() else {
        return;
    };
    if append_record(&state.path, scope, event, details).is_err() {
        eprintln!("[usb-transfer-log] append failed");
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
        "scope": safe_label(scope),
        "event": safe_label(event),
        "details": diagnostic_details(&details, 0),
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

// Diagnostic output is a separate, deliberately smaller schema than the wire
// protocol. Never serialize a peer response, free-form error, path, content
// digest or stable device/user identifier verbatim, even inside a nested ACK.
fn diagnostic_details(details: &Value, depth: usize) -> Value {
    let mut clean = serde_json::Map::new();
    let Some(fields) = details.as_object() else { return Value::Object(clean); };
    for (key, value) in fields {
        let permitted_number = matches!(key.as_str(),
            "index" | "seq" | "sequence" | "offset" | "size" | "files" | "bytes" |
            "fileCount" | "totalBytes" | "rawBytes" | "chunkBytes" | "initialChunkBytes" |
            "chunks" | "attempt" | "attempts" | "transactionAttempt" | "generation" |
            "baud" | "timeoutMs" | "elapsedMs" | "transactionElapsedMs" |
            "readyElapsedMs" | "rawWriteElapsedMs" | "effectiveBytesPerSecond" |
            "expectedBytes" | "actualBytes" | "occurrences" | "totalLines" |
            "decodedBytes" | "nulChars" | "replacementChars" | "core" |
            "lastAckNextSequence" | "lastAckReceivedBytes" | "protocolSchema");
        let permitted_bool = matches!(key.as_str(),
            "ok" | "authenticated" | "resetProven" | "framed" | "rawBoundaryProven" |
            "rawBodyWritten" | "intentPersisted" | "automaticCleanup" | "automaticRetry" |
            "writeStarted" | "writeCompleted" | "mutationReplayed" | "newAttempt" |
            "originalPayloadRetained" | "legacyRelease");
        if (permitted_number && value.as_u64().is_some()) || (permitted_bool && value.is_boolean()) {
            clean.insert(key.clone(), value.clone());
        } else if value.is_string() && matches!(key.as_str(), "scope" | "phase" | "stage" | "outcome" | "kind" |
            "errorKind" | "errorCode" | "lastAckPhase" | "transport" | "byteScope" | "ack" | "oldResult") {
            if let Some(label) = value.as_str() {
                clean.insert(key.clone(), Value::String(safe_label(label).into()));
            }
        } else if depth < 2 && matches!(key.as_str(), "metadata" | "response" | "ack") && value.is_object() {
            clean.insert(key.clone(), diagnostic_details(value, depth + 1));
        }
    }
    Value::Object(clean)
}

fn safe_label(label: &str) -> &str {
    match label {
        "logger" | "connection" | "appearance" | "firmware" | "widget" |
        "configured" | "probe" | "failed" | "verified" | "stage" |
        "failure_metadata" | "transaction_started" | "transaction_failed" |
        "transaction_committed" | "native_transaction_started" | "native_transaction_committed" |
        "native_file_started" | "file_started" | "file_committed" | "builtin_activated" |
        "begin_acknowledged" | "commit_acknowledged" | "chunk_attempt" | "chunk_attempt_failed" |
        "validation_reconnected" | "validation_timeout" | "validation_failed" |
        "abort_requested" | "raw_ready_rejected" | "raw_ready_ack" | "raw_chunk_send_failed" |
        "raw_chunk_ack" | "device_numeric_trace" | "unparsed_line" | "device_fault_hint" |
        "device_abort_site" | "control_integrity_rejected" | "diagnostic_failed_frame_capture" |
        "serial_read_ended" | "serial_write_failed" | "result_unknown_no_replay" |
        "not_submitted" | "activation_failure" | "current_intent_observed_quiescent" |
        "begin" | "chunk" | "raw-chunk" | "raw-ready" | "commit" | "abort" |
        "idle" | "unknown" | "pending" | "accepted" | "rejected" | "timeout" |
        "read" | "write" | "probe_hello" | "live" | "preparing_storage" | "transferring" |
        "preparing" | "activating" | "validating" | "completed" | "cancelled" |
        "uart-raw-v1" | "json-base64-v1" | "native-usb" | "body" | "physical_line" |
        "header" | "length" | "hash" | "required_prefix" | "negotiation" | "utf8" |
        "admission" | "ack" | "response" | "admission_failed" | "write_failed" |
        "post_write_failed" | "ack_timeout" | "ack_channel_closed" | "ack_read_failed" |
        "malformed_response" | "device_rejected" | "none" | "not_observed" |
        "positive" | "exact_negative" | "malformed" | "audio" | "video" => label,
        _ => "other",
    }
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

    #[test]
    fn drops_identifiers_paths_credentials_and_nested_peer_text() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("transfer.jsonl");
        let private = "synthetic-private-value";
        append_record(&path, private, private, serde_json::json!({
            "sourcePath": private, "boardDeviceId": private, "packId": private,
            "transferId": private, "error": private, "token": private,
            "phase": private, "index": private, "size": 4096, "ok": true,
            "response": {"path": private, "error": private, "index": 2, "ok": false,
                "metadata": {"sourcePath": private, "size": 128}},
            "unrecognized": {"size": 3, "secret": private}
        })).unwrap();
        let text = fs::read_to_string(path).unwrap();
        assert!(!text.contains(private));
        let row: Value = serde_json::from_str(text.trim()).unwrap();
        assert_eq!(row["scope"], "other");
        assert_eq!(row["details"]["phase"], "other");
        assert_eq!(row["details"]["size"], 4096);
        assert_eq!(row["details"]["response"]["index"], 2);
        assert!(row["details"]["index"].is_null());
        assert!(row["details"]["unrecognized"].is_null());
    }

    #[test]
    fn rejects_wrong_types_and_unbounded_nesting() {
        let clean = diagnostic_details(&serde_json::json!({
            "bytes": -1, "index": 1.5, "ok": "true", "phase": ["begin"],
            "metadata": {"response": {"metadata": {"size": 1}, "size": 2}},
            "response": [ {"size": 3} ]
        }), 0);
        assert_eq!(clean, serde_json::json!({"metadata":{"response":{"size":2}}}));
    }

    #[test]
    fn preserves_fixed_failure_categories_without_peer_messages() {
        let clean = diagnostic_details(&serde_json::json!({
            "stage":"ack", "errorKind":"ack_timeout", "ack":"not_observed",
            "error":"untrusted peer message", "writeCompleted":true,
            "response":{"ok":false,"error":"untrusted peer message"}
        }), 0);
        assert_eq!(clean, serde_json::json!({"stage":"ack","errorKind":"ack_timeout",
            "ack":"not_observed","writeCompleted":true,"response":{"ok":false}}));
    }

    #[test]
    fn rotates_without_retaining_raw_details() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("transfer.jsonl");
        let file = fs::File::create(&path).unwrap();
        file.set_len(TRANSFER_LOG_MAX_BYTES).unwrap();
        append_record(&path, "firmware", "transaction_failed", serde_json::json!({
            "error":"synthetic-private-error", "phase":"begin", "elapsedMs":100
        })).unwrap();
        assert!(path.with_file_name(TRANSFER_LOG_BACKUP_FILE).exists());
        let text = fs::read_to_string(path).unwrap();
        assert!(!text.contains("synthetic-private-error"));
        assert!(text.contains("elapsedMs"));
    }
}
