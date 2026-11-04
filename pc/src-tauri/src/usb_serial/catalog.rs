//! Generic explicit metadata maintenance. No install/delete/activate fallback,
//! cleanup, request retry, or mutation after an indeterminate acknowledgment.
use super::*;
use crate::app_catalog::{self as catalog,Journal,Outcome,ReconcileRequest,RecoveryEvidence,Snapshot};

impl UsbSerialManager{
    fn catalog_status(&self,board:&str)->Result<(UsbConnectionStatus,u64),String>{
        if !catalog::valid_board(board){return Err("catalog_invalid_board".into());}
        let (status,generation)=self.connected_board_snapshot(board)?;
        if generation==0||status.runtime!="esp-p4"||!catalog::supported(&status.capabilities){return Err("catalog_metadata_unsupported".into());}
        Ok((status,generation))
    }
    fn catalog_response(&self,board:&str,generation:u64,topic:&str,response_topic:&str,request_id:&str,payload:&serde_json::Value)->Result<serde_json::Value,String>{
        let bytes=serde_json::to_vec(payload).map_err(|_|"catalog_payload_unavailable")?;
        if bytes.len()>4096||payload["requestId"]!=request_id{return Err("catalog_payload_unverified".into());}
        let (sender,receiver)=mpsc::channel();
        self.device_response_waiters.lock().map_err(|_|"catalog_waiter_unavailable")?.push(DeviceResponseWaiter{request_id:request_id.into(),response_topic:response_topic.into(),sender});
        if let Err(error)=self.send_inner(topic,payload,true,Some(board),Some(generation),None){self.remove_device_response_waiter(request_id,response_topic);return Err(error);}
        let reply=receiver.recv_timeout(P4_DEVICE_RESPONSE_TIMEOUT);
        self.remove_device_response_waiter(request_id,response_topic);
        let reply=reply.map_err(|_|"catalog_ack_unavailable")?;
        let (_,current)=self.catalog_status(board)?;
        if current!=generation||reply["requestId"]!=request_id||reply["boardDeviceId"]!=board{return Err("catalog_response_unverified".into());}
        Ok(reply)
    }
    fn catalog_inspect_locked(&self,board:&str,generation:u64)->Result<Snapshot,String>{
        let request_id=format!("catalog-{}",uuid::Uuid::new_v4());
        let reply=self.catalog_response(board,generation,"widget/catalog_inspect","widget/catalog_snapshot",&request_id,&serde_json::json!({"requestId":request_id}))?;
        catalog::snapshot(&reply,board,&request_id)
    }
    pub(crate) fn catalog_inspect(&self,board:&str)->Result<Snapshot,String>{
        let _guard=self.asset_transfer_guard.try_lock().map_err(|_|"catalog_transport_busy")?;
        let (_,generation)=self.catalog_status(board)?;self.catalog_inspect_locked(board,generation)
    }
    pub(crate) fn catalog_reconcile(&self,request:&ReconcileRequest,evidence:Option<&RecoveryEvidence>,journal:&Journal)->Result<Outcome,String>{
        request.validate()?;
        if let Some(out)=journal.prior(request)?{return Ok(out);}
        let _guard=self.asset_transfer_guard.try_lock().map_err(|_|"catalog_transport_busy")?;
        let board=&request.expected_board_device_id;let (_,generation)=self.catalog_status(board)?;
        if self.media_requests_pending.load(Ordering::Acquire)!=0||self.appearance_sync_active.load(Ordering::Acquire){return Err("catalog_resources_busy".into());}
        if !self.connection.lock().map_err(|_|"catalog_connection_unavailable")?.as_ref().is_some_and(|c|c.connection_id==generation&&!c.capture_active.load(Ordering::Acquire)){
            return Err("catalog_resources_busy".into());
        }
        let current=self.catalog_inspect_locked(board,generation)?;
        let prepared=catalog::prepare(&current,request,evidence)?;
        // No replacement program can be authorized by deleting its old catalog
        // record. Host admission uses try_lock on this same transfer guard, so
        // it cannot wait here while holding the policy mutex in the other order.
        for id in &request.remove_visible_ids{crate::program_autostart::native::component_changed(id,true);}
        journal.submit(&prepared,|request_id,payload|{
            // Recheck physical ownership immediately before the sole submission.
            // FW additionally checks idle/resource gates and CAS under its own
            // storage owner immediately before committing metadata.
            let (_,current_generation)=self.catalog_status(board)?;
            if generation!=current_generation||self.media_requests_pending.load(Ordering::Acquire)!=0
                ||!self.connection.lock().map_err(|_|"catalog_connection_unavailable")?.as_ref().is_some_and(|c|c.connection_id==generation&&!c.capture_active.load(Ordering::Acquire)){
                return Err("catalog_owner_changed".into());
            }
            self.catalog_response(board,generation,"widget/catalog_reconcile","widget/catalog_reconcile_result",request_id,&payload)
        })
    }
}

