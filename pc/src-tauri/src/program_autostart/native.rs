//! One bounded control worker. RX only try_sends; no request/reply or disk waits.
use super::*;
use crate::usb_serial::{UsbConnectionStatus,UsbSerialManager};
use std::sync::atomic::{AtomicBool,AtomicU64,Ordering};
use std::sync::{mpsc,OnceLock,Weak};

static RUNTIME: OnceLock<Runtime> = OnceLock::new();
static NOTES_WORKER:AtomicBool=AtomicBool::new(false);
pub(crate) fn notes_worker_ready()->bool{NOTES_WORKER.load(Ordering::Acquire)}
/// The intent lives in notes' reserved slot, not this wake queue. Queue-full
/// merely coalesces a wake; periodic drain and every event retain the stop.
pub(crate) fn notes_control(){if let Some(r)=RUNTIME.get(){let _=r.sender.try_send(Event::NotesControl);}}

struct Admission { epoch:AtomicU64, blocked:AtomicBool }
impl Admission {
    const fn new()->Self{Self{epoch:AtomicU64::new(1),blocked:AtomicBool::new(false)}}
    fn current(&self)->Option<u64>{let n=self.epoch.load(Ordering::Acquire);(!self.blocked.load(Ordering::Acquire)&&n!=u64::MAX).then_some(n)}
    fn block(&self){self.blocked.store(true,Ordering::Release);}
    fn invalidate(&self){if self.epoch.fetch_update(Ordering::AcqRel,Ordering::Acquire,|n|n.checked_add(1)).is_err(){self.block();}}
}
static ADMISSION: Admission = Admission::new();
struct Runtime { service: Weak<ProgramAutostart>, sender: mpsc::SyncSender<Event>, last: Mutex<Option<(String,u64,String)>>, boot:Mutex<Option<(String,u64,String)>> }
#[derive(Debug)]
enum Event { NotesControl, Connected(String,u64,String), Suppress(String,u64,Option<String>) }
pub(crate) fn verified_boot(board:&str,generation:u64,boot:&str){
    if let Some(r)=RUNTIME.get(){if let Ok(mut value)=r.boot.lock(){*value=Some((board.into(),generation,boot.into()));}}
}
fn suppress_event(board:String,generation:u64)->Event{
    let boot=RUNTIME.get().and_then(|r|r.boot.try_lock().ok()).and_then(|v|v.as_ref().filter(|(b,g,_)|b==&board&&*g==generation).map(|(_,_,boot)|boot.clone()));
    Event::Suppress(board,generation,boot)
}
pub(crate) fn admission() -> Option<u64> {
    ADMISSION.current()
}
fn invalidate() {
    ADMISSION.invalidate();
}
fn enqueue(event:Event) {
    if let Some(r)=RUNTIME.get(){if r.sender.try_send(event).is_err(){ADMISSION.block();}}
}
pub(crate) fn configure(directory:&Path,home:PathBuf,usb:UsbSerialManager)->ProgramAutostartState {
    let host=Arc::new(crate::usb_serial::program_autostart_host::UsbHost::new(usb.clone(),home));
    let state=ProgramAutostartState::open(directory,host);
    let Some(service)=state.service.as_ref() else{return state;};
    let (sender,receiver)=mpsc::sync_channel(8);
    let weak=Arc::downgrade(service);
    if RUNTIME.set(Runtime{service:weak.clone(),sender,last:Mutex::new(None),boot:Mutex::new(None)}).is_err(){return ProgramAutostartState::unavailable();}
    let worker_usb=usb.clone();
    if std::thread::Builder::new().name("program-autostart-owner".into()).spawn(move||{
        struct Live;
        impl Drop for Live{fn drop(&mut self){NOTES_WORKER.store(false,Ordering::Release);}}
        let _live=Live;
        NOTES_WORKER.store(true,Ordering::Release);
        loop{
            crate::component_notes::drain_control();
            let event=match receiver.recv_timeout(std::time::Duration::from_millis(25)){
                Ok(event)=>event,Err(mpsc::RecvTimeoutError::Timeout)=>continue,
                Err(mpsc::RecvTimeoutError::Disconnected)=>{NOTES_WORKER.store(false,Ordering::Release);break;}
            };
            if matches!(event,Event::NotesControl){continue;}
            let Some(service)=weak.upgrade() else{continue;};
            match event {
                Event::NotesControl=>{},
                Event::Connected(board,generation,hint)=>{
                    if service.device_changed(&board).is_err(){ADMISSION.block();continue;}
                    if admission().is_none(){continue;}
                    // Same-generation native query, never a guessed status field.
                    if let Ok(boot)=worker_usb.autostart_boot(&board,generation){
                        if !hint.is_empty()&&hint!=boot{continue;}
                        let _=service.observe(&board,&boot);
                    }
                }
                Event::Suppress(board,generation,boot)=>{
                    match boot.map(Ok).unwrap_or_else(||worker_usb.autostart_boot(&board,generation)){
                        Ok(boot)=>{if service.suppress_boot(&board,&boot).is_err(){ADMISSION.block();}},
                        Err(_)=>ADMISSION.block(),
                    }
                }
            }
        }
    }).is_err(){ADMISSION.block();return ProgramAutostartState::unavailable();}
    connected(&usb);state
}
/// Call after real handshake and after each validated HELLO identity refresh.
pub(crate) fn connected(usb:&UsbSerialManager) {
    if let Some((status,generation))=usb.autostart_connection(){connected_status(&status,generation);}
}
pub(crate) fn connected_status(status:&UsbConnectionStatus,generation:u64) {
    let Some(r)=RUNTIME.get() else{return;};
    if !status.connected||generation==0||!board_valid(&status.board_device_id){return;}
    let hint=status.capabilities["debugBootId"].as_str().filter(|s|hex(s,16)).unwrap_or("").to_owned();
    let key=(status.board_device_id.clone(),generation,hint.clone());
    let Ok(mut last)=r.last.try_lock() else{ADMISSION.block();return;};
    if last.as_ref()==Some(&key){return;}
    // Every identity transition is retained in the bounded mailbox. Overflow
    // disables admission rather than discarding a switch-away-and-back event.
    invalidate();*last=Some(key);enqueue(Event::Connected(status.board_device_id.clone(),generation,hint));
}
pub(crate) fn preempt(usb:&UsbSerialManager) {
    if RUNTIME.get().is_none(){return;}
    invalidate();
    match usb.autostart_connection(){Some((status,generation))=>enqueue(suppress_event(status.board_device_id,generation)),None=>ADMISSION.block()}
}
/// RX passes its already validated connection generation; never reacquire the
/// serial connection mutex or wait for a reply here.
pub(crate) fn preempt_rx(board:&str,generation:u64) {
    if RUNTIME.get().is_none(){return;}
    invalidate();enqueue(suppress_event(board.to_owned(),generation));
}
pub(crate) fn component_changed(id:&str,deleted:bool) {
    if RUNTIME.get().is_none(){return;}
    invalidate();
    if let Some(service)=RUNTIME.get().and_then(|r|r.service.upgrade()){
        if service.component_changed(id,deleted).is_err(){ADMISSION.block();}
    }
}
pub(crate) fn suppress_now(board:&str,boot:&str) {
    if RUNTIME.get().is_none(){return;}
    invalidate();
    if let Some(service)=RUNTIME.get().and_then(|r|r.service.upgrade()){
        if service.suppress_boot(board,boot).is_err(){ADMISSION.block();}
    }
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn preemption_invalidates_inflight_epoch_without_automatic_readmission(){
        let gate=Admission::new();let before=gate.current().unwrap();gate.invalidate();
        assert_ne!(gate.current(),Some(before));gate.block();gate.invalidate();assert_eq!(gate.current(),None);
    }
    #[test]fn bounded_mailbox_never_waits_or_discards_overflow_as_a_new_permission(){
        let (send,_receive)=mpsc::sync_channel(1);send.try_send(Event::Suppress("a".into(),1,None)).unwrap();
        let gate=Admission::new();if send.try_send(Event::Suppress("b".into(),2,None)).is_err(){gate.block();}
        assert_eq!(gate.current(),None);
    }
}
