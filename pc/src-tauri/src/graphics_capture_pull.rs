//! Bounded stop-and-wait screenshot client. The transport adapter is the real
//! UsbSerialManager; tests inject only time and messages, never a USB device.
use super::integrity::{checked_chunk, frame_checksum, device_error, Capture};
use serde_json::{json, Value};
use std::time::Duration;

pub(super) const TOTAL_BUDGET: Duration = Duration::from_secs(25);
const ATTEMPT_TIMEOUT: Duration = Duration::from_millis(500);
const ADMISSION_BACKOFF: Duration = Duration::from_millis(50);
const END_DRAIN: Duration = Duration::from_millis(250);
const MAX_ATTEMPTS: u32 = 3;
const MAX_BYTES: usize = 640 * 480 * 2;
const MAX_CHUNKS: u64 = 200;
const MAX_READS: u32 = (MAX_CHUNKS as u32 + 1) * MAX_ATTEMPTS;
const MAX_EVENTS: u32 = 4096;

pub(super) trait Transport {
    fn elapsed(&self) -> Duration;
    fn send(&mut self, topic: &str, payload: &Value) -> Result<(), ()>;
    fn receive(&mut self, timeout: Duration) -> Result<Option<(String, Value)>, ()>;
    fn delay(&mut self, duration: Duration);
    /// Exact capability of this connection. Legacy transports keep fire-and-
    /// forget cleanup; support is never inferred from a version string.
    fn close_ack_supported(&self) -> bool { false }
}

pub(super) struct Outcome {
    pub(super) capture: Capture,
    pub(super) diagnostics: Value,
}

struct Pull<'a, T: Transport> {
    io: &'a mut T,
    capture: Capture,
    begin: Option<Value>,
    id: Option<String>,
    request_id: &'a str,
    width: usize,
    height: usize,
    total: usize,
    chunk_bytes: usize,
    chunks: u64,
    reads: u32,
    events: u32,
    late_chunks: u32,
    duplicate_begins: u32,
    duplicate_ends: u32,
    admission_retries: u32,
    attempts: Vec<Value>,
}

fn safe_id(value: &str, max: usize) -> bool {
    !value.is_empty()
        && value.len() <= max
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"_-.".contains(&b))
}

