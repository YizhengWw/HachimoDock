//! One-shot master-volume RPC. No transport fallback, set replay, local volume
//! cache, or inferred persistence. Unknown delivery requires a read-only query.
use super::*;

// Kept std-only so the validation tests can run directly with rustc, without
// building the concurrently edited application or copying its test resources.
mod policy {
    pub(super) const ENGINE: &str = "p4-master-volume-v1";
    pub(super) fn board(value: &str) -> Result<&str, &'static str> {
        if value.is_empty()
            || value.len() > 64
            || value.trim() != value
            || value.chars().any(char::is_control)
        {
            return Err("invalid expectedBoardDeviceId");
        }
        Ok(value)
    }
    pub(super) fn volume(value: Option<u8>) -> Result<(), &'static str> {
        if value.is_some_and(|v| v > 100) {
            return Err("volume must be an integer from 0 to 100");
        }
        Ok(())
    }
    pub(super) fn session(
        expected: &str,
        generation: u64,
        actual: &str,
        actual_generation: u64,
        connected: bool,
        cancelled: bool,
        current: bool,
        capability: Option<&str>,
    ) -> Result<(), &'static str> {
        board(expected)?;
        if generation == 0
            || generation != actual_generation
            || expected != actual
            || !connected
            || cancelled
            || !current
        {
            return Err("master volume connection retired or changed");
        }
        if capability != Some(ENGINE) {
            return Err("master volume capability unavailable");
        }
        Ok(())
    }
    pub(super) struct Receipt<'a> {
        pub ok: Option<bool>,
        pub engine: Option<&'a str>,
        pub board: Option<&'a str>,
        pub request: Option<&'a str>,
        pub volume: Option<u64>,
        pub persisted: Option<bool>,
        pub optional_fields_valid: bool,
    }
    pub(super) fn receipt(
        value: Receipt<'_>,
        expected: &str,
        request: &str,
        set: Option<u8>,
    ) -> Result<(), &'static str> {
        if value.ok != Some(true)
            || value.engine != Some(ENGINE)
            || value.board != Some(expected)
            || request.is_empty()
            || value.request != Some(request)
            || value.persisted.is_none()
            || !value.optional_fields_valid
        {
            return Err("invalid master volume receipt");
        }
        let volume = value
            .volume
            .filter(|v| *v <= 100)
            .ok_or("master volume reading unknown")?;
        if set.is_some_and(|v| u64::from(v) != volume) {
            return Err("master volume set echo mismatch");
        }
        Ok(())
    }
    #[cfg(test)]
    mod tests {
        use super::*;
        fn reply() -> Receipt<'static> {
            Receipt {
                ok: Some(true),
                engine: Some(ENGINE),
                board: Some("board"),
                request: Some("request"),
                volume: Some(45),
                persisted: Some(false),
                optional_fields_valid: true,
            }
        }
        #[test]
        fn exact_board_engine_and_request_are_required() {
            assert!(receipt(reply(), "board", "request", None).is_ok());
            for field in 0..6 {
                let mut r = reply();
                match field {
                    0 => r.board = None,
                    1 => r.board = Some("other"),
                    2 => r.engine = None,
                    3 => r.engine = Some("other"),
                    4 => r.request = Some("other"),
                    _ => r.ok = Some(false),
                };
                assert!(receipt(r, "board", "request", None).is_err());
            }
        }
        #[test]
        fn unknown_null_out_of_range_and_wrong_set_echo_are_not_success() {
            for v in [None, Some(101), Some(u64::MAX)] {
                let mut r = reply();
                r.volume = v;
                assert!(receipt(r, "board", "request", None).is_err());
            }
            assert!(receipt(reply(), "board", "request", Some(46)).is_err());
            assert!(receipt(reply(), "board", "request", Some(45)).is_ok());
            let mut r = reply();
            r.persisted = None;
            assert!(receipt(r, "board", "request", None).is_err());
            let mut r = reply();
            r.optional_fields_valid = false;
            assert!(receipt(r, "board", "request", None).is_err());
        }
        #[test]
        fn stale_generation_cancel_and_retired_owner_fail_closed() {
            for (generation, cancelled, current) in [
                (2, false, true),
                (1, true, true),
                (1, false, false),
                (0, false, true),
            ] {
                assert!(session(
                    "board",
                    1,
                    "board",
                    generation,
                    true,
                    cancelled,
                    current,
                    Some(ENGINE)
                )
                .is_err());
            }
            assert!(session("board", 1, "other", 1, true, false, true, Some(ENGINE)).is_err());
            assert!(session("board", 1, "board", 1, true, false, true, None).is_err());
            assert!(session("board", 1, "board", 1, true, false, true, Some(ENGINE)).is_ok());
        }
        #[test]
        fn input_boundaries_are_not_clamped_or_normalized() {
            for v in [None, Some(0), Some(100)] {
                assert!(volume(v).is_ok());
            }
            assert!(volume(Some(101)).is_err());
            assert!(volume(Some(255)).is_err());
            for b in ["", " board", "board ", "board\n"] {
                assert!(board(b).is_err());
            }
        }
    }
}
use policy::ENGINE;
const RESPONSE_TOPIC: &str = "audio/volume-state";
const UNKNOWN: &str = "audio_volume_result_unknown_refresh_required";

