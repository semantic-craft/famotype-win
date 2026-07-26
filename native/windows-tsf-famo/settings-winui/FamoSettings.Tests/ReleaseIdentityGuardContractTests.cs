using Xunit;

namespace Famo.Settings.Tests;

public sealed class ReleaseIdentityGuardContractTests
{
    [Fact]
    public void IdentityGuard_SeparatesRuntimeCompatibilityPathFromContent()
    {
        string guard = File.ReadAllText(RepoFile("native/windows-tsf-famo/tools/identity_guard/check_release_identity.ps1"));

        Assert.Contains("Test-AllowedRuntimeCompatibilityPath", guard);
        Assert.Contains(@"^(?:(?:payload/)?data/)?weasel(\.custom)?\.yaml$", guard);
        Assert.Contains("-not $allowRuntimeCompatibilityPath -and $relative -match", guard);
        Assert.Contains("Test-SkipIdentityContentPath", guard);
        Assert.Contains(@"\.dict\.yaml$", guard);
        Assert.Contains("^(?:(?:payload/)?data/)?(cn_dicts|en_dicts|opencc)/", guard);
        Assert.Contains("^(payload/)?payload-manifest\\.txt$", guard);
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

    [Fact]
    public void AssemblePayload_ReownsOpenCcOverlayDestinations()
    {
        string assemble = File.ReadAllText(RepoFile("native/windows-tsf-famo/famo-config/assemble-payload.sh"));
        int start = assemble.IndexOf("overlay_opencc_standard()", StringComparison.Ordinal);
        int end = assemble.IndexOf("strip_law_layer()", start, StringComparison.Ordinal);
        string overlay = assemble[start..end];

        Assert.Contains("[ ! -L \"${opencc_dir}\" ]", overlay);
        Assert.Contains("rm -f -- \"${destination}\"", overlay);
        Assert.True(
            overlay.IndexOf("rm -f -- \"${destination}\"", StringComparison.Ordinal) <
            overlay.IndexOf("cp -- \"${OPENCC_STANDARD_DIR}/${f}\" \"${destination}\"", StringComparison.Ordinal));
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
