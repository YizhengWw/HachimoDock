//! Local DSH readiness. Never use ASR keys or a listening TCP port as proof of
//! a connected host / owned composer, and never expose descriptor credentials.
use serde_json::{json, Value};

fn session_valid(s: &str) -> bool {
    !s.is_empty() && s != "auto" && s.len() <= 200 && !s.chars().any(char::is_control)
}

pub(crate) fn status(session: &str) -> Result<Value, String> {
    if !session.is_empty() && !session_valid(session) {
        return Err("DeepSeek 需要明确的会话，不能自动选择最近会话".into());
    }
    let cap = match super::dsh_auth::load("voice") {
        Ok(cap) => cap,
        Err(_) => return Ok(json!({"hostReady":false,"composerReady":false,"reason":"host-unavailable","sessionId":session})),
    };
    let v = super::dsh_auth::post(&cap, "/voice/status", &json!({"sessionId":session}))?;
    if v["protocol"] != "hachimodock-scoped-input-v1" || v["epoch"] != cap.epoch || v["sessionId"] != session
        || !v["composerReady"].is_boolean() || !v["sessionKnown"].is_boolean()
    {
        return Err("DeepSeek 桥接能力合同不支持；状态与语音未确认".into());
    }
    let host_ready = v["hostReady"] == true && v["capabilities"]["sessions"] == true && v["capabilities"]["events"] == true;
    Ok(json!({"hostReady":host_ready,"composerReady":host_ready && v["sessionKnown"] == true && v["composerReady"] == true,
        "sessionId":session,"epoch":cap.epoch,"capabilities":v["capabilities"],
        "clientBundleAvailable":v["clientBundleAvailable"] == true,
        "composerMount":if v["composerMount"] == "paired-connected" {"paired-connected"} else {"unverified"},
        "questionPending":if v["questionPending"].is_boolean() {v["questionPending"].clone()} else {Value::Null},
        "questionStateKnown":v["questionStateKnown"] == true,
        "reason":if !host_ready {"host-contract-unsupported"} else if v["questionPending"] == true {"question-pending"} else if v["composerReady"] != true {"composer-unpaired"} else {"ready"}}))
}

pub(crate) fn pair(session: &str) -> Result<Value, String> {
    if !session_valid(session) { return Err("请先选择明确的 DeepSeek 会话".into()); }
    let cap = super::dsh_auth::load("voice")?;
    // The Desktop scheme/issuer is an explicit adapter contract, not a port,
    // version or executable hash. No null/wildcard/worker HTTP Origin fallback.
    let v = super::dsh_auth::post(&cap, "/pair/ticket", &json!({"sessionId":session,"origin":"dsh-app://app"}))?;
    let code = v["code"].as_str().filter(|s| s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit()))
        .ok_or("DeepSeek 配对回执无效")?;
    if v["epoch"] != cap.epoch || !v["expiresAtMs"].is_u64() { return Err("DeepSeek 配对合同不支持".into()); }
    // UI memory only. Never log, persist or add to app resources/envelopes.
    Ok(json!({"code":code,"expiresAtMs":v["expiresAtMs"],"sessionId":session}))
}
