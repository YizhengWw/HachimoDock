# HachimoDock DSH bridge

This dependency-free Cordis plugin adds DSH session status, exact-session prompt
admission/cancellation, a physical approval relay and session-scoped voice input.
Node tests cover the host protocol in isolation; they do not certify an installed
DSH version or a physical device.

## Compatibility

The statically checked API reference is DeepSeek Harness `0.2.2-alpha.2`, upstream commit
`ce7073d51254c26cd63b22a60835b8f948deb447`:

- [approval/request and closed outcomes](https://github.com/deepseek-ai/deepseek-harness/blob/ce7073d51254c26cd63b22a60835b8f948deb447/packages/interaction/user-approval/src/types.ts)
- [sessionController prompt/cancel](https://github.com/deepseek-ai/deepseek-harness/blob/ce7073d51254c26cd63b22a60835b8f948deb447/packages/api/session-controller/src/index.ts)
- [strict Session slots and InputActions](https://github.com/deepseek-ai/deepseek-harness/blob/ce7073d51254c26cd63b22a60835b8f948deb447/packages/client/ui-conversation/src/client/contract/slots.ts)
- [framework Session inject parameters](https://github.com/deepseek-ai/deepseek-harness/blob/ce7073d51254c26cd63b22a60835b8f948deb447/packages/client/ui-slots/src/index.ts)

The 18 retained reference files match the pinned commit's Git blob identities;
8 API/lifecycle contracts were checked against those files. This is **not an
installed-version contract pass**. The host requires `sessions`, `agents`, `sessionController`. Unsupported
services/slots disable the affected feature. There are no guessed APIs, global
DOM/editor/approval selectors, synthetic keys, delayed send or anonymous fallbacks.

## Desktop input and private bridge bootstrap

DeepSeek Desktop voice input uses the same OS accessibility composer adapter as
ChatGPT and WorkBuddy. Selecting DeepSeek and opening the desired conversation
is sufficient: recording captures the current composer, final recognition appends
only real text, and a separate explicit confirmation submits it. Voice input does
not depend on this plugin, its HTTP endpoints, browser tokens or a pairing code.
The managed client module inserts no pairing controls into DeepSeek.

The plugin remains responsible for session status and supported approval/control
features. Its native status/voice capabilities remain in an owner-only local
descriptor (directory 0700, file 0600; exact protected ACL on Windows). They are
never included in application resources, browser storage, URLs or logs. Legacy
explicit-ticket endpoints and isolated input-consumer helpers remain for older
callers; they are not part of the Desktop voice-input workflow.

The OS adapter freezes the target application/window/editor for each recording.
An unavailable or ambiguous input, changed target, or unknown write result stops
that operation without choosing a historical session or replaying text. Existing
user drafts and placeholder exclusion use the shared desktop-composer behavior.

Windows uses a fixed Windows PowerShell 5.1 program (`-NoProfile`,
`-NonInteractive`, absolute executable path). It resolves the current user's SID
and SYSTEM (`S-1-5-18`) numerically, protects the dedicated directory before
minting credentials, and gives both exactly FullControl with inheritance disabled
on each descriptor file. It checks owner, protected DACL, raw ACE count,
non-inherited allowed SIDs, file kind and every existing path segment for reparse
points. UNC/device/ADS/ambiguous paths and non-local fixed volumes are refused.
An insecure existing descriptor owned by this user is ACL-protected and replaced
without reading or reusing its old token. Foreign owners/reparse entries abort.
Temp files are protected before secret bytes are written; final replacement is
verified again. Readers never repair an unsafe ACL or fall back anonymously.

The PowerShell child receives only operation/path and minimal Windows environment;
no token goes into command arguments, child environment or output. Its bounded
metadata receipt is checked independently by Node/native; errors/timeouts fail
closed. Distributed scripts are byte-identical and native embeds the same script.
The ACL operations use [Get-Acl](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.security/get-acl?view=powershell-5.1),
[Set-Acl](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.security/set-acl?view=powershell-5.1)
and [protected access rules](https://learn.microsoft.com/en-us/dotnet/api/system.security.accesscontrol.objectsecurity.setaccessruleprotection?view=netframework-4.8.1).
POSIX retains uid/0700-directory/0600-file/O_NOFOLLOW checks. Windows OS/PowerShell
execution is **NOT_RUN** on the available macOS host; mock policy tests and static
API checks are not presented as a physical Windows validation pass.

## Approval ownership and cancellation

Deploy this as one terminal `approval/request` answerer. The patch's delegate
policy calls `next()` **only before claiming**, when no authenticated USB relay
is attached. Once claimed, the original request Promise/AbortSignal, session,
opaque requestId, instance epoch and exact relay lease own it. It never runs
native approval UI concurrently or guesses audit IDs by tool/time. Official
approval audit events remain display-only.

One terminal take retires timers/listeners/resolvers. Identical HTTP decisions
return the original receipt; conflicts, other sessions, stale epochs/leases and
expired requests cannot decide. Abort returns `cancelled`; capacity/disconnect
fails closed. The native relay binds each request to the current USB board and
connection generation, serializes physical modals and limits live workers to 16.
Cancel/expiry/disconnect remove and wake waiters, including timeout sentinel
99999 (no approval timer while healthy); they never POST a decision. Reconnection
starts a new lease without replaying approval/voice commands. Unknown decision
POST outcomes are logged generically and never automatically resent.

## Legacy plugin voice protocol and receipt semantics

The following describes the retained explicit-ticket/InputActions compatibility
API for older integrations. The current desktop manager uses the OS composer
path above and does not send `voice/begin` or wait for a plugin input ACK.
For a caller explicitly using this legacy protocol, capture start sends `voice/begin` for the frozen
session/utterance without blocking the USB reader. Final insertion requires the
mounted owner's captured InputActions span ACK (a late/missing ACK fails closed).
Native buffers the recording while awaiting the exact input capture receipt; ASR and audio forwarding start only after that acknowledgement.
Final insertion uses
that original draft revision. An edit, locked editor, session change or unmount
rejects the operation without replacing text or trying a different editor.
Submission references the successful insertion and the same owner/epoch/lease/
utterance/revision. It is refused if the draft contains other text, attachments
or reference chips or its revision changed. InputActions `submit():void` yields
**submit-requested**, not a delivery/tool-success/completion claim. An exception,
lost ACK or disconnect consumes the operation as unknown; it is never retried.

Bus injection requires explicit sessionId plus a random requestId and uses
`sessionController.prompt({requestId,sessionId,mode:'queue',content},signal)`.
Its terminal `done/stopReason:admitted` means admission only. Live status/display
text continues through session events/summaries. Neither `auto`/newest-session
fallback nor whole-agent `whenIdle` is used as a per-utterance completion proof.
Interrupt uses official `sessionController.cancel({sessionId})`, whose referenced
implementation preserves pending inbox entries. It reports cancel-requested.

## Transport bounds

All sensitive requests require scoped Bearer credentials, exact loopback Host
and peer; browser Origin must match its paired capability. Native HTTP disables
proxies/redirects. Writes require strict JSON object/schema (64KiB maximum;
prototype keys rejected). SSE uses authenticated fetch, not query tokens or
EventSource. Replay cursors bind epoch/client, with a 512-event ring; old
approval/voice commands never replay. A subscriber is disconnected above a
128KiB Node output queue, with an 8-stream cap. Approval pending/workers are
bounded to 16 and terminal receipts to 256; voice pending records are bounded to
64 with 120s lifetime and client actions to 128 per mounted lease. Native frames
and bus responses are bounded to 64KiB. Unknown writes never trigger fallback.

## Focused checks

Run `node --test integrations/dsh-hachimodock/test/*.test.js` and
`node --test pc/src-tauri/bridge/packages/agent-session-bus/test/dsh-adapter.test.js`
in a synthetic HOME/minimal environment under the parent's loopback-only OS
sandbox. No installs, model calls, real DSH instance or serial port are required.
A release offline locked lib check validates native types with bundle-resource
inputs omitted for this code-only check; it is not a package validation. A small
native test harness compiles the actual DSH source modules with cached dependencies
and only the disconnected USB boundary stubbed. These checks do not certify the
installed DSH ABI or actual Windows/PowerShell behavior.
