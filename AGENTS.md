# AGENTS Instructions

<!-- vibe-agents:start -->
## Vibe Contract
- Any functional, architectural, or coding-style change must update affected folder docs and file headers before session end.
- Prefer reuse-first refactor: search existing modules/components before adding new implementations.
- Avoid hard-coded business IDs/thresholds/paths; resolve to higher-level config first.

## Source Of Truth
- Root pointer: `CLAUDE.md`
- Folder contracts: `**/.folder.md`
- Rules index: `docs/rules/README.md`
- Cursor rules: `.cursor/rules/*.mdc`

## Repository Layout
- This is now a single repository containing both the desktop manager and the device runtime.
- `ref/` owns the Tauri/React desktop manager and bridge sidecar.
- `board-runtime/` owns the Raspberry Pi board runtime.
- When a change crosses desktop/device boundaries, update both affected folder docs in the same commit.

## Common Commands
- Desktop dev: `cd ref && npm run dev`
- Desktop tests: `cd ref && npm test`
- Desktop build: `cd ref && npm run build`
- Board host build check: `cd board-runtime && cmake -S . -B /tmp/board-runtime-build-check && cmake --build /tmp/board-runtime-build-check --target board-server`
- Board deploy: `cd board-runtime && export BOARD_HOST="<pi-user>@<pi-ip>" && HOST="$BOARD_HOST" sh scripts/deploy-rpi.sh`

## Device Targeting
- Do not hard-code one board's SSH user, IP address, board id, or desktop id in docs or business logic.
- Use `BOARD_HOST="<pi-user>@<pi-ip>"` for SSH/deploy examples and `BOARD_IP="<pi-ip>"` for HTTP examples.

## Validation
- Run requested validation commands after meaningful changes.
- Report concrete evidence (command + exit code + key output) in final summary.
<!-- vibe-agents:end -->
