# Phase C: `.clawpkg` 校验 + USB OTA 安装

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development.

**Goal:** 客户端可以选一个 `.clawpkg` zip（或 `~/.openclaw/component-drafts/<ts>/` 目录），校验内容、把 `negative-screen.json` 的 dashboard 转成 `COMPONENT_DASHBOARD_V1` 文本 payload、经 USB 推到设备并切到 stats 屏。三件套（摸鱼/番茄/喝水）作为 `.clawpkg` zip 内置在仓里，按钮一键安装。

**Architecture:** 三层。
1. **Rust 层**（claw-pet-manager/ref/src-tauri）：`validate_clawpkg(path)` 解 zip + 校验 schema；`install_clawpkg_over_usb(path)` 串联校验 + payload 构造 + USB 推送（复用既有 `usb_send_command` 写文件 + screen-page 切换）。
2. **设备 C 层**（board-runtime）：`board_serial_bridge.c` 增加 `payload_write {path, content}` 消息类型，白名单写到几个已知路径（`.stats-display`, `.current-speech`）；`board_server.c` / overlay 不动（payload 抵达后既有读取机制接管）。
3. **数据层**：三件套 `.clawpkg.zip` 内置仓库 `ref/builtin-clawpkgs/{slack-off-countdown,tomato-clock,drink-reminder}.clawpkg`，由 build/install 脚本一次性生成（用 mock-data 的 dashboard 值）。

**Tech Stack:** Rust（zip crate）、cargo test、node:test（JS 端集成测试）、C11（设备端 JSON-line 协议扩展）。

**仓库：** Phase C 跨同一仓库内的 `ref/` 与 `board-runtime/` 两个目录；在单一 feature 分支内同时提交客户端与设备端改动。

**依赖：** A + B 已落地。本 Phase C 不在合并前要求 A、B 必须先合到 main；但 demo 真机验证需要 A 的 UI + B 的渲染 + C 的传输都装上去。

---

## Task C1: Rust 端 `.clawpkg` 校验（zip 解压 + schema 校验）

**Files:**
- Create: `claw-pet-manager/ref/src-tauri/src/clawpkg.rs` (new module)
- Modify: `claw-pet-manager/ref/src-tauri/src/lib.rs` (`mod clawpkg;`)
- Modify: `claw-pet-manager/ref/src-tauri/Cargo.toml` (add `zip = "0.6"` dependency)

- [ ] **Step 1: Add `zip` crate** — `cd ref/src-tauri && cargo add zip` (or edit Cargo.toml directly). Build to confirm: `cargo build` (no other change yet).

