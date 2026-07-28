using Xunit;

namespace Famo.Settings.Tests;

public sealed class InstallerContractTests
{
    [Fact]
    public void BuildInstaller_ConsumesOneStableNativeOutput()
    {
        string script = InstallerText("build-installer.ps1");

        Assert.Contains("$NativeOutput", script);
        Assert.Contains("Join-Path $NativeOutput $Configuration", script);
        Assert.Contains("FAMO_IDENTITY:STRING=Stable", script);
        foreach (string file in new[] { "FamoTextService.dll", "FamoRuntime.exe", "FamoRimeEngine.dll", "FamoProfileTool.exe", "rime.dll" })
        {
            Assert.Contains(file, script);
        }

        foreach (string forbidden in new[] { "$WeaselOutput", "$EngineDll", "weaselx64.dll", "WeaselServer.exe", "WeaselDeployer.exe", "FamoDeploy.exe", "WinSparkle.dll" })
        {
            Assert.DoesNotContain(forbidden, script, StringComparison.OrdinalIgnoreCase);
        }
    }

    [Fact]
    public void BuildInstaller_EmitsAndChecksOnePayloadManifest()
    {
        string script = InstallerText("build-installer.ps1");

        Assert.Contains("payload-manifest.txt", script);
        Assert.Contains("format=1", script);
        Assert.Contains("protocol=1", script);
        Assert.Contains("architecture=x64", script);
        Assert.Contains("identity=Stable", script);
        Assert.Contains("file_count=", script);
        Assert.Contains("Get-FileHash", script);
        Assert.Contains("Get-AuthenticodeSignature", script);
        Assert.Contains("AllowUnsignedDevelopment", script);
        Assert.Contains("Test-PayloadManifest", script);
        Assert.Contains("/DManifestPrefix=", script);
    }

    [Fact]
    public void InnoSetup_QuotesEveryMachineRuntimeRunCommand()
    {
        string iss = InstallerText("famo-setup.iss");
        int writeActive = Position(iss, "procedure WriteActiveRegistry");
        string writeActiveBody = iss[writeActive..Position(iss, "procedure RestorePreviousRegistry", writeActive)];
        int restore = Position(iss, "procedure RestorePreviousRegistry");
        string restoreBody = iss[restore..Position(iss, "function NormalizeDirectoryPath", restore)];

        Assert.Contains(
            "RegWriteStringValue(HKLM64, RunKey, 'FamoRuntime'," +
            " AddQuotes(AddBackslash(Target) + 'FamoRuntime.exe'))",
            writeActiveBody);
        Assert.Contains(
            "RegWriteStringValue(HKLM64, RunKey, 'FamoRuntime', AddQuotes(PreviousServer))",
            restoreBody);
        Assert.DoesNotContain(
            "RegWriteStringValue(HKLM64, RunKey, 'FamoRuntime', PreviousServer)",
            restoreBody);
    }

