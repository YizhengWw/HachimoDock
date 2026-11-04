// Thin ignored production-reader entry. Never run by offline focused tests.
#[test]
#[ignore="Explicit parent-controlled catalog maintenance only; reviewed plan, bound reader, exclusive port and durable journal required"]
fn local_catalog_production_plan(){
    use sha2::{Digest,Sha256};
    use std::{fs::{self,File,OpenOptions},io::Read,path::{Path,PathBuf}};
    #[derive(serde::Deserialize)]#[serde(rename_all="camelCase",deny_unknown_fields)]
    struct Job{schema_version:u32,source_digest:String,reader_sha256:String,expected_board_id:String,expected_firmware:String,
        expected_elf_sha256:String,expected_elf_path:PathBuf,journal_directory:PathBuf,expected_fault_reset_count:u32,driver:DriverInput}
    fn digest(path:&Path)->Result<String,String>{
        let mut file=File::open(path).map_err(|_|"driver_artifact_unavailable")?;let mut hash=Sha256::new();let mut bytes=[0u8;65536];
        loop{let n=file.read(&mut bytes).map_err(|_|"driver_artifact_unavailable")?;if n==0{break;}hash.update(&bytes[..n]);}Ok(format!("{:x}",hash.finalize()))
    }
    fn save(path:&Path,value:&Value)->Result<(),String>{
        let mut file=OpenOptions::new().write(true).create_new(true).open(path).map_err(|_|"driver_evidence_exists_or_unavailable")?;
        file.write_all(&serde_json::to_vec_pretty(value).map_err(|_|"driver_evidence_unavailable")?).and_then(|_|file.sync_all()).map_err(|_|"driver_evidence_unavailable".into())
    }
    assert_eq!(std::env::var("PET_CATALOG_ACCEPT").as_deref(),Ok("1"));
    assert_ne!(std::env::var("P4_SERIAL_LOG_RAW").as_deref(),Ok("1"));
    let input=PathBuf::from(std::env::var("PET_CATALOG_PLAN").unwrap());let output=PathBuf::from(std::env::var("PET_CATALOG_OUTPUT").unwrap());
    let bytes=fs::read(&input).unwrap();assert!(bytes.len()<=131072);let job:Job=serde_json::from_slice(&bytes).unwrap();
    assert_eq!(job.schema_version,1);assert!(catalog::hex(&job.source_digest,64)&&catalog::hex(&job.reader_sha256,64)&&catalog::hex(&job.expected_elf_sha256,64));
    assert_eq!(std::env::var("PET_CATALOG_SOURCE_DIGEST").as_deref(),Ok(job.source_digest.as_str()));
    assert_eq!(std::env::var("P4_EXPECTED_BOARD_ID").as_deref(),Ok(job.expected_board_id.as_str()));
    assert_eq!(job.driver.expected_board_device_id,job.expected_board_id);assert!(matches!(job.driver.action.as_str(),"inspect"|"reconcile"));
    assert!(job.expected_firmware.len()<=95&&!job.expected_firmware.is_empty());
    assert_eq!(digest(&std::env::current_exe().unwrap()).unwrap(),job.reader_sha256);assert_eq!(digest(&job.expected_elf_path).unwrap(),job.expected_elf_sha256);
    #[cfg(unix)]{
        let fd=std::env::var("PET_CATALOG_LOCK_FD").unwrap().parse::<i32>().unwrap();
        // SAFETY: only validate the inherited parent-owned descriptor. Never
        // open/reacquire the parent's flock or read/write the serial device.
        assert!(fd>=3&&unsafe{libc::fcntl(fd,libc::F_GETFD)}>=0);
    }
    assert!(input.is_absolute()&&output.is_absolute()&&job.journal_directory.is_absolute());
    fs::create_dir(&output).unwrap();save(&output.join("input-evidence.json"),&serde_json::from_slice(&bytes).unwrap()).unwrap();
    let journal=if job.driver.action=="reconcile"{Some(Journal::open(&job.journal_directory).unwrap())}else{None};
    let usb=UsbSerialManager::new();usb.set_desktop_device_id("pet-maintenance-hil");let lifetime=Arc::new(());let held=lifetime.clone();
    let messages=Arc::new(Mutex::new(Vec::<Value>::new()));let captured=messages.clone();let overflow=Arc::new(AtomicBool::new(false));let dropped=overflow.clone();
    let mut report=json!({"schemaVersion":1,"passed":false,"sourceDigest":job.source_digest,"readerSha256":job.reader_sha256,
        "sourceBinding":"caller_verified_source_and_two_target_receipts; binary_and_elf_rehashed_here","readerClosed":false,"mutationRetries":0,
        "journalReset":false,"automaticCleanup":false,"snapshot":null,"outcome":null,"nativeJournal":null,"ackEvidence":null});
    let result=std::panic::catch_unwind(std::panic::AssertUnwindSafe(||->Result<(),String>{
        usb.connect(&std::env::var("P4_SERIAL_PORT").map_err(|_|"driver_explicit_port_required")?,move|topic,payload|{
            let _keep=&held;
            if matches!(topic.as_str(),"debug/identity"|"diagnostics/status"|"widget/catalog_snapshot"|"widget/catalog_reconcile_result"){
                if serde_json::to_vec(&payload).map_or(true,|p|p.len()>16384){dropped.store(true,Ordering::Release);return;}
                if let Ok(mut rows)=captured.try_lock(){if rows.len()<64{rows.push(json!({"topic":topic,"payload":payload}));}else{dropped.store(true,Ordering::Release);}}
                else{dropped.store(true,Ordering::Release);}
            }
        })?;
        let (_,generation)=usb.catalog_status(&job.expected_board_id)?;
        let id=format!("catalog-{}",uuid::Uuid::new_v4());let identity=usb.catalog_response(&job.expected_board_id,generation,"debug/identity","debug/identity",&id,&json!({"requestId":id}))?;
        if identity["engine"]!="p4-evidence-identity-v1"||identity["appElfSha256"]!=job.expected_elf_sha256
            ||!identity["bootId"].as_str().is_some_and(|s|catalog::hex(s,16))
            ||usb.status().firmware!=job.expected_firmware{return Err("driver_identity_unverified".into());}
        report["identity"]=identity.clone();
        let id=format!("catalog-{}",uuid::Uuid::new_v4());let diagnostics=usb.catalog_response(&job.expected_board_id,generation,"diagnostics/query","diagnostics/status",&id,&json!({"requestId":id}))?;
        if diagnostics["ok"]!=true||diagnostics["faultResetCount"].as_u64()!=Some(u64::from(job.expected_fault_reset_count))
            ||diagnostics["runtime"]["imageState"]!="valid"||diagnostics["runtime"]["rebootPending"]!=false{return Err("driver_runtime_unverified".into());}
        report["diagnostics"]=diagnostics;
        if overflow.load(Ordering::Acquire){return Err("driver_observation_overflow".into());}
        if let Some(request)=&job.driver.request{if identity["bootId"].as_str()!=Some(request.expected_boot_id.as_str()){return Err("driver_boot_changed".into());}}
        let value=run_catalog_maintenance_driver(&usb,&job.driver,journal.as_ref())?;
        if job.driver.action=="inspect"{report["snapshot"]=value;}
        else{
            let known=value["confirmed"]==true&&value["readOnly"]==false;report["outcome"]=value;
            if !known{return Err("driver_unknown_stop_no_followup".into());}
            // Explicit known-ACK readback, never executed after an unknown ACK.
            report["snapshot"]=serde_json::to_value(usb.catalog_inspect(&job.expected_board_id)?).map_err(|_|"driver_result_unavailable")?;
        }
        if report["snapshot"]["bootId"]!=identity["bootId"]{return Err("driver_boot_changed".into());}
        let id=format!("catalog-{}",uuid::Uuid::new_v4());let after=usb.catalog_response(&job.expected_board_id,generation,"debug/identity","debug/identity",&id,&json!({"requestId":id}))?;
        if after["bootId"]!=identity["bootId"]||after["appElfSha256"]!=identity["appElfSha256"]{return Err("driver_identity_changed".into());}
        report["identityAfter"]=after;
        Ok(())
    }));
    usb.disconnect();
    let end=Instant::now()+Duration::from_secs(5);
    while Arc::strong_count(&lifetime)>1&&Instant::now()<end{std::thread::sleep(Duration::from_millis(10));}
    let closed=Arc::strong_count(&lifetime)==1&&!usb.status().connected;drop(usb);
    let port_clear=std::process::Command::new("lsof").args(["-t","-nP",&std::env::var("P4_SERIAL_PORT").unwrap()]).output()
        .is_ok_and(|r|r.status.code()==Some(1)&&r.stdout.is_empty());
    report["readerClosed"]=json!(closed&&port_clear);report["observationOverflow"]=json!(overflow.load(Ordering::Acquire));
    let trace=output.join("ack-evidence.json");save(&trace,&json!({"engine":"production-rust-bufreader","messages":*messages.lock().unwrap()})).unwrap();
    report["ackEvidence"]=json!({"path":trace,"sha256":digest(&trace).unwrap()});
    if let Some(journal)=&journal{report["nativeJournal"]=json!({"path":journal.evidence_path(),"sha256":digest(journal.evidence_path()).unwrap()});}
    let error=match result{Ok(Ok(()))=>None,Ok(Err(error))=>Some(error),Err(_)=>Some("driver_panicked_closed_no_replay".into())};
    report["passed"]=json!(error.is_none()&&closed&&port_clear&&!overflow.load(Ordering::Acquire));report["error"]=json!(error);
    save(&output.join("result.json"),&report).unwrap();assert_eq!(report["passed"],true,"driver stopped; do not repeat mutation");
}
