//! Explicit parent-only actual production full-upload entry. No production API changes.
use super::*;
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{fs::{self, File, OpenOptions}, io::{self, Read, Write}, path::{Path, PathBuf}};

const PHASE: &str = "full-upload-commit";
const MAX_TX: usize = 4 * 1024 * 1024;
const MAX_RX: usize = 2048;
#[derive(Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
struct RefFile { path:PathBuf, sha256:String }
#[derive(Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
struct PackFile { source_path:PathBuf, device_path:String, bytes:u64, sha256:String, checksum:String }
#[derive(Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
struct RetainedFile { path:PathBuf, bytes:u64, sha256:String }
#[derive(Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
struct Plan {
    schema_version:u32, execute:bool, source_digest:String, reader_sha256:String,
    board:String, boot:String, elf_sha256:String, elf_path:PathBuf, image_sha256:String, version:String, port:String,
    run_directory:PathBuf, output_directory:PathBuf, app_data_directory:PathBuf,
    pack_directory:PathBuf, pack_id:String, files:Vec<PackFile>, expected_bytes:u64, expected_raw_bytes:u64,
    factory_pack_id:String, custom_pack_id:Option<String>, overwrite_proof:Option<RefFile>,
    once_receipt:Option<RefFile>, lock_path:PathBuf,
}
fn need(value:bool,reason:&str)->Result<(),String>{if value{Ok(())}else{Err(reason.into())}}
fn hex(value:&str,n:usize)->bool{value.len()==n&&value.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b))}
fn number(v:&Value,p:&str)->Result<u64,String>{v.pointer(p).and_then(Value::as_u64).ok_or_else(||format!("missing numeric evidence: {p}"))}
fn digest(path:&Path)->Result<String,String>{
    let mut file=File::open(path).map_err(|_|"artifact unavailable")?;let mut h=Sha256::new();let mut buf=[0u8;65536];
    loop{let n=file.read(&mut buf).map_err(|_|"artifact read failed")?;if n==0{break;}h.update(&buf[..n]);}Ok(format!("{:x}",h.finalize()))
}
fn regular(path:&Path)->Result<(),String>{
    need(path.is_absolute()&&fs::symlink_metadata(path).is_ok_and(|m|m.is_file()&&!m.file_type().is_symlink())
        &&path.canonicalize().is_ok_and(|p|p==path),"absolute non-symlink regular path required")
}
fn reference(r:&RefFile)->Result<Value,String>{regular(&r.path)?;need(digest(&r.path)?==r.sha256,"receipt SHA changed")?;
    let bytes=fs::read(&r.path).map_err(|_|"receipt unavailable")?;need(bytes.len()<=262144,"receipt bound")?;
    serde_json::from_slice(&bytes).map_err(|_|"receipt JSON invalid".into())}
