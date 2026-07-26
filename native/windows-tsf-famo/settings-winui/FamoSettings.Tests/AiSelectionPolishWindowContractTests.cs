using Xunit;

namespace Famo.Settings.Tests;

public sealed class AiSelectionPolishWindowContractTests
{
    [Fact]
    public void AiSelectionPolishWindow_CapturesSelectionRunsAiAndOffersSafeReplacementForRewriteSkills()
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
        Assert.Contains("ITextInsertionService", window);
        Assert.Contains("确认替换原选区", window);
        Assert.Contains("CanReplaceSelection", window);
        Assert.Contains("InsertAsync", window);

        Assert.DoesNotContain("TextInjector", window);
        Assert.DoesNotContain("DeployService", window);
        Assert.DoesNotContain("SaveAndApply", window);
    }

    [Fact]
    public void Toolbox_KeepsTranslationAndResearchInlineButRoutesOtherSkillsToIndependentWindows()
    {
        string toolbox = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiConversationWindow.cs"));
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));

        Assert.Contains("Grid.SetRow", toolbox);
        Assert.Contains("Grid.SetColumn", toolbox);
        Assert.Contains("skill.Id is \"translation\" or \"research-assist\"", toolbox);
        Assert.Contains("RunToolboxSkillAsync", toolbox);
        Assert.Contains("_turns.Add", toolbox);
        Assert.Contains("可继续追问或选择其他技能", toolbox);
        Assert.Contains("App.ShowAiSelectionSkill(skill, _selectedText!, _replacement, _focusTarget)", toolbox);
        Assert.Contains("Close();", toolbox);
        Assert.Contains("任意提问", toolbox);
        Assert.Contains("ShowCapturedAiSelectionSkillAsync", app);
        Assert.Contains("LoadCapturedSelection", app);
    }

    [Fact]
    public void SelectionWindows_RestoreOnlyTheCapturedLiveHost()
    {
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));
        string toolbox = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiConversationWindow.cs"));
        string skill = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiSelectionPolishWindow.cs"));
        string target = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/Insertion/WindowsForegroundWindowTarget.cs"));

        int selectionFlow = app.IndexOf("ShowAiConversationForSelectionAsync", StringComparison.Ordinal);
        int captureTarget = app.IndexOf("WindowsForegroundWindowTarget.CaptureForeground()", selectionFlow, StringComparison.Ordinal);
        int clipboardFallback = app.IndexOf("CaptureSelectionForReplacementAsync()", selectionFlow, StringComparison.Ordinal);
        Assert.True(captureTarget >= 0 && captureTarget < clipboardFallback);
        Assert.Contains("Closed += (_, _) => _focusTarget?.TryRestore()", toolbox);
        Assert.Contains("Closed += (_, _) => _focusTarget?.TryRestore()", skill);
        Assert.Contains("GetWindowThreadProcessId", target);
        Assert.Contains("process.StartTime", target);
        Assert.Contains("GetClassNameW", target);
        Assert.Contains("IsStillValid() && SetForegroundWindow", target);
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
        Assert.Contains("ai-publish-formatting", File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/Ai/AiSelectionPolishService.cs")));
        Assert.Contains("ai-translation", File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/Ai/AiSelectionPolishService.cs")));
    }

    [Fact]
    public void AiSelectionPolishWindow_GatesCaptureOnSkillToggleBeforeAnyCaptureOrNetwork()
    {
        string window = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiSelectionPolishWindow.cs"));

        Assert.DoesNotContain("App.Settings.Ai.SelectionMenuEnabled", window);
        Assert.Contains("AiSelectionSkills.IsEnabled(App.Settings, _skill.Id)", window);
        Assert.Contains("该技能已在设置中关闭", window);

        int skillCheckIdx = window.IndexOf("AiSelectionSkills.IsEnabled(App.Settings, _skill.Id)", StringComparison.Ordinal);
        int captureCallIdx = window.IndexOf("_captureService.CaptureAsync", StringComparison.Ordinal);

        Assert.True(skillCheckIdx >= 0 && captureCallIdx > skillCheckIdx,
            "the per-skill gate must be checked before SelectedTextCaptureService.CaptureAsync is called");
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
