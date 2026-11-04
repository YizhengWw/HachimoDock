//! Production manager/reader + memory I/O only. No fake mirror of transport
//! policies, device handles, shortened global timeout, dependency or hardware.
use super::*;
use serde_json::{json,Value};
use std::io::{self,Cursor};

const BOARD:&str="owned-board";
const NONCE:&str="0123456789abcdef0123456789abcdef";
#[derive(Clone,Copy)] enum Mode {Ack,Close,Drop,Partial,Nack}
struct MemoryWriter {
    pending:Vec<u8>,trace:Arc<Mutex<Vec<Value>>>,waiters:Arc<Mutex<Vec<AssetAckWaiter>>>,mode:Mode,
    raw:Option<(Value,usize)>,raw_bytes:Arc<AtomicUsize>,
}
impl Write for MemoryWriter {
    fn write(&mut self,bytes:&[u8])->io::Result<usize>{
        if matches!(self.mode,Mode::Partial){
            if self.pending.is_empty()&&!bytes.is_empty(){self.pending.push(bytes[0]);self.trace.lock().unwrap().push(json!({"partialBytes":1}));return Ok(1);}
            return Err(io::Error::new(io::ErrorKind::BrokenPipe,"injected partial/unknown transport"));
        }
        if let Some((header,remaining))=&mut self.raw {
            let count=bytes.len().min(*remaining);*remaining-=count;self.raw_bytes.fetch_add(count,Ordering::SeqCst);
            if *remaining==0 {
                let payload=header.clone();self.raw=None;
                if matches!(self.mode,Mode::Close){self.waiters.lock().unwrap().clear();}
                else if matches!(self.mode,Mode::Ack|Mode::Nack){resolve_asset_ack(&self.waiters,"asset/ack",&json!({"transferId":payload["transferId"],"phase":"raw-chunk","path":payload["path"],"index":payload["index"],"ok":!matches!(self.mode,Mode::Nack)}));}
            }
            return Ok(count);
        }
        self.pending.extend_from_slice(bytes);
        while let Some(end)=self.pending.iter().position(|b|*b==b'\n') {
            let bytes=self.pending.drain(..=end).collect::<Vec<_>>();let request:Value=serde_json::from_slice(&bytes).unwrap();
            self.trace.lock().unwrap().push(request.clone());let p=&request["payload"];
            let topic=request["topic"].as_str().unwrap();
            if topic=="asset/raw-chunk" {self.raw=Some((p.clone(),p["size"].as_u64().unwrap() as usize));continue;}
            if matches!(self.mode,Mode::Close){self.waiters.lock().unwrap().clear();continue;}
            if matches!(self.mode,Mode::Drop){continue;}
            let phase=topic.strip_prefix("asset/").unwrap();
            let mut reply=json!({"transferId":p["transferId"],"phase":phase,"ok":true});
            if p.get("path").is_some(){reply["path"]=p["path"].clone();}
            if p.get("index").is_some(){reply["index"]=p["index"].clone();}
            resolve_asset_ack(&self.waiters,"asset/ack",&reply);
        }
        Ok(bytes.len())
    }
    fn flush(&mut self)->io::Result<()>{Ok(())}
}
fn connection(manager:&UsbSerialManager,board:&str,generation:u64,mode:Mode,trace:Arc<Mutex<Vec<Value>>>,raw:Arc<AtomicUsize>)->UsbConnection {
    UsbConnection{connection_id:generation,port_name:"memory-only-no-port".into(),baud_rate:115200,
        writer:Box::new(MemoryWriter{pending:vec![],trace,waiters:manager.asset_ack_waiters.clone(),mode,raw:None,raw_bytes:raw}),
        board_device_id:board.into(),runtime:"esp-p4".into(),device_model:"ESP32-P4".into(),firmware:String::new(),build_id:String::new(),git_sha:String::new(),build_dirty:false,
        protocol_schema:7,wire_protocol:String::new(),capabilities:json!({"debugBootId":"0123456789abcdef"}),connected:true,
        cancel_reader:Arc::new(AtomicBool::new(false)),capture_active:Arc::new(AtomicBool::new(false))}
}
fn manager(mode:Mode)->(UsbSerialManager,Arc<Mutex<Vec<Value>>>,Arc<AtomicUsize>){
    let manager=UsbSerialManager::new();let trace=Arc::new(Mutex::new(vec![]));let raw=Arc::new(AtomicUsize::new(0));
    *manager.connection.lock().unwrap()=Some(connection(&manager,BOARD,1,mode,trace.clone(),raw.clone()));
    {let _owner=manager.inbound_owner.lock().unwrap();manager.inbound_owner.publish(1);}(manager,trace,raw)
}
fn frame(topic:&str,payload:Value,guarded:bool)->Vec<u8>{
    let body=serde_json::to_vec(&json!({"topic":topic,"payload":payload})).unwrap();
    let mut bytes=if guarded{format!("!P4C1:{:05}:{:08x}:",body.len(),control_integrity::fingerprint(&body)).into_bytes()}else{vec![]};
    bytes.extend(body);bytes.push(b'\n');bytes
}
fn run_reader<R:BufRead,F:Fn(String,Value)>(manager:&UsbSerialManager,reader:R,callback:F){
    let (cancel,capture)={let conn=manager.connection.lock().unwrap();let c=conn.as_ref().unwrap();(c.cancel_reader.clone(),c.capture_active.clone())};
    manager.read_connected(reader,1,115200,cancel,capture,true,Some(NONCE.into()),callback);
}
#[test]
fn reboot_offer_retires_real_reader_without_delivering_or_downgrading_hello(){
    let (manager,_,_)=manager(Mode::Ack);let seen=Arc::new(Mutex::new(vec![]));
    let old_cancel=manager.connection.lock().unwrap().as_ref().unwrap().cancel_reader.clone();
    let offer=json!({"boardDeviceId":BOARD,"runtime":"esp-p4","capabilities":{"serialControlOffer":control_integrity::ENGINE}});
    let mut bytes=frame("hello",offer,false);bytes.extend(frame("protocol/ack",json!({"ok":true}),false));
    let events=seen.clone();run_reader(&manager,Cursor::new(bytes),move|topic,_|events.lock().unwrap().push(topic));
    assert!(!manager.status().connected);assert!(seen.lock().unwrap().is_empty());
    // A fresh owner/probe has its own nonce, and the SAME production reader
    // accepts only its matching confirmation. No old business message is replayed.
    let nonce="1123456789abcdef0123456789abcdef";
    let hello=json!({"boardDeviceId":BOARD,"runtime":"esp-p4","capabilities":{"serialControlOffer":control_integrity::ENGINE,"serialControlIntegrity":control_integrity::ENGINE,"serialControlNonce":nonce,"debugBootId":"1123456789abcdef"}});
    // Match connect_with_probe_timeout: a verified new connection owns NEW
    // cancellation/capture tokens. Never revive or reuse the retired reader.
    let mut fresh=connection(&manager,BOARD,2,Mode::Ack,Arc::new(Mutex::new(vec![])),Arc::new(AtomicUsize::new(0)));
    apply_hello_payload_to_connection(&mut fresh,&hello);
    let cancel=fresh.cancel_reader.clone();let capture=fresh.capture_active.clone();
    assert!(old_cancel.load(Ordering::Acquire));
    assert!(!Arc::ptr_eq(&old_cancel,&cancel));assert!(!cancel.load(Ordering::Acquire));
    {
        let (_owner,mut slot)=manager.inbound_owner.connection(&manager.connection).unwrap();
        *slot=Some(fresh);manager.inbound_owner.publish(2);
    }
    manager.read_connected(Cursor::new(frame("hello",hello,true)),2,115200,cancel,capture,true,Some(nonce.into()),|topic,_|seen.lock().unwrap().push(topic));
    assert!(old_cancel.load(Ordering::Acquire),"fresh handshake must not revive the old reader");
    assert_eq!(*seen.lock().unwrap(),vec!["hello"]);
}
struct DeferredRead {bytes:Cursor<Vec<u8>>,entered:Option<mpsc::Sender<()>>,release:mpsc::Receiver<()>}
impl Read for DeferredRead {fn read(&mut self,b:&mut [u8])->io::Result<usize>{self.bytes.read(b)}}
impl BufRead for DeferredRead {
    fn fill_buf(&mut self)->io::Result<&[u8]>{if let Some(send)=self.entered.take(){send.send(()).unwrap();self.release.recv().unwrap();}self.bytes.fill_buf()}
    fn consume(&mut self,n:usize){self.bytes.consume(n)}
}
#[test]
fn retired_reader_cannot_deliver_buffered_business_event_or_resolve_waiter(){
    for receipt in [false,true] {
        let (manager,_,_)=manager(Mode::Ack);let old_cancel=manager.connection.lock().unwrap().as_ref().unwrap().cancel_reader.clone();
        let (entered,ready)=mpsc::channel();let (release,wait)=mpsc::channel();let called=Arc::new(AtomicBool::new(false));
        let bytes=if receipt{frame("asset/ack",json!({"transferId":"new-request","phase":"activate","ok":true}),true)}
            else{frame("media/event",json!({"global":true,"operation":"stop"}),false)};
        let worker=manager.clone();let fired=called.clone();let reader=thread::spawn(move||run_reader(&worker,DeferredRead{bytes:Cursor::new(bytes),entered:Some(entered),release:wait},move|_,_|{fired.store(true,Ordering::SeqCst);}));
        ready.recv().unwrap();manager.disconnect();
        {let _owner=manager.inbound_owner.lock().unwrap();*manager.connection.lock().unwrap()=Some(connection(&manager,"new-board",2,Mode::Ack,Arc::new(Mutex::new(vec![])),Arc::new(AtomicUsize::new(0))));manager.inbound_owner.publish(2);}
        let waiter=manager.register_asset_ack_waiter("new-request","activate",None,None).unwrap();
        assert!(old_cancel.load(Ordering::Acquire));release.send(()).unwrap();reader.join().unwrap();
        assert!(matches!(waiter.try_recv(),Err(mpsc::TryRecvError::Empty)));
        assert!(!called.load(Ordering::Acquire));assert_eq!(manager.status().board_device_id,"new-board");assert!(manager.status().connected);
    }
}
#[test]
fn dispatch_and_disconnect_linearize_without_holding_connection_or_waiter_locks(){
    let (manager,_,_)=manager(Mode::Ack);let (entered,ready)=mpsc::channel();let (release,wait)=mpsc::channel();
    let callback_manager=manager.clone();let reader_manager=manager.clone();
    let reader=thread::spawn(move||run_reader(&reader_manager,Cursor::new(frame("media/event",json!({"operation":"stop"}),false)),move|_,_|{
        assert!(callback_manager.connection.try_lock().is_ok());assert!(callback_manager.asset_ack_waiters.try_lock().is_ok());assert!(callback_manager.device_response_waiters.try_lock().is_ok());
        entered.send(()).unwrap();wait.recv().unwrap();
    }));
    ready.recv().unwrap();let replacement=manager.clone();let (done,finish)=mpsc::channel();
    let disconnect=thread::spawn(move||{replacement.disconnect();done.send(()).unwrap();});
    assert!(finish.recv_timeout(Duration::from_millis(20)).is_err());
    release.send(()).unwrap();reader.join().unwrap();finish.recv_timeout(Duration::from_secs(1)).unwrap();disconnect.join().unwrap();assert!(!manager.status().connected);
}
#[test]
fn changed_board_or_generation_gets_no_asset_query_begin_raw_or_cleanup(){
    for changed_board in [false,true]{
        let (manager,original,_)=manager(Mode::Ack);let replacement=Arc::new(Mutex::new(vec![]));let raw=Arc::new(AtomicUsize::new(0));
        let result:Result<(),String>=manager.with_appearance_attempt(BOARD,||{
            {let _owner=manager.inbound_owner.lock()?;*manager.connection.lock().unwrap()=Some(connection(&manager,if changed_board{"new-board"}else{BOARD},2,Mode::Ack,replacement.clone(),raw.clone()));manager.inbound_owner.publish(2);}
            assert!(manager.send_asset_begin_checked("owned",1).is_err());
            assert!(manager.send_asset_raw_chunk_checked("owned","p4/a.h264",b"abc",0).is_err());
            manager.best_effort_asset_abort("owned");
            Err("owner changed".into())
        });
        assert!(result.is_err());assert!(original.lock().unwrap().is_empty());assert!(replacement.lock().unwrap().is_empty());assert_eq!(raw.load(Ordering::Acquire),0);
    }
}
#[test]
fn real_commit_timeout_is_unknown_and_prevents_abort_fallback_and_second_attempt(){
    let (manager,trace,_)=manager(Mode::Drop);
    let result:Result<(),String>=manager.with_appearance_attempt(BOARD,||{
        manager.run_serial_asset_transaction("owned",||{
            manager.send_asset_command_and_wait_timeout("asset/commit",&json!({"transferId":"owned"}),"owned","commit",None,None,Duration::from_millis(10))?;Ok(())
        })
    });
    assert!(result.is_err());assert_eq!(trace.lock().unwrap().len(),1);assert_eq!(trace.lock().unwrap()[0]["topic"],"asset/commit");
    manager.best_effort_asset_abort("owned");
    assert!(manager.with_appearance_attempt(BOARD,||manager.send("asset/begin",&json!({"transferId":"second"}))).is_err());
    assert_eq!(trace.lock().unwrap().len(),1);
    // Read-only follow-up remains possible even while mutations are latched.
    assert!(manager.send("asset/slot-query",&json!({"transferId":"inspect"})).is_ok());assert_eq!(trace.lock().unwrap().len(),2);
}
#[test]
fn unknown_raw_ack_sends_one_header_one_body_no_padding_retry_or_abort(){
    let (manager,trace,raw)=manager(Mode::Close);
    let result=manager.with_appearance_attempt(BOARD,||manager.run_serial_asset_transaction("raw-owned",||manager.send_asset_raw_chunk_checked("raw-owned","p4/a.h264",b"abc",0)));
    assert!(result.is_err());assert_eq!(trace.lock().unwrap().len(),1);assert_eq!(trace.lock().unwrap()[0]["topic"],"asset/raw-chunk");assert_eq!(raw.load(Ordering::Acquire),3);
}
#[test]
fn durable_unknown_survives_manager_and_journal_reopen_and_known_ack_clears_only_its_own_intent(){
    let directory=tempfile::tempdir().unwrap();
    {
        let (mut manager,trace,_)=manager(Mode::Close);manager.appearance_journal=Arc::new(appearance_safety::Journal::open(directory.path()).unwrap());
        assert!(manager.with_appearance_attempt(BOARD,||manager.send_asset_command_and_wait_timeout("asset/activate",&json!({"transferId":"owned"}),"owned","activate",None,None,Duration::from_millis(10))).is_err());
        assert_eq!(trace.lock().unwrap().len(),1);
        let state:Value=serde_json::from_slice(&std::fs::read(directory.path().join("pending-v1.json")).unwrap()).unwrap();
        assert_eq!(state["pending"][0]["board"],BOARD);assert_eq!(state["pending"][0]["generation"],1);assert_eq!(state["pending"][0]["phase"],"asset/activate");assert_eq!(state["pending"][0]["state"],"submitted_unknown");assert_eq!(state["pending"][0]["transferId"],"owned");
    }
    let (mut manager,trace,_)=manager(Mode::Ack);manager.appearance_journal=Arc::new(appearance_safety::Journal::open(directory.path()).unwrap());
    assert!(manager.with_appearance_attempt(BOARD,||manager.send("asset/begin",&json!({"transferId":"owned"}))).is_err());assert!(trace.lock().unwrap().is_empty());
    // A confirmed operation for a DIFFERENT board cannot erase BOARD's latch.
    {let _owner=manager.inbound_owner.lock().unwrap();*manager.connection.lock().unwrap()=Some(connection(&manager,"other-board",2,Mode::Ack,trace.clone(),Arc::new(AtomicUsize::new(0))));manager.inbound_owner.publish(2);}
    manager.with_appearance_attempt("other-board",||manager.send_asset_command_and_wait_timeout("asset/activate",&json!({"transferId":"known"}),"known","activate",None,None,Duration::from_millis(10))).unwrap();
    let state:Value=serde_json::from_slice(&std::fs::read(directory.path().join("pending-v1.json")).unwrap()).unwrap();assert_eq!(state["pending"].as_array().unwrap().len(),1);assert_eq!(state["pending"][0]["board"],BOARD);
}
#[test]
fn journal_failure_prevents_first_device_write_and_partial_send_retains_unknown(){
    let directory=tempfile::tempdir().unwrap();let (mut manager,trace,_)=manager(Mode::Ack);manager.appearance_journal=Arc::new(appearance_safety::Journal::open(directory.path()).unwrap());
    std::fs::remove_file(directory.path().join("pending-v1.json")).unwrap();std::fs::create_dir(directory.path().join("pending-v1.json")).unwrap();
    assert!(manager.with_appearance_attempt(BOARD,||manager.send("asset/begin",&json!({"transferId":"owned"}))).is_err());assert!(trace.lock().unwrap().is_empty());
    let (manager,partial,_)=self::manager(Mode::Partial);
    assert!(manager.with_appearance_attempt(BOARD,||manager.send("asset/begin",&json!({"transferId":"owned"}))).is_err());
    assert_eq!(partial.lock().unwrap().as_slice(),&[json!({"partialBytes":1})]);
    assert!(manager.with_appearance_attempt(BOARD,||Ok(())).is_err());
}

