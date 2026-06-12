# Code Hygiene Scan Report

## Summary

- Scope: full local repository rooted at `claw-pet-manager`.
- The repository is a mixed Tauri/React desktop app plus Raspberry Pi board runtime, with generated media and bundled runtime assets.
- Low-risk cleanup applied: regenerated playstyle output was removed from tracking, `output/playstyle-assets/` was ignored, and the required LFS rule for `ref/src-tauri/bridge/runtime/node` was restored.
- Local agent folder-map files named `.folder.md` are now ignored and should remain local-only.
- No tracked `node_modules`, logs, temp files, or obvious private key material were found.

## Findings

### P1: Bundled Node runtime must stay under Git LFS

- Evidence: `ref/src-tauri/bridge/runtime/node` is 106 MB and is referenced by `ref/src-tauri/tauri.conf.json`, `ref/src-tauri/src/lib.rs`, and `ref/src/ProductExperience.test.js`.
- Risk: without `.gitattributes`, GitHub rejects the file as a normal Git blob over 100 MB.
- Action taken: restored `.gitattributes` with `ref/src-tauri/bridge/runtime/node filter=lfs diff=lfs merge=lfs -text`.
- Follow-up: keep platform runtime binaries either in LFS or move them to a documented download/bootstrap step.

### P1: Windows runtime resource is configured but absent locally

- Evidence: `npm test` failed the product experience test with `bridge/runtime/node.exe should bundle as bridge/runtime/node.exe`.
- Current state: `.gitignore` ignores `ref/src-tauri/bridge/runtime/node.exe`, and the file is not present locally.
- Risk: release-resource tests and Windows packaging assumptions disagree with source control policy.
- Suggested next step: either add a Git LFS-managed `node.exe` artifact, or make the resource/test platform-aware and document the Windows runtime bootstrap.

### P2: Regenerable playstyle output was tracked

- Evidence: `output/playstyle-assets/*.png` totaled about 11 MB, had no source or documentation references, and lived outside the documented folder map.
- Action taken: removed the six tracked files and added `output/playstyle-assets/` to `.gitignore`.
- Rationale: keep generated avatar/playstyle exploration output out of repository history unless promoted into `ref/public/` or docs as product assets.

### P2: Local `.folder.md` files should not be pushed

- Evidence: 13 `.folder.md` files were tracked across root, `.cursor/`, `.github/`, `docs/`, `ref/`, `scripts/`, and `skills/`.
- Action taken: added `.folder.md` to `.gitignore` and removed tracked `.folder.md` paths from the Git index while leaving the local files on disk.
- Rationale: these are local agent folder-map docs, not product source or runtime assets.

### P2: Docs check reports many stale path references

- Evidence: `bash scripts/docs-check.sh` exits 1 and reports stale references such as API topics (`asset/ack`, `input/action`), symbols (`Card.Collapsible`), home paths (`~/.openclaw/...`), and actual path misses.
- Risk: the checker currently mixes true stale paths with backticked protocol names and code symbols, so it is noisy as a quality gate.
- Suggested next step: refine `scripts/docs-check.sh` so it only validates explicit file paths or add an allowlist for protocol routes, symbols, and shell-home paths.

### P2: Frontend test suite now isolates one runtime-resource failure

- Evidence: after `npm ci` in `ref/`, `npm test` reports 297 passing tests and 1 failing test.
- Remaining failure: `ProductExperience.test.js` expects `bridge/runtime/node.exe` to be bundled.
- Current hygiene state: `ref/node_modules/` is ignored and present only as a local validation dependency install.
- Suggested next step: resolve the Windows runtime policy in the P1 finding, then rerun `npm test`.

### P2: npm audit reports moderate dependency advisories

- Evidence: `npm audit --audit-level=moderate` in `ref/` reports 3 moderate vulnerabilities.
- Packages: `esbuild <=0.24.2` through `vite <=6.4.1`, and `postcss <8.5.10`.
- Risk: `npm audit fix --force` would install `vite@8.0.16`, which is a breaking major upgrade.
- Suggested next step: plan a controlled Vite/PostCSS upgrade with a build and UI smoke test rather than applying the force fix blindly.

### P3: Fixture `dist/` is intentionally tracked

- Evidence: only tracked `dist/` files are under `ref/src-tauri/bridge/packages/agent-session-bus/test/fixtures/fake-openclaw/dist/`.
- Rationale: `fake-openclaw/package.json` points `main` at `dist/index.js`; these are fixture files, not generated build output.
- Action taken: kept them tracked.

### P3: Board environment file is a template-style runtime config

- Evidence: `board-runtime/board-runtime-rpi.env` contains defaults and an empty `BOARD_RUNTIME_ADMIN_TOKEN`, not a populated secret.
- Risk: future local copies may accidentally add real credentials.
- Action taken: added generic `.env` and `.env.*` ignores while preserving `*.env.example`.

## Validation Evidence

- `find . -path ./.git -prune -o -type f -size +10M`: only `ref/src-tauri/bridge/runtime/node` exceeds 10 MB.
- `git ls-files | rg '(^|/)node_modules(/|$)'`: no tracked `node_modules`.
- `find . ... '*.log' '*.tmp' '*.bak' '*.orig' '.DS_Store'`: no matching local files found.
- `npm ci` in `ref/`: passed; installed local ignored `node_modules/`.
- `npm test` in `ref/`: failed 1 of 298 tests; remaining failure is missing `bridge/runtime/node.exe`.
- `npm audit --audit-level=moderate` in `ref/`: failed with 3 moderate advisories.
- `cmake -S . -B /tmp/board-runtime-hygiene-check && cmake --build /tmp/board-runtime-hygiene-check --target board-server`: passed.
- `bash scripts/docs-check.sh`: failed due noisy stale-reference detection listed above.

## Cleanup Policy

- Keep source, docs, fixtures, package lockfiles, small product media, and deliberate board/runtime assets.
- Ignore local dependencies, build outputs, caches, coverage, logs, local secrets, local `.folder.md` folder maps, generated anchor metadata, and regenerable playstyle output.
- Store unavoidable large runtime binaries with Git LFS, not normal Git blobs.
