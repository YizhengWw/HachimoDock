//! Private append-only journal, deliberately not wired to an app/connector.
//! Same-slot compare/append is protected by a nonblocking OS file lock. No
//! rename/unlink/compaction means the lock inode cannot change underneath us.
//! Corrupt/torn/unknown tails are NEVER truncated or replaced with Prepared.
//!
//! Native parent supplies an existing private directory and authorizes explicit
//! creation. This is not a component-selected path, a grant, a credential store,
//! a network quota or a hardware/power-loss acceptance claim. Only Linux/macOS
//! barriers are implemented; other Unix targets fail closed. Storage hardware
//! must honor those barriers; no local API proves otherwise.
use super::{Commit, Journal, JournalError, Key, Phase, Record};
use sha2::{Digest, Sha256};
use std::ffi::{CStr, CString};
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::fd::{AsRawFd, FromRawFd, IntoRawFd};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::Path;
use std::sync::Mutex;

const MAGIC: &[u8; 8] = b"P4EJ01\0\0";
const HEADER: usize = 88;
const MAX_RECORD: usize = 2048;
const MAX_TRANSITIONS: u64 = 128;
const MAX_BYTES: u64 = (HEADER as u64 + MAX_RECORD as u64) * MAX_TRANSITIONS;
const MAX_SLOTS: usize = 128;