#[test]
fn native_fallback_is_possible_only_before_first_mutation_and_ids_are_bounded(){
    let (manager,trace,_)=manager(Mode::Ack);
    manager.with_appearance_attempt(BOARD,||{
        let before:Option<()>=manager.native_appearance_or_fallback(||Err("native unavailable before submission".into()))?;
        assert!(before.is_none());
        let after:Result<Option<()>,String>=manager.native_appearance_or_fallback(||{
            manager.send_asset_command_and_wait_timeout("asset/activate",&json!({"transferId":"native-owned"}),"native-owned","activate",None,None,Duration::from_millis(10))?;
            Err("lost native outcome".into())
        });
        assert!(after.is_err());
        assert!(manager.send("asset/begin",&json!({"transferId":"fallback"})).is_err());
        Err::<(),_>("native unknown".into())
    }).unwrap_err();
    assert_eq!(trace.lock().unwrap().len(),1);
    let (manager,trace,_)=self::manager(Mode::Ack);
    for id in [Value::Null,json!(""),json!("x".repeat(64)),json!("bad id"),json!("bad\n")]{
        assert!(manager.send("asset/begin",&json!({"transferId":id})).is_err());
    }
    assert!(trace.lock().unwrap().is_empty());
}

struct ChannelRead {rx:mpsc::Receiver<Option<Vec<u8>>>,bytes:Vec<u8>,offset:usize}
impl Read for ChannelRead {
    fn read(&mut self,out:&mut[u8])->io::Result<usize>{let input=self.fill_buf()?;let n=input.len().min(out.len());out[..n].copy_from_slice(&input[..n]);self.consume(n);Ok(n)}
}
impl BufRead for ChannelRead {
    fn fill_buf(&mut self)->io::Result<&[u8]>{
        if self.offset==self.bytes.len(){self.bytes=self.rx.recv().unwrap_or(None).unwrap_or_default();self.offset=0;}
        Ok(&self.bytes[self.offset..])
    }
    fn consume(&mut self,n:usize){self.offset+=n;}
}
struct RawReaderWriter {tx:mpsc::Sender<Option<Vec<u8>>>,pending:Vec<u8>,raw:Option<(Value,usize)>}
impl Write for RawReaderWriter {
    fn write(&mut self,bytes:&[u8])->io::Result<usize>{
        if let Some((p,remaining))=&mut self.raw {
            let count=bytes.len().min(*remaining);*remaining-=count;
            if *remaining==0 {self.tx.send(Some(frame("asset/ack",json!({"transferId":p["transferId"],"path":p["path"],"index":p["index"],"phase":"raw-chunk","ok":true}),true))).unwrap();self.raw=None;}
            return Ok(count);
        }
        self.pending.extend_from_slice(bytes);
        if let Some(end)=self.pending.iter().position(|b|*b==b'\n'){
            let header=self.pending.drain(..=end).collect::<Vec<_>>();let header:Value=serde_json::from_slice(&header).unwrap();assert_eq!(header["topic"],"asset/raw-chunk");let p=header["payload"].clone();
            self.tx.send(Some(frame("asset/ack",json!({"transferId":p["transferId"],"path":p["path"],"index":p["index"],"phase":"raw-ready","ok":true}),true))).unwrap();
            self.raw=Some((p.clone(),p["size"].as_u64().unwrap() as usize));
        }
        Ok(bytes.len())
    }
    fn flush(&mut self)->io::Result<()>{Ok(())}
}
#[test]
fn raw_ready_ack_reader_never_waits_for_the_writer_connection_mutex(){
    let (manager,_,_)=manager(Mode::Ack);let (send,receive)=mpsc::channel();
    {let mut c=manager.connection.lock().unwrap();let c=c.as_mut().unwrap();c.capabilities["transport"]=json!({"rawAssetReadyAck":true});
        c.writer=Box::new(RawReaderWriter{tx:send.clone(),pending:vec![],raw:None});}
    let (cancel,capture)={let c=manager.connection.lock().unwrap();let c=c.as_ref().unwrap();(c.cancel_reader.clone(),c.capture_active.clone())};
    let copy=manager.clone();let reader=thread::spawn(move||copy.read_connected(
        ChannelRead{rx:receive,bytes:vec![],offset:0},1,115200,cancel,capture,true,Some(NONCE.into()),|_,_|{}
    ));
    // The physical stream stays reserved across raw-ready, but neither
    // connection nor dispatch gate may prevent the real RX waiter progressing.
    let result=manager.with_appearance_attempt(BOARD,||manager.send_asset_raw_chunk_checked("owned-raw","p4/a.h264",b"bounded",0));
    send.send(None).unwrap();reader.join().unwrap();assert!(result.is_ok(),"actual RX must progress while the physical stream is reserved");
}

