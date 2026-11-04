# Fixed Windows PowerShell 5.1, read-only non-secret profile metadata verifier.
# Paths enter only through a sanitized child environment; no content is read.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$WarningPreference = 'SilentlyContinue'
$VerbosePreference = 'SilentlyContinue'
$DebugPreference = 'SilentlyContinue'
$InformationPreference = 'SilentlyContinue'
$PSModuleAutoLoadingPreference = 'ModuleQualified'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
try {
    if ($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) { throw 'runtime' }
    $target = [Environment]::GetEnvironmentVariable('HACHIMODOCK_PROFILE_METADATA_PATH')
    $operation = [Environment]::GetEnvironmentVariable('HACHIMODOCK_PROFILE_METADATA_OPERATION')
    $nonce = [Environment]::GetEnvironmentVariable('HACHIMODOCK_PROFILE_METADATA_NONCE')
    if ($operation -notin @('verify-directory', 'verify-file') -or $nonce -notmatch '^[a-f0-9]{32}$') { throw 'input' }
    if ([String]::IsNullOrEmpty($target) -or $target.Length -gt 2048 -or $target -notmatch '^[A-Za-z]:\\' -or
        $target -match '[\x00-\x1f\x7f]' -or $target.Contains('/') -or $target.Substring(2).Contains(':')) { throw 'path' }
    if ($target.Length -gt 3) {
        foreach ($part in $target.Substring(3).Split('\')) {
            if ([String]::IsNullOrEmpty($part) -or $part -in @('.', '..') -or $part -match '[. ]$' -or
                $part -match '[<>"|?*]' -or $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3]|CONIN\$|CONOUT\$)(?:\.|$)') { throw 'path' }
        }
    }
    if (-not [String]::Equals([IO.Path]::GetFullPath($target), $target, [StringComparison]::OrdinalIgnoreCase)) { throw 'path' }
    if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($target)).DriveType -ne [IO.DriveType]::Fixed) { throw 'volume' }
    $current = [Security.Principal.WindowsIdentity]::GetCurrent().User
    if ($null -eq $current -or $current.Value -in @('S-1-5-18', 'S-1-5-32-544')) { throw 'identity' }
    $trusted = @($current.Value, 'S-1-5-18', 'S-1-5-32-544')
    $readOnlyMask = [Convert]::ToInt64('A01200A9', 16)

    function Read-AclSnapshot([String] $name) {
        $entry = Microsoft.PowerShell.Management\Get-Item -LiteralPath $name -Force -ErrorAction Stop
        if ($entry.PSProvider.Name -ne 'FileSystem' -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            -not [String]::Equals($entry.FullName, $name, [StringComparison]::OrdinalIgnoreCase)) { throw 'entry' }
        $acl = Microsoft.PowerShell.Security\Get-Acl -LiteralPath $name -ErrorAction Stop
        $sddl = $acl.GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]::Access -bor [Security.AccessControl.AccessControlSections]::Owner)
        $raw = [Security.AccessControl.RawSecurityDescriptor]::new($sddl)
        if ($null -eq $raw.DiscretionaryAcl -or ($raw.ControlFlags -band [Security.AccessControl.ControlFlags]::DiscretionaryAclPresent) -eq 0 -or
            $raw.DiscretionaryAcl.Count -gt 128) { throw 'dacl' }
        $rules = @()
        foreach ($ace in $raw.DiscretionaryAcl) {
            if ($ace -isnot [Security.AccessControl.CommonAce] -or $ace.IsCallback -or [Int32]$ace.AceType -notin @(0, 1) -or
                ([Int32]$ace.AceFlags -band (-bnot 31)) -ne 0) { throw 'ace' }
            $rules += @{ sid = $ace.SecurityIdentifier.Value; type = [Int32]$ace.AceType;
                mask = ([Int64]$ace.AccessMask -band [Int64]4294967295); flags = [Int32]$ace.AceFlags }
        }
        return @{ entry = $entry; signature = $sddl; snapshot = @{
            directory = [Bool]$entry.PSIsContainer; reparse = $false;
            ownerSid = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value;
            daclPresent = $true; protected = [Bool]$acl.AreAccessRulesProtected;
            aceCount = $raw.DiscretionaryAcl.Count; aces = @($rules) } }
    }
    function Read-Chain {
        $chain = @(); $cursor = $target
        while ($cursor) {
            if ($chain.Count -ge 64) { throw 'depth' }
            $entry = Microsoft.PowerShell.Management\Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
            if ($entry.PSProvider.Name -ne 'FileSystem' -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
                -not [String]::Equals($entry.FullName, $cursor, [StringComparison]::OrdinalIgnoreCase) -or
                ($chain.Count -gt 0 -and -not $entry.PSIsContainer)) { throw 'ancestor' }
            $chain += @{ creation = $entry.CreationTimeUtc.Ticks; written = $entry.LastWriteTimeUtc.Ticks;
                attributes = [Int32]$entry.Attributes; directory = [Bool]$entry.PSIsContainer }
            if ([String]::Equals($cursor, [IO.Path]::GetPathRoot($cursor), [StringComparison]::OrdinalIgnoreCase)) { break }
            $cursor = [IO.Path]::GetDirectoryName($cursor)
        }
        return ,$chain
    }
    $chainBefore = Read-Chain
    $before = Read-AclSnapshot $target
    if ($before.snapshot.ownerSid -ne $current.Value -or $before.snapshot.directory -ne ($operation -eq 'verify-directory')) { throw 'owner-kind' }
    foreach ($ace in $before.snapshot.aces) {
        if ($ace.type -eq 0 -and $ace.sid -notin $trusted -and ($ace.mask -band (-bnot $readOnlyMask)) -ne 0) { throw 'foreign-write' }
    }
    $after = Read-AclSnapshot $target
    $chainAfter = Read-Chain
    if ($after.snapshot.ownerSid -ne $current.Value -or $before.signature -cne $after.signature -or
        $after.snapshot.directory -ne ($operation -eq 'verify-directory') -or $chainAfter.Count -ne $chainBefore.Count) { throw 'changed' }
    for ($i = 0; $i -lt $chainBefore.Count; $i++) {
        if ($chainBefore[$i].creation -ne $chainAfter[$i].creation -or $chainBefore[$i].written -ne $chainAfter[$i].written -or
            $chainBefore[$i].attributes -ne $chainAfter[$i].attributes -or $chainBefore[$i].directory -ne $chainAfter[$i].directory) { throw 'changed' }
    }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($operation + [Char]0 + $target + [Char]0 + $nonce)
        $binding = ([BitConverter]::ToString($hasher.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally { $hasher.Dispose() }
    # Strict metadata schema: no path, content, token or localized account name.
    $receipt = @{ schema = 'dsh-profile-metadata-acl-v1'; operation = $operation; binding = $binding;
        currentSid = $current.Value; ancestorCount = $chainAfter.Count; ancestorsNoReparse = $true;
        item = $after.snapshot }
    [Console]::Out.WriteLine(($receipt | Microsoft.PowerShell.Utility\ConvertTo-Json -Depth 6 -Compress))
    exit 0
} catch {
    [Console]::Error.WriteLine('DSH_PROFILE_METADATA_ACL_REJECTED')
    exit 1
}