impl<T: Transport> Pull<'_, T> {
    fn budget(&self) -> Result<(), Value> {
        if self.io.elapsed() >= TOTAL_BUDGET {
            Err(json!({"kind":"total_budget"}))
        } else if self.events >= MAX_EVENTS {
            Err(json!({"kind":"event_budget"}))
        } else {
            Ok(())
        }
    }

    fn attempt_error(&mut self, phase: &str, index: Option<u64>, attempt: u32, error: Value) {
        // At most 3 begin + 603 read failures. No unbounded device text or data.
        self.attempts
            .push(json!({"phase":phase,"index":index,"attempt":attempt,
            "elapsedMs":self.io.elapsed().as_millis() as u64,"error":error}));
    }

    fn recv(&mut self, deadline: Duration) -> Result<Option<(String, Value)>, Value> {
        self.budget()?;
        let remaining = deadline.min(TOTAL_BUDGET).saturating_sub(self.io.elapsed());
        if remaining.is_zero() {
            return Ok(None);
        }
        let message = self
            .io
            .receive(remaining)
            .map_err(|_| json!({"kind":"transport_receive"}))?;
        self.budget()?;
        if message.is_some() {
            self.events += 1;
        }
        Ok(message)
    }

    fn check_begin(&self, p: &Value) -> Result<Value, Value> {
        if p["requestId"].as_str() != Some(self.request_id) {
            return Err(json!({"kind":"different_request"}));
        }
        if p["mode"] != "pull-v1"
            || p["format"] != "rgb565le"
            || p["integrity"] != "chunk-fnv1a32-v1"
            || p["width"].as_u64() != Some(self.width as u64)
            || p["height"].as_u64() != Some(self.height as u64)
            || p["totalBytes"].as_u64() != Some(self.total as u64)
            || p["id"].as_str().is_none_or(|id| !safe_id(id, 31))
            || p.get("bootId").is_some_and(|b|b.as_str().is_none_or(|s|s.len()!=16||!s.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b))))
            || p["chunkBytes"].as_u64().is_none_or(|size| {
                size == 0 || size > 3072 || (self.total as u64).div_ceil(size) > MAX_CHUNKS
            })
        {
            return Err(json!({"kind":"invalid_pull_begin"}));
        }
        // Store only bounded protocol fields, never arbitrary extensions.
        let mut metadata=
            json!({"id":p["id"],"mode":"pull-v1","requestId":self.request_id,
            "format":"rgb565le","integrity":"chunk-fnv1a32-v1",
            "width":self.width,"height":self.height,"totalBytes":self.total,
            "chunkBytes":p["chunkBytes"]});
        if let Some(boot)=p.get("bootId"){metadata["bootId"]=boot.clone();}
        Ok(metadata)
    }

    fn start(&mut self) -> Result<(), Value> {
        let mut admission_retry = false;
        for attempt in 1..=MAX_ATTEMPTS {
            self.budget()?;
            if admission_retry {
                self.admission_retries += 1;
            }
            admission_retry = false;
            self.io
                .send(
                    "debug/screenshot",
                    &json!({"fullResolution":true,
                "mode":"pull-v1","requestId":self.request_id}),
                )
                .map_err(|_| json!({"kind":"transport_send","phase":"begin"}))?;
            let deadline = self.io.elapsed() + ATTEMPT_TIMEOUT;
            let error = loop {
                let Some((topic, p)) = self.recv(deadline)? else {
                    break json!({"kind":"timeout"});
                };
                match topic.as_str() {
                    "debug/screenshot_error" => {
                        let error = device_error(&p, "begin");
                        // The firmware releases its resources before this exact
                        // admission failure. No accepted or identified snapshot
                        // may be replaced, even when its error says "busy".
                        if self.begin.is_none()
                            && self.id.is_none()
                            && error["code"] == "renderer_busy"
                            && p["id"].as_str() == Some("")
                            && p["failedIndex"].as_i64() == Some(-1)
                            && p["sentBytes"].as_u64() == Some(0)
                        {
                            admission_retry = true;
                            break error;
                        }
                        return Err(error);
                    }
                    "debug/screenshot_begin" => match self.check_begin(&p) {
                        Ok(begin) => {
                            self.id = begin["id"].as_str().map(str::to_owned);
                            self.chunk_bytes = begin["chunkBytes"].as_u64().unwrap_or(0) as usize;
                            self.chunks = (self.total as u64).div_ceil(self.chunk_bytes as u64);
                            self.capture.observe(&topic, begin.clone());
                            self.begin = Some(begin);
                            return Ok(());
                        }
                        Err(error) if error["kind"] == "different_request" => return Err(error),
                        Err(error) => break error,
                    },
                    "debug/screenshot_chunk" | "debug/screenshot_end" => {
                        return Err(json!({"kind":"response_before_begin"}))
                    }
                    _ => {}
                }
            };
            self.attempt_error("begin", None, attempt, error.clone());
            // A lost/malformed BEGIN is unknown: firmware may have released a
            // partial response and a repeated request could capture a new frame.
            // Only the exact pre-admission renderer_busy proof above may retry.
            if !admission_retry {
                return Err(error);
            }
            if attempt < MAX_ATTEMPTS {
                self.budget()?;
                self.io.delay(
                    ADMISSION_BACKOFF.min(TOTAL_BUDGET.saturating_sub(self.io.elapsed())),
                );
                self.budget()?;
            }
        }
        Err(json!({"kind":"attempts_exhausted","phase":"begin"}))
    }

    /// Late previous-chunk duplicates are accepted only if they equal already
    /// admitted bytes. A different session is always fatal, never a retry hint.
    fn ignore_duplicate(&mut self, topic: &str, p: &Value, index: u64) -> Result<bool, Value> {
        if topic == "debug/screenshot_begin" {
            let begin = self.check_begin(p)?;
            if self.begin.as_ref() != Some(&begin) {
                return Err(json!({"kind":"changed_begin"}));
            }
            self.duplicate_begins += 1;
            return Ok(true);
        }
        if matches!(topic, "debug/screenshot_chunk" | "debug/screenshot_end")
            && p["id"].as_str() != self.id.as_deref()
        {
            return Err(json!({"kind":"different_frame"}));
        }
        if topic == "debug/screenshot_chunk" && index > 0 && p["index"].as_u64() == Some(index - 1)
        {
            let bytes = checked_chunk(
                p,
                self.id.as_deref().unwrap_or(""),
                index - 1,
                self.total,
                self.chunk_bytes,
            )?;
            let offset = (index as usize - 1) * self.chunk_bytes;
            if self.capture.raw.get(offset..offset + bytes.len()) != Some(bytes.as_slice()) {
                return Err(json!({"kind":"changed_duplicate_chunk","index":index - 1}));
            }
            self.late_chunks += 1;
            return Ok(true);
        }
        Ok(false)
    }

    fn check_end(&self, p: &Value) -> Result<Value, Value> {
        if p["id"].as_str() != self.id.as_deref() {
            return Err(json!({"kind":"different_frame"}));
        }
        if p["chunks"].as_u64() != Some(self.chunks)
            || p["totalBytes"].as_u64() != Some(self.total as u64)
        {
            return Err(json!({"kind":"invalid_end_metadata"}));
        }
        let checksum = format!("{:08x}", frame_checksum(&self.capture.raw));
        if p["checksum"] != checksum {
            return Err(json!({"kind":"frame_checksum"}));
        }
        Ok(json!({"id":self.id,"chunks":self.chunks,"totalBytes":self.total,"checksum":checksum}))
    }

    fn read(&mut self, index: u64) -> Result<(), Value> {
        for attempt in 1..=MAX_ATTEMPTS {
            self.budget()?;
            if self.reads >= MAX_READS {
                return Err(json!({"kind":"read_budget"}));
            }
            self.reads += 1;
            self.io
                .send(
                    "debug/screenshot_read",
                    &json!({"id":self.id,"index":index}),
                )
                .map_err(|_| json!({"kind":"transport_send","phase":"read","index":index}))?;
            let deadline = self.io.elapsed() + ATTEMPT_TIMEOUT;
            let error = loop {
                let Some((topic, p)) = self.recv(deadline)? else {
                    break json!({"kind":"timeout"});
                };
                if topic == "debug/screenshot_error" {
                    let error=device_error(&p,"read");
                    // Only the current immutable read may recover a proved
                    // zero-byte mutex refusal. This is not renderer admission,
                    // a driver partial write, a new capture or a business retry.
                    if error["code"]=="snapshot_transport_busy"
                        &&p["id"].as_str()==self.id.as_deref()
                        &&p["failedIndex"].as_u64()==Some(index)&&p["sentBytes"].as_u64()==Some(0)
                        &&error["writer"]["code"]==7&&error["writer"]["part"]==0
                        &&error["writer"]["acceptedBytes"]==0&&error["writer"]["requestedBytes"]==0
                        &&error["writer"]["calls"]==0 {
                        if attempt<MAX_ATTEMPTS {self.io.delay(ADMISSION_BACKOFF);}
                        break error;
                    }
                    return Err(error);
                }
                if self.ignore_duplicate(&topic, &p, index)? {
                    continue;
                }
                match topic.as_str() {
                    "debug/screenshot_chunk" if index < self.chunks => {
                        match checked_chunk(
                            &p,
                            self.id.as_deref().unwrap_or(""),
                            index,
                            self.total,
                            self.chunk_bytes,
                        ) {
                            Ok(_) => {
                                self.capture.observe(&topic, p);
                                return Ok(());
                            }
                            Err(error) => break error,
                        }
                    }
                    "debug/screenshot_end" if index == self.chunks => match self.check_end(&p) {
                        Ok(end) => {
                            self.capture.observe(&topic, end);
                            return Ok(());
                        }
                        Err(error) => break error,
                    },
                    "debug/screenshot_chunk" | "debug/screenshot_end" => {
                        break json!({"kind":"unexpected_response","index":index})
                    }
                    _ => {}
                }
            };
            self.attempt_error("read", Some(index), attempt, error);
        }
        Err(json!({"kind":"attempts_exhausted","phase":"read","index":index}))
    }

    fn check_closed(&self, p: &Value) -> Result<(), Value> {
        if p["id"].as_str() != self.id.as_deref()
            || p["requestId"] != self.request_id
            || p["closed"] != true
        {
            return Err(json!({"kind":"close_ack_mismatch"}));
        }
        Ok(())
    }

    fn drain_end(&mut self, closed: bool) -> Result<(), Value> {
        let deadline = (self.io.elapsed() + END_DRAIN).min(TOTAL_BUDGET);
        while self.io.elapsed() < deadline {
            // Unlike an incomplete capture, an already verified end may drain
            // until the total deadline without scheduling any further work.
            let Some((topic, p)) = self
                .io
                .receive(deadline - self.io.elapsed())
                .map_err(|_| json!({"kind":"transport_receive"}))?
            else {
                break;
            };
            self.events += 1;
            if self.events > MAX_EVENTS {
                return Err(json!({"kind":"event_budget"}));
            }
            if topic == "debug/screenshot_error" {
                return Err(device_error(&p, "end_drain"));
            }
            if topic == "debug/screenshot_closed" {
                // Validate every receipt in the original drain window, not
                // merely the first one. A contradictory late receipt must
                // never turn an unknown close result into verified success.
                if !closed {
                    return Err(json!({"kind":"unexpected_close_ack"}));
                }
                self.check_closed(&p)?;
                continue;
            }
            if self.ignore_duplicate(&topic, &p, self.chunks)? {
                continue;
            }
            if topic == "debug/screenshot_end" {
                self.check_end(&p)?;
                self.duplicate_ends += 1;
            } else if topic == "debug/screenshot_chunk" {
                return Err(json!({"kind":"unexpected_chunk_after_end"}));
            }
        }
        Ok(())
    }

    fn run(&mut self) -> Result<(), Value> {
        if !safe_id(self.request_id, 32) || self.total == 0 || self.total > MAX_BYTES {
            return Err(json!({"kind":"invalid_request"}));
        }
        self.start()?;
        for index in 0..=self.chunks {
            self.read(index)?;
        }
        self.drain_end(false)?;
        if self.capture.report()["passed"] != true {
            return Err(json!({"kind":"strict_capture_failed"}));
        }
        Ok(())
    }

    fn wait_closed(&mut self) -> Result<(), Value> {
        // Use the original frame/attempt budgets. No retry, recapture or
        // deadline extension after an unknown close result.
        let deadline=(self.io.elapsed()+ATTEMPT_TIMEOUT).min(TOTAL_BUDGET);
        loop {
            let Some((topic,p))=self.recv(deadline)? else {
                return Err(json!({"kind":"close_ack_timeout"}));
            };
            if topic=="debug/screenshot_closed" {
                self.check_closed(&p)?;
                return self.drain_end(true);
            }
            if topic=="debug/screenshot_error" {return Err(device_error(&p,"close"));}
            self.capture.observe(&topic,p);
        }
    }
}

