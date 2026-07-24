using Xunit;

namespace Famo.Settings.Tests;

public sealed class SettingsReloadStatusContractTests
{
    private static readonly string[] ReloadPages =
    {
        "native/windows-tsf-famo/settings-winui/FamoSettings/Views/AboutPage.cs",
        "native/windows-tsf-famo/settings-winui/FamoSettings/Views/CandidatePage.cs",
        "native/windows-tsf-famo/settings-winui/FamoSettings/Views/KeyboardPage.cs",
        "native/windows-tsf-famo/settings-winui/FamoSettings/Views/QuickPhrasesPage.cs",
        "native/windows-tsf-famo/settings-winui/FamoSettings/Views/ShortcutsPage.cs",
        "native/windows-tsf-famo/settings-winui/FamoSettings/Views/SkinPage.cs",
    };

    [Fact]
    public void AppExposesQueueBackedReloadStatusReporter()
    {
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));

        Assert.Contains("ReportReloadResult(", app);
        Assert.Contains("DeployService.QueueChanged +=", app);
        Assert.Contains("DeployService.QueueChanged -=", app);
        Assert.Contains("ReloadStatusToken", app);
        Assert.Contains("IsCurrentReloadStatus", app);
        Assert.Contains("RetryAvailable", app);
        Assert.Contains("Text = \"重试\"", app);
        Assert.Contains("DeployService.Retry(result.RequestId)", app);
    }

    [Fact]
    public void ReloadPagesUseQueueBackedStatusReporter()
    {
        foreach (string pagePath in ReloadPages)
        {
            string page = File.ReadAllText(RepoFile(pagePath));
            Assert.Contains("App.ReportReloadResult(", page);
            Assert.DoesNotContain("已发送应用命令。\" : $\"应用失败", page);
        }
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
