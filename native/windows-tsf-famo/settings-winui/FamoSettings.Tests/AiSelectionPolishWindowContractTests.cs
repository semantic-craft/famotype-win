using Xunit;

namespace Famo.Settings.Tests;

public sealed class AiSelectionPolishWindowContractTests
{
    [Fact]
    public void AiSelectionPolishWindow_CapturesSelectionRunsAiAndKeepsCopyOnlyOutput()
    {
        string window = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiSelectionPolishWindow.cs"));

        Assert.Contains("public sealed class AiSelectionPolishWindow : Window", window);
        Assert.Contains("SelectedTextCaptureService", window);
        Assert.Contains("WindowsFocusedTextSelectionReader", window);
        Assert.Contains("ClipboardCopySelectionReader", window);
        Assert.Contains("AiSelectionSkillService", window);
        Assert.Contains("AiSelectionSkills.Polish", window);
        Assert.Contains("复制", window);
        Assert.Contains("Clipboard.SetContent", window);

        Assert.DoesNotContain("TextInjector", window);
        Assert.DoesNotContain("DeployService", window);
        Assert.DoesNotContain("SaveAndApply", window);
    }

    [Fact]
    public void App_DeepLinkCanOpenAiSelectionPolishWindow()
    {
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));

        Assert.Contains("_aiSelectionSkillWindow", app);
        Assert.Contains("IsAiPolishPage", app);
        Assert.Contains("TryResolveAiSelectionSkillPage", app);
        Assert.Contains("ShowAiSelectionPolish", app);
        Assert.Contains("ShowAiSelectionSkill", app);
        Assert.Contains("\"ai-polish\"", app);
        Assert.Contains("ai-source-check", File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/Ai/AiSelectionPolishService.cs")));
        Assert.Contains("ai-research", File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/Ai/AiSelectionPolishService.cs")));
        Assert.Contains("ai-document-formatting", File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/Ai/AiSelectionPolishService.cs")));
    }

    [Fact]
    public void AiSelectionPolishWindow_GatesCaptureOnMasterSwitchThenSkillToggleBeforeAnyCaptureOrNetwork()
    {
        string window = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiSelectionPolishWindow.cs"));

        Assert.Contains("App.Settings.Ai.SelectionMenuEnabled", window);
        Assert.Contains("划词菜单已关闭", window);
        Assert.Contains("AiSelectionSkills.IsEnabled(App.Settings, _skill.Id)", window);
        Assert.Contains("该技能已在设置中关闭", window);

        int masterCheckIdx = window.IndexOf("App.Settings.Ai.SelectionMenuEnabled", StringComparison.Ordinal);
        int skillCheckIdx = window.IndexOf("AiSelectionSkills.IsEnabled(App.Settings, _skill.Id)", StringComparison.Ordinal);
        int captureCallIdx = window.IndexOf("_captureService.CaptureAsync", StringComparison.Ordinal);

        Assert.True(masterCheckIdx >= 0 && skillCheckIdx > masterCheckIdx,
            "master SelectionMenuEnabled gate must be checked before the per-skill IsEnabled gate");
        Assert.True(captureCallIdx > skillCheckIdx,
            "both gates must be checked before SelectedTextCaptureService.CaptureAsync is called");
    }

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
