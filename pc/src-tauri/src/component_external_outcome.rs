//! Default-closed trusted external-outcome boundary. No registered connector, network
//! transport, credentials, package JSON route or worker. A native-only bounded
//! Unix file journal is available but has no application caller or registration.
//! A future connector adapter must atomically compare/commit durable records and
//! hold physical admission until the provider returns, not merely sample a flag.
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::sync::{Arc, Mutex, OnceLock};

#[cfg(unix)]
#[path = "component_external_outcome/file_journal.rs"]
pub(crate) mod file_journal;

#[derive(Clone, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Key {
    component: String,
    definition: String,
    event: String,
    step: u8,
    connector: String,
    operation: String,
    payload_digest: String,
}
/* Process-local logical exclusivity, not network/storage capacity or a grant.
 * The one shared registry is consulted by BOTH entry points before journal IO.
 * Only the table's short critical section is locked; no mutex crosses a
 * journal/provider call. Cross-process exclusion still belongs to the trusted
 * durable adapter. Poison, exhaustion/full table fail closed; never evict work.
 */
#[derive(Default)]
struct OperationState {
    serial: u64,
    active: BTreeMap<OperationSlot, u64>,
}
/* Stable slot identity excludes the request fingerprint. A changed definition,
 * payload or connector is a conflict in the same event/step, not a new job. */
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord)]
struct OperationSlot {
    component: String,
    event: String,
    step: u8,
}
impl From<&Key> for OperationSlot {
    fn from(key: &Key) -> Self {
        Self {
            component: key.component.clone(),
            event: key.event.clone(),
            step: key.step,
        }
    }
}
#[derive(Default)]
struct OperationRegistry {
    state: Mutex<OperationState>,
}
struct OperationPermit {
    registry: Arc<OperationRegistry>,
    slot: OperationSlot,
    serial: u64,
}
impl OperationRegistry {
    fn try_begin(self: &Arc<Self>, key: &Key) -> Result<OperationPermit, Decision> {
        if !key.valid() {
            return Err(Decision::Invalid);
        }
        // Serialize only bounded table bookkeeping. Returning Busy just because
        // an unrelated key holds this short mutex needlessly rejects accepted
        // host work; actual occupied slots still fail immediately below. No
        // native callback, journal operation or provider IO holds this mutex.
        let mut state = self
            .state
            .lock()
            .map_err(|_| Decision::AdmissionUnavailable)?;
        let slot = OperationSlot::from(key);
        if state.active.contains_key(&slot) || state.active.len() >= 32 {
            return Err(Decision::Busy);
        }
        let Some(serial) = state.serial.checked_add(1) else {
            return Err(Decision::AdmissionUnavailable);
        };
        state.serial = serial;
        state.active.insert(slot.clone(), serial);
        Ok(OperationPermit {
            registry: self.clone(),
            slot,
            serial,
        })
    }
}
impl Drop for OperationPermit {
    fn drop(&mut self) {
        if let Ok(mut state) = self.registry.state.lock() {
            if state.active.get(&self.slot) == Some(&self.serial) {
                state.active.remove(&self.slot);
            }
        }
    }
}
fn operation_registry() -> &'static Arc<OperationRegistry> {
    static REGISTRY: OnceLock<Arc<OperationRegistry>> = OnceLock::new();
    REGISTRY.get_or_init(|| Arc::new(OperationRegistry::default()))
}
fn name(value: &str, maximum: usize) -> bool {
    !value.is_empty()
        && value.len() <= maximum
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
}
fn digest(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn component_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 44
        && value.as_bytes()[0].is_ascii_lowercase()
        && value
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"_-".contains(&b))
}
impl Key {
    pub(crate) fn new(
        component: &str,
        definition: &str,
        event: &str,
        step: u8,
        connector: &str,
        operation: &str,
        payload_digest: &str,
    ) -> Option<Self> {
        if !component_id(component)
            || !digest(definition)
            || !name(event, 96)
            || step >= 8
            || !name(connector, 32)
            || !name(operation, 32)
            || !digest(payload_digest)
        {
            return None;
        }
        let key = Self {
            component: component.into(),
            definition: definition.into(),
            event: event.into(),
            step,
            connector: connector.into(),
            operation: operation.into(),
            payload_digest: payload_digest.into(),
        };
        key.valid().then_some(key)
    }
    pub(crate) fn component(&self) -> &str {
        &self.component
    }
    pub(crate) fn definition(&self) -> &str {
        &self.definition
    }
    pub(crate) fn event(&self) -> &str {
        &self.event
    }
    pub(crate) fn step(&self) -> u8 {
        self.step
    }
    pub(crate) fn connector(&self) -> &str {
        &self.connector
    }
    pub(crate) fn operation(&self) -> &str {
        &self.operation
    }
    pub(crate) fn payload_digest(&self) -> &str {
        &self.payload_digest
    }
    fn valid(&self) -> bool {
        component_id(&self.component)
            && digest(&self.definition)
            && name(&self.event, 96)
            && self.step < 8
            && name(&self.connector, 32)
            && name(&self.operation, 32)
            && digest(&self.payload_digest)
    }
}
#[derive(Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Debug)]
#[serde(rename_all = "snake_case")]
pub(crate) enum Phase {
    Prepared,
    AwaitingAdmission,
    Claimed,
    Unknown,
    Completed,
    Rejected,
    Cancelled,
}
#[derive(Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Debug)]
#[serde(rename_all = "snake_case")]
pub(crate) enum Rejection {
    Policy,
    InvalidInput,
    NotAccepted,
}
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Record {
    schema: u8,
    key: Key,
    phase: Phase,
    reference: Option<String>,
    rejection: Option<Rejection>,
}
fn valid_reference(reference: &str) -> bool {
    name(reference, 128)
}
impl Record {
    pub(crate) fn prepared(key: Key) -> Option<Self> {
        key.valid().then_some(Self {
            schema: 1,
            key,
            phase: Phase::Prepared,
            reference: None,
            rejection: None,
        })
    }
    pub(crate) fn decode(bytes: &[u8]) -> Result<Self, JournalError> {
        if bytes.is_empty() || bytes.len() > 2048 {
            return Err(JournalError::Invalid);
        }
        let record: Self = serde_json::from_slice(bytes).map_err(|_| JournalError::Invalid)?;
        if !record.valid() {
            return Err(JournalError::Invalid);
        }
        Ok(record)
    }
    pub(crate) fn encode(&self) -> Result<Vec<u8>, JournalError> {
        if !self.valid() {
            return Err(JournalError::Invalid);
        }
        let bytes = serde_json::to_vec(self).map_err(|_| JournalError::Invalid)?;
        if bytes.len() > 2048 {
            return Err(JournalError::Invalid);
        }
        Ok(bytes)
    }
    pub(crate) fn phase(&self) -> Phase {
        self.phase
    }
    fn valid(&self) -> bool {
        if self.schema != 1
            || !self.key.valid()
            || self.reference.as_ref().is_some_and(|s| !valid_reference(s))
        {
            return false;
        }
        match self.phase {
            Phase::Prepared | Phase::AwaitingAdmission | Phase::Claimed | Phase::Cancelled => {
                self.reference.is_none() && self.rejection.is_none()
            }
            Phase::Unknown => self.rejection.is_none(),
            Phase::Completed => self.reference.is_some() && self.rejection.is_none(),
            Phase::Rejected => self.reference.is_none() && self.rejection.is_some(),
        }
    }
}
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Commit {
    Committed,
    NotCommitted,
    Unknown,
    Conflict,
}
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum JournalError {
    Unavailable,
    Invalid,
}
/// Native-only contract: compare_exchange serializes the same persistent slot,
/// compares the full expected record and proves durability before Committed.
/// Slot identity is component+event+step, NOT a hash of mutable request content.
/// A changed definition/connector/payload must encounter the old record and
/// conflict, never create a second empty journal for the same event/step.
/// Unknown must be used whenever replacement/durability is uncertain. Never
/// overwrite an unknown record with Prepared or reset a missing/corrupt journal.
/// This trait alone is NOT a production durability implementation.
pub(crate) trait Journal {
    fn load(&mut self) -> Result<Record, JournalError>;
    fn compare_exchange(&mut self, expected: &Record, next: &Record) -> Commit;
}
/// begin atomically reserves an audited connector/grant/current-owner scope.
/// The orchestration layer separately holds process-local per-key exclusivity
/// through final journal publication. Cross-process exclusion remains required.
/// Its owned permit is held THROUGH the synchronous native provider call. A
/// cancellation may invalidate delivery but cannot release physical occupancy
/// while IO is still in progress. No lifecycle mutex may be held across IO.
/// There is deliberately no built-in unconditional production implementation.
pub(crate) trait Admission {
    type Permit;
    fn authorized(&self, key: &Key, access: AccessKind) -> bool;
    fn begin(&mut self, key: &Key, access: AccessKind) -> AdmissionResult<Self::Permit>;
}
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum AccessKind {
    Submit,
    Reconcile,
}
/* Distinguish protected/revoked authority from temporarily busy physical
 * resources. Neither is an ambiguous provider result: submit has not run. */
