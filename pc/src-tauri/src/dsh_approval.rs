/*
 * [Input] DSH bridge plugin approval SSE events and ESP32-P4 USB approval decisions.
 * [Output] Single-request approval relay: plugin -> USB approval/request -> device
 *          decision -> plugin /approval/decision, with bounded timeout and no
 *          no competing native UI after a request has been claimed.
 * [Pos] Host-side R5 approval relay for the DeepSeek Harness channel.
 * [Sync] If this contract changes, update docs/dsh-integration-contract.md and
 *         firmware/protocol.md.
 */

use crate::dsh_auth::{self, Capability};
use std::collections::HashMap;
use std::io::{BufRead, BufReader, Read};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Mutex, OnceLock};
use std::thread;
use std::time::Duration;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use serde_json::{json, Value};

use crate::usb_serial::UsbSerialManager;

const APPROVAL_TIMEOUT: Duration = Duration::from_secs(30);
const MAX_PENDING: usize = 16;
static WORKERS: AtomicUsize = AtomicUsize::new(0);
static MODAL: Mutex<()> = Mutex::new(());
struct WorkerPermit;
impl WorkerPermit {
    fn acquire() -> Option<Self> {
        WORKERS
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| {
                (n < MAX_PENDING).then_some(n + 1)
            })
            .ok()
            .map(|_| Self)
    }
}
impl Drop for WorkerPermit {
    fn drop(&mut self) {
        WORKERS.fetch_sub(1, Ordering::AcqRel);
    }
}
#[derive(Debug, PartialEq)]
enum Terminal {
    Decision(String),
    Cancelled,
    Expired,
    Disconnected,
}
struct Pending {
    sender: mpsc::Sender<Terminal>,
    epoch: String,
    usb: UsbSerialManager,
    board: String,
    generation: u64,
    presented: bool,
}
fn pending_decisions() -> &'static Mutex<HashMap<String, Pending>> {
    static PENDING: OnceLock<Mutex<HashMap<String, Pending>>> = OnceLock::new();
    PENDING.get_or_init(|| Mutex::new(HashMap::new()))
}
fn take_terminal(id: &str, event: Terminal) {
    let entry = pending_decisions()
        .lock()
        .ok()
        .and_then(|mut p| p.remove(id));
    if let Some(p) = entry {
        let _ = p.sender.send(event);
    }
}
fn drain_epoch(epoch: &str) {
    let entries = if let Ok(mut p) = pending_decisions().lock() {
        let ids: Vec<_> = p
            .iter()
            .filter(|(_, v)| v.epoch == epoch)
            .map(|(id, _)| id.clone())
            .collect();
        ids.into_iter()
            .filter_map(|id| p.remove(&id).map(|v| (id, v)))
            .collect::<Vec<_>>()
    } else {
        Vec::new()
    };
    for (id, p) in entries {
        let _ = p.sender.send(Terminal::Disconnected);
        let _ = p.usb.send_approval_bound(
            &p.board,
            p.generation,
            "approval/cancel",
            &json!({"requestId":id}),
        );
    }
}

/// Convert one plugin `approval` SSE payload into the bounded USB payload. The
/// device only needs presentation data; requestId is the correlation key.
fn device_request_payload(event: &Value) -> Option<Value> {
    let request_id = event
        .get("requestId")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|value| !value.is_empty() && value.len() <= 63)?;
    let agent = event
        .get("agent")
        .or_else(|| event.get("agentId"))
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .unwrap_or("DeepSeek Harness");
    let reason = event
        .get("displayReason")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .or_else(|| {
            event
                .get("reason")
                .and_then(Value::as_str)
                .map(str::trim)
                .filter(|value| !value.is_empty())
        })
        .unwrap_or("");
    let request = json!({
        "requestId": request_id,
        "agent": agent,
        "tool": event.get("tool").and_then(Value::as_str).unwrap_or(""),
        "reason": reason,
        "sessionId": event.get("sessionId").and_then(Value::as_str).unwrap_or(""),
        "expiresAtMs": event.get("expiresAtMs").and_then(Value::as_u64).unwrap_or(0),
        "neverExpires": event.get("neverExpires").and_then(Value::as_bool).unwrap_or(false),
        "timeoutMs": event.get("timeoutMs").and_then(Value::as_u64).unwrap_or(0),
    });
    Some(request)
}

