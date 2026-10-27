//! Authenticated config encryption. macOS uses an owner-only local key; Windows
//! retains its existing credential vault. macOS never accesses Keychain.
//! Existing unreadable ciphertext is not overwritten with a replacement key.
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use ring::{
    aead,
    rand::{SecureRandom, SystemRandom},
};
use std::{fs, io::Write, path::Path, sync::Mutex};

#[cfg(all(not(test), windows))]
static KEY: Mutex<Option<[u8; 32]>> = Mutex::new(None);
static PROVIDERS: Mutex<()> = Mutex::new(());
static FILES: Mutex<()> = Mutex::new(());
const FORMAT: &str = "pet-manager-aes256gcm-v1";
#[cfg(any(target_os = "macos", test))]
#[path = "secure_config_local.rs"]
mod local;

#[cfg(all(not(test), windows))]
fn vault_key(create: bool) -> Result<[u8; 32], String> {
    let mut cached = KEY.lock().map_err(|_| "凭据存储繁忙")?;
    if let Some(key) = *cached {
        return Ok(key);
    }
    let entry = keyring::Entry::new("com.petmanager.desktop.vault", "config-key-v1")
        .map_err(|_| "无法打开系统凭据存储")?;
    let key = match entry.get_password() {
        Ok(raw) => B64
            .decode(raw)
            .ok()
            .and_then(|v| v.try_into().ok())
            .ok_or("系统凭据密钥损坏，请勿删除原配置")?,
        Err(keyring::Error::NoEntry) if create => {
            let mut key = [0u8; 32];
            SystemRandom::new()
                .fill(&mut key)
                .map_err(|_| "无法生成凭据密钥")?;
            entry
                .set_password(&B64.encode(key))
                .map_err(|_| "系统凭据存储拒绝写入；原配置保留")?;
            key
        }
        Err(keyring::Error::NoEntry) => {
            return Err("未找到原配置密钥；原配置未修改，请勿删除或覆盖配置".into())
        }
        Err(_) => return Err("无法读取系统凭据；原配置保留".into()),
    };
    *cached = Some(key);
    Ok(key)
}

fn config_key(path: &Path, create: bool) -> Result<[u8; 32], String> {
    #[cfg(any(target_os = "macos", test))]
    {
        let dir = path.parent().ok_or("配置目录无效")?;
        local::load_key(dir, create, || {
            if !encrypted_configs(dir)?.is_empty() {
                return Err("本机配置密钥缺失，无法读取已有 API 配置；未清空或覆盖原文件".into());
            }
            Ok(())
        })
    }
    #[cfg(all(windows, not(test)))]
    {
        vault_key(create)
    }
    #[cfg(all(not(any(target_os = "macos", windows)), not(test)))]
    {
        let _ = (path, create);
        Err("此平台暂不支持安全配置存储".into())
    }
}

#[cfg(any(target_os = "macos", test))]
fn encrypted_configs(dir: &Path) -> Result<Vec<(serde_json::Value, &'static str)>, String> {
    let mut encrypted = Vec::new();
    for (name, purpose) in [
        ("voice-chat-settings.json", "voice-chat"),
        ("volcengine-device-asr.json", "speech-asr"),
        ("generation-providers.secure.json", "generation-providers"),
    ] {
        let path = dir.join(name);
        match fs::symlink_metadata(&path) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
            Ok(m) if m.is_file() && m.len() <= 1024 * 1024 => (),
            _ => return Err("无法验证原配置；未创建或替换密钥".into()),
        }
        let value: serde_json::Value =
            serde_json::from_slice(&fs::read(&path).map_err(|_| "无法读取原配置")?)
                .map_err(|_| "原配置格式损坏；未创建或替换密钥")?;
        if !value.is_object() || (value.get("format").is_some() && value["format"] != FORMAT) {
            return Err("原配置格式不受支持；未创建或替换密钥".into());
        }
        if value["format"] == FORMAT {
            encrypted.push((value, purpose));
        }
    }
    Ok(encrypted)
}

