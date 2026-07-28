using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace Famo.Settings.Core.Insertion;

public sealed class WindowsForegroundWindowTarget
{
    private readonly uint _processId;
    private readonly uint _threadId;
    private readonly long _processStartTicks;
    private readonly string _className;

    private WindowsForegroundWindowTarget(
        nint handle, uint processId, uint threadId, long processStartTicks, string className)
    {
        Handle = handle;
        _processId = processId;
        _threadId = threadId;
        _processStartTicks = processStartTicks;
        _className = className;
    }

    public nint Handle { get; }

    public static WindowsForegroundWindowTarget? CaptureForeground() => Capture(GetForegroundWindow());

    public static WindowsForegroundWindowTarget? Capture(nint handle)
    {
        if (!TryReadIdentity(handle, out uint processId, out uint threadId, out long started, out string className))
            return null;
        return new WindowsForegroundWindowTarget(handle, processId, threadId, started, className);
    }

    public bool IsStillValid() =>
        TryReadIdentity(Handle, out uint processId, out uint threadId, out long started, out string className)
        && processId == _processId
        && threadId == _threadId
        && started == _processStartTicks
        && string.Equals(className, _className, StringComparison.Ordinal);

    public bool TryRestore() => IsStillValid() && SetForegroundWindow(Handle);

    private static bool TryReadIdentity(
        nint handle, out uint processId, out uint threadId, out long processStartTicks, out string className)
    {
        processId = 0;
        threadId = 0;
        processStartTicks = 0;
        className = "";
        if (handle == 0 || !IsWindow(handle)) return false;

        threadId = GetWindowThreadProcessId(handle, out processId);
        if (threadId == 0 || processId == 0) return false;
        var name = new StringBuilder(256);
        if (GetClassNameW(handle, name, name.Capacity) == 0) return false;
        try
        {
            using Process process = Process.GetProcessById((int)processId);
            processStartTicks = process.StartTime.ToUniversalTime().Ticks;
            className = name.ToString();
            return true;
        }
        catch
        {
            return false;
        }
    }

    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool IsWindow(nint hWnd);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint hWnd);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint hWnd, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassNameW(nint hWnd, StringBuilder className, int capacity);
}
