use serde_json::{json, Value};

pub(crate) fn compact(value: &Value) -> Value {
    let mut result = value.clone();
    if let Some(object) = result.as_object_mut() { object.remove("cover"); }
    if result["state"].is_object() { result["state"] = compact(&result["state"]); }
    for field in ["queue", "tracks"] {
        if let Some(items) = result[field].as_array_mut() {
            for item in items { *item = track(item); }
        }
    }
    if result["current"].is_object() { result["current"] = track(&result["current"]); }
    result
}
fn track(value: &Value) -> Value {
    json!({"key":value["key"],"title":value["title"],"artist":value["artist"],"durationMs":value["durationMs"]})
}
pub(crate) fn spoken(operation: &str, value: &Value) -> String {
    let state = if value["state"].is_object() { &value["state"] } else { value };
    let state_name = state["state"].as_str().unwrap_or("");
    if matches!(operation,"play"|"next"|"previous"|"resume"|"toggle"|"seek")
        && matches!(state_name,"queued"|"loading"|"buffering"|"playing") {
        let clean = |field: &str| state["current"][field].as_str().unwrap_or("").chars()
            .filter(|c| !c.is_control()).take(60).collect::<String>();
        let title=clean("title");let artist=clean("artist");
        if !title.is_empty() {
            let song=if artist.is_empty(){format!("《{title}》")}else{format!("{artist}的《{title}》")};
            return match state_name {
                "playing" => format!("设备正在播放{song}。"),
                "queued" => format!("已选好{song}，这段回复结束后开始播放。"),
                _ => format!("正在准备{song}，还没有开始播放。"),
            };
        }
    }
    match (operation,state_name) {
        ("pause"|"toggle","paused"|"interrupted") => "音乐已暂停。".into(),
        ("stop","idle") => "音乐已停止。".into(),
        ("enqueue",_) => "已加入播放列表，不代表下一首就会播放它。".into(),
        ("volume",_) => format!("音乐音量已调整为百分之{}。",state["volume"].as_u64().unwrap_or(0)),
        ("mode",_) => "播放模式已更新。".into(),
        _ => "操作已返回，但尚未确认目标歌曲开始播放。".into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reply_uses_selected_track_not_a_different_song_in_queue() {
        let value=json!({"accepted":true,"state":{"state":"queued","current":{"key":"netease:1","title":"唯一","artist":"邓紫棋"},"queue":[{"title":"江南","artist":"林俊杰"}],"cover":"PRIVATE_IMAGE"}});
        let reply=spoken("next",&value);
        assert!(reply.contains("邓紫棋的《唯一》"));assert!(!reply.contains("江南"));
        assert!(!reply.contains("正在播放"));assert!(!compact(&value).to_string().contains("PRIVATE_IMAGE"));
    }
    #[test]
    fn queue_only_is_not_a_promise_to_play_next() {
        assert!(spoken("enqueue",&json!({})).contains("不代表下一首"));
        assert!(!spoken("play",&json!({"state":"error"})).contains("正在播放"));
    }
}
