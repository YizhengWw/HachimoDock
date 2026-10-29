/*
 * [Input] Content-addressed petui publications plus ~/.claw-pet legacy draft roots.
 * [Output] Content-addressed formal local component library with one-time lossless
 *          legacy migration, atomic staging/publish, latest-version discovery,
 *          strict deletion guards, bounded ZIP extraction, and scene/sprite preview metadata.
 * [Pos] Local component-library ownership node in pc/src-tauri/src
 * [Sync] If this file changes, update pc/.folder.md.
 */

use base64::Engine;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const CLAW_PET_DIR_NAME: &str = ".claw-pet";
const LEGACY_OPENCLAW_DIR_NAME: &str = ".openclaw";
const LEGACY_DRAFTS_DIR_NAME: &str = "component-drafts";
const COMPONENTS_DIR_NAME: &str = "components";
const COMPONENT_STAGING_DIR_NAME: &str = ".staging";
const COMPONENT_LIBRARY_DIR_NAME: &str = "library";
static LIBRARY_MUTATION: std::sync::Mutex<()> = std::sync::Mutex::new(());
static BUNDLED_CONTENT: std::sync::OnceLock<std::sync::Mutex<std::collections::HashSet<(String,String)>>> = std::sync::OnceLock::new();
const LEGACY_MIGRATION_MARKER: &str = ".legacy-component-migration-v1.json";
// Import budgets exceed the bounded widget/sprite contract while preventing ZIP bombs.
const COMPONENT_IMPORT_MAX_FILES: usize = 256;
const COMPONENT_IMPORT_MAX_BYTES: u64 = 16 * 1024 * 1024;
const FORBIDDEN_COMPONENT_SUFFIXES: &[&str] = &[
    "bat", "cmd", "com", "css", "dll", "dylib", "exe", "html", "js", "mjs", "cjs", "ps1", "py",
    "sh", "so", "svg",
];

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ComponentLibraryEntry {
    pub id: String,
    pub name: String,
    pub author: String,
    pub description: String,
    pub version: String,
    pub path: String,
    pub version_hash: String,
    pub is_bundled_copy: bool,
    pub mtime_ms: u64,
    pub created_at_ms: u64,
    pub dashboard: HashMap<String, String>,
    pub buttons: Vec<serde_json::Value>,
    pub game_type: Option<String>,
    pub runtime_engine: Option<String>,
    pub data_source: Option<String>,
    pub media_source: Option<String>,
    pub pc_extension: Option<serde_json::Value>,
    pub media_surface: Option<serde_json::Value>,
    pub app_runtime: Option<serde_json::Value>,
    pub input_policy: Option<serde_json::Value>,
    pub pc_application: Option<serde_json::Value>,
    pub instrument_source: Option<String>,
    pub scene_engine: Option<String>,
    pub game_preset: Option<String>,
    pub scene: Option<serde_json::Value>,
    pub scene_sprites: Vec<ComponentSceneSpritePreview>,
    pub kind: Option<String>,
    pub valid: bool,
    pub validation_errors: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ComponentSceneSpritePreview {
    pub id: String,
    pub data_url: String,
    pub frame_width: u32,
    pub frame_height: u32,
    pub frames: u32,
    pub fps: u32,
}

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LegacyMigrationSummary {
    pub schema_version: u32,
    pub completed_at_ms: u64,
    pub migrated_count: u32,
    pub skipped_count: u32,
    pub retained_source_roots: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ComponentLibrarySnapshot {
    pub components: Vec<ComponentLibraryEntry>,
    pub library_path: String,
    pub migration: LegacyMigrationSummary,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DeleteLibraryComponentInput {
    pub path: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeleteLibraryComponentResult {
    pub ok: bool,
    pub deleted_component_id: String,
    pub deleted_path: String,
    pub recovery_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RemovedComponent {
    pub recovery_id: String,
    pub component_id: String,
    pub name: String,
    pub deleted_at_ms: u64,
}
fn trash_root(home: &Path) -> PathBuf { library_root(home).parent().unwrap().join(".trash") }
pub fn removed(home: &Path) -> Result<Vec<RemovedComponent>, String> {
    let root=trash_root(home);
    if !root.exists(){return Ok(vec![]);}
    let mut records=Vec::new();
    for entry in fs::read_dir(root).map_err(|e|e.to_string())?.flatten() {
        if entry.file_type().map_err(|e|e.to_string())?.is_symlink(){continue;}
        let path=entry.path();
        if let Ok(bytes)=fs::read(path.join("record.json")) {
            if let Ok(record)=serde_json::from_slice::<RemovedComponent>(&bytes) {
                if uuid::Uuid::parse_str(&record.recovery_id).is_ok() && path.file_name().and_then(|s|s.to_str())==Some(record.recovery_id.as_str()) && crate::component_extension::valid_id(&record.component_id) && path.join(&record.component_id).is_dir(){records.push(record);}
            }
        }
    }
    records.sort_by_key(|r|std::cmp::Reverse(r.deleted_at_ms));
    Ok(records)
}
pub fn restore(home: &Path, recovery_id: &str) -> Result<(), String> {
    let _guard=LIBRARY_MUTATION.lock().map_err(|_|"组件库繁忙")?;
    let record=removed(home)?.into_iter().find(|r|r.recovery_id==recovery_id).ok_or("找不到可恢复的组件")?;
    let root=trash_root(home).canonicalize().map_err(|e|e.to_string())?;
    let source=root.join(recovery_id).join(&record.component_id).canonicalize().map_err(|e|e.to_string())?;
    if source.parent().and_then(|p|p.parent())!=Some(root.as_path()){return Err("恢复路径无效".into());}
    let target=library_root(home).join(&record.component_id);
    if target.exists(){return Err("本机已存在同 ID 组件。为避免覆盖，请先将当前本地副本移入回收站，再恢复所选版本。".into());}
    fs::rename(source,target).map_err(|e|format!("恢复未完成，备份仍保留：{e}"))
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis() as u64)
        .unwrap_or(0)
}

fn metadata_timestamp_ms(value: Option<SystemTime>) -> u64 {
    value
        .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
        .map(|duration| duration.as_millis() as u64)
        .unwrap_or(0)
}

fn metadata_created_at_ms(metadata: &fs::Metadata) -> u64 {
    metadata_timestamp_ms(metadata.created().ok().or_else(|| metadata.modified().ok()))
}

fn components_root(home: &Path) -> PathBuf {
    home.join(CLAW_PET_DIR_NAME).join(COMPONENTS_DIR_NAME)
}

pub fn library_root(home: &Path) -> PathBuf {
    components_root(home).join(COMPONENT_LIBRARY_DIR_NAME)
}

fn staging_root(home: &Path) -> PathBuf {
    components_root(home).join(COMPONENT_STAGING_DIR_NAME)
}

fn ensure_layout(home: &Path) -> Result<(), String> {
    fs::create_dir_all(library_root(home))
        .map_err(|error| format!("创建正式组件库失败: {}", error))?;
    fs::create_dir_all(staging_root(home))
        .map_err(|error| format!("创建组件事务目录失败: {}", error))?;
    Ok(())
}

fn copy_dir_recursive_safe(source: &Path, destination: &Path) -> Result<(), String> {
    let metadata = fs::symlink_metadata(source)
        .map_err(|error| format!("读取组件源失败 {}: {}", source.display(), error))?;
    if metadata.file_type().is_symlink() {
        return Err(format!("组件包不允许符号链接: {}", source.display()));
    }
    fs::create_dir_all(destination)
        .map_err(|error| format!("创建事务目录失败 {}: {}", destination.display(), error))?;
    for entry in fs::read_dir(source)
        .map_err(|error| format!("读取组件目录失败 {}: {}", source.display(), error))?
    {
        let entry = entry.map_err(|error| error.to_string())?;
        let entry_source = entry.path();
        let entry_destination = destination.join(entry.file_name());
        let entry_type = entry
            .file_type()
            .map_err(|error| format!("读取组件文件类型失败: {}", error))?;
        if entry_type.is_symlink() {
            return Err(format!("组件包不允许符号链接: {}", entry_source.display()));
        }
        if entry_type.is_dir() {
            copy_dir_recursive_safe(&entry_source, &entry_destination)?;
        } else if entry_type.is_file() {
            fs::copy(&entry_source, &entry_destination).map_err(|error| {
                format!("复制组件文件失败 {}: {}", entry_source.display(), error)
            })?;
        }
    }
    Ok(())
}

fn extract_zip_safe(source: &Path, destination: &Path) -> Result<(), String> {
    fs::create_dir_all(destination).map_err(|error| error.to_string())?;
    let file = fs::File::open(source)
        .map_err(|error| format!("打开组件 zip 失败 {}: {}", source.display(), error))?;
    let mut archive = zip::ZipArchive::new(file)
        .map_err(|error| format!("读取组件 zip 失败 {}: {}", source.display(), error))?;
    if archive.len() > COMPONENT_IMPORT_MAX_FILES {
        return Err("组件 zip 文件数量超过导入限制".to_string());
    }
    let mut remaining = COMPONENT_IMPORT_MAX_BYTES;
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index).map_err(|error| error.to_string())?;
        if entry.size() > remaining {
            return Err("组件 zip 解压大小超过导入限制".to_string());
        }
        if entry.name().contains(['\\', ':']) {
            return Err("组件 zip 含不安全的平台路径".to_string());
        }
        if entry
            .unix_mode()
            .is_some_and(|mode| mode & 0o170000 == 0o120000)
        {
            return Err(format!("组件 zip 不允许符号链接: {}", entry.name()));
        }
        let relative = entry
            .enclosed_name()
            .ok_or_else(|| format!("组件 zip 含越界路径: {}", entry.name()))?;
        let output = destination.join(relative);
        if entry.is_dir() {
            fs::create_dir_all(&output).map_err(|error| error.to_string())?;
            continue;
        }
        if let Some(parent) = output.parent() {
            fs::create_dir_all(parent).map_err(|error| error.to_string())?;
        }
        let mut output_file = fs::OpenOptions::new().write(true).create_new(true).open(&output)
            .map_err(|error| format!("组件 zip 含重复或无法写入的路径: {}", error))?;
        let copied = std::io::copy(&mut entry.by_ref().take(remaining + 1), &mut output_file)
            .map_err(|error| error.to_string())?;
        remaining = remaining.checked_sub(copied)
            .ok_or_else(|| "组件 zip 解压大小超过导入限制".to_string())?;
    }
    Ok(())
}

fn collect_package_files(
    root: &Path,
    current: &Path,
    files: &mut Vec<PathBuf>,
) -> Result<(), String> {
    for entry in fs::read_dir(current)
        .map_err(|error| format!("读取组件目录失败 {}: {}", current.display(), error))?
    {
        let entry = entry.map_err(|error| error.to_string())?;
        let path = entry.path();
        let file_type = entry.file_type().map_err(|error| error.to_string())?;
        if file_type.is_symlink() {
            return Err(format!("组件包不允许符号链接: {}", path.display()));
        }
        if file_type.is_dir() {
            collect_package_files(root, &path, files)?;
        } else if file_type.is_file() {
            path.strip_prefix(root)
                .map_err(|_| "组件哈希路径越界".to_string())?;
            files.push(path);
        }
    }
    Ok(())
}

pub(crate) fn package_content_hash(root: &Path) -> Result<String, String> {
    let mut files = Vec::new();
    collect_package_files(root, root, &mut files)?;
    files.sort_by(|left, right| {
        let left = left
            .strip_prefix(root)
            .unwrap_or(left)
            .to_string_lossy()
            .replace('\\', "/");
        let right = right
            .strip_prefix(root)
            .unwrap_or(right)
            .to_string_lossy()
            .replace('\\', "/");
        left.cmp(&right)
    });
    let mut hasher = Sha256::new();
    for path in files {
        let relative = path
            .strip_prefix(root)
            .map_err(|_| "组件哈希路径越界".to_string())?
            .to_string_lossy()
            .replace('\\', "/");
        hasher.update(relative.as_bytes());
        hasher.update([0]);
        let bytes = fs::read(&path)
            .map_err(|error| format!("读取组件文件失败 {}: {}", path.display(), error))?;
        hasher.update(bytes);
        hasher.update([0]);
    }
    let digest = format!("{:x}", hasher.finalize());
    Ok(digest[..16].to_string())
}

fn validate_package_file_policy(root: &Path) -> Result<(), String> {
    let mut files = Vec::new();
    collect_package_files(root, root, &mut files)?;
    for path in files {
        let suffix = path
            .extension()
            .and_then(|value| value.to_str())
            .unwrap_or("")
            .to_ascii_lowercase();
        if FORBIDDEN_COMPONENT_SUFFIXES.contains(&suffix.as_str()) {
            let relative = path.strip_prefix(root).unwrap_or(&path);
            return Err(format!(
                "组件包不允许可执行或脚本文件: {}",
                relative.display()
            ));
        }
    }
    Ok(())
}

fn atomic_write_json(path: &Path, value: &impl Serialize) -> Result<(), String> {
    let parent = path
        .parent()
        .ok_or_else(|| "JSON 目标目录无效".to_string())?;
    fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    let temp = parent.join(format!(
        ".{}.{}.tmp",
        path.file_name().unwrap_or_default().to_string_lossy(),
        uuid::Uuid::new_v4()
    ));
    let bytes = serde_json::to_vec_pretty(value).map_err(|error| error.to_string())?;
    let mut file = fs::File::create(&temp).map_err(|error| error.to_string())?;
    file.write_all(&bytes).map_err(|error| error.to_string())?;
    file.write_all(b"\n").map_err(|error| error.to_string())?;
    file.sync_all().map_err(|error| error.to_string())?;
    fs::rename(&temp, path).map_err(|error| {
        let _ = fs::remove_file(&temp);
        format!("写入组件迁移记录失败: {}", error)
    })
}

fn read_json_from_dir(path: &Path, relative: &str) -> Option<serde_json::Value> {
    fs::read(path.join(relative))
        .ok()
        .and_then(|bytes| serde_json::from_slice(&bytes).ok())
}
fn application_preview(path:&Path)->Option<serde_json::Value>{
    let mut v=read_json_from_dir(path,"pc/application.json")?;
    if v["engine"]=="pc-app-services-v3"&&v["service"]=="knowledge"||matches!(v["service"].as_str(),Some("calendar"|"books")){
        // The list view needs controls, not a multi-megabyte article database.
        v["content"]=serde_json::json!([]);
    }
    Some(v)
}

fn scene_sprite_previews(
    path: &Path,
    runtime: Option<&serde_json::Value>,
) -> Vec<ComponentSceneSpritePreview> {
    runtime
        .and_then(|value| value.get("scene").or_else(||value.get("surface")))
        .and_then(|value| value.get("sprites").or_else(||value.get("images")))
        .and_then(|value| value.as_array())
        .into_iter()
        .flatten()
        .filter_map(|sprite| {
            let id = sprite.get("id")?.as_str()?.to_string();
            let asset = sprite.get("asset")?.as_str()?;
            let bytes = fs::read(path.join(asset)).ok()?;
            Some(ComponentSceneSpritePreview {
                id,
                data_url: format!(
                    "data:image/png;base64,{}",
                    base64::engine::general_purpose::STANDARD.encode(bytes)
                ),
                frame_width: u32::try_from(sprite.get("frame_width")?.as_u64()?).ok()?,
                frame_height: u32::try_from(sprite.get("frame_height")?.as_u64()?).ok()?,
                frames: u32::try_from(sprite.get("frames")?.as_u64()?).ok()?,
                fps: u32::try_from(sprite.get("fps")?.as_u64()?).ok()?,
            })
        })
        .collect()
}

fn entry_from_directory(path: &Path) -> Result<ComponentLibraryEntry, String> {
    let metadata = fs::metadata(path)
        .map_err(|error| format!("读取组件目录失败 {}: {}", path.display(), error))?;
    let validation = crate::clawpkg::validate_clawpkg_at_path(path)?;
    let fallback_manifest = read_json_from_dir(path, "component.json");
    let (id, name, dashboard) = if let Some(manifest) = validation.manifest.as_ref() {
        (
            manifest.id.clone(),
            manifest.name.clone(),
            manifest.dashboard.clone(),
        )
    } else {
        let id = fallback_manifest
            .as_ref()
            .and_then(|value| value.get("id"))
            .and_then(|value| value.as_str())
            .unwrap_or("")
            .to_string();
        if id.is_empty() {
            return Err(format!("组件缺少可读取的 ID: {}", path.display()));
        }
        let name = fallback_manifest
            .as_ref()
            .and_then(|value| value.get("name"))
            .and_then(|value| value.as_str())
            .unwrap_or(&id)
            .to_string();
        (id, name, HashMap::new())
    };
    let description = fallback_manifest
        .as_ref()
        .and_then(|value| value.get("description"))
        .and_then(|value| value.as_str())
        .unwrap_or("")
        .to_string();
    let kind = fallback_manifest
        .as_ref()
        .and_then(|value| value.get("kind"))
        .and_then(|value| value.as_str())
        .map(str::to_string);
    let version = fallback_manifest
        .as_ref()
        .and_then(|value| value.get("version"))
        .and_then(|value| value.as_str())
        .unwrap_or("")
        .to_string();
    let runtime = read_json_from_dir(path, "runtime/widget.json");
    let game_type = runtime
        .as_ref()
        .and_then(|value| value.get("game"))
        .and_then(|value| value.get("type"))
        .and_then(|value| value.as_str())
        .map(str::to_string);
    let runtime_engine = runtime
        .as_ref()
        .and_then(|value| value.get("engine"))
        .and_then(|value| value.as_str())
        .map(str::to_string);
    let scene_engine = runtime
        .as_ref()
        .filter(|value| value.get("scene").is_some())
        .map(|value| {
            if value["scene"]["engine"]=="p4-touch-slice-v1" {"p4-touch-slice-v1".to_string()}
            else if runtime_engine.as_deref() == Some("p4-bounded-runtime-v4") {
                "p4-grid-scene-v2".to_string()
            } else {
                "p4-grid-scene-v1".to_string()
            }
        });
    let scene = runtime
        .as_ref()
        .and_then(|value| value.get("scene"))
        .cloned();
    let scene_sprites = if validation.ok {
        scene_sprite_previews(path, runtime.as_ref())
    } else {
        Vec::new()
    };
    let game_preset = game_type.clone();
    let buttons = if validation.ok {
        read_json_from_dir(path, "buttons.json")
            .and_then(|value| value.as_array().cloned())
            .unwrap_or_default()
    } else {
        Vec::new()
    };
    let version_hash = match path
        .file_name()
        .and_then(|value| value.to_str())
        .filter(|value| value.len() == 16 && value.chars().all(|ch| ch.is_ascii_hexdigit()))
        .map(str::to_string) {
            Some(hash) => hash,
            None => package_content_hash(path)?,
        };
    Ok(ComponentLibraryEntry {
        id: id.clone(),
        name,
        author: fallback_manifest.as_ref().and_then(|v|v.get("author")).and_then(|v|v.as_str()).map(|v|crate::widget_data::bounded_text(v,120)).unwrap_or_default(),
        description,
        version,
        path: path.display().to_string(),
        is_bundled_copy: BUNDLED_CONTENT.get_or_init(Default::default).lock().unwrap().contains(&(id.clone(),version_hash.clone())),
        version_hash,
        mtime_ms: metadata_timestamp_ms(metadata.modified().ok()),
        created_at_ms: metadata_created_at_ms(&metadata),
        dashboard,
        buttons,
        game_type,
        runtime_engine,
        data_source: runtime.as_ref().and_then(|v| v.get("data")).and_then(|v| v.get("source")).and_then(|v| v.as_str()).map(str::to_string),
        media_source: runtime.as_ref().and_then(|v| v.get("media")).and_then(|v| v.get("source")).and_then(|v| v.as_str()).map(str::to_string),
        pc_extension: if validation.ok {read_json_from_dir(path,"pc/extension.json")}else{None},
        media_surface: if validation.ok {runtime.as_ref().and_then(|v|v.get("surface")).cloned()}else{None},
        app_runtime: if validation.ok {runtime.as_ref().filter(|v|v["surface"]["engine"]=="p4-app-surface-v1").cloned()}else{None},
        input_policy: if validation.ok {runtime.as_ref().and_then(|v|v.get("input")).cloned()}else{None},
        pc_application: if validation.ok {application_preview(path)}else{None},
        instrument_source: runtime.as_ref().and_then(|v| v.get("instrument")).and_then(|v| v.get("source")).and_then(|v| v.as_str()).map(str::to_string),
        scene_engine,
        game_preset,
        scene,
        scene_sprites,
        kind,
        valid: validation.ok,
        validation_errors: validation.errors,
    })
}

pub(crate) fn publish_source(home: &Path, source: &Path) -> Result<ComponentLibraryEntry, String> {
    ensure_layout(home)?;
    let validation = crate::clawpkg::validate_clawpkg_at_path(source)?;
    if !validation.ok {
        return Err(format!("组件包校验失败: {}", validation.errors.join("; ")));
    }
    let manifest = validation
        .manifest
        .ok_or_else(|| "组件包缺少可发布 manifest".to_string())?;
    let job_root = staging_root(home).join(format!("desktop-import-{}", uuid::Uuid::new_v4()));
    let staged_package = job_root.join("package");
    let staged_result = if source.is_dir() {
        copy_dir_recursive_safe(source, &staged_package)
    } else if source.is_file() {
        extract_zip_safe(source, &staged_package)
    } else {
        Err(format!("组件源不存在: {}", source.display()))
    };
    if let Err(error) = staged_result {
        let _ = fs::remove_dir_all(&job_root);
        return Err(error);
    }
    let publish_result = (|| {
        validate_package_file_policy(&staged_package)?;
        let staged_validation = crate::clawpkg::validate_clawpkg_at_path(&staged_package)?;
        if !staged_validation.ok {
            return Err(format!(
                "组件复制后校验失败: {}",
                staged_validation.errors.join("; ")
            ));
        }
        let version_hash = package_content_hash(&staged_package)?;
        let component_root = library_root(home).join(&manifest.id);
        let destination = component_root.join(&version_hash);
        fs::create_dir_all(&component_root).map_err(|error| error.to_string())?;
        if destination.exists() {
            let existing_hash = package_content_hash(&destination)?;
            if existing_hash != version_hash {
                return Err(format!("正式组件目录哈希冲突: {}", destination.display()));
            }
            return entry_from_directory(&destination);
        }
        if let Err(error) = fs::rename(&staged_package, &destination) {
            if destination.exists() {
                let existing_hash = package_content_hash(&destination)?;
                if existing_hash == version_hash {
                    return entry_from_directory(&destination);
                }
            }
            return Err(format!("原子发布组件失败: {}", error));
        }
        entry_from_directory(&destination)
    })();
    let _ = fs::remove_dir_all(&job_root);
    publish_result
}

/// Install build-owned resource packages through the same validated atomic
/// importer as downloaded components. Keep every previous/user revision and
/// all host state; never copy the user's library into a distributable build.
pub(crate) fn ensure_bundled(home: &Path, root: &Path) -> Result<(), String> {
    let _guard=LIBRARY_MUTATION.lock().map_err(|_|"组件库繁忙")?;
    if !root.is_dir() { return Ok(()); }
    let removed_ids:std::collections::HashSet<_>=removed(home)?.into_iter().map(|r|r.component_id).collect();
    for item in fs::read_dir(root).map_err(|e|e.to_string())? {
        let source=item.map_err(|e|e.to_string())?.path();
        if !source.is_dir() || !source.join("component.json").is_file() { continue; }
        let manifest=read_json_from_dir(&source,"component.json").ok_or("内置组件清单不可读")?;
        let id=manifest["id"].as_str().filter(|id|crate::component_extension::valid_id(id)).ok_or("内置组件 ID 无效")?;
        // Delisted components must not be silently restored by an upgrade.
        if matches!(id,"music-player"|"hachimi-agent"|"hachimi-desk") { continue; }
        if removed_ids.contains(id) {continue;}
        let hash=package_content_hash(&source)?;
        BUNDLED_CONTENT.get_or_init(Default::default).lock().unwrap().insert((id.to_owned(),hash.clone()));
        let destination=library_root(home).join(id).join(&hash);
        if destination.is_dir() && package_content_hash(&destination)?==hash {continue;}
        publish_source(home,&source)?;
    }
    Ok(())
}

/// Built-in resources are immutable within a packaged process. Retry failures;
/// don't re-hash every bundled PNG on each UI refresh. Developer mode remains live.
pub(crate) fn ensure_bundled_for_ui(home:&Path,root:&Path)->Result<(),String>{
    if cfg!(debug_assertions){return ensure_bundled(home,root);}
    static SEEDED:std::sync::OnceLock<std::sync::Mutex<std::collections::HashSet<(PathBuf,PathBuf)>>> = std::sync::OnceLock::new();
    let mut done=SEEDED.get_or_init(Default::default).lock().map_err(|_|"组件初始化繁忙")?;
    let key=(home.to_path_buf(),root.to_path_buf());
    if done.contains(&key){return Ok(());}
    ensure_bundled(home,root)?;if root.is_dir(){done.insert(key);}Ok(())
}

// Device host requests can arrive before the component-center view has ever
// mounted (especially on a clean installation). Share the same atomic seed
// gate with the UI; never require a user to visit a page to install PC data.
static BUNDLED_HOST_ROOT: std::sync::OnceLock<(PathBuf,PathBuf)> = std::sync::OnceLock::new();
/// Recovery may use only the shipped, validated package for the same identity.
/// Never restore a deliberately removed component or substitute a community update.
pub(crate) fn bundled_repair_source(id:&str)->Result<Option<ComponentLibraryEntry>,String>{
    if !crate::component_extension::valid_id(id){return Ok(None);}
    let Some((home,root))=BUNDLED_HOST_ROOT.get()else{return Ok(None);};
    if removed(home)?.iter().any(|r|r.component_id==id){return Ok(None);}
    let path=root.join(id);if !path.is_dir(){return Ok(None);}
    let entry=inspect(&path)?;
    if !entry.valid||entry.id!=id{return Err("内置恢复包校验失败，设备原数据未修改".into());}
    Ok(Some(entry))
}
pub(crate) fn configure_bundled_host(home:PathBuf,root:PathBuf){
    if BUNDLED_HOST_ROOT.set((home.clone(),root.clone())).is_err(){return;}
    std::thread::spawn(move||{
        if ensure_bundled_for_host(&home,&root).is_err(){
            eprintln!("[components] bundled initialization failed; host requests may retry");
        }
    });
}
fn ensure_bundled_for_host(home:&Path,root:&Path)->Result<(),String>{
    if !root.is_dir(){return Err("客户端内置组件资源缺失，请重新安装客户端；原数据未修改".into());}
    ensure_bundled_for_ui(home,root).map_err(|_|"客户端组件资源初始化失败，请检查磁盘空间后重试；原数据保留".into())
}

fn preview_fingerprint(path:&Path)->Result<Vec<u8>,String>{
    fn visit(path:&Path,depth:usize,count:&mut usize,hash:&mut Sha256)->Result<(),String>{
        *count+=1;if depth>16||*count>10000{return Err("组件预览文件过多".into());}
        let meta=fs::symlink_metadata(path).map_err(|_|"组件预览不可读")?;
        if meta.file_type().is_symlink(){return Err("组件预览不允许符号链接".into());}
        hash.update(path.as_os_str().to_string_lossy().as_bytes());hash.update(meta.len().to_le_bytes());
        hash.update(meta.modified().map_err(|_|"无法读取修改时间")?.duration_since(UNIX_EPOCH).unwrap_or_default().as_nanos().to_le_bytes());
        if meta.is_dir(){let mut entries=fs::read_dir(path).map_err(|_|"组件预览不可读")?.collect::<Result<Vec<_>,_>>().map_err(|_|"组件预览不可读")?;
            entries.sort_by_key(|e|e.path());for entry in entries{visit(&entry.path(),depth+1,count,hash)?;}}
        Ok(())
    }
    let mut hash=Sha256::new();visit(path,0,&mut 0,&mut hash)?;Ok(hash.finalize().to_vec())
}

/// UI-only metadata/image cache. Execution and sync still use full validation.
fn preview_entry(path:&Path)->Result<ComponentLibraryEntry,String>{
    type Cache=Vec<(PathBuf,Vec<u8>,std::time::Instant,ComponentLibraryEntry)>;
    static CACHE:std::sync::OnceLock<std::sync::Mutex<Cache>>=std::sync::OnceLock::new();
    let fingerprint=preview_fingerprint(path)?;
    let mut cache=CACHE.get_or_init(Default::default).lock().map_err(|_|"组件预览繁忙")?;
    if let Some((_,_,_,entry))=cache.iter().find(|(p,fp,at,_)|p==path&&fp==&fingerprint&&at.elapsed()<std::time::Duration::from_secs(300)){return Ok(entry.clone());}
    let mut entry=entry_from_directory(path)?;
    if package_content_hash(path)?!=entry.version_hash {entry.valid=false;entry.validation_errors.push("组件内容已变化，请重新导入".into());}
    cache.retain(|(p,_,_,_)|p!=path);if cache.len()>=64{cache.remove(0);}
    cache.push((path.to_path_buf(),fingerprint,std::time::Instant::now(),entry.clone()));Ok(entry)
}

fn legacy_roots(home: &Path) -> Vec<PathBuf> {
    vec![
        home.join(CLAW_PET_DIR_NAME).join(LEGACY_DRAFTS_DIR_NAME),
        home.join(LEGACY_OPENCLAW_DIR_NAME)
            .join(LEGACY_DRAFTS_DIR_NAME),
    ]
}

fn legacy_candidates(root: &Path) -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    let Ok(entries) = fs::read_dir(root) else {
        return candidates;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            if path.join("component.json").is_file() {
                candidates.push(path);
            } else if let Ok(children) = fs::read_dir(&path) {
                for child in children.flatten() {
                    let child_path = child.path();
                    if child_path.is_dir() && child_path.join("component.json").is_file() {
                        candidates.push(child_path);
                    }
                }
            }
        } else if path
            .extension()
            .and_then(|value| value.to_str())
            .is_some_and(|value| {
                value.eq_ignore_ascii_case("zip") || value.eq_ignore_ascii_case("clawpkg")
            })
        {
            candidates.push(path);
        }
    }
    candidates
}

