use serde_json::Value;

pub(super) fn validate(input: &Value) -> Result<&str, String> {
    let object = input.as_object().ok_or("播放器参数必须为对象")?;
    let op = input["operation"].as_str().ok_or("缺少播放器操作")?;
    let fields: &[&str] = match op {
        "search" => &["query", "source"],
        "play" | "enqueue" | "remove" => &["key"],
        "move" => &["key", "index"],
        "seek" => &["positionMs"],
        "volume" => &["volume"],
        "mode" => &["mode"],
        "status" | "next" | "previous" | "pause" | "resume" | "toggle" | "stop" => &[],
        _ => return Err("不支持的播放器操作".into()),
    };
    if object.keys().any(|key| key != "operation" && !fields.contains(&key.as_str())) {
        return Err(if matches!(op, "next" | "previous") {
            "next/previous 只按当前队列切歌，不接受歌名、歌手、query 或 key；指定歌曲请先 search/status 获取准确 key，再调用 play。".into()
        } else { "播放器参数包含不支持的字段；请按当前 operation 传入参数".into() });
    }
    for field in fields.iter().filter(|&&f| f != "source") {
        let valid = match *field {
            "index" => input[*field].as_u64().is_some_and(|n| n < 20),
            "positionMs" => input[*field].as_u64().is_some_and(|n| n <= 3_600_000),
            "volume" => input[*field].as_u64().is_some_and(|n| n <= 100),
            "mode" => input[*field].as_str().is_some_and(|s| matches!(s, "sequence" | "single" | "loop" | "shuffle")),
            "query" => input[*field].as_str().is_some_and(|s| !s.trim().is_empty() && s.chars().count() <= 100),
            "key" => input[*field].as_str().is_some_and(|s| !s.is_empty() && s.len() <= 200),
            _ => false,
        };
        if !valid { return Err(format!("播放器参数 {field} 缺失或格式无效")); }
    }
    if op == "search" && object.contains_key("source")
        && !input["source"].as_str().is_some_and(|s| matches!(s, "auto" | "netease" | "joox" | "bilibili")) {
        return Err("搜索音源无效".into());
    }
    Ok(op)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn commands_never_silently_ignore_selection_or_other_operation_fields() {
        for op in ["next", "previous", "resume", "status", "pause", "toggle", "stop"] {
            for field in ["query", "key", "source", "volume", "index", "mode", "positionMs"] {
                assert!(validate(&json!({"operation":op,field:"unwanted"})).is_err(), "{op}/{field}");
            }
            assert!(validate(&json!({"operation":op})).is_ok());
        }
        assert!(validate(&json!({"operation":"next","key":"netease:108914"})).unwrap_err().contains("play"));
    }
    #[test]
    fn required_values_have_strict_types_and_bounds() {
        for bad in [json!({"operation":"search","query":null}), json!({"operation":"play"}),
            json!({"operation":"volume","volume":101}), json!({"operation":"seek","positionMs":-1}),
            json!({"operation":"move","key":"x","index":20}), json!({"operation":"search","query":"x","source":"invalid"})] {
            assert!(validate(&bad).is_err());
        }
        assert!(validate(&json!({"operation":"search","query":"林俊杰 江南"})).is_ok());
        assert!(validate(&json!({"operation":"play","key":"netease:108914"})).is_ok());
    }
}
