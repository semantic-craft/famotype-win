; Transactional Famo installer. build-installer.ps1 supplies all public defines.

#define AppName       "法墨输入法"
#define AppNameEN     "Famo"
#ifndef AppVersion
  #define AppVersion  "1.5.9"
#endif
#ifndef ManifestPrefix
  #define ManifestPrefix "UNSET"
#endif
#ifndef ManifestHash
  #define ManifestHash "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#ifndef Identity
  #define Identity "Stable"
#endif
#ifndef BridgeAbi
  #define BridgeAbi "5"
#endif
#ifndef BridgeHash
  #define BridgeHash "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#ifndef BridgeProtocolMin
  #define BridgeProtocolMin "2"
#endif
#ifndef BridgeProtocolMax
  #define BridgeProtocolMax "2"
#endif
#define AppPublisher  "Famo"
#define StagingDir    "staging"
#define SetupIconFile "..\settings-winui\FamoSettings\Assets\famo.ico"

[Setup]
AppId={{F0A12B34-FAA0-4F0E-9C7A-FAMO00010000}
AppName={#AppName}
AppVerName={#AppName} {#AppVersion}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppNameEN}
DisableDirPage=yes
UsePreviousAppDir=no
DisableProgramGroupPage=yes
OutputDir=dist
OutputBaseFilename={#AppNameEN}-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
SetupMutex=FamoInstallerTransactionV2,Global\FamoInstallerTransactionV2
CloseApplications=no
RestartApplications=no
SetupLogging=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile={#StagingDir}\payload\licenses\LICENSE
SetupIconFile={#SetupIconFile}
UninstallDisplayIcon={code:GetActiveSettings}

[Languages]
Name: "zh"; MessagesFile: "compiler:Default.isl"

[Messages]
FinishedRestartLabel=法墨的新版本文件已经安装，但旧输入法模块仍被 Windows 占用。必须重新启动电脑才能完成切换并显示新输入法。是否现在重启？
FinishedRestartMessage=法墨的新版本文件已经安装，但旧输入法模块仍被 Windows 占用。必须重新启动电脑才能完成切换并显示新输入法。%n%n是否现在重启？

[Files]
; Every repair extracts a complete payload to a fresh immutable transaction target.
Source: "{#StagingDir}\payload\*"; DestDir: "{code:GetTransactionTarget}"; Flags: recursesubdirs createallsubdirs ignoreversion uninsrestartdelete; Check: ShouldInstallPayload
; Extracted on demand before payload installation so the original-user token can
; authenticate to the elevated transaction coordinator.
Source: "{#StagingDir}\payload\FamoProfileTool.exe"; Flags: dontcopy noencryption; DestName: "FamoIdentityBroker.exe"
Source: "{#StagingDir}\payload\payload-manifest.txt"; Flags: dontcopy noencryption; DestName: "FamoEmbeddedManifest.txt"
; The TSF DLL is an independent, frozen artifact. Routine Runtime upgrades
; leave this path and its bytes untouched.
Source: "{#StagingDir}\bridge\FamoTextService.dll"; DestDir: "{code:GetBridgeDirectory}"; Flags: ignoreversion onlyifdoesntexist uninsrestartdelete; Check: ShouldInstallBridge
Source: "{#StagingDir}\bridge\bridge-manifest.txt"; DestDir: "{code:GetBridgeDirectory}"; Flags: ignoreversion onlyifdoesntexist; Check: ShouldInstallBridge

[Icons]
Name: "{autoprograms}\法墨设置"; Filename: "{code:GetActiveSettings}"; IconFilename: "{code:GetActiveSettings}"; Check: ShouldInstallPayload

[Code]
type
  TFamoByHandleFileInformation = record
    FileAttributes: Cardinal;
    CreationTime: TFileTime;
    LastAccessTime: TFileTime;
    LastWriteTime: TFileTime;
    VolumeSerialNumber: Cardinal;
    FileSizeHigh: Cardinal;
    FileSizeLow: Cardinal;
    NumberOfLinks: Cardinal;
    FileIndexHigh: Cardinal;
    FileIndexLow: Cardinal;
  end;
  TTransactionJournal = record
    Generation: String;
    Phase: String;
    Version: String;
    Transaction: String;
    ManifestHash: String;
    PendingTarget: String;
    PendingFinalTarget: String;
    PendingObjectId: String;
    PreviousTarget: String;
    PreviousFinalTarget: String;
    PreviousObjectId: String;
    PriorPreviousTarget: String;
    PriorPreviousFinalTarget: String;
    PriorPreviousObjectId: String;
    PreviousManifest: String;
    PreviousManifestHash: String;
    PreviousDefault: String;
    PreviousHost: String;
    PreviousServer: String;
    PreviousProfileTool: String;
    PreviousVersion: String;
    PreviousIdentity: String;
    PreviousTransactionId: String;
    PreviousCompatibilityTransactionId: String;
    PreviousState: String;
    PreviousProfileActive: String;
    PreviousProfileEnabled: String;
    PreviousInputTipPresent: String;
    SeedReceiptHash: String;
    OriginalUserSid: String;
    OriginalUserAccount: String;
    OriginalUserSession: String;
    LastProofSession: String;
    OriginalUserResumeCapable: String;
    ResumeInstaller: String;
    ResumeInstallerHash: String;
    ResumeTaskName: String;
    AllowDowngrade: String;
    LoadedHostHash: String;
    LoadedHostVersion: String;
    LoadedHostExpectedHash: String;
  end;

const
  FamoRootKey = 'Software\Famo';
  BrandKey = 'Software\Famo\InputMethod';
  UninstallDeleteAnchorKey = 'Software\Famo\UninstallRecovery';
  UninstallDeleteAnchorSchema = 'famo-uninstall-delete-anchor-v1';
  JournalVersion = '2';
  DebtSchema = 'famo-debt-v2';
  DebtKindSeedCommit = 'seed-commit';
  DebtKindUserRollback = 'user-rollback';
  DebtKindTargetCleanup = 'target-cleanup';
  DebtKindRecoveryArtifacts = 'recovery-artifacts';
  DebtKindVersionRetention = 'version-retention';
  DebtKindIdentityHelper = 'identity-helper';
  DebtKindMachineCleanupHelper = 'machine-cleanup-helper';
  HelperCleanupDebtName = 'HelperCleanupDebt';
  UninstallIntentSchema = 'famo-uninstall-intent-v1';
  PhasePrepared = 'Prepared';
  PhasePayloadVerified = 'PayloadVerified';
  PhaseResumeArmed = 'ResumeArmed';
  PhaseDetachIntent = 'DetachIntent';
  PhasePendingReboot = 'PendingReboot';
  PhaseActivateIntent = 'ActivateIntent';
  PhaseMachineRegistered = 'MachineRegistered';
  PhaseUserStateIntent = 'UserStateIntent';
  PhaseUserStatePrepared = 'UserStatePrepared';
  PhaseUserStateApplied = 'UserStateApplied';
  PhaseVerifyIntent = 'VerifyIntent';
  PhaseRollbackIntent = 'RollbackIntent';
  PhaseReady = 'Ready';
  PhaseRolledBack = 'RolledBack';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  StableClsid = '{54EAD76A-B864-4A6D-9C82-148E3352BEE7}';
  StateReady = 'Ready';
  StateRolledBack = 'RolledBack';
  StatePendingReboot = 'PendingReboot';
  StateNotInstalled = 'NotInstalled';
  FileAttributeDirectory = $10;
  FileAttributeNormal = $80;
  FileAttributeReparsePoint = $400;
  FileFlagBackupSemantics = $02000000;
  FileFlagOpenReparsePoint = $00200000;
  FileShareRead = $1;
  FileShareWrite = $2;
  FileShareDelete = $4;
  GenericWrite = $40000000;
  OpenExisting = 3;
  FinalPathBufferChars = 32768;
  InvalidHandleValue = -1;
  InvalidFileAttributes = $FFFFFFFF;
  ErrorFileNotFound = 2;
  ErrorPathNotFound = 3;
  ErrorAlreadyExists = 183;
  EarlyTransactionMutexName =
    'Global\FamoInstallerEarlyTransactionV2';
  KeyRead = $20019;
  KeyWow6464Key = $0100;
  DaclSecurityInformation = $4;

var
  TransactionId: String;
  TransactionTarget: String;
  PreviousTarget: String;
  PreviousManifest: String;
  PreviousManifestHash: String;
  PreviousDefault: String;
  PreviousState: String;
  PreviousHost: String;
  PreviousBridgePath: String;
  PreviousBridgeHash: String;
  PreviousBridgeAbi: String;
  PreviousServer: String;
  PreviousProfileTool: String;
  PreviousVersion: String;
  PreviousIdentity: String;
  PreviousTransactionId: String;
  PreviousCompatibilityTransactionId: String;
  PreviousProfileActive: Boolean;
  PreviousProfileEnabled: Boolean;
  PreviousInputTipPresent: Boolean;
  SeedReceiptHash: String;
  OriginalUserSid: String;
  OriginalUserAccount: String;
  OriginalUserSession: String;
  CurrentOriginalUserSession: String;
  OriginalUserResumeCapable: Boolean;
  JournalPhase: String;
  JournalAppVersion: String;
  JournalManifestHash: String;
  JournalPendingFinalTarget: String;
  JournalPendingObjectId: String;
  JournalPreviousFinalTarget: String;
  JournalPreviousObjectId: String;
  PriorPreviousTarget: String;
  JournalPriorPreviousFinalTarget: String;
  JournalPriorPreviousObjectId: String;
  JournalResumeInstaller: String;
  JournalResumeInstallerHash: String;
  SetupSourcePath: String;
  SetupSourceHash: String;
  SetupSourceFinalPath: String;
  SetupSourceObjectId: String;
  JournalTaskName: String;
  JournalAllowDowngrade: Boolean;
  JournalGeneration: Integer;
  LoadedHostDetected: Boolean;
  LoadedHostHash: String;
  LoadedHostVersion: String;
  LoadedHostExpectedHash: String;
  CurrentPayloadProofValid: Boolean;
  CurrentPayloadProofFinalTarget: String;
  CurrentPayloadProofObjectId: String;
  CurrentPayloadProofManifestFinalPath: String;
  CurrentPayloadProofManifestObjectId: String;
  CurrentPayloadProofManifestHash: String;
  ResumeMode: Boolean;
  RollbackMode: Boolean;
  PendingTerminal: Boolean;
  TransactionPrepared: Boolean;
  RegistrationSwitched: Boolean;
  InstallReady: Boolean;
  RollbackComplete: Boolean;
  RuntimeStarted: Boolean;
  DeleteUserData: Boolean;
  UninstallPrepared: Boolean;
  UninstallRestartPending: Boolean;
  UninstallDeleteArmed: Boolean;
  UninstallOwner: String;
  TerminalRecoveryTargetDeleteBlocked: Boolean;
  EarlyTransactionMutex: THandle;

function GetFileAttributesW(FileName: String): Cardinal;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function CreateMutexW(SecurityAttributes: INT_PTR; InitialOwner: BOOL;
  Name: String): THandle;
  external 'CreateMutexW@kernel32.dll stdcall';

procedure SetLastError(ErrorCode: Cardinal);
  external 'SetLastError@kernel32.dll stdcall';

function CreateFileW(FileName: String; DesiredAccess, ShareMode: Cardinal;
  SecurityAttributes: INT_PTR; CreationDisposition, FlagsAndAttributes: Cardinal;
  TemplateFile: THandle): THandle;
  external 'CreateFileW@kernel32.dll stdcall';

function GetFinalPathNameByHandleW(FileHandle: THandle; FilePath: String;
  FilePathChars, Flags: Cardinal): Cardinal;
  external 'GetFinalPathNameByHandleW@kernel32.dll stdcall';

function GetFileInformationByHandle(FileHandle: THandle;
  var FileInformation: TFamoByHandleFileInformation): BOOL;
  external 'GetFileInformationByHandle@kernel32.dll stdcall';

function GetFileSizeEx(FileHandle: THandle; var FileSize: Int64): BOOL;
  external 'GetFileSizeEx@kernel32.dll stdcall';

function FlushFileBuffers(FileHandle: THandle): BOOL;
  external 'FlushFileBuffers@kernel32.dll stdcall';

function CloseHandle(Handle: THandle): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

function CoCreateGuid(var Guid: TGUID): HResult;
  external 'CoCreateGuid@ole32.dll stdcall';

function StringFromGUID2(var Guid: TGUID; Buffer: String;
  BufferChars: Integer): Integer;
  external 'StringFromGUID2@ole32.dll stdcall';

function RegOpenKeyExW(Key: Integer; SubKey: String; Options,
  DesiredAccess: Cardinal; var ResultKey: THandle): LongInt;
  external 'RegOpenKeyExW@advapi32.dll stdcall';

function RegFlushKey(Key: THandle): LongInt;
  external 'RegFlushKey@advapi32.dll stdcall';

function RegCloseKey(Key: THandle): LongInt;
  external 'RegCloseKey@advapi32.dll stdcall';

function ConvertStringSidToSidW(StringSid: String;
  var Sid: INT_PTR): BOOL;
  external 'ConvertStringSidToSidW@advapi32.dll stdcall';

function IsValidSid(Sid: INT_PTR): BOOL;
  external 'IsValidSid@advapi32.dll stdcall';

function LocalFree(Memory: INT_PTR): INT_PTR;
  external 'LocalFree@kernel32.dll stdcall';

function AcquireEarlyTransactionMutex: Boolean;
var
  Candidate: THandle;
  ErrorCode: LongInt;
begin
  Result := EarlyTransactionMutex <> 0;
  if Result then Exit;

  SetLastError(0);
  Candidate := CreateMutexW(0, False, EarlyTransactionMutexName);
  if Candidate = 0 then
  begin
    Log('cannot create the early installer transaction mutex');
    Exit;
  end;
  ErrorCode := DLLGetLastError;
  if ErrorCode = ErrorAlreadyExists then
  begin
    CloseHandle(Candidate);
    Log('another Famo setup or uninstall transaction is active');
    Exit;
  end;
  EarlyTransactionMutex := Candidate;
  Result := True;
end;

procedure ReleaseEarlyTransactionMutex;
begin
  if EarlyTransactionMutex <> 0 then
  begin
    CloseHandle(EarlyTransactionMutex);
    EarlyTransactionMutex := 0;
  end;
end;

function NewIdentityNonce: String;
var
  Guid: TGUID;
  Text: String;
begin
  if CoCreateGuid(Guid) <> 0 then
    RaiseException('cannot generate original-user identity nonce');
  SetLength(Text, 40);
  if StringFromGUID2(Guid, Text, 40) <= 0 then
    RaiseException('cannot format original-user identity nonce');
  Text := Copy(Text, 2, 36);
  StringChangeEx(Text, '-', '', True);
  if Length(Text) <> 32 then
    RaiseException('invalid original-user identity nonce');
  Result := Lowercase(Text);
end;

function FixedInstallRoot: String; forward;
function FixedBridgeDirectory: String; forward;
function FixedBridgeDll: String; forward;
function TransactionChangedBridge: Boolean; forward;
function PathIsNonReparseOrMissing(const Path: String): Boolean; forward;
function TryParseManagedStableBridgePath(
  const Path: String; var BridgeAbi: Integer): Boolean; forward;

function EnsureTransactionTarget: String;
begin
  if TransactionId = '' then
    TransactionId := NewIdentityNonce;
  if TransactionTarget = '' then
    TransactionTarget := AddBackslash(FixedInstallRoot) +
      'versions\{#AppVersion}-{#ManifestPrefix}-' + TransactionId;
  Result := TransactionTarget;
end;

function GetTransactionTarget(Param: String): String;
begin
  Result := EnsureTransactionTarget;
end;

function ShouldInstallPayload: Boolean;
begin
  Result := not RollbackMode;
end;

function GetBridgeDirectory(Param: String): String;
begin
  Result := FixedBridgeDirectory;
end;

function ShouldInstallBridge: Boolean;
begin
  Result := not RollbackMode and TransactionChangedBridge;
end;

function ReadActiveTarget: String;
begin
  if not RegQueryStringValue(HKLM64, BrandKey, 'InstallDir', Result) then
    Result := '';
end;

function GetActiveTarget(Param: String): String;
begin
  if TransactionTarget <> '' then
    Result := TransactionTarget
  else
    Result := ReadActiveTarget;
end;

function GetActiveSettings(Param: String): String;
begin
  Result := AddBackslash(GetActiveTarget('')) + 'settings\FamoSettings.exe';
end;

function ValidatePreviousPayloadForExecution: Boolean; forward;
function ValidateCurrentPayloadForExecution: Boolean; forward;

function ValidateManagedExecutableForExecution(
  const FileName: String): Boolean;
var
  CurrentRoot, PreviousRoot: String;
begin
  Result := True;
  if TransactionTarget <> '' then
  begin
    CurrentRoot := AddBackslash(TransactionTarget);
    if (CompareText(FileName, CurrentRoot + 'FamoProfileTool.exe') = 0) or
       (CompareText(FileName, CurrentRoot + 'FamoRuntime.exe') = 0) or
       (CompareText(FileName,
         CurrentRoot + 'settings\FamoSettings.exe') = 0) then
    begin
      Result := ValidateCurrentPayloadForExecution;
      Exit;
    end;
  end;
  if PreviousTarget <> '' then
  begin
    PreviousRoot := AddBackslash(PreviousTarget);
    if (CompareText(FileName, PreviousRoot + 'FamoProfileTool.exe') = 0) or
       (CompareText(FileName, PreviousRoot + 'FamoRuntime.exe') = 0) or
       (CompareText(FileName,
         PreviousRoot + 'settings\FamoSettings.exe') = 0) then
      Result := ValidatePreviousPayloadForExecution;
  end;
end;

function BuildBoundDesktopParameters(const FileName, Parameters: String;
  WaitForExit: Boolean; var Broker, BrokerParameters: String): Boolean;
var
  Kind, Operation, Leaf, WaitMode: String;
begin
  Result := False;
  if (OriginalUserSid = '') or (TransactionTarget = '') then Exit;
  Broker := AddBackslash(TransactionTarget) + 'FamoProfileTool.exe';
  if not FileExists(Broker) then Exit;
  Leaf := ExtractFileName(FileName);
  Kind := '';
  Operation := '';
  if CompareText(Leaf, 'FamoProfileTool.exe') = 0 then
  begin
    Kind := 'profile';
    if Parameters = 'clear-user-com-shadow ' + OriginalUserSid then
      Operation := 'clear-user-com-shadow'
    else if (Parameters = 'check') or (Parameters = 'check-disabled') or
            (Parameters = 'is-active') or (Parameters = 'is-enabled') or
            (Parameters = 'switch-away') or (Parameters = 'enable') or
            (Parameters = 'disable') or (Parameters = 'activate') or
            (Parameters = 'cleanup-user-state') then
      Operation := Parameters;
  end
  else if CompareText(Leaf, 'FamoSettings.exe') = 0 then
  begin
    Kind := 'settings';
    if Parameters = '--seed-only' then Operation := 'seed'
    else if Parameters = '--seed-only --no-activate' then
      Operation := 'seed-no-activate'
    else if Parameters = '--prepare-seed-transaction ' + TransactionId then
      Operation := 'prepare-seed-transaction-' + TransactionId
    else if Parameters = '--apply-seed-transaction ' + TransactionId + ' ' +
            SeedReceiptHash then
      Operation := 'apply-seed-transaction-' + TransactionId + '-' +
        SeedReceiptHash
    else if Parameters = '--apply-seed-transaction ' + TransactionId + ' ' +
            SeedReceiptHash +
            ' --no-activate' then
      Operation := 'apply-seed-transaction-no-activate-' + TransactionId +
        '-' + SeedReceiptHash
    else if Parameters = '--rollback-seed-transaction ' + TransactionId +
            ' ' + SeedReceiptHash then
      Operation := 'rollback-seed-transaction-' + TransactionId + '-' +
        SeedReceiptHash
    else if Parameters = '--commit-seed-transaction ' + TransactionId +
            ' ' + SeedReceiptHash then
      Operation := 'commit-seed-transaction-' + TransactionId + '-' +
        SeedReceiptHash
    else if Parameters = '--discard-seed-transaction ' + TransactionId then
      Operation := 'discard-seed-transaction-' + TransactionId
    else if Parameters = '--is-input-tip' then Operation := 'is-input-tip'
    else if Parameters = '--add-input-tip' then Operation := 'add-input-tip'
    else if Parameters = '--remove-input-tip' then
      Operation := 'remove-input-tip';
  end
  else if CompareText(Leaf, 'FamoRuntime.exe') = 0 then
  begin
    Kind := 'runtime';
    if Parameters = '' then Operation := 'start'
    else if Parameters = '--control shutdown' then Operation := 'shutdown'
    else if Parameters = '/q' then Operation := 'quit'
    else if Parameters = '--control deploy' then Operation := 'deploy'
    else if Parameters = '--control reload-options' then
      Operation := 'reload-options';
  end;
  if (Kind = '') or (Operation = '') then Exit;
  if WaitForExit then WaitMode := 'wait' else WaitMode := 'nowait';
  BrokerParameters := 'desktop-run-for ' + OriginalUserSid + ' ' +
    WaitMode + ' ' + Kind + ' ' + Operation + ' ' + AddQuotes(FileName);
  Result := True;
end;

function RunBoundDesktopExitCode(const FileName, Parameters: String;
  WaitForExit: Boolean): Integer;
var
  Broker, BrokerParameters: String;
begin
  Result := -1;
  { ValidateManagedExecutableForExecution proves the complete current payload
    for every supported desktop executable, including the embedded broker.
    Calling the full proof separately here doubled every 641-file scan. }
  if not ValidateManagedExecutableForExecution(FileName) then
    Exit;
  if not BuildBoundDesktopParameters(FileName, Parameters, WaitForExit,
       Broker, BrokerParameters) then
    Exit;
  if not Exec(Broker, BrokerParameters, '', SW_HIDE, ewWaitUntilTerminated,
       Result) then
    Result := -1;
end;

function RunAndRequire(const FileName, Parameters: String; OriginalUser: Boolean): Boolean;
var
  ResultCode: Integer;
begin
  if OriginalUser then
  begin
    ResultCode := RunBoundDesktopExitCode(FileName, Parameters, True);
    Result := ResultCode >= 0;
  end
  else
  begin
    Result := ValidateManagedExecutableForExecution(FileName) and
      Exec(FileName, Parameters, '', SW_HIDE,
        ewWaitUntilTerminated, ResultCode);
  end;
  Result := Result and (ResultCode = 0);
end;

function RunExitCode(const FileName, Parameters: String): Integer;
begin
  Result := -1;
  if not ValidateManagedExecutableForExecution(FileName) or
     not Exec(FileName, Parameters, '', SW_HIDE,
       ewWaitUntilTerminated, Result) then
    Result := -1;
end;

function RunAsOriginalUserExitCode(const FileName, Parameters: String): Integer;
begin
  Result := RunBoundDesktopExitCode(FileName, Parameters, True);
end;

function ProfileTool(const Target: String): String;
begin
  Result := AddBackslash(Target) + 'FamoProfileTool.exe';
end;

procedure FailIfRequested(const Phase: String);
begin
  if CompareText(ExpandConstant('{param:FamoFail|}'), Phase) = 0 then
    RaiseException('injected transaction failure: ' + Phase);
end;

function RegisterTarget(const Target: String): Boolean;
begin
  Result := FileExists(FixedBridgeDll) and
    (CompareText(GetSHA256OfFile(FixedBridgeDll), '{#BridgeHash}') = 0) and
    RunAndRequire(ProfileTool(Target),
      'register-machine-at ' + AddQuotes(FixedBridgeDll), False) and
    RunAndRequire(ProfileTool(Target), 'check-machine', False);
end;

function MachineComPointsToTarget(const Target: String): Boolean;
var
  RegisteredDll: String;
begin
  Result := RegQueryStringValue(HKLM64,
    'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '',
    RegisteredDll) and
    (CompareText(RegisteredDll, FixedBridgeDll) = 0);
end;

function UnregisterTarget(const Target: String): Boolean;
begin
  if not MachineComPointsToTarget(Target) then
  begin
    Result := True;
    Exit;
  end;
  Result := FileExists(ProfileTool(Target)) and
    RunAndRequire(ProfileTool(Target),
      'unregister-machine-at ' + AddQuotes(FixedBridgeDll), False) and
    not MachineComPointsToTarget(Target);
end;

function UnregisterMachineTarget(const Target: String): Boolean;
begin
  Result := True;
  if FileExists(ProfileTool(Target)) then
    Result := RunAndRequire(ProfileTool(Target),
      'unregister-machine-at ' + AddQuotes(FixedBridgeDll), False);
end;

procedure WriteOrDelete(const Name, Value: String);
begin
  if Value <> '' then
    RegWriteStringValue(HKLM64, BrandKey, Name, Value)
  else
    RegDeleteValue(HKLM64, BrandKey, Name);
end;

procedure ClearPendingRegistry;
begin
  RegDeleteValue(HKLM64, BrandKey, 'PendingTarget');
  RegDeleteValue(HKLM64, BrandKey, 'PendingManifest');
  RegDeleteValue(HKLM64, BrandKey, 'PendingVersion');
  RegDeleteValue(HKLM64, BrandKey, 'PendingReason');
  RegDeleteValue(HKLM64, BrandKey, 'ResumeInstaller');
  RegDeleteValue(HKLM64, BrandKey, 'LoadedHostPath');
  RegDeleteValue(HKLM64, BrandKey, 'LoadedHostHash');
  RegDeleteValue(HKLM64, BrandKey, 'LoadedHostVersion');
  RegDeleteValue(HKLM64, BrandKey, 'LoadedHostExpectedHash');
  RegDeleteValue(HKLM64, BrandKey, 'PreviousHost');
  RegDeleteValue(HKLM64, BrandKey, 'PreviousManifest');
  RegDeleteValue(HKLM64, BrandKey, 'PreviousServer');
  RegDeleteValue(HKLM64, BrandKey, 'PreviousProfileTool');
  RegDeleteValue(HKLM64, BrandKey, 'PreviousVersion');
  RegDeleteValue(HKLM64, BrandKey, 'PreviousIdentity');
  RegDeleteValue(HKLM64, BrandKey, 'PreviousProfileActive');
end;

procedure WriteActiveRegistry(const Target, State: String);
begin
  RegWriteStringValue(HKLM64, BrandKey, 'InstallDir', Target);
  RegWriteStringValue(HKLM64, BrandKey, 'ServerExecutable', AddBackslash(Target) + 'FamoRuntime.exe');
  RegWriteStringValue(HKLM64, BrandKey, 'ProfileTool', ProfileTool(Target));
  RegWriteStringValue(HKLM64, BrandKey, 'ActiveManifest', AddBackslash(Target) + 'payload-manifest.txt');
  RegWriteStringValue(HKLM64, BrandKey, 'BridgePath', FixedBridgeDll);
  RegWriteStringValue(HKLM64, BrandKey, 'BridgeHash', '{#BridgeHash}');
  RegWriteStringValue(HKLM64, BrandKey, 'BridgeAbi', '{#BridgeAbi}');
  RegWriteStringValue(HKLM64, BrandKey, 'BridgeProtocolMin',
    '{#BridgeProtocolMin}');
  RegWriteStringValue(HKLM64, BrandKey, 'BridgeProtocolMax',
    '{#BridgeProtocolMax}');
  RegWriteStringValue(HKLM64, BrandKey, 'ActiveVersion', '{#AppVersion}');
  RegWriteStringValue(HKLM64, BrandKey, 'Identity', '{#Identity}');
  RegWriteStringValue(HKLM64, BrandKey, 'TransactionId', TransactionId);
  RegWriteStringValue(HKLM64, BrandKey, 'PreviousTarget', PreviousTarget);
  RegWriteStringValue(HKLM64, BrandKey, 'PreviousDefault', PreviousDefault);
  RegWriteStringValue(HKLM64, BrandKey, 'InstallState', State);
  RegWriteStringValue(HKLM64, RunKey, 'FamoRuntime', AddQuotes(AddBackslash(Target) + 'FamoRuntime.exe'));
end;

procedure RestorePreviousRegistry;
var
  PreviousManagedBridgeAbi: Integer;
begin
  if PreviousTarget <> '' then
  begin
    RegWriteStringValue(HKLM64, BrandKey, 'InstallDir', PreviousTarget);
    WriteOrDelete('ServerExecutable', PreviousServer);
    WriteOrDelete('ProfileTool', PreviousProfileTool);
    WriteOrDelete('ActiveManifest', PreviousManifest);
    WriteOrDelete('ActiveVersion', PreviousVersion);
    WriteOrDelete('Identity', PreviousIdentity);
    WriteOrDelete('PreviousTarget', PriorPreviousTarget);
    RegWriteStringValue(HKLM64, BrandKey, 'PreviousDefault', PreviousDefault);
    if PreviousServer <> '' then
      RegWriteStringValue(HKLM64, RunKey, 'FamoRuntime', AddQuotes(PreviousServer))
    else
      RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
  end
  else
  begin
    RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
    { The immutable transaction journal lives below BrandKey. Never delete the
      key tree while rollback is still in progress; clear only compatibility
      projection values. }
    RegDeleteValue(HKLM64, BrandKey, 'InstallDir');
    RegDeleteValue(HKLM64, BrandKey, 'ServerExecutable');
    RegDeleteValue(HKLM64, BrandKey, 'ProfileTool');
    RegDeleteValue(HKLM64, BrandKey, 'ActiveManifest');
    RegDeleteValue(HKLM64, BrandKey, 'ActiveVersion');
    RegDeleteValue(HKLM64, BrandKey, 'Identity');
    WriteOrDelete('PreviousTarget', PriorPreviousTarget);
    RegWriteStringValue(HKLM64, BrandKey, 'PreviousDefault', PreviousDefault);
  end;
  if TryParseManagedStableBridgePath(
       PreviousHost, PreviousManagedBridgeAbi) then
  begin
    RegWriteStringValue(HKLM64, BrandKey, 'BridgePath', PreviousHost);
    RegWriteStringValue(HKLM64, BrandKey, 'BridgeHash',
      LoadedHostExpectedHash);
    RegWriteStringValue(HKLM64, BrandKey, 'BridgeAbi',
      IntToStr(PreviousManagedBridgeAbi));
  end
  else
  begin
    RegDeleteValue(HKLM64, BrandKey, 'BridgePath');
    RegDeleteValue(HKLM64, BrandKey, 'BridgeHash');
    RegDeleteValue(HKLM64, BrandKey, 'BridgeAbi');
  end;
  if CompareText(PreviousHost, FixedBridgeDll) = 0 then
  begin
    RegWriteStringValue(HKLM64, BrandKey, 'BridgeProtocolMin',
      '{#BridgeProtocolMin}');
    RegWriteStringValue(HKLM64, BrandKey, 'BridgeProtocolMax',
      '{#BridgeProtocolMax}');
  end
  else
  begin
    RegDeleteValue(HKLM64, BrandKey, 'BridgeProtocolMin');
    RegDeleteValue(HKLM64, BrandKey, 'BridgeProtocolMax');
  end;
  if (PreviousDefault <> '') and (OriginalUserSid <> '') then
    RegWriteStringValue(HKU, OriginalUserSid + '\Keyboard Layout\Preload',
      '1', PreviousDefault);
  ClearPendingRegistry;
end;

function NormalizeDirectoryPath(const Path: String): String;
begin
  Result := RemoveBackslashUnlessRoot(ExpandFileName(PathNormalizeSlashes(Path)));
end;

function FixedInstallRoot: String;
begin
  Result := NormalizeDirectoryPath(ExpandConstant('{autopf}\Famo'));
end;

function FixedBridgeDirectory: String;
begin
  Result := AddBackslash(FixedInstallRoot) + 'bridge\v{#BridgeAbi}';
end;

function FixedBridgeDll: String;
begin
  Result := AddBackslash(FixedBridgeDirectory) + 'FamoTextService.dll';
end;

procedure VerifyFrozenBridgePreflight;
var
  ExistingHash: String;
begin
  if not FileExists(FixedBridgeDll) then Exit;
  ExistingHash := Uppercase(GetSHA256OfFile(FixedBridgeDll));
  if CompareText(ExistingHash, '{#BridgeHash}') <> 0 then
  begin
    Log('frozen Bridge conflict: path=' + FixedBridgeDll +
      '; expectedHash={#BridgeHash}; actualHash=' + ExistingHash);
    RaiseException(
      'frozen Bridge v{#BridgeAbi} already exists with a different hash');
  end;
end;

function TransactionChangedBridge: Boolean;
begin
  Result :=
    (CompareText(PreviousHost, FixedBridgeDll) <> 0) or
    (CompareText(LoadedHostHash, '{#BridgeHash}') <> 0);
end;

function TryGetPathAttributes(const Path: String; var Exists: Boolean;
  var Attributes: Cardinal): Boolean;
var
  ErrorCode: LongInt;
begin
  Attributes := GetFileAttributesW(Path);
  Exists := Attributes <> InvalidFileAttributes;
  if Exists then
  begin
    Result := True;
    Exit;
  end;
  ErrorCode := DLLGetLastError;
  Result := (ErrorCode = ErrorFileNotFound) or
    (ErrorCode = ErrorPathNotFound);
end;

function NormalizeFinalObjectPath(const Path: String): String;
begin
  Result := PathNormalizeSlashes(Path);
  if PathStartsWith(Result, '\\?\UNC\', True) then
    Result := '\\' + Copy(Result, 9, Length(Result))
  else if PathStartsWith(Result, '\\?\', True) or
          PathStartsWith(Result, '\??\', True) then
    Result := Copy(Result, 5, Length(Result));
  Result := RemoveBackslashUnlessRoot(Result);
end;

function TryGetFinalObjectInfo(const Path: String;
  var FinalPath, ObjectId: String): Boolean;
var
  Attributes, Flags, FinalLength: Cardinal;
  Exists: Boolean;
  FileHandle: THandle;
  Buffer: String;
  FileInformation: TFamoByHandleFileInformation;
begin
  Result := False;
  FinalPath := '';
  ObjectId := '';
  if not TryGetPathAttributes(Path, Exists, Attributes) or not Exists then Exit;

  Flags := FileAttributeNormal;
  if (Attributes and FileAttributeDirectory) <> 0 then
    Flags := Flags or FileFlagBackupSemantics;
  FileHandle := CreateFileW(Path, 0,
    FileShareRead or FileShareWrite or FileShareDelete, 0, OpenExisting,
    Flags, 0);
  if FileHandle = InvalidHandleValue then Exit;
  try
    SetLength(Buffer, FinalPathBufferChars);
    FinalLength := GetFinalPathNameByHandleW(FileHandle, Buffer,
      FinalPathBufferChars, 0);
    if (FinalLength = 0) or (FinalLength >= FinalPathBufferChars) then Exit;
    SetLength(Buffer, FinalLength);
    FinalPath := NormalizeFinalObjectPath(Buffer);
    if FinalPath = '' then Exit;
    if not GetFileInformationByHandle(FileHandle, FileInformation) then Exit;
    if (FileInformation.FileIndexHigh <> 0) or
       (FileInformation.FileIndexLow <> 0) then
      ObjectId := IntToStr(FileInformation.VolumeSerialNumber) + ':' +
        IntToStr(FileInformation.FileIndexHigh) + ':' +
        IntToStr(FileInformation.FileIndexLow);
    Result := True;
  finally
    CloseHandle(FileHandle);
  end;
end;

function FinalObjectsSame(const FirstFinalPath, FirstObjectId,
  SecondFinalPath, SecondObjectId: String): Boolean;
begin
  if (FirstObjectId <> '') and (SecondObjectId <> '') then
    Result := CompareText(FirstObjectId, SecondObjectId) = 0
  else
    Result := PathSame(FirstFinalPath, SecondFinalPath);
end;

function ProtectedPathIsDifferent(const Target: String; TargetExists: Boolean;
  const TargetFinalPath, TargetObjectId, ProtectedPath: String): Boolean;
var
  ProtectedExists: Boolean;
  ProtectedAttributes: Cardinal;
  ProtectedFinalPath, ProtectedObjectId: String;
begin
  Result := False;
  if ProtectedPath = '' then
  begin
    Result := True;
    Exit;
  end;
  if PathSame(NormalizeDirectoryPath(ProtectedPath), Target) then Exit;
  if not TryGetPathAttributes(ProtectedPath, ProtectedExists,
    ProtectedAttributes) then Exit;
  if not ProtectedExists or not TargetExists then
  begin
    Result := True;
    Exit;
  end;
  if not TryGetFinalObjectInfo(ProtectedPath, ProtectedFinalPath,
    ProtectedObjectId) then Exit;
  Result := not FinalObjectsSame(TargetFinalPath, TargetObjectId,
    ProtectedFinalPath, ProtectedObjectId);
end;

function ValidManifestPathSegment(const Segment: String): Boolean;
begin
  Result := False;
  if Segment = '' then Exit;
  if (Segment = '.') or (Segment = '..') then Exit;
  Result := (Segment[Length(Segment)] <> '.') and
    (Segment[Length(Segment)] <> ' ');
end;

function NormalizeSafeRelativePath(const Value: String;
  var NormalizedValue: String): Boolean;
var
  I, SegmentStart: Integer;
  Segment: String;
begin
  Result := False;
  NormalizedValue := PathNormalizeSlashes(Value);
  if Value = '' then Exit;
  if NormalizedValue <> Value then Exit;
  if (Pos(':', Value) <> 0) or (Value[1] = '\') then Exit;
  SegmentStart := 1;
  for I := 1 to Length(NormalizedValue) do
  begin
    if NormalizedValue[I] = '\' then
    begin
      Segment := Copy(NormalizedValue, SegmentStart, I - SegmentStart);
      if not ValidManifestPathSegment(Segment) then Exit;
      SegmentStart := I + 1;
    end;
  end;
  Segment := Copy(NormalizedValue, SegmentStart, Length(NormalizedValue));
  Result := ValidManifestPathSegment(Segment);
end;

function IsSha256Hex(const Value: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  if Length(Value) <> 64 then Exit;
  for I := 1 to Length(Value) do
  begin
    if not (((Value[I] >= '0') and (Value[I] <= '9')) or
            ((Value[I] >= 'A') and (Value[I] <= 'F')) or
            ((Value[I] >= 'a') and (Value[I] <= 'f'))) then
      Exit;
  end;
  Result := True;
end;

function ParseFileEntryDetailed(const Line: String; var RelativePath: String;
  var ExpectedSize: Int64; var ExpectedHash: String): Boolean;
var
  Payload, Rest, RawRelativePath, SizeText: String;
  FirstBar, SecondBar: Integer;
begin
  Result := False;
  ExpectedSize := -1;
  Payload := Copy(Line, 6, Length(Line));
  FirstBar := Pos('|', Payload);
  if FirstBar = 0 then Exit;
  RawRelativePath := Copy(Payload, 1, FirstBar - 1);
  Rest := Copy(Payload, FirstBar + 1, Length(Payload));
  SecondBar := Pos('|', Rest);
  if SecondBar = 0 then Exit;
  SizeText := Copy(Rest, 1, SecondBar - 1);
  ExpectedHash := Copy(Rest, SecondBar + 1, Length(Rest));
  try
    ExpectedSize := StrToInt64(SizeText);
  except
    Exit;
  end;
  if (ExpectedSize < 0) or (SizeText <> IntToStr(ExpectedSize)) then Exit;
  if not NormalizeSafeRelativePath(RawRelativePath, RelativePath) then Exit;
  Result := IsSha256Hex(ExpectedHash);
end;

function ParseFileEntry(const Line: String;
  var RelativePath, ExpectedHash: String): Boolean;
var
  ExpectedSize: Int64;
begin
  Result := ParseFileEntryDetailed(Line, RelativePath, ExpectedSize,
    ExpectedHash);
end;

function TryGetFileSize64(const Path: String; var Size: Int64): Boolean;
var
  FileHandle: THandle;
begin
  Result := False;
  FileHandle := CreateFileW(Path, 0,
    FileShareRead or FileShareWrite or FileShareDelete, 0, OpenExisting,
    FileAttributeNormal, 0);
  if FileHandle = InvalidHandleValue then Exit;
  try
    Result := GetFileSizeEx(FileHandle, Size);
  finally
    CloseHandle(FileHandle);
  end;
end;

function TryParseManagedStableBridgePath(
  const Path: String; var BridgeAbi: Integer): Boolean;
var
  BridgeRoot, BridgeDirectory, AbiLeaf, AbiText,
    RootFinalPath, RootObjectId, DirectoryFinalPath, DirectoryObjectId,
    HostFinalPath, HostObjectId: String;
begin
  Result := False;
  BridgeAbi := 0;
  if (Path = '') or
     (CompareText(ExtractFileName(Path), 'FamoTextService.dll') <> 0) then
    Exit;
  BridgeRoot :=
    NormalizeDirectoryPath(AddBackslash(FixedInstallRoot) + 'bridge');
  BridgeDirectory := NormalizeDirectoryPath(ExtractFileDir(Path));
  AbiLeaf := ExtractFileName(BridgeDirectory);
  if (Length(AbiLeaf) < 2) or
     ((AbiLeaf[1] <> 'v') and (AbiLeaf[1] <> 'V')) then
    Exit;
  AbiText := Copy(AbiLeaf, 2, Length(AbiLeaf));
  BridgeAbi := StrToIntDef(AbiText, 0);
  if (BridgeAbi <= 0) or (AbiText <> IntToStr(BridgeAbi)) or
     not PathSame(ExtractFileDir(BridgeDirectory), BridgeRoot) or
     not PathIsNonReparseOrMissing(BridgeRoot) or
     not PathIsNonReparseOrMissing(BridgeDirectory) or
     not PathIsNonReparseOrMissing(Path) or
     not TryGetFinalObjectInfo(BridgeRoot, RootFinalPath, RootObjectId) or
     not TryGetFinalObjectInfo(
       BridgeDirectory, DirectoryFinalPath, DirectoryObjectId) or
     not TryGetFinalObjectInfo(Path, HostFinalPath, HostObjectId) or
     not PathSame(ExtractFileDir(DirectoryFinalPath), RootFinalPath) or
     not PathSame(ExtractFileDir(HostFinalPath), DirectoryFinalPath) then
  begin
    BridgeAbi := 0;
    Exit;
  end;
  Result := True;
end;

procedure ReadPreviousHostMetadata;
var
  Lines: TArrayOfString;
  I, ManagedBridgeAbi: Integer;
  RelativePath, ExpectedHash: String;
begin
  LoadedHostHash := '';
  LoadedHostVersion := '';
  LoadedHostExpectedHash := '';
  PreviousManifestHash := '';
  if (PreviousHost <> '') and FileExists(PreviousHost) then
  begin
    LoadedHostHash := Uppercase(GetSHA256OfFile(PreviousHost));
    if not GetVersionNumbersString(PreviousHost, LoadedHostVersion) then
      LoadedHostVersion := 'unversioned';
  end;
  if (PreviousManifest <> '') and FileExists(PreviousManifest) then
  begin
    PreviousManifestHash := Uppercase(GetSHA256OfFile(PreviousManifest));
    ManagedBridgeAbi := 0;
    if CompareText(PreviousHost, FixedBridgeDll) = 0 then
      LoadedHostExpectedHash := Uppercase('{#BridgeHash}')
    else if TryParseManagedStableBridgePath(
              PreviousHost, ManagedBridgeAbi) and
            (CompareText(PreviousBridgePath, PreviousHost) = 0) and
            (PreviousBridgeAbi = IntToStr(ManagedBridgeAbi)) and
            IsSha256Hex(PreviousBridgeHash) then
      LoadedHostExpectedHash := Uppercase(PreviousBridgeHash);
    if not LoadStringsFromFile(PreviousManifest, Lines) then
      RaiseException('previous payload manifest unreadable');
    for I := 0 to GetArrayLength(Lines) - 1 do
    begin
      if (PreviousVersion = '') and (Pos('version=', Lines[I]) = 1) then
        PreviousVersion := Copy(Lines[I], 9, Length(Lines[I]));
      if (LoadedHostExpectedHash = '') and
         (Pos('file=', Lines[I]) = 1) and
         ParseFileEntry(Lines[I], RelativePath, ExpectedHash) and
         (CompareText(RelativePath, 'FamoTextService.dll') = 0) then
        LoadedHostExpectedHash := Uppercase(ExpectedHash);
    end;
    if (LoadedHostExpectedHash = '') or
       (CompareText(LoadedHostHash, LoadedHostExpectedHash) <> 0) then
      RaiseException('previous loaded-host path/hash/version does not match its manifest');
  end;
end;

function PreviousBridgeSnapshotValid: Boolean;
var
  ManagedBridgeAbi: Integer;
begin
  Result :=
    (PreviousHost <> '') and
    FileExists(PreviousHost) and
    IsSha256Hex(LoadedHostHash) and
    IsSha256Hex(LoadedHostExpectedHash) and
    (CompareText(LoadedHostHash, LoadedHostExpectedHash) = 0) and
    (TryParseManagedStableBridgePath(
        PreviousHost, ManagedBridgeAbi) or
     ((CompareText(PreviousHost,
        AddBackslash(PreviousTarget) + 'FamoTextService.dll') = 0) and
      (CompareText(PreviousHost, FixedBridgeDll) <> 0)));
end;

function ValidatePreviousV2Transaction(const Id, Target,
  Manifest: String): Boolean; forward;
function IsLegacyRollbackAnchorForProjection(const Id, Target,
  Manifest: String): Boolean; forward;
function IsEmptyRollbackAnchorForProjection(const Id: String): Boolean; forward;
function ValidTransactionId(const Value: String): Boolean; forward;
function ValidLegacyTransactionId(const Value: String): Boolean; forward;
function VerifyManagedPayloadForCleanup(const VersionTarget, VersionFinalPath,
  Manifest, ManifestFinalPath, Version, Prefix: String): Boolean; forward;
function ReadPinnedManagedFileIdentity(const Manifest, RelativeName: String;
  var ExpectedSize: Int64; var ExpectedHash: String): Boolean; forward;
function ValidatePinnedBrokerForExecution(const Broker,
  ExpectedHash, PinnedFinalPath, PinnedObjectId,
  PinnedDirectoryFinalPath, PinnedDirectoryObjectId: String;
  ExpectedSize: Int64): Boolean; forward;
procedure ArmHelperCleanupDebt(const Kind, Nonce: String); forward;
procedure ClearHelperCleanupDebt(const Owner, Kind, Nonce: String); forward;
function CleanupExactHelperAndDebt(
  const Owner, Kind, Nonce: String; MaxDeleteAttempts: Integer;
  var Failure: String): Boolean; forward;

function ValidateLegacyPreviousSnapshot: Boolean;
var
  VersionsRoot, VersionsFinalPath, VersionsObjectId, TargetFinalPath,
    TargetObjectId, ManifestFinalPath, ManifestObjectId,
    ExpectedLeaf: String;
begin
  VersionsRoot := NormalizeDirectoryPath(
    AddBackslash(FixedInstallRoot) + 'versions');
  ExpectedLeaf := PreviousVersion + '-' +
    Copy(PreviousManifestHash, 1, 12) + '-' +
    PreviousCompatibilityTransactionId;
  Result :=
    (PreviousTarget <> '') and
    (PreviousTransactionId = '') and
    ValidLegacyTransactionId(PreviousCompatibilityTransactionId) and
    (PreviousState = StateReady) and
    (CompareText(PreviousManifest,
      AddBackslash(PreviousTarget) + 'payload-manifest.txt') = 0) and
    PreviousBridgeSnapshotValid and
    (CompareText(PreviousProfileTool,
      AddBackslash(PreviousTarget) + 'FamoProfileTool.exe') = 0) and
    (CompareText(PreviousServer,
      AddBackslash(PreviousTarget) + 'FamoRuntime.exe') = 0) and
    IsSha256Hex(PreviousManifestHash) and
    FileExists(PreviousManifest) and
    FileExists(PreviousProfileTool) and FileExists(PreviousServer) and
    TryGetFinalObjectInfo(VersionsRoot, VersionsFinalPath,
      VersionsObjectId) and
    TryGetFinalObjectInfo(PreviousTarget, TargetFinalPath,
      TargetObjectId) and
    TryGetFinalObjectInfo(PreviousManifest, ManifestFinalPath,
      ManifestObjectId) and
    PathSame(ExtractFileDir(TargetFinalPath), VersionsFinalPath) and
    (CompareText(ExtractFileName(TargetFinalPath), ExpectedLeaf) = 0) and
    PathSame(ExtractFileDir(ManifestFinalPath), TargetFinalPath) and
    IsSha256Hex(LoadedHostExpectedHash) and
    (CompareText(LoadedHostHash, LoadedHostExpectedHash) = 0) and
    VerifyManagedPayloadForCleanup(PreviousTarget, TargetFinalPath,
      PreviousManifest, ManifestFinalPath, PreviousVersion,
      Copy(PreviousManifestHash, 1, 12));
  if Result then PreviousIdentity := '{#Identity}';
end;

procedure SnapshotPreviousState;
begin
  PriorPreviousTarget := '';
  PreviousTarget := '';
  PreviousManifest := '';
  PreviousManifestHash := '';
  PreviousDefault := '';
  PreviousState := '';
  PreviousHost := '';
  PreviousBridgePath := '';
  PreviousBridgeHash := '';
  PreviousBridgeAbi := '';
  PreviousServer := '';
  PreviousProfileTool := '';
  PreviousVersion := '';
  PreviousIdentity := '';
  PreviousTransactionId := '';
  PreviousCompatibilityTransactionId := '';
  PreviousProfileActive := False;
  PreviousProfileEnabled := False;
  PreviousInputTipPresent := False;
  SeedReceiptHash := '';
  RegQueryStringValue(HKLM64, BrandKey, 'PreviousTarget',
    PriorPreviousTarget);
  RegQueryStringValue(HKLM64, BrandKey, 'InstallDir', PreviousTarget);
  RegQueryStringValue(HKLM64, BrandKey, 'ActiveManifest', PreviousManifest);
  RegQueryStringValue(HKLM64, BrandKey, 'InstallState', PreviousState);
  RegQueryStringValue(HKLM64, BrandKey, 'ActiveTransactionId',
    PreviousTransactionId);
  RegQueryStringValue(HKLM64, BrandKey, 'TransactionId',
    PreviousCompatibilityTransactionId);
  RegQueryStringValue(HKLM64, BrandKey, 'ServerExecutable', PreviousServer);
  RegQueryStringValue(HKLM64, BrandKey, 'ProfileTool', PreviousProfileTool);
  RegQueryStringValue(HKLM64, BrandKey, 'BridgePath', PreviousBridgePath);
  RegQueryStringValue(HKLM64, BrandKey, 'BridgeHash', PreviousBridgeHash);
  RegQueryStringValue(HKLM64, BrandKey, 'BridgeAbi', PreviousBridgeAbi);
  RegQueryStringValue(HKLM64, BrandKey, 'ActiveVersion', PreviousVersion);
  RegQueryStringValue(HKLM64, BrandKey, 'Identity', PreviousIdentity);
  { Elevated lifecycle code must only consume machine-scoped registration.
    The verified new profile tool removes legacy HKCU shadows during register. }
  PreviousHost := '';
  RegQueryStringValue(HKLM64,
    'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '', PreviousHost);
  if (PreviousProfileTool = '') and (PreviousTarget <> '') and
     FileExists(ProfileTool(PreviousTarget)) then
    PreviousProfileTool := ProfileTool(PreviousTarget);
  if (PreviousServer = '') and (PreviousTarget <> '') and
     FileExists(AddBackslash(PreviousTarget) + 'FamoRuntime.exe') then
    PreviousServer := AddBackslash(PreviousTarget) + 'FamoRuntime.exe';
  if PreviousIdentity = '' then
    PreviousIdentity := 'LegacyStable';
  if OriginalUserSid = '' then
    RaiseException('original user identity missing before snapshot');
  RegQueryStringValue(HKU, OriginalUserSid + '\Keyboard Layout\Preload',
    '1', PreviousDefault);
  ReadPreviousHostMetadata;
  if PreviousTarget = '' then
  begin
    if (PreviousTransactionId <> '') and
       IsEmptyRollbackAnchorForProjection(PreviousTransactionId) then
    begin
      { A failed first install keeps its terminal journal as the only durable
        recovery/cleanup anchor, but it is not an installed predecessor. }
      PreviousTransactionId := '';
      PreviousState := '';
    end
    else if (PreviousTransactionId <> '') or
       (PreviousCompatibilityTransactionId <> '') or
       (PreviousState <> '') or (PreviousHost <> '') then
      RaiseException('inconsistent empty previous installation projection');
  end
  else
  begin
    if (PreviousState <> StateReady) or
       not (ValidTransactionId(PreviousCompatibilityTransactionId) or
         ValidLegacyTransactionId(
           PreviousCompatibilityTransactionId)) then
      RaiseException('previous installation projection is not Ready');
    if PreviousTransactionId <> '' then
    begin
      if (CompareText(PreviousTransactionId,
           PreviousCompatibilityTransactionId) = 0) and
         ValidatePreviousV2Transaction(PreviousTransactionId,
           PreviousTarget, PreviousManifest) then
      begin
        { A normal v2 Ready predecessor. }
      end
      else if IsLegacyRollbackAnchorForProjection(
        PreviousTransactionId, PreviousTarget, PreviousManifest) then
        PreviousTransactionId := ''
      else
        RaiseException('previous transaction anchor identity mismatch');
    end
    else if not ValidateLegacyPreviousSnapshot then
      RaiseException('legacy previous installation identity mismatch');
  end;
end;

function FindPathInList(Paths: TStringList; const Path: String): Integer;
var
  I: Integer;
begin
  Result := -1;
  for I := 0 to Paths.Count - 1 do
  begin
    if PathSame(Paths[I], Path) then
    begin
      Result := I;
      Exit;
    end;
  end;
end;

procedure VerifyActualPayloadFiles(const Directory, FinalRoot,
  FinalManifest: String; ManifestFinalPaths, SeenActualPaths,
  SeenActualObjectIds: TStringList; var ActualCount: Integer);
var
  FindRec: TFindRec;
  Path, FinalPath, ObjectId: String;
begin
  if FindFirst(AddBackslash(Directory) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Path := AddBackslash(Directory) + FindRec.Name;
          if (FindRec.Attributes and FileAttributeReparsePoint) <> 0 then
            RaiseException('payload contains a reparse point: ' + Path);
          if (FindRec.Attributes and FileAttributeDirectory) <> 0 then
            VerifyActualPayloadFiles(Path, FinalRoot, FinalManifest,
              ManifestFinalPaths, SeenActualPaths, SeenActualObjectIds,
              ActualCount)
          else
          begin
            if not TryGetFinalObjectInfo(Path, FinalPath, ObjectId) then
              RaiseException('payload file identity unavailable: ' + Path);
            if not PathStartsWith(FinalPath, AddBackslash(FinalRoot), True) then
              RaiseException('payload file resolves outside transaction root: ' +
                Path);
            if not PathSame(FinalPath, FinalManifest) then
            begin
              if (FindPathInList(SeenActualPaths, FinalPath) >= 0) or
                 ((ObjectId <> '') and
                  (SeenActualObjectIds.IndexOf(ObjectId) >= 0)) then
                RaiseException('payload contains duplicate file objects: ' +
                  Path);
              SeenActualPaths.Add(FinalPath);
              if ObjectId <> '' then SeenActualObjectIds.Add(ObjectId);
              if FindPathInList(ManifestFinalPaths, FinalPath) < 0 then
                RaiseException('payload file missing from manifest: ' + Path);
              ActualCount := ActualCount + 1;
            end;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure VerifyPayloadOrFail;
var
  Lines: TArrayOfString;
  Manifest, TransactionRoot, FinalRoot, RootObjectId, FinalManifest,
    ManifestObjectId, RelativePath, FullPath, FinalPath, ObjectId,
    ExpectedHash, ActualHash: String;
  SeenPaths, SeenFinalPaths, SeenObjectIds, SeenActualPaths,
    SeenActualObjectIds: TStringList;
  I, DeclaredCount, EntryCount, ActualCount: Integer;
  ExpectedSize, ActualSize: Int64;
  HasFormat, HasProduct, HasVersion, HasProtocol, HasArch, HasIdentity: Boolean;
begin
  Manifest := AddBackslash(EnsureTransactionTarget) + 'payload-manifest.txt';
  if not FileExists(Manifest) then RaiseException('payload manifest missing');
  ActualHash := Uppercase(GetSHA256OfFile(Manifest));
  if CompareText(ActualHash, JournalManifestHash) <> 0 then
    RaiseException('payload manifest full hash mismatch');
  if CompareText(Copy(ActualHash, 1, 12),
       Copy(JournalManifestHash, 1, 12)) <> 0 then
    RaiseException('payload manifest identity mismatch');
  if not LoadStringsFromFile(Manifest, Lines) then RaiseException('payload manifest unreadable');

  TransactionRoot := NormalizeDirectoryPath(EnsureTransactionTarget);
  if not TryGetFinalObjectInfo(TransactionRoot, FinalRoot, RootObjectId) then
    RaiseException('transaction root identity unavailable');
  if not TryGetFinalObjectInfo(Manifest, FinalManifest, ManifestObjectId) or
     not PathSame(ExtractFileDir(FinalManifest), FinalRoot) then
    RaiseException('payload manifest resolves outside transaction root');
  SeenPaths := TStringList.Create;
  SeenFinalPaths := TStringList.Create;
  SeenObjectIds := TStringList.Create;
  SeenActualPaths := TStringList.Create;
  SeenActualObjectIds := TStringList.Create;
  try
    SeenPaths.CaseSensitive := False;
    SeenObjectIds.CaseSensitive := True;
    SeenActualObjectIds.CaseSensitive := True;
    DeclaredCount := -1;
    EntryCount := 0;
    ActualCount := 0;
    HasFormat := False;
    HasProduct := False;
    HasVersion := False;
    HasProtocol := False;
    HasArch := False;
    HasIdentity := False;
    for I := 0 to GetArrayLength(Lines) - 1 do
    begin
      HasFormat := HasFormat or (Lines[I] = 'format=1');
      HasProduct := HasProduct or (Lines[I] = 'product=Famo');
      HasVersion := HasVersion or
        (Lines[I] = 'version=' + JournalAppVersion);
      HasProtocol := HasProtocol or (Lines[I] = 'protocol=1');
      HasArch := HasArch or (Lines[I] = 'architecture=x64');
      HasIdentity := HasIdentity or (Lines[I] = 'identity={#Identity}');
      if Pos('file_count=', Lines[I]) = 1 then
        DeclaredCount := StrToInt(Copy(Lines[I], 12, Length(Lines[I])));
      if Pos('file=', Lines[I]) = 1 then
      begin
        if not ParseFileEntryDetailed(Lines[I], RelativePath, ExpectedSize,
          ExpectedHash) then
          RaiseException('invalid payload manifest entry');
        FullPath := ExpandFileName(PathCombine(TransactionRoot, RelativePath));
        if not PathStartsWith(FullPath, AddBackslash(TransactionRoot), True) then
          RaiseException('payload path escapes transaction root: ' + RelativePath);
        if SeenPaths.IndexOf(FullPath) >= 0 then
          RaiseException('duplicate payload manifest path: ' + RelativePath);
        SeenPaths.Add(FullPath);
        if not TryGetFinalObjectInfo(FullPath, FinalPath, ObjectId) then
          RaiseException('payload file identity unavailable: ' + RelativePath);
        if not PathStartsWith(FinalPath, AddBackslash(FinalRoot), True) then
          RaiseException('payload file resolves outside transaction root: ' +
            RelativePath);
        if (FindPathInList(SeenFinalPaths, FinalPath) >= 0) or
           ((ObjectId <> '') and (SeenObjectIds.IndexOf(ObjectId) >= 0)) then
          RaiseException('duplicate payload manifest object: ' + RelativePath);
        SeenFinalPaths.Add(FinalPath);
        if ObjectId <> '' then SeenObjectIds.Add(ObjectId);
        if not TryGetFileSize64(FullPath, ActualSize) or
           (ActualSize <> ExpectedSize) then
          RaiseException('payload size mismatch: ' + RelativePath);
        ActualHash := Uppercase(GetSHA256OfFile(FullPath));
        if CompareText(ActualHash, ExpectedHash) <> 0 then
          RaiseException('payload hash mismatch: ' + RelativePath);
        EntryCount := EntryCount + 1;
      end;
    end;
    if not (HasFormat and HasProduct and HasVersion and HasProtocol and HasArch and HasIdentity) then
      RaiseException('payload manifest header mismatch');
    VerifyActualPayloadFiles(TransactionRoot, FinalRoot, FinalManifest,
      SeenFinalPaths, SeenActualPaths, SeenActualObjectIds, ActualCount);
    if (DeclaredCount <> EntryCount) or (ActualCount <> EntryCount) then
      RaiseException('payload file_count mismatch');
    JournalPendingFinalTarget := FinalRoot;
    JournalPendingObjectId := RootObjectId;
  finally
    SeenActualObjectIds.Free;
    SeenActualPaths.Free;
    SeenObjectIds.Free;
    SeenFinalPaths.Free;
    SeenPaths.Free;
  end;
end;

function CachedCurrentPayloadExecutionProofMatches: Boolean;
var
  TargetFinalPath, TargetObjectId, Manifest, ManifestFinalPath,
    ManifestObjectId: String;
begin
  { Cache only within this elevated Setup process. The target inherits the
    Program Files ACL, while every desktop child runs with the original
    non-elevated token. Re-pin the target and manifest objects on every use,
    and invalidate the cache before any target deletion. }
  Result := False;
  Manifest := AddBackslash(TransactionTarget) + 'payload-manifest.txt';
  if not CurrentPayloadProofValid or
     (CompareText(CurrentPayloadProofManifestHash,
       JournalManifestHash) <> 0) or
     not FinalObjectsSame(CurrentPayloadProofFinalTarget,
       CurrentPayloadProofObjectId, JournalPendingFinalTarget,
       JournalPendingObjectId) or
     not TryGetFinalObjectInfo(TransactionTarget, TargetFinalPath,
       TargetObjectId) or
     not TryGetFinalObjectInfo(Manifest, ManifestFinalPath,
       ManifestObjectId) then
    Exit;
  Result :=
    FinalObjectsSame(TargetFinalPath, TargetObjectId,
      CurrentPayloadProofFinalTarget, CurrentPayloadProofObjectId) and
    FinalObjectsSame(ManifestFinalPath, ManifestObjectId,
      CurrentPayloadProofManifestFinalPath,
      CurrentPayloadProofManifestObjectId) and
    PathSame(ExtractFileDir(ManifestFinalPath), TargetFinalPath) and
    (CompareText(GetSHA256OfFile(Manifest), JournalManifestHash) = 0);
end;

function ValidateCurrentPayloadForExecution: Boolean;
var
  PinnedFinalTarget, PinnedObjectId, Manifest, ManifestFinalPath,
    ManifestObjectId: String;
begin
  Result := False;
  PinnedFinalTarget := JournalPendingFinalTarget;
  PinnedObjectId := JournalPendingObjectId;
  if (PinnedFinalTarget = '') or (PinnedObjectId = '') then Exit;
  if CachedCurrentPayloadExecutionProofMatches then
  begin
    Result := True;
    Exit;
  end;
  CurrentPayloadProofValid := False;
  try
    try
      VerifyPayloadOrFail;
      Result := FinalObjectsSame(JournalPendingFinalTarget,
        JournalPendingObjectId, PinnedFinalTarget, PinnedObjectId);
      Manifest := AddBackslash(TransactionTarget) +
        'payload-manifest.txt';
      if Result and
         TryGetFinalObjectInfo(Manifest, ManifestFinalPath,
           ManifestObjectId) and
         PathSame(ExtractFileDir(ManifestFinalPath),
           JournalPendingFinalTarget) then
      begin
        CurrentPayloadProofFinalTarget := PinnedFinalTarget;
        CurrentPayloadProofObjectId := PinnedObjectId;
        CurrentPayloadProofManifestFinalPath := ManifestFinalPath;
        CurrentPayloadProofManifestObjectId := ManifestObjectId;
        CurrentPayloadProofManifestHash := JournalManifestHash;
        CurrentPayloadProofValid := True;
      end
      else
        Result := False;
    except
      Log('current payload execution proof failed: ' + GetExceptionMessage);
      Result := False;
    end;
  finally
    JournalPendingFinalTarget := PinnedFinalTarget;
    JournalPendingObjectId := PinnedObjectId;
  end;
end;

function RunRegSvr32(const DllPath: String; Unregister: Boolean): Boolean;
var
  ResultCode: Integer;
  Parameters: String;
begin
  Parameters := '/s ' + AddQuotes(DllPath);
  if Unregister then
    Parameters := '/u ' + Parameters;
  Result := Exec(ExpandConstant('{sys}\regsvr32.exe'), Parameters, '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function PreviousRegistrationTool: String;
begin
  if (PreviousProfileTool <> '') and FileExists(PreviousProfileTool) then
    Result := PreviousProfileTool
  else
    Result := ProfileTool(TransactionTarget);
end;

function UnregisterPreviousRegistration: Boolean;
begin
  if PreviousHost = '' then
  begin
    Result := True;
    Exit;
  end;
  if not ValidatePreviousPayloadForExecution then
  begin
    Result := False;
    Exit;
  end;
  if FileExists(ProfileTool(TransactionTarget)) then
    if RunAndRequire(ProfileTool(TransactionTarget),
         'unregister-machine-at ' + AddQuotes(PreviousHost), False) then
    begin
      Result := True;
      Exit;
    end;
  Result := RunRegSvr32(PreviousHost, True);
end;

function RegisterPreviousRegistration: Boolean;
begin
  if PreviousHost = '' then
  begin
    Result := True;
    Exit;
  end;
  if not ValidatePreviousPayloadForExecution then
  begin
    Result := False;
    Exit;
  end;
  if FileExists(ProfileTool(TransactionTarget)) then
    if RunAndRequire(ProfileTool(TransactionTarget),
         'register-machine-at ' + AddQuotes(PreviousHost), False) then
    begin
      Result := True;
      Exit;
    end;
  Result := RunRegSvr32(PreviousHost, False);
end;

function DetectLoadedPreviousHost: Boolean;
var
  ProbeExit: Integer;
begin
  Result := False;
  if PreviousHost = '' then Exit;
  if not FileExists(PreviousHost) then
    RaiseException('registered previous host path is missing');
  ProbeExit := RunExitCode(ProfileTool(TransactionTarget),
    'loaded ' + AddQuotes(PreviousHost));
  if ProbeExit = 0 then
    Result := True
  else if ProbeExit <> 1 then
    RaiseException('loaded-module probe failed');
  Log('previous host: path=' + PreviousHost + '; hash=' + LoadedHostHash +
    '; version=' + LoadedHostVersion + '; expectedHash=' +
    LoadedHostExpectedHash + '; loaded=' + IntToStr(Ord(Result)));
end;

function ContainsParentTraversal(const Path: String): Boolean;
begin
  Result := Pos('\..\', '\' + PathNormalizeSlashes(Path) + '\') > 0;
end;

function PathIsNonReparseOrMissing(const Path: String): Boolean;
var
  Attributes: Cardinal;
  Exists: Boolean;
begin
  Result := TryGetPathAttributes(Path, Exists, Attributes) and
    (not Exists or ((Attributes and FileAttributeReparsePoint) = 0));
end;

function ValidateProtectedChild(const Parent, ChildName: String): Boolean;
var
  NormalizedParent, Child, ParentFinalPath, ParentObjectId,
    ChildFinalPath, ChildObjectId: String;
  ParentAttributes, ChildAttributes: Cardinal;
  ParentExists, ChildExists: Boolean;
begin
  Result := False;
  NormalizedParent := NormalizeDirectoryPath(Parent);
  Child := NormalizeDirectoryPath(AddBackslash(NormalizedParent) + ChildName);
  if not PathSame(ExtractFileDir(Child), NormalizedParent) or
     not TryGetPathAttributes(NormalizedParent, ParentExists,
       ParentAttributes) or
     not TryGetPathAttributes(Child, ChildExists, ChildAttributes) or
     not PathIsNonReparseOrMissing(NormalizedParent) or
     not PathIsNonReparseOrMissing(Child) then
    Exit;
  if not ParentExists then
  begin
    Result := not ChildExists;
    Exit;
  end;
  if ((ParentAttributes and FileAttributeDirectory) = 0) or
     not TryGetFinalObjectInfo(NormalizedParent, ParentFinalPath,
       ParentObjectId) then
    Exit;
  if ChildExists then
  begin
    if ((ChildAttributes and FileAttributeDirectory) = 0) or
       not TryGetFinalObjectInfo(Child, ChildFinalPath, ChildObjectId) or
       not PathSame(ExtractFileDir(ChildFinalPath), ParentFinalPath) then
      Exit;
  end;
  Result := True;
end;

function ValidateProtectedFile(const Parent, ChildName: String): Boolean;
var
  NormalizedParent, Child, ParentFinalPath, ParentObjectId,
    ChildFinalPath, ChildObjectId: String;
  ParentAttributes, ChildAttributes: Cardinal;
  ParentExists, ChildExists: Boolean;
begin
  Result := False;
  NormalizedParent := NormalizeDirectoryPath(Parent);
  Child := NormalizeDirectoryPath(AddBackslash(NormalizedParent) + ChildName);
  if not PathSame(ExtractFileDir(Child), NormalizedParent) or
     not TryGetPathAttributes(NormalizedParent, ParentExists,
       ParentAttributes) or
     not TryGetPathAttributes(Child, ChildExists, ChildAttributes) or
     not PathIsNonReparseOrMissing(NormalizedParent) or
     not PathIsNonReparseOrMissing(Child) then
    Exit;
  if not ParentExists then
  begin
    Result := not ChildExists;
    Exit;
  end;
  if ((ParentAttributes and FileAttributeDirectory) = 0) or
     not TryGetFinalObjectInfo(NormalizedParent, ParentFinalPath,
       ParentObjectId) then
    Exit;
  if ChildExists then
  begin
    if ((ChildAttributes and
         (FileAttributeDirectory or FileAttributeReparsePoint)) <> 0) or
       not TryGetFinalObjectInfo(Child, ChildFinalPath, ChildObjectId) or
       not PathSame(ExtractFileDir(ChildFinalPath), ParentFinalPath) then
      Exit;
  end;
  Result := True;
end;

procedure RequireFixedProtectedInstallRoot;
var
  AppRoot, ProgramFilesRoot, DriveRoot, ProgramFilesFinalPath,
    ProgramFilesObjectId, AppFinalPath, AppObjectId: String;
  AppAttributes, ProgramFilesAttributes: Cardinal;
  AppExists, ProgramFilesExists: Boolean;
begin
  AppRoot := FixedInstallRoot;
  ProgramFilesRoot := NormalizeDirectoryPath(ExpandConstant('{autopf}'));
  DriveRoot := AddBackslash(ExtractFileDrive(ProgramFilesRoot));
  if not PathIsNonReparseOrMissing(DriveRoot) or
     not PathIsNonReparseOrMissing(ProgramFilesRoot) or
     not PathIsNonReparseOrMissing(AppRoot) or
     not TryGetPathAttributes(ProgramFilesRoot, ProgramFilesExists,
       ProgramFilesAttributes) or not ProgramFilesExists or
     ((ProgramFilesAttributes and FileAttributeDirectory) = 0) or
     not TryGetFinalObjectInfo(ProgramFilesRoot, ProgramFilesFinalPath,
       ProgramFilesObjectId) or
     not TryGetPathAttributes(AppRoot, AppExists, AppAttributes) then
    RaiseException('protected Program Files install root is unavailable');
  if AppExists and
     (((AppAttributes and FileAttributeDirectory) = 0) or
      not TryGetFinalObjectInfo(AppRoot, AppFinalPath, AppObjectId) or
      not PathSame(ExtractFileDir(AppFinalPath), ProgramFilesFinalPath)) then
    RaiseException('protected install root identity mismatch');
  if not ValidateProtectedChild(AppRoot, 'pending') or
     not ValidateProtectedChild(AppRoot, 'versions') or
     not ValidateProtectedChild(AppRoot, 'bridge') or
     not ValidateProtectedChild(AddBackslash(AppRoot) + 'bridge',
       'v{#BridgeAbi}') then
    RaiseException('protected transaction roots are unsafe');
end;

procedure RequireSelectedFixedInstallRoot;
var
  SelectedRoot: String;
begin
  SelectedRoot := NormalizeDirectoryPath(ExpandConstant('{app}'));
  if not PathSame(SelectedRoot, FixedInstallRoot) then
    RaiseException('custom /DIR install roots are not allowed');
  RequireFixedProtectedInstallRoot;
end;

function IdentityRecordValue(Lines: TArrayOfString;
  const Name: String; var Value: String): Boolean;
var
  I, Found: Integer;
  Prefix: String;
begin
  Prefix := Name + '=';
  Found := 0;
  Value := '';
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    if Pos(Prefix, Lines[I]) = 1 then
    begin
      Found := Found + 1;
      Value := Copy(Lines[I], Length(Prefix) + 1, Length(Lines[I]));
    end;
  end;
  Result := Found = 1;
end;

function ValidSidText(const Value: String): Boolean;
var
  I, SegmentStart: Integer;
  Sid: INT_PTR;
begin
  Result := False;
  if (Length(Value) < 5) or (Copy(Value, 1, 4) <> 'S-1-') then Exit;
  SegmentStart := 5;
  for I := 5 to Length(Value) do
  begin
    if Value[I] = '-' then
    begin
      if I = SegmentStart then Exit;
      SegmentStart := I + 1;
    end
    else if not ((Value[I] >= '0') and (Value[I] <= '9')) then
      Exit;
  end;
  if SegmentStart > Length(Value) then Exit;
  Sid := 0;
  if not ConvertStringSidToSidW(Value, Sid) then Exit;
  try
    if Sid <> 0 then Result := IsValidSid(Sid);
  finally
    if Sid <> 0 then LocalFree(Sid);
  end;
end;

function ValidTransactionId(const Value: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  if Length(Value) <> 32 then Exit;
  for I := 1 to Length(Value) do
    if not (((Value[I] >= '0') and (Value[I] <= '9')) or
            ((Value[I] >= 'a') and (Value[I] <= 'f'))) then Exit;
  Result := True;
end;

function ValidLegacyTransactionId(const Value: String): Boolean;
var
  I, Suffix: Integer;
  SuffixText: String;
begin
  Result := False;
  if (Length(Value) < 16) or (Length(Value) > 21) or
     (Value[15] <> '-') then
    Exit;
  for I := 1 to 14 do
    if (Value[I] < '0') or (Value[I] > '9') then Exit;
  SuffixText := Copy(Value, 16, Length(Value));
  for I := 1 to Length(SuffixText) do
    if (SuffixText[I] < '0') or (SuffixText[I] > '9') then Exit;
  Suffix := StrToIntDef(SuffixText, -1);
  Result := (Suffix >= 0) and (Suffix <= 999999) and
    (SuffixText = IntToStr(Suffix));
end;

procedure HardenPendingDirectory(const Directory, AllowedSid: String);
var
  Parameters: String;
begin
  Parameters := AddQuotes(Directory) +
    ' /inheritance:r /grant:r ' +
    AddQuotes('*S-1-5-18:(OI)(CI)(F)') + ' ' +
    AddQuotes('*S-1-5-32-544:(OI)(CI)(F)') + ' ';
  if AllowedSid = '' then
    Parameters := Parameters + AddQuotes('*S-1-5-11:(OI)(CI)(RX)')
  else
    Parameters := Parameters + AddQuotes('*' + AllowedSid +
      ':(OI)(CI)(RX)') + ' /remove:g ' + AddQuotes('*S-1-5-11');
  if not RunAndRequire(ExpandConstant('{sys}\icacls.exe'), Parameters,
    False) then
    RaiseException('cannot protect pending transaction directory');
end;

function ValidatePinnedBrokerForExecution(const Broker,
  ExpectedHash, PinnedFinalPath, PinnedObjectId,
  PinnedDirectoryFinalPath, PinnedDirectoryObjectId: String;
  ExpectedSize: Int64): Boolean;
var
  ActualSize: Int64;
  BrokerFinalPath, BrokerObjectId, DirectoryFinalPath,
    DirectoryObjectId: String;
begin
  Result :=
    (ExpectedSize >= 0) and IsSha256Hex(ExpectedHash) and
    FileExists(Broker) and
    TryGetFileSize64(Broker, ActualSize) and
    (ActualSize = ExpectedSize) and
    (CompareText(GetSHA256OfFile(Broker), ExpectedHash) = 0) and
    TryGetFinalObjectInfo(ExtractFileDir(Broker), DirectoryFinalPath,
      DirectoryObjectId) and
    TryGetFinalObjectInfo(Broker, BrokerFinalPath, BrokerObjectId) and
    FinalObjectsSame(DirectoryFinalPath, DirectoryObjectId,
      PinnedDirectoryFinalPath, PinnedDirectoryObjectId) and
    FinalObjectsSame(BrokerFinalPath, BrokerObjectId,
      PinnedFinalPath, PinnedObjectId) and
    PathSame(ExtractFileDir(BrokerFinalPath), DirectoryFinalPath);
end;

procedure CaptureOriginalUserIdentity;
var
  PendingRoot, PendingDirectory, EmbeddedManifest, EmbeddedBroker, Broker,
    IdentityRecord, PipeId, Challenge, Parameters, FormatText, CapturedPipeId,
    CapturedChallenge, CapturedSid, CapturedSession, CapturedAccount,
  ResumeText, Failure, CleanupFailure, ExpectedBrokerHash,
    BrokerFinalPath, BrokerObjectId, DirectoryFinalPath,
    DirectoryObjectId: String;
  Lines: TArrayOfString;
  ServerResult, Attempts: Integer;
  ExpectedBrokerSize, ActualBrokerSize: Int64;
  RecordLoaded: Boolean;
begin
  PipeId := NewIdentityNonce;
  Challenge := NewIdentityNonce;
  if CompareText(PipeId, Challenge) = 0 then
    RaiseException('original-user identity nonces collided');

  PendingRoot := AddBackslash(FixedInstallRoot) + 'pending';
  PendingDirectory := AddBackslash(PendingRoot) + 'identity-' + PipeId;
  ArmHelperCleanupDebt(DebtKindIdentityHelper, PipeId);
  FailIfRequested('after-identity-helper-debt');
  if (DirExists(PendingRoot) and not PathIsNonReparseOrMissing(PendingRoot)) or
     not ForceDirectories(PendingRoot) or
     not PathIsNonReparseOrMissing(PendingRoot) then
    RaiseException('cannot create protected identity directory');
  if DirExists(PendingDirectory) or
     not ForceDirectories(PendingDirectory) or
     not PathIsNonReparseOrMissing(PendingDirectory) then
    RaiseException('cannot create fresh identity directory');
  HardenPendingDirectory(PendingDirectory, '');
  FailIfRequested('after-identity-helper-create');

  ExtractTemporaryFile('FamoEmbeddedManifest.txt');
  EmbeddedManifest := ExpandConstant('{tmp}\FamoEmbeddedManifest.txt');
  ExtractTemporaryFile('FamoIdentityBroker.exe');
  EmbeddedBroker := ExpandConstant('{tmp}\FamoIdentityBroker.exe');
  Broker := AddBackslash(PendingDirectory) + 'FamoIdentityBroker-' +
    PipeId + '.exe';
  IdentityRecord := AddBackslash(PendingDirectory) + 'identity-' +
    PipeId + '.txt';
  Failure := '';
  try
    if (CompareText(GetSHA256OfFile(EmbeddedManifest),
         '{#ManifestHash}') <> 0) or
       not ReadPinnedManagedFileIdentity(EmbeddedManifest,
         'FamoProfileTool.exe', ExpectedBrokerSize,
         ExpectedBrokerHash) or
       FileExists(IdentityRecord) or
       not CopyFile(EmbeddedBroker, Broker, True) or
       not TryGetFileSize64(Broker, ActualBrokerSize) or
       (ActualBrokerSize <> ExpectedBrokerSize) or
       (CompareText(GetSHA256OfFile(Broker),
         ExpectedBrokerHash) <> 0) or
       not TryGetFinalObjectInfo(PendingDirectory,
         DirectoryFinalPath, DirectoryObjectId) or
       not TryGetFinalObjectInfo(Broker,
         BrokerFinalPath, BrokerObjectId) or
       not PathSame(ExtractFileDir(BrokerFinalPath),
         DirectoryFinalPath) then
      RaiseException('cannot stage original-user identity broker');

    if OriginalUserSid = '' then
      Parameters := 'capture-original-user ' + PipeId + ' ' + Challenge + ' ' +
        AddQuotes(IdentityRecord)
    else
      Parameters := 'capture-original-user-for ' + OriginalUserSid + ' ' +
        PipeId + ' ' + Challenge + ' ' + AddQuotes(IdentityRecord);
    if not ValidatePinnedBrokerForExecution(Broker,
         ExpectedBrokerHash, BrokerFinalPath, BrokerObjectId,
         DirectoryFinalPath, DirectoryObjectId,
         ExpectedBrokerSize) then
      RaiseException('original-user identity broker changed before capture');
    if not Exec(Broker, Parameters, '', SW_HIDE, ewNoWait, ServerResult) then
      RaiseException('cannot start original-user identity coordinator');
    if OriginalUserSid = '' then
      Parameters := 'prove-shell-token ' + PipeId + ' ' + Challenge
    else
      Parameters := 'prove-shell-token-for ' + OriginalUserSid + ' ' +
        PipeId + ' ' + Challenge;
    if not ValidatePinnedBrokerForExecution(Broker,
         ExpectedBrokerHash, BrokerFinalPath, BrokerObjectId,
         DirectoryFinalPath, DirectoryObjectId,
         ExpectedBrokerSize) then
      RaiseException('original-user identity broker changed before proof');
    if not RunAndRequire(Broker, Parameters, False) then
      RaiseException('original-user token proof failed');

    RecordLoaded := False;
    for Attempts := 1 to 150 do
    begin
      if FileExists(IdentityRecord) then
      begin
        RecordLoaded := LoadStringsFromFile(IdentityRecord, Lines);
        if RecordLoaded then Break;
      end;
      Sleep(100);
    end;
    if not RecordLoaded then
      RaiseException('original-user identity record missing');
    if (GetArrayLength(Lines) <> 7) or
       not IdentityRecordValue(Lines, 'format', FormatText) or
       (FormatText <> '1') or
       not IdentityRecordValue(Lines, 'pipe_id', CapturedPipeId) or
       (CompareText(CapturedPipeId, PipeId) <> 0) or
       not IdentityRecordValue(Lines, 'challenge', CapturedChallenge) or
       (CompareText(CapturedChallenge, Challenge) <> 0) or
       not IdentityRecordValue(Lines, 'sid', CapturedSid) or
       not ValidSidText(CapturedSid) or
       not IdentityRecordValue(Lines, 'session', CapturedSession) or
       (StrToIntDef(CapturedSession, 0) <= 0) or
       not IdentityRecordValue(Lines, 'account', CapturedAccount) or
       (CapturedAccount = '') or
       not IdentityRecordValue(Lines, 'resume_capable', ResumeText) or
       ((ResumeText <> '0') and (ResumeText <> '1')) then
      RaiseException('original-user identity record is invalid');

    if (OriginalUserSid <> '') and
       (CompareText(OriginalUserSid, CapturedSid) <> 0) then
      RaiseException('transaction belongs to a different interactive user');
    if OriginalUserSid = '' then
    begin
      OriginalUserSid := CapturedSid;
      OriginalUserSession := CapturedSession;
      OriginalUserAccount := CapturedAccount;
      OriginalUserResumeCapable := ResumeText = '1';
    end;
    CurrentOriginalUserSession := CapturedSession;
  except
    Failure := GetExceptionMessage;
  end;

  CleanupFailure := '';
  CleanupExactHelperAndDebt(TransactionId,
    DebtKindIdentityHelper, PipeId, 150, CleanupFailure);
  if Failure <> '' then
  begin
    if CleanupFailure <> '' then Failure := Failure + '; ' + CleanupFailure;
    RaiseException(Failure);
  end;
  if CleanupFailure <> '' then RaiseException(CleanupFailure);
end;

function ReadPinnedManagedFileIdentity(const Manifest, RelativeName: String;
  var ExpectedSize: Int64; var ExpectedHash: String): Boolean;
var
  Lines: TArrayOfString;
  I, Matches: Integer;
  RelativePath, CandidateHash: String;
  CandidateSize: Int64;
begin
  Result := False;
  ExpectedSize := -1;
  ExpectedHash := '';
  if not LoadStringsFromFile(Manifest, Lines) then Exit;
  Matches := 0;
  for I := 0 to GetArrayLength(Lines) - 1 do
    if Pos('file=', Lines[I]) = 1 then
    begin
      if not ParseFileEntryDetailed(Lines[I], RelativePath,
           CandidateSize, CandidateHash) then Exit;
      if CompareText(RelativePath, RelativeName) = 0 then
      begin
        Matches := Matches + 1;
        ExpectedSize := CandidateSize;
        ExpectedHash := Uppercase(CandidateHash);
      end;
    end;
  Result := (Matches = 1) and (ExpectedSize >= 0) and
    IsSha256Hex(ExpectedHash);
end;

function RunTrustedDirectMachineUnregister: Boolean;
var
  PendingRoot, ProtectedDirectory, Nonce, EmbeddedManifest,
    ProtectedManifest, EmbeddedBroker, ProtectedBroker,
    ExpectedBrokerHash, BrokerFinalPath, BrokerObjectId,
    DirectoryFinalPath, DirectoryObjectId, Failure, CleanupFailure: String;
  ExpectedBrokerSize, ActualBrokerSize: Int64;
  CleanupSucceeded: Boolean;
begin
  Result := False;
  Nonce := NewIdentityNonce;
  PendingRoot := AddBackslash(FixedInstallRoot) + 'pending';
  ProtectedDirectory := AddBackslash(PendingRoot) +
    'machine-cleanup-' + Nonce;
  ProtectedManifest := AddBackslash(ProtectedDirectory) +
    'payload-manifest.txt';
  ProtectedBroker := AddBackslash(ProtectedDirectory) +
    'FamoMachineCleanup.exe';
  Failure := '';
  ArmHelperCleanupDebt(DebtKindMachineCleanupHelper, Nonce);
  FailIfRequested('after-machine-helper-debt');
  try
    if not ValidTransactionId(Nonce) or
       not IsSha256Hex(JournalManifestHash) or
       (DirExists(PendingRoot) and
        not PathIsNonReparseOrMissing(PendingRoot)) or
       not ForceDirectories(PendingRoot) or
       not PathIsNonReparseOrMissing(PendingRoot) or
       DirExists(ProtectedDirectory) or
       not ForceDirectories(ProtectedDirectory) or
       not PathIsNonReparseOrMissing(ProtectedDirectory) then
      RaiseException('cannot create trusted machine cleanup directory');
    HardenPendingDirectory(ProtectedDirectory, OriginalUserSid);
    FailIfRequested('after-machine-helper-create');

    ExtractTemporaryFile('FamoEmbeddedManifest.txt');
    EmbeddedManifest := ExpandConstant('{tmp}\FamoEmbeddedManifest.txt');
    if not CopyFile(EmbeddedManifest, ProtectedManifest, True) or
       (CompareText(GetSHA256OfFile(ProtectedManifest),
          JournalManifestHash) <> 0) or
       not ReadPinnedManagedFileIdentity(ProtectedManifest,
         'FamoProfileTool.exe', ExpectedBrokerSize,
         ExpectedBrokerHash) then
      RaiseException('embedded cleanup manifest identity mismatch');

    ExtractTemporaryFile('FamoIdentityBroker.exe');
    EmbeddedBroker := ExpandConstant('{tmp}\FamoIdentityBroker.exe');
    if not CopyFile(EmbeddedBroker, ProtectedBroker, True) or
       not TryGetFileSize64(ProtectedBroker, ActualBrokerSize) or
       (ActualBrokerSize <> ExpectedBrokerSize) or
       (CompareText(GetSHA256OfFile(ProtectedBroker),
          ExpectedBrokerHash) <> 0) or
       not TryGetFinalObjectInfo(ProtectedDirectory,
         DirectoryFinalPath, DirectoryObjectId) or
       not TryGetFinalObjectInfo(ProtectedBroker,
         BrokerFinalPath, BrokerObjectId) or
       not PathSame(ExtractFileDir(BrokerFinalPath),
         DirectoryFinalPath) then
      RaiseException('trusted machine cleanup broker identity mismatch');
    Result := ValidatePinnedBrokerForExecution(ProtectedBroker,
      ExpectedBrokerHash, BrokerFinalPath, BrokerObjectId,
      DirectoryFinalPath, DirectoryObjectId, ExpectedBrokerSize) and
      RunAndRequire(ProtectedBroker, 'unregister-machine-direct', False);
  except
    Failure := GetExceptionMessage;
    Result := False;
  end;

  CleanupSucceeded := CleanupExactHelperAndDebt(TransactionId,
    DebtKindMachineCleanupHelper, Nonce, 1, CleanupFailure);
  if not CleanupSucceeded then
  begin
    if Failure <> '' then Failure := Failure + '; ';
    Failure := Failure + CleanupFailure;
  end;
  if not CleanupSucceeded then Result := False;
  if Failure <> '' then
    Log('trusted direct machine cleanup failed: ' + Failure);
end;

function TransactionJournalKey(const Id: String): String;
begin
  Result := BrandKey + '\Transactions\' + Id;
end;

function JournalGenerationKey(const Id: String; Generation: Integer): String;
begin
  Result := TransactionJournalKey(Id) + '\g' + IntToStr(Generation);
end;

function JournalCanonicalField(const Name, Value: String): String;
begin
  Result := Name + ':' + IntToStr(Length(Value)) + ':' + Value + ';';
end;

function JournalCanonical(const Journal: TTransactionJournal): String;
begin
  Result :=
    JournalCanonicalField('schema', JournalVersion) +
    JournalCanonicalField('product', 'Famo') +
    JournalCanonicalField('generation', Journal.Generation) +
    JournalCanonicalField('phase', Journal.Phase) +
    JournalCanonicalField('version', Journal.Version) +
    JournalCanonicalField('transaction', Journal.Transaction) +
    JournalCanonicalField('manifest_hash', Journal.ManifestHash) +
    JournalCanonicalField('pending_target', Journal.PendingTarget) +
    JournalCanonicalField('pending_final_target',
      Journal.PendingFinalTarget) +
    JournalCanonicalField('pending_object_id', Journal.PendingObjectId) +
    JournalCanonicalField('previous_target', Journal.PreviousTarget) +
    JournalCanonicalField('previous_final_target',
      Journal.PreviousFinalTarget) +
    JournalCanonicalField('previous_object_id', Journal.PreviousObjectId) +
    JournalCanonicalField('prior_previous_target',
      Journal.PriorPreviousTarget) +
    JournalCanonicalField('prior_previous_final_target',
      Journal.PriorPreviousFinalTarget) +
    JournalCanonicalField('prior_previous_object_id',
      Journal.PriorPreviousObjectId) +
    JournalCanonicalField('previous_manifest', Journal.PreviousManifest) +
    JournalCanonicalField('previous_manifest_hash',
      Journal.PreviousManifestHash) +
    JournalCanonicalField('previous_default', Journal.PreviousDefault) +
    JournalCanonicalField('previous_host', Journal.PreviousHost) +
    JournalCanonicalField('previous_server', Journal.PreviousServer) +
    JournalCanonicalField('previous_profile_tool',
      Journal.PreviousProfileTool) +
    JournalCanonicalField('previous_version', Journal.PreviousVersion) +
    JournalCanonicalField('previous_identity', Journal.PreviousIdentity) +
    JournalCanonicalField('previous_transaction_id',
      Journal.PreviousTransactionId) +
    JournalCanonicalField('previous_compatibility_transaction_id',
      Journal.PreviousCompatibilityTransactionId) +
    JournalCanonicalField('previous_state', Journal.PreviousState) +
    JournalCanonicalField('previous_profile_active',
      Journal.PreviousProfileActive) +
    JournalCanonicalField('previous_profile_enabled',
      Journal.PreviousProfileEnabled) +
    JournalCanonicalField('previous_input_tip_present',
      Journal.PreviousInputTipPresent) +
    JournalCanonicalField('seed_receipt_hash', Journal.SeedReceiptHash) +
    JournalCanonicalField('original_user_sid', Journal.OriginalUserSid) +
    JournalCanonicalField('original_user_account',
      Journal.OriginalUserAccount) +
    JournalCanonicalField('original_user_session',
      Journal.OriginalUserSession) +
    JournalCanonicalField('last_proof_session', Journal.LastProofSession) +
    JournalCanonicalField('original_user_resume_capable',
      Journal.OriginalUserResumeCapable) +
    JournalCanonicalField('resume_installer', Journal.ResumeInstaller) +
    JournalCanonicalField('resume_installer_hash',
      Journal.ResumeInstallerHash) +
    JournalCanonicalField('resume_task_name', Journal.ResumeTaskName) +
    JournalCanonicalField('allow_downgrade', Journal.AllowDowngrade) +
    JournalCanonicalField('loaded_host_hash', Journal.LoadedHostHash) +
    JournalCanonicalField('loaded_host_version', Journal.LoadedHostVersion) +
    JournalCanonicalField('loaded_host_expected_hash',
      Journal.LoadedHostExpectedHash);
end;

function JournalDigest(const Journal: TTransactionJournal): String;
begin
  Result := Uppercase(GetSHA256OfString(JournalCanonical(Journal)));
end;

procedure RequireJournalWrite(const Key, Name, Value: String);
begin
  if not RegWriteStringValue(HKLM64, Key, Name, Value) then
    RaiseException('cannot write transaction journal field: ' + Name);
end;

procedure FlushMachineRegistryKey(const Key: String);
var
  Handle: THandle;
begin
  Handle := 0;
  if RegOpenKeyExW(HKLM, Key, 0, KeyRead or KeyWow6464Key, Handle) <> 0 then
    RaiseException('cannot open transaction journal for flush');
  try
    if RegFlushKey(Handle) <> 0 then
      RaiseException('cannot flush transaction journal');
  finally
    RegCloseKey(Handle);
  end;
end;

function TransactionDebtValue(const Owner, Kind: String): String;
begin
  Result := DebtSchema + '|' + Owner + '|' + Kind;
end;

function ParseTransactionDebt(const Value: String;
  var Owner, Kind: String): Boolean;
var
  Prefix, Rest: String;
  Separator: Integer;
begin
  Result := False;
  Owner := '';
  Kind := '';
  Prefix := DebtSchema + '|';
  if Copy(Value, 1, Length(Prefix)) <> Prefix then Exit;
  Rest := Copy(Value, Length(Prefix) + 1, Length(Value));
  Separator := Pos('|', Rest);
  if Separator <= 1 then Exit;
  Owner := Copy(Rest, 1, Separator - 1);
  Kind := Copy(Rest, Separator + 1, Length(Rest));
  Result := ValidTransactionId(Owner) and (Kind <> '') and
    (Pos('|', Kind) = 0);
  if not Result then
  begin
    Owner := '';
    Kind := '';
  end;
end;

procedure ArmTransactionDebt(const Name, Kind: String);
var
  Existing, Expected, Readback: String;
begin
  Expected := TransactionDebtValue(TransactionId, Kind);
  if RegQueryStringValue(HKLM64, BrandKey, Name, Existing) and
     (Existing <> Expected) then
    RaiseException('foreign or malformed transaction debt blocks write: ' +
      Name);
  RequireJournalWrite(BrandKey, Name, Expected);
  FlushMachineRegistryKey(BrandKey);
  if not RegQueryStringValue(HKLM64, BrandKey, Name, Readback) or
     (Readback <> Expected) then
    RaiseException('transaction debt durability readback failed: ' + Name);
end;

procedure ClearTransactionDebt(const Name, Kind: String);
var
  Existing, Expected: String;
begin
  Expected := TransactionDebtValue(TransactionId, Kind);
  if not RegQueryStringValue(HKLM64, BrandKey, Name, Existing) then Exit;
  if Existing <> Expected then
    RaiseException('foreign or malformed transaction debt blocks clear: ' +
      Name);
  if not RegDeleteValue(HKLM64, BrandKey, Name) then
    RaiseException('cannot clear transaction debt: ' + Name);
  FlushMachineRegistryKey(BrandKey);
  if RegQueryStringValue(HKLM64, BrandKey, Name, Existing) then
    RaiseException('transaction debt clear readback failed: ' + Name);
end;

function ParseHelperCleanupDebt(const Value: String;
  var Owner, Kind, Nonce: String): Boolean;
var
  TypedKind: String;
  Separator: Integer;
begin
  Result := False;
  Owner := '';
  Kind := '';
  Nonce := '';
  if not ParseTransactionDebt(Value, Owner, TypedKind) then Exit;
  Separator := Pos(':', TypedKind);
  if Separator <= 1 then Exit;
  Kind := Copy(TypedKind, 1, Separator - 1);
  Nonce := Copy(TypedKind, Separator + 1, Length(TypedKind));
  Result :=
    ((Kind = DebtKindIdentityHelper) or
     (Kind = DebtKindMachineCleanupHelper)) and
    ValidTransactionId(Nonce) and
    (Nonce = Lowercase(Nonce)) and
    (TypedKind = Kind + ':' + Nonce);
  if not Result then
  begin
    Owner := '';
    Kind := '';
    Nonce := '';
  end;
end;

procedure ArmHelperCleanupDebt(const Kind, Nonce: String);
var
  Existing, Expected, Readback: String;
begin
  if not ValidTransactionId(TransactionId) or
     not ValidTransactionId(Nonce) or
     ((Kind <> DebtKindIdentityHelper) and
      (Kind <> DebtKindMachineCleanupHelper)) then
    RaiseException('invalid helper cleanup debt identity');
  Expected := TransactionDebtValue(TransactionId, Kind + ':' + Nonce);
  if RegQueryStringValue(HKLM64, BrandKey,
       HelperCleanupDebtName, Existing) and
     (Existing <> Expected) then
    RaiseException('foreign or malformed helper cleanup debt blocks write');
  RequireJournalWrite(BrandKey, HelperCleanupDebtName, Expected);
  FlushMachineRegistryKey(BrandKey);
  if not RegQueryStringValue(HKLM64, BrandKey,
       HelperCleanupDebtName, Readback) or
     (Readback <> Expected) then
    RaiseException('helper cleanup debt durability readback failed');
end;

procedure ClearHelperCleanupDebt(
  const Owner, Kind, Nonce: String);
var
  Existing, Expected: String;
begin
  Expected := TransactionDebtValue(Owner, Kind + ':' + Nonce);
  if not RegQueryStringValue(HKLM64, BrandKey,
       HelperCleanupDebtName, Existing) then
    RaiseException('helper cleanup debt missing before clear');
  if Existing <> Expected then
    RaiseException('foreign or malformed helper cleanup debt blocks clear');
  if not RegDeleteValue(HKLM64, BrandKey, HelperCleanupDebtName) then
    RaiseException('cannot clear helper cleanup debt');
  FlushMachineRegistryKey(BrandKey);
  if RegQueryStringValue(HKLM64, BrandKey,
       HelperCleanupDebtName, Existing) then
    RaiseException('helper cleanup debt clear readback failed');
end;

procedure ClosePinnedHelperHandle(var Handle: THandle);
begin
  if (Handle <> 0) and (Handle <> InvalidHandleValue) then
    CloseHandle(Handle);
  Handle := InvalidHandleValue;
end;

function TryOpenPinnedHelperDirectory(const Directory: String;
  var DirectoryHandle: THandle; var FinalPath, ObjectId: String): Boolean;
var
  Attributes, FinalLength: Cardinal;
  Exists: Boolean;
  Buffer: String;
  FileInformation: TFamoByHandleFileInformation;
begin
  Result := False;
  DirectoryHandle := InvalidHandleValue;
  FinalPath := '';
  ObjectId := '';
  if not TryGetPathAttributes(Directory, Exists, Attributes) or
     not Exists or
     ((Attributes and
       (FileAttributeDirectory or FileAttributeReparsePoint)) <>
       FileAttributeDirectory) then
    Exit;

  DirectoryHandle := CreateFileW(Directory, 0,
    FileShareRead or FileShareWrite, 0, OpenExisting,
    FileFlagBackupSemantics or FileFlagOpenReparsePoint, 0);
  if DirectoryHandle = InvalidHandleValue then Exit;

  SetLength(Buffer, FinalPathBufferChars);
  FinalLength := GetFinalPathNameByHandleW(DirectoryHandle, Buffer,
    FinalPathBufferChars, 0);
  if (FinalLength = 0) or (FinalLength >= FinalPathBufferChars) then
  begin
    ClosePinnedHelperHandle(DirectoryHandle);
    Exit;
  end;
  if not GetFileInformationByHandle(DirectoryHandle, FileInformation) then
  begin
    ClosePinnedHelperHandle(DirectoryHandle);
    Exit;
  end;
  if ((FileInformation.FileAttributes and
       (FileAttributeDirectory or FileAttributeReparsePoint)) <>
       FileAttributeDirectory) then
  begin
    ClosePinnedHelperHandle(DirectoryHandle);
    Exit;
  end;
  SetLength(Buffer, FinalLength);
  FinalPath := NormalizeFinalObjectPath(Buffer);
  if FinalPath = '' then
  begin
    ClosePinnedHelperHandle(DirectoryHandle);
    Exit;
  end;
  if (FileInformation.FileIndexHigh <> 0) or
     (FileInformation.FileIndexLow <> 0) then
    ObjectId := IntToStr(FileInformation.VolumeSerialNumber) + ':' +
      IntToStr(FileInformation.FileIndexHigh) + ':' +
      IntToStr(FileInformation.FileIndexLow);
  Result := ObjectId <> '';
  if not Result then
    ClosePinnedHelperHandle(DirectoryHandle);
end;

function PinExactHelperTree(const PendingRoot, HelperDirectory: String;
  var AppHandle, PendingHandle, HelperHandle: THandle;
  var HelperFinalPath: String): Boolean;
var
  AppRoot, ExpectedPendingRoot, AppFinalPath, AppObjectId,
    PendingFinalPath, PendingObjectId, HelperObjectId: String;
begin
  Result := False;
  AppHandle := InvalidHandleValue;
  PendingHandle := InvalidHandleValue;
  HelperHandle := InvalidHandleValue;
  HelperFinalPath := '';
  AppRoot := FixedInstallRoot;
  ExpectedPendingRoot := NormalizeDirectoryPath(
    AddBackslash(AppRoot) + 'pending');
  if not PathSame(PendingRoot, ExpectedPendingRoot) or
     not PathSame(ExtractFileDir(HelperDirectory), PendingRoot) then
    Exit;

  if not TryOpenPinnedHelperDirectory(AppRoot, AppHandle,
       AppFinalPath, AppObjectId) or
     not TryOpenPinnedHelperDirectory(PendingRoot, PendingHandle,
       PendingFinalPath, PendingObjectId) or
     not TryOpenPinnedHelperDirectory(HelperDirectory, HelperHandle,
       HelperFinalPath, HelperObjectId) or
     not PathSame(ExtractFileDir(PendingFinalPath), AppFinalPath) or
     (CompareText(ExtractFileName(PendingFinalPath), 'pending') <> 0) or
     not PathSame(ExtractFileDir(HelperFinalPath), PendingFinalPath) or
     (CompareText(ExtractFileName(HelperFinalPath),
       ExtractFileName(HelperDirectory)) <> 0) then
  begin
    ClosePinnedHelperHandle(HelperHandle);
    ClosePinnedHelperHandle(PendingHandle);
    ClosePinnedHelperHandle(AppHandle);
    Exit;
  end;
  Result := True;
end;

procedure FlushHelperCleanupVolume;
var
  AppRoot, Drive, VolumePath: String;
  VolumeHandle: THandle;
begin
  AppRoot := FixedInstallRoot;
  Drive := ExtractFileDrive(AppRoot);
  if Length(Drive) <> 2 then
    RaiseException('helper cleanup volume is not a fixed drive');
  if Drive[2] <> ':' then
    RaiseException('helper cleanup volume is not a fixed drive');
  VolumePath := '\\.\' + Uppercase(Drive);
  VolumeHandle := CreateFileW(VolumePath, GenericWrite,
    FileShareRead or FileShareWrite or FileShareDelete, 0, OpenExisting,
    FileAttributeNormal, 0);
  if VolumeHandle = InvalidHandleValue then
    RaiseException('cannot open helper cleanup volume for flush');
  try
    if not FlushFileBuffers(VolumeHandle) then
      RaiseException('cannot flush helper cleanup filesystem metadata');
  finally
    CloseHandle(VolumeHandle);
  end;
end;

function ValidateExactHelperFile(const FileName,
  HelperFinalPath: String): Boolean;
var
  Exists: Boolean;
  Attributes: Cardinal;
  FinalPath, ObjectId: String;
begin
  Result := False;
  if not TryGetPathAttributes(FileName, Exists, Attributes) then Exit;
  if not Exists then
  begin
    Result := True;
    Exit;
  end;
  if ((Attributes and
       (FileAttributeDirectory or FileAttributeReparsePoint)) <> 0) or
     not TryGetFinalObjectInfo(FileName, FinalPath, ObjectId) then
    Exit;
  Result :=
    PathSame(ExtractFileDir(FinalPath), HelperFinalPath) and
    (CompareText(ExtractFileName(FinalPath),
      ExtractFileName(FileName)) = 0);
end;

function ValidateExactHelperDirectory(const PendingRoot, HelperDirectory,
  FirstFile, SecondFile: String): Boolean;
var
  AppRoot, ExpectedPendingRoot, AppFinalPath, AppObjectId,
    PendingFinalPath, PendingObjectId, HelperFinalPath,
    HelperObjectId, Path: String;
  AppExists, PendingExists, HelperExists: Boolean;
  AppAttributes, PendingAttributes, HelperAttributes: Cardinal;
  FindRec: TFindRec;
begin
  Result := False;
  AppRoot := FixedInstallRoot;
  ExpectedPendingRoot := NormalizeDirectoryPath(
    AddBackslash(AppRoot) + 'pending');
  if not PathSame(PendingRoot, ExpectedPendingRoot) or
     not PathSame(ExtractFileDir(HelperDirectory), PendingRoot) or
     not TryGetPathAttributes(AppRoot, AppExists, AppAttributes) or
     not TryGetPathAttributes(PendingRoot, PendingExists,
       PendingAttributes) or
     not TryGetPathAttributes(HelperDirectory, HelperExists,
       HelperAttributes) or
     not AppExists or not PendingExists or not HelperExists or
     ((AppAttributes and
       (FileAttributeDirectory or FileAttributeReparsePoint)) <>
       FileAttributeDirectory) or
     ((PendingAttributes and
       (FileAttributeDirectory or FileAttributeReparsePoint)) <>
       FileAttributeDirectory) or
     ((HelperAttributes and
       (FileAttributeDirectory or FileAttributeReparsePoint)) <>
       FileAttributeDirectory) or
     not TryGetFinalObjectInfo(AppRoot, AppFinalPath, AppObjectId) or
     not TryGetFinalObjectInfo(PendingRoot, PendingFinalPath,
       PendingObjectId) or
     not TryGetFinalObjectInfo(HelperDirectory, HelperFinalPath,
       HelperObjectId) or
     not PathSame(ExtractFileDir(PendingFinalPath), AppFinalPath) or
     (CompareText(ExtractFileName(PendingFinalPath), 'pending') <> 0) or
     not PathSame(ExtractFileDir(HelperFinalPath),
       PendingFinalPath) or
     (CompareText(ExtractFileName(HelperFinalPath),
       ExtractFileName(HelperDirectory)) <> 0) then
    Exit;

  if FindFirst(AddBackslash(HelperDirectory) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Path := AddBackslash(HelperDirectory) + FindRec.Name;
          if ((CompareText(Path, FirstFile) <> 0) and
              (CompareText(Path, SecondFile) <> 0)) or
             not ValidateExactHelperFile(Path, HelperFinalPath) then
            Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  Result :=
    ValidateExactHelperFile(FirstFile, HelperFinalPath) and
    ValidateExactHelperFile(SecondFile, HelperFinalPath);
end;

function DeleteExactHelperFile(const FileName,
  HelperFinalPath: String): Boolean;
var
  Exists: Boolean;
  Attributes: Cardinal;
begin
  Result := False;
  if not TryGetPathAttributes(FileName, Exists, Attributes) then Exit;
  if not Exists then
  begin
    Result := True;
    Exit;
  end;
  if not ValidateExactHelperFile(FileName, HelperFinalPath) or
     not DeleteFile(FileName) or
     not TryGetPathAttributes(FileName, Exists, Attributes) then
    Exit;
  Result := not Exists;
end;

function DeleteExactHelperFileWithRetries(const FileName,
  HelperFinalPath: String; MaxAttempts: Integer): Boolean;
var
  Attempt: Integer;
begin
  Result := False;
  if MaxAttempts < 1 then Exit;
  for Attempt := 1 to MaxAttempts do
  begin
    if DeleteExactHelperFile(FileName, HelperFinalPath) then
    begin
      Result := True;
      Exit;
    end;
    if Attempt < MaxAttempts then Sleep(100);
  end;
end;

function CleanupExactHelperAndDebt(
  const Owner, Kind, Nonce: String; MaxDeleteAttempts: Integer;
  var Failure: String): Boolean;
var
  PendingRoot, HelperDirectory, FirstFile, SecondFile,
    HelperFinalPath: String;
  HelperExists: Boolean;
  HelperAttributes: Cardinal;
  AppHandle, PendingHandle, HelperHandle: THandle;
begin
  Result := False;
  Failure := '';
  AppHandle := InvalidHandleValue;
  PendingHandle := InvalidHandleValue;
  HelperHandle := InvalidHandleValue;
  if not ValidTransactionId(Owner) or
     not ValidTransactionId(Nonce) or
     (MaxDeleteAttempts < 1) or
     ((Kind <> DebtKindIdentityHelper) and
      (Kind <> DebtKindMachineCleanupHelper)) then
  begin
    Failure := 'invalid helper cleanup request';
    Exit;
  end;

  PendingRoot := NormalizeDirectoryPath(
    AddBackslash(FixedInstallRoot) + 'pending');
  if Kind = DebtKindIdentityHelper then
  begin
    HelperDirectory := NormalizeDirectoryPath(
      AddBackslash(PendingRoot) + 'identity-' + Nonce);
    FirstFile := AddBackslash(HelperDirectory) +
      'FamoIdentityBroker-' + Nonce + '.exe';
    SecondFile := AddBackslash(HelperDirectory) +
      'identity-' + Nonce + '.txt';
  end
  else
  begin
    HelperDirectory := NormalizeDirectoryPath(
      AddBackslash(PendingRoot) + 'machine-cleanup-' + Nonce);
    FirstFile := AddBackslash(HelperDirectory) +
      'payload-manifest.txt';
    SecondFile := AddBackslash(HelperDirectory) +
      'FamoMachineCleanup.exe';
  end;

  if not TryGetPathAttributes(HelperDirectory, HelperExists,
       HelperAttributes) then
  begin
    Failure := 'helper cleanup path state is unavailable';
    Exit;
  end;
  if not HelperExists then
  begin
    try
      FailIfRequested('after-helper-remove-before-volume-flush');
      FlushHelperCleanupVolume;
      FailIfRequested('after-helper-volume-flush-before-debt-clear');
      ClearHelperCleanupDebt(Owner, Kind, Nonce);
      Result := True;
    except
      Failure := GetExceptionMessage;
    end;
    Exit;
  end;

  if not PinExactHelperTree(PendingRoot, HelperDirectory,
       AppHandle, PendingHandle, HelperHandle, HelperFinalPath) then
  begin
    Failure := 'helper cleanup directory identity is invalid';
    Exit;
  end;
  try
    if not ValidateExactHelperDirectory(PendingRoot, HelperDirectory,
         FirstFile, SecondFile) then
    begin
      Failure := 'helper cleanup directory contents are invalid';
      Exit;
    end;
    if not DeleteExactHelperFileWithRetries(
         FirstFile, HelperFinalPath, MaxDeleteAttempts) or
       not DeleteExactHelperFileWithRetries(
         SecondFile, HelperFinalPath, MaxDeleteAttempts) then
    begin
      Failure := 'cannot remove an exact helper cleanup file';
      Exit;
    end;

    ClosePinnedHelperHandle(HelperHandle);
    if not RemoveDir(HelperDirectory) or
       not TryGetPathAttributes(HelperDirectory, HelperExists,
         HelperAttributes) or HelperExists then
    begin
      Failure := 'cannot remove the exact helper cleanup directory';
      Exit;
    end;
    try
      FailIfRequested('after-helper-remove-before-volume-flush');
      FlushHelperCleanupVolume;
      FailIfRequested('after-helper-volume-flush-before-debt-clear');
      ClearHelperCleanupDebt(Owner, Kind, Nonce);
      Result := True;
    except
      Failure := GetExceptionMessage;
    end;
  finally
    ClosePinnedHelperHandle(HelperHandle);
    ClosePinnedHelperHandle(PendingHandle);
    ClosePinnedHelperHandle(AppHandle);
  end;
end;

function RecoverHelperCleanupDebt: Boolean;
var
  Stored, Owner, Kind, Nonce, Failure: String;
begin
  Result := False;
  if not RegQueryStringValue(HKLM64, BrandKey,
       HelperCleanupDebtName, Stored) then
  begin
    Result := True;
    Exit;
  end;
  if not ParseHelperCleanupDebt(Stored, Owner, Kind, Nonce) then
  begin
    Log('foreign or malformed helper cleanup debt blocks recovery');
    Exit;
  end;

  Result := CleanupExactHelperAndDebt(
    Owner, Kind, Nonce, 1, Failure);
  if not Result then
    Log('cannot recover exact helper cleanup debt: ' + Failure);
end;

function TransactionDebtPresent(const Name, Kind: String): Boolean;
var
  Existing: String;
begin
  Result := RegQueryStringValue(HKLM64, BrandKey, Name, Existing);
  if Result and
     (Existing <> TransactionDebtValue(TransactionId, Kind)) then
    RaiseException('foreign or malformed transaction debt blocks recovery: ' +
      Name);
end;

procedure ClearExactLegacyRegistryValue(
  const Name, Expected: String);
var
  Existing: String;
begin
  if not RegQueryStringValue(HKLM64, BrandKey, Name, Existing) then Exit;
  if Existing <> Expected then
    RaiseException('legacy cleanup debt changed before clear: ' + Name);
  if not RegDeleteValue(HKLM64, BrandKey, Name) then
    RaiseException('cannot clear legacy cleanup debt: ' + Name);
  FlushMachineRegistryKey(BrandKey);
  if RegQueryStringValue(HKLM64, BrandKey, Name, Existing) then
    RaiseException('legacy cleanup debt clear readback failed: ' + Name);
end;

function ValidLegacyVersionCleanupDebt(
  const Value: String; var EntryCount: Integer): Boolean;
var
  Position, Separator, PathLength: Integer;
  Remaining, LengthText, Path: String;
begin
  Result := False;
  EntryCount := 0;
  Position := 1;
  while Position <= Length(Value) do
  begin
    Remaining := Copy(Value, Position, Length(Value));
    Separator := Pos(':', Remaining);
    if Separator <= 1 then Exit;
    LengthText := Copy(Remaining, 1, Separator - 1);
    PathLength := StrToIntDef(LengthText, -1);
    if (PathLength <= 0) or
       (LengthText <> IntToStr(PathLength)) or
       (PathLength > Length(Remaining) - Separator - 1) then Exit;
    Path := Copy(Remaining, Separator + 1, PathLength);
    if (Remaining[Separator + PathLength + 1] <> ';') or
       not PathIsRooted(Path) or ContainsParentTraversal(Path) then Exit;
    Position := Position + Separator + PathLength + 1;
    EntryCount := EntryCount + 1;
  end;
  Result := (Position = Length(Value) + 1) and (EntryCount > 0);
end;

function ExactStoredTransactionDebt(
  const Name, Owner, Kind: String): Boolean;
var
  Value: String;
begin
  Result :=
    RegQueryStringValue(HKLM64, BrandKey, Name, Value) and
    (Value = TransactionDebtValue(Owner, Kind));
end;

function ValidLegacyVersionCleanupDebtForOwner(
  const Value, Owner: String; var EntryCount: Integer): Boolean;
var
  CountText, ActiveId: String;
begin
  Result := False;
  if not ValidTransactionId(Owner) or
     not ValidLegacyVersionCleanupDebt(Value, EntryCount) or
     not RegQueryStringValue(HKLM64, BrandKey,
       'ActiveTransactionId', ActiveId) or
     (ActiveId <> Owner) then Exit;
  if RegQueryStringValue(HKLM64, BrandKey,
       'CleanupDebtCount', CountText) then
  begin
    Result := CountText = IntToStr(EntryCount);
    Exit;
  end;
  if RegValueExists(HKLM64, BrandKey, 'CleanupDebtCount') then Exit;
  { Count-first deletion can crash after the count is durably absent.
    The still-durable typed retention debt is the only evidence that a
    structurally valid list without its count is our partial migration. }
  Result := ExactStoredTransactionDebt(
    'VersionCleanupDebt', Owner, DebtKindVersionRetention);
end;

procedure MigrateLegacyRollbackCleanupDebt;
var
  Existing: String;
begin
  if not RegQueryStringValue(HKLM64, BrandKey,
       'CleanupDebt', Existing) then
  begin
    if RegValueExists(HKLM64, BrandKey, 'CleanupDebtCount') then
      RaiseException(
        'orphaned legacy cleanup count blocks rollback recovery');
    Exit;
  end;
  if Existing <> TransactionId then
    RaiseException(
      'non-rollback legacy cleanup debt blocks rollback recovery');
  if RegValueExists(HKLM64, BrandKey, 'CleanupDebtCount') then
    RaiseException(
      'legacy rollback cleanup debt has an unexpected count value');
  ArmTransactionDebt('TargetCleanupDebt', DebtKindTargetCleanup);
  ClearExactLegacyRegistryValue('CleanupDebt', Existing);
end;

procedure ClearAdoptedLegacyVersionCleanupDebt;
var
  Existing, CountText: String;
  EntryCount: Integer;
begin
  if not RegQueryStringValue(HKLM64, BrandKey,
       'CleanupDebt', Existing) then
  begin
    if RegValueExists(HKLM64, BrandKey, 'CleanupDebtCount') then
      RaiseException(
        'orphaned legacy version cleanup count blocks adoption');
    Exit;
  end;
  if not ValidLegacyVersionCleanupDebtForOwner(
       Existing, TransactionId, EntryCount) then
    RaiseException('malformed legacy version cleanup debt blocks adoption');
  if RegQueryStringValue(HKLM64, BrandKey,
       'CleanupDebtCount', CountText) then
    ClearExactLegacyRegistryValue('CleanupDebtCount', CountText)
  else if not ExactStoredTransactionDebt(
       'VersionCleanupDebt',
       TransactionId,
       DebtKindVersionRetention) then
    RaiseException(
      'partial legacy version cleanup lacks its typed debt');
  ClearExactLegacyRegistryValue('CleanupDebt', Existing);
end;

procedure BuildCurrentJournal(const NextPhase: String;
  NextGeneration: Integer; var Journal: TTransactionJournal);
begin
  Journal.Generation := IntToStr(NextGeneration);
  Journal.Phase := NextPhase;
  Journal.Version := '{#AppVersion}';
  Journal.Transaction := TransactionId;
  Journal.ManifestHash := JournalManifestHash;
  Journal.PendingTarget := TransactionTarget;
  Journal.PendingFinalTarget := JournalPendingFinalTarget;
  Journal.PendingObjectId := JournalPendingObjectId;
  Journal.PreviousTarget := PreviousTarget;
  Journal.PreviousFinalTarget := JournalPreviousFinalTarget;
  Journal.PreviousObjectId := JournalPreviousObjectId;
  Journal.PriorPreviousTarget := PriorPreviousTarget;
  Journal.PriorPreviousFinalTarget := JournalPriorPreviousFinalTarget;
  Journal.PriorPreviousObjectId := JournalPriorPreviousObjectId;
  Journal.PreviousManifest := PreviousManifest;
  Journal.PreviousManifestHash := PreviousManifestHash;
  Journal.PreviousDefault := PreviousDefault;
  Journal.PreviousHost := PreviousHost;
  Journal.PreviousServer := PreviousServer;
  Journal.PreviousProfileTool := PreviousProfileTool;
  Journal.PreviousVersion := PreviousVersion;
  Journal.PreviousIdentity := PreviousIdentity;
  Journal.PreviousTransactionId := PreviousTransactionId;
  Journal.PreviousCompatibilityTransactionId :=
    PreviousCompatibilityTransactionId;
  Journal.PreviousState := PreviousState;
  Journal.PreviousProfileActive := IntToStr(Ord(PreviousProfileActive));
  Journal.PreviousProfileEnabled := IntToStr(Ord(PreviousProfileEnabled));
  Journal.PreviousInputTipPresent :=
    IntToStr(Ord(PreviousInputTipPresent));
  Journal.SeedReceiptHash := SeedReceiptHash;
  Journal.OriginalUserSid := OriginalUserSid;
  Journal.OriginalUserAccount := OriginalUserAccount;
  Journal.OriginalUserSession := OriginalUserSession;
  Journal.LastProofSession := CurrentOriginalUserSession;
  if Journal.LastProofSession = '' then
    Journal.LastProofSession := OriginalUserSession;
  Journal.OriginalUserResumeCapable :=
    IntToStr(Ord(OriginalUserResumeCapable));
  Journal.ResumeInstaller := JournalResumeInstaller;
  Journal.ResumeInstallerHash := JournalResumeInstallerHash;
  Journal.ResumeTaskName := JournalTaskName;
  Journal.AllowDowngrade := IntToStr(Ord(JournalAllowDowngrade));
  Journal.LoadedHostHash := LoadedHostHash;
  Journal.LoadedHostVersion := LoadedHostVersion;
  Journal.LoadedHostExpectedHash := LoadedHostExpectedHash;
end;

procedure WriteJournalGeneration(const Key: String;
  const Journal: TTransactionJournal);
begin
  RequireJournalWrite(Key, 'Schema', JournalVersion);
  RequireJournalWrite(Key, 'Product', 'Famo');
  RequireJournalWrite(Key, 'Generation', Journal.Generation);
  RequireJournalWrite(Key, 'Version', Journal.Version);
  RequireJournalWrite(Key, 'TransactionId', Journal.Transaction);
  RequireJournalWrite(Key, 'ManifestHash', Journal.ManifestHash);
  RequireJournalWrite(Key, 'PendingTarget', Journal.PendingTarget);
  RequireJournalWrite(Key, 'PendingFinalTarget',
    Journal.PendingFinalTarget);
  RequireJournalWrite(Key, 'PendingObjectId', Journal.PendingObjectId);
  RequireJournalWrite(Key, 'PreviousTarget', Journal.PreviousTarget);
  RequireJournalWrite(Key, 'PreviousFinalTarget',
    Journal.PreviousFinalTarget);
  RequireJournalWrite(Key, 'PreviousObjectId', Journal.PreviousObjectId);
  RequireJournalWrite(Key, 'PriorPreviousTarget',
    Journal.PriorPreviousTarget);
  RequireJournalWrite(Key, 'PriorPreviousFinalTarget',
    Journal.PriorPreviousFinalTarget);
  RequireJournalWrite(Key, 'PriorPreviousObjectId',
    Journal.PriorPreviousObjectId);
  RequireJournalWrite(Key, 'PreviousManifest', Journal.PreviousManifest);
  RequireJournalWrite(Key, 'PreviousManifestHash',
    Journal.PreviousManifestHash);
  RequireJournalWrite(Key, 'PreviousDefault', Journal.PreviousDefault);
  RequireJournalWrite(Key, 'PreviousHost', Journal.PreviousHost);
  RequireJournalWrite(Key, 'PreviousServer', Journal.PreviousServer);
  RequireJournalWrite(Key, 'PreviousProfileTool',
    Journal.PreviousProfileTool);
  RequireJournalWrite(Key, 'PreviousVersion', Journal.PreviousVersion);
  RequireJournalWrite(Key, 'PreviousIdentity', Journal.PreviousIdentity);
  RequireJournalWrite(Key, 'PreviousTransactionId',
    Journal.PreviousTransactionId);
  RequireJournalWrite(Key, 'PreviousCompatibilityTransactionId',
    Journal.PreviousCompatibilityTransactionId);
  RequireJournalWrite(Key, 'PreviousState', Journal.PreviousState);
  RequireJournalWrite(Key, 'PreviousProfileActive',
    Journal.PreviousProfileActive);
  RequireJournalWrite(Key, 'PreviousProfileEnabled',
    Journal.PreviousProfileEnabled);
  RequireJournalWrite(Key, 'PreviousInputTipPresent',
    Journal.PreviousInputTipPresent);
  RequireJournalWrite(Key, 'SeedReceiptHash', Journal.SeedReceiptHash);
  RequireJournalWrite(Key, 'OriginalUserSid', Journal.OriginalUserSid);
  RequireJournalWrite(Key, 'OriginalUserAccount',
    Journal.OriginalUserAccount);
  RequireJournalWrite(Key, 'OriginalUserSession',
    Journal.OriginalUserSession);
  RequireJournalWrite(Key, 'LastProofSession', Journal.LastProofSession);
  RequireJournalWrite(Key, 'OriginalUserResumeCapable',
    Journal.OriginalUserResumeCapable);
  RequireJournalWrite(Key, 'ResumeInstaller', Journal.ResumeInstaller);
  RequireJournalWrite(Key, 'ResumeInstallerHash',
    Journal.ResumeInstallerHash);
  RequireJournalWrite(Key, 'ResumeTaskName', Journal.ResumeTaskName);
  RequireJournalWrite(Key, 'AllowDowngrade', Journal.AllowDowngrade);
  RequireJournalWrite(Key, 'LoadedHostHash', Journal.LoadedHostHash);
  RequireJournalWrite(Key, 'LoadedHostVersion', Journal.LoadedHostVersion);
  RequireJournalWrite(Key, 'LoadedHostExpectedHash',
    Journal.LoadedHostExpectedHash);
  RequireJournalWrite(Key, 'Digest', JournalDigest(Journal));
  { Phase is the last field inside an immutable generation. The generation
    remains unreachable until ActiveGeneration is committed below. }
  RequireJournalWrite(Key, 'Phase', Journal.Phase);
end;

function ReadJournalValue(const Key, Name: String;
  var Value: String): Boolean;
begin
  Result := RegQueryStringValue(HKLM64, Key, Name, Value);
end;

function ReadJournalGeneration(const Key: String;
  var Journal: TTransactionJournal): Boolean;
var
  Schema, Product, StoredDigest: String;
begin
  Result :=
    ReadJournalValue(Key, 'Schema', Schema) and
    (Schema = JournalVersion) and
    ReadJournalValue(Key, 'Product', Product) and
    (Product = 'Famo') and
    ReadJournalValue(Key, 'Generation', Journal.Generation) and
    ReadJournalValue(Key, 'Phase', Journal.Phase) and
    ReadJournalValue(Key, 'Version', Journal.Version) and
    ReadJournalValue(Key, 'TransactionId', Journal.Transaction) and
    ReadJournalValue(Key, 'ManifestHash', Journal.ManifestHash) and
    ReadJournalValue(Key, 'PendingTarget', Journal.PendingTarget) and
    ReadJournalValue(Key, 'PendingFinalTarget',
      Journal.PendingFinalTarget) and
    ReadJournalValue(Key, 'PendingObjectId', Journal.PendingObjectId) and
    ReadJournalValue(Key, 'PreviousTarget', Journal.PreviousTarget) and
    ReadJournalValue(Key, 'PreviousFinalTarget',
      Journal.PreviousFinalTarget) and
    ReadJournalValue(Key, 'PreviousObjectId', Journal.PreviousObjectId) and
    ReadJournalValue(Key, 'PriorPreviousTarget',
      Journal.PriorPreviousTarget) and
    ReadJournalValue(Key, 'PriorPreviousFinalTarget',
      Journal.PriorPreviousFinalTarget) and
    ReadJournalValue(Key, 'PriorPreviousObjectId',
      Journal.PriorPreviousObjectId) and
    ReadJournalValue(Key, 'PreviousManifest', Journal.PreviousManifest) and
    ReadJournalValue(Key, 'PreviousManifestHash',
      Journal.PreviousManifestHash) and
    ReadJournalValue(Key, 'PreviousDefault', Journal.PreviousDefault) and
    ReadJournalValue(Key, 'PreviousHost', Journal.PreviousHost) and
    ReadJournalValue(Key, 'PreviousServer', Journal.PreviousServer) and
    ReadJournalValue(Key, 'PreviousProfileTool',
      Journal.PreviousProfileTool) and
    ReadJournalValue(Key, 'PreviousVersion', Journal.PreviousVersion) and
    ReadJournalValue(Key, 'PreviousIdentity', Journal.PreviousIdentity) and
    ReadJournalValue(Key, 'PreviousTransactionId',
      Journal.PreviousTransactionId) and
    ReadJournalValue(Key, 'PreviousCompatibilityTransactionId',
      Journal.PreviousCompatibilityTransactionId) and
    ReadJournalValue(Key, 'PreviousState', Journal.PreviousState) and
    ReadJournalValue(Key, 'PreviousProfileActive',
      Journal.PreviousProfileActive) and
    ReadJournalValue(Key, 'PreviousProfileEnabled',
      Journal.PreviousProfileEnabled) and
    ReadJournalValue(Key, 'PreviousInputTipPresent',
      Journal.PreviousInputTipPresent) and
    ReadJournalValue(Key, 'SeedReceiptHash', Journal.SeedReceiptHash) and
    ReadJournalValue(Key, 'OriginalUserSid', Journal.OriginalUserSid) and
    ReadJournalValue(Key, 'OriginalUserAccount',
      Journal.OriginalUserAccount) and
    ReadJournalValue(Key, 'OriginalUserSession',
      Journal.OriginalUserSession) and
    ReadJournalValue(Key, 'LastProofSession',
      Journal.LastProofSession) and
    ReadJournalValue(Key, 'OriginalUserResumeCapable',
      Journal.OriginalUserResumeCapable) and
    ReadJournalValue(Key, 'ResumeInstaller', Journal.ResumeInstaller) and
    ReadJournalValue(Key, 'ResumeInstallerHash',
      Journal.ResumeInstallerHash) and
    ReadJournalValue(Key, 'ResumeTaskName', Journal.ResumeTaskName) and
    ReadJournalValue(Key, 'AllowDowngrade', Journal.AllowDowngrade) and
    ReadJournalValue(Key, 'LoadedHostHash', Journal.LoadedHostHash) and
    ReadJournalValue(Key, 'LoadedHostVersion', Journal.LoadedHostVersion) and
    ReadJournalValue(Key, 'LoadedHostExpectedHash',
      Journal.LoadedHostExpectedHash) and
    ReadJournalValue(Key, 'Digest', StoredDigest);
  if Result then
    Result := IsSha256Hex(Journal.ManifestHash) and
      IsSha256Hex(StoredDigest) and
      (CompareText(StoredDigest, JournalDigest(Journal)) = 0);
end;

function ValidateJournalGeneration(const Key, ExpectedPhase: String;
  ExpectedGeneration: Integer): Boolean;
var
  Journal: TTransactionJournal;
begin
  Result := ReadJournalGeneration(Key, Journal) and
    (Journal.Generation = IntToStr(ExpectedGeneration)) and
    (CompareText(Journal.Phase, ExpectedPhase) = 0) and
    (CompareText(Journal.Transaction, TransactionId) = 0);
end;

function KnownJournalPhase(const Phase: String): Boolean;
begin
  Result :=
    (Phase = PhasePrepared) or
    (Phase = PhasePayloadVerified) or
    (Phase = PhaseResumeArmed) or
    (Phase = PhaseDetachIntent) or
    (Phase = PhasePendingReboot) or
    (Phase = PhaseActivateIntent) or
    (Phase = PhaseMachineRegistered) or
    (Phase = PhaseUserStateIntent) or
    (Phase = PhaseUserStatePrepared) or
    (Phase = PhaseUserStateApplied) or
    (Phase = PhaseVerifyIntent) or
    (Phase = PhaseRollbackIntent) or
    (Phase = PhaseReady) or
    (Phase = PhaseRolledBack);
end;

function ValidJournalObjectId(const Value: String): Boolean;
var
  I, Colons: Integer;
begin
  Result := Value = '';
  if Result then Exit;
  Colons := 0;
  for I := 1 to Length(Value) do
  begin
    if Value[I] = ':' then
      Colons := Colons + 1
    else if not ((Value[I] >= '0') and (Value[I] <= '9')) then
      Exit;
  end;
  Result := (Colons = 2) and (Value[1] <> ':') and
    (Value[Length(Value)] <> ':') and (Pos('::', Value) = 0);
end;

function ValidPendingObjectIdForPhase(
  const Journal: TTransactionJournal): Boolean;
begin
  if Journal.PendingObjectId <> '' then
  begin
    Result := ValidJournalObjectId(Journal.PendingObjectId);
    Exit;
  end;
  { Journal semantics must remain stable for historical generations after a
    later generation creates the target. LoadPendingState separately requires
    a pinned object ID whenever the current target exists. }
  Result :=
    (Journal.Phase = PhasePrepared) or
    (Journal.Phase = PhaseRollbackIntent) or
    (Journal.Phase = PhaseRolledBack);
end;

function ValidateJournalSemantics(const Journal: TTransactionJournal;
  const ExpectedId: String): Boolean;
var
  ExpectedTarget, ExpectedRecovery, ExpectedTask, NormalizedVersion: String;
  Generation: Integer;
begin
  Result := False;
  try
    Generation := StrToIntDef(Journal.Generation, 0);
    ExpectedTarget := NormalizeDirectoryPath(
      AddBackslash(FixedInstallRoot) +
      'versions\' + Journal.Version + '-' +
      Copy(Journal.ManifestHash, 1, 12) + '-' + ExpectedId);
    ExpectedRecovery := NormalizeDirectoryPath(
      AddBackslash(FixedInstallRoot) + 'pending\' + ExpectedId +
      '\Famo-Resume-' + ExpectedId + '.exe');
    ExpectedTask := '\Famo\Transaction-' + ExpectedId;
    Result :=
      ValidTransactionId(ExpectedId) and
      (CompareText(Journal.Transaction, ExpectedId) = 0) and
      (Generation > 0) and
      (Journal.Generation = IntToStr(Generation)) and
      KnownJournalPhase(Journal.Phase) and
      NormalizeSafeRelativePath(Journal.Version, NormalizedVersion) and
      (Pos('\', NormalizedVersion) = 0) and
      IsSha256Hex(Journal.ManifestHash) and
      (CompareText(NormalizeDirectoryPath(Journal.PendingTarget),
        ExpectedTarget) = 0) and
      (Journal.PendingFinalTarget <> '') and
      not ContainsParentTraversal(Journal.PendingFinalTarget) and
      PathIsRooted(Journal.PendingFinalTarget) and
      (CompareText(NormalizeDirectoryPath(Journal.PendingFinalTarget),
        ExpectedTarget) = 0) and
      ValidPendingObjectIdForPhase(Journal) and
      (((Journal.PreviousTarget = '') and
        (Journal.PreviousFinalTarget = '') and
        (Journal.PreviousObjectId = '')) or
       ((Journal.PreviousTarget <> '') and
        PathIsRooted(Journal.PreviousTarget) and
        not ContainsParentTraversal(Journal.PreviousTarget) and
        (Journal.PreviousFinalTarget <> ''))) and
      ValidJournalObjectId(Journal.PreviousObjectId) and
      (((Journal.PreviousTarget = '') and
        (Journal.PreviousManifestHash = '')) or
       ((Journal.PreviousTarget <> '') and
        IsSha256Hex(Journal.PreviousManifestHash))) and
      (((Journal.PriorPreviousTarget = '') and
        (Journal.PriorPreviousFinalTarget = '') and
        (Journal.PriorPreviousObjectId = '')) or
       ((Journal.PriorPreviousTarget <> '') and
        PathIsRooted(Journal.PriorPreviousTarget) and
        not ContainsParentTraversal(Journal.PriorPreviousTarget) and
        (Journal.PriorPreviousFinalTarget <> ''))) and
      ValidJournalObjectId(Journal.PriorPreviousObjectId) and
      (((Journal.PreviousTarget = '') and
        (Journal.PreviousTransactionId = '') and
        (Journal.PreviousCompatibilityTransactionId = '') and
       (Journal.PreviousState = '')) or
       ((Journal.PreviousTarget <> '') and
        (Journal.PreviousState = StateReady) and
        (((Journal.PreviousTransactionId = '') and
          ValidLegacyTransactionId(
            Journal.PreviousCompatibilityTransactionId)) or
         (ValidTransactionId(Journal.PreviousTransactionId) and
          (CompareText(Journal.PreviousTransactionId,
            Journal.PreviousCompatibilityTransactionId) = 0))))) and
      ((Journal.PreviousProfileActive = '0') or
       (Journal.PreviousProfileActive = '1')) and
      ((Journal.PreviousProfileEnabled = '0') or
       (Journal.PreviousProfileEnabled = '1')) and
      ((Journal.PreviousInputTipPresent = '0') or
       (Journal.PreviousInputTipPresent = '1')) and
      ((Journal.PreviousTarget <> '') or
       ((Journal.PreviousProfileActive = '0') and
        (Journal.PreviousProfileEnabled = '0') and
        (Journal.PreviousInputTipPresent = '0'))) and
      ((Journal.SeedReceiptHash = '') or
       IsSha256Hex(Journal.SeedReceiptHash)) and
      ValidSidText(Journal.OriginalUserSid) and
      (Journal.OriginalUserAccount <> '') and
      (Pos(Chr(13), Journal.OriginalUserAccount) = 0) and
      (Pos(Chr(10), Journal.OriginalUserAccount) = 0) and
      (StrToIntDef(Journal.OriginalUserSession, 0) > 0) and
      (StrToIntDef(Journal.LastProofSession, 0) > 0) and
      ((Journal.OriginalUserResumeCapable = '0') or
       (Journal.OriginalUserResumeCapable = '1')) and
      ((Journal.AllowDowngrade = '0') or
       (Journal.AllowDowngrade = '1')) and
      ((Journal.ResumeInstaller = '') or
       ((CompareText(NormalizeDirectoryPath(Journal.ResumeInstaller),
          ExpectedRecovery) = 0) and
        IsSha256Hex(Journal.ResumeInstallerHash))) and
      ((Journal.ResumeInstaller <> '') or
       ((Journal.ResumeInstallerHash = '') and
        (Journal.ResumeTaskName = ''))) and
      ((Journal.ResumeTaskName = '') or
       ((Journal.OriginalUserResumeCapable = '1') and
        (CompareText(Journal.ResumeTaskName, ExpectedTask) = 0))) and
      ((Journal.Phase <> PhasePendingReboot) or
       ((Journal.ResumeInstaller <> '') and
        (Journal.ResumeTaskName <> '') and
        (Journal.OriginalUserResumeCapable = '1'))) and
      ((Journal.LoadedHostHash = '') or
       IsSha256Hex(Journal.LoadedHostHash)) and
      ((Journal.LoadedHostExpectedHash = '') or
       IsSha256Hex(Journal.LoadedHostExpectedHash));
  except
    Result := False;
  end;
end;

function ValidateCurrentJournalArtifact(const Journal: TTransactionJournal;
  const ExpectedId: String): Boolean;
begin
  Result := ValidateJournalSemantics(Journal, ExpectedId) and
    (CompareText(Journal.Version, '{#AppVersion}') = 0) and
    (CompareText(Journal.ManifestHash, '{#ManifestHash}') = 0);
end;

function IsVersionNotNewerThanInstaller(const Value: String): Boolean;
var
  CandidateVersion, InstallerVersion: Int64;
begin
  Result :=
    StrToVersion(Value + '.0', CandidateVersion) and
    StrToVersion('{#AppVersion}.0', InstallerVersion) and
    (ComparePackedVersion(CandidateVersion, InstallerVersion) <= 0);
end;

function ValidateRecoverableJournalArtifact(
  const Journal: TTransactionJournal; const ExpectedId: String): Boolean;
begin
  Result :=
    ValidateCurrentJournalArtifact(Journal, ExpectedId) or
    (ValidateJournalSemantics(Journal, ExpectedId) and
     IsVersionNotNewerThanInstaller(Journal.Version) and
     ((Journal.Phase = PhaseReady) or
      (Journal.Phase = PhaseRolledBack)));
end;

function ValidatePreviousV2Transaction(const Id, Target,
  Manifest: String): Boolean;
var
  GenerationText, Key, TargetFinalPath, TargetObjectId,
    ManifestFinalPath, ManifestObjectId: String;
  Generation: Integer;
  Journal: TTransactionJournal;
begin
  Result := False;
  if not ValidTransactionId(Id) or
     not RegQueryStringValue(HKLM64, TransactionJournalKey(Id),
       'ActiveGeneration', GenerationText) then
    Exit;
  Generation := StrToIntDef(GenerationText, 0);
  if (Generation <= 0) or
     (GenerationText <> IntToStr(Generation)) then
    Exit;
  Key := JournalGenerationKey(Id, Generation);
  Result := ReadJournalGeneration(Key, Journal) and
    ValidateJournalSemantics(Journal, Id) and
    (Journal.Phase = PhaseReady) and
    (CompareText(NormalizeDirectoryPath(Journal.PendingTarget),
      NormalizeDirectoryPath(Target)) = 0) and
    (CompareText(Manifest,
      AddBackslash(Target) + 'payload-manifest.txt') = 0) and
    FileExists(Manifest) and
    (CompareText(GetSHA256OfFile(Manifest), Journal.ManifestHash) = 0) and
    (CompareText(PreviousManifestHash, Journal.ManifestHash) = 0) and
    TryGetFinalObjectInfo(Target, TargetFinalPath, TargetObjectId) and
    TryGetFinalObjectInfo(Manifest, ManifestFinalPath, ManifestObjectId) and
    PathSame(ExtractFileDir(ManifestFinalPath), TargetFinalPath) and
    FinalObjectsSame(TargetFinalPath, TargetObjectId,
      Journal.PendingFinalTarget, Journal.PendingObjectId) and
    VerifyManagedPayloadForCleanup(Target, TargetFinalPath, Manifest,
      ManifestFinalPath, Journal.Version,
      Copy(Journal.ManifestHash, 1, 12));
end;

function ValidatePreviousPayloadForExecution: Boolean;
var
  TargetFinalPath, TargetObjectId, ManifestFinalPath,
    ManifestObjectId: String;
begin
  if PreviousTarget = '' then
  begin
    Result := True;
    Exit;
  end;
  Result :=
    (PreviousState = StateReady) and
    (CompareText(PreviousManifest,
      AddBackslash(PreviousTarget) + 'payload-manifest.txt') = 0) and
    PreviousBridgeSnapshotValid and
    (CompareText(PreviousProfileTool,
      AddBackslash(PreviousTarget) + 'FamoProfileTool.exe') = 0) and
    (CompareText(PreviousServer,
      AddBackslash(PreviousTarget) + 'FamoRuntime.exe') = 0) and
    IsSha256Hex(PreviousManifestHash) and
    FileExists(PreviousManifest) and
    FileExists(PreviousProfileTool) and FileExists(PreviousServer) and
    TryGetFinalObjectInfo(PreviousTarget, TargetFinalPath,
      TargetObjectId) and
    TryGetFinalObjectInfo(PreviousManifest, ManifestFinalPath,
      ManifestObjectId) and
    PathSame(ExtractFileDir(ManifestFinalPath), TargetFinalPath) and
    FinalObjectsSame(TargetFinalPath, TargetObjectId,
      JournalPreviousFinalTarget, JournalPreviousObjectId) and
    (CompareText(GetSHA256OfFile(PreviousManifest),
      PreviousManifestHash) = 0);
  if not Result then Exit;
  if PreviousTransactionId <> '' then
    Result := ValidatePreviousV2Transaction(PreviousTransactionId,
      PreviousTarget, PreviousManifest)
  else
    Result := ValidateLegacyPreviousSnapshot;
end;

function ReadActiveJournal(const Id: String;
  var Journal: TTransactionJournal): Boolean;
var
  GenerationText, Key: String;
  Generation: Integer;
begin
  Result := False;
  if not ValidTransactionId(Id) or
     not RegQueryStringValue(HKLM64, TransactionJournalKey(Id),
       'ActiveGeneration', GenerationText) then
    Exit;
  Generation := StrToIntDef(GenerationText, 0);
  if (Generation <= 0) or
     (GenerationText <> IntToStr(Generation)) then
    Exit;
  Key := JournalGenerationKey(Id, Generation);
  Result := ReadJournalGeneration(Key, Journal) and
    ValidateJournalSemantics(Journal, Id);
end;

function IsEmptyRollbackAnchorForProjection(const Id: String): Boolean;
var
  Journal: TTransactionJournal;
  Value: String;
begin
  Result :=
    ReadActiveJournal(Id, Journal) and
    (Journal.Phase = PhaseRolledBack) and
    (Journal.PreviousTarget = '') and
    (Journal.PreviousManifest = '') and
    (Journal.PreviousHost = '') and
    (Journal.PreviousTransactionId = '') and
    (Journal.PreviousCompatibilityTransactionId = '') and
    (Journal.PreviousState = '') and
    RegQueryStringValue(HKLM64, BrandKey, 'ActiveTransactionId', Value) and
    (CompareText(Value, Id) = 0) and
    not RegQueryStringValue(HKLM64, BrandKey, 'TransactionId', Value) and
    RegQueryStringValue(HKLM64, BrandKey, 'InstallState', Value) and
    (Value = StateRolledBack) and
    not RegQueryStringValue(HKLM64, BrandKey, 'InstallDir', Value) and
    not RegQueryStringValue(HKLM64, BrandKey, 'ServerExecutable', Value) and
    not RegQueryStringValue(HKLM64, BrandKey, 'ProfileTool', Value) and
    not RegQueryStringValue(HKLM64, BrandKey, 'ActiveManifest', Value) and
    not RegQueryStringValue(HKLM64, BrandKey, 'ActiveVersion', Value) and
    not RegQueryStringValue(HKLM64, BrandKey, 'Identity', Value) and
    not RegQueryStringValue(HKLM64, RunKey, 'FamoRuntime', Value) and
    not RegQueryStringValue(HKLM64,
      'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '', Value);
end;

function RestoredPreviousProjectionMatches(
  RequireCommittedProjection: Boolean): Boolean;
var
  BrandTarget, BrandManifest, BrandTransaction, BrandState,
    RegisteredDll, TargetFinalPath, TargetObjectId, ManifestFinalPath,
    ManifestObjectId: String;
begin
  Result :=
    (PreviousTarget <> '') and
    (PreviousState = StateReady) and
    (((PreviousTransactionId = '') and
      ValidLegacyTransactionId(PreviousCompatibilityTransactionId)) or
     ((PreviousTransactionId <> '') and
      ValidTransactionId(PreviousCompatibilityTransactionId) and
      (CompareText(PreviousTransactionId,
        PreviousCompatibilityTransactionId) = 0))) and
    RegQueryStringValue(HKLM64, BrandKey, 'InstallDir',
      BrandTarget) and
    RegQueryStringValue(HKLM64, BrandKey, 'ActiveManifest',
      BrandManifest) and
    RegQueryStringValue(HKLM64,
      'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '',
      RegisteredDll) and
    (CompareText(NormalizeDirectoryPath(BrandTarget),
      NormalizeDirectoryPath(PreviousTarget)) = 0) and
    (CompareText(BrandManifest, PreviousManifest) = 0) and
    (CompareText(RegisteredDll, PreviousHost) = 0) and
    (CompareText(PreviousManifest,
      AddBackslash(PreviousTarget) + 'payload-manifest.txt') = 0) and
    PreviousBridgeSnapshotValid and
    (CompareText(PreviousProfileTool,
      AddBackslash(PreviousTarget) + 'FamoProfileTool.exe') = 0) and
    (CompareText(PreviousServer,
      AddBackslash(PreviousTarget) + 'FamoRuntime.exe') = 0) and
    FileExists(PreviousManifest) and
    FileExists(PreviousProfileTool) and FileExists(PreviousServer) and
    TryGetFinalObjectInfo(PreviousTarget, TargetFinalPath,
      TargetObjectId) and
    TryGetFinalObjectInfo(PreviousManifest, ManifestFinalPath,
      ManifestObjectId) and
    PathSame(ExtractFileDir(ManifestFinalPath), TargetFinalPath) and
    FinalObjectsSame(TargetFinalPath, TargetObjectId,
      JournalPreviousFinalTarget, JournalPreviousObjectId) and
    IsSha256Hex(LoadedHostExpectedHash) and
    IsSha256Hex(PreviousManifestHash) and
    (CompareText(GetSHA256OfFile(PreviousManifest),
      PreviousManifestHash) = 0) and
    VerifyManagedPayloadForCleanup(PreviousTarget, TargetFinalPath,
      PreviousManifest, ManifestFinalPath, PreviousVersion,
      Copy(PreviousManifestHash, 1, 12)) and
    (CompareText(GetSHA256OfFile(PreviousHost),
      LoadedHostExpectedHash) = 0);
  if Result and RequireCommittedProjection then
    Result :=
      RegQueryStringValue(HKLM64, BrandKey, 'TransactionId',
        BrandTransaction) and
      RegQueryStringValue(HKLM64, BrandKey, 'InstallState',
        BrandState) and
      (CompareText(BrandTransaction,
        PreviousCompatibilityTransactionId) = 0) and
      (BrandState = PreviousState);
end;

function IsLegacyRollbackAnchorForProjection(const Id, Target,
  Manifest: String): Boolean;
var
  Journal: TTransactionJournal;
  SavedTransactionId, SavedTarget, SavedManifest, SavedHost,
    SavedProfileTool, SavedServer, SavedState, SavedCompatibility,
    SavedPreviousId, SavedFinalTarget, SavedObjectId,
    SavedExpectedHash, SavedManifestHash: String;
begin
  Result := False;
  if not ReadActiveJournal(Id, Journal) or
     (Journal.Phase <> PhaseRolledBack) or
     (Journal.PreviousTransactionId <> '') or
     (CompareText(Journal.PreviousTarget, Target) <> 0) or
     (CompareText(Journal.PreviousManifest, Manifest) <> 0) then
    Exit;
  SavedTransactionId := TransactionId;
  SavedTarget := PreviousTarget;
  SavedManifest := PreviousManifest;
  SavedHost := PreviousHost;
  SavedProfileTool := PreviousProfileTool;
  SavedServer := PreviousServer;
  SavedState := PreviousState;
  SavedCompatibility := PreviousCompatibilityTransactionId;
  SavedPreviousId := PreviousTransactionId;
  SavedFinalTarget := JournalPreviousFinalTarget;
  SavedObjectId := JournalPreviousObjectId;
  SavedExpectedHash := LoadedHostExpectedHash;
  SavedManifestHash := PreviousManifestHash;
  try
    PreviousTarget := Journal.PreviousTarget;
    PreviousManifest := Journal.PreviousManifest;
    PreviousHost := Journal.PreviousHost;
    PreviousProfileTool := Journal.PreviousProfileTool;
    PreviousServer := Journal.PreviousServer;
    PreviousState := Journal.PreviousState;
    PreviousCompatibilityTransactionId :=
      Journal.PreviousCompatibilityTransactionId;
    PreviousTransactionId := Journal.PreviousTransactionId;
    JournalPreviousFinalTarget := Journal.PreviousFinalTarget;
    JournalPreviousObjectId := Journal.PreviousObjectId;
    LoadedHostExpectedHash := Journal.LoadedHostExpectedHash;
    PreviousManifestHash := Journal.PreviousManifestHash;
    Result := RestoredPreviousProjectionMatches(True);
  finally
    TransactionId := SavedTransactionId;
    PreviousTarget := SavedTarget;
    PreviousManifest := SavedManifest;
    PreviousHost := SavedHost;
    PreviousProfileTool := SavedProfileTool;
    PreviousServer := SavedServer;
    PreviousState := SavedState;
    PreviousCompatibilityTransactionId := SavedCompatibility;
    PreviousTransactionId := SavedPreviousId;
    JournalPreviousFinalTarget := SavedFinalTarget;
    JournalPreviousObjectId := SavedObjectId;
    LoadedHostExpectedHash := SavedExpectedHash;
    PreviousManifestHash := SavedManifestHash;
  end;
end;

procedure CommitRollbackActiveProjection;
var
  DesiredActiveId, ActiveReadback, TransactionReadback,
    StateReadback: String;
begin
  if PreviousTarget <> '' then
  begin
    if not RestoredPreviousProjectionMatches(False) then
      RaiseException('restored previous projection identity mismatch');
    if PreviousTransactionId <> '' then
    begin
      if not ValidatePreviousV2Transaction(PreviousTransactionId,
           PreviousTarget, PreviousManifest) then
        RaiseException('restored previous v2 journal mismatch');
      DesiredActiveId := PreviousTransactionId;
    end
    else
      DesiredActiveId := TransactionId;
    RequireJournalWrite(BrandKey, 'TransactionId',
      PreviousCompatibilityTransactionId);
    RequireJournalWrite(BrandKey, 'InstallState', PreviousState);
    FlushMachineRegistryKey(BrandKey);
    RequireJournalWrite(BrandKey, 'ActiveTransactionId',
      DesiredActiveId);
    FlushMachineRegistryKey(BrandKey);
    if not RegQueryStringValue(HKLM64, BrandKey,
         'ActiveTransactionId', ActiveReadback) or
       (CompareText(ActiveReadback, DesiredActiveId) <> 0) or
       not RegQueryStringValue(HKLM64, BrandKey, 'TransactionId',
         TransactionReadback) or
       (CompareText(TransactionReadback,
         PreviousCompatibilityTransactionId) <> 0) or
       not RegQueryStringValue(HKLM64, BrandKey, 'InstallState',
         StateReadback) or (StateReadback <> PreviousState) or
       not RestoredPreviousProjectionMatches(True) then
      RaiseException('restored active transaction commit readback failed');
  end
  else
  begin
    RequireJournalWrite(BrandKey, 'InstallState', StateRolledBack);
    RegDeleteValue(HKLM64, BrandKey, 'TransactionId');
    FlushMachineRegistryKey(BrandKey);
    { Preserve the RolledBack journal as the single authenticated anchor until
      uninstall/retention has removed its task and transaction tree. }
    RequireJournalWrite(BrandKey, 'ActiveTransactionId', TransactionId);
    FlushMachineRegistryKey(BrandKey);
    if RegQueryStringValue(HKLM64, BrandKey, 'TransactionId',
         TransactionReadback) or
       not RegQueryStringValue(HKLM64, BrandKey, 'ActiveTransactionId',
         ActiveReadback) or
       (CompareText(ActiveReadback, TransactionId) <> 0) or
       not RegQueryStringValue(HKLM64, BrandKey, 'InstallState',
         StateReadback) or (StateReadback <> StateRolledBack) or
       not IsEmptyRollbackAnchorForProjection(TransactionId) then
      RaiseException('empty rollback projection commit readback failed');
  end;
end;

procedure ApplyTransactionJournal(const Journal: TTransactionJournal); forward;

function RepairRolledBackActiveProjection: Boolean;
var
  ActiveId: String;
  Journal: TTransactionJournal;
begin
  Result := True;
  if not RegQueryStringValue(HKLM64, BrandKey, 'ActiveTransactionId',
       ActiveId) then
    Exit;
  if not ReadActiveJournal(ActiveId, Journal) then
  begin
    Result := False;
    Exit;
  end;
  if Journal.Phase <> PhaseRolledBack then Exit;
  if not ValidateRecoverableJournalArtifact(Journal, ActiveId) then
  begin
    Result := False;
    Exit;
  end;
  ApplyTransactionJournal(Journal);
  RestorePreviousRegistry;
  CommitRollbackActiveProjection;
  TransactionId := '';
  TransactionTarget := '';
  JournalPhase := '';
  JournalGeneration := 0;
end;

function InspectJournalGenerations(const Id: String;
  var BestGeneration: Integer): Boolean; forward;

function AllowedJournalTransition(const Current, Next: String): Boolean;
begin
  Result :=
    ((Current = '') and (Next = PhasePrepared)) or
    ((Current = PhasePrepared) and (Next = PhasePayloadVerified)) or
    ((Current = PhasePayloadVerified) and
      ((Next = PhaseResumeArmed) or (Next = PhaseActivateIntent))) or
    ((Current = PhaseResumeArmed) and
      ((Next = PhaseDetachIntent) or (Next = PhaseActivateIntent))) or
    ((Current = PhaseDetachIntent) and (Next = PhasePendingReboot)) or
    ((Current = PhasePendingReboot) and (Next = PhaseActivateIntent)) or
    ((Current = PhaseActivateIntent) and
      (Next = PhaseMachineRegistered)) or
    ((Current = PhaseMachineRegistered) and
      (Next = PhaseUserStateIntent)) or
    ((Current = PhaseUserStateIntent) and
      (Next = PhaseUserStatePrepared)) or
    ((Current = PhaseUserStatePrepared) and
      (Next = PhaseUserStateApplied)) or
    ((Current = PhaseUserStateApplied) and (Next = PhaseVerifyIntent)) or
    ((Current = PhaseVerifyIntent) and (Next = PhaseReady)) or
    ((Next = PhaseRollbackIntent) and
      (Current <> '') and (Current <> PhaseReady) and
      (Current <> PhaseRolledBack)) or
    ((Current = PhaseRollbackIntent) and (Next = PhaseRolledBack));
end;

procedure TransitionTransactionPhase(const NextPhase: String);
var
  Journal: TTransactionJournal;
  NextGeneration, GenerationAttempts: Integer;
  GenerationKey, BaseKey, PointerReadback, TransactionReadback: String;
begin
  if not KnownJournalPhase(NextPhase) or
     not AllowedJournalTransition(JournalPhase, NextPhase) then
    RaiseException('invalid transaction journal phase transition: ' +
      JournalPhase + ' -> ' + NextPhase);
  NextGeneration := JournalGeneration;
  for GenerationAttempts := 1 to 1024 do
  begin
    NextGeneration := NextGeneration + 1;
    GenerationKey := JournalGenerationKey(TransactionId, NextGeneration);
    if not RegKeyExists(HKLM64, GenerationKey) then Break;
  end;
  if RegKeyExists(HKLM64, GenerationKey) then
    RaiseException('transaction journal generation space exhausted');
  BuildCurrentJournal(NextPhase, NextGeneration, Journal);
  WriteJournalGeneration(GenerationKey, Journal);
  FlushMachineRegistryKey(GenerationKey);
  if not ValidateJournalGeneration(GenerationKey, NextPhase,
    NextGeneration) then
    RaiseException('transaction journal generation readback failed');
  if not ReadJournalGeneration(GenerationKey, Journal) or
     not ValidateCurrentJournalArtifact(Journal, TransactionId) then
    RaiseException('transaction journal generation semantic readback failed');

  BaseKey := TransactionJournalKey(TransactionId);
  RequireJournalWrite(BaseKey, 'ActiveGeneration',
    IntToStr(NextGeneration));
  FlushMachineRegistryKey(BaseKey);
  if not RegQueryStringValue(HKLM64, BaseKey, 'ActiveGeneration',
    PointerReadback) or (PointerReadback <> IntToStr(NextGeneration)) then
    RaiseException('transaction journal generation commit failed');
  RequireJournalWrite(BrandKey, 'ActiveTransactionId', TransactionId);
  FlushMachineRegistryKey(BrandKey);
  if not RegQueryStringValue(HKLM64, BrandKey, 'ActiveTransactionId',
    TransactionReadback) or
    (CompareText(TransactionReadback, TransactionId) <> 0) then
    RaiseException('active transaction pointer commit failed');
  JournalGeneration := NextGeneration;
  JournalPhase := NextPhase;
  RegWriteStringValue(HKLM64, BrandKey, 'TransactionId', TransactionId);
  RegWriteStringValue(HKLM64, BrandKey, 'InstallState', NextPhase);
end;

procedure ApplyTransactionJournal(const Journal: TTransactionJournal);
begin
  JournalGeneration := StrToInt(Journal.Generation);
  JournalPhase := Journal.Phase;
  JournalAppVersion := Journal.Version;
  TransactionId := Journal.Transaction;
  JournalManifestHash := Journal.ManifestHash;
  TransactionTarget := Journal.PendingTarget;
  JournalPendingFinalTarget := Journal.PendingFinalTarget;
  JournalPendingObjectId := Journal.PendingObjectId;
  PreviousTarget := Journal.PreviousTarget;
  JournalPreviousFinalTarget := Journal.PreviousFinalTarget;
  JournalPreviousObjectId := Journal.PreviousObjectId;
  PriorPreviousTarget := Journal.PriorPreviousTarget;
  JournalPriorPreviousFinalTarget := Journal.PriorPreviousFinalTarget;
  JournalPriorPreviousObjectId := Journal.PriorPreviousObjectId;
  PreviousManifest := Journal.PreviousManifest;
  PreviousManifestHash := Journal.PreviousManifestHash;
  PreviousDefault := Journal.PreviousDefault;
  PreviousHost := Journal.PreviousHost;
  PreviousServer := Journal.PreviousServer;
  PreviousProfileTool := Journal.PreviousProfileTool;
  PreviousVersion := Journal.PreviousVersion;
  PreviousIdentity := Journal.PreviousIdentity;
  PreviousTransactionId := Journal.PreviousTransactionId;
  PreviousCompatibilityTransactionId :=
    Journal.PreviousCompatibilityTransactionId;
  PreviousState := Journal.PreviousState;
  PreviousProfileActive := Journal.PreviousProfileActive = '1';
  PreviousProfileEnabled := Journal.PreviousProfileEnabled = '1';
  PreviousInputTipPresent := Journal.PreviousInputTipPresent = '1';
  SeedReceiptHash := Journal.SeedReceiptHash;
  OriginalUserSid := Journal.OriginalUserSid;
  OriginalUserAccount := Journal.OriginalUserAccount;
  OriginalUserSession := Journal.OriginalUserSession;
  CurrentOriginalUserSession := Journal.LastProofSession;
  OriginalUserResumeCapable := Journal.OriginalUserResumeCapable = '1';
  JournalResumeInstaller := Journal.ResumeInstaller;
  JournalResumeInstallerHash := Journal.ResumeInstallerHash;
  JournalTaskName := Journal.ResumeTaskName;
  JournalAllowDowngrade := Journal.AllowDowngrade = '1';
  LoadedHostHash := Journal.LoadedHostHash;
  LoadedHostVersion := Journal.LoadedHostVersion;
  LoadedHostExpectedHash := Journal.LoadedHostExpectedHash;
end;

function LoadTransactionJournal(const ExpectedId: String): Boolean;
var
  BaseKey, GenerationText, Key: String;
  Generation, BestGeneration: Integer;
  Journal: TTransactionJournal;
begin
  Result := False;
  if not ValidTransactionId(ExpectedId) then Exit;
  BaseKey := TransactionJournalKey(ExpectedId);
  if not InspectJournalGenerations(ExpectedId, BestGeneration) then Exit;
  if not RegQueryStringValue(HKLM64, BaseKey, 'ActiveGeneration',
    GenerationText) then Exit;
  Generation := StrToIntDef(GenerationText, 0);
  if (Generation <= 0) or
     (GenerationText <> IntToStr(Generation)) then Exit;
  Key := JournalGenerationKey(ExpectedId, Generation);
  if not ReadJournalGeneration(Key, Journal) or
     (Journal.Generation <> GenerationText) or
     not ValidateRecoverableJournalArtifact(Journal, ExpectedId) then Exit;
  ApplyTransactionJournal(Journal);
  Result := True;
end;

function ValidateTransactionTarget(const Target, ExpectedId,
  ExpectedVersion, ExpectedManifestHash, ProtectedPreviousTarget: String;
  AllowCurrentActiveTarget: Boolean;
  var NormalizedTarget: String): Boolean;
var
  VersionsRoot, ExpectedLeaf, ActiveTarget, VersionsFinalPath,
    VersionsObjectId, TargetFinalPath, TargetObjectId,
    NormalizedVersion: String;
  VersionsAttributes, TargetAttributes: Cardinal;
  VersionsExists, TargetExists: Boolean;
begin
  Result := False;
  NormalizedTarget := '';
  if (Target = '') or (ExpectedId = '') or not PathIsRooted(Target) or
     ContainsParentTraversal(Target) then
    Exit;
  try
    if not NormalizeSafeRelativePath(ExpectedVersion, NormalizedVersion) or
       (Pos('\', NormalizedVersion) > 0) or
       not IsSha256Hex(ExpectedManifestHash) then
      Exit;
    VersionsRoot := NormalizeDirectoryPath(
      AddBackslash(FixedInstallRoot) + 'versions');
    NormalizedTarget := NormalizeDirectoryPath(Target);
    ExpectedLeaf := NormalizedVersion + '-' +
      Copy(ExpectedManifestHash, 1, 12) + '-' + ExpectedId;
    if PathSame(NormalizedTarget, VersionsRoot) or
       not PathSame(ExtractFileDir(NormalizedTarget), VersionsRoot) or
       (CompareText(ExtractFileName(NormalizedTarget), ExpectedLeaf) <> 0) then
      Exit;
    if not TryGetPathAttributes(VersionsRoot, VersionsExists,
       VersionsAttributes) or
       not TryGetPathAttributes(NormalizedTarget, TargetExists,
       TargetAttributes) then
      Exit;
    if not PathIsNonReparseOrMissing(VersionsRoot) or
       not PathIsNonReparseOrMissing(NormalizedTarget) then
      Exit;
    if VersionsExists then
    begin
      if (VersionsAttributes and FileAttributeDirectory) = 0 then Exit;
      if not TryGetFinalObjectInfo(VersionsRoot, VersionsFinalPath,
        VersionsObjectId) then Exit;
    end;
    if TargetExists then
    begin
      if not VersionsExists or
         ((TargetAttributes and FileAttributeDirectory) = 0) then Exit;
      if not TryGetFinalObjectInfo(NormalizedTarget, TargetFinalPath,
        TargetObjectId) then Exit;
      if not PathSame(ExtractFileDir(TargetFinalPath), VersionsFinalPath) or
         (CompareText(ExtractFileName(TargetFinalPath), ExpectedLeaf) <> 0) then
        Exit;
    end;
    ActiveTarget := ReadActiveTarget;
    if ((not AllowCurrentActiveTarget) and
        not ProtectedPathIsDifferent(NormalizedTarget, TargetExists,
          TargetFinalPath, TargetObjectId, ActiveTarget)) or
       not ProtectedPathIsDifferent(NormalizedTarget, TargetExists,
       TargetFinalPath, TargetObjectId, ProtectedPreviousTarget) then
      Exit;
    Result := True;
  except
    Result := False;
  end;
  if not Result then NormalizedTarget := '';
end;

procedure PrepareTransaction;
var
  ValidatedTarget: String;
begin
  RequireSelectedFixedInstallRoot;
  EnsureTransactionTarget;
  if not ValidateTransactionTarget(TransactionTarget, TransactionId,
    '{#AppVersion}', '{#ManifestHash}', PreviousTarget,
    False, ValidatedTarget) then
    RaiseException('unsafe transaction target refused during prepare');
  TransactionTarget := ValidatedTarget;
  CaptureOriginalUserIdentity;
  if ResumeMode or RollbackMode then
  begin
    if (JournalPhase <> PhasePrepared) and
       (JournalPhase <> PhaseRollbackIntent) and
       (JournalPhase <> PhaseRolledBack) and
       not DirExists(TransactionTarget) then
      RaiseException('pending transaction target is missing');
    TransactionPrepared := True;
    Exit;
  end;
  if DirExists(TransactionTarget) then RaiseException('transaction target already exists');
  SnapshotPreviousState;
  JournalResumeInstaller := '';
  JournalResumeInstallerHash := '';
  JournalTaskName := '';
  JournalAppVersion := '{#AppVersion}';
  JournalManifestHash := Uppercase('{#ManifestHash}');
  JournalPendingFinalTarget := TransactionTarget;
  JournalPendingObjectId := '';
  JournalPreviousFinalTarget := '';
  JournalPreviousObjectId := '';
  JournalPriorPreviousFinalTarget := '';
  JournalPriorPreviousObjectId := '';
  if (PreviousTarget <> '') and
     not TryGetFinalObjectInfo(PreviousTarget, JournalPreviousFinalTarget,
       JournalPreviousObjectId) then
    RaiseException('previous transaction target identity unavailable');
  if (PriorPreviousTarget <> '') and
     not TryGetFinalObjectInfo(PriorPreviousTarget,
       JournalPriorPreviousFinalTarget, JournalPriorPreviousObjectId) then
    RaiseException('prior rollback target identity unavailable');
  JournalAllowDowngrade :=
    CompareText(ExpandConstant('{param:FamoAllowDowngrade|}'), '1') = 0;
  TransitionTransactionPhase(PhasePrepared);
  TransactionPrepared := True;
  FailIfRequested('after-prepare');
end;

procedure CheckDowngradePolicy;
var
  NewRuntime, NewFinalPath, NewObjectId, PreviousFinalPath,
    PreviousObjectId: String;
  NewVersion, PreviousPackedVersion: Int64;
begin
  if PreviousTarget = '' then Exit;
  if not ValidatePreviousPayloadForExecution then
    RaiseException('previous payload identity mismatch before version comparison');
  NewRuntime := AddBackslash(TransactionTarget) + 'FamoRuntime.exe';
  if (PreviousServer = '') or not FileExists(PreviousServer) or
     not TryGetFinalObjectInfo(NewRuntime, NewFinalPath, NewObjectId) or
     not TryGetFinalObjectInfo(PreviousServer, PreviousFinalPath,
       PreviousObjectId) or
     not PathSame(ExtractFileDir(NewFinalPath),
       JournalPendingFinalTarget) or
     not PathSame(ExtractFileDir(PreviousFinalPath),
       JournalPreviousFinalTarget) then
    RaiseException('runtime version comparison path identity mismatch');
  if not StrToVersion(JournalAppVersion + '.0', NewVersion) or
     not StrToVersion(PreviousVersion + '.0', PreviousPackedVersion) then
    RaiseException('manifest version metadata is unavailable');
  if ComparePackedVersion(NewVersion, PreviousPackedVersion) < 0 then
  begin
    if not JournalAllowDowngrade then
      RaiseException('downgrade refused; rerun with /FamoAllowDowngrade=1');
    Log('explicit downgrade accepted and recorded in transaction journal');
  end;
end;

procedure SwitchRegistration;
begin
  TransitionTransactionPhase(PhaseActivateIntent);
  if TransactionChangedBridge and (PreviousHost <> '') then
  begin
    if not RunAndRequire(ProfileTool(TransactionTarget), 'switch-away', True) then
      RaiseException('previous profile switch-away failed');
    if not UnregisterPreviousRegistration then
      RaiseException('previous profile unregister failed');
  end;
  if TransactionChangedBridge then
  begin
    if not RegisterTarget(TransactionTarget) then
      RaiseException('new profile registration failed');
    RegistrationSwitched := True;
  end
  else
    Log('stable Bridge unchanged; skipped TSF detach and registration');
  WriteActiveRegistry(TransactionTarget, 'Activating');
  TransitionTransactionPhase(PhaseMachineRegistered);
end;

function XmlEscape(Value: String): String;
begin
  StringChangeEx(Value, '&', '&amp;', True);
  StringChangeEx(Value, '<', '&lt;', True);
  StringChangeEx(Value, '>', '&gt;', True);
  Result := Value;
end;

function JoinOutputLines(Lines: TArrayOfString): String;
var
  I: Integer;
begin
  Result := '';
  for I := 0 to GetArrayLength(Lines) - 1 do
    Result := Result + Lines[I] + Chr(13) + Chr(10);
end;

function CountText(const Text, Needle: String): Integer;
var
  At, Offset: Integer;
begin
  Result := 0;
  Offset := 1;
  repeat
    At := Pos(Needle, Copy(Text, Offset, Length(Text)));
    if At > 0 then
    begin
      Result := Result + 1;
      Offset := Offset + At + Length(Needle) - 1;
    end;
  until At = 0;
end;

function ExtractUniqueXmlElement(const Xml, OpenTag, CloseTag: String;
  var Element: String): Boolean;
var
  OpenAt, CloseRelative, CloseAt: Integer;
begin
  Result := False;
  Element := '';
  if (CountText(Xml, OpenTag) <> 1) or
     (CountText(Xml, CloseTag) <> 1) then
    Exit;
  OpenAt := Pos(OpenTag, Xml);
  CloseRelative := Pos(CloseTag,
    Copy(Xml, OpenAt + Length(OpenTag), Length(Xml)));
  if CloseRelative = 0 then Exit;
  CloseAt := OpenAt + Length(OpenTag) + CloseRelative - 1;
  Element := Copy(Xml, OpenAt,
    CloseAt + Length(CloseTag) - OpenAt);
  Result := True;
end;

function RecoveryTaskSecurityDescriptor: String;
begin
  Result := 'D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;' +
    OriginalUserSid + ')';
end;

function RecoveryTaskFolderSecurityDescriptor: String;
begin
  Result := 'D:PAI(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;' +
    OriginalUserSid + ')';
end;

function ValidateRecoveryTaskXml(const Xml, InstallerPath,
  Arguments: String): Boolean;
var
  UserTag, TriggerUserTag, CommandTag, ArgumentsTag, TaskSddl, SecurityTag,
    PrincipalElement, TriggerElement, SettingsElement: String;
begin
  UserTag := '<UserId>' + XmlEscape(OriginalUserSid) + '</UserId>';
  TriggerUserTag := '<UserId>' + XmlEscape(OriginalUserAccount) +
    '</UserId>';
  CommandTag := '<Command>' + XmlEscape(InstallerPath) + '</Command>';
  ArgumentsTag := '<Arguments>' + XmlEscape(Arguments) + '</Arguments>';
  TaskSddl := RecoveryTaskSecurityDescriptor;
  SecurityTag := '<SecurityDescriptor>' + XmlEscape(TaskSddl) +
    '</SecurityDescriptor>';
  Result :=
    ExtractUniqueXmlElement(Xml, '<Principal id="OriginalUser">',
      '</Principal>', PrincipalElement) and
    ExtractUniqueXmlElement(Xml, '<LogonTrigger>', '</LogonTrigger>',
      TriggerElement) and
    ExtractUniqueXmlElement(Xml, '<Settings>', '</Settings>',
      SettingsElement) and
    (CountText(Xml, '<Principal id="OriginalUser">') = 1) and
    (CountText(Xml, '<LogonTrigger>') = 1) and
    (CountText(Xml, 'Trigger>') = 2) and
    (CountText(Xml, '<Actions Context="OriginalUser">') = 1) and
    (CountText(Xml, '<Exec>') = 1) and
    (CountText(Xml, SecurityTag) = 1) and
    (CountText(PrincipalElement, '<UserId>') = 1) and
    (CountText(PrincipalElement, UserTag) = 1) and
    (CountText(TriggerElement, '<UserId>') = 1) and
    ((CountText(TriggerElement, UserTag) = 1) or
     (CountText(TriggerElement, TriggerUserTag) = 1)) and
    (CountText(Xml, '<Enabled>false</Enabled>') = 0) and
    (CountText(TriggerElement, '<Enabled>') <= 1) and
    (CountText(TriggerElement, '<Enabled>') =
      CountText(TriggerElement, '<Enabled>true</Enabled>')) and
    (CountText(SettingsElement, '<Enabled>') <= 1) and
    (CountText(SettingsElement, '<Enabled>') =
      CountText(SettingsElement, '<Enabled>true</Enabled>')) and
    (CountText(Xml, '<LogonType>InteractiveToken</LogonType>') = 1) and
    (CountText(Xml, '<RunLevel>HighestAvailable</RunLevel>') = 1) and
    (CountText(Xml, CommandTag) = 1) and
    (CountText(Xml, ArgumentsTag) = 1) and
    (Pos('/FamoRecover=' + TransactionId, Xml) > 0) and
    (Pos('/FamoManifest=' + JournalManifestHash, Xml) > 0) and
    (Pos('/FamoVersion=' + JournalAppVersion, Xml) > 0) and
    (Pos('<LogonTrigger>', Xml) > 0) and
    (Pos('<Exec>', Xml) > 0) and
    (Pos('<BootTrigger>', Xml) = 0) and
    (Pos('<RegistrationTrigger>', Xml) = 0) and
    (Pos('<TimeTrigger>', Xml) = 0) and
    (Pos('<EventTrigger>', Xml) = 0) and
    (Pos('<IdleTrigger>', Xml) = 0) and
    (Pos('<CalendarTrigger>', Xml) = 0) and
    (Pos('<SessionStateChangeTrigger>', Xml) = 0) and
    (Pos('<ComHandler>', Xml) = 0) and
    (Pos('<SendEmail>', Xml) = 0) and
    (Pos('<ShowMessage>', Xml) = 0) and
    (Pos('SYSTEM', Xml) = 0);
end;

function ExpectedRecoveryDirectory(const Id: String): String;
begin
  if not ValidTransactionId(Id) then
    RaiseException('invalid recovery transaction id');
  Result := AddBackslash(FixedInstallRoot) + 'pending\' + Id;
end;

function ExpectedRecoveryInstaller(const Id: String): String;
begin
  Result := AddBackslash(ExpectedRecoveryDirectory(Id)) + 'Famo-Resume-' +
    Id + '.exe';
end;

function ExpectedRecoveryTaskName(const Id: String): String;
begin
  if not ValidTransactionId(Id) then
    RaiseException('invalid recovery transaction id');
  Result := '\Famo\Transaction-' + Id;
end;

function ExpectedRecoveryArguments(const Id: String): String;
begin
  Result := '/FamoRecover=' + Id +
    ' /FamoManifest=' + JournalManifestHash +
    ' /FamoVersion=' + JournalAppVersion +
    ' /SILENT /SP- /NORESTART';
end;

function EnsureRecoveryTaskFolderByCom: Boolean;
var
  Service, RootFolder, Folders, Candidate, Folder, Tasks,
    Subfolders: Variant;
  I, Matches: Integer;
  ExpectedSddl, ActualSddl: String;
begin
  Result := False;
  ExpectedSddl := RecoveryTaskFolderSecurityDescriptor;
  try
    Service := CreateOleObject('Schedule.Service');
    Service.Connect;
    RootFolder := Service.GetFolder('\');
    Folders := RootFolder.GetFolders(0);
    Matches := 0;
    for I := 1 to Folders.Count do
    begin
      Candidate := Folders.Item(I);
      if CompareText(Candidate.Path, '\Famo') = 0 then
      begin
        Matches := Matches + 1;
        Folder := Candidate;
      end;
    end;
    if Matches > 1 then Exit;
    if Matches = 0 then
      Folder := RootFolder.CreateFolder('Famo', ExpectedSddl);

    { Re-enumerate instead of trusting CreateFolder's return value. }
    Folders := RootFolder.GetFolders(0);
    Matches := 0;
    for I := 1 to Folders.Count do
    begin
      Candidate := Folders.Item(I);
      if CompareText(Candidate.Path, '\Famo') = 0 then
      begin
        Matches := Matches + 1;
        Folder := Candidate;
      end;
    end;
    if Matches <> 1 then Exit;
    ActualSddl := Folder.GetSecurityDescriptor(
      DaclSecurityInformation);
    Tasks := Folder.GetTasks(1);
    Subfolders := Folder.GetFolders(0);
    Result := (CompareText(Folder.Path, '\Famo') = 0) and
      (CompareText(ActualSddl, ExpectedSddl) = 0) and
      (Tasks.Count = 0) and (Subfolders.Count = 0);
  except
    Log('Task Scheduler folder creation/validation failed: ' +
      GetExceptionMessage);
    Result := False;
  end;
end;

function ValidateRecoveryArtifactPath(const InstallerPath, Id: String;
  var RecoveryDirectory: String): Boolean;
var
  PendingRoot, InstallerFinalPath, InstallerObjectId, DirectoryFinalPath,
    DirectoryObjectId: String;
  InstallerAttributes, DirectoryAttributes: Cardinal;
  InstallerExists, DirectoryExists: Boolean;
begin
  Result := False;
  RecoveryDirectory := ExpectedRecoveryDirectory(Id);
  PendingRoot := AddBackslash(FixedInstallRoot) + 'pending';
  if (CompareText(NormalizeDirectoryPath(InstallerPath),
       NormalizeDirectoryPath(ExpectedRecoveryInstaller(Id))) <> 0) or
     not ValidateProtectedChild(FixedInstallRoot, 'pending') or
     not ValidateProtectedChild(PendingRoot, Id) or
     not TryGetPathAttributes(RecoveryDirectory, DirectoryExists,
       DirectoryAttributes) or
     not TryGetPathAttributes(InstallerPath, InstallerExists,
       InstallerAttributes) then
    Exit;
  if not DirectoryExists then
  begin
    Result := not InstallerExists;
    Exit;
  end;
  if ((DirectoryAttributes and FileAttributeDirectory) = 0) or
     ((DirectoryAttributes and FileAttributeReparsePoint) <> 0) or
     not TryGetFinalObjectInfo(RecoveryDirectory, DirectoryFinalPath,
       DirectoryObjectId) then
    Exit;
  if InstallerExists then
  begin
    if ((InstallerAttributes and FileAttributeDirectory) <> 0) or
       ((InstallerAttributes and FileAttributeReparsePoint) <> 0) or
       not TryGetFinalObjectInfo(InstallerPath, InstallerFinalPath,
         InstallerObjectId) or
       not PathSame(ExtractFileDir(InstallerFinalPath),
         DirectoryFinalPath) then
      Exit;
  end;
  Result := True;
end;

procedure PinRunningSetupSource;
begin
  SetupSourcePath := ExpandConstant('{srcexe}');
  SetupSourceHash := '';
  SetupSourceFinalPath := '';
  SetupSourceObjectId := '';
  if not FileExists(SetupSourcePath) or
     not TryGetFinalObjectInfo(SetupSourcePath, SetupSourceFinalPath,
       SetupSourceObjectId) or
     (SetupSourceObjectId = '') then
    RaiseException('cannot pin the running setup source object');
  SetupSourceHash := Uppercase(GetSHA256OfFile(SetupSourcePath));
  if not IsSha256Hex(SetupSourceHash) then
    RaiseException('cannot pin the running setup source hash');
end;

procedure RetainRecoveryInstaller;
var
  RecoveryRoot, RecoveryDirectory, Source, Destination,
    SourceFinalPath, SourceObjectId, RecoveryFinalPath, RecoveryObjectId,
    DestinationFinalPath, DestinationObjectId: String;
begin
  RequireFixedProtectedInstallRoot;
  if JournalResumeInstaller <> '' then
  begin
    if not FileExists(JournalResumeInstaller) or
       not IsSha256Hex(JournalResumeInstallerHash) or
       (CompareText(GetSHA256OfFile(JournalResumeInstaller),
         JournalResumeInstallerHash) <> 0) then
      RaiseException('retained recovery installer identity mismatch');
    Exit;
  end;
  RecoveryRoot := AddBackslash(FixedInstallRoot) + 'pending';
  RecoveryDirectory := ExpectedRecoveryDirectory(TransactionId);
  if DirExists(RecoveryDirectory) or
     not ForceDirectories(RecoveryDirectory) or
     not ValidateProtectedChild(RecoveryRoot, TransactionId) then
    RaiseException('cannot create fresh recovery task directory');
  HardenPendingDirectory(RecoveryDirectory, OriginalUserSid);
  Source := ExpandConstant('{srcexe}');
  if (CompareText(Source, SetupSourcePath) <> 0) or
     not IsSha256Hex(SetupSourceHash) or
     (SetupSourceObjectId = '') or
     not TryGetFinalObjectInfo(Source, SourceFinalPath, SourceObjectId) or
     not FinalObjectsSame(SourceFinalPath, SourceObjectId,
       SetupSourceFinalPath, SetupSourceObjectId) or
     (CompareText(GetSHA256OfFile(Source), SetupSourceHash) <> 0) or
     not TryGetFinalObjectInfo(RecoveryDirectory, RecoveryFinalPath,
       RecoveryObjectId) or (RecoveryObjectId = '') then
    RaiseException('running setup source identity changed before retention');
  Destination := ExpectedRecoveryInstaller(TransactionId);
  if not CopyFile(Source, Destination, True) or
     not TryGetFinalObjectInfo(Source, SourceFinalPath, SourceObjectId) or
     not FinalObjectsSame(SourceFinalPath, SourceObjectId,
       SetupSourceFinalPath, SetupSourceObjectId) or
     (CompareText(GetSHA256OfFile(Source), SetupSourceHash) <> 0) or
     not TryGetFinalObjectInfo(RecoveryDirectory, SourceFinalPath,
       SourceObjectId) or
     not FinalObjectsSame(SourceFinalPath, SourceObjectId,
       RecoveryFinalPath, RecoveryObjectId) or
     not TryGetFinalObjectInfo(Destination, DestinationFinalPath,
       DestinationObjectId) or (DestinationObjectId = '') or
     not PathSame(ExtractFileDir(DestinationFinalPath),
       RecoveryFinalPath) or
     FinalObjectsSame(DestinationFinalPath, DestinationObjectId,
       SetupSourceFinalPath, SetupSourceObjectId) or
     (CompareText(GetSHA256OfFile(Destination), SetupSourceHash) <> 0) then
    RaiseException('cannot retain verified recovery installer');
  JournalResumeInstaller := Destination;
  JournalResumeInstallerHash := SetupSourceHash;
end;

function RecoveryTaskExistsByCom(const TaskName: String;
  var Exists: Boolean): Boolean; forward;

function SaveStringsToUTF16LEFile(const FileName: String;
  const Lines: TArrayOfString): Boolean;
var
  Stream: TFileStream;
  Text: String;
  I: Integer;
begin
  Result := False;
  Text := #$FEFF;
  for I := 0 to GetArrayLength(Lines) - 1 do
    Text := Text + Lines[I] + #13#10;
  try
    Stream := TFileStream.Create(FileName, fmCreate);
    try
      Stream.WriteBuffer(Text, Length(Text) * 2);
      Result := True;
    finally
      Stream.Free;
    end;
  except
    Result := False;
  end;
end;

procedure ScheduleRecoveryTask;
var
  RecoveryDirectory, Destination, TaskXml,
    TaskName, TaskArguments, TaskSddl, Parameters, QueriedXml: String;
  Lines: TArrayOfString;
  ResultCode: Integer;
  Output: TExecOutput;
  TaskExists: Boolean;
begin
  if not OriginalUserResumeCapable then
    RaiseException('the original user cannot run an elevated recovery task');
  RetainRecoveryInstaller;
  Destination := JournalResumeInstaller;
  RecoveryDirectory := ExtractFileDir(Destination);
  TaskXml := AddBackslash(RecoveryDirectory) + 'task-' +
    TransactionId + '.xml';
  TaskName := ExpectedRecoveryTaskName(TransactionId);
  TaskArguments := ExpectedRecoveryArguments(TransactionId);
  TaskSddl := RecoveryTaskSecurityDescriptor;
  if not EnsureRecoveryTaskFolderByCom then
    RaiseException('cannot create or validate the recovery task folder');
  if not RecoveryTaskExistsByCom(TaskName, TaskExists) or
     TaskExists then
    RaiseException('recovery task name is not fresh');
  JournalTaskName := TaskName;
  { Persist ownership of the exact task path before task creation. A crash or
    malformed task readback can then disable/delete only this owned path. }
  TransitionTransactionPhase(PhaseResumeArmed);

  SetArrayLength(Lines, 31);
  Lines[0] := '<?xml version="1.0" encoding="UTF-16"?>';
  Lines[1] := '<Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">';
  Lines[2] := '  <RegistrationInfo>';
  Lines[3] := '    <Description>Famo transaction ' + TransactionId + '</Description>';
  Lines[4] := '    <SecurityDescriptor>' + TaskSddl + '</SecurityDescriptor>';
  Lines[5] := '  </RegistrationInfo>';
  Lines[6] := '  <Triggers>';
  Lines[7] := '    <LogonTrigger>';
  Lines[8] := '      <Enabled>true</Enabled>';
  Lines[9] := '      <UserId>' + OriginalUserSid + '</UserId>';
  Lines[10] := '    </LogonTrigger>';
  Lines[11] := '  </Triggers>';
  Lines[12] := '  <Principals>';
  Lines[13] := '    <Principal id="OriginalUser">';
  Lines[14] := '      <UserId>' + OriginalUserSid + '</UserId>';
  Lines[15] := '      <LogonType>InteractiveToken</LogonType>';
  Lines[16] := '      <RunLevel>HighestAvailable</RunLevel>';
  Lines[17] := '    </Principal>';
  Lines[18] := '  </Principals>';
  Lines[19] := '  <Settings>';
  Lines[20] := '    <Enabled>true</Enabled>';
  Lines[21] := '    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>';
  Lines[22] := '    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>';
  Lines[23] := '    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>';
  Lines[24] := '    <StartWhenAvailable>true</StartWhenAvailable>';
  Lines[25] := '    <ExecutionTimeLimit>PT30M</ExecutionTimeLimit>';
  Lines[26] := '  </Settings>';
  Lines[27] := '  <Actions Context="OriginalUser">';
  Lines[28] := '    <Exec><Command>' + XmlEscape(Destination) +
    '</Command><Arguments>' + XmlEscape(TaskArguments) +
    '</Arguments></Exec>';
  Lines[29] := '  </Actions>';
  Lines[30] := '</Task>';
  if not SaveStringsToUTF16LEFile(TaskXml, Lines) then
    RaiseException('cannot write recovery task XML');
  try
    Parameters := ' /Create /TN ' + AddQuotes(TaskName) +
      ' /XML ' + AddQuotes(TaskXml);
    if not RunAndRequire(ExpandConstant('{sys}\schtasks.exe'), Parameters,
      False) then
      RaiseException('cannot create recovery task');
    if not RecoveryTaskExistsByCom(TaskName, TaskExists) or
       not TaskExists then
      RaiseException('recovery task creation readback failed');
    Parameters := ' /Query /TN ' + AddQuotes(TaskName) + ' /XML';
    if not ExecAndCaptureOutput(ExpandConstant('{sys}\schtasks.exe'),
      Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode,
      Output) or (ResultCode <> 0) or Output.Error or
      (GetArrayLength(Output.StdErr) <> 0) then
      RaiseException('cannot read back recovery task');
    QueriedXml := JoinOutputLines(Output.StdOut);
    if not ValidateRecoveryTaskXml(QueriedXml, Destination,
      TaskArguments) then
      RaiseException('recovery task readback mismatch');
  finally
    DeleteFile(TaskXml);
  end;
end;

function RecoveryTaskExistsByCom(const TaskName: String;
  var Exists: Boolean): Boolean;
var
  Service, RootFolder, Folders, Candidate, Folder, Tasks, Task,
    Subfolders: Variant;
  I, Matches: Integer;
  FamoFolderFound: Boolean;
  ActualSddl: String;
begin
  Result := False;
  Exists := False;
  try
    Service := CreateOleObject('Schedule.Service');
    Service.Connect;
    RootFolder := Service.GetFolder('\');
    Folders := RootFolder.GetFolders(0);
    FamoFolderFound := False;
    for I := 1 to Folders.Count do
    begin
      Candidate := Folders.Item(I);
      if CompareText(Candidate.Path, '\Famo') = 0 then
      begin
        if FamoFolderFound then Exit;
        FamoFolderFound := True;
        Folder := Candidate;
        Tasks := Folder.GetTasks(1);
      end;
    end;
    if not FamoFolderFound then
    begin
      Result := True;
      Exit;
    end;
    ActualSddl := Folder.GetSecurityDescriptor(
      DaclSecurityInformation);
    Subfolders := Folder.GetFolders(0);
    if (CompareText(ActualSddl,
         RecoveryTaskFolderSecurityDescriptor) <> 0) or
       (Subfolders.Count <> 0) then
      Exit;
    Matches := 0;
    for I := 1 to Tasks.Count do
    begin
      Task := Tasks.Item(I);
      if CompareText(Task.Path, TaskName) = 0 then
        Matches := Matches + 1;
      if CompareText(Task.Path, TaskName) <> 0 then Exit;
    end;
    if Matches > 1 then Exit;
    Exists := Matches = 1;
    Result := True;
  except
    Log('Task Scheduler COM enumeration failed: ' + GetExceptionMessage);
    Result := False;
  end;
end;

function JournalOwnsRecoveryTask(const TaskName: String): Boolean;
begin
  Result := (JournalTaskName <> '') and
    (CompareText(JournalTaskName, TaskName) = 0) and
    (JournalPhase <> PhasePrepared) and
    (JournalPhase <> PhasePayloadVerified);
end;

function DisableAndDeleteOwnedRecoveryTaskByCom(
  const TaskName: String): Boolean;
var
  Service, RootFolder, Folders, Folder, Candidate, Tasks, Task,
    Subfolders: Variant;
  I, Matches: Integer;
  ActualSddl, TaskLeaf: String;
begin
  Result := False;
  if not JournalOwnsRecoveryTask(TaskName) then Exit;
  try
    Service := CreateOleObject('Schedule.Service');
    Service.Connect;
    RootFolder := Service.GetFolder('\');
    Folders := RootFolder.GetFolders(0);
    Matches := 0;
    for I := 1 to Folders.Count do
    begin
      Candidate := Folders.Item(I);
      if CompareText(Candidate.Path, '\Famo') = 0 then
      begin
        Matches := Matches + 1;
        Folder := Candidate;
      end;
    end;
    if Matches = 0 then
    begin
      Result := True;
      Exit;
    end;
    if Matches <> 1 then Exit;
    ActualSddl := Folder.GetSecurityDescriptor(
      DaclSecurityInformation);
    Subfolders := Folder.GetFolders(0);
    if (CompareText(ActualSddl,
         RecoveryTaskFolderSecurityDescriptor) <> 0) or
       (Subfolders.Count <> 0) then
      Exit;
    Tasks := Folder.GetTasks(1);
    if Tasks.Count = 0 then
    begin
      Result := True;
      Exit;
    end;
    if Tasks.Count <> 1 then Exit;
    Task := Tasks.Item(1);
    TaskLeaf := 'Transaction-' + TransactionId;
    if (CompareText(Task.Path, TaskName) <> 0) or
       (CompareText(Task.Name, TaskLeaf) <> 0) then
      Exit;
    Task.Enabled := False;
    Task := Folder.GetTask(TaskLeaf);
    if Task.Enabled then Exit;
    Folder.DeleteTask(TaskLeaf, 0);
    Tasks := Folder.GetTasks(1);
    Result := Tasks.Count = 0;
  except
    Log('Owned recovery task disable/delete failed: ' +
      GetExceptionMessage);
    Result := False;
  end;
end;

function CleanupEmptyRecoveryTaskFolderByCom: Boolean;
var
  Service, RootFolder, Folders, Folder, Tasks, Subfolders,
    Candidate: Variant;
  I, Matches: Integer;
  ActualSddl: String;
begin
  Result := False;
  try
    Service := CreateOleObject('Schedule.Service');
    Service.Connect;
    RootFolder := Service.GetFolder('\');
    Folders := RootFolder.GetFolders(0);
    Matches := 0;
    for I := 1 to Folders.Count do
    begin
      Candidate := Folders.Item(I);
      if CompareText(Candidate.Path, '\Famo') = 0 then
      begin
        Matches := Matches + 1;
        Folder := Candidate;
      end;
    end;
    if Matches = 0 then
    begin
      Result := True;
      Exit;
    end;
    if Matches <> 1 then Exit;
    ActualSddl := Folder.GetSecurityDescriptor(
      DaclSecurityInformation);
    if CompareText(ActualSddl,
         RecoveryTaskFolderSecurityDescriptor) <> 0 then
      Exit;
    Tasks := Folder.GetTasks(1);
    Subfolders := Folder.GetFolders(0);
    if (Tasks.Count <> 0) or (Subfolders.Count <> 0) then Exit;
    RootFolder.DeleteFolder('Famo', 0);

    Folders := RootFolder.GetFolders(0);
    for I := 1 to Folders.Count do
    begin
      Candidate := Folders.Item(I);
      if CompareText(Candidate.Path, '\Famo') = 0 then Exit;
    end;
    Result := True;
  except
    Log('Task Scheduler folder cleanup failed: ' + GetExceptionMessage);
    Result := False;
  end;
end;

function RecoveryTaskFolderAbsentByCom: Boolean;
var
  Service, RootFolder, Folders, Candidate: Variant;
  I: Integer;
begin
  Result := False;
  try
    Service := CreateOleObject('Schedule.Service');
    Service.Connect;
    RootFolder := Service.GetFolder('\');
    Folders := RootFolder.GetFolders(0);
    for I := 1 to Folders.Count do
    begin
      Candidate := Folders.Item(I);
      if CompareText(Candidate.Path, '\Famo') = 0 then Exit;
    end;
    Result := True;
  except
    Log('Task Scheduler folder absence readback failed: ' +
      GetExceptionMessage);
    Result := False;
  end;
end;

procedure ClearRecoveryCleanupDebt; forward;

procedure DeleteRecoveryTask;
var
  ResultCode: Integer;
  RecoveryDirectory, TaskName, InstallerPath, TaskArguments, Parameters,
    QueriedXml: String;
  Output: TExecOutput;
  TaskExists, TaskXmlValid: Boolean;
begin
  if not ValidTransactionId(TransactionId) then
    RaiseException('cannot derive recovery artifacts for invalid transaction');
  TaskName := ExpectedRecoveryTaskName(TransactionId);
  InstallerPath := ExpectedRecoveryInstaller(TransactionId);
  TaskArguments := ExpectedRecoveryArguments(TransactionId);
  if ((JournalTaskName <> '') and
      (CompareText(JournalTaskName, TaskName) <> 0)) or
     ((JournalResumeInstaller <> '') and
      (CompareText(NormalizeDirectoryPath(JournalResumeInstaller),
       NormalizeDirectoryPath(InstallerPath)) <> 0)) or
     not ValidateRecoveryArtifactPath(InstallerPath, TransactionId,
       RecoveryDirectory) then
    RaiseException('recovery artifact identity mismatch during cleanup');
  if FileExists(InstallerPath) and
     (JournalResumeInstallerHash <> '') and
     (not IsSha256Hex(JournalResumeInstallerHash) or
      (CompareText(GetSHA256OfFile(InstallerPath),
       JournalResumeInstallerHash) <> 0)) then
    RaiseException('recovery installer hash mismatch during cleanup');

  if not RecoveryTaskExistsByCom(TaskName, TaskExists) then
    RaiseException('cannot enumerate recovery tasks during cleanup');
  if TaskExists then
  begin
    if not JournalOwnsRecoveryTask(TaskName) then
      RaiseException('unowned recovery task blocks cleanup');
    Parameters := ' /Query /TN ' + AddQuotes(TaskName) + ' /XML';
    TaskXmlValid := ExecAndCaptureOutput(ExpandConstant('{sys}\schtasks.exe'),
      Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode, Output);
    TaskXmlValid := TaskXmlValid and (ResultCode = 0) and
      not Output.Error and (GetArrayLength(Output.StdErr) = 0);
    if TaskXmlValid then
    begin
      QueriedXml := JoinOutputLines(Output.StdOut);
      TaskXmlValid := ValidateRecoveryTaskXml(QueriedXml, InstallerPath,
        TaskArguments);
    end;
    if not TaskXmlValid then
      Log('Owned recovery task XML is invalid; disabling and deleting the ' +
        'journal-owned exact task path');
    if not DisableAndDeleteOwnedRecoveryTaskByCom(TaskName) then
      RaiseException('cannot disable/delete the owned recovery task');
    if not RecoveryTaskExistsByCom(TaskName, TaskExists) or TaskExists then
      RaiseException('recovery task deletion readback failed');
  end;
  if not CleanupEmptyRecoveryTaskFolderByCom then
    RaiseException('recovery task folder cleanup failed');
  if FileExists(InstallerPath) and not DeleteFile(InstallerPath) then
    RaiseException('cannot delete retained recovery installer');
  if (RecoveryDirectory <> '') and DirExists(RecoveryDirectory) and
     not RemoveDir(RecoveryDirectory) then
    RaiseException('cannot delete recovery task directory');
  if FileExists(InstallerPath) or DirExists(RecoveryDirectory) then
    RaiseException('recovery artifact deletion readback failed');
  JournalTaskName := '';
  JournalResumeInstaller := '';
  JournalResumeInstallerHash := '';
  ClearRecoveryCleanupDebt;
end;

procedure PersistRecoveryCleanupDebtBeforeReady;
begin
  if (JournalResumeInstaller = '') and (JournalTaskName = '') then
  begin
    ClearRecoveryCleanupDebt;
    Exit;
  end;
  ArmTransactionDebt('RecoveryCleanupDebt', DebtKindRecoveryArtifacts);
end;

procedure ClearRecoveryCleanupDebt;
begin
  ClearTransactionDebt(
    'RecoveryCleanupDebt', DebtKindRecoveryArtifacts);
end;

procedure WritePendingRegistry;
begin
  WriteOrDelete('InstallDir', PreviousTarget);
  WriteOrDelete('ServerExecutable', PreviousServer);
  WriteOrDelete('ProfileTool', PreviousProfileTool);
  WriteOrDelete('ActiveManifest', PreviousManifest);
  WriteOrDelete('ActiveVersion', PreviousVersion);
  WriteOrDelete('Identity', PreviousIdentity);
  RegWriteStringValue(HKLM64, BrandKey, 'TransactionId', TransactionId);
  RegWriteStringValue(HKLM64, BrandKey, 'PreviousTarget', PreviousTarget);
  RegWriteStringValue(HKLM64, BrandKey, 'PreviousDefault', PreviousDefault);
  WriteOrDelete('PreviousHost', PreviousHost);
  WriteOrDelete('PreviousManifest', PreviousManifest);
  WriteOrDelete('PreviousServer', PreviousServer);
  WriteOrDelete('PreviousProfileTool', PreviousProfileTool);
  WriteOrDelete('PreviousVersion', PreviousVersion);
  WriteOrDelete('PreviousIdentity', PreviousIdentity);
  RegWriteStringValue(HKLM64, BrandKey, 'PreviousProfileActive',
    IntToStr(Ord(PreviousProfileActive)));
  RegWriteStringValue(HKLM64, BrandKey, 'PendingTarget', TransactionTarget);
  RegWriteStringValue(HKLM64, BrandKey, 'PendingManifest',
    AddBackslash(TransactionTarget) + 'payload-manifest.txt');
  RegWriteStringValue(HKLM64, BrandKey, 'PendingVersion', '{#AppVersion}');
  RegWriteStringValue(HKLM64, BrandKey, 'PendingReason',
    'registered host module is still loaded');
  RegWriteStringValue(HKLM64, BrandKey, 'LoadedHostPath', PreviousHost);
  RegWriteStringValue(HKLM64, BrandKey, 'LoadedHostHash', LoadedHostHash);
  RegWriteStringValue(HKLM64, BrandKey, 'LoadedHostVersion', LoadedHostVersion);
  WriteOrDelete('LoadedHostExpectedHash', LoadedHostExpectedHash);
  RegWriteStringValue(HKLM64, BrandKey, 'InstallState', StatePendingReboot);
  RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
end;

procedure VerifyPendingInstall;
var
  RegisteredDll, RunValue: String;
begin
  VerifyPayloadOrFail;
  if RegQueryStringValue(HKLM64,
    'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '', RegisteredDll) then
    RaiseException('pending COM registration must be absent');
  if not RunAndRequire(ProfileTool(TransactionTarget), 'check-absent', False) then
    RaiseException('pending profile registration must be absent');
  if RunAsOriginalUserExitCode(ProfileTool(TransactionTarget),
    'is-active') <> 1 then
    RaiseException('pending profile remains active');
  if RegQueryStringValue(HKLM64, RunKey, 'FamoRuntime', RunValue) then
    RaiseException('pending runtime Run entry must be absent');
end;

procedure EnterPendingReboot;
begin
  if not OriginalUserResumeCapable then
    RaiseException('reboot continuation requires the original user to be an administrator');
  ScheduleRecoveryTask;
  TransitionTransactionPhase(PhaseDetachIntent);
  if PreviousHost <> '' then
  begin
    if not RunAndRequire(ProfileTool(TransactionTarget), 'switch-away', True) then
      RaiseException('cannot switch away before pending reboot');
    if not RunAndRequire(AddBackslash(TransactionTarget) +
      'settings\FamoSettings.exe', '--remove-input-tip', True) then
      RaiseException('cannot remove current-user input tip before pending reboot');
    if not UnregisterPreviousRegistration then
      RaiseException('cannot unregister previous host before pending reboot');
  end;
  RegistrationSwitched := True;
  RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
  VerifyPendingInstall;
  FailIfRequested('after-pending-registration');
  WritePendingRegistry;
  TransitionTransactionPhase(PhasePendingReboot);
  FailIfRequested('after-pending-state');
  PendingTerminal := True;
  InstallReady := True;
end;

procedure StartRuntimeAsOriginalUser;
var
  ResultCode: Integer;
  Broker, ActiveState, ActiveTarget, ActiveVersion, ActiveServer: String;
begin
  Broker := ProfileTool(TransactionTarget);
  FlushMachineRegistryKey(BrandKey);
  if not RegQueryStringValue(HKLM64, BrandKey, 'InstallState',
       ActiveState) or
     not RegQueryStringValue(HKLM64, BrandKey, 'InstallDir',
       ActiveTarget) or
     not RegQueryStringValue(HKLM64, BrandKey, 'ActiveVersion',
       ActiveVersion) or
     not RegQueryStringValue(HKLM64, BrandKey, 'ServerExecutable',
       ActiveServer) or
     (ActiveState <> 'Activating') or
     (CompareText(ActiveTarget, TransactionTarget) <> 0) or
     (CompareText(ActiveVersion, JournalAppVersion) <> 0) or
     (CompareText(ActiveServer,
       AddBackslash(TransactionTarget) + 'FamoRuntime.exe') <> 0) then
  begin
    Log('runtime activation projection mismatch: state=' + ActiveState +
      '; target=' + ActiveTarget + '; version=' + ActiveVersion +
      '; server=' + ActiveServer);
    RaiseException('runtime activation projection readback failed');
  end;
  if not ValidateCurrentPayloadForExecution or
    not Exec(Broker, 'start-runtime-for ' + OriginalUserSid, '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
    RaiseException('runtime start failed');
  RuntimeStarted := True;
  Sleep(750);
end;

function StopRuntimeAsOriginalUser(const RuntimePath: String): Boolean;
var
  Parameters: String;
begin
  Result := RuntimePath = '';
  if Result then Exit;
  Parameters := 'stop-runtime-for ' + OriginalUserSid + ' ' +
    AddQuotes(RuntimePath);
  Result := RunAndRequire(
    ProfileTool(TransactionTarget), Parameters, False);
  if Result then
    Log('previous runtime stop readback succeeded: ' + RuntimePath)
  else
    Log('previous runtime stop readback failed: ' + RuntimePath);
end;

procedure CapturePreviousUserState;
var
  ProbeExit: Integer;
  Settings: String;
begin
  { A first install has no previous user state to capture. Every restore
    path that reads these flags sits behind PreviousHost <> '', and
    ValidateJournalSemantics requires all three to stay '0' while
    PreviousTarget is empty. Probing anyway records a state nothing can
    consume and fails the journal readback on every first install. }
  if PreviousTarget = '' then
  begin
    PreviousProfileActive := False;
    PreviousProfileEnabled := False;
    PreviousInputTipPresent := False;
    Exit;
  end;

  ProbeExit := RunAsOriginalUserExitCode(
    ProfileTool(TransactionTarget), 'is-active');
  if (ProbeExit <> 0) and (ProbeExit <> 1) then
    RaiseException('previous profile active-state probe failed');
  PreviousProfileActive := ProbeExit = 0;

  ProbeExit := RunAsOriginalUserExitCode(
    ProfileTool(TransactionTarget), 'is-enabled');
  if (ProbeExit <> 0) and (ProbeExit <> 1) then
    RaiseException('previous profile enabled-state probe failed');
  PreviousProfileEnabled := ProbeExit = 0;

  Settings := AddBackslash(TransactionTarget) + 'settings\FamoSettings.exe';
  ProbeExit := RunAsOriginalUserExitCode(Settings, '--is-input-tip');
  if (ProbeExit <> 0) and (ProbeExit <> 1) then
    RaiseException('previous input-tip membership probe failed');
  PreviousInputTipPresent := ProbeExit = 0;
end;

function ReadPreparedSeedReceiptHash(var ReceiptHash: String): Boolean;
var
  LocalAppData, FamoDirectory, TransactionsDirectory,
    TransactionDirectory, ReceiptPath, BeforeFinalPath, BeforeObjectId,
    AfterFinalPath, AfterObjectId: String;
  Attributes: Cardinal;
  Exists: Boolean;
begin
  Result := False;
  ReceiptHash := '';
  if not ValidTransactionId(TransactionId) or
     not RegQueryStringValue(HKU, OriginalUserSid +
       '\Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders',
       'Local AppData', LocalAppData) or
     not PathIsRooted(LocalAppData) or ContainsParentTraversal(LocalAppData) or
     not PathIsNonReparseOrMissing(LocalAppData) then
    Exit;
  LocalAppData := NormalizeDirectoryPath(LocalAppData);
  FamoDirectory := AddBackslash(LocalAppData) + 'Famo';
  TransactionsDirectory := AddBackslash(FamoDirectory) + '.transactions';
  TransactionDirectory := AddBackslash(TransactionsDirectory) + TransactionId;
  ReceiptPath := AddBackslash(TransactionDirectory) + 'receipt.json';
  if not ValidateProtectedChild(LocalAppData, 'Famo') or
     not ValidateProtectedChild(FamoDirectory, '.transactions') or
     not ValidateProtectedChild(TransactionsDirectory, TransactionId) or
     not ValidateProtectedFile(TransactionDirectory, 'receipt.json') or
     not TryGetPathAttributes(ReceiptPath, Exists, Attributes) or not Exists or
     ((Attributes and (FileAttributeDirectory or FileAttributeReparsePoint)) <> 0) or
     not TryGetFinalObjectInfo(ReceiptPath, BeforeFinalPath, BeforeObjectId) then
    Exit;
  ReceiptHash := Uppercase(GetSHA256OfFile(ReceiptPath));
  Result := IsSha256Hex(ReceiptHash) and
    TryGetFinalObjectInfo(ReceiptPath, AfterFinalPath, AfterObjectId) and
    FinalObjectsSame(BeforeFinalPath, BeforeObjectId,
      AfterFinalPath, AfterObjectId);
  if not Result then ReceiptHash := '';
end;

procedure InstallUserState;
var
  SeedArguments, Settings: String;
  DeployAttempt, DeployExit: Integer;
  DeployOk: Boolean;
begin
  TransitionTransactionPhase(PhaseUserStateIntent);
  Settings := AddBackslash(TransactionTarget) + 'settings\FamoSettings.exe';
  if not RunAndRequire(Settings,
    '--prepare-seed-transaction ' + TransactionId, True) then
    RaiseException('user seed transaction prepare failed');
  if not ReadPreparedSeedReceiptHash(SeedReceiptHash) then
    RaiseException('user seed receipt identity unavailable');
  TransitionTransactionPhase(PhaseUserStatePrepared);
  if not RunAndRequire(ProfileTool(TransactionTarget),
    'clear-user-com-shadow ' + OriginalUserSid, True) then
    RaiseException('original-user COM shadow cleanup failed');
  if not RunAndRequire(ProfileTool(TransactionTarget), 'enable', True) then
    RaiseException('original-user profile enablement failed');
  SeedArguments := '--apply-seed-transaction ' + TransactionId + ' ' +
    SeedReceiptHash;
  if (PreviousHost <> '') and not PreviousProfileActive then
    SeedArguments := SeedArguments + ' --no-activate';
  if not RunAndRequire(Settings, SeedArguments, True) then
    RaiseException('user seed transaction apply failed');
  TransitionTransactionPhase(PhaseUserStateApplied);
  WriteActiveRegistry(TransactionTarget, 'Activating');
  StartRuntimeAsOriginalUser;
  { First launch of freshly written binaries is slow (Defender scans them on
    execute), so the runtime's control pipe may not be up 750ms after start.
    One shot here killed a real 1.4.9 install; the control client is
    idempotent, so retry briefly instead. }
  DeployOk := False;
  for DeployAttempt := 1 to 15 do
  begin
    DeployExit := RunBoundDesktopExitCode(
      AddBackslash(TransactionTarget) + 'FamoRuntime.exe',
      '--control deploy', True);
    Log('runtime deploy attempt ' + IntToStr(DeployAttempt) +
      ' exit=' + IntToStr(DeployExit));
    if DeployExit = 0 then
    begin
      DeployOk := True;
      Break;
    end;
    { A just-stopped predecessor can briefly retain the per-session singleton.
      In that case the first new process exits cleanly and no server remains.
      Restart before retrying the control pipe instead of polling an absent
      process forever. }
    if DeployAttempt < 15 then
      StartRuntimeAsOriginalUser;
    Sleep(2000);
  end;
  if not DeployOk then
    RaiseException('runtime deploy failed');
  TransitionTransactionPhase(PhaseVerifyIntent);
end;

procedure PersistUserCleanupDebtBeforeReady;
begin
  if SeedReceiptHash <> '' then
    ArmTransactionDebt('UserCleanupDebt', DebtKindSeedCommit);
end;

function CommitSeedReceiptAfterReady: Boolean;
var
  Settings: String;
begin
  Result := False;
  if SeedReceiptHash = '' then
  begin
    Result := not TransactionDebtPresent(
      'UserCleanupDebt', DebtKindSeedCommit);
    Exit;
  end;
  if not TransactionDebtPresent(
       'UserCleanupDebt', DebtKindSeedCommit) then
    ArmTransactionDebt('UserCleanupDebt', DebtKindSeedCommit);
  Settings := AddBackslash(TransactionTarget) +
    'settings\FamoSettings.exe';
  Result := ValidateCurrentPayloadForExecution and
     FileExists(Settings) and
     RunAndRequire(Settings, '--commit-seed-transaction ' + TransactionId +
       ' ' + SeedReceiptHash, True);
  if Result then
    ClearTransactionDebt('UserCleanupDebt', DebtKindSeedCommit);
end;

function RetryRolledBackCleanupDebt: Boolean; forward;
function ValidateJournalBoundPartialTargetForCleanup(
  var NormalizedTarget: String): Boolean; forward;

procedure RollbackTransaction;
var
  ResultCode: Integer;
  RestoreSettings, ValidatedTarget, PriorFinalPath, PriorObjectId,
  EnableCommand, CheckCommand: String;
  UserRollbackOk, HadUserStateIntent, HadProfileMutationIntent,
    CurrentPayloadTrusted,
    TargetCleanupComplete, HasReadyCommitDebt: Boolean;
begin
  if RollbackComplete then Exit;
  if JournalPhase = PhaseRolledBack then
  begin
    CommitRollbackActiveProjection;
    if not RetryRolledBackCleanupDebt then
      RaiseException(
        'rolled-back cleanup debt remains durably recoverable');
    RollbackComplete := True;
    Exit;
  end;
  HadUserStateIntent :=
    (SeedReceiptHash <> '') or
    (JournalPhase = PhaseUserStatePrepared) or
    (JournalPhase = PhaseUserStateApplied) or
    (JournalPhase = PhaseVerifyIntent);
  HadProfileMutationIntent :=
    HadUserStateIntent or
    (JournalPhase = PhaseDetachIntent) or
    (JournalPhase = PhasePendingReboot) or
    (JournalPhase = PhaseActivateIntent) or
    (JournalPhase = PhaseMachineRegistered);
  HasReadyCommitDebt := TransactionDebtPresent(
    'UserCleanupDebt', DebtKindSeedCommit);
  if HasReadyCommitDebt and (SeedReceiptHash = '') then
    RaiseException(
      'seed commit debt lacks its rollback receipt binding');
  if JournalPhase <> PhaseRollbackIntent then
    TransitionTransactionPhase(PhaseRollbackIntent);
  if HadUserStateIntent then
    ArmTransactionDebt('UserRollbackDebt', DebtKindUserRollback);
  ArmTransactionDebt('TargetCleanupDebt', DebtKindTargetCleanup);
  if (JournalResumeInstaller <> '') or (JournalTaskName <> '') then
    ArmTransactionDebt(
      'RecoveryCleanupDebt', DebtKindRecoveryArtifacts);
  { UserCleanupDebt is the forward Ready/seed-commit obligation. Once the
    rollback debts are durable, rollback supersedes that forward intent; unlike
    compensation debts it must not survive into the RolledBack terminal set. }
  if HasReadyCommitDebt then
    ClearTransactionDebt('UserCleanupDebt', DebtKindSeedCommit);
  FailIfRequested('rollback-debts-before-terminal');
  UserRollbackOk := True;
  CurrentPayloadTrusted := ValidateCurrentPayloadForExecution;
  if not CurrentPayloadTrusted and HadUserStateIntent then
    UserRollbackOk := False;
  if CurrentPayloadTrusted and
     ValidateCurrentPayloadForExecution and
     FileExists(AddBackslash(TransactionTarget) + 'FamoRuntime.exe') then
  begin
    RunAndRequire(AddBackslash(TransactionTarget) + 'FamoRuntime.exe',
      '--control shutdown', True);
    Sleep(500);
    RuntimeStarted := False;
  end;
  if HadUserStateIntent and CurrentPayloadTrusted then
  begin
    RestoreSettings := AddBackslash(TransactionTarget) +
      'settings\FamoSettings.exe';
    if FileExists(RestoreSettings) and
       ValidateCurrentPayloadForExecution and
       not RunAndRequire(RestoreSettings, '--remove-input-tip', True) then
      UserRollbackOk := False;
    if FileExists(ProfileTool(TransactionTarget)) and
       ValidateCurrentPayloadForExecution and
       not RunAndRequire(ProfileTool(TransactionTarget),
         'cleanup-user-state', True) then
      UserRollbackOk := False;
  end;
  if TransactionChangedBridge and
     MachineComPointsToTarget(TransactionTarget) then
  begin
    if CurrentPayloadTrusted and ValidateCurrentPayloadForExecution then
    begin
      if not UnregisterTarget(TransactionTarget) then
        RaiseException('new machine registration rollback failed');
    end
    else if not RunTrustedDirectMachineUnregister then
      RaiseException('trusted direct machine registration rollback failed');
  end;
  if TransactionChangedBridge and
     MachineComPointsToTarget(TransactionTarget) then
    RaiseException('new machine COM registration remains after rollback');
  if (PriorPreviousTarget <> '') and
     (not TryGetFinalObjectInfo(PriorPreviousTarget, PriorFinalPath,
        PriorObjectId) or
      not FinalObjectsSame(PriorFinalPath, PriorObjectId,
        JournalPriorPreviousFinalTarget, JournalPriorPreviousObjectId)) then
    RaiseException('prior rollback target identity mismatch');
  if (PreviousTarget <> '') and
     not ValidatePreviousPayloadForExecution then
    RaiseException('previous payload identity mismatch before rollback restore');
  RestorePreviousRegistry;
  if PreviousHost <> '' then
  begin
    if TransactionChangedBridge and
       not RegisterPreviousRegistration then
      RaiseException('previous profile rollback failed');
    if HadProfileMutationIntent then
    begin
      if PreviousProfileEnabled then
      begin
        EnableCommand := 'enable';
        CheckCommand := 'check';
      end
      else
      begin
        EnableCommand := 'disable';
        CheckCommand := 'check-disabled';
      end;
      if CurrentPayloadTrusted then
      begin
        if not ValidateCurrentPayloadForExecution or
           not RunAndRequire(ProfileTool(TransactionTarget),
             'clear-user-com-shadow ' + OriginalUserSid, True) or
           not ValidateCurrentPayloadForExecution or
           not ValidatePreviousPayloadForExecution or
           not RunAndRequire(PreviousRegistrationTool, EnableCommand, True) or
           not ValidateCurrentPayloadForExecution or
           not ValidatePreviousPayloadForExecution or
           not RunAndRequire(PreviousRegistrationTool, CheckCommand, True) then
          UserRollbackOk := False;
      end
      else
        UserRollbackOk := False;
      RestoreSettings := AddBackslash(PreviousTarget) +
        'settings\FamoSettings.exe';
      if not FileExists(RestoreSettings) then
        RestoreSettings := AddBackslash(TransactionTarget) +
          'settings\FamoSettings.exe';
      if PreviousInputTipPresent and
         (not CurrentPayloadTrusted or
          not ValidateCurrentPayloadForExecution or
          ((CompareText(ExtractFileDir(ExtractFileDir(RestoreSettings)),
             NormalizeDirectoryPath(PreviousTarget)) = 0) and
           not ValidatePreviousPayloadForExecution) or
          not RunAndRequire(RestoreSettings, '--add-input-tip', True)) then
        UserRollbackOk := False;
      if PreviousProfileActive and
        (not CurrentPayloadTrusted or
         not ValidateCurrentPayloadForExecution or
         not ValidatePreviousPayloadForExecution or
         not RunAndRequire(PreviousRegistrationTool, 'activate', True)) then
      begin
        UserRollbackOk := False;
        Log('previous profile activation deferred; available via Win+Space');
      end;
    end;
    if (PreviousServer <> '') and FileExists(PreviousServer) then
    begin
      if not CurrentPayloadTrusted or
         not ValidateCurrentPayloadForExecution or
         not ValidatePreviousPayloadForExecution then
        UserRollbackOk := False;
      if UserRollbackOk then
      begin
        ResultCode := RunBoundDesktopExitCode(PreviousServer, '', False);
        if ResultCode <> 0 then
          RaiseException('previous runtime rollback failed');
        Sleep(750);
        if (PreviousManifest <> '') and
           (not ValidateCurrentPayloadForExecution or
            not ValidatePreviousPayloadForExecution or
            not RunAndRequire(PreviousServer,
              '--control reload-options', True)) then
          UserRollbackOk := False;
      end;
    end;
  end;
  { Publish the terminal decision before retiring any cleanup debt or deleting
    its executable anchor. A crash from this point is a RolledBack re-entry,
    and RetryRolledBackCleanupDebt can replay the still-durable obligations. }
  TransitionTransactionPhase(PhaseRolledBack);
  FailIfRequested('rolledback-before-debt-finalize');
  { The seed CAS rollback is deliberately last. If an earlier exact-user
    restoration fails, its authenticated transaction remains intact for the
    retained recovery task. Once this succeeds there are no later fallible
    user-state steps that could create an ambiguous aggregate debt. }
  if UserRollbackOk and HadUserStateIntent and CurrentPayloadTrusted then
  begin
    RestoreSettings := AddBackslash(TransactionTarget) +
      'settings\FamoSettings.exe';
    if (SeedReceiptHash <> '') and
       (not ValidateCurrentPayloadForExecution or
        not RunAndRequire(RestoreSettings,
          '--rollback-seed-transaction ' + TransactionId + ' ' +
          SeedReceiptHash, True)) then
      UserRollbackOk := False
    else if (SeedReceiptHash = '') and
            (not ValidateCurrentPayloadForExecution or
             not RunAndRequire(RestoreSettings,
               '--discard-seed-transaction ' + TransactionId, True)) then
      UserRollbackOk := False;
  end;
  if UserRollbackOk and HadUserStateIntent then
    ClearTransactionDebt('UserRollbackDebt', DebtKindUserRollback);
  TargetCleanupComplete := not DirExists(TransactionTarget);
  if DirExists(TransactionTarget) and UserRollbackOk and
     CurrentPayloadTrusted and ValidateCurrentPayloadForExecution then
  begin
    if not ValidateTransactionTarget(TransactionTarget, TransactionId,
      JournalAppVersion, JournalManifestHash, PreviousTarget,
      False, ValidatedTarget) then
      RaiseException('unsafe transaction target refused during rollback');
    CurrentPayloadProofValid := False;
    if not DelTree(ValidatedTarget, True, True, True) then
      RaiseException('transaction target deletion failed during rollback');
    TargetCleanupComplete := not DirExists(ValidatedTarget);
  end
  else if DirExists(TransactionTarget) then
    TargetCleanupComplete := False;
  if TargetCleanupComplete then
    ClearTransactionDebt('TargetCleanupDebt', DebtKindTargetCleanup);
  if not UserRollbackOk or not TargetCleanupComplete then
    RaiseException(
      'rollback compensation remains durably recoverable');
  FailIfRequested('rolledback-before-artifact-cleanup');
  CommitRollbackActiveProjection;
  try
    DeleteRecoveryTask;
  except
    Log('deferred recovery artifact cleanup after rollback: ' +
      GetExceptionMessage);
  end;
  RollbackComplete := True;
end;

function RetryRolledBackCleanupDebt: Boolean;
var
  RestoreSettings, ValidatedTarget, EnableCommand,
    CheckCommand: String;
  ResultCode: Integer;
  HasUserDebt, HasTargetDebt, UserRollbackOk,
    TargetCleanupComplete, CurrentPayloadTrusted: Boolean;
begin
  Result := False;
  if JournalPhase <> PhaseRolledBack then Exit;
  MigrateLegacyRollbackCleanupDebt;
  HasUserDebt := TransactionDebtPresent(
    'UserRollbackDebt', DebtKindUserRollback);
  HasTargetDebt := TransactionDebtPresent(
    'TargetCleanupDebt', DebtKindTargetCleanup);
  if not HasUserDebt and not HasTargetDebt then
  begin
    Result := True;
    Exit;
  end;

  CommitRollbackActiveProjection;
  CurrentPayloadTrusted := ValidateCurrentPayloadForExecution;
  UserRollbackOk := not HasUserDebt;
  if HasUserDebt and (SeedReceiptHash = '') then
  begin
    UserRollbackOk := CurrentPayloadTrusted;
    RestoreSettings := AddBackslash(TransactionTarget) +
      'settings\FamoSettings.exe';
    if UserRollbackOk and
       (not ValidateCurrentPayloadForExecution or
        not RunAndRequire(RestoreSettings,
          '--discard-seed-transaction ' + TransactionId, True)) then
      UserRollbackOk := False;
    if UserRollbackOk and (PreviousServer <> '') and
       FileExists(PreviousServer) then
    begin
      if not ValidatePreviousPayloadForExecution then
        UserRollbackOk := False;
      if UserRollbackOk then
      begin
        ResultCode := RunBoundDesktopExitCode(PreviousServer, '', False);
        if ResultCode <> 0 then
          UserRollbackOk := False;
        if UserRollbackOk then
        begin
          Sleep(750);
          if not ValidatePreviousPayloadForExecution or
             not RunAndRequire(PreviousServer,
               '--control reload-options', True) then
            UserRollbackOk := False;
        end;
      end;
    end;
    if UserRollbackOk then
      ClearTransactionDebt('UserRollbackDebt', DebtKindUserRollback);
  end
  else if HasUserDebt then
  begin
    UserRollbackOk := CurrentPayloadTrusted;
    RestoreSettings := AddBackslash(TransactionTarget) +
      'settings\FamoSettings.exe';
    if UserRollbackOk and
       (not ValidateCurrentPayloadForExecution or
        not RunAndRequire(RestoreSettings, '--remove-input-tip', True)) then
      UserRollbackOk := False;
    if UserRollbackOk and
       (not ValidateCurrentPayloadForExecution or
        not RunAndRequire(ProfileTool(TransactionTarget),
          'cleanup-user-state', True)) then
      UserRollbackOk := False;

    if UserRollbackOk and (PreviousHost <> '') then
    begin
      if not ValidatePreviousPayloadForExecution or
         (TransactionChangedBridge and
          not RegisterPreviousRegistration) then
        UserRollbackOk := False;
      if PreviousProfileEnabled then
      begin
        EnableCommand := 'enable';
        CheckCommand := 'check';
      end
      else
      begin
        EnableCommand := 'disable';
        CheckCommand := 'check-disabled';
      end;
      if UserRollbackOk and
         (not ValidateCurrentPayloadForExecution or
          not RunAndRequire(ProfileTool(TransactionTarget),
            'clear-user-com-shadow ' + OriginalUserSid, True)) then
        UserRollbackOk := False;
      if UserRollbackOk and
         (not ValidateCurrentPayloadForExecution or
          not ValidatePreviousPayloadForExecution or
          not RunAndRequire(PreviousRegistrationTool,
            EnableCommand, True) or
          not ValidateCurrentPayloadForExecution or
          not ValidatePreviousPayloadForExecution or
          not RunAndRequire(PreviousRegistrationTool,
            CheckCommand, True)) then
        UserRollbackOk := False;

      RestoreSettings := AddBackslash(PreviousTarget) +
        'settings\FamoSettings.exe';
      if not FileExists(RestoreSettings) then
        RestoreSettings := AddBackslash(TransactionTarget) +
          'settings\FamoSettings.exe';
      if UserRollbackOk and PreviousInputTipPresent and
         (not ValidateCurrentPayloadForExecution or
          ((CompareText(ExtractFileDir(ExtractFileDir(RestoreSettings)),
             NormalizeDirectoryPath(PreviousTarget)) = 0) and
           not ValidatePreviousPayloadForExecution) or
          not RunAndRequire(RestoreSettings, '--add-input-tip', True)) then
        UserRollbackOk := False;
      if UserRollbackOk and PreviousProfileActive and
         (not ValidateCurrentPayloadForExecution or
          not ValidatePreviousPayloadForExecution or
          not RunAndRequire(PreviousRegistrationTool,
            'activate', True)) then
        UserRollbackOk := False;
      if UserRollbackOk and (PreviousServer <> '') and
         FileExists(PreviousServer) then
      begin
        if not ValidateCurrentPayloadForExecution or
           not ValidatePreviousPayloadForExecution then
          UserRollbackOk := False;
        if UserRollbackOk then
        begin
          ResultCode := RunBoundDesktopExitCode(PreviousServer, '', False);
          if ResultCode <> 0 then
            UserRollbackOk := False;
          if UserRollbackOk then
          begin
            Sleep(750);
            if not ValidateCurrentPayloadForExecution or
               not ValidatePreviousPayloadForExecution or
               not RunAndRequire(PreviousServer,
                 '--control reload-options', True) then
              UserRollbackOk := False;
          end;
        end;
      end;
    end;

    { Keep the authenticated seed receipt until every other exact-user
      restoration has succeeded, so another logon can retry safely. }
    if UserRollbackOk then
    begin
      RestoreSettings := AddBackslash(TransactionTarget) +
        'settings\FamoSettings.exe';
      if not ValidateCurrentPayloadForExecution or
         not RunAndRequire(RestoreSettings,
           '--rollback-seed-transaction ' + TransactionId + ' ' +
           SeedReceiptHash, True) then
        UserRollbackOk := False;
    end;
    if UserRollbackOk then
      ClearTransactionDebt('UserRollbackDebt', DebtKindUserRollback);
  end;

  TargetCleanupComplete := not DirExists(TransactionTarget);
  if DirExists(TransactionTarget) and UserRollbackOk then
  begin
    if CurrentPayloadTrusted and ValidateCurrentPayloadForExecution then
    begin
      if not ValidateTransactionTarget(TransactionTarget, TransactionId,
        JournalAppVersion, JournalManifestHash, PreviousTarget,
        False, ValidatedTarget) then
        RaiseException('unsafe transaction target refused during debt retry');
    end
    else if not ValidateJournalBoundPartialTargetForCleanup(
      ValidatedTarget) then
      RaiseException(
        'unsafe partial transaction target refused during debt retry');
    CurrentPayloadProofValid := False;
    if DelTree(ValidatedTarget, True, True, True) then
      TargetCleanupComplete := not DirExists(ValidatedTarget);
    if not TargetCleanupComplete and DirExists(ValidatedTarget) then
      TerminalRecoveryTargetDeleteBlocked := True;
  end;
  if TargetCleanupComplete then
    ClearTransactionDebt('TargetCleanupDebt', DebtKindTargetCleanup);
  Result := UserRollbackOk and TargetCleanupComplete;
end;

function IsFixedHexText(const Value: String; ExpectedLength: Integer): Boolean;
var
  I: Integer;
begin
  Result := False;
  if Length(Value) <> ExpectedLength then Exit;
  for I := 1 to Length(Value) do
    if not (((Value[I] >= '0') and (Value[I] <= '9')) or
            ((Value[I] >= 'A') and (Value[I] <= 'F')) or
            ((Value[I] >= 'a') and (Value[I] <= 'f'))) then
      Exit;
  Result := True;
end;

function ParseManagedVersionLeaf(const Leaf: String;
  var Version, ManifestPrefix, Id: String): Boolean;
var
  TransactionSeparator, PrefixStart, LegacyLength, Matches: Integer;
  CandidateVersion, CandidatePrefix, CandidateId: String;
begin
  Result := False;
  Version := '';
  ManifestPrefix := '';
  Id := '';
  if Length(Leaf) >= 48 then
  begin
    TransactionSeparator := Length(Leaf) - 32;
    PrefixStart := TransactionSeparator - 12;
    if (PrefixStart > 2) and (TransactionSeparator > 1) and
       (Leaf[TransactionSeparator] = '-') and
       (Leaf[PrefixStart - 1] = '-') then
    begin
      CandidateId := Copy(Leaf, TransactionSeparator + 1, 32);
      CandidatePrefix := Copy(Leaf, PrefixStart, 12);
      CandidateVersion := Copy(Leaf, 1, PrefixStart - 2);
      if (CandidateVersion <> '') and
         (CandidateId = Lowercase(CandidateId)) and
         ValidTransactionId(CandidateId) and
         IsFixedHexText(CandidatePrefix, 12) then
      begin
        Version := CandidateVersion;
        ManifestPrefix := CandidatePrefix;
        Id := CandidateId;
        Result := True;
        Exit;
      end;
    end;
  end;

  { Pre-v2 directories used yyyyMMddHHmmss-counter transaction ids.
    Retention accepts that leaf grammar only as an input to the same full
    manifest/final-object/tree proof used for v2 directories below. }
  Matches := 0;
  for LegacyLength := 16 to 21 do
  begin
    TransactionSeparator := Length(Leaf) - LegacyLength;
    PrefixStart := TransactionSeparator - 12;
    if (PrefixStart > 2) and (TransactionSeparator > 1) and
       (Leaf[TransactionSeparator] = '-') and
       (Leaf[PrefixStart - 1] = '-') then
    begin
      CandidateId := Copy(Leaf, TransactionSeparator + 1, LegacyLength);
      CandidatePrefix := Copy(Leaf, PrefixStart, 12);
      CandidateVersion := Copy(Leaf, 1, PrefixStart - 2);
      if (CandidateVersion <> '') and
         ValidLegacyTransactionId(CandidateId) and
         IsFixedHexText(CandidatePrefix, 12) then
      begin
        Matches := Matches + 1;
        Version := CandidateVersion;
        ManifestPrefix := CandidatePrefix;
        Id := CandidateId;
      end;
    end;
  end;
  Result := Matches = 1;
  if not Result then
  begin
    Version := '';
    ManifestPrefix := '';
    Id := '';
  end;
end;

function CountExactLine(Lines: TArrayOfString; const Value: String): Integer;
var
  I: Integer;
begin
  Result := 0;
  for I := 0 to GetArrayLength(Lines) - 1 do
    if Lines[I] = Value then Result := Result + 1;
end;

function VerifyManagedPayloadForCleanup(const VersionTarget, VersionFinalPath,
  Manifest, ManifestFinalPath, Version, Prefix: String): Boolean;
var
  Lines: TArrayOfString;
  SeenPaths, SeenFinalPaths, SeenObjectIds, SeenActualPaths,
    SeenActualObjectIds: TStringList;
  I, DeclaredCount, EntryCount, ActualCount, FileCountLines: Integer;
  ExpectedSize, ActualSize: Int64;
  RelativePath, ExpectedHash, FullPath, FinalPath, ObjectId,
    ManifestHash: String;
begin
  Result := False;
  if not LoadStringsFromFile(Manifest, Lines) then Exit;
  SeenPaths := TStringList.Create;
  SeenFinalPaths := TStringList.Create;
  SeenObjectIds := TStringList.Create;
  SeenActualPaths := TStringList.Create;
  SeenActualObjectIds := TStringList.Create;
  try
    try
      SeenPaths.CaseSensitive := False;
      SeenObjectIds.CaseSensitive := True;
      SeenActualObjectIds.CaseSensitive := True;
      ManifestHash := Uppercase(GetSHA256OfFile(Manifest));
      if CompareText(Copy(ManifestHash, 1, 12), Prefix) <> 0 then Exit;
      if (CountExactLine(Lines, 'format=1') <> 1) or
         (CountExactLine(Lines, 'product=Famo') <> 1) or
         (CountExactLine(Lines, 'version=' + Version) <> 1) or
         (CountExactLine(Lines, 'protocol=1') <> 1) or
         (CountExactLine(Lines, 'architecture=x64') <> 1) or
         (CountExactLine(Lines, 'identity={#Identity}') <> 1) then Exit;
      DeclaredCount := -1;
      FileCountLines := 0;
      EntryCount := 0;
      ActualCount := 0;
      for I := 0 to GetArrayLength(Lines) - 1 do
      begin
        if Pos('file_count=', Lines[I]) = 1 then
        begin
          FileCountLines := FileCountLines + 1;
          DeclaredCount := StrToInt(Copy(Lines[I], 12, Length(Lines[I])));
        end
        else if Pos('file=', Lines[I]) = 1 then
        begin
          if not ParseFileEntryDetailed(Lines[I], RelativePath,
             ExpectedSize, ExpectedHash) then Exit;
          FullPath := ExpandFileName(PathCombine(VersionTarget, RelativePath));
          if not PathStartsWith(FullPath, AddBackslash(VersionTarget), True) or
             (SeenPaths.IndexOf(FullPath) >= 0) or
             not TryGetFinalObjectInfo(FullPath, FinalPath, ObjectId) or
             not PathStartsWith(FinalPath, AddBackslash(VersionFinalPath),
               True) or
             (FindPathInList(SeenFinalPaths, FinalPath) >= 0) or
             ((ObjectId <> '') and (SeenObjectIds.IndexOf(ObjectId) >= 0)) or
             not TryGetFileSize64(FullPath, ActualSize) or
             (ActualSize <> ExpectedSize) or
             (CompareText(GetSHA256OfFile(FullPath), ExpectedHash) <> 0) then
            Exit;
          SeenPaths.Add(FullPath);
          SeenFinalPaths.Add(FinalPath);
          if ObjectId <> '' then SeenObjectIds.Add(ObjectId);
          EntryCount := EntryCount + 1;
        end
        else if (Lines[I] <> 'format=1') and
                (Lines[I] <> 'product=Famo') and
                (Lines[I] <> 'version=' + Version) and
                (Lines[I] <> 'protocol=1') and
                (Lines[I] <> 'architecture=x64') and
                (Lines[I] <> 'identity={#Identity}') then
          Exit;
      end;
      if (FileCountLines <> 1) or (DeclaredCount <> EntryCount) then Exit;
      VerifyActualPayloadFiles(VersionTarget, VersionFinalPath,
        ManifestFinalPath, SeenFinalPaths, SeenActualPaths,
        SeenActualObjectIds, ActualCount);
      Result := ActualCount = EntryCount;
    except
      Result := False;
    end;
  finally
    SeenActualObjectIds.Free;
    SeenActualPaths.Free;
    SeenObjectIds.Free;
    SeenFinalPaths.Free;
    SeenPaths.Free;
  end;
end;

function ValidateCleanupTree(const Directory, FinalRoot: String): Boolean;
var
  FindRec: TFindRec;
  Path, FinalPath, ObjectId: String;
begin
  Result := True;
  if FindFirst(AddBackslash(Directory) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Path := AddBackslash(Directory) + FindRec.Name;
          if ((FindRec.Attributes and FileAttributeReparsePoint) <> 0) or
             not TryGetFinalObjectInfo(Path, FinalPath, ObjectId) or
             not PathStartsWith(FinalPath, AddBackslash(FinalRoot), True) then
          begin
            Result := False;
            Exit;
          end;
          if ((FindRec.Attributes and FileAttributeDirectory) <> 0) and
             not ValidateCleanupTree(Path, FinalRoot) then
          begin
            Result := False;
            Exit;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function ValidateJournalBoundPartialTargetForCleanup(
  var NormalizedTarget: String): Boolean;
var
  TargetFinalPath, TargetObjectId: String;
begin
  Result :=
    (JournalPhase = PhaseRolledBack) and
    TransactionDebtPresent(
      'TargetCleanupDebt', DebtKindTargetCleanup) and
    ValidateTransactionTarget(TransactionTarget, TransactionId,
      JournalAppVersion, JournalManifestHash, PreviousTarget,
      False, NormalizedTarget) and
    TryGetFinalObjectInfo(NormalizedTarget,
      TargetFinalPath, TargetObjectId) and
    FinalObjectsSame(TargetFinalPath, TargetObjectId,
      JournalPendingFinalTarget, JournalPendingObjectId) and
    ValidateCleanupTree(NormalizedTarget, TargetFinalPath);
  if not Result then NormalizedTarget := '';
end;

function ValidateVersionDirectoryForCleanup(const VersionTarget,
  VersionsFinalPath: String): Boolean;
var
  Attributes: Cardinal;
  Exists: Boolean;
  VersionFinalPath, VersionObjectId, Manifest, ManifestFinalPath,
    ManifestObjectId, Version, Prefix, Id: String;
begin
  Result := False;
  if not TryGetPathAttributes(VersionTarget, Exists, Attributes) or
     not Exists or ((Attributes and FileAttributeDirectory) = 0) or
     ((Attributes and FileAttributeReparsePoint) <> 0) or
     not TryGetFinalObjectInfo(VersionTarget, VersionFinalPath,
       VersionObjectId) or
     not PathSame(ExtractFileDir(VersionFinalPath), VersionsFinalPath) or
     not ParseManagedVersionLeaf(ExtractFileName(VersionTarget), Version,
       Prefix, Id) then Exit;
  Manifest := AddBackslash(VersionTarget) + 'payload-manifest.txt';
  if not PathIsNonReparseOrMissing(Manifest) or not FileExists(Manifest) or
     not TryGetFinalObjectInfo(Manifest, ManifestFinalPath,
       ManifestObjectId) or
     not PathSame(ExtractFileDir(ManifestFinalPath), VersionFinalPath) then Exit;
  Result := VerifyManagedPayloadForCleanup(VersionTarget, VersionFinalPath,
    Manifest, ManifestFinalPath, Version, Prefix) and
    ValidateCleanupTree(VersionTarget, VersionFinalPath);
end;

procedure CleanupObsoleteVersions;
var
  VersionsRoot, VersionsFinalPath, VersionsObjectId, ActiveFinalPath,
  ActiveObjectId, PreviousFinalPath, PreviousObjectId, VersionTarget,
  CandidateFinalPath, CandidateObjectId, LegacyCleanupDebt: String;
  RootAttributes: Cardinal;
  RootExists, CleanupIncomplete: Boolean;
  LegacyEntryCount: Integer;
  FindRec: TFindRec;
begin
  CleanupIncomplete := False;
  if RegQueryStringValue(HKLM64, BrandKey,
       'CleanupDebt', LegacyCleanupDebt) then
  begin
    if not ValidLegacyVersionCleanupDebtForOwner(
         LegacyCleanupDebt, TransactionId, LegacyEntryCount) then
      RaiseException(
        'malformed legacy version cleanup debt blocks retention');
  end
  else if RegValueExists(
       HKLM64, BrandKey, 'CleanupDebtCount') then
    RaiseException('malformed legacy version cleanup debt blocks retention');
  ArmTransactionDebt('VersionCleanupDebt', DebtKindVersionRetention);
  VersionsRoot := NormalizeDirectoryPath(
    AddBackslash(FixedInstallRoot) + 'versions');
  if not TryGetPathAttributes(VersionsRoot, RootExists, RootAttributes) then
    RaiseException('versions root state is unavailable during retention');
  if not RootExists then
  begin
    ClearAdoptedLegacyVersionCleanupDebt;
    ClearTransactionDebt(
      'VersionCleanupDebt', DebtKindVersionRetention);
    Exit;
  end;
  if ((RootAttributes and FileAttributeDirectory) = 0) or
     ((RootAttributes and FileAttributeReparsePoint) <> 0) or
     not TryGetFinalObjectInfo(VersionsRoot, VersionsFinalPath,
       VersionsObjectId) or
     not TryGetFinalObjectInfo(TransactionTarget, ActiveFinalPath,
       ActiveObjectId) or
     not FinalObjectsSame(ActiveFinalPath, ActiveObjectId,
       JournalPendingFinalTarget, JournalPendingObjectId) then
    RaiseException('active version identity mismatch during retention');
  PreviousFinalPath := '';
  PreviousObjectId := '';
  if PreviousTarget <> '' then
  begin
    if not TryGetFinalObjectInfo(PreviousTarget, PreviousFinalPath,
       PreviousObjectId) or
       not FinalObjectsSame(PreviousFinalPath, PreviousObjectId,
         JournalPreviousFinalTarget, JournalPreviousObjectId) then
      RaiseException('previous version identity mismatch during retention');
  end;

  if FindFirst(AddBackslash(VersionsRoot) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          VersionTarget := AddBackslash(VersionsRoot) + FindRec.Name;
          CandidateFinalPath := '';
          CandidateObjectId := '';
          if ((FindRec.Attributes and FileAttributeDirectory) = 0) or
             ((FindRec.Attributes and FileAttributeReparsePoint) <> 0) or
             not TryGetFinalObjectInfo(VersionTarget, CandidateFinalPath,
               CandidateObjectId) then
          begin
            CleanupIncomplete := True;
            Log('retention left unknown versions entry: ' + VersionTarget);
          end
          else if FinalObjectsSame(CandidateFinalPath, CandidateObjectId,
                    ActiveFinalPath, ActiveObjectId) or
                  ((PreviousFinalPath <> '') and
                   FinalObjectsSame(CandidateFinalPath, CandidateObjectId,
                     PreviousFinalPath, PreviousObjectId)) then
          begin
            { Retain the active target and its exact rollback predecessor. }
          end
          else if not ValidateVersionDirectoryForCleanup(VersionTarget,
                    VersionsFinalPath) then
          begin
            CleanupIncomplete := True;
            Log('retention refused unverified version directory: ' +
              VersionTarget);
          end
          else if not DelTree(VersionTarget, True, True, True) then
          begin
            CleanupIncomplete := True;
            Log('retention deferred locked version directory: ' +
              VersionTarget);
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  if not CleanupIncomplete then
  begin
    ClearAdoptedLegacyVersionCleanupDebt;
    ClearTransactionDebt(
      'VersionCleanupDebt', DebtKindVersionRetention);
  end;
end;

procedure EnsureStableUserProfileState;
var
  Attempt, StableReadbacks, TipExit: Integer;
  Settings: String;
  ProfileHealthy, EnableOk, AddOk: Boolean;
begin
  Settings := AddBackslash(TransactionTarget) +
    'settings\FamoSettings.exe';
  StableReadbacks := 0;
  for Attempt := 1 to 6 do
  begin
    ProfileHealthy := RunAndRequire(
      ProfileTool(TransactionTarget), 'check', True);
    TipExit := RunAsOriginalUserExitCode(Settings, '--is-input-tip');
    if ProfileHealthy and (TipExit = 0) then
    begin
      StableReadbacks := StableReadbacks + 1;
      if StableReadbacks >= 2 then Exit;
    end
    else
    begin
      StableReadbacks := 0;
      EnableOk := RunAndRequire(
        ProfileTool(TransactionTarget), 'enable', True);
      AddOk := RunAndRequire(Settings, '--add-input-tip', True);
      Log('user profile persistence repair attempt ' +
        IntToStr(Attempt) + ': profileHealthy=' +
        IntToStr(Ord(ProfileHealthy)) + '; tipExit=' +
        IntToStr(TipExit) + '; enableOk=' +
        IntToStr(Ord(EnableOk)) + '; addOk=' +
        IntToStr(Ord(AddOk)));
    end;
    Sleep(2000);
  end;
  RaiseException('user profile state did not remain stable');
end;

procedure VerifyActiveInstall;
var
  RegisteredDll: String;
begin
  VerifyPayloadOrFail;
  { COM registration is machine-scoped since the HKLM re-registration fix --
    an HKCU-only TIP never shows in the Win11 immersive switcher. }
  if not RegQueryStringValue(HKLM64,
    'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '', RegisteredDll) then
    RaiseException('active COM registration missing');
  if CompareText(RegisteredDll, FixedBridgeDll) <> 0 then
    RaiseException('active COM registration target mismatch');
  if not FileExists(FixedBridgeDll) or
     (CompareText(GetSHA256OfFile(FixedBridgeDll), '{#BridgeHash}') <> 0) then
    RaiseException('active stable Bridge hash mismatch');
  if not RunAndRequire(ProfileTool(TransactionTarget), 'check-machine', False) then
    RaiseException('machine profile health readback failed');
  EnsureStableUserProfileState;
end;

procedure CompletePendingTransaction;
var
  RegisterAttempt: Integer;
  RegisterOk: Boolean;
begin
  { The predecessor can race back during logon even though the pending phase
    removed its Run entry before reboot. Retire that exact binary again before
    the replacement claims the per-session Runtime singleton. }
  if (PreviousServer <> '') and FileExists(PreviousServer) then
  begin
    if not ValidatePreviousPayloadForExecution then
      RaiseException(
        'previous payload identity mismatch before pending activation');
    if not StopRuntimeAsOriginalUser(PreviousServer) then
      RaiseException(
        'previous runtime did not exit before pending activation');
  end;
  TransitionTransactionPhase(PhaseActivateIntent);
  { The exact-SID logon task can run early in logon, before the CTF/TSF
    services are ready. Registration is idempotent, so retry briefly instead
    of failing the whole transaction on the first attempt. }
  RegisterOk := False;
  for RegisterAttempt := 1 to 5 do
  begin
    if RegisterTarget(TransactionTarget) then
    begin
      RegisterOk := True;
      Break;
    end;
    Sleep(2000);
  end;
  if not RegisterOk then
    RaiseException('pending profile registration failed');
  RegistrationSwitched := True;
  WriteActiveRegistry(TransactionTarget, 'Activating');
  TransitionTransactionPhase(PhaseMachineRegistered);
  FailIfRequested('after-resume-registration');
  InstallUserState;
  FailIfRequested('after-resume-user-state');
  VerifyActiveInstall;
  PersistUserCleanupDebtBeforeReady;
  PersistRecoveryCleanupDebtBeforeReady;
  FailIfRequested('ready-debt-before-ready');
  WriteActiveRegistry(TransactionTarget, StateReady);
  TransitionTransactionPhase(PhaseReady);
  FailIfRequested('ready-after-phase-before-seedcommit');
  InstallReady := True;
  if CommitSeedReceiptAfterReady then
  begin
    try
      FailIfRequested('after-seed-commit-before-recovery-cleanup');
      DeleteRecoveryTask;
    except
      Log('deferred recovery artifact cleanup after ready: ' +
        GetExceptionMessage);
    end;
  end;
  try
    CleanupObsoleteVersions;
  except
    Log('deferred version retention cleanup after ready: ' +
      GetExceptionMessage);
  end;
  ClearPendingRegistry;
end;

function ValidatePendingTransaction(const PendingTarget, PendingManifest,
  ExpectedId, ProtectedPreviousTarget: String;
  var NormalizedTarget: String): Boolean;
begin
  Result := ValidateTransactionTarget(PendingTarget, ExpectedId,
    '{#AppVersion}', '{#ManifestHash}', ProtectedPreviousTarget,
    False, NormalizedTarget) and
    (CompareText(PendingManifest,
      AddBackslash(NormalizedTarget) + 'payload-manifest.txt') = 0);
  if not Result then NormalizedTarget := '';
end;

function LoadPendingState(const ExpectedId: String): Boolean;
var
  NormalizedTarget, FinalTarget, ObjectId, ExpectedRecovery: String;
begin
  Result := False;
  if not LoadTransactionJournal(ExpectedId) then
  begin
    Log('pending state load rejected: journal');
    Exit;
  end;
  if not ValidateTransactionTarget(TransactionTarget, TransactionId,
       JournalAppVersion, JournalManifestHash, PreviousTarget,
       JournalPhase = PhaseReady, NormalizedTarget) or
     (CompareText(NormalizedTarget, TransactionTarget) <> 0) then
  begin
    Log('pending state load rejected: target path');
    Exit;
  end;
  Result := True;
  if DirExists(TransactionTarget) then
  begin
    Result := TryGetFinalObjectInfo(TransactionTarget, FinalTarget, ObjectId) and
      (CompareText(FinalTarget, JournalPendingFinalTarget) = 0) and
      (JournalPendingObjectId <> '') and
      (CompareText(ObjectId, JournalPendingObjectId) = 0);
    if not Result then
    begin
      Log('pending state load rejected: target object');
      Exit;
    end;
  end
  else if (JournalPhase <> PhasePrepared) and
          (JournalPhase <> PhaseRollbackIntent) and
          (JournalPhase <> PhaseRolledBack) then
  begin
    Log('pending state load rejected: target object');
    Result := False;
    Exit;
  end;
  if (PreviousTarget <> '') and
     (not TryGetFinalObjectInfo(PreviousTarget, FinalTarget, ObjectId) or
      (CompareText(FinalTarget, JournalPreviousFinalTarget) <> 0) or
      ((JournalPreviousObjectId <> '') and
       (CompareText(ObjectId, JournalPreviousObjectId) <> 0))) then
  begin
    Log('pending state load rejected: previous target identity');
    Result := False;
    Exit;
  end;
  if JournalResumeInstaller <> '' then
  begin
    ExpectedRecovery := AddBackslash(FixedInstallRoot) + 'pending\' +
      TransactionId + '\Famo-Resume-' + TransactionId + '.exe';
    Result := CompareText(JournalResumeInstaller, ExpectedRecovery) = 0;
    if FileExists(JournalResumeInstaller) then
      Result := Result and
        (CompareText(GetSHA256OfFile(JournalResumeInstaller),
          JournalResumeInstallerHash) = 0)
    else
      Result := Result and
        ((JournalPhase = PhaseReady) or (JournalPhase = PhaseRolledBack));
    if not Result then
    begin
      Log('pending state load rejected: recovery installer identity');
      Exit;
    end;
  end;
  if (JournalPhase = PhasePendingReboot) and
     ((JournalTaskName = '') or (JournalResumeInstaller = '') or
      not OriginalUserResumeCapable) then
  begin
    Log('pending state load rejected: recovery installer identity');
    Result := False;
  end;
end;

function InspectJournalGenerations(const Id: String;
  var BestGeneration: Integer): Boolean;
var
  Names: TArrayOfString;
  I, Generation: Integer;
  Candidate: TTransactionJournal;
  BaseKey, Key, PhaseText: String;
begin
  Result := True;
  BestGeneration := 0;
  BaseKey := TransactionJournalKey(Id);
  if not RegGetSubkeyNames(HKLM64, BaseKey, Names) then
  begin
    Result := False;
    Exit;
  end;
  for I := 0 to GetArrayLength(Names) - 1 do
  begin
    if (Length(Names[I]) <= 1) or (Names[I][1] <> 'g') then
    begin
      Result := False;
      Exit;
    end;
    Generation := StrToIntDef(Copy(Names[I], 2, Length(Names[I])), 0);
    if (Generation <= 0) or
       (Names[I] <> 'g' + IntToStr(Generation)) then
    begin
      Result := False;
      Exit;
    end;
    Key := BaseKey + '\' + Names[I];
    { Phase is written last. A generation without it is a legitimate
      unreachable crash remnant; a generation claiming completion must be
      digest- and semantics-valid. }
    if RegQueryStringValue(HKLM64, Key, 'Phase', PhaseText) then
    begin
      if not ReadJournalGeneration(Key, Candidate) or
         (Candidate.Generation <> IntToStr(Generation)) or
         not ValidateJournalSemantics(Candidate, Id) then
      begin
        Result := False;
        Exit;
      end;
      if Generation > BestGeneration then
        BestGeneration := Generation;
    end;
  end;
end;

function AdoptCompleteOrphanGeneration(const Id: String;
  var GenerationText: String): Boolean;
var
  BestGeneration: Integer;
  BaseKey, Key, Readback: String;
  Journal: TTransactionJournal;
begin
  Result := False;
  if not InspectJournalGenerations(Id, BestGeneration) or
     (BestGeneration <= 0) then Exit;
  BaseKey := TransactionJournalKey(Id);
  GenerationText := IntToStr(BestGeneration);
  Key := JournalGenerationKey(Id, BestGeneration);
  if not ReadJournalGeneration(Key, Journal) or
     (Journal.Generation <> GenerationText) or
     not ValidateRecoverableJournalArtifact(Journal, Id) then
    Exit;
  RequireJournalWrite(BaseKey, 'ActiveGeneration', GenerationText);
  FlushMachineRegistryKey(BaseKey);
  Result := RegQueryStringValue(HKLM64, BaseKey, 'ActiveGeneration',
    Readback) and (Readback = GenerationText);
end;

function MergeDebtOwner(const Candidate: String;
  var Owner: String): Boolean;
begin
  Result := ValidTransactionId(Candidate) and
    ((Owner = '') or (Owner = Candidate));
  if Result and (Owner = '') then Owner := Candidate;
end;

function MergeNamedTransactionDebtOwner(const Name, ExpectedKind: String;
  var Owner: String): Boolean;
var
  Value, Candidate, Kind: String;
begin
  Result := True;
  if not RegQueryStringValue(HKLM64, BrandKey, Name, Value) then Exit;
  Result := ParseTransactionDebt(Value, Candidate, Kind) and
    (Kind = ExpectedKind) and
    MergeDebtOwner(Candidate, Owner);
end;

function FindTransactionDebtOwner(var Owner: String): Boolean;
var
  Legacy, ActiveId: String;
  LegacyEntries: Integer;
begin
  Owner := '';
  Result :=
    MergeNamedTransactionDebtOwner(
      'UserCleanupDebt', DebtKindSeedCommit, Owner) and
    MergeNamedTransactionDebtOwner(
      'UserRollbackDebt', DebtKindUserRollback, Owner) and
    MergeNamedTransactionDebtOwner(
      'TargetCleanupDebt', DebtKindTargetCleanup, Owner) and
    MergeNamedTransactionDebtOwner(
      'RecoveryCleanupDebt', DebtKindRecoveryArtifacts, Owner) and
    MergeNamedTransactionDebtOwner(
      'VersionCleanupDebt', DebtKindVersionRetention, Owner);
  if not Result then Exit;
  if RegQueryStringValue(HKLM64, BrandKey, 'CleanupDebt', Legacy) then
  begin
    if ValidTransactionId(Legacy) then
      Result := MergeDebtOwner(Legacy, Owner)
    else
    begin
      Result :=
        RegQueryStringValue(HKLM64, BrandKey,
          'ActiveTransactionId', ActiveId) and
        ValidLegacyVersionCleanupDebtForOwner(
          Legacy, ActiveId, LegacyEntries) and
        MergeDebtOwner(ActiveId, Owner);
    end;
  end
  else if RegValueExists(HKLM64, BrandKey, 'CleanupDebtCount') then
    Result := False;
end;

function FindRecoverableTransaction(var RecoverId: String): Boolean;
var
  Ids: TArrayOfString;
  I, Count, Generation: Integer;
  Id, GenerationText, Key, PointerReadback, DebtOwner: String;
  Journal: TTransactionJournal;
begin
  Result := True;
  RecoverId := '';
  Count := 0;
  if not FindTransactionDebtOwner(DebtOwner) then
  begin
    Result := False;
    Exit;
  end;
  if not RepairRolledBackActiveProjection then
  begin
    Result := False;
    Exit;
  end;
  if not RegKeyExists(HKLM64, BrandKey + '\Transactions') then
  begin
    Result := DebtOwner = '';
    Exit;
  end;
  if not RegGetSubkeyNames(HKLM64, BrandKey + '\Transactions', Ids) then
  begin
    Result := False;
    Exit;
  end;
  for I := 0 to GetArrayLength(Ids) - 1 do
  begin
    Id := Ids[I];
    if not ValidTransactionId(Id) then
    begin
      Result := False;
      Exit;
    end;
    if not InspectJournalGenerations(Id, Generation) then
    begin
      Result := False;
      Exit;
    end;
    if not RegQueryStringValue(HKLM64, TransactionJournalKey(Id),
      'ActiveGeneration', GenerationText) then
    begin
      if not AdoptCompleteOrphanGeneration(Id, GenerationText) then
      begin
        Result := False;
        Exit;
      end;
    end;
    Generation := StrToIntDef(GenerationText, 0);
    if (Generation <= 0) or
       (GenerationText <> IntToStr(Generation)) then
    begin
      Result := False;
      Exit;
    end;
    Key := JournalGenerationKey(Id, Generation);
    if (Generation <= 0) or not ReadJournalGeneration(Key, Journal) or
       not ValidateRecoverableJournalArtifact(Journal, Id) then
    begin
      Result := False;
      Exit;
    end;
    if (Journal.Phase <> PhaseReady) and
       (Journal.Phase <> PhaseRolledBack) then
    begin
      if not ValidateCurrentJournalArtifact(Journal, Id) then
      begin
        Log('unfinished transaction requires its retained installer: ' +
          Journal.ResumeInstaller);
        Result := False;
        Exit;
      end;
      Count := Count + 1;
      RecoverId := Id;
    end;
    if ((Journal.Phase = PhaseReady) or
        (Journal.Phase = PhaseRolledBack)) and
       (DebtOwner <> '') and
       (Id = DebtOwner) then
    begin
      Count := Count + 1;
      RecoverId := Id;
    end;
  end;
  if (DebtOwner <> '') and
     (RecoverId <> DebtOwner) then
    Count := 2;
  Result := Count <= 1;
  if Result and (Count = 1) then
  begin
    RequireJournalWrite(BrandKey, 'ActiveTransactionId', RecoverId);
    FlushMachineRegistryKey(BrandKey);
    Result := RegQueryStringValue(HKLM64, BrandKey,
      'ActiveTransactionId', PointerReadback) and
      (PointerReadback = RecoverId);
  end;
end;

procedure CleanupAllValidatedRecoveryArtifacts;
var
  Ids: TArrayOfString;
  I, Generation, BestGeneration, NonterminalCount: Integer;
  Id, ActiveId, GenerationText, Key: String;
  Journal: TTransactionJournal;
begin
  if not RegKeyExists(HKLM64, BrandKey + '\Transactions') then
  begin
    if not RecoveryTaskFolderAbsentByCom then
      RaiseException('foreign recovery task folder blocks uninstall');
    Exit;
  end;
  if not RegQueryStringValue(HKLM64, BrandKey, 'ActiveTransactionId',
       ActiveId) or
     not ValidTransactionId(ActiveId) or
     not RegGetSubkeyNames(HKLM64, BrandKey + '\Transactions', Ids) then
    RaiseException('transaction set unavailable during recovery cleanup');
  NonterminalCount := 0;
  for I := 0 to GetArrayLength(Ids) - 1 do
  begin
    Id := Ids[I];
    if not ValidTransactionId(Id) or
       not InspectJournalGenerations(Id, BestGeneration) then
      RaiseException('invalid transaction set during recovery cleanup');
    if not RegQueryStringValue(HKLM64, TransactionJournalKey(Id),
       'ActiveGeneration', GenerationText) then
    begin
      if not AdoptCompleteOrphanGeneration(Id, GenerationText) then
        RaiseException('orphan transaction cannot be adopted during cleanup');
    end;
    Generation := StrToIntDef(GenerationText, 0);
    if (Generation <= 0) or
       (GenerationText <> IntToStr(Generation)) then
      RaiseException('invalid active generation during recovery cleanup');
    Key := JournalGenerationKey(Id, Generation);
    if not ReadJournalGeneration(Key, Journal) or
       not ValidateJournalSemantics(Journal, Id) then
      RaiseException('invalid transaction journal during recovery cleanup');
    if (Journal.Phase <> PhaseReady) and
       (Journal.Phase <> PhaseRolledBack) then
    begin
      NonterminalCount := NonterminalCount + 1;
      if (CompareText(Id, ActiveId) <> 0) or
         not ValidateCurrentJournalArtifact(Journal, Id) then
        RaiseException('foreign unfinished transaction blocks uninstall');
    end;
    ApplyTransactionJournal(Journal);
    DeleteRecoveryTask;
  end;
  if NonterminalCount > 1 then
    RaiseException('multiple unfinished transactions block uninstall');
  if not RecoveryTaskFolderAbsentByCom then
    RaiseException('foreign recovery task folder blocks uninstall');
end;

function TerminalDebtSetMatchesPhase: Boolean;
var
  HasUserCleanup, HasUserRollback, HasTargetCleanup,
    HasRecoveryCleanup, HasVersionCleanup,
    HasRecoveryArtifacts: Boolean;
  Legacy: String;
  LegacyEntries: Integer;
begin
  Result := False;
  HasUserCleanup := TransactionDebtPresent(
    'UserCleanupDebt', DebtKindSeedCommit);
  HasUserRollback := TransactionDebtPresent(
    'UserRollbackDebt', DebtKindUserRollback);
  HasTargetCleanup := TransactionDebtPresent(
    'TargetCleanupDebt', DebtKindTargetCleanup);
  HasRecoveryCleanup := TransactionDebtPresent(
    'RecoveryCleanupDebt', DebtKindRecoveryArtifacts);
  HasVersionCleanup := TransactionDebtPresent(
    'VersionCleanupDebt', DebtKindVersionRetention);
  HasRecoveryArtifacts :=
    (JournalResumeInstaller <> '') or (JournalTaskName <> '');
  if HasRecoveryCleanup and not HasRecoveryArtifacts then Exit;

  if JournalPhase = PhaseReady then
  begin
    if HasUserRollback or HasTargetCleanup or
       (HasUserCleanup and (SeedReceiptHash = '')) then Exit;
  end
  else if JournalPhase = PhaseRolledBack then
  begin
    if HasUserCleanup or HasVersionCleanup then Exit;
  end
  else
    Exit;

  if RegQueryStringValue(HKLM64, BrandKey, 'CleanupDebt', Legacy) then
  begin
    if JournalPhase = PhaseRolledBack then
    begin
      if (Legacy <> TransactionId) or
         RegValueExists(HKLM64, BrandKey, 'CleanupDebtCount') then Exit;
    end
    else
    begin
      if not ValidLegacyVersionCleanupDebtForOwner(
           Legacy, TransactionId, LegacyEntries) then Exit;
    end;
  end
  else if RegValueExists(HKLM64, BrandKey, 'CleanupDebtCount') then
    Exit;
  Result := True;
end;

function RecoverTerminalTransaction: Boolean;
var
  HasRecoveryArtifacts, HasRecoveryDebt: Boolean;
begin
  Result := False;
  TerminalRecoveryTargetDeleteBlocked := False;
  if not TerminalDebtSetMatchesPhase then Exit;
  HasRecoveryArtifacts :=
    (JournalResumeInstaller <> '') or (JournalTaskName <> '');
  HasRecoveryDebt := TransactionDebtPresent(
    'RecoveryCleanupDebt', DebtKindRecoveryArtifacts);
  if HasRecoveryArtifacts and not HasRecoveryDebt and
     (not RecoveryTaskFolderAbsentByCom or
      FileExists(ExpectedRecoveryInstaller(TransactionId)) or
      DirExists(ExpectedRecoveryDirectory(TransactionId))) then
    Exit;

  if JournalPhase = PhaseRolledBack then
  begin
    RestorePreviousRegistry;
    CommitRollbackActiveProjection;
    { A terminal rollback must not keep replaying an obsolete installer at
      every logon. Retire its exact authenticated task before attempting to
      delete a TSF DLL that may remain loaded until the next reboot. }
    if HasRecoveryDebt then
    begin
      try
        DeleteRecoveryTask;
      except
        Log('deferred terminal recovery artifact cleanup: ' +
          GetExceptionMessage);
        Exit;
      end;
    end;
    if TransactionDebtPresent(
         'UserRollbackDebt', DebtKindUserRollback) then
      CaptureOriginalUserIdentity;
    if not RetryRolledBackCleanupDebt then Exit;
  end
  else if JournalPhase = PhaseReady then
  begin
    if TransactionDebtPresent(
         'UserCleanupDebt', DebtKindSeedCommit) then
    begin
      CaptureOriginalUserIdentity;
      if not CommitSeedReceiptAfterReady then Exit;
    end;
  end
  else
    Exit;

  if (JournalPhase = PhaseReady) and HasRecoveryDebt then
  begin
    try
      DeleteRecoveryTask;
    except
      Log('deferred terminal recovery artifact cleanup: ' +
        GetExceptionMessage);
      Exit;
    end;
  end;

  if JournalPhase = PhaseReady then
  begin
    try
      CleanupObsoleteVersions;
    except
      Log('deferred terminal version retention cleanup: ' +
        GetExceptionMessage);
      Exit;
    end;
    if TransactionDebtPresent(
         'VersionCleanupDebt', DebtKindVersionRetention) then
      Exit;
  end;
  Result :=
    not TransactionDebtPresent(
      'UserCleanupDebt', DebtKindSeedCommit) and
    not TransactionDebtPresent(
      'UserRollbackDebt', DebtKindUserRollback) and
    not TransactionDebtPresent(
      'TargetCleanupDebt', DebtKindTargetCleanup) and
    not TransactionDebtPresent(
      'RecoveryCleanupDebt', DebtKindRecoveryArtifacts) and
    not TransactionDebtPresent(
      'VersionCleanupDebt', DebtKindVersionRetention);
end;

procedure ResetLoadedTransactionForFreshInstall;
begin
  TransactionId := '';
  TransactionTarget := '';
  PreviousTarget := '';
  PreviousManifest := '';
  PreviousManifestHash := '';
  PreviousDefault := '';
  PreviousState := '';
  PreviousHost := '';
  PreviousServer := '';
  PreviousProfileTool := '';
  PreviousVersion := '';
  PreviousIdentity := '';
  PreviousTransactionId := '';
  PreviousCompatibilityTransactionId := '';
  PreviousProfileActive := False;
  PreviousProfileEnabled := False;
  PreviousInputTipPresent := False;
  SeedReceiptHash := '';
  OriginalUserSid := '';
  OriginalUserAccount := '';
  OriginalUserSession := '';
  CurrentOriginalUserSession := '';
  OriginalUserResumeCapable := False;
  JournalPhase := '';
  JournalAppVersion := '';
  JournalManifestHash := '';
  JournalPendingFinalTarget := '';
  JournalPendingObjectId := '';
  JournalPreviousFinalTarget := '';
  JournalPreviousObjectId := '';
  PriorPreviousTarget := '';
  JournalPriorPreviousFinalTarget := '';
  JournalPriorPreviousObjectId := '';
  JournalResumeInstaller := '';
  JournalResumeInstallerHash := '';
  JournalTaskName := '';
  JournalAllowDowngrade := False;
  JournalGeneration := 0;
  LoadedHostDetected := False;
  LoadedHostHash := '';
  LoadedHostVersion := '';
  LoadedHostExpectedHash := '';
  ResumeMode := False;
  RollbackMode := False;
  PendingTerminal := False;
  TransactionPrepared := False;
  RegistrationSwitched := False;
  InstallReady := False;
  RollbackComplete := False;
  RuntimeStarted := False;
  TerminalRecoveryTargetDeleteBlocked := False;
  CurrentPayloadProofValid := False;
end;

function InitializeSetup: Boolean;
var
  ResumeId, RollbackId, RecoverId, RequestedId, ManifestArgument,
    VersionArgument: String;
begin
  Result := False;
  if not AcquireEarlyTransactionMutex then Exit;
  PinRunningSetupSource;
  RequireFixedProtectedInstallRoot;
  if not RecoverHelperCleanupDebt then
  begin
    Result := False;
    Exit;
  end;
  if RegValueExists(HKLM64, BrandKey, 'UninstallIntentOwner') or
     RegValueExists(HKLM64, BrandKey, 'UninstallIntent') or
     RegValueExists(HKLM64, UninstallDeleteAnchorKey, 'Owner') or
     RegValueExists(HKLM64, UninstallDeleteAnchorKey, 'Commit') then
  begin
    Log('setup blocked by committed uninstall recovery state');
    Result := False;
    Exit;
  end;
  ResumeId := ExpandConstant('{param:FamoResume|}');
  RollbackId := ExpandConstant('{param:FamoRollback|}');
  RecoverId := ExpandConstant('{param:FamoRecover|}');
  if Ord(ResumeId <> '') + Ord(RollbackId <> '') + Ord(RecoverId <> '') > 1 then
  begin
    Result := False;
    Exit;
  end;
  RequestedId := ResumeId;
  if RequestedId = '' then RequestedId := RollbackId;
  if RequestedId = '' then RequestedId := RecoverId;
  if RequestedId <> '' then
  begin
    if RecoverId <> '' then
    begin
      ManifestArgument := ExpandConstant('{param:FamoManifest|}');
      VersionArgument := ExpandConstant('{param:FamoVersion|}');
      if (CompareText(ManifestArgument, '{#ManifestHash}') <> 0) or
         (CompareText(VersionArgument, '{#AppVersion}') <> 0) then
      begin
        Result := False;
        Exit;
      end;
    end;
    Result := LoadPendingState(RequestedId);
    if not Result then Exit;
    if (JournalPhase = PhaseReady) or
       (JournalPhase = PhaseRolledBack) then
    begin
      RecoverTerminalTransaction;
      { A stale scheduled invocation against a durable terminal generation is
        a successful no-op. Returning here prevents any payload extraction or
        state transition. }
      Result := False;
      Exit;
    end;
    RollbackMode := (RollbackId <> '') or
      (JournalPhase <> PhasePendingReboot);
    ResumeMode := not RollbackMode;
    Exit;
  end;
  if not FindRecoverableTransaction(RequestedId) then
  begin
    Log('setup blocked because the retained transaction set failed validation');
    SuppressibleMsgBox(
      '检测到无法安全识别的历史安装记录，安装未继续。' + #13#10 +
      '请保留安装日志并联系支持。',
      mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;
  if RequestedId <> '' then
  begin
    Result := LoadPendingState(RequestedId);
    if not Result then Exit;
    if (JournalPhase = PhaseReady) or
       (JournalPhase = PhaseRolledBack) then
    begin
      if not RecoverTerminalTransaction then
      begin
        Log('ordinary setup deferred by terminal transaction cleanup');
        if TerminalRecoveryTargetDeleteBlocked then
          SuppressibleMsgBox(
            '上次更新的旧输入法文件仍未能清理，通常是文件仍被系统占用。' + #13#10 +
            '请重新启动 Windows，然后再次运行此安装包。',
            mbInformation, MB_OK, IDOK)
        else
          SuppressibleMsgBox(
            '无法完成上次更新的安全恢复，安装未继续。' + #13#10 +
            '请保留安装日志并联系支持。',
            mbError, MB_OK, IDOK);
        Result := False;
        Exit;
      end;
      RequestedId := '';
      if not FindRecoverableTransaction(RequestedId) or
         (RequestedId <> '') then
      begin
        Result := False;
        Exit;
      end;
      ResetLoadedTransactionForFreshInstall;
      Result := True;
      Exit;
    end;
    RollbackMode := JournalPhase <> PhasePendingReboot;
    ResumeMode := not RollbackMode;
    Exit;
  end;
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Failure: String;
begin
  if CurStep = ssInstall then
  begin
    VerifyFrozenBridgePreflight;
    PrepareTransaction;
  end;
  if CurStep = ssPostInstall then
  begin
    if RollbackMode then
    begin
      RollbackTransaction;
      InstallReady := True;
      Exit;
    end;
    try
      VerifyPayloadOrFail;
      if (PreviousServer <> '') and FileExists(PreviousServer) then
      begin
        if not ValidatePreviousPayloadForExecution then
          RaiseException('previous payload identity mismatch before runtime shutdown');
        if not StopRuntimeAsOriginalUser(PreviousServer) then
          RaiseException('previous runtime did not exit after shutdown');
      end;
      if JournalPhase = PhasePrepared then
        TransitionTransactionPhase(PhasePayloadVerified);
      CheckDowngradePolicy;
      FailIfRequested('after-verify');
      LoadedHostDetected := False;
      if TransactionChangedBridge then
        LoadedHostDetected := DetectLoadedPreviousHost;
      if ResumeMode and LoadedHostDetected then
      begin
        VerifyPendingInstall;
        WritePendingRegistry;
        PendingTerminal := True;
        InstallReady := True;
      end
      else if ResumeMode then
        CompletePendingTransaction
      else
      begin
        CapturePreviousUserState;
        if LoadedHostDetected then
          EnterPendingReboot
        else
        begin
          if OriginalUserResumeCapable then
            ScheduleRecoveryTask
          else
          begin
            RetainRecoveryInstaller;
            TransitionTransactionPhase(PhaseResumeArmed);
          end;
          SwitchRegistration;
          FailIfRequested('after-switch');
          InstallUserState;
          FailIfRequested('after-user-state');
          VerifyActiveInstall;
          FailIfRequested('after-active-verify');
          PersistUserCleanupDebtBeforeReady;
          PersistRecoveryCleanupDebtBeforeReady;
          FailIfRequested('ready-debt-before-ready');
          WriteActiveRegistry(TransactionTarget, StateReady);
          TransitionTransactionPhase(PhaseReady);
          FailIfRequested('ready-after-phase-before-seedcommit');
          InstallReady := True;
          if CommitSeedReceiptAfterReady then
          begin
            try
              FailIfRequested(
                'after-seed-commit-before-recovery-cleanup');
              DeleteRecoveryTask;
            except
              Log('deferred recovery artifact cleanup after ready: ' +
                GetExceptionMessage);
            end;
          end;
          try
            CleanupObsoleteVersions;
          except
            Log('deferred version retention cleanup after ready: ' +
              GetExceptionMessage);
          end;
          ClearPendingRegistry;
        end;
      end;
    except
      Failure := GetExceptionMessage;
      Log('installation failed before rollback: ' + Failure);
      if (JournalPhase <> PhaseReady) and
         (JournalPhase <> PhaseRolledBack) then
      begin
        try
          RollbackTransaction;
        except
          Log('rollback compensation failed: ' + GetExceptionMessage);
        end;
      end;
      if JournalPhase = PhaseRolledBack then
        InstallReady := True;
      if WizardSilent then
      begin
        Log('silent setup aborted after durable rollback');
        Abort;
      end;
      RaiseException(Failure);
    end;
  end;
end;

function NeedRestart: Boolean;
begin
  Result := PendingTerminal;
end;

procedure DeinitializeSetup;
begin
  try
    if TransactionPrepared and not InstallReady then RollbackTransaction;
    if ResumeMode and InstallReady and not PendingTerminal then
      DelayDeleteFile(ExpandConstant('{srcexe}'), 5);
  finally
    ReleaseEarlyTransactionMutex;
  end;
end;

function InitializeUninstall: Boolean;
begin
  Result := False;
  if not AcquireEarlyTransactionMutex then Exit;
  UninstallPrepared := False;
  UninstallDeleteArmed := False;
  UninstallOwner := '';
  RequireFixedProtectedInstallRoot;
  if not RecoverHelperCleanupDebt then
  begin
    Result := False;
    Exit;
  end;
  DeleteUserData := False;
  if not UninstallSilent then
    DeleteUserData := MsgBox('是否同时删除 %LOCALAPPDATA%\Famo 中的用户词库和设置？',
      mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
  Result := True;
end;

procedure DeinitializeUninstall;
begin
  ReleaseEarlyTransactionMutex;
end;

function OnlyLoadedHostResidue(const VersionTarget, LoadedHost: String): Boolean;
var
  FindRec: TFindRec;
  Path: String;
begin
  Result := FileExists(LoadedHost);
  if not Result then Exit;
  if FindFirst(AddBackslash(VersionTarget) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Path := AddBackslash(VersionTarget) + FindRec.Name;
          if ((FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) or
             (CompareText(Path, LoadedHost) <> 0) then
          begin
            Log('unexpected uninstall residue: ' + Path);
            Result := False;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function ScheduleLoadedHostResidueForRestart(const VersionsRoot: String): Boolean;
var
  FindRec: TFindRec;
  VersionTarget, LoadedHost: String;
begin
  Result := True;
  if FindFirst(AddBackslash(VersionsRoot) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          VersionTarget := AddBackslash(VersionsRoot) + FindRec.Name;
          LoadedHost := AddBackslash(VersionTarget) + 'FamoTextService.dll';
          if ((FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) or
             not OnlyLoadedHostResidue(VersionTarget, LoadedHost) then
            Result := False
          else
          begin
            try
              RestartReplace(LoadedHost, '');
              RestartReplace(VersionTarget, '');
              Log('loaded TSF host scheduled for restart deletion: ' + LoadedHost);
            except
              Log('cannot schedule loaded TSF host deletion: ' + GetExceptionMessage);
              Result := False;
            end;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  if Result then
  begin
    try
      RestartReplace(VersionsRoot, '');
      UninstallRestartPending := True;
    except
      Log('cannot schedule versions directory deletion: ' + GetExceptionMessage);
      Result := False;
    end;
  end;
end;

function OnlyLoadedStableBridgeResidue(const BridgeRoot: String): Boolean;
var
  FindRec: TFindRec;
  Path: String;
begin
  Result := FileExists(FixedBridgeDll) and
    (CompareText(GetSHA256OfFile(FixedBridgeDll), '{#BridgeHash}') = 0);
  if not Result then Exit;
  if FindFirst(AddBackslash(BridgeRoot) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Path := AddBackslash(BridgeRoot) + FindRec.Name;
          if ((FindRec.Attributes and FileAttributeDirectory) = 0) or
             (CompareText(Path, FixedBridgeDirectory) <> 0) then
          begin
            Result := False;
            Exit;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  if FindFirst(AddBackslash(FixedBridgeDirectory) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Path := AddBackslash(FixedBridgeDirectory) + FindRec.Name;
          if ((FindRec.Attributes and FileAttributeDirectory) <> 0) or
             (CompareText(Path, FixedBridgeDll) <> 0) then
          begin
            Result := False;
            Exit;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure CleanupStableBridgeForUninstall;
var
  BridgeRoot, BridgeManifest, FinalPath, ObjectId: String;
begin
  BridgeRoot := AddBackslash(FixedInstallRoot) + 'bridge';
  if not DirExists(BridgeRoot) then Exit;
  if not TryGetFinalObjectInfo(BridgeRoot, FinalPath, ObjectId) or
     not ValidateCleanupTree(BridgeRoot, FinalPath) then
    RaiseException('unsafe stable Bridge tree refused during uninstall');
  BridgeManifest :=
    AddBackslash(FixedBridgeDirectory) + 'bridge-manifest.txt';
  if FileExists(BridgeManifest) and not DeleteFile(BridgeManifest) then
    RaiseException('cannot delete stable Bridge manifest');
  if DelTree(BridgeRoot, True, True, True) then Exit;
  if not OnlyLoadedStableBridgeResidue(BridgeRoot) then
    RaiseException('unexpected stable Bridge uninstall residue');
  try
    RestartReplace(FixedBridgeDll, '');
    RestartReplace(FixedBridgeDirectory, '');
    RestartReplace(BridgeRoot, '');
    UninstallRestartPending := True;
    Log('loaded stable Bridge scheduled for restart deletion: ' +
      FixedBridgeDll);
  except
    RaiseException('cannot schedule stable Bridge deletion: ' +
      GetExceptionMessage);
  end;
end;

function UninstallNeedRestart: Boolean;
begin
  Result := UninstallRestartPending;
end;

function UninstallDeleteAnchorDigest(const Owner: String): String;
begin
  Result := Uppercase(GetSHA256OfString(
    JournalCanonicalField('schema', UninstallDeleteAnchorSchema) +
    JournalCanonicalField('owner', Owner)));
end;

procedure PersistUninstallDeleteAnchor(const Owner: String);
var
  ExpectedDigest, StoredOwner, StoredDigest, StoredCommit: String;
begin
  if not ValidTransactionId(Owner) then
    RaiseException('invalid uninstall delete anchor owner');
  ExpectedDigest := UninstallDeleteAnchorDigest(Owner);
  RequireJournalWrite(UninstallDeleteAnchorKey, 'Owner', Owner);
  RequireJournalWrite(UninstallDeleteAnchorKey, 'Digest', ExpectedDigest);
  FailIfRequested('uninstall-delete-anchor-before-commit');
  RequireJournalWrite(UninstallDeleteAnchorKey, 'Commit', UninstallDeleteAnchorSchema);
  FlushMachineRegistryKey(UninstallDeleteAnchorKey);
  if not RegQueryStringValue(HKLM64, UninstallDeleteAnchorKey,
       'Owner', StoredOwner) or
     not RegQueryStringValue(HKLM64, UninstallDeleteAnchorKey,
       'Digest', StoredDigest) or
     not RegQueryStringValue(HKLM64, UninstallDeleteAnchorKey,
       'Commit', StoredCommit) or
     (CompareText(StoredOwner, Owner) <> 0) or
     (CompareText(StoredDigest, ExpectedDigest) <> 0) or
     (StoredCommit <> UninstallDeleteAnchorSchema) then
    RaiseException('uninstall delete anchor commit readback failed');
  UninstallOwner := Owner;
  UninstallDeleteArmed := True;
  FailIfRequested('uninstall-delete-anchor-after-commit');
end;

function LoadCommittedUninstallDeleteAnchor(var Owner: String): Boolean;
var
  StoredOwner, StoredDigest, StoredCommit, ActiveId: String;
begin
  Result := False;
  Owner := '';
  if not RegQueryStringValue(HKLM64, UninstallDeleteAnchorKey,
       'Commit', StoredCommit) then Exit;
  if (StoredCommit <> UninstallDeleteAnchorSchema) or
     not RegQueryStringValue(HKLM64, UninstallDeleteAnchorKey,
       'Owner', StoredOwner) or
     not RegQueryStringValue(HKLM64, UninstallDeleteAnchorKey,
       'Digest', StoredDigest) or
     not ValidTransactionId(StoredOwner) or
     (CompareText(StoredDigest,
       UninstallDeleteAnchorDigest(StoredOwner)) <> 0) or
     (RegQueryStringValue(HKLM64, BrandKey,
        'ActiveTransactionId', ActiveId) and
      (CompareText(ActiveId, StoredOwner) <> 0)) then
    RaiseException('invalid committed uninstall delete anchor');
  Owner := StoredOwner;
  Result := True;
end;

procedure RetireUninstallDeleteAnchor;
var
  StoredCommit: String;
begin
  if RegQueryStringValue(HKLM64, UninstallDeleteAnchorKey,
       'Commit', StoredCommit) then
  begin
    if StoredCommit <> UninstallDeleteAnchorSchema then
      RaiseException('foreign uninstall delete anchor blocks retirement');
    if not RegDeleteValue(HKLM64, UninstallDeleteAnchorKey, 'Commit') then
      RaiseException('cannot retire uninstall delete anchor commit');
    FlushMachineRegistryKey(UninstallDeleteAnchorKey);
    if RegValueExists(HKLM64, UninstallDeleteAnchorKey, 'Commit') then
      RaiseException('uninstall delete anchor commit retirement failed');
  end;
  if RegKeyExists(HKLM64, UninstallDeleteAnchorKey) then
  begin
    if not RegDeleteKeyIncludingSubkeys(
         HKLM64, UninstallDeleteAnchorKey) then
      RaiseException('cannot retire uninstall delete anchor');
    FlushMachineRegistryKey(FamoRootKey);
    if RegKeyExists(HKLM64, UninstallDeleteAnchorKey) then
      RaiseException('uninstall delete anchor retirement readback failed');
  end;
  UninstallDeleteArmed := False;
end;

procedure PersistUninstallIntent(const Owner, Target, FinalTarget,
  ObjectId: String);
begin
  RequireJournalWrite(BrandKey, 'UninstallIntentOwner', Owner);
  RequireJournalWrite(BrandKey, 'UninstallIntentTarget', Target);
  RequireJournalWrite(BrandKey, 'UninstallIntentFinalTarget', FinalTarget);
  RequireJournalWrite(BrandKey, 'UninstallIntentObjectId', ObjectId);
  FailIfRequested('uninstall-intent-before-commit');
  RequireJournalWrite(BrandKey, 'UninstallIntent', UninstallIntentSchema);
  FlushMachineRegistryKey(BrandKey);
  FailIfRequested('uninstall-intent-after-commit');
end;

function LoadCommittedUninstallIntent(const ActiveId: String;
  var IntentTarget: String): Boolean;
var
  Schema, Owner, FinalTarget, ObjectId, NormalizedTarget, ExpectedTarget,
    CurrentFinalTarget, CurrentObjectId: String;
begin
  Result := False;
  IntentTarget := '';
  if not RegQueryStringValue(
       HKLM64, BrandKey, 'UninstallIntent', Schema) then Exit;
  if (Schema <> UninstallIntentSchema) or
     not RegQueryStringValue(
       HKLM64, BrandKey, 'UninstallIntentOwner', Owner) or
     not RegQueryStringValue(
       HKLM64, BrandKey, 'UninstallIntentTarget', IntentTarget) or
     not RegQueryStringValue(
       HKLM64, BrandKey, 'UninstallIntentFinalTarget', FinalTarget) or
     not RegQueryStringValue(
       HKLM64, BrandKey, 'UninstallIntentObjectId', ObjectId) or
     (CompareText(Owner, ActiveId) <> 0) or
     not LoadTransactionJournal(ActiveId) then
    RaiseException('invalid committed uninstall intent');

  if JournalPhase = PhaseReady then
  begin
    if (CompareText(TransactionTarget, IntentTarget) <> 0) or
       (CompareText(JournalPendingFinalTarget, FinalTarget) <> 0) or
       (CompareText(JournalPendingObjectId, ObjectId) <> 0) then
      RaiseException('invalid committed Ready uninstall intent');
    ExpectedTarget := NormalizeDirectoryPath(
      AddBackslash(FixedInstallRoot) + 'versions\' +
      JournalAppVersion + '-' + Copy(JournalManifestHash, 1, 12) +
      '-' + ActiveId);
  end
  else if (JournalPhase = PhaseRolledBack) and
          (PreviousTransactionId = '') and
          (PreviousTarget <> '') then
  begin
    if (CompareText(PreviousTarget, IntentTarget) <> 0) or
       (CompareText(JournalPreviousFinalTarget, FinalTarget) <> 0) or
       (CompareText(JournalPreviousObjectId, ObjectId) <> 0) or
       not ValidLegacyTransactionId(
         PreviousCompatibilityTransactionId) then
      RaiseException('invalid committed legacy uninstall intent');
    ExpectedTarget := NormalizeDirectoryPath(
      AddBackslash(FixedInstallRoot) + 'versions\' +
      PreviousVersion + '-' + Copy(PreviousManifestHash, 1, 12) +
      '-' + PreviousCompatibilityTransactionId);
  end
  else
    RaiseException('invalid committed uninstall intent phase');

  NormalizedTarget := NormalizeDirectoryPath(IntentTarget);
  if not PathSame(NormalizedTarget, ExpectedTarget) or
     not PathIsNonReparseOrMissing(NormalizedTarget) then
    RaiseException('unsafe committed uninstall target');
  if DirExists(IntentTarget) and
     (not TryGetFinalObjectInfo(
        IntentTarget, CurrentFinalTarget, CurrentObjectId) or
      not FinalObjectsSame(
        CurrentFinalTarget, CurrentObjectId, FinalTarget, ObjectId)) then
    RaiseException('committed uninstall target identity changed');
  Result := True;
end;

procedure RemoveActiveInstall;
var
  ActiveTarget, RegisteredDll, ActiveTransactionId, BrandTarget,
    BrandManifest, IntentTarget, IntentFinalTarget,
    IntentObjectId: String;
  EmptyRollbackAnchor, ReadyAnchor: Boolean;
begin
  RequireFixedProtectedInstallRoot;
  if LoadCommittedUninstallDeleteAnchor(UninstallOwner) then
  begin
    UninstallDeleteArmed := True;
    TransactionId := UninstallOwner;
    RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
    if RegQueryStringValue(HKLM64, RunKey, 'FamoRuntime',
         RegisteredDll) or
       RegQueryStringValue(HKLM64,
         'Software\Classes\CLSID\' + StableClsid +
         '\InprocServer32', '', RegisteredDll) or
       not RecoveryTaskFolderAbsentByCom then
      RaiseException(
        'committed uninstall delete anchor has live machine state');
    UninstallPrepared := True;
    Exit;
  end;
  if not RegQueryStringValue(HKLM64, BrandKey, 'ActiveTransactionId',
       ActiveTransactionId) then
  begin
    if RegKeyExists(HKLM64, BrandKey) or
       RegQueryStringValue(HKLM64, RunKey, 'FamoRuntime',
         RegisteredDll) or
       RegQueryStringValue(HKLM64,
         'Software\Classes\CLSID\' + StableClsid +
         '\InprocServer32', '', RegisteredDll) or
       not RecoveryTaskFolderAbsentByCom then
      RaiseException('cannot load the active transaction for uninstall');
    UninstallDeleteArmed := True;
    UninstallOwner := '';
    UninstallPrepared := True;
    Exit;
  end;
  UninstallOwner := ActiveTransactionId;
  if LoadCommittedUninstallIntent(ActiveTransactionId, IntentTarget) then
  begin
    if MachineComPointsToTarget(IntentTarget) and
       not RunTrustedDirectMachineUnregister then
      RaiseException('cannot finish committed machine unregister');
    RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
    if RegQueryStringValue(HKLM64,
      'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '',
      RegisteredDll) then
      RaiseException(
        'dangling machine COM registration after uninstall re-entry');
    DeleteRecoveryTask;
    UninstallPrepared := True;
    Exit;
  end;
  if not LoadPendingState(ActiveTransactionId) then
    RaiseException('cannot load the active transaction for uninstall');
  EmptyRollbackAnchor := False;
  ReadyAnchor := JournalPhase = PhaseReady;
  if JournalPhase = PhaseReady then
  begin
    if not RegQueryStringValue(HKLM64, BrandKey, 'InstallDir',
         BrandTarget) or
       not RegQueryStringValue(HKLM64, BrandKey, 'ActiveManifest',
         BrandManifest) or
       (CompareText(NormalizeDirectoryPath(BrandTarget),
         NormalizeDirectoryPath(TransactionTarget)) <> 0) or
       (CompareText(BrandManifest,
         AddBackslash(TransactionTarget) + 'payload-manifest.txt') <> 0) then
      RaiseException('Ready active journal projection mismatch');
    ActiveTarget := TransactionTarget;
    if not ValidateCurrentPayloadForExecution then
      RaiseException('Ready active payload execution proof failed');
  end
  else if (JournalPhase = PhaseRolledBack) and
          (PreviousTransactionId = '') and
          (PreviousTarget = '') and
          IsEmptyRollbackAnchorForProjection(ActiveTransactionId) then
  begin
    ActiveTarget := '';
    EmptyRollbackAnchor := True;
  end
  else if (JournalPhase = PhaseRolledBack) and
          (PreviousTransactionId = '') and
          IsLegacyRollbackAnchorForProjection(ActiveTransactionId,
            PreviousTarget, PreviousManifest) then
    ActiveTarget := PreviousTarget
  else
    RaiseException('active transaction is not a safe uninstall anchor');
  if (not EmptyRollbackAnchor) and
     ((ActiveTarget = '') or (OriginalUserSid = '')) then
    RaiseException('active transaction identity is incomplete');
  if EmptyRollbackAnchor then
  begin
    DeleteRecoveryTask;
    UninstallPrepared := True;
    Exit;
  end;
  if ReadyAnchor and not CommitSeedReceiptAfterReady then
    RaiseException('cannot commit the authenticated seed receipt before uninstall');
  if (ReadyAnchor and not ValidateCurrentPayloadForExecution) or
     ((not ReadyAnchor) and not ValidatePreviousPayloadForExecution) then
    RaiseException('active payload changed before original-user cleanup');
  if not RunAndRequire(ProfileTool(ActiveTarget),
    'cleanup-user-for ' + OriginalUserSid, False) then
    RaiseException('cannot clean the original desktop user before uninstall');
  if DeleteUserData then
  begin
    if (ReadyAnchor and not ValidateCurrentPayloadForExecution) or
       ((not ReadyAnchor) and not ValidatePreviousPayloadForExecution) then
      RaiseException('active payload changed before original-user data cleanup');
    if not RunAndRequire(ProfileTool(ActiveTarget),
      'delete-user-data-for ' + OriginalUserSid, False) then
      RaiseException('cannot delete the exact original user data');
  end;
  if (ReadyAnchor and not ValidateCurrentPayloadForExecution) or
     ((not ReadyAnchor) and not ValidatePreviousPayloadForExecution) then
    RaiseException('active payload changed before machine unregister');
  if not UnregisterMachineTarget(ActiveTarget) then
    RaiseException('cannot unregister Famo profile');
  RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
  if RegQueryStringValue(HKLM64,
    'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '', RegisteredDll) then
    RaiseException('dangling machine COM registration after unregister');
  if not TryGetFinalObjectInfo(
       ActiveTarget, IntentFinalTarget, IntentObjectId) or
     (ReadyAnchor and
      not FinalObjectsSame(
        IntentFinalTarget, IntentObjectId,
        JournalPendingFinalTarget, JournalPendingObjectId)) or
     ((not ReadyAnchor) and
      not FinalObjectsSame(
        IntentFinalTarget, IntentObjectId,
        JournalPreviousFinalTarget, JournalPreviousObjectId)) then
    RaiseException('active target changed before uninstall intent');
  PersistUninstallIntent(
    ActiveTransactionId, ActiveTarget,
    IntentFinalTarget, IntentObjectId);
  DeleteRecoveryTask;
  UninstallPrepared := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  VersionsRoot, PendingRoot, TreeFinalPath, TreeObjectId: String;
begin
  if (CurUninstallStep = usUninstall) and not UninstallPrepared then
    RemoveActiveInstall;
  if CurUninstallStep = usPostUninstall then
  begin
    RequireFixedProtectedInstallRoot;
    RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
    FlushMachineRegistryKey(RunKey);
    if RegQueryStringValue(HKLM64, RunKey, 'FamoRuntime',
         TreeFinalPath) then
      RaiseException('runtime startup registration survived uninstall');
    FlushMachineRegistryKey('Software\Classes\CLSID');
    if RegQueryStringValue(HKLM64,
         'Software\Classes\CLSID\' + StableClsid +
         '\InprocServer32', '', TreeFinalPath) then
      RaiseException('machine COM registration survived uninstall');
    if not UninstallDeleteArmed then
      CleanupAllValidatedRecoveryArtifacts;
    VersionsRoot := AddBackslash(FixedInstallRoot) + 'versions';
    if DirExists(VersionsRoot) then
    begin
      if not TryGetFinalObjectInfo(VersionsRoot, TreeFinalPath,
           TreeObjectId) or
         not ValidateCleanupTree(VersionsRoot, TreeFinalPath) then
        RaiseException('unsafe versions tree refused during uninstall');
      if not DelTree(VersionsRoot, True, True, True) and
         not ScheduleLoadedHostResidueForRestart(VersionsRoot) then
        RaiseException('cannot schedule transaction version cleanup');
    end;
    CleanupStableBridgeForUninstall;
    PendingRoot := AddBackslash(FixedInstallRoot) + 'pending';
    if DirExists(PendingRoot) then
    begin
      if not TryGetFinalObjectInfo(PendingRoot, TreeFinalPath,
           TreeObjectId) or
         not ValidateCleanupTree(PendingRoot, TreeFinalPath) or
         not DelTree(PendingRoot, True, True, True) then
        RaiseException('unsafe pending tree refused during uninstall');
    end;
    { The authenticated transaction journal lives inside BrandKey. Arm a
      sibling recovery record only after every journal-dependent cleanup has
      succeeded, then it can safely bridge a torn recursive key deletion. }
    if not UninstallDeleteArmed then
      PersistUninstallDeleteAnchor(UninstallOwner);
    if RegKeyExists(HKLM64, BrandKey) then
    begin
      if not RegDeleteKeyIncludingSubkeys(HKLM64, BrandKey) then
        RaiseException('cannot delete the committed install registry');
      FlushMachineRegistryKey(FamoRootKey);
      if RegKeyExists(HKLM64, BrandKey) then
        RaiseException('install registry deletion readback failed');
    end;
    FailIfRequested('uninstall-brand-deleted-before-anchor-retire');
    RetireUninstallDeleteAnchor;
  end;
end;
