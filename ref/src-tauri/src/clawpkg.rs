use serde::Serialize;
use std::collections::HashMap;
use std::io::Read;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ClawpkgManifestPreview {
    pub id: String,
    pub name: String,
    pub version: String,
    pub dashboard: HashMap<String, String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ValidateClawpkgResult {
    pub ok: bool,
    pub manifest: Option<ClawpkgManifestPreview>,
    pub errors: Vec<String>,
}

const REQUIRED_FILES: &[&str] = &[
    "component.json",
    "negative-screen.json",
    "buttons.json",
    "runtime/widget.json",
    "share.json",
];
const REQUIRED_DIRS: &[&str] = &["runtime/", "assets/"];

/// (slot_id, max_utf8_bytes). MUST mirror ref/src/lib/clawpkg-contract.js.
pub const COMPONENT_DASHBOARD_V1_SLOTS: &[(&str, usize)] = &[
    ("title", 60),
    ("eyebrow", 90),
    ("headline", 156),
    ("metricLabel", 90),
    ("metricValue", 60),
    ("metricUnit", 30),
    ("badge", 12),
    ("note", 156),
    ("footer", 156),
    /* progress: serialized as "<0-100>:<label>" by validator (negative-screen.json
    carries it as {value, label} object that gets flattened on read). */
    ("progress", 64),
];

pub fn validate_clawpkg_bytes(bytes: &[u8]) -> Result<ValidateClawpkgResult, String> {
    let mut files: HashMap<String, Vec<u8>> = HashMap::new();
    let mut dir_seen: HashMap<String, bool> = HashMap::new();

    let mut archive = zip::ZipArchive::new(std::io::Cursor::new(bytes))
        .map_err(|e| format!("not a valid zip: {}", e))?;

    for i in 0..archive.len() {
        let mut entry = archive.by_index(i).map_err(|e| e.to_string())?;
        let name = entry.name().to_string();
        for d in REQUIRED_DIRS {
            if name.starts_with(d) {
                dir_seen.insert((*d).to_string(), true);
            }
        }
        if entry.is_file() {
            let mut buf = Vec::new();
            entry.read_to_end(&mut buf).map_err(|e| e.to_string())?;
            files.insert(name, buf);
        }
    }

    validate_clawpkg_collected(files, |d| dir_seen.get(d).copied().unwrap_or(false))
}

/// Build the manifest preview from the collected file map. Mutates `errors` with
/// any structural problems (missing fields, unknown slots, byte overflow). Returns
/// `Some(preview)` only when nothing was pushed to `errors` during this call.
fn build_manifest_preview(
    files: &HashMap<String, Vec<u8>>,
    errors: &mut Vec<String>,
) -> Option<ClawpkgManifestPreview> {
    let meta_bytes = files.get("component.json")?;
    let v: serde_json::Value = match serde_json::from_slice(meta_bytes) {
        Ok(v) => v,
        Err(e) => {
            errors.push(format!("component.json 解析失败: {}", e));
            return None;
        }
    };
    let id = v.get("id").and_then(|x| x.as_str()).unwrap_or("");
    let name = v.get("name").and_then(|x| x.as_str()).unwrap_or("");
    let version = v.get("version").and_then(|x| x.as_str()).unwrap_or("");
    let had_errors_before = errors.len();
    if id.is_empty() || name.is_empty() || version.is_empty() {
        errors.push("component.json 必须含 id、name、version".to_string());
    }
    /* Read dashboard. Most slots are flat strings; `progress` may be an object
    {value, label} which we flatten to "<value>:<label>" so it serializes as
    a normal slot for the device parser. */
    let mut dashboard_map: HashMap<String, String> = HashMap::new();
    if let Some(dash_val) = files
        .get("negative-screen.json")
        .and_then(|d| serde_json::from_slice::<serde_json::Value>(d).ok())
        .and_then(|v| v.get("dashboard").cloned())
    {
        if let Some(obj) = dash_val.as_object() {
            for (k, v) in obj {
                if k == "progress" && v.is_object() {
                    let value = v
                        .get("value")
                        .and_then(|x| x.as_i64().or_else(|| x.as_f64().map(|f| f as i64)))
                        .unwrap_or(0);
                    let label = v.get("label").and_then(|x| x.as_str()).unwrap_or("");
                    dashboard_map.insert("progress".to_string(), format!("{}:{}", value, label));
                } else if let Some(s) = v.as_str() {
                    dashboard_map.insert(k.clone(), s.to_string());
                }
            }
        }
    }
    for (slot, value) in &dashboard_map {
        let known = COMPONENT_DASHBOARD_V1_SLOTS.iter().find(|(k, _)| k == slot);
        match known {
            None => errors.push(format!("negative-screen.json 含未知槽位 {}", slot)),
            Some((_, max_bytes)) => {
                if value.as_bytes().len() > *max_bytes {
                    errors.push(format!("槽位 {} 超出 {} 字节上限", slot, max_bytes));
                }
            }
        }
    }
    if errors.len() == had_errors_before {
        Some(ClawpkgManifestPreview {
            id: id.to_string(),
            name: name.to_string(),
            version: version.to_string(),
            dashboard: dashboard_map,
        })
    } else {
        None
    }
}

pub fn validate_clawpkg_at_path(path: &std::path::Path) -> Result<ValidateClawpkgResult, String> {
    /* Skill-generated drafts land as directories (the agent's working copy);
    distributed clawpkgs land as .zip / .clawpkg files. Dispatch on metadata
    so both shapes feed the same validator. */
    let meta = std::fs::metadata(path).map_err(|e| format!("stat {}: {}", path.display(), e))?;
    if meta.is_dir() {
        let mut files: HashMap<String, Vec<u8>> = HashMap::new();
        for f in REQUIRED_FILES {
            let fp = path.join(f);
            if fp.exists() {
                let bytes =
                    std::fs::read(&fp).map_err(|e| format!("read {}: {}", fp.display(), e))?;
                files.insert((*f).to_string(), bytes);
            }
        }
        validate_clawpkg_collected(files, |dir| path.join(dir).is_dir())
    } else {
        let bytes = std::fs::read(path).map_err(|e| format!("read {}: {}", path.display(), e))?;
        validate_clawpkg_bytes(&bytes)
    }
}

/// Shared core: validate a manifest after files are collected (from zip or fs walk).
/// `dir_exists` reports whether REQUIRED_DIRS are present in the source.
fn validate_clawpkg_collected(
    files: HashMap<String, Vec<u8>>,
    dir_exists: impl Fn(&str) -> bool,
) -> Result<ValidateClawpkgResult, String> {
    let mut errors: Vec<String> = Vec::new();
    for f in REQUIRED_FILES {
        if !files.contains_key(*f) {
            errors.push(format!("缺少 {}", f));
        }
    }
    for d in REQUIRED_DIRS {
        if !dir_exists(d) {
            errors.push(format!("缺少 {}", d));
        }
    }
    let preview = build_manifest_preview(&files, &mut errors);
    Ok(ValidateClawpkgResult {
        ok: errors.is_empty(),
        manifest: preview,
        errors,
    })
}

/// Render slot map to COMPONENT_DASHBOARD_V1 text payload understood by device.
/// Format: first line = magic, subsequent lines = "key=value" for non-empty values
/// in canonical slot order. Empty slots are omitted.
pub fn render_component_dashboard_payload(dashboard: &HashMap<String, String>) -> String {
    let mut out = String::from("COMPONENT_DASHBOARD_V1\n");
    for (slot, _max_bytes) in COMPONENT_DASHBOARD_V1_SLOTS {
        if let Some(value) = dashboard.get(*slot) {
            if !value.is_empty() {
                out.push_str(slot);
                out.push('=');
                out.push_str(value);
                out.push('\n');
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn make_zip(files: &[(&str, &[u8])]) -> Vec<u8> {
        let mut buf = Vec::new();
        {
            let mut zw = zip::ZipWriter::new(std::io::Cursor::new(&mut buf));
            for (name, data) in files {
                zw.start_file(*name, zip::write::FileOptions::default())
                    .unwrap();
                zw.write_all(data).unwrap();
            }
            zw.finish().unwrap();
        }
        buf
    }

    fn valid_files() -> Vec<(&'static str, Vec<u8>)> {
        vec![
            (
                "component.json",
                br#"{"id":"x","name":"X","version":"1.0.0"}"#.to_vec(),
            ),
            (
                "negative-screen.json",
                b"{\"dashboard\":{\"title\":\"X\",\"headline\":\"\xe4\xbd\xa0\xe5\xa5\xbd\"}}"
                    .to_vec(),
            ),
            ("buttons.json", b"[]".to_vec()),
            ("runtime/widget.json", br#"{"schema_version":1}"#.to_vec()),
            ("assets/.keep", b"".to_vec()),
            ("share.json", br#"{"title":"X"}"#.to_vec()),
        ]
    }

    #[test]
    fn validates_complete_clawpkg() {
        let files_owned = valid_files();
        let files: Vec<(&str, &[u8])> = files_owned
            .iter()
            .map(|(n, d)| (*n, d.as_slice()))
            .collect();
        let zip_bytes = make_zip(&files);
        let result = validate_clawpkg_bytes(&zip_bytes).expect("validate should run");
        assert!(result.ok, "expected valid; errors={:?}", result.errors);
        assert_eq!(result.manifest.as_ref().unwrap().id, "x");
    }

    #[test]
    fn rejects_missing_component_json() {
        let files: Vec<(&str, &[u8])> = vec![
            ("negative-screen.json", br#"{"dashboard":{}}"#.as_slice()),
            ("buttons.json", b"[]".as_slice()),
            ("runtime/widget.json", br#"{"schema_version":1}"#.as_slice()),
        ];
        let zip_bytes = make_zip(&files);
        let result = validate_clawpkg_bytes(&zip_bytes).unwrap();
        assert!(!result.ok);
        assert!(result.errors.iter().any(|e| e.contains("component.json")));
    }

    #[test]
    fn rejects_missing_runtime_widget_json() {
        let mut files_owned = valid_files();
        files_owned.retain(|(name, _)| *name != "runtime/widget.json");
        files_owned.push(("runtime/.keep", b"".to_vec()));
        let files: Vec<(&str, &[u8])> = files_owned
            .iter()
            .map(|(n, d)| (*n, d.as_slice()))
            .collect();
        let zip_bytes = make_zip(&files);
        let result = validate_clawpkg_bytes(&zip_bytes).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|e| e.contains("runtime/widget.json")));
    }

    #[test]
    fn renders_component_dashboard_v1_payload() {
        let mut dashboard = HashMap::new();
        dashboard.insert("title".to_string(), "摸鱼倒计时".to_string());
        dashboard.insert("eyebrow".to_string(), "距离今天下班".to_string());
        dashboard.insert("headline".to_string(), "还有 2 小时 13 分".to_string());
        dashboard.insert("metricLabel".to_string(), "下班时间".to_string());
        dashboard.insert("metricValue".to_string(), "18:00".to_string());
        dashboard.insert("badge".to_string(), "5".to_string());
        dashboard.insert("note".to_string(), "本周已坚持 5 天".to_string());
        dashboard.insert("footer".to_string(), "红钮 切显示".to_string());
        let payload = render_component_dashboard_payload(&dashboard);
        let lines: Vec<&str> = payload.lines().collect();
        assert_eq!(lines[0], "COMPONENT_DASHBOARD_V1");
        assert!(lines.iter().any(|l| *l == "title=摸鱼倒计时"));
        assert!(lines.iter().any(|l| *l == "headline=还有 2 小时 13 分"));
        assert!(lines.iter().any(|l| *l == "footer=红钮 切显示"));
        // metricUnit not present in input - must be omitted from output
        assert!(!lines.iter().any(|l| l.starts_with("metricUnit=")));
    }

    #[test]
    fn payload_omits_empty_slots() {
        let dashboard = HashMap::new();
        let payload = render_component_dashboard_payload(&dashboard);
        assert_eq!(payload, "COMPONENT_DASHBOARD_V1\n");
    }

    #[test]
    fn validate_at_path_handles_directory_drafts() {
        /* Skill-generated drafts arrive as directories (no zip). The validator
        must walk the 6-file contract from disk and produce the same manifest
        preview as the zip path. */
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path().join("meeting-timer");
        std::fs::create_dir_all(dir.join("runtime")).unwrap();
        std::fs::create_dir_all(dir.join("assets")).unwrap();
        std::fs::write(
            dir.join("component.json"),
            r#"{"id":"meeting-timer","name":"会议计时","version":"1.0.0"}"#.as_bytes(),
        )
        .unwrap();
        std::fs::write(
            dir.join("negative-screen.json"),
            r#"{"dashboard":{"title":"会议","headline":"还有 12 分"}}"#.as_bytes(),
        )
        .unwrap();
        std::fs::write(dir.join("buttons.json"), b"[]").unwrap();
        std::fs::write(dir.join("share.json"), r#"{"title":"会议计时"}"#.as_bytes()).unwrap();
        std::fs::write(dir.join("runtime/widget.json"), br#"{"schema_version":1}"#).unwrap();
        std::fs::write(dir.join("assets/.keep"), b"").unwrap();

        let result = validate_clawpkg_at_path(&dir).expect("validate should run");
        assert!(result.ok, "expected valid; errors={:?}", result.errors);
        let manifest = result.manifest.expect("manifest should be built");
        assert_eq!(manifest.id, "meeting-timer");
        assert_eq!(manifest.name, "会议计时");
        assert_eq!(
            manifest.dashboard.get("title").map(String::as_str),
            Some("会议")
        );
    }

    #[test]
    fn validate_at_path_directory_missing_required_file_errors() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path().join("incomplete");
        std::fs::create_dir_all(dir.join("runtime")).unwrap();
        std::fs::create_dir_all(dir.join("assets")).unwrap();
        std::fs::write(
            dir.join("component.json"),
            br#"{"id":"x","name":"X","version":"1.0.0"}"#,
        )
        .unwrap();
        // missing negative-screen.json, buttons.json, share.json

        let result = validate_clawpkg_at_path(&dir).unwrap();
        assert!(!result.ok);
        assert!(result
            .errors
            .iter()
            .any(|e| e.contains("negative-screen.json")));
        assert!(result.errors.iter().any(|e| e.contains("buttons.json")));
        assert!(result.errors.iter().any(|e| e.contains("share.json")));
    }

    #[test]
    fn rejects_slot_over_max_bytes() {
        let big_badge: String = "A".repeat(20); /* badge maxBytes 12 */
        let neg_screen = format!(r#"{{"dashboard":{{"badge":"{}"}}}}"#, big_badge);
        let files: Vec<(&str, &[u8])> = vec![
            (
                "component.json",
                br#"{"id":"x","name":"X","version":"1.0.0"}"#.as_slice(),
            ),
            ("negative-screen.json", neg_screen.as_bytes()),
            ("buttons.json", b"[]".as_slice()),
            ("runtime/widget.json", br#"{"schema_version":1}"#.as_slice()),
            ("assets/.keep", b"".as_slice()),
            ("share.json", br#"{"title":"X"}"#.as_slice()),
        ];
        let zip_bytes = make_zip(&files);
        let result = validate_clawpkg_bytes(&zip_bytes).unwrap();
        assert!(!result.ok);
        assert!(result.errors.iter().any(|e| e.contains("badge")));
    }
}
