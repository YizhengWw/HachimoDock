//! Shared by the real fetch worker and offline fake-transport tests. A PC
//! service owns polling/cache; an optional device epoch owns only publication.
use std::sync::{
    atomic::{AtomicU64, AtomicUsize, Ordering},
    Mutex,
};
pub struct Budget(AtomicUsize);
pub struct Permit(&'static Budget);
impl Budget {
    pub const fn new() -> Self {
        Self(AtomicUsize::new(0))
    }
    pub fn acquire(&'static self) -> Option<Permit> {
        self.0
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |n| {
                (n < 8).then_some(n + 1)
            })
            .ok()
            .map(|_| Permit(self))
    }
}
impl Drop for Permit {
    fn drop(&mut self) {
        self.0 .0.fetch_sub(1, Ordering::SeqCst);
    }
}
struct State<T> {
    cancelled: bool,
    cache: T,
}
pub struct Session<T> {
    epoch: AtomicU64,
    state: Mutex<State<T>>,
}
impl<T: Clone> Session<T> {
    pub fn new(cache: T, epoch: Option<u64>) -> Self {
        Self {
            epoch: AtomicU64::new(epoch.unwrap_or(0)),
            state: Mutex::new(State {
                cancelled: false,
                cache,
            }),
        }
    }
    pub fn active(&self) -> bool {
        !self.state.lock().unwrap().cancelled
    }
    pub fn cancel(&self) {
        let mut s = self.state.lock().unwrap();
        s.cancelled = true;
        self.epoch.store(0, Ordering::SeqCst);
    }
    pub fn attach(&self, epoch: u64) {
        let s = self.state.lock().unwrap();
        if !s.cancelled {
            self.epoch.store(epoch, Ordering::SeqCst);
        }
    }
    pub fn detach(&self, epoch: u64) {
        let _ = self
            .epoch
            .compare_exchange(epoch, 0, Ordering::SeqCst, Ordering::SeqCst);
    }
    pub fn cached(&self) -> T {
        self.state.lock().unwrap().cache.clone()
    }
    /// Fetch runs outside the state lock. Cancellation and external eligibility
    /// are rechecked before commit. cancel() serializes with the publication,
    /// so no result can be published after cancel() has returned.
    pub fn cycle(
        &self,
        eligible: impl Fn() -> bool,
        fetch: impl FnOnce() -> T,
        publish: impl FnOnce(u64, &T),
    ) -> bool {
        if !self.active() {
            return false;
        }
        if !eligible() {
            self.cancel();
            return false;
        }
        let candidate = fetch();
        if !eligible() {
            self.cancel();
            return false;
        }
        let mut s = self.state.lock().unwrap();
        if s.cancelled {
            return false;
        }
        s.cache = candidate;
        let epoch = self.epoch.load(Ordering::SeqCst);
        if epoch != 0 {
            publish(epoch, &s.cache);
        }
        true
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        atomic::{AtomicBool, AtomicU64},
        mpsc, Arc,
    };
    #[test]
    fn hidden_worker_fetches_and_caches_then_reopen_reuses_cache() {
        let session = Arc::new(Session::new(0u64, Some(10)));
        let enabled = Arc::new(AtomicBool::new(true));
        let calls = Arc::new(AtomicU64::new(0));
        let publications = Arc::new(Mutex::new(Vec::new()));
        let (tx, rx) = mpsc::channel();
        let (done_tx, done_rx) = mpsc::channel();
        let (s, e, c, p) = (
            session.clone(),
            enabled.clone(),
            calls.clone(),
            publications.clone(),
        );
        let worker = std::thread::spawn(move || {
            while rx.recv().unwrap_or(false) {
                let ok = s.cycle(
                    || e.load(Ordering::SeqCst),
                    || c.fetch_add(1, Ordering::SeqCst) + 1,
                    |epoch, value| p.lock().unwrap().push((epoch, *value)),
                );
                done_tx.send(ok).unwrap();
            }
        });
        tx.send(true).unwrap();
        assert!(done_rx.recv().unwrap());
        assert_eq!(*publications.lock().unwrap(), vec![(10, 1)]);
        session.detach(10);
        tx.send(true).unwrap();
        assert!(done_rx.recv().unwrap());
        tx.send(true).unwrap();
        assert!(done_rx.recv().unwrap());
        assert_eq!(calls.load(Ordering::SeqCst), 3);
        assert_eq!(session.cached(), 3);
        assert_eq!(publications.lock().unwrap().len(), 1);
        session.attach(20);
        assert_eq!(session.cached(), 3);
        assert_eq!(calls.load(Ordering::SeqCst), 3); // reopen: same worker/cache
        session.detach(10); // late hide of the old epoch cannot detach new instance
        tx.send(true).unwrap();
        assert!(done_rx.recv().unwrap());
        assert_eq!(publications.lock().unwrap().last(), Some(&(20, 4)));
        enabled.store(false, Ordering::SeqCst);
        tx.send(true).unwrap();
        assert!(!done_rx.recv().unwrap());
        assert!(!session.active());
        assert_eq!(calls.load(Ordering::SeqCst), 4);
        tx.send(false).unwrap();
        worker.join().unwrap();
    }
    #[test]
    fn disable_during_transport_and_new_service_never_receive_old_result() {
        let old = Arc::new(Session::new(7, Some(10)));
        let (in_tx, in_rx) = mpsc::channel();
        let (go_tx, go_rx) = mpsc::channel();
        let s = old.clone();
        let worker = std::thread::spawn(move || {
            s.cycle(
                || true,
                || {
                    in_tx.send(()).unwrap();
                    go_rx.recv().unwrap();
                    99
                },
                |_, _| panic!("cancelled response published"),
            )
        });
        in_rx.recv().unwrap();
        old.cancel();
        let new = Session::new(8, Some(20));
        go_tx.send(()).unwrap();
        assert!(!worker.join().unwrap());
        assert_eq!(old.cached(), 7);
        assert_eq!(new.cached(), 8);
    }
    #[test]
    fn deletion_declaration_change_grant_revocation_and_exit_drop_inflight_results() {
        for reason in ["delete", "declaration", "grant", "desktop exit"] {
            let valid = AtomicBool::new(true);
            let s = Session::new(3, Some(1));
            assert!(!s.cycle(
                || valid.load(Ordering::SeqCst),
                || {
                    valid.store(false, Ordering::SeqCst);
                    5
                },
                |_, _| panic!("{reason}: stale publish")
            ));
            assert_eq!(s.cached(), 3);
            assert!(!s.active());
        }
    }
    #[test]
    fn actual_worker_budget_is_eight_including_cancelled_but_unfinished_jobs() {
        let budget: &'static Budget = Box::leak(Box::new(Budget::new()));
        let mut permits = Vec::new();
        for _ in 0..8 {
            permits.push(budget.acquire().unwrap());
        }
        assert!(budget.acquire().is_none());
        let cancelled = Session::new(0, None);
        cancelled.cancel();
        assert!(budget.acquire().is_none());
        drop(permits.pop());
        assert!(budget.acquire().is_some());
    }
}
