//! Installation permits the component's validated, bounded host services by default.
//! This is a service on/off preference, not an OS permission or a per-version grant.
use serde::{Deserialize, Serialize};
use std::{io::{Read, Write}, path::Path};

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Settings {
    schema_version: u8,
    enabled: bool,
}

pub fn enabled(dir: &Path) -> Result<bool, String> {
    let file = match std::fs::File::open(dir.join("service-settings.json")) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(true),
        Err(_) => return Err("无法读取组件服务设置".into()),
    };
    let mut bytes = Vec::new();
    file.take(1025).read_to_end(&mut bytes).map_err(|_| "无法读取组件服务设置")?;
    if bytes.len() > 1024 { return Err("组件服务设置损坏，请联系开发者恢复".into()); }
    let settings: Settings = serde_json::from_slice(&bytes)
        .map_err(|_| "组件服务设置损坏，请联系开发者恢复")?;
    if settings.schema_version != 1 { return Err("组件服务设置版本不受支持".into()); }
    Ok(settings.enabled)
}

pub fn set_enabled(dir: &Path, enabled: bool) -> Result<(), String> {
    std::fs::create_dir_all(dir).map_err(|_| "无法保存组件服务设置")?;
    let mut file = tempfile::NamedTempFile::new_in(dir).map_err(|_| "无法保存组件服务设置")?;
    let bytes = serde_json::to_vec(&Settings { schema_version: 1, enabled })
        .map_err(|_| "组件服务设置无效")?;
    file.write_all(&bytes).and_then(|_| file.as_file().sync_all())
        .map_err(|_| "无法保存组件服务设置")?;
    file.persist(dir.join("service-settings.json")).map_err(|_| "无法保存组件服务设置")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fresh_and_legacy_components_are_enabled_without_a_grant_or_write() {
        let root = tempfile::tempdir().unwrap();
        let dir = root.path().join("component");
        assert!(enabled(&dir).unwrap());
        assert!(!dir.exists(), "status does not create a grant or state");
        std::fs::create_dir(&dir).unwrap();
        for old_grant in [r#"{"version":"old","policy":"app-services-1"}"#,
            r#"{"version":"old","networkPolicy":"public-dns-v1"}"#, "broken legacy data"] {
            std::fs::write(dir.join("grant.json"), old_grant).unwrap();
            assert!(enabled(&dir).unwrap());
            assert!(!dir.join("service-settings.json").exists());
        }
    }

    #[test]
    fn explicit_stop_survives_updates_and_does_not_touch_user_data() {
        let root = tempfile::tempdir().unwrap();
        let a = root.path().join("a");
        set_enabled(&a, false).unwrap();
        for filename in ["state.json", "library.json", "notes.json"] {
            std::fs::write(a.join(filename), "user data").unwrap();
        }
        std::fs::write(a.join("grant.json"), r#"{"version":"new"}"#).unwrap();
        assert!(!enabled(&a).unwrap());
        assert!(enabled(&root.path().join("b")).unwrap());
        set_enabled(&a, true).unwrap();
        assert!(enabled(&a).unwrap());
        for filename in ["state.json", "library.json", "notes.json"] {
            assert_eq!(std::fs::read_to_string(a.join(filename)).unwrap(), "user data");
        }
    }

    #[test]
    fn damaged_explicit_preference_never_silently_reenables_a_service() {
        let root = tempfile::tempdir().unwrap();
        for data in ["{", r#"{"schemaVersion":1}"#, r#"{"schemaVersion":2,"enabled":true}"#,
            r#"{"schemaVersion":1,"enabled":"false"}"#,
            r#"{"schemaVersion":1,"enabled":true,"unexpected":1}"#] {
            std::fs::write(root.path().join("service-settings.json"), data).unwrap();
            assert!(enabled(root.path()).is_err());
        }
        std::fs::write(root.path().join("service-settings.json"), " ".repeat(1025)).unwrap();
        assert!(enabled(root.path()).is_err());
        set_enabled(root.path(), true).unwrap();
        assert!(enabled(root.path()).unwrap());
    }
}
