//! Read-only current-state reconciliation. Never acknowledges/replays an old mutation.
use super::*;
pub(super) const SCHEMA: &str = "p4-appearance-quiescent-v1";
const UPDATE: &str = "appearance_recovery_requires_firmware_update";
const UNPROVEN: &str = "appearance_recovery_not_quiescent_or_unproven";
const WAITING: &str = "appearance_recovery_waiting_quiescence";
const UNKNOWN: &str = "appearance_recovery_device_outcome_unknown";
const BUSY_FIELDS: [&str; 13] = ["snapshotChanged", "assetTransferActive", "rawReceiving", "appearanceOwnerActive",
    "assetFileOpen", "assetSlotPending", "nativeTransferActive", "nativeReceiving",
    "nativeWorkerPending", "protocolWorkerPending", "storageWorkerPending", "firmwareOtaActive", "rebootPending"];

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub(super) struct Observation {
    pub(super) schema: String,
    pub(super) query_nonce: String,
    pub(super) board: String,
    pub(super) generation: u64,
    pub(super) boot: String,
    pub(super) active_slot: u32,
    pub(super) active_pack_id: String,
    pub(super) native_io_epoch: u32,
    pub(super) serialized: bool,
    pub(super) quiescent: bool,
    pub(super) busy_flags: std::collections::BTreeMap<String, bool>,
}
impl Observation {
    pub(super) fn valid(&self) -> bool {
        self.schema == SCHEMA && appearance_safety::valid_transfer_id(&self.query_nonce)
            && !self.board.is_empty() && self.board.len() <= 128 && !self.board.chars().any(char::is_control)
            && self.generation != 0 && !self.boot.is_empty() && self.boot.len() <= 128
            && !self.boot.chars().any(char::is_control) && self.active_slot <= 1
            && !self.active_pack_id.is_empty() && self.active_pack_id.len() <= 64
            && self.active_pack_id.bytes().all(|b| b.is_ascii_graphic())
            && self.native_io_epoch != u32::MAX
            && self.serialized && self.quiescent && self.busy_flags.len() == BUSY_FIELDS.len()
            && BUSY_FIELDS.iter().all(|key| self.busy_flags.get(*key) == Some(&false))
    }
}
fn parse_observation(ack: &serde_json::Value, nonce: &str, board: &str, generation: u64, boot: &str)
    -> Result<Observation, String>
{
    // Capability absence is classified only from the current hello below.
    // A capable device's malformed/untyped NACK is never an upgrade diagnosis.
    if !appearance_safety::valid_transfer_id(nonce) || generation == 0
        || board.is_empty() || board.len() > 128 || board.chars().any(char::is_control)
        || boot.is_empty() || boot.len() > 128 || boot.chars().any(char::is_control)
        || ack["observationSchema"].as_str() != Some(SCHEMA)
    { return Err(UNPROVEN.into()); }
    if ack["transferId"].as_str() != Some(nonce) || ack["queryNonce"].as_str() != Some(nonce)
        || ack["phase"] != "slot-query"
        || ack["boardDeviceId"].as_str() != Some(board) || ack["bootId"].as_str() != Some(boot)
        || ack["serialized"] != true
    { return Err(UNPROVEN.into()); }
    if ack["ok"].as_bool() == Some(false) {
        // Not a physical snapshot: no slots, epoch or fabricated busy flags.
        const KEYS: [&str; 11] = ["transferId", "phase", "ok", "observationSchema", "queryNonce",
            "boardDeviceId", "bootId", "serialized", "proof", "quiescent", "errorCode"];
        let fields = ack.as_object().ok_or(UNPROVEN)?;
        if fields.len() != KEYS.len() || !KEYS.iter().all(|key| fields.contains_key(*key))
            || ack["proof"].as_bool() != Some(false) || ack["quiescent"].as_bool() != Some(false)
        { return Err(UNPROVEN.into()); }
        return Err(match ack["errorCode"].as_str() {
            Some("appearance_query_busy") => WAITING,
            Some("appearance_query_outcome_unknown") => UNKNOWN,
            _ => UNPROVEN,
        }.into());
    }
    if ack["ok"].as_bool() != Some(true) || ack.get("errorCode").is_some() || ack.get("proof").is_some()
    { return Err(UNPROVEN.into()); }
    let quiescent = ack["quiescent"].as_bool().ok_or(UNPROVEN)?;
    let mut busy_flags = std::collections::BTreeMap::new();
    for key in BUSY_FIELDS
    {
        busy_flags.insert(key.to_owned(), ack.get(key).and_then(serde_json::Value::as_bool).ok_or(UNPROVEN)?);
    }
    let active = ack["activeSlot"].as_u64().filter(|n| *n <= 1).ok_or(UNPROVEN)? as u32;
    let slots = ack["slots"].as_array().filter(|rows| rows.len() == 2).ok_or(UNPROVEN)?;
    let mut seen = HashSet::new();
    let mut pack = None;
    for slot in slots {
        let number = slot["slot"].as_u64().filter(|n| *n <= 1).ok_or(UNPROVEN)? as u32;
        if !seen.insert(number) { return Err(UNPROVEN.into()); }
        let valid = slot["valid"].as_bool().ok_or(UNPROVEN)?;
        if number == active {
            if !valid { return Err(UNPROVEN.into()); }
            pack = Some(slot["packId"].as_str().ok_or(UNPROVEN)?.to_owned());
        }
    }
    let observation = Observation { schema: SCHEMA.into(), query_nonce: nonce.into(), board: board.into(),
        generation, boot: boot.into(), active_slot: active, active_pack_id: pack.ok_or(UNPROVEN)?,
        native_io_epoch: ack["nativeIoEpoch"].as_u64().and_then(|n| u32::try_from(n).ok()).ok_or(UNPROVEN)?,
        serialized: true, quiescent,
        busy_flags };
    // Only a fully typed, exact-nonce/board/boot response is retryable busy.
    // Validate the remaining proof structure without accepting its busy state.
    let mut structure = observation.clone();
    structure.quiescent = true;
    for flag in structure.busy_flags.values_mut() { *flag = false; }
    if !structure.valid() { return Err(UNPROVEN.into()); }
    if !quiescent || observation.busy_flags.values().any(|busy| *busy) { return Err(WAITING.into()); }
    if !observation.valid() { return Err(UNPROVEN.into()); }
    Ok(observation)
}
impl UsbSerialManager {
    fn recovery_connection(&self, board: &str, generation: u64, boot: &str) -> Result<(), String> {
        let (_lease, connection) = self.inbound_owner.connection(&self.connection)?;
        let c = connection.as_ref().ok_or("appearance_recovery_connection_changed")?;
        if !c.connected || c.cancel_reader.load(Ordering::Acquire) || !self.inbound_owner.is_current(generation)
            || c.board_device_id != board || c.connection_id != generation
            || c.capabilities["debugBootId"].as_str() != Some(boot)
        { return Err("appearance_recovery_connection_changed".into()); }
        self.ensure_wire_available(&c.port_name, Some(board))
    }
    pub fn reconcile_appearance_unknown(&self, board: &str, user_intent_id: &str) -> Result<serde_json::Value, String> {
        let intent = uuid::Uuid::parse_str(user_intent_id).map_err(|_| "appearance_recovery_fresh_intent_required")?;
        if intent.get_version_num() != 4 { return Err("appearance_recovery_fresh_intent_required".into()); }
        let intent = intent.to_string();
        let board = validate_expected_board_device_id(board)?;
        let _exclusive = self.asset_transfer_guard.try_lock().map_err(|error| match error {
            std::sync::TryLockError::WouldBlock => "appearance_recovery_busy",
            std::sync::TryLockError::Poisoned(_) => "appearance_recovery_owner_unavailable",
        })?;
        if self.appearance_sync_active.load(Ordering::Acquire) || self.active_appearance_attempt()?.is_some()
        { return Err("appearance_recovery_busy".into()); }
        self.begin_appearance_sync().map_err(|_| "appearance_recovery_busy".to_owned())?;
        struct Busy<'a>(&'a UsbSerialManager);
        impl Drop for Busy<'_> { fn drop(&mut self) { self.0.finish_appearance_sync(); } }
        let _busy = Busy(self);
        let observed = self.observe_pending_appearance_owned(board, &intent, &_exclusive)?;
        Ok(serde_json::json!({ "status": "observed_current_state_quiescent", "oldResult": "unknown",
            "mutationSent": false, "requiresFreshUserIntent": true,
            "activeSlot": observed.active_slot, "activePackId": observed.active_pack_id }))
    }
    // All callers own the same asset gate. This never acquires it recursively
    // or resets cancellation/busy state owned by the current follow IPC.
    fn observe_pending_appearance_owned(
        &self, board: &str, intent: &str, _exclusive: &std::sync::MutexGuard<'_, ()>,
    ) -> Result<Observation, String> {
        if self.active_appearance_attempt()?.is_some() { return Err("appearance_recovery_busy".into()); }
        #[cfg(test)] let journal = self.appearance_journal.clone();
        #[cfg(not(test))] let journal = appearance_safety::journal_for_recovery()?;
        let (generation, boot) = {
            let (_lease, mut connection) = self.inbound_owner.connection(&self.connection)?;
            let c = connection.as_mut().ok_or("appearance_recovery_connection_changed")?;
            self.refresh_pending_hello(c);
            // Do this BEFORE any probe/query, including capability checks.
            self.ensure_wire_available(&c.port_name, Some(&board))?;
            if !c.connected || c.board_device_id != board || c.cancel_reader.load(Ordering::Acquire)
                || !self.inbound_owner.is_current(c.connection_id)
            { return Err("appearance_recovery_connection_changed".into()); }
            // No unresolved mutation needs no recovery query capability. Do not
            // require a firmware upgrade just to start a fresh normal sync.
            journal.pending_for_recovery(&board, &intent)?;
            if c.capabilities["appearance"]["quiescentQuery"].as_str() != Some(SCHEMA) { return Err(UPDATE.into()); }
            let boot = c.capabilities["debugBootId"].as_str().filter(|s| !s.is_empty() && s.len() <= 128).ok_or(UPDATE)?.to_owned();
            (c.connection_id, boot)
        };
        let pending = journal.pending_for_recovery(&board, &intent)?;
        let nonce = format!("observe-{}", uuid::Uuid::new_v4());
        let receiver = self.register_asset_ack_waiter(&nonce, "slot-query", None, None)?;
        let payload = serde_json::json!({ "transferId": nonce, "queryNonce": nonce,
            "observationSchema": SCHEMA, "expectedBoardDeviceId": board, "expectedBootId": boot });
        let read_result = (|| {
            self.recovery_connection(&board, generation, &boot)?;
            self.send_inner("asset/slot-query", &payload, true, Some(&board), Some(generation), Some(P4_CH343_SERIAL_WRITE_SLICE_BYTES))?;
            let started = Instant::now();
            loop {
                self.recovery_connection(&board, generation, &boot)?;
                if self.appearance_sync_cancel_requested.load(Ordering::Acquire) { return Err("appearance_recovery_cancelled".into()); }
                let remaining = ASSET_ACK_TIMEOUT.checked_sub(started.elapsed()).ok_or("appearance_recovery_query_unknown_no_replay")?;
                match receiver.recv_timeout(remaining.min(Duration::from_millis(100))) {
                    Ok(ack) => return parse_observation(&ack, &nonce, &board, generation, &boot),
                    Err(mpsc::RecvTimeoutError::Timeout) => {},
                    Err(mpsc::RecvTimeoutError::Disconnected) => return Err("appearance_recovery_query_unknown_no_replay".into()),
                }
            }
        })();
        self.remove_asset_ack_waiter(&nonce, "slot-query", None, None);
        let observed = read_result?;
        // Linearize the exact connection/boot and durable archive with retirement.
        let (_lease, connection) = self.inbound_owner.connection(&self.connection)?;
        let c = connection.as_ref().ok_or("appearance_recovery_connection_changed")?;
        if !c.connected || c.cancel_reader.load(Ordering::Acquire) || !self.inbound_owner.is_current(generation)
            || c.connection_id != generation || c.board_device_id != board
            || c.capabilities["debugBootId"].as_str() != Some(boot.as_str())
            || self.appearance_sync_cancel_requested.load(Ordering::Acquire)
        { return Err("appearance_recovery_connection_changed".into()); }
        self.ensure_wire_available(&c.port_name, Some(&board))?;
        journal.archive_observation(&pending, &observed, &intent)?;
        Ok(observed)
    }
    /// Entry only for a NEW explicit appearance-sync invocation, before its
    /// mutation Attempt exists. Never call from a failed-attempt catch/retry.
    pub(super) fn with_appearance_user_intent<T>(
        &self, board: &str, exclusive: &std::sync::MutexGuard<'_, ()>,
        work: impl FnOnce() -> Result<T, String>,
    ) -> Result<T, String> {
        #[cfg(test)] let journal = self.appearance_journal.clone();
        #[cfg(not(test))] let journal = appearance_safety::journal_for_recovery()?;
        if !journal.needs_observation(board)? {
            return self.with_appearance_attempt(board, work);
        }
        self.ensure_appearance_sync_not_cancelled(None)?;
        // A fresh observation nonce and intent, never the old transfer/attempt.
        let intent = uuid::Uuid::new_v4().to_string();
        let observed = self.observe_pending_appearance_owned(board, &intent, exclusive)?;
        self.ensure_appearance_sync_not_cancelled(None)?;
        transfer_log::record("appearance","current_intent_observed_quiescent",
            serde_json::json!({"oldResult":"unknown","mutationReplayed":false,"newAttempt":true}));
        // Same explicit user invocation may now create its NEW UUID Attempt.
        // Board/generation/boot must still equal the read-only observation.
        self.with_appearance_attempt_bound(board, Some((observed.generation, &observed.boot)), work)
    }
}

