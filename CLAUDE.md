# Claude Workspace Control Plane

<!-- vibe-loader-pointer:start -->
## 0) Update Contract
- Any functional, architectural, or coding-style change must update related folder docs before session end.
## 0.5) Repository Overview
- This is a single repository for both the desktop Pet Manager and the Raspberry Pi board runtime.
- `ref/` is the Tauri 2 + React desktop app and local bridge sidecar.
- `board-runtime/` is the Raspberry Pi device runtime.
- Use `README.md`, `ref/README.md`, and `board-runtime/README.md` as the human-facing entry docs.
## 1) Rules Reference
- `docs/rules/README.md`
- `docs/rules/no-hardcoding.md`
- `docs/rules/component-reuse.md`
## 2) Golden Rules
- No hardcoding in business code.
- Before introducing a new variable/constant, search higher-level global definitions first.
## 3) Tech Stack
- `docs/tech-stack.md` (Backend/Frontend/DB/SDK definitions)
## 4) Session Start
- `docs/session-start.md`
## 5) Vibe Loader Pointer
- Hot: `CLAUDE.md`, `today.md`
- Rules: `.cursor/rules/vibe-engineering.mdc`, `.cursor/rules/vibe-loading.mdc`, `.cursor/rules/vibe-doc-sync.mdc`, `.cursor/rules/vibe-component-reuse.mdc`
- Heavy: `docs/loading-index.md`, `docs/workspace-structure.md`, and selected `docs/*.md`
## 6) Common Commands
- Desktop dev/test/build: `cd ref && npm run dev`, `cd ref && npm test`, `cd ref && npm run build`
- Board build check: `cd board-runtime && cmake -S . -B /tmp/board-runtime-build-check && cmake --build /tmp/board-runtime-build-check --target board-server`
- Board deploy template: `cd board-runtime && export BOARD_HOST="<pi-user>@<pi-ip>" && HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh`
<!-- vibe-loader-pointer:end -->
