/*
 * [Input] 对话大模型（DeepSeek / 豆包 / MiMo 预设或自定义 OpenAI 兼容端点）与豆包 Seed TTS 2.0
 *         的凭据；ASR/TTS 共用语音识别凭据存储，旧版独立 TTS Key 仅用于无 ASR Key 时迁移。
 * [Output] One JSON settings file under the app data dir (`voice-chat-settings.json`, 0600 on
 *          unix), masked status for the UI, and resolved LLM/TTS configs for the realtime chat
 *          pipeline. LLM keys are saved per provider (custom keys also per endpoint),
 *          with only masks exposed to the UI. Public builds embed nothing; internal builds may embed defaults.
 * [Pos] Tauri-side credential store for realtime persona chat (mirrors volcengine_asr's file mode).
 * [Sync] If fields change, update `ApiSettings.jsx`, `pc/.folder.md`, and `pc/docs/realtime-chat.md`.
 */

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use crate::doubao_tts::{DoubaoTtsConfig, DEFAULT_RESOURCE_ID as TTS_DEFAULT_RESOURCE_ID};
use crate::persona_llm::{LlmConfig, DEFAULT_BASE_URL as LLM_DEFAULT_BASE_URL, DEFAULT_MODEL as LLM_DEFAULT_MODEL};

const FILE_NAME: &str = "voice-chat-settings.json";

static STORAGE_DIR: OnceLock<PathBuf> = OnceLock::new();

/// Built-in chat defaults: endpoints are fixed, model names may be overridden.
pub struct LlmPreset {
    pub id: &'static str,
    pub label: &'static str,
    pub base_url: &'static str,
    pub model: &'static str,
}

pub const LLM_PRESETS: &[LlmPreset] = &[
    LlmPreset { id: "deepseek", label: "DeepSeek", base_url: "https://api.deepseek.com", model: "deepseek-flash" },
    LlmPreset { id: "doubao", label: "豆包（火山方舟）", base_url: "https://ark.cn-beijing.volces.com/api/v3", model: "doubao-seed-2-0-lite-260428" },
    LlmPreset { id: "mimo", label: "Xiaomi MiMo", base_url: "https://api.xiaomimimo.com/v1", model: "mimo-v2.6-flash" },
];
pub const DEFAULT_LLM_PROVIDER: &str = "doubao";

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
    /// "deepseek" | "doubao" | "mimo" | "custom" — presets supply defaults, not a model whitelist.
    pub llm_provider: String,
    pub llm_base_url: String,
    pub llm_model: String,
    pub llm_api_key: String,
    /// Presets are scoped by provider; custom keys are additionally scoped by endpoint.
    pub llm_api_keys: BTreeMap<String, String>,
    pub llm_custom_base_url: String,
    pub llm_web_search: Option<bool>,
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
    pub llm_web_search: Option<bool>,
    pub tts_api_key: Option<String>,
    pub tts_resource_id: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VoiceChatSettingsStatus {
    pub llm_web_search: bool,
    pub llm_configured: bool,
    pub tts_configured: bool,
    pub llm_provider: String,
    /// true when the TTS key falls back to the speech-recognition key
    pub tts_uses_asr_key: bool,
    pub llm_base_url: String,
    pub llm_model: String,
    pub llm_api_key_masked: String,
    pub llm_api_key_masks: BTreeMap<String, String>,
    pub llm_custom_base_url: String,
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
        Ok(raw) => serde_json::from_str::<VoiceChatSettings>(&raw)
            .map(|mut settings| {
                // Pre-provider files used DeepSeek by default. Do not redirect
                // an old credential when changing the first-install default.
                if settings.llm_provider.trim().is_empty() && (!settings.llm_api_key.is_empty()
                    || !settings.llm_base_url.is_empty() || !settings.llm_model.is_empty()) {
                    settings.llm_provider = LLM_PRESETS.iter().find(|p|
                        (!settings.llm_base_url.is_empty() && settings.llm_base_url.trim().trim_end_matches('/') == p.base_url)
                        || (settings.llm_base_url.is_empty() && settings.llm_model.starts_with(p.id)))
                        .map(|p| p.id).unwrap_or(if settings.llm_base_url.is_empty() { "deepseek" } else { "custom" }).into();
                }
                Some(settings)
            })
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
    remember_active_key(&mut settings);
    if settings.llm_api_key.trim().is_empty() {
        settings.llm_api_key = settings.llm_api_keys.get(&credential_scope(&settings)).cloned().unwrap_or_default();
    }
    if apply_internal_ark_default(&mut settings, crate::internal_credentials::ark_key()) { source = "internal-build"; }
    apply_llm_defaults(&mut settings);
    settings.tts_resource_id = TTS_DEFAULT_RESOURCE_ID.to_string();
    Ok((settings, source))
}

