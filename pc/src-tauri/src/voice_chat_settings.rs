/*
 * [Input] 对话大模型（DeepSeek / 豆包 / MiMo 预设或自定义 OpenAI 兼容端点）与豆包 Seed TTS 2.0
 *         的凭据；ASR/TTS 共用语音识别凭据存储，旧版独立 TTS Key 仅用于无 ASR Key 时迁移。
 * [Output] One JSON settings file under the app data dir (`voice-chat-settings.json`, 0600 on
 *          unix), masked status for the UI, and resolved LLM/TTS configs for the realtime chat
 *          pipeline. Public builds embed nothing; internal builds may embed defaults.
 * [Pos] Tauri-side credential store for realtime persona chat (mirrors volcengine_asr's file mode).
 * [Sync] If fields change, update `ApiSettings.jsx`, `pc/.folder.md`, and `pc/docs/realtime-chat.md`.
 */

use serde::{Deserialize, Serialize};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use crate::doubao_tts::{DoubaoTtsConfig, DEFAULT_RESOURCE_ID as TTS_DEFAULT_RESOURCE_ID};
use crate::persona_llm::{LlmConfig, DEFAULT_BASE_URL as LLM_DEFAULT_BASE_URL, DEFAULT_MODEL as LLM_DEFAULT_MODEL};

const FILE_NAME: &str = "voice-chat-settings.json";

static STORAGE_DIR: OnceLock<PathBuf> = OnceLock::new();

/// Built-in chat presets: users only paste a key, everything else is filled in.
pub struct LlmPreset {
    pub id: &'static str,
    pub label: &'static str,
    pub base_url: &'static str,
    pub model: &'static str,
}

pub const LLM_PRESETS: &[LlmPreset] = &[
    LlmPreset { id: "deepseek", label: "DeepSeek", base_url: "https://api.deepseek.com", model: "deepseek-flash" },
    LlmPreset { id: "doubao", label: "豆包（火山方舟）", base_url: "https://ark.cn-beijing.volces.com/api/v3", model: "doubao-seed-2-0-lite-260428" },
    LlmPreset { id: "mimo", label: "Xiaomi MiMo", base_url: "https://api.xiaomimimo.com/v1", model: "mimo-v2.5" },
];
pub const DEFAULT_LLM_PROVIDER: &str = "deepseek";

pub fn llm_preset(id: &str) -> Option<&'static LlmPreset> {
    LLM_PRESETS.iter().find(|preset| preset.id == id)
}

fn normalize_provider(value: &str) -> String {
    let id = value.trim().to_ascii_lowercase();
    if id == "custom" || llm_preset(&id).is_some() {
        id
    } else {
        DEFAULT_LLM_PROVIDER.to_string()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct VoiceChatSettings {
    /// "deepseek" | "doubao" | "mimo" | "custom" — presets fill base URL and model.
    pub llm_provider: String,
    pub llm_base_url: String,
    pub llm_model: String,
    pub llm_api_key: String,
    pub tts_api_key: String,
    pub tts_resource_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct VoiceChatSettingsInput {
    pub llm_provider: Option<String>,
    pub llm_base_url: Option<String>,
    pub llm_model: Option<String>,
    pub llm_api_key: Option<String>,
    pub tts_api_key: Option<String>,
    pub tts_resource_id: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VoiceChatSettingsStatus {
    pub llm_configured: bool,
    pub tts_configured: bool,
    pub llm_provider: String,
    /// true when the TTS key falls back to the speech-recognition key
    pub tts_uses_asr_key: bool,
    pub llm_base_url: String,
    pub llm_model: String,
    pub llm_api_key_masked: String,
    pub tts_api_key_masked: String,
    pub tts_resource_id: String,
    pub credential_source: &'static str,
}

pub fn configure_storage_dir(path: PathBuf) -> Result<(), String> {
    match STORAGE_DIR.set(path.clone()) {
        Ok(()) => Ok(()),
        Err(_) if STORAGE_DIR.get() == Some(&path) => Ok(()),
        Err(_) => Err("语音对话配置目录已初始化为其他路径".to_string()),
    }
}

fn settings_path() -> Result<PathBuf, String> {
    let dir = STORAGE_DIR.get().ok_or_else(|| "语音对话配置目录尚未初始化".to_string())?;
    Ok(dir.join(FILE_NAME))
}

pub fn mask_secret(value: &str) -> String {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return String::new();
    }
    let chars: Vec<char> = trimmed.chars().collect();
    if chars.len() <= 8 {
        return "••••".to_string();
    }
    let head: String = chars[..3].iter().collect();
    let tail: String = chars[chars.len() - 4..].iter().collect();
    format!("{head}…{tail}")
}

fn read_file(path: &Path) -> Result<Option<VoiceChatSettings>, String> {
    match fs::read_to_string(path) {
        Ok(raw) => serde_json::from_str(&raw)
            .map(Some)
            .map_err(|error| format!("语音对话配置损坏 {}: {error}", path.display())),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(format!("读取语音对话配置失败 {}: {error}", path.display())),
    }
}

fn write_file(path: &Path, settings: &VoiceChatSettings) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("创建配置目录失败: {error}"))?;
    }
    let raw = serde_json::to_string_pretty(settings).map_err(|error| error.to_string())?;
    let mut temporary = tempfile::NamedTempFile::new_in(path.parent().ok_or("配置目录无效")?)
        .map_err(|_| "无法创建语音配置临时文件")?;
    temporary.write_all(raw.as_bytes()).and_then(|_| temporary.as_file().sync_all())
        .map_err(|_| "无法写入语音配置")?;
    temporary.persist(path).map_err(|_| "无法保存语音配置")?;
    Ok(())
}