fn check_connection(
    manager: &UsbSerialManager,
    connection: &UsbConnection,
    board: &str,
    generation: u64,
) -> Result<(), String> {
    policy::session(
        board,
        generation,
        &connection.board_device_id,
        connection.connection_id,
        connection.connected,
        connection.cancel_reader.load(Ordering::Acquire),
        manager.inbound_owner.is_current(generation),
        connection.capabilities["audioMasterVolume"].as_str(),
    )
    .map_err(str::to_owned)
}

/// Rechecked while send_inner owns both the inbound lease and connection lock.
/// Generic/unbound sends cannot obtain the trusted volume control path.
pub(super) fn validate_write(
    connection: &UsbConnection,
    board: Option<&str>,
    generation: Option<u64>,
    payload: &serde_json::Value,
) -> Result<(), String> {
    let board = board.ok_or("master volume requires an exact board binding")?;
    let generation = generation
        .filter(|v| *v != 0)
        .ok_or("master volume requires a connection generation")?;
    if connection.connection_id != generation
        || connection.board_device_id != board
        || connection.capabilities["audioMasterVolume"] != ENGINE
    {
        return Err("master volume binding or capability changed; not submitted".into());
    }
    let object = payload.as_object().ok_or("invalid master volume request")?;
    let set = match payload["operation"].as_str() {
        Some("query") => false,
        Some("set") => true,
        _ => return Err("invalid master volume operation".into()),
    };
    if object.len() != if set { 4 } else { 3 }
        || payload["expectedBoardDeviceId"].as_str() != Some(board)
        || !payload["requestId"]
            .as_str()
            .is_some_and(|v| !v.is_empty() && v.len() <= 96 && !v.chars().any(char::is_control))
        || (set && !payload["volume"].as_u64().is_some_and(|v| v <= 100))
        || (!set && object.contains_key("volume"))
    {
        return Err("invalid master volume request".into());
    }
    Ok(())
}
fn validate_response(
    response: &serde_json::Value,
    board: &str,
    request: &str,
    set: Option<u8>,
) -> Result<(), String> {
    policy::receipt(
        policy::Receipt {
            ok: response["ok"].as_bool(),
            engine: response["engine"].as_str(),
            board: response["boardDeviceId"].as_str(),
            request: response["requestId"].as_str(),
            volume: response["volume"].as_u64(),
            persisted: response["persisted"].as_bool(),
            optional_fields_valid: response.get("savePending").is_none_or(|v| v.is_boolean())
                && response
                    .get("persistState")
                    .is_none_or(|v| v.as_str().is_some_and(|s| !s.is_empty() && s.len() <= 64)),
        },
        board,
        request,
        set,
    )
    .map_err(str::to_owned)
}
impl UsbSerialManager {
    pub fn device_volume(
        &self,
        board: &str,
        volume: Option<u8>,
    ) -> Result<serde_json::Value, String> {
        self.device_volume_timeout(board, volume, P4_DEVICE_RESPONSE_TIMEOUT)
    }
    fn device_volume_timeout(
        &self,
        board: &str,
        volume: Option<u8>,
        timeout: Duration,
    ) -> Result<serde_json::Value, String> {
        policy::board(board).map_err(str::to_owned)?;
        policy::volume(volume).map_err(str::to_owned)?;
        let _bulk = self
            .asset_transfer_guard
            .try_lock()
            .map_err(|_| "device transfer busy; volume not submitted")?;
        let (generation, cancel) = {
            let (_lease, slot) = self.inbound_owner.connection(&self.connection)?;
            let connection = slot.as_ref().ok_or("USB is not connected")?;
            check_connection(self, connection, board, connection.connection_id)?;
            (connection.connection_id, connection.cancel_reader.clone())
        };
        let request = format!("volume-{}", uuid::Uuid::new_v4());
        let mut payload = serde_json::json!({"operation":if volume.is_some(){"set"}else{"query"},
            "requestId":request,"expectedBoardDeviceId":board});
        if let Some(volume) = volume {
            payload["volume"] = volume.into();
        }
        let (sender, receiver) = mpsc::channel();
        self.device_response_waiters
            .lock()
            .map_err(|_| "volume receipt queue unavailable")?
            .push(DeviceResponseWaiter {
                request_id: request.clone(),
                response_topic: RESPONSE_TOPIC.into(),
                sender,
            });
        // Exactly one logical send. Partial writes, lost ACKs, NACKs and stale
        // sessions must never cause a retry, reconnect, or automatic set replay.
        let sent = self.send_inner(
            "audio/volume",
            &payload,
            true,
            Some(board),
            Some(generation),
            None,
        );
        let response = match sent {
            Ok(()) => receiver
                .recv_timeout(timeout)
                .map_err(|_| format!("{UNKNOWN}: receipt unavailable")),
            Err(_) => Err(format!("{UNKNOWN}: submission unconfirmed")),
        };
        self.remove_device_response_waiter(&request, RESPONSE_TOPIC);
        let response = response?;
        validate_response(&response, board, &request, volume)
            .map_err(|e| format!("{UNKNOWN}: {e}"))?;
        // The receipt can already be queued when disconnect/replacement wins.
        // Linearize success under the same owner gate as inbound dispatch.
        {
            let (_lease, slot) = self
                .inbound_owner
                .connection(&self.connection)
                .map_err(|_| format!("{UNKNOWN}: owner unavailable"))?;
            let connection = slot
                .as_ref()
                .ok_or_else(|| format!("{UNKNOWN}: disconnected"))?;
            if cancel.load(Ordering::Acquire) || !Arc::ptr_eq(&cancel, &connection.cancel_reader) {
                return Err(format!("{UNKNOWN}: connection cancelled"));
            }
            check_connection(self, connection, board, generation)
                .map_err(|e| format!("{UNKNOWN}: {e}"))?;
        }
        Ok(response)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::{json, Value};
    #[derive(Clone, Copy)]
    enum Mode {
        Reply,
        WrongBoard,
        WrongEcho,
        Null,
        Drop,
        Close,
        Retire,
        Cancel,
        Partial,
    }
    struct MemoryWriter {
        pending: Vec<u8>,
        trace: Arc<Mutex<Vec<Value>>>,
        waiters: Arc<Mutex<Vec<DeviceResponseWaiter>>>,
        owner: Arc<inbound_owner::Owner>,
        cancel: Arc<AtomicBool>,
        mode: Mode,
        writes: Arc<AtomicUsize>,
    }
    impl Write for MemoryWriter {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            self.writes.fetch_add(1, Ordering::SeqCst);
            if matches!(self.mode, Mode::Partial) {
                if self.pending.is_empty() {
                    self.pending.push(bytes[0]);
                    return Ok(1);
                }
                return Err(std::io::Error::new(
                    std::io::ErrorKind::BrokenPipe,
                    "injected partial write",
                ));
            }
            self.pending.extend_from_slice(bytes);
            while let Some(end) = self.pending.iter().position(|v| *v == b'\n') {
                let line = self.pending.drain(..=end).collect::<Vec<_>>();
                let request: Value = serde_json::from_slice(&line).unwrap();
                self.trace.lock().unwrap().push(request.clone());
                let p = &request["payload"];
                if matches!(self.mode, Mode::Drop) {
                    continue;
                }
                if matches!(self.mode, Mode::Close) {
                    self.waiters.lock().unwrap().clear();
                    continue;
                }
                let mut response = json!({"ok":true,"engine":ENGINE,"boardDeviceId":"volume-board","requestId":p["requestId"],
                    "volume":p.get("volume").cloned().unwrap_or(json!(42)),"persisted":false,"savePending":true});
                match self.mode {
                    Mode::WrongBoard => response["boardDeviceId"] = json!("other"),
                    Mode::WrongEcho => response["volume"] = json!(1),
                    Mode::Null => response["volume"] = Value::Null,
                    _ => {}
                }
                // Wrong topic and wrong request must leave the owned waiter intact.
                resolve_device_response(&self.waiters, "protocol/ack", &response);
                let mut unrelated = response.clone();
                unrelated["requestId"] = json!("unrelated");
                resolve_device_response(&self.waiters, RESPONSE_TOPIC, &unrelated);
                assert_eq!(self.waiters.lock().unwrap().len(), 1);
                resolve_device_response(&self.waiters, RESPONSE_TOPIC, &response);
                if matches!(self.mode, Mode::Retire) {
                    self.owner.retire(1);
                }
                if matches!(self.mode, Mode::Cancel) {
                    self.cancel.store(true, Ordering::Release);
                }
            }
            Ok(bytes.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            panic!("volume must use the live path without draining")
        }
    }
    fn manager(mode: Mode) -> (UsbSerialManager, Arc<Mutex<Vec<Value>>>, Arc<AtomicUsize>) {
        let manager = UsbSerialManager::new();
        let trace = Arc::new(Mutex::new(vec![]));
        let writes = Arc::new(AtomicUsize::new(0));
        let cancel = Arc::new(AtomicBool::new(false));
        *manager.connection.lock().unwrap() = Some(UsbConnection {
            connection_id: 1,
            port_name: "memory-only-volume".into(),
            baud_rate: 115200,
            writer: Box::new(MemoryWriter {
                pending: vec![],
                trace: trace.clone(),
                waiters: manager.device_response_waiters.clone(),
                owner: manager.inbound_owner.clone(),
                cancel: cancel.clone(),
                mode,
                writes: writes.clone(),
            }),
            board_device_id: "volume-board".into(),
            runtime: "esp-p4".into(),
            device_model: String::new(),
            firmware: String::new(),
            build_id: String::new(),
            git_sha: String::new(),
            build_dirty: false,
            protocol_schema: 7,
            wire_protocol: String::new(),
            capabilities: json!({"audioMasterVolume":ENGINE}),
            connected: true,
            cancel_reader: cancel,
            capture_active: Arc::new(AtomicBool::new(true)),
        });
        {
            let _owner = manager.inbound_owner.lock().unwrap();
            manager.inbound_owner.publish(1);
        }
        (manager, trace, writes)
    }
    #[test]
    fn memory_query_and_set_are_bound_one_shot_and_preserve_persistence_state() {
        for volume in [None, Some(0), Some(100)] {
            let (manager, trace, _) = manager(Mode::Reply);
            let result = manager
                .device_volume_timeout("volume-board", volume, Duration::from_millis(20))
                .unwrap();
            assert_eq!(result["persisted"], false);
            assert_eq!(result["savePending"], true);
            let trace = trace.lock().unwrap();
            assert_eq!(trace.len(), 1);
            let p = &trace[0]["payload"];
            assert_eq!(trace[0]["topic"], "audio/volume");
            assert_eq!(p["expectedBoardDeviceId"], "volume-board");
            assert_eq!(
                p["operation"],
                if volume.is_some() { "set" } else { "query" }
            );
            assert_eq!(p.get("volume").is_some(), volume.is_some());
            assert!(manager.device_response_waiters.lock().unwrap().is_empty());
            assert!(manager
                .connection
                .lock()
                .unwrap()
                .as_ref()
                .unwrap()
                .capture_active
                .load(Ordering::Acquire));
        }
    }
    #[test]
    fn memory_wrong_board_echo_unknown_timeout_and_stale_owner_never_retry() {
        for mode in [
            Mode::WrongBoard,
            Mode::WrongEcho,
            Mode::Null,
            Mode::Drop,
            Mode::Close,
            Mode::Retire,
            Mode::Cancel,
        ] {
            let (manager, trace, _) = manager(mode);
            let error = manager
                .device_volume_timeout("volume-board", Some(55), Duration::from_millis(10))
                .unwrap_err();
            assert!(error.starts_with(UNKNOWN), "{error}");
            assert_eq!(trace.lock().unwrap().len(), 1);
            assert!(manager.device_response_waiters.lock().unwrap().is_empty());
        }
        let (manager, trace, writes) = manager(Mode::Partial);
        assert!(manager
            .device_volume_timeout("volume-board", Some(55), Duration::from_millis(10))
            .unwrap_err()
            .starts_with(UNKNOWN));
        assert!(trace.lock().unwrap().is_empty());
        assert_eq!(writes.load(Ordering::SeqCst), 2);
        assert!(manager.device_response_waiters.lock().unwrap().is_empty());
    }
    #[test]
    fn memory_preflight_and_write_binding_reject_without_bytes() {
        for case in 0..7 {
            let (manager, trace, writes) = manager(Mode::Reply);
            let mut board = "volume-board";
            let mut value = Some(55);
            match case {
                0 => board = "wrong",
                1 => value = Some(101),
                2 => {
                    manager
                        .connection
                        .lock()
                        .unwrap()
                        .as_mut()
                        .unwrap()
                        .capabilities = Value::Null
                }
                3 => manager
                    .connection
                    .lock()
                    .unwrap()
                    .as_mut()
                    .unwrap()
                    .cancel_reader
                    .store(true, Ordering::Release),
                4 => manager.inbound_owner.retire(1),
                5 => {
                    manager
                        .connection
                        .lock()
                        .unwrap()
                        .as_mut()
                        .unwrap()
                        .connection_id = 2
                }
                _ => {}
            }
            let bulk = (case == 6).then(|| manager.asset_transfer_guard.lock().unwrap());
            assert!(manager
                .device_volume_timeout(board, value, Duration::from_millis(10))
                .is_err());
            drop(bulk);
            assert!(trace.lock().unwrap().is_empty());
            assert_eq!(writes.load(Ordering::SeqCst), 0);
        }
        let (manager, trace, writes) = manager(Mode::Reply);
        let p =
            json!({"operation":"query","requestId":"test","expectedBoardDeviceId":"volume-board"});
        assert!(manager
            .send_inner(
                "audio/volume",
                &p,
                true,
                Some("volume-board"),
                Some(2),
                None
            )
            .is_err());
        assert!(manager.send("audio/volume", &p).is_err());
        manager
            .connection
            .lock()
            .unwrap()
            .as_mut()
            .unwrap()
            .capabilities = Value::Null;
        assert!(manager
            .send_inner(
                "audio/volume",
                &p,
                true,
                Some("volume-board"),
                Some(1),
                None
            )
            .is_err());
        assert!(trace.lock().unwrap().is_empty());
        assert_eq!(writes.load(Ordering::SeqCst), 0);
    }
    #[test]
    fn json_receipt_requires_integer_and_known_reading() {
        let base = json!({"ok":true,"engine":ENGINE,"boardDeviceId":"volume-board","requestId":"request","volume":50,"persisted":true});
        for value in [
            Value::Null,
            json!(50.0),
            json!("50"),
            json!(-1),
            json!(101),
            json!(true),
        ] {
            let mut reply = base.clone();
            reply["volume"] = value;
            assert!(validate_response(&reply, "volume-board", "request", None).is_err());
        }
        assert!(validate_response(&base, "volume-board", "request", Some(50)).is_ok());
    }
}