fn parse_sse_block(_event: &str, data: &str) -> Option<Value> {
    serde_json::from_str::<Value>(data.trim()).ok()
}

/// Map compact DSH session events to the same `state/<agent>` topic the other
/// agents publish, so the P4 can follow DSH status without a second channel.
/// Approval events are handled separately and must not be sent as state.
fn device_state_payload(event: &str, payload: &Value) -> Option<Value> {
    let session_id = payload
        .get("sessionId")
        .and_then(Value::as_str)
        .or_else(|| {
            payload
                .get("session")
                .and_then(|session| session.get("id"))
                .and_then(Value::as_str)
        })
        .unwrap_or_default();
    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_millis() as u64)
        .unwrap_or(0);
    let title = payload
        .get("session")
        .and_then(|session| session.get("title"))
        .and_then(Value::as_str)
        .or_else(|| payload.get("title").and_then(Value::as_str))
        .unwrap_or_default();
    let (state, status_text, event_name) = match event {
        "session" => {
            let running = payload
                .get("session")
                .and_then(|session| session.get("status"))
                .and_then(Value::as_str)
                == Some("running");
            (
                if running { "working" } else { "idle" },
                if running {
                    "工作中"
                } else {
                    "休息中"
                },
                "session",
            )
        }
        "turn" => match payload.get("phase").and_then(Value::as_str) {
            Some("start") => ("working", "工作中", "turn/start"),
            Some("end") => ("done", "已完成", "turn/end"),
            _ => return None,
        },
        "tool" => match payload.get("phase").and_then(Value::as_str) {
            Some("start") => ("tool_running", "调用工具", "tool/start"),
            Some("end") => ("working", "处理中", "tool/end"),
            _ => return None,
        },
        // Title updates are metadata, not lifecycle transitions. Session
        // discovery retains titles separately; do not turn an active pet idle.
        "session-title" => return None,
        _ => return None,
    };
    let mut output = serde_json::Map::new();
    output.insert("state".into(), Value::String(state.into()));
    output.insert("status".into(), Value::String(state.into()));
    output.insert("statusText".into(), Value::String(status_text.into()));
    output.insert("event".into(), Value::String(event_name.into()));
    output.insert("tsMs".into(), Value::Number(now_ms.into()));
    if !session_id.is_empty() {
        output.insert("sessionId".into(), Value::String(session_id.into()));
    }
    if !title.is_empty() {
        output.insert("title".into(), Value::String(title.into()));
        output.insert("sessionTitle".into(), Value::String(title.into()));
    }
    Some(Value::Object(output))
}

fn forward_state_event(usb: &UsbSerialManager, event: &str, payload: &Value) {
    // Approval audit events are not device status; the answerer/relay owns them.
    if event.starts_with("approval") {
        return;
    }
    if let Some(state_payload) = device_state_payload(event, payload) {
        if let Err(error) = usb.send_state("deepseek-harness", &state_payload) {
            eprintln!("[dsh-approval] state forward failed: {error}");
        }
    }
}

/// Parse one SSE line into `(event, data)` state. Returns true when the blank
/// separator line completes an event and the caller should dispatch it.
fn consume_sse_line(line: &str, event: &mut String, data: &mut String) -> bool {
    if line.is_empty() {
        return true;
    }
    if let Some(value) = line.strip_prefix("event:") {
        *event = value.trim().to_string();
    } else if let Some(value) = line.strip_prefix("data:") {
        if !data.is_empty() {
            data.push('\n');
        }
        data.push_str(value.trim_start());
    }
    false
}

fn post_decision(cap: &Capability, event: &Value, decision: &str) -> Result<(), String> {
    let receipt = dsh_auth::post(
        cap,
        "/approval/decision",
        &json!({
            "requestId":event["requestId"], "decision":decision, "epoch":cap.epoch,
            "sessionId":event["sessionId"], "lease":event["lease"],
        }),
    )?;
    if receipt["accepted"] != true || receipt["requestId"] != event["requestId"] {
        return Err("DSH decision receipt mismatch".into());
    }
    Ok(())
}