/// Stored values first, then internal-build defaults, then public defaults.
pub fn load() -> Result<(VoiceChatSettings, &'static str), String> {
    let stored = read_file(&settings_path()?)?.unwrap_or_default();
    let mut source = "local";
    let mut settings = stored;
    if apply_internal_ark_default(&mut settings, crate::internal_credentials::ark_key()) { source = "internal-build"; }
    settings.llm_provider = normalize_provider(&settings.llm_provider);
    if let Some(preset) = llm_preset(&settings.llm_provider) {
        // presets are authoritative: the user only supplies the key
        settings.llm_base_url = preset.base_url.to_string();
        settings.llm_model = preset.model.to_string();
    } else {
        if settings.llm_base_url.trim().is_empty() {
            settings.llm_base_url = LLM_DEFAULT_BASE_URL.to_string();
        }
        if settings.llm_model.trim().is_empty() {
            settings.llm_model = LLM_DEFAULT_MODEL.to_string();
        }
    }
    settings.tts_resource_id = TTS_DEFAULT_RESOURCE_ID.to_string();
    Ok((settings, source))
}

fn apply_internal_ark_default(settings: &mut VoiceChatSettings, key: Option<&str>) -> bool {
    let Some(key) = key.filter(|key| !key.trim().is_empty()) else { return false; };
    if !settings.llm_api_key.trim().is_empty() { return false; }
    if settings.llm_provider.trim().is_empty() { settings.llm_provider = "doubao".into(); }
    // Never send a built-in Ark credential to DeepSeek, MiMo or a custom endpoint.
    if settings.llm_provider.trim().eq_ignore_ascii_case("doubao") {
        settings.llm_api_key = key.into();
        true
    } else { false }
}

/// ASR and TTS resolve the same key after legacy migration; no hidden TTS override.
fn resolve_tts_key(_settings: &VoiceChatSettings) -> (String, bool) {
    match crate::volcengine_asr::stored_api_key() {
        Some(key) => (key, true),
        None => (String::new(), false),
    }
}

/// Keep the existing ASR key; migrate a TTS-only installation before removing its old override.
/// Copy first, then clear: a failed credential-store write must never lose the only key.
pub fn migrate_shared_speech_key() -> Result<(), String> {
    let (effective, _) = load()?;
    let asr = crate::volcengine_asr::settings_status()?;
    migrate_shared_speech_key_at(&settings_path()?, asr.configured, &effective.tts_api_key, |key| {
        crate::volcengine_asr::save_settings(crate::volcengine_asr::DeviceAsrSettingsInput {
            api_key: Some(key.to_string()), resource_id: None,
        }).map(|_| ())
    })
}

