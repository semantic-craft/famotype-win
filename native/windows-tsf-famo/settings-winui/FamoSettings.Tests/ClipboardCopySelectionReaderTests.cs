using Famo.Settings.Core.Selection;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class ClipboardCopySelectionReaderTests
{
    [Fact]
    public async Task ReadAsync_UsesSentinelCopiesSelectionAndRestoresOriginalClipboard()
    {
        var clipboard = new FakeClipboardTextChannel("旧剪贴板");
        var shortcut = new FakeCopyShortcutSender(() => clipboard.CurrentText = "选中的文字");
        var reader = new ClipboardCopySelectionReader(clipboard, shortcut, new ImmediateSelectionCaptureDelay());

        SelectedTextCaptureResult result = await reader.ReadAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.Success, result.Status);
        Assert.Equal(SelectedTextCaptureSource.Clipboard, result.Source);
        Assert.Equal("选中的文字", result.Text);
        Assert.Equal(1, shortcut.Calls);
        Assert.Contains(clipboard.SetTexts, text => text.StartsWith(ClipboardCopySelectionReader.SentinelPrefix, StringComparison.Ordinal));
        Assert.True(clipboard.RestoreCalled);
        Assert.Equal("旧剪贴板", clipboard.CurrentText);
    }

    [Fact]
    public async Task ReadAsync_WhenCopyLeavesSentinel_ReturnsNoSelectionAndRestoresClipboard()
    {
        var clipboard = new FakeClipboardTextChannel("旧剪贴板");
        var shortcut = new FakeCopyShortcutSender(() => { });
        var reader = new ClipboardCopySelectionReader(clipboard, shortcut, new ImmediateSelectionCaptureDelay());

        SelectedTextCaptureResult result = await reader.ReadAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.NoSelection, result.Status);
        Assert.Null(result.Text);
        Assert.True(clipboard.RestoreCalled);
        Assert.Equal("旧剪贴板", clipboard.CurrentText);
    }

    [Fact]
    public async Task ReadAsync_WhenCopyProducesWhitespace_ReturnsNoSelectionAndRestoresClipboard()
    {
        var clipboard = new FakeClipboardTextChannel("旧剪贴板");
        var shortcut = new FakeCopyShortcutSender(() => clipboard.CurrentText = " \r\n\t ");
        var reader = new ClipboardCopySelectionReader(clipboard, shortcut, new ImmediateSelectionCaptureDelay());

        SelectedTextCaptureResult result = await reader.ReadAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.NoSelection, result.Status);
        Assert.True(clipboard.RestoreCalled);
    }

    [Fact]
    public async Task ReadAsync_RestoresClipboardWhenCopyShortcutFails()
    {
        var clipboard = new FakeClipboardTextChannel("旧剪贴板");
        var shortcut = new FakeCopyShortcutSender(() => throw new InvalidOperationException("copy failed"));
        var reader = new ClipboardCopySelectionReader(clipboard, shortcut, new ImmediateSelectionCaptureDelay());

        SelectedTextCaptureResult result = await reader.ReadAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.Failed, result.Status);
        Assert.Contains("copy failed", result.Error);
        Assert.True(clipboard.RestoreCalled);
        Assert.Equal("旧剪贴板", clipboard.CurrentText);
    }

    [Fact]
    public async Task ReadAsync_WhenClipboardCaptureThrows_ReturnsFailedWithoutRestoring()
    {
        var clipboard = new FakeClipboardTextChannel("旧剪贴板") { ThrowOnCapture = true };
        var shortcut = new FakeCopyShortcutSender(() => { });
        var reader = new ClipboardCopySelectionReader(clipboard, shortcut, new ImmediateSelectionCaptureDelay());

        SelectedTextCaptureResult result = await reader.ReadAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.Failed, result.Status);
        Assert.Contains("clipboard unavailable", result.Error);
        Assert.Equal(0, shortcut.Calls);
        Assert.False(clipboard.RestoreCalled);
    }

    [Fact]
    public async Task ReadAsync_WhenCopyShortcutCannotBeSent_ReturnsFailedAndRestoresClipboard()
    {
        var clipboard = new FakeClipboardTextChannel("旧剪贴板");
        var shortcut = new FakeCopyShortcutSender(() => { }) { SendSucceeds = false };
        var reader = new ClipboardCopySelectionReader(clipboard, shortcut, new ImmediateSelectionCaptureDelay());

        SelectedTextCaptureResult result = await reader.ReadAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.Failed, result.Status);
        Assert.Contains("无法发送复制命令", result.Error);
        Assert.True(clipboard.RestoreCalled);
        Assert.Equal("旧剪贴板", clipboard.CurrentText);
    }

    [Fact]
    public async Task ReadAsync_WhenTargetWindowClosesDuringPoll_ReturnsFailedAndRestoresClipboard()
    {
        var clipboard = new FakeClipboardTextChannel("旧剪贴板") { ThrowOnGetText = true };
        var shortcut = new FakeCopyShortcutSender(() => { });
        var reader = new ClipboardCopySelectionReader(clipboard, shortcut, new ImmediateSelectionCaptureDelay());

        SelectedTextCaptureResult result = await reader.ReadAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.Failed, result.Status);
        Assert.Contains("window closed", result.Error);
        Assert.True(clipboard.RestoreCalled);
        Assert.Equal("旧剪贴板", clipboard.CurrentText);
    }

    [Fact]
    public async Task ReadAsync_WhenClipboardStaysEmptyThroughoutPolling_ReturnsNoSelectionAndRestoresClipboard()
    {
        var clipboard = new FakeClipboardTextChannel("旧剪贴板") { ClearTextOnSet = true };
        var shortcut = new FakeCopyShortcutSender(() => { });
        var reader = new ClipboardCopySelectionReader(clipboard, shortcut, new ImmediateSelectionCaptureDelay());

        SelectedTextCaptureResult result = await reader.ReadAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.NoSelection, result.Status);
        Assert.Null(result.Text);
        Assert.True(clipboard.RestoreCalled);
        Assert.Equal("旧剪贴板", clipboard.CurrentText);
    }

    private sealed class FakeClipboardTextChannel : IClipboardTextChannel
    {
        private ClipboardTextSnapshot _snapshot = ClipboardTextSnapshot.Empty;

        public string? CurrentText { get; set; }
        public List<string> SetTexts { get; } = new();
        public bool RestoreCalled { get; private set; }
        public bool ThrowOnCapture { get; init; }
        public bool ThrowOnGetText { get; init; }
        public bool ClearTextOnSet { get; init; }

        public FakeClipboardTextChannel(string? initialText)
        {
            CurrentText = initialText;
        }

        public Task<ClipboardTextSnapshot> CaptureAsync(CancellationToken cancellationToken)
        {
            if (ThrowOnCapture) throw new InvalidOperationException("clipboard unavailable");
            _snapshot = CurrentText is null
                ? ClipboardTextSnapshot.Empty
                : ClipboardTextSnapshot.FromText(CurrentText);
            return Task.FromResult(_snapshot);
        }

        public Task SetTextAsync(string text, CancellationToken cancellationToken)
        {
            CurrentText = ClearTextOnSet ? null : text;
            SetTexts.Add(text);
            return Task.CompletedTask;
        }

        public Task<string?> GetTextAsync(CancellationToken cancellationToken)
        {
            if (ThrowOnGetText) throw new InvalidOperationException("window closed");
            return Task.FromResult(CurrentText);
        }

        public Task RestoreAsync(ClipboardTextSnapshot snapshot, CancellationToken cancellationToken)
        {
            RestoreCalled = true;
            CurrentText = snapshot.HasText ? snapshot.Text : null;
            return Task.CompletedTask;
        }
    }

    private sealed class FakeCopyShortcutSender : ICopyShortcutSender
    {
        private readonly Action _send;

        public int Calls { get; private set; }
        public bool SendSucceeds { get; init; } = true;

        public FakeCopyShortcutSender(Action send)
        {
            _send = send;
        }

        public bool SendCopyShortcut()
        {
            Calls++;
            _send();
            return SendSucceeds;
        }
    }

    private sealed class ImmediateSelectionCaptureDelay : ISelectionCaptureDelay
    {
        public Task DelayAsync(TimeSpan delay, CancellationToken cancellationToken) => Task.CompletedTask;
    }
}