/// Private orchestration may call this from an injected Rust test after it has
/// acquired serial exclusion and connected the production manager/reader.
/// No port opening, journal reset or output-file overwrite occurs here.
#[cfg(test)]
#[derive(serde::Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
pub(crate) struct DriverInput{pub action:String,pub expected_board_device_id:String,pub request:Option<ReconcileRequest>,
    pub evidence:Option<RecoveryEvidence>,pub user_confirmed:bool}
#[cfg(test)]
pub(crate) fn run_catalog_maintenance_driver(manager:&UsbSerialManager,input:&DriverInput,journal:Option<&Journal>)->Result<serde_json::Value,String>{
    if !catalog::valid_board(&input.expected_board_device_id){return Err("catalog_invalid_board".into());}
    match input.action.as_str(){
        "inspect" if input.request.is_none()&&input.evidence.is_none()=>serde_json::to_value(manager.catalog_inspect(&input.expected_board_device_id)?).map_err(|_|"catalog_result_unavailable".into()),
        "reconcile" if input.user_confirmed=>{
            let request=input.request.as_ref().ok_or("catalog_request_required")?;
            if request.expected_board_device_id!=input.expected_board_device_id{return Err("catalog_driver_board_changed".into());}
            serde_json::to_value(manager.catalog_reconcile(request,input.evidence.as_ref(),journal.ok_or("catalog_journal_required")?)?).map_err(|_|"catalog_result_unavailable".into())
        },
        _=>Err("catalog_explicit_driver_action_required".into()),
    }
}