fn migrate_shared_speech_key_at(path: &Path, asr_configured: bool, legacy_key: &str,
    save_asr: impl FnOnce(&str) -> Result<(), String>) -> Result<(), String> {
    if !asr_configured && !legacy_key.trim().is_empty() {
        save_asr(legacy_key.trim())?;
    }
    if let Some(mut stored) = read_file(path)? {
        if !stored.tts_api_key.is_empty() || stored.tts_resource_id != TTS_DEFAULT_RESOURCE_ID {
            stored.tts_api_key.clear();
            stored.tts_resource_id = TTS_DEFAULT_RESOURCE_ID.into();
            write_file(path, &stored)?;
        }
    }
    Ok(())
}

pub fn status() -> Result<VoiceChatSettingsStatus, String> {
    migrate_shared_speech_key()?;
    let (settings, source) = load()?;
    let (tts_key, tts_uses_asr_key) = resolve_tts_key(&settings);
    Ok(VoiceChatSettingsStatus {
        llm_configured: !settings.llm_api_key.trim().is_empty(),
        tts_configured: !tts_key.is_empty(),
        llm_provider: settings.llm_provider.clone(),
        tts_uses_asr_key,
        llm_base_url: settings.llm_base_url.clone(),
        llm_model: settings.llm_model.clone(),
        llm_api_key_masked: mask_secret(&settings.llm_api_key),
        tts_api_key_masked: mask_secret(&tts_key),
        tts_resource_id: settings.tts_resource_id.clone(),
        credential_source: source,
    })
}

/// Merge an update from the settings page. Empty/missing secrets keep the stored value;
/// a secret that looks like our own mask is ignored so re-saving the form never clobbers keys.
pub fn save(input: VoiceChatSettingsInput) -> Result<VoiceChatSettingsStatus, String> {
    let path = settings_path()?;
    let mut settings = read_file(&path)?.unwrap_or_default();
    if let Some(value) = input.llm_provider {
        settings.llm_provider = normalize_provider(&value);
    }
    if let Some(value) = input.llm_base_url {
        settings.llm_base_url = value.trim().to_string();
    }
    if let Some(value) = input.llm_model {
        settings.llm_model = value.trim().to_string();
    }
    if let Some(value) = input.llm_api_key {
        let trimmed = value.trim();
        if !trimmed.is_empty() && !trimmed.contains('…') && trimmed != "••••" {
            settings.llm_api_key = trimmed.to_string();
        }
    }
    if let Some(value) = input.tts_api_key {
        let trimmed = value.trim();
        if !trimmed.is_empty() && !trimmed.contains('…') && trimmed != "••••" {
            settings.tts_api_key = trimmed.to_string();
        }
    }
    if let Some(value) = input.tts_resource_id {
        settings.tts_resource_id = value.trim().to_string();
    }
    write_file(&path, &settings)?;
    status()
}

pub fn clear_secret(which: &str) -> Result<VoiceChatSettingsStatus, String> {
    let path = settings_path()?;
    let mut settings = read_file(&path)?.unwrap_or_default();
    match which {
        "llm" => settings.llm_api_key.clear(),
        "tts" => settings.tts_api_key.clear(),
        _ => return Err("unknown secret".to_string()),
    }
    write_file(&path, &settings)?;
    status()
}

pub fn llm_config() -> Result<LlmConfig, String> {
    let (settings, _) = load()?;
    Ok(LlmConfig {
        base_url: settings.llm_base_url,
        model: settings.llm_model,
        api_key: settings.llm_api_key,
    })
}