#[test]
fn lifecycle_waiting_for_a_writer_does_not_block_the_reader_that_releases_it(){
    let (manager,_,_)=manager(Mode::Ack);
    let (cancel,capture)={let c=manager.connection.lock().unwrap();let c=c.as_ref().unwrap();(c.cancel_reader.clone(),c.capture_active.clone())};
    let writer=manager.connection.lock().unwrap();let (delivered,receive)=mpsc::channel();
    let copy=manager.clone();let disconnect=thread::spawn(move||copy.disconnect());
    let copy=manager.clone();let reader=thread::spawn(move||copy.read_connected(
        Cursor::new(frame("media/event",json!({"operation":"stop"}),false)),1,115200,cancel,capture,true,Some(NONCE.into()),move|_,_|{delivered.send(()).unwrap();}
    ));
    let received=receive.recv_timeout(Duration::from_secs(1));drop(writer);
    reader.join().unwrap();disconnect.join().unwrap();assert!(received.is_ok(),"lifecycle must release dispatch while waiting for writer mutex");
}

// Freeze a real retired reader immediately before its delayed connected=false
// store. This hook changes no production admission and is thread-local.
thread_local! {static BEFORE_DISCONNECT:std::cell::RefCell<Option<Box<dyn FnOnce()>>>=const{std::cell::RefCell::new(None)};}
pub(super) fn pause_before_disconnect(){BEFORE_DISCONNECT.with(|slot|{if let Some(pause)=slot.borrow_mut().take(){pause();}});}
#[test]
fn phase3_retired_still_connected_object_cannot_send_or_complete_intent(){
    let directory=tempfile::tempdir().unwrap();let (mut manager,trace,raw)=manager(Mode::Ack);
    manager.appearance_journal=Arc::new(appearance_safety::Journal::open(directory.path()).unwrap());
    let (paused,ready)=mpsc::channel();let (release,resume)=mpsc::channel();let mut thread=None;
    let result:Result<(),String>=manager.with_appearance_attempt(BOARD,||{
        manager.send_asset_command_and_wait_timeout("asset/activate",&json!({"transferId":"retired-owned"}),"retired-owned","activate",None,None,Duration::from_millis(10))?;
        let (cancel,capture)={let c=manager.connection.lock().unwrap();let c=c.as_ref().unwrap();(c.cancel_reader.clone(),c.capture_active.clone())};
        let held=manager.connection.lock().unwrap();let copy=manager.clone();
        thread=Some(std::thread::spawn(move||{
            BEFORE_DISCONNECT.with(|slot|*slot.borrow_mut()=Some(Box::new(move||{paused.send(()).unwrap();resume.recv().unwrap();})));
            copy.read_connected(Cursor::new(frame("hello",json!({"boardDeviceId":BOARD,"runtime":"esp-p4","capabilities":{"serialControlOffer":control_integrity::ENGINE}}),false)),1,115200,cancel,capture,true,Some(NONCE.into()),|_,_|panic!("retired offer dispatched"));
        }));
        ready.recv_timeout(Duration::from_secs(1)).unwrap();
        let c=held.as_ref().unwrap();assert!(c.connected);assert_eq!(c.connection_id,1);assert!(c.cancel_reader.load(Ordering::Acquire));assert!(!manager.inbound_owner.is_current(1));drop(held);
        assert!(manager.send_asset_begin_checked("retired-owned",3).is_err());
        assert!(manager.send_asset_raw_chunk_checked("retired-owned","p4/a.h264",b"abc",0).is_err());
        assert!(manager.check_appearance_connection().is_err()); // native frame admission
        assert!(manager.send_owned_notes_control(BOARD,1,"owned-notes","stop").is_err());
        let c=manager.connection.lock().unwrap();assert!(c.as_ref().unwrap().connected);assert_eq!(c.as_ref().unwrap().connection_id,1);
        Ok(()) // final-success admission must ALSO reject; never clear the intent
    });
    release.send(()).unwrap();thread.unwrap().join().unwrap();
    assert!(result.is_err());assert_eq!(trace.lock().unwrap().len(),1);assert_eq!(raw.load(Ordering::Acquire),0);
    let disk:Value=serde_json::from_slice(&std::fs::read(directory.path().join("pending-v1.json")).unwrap()).unwrap();assert_eq!(disk["pending"].as_array().unwrap().len(),1);
}
#[test]
fn phase3_cancel_alone_blocks_appearance_and_owned_notes_with_same_live_generation(){
    let (manager,trace,_)=manager(Mode::Ack);
    let result:Result<(),String>=manager.with_appearance_attempt(BOARD,||{
        {let _lease=manager.inbound_owner.lock()?;manager.connection.lock().unwrap().as_ref().unwrap().cancel_reader.store(true,Ordering::Release);}
        assert!(manager.inbound_owner.is_current(1));assert!(manager.connection.lock().unwrap().as_ref().unwrap().connected);
        assert!(manager.send("asset/begin",&json!({"transferId":"cancel-owned"})).is_err());
        assert!(manager.send_owned_notes_control(BOARD,1,"notes-owned","stop").is_err());
        assert!(manager.check_appearance_connection().is_err());Ok(())
    });
    assert!(result.is_err());assert!(trace.lock().unwrap().is_empty());
}
struct WindowWriter {tx:mpsc::Sender<Option<Vec<u8>>>,pending:Vec<u8>,raw:Option<(Value,usize)>,trace:Arc<Mutex<Vec<Value>>>,body:Arc<Mutex<Vec<u8>>>,event:Option<(String,Value,bool)>}
impl Write for WindowWriter {
    fn write(&mut self,bytes:&[u8])->io::Result<usize>{
        if let Some((p,remaining))=&mut self.raw {
            let n=bytes.len().min(*remaining);self.body.lock().unwrap().extend_from_slice(&bytes[..n]);*remaining-=n;
            if *remaining==0{self.trace.lock().unwrap().push(json!({"kind":"raw-body"}));self.tx.send(Some(frame("asset/ack",json!({"transferId":p["transferId"],"path":p["path"],"index":p["index"],"phase":"raw-chunk","ok":true}),true))).unwrap();self.raw=None;}
            return Ok(n);
        }
        self.pending.extend_from_slice(bytes);
        while let Some(end)=self.pending.iter().position(|b|*b==b'\n'){
            let request:Value=serde_json::from_slice(&self.pending.drain(..=end).collect::<Vec<_>>()).unwrap();self.trace.lock().unwrap().push(request.clone());
            if request["topic"]=="asset/raw-chunk" {
                let p=&request["payload"];self.raw=Some((p.clone(),p["size"].as_u64().unwrap() as usize));
                if let Some((topic,payload,framed))=self.event.take(){self.tx.send(Some(frame(&topic,payload,framed))).unwrap();}
                self.tx.send(Some(frame("asset/ack",json!({"transferId":p["transferId"],"path":p["path"],"index":p["index"],"phase":"raw-ready","ok":true}),true))).unwrap();
            }else{assert_eq!(request["topic"],"audio/control");}
        }
        Ok(bytes.len())
    }
    fn flush(&mut self)->io::Result<()>{Ok(())}
}
#[test]
fn phase3_retirement_after_ready_never_writes_the_prepared_raw_body(){
    let (manager,trace,_)=manager(Mode::Ack);let (send,receive)=mpsc::channel();let body=Arc::new(Mutex::new(vec![]));
    {let mut c=manager.connection.lock().unwrap();let c=c.as_mut().unwrap();c.capabilities["transport"]=json!({"rawAssetReadyAck":true});c.writer=Box::new(WindowWriter{tx:send,pending:vec![],raw:None,trace:trace.clone(),body:body.clone(),event:None});}
    let (cancel,capture)={let c=manager.connection.lock().unwrap();let c=c.as_ref().unwrap();(c.cancel_reader.clone(),c.capture_active.clone())};
    let (paused,ready)=mpsc::channel();let (release,resume)=mpsc::channel();let worker=manager.clone();let callback=manager.clone();
    let reader=std::thread::spawn(move||{
        BEFORE_DISCONNECT.with(|slot|*slot.borrow_mut()=Some(Box::new(move||{paused.send(()).unwrap();resume.recv().unwrap();})));
        let stop=cancel.clone();worker.read_connected(ChannelRead{rx:receive,bytes:vec![],offset:0},1,115200,cancel,capture,true,Some(NONCE.into()),move|topic,payload|{
            if topic=="asset/ack"&&payload["phase"]=="raw-ready"{
                callback.inbound_owner.retire(1);stop.store(true,Ordering::Release);
                let c=callback.connection.lock().unwrap();assert!(c.as_ref().unwrap().connected);assert_eq!(c.as_ref().unwrap().connection_id,1);
            }
        });
    });
    let result=manager.with_appearance_attempt(BOARD,||manager.send_asset_raw_chunk_checked("raw-retired","p4/a.h264",b"prepared-body",0));
    ready.recv_timeout(Duration::from_secs(1)).unwrap();
    {let c=manager.connection.lock().unwrap();assert!(!c.as_ref().unwrap().connected);assert_eq!(c.as_ref().unwrap().connection_id,1);}
    assert!(result.is_err());assert!(body.lock().unwrap().is_empty());assert_eq!(trace.lock().unwrap().len(),1);
    release.send(()).unwrap();reader.join().unwrap();
}

