/*
 * [Input] Cargo/Tauri environment and Git state; public builds never load bundle secrets.
 * [Output] Compile-time desktop build identity with the cross-end protocol schema.
 * [Pos] Tauri build-script boundary for release metadata and bundled resources.
 * [Sync] If the protocol schema changes, update the P4 CMake contract and tests.
 */

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

const PET_MANAGER_PROTOCOL_SCHEMA: u32 = 7;

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

fn main() {
    prepare_internal_ca();
    emit_build_identity();
    tauri_build::build()
}

fn prepare_internal_ca() {
    for name in ["PET_MANAGER_INTERNAL_CA_FILE", "PET_MANAGER_INTERNAL_CA_SHA256", "PET_MANAGER_INTERNAL_CREDENTIALS_FILE"] {
        println!("cargo:rerun-if-env-changed={name}");
    }
    let input = env::var_os("PET_MANAGER_INTERNAL_CA_FILE");
    let approved = env::var_os("PET_MANAGER_INTERNAL_CA_SHA256");
    let credentials = env::var_os("PET_MANAGER_INTERNAL_CREDENTIALS_FILE");
    if env::var_os("CARGO_FEATURE_INTERNAL_NETWORK").is_none() {
        assert!(input.is_none() && approved.is_none() && credentials.is_none(), "Public builds refuse internal CA or credential inputs");
        return;
    }
    let input = input.expect("Internal builds require an explicitly approved CA file");
    assert!(approved.is_some(), "Internal builds require approved CA fingerprints");
    println!("cargo:rerun-if-changed={}", PathBuf::from(input).display());
    let script = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap())
        .join("../scripts/prepare-internal-ca.mjs");
    println!("cargo:rerun-if-changed={}", script.display());
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("approved-internal-ca.pem");
    let status = Command::new("node").arg(script).arg(output).status()
        .expect("Node.js is required to validate the approved CA chain");
    assert!(status.success(), "Internal CA validation failed");
    let credentials = credentials.expect("Internal builds require an explicit credential file");
    println!("cargo:rerun-if-changed={}", PathBuf::from(credentials).display());
    let script = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap())
        .join("../scripts/prepare-internal-credentials.mjs");
    println!("cargo:rerun-if-changed={}", script.display());
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("internal-credentials.json");
    let status = Command::new("node").arg(script).arg(output).status()
        .expect("Node.js is required to validate internal credentials");
    assert!(status.success(), "Internal credential validation failed");
}
