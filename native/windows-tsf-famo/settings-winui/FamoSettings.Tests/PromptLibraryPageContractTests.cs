using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class PromptLibraryPageContractTests
{
    [Fact]
    public void Navigation_KeepsPromptLibraryUnderAiWithoutTopLevelSidebarSlot()
    {
        SettingsPageDef[] pages = SettingsNavigation.VisiblePages.ToArray();

        Assert.Contains(pages, p => p.Id == "ai" && p.Badge == "智" && p.Title == "AI 助手");
        Assert.DoesNotContain(pages, p => p.Id == "prompt-library");
        Assert.Equal("prompt-library", SettingsNavigation.ResolvePageId("prompt-library"));
        Assert.Equal("ai", SettingsNavigation.VisibleParentPageId("prompt-library"));
    }

    [Fact]
    public void PromptLibraryPage_IsLocalStoreBackedAndSeparateFromQuickPhrases()
    {
        string paths = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/FamoPaths.cs"));
        string mainWindow = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/MainWindow.xaml.cs"));
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/PromptLibraryPage.cs"));

        Assert.Contains("PromptLibraryFile =>", paths);
        Assert.Contains("\"prompt-library\" => new PromptLibraryPage()", mainWindow);
        Assert.Contains("PromptLibraryStore", page);
        Assert.Contains("PromptVariableParser.Extract", page);
        Assert.Contains("ImportJson", page);
        Assert.Contains("ExportJson", page);
        Assert.Contains("App.ShowPromptPicker", page);
        Assert.Contains("App.ShowPromptSaveSelection", page);
        Assert.Contains("includeDisabled: true", page);
        Assert.Contains("AcceptsReturn = true", page);
        Assert.Contains("不进入 Rime 热路径", page);
        Assert.Contains("不自动同步、不上传、不触发 AI 请求", page);
        Assert.DoesNotContain("QuickPhraseStore", page);
        Assert.DoesNotContain("SaveAndApplyDeploy", page);
    }

    [Fact]
    public void PromptPicker_HasDeepLinkVariableFillAndClipboardPasteInsertion()
    {
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));
        string picker = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/PromptPickerWindow.cs"));
        string insertion = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Interop/ClipboardPasteTextInsertion.cs"));

        Assert.Contains("IsPromptPickerPage", app);
        Assert.Contains("prompt-picker", app);
        Assert.Contains("ShowPromptPicker", app);
        Assert.Contains("TextInsertionServices.ClipboardPasteForForegroundTarget()", app);

        Assert.Contains("PromptLibraryStore", picker);
        Assert.Contains("PromptVariableParser.Extract", picker);
        Assert.Contains("PromptRenderer.Render", picker);
        Assert.Contains("Text = variable.DefaultValue ?? string.Empty", picker);
        Assert.Contains("Variable", picker);
        Assert.Contains("ITextInsertionService", picker);

        Assert.Contains("WindowsClipboardTextBridge", insertion);
        Assert.Contains("ClipboardPasteInsertionService", insertion);
        Assert.Contains("SendInputPasteCommandSender", insertion);
        Assert.Contains("VK_CONTROL", insertion);
        Assert.DoesNotContain("ClipboardHistoryStore", insertion);
    }

    [Fact]
    public void SaveSelectionAsPrompt_UsesSelectionCaptureAndPrefillsPromptLibrary()
    {
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/PromptLibraryPage.cs"));
        string skills = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/SkillsPage.cs"));

        Assert.Contains("prompt-save-selection", app);
        Assert.Contains("BuildSelectionCaptureService", app);
        Assert.Contains("SelectedTextCaptureService", app);
        Assert.Contains("WindowsFocusedTextSelectionReader", app);
        Assert.Contains("ClipboardCopySelectionReader", app);
        Assert.Contains("TakePendingPromptContent", page);
        Assert.Contains("SuggestedTitle", page);
        Assert.DoesNotContain("AiSelectionSkillService", page);

        Assert.Contains("App.OpenSettingsPage(\"prompt-library\")", skills);
        Assert.Contains("App.ShowPromptPicker", skills);
        Assert.Contains("App.ShowPromptSaveSelection", skills);
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
