using System.Runtime.InteropServices;
using Famo.Settings.Core.Insertion;
using Windows.ApplicationModel.DataTransfer;

namespace Famo.Settings.Interop;

public sealed class WindowsClipboardTextBridge : IClipboardTextBridge
{
    public async Task<ClipboardTextSnapshot> CaptureAsync(CancellationToken cancellationToken)
    {
        DataPackageView content = Clipboard.GetContent();
        IReadOnlyList<string> formats = content.AvailableFormats;
        if (formats.Count > 0 && !(formats.Count == 1 && formats[0] == StandardDataFormats.Text))
        {
            throw new InvalidOperationException("剪贴板当前有非文本内容，已取消粘贴插入以免覆盖它。");
        }

        if (!content.Contains(StandardDataFormats.Text))
        {
            return new ClipboardTextSnapshot(false, null);
        }

        string text = await content.GetTextAsync();
        return new ClipboardTextSnapshot(true, text);
    }

    public Task SetTextAsync(string text, CancellationToken cancellationToken)
    {
        var package = new DataPackage();
        package.SetText(text);
        Clipboard.SetContent(package);
        Clipboard.Flush();
        return Task.CompletedTask;
    }

    public Task RestoreAsync(ClipboardTextSnapshot snapshot, CancellationToken cancellationToken)
    {
        if (snapshot.HasText)
        {
            var package = new DataPackage();
            package.SetText(snapshot.Text ?? string.Empty);
            Clipboard.SetContent(package);
            Clipboard.Flush();
        }
        else
        {
            Clipboard.Clear();
        }
        return Task.CompletedTask;
    }
}

public sealed class SendInputPasteCommandSender : IPasteCommandSender
{
    private readonly nint _targetWindow;

    public SendInputPasteCommandSender(nint targetWindow)
    {
        _targetWindow = targetWindow;
    }

    public Task SendPasteAsync(CancellationToken cancellationToken)
    {
        if (_targetWindow != 0)
        {
            if (!IsWindow(_targetWindow))
            {
                throw new InvalidOperationException("目标窗口已关闭，无法粘贴");
            }
            if (!SetForegroundWindow(_targetWindow))
            {
                throw new InvalidOperationException("无法切换到目标窗口，无法粘贴");
            }
        }

        var inputs = new[]
        {
            Key(VK_CONTROL, keyUp: false),
            Key((ushort)'V', keyUp: false),
            Key((ushort)'V', keyUp: true),
            Key(VK_CONTROL, keyUp: true),
        };
        uint sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
        if (sent != inputs.Length)
        {
            throw new InvalidOperationException("无法发送 Ctrl+V 粘贴命令");
        }
        return Task.CompletedTask;
    }

    public static nint CaptureForegroundWindow() => GetForegroundWindow();

    private static INPUT Key(ushort virtualKey, bool keyUp) => new()
    {
        type = INPUT_KEYBOARD,
        u = new INPUTUNION
        {
            ki = new KEYBDINPUT
            {
                wVk = virtualKey,
                wScan = 0,
                dwFlags = keyUp ? KEYEVENTF_KEYUP : 0u,
                time = 0,
                dwExtraInfo = IntPtr.Zero,
            },
        },
    };

    private const uint INPUT_KEYBOARD = 1;
    private const uint KEYEVENTF_KEYUP = 0x0002;
    private const ushort VK_CONTROL = 0x11;

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

    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint hWnd);
    [DllImport("user32.dll")] private static extern bool IsWindow(nint hWnd);
    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
}

public static class TextInsertionServices
{
    public static ITextInsertionService ClipboardPasteForForegroundTarget() =>
        new ClipboardPasteInsertionService(
            new WindowsClipboardTextBridge(),
            new SendInputPasteCommandSender(SendInputPasteCommandSender.CaptureForegroundWindow()));
}
