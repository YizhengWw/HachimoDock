# Cross-platform parser and mocked policy tests; never accesses desktop UI.
# Run with PowerShell 7+. Native UIA/Windows 5.1 integration still needs Windows.
$ErrorActionPreference = 'Stop'
$source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../src-tauri/src/codex_composer.rs'))
$script = [regex]::Match($source, '(?s)const SCRIPT: &str = r#"(.*?)"#;').Groups[1].Value
if (-not $script) { throw 'Embedded production script missing' }
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseInput($script, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$native = [regex]::Match($script, '(?s)Add-Type @"\r?\n(.*?)\r?\n"@').Groups[1].Value
if (-not $native.Contains('public static class WorkBuddyUia {')) { throw 'Native WorkBuddy UIA adapter missing' }
# The COM UIA adapter (interfaces, wrappers and walkers) compiles as-is; only
# its static entry point is renamed so the fixture below can stand in for it.
Add-Type ([regex]::Replace($native.Replace('class CodexVoiceNative', 'class CompileOnlyNative'),
  '\bWorkBuddyUia\b', 'CompileOnlyWorkBuddyUia'))
# Compile the real P/Invoke declarations but never invoke them on this host.
Add-Type @'
namespace System.Windows.Automation {
  // A managed element would hide Chromium's native provider for the whole
  // helper process, so any WorkBuddy use of it fails these tests.
  public class AutomationElement {
    public static object FocusedElement { get { throw new System.InvalidOperationException("managed UIA used"); } }
    public static object FromHandle(System.IntPtr window) { throw new System.InvalidOperationException("managed UIA used"); }
  }
  public class ValuePattern { public ValueState Current = new ValueState(); }
  public class ValueState { public bool IsReadOnly; }
  public class TextPattern {
    public static object IsReadOnlyAttribute = new object();
    public TextRange DocumentRange = new TextRange();
  }
  public class TextRange { public object ReadOnly; public object GetAttributeValue(object a) { return ReadOnly; } }
  public class AutomationProperty { public static object LookupById(int id) { return new object(); } }
  public class ControlType {
    public int Id;
    public ControlType(int id) { Id = id; }
    public static ControlType Edit = new ControlType(50004);
    public static ControlType Group = new ControlType(50026);
    public static ControlType Custom = new ControlType(50025);
    public static ControlType Document = new ControlType(50030);
  }
}
public static class CodexVoiceNative {
  public static bool Foreground = true;
  public static bool ThrowRequest;
  public static int Response = 3;
  public static object ActivatedEditor;
  public static System.IntPtr[] Children = new System.IntPtr[0];
  public static System.Collections.Generic.List<System.IntPtr> Requests = new System.Collections.Generic.List<System.IntPtr>();
  public static bool IsForeground(System.IntPtr window) { return Foreground; }
  public static bool ActivateWindow(System.IntPtr window) { return Foreground; }
  public static bool IsOwnedWindow(System.IntPtr parent, System.IntPtr child) {
    return parent == child || (child.ToInt64() != 999 && System.Array.IndexOf(Children, child) >= 0);
  }
  public static System.IntPtr[] FindChildWindowsByClass(System.IntPtr parent, string className) { return Children; }
  public static int RequestClientAccessibility(System.IntPtr parent, System.IntPtr window, int processId) {
    Requests.Add(window);
    if (ThrowRequest) { throw new System.InvalidOperationException("fixture rejected"); }
    if (ActivatedEditor != null) { WorkBuddyUia.Focused = ActivatedEditor; }
    return Response;
  }
}
// Stands in for the native COM UIA entry point. The production WorkBuddy path
// must never touch System.Windows.Automation.AutomationElement.
public static class WorkBuddyUia {
  public static object Focused;
  public static object Root;
  public static int RootReads;
  public static object FromHandle(System.IntPtr window) { RootReads++; return Root; }
  public static object FocusedElement() { return Focused; }
  public static object[] FindBounded(System.IntPtr window, bool rawView, int budgetMs, int maxNodes) { return new object[0]; }
  public static object[] FindAtPoints(System.IntPtr window, double[] xRatios, double[] yRatios, int budgetMs) { return new object[0]; }
}
'@
$names = @('Normalize-Label', 'Normalize-ComposerText', 'Test-AllowedComposerValue',
  'Test-FiniteWindowRectangle', 'Test-WorkBuddyWritablePattern', 'Get-ComposerCandidates',
  'Get-ElementRuntimeId', 'Select-WorkBuddyComposer', 'Find-WorkBuddyFocusedComposer',
  'Initialize-WorkBuddyAccessibility', 'Get-WorkBuddyTarget')
foreach ($name in $names) {
  $function = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }.GetNewClosure(), $true)
  if (-not $function) { throw "Production function missing: $name" }
  Invoke-Expression $function.Extent.Text
}
# UIA adapters are fixtures; filtering and selection above are production code.
function Test-WorkBuddyElementOwner($root, $element) { return $element.Owned }
function Get-ComposerText($element) { return @($element.Pattern, $element.Value) }
function Assert($condition, $label) { if (-not $condition) { throw "FAILED: $label" }; Write-Output "PASS: $label" }
function Rect($x, $y, $w, $h) { return [pscustomobject]@{Left=$x; Top=$y; Width=$w; Height=$h; Right=($x+$w); Bottom=($y+$h); IsEmpty=$false} }
$root = [pscustomobject]@{Current=[pscustomobject]@{BoundingRectangle=(Rect 0 0 1200 900)}}
function Editor($type, $role = '', $name = '') {
  $e = [pscustomobject]@{
    Current=[pscustomobject]@{IsEnabled=$true; IsOffscreen=$false; IsKeyboardFocusable=$true; HasKeyboardFocus=$false;
      BoundingRectangle=(Rect 320 510 600 120); ClassName=''; Name=$name; ControlType=$type}
    Pattern=[System.Windows.Automation.ValuePattern]::new(); Value=''; Owned=$true; AriaRole=$role; Identity=1
  }
  $e | Add-Member ScriptMethod GetCurrentPropertyValue { param($property) return $this.AriaRole }
  $e | Add-Member ScriptMethod GetRuntimeId { return @(42, $this.Identity) }
  return $e
}
$document = Editor ([System.Windows.Automation.ControlType]::Document) 'textbox'
Assert (@((Get-ComposerCandidates $root @($document) $null $true).Candidates).Count -eq 1) 'editable textbox Document accepted for WorkBuddy'
Assert (@((Get-ComposerCandidates $root @($document) $null).Candidates).Count -eq 0) 'other Agent policy unchanged'
$document.Pattern.Current.IsReadOnly = $true
Assert (@((Get-ComposerCandidates $root @($document) $null $true).Candidates).Count -eq 0) 'readonly document excluded'
$document.Pattern.Current.IsReadOnly = $false
$document.AriaRole = ''; $document.Current.Name = '聊天消息'
Assert (@((Get-ComposerCandidates $root @($document) $null $true).Candidates).Count -eq 0) 'chat transcript excluded even with message label'
$edit = Editor ([System.Windows.Automation.ControlType]::Edit)
Assert (@((Get-ComposerCandidates $root @($edit) $null $true).Candidates).Count -eq 1) 'anonymous writable Edit accepted'
$edit.AriaRole = 'searchbox'
Assert (@((Get-ComposerCandidates $root @($edit) $null $true).Candidates).Count -eq 0) 'search excluded'
$edit.AriaRole = ''; $edit.Owned = $false
Assert (@((Get-ComposerCandidates $root @($edit) $null $true).Candidates).Count -eq 0) 'foreign window excluded'
$edit.Owned = $true; $edit.Value = 'changed'
$set = Get-ComposerCandidates $root @($edit) @('original') $true
$refused = $false; try { Select-WorkBuddyComposer $set } catch { $refused = $true }
Assert $refused 'externally edited draft refused'
$edit.Value = ''
$set = Get-ComposerCandidates $root @($edit, $edit) $null $true
Assert ($null -ne (Select-WorkBuddyComposer $set)) 'same UIA identity deduplicated'
$second = Editor ([System.Windows.Automation.ControlType]::Edit); $second.Identity = 2
$set = Get-ComposerCandidates $root @($edit, $second) $null $true
$refused = $false; try { Select-WorkBuddyComposer $set } catch { $refused = $true }
Assert $refused 'multiple editors refused rather than ranked'
$pattern = [System.Windows.Automation.TextPattern]::new()
$pattern.DocumentRange.ReadOnly = $false
Assert (Test-WorkBuddyWritablePattern $pattern) 'editable TextPattern accepted'
$pattern.DocumentRange.ReadOnly = [object]::new()
Assert (-not (Test-WorkBuddyWritablePattern $pattern)) 'unknown or mixed writability refused'
$focused = Editor ([System.Windows.Automation.ControlType]::Edit)
$focused.Current.HasKeyboardFocus = $true
[WorkBuddyUia]::Focused = $focused
Assert ($null -ne (Find-WorkBuddyFocusedComposer $root $null)) 'focused writable owned editor is accepted without a full tree scan'
$focused.Owned = $false
Assert ($null -eq (Find-WorkBuddyFocusedComposer $root $null)) 'focused foreign editor is refused'
$focused.Owned = $true; $focused.Pattern.Current.IsReadOnly = $true
Assert ($null -eq (Find-WorkBuddyFocusedComposer $root $null)) 'focused readonly editor is refused'
$focused.Pattern.Current.IsReadOnly = $false; $focused.Current.HasKeyboardFocus = $false
Assert ($null -eq (Find-WorkBuddyFocusedComposer $root $null)) 'stale focus is refused'
$focused.Current.HasKeyboardFocus = $true; $focused.Value = 'user changed draft'
$refused = $false; try { Find-WorkBuddyFocusedComposer $root @('original') } catch { $refused = $true }
Assert $refused 'focused editor cannot bypass draft change protection'
[WorkBuddyUia]::Focused = $null
Assert ($null -eq (Find-WorkBuddyFocusedComposer $root $null)) 'absent focus falls back to bounded semantic lookup'
# Run the real initialization and target-lookup orchestration with stubbed OS
# adapters. No P/Invoke is called, and no live desktop text is read or written.
function Write-ComposerProgress([string]$stage) { $script:Stages.Add($stage) }
function Get-MonotonicMilliseconds { $script:Clock += 25; return $script:Clock }
function Start-Sleep { param($Milliseconds) }
function Reset-ActivationFixture {
  $script:WorkBuddyAccessibilityRequests = @{}
  $script:Stages = [System.Collections.Generic.List[string]]::new()
  $script:Clock = 0
  [CodexVoiceNative]::Requests.Clear()
  [CodexVoiceNative]::Children = @([IntPtr]11, [IntPtr]999, [IntPtr]11)
  [CodexVoiceNative]::Foreground = $true
  [CodexVoiceNative]::ThrowRequest = $false
  [CodexVoiceNative]::Response = 3
  [CodexVoiceNative]::ActivatedEditor = $null
}
Reset-ActivationFixture
$info = Initialize-WorkBuddyAccessibility ([IntPtr]10) 20 5000
Assert ($info.Requested -eq 2 -and $info.Renderers -eq 1 -and $info.Msaa -eq 2) 'activation probes only owned unique windows and reports responses'
$again = Initialize-WorkBuddyAccessibility ([IntPtr]10) 20 5000
Assert ([CodexVoiceNative]::Requests.Count -eq 2 -and [object]::ReferenceEquals($info, $again)) 'activation runs once per recording and target'
Reset-ActivationFixture
[CodexVoiceNative]::Children = 11..20 | ForEach-Object { [IntPtr]$_ }
$info = Initialize-WorkBuddyAccessibility ([IntPtr]10) 20 5000
Assert ($info.Requested -eq 5 -and $info.Renderers -eq 4) 'activation caps renderer probes at four'
Reset-ActivationFixture
[CodexVoiceNative]::Foreground = $false
$info = Initialize-WorkBuddyAccessibility ([IntPtr]10) 20 5000
Assert ($info.Requested -eq 0) 'activation stops after foreground changes'
Reset-ActivationFixture
$info = Initialize-WorkBuddyAccessibility ([IntPtr]10) 20 0
Assert ($info.Requested -eq 0) 'activation respects lookup deadline'
Reset-ActivationFixture
[CodexVoiceNative]::ThrowRequest = $true
$info = Initialize-WorkBuddyAccessibility ([IntPtr]10) 20 5000
$again = Initialize-WorkBuddyAccessibility ([IntPtr]10) 20 5000
Assert ($info.Msaa -eq 0 -and [CodexVoiceNative]::Requests.Count -eq 2) 'rejected activation is not success and does not spin'
Reset-ActivationFixture
[CodexVoiceNative]::Response = 0
$info = Initialize-WorkBuddyAccessibility ([IntPtr]10) 20 5000
Assert ($info.Msaa -eq 0 -and $info.Detected -eq 0) 'lack of activation response is recorded without inventing success'