#[test]
fn phase3_real_notes_stop_and_error_callbacks_leave_raw_stream_contiguous(){
    for error in [false,true]{
        let (manager,trace,_)=manager(Mode::Ack);let id="phase3-owned-notes";
        let (_capture_guard,token)=crate::component_notes::install_owned_capture_for_test(manager.clone(),BOARD,1,id);
        let (send,receive)=mpsc::channel();let body=Arc::new(Mutex::new(vec![]));
        let event=if error{("protocol/ack".to_owned(),json!({"requestTopic":"audio/control","requestId":format!("notes-{token}-start"),"ok":false}),true)}
            else{("app/event".to_owned(),json!({"id":id,"visible":false}),true)};
        {let mut c=manager.connection.lock().unwrap();let c=c.as_mut().unwrap();c.capabilities["transport"]=json!({"rawAssetReadyAck":true});c.writer=Box::new(WindowWriter{tx:send.clone(),pending:vec![],raw:None,trace:trace.clone(),body:body.clone(),event:Some(event)});}
        let (cancel,capture)={let c=manager.connection.lock().unwrap();let c=c.as_ref().unwrap();(c.cancel_reader.clone(),c.capture_active.clone())};
        let (wake,work)=mpsc::channel();let worker=std::thread::spawn(move||{
            work.recv_timeout(Duration::from_secs(2)).unwrap();
            crate::component_notes::drain_control();crate::component_notes::drain_control();
        });
        let reader_manager=manager.clone();let reader=std::thread::spawn(move||reader_manager.read_connected(
            ChannelRead{rx:receive,bytes:vec![],offset:0},1,115200,cancel,capture,true,Some(NONCE.into()),move|topic,payload|{
                if topic=="app/event"{crate::component_notes::stop_recording(id);wake.send(()).unwrap();}
                else if topic=="protocol/ack"{assert!(crate::component_notes::route(&topic,&payload));wake.send(()).unwrap();}
            }
        ));
        let result=manager.with_appearance_attempt(BOARD,||manager.send_asset_raw_chunk_checked("notes-window","p4/a.h264",b"bounded-body",0));
        // Keep the real reader alive until the reserved worker has sent stop;
        // ending it early would correctly retire this generation.
        worker.join().unwrap();send.send(None).unwrap();reader.join().unwrap();
        assert!(result.is_ok());assert_eq!(body.lock().unwrap().as_slice(),b"bounded-body");
        let trace=trace.lock().unwrap();assert_eq!(trace.len(),3);
        assert_eq!(trace[0]["topic"],"asset/raw-chunk");assert_eq!(trace[1]["kind"],"raw-body");
        assert_eq!(trace[2]["topic"],"audio/control");assert_eq!(trace[2]["payload"]["action"],"stop");
        assert_eq!(trace[2]["payload"]["captureOwner"],token);assert_eq!(trace[2]["payload"]["requestId"],format!("notes-{token}-stop"));
    }
}

