//! Bounded desktop generation text responses. Does not retry or log payloads.
use crate::generation_diagnostics::Record;

pub const IMAGE_EDIT_TOTAL_TIMEOUT_MS: u64 = 600_000;
pub const MAX_RESPONSE_BYTES: usize = 64 * 1024 * 1024;

pub fn request_timeout_ms(supplied: Option<u64>, stage: Option<&str>, method: &str) -> u64 {
    if stage == Some("image_edit") && method.eq_ignore_ascii_case("POST") {
        supplied.unwrap_or(120_000).clamp(1, IMAGE_EDIT_TOTAL_TIMEOUT_MS)
    } else {
        supplied.unwrap_or(120_000).max(1)
    }
}

pub fn bounded_body_size(current: usize, additional: usize) -> Option<usize> {
    current.checked_add(additional).filter(|size| *size <= MAX_RESPONSE_BYTES)
}

fn limit_error(diagnostic: &mut Record) -> String {
    diagnostic.outcome = "invalid_output".into();
    "云服务响应内容超过64 MiB安全上限，结果未接收；请求可能已执行，不会自动重发".into()
}

pub async fn read_text_bounded(mut response: reqwest::Response, diagnostic: &mut Record) -> Result<String, String> {
    if response.content_length().is_some_and(|size| size > MAX_RESPONSE_BYTES as u64) {
        return Err(limit_error(diagnostic));
    }
    let mut bytes = Vec::new();
    while let Some(chunk) = response.chunk().await.map_err(|error| diagnostic.transport_error(&error))? {
        if bounded_body_size(bytes.len(), chunk.len()).is_none() { return Err(limit_error(diagnostic)); }
        bytes.extend_from_slice(&chunk);
    }
    // Reuse reqwest's exact charset/BOM handling after the network body has
    // been bounded. This response only wraps local bytes; it makes no request.
    let mut buffered = tauri::http::Response::new(bytes);
    *buffered.headers_mut() = response.headers().clone();
    let text = reqwest::Response::from(buffered).text().await
        .map_err(|error| diagnostic.transport_error(&error))?;
    if text.len() > MAX_RESPONSE_BYTES { return Err(limit_error(diagnostic)); }
    Ok(text)
}

/// Only known result GETs opt into this short body-idle budget. Never applied
/// to synchronous image generation while the model may still be computing.
pub async fn read_image_bytes_bounded(mut response: reqwest::Response, diagnostic: &mut Record, limit: usize) -> Result<Vec<u8>, String> {
    if response.content_length().is_some_and(|size| size > limit as u64) { return Err(limit_error(diagnostic)); }
    let mut bytes = Vec::new();
    loop {
        let chunk = tokio::time::timeout(std::time::Duration::from_secs(10), response.chunk()).await
            .map_err(|_| { diagnostic.outcome="timeout".into(); "已知图片结果下载连续10秒无数据；结果链接已保留，不会重新生成".to_owned() })?
            .map_err(|error| diagnostic.transport_error(&error))?;
        let Some(chunk) = chunk else { break; };
        if bytes.len().checked_add(chunk.len()).is_none_or(|size| size > limit) { return Err(limit_error(diagnostic)); }
        bytes.extend_from_slice(&chunk);
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn image_has_a_finite_total_budget_without_changing_other_stage_defaults() {
        assert_eq!(request_timeout_ms(None,Some("image_edit"),"POST"),120_000);
        assert_eq!(request_timeout_ms(Some(900_000),Some("image_edit"),"POST"),600_000);
        assert_eq!(request_timeout_ms(Some(75),Some("image_edit"),"POST"),75);
        assert_eq!(request_timeout_ms(Some(0),Some("image_edit"),"POST"),1);
        assert_eq!(request_timeout_ms(None,Some("thinking"),"POST"),120_000);
        assert_eq!(request_timeout_ms(None,Some("image_edit"),"GET"),120_000);
        assert_eq!(request_timeout_ms(Some(900_000),Some("video_poll"),"GET"),900_000);
    }
    #[test]
    fn response_size_is_checked_without_overflow_or_large_allocations() {
        assert_eq!(bounded_body_size(0,MAX_RESPONSE_BYTES),Some(MAX_RESPONSE_BYTES));
        assert_eq!(bounded_body_size(MAX_RESPONSE_BYTES,1),None);
        assert_eq!(bounded_body_size(usize::MAX,1),None);
    }
}
