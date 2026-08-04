/*
 * [Input] Content-addressed petui publications plus ~/.claw-pet legacy draft roots.
 * [Output] Content-addressed formal local component library with one-time lossless
 *          legacy migration, atomic staging/publish, latest-version discovery,
 *          strict deletion guards, and preview metadata for Component Center.
 * [Pos] Local component-library ownership node in ref/src-tauri/src
 * [Sync] If this file changes, update ref/.folder.md.
 */

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const CLAW_PET_DIR_NAME: &str = ".claw-pet";
const LEGACY_OPENCLAW_DIR_NAME: &str = ".openclaw";
const LEGACY_DRAFTS_DIR_NAME: &str = "component-drafts";
const COMPONENTS_DIR_NAME: &str = "components";
const COMPONENT_STAGING_DIR_NAME: &str = ".staging";
const COMPONENT_LIBRARY_DIR_NAME: &str = "library";
const LEGACY_MIGRATION_MARKER: &str = ".legacy-component-migration-v1.json";
const FORBIDDEN_COMPONENT_SUFFIXES: &[&str] = &[
    "bat", "cmd", "com", "css", "dll", "dylib", "exe", "html", "js", "mjs", "cjs", "ps1", "py",
    "sh", "so", "svg",
];

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ComponentLibraryEntry {
    pub id: String,
    pub name: String,
    pub description: String,
    pub path: String,
    pub version_hash: String,
    pub mtime_ms: u64,
    pub created_at_ms: u64,
    pub dashboard: HashMap<String, String>,
    pub buttons: Vec<serde_json::Value>,
    pub game_type: Option<String>,
    pub runtime_engine: Option<String>,
    pub scene_engine: Option<String>,
    pub game_preset: Option<String>,
    pub scene: Option<serde_json::Value>,
    pub kind: Option<String>,
    pub valid: bool,
    pub validation_errors: Vec<String>,
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
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index).map_err(|error| error.to_string())?;
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
        let mut output_file = fs::File::create(&output).map_err(|error| error.to_string())?;
        std::io::copy(&mut entry, &mut output_file).map_err(|error| error.to_string())?;
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

fn package_content_hash(root: &Path) -> Result<String, String> {
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
        .map(|_| "p4-grid-scene-v1".to_string());
    let scene = runtime
        .as_ref()
        .and_then(|value| value.get("scene"))
        .cloned();
    let game_preset = game_type.clone();
    let buttons = if validation.ok {
        read_json_from_dir(path, "buttons.json")
            .and_then(|value| value.as_array().cloned())
            .unwrap_or_default()
    } else {
        Vec::new()
    };
    let version_hash = path
        .file_name()
        .and_then(|value| value.to_str())
        .filter(|value| value.len() == 16 && value.chars().all(|ch| ch.is_ascii_hexdigit()))
        .map(str::to_string)
        .unwrap_or(package_content_hash(path)?);
    Ok(ComponentLibraryEntry {
        id,
        name,
        description,
        path: path.display().to_string(),
        version_hash,
        mtime_ms: metadata_timestamp_ms(metadata.modified().ok()),
        created_at_ms: metadata_created_at_ms(&metadata),
        dashboard,
        buttons,
        game_type,
        runtime_engine,
        scene_engine,
        game_preset,
        scene,
        kind,
        valid: validation.ok,
        validation_errors: validation.errors,
    })
}

fn publish_source(home: &Path, source: &Path) -> Result<ComponentLibraryEntry, String> {
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
                match entry_from_directory(&path) {
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
        versions.sort_by(|left, right| {
            right
                .created_at_ms
                .cmp(&left.created_at_ms)
                .then_with(|| right.mtime_ms.cmp(&left.mtime_ms))
                .then_with(|| right.version_hash.cmp(&left.version_hash))
        });
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
    fs::remove_dir_all(component_root).map_err(|error| {
        format!(
            "删除正式本地组件失败 {}: {}",
            component_root.display(),
            error
        )
    })?;
    Ok(DeleteLibraryComponentResult {
        ok: true,
        deleted_component_id: component_id,
        deleted_path: component_root.display().to_string(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

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
}
