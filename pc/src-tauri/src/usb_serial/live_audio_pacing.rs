//! Hardware-safe live frames: bounded writes, no driver drain, RX stays active.
use std::io::{self, Write};
use std::time::{Duration, Instant};

const MAX_RECOVERY_RETRIES: usize = 3;
const RECOVERY_DEADLINE: Duration = Duration::from_millis(350);
const RETRY_BACKOFF: Duration = Duration::from_millis(2);

// Failed Windows overlapped completion can have unknown partial progress.
// Retain its OS code for diagnostics, but never retry it as a zero-byte timeout.
#[derive(Debug)]
struct UncertainWrite(io::Error);
impl std::fmt::Display for UncertainWrite {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { write!(f,"serial write completion uncertain") }
}
impl std::error::Error for UncertainWrite {}
#[cfg(any(windows, test))]
pub(super) fn uncertain_write_error(error: io::Error) -> io::Error {
    io::Error::new(io::ErrorKind::Other, UncertainWrite(error))
}

pub(super) fn io_kind(error: &io::Error) -> &'static str {
    if error.get_ref().is_some_and(|e| e.is::<UncertainWrite>()) { return "write_progress_unknown"; }
    match error.kind() {
        io::ErrorKind::TimedOut => "timeout",
        io::ErrorKind::WouldBlock => "would_block",
        io::ErrorKind::Interrupted => "interrupted",
        io::ErrorKind::BrokenPipe => "broken_pipe",
        io::ErrorKind::NotConnected => "not_connected",
        io::ErrorKind::WriteZero => "write_zero",
        io::ErrorKind::PermissionDenied => "permission_denied",
        io::ErrorKind::NotFound => "not_found",
        _ => "io_error",
    }
}

#[derive(Debug, Default)]
pub(super) struct LiveWriteReport {
    pub bytes_written: usize,
    pub retries: usize,
    pub error_kind: Option<&'static str>,
    pub os_error: Option<i32>,
}
impl LiveWriteReport {
    fn observe(&mut self, error: &io::Error) {
        self.error_kind=Some(io_kind(error));
        self.os_error=error.raw_os_error().or_else(|| error.get_ref()
            .and_then(|e|e.downcast_ref::<UncertainWrite>()).and_then(|e|e.0.raw_os_error()));
    }
}
#[derive(Debug)]
pub(super) struct LiveWriteFailure { pub report: LiveWriteReport, error: io::Error }
impl LiveWriteFailure {
    #[cfg(test)]
    fn kind(&self) -> io::ErrorKind { self.error.kind() }
}
impl std::fmt::Display for LiveWriteFailure {
    fn fmt(&self,f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        // Fixed classification only; do not persist driver strings or port paths.
        write!(f,"{} after {} bytes ({} retries)",io_kind(&self.error),self.report.bytes_written,self.report.retries)
    }
}

fn can_retry(error: &io::Error, retries: usize, elapsed: Duration) -> bool {
    matches!(error.kind(),io::ErrorKind::TimedOut|io::ErrorKind::WouldBlock|io::ErrorKind::Interrupted)
        && retries < MAX_RECOVERY_RETRIES && elapsed < RECOVERY_DEADLINE
        && !error.get_ref().is_some_and(|e|e.is::<UncertainWrite>())
}

