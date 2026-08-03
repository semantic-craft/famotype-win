using Xunit;

namespace Famo.Settings.Tests;

public sealed class TsfRegistrationAuditContractTests
{
    [Fact]
    public void AuditScriptCoversProfileCategoriesAndCurrentUserTip()
    {
        string script = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Test-FamoTsfRegistration.ps1"));

        foreach (string expected in new[]
        {
            "TSF-COM",
            "TSF-PROFILE",
            "TSF-ACTIVE",
            "TSF-CURRENT-USER-TIP",
            "TSF-IDENTITY",
            "HKLM COM registration",
            "per-user COM override absent",
            "FamoProfileTool verifies",
            "check-absent",
            "PendingReboot",
            "keyboard category",
            "Win+Space visibility",
            "Ready activation is best-effort",
        })
        {
            Assert.Contains(expected, script);
        }
    }

    [Fact]
    public void AuditScriptPinsStableAndDevelopmentIdentityIsolation()
    {
        string script = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Test-FamoTsfRegistration.ps1"));

        foreach (string expected in new[]
        {
            "clsidTextService",
            "guidProfile",
            "0804:$clsid$profileGuid",
            "HKLM:\\Software\\Classes\\CLSID\\$clsid",
            "HKLM:\\Software\\Microsoft\\CTF\\TIP\\$clsid",
            "{A6E6F585-4C92-459D-8D5B-175559605FB9}",
            "Identity -eq 'Stable'",
        })
        {
            Assert.Contains(expected, script);
        }
    }

    [Fact]
    public void AuditScriptVerifiesInputModeCapabilityCategory()
    {
        string script = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Test-FamoTsfRegistration.ps1"));

        Assert.Contains("TSF-INPUT-MODE", script);
        Assert.Contains("{CCF05DD7-4A87-11D7-A6E2-00065B84435C}", script);
        Assert.Contains("input mode compartment capability", script);
    }

    [Fact]
    public void AuditScriptExplainsMissingStateAndCanEmitJsonForCi()
    {
        string script = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Test-FamoTsfRegistration.ps1"));

        Assert.Contains("[switch] $Json", script);
        Assert.Contains("expected", script);
        Assert.Contains("actual", script);
        Assert.Contains("probeMode", script);
        Assert.Contains("ReadOnly", script);
        Assert.Contains("NoWrite:%AppData%\\Rime", script);
        Assert.Contains("ConvertTo-Json", script);
        Assert.Contains("exit 1", script);
    }

    [Fact]
    public void AuditScriptIsReadOnlyAndDoesNotModifyUserWeaselData()
    {
        string script = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Test-FamoTsfRegistration.ps1"));

        Assert.DoesNotContain("Set-ItemProperty", script);
        Assert.DoesNotContain("New-Item", script);
        Assert.DoesNotContain("Remove-Item", script);
        Assert.DoesNotContain("RegisterCategory", script);
        Assert.DoesNotContain("RegisterProfile", script);
        Assert.DoesNotContain("InstallLayoutOrTip", script);
        Assert.DoesNotContain("AppData\\Roaming\\Rime", script, StringComparison.OrdinalIgnoreCase);
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
