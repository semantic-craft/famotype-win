using Famo.Settings.Core.Clipboard;

namespace Famo.Settings.Core.Selection;

public enum SelectedTextCaptureStatus
{
    Success,
    NoSelection,
    SecureField,
    Unavailable,
    Failed,
}

public enum SelectedTextCaptureSource
{
    None,
    FocusedControl,
    Clipboard,
}

public sealed record SelectedTextCaptureResult(
    SelectedTextCaptureStatus Status,
    SelectedTextCaptureSource Source,
    string? Text = null,
    string Error = "")
{
    public static SelectedTextCaptureResult Success(string text, SelectedTextCaptureSource source) =>
        new(SelectedTextCaptureStatus.Success, source, text);

    public static SelectedTextCaptureResult NoSelection(SelectedTextCaptureSource source = SelectedTextCaptureSource.None) =>
        new(SelectedTextCaptureStatus.NoSelection, source);

    public static SelectedTextCaptureResult SecureField(string error = "当前焦点位于密码或安全输入框，已取消读取选中文本。") =>
        new(SelectedTextCaptureStatus.SecureField, SelectedTextCaptureSource.FocusedControl, Error: error);

    public static SelectedTextCaptureResult Unavailable(string error = "当前控件不支持读取选中文本。") =>
        new(SelectedTextCaptureStatus.Unavailable, SelectedTextCaptureSource.FocusedControl, Error: error);

    public static SelectedTextCaptureResult Failed(string error) =>
        new(SelectedTextCaptureStatus.Failed, SelectedTextCaptureSource.None, Error: error);
}

public sealed record FocusedTextSelectionResult(
    SelectedTextCaptureStatus Status,
    string? Text = null,
    string Error = "")
{
    public static FocusedTextSelectionResult SelectedText(string text) =>
        new(SelectedTextCaptureStatus.Success, text);

    public static FocusedTextSelectionResult NoSelection() =>
        new(SelectedTextCaptureStatus.NoSelection);

    public static FocusedTextSelectionResult SecureField(string error = "当前焦点位于密码或安全输入框，已取消读取选中文本。") =>
        new(SelectedTextCaptureStatus.SecureField, Error: error);

    public static FocusedTextSelectionResult Unavailable(string error = "当前控件不支持读取选中文本。") =>
        new(SelectedTextCaptureStatus.Unavailable, Error: error);

    public static FocusedTextSelectionResult Failed(string error) =>
        new(SelectedTextCaptureStatus.Failed, Error: error);
}

public interface IFocusedTextSelectionReader
{
    Task<FocusedTextSelectionResult> ReadAsync(CancellationToken cancellationToken);
}

public interface IClipboardCopySelectionReader
{
    Task<SelectedTextCaptureResult> ReadAsync(CancellationToken cancellationToken);
}

public sealed class SelectedTextCaptureService
{
    private readonly IFocusedTextSelectionReader _focusedReader;
    private readonly IClipboardCopySelectionReader _clipboardReader;

    public SelectedTextCaptureService(
        IFocusedTextSelectionReader focusedReader,
        IClipboardCopySelectionReader clipboardReader)
    {
        _focusedReader = focusedReader;
        _clipboardReader = clipboardReader;
    }

    public async Task<SelectedTextCaptureResult> CaptureAsync(CancellationToken cancellationToken)
    {
        FocusedTextSelectionResult focused = await ReadFocusedAsync(cancellationToken);
        if (focused.Status == SelectedTextCaptureStatus.SecureField)
        {
            return SelectedTextCaptureResult.SecureField(focused.Error);
        }
        if (focused.Status == SelectedTextCaptureStatus.Success
            && !string.IsNullOrWhiteSpace(focused.Text))
        {
            return SelectedTextCaptureResult.Success(focused.Text!, SelectedTextCaptureSource.FocusedControl);
        }

        SelectedTextCaptureResult clipboard = await ReadClipboardAsync(cancellationToken);
        if (clipboard.Status == SelectedTextCaptureStatus.Success
            && string.IsNullOrWhiteSpace(clipboard.Text))
        {
            return SelectedTextCaptureResult.NoSelection(SelectedTextCaptureSource.Clipboard);
        }

        return clipboard;
    }