fn wait_for_device_decision(
    usb: UsbSerialManager,
    request: Value,
    event: Value,
    cap: Capability,
    receiver: mpsc::Receiver<Terminal>,
    connection: (String, u64),
) {
    let id = request["requestId"].as_str().unwrap_or_default();
    let never = request["neverExpires"].as_bool().unwrap_or(false)
        || request["timeoutMs"].as_u64() == Some(99999);
    let remaining = request["expiresAtMs"].as_u64().unwrap_or(0).saturating_sub(
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as u64,
    );
    let timeout = if remaining > 0 {
        Duration::from_millis(remaining)
    } else {
        APPROVAL_TIMEOUT
    };
    if !never && request["expiresAtMs"].as_u64().unwrap_or(0) > 0 && remaining == 0 {
        take_terminal(id, Terminal::Expired);
        return;
    }
    let start = Instant::now();
    let _modal = loop {
        if let Ok(guard) = MODAL.try_lock() {
            break guard;
        }
        match receiver.recv_timeout(Duration::from_millis(50)) {
            Ok(_) | Err(mpsc::RecvTimeoutError::Disconnected) => return,
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }
        if usb.approval_connection().as_ref() != Some(&connection)
            || (!never && start.elapsed() >= timeout)
        {
            take_terminal(id, Terminal::Expired);
            return;
        }
    };
    // Cancellation can arrive between admission and this worker. Never send a
    // USB prompt whose exact pending entry has already been consumed.
    let still_pending = pending_decisions()
        .lock()
        .map(|p| p.contains_key(id))
        .unwrap_or(false);
    if !still_pending {
        return;
    }
    if usb
        .send_approval_bound(&connection.0, connection.1, "approval/request", &request)
        .is_err()
    {
        take_terminal(id, Terminal::Disconnected);
        return;
    }
    if let Ok(mut p) = pending_decisions().lock() {
        if let Some(p) = p.get_mut(id) {
            p.presented = true;
        }
    }
    let outcome = loop {
        match receiver.recv_timeout(Duration::from_millis(100)) {
            Ok(value) => break Some(value),
            Err(mpsc::RecvTimeoutError::Disconnected) => break None,
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }
        if usb.approval_connection().as_ref() != Some(&connection) {
            take_terminal(id, Terminal::Disconnected);
            break None;
        }
        if !never && start.elapsed() >= timeout {
            take_terminal(id, Terminal::Expired);
            break None;
        }
    };
    if let Some(Terminal::Decision(decision)) = outcome {
        if usb.approval_connection().as_ref() == Some(&connection) {
            if post_decision(&cap, &event, &decision).is_err() {
                eprintln!("[dsh-approval] decision delivery unknown; no resend");
            }
        }
    }
    // Terminal cancellation/expiry NEVER becomes a decision POST.
    let _ = usb.send_approval_bound(
        &connection.0,
        connection.1,
        "approval/cancel",
        &json!({"requestId":id}),
    );
}

/// Handle a device-originated approval decision. Returns true when the topic is
/// claimed by this relay (including unknown/stale requestIds, which are ignored
/// after the bridge has already timed out).
pub fn handle_device_decision(payload: &Value) -> bool {
    let request_id = payload
        .get("requestId")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .trim()
        .to_string();
    let decision = payload
        .get("decision")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .trim()
        .to_ascii_lowercase();
    if request_id.is_empty() || (decision != "allow" && decision != "deny") {
        eprintln!("[dsh-approval] ignored malformed device decision");
        return true;
    }
    let sender = pending_decisions().lock().ok().and_then(|mut pending| {
        let p = pending.get(&request_id)?;
        if !p.presented && p.usb.approval_connection() == Some((p.board.clone(), p.generation)) {
            return None;
        }
        pending.remove(&request_id)
    });
    if let Some(sender) = sender {
        let normalized = if decision == "allow" { "allow" } else { "deny" };
        if sender.usb.approval_connection() == Some((sender.board.clone(), sender.generation)) {
            let _ = sender
                .sender
                .send(Terminal::Decision(normalized.to_string()));
        } else {
            let _ = sender.sender.send(Terminal::Disconnected);
        }
    } else {
        eprintln!("[dsh-approval] ignored stale decision {request_id}");
    }
    true
}

