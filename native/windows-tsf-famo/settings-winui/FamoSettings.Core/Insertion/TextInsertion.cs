using Famo.Settings.Core.Clipboard;

namespace Famo.Settings.Core.Insertion;

public sealed record TextInsertionResult(bool Success, string Message)
{
    public static TextInsertionResult Ok(string message = "已插入文本。") => new(true, message);
    public static TextInsertionResult Fail(string message) => new(false, message);
}

public interface ITextInsertionService
{
    Task<TextInsertionResult> InsertAsync(string text, CancellationToken cancellationToken);
}

public sealed class SelectionVerifiedInsertionService : ITextInsertionService
{
    private readonly string _originalSelection;
    private readonly Func<CancellationToken, Task<string?>> _readCurrentSelection;
    private readonly ITextInsertionService _inner;

    public SelectionVerifiedInsertionService(
        string originalSelection,
        Func<CancellationToken, Task<string?>> readCurrentSelection,
        ITextInsertionService inner)
    {
        _originalSelection = originalSelection;
        _readCurrentSelection = readCurrentSelection;
        _inner = inner;
    }

    public async Task<TextInsertionResult> InsertAsync(string text, CancellationToken cancellationToken)
    {
        try
        {
            string? currentSelection = await _readCurrentSelection(cancellationToken);
            if (!string.Equals(currentSelection, _originalSelection, StringComparison.Ordinal))
            {
                return TextInsertionResult.Fail("原选区已变化，未执行替换；结果仍可手动复制。");
            }
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return TextInsertionResult.Fail("无法确认原选区，未执行替换：" + ex.Message);
        }

        return await _inner.InsertAsync(text, cancellationToken);
    }
}

public sealed record ClipboardTextSnapshot(bool HasText, string? Text);

public interface IClipboardTextBridge
{
    Task<ClipboardTextSnapshot> CaptureAsync(CancellationToken cancellationToken);
    Task SetTextAsync(string text, CancellationToken cancellationToken);
    Task RestoreAsync(ClipboardTextSnapshot snapshot, CancellationToken cancellationToken);
}

public interface IPasteCommandSender
{
    Task SendPasteAsync(CancellationToken cancellationToken);
}

public sealed class ClipboardPasteInsertionService : ITextInsertionService
{
    private readonly IClipboardTextBridge _clipboard;
    private readonly IPasteCommandSender _paste;
    private readonly TimeSpan _restoreDelay;

    public ClipboardPasteInsertionService(
        IClipboardTextBridge clipboard,
        IPasteCommandSender paste,
        TimeSpan? restoreDelay = null)
    {
        _clipboard = clipboard;
        _paste = paste;
        _restoreDelay = restoreDelay ?? TimeSpan.FromMilliseconds(80);
    }

    public async Task<TextInsertionResult> InsertAsync(string text, CancellationToken cancellationToken)
    {
        if (string.IsNullOrEmpty(text)) return TextInsertionResult.Fail("没有可插入的文本。");

        ClipboardTextSnapshot snapshot = new(false, null);
        bool captured = false;
        await ClipboardGate.Gate.WaitAsync(cancellationToken);
        try
        {
            snapshot = await _clipboard.CaptureAsync(cancellationToken);
            captured = true;
            await _clipboard.SetTextAsync(text, cancellationToken);
            await _paste.SendPasteAsync(cancellationToken);
            await DelayBeforeRestore(cancellationToken);
            return TextInsertionResult.Ok("已通过剪贴板粘贴提示词。");
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return TextInsertionResult.Fail("插入失败：" + ex.Message);
        }
        finally
        {
            if (captured)
            {
                try
                {
                    await _clipboard.RestoreAsync(snapshot, CancellationToken.None);
                }
                catch
                {
                    // Restore is best effort; insertion result already reports the primary path.
                }
            }
            ClipboardGate.Gate.Release();
        }
    }

    private Task DelayBeforeRestore(CancellationToken cancellationToken) =>
        _restoreDelay <= TimeSpan.Zero
            ? Task.CompletedTask
            : Task.Delay(_restoreDelay, cancellationToken);
}
