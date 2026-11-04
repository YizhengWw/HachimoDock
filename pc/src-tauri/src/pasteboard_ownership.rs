//! Ownership of temporary pasteboard contents. No native clipboard access here.
use std::sync::atomic::{AtomicBool, Ordering};

static INPUT_LEASED: AtomicBool = AtomicBool::new(false);

/// Never wait for the main thread while it waits for a previous clipboard user.
/// A second local input attempt fails before either attempt changes the board.
pub struct InputLease { _private: () }
impl InputLease {
    pub fn try_claim() -> Option<Self> {
        INPUT_LEASED.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .ok().map(|_| Self { _private: () })
    }
}
impl Drop for InputLease {
    fn drop(&mut self) { INPUT_LEASED.store(false, Ordering::Release); }
}

#[derive(Clone, Copy, Debug)]
pub struct ChangeOwner {
    expected: isize,
    written: bool,
}
impl ChangeOwner {
    pub fn captured(change: isize) -> Self { Self { expected: change, written: false } }
    pub fn may_write(self, current: isize) -> bool { self.expected == current }
    pub fn wrote(&mut self, change: isize) { self.expected = change; self.written = true; }
    pub fn owns_written(self, current: isize) -> bool { self.written && self.expected == current }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn user_copy_before_write_or_restore_is_preserved() {
        let mut owner = ChangeOwner::captured(41);
        assert!(owner.may_write(41));
        assert!(!owner.may_write(42));
        assert!(!owner.owns_written(41)); // an early failure must not restore at all
        owner.wrote(43);
        assert!(owner.owns_written(43));
        assert!(!owner.owns_written(44)); // another app copied during readback
        assert!(!owner.may_write(44));
        owner.wrote(45); // a second owned write tracks the latest change
        assert!(!owner.owns_written(43));
        assert!(owner.owns_written(45));
    }
    #[test]
    fn overlapping_input_fails_without_waiting_and_lease_releases_on_drop() {
        let lease = InputLease::try_claim().unwrap();
        assert!(InputLease::try_claim().is_none());
        drop(lease);
        assert!(InputLease::try_claim().is_some());
        assert!(InputLease::try_claim().is_some());
    }
}
