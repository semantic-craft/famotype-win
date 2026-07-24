using Xunit;

namespace Famo.Settings.Tests;

public sealed class PanelSmoothnessContractTests
{
    [Fact]
    public void AuditDocumentRecordsNoActivateDpiStaleAndDiagnosticBoundaries()
    {
        string doc = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/PANEL-SMOOTHNESS.md"));

        Assert.Contains("No-Focus-Steal Contract", doc);
        Assert.Contains("First-Frame DPI Contract", doc);
        Assert.Contains("Stale-State Contract", doc);
        Assert.Contains("TSF UIElement Audit", doc);
        Assert.Contains("Diagnostic Boundary", doc);
        Assert.Contains("panelProbe", doc);
        Assert.Contains("SeparateFromRuntimeAndDeploy", doc);
    }

    [Fact]
    public void CandidateAndFloatingPanelsUseNoActivateAndDpiBeforeShow()
    {
        string candidatePatch = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/candidate-ui.patch"));
        string status = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/overlay/WeaselUI/FamoStatusBar.cpp"));
        string statusHeader = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/overlay/include/FamoStatusBar.h"));
        string popup = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/overlay/WeaselUI/FamoPopupPanel.cpp"));
        string popupHeader = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/overlay/include/FamoPopupPanel.h"));

        Assert.Contains("MonitorFromRect(m_inputPos", candidatePatch);
        Assert.Contains("SWP_NOACTIVATE", candidatePatch);

        Assert.Contains("WS_EX_NOACTIVATE", statusHeader);
        Assert.Contains("MA_NOACTIVATE", status);
        AssertContainsInOrder(status, "dpi_ = _CurrentDpi();", "_LayoutButtons();", "SetWindowPos(HWND_TOPMOST", "SWP_NOACTIVATE", "ShowWindow(SW_SHOWNOACTIVATE)");

        Assert.Contains("WS_EX_NOACTIVATE", popupHeader);
        Assert.Contains("MA_NOACTIVATE", popup);
        AssertContainsInOrder(popup, "dpi_ = _CurrentDpi();", "_Layout();", "SetWindowPos(HWND_TOPMOST", "SWP_NOACTIVATE", "ShowWindow(SW_SHOWNOACTIVATE)");
    }

    [Fact]
    public void StaleStateTriggersHideOrClearPanelState()
    {
        string candidate = File.ReadAllText(RepoFile("native/windows-tsf-famo/text-service/src/candidate_ui_element.cpp"));
        string enginePatch = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/engine-abi.patch"));
        string statusPatch = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/status-bar.patch"));
        string interaction = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/overlay/include/FamoStatusBarInteraction.h"));
        string popup = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/overlay/WeaselUI/FamoPopupPanel.cpp"));

        AssertContainsInOrder(candidate, "if (converted.empty())", "candidates_.clear();", "End();");
        Assert.Contains("void CandidateUiElement::End()", candidate);
        Assert.Contains("m_ui->Hide();", enginePatch);
        Assert.Contains("weasel_status.disabled = m_disabled;", enginePatch);
        Assert.Contains("OnFocusOut", statusPatch);
        Assert.Contains("m_status_bar.HideBar();", statusPatch);
        Assert.Contains("CaptureChanged()", interaction);
        Assert.Contains("hover_index_ = -1;", interaction);
        Assert.Contains("_BeginClose();", popup);
    }

    [Fact]
    public void TsfUiElementSupportAndIntentionalGapsAreRecorded()
    {
        string candidate = File.ReadAllText(RepoFile("native/windows-tsf-famo/text-service/src/candidate_ui_element.cpp"));
        string header = File.ReadAllText(RepoFile("native/windows-tsf-famo/text-service/src/candidate_ui_element.h"));
        string patch = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/instant-apply.patch"));
        string audit = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/PANEL-SMOOTHNESS.md"));

        Assert.Contains("ITfCandidateListUIElement", header);
        Assert.Contains("TF_CLUIE_COUNT", candidate);
        Assert.Contains("TF_CLUIE_SELECTION", candidate);
        Assert.Contains("TF_CLUIE_STRING", candidate);
        Assert.Contains("TF_CLUIE_CURRENTPAGE", candidate);
        Assert.Contains("GetCount", candidate);
        Assert.Contains("GetSelection", candidate);
        Assert.Contains("GetString", candidate);
        Assert.Contains("GetPageIndex", candidate);
        Assert.Contains("BeginUIElement(this, &allowed, &element_id_)", candidate);
        Assert.Contains("UpdateUIElement(element_id_)", candidate);
        Assert.Contains("EndUIElement(element_id_)", candidate);
        Assert.Contains("_UpdateUIElement();", patch);

        foreach (string facet in new[] { "Count", "Selection", "Strings", "Page index", "Show mode", "UI-less mode" })
        {
            Assert.Contains(facet, audit);
        }

        Assert.Contains("Partial compatibility", audit);
        Assert.Contains("Labels/comments", audit);
    }

    private static void AssertContainsInOrder(string text, params string[] needles)
    {
        int position = 0;
        foreach (string needle in needles)
        {
            int found = text.IndexOf(needle, position, StringComparison.Ordinal);
            Assert.True(found >= 0, $"Expected to find '{needle}' after offset {position}.");
            position = found + needle.Length;
        }
    }

    private static string RepoFile(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate {relativePath}");
    }
}