fn usb_candidate(serial:&str)->UsbDeviceInfo{UsbDeviceInfo{port_name:"memory-candidate".into(),vid:0x1a86,pid:0x55d4,serial_number:serial.into(),manufacturer:String::new(),product:String::new()}}
#[test]
fn phase3_missing_ready_or_terminal_ack_blocks_notes_generic_and_probe_across_restart(){
    for ready in [true,false]{
        let directory=tempfile::tempdir().unwrap();let port="/dev/cu.phase3-owned";
        {
            let (mut manager,trace,raw)=manager(Mode::Drop);manager.appearance_journal=Arc::new(appearance_safety::Journal::open(directory.path()).unwrap());
            {let mut c=manager.connection.lock().unwrap();let c=c.as_mut().unwrap();c.port_name=port.into();c.capabilities["transport"]=json!({"rawAssetReadyAck":ready});}
            *manager.last_serial_baud.lock().unwrap()=Some((format!("{port}|1a86|55d4|owned-serial"),115200));
            let (_notes,_) = crate::component_notes::install_owned_capture_for_test(manager.clone(),BOARD,1,"raw-unknown-notes");
            let result=manager.with_appearance_attempt(BOARD,||manager.send_asset_raw_chunk_checked("unknown-raw","p4/a.h264",b"bounded",0));
            assert!(result.is_err());assert!(!manager.status().connected);assert!(!manager.inbound_owner.is_current(1));
            assert!(manager.connection.lock().unwrap().as_ref().unwrap().cancel_reader.load(Ordering::Acquire));
            crate::component_notes::stop_recording("raw-unknown-notes");
            std::thread::spawn(||{crate::component_notes::drain_control();crate::component_notes::drain_control();}).join().unwrap();
            assert!(manager.send("system/heartbeat",&json!({})).is_err());
            assert!(manager.connect_for_auto(port,|_,_|panic!("no probe/open/callback permitted")).is_err());
            assert!(manager.connect_for_auto("/dev/tty.phase3-owned",|_,_|panic!("alias must remain quarantined")).is_err());
            assert!(manager.ensure_probe_candidate("/dev/cu.renamed",Some(&usb_candidate("owned-serial"))).is_err());
            assert!(manager.ensure_probe_candidate("/dev/cu.other",Some(&usb_candidate("different-device"))).is_ok());
            assert!(manager.ensure_wire_available("/dev/cu.other",Some("other-board")).is_ok());
            assert_eq!(trace.lock().unwrap().len(),1);assert_eq!(trace.lock().unwrap()[0]["topic"],"asset/raw-chunk");
            assert_eq!(raw.load(Ordering::Acquire),if ready{0}else{7});
            let disk:Value=serde_json::from_slice(&std::fs::read(directory.path().join("pending-v1.json")).unwrap()).unwrap();
            assert_eq!(disk["pending"].as_array().unwrap().len(),1);assert_eq!(disk["quarantines"].as_array().unwrap().len(),1);assert_eq!(disk["quarantines"][0]["port"],port);
        }
        let mut reopened=UsbSerialManager::new();reopened.appearance_journal=Arc::new(appearance_safety::Journal::open(directory.path()).unwrap());
        assert!(reopened.connect_for_auto(port,|_,_|panic!("restart cannot erase raw quarantine")).is_err());
        assert!(reopened.ensure_probe_candidate("/dev/cu.renamed",Some(&usb_candidate("owned-serial"))).is_err());
        assert!(reopened.ensure_probe_candidate("/dev/cu.other",Some(&usb_candidate("different-device"))).is_ok());
    }
}
#[test]
fn phase3_exact_terminal_nack_exits_raw_mode_but_never_confirms_the_operation(){
    let directory=tempfile::tempdir().unwrap();let (mut manager,trace,raw)=manager(Mode::Nack);
    manager.appearance_journal=Arc::new(appearance_safety::Journal::open(directory.path()).unwrap());
    let result=manager.with_appearance_attempt(BOARD,||manager.send_asset_raw_chunk_checked("terminal-nack","p4/a.h264",b"bounded",0));
    assert!(result.is_err());assert!(manager.status().connected);assert_eq!(raw.load(Ordering::Acquire),7);
    manager.send("asset/slot-query",&json!({"transferId":"read-only-proof"})).unwrap();assert_eq!(trace.lock().unwrap().len(),2);
    let disk:Value=serde_json::from_slice(&std::fs::read(directory.path().join("pending-v1.json")).unwrap()).unwrap();
    assert!(disk["quarantines"].as_array().unwrap().is_empty());assert_eq!(disk["pending"].as_array().unwrap().len(),1);
    assert!(manager.with_appearance_attempt(BOARD,||Ok(())).is_err());
}
#[test]
fn phase3_legacy_empty_journal_migrates_but_missing_metadata_pending_never_guesses_safe(){
    for pending in [false,true]{
        let directory=tempfile::tempdir().unwrap();let path=directory.path().join("pending-v1.json");
        let rows=if pending{vec![json!({"board":BOARD,"generation":1,"attempt":uuid::Uuid::new_v4().to_string(),"phase":"asset/begin","state":"submitted_unknown","transferId":"old-unknown"})]}else{vec![]};
        let original=serde_json::to_vec(&json!({"pending":rows})).unwrap();std::fs::write(&path,&original).unwrap();
        let (mut manager,trace,_)=manager(Mode::Ack);manager.appearance_journal=Arc::new(appearance_safety::Journal::open(directory.path()).unwrap());
        if pending{
            assert!(manager.connect_for_auto("memory-port",|_,_|panic!("legacy unknown cannot probe")).is_err());
            assert!(manager.ensure_probe_candidate("memory-port",Some(&usb_candidate("unrelated-but-unprovable"))).is_err());
            assert_eq!(std::fs::read(&path).unwrap(),original);assert!(trace.lock().unwrap().is_empty());
        }else{
            manager.with_appearance_attempt(BOARD,||manager.send_asset_command_and_wait_timeout("asset/activate",&json!({"transferId":"new-known"}),"new-known","activate",None,None,Duration::from_millis(10))).unwrap();
            let disk:Value=serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();assert_eq!(disk["schema_version"],2);assert!(disk["pending"].as_array().unwrap().is_empty());assert_eq!(trace.lock().unwrap().len(),1);
        }
    }
}

