//! Explicit diagnostic TEST evidence only. Never linked into a release build.
//! Keep offsets and CONTROL checksums, not original payload keys/scalars. Even
//! diagnostics contain session IDs; an untrusted topic is no privacy boundary.
use super::control_integrity::{self, Failure, MAX_BODY};
use std::{fs::OpenOptions, io::Write, path::PathBuf, sync::{OnceLock, atomic::{AtomicUsize, Ordering}}, time::{SystemTime, UNIX_EPOCH}};

const HEADER: usize = 21;
const ENVELOPE: &[u8] = b"{\"topic\":\"diagnostics/status\",\"payload\":";
const MODE: &str = "diagnostics-structure-v1";
const MAX_FILES: usize = 4;

pub(super) fn enabled() -> bool {
    mode_enabled(std::env::var("P4_SERIAL_DIAGNOSTIC_CAPTURE").ok().as_deref())
}
fn mode_enabled(value: Option<&str>) -> bool {
    value == Some(MODE)
}

pub(super) struct Snapshot {
    redacted: Vec<u8>,
    expected: usize,
    actual: usize,
    expected_fnv: u32,
    actual_fnv: u32,
    failure: Failure,
    terminator_bytes: usize,
}

impl Snapshot {
    fn from_raw(raw: &[u8]) -> Option<Self> {
        // No partial EOF lines, overbound bodies, malformed headers, suffix
        // searches, audio/media, other topics, or successful frames are saved.
        if raw.len() > HEADER + MAX_BODY + 2 { return None; }
        let line = raw.strip_suffix(b"\r\n").or_else(|| raw.strip_suffix(b"\n"))?;
        let (_, observation) = control_integrity::decode(line);
        let failure = observation.failure?;
        if !matches!(failure, Failure::Length | Failure::Hash) { return None; }
        let expected = observation.expected.filter(|n| (1..=MAX_BODY).contains(n))?;
        let body = line.get(HEADER..)?;
        if !body.starts_with(ENVELOPE) { return None; }
        let expected_fnv = u32::from_str_radix(std::str::from_utf8(&line[12..20]).ok()?, 16).ok()?;
        let mut redacted = raw.to_vec();
        // Preserve only the validated header, fixed envelope and JSON shape.
        // Mask EVERY payload key/scalar byte (including numbers, whitespace in
        // strings, escapes, UTF-8 and malformed junk), without JSON repair.
        // Escape tracking only decides which quote is a delimiter; no decoded
        // strings or user-controlled field names enter this evidence.
        let mut quoted = false;
        let mut escaped = false;
        for byte in &mut redacted[HEADER + ENVELOPE.len()..line.len()] {
            let original = *byte;
            let keep = if quoted {
                let closing = original == b'"' && !escaped;
                if closing { quoted = false; }
                escaped = original == b'\\' && !escaped;
                closing
            } else if original == b'"' {
                quoted = true; escaped = false; true
            } else {
                matches!(original, b'{' | b'}' | b'[' | b']' | b',' | b':' | b' ' | b'\t')
            };
            if !keep { *byte = b'*'; }
        }
        Some(Self { redacted, expected, actual: observation.actual, expected_fnv,
            actual_fnv: control_integrity::fingerprint(body), failure,
            terminator_bytes: raw.len() - line.len() })
    }

    fn encode(&self, generation: u64, baud: u32, slot: usize) -> Vec<u8> {
        // All interpolated values are numbers, fixed labels, or hex generated
        // here. No driver text, IDs, paths, JSON content or real keys.
        let mut hex = String::with_capacity(self.redacted.len() * 2);
        const DIGITS: &[u8] = b"0123456789abcdef";
        for b in &self.redacted { hex.push(DIGITS[(b >> 4) as usize] as char); hex.push(DIGITS[(b & 15) as usize] as char); }
        let timestamp_ms = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis();
        format!(concat!("{{\"schema\":1,\"mode\":\"diagnostics-structure-v1\",",
            "\"originalPayloadRetained\":false,\"byteOffsetsPreserved\":true,",
            "\"timestampMs\":{},\"generation\":{},\"baud\":{},\"slot\":{},\"kind\":\"{}\",",
            "\"expectedBodyBytes\":{},\"actualBodyBytes\":{},\"physicalLineBytes\":{},",
            "\"terminatorBytes\":{},\"expectedFnv1a32\":\"{:08x}\",\"actualFnv1a32\":\"{:08x}\",",
            "\"rawRedactedHex\":\"{}\"}}\n"),
            timestamp_ms, generation, baud, slot, self.failure.label(), self.expected, self.actual,
            self.redacted.len(), self.terminator_bytes, self.expected_fnv, self.actual_fnv, hex).into_bytes()
    }
}

