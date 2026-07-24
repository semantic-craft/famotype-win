using Xunit;

namespace Famo.Settings.Tests;

public sealed class InputMethodWinuiContractTests
{
    [Fact]
    public void InputMethodPage_ChangesWriteStoreAndTriggerSelectSchema()
    {
        // InputMethodPage.cs was deleted (unreachable orphan); the same method-switch
        // wiring lives in the canonical reachable page, KeyboardPage.cs.
        string page = File.ReadAllText(SettingsFile("FamoSettings", "Views", "KeyboardPage.cs"));
        string app = File.ReadAllText(SettingsFile("FamoSettings", "App.xaml.cs"));
        string deploy = File.ReadAllText(SettingsFile("FamoSettings.Core", "DeployService.cs"));

        Assert.Contains("(\"拼音输入\", \"pinyin\")", page);
        Assert.Contains("(\"双拼输入\", \"double_pinyin\")", page);
        Assert.Contains("(\"五笔输入\", \"wubi\")", page);
        Assert.Contains("(\"小鹤双拼\", \"flypy\")", page);
        Assert.Contains("(\"拼音加加双拼\", \"jiajia\")", page);
        Assert.Contains("App.SaveAndApplySchema();", page);

        Assert.Contains("ConfigWriter.WriteSelectSchema(Settings, FamoPaths.FamoDir)", app);
        Assert.Contains("DeployService.SelectSchema()", app);
        Assert.Contains("public const string SelectSchemaArgs = \"--control select-schema\";", deploy);
        Assert.Contains("Run(SelectSchemaArgs, baseDirectory)", deploy);
    }

    [Fact]
    public void SelectSchemaPatch_RegistersIpcAndSessionReplay()
    {
        string patch = File.ReadAllText(WeaselForkFile("features", "select-schema.patch"));
        string apply = File.ReadAllText(WeaselForkFile("apply-famo-features.ps1"));

        Assert.Contains("WEASEL_IPC_SELECT_SCHEMA", patch);
        Assert.Contains("L\"/selectschema", patch);
        Assert.Contains("famo-select-schema.txt", patch);
        Assert.Contains("RimeWithWeaselHandler::SelectSchema", patch);
        Assert.Contains("rime_api->select_schema(session_id, sel.c_str())", patch);
        Assert.Contains("rime_api->select_schema(to_session_id(pair.first), id.c_str())", patch);

        int instant = apply.IndexOf("features/instant-apply.patch", StringComparison.Ordinal);
        int select = apply.IndexOf("features/select-schema.patch", StringComparison.Ordinal);
        Assert.True(instant >= 0, "instant-apply.patch must remain in the feature chain");
        Assert.True(select > instant, "select-schema.patch must run after instant-apply.patch");
    }

    [Fact]
    public void FeaturePatchDryRun_UsesTempCopyInsteadOfMutatingCallerWorktree()
    {
        string apply = File.ReadAllText(WeaselForkFile("apply-famo-features.ps1"));

        Assert.Contains("famo-features-dryrun-", apply);
        Assert.Contains("Copy-Item -LiteralPath $UpstreamDir -Destination $patchUpstreamDir -Recurse -Force", apply);
        Assert.Contains("Push-Location $patchUpstreamDir", apply);
        Assert.DoesNotContain("git apply --reverse", apply);
    }

    private static string SettingsFile(params string[] parts)
    {
        string[] path = new[] { "native", "windows-tsf-famo", "settings-winui" }.Concat(parts).ToArray();
        return RepoFile(path);
    }

    private static string WeaselForkFile(params string[] parts)
    {
        string[] path = new[] { "native", "windows-tsf-famo", "weasel-fork" }.Concat(parts).ToArray();
        return RepoFile(path);
    }

    private static string RepoFile(string[] pathParts)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(new[] { dir }.Concat(pathParts).ToArray());
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate {Path.Combine(pathParts)}");
    }
}
