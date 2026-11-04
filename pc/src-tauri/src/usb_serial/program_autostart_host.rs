//! Native policy port: one transfer guard + one formal-library guard spans
//! checksum/hash/boot/generation verification and the single fixed submission.
use super::*;
use crate::program_autostart::{self as policy,CheckFailure,Host,VerifiedProgram};

pub(crate) struct UsbHost { usb:UsbSerialManager, home:PathBuf }
impl UsbHost { pub(crate) fn new(usb:UsbSerialManager,home:PathBuf)->Self{Self{usb,home}} }
fn unavailable<T>(_:T)->CheckFailure{CheckFailure::Unavailable}
fn fnv(bytes:&[u8])->u32{bytes.iter().fold(2166136261u32,|s,b|(s^u32::from(*b)).wrapping_mul(16777619))}
fn add(sum:&mut u32,bytes:&[u8]){for b in bytes{*sum=(*sum^u32::from(*b)).wrapping_mul(16777619);}}
fn response_matches(response:&serde_json::Value,board:&str,expected_generation:u64,current_generation:u64,request_id:&str,topic:&str)->bool{
    if expected_generation==0||expected_generation!=current_generation||response["requestId"]!=request_id||response["ok"]!=true{return false;}
    match response.get("boardDeviceId"){
        Some(value)=>value.as_str()==Some(board),
        None=>topic=="media/status",
    }
}

