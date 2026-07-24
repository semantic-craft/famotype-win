using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class FirstLaunchSeederTests : IDisposable
{
    private readonly string _root;
    private readonly string _source;
    private readonly string _dest;

    public FirstLaunchSeederTests()
    {
        _root = Path.Combine(Path.GetTempPath(), "famo-seed-test-" + Guid.NewGuid().ToString("N"));
        _source = Path.Combine(_root, "app", "data");
        _dest = Path.Combine(_root, "local", "Famo");
        Directory.CreateDirectory(Path.Combine(_source, "opencc"));
        File.WriteAllText(Path.Combine(_source, "default.yaml"), "schema_list:\n");
        File.WriteAllText(Path.Combine(_source, "default.custom.yaml"), "patch:\n");
        File.WriteAllText(Path.Combine(_source, "opencc", "emoji.json"), "{}");
    }

    public void Dispose()
    {
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }

    [Fact]
    public void Seed_CopiesPayloadToLocalFamoWithoutTouchingRime()
    {
        FirstLaunchSeedResult result = FirstLaunchSeeder.Seed(_source, _dest);

        Assert.Equal(3, result.PayloadFiles);
        Assert.Equal(3, result.Copied);
        Assert.Equal(0, result.Skipped);
        Assert.True(File.Exists(Path.Combine(_dest, "default.yaml")));
        Assert.True(File.Exists(Path.Combine(_dest, "default.custom.yaml")));
        Assert.True(File.Exists(Path.Combine(_dest, "opencc", "emoji.json")));
        Assert.False(Directory.Exists(Path.Combine(_root, "AppData", "Rime")));
    }

    [Fact]
    public void Seed_PreservesExistingUserFilesByDefault()
    {
        Directory.CreateDirectory(_dest);
        string custom = Path.Combine(_dest, "default.custom.yaml");
        File.WriteAllText(custom, "user edit");

        FirstLaunchSeedResult result = FirstLaunchSeeder.Seed(_source, _dest);

        Assert.Equal("user edit", File.ReadAllText(custom));
        Assert.Equal(2, result.Copied);
        Assert.Equal(1, result.Skipped);
    }

    [Fact]
    public void ResolveInstalledDataDir_UsesSettingsSiblingDataDirectory()
    {
        string settingsDir = Path.Combine(_root, "app", "settings");
        string dataDir = Path.Combine(_root, "app", "data");
        Directory.CreateDirectory(settingsDir);
        Directory.CreateDirectory(dataDir);

        Assert.Equal(
            Path.GetFullPath(dataDir),
            FirstLaunchSeeder.ResolveInstalledDataDir(settingsDir));
    }

    [Fact]
    public void ResolveInstalledDataDir_AlsoSupportsFlatDeveloperOutput()
    {
        string outputDir = Path.Combine(_root, "output");
        string dataDir = Path.Combine(outputDir, "data");
        Directory.CreateDirectory(dataDir);

        Assert.Equal(
            Path.GetFullPath(dataDir),
            FirstLaunchSeeder.ResolveInstalledDataDir(outputDir));
    }
}
