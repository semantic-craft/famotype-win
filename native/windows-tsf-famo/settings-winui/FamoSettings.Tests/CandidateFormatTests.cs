using System.Text.Json;
using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class CandidateFormatTests
{
    [Fact]
    public void DefaultAndSchema_ExposeCandidateFormat()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        Assert.Equal("full", settings.Appearance.CandidateFormat);

        using JsonDocument defaults = JsonDocument.Parse(SettingsStore.DefaultSettingsJson);
        JsonElement appearanceDefaults = defaults.RootElement.GetProperty("appearance");
        Assert.Equal("full", appearanceDefaults.GetProperty("candidateFormat").GetString());

        string schemaText = File.ReadAllText(RepoFile("native/windows-tsf-famo/famo-config/famo-settings.schema.json"));
        using JsonDocument schema = JsonDocument.Parse(schemaText);
        JsonElement appearanceSchema = schema.RootElement
            .GetProperty("properties").GetProperty("appearance");

        Assert.Contains(
            "candidateFormat",
            appearanceSchema.GetProperty("required").EnumerateArray().Select(x => x.GetString()));

        JsonElement candidateFormat = appearanceSchema
            .GetProperty("properties").GetProperty("candidateFormat");
        Assert.Equal(new[] { "full", "no_comment", "candidate_only" },
            candidateFormat.GetProperty("enum").EnumerateArray().Select(x => x.GetString()).ToArray());
        Assert.Equal("instant", candidateFormat.GetProperty("x-famo-bucket").GetString());
    }

    [Theory]
    [InlineData("full", "label_format: \"%s.\"", "label_font_point: 13", "comment_font_point: 14.4")]
    [InlineData("no_comment", "label_format: \"%s.\"", "label_font_point: 13", "comment_font_point: 0")]
    [InlineData("candidate_only", "label_format: \"\"", "label_font_point: 0", "comment_font_point: 0")]
    public void StyleOverlay_MapsCandidateFormatToRealWeaselStyleKeys(
        string format,
        string labelFormat,
        string labelPoint,
        string commentPoint)
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Appearance.FontPoint = 18;
        settings.Appearance.CandidateFormat = format;

        string yaml = ConfigWriter.BuildStyleOverlay(settings);

        Assert.Contains(labelFormat, yaml);
        Assert.Contains(labelPoint, yaml);
        Assert.Contains(commentPoint, yaml);
        Assert.Contains("font_point: 18", yaml);
    }

    [Fact]
    public void WeaselCustomBaseline_AlsoCarriesCandidateFormatKeys()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Appearance.FontPoint = 16;
        settings.Appearance.CandidateFormat = "candidate_only";

        string yaml = ConfigWriter.BuildWeaselCustom(settings);

        Assert.Contains("label_format: \"\"", yaml);
        Assert.Contains("label_font_point: 0", yaml);
        Assert.Contains("comment_font_point: 0", yaml);
        Assert.Contains("font_point: 16", yaml);
        Assert.Contains("preset_color_schemes/shenda:", yaml);
    }

    [Fact]
    public void CandidatePage_RendersRealInstantControl()
    {
        string page = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/CandidatePage.cs"));

        Assert.Contains("候选格式", page);
        Assert.Contains("(\"标准\", \"full\")", page);
        Assert.Contains("(\"隐藏注释\", \"no_comment\")", page);
        Assert.Contains("(\"仅候选\", \"candidate_only\")", page);
        Assert.Contains("A.CandidateFormat", page);
        Assert.Contains("App.SaveAndApplyInstant();", page);
    }

    [Fact]
    public void DefaultAndSchema_ExposeInlineCandidatePreview()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        Assert.False(settings.Appearance.InlinePreedit);
        Assert.True(settings.Appearance.ShowPreedit);
        Assert.False(settings.Appearance.PreviewPages);
        Assert.Equal(2, settings.Appearance.PreviewRows);
        Assert.False(settings.Appearance.InlineCandidatePreview);

        using JsonDocument defaults = JsonDocument.Parse(SettingsStore.DefaultSettingsJson);
        JsonElement appearanceDefaults = defaults.RootElement.GetProperty("appearance");
        Assert.False(appearanceDefaults.GetProperty("inlinePreedit").GetBoolean());
        Assert.True(appearanceDefaults.GetProperty("showPreedit").GetBoolean());
        Assert.False(appearanceDefaults.GetProperty("previewPages").GetBoolean());
        Assert.Equal(2, appearanceDefaults.GetProperty("previewRows").GetInt32());
        Assert.False(appearanceDefaults.GetProperty("inlineCandidatePreview").GetBoolean());

        string schemaText = File.ReadAllText(RepoFile("native/windows-tsf-famo/famo-config/famo-settings.schema.json"));
        using JsonDocument schema = JsonDocument.Parse(schemaText);
        JsonElement appearanceSchema = schema.RootElement.GetProperty("properties").GetProperty("appearance");

        Assert.Contains(
            "inlinePreedit",
            appearanceSchema.GetProperty("required").EnumerateArray().Select(x => x.GetString()));
        Assert.Contains(
            "inlineCandidatePreview",
            appearanceSchema.GetProperty("required").EnumerateArray().Select(x => x.GetString()));
        Assert.Contains(
            "showPreedit",
            appearanceSchema.GetProperty("required").EnumerateArray().Select(x => x.GetString()));
        Assert.Contains(
            "previewPages",
            appearanceSchema.GetProperty("required").EnumerateArray().Select(x => x.GetString()));
        Assert.Contains(
            "previewRows",
            appearanceSchema.GetProperty("required").EnumerateArray().Select(x => x.GetString()));

        JsonElement inlinePreedit = appearanceSchema
            .GetProperty("properties").GetProperty("inlinePreedit");
        Assert.Equal("boolean", inlinePreedit.GetProperty("type").GetString());
        Assert.Equal("instant", inlinePreedit.GetProperty("x-famo-bucket").GetString());

        JsonElement inlineCandidatePreview = appearanceSchema
            .GetProperty("properties").GetProperty("inlineCandidatePreview");
        Assert.Equal("boolean", inlineCandidatePreview.GetProperty("type").GetString());
        Assert.Equal("instant", inlineCandidatePreview.GetProperty("x-famo-bucket").GetString());
    }

    [Theory]
    [InlineData(false, "composition")]
    [InlineData(true, "preview")]
    public void NativeStyleOverlayAndBaseline_MapInlineCandidatePreview(bool on, string expected)
    {
        // style/preedit_type 是上游 Weasel 真实字段（RimeWithWeasel.cpp _UpdateUIStyle 读取），
        // composition=原样回显、preview=回显当前候选的实际文字；两条链路（即时覆盖层 + 部署基线）都要写。
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Appearance.InlineCandidatePreview = on;

        string overlay = ConfigWriter.BuildStyleOverlay(settings);
        string baseline = ConfigWriter.BuildWeaselCustom(settings);

        Assert.Contains($"preedit_type: {expected}", overlay);
        Assert.Contains($"preedit_type: {expected}", baseline);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void NativeStyleOverlayAndBaseline_MapInlinePreedit(bool on)
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Appearance.InlinePreedit = on;

        string overlay = ConfigWriter.BuildStyleOverlay(settings);
        string baseline = ConfigWriter.BuildWeaselCustom(settings);

        Assert.Contains($"inline_preedit: {on.ToString().ToLowerInvariant()}", overlay);
        Assert.Contains($"inline_preedit: {on.ToString().ToLowerInvariant()}", baseline);
        Assert.Contains("preedit_type: composition", overlay);
        Assert.Contains("preedit_type: composition", baseline);
    }

    [Fact]
    public void NativeStyleOverlay_MapsPanelPreeditAndPreviewIndependently()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Appearance.InlinePreedit = true;
        settings.Appearance.ShowPreedit = false;
        settings.Appearance.PreviewPages = true;
        settings.Appearance.PreviewRows = 1;

        string overlay = ConfigWriter.BuildStyleOverlay(settings);
        string baseline = ConfigWriter.BuildWeaselCustom(settings);

        foreach (string yaml in new[] { overlay, baseline })
        {
            Assert.Contains("inline_preedit: true", yaml);
            Assert.Contains("show_preedit: false", yaml);
            Assert.Contains("preview_pages: true", yaml);
            Assert.Contains("preview_rows: 1", yaml);
        }
    }

    [Fact]
    public void CandidatePage_RefreshesLivePreviewAfterEveryInstantHandler()
    {
        string page = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/CandidatePage.cs"));

        // R1：即时显示/行为/形态控件每次 SaveAndApplyInstant 后都要原地刷新预览卡。
        int instantCalls = CountOccurrences(page, "App.SaveAndApplyInstant();");
        int refreshCalls = CountOccurrences(page, "RefreshPreview();");
        Assert.True(instantCalls >= 8, $"expected >=8 native instant-apply handlers in CandidatePage, found {instantCalls}");
        Assert.Equal(instantCalls, refreshCalls);
        Assert.Contains("_previewHost.Content = FamoPreview.Build();", page);
        Assert.Contains("A.InlineCandidatePreview", page);
        Assert.Contains("A.ShowPreedit", page);
        Assert.Contains("A.PreviewPages", page);
        Assert.Contains("A.PreviewRows", page);
    }

    [Fact]
    public void CandidatePage_RendersBackedLayoutControls()
    {
        string page = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/CandidatePage.cs"));

        Assert.Contains("候选窗形态", page);
        Assert.Contains("A.Layout.CornerRadius", page);
        Assert.Contains("A.Layout.Margin", page);
        Assert.Contains("App.SaveAndApplyInstant();", page);
    }

    [Fact]
    public void DefaultAndSchema_ExposeCandidateLayoutControls()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        Assert.Equal(13, settings.Appearance.Layout.CornerRadius);
        Assert.Equal(16, settings.Appearance.Layout.ShadowRadius);
        Assert.Equal(8, settings.Appearance.Layout.Margin);

        using JsonDocument defaults = JsonDocument.Parse(SettingsStore.DefaultSettingsJson);
        JsonElement layoutDefaults = defaults.RootElement.GetProperty("appearance").GetProperty("layout");
        Assert.Equal(13, layoutDefaults.GetProperty("cornerRadius").GetInt32());
        Assert.Equal(16, layoutDefaults.GetProperty("shadowRadius").GetInt32());
        Assert.Equal(8, layoutDefaults.GetProperty("margin").GetInt32());

        string schemaText = File.ReadAllText(RepoFile("native/windows-tsf-famo/famo-config/famo-settings.schema.json"));
        using JsonDocument schema = JsonDocument.Parse(schemaText);
        JsonElement layoutSchema = schema.RootElement
            .GetProperty("properties").GetProperty("appearance")
            .GetProperty("properties").GetProperty("layout");

        Assert.Contains("cornerRadius", layoutSchema.GetProperty("required").EnumerateArray().Select(x => x.GetString()));
        Assert.Contains("margin", layoutSchema.GetProperty("required").EnumerateArray().Select(x => x.GetString()));
        Assert.Equal("instant", layoutSchema.GetProperty("properties").GetProperty("cornerRadius").GetProperty("x-famo-bucket").GetString());
        Assert.Equal("instant", layoutSchema.GetProperty("properties").GetProperty("margin").GetProperty("x-famo-bucket").GetString());
    }

    [Fact]
    public void StyleOverlayAndBaseline_MapLayoutControlsToRealWeaselStyleKeys()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Appearance.Layout.CornerRadius = 14;
        settings.Appearance.Layout.Margin = 20;

        string overlay = ConfigWriter.BuildStyleOverlay(settings);
        string baseline = ConfigWriter.BuildWeaselCustom(settings);

        Assert.Contains("corner_radius: 14", overlay);
        Assert.Contains("margin_x: 20", overlay);
        Assert.Contains("margin_y: 20", overlay);
        Assert.Contains("corner_radius: 14", baseline);
        Assert.Contains("margin_x: 20", baseline);
        Assert.Contains("margin_y: 20", baseline);
    }

    [Fact]
    public void CandidatePage_PageSizeStepperSurfacesRealDeployResult()
    {
        string page = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/CandidatePage.cs"));
        string app = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));

        // R2：不再无脑显示"已应用"——禁用步进器 + 把真实 ReloadResult 交给统一终态反馈。
        Assert.Contains("ReloadResult result = App.SaveAndApplyDeploy();", page);
        Assert.Contains("App.ReportReloadResult(", page);
        Assert.Contains("result,", page);
        Assert.Contains("succeeded: \"候选个数已生效。\"", page);
        Assert.Contains("_pageSizeStepper.IsHitTestVisible = false;", page);
        Assert.Contains("public static ReloadResult SaveAndApplyDeploy()", app);
    }

    [Fact]
    public void ReloadFailureStatus_ExposesAnExplicitRetryAction()
    {
        string app = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));

        Assert.Contains("var retry = new Hyperlink();", app);
        Assert.Contains("DeployService.Retry(result.RequestId)", app);
        Assert.Contains("Text = \"重试\"", app);
    }

    [Fact]
    public void FamoPreview_ReflectsFontFaceAndCandidateFormat()
    {
        string preview = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Theming/FamoPreview.cs"));

        Assert.Contains("A.FontFace", preview);
        Assert.Contains("A.CandidateFormat", preview);
        Assert.Contains("A.Layout.CornerRadius", preview);
        Assert.Contains("A.Layout.Margin", preview);
        Assert.Contains("showComment", preview);
        Assert.Contains("showLabel", preview);
    }

    [Fact]
    public void CandidateRenderer_UsesFamoSkinColors()
    {
        string skin = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/famo-candidate-ui/skin/famo_skin.cpp"));
        string patch = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/weasel-fork/features/candidate-ui.patch"));

        Assert.Contains("0xFFA82C53u", skin);
        Assert.DoesNotContain("0xFF3A6EA5u", skin);
        Assert.Contains("s.hilited_candidate_text_color", patch);
        Assert.Contains("s.hilited_candidate_back_color", patch);
        Assert.DoesNotContain("k.hilited_back_color = (uint32_t)s.hilited_back_color;", patch);
    }

    [Fact]
    public void ReloadStyleOverlay_RefreshesExistingSessionStyles()
    {
        string instantPatch = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/weasel-fork/features/instant-apply.patch")).Replace("\r\n", "\n");
        string abiPatch = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/weasel-fork/features/engine-abi.patch")).Replace("\r\n", "\n");

        AssertPatchHunkCopiesStyleBeforeSchemaLoad(
            instantPatch,
            "void RimeWithWeaselHandler::ReloadStyleOverlay(WeaselSessionId ipc_id,",
            "+      pair.second.style = m_base_style;",
            "+      _LoadSchemaSpecificSettings(pair.first, std::string(status.schema_id));");
        AssertPatchHunkCopiesStyleBeforeSchemaLoad(
            abiPatch,
            "@@ -306,14 +328,13 @@ void RimeWithWeaselHandler::UpdateColorTheme(BOOL darkMode)",
            "+      pair.second.style = m_base_style;",
            "+      _LoadSchemaSpecificSettings(pair.first, schema_id);");
        AssertPatchHunkCopiesStyleBeforeSchemaLoad(
            abiPatch,
            "@@ -370,27 +390,29 @@ void RimeWithWeaselHandler::ReloadStyleOverlay(WeaselSessionId ipc_id,",
            "+      pair.second.style = m_base_style;",
            "+      _LoadSchemaSpecificSettings(pair.first, schema_id);");
    }

    [Fact]
    public void ReloadStyleOverlay_UsesAbiViewInsteadOfLegacyRimeStatus()
    {
        string abiPatch = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/weasel-fork/features/engine-abi.patch")).Replace("\r\n", "\n");

        int hunk = abiPatch.IndexOf("@@ -370,27 +390,29 @@ void RimeWithWeaselHandler::ReloadStyleOverlay(WeaselSessionId ipc_id,", StringComparison.Ordinal);
        Assert.True(hunk >= 0, "engine-abi.patch must update ReloadStyleOverlay");

        int nextHunk = abiPatch.IndexOf("\n@@ ", hunk + 1, StringComparison.Ordinal);
        string reloadHunk = abiPatch.Substring(hunk, (nextHunk < 0 ? abiPatch.Length : nextHunk) - hunk);
        Assert.Contains("+    if (_RefillAbiView(pair.second.engine_ctx)) {", reloadHunk);
        Assert.Contains("+      std::string schema_id =", reloadHunk);
        Assert.Contains("+          m_abi_view.schema_id.data ? m_abi_view.schema_id.data : \"\";", reloadHunk);
        Assert.Contains("+      _LoadSchemaSpecificSettings(pair.first, schema_id);", reloadHunk);
        Assert.Contains("-    if (rime_api->get_status(to_session_id(pair.first), &status)) {", reloadHunk);
    }

    [Fact]
    public void ReloadStyleOverlay_RedrawsActiveSessionAfterStyleRefresh()
    {
        string instantPatch = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/weasel-fork/features/instant-apply.patch")).Replace("\r\n", "\n");
        string abiPatch = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/weasel-fork/features/engine-abi.patch")).Replace("\r\n", "\n");

        Assert.Contains("@@ -261,6 +313,110 @@ void RimeWithWeaselHandler::UpdateColorTheme(BOOL darkMode)", instantPatch);
        Assert.Contains("+  if (m_active_session) {\n+    m_ui->style() = get_session_status(m_active_session).style;\n+    _UpdateUI(m_active_session);\n+  }", instantPatch);
        Assert.Contains("+      if (m_ui) {\n+        m_ui->style() = get_session_status(pair.first).style;\n+        _UpdateUI(pair.first);\n+      }", abiPatch);
        Assert.Contains("+    _RefillAbiView(get_session_status(ipc_id).engine_ctx);", abiPatch);
        Assert.DoesNotContain("redraw_session", abiPatch);
    }

    [Fact]
    public void ReloadStyleOverlay_ClientPollsStyleFileAndParsesStyleResponse()
    {
        string instantPatch = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/weasel-fork/features/instant-apply.patch")).Replace("\r\n", "\n");

        Assert.Contains("+  virtual void ReloadStyleOverlay(DWORD session_id = 0, EatLine eat = 0) {}", instantPatch);
        Assert.Contains("+void RimeWithWeaselHandler::ReloadStyleOverlay(WeaselSessionId ipc_id,\n+                                               EatLine eat) {", instantPatch);
        Assert.Contains("+  if (ipc_id && eat && m_session_status_map.find(ipc_id) != m_session_status_map.end())", instantPatch);
        Assert.Contains("+    _Respond(ipc_id, eat);", instantPatch);
        Assert.Contains("+  _SendMessage(WEASEL_IPC_RELOAD_STYLE, 0, session_id);", instantPatch);
        Assert.Contains("+    m_pRequestHandler->ReloadStyleOverlay(lParam, eat);", instantPatch);
        Assert.Contains("+#include <ResponseParser.h>", instantPatch);
        Assert.Contains("+    _StartStylePoll();", instantPatch);
        Assert.Contains("+    _StopStylePoll();", instantPatch);
        Assert.Contains("+  path.append(L\"\\\\Famo\\\\famo-style.yaml\");", instantPatch);
        Assert.Contains("+  _tsf->_ReloadStyleFromServerForCandidate();", instantPatch);
        Assert.Contains("+void WeaselTSF::_ReloadStyleFromServerForCandidate() {", instantPatch);
        Assert.Contains("+  m_client.ReloadStyle();", instantPatch);
        Assert.Contains("+  weasel::ResponseParser parser(NULL, context.get(), &_status, NULL,\n+                                &_cand->style());", instantPatch);
        Assert.Contains("+    _UpdateUI(*context, _status);", instantPatch);
        Assert.Contains("+  void _ReloadStyleFromServerForCandidate();", instantPatch);
        Assert.Contains("+  bool _style_polling = false;", instantPatch);
    }

    [Fact]
    public void FamoRuntimeStyleLoading_DoesNotFallBackToRoamingRime()
    {
        string identityScript = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/weasel-fork/apply-famo-identity.ps1"));
        string instantPatch = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/weasel-fork/features/instant-apply.patch"));

        Assert.Contains("$appdataOld = '%AppData%\\\\Rime'", identityScript);
        Assert.Contains("$appdataNew = $identity.registry.rimeUserDir.Replace('\\', '\\\\')", identityScript);
        Assert.Contains("_ReadFamoStyleOverlayYaml()", instantPatch);
        Assert.Contains("WeaselUserDataPath() / L\"famo-style.yaml\"", instantPatch);
        Assert.DoesNotContain("%AppData%\\\\Rime", instantPatch);
    }

    [Fact]
    public void WeaselLauncher_FindsInstalledSettingsSubdirectory()
    {
        string launcher = File.ReadAllText(
            RepoFile("native/windows-tsf-famo/weasel-fork/features/launch-settings.patch"));

        Assert.Contains(@"dir / L""settings"" / L""FamoSettings.exe""", launcher);
        Assert.Contains(@"dir / L""FamoSettings.exe""", launcher);
        Assert.Contains("fs::exists(settings)", launcher);
    }

    private static int CountOccurrences(string haystack, string needle)
    {
        int count = 0, idx = 0;
        while ((idx = haystack.IndexOf(needle, idx, StringComparison.Ordinal)) >= 0)
        {
            count++;
            idx += needle.Length;
        }
        return count;
    }

    private static void AssertPatchHunkCopiesStyleBeforeSchemaLoad(
        string patch,
        string hunkAnchor,
        string styleLine,
        string schemaLine)
    {
        int hunk = patch.IndexOf(hunkAnchor, StringComparison.Ordinal);
        Assert.True(hunk >= 0, $"Missing patch hunk anchor: {hunkAnchor}");

        int style = patch.IndexOf(styleLine, hunk, StringComparison.Ordinal);
        int schema = patch.IndexOf(schemaLine, hunk, StringComparison.Ordinal);
        Assert.True(style >= 0, $"Missing style refresh line after: {hunkAnchor}");
        Assert.True(schema >= 0, $"Missing schema load line after: {hunkAnchor}");
        Assert.True(style < schema, $"Session style must refresh before schema settings after: {hunkAnchor}");
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
