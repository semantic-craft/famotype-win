using System.Runtime.InteropServices;
using System.Reflection;
using System.Text;
using Famo.Settings.Core;
using Microsoft.UI.Dispatching;
using Microsoft.Windows.AppLifecycle;

namespace Famo.Settings;

/// <summary>
/// 自定义入口（DISABLE_XAML_GENERATED_MAIN）实现 unpackaged WinUI 3 单实例：
/// 已有实例在跑时，新启动的进程把请求的 --page 写到 handoff 文件后重定向激活到主实例并退出，
/// 主实例 Activated 回调读 handoff → 置前窗口 + 导航，绝不开第二个窗口。
/// </summary>
public static class Program
{
    private const string SingleInstanceKeyPrefix = "Famo.Settings.SingleInstance";
    private const uint RedirectTimeoutMilliseconds = 10_000;
    private static string SingleInstanceKey => $"{SingleInstanceKeyPrefix}.{BuildKeySegment()}";
    private enum RedirectionDecision { Primary, Redirected, Failed }

    [STAThread]
    private static int Main(string[] args)
    {
        // Installer/headless modes must not enter XAML. A publish missing PRI/XBF resources
        // should not break per-user seed or demo smoke commands.
        if (HasFlag(args, "--seed-only"))
        {
            return RunSeedOnly(args);
        }
        if (TryGetOption(args, "--prepare-seed-transaction", out string? prepareId))
        {
            return PrepareSeedTransaction(prepareId!);
        }
        if (TryGetTwoOptions(args, "--apply-seed-transaction",
                out string? applyId, out string? applyHash))
        {
            return ApplySeedTransaction(args, applyId!, applyHash!);
        }
        if (TryGetTwoOptions(args, "--rollback-seed-transaction",
                out string? rollbackId, out string? rollbackHash))
        {
            return SeedFileTransaction.Rollback(rollbackId!, rollbackHash!) ? 0 : 1;
        }
        if (TryGetTwoOptions(args, "--commit-seed-transaction",
                out string? commitId, out string? commitHash))
        {
            return SeedFileTransaction.Commit(commitId!, commitHash!) ? 0 : 1;
        }
        if (TryGetOption(args, "--discard-seed-transaction", out string? discardId))
        {
            return SeedFileTransaction.DiscardPrepared(discardId!) ? 0 : 1;
        }
        if (HasFlag(args, "--is-input-tip"))
        {
            return InputMethodList.TryIsFamoInUserList(out bool present)
                ? (present ? 0 : 1)
                : 2;
        }
        if (HasFlag(args, "--remove-input-tip"))
        {
            // 卸载链路：把法墨从当前用户输入法列表移除（文件删除/反注册由安装器负责）。
            return InputMethodList.RemoveFamoFromUserList() ? 0 : 1;
        }
        if (HasFlag(args, "--add-input-tip"))
        {
            return InputMethodList.EnsureFamoInUserList() ? 0 : 1;
        }
        if (HasFlag(args, "--demo-appearance"))
        {
            return RunDemoAppearance();
        }

        WinRT.ComWrappersSupport.InitializeComWrappers();

        RedirectionDecision redirection = DecideRedirection();
        if (redirection != RedirectionDecision.Primary)
        {
            return redirection == RedirectionDecision.Redirected ? 0 : 2;
        }

        Microsoft.UI.Xaml.Application.Start(p =>
        {
            var ctx = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(ctx);
            _ = new App();
        });
        App.StopUpdates();
        return 0;
    }

    private static int RunSeedOnly(string[] args)
    {
        try
        {
            FirstLaunchSeeder.SeedFromInstalledData(force: HasFlag(args, "--force"));
            FamoSettings settings = new SettingsStore().Load();
            WriteHeadlessOverlays(settings);
            // 注册 ≠ 进列表：regsvr32 只做机器级注册，这里以原始用户身份把法墨
            // 加进当前用户输入法列表（幂等、失败不阻断 seed）。
            if (!InputMethodList.EnsureFamoInUserList())
            {
                FamoLog.Append("EnsureFamoInUserList returned false (Famo not added to user input list)");
            }
            else if (!HasFlag(args, "--no-activate") &&
                     !InputMethodList.ActivateFamoForCurrentDesktop())
            {
                FamoLog.Append("ActivateFamoForCurrentDesktop returned false (Famo remains available via Win+Space)");
            }
            return 0;
        }
        catch (Exception ex)
        {
            // 失败可见（P1-B）：Inno [Run] 不看退出码，落一行日志留证。
            FamoLog.Append($"--seed-only failed: {ex.Message}");
            return 1;
        }
    }

