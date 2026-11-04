// Portable source/ABI guards, separate from actual PowerShell mock execution.
import {readFileSync} from 'node:fs';
import {test} from 'node:test';
import assert from 'node:assert/strict';
const source=readFileSync(new URL('../src/codex_composer.rs',import.meta.url),'utf8');
const between=(start,end)=>{
  const at=source.indexOf(start);assert.ok(at>=0);
  const stop=source.indexOf(end,at+start.length);assert.ok(stop>at);
  return source.slice(at,stop);
};
test('shared reader prefers authoritative values and never clears matching prompt text',()=>{
  const read=between('function Get-ComposerText($element) {','function Get-NativeLegacyComposerSnapshot(');
  assert.ok(read.indexOf('ValuePattern]::Pattern')<read.indexOf('UIA_LegacyIAccessiblePatternId'));
  assert.ok(read.indexOf('UIA_LegacyIAccessiblePatternId')<read.indexOf('TextPattern]::Pattern'));
  for(const forbidden of ['Current.Name','HelpText','CurrentComposerAgent','TrimStart','GetText(-1)','ask anything','type / for commands']){
    assert.ok(!read.includes(forbidden),forbidden);
  }
  assert.ok(read.includes('Read-ComposerTextPattern $element $pattern'));
});
test('TextPattern requires editor ownership and fails closed on mixed/unknown ranges',()=>{
  const read=between('function Read-ComposerTextPattern(', 'function Get-ComposerText(');
  for(const evidence of ['GetEnclosingElement()', 'GetRuntimeId()', 'Assert-ComposerEditableRange $range',
    '$pattern.RangeFromChild($child)', 'ControlType]::Button.Id', 'GetText(65537)', '$children.Count -gt 64']){
    assert.ok(read.includes(evidence),evidence);
  }
  const gate=between('function Assert-ComposerEditableRange(', 'function Read-ComposerTextPattern(');
  assert.ok(gate.includes('IsReadOnlyAttribute')&&gate.includes('IsHiddenAttribute'));
  assert.ok(gate.includes('$readOnly -isnot [bool]')&&gate.includes('$hidden -isnot [bool]'));
  assert.ok(read.includes('-cne $text'));
  const candidates=between('function Get-ComposerCandidates(', 'function Find-Composer(');
  assert.ok(candidates.includes('Get-ComposerText $element'));
});
test('native COM interfaces preserve SDK vtable slots for Legacy value and child ranges',()=>{
  const methods=(name,end)=>[...between(name,end).matchAll(/^  (?:\w+\s+)+(\w+)\([^\n]*\);$/gm)].map(x=>x[1]);
  assert.deepEqual(methods('internal interface IWorkBuddyUiaLegacyPattern {','[ComImport, Guid("fb377fbe-'),[
    'Select','DoDefaultAction','SetValue','GetCurrentChildId','GetCurrentName',
    'GetCurrentValue','GetCurrentDescription','GetCurrentRole','GetCurrentState',
  ]);
  assert.deepEqual(methods('internal interface IWorkBuddyUiaTextRange {','[ComImport, Guid("14314595-'),[
    'Clone','Compare','CompareEndpoints','ExpandToEnclosingUnit','FindAttribute','FindText',
    'GetAttributeValue','GetBoundingRectangles','GetEnclosingElement','GetText','Move',
    'MoveEndpointByUnit','MoveEndpointByRange','Select','AddToSelection','RemoveFromSelection','ScrollIntoView','GetChildren',
  ]);
  assert.ok(source.includes('Guid("828055ad-355b-4435-86d5-3b51c14a9b1b")'));
  assert.ok(source.includes('patternId == 10018'));
});

test('Windows bridge never resolves the nonexistent managed LegacyIAccessiblePattern type',()=>{
  assert.ok(!source.includes('[System.Windows.Automation.LegacyIAccessiblePattern]'));
  const input=between('function Get-NativeComposerInputPattern(', 'function Get-NativeVoiceConversationAnchor(');
  assert.ok(input.includes('@(10002, 10018, 10014)'));
  const lookup=between('function Get-ComposerPattern(', 'function Test-ComposerEditor(');
  assert.ok(lookup.includes('[WorkBuddyUiaElement]'));
  assert.ok(lookup.includes('AutomationPattern]::LookupById($id)'));
});
