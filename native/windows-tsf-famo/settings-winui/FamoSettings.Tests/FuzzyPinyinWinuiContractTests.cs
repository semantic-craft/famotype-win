using System.Text.Json;
using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class FuzzyPinyinWinuiContractTests
{
    private static readonly string[] Keys =
    {
        "zh_z", "ch_c", "sh_s", "n_l", "r_l", "f_h", "an_ang", "en_eng", "in_ing",
    };

    [Fact]
    public void DefaultAndSchema_ExposeNineIndependentPairsOnly()
    {
        using JsonDocument defaults = JsonDocument.Parse(SettingsStore.DefaultSettingsJson);
        JsonElement fuzzyDefaults = defaults.RootElement.GetProperty("engine").GetProperty("fuzzyPinyin");
        Assert.Equal(Keys, fuzzyDefaults.EnumerateObject().Select(p => p.Name).ToArray());
        Assert.All(Keys, key => Assert.False(fuzzyDefaults.GetProperty(key).GetBoolean()));

        string schemaText = File.ReadAllText(RepoFile("native/windows-tsf-famo/famo-config/famo-settings.schema.json"));
        using JsonDocument schema = JsonDocument.Parse(schemaText);
        JsonElement fuzzySchema = schema.RootElement
            .GetProperty("properties").GetProperty("engine")
            .GetProperty("properties").GetProperty("fuzzyPinyin");
        Assert.Equal(Keys, fuzzySchema.GetProperty("required").EnumerateArray().Select(x => x.GetString()).ToArray());

        Assert.DoesNotContain("zh_ch_sh", SettingsStore.DefaultSettingsJson);
        Assert.DoesNotContain("an_en_in", SettingsStore.DefaultSettingsJson);
        Assert.DoesNotContain("l_n_f_h_r_l", SettingsStore.DefaultSettingsJson);
    }

    [Fact]
    public void KeyboardPage_RendersNineRealDeployBackedControls()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/KeyboardPage.cs"));

        Assert.Contains("FamoUI.Card(\"模糊拼音\"", page);

        string[] labels =
        {
            "zh = z", "ch = c", "sh = s", "n = l", "r = l", "f = h",
            "an = ang", "en = eng", "in = ing",
        };
        foreach (string label in labels)
        {
            Assert.Contains($"FuzzyRow(\"{label}\"", page);
        }

        Assert.Equal(9, Count(page, "FuzzyRow(\""));
        Assert.Contains("App.SaveAndApplyDeploy();", page);
        Assert.DoesNotContain("ZhChSh", page);
        Assert.DoesNotContain("AnEnIn", page);
        Assert.DoesNotContain("LNFHRL", page);
    }

    private static int Count(string text, string needle)
    {
        int count = 0;
        int start = 0;
        while (true)
        {
            int idx = text.IndexOf(needle, start, StringComparison.Ordinal);
            if (idx < 0) return count;
            count++;
            start = idx + needle.Length;
        }
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