pub(super) fn capture<T: Transport>(
    io: &mut T,
    width: usize,
    height: usize,
    request_id: &str,
) -> Outcome {
    let total = width
        .checked_mul(height)
        .and_then(|pixels| pixels.checked_mul(2))
        .unwrap_or(usize::MAX);
    let mut pull = Pull {
        io,
        capture: Capture::new(width, height),
        begin: None,
        id: None,
        request_id,
        width,
        height,
        total,
        chunk_bytes: 0,
        chunks: 0,
        reads: 0,
        events: 0,
        late_chunks: 0,
        duplicate_begins: 0,
        duplicate_ends: 0,
        admission_retries: 0,
        attempts: Vec::new(),
    };
    if let Err(error) = pull.run() {
        pull.capture.error(error);
    }
    // One cleanup on every exit. Only an explicitly negotiated peer receives
    // the new exact close transaction. Legacy cleanup remains byte-identical.
    // If begin itself was lost, the immutable snapshot expires by its old TTL.
    let close_ack_required=pull.io.close_ack_supported();
    let mut close_acked=false;
    let mut close_request_metadata=Value::Null;
    let close_sent = if let Some(id) = &pull.id {
        let payload=if close_ack_required {json!({"id":id,"requestId":pull.request_id,"ack":true})} else {json!({"id":id})};
        // Same canonical serde_json envelope used by send_to_board. Numeric
        // correlation only; no raw JSON/token/image is copied to diagnostics.
        if close_ack_required {
            if let Ok(bytes)=serde_json::to_vec(&json!({"topic":"debug/screenshot_close","payload":&payload})) {
                close_request_metadata=json!({"bytes":bytes.len(),"hash":frame_checksum(&bytes)});
            }
        }
        match pull.io.send("debug/screenshot_close", &payload) {
            Ok(()) => {
                if close_ack_required {
                    match pull.wait_closed() {Ok(())=>close_acked=true,Err(error)=>pull.capture.error(error)}
                }
                true
            },
            Err(()) => {
                pull.capture.error(json!({"kind":"close_send_failed"}));
                false
            }
        }
    } else {
        false
    };
    if pull.io.elapsed() > TOTAL_BUDGET {
        pull.capture.error(json!({"kind":"total_budget"}));
    }
    let diagnostics = json!({"mode":"pull-v1","totalBudgetMs":TOTAL_BUDGET.as_millis(),
        "attemptTimeoutMs":ATTEMPT_TIMEOUT.as_millis(),"maxAttempts":MAX_ATTEMPTS,
        "elapsedMs":pull.io.elapsed().as_millis(),"reads":pull.reads,"events":pull.events,
        "latePreviousChunks":pull.late_chunks,"duplicateBegins":pull.duplicate_begins,
        "duplicateEnds":pull.duplicate_ends,"attemptErrors":pull.attempts,"closeSent":close_sent,
        "closeAckRequired":close_ack_required,"closeAcked":close_acked,"closeRequest":close_request_metadata,
        "admissionRetries":pull.admission_retries,"admissionBackoffMs":ADMISSION_BACKOFF.as_millis(),
        "readBusyBackoffMs":ADMISSION_BACKOFF.as_millis(),
        "autoRecaptures":0,"budgetScope":"checked at transport boundaries; cannot interrupt driver calls"});
    Outcome {
        capture: pull.capture,
        diagnostics,
    }
}

#[cfg(test)]
mod tests {
    use super::super::integrity::chunk_checksum;
    use super::*;
    use base64::{engine::general_purpose::STANDARD, Engine};
    use std::collections::{BTreeMap, VecDeque};

    #[derive(Clone, Copy, PartialEq)]
    enum Fault {
        None,
        LostBegin,
        LostChunk,
        Truncated96,
        Corrupted,
        InvalidBase64,
        OldSession,
        OldRequest,
        LatePrevious,
        DuplicateBegin,
        LostEnd,
        WrongEndHash,
        WrongEndSize,
        DeviceError,
        EndThenError,
        UnknownDeviceError,
        PermanentLoss,
        OversizedData,
        FutureIndex,
        ChangedDuplicate,
        Slow,
        Flood,
        AllRetries,
        BeginRendererBusyThenSuccess,
        BeginRendererBusy,
        BeginRendererBusyWithId,
        BeginRendererBusyWithProgress,
        BeginOtherDeviceError,
        ChunkRendererBusy,
        EndRendererBusy,
        TxBusyOnce,TxBusyEndOnce,TxBusyPermanent,TxBusyWrongSnapshot,TxBusyPartial,TxBusyMissingMetadata,
    }

