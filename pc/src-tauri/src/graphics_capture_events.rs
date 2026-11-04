//! Nonblocking bounded screenshot event admission and fail-closed finalization.
//! Closing USB requests cancellation; only sender destruction proves callback exit.
use super::integrity::Capture;
use serde_json::{json, Value};
use std::{
    sync::{
        atomic::{AtomicU32, Ordering},
        mpsc,
    },
    time::{Duration, Instant},
};

pub(super) type Message = (String, Value);
pub(super) const QUEUE_CAPACITY: usize = 32;
pub(super) const CLOSE_BUDGET: Duration = Duration::from_secs(1);

pub(super) fn forward(
    tx: &mpsc::SyncSender<Message>,
    dropped: &AtomicU32,
    topic: String,
    payload: Value,
) {
    if matches!(
        topic.as_str(),
        "audio/status"
            | "debug/identity"
            | "debug/screenshot_begin"
            | "debug/screenshot_chunk"
            | "debug/screenshot_end"
            | "debug/screenshot_error"
            | "debug/screenshot_closed"
    ) && tx.try_send((topic, payload)).is_err()
    {
        // Never wrap back to zero and turn a failed acceptance into a pass.
        let _ = dropped.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
            Some(n.saturating_add(1))
        });
    }
}

/// Call only after requesting transport disconnect. A still-live sender at the
/// deadline is a failure, even if no drops have been observed yet. Late frame
/// events remain evidence; never recapture, replay or silently discard errors.
pub(super) fn finalize(
    capture: &mut Capture,
    rx: &mpsc::Receiver<Message>,
    dropped: &AtomicU32,
    budget: Duration,
) -> u32 {
    let started = Instant::now();
    let mut closed = false;
    while let Some(remaining) = budget
        .checked_sub(started.elapsed())
        .filter(|d| !d.is_zero())
    {
        match rx.recv_timeout(remaining) {
            Ok((topic, payload)) => capture.observe(&topic, payload),
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                closed = true;
                break;
            }
            Err(mpsc::RecvTimeoutError::Timeout) => break,
        }
    }
    if !closed {
        capture.error(json!({"kind":"event_stream_not_closed"}));
    }
    let count = dropped.load(Ordering::Relaxed);
    if count != 0 {
        capture.error(json!({"kind":"event_queue_drops","count":count}));
    }
    count
}

#[cfg(test)]
mod tests {
    use super::super::integrity::{chunk_checksum, frame_checksum};
    use super::*;
    use base64::{engine::general_purpose::STANDARD, Engine};

    fn complete() -> Capture {
        let raw = [1u8, 2, 3, 4];
        let mut capture = Capture::new(2, 1);
        capture.observe(
            "debug/screenshot_begin",
            json!({"id":"offline","width":2,"height":1,
            "format":"rgb565le","integrity":"chunk-fnv1a32-v1","totalBytes":4,"chunkBytes":4}),
        );
        capture.observe(
            "debug/screenshot_chunk",
            json!({"id":"offline","index":0,"rawSize":4,
            "checksum":format!("{:08x}",chunk_checksum(&raw)),"data":STANDARD.encode(raw)}),
        );
        capture.observe(
            "debug/screenshot_end",
            json!({"id":"offline","chunks":1,"totalBytes":4,
            "checksum":format!("{:08x}",frame_checksum(&raw))}),
        );
        assert_eq!(capture.report()["passed"], true);
        capture
    }
    fn error_kind(capture: &Capture, kind: &str) -> bool {
        capture.report()["errors"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["kind"] == kind)
    }
    #[test]
    fn dropped_final_drain_error_cannot_pass_with_an_otherwise_complete_frame() {
        let (tx, rx) = mpsc::sync_channel(QUEUE_CAPACITY);
        let dropped = AtomicU32::new(0);
        let mut capture = complete();
        for _ in 0..QUEUE_CAPACITY {
            forward(&tx, &dropped, "audio/status".into(), json!({}));
        }
        forward(
            &tx,
            &dropped,
            "debug/screenshot_error".into(),
            json!({"error":"drain failed"}),
        );
        drop(tx);
        assert_eq!(finalize(&mut capture, &rx, &dropped, CLOSE_BUDGET), 1);
        assert_eq!(capture.raw, [1, 2, 3, 4]);
        assert!(error_kind(&capture, "event_queue_drops"));
        assert_eq!(capture.report()["passed"], false);
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("queue-loss.json");
        let saved = capture.save_report(&path, json!({"eventQueueDrops":1,"hardware":false}));
        assert_eq!(saved["passed"], false);
        assert_eq!(
            serde_json::from_slice::<Value>(&std::fs::read(path).unwrap()).unwrap(),
            saved
        );
    }
    #[test]
    fn remaining_final_drain_error_is_observed_before_callback_exit() {
        let (tx, rx) = mpsc::sync_channel(QUEUE_CAPACITY);
        let dropped = AtomicU32::new(0);
        let mut capture = complete();
        forward(
            &tx,
            &dropped,
            "debug/screenshot_error".into(),
            json!({"error":"drain failed"}),
        );
        drop(tx);
        assert_eq!(finalize(&mut capture, &rx, &dropped, CLOSE_BUDGET), 0);
        assert!(error_kind(&capture, "device_error"));
        assert_eq!(capture.report()["passed"], false);
    }
    #[test]
    fn finalization_waits_for_an_inflight_callback_and_its_late_error() {
        let (tx, rx) = mpsc::sync_channel(QUEUE_CAPACITY);
        let dropped = std::sync::Arc::new(AtomicU32::new(0));
        let observed = dropped.clone();
        let callback = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(5));
            forward(
                &tx,
                &observed,
                "debug/screenshot_error".into(),
                json!({"error":"drain failed"}),
            );
        });
        let mut capture = complete();
        finalize(&mut capture, &rx, &dropped, CLOSE_BUDGET);
        callback.join().unwrap();
        assert!(error_kind(&capture, "device_error"));
        assert!(!error_kind(&capture, "event_stream_not_closed"));
        assert_eq!(capture.report()["passed"], false);
    }
    #[test]
    fn callback_that_has_not_exited_is_not_a_successful_end() {
        let (_tx, rx) = mpsc::sync_channel(QUEUE_CAPACITY);
        let dropped = AtomicU32::new(0);
        let mut capture = complete();
        finalize(&mut capture, &rx, &dropped, Duration::from_millis(1));
        assert!(error_kind(&capture, "event_stream_not_closed"));
        assert_eq!(capture.report()["passed"], false);
        assert_eq!(capture.raw, [1, 2, 3, 4]);
    }
    #[test]
    fn closed_lossless_channel_preserves_success_and_ignores_other_topics() {
        let (tx, rx) = mpsc::sync_channel(QUEUE_CAPACITY);
        let dropped = AtomicU32::new(0);
        let mut capture = complete();
        for _ in 0..100 {
            forward(&tx, &dropped, "unrelated".into(), json!({}));
        }
        forward(&tx, &dropped, "audio/status".into(), json!({}));
        drop(tx);
        assert_eq!(finalize(&mut capture, &rx, &dropped, CLOSE_BUDGET), 0);
        assert_eq!(capture.report()["passed"], true);
    }
    #[test]
    fn disconnected_admission_and_counter_saturation_cannot_erase_evidence() {
        let (tx, rx) = mpsc::sync_channel(QUEUE_CAPACITY);
        drop(rx);
        let dropped = AtomicU32::new(u32::MAX);
        forward(&tx, &dropped, "debug/screenshot_error".into(), json!({}));
        assert_eq!(dropped.load(Ordering::Relaxed), u32::MAX);
    }
}