fn seal(raw: &[u8], purpose: &str, key: &[u8; 32]) -> Result<Vec<u8>, String> {
    let key = aead::LessSafeKey::new(
        aead::UnboundKey::new(&aead::AES_256_GCM, key).map_err(|_| "密钥无效")?,
    );
    let mut nonce = [0u8; 12];
    SystemRandom::new()
        .fill(&mut nonce)
        .map_err(|_| "随机数生成失败")?;
    let mut ciphertext = raw.to_vec();
    key.seal_in_place_append_tag(
        aead::Nonce::assume_unique_for_key(nonce),
        aead::Aad::from(purpose.as_bytes()),
        &mut ciphertext,
    )
    .map_err(|_| "配置加密失败")?;
    serde_json::to_vec(&serde_json::json!({"format":FORMAT,"nonce":B64.encode(nonce),"ciphertext":B64.encode(ciphertext)}))
        .map_err(|_|"配置编码失败".into())
}
fn unseal(value: &serde_json::Value, purpose: &str, key: &[u8; 32]) -> Result<Vec<u8>, String> {
    let decode = |field: &str| {
        B64.decode(value[field].as_str().unwrap_or(""))
            .map_err(|_| "加密配置损坏")
    };
    let nonce: [u8; 12] = decode("nonce")?.try_into().map_err(|_| "加密配置损坏")?;
    let mut ciphertext = decode("ciphertext")?;
    let key = aead::LessSafeKey::new(
        aead::UnboundKey::new(&aead::AES_256_GCM, key).map_err(|_| "密钥无效")?,
    );
    key.open_in_place(
        aead::Nonce::assume_unique_for_key(nonce),
        aead::Aad::from(purpose.as_bytes()),
        &mut ciphertext,
    )
    .map(|raw| raw.to_vec())
    .map_err(|_| "配置解密校验失败；请保留原文件并检查本机配置密钥".into())
}
pub fn write(path: &Path, purpose: &str, raw: &[u8]) -> Result<(), String> {
    let _guard = FILES.lock().map_err(|_| "配置文件繁忙")?;
    write_unlocked(path, purpose, raw)
}
fn write_unlocked(path: &Path, purpose: &str, raw: &[u8]) -> Result<(), String> {
    let parent = path.parent().ok_or("配置目录无效")?;
    let encrypted = seal(raw, purpose, &config_key(path, true)?)?;
    fs::create_dir_all(parent).map_err(|_| "无法创建配置目录")?;
    let mut temp = tempfile::NamedTempFile::new_in(parent).map_err(|_| "无法创建配置临时文件")?;
    temp.write_all(&encrypted)
        .and_then(|_| temp.as_file().sync_all())
        .map_err(|_| "无法写入配置")?;
    temp.persist(path).map_err(|_| "无法提交配置；原配置保留")?;
    Ok(())
}
pub fn read(path: &Path, purpose: &str) -> Result<Option<String>, String> {
    let _guard = FILES.lock().map_err(|_| "配置文件繁忙")?;
    match fs::symlink_metadata(path) {
        Ok(meta) if !meta.is_file() || meta.len() > 1024 * 1024 => {
            return Err("配置路径或大小不受支持；原文件未修改".into())
        }
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => return Err("无法检查配置文件".into()),
        _ => {}
    }
    let raw = match fs::read(path) {
        Ok(v) => v,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err("无法读取配置".into()),
    };
    let value: serde_json::Value =
        serde_json::from_slice(&raw).map_err(|_| "配置格式损坏；原文件未修改")?;
    if !value.is_object() || (value.get("format").is_some() && value["format"] != FORMAT) {
        return Err("配置格式或加密版本不受支持；原文件未修改".into());
    }
    let plaintext = if value["format"] == FORMAT {
        unseal(&value, purpose, &config_key(path, false)?)?
    } else {
        // Legacy JSON is validated and atomically replaced only after local-key
        // persistence and encryption succeed; failed migration preserves it.
        write_unlocked(path, purpose, &raw)?;
        raw
    };
    String::from_utf8(plaintext)
        .map(Some)
        .map_err(|_| "配置编码无效".into())
}

