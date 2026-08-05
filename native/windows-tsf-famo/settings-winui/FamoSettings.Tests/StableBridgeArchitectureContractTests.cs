using Xunit;

namespace Famo.Settings.Tests;

public sealed class StableBridgeArchitectureContractTests
{
    [Fact]
    public void InstallerBuild_ConsumesAnIndependentVersionedBridgeArtifact()
    {
        string script = RepoText(
            "native/windows-tsf-famo/installer/build-installer.ps1");

        Assert.Contains("$BridgeArtifact", script);
        Assert.Contains("bridge-manifest.txt", script);
        Assert.Contains("'bridge_abi'", script);
        Assert.Contains("'protocol_min'", script);
        Assert.Contains("'protocol_max'", script);
        Assert.Contains("/DBridgeAbi=$bridgeAbi", script);
        Assert.Contains("/DBridgeHash=$bridgeHash", script);
        Assert.Contains(
            "text-service\\build-bridge-v13-artifact",
            script);

        string installer = RepoText(
            "native/windows-tsf-famo/installer/famo-setup.iss");
        Assert.Contains("#define BridgeAbi \"13\"", installer);

        int runtimeFiles = Position(script, "$runtimeFiles = @(");
        int runtimeFilesEnd = Position(script, ")", runtimeFiles);
        Assert.DoesNotContain(
            "FamoTextService.dll",
            script[runtimeFiles..runtimeFilesEnd]);
    }

    [Fact]
    public void Installer_RuntimeOnlyUpgrade_DoesNotProbeOrReregisterBridge()
    {
        string installer = RepoText(
            "native/windows-tsf-famo/installer/famo-setup.iss");

        Assert.Contains(@"bridge\v{#BridgeAbi}", installer);
        Assert.Contains(
            "Result := AddBackslash(FixedBridgeDirectory) + 'FamoTextService.dll'",
            installer);
        Assert.Contains("function TransactionChangedBridge: Boolean;", installer);
        Assert.Contains("function ShouldInstallBridge: Boolean;", installer);
        Assert.Contains("if TransactionChangedBridge then", installer);
        Assert.Contains(
            "if TransactionChangedBridge then",
            installer);
        Assert.Contains(
            "Source: \"{#StagingDir}\\bridge\\FamoTextService.dll\"",
            installer);
        Assert.Contains("Check: ShouldInstallBridge", installer);
        Assert.Contains(
            "CompareText(RegisteredDll, FixedBridgeDll) <> 0",
            installer);
    }

    [Fact]
    public void ProfileTool_RegistersAnExplicitStableBridgePath()
    {
        string tool = RepoText(
            "native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");

        Assert.Contains("InvokeRegistrationAt", tool);
        Assert.Contains("register-machine-at", tool);
        Assert.Contains("unregister-machine-at", tool);
    }

    [Fact]
    public void HealthProbes_ValidateTheIndependentBridgeProjection()
    {
        foreach (string relativePath in new[]
        {
            "native/windows-tsf-famo/weasel-fork/tests/Test-FamoHealth.ps1",
            "native/windows-tsf-famo/weasel-fork/tests/Test-FamoTsfRegistration.ps1",
        })
        {
            string probe = RepoText(relativePath);
            Assert.Contains("BridgePath", probe);
            Assert.Contains("BridgeHash", probe);
            Assert.Contains("BridgeAbi", probe);
            Assert.Contains("bridge-manifest.txt", probe);
            Assert.Contains("FamoTextService.dll", probe);
        }
    }

    [Fact]
    public void BridgeAbi_IsEmbeddedAndCheckedAsTheDllFileMajorVersion()
    {
        string cmake = RepoText(
            "native/windows-tsf-famo/text-service/CMakeLists.txt");
        string artifactBuilder = RepoText(
            "native/windows-tsf-famo/installer/build-bridge-artifact.ps1");
        string resource = RepoText(
            "native/windows-tsf-famo/text-service/src/FamoTextService.rc.in");

        Assert.Contains("FAMO_BRIDGE_ABI", cmake);
        Assert.Contains("set(FAMO_BRIDGE_ABI \"13\"", cmake);
        Assert.Contains("FamoTextService.rc.in", cmake);
        Assert.Contains("FILEVERSION @FAMO_BRIDGE_ABI@", resource);
        Assert.Contains("FileMajorPart", artifactBuilder);
        Assert.Contains("$BridgeAbi", artifactBuilder);
    }

    [Fact]
    public void HostOwnedCandidate_PresentsInTheExactContextViewOwner()
    {
        string cmake = RepoText(
            "native/windows-tsf-famo/text-service/CMakeLists.txt");
        string service = RepoText(
            "native/windows-tsf-famo/text-service/src/text_service.cpp");
        string ui = RepoText(
            "native/windows-tsf-famo/text-service/src/text_service_ui.cpp");
        string candidate = RepoText(
            "native/windows-tsf-famo/runtime-protocol/src/candidate_window.cpp");

        Assert.Contains("famo_candidate_window", cmake);
        Assert.Contains("candidate_window_.Start()", service);
        Assert.Contains("view->GetWnd(&window)", ui);
        Assert.Contains("entry->candidate_owner = window", ui);
        Assert.Contains("snapshot->require_in_process_owner = true", ui);
        Assert.Contains("snapshot->selection_target = recovery_window_", ui);
        Assert.Contains("published.show_allowed = false", ui);
        Assert.Contains("GetWindow(window, GW_OWNER) != owner", candidate);
        Assert.DoesNotContain("pbShow = TRUE", ui);
    }

