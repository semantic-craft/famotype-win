using Famo.Settings.Theming;
using Microsoft.UI.Xaml.Controls;

namespace Famo.Settings.Views;

/// <summary>悬浮状态栏（浮）—— Windows 独有的输入状态与维护入口。</summary>
public sealed class StatusBarPage : UserControl
{
    public StatusBarPage()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("悬浮状态栏", "Windows 版的输入状态控制。"));

        sp.Children.Add(FamoUI.Card("悬浮状态条",
            FamoUI.Row("语言栏菜单", "右键语言栏图标后可执行「显示悬浮状态条」。该命令由 WeaselServer 真实处理。",
                FamoUI.Value("已接入"), divider: false),
            FamoUI.Row("状态按钮", "中英、标点、简繁、全半角按钮直达当前会话的 Rime option。", FamoUI.Value("即时生效")),
            FamoUI.Row("三点菜单", "输入法设定放在最上面；刷新配置放在输入法设定的维护与诊断里；输入区技能不放在这里。",
                FamoUI.Value("Windows 独有"))));

        Content = sp;
    }
}
