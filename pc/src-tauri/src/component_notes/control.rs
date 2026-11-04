//! A stop is reserved BEFORE capture starts. RX marks an atomic intent; a full
//! wake mailbox never owns (and therefore cannot drop) that stop. One physical
//! attempt, no retries. A failed attempt retains the slot as a safety latch.
use std::sync::{Arc, Mutex, atomic::{AtomicU8, Ordering}};
const PREPARED:u8=1; const INFLIGHT:u8=2; const STARTED:u8=4;
const STOP:u8=8; const CLAIMED:u8=16; const FAILED:u8=32;
#[derive(Default)]
pub(super) struct Ticket(AtomicU8);
impl Ticket {
    pub(super) fn prepared(&self){self.0.fetch_or(PREPARED,Ordering::AcqRel);}
    pub(super) fn request(&self){self.0.fetch_or(STOP,Ordering::AcqRel);}
    pub(super) fn requested(&self)->bool{self.0.load(Ordering::Acquire)&STOP!=0}
    pub(super) fn admit_start(&self)->bool{
        self.0.fetch_update(Ordering::AcqRel,Ordering::Acquire,|s|
            (s&PREPARED!=0&&s&(STOP|CLAIMED|INFLIGHT|STARTED)==0).then_some(s|INFLIGHT)).is_ok()
    }
    pub(super) fn start_returned(&self){
        self.0.fetch_update(Ordering::AcqRel,Ordering::Acquire,|s|Some((s&!INFLIGHT)|STARTED)).unwrap();
    }
    /// Some(true): one exact-owner physical stop. Some(false): no start was
    /// attempted, so retire the reservation without sending a command.
    pub(super) fn claim(&self)->Option<bool>{
        self.0.fetch_update(Ordering::AcqRel,Ordering::Acquire,|s|
            (s&STOP!=0&&s&PREPARED!=0&&s&(INFLIGHT|CLAIMED)==0).then_some(s|CLAIMED))
            .ok().map(|s|s&STARTED!=0)
    }
    pub(super) fn fail(&self){self.0.fetch_or(FAILED,Ordering::AcqRel);}
    pub(super) fn failed(&self)->bool{self.0.load(Ordering::Acquire)&FAILED!=0}
}
pub(super) struct Slot<T>(Mutex<Option<Arc<T>>>);
impl<T> Default for Slot<T>{fn default()->Self{Self(Mutex::new(None))}}
impl<T> Slot<T>{
    pub(super) fn reserve(&self,value:&Arc<T>)->Result<(),&'static str>{
        let mut s=self.0.lock().map_err(|_|"录音控制槽不可用")?;
        if s.is_some(){return Err("上次录音停止尚未完成；未启动新的录音");}
        *s=Some(value.clone());Ok(())
    }
    pub(super) fn snapshot(&self)->Option<Arc<T>>{self.0.lock().ok()?.clone()}
    pub(super) fn release(&self,value:&Arc<T>){
        if let Ok(mut s)=self.0.lock(){if s.as_ref().is_some_and(|v|Arc::ptr_eq(v,value)){s.take();}}
    }
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn pending_stop_survives_full_wake_queue_and_preserves_start_then_stop(){
        let slot=Arc::new(Slot::default());let t=Arc::new(Ticket::default());slot.reserve(&t).unwrap();
        t.prepared();assert!(t.admit_start());
        let (wake,_rx)=std::sync::mpsc::sync_channel(1);wake.try_send(()).unwrap();
        t.request();assert!(wake.try_send(()).is_err());assert_eq!(t.claim(),None);
        assert!(slot.reserve(&Arc::new(Ticket::default())).is_err());
        t.start_returned();assert_eq!(slot.snapshot().unwrap().claim(),Some(true));
        assert_eq!(t.claim(),None);slot.release(&t);assert!(slot.snapshot().is_none());
    }
    #[test]fn revoke_during_prepare_never_starts_and_no_slot_reuse_until_prepare_returns(){
        let slot=Slot::default();let t=Arc::new(Ticket::default());slot.reserve(&t).unwrap();
        t.request();assert_eq!(t.claim(),None);assert!(!t.admit_start());
        t.prepared();assert_eq!(t.claim(),Some(false));slot.release(&t);
        let next=Arc::new(Ticket::default());slot.reserve(&next).unwrap();slot.release(&t);
        assert!(Arc::ptr_eq(&slot.snapshot().unwrap(),&next));
    }
    #[test]fn concurrent_stop_claim_is_once_and_failure_retains_reserved_owner(){
        let slot=Arc::new(Slot::default());let t=Arc::new(Ticket::default());slot.reserve(&t).unwrap();
        t.prepared();assert!(t.admit_start());t.start_returned();t.request();
        let mut workers=vec![];for _ in 0..8{let t=t.clone();workers.push(std::thread::spawn(move||t.claim().is_some()));}
        assert_eq!(workers.into_iter().map(|w|w.join().unwrap()).filter(|v|*v).count(),1);
        t.fail();assert!(t.failed());assert!(slot.reserve(&Arc::new(Ticket::default())).is_err());
        assert!(Arc::ptr_eq(&slot.snapshot().unwrap(),&t));assert_eq!(t.claim(),None);
    }
}