function Get-Process {
  [CmdletBinding()] param($Name)
  $process = [pscustomobject]@{ MainWindowHandle = [IntPtr]10; Id = 20 }
  $process | Add-Member ScriptMethod Refresh {}
  return $process
}
$root.Current | Add-Member NoteProperty NativeWindowHandle 10
[WorkBuddyUia]::Root = $root
Reset-ActivationFixture
$ready = Editor ([System.Windows.Automation.ControlType]::Edit)
$ready.Current.HasKeyboardFocus = $true
[CodexVoiceNative]::ActivatedEditor = $ready
[WorkBuddyUia]::Focused = $null
[WorkBuddyUia]::RootReads = 0
$target = Get-WorkBuddyTarget $null
Assert ($target.Composer.Element -eq $ready -and [WorkBuddyUia]::RootReads -ge 2) 'empty tree is initialized then re-read before binding the editor'
Assert ($script:Stages.Contains('enable_accessibility') -and $script:Stages.Contains('validate')) 'activation still passes normal validation before returning a target'
Reset-ActivationFixture
[WorkBuddyUia]::Focused = $ready
$target = Get-WorkBuddyTarget $null
Assert ([CodexVoiceNative]::Requests.Count -eq 0) 'healthy focused editor does not trigger activation'
Reset-ActivationFixture
[WorkBuddyUia]::Focused = $null
$failure = ''
try { Get-WorkBuddyTarget $null } catch { $failure = $_.Exception.Message }
Assert ($failure -match '输入框未就绪' -and $failure -match 'MSAA应答=2') 'activation response alone never authorizes writing into an absent editor'
Assert ([CodexVoiceNative]::Requests.Count -eq 2) 'unavailable tree exhausts bounded lookup without repeatedly activating'
Write-Output 'Embedded PowerShell parse, native C# compile and 30 mocked policy/orchestration cases passed. No native Windows UI tested.'
