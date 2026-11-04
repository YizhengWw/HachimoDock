//! Durable metadata-only unknown-result latch, scoped to an exact appearance
//! board/generation. No automatic replay, abort, reset, expiration or eviction.
use super::*;
use std::{fs::{self,File,OpenOptions},io::{Read,Write},sync::atomic::{AtomicBool,Ordering}};
use serde::{Deserialize,Serialize};
const ERROR:&str="appearance_safety_journal_unavailable";
pub(super) const UNKNOWN:&str="appearance_result_unknown_read_only_no_replay";
const MAX_BYTES:u64=65536;
#[derive(Clone,PartialEq,Eq,Serialize,Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
pub(super) struct Pending {board:String,generation:u64,attempt:String,phase:String,state:String,transfer_id:String}
#[derive(Clone,Serialize,Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
struct RawQuarantine {board:String,generation:u64,attempt:String,port:String,#[serde(default)] identity:Option<UsbIdentity>}
#[derive(Clone,Serialize,Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct UsbIdentity {vid:u16,pid:u16,serial:String}
fn port_key(port:&str)->String{
    if let Some(tail)=port.strip_prefix("/dev/tty."){return format!("/dev/cu.{tail}");}
    #[cfg(windows)] {return port.trim_start_matches(r"\\.\").to_ascii_uppercase();}
    #[cfg(not(windows))] port.to_owned()
}
fn legacy_schema()->u32{1}
#[derive(Clone,Serialize,Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
struct Archived {
    original:Pending, observation:super::appearance_recovery::Observation,
    user_intent_id:String, resolution:String, old_result:String,
}
#[derive(Clone,Serialize,Deserialize)]
#[serde(deny_unknown_fields)]
struct Disk {
    #[serde(default="legacy_schema")] schema_version:u32,
    pending:Vec<Pending>,
    #[serde(default)] quarantines:Vec<RawQuarantine>,
    #[serde(default,skip_serializing_if="Vec::is_empty")] archives:Vec<Archived>,
}
impl Default for Disk{fn default()->Self{Self{schema_version:2,pending:vec![],quarantines:vec![],archives:vec![]}}}
struct State {disk:Disk,blocked:bool}
pub(super) struct Journal {path:Option<PathBuf>,_lock:Option<File>,state:Mutex<State>}
static JOURNAL:OnceLock<Arc<Journal>>=OnceLock::new();
fn valid_board(s:&str)->bool{!s.is_empty()&&s.len()<=128&&!s.chars().any(char::is_control)}
fn save(path:&Path,disk:&Disk)->Result<(),String>{
    let bytes=serde_json::to_vec(disk).map_err(|_|ERROR)?;
    if bytes.len() as u64>MAX_BYTES{return Err(ERROR.into());}
    let parent=path.parent().ok_or(ERROR)?;let mut staged=tempfile::NamedTempFile::new_in(parent).map_err(|_|ERROR)?;
    staged.write_all(&bytes).and_then(|_|staged.as_file().sync_all()).map_err(|_|ERROR)?;
    staged.persist(path).map_err(|_|ERROR)?.sync_all().map_err(|_|ERROR)?;
    #[cfg(unix)] File::open(parent).and_then(|f|f.sync_all()).map_err(|_|ERROR)?;
    Ok(())
}
impl Journal {
    pub(super) fn open(directory:&Path)->Result<Self,String>{
        fs::create_dir_all(directory).map_err(|_|ERROR)?;
        let mut options=OpenOptions::new();options.read(true).write(true).create(true).truncate(false);
        #[cfg(windows)] {use std::os::windows::fs::OpenOptionsExt;options.share_mode(0);}
        let mut lock=options.open(directory.join("appearance.lock")).map_err(|_|ERROR)?;
        #[cfg(unix)] {use std::os::fd::AsRawFd;
            // The lock remains owned for this Journal's entire lifetime.
            if unsafe{libc::flock(lock.as_raw_fd(),libc::LOCK_EX|libc::LOCK_NB)}!=0{return Err(ERROR.into());}
        }
        #[cfg(not(any(unix,windows)))] return Err(ERROR.into());
        let initialized=lock.metadata().map_err(|_|ERROR)?.len()!=0;
        let path=directory.join("pending-v1.json");
        let mut disk=match File::open(&path){
            Ok(file)=>{let mut bytes=Vec::new();file.take(MAX_BYTES+1).read_to_end(&mut bytes).map_err(|_|ERROR)?;
                if bytes.len() as u64>MAX_BYTES{return Err(ERROR.into());}serde_json::from_slice::<Disk>(&bytes).map_err(|_|ERROR)?},
            Err(e) if e.kind()==std::io::ErrorKind::NotFound&&!initialized=>{let d=Disk::default();save(&path,&d)?;d},
            Err(_)=>return Err(ERROR.into()),
        };
        let mut boards=HashSet::new();
        if !matches!(disk.schema_version,1|2|3)||disk.quarantines.len()>64||disk.quarantines.iter().any(|q|
            !valid_board(&q.board)||q.generation==0||q.port.is_empty()||q.port.len()>512||q.port.chars().any(char::is_control)
            ||q.identity.as_ref().is_some_and(|i|i.serial.len()>128||i.serial.chars().any(char::is_control))
            ||!disk.pending.iter().any(|p|p.attempt==q.attempt&&p.board==q.board&&p.generation==q.generation))
            ||disk.pending.len()>64||disk.pending.iter().any(|p|!valid_board(&p.board)||p.generation==0
            ||uuid::Uuid::parse_str(&p.attempt).is_err()||p.state!="submitted_unknown"
            ||!valid_transfer_id(&p.transfer_id)||p.phase.is_empty()||p.phase.len()>64||!p.phase.bytes().all(|b|b.is_ascii_alphanumeric()||b"/_-".contains(&b))||!boards.insert(p.board.clone())){return Err(ERROR.into());}
        let mut intents=HashSet::new();
        if disk.archives.len()>64||(!disk.archives.is_empty()&&disk.schema_version!=3)
            ||disk.archives.iter().any(|a|a.resolution!="observed-current-state/quiescent"||a.old_result!="unknown"
                ||a.original.state!="submitted_unknown"||!valid_board(&a.original.board)||a.original.generation==0
                ||uuid::Uuid::parse_str(&a.original.attempt).is_err()||!valid_transfer_id(&a.original.transfer_id)
                ||a.original.phase.is_empty()||a.original.phase.len()>64
                ||!a.original.phase.bytes().all(|b|b.is_ascii_alphanumeric()||b"/_-".contains(&b))
                ||!a.observation.valid()||a.observation.board!=a.original.board
                ||uuid::Uuid::parse_str(&a.user_intent_id).map_or(true,|u|u.get_version_num()!=4)
                ||!intents.insert(a.user_intent_id.clone())) {return Err(ERROR.into());}
        // Only an actually EMPTY legacy journal can migrate without guessing
        // whether its missing transport metadata hid an uncertain raw stream.
        if disk.schema_version==1&&disk.pending.is_empty()&&disk.quarantines.is_empty(){disk.schema_version=2;save(&path,&disk)?;}
        lock.write_all(b"1").and_then(|_|lock.sync_all()).map_err(|_|ERROR)?;
        Ok(Self{path:Some(path),_lock:Some(lock),state:Mutex::new(State{disk,blocked:false})})
    }
    #[cfg(test)] pub(super) fn memory()->Self{Self{path:None,_lock:None,state:Mutex::new(State{disk:Disk::default(),blocked:false})}}
    fn persist(&self,state:&mut State)->Result<(),String>{
        if let Some(path)=&self.path {if save(path,&state.disk).is_err(){state.blocked=true;return Err(ERROR.into());}}
        Ok(())
    }
    pub(super) fn needs_observation(&self,board:&str)->Result<bool,String>{
        let state=self.state.lock().map_err(|_|ERROR)?;
        if state.blocked{return Err(ERROR.into());}
        let pending=state.disk.pending.iter().any(|p|p.board==board);
        if state.disk.quarantines.iter().any(|q|q.board==board)
            ||(state.disk.schema_version==1&&pending){
            return Err("appearance_raw_stream_quarantined_no_probe".into());
        }
        Ok(pending)
    }
    pub(super) fn pending_for_recovery(&self,board:&str,intent:&str)->Result<Pending,String>{
        let state=self.state.lock().map_err(|_|ERROR)?;
        if state.blocked||state.disk.schema_version==1{return Err(ERROR.into());}
        if state.disk.quarantines.iter().any(|q|q.board==board){return Err("appearance_raw_stream_quarantined_no_probe".into());}
        if state.disk.archives.iter().any(|a|a.user_intent_id==intent){return Err("appearance_recovery_fresh_intent_required".into());}
        let pending=state.disk.pending.iter().find(|p|p.board==board).cloned().ok_or("appearance_recovery_no_pending")?;
        if pending.attempt==intent{return Err("appearance_recovery_fresh_intent_required".into());}
        Ok(pending)
    }
    pub(super) fn archive_observation(&self,pending:&Pending,observed:&super::appearance_recovery::Observation,intent:&str)->Result<(),String>{
        let mut state=self.state.lock().map_err(|_|ERROR)?;
        if state.blocked||state.disk.schema_version==1||!observed.valid()||observed.board!=pending.board
            ||state.disk.archives.len()>=64||state.disk.archives.iter().any(|a|a.user_intent_id==intent)
            ||state.disk.quarantines.iter().any(|q|q.board==pending.board)
            ||!state.disk.pending.iter().any(|p|p==pending){return Err(ERROR.into());}
        let mut next=state.disk.clone();
        next.archives.push(Archived{original:pending.clone(),observation:observed.clone(),user_intent_id:intent.into(),
            resolution:"observed-current-state/quiescent".into(),old_result:"unknown".into()});
        next.pending.retain(|p|p!=pending);
        next.schema_version=3;
        // Persist a single atomic replacement containing BOTH the immutable old
        // evidence and the observation before releasing its unresolved gate.
        if let Some(path)=&self.path{if save(path,&next).is_err(){state.blocked=true;return Err(ERROR.into());}}
        state.disk=next;Ok(())
    }
    fn available(&self,board:&str)->Result<(),String>{
        let state=self.state.lock().map_err(|_|ERROR)?;
        if state.blocked{return Err(ERROR.into());}
        if state.disk.pending.iter().any(|p|p.board==board){return Err(UNKNOWN.into());}
        Ok(())
    }
    fn begin(&self,attempt:&Attempt,phase:&str,transfer_id:&str)->Result<(),String>{
        let mut state=self.state.lock().map_err(|_|ERROR)?;
        if state.blocked{return Err(ERROR.into());}
        if let Some(old)=state.disk.pending.iter().find(|p|p.board==attempt.board){
            return if old.attempt==attempt.id&&old.transfer_id==transfer_id{Ok(())}else{Err(UNKNOWN.into())};
        }
        if state.disk.pending.len()>=64{return Err(ERROR.into());}
        state.disk.pending.push(Pending{board:attempt.board.clone(),generation:attempt.generation,attempt:attempt.id.clone(),phase:phase.into(),state:"submitted_unknown".into(),transfer_id:transfer_id.into()});
        self.persist(&mut state)
    }
    fn wire_available(&self,port:&str,board:Option<&str>)->Result<(),String>{
        let state=self.state.lock().map_err(|_|ERROR)?;
        if state.blocked{return Err(ERROR.into());}
        if (state.disk.schema_version==1&&!state.disk.pending.is_empty())||state.disk.quarantines.iter().any(|q|
            port_key(&q.port)==port_key(port)||board==Some(q.board.as_str())){
            return Err("appearance_raw_stream_quarantined_no_probe".into());
        }
        Ok(())
    }
    fn probe_candidate(&self,port:&str,device:Option<&UsbDeviceInfo>)->Result<(),String>{
        self.wire_available(port,None)?;
        let state=self.state.lock().map_err(|_|ERROR)?;
        for q in &state.disk.quarantines{
            match (&q.identity,device){
                (Some(old),Some(new)) if old.vid!=new.vid||old.pid!=new.pid
                    ||(!old.serial.is_empty()&&!new.serial_number.is_empty()&&old.serial!=new.serial_number)=>{},
                // Missing identity is NOT proof that a renamed node is safe.
                _=>return Err("appearance raw port identity unresolved; probe blocked".into()),
            }
        }
        Ok(())
    }
    fn raw_intent(&self,attempt:&Attempt,port:&str,identity:Option<UsbIdentity>)->Result<(),String>{
        if port.is_empty()||port.len()>512||port.chars().any(char::is_control){return Err(ERROR.into());}
        let mut state=self.state.lock().map_err(|_|ERROR)?;
        if state.blocked||!state.disk.pending.iter().any(|p|p.attempt==attempt.id){return Err(ERROR.into());}
        if state.disk.quarantines.iter().any(|q|q.attempt==attempt.id){return Ok(());}
        if state.disk.quarantines.len()>=64||state.disk.quarantines.iter().any(|q|
            q.board==attempt.board||port_key(&q.port)==port_key(port)||match (&q.identity,&identity){
                (Some(a),Some(b))=>a.vid==b.vid&&a.pid==b.pid&&(a.serial.is_empty()||b.serial.is_empty()||a.serial==b.serial),
                _=>false,
            }){return Err("appearance raw stream identity already quarantined".into());}
        state.disk.quarantines.push(RawQuarantine{board:attempt.board.clone(),generation:attempt.generation,attempt:attempt.id.clone(),port:port.into(),identity});
        self.persist(&mut state)
    }
    fn raw_confirmed(&self,attempt:&Attempt)->Result<(),String>{
        let mut state=self.state.lock().map_err(|_|ERROR)?;
        if state.blocked||!state.disk.quarantines.iter().any(|q|q.attempt==attempt.id){return Err(ERROR.into());}
        state.disk.quarantines.retain(|q|q.attempt!=attempt.id);self.persist(&mut state)
    }
    fn complete(&self,attempt:&Attempt)->Result<(),String>{
        let mut state=self.state.lock().map_err(|_|ERROR)?;
        if state.blocked||state.disk.quarantines.iter().any(|q|q.attempt==attempt.id){return Err(ERROR.into());}
        if !state.disk.pending.iter().any(|p|p.board==attempt.board&&p.attempt==attempt.id){return Err(ERROR.into());}
        state.disk.pending.retain(|p|p.attempt!=attempt.id);self.persist(&mut state)
    }
}
pub(super) fn journal_for_recovery()->Result<Arc<Journal>,String>{JOURNAL.get().cloned().ok_or_else(||ERROR.into())}
pub(super) fn configure(app_data:&Path)->Result<(),String>{
    if JOURNAL.get().is_some(){return Ok(());}
    let journal=Arc::new(Journal::open(&app_data.join("usb-appearance-safety"))?);
    JOURNAL.set(journal).map_err(|_|ERROR.to_owned())
}
pub(super) struct Attempt {pub(super) board:String,pub(super) generation:u64,id:String,journal:Arc<Journal>,cancel:Arc<AtomicBool>,submitted:AtomicBool,unknown:AtomicBool,activation:Mutex<Option<super::appearance_activation_evidence::Evidence>>}
impl Attempt {
    fn new(board:&str,generation:u64,journal:Arc<Journal>,cancel:Arc<AtomicBool>)->Result<Self,String>{
        if !valid_board(board)||generation==0{return Err("appearance_owner_unverified".into());}
        journal.available(board)?;
        Ok(Self{board:board.into(),generation,id:uuid::Uuid::new_v4().to_string(),journal,cancel,submitted:AtomicBool::new(false),unknown:AtomicBool::new(false),activation:Mutex::new(None)})
    }
    pub(super) fn check(&self,board:&str,generation:u64)->Result<(),String>{
        if self.cancel.load(Ordering::Acquire){return Err("appearance_connection_retired_no_replay".into());}
        if self.board!=board||self.generation!=generation{return Err("appearance_connection_changed_no_replay".into());}
        if self.unknown.load(Ordering::Acquire){return Err(UNKNOWN.into());}Ok(())
    }
    pub(super) fn before_mutation(&self,phase:&str,transfer_id:&str)->Result<(),String>{
        if self.unknown.load(Ordering::Acquire){return Err(UNKNOWN.into());}
        if !valid_transfer_id(transfer_id){return Err("invalid appearance transferId".into());}
        self.journal.begin(self,phase,transfer_id)?;self.submitted.store(true,Ordering::Release);Ok(())
    }
    pub(super) fn raw_intent(&self,port:&str,identity:Option<UsbIdentity>)->Result<(),String>{self.journal.raw_intent(self,port,identity)}
    pub(super) fn raw_confirmed(&self)->Result<(),String>{self.journal.raw_confirmed(self)}
    pub(super) fn uncertain(&self){if self.submitted.load(Ordering::Acquire){self.unknown.store(true,Ordering::Release);}}
}
// A negative header is not a parser-boundary proof unless the future firmware
// explicitly supplies this exact, current physical-owner receipt. Never infer
// idle from an error string, a timeout, zero bytes written, or a reopened port.
pub(super) fn raw_ready_rejected_idle(ack:&serde_json::Value,transfer_id:&str,path:&str,index:u32,board:&str,boot:&str)->bool {
    let index_text=index.to_string();
    let exact_index=ack["index"].as_u64()==Some(u64::from(index))
        ||ack["index"].as_str()==Some(index_text.as_str());
    !board.is_empty()&&boot.len()==16&&boot.bytes().all(|c|c.is_ascii_digit()||(b'a'..=b'f').contains(&c))
        &&ack["ok"].as_bool()==Some(false)
        &&ack["transferId"].as_str()==Some(transfer_id)&&ack["phase"].as_str()==Some("raw-ready")
        &&ack["path"].as_str()==Some(path)&&exact_index
        &&ack["rawReadyProof"].as_str()==Some("p4-raw-ready-proof-v1")
        &&ack["boardDeviceId"].as_str()==Some(board)&&ack["bootId"].as_str()==Some(boot)
        &&ack["ownerMatched"].as_bool()==Some(true)&&ack["phaseCurrent"].as_bool()==Some(true)
        &&ack["rawIdle"].as_bool()==Some(true)&&ack["rawRemaining"].as_u64()==Some(0)
        &&ack["headerAccepted"].as_bool()==Some(false)
        &&matches!(ack["errorCode"].as_str(),Some("invalid_raw_header"|"transfer_slot_mismatch"|"raw_buffer_unavailable"|"phase_admission_rejected"))
}
pub(super) fn raw_ready_error_code(ack:&serde_json::Value)->&'static str {
    match ack["errorCode"].as_str() {
        Some("invalid_raw_header")=>"invalid_raw_header",
        Some("transfer_slot_mismatch")=>"transfer_slot_mismatch",
        Some("raw_buffer_unavailable")=>"raw_buffer_unavailable",
        Some("phase_admission_rejected")=>"phase_admission_rejected",
        Some("raw_already_receiving")=>"raw_already_receiving",
        Some("appearance_owner_mismatch")=>"appearance_owner_mismatch",
        Some("raw_ready_ack_unconfirmed")=>"raw_ready_ack_unconfirmed",
        Some("phase_outcome_unknown")=>"phase_outcome_unknown",
        _=>match ack["error"].as_str() {
            Some("invalid raw asset chunk header")=>"invalid_raw_header",
            Some("slot prepare failed")=>"transfer_slot_mismatch",
            Some("no memory")=>"raw_buffer_unavailable",
            Some("raw chunk still active")=>"raw_already_receiving",
            Some("appearance phase outcome unknown; read-only reconciliation required; do not replay")=>"phase_outcome_unknown",
            _=>"unclassified_raw_ready_rejection",
        }
    }
}
pub(super) fn raw_ready_diagnostics(ack:&serde_json::Value)->serde_json::Value {
    let mut out=serde_json::json!({"errorCode":raw_ready_error_code(ack),"typedProof":ack["rawReadyProof"]=="p4-raw-ready-proof-v1"});
    for field in ["ok","ownerMatched","phaseCurrent","rawIdle","headerAccepted"] {
        if let Some(value)=ack[field].as_bool(){out[field]=value.into();}
    }
    if let Some(value)=ack["rawRemaining"].as_u64().filter(|n|*n<=65536){out["rawRemaining"]=value.into();}
    out
}
pub(super) fn valid_transfer_id(id:&str)->bool{!id.is_empty()&&id.len()<=63&&id.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))}
pub(super) fn command_id<'a>(topic:&str,payload:&'a serde_json::Value)->Result<&'a str,String>{
    if !topic.starts_with("asset/"){return Err("invalid appearance topic".into());}
    payload["transferId"].as_str().filter(|id|valid_transfer_id(id)).ok_or_else(||"invalid appearance transferId".into())
}
pub(super) fn mutation(topic:&str)->bool{topic.starts_with("asset/")&&!matches!(topic,"asset/stat"|"asset/slot-query")}
impl UsbSerialManager {
    pub(super) fn activation_start(&self) {
        if let Ok(Some(a))=self.active_appearance_attempt(){if let Ok(mut trace)=a.activation.lock(){*trace=Some(Default::default());}}
    }
    pub(super) fn activation_update(&self,update:impl FnOnce(&mut super::appearance_activation_evidence::Evidence)) {
        if let Ok(Some(a))=self.active_appearance_attempt(){if let Ok(mut trace)=a.activation.lock(){if let Some(t)=trace.as_mut(){update(t);}}}
    }
    pub(super) fn ensure_wire_available(&self,port:&str,board:Option<&str>)->Result<(),String>{
        #[cfg(test)] let journal=&self.appearance_journal;
        #[cfg(not(test))] let journal=JOURNAL.get().ok_or(ERROR)?;
        journal.wire_available(port,board)
    }
    pub(super) fn ensure_probe_candidate(&self,port:&str,device:Option<&UsbDeviceInfo>)->Result<(),String>{
        #[cfg(test)] let journal=&self.appearance_journal;
        #[cfg(not(test))] let journal=JOURNAL.get().ok_or(ERROR)?;
        journal.probe_candidate(port,device)
    }
    pub(super) fn raw_usb_identity(&self,port:&str)->Option<UsbIdentity>{
        let cache=self.last_serial_baud.lock().ok()?;let (key,_)=cache.as_ref()?;
        let prefix=format!("{port}|");let mut fields=key.strip_prefix(&prefix)?.splitn(3,'|');
        let vid=u16::from_str_radix(fields.next()?,16).ok()?;let pid=u16::from_str_radix(fields.next()?,16).ok()?;
        let serial=fields.next()?.to_owned();if serial.len()>128||serial.chars().any(char::is_control){return None;}
        Some(UsbIdentity{vid,pid,serial})
    }
    pub(super) fn retire_uncertain_raw(&self,attempt:&Attempt){
        attempt.uncertain();
        // Caller still owns the physical wire. These exact old tokens remain
        // safe to retire even if the connection mutex became unavailable.
        attempt.cancel.store(true,Ordering::Release);self.inbound_owner.retire(attempt.generation);
        // Never retire a newer connection object.
        let retired=if let Ok((_lease,mut slot))=self.inbound_owner.connection(&self.connection){
            self.inbound_owner.retire(attempt.generation);
            if let Some(c)=slot.as_mut().filter(|c|c.connection_id==attempt.generation&&c.board_device_id==attempt.board){
                c.cancel_reader.store(true,Ordering::Release);c.connected=false;cache_usb_status(&self.status_snapshot,Some(c));
                slot.take()
            }else{None}
        }else{None};
        // Release this exact retired writer outside dispatch/connection mutexes.
        // Durable raw quarantine and logical pending evidence remain unchanged.
        drop(retired);
    }
    pub(super) fn active_appearance_attempt(&self)->Result<Option<Arc<Attempt>>,String>{
        self.appearance_attempt.lock().map(|a|a.clone()).map_err(|_|"appearance owner unavailable".into())
    }
    pub(super) fn check_appearance_locked(&self,attempt:&Attempt,connection:&UsbConnection)->Result<(),String>{
        if !connection.connected||connection.cancel_reader.load(Ordering::Acquire)
            ||!self.inbound_owner.is_current(connection.connection_id){return Err("appearance_connection_retired_no_replay".into());}
        attempt.check(&connection.board_device_id,connection.connection_id)
    }
    pub(super) fn appearance_admission<'a>(&'a self,attempt:&Attempt)
        ->Result<(std::sync::MutexGuard<'a,()>,std::sync::MutexGuard<'a,Option<UsbConnection>>),String>{
        let (lease,connection)=self.inbound_owner.connection(&self.connection)?;
        self.check_appearance_locked(attempt,connection.as_ref().ok_or("appearance connection missing")?)?;
        Ok((lease,connection))
    }
    pub(super) fn check_appearance_connection(&self)->Result<(),String>{
        let attempt=self.active_appearance_attempt()?.ok_or("appearance owner missing")?;
        let (_lease,connection)=self.appearance_admission(&attempt)?;
        let c=connection.as_ref().ok_or("appearance connection missing")?;
        self.ensure_wire_available(&c.port_name,Some(&c.board_device_id))
    }
    pub(super) fn appearance_submitted(&self)->bool{self.active_appearance_attempt().ok().flatten().is_some_and(|a|a.submitted.load(Ordering::Acquire))}
    pub(super) fn appearance_uncertain(&self){if let Ok(Some(a))=self.active_appearance_attempt(){a.uncertain();}}
    pub(super) fn ensure_unowned_asset_write(&self,board:&str)->Result<(),String>{
        #[cfg(test)] let journal=&self.appearance_journal;
        #[cfg(not(test))] let journal=JOURNAL.get().ok_or(ERROR)?;
        journal.available(board)?;
        // Generic sends cannot bypass durable-before-submission ownership.
        Err("appearance transaction owner required".into())
    }
    pub(super) fn native_appearance_or_fallback<T>(&self,work:impl FnOnce()->Result<T,String>)->Result<Option<T>,String>{
        match work(){
            Ok(value)=>Ok(Some(value)),
            Err(error) if error==APPEARANCE_SYNC_CANCELLED_ERROR||self.appearance_submitted()=>{
                self.appearance_uncertain();Err(error)
            }
            Err(_)=>Ok(None), // only a native failure BEFORE the first mutation
        }
    }
    pub(super) fn with_appearance_attempt<T>(&self,board:&str,work:impl FnOnce()->Result<T,String>)->Result<T,String>{
        self.with_appearance_attempt_bound(board,None,work)
    }
    pub(super) fn with_appearance_attempt_bound<T>(&self,board:&str,expected:Option<(u64,&str)>,work:impl FnOnce()->Result<T,String>)->Result<T,String>{
        let (generation,cancel)={
            let (_lease,connection)=self.inbound_owner.connection(&self.connection)?;
            let c=connection.as_ref().ok_or("appearance connection missing")?;
            if !c.connected||c.board_device_id!=board||c.cancel_reader.load(Ordering::Acquire)
                ||!self.inbound_owner.is_current(c.connection_id){return Err("appearance_connection_retired_no_replay".into());}
            if let Some((generation,boot))=expected{
                if c.connection_id!=generation||c.capabilities["debugBootId"].as_str()!=Some(boot){
                    return Err("appearance_recovery_connection_changed".into());
                }
            }
            (c.connection_id,c.cancel_reader.clone())
        };
        #[cfg(test)] let journal=self.appearance_journal.clone();
        #[cfg(not(test))] let journal=JOURNAL.get().cloned().ok_or(ERROR)?;
        let attempt=Arc::new(Attempt::new(board,generation,journal,cancel)?);
        {let mut slot=self.appearance_attempt.lock().map_err(|_|ERROR)?;
            if slot.is_some(){return Err("appearance attempt already active".into());}*slot=Some(attempt.clone());}
        struct Clear<'a>(&'a UsbSerialManager);
        impl Drop for Clear<'_>{fn drop(&mut self){if let Ok(mut slot)=self.0.appearance_attempt.lock(){*slot=None;}}}
        let _clear=Clear(self);
        let result=work();
        match result {
            Ok(value)=>{
                // Retirement cannot linearize between this final admission and
                // durable intent completion. Never clear unknown on a stale object.
                let _admission=self.appearance_admission(&attempt)?;
                if attempt.submitted.load(Ordering::Acquire){attempt.journal.complete(&attempt)?;}
                Ok(value)
            }
            Err(error)=>{
                attempt.uncertain();
                transfer_log::record("appearance",if attempt.submitted.load(Ordering::Acquire){"result_unknown_no_replay"}else{"not_submitted"},
                    serde_json::json!({"boardDeviceId":attempt.board,"generation":attempt.generation,"attemptId":attempt.id,
                        "intentPersisted":attempt.submitted.load(Ordering::Acquire),"automaticCleanup":false,"automaticRetry":false}));
                if let Ok(trace)=attempt.activation.lock(){if let Some(t)=*trace {
                    transfer_log::record("appearance","activation_failure",serde_json::json!({
                        "schema":"appearance-activation-evidence-v1","stage":t.stage.label(),"errorKind":t.error_kind.label(),
                        "writeStarted":t.write_started,"writeCompleted":t.write_completed,"ack":t.ack.label(),
                        "intentPersisted":attempt.submitted.load(Ordering::Acquire),"automaticRetry":false,"automaticCleanup":false}));
                }}
                let outcome=if attempt.submitted.load(Ordering::Acquire){"APPEARANCE_RESULT_UNKNOWN:"}else{"APPEARANCE_NOT_SUBMITTED:"};
                Err(format!("{outcome} {error}"))
            }
        }
    }
}
