//! Codex pet importer and controlled community installer (M13.1/M13.2).
//!
//! Reads a Codex pet's v1 `spritesheet.webp` (8 cols × 9 rows × 192×208) or
//! v2 sheet (8 cols × 11 rows × 192×208), cuts the shared first nine animation
//! rows, writes a trimmed transparent gallery preview, and produces
//! per-family MP4 files by writing one PNG per unique atlas cell and driving
//! hidden FFmpeg concat processes.
//! The list API also reports each pet's latest source-file modified time so
//! the community-import UI can detect both newly installed and updated pets;
//! pasted community commands are converted into a constrained `npx codex-pets`
//! install rather than executing arbitrary shell text. Import resolution accepts
//! pet.json ids and relative `spritesheetPath` values instead of assuming the
//! directory name always equals the public pet id.
//! ffmpeg discovery prefers the target-native executable bundled under the
//! install-relative `tools/` resource directory, then delegates to the desktop
//! host PATH resolver and current-user Windows package-manager roots.
//! Writes a manifest.json that matches the shape produced by
//! `ref/src/lib/appearance-store.js` so the gallery/detail UI can render it
//! without any special-casing beyond `type === "codex-import"`.

use std::collections::HashSet;
use std::fs;
use std::io::Cursor;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::OnceLock;
use std::time::UNIX_EPOCH;

use base64::Engine as _;
use image::{GenericImageView, ImageFormat, RgbImage, RgbaImage};
use serde::{Deserialize, Serialize};

const CELL_W: u32 = 192;
const CELL_H: u32 = 208;
/// Device players and fb-display expect CFR. Quantize atlas hold durations at
/// this rate while concat metadata reuses each unique PNG instead of duplicating
/// it for every output tick so short holds still map to multiple CFR ticks.
const OUTPUT_FPS: u32 = 24;
/// Multiplier applied to atlas hold-times before quantizing onto OUTPUT_FPS ticks.
/// 1.0 = source-faithful; >1.0 slows playback. Source atlases (e.g. running rows
/// at 120 ms/frame) felt rushed on-device, so default to 2.0× for a calmer pace.
const PLAYBACK_SLOWDOWN: f64 = 2.0;
const ATLAS_COLS: u32 = 8;
const V1_ATLAS_ROWS: u32 = 9;
const V2_ATLAS_ROWS: u32 = 11;
const EXPECTED_W: u32 = CELL_W * ATLAS_COLS; // 1536
const V1_EXPECTED_H: u32 = CELL_H * V1_ATLAS_ROWS; // 1872
const V2_EXPECTED_H: u32 = CELL_H * V2_ATLAS_ROWS; // 2288

/// Solid background used when flattening alpha before H.264 encoding.
/// Black matches the dark UI surface and device LCD background.
const BG_R: u8 = 0x00;
const BG_G: u8 = 0x00;
const BG_B: u8 = 0x00;

/// Pixels below this alpha are dropped before compositing — removes the
/// semi-transparent fringe that otherwise bleeds into the solid background
/// during yuv420p flattening.
const ALPHA_THRESHOLD: u8 = 96;
const PREVIEW_PADDING: u32 = 4;

/// Magenta/red halo detection radius used by `clean_halo_pixels`. A pixel
/// matching the halo profile is dropped if any neighbour within this radius
/// is already transparent.
const HALO_RADIUS: i32 = 2;

/// Codex row → (pet-manager family, used columns count, per-frame durations in ms).
/// v1 and v2 share these first nine animation rows. v2 rows 9 and 10 contain
/// 16 directional look frames, which Pet Manager's appearance-family contract
/// does not consume and therefore intentionally leaves untouched.
/// Source: ~/.codex/skills/hatch-pet/references/animation-rows.md + mapping image.
struct RowMap {
    row: u32,
    family: &'static str,
    cols: u32,
    durations_ms: &'static [u32],
}

const MAPPING: &[RowMap] = &[
    // row 0: idle → idle.default
    RowMap {
        row: 0,
        family: "idle.default",
        cols: 6,
        durations_ms: &[280, 110, 110, 140, 140, 320],
    },
    // row 1: running-right → touch.right
    RowMap {
        row: 1,
        family: "touch.right",
        cols: 8,
        durations_ms: &[120, 120, 120, 120, 120, 120, 120, 220],
    },
    // row 2: running-left → touch.left
    RowMap {
        row: 2,
        family: "touch.left",
        cols: 8,
        durations_ms: &[120, 120, 120, 120, 120, 120, 120, 220],
    },
    // row 3: waving → welcome
    RowMap {
        row: 3,
        family: "welcome",
        cols: 4,
        durations_ms: &[140, 140, 140, 280],
    },
    // row 4: jumping → idle.jumping
    RowMap {
        row: 4,
        family: "idle.jumping",
        cols: 5,
        durations_ms: &[140, 140, 140, 140, 280],
    },
    // row 5: failed → error
    RowMap {
        row: 5,
        family: "error",
        cols: 8,
        durations_ms: &[140, 140, 140, 140, 140, 140, 140, 240],
    },
    // row 6: waiting → waiting_user
    RowMap {
        row: 6,
        family: "waiting_user",
        cols: 6,
        durations_ms: &[150, 150, 150, 150, 150, 260],
    },
    // row 7: running → working
    RowMap {
        row: 7,
        family: "working",
        cols: 6,
        durations_ms: &[120, 120, 120, 120, 120, 220],
    },
    // row 8: review → done
    RowMap {
        row: 8,
        family: "done",
        cols: 6,
        durations_ms: &[150, 150, 150, 150, 150, 280],
    },
];

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CodexPetSummary {
    pub id: String,
    pub display_name: String,
    pub description: String,
    pub spritesheet_path: String,
    pub preview_data_url: String,
    pub modified_at: u64,
}

