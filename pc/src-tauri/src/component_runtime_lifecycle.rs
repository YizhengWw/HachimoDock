//! Host-owned, bounded work admission. Tokens never come from component JSON.
//! Cancellation invalidates delivery, not an already-started external side effect.
use std::collections::VecDeque;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};

mod resource_broker;
pub(crate) use resource_broker::{capture_owner, enter_owner, storage_commit_permit, Resource, ResourceOwner};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) struct OwnerToken(u64);

impl OwnerToken {
    pub(crate) fn fresh() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        Self(
            NEXT.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1))
                .expect("component owner token space exhausted"),
        )
    }
    pub(crate) fn generation(self) -> u64 {
        self.0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Priority {
    Status,
    Action,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Lease {
    owner: OwnerToken,
    serial: u64,
}

pub(crate) struct Work<T> {
    pub(crate) lease: Lease,
    pub(crate) value: T,
}
struct Pending<T> {
    priority: Priority,
    value: T,
}
struct State<T> {
    owner: Option<OwnerToken>,
    resource_owner: Option<ResourceOwner>,
    newest_owner: Option<OwnerToken>,
    pending: VecDeque<Pending<T>>,
    running: Option<Lease>,
    serial: u64,
    action_burst: usize,
}

pub(crate) struct OwnedQueue<T> {
    capacity: usize,
    state: Mutex<State<T>>,
    ready: Condvar,
}

impl<T> OwnedQueue<T> {
    pub(crate) fn new(capacity: usize) -> Self {
        assert!(capacity > 0);
        Self {
            capacity,
            state: Mutex::new(State {
                owner: None,
                resource_owner: None,
                newest_owner: None,
                pending: VecDeque::new(),
                running: None,
                serial: 0,
                action_burst: 0,
            }),
            ready: Condvar::new(),
        }
    }

    /// A delayed activation/cancellation cannot take ownership from a newer instance.
    pub(crate) fn activate(&self, owner: OwnerToken) -> bool {
        let mut s = self.state.lock().unwrap();
        if s.owner == Some(owner) {
            return true;
        }
        if s.newest_owner.is_some_and(|latest| owner <= latest) {
            return false;
        }
        s.newest_owner = Some(owner);
        if let Some(previous) = s.resource_owner.take() {
            previous.cancel();
        }
        s.owner = Some(owner);
        s.resource_owner = Some(ResourceOwner::for_token(owner));
        s.pending.clear();
        s.action_burst = 0;
        true
    }

    pub(crate) fn cancel(&self, owner: OwnerToken) -> bool {
        let mut s = self.state.lock().unwrap();
        if s.owner != Some(owner) {
            return false;
        }
        s.owner = None;
        if let Some(previous) = s.resource_owner.take() {
            previous.cancel();
        }
        s.pending.clear();
        // Retain a canceled physical worker's lease until it actually finishes.
        true
    }

    pub(crate) fn submit(&self, owner: OwnerToken, priority: Priority, value: T) -> Result<(), T> {
        let mut s = self.state.lock().unwrap();
        if s.owner != Some(owner) {
            return Err(value);
        }
        if priority == Priority::Status {
            if let Some(pending) = s
                .pending
                .iter_mut()
                .find(|p| p.priority == Priority::Status)
            {
                pending.value = value;
                return Ok(());
            }
        }
        if s.pending.len() == self.capacity {
            // Polls are replaceable; accepted actions are never silently evicted.
            if priority == Priority::Action {
                if let Some(index) = s
                    .pending
                    .iter()
                    .position(|p| p.priority == Priority::Status)
                {
                    s.pending.remove(index);
                } else {
                    return Err(value);
                }
            } else {
                return Err(value);
            }
        }
        s.pending.push_back(Pending { priority, value });
        self.ready.notify_one();
        Ok(())
    }

    fn pop(s: &mut State<T>) -> Option<Work<T>> {
        if s.running.is_some() {
            return None;
        }
        let owner = s.owner?;
        // Bound status starvation while giving user actions precedence over polls.
        let priority = if s.action_burst >= 8 {
            Priority::Status
        } else {
            Priority::Action
        };
        let index = s
            .pending
            .iter()
            .position(|p| p.priority == priority)
            .unwrap_or(0);
        let pending = s.pending.remove(index)?;
        s.action_burst = if pending.priority == Priority::Action {
            s.action_burst.saturating_add(1)
        } else {
            0
        };
        s.serial = s
            .serial
            .checked_add(1)
            .expect("component lease space exhausted");
        let lease = Lease {
            owner,
            serial: s.serial,
        };
        s.running = Some(lease);
        Some(Work {
            lease,
            value: pending.value,
        })
    }

    pub(crate) fn receive(&self) -> Work<T> {
        let mut s = self.state.lock().unwrap();
        loop {
            if let Some(work) = Self::pop(&mut s) {
                return work;
            }
            s = self.ready.wait(s).unwrap();
        }
    }

    pub(crate) fn is_current(&self, lease: Lease) -> bool {
        let s = self.state.lock().unwrap();
        s.owner == Some(lease.owner) && s.running == Some(lease)
    }

    /// Bind explicit desktop media actions to the current physical view too,
    /// without manufacturing a running event lease or reactivating an old token.
    pub(crate) fn visible_resource_owner(&self,token:OwnerToken)->Option<ResourceOwner>{
        let s=self.state.lock().unwrap();
        (s.owner==Some(token)).then(||s.resource_owner.clone()).flatten().filter(ResourceOwner::is_current)
    }

    /// Only a currently running host-owned request may enter its resource scope.
    pub(crate) fn resource_owner(&self, lease: Lease) -> Option<ResourceOwner> {
        let s = self.state.lock().unwrap();
        (s.owner == Some(lease.owner) && s.running == Some(lease))
            .then(|| s.resource_owner.clone())
            .flatten()
    }

    /// Return whether delivery is still permitted; release only this physical lease.
    pub(crate) fn finish(&self, lease: Lease) -> bool {
        let mut s = self.state.lock().unwrap();
        if s.running != Some(lease) {
            return false;
        }
        s.running = None;
        self.ready.notify_one();
        s.owner == Some(lease.owner)
    }
}

/// Admission happens before spawning blocking desktop work, not after it queues.
#[derive(Clone)]
pub(crate) struct Admission {
    used: Arc<Mutex<usize>>,
    limit: usize,
}
pub(crate) struct Permit {
    used: Arc<Mutex<usize>>,
}
impl Admission {
    pub(crate) fn new(limit: usize) -> Self {
        Self {
            used: Arc::new(Mutex::new(0)),
            limit,
        }
    }
    pub(crate) fn acquire(&self) -> Option<Permit> {
        let mut used = self.used.lock().unwrap();
        if *used >= self.limit {
            return None;
        }
        *used += 1;
        Some(Permit {
            used: self.used.clone(),
        })
    }
}
impl Drop for Permit {
    fn drop(&mut self) {
        *self.used.lock().unwrap() -= 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn phase2_visible_audio_authority_tracks_token_without_claiming_event_worker(){
        let q=OwnedQueue::<()>::new(2);let first=OwnerToken::fresh();assert!(q.activate(first));
        let old=q.visible_resource_owner(first).unwrap();assert!(old.is_current());
        let next=OwnerToken::fresh();assert!(q.activate(next));
        assert!(!old.is_current());assert!(q.visible_resource_owner(first).is_none());
        let current=q.visible_resource_owner(next).unwrap();assert!(current.is_current());
        assert!(q.cancel(next));assert!(!current.is_current());assert!(q.visible_resource_owner(next).is_none());
    }
    #[test]
    fn lifecycle_cancelled_completion_cannot_release_or_cancel_a_new_owner() {
        let q = OwnedQueue::new(2);
        let old = OwnerToken::fresh();
        assert!(q.activate(old));
        q.submit(old, Priority::Action, 1).unwrap();
        let first = q.receive();
        assert!(q.cancel(old));
        let new = OwnerToken::fresh();
        assert!(new.generation() > old.generation());
        assert!(q.activate(new));
        q.submit(new, Priority::Action, 2).unwrap();
        assert!(!q.is_current(first.lease));
        assert!(!q.activate(old));
        assert!(!q.cancel(old));
        assert!(OwnedQueue::pop(&mut q.state.lock().unwrap()).is_none());
        assert!(!q.finish(first.lease));
        let second = q.receive();
        assert_eq!(second.value, 2);
        assert!(!q.finish(first.lease));
        assert!(q.is_current(second.lease));
        assert!(q.finish(second.lease));
    }
    #[test]
    fn lifecycle_status_flood_coalesces_and_actions_have_bounded_priority() {
        let q = OwnedQueue::new(24);
        let owner = OwnerToken::fresh();
        q.activate(owner);
        for n in 0..1000 {
            q.submit(owner, Priority::Status, n).unwrap();
        }
        for n in 0..10 {
            q.submit(owner, Priority::Action, n).unwrap();
        }
        assert_eq!(q.state.lock().unwrap().pending.len(), 11);
        for n in 0..8 {
            let w = q.receive();
            assert_eq!(w.value, n);
            q.finish(w.lease);
        }
        let poll = q.receive();
        assert_eq!(poll.value, 999);
        q.finish(poll.lease);
        let action = q.receive();
        assert_eq!(action.value, 8);
        q.finish(action.lease);
    }
    #[test]
    fn lifecycle_capacity_never_drops_an_accepted_action_or_reactivates_cancelled_owner() {
        let q = OwnedQueue::new(2);
        let owner = OwnerToken::fresh();
        q.activate(owner);
        q.submit(owner, Priority::Status, 0).unwrap();
        q.submit(owner, Priority::Action, 1).unwrap();
        q.submit(owner, Priority::Action, 2).unwrap();
        assert_eq!(q.submit(owner, Priority::Action, 3), Err(3));
        assert_eq!(q.submit(owner, Priority::Status, 4), Err(4));
        let first = q.receive();
        assert_eq!(first.value, 1);
        q.finish(first.lease);
        let second = q.receive();
        assert_eq!(second.value, 2);
        q.finish(second.lease);
        q.cancel(owner);
        assert!(!q.activate(owner));
        assert_eq!(q.submit(owner, Priority::Action, 5), Err(5));
    }
    #[test]
    fn lifecycle_desktop_admission_is_bounded_and_released_on_unwind() {
        let admission = Admission::new(2);
        let first = admission.acquire().unwrap();
        let second = admission.acquire().unwrap();
        assert!(admission.acquire().is_none());
        drop(first);
        let _ = std::panic::catch_unwind(|| {
            let _permit = admission.acquire().unwrap();
            panic!("test");
        });
        assert!(admission.acquire().is_some());
        drop(second);
        assert_eq!(*admission.used.lock().unwrap(), 0);
    }
    #[test]
    fn lifecycle_resource_scope_tracks_queue_cancel_and_reentry_for_real_atomic_storage() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("state.json");
        let queue = OwnedQueue::new(2);
        let first = OwnerToken::fresh();
        queue.activate(first);
        queue.submit(first, Priority::Action, ()).unwrap();
        let work = queue.receive();
        let old = queue.resource_owner(work.lease).unwrap();
        {
            let _scope = enter_owner(&old);
            crate::component_app::atomic(&path, &serde_json::json!({"value":1})).unwrap();
            queue.cancel(first);
            assert!(crate::component_app::atomic(&path, &serde_json::json!({"value":99})).is_err());
        }
        let committed = std::fs::read(&path).unwrap();
        assert_eq!(serde_json::from_slice::<serde_json::Value>(&committed).unwrap()["value"], 1);
        assert!(queue.resource_owner(work.lease).is_none());
        let second = OwnerToken::fresh();
        queue.activate(second);
        queue.submit(second, Priority::Action, ()).unwrap();
        assert!(!queue.finish(work.lease));
        let work = queue.receive();
        let new = queue.resource_owner(work.lease).unwrap();
        old.cancel();
        {
            let _scope = enter_owner(&new);
            crate::component_app::atomic(&path, &serde_json::json!({"value":2})).unwrap();
        }
        new.cancel(); // A successful rename is not retroactively uncommitted.
        assert_eq!(serde_json::from_slice::<serde_json::Value>(&std::fs::read(&path).unwrap()).unwrap()["value"], 2);
        queue.finish(work.lease);
    }
}