#[cfg(test)]mod tests{
    use super::*;
    use serde_json::{json,Value};
    include!("catalog/acceptance.rs");
    const BOARD:&str="catalog-board";const BOOT:&str="0123456789abcdef";
    #[derive(Clone,Copy)]enum Mode{Known,Negative,WrongSeq,DropAck,Deferred}
    fn metadata(id:&Value)->Value{
        let items=[("extra_x",true),("retained_shadow",false),("keep_a",true),("keep_b",true)].into_iter().enumerate().map(|(slot,(id,visible))|
            json!({"id":id,"visible":visible,"slot":slot,"packageGeneration":1,"widgetChecksum":"11111111","buttonsChecksum":"22222222",
                "spritesChecksum":"33333333","resourceState":"ready","imageCount":2})).collect::<Vec<_>>();
        json!({"schemaVersion":1,"engine":catalog::ENGINE,"requestId":id,"boardDeviceId":BOARD,"bootId":BOOT,"ok":true,"complete":true,"recoveryBlocked":false,
            "catalogSeq":7,"catalogDigest":"12345678","physicalCount":4,"activeWidgetId":"keep_a","selectedWidgetId":"keep_a","items":items})
    }
    fn evidence(order:&[&str])->Value{
        json!({"schemaVersion":1,"boardDeviceId":BOARD,"ok":true,"complete":true,"recoveryBlocked":false,"warnings":[],"activeWidgetId":"keep_a",
            "selectedWidgetId":"keep_a","selectedIndex":order.iter().position(|id|*id=="keep_a"),"items":order.iter().map(|id|
                json!({"id":id,"active":*id=="keep_a","resourceState":"ready","manifestState":"valid","imageCount":2})).collect::<Vec<_>>()})
    }
    fn plan()->(ReconcileRequest,RecoveryEvidence){
        (ReconcileRequest{expected_board_device_id:BOARD.into(),expected_boot_id:BOOT.into(),expected_catalog_seq:7,expected_catalog_digest:"12345678".into(),
            remove_visible_ids:vec!["extra_x".into()],survivor_visible_order:vec!["keep_b".into(),"keep_a".into()],preserve_save:true},
         RecoveryEvidence{before_inventory:evidence(&["keep_b","keep_a"]),after_inventory:evidence(&["extra_x","keep_a","keep_b"])})
    }
    struct Writer{bytes:Vec<u8>,waiters:Arc<Mutex<Vec<DeviceResponseWaiter>>>,trace:Arc<Mutex<Vec<Value>>>,mode:Mode,deferred:Option<mpsc::Sender<Value>>}
    impl Write for Writer{
        fn write(&mut self,bytes:&[u8])->std::io::Result<usize>{
            self.bytes.extend_from_slice(bytes);assert!(self.bytes.len()<8192);
            while let Some(end)=self.bytes.iter().position(|b|*b==b'\n'){
                let line=self.bytes.drain(..=end).collect::<Vec<_>>();let request:Value=serde_json::from_slice(&line).unwrap();self.trace.lock().unwrap().push(request.clone());
                let payload=&request["payload"];
                let (topic,reply)=match request["topic"].as_str().unwrap(){
                    "widget/catalog_inspect"=>("widget/catalog_snapshot",metadata(&payload["requestId"])),
                    "widget/catalog_reconcile"=>{
                        if matches!(self.mode,Mode::DropAck){continue;}
                        let reply=json!({"schemaVersion":1,"engine":catalog::ENGINE,"requestId":payload["requestId"],"boardDeviceId":BOARD,"bootId":BOOT,
                            "ok":!matches!(self.mode,Mode::Negative),"commitState":if matches!(self.mode,Mode::Negative){"not_committed"}else{"committed"},
                            "catalogSeq":if matches!(self.mode,Mode::WrongSeq){9}else{8},"catalogDigest":"87654321"});
                        if let Some(sender)=&self.deferred{sender.send(reply).unwrap();continue;}
                        ("widget/catalog_reconcile_result",reply)
                    },_=>panic!("unexpected fallback/cleanup/reinstall/activate topic"),
                };
                let mut foreign=reply.clone();foreign["requestId"]="foreign-request".into();resolve_device_response(&self.waiters,topic,&foreign);
                resolve_device_response(&self.waiters,topic,&reply);
            }Ok(bytes.len())
        }
        fn flush(&mut self)->std::io::Result<()>{Ok(())}
    }
    fn manager(mode:Mode,deferred:Option<mpsc::Sender<Value>>)->(UsbSerialManager,Arc<Mutex<Vec<Value>>>){
        let manager=UsbSerialManager::new();let trace=Arc::new(Mutex::new(vec![]));
        *manager.connection.lock().unwrap()=Some(UsbConnection{connection_id:u64::MAX,port_name:"memory-only".into(),baud_rate:115200,
            writer:Box::new(Writer{bytes:vec![],waiters:manager.device_response_waiters.clone(),trace:trace.clone(),mode,deferred}),board_device_id:BOARD.into(),runtime:"esp-p4".into(),
            device_model:String::new(),firmware:String::new(),build_id:String::new(),git_sha:String::new(),build_dirty:false,protocol_schema:7,wire_protocol:String::new(),
            capabilities:json!({"widgetCatalogMetadata":catalog::ENGINE,"debugBootId":BOOT}),connected:true,cancel_reader:Arc::new(AtomicBool::new(false)),capture_active:Arc::new(AtomicBool::new(false))});
manager.activate_memory_test_connection();
        (manager,trace)
    }
    #[test]fn typed_driver_uses_real_waiters_and_exact_single_reconcile_payload(){
        let (manager,trace)=manager(Mode::Known,None);let dir=tempfile::tempdir().unwrap();let journal=Journal::open(dir.path()).unwrap();let (request,evidence)=plan();
        let input=DriverInput{action:"reconcile".into(),expected_board_device_id:BOARD.into(),request:Some(request.clone()),evidence:Some(evidence),user_confirmed:true};
        let result=run_catalog_maintenance_driver(&manager,&input,Some(&journal)).unwrap();assert_eq!(result["confirmed"],true);
        assert_eq!(manager.status().connection_id,"18446744073709551615");
        let trace=trace.lock().unwrap();assert_eq!(trace.len(),2);assert_eq!(trace[0]["topic"],"widget/catalog_inspect");assert_eq!(trace[1]["topic"],"widget/catalog_reconcile");
        let mut sent=trace[1]["payload"].clone();sent.as_object_mut().unwrap().remove("requestId");assert_eq!(sent,serde_json::to_value(request).unwrap());
    }
    #[test]fn negative_or_malformed_ack_latches_read_only_with_no_automatic_query_or_resend(){
        for mode in [Mode::Negative,Mode::WrongSeq]{
            let (manager,trace)=manager(mode,None);let dir=tempfile::tempdir().unwrap();let journal=Journal::open(dir.path()).unwrap();let (request,evidence)=plan();
            let result=manager.catalog_reconcile(&request,Some(&evidence),&journal).unwrap();assert!(!result.confirmed&&result.read_only);assert_eq!(trace.lock().unwrap().len(),2);
            assert!(manager.catalog_reconcile(&request,Some(&evidence),&journal).is_err());assert_eq!(trace.lock().unwrap().len(),2);
            assert!(manager.catalog_inspect(BOARD).is_ok());assert_eq!(trace.lock().unwrap().len(),3);
        }
    }
    #[test]fn real_timeout_retains_unknown_and_does_not_submit_a_second_mutation(){
        let (manager,trace)=manager(Mode::DropAck,None);let dir=tempfile::tempdir().unwrap();let journal=Journal::open(dir.path()).unwrap();let (request,evidence)=plan();
        let start=Instant::now();let result=manager.catalog_reconcile(&request,Some(&evidence),&journal).unwrap();
        assert!(start.elapsed()>=P4_DEVICE_RESPONSE_TIMEOUT);assert!(!result.confirmed&&result.read_only);
        assert!(manager.catalog_reconcile(&request,Some(&evidence),&journal).is_err());assert_eq!(trace.lock().unwrap().len(),2);
    }
    #[test]fn correlated_ack_on_a_changed_real_generation_is_still_unknown(){
        let (send,receive)=mpsc::channel();let (manager,trace)=manager(Mode::Deferred,Some(send));let worker=manager.clone();
        let reader=std::thread::spawn(move||{let reply=receive.recv().unwrap();worker.connection.lock().unwrap().as_mut().unwrap().connection_id=2;
            resolve_device_response(&worker.device_response_waiters,"widget/catalog_reconcile_result",&reply);});
        let dir=tempfile::tempdir().unwrap();let journal=Journal::open(dir.path()).unwrap();let (request,evidence)=plan();
        assert!(!manager.catalog_reconcile(&request,Some(&evidence),&journal).unwrap().confirmed);reader.join().unwrap();assert_eq!(trace.lock().unwrap().len(),2);
        assert!(manager.catalog_reconcile(&request,Some(&evidence),&journal).is_err());
    }
    #[test]fn stale_snapshot_or_invalid_evidence_sends_no_mutation(){
        for stale in [false,true]{
            let (manager,trace)=manager(Mode::Known,None);let dir=tempfile::tempdir().unwrap();let journal=Journal::open(dir.path()).unwrap();let (mut request,mut evidence)=plan();
            if stale{request.expected_catalog_seq+=1;}else{evidence.before_inventory["complete"]=false.into();}
            assert!(manager.catalog_reconcile(&request,Some(&evidence),&journal).is_err());assert_eq!(trace.lock().unwrap().len(),1);
            assert!(journal.prior(&request).unwrap().is_none());
        }
    }
    #[test]fn resource_or_transfer_ownership_conflict_never_queries_or_submits(){
        let (manager,trace)=manager(Mode::Known,None);let dir=tempfile::tempdir().unwrap();let journal=Journal::open(dir.path()).unwrap();let (request,evidence)=plan();
        {let _guard=manager.asset_transfer_guard.lock().unwrap();assert!(manager.catalog_reconcile(&request,Some(&evidence),&journal).is_err());}
        manager.connection.lock().unwrap().as_mut().unwrap().capture_active.store(true,Ordering::Release);
        assert!(manager.catalog_reconcile(&request,Some(&evidence),&journal).is_err());assert!(trace.lock().unwrap().is_empty());
    }
}
