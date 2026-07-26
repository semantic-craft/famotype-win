using System.Runtime.InteropServices;
using System.Text;
using Famo.Settings.Core.Selection;
using Windows.ApplicationModel.DataTransfer;

namespace Famo.Settings.Interop;

public sealed class WindowsUiAutomationSelectionAnchor
{
    private static readonly Guid CUIAutomationClsid = new("ff48dba4-60ef-4201-aa87-54103eef594e");
    private const int UIA_TextPatternId = 10014;
    private readonly object _automation;
    private readonly object _originalRange;
    private readonly int[] _runtimeId;

    private WindowsUiAutomationSelectionAnchor(
        object automation,
        object originalRange,
        int[] runtimeId,
        string text)
    {
        _automation = automation;
        _originalRange = originalRange;
        _runtimeId = runtimeId;
        Text = text;
    }

    public string Text { get; }

    public static Task<WindowsUiAutomationSelectionAnchor?> CaptureAsync(
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            Type? automationType = Type.GetTypeFromCLSID(CUIAutomationClsid);
            dynamic automation = automationType is null ? null! : Activator.CreateInstance(automationType)!;
            if (automation is null) return Task.FromResult<WindowsUiAutomationSelectionAnchor?>(null);
            dynamic element = automation.GetFocusedElement();
            if (element is null || (bool)element.CurrentIsPassword)
                return Task.FromResult<WindowsUiAutomationSelectionAnchor?>(null);
            dynamic ranges = element.GetCurrentPattern(UIA_TextPatternId).GetSelection();
            if ((int)ranges.Length != 1)
                return Task.FromResult<WindowsUiAutomationSelectionAnchor?>(null);
            dynamic range = ranges.GetElement(0);
            string text = (range.GetText(-1) as string) ?? string.Empty;
            Array runtimeId = (Array)element.GetRuntimeId();
            if (string.IsNullOrWhiteSpace(text) || runtimeId.Length == 0)
                return Task.FromResult<WindowsUiAutomationSelectionAnchor?>(null);
            int[] identity = runtimeId.Cast<object>().Select(Convert.ToInt32).ToArray();
            return Task.FromResult<WindowsUiAutomationSelectionAnchor?>(
                new(automation, range.Clone(), identity, text));
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return Task.FromResult<WindowsUiAutomationSelectionAnchor?>(null);
        }
    }

    public Task<string?> VerifyAndReselectAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            dynamic automation = _automation;
            dynamic element = automation.GetFocusedElement();
            if (element is null || (bool)element.CurrentIsPassword)
                return Task.FromResult<string?>(null);
            Array runtimeId = (Array)element.GetRuntimeId();
            int[] identity = runtimeId.Cast<object>().Select(Convert.ToInt32).ToArray();
            if (!_runtimeId.SequenceEqual(identity)) return Task.FromResult<string?>(null);

            dynamic ranges = element.GetCurrentPattern(UIA_TextPatternId).GetSelection();
            if ((int)ranges.Length != 1) return Task.FromResult<string?>(null);
            dynamic current = ranges.GetElement(0);
            dynamic original = _originalRange;
            if ((int)current.CompareEndpoints(0, original, 0) != 0 ||
                (int)current.CompareEndpoints(1, original, 1) != 0)
                return Task.FromResult<string?>(null);
            string text = (current.GetText(-1) as string) ?? string.Empty;
            if (!string.Equals(text, Text, StringComparison.Ordinal))
                return Task.FromResult<string?>(null);
            original.Select();
            return Task.FromResult<string?>(text);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return Task.FromResult<string?>(null);
        }
    }
}

public sealed class WindowsFocusedTextSelectionReader : IFocusedTextSelectionReader
{
    private static readonly Guid CUIAutomationClsid = new("ff48dba4-60ef-4201-aa87-54103eef594e");

    private const int UIA_TextPatternId = 10014;

    public Task<FocusedTextSelectionResult> ReadAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();

