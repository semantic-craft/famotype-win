using Famo.Settings.Core.Updates;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class UpdateCoordinatorTests
{
    [Fact]
    public void StartingAutomaticChecksTwiceIsSafe()
    {
        var backend = new RecordingUpdateBackend();
        var updates = new UpdateCoordinator(backend);

        UpdateActionResult first = updates.Start(automaticChecksEnabled: true);
        UpdateActionResult second = updates.Start(automaticChecksEnabled: true);

        Assert.Equal(UpdateActionStatus.Started, first.Status);
        Assert.Equal(UpdateActionStatus.AlreadyStarted, second.Status);
        Assert.Equal(1, backend.StartCount);
    }

    [Fact]
    public void ManualCheckWorksBeforeAutomaticStartup()
    {
        var backend = new RecordingUpdateBackend();
        var updates = new UpdateCoordinator(backend);

        UpdateActionResult result = updates.CheckNow();

        Assert.Equal(UpdateActionStatus.Started, result.Status);
        Assert.Equal(1, backend.StartCount);
        Assert.Equal(1, backend.CheckCount);
    }

    [Fact]
    public void AutomaticCheckPreferenceChangesImmediately()
    {
        var backend = new RecordingUpdateBackend();
        var updates = new UpdateCoordinator(backend);
        updates.Start(automaticChecksEnabled: true);

        UpdateActionResult result = updates.SetAutomaticChecksEnabled(false);

        Assert.Equal(UpdateActionStatus.Started, result.Status);
        Assert.False(backend.AutomaticChecksEnabled);
    }

    [Fact]
    public void StoppingTwiceCleansUpBackendOnlyOnce()
    {
        var backend = new RecordingUpdateBackend();
        var updates = new UpdateCoordinator(backend);
        updates.Start(automaticChecksEnabled: true);

        updates.Stop();
        updates.Stop();

        Assert.Equal(1, backend.StopCount);
    }

    [Fact]
    public void BackendStartupFailureIsReturnedInsteadOfCrashing()
    {
        var backend = new RecordingUpdateBackend
        {
            StartFailure = new DllNotFoundException("WinSparkle.dll"),
        };
        var updates = new UpdateCoordinator(backend);

        UpdateActionResult result = updates.Start(automaticChecksEnabled: true);

        Assert.Equal(UpdateActionStatus.Failed, result.Status);
        Assert.Contains("WinSparkle.dll", result.Error);
    }

    [Fact]
    public void BackendCleanupFailureDoesNotCrashShutdown()
    {
        var backend = new RecordingUpdateBackend
        {
            StopFailure = new InvalidOperationException("cleanup failed"),
        };
        var updates = new UpdateCoordinator(backend);
        updates.Start(automaticChecksEnabled: true);

        Exception? error = Record.Exception(updates.Stop);

        Assert.Null(error);
    }

    [Fact]
    public void ManualCheckFailureIsReturnedInsteadOfCrashing()
    {
        var backend = new RecordingUpdateBackend
        {
            CheckFailure = new InvalidOperationException("network unavailable"),
        };
        var updates = new UpdateCoordinator(backend);

        UpdateActionResult result = updates.CheckNow();

        Assert.Equal(UpdateActionStatus.Failed, result.Status);
        Assert.Contains("network unavailable", result.Error);
    }

    private sealed class RecordingUpdateBackend : IUpdateBackend
    {
        public int StartCount { get; private set; }
        public int CheckCount { get; private set; }
        public int StopCount { get; private set; }
        public bool AutomaticChecksEnabled { get; private set; }
        public Exception? StartFailure { get; init; }
        public Exception? CheckFailure { get; init; }
        public Exception? StopFailure { get; init; }

        public void Start(bool automaticChecksEnabled)
        {
            StartCount++;
            AutomaticChecksEnabled = automaticChecksEnabled;
            if (StartFailure is not null)
            {
                throw StartFailure;
            }
        }

        public void SetAutomaticChecksEnabled(bool enabled) =>
            AutomaticChecksEnabled = enabled;

        public void CheckNow()
        {
            CheckCount++;
            if (CheckFailure is not null)
            {
                throw CheckFailure;
            }
        }

        public void Stop()
        {
            StopCount++;
            if (StopFailure is not null)
            {
                throw StopFailure;
            }
        }
    }
}
