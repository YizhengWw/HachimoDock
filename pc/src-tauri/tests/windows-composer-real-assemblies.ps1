# Offline regression: actual Windows PowerShell UIA types, fake provider only.
# No window handles, desktop enumeration, input, clipboard or network calls.
param([string]$Source = (Join-Path $PSScriptRoot '../src/codex_composer.rs'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -ReferencedAssemblies UIAutomationClient,UIAutomationTypes -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Windows.Automation;
public class FixtureCurrent {
  public bool IsEnabled=true, IsOffscreen=false, IsKeyboardFocusable=true;
  public ControlType ControlType=ControlType.Edit;
  public string ClassName="";
  public bool IsReadOnly=false;
  public uint Role=42, State=0;
}
public class WorkBuddyUiaValuePattern { public FixtureCurrent Current=new FixtureCurrent(); }
public class WorkBuddyUiaLegacyPattern { public FixtureCurrent Current=new FixtureCurrent(); }
public class FixtureRange {
  public object ReadOnly=false;
  public object GetAttributeValue(object id) { return ReadOnly; }
}
public class WorkBuddyUiaTextPattern { public FixtureRange DocumentRange=new FixtureRange(); }
public class WorkBuddyUiaElement {
  public FixtureCurrent Current=new FixtureCurrent();
  public Dictionary<int,object> Patterns=new Dictionary<int,object>();
  public List<int> Requests=new List<int>();
  public bool Password=false, LegacyFlags=false;
  public object ValueAvailable=false, ValueReadOnly=null;
  public bool HasWritableLegacyText() { return LegacyFlags; }
  public object GetCurrentPropertyValue(object id) {
    switch((int)id) {
      case 30019:return Password;
      case 30043:return ValueAvailable;
      case 30046:return ValueReadOnly;
      case 30095:return 42;
      case 30096:return 0;
      default:return null;
    }
  }
  public bool TryGetCurrentPattern(object id,out object result) {
    int key=(int)id;Requests.Add(key);return Patterns.TryGetValue(key,out result);
  }
}
public class FixtureManagedElement {
  public int Requests=0;
  public bool TryGetCurrentPattern(object id,out object result) { Requests++;result=null;return false; }
}
'@
$sourceText=[IO.File]::ReadAllText((Resolve-Path $Source))
$scriptText=[regex]::Match($sourceText,'(?s)const SCRIPT: &str = r#"(.*?)"#;').Groups[1].Value
$tokens=$null;$errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseInput($scriptText,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Production script parse failure'}
$names=@('Get-ComposerProperty','Get-ComposerPattern','Test-ComposerEditor',
  'Get-NativeLegacyComposerSnapshot','Get-NativeComposerInputPattern','Test-WorkBuddyWritablePattern')
foreach($function in $ast.FindAll({param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst]},$true)) {
  if($names -contains $function.Name){. ([scriptblock]::Create($function.Extent.Text))}
}
$script:passed=0
function Check($condition,[string]$label){if(-not $condition){throw "FAIL: $label"};$script:passed++;Write-Output "PASS: $label"}
function Reject($element,[string]$label){
  $failed=$false;try{[void](Get-NativeComposerInputPattern $element)}catch{$failed=$true}
  Check $failed $label
}
# Real Windows PowerShell has no managed LegacyIAccessiblePattern. Do not add
# a fake version: that made the previous synthetic suite hide this regression.
foreach($kind in @(10002,10018,10014)) {
  $e=[WorkBuddyUiaElement]::new()
  $p=switch($kind){10002{[WorkBuddyUiaValuePattern]::new()}10018{[WorkBuddyUiaLegacyPattern]::new()}10014{[WorkBuddyUiaTextPattern]::new()}}
  $e.Patterns.Add($kind,$p)
  $result=Get-NativeComposerInputPattern $e
  Check ([object]::ReferenceEquals($result,$p)) "native pattern $kind accepted with real system assemblies"
  if($kind -eq 10014){$p.DocumentRange.ReadOnly=$true}else{
    if($kind -eq 10018){$p.Current.State=64}else{$p.Current.IsReadOnly=$true}
  }
  Reject $e "read-only pattern $kind refused"
}
$e=[WorkBuddyUiaElement]::new();$e.ValueAvailable=$true;$e.ValueReadOnly=$false
Check ($null -ne (Get-NativeComposerInputPattern $e)) 'typed writable Value property fallback'
$e.ValueReadOnly='unknown';Reject $e 'unknown read-only property refused'
$e=[WorkBuddyUiaElement]::new();Reject $e 'no writable capability refused'
$e.LegacyFlags=$true
Check ($null -ne (Get-NativeComposerInputPattern $e)) 'native legacy flags fallback remains usable'
$e.Password=$true;Reject $e 'password editor refused'
$e=[WorkBuddyUiaElement]::new();$e.Current.IsOffscreen=$true;Reject $e 'offscreen editor refused'
$e=[WorkBuddyUiaElement]::new();$e.Current.IsEnabled=$false;Reject $e 'disabled editor refused'
$managed=[FixtureManagedElement]::new()
[void](Get-ComposerPattern $managed 10018)
Check ($managed.Requests -eq 0) 'unregistered managed legacy identifier never queried'
[void](Get-ComposerPattern $managed 10002)
Check ($managed.Requests -eq 1) 'registered managed Value identifier still queried'
Write-Output "REAL_ASSEMBLY_COMPOSER_PASS count=$script:passed desktopCalls=0"
