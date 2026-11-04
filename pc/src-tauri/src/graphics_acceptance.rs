//! Opt-in screenshot integrity check through the production serial reader.
//! Screenshots stay local; no microphone, speaker, cloud or application launch.
#[cfg(test)]
#[path = "capability_acceptance.rs"]
mod capability_acceptance;
use crate::usb_serial::UsbSerialManager;
use serde_json::json;
#[path = "graphics_capture_events.rs"]
mod events;
#[path = "graphics_capture_integrity.rs"]
mod integrity;
#[path = "graphics_capture_pull.rs"]
mod pull;
#[path = "graphics_capture_identity.rs"]
mod identity;
use std::{
    path::PathBuf,
    sync::{atomic::AtomicU32, mpsc, Arc},
    time::{Duration, Instant},
};
struct ProductionPull<'a> {
    usb: &'a UsbSerialManager,
    board: &'a str,
    rx: &'a mpsc::Receiver<(String, serde_json::Value)>,
    started: Instant,
}
impl pull::Transport for ProductionPull<'_> {
    fn elapsed(&self) -> Duration {
        self.started.elapsed()
    }
    fn send(&mut self, topic: &str, payload: &serde_json::Value) -> Result<(), ()> {
        self.usb
            .send_to_board(self.board, topic, payload)
            .map_err(|_| ())
    }
    fn receive(&mut self, timeout: Duration) -> Result<Option<(String, serde_json::Value)>, ()> {
        match self.rx.recv_timeout(timeout) {
            Ok(message) => Ok(Some(message)),
            Err(mpsc::RecvTimeoutError::Timeout) => Ok(None),
            Err(mpsc::RecvTimeoutError::Disconnected) => Err(()),
        }
    }
    fn delay(&mut self, duration: Duration) {
        std::thread::sleep(duration);
    }
    fn close_ack_supported(&self) -> bool {
        self.usb.status().capabilities["screenshotCloseAck"]=="p4-snapshot-close-v1"
    }
}
struct Disconnect(UsbSerialManager);
impl Drop for Disconnect {
    fn drop(&mut self) {
        self.0.disconnect();
    }
}
fn diagnostic_rounds(value: Option<&str>) -> Result<usize, &'static str> {
    let Some(value) = value else { return Ok(0); };
    if value.is_empty() || value.len() > 3 || !value.bytes().all(|b| b.is_ascii_digit()) {
        return Err("invalid diagnostic round count");
    }
    let count = value.parse::<usize>().map_err(|_| "invalid diagnostic round count")?;
    if !(1..=600).contains(&count) || count.to_string() != value {
        return Err("invalid diagnostic round count");
    }
    Ok(count)
}
#[test]
fn diagnostic_round_count_is_opt_in_bounded_and_canonical() {
    assert_eq!(diagnostic_rounds(None), Ok(0));
    assert_eq!(diagnostic_rounds(Some("1")), Ok(1));
    assert_eq!(diagnostic_rounds(Some("600")), Ok(600));
    for value in ["", "0", "601", "9999", "+1", " 1", "1.0", "01", "true", "secret"] {
        assert!(diagnostic_rounds(Some(value)).is_err());
    }
}
#[test]
#[ignore = "Explicit connected-device screenshot; stores local PNG and integrity metadata only"]
fn local_graphics_production_transport() {
    assert_ne!(std::env::var("P4_SERIAL_LOG_RAW").as_deref(), Ok("1"));
    let output = PathBuf::from(std::env::var("PET_GRAPHICS_OUTPUT").unwrap());
    let rounds = diagnostic_rounds(std::env::var("PET_GRAPHICS_DIAGNOSTIC_ROUNDS").ok().as_deref()).unwrap();
    assert!(rounds == 0 || std::env::var_os("PET_GRAPHICS_ACTIVATE").is_none(),
        "diagnostic stress cannot activate a component");
    let usb = UsbSerialManager::new();
    usb.set_desktop_device_id("pet-maintenance-hil");
    // Never block the production serial reader, nor grow a queue without bound.
    let (tx, rx) = mpsc::sync_channel(events::QUEUE_CAPACITY);
    let dropped = Arc::new(AtomicU32::new(0));
    let callback_dropped = dropped.clone();
    usb.connect(
        &std::env::var("P4_SERIAL_PORT").unwrap(),
        move |topic, payload| {
            events::forward(&tx, &callback_dropped, topic, payload);
        },
    )
    .unwrap();
    let _disconnect = Disconnect(usb.clone());
    let board = usb.status().board_device_id;
    assert_eq!(board, std::env::var("P4_EXPECTED_BOARD_ID").unwrap());
    usb.send_to_board(&board, "audio/query", &json!({}))
        .unwrap();
    let limit = Instant::now() + Duration::from_secs(3);
    loop {
        assert!(Instant::now() < limit, "audio idle status not received");
        if let Ok((topic, p)) = rx.recv_timeout(Duration::from_millis(20)) {
            if topic == "audio/status" {
                assert_eq!(p["active"], false);
                assert_eq!(p["playbackActive"], false);
                assert_eq!(p["conversation"], false);
                break;
            }
        }
    }
    let mut navigation_acked = false;
    if let Ok(widget) = std::env::var("PET_GRAPHICS_ACTIVATE") {
        let inventory = usb.query_widget_inventory(&board).unwrap();
        assert!(inventory["items"]
            .as_array()
            .unwrap()
            .iter()
            .any(|item| item["id"] == widget));
        usb.activate_installed_widget(&board, &widget, true)
            .unwrap();
        navigation_acked = true;
        std::thread::sleep(Duration::from_millis(300));
    }
    let identity_supported=usb.status().capabilities["debugEvidenceIdentity"]=="p4-evidence-identity-v1";
    let expected_elf=std::env::var("PET_GRAPHICS_EXPECTED_ELF_SHA256").ok();
    let require_identity=std::env::var("PET_GRAPHICS_REQUIRE_IDENTITY").as_deref()==Ok("1");
    let before=identity_supported.then(||{
        let request=uuid::Uuid::new_v4().simple().to_string();
        let mut transport=ProductionPull{usb:&usb,board:&board,rx:&rx,started:Instant::now()};
        identity::query(&mut transport,&request,&board,None)
    });
    // Explicit query-only stress uses the actual live reader and exact waiter
    // matching; it never resends a failed request or enables raw logging. The
    // separate checkpoint survives a timeout before screenshot acquisition.
    let transport_probe_path = output.with_extension("transport.json");
    if rounds > 0 {
        // Explicit private test output: rejection metadata, never raw payloads.
        crate::usb_serial::configure_acceptance_transfer_log(&output.with_extension("transport-evidence"));
    }
    let guarded_control=usb.status().capabilities["serialControlIntegrity"].as_str()==Some("p4-control-fnv1a32-v1");
    let mut transport_probe = json!({"status":if rounds==0 {"not_requested"} else {"running"},
        "requested":rounds,"sent":0,"matched":0,"queryRetries":0,"businessTasksSent":0,
        "audioUsed":false,"cloudCalls":0,"reader":"production-rust-bufreader",
        "rawIntegrityConfirmed":false,"controlFrameIntegrityRequired":guarded_control,
        "controlQueriesIntegrityConfirmed":false,"maxQueryUs":0});
    let expected_firmware = usb.status().firmware;
    let mut previous_uptime = None;
    for index in 0..rounds {
        transport_probe["sent"] = json!(index+1);
        let started = Instant::now();
        let response = usb.query_diagnostics(&board);
        let valid = response.as_ref().ok().filter(|value| {
            value["boardDeviceId"].as_str() == Some(board.as_str())
                && value["runtime"]["firmware"].as_str() == Some(expected_firmware.as_str())
                && value["runtime"]["uptimeMs"].as_u64().is_some_and(|now| previous_uptime.is_none_or(|old| now >= old))
        });
        let elapsed = started.elapsed().as_micros().min(u64::MAX as u128) as u64;
        transport_probe["maxQueryUs"] = json!(elapsed.max(transport_probe["maxQueryUs"].as_u64().unwrap()));
        if let Some(value) = valid {
            previous_uptime = value["runtime"]["uptimeMs"].as_u64();
            transport_probe["matched"] = json!(index+1);
        } else {
            transport_probe["status"] = json!("failed");
            transport_probe["failure"] = json!(if response.is_err() {"query_unavailable"} else {"identity_or_clock_mismatch"});
            // Preserve the original failure. A DIFFERENT read-only identity
            // request may distinguish a reset from an unavailable control
            // reply; it never makes this failed window pass or replays work.
            if let Some(Ok(first))=&before {
                let request=uuid::Uuid::new_v4().simple().to_string();
                let mut transport=ProductionPull{usb:&usb,board:&board,rx:&rx,started:Instant::now()};
                transport_probe["failureContinuity"]=match identity::query(&mut transport,&request,&board,None){
                    Ok(after)=>identity::failure_continuity(first,&after),
                    Err(error)=>json!({"freshReadOnlyObservation":true,"available":false,"error":error})
                };
            }
            std::fs::write(&transport_probe_path, serde_json::to_vec_pretty(&transport_probe).unwrap()).unwrap();
            panic!("production diagnostic request failed; no request was repeated, checkpoint saved");
        }
        if (index+1)%50==0 {
            std::fs::write(&transport_probe_path, serde_json::to_vec_pretty(&transport_probe).unwrap()).unwrap();
            println!("production read-only diagnostics matched {}/{}", index+1, rounds);
        }
        std::thread::sleep(Duration::from_millis(700));
    }
    if rounds > 0 {
        transport_probe["status"] = json!("matched");
        // Scoped to these matched downstream CONTROL replies. Audio/media,
        // upstream requests and snapshot transfer retain their own contracts.
        transport_probe["controlQueriesIntegrityConfirmed"] = json!(guarded_control);
        std::fs::write(&transport_probe_path, serde_json::to_vec_pretty(&transport_probe).unwrap()).unwrap();
    }
    // No implicit fallback: legacy push is an explicit comparison mode only.
    let legacy = std::env::var("PET_GRAPHICS_LEGACY").as_deref() == Ok("1");
    let (mut capture, pull_diagnostics) = if legacy {
        usb.send_to_board(&board, "debug/screenshot", &json!({"fullResolution":true}))
            .unwrap();
        let mut capture = integrity::Capture::new(640, 480);
        let mut limit = Instant::now() + Duration::from_secs(15);
        while Instant::now() < limit {
            let Ok((topic, p)) = rx.recv_timeout(Duration::from_millis(20)) else {
                continue;
            };
            let terminal = matches!(
                topic.as_str(),
                "debug/screenshot_end" | "debug/screenshot_error"
            );
            capture.observe(&topic, p);
            // A final-line drain can fail after its bytes were queued.
            if terminal {
                limit = limit.min(Instant::now() + Duration::from_millis(250));
            }
        }
        (capture, json!({"mode":"legacy-push","autoRecaptures":0}))
    } else if usb.status().capabilities["screenshotTransfer"] == "pull-fnv1a32-v1" {
        let request_id = uuid::Uuid::new_v4().simple().to_string();
        let mut transport = ProductionPull {
            usb: &usb,
            board: &board,
            rx: &rx,
            started: Instant::now(),
        };
        let result = pull::capture(&mut transport, 640, 480, &request_id);
        (result.capture, result.diagnostics)
    } else {
        let mut capture = integrity::Capture::new(640, 480);
        capture.error(json!({"kind":"missing_pull_capability"}));
        (capture, json!({"mode":"pull-v1","autoRecaptures":0}))
    };
    if capture.end.is_none() {
        capture.error(json!({"kind":"missing_end_or_timeout"}));
    }
    let identity_binding=if let Some(before)=before{
        let request=uuid::Uuid::new_v4().simple().to_string();
        let mut transport=ProductionPull{usb:&usb,board:&board,rx:&rx,started:Instant::now()};
        let after=identity::query(&mut transport,&request,&board,Some(&mut capture));
        match before.and_then(|before|after.and_then(|after|identity::bind(&before,&after,&capture.report(),expected_elf.as_deref()))){
            Ok(binding)=>binding,
            Err(error)=>{capture.error(error.clone());json!({"status":"failed","error":error})}
        }
    }else{json!({"status":"unsupported","bootBound":false,"sourceBound":false})};
    if require_identity&&identity_binding["sourceBound"]!=true{capture.error(json!({"kind":"identity_source_gate_pending"}));}
    let capabilities = usb.status().capabilities;
    // disconnect() requests cancellation but does not join the serial reader.
    // Do not report success until the callback's sender has actually exited.
    usb.disconnect();
    let queue_drops = events::finalize(&mut capture, &rx, &dropped, events::CLOSE_BUDGET);
    // Preserve the received bytes even on failed integrity; this is forensic
    // RGB565 data, never presented as a verified PNG or automatically retried.
    std::fs::write(output.with_extension("rgb565le"), &capture.raw).unwrap();
    let report=capture.save_report(&output.with_extension("json"),json!({"navigationAcked":navigation_acked,"capabilities":capabilities,"cloudCalls":0,"audioUsed":false,"pull":pull_diagnostics,"eventQueueDrops":queue_drops,"receivedRawPreserved":true,"identity":identity_binding,"transportProbe":transport_probe}));
    assert_eq!(
        report["passed"], true,
        "screenshot integrity failed: {report}"
    );
    let mut rgb = Vec::with_capacity(640 * 480 * 3);
    for word in capture.raw.chunks_exact(2) {
        let pixel = u16::from_le_bytes([word[0], word[1]]);
        let (r, g, b) = ((pixel >> 11) & 31, (pixel >> 5) & 63, pixel & 31);
        rgb.extend_from_slice(&[
            ((r << 3) | (r >> 2)) as u8,
            ((g << 2) | (g >> 4)) as u8,
            ((b << 3) | (b >> 2)) as u8,
        ]);
    }
    image::save_buffer(
        output.with_extension("png"),
        &rgb,
        640,
        480,
        image::ColorType::Rgb8,
    )
    .unwrap();
}
