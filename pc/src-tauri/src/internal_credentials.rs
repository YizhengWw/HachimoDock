//! Explicit internal-build defaults; public binaries do not reference generated credentials.
#[cfg(feature = "internal-network")]
fn values() -> &'static serde_json::Value {
    static VALUE: std::sync::OnceLock<serde_json::Value> = std::sync::OnceLock::new();
    VALUE.get_or_init(|| serde_json::from_str(include_str!(concat!(env!("OUT_DIR"), "/internal-credentials.json")))
        .expect("build-validated internal credentials"))
}

fn key(name: &str) -> Option<&'static str> {
    #[cfg(feature = "internal-network")]
    { values().get(name).and_then(serde_json::Value::as_str).filter(|s| !s.is_empty()) }
    #[cfg(not(feature = "internal-network"))]
    { let _ = name; None }
}

pub fn ark_key() -> Option<&'static str> { key("arkApiKey") }
pub fn speech_key() -> Option<&'static str> { key("speechApiKey") }

#[cfg(test)]
mod tests {
    #[test]
    fn credentials_only_exist_in_internal_builds() {
        assert_eq!(super::ark_key().is_some(), cfg!(feature = "internal-network"));
        assert_eq!(super::speech_key().is_some(), cfg!(feature = "internal-network"));
    }
}