fn apply_llm_defaults(settings: &mut VoiceChatSettings) {
    settings.llm_provider = normalize_provider(&settings.llm_provider);
    settings.llm_model = settings.llm_model.trim().to_string();
    // Migrate the previous incomplete preset name, not user-selected model IDs.
    if settings.llm_provider == "mimo" && settings.llm_model == "mimo-v2.6" {
        settings.llm_model = "mimo-v2.6-flash".into();
    }
    if let Some(preset) = llm_preset(&settings.llm_provider) {
        // Keep preset credentials on the official endpoint, but honor the saved model.
        settings.llm_base_url = preset.base_url.to_string();
        if settings.llm_model.is_empty() {
            settings.llm_model = preset.model.to_string();
        }
    } else {
        if settings.llm_base_url.trim().is_empty() {
            settings.llm_base_url = LLM_DEFAULT_BASE_URL.to_string();
        }
        if settings.llm_model.trim().is_empty() {
            settings.llm_model = LLM_DEFAULT_MODEL.to_string();
        }
    }
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
        llm_web_search: settings.llm_web_search.unwrap_or(true),
        llm_configured: !settings.llm_api_key.trim().is_empty(),
        tts_configured: !tts_key.is_empty(),
        llm_provider: settings.llm_provider.clone(),
        tts_uses_asr_key,
        llm_base_url: settings.llm_base_url.clone(),
        llm_model: settings.llm_model.clone(),
        llm_api_key_masked: mask_secret(&settings.llm_api_key),
        llm_api_key_masks: credential_masks(&settings, crate::internal_credentials::ark_key()),
        llm_custom_base_url: settings.llm_custom_base_url.clone(),
        tts_api_key_masked: mask_secret(&tts_key),
        tts_resource_id: settings.tts_resource_id.clone(),
        credential_source: source,
    })
}

fn credential_scope(settings: &VoiceChatSettings) -> String {
    let provider = normalize_provider(&settings.llm_provider);
    if provider == "custom" {
        let url = settings.llm_base_url.trim().trim_end_matches('/');
        format!("custom:{}", if url.is_empty() { LLM_DEFAULT_BASE_URL } else { url })
    } else { provider }
}

fn remember_active_key(settings: &mut VoiceChatSettings) {
    if normalize_provider(&settings.llm_provider) == "custom" {
        settings.llm_custom_base_url = settings.llm_base_url.trim().to_owned();
    }
    if !settings.llm_api_key.trim().is_empty() {
        settings.llm_api_keys.insert(credential_scope(settings), settings.llm_api_key.clone());
    }
}

fn credential_masks(settings: &VoiceChatSettings, internal_ark: Option<&str>) -> BTreeMap<String, String> {
    let mut masks: BTreeMap<_, _> = settings.llm_api_keys.iter().filter(|(_, key)| !key.trim().is_empty())
        .map(|(scope, key)| (scope.clone(), mask_secret(key))).collect();
    if !settings.llm_api_key.trim().is_empty() {
        masks.insert(credential_scope(settings), mask_secret(&settings.llm_api_key));
    }
    if let Some(key) = internal_ark.filter(|key| !key.trim().is_empty()) {
        masks.entry("doubao".into()).or_insert_with(|| mask_secret(key));
    }
    masks
}

/// Keep each saved credential in its own scope. An empty input reuses only the
/// target provider/endpoint's key, never the previously selected provider's key.
pub fn save(input: VoiceChatSettingsInput) -> Result<VoiceChatSettingsStatus, String> {
    let path = settings_path()?;
    let settings = merge_settings(read_file(&path)?.unwrap_or_default(), input);
    write_file(&path, &settings)?;
    status()
}

