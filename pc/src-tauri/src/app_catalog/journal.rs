//! Durable single-submission ownership. An unknown board stays read-only,
//! including across process restarts. No automatic reset, eviction or replay.
use super::*;
use sha2::{Digest,Sha256};
use std::{fs::{self,File,OpenOptions},io::{Read,Write},path::{Path,PathBuf},sync::Mutex};
const ERROR:&str="catalog_journal_unavailable";
const LIMIT:usize=64;
const BYTES:u64=65536;
#[derive(Clone,Serialize,Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
struct Attempt{board:String,boot:String,request_hash:String,request_id:String,receipt:Option<Receipt>}
#[derive(Serialize,Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
struct Disk{schema_version:u32,attempts:Vec<Attempt>}
struct State{disk:Disk,blocked:bool}
pub(crate) struct Journal{path:PathBuf,_lock:File,state:Mutex<State>}
fn request_hash(request:&ReconcileRequest)->Result<String,String>{
    Ok(format!("{:x}",Sha256::digest(serde_json::to_vec(request).map_err(|_|ERROR)?)))
}
fn save(path:&Path,disk:&Disk)->Result<(),String>{
    let bytes=serde_json::to_vec(disk).map_err(|_|ERROR)?;if bytes.len() as u64>BYTES{return Err(ERROR.into());}
    let parent=path.parent().ok_or(ERROR)?;let mut file=tempfile::NamedTempFile::new_in(parent).map_err(|_|ERROR)?;
    file.write_all(&bytes).and_then(|_|file.as_file().sync_all()).map_err(|_|ERROR)?;
    file.persist(path).map_err(|_|ERROR)?.sync_all().map_err(|_|ERROR)?;
    #[cfg(unix)] File::open(parent).and_then(|f|f.sync_all()).map_err(|_|ERROR)?;
    Ok(())
}
impl Journal{
    #[cfg(test)]
    pub(crate) fn evidence_path(&self)->&Path{&self.path}
    pub(crate) fn open(directory:&Path)->Result<Self,String>{
        fs::create_dir_all(directory).map_err(|_|ERROR)?;
        let mut options=OpenOptions::new();options.read(true).write(true).create(true).truncate(false);
        #[cfg(windows)] {use std::os::windows::fs::OpenOptionsExt;options.share_mode(0);}
        let mut lock=options.open(directory.join("catalog.lock")).map_err(|_|ERROR)?;
        #[cfg(unix)] {use std::os::fd::AsRawFd;
            // SAFETY: owned descriptor remains open for the lifetime of Journal.
            if unsafe{libc::flock(lock.as_raw_fd(),libc::LOCK_EX|libc::LOCK_NB)}!=0{return Err(ERROR.into());}
        }
        #[cfg(not(any(unix,windows)))] return Err(ERROR.into());
        let initialized=lock.metadata().map_err(|_|ERROR)?.len()!=0;
        let path=directory.join("catalog-attempts-v1.json");
        let disk=match File::open(&path){
            Ok(file)=>{let mut bytes=Vec::new();file.take(BYTES+1).read_to_end(&mut bytes).map_err(|_|ERROR)?;
                if bytes.len() as u64>BYTES{return Err(ERROR.into());}serde_json::from_slice::<Disk>(&bytes).map_err(|_|ERROR)?},
            Err(e) if e.kind()==std::io::ErrorKind::NotFound&&!initialized=>{let disk=Disk{schema_version:1,attempts:vec![]};save(&path,&disk)?;disk},
            Err(_)=>return Err(ERROR.into()),
        };
        if disk.schema_version!=1||disk.attempts.len()>LIMIT{return Err(ERROR.into());}
        let mut hashes=BTreeSet::new();
        for a in &disk.attempts{
            if !valid_board(&a.board)||!hex(&a.boot,16)||!hex(&a.request_hash,64)||!hashes.insert(a.request_hash.as_str())
                ||a.request_id.len()>64||!a.request_id.starts_with("catalog-")||!a.request_id.bytes().all(|b|b.is_ascii_alphanumeric()||b==b'-')
                ||a.receipt.as_ref().is_some_and(|r|!matches!(r.commit_state.as_str(),"unchanged"|"committed")||!hex(&r.catalog_digest,8)){return Err(ERROR.into());}
        }
        lock.write_all(b"1").and_then(|_|lock.sync_all()).map_err(|_|ERROR)?;
        Ok(Self{path,_lock:lock,state:Mutex::new(State{disk,blocked:false})})
    }
    fn prior_locked(state:&State,request:&ReconcileRequest,hash:&str)->Result<Option<Outcome>,String>{
        if state.blocked{return Err(ERROR.into());}
        if state.disk.attempts.iter().any(|a|a.board==request.expected_board_device_id&&a.receipt.is_none()){
            return Err("catalog_read_only_after_unknown".into());
        }
        if let Some(receipt)=state.disk.attempts.iter().find(|a|a.request_hash==hash).and_then(|a|a.receipt.clone()){
            let mut out=Outcome::confirmed(receipt);out.code="already_confirmed_no_replay";return Ok(Some(out));
        }
        if state.disk.attempts.len()>=LIMIT{return Err("catalog_journal_full_read_only".into());}Ok(None)
    }
    pub(crate) fn prior(&self,request:&ReconcileRequest)->Result<Option<Outcome>,String>{
        request.validate()?;let hash=request_hash(request)?;let state=self.state.lock().map_err(|_|ERROR)?;Self::prior_locked(&state,request,&hash)
    }
    pub(crate) fn submit(&self,prepared:&Prepared,send:impl FnOnce(&str,Value)->Result<Value,String>)->Result<Outcome,String>{
        let request=&prepared.request;let hash=request_hash(request)?;let mut state=self.state.lock().map_err(|_|ERROR)?;
        if let Some(out)=Self::prior_locked(&state,request,&hash)?{return Ok(out);}
        let request_id=format!("catalog-{}",uuid::Uuid::new_v4());let payload=request.payload(&request_id)?;
        state.disk.attempts.push(Attempt{board:request.expected_board_device_id.clone(),boot:request.expected_boot_id.clone(),request_hash:hash,request_id:request_id.clone(),receipt:None});
        if save(&self.path,&state.disk).is_err(){state.blocked=true;return Err(ERROR.into());}
        // The durable unknown intent exists BEFORE the only transport callback.
        let receipt=match send(&request_id,payload).and_then(|reply|super::receipt(&reply,&request_id,prepared)){
            Ok(receipt)=>receipt,Err(_)=>return Ok(Outcome::unknown()),
        };
        state.disk.attempts.last_mut().unwrap().receipt=Some(receipt.clone());
        if save(&self.path,&state.disk).is_err(){state.blocked=true;return Ok(Outcome::unknown());}
        Ok(Outcome::confirmed(receipt))
    }
}