/// Exact miniapp sprite_pack_checksum over the installer's compiled native
/// files, not PNG bytes, filenames, asset transport's unrelated FNV64, or JSON.
fn pack_checksum(files:&[(String,Vec<u8>)])->Result<String,CheckFailure>{
    if files.is_empty(){return Ok("00000000".into());}
    if files.len()>16{return Err(CheckFailure::Unavailable);}
    let mut sum=2166136261u32;add(&mut sum,&[files.len() as u8]);
    let mut ids=HashSet::new();
    for (path,data) in files{
        let id=Path::new(path).file_stem().and_then(|s|s.to_str()).ok_or(CheckFailure::Unavailable)?;
        if id.is_empty()||id.contains('\0')||!ids.insert(id)||data.len()<8{return Err(CheckFailure::Unavailable);}
        let (width,height,frames,fps,pixels)=match &data[..4]{
            b"P4S1"=>(u16::from(data[4]),u16::from(data[5]),data[6],data[7],&data[8..]),
            b"P4I1"=>(u16::from(data[4]),u16::from(data[5]),1,0,data.as_slice()),
            b"P4I2"|b"P4J1"=>(u16::from_le_bytes([data[4],data[5]]),u16::from_le_bytes([data[6],data[7]]),1,0,data.as_slice()),
            _=>return Err(CheckFailure::Unavailable),
        };
        add(&mut sum,id.as_bytes());add(&mut sum,&[0,width as u8,height as u8]);
        if width>255||height>255{add(&mut sum,&[(width>>8) as u8,(height>>8) as u8]);}
        add(&mut sum,&[frames,fps]);add(&mut sum,pixels);
    }
    Ok(format!("{sum:08x}"))
}
fn local_checksums(root:&Path,id:&str,caps:&serde_json::Value)->Result<policy::Checksums,CheckFailure>{
    let source=std::fs::read(root.join("runtime/widget.json")).map_err(unavailable)?;
    if source.len()>16384{return Err(CheckFailure::Unavailable);}
    let widget:serde_json::Value=serde_json::from_slice(&source).map_err(unavailable)?;
    // A formal validated package, never a preview/manifest-only program.
    crate::app_component_program::require_capability(&widget,caps).map_err(unavailable)?;
    let engine=widget["program"]["engine"].as_str().ok_or(CheckFailure::Unavailable)?;
    let negotiated=caps["widgetProgram"]==engine||caps["widgetPrograms"].as_array().is_some_and(|a|a.iter().any(|v|v==engine));
    if !matches!(engine,"p4-component-program-v1"|"p4-component-program-v2")||!negotiated{return Err(CheckFailure::Unavailable);}
    let widget_bytes=prepare_p4_widget_file(id,"runtime/widget.json",&source).map_err(unavailable)?;
    let buttons=std::fs::read(root.join("buttons.json")).map_err(unavailable)?;
    if buttons.len()>16384{return Err(CheckFailure::Unavailable);}
    let button_bytes=prepare_p4_widget_file(id,"buttons.json",&buttons).map_err(unavailable)?;
    let sprites=prepare_p4_widget_sprite_files(root,&source).map_err(unavailable)?;
    Ok(policy::Checksums{widget_checksum:format!("{:08x}",fnv(&widget_bytes)),buttons_checksum:format!("{:08x}",fnv(&button_bytes)),sprites_checksum:pack_checksum(&sprites)?})
}
impl UsbSerialManager {
    pub(crate) fn autostart_connection(&self)->Option<(UsbConnectionStatus,u64)>{
        let status=self.status();if !status.connected{return None;}
        self.connected_board_snapshot(&status.board_device_id).ok()
    }
    // Separate from the legacy helper: every READ and the sole WRITE pins the
    // exact native connection generation. No reconnect or re-send fallback.
    fn autostart_request(&self,board:&str,generation:u64,topic:&str,response_topic:&str,payload:serde_json::Value)->Result<serde_json::Value,String>{
        self.autostart_request_admitted(board,generation,topic,response_topic,payload,None)
    }
    fn autostart_request_admitted(&self,board:&str,generation:u64,topic:&str,response_topic:&str,mut payload:serde_json::Value,epoch:Option<u64>)->Result<serde_json::Value,String>{
        let request_id=format!("autostart-{}",uuid::Uuid::new_v4());payload["requestId"]=request_id.clone().into();
        let (sender,receiver)=mpsc::channel();
        self.device_response_waiters.lock().map_err(|_|"autostart response unavailable")?.push(DeviceResponseWaiter{request_id:request_id.clone(),response_topic:response_topic.into(),sender});
        if let Err(e)=self.send_inner_admitted(topic,&payload,true,Some(board),Some(generation),None,epoch){self.remove_device_response_waiter(&request_id,response_topic);return Err(e);}
        let response=receiver.recv_timeout(P4_DEVICE_RESPONSE_TIMEOUT);
        self.remove_device_response_waiter(&request_id,response_topic);
        let response=response.map_err(|_|"autostart response unavailable")?;
        let (_,current)=self.connected_board_snapshot(board)?;
        if !response_matches(&response,board,generation,current,&request_id,response_topic){return Err("autostart response unverified".into());}
        Ok(response)
    }
    pub(crate) fn autostart_boot(&self,board:&str,generation:u64)->Result<String,String>{
        let _guard=self.asset_transfer_guard.try_lock().map_err(|_|"autostart transport busy")?;
        let (status,current)=self.connected_board_snapshot(board)?;
        if generation==0||generation!=current||status.runtime!="esp-p4"||!policy::supported(&status.capabilities){return Err("autostart capability unavailable".into());}
        let reply=self.autostart_request(board,generation,"widget/program-query","widget/program-status",serde_json::json!({}))?;
        let boot=crate::app_program_background::snapshot(&reply,board,None)?.boot_id;
        policy::native::verified_boot(board,generation,&boot);Ok(boot)
    }
}
impl UsbHost {
    fn verified(&self,board:&str,id:&str,root:&Path,generation:u64,epoch:u64)->Result<(VerifiedProgram,crate::app_program_background::Installed),CheckFailure>{
        let (status,current)=self.usb.connected_board_snapshot(board).map_err(unavailable)?;
        if generation==0||current!=generation||status.runtime!="esp-p4"||!policy::supported(&status.capabilities){return Err(CheckFailure::Unavailable);}
        if !self.usb.connection.lock().map_err(unavailable)?.as_ref().is_some_and(|c|c.connection_id==generation&&!c.capture_active.load(Ordering::Acquire)){return Err(CheckFailure::Unavailable);}
        if self.usb.media_requests_pending.load(Ordering::Acquire)!=0{return Err(CheckFailure::Unavailable);}
        let hash=policy::component_content_hash(root).map_err(unavailable)?;
        let sums=local_checksums(root,id,&status.capabilities)?;
        let inventory=self.usb.autostart_request(board,generation,"widget/list","widget/inventory",serde_json::json!({})).map_err(unavailable)?;
        if inventory["complete"]!=true||inventory["recoveryBlocked"]!=false{return Err(CheckFailure::Unavailable);}
        let rows=inventory["items"].as_array().filter(|a|a.len()<=32).ok_or(CheckFailure::Unavailable)?;
        if !rows.iter().any(|r|r["id"]==id){return Err(CheckFailure::ComponentDeleted);}
        let inspected=self.usb.autostart_request(board,generation,"widget/program-inspect","widget/program-status",serde_json::json!({"widgetId":id})).map_err(unavailable)?;
        let verified=VerifiedProgram::from_device(board,id,&hash,&status.capabilities,&inventory,&inspected).map_err(unavailable)?.bind(generation,epoch);
        policy::native::verified_boot(board,generation,verified.boot_id());
        if verified.target().checksums!=sums{return Err(CheckFailure::ContentChanged);}
        // Diagnostic reads remain under the same guard and generation. Music
        // 'ended' still owns MUSIC until explicitly released by its owner.
        let diag=self.usb.autostart_request(board,generation,"diagnostics/query","diagnostics/status",serde_json::json!({})).map_err(unavailable)?;
        if diag["runtime"]["audioPlaybackActive"].as_bool()!=Some(false){return Err(CheckFailure::Unavailable);}
        if status.capabilities["widgetMedia"]=="p4-media-v1"{
            let media=self.usb.autostart_request(board,generation,"media/query","media/status",serde_json::json!({})).map_err(unavailable)?;
            if !media["state"].as_str().is_some_and(|s|matches!(s,"idle"|"stopped")&&!super::component_sync_media_blocks(s)){return Err(CheckFailure::Unavailable);}
        }
        if policy::component_content_hash(root).map_err(unavailable)?!=hash{return Err(CheckFailure::ContentChanged);}
        if policy::native::admission()!=Some(epoch){return Err(CheckFailure::Unavailable);}
        let (_,current)=self.usb.connected_board_snapshot(board).map_err(unavailable)?;
        if current!=generation{return Err(CheckFailure::DeviceChanged);}
        let content=crate::app_program_background::installed(&inspected,board,id).map_err(unavailable)?;
        Ok((verified,content))
    }
}
impl Host for UsbHost {
    fn inspect(&self,board:&str,id:&str)->Result<VerifiedProgram,CheckFailure>{
        let epoch=policy::native::admission().ok_or(CheckFailure::Unavailable)?;
        let _transfer=self.usb.asset_transfer_guard.try_lock().map_err(unavailable)?;
        let (_,generation)=self.usb.connected_board_snapshot(board).map_err(unavailable)?;
        crate::component_library::with_autostart_component(&self.home,id,|root|self.verified(board,id,root,generation,epoch).map(|p|p.0))
    }
    fn start_once(&self,expected:&VerifiedProgram)->Result<crate::app_program_background::Outcome,CheckFailure>{
        let (generation,epoch)=expected.binding().ok_or(CheckFailure::Unavailable)?;
        if policy::native::admission()!=Some(epoch){return Err(CheckFailure::Unavailable);}
        let _transfer=self.usb.asset_transfer_guard.try_lock().map_err(unavailable)?;
        let board=&expected.target().board_id;let id=&expected.target().component_id;
        crate::component_library::with_autostart_component(&self.home,id,|root|{
            let (fresh,content)=self.verified(board,id,root,generation,epoch)?;
            if fresh.target()!=expected.target(){return Err(CheckFailure::ContentChanged);}
            if fresh.boot_id()!=expected.boot_id()||fresh.has_registration(){return Err(CheckFailure::Unavailable);}
            if policy::native::admission()!=Some(epoch){return Err(CheckFailure::Unavailable);}
            // Precisely ONE fixed payload, retaining original boot/checksums.
            // All post-submission errors are unknown, with no inspect/replay.
            Ok(match self.usb.autostart_request_admitted(board,generation,"widget/program-start","widget/program-status",expected.start_payload(),Some(epoch)){
                Ok(reply)=>crate::app_program_background::Outcome::started(&reply,board,&content),
                Err(_)=>crate::app_program_background::Outcome::unknown(),
            })
        })
    }
}