fn save(path:&Path,value:&Value)->Result<(),String>{
    let mut o=OpenOptions::new();o.write(true).create_new(true);
    #[cfg(unix)]{use std::os::unix::fs::OpenOptionsExt;o.mode(0o600);}
    let mut f=o.open(path).map_err(|_|"new evidence path required")?;
    f.write_all(&serde_json::to_vec_pretty(value).map_err(|_|"evidence encode")?).and_then(|_|f.sync_all()).map_err(|_|"evidence write".into())
}
fn mkdir(path:&Path)->Result<(),String>{fs::create_dir(path).map_err(|_|"new private directory required")?;
    #[cfg(unix)]{use std::os::unix::fs::PermissionsExt;fs::set_permissions(path,fs::Permissions::from_mode(0o700)).map_err(|_|"private directory mode")?;}Ok(())}
fn native_absent()->Result<(),String>{
    // Production bool treats enumeration errors as false. This adapter must
    // distinguish inability to inspect from a positive absence observation.
    let devices=rusb::devices().map_err(|_|"native USB enumeration unavailable; refuse fallback")?;
    for device in devices.iter(){
        let descriptor=device.device_descriptor().map_err(|_|"USB descriptor unavailable; native absence unknown")?;
        need(descriptor.vendor_id()!=P4_NATIVE_USB_VID||descriptor.product_id()!=P4_NATIVE_USB_PID,
            "native USB available: refuse second route")?;
    }
    need(!p4_native_usb_available(),"native USB appeared during inspection")
}
fn acceptance_path(value:Option<std::ffi::OsString>)->Result<PathBuf,String> {
    let path=PathBuf::from(value.ok_or("explicit acceptance path required; no personal-path default")?);
    need(path.is_absolute()&&path.parent().is_some()&&path.to_str().is_some_and(|v|!v.chars().any(char::is_control))
        &&!path.components().any(|c|matches!(c,std::path::Component::ParentDir)),
        "explicit absolute acceptance path required")?;
    Ok(path)
}
fn retained_root_under(root:&Path,private_root:&Path)->bool {
    root.is_absolute()&&private_root.is_absolute()&&root!=private_root&&root.starts_with(private_root)
        &&![root,private_root].iter().any(|p|p.components().any(|c|matches!(c,std::path::Component::ParentDir)))
}
fn private_acceptance_root()->Result<PathBuf,String> {
    let root=acceptance_path(std::env::var_os("P4_APPEARANCE_PRIVATE_ROOT"))?;
    need(root.canonicalize().is_ok_and(|p|p==root)
        &&fs::symlink_metadata(&root).is_ok_and(|m|m.is_dir()&&!m.file_type().is_symlink()),
        "canonical parent-owned acceptance directory required")?;
    Ok(root)
}
fn retained_media_path(path:&str)->bool {
    if path.starts_with('/')||path.contains('\\')||path.split('/').any(|p|p.is_empty()||p=="."||p=="..") {return false;}
    path=="p4/manifest.json" || (path.starts_with("p4/")
        && matches!(Path::new(path).extension().and_then(|s|s.to_str()),Some("h264"|"mjpg"|"pcm"|"wav")))
}
fn retained_permissions(mode:u32,directory:bool)->bool {
    (mode&0o7777)==if directory{0o700}else{0o600}
}
fn retained_authorized(v:&Value,board:&str,boot:&str,id:&str)->bool {
    let expected=&v["expectedSlotSnapshot"];
    v["boardDeviceId"]==board&&v["bootId"]==boot&&v["slot"]==1&&v["packId"]==id
        &&v["restorePackId"]==id&&v["userAuthorizedOverwrite"]==true&&v["knownRetainedLocalSource"]==true
        &&expected["slot"]==1&&expected["valid"]==true&&expected["packId"]==id
}
fn private_media_inventory(root:&Path)->Result<Vec<PathBuf>,String>{
    let private_root=private_acceptance_root()?;
    need(retained_root_under(root,&private_root)&&root.canonicalize().is_ok_and(|v|v==root),
        "retained media must be a canonical private directory under the explicit parent root")?;
    let mut pending=vec![root.to_path_buf()];let mut files=vec![];let mut count=0usize;
    while let Some(path)=pending.pop(){
        count+=1;need(count<=4096,"retained media inventory bound")?;
        let m=fs::symlink_metadata(&path).map_err(|_|"retained media metadata unavailable")?;
        need(!m.file_type().is_symlink()&&(m.is_dir()||m.is_file()),"retained media symlink/special file forbidden")?;
        #[cfg(unix)]{
            use std::os::unix::fs::MetadataExt;
            need(m.uid()==unsafe{libc::geteuid()}&&retained_permissions(m.mode(),m.is_dir()),"retained directories0700/files0600 and current owner required")?;
        }
        #[cfg(not(unix))]{return Err("reviewed private media permission check requires Unix parent".into());}
        if m.is_dir(){for item in fs::read_dir(&path).map_err(|_|"retained media inventory unavailable")?{
            pending.push(item.map_err(|_|"retained media entry unavailable")?.path());
        }}else{
            regular(&path)?;
            let relative=path.strip_prefix(root).map_err(|_|"retained file escaped root")?.to_string_lossy();
            let device=if relative.starts_with("p4/"){relative.to_string()}
                else{relative.split_once("/p4/").map(|(_,tail)|format!("p4/{tail}")).ok_or("retained file outside media tree")?};
            need(retained_media_path(&device),"reject provider/profile/extra files before any production inspector read")?;
            files.push(path);
        }
    }
    files.sort();Ok(files)
}
fn retained_restore(p:&Plan,id:&str)->Result<Value,String>{
    let v=reference(p.overwrite_proof.as_ref().ok_or("retained local media overwrite receipt required")?)?;
    need(retained_authorized(&v,&p.board,&p.boot,id),"custom slot needs exact retained-local identity/snapshot and explicit overwrite authorization")?;
    let root=PathBuf::from(v["restoreAppearanceDirectory"].as_str().ok_or("retained media directory required")?);
    let files=private_media_inventory(&root)?;
    let inspected=inspect_prepared_p4_appearance(&root)?;let assets=load_prepared_p4_appearance_pack(&root)?;
    need(inspected.pack_id==id,"actual retained restore packId differs from old custom1")?;
    let refs:Vec<RetainedFile>=serde_json::from_value(v["retainedFiles"].clone()).map_err(|_|"retained file SHA inventory required")?;
    let mut asset_paths=assets.iter().map(|a|a.source_path.clone()).collect::<Vec<_>>();asset_paths.sort();
    need(files==asset_paths&&refs.len()==assets.len(),"only the production manifest and declared media may be retained; no profile/provider/extra files")?;
    let mut seen=std::collections::HashSet::new();
    for a in &assets{
        need(retained_media_path(&a.device_path),"non-whitelisted retained media path")?;
        let r=refs.iter().find(|r|r.path==a.source_path).ok_or("retained asset SHA record missing")?;
        need(seen.insert(r.path.clone())&&hex(&r.sha256,64)
            &&fs::metadata(&r.path).map_err(|_|"retained file stat unavailable")?.len()==r.bytes
            &&digest(&r.path)?==r.sha256,"retained asset bytes/SHA differs")?;
    }
    Ok(v)
}
fn test_device_no_backup(v:&Value,board:&str,boot:&str)->bool{
    v["explicitTestDeviceNoBackupAuthorized"]==true&&v["boardDeviceId"]==board&&v["bootId"]==boot
        &&v["slot"]==1&&v["expectedSlotSnapshot"]["slot"]==1&&v["expectedSlotSnapshot"]["valid"]==true
}
fn overwrite_authorization(p:&Plan)->Result<Option<Value>,String>{
    let Some(r)=&p.overwrite_proof else{
        need(p.custom_pack_id.is_none(),"valid custom pack needs explicit overwrite proof")?;
        return Ok(None);
    };
    let v=reference(r)?;
    if test_device_no_backup(&v,&p.board,&p.boot){
        if let Some(id)=&p.custom_pack_id{need(v["expectedSlotSnapshot"]["packId"]==*id,"no-backup snapshot pack differs")?;}
        return Ok(Some(v));
    }
    let id=p.custom_pack_id.as_deref().ok_or("retained-source branch needs old packId")?;
    Ok(Some(retained_restore(p,id)?))
}
fn validate_plan(p:&Plan)->Result<(),String>{
    need(p.schema_version==1&&hex(&p.source_digest,64)&&hex(&p.reader_sha256,64)&&hex(&p.elf_sha256,64)
        &&hex(&p.image_sha256,64)&&hex(&p.pack_id,64)&&hex(&p.factory_pack_id,64),"all final pins required")?;
    need(p.board.starts_with("p4-")&&hex(&p.board[3..],12)&&hex(&p.boot,16)&&!p.version.is_empty()
        &&p.version.len()<96&&p.port.starts_with("/dev/cu.")&&!p.port.chars().any(char::is_control),"explicit board/boot/version/port required; no defaults")?;
    need(p.run_directory.is_absolute()&&p.run_directory.canonicalize().is_ok_and(|x|x==p.run_directory)
        &&p.output_directory==p.run_directory.join("native")&&p.app_data_directory==p.output_directory.join("app-data"),"new private output/appdata must be under exact run")?;
    need(!p.output_directory.exists()&&!p.app_data_directory.exists(),"output/appdata already exists")?;
    regular(&p.elf_path)?;need(digest(&p.elf_path)?==p.elf_sha256,"target ELF SHA differs")?;
    need(digest(&std::env::current_exe().map_err(|_|"actual reader path")?)?==p.reader_sha256,"actual final reader SHA differs")?;
    need(std::env::var("P4_APPEARANCE_SOURCE_DIGEST").as_deref()==Ok(p.source_digest.as_str())
        &&std::env::var("P4_SERIAL_PORT").as_deref()==Ok(p.port.as_str())
        &&std::env::var("P4_FORCE_FULL_SYNC").as_deref()==Ok("1"),"parent final source/port/full-sync pins required")?;
    need(std::env::var("P4_SERIAL_LOG_RAW").as_deref()!=Ok("1"),"unbounded raw stderr forbidden")?;
    let inspected=inspect_prepared_p4_appearance(&p.pack_directory)?;
    let assets=load_prepared_p4_appearance_pack(&p.pack_directory)?;
    need(p.files.len()==2&&assets.len()==2&&inspected.file_count==2&&inspected.byte_count==p.expected_bytes
        &&inspected.pack_id==p.pack_id&&p4_pack_id_from_assets(&assets)?==p.pack_id,"production pack inspector/file count/identity differs")?;
    let mut total=0;let mut raw=0;
    for a in &assets{
        let f=p.files.iter().find(|f|f.device_path==a.device_path).ok_or("undeclared production file")?;
        regular(&f.source_path)?;need(a.source_path==f.source_path&&fs::metadata(&f.source_path).map_err(|_|"pack file metadata")?.len()==f.bytes
            &&digest(&f.source_path)?==f.sha256&&asset_checksum_hex(&fs::read(&f.source_path).map_err(|_|"pack file read")?)==f.checksum,"pack file bytes/hash/FNV differ")?;
        total+=f.bytes;if f.device_path.ends_with(".h264"){raw+=f.bytes;}
    }
    need(total==p.expected_bytes&&raw==p.expected_raw_bytes&&raw>0,"pack total/raw bytes differ")?;
    if let Some(id)=&p.custom_pack_id{need(hex(id,64),"custom pack pin invalid")?;}
    overwrite_authorization(p)?;
    Ok(())
}
#[cfg(unix)]
fn lock_current(p:&Plan)->Result<(),String>{
    use std::os::unix::{fs::MetadataExt,io::AsRawFd};
    let shared_lock=acceptance_path(std::env::var_os("P4_APPEARANCE_SHARED_LOCK_PATH"))?;
    regular(&shared_lock)?;
    need(p.lock_path==shared_lock,"explicit original shared flock path required")?;
    let fd=std::env::var("P4_APPEARANCE_LOCK_FD").map_err(|_|"inherited lock fd required")?.parse::<i32>().map_err(|_|"invalid lock fd")?;
    need(fd>=3,"invalid lock fd")?;let m=fs::symlink_metadata(&p.lock_path).map_err(|_|"existing shared lock missing")?;
    need(m.is_file()&&!m.file_type().is_symlink(),"regular shared lock required")?;
    let mut st=std::mem::MaybeUninit::<libc::stat>::uninit();
    // Validate inherited parent-owned file description, never unlink a lock.
    need(unsafe{libc::fstat(fd,st.as_mut_ptr())}==0,"inherited lock fd unavailable")?;
    let st=unsafe{st.assume_init()};need(m.dev()==st.st_dev as u64&&m.ino()==st.st_ino as u64,"shared lock inode changed")?;
    let probe=OpenOptions::new().read(true).write(true).open(&p.lock_path).map_err(|_|"lock probe unavailable")?;
    let free=unsafe{libc::flock(probe.as_raw_fd(),libc::LOCK_EX|libc::LOCK_NB)}==0;
    if free{unsafe{libc::flock(probe.as_raw_fd(),libc::LOCK_UN)};return Err("parent exclusive lock not held".into());}
    need(unsafe{libc::flock(fd,libc::LOCK_EX|libc::LOCK_NB)}==0,"inherited fd is not exclusive lock owner")
}
#[cfg(not(unix))]
fn lock_current(_: &Plan)->Result<(),String>{Err("reviewed inherited flock adapter requires Unix parent".into())}