    struct Fake {
        now: Duration,
        delay: Duration,
        fault: Fault,
        width: usize,
        height: usize,
        raw: Vec<u8>,
        chunk_bytes: usize,
        queued: VecDeque<(String, Value)>,
        sent: Vec<(String, Value)>,
        per_index: BTreeMap<u64, u32>,
        begins: u32,
        delays: Vec<Duration>,
    }
    impl Fake {
        fn new(fault: Fault, full: bool) -> Self {
            let (width, height) = if full { (640, 480) } else { (64, 48) };
            let raw = (0..width * height * 2)
                .map(|i| ((i * 73 + i / 7) % 256) as u8)
                .collect();
            Self {
                now: Duration::ZERO,
                delay: if fault == Fault::Slow {
                    Duration::from_millis(500)
                } else {
                    Duration::from_millis(1)
                },
                fault,
                width,
                height,
                raw,
                chunk_bytes: 3072,
                queued: VecDeque::new(),
                sent: Vec::new(),
                per_index: BTreeMap::new(),
                begins: 0,
                delays: Vec::new(),
            }
        }
        fn chunk(&self, index: u64) -> Value {
            let start = index as usize * self.chunk_bytes;
            let end = (start + self.chunk_bytes).min(self.raw.len());
            let bytes = &self.raw[start..end];
            json!({"id":"immutable-frame","index":index,"rawSize":bytes.len(),
                "checksum":format!("{:08x}",chunk_checksum(bytes)),"data":STANDARD.encode(bytes)})
        }
        fn enqueue(&mut self, topic: &str, p: Value) {
            self.queued.push_back((topic.into(), p));
        }
        fn run(&mut self) -> Outcome {
            capture(self, self.width, self.height, "safe-immutable-request")
        }
        fn reads(&self) -> Vec<u64> {
            self.sent
                .iter()
                .filter(|(topic, _)| topic == "debug/screenshot_read")
                .map(|(_, p)| p["index"].as_u64().unwrap())
                .collect()
        }
    }
    impl Transport for Fake {
        fn elapsed(&self) -> Duration {
            self.now
        }
        fn send(&mut self, topic: &str, p: &Value) -> Result<(), ()> {
            self.sent.push((topic.into(), p.clone()));
            if topic == "debug/screenshot" {
                self.begins += 1;
                if self.fault == Fault::LostBegin && self.begins == 1 {
                    return Ok(());
                }
                if self.fault == Fault::AllRetries && self.begins < 3 {
                    self.enqueue("debug/screenshot_error", json!({"id":"",
                        "error":"renderer busy","failedIndex":-1,"sentBytes":0}));
                    return Ok(());
                }
                if matches!(
                    self.fault,
                    Fault::BeginRendererBusy
                        | Fault::BeginRendererBusyWithId
                        | Fault::BeginRendererBusyWithProgress
                        | Fault::BeginOtherDeviceError
                ) || self.fault == Fault::BeginRendererBusyThenSuccess && self.begins < 3
                {
                    self.enqueue("debug/screenshot_error", json!({
                        "id":if self.fault == Fault::BeginRendererBusyWithId {"SECRET/stale-frame"} else {""},
                        "error":if self.fault == Fault::BeginOtherDeviceError {"audio busy"} else {"renderer busy"},
                        "failedIndex":-1,
                        "sentBytes":if self.fault == Fault::BeginRendererBusyWithProgress {3072} else {0},
                        "untrustedExtension":"SECRET/device/path"}));
                    return Ok(());
                }
                let request = if self.fault == Fault::OldRequest {
                    json!("stale-request")
                } else {
                    p["requestId"].clone()
                };
                let begin = json!({"id":"immutable-frame","requestId":request,"mode":"pull-v1",
                    "width":self.width,"height":self.height,"format":"rgb565le",
                    "integrity":"chunk-fnv1a32-v1","totalBytes":self.raw.len(),"chunkBytes":self.chunk_bytes,
                    "untrustedExtension":"must not be retained"});
                self.enqueue("debug/screenshot_begin", begin.clone());
                if self.fault == Fault::DuplicateBegin {
                    self.enqueue("debug/screenshot_begin", begin);
                }
                return Ok(());
            }
            if topic != "debug/screenshot_read" {
                return Ok(());
            }
            assert_eq!(p["id"], "immutable-frame");
            let index = p["index"].as_u64().unwrap();
            let attempt = self.per_index.entry(index).or_default();
            *attempt += 1;
            let attempt = *attempt;
            let chunks = self.raw.len().div_ceil(self.chunk_bytes) as u64;
            assert!(index <= chunks);
            if (matches!(self.fault,Fault::TxBusyOnce|Fault::TxBusyPermanent|Fault::TxBusyWrongSnapshot|Fault::TxBusyPartial|Fault::TxBusyMissingMetadata)&&index==0
                ||self.fault==Fault::TxBusyEndOnce&&index==chunks)
                &&(attempt==1||self.fault==Fault::TxBusyPermanent){
                let mut busy=json!({"id":if self.fault==Fault::TxBusyWrongSnapshot {"different-frame"}else{"immutable-frame"},
                    "error":"snapshot transport busy","failedIndex":index,"sentBytes":0});
                if self.fault!=Fault::TxBusyMissingMetadata {
                    let o=busy.as_object_mut().unwrap();
                    for (k,v) in [("writerReason",7),("writerPart",0),("writerRequested",0),("writerAccepted",if self.fault==Fault::TxBusyPartial{1}else{0}),
                        ("writerCalls",0),("writerReturn",0),("writerDrain",0),("writerElapsedTicks",0)]{o.insert(k.into(),json!(v));}
                }
                self.enqueue("debug/screenshot_error",busy);return Ok(());
            }
            if self.fault == Fault::ChunkRendererBusy
                || self.fault == Fault::EndRendererBusy && index == chunks
            {
                // Even a malformed mid-transfer error claiming no snapshot is
                // not an admission retry after begin has been accepted.
                self.enqueue("debug/screenshot_error", json!({"id":"",
                    "error":"renderer busy","failedIndex":-1,"sentBytes":0}));
                return Ok(());
            }
            if self.fault == Fault::PermanentLoss && index == 1
                || self.fault == Fault::LostChunk && index == 1 && attempt == 1
                || self.fault == Fault::LostEnd && index == chunks && attempt == 1
            {
                return Ok(());
            }
            if self.fault == Fault::DeviceError || self.fault == Fault::UnknownDeviceError {
                self.enqueue("debug/screenshot_error", json!({"id":"immutable-frame",
                    "error":if self.fault == Fault::DeviceError {"transport or encode failed"} else {"SECRET/unknown/device/path"},
                    "failedIndex":index,"sentBytes":0}));
                return Ok(());
            }
            if self.fault == Fault::Flood {
                for _ in 0..MAX_EVENTS + 1 {
                    self.enqueue("unrelated/status", json!({}));
                }
                self.delay = Duration::ZERO;
                return Ok(());
            }
            if index == chunks {
                let mut end = json!({"id":"immutable-frame","chunks":chunks,"totalBytes":self.raw.len(),
                    "checksum":format!("{:08x}",frame_checksum(&self.raw))});
                if self.fault == Fault::WrongEndHash
                    || self.fault == Fault::AllRetries && attempt < 3
                {
                    end["checksum"] = json!("deadbeef");
                }
                if self.fault == Fault::WrongEndSize {
                    end["totalBytes"] = json!(self.raw.len() - 1);
                }
                self.enqueue("debug/screenshot_end", end);
                if self.fault == Fault::EndThenError {
                    self.enqueue("debug/screenshot_error", json!({"error":"transport or encode failed","failedIndex":chunks,"sentBytes":self.raw.len()}));
                }
                return Ok(());
            }
            let mut chunk = self.chunk(index);
            if self.fault == Fault::AllRetries && attempt < 3 {
                chunk["data"] = json!("invalid!!");
            }
            if self.fault == Fault::OldSession {
                chunk["id"] = json!("stale-frame");
            }
            if self.fault == Fault::Truncated96 && index == 23 && attempt == 1 {
                let mut data = chunk["data"].as_str().unwrap().to_owned();
                data.replace_range(1024..1152, "");
                assert_eq!(STANDARD.decode(&data).unwrap().len(), 2976);
                chunk["data"] = json!(data);
            }
            if index == 0 && attempt == 1 {
                match self.fault {
                    Fault::Corrupted => chunk["data"] = json!(STANDARD.encode([255u8; 3072])),
                    Fault::InvalidBase64 => chunk["data"] = json!("invalid!!"),
                    Fault::OversizedData => chunk["data"] = json!("A".repeat(16384)),
                    Fault::FutureIndex => chunk["index"] = json!(1),
                    _ => {}
                }
            }
            if index == 1 && matches!(self.fault, Fault::LatePrevious | Fault::ChangedDuplicate) {
                let mut previous = self.chunk(0);
                if self.fault == Fault::ChangedDuplicate {
                    let changed = [13u8; 3072];
                    previous["data"] = json!(STANDARD.encode(changed));
                    previous["checksum"] = json!(format!("{:08x}", chunk_checksum(&changed)));
                }
                self.enqueue("debug/screenshot_chunk", previous);
            }
            self.enqueue("debug/screenshot_chunk", chunk);
            Ok(())
        }
        fn receive(&mut self, timeout: Duration) -> Result<Option<(String, Value)>, ()> {
            if self.queued.is_empty() || timeout < self.delay {
                self.now += timeout;
                return Ok(None);
            }
            self.now += self.delay;
            Ok(self.queued.pop_front())
        }
        fn delay(&mut self, duration: Duration) {
            self.delays.push(duration);
            self.now += duration;
        }
    }

