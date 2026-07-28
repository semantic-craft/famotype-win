using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class InputEntryWinuiContractTests
{
    [Fact]
    public void WeaselFeaturePatch_RoutesTraySettingsToFamoSettingsInputPage()
    {
        string patch = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/launch-settings.patch"));

        // Feature patches apply on the clean upstream pin (still Weasel-named); the deployer
        // exec keeps L"WeaselDeployer.exe" here and is renamed to FamoDeploy.exe by
        // apply-famo-identity.ps1 at the identity stage (see IdentityScript_RenamesRuntimeExeLiterals).
        Assert.Contains("launch_famo_settings", patch);
        Assert.Contains(@"std::bind(execute, dir / L""WeaselDeployer.exe"",", patch);
        Assert.Contains(@"dir / L""settings"" / L""FamoSettings.exe""", patch);
        Assert.Contains(@"dir / L""FamoSettings.exe""", patch);
        Assert.Contains("fs::exists(settings)", patch);
        Assert.Contains(@"execute(settings, L""--page "" + page)", patch);
        Assert.Contains(@"std::bind(launch_famo_settings, dir, std::wstring(L""input""))", patch);
    }

    [Fact]
    public void StatusBarPatch_UsesSameInputSettingsEntry()
    {
        string patch = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/status-bar.patch"));

        Assert.Contains(@"launch_famo_settings(dir, L""input"")", patch);
        Assert.DoesNotContain(@"execute(dir / L""WeaselDeployer.exe"", L""/deploy"")", patch);
    }

    [Fact]
    public void WinuiApp_HasPageDeepLinkAndSingleInstanceRedirect()
    {
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));
        string program = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Program.cs"));
        string mainWindow = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/MainWindow.xaml.cs"));

        Assert.Contains("GetArgValue(argv, \"--page\")", app);
        Assert.Contains("OpenMainWindow(startPage)", app);
        Assert.Contains("new MainWindow(page)", app);
        Assert.Contains("AppInstance.FindOrRegisterForKey", program);
        Assert.Contains("FindOrRegisterForKey(SingleInstanceKey)", program);
        Assert.Contains("AssemblyInformationalVersionAttribute", program);
        Assert.Contains("SingleInstanceKeyPrefix", program);
        Assert.Contains("RedirectActivationTo", program);
        Assert.Contains("WritePendingPage(GetPageArg(Environment.GetCommandLineArgs()))", program);
        Assert.Contains("RedirectTimeoutMilliseconds", program);
        Assert.Contains("finally", program);
        Assert.Contains("completed.Set()", program);
        Assert.Contains("using var completed = new EventWaitHandle", program);
        Assert.Contains("AppInstance retry = AppInstance.FindOrRegisterForKey", program);
        Assert.Contains("RedirectionDecision.Failed", program);
        Assert.DoesNotContain("INFINITE", program);
        Assert.DoesNotContain("CreateEvent(", program);
        Assert.DoesNotContain("private const string SingleInstanceKey = \"Famo.Settings.SingleInstance\";", program);
        Assert.Equal("keyboard", SettingsNavigation.ResolvePageId("input"));
        Assert.Contains("SettingsNavigation.ResolvePageId", mainWindow);
    }

    [Fact]
    public void WinuiApp_HandlesInstallerSeedBeforeXamlStart()
    {
        string program = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Program.cs"));

        Assert.Contains("RunSeedOnly(args)", program);
        Assert.Contains("RunDemoAppearance()", program);
        Assert.Contains("FirstLaunchSeeder.SeedFromInstalledData", program);
        Assert.Contains("WriteHeadlessOverlays(settings)", program);
        Assert.Contains("--prepare-seed-transaction", program);
        Assert.Contains("--apply-seed-transaction", program);
        Assert.Contains("--rollback-seed-transaction", program);
        Assert.Contains("--commit-seed-transaction", program);
        Assert.Contains("WriteHeadlessOverlays(settings, stagedRoot)", program);
        Assert.Contains("ConfigWriter.WriteDeployBucket(settings, target)", program);
        Assert.Contains("ConfigWriter.WriteSelectSchema", program);
        Assert.True(
            program.IndexOf("RunSeedOnly(args)", StringComparison.Ordinal) <
            program.IndexOf("Application.Start", StringComparison.Ordinal),
            "--seed-only must finish before WinUI/XAML Application.Start");
    }

    [Fact]
    public void SettingsProject_PublishesRequiredWinuiResources()
    {
        string project = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/FamoSettings.csproj"));

        Assert.Contains("FamoCopyWinUIResourcesToPublishDir", project);
        Assert.Contains("FamoSettings.pri", project);
        Assert.Contains("App.xbf", project);
        Assert.Contains("MainWindow.xbf", project);
        Assert.Contains(@"Theming\*.xbf", project);
        Assert.Contains("will crash before showing", project);
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