#[derive(Debug, Clone, Deserialize)]
struct CodexPetJson {
    id: Option<String>,
    #[serde(rename = "displayName")]
    display_name: Option<String>,
    description: Option<String>,
    #[serde(rename = "spritesheetPath")]
    spritesheet_path: Option<String>,
    #[serde(rename = "spriteVersionNumber")]
    sprite_version_number: Option<u32>,
}

fn resolve_codex_atlas_version(
    pet_json: &CodexPetJson,
    width: u32,
    height: u32,
) -> Result<u32, String> {
    if width != EXPECTED_W {
        return Err(format!(
            "spritesheet 宽度 {} 不符合预期 {}（每行 {} 格）",
            width, EXPECTED_W, ATLAS_COLS
        ));
    }

    let size_version = match height {
        V1_EXPECTED_H => 1,
        V2_EXPECTED_H => 2,
        _ => {
            return Err(format!(
                "spritesheet 高度 {} 不符合 Codex v1/v2 规格（v1: {}，v2: {}）",
                height, V1_EXPECTED_H, V2_EXPECTED_H
            ));
        }
    };

    match pet_json.sprite_version_number {
        None => Ok(size_version),
        Some(version @ 1..=2) if version == size_version => Ok(size_version),
        Some(version @ 1..=2) => Err(format!(
            "pet.json 声明 spriteVersionNumber={}，但 spritesheet 尺寸 {}×{} 对应 v{}",
            version, width, height, size_version
        )),
        Some(version) => Err(format!(
            "暂不支持 spriteVersionNumber={}；当前支持 Codex v1 和 v2",
            version
        )),
    }
}

fn codex_pets_root() -> Result<PathBuf, String> {
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .ok_or_else(|| "无法解析当前用户目录".to_string())?;
    Ok(PathBuf::from(home).join(".codex").join("pets"))
}

fn file_modified_at_ms(path: &Path) -> u64 {
    fs::metadata(path)
        .and_then(|meta| meta.modified())
        .ok()
        .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
        .map(|duration| duration.as_millis().min(u128::from(u64::MAX)) as u64)
        .unwrap_or(0)
}

fn pet_modified_at_ms(pet_dir: &Path, pet_json: &Path, spritesheet: &Path) -> u64 {
    [
        file_modified_at_ms(pet_dir),
        file_modified_at_ms(pet_json),
        file_modified_at_ms(spritesheet),
    ]
    .into_iter()
    .max()
    .unwrap_or(0)
}

fn resolve_spritesheet_path(pet_dir: &Path, pet_json: &CodexPetJson) -> Result<PathBuf, String> {
    if let Some(raw) = pet_json.spritesheet_path.as_deref() {
        let trimmed = raw.trim();
        if !trimmed.is_empty() {
            let candidate = PathBuf::from(trimmed);
            let resolved = if candidate.is_absolute() {
                candidate
            } else {
                pet_dir.join(candidate)
            };
            if resolved.is_file() {
                return Ok(resolved);
            }
        }
    }

    let fallback = pet_dir.join("spritesheet.webp");
    if fallback.is_file() {
        return Ok(fallback);
    }

    Err(format!(
        "找不到 codex 宠物目录 {} 下的 spritesheet.webp",
        pet_dir.display()
    ))
}

fn codex_pet_preview_data_url(spritesheet: &Path) -> Option<String> {
    let sheet = image::open(spritesheet).ok()?;
    let frame = codex_source_preview_from_sheet(&sheet)?;
    let mut encoded = Cursor::new(Vec::new());
    image::DynamicImage::ImageRgba8(frame)
        .write_to(&mut encoded, ImageFormat::Png)
        .ok()?;
    Some(format!(
        "data:image/png;base64,{}",
        base64::engine::general_purpose::STANDARD.encode(encoded.into_inner())
    ))
}

fn codex_source_preview_from_sheet(sheet: &image::DynamicImage) -> Option<RgbaImage> {
    let (width, height) = sheet.dimensions();
    if width < CELL_W || height < CELL_H {
        return None;
    }

    let mut frame = sheet.crop_imm(0, 0, CELL_W, CELL_H).to_rgba8();
    clean_frame_edges(&mut frame);
    Some(trim_alpha_bounds(&frame, PREVIEW_PADDING))
}

fn read_pet_json(path: &Path) -> Result<CodexPetJson, String> {
    let text = fs::read_to_string(path).map_err(|e| format!("读取 pet.json 失败: {}", e))?;
    serde_json::from_str(&text).map_err(|e| format!("解析 pet.json 失败: {}", e))
}