// The mutex protects callers sharing the SAME open file description, for which
// flock alone is reentrant. Separate processes/Directory opens use the OS lock.
// This is namespace allocation admission, never a UI/lifecycle/provider lock.
pub(crate) struct Directory(File, Mutex<()>);
pub(crate) struct FileJournal {
    directory: File,
    file: File,
    name: CString,
    key: Key,
    #[cfg(test)]
    fault: Option<Fault>,
    #[cfg(test)]
    fault_after: usize,
}
struct Locked<'a>(&'a File);
impl Drop for Locked<'_> {
    fn drop(&mut self) {
        // Releasing a lock never changes the recorded outcome or file bytes.
        unsafe { libc::flock(self.0.as_raw_fd(), libc::LOCK_UN) };
    }
}
fn lock(file: &File) -> Result<Locked<'_>, JournalError> {
    if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        return Err(JournalError::Unavailable);
    }
    Ok(Locked(file))
}
fn unavailable<T>(_: T) -> JournalError {
    JournalError::Unavailable
}
fn supported() -> bool {
    cfg!(any(target_os = "linux", target_os = "macos"))
}
fn private_directory(file: &File) -> Result<(), JournalError> {
    let m = file.metadata().map_err(unavailable)?;
    if !supported()
        || !m.is_dir()
        || m.uid() != unsafe { libc::geteuid() }
        || m.mode() & 0o777 != 0o700
    {
        return Err(JournalError::Invalid);
    }
    Ok(())
}
fn sync_file(file: &File) -> Result<(), JournalError> {
    file.sync_all().map_err(unavailable)?;
    // macOS fsync alone need not flush the drive's volatile write cache.
    #[cfg(target_os = "macos")]
    if unsafe { libc::fcntl(file.as_raw_fd(), libc::F_FULLFSYNC) } == -1 {
        return Err(JournalError::Unavailable);
    }
    Ok(())
}
fn durable(file: &File, directory: &File) -> Result<(), JournalError> {
    sync_file(file)?;
    sync_directory(file, directory)
}
fn sync_directory(file: &File, directory: &File) -> Result<(), JournalError> {
    directory.sync_all().map_err(unavailable)?;
    // The directory barrier can itself leave metadata in the drive's write
    // cache. A file-cache flush BEFORE that barrier cannot cover those writes.
    #[cfg(target_os = "macos")]
    if unsafe { libc::fcntl(file.as_raw_fd(), libc::F_FULLFSYNC) } == -1 {
        return Err(JournalError::Unavailable);
    }
    #[cfg(not(target_os = "macos"))]
    let _ = file;
    Ok(())
}
fn slot_name(key: &Key) -> Result<CString, JournalError> {
    if !key.valid() {
        return Err(JournalError::Invalid);
    }
    // Stable component/event/step only. Mutable request metadata is compared
    // inside the record; changing it cannot select a different empty file.
    let mut h = Sha256::new();
    h.update(b"p4-external-slot-v1\0");
    for field in [key.component(), key.event()] {
        h.update((field.len() as u32).to_le_bytes());
        h.update(field.as_bytes());
    }
    h.update([key.step()]);
    CString::new(format!("{:x}.journal", h.finalize())).map_err(|_| JournalError::Invalid)
}
impl Directory {
    /// No creation, recursive scan, permission repair or fallback location.
    /// The parent owns/trusts the path's ancestors. Final directory symlinks
    /// are refused; subsequent access uses this opened directory descriptor.
    pub(crate) fn open_existing(root: &Path) -> Result<Self, JournalError> {
        if !root.is_absolute() || !supported() {
            return Err(JournalError::Invalid);
        }
        let file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(root)
            .map_err(unavailable)?;
        private_directory(&file)?;
        Ok(Self(file, Mutex::new(())))
    }
    fn open(&self, key: &Key, create: bool) -> Result<FileJournal, JournalError> {
        private_directory(&self.0)?;
        let name = slot_name(key)?;
        let flags = libc::O_RDWR
            | libc::O_NOFOLLOW
            | libc::O_CLOEXEC
            | libc::O_NONBLOCK
            | if create {
                libc::O_CREAT | libc::O_EXCL
            } else {
                0
            };
        let fd = unsafe { libc::openat(self.0.as_raw_fd(), name.as_ptr(), flags, 0o600) };
        if fd == -1 {
            return Err(JournalError::Unavailable);
        }
        let file = unsafe { File::from_raw_fd(fd) };
        let journal = FileJournal {
            directory: self.0.try_clone().map_err(unavailable)?,
            file,
            name,
            key: key.clone(),
            #[cfg(test)]
            fault: None,
            #[cfg(test)]
            fault_after: 0,
        };
        journal.linked_private_file()?;
        Ok(journal)
    }
}
#[cfg(any(target_os = "linux", target_os = "macos"))]
fn creation_capacity(directory: &File) -> Result<(), JournalError> {
    // Enumerate only the already-open dedicated directory, never recurse or
    // read record bodies. Unknown entries are retained and stop NEW creation;
    // existing outcomes can still be loaded/reconciled independently.
    let fd = directory.try_clone().map_err(unavailable)?.into_raw_fd();
    let stream = unsafe { libc::fdopendir(fd) };
    if stream.is_null() {
        unsafe { libc::close(fd) };
        return Err(JournalError::Unavailable);
    }
    let result = (|| {
        unsafe { libc::rewinddir(stream) };
        let mut count = 0;
        loop {
            #[cfg(target_os = "macos")]
            let error = unsafe { libc::__error() };
            #[cfg(target_os = "linux")]
            let error = unsafe { libc::__errno_location() };
            // Normal EOF must not inherit errno from any unrelated syscall.
            unsafe { *error = 0 };
            let entry = unsafe { libc::readdir(stream) };
            if entry.is_null() {
                return if unsafe { *error } == 0 {
                    Ok(())
                } else {
                    Err(JournalError::Unavailable)
                };
            }
            let name = unsafe { CStr::from_ptr((*entry).d_name.as_ptr()) }.to_bytes();
            if name == b"." || name == b".." {
                continue;
            }
            if name.len() != 72
                || &name[64..] != b".journal"
                || !name[..64]
                    .iter()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(b))
            {
                return Err(JournalError::Invalid);
            }
            count += 1;
            if count >= MAX_SLOTS {
                return Err(JournalError::Unavailable);
            }
        }
    })();
    if unsafe { libc::closedir(stream) } != 0 {
        return Err(JournalError::Unavailable);
    }
    result
}
#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn creation_capacity(_: &File) -> Result<(), JournalError> {
    Err(JournalError::Unavailable)
}
fn transition(expected: &Record, next: &Record) -> bool {
    expected.valid()
        && next.valid()
        && expected.key == next.key
        && match expected.phase {
            Phase::Prepared | Phase::AwaitingAdmission => next.phase == Phase::Claimed,
            Phase::Claimed => matches!(
                next.phase,
                Phase::AwaitingAdmission
                    | Phase::Unknown
                    | Phase::Completed
                    | Phase::Rejected
                    | Phase::Cancelled
            ),
            Phase::Unknown => matches!(next.phase, Phase::Completed | Phase::Rejected),
            Phase::Completed | Phase::Rejected | Phase::Cancelled => false,
        }
}
struct Tail {
    record: Record,
    sequence: u64,
    digest: [u8; 32],
    bytes: u64,
}
fn header(sequence: u64, prior: &[u8; 32], payload: &[u8]) -> [u8; HEADER] {
    let mut out = [0; HEADER];
    out[..8].copy_from_slice(MAGIC);
    out[8..16].copy_from_slice(&sequence.to_le_bytes());
    out[16..20].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    out[24..56].copy_from_slice(prior);
    let mut h = Sha256::new();
    h.update(&out[..56]);
    h.update(payload);
    out[56..].copy_from_slice(&h.finalize());
    out
}
fn read_exact(file: &mut &File, bytes: &mut [u8]) -> Result<(), JournalError> {
    file.read_exact(bytes).map_err(|e| {
        if e.kind() == std::io::ErrorKind::UnexpectedEof {
            JournalError::Invalid
        } else {
            JournalError::Unavailable
        }
    })
}
impl FileJournal {
    /// Explicit fresh event only. EEXIST/partial prior creation is never reset.
    /// Failure can leave a protected unusable file; it is retained for review.
    pub(crate) fn create_new(directory: &Directory, key: &Key) -> Result<Self, JournalError> {
        if !key.valid() {
            return Err(JournalError::Invalid);
        }
        let _local = directory.1.try_lock().map_err(unavailable)?;
        let _namespace = lock(&directory.0)?;
        private_directory(&directory.0)?;
        creation_capacity(&directory.0)?;
        let journal = directory.open(key, true)?;
        {
            let _held = lock(&journal.file)?;
            let record = Record::prepared(key.clone()).ok_or(JournalError::Invalid)?;
            let payload = record.encode()?;
            let mut writer = &journal.file;
            writer
                .write_all(&header(1, &[0; 32], &payload))
                .map_err(unavailable)?;
            writer.write_all(&payload).map_err(unavailable)?;
            durable(&journal.file, &journal.directory)?;
            journal.linked_private_file()?;
        }
        Ok(journal)
    }
    /// Existing only. A missing/corrupt slot is not a new event.
    pub(crate) fn open_existing(directory: &Directory, key: &Key) -> Result<Self, JournalError> {
        let mut journal = directory.open(key, false)?;
        journal.load()?;
        Ok(journal)
    }
    fn linked_private_file(&self) -> Result<(), JournalError> {
        private_directory(&self.directory)?;
        let m = self.file.metadata().map_err(unavailable)?;
        if !m.is_file()
            || m.nlink() != 1
            || m.uid() != unsafe { libc::geteuid() }
            || m.mode() & 0o777 != 0o600
            || m.len() > MAX_BYTES
        {
            return Err(JournalError::Invalid);
        }
        let mut path = std::mem::MaybeUninit::<libc::stat>::uninit();
        if unsafe {
            libc::fstatat(
                self.directory.as_raw_fd(),
                self.name.as_ptr(),
                path.as_mut_ptr(),
                libc::AT_SYMLINK_NOFOLLOW,
            )
        } != 0
        {
            return Err(JournalError::Unavailable);
        }
        let path = unsafe { path.assume_init() };
        if path.st_dev as u64 != m.dev()
            || path.st_ino as u64 != m.ino()
            || path.st_mode & libc::S_IFMT != libc::S_IFREG
        {
            return Err(JournalError::Invalid);
        }
        Ok(())
    }
    fn read_locked(&self) -> Result<Tail, JournalError> {
        self.linked_private_file()?;
        let length = self.file.metadata().map_err(unavailable)?.len();
        if length < HEADER as u64 {
            return Err(JournalError::Invalid);
        }
        let mut reader = &self.file;
        reader.seek(SeekFrom::Start(0)).map_err(unavailable)?;
        let mut bytes = 0;
        let mut previous: Option<Tail> = None;
        while bytes < length {
            let sequence = previous.as_ref().map_or(1, |p| p.sequence + 1);
            if sequence > MAX_TRANSITIONS || length - bytes < HEADER as u64 {
                return Err(JournalError::Invalid);
            }
            let mut wire = [0; HEADER];
            read_exact(&mut reader, &mut wire)?;
            let size = u32::from_le_bytes(wire[16..20].try_into().unwrap()) as usize;
            if size == 0 || size > MAX_RECORD || length - bytes - (HEADER as u64) < size as u64 {
                return Err(JournalError::Invalid);
            }
            let mut payload = [0; MAX_RECORD];
            read_exact(&mut reader, &mut payload[..size])?;
            let prior = previous.as_ref().map_or([0; 32], |p| p.digest);
            if wire != header(sequence, &prior, &payload[..size]) {
                return Err(JournalError::Invalid);
            }
            let record = Record::decode(&payload[..size])?;
            if slot_name(&record.key)? != self.name
                || record.encode()? != payload[..size]
                || previous
                    .as_ref()
                    .map_or(record.phase != Phase::Prepared, |p| {
                        !transition(&p.record, &record)
                    })
            {
                return Err(JournalError::Invalid);
            }
            bytes += (HEADER + size) as u64;
            previous = Some(Tail {
                record,
                sequence,
                digest: wire[56..].try_into().unwrap(),
                bytes,
            });
        }
        self.linked_private_file()?;
        previous.ok_or(JournalError::Invalid)
    }
}
impl Journal for FileJournal {
    fn load(&mut self) -> Result<Record, JournalError> {
        let _held = lock(&self.file)?;
        let tail = self.read_locked()?;
        // A prior caller may have lost its commit acknowledgement. Establish
        // barriers before returning a record as durable after reopening.
        durable(&self.file, &self.directory)?;
        self.linked_private_file()?;
        Ok(tail.record)
    }
    fn compare_exchange(&mut self, expected: &Record, next: &Record) -> Commit {
        if expected.key != self.key || !transition(expected, next) {
            return Commit::Conflict;
        }
        let Ok(_held) = lock(&self.file) else {
            return Commit::NotCommitted;
        };
        let tail = match self.read_locked() {
            Ok(tail) => tail,
            Err(JournalError::Invalid) => return Commit::Conflict,
            Err(JournalError::Unavailable) => return Commit::NotCommitted,
        };
        if tail.record != *expected {
            return Commit::Conflict;
        }
        if tail.sequence >= MAX_TRANSITIONS {
            return Commit::NotCommitted;
        }
        let Ok(payload) = next.encode() else {
            return Commit::Conflict;
        };
        if durable(&self.file, &self.directory).is_err() {
            return Commit::NotCommitted;
        }
        let mut writer = &self.file;
        if writer.seek(SeekFrom::Start(tail.bytes)).is_err() {
            return Commit::NotCommitted;
        }
        let wire = header(tail.sequence + 1, &tail.digest, &payload);
        #[cfg(test)]
        let fault = if self.fault_after > 0 {
            self.fault_after -= 1;
            None
        } else {
            self.fault.take()
        };
        #[cfg(test)]
        if fault == Some(Fault::BeforeAppend) {
            return Commit::NotCommitted;
        }
        #[cfg(test)]
        if fault == Some(Fault::TornAppend) {
            let _ = writer.write_all(&wire[..HEADER / 2]);
            return Commit::Unknown;
        }
        // From the first write onward any failure is uncertain, never a reset
        // or evidence that a prior provider action did not happen.
        if writer
            .write_all(&wire)
            .and_then(|_| writer.write_all(&payload))
            .is_err()
        {
            return Commit::Unknown;
        }
        #[cfg(test)]
        if fault == Some(Fault::AfterAppend) {
            return Commit::Unknown;
        }
        if sync_file(&self.file).is_err() {
            return Commit::Unknown;
        }
        #[cfg(test)]
        if fault == Some(Fault::AfterFileSync) {
            return Commit::Unknown;
        }
        if sync_directory(&self.file, &self.directory).is_err()
            || self.linked_private_file().is_err()
        {
            return Commit::Unknown;
        }
        #[cfg(test)]
        if fault == Some(Fault::LostAcknowledgement) {
            return Commit::Unknown;
        }
        Commit::Committed
    }
}

