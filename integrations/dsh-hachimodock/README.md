# HachimoDock DSH bridge

This dependency-free Cordis plugin adds DSH session status, exact-session prompt
admission/cancellation, a physical approval relay and session-scoped voice input.
The original PR's firmware cues and DSP implementation are unchanged by the host
remediation. No device or installed DSH validation is claimed by the Node tests.

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

## Private bootstrap and explicit pairing

On startup the host binds `127.0.0.1:17892` and generates a random instance epoch
and separate 256-bit relay, bus and voice capabilities. It atomically writes
`~/.claw-pet/dsh-bridge/capability.json` (directory 0700, file 0600). Set
`HACHIMODOCK_DSH_CAPABILITY_FILE` consistently in host/native/bus to use another
private path. Symlinks, wrong owners and permissive files/directories are refused.
No token belongs in a repository, URL, log, package or ordinary browser storage.
The same OS user with access to this file is trusted; this does not isolate that
user's malicious processes.

Configure `allowedOrigins` in `cordis.patch.yml` to the **actual exact** DSH web
Origin (scheme, hostname, port). The default empty list disables browser voice.
No guessed localhost port, `*`, `null` or reflected foreign Origin is accepted.
For each desired session run this local control explicitly:

```sh
node integrations/dsh-hachimodock/pair.js https://YOUR-EXACT-DSH-ORIGIN EXACT_SESSION_ID
```

Paste its one-use, two-minute ticket into that session's HachimoDock composer
dock. The ticket is bound to that Origin/session. Browser tokens stay in memory,
expire after one hour and are revoked on stream disconnect/unmount. Only one
mounted window owns a session's voice stream. Re-pair explicitly after a switch,
unmount or lost connection; no pending action is automatically replayed.
The browser dock currently targets the default port 17892; custom host ports
support native/bus callers, but browser voice is unavailable there.

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

## Voice and receipt semantics

At capture start, native asynchronously sends `voice/begin` for the frozen
session/utterance without blocking the USB reader. Final insertion requires the
mounted owner's captured InputActions span ACK (a late/missing ACK fails closed).
ASR startup is unchanged; this is not a claim that browser capture precedes ASR.
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
