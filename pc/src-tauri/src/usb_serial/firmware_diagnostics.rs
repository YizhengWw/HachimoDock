//! Numeric-only OTA failure snapshots. No transport or retry behavior lives here.
use std::time::Instant;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Phase { Preflight, Begin, Chunk, Commit, Validate, Unknown }
impl Phase {
    pub(super) fn from_command(phase: &str) -> Self {
        match phase { "begin" => Self::Begin, "chunk" => Self::Chunk,
            "commit" => Self::Commit, _ => Self::Unknown }
    }
    pub(super) fn label(self) -> &'static str {
        match self { Self::Preflight => "preflight", Self::Begin => "begin",
            Self::Chunk => "chunk", Self::Commit => "commit",
            Self::Validate => "validate", Self::Unknown => "unknown" }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Stage { Preflight, RegisterWaiter, Write, WaitAck, ValidateAck, ValidateBoot }
impl Stage {
    pub(super) fn label(self) -> &'static str {
        match self { Self::Preflight => "preflight", Self::RegisterWaiter => "register_waiter",
            Self::Write => "write", Self::WaitAck => "wait_ack", Self::ValidateAck => "validate_ack",
            Self::ValidateBoot => "validate_boot" }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Outcome { Incomplete, Acknowledged, Success, SendError, AckTimeout, Rejected }
impl Outcome {
    pub(super) fn label(self) -> &'static str {
        match self { Self::Incomplete => "exit_without_completion", Self::Acknowledged => "acknowledged",
            Self::Success => "success", Self::SendError => "send_error",
            Self::AckTimeout => "ack_timeout", Self::Rejected => "rejected" }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Ack {
    pub(super) phase: Phase,
    pub(super) next_sequence: u64,
    pub(super) received_bytes: u64,
}
#[derive(Clone, Copy, Debug)]
pub(super) struct Snapshot {
    pub(super) command: bool,
    pub(super) phase: Phase,
    pub(super) stage: Stage,
    pub(super) outcome: Outcome,
    pub(super) sequence: Option<u64>,
    pub(super) offset: u64,
    pub(super) expected_bytes: u64,
    pub(super) total_bytes: u64,
    pub(super) attempt: u64,
    pub(super) last_ack: Option<Ack>,
    pub(super) elapsed_ms: u64,
}
// A local guard owns only fixed enums and counters, never payloads, errors or IDs.
// Drop covers ?/return and unwinding, not process abort/kill/power loss.
pub(super) struct Diagnostic {
    pub(super) snapshot: Snapshot,
    started: Instant,
    emit: fn(Snapshot),
}
impl Diagnostic {
    pub(super) fn new(command: bool, emit: fn(Snapshot)) -> Self {
        Self { snapshot: Snapshot { command, phase: Phase::Preflight, stage: Stage::Preflight,
            outcome: Outcome::Incomplete, sequence: None, offset: 0, expected_bytes: 0,
            total_bytes: 0, attempt: 0, last_ack: None, elapsed_ms: 0 }, started: Instant::now(), emit }
    }
    pub(super) fn request(&mut self, phase: Phase, sequence: Option<u64>, offset: u64,
        expected_bytes: u64, attempt: u64) {
        self.snapshot.phase = phase;
        self.snapshot.stage = Stage::Write;
        self.snapshot.sequence = sequence;
        self.snapshot.offset = offset;
        self.snapshot.expected_bytes = expected_bytes;
        self.snapshot.attempt = attempt;
    }
    pub(super) fn ack(&mut self, phase: Phase, next_sequence: u64, received_bytes: u64) {
        self.snapshot.stage = Stage::ValidateAck;
        self.snapshot.last_ack = Some(Ack { phase, next_sequence, received_bytes });
    }
    pub(super) fn finish(&mut self, outcome: Outcome) { self.snapshot.outcome = outcome; }
}
impl Drop for Diagnostic {
    fn drop(&mut self) {
        if !matches!(self.snapshot.outcome, Outcome::Acknowledged | Outcome::Success) {
            self.snapshot.elapsed_ms = self.started.elapsed().as_millis().min(u64::MAX as u128) as u64;
            (self.emit)(self.snapshot);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    thread_local! { static ROWS: RefCell<Vec<Snapshot>> = const { RefCell::new(Vec::new()) }; }
    fn emit(row: Snapshot) { ROWS.with(|v| v.borrow_mut().push(row)); }
    fn take() -> Vec<Snapshot> { ROWS.with(|v| std::mem::take(&mut *v.borrow_mut())) }
    fn early_return(stage: Stage) -> Result<(), ()> {
        let mut guard=Diagnostic::new(true,emit);
        guard.snapshot.stage=stage;
        Err(())?;
        Ok(())
    }
    #[test] fn every_early_error_stage_has_one_drop_record() {
        take();
        for stage in [Stage::Preflight,Stage::RegisterWaiter,Stage::Write,Stage::WaitAck,Stage::ValidateAck,Stage::ValidateBoot] {
            assert!(early_return(stage).is_err());
            let rows=take();assert_eq!(rows.len(),1);assert_eq!(rows[0].stage,stage);
        }
    }
    #[test] fn last_known_ack_survives_a_later_unconfirmed_chunk() {
        take();
        { let mut d=Diagnostic::new(false,emit);d.snapshot.total_bytes=20000;
          d.ack(Phase::Begin,0,0);d.ack(Phase::Chunk,2,8184);
          d.request(Phase::Chunk,Some(2),8184,12276,1); }
        let rows=take();assert_eq!(rows.len(),1);
        let row=rows[0];assert!(!row.command);assert_eq!(row.sequence,Some(2));assert_eq!(row.offset,8184);
        assert_eq!(row.last_ack,Some(Ack{phase:Phase::Chunk,next_sequence:2,received_bytes:8184}));
        assert_eq!(row.outcome,Outcome::Incomplete);
    }
    #[test] fn success_and_known_ack_do_not_create_per_chunk_log_volume() {
        take();for outcome in [Outcome::Acknowledged,Outcome::Success] {
            let mut d=Diagnostic::new(true,emit);d.finish(outcome);
        }assert!(take().is_empty());
    }
    #[test] fn unwind_and_fixed_error_classes_are_covered_without_raw_text() {
        take();let _=std::panic::catch_unwind(||{let _d=Diagnostic::new(false,emit);panic!("fixture");});
        assert_eq!(take().len(),1);
        for o in [Outcome::SendError,Outcome::AckTimeout,Outcome::Rejected] {
            {let mut d=Diagnostic::new(true,emit);d.finish(o);}
            assert_eq!(take()[0].outcome,o);
        }
        assert_eq!(Phase::from_command("private-input-is-not-retained"),Phase::Unknown);
        assert_eq!(Phase::Unknown.label(),"unknown");
        assert_eq!(Outcome::AckTimeout.label(),"ack_timeout");
    }
}
