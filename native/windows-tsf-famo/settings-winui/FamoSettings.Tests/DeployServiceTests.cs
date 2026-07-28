using Famo.Settings.Core;
using System.Diagnostics;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>Native runtime control 路径解析 + 命令构造（纯逻辑，不真跑部署）。</summary>
public class DeployServiceTests
{
    private const string EnvExe = "FAMO_RUNTIME";
    private const string EnvDir = "FAMO_NATIVE_OUTPUT";

    private static void ClearEnv()
    {
        Environment.SetEnvironmentVariable(EnvExe, null);
        Environment.SetEnvironmentVariable(EnvDir, null);
    }

    [Fact]
    public void Resolve_FromEnvExe_ReturnsThatPath()
    {
        ClearEnv();
        string tmp = Path.Combine(Path.GetTempPath(), $"famo-dep-{Guid.NewGuid():N}.exe");
        File.WriteAllText(tmp, "stub");
        try
        {
            Environment.SetEnvironmentVariable(EnvExe, tmp);
            Assert.Equal(tmp, DeployService.ResolveRuntimePath());
        }
        finally { ClearEnv(); File.Delete(tmp); }
    }

    [Fact]
    public void Resolve_FromEnvOutputDir_ReturnsRuntimeInDir()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-out-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        string exe = Path.Combine(dir, "FamoRuntime.exe");
        File.WriteAllText(exe, "stub");
        try
        {
            Environment.SetEnvironmentVariable(EnvDir, dir);
            Assert.Equal(exe, DeployService.ResolveRuntimePath());
        }
        finally { ClearEnv(); Directory.Delete(dir, true); }
    }

    [Fact]
    public void Resolve_AlongsideExe_WhenEnvUnset()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-base-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        string exe = Path.Combine(dir, "FamoRuntime.exe");
        File.WriteAllText(exe, "stub");
        try
        {
            Assert.Equal(exe, DeployService.ResolveRuntimePath(dir));
        }
        finally { ClearEnv(); Directory.Delete(dir, true); }
    }

    [Fact]
    public void Resolve_InstallerParentDir_WhenSettingsLivesUnderSubdirectory()
    {
        ClearEnv();
        string appDir = Path.Combine(Path.GetTempPath(), $"famo-app-{Guid.NewGuid():N}");
        string settingsDir = Path.Combine(appDir, "settings");
        Directory.CreateDirectory(settingsDir);
        string exe = Path.Combine(appDir, "FamoRuntime.exe");
        File.WriteAllText(exe, "stub");
        try
        {
            Assert.Equal(exe, DeployService.ResolveRuntimePath(settingsDir));
        }
        finally { ClearEnv(); Directory.Delete(appDir, true); }
    }

    [Fact]
    public void Resolve_NotFound_ReturnsNull()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-empty-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        try
        {
            Assert.Null(DeployService.ResolveRuntimePath(dir));
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void BuildCommand_QuotesPath_AppendsDeploy()
    {
        string cmd = DeployService.BuildCommand(@"C:\Program Files\Famo\FamoRuntime.exe");
        Assert.Equal("\"C:\\Program Files\\Famo\\FamoRuntime.exe\" --control deploy", cmd);
    }

    [Fact]
    public void BuildCommand_ReloadStyle_AppendsReloadStyleArg()
    {
        string cmd = DeployService.BuildCommand(
            @"C:\Program Files\Famo\FamoRuntime.exe", DeployService.ReloadStyleArgs);
        Assert.Equal("\"C:\\Program Files\\Famo\\FamoRuntime.exe\" --control reload-style", cmd);
    }

    [Fact]
    public void BuildCommand_ResetUserDictionary_AppendsDedicatedControlArg()
    {
        string cmd = DeployService.BuildCommand(
            @"C:\Program Files\Famo\FamoRuntime.exe", DeployService.ResetUserDictionaryArgs);

        Assert.Equal(@"""C:\Program Files\Famo\FamoRuntime.exe"" --control reset-user-dictionary", cmd);
    }

    [Fact]
    public void ReloadStyle_WhenRuntimeMissing_ReturnsNotStarted_NoThrow()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-none-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        try
        {
            ReloadResult r = DeployService.ReloadStyle(dir);
            Assert.False(r.Started);
            Assert.Null(r.RuntimePath);
            Assert.Contains("--control reload-style", r.Command);
            Assert.Equal(DeployQueueStatus.Failed, r.Status);
            Assert.True(r.RetryAvailable);
            Assert.NotNull(r.Error);
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void TriggerReload_WhenRuntimeMissing_ReturnsNotStarted_NoThrow()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-none-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        try
        {
            ReloadResult r = DeployService.TriggerReload(ReloadKind.InstantStyle, dir);
            Assert.False(r.Started);
            Assert.Null(r.RuntimePath);
            Assert.Contains("--control reload-style", r.Command);
            Assert.Equal(DeployQueueStatus.Failed, r.Status);
            Assert.True(r.RetryAvailable);
            Assert.NotNull(r.Error);
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void BuildCommand_ReloadOptions_AppendsReloadOptionsArg()
    {
        string cmd = DeployService.BuildCommand(
            @"C:\Program Files\Famo\FamoRuntime.exe", DeployService.ReloadOptionsArgs);
        Assert.Equal("\"C:\\Program Files\\Famo\\FamoRuntime.exe\" --control reload-options", cmd);
    }

    [Fact]
    public void ReloadOptions_WhenRuntimeMissing_ReturnsNotStarted_NoThrow()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-none-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        try
        {
            ReloadResult r = DeployService.ReloadOptions(dir);
            Assert.False(r.Started);
            Assert.Contains("--control reload-options", r.Command);
            Assert.Equal(DeployQueueStatus.Failed, r.Status);
            Assert.True(r.RetryAvailable);
            Assert.NotNull(r.Error);
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void BuildCommand_SelectSchema_AppendsSelectSchemaArg()
    {
        string cmd = DeployService.BuildCommand(
            @"C:\Program Files\Famo\FamoRuntime.exe", DeployService.SelectSchemaArgs);
        Assert.Equal("\"C:\\Program Files\\Famo\\FamoRuntime.exe\" --control select-schema", cmd);
    }

    [Fact]
    public void SelectSchema_WhenRuntimeMissing_ReturnsNotStarted_NoThrow()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-none-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        try
        {
            ReloadResult r = DeployService.SelectSchema(dir);
            Assert.False(r.Started);
            Assert.Contains("--control select-schema", r.Command);
            Assert.Equal(DeployQueueStatus.Failed, r.Status);
            Assert.True(r.RetryAvailable);
            Assert.NotNull(r.Error);
        }
        finally { Directory.Delete(dir, true); }
    }

    [Theory]
    [InlineData(ReloadKind.InstantStyle, DeployService.ReloadStyleArgs)]
    [InlineData(ReloadKind.InstantOptions, DeployService.ReloadOptionsArgs)]
    [InlineData(ReloadKind.SchemaSelect, DeployService.SelectSchemaArgs)]
    [InlineData(ReloadKind.FullDeploy, DeployService.DeployArgs)]
    public void TriggerReload_RoutesKindToExpectedCommand(ReloadKind kind, string expectedArgs)
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-route-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "FamoRuntime.exe"), "stub");
        var calls = new List<string>();

        using IDisposable reset = DeployService.UseProcessRunnerForTests((psi, _) =>
        {
            calls.Add(psi.Arguments);
            return 0;
        });

        try
        {
            ReloadResult r = DeployService.TriggerReload(kind, dir);
            Assert.True(r.Started);
            Assert.Equal(DeployQueueStatus.Pending, r.Status);
            Assert.True(DeployService.WaitForIdleForTests(TimeSpan.FromSeconds(2)));

            Assert.Equal(new[] { expectedArgs }, calls);
            DeployQueueSnapshot snapshot = DeployService.GetQueueSnapshot();
            Assert.Equal(DeployQueueStatus.Succeeded, snapshot.Status);
            Assert.Contains(expectedArgs, snapshot.Command);
        }
        finally
        {
            ClearEnv();
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void ReloadQueue_RecordsFailureAsRetryable()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-fail-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "FamoRuntime.exe"), "stub");

        using IDisposable reset = DeployService.UseProcessRunnerForTests((_, _) => -2);

        try
        {
            Assert.True(DeployService.ReloadOptions(dir).Started);
            Assert.True(DeployService.WaitForIdleForTests(TimeSpan.FromSeconds(2)));

            DeployQueueSnapshot snapshot = DeployService.GetQueueSnapshot();
            Assert.Equal(DeployQueueStatus.Failed, snapshot.Status);
            Assert.True(snapshot.RetryAvailable);
            Assert.Contains("--control reload-options", snapshot.Command);
            Assert.Contains("exit=-2", snapshot.Error);
        }
        finally
        {
            ClearEnv();
            Directory.Delete(dir, true);
        }
    }

    [Theory]
    [InlineData(16, "无法读取用户词典目录", true)]
    [InlineData(17, "用户词典可能不完整", false)]
    public void ResetUserDictionary_MapsNativeFailureToActionableStatus(
        int exitCode, string expected, bool retryAvailable)
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-userdb-fail-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "FamoRuntime.exe"), "stub");
        using IDisposable reset = DeployService.UseProcessRunnerForTests((_, _) => exitCode);

        try
        {
            Assert.True(DeployService.ResetUserDictionary(dir).Started);
            Assert.True(DeployService.WaitForIdleForTests(TimeSpan.FromSeconds(2)));
            DeployQueueSnapshot snapshot = DeployService.GetQueueSnapshot();
            Assert.Equal(DeployQueueStatus.Failed, snapshot.Status);
            Assert.Equal(retryAvailable, snapshot.RetryAvailable);
            Assert.Contains(expected, snapshot.Error);
        }
        finally
        {
            ClearEnv();
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void RetryFailed_RequeuesSameOperation_WithNewRequestIdentity()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-retry-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "FamoRuntime.exe"), "stub");
        int calls = 0;

        using IDisposable reset = DeployService.UseProcessRunnerForTests((psi, _) =>
        {
            Assert.Equal(DeployService.ReloadOptionsArgs, psi.Arguments);
            return Interlocked.Increment(ref calls) == 1 ? 4 : 0;
        });

        try
        {
            ReloadResult failed = DeployService.ReloadOptions(dir);
            Assert.True(DeployService.WaitForIdleForTests(TimeSpan.FromSeconds(2)));
            Assert.Equal(DeployQueueStatus.Failed, DeployService.GetQueueSnapshot().Status);

            ReloadResult retried = DeployService.Retry(failed.RequestId);
            Assert.True(retried.Started);
            Assert.NotEqual(failed.RequestId, retried.RequestId);
            Assert.True(DeployService.WaitForIdleForTests(TimeSpan.FromSeconds(2)));
            Assert.Equal(2, calls);
            Assert.Equal(DeployQueueStatus.Succeeded, DeployService.GetQueueSnapshot().Status);
            Assert.Equal(retried.RequestId, DeployService.GetQueueSnapshot().RequestId);
        }
        finally
        {
            ClearEnv();
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void ReloadQueue_RecordsProcessExceptionAsRetryable()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-throw-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "FamoRuntime.exe"), "stub");

        using IDisposable reset = DeployService.UseProcessRunnerForTests((_, _) =>
            throw new InvalidOperationException("boom"));

        try
        {
            Assert.True(DeployService.ReloadStyle(dir).Started);
            Assert.True(DeployService.WaitForIdleForTests(TimeSpan.FromSeconds(2)));

            DeployQueueSnapshot snapshot = DeployService.GetQueueSnapshot();
            Assert.Equal(DeployQueueStatus.Failed, snapshot.Status);
            Assert.True(snapshot.RetryAvailable);
            Assert.Contains("--control reload-style", snapshot.Command);
            Assert.Contains("boom", snapshot.Error);
        }
        finally
        {
            ClearEnv();
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void ReloadQueue_NotifiesSnapshotChanges()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-notify-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "FamoRuntime.exe"), "stub");

        var snapshots = new List<DeployQueueSnapshot>();
        using IDisposable reset = DeployService.UseProcessRunnerForTests((_, _) => 0);
        Action<DeployQueueSnapshot> handler = snapshot =>
        {
            lock (snapshots)
            {
                snapshots.Add(snapshot);
            }
        };

        DeployService.QueueChanged += handler;
        try
        {
            Assert.True(DeployService.ReloadOptions(dir).Started);
            Assert.True(DeployService.WaitForIdleForTests(TimeSpan.FromSeconds(2)));

            DeployQueueStatus[] statuses;
            lock (snapshots)
            {
                statuses = snapshots.Select(snapshot => snapshot.Status).ToArray();
            }

            Assert.Contains(DeployQueueStatus.Pending, statuses);
            Assert.Contains(DeployQueueStatus.Running, statuses);
            Assert.Contains(DeployQueueStatus.Succeeded, statuses);
        }
        finally
        {
            DeployService.QueueChanged -= handler;
            ClearEnv();
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void ReloadQueue_SubscriberExceptionDoesNotEscapeOrStallWorker()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-observer-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "FamoRuntime.exe"), "stub");
        int processCalls = 0;
        int healthyNotifications = 0;

        using IDisposable reset = DeployService.UseProcessRunnerForTests((_, _) =>
        {
            Interlocked.Increment(ref processCalls);
            return 0;
        });
        Action<DeployQueueSnapshot> broken = _ => throw new InvalidOperationException("observer boom");
        Action<DeployQueueSnapshot> healthy = _ => Interlocked.Increment(ref healthyNotifications);

        DeployService.QueueChanged += broken;
        DeployService.QueueChanged += healthy;
        try
        {
            ReloadResult result = default;
            Exception? error = Record.Exception(() =>
            {
                result = DeployService.ReloadOptions(dir);
            });

            Assert.Null(error);
            Assert.True(result.Started);
            Assert.True(DeployService.WaitForIdleForTests(TimeSpan.FromSeconds(2)));
            Assert.Equal(1, processCalls);
            Assert.True(healthyNotifications >= 3);
            Assert.Equal(DeployQueueStatus.Succeeded, DeployService.GetQueueSnapshot().Status);
        }
        finally
        {
            DeployService.QueueChanged -= broken;
            DeployService.QueueChanged -= healthy;
            ClearEnv();
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void ReloadQueue_WhileRunning_CoalescesPerCommandKind()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-queue-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "FamoRuntime.exe"), "stub");

        var firstStarted = new ManualResetEventSlim(false);
        var releaseFirst = new ManualResetEventSlim(false);
        var calls = new List<string>();

        using IDisposable reset = DeployService.UseProcessRunnerForTests((psi, _) =>
        {
            lock (calls)
            {
                calls.Add(psi.Arguments);
                if (calls.Count == 1)
                {
                    firstStarted.Set();
                }
            }

            if (psi.Arguments == DeployService.ReloadStyleArgs)
            {
                Assert.True(releaseFirst.Wait(TimeSpan.FromSeconds(2)));
            }

            return 0;
        });

        try
        {
            Assert.True(DeployService.ReloadStyle(dir).Started);
            Assert.True(firstStarted.Wait(TimeSpan.FromSeconds(2)));
            Assert.Equal(DeployQueueStatus.Running, DeployService.GetQueueSnapshot().Status);

            Assert.True(DeployService.ReloadOptions(dir).Started);
            Assert.True(DeployService.ReloadOptions(dir).Started);
            Assert.True(DeployService.TriggerReload(ReloadKind.FullDeploy, dir).Started);
            Assert.True(DeployService.SelectSchema(dir).Started);
            Assert.Equal(DeployQueueStatus.Pending, DeployService.GetQueueSnapshot().Status);

            releaseFirst.Set();
            Assert.True(DeployService.WaitForIdleForTests(TimeSpan.FromSeconds(2)));

            Assert.Equal(
                new[]
                {
                    DeployService.ReloadStyleArgs,
                    DeployService.DeployArgs,
                    DeployService.ReloadOptionsArgs,
                    DeployService.SelectSchemaArgs,
                },
                calls);
        }
        finally
        {
            ClearEnv();
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void ReloadQueue_RepeatedCommandKeepsDistinctRequestIdentity()
    {
        ClearEnv();
        string dir = Path.Combine(Path.GetTempPath(), $"famo-repeat-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "FamoRuntime.exe"), "stub");

        var firstStarted = new ManualResetEventSlim(false);
        var releaseFirst = new ManualResetEventSlim(false);
        var snapshots = new List<DeployQueueSnapshot>();
        int runCount = 0;

        using IDisposable reset = DeployService.UseProcessRunnerForTests((_, _) =>
        {
            if (Interlocked.Increment(ref runCount) == 1)
            {
                firstStarted.Set();
                Assert.True(releaseFirst.Wait(TimeSpan.FromSeconds(2)));
            }

            return 0;
        });
        Action<DeployQueueSnapshot> handler = snapshot =>
        {
            lock (snapshots)
            {
                snapshots.Add(snapshot);
            }
        };

        DeployService.QueueChanged += handler;
        try
        {
            ReloadResult first = DeployService.ReloadStyle(dir);
            Assert.True(firstStarted.Wait(TimeSpan.FromSeconds(2)));
            ReloadResult latest = DeployService.ReloadStyle(dir);

            Assert.NotEqual(first.RequestId, latest.RequestId);
            releaseFirst.Set();
            Assert.True(DeployService.WaitForIdleForTests(TimeSpan.FromSeconds(2)));

            DeployQueueSnapshot[] succeeded;
            lock (snapshots)
            {
                succeeded = snapshots
                    .Where(snapshot => snapshot.Status == DeployQueueStatus.Succeeded)
                    .ToArray();
            }

            Assert.Equal(new[] { first.RequestId, latest.RequestId }, succeeded.Select(snapshot => snapshot.RequestId));
            Assert.Equal(latest.RequestId, DeployService.GetQueueSnapshot().RequestId);
        }
        finally
        {
            DeployService.QueueChanged -= handler;
            ClearEnv();
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void TestProcessRunner_DoesNotWriteProductionDiagnostics()
    {
        Action<string> productionFailureLogger = DeployService.FailureLogger;
        Action<string, string, TimeSpan, string> productionTimingLogger = DeployService.TimingLogger;

        using (DeployService.UseProcessRunnerForTests((_, _) => 0))
        {
            Assert.NotSame(productionFailureLogger, DeployService.FailureLogger);
            Assert.NotSame(productionTimingLogger, DeployService.TimingLogger);
            DeployService.FailureLogger("synthetic failure");
            DeployService.TimingLogger("deployQueue", DeployService.DeployArgs, TimeSpan.Zero, "synthetic");
        }

        Assert.Same(productionFailureLogger, DeployService.FailureLogger);
        Assert.Same(productionTimingLogger, DeployService.TimingLogger);
    }
}
