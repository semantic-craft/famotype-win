using Famo.Settings.Core.Ai;
using Famo.Settings.Theming;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Famo.Settings.Views;

/// <summary>技能平台（技）—— 选中文本后显式触发的本地入口与 AI 技能。</summary>
public sealed class SkillsPage : UserControl
{
    public SkillsPage()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("技能平台", "选区到技能的显式入口；不进入 Rime 输入热路径。"));

        sp.Children.Add(FamoUI.Card("划词菜单",
            FamoUI.Row("启用划词菜单", "系统菜单会显示已实现的划词技能入口；每次都由你显式触发。",
                FamoUI.Pill(App.Settings.Ai.SelectionMenuEnabled, v =>
                {
                    App.Settings.Ai.SelectionMenuEnabled = v;
                    App.Store.Save(App.Settings);
                }),
                divider: false),
            FamoUI.Row("辅助功能权限", "优先使用 Windows UI Automation 读取选区；密码或安全输入框会直接停止。",
                FamoUI.Value("无需额外授权")),
            FamoUI.Row("剪贴板兜底", "UIA 无可用文本时才临时发送 Ctrl+C；用 sentinel 区分空选区，并尽量恢复原剪贴板。",
                FamoUI.Value("自动恢复"))));

        sp.Children.Add(FamoUI.Card("云端 AI（全局）",
            FamoUI.Row("启用云端 AI（划词润色 / 任意提问）",
                "关闭时 AI 对话和划词润色不会发起网络请求。",
                FamoUI.Pill(App.Settings.Ai.CloudEnabled, v =>
                {
                    App.Settings.Ai.CloudEnabled = v;
                    App.Store.Save(App.Settings);
                }),
                divider: false),
            FamoUI.Row("隐私边界",
                "AI 只在用户点击 AI 对话或润色时读取对应文本；普通输入、候选排序和上屏仍走本地 Rime。",
                FamoUI.Value("显式触发"))));

        var builtInSkills = FamoUI.Card("内置技能",
            SkillRow(AiSelectionSkills.Polish, "把选中文本改写为更顺畅的书面中文，结果只显示和复制。",
                App.Settings.Ai.PolishSkillEnabled,
                v => { App.Settings.Ai.PolishSkillEnabled = v; App.Store.Save(App.Settings); },
                divider: false),
            SkillRow(AiSelectionSkills.SourceCheck, "从选中文本提取待核验断言、一手来源类型和检索关键词。",
                App.Settings.Ai.SourceCheckSkillEnabled,
                v => { App.Settings.Ai.SourceCheckSkillEnabled = v; App.Store.Save(App.Settings); }),
            SkillRow(AiSelectionSkills.ResearchAssist, "从选中文本生成检索式、追问方向和资料路径。",
                App.Settings.Ai.ResearchAssistSkillEnabled,
                v => { App.Settings.Ai.ResearchAssistSkillEnabled = v; App.Store.Save(App.Settings); }),
            SkillRow(AiSelectionSkills.DocumentFormatting, "把选中文本按公文格式改写：结论先行、公文用语、标准格式。",
                App.Settings.Ai.DocumentFormattingSkillEnabled,
                v => { App.Settings.Ai.DocumentFormattingSkillEnabled = v; App.Store.Save(App.Settings); }),
            SkillRow(AiSelectionSkills.PromptOptimize, "把选中的草稿提示词补齐意图/目标/约束/产出形态四要素；信息不足时先反问再定稿。",
                App.Settings.Ai.PromptOptimizeSkillEnabled,
                v => { App.Settings.Ai.PromptOptimizeSkillEnabled = v; App.Store.Save(App.Settings); }),
            FamoUI.Row("AI 对话", "打开任意提问窗口；只发送你在窗口里主动输入的内容。",
                FamoUI.ActionButton("打开", App.ShowAiConversation)));
        sp.Children.Add(builtInSkills);

        sp.Children.Add(FamoUI.Card("提示词",
            FamoUI.Row("管理提示词库", "打开本地提示词管理页；不进入 Rime 输入热路径。",
                FamoUI.ActionButton("管理", () => App.OpenSettingsPage("prompt-library")), divider: false),
            FamoUI.Row("快速插入提示词", "打开提示词选择器；变量填完后粘贴到先前焦点窗口。",
                FamoUI.ActionButton("插入", App.ShowPromptPicker)),
            FamoUI.Row("保存选中文本为提示词", "先读取当前选区，再打开提示词库编辑器预填内容。",
                FamoUI.ActionButton("保存", App.ShowPromptSaveSelection))));

        Content = sp;
    }

    private static FrameworkElement SkillRow(
        AiSelectionSkillDefinition skill,
        string description,
        bool enabled,
        Action<bool> onToggle,
        bool divider = true)
    {
        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        controls.Children.Add(FamoUI.ActionButton("打开", () => App.ShowAiSelectionSkill(skill)));
        controls.Children.Add(FamoUI.Pill(enabled, onToggle));
        return FamoUI.Row(skill.Title, description, controls, divider);
    }
}