pub fn tts_config(speaker: &str, clone_speaker_id: &str, speed: f32) -> Result<DoubaoTtsConfig, String> {
    migrate_shared_speech_key()?;
    let (settings, _) = load()?;
    let (tts_key, _) = resolve_tts_key(&settings);
    Ok(DoubaoTtsConfig::new(
        &tts_key,
        speaker,
        clone_speaker_id,
        speed,
        &settings.tts_resource_id,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bundled_ark_key_only_defaults_to_official_doubao_and_never_overwrites_user_key() {
        let mut fresh = VoiceChatSettings::default();
        assert!(apply_internal_ark_default(&mut fresh, Some("test-internal-key")));
        assert_eq!(fresh.llm_provider, "doubao");
        for provider in ["deepseek", "mimo", "custom", "unknown"] {
            let mut other = VoiceChatSettings { llm_provider: provider.into(), ..Default::default() };
            assert!(!apply_internal_ark_default(&mut other, Some("test-internal-key")));
            assert!(other.llm_api_key.is_empty());
        }
        let mut saved = VoiceChatSettings { llm_provider: "doubao".into(), llm_api_key:"user-test-key".into(), ..Default::default() };
        assert!(!apply_internal_ark_default(&mut saved, Some("test-internal-key")));
        assert_eq!(saved.llm_api_key, "user-test-key");
        let mut public = VoiceChatSettings::default();
        assert!(!apply_internal_ark_default(&mut public, None));
        assert!(public.llm_api_key.is_empty());
    }

    #[test]
    fn shared_speech_migration_preserves_existing_asr_and_llm() {
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().join(FILE_NAME);
        let legacy = VoiceChatSettings { tts_api_key: "legacy-test-key".into(), tts_resource_id: "old".into(), llm_api_key:"llm-test-key".into(), ..Default::default() };
        write_file(&path, &legacy).unwrap();
        migrate_shared_speech_key_at(&path, true, &legacy.tts_api_key, |_| panic!("must not replace ASR key")).unwrap();
        let migrated = read_file(&path).unwrap().unwrap();
        assert!(migrated.tts_api_key.is_empty());
        assert_eq!(migrated.llm_api_key, legacy.llm_api_key);
        assert_eq!(migrated.tts_resource_id, TTS_DEFAULT_RESOURCE_ID);
        #[cfg(unix)] { use std::os::unix::fs::PermissionsExt; assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600); }
    }

    #[test]
    fn tts_only_migration_copies_before_clearing_and_can_retry() {
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().join(FILE_NAME);
        let legacy = VoiceChatSettings { tts_api_key: "legacy-test-key".into(), ..Default::default() };
        write_file(&path, &legacy).unwrap();
        assert!(migrate_shared_speech_key_at(&path, false, &legacy.tts_api_key, |_| Err("credential store unavailable".into())).is_err());
        assert_eq!(read_file(&path).unwrap().unwrap().tts_api_key, legacy.tts_api_key);
        let mut copied = String::new();
        migrate_shared_speech_key_at(&path, false, &legacy.tts_api_key, |key| { copied = key.to_string(); Ok(()) }).unwrap();
        assert_eq!(copied, legacy.tts_api_key);
        assert!(read_file(&path).unwrap().unwrap().tts_api_key.is_empty());
        migrate_shared_speech_key_at(&path, true, "", |_| panic!("migration must be idempotent")).unwrap();
    }

    #[test]
    fn presets_fill_endpoint_and_model_and_unknown_providers_fall_back_to_deepseek() {
        assert_eq!(normalize_provider("Doubao"), "doubao");
        assert_eq!(normalize_provider("nope"), "deepseek");
        assert_eq!(normalize_provider("custom"), "custom");
        let preset = llm_preset("mimo").unwrap();
        assert_eq!(preset.base_url, "https://api.xiaomimimo.com/v1");
        assert_eq!(preset.model, "mimo-v2.5");
        assert_eq!(LLM_PRESETS.len(), 3);
    }

    #[test]
    fn masks_keep_only_head_and_tail() {
        assert_eq!(mask_secret(""), "");
        assert_eq!(mask_secret("short"), "••••");
        assert_eq!(mask_secret("sk-1234567890abcdef"), "sk-…cdef");
    }

    #[test]
    fn saving_a_masked_secret_keeps_the_stored_key() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(FILE_NAME);
        write_file(
            &path,
            &VoiceChatSettings { llm_api_key: "sk-realkey123456".into(), ..Default::default() },
        )
        .unwrap();
        let mut stored = read_file(&path).unwrap().unwrap();
        let masked = mask_secret(&stored.llm_api_key);
        // simulate the merge rule used by `save`
        let trimmed = masked.trim();
        if !trimmed.is_empty() && !trimmed.contains('…') && trimmed != "••••" {
            stored.llm_api_key = trimmed.to_string();
        }
        assert_eq!(stored.llm_api_key, "sk-realkey123456");
    }
}
