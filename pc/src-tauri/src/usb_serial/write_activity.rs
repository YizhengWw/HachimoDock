/*
 * [Input] Explicit device-writing operation lifetimes.
 * [Output] Shared, scoped exit protection independent of USB transport contention.
 * [Pos] USB manager write activity; read-only requests retain the transport mutex.
 * [Sync] If this file changes, update `pc/.folder.md` and `pc/BACKGROUND.md`.
 */
use super::{Arc, AtomicUsize, Ordering, UsbSerialManager};

#[must_use]
pub(crate) struct WriteGuard {
    active: Arc<AtomicUsize>,
}

impl WriteGuard {
    pub(super) fn new(active: Arc<AtomicUsize>) -> Self {
        active.fetch_add(1, Ordering::AcqRel);
        Self { active }
    }
}

impl Drop for WriteGuard {
    fn drop(&mut self) {
        self.active.fetch_sub(1, Ordering::AcqRel);
    }
}

impl UsbSerialManager {
    // Keep the lease across all packets, ACK waits and final validation. A
    // transport lock or an individual write syscall cannot express that scope.
    pub(crate) fn protect_device_write(&self) -> WriteGuard {
        WriteGuard::new(self.protected_write_count.clone())
    }
}