#[derive(Clone,Default)]
struct Parser { line:Vec<u8>, remaining:usize, raw:Vec<u8>, expected:String }
impl Parser{
    fn feed(&mut self,bytes:&[u8])->io::Result<Vec<Value>>{
        let mut frames=vec![];
        for &b in bytes{
            if self.remaining>0{
                self.raw.push(b);self.remaining-=1;
                if self.remaining==0{
                    if asset_checksum_hex(&self.raw)!=self.expected{return Err(io::Error::other("TX raw checksum differs"));}
                    self.raw.clear();
                }
                continue;
            }
            self.line.push(b);if self.line.len()>32768{return Err(io::Error::other("TX header bound"));}
            if b!=b'\n'{continue;}
            let v:Value=serde_json::from_slice(&self.line).map_err(|_|io::Error::other("TX header JSON invalid"))?;self.line.clear();
            let topic=v["topic"].as_str().unwrap_or("");
            if !matches!(topic,"debug/identity"|"diagnostics/query"|"audio/query"|"firmware/query"|"miniapp/query"|"widget/program-query"|"asset/slot-query"|"asset/begin"|"asset/raw-chunk"|"asset/file"|"asset/commit"){
                return Err(io::Error::other("unreviewed TX topic: no activate/abort/reboot/reset permitted"));
            }
            if topic=="asset/raw-chunk"{
                self.remaining=v["payload"]["size"].as_u64().filter(|n|*n>0&&*n<=262144).ok_or_else(||io::Error::other("raw size bound"))? as usize;
                self.expected=v["payload"]["checksum"].as_str().unwrap_or("").to_owned();
            }
            frames.push(v);
        }Ok(frames)
    }
}
#[derive(Default)]
struct Trace { parser:Parser,tx:Vec<u8>,frames:Vec<Value>,rx:Vec<Value>,stages:Vec<Value>,progress:Vec<Value> }
struct Writer { inner:Box<dyn Write+Send>,trace:Arc<Mutex<Trace>>,paths:Vec<String>,bad:Arc<AtomicBool> }
impl Write for Writer{
    fn write(&mut self,bytes:&[u8])->io::Result<usize>{
        // Check before forwarding a command terminator; record only bytes the
        // real writer reports as submitted. No filesystem IO in raw-ready span.
        {let t=self.trace.lock().unwrap();
            if self.bad.load(Ordering::Acquire)&&t.parser.remaining==0{return Err(io::Error::other("device activity changed; no new header/mutation permitted"));}
            if t.tx.len().saturating_add(bytes.len())>MAX_TX{return Err(io::Error::other("TX capture bound"));}
            if t.parser.remaining==0 || bytes.len()>t.parser.remaining {
                let mut preview=t.parser.clone();
                let mut seen=t.frames.clone();
                for v in preview.feed(bytes)? {
                    let topic=v["topic"].as_str().unwrap_or("");
                    if matches!(topic,"asset/raw-chunk"|"asset/file")
                        && !self.paths.iter().any(|p|v["payload"]["path"]==*p) {
                        return Err(io::Error::other("TX file not in frozen two-file pack"));
                    }
                    if matches!(topic,"asset/begin"|"asset/commit")&&seen.iter().any(|f|f["topic"]==topic){
                        return Err(io::Error::other("second BEGIN/COMMIT forbidden before forwarding"));
                    }
                    if topic=="asset/file"&&seen.iter().any(|f|f["topic"]==topic&&f["payload"]["path"]==v["payload"]["path"]){
                        return Err(io::Error::other("second FILE commit forbidden before forwarding"));
                    }
                    if topic=="asset/raw-chunk"{
                        let count=seen.iter().filter(|f|f["topic"]==topic&&f["payload"]["path"]==v["payload"]["path"]).count();
                        if v["payload"]["index"].as_str()!=Some(count.to_string().as_str()){
                            return Err(io::Error::other("raw index replay/gap forbidden before forwarding"));
                        }
                    }
                    seen.push(v);
                }
            }}
        let n=self.inner.write(bytes)?;
        let mut t=self.trace.lock().unwrap();t.tx.extend_from_slice(&bytes[..n]);let frames=t.parser.feed(&bytes[..n])?;t.frames.extend(frames);Ok(n)
    }
    fn flush(&mut self)->io::Result<()>{self.inner.flush()}
}
struct Session<'a>{usb:UsbSerialManager,p:&'a Plan,trace:Arc<Mutex<Trace>>,bad:Arc<AtomicBool>,started:Instant,generation:u64,reader_lifetime:std::sync::Weak<()>}
impl Session<'_>{
    fn scope(&self)->Result<(),String>{
        need(!self.bad.load(Ordering::Acquire),"unexpected input/boot/audio/overflow; stop no replay")?;
        let s=self.usb.status();need(s.connected&&s.board_device_id==self.p.board&&s.boot_id==self.p.boot
            &&s.firmware==self.p.version&&s.connection_id==self.generation.to_string(),"same manager/board/boot/version/generation required")
    }
    fn query(&self,topic:&str,reply:&str,mut payload:Value)->Result<Value,String>{
        self.scope()?;let id=format!("full-{}",uuid::Uuid::new_v4());
        let key=if topic=="asset/slot-query"{"transferId"}else{"requestId"};payload[key]=id.clone().into();
        if topic=="asset/slot-query"{payload["queryNonce"]=id.clone().into();}
        let cursor=self.trace.lock().unwrap().rx.len();
        self.usb.send_inner(topic,&payload,true,Some(&self.p.board),Some(self.generation),None)?;
        let until=Instant::now()+Duration::from_secs(15);
        loop{
            self.scope()?;
            let found={let t=self.trace.lock().unwrap();t.rx[cursor..].iter().find(|v|v["topic"]==reply
                && (v["payload"][key]==id||(reply=="audio/status"&&v["payload"].get(key).is_none()))).cloned()};
            if let Some(v)=found{
                if topic=="miniapp/query"{
                    let ack={let t=self.trace.lock().unwrap();t.rx[cursor..].iter().find(|v|v["topic"]=="protocol/ack"&&v["payload"]["requestId"]==id).cloned()};
                    if let Some(ack)=ack{need(ack["payload"]["requestTopic"]==topic&&ack["payload"]["ok"]==true,"live query ACK unknown/rejected")?;return Ok(v["payload"].clone());}
                }else{return Ok(v["payload"].clone());}
            }
            need(Instant::now()<until,"read-only response unknown; never replay query or mutation")?;std::thread::sleep(Duration::from_millis(5));
        }
    }
    fn snapshot(&self)->Result<Value,String>{
        let identity=self.query("debug/identity","debug/identity",json!({}))?;
        need(identity["engine"]=="p4-evidence-identity-v1"&&identity["boardDeviceId"]==self.p.board
            &&identity["bootId"]==self.p.boot&&identity["appElfSha256"]==self.p.elf_sha256,"fresh exact identity/ELF required")?;
        let d=self.query("diagnostics/query","diagnostics/status",json!({}))?;gate(&d,self.p)?;
        let audio=self.query("audio/query","audio/status",json!({}))?;
        need(["active","conversation","playbackActive","streamActive"].iter().all(|k|audio[*k]==false),"audio must be explicitly idle")?;
        let fw=self.query("firmware/query","firmware/status",json!({}))?;need(fw["active"]==false&&fw["pendingVerify"]==false,"OTA must be idle/valid")?;
        let programs=self.query("widget/program-query","widget/program-status",json!({}))?;
        need(programs["ok"]==true&&programs["boardDeviceId"]==self.p.board&&programs["bootId"]==self.p.boot,"PROGRAM query identity")?;
        let items=programs["items"].as_array().ok_or("PROGRAM items absent")?;
        for item in items{need(number(item,"/maxStepUs")?<=750,"PROGRAM750us violation")?;}
        need(items.is_empty(),"full upload requires no live PROGRAM; no implicit stop")?;
        let live=self.query("miniapp/query","miniapp/state",json!({}))?;
        need(live["bootId"]==self.p.boot&&live["observationAvailable"]==true,"live state unavailable/wrong boot")?;
        let slots=self.query("asset/slot-query","asset/ack",json!({"observationSchema":appearance_recovery::SCHEMA,
            "expectedBoardDeviceId":self.p.board,"expectedBootId":self.p.boot}))?;
        need(slots["ok"]==true&&slots["phase"]=="slot-query"&&slots["queryNonce"]==slots["transferId"]
            &&slots["boardDeviceId"]==self.p.board&&slots["bootId"]==self.p.boot&&slots["observationSchema"]==appearance_recovery::SCHEMA
            &&slots["serialized"]==true&&slots["quiescent"]==true,"fresh same-manager quiescent slots required")?;
        for k in ["snapshotChanged","assetTransferActive","rawReceiving","appearanceOwnerActive","assetFileOpen","assetSlotPending",
            "nativeTransferActive","nativeReceiving","nativeWorkerPending","protocolWorkerPending","storageWorkerPending","firmwareOtaActive","rebootPending"]{
            need(slots[k]==false,"appearance/OTA/worker not quiescent")?;
        }
        self.scope()?;Ok(json!({"identity":identity,"diagnostics":d,"audio":audio,"firmware":fw,"programs":programs,"live":live,"slots":slots}))
    }
}
fn slot(snapshot:&Value,id:u64)->Result<Value,String>{
    let a=snapshot["slots"]["slots"].as_array().ok_or("slot array missing")?;
    need(a.len()==2&&a.iter().filter(|v|v["slot"]==id).count()==1,"exact two unique slots required")?;
    a.iter().find(|v|v["slot"]==id).cloned().ok_or("slot absent".into())
}
fn gate(d:&Value,p:&Plan)->Result<(),String>{
    need(d["ok"]==true&&d["boardDeviceId"]==p.board&&number(d,"/faultResetCount")?==7,"fault7 must not increase")?;
    need(d["runtime"]["firmware"]==p.version&&d["runtime"]["agentState"]=="idle"&&d["runtime"]["imageState"]=="valid"
        &&d["runtime"]["rebootPending"]==false&&d["runtime"]["audioPlaybackActive"]==false,"quiet ready runtime required")?;
    for (key,min) in [("/memory/minimumFreeInternalBytes",24576),("/memory/largestDmaBlockBytes",65536)]{need(number(d,key)? >=min,"heap/DMA floor")?;}
    need(number(d,"/runtime/renderLifetime/samples")?>0&&number(d,"/runtime/renderLifetime/maxUs")?<=66667
        &&number(d,"/runtime/renderLifetime/maxGapUs")?<=66667,"whole lifetime frame/gap66667")?;
    for k in ["overBudget","gapOverBudget","invalidClock"]{need(number(d,&format!("/runtime/renderLifetime/{k}"))?==0,"lifetime violations; no peak reset allowed")?;}
    let s=&d["runtime"]["saveWorkerStack"];
    need(s["available"]==true&&number(s,"/invalidSamples")?==0&&number(s,"/taskStackBytes")?==4096
        &&number(s,"/observations")?>=2&&(1024..=4096).contains(&number(s,"/minimumFreeBytes")?),"SAVE4096/min1024 contract")?;
    let f=&d["runtime"]["nativePrefetch"];
    need(number(f,"/completed")?>0&&number(f,"/presentedFrames")?>0&&(1024..=6144).contains(&number(f,"/minWorkerFreeBytes")?),"prefetch6144/min1024 contract")
}
fn advance(before:&Value,after:&Value)->Result<(),String>{
    for key in ["/runtime/renderLifetime/samples","/runtime/uptimeMs"]{
        need(number(&after["diagnostics"],key)?>number(&before["diagnostics"],key)?,"frame/clock counters did not advance")?;
    }
    for key in ["/runtime/renderLifetime/maxUs","/runtime/renderLifetime/maxGapUs"]{
        need(number(&after["diagnostics"],key)?>=number(&before["diagnostics"],key)?,"lifetime peak regressed/reset")?;
    }Ok(())
}
fn verify_transfer(t:&Trace,p:&Plan)->Result<Value,String>{
    need(t.parser.line.is_empty()&&t.parser.remaining==0,"partial TX raw/header remains")?;
    let frames=|topic:&str|t.frames.iter().filter(|v|v["topic"]==topic).collect::<Vec<_>>();
    let begins=frames("asset/begin");let commits=frames("asset/commit");let files=frames("asset/file");
    need(begins.len()==1&&commits.len()==1&&files.len()==2,"exactly one BEGIN/COMMIT and two FILEs required")?;
    let id=begins[0]["payload"]["transferId"].as_str().ok_or("transferId missing")?;
    need(begins[0]["payload"]["totalBytes"]==p.expected_bytes&&begins[0]["payload"]["rawBytes"]==p.expected_raw_bytes
        &&commits[0]["payload"]["transferId"]==id&&commits[0]["payload"]["fileCount"]==2&&commits[0]["payload"]["totalBytes"]==p.expected_bytes,"BEGIN/COMMIT exact metadata")?;
    for file in &p.files{
        let sent=files.iter().filter(|v|v["payload"]["path"]==file.device_path).collect::<Vec<_>>();
        need(sent.len()==1&&sent[0]["payload"]["transferId"]==id&&sent[0]["payload"]["size"]==file.bytes
            &&sent[0]["payload"]["checksum"]==file.checksum,"FILE exact source metadata")?;
        let raw=frames("asset/raw-chunk").into_iter().filter(|v|v["payload"]["path"]==file.device_path).collect::<Vec<_>>();
        need(!raw.is_empty()&&raw.iter().map(|v|v["payload"]["size"].as_u64().unwrap_or(0)).sum::<u64>()==file.bytes,"raw file byte total")?;
        for (i,v) in raw.iter().enumerate(){need(v["payload"]["transferId"]==id&&v["payload"]["index"].as_str()==Some(i.to_string().as_str()),"raw index replay/gap")?;}
    }
    let begins_rx=t.rx.iter().filter(|v|v["topic"]=="asset/ack"&&v["payload"]["phase"]=="begin"&&v["payload"]["transferId"]==id).collect::<Vec<_>>();
    need(begins_rx.len()==1&&begins_rx[0]["payload"]["ok"]==true,"unique actual BEGIN ACK required")?;
    for file in &p.files {
        let file_rx=t.rx.iter().filter(|v|v["topic"]=="asset/ack"&&v["payload"]["phase"]=="file"
            &&v["payload"]["transferId"]==id&&v["payload"]["path"]==file.device_path).collect::<Vec<_>>();
        need(file_rx.len()==1&&file_rx[0]["payload"]["ok"]==true,"unique actual FILE ACK required")?;
    }
    let ack=t.rx.iter().filter(|v|v["topic"]=="asset/ack"&&v["payload"]["phase"]=="commit"&&v["payload"]["transferId"]==id).collect::<Vec<_>>();
    need(ack.len()==1&&ack[0]["payload"]["ok"]==true,"unique real correlated positive COMMIT ACK required")?;
    Ok(json!({"transferId":id,"commitAck":ack[0],"beginCount":1,"commitCount":1,"fileCount":2,"duringCommitQueries":"NOT_MEASURED_SINGLE_PROTOCOL_WORKER_NO_QUERY_ISSUED"}))
}

