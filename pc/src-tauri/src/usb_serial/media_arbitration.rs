//! Media has priority over replaceable widget snapshots, but never interleaves
//! with a bulk transaction. Only lock acquisition is retried: a transmitted
//! audio chunk with an uncertain ACK must not be replayed.
use std::sync::{atomic::{AtomicUsize, Ordering}, Mutex, MutexGuard, TryLockError};
use std::time::{Duration, Instant};

pub(super) const WAIT_BUDGET: Duration = Duration::from_millis(400);

pub(super) struct PendingMedia<'a>(&'a AtomicUsize);
impl<'a> PendingMedia<'a> {
    pub(super) fn new(count: &'a AtomicUsize) -> Self {
        count.fetch_add(1, Ordering::SeqCst);
        Self(count)
    }
}
impl Drop for PendingMedia<'_> {
    fn drop(&mut self) { self.0.fetch_sub(1, Ordering::SeqCst); }
}

pub(super) fn acquire(lock: &Mutex<()>, budget: Duration) -> Result<MutexGuard<'_, ()>, String> {
    let started = Instant::now();
    loop {
        match lock.try_lock() {
            Ok(guard) => return Ok(guard),
            Err(TryLockError::Poisoned(_)) => return Err("设备传输锁异常，请重新启动客户端".into()),
            Err(TryLockError::WouldBlock) if started.elapsed() >= budget =>
                return Err("设备资源传输持续繁忙，音乐已停止；请完成传输后重试".into()),
            Err(TryLockError::WouldBlock) => std::thread::sleep(Duration::from_millis(2)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, mpsc};
    #[test]
    fn transient_snapshot_contention_does_not_abort_music() {
        let lock = Arc::new(Mutex::new(()));
        let other = lock.clone();
        let (tx, rx) = mpsc::channel();
        let worker = std::thread::spawn(move || {
            let _guard = other.lock().unwrap();
            tx.send(()).unwrap();
            std::thread::sleep(Duration::from_millis(30));
        });
        rx.recv().unwrap();
        let _guard = acquire(&lock, WAIT_BUDGET).unwrap();
        worker.join().unwrap();
    }
    #[test]
    fn bulk_transfer_wait_is_bounded_and_priority_is_released_on_failure() {
        let lock = Mutex::new(());
        let _bulk = lock.lock().unwrap();
        let pending = AtomicUsize::new(0);
        let started = Instant::now();
        {
            let _priority = PendingMedia::new(&pending);
            assert_eq!(pending.load(Ordering::SeqCst), 1);
            assert!(acquire(&lock, Duration::from_millis(20)).unwrap_err().contains("持续繁忙"));
        }
        assert!(started.elapsed() < Duration::from_secs(1));
        assert_eq!(pending.load(Ordering::SeqCst), 0);
    }
    #[test]
    fn overlapping_requests_keep_priority_until_last_request_finishes() {
        let pending = AtomicUsize::new(0);
        let first = PendingMedia::new(&pending);
        let second = PendingMedia::new(&pending);
        drop(first);
        assert_eq!(pending.load(Ordering::SeqCst), 1);
        drop(second);
        assert_eq!(pending.load(Ordering::SeqCst), 0);
    }
    #[test]
    fn poisoned_lock_is_not_reported_as_contention() {
        let lock = Arc::new(Mutex::new(()));
        let other = lock.clone();
        let _ = std::thread::spawn(move || { let _guard = other.lock().unwrap(); panic!("test"); }).join();
        assert!(acquire(&lock, WAIT_BUDGET).unwrap_err().contains("锁异常"));
    }
}
