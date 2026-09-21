# Clawd on Desk 源码来源

本文列出项目使用的 Clawd on Desk MIT 版本代码、对应文件和来源提交。相关版权与 MIT 许可全文见 [第三方声明](../../THIRD_PARTY_NOTICES.md)。

## Upstream license boundary

The complete upstream Git history was inspected on 2026-08-11.

- Clawd on Desk tags `v0.5.9` and `v0.6.1` declare the source license as MIT.
- Upstream commit `19e8f82493b0993554df62b3eba419b1127fff14`
  dated 2026-04-25 is the last audited MIT commit.
- Upstream commit `3b6277ff39b4473bd0b0a09d55a695b176c815e9`
  changes the source license from MIT to AGPL-3.0.
- The exact Clawd blobs retained by HachimoDock already existed before that
  license change.

## Byte-identical retained files

The following current HachimoDock files are byte-identical to Clawd files
published in the MIT period:

| HachimoDock path | Git blob | Audited upstream evidence |
|---|---|---|
| `pc/src-tauri/bridge/hooks/auto-start.js` | `d782a2912f3e559ab2848affc4b382c901bea789` | `hooks/auto-start.js`, present at `df9573a4` |
| `pc/src-tauri/bridge/hooks/codebuddy-install.js` | `50a4e75a0044d67425f64a06fb7457bb7f994ad1` | `hooks/codebuddy-install.js`, present at `1b87fc80` |
| `pc/src-tauri/bridge/hooks/cursor-install.js` | `63a25e971aed989392eb16231875f72ac569598e` | `hooks/cursor-install.js`, present at `0ed767a6` |
| `pc/src-tauri/bridge/hooks/gemini-install.js` | `952b537d22db7c860341db44c7c61aa8c9b22a90` | `hooks/gemini-install.js`, present at `0ed767a6` |
| `pc/src-tauri/bridge/hooks/server-config.js` | `aa3c722880e237a1b2b6afe10c841fc206ff0259` | `hooks/server-config.js`, present at `0ed767a6` |

The full upstream MIT notice is preserved in
[`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md).

## Modified MIT-derived runtime files

The current bridge also contains modified descendants of the same MIT-era
hook and Agent-monitor implementation. They include:

- `pc/src-tauri/bridge/hooks/install.js`
- `pc/src-tauri/bridge/hooks/clawd-hook.js`
- `pc/src-tauri/bridge/hooks/codebuddy-hook.js`
- `pc/src-tauri/bridge/hooks/cursor-hook.js`
- `pc/src-tauri/bridge/hooks/gemini-hook.js`
- `pc/src-tauri/bridge/agents/codex-log-monitor.js`
- `pc/src-tauri/bridge/agents/codex.js`
- `pc/src-tauri/bridge/agents/claude-log-monitor.js`
- `pc/src-tauri/bridge/agents/claude-code.js`

These files have since received substantial Pet Manager-specific changes, but
their source lineage must continue to retain the upstream MIT notice.

## Current runtime reachability

The Clawd-derived layer is not documentation-only:

1. Tauri packages the bridge source, Agent monitors, and hook scripts.
2. The Rust desktop runtime starts
   `packages/clawd-backend-service/src/headless-mqtt.js` with the bundled Node
   runtime.
3. The bridge always imports `hooks/server-config.js`.
4. Codex and Claude enable their corresponding log-monitor workers.
5. When legacy Agent hook synchronization is enabled, `hooks/sync-all.js`
   installs or repairs the Claude, Gemini, Cursor, and CodeBuddy hooks.

## 维护这些文件

`npm test` 会执行 `scripts/check-clawd-provenance.mjs`，核对未修改文件的 SHA-256 与许可声明。修改或引入上游代码时，需要确认对应版本的许可并更新来源记录。

第三方声明须随源码及客户端分发。历史版本已授予的许可权利不受本项目后续许可变更影响。
