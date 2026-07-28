using Xunit;

namespace Famo.Settings.Tests;

public sealed class MaintenanceSafeTypingContractTests
{
    [Fact]
    public void RuntimeMaintenanceInvalidatesSessionsBeforeConditionalReady()
    {
        string control = File.ReadAllText(RepoFile("native/windows-tsf-famo/runtime-protocol/src/runtime_control_engine.cpp"));

        Assert.Contains("readiness_.exchange(RuntimeReadiness::Maintenance)", control);
        Assert.Contains("engine_.DestroyContext", control);
        Assert.Contains("engine_.LoadV2", control);
        Assert.Contains("engine_.V2Runnable", control);
        Assert.DoesNotContain("engine_.Load(", control);
        Assert.DoesNotContain("engine_.api()", control);
        Assert.Contains("sessions_.clear()", control);
        Assert.Contains("clients_.clear()", control);
        Assert.Contains("if (deploy_rc == FAMO_ENGINE_OK)", control);
        Assert.Contains("readiness_.store(RuntimeReadiness::Ready)", control);
    }

    [Fact]
    public void KeyProcessingFailsOpenDuringMaintenance()
    {
        string runtime = File.ReadAllText(RepoFile("native/windows-tsf-famo/runtime-protocol/src/runtime_service.cpp"));

        Assert.Contains("readiness_.load() != RuntimeReadiness::Ready", runtime);
        Assert.Contains("std::try_to_lock", runtime);
        Assert.Contains("Status::Unavailable", runtime);
    }

    [Fact]
    public void DeployQueueAndHealthExposeTerminalMaintenanceStates()
    {
        string deploy = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/DeployService.cs"));
        string health = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Test-FamoHealth.ps1"));

        Assert.Contains("DeployQueueStatus.Running", deploy);
        Assert.Contains("DeployQueueStatus.Succeeded", deploy);
        Assert.Contains("DeployQueueStatus.Failed", deploy);
        Assert.Contains("RetryAvailable", deploy);
        Assert.Contains("Degraded", health);
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
