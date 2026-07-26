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
    private static string SingleInstanceKey => $"{SingleInstanceKeyPrefix}.{BuildKeySegment()}";

    [STAThread]
    private static int Main(string[] args)
    {
        // Installer/headless modes must not enter XAML. A publish missing PRI/XBF resources
        // should not break per-user seed or demo smoke commands.
        if (HasFlag(args, "--seed-only"))
        {
            return RunSeedOnly(args);
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

        bool isRedirect = DecideRedirection();
        if (isRedirect)
        {
            return 0; // 已重定向到主实例，本进程退出。
        }

        Microsoft.UI.Xaml.Application.Start(p =>
        {
            var ctx = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(ctx);
            _ = new App();
        });
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

    private static void WriteHeadlessOverlays(FamoSettings settings)
    {
        ConfigWriter.WriteStyleOverlay(settings, FamoPaths.FamoDir);
        ConfigWriter.WriteOptionsOverlay(settings, FamoPaths.FamoDir);
        ConfigWriter.WriteSelectSchema(settings, FamoPaths.FamoDir);
        ConfigWriter.WriteDeployBucket(settings, FamoPaths.FamoDir);
    }

    /// <summary>注册单实例 key。本实例为主 → 订阅 Activated 返 false；否则写 handoff + 重定向返 true。</summary>
    private static bool DecideRedirection()
    {
        AppActivationArguments activation = AppInstance.GetCurrent().GetActivatedEventArgs();
        AppInstance keyInstance = AppInstance.FindOrRegisterForKey(SingleInstanceKey);

        if (keyInstance.IsCurrent)
        {
            keyInstance.Activated += OnActivated;
            return false;
        }

        // 已有主实例：把本次请求的 page 交给主实例（handoff 文件，不依赖激活参数携带命令行）。
        App.WritePendingPage(GetPageArg(Environment.GetCommandLineArgs()));
        RedirectActivationTo(activation, keyInstance);
        return true;
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
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateEvent(IntPtr lpEventAttributes, bool bManualReset, bool bInitialState, string? lpName);

    [DllImport("kernel32.dll")]
    private static extern bool SetEvent(IntPtr hEvent);

    [DllImport("ole32.dll")]
    private static extern uint CoWaitForMultipleObjects(uint dwFlags, uint dwMilliseconds, ulong nHandles, IntPtr[] pHandles, out uint dwIndex);

    private static void RedirectActivationTo(AppActivationArguments args, AppInstance keyInstance)
    {
        IntPtr eventHandle = CreateEvent(IntPtr.Zero, true, false, null);
        Task.Run(() =>
        {
            keyInstance.RedirectActivationToAsync(args).AsTask().Wait();
            SetEvent(eventHandle);
        });
        const uint CWMO_DEFAULT = 0;
        const uint INFINITE = 0xFFFFFFFF;
        _ = CoWaitForMultipleObjects(CWMO_DEFAULT, INFINITE, 1, new[] { eventHandle }, out _);
    }
}
