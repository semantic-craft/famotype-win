[CmdletBinding()]
param(
  [switch] $Json
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

if (-not ('FamoHealth.NativeFileIdentity' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace FamoHealth
{
    public sealed class FileIdentity
    {
        public string FinalPath { get; set; }
        public string ObjectId { get; set; }
        public uint LinkCount { get; set; }
    }

    public static class NativeFileIdentity
    {
        private const uint FileReadAttributes = 0x80;
        private const uint FileShareRead = 0x1;
        private const uint FileShareWrite = 0x2;
        private const uint FileShareDelete = 0x4;
        private const uint OpenExisting = 3;
        private const uint FileFlagBackupSemantics = 0x02000000;

        [StructLayout(LayoutKind.Sequential)]
        private struct ByHandleFileInformation
        {
            public uint FileAttributes;
            public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
            public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
            public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
            public uint VolumeSerialNumber;
            public uint FileSizeHigh;
            public uint FileSizeLow;
            public uint NumberOfLinks;
            public uint FileIndexHigh;
            public uint FileIndexLow;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            IntPtr securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFinalPathNameByHandleW(
            SafeFileHandle file,
            StringBuilder path,
            uint pathLength,
            uint flags);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetFileInformationByHandle(
            SafeFileHandle file,
            out ByHandleFileInformation information);

        public static FileIdentity Get(string path)
        {
            using (SafeFileHandle handle = CreateFileW(
                path,
                FileReadAttributes,
                FileShareRead | FileShareWrite | FileShareDelete,
                IntPtr.Zero,
                OpenExisting,
                FileFlagBackupSemantics,
                IntPtr.Zero))
            {
                if (handle.IsInvalid)
                    throw new Win32Exception(Marshal.GetLastWin32Error());

                StringBuilder finalPath = new StringBuilder(32768);
                uint length = GetFinalPathNameByHandleW(
                    handle, finalPath, (uint)finalPath.Capacity, 0);
                if (length == 0 || length >= finalPath.Capacity)
                    throw new Win32Exception(Marshal.GetLastWin32Error());

                ByHandleFileInformation information;
                if (!GetFileInformationByHandle(handle, out information))
                    throw new Win32Exception(Marshal.GetLastWin32Error());

                string normalized = finalPath.ToString();
                if (normalized.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase))
                    normalized = @"\\" + normalized.Substring(8);
                else if (normalized.StartsWith(@"\\?\", StringComparison.OrdinalIgnoreCase))
                    normalized = normalized.Substring(4);

                string objectId =
                    information.FileIndexHigh == 0 && information.FileIndexLow == 0
                    ? String.Empty
                    : String.Format(
                        System.Globalization.CultureInfo.InvariantCulture,
                        "{0}:{1}:{2}",
                        information.VolumeSerialNumber,
                        information.FileIndexHigh,
                        information.FileIndexLow);
                return new FileIdentity
                {
                    FinalPath = normalized,
                    ObjectId = objectId,
                    LinkCount = information.NumberOfLinks
                };
            }
        }
    }
}
'@
}

$brandKey = 'HKLM:\Software\Famo\InputMethod'
$machineComKey = 'HKLM:\Software\Classes\CLSID\{54EAD76A-B864-4A6D-9C82-148E3352BEE7}\InprocServer32'
$userComKey = 'HKCU:\Software\Classes\CLSID\{54EAD76A-B864-4A6D-9C82-148E3352BEE7}\InprocServer32'
$userTipKey = 'HKCU:\Software\Microsoft\CTF\TIP\{54EAD76A-B864-4A6D-9C82-148E3352BEE7}'
$tipKey = 'HKLM:\Software\Microsoft\CTF\TIP\{54EAD76A-B864-4A6D-9C82-148E3352BEE7}'
$runKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run'
$probeMode = 'ReadOnly'
$userWeaselDataPolicy = 'NoWrite:%AppData%\Rime'
$results = New-Object System.Collections.Generic.List[object]

function Add-Check {
  param([string] $Id, [string] $Severity, [bool] $Pass, [string] $Detail)
  $results.Add([pscustomobject]@{
    id = $Id
    severity = $Severity
    pass = $Pass
    detail = $Detail
    probeMode = $probeMode
    userWeaselDataPolicy = $userWeaselDataPolicy
  })
}

function Normalize-PathValue {
  param([string] $Path)
  if (-not $Path) { return '' }
  try { return [System.IO.Path]::GetFullPath($Path.Trim('"')).TrimEnd('\') }
  catch { return $Path.Trim('"').TrimEnd('\') }
}

function Same-Path {
  param([string] $Left, [string] $Right)
  return [string]::Equals(
    (Normalize-PathValue $Left),
    (Normalize-PathValue $Right),
    [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-TransactionDebtBinding {
  param(
    [string] $Value,
    [string] $Owner,
    [string] $Kind)
  if (-not $Value -or $Owner -cnotmatch '^[0-9a-f]{32}$') {
    return $false
  }
  return [string]::Equals(
    $Value,
    "famo-debt-v2|$Owner|$Kind",
    [System.StringComparison]::Ordinal)
}

function Get-FinalObjectInfo {
  param([Parameter(Mandatory)][string] $Path)
  return [FamoHealth.NativeFileIdentity]::Get(
    [System.IO.Path]::GetFullPath($Path))
}

function Test-ReparsePoint {
  param([Parameter(Mandatory)][string] $Path)
  return ([System.IO.File]::GetAttributes($Path) -band
    [System.IO.FileAttributes]::ReparsePoint) -ne 0
}

function Test-SafeManifestRelativePath {
  param([string] $Value)
  if ([string]::IsNullOrEmpty($Value) -or
      $Value.IndexOf('/') -ge 0 -or
      $Value.IndexOf(':') -ge 0 -or
      [System.IO.Path]::IsPathRooted($Value)) {
    return $false
  }
  foreach ($segment in $Value.Split([char]'\')) {
    if ([string]::IsNullOrEmpty($segment) -or
        $segment -ceq '.' -or $segment -ceq '..' -or
        $segment.EndsWith('.', [System.StringComparison]::Ordinal) -or
        $segment.EndsWith(' ', [System.StringComparison]::Ordinal) -or
        $segment -match '[<>:"/|?*\x00-\x1F]') {
      return $false
    }
    $deviceBase = $segment.Split([char]'.')[0]
    if ($deviceBase -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') {
      return $false
    }
  }
  return $true
}

function Get-FamoInputTipMembership {
  $tip = '0804:{54EAD76A-B864-4A6D-9C82-148E3352BEE7}{0158C2BA-4E96-4BA8-B505-E1BBEBB3FA33}'
  $root = 'HKCU:\Control Panel\International\User Profile'
  try {
    if (-not (Test-Path -LiteralPath $root)) {
      return [pscustomobject]@{ pass = $true; present = $false; detail = 'language-list root absent' }
    }
    $keys = @((Get-Item -LiteralPath $root)) +
      @(Get-ChildItem -LiteralPath $root -Recurse -ErrorAction Stop)
    foreach ($key in $keys) {
      $record = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction Stop
      foreach ($property in $record.PSObject.Properties) {
        if ([string]::Equals(
            $property.Name,
            $tip,
            [System.StringComparison]::OrdinalIgnoreCase)) {
          return [pscustomobject]@{ pass = $true; present = $true; detail = "TIP listed at $($key.Name)" }
        }
      }
    }
    return [pscustomobject]@{ pass = $true; present = $false; detail = 'TIP not listed' }
  } catch {
    return [pscustomobject]@{ pass = $false; present = $false; detail = "TIP membership probe failed: $($_.Exception.Message)" }
  }
}

function Get-JournalDigest {
  param([object] $Record)
  $fields = [ordered]@{
    schema = '2'
    product = 'Famo'
    generation = [string]$Record.Generation
    phase = [string]$Record.Phase
    version = [string]$Record.Version
    transaction = [string]$Record.TransactionId
    manifest_hash = [string]$Record.ManifestHash
    pending_target = [string]$Record.PendingTarget
    pending_final_target = [string]$Record.PendingFinalTarget
    pending_object_id = [string]$Record.PendingObjectId
    previous_target = [string]$Record.PreviousTarget
    previous_final_target = [string]$Record.PreviousFinalTarget
    previous_object_id = [string]$Record.PreviousObjectId
    prior_previous_target = [string]$Record.PriorPreviousTarget
    prior_previous_final_target = [string]$Record.PriorPreviousFinalTarget
    prior_previous_object_id = [string]$Record.PriorPreviousObjectId
    previous_manifest = [string]$Record.PreviousManifest
    previous_manifest_hash = [string]$Record.PreviousManifestHash
    previous_default = [string]$Record.PreviousDefault
    previous_host = [string]$Record.PreviousHost
    previous_server = [string]$Record.PreviousServer
    previous_profile_tool = [string]$Record.PreviousProfileTool
    previous_version = [string]$Record.PreviousVersion
    previous_identity = [string]$Record.PreviousIdentity
    previous_transaction_id = [string]$Record.PreviousTransactionId
    previous_compatibility_transaction_id = [string]$Record.PreviousCompatibilityTransactionId
    previous_state = [string]$Record.PreviousState
    previous_profile_active = [string]$Record.PreviousProfileActive
    previous_profile_enabled = [string]$Record.PreviousProfileEnabled
    previous_input_tip_present = [string]$Record.PreviousInputTipPresent
    seed_receipt_hash = [string]$Record.SeedReceiptHash
    original_user_sid = [string]$Record.OriginalUserSid
    original_user_account = [string]$Record.OriginalUserAccount
    original_user_session = [string]$Record.OriginalUserSession
    last_proof_session = [string]$Record.LastProofSession
    original_user_resume_capable = [string]$Record.OriginalUserResumeCapable
    resume_installer = [string]$Record.ResumeInstaller
    resume_installer_hash = [string]$Record.ResumeInstallerHash
    resume_task_name = [string]$Record.ResumeTaskName
    allow_downgrade = [string]$Record.AllowDowngrade
    loaded_host_hash = [string]$Record.LoadedHostHash
    loaded_host_version = [string]$Record.LoadedHostVersion
    loaded_host_expected_hash = [string]$Record.LoadedHostExpectedHash
  }
  $canonical = [System.Text.StringBuilder]::new()
  foreach ($field in $fields.GetEnumerator()) {
    $value = [string]$field.Value
    $canonicalField = '{0}:{1}:{2};' -f $field.Key, $value.Length, $value
    [void]$canonical.Append($canonicalField)
  }
  $sha256 = [System.Security.Cryptography.SHA256]::Create()
  try {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($canonical.ToString())
    $digest = [System.Text.StringBuilder]::new(64)
    foreach ($byte in $sha256.ComputeHash($bytes)) {
      [void]$digest.Append($byte.ToString('X2'))
    }
    return $digest.ToString()
  } finally {
    $sha256.Dispose()
  }
}

function Get-FamoRecoveryTaskInventory {
  try {
    $service = New-Object -ComObject 'Schedule.Service'
    $service.Connect()
    $root = $service.GetFolder('\')
    $folders = $root.GetFolders(0)
    $famoFolder = $null
    for ($index = 1; $index -le $folders.Count; $index++) {
      $candidate = $folders.Item($index)
      if ([string]::Equals(
          [string]$candidate.Path, '\Famo',
          [System.StringComparison]::OrdinalIgnoreCase)) {
        if ($null -ne $famoFolder) { throw 'duplicate \Famo task folders' }
        $famoFolder = $candidate
      }
    }
    $names = New-Object System.Collections.Generic.List[string]
    $subfolderNames = New-Object System.Collections.Generic.List[string]
    $folderSddl = ''
    if ($null -ne $famoFolder) {
      $folderSddl = [string]$famoFolder.GetSecurityDescriptor(4)
      $tasks = $famoFolder.GetTasks(1)
      for ($index = 1; $index -le $tasks.Count; $index++) {
        $names.Add([string]$tasks.Item($index).Path)
      }
      $subfolders = $famoFolder.GetFolders(0)
      for ($index = 1; $index -le $subfolders.Count; $index++) {
        $subfolderNames.Add([string]$subfolders.Item($index).Path)
      }
    }
    return [pscustomobject]@{
      pass = $true
      folderPresent = $null -ne $famoFolder
      folderSddl = $folderSddl
      names = @($names)
      subfolders = @($subfolderNames)
      detail = if ($null -ne $famoFolder) {
        "folder=\Famo; sddl=$folderSddl; tasks=$($names -join ', '); subfolders=$($subfolderNames -join ', ')"
      } else {
        'no Famo recovery task folder'
      }
    }
  } catch {
    return [pscustomobject]@{
      pass = $false
      folderPresent = $false
      folderSddl = ''
      names = @()
      subfolders = @()
      detail = "recovery task enumeration failed: $($_.Exception.Message)"
    }
  }
}

function Get-ActiveTransactionJournal {
  param([object] $Brand)
  try {
    $id = [string]$Brand.ActiveTransactionId
    if ($id -cnotmatch '^[0-9a-f]{32}$') { throw 'invalid ActiveTransactionId' }
    $transactionKey = Join-Path $brandKey "Transactions\$id"
    $generationText = [string](Get-ItemPropertyValue -LiteralPath $transactionKey -Name ActiveGeneration)
    if ($generationText -notmatch '^[1-9]\d*$') { throw 'invalid ActiveGeneration' }
    $generationKey = Join-Path $transactionKey "g$generationText"
    $record = Get-ItemProperty -LiteralPath $generationKey
    $requiredFields = @(
      'Schema', 'Product', 'Generation', 'Phase', 'Version',
      'TransactionId', 'ManifestHash', 'PendingTarget',
      'PendingFinalTarget', 'PendingObjectId', 'PreviousTarget',
      'PreviousFinalTarget', 'PreviousObjectId', 'PriorPreviousTarget',
      'PriorPreviousFinalTarget', 'PriorPreviousObjectId',
      'PreviousManifest', 'PreviousManifestHash', 'PreviousDefault', 'PreviousHost',
      'PreviousServer', 'PreviousProfileTool', 'PreviousVersion',
      'PreviousIdentity', 'PreviousTransactionId',
      'PreviousCompatibilityTransactionId', 'PreviousState',
      'PreviousProfileActive', 'PreviousProfileEnabled',
      'PreviousInputTipPresent', 'SeedReceiptHash', 'OriginalUserSid',
      'OriginalUserAccount', 'OriginalUserSession', 'LastProofSession',
      'OriginalUserResumeCapable', 'ResumeInstaller',
      'ResumeInstallerHash', 'ResumeTaskName', 'AllowDowngrade',
      'LoadedHostHash', 'LoadedHostVersion', 'LoadedHostExpectedHash',
      'Digest'
    )
    foreach ($field in $requiredFields) {
      if ($null -eq $record.PSObject.Properties[$field]) {
        throw "missing journal field: $field"
      }
    }
    $pendingObjectId = [string]$record.PendingObjectId
    $phaseAllowsAbsentPendingObject =
      [string]$record.Phase -cin @(
        'Prepared', 'RollbackIntent', 'RolledBack')
    $pendingTargetExists =
      Test-Path -LiteralPath ([string]$record.PendingTarget) -PathType Container
    $pendingObjectIdValid =
      if ([string]::IsNullOrEmpty($pendingObjectId)) {
        $phaseAllowsAbsentPendingObject -and -not $pendingTargetExists
      } else {
        $pendingObjectId -match '^\d+:\d+:\d+$' -and
          $pendingObjectId -notmatch ':0:0$'
      }
    if ([string]$record.Schema -ne '2' -or [string]$record.Product -ne 'Famo' -or
        [string]$record.Generation -ne $generationText -or
        [string]$record.TransactionId -cne $id -or
        [string]$record.ManifestHash -notmatch '^[0-9A-Fa-f]{64}$' -or
        [string]$record.Digest -notmatch '^[0-9A-Fa-f]{64}$' -or
        [string]$record.PendingFinalTarget -eq '' -or
        -not $pendingObjectIdValid -or
        [string]$record.PreviousProfileActive -notin @('0', '1') -or
        [string]$record.PreviousProfileEnabled -notin @('0', '1') -or
        [string]$record.PreviousInputTipPresent -notin @('0', '1') -or
        [string]$record.OriginalUserResumeCapable -notin @('0', '1') -or
        [string]$record.AllowDowngrade -notin @('0', '1')) {
      throw 'generation schema or identity mismatch'
    }
    $hasPrevious = -not [string]::IsNullOrEmpty([string]$record.PreviousTarget)
    if (($hasPrevious -and [string]$record.PreviousManifestHash -notmatch '^[0-9A-Fa-f]{64}$') -or
        (-not $hasPrevious -and -not [string]::IsNullOrEmpty([string]$record.PreviousManifestHash)) -or
        ($hasPrevious -and
          ([string]$record.PreviousFinalTarget -eq '' -or
           [string]$record.PreviousObjectId -notmatch '^\d+:\d+:\d+$' -or
           [string]$record.PreviousObjectId -match ':0:0$')) -or
        (-not $hasPrevious -and
          ([string]$record.PreviousFinalTarget -ne '' -or
           [string]$record.PreviousObjectId -ne '')) -or
        (-not [string]::IsNullOrEmpty([string]$record.SeedReceiptHash) -and
          [string]$record.SeedReceiptHash -notmatch '^[0-9A-Fa-f]{64}$') -or
        ([string]$record.PreviousProfileActive -eq '1' -and
          [string]$record.PreviousProfileEnabled -ne '1') -or
        (-not $hasPrevious -and
          ([string]$record.PreviousProfileActive -ne '0' -or
           [string]$record.PreviousProfileEnabled -ne '0' -or
           [string]$record.PreviousInputTipPresent -ne '0'))) {
      throw 'generation predecessor or user-state mismatch'
    }
    if ((Get-JournalDigest $record) -ne [string]$record.Digest) {
      throw 'generation digest mismatch'
    }
    $null = [System.Security.Principal.SecurityIdentifier]::new([string]$record.OriginalUserSid)
    $expectedLeaf = '{0}-{1}-{2}' -f $record.Version, ([string]$record.ManifestHash).Substring(0, 12), $id
    $expectedTarget = Join-Path $env:ProgramFiles "Famo\versions\$expectedLeaf"
    if (-not (Same-Path ([string]$record.PendingTarget) $expectedTarget)) {
      throw 'pending target does not match journal identity'
    }
    return [pscustomobject]@{
      pass = $true
      detail = "journal=$generationKey; phase=$($record.Phase)"
      id = $id
      key = $generationKey
      record = $record
    }
  } catch {
    return [pscustomobject]@{
      pass = $false
      detail = "journal invalid: $($_.Exception.Message)"
      id = ''
      key = ''
      record = $null
    }
  }
}

function Test-RecoveryTask {
  param([object] $JournalInfo)
  try {
    $record = $JournalInfo.record
    $id = $JournalInfo.id
    $installer = [string]$record.ResumeInstaller
    $installerHash = [string]$record.ResumeInstallerHash
    $taskName = [string]$record.ResumeTaskName
    $sid = [string]$record.OriginalUserSid
    $expectedTask = "\Famo\Transaction-$id"
    $expectedInstaller = Join-Path $env:ProgramFiles "Famo\pending\$id\Famo-Resume-$id.exe"
    $expectedArguments = "/FamoRecover=$id /FamoManifest=$($record.ManifestHash) /FamoVersion=$($record.Version) /VERYSILENT /SUPPRESSMSGBOXES /NORESTART"
    $expectedSddl = "D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;$sid)"
    if ($taskName -cne $expectedTask -or -not (Same-Path $installer $expectedInstaller) -or
        $installerHash -notmatch '^[0-9A-Fa-f]{64}$' -or
        -not (Test-Path -LiteralPath $installer -PathType Leaf) -or
        (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $installerHash) {
      throw 'retained installer identity mismatch'
    }
    $taskOutput = & "$env:SystemRoot\System32\schtasks.exe" /Query /TN $taskName /XML 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'scheduled task is missing' }
    [xml]$taskXml = $taskOutput -join [Environment]::NewLine
    $nodes = {
      param([string] $Name)
      @($taskXml.SelectNodes("//*[local-name()='$Name']"))
    }
    $userIds = & $nodes 'UserId'
    $principals = & $nodes 'Principal'
    $logonTriggers = & $nodes 'LogonTrigger'
    $actions = & $nodes 'Actions'
    $execs = & $nodes 'Exec'
    $commands = & $nodes 'Command'
    $arguments = & $nodes 'Arguments'
    $logonTypes = & $nodes 'LogonType'
    $runLevels = & $nodes 'RunLevel'
    $securityDescriptors = & $nodes 'SecurityDescriptor'
    $enabled = & $nodes 'Enabled'
    $settings = & $nodes 'Settings'
    $triggerElements = @($taskXml.SelectNodes("//*[local-name()='Triggers']/*"))
    $actionElements = @($taskXml.SelectNodes("//*[local-name()='Actions']/*"))
    $triggerEnabled = @($taskXml.SelectNodes(
      "//*[local-name()='LogonTrigger']/*[local-name()='Enabled']"))
    $settingsEnabled = @($taskXml.SelectNodes(
      "//*[local-name()='Settings']/*[local-name()='Enabled']"))
    if ($principals.Count -ne 1 -or $logonTriggers.Count -ne 1 -or
        $actions.Count -ne 1 -or $settings.Count -ne 1 -or
        $execs.Count -ne 1 -or
        $commands.Count -ne 1 -or $arguments.Count -ne 1 -or
        $securityDescriptors.Count -ne 1 -or
        [string]$securityDescriptors[0].'#text' -cne $expectedSddl -or
        [string]$principals[0].id -cne 'OriginalUser' -or
        [string]$actions[0].Context -cne 'OriginalUser' -or
        $triggerElements.Count -ne 1 -or
        [string]$triggerElements[0].LocalName -cne 'LogonTrigger' -or
        $actionElements.Count -ne 1 -or
        [string]$actionElements[0].LocalName -cne 'Exec' -or
        $triggerEnabled.Count -ne 1 -or
        [string]$triggerEnabled[0].'#text' -cne 'true' -or
        $settingsEnabled.Count -ne 1 -or
        [string]$settingsEnabled[0].'#text' -cne 'true' -or
        $enabled.Count -ne 2 -or
        @($enabled | Where-Object { [string]$_.'#text' -cne 'true' }).Count -ne 0 -or
        $userIds.Count -ne 2 -or @($userIds | Where-Object { $_.'#text' -cne $sid }).Count -ne 0 -or
        $logonTypes.Count -ne 1 -or [string]$logonTypes[0].'#text' -cne 'InteractiveToken' -or
        $runLevels.Count -ne 1 -or [string]$runLevels[0].'#text' -cne 'HighestAvailable' -or
        -not (Same-Path ([string]$commands[0].'#text') $installer) -or
        [string]$arguments[0].'#text' -cne $expectedArguments) {
      throw 'scheduled task XML identity mismatch'
    }
    return [pscustomobject]@{ pass = $true; detail = "task=$taskName; sid=$sid; installer=$installer" }
  } catch {
    return [pscustomobject]@{ pass = $false; detail = $_.Exception.Message }
  }
}

function Invoke-ProfileTool {
  param(
    [Parameter(Mandatory)]
    [string] $Path,
    [Parameter(Mandatory)]
    [string] $Argument
  )

  # FamoProfileTool is a Windows-subsystem executable. PowerShell does not wait
  # for such applications when they are invoked directly, and rejects them in
  # the middle of a pipeline. Use Process so health checks receive the real
  # output and exit code without changing registration or input-method state.
  $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
  $startInfo.FileName = $Path
  $startInfo.Arguments = $Argument
  $startInfo.UseShellExecute = $false
  $startInfo.CreateNoWindow = $true
  $startInfo.RedirectStandardOutput = $true
  $startInfo.RedirectStandardError = $true

  $process = [System.Diagnostics.Process]::new()
  $process.StartInfo = $startInfo
  try {
    if (-not $process.Start()) {
      throw "Failed to start profile tool: $Path"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $output = @(
      $stdoutTask.GetAwaiter().GetResult().Trim()
      $stderrTask.GetAwaiter().GetResult().Trim()
    ) | Where-Object { $_ }
    return [pscustomobject]@{
      ExitCode = $process.ExitCode
      Output = ($output -join [Environment]::NewLine)
    }
  } finally {
    $process.Dispose()
  }
}

function Test-Manifest {
  param(
    [string] $Target,
    [string] $Manifest,
    [string] $ExpectedVersion
  )

  $problems = New-Object System.Collections.Generic.List[string]
  $manifestHash = ''
  $targetIdentity = $null
  $manifestIdentity = $null
  $addProblem = {
    param([string] $Problem)
    if ($problems.Count -lt 20) { $problems.Add($Problem) }
  }
  try {
    if (-not $Target -or
        -not (Test-Path -LiteralPath $Target -PathType Container)) {
      throw 'active transaction target is missing'
    }
    if (-not $Manifest -or
        -not (Test-Path -LiteralPath $Manifest -PathType Leaf)) {
      throw 'active payload manifest is missing'
    }
    if (Test-ReparsePoint $Target) {
      throw 'active transaction target is a reparse point'
    }
    if (Test-ReparsePoint $Manifest) {
      throw 'active payload manifest is a reparse point'
    }

    $targetPath = [System.IO.Path]::GetFullPath($Target).TrimEnd('\')
    $manifestPath = [System.IO.Path]::GetFullPath($Manifest)
    $expectedManifestPath = Join-Path $targetPath 'payload-manifest.txt'
    if (-not (Same-Path $manifestPath $expectedManifestPath)) {
      throw 'payload manifest logical path mismatch'
    }

    $targetIdentity = Get-FinalObjectInfo $targetPath
    $manifestIdentity = Get-FinalObjectInfo $manifestPath
    if (-not $targetIdentity.ObjectId -or
        -not $manifestIdentity.ObjectId) {
      throw 'payload root or manifest object identity is unavailable'
    }
    if (-not (Same-Path (
          [System.IO.Path]::GetDirectoryName($manifestIdentity.FinalPath)) (
          $targetIdentity.FinalPath))) {
      throw 'payload manifest resolves outside transaction root'
    }
    if ($targetIdentity.LinkCount -ne 1 -or
        $manifestIdentity.LinkCount -ne 1) {
      throw 'payload root or manifest has an unexpected hard-link identity'
    }

    $utf8 = [System.Text.UTF8Encoding]::new($false, $true)
    $lines = [System.IO.File]::ReadAllLines($manifestPath, $utf8)
    $expectedHeaders = [ordered]@{
      format = '1'
      product = 'Famo'
      version = $ExpectedVersion
      protocol = '1'
      architecture = 'x64'
      identity = 'Stable'
    }
    $headerCounts = @{}
    foreach ($name in $expectedHeaders.Keys) { $headerCounts[$name] = 0 }
    $declaredCount = -1
    $fileCountLines = 0
    $entries = New-Object System.Collections.Generic.List[object]

    foreach ($line in $lines) {
      if ($line.StartsWith('file=', [System.StringComparison]::Ordinal)) {
        $parts = $line.Substring(5).Split([char]'|')
        if ($parts.Count -ne 3 -or
            -not (Test-SafeManifestRelativePath $parts[0]) -or
            $parts[1] -notmatch '^(0|[1-9][0-9]*)$' -or
            $parts[2] -notmatch '^[0-9A-Fa-f]{64}$') {
          & $addProblem 'invalid file entry'
          continue
        }
        $expectedSize = 0L
        if (-not [int64]::TryParse(
            $parts[1],
            [System.Globalization.NumberStyles]::None,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$expectedSize)) {
          & $addProblem 'invalid file size'
          continue
        }
        $entries.Add([pscustomobject]@{
          relative = $parts[0]
          size = $expectedSize
          hash = $parts[2].ToUpperInvariant()
        })
        continue
      }

      if ($line.StartsWith('file_count=', [System.StringComparison]::Ordinal)) {
        $fileCountLines++
        $countText = $line.Substring(11)
        $parsedCount = 0
        if ($countText -notmatch '^(0|[1-9][0-9]*)$' -or
            -not [int]::TryParse(
              $countText,
              [System.Globalization.NumberStyles]::None,
              [System.Globalization.CultureInfo]::InvariantCulture,
              [ref]$parsedCount)) {
          & $addProblem 'invalid file_count'
        } else {
          $declaredCount = $parsedCount
        }
        continue
      }

      $separator = $line.IndexOf('=')
      $name = if ($separator -gt 0) { $line.Substring(0, $separator) } else { '' }
      if ($expectedHeaders.Contains($name)) {
        $headerCounts[$name]++
        if ($line.Substring($separator + 1) -cne
            [string]$expectedHeaders[$name]) {
          & $addProblem "header mismatch:$name"
        }
      } else {
        & $addProblem 'unknown manifest record'
      }
    }

    foreach ($name in $expectedHeaders.Keys) {
      if ($headerCounts[$name] -ne 1) {
        & $addProblem "header count:$name"
      }
    }
    if ($fileCountLines -ne 1 -or $declaredCount -ne $entries.Count) {
      & $addProblem "file_count:$declaredCount/$($entries.Count)"
    }

    $actualFiles = New-Object System.Collections.Generic.List[object]
    $directories = New-Object System.Collections.Generic.Stack[object]
    $directories.Push((Get-Item -LiteralPath $targetPath -Force))
    while ($directories.Count -gt 0) {
      $directory = $directories.Pop()
      foreach ($child in $directory.EnumerateFileSystemInfos()) {
        if (($child.Attributes -band
            [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
          & $addProblem 'payload contains a reparse point'
          continue
        }
        if (($child.Attributes -band
            [System.IO.FileAttributes]::Directory) -ne 0) {
          $directories.Push($child)
        } else {
          $actualFiles.Add($child)
        }
      }
    }

    $seenLogicalPaths = [System.Collections.Generic.HashSet[string]]::new(
      [System.StringComparer]::OrdinalIgnoreCase)
    $seenFinalPaths = [System.Collections.Generic.HashSet[string]]::new(
      [System.StringComparer]::OrdinalIgnoreCase)
    $seenObjectIds = [System.Collections.Generic.HashSet[string]]::new(
      [System.StringComparer]::Ordinal)
    $manifestFinalPaths = [System.Collections.Generic.HashSet[string]]::new(
      [System.StringComparer]::OrdinalIgnoreCase)
    $manifestObjectIds = [System.Collections.Generic.HashSet[string]]::new(
      [System.StringComparer]::Ordinal)
    $finalRootPrefix = $targetIdentity.FinalPath.TrimEnd('\') + '\'

    foreach ($entry in $entries) {
      $fullPath = [System.IO.Path]::GetFullPath(
        (Join-Path $targetPath $entry.relative))
      if (-not $fullPath.StartsWith(
          $targetPath + '\',
          [System.StringComparison]::OrdinalIgnoreCase) -or
          -not $seenLogicalPaths.Add($fullPath)) {
        & $addProblem 'duplicate or escaping payload path'
        continue
      }
      if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        & $addProblem "missing:$($entry.relative)"
        continue
      }
      if (Test-ReparsePoint $fullPath) {
        & $addProblem "reparse:$($entry.relative)"
        continue
      }

      $before = Get-FinalObjectInfo $fullPath
      if (-not $before.FinalPath.StartsWith(
          $finalRootPrefix,
          [System.StringComparison]::OrdinalIgnoreCase) -or
          -not $seenFinalPaths.Add($before.FinalPath) -or
          -not $before.ObjectId -or
          -not $seenObjectIds.Add($before.ObjectId) -or
          $before.LinkCount -ne 1) {
        & $addProblem "unsafe object:$($entry.relative)"
        continue
      }

      $item = Get-Item -LiteralPath $fullPath -Force
      if ($item.Length -ne [int64]$entry.size) {
        & $addProblem "size:$($entry.relative)"
        continue
      }
      $actualHash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash
      $after = Get-FinalObjectInfo $fullPath
      if (-not (Same-Path $before.FinalPath $after.FinalPath) -or
          $before.ObjectId -cne $after.ObjectId -or
          $after.LinkCount -ne 1 -or
          (Get-Item -LiteralPath $fullPath -Force).Length -ne
            [int64]$entry.size) {
        & $addProblem "changed:$($entry.relative)"
        continue
      }
      if ($actualHash -cne [string]$entry.hash) {
        & $addProblem "hash:$($entry.relative)"
        continue
      }
      [void]$manifestFinalPaths.Add($before.FinalPath)
      [void]$manifestObjectIds.Add($before.ObjectId)
    }

    $seenActualPaths = [System.Collections.Generic.HashSet[string]]::new(
      [System.StringComparer]::OrdinalIgnoreCase)
    $seenActualObjectIds = [System.Collections.Generic.HashSet[string]]::new(
      [System.StringComparer]::Ordinal)
    $actualCount = 0
    foreach ($actual in $actualFiles) {
      $identity = Get-FinalObjectInfo $actual.FullName
      if (Same-Path $identity.FinalPath $manifestIdentity.FinalPath) {
        continue
      }
      if (-not $identity.FinalPath.StartsWith(
          $finalRootPrefix,
          [System.StringComparison]::OrdinalIgnoreCase) -or
          -not $seenActualPaths.Add($identity.FinalPath) -or
          -not $identity.ObjectId -or
          -not $seenActualObjectIds.Add($identity.ObjectId) -or
          $identity.LinkCount -ne 1 -or
          -not $manifestFinalPaths.Contains($identity.FinalPath) -or
          -not $manifestObjectIds.Contains($identity.ObjectId)) {
        & $addProblem 'payload contains an unmanifested or duplicate object'
      }
      $actualCount++
    }
    if ($actualCount -ne $entries.Count -or
        $manifestFinalPaths.Count -ne $entries.Count) {
      & $addProblem "actual_count:$actualCount/$($entries.Count)"
    }
    $manifestHash =
      (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
    $targetAfter = Get-FinalObjectInfo $targetPath
    $manifestAfter = Get-FinalObjectInfo $manifestPath
    if (-not (Same-Path $targetIdentity.FinalPath $targetAfter.FinalPath) -or
        $targetIdentity.ObjectId -cne $targetAfter.ObjectId -or
        $targetAfter.LinkCount -ne 1 -or
        -not (Same-Path $manifestIdentity.FinalPath $manifestAfter.FinalPath) -or
        $manifestIdentity.ObjectId -cne $manifestAfter.ObjectId -or
        $manifestAfter.LinkCount -ne 1) {
      & $addProblem 'payload root or manifest changed during verification'
    }
  } catch {
    & $addProblem $_.Exception.Message
  }

  return [pscustomobject]@{
    pass = $problems.Count -eq 0
    targetFinalPath = if ($targetIdentity) { $targetIdentity.FinalPath } else { '' }
    targetObjectId = if ($targetIdentity) { $targetIdentity.ObjectId } else { '' }
    manifestHash = $manifestHash
    detail = if ($problems.Count) {
      $problems -join '; '
    } else {
      "manifest verified: $($entries.Count) files"
    }
  }
}

function Test-ControlPipe {
  $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
  $session = (Get-Process -Id $PID).SessionId
  $name = "Famo.Runtime.v2.$sid.$session.control-v2"
  $client = New-Object System.IO.Pipes.NamedPipeClientStream(
    '.', $name, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::Asynchronous)
  try {
    $client.Connect(200)
    return [pscustomobject]@{ state = 'Ready'; detail = "bounded secure control connect succeeded: $name" }
  } catch [System.TimeoutException] {
    return [pscustomobject]@{ state = 'Hung'; detail = "control pipe connect exceeded 200 ms: $name" }
  } catch {
    return [pscustomobject]@{ state = 'Broken'; detail = "control pipe connect failed: $($_.Exception.Message)" }
  } finally {
    $client.Dispose()
  }
}

$brandPresent = Test-Path -LiteralPath $brandKey
$machineComPresent = Test-Path -LiteralPath $machineComKey
$tipPresent = Test-Path -LiteralPath $tipKey
$runValue = [string](Get-ItemProperty -LiteralPath $runKey -Name FamoRuntime -ErrorAction SilentlyContinue).FamoRuntime
$brand = if ($brandPresent) { Get-ItemProperty -LiteralPath $brandKey } else { $null }
$recoveryTaskInventory = Get-FamoRecoveryTaskInventory
$journalInfo = if ($brand) { Get-ActiveTransactionJournal $brand } else {
  [pscustomobject]@{ pass = $true; detail = 'journal N/A'; id = ''; key = ''; record = $null }
}
$currentUserSid = try {
  [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
} catch {
  ''
}
$journalOwnerSid = if ($journalInfo.pass -and $journalInfo.record) {
  [string]$journalInfo.record.OriginalUserSid
} else {
  ''
}
$wrongUserContext = $journalOwnerSid -and (
  -not [string]::Equals(
    $currentUserSid,
    $journalOwnerSid,
    [System.StringComparison]::OrdinalIgnoreCase))
$exactUserContext = if (-not $brand) {
  [bool]$currentUserSid
} else {
  [bool]($journalOwnerSid -and -not $wrongUserContext)
}

# HKCU and per-user language-list probes are meaningful only for the exact SID
# authenticated by the active journal. A different user receives Unsupported
# without inspecting or executing against that user's state.
$userComPresent = $false
$userTipPresent = $false
$inputTipMembership = [pscustomobject]@{
  pass = $false
  present = $false
  detail = 'per-user probe skipped: journal SID is not the current SID'
}
if ($exactUserContext) {
  $userComPresent = Test-Path -LiteralPath $userComKey
  $userTipPresent = Test-Path -LiteralPath $userTipKey
  $inputTipMembership = Get-FamoInputTipMembership
}

$journalPhase = if ($journalInfo.record) { [string]$journalInfo.record.Phase } else { '' }
$debtNames = New-Object System.Collections.Generic.List[string]
foreach ($debtName in @(
    'UserCleanupDebt', 'UserRollbackDebt', 'TargetCleanupDebt',
    'RecoveryCleanupDebt', 'VersionCleanupDebt', 'CleanupDebt')) {
  if ($brand -and [string]$brand.$debtName) { $debtNames.Add($debtName) }
}
$cleanupDebtCount = if ($brand) { [string]$brand.CleanupDebtCount } else { '' }
if ($cleanupDebtCount) {
  $parsedCleanupDebtCount = 0L
  if ($cleanupDebtCount -notmatch '^\d+$' -or
      -not [int64]::TryParse($cleanupDebtCount, [ref]$parsedCleanupDebtCount) -or
      $parsedCleanupDebtCount -ne 0) {
    $debtNames.Add('CleanupDebtCount')
  }
}
$cleanupDebtPresent = $debtNames.Count -gt 0
$userCleanupDebtBound = $brand -and $journalInfo.id -and
  (Test-TransactionDebtBinding (
    [string]$brand.UserCleanupDebt) $journalInfo.id 'seed-commit')
$userRollbackDebtBound = $brand -and $journalInfo.id -and
  (Test-TransactionDebtBinding (
    [string]$brand.UserRollbackDebt) $journalInfo.id 'user-rollback')
$targetCleanupDebtBound = $brand -and $journalInfo.id -and
  (Test-TransactionDebtBinding (
    [string]$brand.TargetCleanupDebt) $journalInfo.id 'target-cleanup')
$versionCleanupDebtBound = $brand -and $journalInfo.id -and
  (Test-TransactionDebtBinding (
    [string]$brand.VersionCleanupDebt) $journalInfo.id 'version-retention')
$recoveryCleanupDebtPresent = [bool](
  $brand -and [string]$brand.RecoveryCleanupDebt)
$recoveryCleanupDebtBound = $brand -and $journalInfo.id -and
  (Test-TransactionDebtBinding (
    [string]$brand.RecoveryCleanupDebt) $journalInfo.id 'recovery-artifacts')
$recoveryDebtPresent = [bool](
  $userCleanupDebtBound -or $userRollbackDebtBound -or
  $targetCleanupDebtBound -or $versionCleanupDebtBound -or
  $recoveryCleanupDebtBound -or
  ($brand -and $journalInfo.id -and
    [string]::Equals(
      [string]$brand.CleanupDebt,
      $journalInfo.id,
      [System.StringComparison]::Ordinal)))

$emptyRollbackShape = $journalInfo.record -and $journalPhase -eq 'RolledBack' -and
  [string]$brand.ActiveTransactionId -ceq $journalInfo.id -and
  -not [string]$brand.TransactionId -and
  [string]$brand.InstallState -ceq 'RolledBack' -and
  -not [string]$brand.InstallDir -and
  -not [string]$brand.ServerExecutable -and
  -not [string]$brand.ProfileTool -and
  -not [string]$brand.ActiveManifest -and
  -not [string]$brand.ActiveVersion -and
  -not [string]$brand.Identity -and
  -not [string]$journalInfo.record.PreviousTarget -and
  -not [string]$journalInfo.record.PreviousManifest -and
  -not [string]$journalInfo.record.PreviousHost -and
  -not [string]$journalInfo.record.PreviousTransactionId -and
  -not [string]$journalInfo.record.PreviousCompatibilityTransactionId -and
  -not [string]$journalInfo.record.PreviousState
$emptyRollbackAnchor = $emptyRollbackShape -and -not $cleanupDebtPresent
$installationFootprintsAbsent = (-not $brandPresent -or $emptyRollbackAnchor) -and
  -not $machineComPresent -and -not $userComPresent -and -not $userTipPresent -and
  -not $tipPresent -and -not $runValue -and $inputTipMembership.pass -and
  -not $inputTipMembership.present
$notInstalled = $installationFootprintsAbsent -and $recoveryTaskInventory.pass -and
  -not $recoveryTaskInventory.folderPresent
$isLegacyRollbackAnchor = $journalInfo.record -and $journalPhase -eq 'RolledBack' -and
  -not [string]$journalInfo.record.PreviousTransactionId -and
  [string]$journalInfo.record.PreviousCompatibilityTransactionId -match '^\d{14}-(0|[1-9]\d{0,5})$' -and
  [string]$brand.ActiveTransactionId -ceq $journalInfo.id -and
  [string]$brand.TransactionId -ceq [string]$journalInfo.record.PreviousCompatibilityTransactionId -and
  [string]$brand.InstallState -ceq [string]$journalInfo.record.PreviousState -and
  [string]$brand.InstallState -ceq 'Ready' -and
  (Same-Path ([string]$brand.InstallDir) ([string]$journalInfo.record.PreviousTarget)) -and
  (Same-Path ([string]$brand.ActiveManifest) ([string]$journalInfo.record.PreviousManifest))
$activeReady = $journalInfo.record -and $journalPhase -eq 'Ready' -and
  [string]$brand.ActiveTransactionId -ceq $journalInfo.id -and
  [string]$brand.TransactionId -ceq $journalInfo.id -and
  [string]$brand.InstallState -ceq 'Ready' -and
  (Same-Path ([string]$brand.InstallDir) ([string]$journalInfo.record.PendingTarget)) -and
  (Same-Path ([string]$brand.ActiveManifest) (
    Join-Path ([string]$journalInfo.record.PendingTarget) 'payload-manifest.txt'))
$isPending = $journalPhase -eq 'PendingReboot'
$installState = if ($isLegacyRollbackAnchor) {
  [string]$brand.InstallState
} elseif ($journalInfo.record) {
  $journalPhase
} elseif ($brand) {
  [string]$brand.InstallState
} else {
  'NotInstalled'
}
$journalTerminalOk = $activeReady -or $isPending -or
  $isLegacyRollbackAnchor -or $emptyRollbackShape

Add-Check 'H0' 'S1' (-not $wrongUserContext) `
  $(if ($wrongUserContext) {
    "unsupported user context: current SID=$currentUserSid; journal SID=$journalOwnerSid"
  } elseif ($journalOwnerSid) {
    "exact journal user SID=$journalOwnerSid"
  } else {
    'journal user SID N/A'
  })

Add-Check 'H1' 'S0' ($notInstalled -or ($journalInfo.pass -and $journalTerminalOk)) `
  $(if ($notInstalled) { 'clean NotInstalled terminal state' } else { "$($journalInfo.detail); InstallState=$installState" })

$version = if ($isLegacyRollbackAnchor) {
  [string]$journalInfo.record.PreviousVersion
} elseif ($journalInfo.record) {
  [string]$journalInfo.record.Version
} elseif ($isPending) {
  [string]$brand.PendingVersion
} else {
  [string]$brand.ActiveVersion
}
$noActivePayload = [bool]$emptyRollbackShape
$identityOk = $noActivePayload -or (
  $brand -and $brand.Identity -eq 'Stable' -and $version -and
  $journalInfo.id -and (-not $isPending -or [string]$brand.PendingReason))
Add-Check 'H2' 'S0' ($notInstalled -or [bool]$identityOk) `
  $(if ($notInstalled -or $noActivePayload) {
    'identity N/A without an active payload'
  } else {
    "Identity=$($brand.Identity); version=$version; transaction=$($brand.TransactionId); reason=$($brand.PendingReason)"
  })

$target = if ($isPending -and $journalInfo.record) { [string]$journalInfo.record.PendingTarget } elseif ($brand) { [string]$brand.InstallDir } else { '' }
$server = if ($isPending -and $target) { Join-Path $target 'FamoRuntime.exe' } elseif ($brand) { [string]$brand.ServerExecutable } else { '' }
$profileTool = if ($isPending -and $target) { Join-Path $target 'FamoProfileTool.exe' } elseif ($brand) { [string]$brand.ProfileTool } else { '' }
$manifest = if ($isPending -and $target) { Join-Path $target 'payload-manifest.txt' } elseif ($brand) { [string]$brand.ActiveManifest } else { '' }
$pathsOk = $noActivePayload -or ($target -and
  (Test-Path -LiteralPath $target -PathType Container) -and
  (Same-Path $server (Join-Path $target 'FamoRuntime.exe')) -and
  (Same-Path $profileTool (Join-Path $target 'FamoProfileTool.exe')) -and
  (Same-Path $manifest (Join-Path $target 'payload-manifest.txt')))
Add-Check 'H3' 'S0' ($notInstalled -or [bool]$pathsOk) `
  $(if ($notInstalled -or $noActivePayload) {
    'transaction paths N/A without an active payload'
  } else {
    "target=$target; server=$server; profileTool=$profileTool"
  })

$manifestResult = if ($notInstalled -or $noActivePayload) {
  [pscustomobject]@{
    pass = $true
    detail = 'manifest N/A without an active payload'
    targetFinalPath = ''
    targetObjectId = ''
    manifestHash = ''
  }
} else { Test-Manifest $target $manifest $version }
$manifestJournalHashOk = $notInstalled -or $noActivePayload -or
  ($manifestResult.pass -and $journalInfo.record -and
   $(if ($isLegacyRollbackAnchor) {
     [string]$journalInfo.record.PreviousIdentity -ceq 'Stable' -and
       (Same-Path $manifest ([string]$journalInfo.record.PreviousManifest)) -and
       (Same-Path -Left $manifestResult.targetFinalPath -Right ([string]$journalInfo.record.PreviousFinalTarget)) -and
       [string]$manifestResult.targetObjectId -ceq
         [string]$journalInfo.record.PreviousObjectId -and
       [string]$manifestResult.manifestHash -ceq
         [string]$journalInfo.record.PreviousManifestHash -and
       (Get-FileHash -LiteralPath (Join-Path $target 'FamoTextService.dll') -Algorithm SHA256).Hash -eq
         [string]$journalInfo.record.LoadedHostExpectedHash
   } else {
     (Same-Path -Left $manifestResult.targetFinalPath -Right ([string]$journalInfo.record.PendingFinalTarget)) -and
     [string]$manifestResult.targetObjectId -ceq
       [string]$journalInfo.record.PendingObjectId -and
     [string]$manifestResult.manifestHash -ceq
       [string]$journalInfo.record.ManifestHash
   }))
Add-Check 'H3b' 'S0' ([bool]($manifestResult.pass -and $manifestJournalHashOk)) `
  ("{0}; journal hash match={1}" -f $manifestResult.detail, $manifestJournalHashOk)

$registeredDll = ''
$threadingModel = ''
if ($machineComPresent) {
  $com = Get-Item -LiteralPath $machineComKey
  $registeredDll = [string]$com.GetValue('')
  $threadingModel = [string]$com.GetValue('ThreadingModel')
}
$comOk = if ($noActivePayload) {
  -not $machineComPresent
} elseif ($isPending) {
  -not $machineComPresent -and
    (-not $exactUserContext -or -not $userComPresent)
} else {
  $machineComPresent -and
    (-not $exactUserContext -or -not $userComPresent) -and
    (Same-Path $registeredDll (Join-Path $target 'FamoTextService.dll')) -and
    $threadingModel -eq 'Apartment'
}
Add-Check 'H4' 'S0' ($notInstalled -or [bool]$comOk) `
  $(if ($notInstalled -or $noActivePayload) {
    'COM registration absent without an active payload'
  } elseif (-not $exactUserContext) {
    "HKLM COM=$registeredDll; ThreadingModel=$threadingModel; HKCU probe skipped"
  } else {
    "HKLM COM=$registeredDll; ThreadingModel=$threadingModel; HKCU override=$userComPresent"
  })

$profileOutput = ''
$profileExit = -1
$profileCommand = if ($isPending) { 'check-absent' } else { 'check' }
$payloadExecutionTrusted = -not $notInstalled -and -not $noActivePayload -and
  $exactUserContext -and $pathsOk -and $manifestResult.pass -and
  $manifestJournalHashOk
if ($payloadExecutionTrusted -and
    (Test-Path -LiteralPath $profileTool -PathType Leaf)) {
  $profileCommand = if ($isPending) { 'check-absent' } else { 'check' }
  try {
    $profileResult = Invoke-ProfileTool -Path $profileTool -Argument $profileCommand
    $profileOutput = $profileResult.Output
    $profileExit = $profileResult.ExitCode
  } catch {
    $profileOutput = $_.Exception.Message
  }
}
Add-Check 'H5' 'S0' (
  $notInstalled -or $noActivePayload -or
  (-not $exactUserContext) -or $profileExit -eq 0) `
  $(if ($notInstalled -or $noActivePayload) {
    'TSF profile N/A without an active payload'
  } elseif (-not $exactUserContext) {
    'profile execution skipped: journal SID is not the current SID'
  } elseif (-not $payloadExecutionTrusted) {
    'profile execution skipped: payload proof is not trusted'
  } else {
    "profile command=$profileCommand; exit=$profileExit; $profileOutput"
  })

$matchingProcess = $null
$processNotes = @()
if ($exactUserContext) {
  foreach ($process in @(Get-Process -Name FamoRuntime -ErrorAction SilentlyContinue)) {
    $path = try { [string]$process.Path } catch { '' }
    $processNotes += if ($path) { "$($process.Id):$path" } else { "$($process.Id):(path unreadable)" }
    if ($path -and (Same-Path $path $server)) { $matchingProcess = $process }
  }
}
$pipeProbe = if ($matchingProcess -and $payloadExecutionTrusted) {
  Test-ControlPipe
} elseif (-not $exactUserContext) {
  [pscustomobject]@{ state = 'Unsupported'; detail = 'runtime probe skipped: journal SID is not the current SID' }
} elseif ($matchingProcess) {
  [pscustomobject]@{ state = 'Untrusted'; detail = 'control pipe probe skipped: payload proof is not trusted' }
} else {
  [pscustomobject]@{ state = 'Missing'; detail = 'exact FamoRuntime process is not running' }
}
$runtimeOk = if ($noActivePayload) {
  $true
} elseif (-not $exactUserContext) {
  $true
} elseif ($isPending) {
  -not $matchingProcess -and $processNotes.Count -eq 0 -and -not $runValue
} else {
  $matchingProcess -and $pipeProbe.state -eq 'Ready'
}
Add-Check 'H6' $(if ($isPending) { 'S0' } else { 'S1' }) ($notInstalled -or [bool]$runtimeOk) `
  $(if ($notInstalled -or $noActivePayload) {
    'runtime N/A without an active payload'
  } elseif ($isPending) {
    "PendingReboot runtime and Run entry absent=$runtimeOk; processes=$($processNotes -join ', ')"
  } else {
    "$($pipeProbe.detail); processes=$($processNotes -join ', ')"
  })

$isolationProblems = @()
foreach ($value in @($target, $server, $profileTool, $manifest)) {
  if ($value -match '(?i)\\AppData\\Roaming\\Rime') { $isolationProblems += "Rime path:$value" }
}
if ($target -and (Test-Path -LiteralPath (Join-Path $target 'FamoDeploy.exe'))) { $isolationProblems += 'legacy FamoDeploy.exe present' }
if ($target -and (Test-Path -LiteralPath (Join-Path $target 'WinSparkle.dll'))) { $isolationProblems += 'legacy WinSparkle.dll present' }
Add-Check 'H7' 'S0' ($notInstalled -or $isolationProblems.Count -eq 0) `
  $(if ($isolationProblems.Count) { $isolationProblems -join '; ' } else { 'Stable native identity is isolated; NoWrite:%AppData%\Rime' })

$resourceProblems = @()
if (-not $notInstalled -and -not $noActivePayload) {
  foreach ($relative in @(
    'FamoTextService.dll', 'FamoRuntime.exe', 'FamoRimeEngine.dll', 'FamoProfileTool.exe', 'rime.dll',
    'data\default.yaml', 'data\weasel.yaml', 'data\opencc',
    'settings\FamoSettings.exe', 'settings\FamoSettings.pri')) {
    if (-not (Test-Path -LiteralPath (Join-Path $target $relative))) { $resourceProblems += $relative }
  }
}
Add-Check 'H8' 'S1' ($notInstalled -or $noActivePayload -or
  $resourceProblems.Count -eq 0) `
  $(if ($notInstalled -or $noActivePayload) {
    'active product resources N/A without an active payload'
  } elseif ($resourceProblems.Count) {
    "missing: $($resourceProblems -join ', ')"
  } else {
    'native runtime, engine, data, and settings resources are present'
  })

$localFamo = Join-Path $env:LOCALAPPDATA 'Famo'
$recoveryTask = [pscustomobject]@{
  pass = $false
  detail = 'recovery task proof skipped: journal SID is not the current SID'
}
$seedTransactionClean = $true
$firstRunReady = if ($notInstalled -or $noActivePayload) {
  $true
} elseif (-not $exactUserContext) {
  $false
} elseif ($isPending) {
  $recoveryTask = Test-RecoveryTask $journalInfo
  (-not $runValue) -and $recoveryTask.pass
} else {
  if ($journalPhase -eq 'Ready' -and
      [string]$journalInfo.record.SeedReceiptHash) {
    $seedTransactionRoot = Join-Path (
      Join-Path $localFamo '.transactions') $journalInfo.id
    $seedTransactionClean = -not (Test-Path -LiteralPath $seedTransactionRoot)
  }
  (Same-Path $runValue $server) -and
    (Test-Path -LiteralPath (Join-Path $localFamo 'famo-settings.json')) -and
    (Test-Path -LiteralPath (Join-Path $localFamo 'build\default.yaml')) -and
    $seedTransactionClean
}
Add-Check 'H9' $(if ($isPending) { 'S0' } else { 'S1' }) ($notInstalled -or [bool]$firstRunReady) `
  $(if ($notInstalled -or $noActivePayload) {
    'first-run state N/A without an active payload'
  } elseif (-not $exactUserContext) {
    'per-user first-run probe skipped: journal SID is not the current SID'
  } elseif ($isPending) {
    "Run absent; $($recoveryTask.detail); coherent=$firstRunReady"
  } else {
    "Run=$runValue; first-run settings/build and seed transaction clean=$firstRunReady"
  })

$readyRecoveryInstaller = if ($journalInfo.id) {
  Join-Path $env:ProgramFiles (
    "Famo\pending\$($journalInfo.id)\Famo-Resume-$($journalInfo.id).exe")
} else {
  ''
}
$readyRecoveryDirectory = if ($journalInfo.id) {
  Join-Path $env:ProgramFiles "Famo\pending\$($journalInfo.id)"
} else {
  ''
}
$readyRecoveryInstallerPresent = [bool](
  $readyRecoveryInstaller -and
  (Test-Path -LiteralPath $readyRecoveryInstaller -PathType Leaf))
$readyRecoveryDirectoryPresent = [bool](
  $readyRecoveryDirectory -and
  (Test-Path -LiteralPath $readyRecoveryDirectory -PathType Container))
$readyRecoveryResidual = [bool](
  $journalPhase -eq 'Ready' -and (
    $recoveryCleanupDebtPresent -or
    $readyRecoveryInstallerPresent -or
    $readyRecoveryDirectoryPresent -or
    $recoveryTaskInventory.folderPresent))

$recoveryInventoryOk = if (-not $recoveryTaskInventory.pass) {
  $false
} elseif ($journalPhase -eq 'Ready') {
  -not $readyRecoveryResidual
} elseif ($recoveryCleanupDebtPresent -and -not $recoveryCleanupDebtBound) {
  $false
} elseif (($isPending -or $recoveryDebtPresent) -and $journalInfo.record) {
  $expectedFolderSddl = 'D:P(A;;FA;;;SY)(A;;FA;;;BA)'
  $inventoryShapeOk = $recoveryTaskInventory.folderPresent -and
    $recoveryTaskInventory.folderSddl -ceq $expectedFolderSddl -and
    $recoveryTaskInventory.names.Count -eq 1 -and
    $recoveryTaskInventory.subfolders.Count -eq 0 -and
    [string]::Equals(
      [string]$recoveryTaskInventory.names[0],
      [string]$journalInfo.record.ResumeTaskName,
      [System.StringComparison]::OrdinalIgnoreCase)
  if ($inventoryShapeOk -and $exactUserContext) {
    if (-not $isPending) {
      $recoveryTask = Test-RecoveryTask $journalInfo
    }
    $inventoryShapeOk -and $recoveryTask.pass
  } else {
    $inventoryShapeOk
  }
} else {
  -not $recoveryTaskInventory.folderPresent -and
    -not $readyRecoveryInstallerPresent -and
    -not $readyRecoveryDirectoryPresent
}
Add-Check 'H10' 'S0' ([bool]$recoveryInventoryOk) `
  ("{0}; readyCleanupDebt={1}; debtBound={2}; retainedInstaller={3}; retainedDirectory={4}" -f
    $recoveryTaskInventory.detail,
    [bool]$recoveryCleanupDebtPresent,
    [bool]$recoveryCleanupDebtBound,
    $readyRecoveryInstallerPresent,
    $readyRecoveryDirectoryPresent)

Add-Check 'H11' 'S1' (-not $cleanupDebtPresent) `
  $(if ($cleanupDebtPresent) {
    "cleanup debt present: names=$($debtNames -join ','); count=$($debtNames.Count)"
  } else {
    'cleanup debt absent'
  })

function Check-Passed {
  param([string] $Id)
  $hit = @($results | Where-Object { $_.id -eq $Id } | Select-Object -First 1)
  return $hit.Count -gt 0 -and [bool]$hit[0].pass
}

$healthState = if ($notInstalled) {
  'NotInstalled'
} elseif ($wrongUserContext) {
  'Unsupported'
} elseif (-not (Check-Passed 'H10')) {
  'Broken'
} elseif ($installState -eq 'PendingReboot') {
  'PendingReboot'
} elseif (-not (Check-Passed 'H2') -or -not (Check-Passed 'H7')) {
  'WrongIdentity'
} elseif (-not (Check-Passed 'H1') -or -not (Check-Passed 'H3') -or -not (Check-Passed 'H3b') -or
        -not (Check-Passed 'H4') -or -not (Check-Passed 'H5')) {
  'Broken'
} elseif (-not (Check-Passed 'H6') -and $pipeProbe.state -eq 'Hung') {
  'Hung'
} elseif (-not (Check-Passed 'H6')) {
  'InstalledStopped'
} elseif (-not (Check-Passed 'H11')) {
  'Degraded'
} elseif ($installState -eq 'RolledBack') {
  'RolledBack'
} elseif (-not (Check-Passed 'H8') -or -not (Check-Passed 'H9')) {
  'Degraded'
} else {
  'Ready'
}

foreach ($result in $results) {
  $result | Add-Member -NotePropertyName healthState -NotePropertyValue $healthState -Force
}

if ($Json) {
  ConvertTo-Json -InputObject $results.ToArray() -Depth 4
} else {
  foreach ($result in $results) {
    $mark = if ($result.pass) { 'PASS' } else { 'FAIL' }
    Write-Host ('[{0}] {1} ({2}) {3}' -f $result.id, $mark, $result.severity, $result.detail)
  }
  $failed = @($results | Where-Object { -not $_.pass }).Count
  Write-Host ('[STATE] {0}' -f $healthState)
  Write-Host ('-- {0}/{1} checks passed --' -f ($results.Count - $failed), $results.Count)
}

$s0Failed = @($results | Where-Object { -not $_.pass -and $_.severity -eq 'S0' })
if ($s0Failed.Count -gt 0) { exit 1 } else { exit 0 }
