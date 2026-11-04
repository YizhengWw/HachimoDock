// Explicit isolated production uploader acceptance, never an automatic test.
// Only one independently authored silent fixture; never a user's library/store.
#[test]
#[ignore = "Explicit quiet device acceptance: install/remove ONE owned fixture preserving saves; no audio or task replay"]
fn framework_current_install_hardware() {
    use super::*;
    use serde_json::{json, Value};
    assert_eq!(std::env::var("PET_FRAMEWORK_INSTALL_ACCEPTANCE").as_deref(), Ok("1"));
    assert_ne!(std::env::var("P4_SERIAL_LOG_RAW").as_deref(), Ok("1"));
    const OWN: &str = "framework-background-budget-check";
    let fixture=PathBuf::from(std::env::var("PET_FRAMEWORK_INSTALL_FIXTURE").unwrap());
    let output=PathBuf::from(std::env::var("PET_FRAMEWORK_INSTALL_REPORT").unwrap());
    let board=std::env::var("P4_EXPECTED_BOARD_ID").unwrap();
    let expected_boot=std::env::var("PET_FRAMEWORK_EXPECTED_BOOT").unwrap();
    let expected_elf=std::env::var("PET_FRAMEWORK_EXPECTED_ELF").unwrap();
    let expected_firmware=std::env::var("PET_FRAMEWORK_EXPECTED_FIRMWARE").unwrap();
    // Exact input fingerprint, including paths/lengths. No arbitrary files,
    // symlinks, extensions, audio/media providers or ambient credentials.
    let allowed=["assets/.keep","buttons.json","component.json","negative-screen.json","runtime/widget.json","share.json"];
    fn files(root:&Path,dir:&Path,out:&mut Vec<String>){
        for entry in std::fs::read_dir(dir).unwrap(){
            let entry=entry.unwrap();let kind=entry.file_type().unwrap();assert!(!kind.is_symlink());
            let path=entry.path();if kind.is_dir(){files(root,&path,out);}else{
                assert!(kind.is_file());out.push(path.strip_prefix(root).unwrap().to_str().unwrap().replace('\\',"/"));
            }
        }
    }
    let mut found=Vec::new();files(&fixture,&fixture,&mut found);found.sort();assert_eq!(found,allowed);
    let mut hash=Sha256::new();
    for path in allowed {let bytes=std::fs::read(fixture.join(path)).unwrap();assert!(bytes.len()<=16384);
        hash.update(path.as_bytes());hash.update([0]);hash.update((bytes.len() as u64).to_le_bytes());hash.update(&bytes);}
    assert_eq!(format!("{:x}",hash.finalize()),std::env::var("PET_FRAMEWORK_FIXTURE_SHA256").unwrap());
    let manifest:Value=serde_json::from_slice(&std::fs::read(fixture.join("component.json")).unwrap()).unwrap();
    assert_eq!(manifest["id"],OWN);
    let runtime:Value=serde_json::from_slice(&std::fs::read(fixture.join("runtime/widget.json")).unwrap()).unwrap();
    assert!(runtime["program"].is_object());
    for forbidden in ["media","instrument","data","scene","game","host","local"]{assert!(runtime.get(forbidden).is_none());}
    let mut report=json!({"phase":"prepared","passed":false,"mutationRetries":0,
        "commitOutcomeUnknown":false,"audioUsed":false,"cloudCalls":0,"packaged":false,
        "scope":"actual production installer and exact owned fixture; not GUI or acoustic acceptance"});
    // Refuse overwriting an old receipt: accidentally repeating a run cannot
    // silently discard the only evidence of an unknown mutation outcome.
    let mut receipt=std::fs::OpenOptions::new().write(true).create_new(true).open(&output).unwrap();
    let save=|receipt:&mut std::fs::File,value:&Value|{
        use std::io::{Seek,SeekFrom};receipt.seek(SeekFrom::Start(0)).unwrap();
        let bytes=serde_json::to_vec_pretty(value).unwrap();receipt.write_all(&bytes).unwrap();receipt.set_len(bytes.len() as u64).unwrap();receipt.sync_all().unwrap();
    };
    save(&mut receipt,&report);
    let usb=UsbSerialManager::new();let(tx,rx)=mpsc::sync_channel(8);
    let initial_tx=tx.clone();
    usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|topic,payload|{
        if topic=="audio/status"{let _=initial_tx.try_send(payload);}
    }).unwrap();
    struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}
    let _close=Close(usb.clone());assert_eq!(usb.status().board_device_id,board);
    let identity=usb.send_device_request_and_wait(&board,"debug/identity","debug/identity").unwrap();
    assert_eq!(identity["bootId"],expected_boot);assert_eq!(identity["appElfSha256"],expected_elf);
    let before=usb.query_diagnostics(&board).unwrap();assert_eq!(before["runtime"]["firmware"],expected_firmware);
    assert_eq!(before["runtime"]["screenPage"],"main");assert_eq!(before["runtime"]["agentState"],"idle");
    assert_eq!(before["runtime"]["sessionQueueCount"],0);
    usb.send_to_board(&board,"audio/query",&json!({})).unwrap();
    let audio=rx.recv_timeout(Duration::from_secs(3)).unwrap();
    for key in ["active","playbackActive","conversation"]{assert_eq!(audio[key],false);}
    let inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["complete"],true);
    let original=inventory["items"].as_array().unwrap();
    assert!(!original.iter().any(|i|i["id"]==OWN||i["id"]=="framework-mixed-app-check"));
    assert!(original.len()<inventory["maxInstalled"].as_u64().unwrap() as usize);
    let prior=inventory["activeWidgetId"].as_str().unwrap().to_owned();assert!(!prior.is_empty());
    let normalized=|items:&[Value]|items.iter().filter(|i|i["id"]!=OWN).map(|i|{
        let mut i=i.clone();i.as_object_mut().unwrap().remove("active");i
    }).collect::<Vec<_>>();
    report["phase"]="installing".into();report["commitOutcomeUnknown"]=true.into();save(&mut receipt,&report);
    let started=Instant::now();
    let result=usb.install_widget_clawpkg_on_board(&board,OWN,&fixture,&HashMap::new(),|_,_,_|{}).unwrap();
    report["installWallUs"]=(started.elapsed().as_micros() as u64).into();
    report["fileCount"]=result.0.into();report["sentBase64Bytes"]=result.1.into();
    report["commitOutcomeUnknown"]=false.into();report["phase"]="installed".into();save(&mut receipt,&report);
    let installed=usb.query_widget_inventory(&board).unwrap();assert_eq!(installed["complete"],true);
    let items=installed["items"].as_array().unwrap();assert_eq!(items.len(),original.len()+1);
    assert_eq!(normalized(items),normalized(original));
    assert!(items.iter().any(|i|i["id"]==OWN&&i["resourceState"]=="ready"));
    if std::env::var("PET_FRAMEWORK_INSTALL_IO_COMPLETED").as_deref()==Ok("1") {
        // Read immutable numeric counts BEFORE cleanup replaces this native
        // protocol caller's last completed generation. No IO/window restart.
        let io=usb.send_device_payload_and_wait(&board,"debug/storage_io_completed","debug/storage_io_result",
            json!({"bootId":expected_boot,"owner":3,"generation":0,"actor":3})).unwrap();
        assert_eq!(io["ok"],true);assert_eq!(io["complete"],true);assert_eq!(io["flags"],0);
        assert_eq!(io["bootId"],expected_boot);assert_eq!(io["engine"],"p4-storage-completed-v1");
        assert_eq!(io["operations"].as_array().unwrap().len(),3);
        report["completedProtocolIo"]=io;save(&mut receipt,&report);
    }
    if std::env::var("PET_FRAMEWORK_INSTALL_BACKGROUND_LIFECYCLE").as_deref()==Ok("1") {
        assert_eq!(runtime["program"]["timers"][0]["delayMs"],1500);
        assert_eq!(runtime["program"]["timers"][0]["intervalMs"],20000);
        // Explicit extra physical acceptance, NEVER part of ordinary tests or
        // the short installer timing. Start/stop are submitted once only.
        let baseline=usb.query_background_programs(&board).unwrap();
        assert_eq!(baseline.boot_id,expected_boot);assert!(baseline.items.is_empty());
        report["phase"]="background-starting".into();report["backgroundOutcomeUnknown"]=true.into();save(&mut receipt,&report);
        let started=usb.start_background_program(&board,OWN).unwrap();assert!(started.confirmed);
        let started_state=started.snapshot.unwrap();assert_eq!(started_state.boot_id,expected_boot);
        let owner=started_state.items.iter().find(|i|i.widget_id==OWN&&i.live&&!i.stopped).unwrap().clone();
        report["backgroundOutcomeUnknown"]=false.into();report["phase"]="background-running-with-pc-disconnected".into();save(&mut receipt,&report);
        usb.disconnect();std::thread::sleep(Duration::from_secs(24));
        let resumed_tx=tx.clone();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|topic,payload|{
            if topic=="audio/status"{let _=resumed_tx.try_send(payload);}
        }).unwrap();
        let progressed=usb.query_background_programs(&board).unwrap();assert_eq!(progressed.boot_id,expected_boot);
        let current=progressed.items.iter().find(|i|i.widget_id==OWN).unwrap();
        assert!(current.live&&!current.stopped&&current.revision>owner.revision);assert_eq!(current.registration,owner.registration);
        report["backgroundProgressedWithoutPcConnection"]=true.into();report["phase"]="background-stopping".into();
        report["backgroundOutcomeUnknown"]=true.into();save(&mut receipt,&report);
        let stopped=usb.stop_background_program(&board,OWN,&expected_boot,&owner.registration).unwrap();assert!(stopped.confirmed);
        let stopped_state=stopped.snapshot.unwrap();let final_owner=stopped_state.items.iter().find(|i|i.widget_id==OWN).unwrap().clone();
        assert!(!final_owner.live&&final_owner.stopped&&final_owner.registration==owner.registration);
        report["backgroundOutcomeUnknown"]=false.into();report["phase"]="background-stop-latch-pc-disconnected".into();save(&mut receipt,&report);
        usb.disconnect();std::thread::sleep(Duration::from_secs(35));
        let final_tx=tx.clone();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|topic,payload|{
            if topic=="audio/status"{let _=final_tx.try_send(payload);}
        }).unwrap();
        let stable=usb.query_background_programs(&board).unwrap();assert_eq!(stable.boot_id,expected_boot);
        let stable_owner=stable.items.iter().find(|i|i.widget_id==OWN).unwrap();
        assert!(stable_owner.stopped&&!stable_owner.live&&!stable_owner.checkpoint_pending);
        assert_eq!(stable_owner.registration,owner.registration);assert_eq!(stable_owner.revision,final_owner.revision);
        report["actualRustStartAndStopConfirmed"]=true.into();report["backgroundStopLatchedWithoutPcConnection35s"]=true.into();
        report["phase"]="background-lifecycle-verified".into();save(&mut receipt,&report);
    }
    let identity_after=usb.send_device_request_and_wait(&board,"debug/identity","debug/identity").unwrap();
    assert_eq!(identity_after["bootId"],expected_boot);assert_eq!(identity_after["appElfSha256"],expected_elf);
    report["phase"]="cleanup-owned-only".into();save(&mut receipt,&report);
    usb.remove_widget_with_save_policy(&board,OWN,true).unwrap();
    usb.activate_installed_widget(&board,&prior,false).unwrap();
    let after_inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(after_inventory["complete"],true);
    assert_eq!(after_inventory["activeWidgetId"],prior);
    assert_eq!(normalized(after_inventory["items"].as_array().unwrap()),normalized(original));
    assert!(!after_inventory["items"].as_array().unwrap().iter().any(|i|i["id"]==OWN));
    let after=usb.query_diagnostics(&board).unwrap();assert_eq!(after["faultResetCount"],before["faultResetCount"]);
    assert_eq!(after["runtime"]["screenPage"],"main");
    report["phase"]="verified".into();report["passed"]=true.into();report["originalInventoryAndOrderPreserved"]=true.into();
    report["selectionPreserved"]=true.into();report["fixtureSavePreserved"]=true.into();report["noNewFaultReset"]=true.into();
    save(&mut receipt,&report);
}

