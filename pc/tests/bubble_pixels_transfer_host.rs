//! Host fixture: the ACTUAL manager transfer method is extracted by the runner.
//! Only status/wire/ACK delivery are fake; no Tauri/serial/SDK is linked.
mod transport {
    #[path = "production_bubble.rs"]
    mod bubble_pixels;
    use base64::Engine;
    use bubble_pixels::BubbleFrame;
    use serde_json::{json, Value};
    use std::{
        sync::{
            atomic::{AtomicUsize, Ordering},
            mpsc, Arc, Mutex,
        },
        thread,
        time::{Duration, Instant},
    };
    struct DeviceResponseWaiter {
        request_id: String,
        response_topic: String,
        sender: mpsc::Sender<Value>,
    }
    struct Status {
        capabilities: Value,
        baud_rate: u32,
    }
    struct UsbSerialManager {
        bubble_renderer: Arc<Mutex<bubble_pixels::RenderBridge>>,
        asset_transfer_guard: Arc<Mutex<()>>,
        device_response_waiters: Arc<Mutex<Vec<DeviceResponseWaiter>>>,
        media_requests_pending: Arc<AtomicUsize>,
        sent: Arc<Mutex<Vec<Value>>>,
        mode: u8,
        baud: u32,
    }
    impl UsbSerialManager {
        fn status(&self) -> Status {
            Status {
                capabilities: json!({"bubblePixels":bubble_pixels::SCHEMA}),
                baud_rate: self.baud,
            }
        }
        fn remove_device_response_waiter(&self, id: &str, topic: &str) {
            self.device_response_waiters
                .lock()
                .unwrap()
                .retain(|w| w.request_id != id || w.response_topic != topic);
        }
        fn send_inner(
            &self,
            topic: &str,
            p: &Value,
            _flush: bool,
            board: Option<&str>,
            connection: Option<u64>,
            _slice: Option<usize>,
        ) -> Result<(), String> {
            assert_eq!(topic, "ui/text-pixels");
            assert_eq!(board, Some("board"));
            assert_eq!(connection, Some(1));
            if !self
                .bubble_renderer
                .lock()
                .unwrap()
                .current_wire(p, 1, "board", "1234567890abcdef")
            {
                return Err("retired".into());
            }
            self.sent.lock().unwrap().push(p.clone());
            if self.mode == 2 {
                return Ok(());
            }
            let mut reply = p.clone();
            reply["ok"] = json!(true);
            reply["offset"] = if p["phase"] == "chunk" {
                json!(
                    p["offset"].as_u64().unwrap()
                        + base64::engine::general_purpose::STANDARD
                            .decode(p["data"].as_str().unwrap())
                            .unwrap()
                            .len() as u64
                )
            } else {
                json!(0)
            };
            if self.mode == 1 {
                reply["owner"] = json!("f".repeat(16));
            }
            if self.mode == 3 {
                reply["cached"] = json!(true);
            }
            let sender = self
                .device_response_waiters
                .lock()
                .unwrap()
                .iter()
                .find(|w| w.request_id == p["requestId"].as_str().unwrap())
                .unwrap()
                .sender
                .clone();
            let guard = self.asset_transfer_guard.clone();
            let pending = self.media_requests_pending.clone();
            let bridge = self.bubble_renderer.clone();
            let mode = self.mode;
            thread::spawn(move || {
                // ACK is delivered only AFTER the per-frame guard is released.
                // Holding it across recv_timeout would make the real method fail.
                let free = guard.lock().unwrap();
                drop(free);
                if mode == 4 {
                    pending.store(1, Ordering::SeqCst);
                }
                if mode == 5 {
                    bridge.lock().unwrap().clear_peer();
                }
                let _ = sender.send(reply);
            });
            Ok(())
        }
    }
    include!("production_transfer.inc");
    fn fixture(mode: u8) -> (UsbSerialManager, BubbleFrame) {
        let mut b = bubble_pixels::RenderBridge::default();
        b.set_sink(&"a".repeat(32), Some(Arc::new(|_| {})));
        let event = b
            .observe(
                "speech/text",
                &json!({"text":"body"}),
                1,
                "board",
                "1234567890abcdef",
            )
            .remove(0);
        let bytes = vec![15u8; 496 * 54 / 16];
        let hash = bytes.iter().fold(2166136261u32, |h, b| {
            (h ^ u32::from(*b)).wrapping_mul(16777619)
        });
        let mut h = 14695981039346656037u64;
        for b in b"p4-text-alpha-v2:rle4:1:"
            .iter()
            .copied()
            .chain([4])
            .chain(b"body".iter().copied())
        {
            h = (h ^ u64::from(b)).wrapping_mul(1099511628211);
        }
        let f = BubbleFrame {
            board_device_id: "board".into(),
            connection_id: "1".into(),
            boot_id: "1234567890abcdef".into(),
            renderer_id: "a".repeat(32),
            owner: event["owner"].as_str().unwrap().into(),
            version: event["version"].as_str().unwrap().into(),
            id: format!("{h:016x}"),
            style: 4,
            page_sizes: vec![bytes.len()],
            page_count: 1,
            page_index: 0,
            checksum: format!("{hash:08x}"),
            data: base64::engine::general_purpose::STANDARD.encode(bytes),
        };
        (
            UsbSerialManager {
                bubble_renderer: Arc::new(Mutex::new(b)),
                asset_transfer_guard: Arc::new(Mutex::new(())),
                device_response_waiters: Arc::new(Mutex::new(Vec::new())),
                media_requests_pending: Arc::new(AtomicUsize::new(0)),
                sent: Arc::new(Mutex::new(Vec::new())),
                mode,
                baud: 4_000_000,
            },
            f,
        )
    }
    #[test]
    fn actual_transfer_ready_chunk_end_releases_guard_before_every_ack() {
        let (m, f) = fixture(0);
        m.send_bubble_pixels(f).unwrap();
        let s = m.sent.lock().unwrap();
        assert_eq!(s.first().unwrap()["phase"], "begin");
        assert_eq!(s.last().unwrap()["phase"], "end");
        assert_eq!(s.len(), 6);
        assert!(m.device_response_waiters.lock().unwrap().is_empty());
    }
    #[test]
    fn cached_ready_skips_all_data_and_end() {
        let (m, f) = fixture(3);
        m.send_bubble_pixels(f).unwrap();
        assert_eq!(m.sent.lock().unwrap().len(), 1);
        assert!(m.device_response_waiters.lock().unwrap().is_empty());
    }
    #[test]
    fn stale_ack_and_unknown_ack_have_no_replay_or_waiter_leak() {
        for mode in [1, 2] {
            let (m, f) = fixture(mode);
            assert!(m.send_bubble_pixels(f).is_err());
            assert_eq!(m.sent.lock().unwrap().len(), 1);
            assert!(m.device_response_waiters.lock().unwrap().is_empty());
        }
    }
    #[test]
    fn media_or_owner_retirement_between_frames_stops_the_asset() {
        for mode in [4, 5] {
            let (m, f) = fixture(mode);
            assert!(m.send_bubble_pixels(f).is_err());
            assert_eq!(m.sent.lock().unwrap().len(), 1);
            assert!(m.device_response_waiters.lock().unwrap().is_empty());
        }
    }
    #[test]
    fn bandwidth_decline_is_before_the_first_frame() {
        let (mut m, mut f) = fixture(0);
        m.baud = 115200;
        let bytes = vec![0xf0u8; 496 * 54];
        f.page_sizes = vec![bytes.len()];
        f.data = base64::engine::general_purpose::STANDARD.encode(&bytes);
        f.checksum = format!(
            "{:08x}",
            bytes.iter().fold(2166136261u32, |h, b| (h ^ u32::from(*b))
                .wrapping_mul(16777619))
        );
        assert!(m.send_bubble_pixels(f).is_err());
        assert!(m.sent.lock().unwrap().is_empty());
    }
}
