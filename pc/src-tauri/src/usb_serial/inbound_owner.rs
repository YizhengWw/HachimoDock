//! Serial dispatch and lifecycle publication share this gate. Callbacks never
//! hold connection/waiter locks. Replacement takes this gate BEFORE connection.
use super::control_integrity;
use std::sync::{Mutex, MutexGuard, atomic::{AtomicBool,AtomicU64,Ordering}};

#[derive(Default)]
pub(super) struct Owner {gate:Mutex<()>,generation:AtomicU64}
impl Owner {
    pub(super) fn lock(&self)->Result<MutexGuard<'_,()>,String>{
        self.gate.lock().map_err(|_|"USB dispatch owner unavailable".into())
    }
    pub(super) fn connection<'a,T>(&'a self,connection:&'a Mutex<T>)->Result<(MutexGuard<'a,()>,MutexGuard<'a,T>),String>{
        loop {
            let lease=self.lock()?;
            match connection.try_lock(){
                Ok(value)=>return Ok((lease,value)),
                Err(std::sync::TryLockError::Poisoned(_))=>return Err("USB connection unavailable".into()),
                Err(std::sync::TryLockError::WouldBlock)=>{},
            }
            // A raw-ready writer holds connection while waiting for this reader.
            // Never keep the dispatch gate while waiting for that mutex.
            drop(lease);std::thread::sleep(std::time::Duration::from_millis(1));
        }
    }
    /// Owned physical writes must not reserve wire while waiting for RX to
    /// finish its callback. Conversely, never hold dispatch waiting for a
    /// connection/raw-ready holder: acquire both subordinate locks by try-lock,
    /// release everything on contention, then retry admission before any bytes.
    pub(super) fn wire_connection<'a,T>(&'a self,wire:&'a Mutex<()>,connection:&'a Mutex<T>)
        ->Result<(MutexGuard<'a,()>,MutexGuard<'a,()>,MutexGuard<'a,T>),String>{
        loop {
            let lease=self.lock()?;
            match wire.try_lock(){
                Ok(writer)=>match connection.try_lock(){
                    Ok(value)=>return Ok((lease,writer,value)),
                    Err(std::sync::TryLockError::Poisoned(_))=>return Err("USB connection unavailable".into()),
                    Err(std::sync::TryLockError::WouldBlock)=>{},
                },
                Err(std::sync::TryLockError::Poisoned(_))=>return Err("USB wire owner unavailable".into()),
                Err(std::sync::TryLockError::WouldBlock)=>{},
            }
            // No wire, connection or dispatch lock survives this backoff. It
            // retries lock admission only; it never sends/replays a frame.
            drop(lease);std::thread::sleep(std::time::Duration::from_millis(1));
        }
    }
    // Publication happens only while holding this owner gate. Reader admission
    // NEVER locks connection: raw-ready waits hold that mutex while RX must run.
    pub(super) fn publish(&self,generation:u64){self.generation.store(generation,Ordering::Release);}
    pub(super) fn is_current(&self,generation:u64)->bool{generation!=0&&self.generation.load(Ordering::Acquire)==generation}
    pub(super) fn retire(&self,generation:u64){let _=self.generation.compare_exchange(generation,0,Ordering::AcqRel,Ordering::Acquire);}
    pub(super) fn enter(&self,cancel:&AtomicBool,generation:u64)->Result<MutexGuard<'_,()>,String>{
        let lease=self.lock()?;
        if cancel.load(Ordering::Acquire)||!self.is_current(generation){
            return Err("retired USB reader".into());
        }
        Ok(lease)
    }
}

pub(super) fn requires_reprobe(payload:&serde_json::Value,framed:bool,nonce:Option<&str>)->bool {
    if framed||nonce.is_none() {return false;}
    let caps=&payload["capabilities"];
    if caps["serialControlOffer"]!=control_integrity::ENGINE||caps.get("serialControlIntegrity").is_some()
        ||caps.get("serialControlNonce").is_some(){return false;}
    let mut hello=super::SerialMessage{topic:"hello".into(),payload:payload.clone()};
    super::validate_hello_message(&mut hello).is_ok()
}
