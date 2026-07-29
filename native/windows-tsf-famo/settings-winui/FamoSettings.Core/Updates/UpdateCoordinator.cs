namespace Famo.Settings.Core.Updates;

public enum UpdateActionStatus
{
    Started,
    AlreadyStarted,
    Failed,
}

public readonly record struct UpdateActionResult(
    UpdateActionStatus Status,
    string? Error = null);

public interface IUpdateBackend
{
    void Start(bool automaticChecksEnabled);
    void SetAutomaticChecksEnabled(bool enabled);
    void CheckNow();
    void Stop();
}

/// <summary>
/// 更新检查的进程级入口。调用方不需要了解更新源、签名、下载或安装器生命周期。
/// </summary>
public sealed class UpdateCoordinator
{
    private readonly IUpdateBackend _backend;
    private readonly object _gate = new();
    private bool _started;
    private bool _stopped;

    public UpdateCoordinator(IUpdateBackend backend)
    {
        _backend = backend ?? throw new ArgumentNullException(nameof(backend));
    }

    public UpdateActionResult Start(bool automaticChecksEnabled)
    {
        lock (_gate)
        {
            if (_started)
            {
                return new UpdateActionResult(UpdateActionStatus.AlreadyStarted);
            }

            try
            {
                _backend.Start(automaticChecksEnabled);
                _started = true;
                return new UpdateActionResult(UpdateActionStatus.Started);
            }
            catch (Exception ex)
            {
                return new UpdateActionResult(UpdateActionStatus.Failed, ex.Message);
            }
        }
    }

    public UpdateActionResult CheckNow()
    {
        lock (_gate)
        {
            try
            {
                if (!_started)
                {
                    _backend.Start(automaticChecksEnabled: false);
                    _started = true;
                }

                _backend.CheckNow();
                return new UpdateActionResult(UpdateActionStatus.Started);
            }
            catch (Exception ex)
            {
                return new UpdateActionResult(UpdateActionStatus.Failed, ex.Message);
            }
        }
    }

    public UpdateActionResult SetAutomaticChecksEnabled(bool enabled)
    {
        lock (_gate)
        {
            try
            {
                if (!_started)
                {
                    _backend.Start(enabled);
                    _started = true;
                }
                else
                {
                    _backend.SetAutomaticChecksEnabled(enabled);
                }

                return new UpdateActionResult(UpdateActionStatus.Started);
            }
            catch (Exception ex)
            {
                return new UpdateActionResult(UpdateActionStatus.Failed, ex.Message);
            }
        }
    }

    public void Stop()
    {
        lock (_gate)
        {
            if (_stopped)
            {
                return;
            }

            _stopped = true;
            if (_started)
            {
                try
                {
                    _backend.Stop();
                }
                catch
                {
                    // 进程退出不能因为 updater 清理失败而反向崩溃。
                }
            }
        }
    }
}
