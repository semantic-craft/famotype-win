using System.Runtime.InteropServices;

namespace Famo.Settings.Core.Updates;

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate int WinSparkleCanShutdownCallback();

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate void WinSparkleShutdownRequestCallback();

internal interface IWinSparkleNative
{
    void SetAppcastUrl(string url);
    int SetEdDsaPublicKey(string publicKey);
    void SetAppDetails(string company, string app, string version);
    void SetRegistryPath(string path);
    void SetAutomaticCheckForUpdates(int enabled);
    void SetUpdateCheckInterval(int seconds);
    void SetCanShutdownCallback(WinSparkleCanShutdownCallback callback);
    void SetShutdownRequestCallback(WinSparkleShutdownRequestCallback callback);
    void Initialize();
    void CheckWithUi();
    void Cleanup();
}

/// <summary>
/// WinSparkle 生产适配器。固定 Windows 独立更新源与公钥，下载包必须通过 EdDSA 验签。
/// </summary>
public sealed class WinSparkleUpdateBackend : IUpdateBackend
{
    internal const string AppcastUrl =
        "https://github.com/semantic-craft/famotype-win/releases/latest/download/appcast.xml";
    internal const string EdDsaPublicKey =
        "gmOZRp5x2eKXmRczTPlX7hMtVZStjSXJFgovIAw5HdM=";
    internal const int UpdateCheckIntervalSeconds = 24 * 60 * 60;

    private readonly string _currentVersion;
    private readonly Action _requestShutdown;
    private readonly IWinSparkleNative _native;
    private readonly WinSparkleCanShutdownCallback _canShutdownCallback;
    private readonly WinSparkleShutdownRequestCallback _shutdownRequestCallback;
    private bool _initialized;

    public WinSparkleUpdateBackend(string currentVersion, Action requestShutdown)
        : this(currentVersion, requestShutdown, new WinSparkleNative())
    {
    }

    internal WinSparkleUpdateBackend(
        string currentVersion,
        Action requestShutdown,
        IWinSparkleNative native)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(currentVersion);
        _currentVersion = currentVersion.Split('+', 2)[0];
        _requestShutdown = requestShutdown ?? throw new ArgumentNullException(nameof(requestShutdown));
        _native = native ?? throw new ArgumentNullException(nameof(native));
        _canShutdownCallback = CanShutdown;
        _shutdownRequestCallback = RequestShutdown;
    }

    public void Start(bool automaticChecksEnabled)
    {
        if (_initialized)
        {
            SetAutomaticChecksEnabled(automaticChecksEnabled);
            return;
        }

        _native.SetAppcastUrl(AppcastUrl);
        if (_native.SetEdDsaPublicKey(EdDsaPublicKey) != 1)
        {
            throw new InvalidOperationException("法墨更新公钥无效。");
        }
        _native.SetAppDetails("Famo", "法墨输入法", _currentVersion);
        _native.SetRegistryPath(@"Software\Famo\InputMethod\Updates");
        _native.SetAutomaticCheckForUpdates(automaticChecksEnabled ? 1 : 0);
        _native.SetUpdateCheckInterval(UpdateCheckIntervalSeconds);
        _native.SetCanShutdownCallback(_canShutdownCallback);
        _native.SetShutdownRequestCallback(_shutdownRequestCallback);
        _native.Initialize();
        _initialized = true;
    }

    public void SetAutomaticChecksEnabled(bool enabled) =>
        _native.SetAutomaticCheckForUpdates(enabled ? 1 : 0);

    public void CheckNow()
    {
        if (!_initialized)
        {
            throw new InvalidOperationException("更新模块尚未启动。");
        }
        _native.CheckWithUi();
    }

    public void Stop()
    {
        if (!_initialized)
        {
            return;
        }

        _initialized = false;
        _native.Cleanup();
    }

    private int CanShutdown() => 1;

    private void RequestShutdown()
    {
        try
        {
            _requestShutdown();
        }
        catch
        {
            // 不能让托管异常越过 native callback。
        }
    }
}

internal sealed class WinSparkleNative : IWinSparkleNative
{
    public void SetAppcastUrl(string url) => NativeSetAppcastUrl(url);
    public int SetEdDsaPublicKey(string publicKey) => NativeSetEdDsaPublicKey(publicKey);
    public void SetAppDetails(string company, string app, string version) =>
        NativeSetAppDetails(company, app, version);
    public void SetRegistryPath(string path) => NativeSetRegistryPath(path);
    public void SetAutomaticCheckForUpdates(int enabled) =>
        NativeSetAutomaticCheckForUpdates(enabled);
    public void SetUpdateCheckInterval(int seconds) => NativeSetUpdateCheckInterval(seconds);
    public void SetCanShutdownCallback(WinSparkleCanShutdownCallback callback) =>
        NativeSetCanShutdownCallback(callback);
    public void SetShutdownRequestCallback(WinSparkleShutdownRequestCallback callback) =>
        NativeSetShutdownRequestCallback(callback);
    public void Initialize() => NativeInitialize();
    public void CheckWithUi() => NativeCheckWithUi();
    public void Cleanup() => NativeCleanup();

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_set_appcast_url",
        CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern void NativeSetAppcastUrl(string url);

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_set_eddsa_public_key",
        CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern int NativeSetEdDsaPublicKey(string publicKey);

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_set_app_details",
        CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern void NativeSetAppDetails(string company, string app, string version);

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_set_registry_path",
        CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern void NativeSetRegistryPath(string path);

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_set_automatic_check_for_updates",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern void NativeSetAutomaticCheckForUpdates(int enabled);

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_set_update_check_interval",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern void NativeSetUpdateCheckInterval(int seconds);

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_set_can_shutdown_callback",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern void NativeSetCanShutdownCallback(
        WinSparkleCanShutdownCallback callback);

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_set_shutdown_request_callback",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern void NativeSetShutdownRequestCallback(
        WinSparkleShutdownRequestCallback callback);

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_init",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern void NativeInitialize();

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_check_update_with_ui",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern void NativeCheckWithUi();

    [DllImport("WinSparkle.dll", EntryPoint = "win_sparkle_cleanup",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern void NativeCleanup();
}
