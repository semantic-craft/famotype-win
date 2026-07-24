using System.Runtime.InteropServices;

namespace Famo.Settings.Interop;

/// <summary>
/// 把一段文本（表情/符号/颜文字）上屏到**当前焦点**窗口：用 SendInput 合成
/// Unicode 键事件（KEYEVENTF_UNICODE），逐 UTF-16 码元发送（emoji 代理对 / 颜文字
/// 多字符串自然拆成多个事件）。emoji 浮窗以 WS_EX_NOACTIVATE 不抢焦点，故焦点仍在
/// 目标 app，注入直达其输入框。不污染剪贴板。
///
/// 边界：仅合成键盘输入，不碰 Rime / TSF 热路径。受保护窗口（提升权限 / 部分游戏）
/// 可能拦截 SendInput——Windows 固有限制，留剪贴板兜底为后续。
/// </summary>
public static class TextInjector
{
    /// <summary>把 <paramref name="text"/> 逐码元注入焦点窗口；空串无操作。返回是否成功发出。</summary>
    public static bool Inject(string text)
    {
        if (string.IsNullOrEmpty(text)) return false;

        // 每个 UTF-16 码元 → 一组 down+up 事件。
        var inputs = new INPUT[text.Length * 2];
        for (int i = 0; i < text.Length; i++)
        {
            ushort unit = text[i];
            inputs[i * 2] = UnicodeKey(unit, keyUp: false);
            inputs[i * 2 + 1] = UnicodeKey(unit, keyUp: true);
        }

        uint sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
        return sent == inputs.Length;
    }

    private static INPUT UnicodeKey(ushort scan, bool keyUp) => new()
    {
        type = INPUT_KEYBOARD,
        u = new INPUTUNION
        {
            ki = new KEYBDINPUT
            {
                wVk = 0,
                wScan = scan,
                dwFlags = KEYEVENTF_UNICODE | (keyUp ? KEYEVENTF_KEYUP : 0u),
                time = 0,
                dwExtraInfo = IntPtr.Zero,
            },
        },
    };

    // ── Win32 SendInput interop ──
    private const uint INPUT_KEYBOARD = 1;
    private const uint KEYEVENTF_KEYUP = 0x0002;
    private const uint KEYEVENTF_UNICODE = 0x0004;

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