fn migrate_legacy_once(home: &Path) -> Result<LegacyMigrationSummary, String> {
    ensure_layout(home)?;
    let marker = components_root(home).join(LEGACY_MIGRATION_MARKER);
    if marker.is_file() {
        if let Ok(bytes) = fs::read(&marker) {
            if let Ok(summary) = serde_json::from_slice::<LegacyMigrationSummary>(&bytes) {
                return Ok(summary);
            }
        }
    }
    let roots = legacy_roots(home);
    let mut summary = LegacyMigrationSummary {
        schema_version: 1,
        completed_at_ms: now_ms(),
        migrated_count: 0,
        skipped_count: 0,
        retained_source_roots: roots
            .iter()
            .filter(|root| root.exists())
            .map(|root| root.display().to_string())
            .collect(),
    };
    for root in roots.iter().filter(|root| root.exists()) {
        for candidate in legacy_candidates(root) {
            match publish_source(home, &candidate) {
                Ok(_) => summary.migrated_count += 1,
                Err(error) => {
                    summary.skipped_count += 1;
                    eprintln!(
                        "[component-library] legacy source retained but not migrated {}: {}",
                        candidate.display(),
                        error
                    );
                }
            }
        }
    }
    summary.completed_at_ms = now_ms();
    atomic_write_json(&marker, &summary)?;
    Ok(summary)
}

