use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::VecDeque;
use std::sync::Arc;

pub(super) type RenderSink = Arc<dyn Fn(Value) + Send + Sync>;

pub(super) fn is_text_topic(topic: &str) -> bool {
    topic.starts_with("state/") || matches!(topic,
        "session/current" | "speech/text" | "ui/conversation" | "audio/play_subtitle")
}

#[derive(Default)]
pub(super) struct RenderBridge {
    sink: Option<RenderSink>,
    generation: u64,
    recent: VecDeque<Value>,
}

impl RenderBridge {
    pub fn set_sink(&mut self, sink: Option<RenderSink>) {
        self.sink = sink;
        if let Some(sink) = &self.sink {
            for event in &self.recent { sink(event.clone()); }
        }
    }

    pub fn observe(&mut self, topic: &str, payload: &Value, generation: u64, board: &str) {
        if !is_text_topic(topic) { return; }
        if self.generation != generation {
            self.generation = generation;
            self.recent.clear();
        }
        let event = json!({"topic":topic,"payload":payload,"connectionId":generation,"boardDeviceId":board});
        // Retain bounded snapshots until the main WebView's renderer is ready.
        self.recent.retain(|old| old["topic"] != topic);
        if self.recent.len() == 16 { self.recent.pop_front(); }
        self.recent.push_back(event.clone());
        if let Some(sink) = &self.sink { sink(event); }
    }
}

#[derive(Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct BubbleFrame {
    pub board_device_id: String,
    pub connection_id: u64,
    pub id: String,
    pub style: u8,
    pub page_sizes: Vec<usize>,
    pub checksum: String,
    pub data: String,
}

pub(super) fn validate_frame(frame: &BubbleFrame, bytes: &[u8]) -> Result<(), String> {
    if !matches!(frame.style, 1..=4) || frame.id.len() != 16 ||
        !frame.id.bytes().all(|c| c.is_ascii_hexdigit()) || frame.checksum.len() != 8 ||
        frame.page_sizes.is_empty() || frame.page_sizes.len() > if frame.style == 2 {16} else {1} ||
        bytes.is_empty() || bytes.len() > 128 * 1024 {
        return Err("Invalid bubble image metadata".into());
    }
    let pixels = match frame.style {1=>474*38, 2|4=>496*54, _=>348*38};
    let mut offset = 0usize;
    for size in &frame.page_sizes {
        let end = offset.checked_add(*size).filter(|end| *end <= bytes.len())
            .ok_or("Invalid bubble image page")?;
        if *size == 0 || bytes[offset..end].iter().map(|b| (b & 15) as usize + 1).sum::<usize>() != pixels {
            return Err("Invalid bubble image alpha runs".into());
        }
        offset = end;
    }
    let hash = bytes.iter().fold(2166136261u32, |hash, byte| (hash ^ *byte as u32).wrapping_mul(16777619));
    if offset != bytes.len() || format!("{hash:08x}") != frame.checksum {
        return Err("Bubble image checksum mismatch".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    fn frame(bytes: &[u8]) -> BubbleFrame {
        let hash = bytes.iter().fold(2166136261u32,
            |hash, byte| (hash ^ *byte as u32).wrapping_mul(16777619));
        BubbleFrame { board_device_id: "board".into(), connection_id: 3,
            id: "41c0fcd4504ad4c3".into(), style: 2, page_sizes: vec![bytes.len()],
            checksum: format!("{hash:08x}"), data: String::new() }
    }

    #[test]
    fn page_geometry_checksum_and_metadata_are_validated_before_transmission() {
        let bytes = vec![15; 496 * 54 / 16];
        assert!(validate_frame(&frame(&bytes), &bytes).is_ok());
        let mut invalid = frame(&bytes);
        invalid.page_sizes = vec![usize::MAX, 1];
        assert!(validate_frame(&invalid, &bytes).is_err());
        invalid = frame(&bytes); invalid.style = 0;
        assert!(validate_frame(&invalid, &bytes).is_err());
        invalid = frame(&bytes); invalid.checksum = "00000000".into();
        assert!(validate_frame(&invalid, &bytes).is_err());
        let mut broken = bytes.clone(); broken[0] = 0;
        assert!(validate_frame(&frame(&broken), &broken).is_err());
        assert!(validate_frame(&frame(&[]), &[]).is_err());
    }

    #[test]
    fn readiness_replays_latest_snapshots_but_not_previous_connections_or_audio() {
        let received = Arc::new(Mutex::new(Vec::<Value>::new()));
        let mut bridge = RenderBridge::default();
        bridge.observe("speech/text", &json!({"text":"old"}), 1, "a");
        bridge.observe("speech/text", &json!({"text":"new"}), 1, "a");
        bridge.observe("session/current", &json!({"sessions":[]}), 2, "b");
        bridge.observe("audio/play_chunk", &json!({"data":"not-a-text-image"}), 2, "b");
        let events = received.clone();
        bridge.set_sink(Some(Arc::new(move |event| events.lock().unwrap().push(event))));
        let events = received.lock().unwrap();
        assert_eq!(events.len(), 1);
        assert_eq!(events[0]["connectionId"], 2);
        assert_eq!(events[0]["boardDeviceId"], "b");
    }
}
