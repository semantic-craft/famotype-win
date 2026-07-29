using Famo.Settings.Core.Updates;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class WinSparkleUpdateBackendTests
{
    [Fact]
    public void StartConfiguresSignedFamoFeedAndDailyAutomaticChecks()
    {
        var native = new RecordingWinSparkleNative();
        var backend = new WinSparkleUpdateBackend("1.5.3", () => { }, native);

        backend.Start(automaticChecksEnabled: true);

        Assert.Equal(
            "https://github.com/semantic-craft/famotype-win/releases/latest/download/appcast.xml",
            native.AppcastUrl);
        Assert.Equal("gmOZRp5x2eKXmRczTPlX7hMtVZStjSXJFgovIAw5HdM=", native.EdDsaPublicKey);
        Assert.Equal(("Famo", "法墨输入法", "1.5.3"), native.AppDetails);
        Assert.Equal(@"Software\Famo\InputMethod\Updates", native.RegistryPath);
        Assert.Equal(1, native.AutomaticChecks);
        Assert.Equal(24 * 60 * 60, native.CheckIntervalSeconds);
        Assert.NotNull(native.CanShutdown);
        Assert.NotNull(native.RequestShutdown);
        Assert.True(native.Initialized);
    }

    [Fact]
    public void InvalidUpdatePublicKeyPreventsUpdaterStartup()
    {
        var native = new RecordingWinSparkleNative { PublicKeyAccepted = false };
        var backend = new WinSparkleUpdateBackend("1.5.3", () => { }, native);

        InvalidOperationException error = Assert.Throws<InvalidOperationException>(
            () => backend.Start(automaticChecksEnabled: true));

        Assert.Contains("公钥无效", error.Message);
        Assert.False(native.Initialized);
    }

    [Fact]
    public void InstallerLaunchRequestsGracefulHostShutdown()
    {
        int shutdownRequests = 0;
        var native = new RecordingWinSparkleNative();
        var backend = new WinSparkleUpdateBackend(
            "1.5.3", () => shutdownRequests++, native);
        backend.Start(automaticChecksEnabled: true);

        Assert.Equal(1, native.CanShutdown!());
        native.RequestShutdown!();

        Assert.Equal(1, shutdownRequests);
    }

    private sealed class RecordingWinSparkleNative : IWinSparkleNative
    {
        public string? AppcastUrl { get; private set; }
        public string? EdDsaPublicKey { get; private set; }
        public (string Company, string App, string Version)? AppDetails { get; private set; }
        public string? RegistryPath { get; private set; }
        public int? AutomaticChecks { get; private set; }
        public int? CheckIntervalSeconds { get; private set; }
        public WinSparkleCanShutdownCallback? CanShutdown { get; private set; }
        public WinSparkleShutdownRequestCallback? RequestShutdown { get; private set; }
        public bool Initialized { get; private set; }
        public bool PublicKeyAccepted { get; init; } = true;

        public void SetAppcastUrl(string url) => AppcastUrl = url;

        public int SetEdDsaPublicKey(string publicKey)
        {
            EdDsaPublicKey = publicKey;
            return PublicKeyAccepted ? 1 : 0;
        }

        public void SetAppDetails(string company, string app, string version) =>
            AppDetails = (company, app, version);

        public void SetRegistryPath(string path) => RegistryPath = path;

        public void SetAutomaticCheckForUpdates(int enabled) => AutomaticChecks = enabled;

        public void SetUpdateCheckInterval(int seconds) => CheckIntervalSeconds = seconds;

        public void SetCanShutdownCallback(WinSparkleCanShutdownCallback callback) =>
            CanShutdown = callback;

        public void SetShutdownRequestCallback(WinSparkleShutdownRequestCallback callback) =>
            RequestShutdown = callback;

        public void Initialize() => Initialized = true;

        public void CheckWithUi()
        {
        }

        public void Cleanup()
        {
        }
    }
}
