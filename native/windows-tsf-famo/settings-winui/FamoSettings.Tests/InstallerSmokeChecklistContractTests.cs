using Xunit;

namespace Famo.Settings.Tests;

public sealed class InstallerSmokeChecklistContractTests
{
    private const string ChecklistPath = "native/windows-tsf-famo/installer/smoke_test.md";
    private const string HarnessPath = "native/windows-tsf-famo/installer/smoke-harness.ps1";
    private const string HealthPath = "native/windows-tsf-famo/weasel-fork/tests/Test-FamoHealth.ps1";

    [Fact]
    public void SmokeChecklist_DefaultsToCurrentMachineAndOriginalRepro()
    {
        string doc = File.ReadAllText(RepoFile(ChecklistPath));

        Assert.Contains("current development machine", doc, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("original long-input Explorer scenario", doc, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Do not install an app", doc, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("download an ISO", doc, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("create a VM", doc, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("%AppData%\\Rime", doc);
        Assert.Contains("%LOCALAPPDATA%\\Famo", doc);
    }

    [Fact]
    public void SmokeHarness_IsFocusedAndDoesNotCreateAReleaseMatrix()
    {
        string script = File.ReadAllText(RepoFile(HarnessPath));

        Assert.Contains("current-machine smoke", script, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Test-FamoHealth.ps1", script);
        Assert.Contains("Test-FamoTsfRegistration.ps1", script);
        Assert.Contains("脚本不会自行重复触发 UAC", script);
        foreach (string forbidden in new[]
        {
            "EvidenceDir", "smoke-report", "windows10", "windows11", "VMConnect",
            "Stop-Process", "PendingFileRenameOperations", "UAC cancel",
        })
        {
            Assert.DoesNotContain(forbidden, script, StringComparison.OrdinalIgnoreCase);
        }
    }

    [Fact]
    public void HealthProbeIsReadOnlyForUserWeaselData()
    {
        string script = File.ReadAllText(RepoFile(HealthPath));

        Assert.Contains("NoWrite:%AppData%\\Rime", script);
        Assert.DoesNotContain("Set-Content", script);
        Assert.DoesNotContain("New-Item", script);
        Assert.DoesNotContain("Remove-Item", script);
        Assert.DoesNotContain("Set-ItemProperty", script);
    }

    private static string RepoFile(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (dir is not null)
        {
            string candidate = Path.Combine(dir, relativePath.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate))
            {
                return candidate;
            }
            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate repository file: {relativePath}");
    }
}
