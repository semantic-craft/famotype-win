using Xunit;

namespace Famo.Settings.Tests;

public sealed class RuntimeBoundaryAuditContractTests
{
    private const string AuditPath = "native/windows-tsf-famo/weasel-fork/RUNTIME-BOUNDARY-AUDIT.md";

    [Fact]
    public void AuditMapsEveryRuntimeSurfaceToOwnerStateRecoveryAndCheck()
    {
        string audit = File.ReadAllText(RepoFile(AuditPath));

        foreach (string column in new[] { "Runtime surface", "Owner", "Health state", "Recovery path", "Existing check" })
        {
            Assert.Contains(column, audit);
        }

        foreach (string surface in new[]
        {
            "TSF registration and profile visibility",
            "Runtime process startup",
            "Pipe readiness and IPC hot path",
            "Engine/session ABI",
            "Deployer and maintenance queue",
            "Settings UI apply surface",
            "Candidate/status UI and TSF UIElement",
            "Safe diagnostics and local timing",
        })
        {
            Assert.Contains(surface, audit);
        }

        foreach (string check in new[]
        {
            "Test-FamoTsfRegistration.ps1 -Json",
            "Test-FamoHealth.ps1 -Json",
            "BoundedIpcPatchContractTests",
            "FamoEngineApiContractTests",
            "DeployServiceTests",
            "SettingsReloadStatusContractTests",
            "PanelSmoothnessContractTests",
            "DiagnosticsAndTimingContractTests",
        })
        {
            Assert.Contains(check, audit);
        }
    }

    [Fact]
    public void DeployerFailureBoundaryKeepsSettingsIndependent()
    {
        string audit = File.ReadAllText(RepoFile(AuditPath));
        string deploy = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/DeployService.cs"));

        Assert.Contains("A failing deployer must not block settings UI or typing.", audit);
        Assert.Contains("DeployQueueStatus.Failed", deploy);
        Assert.Contains("RetryAvailable", deploy);
        Assert.Contains("QueueChanged", deploy);
    }

    [Fact]
    public void CandidateStatusChecksStaySeparateFromEngineRebuilds()
    {
        string audit = File.ReadAllText(RepoFile(AuditPath));
        string panel = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/PANEL-SMOOTHNESS.md"));
        string health = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Test-FamoHealth.ps1"));
        string diagnostics = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Get-FamoDiagnostics.ps1"));

        Assert.Contains("Candidate/status stale-state checks can run without rebuilding the engine.", audit);
        Assert.Contains("PANEL-SMOOTHNESS.md", audit);
        Assert.Contains("healthState", health);
        Assert.DoesNotContain("panelFailureBoundary", health);
        Assert.Contains("candidateStatusUi", diagnostics);
        Assert.Contains("Until a real automated window probe exists, a panel failure is a `PANEL` smoke", panel);
    }

    [Fact]
    public void FcitxAndIbusStyleFrameworkSplitsAreExplicitNonGoals()
    {
        string audit = File.ReadAllText(RepoFile(AuditPath));

        foreach (string nonGoal in new[]
        {
            "Non-goal: a generic Fcitx-style addon/plugin host",
            "Non-goal: a D-Bus-style IBus daemon/engine/panel protocol",
            "Non-goal: moving TSF profile ownership into a separate panel process",
            "Non-goal: a cross-platform runtime framework abstraction",
        })
        {
            Assert.Contains(nonGoal, audit);
        }
    }

    [Fact]
    public void CommittedCompositionIsEndedWithoutClearingCommittedText()
    {
        string source = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/text-service/src/composition_controller.cpp"));
        int emptyPreedit = source.IndexOf("if (preedit.text.empty())", StringComparison.Ordinal);
        Assert.True(emptyPreedit >= 0);
        int nextPreedit = source.IndexOf("if (!composition_)", emptyPreedit, StringComparison.Ordinal);
        Assert.True(nextPreedit > emptyPreedit);
        string branch = source[emptyPreedit..nextPreedit];

        int committed = branch.IndexOf("if (!plan.commit.empty())", StringComparison.Ordinal);
        int end = branch.IndexOf("EndCurrent(cookie)", committed, StringComparison.Ordinal);
        int clear = branch.IndexOf("ReplaceRange(cookie, context, range.get(), L\"\")", StringComparison.Ordinal);
        Assert.True(committed >= 0 && end > committed && clear > end);
    }

    [Fact]
    public void StableBridgeResolvesTheVersionedRuntimeProjection()
    {
        string tsf = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/text-service/src/text_service.cpp"))
            .ReplaceLineEndings("\n");
        string runtime = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/runtime-protocol/src/runtime_main.cpp"));

        Assert.Contains("ResolveProductionRuntime(&expected)", tsf);
        Assert.DoesNotContain(
            "ProductionInstallAllowed(ModuleDirectory())",
            tsf);
        Assert.Contains(
            "if (runtime_executable_name_ == L\"FamoRuntime.exe\") {\n"
            + "    if (!runtime::ResolveProductionRuntime(&expected))",
            tsf);
        Assert.Contains(
            "} else {\n"
            + "    expected = ModuleDirectory() + L\"\\\\\" + runtime_executable_name_;",
            tsf);
        Assert.Contains("ProductionInstallAllowed(ModuleDirectory(), true)", runtime);
        Assert.Contains("famo-runtime-startup.log", runtime);
        Assert.Contains(
            "AppendStartupDiagnostic(data_root, \"install-state\", 3",
            runtime);
        Assert.Contains(
            "AppendStartupDiagnostic(data_root, \"singleton\", 3)",
            runtime);
        Assert.Contains(
            "AppendStartupDiagnostic(data_root, \"candidate-window\", 3)",
            runtime);
        Assert.Contains(
            "AppendStartupDiagnostic(data_root, \"engine\", 4, error)",
            runtime);
        Assert.Contains(
            "AppendStartupDiagnostic(data_root, \"control-pipe\", 5)",
            runtime);
    }

    [Fact]
    public void DevelopmentRuntimeDoesNotRequireAStableInstallProjection()
    {
        string runtime = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/runtime-protocol/src/runtime_main.cpp"))
            .ReplaceLineEndings("\n");

        int startupGuard = runtime.IndexOf(
            "#if defined(FAMO_STABLE_IDENTITY)\n  if (endpoint_suffix",
            StringComparison.Ordinal);
        Assert.True(startupGuard >= 0,
            "the Stable Runtime install-state check must have an identity guard");
        int startupCheck = runtime.IndexOf(
            "ProductionInstallAllowed(ModuleDirectory(), true)",
            startupGuard,
            StringComparison.Ordinal);
        int startupEnd = runtime.IndexOf("#endif", startupGuard,
            StringComparison.Ordinal);
        Assert.True(startupGuard >= 0 && startupCheck > startupGuard &&
            startupEnd > startupCheck,
            "only a Stable Runtime may require the machine install projection");

        int singleton = runtime.IndexOf("ERROR_ALREADY_EXISTS",
            StringComparison.Ordinal);
        int healGuard = runtime.IndexOf(
            "#if defined(FAMO_STABLE_IDENTITY)", singleton,
            StringComparison.Ordinal);
        Assert.True(healGuard > singleton,
            "production TIP self-heal must have an identity guard");
        int healThread = runtime.IndexOf("std::thread([data_root]",
            singleton,
            StringComparison.Ordinal);
        int healEnd = runtime.IndexOf("#endif", healGuard,
            StringComparison.Ordinal);
        Assert.True(healGuard > singleton && healThread > healGuard &&
            healEnd > healThread,
            "Development Runtime must not run production TIP self-heal");
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