pub(super) fn capture_candidate(raw: &[u8]) -> Option<Snapshot> {
    if !enabled() { return None; }
    Snapshot::from_raw(raw)
}

struct Sink { directory: PathBuf, attempts: AtomicUsize }
impl Sink {
    fn save(&self, snapshot: &Snapshot, generation: u64, baud: u32) -> &'static str {
        // Process-wide bound includes collisions and failed writes. Reconnects
        // cannot reset the allowance. Existing evidence is never overwritten.
        let Ok(slot) = self.attempts.fetch_update(Ordering::Relaxed, Ordering::Relaxed,
            |count| (count < MAX_FILES).then_some(count + 1)) else { return "exhausted"; };
        let path = self.directory.join(format!("rawbadframe-{}-{}.json", std::process::id(), slot + 1));
        let mut options = OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)] {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let written = (|| -> std::io::Result<()> {
            let mut file = options.open(path)?;
            file.write_all(&snapshot.encode(generation, baud, slot + 1))?;
            file.sync_all()
        })();
        if written.is_ok() { "saved" } else { "io_failed" }
    }
}

pub(super) fn save(snapshot: &Snapshot, generation: u64, baud: u32) -> &'static str {
    if !enabled() { return "disabled"; }
    static SINK: OnceLock<Option<Sink>> = OnceLock::new();
    let sink = SINK.get_or_init(|| {
        let directory = PathBuf::from(std::env::var_os("P4_SERIAL_DIAGNOSTIC_CAPTURE_DIR")?);
        // A reviewed runner supplies a fresh, absolute local directory. Do not
        // create a directory or fall back to any real app/profile location.
        (directory.is_absolute() && directory.is_dir()).then_some(Sink { directory, attempts: AtomicUsize::new(0) })
    });
    sink.as_ref().map_or("invalid_directory", |sink| sink.save(snapshot, generation, baud))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn frame(body: &[u8]) -> Vec<u8> {
        let mut raw = format!("!P4C1:{:05}:{:08x}:", body.len(), control_integrity::fingerprint(body)).into_bytes();
        raw.extend(body); raw.extend(b"\r\n"); raw
    }
    fn damaged() -> Vec<u8> {
        let mut raw = frame(r#"{"topic":"diagnostics/status","payload":{"session":"synthetic-private","number":12345,"escaped":"x\"y\\z","utf8":"中文","junk":false}}"#.as_bytes());
        raw[12] = if raw[12] == b'0' { b'1' } else { b'0' }; raw
    }
    #[test]
    fn opt_in_is_exact_and_never_accepts_the_generic_raw_debug_switch() {
        assert!(mode_enabled(Some(MODE)));
        for value in [None,Some(""),Some("1"),Some("true"),Some("diagnostics-structure-v1 "),Some("raw")] {
            assert!(!mode_enabled(value));
        }
    }
    #[test]
    fn only_failed_bounded_diagnostic_envelopes_are_candidates() {
        assert!(Snapshot::from_raw(&damaged()).is_some());
        let good = frame(br#"{"topic":"diagnostics/status","payload":{}}"#);
        assert!(Snapshot::from_raw(&good).is_none());
        for topic in ["audio/frame", "media/event", "notes/transcript", "diagnostics/status-extra"] {
            let mut raw = frame(format!("{{\"topic\":\"{topic}\",\"payload\":{{}}}}").as_bytes()); raw[12] = b'z';
            assert!(Snapshot::from_raw(&raw).is_none());
            raw[12] = b'0'; assert!(Snapshot::from_raw(&raw).is_none());
        }
        for raw in [b"!P4C1:".to_vec(), damaged()[..damaged().len()-2].to_vec(),
            [b"noise".as_slice(), &damaged()].concat(), vec![b'x'; HEADER + MAX_BODY + 3]] {
            assert!(Snapshot::from_raw(&raw).is_none());
        }
    }
    #[test]
    fn offsets_header_and_hash_survive_but_all_payload_scalars_are_masked() {
        let raw = damaged(); let s = Snapshot::from_raw(&raw).unwrap();
        assert_eq!(s.redacted.len(), raw.len());
        assert_eq!(&s.redacted[..HEADER+ENVELOPE.len()], &raw[..HEADER+ENVELOPE.len()]);
        assert_eq!(&s.redacted[raw.len()-2..], b"\r\n");
        let text = String::from_utf8(s.redacted.clone()).unwrap();
        for secret in ["session", "synthetic-private", "12345", "escaped", "utf8", "中文", "false", "junk"] { assert!(!text.contains(secret)); }
        assert_eq!(s.actual_fnv, control_integrity::fingerprint(&raw[HEADER..raw.len()-2]));
        assert_eq!(s.terminator_bytes, 2);
        let encoded = String::from_utf8(s.encode(7,4000000,1)).unwrap();
        assert!(encoded.contains("\"originalPayloadRetained\":false"));
        assert!(encoded.len() < (HEADER+MAX_BODY+2)*2+1024);
    }
    #[test]
    fn missing_128_bytes_and_unparseable_body_remain_failed_not_repaired() {
        let mut body = ENVELOPE.to_vec();body.extend(b"{\"padding\":\"");body.extend(vec![b'x';5889-body.len()-3]);body.extend(b"\"}}");
        assert_eq!(body.len(),5889);
        let mut raw=frame(&body);raw.drain(HEADER+100..HEADER+228);
        let s=Snapshot::from_raw(&raw).unwrap();
        assert_eq!((s.expected,s.actual),(5889,5761));assert_eq!(s.failure,Failure::Length);
        assert_eq!(control_integrity::decode(&raw[..raw.len()-2]).1.failure,Some(Failure::Length));
        // Invalid UTF-8/JSON within the payload cannot bypass the mask.
        let mut malformed=frame(&[ENVELOPE,b"{\"broken\":\"secret\\\"\xff"].concat());malformed.remove(HEADER+ENVELOPE.len());
        let s=Snapshot::from_raw(&malformed).unwrap();
        assert!(!s.redacted.contains(&0xff));assert!(!String::from_utf8_lossy(&s.redacted).contains("secret"));
    }
    #[test]
    fn sink_is_bounded_private_and_never_overwrites_evidence() {
        let directory=std::env::temp_dir().join(format!("p4-capture-pure-{}",std::process::id()));
        std::fs::create_dir(&directory).unwrap();
        let sink=Sink {directory:directory.clone(),attempts:AtomicUsize::new(0)};
        let first=directory.join(format!("rawbadframe-{}-1.json",std::process::id()));
        std::fs::write(&first,b"prior-evidence").unwrap();
        let s=Snapshot::from_raw(&damaged()).unwrap();assert_eq!(sink.save(&s,1,4000000),"io_failed");
        for _ in 1..MAX_FILES {assert_eq!(sink.save(&s,1,4000000),"saved");}
        for _ in 0..10 {assert_eq!(sink.save(&s,2,4000000),"exhausted");}
        assert_eq!(std::fs::read(&first).unwrap(),b"prior-evidence");
        assert_eq!(std::fs::read_dir(&directory).unwrap().count(),MAX_FILES);
        #[cfg(unix)] {
            use std::os::unix::fs::PermissionsExt;
            let mode=std::fs::metadata(directory.join(format!("rawbadframe-{}-2.json",std::process::id()))).unwrap().permissions().mode();
            assert_eq!(mode & 0o077,0);
        }
        std::fs::remove_dir_all(directory).unwrap();
    }
}
