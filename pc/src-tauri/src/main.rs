/*
 * [Input] Normal desktop launch, or a debug-only macOS Codex/Claude task ID/title/workspace probe.
 * [Output] Pet Manager Tauri runtime, or a bounded native Accessibility probe result.
 * [Pos] Native executable entry point for pc/src-tauri.
 * [Sync] If this file changes, update `pc/.folder.md`.
 */

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    #[cfg(all(target_os = "macos", debug_assertions))]
    {
        let mut args = std::env::args().skip(1);
        let command = args.next();
        if command.as_deref() == Some("--codex-accessibility-dump") {
            match pet_manager_tauri_lib::dump_codex_accessibility_tree() {
                Ok(lines) => {
                    for line in lines {
                        println!("{line}");
                    }
                    return;
                }
                Err(error) => {
                    eprintln!("Codex Accessibility dump failed: {error}");
                    std::process::exit(1);
                }
            }
        }
        if command.as_deref() == Some("--codex-accessibility-probe") {
            let session_id = args.next().unwrap_or_default();
            let title = args.next().unwrap_or_default();
            let cwd = args.next().unwrap_or_default();
            match pet_manager_tauri_lib::run_codex_accessibility_probe(&session_id, &title, &cwd) {
                Ok(()) => {
                    println!("Codex Accessibility probe succeeded");
                    return;
                }
                Err(error) => {
                    eprintln!("Codex Accessibility probe failed: {error}");
                    std::process::exit(1);
                }
            }
        }
        if command.as_deref() == Some("--claude-accessibility-probe") {
            let session_id = args.next().unwrap_or_default();
            let title = args.next().unwrap_or_default();
            let cwd = args.next().unwrap_or_default();
            match pet_manager_tauri_lib::run_claude_accessibility_probe(&session_id, &title, &cwd) {
                Ok(()) => {
                    println!("Claude Accessibility probe succeeded");
                    return;
                }
                Err(error) => {
                    eprintln!("Claude Accessibility probe failed: {error}");
                    std::process::exit(1);
                }
            }
        }
    }
    pet_manager_tauri_lib::run()
}
