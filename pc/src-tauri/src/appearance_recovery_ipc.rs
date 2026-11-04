//! Explicit read-only observation/archival; never resumes or resends appearance changes.
#[tauri::command]
pub async fn usb_reconcile_appearance_unknown(
    usb_manager: tauri::State<'_, crate::usb_serial::UsbSerialManager>,
    expected_board_device_id: String,
    user_intent_id: String,
) -> Result<serde_json::Value, String> {
    let manager = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        manager.reconcile_appearance_unknown(&expected_board_device_id, &user_intent_id)
            .map_err(|error| match error.as_str() {
                "appearance_recovery_requires_firmware_update"
                | "appearance_recovery_not_quiescent_or_unproven"
                | "appearance_recovery_busy"
                | "appearance_recovery_cancelled"
                | "appearance_recovery_connection_changed"
                | "appearance_raw_stream_quarantined_no_probe"
                | "appearance_recovery_no_pending"
                | "appearance_recovery_fresh_intent_required"
                | "appearance_recovery_query_unknown_no_replay"
                | "appearance_safety_journal_unavailable" => error,
                _ => "appearance_recovery_read_only_failed".to_owned(),
            })
    }).await.map_err(|_| "appearance_recovery_worker_failed".to_owned())?
}
