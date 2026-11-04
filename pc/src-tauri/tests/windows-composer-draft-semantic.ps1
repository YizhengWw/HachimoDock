# Synthetic UIA contract tests. A fresh PowerShell process loads ONLY fake UIA
# types, then executes the actual production reader extracted from Rust. No
# UIAutomation assemblies, windows, desktop input, application or network calls.
param([string]$Source = (Join-Path $PSScriptRoot '../src/codex_composer.rs'))
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
namespace System.Windows.Automation {
  public class ControlType {
    public int Id;
    public ControlType(int id) { Id=id; }
    public static ControlType Edit=new ControlType(50004), Document=new ControlType(50030),
      Group=new ControlType(50026), Custom=new ControlType(50025), Button=new ControlType(50000);
  }
  public static class AutomationProperty { public static object LookupById(int id) { return id; } }
  public static class ValuePattern { public static int Pattern=10002; }
  public static class AutomationPattern { public static object LookupById(int id) { return id; } }
  public static class TextPattern {
    public static int Pattern=10014, IsReadOnlyAttribute=40015, IsHiddenAttribute=40013;
  }
}
public class WorkBuddyUiaElement {}
public class FakeCurrent {
  public bool IsEnabled=true, IsOffscreen=false, IsKeyboardFocusable=true;
  public System.Windows.Automation.ControlType ControlType=System.Windows.Automation.ControlType.Edit;
  public string ClassName="";
  public string Name { get { throw new System.Exception("Name is not a draft"); } }
  public string HelpText { get { throw new System.Exception("Help is not a draft"); } }
}
public class FakeValue {
  public FakeValue Current { get { return this; } }
  public object Value="", IsReadOnly=false;
  public int Role=42;
  public object State=0;
}
public class FakeRange {
  public string Text="", NextText=null;
  public object ReadOnly=false, Hidden=false;
  public FakeElement Enclosing;
  public FakeElement[] Children=new FakeElement[0];
  private int reads=0;
  public string GetText(int limit) { string s=(reads++>0 && NextText!=null)?NextText:Text; return s.Substring(0,System.Math.Min(s.Length,limit)); }
  public object GetAttributeValue(object id) { return (int)id==40015?ReadOnly:Hidden; }
  public FakeElement GetEnclosingElement() { return Enclosing; }
  public FakeElement[] GetChildren() { return Children; }
}
public class FakeText {
  public FakeRange DocumentRange;
  public FakeRange RangeFromChild(FakeElement child) { return child.Range; }
}
public class FakeElement {
  public FakeCurrent Current=new FakeCurrent();
  public int Id=1;
  public string Role="";
  public FakeValue Value=null, Legacy=null;
  public FakeText Text=null;
  public FakeRange Range=null;
  public int[] GetRuntimeId() { return new int[]{Id}; }
  public object GetCurrentPropertyValue(object id) { return Role; }
  public bool TryGetCurrentPattern(object id, out object pattern) {
    pattern=(int)id==10002?(object)Value:((int)id==10018?(object)Legacy:Text);
    return pattern!=null;
  }
}
'@
$sourceText = [IO.File]::ReadAllText((Resolve-Path $Source))
$start = $sourceText.IndexOf('function Normalize-ComposerText(')
$end = $sourceText.IndexOf('function Test-AllowedComposerValue(', $start)
if ($start -lt 0 -or $end -lt $start) { throw 'Production reader anchors changed' }
. ([scriptblock]::Create($sourceText.Substring($start, $end-$start)))
$script:passed=0
function Check-Value($element, [string]$expected) {
  $result = @(Get-ComposerText $element)
  if ($result.Count -ne 2 -or [string]$result[1] -cne $expected) { throw 'Synthetic reader value mismatch' }
  $script:passed++
}
function Check-Rejected($element) {
  $rejected=$false
  try { [void](Get-ComposerText $element) } catch { $rejected=$true }
  if (-not $rejected) { throw 'Ambiguous synthetic composer was accepted' }
  $script:passed++
}
function Editor-Value([string]$text) {
  $e=[FakeElement]::new();$e.Value=[FakeValue]::new();$e.Value.Value=$text;return $e
}
function Editor-Text([string]$text) {
  $e=[FakeElement]::new();$e.Text=[FakeText]::new();$e.Text.DocumentRange=[FakeRange]::new()
  $e.Text.DocumentRange.Text=$text;$e.Text.DocumentRange.Enclosing=$e;return $e
}

# The actual empty value wins over arbitrary multiline display decoration.
$e=Editor-Value ''; $e.Text=(Editor-Text "unfamiliar hint`nshortcut button").Text
Check-Value $e ''
# These are genuine user values, not prompt deny-list entries; preserve exactly.
foreach ($text in @('ask anything', 'type / for commands', '输入消息', 'arbitrary future hint', "  draft`n  indent", ([string][char]0xFEFF+'draft'))) {
  Check-Value (Editor-Value $text) $text
}
$e=Editor-Value 'actual';$e.Value.IsReadOnly=$true;Check-Rejected $e
$e=Editor-Value 'actual';$e.Value.IsReadOnly='NotSupported';Check-Rejected $e
$e=Editor-Value 'actual';$e.Value.Value=$null;Check-Rejected $e
# Legacy authority is specifically MSAA text-editor Value, never Name/Help.
$e=[FakeElement]::new();$e.Legacy=[FakeValue]::new();Check-Value $e ''
$e.Legacy.Value='ask anything';Check-Value $e 'ask anything'
$e.Legacy.State=64;Check-Rejected $e
$e.Legacy.State=$null;Check-Rejected $e
$e.Legacy.State=0;$e.Legacy.Role=15;Check-Rejected $e
Check-Value (Editor-Text 'ask anything') 'ask anything'
Check-Value (Editor-Text '') ''
$e=Editor-Text 'hint plus actual draft';$e.Text.DocumentRange.ReadOnly='Mixed';Check-Rejected $e
$e=Editor-Text 'hint';$e.Text.DocumentRange.ReadOnly=$true;Check-Rejected $e
$e=Editor-Text 'hidden';$e.Text.DocumentRange.Hidden=$true;Check-Rejected $e
$e=Editor-Text 'unknown';$e.Text.DocumentRange.Hidden=$null;Check-Rejected $e
$e=Editor-Text 'draft';$e.Text.DocumentRange.Enclosing=[FakeElement]::new();$e.Text.DocumentRange.Enclosing.Id=9;Check-Rejected $e
$e=Editor-Text 'whole conversation';$e.Current.ControlType=[System.Windows.Automation.ControlType]::Document;Check-Rejected $e
$e=Editor-Text 'draft';$e.Current.ControlType=[System.Windows.Automation.ControlType]::Document;$e.Role='textbox';Check-Value $e 'draft'
$e=Editor-Text "hint`ndraft";$child=[FakeElement]::new();$child.Range=[FakeRange]::new();$child.Range.ReadOnly=$true
$e.Text.DocumentRange.Children=@($child);Check-Rejected $e
$e=Editor-Text "draft`nshortcut";$child=[FakeElement]::new();$child.Current.ControlType=[System.Windows.Automation.ControlType]::Button
$e.Text.DocumentRange.Children=@($child);Check-Rejected $e
$e=Editor-Text 'before';$e.Text.DocumentRange.NextText='after';Check-Rejected $e
Check-Rejected (Editor-Text ('x'*65537))
[Console]::Out.WriteLine("SYNTHETIC_UIA_READER_PASS count=$script:passed; desktopCalls=0; realWindowsUIA=NOT_RUN")
