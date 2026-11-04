//! Host-only wait for one original BEGIN ACK after synchronous flash preparation.
//! Never writes, polls a device, retries a mutation, or interprets ACK fields.
use std::sync::mpsc::{Receiver, RecvTimeoutError};
use std::time::{Duration, Instant};

// Old firmware erases the image range synchronously before its first BEGIN ACK.
// This is a bounded compatibility allowance, not measured device erase latency.
pub(super) const BEGIN_ACK_TIMEOUT: Duration = Duration::from_secs(30);
const OWNER_POLL: Duration = Duration::from_millis(50);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum WaitError { Timeout, ConnectionChanged, ChannelClosed }

pub(super) fn wait<T>(receiver: &Receiver<T>, timeout: Duration,
    current: impl Fn() -> bool) -> Result<T, WaitError> {
    let started = Instant::now();
    loop {
        if !current() { return Err(WaitError::ConnectionChanged); }
        let remaining = timeout.saturating_sub(started.elapsed());
        if remaining.is_zero() { return Err(WaitError::Timeout); }
        match receiver.recv_timeout(remaining.min(OWNER_POLL)) {
            Ok(ack) => {
                // A queued ACK from a now-retired reader cannot revive its owner.
                if !current() { return Err(WaitError::ConnectionChanged); }
                if started.elapsed() >= timeout { return Err(WaitError::Timeout); }
                return Ok(ack);
            }
            Err(RecvTimeoutError::Timeout) => (),
            Err(RecvTimeoutError::Disconnected) => return Err(WaitError::ChannelClosed),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{mpsc, Arc, atomic::{AtomicBool, Ordering}};
    use std::thread;
    #[test] fn original_ack_after_old_five_second_limit_is_accepted_without_resubmit() {
        let (tx,rx)=mpsc::channel();let began=Instant::now();
        let worker=thread::spawn(move||{thread::sleep(Duration::from_millis(5150));tx.send(7u8).unwrap();});
        assert_eq!(wait(&rx,BEGIN_ACK_TIMEOUT,||true),Ok(7));
        assert!(began.elapsed()>Duration::from_secs(5));
        assert!(began.elapsed()<BEGIN_ACK_TIMEOUT);worker.join().unwrap();
    }
    #[test] fn disconnect_interrupts_long_wait_and_retired_queued_ack_is_rejected() {
        let (_tx,rx)=mpsc::channel::<u8>();let current=Arc::new(AtomicBool::new(true));
        let other=current.clone();let worker=thread::spawn(move||{thread::sleep(Duration::from_millis(20));other.store(false,Ordering::Release);});
        let start=Instant::now();assert_eq!(wait(&rx,BEGIN_ACK_TIMEOUT,||current.load(Ordering::Acquire)),Err(WaitError::ConnectionChanged));
        assert!(start.elapsed()<Duration::from_millis(500));worker.join().unwrap();
        let (tx,rx)=mpsc::channel();tx.send(1u8).unwrap();
        assert_eq!(wait(&rx,BEGIN_ACK_TIMEOUT,||false),Err(WaitError::ConnectionChanged));
        let calls=std::cell::Cell::new(0);
        assert_eq!(wait(&rx,BEGIN_ACK_TIMEOUT,||{calls.set(calls.get()+1);calls.get()==1}),Err(WaitError::ConnectionChanged));
    }
    #[test] fn deadline_and_closed_channel_fail_closed_without_retry_or_consuming_late_ack() {
        let (tx,rx)=mpsc::channel();
        assert_eq!(wait(&rx,Duration::from_millis(20),||true),Err(WaitError::Timeout));
        tx.send(3u8).unwrap();assert_eq!(rx.try_recv(),Ok(3));
        drop(tx);assert_eq!(wait(&rx,BEGIN_ACK_TIMEOUT,||true),Err(WaitError::ChannelClosed));
        assert_eq!(BEGIN_ACK_TIMEOUT,Duration::from_secs(30));
    }
}