fn resolve_pet_dir_by_requested_id(root: &Path, pet_id: &str) -> Result<PathBuf, String> {
    let direct = root.join(pet_id);
    if direct.is_dir() {
        return Ok(direct);
    }

    if !root.exists() {
        return Err(format!("找不到 codex pets 目录 {}", root.display()));
    }

    for entry in fs::read_dir(root).map_err(|e| format!("读取 {} 失败: {}", root.display(), e))?
    {
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let dir_name = path.file_name().and_then(|s| s.to_str()).unwrap_or("");
        if dir_name == pet_id {
            return Ok(path);
        }
        let pet_json_path = path.join("pet.json");
        if !pet_json_path.is_file() {
            continue;
        }
        let parsed = match read_pet_json(&pet_json_path) {
            Ok(v) => v,
            Err(_) => continue,
        };
        if parsed.id.as_deref() == Some(pet_id) {
            return Ok(path);
        }
    }

    Err(format!("找不到 codex 宠物 `{}` 的目录", pet_id))
}

pub fn list_codex_pets() -> Result<Vec<CodexPetSummary>, String> {
    let root = codex_pets_root()?;
    if !root.exists() {
        return Ok(Vec::new());
    }
    let mut out = Vec::new();
    for entry in fs::read_dir(&root).map_err(|e| format!("读取 {} 失败: {}", root.display(), e))?
    {
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let pet_json = path.join("pet.json");
        if !pet_json.is_file() {
            continue;
        }
        let parsed = match read_pet_json(&pet_json) {
            Ok(v) => v,
            Err(_) => continue,
        };
        let spritesheet = match resolve_spritesheet_path(&path, &parsed) {
            Ok(v) => v,
            Err(_) => continue,
        };
        let dir_name = path
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("")
            .to_string();
        out.push(CodexPetSummary {
            id: parsed.id.unwrap_or_else(|| dir_name.clone()),
            display_name: parsed.display_name.unwrap_or(dir_name),
            description: parsed.description.unwrap_or_default(),
            preview_data_url: codex_pet_preview_data_url(&spritesheet).unwrap_or_default(),
            spritesheet_path: spritesheet.to_string_lossy().to_string(),
            modified_at: pet_modified_at_ms(&path, &pet_json, &spritesheet),
        });
    }
    out.sort_by(|a, b| {
        b.modified_at
            .cmp(&a.modified_at)
            .then_with(|| a.id.cmp(&b.id))
    });
    Ok(out)
}

fn can_run_ffmpeg(candidate: &str) -> bool {
    crate::command_for_host(candidate)
        .arg("-version")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn bundled_ffmpeg_candidates() -> Vec<PathBuf> {
    let mut out = Vec::new();

    if let Some(configured) = std::env::var_os("PET_MANAGER_BUNDLED_FFMPEG") {
        out.push(PathBuf::from(configured));
    }

    let Ok(executable) = std::env::current_exe() else {
        return out;
    };

    #[cfg(target_os = "macos")]
    if let Some(contents_dir) = executable.parent().and_then(Path::parent) {
        out.push(contents_dir.join("Resources").join("tools").join("ffmpeg"));
    }

    #[cfg(windows)]
    if let Some(install_dir) = executable.parent() {
        out.push(install_dir.join("tools").join("ffmpeg.exe"));
    }

    #[cfg(all(unix, not(target_os = "macos")))]
    if let Some(binary_dir) = executable.parent() {
        out.push(binary_dir.join("tools").join("ffmpeg"));
    }

    out
}

fn ffmpeg_candidates() -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();

    let mut push_unique = |candidate: String| {
        if candidate.trim().is_empty() {
            return;
        }
        let key = if cfg!(windows) {
            candidate.to_ascii_lowercase()
        } else {
            candidate.clone()
        };
        if seen.insert(key) {
            out.push(candidate);
        }
    };

    for candidate in bundled_ffmpeg_candidates() {
        if candidate.is_file() {
            push_unique(candidate.to_string_lossy().to_string());
        }
    }

    if let Some(candidate) = crate::find_executable("ffmpeg", &[]) {
        push_unique(candidate);
    }

    #[cfg(windows)]
    {
        push_unique("ffmpeg.exe".to_string());

        if let Some(path_var) = std::env::var_os("PATH") {
            for dir in std::env::split_paths(&path_var) {
                for name in ["ffmpeg.exe", "ffmpeg.cmd", "ffmpeg.bat", "ffmpeg"] {
                    let candidate = dir.join(name);
                    if candidate.is_file() {
                        push_unique(candidate.to_string_lossy().to_string());
                    }
                }
            }
        }

        if let Some(user_profile) = std::env::var_os("USERPROFILE") {
            let scoop = PathBuf::from(user_profile)
                .join("scoop")
                .join("apps")
                .join("ffmpeg")
                .join("current")
                .join("bin")
                .join("ffmpeg.exe");
            if scoop.is_file() {
                push_unique(scoop.to_string_lossy().to_string());
            }
        }

        for env_key in ["ProgramFiles", "ProgramFiles(x86)"] {
            if let Some(program_files) = std::env::var_os(env_key) {
                let base = PathBuf::from(program_files);
                for candidate in [
                    base.join("ffmpeg").join("bin").join("ffmpeg.exe"),
                    base.join("GyanFFmpeg").join("bin").join("ffmpeg.exe"),
                ] {
                    if candidate.is_file() {
                        push_unique(candidate.to_string_lossy().to_string());
                    }
                }
            }
        }

        if let Some(program_data) = std::env::var_os("ProgramData") {
            let base = PathBuf::from(program_data);
            for candidate in [
                base.join("chocolatey").join("bin").join("ffmpeg.exe"),
                base.join("chocolatey")
                    .join("lib")
                    .join("ffmpeg")
                    .join("tools")
                    .join("ffmpeg")
                    .join("bin")
                    .join("ffmpeg.exe"),
            ] {
                if candidate.is_file() {
                    push_unique(candidate.to_string_lossy().to_string());
                }
            }
        }

        if let Some(local_app_data) = std::env::var_os("LOCALAPPDATA") {
            let winget_packages = PathBuf::from(local_app_data)
                .join("Microsoft")
                .join("WinGet")
                .join("Packages");
            if let Ok(packages) = fs::read_dir(winget_packages) {
                for package in packages.flatten() {
                    let package_name = package.file_name().to_string_lossy().to_ascii_lowercase();
                    if !package_name.contains("ffmpeg") {
                        continue;
                    }
                    let package_path = package.path();
                    let direct_bin = package_path.join("bin").join("ffmpeg.exe");
                    if direct_bin.is_file() {
                        push_unique(direct_bin.to_string_lossy().to_string());
                    }
                    if let Ok(sub_dirs) = fs::read_dir(package_path) {
                        for sub in sub_dirs.flatten() {
                            let candidate = sub.path().join("bin").join("ffmpeg.exe");
                            if candidate.is_file() {
                                push_unique(candidate.to_string_lossy().to_string());
                            }
                        }
                    }
                }
            }
        }
    }

    out
}