fn run_event_stream(usb: UsbSerialManager, cap: Capability) -> Result<(), String> {
    let connection = usb.approval_connection().ok_or("USB unavailable")?;
    let response = dsh_auth::client(Duration::from_secs(600))?
        .get(format!("{}/events", cap.base_url))
        .bearer_auth(&cap.token)
        .send()
        .map_err(|_| "DSH SSE unavailable")?;
    if !response.status().is_success() {
        return Err(format!("DSH SSE HTTP {}", response.status().as_u16()));
    }
    let lease = response
        .headers()
        .get("x-hachimodock-lease")
        .and_then(|h| h.to_str().ok())
        .filter(|s| s.len() == 64)
        .ok_or("DSH relay lease unavailable")?
        .to_string();
    let revoked = Arc::new(AtomicBool::new(false));
    let finished = Arc::new(AtomicBool::new(false));
    let watcher = {
        let usb = usb.clone();
        let connection = connection.clone();
        let revoked = revoked.clone();
        let finished = finished.clone();
        let epoch = cap.epoch.clone();
        let cap = cap.clone();
        let lease = lease.clone();
        thread::spawn(move || {
            while !finished.load(Ordering::Acquire) {
                if usb.approval_connection().as_ref() != Some(&connection)
                    || dsh_auth::load("relay")
                        .map(|c| c.epoch != epoch)
                        .unwrap_or(true)
                {
                    revoked.store(true, Ordering::Release);
                    drain_epoch(&epoch);
                    let _ =
                        dsh_auth::post(&cap, "/relay/close", &json!({"epoch":epoch,"lease":lease}));
                    break;
                }
                thread::sleep(Duration::from_millis(100));
            }
        })
    };
    let result = (|| {
        let mut event = String::new();
        let mut data = String::new();
        let mut reader = BufReader::new(response);
        loop {
            let mut bytes = Vec::new();
            let n = reader
                .by_ref()
                .take(65537)
                .read_until(b'\n', &mut bytes)
                .map_err(|_| "DSH SSE read failed")?;
            if n == 0 {
                break;
            }
            if revoked.load(Ordering::Acquire)
                || bytes.len() > 65536
                || data.len() + bytes.len() > 65536
            {
                return Err("DSH SSE revoked or oversized".into());
            }
            let line = std::str::from_utf8(&bytes)
                .map_err(|_| "invalid DSH SSE UTF8")?
                .trim_end_matches(&['\r', '\n'][..]);
            if consume_sse_line(line, &mut event, &mut data) {
                if let Some(payload) = parse_sse_block(&event, &data) {
                    if event == "approval"
                        && payload["epoch"] == cap.epoch
                        && payload["lease"] == lease
                        && payload["owner"] == "native-relay"
                        && payload["sessionId"].as_str().is_some()
                    {
                        if let Some(request) = device_request_payload(&payload) {
                            let id = request["requestId"]
                                .as_str()
                                .unwrap_or_default()
                                .to_string();
                            let Some(permit) = WorkerPermit::acquire() else {
                                return Err("DSH approval capacity exhausted".into());
                            };
                            let (tx, rx) = mpsc::channel();
                            let admitted = if let Ok(mut p) = pending_decisions().lock() {
                                if p.len() >= MAX_PENDING || p.contains_key(&id) {
                                    false
                                } else {
                                    p.insert(
                                        id,
                                        Pending {
                                            sender: tx,
                                            epoch: cap.epoch.clone(),
                                            usb: usb.clone(),
                                            board: connection.0.clone(),
                                            generation: connection.1,
                                            presented: false,
                                        },
                                    );
                                    true
                                }
                            } else {
                                false
                            };
                            if admitted {
                                let usb = usb.clone();
                                let cap = cap.clone();
                                let connection = connection.clone();
                                thread::spawn(move || {
                                    let _permit = permit;
                                    wait_for_device_decision(
                                        usb, request, payload, cap, rx, connection,
                                    );
                                });
                            }
                        }
                    } else if ["approval-cancel", "approval-expired"].contains(&event.as_str())
                        && payload["epoch"] == cap.epoch
                    {
                        if let Some(id) = payload["requestId"].as_str() {
                            take_terminal(
                                id,
                                if event == "approval-expired" {
                                    Terminal::Expired
                                } else {
                                    Terminal::Cancelled
                                },
                            );
                            let _ = usb.send_approval_bound(
                                &connection.0,
                                connection.1,
                                "approval/cancel",
                                &json!({"requestId":id}),
                            );
                        }
                    }
                }
                event.clear();
                data.clear();
            }
        }
        Err("DSH SSE stream ended".into())
    })();
    finished.store(true, Ordering::Release);
    let _ = watcher.join();
    drain_epoch(&cap.epoch);
    result
}

