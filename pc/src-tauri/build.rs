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
    prepare_reference_image_bridge();
    emit_build_identity();
    tauri_build::build()
}

fn prepare_reference_image_bridge() {
    println!("cargo:rerun-if-changed=src/reference_image_macos.m");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") { return; }
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let arch = if env::var("CARGO_CFG_TARGET_ARCH").as_deref() == Ok("aarch64") { "arm64" } else { "x86_64" };
    let object = out.join("reference_image.o");
    let status = Command::new("xcrun").args(["clang", "-fobjc-arc", "-fblocks", "-O2", "-arch", arch,
        "-mmacosx-version-min=10.15", "-Werror=implicit-function-declaration", "-c", "src/reference_image_macos.m", "-o"])
        .arg(&object).status().expect("Xcode command line tools are required for photo import");
    assert!(status.success(), "Photo import bridge failed to compile");
    let status = Command::new("xcrun").args(["ar", "crs"]).arg(out.join("libreference_image.a")).arg(object)
        .status().expect("Cannot archive photo import bridge");
    assert!(status.success());
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=reference_image");
    println!("cargo:rustc-link-arg=-Wl,-weak_framework,UniformTypeIdentifiers");
    for framework in ["AppKit", "PhotosUI", "ImageIO", "Foundation", "CoreGraphics"] {
        println!("cargo:rustc-link-lib=framework={framework}");
    }
}

