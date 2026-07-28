using System.Diagnostics;
using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

[Collection("SeedFileTransaction serial")]
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
        UserDataTransactionLock.LocalAppDataOverrideForTests =
            Path.Combine(_root, "lock-local-app-data");
        Directory.CreateDirectory(Path.Combine(_source, "opencc"));
        File.WriteAllText(Path.Combine(_source, "default.yaml"), "schema_list:\n");
        File.WriteAllText(Path.Combine(_source, "default.custom.yaml"), "patch:\n");
        File.WriteAllText(Path.Combine(_source, "opencc", "emoji.json"), "{}");
    }

    public void Dispose()
    {
        SeedFileTransaction.BeforeAtomicFilePublishForTests = null;
        SeedFileTransaction.BeforePinnedAtomicFilePublishForTests = null;
        SeedFileTransaction
            .AfterPinnedTemporaryDeleteDispositionClearedForTests = null;
        SeedFileTransaction.AfterPinnedAtomicFileRenameForTests = null;
        UserDataTransactionLock.LocalAppDataOverrideForTests =
            TestUserDataLockEnvironment.LocalAppDataRoot;
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }

    [Fact]
    public void Seed_InterruptedBeforePublishLeavesNoHalfFileAndRetriesCleanly()
    {
        string target = Path.Combine(_dest, "default.yaml");
        SeedFileTransaction.BeforeAtomicFilePublishForTests = path =>
        {
            if (string.Equals(
                path, target, StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException("simulated publish interruption");
            }
        };

        Assert.Throws<IOException>(
            () => FirstLaunchSeeder.Seed(_source, _dest));
        Assert.False(File.Exists(target));
        Assert.Empty(Directory.GetFiles(
            _dest, "default.yaml.famo-tmp-*", SearchOption.TopDirectoryOnly));

        SeedFileTransaction.BeforeAtomicFilePublishForTests = null;
        FirstLaunchSeedResult recovered =
            FirstLaunchSeeder.Seed(_source, _dest);

        Assert.True(File.Exists(target));
        Assert.Equal("schema_list:\n", File.ReadAllText(target));
        Assert.True(recovered.Copied >= 1);
    }

    [Theory]
    [InlineData("regular")]
    [InlineData("reparse")]
    public void Seed_PublishesTheExactPinnedTemporaryObject(
        string replacementKind)
    {
        string target = Path.Combine(_dest, "default.yaml");
        string outside = Path.Combine(_root, "outside-sentinel.txt");
        File.WriteAllText(outside, "outside");
        string? temporary = null;
        bool replacementBlocked = false;
        SeedFileTransaction.BeforePinnedAtomicFilePublishForTests =
            (destination, exactTemporary) =>
            {
                if (!string.Equals(
                        destination,
                        target,
                        StringComparison.OrdinalIgnoreCase))
                {
                    return;
                }
                temporary = exactTemporary;
                try
                {
                    File.Delete(exactTemporary);
                    if (replacementKind == "reparse")
                    {
                        File.CreateSymbolicLink(exactTemporary, outside);
                    }
                    else
                    {
                        File.WriteAllText(exactTemporary, "foreign");
                    }
                }
                catch (Exception ex) when (
                    ex is IOException or UnauthorizedAccessException)
                {
                    replacementBlocked = true;
                }
            };

        FirstLaunchSeeder.Seed(_source, _dest);

        Assert.NotNull(temporary);
        Assert.True(replacementBlocked);
        Assert.Equal("schema_list:\n", File.ReadAllText(target));
        Assert.False(File.Exists(temporary));
        Assert.Equal("outside", File.ReadAllText(outside));
    }

    [Fact]
    public void Seed_DoesNotDeleteForeignTemporaryLookalikes()
    {
        Directory.CreateDirectory(_dest);
        string lookalike = Path.Combine(
            _dest, "default.yaml.famo-tmp-foreign");
        File.WriteAllText(lookalike, "foreign");

        FirstLaunchSeeder.Seed(_source, _dest);

        Assert.Equal("foreign", File.ReadAllText(lookalike));
        Assert.Equal(
            "schema_list:\n",
            File.ReadAllText(Path.Combine(_dest, "default.yaml")));
    }

    [Fact]
    public void Seed_HardCrashBeforePublishKernelDeletesTheExactTemporary()
    {
        string source = Path.Combine(_source, "default.yaml");
        string target = Path.Combine(_dest, "default.yaml");
        string marker = Path.Combine(_root, "crash-marker.txt");
        RunCrashHarness(
            source,
            target,
            marker,
            "before-publish",
            UserDataTransactionLock.LocalAppDataOverrideForTests);

        Assert.True(File.Exists(marker));
        string temporary = File.ReadAllText(marker);
        Assert.False(File.Exists(temporary));
        Assert.False(File.Exists(target));

        SeedFileTransaction.CopyDurableAtomic(
            source, target, overwrite: false);
        Assert.Equal("schema_list:\n", File.ReadAllText(target));
        Assert.False(File.Exists(temporary));
    }

    [Fact]
    public void Seed_HardCrashAfterDeleteDispositionClearRecoversOnNextStartup()
    {
        string source = Path.Combine(_source, "default.yaml");
        string target = Path.Combine(_dest, "default.yaml");
        string marker = Path.Combine(_root, "after-clear-crash-marker.txt");
        string foreign = target + ".famo-tmp-foreign";
        Directory.CreateDirectory(_dest);
        File.WriteAllText(foreign, "foreign");

        RunCrashHarness(
            source,
            target,
            marker,
            "after-clear-before-rename",
            UserDataTransactionLock.LocalAppDataOverrideForTests);

        Assert.True(File.Exists(marker));
        string temporary = File.ReadAllText(marker);
        string recoveryRecord =
            UserDataTransactionLock.AtomicPublicationRecoveryPath(_dest);
        Assert.True(File.Exists(temporary));
        Assert.True(File.Exists(recoveryRecord));
        Assert.False(File.Exists(target));

        FirstLaunchSeedResult recovered =
            FirstLaunchSeeder.Seed(_source, _dest);

        Assert.True(recovered.Copied >= 1);
        Assert.Equal("schema_list:\n", File.ReadAllText(target));
        Assert.False(File.Exists(temporary));
        Assert.False(File.Exists(recoveryRecord));
        Assert.Equal("foreign", File.ReadAllText(foreign));
    }

    [Fact]
    public void Seed_RecoveryPreservesReplacementAtRecordedTemporaryPath()
    {
        string source = Path.Combine(_source, "default.yaml");
        string target = Path.Combine(_dest, "default.yaml");
        string marker = Path.Combine(_root, "identity-crash-marker.txt");
        RunCrashHarness(
            source,
            target,
            marker,
            "after-clear-before-rename",
            UserDataTransactionLock.LocalAppDataOverrideForTests);

        string temporary = File.ReadAllText(marker);
        string parked = temporary + ".parked";
        string recoveryRecord =
            UserDataTransactionLock.AtomicPublicationRecoveryPath(_dest);
        Assert.True(File.Exists(temporary));
        Assert.True(File.Exists(recoveryRecord));
        File.Move(temporary, parked);
        File.WriteAllText(temporary, "foreign replacement");

        IOException error = Assert.Throws<IOException>(
            () => FirstLaunchSeeder.Seed(_source, _dest));

        Assert.Contains("identity changed", error.Message);
        Assert.Equal("foreign replacement", File.ReadAllText(temporary));
        Assert.Equal("schema_list:\n", File.ReadAllText(parked));
        Assert.True(File.Exists(recoveryRecord));
        Assert.False(File.Exists(target));
    }

    [Fact]
    public void Seed_HardCrashAfterRenamePreservesPublishedFileOnNextStartup()
    {
        string source = Path.Combine(_source, "default.yaml");
        string target = Path.Combine(_dest, "default.yaml");
        string marker = Path.Combine(_root, "after-rename-crash-marker.txt");
        string foreign = target + ".famo-tmp-foreign";
        Directory.CreateDirectory(_dest);
        File.WriteAllText(foreign, "foreign");

        RunCrashHarness(
            source,
            target,
            marker,
            "after-rename-before-recovery-cleanup",
            UserDataTransactionLock.LocalAppDataOverrideForTests);

        string temporary = File.ReadAllText(marker);
        string recoveryRecord =
            UserDataTransactionLock.AtomicPublicationRecoveryPath(_dest);
        Assert.False(File.Exists(temporary));
        Assert.True(File.Exists(target));
        Assert.True(File.Exists(recoveryRecord));

        FirstLaunchSeedResult recovered =
            FirstLaunchSeeder.Seed(_source, _dest);

        Assert.True(recovered.Skipped >= 1);
        Assert.Equal("schema_list:\n", File.ReadAllText(target));
        Assert.False(File.Exists(recoveryRecord));
        Assert.Equal("foreign", File.ReadAllText(foreign));
    }

    private static void RunCrashHarness(
        string source,
        string target,
        string marker,
        string? mode = null,
        string? localAppData = null)
    {
        var testOutput = new DirectoryInfo(AppContext.BaseDirectory);
        string configuration = testOutput.Parent?.Name
            ?? throw new InvalidOperationException(
                "Test output has no configuration directory.");
        string settingsRoot = testOutput.Parent?.Parent?.Parent?.Parent?.FullName
            ?? throw new InvalidOperationException(
                "Cannot locate the Settings test projects.");
        string harness = Path.Combine(
            settingsRoot,
            "FamoSettings.SeedCrashHarness",
            "bin",
            configuration,
            "net10.0",
            "FamoSettings.SeedCrashHarness.exe");
        Assert.True(File.Exists(harness), harness);

        var start = new ProcessStartInfo(harness)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        if (mode is not null)
        {
            start.ArgumentList.Add(mode);
        }
        start.ArgumentList.Add(source);
        start.ArgumentList.Add(target);
        start.ArgumentList.Add(marker);
        if (localAppData is not null)
        {
            start.ArgumentList.Add(localAppData);
        }
        using Process process = Process.Start(start)
            ?? throw new InvalidOperationException(
                "Cannot start the seed crash harness.");
        if (!process.WaitForExit(10_000))
        {
            process.Kill(entireProcessTree: true);
            Assert.Fail("Seed crash harness did not terminate.");
        }

        Assert.NotEqual(0, process.ExitCode);
    }

    [Fact]
    public void Seed_RefusesTargetAncestorLinkBeforeCreatingOutsideDirectories()
    {
        string targetParent = Path.GetDirectoryName(_dest)
            ?? throw new InvalidOperationException("Target has no parent.");
        string outside = Path.Combine(_root, "outside");
        Directory.CreateDirectory(outside);
        File.WriteAllText(Path.Combine(outside, "sentinel.txt"), "outside");
        try
        {
            Directory.CreateSymbolicLink(targetParent, outside);
        }
        catch (Exception ex) when (
            ex is UnauthorizedAccessException or IOException or
            PlatformNotSupportedException)
        {
            return;
        }

        Assert.Throws<IOException>(
            () => FirstLaunchSeeder.Seed(_source, _dest));
        Assert.Equal(
            ["sentinel.txt"],
            Directory.EnumerateFileSystemEntries(outside)
                .Select(Path.GetFileName)
                .Order(StringComparer.OrdinalIgnoreCase));
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
