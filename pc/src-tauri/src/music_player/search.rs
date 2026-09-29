use serde_json::{json, Value};

// Sequential and bounded: never fan out a successful Netease search to all sources.
pub(super) fn automatic(mut search: impl FnMut(&str) -> Result<Value, String>) -> Result<Value, String> {
    let mut had_empty = false;
    let mut errors = false;
    for source in ["netease", "joox", "bilibili"] {
        match search(source) {
            Ok(mut result) if result["tracks"].as_array().is_some_and(|a| !a.is_empty()) => {
                result["fallbackUsed"] = json!(source != "netease");
                return Ok(result);
            }
            Ok(_) => had_empty = true,
            Err(error) => {
                // A shared API limit must not be multiplied into more failed requests.
                if error.contains("限流") || error.contains("HTTP 429") { return Err(error); }
                errors = true;
            }
        }
    }
    if errors && !had_empty { return Err("音乐搜索服务暂不可用，请稍后重试".into()); }
    Ok(json!({"tracks":[],"source":"GD音乐台","message":if errors {"未找到歌曲，部分音源暂不可用"} else {"未找到歌曲，请调整歌名或歌手后重试"}}))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn netease_success_never_queries_fallbacks() {
        let mut calls=vec![];
        let result=automatic(|src| {calls.push(src.to_string());Ok(json!({"tracks":[{"key":"netease:1"}]}))}).unwrap();
        assert_eq!(calls,vec!["netease"]);assert_eq!(result["fallbackUsed"],false);
    }
    #[test]
    fn empty_or_unavailable_sources_fall_back_in_order_preserving_keys() {
        let mut calls=vec![];
        let result=automatic(|src| {calls.push(src.to_string());match src {
            "netease"=>Ok(json!({"tracks":[]})), "joox"=>Err("unavailable".into()),
            _=>Ok(json!({"tracks":[{"key":"bilibili:2"}]}))}}).unwrap();
        assert_eq!(calls,vec!["netease","joox","bilibili"]);
        assert_eq!(result["tracks"][0]["key"],"bilibili:2");assert_eq!(result["fallbackUsed"],true);
    }
    #[test]
    fn distinguishes_empty_results_failure_and_rate_limit() {
        assert_eq!(automatic(|_|Ok(json!({"tracks":[]}))).unwrap()["tracks"],json!([]));
        assert!(automatic(|_|Err("offline".into())).is_err());
        let mut calls=0;
        assert!(automatic(|_|{calls+=1;Err("限流".into())}).is_err());assert_eq!(calls,1);
    }
}
