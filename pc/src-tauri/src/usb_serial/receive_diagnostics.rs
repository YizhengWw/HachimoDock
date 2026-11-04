//! Bounded metadata for UNPARSED physical lines, not a parser or recovery path.
//! Never retains content, IDs, paths, keys, hashes, PCM or a reparsed suffix.
//! SDK text/boot logs can be unparsed too; these counters do not prove corruption.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Kind { Nul, Replacement, Json, Text }
impl Kind {
    pub(super) fn label(self) -> &'static str {
        match self { Self::Nul => "nul", Self::Replacement => "lossy_text", Self::Json => "invalid_json", Self::Text => "other_text" }
    }
    fn index(self) -> usize { self as usize }
}
// Fixed categories only: a firmware text hint is not authenticated reset proof.
// A rare fault must not disappear behind the ordinary text power-of-two limit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum FaultKind { TaskWatchdog, InterruptWatchdog, Panic, StackGuard, BootReset, Abort }
impl FaultKind {
    pub(super) fn label(self)-> &'static str {
        match self { Self::TaskWatchdog=>"task_watchdog",Self::InterruptWatchdog=>"interrupt_watchdog",
            Self::Panic=>"panic",Self::StackGuard=>"stack_guard",Self::BootReset=>"boot_reset",Self::Abort=>"abort" }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct FaultHint {pub(super) kind:FaultKind,pub(super) occurrences:u64}
// Only a complete native SDK abort line is accepted. No assertion text,
// source path, stack memory or payload is retained. This is an unauthenticated
// hint; callers must separately establish boot/build identity before decoding.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct AbortSite {pub(super) pc:u32,pub(super) core:u8}
fn sdk_text(line:&str)->Option<&str>{
    if line.len()>1024||line.contains(['\0','\u{fffd}']){return None;}
    let mut text=line.trim_matches(['\r','\n',' ']);
    if let Some(color)=text.strip_prefix("\x1b["){
        let end=color.find('m')?;
        if end>12||!color[..end].bytes().all(|c|c.is_ascii_digit()||c==b';'){return None;}
        text=&color[end+1..];
    }
    Some(text.strip_suffix("\x1b[0m").unwrap_or(text))
}
fn abort_site(line:&str)->Option<AbortSite>{
    let text=sdk_text(line)?.strip_prefix("abort() was called at PC 0x")?;
    let (pc,core)=text.split_once(" on core ")?;
    if pc.len()!=8||!pc.bytes().all(|c|c.is_ascii_hexdigit()){return None;}
    let core=match core{"0"=>0,"1"=>1,_=>return None};
    Some(AbortSite{pc:u32::from_str_radix(pc,16).ok()?,core})
}
fn fault_kind(line:&str)->Option<FaultKind>{
    // Refuse arbitrary JSON/substrings, clipped long text and damaged encoding.
    let text=sdk_text(line)?;
    if abort_site(line).is_some(){return Some(FaultKind::Abort);}
    if text.starts_with("Guru Meditation Error: Core "){
        return Some(if text.contains("Interrupt wdt timeout on CPU"){FaultKind::InterruptWatchdog}else{FaultKind::Panic});
    }
    if text.starts_with("Stack protection fault")||text.starts_with("Stack smashing protect failure!"){return Some(FaultKind::StackGuard);}
    if let Some(reason)=text.strip_prefix("rst:0x"){
        let field=reason.split_once(',')?.0;
        let code=if let Some((code,name))=field.split_once(" ("){
            if !name.ends_with(')'){return None;}code
        }else{field};
        return (!code.is_empty()&&code.len()<=8&&code.bytes().all(|c|c.is_ascii_hexdigit())).then_some(FaultKind::BootReset);
    }
    // Exact IDF prefix, bounded numeric timestamp, then an exact trusted tag.
    let body=text.strip_prefix("E (")?;let (stamp,body)=body.split_once(") ")?;
    if stamp.is_empty()||stamp.len()>20||!stamp.bytes().all(|c|c.is_ascii_digit()){return None;}
    body.strip_prefix("task_wdt: Task watchdog got triggered.").map(|_|FaultKind::TaskWatchdog)
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Report {
    pub(super) kind: Kind,
    pub(super) occurrences: u64,
    pub(super) total_lines: u64,
    pub(super) decoded_bytes: u64,
    pub(super) nul_chars: u64,
    pub(super) replacement_chars: u64,
    pub(super) fault_hint: Option<FaultHint>,
    pub(super) abort_site: Option<AbortSite>,
}
#[derive(Default)]
pub(super) struct Rejections {
    kinds: [u64; 4],
    faults: [u64; 6],
    total_lines: u64,
    decoded_bytes: u64,
    nul_chars: u64,
    replacement_chars: u64,
}
impl Rejections {
    pub(super) fn observe(&mut self, line: &str) -> Option<Report> {
        let nul = line.bytes().filter(|b| *b == 0).count() as u64;
        let replacement = line.chars().filter(|c| *c == '\u{fffd}').count() as u64;
        let kind = if nul != 0 { Kind::Nul } else if replacement != 0 { Kind::Replacement }
            else if line.trim_start().starts_with('{') { Kind::Json } else { Kind::Text };
        self.total_lines = self.total_lines.saturating_add(1);
        self.decoded_bytes = self.decoded_bytes.saturating_add(line.len() as u64);
        self.nul_chars = self.nul_chars.saturating_add(nul);
        self.replacement_chars = self.replacement_chars.saturating_add(replacement);
        let occurrences = self.kinds[kind.index()].saturating_add(1);
        self.kinds[kind.index()] = occurrences;
        let fault_hint=fault_kind(line).and_then(|kind|{
            let count=&mut self.faults[kind as usize];*count=count.saturating_add(1);
            count.is_power_of_two().then_some(FaultHint{kind,occurrences:*count})
        });
        // First observation of EACH class is retained, then only powers of two.
        // <= 64 reports/class per connection, even under indefinitely bad input.
        let abort_site=fault_hint.filter(|h|h.kind==FaultKind::Abort).and_then(|_|abort_site(line));
        (occurrences.is_power_of_two()||fault_hint.is_some()).then_some(Report { kind, occurrences,
            total_lines: self.total_lines, decoded_bytes: self.decoded_bytes,
            nul_chars: self.nul_chars, replacement_chars: self.replacement_chars,fault_hint,abort_site })
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn each_class_is_counted_without_content_or_false_corruption_claims() {
        let mut state = Rejections::default();
        for (line, kind) in [("SDK boot message /private/example", Kind::Text),
            ("\0\0{not recovered}", Kind::Nul), ("bad\u{fffd}stream", Kind::Replacement),
            (" {\"key\":secret}", Kind::Json)] {
            let report = state.observe(line).unwrap();
            assert_eq!(report.kind, kind); assert_eq!(report.occurrences, 1);
            assert!(!format!("{report:?}").contains(line));
            assert!(!format!("{report:?}").contains("private/example"));
        }
        assert_eq!(state.total_lines, 4); assert_eq!(state.nul_chars, 2);
        assert_eq!(state.replacement_chars, 1);
    }
    #[test]
    fn persistent_noise_has_logarithmically_bounded_reports() {
        let mut state = Rejections::default(); let mut emitted = 0;
        for count in 1u64..=65536 {
            let report = state.observe("unparsed");
            assert_eq!(report.is_some(), count.is_power_of_two());
            emitted += usize::from(report.is_some());
        }
        assert_eq!(emitted, 17); assert_eq!(state.total_lines, 65536);
        // A rare new class is NOT hidden behind the text rate limit.
        assert_eq!(state.observe("\0").unwrap().kind, Kind::Nul);
    }
    #[test]
    fn saturation_cannot_wrap_reset_rate_limits_or_change_classes() {
        let mut state = Rejections { kinds: [u64::MAX;4], faults:[u64::MAX;6],total_lines: u64::MAX,
            decoded_bytes: u64::MAX, nul_chars: u64::MAX, replacement_chars: u64::MAX };
        for _ in 0..100 { assert!(state.observe("\0\u{fffd}").is_none()); }
        assert_eq!(state.kinds, [u64::MAX;4]); assert_eq!(state.total_lines, u64::MAX);
        assert_eq!(state.decoded_bytes, u64::MAX); assert_eq!(state.nul_chars, u64::MAX);
    }
    #[test]
    fn rare_watchdog_hint_survives_text_rate_limit_without_retaining_text(){
        let mut state=Rejections::default();for _ in 0..20{state.observe("ordinary firmware text");}
        let secret="E (177) task_wdt: Task watchdog got triggered. private-user-and-secret";
        let report=state.observe(secret).unwrap();
        assert_eq!(report.fault_hint,Some(FaultHint{kind:FaultKind::TaskWatchdog,occurrences:1}));
        assert!(!format!("{report:?}").contains("private-user-and-secret"));
        for n in 2u64..=65536{
            let hint=state.observe(secret).and_then(|r|r.fault_hint);
            assert_eq!(hint.is_some(),n.is_power_of_two());
        }
    }
    #[test]
    fn fault_hints_require_exact_bounded_prefix_and_never_reparse_payloads(){
        for (line,kind) in [
            ("\x1b[0;31mE (0) task_wdt: Task watchdog got triggered.\x1b[0m",FaultKind::TaskWatchdog),
            ("Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0).",FaultKind::InterruptWatchdog),
            ("Guru Meditation Error: Core 0 panic'ed (Load access fault).",FaultKind::Panic),
            ("Stack protection fault",FaultKind::StackGuard),("rst:0x7 (WDT),boot:0xf",FaultKind::BootReset)]{
            assert_eq!(fault_kind(line),Some(kind));
        }
        for line in ["task_wdt: Task watchdog got triggered.","hello Guru Meditation Error: Core 0",
            "{\"text\":\"E (1) task_wdt: Task watchdog got triggered.\"}",
            "E (secret) task_wdt: Task watchdog got triggered.","rst:0xsecret,boot:0xf",
            "\0E (1) task_wdt: Task watchdog got triggered.","E (1) task_wdt: Task\u{fffd} watchdog got triggered."]{
            assert_eq!(fault_kind(line),None);
        }
        assert_eq!(fault_kind(&format!("E (1) task_wdt: Task watchdog got triggered.{}","x".repeat(1024))),None);
        let mut state=Rejections{faults:[u64::MAX;6],..Default::default()};
        assert!(state.observe("E (1) task_wdt: Task watchdog got triggered.").unwrap().fault_hint.is_none());
    }
    #[test]
    fn native_abort_site_is_exact_bounded_numeric_and_not_text(){
        let expected=Some(AbortSite{pc:0x400abcd0,core:1});
        for line in ["abort() was called at PC 0x400abcd0 on core 1",
            "\r\n\x1b[0;31mabort() was called at PC 0x400abcd0 on core 1\x1b[0m\r\n"]{
            assert_eq!(abort_site(line),expected);
            assert_eq!(fault_kind(line),Some(FaultKind::Abort));
        }
        for line in ["prefix abort() was called at PC 0x400abcd0 on core 1",
            "abort() was called at PC 0x400abcd0 on core 2",
            "abort() was called at PC 0x400abcd on core 1",
            "abort() was called at PC 0x400abcd00 on core 1",
            "abort() was called at PC 0x400abcd0 on core 1 /private/secret",
            "abort() was called at PC 0x400abcdg on core 1",
            "{\"text\":\"abort() was called at PC 0x400abcd0 on core 1\"}",
            "\0abort() was called at PC 0x400abcd0 on core 1",
            "abort() was called at PC 0x400abcd0 on core \u{fffd}"]{
            assert_eq!(abort_site(line),None);
        }
        let mut state=Rejections::default();for _ in 0..20{state.observe("ordinary");}
        let site=state.observe("abort() was called at PC 0x400abcd0 on core 1").unwrap();
        assert_eq!(site.abort_site,expected);
        assert_eq!(site.fault_hint,Some(FaultHint{kind:FaultKind::Abort,occurrences:1}));
        assert!(!format!("{site:?}").contains("abort() was called"));
        for count in 2u64..=65536{
            let site=state.observe("abort() was called at PC 0x400abcd0 on core 1").and_then(|r|r.abort_site);
            assert_eq!(site.is_some(),count.is_power_of_two());
        }
        let mut saturated=Rejections{kinds:[u64::MAX;4],faults:[u64::MAX;6],..Default::default()};
        assert!(saturated.observe("abort() was called at PC 0x400abcd0 on core 1").is_none());
    }
}