pub fn check_ffmpeg_available() -> bool {
    resolve_ffmpeg().is_ok()
}

pub(crate) fn resolve_ffmpeg() -> Result<String, String> {
    static RESOLVED_FFMPEG: OnceLock<String> = OnceLock::new();
    if let Some(candidate) = RESOLVED_FFMPEG.get() {
        return Ok(candidate.clone());
    }
    for candidate in ffmpeg_candidates() {
        if can_run_ffmpeg(&candidate) {
            let _ = RESOLVED_FFMPEG.set(candidate.clone());
            return Ok(candidate);
        }
    }
    Err("未检测到 ffmpeg".to_string())
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CodexCommunityInstallResult {
    pub pet_id: String,
    pub command: String,
    pub stdout: String,
    pub stderr: String,
}

fn validate_community_pet_id(pet_id: &str) -> Result<String, String> {
    let trimmed = pet_id.trim();
    let mut chars = trimmed.chars();
    let Some(first) = chars.next() else {
        return Err("缺少社区形象 ID".to_string());
    };
    if trimmed.len() > 128
        || !first.is_ascii_alphanumeric()
        || !chars.all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '_' || c == '-')
    {
        return Err("社区形象 ID 只能包含字母、数字、点、下划线和短横线".to_string());
    }
    Ok(trimmed.to_string())
}

fn npx_candidates() -> Vec<&'static str> {
    #[cfg(windows)]
    {
        vec!["npx.cmd", "npx.exe", "npx"]
    }
    #[cfg(not(windows))]
    {
        vec!["npx"]
    }
}

pub fn install_codex_community_pet(pet_id: &str) -> Result<CodexCommunityInstallResult, String> {
    let pet_id = validate_community_pet_id(pet_id)?;
    let mut not_found = true;
    let mut last_error = String::new();

    for npx in npx_candidates() {
        let output = crate::command_for_host(npx)
            .args(["--yes", "codex-pets", "add", &pet_id])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .output();

        match output {
            Ok(output) => {
                not_found = false;
                let stdout = String::from_utf8_lossy(&output.stdout).trim().to_string();
                let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
                if output.status.success() {
                    return Ok(CodexCommunityInstallResult {
                        pet_id: pet_id.clone(),
                        command: format!("npx --yes codex-pets add {}", pet_id),
                        stdout,
                        stderr,
                    });
                }
                let detail = if stderr.is_empty() {
                    stdout.clone()
                } else {
                    stderr.clone()
                };
                last_error = format!(
                    "执行 npx codex-pets add {} 失败 (exit {:?}){}{}",
                    pet_id,
                    output.status.code(),
                    if detail.is_empty() { "" } else { ": " },
                    detail
                );
            }
            Err(err) => {
                last_error = err.to_string();
            }
        }
    }

    if not_found {
        Err("未找到 npx，请先安装 Node.js/npm 后重试。".to_string())
    } else {
        Err(last_error)
    }
}

/// Port of the Python reference's `is_magenta_halo_pixel`: detects the
/// magenta/red fringe that Codex sprite-sheet anti-aliasing leaves around
/// transparent edges. These pixels look fine against alpha but scream once
/// flattened onto white.
#[inline]
fn is_halo_rgb(r: u8, g: u8, b: u8) -> bool {
    let r = r as i32;
    let g = g as i32;
    let b = b as i32;
    r >= 110 && b >= 90 && (r - g) >= 15 && (b - g) >= 5 && (r + b) >= 260
}