fn compare_component_versions(left: &ComponentLibraryEntry, right: &ComponentLibraryEntry) -> std::cmp::Ordering {
    // Publishing tools preserve source directory timestamps; content hashes do
    // not order releases. Prefer declared SemVer, then dates for same-version
    // edits and legacy packages. Option ordering keeps mixed legacy/SemVer
    // comparisons transitive; build metadata does not affect release precedence.
    let parse = |value: &str| semver::Version::parse(value.trim()).ok().map(|mut v| {
        v.build = semver::BuildMetadata::EMPTY;
        v
    });
    parse(&right.version).cmp(&parse(&left.version))
        .then_with(|| right.created_at_ms.cmp(&left.created_at_ms))
        .then_with(|| right.mtime_ms.cmp(&left.mtime_ms))
        .then_with(|| right.version_hash.cmp(&left.version_hash))
}

fn scan_latest_versions(home: &Path) -> Result<Vec<ComponentLibraryEntry>, String> {
    let root = library_root(home);
    let mut components = Vec::new();
    for component_dir in fs::read_dir(&root)
        .map_err(|error| format!("读取正式组件库失败 {}: {}", root.display(), error))?
        .flatten()
    {
        let component_path = component_dir.path();
        if !component_path.is_dir() {
            continue;
        }
        let mut versions: Vec<ComponentLibraryEntry> = fs::read_dir(&component_path)
            .into_iter()
            .flatten()
            .flatten()
            .filter_map(|entry| {
                let path = entry.path();
                if !path.is_dir() || !path.join("component.json").is_file() {
                    return None;
                }
                match preview_entry(&path) {
                    Ok(component) => Some(component),
                    Err(error) => {
                        eprintln!(
                            "[component-library] skip unreadable version {}: {}",
                            path.display(),
                            error
                        );
                        None
                    }
                }
            })
            .collect();
        versions.sort_by(compare_component_versions);
        if let Some(latest) = versions.into_iter().next() {
            components.push(latest);
        }
    }
    components.sort_by(|left, right| {
        right
            .created_at_ms
            .cmp(&left.created_at_ms)
            .then_with(|| right.mtime_ms.cmp(&left.mtime_ms))
            .then_with(|| left.id.cmp(&right.id))
    });
    Ok(components)
}

