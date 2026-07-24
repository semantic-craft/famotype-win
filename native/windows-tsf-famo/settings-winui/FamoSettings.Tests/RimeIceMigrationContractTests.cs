using Xunit;

namespace Famo.Settings.Tests;

public sealed class RimeIceMigrationContractTests
{
    [Theory]
    [InlineData("AboutPage.cs")]
    public void UserVisibleSettingsCopy_NamesRimeIceNotOhMyRime(string fileName)
    {
        string source = File.ReadAllText(SettingsViewFile(fileName));

        Assert.Contains("rime-ice", source, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("oh-my-rime", source, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("薄荷", source, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("rime_mint", source, StringComparison.OrdinalIgnoreCase);
    }

    private static string SettingsViewFile(string fileName)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(
                dir,
                "native",
                "windows-tsf-famo",
                "settings-winui",
                "FamoSettings",
                "Views",
                fileName);
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate FamoSettings/Views/{fileName}");
    }
}