#[test]
#[ignore="Parent-only reviewed full-upload once plan, final reader/image and exclusive serial lease required"]
fn production_full_upload_commit_once(){
    let path=PathBuf::from(std::env::var("P4_APPEARANCE_COMMIT_PLAN").expect("explicit plan path required"));
    regular(&path).unwrap();let bytes=fs::read(&path).unwrap();assert!(bytes.len()<=131072);
    let plan:Plan=serde_json::from_slice(&bytes).expect("strict plan JSON");
    let mut output:Option<PathBuf>=None;let mut session:Option<Session<'_>>=None;
    let mut report=json!({"passed":false,"readerClosed":false,"commitKnown":false,"mutationRetries":0,"automaticAbort":false,"syncCalls":0,
        "rawRxCaptured":false,"rxEvidence":"actual callbacks after production CONTROL decoder; not a raw-RX transcript",
        "duringCommitQuery":"NOT_MEASURED","program750Exercise":"NO_PROGRAM_STARTED_OR_STOPPED","catalogPreservation":"PARENT_OUTER_TYPED_PHYSICAL_REQUIRED","visualReview":"NOT_RUN"});
    let result=std::panic::catch_unwind(std::panic::AssertUnwindSafe(||->Result<(),String>{
        need(std::env::var("P4_APPEARANCE_COMMIT_ACCEPT").as_deref()==Ok("1"),"explicit parent execution gate required")?;
        validate_plan(&plan)?;mkdir(&plan.output_directory)?;output=Some(plan.output_directory.clone());mkdir(&plan.app_data_directory)?;
        save(&plan.output_directory.join("plan.json"),&serde_json::from_slice(&bytes).map_err(|_|"plan JSON")?)?;
        report["sourceDigest"]=plan.source_digest.clone().into();report["readerSha256"]=plan.reader_sha256.clone().into();
        if !plan.execute{report["status"]="READONLY_LOCAL_PREFLIGHT_NO_SERIAL".into();report["passed"]=true.into();return Ok(());}
        lock_current(&plan)?;
        let once=reference(plan.once_receipt.as_ref().ok_or("durable parent claim required")?)?;
        need(once["state"]=="CLAIMED_NO_REPLAY"&&once["phase"]==PHASE&&once["board"]==plan.board&&once["version"]==plan.version
            &&once["sourceDigest"]==plan.source_digest&&once["imageSha256"]==plan.image_sha256
            &&once["run"].as_str()==plan.run_directory.to_str(),"exact original parent once claim differs")?;
        let once_ref=plan.once_receipt.as_ref().unwrap();
        need(once_ref.path.file_name().and_then(|s|s.to_str())==Some(format!("{PHASE}-{}-{}.once.json",plan.board,plan.version).as_str()),"original fixed phase/board/version claim filename required")?;
        save(&once_ref.path.with_extension("native-attempt.json"),&json!({"state":"NATIVE_ATTEMPT_NO_REPLAY","sourceDigest":plan.source_digest,
            "readerSha256":plan.reader_sha256,"run":plan.run_directory,"parentOnceSha256":once_ref.sha256}))?;
        native_absent()?;
        let log=transfer_log::configure(&plan.app_data_directory)?;
        need(log==plan.app_data_directory.join("logs/usb-transfer.jsonl"),"logger already configured elsewhere")?;
        let usb=UsbSerialManager::new();usb.set_desktop_device_id("full-upload-commit-parent");
        let trace=Arc::new(Mutex::new(Trace::default()));let bad=Arc::new(AtomicBool::new(false));
        let rows=trace.clone();let stopped=bad.clone();let cancel=usb.appearance_sync_cancel_requested.clone();let started=Instant::now();
        let lifetime=Arc::new(());let held=lifetime.clone();let reader_lifetime=Arc::downgrade(&lifetime);drop(lifetime);
        let expected_boot=plan.boot.clone();let expected_board=plan.board.clone();let expected_version=plan.version.clone();
        session=Some(Session {usb,p:&plan,trace,bad,started,generation:0,reader_lifetime});
        session.as_ref().unwrap().usb.connect(&plan.port,move|topic,payload|{
            let _keep=&held;
            let unexpected=topic=="input/event"||matches!(topic.as_str(),"audio/begin"|"audio/chunk"|"audio/end"|"audio/error")
                ||(topic=="audio/status"&&["active","conversation","playbackActive","streamActive"].iter().any(|k|payload[*k]==true))
                ||(topic=="hello"&&(payload["boardDeviceId"]!=expected_board||payload["fw"]!=expected_version
                    ||payload["capabilities"]["debugBootId"]!=expected_boot));
            if unexpected{stopped.store(true,Ordering::Release);cancel.store(true,Ordering::Release);}
            if matches!(topic.as_str(),"hello"|"debug/identity"|"diagnostics/status"|"audio/status"|"firmware/status"|"widget/program-status"|"miniapp/state"|"protocol/ack"|"asset/ack"){
                let payload=if topic=="miniapp/state"{json!({"requestId":payload["requestId"],"bootId":payload["bootId"],"widgetId":payload["widgetId"],
                    "observationAvailable":payload["observationAvailable"],"persistProjectionDigest":payload["persistProjectionDigest"]})}else{payload};
                if let Ok(mut t)=rows.lock(){if t.rx.len()<MAX_RX&&serde_json::to_vec(&payload).is_ok_and(|v|v.len()<65536){
                    t.rx.push(json!({"elapsedMs":started.elapsed().as_millis(),"topic":topic,"payload":payload}));
                }else{stopped.store(true,Ordering::Release);cancel.store(true,Ordering::Release);}}
            }
        })?;
        let generation=session.as_ref().unwrap().usb.status().connection_id.parse::<u64>().map_err(|_|"real connection generation missing")?;
        session.as_mut().unwrap().generation=generation;let s=session.as_ref().unwrap();s.scope()?;
        need(s.usb.status().capabilities["serialControlIntegrity"]==control_integrity::ENGINE,"bound strict CONTROL required")?;
        need(s.usb.supports_p4_raw_asset_chunks()&&s.usb.supports_p4_raw_ready_ack()&&s.usb.supports_p4_raw_appearance_slot(),"actual raw-ready/custom-slot protocol required; no JSON/native fallback")?;
        {let (_owner,mut c)=s.usb.inbound_owner.connection(&s.usb.connection)?;let c=c.as_mut().ok_or("connection lost")?;
            let inner=std::mem::replace(&mut c.writer,Box::new(io::sink()));c.writer=Box::new(Writer {inner,trace:s.trace.clone(),paths:plan.files.iter().map(|f|f.device_path.clone()).collect(),bad:s.bad.clone()});}
        let before=s.snapshot()?;save(&plan.output_directory.join("before.json"),&before)?;
        need(before["slots"]["activeSlot"]==0,"factory0 must already be active; adapter never activates it")?;
        let factory=slot(&before,0)?;let custom=slot(&before,1)?;
        need(factory["valid"]==true&&factory["packId"]==plan.factory_pack_id,"exact factory0 required")?;
        need(before["slots"]["slots"].as_array().unwrap().iter().all(|v|v["packId"]!=plan.pack_id),"candidate already cached; refuse non-commit reuse")?;
        if custom["valid"]==true{
            let proof=overwrite_authorization(&plan)?.ok_or("valid custom1 needs explicit overwrite authorization")?;
            need(custom==proof["expectedSlotSnapshot"],"fresh same-manager custom1 differs from exact authorized snapshot")?;
            if test_device_no_backup(&proof,&plan.board,&plan.boot){
                report["explicitTestDeviceNoBackupAuthorized"]=true.into();
                report["restoration"]="NOT_REQUIRED_BY_EXPLICIT_TEST_DEVICE_AUTHORIZATION_NOT_RUN".into();
            }else{
                report["retainedLocalSourceVerified"]=true.into();
                report["sourcePublic"]=proof["sourcePublic"].as_bool().map(Value::Bool).unwrap_or(Value::Null);
                report["restoration"]="NOT_RUN_NO_RESTORE_SUCCESS_CLAIM".into();
            }
        }else{
            need(custom["valid"]==false&&plan.custom_pack_id.is_none()&&plan.overwrite_proof.is_none(),"empty custom1 baseline/proof differs")?;
        }
        lock_current(&plan)?;validate_pack_again(&plan)?;s.scope()?;
        native_absent()?;
        save(&plan.output_directory.join("sync-intent.json"),&json!({"phase":PHASE,"sourceDigest":plan.source_digest,"board":plan.board,"boot":plan.boot,
            "packId":plan.pack_id,"files":2,"bytes":plan.expected_bytes,"parentOnce":plan.once_receipt.as_ref().unwrap().path,"syncCalls":1,"noReplay":true}))?;
        report["syncCalls"]=1.into();
        let tx=s.trace.clone();let stages=s.trace.clone();let alarm=s.bad.clone();let stage_alarm=s.bad.clone();
        let outcome=s.usb.sync_appearance_p4_with_stages(&plan.pack_directory,&plan.app_data_directory,&plan.board,false,
            move|files,total,bytes,all|{assert!(!alarm.load(Ordering::Acquire),"unexpected device activity; no replay");
                let mut t=tx.lock().unwrap();assert!(t.progress.len()<4096);t.progress.push(json!({"files":files,"totalFiles":total,"bytes":bytes,"totalBytes":all}));},
            move|name|{assert!(!stage_alarm.load(Ordering::Acquire),"unexpected device activity; stop before next phase");let mut t=stages.lock().unwrap();assert!(t.stages.len()<64);t.stages.push(json!({"stage":name}));})?;
        need(outcome==(2,plan.expected_bytes,false),"production outcome differs/full upload was reused")?;
        let observed=Instant::now();let after_commit=s.snapshot()?;advance(&before,&after_commit)?;
        save(&plan.output_directory.join("after-commit.json"),&after_commit)?;let mut previous=after_commit;
        let mut samples=Vec::new();
        while observed.elapsed()<Duration::from_secs(10)||samples.len()<2{std::thread::sleep(Duration::from_secs(1));
            let next=s.snapshot()?;advance(&previous,&next)?;samples.push(next.clone());previous=next;}
        need(previous["slots"]["activeSlot"]==1&&slot(&previous,1)?["valid"]==true&&slot(&previous,1)?["packId"]==plan.pack_id&&slot(&previous,0)?==factory,"candidate1/factory0 preservation failed")?;
        report["transferProof"]=verify_transfer(&s.trace.lock().unwrap(),&plan)?;
        report["afterAckObservationSeconds"]=observed.elapsed().as_secs_f64().into();report["afterAckSamples"]=json!(samples);
        report["status"]="FULL_UPLOAD_COMMIT_MEASUREMENTS_PASSED_OUTER_PRESERVATION_PENDING".into();report["passed"]=true.into();
        lock_current(&plan)?;validate_pack_again(&plan)?;Ok(())
    }));
    match result{Ok(Ok(()))=>{},Ok(Err(e))=>{report["error"]=e.into();report["passed"]=false.into();report["status"]="STOP_NO_REPLAY_READONLY_RECONCILIATION_REQUIRED".into();},Err(_)=>{report["error"]="panic; unknown stop, do not rerun".into();report["passed"]=false.into();report["status"]="STOP_NO_REPLAY_READONLY_RECONCILIATION_REQUIRED".into();}}
    let mut reader_closed=true;
    if let Some(s)=session.as_ref(){s.usb.disconnect();let until=Instant::now()+Duration::from_secs(5);
        while s.reader_lifetime.upgrade().is_some()&&Instant::now()<until{std::thread::sleep(Duration::from_millis(10));}
        reader_closed=s.reader_lifetime.upgrade().is_none();
        if !reader_closed{report["passed"]=false.into();report["error"]="actual reader lifetime still open".into();report["status"]="STOP_NO_REPLAY_READONLY_RECONCILIATION_REQUIRED".into();}
        if let Some(out)=output.as_ref(){let t=s.trace.lock().unwrap();
        save(&out.join("callbacks.json"),&json!(t.rx)).unwrap();save(&out.join("tx-frames.json"),&json!(t.frames)).unwrap();
        save(&out.join("progress.json"),&json!({"stages":t.stages,"progress":t.progress})).unwrap();
        let mut f=OpenOptions::new().write(true).create_new(true).open(out.join("tx.private.bin")).unwrap();f.write_all(&t.tx).unwrap();f.sync_all().unwrap();
        if let Ok(proof)=verify_transfer(&t,&plan){report["commitKnown"]=true.into();report["transferProof"]=proof;}
        report["txSha256"]=format!("{:x}",Sha256::digest(&t.tx)).into();report["windowSeconds"]=s.started.elapsed().as_secs_f64().into();}}
    report["readerClosed"]=reader_closed.into();if let Some(out)=output{save(&out.join("result.json"),&report).unwrap();}
    assert_eq!(report["passed"],true,"full-upload stopped; parent read-only reconciliation only, never rerun mutation");
}
fn validate_pack_again(p:&Plan)->Result<(),String>{
    overwrite_authorization(p)?;
    for f in &p.files{regular(&f.source_path)?;need(digest(&f.source_path)?==f.sha256,"frozen source pack changed")?;}Ok(())
}

