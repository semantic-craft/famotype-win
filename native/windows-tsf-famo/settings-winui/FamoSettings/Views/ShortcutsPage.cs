using System.Runtime.InteropServices;
using Famo.Settings.Core;
using Famo.Settings.Theming;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace Famo.Settings.Views;

/// <summary>快捷键设置（捷）—— 对齐 macOS shortcuts variant：中英切换、候选翻页、候选快速选词。</summary>
public sealed class ShortcutsPage : UserControl
{
    private static ConvenienceSettings C => App.Settings.Convenience;
    private readonly TextBlock _applyStatus;

    public ShortcutsPage()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("快捷键设置", "功能召唤热键立即应用；输入键位改动会部署 Rime 配置。"));
        sp.Children.Add(FamoUI.Banner(true, "功能召唤立即保存；输入键位需刷新配置"));

        sp.Children.Add(FamoUI.Card("功能召唤",
            HotKeyRow("快捷短语面板", "在任意输入方案下直接打开可搜索的快捷短语面板。",
                () => App.Settings.HotKeys.QuickPhrasePanel,
                value => App.Settings.HotKeys.QuickPhrasePanel = value,
                () => App.Settings.HotKeys.SelectionToolbox,
                divider: false),
            HotKeyRow("划词工具箱", "选中文字后召唤工具箱；双击 Alt 仍是平行入口。",
                () => App.Settings.HotKeys.SelectionToolbox,
                value => App.Settings.HotKeys.SelectionToolbox = value,
                () => App.Settings.HotKeys.QuickPhrasePanel)));

        sp.Children.Add(FamoUI.Card("中英文状态切换",
            FamoUI.Row("Shift 切换中英文", "左右 Shift 上屏当前编码并切到英文；关闭后两侧 Shift 都不切换。",
                KeyToggle(C.ShiftSwitch, v => { C.ShiftSwitch = v; ApplyDeploy(); }, "Shift"), divider: false),
            FamoUI.Row("Caps Lock 切换西文", "开启时 Caps Lock 保持大写锁语义；关闭后用于切换中英文。",
                KeyToggle(C.GoodOldCapsLock, v => { C.GoodOldCapsLock = v; ApplyDeploy(); }, "Caps"), divider: false)));

        sp.Children.Add(FamoUI.Card("候选翻页快捷键",
            FamoUI.Row("减号等号翻页", "用 - 上一页、= 下一页；关闭后恢复普通标点输入。",
                KeyToggle(C.PageMinusEquals, v => { C.PageMinusEquals = v; ApplyDeploy(); }, "−", "="), divider: false),
            FamoUI.Row("左右中括号翻页", "用 [ / ] 上下翻页（同时停用以词定字）。",
                KeyToggle(C.PageBrackets, v => { C.PageBrackets = v; ApplyDeploy(); }, "[", "]")),
            FamoUI.Row("逗号句号翻页", "用 , / . 上下翻页。",
                KeyToggle(C.PageCommaPeriod, v => { C.PageCommaPeriod = v; ApplyDeploy(); }, ",", "."))));

        sp.Children.Add(FamoUI.Card("候选快速选词",
            FamoUI.Row("分号引号选 2 / 3 位", "; 选第 2 个候选、' 选第 3 个。",
                KeyToggle(C.Select23Semicolon, v => { C.Select23Semicolon = v; ApplyDeploy(); }, ";", "'"), divider: false)));

        var statusRow = FamoUI.StatusRow(DefaultStatus);
        _applyStatus = (TextBlock)(statusRow.Child as StackPanel)!.Children[1];
        sp.Children.Add(statusRow);
        Content = sp;
    }

    private const string DefaultStatus = "热键写入 default.custom.yaml · 下次部署生效 · 方案菜单热键（F4 / Ctrl+`）已关闭，方案切换走键盘输入页";
    private const string AppliedStatus = "已发送应用命令。热键写入 default.custom.yaml · 下次部署生效";

    private void ApplyDeploy()
    {
        ReloadResult r = App.SaveAndApplyDeploy();
        App.ReportReloadResult(
            r,
            _applyStatus,
            pending: AppliedStatus,
            running: "正在应用热键配置…",
            succeeded: "热键配置已部署生效。",
            failedPrefix: "热键配置应用失败");
    }

    private FrameworkElement HotKeyRow(
        string title,
        string description,
        Func<string> current,
        Action<string> changed,
        Func<string> counterpart,
        bool divider = true)
    {
        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var record = new Button { Content = DisplayHotKey(current()) };
        var clear = new Button { Content = "清除", IsEnabled = !string.IsNullOrEmpty(current()) };
        bool recording = false;

        void StopRecording(string? hint = null)
        {
            recording = false;
            record.Content = hint ?? DisplayHotKey(current());
        }

        record.Click += (_, _) =>
        {
            if (recording)
            {
                StopRecording();
                return;
            }
            recording = true;
            record.Content = "请按快捷键…";
            record.Focus(FocusState.Programmatic);
        };
        record.LostFocus += (_, _) =>
        {
            if (recording) StopRecording();
        };
        record.KeyDown += (_, e) =>
        {
            if (!recording) return;
            e.Handled = true;
            if (e.Key == VirtualKey.Escape)
            {
                StopRecording();
                return;
            }
            if (e.Key is VirtualKey.Control or VirtualKey.Menu or VirtualKey.Shift
                or VirtualKey.LeftWindows or VirtualKey.RightWindows)
            {
                return;
            }

            int key = (int)e.Key;
            string? binding = HotKeyBinding.Create(
                (char)key,
                IsKeyDown(VK_CONTROL),
                IsKeyDown(VK_MENU),
                IsKeyDown(VK_SHIFT));
            if (binding is null)
            {
                record.Content = "需两个修饰键 + 字母";
                return;
            }
            if (string.Equals(binding, counterpart(), StringComparison.Ordinal))
            {
                record.Content = "已被另一功能占用";
                return;
            }

            changed(binding);
            ApplyGlobalHotKeys();
            clear.IsEnabled = true;
            StopRecording();
        };
        clear.Click += (_, _) =>
        {
            changed(string.Empty);
            ApplyGlobalHotKeys();
            clear.IsEnabled = false;
            StopRecording();
        };

        controls.Children.Add(clear);
        controls.Children.Add(record);
        return FamoUI.Row(title, description, controls, divider);
    }

    private void ApplyGlobalHotKeys()
    {
        ReloadResult result = App.SaveAndApplyInstant();
        App.ReportReloadResult(
            result,
            _applyStatus,
            pending: "功能召唤热键已保存，正在应用…",
            running: "正在应用功能召唤热键…",
            succeeded: "功能召唤热键已生效。",
            failedPrefix: "功能召唤热键应用失败");
    }

    private static string DisplayHotKey(string value) =>
        string.IsNullOrEmpty(value) ? "点击录制" : value;

    private static bool IsKeyDown(int virtualKey) => (GetKeyState(virtualKey) & 0x8000) != 0;

    private const int VK_SHIFT = 0x10;
    private const int VK_CONTROL = 0x11;
    private const int VK_MENU = 0x12;

    [DllImport("user32.dll")]
    private static extern short GetKeyState(int nVirtKey);

    private static StackPanel KeyToggle(bool isOn, Action<bool> changed, params string[] keys)
    {
        var sp = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12, VerticalAlignment = VerticalAlignment.Center };
        sp.Children.Add(KeyCaps(keys));
        sp.Children.Add(FamoUI.Pill(isOn, changed));
        return sp;
    }

    private static StackPanel KeyCaps(params string[] keys)
    {
        var sp = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5, VerticalAlignment = VerticalAlignment.Center };
        for (int i = 0; i < keys.Length; i++)
        {
            if (i > 0)
                sp.Children.Add(new TextBlock { Text = "+", FontSize = 12, Foreground = FamoUI.Br("Famo.Ink3"), VerticalAlignment = VerticalAlignment.Center });
            sp.Children.Add(new Border
            {
                Background = FamoUI.Br("Famo.Field"),
                BorderBrush = FamoUI.Br("Famo.Line"),
                BorderThickness = new Thickness(1, 1, 1, 2),
                CornerRadius = new CornerRadius(6),
                Padding = new Thickness(8, 6, 8, 6),
                MinWidth = 26,
                Child = new TextBlock { Text = keys[i], FontFamily = FamoUI.Mono, FontSize = 12, Foreground = FamoUI.Br("Famo.Ink"), TextAlignment = TextAlignment.Center },
            });
        }
        return sp;
    }
}
