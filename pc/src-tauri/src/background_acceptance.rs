//! Explicit local hardware acceptance of the real installed-program methods.
//! An external reviewed runner owns installation/cleanup. This test cannot
//! install, delete, reboot, activate, record, play, launch a GUI or call a model.
use crate::{app_program_background::{Entry,Snapshot},usb_serial::UsbSerialManager};
use serde_json::{json,Value};
use std::{path::PathBuf,sync::mpsc,time::{Duration,Instant}};

struct Disconnect(UsbSerialManager);
impl Drop for Disconnect{fn drop(&mut self){self.0.disconnect();}}
fn own(state:&Snapshot,id:&str)->Entry{
    assert_eq!(state.items.len(),1,"unexpected concurrent program owner");
    let entry=state.items[0].clone();assert_eq!(entry.widget_id,id);entry
}
fn observe(usb:&UsbSerialManager,board:&str,rx:&mpsc::Receiver<(String,Value)>,topic:&str,reply:&str)->Value{
    static SEQUENCE:std::sync::atomic::AtomicU32=std::sync::atomic::AtomicU32::new(1);
    let rid=format!("background-acceptance-{}",SEQUENCE.fetch_add(1,std::sync::atomic::Ordering::Relaxed));
    // The bounded local test issues exactly one request at a time and consumes
    // the full matching response before issuing another. No request replay.
    usb.send_to_board(board,topic,&json!({"requestId":rid})).expect("single read-only request");
    let deadline=Instant::now()+Duration::from_secs(5);
    loop{
        let remaining=deadline.saturating_duration_since(Instant::now());assert!(!remaining.is_zero(),"read-only observation timed out");
        let (name,payload)=rx.recv_timeout(remaining).expect("observation reply");
        if name==reply&&(reply=="audio/status"||payload["requestId"]==rid){return payload;}
    }
}
fn inventory_model(v:&Value)->Value{
    let mut rows=v["items"].as_array().expect("inventory items").clone();
    for row in &mut rows{row.as_object_mut().expect("inventory row").remove("active");}
    rows.sort_by_key(|r|r["id"].as_str().unwrap_or("").to_owned());json!(rows)
}
#[test]
#[ignore = "explicit silent P4 controller acceptance; requires a separately installed owned fixture"]
fn local_background_controller_transport(){
    assert_ne!(std::env::var("P4_SERIAL_LOG_RAW").as_deref(),Ok("1"));
    assert_eq!(std::env::var("PET_BACKGROUND_ACCEPT_OWNED_FIXTURE").as_deref(),Ok("framework-background-cycle-check"));
    let id="framework-background-cycle-check";
    let board=std::env::var("P4_EXPECTED_BOARD_ID").expect("expected board");
    let elf=std::env::var("PET_BACKGROUND_EXPECTED_ELF").expect("expected ELF");
    assert!(elf.len()==64&&elf.bytes().all(|b|b.is_ascii_hexdigit()));
    let output=PathBuf::from(std::env::var("PET_BACKGROUND_ACCEPT_OUTPUT").expect("local result path"));
    assert!(!output.exists(),"do not overwrite earlier evidence");
    let usb=UsbSerialManager::new();let(tx,rx)=mpsc::sync_channel(128);
    let drops=std::sync::Arc::new(std::sync::atomic::AtomicU32::new(0));let callback_drops=drops.clone();
    usb.connect(&std::env::var("P4_SERIAL_PORT").expect("explicit port"),move|topic,payload|{
        if matches!(topic.as_str(),"debug/identity"|"audio/status")&&tx.try_send((topic,payload)).is_err(){
            callback_drops.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
        }
    }).expect("connect exact board");let _disconnect=Disconnect(usb.clone());
    assert_eq!(usb.status().board_device_id,board);
    let identity=observe(&usb,&board,&rx,"debug/identity","debug/identity");
    assert_eq!(identity["boardDeviceId"],board);assert_eq!(identity["appElfSha256"],elf);
    let audio=observe(&usb,&board,&rx,"audio/query","audio/status");
    for key in ["active","conversation","playbackActive","streamActive"]{assert_ne!(audio[key],true,"audio must be idle");}
    let before=usb.query_widget_inventory(&board).expect("complete inventory");
    assert_eq!(before["complete"],true);assert_eq!(before["recoveryBlocked"],false);
    assert!(before["items"].as_array().unwrap().iter().any(|i|i["id"]==id));
    let initial=usb.query_background_programs(&board).expect("read-only initial status");
    assert_eq!(identity["bootId"],initial.boot_id);assert!(initial.items.is_empty(),"never take over an existing owner");
    let admitted=usb.start_background_program(&board,id).expect("preflight/start method");
    assert!(admitted.confirmed,"unknown start is retained; NEVER resend");
    let first_state=admitted.snapshot.unwrap();let first=own(&first_state,id);assert!(first.live&&!first.stopped);
    std::thread::sleep(Duration::from_millis(4200));
    let progress_state=usb.query_background_programs(&board).expect("hidden progress");
    assert_eq!(progress_state.boot_id,initial.boot_id);let progress=own(&progress_state,id);
    assert_eq!(progress.registration,first.registration);assert!(progress.live&&progress.revision>=first.revision+2);
    let stopped=usb.stop_background_program(&board,id,&initial.boot_id,&first.registration).expect("single exact stop");
    assert!(stopped.confirmed,"unknown stop is retained; NEVER resend");
    let stopped=own(&stopped.snapshot.unwrap(),id);assert!(!stopped.live&&stopped.stopped);
    std::thread::sleep(Duration::from_secs(35));
    let steady_state=usb.query_background_programs(&board).expect("stop latch/durability observation");let steady=own(&steady_state,id);
    assert_eq!(steady_state.boot_id,initial.boot_id);assert_eq!(steady.registration,first.registration);
    assert!(steady.stopped&&!steady.live&&!steady.checkpoint_pending);assert_eq!(steady.revision,stopped.revision);
    let after=usb.query_widget_inventory(&board).expect("unchanged inventory");
    assert_eq!(inventory_model(&before),inventory_model(&after));assert_eq!(before["activeWidgetId"],after["activeWidgetId"]);
    let last=observe(&usb,&board,&rx,"debug/identity","debug/identity");
    assert_eq!(last["bootId"],identity["bootId"]);assert_eq!(last["appElfSha256"],elf);
    let audio=observe(&usb,&board,&rx,"audio/query","audio/status");
    for key in ["active","conversation","playbackActive","streamActive"]{assert_ne!(audio[key],true);}
    let drops=drops.load(std::sync::atomic::Ordering::Relaxed);assert_eq!(drops,0);
    let report=json!({"passed":true,"realClientMethods":true,"nativeInspectUsed":true,"guiStarted":false,
      "startMethodCalls":1,"stopMethodCalls":1,"mutationRetries":0,"hiddenTimerProgress":true,
      "manualStopLatched35s":true,"checkpointPendingCleared":true,"inventoryPreserved":true,"selectionPreserved":true,
      "bootUnchanged":true,"callbackDrops":drops,"audioUsed":false,"cloudCalls":0,"packaged":false,
      "maxStepUs":progress.max_step_us.max(steady.max_step_us),"maxLoadVerified":false,"acousticVerified":false});
    use std::io::Write;
    let mut file=std::fs::OpenOptions::new().write(true).create_new(true).open(output).expect("exclusive result file");
    file.write_all(serde_json::to_string_pretty(&report).unwrap().as_bytes()).unwrap();
}