#[test]
fn memory_contract_rejects_extra_mutations_before_forwarding(){
    struct Sink(Arc<Mutex<Vec<u8>>>);
    impl Write for Sink{fn write(&mut self,b:&[u8])->io::Result<usize>{self.0.lock().unwrap().extend_from_slice(b);Ok(b.len())}fn flush(&mut self)->io::Result<()>{Ok(())}}
    for topic in ["asset/abort","asset/patch-commit","asset/activate","firmware/begin","system/reboot","diagnostics/reset"]{
        let bytes=Arc::new(Mutex::new(vec![]));let trace=Arc::new(Mutex::new(Trace::default()));
        let mut writer=Writer{inner:Box::new(Sink(bytes.clone())),trace,paths:vec!["p4/manifest.json".into()],bad:Arc::new(AtomicBool::new(false))};
        let line=format!("{}\n",json!({"topic":topic,"payload":{}}));
        assert!(writer.write(line.as_bytes()).is_err());assert!(bytes.lock().unwrap().is_empty());
    }
}
#[test]
fn memory_contract_raw_payload_is_not_a_second_commit_frame(){
    let raw=b"{\"topic\":\"asset/commit\",\"payload\":{}}\n";
    let header=format!("{}\n",json!({"topic":"asset/raw-chunk","payload":{"size":raw.len(),"checksum":asset_checksum_hex(raw)}}));
    let commit=format!("{}\n",json!({"topic":"asset/commit","payload":{"transferId":"owned"}}));
    let mut parser=Parser::default();let mut frames=vec![];
    let data=[header.as_bytes(),raw.as_slice(),commit.as_bytes()].concat();
    for bytes in data.chunks(7){frames.extend(parser.feed(bytes).unwrap());}
    assert_eq!(frames.iter().filter(|v|v["topic"]=="asset/commit").count(),1);
    assert_eq!(frames.len(),2);assert!(parser.line.is_empty()&&parser.remaining==0);
    let mut bad=Parser::default();bad.feed(header.as_bytes()).unwrap();
    let changed=vec![0u8;raw.len()];assert!(bad.feed(&changed).is_err());
}
#[test]
fn memory_contract_missing_plan_pins_do_not_get_defaults(){
    assert!(serde_json::from_value::<Plan>(json!({"schemaVersion":1,"execute":true})).is_err());
    assert!(!hex("REQUIRED_FINAL_SOURCE",64));assert!(!hex("",16));
}