        try
        {
            Type? automationType = Type.GetTypeFromCLSID(CUIAutomationClsid);
            if (automationType is null)
            {
                return Task.FromResult(FocusedTextSelectionResult.Unavailable("Windows UI Automation 不可用。"));
            }

            dynamic automation = Activator.CreateInstance(automationType)
                ?? throw new InvalidOperationException("Windows UI Automation 初始化失败。");
            dynamic element = automation.GetFocusedElement();
            if (element is null)
            {
                return Task.FromResult(FocusedTextSelectionResult.Unavailable("当前没有可读取的焦点控件。"));
            }

            bool? isPassword = IsPasswordElement(element);
            if (isPassword != false)
            {
                return Task.FromResult(FocusedTextSelectionResult.SecureField(isPassword is null
                    ? "无法确认当前控件是否为安全输入框，已保守跳过读取。"
                    : "当前焦点位于密码或安全输入框，已取消读取选中文本。"));
            }

            dynamic pattern = element.GetCurrentPattern(UIA_TextPatternId);
            if (pattern is null)
            {
                return Task.FromResult(FocusedTextSelectionResult.NoSelection());
            }

            dynamic ranges = pattern.GetSelection();
            int length = (int)ranges.Length;
            if (length <= 0)
            {
                return Task.FromResult(FocusedTextSelectionResult.NoSelection());
            }

            var selected = new StringBuilder();
            for (int i = 0; i < length; i++)
            {
                dynamic range = ranges.GetElement(i);
                string? text = range.GetText(-1) as string;
                if (!string.IsNullOrEmpty(text))
                {
                    selected.Append(text);
                }
            }

            string value = selected.ToString();
            return Task.FromResult(string.IsNullOrWhiteSpace(value)
                ? FocusedTextSelectionResult.NoSelection()
                : FocusedTextSelectionResult.SelectedText(value));
        }
        catch (COMException ex)
        {
            return Task.FromResult(FocusedTextSelectionResult.Unavailable(ex.Message));
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return Task.FromResult(FocusedTextSelectionResult.Unavailable(ex.Message));
        }
    }

    private static bool? IsPasswordElement(dynamic element)
    {
        try
        {
            return (bool)element.CurrentIsPassword;
        }
        catch
        {
            return null;
        }
    }
}

public sealed class WindowsClipboardTextChannel : IClipboardTextChannel
{
    public async Task<ClipboardTextSnapshot> CaptureAsync(CancellationToken cancellationToken)
    {
        DataPackageView content = Clipboard.GetContent();
        IReadOnlyList<string> formats = content.AvailableFormats;
        if (formats.Count > 0 && !(formats.Count == 1 && formats[0] == StandardDataFormats.Text))
        {
            throw new InvalidOperationException("剪贴板中有无法安全保留的内容，已跳过读取。");
        }

        string? text = await GetTextAsync(cancellationToken);
        return text is null ? ClipboardTextSnapshot.Empty : ClipboardTextSnapshot.FromText(text);
    }

    public Task SetTextAsync(string text, CancellationToken cancellationToken)
    {
        var data = new DataPackage();
        data.SetText(text);
        Clipboard.SetContent(data);
        return Task.CompletedTask;
    }

    public async Task<string?> GetTextAsync(CancellationToken cancellationToken)
    {
        try
        {
            DataPackageView content = Clipboard.GetContent();
            if (!content.Contains(StandardDataFormats.Text)) return null;
            return await content.GetTextAsync().AsTask(cancellationToken);
        }
        catch
        {
            return null;
        }
    }

    public Task RestoreAsync(ClipboardTextSnapshot snapshot, CancellationToken cancellationToken)
    {
        if (snapshot.HasText && snapshot.Text is not null)
        {
            var data = new DataPackage();
            data.SetText(snapshot.Text);
            Clipboard.SetContent(data);
        }
        else
        {
            Clipboard.Clear();
        }

        return Task.CompletedTask;
    }
}

public sealed class Win32CopyShortcutSender : ICopyShortcutSender
{
    public bool SendCopyShortcut()
    {
        var inputs = new[]
        {
            Key(VK_CONTROL, keyUp: false),
            Key((ushort)'C', keyUp: false),
            Key((ushort)'C', keyUp: true),
            Key(VK_CONTROL, keyUp: true),
        };

        uint sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
        return sent == inputs.Length;
    }

    private static INPUT Key(ushort virtualKey, bool keyUp) => new()
    {
        type = INPUT_KEYBOARD,
        u = new INPUTUNION
        {
            ki = new KEYBDINPUT
            {
                wVk = virtualKey,
                wScan = 0,
                dwFlags = keyUp ? KEYEVENTF_KEYUP : 0,
                time = 0,
                dwExtraInfo = IntPtr.Zero,
            },
        },
    };

    private const ushort VK_CONTROL = 0x11;
    private const uint INPUT_KEYBOARD = 1;
    private const uint KEYEVENTF_KEYUP = 0x0002;

    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT
    {
        public uint type;
        public INPUTUNION u;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct INPUTUNION
    {
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public HARDWAREINPUT hi;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT
    {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MOUSEINPUT
    {
        public int dx;
        public int dy;
        public uint mouseData;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct HARDWAREINPUT
    {
        public uint uMsg;
        public ushort wParamL;
        public ushort wParamH;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
}
