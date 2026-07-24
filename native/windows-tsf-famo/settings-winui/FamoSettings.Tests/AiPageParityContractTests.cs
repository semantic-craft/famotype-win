using Xunit;

namespace Famo.Settings.Tests;

public sealed class AiPageParityContractTests
{
    [Fact]
    public void AiPage_MatchesLatestMacSettingsStructureWithoutSceneVocabulary()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiPage.cs"));

        Assert.Contains("AI 助手", page);
        Assert.Contains("本地 Rime", page);
        Assert.Contains("供应商（配一个就行）", page);

        Assert.Contains("DeepSeek", page);
        Assert.Contains("OpenAI", page);
        Assert.Contains("Google Gemini", page);
        Assert.Contains("小米 MiMo", page);
        Assert.Contains("火山引擎 · 豆包 Seed", page);
        Assert.Contains("+ 自定义（OpenAI 兼容）", page);
        Assert.Contains("Chat · V4 Flash", page);
        Assert.Contains("Reasoner · V4 Pro", page);

        // 云端 AI（全局）卡已搬到 SkillsPage.cs（技能平台），AiPage.cs 只剩供应商配置。
        Assert.DoesNotContain("云端 AI（全局）", page);
        Assert.DoesNotContain("启用云端 AI（划词润色 / 任意提问）", page);
        Assert.DoesNotContain("普通输入", page);
        Assert.DoesNotContain("场景词库", page);
        Assert.DoesNotContain("术语", page);
        Assert.DoesNotContain("候选条数", page);
    }

    [Fact]
    public void AiPage_ProviderControlsAreBackedByRealStoreAndSecretStore()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiPage.cs"));
        string store = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/Ai/AiProviderProfileStore.cs"));
        string paths = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/FamoPaths.cs"));

        Assert.Contains("AiProviderProfileStore", page);
        Assert.Contains("AiProviderProfileService", page);
        Assert.Contains("WindowsCredentialSecretStore", page);
        Assert.Contains("SaveProviderProfile", page);
        Assert.Contains("SetDefaultProvider", page);
        Assert.Contains("DeleteProviderProfile", page);
        Assert.Contains("RenderProviderList", page);
        Assert.Contains("SecretName", store);
        Assert.Contains("AiProviderProfilesFile =>", paths);

        string persistedProfileModel = store[..store.IndexOf("public sealed class AiProviderProfileDraft", StringComparison.Ordinal)];
        Assert.DoesNotContain("ApiKey", persistedProfileModel);
        Assert.DoesNotContain("sk-test", page);
    }

    [Fact]
    public void AiPage_ProviderActionsExposeUserVisibleStatus()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiPage.cs"));

        Assert.Contains("_status", page);
        Assert.Contains("密钥已保存", page);
        Assert.Contains("已设为默认 AI 供应商", page);
        Assert.Contains("AI 供应商已删除", page);
        Assert.Contains("不会显示已保存的 API Key", page);
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
