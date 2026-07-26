using Xunit;

namespace Famo.Settings.Tests;

public sealed class SkillsPageParityContractTests
{
    [Fact]
    public void SkillsPage_ExposesRealSelectionMenuAndBuiltInSkillDeepLinks()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/SkillsPage.cs"));
        string skills = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/Ai/AiSelectionPolishService.cs"));
        string menuTest = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Test-FamoMenuParity.ps1"));
        string menuPatches =
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/status-bar.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/tray-options.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/language-bar-menu.patch"));

        Assert.Contains("技能平台", page);
        Assert.Contains("划词工具箱", page);
        Assert.Contains("启用划词工具箱", page);
        Assert.Contains("辅助功能权限", page);
        Assert.Contains("内置技能", page);
        Assert.Contains("快速双击 Alt", page);
        Assert.Contains("AiSelectionSkills.Polish", page);
        Assert.Contains("AiSelectionSkills.SourceCheck", page);
        Assert.Contains("AiSelectionSkills.ResearchAssist", page);
        Assert.Contains("润色", skills);
        Assert.Contains("ai-polish", skills);
        Assert.Contains("来源核验", skills);
        Assert.Contains("ai-source-check", skills);
        Assert.Contains("辅助检索", skills);
        Assert.Contains("ai-research", skills);
        Assert.DoesNotContain("AI 对话", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""ai-chat"")", menuPatches);
        Assert.DoesNotContain("AI 润色选中", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""ai-polish"")", menuPatches);
        Assert.DoesNotContain("来源核验", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""ai-source-check"")", menuPatches);
        Assert.DoesNotContain("辅助检索", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""ai-research"")", menuPatches);
        Assert.Contains("ID_WEASELTRAY_FAMO_STATUS_BAR", menuTest);
        Assert.Contains("显示悬浮状态条", menuTest);

        Assert.DoesNotContain("Windows 入口", page);
        Assert.DoesNotContain("兼容深链", page);
        Assert.DoesNotContain("status-bar -> skills", page);
        Assert.DoesNotContain("状态按钮", page);
        Assert.DoesNotContain("场景词库", page);
        Assert.DoesNotContain("术语", page);
        Assert.DoesNotContain("FamoSettings --page", page);
    }

    [Fact]
    public void SkillsPage_ExposesSelectionMenuMasterSwitchMovedCloudCardAndPerSkillToggles()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/SkillsPage.cs"));
        string skills = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/Ai/AiSelectionPolishService.cs"));

        // 划词菜单总开关：真实开关绑定 AiSettings.SelectionMenuEnabled，不再是静态 Value("已接入")。
        Assert.Contains("App.Settings.Ai.SelectionMenuEnabled", page);

        // 云端 AI（全局）卡从 AiPage.cs 搬来，位置在「划词菜单」卡之后、「内置技能」卡之前。
        Assert.Contains("云端 AI（全局）", page);
        Assert.Contains("启用云端 AI（划词润色 / 任意提问）", page);
        Assert.Contains("普通输入", page);
        Assert.Contains("App.Settings.Ai.CloudEnabled", page);

        int selectionMenuIdx = page.IndexOf("划词工具箱", StringComparison.Ordinal);
        int cloudCardIdx = page.IndexOf("云端 AI（全局）", StringComparison.Ordinal);
        int builtInIdx = page.IndexOf("内置技能", StringComparison.Ordinal);
        Assert.True(selectionMenuIdx >= 0, "missing 划词菜单 card");
        Assert.True(cloudCardIdx > selectionMenuIdx, "云端 AI（全局）must come after 划词菜单");
        Assert.True(builtInIdx > cloudCardIdx, "内置技能 must come after 云端 AI（全局）");

        // 当前 7 个动作各自有真实开关，且都真的落盘。
        Assert.Contains("App.Settings.Ai.AskAnythingSkillEnabled", page);
        Assert.Contains("App.Settings.Ai.PolishSkillEnabled", page);
        Assert.Contains("App.Settings.Ai.SourceCheckSkillEnabled", page);
        Assert.Contains("App.Settings.Ai.ResearchAssistSkillEnabled", page);
        Assert.Contains("App.Settings.Ai.PublishFormattingSkillEnabled", page);
        Assert.Contains("App.Settings.Ai.TranslationSkillEnabled", page);
        Assert.Contains("AiSelectionSkills.PublishFormatting", page);
        Assert.Contains("AiSelectionSkills.Translation", page);
        Assert.Contains("App.Store.Save(App.Settings)", page);

        int ask = page.IndexOf("SkillRow(\"任意提问\"", StringComparison.Ordinal);
        int source = page.IndexOf("SkillRow(AiSelectionSkills.SourceCheck", StringComparison.Ordinal);
        int research = page.IndexOf("SkillRow(AiSelectionSkills.ResearchAssist", StringComparison.Ordinal);
        int formatting = page.IndexOf("SkillRow(AiSelectionSkills.PublishFormatting", StringComparison.Ordinal);
        int translation = page.IndexOf("SkillRow(AiSelectionSkills.Translation", StringComparison.Ordinal);
        int polish = page.IndexOf("SkillRow(AiSelectionSkills.Polish", StringComparison.Ordinal);
        int prompt = page.IndexOf("SkillRow(AiSelectionSkills.PromptOptimize", StringComparison.Ordinal);
        Assert.True(ask < source && source < research && research < formatting &&
                    formatting < translation && translation < polish && polish < prompt,
            "技能平台顺序必须与当前 macOS 一致");

        Assert.Contains("规范排版", skills);
        Assert.Contains("ai-publish-formatting", skills);
        Assert.Contains("划词翻译", skills);
        Assert.Contains("ai-translation", skills);

        // 第 5 个内置技能：提示词优化（对齐 macOS FamoPromptOptimizer 的两态契约）。
        Assert.Contains("App.Settings.Ai.PromptOptimizeSkillEnabled", page);
        Assert.Contains("AiSelectionSkills.PromptOptimize", page);
        Assert.Contains("提示词优化", skills);
        Assert.Contains("成对 XML 标签", page);
        Assert.Contains("ai-prompt-optimize", skills);
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
