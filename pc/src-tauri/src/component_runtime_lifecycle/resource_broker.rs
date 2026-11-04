//! Typed, process-local budgets for the consumers explicitly integrated here.
//! Audio/recording and transport priorities remain with their existing arbiters.
//! Cancel revokes future use, never frees a still-executing physical operation.
use super::OwnerToken;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::marker::PhantomData;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Resource {
    PublicNetwork,
    StorageCommit,
}
impl Resource {
    fn index(self) -> usize {
        match self {
            Self::PublicNetwork => 0,
            Self::StorageCommit => 1,
        }
    }
}

struct OwnerState {
    token: OwnerToken,
    cancelled: AtomicBool,
}
#[derive(Clone)]
pub(crate) struct ResourceOwner {
    state: Arc<OwnerState>,
    broker: Arc<Broker>,
}
impl Default for ResourceOwner {
    fn default() -> Self { Self::fresh() }
}
impl ResourceOwner {
    pub(crate) fn fresh() -> Self { Self::for_token(OwnerToken::fresh()) }
    pub(super) fn for_token(token: OwnerToken) -> Self {
        Self::with_broker(token, global())
    }
    fn with_broker(token: OwnerToken, broker: Arc<Broker>) -> Self {
        Self { state: Arc::new(OwnerState { token, cancelled: AtomicBool::new(false) }), broker }
    }
    pub(crate) fn cancel(&self) {
        // Use the admission mutex for a cancellation/wait registration barrier.
        // The cancellation path does not wait for any physical resource to end.
        let _state = self.broker.state.lock().unwrap();
        self.state.cancelled.store(true, Ordering::Release);
        self.broker.ready.notify_all();
    }
    pub(crate) fn is_current(&self) -> bool {
        !self.state.cancelled.load(Ordering::Acquire)
    }
    pub(crate) fn same_owner(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.state, &other.state)
    }
    pub(crate) fn acquire(&self, resource: Resource) -> Result<ResourceLease, String> {
        self.acquire_for(resource, Duration::from_millis(250)).map_err(|error| error.message().into())
    }
    pub(crate) fn try_acquire(&self, resource: Resource) -> Result<ResourceLease, String> {
        self.acquire_for(resource, Duration::ZERO).map_err(|error| error.message().into())
    }
    #[cfg(test)]
    pub(crate) fn isolated_for_test(limits: [usize; 2]) -> Self {
        Self::with_broker(OwnerToken::fresh(), Broker::new(limits))
    }
    #[cfg(test)]
    pub(crate) fn sibling_for_test(&self) -> Self {
        Self::with_broker(OwnerToken::fresh(), self.broker.clone())
    }
    #[cfg(test)]
    pub(crate) fn used_for_test(&self, resource: Resource) -> usize {
        self.broker.state.lock().unwrap().used[resource.index()]
    }
    fn acquire_for(&self, resource: Resource, maximum_wait: Duration) -> Result<ResourceLease, Denied> {
        let index = resource.index();
        let deadline = Instant::now() + maximum_wait;
        let mut state = self.broker.state.lock().unwrap();
        let mut waiting = false;
        loop {
            if !self.is_current() {
                if waiting { state.waiting[index] -= 1; }
                return Err(Denied::Cancelled);
            }
            if state.used[index] < self.broker.limits[index] {
                if waiting { state.waiting[index] -= 1; }
                let serial = state.serial.checked_add(1).ok_or(Denied::Unavailable)?;
                state.serial = serial;
                state.used[index] += 1;
                state.active.insert(serial, (resource, self.state.token));
                return Ok(ResourceLease { owner: self.clone(), resource, serial });
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() || (!waiting && state.waiting[index] >= 16) {
                if waiting { state.waiting[index] -= 1; }
                return Err(Denied::Busy);
            }
            if !waiting { state.waiting[index] += 1; waiting = true; }
            state = self.broker.ready.wait_timeout(state, remaining).unwrap().0;
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
enum Denied { Cancelled, Busy, Unavailable }
impl Denied {
    fn message(&self) -> &'static str {
        match self {
            Self::Cancelled => "组件任务已取消，资源操作未继续",
            Self::Busy => "组件资源繁忙，操作未自动重试",
            Self::Unavailable => "组件资源上下文不可用，操作未继续",
        }
    }
}
#[derive(Default)]
struct State {
    used: [usize; 2],
    waiting: [usize; 2],
    serial: u64,
    active: BTreeMap<u64, (Resource, OwnerToken)>,
}
struct Broker {
    limits: [usize; 2],
    state: Mutex<State>,
    ready: Condvar,
}
impl Broker {
    fn new(limits: [usize; 2]) -> Arc<Self> {
        Arc::new(Self { limits, state: Mutex::new(State::default()), ready: Condvar::new() })
    }
}
fn global() -> Arc<Broker> {
    static BROKER: OnceLock<Arc<Broker>> = OnceLock::new();
    // Public GET remains at two physical requests, including polling and
    // durable workflows. Storage limits only concurrent atomic replacements;
    // existing per-store transaction locks still provide consistency.
    BROKER.get_or_init(|| Broker::new([2, 16])).clone()
}

pub(crate) struct ResourceLease {
    owner: ResourceOwner,
    resource: Resource,
    serial: u64,
}
impl ResourceLease {
    pub(crate) fn ensure_current(&self) -> Result<(), String> {
        if self.owner.is_current() { Ok(()) } else { Err(Denied::Cancelled.message().into()) }
    }
}
impl Drop for ResourceLease {
    fn drop(&mut self) {
        let mut state = self.owner.broker.state.lock().unwrap();
        if state.active.remove(&self.serial) == Some((self.resource, self.owner.state.token)) {
            state.used[self.resource.index()] -= 1;
            self.owner.broker.ready.notify_all();
        }
    }
}

const OWNER_SCOPE_LIMIT: usize = 16;
struct ScopeFrame {
    serial: u64,
    owner: ResourceOwner,
    active: bool,
}
#[derive(Clone, Copy)]
enum ScopeRegistration { Active(u64), Denied, Unavailable }
struct OwnerStack {
    frames: [Option<ScopeFrame>; OWNER_SCOPE_LIMIT],
    depth: usize,
    serial: u64,
    blocked: usize,
    unavailable: bool,
}
impl Default for OwnerStack {
    fn default() -> Self {
        Self { frames: std::array::from_fn(|_| None), depth: 0, serial: 0, blocked: 0, unavailable: false }
    }
}
impl OwnerStack {
    fn enter(&mut self, owner: &ResourceOwner) -> ScopeRegistration {
        if self.unavailable { return ScopeRegistration::Unavailable; }
        if self.serial == u64::MAX {
            self.unavailable = true;
            return ScopeRegistration::Unavailable;
        }
        if self.blocked != 0 || self.depth == OWNER_SCOPE_LIMIT {
            if let Some(blocked) = self.blocked.checked_add(1) {
                self.blocked = blocked;
                return ScopeRegistration::Denied;
            }
            self.unavailable = true;
            return ScopeRegistration::Unavailable;
        }
        self.serial += 1;
        self.frames[self.depth] = Some(ScopeFrame { serial: self.serial, owner: owner.clone(), active: true });
        self.depth += 1;
        ScopeRegistration::Active(self.serial)
    }
    fn leave(&mut self, registration: ScopeRegistration) {
        match registration {
            ScopeRegistration::Active(serial) => {
                let Some(frame) = self.frames[..self.depth].iter_mut()
                    .flatten().find(|frame| frame.serial == serial && frame.active) else {
                    self.unavailable = true;
                    return;
                };
                // Keep live children current. An inactive ancestor must never
                // be restored by a child's later cleanup, even if moved or
                // dropped out of lexical order by trusted native callers.
                frame.active = false;
                while self.depth > 0 && self.frames[self.depth - 1].as_ref().is_some_and(|frame| !frame.active) {
                    self.depth -= 1;
                    self.frames[self.depth] = None;
                }
            }
            ScopeRegistration::Denied => {
                if self.blocked == 0 { self.unavailable = true; }
                else { self.blocked -= 1; }
            }
            ScopeRegistration::Unavailable => {}
        }
    }
    fn current(&self) -> Result<Option<ResourceOwner>, Denied> {
        if self.unavailable || self.blocked != 0 { return Err(Denied::Unavailable); }
        Ok(self.depth.checked_sub(1).and_then(|index| self.frames[index].as_ref()).map(|frame| frame.owner.clone()))
    }
}
thread_local! { static CURRENT: RefCell<OwnerStack> = RefCell::new(OwnerStack::default()); }
/// Synchronous, thread-bound native scope only. Worker threads explicitly enter
/// their own owner. At most 16 frames; denied scopes fail closed until cleanup.
/// Moving/dropping guards out of order never restores a retired ancestor or
/// falls back to a legacy anonymous grant while a child/refused scope is live.
pub(crate) struct OwnerScope {
    registration: ScopeRegistration,
    _not_send: PhantomData<Rc<()>>,
}
pub(crate) fn enter_owner(owner: &ResourceOwner) -> OwnerScope {
    let registration = CURRENT.with(|current| current.borrow_mut().enter(owner));
    OwnerScope { registration, _not_send: PhantomData }
}
impl Drop for OwnerScope {
    fn drop(&mut self) {
        CURRENT.with(|current| current.borrow_mut().leave(self.registration));
    }
}
/// Preserve the original authority across preparation/worker handoff. Never
/// replace a cancelled or unavailable scope with a fresh owner.
pub(crate) fn capture_owner() -> Result<ResourceOwner, String> {
    let owner = CURRENT.with(|current| current.borrow().current())
        .map_err(|error| error.message().to_owned())?.unwrap_or_default();
    if !owner.is_current() { return Err("组件已退出，此次操作未执行".into()); }
    Ok(owner)
}

/// Legacy synchronous callers get a one-operation scope, not a user identity.
/// A host/device/workflow scope, when present, retains its cancellation policy.
pub(crate) fn storage_commit_permit() -> Result<ResourceLease, String> {
    let owner = CURRENT.with(|current| current.borrow().current())
        .map_err(|error| error.message().to_owned())?.unwrap_or_default();
    // Atomic callers may already hold their existing store transaction lock.
    // Never wait here (including nested serialization/atomic calls), and never
    // retry the mutation after ambiguous I/O. No broker lock spans file I/O.
    owner.try_acquire(Resource::StorageCommit)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn owner(broker: &Arc<Broker>) -> ResourceOwner {
        ResourceOwner::with_broker(OwnerToken::fresh(), broker.clone())
    }
    #[test]
    fn resource_cancel_keeps_physical_capacity_and_reentry_has_fresh_authority() {
        let broker = Broker::new([1, 1]);
        let old = owner(&broker);
        let lease = old.acquire(Resource::PublicNetwork).unwrap();
        old.cancel();
        assert!(lease.ensure_current().is_err());
        let new = owner(&broker);
        assert!(matches!(new.acquire_for(Resource::PublicNetwork, Duration::ZERO), Err(Denied::Busy)));
        assert!(matches!(old.acquire_for(Resource::StorageCommit, Duration::ZERO), Err(Denied::Cancelled)));
        drop(lease);
        let next = new.acquire(Resource::PublicNetwork).unwrap();
        old.cancel();
        assert!(next.ensure_current().is_ok());
        assert_eq!(broker.state.lock().unwrap().used, [1, 0]);
        drop(next);
        assert_eq!(broker.state.lock().unwrap().used, [0, 0]);
    }
    #[test]
    fn resource_types_share_a_broker_but_not_each_others_capacity() {
        let broker = Broker::new([1, 1]);
        let first = owner(&broker);
        let second = owner(&broker);
        let network = first.acquire(Resource::PublicNetwork).unwrap();
        let storage = second.acquire(Resource::StorageCommit).unwrap();
        assert!(matches!(second.acquire_for(Resource::PublicNetwork, Duration::ZERO), Err(Denied::Busy)));
        assert!(matches!(first.acquire_for(Resource::StorageCommit, Duration::ZERO), Err(Denied::Busy)));
        drop((network, storage));
        assert_eq!(broker.state.lock().unwrap().used, [0, 0]);
    }
    #[test]
    fn resource_scopes_restore_outer_owner_and_release_on_unwind() {
        let broker = Broker::new([1, 1]);
        let outer = owner(&broker);
        let inner = owner(&broker);
        let _outer = enter_owner(&outer);
        outer.cancel();
        assert!(storage_commit_permit().is_err());
        assert!(std::panic::catch_unwind(|| {
            let _inner = enter_owner(&inner);
            let _permit = storage_commit_permit().unwrap();
            panic!("synthetic unwind");
        }).is_err());
        assert_eq!(broker.state.lock().unwrap().used, [0, 0]);
        assert!(storage_commit_permit().is_err());
    }
    #[test]
    fn resource_out_of_order_scope_drop_never_falls_back_to_anonymous_storage() {
        let broker = Broker::new([1, 1]);
        let outer = owner(&broker);
        let inner = owner(&broker);
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("state.json");
        let outer_scope = enter_owner(&outer);
        let inner_scope = enter_owner(&inner);
        crate::component_app::atomic(&path, &serde_json::json!({"value":1})).unwrap();
        let before = std::fs::read(&path).unwrap();
        drop(outer_scope);
        inner.cancel();
        assert!(crate::component_app::atomic(&path, &serde_json::json!({"value":2})).is_err());
        assert_eq!(std::fs::read(&path).unwrap(), before);
        drop(inner_scope);
        assert_eq!(broker.state.lock().unwrap().used, [0, 0]);
    }
    #[test]
    fn resource_out_of_order_scope_drop_retains_the_inner_owner_and_retirement() {
        let broker = Broker::new([1, 1]);
        let outer = owner(&broker);
        let inner = owner(&broker);
        let outer_scope = enter_owner(&outer);
        let inner_scope = enter_owner(&inner);
        drop(outer_scope);
        let lease = storage_commit_permit().unwrap();
        assert!(lease.owner.same_owner(&inner));
        drop(inner_scope);
        assert!(lease.ensure_current().is_ok());
        inner.cancel();
        assert!(lease.ensure_current().is_err());
        assert_eq!(broker.state.lock().unwrap().used, [0, 1]);
        drop(lease);
        assert_eq!(broker.state.lock().unwrap().used, [0, 0]);
    }
    #[test]
    fn resource_serial_exhaustion_refuses_without_panicking_or_poisoning_the_broker() {
        let broker = Broker::new([1, 1]);
        let caller = owner(&broker);
        broker.state.lock().unwrap().serial = u64::MAX;
        assert!(caller.acquire_for(Resource::StorageCommit, Duration::ZERO).is_err());
        assert_eq!(broker.state.lock().unwrap().used, [0, 0]);
        assert!(broker.state.lock().unwrap().active.is_empty());
        caller.cancel();
        assert!(!caller.is_current());
    }
    #[test]
    fn resource_scope_depth_refusal_never_inherits_an_outer_storage_grant() {
        let broker = Broker::new([1, 1]);
        let caller = owner(&broker);
        let refused = owner(&broker);
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("state.json");
        let mut scopes = (0..OWNER_SCOPE_LIMIT).map(|_| enter_owner(&caller)).collect::<Vec<_>>();
        crate::component_app::atomic(&path, &serde_json::json!({"value":1})).unwrap();
        let before = std::fs::read(&path).unwrap();
        let denied_one = enter_owner(&refused);
        let denied_two = enter_owner(&refused);
        assert!(crate::component_app::atomic(&path, &serde_json::json!({"value":2})).is_err());
        drop(denied_one);
        drop(scopes.remove(3));
        assert!(storage_commit_permit().is_err());
        assert_eq!(std::fs::read(&path).unwrap(), before);
        drop(denied_two);
        let lease = storage_commit_permit().unwrap();
        assert!(lease.owner.same_owner(&caller));
        drop(lease);
        drop(scopes); // Vec drops in non-LIFO order; no retired ancestor returns.
        let fresh = enter_owner(&refused);
        let lease = storage_commit_permit().unwrap();
        assert!(lease.owner.same_owner(&refused));
        drop(lease);
        drop(fresh);
        assert_eq!(broker.state.lock().unwrap().used, [0, 0]);
    }
    #[test]
    fn resource_scope_middle_retirement_does_not_hide_or_resurrect_other_owners() {
        let broker = Broker::new([1, 1]);
        let first = owner(&broker);
        let second = owner(&broker);
        let third = owner(&broker);
        let a = enter_owner(&first);
        let b = enter_owner(&second);
        let c = enter_owner(&third);
        first.cancel();second.cancel();drop(b);
        let lease = storage_commit_permit().unwrap();
        assert!(lease.owner.same_owner(&third));drop(lease);
        drop(c);
        assert!(storage_commit_permit().is_err());
        drop(a);
        let new_scope = enter_owner(&third);
        assert!(storage_commit_permit().is_ok());
        drop(new_scope);
    }
    #[test]
    fn resource_scope_serial_exhaustion_latches_unavailable_without_releasing_physical_work() {
        let broker = Broker::new([1, 1]);
        let caller = owner(&broker);
        let held = caller.acquire(Resource::PublicNetwork).unwrap();
        let mut stack = OwnerStack::default();
        let original = stack.enter(&caller);
        stack.serial = u64::MAX;
        assert!(matches!(stack.enter(&caller), ScopeRegistration::Unavailable));
        assert!(matches!(stack.current(), Err(Denied::Unavailable)));
        stack.leave(original);
        assert_eq!(stack.depth, 0);
        assert!(matches!(stack.current(), Err(Denied::Unavailable)));
        assert_eq!(broker.state.lock().unwrap().used, [1, 0]);
        drop(held);
    }
    #[test]
    fn resource_scope_fixed_storage_handles_mixed_order_cleanup_against_a_reference_stack() {
        assert!(std::mem::size_of::<OwnerStack>() <= 1024);
        let broker = Broker::new([1, 1]);
        let callers = [owner(&broker), owner(&broker), owner(&broker)];
        let mut stack = OwnerStack::default();
        let mut reference: Vec<(u64, usize, bool)> = Vec::new();
        let mut registrations = Vec::new();
        let (mut sequence, mut blocked, mut admitted) = (0u64, 0usize, 0usize);
        let mut random = 0x5e7d91u32;
        for _ in 0..20000 {
            random = random.wrapping_mul(1664525).wrapping_add(1013904223);
            if registrations.is_empty() || registrations.len() < 40 && random & 3 != 0 {
                let who = (random as usize >> 8) % callers.len();
                let registration = stack.enter(&callers[who]);
                if blocked > 0 || reference.len() == OWNER_SCOPE_LIMIT {
                    assert!(matches!(registration, ScopeRegistration::Denied));
                    blocked += 1;
                } else {
                    sequence += 1;admitted += 1;
                    assert!(matches!(registration, ScopeRegistration::Active(id) if id == sequence));
                    reference.push((sequence, who, true));
                }
                registrations.push(registration);
            } else {
                let index = (random as usize >> 8) % registrations.len();
                let registration = registrations.swap_remove(index);
                stack.leave(registration);
                match registration {
                    ScopeRegistration::Active(serial) => {
                        reference.iter_mut().find(|frame| frame.0 == serial).unwrap().2 = false;
                        while reference.last().is_some_and(|frame| !frame.2) { reference.pop(); }
                    }
                    ScopeRegistration::Denied => { blocked -= 1; }
                    ScopeRegistration::Unavailable => panic!("bounded trace cannot exhaust serials"),
                }
            }
            if blocked > 0 { assert!(matches!(stack.current(), Err(Denied::Unavailable))); }
            else if let Some((_, who, _)) = reference.last() {
                assert!(stack.current().unwrap().unwrap().same_owner(&callers[*who]));
            } else { assert!(stack.current().unwrap().is_none()); }
            assert_eq!(stack.depth, reference.len());
        }
        for registration in registrations { stack.leave(registration); }
        assert!(admitted > 16);
        assert_eq!((stack.depth, stack.blocked), (0, 0));
        assert!(stack.current().unwrap().is_none());
        assert!(stack.frames.iter().all(Option::is_none));
    }
    #[test]
    fn resource_cancel_wakes_a_waiter_without_releasing_the_running_operation() {
        let broker = Broker::new([1, 1]);
        let running = owner(&broker).acquire(Resource::PublicNetwork).unwrap();
        let waiting = owner(&broker);
        let worker_owner = waiting.clone();
        let worker = std::thread::spawn(move || worker_owner.acquire_for(Resource::PublicNetwork, Duration::from_secs(2)).err());
        let deadline = Instant::now() + Duration::from_secs(1);
        while broker.state.lock().unwrap().waiting[0] == 0 {
            assert!(Instant::now() < deadline);
            std::thread::yield_now();
        }
        waiting.cancel();
        assert_eq!(worker.join().unwrap(), Some(Denied::Cancelled));
        assert_eq!(broker.state.lock().unwrap().used, [1, 0]);
        assert_eq!(broker.state.lock().unwrap().waiting, [0, 0]);
        drop(running);
    }
    #[test]
    fn resource_nested_storage_is_nonblocking_and_cancel_before_rename_preserves_old_file() {
        let broker = Broker::new([1, 1]);
        let owner = owner(&broker);
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("state.json");
        let _scope = enter_owner(&owner);
        crate::component_app::atomic(&path, &serde_json::json!({"value":1})).unwrap();
        let before = std::fs::read(&path).unwrap();
        let held = storage_commit_permit().unwrap();
        // No blocking acquisition while an outer serializer/transaction owns
        // the only slot; failed admission never invokes serialization.
        struct MustNotSerialize;
        impl serde::Serialize for MustNotSerialize {
            fn serialize<S: serde::Serializer>(&self, _: S) -> Result<S::Ok, S::Error> {
                panic!("storage admission failed before serialization");
            }
        }
        assert!(crate::component_app::atomic(&path, &MustNotSerialize).is_err());
        drop(held);
        struct CancelDuringSerialize(ResourceOwner);
        impl serde::Serialize for CancelDuringSerialize {
            fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
                self.0.cancel();
                serde::Serialize::serialize(&serde_json::json!({"value":2}), serializer)
            }
        }
        assert!(crate::component_app::atomic(&path, &CancelDuringSerialize(owner.clone())).is_err());
        assert_eq!(std::fs::read(&path).unwrap(), before);
        assert_eq!(std::fs::read_dir(dir.path()).unwrap().count(), 1, "uncommitted tempfile removed");
        assert_eq!(broker.state.lock().unwrap().used, [0, 0]);
    }
}