    private async Task<FocusedTextSelectionResult> ReadFocusedAsync(CancellationToken cancellationToken)
    {
        try
        {
            return await _focusedReader.ReadAsync(cancellationToken);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return FocusedTextSelectionResult.Unavailable(ex.Message);
        }
    }

    private async Task<SelectedTextCaptureResult> ReadClipboardAsync(CancellationToken cancellationToken)
    {
        try
        {
            return await _clipboardReader.ReadAsync(cancellationToken);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return SelectedTextCaptureResult.Failed(ex.Message);
        }
    }
}

public sealed record ClipboardTextSnapshot(bool HasText, string? Text)
{
    public static ClipboardTextSnapshot Empty { get; } = new(false, null);

    public static ClipboardTextSnapshot FromText(string text) => new(true, text);
}

public interface IClipboardTextChannel
{
    Task<ClipboardTextSnapshot> CaptureAsync(CancellationToken cancellationToken);

    Task SetTextAsync(string text, CancellationToken cancellationToken);

    Task<string?> GetTextAsync(CancellationToken cancellationToken);

    Task RestoreAsync(ClipboardTextSnapshot snapshot, CancellationToken cancellationToken);
}

public interface ICopyShortcutSender
{
    bool SendCopyShortcut();
}

public interface ISelectionCaptureDelay
{
    Task DelayAsync(TimeSpan delay, CancellationToken cancellationToken);
}

public sealed class SystemSelectionCaptureDelay : ISelectionCaptureDelay
{
    public Task DelayAsync(TimeSpan delay, CancellationToken cancellationToken) =>
        Task.Delay(delay, cancellationToken);
}

public sealed class ClipboardCopySelectionReader : IClipboardCopySelectionReader
{
    public const string SentinelPrefix = "famo-selection-sentinel:";

    private static readonly TimeSpan DefaultPollDelay = TimeSpan.FromMilliseconds(90);

    private readonly IClipboardTextChannel _clipboard;
    private readonly ICopyShortcutSender _shortcutSender;
    private readonly ISelectionCaptureDelay _delay;
    private readonly int _pollAttempts;

    public ClipboardCopySelectionReader(
        IClipboardTextChannel clipboard,
        ICopyShortcutSender shortcutSender,
        ISelectionCaptureDelay? delay = null,
        int pollAttempts = 8)
    {
        _clipboard = clipboard;
        _shortcutSender = shortcutSender;
        _delay = delay ?? new SystemSelectionCaptureDelay();
        _pollAttempts = Math.Max(1, pollAttempts);
    }

    public async Task<SelectedTextCaptureResult> ReadAsync(CancellationToken cancellationToken)
    {
        ClipboardTextSnapshot snapshot = ClipboardTextSnapshot.Empty;
        bool captured = false;

        await ClipboardGate.Gate.WaitAsync(cancellationToken);
        try
        {
            snapshot = await _clipboard.CaptureAsync(cancellationToken);
            captured = true;

            string sentinel = SentinelPrefix + Guid.NewGuid().ToString("N");
            await _clipboard.SetTextAsync(sentinel, cancellationToken);
            if (!_shortcutSender.SendCopyShortcut())
            {
                return SelectedTextCaptureResult.Failed("无法发送复制命令，目标窗口可能拥有更高权限（如以管理员身份运行）。");
            }

            for (int attempt = 0; attempt < _pollAttempts; attempt++)
            {
                await _delay.DelayAsync(DefaultPollDelay, cancellationToken);
                string? text = await _clipboard.GetTextAsync(cancellationToken);
                if (string.IsNullOrEmpty(text)
                    || string.Equals(text, sentinel, StringComparison.Ordinal))
                {
                    continue;
                }
                if (string.IsNullOrWhiteSpace(text))
                {
                    return SelectedTextCaptureResult.NoSelection(SelectedTextCaptureSource.Clipboard);
                }

                return SelectedTextCaptureResult.Success(text, SelectedTextCaptureSource.Clipboard);
            }

            return SelectedTextCaptureResult.NoSelection(SelectedTextCaptureSource.Clipboard);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return SelectedTextCaptureResult.Failed(ex.Message);
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
                    // Best effort: selection capture should not crash if another app owns the clipboard.
                }
            }
            ClipboardGate.Gate.Release();
        }
    }
}