#[cfg(test)]
mod appearance_query_contract_tests {
    use super::*;

    #[test]
    fn actual_c_query_envelopes_and_fail_closed_shared_vectors() {
        let data: serde_json::Value = serde_json::from_str(include_str!(
            "../../test-fixtures/appearance-query-envelope-v1.json"
        )).expect("synthetic shared C response vectors");
        assert_eq!(data["syntheticOnly"], true);
        let vectors = data["vectors"].as_array().expect("vectors");
        assert!(vectors.len() >= 80);
        for vector in vectors {
            let name = vector["name"].as_str().expect("name");
            let parsed = parse_observation(&vector["ack"],
                vector["nonce"].as_str().unwrap(), vector["board"].as_str().unwrap(),
                vector["generation"].as_u64().unwrap(), vector["boot"].as_str().unwrap());
            match vector["expect"].as_str().unwrap() {
                "ok" => {
                    let proof = parsed.unwrap_or_else(|error| panic!("{name}: {error}"));
                    assert!(proof.valid(), "{name}");
                    assert_eq!(proof.active_slot, 0, "{name}");
                    assert_eq!(proof.active_pack_id, "a".repeat(64), "{name}");
                }
                expected => {
                    // No NACK, busy or malformed response can return an
                    // Observation that could reach archive_observation.
                    let error = match parsed {
                        Ok(_) => panic!("{name}: non-proof accepted"),
                        Err(error) => error,
                    };
                    let code = match expected {
                        "waiting" => WAITING,
                        "unknown" => UNKNOWN,
                        "unproven" => UNPROVEN,
                        _ => panic!("{name}: invalid vector expectation"),
                    };
                    assert_eq!(error, code, "{name}");
                    assert_ne!(error, UPDATE, "{name}: NACK is not a capability diagnosis");
                    if expected == "unknown" { assert_ne!(error, WAITING, "{name}"); }
                }
            }
        }
    }
}
