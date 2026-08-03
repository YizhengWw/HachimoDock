use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const BUNDLE_SECRETS_FILE: &str = "bundle-secrets.env";
const BUNDLE_SECRET_KEYS: [&str; 3] = [
    "PET_MANAGER_BUNDLED_MQTT_URL",
    "PET_MANAGER_BUNDLED_MQTT_USERNAME",
    "PET_MANAGER_BUNDLED_MQTT_PASSWORD",
];
const PET_MANAGER_PROTOCOL_SCHEMA: u32 = 4;

fn git_output(repo_root: &Path, args: &[&str]) -> Option<String> {
    let output = Command::new("git")
        .arg("-C")
        .arg(repo_root)
        .args(args)
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    String::from_utf8(output.stdout)
        .ok()
        .map(|value| value.trim().to_string())
}

fn emit_build_identity() {
    let manifest_dir =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap_or_else(|| ".".into()));
    let repo_root = manifest_dir
        .parent()
        .and_then(Path::parent)
        .unwrap_or(&manifest_dir);
    let git_sha = git_output(repo_root, &["rev-parse", "--short=12", "HEAD"])
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "unknown".to_string());
    let dirty = git_output(
        repo_root,
        &["status", "--porcelain", "--untracked-files=normal"],
    )
    .is_some_and(|value| !value.is_empty());
    let version = env::var("CARGO_PKG_VERSION").unwrap_or_else(|_| "unknown".to_string());
    let build_id = format!("{version}+{git_sha}{}", if dirty { "-dirty" } else { "" });

    println!(
        "cargo:rerun-if-changed={}",
        repo_root.join(".git/HEAD").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        repo_root.join(".git/index").display()
    );
    println!("cargo:rustc-env=PET_MANAGER_BUILD_GIT_SHA={git_sha}");
    println!(
        "cargo:rustc-env=PET_MANAGER_BUILD_DIRTY={}",
        u8::from(dirty)
    );
    println!("cargo:rustc-env=PET_MANAGER_BUILD_ID={build_id}");
    println!("cargo:rustc-env=PET_MANAGER_PROTOCOL_SCHEMA={PET_MANAGER_PROTOCOL_SCHEMA}");
}

fn parse_bundle_secrets(path: &Path) -> HashMap<String, String> {
    let Ok(content) = fs::read_to_string(path) else {
        return HashMap::new();
    };

    content
        .lines()
        .filter_map(|line| {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                return None;
            }
            let (key, value) = line.split_once('=')?;
            let key = key.trim();
            if key.is_empty() {
                return None;
            }
            let value = parse_env_value(value.trim());
            Some((key.to_string(), value))
        })
        .collect()
}

fn parse_env_value(value: &str) -> String {
    if value.len() >= 2 {
        let first = value.as_bytes()[0];
        let last = value.as_bytes()[value.len() - 1];
        if (first == b'"' && last == b'"') || (first == b'\'' && last == b'\'') {
            return unescape_env_quoted_value(&value[1..value.len() - 1]);
        }
    }
    value.to_string()
}

fn unescape_env_quoted_value(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    let mut chars = value.chars();
    while let Some(ch) = chars.next() {
        if ch != '\\' {
            out.push(ch);
            continue;
        }
        match chars.next() {
            Some('n') => out.push('\n'),
            Some('r') => out.push('\r'),
            Some('t') => out.push('\t'),
            Some('"') => out.push('"'),
            Some('\'') => out.push('\''),
            Some('\\') => out.push('\\'),
            Some(other) => {
                out.push('\\');
                out.push(other);
            }
            None => out.push('\\'),
        }
    }
    out
}

fn main() {
    println!("cargo:rerun-if-changed={BUNDLE_SECRETS_FILE}");
    emit_build_identity();

    let secrets = parse_bundle_secrets(Path::new(BUNDLE_SECRETS_FILE));
    for key in BUNDLE_SECRET_KEYS {
        let value = secrets
            .get(key)
            .cloned()
            .or_else(|| env::var(key).ok())
            .unwrap_or_default();
        if !value.trim().is_empty() {
            println!("cargo:rustc-env={key}={}", value.trim());
        }
    }

    tauri_build::build()
}