#[test]
fn memory_contract_retained_media_never_admits_profile_or_provider_files(){
    for path in ["p4/manifest.json","p4/families/pet.h264","p4/families/pet.mjpg","p4/audio/cue.pcm","p4/audio/cue.wav"]{
        assert!(retained_media_path(path));
    }
    for path in ["manifest.json","profile.json","credentials.yaml","provider.json","p4/provider.json","p4/extra.json","p4/helper.js","p4/../provider.wav","/p4/pet.h264","p4//pet.h264"]{
        assert!(!retained_media_path(path));
    }
    // Exercise the actual production authorization predicate, not a JSON model.
    let mut proof=json!({"boardDeviceId":"board","bootId":"boot","slot":1,"packId":"id","restorePackId":"id",
        "knownRetainedLocalSource":true,"userAuthorizedOverwrite":true,
        "expectedSlotSnapshot":{"slot":1,"valid":true,"packId":"id"}});
    for public in [Value::Null,json!(false),json!(true)]{
        proof["sourcePublic"]=public;assert!(retained_authorized(&proof,"board","boot","id"));
    }
    for key in ["knownRetainedLocalSource","userAuthorizedOverwrite"]{
        let mut bad=proof.clone();bad[key]=false.into();assert!(!retained_authorized(&bad,"board","boot","id"));
    }
    let mut bad=proof.clone();bad["expectedSlotSnapshot"]["packId"]="other".into();assert!(!retained_authorized(&bad,"board","boot","id"));
    assert!(!retained_authorized(&proof,"other","boot","id"));assert!(!retained_authorized(&proof,"board","other","id"));
    assert!(retained_permissions(0o700,true));assert!(retained_permissions(0o600,false));
    for mode in [0o755,0o770,0o644,0o660,0o4600]{assert!(!retained_permissions(mode,false));assert!(!retained_permissions(mode,true));}
}