#[test]
fn phase3_one_quarantined_device_does_not_block_a_verified_different_raw_device(){
    let directory=tempfile::tempdir().unwrap();let (mut manager,_,_)=manager(Mode::Close);
    manager.appearance_journal=Arc::new(appearance_safety::Journal::open(directory.path()).unwrap());
    *manager.last_serial_baud.lock().unwrap()=Some(("memory-only-no-port|1a86|55d4|device-a".into(),115200));
    assert!(manager.with_appearance_attempt(BOARD,||manager.send_asset_raw_chunk_checked("a-unknown","p4/a.h264",b"old",0)).is_err());
    assert!(manager.ensure_probe_candidate("other-port",Some(&usb_candidate("device-b"))).is_ok());
    let trace=Arc::new(Mutex::new(vec![]));let bytes=Arc::new(AtomicUsize::new(0));
    {
        let (_lease,mut slot)=manager.inbound_owner.connection(&manager.connection).unwrap();
        let mut c=connection(&manager,"other-board",2,Mode::Ack,trace.clone(),bytes.clone());c.port_name="other-port".into();
        *slot=Some(c);manager.inbound_owner.publish(2);
    }
    *manager.last_serial_baud.lock().unwrap()=Some(("other-port|1a86|55d4|device-b".into(),115200));
    manager.with_appearance_attempt("other-board",||manager.send_asset_raw_chunk_checked("b-known","p4/a.h264",b"new",0)).unwrap();
    assert_eq!(bytes.load(Ordering::Acquire),3);assert_eq!(trace.lock().unwrap().len(),1);
    let disk:Value=serde_json::from_slice(&std::fs::read(directory.path().join("pending-v1.json")).unwrap()).unwrap();
    assert_eq!(disk["pending"].as_array().unwrap().len(),1);assert_eq!(disk["pending"][0]["board"],BOARD);
    assert_eq!(disk["quarantines"].as_array().unwrap().len(),1);assert_eq!(disk["quarantines"][0]["board"],BOARD);
}
