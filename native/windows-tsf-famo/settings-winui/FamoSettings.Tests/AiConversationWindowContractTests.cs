using Xunit;

namespace Famo.Settings.Tests;

public sealed class AiConversationWindowContractTests
{
    [Fact]
    public void AiConversationWindow_UsesRealClientAndCopyOnlyResult()
    {
        string window = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiConversationWindow.cs"));

        Assert.Contains("public sealed class AiConversationWindow : Window", window);
        Assert.Contains("AiChatClient", window);
        Assert.Contains("AiProviderProfileStore", window);
        Assert.Contains("WindowsCredentialSecretStore", window);
        Assert.Contains("SendAsync", window);
        Assert.Contains("复制结果", window);
        Assert.Contains("复制并关闭", window);
        Assert.Contains("_selectedText", window);
        Assert.Contains("FocusState.Programmatic", window);
        Assert.Contains("清空", window);
        Assert.Contains("Clipboard.SetContent", window);

        Assert.DoesNotContain("DeployService", window);
        Assert.DoesNotContain("ConfigWriter", window);
        Assert.DoesNotContain("TextInjector", window);
        Assert.DoesNotContain("SaveAndApply", window);
    }

    [Fact]
    public void App_DeepLinkCanOpenAiConversationWindow()
    {
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));

        Assert.Contains("_aiConversationWindow", app);
        Assert.Contains("IsAiChatPage", app);
        Assert.Contains("ShowAiConversation", app);
        Assert.Contains("ShowAiConversationForSelection", app);
        Assert.Contains("BuildSelectionCaptureService().CaptureAsync", app);
        Assert.Contains("TryMigrateQuickPhraseArtifacts", app);
        Assert.Contains("\"ai-chat\"", app);
    }

    [Fact]
    public void AiConversationWindow_DoesNotExposeSceneVocabulary()
    {
        string window = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiConversationWindow.cs"));

        Assert.DoesNotContain("场景词库", window);
        Assert.DoesNotContain("术语", window);
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