    struct CloseFake {wire:Fake,fault:u8}
    impl Transport for CloseFake {
        fn elapsed(&self)->Duration {self.wire.elapsed()}
        fn close_ack_supported(&self)->bool {true}
        fn delay(&mut self,d:Duration){self.wire.delay(d);}
        fn receive(&mut self,d:Duration)->Result<Option<(String,Value)>,()>{self.wire.receive(d)}
        fn send(&mut self,topic:&str,p:&Value)->Result<(),()> {
            self.wire.send(topic,p)?;
            if topic=="debug/screenshot_close" {
                assert_eq!(p["ack"],true);assert_eq!(p["requestId"],"safe-immutable-request");
                let mut ack=json!({"id":p["id"],"requestId":p["requestId"],"closed":true});
                match self.fault {
                    1=>return Ok(()),
                    2=>ack["id"]=json!("another-snapshot"),
                    3=>ack["requestId"]=json!("stale-request"),
                    4=>ack["closed"]=json!(false),
                    5=>{self.wire.enqueue("debug/screenshot_error",json!({"error":"snapshot id mismatched"}));return Ok(());},
                    7=>self.wire.now=TOTAL_BUDGET-Duration::from_millis(1),
                    8=>return Err(()),
                    _=>{}
                }
                self.wire.enqueue("debug/screenshot_closed",ack.clone());
                if self.fault==6 {self.wire.enqueue("debug/screenshot_error",json!({"error":"snapshot close acknowledgement failed"}));}
                if (9..=12).contains(&self.fault) {
                    match self.fault {
                        9=>ack["id"]=json!("another-snapshot"),
                        10=>ack["requestId"]=json!("stale-request"),
                        11=>ack["closed"]=json!(false),
                        _=>{}
                    }
                    self.wire.enqueue("debug/screenshot_closed",ack);
                }
            }
            Ok(())
        }
    }
    #[test]
    fn screenshot_close_receipt_is_negotiated_exact_bounded_and_never_retried(){
        for fault in 0..=12 {
            let mut io=CloseFake{wire:Fake::new(Fault::None,false),fault};
            let out=capture(&mut io,64,48,"safe-immutable-request");
            let passed=fault==0||fault==12;
            assert_eq!(out.capture.report()["passed"],passed,"close fault {fault}");
            assert_eq!(out.capture.raw,io.wire.raw);assert_eq!(io.wire.begins,1);
            let close=io.wire.sent.iter().filter(|(topic,_)|topic=="debug/screenshot_close").collect::<Vec<_>>();
            assert_eq!(close.len(),1);assert_eq!(out.diagnostics["autoRecaptures"],0);
            assert_eq!(out.diagnostics["closeAckRequired"],true);
            assert_eq!(out.diagnostics["closeAcked"],passed);
            let bytes=serde_json::to_vec(&json!({"topic":close[0].0,"payload":close[0].1})).unwrap();
            assert_eq!(out.diagnostics["closeRequest"],json!({"bytes":bytes.len(),"hash":frame_checksum(&bytes)}));
            assert!(io.wire.now<=TOTAL_BUDGET);
        }
        let mut legacy=Fake::new(Fault::None,false);let out=legacy.run();
        assert_eq!(out.capture.report()["passed"],true);assert_eq!(out.diagnostics["closeAckRequired"],false);
        let close=legacy.sent.last().unwrap();assert_eq!(close.0,"debug/screenshot_close");
        assert_eq!(close.1,json!({"id":"immutable-frame"}));
    }

