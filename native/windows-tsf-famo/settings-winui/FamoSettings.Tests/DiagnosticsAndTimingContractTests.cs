using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class DiagnosticsAndTimingContractTests
{
    [Fact]
    public void TimingLogIsOptInBoundedRateLimitedAndSanitized()
    {
        string dir = Path.Combine(Path.GetTempPath(), "famo-timing-tests-" + Guid.NewGuid().ToString("N"));
        try
        {
            FamoTimingLog.ResetForTests();
            FamoTimingLog.MinWriteInterval = TimeSpan.Zero;
            FamoTimingLog.ForceEnabledForTests = false;
            FamoTimingLog.Append("deploy Queue", "/deploy local mode", TimeSpan.FromMilliseconds(12.345), "exit 0", dir);
            Assert.False(File.Exists(Path.Combine(dir, "famo-timing.log")));

            FamoTimingLog.ForceEnabledForTests = true;
            FamoTimingLog.Append("deploy Queue", "/deploy local mode", TimeSpan.FromMilliseconds(12.345), "exit 0", dir);
            string text = File.ReadAllText(Path.Combine(dir, "famo-timing.log"));
            Assert.Contains("component=deploy_Queue", text);
            Assert.Contains("operation=/deploy_local_mode", text);
            Assert.Contains("elapsedMs=12.345", text);
            Assert.Contains("status=exit_0", text);
            Assert.DoesNotContain("api_key", text, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("token", text, StringComparison.OrdinalIgnoreCase);
            Assert.Equal(128 * 1024, FamoTimingLog.MaxLogBytes);
        }
        finally
        {
            FamoTimingLog.ResetForTests();
            if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public void DeployServiceEmitsOptInDeployQueueTimingWithoutLoggingCommandPath()
    {
        string deploy = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/DeployService.cs"));

        Assert.Contains("ProductionTimingLogger", deploy);
        Assert.Contains("FamoTimingLog.Append(component, operation, elapsed, status)", deploy);
        Assert.Contains("TimingLogger(\"deployQueue\", reload.Args, elapsed.Elapsed, \"succeeded\")", deploy);
        Assert.Contains("TimingLogger(\"deployQueue\", reload.Args, elapsed.Elapsed, $\"exit:{exitCode}\")", deploy);
        Assert.Contains("TimingLogger(\"deployQueue\", reload.Args, elapsed.Elapsed, \"exception\")", deploy);
        Assert.DoesNotContain("TimingLogger(\"deployQueue\", reload.Command", deploy);
    }

    [Fact]
    public void DiagnosticsScriptCollectsSafeSupportBundle()
    {
        string script = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Get-FamoDiagnostics.ps1"));

        foreach (string expected in new[]
        {
            "install",
            "registry",
            "health",
            "readiness",
            "dataIsolation",
            "settingsLog",
            "timingLog",
            "healthProbeMs",
            "tsfRegistrationAuditMs",
            "ipcPipeConnectMs",
            "deployQueue",
            "candidateStatusUi",
            "panelProbe",
            "concerns",
        })
        {
            Assert.Contains(expected, script);
        }
    }

    [Fact]
    public void DiagnosticsScriptAvoidsSensitiveOrTypedPayloadSources()
    {
        string script = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Get-FamoDiagnostics.ps1"));

        Assert.Contains("collectsClipboard = $false", script);
        Assert.Contains("collectsTypedText = $false", script);
        Assert.Contains("collectsDictionaries = $false", script);
        Assert.Contains("collectsSecrets = $false", script);
        Assert.Contains("Redact-Line", script);
        Assert.DoesNotContain("Get-Clipboard", script);
        Assert.DoesNotContain("ClipboardHistoryFile", script);
        Assert.DoesNotContain("quick-phrases.json", script, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("prompt-library.json", script, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("ai-providers.json", script, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("*.dict", script, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("rime_ice.dict", script, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("Set-Content", script);
        Assert.DoesNotContain("Remove-Item", script);
    }

    [Fact]
    public void ExplorerHangDiagnosticsUseTheActiveTransactionalTextService()
    {
        string orchestrator = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/tools/diagnostics/Invoke-FamoExplorerHangAB.ps1"));
        string probe = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/tools/diagnostics/Invoke-FamoExplorerHangProbe.ps1"));

        Assert.Contains("FamoTextService.dll", orchestrator);
        Assert.Contains(@"SOFTWARE\Classes\CLSID\", orchestrator);
        Assert.Contains("InProcServer32", orchestrator, StringComparison.OrdinalIgnoreCase);
        Assert.Contains(@"installer\staging\payload", orchestrator);
        Assert.Contains("PreflightOnly", orchestrator);
        Assert.DoesNotContain(@"$FamoDll = 'C:\Program Files\Famo\FamoTsf.dll'", orchestrator);

        Assert.Contains("FamoModulePath", probe);
        Assert.Contains("FamoTextService.dll", probe);
        Assert.DoesNotContain("$_.name -ieq 'FamoTsf.dll'", probe);
    }

    private static string RepoFile(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate {relativePath}");
    }
}