#[cfg(test)]
#[derive(Clone, Copy, PartialEq, Eq)]
enum Fault {
    BeforeAppend,
    TornAppend,
    AfterAppend,
    AfterFileSync,
    LostAcknowledgement,
}

#[cfg(test)]
mod tests {
    use super::super::{
        reconcile_only, submit_once, AccessKind, Admission, AdmissionResult, Connector, Decision,
        Rejection, Resolution, Submission,
    };
    use super::*;
    use std::os::unix::fs::{symlink, PermissionsExt};
    use std::process::{Command, Stdio};

    fn key() -> Key {
        // Each test owns a distinct logical event, not just a distinct temp
        // directory. The production operation registry intentionally excludes
        // paths from identity. Hashing the test name also gives child processes
        // the SAME event as their parent in the cross-process fixtures.
        let thread = std::thread::current();
        let event = format!(
            "journal-{:x}",
            Sha256::digest(thread.name().unwrap_or("unnamed-synthetic").as_bytes())
        );
        Key::new(
            "synthetic",
            &"a".repeat(64),
            &event,
            0,
            "reviewed",
            "create",
            &"b".repeat(64),
        )
        .unwrap()
    }
    fn fixture() -> (tempfile::TempDir, Directory, FileJournal) {
        let tmp = tempfile::tempdir().unwrap();
        std::fs::set_permissions(tmp.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
        let dir = Directory::open_existing(tmp.path()).unwrap();
        let j = FileJournal::create_new(&dir, &key()).unwrap();
        (tmp, dir, j)
    }
    fn claimed(record: &Record) -> Record {
        let mut next = record.clone();
        next.phase = Phase::Claimed;
        next
    }
    fn path(tmp: &tempfile::TempDir) -> std::path::PathBuf {
        tmp.path()
            .join(slot_name(&key()).unwrap().to_str().unwrap())
    }
    struct Gate;
    impl Admission for Gate {
        type Permit = ();
        fn authorized(&self, _: &Key, _: AccessKind) -> bool {
            true
        }
        fn begin(&mut self, _: &Key, _: AccessKind) -> AdmissionResult<()> {
            AdmissionResult::Admitted(())
        }
    }
    #[derive(Default)]
    struct Provider {
        submits: usize,
        polls: usize,
        panic: bool,
    }
    impl Connector for Provider {
        fn binds(&self, _: &Key) -> bool {
            true
        }
        fn submit(&mut self, _: &Key) -> Submission {
            self.submits += 1;
            assert!(!self.panic, "synthetic provider crash");
            Submission::Completed("synthetic-receipt".into())
        }
        fn reconcile(&mut self, _: &Key, _: Option<&str>) -> Resolution {
            self.polls += 1;
            Resolution::Completed("synthetic-receipt".into())
        }
    }
    #[test]
    fn explicit_create_reopen_and_same_slot_fingerprint_conflict() {
        let (_tmp, dir, mut j) = fixture();
        let prepared = j.load().unwrap();
        assert_eq!(prepared.phase(), Phase::Prepared);
        assert!(FileJournal::create_new(&dir, &key()).is_err());
        let claim = claimed(&prepared);
        assert_eq!(j.compare_exchange(&prepared, &claim), Commit::Committed);
        drop(j);
        let mut reopened = FileJournal::open_existing(&dir, &key()).unwrap();
        assert!(reopened.load().unwrap() == claim);
        let mut changed = key();
        changed.definition = "c".repeat(64);
        changed.connector = "other".into();
        changed.payload_digest = "d".repeat(64);
        assert!(slot_name(&changed).unwrap() == slot_name(&key()).unwrap());
        let mut conflict = FileJournal::open_existing(&dir, &changed).unwrap();
        let mut provider = Provider::default();
        assert_eq!(
            submit_once(&mut conflict, &mut Gate, &mut provider, &changed),
            Decision::Conflict
        );
        assert_eq!(provider.submits, 0);
        assert!(reopened.load().unwrap() == claim);
    }
    #[test]
    fn native_submit_reopen_does_not_repeat_and_reconcile_is_read_only() {
        let (_tmp, dir, mut j) = fixture();
        let mut provider = Provider::default();
        assert!(matches!(
            submit_once(&mut j, &mut Gate, &mut provider, &key()),
            Decision::Completed { durable: true, .. }
        ));
        drop(j);
        let mut j = FileJournal::open_existing(&dir, &key()).unwrap();
        assert!(matches!(
            submit_once(&mut j, &mut Gate, &mut provider, &key()),
            Decision::Completed { durable: true, .. }
        ));
        assert!(matches!(
            reconcile_only(&mut j, &mut Gate, &mut provider, &key()),
            Decision::Completed { durable: true, .. }
        ));
        assert_eq!((provider.submits, provider.polls), (1, 0));
    }
    #[test]
    fn panic_after_durable_claim_never_rearms_on_reopen() {
        let (_tmp, dir, mut j) = fixture();
        let mut provider = Provider {
            panic: true,
            ..Default::default()
        };
        assert!(
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| submit_once(
                &mut j,
                &mut Gate,
                &mut provider,
                &key()
            )))
            .is_err()
        );
        drop(j);
        let mut j = FileJournal::open_existing(&dir, &key()).unwrap();
        assert_eq!(j.load().unwrap().phase(), Phase::Claimed);
        assert_eq!(
            submit_once(&mut j, &mut Gate, &mut provider, &key()),
            Decision::NeedsReconciliation
        );
        provider.panic = false;
        assert!(matches!(
            reconcile_only(&mut j, &mut Gate, &mut provider, &key()),
            Decision::Completed { durable: true, .. }
        ));
        assert_eq!((provider.submits, provider.polls), (1, 1));
    }
    #[test]
    fn uncertain_append_acknowledgement_never_calls_provider() {
        for fault in [
            Fault::BeforeAppend,
            Fault::TornAppend,
            Fault::AfterAppend,
            Fault::AfterFileSync,
            Fault::LostAcknowledgement,
        ] {
            let (tmp, dir, mut j) = fixture();
            j.fault = Some(fault);
            let mut provider = Provider::default();
            let decision = submit_once(&mut j, &mut Gate, &mut provider, &key());
            assert_eq!(provider.submits, 0);
            if fault == Fault::BeforeAppend {
                assert_eq!(decision, Decision::NotSubmitted);
                assert_eq!(j.load().unwrap().phase(), Phase::Prepared);
            } else {
                assert_eq!(decision, Decision::NeedsReconciliation);
                drop(j);
                if fault == Fault::TornAppend {
                    let before = std::fs::read(path(&tmp)).unwrap();
                    assert!(FileJournal::open_existing(&dir, &key()).is_err());
                    assert!(FileJournal::create_new(&dir, &key()).is_err());
                    assert_eq!(before, std::fs::read(path(&tmp)).unwrap());
                } else {
                    let mut j = FileJournal::open_existing(&dir, &key()).unwrap();
                    assert_eq!(j.load().unwrap().phase(), Phase::Claimed);
                    assert_eq!(
                        submit_once(&mut j, &mut Gate, &mut provider, &key()),
                        Decision::NeedsReconciliation
                    );
                    assert_eq!(provider.submits, 0);
                }
            }
        }
    }
    #[test]
    fn strict_chain_and_truncation_preserve_corrupt_evidence() {
        let (tmp, dir, mut j) = fixture();
        let p = j.load().unwrap();
        assert_eq!(j.compare_exchange(&p, &claimed(&p)), Commit::Committed);
        drop(j);
        let bytes = std::fs::read(path(&tmp)).unwrap();
        let mut invalid = vec![
            vec![],
            bytes[..HEADER / 2].to_vec(),
            bytes[..bytes.len() - 1].to_vec(),
        ];
        for index in [0, 8, 16, 20, 24, 56, HEADER, bytes.len() - 1] {
            let mut changed = bytes.clone();
            changed[index] ^= 1;
            invalid.push(changed);
        }
        let mut trailing = bytes.clone();
        trailing.push(0);
        invalid.push(trailing);
        for changed in invalid {
            std::fs::write(path(&tmp), &changed).unwrap();
            assert!(FileJournal::open_existing(&dir, &key()).is_err());
            assert!(FileJournal::create_new(&dir, &key()).is_err());
            assert_eq!(changed, std::fs::read(path(&tmp)).unwrap());
        }
    }

    #[test]
    fn proven_provider_result_survives_each_finalize_failure_without_replay() {
        for fault in [
            Fault::BeforeAppend,
            Fault::TornAppend,
            Fault::AfterAppend,
            Fault::AfterFileSync,
            Fault::LostAcknowledgement,
        ] {
            let (tmp, dir, mut j) = fixture();
            j.fault = Some(fault);
            j.fault_after = 1;
            let mut provider = Provider::default();
            let decision = submit_once(&mut j, &mut Gate, &mut provider, &key());
            assert!(matches!(
                decision,
                Decision::Completed { durable: false, .. }
            ));
            assert_eq!(provider.submits, 1);
            drop(j);
            let before = std::fs::read(path(&tmp)).unwrap();
            if fault == Fault::TornAppend {
                assert!(FileJournal::open_existing(&dir, &key()).is_err());
                assert!(FileJournal::create_new(&dir, &key()).is_err());
            } else {
                let mut j = FileJournal::open_existing(&dir, &key()).unwrap();
                let decision = submit_once(&mut j, &mut Gate, &mut provider, &key());
                if fault == Fault::BeforeAppend {
                    assert_eq!(decision, Decision::NeedsReconciliation);
                } else {
                    assert!(matches!(
                        decision,
                        Decision::Completed { durable: true, .. }
                    ));
                }
            }
            assert_eq!(provider.submits, 1);
            assert_eq!(before, std::fs::read(path(&tmp)).unwrap());
        }
    }

    #[test]
    fn process_exit_after_claim_preserves_unknown_action_without_resubmission() {
        const CHILD: &str = "PET_SYNTHETIC_JOURNAL_EXIT_CHILD";
        if let Some(root) = std::env::var_os(CHILD) {
            struct ExitProvider;
            impl Connector for ExitProvider {
                fn binds(&self, _: &Key) -> bool {
                    true
                }
                fn submit(&mut self, _: &Key) -> Submission {
                    std::process::exit(23)
                }
                fn reconcile(&mut self, _: &Key, _: Option<&str>) -> Resolution {
                    panic!("crash child cannot reconcile")
                }
            }
            let dir = Directory::open_existing(Path::new(&root)).unwrap();
            let mut j = FileJournal::open_existing(&dir, &key()).unwrap();
            submit_once(&mut j, &mut Gate, &mut ExitProvider, &key());
            panic!("synthetic process must exit in native provider");
        }
        let (tmp, dir, j) = fixture();
        drop(j);
        let output = Command::new(std::env::current_exe().unwrap())
            .args(["--exact", "component_external_outcome::file_journal::tests::process_exit_after_claim_preserves_unknown_action_without_resubmission", "--nocapture"])
            .env(CHILD, tmp.path()).output().unwrap();
        assert_eq!(output.status.code(), Some(23));
        let mut j = FileJournal::open_existing(&dir, &key()).unwrap();
        assert_eq!(j.load().unwrap().phase(), Phase::Claimed);
        let mut provider = Provider::default();
        assert_eq!(
            submit_once(&mut j, &mut Gate, &mut provider, &key()),
            Decision::NeedsReconciliation
        );
        assert_eq!(provider.submits, 0);
        assert!(matches!(
            reconcile_only(&mut j, &mut Gate, &mut provider, &key()),
            Decision::Completed { durable: true, .. }
        ));
        assert_eq!((provider.submits, provider.polls), (0, 1));
    }
    #[test]
    fn invalid_transitions_and_stale_compare_never_append() {
        let (tmp, _dir, mut j) = fixture();
        let p = j.load().unwrap();
        let c = claimed(&p);
        let before = std::fs::read(path(&tmp)).unwrap();
        let mut completed = p.clone();
        completed.phase = Phase::Completed;
        completed.reference = Some("receipt".into());
        assert_eq!(j.compare_exchange(&p, &completed), Commit::Conflict);
        assert_eq!(j.compare_exchange(&p, &p), Commit::Conflict);
        let mut changed = c.clone();
        changed.key.payload_digest = "c".repeat(64);
        assert_eq!(j.compare_exchange(&p, &changed), Commit::Conflict);
        assert_eq!(before, std::fs::read(path(&tmp)).unwrap());
        assert_eq!(j.compare_exchange(&p, &c), Commit::Committed);
        assert_eq!(j.compare_exchange(&p, &c), Commit::Conflict);
        assert_eq!(j.compare_exchange(&c, &p), Commit::Conflict);
        assert_eq!(j.compare_exchange(&c, &completed), Commit::Committed);
        assert_eq!(j.compare_exchange(&completed, &c), Commit::Conflict);
    }
    #[test]
    fn full_log_retains_outcome_without_compaction_or_reset() {
        let (tmp, dir, mut j) = fixture();
        let mut current = j.load().unwrap();
        for i in 1..MAX_TRANSITIONS {
            let mut next = current.clone();
            next.phase = if i % 2 == 1 {
                Phase::Claimed
            } else {
                Phase::AwaitingAdmission
            };
            assert_eq!(j.compare_exchange(&current, &next), Commit::Committed);
            current = next;
        }
        let before = std::fs::read(path(&tmp)).unwrap();
        let mut next = current.clone();
        next.phase = Phase::AwaitingAdmission;
        assert_eq!(j.compare_exchange(&current, &next), Commit::NotCommitted);
        assert!(j.load().unwrap() == current);
        assert!(FileJournal::create_new(&dir, &key()).is_err());
        assert_eq!(before, std::fs::read(path(&tmp)).unwrap());
        assert!(before.len() as u64 <= MAX_BYTES);
    }
    #[test]
    fn missing_symlink_hardlink_permissions_and_path_replacement_fail_closed() {
        let (tmp, dir, mut j) = fixture();
        let other = tempfile::tempdir().unwrap();
        let mut missing = key();
        missing.event = "missing".into();
        assert!(FileJournal::open_existing(&dir, &missing).is_err());
        let link = other.path().join("root-link");
        symlink(tmp.path(), &link).unwrap();
        assert!(Directory::open_existing(&link).is_err());
        assert!(Directory::open_existing(Path::new("relative")).is_err());
        let hardlink = other.path().join("linked");
        std::fs::hard_link(path(&tmp), &hardlink).unwrap();
        assert!(j.load().is_err());
        std::fs::remove_file(hardlink).unwrap();
        std::fs::set_permissions(path(&tmp), std::fs::Permissions::from_mode(0o644)).unwrap();
        assert!(j.load().is_err());
        std::fs::set_permissions(path(&tmp), std::fs::Permissions::from_mode(0o600)).unwrap();
        std::fs::set_permissions(tmp.path(), std::fs::Permissions::from_mode(0o755)).unwrap();
        assert!(j.load().is_err());
        std::fs::set_permissions(tmp.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
        assert!(j.load().is_ok());
        let kept = other.path().join("kept");
        std::fs::rename(path(&tmp), &kept).unwrap();
        symlink(&kept, path(&tmp)).unwrap();
        assert!(j.load().is_err());
        assert!(FileJournal::open_existing(&dir, &key()).is_err());
    }
    #[test]
    fn file_lock_contends_across_distinct_handles_without_mutation() {
        let (tmp, dir, mut j) = fixture();
        let p = j.load().unwrap();
        let mut other = FileJournal::open_existing(&dir, &key()).unwrap();
        let held = lock(&j.file).unwrap();
        let before = std::fs::read(path(&tmp)).unwrap();
        assert!(matches!(other.load(), Err(JournalError::Unavailable)));
        assert_eq!(
            other.compare_exchange(&p, &claimed(&p)),
            Commit::NotCommitted
        );
        assert_eq!(before, std::fs::read(path(&tmp)).unwrap());
        drop(held);
        assert_eq!(other.compare_exchange(&p, &claimed(&p)), Commit::Committed);
    }
    #[test]
    fn cross_process_submit_same_slot_only_one_native_provider_call() {
        const CHILD: &str = "PET_SYNTHETIC_JOURNAL_CHILD";
        if let Some(root) = std::env::var_os(CHILD) {
            let dir = Directory::open_existing(Path::new(&root)).unwrap();
            let mut signal = [0];
            std::io::stdin().read_exact(&mut signal).unwrap();
            let mut j = match FileJournal::open_existing(&dir, &key()) {
                Ok(j) => j,
                Err(JournalError::Unavailable) => {
                    println!("submitted=0");
                    return;
                }
                Err(_) => panic!("synthetic journal invalid"),
            };
            let mut provider = Provider::default();
            submit_once(&mut j, &mut Gate, &mut provider, &key());
            println!("submitted={}", provider.submits);
            return;
        }
        let (tmp, dir, j) = fixture();
        drop(j);
        let mut children = Vec::new();
        for _ in 0..8 {
            children.push(Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "component_external_outcome::file_journal::tests::cross_process_submit_same_slot_only_one_native_provider_call", "--nocapture"])
                .env(CHILD, tmp.path()).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped())
                .spawn().unwrap());
        }
        for child in &mut children {
            child.stdin.take().unwrap().write_all(&[1]).unwrap();
        }
        let mut submissions = 0;
        for child in children {
            let out = child.wait_with_output().unwrap();
            assert!(out.status.success());
            let text = String::from_utf8(out.stdout).unwrap();
            assert!(
                text.contains("1 passed"),
                "child must actually run the production-source test"
            );
            submissions += text.lines().filter(|s| *s == "submitted=1").count();
        }
        assert!(
            submissions <= 1,
            "cross-process CAS cannot grant two submissions"
        );
        let mut j = FileJournal::open_existing(&dir, &key()).unwrap();
        if submissions == 0 {
            // Nonblocking independent loads can each lose their following CAS
            // to another reader. That is legitimate contention, not permission
            // for production auto-retry. One separate explicit test invocation
            // may proceed ONLY if every child provably left Prepared.
            assert_eq!(j.load().unwrap().phase(), Phase::Prepared);
            let mut provider = Provider::default();
            assert!(matches!(
                submit_once(&mut j, &mut Gate, &mut provider, &key()),
                Decision::Completed { durable: true, .. }
            ));
            submissions += provider.submits;
        }
        assert_eq!(submissions, 1);
        assert_eq!(j.load().unwrap().phase(), Phase::Completed);
    }

    fn fill_interrupted_slots(dir: &Directory, count: usize) {
        for i in 0..count {
            let mut held = key();
            held.event = format!("interrupted-{i}");
            // Simulate a constructor interrupted before writing its first
            // header. These uncertain files count and must never be evicted.
            drop(dir.open(&held, true).unwrap());
        }
    }
    #[test]
    fn fixed_directory_quota_and_unknown_entries_retain_existing_outcomes() {
        let (tmp, dir, mut j) = fixture();
        fill_interrupted_slots(&dir, MAX_SLOTS - 1);
        let before = std::fs::read_dir(tmp.path()).unwrap().count();
        let mut fresh = key();
        fresh.event = "new-event".into();
        assert!(FileJournal::create_new(&dir, &fresh).is_err());
        assert_eq!(before, MAX_SLOTS);
        assert_eq!(std::fs::read_dir(tmp.path()).unwrap().count(), before);
        // Full directories do not prevent reconciliation of known records.
        let p = j.load().unwrap();
        assert_eq!(j.compare_exchange(&p, &claimed(&p)), Commit::Committed);
        assert_eq!(j.load().unwrap().phase(), Phase::Claimed);

        let (other, dir, mut j) = fixture();
        std::fs::write(other.path().join("unknown-entry"), b"retained").unwrap();
        assert!(matches!(
            FileJournal::create_new(&dir, &fresh),
            Err(JournalError::Invalid)
        ));
        assert_eq!(
            std::fs::read(other.path().join("unknown-entry")).unwrap(),
            b"retained"
        );
        assert_eq!(j.load().unwrap().phase(), Phase::Prepared);
    }
    #[test]
    fn same_directory_handle_creation_is_bounded_across_native_threads() {
        let tmp = tempfile::tempdir().unwrap();
        std::fs::set_permissions(tmp.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
        let dir = std::sync::Arc::new(Directory::open_existing(tmp.path()).unwrap());
        fill_interrupted_slots(&dir, MAX_SLOTS - 1);
        let start = std::sync::Arc::new(std::sync::Barrier::new(16));
        let mut handles = Vec::new();
        for i in 0..16 {
            let dir = dir.clone();
            let start = start.clone();
            handles.push(std::thread::spawn(move || {
                let mut key = key();
                key.event = format!("thread-new-{i}");
                start.wait();
                FileJournal::create_new(&dir, &key).is_ok() as usize
            }));
        }
        let created: usize = handles.into_iter().map(|t| t.join().unwrap()).sum();
        assert_eq!(created, 1);
        assert_eq!(std::fs::read_dir(tmp.path()).unwrap().count(), MAX_SLOTS);
    }
    #[test]
    fn cross_process_creation_cannot_overrun_the_last_available_slot() {
        const CHILD: &str = "PET_SYNTHETIC_JOURNAL_CREATE_CHILD";
        if let Some(root) = std::env::var_os(CHILD) {
            let dir = Directory::open_existing(Path::new(&root)).unwrap();
            let mut signal = [0];
            std::io::stdin().read_exact(&mut signal).unwrap();
            let mut key = key();
            key.event = format!("process-new-{}", signal[0]);
            println!(
                "created={}",
                FileJournal::create_new(&dir, &key).is_ok() as usize
            );
            return;
        }
        let tmp = tempfile::tempdir().unwrap();
        std::fs::set_permissions(tmp.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
        let dir = Directory::open_existing(tmp.path()).unwrap();
        fill_interrupted_slots(&dir, MAX_SLOTS - 1);
        let mut children = Vec::new();
        for _ in 0..8 {
            children.push(Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "component_external_outcome::file_journal::tests::cross_process_creation_cannot_overrun_the_last_available_slot", "--nocapture"])
                .env(CHILD, tmp.path()).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped())
                .spawn().unwrap());
        }
        for (i, child) in children.iter_mut().enumerate() {
            child.stdin.take().unwrap().write_all(&[i as u8]).unwrap();
        }
        let mut created = 0;
        for child in children {
            let out = child.wait_with_output().unwrap();
            assert!(out.status.success());
            let text = String::from_utf8(out.stdout).unwrap();
            assert!(text.contains("1 passed"));
            created += text.lines().filter(|s| *s == "created=1").count();
        }
        assert_eq!(created, 1);
        assert_eq!(std::fs::read_dir(tmp.path()).unwrap().count(), MAX_SLOTS);
    }
}