pub fn list(home: &Path) -> Result<ComponentLibrarySnapshot, String> {
    ensure_layout(home)?;
    let migration = migrate_legacy_once(home)?;
    Ok(ComponentLibrarySnapshot {
        components: scan_latest_versions(home)?,
        library_path: library_root(home).display().to_string(),
        migration,
    })
}

/// Targeted resolution for host events; do not re-decode every installed package.
/// Cache only parsed host metadata. Re-hash actual bytes on every request:
/// timestamps alone must never authorize modified executable/service declarations.
fn host_entry(path:&Path)->Result<ComponentLibraryEntry,String>{
    type Cache=Vec<(PathBuf,String,Option<SystemTime>,ComponentLibraryEntry)>;
    static CACHE:std::sync::OnceLock<std::sync::Mutex<Cache>>=std::sync::OnceLock::new();
    let hash=package_content_hash(path)?;
    let modified=fs::metadata(path).map_err(|_|"组件目录不可读")?.modified().ok();
    let cache=CACHE.get_or_init(Default::default);
    if let Some(entry)=cache.lock().map_err(|_|"组件缓存不可用")?.iter()
        .find(|(p,h,m,_)|p==path&&h==&hash&&m==&modified).map(|(_,_,_,v)|v.clone()){return Ok(entry);}
    let mut entry=entry_from_directory(path)?;
    // Host events never need base64 artwork or a second copy of the knowledge book.
    entry.scene_sprites.clear();entry.pc_application=None;
    if hash!=entry.version_hash {entry.valid=false;entry.validation_errors.push("组件内容已变化，请重新导入".into());}
    if entry.valid{
        let mut cache=cache.lock().map_err(|_|"组件缓存不可用")?;
        cache.retain(|(p,_,_,_)|p!=path);
        if cache.len()>=8{cache.remove(0);}
        cache.push((path.into(),hash,modified,entry.clone()));
    }
    Ok(entry)
}
pub(crate) fn latest(home:&Path,id:&str)->Result<Option<ComponentLibraryEntry>,String>{
    if !crate::component_extension::valid_id(id){return Err("组件 ID 无效".into());}
    if let Some((configured_home,root))=BUNDLED_HOST_ROOT.get(){
        if home==configured_home {ensure_bundled_for_host(home,root)?;}
    }
    let root=library_root(home).join(id);
    if !fs::symlink_metadata(&root).is_ok_and(|m|m.is_dir()&&!m.file_type().is_symlink()){return Ok(None);}
    let mut versions=Vec::new();
    for e in fs::read_dir(root).map_err(|_|"无法读取组件库")?.take(128).flatten(){
        if !e.file_type().is_ok_and(|m|m.is_dir()){continue;}
        if let Ok(v)=host_entry(&e.path()){if v.id==id{versions.push(v);}}
    }
    versions.sort_by(compare_component_versions);Ok(versions.into_iter().next())
}

