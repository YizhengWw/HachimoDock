use super::*;

#[derive(Default)]
struct CheckedState {
    bytes: Vec<u8>,
    flushes: usize,
    transient_after_prefix: bool,
}

struct CheckedWriter {
    pause: Arc<AtomicBool>,
    expected: bool,
    state: Arc<Mutex<CheckedState>>,
}

impl Write for CheckedWriter {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        assert_eq!(self.pause.load(Ordering::Acquire), self.expected);
        let mut state = self.state.lock().unwrap();
        if state.transient_after_prefix && state.bytes.len() >= 7 {
            state.transient_after_prefix = false;
            return Err(std::io::Error::new(std::io::ErrorKind::TimedOut, "synthetic zero progress"));
        }
        let count = bytes.len().min(7);
        state.bytes.extend_from_slice(&bytes[..count]);
        Ok(count)
    }

    fn flush(&mut self) -> std::io::Result<()> {
        assert!(self.expected, "bounded query must not flush");
        self.state.lock().unwrap().flushes += 1;
        Ok(())
    }
}

fn manager(expected_pause: bool, transient: bool) -> (UsbSerialManager, Arc<Mutex<CheckedState>>) {
    let manager = UsbSerialManager::new();
    let state = Arc::new(Mutex::new(CheckedState {
        transient_after_prefix: transient,
        ..CheckedState::default()
    }));
    *manager.connection.lock().unwrap() = Some(UsbConnection {
        connection_id: 1, port_name: "memory-query-duplex".into(), baud_rate: P4_USB_UART_BAUD,
        writer: Box::new(CheckedWriter {
            pause: manager.serial_body_write_active.clone(), expected: expected_pause, state: state.clone(),
        }),
        board_device_id: "p4-memory".into(), runtime: "esp-p4".into(), device_model: String::new(),
        firmware: String::new(), build_id: String::new(), git_sha: String::new(), build_dirty: false,
        protocol_schema: 7, wire_protocol: String::new(), capabilities: serde_json::Value::Null,
        connected: true, cancel_reader: Arc::new(AtomicBool::new(false)),
        capture_active: Arc::new(AtomicBool::new(false)),
    });
manager.activate_memory_test_connection();
    (manager, state)
}

#[test]
fn diagnostic_query_manager_keeps_rx_and_resumes_only_unwritten_suffix() {
    let (manager, state) = manager(false, true);
    let payload = serde_json::json!({"requestId": "memory-once", "unused": "x".repeat(180)});
    manager.send("diagnostics/query", &payload).unwrap();
    let state = state.lock().unwrap();
    let expected = format!("{}\n", serde_json::json!({"topic":"diagnostics/query", "payload":payload}));
    assert_eq!(state.bytes, expected.as_bytes(), "no logical frame replay or duplicated prefix");
    assert_eq!(state.flushes, 0);
    assert!(!state.transient_after_prefix);
    assert!(!manager.serial_body_write_active.load(Ordering::Acquire));
    assert!(manager.connection.lock().unwrap().as_ref().unwrap().connected);
}

#[test]
fn ordinary_mutation_still_pauses_and_keeps_legacy_flush_policy() {
    let (manager, state) = manager(true, false);
    manager.send("widget/activate", &serde_json::json!({"widgetId":"memory-only"})).unwrap();
    assert!(!manager.serial_body_write_active.load(Ordering::Acquire));
    let state = state.lock().unwrap();
    assert_eq!(state.bytes.iter().filter(|&&byte| byte == b'\n').count(), 1);
    #[cfg(target_os = "macos")]
    assert_eq!(state.flushes, 0);
    #[cfg(not(target_os = "macos"))]
    assert!(state.flushes > 0);
}

#[test]
fn diagnostic_whitelist_does_not_include_nearby_actions_or_bulk() {
    assert!(serial_frame_is_bounded("diagnostics/query", false));
    assert!(serial_frame_is_bounded("system/heartbeat", false));
    for topic in ["diagnostics/touch-test", "diagnostics/query-extra", "diagnostics/query/start",
        "widget/activate", "asset/query", "asset/raw-chunk", "firmware/query", "firmware/begin"] {
        assert!(!serial_frame_is_bounded(topic, false), "{topic}");
    }
}

#[test]
fn connected_memory_fixture_cannot_send_after_dispatch_retirement_or_cancel() {
    for cancelled in [false, true] {
        let (manager, state) = manager(false, false);
        assert!(manager.inbound_owner.is_current(1));
        if cancelled {
            manager.connection.lock().unwrap().as_ref().unwrap().cancel_reader.store(true, Ordering::Release);
        } else {
            let _dispatch = manager.inbound_owner.lock().unwrap();
            manager.inbound_owner.retire(1);
        }
        assert!(manager.connection.lock().unwrap().as_ref().unwrap().connected,
            "physical connected flag alone must not grant admission");
        if cancelled {
            assert!(!manager.status().connected, "cancelled connection is correctly reported offline");
        }
        let error = manager.send("diagnostics/query", &serde_json::json!({"requestId":"retired"})).unwrap_err();
        assert!(error.contains("USB connection retired"));
        assert!(state.lock().unwrap().bytes.is_empty());
        assert!(manager.device_response_waiters.lock().unwrap().is_empty());
    }
}
