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
            "text-service\\build-bridge-v3-artifact",
            script);

        string installer = RepoText(
            "native/windows-tsf-famo/installer/famo-setup.iss");
        Assert.Contains("#define BridgeAbi \"3\"", installer);

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
        Assert.Contains("set(FAMO_BRIDGE_ABI \"3\"", cmake);
        Assert.Contains("FamoTextService.rc.in", cmake);
        Assert.Contains("FILEVERSION @FAMO_BRIDGE_ABI@", resource);
        Assert.Contains("FileMajorPart", artifactBuilder);
        Assert.Contains("$BridgeAbi", artifactBuilder);
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