    private static int PrepareSeedTransaction(string transactionId)
    {
        try
        {
            string installedData = FirstLaunchSeeder.ResolveInstalledDataDir();
            string receiptHash = SeedFileTransaction.Prepare(
                transactionId,
                installedData,
                stagedRoot =>
                {
                    FirstLaunchSeeder.Seed(installedData, stagedRoot);
                    var stagedStore = new SettingsStore(
                        Path.Combine(stagedRoot, "famo-settings.json"));
                    FamoSettings settings = stagedStore.Load();
                    WriteHeadlessOverlays(settings, stagedRoot);
                });
            Console.WriteLine($"seed_receipt_hash={receiptHash}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"--prepare-seed-transaction failed: {ex.Message}");
            return 1;
        }
    }

    private static int ApplySeedTransaction(
        string[] args, string transactionId, string receiptHash)
    {
        try
        {
            if (!SeedFileTransaction.ApplyPrepared(transactionId, receiptHash))
            {
                return 1;
            }
            if (!InputMethodList.EnsureFamoInUserList())
            {
                Console.Error.WriteLine(
                    "transactional EnsureFamoInUserList returned false");
                return 1;
            }
            if (!HasFlag(args, "--no-activate") &&
                !InputMethodList.ActivateFamoForCurrentDesktop(logFailures: false))
            {
                Console.Error.WriteLine(
                    "transactional ActivateFamoForCurrentDesktop returned false");
            }
            return 0;
        }
        catch (Exception ex)
        {
            FamoLog.Append(
                $"--apply-seed-transaction failed: {ex.Message}");
            Console.Error.WriteLine($"--apply-seed-transaction failed: {ex.Message}");
            return 1;
        }
    }

    private static int RunDemoAppearance()
    {
        try
        {
            var store = new SettingsStore();
            FamoSettings settings = store.Load();
            settings.Appearance.Skin = "wuda";
            settings.Appearance.FontPoint = 18;
            settings.Appearance.Orientation = "vertical";
            settings.Appearance.Layout.CornerRadius = 12;
            store.Save(settings);
            WriteHeadlessOverlays(settings);
            ConfigWriter.WriteInstantBucket(settings, FamoPaths.FamoDir);
            DeployService.ReloadStyle();
            return 0;
        }
        catch
        {
            return 1;
        }
    }

    private static void WriteHeadlessOverlays(FamoSettings settings, string? famoDir = null)
    {
        string target = famoDir ?? FamoPaths.FamoDir;
        ConfigWriter.WriteStyleOverlay(settings, target);
        ConfigWriter.WriteOptionsOverlay(settings, target);
        ConfigWriter.WriteSelectSchema(settings, target);
        ConfigWriter.WriteDeployBucket(settings, target);
    }

    private static bool HasOption(string[] args, string name) =>
        TryGetOption(args, name, out _);

    private static bool TryGetOption(string[] args, string name, out string? value)
    {
        for (int i = 0; i < args.Length - 1; i++)
        {
            if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
            {
                value = args[i + 1];
                return !string.IsNullOrWhiteSpace(value);
            }
        }
        value = null;
        return false;
    }

    private static bool TryGetTwoOptions(
        string[] args, string name, out string? first, out string? second)
    {
        for (int i = 0; i < args.Length - 2; i++)
        {
            if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
            {
                first = args[i + 1];
                second = args[i + 2];
                return !string.IsNullOrWhiteSpace(first) &&
                    !string.IsNullOrWhiteSpace(second);
            }
        }
        first = null;
        second = null;
        return false;
    }

    /// <summary>注册单实例 key。本实例为主 → 订阅 Activated 返 false；否则写 handoff + 重定向返 true。</summary>
    private static RedirectionDecision DecideRedirection()
    {
        AppActivationArguments activation = AppInstance.GetCurrent().GetActivatedEventArgs();
        AppInstance keyInstance = AppInstance.FindOrRegisterForKey(SingleInstanceKey);

        if (keyInstance.IsCurrent)
        {
            keyInstance.Activated += OnActivated;
            return RedirectionDecision.Primary;
        }

        // 已有主实例：把本次请求的 page 交给主实例（handoff 文件，不依赖激活参数携带命令行）。
        App.WritePendingPage(GetPageArg(Environment.GetCommandLineArgs()));
        if (RedirectActivationTo(activation, keyInstance))
        {
            return RedirectionDecision.Redirected;
        }

        // The old primary may have exited while redirection was in flight. Try
        // once to take ownership; otherwise exit with a visible failure code.
        AppInstance retry = AppInstance.FindOrRegisterForKey(SingleInstanceKey);
        if (retry.IsCurrent)
        {
            retry.Activated += OnActivated;
            return RedirectionDecision.Primary;
        }
        return RedirectionDecision.Failed;
    }

    private static string BuildKeySegment()
    {
        string? version = typeof(Program).Assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion;
        if (string.IsNullOrWhiteSpace(version))
        {
            version = typeof(Program).Assembly.GetName().Version?.ToString() ?? "dev";
        }

        var sb = new StringBuilder(version.Length);
        foreach (char c in version)
        {
            sb.Append(char.IsLetterOrDigit(c) || c is '.' or '-' or '_' ? c : '_');
        }
        return sb.Length == 0 ? "dev" : sb.ToString();
    }

    /// <summary>主实例收到重定向激活：交回 UI 线程读 handoff → 置前 + 导航。</summary>
    private static void OnActivated(object? sender, AppActivationArguments args)
    {
        App.OnRedirected();
    }

    private static string? GetPageArg(string[] argv)
    {
        for (int i = 0; i < argv.Length - 1; i++)
        {
            if (string.Equals(argv[i], "--page", StringComparison.OrdinalIgnoreCase))
            {
                return argv[i + 1];
            }
        }
        return null;
    }

    private static bool HasFlag(string[] argv, string flag) =>
        argv.Any(a => string.Equals(a, flag, StringComparison.OrdinalIgnoreCase));

    // ── 重定向需在 STA Main 内同步等待 async 完成（官方模式：事件 + CoWaitForMultipleObjects 泵消息）──
    [DllImport("ole32.dll")]
    private static extern uint CoWaitForMultipleObjects(uint dwFlags, uint dwMilliseconds, ulong nHandles, IntPtr[] pHandles, out uint dwIndex);

    private static bool RedirectActivationTo(AppActivationArguments args, AppInstance keyInstance)
    {
        try
        {
            using var completed = new EventWaitHandle(false, EventResetMode.ManualReset);
            if (completed.SafeWaitHandle.IsInvalid)
            {
                FamoLog.Append("single-instance redirect event creation failed");
                return false;
            }

            int result = 0;
            _ = Task.Run(async () =>
            {
                try
                {
                    await keyInstance.RedirectActivationToAsync(args).AsTask();
                    Volatile.Write(ref result, 1);
                }
                catch (Exception ex)
                {
                    FamoLog.Append($"single-instance redirect failed: {ex.Message}");
                    Volatile.Write(ref result, -1);
                }
                finally
                {
                    try { completed.Set(); }
                    catch (ObjectDisposedException) { }
                }
            });

            const uint CWMO_DEFAULT = 0;
            uint wait = CoWaitForMultipleObjects(
                CWMO_DEFAULT, RedirectTimeoutMilliseconds, 1,
                new[] { completed.SafeWaitHandle.DangerousGetHandle() }, out _);
            if (wait == 0 && Volatile.Read(ref result) == 1) return true;
            if (wait != 0) FamoLog.Append($"single-instance redirect timed out or wait failed: 0x{wait:X8}");
            return false;
        }
        catch (Exception ex)
        {
            FamoLog.Append($"single-instance redirect setup failed: {ex.Message}");
            return false;
        }
    }
}
