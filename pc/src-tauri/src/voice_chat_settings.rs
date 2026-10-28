/*
 * [Input] 对话大模型（DeepSeek / 豆包 / MiMo 预设或自定义 OpenAI 兼容端点）与豆包 Seed TTS 2.0
 *         的凭据；ASR/TTS 共用语音识别凭据存储，旧版独立 TTS Key 仅用于无 ASR Key 时迁移。
 * [Output] One authenticated encrypted JSON file under the app data dir (`voice-chat-settings.json`,
 *          local private key on macOS / OS vault on Windows, 0600 on unix), masked status for the UI, and resolved LLM/TTS configs for the realtime chat
 *          pipeline. LLM keys are saved per provider (custom keys also per endpoint),
 *          with only masks exposed to the UI. Separate global realtime volume persistence migrates legacy per-pet gain once.
 *          Public builds embed nothing; internal builds may embed defaults.
 * [Pos] Tauri-side credential store for realtime persona chat (mirrors volcengine_asr's file mode).
 * [Sync] If fields change, update `ApiSettings.jsx`, `pc/.folder.md`, and `pc/docs/realtime-chat.md`.
 */

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

use crate::doubao_tts::{DoubaoTtsConfig, DEFAULT_RESOURCE_ID as TTS_DEFAULT_RESOURCE_ID};
use crate::persona_llm::{LlmConfig, DEFAULT_BASE_URL as LLM_DEFAULT_BASE_URL, DEFAULT_MODEL as LLM_DEFAULT_MODEL};

const FILE_NAME: &str = "voice-chat-settings.json";

static STORAGE_DIR: OnceLock<PathBuf> = OnceLock::new();

// Separate from credentials and per-appearance voices. Saving volume must not
// rewrite keys, and switching appearance must never restore an old gain.
const VOLUME_FILE: &str = "realtime-voice-volume.json";
static VOLUME_LOCK: Mutex<()> = Mutex::new(());
static SETTINGS_MUTATION: Mutex<()> = Mutex::new(());

fn valid_volume(volume: f32) -> bool {
    volume.is_finite() && (0.2..=1.5).contains(&volume)
}

fn write_volume_at(dir: &Path, volume: f32) -> Result<f32, String> {
    if !valid_volume(volume) { return Err("实时对话音量须在 0.20–1.50 之间".into()); }
    fs::create_dir_all(dir).map_err(|_| "无法创建音量配置目录")?;
    let mut temp = tempfile::NamedTempFile::new_in(dir).map_err(|_| "无法创建音量配置临时文件")?;
    let bytes = serde_json::to_vec(&serde_json::json!({"version":1,"volume":volume})).map_err(|_| "音量配置无效")?;
    temp.write_all(&bytes).and_then(|_| temp.as_file().sync_all()).map_err(|_| "无法写入音量配置")?;
    temp.persist(dir.join(VOLUME_FILE)).map_err(|_| "无法保存音量配置")?;
    Ok(volume)
}

fn read_volume_at(dir: &Path) -> Result<f32, String> {
    match fs::read(dir.join(VOLUME_FILE)) {
        Ok(raw) => {
            let value: serde_json::Value = serde_json::from_slice(&raw).map_err(|_| "全局音量配置损坏，请重新保存音量")?;
            return value["volume"].as_f64().map(|v| v as f32).filter(|v| valid_volume(*v))
                .ok_or_else(|| "全局音量配置无效，请重新保存音量".into());
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {},
        Err(_) => return Err("无法读取全局音量配置".into()),
    }
    // One-time migration: preserve the user's most recently saved voice gain.
    // Do not read media or credentials, follow symlinks, or export persona text.
    let mut latest = None;
    if let Ok(entries) = fs::read_dir(dir.join("custom-appearances")) {
        for entry in entries.take(1024).flatten() {
            if !entry.file_type().is_ok_and(|t| t.is_dir()) { continue; }
            let path = entry.path().join("persona-voice.json");
            let Ok(meta) = fs::symlink_metadata(&path) else { continue };
            if !meta.is_file() || meta.len() > 65536 { continue; }
            let Ok(raw) = fs::read(&path) else { continue };
            let Ok(value) = serde_json::from_slice::<serde_json::Value>(&raw) else { continue };
            let Some(volume) = value["voice"]["volume"].as_f64().map(|v| v as f32).filter(|v| valid_volume(*v)) else { continue };
            let Ok(modified) = meta.modified() else { continue };
            if latest.is_none_or(|(time, _)| modified > time) { latest = Some((modified, volume)); }
        }
    }
    write_volume_at(dir, latest.map(|(_, v)| v).unwrap_or(1.0))
}

pub fn realtime_volume() -> Result<f32, String> {
    let _lock = VOLUME_LOCK.lock().map_err(|_| "音量配置暂不可用")?;
    read_volume_at(STORAGE_DIR.get().ok_or("语音对话配置目录尚未初始化")?)
}

pub fn save_realtime_volume(volume: f32) -> Result<f32, String> {
    let _lock = VOLUME_LOCK.lock().map_err(|_| "音量配置暂不可用")?;
    write_volume_at(STORAGE_DIR.get().ok_or("语音对话配置目录尚未初始化")?, volume)
}

/// Built-in chat defaults: endpoints are fixed, model names may be overridden.
pub struct LlmPreset {
    pub id: &'static str,
    pub label: &'static str,
    pub base_url: &'static str,
    pub model: &'static str,
}

pub const LLM_PRESETS: &[LlmPreset] = &[
    LlmPreset { id: "deepseek", label: "DeepSeek", base_url: "https://api.deepseek.com", model: "deepseek-flash" },
    LlmPreset { id: "doubao", label: "豆包（火山方舟）", base_url: "https://ark.cn-beijing.volces.com/api/v3", model: "doubao-seed-2-1-lite-260915" },
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
    match crate::secure_config::read(path,"voice-chat")? {
        Some(raw) => serde_json::from_str::<VoiceChatSettings>(&raw)
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
            .map_err(|_| "语音对话配置格式损坏；请保留原文件".into()),
        None => Ok(None),
    }
}

fn write_file(path: &Path, settings: &VoiceChatSettings) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("创建配置目录失败: {error}"))?;
    }
    let raw = serde_json::to_string_pretty(settings).map_err(|error| error.to_string())?;
    crate::secure_config::write(path,"voice-chat",raw.as_bytes())
}