// Explicit read-only establishment, before a diagnostic Python client starts.
// Runs the ACTUAL production probe, nonce validation, buffered handoff and live
// reader. No fixture, user store, task, capture, playback or business retry.
#[test]
#[ignore = "Explicit quiet device control-negotiation acceptance; read-only apart from normal connection handshake"]
fn framework_current_control_session_hardware() {
    use super::*;
    use serde_json::json;
    assert_eq!(std::env::var("PET_FRAMEWORK_CONTROL_ACCEPTANCE").as_deref(),Ok("1"));
    assert_ne!(std::env::var("P4_SERIAL_LOG_RAW").as_deref(),Ok("1"));
    let board=std::env::var("P4_EXPECTED_BOARD_ID").unwrap();
    let boot=std::env::var("PET_FRAMEWORK_EXPECTED_BOOT").unwrap();
    let elf=std::env::var("PET_FRAMEWORK_EXPECTED_ELF").unwrap();
    let firmware=std::env::var("PET_FRAMEWORK_EXPECTED_FIRMWARE").unwrap();
    let output=PathBuf::from(std::env::var("PET_FRAMEWORK_CONTROL_REPORT").unwrap());
    let mut receipt=std::fs::OpenOptions::new().write(true).create_new(true).open(output).unwrap();
    receipt.write_all(b"{\"passed\":false,\"phase\":\"connecting\",\"businessTasks\":0,\"mutationReplay\":false}").unwrap();receipt.sync_all().unwrap();
    let usb=UsbSerialManager::new();
    usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),|_,_|{}).unwrap();
    struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}
    let _close=Close(usb.clone());let status=usb.status();assert_eq!(status.board_device_id,board);
    let hello=json!({"capabilities":status.capabilities});
    assert_eq!(hello["capabilities"]["serialControlOffer"],control_integrity::ENGINE);
    assert!(control_integrity::required_from_hello(&hello));
    assert!(control_integrity::confirmed_nonce(&hello).is_some());
    let identity=usb.send_device_request_and_wait(&board,"debug/identity","debug/identity").unwrap();
    assert_eq!(identity["bootId"],boot);assert_eq!(identity["appElfSha256"],elf);
    let diagnostics=usb.query_diagnostics(&board).unwrap();assert_eq!(diagnostics["runtime"]["firmware"],firmware);
    assert_eq!(diagnostics["runtime"]["screenPage"],"main");assert_eq!(diagnostics["runtime"]["agentState"],"idle");
    assert_eq!(diagnostics["runtime"]["sessionQueueCount"],0);
    let inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["complete"],true);
    let after=usb.send_device_request_and_wait(&board,"debug/identity","debug/identity").unwrap();
    assert_eq!(after["bootId"],boot);assert_eq!(after["appElfSha256"],elf);
    let report=json!({"passed":true,"phase":"verified","normalHandshakeOnly":true,
        "businessTasks":0,"mutationReplay":false,"audioUsed":false,"cloudCalls":0,
        "negotiatedControlConfirmed":true,"exactBootAndElfPreserved":true,"completeInventoryObserved":true,
        "noncePersistedOrLogged":false,"packaged":false,"acousticVerified":false});
    use std::io::{Seek,SeekFrom};receipt.seek(SeekFrom::Start(0)).unwrap();
    let bytes=serde_json::to_vec_pretty(&report).unwrap();receipt.write_all(&bytes).unwrap();
    receipt.set_len(bytes.len() as u64).unwrap();receipt.sync_all().unwrap();
}
