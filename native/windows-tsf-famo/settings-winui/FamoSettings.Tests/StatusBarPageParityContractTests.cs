using Xunit;

namespace Famo.Settings.Tests;

public sealed class StatusBarPageParityContractTests
{
    [Fact]
    public void StatusBarPage_ExposesWindowsOnlyStatusBarSurfaceAndRealMenuDeepLinks()
    {
        string candidate = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/CandidatePage.cs"));
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/StatusBarPage.cs"));
        string menuTest = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Test-FamoMenuParity.ps1"));
        string menuPatches =
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/status-bar.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/tray-options.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/language-bar-menu.patch"));

        Assert.Contains("Windows 悬浮状态栏", candidate);
        Assert.Contains("App.OpenSettingsPage(\"status-bar\")", candidate);
        Assert.DoesNotContain("FamoSettings --page status-bar", candidate);

        Assert.Contains("悬浮状态栏", page);
        Assert.Contains("悬浮状态条", page);
        Assert.Contains("输入焦点", page);
        Assert.Contains("自动显示", page);
        Assert.Contains("状态按钮", page);
        Assert.Contains("输入法设定放在最上面", page);
        Assert.Contains("三点菜单", page);
        Assert.Contains("输入区技能不放在这里", candidate);
        Assert.DoesNotContain("本地面板", page);
        Assert.DoesNotContain("表情符号", page);
        Assert.DoesNotContain("FamoSettings --page emoji", page);
        Assert.DoesNotContain("剪贴板", page);
        Assert.DoesNotContain("FamoSettings --page clipboard-panel", page);
        Assert.DoesNotContain("快捷短语", page);
        Assert.DoesNotContain("FamoSettings --page quick-phrase-picker", page);
        Assert.DoesNotContain("提示词库", page);
        Assert.DoesNotContain("FamoSettings --page prompt-library", page);
        Assert.DoesNotContain("快速插入提示词", page);
        Assert.DoesNotContain("FamoSettings --page prompt-picker", page);
        Assert.DoesNotContain("保存选中为提示词", page);
        Assert.DoesNotContain("FamoSettings --page prompt-save-selection", page);
        Assert.DoesNotContain("划词技能入口", page);
        Assert.DoesNotContain("AI 对话", page);
        Assert.DoesNotContain("FamoSettings --page ai-chat", page);
        Assert.DoesNotContain("AI 润色选中", page);
        Assert.DoesNotContain("FamoSettings --page ai-polish", page);
        Assert.DoesNotContain("来源核验", page);
        Assert.DoesNotContain("FamoSettings --page ai-source-check", page);
        Assert.DoesNotContain("辅助检索", page);
        Assert.DoesNotContain("FamoSettings --page ai-research", page);
        Assert.DoesNotContain("公文排版", page);
        Assert.DoesNotContain("FamoSettings --page ai-document-formatting", page);
        Assert.Contains("ID_WEASELTRAY_FAMO_STATUS_BAR", menuTest);
        Assert.Contains("显示悬浮状态条", menuTest);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""prompt-library"")", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""prompt-picker"")", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""prompt-save-selection"")", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""ai-polish"")", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""ai-source-check"")", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""ai-research"")", menuPatches);
        Assert.DoesNotContain("ID_WEASELTRAY_FAMO_AI_DOCUMENT_FORMATTING", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""ai-document-formatting"")", menuPatches);
        Assert.DoesNotContain(@"L""公文排版…""", menuPatches);

        Assert.DoesNotContain("场景词库", page);
        Assert.DoesNotContain("术语", page);
        Assert.DoesNotContain("语言栏菜单", candidate + page);
        Assert.DoesNotContain("WeaselServer", candidate + page);
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
