//! Production-manager tests. Transport is memory-only; routing uses the actual
//! request/ACK waiters. No serial handle, timing override or installer limiter.
//! Parent integration: #[cfg(test)] mod component_sync_safety_tests;
use super::*;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::Write;
use std::sync::{mpsc, Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

const BOARD: &str = "sync-safety-board";
const WIDGET: &str = "sync-safety-fixture";
#[derive(Clone, Copy)]
enum Mode {
    Audio { playback: bool, state: &'static str },
    MissingChunkAck,
    InvalidChunkAck(u8),
    DeleteAck,
    DeleteWriteRejected,
}
#[derive(Default)]
struct Trace { requests: Vec<Value>, first_chunk: Option<Instant> }
struct MemoryWriter {
    pending: Vec<u8>,
    trace: Arc<Mutex<Trace>>,
    device_waiters: Arc<Mutex<Vec<DeviceResponseWaiter>>>,
    widget_waiters: Arc<Mutex<Vec<WidgetAckWaiter>>>,
    deferred_media: Option<mpsc::Sender<Value>>,
    mode: Mode,
}
impl Write for MemoryWriter {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        self.pending.extend_from_slice(bytes);
        assert!(self.pending.len() <= 65_536, "bounded memory-only fixture frames");
        while let Some(end) = self.pending.iter().position(|b| *b == b'\n') {
            let row = self.pending.drain(..=end).collect::<Vec<_>>();
            let request: Value = serde_json::from_slice(&row).expect("production JSON frame");
            let topic = request["topic"].as_str().expect("topic");
            let payload = &request["payload"];
            {
                let mut trace = self.trace.lock().unwrap();
                assert!(trace.requests.len() < 32, "unbounded transmission/retry");
                if topic == "widget/chunk" && trace.first_chunk.is_none() {
                    trace.first_chunk = Some(Instant::now());
                }
                trace.requests.push(request.clone());
            }
            match (self.mode, topic) {
                (Mode::Audio { playback, .. }, "diagnostics/query") => {
                    assert!(self.device_waiters.lock().unwrap().iter().any(|w|
                        w.request_id == payload["requestId"].as_str().unwrap()
                            && w.response_topic == "diagnostics/status"));
                    resolve_device_response(&self.device_waiters, "diagnostics/status", &json!({
                        "requestId":payload["requestId"], "ok":true, "boardDeviceId":BOARD,
                        "runtime":{"audioPlaybackActive":playback}
                    }));
                }
                (Mode::Audio { state, .. }, "media/query") => {
                    assert!(self.device_waiters.lock().unwrap().iter().any(|w|
                        w.request_id == payload["requestId"].as_str().unwrap()
                            && w.response_topic == "media/status"));
                    let reply = json!({"requestId":payload["requestId"],"ok":true,
                        "state":state,"sessionId":if state=="idle" {""} else {"music-test"}});
                    if let Some(deferred) = &self.deferred_media {
                        // A separate local reader changes the connection under its
                        // real mutex before delivering this request-correlated reply.
                        deferred.send(reply).expect("local reader remains alive");
                    } else {
                        resolve_device_response(&self.device_waiters, "media/status", &reply);
                    }
                }
                (Mode::MissingChunkAck | Mode::InvalidChunkAck(_), "widget/begin") => {
                    assert!(self.widget_waiters.lock().unwrap().iter().any(|w|
                        w.transfer_id == payload["transferId"].as_str().unwrap() && w.phase == "begin"));
                    resolve_widget_ack(&self.widget_waiters,"widget-install-ack",&json!({
                        "transferId":payload["transferId"],"phase":"begin","ok":true
                    }));
                }
                (Mode::MissingChunkAck, "widget/chunk") => {
                    let index: u32 = payload["index"].as_str().unwrap().parse().unwrap();
                    assert!(self.widget_waiters.lock().unwrap().iter().any(|w|
                        w.transfer_id == payload["transferId"].as_str().unwrap() && w.phase == "chunk"
                            && w.path.as_deref() == payload["path"].as_str() && w.index == Some(index)));
                    // Deliberately retain the real sender/waiter and send NOTHING.
                    // This must take recv_timeout(2s), not a disconnected channel
                    // or installer_acceptance's single-attempt override.
                }
                (Mode::InvalidChunkAck(kind), "widget/chunk") => {
                    let mut ack=json!({"transferId":payload["transferId"],"phase":"chunk",
                        "path":payload["path"],"index":payload["index"],"msg":"widget chunk metadata conflict"});
                    match kind {0=>{},1=>ack["ok"]=Value::Null,2=>ack["ok"]=json!("false"),
                        3=>ack["ok"]=json!("true"),4=>ack["ok"]=json!(0),_=>ack["ok"]=json!(false)}
                    resolve_widget_ack(&self.widget_waiters,"widget-install-ack",&ack);
                }
                (Mode::DeleteWriteRejected, "widget/delete") => {
                    // Record a physical write, then stop the old implementation
                    // without a synthetic ACK or the real ACK timeout. The fixed
                    // generation-bound send must never reach this writer.
                    return Err(std::io::Error::new(std::io::ErrorKind::BrokenPipe,
                        "memory fixture: delete reached the replacement connection"));
                }
                (Mode::DeleteAck, "widget/delete") => {
                    resolve_widget_ack(&self.widget_waiters,"widget-install-ack",&json!({
                        "transferId":payload["transferId"],"phase":"delete","ok":true
                    }));
                }
                _ => panic!("unexpected mutation/topic after preflight or unknown ACK: {topic}"),
            }
        }
        Ok(bytes.len())
    }
    fn flush(&mut self) -> std::io::Result<()> { Ok(()) }
}
fn manager(mode: Mode, deferred_media: Option<mpsc::Sender<Value>>, preserve: bool)
    -> (UsbSerialManager, Arc<Mutex<Trace>>, Arc<AtomicBool>) {
    let manager = UsbSerialManager::new();
    let trace = Arc::new(Mutex::new(Trace::default()));
    let capture = Arc::new(AtomicBool::new(false));
    *manager.connection.lock().unwrap() = Some(UsbConnection {
        connection_id:101,port_name:"memory-only-no-serial-port".into(),baud_rate:115_200,
        writer:Box::new(MemoryWriter { pending:Vec::new(),trace:trace.clone(),
            device_waiters:manager.device_response_waiters.clone(),
            widget_waiters:manager.widget_ack_waiters.clone(),deferred_media,mode }),
        board_device_id:BOARD.into(),runtime:"esp-p4".into(),device_model:"test-only".into(),
        firmware:String::new(),build_id:String::new(),git_sha:String::new(),build_dirty:false,
        protocol_schema:7,wire_protocol:"pet-usb-jsonl-v3".into(),
        capabilities:json!({"widgetMedia":"p4-media-v1","widgetDelete":true,
            "widgetDeletePreserveSave":preserve,"debugBootId":"0123456789abcdef"}),
        connected:true,cancel_reader:Arc::new(AtomicBool::new(false)),capture_active:capture.clone(),
    });
manager.activate_memory_test_connection();
    (manager, trace, capture)
}
fn topics(trace: &Arc<Mutex<Trace>>) -> Vec<String> {
    trace.lock().unwrap().requests.iter().map(|r|r["topic"].as_str().unwrap().to_owned()).collect()
}
fn assert_audio_case(playback: bool, state: &'static str, blocked: bool) {
    let (manager,trace,_) = manager(Mode::Audio { playback,state },None,true);
    let result = manager.component_sync_audio_status(BOARD).expect("real preflight reply");
    assert_eq!(result["blocked"],json!(blocked),"{state}: {result}");
    assert_eq!(result["reason"],if blocked {"playback"} else {"ready"});
    assert_eq!(topics(&trace),["diagnostics/query","media/query"]);
    assert!(manager.device_response_waiters.lock().unwrap().is_empty());
}
#[test]
fn production_audio_capture_blocks_without_queries_or_stopping_voice() {
    let (manager,trace,capture) = manager(Mode::Audio { playback:false,state:"idle" },None,true);
    capture.store(true,Ordering::Release);
    let result = manager.component_sync_audio_status(BOARD).unwrap();
    assert_eq!(result["blocked"],true);assert_eq!(result["reason"],"voice");
    assert!(capture.load(Ordering::Acquire));assert!(topics(&trace).is_empty());
    assert!(manager.device_response_waiters.lock().unwrap().is_empty());
}
#[test]
fn production_audio_playing_blocks_before_any_widget_mutation() {
    assert_audio_case(false,"playing",true);
}
#[test]
fn production_audio_ended_music_lease_still_blocks() {
    assert_audio_case(false,"ended",true);
}
#[test]
fn production_audio_idle_allows_without_any_widget_mutation() {
    assert_audio_case(false,"idle",false);
}
#[test]
fn production_audio_native_playback_flag_blocks_even_with_idle_media_view() {
    assert_audio_case(true,"idle",true);
}
#[test]
fn production_audio_reply_after_connection_generation_change_is_rejected() {
    let (send,receive) = mpsc::channel();
    let (manager,trace,_) = manager(Mode::Audio { playback:false,state:"idle" },Some(send),true);
    let connection = manager.connection.clone();
    let waiters = manager.device_response_waiters.clone();
    let reader = std::thread::spawn(move || {
        let reply = receive.recv_timeout(Duration::from_secs(5)).expect("actual media query");
        // Writer holds this same lock while emitting the frame. Waiting here
        // avoids re-entrant deadlock and guarantees generation changes before ACK.
        connection.lock().unwrap().as_mut().unwrap().connection_id = 102;
        resolve_device_response(&waiters,"media/status",&reply);
    });
    let result = manager.component_sync_audio_status(BOARD);
    reader.join().unwrap();
    assert!(result.unwrap_err().contains("设备连接已变化"));
    assert_eq!(topics(&trace),["diagnostics/query","media/query"]);
    assert!(manager.device_response_waiters.lock().unwrap().is_empty());
}
fn minimal_widget() -> tempfile::TempDir {
    let directory = tempfile::tempdir().unwrap();
    std::fs::create_dir(directory.path().join("runtime")).unwrap();
    std::fs::create_dir(directory.path().join("assets")).unwrap();
    let files = [
        ("component.json",json!({"id":WIDGET,"name":"Sync safety","version":"1.0.0",
            "kind":"tool","description":"Count one bounded value using the primary button."})),
        ("negative-screen.json",json!({"dashboard":{"title":"Sync safety"}})),
        ("share.json",json!({"title":"Sync safety"})),
        ("buttons.json",json!([{"action":"counter.add","control":"SW1","event":"button.sw1.short_press","label":"Add"}])),
        ("runtime/widget.json",json!({"schema_version":1,"engine":"p4-bounded-runtime-v4",
            "vars":{"count":{"type":"int","init":0}},
            "states":["idle"],"initial_state":"idle",
            "transitions":[{"from":"idle","on":"counter.add","to":"idle","inc":{"count":1}}],
            "tick":[],"dashboard":{"title":"Sync safety","metricValue":{"var":"count"}}})),
    ];
    for (path,value) in files {std::fs::write(directory.path().join(path),serde_json::to_vec(&value).unwrap()).unwrap();}
    std::fs::write(directory.path().join("assets/.keep"),b"").unwrap();
    // The real package validator must agree this is a complete legal package;
    // don't rely on the old probe's permissive runtime={} shortcut.
    let valid = crate::clawpkg::validate_clawpkg_at_path(directory.path()).unwrap();
    assert!(valid.ok,"minimal widget must validate: {:?}",valid.errors);
    directory
}
#[test]
fn production_install_lost_chunk_ack_waits_two_seconds_without_replay_commit_or_abort() {
    assert_eq!(WIDGET_CHUNK_ACK_TIMEOUT,Duration::from_secs(2));
    assert_eq!(WIDGET_CHUNK_MAX_ATTEMPTS,3,"exercise normal production policy, not a one-attempt fixture");
    let directory = minimal_widget();
    let (manager,trace,_) = manager(Mode::MissingChunkAck,None,true);
    let error = manager.install_widget_clawpkg_on_board(BOARD,WIDGET,directory.path(),&HashMap::new(),|_,_,_|{})
        .expect_err("the missing chunk ACK cannot report installation success");
    assert!(error.contains("未确认") && error.contains("不会自动重发"),"{error}");
    let elapsed = trace.lock().unwrap().first_chunk.expect("real chunk was sent").elapsed();
    assert!(elapsed>=WIDGET_CHUNK_ACK_TIMEOUT,"must await the real 2s timeout, not disconnect a waiter: {elapsed:?}");
    assert_eq!(topics(&trace),["widget/begin","widget/chunk"],"no retry, commit or abort on unknown outcome");
    let trace_guard = trace.lock().unwrap();
    assert_eq!(trace_guard.requests[0]["payload"]["transferId"],trace_guard.requests[1]["payload"]["transferId"]);
    drop(trace_guard);
    assert!(manager.widget_ack_waiters.lock().unwrap().is_empty());
    assert!(manager.device_response_waiters.lock().unwrap().is_empty());
}
#[test]
fn production_delete_defaults_to_preserving_save_with_real_ack() {
    let (manager,trace,_) = manager(Mode::DeleteAck,None,true);
    manager.remove_widget(BOARD,WIDGET).unwrap();
    assert_eq!(topics(&trace),["widget/delete"]);
    assert_eq!(trace.lock().unwrap().requests[0]["payload"]["preserveSave"],true);
    assert!(manager.widget_ack_waiters.lock().unwrap().is_empty());
}
#[test]
fn production_delete_without_preserve_capability_sends_nothing() {
    let (manager,trace,_) = manager(Mode::DeleteAck,None,false);
    assert!(manager.remove_widget(BOARD,WIDGET).unwrap_err().contains("不支持保留存档"));
    assert!(topics(&trace).is_empty());assert!(manager.widget_ack_waiters.lock().unwrap().is_empty());
}
#[test]
fn production_delete_explicit_clear_sends_false_with_real_ack() {
    let (manager,trace,_) = manager(Mode::DeleteAck,None,false);
    // Native command independently requires the UI's explicit clear consent.
    manager.remove_widget_with_save_policy(BOARD,WIDGET,false).unwrap();
    assert_eq!(topics(&trace),["widget/delete"]);
    assert_eq!(trace.lock().unwrap().requests[0]["payload"]["preserveSave"],false);
    assert!(manager.widget_ack_waiters.lock().unwrap().is_empty());
}

#[test]
fn production_widget_malformed_or_metadata_nack_never_replays_chunk() {
    for kind in 0..=5 {
        let directory=minimal_widget();
        let (manager,trace,_)=manager(Mode::InvalidChunkAck(kind),None,true);
        let error=manager.install_widget_clawpkg_on_board(BOARD,WIDGET,directory.path(),&HashMap::new(),|_,_,_|{})
            .expect_err("invalid or rejected ACK cannot confirm install");
        if kind==5 {assert!(error.contains("metadata conflict")&&error.contains("no automatic retry"),"{error}");}
        else {assert!(error.contains("未确认")&&error.contains("不会自动重发"),"{error}");}
        assert_eq!(topics(&trace),["widget/begin","widget/chunk"],"no second chunk/commit/abort for {kind}");
        assert!(manager.widget_ack_waiters.lock().unwrap().is_empty());
        assert!(manager.inbound_owner.is_current(101));
    }
}

#[test]
fn production_delete_reconnect_after_capability_snapshot_sends_nothing() {
    let (manager,trace,_) = manager(Mode::DeleteWriteRejected,None,true);
    let wire = manager.wire_guard.lock().unwrap();
    let worker_manager = manager.clone();
    let worker = std::thread::spawn(move || worker_manager.remove_widget(BOARD,WIDGET));
    // The real waiter is registered only AFTER the capability preflight. With
    // the physical gate held, the worker cannot submit its delete yet.
    let deadline = Instant::now()+Duration::from_secs(5);
    let registered = loop {
        if manager.widget_ack_waiters.lock().unwrap().iter().any(|w|w.phase=="delete") {
            break true;
        }
        if Instant::now()>=deadline { break false; }
        std::thread::yield_now();
    };
    if !registered {
        drop(wire);
        let result=worker.join();
        panic!("delete did not reach its real post-preflight waiter: {result:?}");
    }
    {
        let mut slot = manager.connection.lock().unwrap();
        let previous = slot.as_mut().unwrap();
        assert_eq!(previous.connection_id,101);
        assert_eq!(previous.board_device_id,BOARD);
        previous.cancel_reader.store(true,Ordering::Release);
        previous.connection_id=102;
        previous.capabilities.as_object_mut().unwrap().remove("widgetDeletePreserveSave");
        previous.cancel_reader=Arc::new(AtomicBool::new(false));
    }
    // Publish the real memory fixture's new RX generation, preserving the same
    // board identity. Neither a fake ACK nor a production timing hook is used.
    manager.activate_memory_test_connection();
    drop(wire);
    let result=worker.join().expect("delete worker must finish normally");
    assert!(result.is_err(),"stale capability preflight cannot authorize a new connection");
    assert_eq!(topics(&trace),Vec::<String>::new(),"zero physical delete writes after reconnect");
    assert!(manager.widget_ack_waiters.lock().unwrap().is_empty(),"failed submission removes its waiter");
}

#[test]
fn production_stale_bound_preempting_sends_cannot_write_to_current_peer() {
    for topic in ["audio/play_begin","widget/program-stop"] {
        let (manager,trace,_) = manager(Mode::DeleteWriteRejected,None,true);
        let payload=json!({"sessionId":"memory-bound-session"});
        assert!(manager.send_inner(topic,&payload,true,Some("stale-other-board"),Some(101),None).is_err());
        assert!(manager.send_inner(topic,&payload,true,Some(BOARD),Some(100),None).is_err());
        assert!(topics(&trace).is_empty());
        assert!(manager.inbound_owner.is_current(101));
    }
}

#[test]
fn production_autostart_suppression_occurs_only_after_bound_identity_admission() {
    // Source-order guard is separate from the memory transport test above:
    // native autostart's global singleton is intentionally NOT configured here.
    let source=include_str!("../usb_serial.rs");
    let send=source.split("    fn send_inner_admitted(").nth(1).unwrap();
    let send=send.split("        // Background widgets").next().unwrap();
    assert!(!send.contains("native::preempt(self)"),"never look up/suppress the current connection before validating the bound one");
    let effect=send.find("native::preempt_rx(&conn.board_device_id,conn.connection_id)").unwrap();
    assert!(send.find("if conn.connection_id != expected_connection_id").unwrap()<effect);
    assert!(send.find("if conn.board_device_id != expected_board_device_id").unwrap()<effect);
    assert!(send.find("let preempt_on_send =").unwrap()<effect);
}
