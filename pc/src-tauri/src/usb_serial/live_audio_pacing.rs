//! Hardware-safe live frames: bounded writes, no driver drain, RX stays active.
use std::io::{self, Write};
use std::time::Duration;

pub(super) fn write_serial_live_frame(writer: &mut dyn Write, bytes: &[u8], slice_bytes: usize, gap: Duration) -> io::Result<()> {
    let chunks = bytes.chunks(slice_bytes.max(1));
    let count = chunks.len();
    for (index, chunk) in chunks.enumerate() {
        writer.write_all(chunk)?;
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
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
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
