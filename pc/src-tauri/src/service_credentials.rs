//! Public builds use only user-provided service credentials.
//! A legacy non-secret placeholder is rejected instead of sent as a real key.
pub const ARK_HANDLE: &str = "hachimidock-bundled-ark-v1";
pub fn resolve_service_header(_name: &str, value: &str, _url: &str) -> Result<String, String> {
    if value.contains(ARK_HANDLE) { return Err("请在 API 配置中填写自己的服务凭据".into()); }
    Ok(value.into())
}
pub fn redact_service_values(value: String) -> String { value }

#[cfg(test)]
mod tests {
    #[test]
    fn user_headers_are_not_rewritten() {
        assert_eq!(super::resolve_service_header("Authorization", "Bearer fixture-user-key", "https://custom.example").unwrap(), "Bearer fixture-user-key");
    }
    #[test]
    fn legacy_placeholder_is_never_sent_as_a_key() {
        let header = format!("Bearer {}", super::ARK_HANDLE);
        for url in ["https://ark.cn-beijing.volces.com/api/v3/models", "https://custom.example"] {
            assert!(super::resolve_service_header("Authorization", &header, url).is_err());
        }
        assert_eq!(super::resolve_service_header("Authorization", "Bearer user-own-key", "https://custom.example").unwrap(), "Bearer user-own-key");
    }
    #[test]
    fn legacy_placeholder_is_rejected_in_any_header() {
        assert!(super::resolve_service_header("X-Other", super::ARK_HANDLE, "https://custom.example").is_err());
        assert_eq!(super::redact_service_values("plain response".into()), "plain response");
    }
}