/// Stored user credentials and public model defaults; no embedded credentials.
pub fn load() -> Result<(VoiceChatSettings, &'static str), String> {
    let stored = read_file(&settings_path()?)?.unwrap_or_default();
    let mut settings = stored;
    remember_active_key(&mut settings);
    if settings.llm_api_key.trim().is_empty() {
        settings.llm_api_key = settings.llm_api_keys.get(&credential_scope(&settings)).cloned().unwrap_or_default();
    }
    apply_llm_defaults(&mut settings);
    settings.tts_resource_id = TTS_DEFAULT_RESOURCE_ID.to_string();
    Ok((settings, "local"))
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
    let _guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
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
        llm_api_key_masks: credential_masks(&settings),
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

fn credential_masks(settings: &VoiceChatSettings) -> BTreeMap<String, String> {
    let mut masks: BTreeMap<_, _> = settings.llm_api_keys.iter().filter(|(_, key)| !key.trim().is_empty())
        .map(|(scope, key)| (scope.clone(), mask_secret(key))).collect();
    if !settings.llm_api_key.trim().is_empty() {
        masks.insert(credential_scope(settings), mask_secret(&settings.llm_api_key));
    }
    masks
}

/// Keep each saved credential in its own scope. An empty input reuses only the
/// target provider/endpoint's key, never the previously selected provider's key.
pub fn save(input: VoiceChatSettingsInput) -> Result<VoiceChatSettingsStatus, String> {
    let guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
    let path = settings_path()?;
    let settings = merge_settings(read_file(&path)?.unwrap_or_default(), input);
    write_file(&path, &settings)?;
    drop(guard);
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
    let guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
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
    drop(guard);
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

fn catalog_settings(settings: VoiceChatSettings, input: VoiceChatSettingsInput) -> Result<VoiceChatSettings, String> {
    let mut settings = merge_settings(settings, input);
    if settings.llm_provider == "custom" && settings.llm_base_url.trim().is_empty() {
        return Err("请先填写自定义接口地址".into());
    }
    apply_llm_defaults(&mut settings);
    if settings.llm_api_key.trim().is_empty() { return Err("请先保存或填写这家服务的 API Key".into()); }
    let url = reqwest::Url::parse(&settings.llm_base_url).map_err(|_| "接口地址无效")?;
    if url.scheme() != "https" || !url.username().is_empty() || url.password().is_some()
        || url.query().is_some() || url.fragment().is_some() {
        return Err("模型目录需要不含凭据的 HTTPS 接口地址".into());
    }
    Ok(settings)
}

fn catalog_model_ids(value: &serde_json::Value) -> Result<Vec<String>, String> {
    let rows = value["data"].as_array().filter(|_| value["has_more"] != true)
        .ok_or("模型目录不完整，请手动填写模型 ID")?;
    let mut ids = std::collections::BTreeSet::new();
    for row in rows.iter().take(4096) {
        let Some(id) = row["id"].as_str().map(str::trim).filter(|id| !id.is_empty() && id.len() <= 200) else { continue };
        if ["available", "enabled", "active"].iter().any(|key| row[*key] == false) { continue; }
        let status = row["lifecycle"]["status"].as_str().or_else(|| row["lifecycle"].as_str()).or_else(|| row["status"].as_str()).unwrap_or("").to_ascii_lowercase();
        if ["retir", "shutdown", "disabled", "unavailable"].iter().any(|word| status.contains(word)) { continue; }
        if row["modalities"]["output_modalities"].as_array().is_some_and(|outputs| !outputs.iter().any(|o| o == "text")) { continue; }
        if ["seedance", "seedream", "embedding", "rerank", "tts", "asr"].iter().any(|word| id.to_ascii_lowercase().contains(word)) { continue; }
        ids.insert(id.to_owned());
    }
    Ok(ids.into_iter().collect())
}

/// Read-only: scope the credential exactly like save, but never persist the
/// draft or return secrets. Redirects are disabled by the shared network layer.
pub async fn list_models(input: VoiceChatSettingsInput) -> Result<Vec<String>, String> {
    let settings = catalog_settings(load()?.0, input)?;
    let client = crate::llm_network::client(std::time::Duration::from_secs(15))?;
    let mut response = client.get(format!("{}/models", settings.llm_base_url.trim_end_matches('/')))
        .bearer_auth(&settings.llm_api_key).send().await.map_err(|e| crate::llm_network::connection_error(&e))?;
    if !response.status().is_success() { return Err(format!("模型目录获取失败（HTTP {}），请检查该服务的 Key 和权限；可手动填写模型 ID", response.status().as_u16())); }
    let mut bytes = Vec::new();
    while let Some(chunk) = response.chunk().await.map_err(|_| "模型目录读取中断")? {
        if bytes.len() + chunk.len() > 1024 * 1024 { return Err("模型目录过大，请手动填写模型 ID".into()); }
        bytes.extend_from_slice(&chunk);
    }
    let value = serde_json::from_slice(&bytes).map_err(|_| "模型目录格式不正确")?;
    catalog_model_ids(&value)
}

/// News retrieval stays on the explicitly selected Doubao native search,
/// independently of the model used to edit the briefing. Never persist a
/// provider switch or reuse another provider's (or speech) credential.
pub fn news_search_config() -> Result<LlmConfig, String> {
    news_search_config_from(&load()?.0)
}

fn news_search_config_from(settings:&VoiceChatSettings)->Result<LlmConfig,String>{
    if settings.llm_web_search==Some(false){
        return Err("请在 API 配置启用联网检索，资讯搜索使用豆包".into());
    }
    let preset=llm_preset("doubao").expect("built-in Doubao preset");
    let active=settings.llm_provider=="doubao";
    let key=if active&&!settings.llm_api_key.trim().is_empty(){Some(&settings.llm_api_key)}
        else{settings.llm_api_keys.get("doubao")}
        .filter(|s|!s.trim().is_empty()).ok_or("请先在 API 配置保存豆包对话模型的 Key，用于资讯联网检索；语音合成 Key 不适用")?;
    Ok(LlmConfig{
        web_search:true,base_url:preset.base_url.into(),api_key:key.trim().into(),
        model:if active&&!settings.llm_model.trim().is_empty(){settings.llm_model.trim().into()}else{preset.model.into()},
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
    #[test]
    fn catalog_uses_only_target_provider_or_exact_endpoint_key_and_never_mutates_input() {
        let mut settings = VoiceChatSettings { llm_provider: "deepseek".into(), llm_api_key: "ds-secret".into(), ..Default::default() };
        settings.llm_api_keys.insert("doubao".into(), "ark-secret".into());
        settings.llm_api_keys.insert("custom:https://one.invalid/v1".into(), "custom-secret".into());
        let selected = catalog_settings(settings.clone(), VoiceChatSettingsInput { llm_provider: Some("doubao".into()), ..Default::default() }).unwrap();
        assert_eq!(selected.llm_api_key, "ark-secret");
        assert_eq!(selected.llm_base_url, "https://ark.cn-beijing.volces.com/api/v3");
        assert_eq!(selected.llm_model, "doubao-seed-2-1-lite-260915");
        let custom = |url: &str| VoiceChatSettingsInput { llm_provider: Some("custom".into()), llm_base_url: Some(url.into()), ..Default::default() };
        assert!(catalog_settings(settings.clone(), custom("https://two.invalid/v1")).is_err());
        assert_eq!(catalog_settings(settings.clone(), custom("https://one.invalid/v1/")).unwrap().llm_api_key, "custom-secret");
        assert_eq!(settings.llm_api_key, "ds-secret");
    }

    #[test]
    fn model_catalog_returns_only_active_text_ids_and_checks_completeness() {
        let ids = catalog_model_ids(&serde_json::json!({"data":[
            {"id":"doubao-seed-2-1-lite-260915","modalities":{"output_modalities":["text"]}},
            {"id":"doubao-seed-2-1-pro-260915"}, {"id":"retired","status":"Retiring"},
            {"id":"gone","status":"Shutdown"}, {"id":"off","enabled":false},
            {"id":"audio","modalities":{"output_modalities":["audio"]}},
            {"id":"doubao-seedance-2-0-fast-test"}, {"id":"doubao-seed-2-1-pro-260915"}
        ]})).unwrap();
        assert_eq!(ids, ["doubao-seed-2-1-lite-260915", "doubao-seed-2-1-pro-260915"]);
        assert!(catalog_model_ids(&serde_json::json!({"data":[], "has_more":true})).is_err());
    }

    #[test]
    fn news_search_uses_only_doubao_credential_and_keeps_chat_settings(){
        let mut settings=VoiceChatSettings{llm_provider:"deepseek".into(),llm_model:"deepseek-flash".into(),llm_api_key:"other-provider-key".into(),tts_api_key:"speech-only-key".into(),..Default::default()};
        assert!(news_search_config_from(&settings).is_err());
        settings.llm_api_keys.insert("doubao".into(),"doubao-test-key".into());
        let cfg=news_search_config_from(&settings).unwrap();
        assert_eq!(cfg.api_key,"doubao-test-key");
        assert_eq!(cfg.base_url,llm_preset("doubao").unwrap().base_url);
        assert_eq!(cfg.model,llm_preset("doubao").unwrap().model);
        assert_eq!(settings.llm_provider,"deepseek");
        assert_eq!(settings.llm_api_key,"other-provider-key");
        settings.llm_provider="doubao".into();settings.llm_model="my-doubao-endpoint".into();settings.llm_api_key="active-doubao-key".into();
        assert_eq!(news_search_config_from(&settings).unwrap().model,"my-doubao-endpoint");
        assert_eq!(news_search_config_from(&settings).unwrap().api_key,"active-doubao-key");
        settings.llm_web_search=Some(false);assert!(news_search_config_from(&settings).is_err());
    }
    #[test]
    fn global_volume_migrates_once_and_ignores_future_pet_changes() {
        let dir = tempfile::tempdir().unwrap();
        let pet = dir.path().join("custom-appearances/pet-a");
        std::fs::create_dir_all(&pet).unwrap();
        std::fs::write(pet.join("persona-voice.json"), r#"{"voice":{"volume":0.3},"persona":{"private":"never exported"}}"#).unwrap();
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.3);
        std::fs::write(pet.join("persona-voice.json"), r#"{"voice":{"volume":1.5}}"#).unwrap();
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.3);
        super::write_volume_at(dir.path(), 0.7).unwrap();
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.7);
        let saved = std::fs::read_to_string(dir.path().join(super::VOLUME_FILE)).unwrap();
        assert!(!saved.contains("private") && !saved.contains("pet-a"));
        for invalid in [f32::NAN, f32::INFINITY, -1.0, 0.0, 1.6] {
            assert!(super::write_volume_at(dir.path(), invalid).is_err());
        }
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.7);
    }

    #[test]
    fn global_volume_defaults_and_corruption_is_not_silently_full_volume() {
        let dir = tempfile::tempdir().unwrap();
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 1.0);
        std::fs::write(dir.path().join(super::VOLUME_FILE), "broken").unwrap();
        assert!(super::read_volume_at(dir.path()).is_err());
        super::write_volume_at(dir.path(), 0.3).unwrap();
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.3);
    }
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
    fn switching_provider_never_borrows_a_key() {
        let previous = VoiceChatSettings {llm_provider:"deepseek".into(),llm_api_key:"deepseek-test-key".into(),..Default::default()};
        let next = merge_settings(previous,VoiceChatSettingsInput {llm_provider:Some("doubao".into()),..Default::default()});
        assert!(next.llm_api_key.is_empty());
        let back = merge_settings(next,VoiceChatSettingsInput {llm_provider:Some("deepseek".into()),..Default::default()});
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
            let masks = credential_masks(&settings);
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
        let masks = credential_masks(&legacy);
        assert!(masks.contains_key("mimo")); assert!(!masks.contains_key("doubao")); assert!(!masks.contains_key("deepseek"));
    }

    #[test]
    fn public_model_defaults_never_create_or_overwrite_user_keys() {
        for provider in ["doubao", "deepseek", "mimo", "custom", "unknown"] {
            let mut fresh = VoiceChatSettings { llm_provider:provider.into(), ..Default::default() };
            apply_llm_defaults(&mut fresh);
            assert!(fresh.llm_api_key.is_empty());
            fresh.llm_api_key="fixture-user-key".into();
            apply_llm_defaults(&mut fresh);
            assert_eq!(fresh.llm_api_key,"fixture-user-key");
        }
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
        assert_eq!(llm_preset("doubao").unwrap().model, "doubao-seed-2-1-lite-260915");
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
