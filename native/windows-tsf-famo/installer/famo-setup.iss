; Transactional Famo installer. build-installer.ps1 supplies all public defines.

#define AppName       "法墨输入法"
#define AppNameEN     "Famo"
#ifndef AppVersion
  #define AppVersion  "1.4.8"
#endif
#ifndef ManifestPrefix
  #define ManifestPrefix "UNSET"
#endif
#ifndef Identity
  #define Identity "Stable"
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
DisableProgramGroupPage=yes
OutputDir=dist
OutputBaseFilename={#AppNameEN}-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
CloseApplications=no
RestartApplications=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile={#StagingDir}\payload\licenses\LICENSE
SetupIconFile={#SetupIconFile}
UninstallDisplayIcon={code:GetActiveSettings}

[Languages]
Name: "zh"; MessagesFile: "compiler:Default.isl"

[Files]
; Every repair extracts a complete payload to a fresh immutable transaction target.
Source: "{#StagingDir}\payload\*"; DestDir: "{code:GetTransactionTarget}"; Flags: recursesubdirs createallsubdirs ignoreversion uninsrestartdelete; Check: ShouldInstallPayload

[Icons]
Name: "{autoprograms}\法墨设置"; Filename: "{code:GetActiveSettings}"; IconFilename: "{code:GetActiveSettings}"; Check: ShouldInstallPayload

[Code]
const
  BrandKey = 'Software\Famo\InputMethod';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunOnceKey = 'Software\Microsoft\Windows\CurrentVersion\RunOnce';
  ResumeValue = 'FamoResumePending';
  StableClsid = '{54EAD76A-B864-4A6D-9C82-148E3352BEE7}';
  StateReady = 'Ready';
  StateRolledBack = 'RolledBack';
  StatePendingReboot = 'PendingReboot';
  StateNotInstalled = 'NotInstalled';

var
  TransactionId: String;
  TransactionTarget: String;
  PreviousTarget: String;
  PreviousManifest: String;
  PreviousDefault: String;
  PreviousState: String;
  PreviousHost: String;
  PreviousServer: String;
  PreviousProfileTool: String;
  PreviousVersion: String;
  PreviousIdentity: String;
  PreviousProfileActive: Boolean;
  LoadedHostDetected: Boolean;
  LoadedHostHash: String;
  LoadedHostVersion: String;
  LoadedHostExpectedHash: String;
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

function EnsureTransactionTarget: String;
begin
  if TransactionId = '' then
    TransactionId := GetDateTimeString('yyyymmddhhnnss', '-', ':') + '-' + IntToStr(Random(1000000));
  if TransactionTarget = '' then
    TransactionTarget := AddBackslash(ExpandConstant('{app}')) +
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

function RunAndRequire(const FileName, Parameters: String; OriginalUser: Boolean): Boolean;
var
  ResultCode: Integer;
begin
  if OriginalUser then
    Result := ExecAsOriginalUser(FileName, Parameters, '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode)
  else
    Result := Exec(FileName, Parameters, '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  Result := Result and (ResultCode = 0);
end;

function RunExitCode(const FileName, Parameters: String): Integer;
begin
  Result := -1;
  if not Exec(FileName, Parameters, '', SW_HIDE, ewWaitUntilTerminated, Result) then
    Result := -1;
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
  Result := RunAndRequire(ProfileTool(Target), 'register', False) and
    RunAndRequire(ProfileTool(Target), 'check', False);
end;

function RegisterTargetDisabled(const Target: String): Boolean;
begin
  Result := RunAndRequire(ProfileTool(Target), 'register-disabled', False) and
    RunAndRequire(ProfileTool(Target), 'check-disabled', False);
end;

function UnregisterTarget(const Target: String): Boolean;
begin
  Result := True;
  if FileExists(ProfileTool(Target)) then
    Result := RunAndRequire(ProfileTool(Target), 'unregister', False);
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
  RegDeleteValue(HKLM64, RunOnceKey, ResumeValue);
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
  RegWriteStringValue(HKLM64, BrandKey, 'ActiveVersion', '{#AppVersion}');
  RegWriteStringValue(HKLM64, BrandKey, 'Identity', '{#Identity}');
  RegWriteStringValue(HKLM64, BrandKey, 'TransactionId', TransactionId);
  RegWriteStringValue(HKLM64, BrandKey, 'PreviousTarget', PreviousTarget);
  RegWriteStringValue(HKLM64, BrandKey, 'PreviousDefault', PreviousDefault);
  RegWriteStringValue(HKLM64, BrandKey, 'InstallState', State);
  RegWriteStringValue(HKLM64, RunKey, 'FamoRuntime', AddBackslash(Target) + 'FamoRuntime.exe');
end;

procedure RestorePreviousRegistry;
begin
  if PreviousTarget <> '' then
  begin
    RegWriteStringValue(HKLM64, BrandKey, 'InstallDir', PreviousTarget);
    WriteOrDelete('ServerExecutable', PreviousServer);
    WriteOrDelete('ProfileTool', PreviousProfileTool);
    WriteOrDelete('ActiveManifest', PreviousManifest);
    WriteOrDelete('ActiveVersion', PreviousVersion);
    WriteOrDelete('Identity', PreviousIdentity);
    RegWriteStringValue(HKLM64, BrandKey, 'TransactionId', TransactionId);
    RegWriteStringValue(HKLM64, BrandKey, 'PreviousTarget', TransactionTarget);
    RegWriteStringValue(HKLM64, BrandKey, 'PreviousDefault', PreviousDefault);
    RegWriteStringValue(HKLM64, BrandKey, 'InstallState', StateRolledBack);
    if PreviousServer <> '' then
      RegWriteStringValue(HKLM64, RunKey, 'FamoRuntime', PreviousServer)
    else
      RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
  end
  else
  begin
    RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
    RegDeleteKeyIncludingSubkeys(HKLM64, BrandKey);
    RegWriteStringValue(HKLM64, BrandKey, 'InstallState', StateRolledBack);
    RegWriteStringValue(HKLM64, BrandKey, 'TransactionId', TransactionId);
    RegWriteStringValue(HKLM64, BrandKey, 'PreviousDefault', PreviousDefault);
  end;
  if PreviousDefault <> '' then
    RegWriteStringValue(HKCU, 'Keyboard Layout\Preload', '1', PreviousDefault);
  ClearPendingRegistry;
end;

function SafeRelativePath(const Value: String): Boolean;
begin
  Result := (Value <> '') and (Pos('..', Value) = 0) and
    (Pos(':', Value) = 0) and (Value[1] <> '\') and (Value[1] <> '/');
end;

function ParseFileEntry(const Line: String; var RelativePath, ExpectedHash: String): Boolean;
var
  Payload, Rest: String;
  FirstBar, SecondBar: Integer;
begin
  Result := False;
  Payload := Copy(Line, 6, Length(Line));
  FirstBar := Pos('|', Payload);
  if FirstBar = 0 then Exit;
  RelativePath := Copy(Payload, 1, FirstBar - 1);
  Rest := Copy(Payload, FirstBar + 1, Length(Payload));
  SecondBar := Pos('|', Rest);
  if SecondBar = 0 then Exit;
  ExpectedHash := Copy(Rest, SecondBar + 1, Length(Rest));
  Result := SafeRelativePath(RelativePath) and (Length(ExpectedHash) = 64);
end;

procedure ReadPreviousHostMetadata;
var
  Lines: TArrayOfString;
  I: Integer;
  RelativePath, ExpectedHash: String;
begin
  LoadedHostHash := '';
  LoadedHostVersion := '';
  LoadedHostExpectedHash := '';
  if (PreviousHost <> '') and FileExists(PreviousHost) then
  begin
    LoadedHostHash := Uppercase(GetSHA256OfFile(PreviousHost));
    if not GetVersionNumbersString(PreviousHost, LoadedHostVersion) then
      LoadedHostVersion := 'unversioned';
  end;
  if (PreviousManifest <> '') and FileExists(PreviousManifest) then
  begin
    if not LoadStringsFromFile(PreviousManifest, Lines) then
      RaiseException('previous payload manifest unreadable');
    for I := 0 to GetArrayLength(Lines) - 1 do
    begin
      if (PreviousVersion = '') and (Pos('version=', Lines[I]) = 1) then
        PreviousVersion := Copy(Lines[I], 9, Length(Lines[I]));
      if (Pos('file=', Lines[I]) = 1) and
         ParseFileEntry(Lines[I], RelativePath, ExpectedHash) and
         (CompareText(RelativePath, 'FamoTextService.dll') = 0) then
        LoadedHostExpectedHash := Uppercase(ExpectedHash);
    end;
    if (LoadedHostExpectedHash = '') or
       (CompareText(LoadedHostHash, LoadedHostExpectedHash) <> 0) then
      RaiseException('previous loaded-host path/hash/version does not match its manifest');
  end;
end;

procedure SnapshotPreviousState;
begin
  RegQueryStringValue(HKLM64, BrandKey, 'InstallDir', PreviousTarget);
  RegQueryStringValue(HKLM64, BrandKey, 'ActiveManifest', PreviousManifest);
  RegQueryStringValue(HKLM64, BrandKey, 'InstallState', PreviousState);
  RegQueryStringValue(HKLM64, BrandKey, 'ServerExecutable', PreviousServer);
  RegQueryStringValue(HKLM64, BrandKey, 'ProfileTool', PreviousProfileTool);
  RegQueryStringValue(HKLM64, BrandKey, 'ActiveVersion', PreviousVersion);
  RegQueryStringValue(HKLM64, BrandKey, 'Identity', PreviousIdentity);
  RegQueryStringValue(HKCU,
    'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '', PreviousHost);
  if (PreviousTarget = '') and (PreviousHost <> '') then
    PreviousTarget := ExtractFileDir(PreviousHost);
  if (PreviousProfileTool = '') and (PreviousTarget <> '') and
     FileExists(ProfileTool(PreviousTarget)) then
    PreviousProfileTool := ProfileTool(PreviousTarget);
  if (PreviousServer = '') and (PreviousTarget <> '') and
     FileExists(AddBackslash(PreviousTarget) + 'FamoRuntime.exe') then
    PreviousServer := AddBackslash(PreviousTarget) + 'FamoRuntime.exe';
  if PreviousIdentity = '' then
    PreviousIdentity := 'LegacyStable';
  RegQueryStringValue(HKCU, 'Keyboard Layout\Preload', '1', PreviousDefault);
  ReadPreviousHostMetadata;
end;

function CountFiles(const Directory: String): Integer;
var
  FindRec: TFindRec;
  Path: String;
begin
  Result := 0;
  if FindFirst(AddBackslash(Directory) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Path := AddBackslash(Directory) + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            Result := Result + CountFiles(Path)
          else
            Result := Result + 1;
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
  Manifest, RelativePath, ExpectedHash, ActualHash: String;
  I, DeclaredCount, EntryCount: Integer;
  HasFormat, HasProduct, HasVersion, HasProtocol, HasArch, HasIdentity: Boolean;
begin
  Manifest := AddBackslash(EnsureTransactionTarget) + 'payload-manifest.txt';
  if not FileExists(Manifest) then RaiseException('payload manifest missing');
  ActualHash := Uppercase(GetSHA256OfFile(Manifest));
  if CompareText(Copy(ActualHash, 1, 12), '{#ManifestPrefix}') <> 0 then
    RaiseException('payload manifest identity mismatch');
  if not LoadStringsFromFile(Manifest, Lines) then RaiseException('payload manifest unreadable');

  DeclaredCount := -1;
  EntryCount := 0;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    HasFormat := HasFormat or (Lines[I] = 'format=1');
    HasProduct := HasProduct or (Lines[I] = 'product=Famo');
    HasVersion := HasVersion or (Lines[I] = 'version={#AppVersion}');
    HasProtocol := HasProtocol or (Lines[I] = 'protocol=1');
    HasArch := HasArch or (Lines[I] = 'architecture=x64');
    HasIdentity := HasIdentity or (Lines[I] = 'identity={#Identity}');
    if Pos('file_count=', Lines[I]) = 1 then
      DeclaredCount := StrToInt(Copy(Lines[I], 12, Length(Lines[I])));
    if Pos('file=', Lines[I]) = 1 then
    begin
      if not ParseFileEntry(Lines[I], RelativePath, ExpectedHash) then
        RaiseException('invalid payload manifest entry');
      ActualHash := Uppercase(GetSHA256OfFile(AddBackslash(EnsureTransactionTarget) + RelativePath));
      if CompareText(ActualHash, ExpectedHash) <> 0 then
        RaiseException('payload hash mismatch: ' + RelativePath);
      EntryCount := EntryCount + 1;
    end;
  end;
  if not (HasFormat and HasProduct and HasVersion and HasProtocol and HasArch and HasIdentity) then
    RaiseException('payload manifest header mismatch');
  if (DeclaredCount <> EntryCount) or (CountFiles(EnsureTransactionTarget) <> EntryCount + 1) then
    RaiseException('payload file_count mismatch');
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
  if (PreviousProfileTool <> '') and FileExists(PreviousProfileTool) then
    Result := RunAndRequire(PreviousProfileTool, 'unregister', False)
  else
    Result := RunRegSvr32(PreviousHost, True);
end;

function RegisterPreviousRegistration: Boolean;
begin
  if PreviousHost = '' then
  begin
    Result := True;
    Exit;
  end;
  if (PreviousProfileTool <> '') and FileExists(PreviousProfileTool) then
    Result := RunAndRequire(PreviousProfileTool, 'register', False)
  else
    Result := RunRegSvr32(PreviousHost, False);
  Result := Result and
    RunAndRequire(PreviousRegistrationTool, 'check', False);
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

procedure PrepareTransaction;
begin
  EnsureTransactionTarget;
  if ResumeMode or RollbackMode then
  begin
    if not DirExists(TransactionTarget) then
      RaiseException('pending transaction target is missing');
    TransactionPrepared := True;
    Exit;
  end;
  if DirExists(TransactionTarget) then RaiseException('transaction target already exists');
  SnapshotPreviousState;
  RegWriteStringValue(HKLM64, BrandKey, 'TransactionId', TransactionId);
  RegWriteStringValue(HKLM64, BrandKey, 'PreviousTarget', PreviousTarget);
  RegWriteStringValue(HKLM64, BrandKey, 'PreviousDefault', PreviousDefault);
  WriteOrDelete('PreviousHost', PreviousHost);
  WriteOrDelete('PreviousServer', PreviousServer);
  WriteOrDelete('PreviousProfileTool', PreviousProfileTool);
  WriteOrDelete('PreviousVersion', PreviousVersion);
  WriteOrDelete('PreviousIdentity', PreviousIdentity);
  RegWriteStringValue(HKLM64, BrandKey, 'InstallState', 'Installing');
  if (PreviousServer <> '') and FileExists(PreviousServer) then
  begin
    if not RunAndRequire(PreviousServer, '--control shutdown', True) then
      RunAndRequire(PreviousServer, '/quit', True);
    Sleep(750);
  end;
  TransactionPrepared := True;
  FailIfRequested('after-prepare');
end;

procedure SwitchRegistration;
begin
  if PreviousHost <> '' then
  begin
    if not RunAndRequire(ProfileTool(TransactionTarget), 'switch-away', False) then
      RaiseException('previous profile switch-away failed');
    if not UnregisterPreviousRegistration then
      RaiseException('previous profile unregister failed');
  end;
  if not RegisterTarget(TransactionTarget) then RaiseException('new profile registration failed');
  RegistrationSwitched := True;
  WriteActiveRegistry(TransactionTarget, 'Activating');
end;

function ResumeInstallerPath: String;
begin
  Result := AddBackslash(ExpandConstant('{app}')) + 'pending\Famo-Resume-' +
    TransactionId + '.exe';
end;

procedure SchedulePendingResume;
var
  Source, Destination, Command: String;
begin
  Destination := ResumeInstallerPath;
  if not ForceDirectories(ExtractFileDir(Destination)) then
    RaiseException('cannot create pending resume directory');
  Source := ExpandConstant('{srcexe}');
  if (CompareText(Source, Destination) <> 0) and
     not CopyFile(Source, Destination, False) then
    RaiseException('cannot retain pending resume installer');
  Command := AddQuotes(Destination) + ' /FamoResume=' + TransactionId +
    ' /VERYSILENT /SUPPRESSMSGBOXES /NORESTART';
  if not RegWriteStringValue(HKLM64, RunOnceKey, ResumeValue, Command) then
    RaiseException('cannot schedule pending reboot continuation');
  RegWriteStringValue(HKLM64, BrandKey, 'ResumeInstaller', Destination);
end;

procedure WritePendingRegistry;
begin
  WriteOrDelete('InstallDir', PreviousTarget);
  WriteOrDelete('ServerExecutable', PreviousServer);
  WriteOrDelete('ProfileTool', PreviousProfileTool);
  WriteOrDelete('ActiveManifest', PreviousManifest);
  WriteOrDelete('ActiveVersion', PreviousVersion);
  RegWriteStringValue(HKLM64, BrandKey, 'Identity', '{#Identity}');
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
  if not RegQueryStringValue(HKCU,
    'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '', RegisteredDll) or
    (CompareText(RegisteredDll,
      AddBackslash(TransactionTarget) + 'FamoTextService.dll') <> 0) then
    RaiseException('pending COM registration target mismatch');
  if not RunAndRequire(ProfileTool(TransactionTarget), 'check-disabled', False) then
    RaiseException('pending profile is not registered disabled');
  if RunExitCode(ProfileTool(TransactionTarget), 'is-active') <> 1 then
    RaiseException('pending profile remains active');
  if RegQueryStringValue(HKLM64, RunKey, 'FamoRuntime', RunValue) then
    RaiseException('pending runtime Run entry must be absent');
end;

procedure EnterPendingReboot;
begin
  if PreviousHost <> '' then
  begin
    if not RunAndRequire(ProfileTool(TransactionTarget), 'switch-away', False) then
      RaiseException('cannot switch away before pending reboot');
    if not RunAndRequire(AddBackslash(TransactionTarget) +
      'settings\FamoSettings.exe', '--remove-input-tip', True) then
      RaiseException('cannot remove current-user input tip before pending reboot');
    if not UnregisterPreviousRegistration then
      RaiseException('cannot unregister previous host before pending reboot');
  end;
  if not RegisterTargetDisabled(TransactionTarget) then
    RaiseException('cannot register pending profile disabled');
  RegistrationSwitched := True;
  RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
  VerifyPendingInstall;
  FailIfRequested('after-pending-registration');
  WritePendingRegistry;
  SchedulePendingResume;
  FailIfRequested('after-pending-state');
  PendingTerminal := True;
  InstallReady := True;
end;

procedure StartRuntimeAsOriginalUser;
var
  ResultCode: Integer;
begin
  if not ExecAsOriginalUser(AddBackslash(TransactionTarget) + 'FamoRuntime.exe',
    '', '', SW_HIDE, ewNoWait, ResultCode) then
    RaiseException('runtime start failed');
  RuntimeStarted := True;
  Sleep(750);
end;

procedure InstallUserState;
var
  SeedArguments: String;
begin
  SeedArguments := '--seed-only';
  if (PreviousHost <> '') and not PreviousProfileActive then
    SeedArguments := SeedArguments + ' --no-activate';
  if not RunAndRequire(AddBackslash(TransactionTarget) + 'settings\FamoSettings.exe',
    SeedArguments, True) then RaiseException('user seed failed');
  StartRuntimeAsOriginalUser;
  if not RunAndRequire(AddBackslash(TransactionTarget) + 'FamoRuntime.exe',
    '--control deploy', True) then RaiseException('runtime deploy failed');
end;

procedure RollbackTransaction;
var
  ResultCode: Integer;
  RestoreSettings: String;
begin
  if RollbackComplete then Exit;
  if RuntimeStarted then
  begin
    RunAndRequire(AddBackslash(TransactionTarget) + 'FamoRuntime.exe',
      '--control shutdown', True);
    Sleep(500);
    RuntimeStarted := False;
  end;
  if RegistrationSwitched then UnregisterTarget(TransactionTarget);
  RestorePreviousRegistry;
  if PreviousHost <> '' then
  begin
    if not RegisterPreviousRegistration then
      RaiseException('previous profile rollback failed');
    RestoreSettings := AddBackslash(PreviousTarget) +
      'settings\FamoSettings.exe';
    if not FileExists(RestoreSettings) then
      RestoreSettings := AddBackslash(TransactionTarget) +
        'settings\FamoSettings.exe';
    if not RunAndRequire(RestoreSettings, '--add-input-tip', True) then
      RaiseException('previous input tip rollback failed');
    if PreviousProfileActive and
      not RunAndRequire(PreviousRegistrationTool, 'activate', True) then
      Log('previous profile activation deferred; available via Win+Space');
    if (PreviousServer <> '') and FileExists(PreviousServer) then
    begin
      if not ExecAsOriginalUser(PreviousServer, '', '', SW_HIDE,
        ewNoWait, ResultCode) then
        RaiseException('previous runtime rollback failed');
      Sleep(750);
      if (PreviousManifest <> '') and
         not RunAndRequire(PreviousServer, '--control reload-options', True) then
        RaiseException('previous runtime health readback failed');
    end;
  end;
  if DirExists(TransactionTarget) then DelTree(TransactionTarget, True, True, True);
  RollbackComplete := True;
end;

procedure VerifyActiveInstall;
var
  RegisteredDll: String;
begin
  VerifyPayloadOrFail;
  if not RegQueryStringValue(HKCU,
    'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '', RegisteredDll) then
    RaiseException('active COM registration missing');
  if CompareText(RegisteredDll, AddBackslash(TransactionTarget) + 'FamoTextService.dll') <> 0 then
    RaiseException('active COM registration target mismatch');
  if not RunAndRequire(ProfileTool(TransactionTarget), 'check', False) then
    RaiseException('profile health readback failed');
end;

procedure CompletePendingTransaction;
begin
  if not RunAndRequire(ProfileTool(TransactionTarget), 'enable', False) then
    RaiseException('pending profile enable failed');
  RegistrationSwitched := True;
  WriteActiveRegistry(TransactionTarget, 'Activating');
  FailIfRequested('after-resume-registration');
  InstallUserState;
  FailIfRequested('after-resume-user-state');
  VerifyActiveInstall;
  WriteActiveRegistry(TransactionTarget, StateReady);
  ClearPendingRegistry;
  InstallReady := True;
end;

function LoadPendingState(const ExpectedId: String): Boolean;
var
  StoredId, State, ActiveText, PendingManifest: String;
begin
  Result := RegQueryStringValue(HKLM64, BrandKey, 'InstallState', State) and
    (CompareText(State, StatePendingReboot) = 0) and
    RegQueryStringValue(HKLM64, BrandKey, 'TransactionId', StoredId) and
    (CompareText(StoredId, ExpectedId) = 0) and
    RegQueryStringValue(HKLM64, BrandKey, 'PendingTarget', TransactionTarget) and
    RegQueryStringValue(HKLM64, BrandKey, 'PendingManifest', PendingManifest);
  if not Result then Exit;
  TransactionId := StoredId;
  if CompareText(PendingManifest,
    AddBackslash(TransactionTarget) + 'payload-manifest.txt') <> 0 then
  begin
    Result := False;
    Exit;
  end;
  RegQueryStringValue(HKLM64, BrandKey, 'PreviousTarget', PreviousTarget);
  RegQueryStringValue(HKLM64, BrandKey, 'PreviousDefault', PreviousDefault);
  RegQueryStringValue(HKLM64, BrandKey, 'PreviousHost', PreviousHost);
  RegQueryStringValue(HKLM64, BrandKey, 'PreviousManifest', PreviousManifest);
  RegQueryStringValue(HKLM64, BrandKey, 'PreviousServer', PreviousServer);
  RegQueryStringValue(HKLM64, BrandKey, 'PreviousProfileTool', PreviousProfileTool);
  RegQueryStringValue(HKLM64, BrandKey, 'PreviousVersion', PreviousVersion);
  RegQueryStringValue(HKLM64, BrandKey, 'PreviousIdentity', PreviousIdentity);
  RegQueryStringValue(HKLM64, BrandKey, 'PreviousProfileActive', ActiveText);
  PreviousProfileActive := ActiveText = '1';
  RegQueryStringValue(HKLM64, BrandKey, 'LoadedHostHash', LoadedHostHash);
  RegQueryStringValue(HKLM64, BrandKey, 'LoadedHostVersion', LoadedHostVersion);
  RegQueryStringValue(HKLM64, BrandKey, 'LoadedHostExpectedHash',
    LoadedHostExpectedHash);
  RegistrationSwitched := True;
end;

function InitializeSetup: Boolean;
var
  ResumeId, RollbackId, ExistingState, ExistingId, ResumeInstaller: String;
begin
  ResumeId := ExpandConstant('{param:FamoResume|}');
  RollbackId := ExpandConstant('{param:FamoRollback|}');
  if (ResumeId <> '') and (RollbackId <> '') then
  begin
    Result := False;
    Exit;
  end;
  if ResumeId <> '' then
  begin
    ResumeMode := LoadPendingState(ResumeId);
    Result := ResumeMode;
    Exit;
  end;
  if RollbackId <> '' then
  begin
    RollbackMode := LoadPendingState(RollbackId);
    Result := RollbackMode;
    Exit;
  end;
  if RegQueryStringValue(HKLM64, BrandKey, 'InstallState', ExistingState) and
     (CompareText(ExistingState, StatePendingReboot) = 0) then
  begin
    RegQueryStringValue(HKLM64, BrandKey, 'TransactionId', ExistingId);
    RegQueryStringValue(HKLM64, BrandKey, 'ResumeInstaller', ResumeInstaller);
    Log('pending transaction must be resumed or rolled back: ' + ExistingId);
    if not WizardSilent then
      MsgBox('法墨升级正在等待重启完成。请重启 Windows；如需回滚，以管理员运行：' +
        Chr(13) + Chr(10) + ResumeInstaller + ' /FamoRollback=' + ExistingId,
        mbInformation, MB_OK);
    Result := False;
    Exit;
  end;
  Result := True;
end;

procedure ReturnToPendingAfterResumeFailure(const Reason: String);
begin
  if RuntimeStarted then
  begin
    RunAndRequire(AddBackslash(TransactionTarget) + 'FamoRuntime.exe',
      '--control shutdown', True);
    Sleep(500);
    RuntimeStarted := False;
  end;
  RunAndRequire(ProfileTool(TransactionTarget), 'switch-away', False);
  RunAndRequire(AddBackslash(TransactionTarget) +
    'settings\FamoSettings.exe', '--remove-input-tip', True);
  RegisterTargetDisabled(TransactionTarget);
  RegistrationSwitched := True;
  WritePendingRegistry;
  RegWriteStringValue(HKLM64, BrandKey, 'PendingReason',
    'post-reboot verification failed: ' + Reason);
  RegDeleteValue(HKLM64, RunOnceKey, ResumeValue);
  PendingTerminal := True;
  InstallReady := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then PrepareTransaction;
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
      FailIfRequested('after-verify');
      LoadedHostDetected := DetectLoadedPreviousHost;
      if ResumeMode and LoadedHostDetected then
      begin
        VerifyPendingInstall;
        WritePendingRegistry;
        SchedulePendingResume;
        PendingTerminal := True;
        InstallReady := True;
      end
      else if ResumeMode then
        CompletePendingTransaction
      else
      begin
        PreviousProfileActive :=
          RunExitCode(ProfileTool(TransactionTarget), 'is-active') = 0;
        if LoadedHostDetected then
          EnterPendingReboot
        else
        begin
          SwitchRegistration;
          FailIfRequested('after-switch');
          InstallUserState;
          FailIfRequested('after-user-state');
          VerifyActiveInstall;
          FailIfRequested('after-active-verify');
          WriteActiveRegistry(TransactionTarget, StateReady);
          ClearPendingRegistry;
          InstallReady := True;
        end;
      end;
    except
      if ResumeMode then
        ReturnToPendingAfterResumeFailure(GetExceptionMessage)
      else
        RollbackTransaction;
      RaiseException(GetExceptionMessage);
    end;
  end;
end;

function NeedRestart: Boolean;
begin
  Result := PendingTerminal;
end;

procedure DeinitializeSetup;
begin
  if TransactionPrepared and not InstallReady then RollbackTransaction;
  if ResumeMode and InstallReady and not PendingTerminal then
    DelayDeleteFile(ExpandConstant('{srcexe}'), 5);
end;

function InitializeUninstall: Boolean;
begin
  DeleteUserData := False;
  if not UninstallSilent then
    DeleteUserData := MsgBox('是否同时删除 %LOCALAPPDATA%\Famo 中的用户词库和设置？',
      mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
  Result := True;
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

function UninstallNeedRestart: Boolean;
begin
  Result := UninstallRestartPending;
end;

procedure RemoveActiveInstall;
var
  ActiveTarget, Runtime, Settings, RegisteredDll, State, PendingId: String;
begin
  if RegQueryStringValue(HKLM64, BrandKey, 'InstallState', State) and
     (CompareText(State, StatePendingReboot) = 0) and
     RegQueryStringValue(HKLM64, BrandKey, 'TransactionId', PendingId) and
     LoadPendingState(PendingId) then
    ActiveTarget := TransactionTarget
  else
    ActiveTarget := ReadActiveTarget;
  if ActiveTarget = '' then Exit;
  Runtime := AddBackslash(ActiveTarget) + 'FamoRuntime.exe';
  Settings := AddBackslash(ActiveTarget) + 'settings\FamoSettings.exe';
  RunAndRequire(Runtime, '--control shutdown', False);
  Sleep(750);
  if not RunAndRequire(ProfileTool(ActiveTarget), 'switch-away', False) then
    RaiseException('cannot switch away from Famo before uninstall');
  if not RunAndRequire(Settings, '--remove-input-tip', False) then
    RaiseException('cannot remove current-user input tip');
  if not UnregisterTarget(ActiveTarget) then
    RaiseException('cannot unregister Famo profile');
  RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
  if RegQueryStringValue(HKCU,
    'Software\Classes\CLSID\' + StableClsid + '\InprocServer32', '', RegisteredDll) then
    RaiseException('dangling COM override after unregister');
  UninstallPrepared := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  VersionsRoot: String;
begin
  if (CurUninstallStep = usUninstall) and not UninstallPrepared then
    RemoveActiveInstall;
  if CurUninstallStep = usPostUninstall then
  begin
    RegDeleteValue(HKLM64, RunKey, 'FamoRuntime');
    RegDeleteValue(HKLM64, RunOnceKey, ResumeValue);
    RegDeleteKeyIncludingSubkeys(HKLM64, BrandKey);
    VersionsRoot := ExpandConstant('{app}\versions');
    if DirExists(VersionsRoot) and not DelTree(VersionsRoot, True, True, True) and
       not ScheduleLoadedHostResidueForRestart(VersionsRoot) then
      RaiseException('cannot schedule transaction version cleanup');
    if DirExists(ExpandConstant('{app}\pending')) then
      DelTree(ExpandConstant('{app}\pending'), True, True, True);
    if DeleteUserData and DirExists(ExpandConstant('{localappdata}\Famo')) then
      DelTree(ExpandConstant('{localappdata}\Famo'), True, True, True);
  end;
end;