- [ ] **Step 2: 写 failing test in `src/clawpkg.rs`**

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn make_zip(files: &[(&str, &[u8])]) -> Vec<u8> {
        let mut buf = Vec::new();
        {
            let mut zw = zip::ZipWriter::new(std::io::Cursor::new(&mut buf));
            for (name, data) in files {
                zw.start_file(*name, zip::write::FileOptions::default()).unwrap();
                zw.write_all(data).unwrap();
            }
            zw.finish().unwrap();
        }
        buf
    }

    fn valid_files() -> Vec<(&'static str, Vec<u8>)> {
        vec![
            ("component.json", br#"{"id":"x","name":"X","version":"1.0.0"}"#.to_vec()),
            ("negative-screen.json", br#"{"dashboard":{"title":"X","headline":"你好"}}"#.to_vec()),
            ("buttons.json", b"[]".to_vec()),
            ("runtime/.keep", b"".to_vec()),
            ("assets/.keep", b"".to_vec()),
            ("share.json", br#"{"title":"X"}"#.to_vec()),
        ]
    }

    #[test]
    fn validates_complete_clawpkg() {
        let files: Vec<(&str, &[u8])> = valid_files().iter().map(|(n, d)| (*n, d.as_slice())).collect();
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
        ];
        let zip_bytes = make_zip(&files);
        let result = validate_clawpkg_bytes(&zip_bytes).unwrap();
        assert!(!result.ok);
        assert!(result.errors.iter().any(|e| e.contains("component.json")));
    }

    #[test]
    fn rejects_slot_over_max_bytes() {
        let big_badge: String = "A".repeat(20); // badge maxBytes 12
        let neg_screen = format!(r#"{{"dashboard":{{"badge":"{}"}}}}"#, big_badge);
        let files: Vec<(&str, &[u8])> = vec![
            ("component.json", br#"{"id":"x","name":"X","version":"1.0.0"}"#.as_slice()),
            ("negative-screen.json", neg_screen.as_bytes()),
            ("buttons.json", b"[]".as_slice()),
            ("runtime/.keep", b"".as_slice()),
            ("assets/.keep", b"".as_slice()),
            ("share.json", br#"{"title":"X"}"#.as_slice()),
        ];
        let zip_bytes = make_zip(&files);
        let result = validate_clawpkg_bytes(&zip_bytes).unwrap();
        assert!(!result.ok);
        assert!(result.errors.iter().any(|e| e.contains("badge")));
    }
}
```

- [ ] **Step 3: 运行测试确认 FAIL** — `cd ref/src-tauri && cargo test --lib clawpkg 2>&1 | tail -15` → expect compile errors (validate_clawpkg_bytes / ValidateResult / ClawpkgManifestPreview undefined).

- [ ] **Step 4: 实现 `src/clawpkg.rs`**

```rust
use serde::{Deserialize, Serialize};
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
    "share.json",
];
const REQUIRED_DIRS: &[&str] = &["runtime/", "assets/"];

/// (slot_id, max_utf8_bytes). MUST mirror ref/src/lib/clawpkg-contract.js.
const COMPONENT_DASHBOARD_V1_SLOTS: &[(&str, usize)] = &[
    ("title", 60),
    ("eyebrow", 90),
    ("headline", 156),
    ("metricLabel", 90),
    ("metricValue", 60),
    ("metricUnit", 30),
    ("badge", 12),
    ("note", 156),
    ("footer", 156),
];

pub fn validate_clawpkg_bytes(bytes: &[u8]) -> Result<ValidateClawpkgResult, String> {
    let mut errors: Vec<String> = Vec::new();
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

    for f in REQUIRED_FILES {
        if !files.contains_key(*f) {
            errors.push(format!("缺少 {}", f));
        }
    }
    for d in REQUIRED_DIRS {
        if !dir_seen.get(*d).copied().unwrap_or(false) {
            errors.push(format!("缺少 {}", d));
        }
    }

    let mut preview: Option<ClawpkgManifestPreview> = None;

    if let Some(meta_bytes) = files.get("component.json") {
        match serde_json::from_slice::<serde_json::Value>(meta_bytes) {
            Ok(v) => {
                let id = v.get("id").and_then(|x| x.as_str()).unwrap_or("");
                let name = v.get("name").and_then(|x| x.as_str()).unwrap_or("");
                let version = v.get("version").and_then(|x| x.as_str()).unwrap_or("");
                if id.is_empty() || name.is_empty() || version.is_empty() {
                    errors.push("component.json 必须含 id、name、version".to_string());
                }
                let dashboard_map: HashMap<String, String> = files
                    .get("negative-screen.json")
                    .and_then(|d| serde_json::from_slice::<serde_json::Value>(d).ok())
                    .and_then(|v| v.get("dashboard").cloned())
                    .and_then(|d| serde_json::from_value(d).ok())
                    .unwrap_or_default();
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
                if errors.is_empty() {
                    preview = Some(ClawpkgManifestPreview {
                        id: id.to_string(),
                        name: name.to_string(),
                        version: version.to_string(),
                        dashboard: dashboard_map,
                    });
                }
            }
            Err(e) => errors.push(format!("component.json 解析失败: {}", e)),
        }
    }

    Ok(ValidateClawpkgResult {
        ok: errors.is_empty(),
        manifest: preview,
        errors,
    })
}

pub fn validate_clawpkg_at_path(path: &std::path::Path) -> Result<ValidateClawpkgResult, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("read {}: {}", path.display(), e))?;
    validate_clawpkg_bytes(&bytes)
}
```

- [ ] **Step 5: Add `mod clawpkg;` to lib.rs near other module declarations.** Verify build: `cargo build 2>&1 | tail -8`.

- [ ] **Step 6: Run tests** — `cargo test --lib clawpkg 2>&1 | tail -10` → 3 tests PASS.

- [ ] **Step 7: 提交** — `git add ref/src-tauri/Cargo.toml ref/src-tauri/Cargo.lock ref/src-tauri/src/clawpkg.rs ref/src-tauri/src/lib.rs && git commit -m "feat(clawpkg): Rust 端 .clawpkg zip 校验模块"`

---

## Task C2: Rust `dashboard → COMPONENT_DASHBOARD_V1` payload converter

**Files:**
- Modify: `claw-pet-manager/ref/src-tauri/src/clawpkg.rs`

Adds a pure function that takes the dashboard slot HashMap and produces the exact `COMPONENT_DASHBOARD_V1` text format the device expects.

- [ ] **Step 1: 写 failing test in `src/clawpkg.rs`**

```rust
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
```

- [ ] **Step 2: Confirm test FAILS** — `cargo test --lib clawpkg 2>&1 | tail -8`.

- [ ] **Step 3: Implement** — add to `clawpkg.rs`:

```rust
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
```

- [ ] **Step 4: Test passes** — `cargo test --lib clawpkg 2>&1 | tail -8` → 5 tests PASS.

- [ ] **Step 5: 提交** — `git commit -m "feat(clawpkg): COMPONENT_DASHBOARD_V1 payload renderer"`

---

## Task C3: Tauri `install_clawpkg_over_usb` 命令

**Files:**
- Modify: `claw-pet-manager/ref/src-tauri/src/lib.rs` (新增命令 + 注册 + helper if needed)

- [ ] **Step 1: Recon `usb_send_command` shape** — `grep -n 'fn usb_send_command\|"usb_send_command"' ref/src-tauri/src/lib.rs | head -10`. Read enough to understand its input. The implementer needs to figure out how to send a file-write request to the device using existing primitives. Two probable patterns:
  - (a) existing `usb_send_command(command_type, payload)` accepts an "asset file write" channel — reuse it
  - (b) need a new device command — defer that to C4

- [ ] **Step 2: Implement `install_clawpkg_over_usb` Tauri command**

```rust
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct InstallClawpkgInput {
    clawpkg_path: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct InstallClawpkgResult {
    ok: bool,
    manifest: Option<crate::clawpkg::ClawpkgManifestPreview>,
    errors: Vec<String>,
    transferred_bytes: usize,
}

#[tauri::command]
async fn install_clawpkg_over_usb(
    usb_manager: tauri::State<'_, UsbManagerState>,  // <- adjust to actual existing state type
    input: InstallClawpkgInput,
) -> Result<InstallClawpkgResult, String> {
    let path = std::path::PathBuf::from(&input.clawpkg_path);
    let validation = crate::clawpkg::validate_clawpkg_at_path(&path)?;
    if !validation.ok {
        return Ok(InstallClawpkgResult {
            ok: false,
            manifest: validation.manifest,
            errors: validation.errors,
            transferred_bytes: 0,
        });
    }
    let manifest = validation.manifest.clone().unwrap();
    let payload = crate::clawpkg::render_component_dashboard_payload(&manifest.dashboard);

    // Send the payload via existing USB primitive. Pseudo-code; adapt to the
    // real signature you find in Step 1:
    let sent = usb_manager_send_payload_write(&usb_manager, ".stats-display", &payload)
        .map_err(|e| e.to_string())?;

    // Switch device to stats screen so the new payload is shown.
    let _ = usb_manager_send_command(&usb_manager, "control/screen-page", "stats");

    Ok(InstallClawpkgResult {
        ok: true,
        manifest: validation.manifest,
        errors: vec![],
        transferred_bytes: sent,
    })
}
```

If existing `UsbManagerState` / `usb_manager_send_payload_write` / `usb_manager_send_command` don't exist with exactly these names, locate the equivalent via grep and adapt. **DO NOT invent new helpers without first searching.** If a needed primitive truly doesn't exist (e.g. there's no current "write arbitrary file content" command), STOP and report BLOCKED — Phase C cannot ship without C4 device-side support.

- [ ] **Step 3: Register in invoke_handler** — find the `generate_handler![...]` macro (around line 3160), add `install_clawpkg_over_usb,` alongside `launch_agent_with_prompt`.

- [ ] **Step 4: Cargo test + build** — `cd ref/src-tauri && cargo test 2>&1 | tail -10 && cargo build 2>&1 | tail -5`. Expect: tests pass (no new unit test required for the IPC command itself; testing belongs at the integration layer). Build clean.

- [ ] **Step 5: 提交** — `git commit -m "feat(usb): install_clawpkg_over_usb Tauri 命令"`

---

## Task C4: 设备端 `payload_write` 消息处理（board-runtime）

**Files:**
- Modify: `board-runtime/src/board_serial_bridge.c`

This task expands the JSON-line USB protocol. Existing message types include `asset_begin / asset_chunk / asset_commit`. Add `payload_write` for small text payloads (no chunking; one message contains the whole payload).

- [ ] **Step 1: Recon** — `grep -n '"asset_begin"\|"asset_chunk"\|"asset_commit"\|handle_message\|message_type' src/board_serial_bridge.c | head -20`. Find the message dispatcher and understand the JSON parsing helper used.

- [ ] **Step 2: Add `payload_write` handler** — given dispatcher precedent, the new branch looks like:

```c
} else if (strcmp(type, "payload_write") == 0) {
    const char *path_arg = json_get_string(msg, "path");
    const char *content = json_get_string(msg, "content");
    if (!path_arg || !content) {
        send_ack(fd, "payload_ack", false, "missing path or content");
        continue;
    }
    /* Whitelist allowed paths to prevent arbitrary file writes */
    static const char *const ALLOWED[] = {
        ".stats-display", ".current-speech", ".screen-page", NULL
    };
    bool allowed = false;
    for (const char *const *p = ALLOWED; *p; ++p) {
        if (strcmp(path_arg, *p) == 0) { allowed = true; break; }
    }
    if (!allowed) {
        send_ack(fd, "payload_ack", false, "path not in whitelist");
        continue;
    }
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", runtime_root, path_arg);
    if (!br_write_text_file(full_path, content)) {
        send_ack(fd, "payload_ack", false, "write failed");
        continue;
    }
    send_ack(fd, "payload_ack", true, NULL);
    continue;
}
```

Adapt symbol names to whatever the actual dispatcher uses (`json_get_string`, `send_ack`, `br_write_text_file` may be named differently — search and use the real names).

- [ ] **Step 3: Build + run existing layout tests** — `cmake --build build-host --target board-serial-bridge && cmake --build build-host --target fb-speech-overlay-layout-tests && cd build-host && ctest 2>&1 | tail -8`. Expect: build succeeds for both targets; layout tests still pass.

- [ ] **Step 4: (Optional) Add a unit test if `board_serial_bridge.c` has a test harness** — search `tests/` for any existing bridge test. If none, skip (the integration testing happens at the end-to-end Phase C demo on real hardware).

- [ ] **Step 5: 提交** — `git commit -m "feat(serial-bridge): payload_write 消息类型用于推送 COMPONENT_DASHBOARD_V1 payload"`

---

## Task C5: 三件套 `.clawpkg` zip 内置 + ComponentCenter "安装内置" 按钮

**Files:**
- Create: `claw-pet-manager/ref/builtin-clawpkgs/{slack-off-countdown,tomato-clock,drink-reminder}/{component.json,negative-screen.json,buttons.json,runtime/.keep,assets/.keep,share.json}`
- Create: `claw-pet-manager/scripts/pack-builtin-clawpkgs.js` (build-time packer)
- Modify: `claw-pet-manager/ref/src/ComponentCenter.jsx` (add "安装到设备" button per built-in card)
- Modify: `claw-pet-manager/ref/src/ComponentCenter.test.js` (assert new button + handler)

- [ ] **Step 1: 创建三个 builtin 包目录 + 文件** — for each id (`slack-off-countdown`, `tomato-clock`, `drink-reminder`), pull the `dashboard` + `defaultBindings` + name/id from `mock-data.js` (already authoritative). Each directory gets:
  - `component.json` — `{id, name, version: "1.0.0", author: "openclaw", description: <copy of mock 'goal'>}`
  - `negative-screen.json` — `{dashboard: <copy of mock dashboard>}`
  - `buttons.json` — `<copy of mock defaultBindings>` (the array)
  - `runtime/.keep` — empty
  - `assets/.keep` — empty
  - `share.json` — `{title: <name>, summary: <mock sharePayload>}`

- [ ] **Step 2: Pack script** — `scripts/pack-builtin-clawpkgs.js`:

```js
import { createWriteStream, readdirSync, statSync, readFileSync } from "node:fs";
import { resolve, join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import zlib from "node:zlib";

// Tiny zip writer (no external deps): writes a single STORE (uncompressed) zip
// since payload is small and bundling is build-time only.
// ...minimal pure-JS zip implementation OR shell out to /usr/bin/zip if present.

const here = dirname(fileURLToPath(import.meta.url));
const srcRoot = resolve(here, "../ref/builtin-clawpkgs");
const outRoot = resolve(here, "../ref/builtin-clawpkgs-built");
for (const id of readdirSync(srcRoot)) {
  const dir = join(srcRoot, id);
  if (!statSync(dir).isDirectory()) continue;
  const out = join(outRoot, `${id}.clawpkg`);
  // Simplest: invoke `zip -r` if available, else use pure-JS writer.
  const { execFileSync } = await import("node:child_process");
  execFileSync("zip", ["-r", out, "."], { cwd: dir });
  console.log("packed", out);
}
```

Run script: `cd claw-pet-manager && node scripts/pack-builtin-clawpkgs.js`. Verify 3 `.clawpkg` files appear under `ref/builtin-clawpkgs-built/`.

- [ ] **Step 3: ComponentCenter "安装到设备" button**

In `ref/src/ComponentCenter.jsx`, locate the built-in component card renderer (the cards in the grid that include 三件套). Add a button per card that calls:

```js
async function installBuiltinToDevice(id) {
  const path = `${appBundleResourceDir}/builtin-clawpkgs-built/${id}.clawpkg`;
  const result = await invoke("install_clawpkg_over_usb", { input: { clawpkgPath: path } });
  if (!result.ok) {
    alert(`安装失败: ${result.errors.join("; ")}`);
    return;
  }
  alert(`已安装到设备: ${result.manifest.name}（${result.transferredBytes} bytes）`);
}
```

Reuse the existing `magicMirrorHint` / button styling. The button label: `"安装到设备"`. Note: resource path needs the Tauri `resolveResource` API or equivalent to find the packed zips at runtime — if that integration is non-trivial, fall back to hardcoded `~/.openclaw/builtin-clawpkgs/{id}.clawpkg` and have the pack script also copy outputs to that user dir on `npm install`. **Pick the simpler of these.**

- [ ] **Step 4: Update test asserts** — add to `ComponentCenter.test.js` data section:

```js
assert.match(component, /install_clawpkg_over_usb/);
assert.match(component, /安装到设备/);
```

- [ ] **Step 5: Run all tests** — `cd ref && node --test src/lib/clawpkg-contract.test.js src/lib/component-generation-template.test.js src/ComponentCenter.test.js 2>&1 | tail -10` → expect 11/11 pass (existing 10 + new) and `cd ref/src-tauri && cargo test 2>&1 | tail -5` → still pass.

- [ ] **Step 6: 提交** (split into 2 commits for clarity):

```bash
# Commit 1: builtin packs + pack script
git add ref/builtin-clawpkgs scripts/pack-builtin-clawpkgs.js
git commit -m "feat(clawpkg): 三件套 builtin clawpkg 资源 + 打包脚本"

# Commit 2: UI wire + tests
git add ref/src/ComponentCenter.jsx ref/src/ComponentCenter.test.js
git commit -m "feat(ui): ComponentCenter 三件套 '安装到设备' 按钮接 install_clawpkg_over_usb"
```

---

## Task C6: 文档同步

**Files:**
- Modify: `claw-pet-manager/ref/.folder.md`、`claw-pet-manager/ref/src/.folder.md`、`claw-pet-manager/ref/src-tauri/src/.folder.md`（如存在）
- Modify: `board-runtime/docs/component-dashboard-v1.md`(append a "USB delivery" section)

- [ ] **Step 1:** 同步 folder docs:
  - 新文件：`ref/src-tauri/src/clawpkg.rs`
  - 新命令：`install_clawpkg_over_usb`
  - 新目录：`ref/builtin-clawpkgs/`、`scripts/pack-builtin-clawpkgs.js`
  - ComponentCenter 新增 "安装到设备" 流

- [ ] **Step 2:** 在设备端 `docs/component-dashboard-v1.md` 末尾加：

```markdown
## USB OTA 投放

由 `claw-pet-manager` 的 `install_clawpkg_over_usb` Tauri 命令把 negative-screen.json 中的 dashboard 转成 COMPONENT_DASHBOARD_V1 文本 payload,通过串行 USB 的 `payload_write` JSON-line 消息写入设备 `runtime_root/.stats-display`,然后通过 `control/screen-page` 切换到 stats 视图。`fb_speech_overlay` 在下一轮 poll 读到新 payload 即触发 `br_overlay_render_component_dashboard`。
```

- [ ] **Step 3:** 提交两侧文档同步：
  - `claw-pet-manager`:`git commit -m "docs: Phase C 同步 folder 文档"`
  - `board-runtime`:`git commit -m "docs: COMPONENT_DASHBOARD_V1 USB OTA 章节"`

---

## 自检结论

- 覆盖 spec:
  - 校验（C1）+ payload 转换（C2）+ Tauri 命令（C3）+ 设备 payload_write 消息（C4）+ 三件套 zip + UI 接线（C5）+ 文档（C6）
- 真机依赖:仅 C5 Step 5 浏览器手测 + 端到端 demo 需要硬件;其它任务用 cargo / node test 覆盖。Phase C 不强制端到端真机验证就可合,留 C7（未列入,后续会话执行）做真机 demo。
- 字节预算:Rust 端用 `as_bytes().len()` 计算 UTF-8 byte length,与 JS `TextEncoder` + C `strlen` 三端一致。
- 类型一致性:`COMPONENT_DASHBOARD_V1_SLOTS` 在三处保持相同 9 槽位 id + maxBytes:`ref/src/lib/clawpkg-contract.js`、`ref/src-tauri/src/clawpkg.rs`、`board-runtime/src/fb_speech_overlay.c` `br_component_dashboard_set_value`。