    #[test]
    fn screenshot_pull_rereads_only_proved_unwritten_current_snapshot_busy_with_original_limits(){
        for fault in [Fault::TxBusyOnce,Fault::TxBusyEndOnce]{
            let mut io=Fake::new(fault,true);let out=io.run();assert_eq!(out.capture.report()["passed"],true);
            assert_eq!(out.capture.raw,io.raw);assert_eq!(io.begins,1);assert_eq!(out.diagnostics["autoRecaptures"],0);
            assert_eq!(out.diagnostics["reads"],MAX_CHUNKS+2);assert_eq!(io.delays,vec![ADMISSION_BACKOFF]);
            assert_eq!(out.diagnostics["attemptErrors"][0]["error"]["writer"]["reason"],"transport_mutex");
            assert!(io.now<TOTAL_BUDGET);
        }
        let mut io=Fake::new(Fault::TxBusyPermanent,true);let out=io.run();
        assert_eq!(out.capture.report()["passed"],false);assert_eq!(out.diagnostics["reads"],MAX_ATTEMPTS);
        assert_eq!(io.begins,1);assert_eq!(io.delays,vec![ADMISSION_BACKOFF;2]);
        for fault in [Fault::TxBusyWrongSnapshot,Fault::TxBusyPartial,Fault::TxBusyMissingMetadata]{
            let mut io=Fake::new(fault,true);let out=io.run();assert_eq!(out.capture.report()["passed"],false);
            assert_eq!(out.diagnostics["reads"],1);assert!(io.delays.is_empty());assert_eq!(io.begins,1);
        }
    }

    #[test]
    fn screenshot_pull_renderer_busy_admission_retries_same_nonce_then_full_frame() {
        let mut io = Fake::new(Fault::BeginRendererBusyThenSuccess, true);
        let out = io.run();
        let report = out.capture.report();
        assert_eq!(report["passed"], true, "{report}");
        assert_eq!(report["checksumMatches"], true);
        assert_eq!(out.capture.raw, io.raw);
        assert_eq!(out.capture.raw.len(), MAX_BYTES);
        assert_eq!(io.begins, MAX_ATTEMPTS);
        let requests = io.sent.iter().filter(|(topic, _)| topic == "debug/screenshot")
            .map(|(_, payload)| payload).collect::<Vec<_>>();
        assert!(requests.windows(2).all(|pair| pair[0] == pair[1]));
        assert_eq!(requests[0]["requestId"], "safe-immutable-request");
        assert_eq!(io.delays, vec![ADMISSION_BACKOFF; 2]);
        assert_eq!(out.diagnostics["admissionRetries"], 2);
        assert_eq!(out.diagnostics["autoRecaptures"], 0);
        assert_eq!(out.diagnostics["closeSent"], true);
        assert_eq!(io.reads().len(), MAX_CHUNKS as usize + 1);
        assert!(io.per_index.values().all(|attempts| *attempts == 1));
        assert_eq!(io.sent.iter().filter(|(topic, _)| topic == "debug/screenshot_close").count(), 1);
        let attempts = out.diagnostics["attemptErrors"].as_array().unwrap();
        assert_eq!(attempts.len(), 2);
        for (index, attempt) in attempts.iter().enumerate() {
            assert_eq!(attempt["phase"], "begin");
            assert_eq!(attempt["attempt"], index + 1);
            assert_eq!(attempt["error"]["code"], "renderer_busy");
        }
        assert!(!out.diagnostics.to_string().contains("SECRET"));
        assert!(io.now <= TOTAL_BUDGET);
        let dir = tempfile::tempdir().unwrap();
        let saved = out.capture.save_report(&dir.path().join("capture.json"),
            json!({"pull":out.diagnostics}));
        assert_eq!(saved["passed"], true);
        assert_eq!(saved["pull"]["admissionRetries"], 2);
        assert_eq!(saved["pull"]["autoRecaptures"], 0);
    }

    #[test]
    fn screenshot_pull_renderer_busy_admission_exhausts_without_snapshot_or_recapture() {
        let mut io = Fake::new(Fault::BeginRendererBusy, true);
        let out = io.run();
        let report = out.capture.report();
        assert_eq!(report["passed"], false);
        assert_eq!(report["begin"], Value::Null);
        assert_eq!(report["errors"][0]["kind"], "attempts_exhausted");
        assert_eq!(report["errors"][0]["phase"], "begin");
        assert!(out.capture.raw.is_empty());
        assert!(io.reads().is_empty());
        assert_eq!(io.sent.len(), MAX_ATTEMPTS as usize);
        assert_eq!(io.begins, MAX_ATTEMPTS);
        assert_eq!(io.delays, vec![ADMISSION_BACKOFF; 2]);
        assert_eq!(out.diagnostics["admissionRetries"], 2);
        assert_eq!(out.diagnostics["autoRecaptures"], 0);
        assert_eq!(out.diagnostics["closeSent"], false);
        let attempts = out.diagnostics["attemptErrors"].as_array().unwrap();
        assert_eq!(attempts.len(), MAX_ATTEMPTS as usize);
        assert!(attempts.iter().all(|attempt| attempt["error"]["code"] == "renderer_busy"));
        assert!(!out.diagnostics.to_string().contains("SECRET"));
        assert_eq!(io.now, ADMISSION_BACKOFF * 2 + Duration::from_millis(3));
    }