    [Fact]
    public void InnoSetup_RejectsDuplicateManifestPathsAndNonHexHashes()
    {
        string iss = InstallerText("famo-setup.iss");
        int segmentValidator = Position(iss, "function ValidManifestPathSegment");
        int pathValidator = Position(iss, "function NormalizeSafeRelativePath", segmentValidator);
        int hashValidator = Position(iss, "function IsSha256Hex", pathValidator);
        int parseEntry = Position(iss, "function ParseFileEntry", hashValidator);
        string parseBody = iss[segmentValidator..Position(iss, "procedure ReadPreviousHostMetadata", parseEntry)];
        int verify = Position(iss, "procedure VerifyPayloadOrFail");
        string verifyBody = iss[verify..Position(iss, "function RunRegSvr32", verify)];

        // One accepted spelling per Windows path: no slash aliases, empty or
        // dot segments, nor Win32-trimmed trailing dots/spaces.
        Assert.Contains("NormalizedValue := PathNormalizeSlashes(Value)", parseBody);
        Assert.Contains("if NormalizedValue <> Value then Exit", parseBody);
        Assert.Contains("(Segment = '.') or (Segment = '..')", parseBody);
        Assert.Contains("Result := (Segment[Length(Segment)] <> '.') and", parseBody);
        Assert.Contains("(Segment[Length(Segment)] <> ' ')", parseBody);
        Assert.Contains("NormalizeSafeRelativePath(RawRelativePath, RelativePath)", parseBody);

        Assert.Contains("Length(Value) <> 64", parseBody);
        Assert.Contains("(Value[I] >= '0') and (Value[I] <= '9')", parseBody);
        Assert.Contains("(Value[I] >= 'A') and (Value[I] <= 'F')", parseBody);
        Assert.Contains("(Value[I] >= 'a') and (Value[I] <= 'f')", parseBody);
        Assert.Contains("Result := IsSha256Hex(ExpectedHash)", parseBody);

        Assert.Contains("SeenPaths := TStringList.Create", verifyBody);
        Assert.Contains("SeenPaths.CaseSensitive := False", verifyBody);
        Assert.Contains("TransactionRoot := NormalizeDirectoryPath(EnsureTransactionTarget)", verifyBody);
        Assert.Contains("FullPath := ExpandFileName(PathCombine(TransactionRoot, RelativePath))", verifyBody);
        Assert.Contains("PathStartsWith(FullPath, AddBackslash(TransactionRoot), True)", verifyBody);
        Assert.Contains("SeenPaths.IndexOf(FullPath) >= 0", verifyBody);
        Assert.Contains("RaiseException('duplicate payload manifest path: ' + RelativePath)", verifyBody);
        Assert.Contains("SeenPaths.Add(FullPath)", verifyBody);
        Assert.Contains("SeenPaths.Free", verifyBody);
    }

    [Fact]
    public void InnoSetup_MatchesManifestToCanonicalActualFileSet()
    {
        string iss = InstallerText("famo-setup.iss");
        int pathLookup = Position(iss, "function FindPathInList");
        int enumerate = Position(iss, "procedure VerifyActualPayloadFiles", pathLookup);
        int verify = Position(iss, "procedure VerifyPayloadOrFail", enumerate);
        string enumerationHelpers = iss[pathLookup..verify];
        string verifyBody = iss[verify..Position(iss, "function RunRegSvr32", verify)];

        Assert.Contains("(FindRec.Attributes and FileAttributeReparsePoint) <> 0", enumerationHelpers);
        Assert.Contains("TryGetFinalObjectInfo(Path, FinalPath, ObjectId)", enumerationHelpers);
        Assert.Contains("PathStartsWith(FinalPath, AddBackslash(FinalRoot), True)", enumerationHelpers);
        Assert.Contains("FindPathInList(ManifestFinalPaths, FinalPath) < 0", enumerationHelpers);
        Assert.Contains("FindPathInList(SeenActualPaths, FinalPath) >= 0", enumerationHelpers);
        Assert.Contains("SeenActualObjectIds.IndexOf(ObjectId) >= 0", enumerationHelpers);

        Assert.Contains("TryGetFinalObjectInfo(FullPath, FinalPath, ObjectId)", verifyBody);
        Assert.Contains("FindPathInList(SeenFinalPaths, FinalPath) >= 0", verifyBody);
        Assert.Contains("SeenObjectIds.IndexOf(ObjectId) >= 0", verifyBody);
        Assert.Contains("VerifyActualPayloadFiles(TransactionRoot, FinalRoot,", verifyBody);
        Assert.Contains("ActualCount <> EntryCount", verifyBody);
        Assert.DoesNotContain("CountFiles(", verifyBody);
    }

    [Fact]
    public void BuildInstaller_CopiesRequiredWinuiResourcesAndPropagatesCompilerFailure()
    {
        string script = InstallerText("build-installer.ps1");

        foreach (string resource in new[] { "FamoSettings.pri", "App.xbf", "MainWindow.xbf", "Theming" })
        {
            Assert.Contains(resource, script);
        }
        Assert.Contains("-p:Version=$AppVersion", script);
        Assert.Contains("-p:InformationalVersion=$AppVersion", script);
        Assert.Contains("if ($LASTEXITCODE -ne 0) { throw 'ISCC 编译失败。' }", script);
    }