pub(crate) enum AdmissionResult<P> {
    Admitted(P),
    Busy,
    Denied,
    Unavailable,
}
pub(crate) enum Submission {
    Completed(String),
    Rejected(Rejection),
    Unknown(Option<String>),
}
pub(crate) enum Resolution {
    Completed(String),
    ProvenNotAccepted,
    Unknown,
}
/// Only audited native adapters implement this trait. The adapter must bind the
/// immutable request body/destination/credential scope to the expected key.
/// Component/model text is not authority to claim a rejection or completion.
/// Reconcile is READ ONLY and must never submit/retry the underlying request.
/// Ordinary 404/not-found, timeout or cancel is NOT ProvenNotAccepted.
pub(crate) trait Connector {
    fn binds(&self, key: &Key) -> bool;
    fn submit(&mut self, key: &Key) -> Submission;
    fn reconcile(&mut self, key: &Key, reference: Option<&str>) -> Resolution;
}
#[derive(PartialEq, Eq)]
pub(crate) enum Decision {
    Completed { reference: String, durable: bool },
    Rejected { reason: Rejection, durable: bool },
    Cancelled { durable: bool },
    NeedsReconciliation,
    NotSubmitted,
    Conflict,
    Denied,
    Invalid,
    StoreUnavailable,
    Busy,
    AdmissionUnavailable,
}
impl std::fmt::Debug for Decision {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Completed { durable, .. } => f
                .debug_struct("Completed")
                .field("durable", durable)
                .finish(),
            Self::Rejected { reason, durable } => f
                .debug_struct("Rejected")
                .field("reason", reason)
                .field("durable", durable)
                .finish(),
            Self::Cancelled { durable } => f
                .debug_struct("Cancelled")
                .field("durable", durable)
                .finish(),
            Self::NeedsReconciliation => f.write_str("NeedsReconciliation"),
            Self::NotSubmitted => f.write_str("NotSubmitted"),
            Self::Conflict => f.write_str("Conflict"),
            Self::Denied => f.write_str("Denied"),
            Self::Invalid => f.write_str("Invalid"),
            Self::StoreUnavailable => f.write_str("StoreUnavailable"),
            Self::Busy => f.write_str("Busy"),
            Self::AdmissionUnavailable => f.write_str("AdmissionUnavailable"),
        }
    }
}
fn terminal(record: &Record, durable: bool) -> Decision {
    match record.phase {
        Phase::Completed => Decision::Completed {
            reference: record.reference.clone().unwrap(),
            durable,
        },
        Phase::Rejected => Decision::Rejected {
            reason: record.rejection.unwrap(),
            durable,
        },
        Phase::Cancelled => Decision::Cancelled { durable },
        _ => Decision::NeedsReconciliation,
    }
}
fn load(journal: &mut impl Journal, key: &Key) -> Result<Record, Decision> {
    let record = journal.load().map_err(|e| match e {
        JournalError::Invalid => Decision::Invalid,
        JournalError::Unavailable => Decision::StoreUnavailable,
    })?;
    if !record.valid() {
        return Err(Decision::Invalid);
    }
    if record.key != *key {
        return Err(Decision::Conflict);
    }
    Ok(record)
}
fn finish(journal: &mut impl Journal, claimed: &Record, next: &Record) -> Decision {
    // A result is an observed fact even if the journal cannot record it. Do
    // not relabel it unexecuted or resubmit: the prior durable claim remains.
    let committed = journal.compare_exchange(claimed, next) == Commit::Committed;
    terminal(next, committed)
}
pub(crate) fn submit_once<J: Journal, A: Admission, C: Connector>(
    journal: &mut J,
    admission: &mut A,
    connector: &mut C,
    key: &Key,
) -> Decision {
    if !key.valid() {
        return Decision::Invalid;
    }
    if !admission.authorized(key, AccessKind::Submit) || !connector.binds(key) {
        return Decision::Denied;
    }
    let _operation = match operation_registry().try_begin(key) {
        Ok(permit) => permit,
        Err(result) => return result,
    };
    let record = match load(journal, key) {
        Ok(record) => record,
        Err(result) => return result,
    };
    if !matches!(record.phase, Phase::Prepared | Phase::AwaitingAdmission) {
        return terminal(&record, true);
    }
    let mut claimed = record.clone();
    claimed.phase = Phase::Claimed;
    match journal.compare_exchange(&record, &claimed) {
        Commit::Committed => {}
        Commit::NotCommitted => return Decision::NotSubmitted,
        Commit::Unknown => return Decision::NeedsReconciliation,
        Commit::Conflict => return Decision::Conflict,
    }
    let _permit = match admission.begin(key, AccessKind::Submit) {
        AdmissionResult::Admitted(permit) => permit,
        AdmissionResult::Denied => {
            let mut cancelled = claimed.clone();
            cancelled.phase = Phase::Cancelled;
            return finish(journal, &claimed, &cancelled);
        }
        // The approved provider has provably NOT been called. Only a proven
        // checkpoint of this fact allows an explicit later admission attempt.
        // A crash/unknown checkpoint leaves Claimed, which cannot be replayed.
        denied @ (AdmissionResult::Busy | AdmissionResult::Unavailable) => {
            let mut waiting = claimed.clone();
            waiting.phase = Phase::AwaitingAdmission;
            if journal.compare_exchange(&claimed, &waiting) != Commit::Committed {
                return Decision::NeedsReconciliation;
            }
            return if matches!(denied, AdmissionResult::Busy) {
                Decision::Busy
            } else {
                Decision::AdmissionUnavailable
            };
        }
    };
    // There is intentionally no catch/retry loop. A panic/crash leaves Claimed
    // durable; subsequent invocation goes to reconciliation, never submit.
    let mut next = claimed.clone();
    match connector.submit(key) {
        Submission::Completed(reference) if valid_reference(&reference) => {
            next.phase = Phase::Completed;
            next.reference = Some(reference);
        }
        Submission::Rejected(reason) => {
            next.phase = Phase::Rejected;
            next.rejection = Some(reason);
        }
        Submission::Unknown(reference) => {
            next.phase = Phase::Unknown;
            next.reference = reference.filter(|r| valid_reference(r));
        }
        Submission::Completed(_) => next.phase = Phase::Unknown,
    }
    finish(journal, &claimed, &next)
}
pub(crate) fn reconcile_only<J: Journal, A: Admission, C: Connector>(
    journal: &mut J,
    admission: &mut A,
    connector: &mut C,
    key: &Key,
) -> Decision {
    if !key.valid() {
        return Decision::Invalid;
    }
    if !admission.authorized(key, AccessKind::Reconcile) || !connector.binds(key) {
        return Decision::Denied;
    }
    let _operation = match operation_registry().try_begin(key) {
        Ok(permit) => permit,
        Err(result) => return result,
    };
    let record = match load(journal, key) {
        Ok(record) => record,
        Err(result) => return result,
    };
    if matches!(record.phase, Phase::Prepared | Phase::AwaitingAdmission) {
        return Decision::NotSubmitted;
    }
    if !matches!(record.phase, Phase::Claimed | Phase::Unknown) {
        return terminal(&record, true);
    }
    let _permit = match admission.begin(key, AccessKind::Reconcile) {
        AdmissionResult::Admitted(permit) => permit,
        AdmissionResult::Busy => return Decision::Busy,
        AdmissionResult::Denied => return Decision::Denied,
        AdmissionResult::Unavailable => return Decision::AdmissionUnavailable,
    };
    let mut next = record.clone();
    match connector.reconcile(key, record.reference.as_deref()) {
        Resolution::Completed(reference) if valid_reference(&reference) => {
            next.phase = Phase::Completed;
            next.reference = Some(reference);
        }
        Resolution::ProvenNotAccepted => {
            next.phase = Phase::Rejected;
            next.reference = None;
            next.rejection = Some(Rejection::NotAccepted);
        }
        _ => return Decision::NeedsReconciliation,
    }
    finish(journal, &record, &next)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        cell::Cell,
        collections::VecDeque,
        rc::Rc,
        sync::atomic::{AtomicU64, Ordering},
    };
    fn key() -> Key {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        thread_local! {static TEST_EVENT:u64=NEXT.fetch_add(1,Ordering::Relaxed);}
        let event = TEST_EVENT.with(|id| format!("event-{id}"));
        Key::new(
            "synthetic",
            "a".repeat(64).as_str(),
            &event,
            0,
            "reviewed",
            "create",
            "b".repeat(64).as_str(),
        )
        .unwrap()
    }
    #[derive(Clone, Copy)]
    enum Fault {
        None,
        NoWrite,
        UnknownBefore,
        UnknownAfter,
        Conflict,
    }
    struct MemoryJournal {
        record: Record,
        faults: VecDeque<Fault>,
        writes: usize,
        unavailable: bool,
    }
    impl Journal for MemoryJournal {
        fn load(&mut self) -> Result<Record, JournalError> {
            if self.unavailable {
                Err(JournalError::Unavailable)
            } else {
                Ok(self.record.clone())
            }
        }
        fn compare_exchange(&mut self, expected: &Record, next: &Record) -> Commit {
            assert!(matches!(
                operation_registry().try_begin(&expected.key),
                Err(Decision::Busy)
            ));
            self.writes += 1;
            if self.record != *expected {
                return Commit::Conflict;
            }
            match self.faults.pop_front().unwrap_or(Fault::None) {
                Fault::None => {
                    self.record = next.clone();
                    Commit::Committed
                }
                Fault::NoWrite => Commit::NotCommitted,
                Fault::UnknownBefore => Commit::Unknown,
                Fault::UnknownAfter => {
                    self.record = next.clone();
                    Commit::Unknown
                }
                Fault::Conflict => Commit::Conflict,
            }
        }
    }
    struct Permit(Rc<Cell<u32>>);
    impl Drop for Permit {
        fn drop(&mut self) {
            self.0.set(self.0.get() - 1);
        }
    }
    struct Gate {
        authorized: bool,
        allow_submit: bool,
        allow_reconcile: bool,
        begin: bool,
        busy: bool,
        unavailable: bool,
        held: Rc<Cell<u32>>,
    }
    impl Admission for Gate {
        type Permit = Permit;
        fn authorized(&self, _: &Key, access: AccessKind) -> bool {
            self.authorized
                && match access {
                    AccessKind::Submit => self.allow_submit,
                    AccessKind::Reconcile => self.allow_reconcile,
                }
        }
        fn begin(&mut self, key: &Key, access: AccessKind) -> AdmissionResult<Permit> {
            if !self.begin || !self.authorized(key, access) {
                return AdmissionResult::Denied;
            }
            if self.unavailable {
                return AdmissionResult::Unavailable;
            }
            if self.busy || self.held.get() != 0 {
                return AdmissionResult::Busy;
            }
            self.held.set(self.held.get() + 1);
            AdmissionResult::Admitted(Permit(self.held.clone()))
        }
    }
    struct Provider {
        bound: bool,
        submits: usize,
        polls: usize,
        panic: bool,
        held: Rc<Cell<u32>>,
        submission: Option<Submission>,
        resolution: Option<Resolution>,
    }
    impl Connector for Provider {
        fn binds(&self, _: &Key) -> bool {
            self.bound
        }
        fn submit(&mut self, _: &Key) -> Submission {
            assert_eq!(self.held.get(), 1);
            self.submits += 1;
            assert!(!self.panic, "synthetic provider panic");
            self.submission.take().unwrap()
        }
        fn reconcile(&mut self, _: &Key, _: Option<&str>) -> Resolution {
            assert_eq!(self.held.get(), 1);
            self.polls += 1;
            self.resolution.take().unwrap()
        }
    }
    fn setup() -> (MemoryJournal, Gate, Provider) {
        let held = Rc::new(Cell::new(0));
        (
            MemoryJournal {
                record: Record::prepared(key()).unwrap(),
                faults: VecDeque::new(),
                writes: 0,
                unavailable: false,
            },
            Gate {
                authorized: true,
                allow_submit: true,
                allow_reconcile: true,
                begin: true,
                busy: false,
                unavailable: false,
                held: held.clone(),
            },
            Provider {
                bound: true,
                submits: 0,
                polls: 0,
                panic: false,
                held,
                submission: Some(Submission::Completed("job-1".into())),
                resolution: None,
            },
        )
    }
    #[test]
    fn completed_and_rejected_records_are_never_submitted_again() {
        for rejected in [false, true] {
            let (mut j, mut a, mut c) = setup();
            if rejected {
                c.submission = Some(Submission::Rejected(Rejection::InvalidInput));
            }
            let result = submit_once(&mut j, &mut a, &mut c, &key());
            assert_eq!(c.submits, 1);
            assert_eq!(a.held.get(), 0);
            assert_eq!(submit_once(&mut j, &mut a, &mut c, &key()), result);
            assert_eq!(c.submits, 1);
            assert_eq!(j.writes, 2);
        }
    }
    #[test]
    fn every_claim_commit_fault_prevents_provider_dispatch() {
        for fault in [
            Fault::NoWrite,
            Fault::UnknownBefore,
            Fault::UnknownAfter,
            Fault::Conflict,
        ] {
            let (mut j, mut a, mut c) = setup();
            j.faults.push_back(fault);
            let result = submit_once(&mut j, &mut a, &mut c, &key());
            assert_eq!(c.submits, 0);
            assert_eq!(a.held.get(), 0);
            assert!(matches!(
                result,
                Decision::NotSubmitted | Decision::NeedsReconciliation | Decision::Conflict
            ));
            if matches!(fault, Fault::UnknownAfter) {
                assert_eq!(
                    submit_once(&mut j, &mut a, &mut c, &key()),
                    Decision::NeedsReconciliation
                );
                assert_eq!(c.submits, 0);
            }
        }
    }
    #[test]
    fn interrupted_claim_is_not_a_resume_permission_even_after_new_native_admission() {
        let (mut j, mut a, mut c) = setup();
        j.record.phase = Phase::Claimed;
        for _ in 0..100 {
            assert_eq!(
                submit_once(&mut j, &mut a, &mut c, &key()),
                Decision::NeedsReconciliation
            );
        }
        assert_eq!(c.submits, 0);
        assert_eq!(j.writes, 0);
    }
    #[test]
    fn authority_or_binding_denial_never_reads_or_writes_as_a_default_reset() {
        let (mut j, mut a, mut c) = setup();
        a.authorized = false;
        j.unavailable = true;
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::Denied
        );
        a.authorized = true;
        c.bound = false;
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::Denied
        );
        c.bound = true;
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::StoreUnavailable
        );
        assert_eq!(c.submits, 0);
        assert_eq!(j.writes, 0);
    }
    #[test]
    fn revocation_after_durable_claim_latches_cancel_without_dispatch() {
        let (mut j, mut a, mut c) = setup();
        a.begin = false;
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::Cancelled { durable: true }
        );
        a.begin = true;
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::Cancelled { durable: true }
        );
        assert_eq!(c.submits, 0);
    }
    #[test]
    fn ambiguous_completion_save_never_becomes_an_unexecuted_retry() {
        for fault in [
            Fault::NoWrite,
            Fault::UnknownBefore,
            Fault::UnknownAfter,
            Fault::Conflict,
        ] {
            let (mut j, mut a, mut c) = setup();
            j.faults.extend([Fault::None, fault]);
            assert_eq!(
                submit_once(&mut j, &mut a, &mut c, &key()),
                Decision::Completed {
                    reference: "job-1".into(),
                    durable: false
                }
            );
            let result = submit_once(&mut j, &mut a, &mut c, &key());
            assert!(matches!(
                result,
                Decision::Completed { durable: true, .. } | Decision::NeedsReconciliation
            ));
            assert_eq!(c.submits, 1);
        }
    }
    #[test]
    fn native_provider_panic_drops_physical_permit_but_preserves_durable_claim() {
        let (mut j, mut a, mut c) = setup();
        c.panic = true;
        assert!(
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| submit_once(
                &mut j,
                &mut a,
                &mut c,
                &key()
            )))
            .is_err()
        );
        assert_eq!(a.held.get(), 0);
        assert_eq!(j.record.phase, Phase::Claimed);
        c.panic = false;
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::NeedsReconciliation
        );
        assert_eq!(c.submits, 1);
    }
    #[test]
    fn unknown_result_is_read_only_reconciled_without_any_submit_retry() {
        let (mut j, mut a, mut c) = setup();
        c.submission = Some(Submission::Unknown(Some("pending-1".into())));
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::NeedsReconciliation
        );
        c.resolution = Some(Resolution::Unknown);
        assert_eq!(
            reconcile_only(&mut j, &mut a, &mut c, &key()),
            Decision::NeedsReconciliation
        );
        c.resolution = Some(Resolution::Completed("job-1".into()));
        assert_eq!(
            reconcile_only(&mut j, &mut a, &mut c, &key()),
            Decision::Completed {
                reference: "job-1".into(),
                durable: true
            }
        );
        assert_eq!(c.submits, 1);
        assert_eq!(c.polls, 2);
        assert_eq!(a.held.get(), 0);
    }
    #[test]
    fn provider_proven_nonacceptance_is_terminal_not_reset_to_prepared() {
        let (mut j, mut a, mut c) = setup();
        j.record.phase = Phase::Unknown;
        c.resolution = Some(Resolution::ProvenNotAccepted);
        let result = Decision::Rejected {
            reason: Rejection::NotAccepted,
            durable: true,
        };
        assert_eq!(reconcile_only(&mut j, &mut a, &mut c, &key()), result);
        assert_eq!(submit_once(&mut j, &mut a, &mut c, &key()), result);
        assert_eq!(c.submits, 0);
    }
    #[test]
    fn invalid_references_or_corrupt_journals_cannot_forge_terminal_success() {
        let (mut j, mut a, mut c) = setup();
        c.submission = Some(Submission::Completed("raw response / secret".into()));
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::NeedsReconciliation
        );
        assert_eq!(j.record.phase, Phase::Unknown);
        j.record.phase = Phase::Completed;
        j.record.reference = None;
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::Invalid
        );
        assert_eq!(c.submits, 1);
    }
    #[test]
    fn changed_definition_event_or_payload_is_not_authority_over_an_existing_run() {
        let (mut j, mut a, mut c) = setup();
        let original = j.record.clone();
        for field in 0..3 {
            let mut changed = key();
            match field {
                0 => changed.definition = "c".repeat(64),
                1 => changed.event.push_str("-changed"),
                _ => changed.payload_digest = "c".repeat(64),
            }
            assert_eq!(
                submit_once(&mut j, &mut a, &mut c, &changed),
                Decision::Conflict
            );
            assert!(j.record == original);
        }
        assert_eq!(c.submits, 0);
        assert_eq!(j.writes, 0);
    }
    #[test]
    fn record_codec_is_bounded_strict_and_keeps_unknown_state_without_reset() {
        let (mut j, _, _) = setup();
        j.record.phase = Phase::Unknown;
        j.record.reference = Some("pending-1".into());
        let encoded = j.record.encode().unwrap();
        let restored = Record::decode(&encoded).unwrap();
        assert!(restored == j.record);
        for bytes in [
            Vec::new(),
            vec![b'x'; 2049],
            b"{}".to_vec(),
            b"null".to_vec(),
            [encoded.clone(), b" garbage".to_vec()].concat(),
        ] {
            assert!(Record::decode(&bytes).is_err());
        }
        let mut value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        value["extra"] = true.into();
        assert!(Record::decode(&serde_json::to_vec(&value).unwrap()).is_err());
        value.as_object_mut().unwrap().remove("extra");
        value["key"]["step"] = 8.into();
        assert!(Record::decode(&serde_json::to_vec(&value).unwrap()).is_err());
        assert!(restored == j.record);
        assert!(Key::new(
            "x/../../private",
            &"a".repeat(64),
            "event-1",
            0,
            "reviewed",
            "create",
            &"b".repeat(64)
        )
        .is_none());
    }
    #[test]
    fn reconciliation_cannot_race_an_existing_physical_and_logical_operation_permit() {
        let (mut j, mut a, mut c) = setup();
        j.record.phase = Phase::Unknown;
        c.resolution = Some(Resolution::Completed("job-1".into()));
        let AdmissionResult::Admitted(permit) = a.begin(&key(), AccessKind::Reconcile) else {
            panic!("synthetic admission must succeed");
        };
        assert_eq!(a.held.get(), 1);
        assert_eq!(
            reconcile_only(&mut j, &mut a, &mut c, &key()),
            Decision::Busy
        );
        assert_eq!(c.polls, 0);
        assert_eq!(j.writes, 0);
        drop(permit);
        assert_eq!(
            reconcile_only(&mut j, &mut a, &mut c, &key()),
            Decision::Completed {
                reference: "job-1".into(),
                durable: true
            }
        );
        assert_eq!(c.submits, 0);
        assert_eq!(c.polls, 1);
    }
    #[test]
    fn diagnostics_do_not_emit_provider_references_and_component_keys_are_not_paths() {
        let result = Decision::Completed {
            reference: "synthetic-private-reference".into(),
            durable: false,
        };
        let diagnostic = format!("{result:?}");
        assert!(!diagnostic.contains("synthetic-private-reference"));
        assert!(diagnostic.contains("durable: false"));
        for invalid in [
            ".",
            "..",
            "/private",
            "bad.name",
            "bad/other",
            "Uppercase",
            "a..b",
        ] {
            assert!(Key::new(
                invalid,
                &"a".repeat(64),
                "event-1",
                0,
                "reviewed",
                "create",
                &"b".repeat(64)
            )
            .is_none());
        }
    }
    #[test]
    fn logical_slots_are_bounded_and_changed_request_content_cannot_bypass_an_existing_event() {
        let pool = Arc::new(OperationRegistry::default());
        let original = key();
        let first = pool.try_begin(&original).ok().unwrap();
        let stale_slot = first.slot.clone();
        let stale_serial = first.serial;
        let mut changed = original.clone();
        changed.payload_digest = "c".repeat(64);
        changed.definition = "c".repeat(64);
        changed.connector = "other".into();
        assert!(matches!(pool.try_begin(&changed), Err(Decision::Busy)));
        drop(first);
        let second = pool.try_begin(&original).ok().unwrap();
        drop(OperationPermit {
            registry: pool.clone(),
            slot: stale_slot,
            serial: stale_serial,
        });
        assert!(matches!(pool.try_begin(&original), Err(Decision::Busy)));
        drop(second);
        let mut leases = Vec::new();
        for i in 0..32 {
            let mut next = original.clone();
            next.event = format!("bounded-{i}");
            leases.push(pool.try_begin(&next).ok().unwrap());
        }
        assert!(matches!(pool.try_begin(&original), Err(Decision::Busy)));
        assert_eq!(pool.state.lock().unwrap().active.len(), 32);
        drop(leases);
        let lease = pool.try_begin(&original).ok().unwrap();
        pool.state.lock().unwrap().serial = u64::MAX;
        changed.event = "new-after-exhaustion".into();
        assert!(matches!(
            pool.try_begin(&changed),
            Err(Decision::AdmissionUnavailable)
        ));
        drop(lease);
        assert!(pool.state.lock().unwrap().active.is_empty());
        assert!(matches!(
            pool.try_begin(&original),
            Err(Decision::AdmissionUnavailable)
        ));
    }
    #[test]
    fn logical_slot_concurrency_admits_one_native_owner_without_holding_a_mutex_across_work() {
        use std::sync::{atomic::AtomicUsize, Barrier};
        let pool = Arc::new(OperationRegistry::default());
        let count = Arc::new(AtomicUsize::new(0));
        let start = Arc::new(Barrier::new(17));
        let attempted = Arc::new(Barrier::new(17));
        let mut threads = Vec::new();
        for _ in 0..16 {
            let (pool, count, start, attempted, key) = (
                pool.clone(),
                count.clone(),
                start.clone(),
                attempted.clone(),
                key(),
            );
            threads.push(std::thread::spawn(move || {
                start.wait();
                let result = pool.try_begin(&key);
                let okay = match &result {
                    Ok(_) => {
                        count.fetch_add(1, Ordering::SeqCst);
                        true
                    }
                    Err(Decision::Busy) => true,
                    _ => false,
                };
                attempted.wait();
                drop(result);
                okay
            }));
        }
        start.wait();
        attempted.wait();
        for t in threads {
            assert!(t.join().unwrap());
        }
        assert_eq!(count.load(Ordering::SeqCst), 1);
        assert!(pool.state.lock().unwrap().active.is_empty());
    }
    #[test]
    fn unrelated_native_slots_do_not_fail_busy_only_because_bookkeeping_is_concurrent() {
        use std::sync::Barrier;
        let pool = Arc::new(OperationRegistry::default());
        let start = Arc::new(Barrier::new(17));
        let attempted = Arc::new(Barrier::new(17));
        let mut threads = Vec::new();
        for index in 0..16 {
            let (pool, start, attempted, base) =
                (pool.clone(), start.clone(), attempted.clone(), key());
            threads.push(std::thread::spawn(move || {
                let mut failures = 0;
                for round in 0..64 {
                    let mut key = base.clone();
                    key.event = format!("independent-{index}-{round}");
                    start.wait();
                    let result = pool.try_begin(&key);
                    if result.is_err() {
                        failures += 1;
                    }
                    attempted.wait();
                    drop(result);
                }
                failures
            }));
        }
        for _ in 0..64 {
            start.wait();
            attempted.wait();
        }
        let failures: usize = threads
            .into_iter()
            .map(|thread| thread.join().unwrap())
            .sum();
        assert_eq!(failures, 0);
        assert!(pool.state.lock().unwrap().active.is_empty());
    }
    #[test]
    fn known_unstarted_resource_busy_or_unavailable_is_not_mislabelled_cancelled() {
        for unavailable in [false, true] {
            let (mut j, mut a, mut c) = setup();
            a.busy = !unavailable;
            a.unavailable = unavailable;
            assert_eq!(
                submit_once(&mut j, &mut a, &mut c, &key()),
                if unavailable {
                    Decision::AdmissionUnavailable
                } else {
                    Decision::Busy
                }
            );
            assert_eq!(j.record.phase, Phase::AwaitingAdmission);
            assert_eq!(c.submits, 0);
            let restored = Record::decode(&j.record.encode().unwrap()).unwrap();
            assert!(restored == j.record);
            assert_eq!(
                reconcile_only(&mut j, &mut a, &mut c, &key()),
                Decision::NotSubmitted
            );
            assert_eq!(c.polls, 0);
            a.busy = false;
            a.unavailable = false;
            assert_eq!(
                submit_once(&mut j, &mut a, &mut c, &key()),
                Decision::Completed {
                    reference: "job-1".into(),
                    durable: true
                }
            );
            assert_eq!(c.submits, 1);
            assert_eq!(j.record.phase, Phase::Completed);
            assert_eq!(
                submit_once(&mut j, &mut a, &mut c, &key()),
                Decision::Completed {
                    reference: "job-1".into(),
                    durable: true
                }
            );
            assert_eq!(c.submits, 1);
        }
    }
    #[test]
    fn unknown_checkpoint_after_unstarted_admission_does_not_guess_permission_to_submit() {
        for fault in [
            Fault::NoWrite,
            Fault::UnknownBefore,
            Fault::UnknownAfter,
            Fault::Conflict,
        ] {
            let (mut j, mut a, mut c) = setup();
            a.busy = true;
            j.faults.extend([Fault::None, fault]);
            assert_eq!(
                submit_once(&mut j, &mut a, &mut c, &key()),
                Decision::NeedsReconciliation
            );
            assert_eq!(c.submits, 0);
            a.busy = false;
            let result = submit_once(&mut j, &mut a, &mut c, &key());
            if matches!(fault, Fault::UnknownAfter) {
                // A subsequent native load now proves the Waiting checkpoint
                // really exists. This is an explicit call, never an auto retry.
                assert_eq!(
                    result,
                    Decision::Completed {
                        reference: "job-1".into(),
                        durable: true
                    }
                );
                assert_eq!(c.submits, 1);
            } else {
                assert_eq!(result, Decision::NeedsReconciliation);
                assert_eq!(c.submits, 0);
                assert_eq!(j.record.phase, Phase::Claimed);
            }
        }
    }
    #[test]
    fn reconcile_admission_busy_never_rearms_an_unknown_submitted_request() {
        let (mut j, mut a, mut c) = setup();
        c.submission = Some(Submission::Unknown(Some("pending-1".into())));
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::NeedsReconciliation
        );
        let saved = j.record.clone();
        a.busy = true;
        assert_eq!(
            reconcile_only(&mut j, &mut a, &mut c, &key()),
            Decision::Busy
        );
        assert!(j.record == saved);
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::NeedsReconciliation
        );
        assert_eq!(c.submits, 1);
        assert_eq!(c.polls, 0);
        a.busy = false;
        c.resolution = Some(Resolution::Completed("job-1".into()));
        assert_eq!(
            reconcile_only(&mut j, &mut a, &mut c, &key()),
            Decision::Completed {
                reference: "job-1".into(),
                durable: true
            }
        );
        assert_eq!(c.submits, 1);
        assert_eq!(c.polls, 1);
    }
    #[test]
    fn read_only_reconciliation_authority_does_not_grant_submit_or_receipt_access_implicitly() {
        let (mut j, mut a, mut c) = setup();
        a.allow_submit = false;
        j.record.phase = Phase::Unknown;
        c.resolution = Some(Resolution::Completed("job-1".into()));
        assert_eq!(
            submit_once(&mut j, &mut a, &mut c, &key()),
            Decision::Denied
        );
        assert_eq!(j.writes, 0);
        assert_eq!(c.submits, 0);
        assert_eq!(
            reconcile_only(&mut j, &mut a, &mut c, &key()),
            Decision::Completed {
                reference: "job-1".into(),
                durable: true
            }
        );
        a.allow_reconcile = false;
        assert_eq!(
            reconcile_only(&mut j, &mut a, &mut c, &key()),
            Decision::Denied
        );
        assert_eq!(c.submits, 0);
        assert_eq!(c.polls, 1);
    }
    #[test]
    fn an_unavailable_logical_registry_never_resets_or_evicts_an_existing_slot() {
        let pool = Arc::new(OperationRegistry::default());
        let key = key();
        let held = pool.try_begin(&key).ok().unwrap();
        let poisoning = pool.clone();
        let result = std::panic::catch_unwind(move || {
            let _state = poisoning.state.lock().unwrap();
            panic!("synthetic logical registry fault");
        });
        assert!(result.is_err());
        assert!(matches!(
            pool.try_begin(&key),
            Err(Decision::AdmissionUnavailable)
        ));
        assert_eq!(
            pool.state.lock().err().unwrap().into_inner().active.len(),
            1
        );
        drop(held);
        assert!(matches!(
            pool.try_begin(&key),
            Err(Decision::AdmissionUnavailable)
        ));
    }
    #[test]
    fn submit_and_reconciliation_share_native_logical_exclusivity_before_journal_access() {
        let (mut j, mut a, mut c) = setup();
        let key = key();
        let held = operation_registry().try_begin(&key).ok().unwrap();
        j.unavailable = true;
        assert_eq!(submit_once(&mut j, &mut a, &mut c, &key), Decision::Busy);
        assert_eq!(reconcile_only(&mut j, &mut a, &mut c, &key), Decision::Busy);
        assert_eq!(j.writes, 0);
        assert_eq!(c.submits, 0);
        assert_eq!(c.polls, 0);
        drop(held);
        j.unavailable = false;
        j.record.phase = Phase::Unknown;
        c.resolution = Some(Resolution::Completed("job-1".into()));
        assert_eq!(
            reconcile_only(&mut j, &mut a, &mut c, &key),
            Decision::Completed {
                reference: "job-1".into(),
                durable: true
            }
        );
        assert_eq!(c.polls, 1);
        assert_eq!(c.submits, 0);
    }
}
