//! Real connected-reader dispatch, memory input and an owner generation only.
//! No serial handle/probe, app directory, business send, or hardware mutation.
use super::*;
use serde_json::json;
use std::io::Cursor;

fn frame(payload: serde_json::Value) -> Vec<u8> {
    let body=serde_json::to_vec(&json!({"topic":"diagnostics/status","payload":payload})).unwrap();
    let mut raw=format!("!P4C1:{:05}:{:08x}:",body.len(),control_integrity::fingerprint(&body)).into_bytes();
    raw.extend(body);raw.push(b'\n');raw
}
fn manager() -> UsbSerialManager {
    let manager=UsbSerialManager::new();
    {let _lease=manager.inbound_owner.lock().unwrap();manager.inbound_owner.publish(1);}
    manager
}

#[test]
fn actual_connected_reader_rejects_length_and_hash_before_exact_waiter_or_callback() {
    for length_failure in [true,false] {
        let manager=manager();let (bad_tx,bad_rx)=mpsc::channel();let (good_tx,good_rx)=mpsc::channel();
        manager.device_response_waiters.lock().unwrap().extend([
            DeviceResponseWaiter {request_id:"bad-synthetic".into(),response_topic:"diagnostics/status".into(),sender:bad_tx},
            DeviceResponseWaiter {request_id:"good-synthetic".into(),response_topic:"diagnostics/status".into(),sender:good_tx},
        ]);
        let mut raw=frame(json!({"requestId":"bad-synthetic","padding":"x".repeat(512)}));
        if length_failure {
            let at=raw.windows(128).position(|w|w==[b'x';128]).unwrap();raw.drain(at..at+128);
        } else {
            let at=raw.windows(128).position(|w|w==[b'x';128]).unwrap();raw[at]=b'y';
        }
        let failure=control_integrity::decode(&raw[..raw.len()-1]).1.failure;
        assert_eq!(failure,Some(if length_failure {control_integrity::Failure::Length}else{control_integrity::Failure::Hash}));
        raw.extend(frame(json!({"requestId":"good-synthetic","ok":true})));
        let delivered=Mutex::new(vec![]);
        manager.read_connected(BufReader::with_capacity(7,Cursor::new(raw)),1,4000000,
            Arc::new(AtomicBool::new(false)),Arc::new(AtomicBool::new(false)),true,None,
            |topic,payload|delivered.lock().unwrap().push((topic,payload)));
        assert!(matches!(bad_rx.try_recv(),Err(mpsc::TryRecvError::Empty)));
        assert_eq!(good_rx.try_recv().unwrap()["requestId"],"good-synthetic");
        let seen=delivered.lock().unwrap();assert_eq!(seen.len(),1);assert_eq!(seen[0].1["requestId"],"good-synthetic");
        assert_eq!(manager.device_response_waiters.lock().unwrap().len(),1);
    }
}

#[test]
fn actual_connected_reader_pause_defers_drain_then_delivers_the_complete_frame() {
    let manager=manager();let worker=manager.clone();let (sent,received)=mpsc::channel();
    let pause=SerialReaderPause::new(&manager.serial_body_write_active);
    let reader=thread::spawn(move||worker.read_connected(Cursor::new(frame(json!({"requestId":"paused-synthetic"}))),
        1,4000000,Arc::new(AtomicBool::new(false)),Arc::new(AtomicBool::new(false)),true,None,
        move|topic,payload|{sent.send((topic,payload)).unwrap();}));
    assert!(matches!(received.recv_timeout(Duration::from_millis(20)),Err(mpsc::RecvTimeoutError::Timeout)));
    drop(pause);
    let (topic,payload)=received.recv_timeout(Duration::from_secs(2)).unwrap();
    assert_eq!(topic,"diagnostics/status");assert_eq!(payload["requestId"],"paused-synthetic");reader.join().unwrap();
}
