# Offline P4 client checks. No configuration/credentials/audio payload capture.
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$ResourceRoot,
  [string]$ClientExe,
  [string]$OutputDirectory,
  [switch]$ProbeCom,
  [string]$Port,
  [ValidateSet(115200,4000000)][int]$BaudRate=4000000,
  [ValidateRange(1,10)][int]$ListenSeconds=3
)
$ErrorActionPreference='Stop'
if (-not $OutputDirectory) { $OutputDirectory=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('HachimiDockAcceptance\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')+'-'+[Guid]::NewGuid().ToString('N')) }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory already exists; preserve old evidence and choose a NEW directory' }
$null=New-Item -ItemType Directory -Path $OutputDirectory
$report=[ordered]@{
 schema=1;status='RUNNING';softwareChecks='NOT_RUN';comProbe='NOT_RUN';
 p4NativeAsrRequiresLegacyService=$false;legacyServiceChecked=$false;
 serviceStarted=$false;deviceCommandsSent=0;audioCaptured=$false;credentialsRead=$false;
 windowsP4VoiceAcceptance='NOT_RUN';windowsGuiAcceptance='NOT_RUN';
 v3HardwareAcceptance='NOT_RUN';errors=@();checks=@()
}
$utf8=New-Object System.Text.UTF8Encoding($false)
function Save-Receipt {
 $checkpoint=@{timeUtc=[DateTime]::UtcNow.ToString('o');status=$report.status;softwareChecks=$report.softwareChecks;comProbe=$report.comProbe;errors=$report.errors.Count}|ConvertTo-Json -Compress
 [System.IO.File]::AppendAllText((Join-Path $OutputDirectory 'checkpoints.jsonl'),$checkpoint+"`n",$utf8)
 $text=$report|ConvertTo-Json -Depth 10
 [System.IO.File]::WriteAllText((Join-Path $OutputDirectory 'report.json'),$text,$utf8)
}
function Evidence-Error([string]$stage,$errorRecord) {
 # Keep stage/type only, never exception text containing arbitrary serial data.
 $report.errors+=@{stage=$stage;exceptionType=$errorRecord.Exception.GetType().FullName}
}
function Check-X64Pe([string]$path) {
 if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Required executable missing' }
 $file=[IO.File]::OpenRead($path)
 try {
  $reader=[IO.BinaryReader]::new($file)
  if ($file.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Invalid PE DOS header' }
  $file.Position=0x3c;$offset=$reader.ReadUInt32()
  if ($offset+6 -gt $file.Length) { throw 'Invalid PE offset' }
  $file.Position=$offset
  if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Expected x64 PE' }
 } finally { $file.Dispose() }
 return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
Save-Receipt
try {
 if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) { throw 'An actual Windows x64 machine is required' }
 $node=Join-Path $ResourceRoot 'bridge/runtime/node.exe'
 $ffmpeg=Join-Path $ResourceRoot 'tools/ffmpeg.exe'
 $nodeHash=Check-X64Pe $node
 if ($nodeHash -ne 'e921fe5307e29bf6fd00000dd594356affd3a7b044e52720c7f10decbdc305b9') { throw 'Node executable differs from reviewed offline Windows runtime' }
 $nodeVersion=(& $node --version 2>$null|Out-String).Trim()
 if ($LASTEXITCODE -ne 0 -or $nodeVersion -notmatch '^v\d+\.\d+\.\d+$') { throw 'Bundled Node execution failed' }
 $ffmpegHash=Check-X64Pe $ffmpeg
 if ($ffmpegHash -ne '1cefca14ea713d5d7e7a1b948299142831185487f5efb14914d049ba9090d95f') { throw 'Unreviewed FFmpeg executable' }
 $version=(& $ffmpeg -hide_banner -version 2>$null|Out-String)
 if ($LASTEXITCODE -ne 0 -or $version -notmatch '--disable-gpl' -or $version -notmatch '--disable-nonfree' -or $version -match '--enable-gpl|--enable-nonfree') { throw 'FFmpeg configuration mismatch' }
 $encoders=(& $ffmpeg -hide_banner -encoders 2>$null|Out-String)
 if ($LASTEXITCODE -ne 0 -or $encoders -notmatch '\bh264_mf\b') { throw 'Required Windows encoder missing' }
 $report.checks+=@{kind='bundledNode';peX64=$true;version=$nodeVersion;sha256=$nodeHash}
 $report.checks+=@{kind='bundledFfmpeg';peX64=$true;sha256=$ffmpegHash;lgplConfiguration=$true;h264MfDeclared=$true;encodingExecuted=$false}
 if ($ClientExe) { $report.checks+=@{kind='clientExecutable';peX64=$true;sha256=(Check-X64Pe $ClientExe);guiStarted=$false} }
 $webview=$false
 foreach($base in @([Environment]::GetEnvironmentVariable('ProgramFiles(x86)'),$env:ProgramFiles,$env:LOCALAPPDATA)) {
  if(-not $base){continue};$path=Join-Path $base 'Microsoft\EdgeWebView\Application'
  if(Test-Path -LiteralPath $path){foreach($versionDir in Get-ChildItem -LiteralPath $path -Directory){if(Test-Path -LiteralPath (Join-Path $versionDir.FullName 'msedgewebview2.exe')){$webview=$true}}}
 }
 $report.checks+=@{kind='webview2';installationDetected=$webview;guiStarted=$false}
 if(-not $webview){throw 'WebView2 not detected; retain evidence, do not install automatically'}
 $report.comPorts=@([IO.Ports.SerialPort]::GetPortNames()|Sort-Object)
 $report.softwareChecks='PASSED_CHECKED_SCOPE_ONLY'
 Save-Receipt
 if ($ProbeCom) {
  if ($Port -notmatch '^COM[1-9][0-9]*$' -or $Port -notin $report.comPorts) { throw 'Choose an enumerated COM port explicitly; never guess or retry another port' }
  $serial=$null;$probe=[ordered]@{port=$Port;baud=$BaudRate;opened=$false;closed=$null;disposed=$false;receivedBytes=0;rawSaved=$false;sentBytes=0;deviceIdentityVerified=$false;attempts=1}
  $report.comProbe='RUNNING';$report.com=$probe;Save-Receipt
  try {
   # One exclusive passive open only. Close the GUI/other readers beforehand.
   $serial=[IO.Ports.SerialPort]::new($Port,$BaudRate,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
   $serial.DtrEnable=$false;$serial.RtsEnable=$false;$serial.Handshake=[IO.Ports.Handshake]::None
   $serial.ReadTimeout=200;$serial.WriteTimeout=200;$serial.Open();$probe.opened=$true
   $clock=[Diagnostics.Stopwatch]::StartNew();$bytes=New-Object byte[] 4096
   while($clock.Elapsed.TotalSeconds -lt $ListenSeconds){
    try{$count=$serial.Read($bytes,0,$bytes.Length);$probe.receivedBytes+=$count}
    catch [TimeoutException] { }
   }
  } catch { Evidence-Error 'com-open-or-read' $_; $report.comProbe='FAILED' }
  finally {
   if($null -ne $serial){
    try{$serial.Close();$probe.closed=(-not $serial.IsOpen)}catch{Evidence-Error 'com-close' $_;$report.comProbe='FAILED'}
    try{$serial.Dispose();$probe.disposed=$true}catch{Evidence-Error 'com-dispose' $_;$report.comProbe='FAILED'}
   }
   if($report.comProbe -ne 'FAILED' -and $probe.opened -and $probe.closed -eq $true -and $probe.disposed){$report.comProbe='PASSIVE_OPEN_READ_CLOSE_ONLY'}
   elseif($report.comProbe -ne 'FAILED'){$report.comProbe='FAILED'}
   Save-Receipt
  }
 }
 $report.status=if($report.errors.Count){'FAILED'}else{'COMPLETED_LIMITED_SCOPE'}
} catch { Evidence-Error 'software-or-arguments' $_;$report.status='FAILED';if($report.softwareChecks -eq 'NOT_RUN'){$report.softwareChecks='FAILED'} }
finally { Save-Receipt }
Write-Output ('Receipt: '+(Join-Path $OutputDirectory 'report.json'))
if($report.status -eq 'FAILED'){exit 1}
