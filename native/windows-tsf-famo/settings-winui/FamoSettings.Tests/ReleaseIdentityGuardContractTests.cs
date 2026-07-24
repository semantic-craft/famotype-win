using Xunit;

namespace Famo.Settings.Tests;

public sealed class ReleaseIdentityGuardContractTests
{
    [Fact]
    public void IdentityGuard_SeparatesRuntimeCompatibilityPathFromContent()
    {
        string guard = File.ReadAllText(RepoFile("native/windows-tsf-famo/tools/identity_guard/check_release_identity.ps1"));

        Assert.Contains("Test-AllowedRuntimeCompatibilityPath", guard);
        Assert.Contains(@"^(data/)?weasel(\.custom)?\.yaml$", guard);
        Assert.Contains("-not $allowRuntimeCompatibilityPath -and $relative -match", guard);
        Assert.Contains("Test-SkipIdentityContentPath", guard);
        Assert.Contains(@"\.dict\.yaml$", guard);
        Assert.Contains("^(data/)?(cn_dicts|en_dicts|opencc)/", guard);
    }

    [Fact]
    public void AssemblePayload_ExcludesResearchBundleAndNeutralizesYamlComments()
    {
        string assemble = File.ReadAllText(RepoFile("native/windows-tsf-famo/famo-config/assemble-payload.sh"));

        Assert.Contains("--exclude='others'", assemble);
        Assert.Contains("neutralize_payload_identity_text", assemble);
        Assert.Contains("! -name '*.dict.yaml'", assemble);
        Assert.Contains("[Ww]easel", assemble);
        Assert.Contains("小狼毫", assemble);
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

        throw new FileNotFoundException(relativePath);
    }
}
