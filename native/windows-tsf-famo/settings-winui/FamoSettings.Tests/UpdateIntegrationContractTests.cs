using Xunit;

namespace Famo.Settings.Tests;

public sealed class UpdateIntegrationContractTests
{
    [Fact]
    public void SettingsApplicationOwnsUpdaterStartupManualCheckAndShutdown()
    {
        string app = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));
        string program = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings/Program.cs"));

        Assert.Contains("WinSparkleUpdateBackend", app);
        Assert.Contains("DispatcherQueuePriority.Low", app);
        Assert.Contains("Settings.Updates.AutomaticChecksEnabled", app);
        Assert.Contains("public static UpdateActionResult CheckForUpdates()", app);
        Assert.Contains("public static UpdateActionResult SetAutomaticUpdateChecksEnabled", app);
        Assert.Contains("RequestUpdateShutdown", app);
        Assert.Contains("App.StopUpdates();", program);
    }

    [Fact]
    public void InstallerPublishesPinnedWinSparkleWithLicenseNotice()
    {
        string project = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings/FamoSettings.csproj"));
        string notices = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/THIRD-PARTY-NOTICES.txt"));
        string sbom = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/SBOM.spdx.json"));
        string build = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/build-installer.ps1"));
        string license = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/WinSparkle-LICENSE.txt"));

        Assert.Contains("PackageReference Include=\"WinSparkle\" Version=\"0.9.3\"", project);
        Assert.Contains("WinSparkle", notices);
        Assert.Contains("https://github.com/vslavik/winsparkle", notices);
        Assert.Contains("License: MIT", notices);
        Assert.DoesNotContain("no legacy TSF host, server, deployer, or updater binary", notices);
        Assert.Contains("\"name\": \"WinSparkle\"", sbom);
        Assert.Contains("\"versionInfo\": \"0.9.3\"", sbom);
        Assert.Contains("\"relatedSpdxElement\": \"SPDXRef-Package-WinSparkle\"", sbom);
        Assert.Contains("WinSparkle-LICENSE.txt", build);
        Assert.Contains("Copyright (c) 2009-2026 Vaclav Slavik", license);
        Assert.Contains("OpenSSL Project", license);
    }

    [Fact]
    public void ReleaseToolBuildsSignedImmutableWindowsAppcast()
    {
        string release = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/make-appcast.ps1"));
        string selfTest = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/make-appcast-selftest.ps1"));
        string releaseDocs = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/README.md"));

        Assert.Contains("$env:FAMO_UPDATE_PRIVATE_KEY", release);
        Assert.Contains("winsparkle-tool.exe", release);
        Assert.Contains("'public-key'", release);
        Assert.Contains("if ($publicKey -cne $ExpectedPublicKey)", release);
        Assert.Contains("sparkle:edSignature", release);
        Assert.Contains("sparkle:os=\"windows-x64\"", release);
        Assert.Contains(
            "sparkle:installerArguments=\"/SILENT /SP- /NOICONS\"",
            release);
        Assert.DoesNotContain(
            "sparkle:installerArguments=\"/SILENT /SP- /NOICONS /NORESTART\"",
            release);
        Assert.Contains("/releases/download/$AppVersion/", release);
        Assert.DoesNotContain("windows-update-eddsa-private.key", release);
        Assert.Contains("'verify'", selfTest);
        Assert.Contains("'--public-key', $ExpectedPublicKey", selfTest);
        Assert.Contains("make-appcast.ps1", selfTest);
        Assert.Contains("cmd.exe /d /c", releaseDocs);
        Assert.Contains("它不访问真实 GitHub appcast", releaseDocs);
        Assert.Contains("Win10 / Win11", releaseDocs);
    }

    [Fact]
    public void ReleaseMetadataIsPreparedFor1512()
    {
        string build = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/build-installer.ps1"));
        string appcast = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/make-appcast.ps1"));
        string installer = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/famo-setup.iss"));
        string sbom = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/installer/SBOM.spdx.json"));

        Assert.Contains("[string] $AppVersion = '1.5.16'", build);
        Assert.Contains("[string] $AppVersion = '1.5.16'", appcast);
        Assert.Contains("#define AppVersion  \"1.5.16\"", installer);
        Assert.Contains("\"versionInfo\": \"1.5.16\"", sbom);
    }

    private static string RepoFile(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath);
            if (File.Exists(candidate))
            {
                return candidate;
            }
            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate {relativePath}");
    }
}
