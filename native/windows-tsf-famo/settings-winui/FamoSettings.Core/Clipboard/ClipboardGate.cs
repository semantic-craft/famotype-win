namespace Famo.Settings.Core.Clipboard;

internal static class ClipboardGate
{
    internal static readonly SemaphoreSlim Gate = new(1, 1);
}