fn merge_settings(mut settings: VoiceChatSettings, input: VoiceChatSettingsInput) -> VoiceChatSettings {
    if let Some(enabled) = input.llm_web_search { settings.llm_web_search = Some(enabled); }
    remember_active_key(&mut settings);
    let previous_scope = credential_scope(&settings);
    if let Some(value) = input.llm_provider {
        let provider = normalize_provider(&value);
        if normalize_provider(&settings.llm_provider) != provider {
            settings.llm_model.clear();
            settings.llm_base_url.clear();
            if provider == "custom" { settings.llm_base_url = settings.llm_custom_base_url.clone(); }
        }
        settings.llm_provider = provider;
    }
    if let Some(value) = input.llm_base_url {
        settings.llm_base_url = value.trim().to_string();
    }
    let next_scope = credential_scope(&settings);
    if previous_scope != next_scope || settings.llm_api_key.trim().is_empty() {
        settings.llm_api_key = settings.llm_api_keys.get(&next_scope).cloned().unwrap_or_default();
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
    remember_active_key(&mut settings);
    settings
}

pub fn clear_secret(which: &str) -> Result<VoiceChatSettingsStatus, String> {
    let path = settings_path()?;
    let mut settings = read_file(&path)?.unwrap_or_default();
    match which {
        "llm" => {
            settings.llm_api_keys.remove(&credential_scope(&settings));
            settings.llm_api_key.clear();
        },
        "tts" => settings.tts_api_key.clear(),
        _ => return Err("unknown secret".to_string()),
    }
    write_file(&path, &settings)?;
    status()
}

pub fn llm_config() -> Result<LlmConfig, String> {
    let (settings, _) = load()?;
    Ok(LlmConfig {
        web_search: settings.llm_web_search.unwrap_or(true),
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
    fn provider_changes_drop_old_secrets_even_when_input_is_blank_or_masked() {
        for from in ["deepseek", "doubao", "mimo", "custom"] {
            for to in ["deepseek", "doubao", "mimo", "custom"] {
                for incoming in [None, Some(""), Some("  "), Some("tes…mask"), Some("••••")] {
                    let stored = VoiceChatSettings { llm_provider: from.into(), llm_api_key:"old-test-key".into(),
                        llm_model:"saved-model".into(), ..Default::default() };
                    let merged = merge_settings(stored, VoiceChatSettingsInput { llm_provider:Some(to.into()),
                        llm_api_key:incoming.map(str::to_owned), ..Default::default() });
                    assert_eq!(merged.llm_api_key, if from == to {"old-test-key"} else {""}, "{from} -> {to}");
                    assert_eq!(merged.llm_model, if from == to {"saved-model"} else {""});
                }
            }
        }
    }

    #[test]
    fn switching_and_saving_a_new_key_preserves_each_provider_in_its_own_slot() {
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().join(FILE_NAME);
        let previous = VoiceChatSettings {llm_provider:"doubao".into(),llm_api_key:"old-test-key".into(),..Default::default()};
        let next = merge_settings(previous, VoiceChatSettingsInput {llm_provider:Some("mimo".into()),
            llm_api_key:Some("  new-test-key  ".into()), ..Default::default()});
        write_file(&path, &next).unwrap();
        let mut saved = read_file(&path).unwrap().unwrap();
        apply_llm_defaults(&mut saved);
        assert_eq!(saved.llm_provider,"mimo"); assert_eq!(saved.llm_api_key,"new-test-key");
        assert_eq!(saved.llm_model,"mimo-v2.6-flash");
        assert_eq!(saved.llm_api_keys.get("doubao").map(String::as_str),Some("old-test-key"));
        let restored = merge_settings(saved,VoiceChatSettingsInput {llm_provider:Some("doubao".into()),..Default::default()});
        assert_eq!(restored.llm_api_key,"old-test-key");
        assert_eq!(restored.llm_api_keys.get("mimo").map(String::as_str),Some("new-test-key"));
    }

    #[test]
    fn custom_endpoint_changes_require_a_new_key_but_model_changes_do_not() {
        let previous = VoiceChatSettings {llm_provider:"custom".into(),llm_base_url:"https://example.invalid/v1".into(),
            llm_api_key:"old-test-key".into(),..Default::default()};
        for endpoint in ["https://another.invalid/v1", "https://example.invalid/v2", ""] {
            let merged = merge_settings(previous.clone(), VoiceChatSettingsInput {llm_base_url:Some(endpoint.into()),..Default::default()});
            assert!(merged.llm_api_key.is_empty());
        }
        let same = merge_settings(previous.clone(), VoiceChatSettingsInput {
            llm_provider:Some("custom".into()),llm_base_url:Some(" https://example.invalid/v1/ ".into()),
            llm_model:Some("other-model".into()),..Default::default()});
        assert_eq!(same.llm_api_key,"old-test-key"); assert_eq!(same.llm_model,"other-model");
        let replaced = merge_settings(previous, VoiceChatSettingsInput {llm_base_url:Some("https://new.invalid".into()),
            llm_api_key:Some("new-test-key".into()), ..Default::default()});
        assert_eq!(replaced.llm_api_key,"new-test-key");
    }

    #[test]
    fn switch_to_doubao_uses_only_its_internal_default_and_never_carries_it_back() {
        let previous = VoiceChatSettings {llm_provider:"deepseek".into(),llm_api_key:"deepseek-test-key".into(),..Default::default()};
        let mut next = merge_settings(previous,VoiceChatSettingsInput {llm_provider:Some("doubao".into()),..Default::default()});
        assert!(next.llm_api_key.is_empty());
        assert!(!apply_internal_ark_default(&mut next,None));
        assert!(apply_internal_ark_default(&mut next,Some("ark-test-key")));
        let mut back = merge_settings(next,VoiceChatSettingsInput {llm_provider:Some("deepseek".into()),..Default::default()});
        assert!(!apply_internal_ark_default(&mut back,Some("ark-test-key")));
        assert_eq!(back.llm_api_key,"deepseek-test-key");
    }

    #[test]
    fn each_provider_and_custom_endpoint_restores_its_saved_key_after_restart() {
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().join(FILE_NAME);
        let cases = [("deepseek", "", "ds-test-key"), ("doubao", "", "db-test-key"),
            ("mimo", "", "mi-test-key"), ("custom", "https://one.invalid/v1", "one-test-key"),
            ("custom", "https://two.invalid/v1", "two-test-key")];
        let mut settings = VoiceChatSettings::default();
        for (provider, endpoint, key) in cases {
            settings = merge_settings(settings,VoiceChatSettingsInput {llm_provider:Some(provider.into()),
                llm_base_url:Some(endpoint.into()),llm_api_key:Some(key.into()),..Default::default()});
            write_file(&path,&settings).unwrap(); settings=read_file(&path).unwrap().unwrap();
        }
        for (provider, endpoint, key) in cases.into_iter().rev() {
            settings = merge_settings(settings,VoiceChatSettingsInput {llm_provider:Some(provider.into()),
                llm_base_url:Some(endpoint.into()),..Default::default()});
            assert_eq!(settings.llm_api_key,key);
            let masks = credential_masks(&settings,None);
            assert_eq!(masks.len(),5);
            for (_, _, raw) in cases { assert!(!serde_json::to_string(&masks).unwrap().contains(raw)); }
            write_file(&path,&settings).unwrap(); settings=read_file(&path).unwrap().unwrap();
        }
        settings = merge_settings(settings,VoiceChatSettingsInput {llm_provider:Some("custom".into()),..Default::default()});
        assert_eq!(settings.llm_base_url,"https://one.invalid/v1");
        assert_eq!(settings.llm_api_key,"one-test-key");
    }

    #[test]
    fn legacy_single_key_is_migrated_only_to_its_recorded_provider() {
        let mut legacy: VoiceChatSettings = serde_json::from_str(r#"{"llmProvider":"mimo","llmApiKey":"legacy-mimo-key"}"#).unwrap();
        remember_active_key(&mut legacy);
        assert_eq!(legacy.llm_api_keys.len(),1);
        assert_eq!(legacy.llm_api_keys.get("mimo").map(String::as_str),Some("legacy-mimo-key"));
        let masks = credential_masks(&legacy,Some("internal-ark-key"));
        assert!(masks.contains_key("doubao")); assert!(!masks.contains_key("deepseek"));
    }

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
    fn presets_fill_endpoint_and_model_and_unknown_providers_fall_back_to_doubao() {
        assert_eq!(normalize_provider("Doubao"), "doubao");
        assert_eq!(normalize_provider("nope"), "doubao");
        assert_eq!(normalize_provider("custom"), "custom");
        let preset = llm_preset("mimo").unwrap();
        assert_eq!(preset.base_url, "https://api.xiaomimimo.com/v1");
        assert_eq!(preset.model, "mimo-v2.6-flash");
        assert_eq!(LLM_PRESETS.len(), 3);
    }

    #[test]
    fn editable_preset_models_survive_persistence_and_defaults_only_fill_blanks() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(FILE_NAME);
        for preset in LLM_PRESETS {
            let mut settings = VoiceChatSettings {
                llm_provider: preset.id.into(), llm_model: "  account-model-id  ".into(),
                llm_base_url: "https://untrusted.invalid".into(), ..Default::default()
            };
            write_file(&path, &settings).unwrap();
            settings = read_file(&path).unwrap().unwrap();
            apply_llm_defaults(&mut settings);
            assert_eq!(settings.llm_model, "account-model-id");
            assert_eq!(settings.llm_base_url, preset.base_url);
            apply_llm_defaults(&mut settings);
            assert_eq!(settings.llm_model, "account-model-id");
            settings.llm_model = "  ".into();
            apply_llm_defaults(&mut settings);
            assert_eq!(settings.llm_model, preset.model);
        }
        assert_eq!(llm_preset("deepseek").unwrap().model, "deepseek-flash");
        assert_eq!(llm_preset("doubao").unwrap().model, "doubao-seed-2-0-lite-260428");
        let mut custom = VoiceChatSettings { llm_provider: "custom".into(),
            llm_base_url: "https://example.invalid/v1".into(), llm_model: "my-model".into(), ..Default::default() };
        apply_llm_defaults(&mut custom);
        assert_eq!(custom.llm_model, "my-model");
        assert_eq!(custom.llm_base_url, "https://example.invalid/v1");
    }

    #[test]
    fn masks_keep_only_head_and_tail() {
        assert_eq!(mask_secret(""), "");
        assert_eq!(mask_secret("short"), "••••");
        assert_eq!(mask_secret("sk-1234567890abcdef"), "sk-…cdef");
    }

    #[test]
    fn fresh_install_uses_doubao_but_saved_choices_and_legacy_keys_stay_scoped() {
        let mut fresh = VoiceChatSettings::default();
        apply_llm_defaults(&mut fresh);
        assert_eq!(fresh.llm_provider, "doubao");
        assert_eq!(fresh.llm_model, llm_preset("doubao").unwrap().model);
        assert!(fresh.llm_api_key.is_empty());
        let dir=tempfile::tempdir().unwrap(); let path=dir.path().join(FILE_NAME);
        let legacy=VoiceChatSettings{llm_api_key:"legacy-test-key".into(),..Default::default()};
        write_file(&path,&legacy).unwrap();
        let mut loaded=read_file(&path).unwrap().unwrap(); apply_llm_defaults(&mut loaded);
        assert_eq!(loaded.llm_provider,"deepseek");
        let switched=merge_settings(loaded,VoiceChatSettingsInput{llm_provider:Some("doubao".into()),..Default::default()});
        assert!(switched.llm_api_key.is_empty());
        for provider in ["deepseek","mimo","custom"] {
            let mut saved=VoiceChatSettings{llm_provider:provider.into(),llm_model:"my-model".into(),..Default::default()};
            apply_llm_defaults(&mut saved); assert_eq!(saved.llm_provider,provider); assert_eq!(saved.llm_model,"my-model");
        }
    }

    #[test]
    fn search_opt_out_persists_and_old_mimo_preset_migrates() {
        let mut settings=VoiceChatSettings{llm_provider:"mimo".into(),llm_model:"mimo-v2.6".into(),..Default::default()};
        apply_llm_defaults(&mut settings); assert_eq!(settings.llm_model,"mimo-v2.6-flash");
        assert!(settings.llm_web_search.unwrap_or(true));
        settings=merge_settings(settings,VoiceChatSettingsInput{llm_web_search:Some(false),..Default::default()});
        let decoded:VoiceChatSettings=serde_json::from_value(serde_json::to_value(&settings).unwrap()).unwrap();
        assert_eq!(decoded.llm_web_search,Some(false));
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
        stored = merge_settings(stored, VoiceChatSettingsInput {llm_api_key:Some(masked),..Default::default()});
        assert_eq!(stored.llm_api_key, "sk-realkey123456");
    }
}
