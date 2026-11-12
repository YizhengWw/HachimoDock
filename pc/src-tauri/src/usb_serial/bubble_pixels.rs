//! Main-renderer-owned, bounded optional alpha assets. Tokens correlate
//! lifecycle; they do not authenticate a remote peer. Latest text wins.
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::VecDeque;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use std::time::{Duration, Instant};
pub(super) type RenderSink = Arc<dyn Fn(Value) + Send + Sync>;
pub(super) const SCHEMA: &str = "p4-text-alpha-v2";
pub(super) const MAX_BYTES: usize = 32 * 1024;
pub(super) const CHUNK_BYTES: usize = 512;
pub(super) const DEADLINE_MS: u64 = 10_000;
fn scope(topic: &str) -> &str {
    if topic.starts_with("state/") || topic == "speech/text" {
        "bubble"
    } else {
        topic
    }
}
pub(super) fn is_text_topic(topic: &str) -> bool {
    topic.starts_with("state/")
        || matches!(
            topic,
            "session/current" | "speech/text" | "ui/conversation" | "audio/play_subtitle"
        )
}
fn hex(s: &str, n: usize) -> bool {
    s.len() == n
        && s.bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
pub(super) fn valid_boot(s: &str) -> bool {
    hex(s, 16) && s != "0000000000000000"
}
fn text(v: Option<&Value>, limit: usize) -> String {
    let s = v
        .and_then(Value::as_str)
        .unwrap_or("")
        .split('\0')
        .next()
        .unwrap_or("");
    let mut end = s.len().min(limit);
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_owned()
}
fn hash(bytes: impl IntoIterator<Item = u8>) -> String {
    let h = bytes.into_iter().fold(14695981039346656037u64, |h, b| {
        (h ^ u64::from(b)).wrapping_mul(1099511628211)
    });
    format!("{h:016x}")
}
fn valid_owner(value: &str) -> bool {
    value.len() == 16 && value.bytes().all(|b| b.is_ascii_hexdigit())
}
fn asset(style: u8, s: &str) -> String {
    hash(
        b"p4-text-alpha-v2:rle4:1:"
            .iter()
            .copied()
            .chain([style])
            .chain(s.bytes()),
    )
}
#[derive(Clone)]
struct Desired {
    identity: String,
    owner: String,
    version: String,
    topic: String,
    payload: Value,
    assets: Vec<(u8, String)>,
}
#[derive(Default)]
pub(super) struct RenderBridge {
    sink: Option<RenderSink>,
    instance: String,
    peer: String,
    sequence: u64,
    owner_sequence: u64,
    recent: VecDeque<Desired>,
    last_need: Option<Instant>,
}
impl RenderBridge {
    // Compare-and-clear: an old WebView disposer cannot retire its successor.
    pub fn set_sink(&mut self, instance: &str, sink: Option<RenderSink>) -> bool {
        if !hex(instance, 32) || (sink.is_none() && self.instance != instance) {
            return false;
        }
        self.sink = sink;
        self.instance = if self.sink.is_some() {
            instance.to_owned()
        } else {
            String::new()
        };
        self.recent.clear();
        self.peer.clear();
        self.last_need = None;
        true
    }
    pub fn clear_peer(&mut self) {
        self.recent.clear();
        self.peer.clear();
        self.last_need = None;
    }
    pub fn sink(&self) -> Option<RenderSink> {
        self.sink.clone()
    }
    pub fn current(&self, f: &BubbleFrame) -> bool {
        self.sink.is_some()
            && self.instance == f.renderer_id
            && self.peer == format!("{}:{}:{}", f.board_device_id, f.connection_id, f.boot_id)
            && self.recent.iter().any(|d| {
                d.owner == f.owner
                    && d.version == f.version
                    && d.assets.iter().any(|(s, k)| *s == f.style && *k == f.id)
            })
    }
    /// Re-emit every text the bridge currently knows about. Called when the
    /// renderer (re)registers: the device side only requests pixels for texts
    /// it can still see, and its request path depends on metadata the host only
    /// sends on text updates, so without this a reboot leaves most texts with
    /// no pixels at all (local glyphs).
    pub fn resend(&self) -> Vec<Value> {
        if self.sink.is_none() {
            return Vec::new();
        }
        let mut parts = self.peer.split(':');
        let board = parts.next().unwrap_or("");
        let generation = parts.next().unwrap_or("");
        let boot = parts.next().unwrap_or("");
        if board.is_empty() || generation.is_empty() || !valid_boot(boot) {
            return Vec::new();
        }
        /* Only the newest payload per owner is still valid on the device; older
         * versions were rejected as stale_content and crowded out fresh frames. */
        let mut newest: std::collections::HashMap<String, &Desired> = std::collections::HashMap::new();
        for desired in self.recent.iter() {
            newest.insert(desired.owner.clone(), desired);
        }
        newest
            .into_values()
            .map(|d| {
                json!({"topic":d.topic,"payload":d.payload,"connectionId":generation,
                    "boardDeviceId":board,"bootId":boot,"rendererId":self.instance,
                    "owner":d.owner,"version":d.version})
            })
            .collect()
    }
    pub fn need(&mut self, p: &Value, generation: u64, board: &str) -> Vec<Value> {
        let boot = p["boot"].as_str().unwrap_or("");
        if !valid_boot(boot)
            || p["client"] != self.instance
            || self.peer != format!("{board}:{generation}:{boot}")
            || self.sink.is_none()
            || self
                .last_need
                /* The device already throttles its own need requests (80 ms);
                 * a 900 ms host throttle silently dropped most of them, so a
                 * new session's cards never got pixels and fell back to local
                 * glyphs. */
                .map(|n| n.elapsed() < Duration::from_millis(120))
                .unwrap_or(false)
        {
            return Vec::new();
        }
        let page = match p["page"].as_u64() {
            Some(n) if n < 16 => n,
            _ => return Vec::new(),
        };
        let mut events = Vec::new();
        for d in &self.recent {
            for (style, id) in &d.assets {
                if p["style"] == *style && p["id"] == *id && (*style == 2 || page == 0) {
                    events.push(json!({"topic":d.topic,"payload":d.payload,"connectionId":generation.to_string(),"boardDeviceId":board,
              "bootId":boot,"rendererId":self.instance,"owner":d.owner,"version":d.version,"need":{"style":style,"id":id,"page":page}}));
                    break;
                }
            }
        }
        if events.is_empty() {
            /* The device sends the text it actually renders. When the host's own
             * per-topic derivation cannot produce that exact string (hints, compact
             * fallbacks, subtitle pages), render it directly instead of leaving the
             * bubble on local glyphs. */
            if let (Some(text_value), Some(style), Some(owner), Some(version)) = (
                p["text"].as_str().filter(|t| !t.is_empty() && t.len() <= 512),
                p["style"].as_u64().filter(|s| (1..=4).contains(s)),
                p["owner"].as_str().filter(|o| valid_owner(o)),
                p["version"].as_str().filter(|v| valid_owner(v)),
            ) {
                events.push(json!({
                    "topic": "device/need",
                    "payload": {"text": text_value},
                    "connectionId": generation.to_string(),
                    "boardDeviceId": board,
                    "bootId": boot,
                    "rendererId": self.instance,
                    "owner": owner,
                    "version": version,
                    "need": {"style": style, "id": p["id"], "page": 0},
                }));
            }
        }
        if !events.is_empty() {
            self.last_need = Some(Instant::now());
        }
        events
    }
    pub fn current_wire(&self, p: &Value, generation: u64, board: &str, boot: &str) -> bool {
        self.sink.is_some()
            && p["client"] == self.instance
            && p["boot"] == boot
            && self.peer == format!("{board}:{generation}:{boot}")
            && self.recent.iter().any(|d| {
                p["owner"] == d.owner
                    && p["version"] == d.version
                    && d.assets
                        .iter()
                        .any(|(style, key)| p["style"] == *style && p["id"] == *key)
            })
    }
    pub fn metadata(&self, _topic: &str, boot: &str) -> Value {
        if self.sink.is_none() || !valid_boot(boot) {
            return Value::Bool(false);
        }
        json!({"boot":boot,"client":self.instance,"owners":self.recent.iter()
            .map(|d|json!({"id":d.owner,"version":d.version,"assets":d.assets.iter().map(|(style,id)|json!({"style":style,"id":id})).collect::<Vec<_>>()})).collect::<Vec<_>>()})
    }
    pub fn observe(
        &mut self,
        topic: &str,
        payload: &Value,
        generation: u64,
        board: &str,
        boot: &str,
    ) -> Vec<Value> {
        if !is_text_topic(topic) || !valid_boot(boot) || self.sink.is_none() {
            return Vec::new();
        }
        let peer = format!("{board}:{generation}:{boot}");
        if self.peer != peer {
            self.recent.clear();
            self.peer = peer;
        }
        let mut candidates = Vec::new();
        if topic == "session/current" {
            if let Some(items) = payload["sessions"].as_array() {
                let items = &items[..items.len().min(8)];
                let selected = items
                    .iter()
                    .position(|i| i["id"] == payload["sessionId"])
                    .unwrap_or(0);
                let start = selected
                    .saturating_sub(1)
                    .min(items.len().saturating_sub(3));
                for item in items.iter().skip(start).take(3) {
                    let raw_id = item.get("id").and_then(Value::as_str).unwrap_or("");
                    let id = text(item.get("id"), 128);
                    if id.is_empty() || raw_id.len() > 128 || raw_id.contains('\0') {
                        continue;
                    }
                    let title = text(item.get("title"), 191);
                    let content = text(item.get("content"), 383);
                    let state = text(item.get("state"), 383);
                    candidates.push((format!("session:{id}"),json!({"sessionId":id,"sessions":[{"id":id,"title":title,"content":content,"state":state}]})));
                }
            }
        } else {
            let mut p = serde_json::Map::new();
            for k in [
                "displayTitle",
                "sessionName",
                "title",
                "agent",
                "displayContent",
                "text",
                "message",
                "speech",
                "summary",
                "statusText",
                "name",
                "role",
                "sessionId",
                "conversationId",
            ] {
                if payload[k].is_string() {
                    p.insert(
                        k.into(),
                        Value::String(text(
                            payload.get(k),
                            if matches!(k, "name" | "role") {
                                63
                            } else {
                                511
                            },
                        )),
                    );
                }
            }
            let raw_owner = payload
                .get("sessionId")
                .or_else(|| payload.get("conversationId"))
                .and_then(Value::as_str)
                .unwrap_or("");
            if raw_owner.len() > 128 || raw_owner.contains('\0') {
                return Vec::new();
            }
            let owner = text(
                payload
                    .get("sessionId")
                    .or_else(|| payload.get("conversationId")),
                128,
            );
            candidates.push((format!("{}:{owner}", scope(topic)), Value::Object(p)));
        }
        let old: Vec<_> = self.recent.iter().cloned().collect();
        let replacing: Vec<_> = candidates
            .iter()
            .map(|(identity, _)| identity.clone())
            .collect();
        self.recent.retain(|d| {
            if topic == "session/current" {
                d.topic != topic
            } else {
                !replacing.contains(&d.identity)
            }
        });
        let mut events = Vec::new();
        for (identity, payload) in candidates {
            let owner = if let Some(d) = old.iter().find(|d| d.identity == identity) {
                d.owner.clone()
            } else {
                match self.owner_sequence.checked_add(1) {
                    Some(n) => {
                        self.owner_sequence = n;
                        format!("{n:016x}")
                    }
                    None => {
                        self.clear_peer();
                        return Vec::new();
                    }
                }
            };
            let version = if let Some(d) = old
                .iter()
                .find(|d| d.identity == identity && d.topic == topic && d.payload == payload)
            {
                d.version.clone()
            } else {
                match self.sequence.checked_add(1) {
                    Some(n) => {
                        self.sequence = n;
                        format!("{n:016x}")
                    }
                    None => {
                        self.clear_peer();
                        return Vec::new();
                    }
                }
            };
            /* Live transcripts are drawn by the device itself; rendering a pixel
             * frame per partial saturated the link and dropped the connection. */
            if topic == "ui/conversation" && payload["state"] == "listening" {
                return Vec::new();
            }
            let assets = assets_for(topic, &payload);
            let event = json!({"topic":topic,"payload":payload,"connectionId":generation.to_string(),"boardDeviceId":board,
                "bootId":boot,"rendererId":self.instance,"owner":owner,"version":version});
            if !old.iter().any(|d| d.owner == owner && d.version == version) {
                events.push(event);
            }
            self.recent.push_back(Desired {
                identity,
                owner,
                version,
                topic: topic.into(),
                payload,
                assets,
            });
        }
        for d in old {
            if !self.recent.iter().any(|n| n.owner == d.owner) {
                events.push(json!({"retired":true,"owner":d.owner,"version":d.version,"topic":topic,
            "connectionId":generation.to_string(),"boardDeviceId":board,"bootId":boot,"rendererId":self.instance}));
            }
        }
        while self.recent.len() > 12 {
            self.recent.pop_front();
        }
        events
    }
}
fn assets_for(topic: &str, p: &Value) -> Vec<(u8, String)> {
    let mut out = Vec::new();
    let mut add = |style: u8, s: String| {
        if !s.is_empty() {
            out.push((style, asset(style, &s)));
        }
    };
    if topic == "session/current" {
        if let Some(a) = p["sessions"].as_array() {
            for s in a {
                add(1, text(s.get("title"), 191));
                add(3, text(
                    if s["content"].as_str().unwrap_or("").is_empty() { s.get("state") } else { s.get("content") },
                    511,
                ));
                // The compact bubble falls back to the title when the body is empty.
                add(3, text(s.get("title"), 511));
                add(
                    4,
                    if s["content"].as_str().unwrap_or("").is_empty() {
                        text(s.get("state"), 383)
                    } else {
                        text(s.get("content"), 383)
                    },
                );
            }
        }
    } else if topic == "ui/conversation" {
        let name = if p["name"].as_str().unwrap_or("").is_empty() {
            "宠物".into()
        } else {
            text(p.get("name"), 63)
        };
        add(1, name.clone());
        add(
            1,
            if p["role"] == "user" {
                "你".into()
            } else {
                name
            },
        );
        add(2, text(p.get("text"), 511));
        add(3, text(p.get("text"), 511));
        add(3, text(p.get("name"), 511));
        // Status hints the device renders as the bubble body.
        for hint in ["我在听", "想一想", "准备回答", "准备中"] {
            add(2, hint.to_string());
        }
    } else if topic == "audio/play_subtitle" {
        add(2, text(p.get("text"), 511));
        add(3, text(p.get("text"), 511));
    } else {
        let title = ["displayTitle", "sessionName", "title", "agent"]
            .iter()
            .find_map(|k| p[*k].as_str().filter(|s| !s.is_empty()))
            .unwrap_or("pet");
        add(1, text(Some(&Value::String(title.into())), 127));
        let body = [
            "displayContent",
            "text",
            "message",
            "speech",
            "summary",
            "statusText",
        ]
        .iter()
        .find_map(|k| p[*k].as_str().filter(|s| !s.is_empty()))
        .unwrap_or("");
        for (style, limit) in [(4, 511), (3, 511)] {
            add(style, text(Some(&Value::String(body.into())), limit));
        }
        if p["sessionId"].as_str().is_some() {
            add(4, text(Some(&Value::String(body.into())), 383));
        }
    }
    out
}
#[derive(Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub(crate) struct BubbleFrame {
    pub board_device_id: String,
    pub connection_id: String,
    pub boot_id: String,
    pub renderer_id: String,
    pub owner: String,
    pub version: String,
    pub id: String,
    pub style: u8,
    pub page_sizes: Vec<usize>,
    pub page_index: u8,
    pub page_count: u8,
    pub checksum: String,
    pub data: String,
}
pub(super) fn validate_frame(f: &BubbleFrame, bytes: &[u8]) -> Result<(), String> {
    if !matches!(f.style, 1..=4)
        || !hex(&f.id, 16)
        || !hex(&f.checksum, 8)
        || !valid_boot(&f.boot_id)
        || !hex(&f.renderer_id, 32)
        || !hex(&f.owner, 16)
        || !hex(&f.version, 16)
        || f.connection_id
            .parse::<u64>()
            .ok()
            .filter(|n| *n > 0)
            .is_none()
        || bytes.is_empty()
        || bytes.len() > MAX_BYTES
        || f.page_sizes != vec![bytes.len()]
        || f.page_count == 0
        || f.page_count > 16
        || f.page_index >= f.page_count
        || (f.style != 2 && (f.page_count != 1 || f.page_index != 0))
    {
        return Err("BUBBLE_FALLBACK: invalid metadata".into());
    }
    let pixels = match f.style {
        1 => 474 * 38,
        2 | 4 => 496 * 54,
        _ => 348 * 38,
    };
    if bytes.iter().map(|b| usize::from(b & 15) + 1).sum::<usize>() != pixels {
        return Err("BUBBLE_FALLBACK: invalid alpha runs".into());
    }
    let sum = bytes.iter().fold(2166136261u32, |h, b| {
        (h ^ u32::from(*b)).wrapping_mul(16777619)
    });
    if format!("{sum:08x}") != f.checksum {
        return Err("BUBBLE_FALLBACK: checksum".into());
    }
    Ok(())
}
pub(crate) struct WorkerLease(Arc<AtomicBool>);
impl WorkerLease {
    pub fn acquire(gate: Arc<AtomicBool>) -> Result<Self, String> {
        gate.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| "BUBBLE_BUSY: worker occupied")?;
        Ok(Self(gate))
    }
}
impl Drop for WorkerLease {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}
pub(super) fn wire_rate(baud: u32) -> usize {
    /* Bubble frames were capped at 12 KB/s regardless of link speed, so a
     * 4M-baud link spent its whole budget sleeping between chunks (~320 ms
     * median per frame) and lost the race against the local fallback. Keep the
     * rate below the writer's 64 B/250 us slice ceiling (~256 KB/s) but stop
     * throttling to 3% of the link. */
    /* Enough to beat the local-fallback window, low enough that a burst of
     * chunk frames cannot overflow the device UART RX buffer (which showed up
     * as length-mismatch frames and then a write timeout that dropped the
     * link). */
    ((baud as usize / 10) * 16 / 100).clamp(1, 131072)
}
pub(super) fn estimated_wire_bytes(bytes: usize) -> usize {
    bytes.div_ceil(3) * 4 + bytes.div_ceil(CHUNK_BYTES) * 320 + 2048
}

