//! Public builds use only user-provided service credentials.
//! A legacy non-secret placeholder is rejected instead of sent as a real key.
pub const ARK_HANDLE: &str = "hachimidock-bundled-ark-v1";
pub fn ark_key() -> Option<&'static str> { None }
pub fn speech_key() -> Option<&'static str> { None }
pub fn resolve_service_header(_name: &str, value: &str, _url: &str) -> Result<String, String> {
    if value.contains(ARK_HANDLE) { return Err("请在 API 配置中填写自己的服务凭据".into()); }
    Ok(value.into())
}
pub fn redact_service_values(value: String) -> String { value }

pub fn apply_generation_default(value: &mut serde_json::Value, api_key: Option<&str>) {
    let Some(api_key) = api_key.filter(|key| !key.trim().is_empty()) else { return; };
    let Some(all) = value.as_object_mut() else { return; };
    let provider = all.entry("volcengine").or_insert_with(|| serde_json::json!({}));
    let Some(provider) = provider.as_object_mut() else { return; };
    if provider.get("apiKey").and_then(|v| v.as_str()).is_some_and(|v| !v.trim().is_empty()) { return; }
    if provider.get("baseUrl").and_then(|v| v.as_str()).is_some_and(|v|
        !v.trim().is_empty() && v.trim().trim_end_matches('/') != "https://ark.cn-beijing.volces.com/api/v3") { return; }
    provider.insert("apiKey".into(), serde_json::json!(api_key));
}


#[cfg(test)]
mod tests {
    #[test]
    fn public_build_has_no_embedded_credentials() {
        assert!(super::ark_key().is_none());
        assert!(super::speech_key().is_none());
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
    fn public_generation_defaults_never_add_keys() {
        let mut value=serde_json::json!({"volcengine":{"model":"user-model"}});
        let before=value.clone();
        super::apply_generation_default(&mut value,super::ark_key());
        assert_eq!(value,before);
    }
}
