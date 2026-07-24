using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class SettingsNavigationContractTests
{
    [Fact]
    public void VisiblePages_DescribeCurrentWindowsSettingsContract()
    {
        string[] titles = SettingsNavigation.VisiblePages.Select(page => page.Title).ToArray();
        string[] ids = SettingsNavigation.VisiblePages.Select(page => page.Id).ToArray();

        Assert.Equal(
            [
                "键盘输入",
                "快捷键设置",
                "候选窗设置",
                "快捷短语",
                "剪贴板",
                "技能平台",
                "AI 助手",
                "悬浮状态栏",
                "皮肤外观",
                "关于",
            ],
            titles);

        Assert.DoesNotContain("input", ids);
        Assert.DoesNotContain("convenience", ids);
        Assert.DoesNotContain("switches", ids);
        Assert.DoesNotContain("deploy", ids);
        Assert.DoesNotContain("schemes", ids);
        Assert.DoesNotContain("project", ids);
        Assert.DoesNotContain("prompt-library", ids);
        Assert.Contains("skills", ids);
        Assert.Contains("ai", ids);
        Assert.Contains("status-bar", ids);
    }

    [Fact]
    public void VisiblePages_AllHaveNonEmptyGlyph()
    {
        Assert.All(SettingsNavigation.VisiblePages, page => Assert.False(string.IsNullOrEmpty(page.Glyph)));
    }

    [Theory]
    [InlineData(null, "keyboard")]
    [InlineData("", "keyboard")]
    [InlineData("input", "keyboard")]
    [InlineData("convenience", "keyboard")]
    [InlineData("switches", "keyboard")]
    [InlineData("deploy", "about")]
    [InlineData("quick-phrases", "quick-phrases")]
    [InlineData("prompt-library", "prompt-library")]
    [InlineData("skills", "skills")]
    [InlineData("ai", "ai")]
    [InlineData("status-bar", "status-bar")]
    [InlineData("project", "keyboard")]
    [InlineData("unknown", "keyboard")]
    public void PageIds_ResolveToVisibleOrHiddenWindowsPages(string? startPage, string expected)
    {
        Assert.Equal(expected, SettingsNavigation.ResolvePageId(startPage));
    }

    [Theory]
    [InlineData("prompt-library", "ai")]
    public void HiddenWindowsPages_HighlightTheirSemanticMacParent(string startPage, string expected)
    {
        Assert.Equal(expected, SettingsNavigation.VisibleParentPageId(startPage));
    }

    [Fact]
    public void MainWindow_UsesNavigationContractInsteadOfLegacyGroupedPages()
    {
        string mainWindow = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/MainWindow.xaml.cs"));

        Assert.Contains("SettingsNavigation.VisiblePages", mainWindow);
        Assert.Contains("SettingsNavigation.ResolvePageId", mainWindow);
        Assert.Contains("SettingsNavigation.VisibleParentPageId", mainWindow);
        Assert.DoesNotContain("\"输入方式\"", mainWindow);
        Assert.DoesNotContain("\"输入便利\"", mainWindow);
        Assert.DoesNotContain("\"输入开关\"", mainWindow);
        Assert.DoesNotContain("\"部署\"", mainWindow);
        Assert.DoesNotContain("\"剪贴板历史\"", mainWindow);
        Assert.Contains("\"prompt-library\" => new PromptLibraryPage()", mainWindow);
        Assert.DoesNotContain("\"project\" => new ProjectPage()", mainWindow);
        Assert.Contains("\"skills\" => new SkillsPage()", mainWindow);
        Assert.Contains("\"ai\" => new AiPage()", mainWindow);
        Assert.Contains("\"status-bar\" => new StatusBarPage()", mainWindow);
        Assert.DoesNotContain("new NavDef(\"input\"", mainWindow);
        Assert.DoesNotContain("new NavDef(\"convenience\"", mainWindow);
        Assert.DoesNotContain("new NavDef(\"switches\"", mainWindow);
        Assert.DoesNotContain("new NavDef(\"deploy\"", mainWindow);
        Assert.DoesNotContain("\"project\" => new SkillsPage()", mainWindow);
    }

    [Fact]
    public void MainWindow_NavigationItemsAreAccessibleButtons()
    {
        string mainWindow = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/MainWindow.xaml.cs"));

        Assert.Contains("private Button BuildNavItem", mainWindow);
        Assert.Contains("AutomationProperties.SetName(root, def.Title)", mainWindow);
        Assert.Contains("root.Click += (_, _) => Select(def.Id);", mainWindow);
        Assert.DoesNotContain("root.PointerPressed", mainWindow);
    }

    [Fact]
    public void LegacyProjectPage_IsNotRoutableOrInstantiated()
    {
        string mainWindow = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/MainWindow.xaml.cs"));

        Assert.Equal("keyboard", SettingsNavigation.ResolvePageId("project"));
        Assert.DoesNotContain("ProjectPage", mainWindow);
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