/// Background relay for the DSH plugin. It reconnects forever; USB absence is
/// reported and the plugin keeps its normal browser approval path.
pub fn start(usb: UsbSerialManager) {
    thread::spawn(move || loop {
        // Do not attach the /events subscriber while no P4 is connected: the
        // DSH plugin treats an attached bridge as the terminal answerer, so a
        // resident app without a device must leave the normal DSH UI in charge.
        if !usb.status().connected {
            thread::sleep(Duration::from_secs(2));
            continue;
        }
        let cap = match dsh_auth::load("relay") {
            Ok(c) => c,
            Err(_) => {
                thread::sleep(Duration::from_secs(3));
                continue;
            }
        };
        if let Err(error) = run_event_stream(usb.clone(), cap) {
            eprintln!("[dsh-approval] {error}; retrying in 3s");
        }
        thread::sleep(Duration::from_secs(3));
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    static TEST_PENDING: Mutex<()> = Mutex::new(());
    fn entry(epoch: &str) -> (Pending, mpsc::Receiver<Terminal>) {
        let (tx, rx) = mpsc::channel();
        (
            Pending {
                sender: tx,
                epoch: epoch.into(),
                usb: UsbSerialManager::new(),
                board: "offline-test".into(),
                generation: 1,
                presented: false,
            },
            rx,
        )
    }
    #[test]
    fn cancellation_and_expiry_take_and_wake_exact_waiter() {
        let _lock = TEST_PENDING.lock().unwrap();
        for terminal in [Terminal::Cancelled, Terminal::Expired] {
            let (p, rx) = entry("cancel-test");
            pending_decisions()
                .lock()
                .unwrap()
                .insert("cancel-test".into(), p);
            take_terminal("cancel-test", terminal);
            assert!(matches!(
                rx.recv_timeout(Duration::from_millis(50)),
                Ok(Terminal::Cancelled | Terminal::Expired)
            ));
            assert!(!pending_decisions()
                .lock()
                .unwrap()
                .contains_key("cancel-test"));
            take_terminal("cancel-test", Terminal::Decision("allow".into()));
            assert!(rx.try_recv().is_err());
        }
    }
    #[test]
    fn disconnect_drains_only_its_epoch_and_wakes_never_waiter() {
        let _lock = TEST_PENDING.lock().unwrap();
        let (a, ra) = entry("old");
        let (b, rb) = entry("new");
        {
            let mut p = pending_decisions().lock().unwrap();
            p.insert("epoch-old".into(), a);
            p.insert("epoch-new".into(), b);
        }
        drain_epoch("old");
        assert_eq!(
            ra.recv_timeout(Duration::from_millis(50)).unwrap(),
            Terminal::Disconnected
        );
        assert!(rb.try_recv().is_err());
        take_terminal("epoch-new", Terminal::Cancelled);
        assert_eq!(
            rb.recv_timeout(Duration::from_millis(50)).unwrap(),
            Terminal::Cancelled
        );
    }
    #[test]
    fn physical_disconnect_cannot_post_a_stale_device_allow() {
        let _lock = TEST_PENDING.lock().unwrap();
        let (p, rx) = entry("old-connection");
        pending_decisions()
            .lock()
            .unwrap()
            .insert("stale-device".into(), p);
        assert!(handle_device_decision(
            &json!({"requestId":"stale-device","decision":"allow"})
        ));
        assert_eq!(
            rx.recv_timeout(Duration::from_millis(50)).unwrap(),
            Terminal::Disconnected
        );
    }
    #[test]
    fn admission_has_hard_worker_bound_and_releases_permits() {
        let _lock = TEST_PENDING.lock().unwrap();
        let permits: Vec<_> = (0..MAX_PENDING)
            .map(|_| WorkerPermit::acquire().unwrap())
            .collect();
        assert!(WorkerPermit::acquire().is_none());
        drop(permits);
        assert_eq!(WORKERS.load(Ordering::Acquire), 0);
    }

    #[test]
    fn sse_parser_tracks_approval_event() {
        let mut event = String::new();
        let mut data = String::new();
        assert!(!consume_sse_line("event: approval", &mut event, &mut data));
        assert!(!consume_sse_line(
            "data: {\"requestId\":\"req-1\"}",
            &mut event,
            &mut data
        ));
        assert!(consume_sse_line("", &mut event, &mut data));
        assert_eq!(event, "approval");
        assert_eq!(data, "{\"requestId\":\"req-1\"}");
        assert!(parse_sse_block(&event, &data).is_some());
    }

    #[test]
    fn device_payload_uses_display_reason_and_defaults() {
        let event = json!({
            "requestId": "req-1",
            "agentId": "deepseek-harness",
            "tool": "bash",
            "reason": "raw",
            "displayReason": "需要执行命令",
            "expiresAtMs": 123,
        });
        let payload = device_request_payload(&event).expect("payload");
        assert_eq!(payload["requestId"], "req-1");
        assert_eq!(payload["agent"], "deepseek-harness");
        assert_eq!(payload["reason"], "需要执行命令");
        assert_eq!(payload["expiresAtMs"], 123);
    }

    #[test]
    fn session_and_tool_events_map_to_device_state() {
        let session = json!({
            "session": {
                "id": "session-1",
                "title": "DSH Session",
                "status": "running"
            }
        });
        let payload = device_state_payload("session", &session).expect("session state");
        assert_eq!(payload["state"], "working");
        assert_eq!(payload["sessionId"], "session-1");
        assert_eq!(payload["sessionTitle"], "DSH Session");

        let tool = json!({"sessionId":"session-1","phase":"start","name":"bash","ok":true});
        let payload = device_state_payload("tool", &tool).expect("tool state");
        assert_eq!(payload["state"], "tool_running");
        assert_eq!(payload["event"], "tool/start");

        let turn_end = json!({"sessionId":"session-1","phase":"end"});
        assert_eq!(
            device_state_payload("turn", &turn_end).expect("turn state")["state"],
            "done"
        );
        assert!(device_state_payload("approval", &json!({})).is_none());
    }

    #[test]
    fn session_title_metadata_never_replaces_pet_lifecycle() {
        let metadata = json!({"sessionId":"a", "title":"DeepSeek Harness", "session":{"id":"a","status":"running"}});
        assert!(device_state_payload("session-title", &metadata).is_none());
        let working = device_state_payload("turn", &json!({"sessionId":"a","phase":"start"})).unwrap();
        assert_eq!(working["statusText"], "工作中");
        assert_eq!(working["state"], "working");
    }

    #[test]
    fn device_payload_falls_back_to_reason_when_display_reason_is_empty() {
        let event = json!({
            "requestId": "req-1",
            "tool": "bash",
            "reason": "真实原因",
            "displayReason": "",
        });
        let payload = device_request_payload(&event).expect("payload");
        assert_eq!(payload["reason"], "真实原因");
    }

    #[test]
    fn malformed_or_non_approval_events_are_ignored() {
        assert!(device_request_payload(&json!({"tool": "bash"})).is_none());
        assert!(parse_sse_block("session", "{\"type\":\"session\"}").is_some());
        assert!(parse_sse_block("approval", "not-json").is_none());
    }

    #[test]
    fn device_decision_claims_unknown_request() {
        // Topic ownership is deliberate: stale/unknown decisions must not fall
        // through to generic USB message handling.
        assert!(handle_device_decision(
            &json!({"requestId":"missing","decision":"allow"})
        ));
        assert!(handle_device_decision(
            &json!({"requestId":"","decision":"allow"})
        ));
        assert!(handle_device_decision(
            &json!({"requestId":"req","decision":"maybe"})
        ));
    }
}
