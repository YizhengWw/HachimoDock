//! Local metadata for one device-voice round. No USB, desktop or cloud calls.
//!
//! Register the round in the parent's pending-writer registry BEFORE obtaining
//! a startup permit. Store a returned Bridge in that round BEFORE publishing
//! `Ready`, even when admission has closed. An actual release ACK or joined native worker may
//! complete ownership release; neither a queued cancel nor a closed flag
//! proves that an old writer has stopped. Keep unknown/failed global barriers.

use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};


/// Ownership proof only: never evidence that a business submit succeeded.
/// Constructed only after the exact native worker JoinHandle has been joined.
pub(crate) struct WriterTerminationProof { _joined: () }

enum NativeWorkerState {
    Running(std::thread::JoinHandle<()>),
    Joined,
}
pub(crate) trait OwnedProcessTree: Send + Sync {
    /// Administrative teardown only. Never replay editor commands.
    fn terminate(&self) -> Result<(), String>;
    /// True only after actual root-child wait and contained job ActiveProcesses=0.
    fn reaped_and_empty(&self) -> Result<bool, String>;
}
pub(crate) struct WriterTermination {
    state: Mutex<NativeWorkerState>,
    process: Option<Arc<dyn OwnedProcessTree>>,
}
impl WriterTermination {
    pub(crate) fn new(handle: std::thread::JoinHandle<()>) -> Self {
        Self { state: Mutex::new(NativeWorkerState::Running(handle)), process: None }
    }
    pub(crate) fn owned_process(handle: Option<std::thread::JoinHandle<()>>, process: Arc<dyn OwnedProcessTree>) -> Self {
        Self { state: Mutex::new(match handle { Some(h) => NativeWorkerState::Running(h), None => NativeWorkerState::Joined }), process: Some(process) }
    }
    pub(crate) fn terminate_process(&self) -> Result<(), String> {
        match self.process.as_ref() { Some(process) => process.terminate(), None => Ok(()) }
    }
    pub(crate) fn wait(&self, timeout: Duration) -> Result<Option<WriterTerminationProof>, String> {
        let deadline = Instant::now().checked_add(timeout).ok_or_else(lock_error)?;
        loop {
            if let Some(proof) = self.observe()? { return Ok(Some(proof)); }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() { return Ok(None); }
            std::thread::sleep(remaining.min(Duration::from_millis(2)));
        }
    }
    pub(crate) fn observe(&self) -> Result<Option<WriterTerminationProof>, String> {
        if let Some(process) = self.process.as_ref() {
            if !process.reaped_and_empty()? { return Ok(None); }
        }
        let mut state = self.state.lock().map_err(|_| lock_error())?;
        match &*state {
            NativeWorkerState::Running(handle) if !handle.is_finished() => return Ok(None),
            NativeWorkerState::Joined => return Ok(Some(WriterTerminationProof { _joined: () })),
            _ => {}
        }
        if let NativeWorkerState::Running(handle) =
            std::mem::replace(&mut *state, NativeWorkerState::Joined)
        {
            // Even panic joins prove there can be no later UI operations from
            // this worker. They do not acknowledge confirm/cancel business results.
            let _ = handle.join();
        }
        Ok(Some(WriterTerminationProof { _joined: () }))
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum StopReason {
    Error(String),
    /// Input/recognition already completed; this is delivery, not microphone failure.
    DeliveryError(String),
    Cancelled(String),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WriterState {
    /// Startup was never admitted, or its factory returned no Bridge.
    NoWriter,
    /// The actual Bridge has been stored in the owning context/barrier.
    Ready,
}

enum StartupState {
    NotStarted,
    Pending,
    Finished(WriterState),
    Unknown,
}

enum ReleaseState {
    Idle,
    Running,
    Finished(Result<(), String>),
}

struct State {
    admission_open: bool,
    first_stop: Option<StopReason>,
    startup: StartupState,
    release: ReleaseState,
}

pub struct DeviceVoiceLifecycle {
    state: Mutex<State>,
    changed: Condvar,
}

impl Default for DeviceVoiceLifecycle {
    fn default() -> Self {
        Self::new()
    }
}

impl DeviceVoiceLifecycle {
    pub fn new() -> Self {
        Self {
            state: Mutex::new(State {
                admission_open: true,
                first_stop: None,
                startup: StartupState::NotStarted,
                release: ReleaseState::Idle,
            }),
            changed: Condvar::new(),
        }
    }

    /// Exactly one Error/Cancelled publisher wins. A normal ASR Final is not
    /// a stop: it may still need the already admitted composer startup.
    pub fn stop_once(&self, reason: StopReason) -> Result<bool, String> {
        let mut state = self.state.lock().map_err(|_| lock_error())?;
        state.admission_open = false;
        if state.first_stop.is_some() {
            return Ok(false);
        }
        state.first_stop = Some(reason);
        self.changed.notify_all();
        Ok(true)
    }

    pub fn stop_reason(&self) -> Result<Option<StopReason>, String> {
        Ok(self.state.lock().map_err(|_| lock_error())?.first_stop.clone())
    }

    pub fn close_admission(&self) -> Result<(), String> {
        self.state.lock().map_err(|_| lock_error())?.admission_open = false;
        self.changed.notify_all();
        Ok(())
    }

    pub fn admission_open(&self) -> bool {
        self.state.lock().map(|s| s.admission_open).unwrap_or(false)
    }

    /// Call BEFORE spawning/entering the native Bridge factory. Moving this
    /// permit into a worker keeps Pending visible even before that worker runs.
    pub fn begin_startup(self: &Arc<Self>) -> Result<StartupPermit, String> {
        let mut state = self.state.lock().map_err(|_| lock_error())?;
        if !state.admission_open || !matches!(state.startup, StartupState::NotStarted) {
            return Err("voice writer startup is closed or already registered".into());
        }
        state.startup = StartupState::Pending;
        Ok(StartupPermit { lifecycle: self.clone(), finished: false })
    }

    /// Both cleanup and the next begin call this API. Only Owner performs the
    /// external release; all other callers wait on the very same stored result.
    pub fn release(self: &Arc<Self>) -> Result<ReleaseRole, String> {
        let mut state = self.state.lock().map_err(|_| lock_error())?;
        state.admission_open = false;
        if matches!(state.release, ReleaseState::Idle) {
            state.release = ReleaseState::Running;
            Ok(ReleaseRole::Owner(ReleaseOwner {
                lifecycle: self.clone(), observed_startup: false, finished: false,
            }))
        } else {
            Ok(ReleaseRole::Waiter(ReleaseWaiter { lifecycle: self.clone() }))
        }
    }

    /// A later observation may resolve an earlier release timeout without
    /// resending any command. Caller binds this proof to this context's Bridge.
    pub(crate) fn observe_terminated_writer(&self, _proof: &WriterTerminationProof) -> Result<(), String> {
        let mut state = self.state.lock().map_err(|_| lock_error())?;
        if !matches!(state.startup, StartupState::Finished(WriterState::Ready)) {
            return Err("terminated writer is not the registered Ready writer".into());
        }
        if matches!(state.release, ReleaseState::Finished(Err(_))) {
            state.release = ReleaseState::Finished(Ok(()));
            self.changed.notify_all();
        }
        Ok(())
    }

    pub fn release_result(&self) -> Result<Option<Result<(), String>>, String> {
        let state = self.state.lock().map_err(|_| lock_error())?;
        Ok(match &state.release {
            ReleaseState::Finished(result) => Some(result.clone()),
            _ => None,
        })
    }
}

#[must_use = "a registered startup must publish the actual writer or remain Unknown"]
pub struct StartupPermit {
    lifecycle: Arc<DeviceVoiceLifecycle>,
    finished: bool,
}

impl StartupPermit {
    /// A false result means no further ordinary writes are admitted. It does
    /// NOT permit discarding a Ready Bridge: the release owner must drain it.
    /// Never publish NoWriter merely because the context slot is still empty.
    pub fn finish_startup(mut self, writer: WriterState) -> Result<bool, String> {
        let mut state = self.lifecycle.state.lock().map_err(|_| lock_error())?;
        if !matches!(state.startup, StartupState::Pending) {
            return Err("voice writer startup result is already published".into());
        }
        state.startup = StartupState::Finished(writer);
        let admitted = state.admission_open;
        self.finished = true;
        self.lifecycle.changed.notify_all();
        Ok(admitted)
    }
}

impl Drop for StartupPermit {
    fn drop(&mut self) {
        if self.finished { return; }
        if let Ok(mut state) = self.lifecycle.state.lock() {
            if matches!(state.startup, StartupState::Pending) {
                state.startup = StartupState::Unknown;
                state.admission_open = false;
            }
        }
        self.lifecycle.changed.notify_all();
    }
}

pub enum ReleaseRole {
    Owner(ReleaseOwner),
    Waiter(ReleaseWaiter),
}

#[must_use = "release ownership must publish its actual result"]
pub struct ReleaseOwner {
    lifecycle: Arc<DeviceVoiceLifecycle>,
    observed_startup: bool,
    finished: bool,
}

impl ReleaseOwner {
    /// Wait only on a cleanup/startup worker, never on RX, heartbeat, or ASR.
    /// Ready means the Bridge was stored BEFORE this notification.
    pub fn wait_startup(&mut self, timeout: Duration) -> Result<WriterState, String> {
        let deadline = Instant::now().checked_add(timeout)
            .ok_or_else(|| "invalid writer startup deadline".to_string())?;
        let mut state = self.lifecycle.state.lock().map_err(|_| lock_error())?;
        loop {
            let writer = match state.startup {
                StartupState::NotStarted => Some(WriterState::NoWriter),
                StartupState::Finished(writer) => Some(writer),
                StartupState::Unknown => return Err("writer startup ended without a confirmed result".into()),
                StartupState::Pending => None,
            };
            if let Some(writer) = writer {
                self.observed_startup = true;
                return Ok(writer);
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err("writer startup is still pending; release unconfirmed".into());
            }
            state = self.lifecycle.changed.wait_timeout(state, remaining)
                .map_err(|_| lock_error())?.0;
        }
    }

    /// Pass the actual Bridge preserve/release ACK result. Errors are sticky;
    /// an owner timeout must never be retried as another release operation.
    pub fn finish(mut self, result: Result<(), String>) -> Result<(), String> {
        let result = if result.is_ok() && !self.observed_startup {
            Err("release cannot succeed before writer startup is confirmed".into())
        } else { result };
        let mut state = self.lifecycle.state.lock().map_err(|_| lock_error())?;
        let result = match &state.release {
            ReleaseState::Running => {
                state.release = ReleaseState::Finished(result.clone());
                result
            }
            ReleaseState::Finished(prior) => prior.clone(),
            ReleaseState::Idle => Err("writer release owner is not registered".into()),
        };
        self.finished = true;
        self.lifecycle.changed.notify_all();
        result
    }
}

impl Drop for ReleaseOwner {
    fn drop(&mut self) {
        if self.finished { return; }
        if let Ok(mut state) = self.lifecycle.state.lock() {
            if matches!(state.release, ReleaseState::Running) {
                state.release = ReleaseState::Finished(Err(
                    "writer release owner exited without an ACK; release unconfirmed".into(),
                ));
            }
        }
        self.lifecycle.changed.notify_all();
    }
}

pub struct ReleaseWaiter {
    lifecycle: Arc<DeviceVoiceLifecycle>,
}

impl ReleaseWaiter {
    /// A waiter timeout does not cancel the owner or overwrite its result.
    /// The global pending barrier remains until a real positive result arrives.
    pub fn wait(&self, timeout: Duration) -> Result<(), String> {
        let deadline = Instant::now().checked_add(timeout)
            .ok_or_else(|| "invalid writer release deadline".to_string())?;
        let mut state = self.lifecycle.state.lock().map_err(|_| lock_error())?;
        loop {
            if let ReleaseState::Finished(result) = &state.release {
                return result.clone();
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err("writer release is still pending; no new writer admitted".into());
            }
            state = self.lifecycle.changed.wait_timeout(state, remaining)
                .map_err(|_| lock_error())?.0;
        }
    }
}

fn lock_error() -> String {
    "voice lifecycle state unavailable; writer release unconfirmed".into()
}