#[cfg(test)]mod tests{
    use super::*;
    static HOST_TEST_SERIAL:Mutex<()>=Mutex::new(());
    const BOOT:&str="0123456789abcdef";
    fn caps()->serde_json::Value{serde_json::json!({"widgetProgram":"p4-component-program-v2","widgetPrograms":["p4-component-program-v1","p4-component-program-v2"],
        "widgetProgramBackground":crate::app_program_background::ENGINE,"widgetProgramInspect":crate::app_program_background::INSPECT,"widgetProgramBackgroundMaxActive":1,
        "widgetProgramAggregateFuel":4096,"widgetProgramMaxFuel":4096,"widgetProgramQuantum":64,"widgetProgramAggregateQuantum":64,
        "widgetMedia":"p4-media-v1","debugBootId":BOOT})}
    struct MemoryWriter{pending:Vec<u8>,waiters:Arc<Mutex<Vec<DeviceResponseWaiter>>>,requests:Arc<Mutex<Vec<serde_json::Value>>>,sums:Arc<Mutex<policy::Checksums>>,boot:Arc<Mutex<String>>,home:PathBuf,library_held:Arc<AtomicBool>}
    impl Write for MemoryWriter{
        fn write(&mut self,bytes:&[u8])->std::io::Result<usize>{
            self.pending.extend_from_slice(bytes);
            while let Some(end)=self.pending.iter().position(|b|*b==b'\n'){
                let row=self.pending.drain(..=end).collect::<Vec<_>>();let req:serde_json::Value=serde_json::from_slice(&row).unwrap();
                let topic=req["topic"].as_str().unwrap();self.requests.lock().unwrap().push(req.clone());
                let mut reply=serde_json::json!({"requestId":req["payload"]["requestId"],"boardDeviceId":"p4-test","ok":true,
                    "engine":crate::app_program_background::ENGINE,"bootId":self.boot.lock().unwrap().clone(),"items":[]});
                let response=match topic{
                    "widget/list"=>{reply["complete"]=true.into();reply["recoveryBlocked"]=false.into();reply["items"]=serde_json::json!([{"id":"focus"}]);"widget/inventory"},
                    "widget/program-inspect"=>{let sums=self.sums.lock().unwrap();reply["installedContent"]=serde_json::json!({"widgetId":"focus","widgetChecksum":sums.widget_checksum,"buttonsChecksum":sums.buttons_checksum,"spritesChecksum":sums.sprites_checksum});"widget/program-status"},
                    "diagnostics/query"=>{reply["runtime"]=serde_json::json!({"audioPlaybackActive":false});"diagnostics/status"},
                    "media/query"=>{reply.as_object_mut().unwrap().remove("boardDeviceId");reply["state"]="idle".into();"media/status"},
                    "widget/program-start"=>{
                        let held=matches!(crate::component_library::with_autostart_component(&self.home,"focus",|_|Ok(())),Err(CheckFailure::Unavailable));
                        self.library_held.store(held,Ordering::SeqCst);
                        reply["items"]=serde_json::json!([{"widgetId":"focus","registration":"18446744073709551615","live":true,"stopped":false,"checkpointPending":false,"revision":1,"phase":1,"lastStepUs":1,"maxStepUs":2}]);"widget/program-status"
                    },_=>panic!("unexpected autostart topic {topic}"),
                };
                // A stale ACK with the same topic MUST NOT resolve this waiter.
                let mut foreign=reply.clone();foreign["requestId"]="other-request".into();resolve_device_response(&self.waiters,response,&foreign);
                resolve_device_response(&self.waiters,response,&reply);
            }Ok(bytes.len())
        }
        fn flush(&mut self)->std::io::Result<()>{Ok(())}
    }
    struct Fixture{_dir:tempfile::TempDir,host:UsbHost,root:PathBuf,requests:Arc<Mutex<Vec<serde_json::Value>>>,sums:Arc<Mutex<policy::Checksums>>,boot:Arc<Mutex<String>>,library_held:Arc<AtomicBool>,_serial:std::sync::MutexGuard<'static,()>}
    fn fixture()->Fixture{
        let serial=HOST_TEST_SERIAL.lock().unwrap_or_else(|e|e.into_inner());
        let dir=tempfile::tempdir().unwrap();let source=dir.path().join("source");let home=dir.path().join("home");
        std::fs::create_dir_all(source.join("runtime")).unwrap();
        std::fs::create_dir_all(source.join("assets")).unwrap();std::fs::write(source.join("assets/.keep"),b"").unwrap();
        let contract:serde_json::Value=serde_json::from_str(include_str!("../../../skills/petui/contracts/component-program-v1.json")).unwrap();
        std::fs::write(source.join("runtime/widget.json"),serde_json::to_vec(&contract["runtime"]).unwrap()).unwrap();
        for (name,bytes) in [("component.json",br#"{"id":"focus","name":"Focus","version":"1.0.0","kind":"tool","description":"test"}"#.as_slice()),
            ("negative-screen.json",br#"{"dashboard":{"title":"Focus"}}"#.as_slice()),("buttons.json",b"[]".as_slice()),("share.json",br#"{"title":"Focus"}"#.as_slice())]{std::fs::write(source.join(name),bytes).unwrap();}
        let entry=crate::component_library::publish_source(&home,&source).unwrap();let root=PathBuf::from(entry.path);
        let sums=Arc::new(Mutex::new(local_checksums(&root,"focus",&caps()).unwrap()));let requests=Arc::new(Mutex::new(Vec::new()));
        let boot=Arc::new(Mutex::new(BOOT.to_owned()));let library_held=Arc::new(AtomicBool::new(false));let usb=UsbSerialManager::new();
        *usb.connection.lock().unwrap()=Some(UsbConnection{connection_id:u64::MAX,port_name:"memory-only".into(),baud_rate:P4_USB_UART_BAUD,
            writer:Box::new(MemoryWriter{pending:vec![],waiters:usb.device_response_waiters.clone(),requests:requests.clone(),sums:sums.clone(),boot:boot.clone(),home:home.clone(),library_held:library_held.clone()}),
            board_device_id:"p4-test".into(),runtime:"esp-p4".into(),device_model:String::new(),firmware:String::new(),build_id:String::new(),git_sha:String::new(),build_dirty:false,
            protocol_schema:7,wire_protocol:String::new(),capabilities:caps(),connected:true,cancel_reader:Arc::new(AtomicBool::new(false)),capture_active:Arc::new(AtomicBool::new(false))});
usb.activate_memory_test_connection();
        Fixture{_dir:dir,host:UsbHost::new(usb,home),root,requests,sums,boot,library_held,_serial:serial}
    }
    #[test]fn host_exact_content_fixed_payload_and_correlated_ack_hold_library_guard(){
        let f=fixture();let expected=f.host.inspect("p4-test","focus").unwrap();
        let status=serde_json::to_value(f.host.usb.status()).unwrap();assert_eq!(status["connectionId"],"18446744073709551615");assert_eq!(status["bootId"],BOOT);
        assert!(f.host.start_once(&expected).unwrap().confirmed);assert!(f.library_held.load(Ordering::SeqCst));
        let requests=f.requests.lock().unwrap();let starts=requests.iter().filter(|r|r["topic"]=="widget/program-start").collect::<Vec<_>>();assert_eq!(starts.len(),1);
        let mut payload=starts[0]["payload"].clone();payload.as_object_mut().unwrap().remove("requestId");assert_eq!(payload,expected.start_payload());
    }
    #[test]fn host_content_checksum_mismatch_never_selects_replacement_or_sends_start(){
        let f=fixture();let expected=f.host.inspect("p4-test","focus").unwrap();
        f.sums.lock().unwrap().buttons_checksum="ffffffff".into();assert!(matches!(f.host.start_once(&expected),Err(CheckFailure::ContentChanged)));
        assert!(!f.requests.lock().unwrap().iter().any(|r|r["topic"]=="widget/program-start"));
    }
    #[test]fn host_changed_local_bytes_boot_generation_or_preempted_epoch_cannot_submit(){
        for change in 0..4{
            let f=fixture();let mut expected=f.host.inspect("p4-test","focus").unwrap();
            match change{
                0=>std::fs::write(f.root.join("share.json"),br#"{"title":"Changed"}"#).unwrap(),
                1=>*f.boot.lock().unwrap()="1123456789abcdef".into(),
                2=>f.host.usb.connection.lock().unwrap().as_mut().unwrap().connection_id=2,
                _=>{let (connection,epoch)=expected.binding().unwrap();expected=expected.bind(connection,epoch+1);},
            }
            assert!(f.host.start_once(&expected).is_err());assert!(!f.requests.lock().unwrap().iter().any(|r|r["topic"]=="widget/program-start"));
        }
    }
    #[test]fn host_library_guard_conflict_fails_without_usb_work(){
        let f=fixture();crate::component_library::with_autostart_component(&f.host.home,"focus",|_|{
            assert!(matches!(f.host.inspect("p4-test","focus"),Err(CheckFailure::Unavailable)));Ok(())
        }).unwrap();assert!(f.requests.lock().unwrap().is_empty());
    }
    #[test]fn media_reply_may_omit_board_only_with_exact_request_and_generation(){
        let reply=serde_json::json!({"ok":true,"requestId":"r1","state":"idle"});
        assert!(response_matches(&reply,"board-a",7,7,"r1","media/status"));
        assert!(!response_matches(&reply,"board-a",7,8,"r1","media/status"));
        assert!(!response_matches(&reply,"board-a",0,0,"r1","media/status"));
        assert!(!response_matches(&reply,"board-a",7,7,"r2","media/status"));
        for topic in ["widget/program-status","widget/inventory","diagnostics/status"]{assert!(!response_matches(&reply,"board-a",7,7,"r1",topic));}
        for value in [serde_json::json!("board-b"),serde_json::Value::Null,serde_json::json!(7)]{
            let mut bad=reply.clone();bad["boardDeviceId"]=value;assert!(!response_matches(&bad,"board-a",7,7,"r1","media/status"));
        }
        let mut exact=reply.clone();exact["boardDeviceId"]="board-a".into();assert!(response_matches(&exact,"board-a",7,7,"r1","widget/program-status"));
        exact["ok"]=false.into();assert!(!response_matches(&exact,"board-a",7,7,"r1","media/status"));
    }
    #[test]fn empty_and_native_sprite_checksums_use_firmware_pack_layout(){
        assert_eq!(pack_checksum(&[]).unwrap(),"00000000");
        let data=[b"P4S1".as_slice(),&[1,1,1,1],&[0,0,255]].concat();
        let mut bytes=vec![1];bytes.extend(b"dot\0");bytes.extend([1,1,1,1,0,0,255]);
        assert_eq!(pack_checksum(&[("runtime/sprites/dot.p4s".into(),data)]).unwrap(),format!("{:08x}",fnv(&bytes)));
    }
}