pub fn inspect(path: &Path) -> Result<ComponentLibraryEntry, String> {
    if path.is_dir() {
        return entry_from_directory(path);
    }
    let temporary = tempfile::tempdir().map_err(|error| error.to_string())?;
    let extracted = temporary.path().join("package");
    extract_zip_safe(path, &extracted)?;
    let mut entry = entry_from_directory(&extracted)?;
    entry.path = path.display().to_string();
    Ok(entry)
}

pub fn delete(
    home: &Path,
    input: DeleteLibraryComponentInput,
) -> Result<DeleteLibraryComponentResult, String> {
    let _guard=LIBRARY_MUTATION.lock().map_err(|_|"组件库繁忙")?;
    ensure_layout(home)?;
    let root = library_root(home)
        .canonicalize()
        .map_err(|error| format!("无法解析正式组件库: {}", error))?;
    let target = PathBuf::from(&input.path);
    if !target.is_absolute() {
        return Err("正式组件路径必须是绝对路径".to_string());
    }
    let target = target
        .canonicalize()
        .map_err(|error| format!("无法解析正式组件路径: {}", error))?;
    if !target.is_dir() || !target.join("component.json").is_file() {
        return Err("只能删除正式组件库中的单个已发布组件".to_string());
    }
    let component_root = target
        .parent()
        .ok_or_else(|| "正式组件路径层级无效".to_string())?;
    if component_root.parent() != Some(root.as_path()) || component_root == root {
        return Err("拒绝删除正式组件库之外的路径".to_string());
    }
    let component_id = component_root
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| "正式组件 ID 无效".to_string())?
        .to_string();
    let recovery_id=uuid::Uuid::new_v4().to_string();
    let backup=trash_root(home).join(&recovery_id);
    fs::create_dir_all(&backup).map_err(|e|e.to_string())?;
    let record=RemovedComponent{recovery_id:recovery_id.clone(),component_id:component_id.clone(),name:read_json_from_dir(&target,"component.json").and_then(|m|m["name"].as_str().map(str::to_owned)).unwrap_or_else(||component_id.clone()),deleted_at_ms:now_ms()};
    fs::write(backup.join("record.json"),serde_json::to_vec(&record).map_err(|e|e.to_string())?).map_err(|e|e.to_string())?;
    fs::rename(component_root,backup.join(&component_id)).map_err(|e|format!("移入回收站失败，原组件保留：{e}"))?;
    Ok(DeleteLibraryComponentResult {
        ok: true,
        deleted_component_id: component_id,
        deleted_path: component_root.display().to_string(),
        recovery_id,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fresh_host_can_resolve_news_without_opening_component_center(){
        let home=tempfile::tempdir().unwrap();
        let root=PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs");
        assert!(latest(home.path(),"daily-news").unwrap().is_none());
        ensure_bundled_for_host(home.path(),&root).unwrap();
        let entry=latest(home.path(),"daily-news").unwrap().unwrap();
        assert!(entry.valid && entry.app_runtime.is_some());
        ensure_bundled_for_host(home.path(),&root).unwrap();
        assert_eq!(latest(home.path(),"daily-news").unwrap().unwrap().version_hash,entry.version_hash);
    }

    #[test]
    fn missing_bundled_host_root_is_not_reported_as_component_validation_failure(){
        let home=tempfile::tempdir().unwrap();
        let error=ensure_bundled_for_host(home.path(),&home.path().join("missing")).unwrap_err();
        assert!(error.contains("内置组件资源缺失"));
        assert!(!library_root(home.path()).exists());
    }

    #[test]
    fn ui_preview_cache_revalidates_changed_files() {
        let home=tempfile::tempdir().unwrap();
        let source=PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs/hachimi-weather");
        let published=publish_source(home.path(),&source).unwrap();
        let path=PathBuf::from(&published.path);
        assert!(preview_entry(&path).unwrap().valid);
        assert!(preview_entry(&path).unwrap().valid);
        let file=path.join("pc/application.json");
        let original=fs::read(&file).unwrap();let mut changed=original.clone();changed.push(b' ');
        fs::write(&file,changed).unwrap();
        assert!(!preview_entry(&path).unwrap().valid);
        fs::write(&file,original).unwrap();
        assert!(preview_entry(&path).unwrap().valid);
    }

    #[test]
    fn host_cache_reuses_art_free_metadata_but_never_hides_content_changes(){
        let home=tempfile::tempdir().unwrap();
        let source=PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs/hachimi-weather");
        let published=publish_source(home.path(),&source).unwrap();
        let first=latest(home.path(),"hachimi-weather").unwrap().unwrap();
        assert!(first.valid);assert!(first.scene_sprites.is_empty());assert!(first.pc_application.is_none());
        assert_eq!(latest(home.path(),"hachimi-weather").unwrap().unwrap().version_hash,first.version_hash);
        let file=PathBuf::from(&published.path).join("pc/application.json");
        let original=fs::read(&file).unwrap();let mut edited=original.clone();edited.push(b' ');
        fs::write(&file,&edited).unwrap();
        assert!(!latest(home.path(),"hachimi-weather").unwrap().unwrap().valid);
        fs::write(&file,original).unwrap();
        assert!(latest(home.path(),"hachimi-weather").unwrap().unwrap().valid);
    }

    #[test]
    fn bundled_catalog_is_complete_idempotent_and_keeps_user_revisions() {
        let home=tempfile::tempdir().unwrap();
        let root=PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs");
        ensure_bundled(home.path(),&root).unwrap();
        let first=scan_latest_versions(home.path()).unwrap();
        assert_eq!(first.len(),28);
        assert!(first.iter().all(|c|c.is_bundled_copy));
        for id in ["cloud-hop","daily-almanac","book-pocket","daily-news","decision-capsule","answer-book","hope-train"] {
            assert!(first.iter().any(|c|c.id==id&&c.valid));
        }
        assert!(!first.iter().any(|c|matches!(c.id.as_str(),"music-player"|"hachimi-agent"|"hachimi-desk")));
        let learn=first.iter().find(|c|c.id=="hachimi-learn").unwrap();
        assert_eq!(learn.name,"每日新知");assert!(learn.pc_application.is_some());
        assert_eq!(first.iter().find(|c|c.id=="two-key-pong").unwrap().author,"弹珠汽水");
        let before:HashMap<_,_>=first.iter().map(|c|(c.id.clone(),c.version_hash.clone())).collect();
        ensure_bundled(home.path(),&root).unwrap();
        let after:HashMap<_,_>=scan_latest_versions(home.path()).unwrap().iter().map(|c|(c.id.clone(),c.version_hash.clone())).collect();
        assert_eq!(before,after);
        // Importing resources must not create host recordings, todos or API settings.
        assert!(!home.path().join(".claw-pet/component-apps").exists());
    }

    #[test]
    fn generic_media_package_keeps_extension_and_surface_after_library_scan() {
        let home = tempfile::tempdir().unwrap();
        let source = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../tests/fixtures/media-sample");
        publish_source(home.path(), &source).unwrap();
        let entries = scan_latest_versions(home.path()).unwrap();
        assert_eq!(entries.len(), 1);
        let entry = &entries[0];
        assert_eq!(entry.id, "media-sample");
        assert!(entry.valid, "{:?}", entry.validation_errors);
        assert!(!entry.is_bundled_copy);
        assert!(entry.pc_extension.is_some());
        assert!(entry.media_surface.is_some());
        assert_eq!(entry.media_source.as_deref(), Some("pc.media-sample"));
    }

    fn write_package(path: &Path, id: &str) {
        fs::create_dir_all(path.join("runtime")).unwrap();
        fs::create_dir_all(path.join("assets")).unwrap();
        fs::write(
            path.join("component.json"),
            format!(
                r#"{{"id":"{}","name":"Timer","version":"1.0.0","kind":"tool","description":"test"}}"#,
                id
            ),
        )
        .unwrap();
        fs::write(
            path.join("negative-screen.json"),
            br#"{"dashboard":{"title":"Timer"}}"#,
        )
        .unwrap();
        fs::write(path.join("buttons.json"), b"[]").unwrap();
        fs::write(path.join("runtime/widget.json"), br#"{"schema_version":1}"#).unwrap();
        fs::write(path.join("share.json"), br#"{"title":"Timer"}"#).unwrap();
        fs::write(path.join("assets/.keep"), b"").unwrap();
    }

    #[test]
    fn publish_list_and_delete_use_formal_content_addressed_paths() {
        let temp = tempfile::tempdir().unwrap();
        let home = temp.path().join("home");
        let source = temp.path().join("source");
        write_package(&source, "timer");

        let published = publish_source(&home, &source).unwrap();
        assert_eq!(published.version_hash.len(), 16);
        assert!(published.path.contains("components"));
        assert!(published.path.contains("library"));

        let snapshot = list(&home).unwrap();
        assert_eq!(snapshot.components.len(), 1);
        assert_eq!(snapshot.components[0].id, "timer");

        let deleted = delete(
            &home,
            DeleteLibraryComponentInput {
                path: published.path,
            },
        )
        .unwrap();
        assert_eq!(deleted.deleted_component_id, "timer");
        assert!(list(&home).unwrap().components.is_empty());
        let records=removed(&home).unwrap();
        assert_eq!(records.len(),1);
        assert_eq!(records[0].recovery_id,deleted.recovery_id);
        restore(&home,&deleted.recovery_id).unwrap();
        assert_eq!(list(&home).unwrap().components[0].id,"timer");
        assert!(removed(&home).unwrap().is_empty());
    }

    #[test]
    fn recovery_never_overwrites_new_content_and_rejects_traversal() {
        let temp=tempfile::tempdir().unwrap();let home=temp.path().join("home");let source=temp.path().join("source");
        write_package(&source,"timer");
        let first=publish_source(&home,&source).unwrap();
        let deleted=delete(&home,DeleteLibraryComponentInput{path:first.path}).unwrap();
        assert!(restore(&home,"../library").is_err());
        publish_source(&home,&source).unwrap();
        assert!(restore(&home,&deleted.recovery_id).unwrap_err().contains("同 ID"));
        assert_eq!(removed(&home).unwrap().len(),1);
        assert_eq!(list(&home).unwrap().components.len(),1);
    }

    #[test]
    fn removed_builtin_is_not_reseeded_while_backup_is_available() {
        let temp=tempfile::tempdir().unwrap();let home=temp.path().join("home");let bundled=temp.path().join("bundled");
        write_package(&bundled.join("timer"),"timer");
        ensure_bundled(&home,&bundled).unwrap();
        let first=list(&home).unwrap().components.remove(0);
        let deleted=delete(&home,DeleteLibraryComponentInput{path:first.path}).unwrap();
        ensure_bundled(&home,&bundled).unwrap();
        assert!(list(&home).unwrap().components.is_empty());
        restore(&home,&deleted.recovery_id).unwrap();
        assert_eq!(list(&home).unwrap().components.len(),1);
    }

    #[test]
    fn release_version_wins_over_preserved_timestamps_and_hash_order() {
        let temp = tempfile::tempdir().unwrap();
        let root = library_root(temp.path()).join("radio");
        for (hash, version) in [("ffffffffffffffff", "1.1.0"), ("0000000000000001", "1.2.0")] {
            let path = root.join(hash);
            write_package(&path, "radio");
            let manifest = fs::read_to_string(path.join("component.json")).unwrap();
            fs::write(path.join("component.json"), manifest.replace("1.0.0", version)).unwrap();
        }
        let selected = scan_latest_versions(temp.path()).unwrap();
        assert_eq!(selected[0].version, "1.2.0");
        let mut newer = entry_from_directory(&root.join("0000000000000001")).unwrap();
        let mut older = entry_from_directory(&root.join("ffffffffffffffff")).unwrap();
        newer.created_at_ms = 1;
        newer.mtime_ms = 1;
        older.created_at_ms = 99;
        older.mtime_ms = 99;
        assert!(compare_component_versions(&newer, &older).is_lt());
        for (a, b) in [("1.10.0", "1.9.0"), ("1.2.0", "1.2.0-rc.1"), ("1.2.0-rc.10", "1.2.0-rc.2")] {
            newer.version = a.into(); older.version = b.into();
            assert!(compare_component_versions(&newer, &older).is_lt(), "{a} > {b}");
        }
        newer.version = "1.2.0+new".into(); older.version = "1.2.0+old".into();
        assert!(compare_component_versions(&newer, &older).is_gt(), "equal release uses publication dates");
        newer.version = "legacy-new".into(); older.version = "legacy-old".into();
        assert!(compare_component_versions(&newer, &older).is_gt());
    }

    #[test]
    fn formal_directory_sprite_packages_validate_and_expose_preview_data() {
        let temp = tempfile::tempdir().unwrap();
        let home = temp.path().join("home");
        let source = temp.path().join("source");
        write_package(&source, "sprite-tool");
        fs::write(
            source.join("runtime/widget.json"),
            br#"{
              "schema_version":1,
              "engine":"p4-bounded-runtime-v4",
              "vars":{},
              "states":["idle"],
              "initial_state":"idle",
              "transitions":[],
              "tick":[],
              "dashboard":{"title":"Timer"},
              "scene":{
                "tick_ms":200,
                "active_state":"idle",
                "auto_start":true,
                "grid":{"width":4,"height":4},
                "sprites":[{"id":"hero","asset":"assets/hero.png","frame_width":8,"frame_height":8,"frames":2,"fps":8}],
                "entities":[{"id":"hero","x":0,"y":0,"width":1,"height":1,"sprite":"hero"}],
                "rules":[{"on":"tick","do":[{"op":"move","entity":"hero","dx":1,"dy":0}]}]
              }
            }"#,
        )
        .unwrap();
        let sprite = image::RgbaImage::from_pixel(16, 8, image::Rgba([255, 128, 32, 255]));
        let mut png = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgba8(sprite)
            .write_to(&mut png, image::ImageFormat::Png)
            .unwrap();
        fs::write(source.join("assets/hero.png"), png.into_inner()).unwrap();

        let published = publish_source(&home, &source).unwrap();
        assert!(published.valid, "{:?}", published.validation_errors);
        let snapshot = list(&home).unwrap();
        assert_eq!(snapshot.components.len(), 1);
        assert_eq!(snapshot.components[0].scene_sprites.len(), 1);
        assert_eq!(snapshot.components[0].scene_sprites[0].id, "hero");
        assert!(snapshot.components[0].scene_sprites[0]
            .data_url
            .starts_with("data:image/png;base64,"));
    }

    #[test]
    fn legacy_migration_copies_valid_packages_without_deleting_sources() {
        let temp = tempfile::tempdir().unwrap();
        let home = temp.path().join("home");
        let source = home
            .join(CLAW_PET_DIR_NAME)
            .join(LEGACY_DRAFTS_DIR_NAME)
            .join("old-job")
            .join("legacy-timer");
        write_package(&source, "legacy-timer");

        let snapshot = list(&home).unwrap();
        assert_eq!(snapshot.migration.migrated_count, 1);
        assert_eq!(snapshot.components[0].id, "legacy-timer");
        assert!(source.exists());
        assert!(components_root(&home)
            .join(LEGACY_MIGRATION_MARKER)
            .is_file());

        let second = list(&home).unwrap();
        assert_eq!(second.components.len(), 1);
        assert_eq!(second.migration.migrated_count, 1);
    }

    #[test]
    fn delete_rejects_paths_outside_the_formal_library() {
        let temp = tempfile::tempdir().unwrap();
        let home = temp.path().join("home");
        let outside = temp.path().join("outside");
        write_package(&outside, "outside");
        ensure_layout(&home).unwrap();

        let error = delete(
            &home,
            DeleteLibraryComponentInput {
                path: outside.display().to_string(),
            },
        )
        .unwrap_err();
        assert!(error.contains("正式组件库之外"));
        assert!(outside.exists());
    }

    #[test]
    fn publication_rejects_executable_or_script_payloads() {
        let temp = tempfile::tempdir().unwrap();
        let home = temp.path().join("home");
        let source = temp.path().join("source");
        write_package(&source, "unsafe-widget");
        fs::write(source.join("assets/behavior.js"), "alert('no')").unwrap();

        let error = publish_source(&home, &source).unwrap_err();
        assert!(error.contains("不允许可执行或脚本文件"));
        assert!(list(&home).unwrap().components.is_empty());
    }

    #[test]
    fn zip_import_rejects_oversize_duplicate_and_platform_paths() {
        let temp = tempfile::tempdir().unwrap();
        for (case, names, size) in [
            ("oversize", vec!["payload"], COMPONENT_IMPORT_MAX_BYTES as usize + 1),
            ("duplicate", vec!["payload", "payload"], 1),
            ("alternate-stream", vec!["payload:stream"], 1),
            ("backslash", vec![r"..\payload"], 1),
            ("traversal", vec!["../payload"], 1),
        ] {
            let input = temp.path().join(format!("{case}.zip"));
            let mut zip = zip::ZipWriter::new(fs::File::create(&input).unwrap());
            for name in names {
                zip.start_file(name, zip::write::FileOptions::default()
                    .compression_method(zip::CompressionMethod::Deflated)).unwrap();
                zip.write_all(&vec![0u8; size]).unwrap();
            }
            zip.finish().unwrap();
            assert!(extract_zip_safe(&input, &temp.path().join(case)).is_err(), "{case}");
        }
    }

    #[test]
    fn zip_import_accepts_normal_files_and_rejects_excess_entries() {
        let temp = tempfile::tempdir().unwrap();
        for count in [2, COMPONENT_IMPORT_MAX_FILES + 1] {
            let input = temp.path().join(format!("{count}.zip"));
            let mut zip = zip::ZipWriter::new(fs::File::create(&input).unwrap());
            for i in 0..count {
                zip.start_file(format!("assets/{i}.json"), zip::write::FileOptions::default()).unwrap();
                zip.write_all(b"{}").unwrap();
            }
            zip.finish().unwrap();
            let result = extract_zip_safe(&input, &temp.path().join(count.to_string()));
            assert_eq!(result.is_ok(), count <= COMPONENT_IMPORT_MAX_FILES);
        }
    }
}