/// Drop low-alpha fringe pixels and transparent-adjacent magenta halo pixels
/// *in place* on an RGBA cell. Matches the reference Python implementation
/// with `alpha_threshold=96` and `edge_cleanup=magenta, radius=2`.
fn clean_frame_edges(frame: &mut RgbaImage) {
    let (w, h) = (frame.width() as i32, frame.height() as i32);

    // Snapshot the alpha channel *before* mutation so the halo scan doesn't
    // expand into pixels this same pass just zeroed out (which would eat into
    // the sprite body rather than only its fringe).
    let original_alpha: Vec<u8> = frame.pixels().map(|p| p.0[3]).collect();
    let idx = |x: i32, y: i32| (y as usize) * (w as usize) + (x as usize);

    for y in 0..h {
        for x in 0..w {
            let px = frame.get_pixel_mut(x as u32, y as u32);
            let [r, g, b, a] = px.0;
            if a == 0 {
                continue;
            }
            if a < ALPHA_THRESHOLD {
                px.0[3] = 0;
                continue;
            }
            if !is_halo_rgb(r, g, b) {
                continue;
            }

            let mut near_transparent = false;
            'scan: for yy in (y - HALO_RADIUS).max(0)..=(y + HALO_RADIUS).min(h - 1) {
                for xx in (x - HALO_RADIUS).max(0)..=(x + HALO_RADIUS).min(w - 1) {
                    if original_alpha[idx(xx, yy)] == 0 {
                        near_transparent = true;
                        break 'scan;
                    }
                }
            }
            if near_transparent {
                px.0[3] = 0;
            }
        }
    }
}

fn trim_alpha_bounds(frame: &RgbaImage, padding: u32) -> RgbaImage {
    let (w, h) = frame.dimensions();
    let mut min_x = w;
    let mut min_y = h;
    let mut max_x = 0;
    let mut max_y = 0;
    let mut found = false;

    for (x, y, px) in frame.enumerate_pixels() {
        if px.0[3] == 0 {
            continue;
        }
        found = true;
        min_x = min_x.min(x);
        min_y = min_y.min(y);
        max_x = max_x.max(x);
        max_y = max_y.max(y);
    }

    if !found {
        return frame.clone();
    }

    let x = min_x.saturating_sub(padding);
    let y = min_y.saturating_sub(padding);
    let right = (max_x + padding).min(w - 1);
    let bottom = (max_y + padding).min(h - 1);
    image::imageops::crop_imm(frame, x, y, right - x + 1, bottom - y + 1).to_image()
}

#[inline]
fn duration_to_frame_count(duration_ms: u32) -> u32 {
    // Apply PLAYBACK_SLOWDOWN before quantizing. Use ceil so the result never
    // plays faster than the (slowed) target — round() let 140 ms collapse onto
    // 3 frames (125 ms) and made sequences feel rushed.
    let target_ms = (duration_ms as f64) * PLAYBACK_SLOWDOWN;
    let count = (target_ms * f64::from(OUTPUT_FPS) / 1000.0).ceil() as u32;
    count.max(1)
}

