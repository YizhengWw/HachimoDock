# Fixed Windows PowerShell 5.1 program. Inputs are paths/operation only, never tokens.
# No profile, shell interpolation, localized account names, or descriptor content.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$WarningPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
try {
    $target = [Environment]::GetEnvironmentVariable('HACHIMODOCK_DSH_ACL_PATH')
    $operation = [Environment]::GetEnvironmentVariable('HACHIMODOCK_DSH_ACL_OPERATION')
    if ($operation -notin @('prepare-directory', 'protect-file', 'verify-directory', 'verify-file')) { throw 'operation' }
    if ([String]::IsNullOrEmpty($target) -or $target -notmatch '^[A-Za-z]:\\' -or $target -match '[\x00-\x1f]' -or $target.Substring(2).Contains(':')) { throw 'path' }
    foreach ($part in $target.Substring(3).Split('\')) {
        if ($part -eq '.' -or $part -eq '..' -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part.Contains('/')) { throw 'path' }
    }
    $target = [IO.Path]::GetFullPath($target)
    if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($target)).DriveType -ne [IO.DriveType]::Fixed) { throw 'local fixed volume required' }
    $current = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    if ($null -eq $current -or $current.Value -eq $system.Value) { throw 'user' }

    function Assert-NoReparse([String] $name) {
        $cursor = $name
        while ($cursor) {
            $entry = $null
            try { $entry = Microsoft.PowerShell.Management\Get-Item -LiteralPath $cursor -Force -ErrorAction Stop }
            catch [Management.Automation.ItemNotFoundException] { $entry = $null }
            if ($null -ne $entry) {
                if ($entry.PSProvider.Name -ne 'FileSystem' -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'reparse' }
            }
            $parent = [IO.Path]::GetDirectoryName($cursor)
            if ($parent -eq $cursor) { break }
            $cursor = $parent
        }
    }
    function Read-Snapshot([String] $name, [Bool] $directory) {
        Assert-NoReparse $name
        $attrs = [IO.File]::GetAttributes($name)
        $isDirectory = ($attrs -band [IO.FileAttributes]::Directory) -ne 0
        if ($isDirectory -ne $directory) { throw 'kind' }
        $acl = Microsoft.PowerShell.Security\Get-Acl -LiteralPath $name
        $rules = @($acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) | ForEach-Object {
            @{ sid = $_.IdentityReference.Value; type = [Int32] $_.AccessControlType;
               inherited = [Bool] $_.IsInherited; rights = [Int64] $_.FileSystemRights;
               inheritance = [Int32] $_.InheritanceFlags; propagation = [Int32] $_.PropagationFlags }
        })
        $rawDacl = [Security.AccessControl.RawSecurityDescriptor]::new($acl.GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]::Access))
        return @{ path = $name; directory = $directory; reparse = $false; rawAceCount = $rawDacl.DiscretionaryAcl.Count;
            owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value;
            protected = [Bool] $acl.AreAccessRulesProtected; rules = $rules }
    }
    function Assert-Private($snapshot) {
        if ($snapshot.owner -ne $current.Value -or -not $snapshot.protected -or $snapshot.rules.Count -ne 2 -or $snapshot.rawAceCount -ne 2) { throw 'acl' }
        $seen = @{}
        foreach ($rule in $snapshot.rules) {
            if ($rule.sid -notin @($current.Value, $system.Value) -or $rule.type -ne 0 -or $rule.inherited -or
                $rule.rights -ne [Int64][Security.AccessControl.FileSystemRights]::FullControl -or $rule.propagation -ne 0 -or
                $rule.inheritance -ne $(if ($snapshot.directory) { 3 } else { 0 }) -or $seen.ContainsKey($rule.sid)) { throw 'acl' }
            $seen[$rule.sid] = $true
        }
        if (-not $seen.ContainsKey($current.Value) -or -not $seen.ContainsKey($system.Value)) { throw 'acl' }
    }
    function Protect-Entry([String] $name, [Bool] $directory) {
        $old = Read-Snapshot $name $directory
        # Never repair a foreign owner's entry, nor inspect any old token bytes.
        if ($old.owner -ne $current.Value) { throw 'owner' }
        $acl = if ($directory) { [Security.AccessControl.DirectorySecurity]::new() } else { [Security.AccessControl.FileSecurity]::new() }
        $acl.SetOwner($current)
        $acl.SetAccessRuleProtection($true, $false)
        $inherit = if ($directory) { [Security.AccessControl.InheritanceFlags] 3 } else { [Security.AccessControl.InheritanceFlags] 0 }
        foreach ($sid in @($current, $system)) {
            $rule = [Security.AccessControl.FileSystemAccessRule]::new($sid,
                [Security.AccessControl.FileSystemRights]::FullControl, $inherit,
                [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow)
            [Void] $acl.AddAccessRule($rule)
        }
        Microsoft.PowerShell.Security\Set-Acl -LiteralPath $name -AclObject $acl
        $verified = Read-Snapshot $name $directory
        Assert-Private $verified
        return $verified
    }
    Assert-NoReparse $target
    if ($operation -eq 'prepare-directory') {
        # Directory may inherit permissions briefly; it is empty of new secrets.
        # No token generation/file creation is allowed until this succeeds.
        [Void] [IO.Directory]::CreateDirectory($target)
        $directory = Protect-Entry $target $true
        $item = $null
    } elseif ($operation -eq 'verify-directory') {
        $directory = Read-Snapshot $target $true
        Assert-Private $directory
        $item = $null
    } else {
        $directory = Read-Snapshot ([IO.Path]::GetDirectoryName($target)) $true
        Assert-Private $directory
        $item = if ($operation -eq 'protect-file') { Protect-Entry $target $false } else { Read-Snapshot $target $false }
        Assert-Private $item
    }
    # Metadata receipt only. Descriptor bytes and credentials are never read.
    @{ version = 1; operation = $operation; currentSid = $current.Value; directory = $directory; item = $item } |
        Microsoft.PowerShell.Utility\ConvertTo-Json -Depth 6 -Compress | Microsoft.PowerShell.Utility\Write-Output
    exit 0
} catch {
    # PowerShell exception messages can contain paths; suppress all such output.
    [Console]::Error.WriteLine('DSH_ACL_REJECTED')
    exit 1
}