pub(super) fn write_serial_live_frame(writer: &mut dyn Write, bytes: &[u8], slice_bytes: usize, gap: Duration) -> Result<LiveWriteReport,LiveWriteFailure> {
    let started=Instant::now();
    let mut report=LiveWriteReport::default();
    let chunks = bytes.chunks(slice_bytes.max(1));
    let count = chunks.len();
    for (index, chunk) in chunks.enumerate() {
        let mut offset=0;
        while offset < chunk.len() {
            // Bound the WHOLE frame recovery, not a fresh budget per slice.
            // Driver calls retain their own timeout; this is not cancellation.
            if report.retries > 0 && started.elapsed() >= RECOVERY_DEADLINE {
                let error=io::Error::new(io::ErrorKind::TimedOut,"live frame recovery deadline");
                report.observe(&error);
                return Err(LiveWriteFailure{report,error});
            }
            let error=match writer.write(&chunk[offset..]) {
                Ok(n) if n > 0 && n <= chunk.len()-offset => {
                    offset+=n; report.bytes_written+=n; continue;
                }
                Ok(0)=>io::Error::new(io::ErrorKind::WriteZero,"serial write made no progress"),
                Ok(_)=>io::Error::new(io::ErrorKind::InvalidData,"invalid serial write count"),
                Err(error)=>error,
            };
            report.observe(&error);
            if !can_retry(&error,report.retries,started.elapsed()) {
                return Err(LiveWriteFailure{report,error});
            }
            report.retries+=1;
            std::thread::sleep(RETRY_BACKOFF);
            // Resume ONLY the unwritten suffix on this same handle. Never
            // replay a JSON frame, drain the driver, or pause concurrent RX.
        }
        if index + 1 < count && !gap.is_zero() {
            // Windows Sleep may round a sub-ms gap to a scheduler tick.
            #[cfg(windows)]
            {
                let deadline = std::time::Instant::now() + gap;
                while std::time::Instant::now() < deadline { std::hint::spin_loop(); }
            }
            #[cfg(not(windows))]
            std::thread::sleep(gap);
        }
    }
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;
    struct Flaky {
        bytes: Vec<u8>,
        failures: VecDeque<(usize,io::ErrorKind)>,
        attempts: usize,
    }
    impl Write for Flaky {
        fn write(&mut self, data: &[u8]) -> io::Result<usize> {
            self.attempts+=1;
            if self.failures.front().is_some_and(|(offset,_)|self.bytes.len() >= *offset) {
                return Err(io::Error::new(self.failures.pop_front().unwrap().1,"synthetic error"));
            }
            let n=data.len().min(7);
            self.bytes.extend_from_slice(&data[..n]); Ok(n)
        }
        fn flush(&mut self) -> io::Result<()> { panic!("never drain live serial"); }
    }
    fn flaky(failures: Vec<(usize,io::ErrorKind)>) -> Flaky {
        Flaky{bytes:Vec::new(),failures:failures.into(),attempts:0}
    }
    #[test]
    fn transient_errors_resume_exact_suffix_even_mid_slice() {
        let bytes:Vec<_>=(0..200).map(|i|i as u8).collect();
        for kind in [io::ErrorKind::TimedOut,io::ErrorKind::WouldBlock,io::ErrorKind::Interrupted] {
            for offset in [0,7,63,64,129] {
                let mut writer=flaky(vec![(offset,kind)]);
                let report=write_serial_live_frame(&mut writer,&bytes,64,Duration::ZERO).unwrap();
                assert_eq!(writer.bytes,bytes,"must not duplicate a partially sent frame");
                assert_eq!(report.bytes_written,bytes.len()); assert_eq!(report.retries,1);
            }
        }
    }
    #[test]
    fn retries_are_bounded_across_the_frame_not_reset_by_progress() {
        let mut writer=flaky(vec![(7,io::ErrorKind::TimedOut),(70,io::ErrorKind::WouldBlock),
            (130,io::ErrorKind::Interrupted),(200,io::ErrorKind::TimedOut)]);
        let bytes:Vec<_>=(0..256).map(|i|i as u8).collect();
        let failure=write_serial_live_frame(&mut writer,&bytes,64,Duration::ZERO).unwrap_err();
        assert_eq!(failure.report.retries,3);
        assert_eq!(failure.report.bytes_written,writer.bytes.len());
        assert_eq!(writer.bytes,bytes[..writer.bytes.len()]);
        assert!(writer.bytes.len()<bytes.len());
    }
    #[test]
    fn no_progress_and_recovery_deadline_have_finite_bounds() {
        let mut writer=flaky(vec![(0,io::ErrorKind::Interrupted);100]);
        let failure=write_serial_live_frame(&mut writer,b"frame\n",64,Duration::ZERO).unwrap_err();
        assert_eq!(writer.attempts,4); assert_eq!(failure.report.bytes_written,0);
        let timeout=io::Error::new(io::ErrorKind::TimedOut,"timeout");
        assert!(can_retry(&timeout,2,RECOVERY_DEADLINE-Duration::from_millis(1)));
        assert!(!can_retry(&timeout,2,RECOVERY_DEADLINE));
        assert!(!can_retry(&timeout,3,Duration::ZERO));
    }
    #[test]
    fn permanent_and_uncertain_errors_never_retry() {
        for kind in [io::ErrorKind::BrokenPipe,io::ErrorKind::PermissionDenied,io::ErrorKind::NotConnected] {
            let mut writer=flaky(vec![(0,kind)]);
            let failure=write_serial_live_frame(&mut writer,b"frame\n",64,Duration::ZERO).unwrap_err();
            assert_eq!(writer.attempts,1); assert_eq!(failure.report.retries,0);
        }
        let error=uncertain_write_error(io::Error::from_raw_os_error(995));
        assert!(!can_retry(&error,0,Duration::ZERO));
        let mut report=LiveWriteReport::default(); report.observe(&error);
        assert_eq!(report.error_kind,Some("write_progress_unknown"));
        assert_eq!(report.os_error,Some(995));
        struct Unknown;
        impl Write for Unknown {
            fn write(&mut self,_:&[u8])->io::Result<usize>{Err(uncertain_write_error(io::Error::new(io::ErrorKind::TimedOut,"private driver text")))}
            fn flush(&mut self)->io::Result<()>{panic!("no drain")}
        }
        let failure=write_serial_live_frame(&mut Unknown,b"frame\n",64,Duration::ZERO).unwrap_err();
        assert_eq!(failure.report.retries,0);
        assert!(!failure.to_string().contains("private"));
        assert!(failure.to_string().contains("write_progress_unknown"));
    }
    #[test]
    fn zero_byte_write_stops_without_spinning() {
        let mut writer=capture(0);
        let failure=write_serial_live_frame(&mut writer,b"frame\n",64,Duration::ZERO).unwrap_err();
        assert_eq!(failure.kind(),io::ErrorKind::WriteZero);
        assert_eq!(writer.calls.len(),1);
    }
    // Explicit native performance regression: run separately from parallel CI
    // tests. Exercises the actual writer on a thread inherited as background,
    // without opening a serial port or playing audio on the user's device.
    #[cfg(target_os = "macos")]
    #[test]
    #[ignore = "native macOS scheduling benchmark; run explicitly with --nocapture"]
    fn macos_background_player_keeps_up_with_100ms_pcm() {
        std::thread::spawn(|| {
            assert_eq!(unsafe { libc::pthread_set_qos_class_self_np(libc::qos_class_t::QOS_CLASS_BACKGROUND, 0) }, 0);
            let measure = |frames: usize| {
                let started = std::time::Instant::now();
                for _ in 0..frames {
                    let mut writer = capture(usize::MAX);
                    write_serial_live_frame(&mut writer, &[1; 4400], 64, Duration::from_micros(250)).unwrap();
                    assert_eq!(writer.bytes.len(), 4400);
                    assert_eq!(writer.calls.len(), 69);
                }
                started.elapsed().as_secs_f64() * 1000.0 / frames as f64
            };
            let before = measure(5);
            let activity = crate::realtime_audio_macos::AudioActivity::begin_on_player_thread();
            assert_eq!(activity.qos_error, 0);
            let after = measure(100);
            eprintln!("macOS 100ms PCM frame pacing: background={before:.2}ms protected={after:.2}ms");
            assert!(after >= 17.0, "CH343 minimum inter-slice gaps must remain");
            assert!(after < 80.0, "audio must be delivered faster than playback, with headroom");
        }).join().unwrap();
    }
    struct Capture { bytes: Vec<u8>, calls: Vec<usize>, max_write: usize, fail_after: usize }
    impl Write for Capture {
        fn write(&mut self, data: &[u8]) -> io::Result<usize> {
            if self.bytes.len() >= self.fail_after { return Err(io::Error::new(io::ErrorKind::BrokenPipe, "test disconnect")); }
            self.calls.push(data.len());
            let count = data.len().min(self.max_write);
            self.bytes.extend_from_slice(&data[..count]);
            Ok(count)
        }
        fn flush(&mut self) -> io::Result<()> { panic!("live frames must never drain the driver"); }
    }
    fn capture(max_write: usize) -> Capture { Capture { bytes: vec![], calls: vec![], max_write, fail_after: usize::MAX } }
    #[test]
    fn preserves_frames_in_64_byte_bursts_without_flush() {
        let bytes: Vec<u8> = (0..4400).map(|n| (n % 251) as u8).collect();
        let mut writer = capture(usize::MAX);
        write_serial_live_frame(&mut writer, &bytes, 64, Duration::ZERO).unwrap();
        assert_eq!(writer.bytes, bytes);
        assert_eq!(writer.calls.len(), 69);
        assert!(writer.calls.iter().all(|n| *n <= 64));
    }
    #[test]
    fn partial_writes_are_completed_in_order() {
        let bytes: Vec<u8> = (0..4400).map(|n| (n % 251) as u8).collect();
        let mut writer = capture(7);
        write_serial_live_frame(&mut writer, &bytes, 64, Duration::ZERO).unwrap();
        assert_eq!(writer.bytes, bytes);
        assert!(writer.calls.iter().all(|n| *n <= 64));
    }
    #[test]
    fn write_failure_stops_the_frame() {
        let mut writer = capture(64);
        writer.fail_after = 128;
        let error = write_serial_live_frame(&mut writer, &[1; 4400], 64, Duration::ZERO).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::BrokenPipe);
        assert_eq!(writer.bytes.len(), 128);
    }
    #[test]
    fn empty_and_zero_slice_inputs_are_safe() {
        let mut writer = capture(64);
        write_serial_live_frame(&mut writer, &[], 64, Duration::ZERO).unwrap();
        assert!(writer.calls.is_empty());
        write_serial_live_frame(&mut writer, &[1, 2, 3], 0, Duration::ZERO).unwrap();
        assert_eq!(writer.calls, vec![1, 1, 1]);
        assert_eq!(writer.bytes, vec![1, 2, 3]);
    }
}