/// Alpha-composite an RGBA frame onto the solid `BG_*` backdrop and return
/// an RGB image ready for PNG → ffmpeg. We do the blend ourselves rather than
/// letting ffmpeg handle it so the fringe cleanup above is preserved exactly
/// and yuv420p encoding sees a flat opaque source.
fn flatten_to_background(frame: &RgbaImage) -> RgbImage {
    let (w, h) = (frame.width(), frame.height());
    let mut out = RgbImage::from_pixel(w, h, image::Rgb([BG_R, BG_G, BG_B]));
    for (x, y, px) in frame.enumerate_pixels() {
        let [r, g, b, a] = px.0;
        if a == 0 {
            continue;
        }
        if a == 255 {
            out.put_pixel(x, y, image::Rgb([r, g, b]));
            continue;
        }
        let af = a as u32;
        let inv = 255 - af;
        let blend =
            |src: u8, dst: u8| -> u8 { ((src as u32 * af + dst as u32 * inv + 127) / 255) as u8 };
        out.put_pixel(
            x,
            y,
            image::Rgb([blend(r, BG_R), blend(g, BG_G), blend(b, BG_B)]),
        );
    }
    out
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CodexImportResult {
    pub appearance_id: String,
    pub appearance_dir: String,
}

pub fn import_codex_pet(
    pet_id: &str,
    app_local_data_dir: &Path,
) -> Result<CodexImportResult, String> {
    let ffmpeg = resolve_ffmpeg()?;
    let requested_pet_id = validate_community_pet_id(pet_id)?;

    let root = codex_pets_root()?;
    let pet_dir = resolve_pet_dir_by_requested_id(&root, &requested_pet_id)?;
    let pet_json_path = pet_dir.join("pet.json");
    if !pet_json_path.is_file() {
        return Err(format!(
            "找不到 codex 宠物 `{}` 的 pet.json",
            requested_pet_id
        ));
    }

    let pet_json = read_pet_json(&pet_json_path)?;
    let spritesheet_path = resolve_spritesheet_path(&pet_dir, &pet_json)?;
    let display_name = pet_json
        .display_name
        .clone()
        .unwrap_or_else(|| requested_pet_id.to_string());
    let description = pet_json.description.clone().unwrap_or_default();

    let img =
        image::open(&spritesheet_path).map_err(|e| format!("解码 spritesheet.webp 失败: {}", e))?;
    let (w, h) = img.dimensions();
    resolve_codex_atlas_version(&pet_json, w, h)?;
    let rgba: RgbaImage = img.to_rgba8();
    let source_preview = codex_source_preview_from_sheet(&img)
        .ok_or_else(|| "提取 Codex 源预览图失败".to_string())?;

    // Appearance directory layout mirrors saveAppearance() in appearance-store.js.
    let appearance_id = format!("codex-{}", requested_pet_id);
    let appearance_rel = format!("custom-appearances/{}", appearance_id);
    let appearance_abs = app_local_data_dir.join(&appearance_rel);
    let videos_abs = appearance_abs.join("videos");
    fs::create_dir_all(&videos_abs).map_err(|e| format!("创建 appearance 目录失败: {}", e))?;

    // Copy the spritesheet as source (simple; the detail/retry flow doesn't need
    // a single-cell crop for codex imports).
    let source_rel = format!("{}/source.webp", appearance_rel);
    fs::copy(&spritesheet_path, app_local_data_dir.join(&source_rel))
        .map_err(|e| format!("拷贝 source 图失败: {}", e))?;
    let source_preview_rel = format!("{}/preview.png", appearance_rel);
    source_preview
        .save(app_local_data_dir.join(&source_preview_rel))
        .map_err(|e| format!("写入 source preview 图失败: {}", e))?;

    // Temp workspace for extracted frames + concat lists.
    let temp_dir = std::env::temp_dir().join(format!("codex-import-{}", uuid::Uuid::new_v4()));
    fs::create_dir_all(&temp_dir).map_err(|e| format!("创建临时目录失败: {}", e))?;

    let mut families_out: Vec<serde_json::Value> = Vec::new();

    for m in MAPPING {
        if m.cols == 0 || m.durations_ms.is_empty() {
            continue;
        }
        if m.cols as usize != m.durations_ms.len() {
            return Err(format!(
                "mapping 错误: row {} cols {} ≠ durations {}",
                m.row,
                m.cols,
                m.durations_ms.len()
            ));
        }

        let family_tmp = temp_dir.join(m.family);
        fs::create_dir_all(&family_tmp).map_err(|e| format!("创建帧目录失败: {}", e))?;

        // Write per-column PNGs.
        //
        // Alpha in the codex atlas is *not* clean: edges carry a low-opacity
        // fringe and a red/magenta halo from the original anti-aliasing. If we
        // hand those RGBA frames to ffmpeg and let it flatten via yuv420p, the
        // fringe turns into a muddy colored rectangle around the sprite once
        // played back. Mirror the reference Python pipeline: drop the fringe,
        // scrub halo pixels adjacent to transparency, then composite onto the
        // UI surface color and save RGB PNGs.
        let mut frame_paths: Vec<(PathBuf, u32)> = Vec::new();
        for col in 0..m.cols {
            let x = col * CELL_W;
            let y = m.row * CELL_H;
            let mut cell = image::imageops::crop_imm(&rgba, x, y, CELL_W, CELL_H).to_image();
            clean_frame_edges(&mut cell);
            let flat = flatten_to_background(&cell);
            let hold_frames = duration_to_frame_count(m.durations_ms[col as usize]);
            let frame_path = family_tmp.join(format!("frame_{:02}.png", col));
            flat.save(&frame_path)
                .map_err(|e| format!("写入 frame PNG 失败: {}", e))?;
            frame_paths.push((frame_path, hold_frames));
        }

        // Keep one PNG per unique atlas cell and express its quantized hold time
        // through concat metadata. ffmpeg expands the VFR input onto the CFR
        // output timeline, avoiding hundreds of identical PNG encodes per pet.
        let mut concat_lines = String::from("ffconcat version 1.0\n");
        for (frame_path, hold_frames) in &frame_paths {
            let escaped = frame_path.to_string_lossy().replace('\'', "'\\''");
            concat_lines.push_str(&format!("file '{}'\n", escaped));
            concat_lines.push_str(&format!(
                "duration {:.6}\n",
                f64::from(*hold_frames) / f64::from(OUTPUT_FPS)
            ));
        }
        // concat demuxer requires the last file to be repeated without a
        // duration so the final frame's timestamp is computed correctly.
        if let Some((last, _)) = frame_paths.last() {
            let escaped = last.to_string_lossy().replace('\'', "'\\''");
            concat_lines.push_str(&format!("file '{}'\n", escaped));
        }
        let concat_list_path = family_tmp.join("concat.txt");
        fs::write(&concat_list_path, concat_lines)
            .map_err(|e| format!("写入 concat list 失败: {}", e))?;

        let out_path = videos_abs.join(format!("{}.mp4", m.family));
        let fps_arg = OUTPUT_FPS.to_string();
        let status = crate::command_for_host(&ffmpeg)
            .args(["-y", "-f", "concat", "-safe", "0", "-i"])
            .arg(&concat_list_path)
            .args([
                "-vf",
                "format=yuv420p",
                "-r",
                &fps_arg,
                "-c:v",
                "libx264",
                "-preset",
                "veryfast",
                "-crf",
                "22",
                // All-intra: short sprite clips with B-frames decode poorly on Pi
                // and can corrupt the upper portion of each frame.
                "-g",
                "1",
                "-keyint_min",
                "1",
                "-sc_threshold",
                "0",
                "-bf",
                "0",
                "-movflags",
                "+faststart",
            ])
            .arg(&out_path)
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .output()
            .map_err(|e| format!("启动 ffmpeg 失败: {}", e))?;
        if !status.status.success() {
            let err_text = String::from_utf8_lossy(&status.stderr);
            return Err(format!(
                "ffmpeg 转码 family {} 失败: {}",
                m.family,
                err_text.trim()
            ));
        }

        let video_rel = format!("{}/videos/{}.mp4", appearance_rel, m.family);
        families_out.push(serde_json::json!({
            "family": m.family,
            "ok": true,
            "prompt": format!("codex row {}", m.row),
            "videoPath": video_rel,
            "taskId": format!("codex-row-{}", m.row),
            "videoUrl": "",
        }));
    }

    let manifest = serde_json::json!({
        "schema_version": 1,
        "id": appearance_id,
        "type": "codex-import",
        "name": display_name,
        "description": description,
        "provider": "codex",
        "model": "",
        "base_url": "",
        "thinking_model": "",
        "persona": { "source": "codex-import", "pet_id": requested_pet_id },
        "source_image": source_rel,
        "source_mime": "image/webp",
        "source_preview": source_preview_rel,
        "source_preview_mime": "image/png",
        "families": families_out,
        "created_at": chrono_like_iso_now(),
    });
    let manifest_path = appearance_abs.join("manifest.json");
    fs::write(
        &manifest_path,
        serde_json::to_vec_pretty(&manifest).unwrap(),
    )
    .map_err(|e| format!("写入 manifest.json 失败: {}", e))?;

    let _ = fs::remove_dir_all(&temp_dir);

    Ok(CodexImportResult {
        appearance_id,
        appearance_dir: appearance_abs.to_string_lossy().to_string(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn duration_to_frame_count_maps_sprite_holds_to_cfr_ticks() {
        // With PLAYBACK_SLOWDOWN=2.0 and ceil quantization at 24 fps:
        // 280 ms × 2 × 24 / 1000 = 13.44 → 14
        assert_eq!(duration_to_frame_count(280), 14);
        // 110 ms × 2 × 24 / 1000 = 5.28 → 6
        assert_eq!(duration_to_frame_count(110), 6);
        // 140 ms × 2 × 24 / 1000 = 6.72 → 7
        assert_eq!(duration_to_frame_count(140), 7);
        // 50 ms × 2 × 24 / 1000 = 2.4 → 3
        assert_eq!(duration_to_frame_count(50), 3);
    }

    #[test]
    fn codex_import_maps_running_row_to_single_working_family() {
        assert!(MAPPING.iter().any(|row| row.family == "working"));
        assert!(!MAPPING.iter().any(|row| row.family == "working.thinking"));
        assert!(!MAPPING.iter().any(|row| row.family == "working.default"));
    }

    #[test]
    fn validate_community_pet_id_accepts_slug_ids() {
        assert_eq!(validate_community_pet_id("sakura-jk").unwrap(), "sakura-jk");
    }

    #[test]
    fn validate_community_pet_id_rejects_shell_text() {
        assert!(validate_community_pet_id("sakura-jk;rm").is_err());
    }

    #[test]
    fn resolve_pet_dir_matches_pet_json_id_when_directory_name_differs() {
        let root =
            std::env::temp_dir().join(format!("codex-pet-dir-test-{}", uuid::Uuid::new_v4()));
        let pet_dir = root.join("pretty-folder-name");
        fs::create_dir_all(&pet_dir).unwrap();
        fs::write(
            pet_dir.join("pet.json"),
            r#"{"id":"sakura-jk","displayName":"Sakura JK"}"#,
        )
        .unwrap();
        fs::File::create(pet_dir.join("spritesheet.webp"))
            .unwrap()
            .write_all(b"webp")
            .unwrap();

        let resolved = resolve_pet_dir_by_requested_id(&root, "sakura-jk").unwrap();

        assert_eq!(resolved, pet_dir);
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn resolve_spritesheet_path_honors_pet_json_relative_path() {
        let pet_dir = std::env::temp_dir().join(format!(
            "codex-pet-spritesheet-test-{}",
            uuid::Uuid::new_v4()
        ));
        fs::create_dir_all(pet_dir.join("assets")).unwrap();
        let expected = pet_dir.join("assets").join("sheet.webp");
        fs::write(&expected, b"webp").unwrap();
        let pet_json = CodexPetJson {
            id: Some("sakura-jk".to_string()),
            display_name: None,
            description: None,
            spritesheet_path: Some("assets/sheet.webp".to_string()),
            sprite_version_number: None,
        };

        let resolved = resolve_spritesheet_path(&pet_dir, &pet_json).unwrap();

        assert_eq!(resolved, expected);
        let _ = fs::remove_dir_all(&pet_dir);
    }

    #[test]
    fn codex_atlas_accepts_v1_v2_and_legacy_inference() {
        let legacy = CodexPetJson {
            id: Some("legacy".to_string()),
            display_name: None,
            description: None,
            spritesheet_path: None,
            sprite_version_number: None,
        };
        let v1 = CodexPetJson {
            sprite_version_number: Some(1),
            ..legacy.clone()
        };
        let v2 = CodexPetJson {
            sprite_version_number: Some(2),
            ..legacy.clone()
        };

        assert_eq!(
            resolve_codex_atlas_version(&legacy, EXPECTED_W, V1_EXPECTED_H),
            Ok(1)
        );
        assert_eq!(
            resolve_codex_atlas_version(&legacy, EXPECTED_W, V2_EXPECTED_H),
            Ok(2)
        );
        assert_eq!(
            resolve_codex_atlas_version(&v1, EXPECTED_W, V1_EXPECTED_H),
            Ok(1)
        );
        assert_eq!(
            resolve_codex_atlas_version(&v2, EXPECTED_W, V2_EXPECTED_H),
            Ok(2)
        );
    }

    #[test]
    fn codex_atlas_rejects_version_size_mismatches_and_unknown_layouts() {
        let v2 = CodexPetJson {
            id: Some("v2".to_string()),
            display_name: None,
            description: None,
            spritesheet_path: None,
            sprite_version_number: Some(2),
        };
        let future = CodexPetJson {
            sprite_version_number: Some(3),
            ..v2.clone()
        };

        assert!(resolve_codex_atlas_version(&v2, EXPECTED_W, V1_EXPECTED_H)
            .unwrap_err()
            .contains("对应 v1"));
        assert!(
            resolve_codex_atlas_version(&future, EXPECTED_W, V2_EXPECTED_H)
                .unwrap_err()
                .contains("当前支持 Codex v1 和 v2")
        );
        assert!(
            resolve_codex_atlas_version(&v2, EXPECTED_W + 1, V2_EXPECTED_H)
                .unwrap_err()
                .contains("宽度")
        );
        assert!(
            resolve_codex_atlas_version(&v2, EXPECTED_W, V2_EXPECTED_H + CELL_H)
                .unwrap_err()
                .contains("v1/v2 规格")
        );
    }

    #[test]
    #[ignore = "requires CODEX_PET_BENCH_ID and a locally installed Codex pet"]
    fn codex_pet_import_prepares_the_complete_p4_pack_benchmark() {
        let pet_id = std::env::var("CODEX_PET_BENCH_ID")
            .expect("CODEX_PET_BENCH_ID must name an installed Codex pet");
        let app_data =
            std::env::temp_dir().join(format!("codex-pet-import-bench-{}", uuid::Uuid::new_v4()));
        fs::create_dir_all(&app_data).unwrap();
        let started = std::time::Instant::now();

        let imported = import_codex_pet(&pet_id, &app_data).unwrap();
        let imported_at = std::time::Instant::now();
        let prepared = crate::usb_serial::prepare_p4_appearance(
            Path::new(&imported.appearance_dir),
            &app_data,
        )
        .unwrap();
        let finished_at = std::time::Instant::now();

        eprintln!(
            "codex_import_benchmark pet={} import_ms={} p4_prepare_ms={} elapsed_ms={} p4_files={} p4_bytes={}",
            pet_id,
            imported_at.duration_since(started).as_millis(),
            finished_at.duration_since(imported_at).as_millis(),
            finished_at.duration_since(started).as_millis(),
            prepared.file_count,
            prepared.byte_count
        );
        assert!(prepared.file_count > 1);
        assert!(prepared.byte_count > 0);
        let _ = fs::remove_dir_all(&app_data);
    }

    #[test]
    fn codex_pet_preview_data_url_extracts_trimmed_first_atlas_cell() {
        let temp_dir =
            std::env::temp_dir().join(format!("codex-pet-preview-test-{}", uuid::Uuid::new_v4()));
        fs::create_dir_all(&temp_dir).unwrap();
        let sheet_path = temp_dir.join("spritesheet.png");
        let mut sheet = RgbaImage::from_pixel(CELL_W, CELL_H, image::Rgba([0, 0, 0, 0]));
        sheet.put_pixel(10, 12, image::Rgba([255, 32, 16, 255]));
        let mut encoded = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgba8(sheet)
            .write_to(&mut encoded, image::ImageFormat::Png)
            .unwrap();
        fs::write(&sheet_path, encoded.into_inner()).unwrap();

        let data_url = codex_pet_preview_data_url(&sheet_path).unwrap();
        let payload = data_url
            .strip_prefix("data:image/png;base64,")
            .expect("preview should be a PNG data URL");
        let decoded = base64::engine::general_purpose::STANDARD
            .decode(payload)
            .unwrap();
        let preview = image::load_from_memory(&decoded).unwrap().to_rgba8();

        assert_eq!(
            preview.dimensions(),
            (PREVIEW_PADDING * 2 + 1, PREVIEW_PADDING * 2 + 1)
        );
        assert_eq!(
            preview.get_pixel(PREVIEW_PADDING, PREVIEW_PADDING).0,
            [255, 32, 16, 255]
        );
        assert_eq!(preview.get_pixel(0, 0).0, [0, 0, 0, 0]);
        let _ = fs::remove_dir_all(&temp_dir);
    }
}

/// ISO-8601 timestamp without pulling in the `chrono` crate — matches the
/// format produced by JavaScript's `new Date().toISOString()` well enough for
/// `listAppearances` sort-by-created_at.
fn chrono_like_iso_now() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let secs = now.as_secs() as i64;
    let millis = now.subsec_millis();
    // Convert unix epoch seconds to civil date (Howard Hinnant's algorithm).
    let days = secs.div_euclid(86_400);
    let time_of_day = secs.rem_euclid(86_400);
    let (y, m, d) = civil_from_days(days);
    let hh = time_of_day / 3600;
    let mm = (time_of_day % 3600) / 60;
    let ss = time_of_day % 60;
    format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        y, m, d, hh, mm, ss, millis
    )
}

fn civil_from_days(days: i64) -> (i32, u32, u32) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let y = if m <= 2 { y + 1 } else { y };
    (y as i32, m, d)
}
