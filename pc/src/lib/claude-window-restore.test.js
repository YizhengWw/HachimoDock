import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { spawnSync } from "node:child_process";

const source = readFileSync(new URL("../../src-tauri/src/codex_composer.rs", import.meta.url), "utf8");
const restore = source.match(/function Get-RestoredClaudeWindows \{[\s\S]*?(?=function Get-OrLaunchClaudeWindows)/)?.[0];

test("bound Claude navigation restores existing windows without launching sessions", () => {
  assert.ok(restore);
  const open = source.match(/function Open-ClaudeSession\([\s\S]*?(?=function Test-WorkspaceAncestor)/)?.[0];
  assert.match(open, /\$existingWindows = @\(Get-RestoredClaudeWindows\)/);
  assert.doesNotMatch(open, /Get-OrLaunchClaudeWindows|Start-Process/);
  assert.doesNotMatch(restore, /Start-Process/);
});

test("Claude restore handles visible, minimized, absent and nonresponsive windows", {
  skip: process.platform !== "win32",
}, () => {
  const script = `
$ErrorActionPreference = 'Stop'
Add-Type 'public static class CodexVoiceNative { public static int Restores; public static bool RestoreWindow(System.IntPtr h) { Restores++; return true; } }'
function Get-Process { param($Name, $ErrorAction) if ($script:scenario -ne 'absent') { [pscustomobject]@{MainWindowHandle=123} } }
function Get-ClaudeWindows {
  if ($script:scenario -eq 'visible' -or ($script:scenario -eq 'minimized' -and [CodexVoiceNative]::Restores -gt 0)) {
    [pscustomobject]@{WindowHandle=123; Root='fresh-uia'}
  }
}
function Get-MonotonicMilliseconds { $script:clock += 500; return $script:clock }
function Start-Sleep { param($Milliseconds) }
${restore}
foreach ($case in @('visible','minimized','absent','timeout')) {
  $script:scenario = $case
  $script:clock = 0
  [CodexVoiceNative]::Restores = 0
  $errorMessage = ''
  $result = @()
  try { $result = @(Get-RestoredClaudeWindows) } catch { $errorMessage = $_.Exception.Message }
  switch ($case) {
    'visible' { if ($result.Count -ne 1 -or [CodexVoiceNative]::Restores -ne 0) { throw 'visible window was unnecessarily restored' } }
    'minimized' { if ($result.Count -ne 1 -or $result[0].Root -ne 'fresh-uia' -or [CodexVoiceNative]::Restores -ne 1) { throw 'minimized window was not reacquired' } }
    'absent' { if ($result.Count -ne 0 -or [CodexVoiceNative]::Restores -ne 0) { throw 'absent window was restored' } }
    'timeout' { if ($errorMessage -notlike '*could not be restored*' -or $script:clock -gt 3000) { throw 'restore timeout was not bounded' } }
  }
  if ($case -ne 'timeout' -and $errorMessage) { throw $errorMessage }
}
Write-Output 'restore cases passed'
`;
  const result = spawnSync("powershell.exe", ["-NoProfile", "-NonInteractive", "-EncodedCommand", Buffer.from(script, "utf16le").toString("base64")], {
    encoding: "utf8", windowsHide: true, timeout: 20000,
  });
  assert.equal(result.status, 0, result.stderr || result.error?.message);
  assert.match(result.stdout, /restore cases passed/);
});