    #[test]
    fn screenshot_pull_busy_is_not_retried_after_begin_or_with_snapshot_metadata() {
        for fault in [Fault::ChunkRendererBusy, Fault::EndRendererBusy,
            Fault::BeginRendererBusyWithId, Fault::BeginRendererBusyWithProgress,
            Fault::BeginOtherDeviceError] {
            let mut io = Fake::new(fault, false);
            let out = io.run();
            let report = out.capture.report();
            assert_eq!(report["passed"], false);
            assert_eq!(report["errors"][0]["kind"], "device_error");
            assert_eq!(io.begins, 1);
            assert!(io.delays.is_empty());
            assert_eq!(out.diagnostics["admissionRetries"], 0);
            assert_eq!(out.diagnostics["autoRecaptures"], 0);
            assert!(out.diagnostics["attemptErrors"].as_array().unwrap().is_empty());
            assert!(!format!("{report} {}", out.diagnostics).contains("SECRET"));
            if matches!(fault, Fault::ChunkRendererBusy | Fault::EndRendererBusy) {
                assert_eq!(out.diagnostics["closeSent"], true);
                assert_eq!(report["errors"][0]["phase"], "read");
                assert!(io.per_index.values().all(|attempts| *attempts == 1));
                if fault == Fault::ChunkRendererBusy { assert_eq!(io.reads(), vec![0]); }
            } else {
                assert_eq!(out.diagnostics["closeSent"], false);
                assert!(io.reads().is_empty());
            }
        }
    }

    #[test]
    fn screenshot_pull_admission_backoff_obeys_original_total_budget() {
        let mut io = Fake::new(Fault::BeginRendererBusyThenSuccess, true);
        io.now = TOTAL_BUDGET - Duration::from_millis(40);
        let out = io.run();
        let report = out.capture.report();
        assert_eq!(report["passed"], false);
        assert_eq!(report["errors"][0]["kind"], "total_budget");
        assert_eq!(io.now, TOTAL_BUDGET);
        assert_eq!(io.begins, 1);
        assert_eq!(io.delays, vec![Duration::from_millis(39)]);
        assert_eq!(out.diagnostics["admissionRetries"], 0);
        assert_eq!(out.diagnostics["autoRecaptures"], 0);
        assert_eq!(out.diagnostics["attemptErrors"].as_array().unwrap().len(), 1);
        assert_eq!(out.diagnostics["attemptErrors"][0]["error"]["code"], "renderer_busy");
        assert!(out.capture.raw.is_empty());
        assert!(io.reads().is_empty());
    }

    #[test]
    fn screenshot_pull_recovers_only_current_block_and_retains_redacted_attempts() {
        for fault in [
            Fault::None,
            Fault::LostChunk,
            Fault::Truncated96,
            Fault::Corrupted,
            Fault::InvalidBase64,
            Fault::LatePrevious,
            Fault::DuplicateBegin,
            Fault::LostEnd,
            Fault::OversizedData,
            Fault::FutureIndex,
        ] {
            let mut io = Fake::new(fault, fault == Fault::Truncated96);
            let out = io.run();
            assert_eq!(out.capture.report()["passed"], true, "{}", out.diagnostics);
            assert_eq!(out.capture.raw, io.raw);
            assert_eq!(out.diagnostics["autoRecaptures"], 0);
            assert_eq!(out.diagnostics["closeSent"], true);
            assert!(io.now <= TOTAL_BUDGET);
            assert_eq!(io.sent.last().unwrap().0, "debug/screenshot_close");
            assert!(io
                .per_index
                .values()
                .all(|attempts| *attempts <= MAX_ATTEMPTS));
            let report = out.capture.report();
            assert!(report["begin"].get("untrustedExtension").is_none());
            let diagnostics = out.diagnostics.to_string();
            assert!(!diagnostics.contains("data\"") && !diagnostics.contains("immutable-frame"));
            if fault == Fault::Truncated96 {
                assert_eq!(io.per_index[&23], 2);
                assert!(io
                    .per_index
                    .iter()
                    .all(|(index, count)| *index == 23 || *count == 1));
                assert_eq!(
                    out.diagnostics["attemptErrors"][0]["error"]["received"],
                    2976
                );
                assert_eq!(
                    out.diagnostics["attemptErrors"][0]["error"]["expected"],
                    3072
                );
                assert_eq!(out.capture.raw.len(), 614400);
            }
            if fault == Fault::LatePrevious {
                assert_eq!(out.diagnostics["latePreviousChunks"], 1);
            }
            if fault == Fault::DuplicateBegin {
                assert_eq!(out.diagnostics["duplicateBegins"], 1);
            }
            assert_eq!(io.begins, 1);
            let dir = tempfile::tempdir().unwrap();
            let saved = out.capture.save_report(
                &dir.path().join("capture.json"),
                json!({"pull":out.diagnostics}),
            );
            assert_eq!(saved["passed"], true);
        }
    }

    #[test]
    fn screenshot_pull_fails_closed_for_session_errors_final_checksum_and_exhaustion() {
        for fault in [
            Fault::OldSession,
            Fault::OldRequest,
            Fault::WrongEndHash,
            Fault::WrongEndSize,
            Fault::DeviceError,
            Fault::UnknownDeviceError,
            Fault::EndThenError,
            Fault::PermanentLoss,
            Fault::ChangedDuplicate,
        ] {
            let mut io = Fake::new(fault, false);
            let out = io.run();
            let report = out.capture.report();
            assert_eq!(report["passed"], false, "{report}");
            assert_eq!(out.diagnostics["autoRecaptures"], 0);
            assert!(out.capture.raw.len() <= io.raw.len());
            assert!(io
                .per_index
                .values()
                .all(|attempts| *attempts <= MAX_ATTEMPTS));
            assert_eq!(io.begins, 1);
            assert!(!format!("{report} {}", out.diagnostics).contains("SECRET"));
            if fault == Fault::PermanentLoss {
                assert_eq!(io.reads(), vec![0, 1, 1, 1]);
                assert_eq!(
                    out.diagnostics["attemptErrors"].as_array().unwrap().len(),
                    3
                );
                assert_eq!(out.capture.raw.len(), 3072);
            }
            if fault == Fault::WrongEndHash {
                assert_eq!(io.reads(), vec![0, 1, 2, 2, 2]);
                assert_eq!(
                    out.diagnostics["attemptErrors"][0]["error"]["kind"],
                    "frame_checksum"
                );
                assert_eq!(report["checksumMatches"], false);
            }
            if fault == Fault::DeviceError || fault == Fault::UnknownDeviceError {
                assert_eq!(io.reads(), vec![0]);
                assert_eq!(report["errors"][0]["kind"], "device_error");
            }
            if fault == Fault::EndThenError {
                assert_eq!(report["endReceived"], true);
                assert_eq!(report["checksumMatches"], true);
                assert_eq!(report["errors"][0]["phase"], "end_drain");
            }
        }
    }