#[test]
fn memory_contract_no_backup_authorization_is_exact_custom1_only(){
    let proof=json!({"boardDeviceId":"board","bootId":"boot","slot":1,
        "explicitTestDeviceNoBackupAuthorized":true,"expectedSlotSnapshot":{"slot":1,"valid":true}});
    assert!(test_device_no_backup(&proof,"board","boot"));
    assert!(!test_device_no_backup(&proof,"other","boot"));assert!(!test_device_no_backup(&proof,"board","other"));
    for key in ["slot","explicitTestDeviceNoBackupAuthorized"]{
        let mut bad=proof.clone();bad[key]=json!(0);assert!(!test_device_no_backup(&bad,"board","boot"));
    }
    for value in [json!(0),Value::Null]{let mut bad=proof.clone();bad["expectedSlotSnapshot"]["slot"]=value;assert!(!test_device_no_backup(&bad,"board","boot"));}
    let mut bad=proof.clone();bad["expectedSlotSnapshot"]["valid"]=false.into();assert!(!test_device_no_backup(&bad,"board","boot"));
}

#[test]
fn memory_contract_acceptance_paths_are_explicit_and_scoped(){
    assert!(acceptance_path(None).is_err());
    assert!(acceptance_path(Some(std::ffi::OsString::new())).is_err());
    assert!(acceptance_path(Some("relative-fixture".into())).is_err());
    let base=std::env::temp_dir().join("appearance-private-fixture");
    assert_eq!(acceptance_path(Some(base.clone().into_os_string())).unwrap(),base);
    let filesystem_root=base.ancestors().last().unwrap();
    assert!(acceptance_path(Some(filesystem_root.as_os_str().to_owned())).is_err());
    assert!(retained_root_under(&base.join("case/p4"),&base));
    assert!(!retained_root_under(&base,&base));
    assert!(!retained_root_under(&base.with_file_name("appearance-private-fixture-other"),&base));
    assert!(!retained_root_under(&base.join("../outside/p4"),&base));
    assert!(acceptance_path(Some(base.join("../outside").into_os_string())).is_err());
    let control=std::ffi::OsString::from(format!("{}{}",base.display(),'\n'));
    assert!(acceptance_path(Some(control)).is_err());
}
