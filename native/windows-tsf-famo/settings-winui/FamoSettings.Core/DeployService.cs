using System.Diagnostics;

namespace Famo.Settings.Core;

/// <summary>即时桶/部署桶写盘后触发的 reload 种类。</summary>
public enum ReloadKind
{
    /// <summary>即时桶（weasel.custom.yaml：字号/皮肤/横竖排）。配置重编译快，不重建词典。</summary>
    InstantStyle,

    /// <summary>即时开关桶（famo-options.yaml）。</summary>
    InstantOptions,

    /// <summary>即时方案选择桶（famo-select-schema.txt）。</summary>
    SchemaSelect,

    /// <summary>部署桶（default/rime_ice.custom.yaml：方案/候选数/模糊音）。可能重建 prism。</summary>
    FullDeploy,
}

public enum DeployQueueStatus
{
    Idle,
    Pending,
    Running,
    Succeeded,
    Failed,
}

public readonly record struct DeployQueueSnapshot(
    DeployQueueStatus Status,
    string? Command,
    string? Error,
    bool RetryAvailable)
{
    public long RequestId { get; init; }
}

/// <summary>解析 + 触发的结果（供 UI 提示与取证；找不到 runtime 不阻断 UI）。</summary>
public readonly record struct ReloadResult(
    bool Started,
    string? RuntimePath,
    string Command,
    string? Error)
{
    public DeployQueueStatus Status { get; init; } = Started ? DeployQueueStatus.Pending : DeployQueueStatus.Failed;
    public bool RetryAvailable { get; init; } = !Started;
    public long RequestId { get; init; }
}

/// <summary>
/// 设置面板写盘后以短命 client 模式拉起同一个 <c>FamoRuntime.exe --control ...</c>。
/// client 只连接独立 control endpoint；常驻 runtime 的 FIFO 控制 worker 执行命令，
/// 与按键 endpoint/queue 隔离。部署期间按键由 runtime 立即 fail-open。
/// </summary>
public static class DeployService
{
    /// <summary>部署桶（方案/词典/模糊音）参数：重建 prism。</summary>
    public const string DeployArgs = "--control deploy";

    /// <summary>显式维护动作：备份并清空 Rime 学习数据，再重载引擎。</summary>
    public const string ResetUserDictionaryArgs = "--control reset-user-dictionary";

    /// <summary>即时外观②参数：令运行中的 server 重读 famo-style.yaml 覆盖层并重绘，零部署。</summary>
    public const string ReloadStyleArgs = "--control reload-style";

    /// <summary>即时开关①参数：令运行中的 server 重读 famo-options.yaml 并 set_option 到各会话，零部署。</summary>
    public const string ReloadOptionsArgs = "--control reload-options";

    /// <summary>即时输入方式②参数：令运行中的 server 读 famo-select-schema.txt 并 select_schema 到各会话，零部署。</summary>
    public const string SelectSchemaArgs = "--control select-schema";

    private const string ReleaseRuntimeExe = "FamoRuntime.exe";
    private const int UserDictionaryEnumerationExit = 16;
    private const int UserDictionaryRollbackExit = 17;
    private static readonly TimeSpan ControlTimeout = TimeSpan.FromMinutes(2);
    private static readonly object QueueGate = new();
    private static readonly Dictionary<string, QueuedReload> PendingReloads = new();
    private static readonly Dictionary<long, RetryReload> FailedReloads = new();
    private static readonly string[] DrainOrder = [ResetUserDictionaryArgs, DeployArgs, ReloadOptionsArgs, ReloadStyleArgs, SelectSchemaArgs];
    private static readonly Action<string> ProductionFailureLogger = message => FamoLog.Append(message);
    private static readonly Action<string, string, TimeSpan, string> ProductionTimingLogger =
        (component, operation, elapsed, status) => FamoTimingLog.Append(component, operation, elapsed, status);
    private static long NextRequestId;
    private static bool WorkerRunning;
    private static DeployQueueSnapshot LastSnapshot = new(DeployQueueStatus.Idle, null, null, false);