    [Fact]
    public void BuildInstaller_StagesSpdxWithoutExternalReleaseMatrix()
    {
        string script = InstallerText("build-installer.ps1");

        Assert.Contains("SBOM.spdx.json", script);
        Assert.DoesNotContain("Test-FamoReleaseGate.ps1", script);
        Assert.DoesNotContain("Windows 10", script, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("Windows 11", script, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void InnoSetup_InstallsAnImmutableTransactionTarget()
    {
        string iss = InstallerText("famo-setup.iss");

        Assert.Contains(@"Source: ""{#StagingDir}\payload\*""; DestDir: ""{code:GetTransactionTarget}""", iss);
        Assert.Contains(@"versions\{#AppVersion}-{#ManifestPrefix}-", iss);
        Assert.Contains("GetDateTimeString('yyyymmddhhnnss', '-', ':')", iss);
        Assert.DoesNotContain("GetDateTimeString('yyyymmddhhnnss', '', '')", iss);
        Assert.DoesNotContain("restartreplace", iss[..Position(iss, "function InitializeUninstall")], StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain(@"DestDir: ""{app}""", EffectiveInnoContent(iss), StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void InnoSetup_ValidatesPendingTargetBeforeRollbackOrResume()
    {
        string iss = InstallerText("famo-setup.iss");
        int normalize = Position(iss, "function NormalizeDirectoryPath");
        int validateTarget = Position(iss, "function ValidateTransactionTarget", normalize);
        int validatePending = Position(iss, "function ValidatePendingTransaction", validateTarget);
        int load = Position(iss, "function LoadPendingState", validatePending);
        string validationHelpers = iss[normalize..load];
        string loadBody = iss[load..Position(iss, "function InitializeSetup", load)];

        // Canonicalization plus exact-parent equality rejects the versions root,
        // other drives, and grandchildren instead of relying on a string prefix.
        Assert.Contains("RemoveBackslashUnlessRoot(ExpandFileName(PathNormalizeSlashes(Path)))", validationHelpers);
        Assert.Contains(@"Pos('\..\', '\' + PathNormalizeSlashes(Path) + '\') > 0", validationHelpers);
        Assert.Contains("not PathIsRooted(Target)", validationHelpers);
        Assert.Contains("ContainsParentTraversal(Target)", validationHelpers);
        Assert.Contains("NormalizedTarget := NormalizeDirectoryPath(Target)", validationHelpers);
        Assert.Contains("if PathSame(NormalizedTarget, VersionsRoot) or", validationHelpers);
        Assert.Contains("PathSame(ExtractFileDir(NormalizedTarget), VersionsRoot)", validationHelpers);
        Assert.Contains("'{#AppVersion}-{#ManifestPrefix}-' + ExpectedId", validationHelpers);
        Assert.Contains("CompareText(ExtractFileName(NormalizedTarget), ExpectedLeaf)", validationHelpers);
        Assert.Contains("ActiveTarget := ReadActiveTarget", validationHelpers);
        Assert.Contains("ProtectedPathIsDifferent(NormalizedTarget, TargetExists,", validationHelpers);
        Assert.Contains("TargetFinalPath, TargetObjectId, ActiveTarget", validationHelpers);
        Assert.Contains("TargetFinalPath, TargetObjectId, ProtectedPreviousTarget", validationHelpers);

        // Existing junctions/symlinks are rejected. A genuinely absent fresh
        // transaction target remains valid so Setup can create it.
        Assert.Contains("GetFileAttributesW", iss);
        Assert.Contains("FileAttributeReparsePoint", validationHelpers);
        Assert.Contains("InvalidFileAttributes", validationHelpers);
        Assert.Contains("ErrorFileNotFound", validationHelpers);
        Assert.Contains("ErrorPathNotFound", validationHelpers);
        Assert.Contains("Attributes and FileAttributeReparsePoint", validationHelpers);
        Assert.Contains("PathIsNonReparseOrMissing(VersionsRoot)", validationHelpers);
        Assert.Contains("PathIsNonReparseOrMissing(NormalizedTarget)", validationHelpers);

        int prepare = Position(iss, "procedure PrepareTransaction");
        string prepareBody = iss[prepare..Position(iss, "procedure SwitchRegistration", prepare)];
        int prepareValidation = Position(prepareBody,
            "ValidateTransactionTarget(TransactionTarget, TransactionId,");
        int freshTargetCheck = Position(prepareBody, "if DirExists(TransactionTarget)", prepareValidation);
        Assert.True(prepareValidation < freshTargetCheck);
        Assert.Contains("TransactionTarget := ValidatedTarget;", prepareBody);

        Assert.Contains("CompareText(PendingManifest,", validationHelpers);
        Assert.Contains("AddBackslash(NormalizedTarget) + 'payload-manifest.txt'", validationHelpers);
        Assert.Contains("if not Result then NormalizedTarget := '';", validationHelpers);

        // An untrusted registry value must not reach the global deletion target
        // until the shared resume/rollback loader has validated it.
        int validateCall = Position(loadBody,
            "ValidatePendingTransaction(PendingTarget, PendingManifest, StoredId,");
        Assert.Contains("PendingPreviousTarget, NormalizedTarget", loadBody);
        int targetAssignment = Position(loadBody, "TransactionTarget := NormalizedTarget;", validateCall);
        Assert.True(validateCall < targetAssignment);
        Assert.DoesNotContain(
            "RegQueryStringValue(HKLM64, BrandKey, 'PendingTarget', TransactionTarget)",
            loadBody);

        // Re-check the mutable filesystem boundary immediately before recursive
        // deletion instead of trusting only the earlier pending-state load.
        int rollback = Position(iss, "procedure RollbackTransaction");
        string rollbackBody = iss[rollback..Position(iss, "procedure VerifyActiveInstall", rollback)];
        int deleteValidation = Position(rollbackBody,
            "ValidateTransactionTarget(TransactionTarget, TransactionId,");
        Assert.Contains("PreviousTarget, ValidatedTarget", rollbackBody);
        int deleteTree = Position(
            rollbackBody,
            "if not DelTree(ValidatedTarget, True, True, True) then",
            deleteValidation);
        int deleteFailure = Position(
            rollbackBody,
            "RaiseException('transaction target deletion failed during rollback')",
            deleteTree);
        int rollbackComplete = Position(
            rollbackBody,
            "RollbackComplete := True;",
            deleteFailure);
        Assert.True(deleteValidation < deleteTree);
        Assert.True(deleteTree < deleteFailure);
        Assert.True(deleteFailure < rollbackComplete);
        Assert.DoesNotContain("DelTree(TransactionTarget", rollbackBody);
    }

    [Fact]
    public void InnoSetup_ProtectsExistingTargetsByFinalObjectIdentity()
    {
        string iss = InstallerText("famo-setup.iss");
        int finalObject = Position(iss, "function NormalizeFinalObjectPath");
        int validateTarget = Position(iss, "function ValidateTransactionTarget", finalObject);
        string finalObjectHelpers = iss[finalObject..validateTarget];
        string validateTargetBody = iss[validateTarget..Position(iss, "procedure PrepareTransaction", validateTarget)];

        Assert.Contains("CreateFileW", iss);
        Assert.Contains("GetFinalPathNameByHandleW", iss);
        Assert.Contains("GetFileInformationByHandle", iss);
        Assert.Contains("CloseHandle", iss);
        Assert.Contains("FileFlagBackupSemantics", finalObjectHelpers);
        Assert.Contains(@"PathStartsWith(Result, '\\?\UNC\', True)", finalObjectHelpers);
        Assert.Contains(@"Result := '\\' + Copy(Result, 9, Length(Result))", finalObjectHelpers);
        Assert.Contains(@"PathStartsWith(Result, '\\?\', True)", finalObjectHelpers);
        Assert.Contains("PathSame(FirstFinalPath, SecondFinalPath)", finalObjectHelpers);
        Assert.Contains("FirstObjectId <> ''", finalObjectHelpers);
        Assert.Contains("CompareText(FirstObjectId, SecondObjectId) = 0", finalObjectHelpers);

        Assert.Contains("ProtectedPathIsDifferent(NormalizedTarget, TargetExists,", validateTargetBody);
        Assert.Contains("ActiveTarget", validateTargetBody);
        Assert.Contains("ProtectedPreviousTarget", validateTargetBody);
        Assert.Contains("PathSame(ExtractFileDir(TargetFinalPath), VersionsFinalPath)", validateTargetBody);
    }

    [Fact]
    public void InnoSetup_VerifiesManifestBeforeRegistrationSwitch()
    {
        string iss = InstallerText("famo-setup.iss");

        int verify = Position(iss, "procedure VerifyPayloadOrFail");
        int switchRegistration = Position(iss, "procedure SwitchRegistration");
        int postInstall = Position(iss, "VerifyPayloadOrFail;", Position(iss, "ssPostInstall"));
        int switchCall = Position(iss, "SwitchRegistration;", postInstall);

        Assert.Contains("GetSHA256OfFile", iss);
        Assert.Contains("payload-manifest.txt", iss);
        Assert.Contains("file_count", iss);
        Assert.True(verify < switchRegistration);
        Assert.True(postInstall < switchCall);
    }

    [Fact]
    public void InnoSetup_RecordsOnlyTheFourTerminalStates()
    {
        string iss = InstallerText("famo-setup.iss");

        foreach (string state in new[] { "Ready", "RolledBack", "PendingReboot", "NotInstalled" })
        {
            Assert.Contains(state, iss);
        }
        foreach (string value in new[] { "TransactionId", "PreviousTarget", "PreviousDefault", "ActiveManifest", "InstallState" })
        {
            Assert.Contains(value, iss);
        }
    }

    [Fact]
    public void InnoSetup_UsesNativeRuntimeControlAndOriginalUserSeed()
    {
        string iss = InstallerText("famo-setup.iss");

        int seed = Position(iss, "--seed-only");
        int start = Position(iss, "StartRuntimeAsOriginalUser", seed);
        int deploy = Position(iss, "--control deploy", start);
        Assert.Contains("ExecAsOriginalUser", iss);
        Assert.True(seed < start && start < deploy);
        Assert.Contains("is-active", iss);
        Assert.DoesNotContain("new profile activation failed", iss);
        int shutdown = Position(iss, "'--control shutdown', True)");
        int legacyQuit = Position(iss, "'/quit', True", shutdown);
        int settle = Position(iss, "Sleep(750)", legacyQuit);
        Assert.True(shutdown < legacyQuit && legacyQuit < settle);
        Assert.DoesNotContain("--control status", iss);
        Assert.DoesNotContain("FamoDeploy.exe", iss, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void InnoSetup_ResumeStartsRuntimeThroughNativeDesktopTokenBroker()
    {
        string iss = InstallerText("famo-setup.iss");
        int start = Position(iss, "procedure StartRuntimeAsOriginalUser");
        int seed = Position(iss, "procedure InstallUserState", start);
        string startBody = iss[start..seed];

        Assert.Contains("if ResumeMode then", startBody);
        Assert.Contains("RunAndRequire(ProfileTool(TransactionTarget), 'start-runtime', False)", startBody);
        Assert.DoesNotContain("Shell.Application", startBody);
        Assert.DoesNotContain("FindWindowSW", startBody);
    }

    [Fact]
    public void InnoSetup_RollsBackInReverseOrder()
    {
        string iss = InstallerText("famo-setup.iss");
        int rollback = Position(iss, "procedure RollbackTransaction");

        int shutdownNew = Position(iss, "--control shutdown", rollback);
        int unregisterNew = Position(iss, "UnregisterTarget(TransactionTarget)", shutdownNew);
        int restoreRegistry = Position(iss, "RestorePreviousRegistry", unregisterNew);
        int restoreProfile = Position(iss, "RegisterPreviousRegistration", restoreRegistry);
        Assert.True(shutdownNew < unregisterNew && unregisterNew < restoreRegistry && restoreRegistry < restoreProfile);
        Assert.Contains("previous runtime health readback failed", iss);
        Assert.Contains("previous profile activation deferred; available via Win+Space", iss);
        Assert.DoesNotContain("previous profile activation rollback failed", iss);
    }

    [Fact]
    public void InnoSetup_HasDeterministicFailurePointsForTransactionRecovery()
    {
        string iss = InstallerText("famo-setup.iss");

        Assert.Contains("{param:FamoFail|}", iss);
        foreach (string phase in new[]
        {
            "after-prepare", "after-verify", "after-switch", "after-user-state", "after-active-verify",
            "after-pending-registration", "after-pending-state", "after-resume-registration", "after-resume-user-state",
        })
        {
            Assert.Contains($"FailIfRequested('{phase}')", iss);
        }
    }

    [Fact]
    public void NativeProfileTool_ProbesLoadedHostAndSupportsDisabledRegistration()
    {
        string tool = RepoText("native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        string cmake = RepoText("native/windows-tsf-famo/text-service/CMakeLists.txt");
        string selfcheck = RepoText("native/windows-tsf-famo/text-service/tests/module_selfcheck.cpp");

        foreach (string api in new[] { "RmStartSession", "RmRegisterResources", "RmGetList", "RmEndSession" })
        {
            Assert.Contains(api, tool);
        }
        foreach (string command in new[] { "register-disabled", "check-disabled", "check-absent", "enable", "disable", "loaded <dll>" })
        {
            Assert.Contains(command, tool);
        }
        Assert.Contains("Rstrtmgr", cmake);
        Assert.Contains("$<TARGET_FILE:${FAMO_PROFILE_TOOL}>", cmake);
        Assert.Contains("loaded", selfcheck);
        Assert.Contains("if (!ProfileActive())", tool);
        Assert.Contains("return S_OK;", tool);
    }

    [Fact]
    public void NativeRegistration_RemovesLegacyPerUserComOverrideOnRegister()
    {
        string source = RepoText("native/windows-tsf-famo/text-service/src/registration.cpp");
        int register = Position(source, "HRESULT RegisterComServer()");
        int unregister = Position(source, "void UnregisterComServer(bool cleanup_current_user)", register);
        string registerBody = source[register..unregister];

        Assert.Contains("RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str())", registerBody);
        Assert.Contains("ERROR_FILE_NOT_FOUND", registerBody);
        Assert.Contains("RegDeleteTreeW(HKEY_LOCAL_MACHINE, root.c_str())", registerBody);
    }

    [Fact]
    public void InnoSetup_DoesNotTrustPerUserComPathForElevatedPreviousHost()
    {
        string iss = InstallerText("famo-setup.iss");
        int snapshot = Position(iss, "procedure SnapshotPreviousState");
        int next = Position(iss, "function FindPathInList", snapshot);
        string body = iss[snapshot..next].Replace("\r\n", "\n");

        Assert.Contains("PreviousHost := '';", body);
        Assert.Contains("RegQueryStringValue(HKLM64", body);
        Assert.Contains("(PreviousTarget <> '') and (PreviousManifest <> '')", body);
        Assert.Contains("PreviousHost := AddBackslash(PreviousTarget) + 'FamoTextService.dll';", body);
        Assert.DoesNotContain("RegQueryStringValue(HKCU,\n      'Software\\Classes\\CLSID\\'", body);
    }

    [Fact]
    public void InnoSetup_LoadedLegacyHostEntersCoherentPendingReboot()
    {
        string iss = InstallerText("famo-setup.iss");
        int detect = Position(iss, "DetectLoadedPreviousHost");
        int pending = Position(iss, "EnterPendingReboot", detect);

        Assert.True(detect < pending);
        foreach (string value in new[]
        {
            "LoadedHostPath", "LoadedHostHash", "LoadedHostVersion", "LoadedHostExpectedHash",
            "PendingTarget", "PendingManifest", "PendingVersion", "PendingReason", "ResumeInstaller",
        })
        {
            Assert.Contains(value, iss);
        }
        Assert.Contains("'loaded ' + AddQuotes(PreviousHost)", iss);
        Assert.DoesNotContain("RegisterTargetDisabled(TransactionTarget)",
            iss[pending..Position(iss, "procedure StartRuntimeAsOriginalUser", pending)]);
        Assert.Contains("'check-absent'", iss);
        Assert.Contains("RegDeleteValue(HKLM64, RunKey, 'FamoRuntime')", iss);
        int clearRuntimeRun = Position(iss, "RegDeleteValue(HKLM64, RunKey, 'FamoRuntime')", pending);
        int verifyPending = Position(iss, "VerifyPendingInstall;", pending);
        Assert.True(clearRuntimeRun < verifyPending);
        Assert.Contains("/FamoResume=' + TransactionId", iss);
        Assert.Contains("StatePendingReboot", iss);
    }

    [Fact]
    public void InnoSetup_ResumeAndExplicitRollbackStayBoundToOneTransaction()
    {
        string iss = InstallerText("famo-setup.iss");
        int completePending = Position(iss, "procedure CompletePendingTransaction");
        int loadPending = Position(iss, "function LoadPendingState", completePending);
        string completePendingBody = iss[completePending..loadPending];

        Assert.Contains("{param:FamoResume|}", iss);
        Assert.Contains("{param:FamoRollback|}", iss);
        Assert.Contains("CompareText(StoredId, ExpectedId) = 0", iss);
        Assert.Contains("CompareText(PendingManifest", iss);
        Assert.Contains("ReturnToPendingAfterResumeFailure", iss);
        Assert.Contains("post-reboot verification failed:", iss);
        Assert.Contains("RegDeleteValue(HKLM64, RunOnceKey, ResumeValue)", iss);
        Assert.Contains("LoadPendingState(PendingId)", iss);
        Assert.DoesNotContain("restartreplace", completePendingBody, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("'activate', False", iss);
        Assert.Contains("RegisterTarget(TransactionTarget)", completePendingBody);
        Assert.DoesNotContain("pending profile activation failed", completePendingBody);
        Assert.DoesNotContain("RegisterTargetDisabled(TransactionTarget)", completePendingBody);
    }

    [Fact]
    public void InnoSetup_UninstallSplitsDesktopUserAndMachineCleanup()
    {
        string iss = InstallerText("famo-setup.iss");
        string profileTool = RepoText("native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        int uninstall = Position(iss, "procedure RemoveActiveInstall");
        string uninstallBody = iss[uninstall..Position(iss, "procedure CurUninstallStepChanged", uninstall)];

        int userCleanup = Position(uninstallBody, "'cleanup-user', False");
        int unregister = Position(uninstallBody, "UnregisterMachineTarget(ActiveTarget)", userCleanup);
        int deleteRun = Position(uninstallBody, "FamoRuntime", unregister);
        Assert.True(userCleanup < unregister && unregister < deleteRun);
        Assert.DoesNotContain("ExecAsOriginalUser", uninstallBody);
        Assert.DoesNotContain("'switch-away', False", uninstallBody);
        Assert.DoesNotContain("'--remove-input-tip', False", uninstallBody);
        Assert.Contains("GetShellWindow()", profileTool);
        Assert.Contains("CreateProcessWithTokenW", profileTool);
        Assert.Contains("LOGON_WITH_PROFILE", profileTool);
        Assert.Contains("L\"cleanup-user-state\"", profileTool);
        Assert.Contains("L\"--remove-input-tip\"", profileTool);
        Assert.Contains("RegDeleteTreeW(HKEY_CURRENT_USER", profileTool);
        Assert.Contains("DllUnregisterMachine", profileTool);
        Assert.Contains("DeleteUserData", iss);
        Assert.Contains("UninstallSilent", iss);
        Assert.Contains(@"{app}\versions", iss);
    }

    [Fact]
    public void InnoSetup_UninstallDefersOnlyLockedTsfHostDeletionToRestart()
    {
        string iss = InstallerText("famo-setup.iss");
        int uninstall = Position(iss, "function InitializeUninstall");
        string uninstallBody = iss[uninstall..];

        Assert.Contains("uninsrestartdelete", iss, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("OnlyLoadedHostResidue", uninstallBody);
        Assert.Contains("RestartReplace(LoadedHost, '')", uninstallBody);
        Assert.Contains("RestartReplace(VersionTarget, '')", uninstallBody);
        Assert.Contains("RestartReplace(VersionsRoot, '')", uninstallBody);
        Assert.Contains("function UninstallNeedRestart: Boolean", uninstallBody);
        Assert.Contains("Result := UninstallRestartPending", uninstallBody);
        Assert.Contains("loaded TSF host scheduled for restart deletion", uninstallBody);
        Assert.DoesNotContain("cannot remove transaction version directories", uninstallBody);
    }

    [Fact]
    public void InnoSetup_PinsVisibleFamoIdentityAndNeverWritesUserRime()
    {
        string iss = InstallerText("famo-setup.iss");
        string effective = EffectiveInnoContent(iss);

        Assert.Contains("#define AppName       \"法墨输入法\"", iss);
        Assert.Contains("SetupIconFile={#SetupIconFile}", iss);
        Assert.Contains(@"IconFilename: ""{code:GetActiveSettings}""", iss);
        Assert.DoesNotContain(@"DestDir: ""{userappdata}", iss, StringComparison.OrdinalIgnoreCase);
        foreach (string forbidden in new[] { "WeaselServer.exe", "WeaselDeployer.exe", "weaselx64.dll", "FamoDeploy.exe", "小狼毫" })
        {
            Assert.DoesNotContain(forbidden, effective, StringComparison.OrdinalIgnoreCase);
        }
    }

    [Fact]
    public void NativeBuildHasDistinctStableAndDevelopmentIdentitySets()
    {
        string cmake = RepoText("native/windows-tsf-famo/text-service/CMakeLists.txt");
        string tsf = RepoText("native/windows-tsf-famo/text-service/src/famo_guids.h");
        string runtime = RepoText("native/windows-tsf-famo/runtime-protocol/include/famo_runtime_identity.h");

        Assert.Contains("FAMO_IDENTITY", cmake);
        Assert.Contains("FAMO_STABLE_IDENTITY", cmake);
        Assert.Contains("54ead76a", tsf, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("a6e6f585", tsf, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("0158c2ba", tsf, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("189ab82f", tsf, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("dev-runtime-v1", runtime);
        Assert.Contains("runtime-v1", runtime);
    }

    private static int Position(string text, string needle, int start = 0)
    {
        int at = text.IndexOf(needle, start, StringComparison.OrdinalIgnoreCase);
        Assert.True(at >= 0, $"expected installer text: {needle}");
        return at;
    }

    private static string InstallerText(string name) =>
        RepoText($"native/windows-tsf-famo/installer/{name}");

    private static string RepoText(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return File.ReadAllText(candidate);
            dir = Directory.GetParent(dir)?.FullName;
        }
        throw new FileNotFoundException(relativePath);
    }

    private static string EffectiveInnoContent(string iss) =>
        string.Join("\n", iss.Split('\n')
            .Select(line => line.Trim())
            .Where(line => line.Length > 0)
            .Where(line => !line.StartsWith(';'))
            .Where(line => !line.StartsWith("//")));
}