#[cfg(test)]mod tests{
    use super::*;
    fn prepared()->Prepared{
        Prepared{request:ReconcileRequest{expected_board_device_id:"board-a".into(),expected_boot_id:"0123456789abcdef".into(),expected_catalog_seq:7,
            expected_catalog_digest:"12345678".into(),remove_visible_ids:vec![],survivor_visible_order:vec![],preserve_save:true},physical_survivors:vec![],no_op:true}
    }
    #[test]fn unknown_is_durable_before_send_and_blocks_new_boot_and_process_restart(){
        let dir=tempfile::tempdir().unwrap();let journal=Journal::open(dir.path()).unwrap();let plan=prepared();let mut sends=0;
        let out=journal.submit(&plan,|_,_|{sends+=1;let disk:Disk=serde_json::from_slice(&fs::read(dir.path().join("catalog-attempts-v1.json")).unwrap()).unwrap();
            assert!(disk.attempts[0].receipt.is_none());Err("lost ack".into())}).unwrap();
        assert!(!out.confirmed&&out.read_only);assert_eq!(sends,1);assert!(journal.prior(&plan.request).is_err());drop(journal);
        let journal=Journal::open(dir.path()).unwrap();let mut next=plan.request;next.expected_boot_id="1123456789abcdef".into();assert!(journal.prior(&next).is_err());
    }
    #[test]fn known_duplicate_returns_receipt_without_submitting_a_second_payload(){
        let dir=tempfile::tempdir().unwrap();let journal=Journal::open(dir.path()).unwrap();let plan=prepared();
        assert!(journal.submit(&plan,|id,_|Ok(serde_json::json!({"schemaVersion":1,"engine":ENGINE,"requestId":id,"boardDeviceId":"board-a","bootId":"0123456789abcdef",
            "ok":true,"commitState":"unchanged","catalogSeq":7,"catalogDigest":"12345678"}))).unwrap().confirmed);
        assert_eq!(journal.submit(&plan,|_,_|panic!("duplicate submission")).unwrap().code,"already_confirmed_no_replay");
    }
    #[test]fn storage_failure_before_intent_commit_prevents_transport_submission(){
        let dir=tempfile::tempdir().unwrap();let journal=Journal::open(dir.path()).unwrap();
        fs::remove_file(&journal.path).unwrap();fs::create_dir(&journal.path).unwrap();
        assert!(journal.submit(&prepared(),|_,_|panic!("send before durable intent")).is_err());
    }
}
