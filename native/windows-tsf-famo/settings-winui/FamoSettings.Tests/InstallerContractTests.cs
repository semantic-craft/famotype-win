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
        foreach (string command in new[] { "register-disabled", "check-disabled", "enable", "disable", "loaded <dll>" })
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
        int unregister = Position(source, "void UnregisterComServer()", register);
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
        int next = Position(iss, "function CountFiles", snapshot);
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
        Assert.Contains("RegisterTargetDisabled(TransactionTarget)", iss);
        Assert.Contains("'check-disabled'", iss);
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
        Assert.Contains("RunAndRequire(ProfileTool(TransactionTarget), 'enable', False)", completePendingBody);
        Assert.DoesNotContain("pending profile activation failed", completePendingBody);
        Assert.DoesNotContain("RegisterTarget(TransactionTarget)", completePendingBody);
    }

    [Fact]
    public void InnoSetup_UninstallSwitchesAwayBeforeRemoval()
    {
        string iss = InstallerText("famo-setup.iss");
        int uninstall = Position(iss, "procedure RemoveActiveInstall");

        int switchAway = Position(iss, "switch-away", uninstall);
        int removeTip = Position(iss, "--remove-input-tip", switchAway);
        int unregister = Position(iss, "UnregisterTarget(ActiveTarget)", removeTip);
        int deleteRun = Position(iss, "FamoRuntime", unregister);
        Assert.True(switchAway < removeTip && removeTip < unregister && unregister < deleteRun);
        Assert.Contains("RunAndRequire(Runtime, '--control shutdown', False)", iss);
        Assert.Contains("RunAndRequire(Settings, '--remove-input-tip', False)", iss);
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
