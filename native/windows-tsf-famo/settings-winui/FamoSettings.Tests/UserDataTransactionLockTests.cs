using System.Diagnostics;
using System.Security.Principal;
using System.Text;
using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

[Collection("SeedFileTransaction serial")]
public sealed class UserDataTransactionLockTests : IDisposable
{
    private readonly string _root = Path.Combine(
        Path.GetTempPath(), $"famo-lock-tests-{Guid.NewGuid():N}");

    public UserDataTransactionLockTests()
    {
        UserDataTransactionLock.LocalAppDataOverrideForTests =
            Path.Combine(_root, "local-app-data");
    }

    [Fact]
    public void GlobalMutexNameIsBoundToTheExactWindowsUserSid()
    {
        if (!OperatingSystem.IsWindows())
        {
            return;
        }

        string sid = WindowsIdentity.GetCurrent().User?.Value
            ?? throw new InvalidOperationException("Current user SID is unavailable.");
        Assert.Equal(
            $@"Global\Famo.Settings.UserData.Transaction.{sid}",
            UserDataTransactionLock.GlobalMutexNameForTests);

        using IDisposable held = UserDataTransactionLock.Acquire(_root);
    }

    [Fact]
    public async Task FileLockFallbackNeverContinuesWithoutMutualExclusion()
    {
        string lockPath =
            UserDataTransactionLock.LockFilePathForTests(_root);
        Directory.CreateDirectory(Path.GetDirectoryName(lockPath)!);
        using var blocker = new FileStream(
            lockPath,
            FileMode.OpenOrCreate,
            FileAccess.ReadWrite,
            FileShare.None);
        UserDataTransactionLock.DisableNamedMutexForTests = true;
        using var started = new ManualResetEventSlim();
        Task waiter = Task.Run(() =>
        {
            started.Set();
            using IDisposable held =
                UserDataTransactionLock.Acquire(
                    Path.Combine(_root, "different", "nested"));
        });

        Assert.True(started.Wait(TimeSpan.FromSeconds(2)));
        await Task.Delay(200);
        Assert.False(waiter.IsCompleted);

        blocker.Dispose();
        await waiter.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public void FileLockDomainIsStableAcrossRootsSubdirectoriesAndTemp()
    {
        string originalTemp = Environment.GetEnvironmentVariable("TEMP") ?? "";
        string originalTmp = Environment.GetEnvironmentVariable("TMP") ?? "";
        try
        {
            string overrideLocalData =
                UserDataTransactionLock.LocalAppDataOverrideForTests!;
            string first =
                UserDataTransactionLock.LockFilePathForTests(_root);
            string alternateTemp = Path.Combine(
                _root, $"alternate-temp-{Guid.NewGuid():N}");
            Environment.SetEnvironmentVariable("TEMP", alternateTemp);
            Environment.SetEnvironmentVariable("TMP", alternateTemp);
            string second = UserDataTransactionLock.LockFilePathForTests(
                Path.Combine(_root, "other", "nested"));

            Assert.Equal(first, second);
            Assert.StartsWith(
                Path.TrimEndingDirectorySeparator(
                    Path.GetFullPath(overrideLocalData)),
                first,
                StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain(
                "alternate-temp",
                first,
                StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            Environment.SetEnvironmentVariable("TEMP", originalTemp);
            Environment.SetEnvironmentVariable("TMP", originalTmp);
        }
    }

    [Fact]
    public void DefaultFileLockPathUsesCurrentUserLocalAppData()
    {
        string? temporaryOverride =
            UserDataTransactionLock.LocalAppDataOverrideForTests;
        try
        {
            UserDataTransactionLock.LocalAppDataOverrideForTests = null;
            string path =
                UserDataTransactionLock.LockFilePathForTests(_root);

            Assert.StartsWith(
                Path.TrimEndingDirectorySeparator(
                    Environment.GetFolderPath(
                        Environment.SpecialFolder.LocalApplicationData)),
                path,
                StringComparison.OrdinalIgnoreCase);
            Assert.Contains(
                Path.Combine("Famo.UserDataLocks", ""),
                path,
                StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            UserDataTransactionLock.LocalAppDataOverrideForTests =
                temporaryOverride;
        }
    }

    [Fact]
    public void SameThreadNestedWritersShareTheStableLockRecursively()
    {
        using IDisposable outer =
            UserDataTransactionLock.Acquire(_root);
        using IDisposable nested =
            UserDataTransactionLock.Acquire(
                Path.Combine(_root, "nested", "transaction"));
    }

    [Fact]
    public void PinnedLockDirectoryBlocksSwapBeforeRelativeFileOpen()
    {
        string lockPath =
            UserDataTransactionLock.LockFilePathForTests(_root);
        string lockDirectory = Path.GetDirectoryName(lockPath)!;
        string parked = lockDirectory + ".parked";
        string outside = Path.Combine(_root, "outside-lock-target");
        Directory.CreateDirectory(outside);
        File.WriteAllText(Path.Combine(outside, "sentinel.txt"), "outside");
        bool attempted = false;
        bool blocked = false;
        bool swapped = false;
        UserDataTransactionLock.BeforeLockFileOpenForTests = path =>
        {
            Assert.Equal(lockDirectory, path, ignoreCase: true);
            attempted = true;
            try
            {
                Directory.Move(lockDirectory, parked);
                swapped = true;
                Directory.CreateSymbolicLink(lockDirectory, outside);
            }
            catch (IOException)
            {
                blocked = true;
            }
        };

        try
        {
            using IDisposable held =
                UserDataTransactionLock.Acquire(_root);

            Assert.True(attempted);
            Assert.True(blocked);
            Assert.False(swapped);
            Assert.True(File.Exists(lockPath));
            Assert.Equal(
                ["sentinel.txt"],
                Directory.EnumerateFileSystemEntries(outside)
                    .Select(Path.GetFileName)
                    .Order(StringComparer.OrdinalIgnoreCase));
        }
        finally
        {
            UserDataTransactionLock.BeforeLockFileOpenForTests = null;
            if (swapped)
            {
                if (Directory.Exists(lockDirectory) &&
                    (File.GetAttributes(lockDirectory) &
                     FileAttributes.ReparsePoint) != 0)
                {
                    Directory.Delete(lockDirectory);
                }
                if (Directory.Exists(parked))
                {
                    Directory.Move(parked, lockDirectory);
                }
            }
        }
    }

    [Fact]
    public async Task FileDisposeFailureStillReleasesTheGlobalMutex()
    {
        string expectedLock =
            UserDataTransactionLock.LockFilePathForTests(_root);
        UserDataTransactionLock.AfterFileLockDisposeForTests = path =>
        {
            if (string.Equals(
                path, expectedLock, StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException("simulated file-lock dispose failure");
            }
        };
        IDisposable held = UserDataTransactionLock.Acquire(_root);

        IOException error = Assert.Throws<IOException>(() => held.Dispose());
        Assert.Contains("simulated", error.Message);
        UserDataTransactionLock.AfterFileLockDisposeForTests = null;

        Task nextProcessThread = Task.Run(() =>
        {
            using IDisposable next =
                UserDataTransactionLock.Acquire(_root);
        });
        await nextProcessThread.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task GlobalMutexAndFileLockExcludeAnotherProcess()
    {
        if (!OperatingSystem.IsWindows())
        {
            return;
        }

        Directory.CreateDirectory(_root);
        string started = Path.Combine(_root, "child-started");
        string acquired = Path.Combine(_root, "child-acquired");
        string mutexName =
            UserDataTransactionLock.GlobalMutexNameForTests;
        string lockPath =
            UserDataTransactionLock.LockFilePathForTests(_root);
        string script = $$"""
            $name = [Text.Encoding]::UTF8.GetString(
              [Convert]::FromBase64String('{{Utf8Base64(mutexName)}}'))
            $lockPath = [Text.Encoding]::UTF8.GetString(
              [Convert]::FromBase64String('{{Utf8Base64(lockPath)}}'))
            $started = [Text.Encoding]::UTF8.GetString(
              [Convert]::FromBase64String('{{Utf8Base64(started)}}'))
            $acquired = [Text.Encoding]::UTF8.GetString(
              [Convert]::FromBase64String('{{Utf8Base64(acquired)}}'))
            $mutex = [Threading.Mutex]::new($false, $name)
            $entered = $false
            $file = $null
            [IO.File]::WriteAllText($started, 'started')
            try {
              try {
                $entered = $mutex.WaitOne(10000)
              } catch [Threading.AbandonedMutexException] {
                $entered = $true
              }
              if (-not $entered) { exit 3 }
              $deadline = [DateTime]::UtcNow.AddSeconds(10)
              while ($null -eq $file) {
                try {
                  $file = [IO.File]::Open(
                    $lockPath,
                    [IO.FileMode]::OpenOrCreate,
                    [IO.FileAccess]::ReadWrite,
                    [IO.FileShare]::None)
                } catch [IO.IOException] {
                  if ([DateTime]::UtcNow -ge $deadline) { exit 4 }
                  Start-Sleep -Milliseconds 25
                }
              }
              [IO.File]::WriteAllText($acquired, 'acquired')
            } finally {
              if ($null -ne $file) { $file.Dispose() }
              if ($entered) { $mutex.ReleaseMutex() }
              $mutex.Dispose()
            }
            """;
        string encoded = Convert.ToBase64String(
            Encoding.Unicode.GetBytes(script));
        using var holderReady = new ManualResetEventSlim();
        using var releaseHolder = new ManualResetEventSlim();
        Exception? holderFailure = null;
        var holder = new Thread(() =>
        {
            try
            {
                using IDisposable held =
                    UserDataTransactionLock.Acquire(_root);
                holderReady.Set();
                releaseHolder.Wait(TimeSpan.FromSeconds(20));
            }
            catch (Exception ex)
            {
                holderFailure = ex;
                holderReady.Set();
            }
        });
        holder.Start();
        Assert.True(holderReady.Wait(TimeSpan.FromSeconds(5)));
        Assert.Null(holderFailure);
        using var child = Process.Start(new ProcessStartInfo
        {
            FileName = "powershell.exe",
            Arguments = $"-NoLogo -NoProfile -NonInteractive -EncodedCommand {encoded}",
            UseShellExecute = false,
            CreateNoWindow = true,
        }) ?? throw new InvalidOperationException("Could not start lock probe.");
        try
        {
            await WaitForFile(started, TimeSpan.FromSeconds(5));
            await Task.Delay(200);
            Assert.False(File.Exists(acquired));

            releaseHolder.Set();
            Assert.True(holder.Join(TimeSpan.FromSeconds(5)));
            Assert.Null(holderFailure);
            await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(15));
            Assert.Equal(0, child.ExitCode);
            Assert.True(File.Exists(acquired));
        }
        finally
        {
            if (!child.HasExited)
            {
                child.Kill(entireProcessTree: true);
            }
            releaseHolder.Set();
            holder.Join(TimeSpan.FromSeconds(5));
        }
    }

    public void Dispose()
    {
        UserDataTransactionLock.DisableNamedMutexForTests = false;
        UserDataTransactionLock.AfterFileLockDisposeForTests = null;
        UserDataTransactionLock.BeforeLockFileOpenForTests = null;
        UserDataTransactionLock.LocalAppDataOverrideForTests =
            TestUserDataLockEnvironment.LocalAppDataRoot;
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }

    private static string Utf8Base64(string value) =>
        Convert.ToBase64String(Encoding.UTF8.GetBytes(value));

    private static async Task WaitForFile(
        string path, TimeSpan timeout)
    {
        var elapsed = Stopwatch.StartNew();
        while (!File.Exists(path))
        {
            if (elapsed.Elapsed >= timeout)
            {
                throw new TimeoutException(
                    $"Timed out waiting for child marker: {path}");
            }
            await Task.Delay(25);
        }
    }
}
