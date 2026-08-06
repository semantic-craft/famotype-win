using Famo.Settings.Core.Ai;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class LatestMacParityContractTests
{
    [Fact]
    public void CandidatePanel_UsesClickableScrollRowsAndFlatSelection()
    {
        string page = RepoText("native/windows-tsf-famo/settings-winui/FamoSettings/Views/CandidatePage.cs");
        string writer = RepoText("native/windows-tsf-famo/settings-winui/FamoSettings.Core/ConfigWriter.cs");
        string window = RepoText("native/windows-tsf-famo/runtime-protocol/src/candidate_window.cpp");
        string keyPath = RepoText("native/windows-tsf-famo/text-service/src/text_service_key.cpp");
        string paint = RepoText("native/windows-tsf-famo/famo-candidate-ui/render/famo_paint.cpp");

        Assert.Contains("\"自动\", \"横排\", \"竖排\", \"卷轴\"", page);
        Assert.DoesNotContain("\"预览后页\"", page);
        Assert.DoesNotContain("不可点", page);
        Assert.Contains("a.Orientation == \"scroll\"", writer);
        Assert.Contains("PreviewSelectionAt", window);
        Assert.Contains("SendPreviewSelection(window, request,", window);
        Assert.Contains("SendPreviewSelectionToOwner(", window);
        Assert.Contains("notifications->selection_target", window);
        Assert.Contains("AcquirePipeClientIdentityLease(selection_owner)", window);
        Assert.Contains("reinterpret_cast<WPARAM>(source_window)", window);
        Assert.Contains("WM_COPYDATA", window);
        Assert.DoesNotContain("SendInput", window);
        Assert.Contains("in_process_source && current_page", keyPath);
        Assert.Contains("Command::SelectCandidate;", keyPath);
        Assert.Contains("Command::SelectCandidateAbsolute", keyPath);
        Assert.Contains("AcquirePipeClientIdentityLease(runtime_identity)", keyPath);
        Assert.Contains("SelectionCapabilityMatches", keyPath);
        Assert.DoesNotContain("top_light", paint);
        Assert.DoesNotContain("edge_light", paint);
    }

    [Fact]
    public void SelectionToolbox_HasCurrentSevenActionsAndDoubleAltSummon()
    {
        Assert.Equal(
            ["润色", "来源核验", "辅助检索", "规范排版", "划词翻译", "提示词优化"],
            AiSelectionSkills.BuiltIn.Select(skill => skill.Title).ToArray());

        string page = RepoText("native/windows-tsf-famo/settings-winui/FamoSettings/Views/SkillsPage.cs");
        string window = RepoText("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiConversationWindow.cs");
        string statusHeader = RepoText("native/windows-tsf-famo/runtime-protocol/include/famo_status_ui.h");
        string statusSource = RepoText("native/windows-tsf-famo/runtime-protocol/src/status_ui.cpp");

        Assert.Contains("划词工具箱", page);
        Assert.Contains("快速双击 Alt", page);
        Assert.Contains("AskAnythingSkillEnabled", page);
        Assert.DoesNotContain("公文排版", page);
        Assert.Contains("BuildToolboxSkills", window);
        Assert.Contains("Grid.SetColumn", window);
        Assert.Contains("App.ShowAiSelectionSkill", window);
        Assert.Contains("skill.Id is \"translation\" or \"research-assist\"", window);
        Assert.Contains("await RunToolboxSkillAsync", window);
        Assert.Contains("_result.Text = result", window);
        Assert.Contains("任意提问", window);
        Assert.Contains("AltDoubleTapDetector", statusHeader);
        Assert.Contains("GlobalHotKeyBindingMatches", statusHeader);
        Assert.Contains("OpenPage(L\"ai-chat\")", statusSource);
        Assert.Contains("quick-phrase-picker", statusSource);
        Assert.Contains("state_->focused.store", statusSource);
    }

    private static string RepoText(string relativePath) =>
        File.ReadAllText(RepoFile(relativePath));

    private static string RepoFile(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
            dir = Directory.GetParent(dir)?.FullName;
        }
        throw new FileNotFoundException(relativePath);
    }
}
