/*
 * [Input] DSH bridge plugin approval SSE events and ESP32-P4 USB approval decisions.
 * [Output] Single-request approval relay: plugin -> USB approval/request -> device
 *          decision -> plugin /approval/decision, with bounded timeout and no
 *          fallback that blocks the normal DSH browser approval path.
 * [Pos] Host-side R5 approval relay for the DeepSeek Harness channel.
 * [Sync] If this contract changes, update docs/dsh-integration-contract.md and
 *         firmware/protocol.md.
 */

use std::collections::HashMap;
use std::io::{BufRead, BufReader};
use std::sync::{mpsc, Mutex, OnceLock};
use std::thread;
use std::time::Duration;

use serde_json::{json, Value};

use crate::usb_serial::UsbSerialManager;

const DEFAULT_BRIDGE_URL: &str = "http://127.0.0.1:17892";
const APPROVAL_TIMEOUT: Duration = Duration::from_secs(30);

type DecisionSender = mpsc::Sender<String>;

fn pending_decisions() -> &'static Mutex<HashMap<String, DecisionSender>> {
    static PENDING: OnceLock<Mutex<HashMap<String, DecisionSender>>> = OnceLock::new();
    PENDING.get_or_init(|| Mutex::new(HashMap::new()))
}

fn bridge_base_url() -> String {
    std::env::var("HACHIMODOCK_DSH_URL")
        .ok()
        .map(|value| value.trim().trim_end_matches('/').to_string())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| DEFAULT_BRIDGE_URL.to_string())
}

/// Convert one plugin `approval` SSE payload into the bounded USB payload. The
/// device only needs presentation data; requestId is the correlation key.
fn device_request_payload(event: &Value) -> Option<Value> {
    let request_id = event
        .get("requestId")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|value| !value.is_empty())?;
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
                if running { "DeepSeek Harness 工作中" } else { "DeepSeek Harness 待机" },
                "session",
            )
        }
        "turn" => match payload.get("phase").and_then(Value::as_str) {
            Some("start") => ("working", "DeepSeek Harness 工作中", "turn/start"),
            Some("end") => ("done", "DeepSeek Harness 已完成", "turn/end"),
            _ => return None,
        },
        "tool" => match payload.get("phase").and_then(Value::as_str) {
            Some("start") => ("tool_running", "DeepSeek Harness 调用工具", "tool/start"),
            Some("end") => ("working", "DeepSeek Harness 处理中", "tool/end"),
            _ => return None,
        },
        "session-title" => ("idle", "DeepSeek Harness", "session/title"),
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

fn post_decision(base_url: &str, request_id: &str, decision: &str) -> Result<(), String> {
    let client = reqwest::blocking::Client::builder()
        .no_proxy()
        .timeout(Duration::from_secs(5))
        .build()
        .map_err(|error| format!("DSH approval client failed: {error}"))?;
    let response = client
        .post(format!("{base_url}/approval/decision"))
        .json(&json!({ "requestId": request_id, "decision": decision }))
        .send()
        .map_err(|error| format!("DSH approval decision failed: {error}"))?;
    if !response.status().is_success() {
        return Err(format!(
            "DSH approval decision HTTP {}",
            response.status().as_u16()
        ));
    }
    Ok(())
}

fn wait_for_device_decision(
    usb: UsbSerialManager,
    request: Value,
    base_url: String,
) {
    let request_id = request
        .get("requestId")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    if request_id.is_empty() {
        return;
    }
    let (sender, receiver) = mpsc::channel::<String>();
    if let Ok(mut pending) = pending_decisions().lock() {
        pending.insert(request_id.clone(), sender);
    }
    if let Err(error) = usb.send("approval/request", &request) {
        eprintln!("[dsh-approval] usb approval/request failed: {error}");
        if let Ok(mut pending) = pending_decisions().lock() {
            pending.remove(&request_id);
        }
        return;
    }
    match receiver.recv_timeout(APPROVAL_TIMEOUT) {
        Ok(decision) => {
            if let Err(error) = post_decision(&base_url, &request_id, &decision) {
                eprintln!("[dsh-approval] {error}");
            }
        }
        Err(_) => {
            eprintln!("[dsh-approval] device decision timeout for {request_id}");
        }
    }
    if let Ok(mut pending) = pending_decisions().lock() {
        pending.remove(&request_id);
    }
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
    let sender = pending_decisions()
        .lock()
        .ok()
        .and_then(|mut pending| pending.remove(&request_id));
    if let Some(sender) = sender {
        let normalized = if decision == "allow" { "allow" } else { "deny" };
        let _ = sender.send(normalized.to_string());
    } else {
        eprintln!("[dsh-approval] ignored stale decision {request_id}");
    }
    true
}

fn run_event_stream(usb: UsbSerialManager, base_url: String) -> Result<(), String> {
    let client = reqwest::blocking::Client::builder()
        .no_proxy()
        .timeout(Duration::from_secs(10 * 60))
        .build()
        .map_err(|error| format!("DSH SSE client failed: {error}"))?;
    let response = client
        .get(format!("{base_url}/events"))
        .send()
        .map_err(|error| format!("DSH SSE connect failed: {error}"))?;
    if !response.status().is_success() {
        return Err(format!("DSH SSE HTTP {}", response.status().as_u16()));
    }
    eprintln!("[dsh-approval] listening on {base_url}/events");
    let mut event = String::new();
    let mut data = String::new();
    let reader = BufReader::new(response);
    for line in reader.lines() {
        let line = line.map_err(|error| format!("DSH SSE read failed: {error}"))?;
        if consume_sse_line(&line, &mut event, &mut data) {
            if let Some(payload) = parse_sse_block(&event, &data) {
                if event == "approval" {
                    if let Some(request) = device_request_payload(&payload) {
                        let usb = usb.clone();
                        let base_url = base_url.clone();
                        thread::spawn(move || wait_for_device_decision(usb, request, base_url));
                    }
                } else if event == "approval-cancel" {
                    if let Some(request_id) = payload.get("requestId").and_then(Value::as_str) {
                        let _ = usb.send(
                            "approval/cancel",
                            &json!({"requestId": request_id}),
                        );
                    }
                }
                // Session/status data is owned by the normal session/current
                // pipeline. Do not emit a second generic state bubble here:
                // README bubble rules are active-only + 60s terminal retention.
            }
            event.clear();
            data.clear();
        }
    }
    Err("DSH SSE stream ended".to_string())
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
        let base_url = bridge_base_url();
        if let Err(error) = run_event_stream(usb.clone(), base_url) {
            eprintln!("[dsh-approval] {error}; retrying in 3s");
        }
        thread::sleep(Duration::from_secs(3));
    });
}

#[cfg(test)]
mod tests {
    use super::*;

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
        assert!(handle_device_decision(&json!({"requestId":"missing","decision":"allow"})));
        assert!(handle_device_decision(&json!({"requestId":"","decision":"allow"})));
        assert!(handle_device_decision(&json!({"requestId":"req","decision":"maybe"})));
    }
}
