using System.Text.Json;
using System.Text.RegularExpressions;
using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>法墨 TIP 串与安装/卸载链路接线的契约锁(注册 ≠ 进列表,见 InputMethodList)。</summary>
public sealed class InputMethodListTests
{
    [Fact]
    public void FamoTip_IsWellFormed_ZhCnLangidPlusTwoGuids()
    {
        Assert.Matches(
            new Regex(@"^0804:\{[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}\}\{[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}\}$"),
            InputMethodList.FamoTip);
    }

    [Fact]
    public void FamoTip_MatchesFamoIdentityJson_SingleSourceOfTruth()
    {
        using JsonDocument doc = JsonDocument.Parse(
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/famo-identity.json")));
        JsonElement guids = doc.RootElement.GetProperty("guids");
        string clsid = guids.GetProperty("clsidTextService").GetString()!;
        string profile = guids.GetProperty("guidProfile").GetString()!;

        Assert.Equal(
            $"0804:{{{clsid}}}{{{profile}}}".ToUpperInvariant(),
            InputMethodList.FamoTip.ToUpperInvariant());
    }

    [Fact]
    public void SeedOnlyPath_AddsFamoToUserInputList_AsOriginalUser()
    {
        string program = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings/Program.cs"));

        // 加列表在 --seed-only(首启 runasoriginaluser 链路)内,不在提权安装段。
        int seed = program.IndexOf("private static int RunSeedOnly", StringComparison.Ordinal);
        int ensure = program.IndexOf("InputMethodList.EnsureFamoInUserList()", seed, StringComparison.Ordinal);
        Assert.True(seed >= 0 && ensure > seed,
            "RunSeedOnly must call InputMethodList.EnsureFamoInUserList() (registration alone does not add the IME to the user's input list)");

        Assert.Contains("--remove-input-tip", program);
        Assert.Contains("InputMethodList.RemoveFamoFromUserList()", program);
        Assert.Contains("--add-input-tip", program);
        Assert.Contains("--no-activate", program);
    }

    [Fact]
    public void SeedOnlyPath_ActivatesFamoAfterAddingToUserInputList()
    {
        string program = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings/Program.cs"));

        int seed = program.IndexOf("private static int RunSeedOnly", StringComparison.Ordinal);
        int ensure = program.IndexOf("InputMethodList.EnsureFamoInUserList()", seed, StringComparison.Ordinal);
        int activate = program.IndexOf("InputMethodList.ActivateFamoForCurrentDesktop()", seed, StringComparison.Ordinal);

        Assert.True(ensure >= 0 && activate > ensure,
            "--seed-only must activate Famo only after adding it to the current user's input list");
    }

    [Fact]
    public void InputMethodList_UsesTextServicesFrameworkActivationApi()
    {
        string source = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings.Core/InputMethodList.cs"));

        Assert.Contains("ITfInputProcessorProfileMgr", source);
        Assert.Contains("ActivateProfile", source);
        Assert.Contains("TfIppmfForSession", source);
        Assert.Contains("TfIppmfDontCareCurrentInputLanguage", source);
    }

    [Fact]
    public void UserListProbe_RecognizesOnlyTheExactRegistryValueName()
    {
        Assert.True(InputMethodList.ContainsFamoTipValueName(
            [InputMethodList.FamoTip.ToLowerInvariant()]));
        Assert.False(InputMethodList.ContainsFamoTipValueName(
            ["UnrelatedValue", $"prefix-{InputMethodList.FamoTip}"]));

        string source = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings.Core/InputMethodList.cs"));
        string probe = source[source.IndexOf(
            "public static bool TryIsFamoInUserList",
            StringComparison.Ordinal)..source.IndexOf(
            "private static (ushort LangId",
            StringComparison.Ordinal)];
        Assert.DoesNotContain("GetValue(", probe);
        Assert.DoesNotContain("ValueContainsTip", probe);
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
            dir = Path.GetDirectoryName(dir);
        }
        throw new FileNotFoundException(relativePath);
    }
}
