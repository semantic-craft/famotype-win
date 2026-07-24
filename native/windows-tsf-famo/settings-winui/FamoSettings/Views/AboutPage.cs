using System.Diagnostics;
using System.Reflection;
using Famo.Settings.Core;
using Famo.Settings.Theming;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Famo.Settings.Views;

/// <summary>关于（关）—— 版本 + 开源许可。</summary>
public sealed class AboutPage : UserControl
{
    private const string ReleasesUrl = "https://github.com/semantic-craft/famotype-win/releases/latest";

    public AboutPage()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("关于", "法墨输入法 · Windows。面向中文法律写作的本地输入工具。"));

        var status = FamoUI.StatusRow("点「刷新配置」可重建索引；点「检查更新」会打开法墨发布页。");
        TextBlock statusText = (TextBlock)(status.Child as StackPanel)!.Children[1];

        sp.Children.Add(FamoUI.Card("版本",
            FamoUI.Row("当前版本", null, FamoUI.Value(ReadAppVersion()), divider: false),
            FamoUI.Row("产品身份", "Windows TSF 输入服务 + 设置面板。", FamoUI.Value("Famo Input Method")),
            FamoUI.Row("运行组件", "安装版组件名与系统入口保持法墨命名。", FamoUI.Value("FamoTsf.dll / FamoRuntime.exe")),
            FamoUI.Row("软件更新", "打开法墨发布页手动检查 Windows 安装包；不声称后台自动更新已接入。",
                FamoUI.ActionButton("检查更新", () => CheckForUpdates(statusText)))));

        sp.Children.Add(FamoUI.Card("维护与诊断",
            FamoUI.Row("配置底座", "当前生效的拼音 / 五笔底座。", FamoUI.Value("rime-ice · 极点五笔"), divider: false),
            FamoUI.Row("配置目录", "法墨独立目录，不影响其他输入法配置；点右侧路径在资源管理器中打开。", OpenFolderValue(FamoPaths.FamoDir))));

        sp.Children.Add(FamoUI.FilledButton("刷新配置", "", () =>
        {
            ReloadResult r = DeployService.TriggerReload(ReloadKind.FullDeploy);
            App.ReportReloadResult(
                r,
                statusText,
                pending: $"已触发刷新配置：{r.Command}",
                running: "正在刷新配置…",
                succeeded: "配置刷新已完成。",
                failedPrefix: "刷新配置失败");
        }));
        sp.Children.Add(status);

        sp.Children.Add(FamoUI.Card("开源与第三方声明",
            FamoUI.Row("本程序许可", "法墨当前包含由 Weasel 改写的 GPL-3.0 组件；修改与源码获取信息见 THIRD-PARTY-NOTICES。", FamoUI.Value("GPL-3.0"), divider: false),
            FamoUI.Row("RIME 兼容层", "librime / RIME schema / OpenCC 等组件来源，见 THIRD-PARTY-NOTICES。", FamoUI.Value("BSD / GPL")),
            FamoUI.Row("拼音底座", "rime-ice 雾凇拼音及其词库来源，见 THIRD-PARTY-NOTICES。", FamoUI.Value("GPL-3.0")),
            FamoUI.Row("五笔", "rime-wubi86-jidian · KyleBing。", FamoUI.Value("Apache-2.0")),
            FamoUI.Row("第三方组件", "librime / Boost 等，见 THIRD-PARTY-NOTICES。", FamoUI.Value("BSD / BSL"))));

        Content = sp;
    }

    /// <summary>与 Program.BuildKeySegment 相同的版本读取逻辑；不做“已是最新”的假断言。</summary>
    private static string ReadAppVersion()
    {
        string? version = typeof(AboutPage).Assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion;
        return string.IsNullOrWhiteSpace(version)
            ? typeof(AboutPage).Assembly.GetName().Version?.ToString() ?? "dev"
            : version;
    }

    private static void CheckForUpdates(TextBlock status)
    {
        try
        {
            Process.Start(new ProcessStartInfo { FileName = ReleasesUrl, UseShellExecute = true });
            status.Text = "已打开法墨发布页，请对照当前版本下载安装包。";
        }
        catch (Exception ex)
        {
            status.Text = "未能打开发布页：" + ex.Message;
        }
    }

    /// <summary>只读路径值，点击在资源管理器中打开（对应 macOS openRimeFolder()）。</summary>
    private static Button OpenFolderValue(string path)
    {
        var b = new Button
        {
            Content = FamoUI.Value(path),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            BorderThickness = new Thickness(0),
            Padding = new Thickness(0),
        };
        b.Click += (_, _) =>
        {
            Directory.CreateDirectory(path);
            Process.Start(new ProcessStartInfo { FileName = path, UseShellExecute = true });
        };
        return b;
    }
}