#[cfg(test)]
mod tests {
    use super::*;
    fn ready() -> RenderBridge {
        let mut b = RenderBridge::default();
        assert!(b.set_sink(&"a".repeat(32), Some(Arc::new(|_| {}))));
        b
    }
    fn frame(e: &Value) -> BubbleFrame {
        let bytes = vec![15u8; 496 * 54 / 16];
        let sum = bytes.iter().fold(2166136261u32, |h, b| {
            (h ^ u32::from(*b)).wrapping_mul(16777619)
        });
        BubbleFrame {
            board_device_id: e["boardDeviceId"].as_str().unwrap().into(),
            connection_id: e["connectionId"].as_str().unwrap().into(),
            boot_id: e["bootId"].as_str().unwrap().into(),
            renderer_id: e["rendererId"].as_str().unwrap().into(),
            owner: e["owner"].as_str().unwrap().into(),
            version: e["version"].as_str().unwrap().into(),
            id: asset(4, "body"),
            style: 4,
            page_sizes: vec![bytes.len()],
            page_index: 0,
            page_count: 1,
            checksum: format!("{sum:08x}"),
            data: String::new(),
        }
    }
    #[test]
    fn incremental_same_topic_owners_do_not_erase_each_other() {
        let mut b = ready();
        let a = b.observe(
            "speech/text",
            &json!({"sessionId":"a","text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let c = b.observe(
            "speech/text",
            &json!({"sessionId":"b","text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert!(b.current(&frame(&a[0])) && b.current(&frame(&c[0])));
        b.observe(
            "speech/text",
            &json!({"sessionId":"a","text":"new"}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert!(!b.current(&frame(&a[0])) && b.current(&frame(&c[0])));
        assert_eq!(
            b.metadata("speech/text", "1234567890abcdef")["owners"]
                .as_array()
                .unwrap()
                .len(),
            2
        );
    }
    #[test]
    fn same_connection_boot_change_and_aba_versions_never_revive() {
        let mut b = ready();
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let old = frame(&e[0]);
        b.observe(
            "speech/text",
            &json!({"text":"different"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let current = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert!(!b.current(&old));
        assert_ne!(old.version, current[0]["version"].as_str().unwrap());
        b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "ffffffffffffffff",
        );
        assert!(!b.current(&old));
    }
    #[test]
    fn high_u64_connections_are_decimal_strings_and_old_disposer_is_ignored() {
        let mut b = ready();
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            u64::MAX,
            "board",
            "1234567890abcdef",
        );
        assert_eq!(e[0]["connectionId"], u64::MAX.to_string());
        assert!(b.set_sink(&"b".repeat(32), Some(Arc::new(|_| {}))));
        assert!(!b.set_sink(&"a".repeat(32), None));
        assert!(b.sink().is_some());
        assert!(!b.current(&frame(&e[0])));
    }
    #[test]
    fn descriptors_are_bounded_and_retirement_does_not_replay_old_peer() {
        let mut b = ready();
        for n in 0..100 {
            b.observe(
                "speech/text",
                &json!({"sessionId":n.to_string(),"text":"x".repeat(10000)}),
                1,
                "board",
                "1234567890abcdef",
            );
        }
        assert_eq!(b.recent.len(), 12);
        assert!(b
            .recent
            .iter()
            .all(|d| d.payload["text"].as_str().unwrap().len() == 511));
        b.clear_peer();
        assert!(b.recent.is_empty());
    }
    #[test]
    fn alpha_geometry_single_page_checksum_and_limits_fail_closed() {
        let mut b = ready();
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let mut f = frame(&e[0]);
        let bytes = vec![15u8; 496 * 54 / 16];
        assert!(validate_frame(&f, &bytes).is_ok());
        f.page_sizes = vec![usize::MAX];
        assert!(validate_frame(&f, &bytes).is_err());
        f = frame(&e[0]);
        f.checksum = "00000000".into();
        assert!(validate_frame(&f, &bytes).is_err());
        f = frame(&e[0]);
        f.page_count = 2;
        assert!(validate_frame(&f, &bytes).is_err());
        assert!(validate_frame(&frame(&e[0]), &vec![255; MAX_BYTES + 1]).is_err());
    }
    #[test]
    fn final_wire_token_rejects_warm_boot_renderer_and_content_changes() {
        let mut b = ready();
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let f = frame(&e[0]);
        let mut p = json!({"client":f.renderer_id,"boot":f.boot_id,"owner":f.owner,"version":f.version,"style":4,"id":f.id});
        assert!(b.current_wire(&p, 1, "board", "1234567890abcdef"));
        assert!(!b.current_wire(&p, 1, "board", "ffffffffffffffff"));
        assert!(!b.current_wire(&p, 2, "board", "1234567890abcdef"));
        p["version"] = json!("f".repeat(16));
        assert!(!b.current_wire(&p, 1, "board", "1234567890abcdef"));
    }
    #[test]
    fn device_missing_requires_current_peer_asset_and_is_rate_bounded() {
        let mut b = ready();
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let f = frame(&e[0]);
        let mut p = json!({"boot":f.boot_id,"client":f.renderer_id,"id":f.id,"style":4,"page":0});
        assert!(b.need(&p, 2, "board").is_empty());
        p["page"] = json!(1);
        assert!(b.need(&p, 1, "board").is_empty());
        p["page"] = json!(0);
        let events = b.need(&p, 1, "board");
        assert_eq!(events.len(), 1);
        assert_eq!(events[0]["version"], f.version);
        assert!(b.need(&p, 1, "board").is_empty());
    }
    #[test]
    fn one_display_owner_follows_state_to_speech_without_stale_parallel_tokens() {
        let mut b = ready();
        let old = b.observe(
            "state/working",
            &json!({"sessionId":"a","text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let f = frame(&old[0]);
        let new = b.observe(
            "speech/text",
            &json!({"sessionId":"a","text":"new"}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert_eq!(new[0]["owner"], f.owner);
        assert!(!b.current(&f));
        assert_eq!(
            b.metadata("state/done", "1234567890abcdef")["owners"]
                .as_array()
                .unwrap()
                .len(),
            1
        );
    }
    #[test]
    fn worker_admission_is_single_and_drop_releases_on_failure() {
        let gate = Arc::new(AtomicBool::new(false));
        let held = WorkerLease::acquire(gate.clone()).unwrap();
        assert!(WorkerLease::acquire(gate.clone()).is_err());
        drop(held);
        assert!(WorkerLease::acquire(gate).is_ok());
    }
    #[test]
    fn baud_budget_declines_assets_that_cannot_finish_and_has_a_fixed_ceiling() {
        assert_eq!(wire_rate(115200), 1843);
        assert_eq!(wire_rate(4_000_000), 64000);
        assert!(estimated_wire_bytes(MAX_BYTES) > wire_rate(115200) * 10);
        assert!(estimated_wire_bytes(MAX_BYTES) < wire_rate(4_000_000) * 10);
    }
    #[test]
    fn manifest_is_the_complete_bounded_authority_snapshot_across_roles() {
        let mut b = ready();
        b.observe(
            "speech/text",
            &json!({"sessionId":"a","text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        b.observe(
            "ui/conversation",
            &json!({"conversationId":"b","text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let m = b.metadata("ui/conversation", "1234567890abcdef");
        assert_eq!(m["owners"].as_array().unwrap().len(), 2);
    }
}
