using Xunit;

namespace Famo.Settings.Tests;

public sealed class ApplyFeedbackContractTests
{
    [Fact]
    public void ApplyStatusCopySeparatesQueuedAndConfirmedStates()
    {
        string app = File.ReadAllText(RepoFile(Path.Combine("native", "windows-tsf-famo", "settings-winui", "FamoSettings", "App.xaml.cs")));
        string keyboard = File.ReadAllText(ViewFile("KeyboardPage.cs"));
        string candidate = File.ReadAllText(ViewFile("CandidatePage.cs"));
        string quickPhrases = File.ReadAllText(ViewFile("QuickPhrasesPage.cs"));
        string shortcuts = File.ReadAllText(ViewFile("ShortcutsPage.cs"));
        string skin = File.ReadAllText(ViewFile("SkinPage.cs"));

        Assert.Contains("DeployService.QueueChanged", app);
        Assert.Contains("ReloadStatusToken", app);
        Assert.Contains("snapshot.RequestId != result.RequestId", app);
        Assert.Contains("配置已生效。", app);
        Assert.Contains("Text = \"重试\"", app);
        Assert.Contains("DeployService.Retry(result.RequestId)", app);
        Assert.Contains("输入方式已生效。", keyboard);
        Assert.Contains("候选个数已保存，并已发送应用命令。", candidate);
        Assert.Contains("候选个数已生效。", candidate);
        Assert.Contains("已保存，并已发送应用命令。", quickPhrases);
        Assert.Contains("已保存，并已应用配置改动。", quickPhrases);
        Assert.Contains("已删除，并已发送应用命令。", quickPhrases);
        Assert.Contains("已删除，并已应用配置改动。", quickPhrases);
        Assert.Contains("AppliedStatus", shortcuts);
        Assert.Contains("热键配置已部署生效。", shortcuts);
        Assert.Contains("皮肤 / 明暗已生效。", skin);

        string joined = string.Join("\n", keyboard, candidate, quickPhrases, shortcuts, skin);
        Assert.DoesNotContain("? \"已应用。\"", joined);
        Assert.DoesNotContain("候选个数已应用。", joined);
    }

    private static string ViewFile(string fileName) =>
        RepoFile(Path.Combine("native", "windows-tsf-famo", "settings-winui", "FamoSettings", "Views", fileName));

    private static string RepoFile(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath);
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate {relativePath}");
    }
}