    [Fact]
    public void CandidateAppearance_ReachesTheHostOverTheWireNotFromDisk()
    {
        string service = RepoText(
            "native/windows-tsf-famo/text-service/src/text_service.cpp");
        string ui = RepoText(
            "native/windows-tsf-famo/text-service/src/text_service_ui.cpp");
        string candidate = RepoText(
            "native/windows-tsf-famo/runtime-protocol/src/candidate_window.cpp");

        // The presenter draws inside the host process, and a sandboxed host
        // such as SearchHost cannot read the user profile at all. Appearance
        // therefore has to arrive over the authenticated pipe and ride on the
        // published snapshot; a presenter that opens the style file itself
        // silently falls back to the built-in skin in exactly those hosts.
        Assert.Contains("Command::GetStyleOverlay", service);
        Assert.Contains("snapshot->style = runtime_style_.load()", ui);
        Assert.Contains("next_presentation = snapshot->style->presentation",
                        candidate);
        Assert.DoesNotContain("style_root", candidate);
        Assert.DoesNotContain("ReadRuntimeStyleOverlay", candidate);
    }

    [Fact]
    public void SmokeHarness_CanRequireARebootFreeRuntimeOnlyUpgrade()
    {
        string smoke = RepoText(
            "native/windows-tsf-famo/installer/smoke-harness.ps1");

        Assert.Contains("$RequireRuntimeOnly", smoke);
        Assert.Contains("BridgePath", smoke);
        Assert.Contains("FileId", smoke);
        Assert.Contains("PendingReboot", smoke);
        Assert.Contains("RUNTIME-ONLY SMOKE PASS", smoke);
    }

    [Fact]
    public void Uninstall_CleansOrSchedulesTheLoadedStableBridge()
    {
        string installer = RepoText(
            "native/windows-tsf-famo/installer/famo-setup.iss");

        Assert.Contains("procedure CleanupStableBridgeForUninstall;", installer);
        Assert.Contains("RestartReplace(FixedBridgeDll, '')", installer);
        Assert.Contains("CleanupStableBridgeForUninstall;", installer);
    }

    [Fact]
    public void ExplorerDiagnostic_UsesTheStableBridgePath()
    {
        string diagnostic = RepoText(
            "native/windows-tsf-famo/tools/diagnostics/Invoke-FamoExplorerHangAB.ps1");

        Assert.Contains("[string] $BridgePath", diagnostic);
        Assert.Contains("RegisteredDll", diagnostic);
        Assert.Contains("BridgePath", diagnostic);
        Assert.DoesNotContain(
            "COM registration does not match InstallDir",
            diagnostic);
    }

    [Fact]
    public void Installer_CanValidateAndRestoreAPreviousBridgeAbi()
    {
        string installer = RepoText(
            "native/windows-tsf-famo/installer/famo-setup.iss");

        Assert.Contains("function TryParseManagedStableBridgePath", installer);
        Assert.Contains("PreviousBridgePath", installer);
        Assert.Contains("PreviousBridgeHash", installer);
        Assert.Contains(
            "RegWriteStringValue(HKLM64, BrandKey, 'BridgePath', PreviousHost)",
            installer);
        Assert.Contains(
            "RegWriteStringValue(HKLM64, BrandKey, 'BridgeHash',",
            installer);
    }

    [Fact]
    public void BridgeAbi_AllFourDeclarationSitesAgree()
    {
        // The v2 release bumped only the installer sites and shipped a 2.0.0.0
        // DLL while the tree still declared ABI 1 — the pinned-literal checks
        // above were updated selectively, which is exactly how that drift
        // slipped through. This test does not pin a number: it parses the ABI
        // out of every declaration site and requires them to be one value.
        string cmake = RepoText(
            "native/windows-tsf-famo/text-service/CMakeLists.txt");
        string header = RepoText(
            "native/windows-tsf-famo/text-service/include/famo_bridge_abi.h");
        string installer = RepoText(
            "native/windows-tsf-famo/installer/famo-setup.iss");
        string build = RepoText(
            "native/windows-tsf-famo/installer/build-installer.ps1");

        string fromCmake = Extract(
            cmake, "set\\(FAMO_BRIDGE_ABI \"(\\d+)\"");
        string fromHeader = Extract(
            header, "#define FAMO_BRIDGE_ABI_VERSION (\\d+)");
        string fromInstaller = Extract(
            installer, "#define BridgeAbi \"(\\d+)\"");
        string fromBuildScript = Extract(
            build, "build-bridge-v(\\d+)-artifact");

        Assert.Equal(fromCmake, fromHeader);
        Assert.Equal(fromCmake, fromInstaller);
        Assert.Equal(fromCmake, fromBuildScript);
    }

    private static string Extract(string text, string pattern)
    {
        var match = System.Text.RegularExpressions.Regex.Match(text, pattern);
        Assert.True(match.Success, $"expected ABI declaration: {pattern}");
        return match.Groups[1].Value;
    }

    private static int Position(string text, string needle, int start = 0)
    {
        int at = text.IndexOf(needle, start, StringComparison.OrdinalIgnoreCase);
        Assert.True(at >= 0, $"expected architecture contract text: {needle}");
        return at;
    }

    private static string RepoText(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(
                dir,
                relativePath.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }
            dir = Directory.GetParent(dir)?.FullName;
        }
        throw new FileNotFoundException(relativePath);
    }
}