pub fn load_providers(dir: &Path) -> Result<serde_json::Value, String> {
    let _guard = PROVIDERS.lock().map_err(|_| "配置繁忙")?;
    read(
        &dir.join("generation-providers.secure.json"),
        "generation-providers",
    )?
    .map(|raw| serde_json::from_str(&raw).map_err(|_| "生成配置无效".into()))
    .unwrap_or_else(|| Ok(serde_json::json!({})))
}
pub fn save_provider(dir: &Path, id: &str, config: serde_json::Value) -> Result<(), String> {
    if !matches!(id, "volcengine" | "kling" | "custom") || !config.is_object() {
        return Err("服务配置无效".into());
    }
    let _guard = PROVIDERS.lock().map_err(|_| "配置繁忙")?;
    let path = dir.join("generation-providers.secure.json");
    let mut all: serde_json::Value = read(&path, "generation-providers")?
        .map(|raw| serde_json::from_str(&raw))
        .transpose()
        .map_err(|_| "生成配置无效")?
        .unwrap_or_else(|| serde_json::json!({}));
    if !all.is_object() {
        return Err("生成配置无效；原文件未修改".into());
    }
    all[id] = config;
    let raw = serde_json::to_vec(&all).map_err(|_| "生成配置编码失败")?;
    if raw.len() > 256 * 1024 {
        return Err("生成配置超过大小限制".into());
    }
    write(&path, "generation-providers", &raw)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fresh_install_saves_all_api_configs_and_reads_them_after_restart() {
        let dir = tempfile::Builder::new()
            .prefix("pet-config-cold-")
            .tempdir()
            .unwrap();
        assert_eq!(load_providers(dir.path()).unwrap(), serde_json::json!({}));
        assert!(!dir.path().join("private/config-key-v1").exists());
        let fixtures = [
            (
                "voice-chat-settings.json",
                "voice-chat",
                br#"{"llmApiKey":"fixture-chat"}"#.as_slice(),
            ),
            (
                "volcengine-device-asr.json",
                "speech-asr",
                br#"{"apiKey":"fixture-speech"}"#.as_slice(),
            ),
            (
                "generation-providers.secure.json",
                "generation-providers",
                br#"{"custom":{"apiKey":"fixture-generation"}}"#.as_slice(),
            ),
        ];
        for (name, purpose, raw) in fixtures {
            assert!(read(&dir.path().join(name), purpose).unwrap().is_none());
            write(&dir.path().join(name), purpose, raw).unwrap();
            assert!(!fs::read(dir.path().join(name))
                .unwrap()
                .windows(raw.len())
                .any(|w| w == raw));
        }
        // No in-memory macOS key cache: every read reloads the persisted local
        // key, the same path used by a newly started application process.
        let key = fs::read(dir.path().join("private/config-key-v1")).unwrap();
        assert_eq!(key.len(), 32);
        for (name, purpose, raw) in fixtures {
            assert_eq!(
                read(&dir.path().join(name), purpose)
                    .unwrap()
                    .unwrap()
                    .as_bytes(),
                raw
            );
        }
        assert_eq!(
            fs::read(dir.path().join("private/config-key-v1")).unwrap(),
            key
        );
        let result = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--ignored",
                "--exact",
                "secure_config::tests::fresh_install_child_read",
            ])
            .env("PET_CONFIG_COLD_TEST_DIR", dir.path())
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "independent process must load all saved configs"
        );
    }
    #[test]
    #[ignore = "Invoked only by the parent clean-install test against its temporary fixture directory"]
    fn fresh_install_child_read() {
        let dir = std::path::PathBuf::from(
            std::env::var_os("PET_CONFIG_COLD_TEST_DIR").expect("temporary fixture path"),
        );
        assert!(dir
            .file_name()
            .unwrap()
            .to_string_lossy()
            .starts_with("pet-config-cold-"));
        assert!(dir
            .canonicalize()
            .unwrap()
            .starts_with(std::env::temp_dir().canonicalize().unwrap()));
        for (name, purpose, key) in [
            ("voice-chat-settings.json", "voice-chat", "fixture-chat"),
            ("volcengine-device-asr.json", "speech-asr", "fixture-speech"),
            (
                "generation-providers.secure.json",
                "generation-providers",
                "fixture-generation",
            ),
        ] {
            assert!(read(&dir.join(name), purpose)
                .unwrap()
                .unwrap()
                .contains(key));
        }
    }
    #[test]
    fn explicit_reset_allows_a_clean_start_without_a_legacy_backend() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("voice-chat-settings.json");
        fs::write(
            &path,
            seal(br#"{"key":"old"}"#, "voice-chat", &[31; 32]).unwrap(),
        )
        .unwrap();
        assert!(read(&path, "voice-chat").is_err());
        assert!(!dir.path().join("private/config-key-v1").exists());
        // Explicit user-authorized removal, never an automatic startup action.
        fs::remove_file(&path).unwrap();
        write(&path, "voice-chat", br#"{"key":"new-manual-input"}"#).unwrap();
        assert_eq!(
            read(&path, "voice-chat").unwrap().unwrap(),
            r#"{"key":"new-manual-input"}"#
        );
    }
    #[test]
    fn unavailable_legacy_key_preserves_all_ciphertext_and_rejects_overwrite() {
        let dir = tempfile::tempdir().unwrap();
        let encrypted = seal(br#"{"key":"fixture"}"#, "voice-chat", &[17; 32]).unwrap();
        let path = dir.path().join("voice-chat-settings.json");
        fs::write(&path, &encrypted).unwrap();
        assert!(read(&path, "voice-chat").is_err());
        assert!(write(&path, "voice-chat", br#"{"key":"replacement"}"#).is_err());
        assert_eq!(fs::read(&path).unwrap(), encrypted);
        assert!(!dir.path().join("private/config-key-v1").exists());
    }
    #[test]
    fn corrupted_or_future_files_are_preserved() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("config.json");
        for raw in ["{broken", "[]", r#"{"format":"pet-manager-aes256gcm-v99"}"#] {
            fs::write(&path, raw).unwrap();
            assert!(read(&path, "voice").is_err());
            assert_eq!(fs::read_to_string(&path).unwrap(), raw);
        }
    }
    #[test]
    fn encryption_rejects_tampering_wrong_key_and_context() {
        let raw = br#"{"apiKey":"test-secret"}"#;
        let sealed = seal(raw, "voice", &[1; 32]).unwrap();
        assert!(!String::from_utf8_lossy(&sealed).contains("test-secret"));
        let mut value: serde_json::Value = serde_json::from_slice(&sealed).unwrap();
        assert_eq!(unseal(&value, "voice", &[1; 32]).unwrap(), raw);
        assert!(unseal(&value, "other", &[1; 32]).is_err());
        assert!(unseal(&value, "voice", &[2; 32]).is_err());
        value["ciphertext"] = serde_json::json!(B64.encode([0; 32]));
        assert!(unseal(&value, "voice", &[1; 32]).is_err());
    }
    #[test]
    fn legacy_migration_and_provider_roundtrip() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("legacy.json");
        fs::write(&path, br#"{"key":"test-value"}"#).unwrap();
        assert!(read(&path, "legacy")
            .unwrap()
            .unwrap()
            .contains("test-value"));
        assert!(!fs::read_to_string(&path).unwrap().contains("test-value"));
        save_provider(
            dir.path(),
            "custom",
            serde_json::json!({"apiKey":"synthetic"}),
        )
        .unwrap();
        assert_eq!(
            load_providers(dir.path()).unwrap()["custom"]["apiKey"],
            "synthetic"
        );
        assert!(save_provider(dir.path(), "../escape", serde_json::json!({})).is_err());
    }
}
