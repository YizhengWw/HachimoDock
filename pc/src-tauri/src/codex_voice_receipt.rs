//! Read-only confirmation of a human-triggered native Codex submission.
//! Reuses Pet Manager's existing local session event bus. Never posts a prompt.
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{io::Read, time::Duration};

pub(super) struct Receipt {
    client: reqwest::blocking::Client,
    stream: String,
    cursor: u64,
    hash: String,
    polls: usize,
    user_events: usize,
}

fn read_events(client: &reqwest::blocking::Client, stream: Option<&str>, cursor: u64) -> Result<Value, String> {
    let mut query = vec![("agentId", "codex".to_string()), ("limit", "200".to_string())];
    if let Some(stream) = stream {
        query.extend([("streamId", stream.to_string()), ("cursor", cursor.to_string())]);
    }
    let response = client.get("http://127.0.0.1:8181/agent/session-events").query(&query)
        .send().and_then(|r| r.error_for_status()).map_err(|_| "本地消息回执服务暂不可用")?;
    let mut bytes = Vec::new();
    response.take(256 * 1024 + 1).read_to_end(&mut bytes).map_err(|_| "消息回执读取失败")?;
    if bytes.len() > 256 * 1024 { return Err("消息回执超过安全大小".into()); }
    let value: Value = serde_json::from_slice(&bytes).map_err(|_| "消息回执格式错误")?;
    if value["ok"] != true { return Err("消息回执服务尚未就绪".into()); }
    Ok(value)
}

fn match_batch(data: &Value, stream: &str, cursor: &mut u64, hash: &str) -> Result<bool, String> {
    let next = data["cursor"].as_u64().ok_or("消息回执游标缺失")?;
    if data["ok"] != true || data["reset"] == true || data["streamId"].as_str() != Some(stream) || next < *cursor {
        return Err("消息回执流已变化，不会重复发送".into());
    }
    let events = data["events"].as_array().ok_or("消息回执列表缺失")?;
    let mut previous = *cursor;
    let mut sessions = std::collections::HashSet::new();
    for event in events {
        let position = event["cursor"].as_u64().ok_or("消息事件游标缺失")?;
        if position <= previous || position > next || event["bootstrap"] == true {
            return Err("消息回执不是本次提交后的新事件".into());
        }
        previous = position;
        if event["event"] == "event_msg:user_message"
            && event["taskCorrelation"]["userHash"].as_str() == Some(hash) {
            let session = event["session"]["id"].as_str().filter(|s| !s.is_empty() && s.len() <= 512)
                .ok_or("消息回执会话标识缺失")?;
            sessions.insert(session);
        }
    }
    if sessions.len() > 1 { return Err("多个会话出现相同消息，无法唯一确认".into()); }
    *cursor = next;
    Ok(sessions.len() == 1)
}

impl Receipt {
    pub(super) fn capture(text: &str) -> Result<Self, String> {
        let client = reqwest::blocking::Client::builder().no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .connect_timeout(Duration::from_millis(300)).timeout(Duration::from_millis(800))
            .build().map_err(|_| "无法建立本地消息回执连接")?;
        let data = read_events(&client, None, 0)?;
        let stream = data["streamId"].as_str().filter(|s| !s.is_empty() && s.len() <= 512)
            .ok_or("消息回执流尚未就绪")?.to_string();
        let cursor = data["cursor"].as_u64().ok_or("消息回执游标缺失")?;
        Ok(Self { client, stream, cursor, hash: format!("{:x}", Sha256::digest(text.trim().as_bytes())), polls: 0, user_events: 0 })
    }

    pub(super) fn poll(&mut self) -> Result<bool, String> {
        self.polls += 1;
        let data = read_events(&self.client, Some(&self.stream), self.cursor)?;
        let matched = match_batch(&data, &self.stream, &mut self.cursor, &self.hash)?;
        self.user_events += data["events"].as_array().map_or(0, |events|
            events.iter().filter(|e| e["event"] == "event_msg:user_message").count());
        Ok(matched)
    }

    // Counts only: never export prompt text, hashes, thread IDs or local paths.
    pub(super) fn diagnostic(&self) -> String {
        format!("回执检查次数={}；新用户消息数={}", self.polls, self.user_events)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn event(cursor: u64, hash: &str, session: &str) -> Value {
        json!({"cursor":cursor,"event":"event_msg:user_message","session":{"id":session},"taskCorrelation":{"userHash":hash}})
    }
    fn batch(events: Vec<Value>) -> Value { json!({"ok":true,"streamId":"stream","cursor":12,"events":events}) }
    #[test]
    fn accepts_only_new_exact_user_receipt() {
        let mut cursor = 10;
        assert!(match_batch(&batch(vec![event(11,"exact","one")]), "stream", &mut cursor,"exact").unwrap());
        assert_eq!(cursor,12);
        assert!(!match_batch(&batch(vec![event(11,"other","one")]),"stream",&mut 10,"exact").unwrap());
        let mut assistant=event(11,"exact","one"); assistant["event"]=json!("event_msg:agent_message");
        assert!(!match_batch(&batch(vec![assistant]),"stream",&mut 10,"exact").unwrap());
    }
    #[test]
    fn rejects_old_bootstrap_reset_reordered_and_ambiguous_receipts() {
        for mut data in [batch(vec![event(10,"exact","one")]), batch(vec![event(11,"exact","")]),
            batch(vec![event(12,"exact","one"),event(11,"exact","one")]),
            batch(vec![event(11,"exact","one"),event(12,"exact","two")])] {
            assert!(match_batch(&data,"stream",&mut 10,"exact").is_err());
            data["ok"]=json!(false);
            assert!(match_batch(&data,"stream",&mut 10,"exact").is_err());
        }
        for key in ["reset", "bootstrap", "stream"] {
            let mut data=batch(vec![event(11,"exact","one")]);
            match key { "reset"=>data["reset"]=json!(true), "bootstrap"=>data["events"][0]["bootstrap"]=json!(true), _=>data["streamId"]=json!("other") }
            assert!(match_batch(&data,"stream",&mut 10,"exact").is_err());
        }
    }
}