    public static event Action<DeployQueueSnapshot>? QueueChanged;

    internal static Func<ProcessStartInfo, TimeSpan, int> ProcessRunner { get; set; } = RunProcess;
    internal static Action<string> FailureLogger { get; set; } = ProductionFailureLogger;
    internal static Action<string, string, TimeSpan, string> TimingLogger { get; set; } = ProductionTimingLogger;

    private readonly record struct QueuedReload(
        long RequestId,
        string RuntimePath,
        string Args,
        string Command,
        string? BaseDirectory);
    private readonly record struct RetryReload(string Args, string? BaseDirectory);

    /// <summary>
    /// 解析 native FamoRuntime.exe 路径：
    /// 1) 环境变量 <c>FAMO_RUNTIME</c>（exe 全路径，开发/测试用）；
    /// 2) 环境变量 <c>FAMO_NATIVE_OUTPUT</c>（目录）；
    /// 3) 与设置 exe 同目录或父目录（安装版：{app}\settings\FamoSettings.exe + {app}\FamoRuntime.exe）。
    /// 不回退到 legacy server/deployer。
    /// </summary>
    public static string? ResolveRuntimePath(string? baseDirectory = null)
    {
        string? fromExe = Environment.GetEnvironmentVariable("FAMO_RUNTIME");
        if (!string.IsNullOrWhiteSpace(fromExe) && File.Exists(fromExe))
        {
            return fromExe;
        }

        string? fromDir = Environment.GetEnvironmentVariable("FAMO_NATIVE_OUTPUT");
        if (!string.IsNullOrWhiteSpace(fromDir))
        {
            string candidate = Path.Combine(fromDir, ReleaseRuntimeExe);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        string baseDir = baseDirectory ?? AppContext.BaseDirectory;
        string alongside = Path.Combine(baseDir, ReleaseRuntimeExe);
        if (File.Exists(alongside))
        {
            return alongside;
        }

        string parentCandidate = Path.GetFullPath(Path.Combine(baseDir, "..", ReleaseRuntimeExe));
        if (File.Exists(parentCandidate))
        {
            return parentCandidate;
        }

        return null;
    }

    /// <summary>构造可读命令串（取证/日志用），不实际执行。默认部署 control 命令。</summary>
    public static string BuildCommand(string runtimePath, string args = DeployArgs) =>
        $"\"{runtimePath}\" {args}";

    /// <summary>触发部署：后台排队运行 native control client，不阻塞 UI。</summary>
    public static ReloadResult TriggerReload(ReloadKind kind, string? baseDirectory = null) =>
        Run(ArgsForKind(kind), baseDirectory);

    public static DeployQueueSnapshot GetQueueSnapshot()
    {
        lock (QueueGate)
        {
            return LastSnapshot;
        }
    }

    /// <summary>显式重试一个已失败请求；成功排队后返回新的 request id。</summary>
    public static ReloadResult Retry(long failedRequestId)
    {
        RetryReload retry;
        lock (QueueGate)
        {
            if (!FailedReloads.Remove(failedRequestId, out retry))
            {
                const string error = "该失败请求已失效或已重试。";
                return new ReloadResult(false, null, "(retry unavailable)", error)
                {
                    RetryAvailable = false,
                };
            }
        }

        return Run(retry.Args, retry.BaseDirectory);
    }

    /// <summary>
    /// 即时外观②：请求运行中的 runtime 重读 famo-style.yaml
    /// 覆盖层并重绘（fire-and-forget，零部署、不重建 prism）。server 未运行则静默无操作。
    /// </summary>
    public static ReloadResult ReloadStyle(string? baseDirectory = null) =>
        Run(ReloadStyleArgs, baseDirectory);

    /// <summary>
    /// 即时开关①：请求运行中的 runtime 重读 famo-options.yaml
    /// 并 set_option 到各会话（fire-and-forget，零部署、不重建 prism）。server 未运行则静默无操作。
    /// </summary>
    public static ReloadResult ReloadOptions(string? baseDirectory = null) =>
        Run(ReloadOptionsArgs, baseDirectory);

    /// <summary>
    /// 即时输入方式②：请求运行中的 runtime 读 famo-select-schema.txt
    /// 并 rime_api->select_schema 到各会话（fire-and-forget，零部署、不重建 prism）。server 未运行则静默无操作。
    /// </summary>
    public static ReloadResult SelectSchema(string? baseDirectory = null) =>
        Run(SelectSchemaArgs, baseDirectory);

    public static ReloadResult ResetUserDictionary(string? baseDirectory = null) =>
        Run(ResetUserDictionaryArgs, baseDirectory);

    /// <summary>解析 runtime 后以给定参数排队拉起 control client；找不到/失败不抛。</summary>
    private static ReloadResult Run(string args, string? baseDirectory)
    {
        long requestId = Interlocked.Increment(ref NextRequestId);
        string? path = ResolveRuntimePath(baseDirectory);
        if (path is null)
        {
            string missingCommand = $"({ReleaseRuntimeExe} not found) {args}";
            string error = "FamoRuntime.exe 未找到：设 FAMO_RUNTIME 或随安装版同目录/父目录。";
            RememberFailure(requestId, args, baseDirectory);
            SetSnapshot(new DeployQueueSnapshot(DeployQueueStatus.Failed, missingCommand, error, true)
            {
                RequestId = requestId,
            });
            return new ReloadResult(false, null, missingCommand, error)
            {
                RequestId = requestId,
            };
        }

        string command = BuildCommand(path, args);
        var queued = new QueuedReload(requestId, path, args, command, baseDirectory);

        bool startWorker;
        lock (QueueGate)
        {
            if (WorkerRunning)
            {
                // ponytail: one pending command per bucket; a scheduler is overkill until this is measured.
                PendingReloads[args] = queued;
                startWorker = false;
            }
            else
            {
                WorkerRunning = true;
                startWorker = true;
            }
        }

        SetSnapshot(new DeployQueueSnapshot(DeployQueueStatus.Pending, command, null, false)
        {
            RequestId = requestId,
        });
        if (startWorker)
        {
            _ = Task.Run(() => DrainQueue(queued));
        }

        return new ReloadResult(true, path, command, null)
        {
            RequestId = requestId,
        };
    }

    private static void DrainQueue(QueuedReload current)
    {
        while (true)
        {
            RunOne(current);
            lock (QueueGate)
            {
                if (TakeNextPending() is not { } next)
                {
                    WorkerRunning = false;
                    return;
                }

                current = next;
            }
        }
    }

    private static QueuedReload? TakeNextPending()
    {
        foreach (string args in DrainOrder)
        {
            if (PendingReloads.Remove(args, out QueuedReload reload))
            {
                return reload;
            }
        }

        return null;
    }

    private static string ArgsForKind(ReloadKind kind) => kind switch
    {
        ReloadKind.InstantStyle => ReloadStyleArgs,
        ReloadKind.InstantOptions => ReloadOptionsArgs,
        ReloadKind.SchemaSelect => SelectSchemaArgs,
        ReloadKind.FullDeploy => DeployArgs,
        _ => DeployArgs,
    };

    private static void RunOne(QueuedReload reload)
    {
        var elapsed = Stopwatch.StartNew();
        try
        {
            SetSnapshot(new DeployQueueSnapshot(DeployQueueStatus.Running, reload.Command, null, false)
            {
                RequestId = reload.RequestId,
            });
            var psi = new ProcessStartInfo
            {
                FileName = reload.RuntimePath,
                Arguments = reload.Args,
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = Path.GetDirectoryName(reload.RuntimePath) ?? Environment.CurrentDirectory,
            };
            int exitCode = ProcessRunner(psi, ControlTimeout);
            elapsed.Stop();
            if (exitCode != 0)
            {
                bool retryAvailable = exitCode != UserDictionaryRollbackExit;
                string error = ControlFailureMessage(reload, exitCode);
                if (retryAvailable)
                {
                    RememberFailure(reload.RequestId, reload.Args, reload.BaseDirectory);
                }
                SetSnapshot(new DeployQueueSnapshot(DeployQueueStatus.Failed, reload.Command, error, retryAvailable)
                {
                    RequestId = reload.RequestId,
                });
                TimingLogger("deployQueue", reload.Args, elapsed.Elapsed, $"exit:{exitCode}");
                FailureLogger(error);
                return;
            }

            SetSnapshot(new DeployQueueSnapshot(DeployQueueStatus.Succeeded, reload.Command, null, false)
            {
                RequestId = reload.RequestId,
            });
            TimingLogger("deployQueue", reload.Args, elapsed.Elapsed, "succeeded");
        }
        catch (Exception ex)
        {
            elapsed.Stop();
            string error = $"runtime control failed: {reload.Command}, error={ex.Message}";
            RememberFailure(reload.RequestId, reload.Args, reload.BaseDirectory);
            SetSnapshot(new DeployQueueSnapshot(DeployQueueStatus.Failed, reload.Command, error, true)
            {
                RequestId = reload.RequestId,
            });
            TimingLogger("deployQueue", reload.Args, elapsed.Elapsed, "exception");
            FailureLogger(error);
        }
    }

    private static string ControlFailureMessage(QueuedReload reload, int exitCode) =>
        (reload.Args, exitCode) switch
        {
            (ResetUserDictionaryArgs, UserDictionaryEnumerationExit) =>
                "无法读取用户词典目录（权限或磁盘错误）；未删除任何词典。",
            (ResetUserDictionaryArgs, UserDictionaryRollbackExit) =>
                "删除中断且从 .famo-backup 恢复失败；用户词典可能不完整，请保留备份并打开配置目录检查。",
            _ => $"runtime control failed: {reload.Command}, exit={exitCode}",
        };

    private static void SetSnapshot(DeployQueueSnapshot snapshot)
    {
        Action<DeployQueueSnapshot>? changed;
        lock (QueueGate)
        {
            LastSnapshot = snapshot;
            changed = QueueChanged;
        }

        changed?.Invoke(snapshot);
    }

    private static void RememberFailure(long requestId, string args, string? baseDirectory)
    {
        lock (QueueGate)
        {
            if (FailedReloads.Count >= 64)
            {
                FailedReloads.Remove(FailedReloads.Keys.Min());
            }

            FailedReloads[requestId] = new RetryReload(args, baseDirectory);
        }
    }

    private static int RunProcess(ProcessStartInfo psi, TimeSpan timeout)
    {
        using Process? process = Process.Start(psi);
        if (process is null)
        {
            return -1;
        }

        if (!process.WaitForExit(timeout))
        {
            try { process.Kill(entireProcessTree: true); }
            catch { }
            return -2;
        }

        return process.ExitCode;
    }

    internal static bool WaitForIdleForTests(TimeSpan timeout)
    {
        DateTime deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            lock (QueueGate)
            {
                if (!WorkerRunning && PendingReloads.Count == 0)
                {
                    return true;
                }
            }

            Thread.Sleep(10);
        }

        return false;
    }

    internal static IDisposable UseProcessRunnerForTests(Func<ProcessStartInfo, TimeSpan, int> runner)
    {
        lock (QueueGate)
        {
            PendingReloads.Clear();
            FailedReloads.Clear();
            WorkerRunning = false;
            LastSnapshot = new DeployQueueSnapshot(DeployQueueStatus.Idle, null, null, false);
            QueueChanged = null;
            ProcessRunner = runner;
            FailureLogger = _ => { };
            TimingLogger = (_, _, _, _) => { };
        }

        return new ResetProcessRunner();
    }

    private sealed class ResetProcessRunner : IDisposable
    {
        public void Dispose()
        {
            lock (QueueGate)
            {
                PendingReloads.Clear();
                FailedReloads.Clear();
                WorkerRunning = false;
                LastSnapshot = new DeployQueueSnapshot(DeployQueueStatus.Idle, null, null, false);
                QueueChanged = null;
                ProcessRunner = RunProcess;
                FailureLogger = ProductionFailureLogger;
                TimingLogger = ProductionTimingLogger;
            }
        }
    }
}