    #[test]
    fn screenshot_pull_total_event_request_and_allocation_budgets_are_bounded() {
        let mut io = Fake::new(Fault::Slow, true);
        let out = io.run();
        assert_eq!(out.capture.report()["passed"], false);
        assert!(io.now <= TOTAL_BUDGET);
        assert!(io.reads().len() < MAX_READS as usize);
        assert!(out.capture.raw.len() <= MAX_BYTES);
        assert!(out.capture.report()["errors"]
            .to_string()
            .contains("total_budget"));
        let mut io = Fake::new(Fault::Flood, false);
        let out = io.run();
        assert_eq!(out.capture.report()["passed"], false);
        assert!(out.capture.report()["errors"]
            .to_string()
            .contains("event_budget"));
        assert_eq!(out.diagnostics["events"], MAX_EVENTS);
        for request_id in ["", "not safe", "01234567890123456789012345678901234"] {
            let mut io = Fake::new(Fault::None, false);
            let out = capture(&mut io, 640, 480, request_id);
            assert_eq!(out.capture.report()["passed"], false);
            assert!(io.sent.is_empty());
        }
        let mut io = Fake::new(Fault::None, false);
        let out = capture(&mut io, usize::MAX, 2, "valid");
        assert_eq!(out.capture.report()["passed"], false);
        assert!(io.sent.is_empty() && out.capture.raw.is_empty());
        // Non-full last chunk retains exact length, not a forced 3072 bytes.
        let mut io = Fake::new(Fault::None, false);
        io.width = 64;
        io.height = 25;
        io.raw.truncate(64 * 25 * 2);
        let out = io.run();
        assert_eq!(out.capture.report()["passed"], true);
        assert_eq!(out.capture.raw.len(), 3200);
        assert_eq!(out.capture.report()["chunks"][1]["receivedBytes"], 128);
    }

    #[test]
    fn screenshot_pull_maximum_retries_remain_below_firmware_read_limit() {
        let mut io = Fake::new(Fault::AllRetries, true);
        let out = io.run();
        assert_eq!(out.capture.report()["passed"], true);
        assert_eq!(io.reads().len(), MAX_READS as usize);
        assert_eq!(io.sent.len(), 607); // 3 begin + 603 reads + one close.
        assert!(io.reads().len() < 800);
        assert_eq!(
            out.diagnostics["attemptErrors"].as_array().unwrap().len(),
            404
        );
        assert_eq!(out.capture.raw.len(), MAX_BYTES);
        assert_eq!(out.capture.raw.capacity(), MAX_BYTES);
        assert!(io.now <= TOTAL_BUDGET);
        assert_eq!(out.diagnostics["autoRecaptures"], 0);
    }
    #[test]
    fn screenshot_pull_lost_begin_stops_even_when_second_begin_would_succeed() {
        // Fake drops only BEGIN #1. Its next BEGIN would return a valid full
        // frame: the old implementation wrongly passed with autoRecaptures=0.
        let mut io = Fake::new(Fault::LostBegin, false);
        let out = io.run();
        let report = out.capture.report();
        assert_eq!(io.begins, 1, "unknown BEGIN replay: passed={}, autoRecaptures={}",
            report["passed"], out.diagnostics["autoRecaptures"]);
        assert_eq!(report["passed"], false, "unknown BEGIN must stop, not recapture");
        assert_eq!(io.sent.len(), 1);
        assert!(io.reads().is_empty() && io.delays.is_empty());
        assert!(out.capture.raw.is_empty());
        assert_eq!(report["begin"], Value::Null);
        assert_eq!(report["errors"][0]["kind"], "timeout");
        assert_eq!(out.diagnostics["attemptErrors"].as_array().unwrap().len(), 1);
        assert_eq!(out.diagnostics["admissionRetries"], 0);
        assert_eq!(out.diagnostics["closeSent"], false);
        assert_eq!(out.diagnostics["autoRecaptures"], 0);
    }

    struct PoisonBegin { wire: Fake, response: (String, Value) }
    impl Transport for PoisonBegin {
        fn elapsed(&self) -> Duration { self.wire.elapsed() }
        fn delay(&mut self, d: Duration) { self.wire.delay(d); }
        fn receive(&mut self, d: Duration) -> Result<Option<(String,Value)>,()> { self.wire.receive(d) }
        fn send(&mut self, topic: &str, p: &Value) -> Result<(),()> {
            self.wire.send(topic,p)?;
            if topic == "debug/screenshot" && self.wire.begins == 1 {
                self.wire.queued.clear();
                self.wire.queued.push_back(self.response.clone());
            }
            Ok(())
        }
    }

    #[test]
    fn screenshot_pull_malformed_begin_stops_before_available_second_capture() {
        for response in [Value::Null,
            json!({"requestId":"safe-immutable-request","id":"immutable-frame","mode":"pull-v1",
                "format":"rgb565le","integrity":"chunk-fnv1a32-v1","width":64,"height":48,
                "totalBytes":6144,"chunkBytes":0})] {
            let mut io=PoisonBegin { wire:Fake::new(Fault::None,false),
                response:("debug/screenshot_begin".into(),response) };
            let out=capture(&mut io,64,48,"safe-immutable-request");
            assert_eq!(out.capture.report()["passed"],false);
            assert_eq!(io.wire.begins,1);
            assert_eq!(io.wire.sent.len(),1);
            assert!(io.wire.reads().is_empty() && io.wire.delays.is_empty());
            assert_eq!(out.diagnostics["admissionRetries"],0);
            assert_eq!(out.diagnostics["closeSent"],false);
        }
    }

    #[test]
    fn screenshot_pull_busy_begin_retry_requires_exact_typed_pre_admission_fields() {
        let valid=json!({"error":"renderer busy","id":"","failedIndex":-1,"sentBytes":0});
        for (key,value) in [("id",Value::Null),("id",json!("snapshot")),
            ("failedIndex",json!("-1")),("failedIndex",json!(0)),
            ("sentBytes",json!("0")),("sentBytes",json!(1)),("error",json!("snapshot busy"))] {
            let mut response=valid.clone();response[key]=value;
            let mut io=PoisonBegin {wire:Fake::new(Fault::None,false),
                response:("debug/screenshot_error".into(),response)};
            let out=capture(&mut io,64,48,"safe-immutable-request");
            assert_eq!(out.capture.report()["passed"],false,"field {key}");
            assert_eq!(io.wire.begins,1);assert_eq!(io.wire.sent.len(),1);
            assert!(io.wire.delays.is_empty());assert_eq!(out.diagnostics["admissionRetries"],0);
        }
    }

}
