using Famo.Settings.Core.Insertion;
using System.Runtime.InteropServices;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class TextInsertionTests
{
    [Fact]
    public async Task InsertAsync_SetsClipboardPastesAndRestoresSnapshot()
    {
        var clipboard = new FakeClipboard(new ClipboardTextSnapshot(true, "old"));
        var paste = new FakePasteCommandSender();
        var service = new ClipboardPasteInsertionService(clipboard, paste, TimeSpan.Zero);

        TextInsertionResult result = await service.InsertAsync("rendered prompt", CancellationToken.None);

        Assert.True(result.Success);
        Assert.Equal(["rendered prompt"], clipboard.SetTexts);
        Assert.Equal(1, paste.SendCount);
        Assert.Equal(new ClipboardTextSnapshot(true, "old"), clipboard.Restored);
        Assert.True(clipboard.Events.IndexOf("set", StringComparison.Ordinal)
            < clipboard.Events.IndexOf("restore", StringComparison.Ordinal));
    }

    [Fact]
    public async Task InsertAsync_RestoresClipboardWhenPasteFails()
    {
        var clipboard = new FakeClipboard(new ClipboardTextSnapshot(true, "old"));
        var paste = new FakePasteCommandSender { ThrowOnSend = true };
        var service = new ClipboardPasteInsertionService(clipboard, paste, TimeSpan.Zero);

        TextInsertionResult result = await service.InsertAsync("rendered prompt", CancellationToken.None);

        Assert.False(result.Success);
        Assert.Contains("插入失败", result.Message);
        Assert.Equal(new ClipboardTextSnapshot(true, "old"), clipboard.Restored);
    }

    [Fact]
    public async Task InsertAsync_RejectsEmptyTextWithoutTouchingClipboard()
    {
        var clipboard = new FakeClipboard(new ClipboardTextSnapshot(true, "old"));
        var paste = new FakePasteCommandSender();
        var service = new ClipboardPasteInsertionService(clipboard, paste, TimeSpan.Zero);

        TextInsertionResult result = await service.InsertAsync("", CancellationToken.None);

        Assert.False(result.Success);
        Assert.Empty(clipboard.SetTexts);
        Assert.Equal(0, paste.SendCount);
        Assert.Null(clipboard.Restored);
    }

    [Fact]
    public async Task VerifiedInsertAsync_InsertsOnlyWhenOriginalSelectionIsStillSelected()
    {
        var inner = new FakeTextInsertionService();
        var service = new SelectionVerifiedInsertionService(
            "original", _ => Task.FromResult<string?>("original"), inner);

        TextInsertionResult result = await service.InsertAsync("replacement", CancellationToken.None);

        Assert.True(result.Success);
        Assert.Equal("replacement", inner.InsertedText);
    }

    [Fact]
    public async Task VerifiedInsertAsync_RejectsChangedSelectionWithoutInserting()
    {
        var inner = new FakeTextInsertionService();
        var service = new SelectionVerifiedInsertionService(
            "original", _ => Task.FromResult<string?>("changed"), inner);

        TextInsertionResult result = await service.InsertAsync("replacement", CancellationToken.None);

        Assert.False(result.Success);
        Assert.Contains("原选区已变化", result.Message);
        Assert.Null(inner.InsertedText);
    }

    [Fact]
    public void ForegroundWindowTarget_RejectsAClosedRealWindow()
    {
        if (!OperatingSystem.IsWindows()) return;
        nint window = CreateWindowExW(0, "STATIC", "Famo focus target test", 0,
            0, 0, 100, 100, 0, 0, 0, 0);
        Assert.NotEqual(0, window);
        WindowsForegroundWindowTarget? target = WindowsForegroundWindowTarget.Capture(window);
        Assert.NotNull(target);
        Assert.True(target.IsStillValid());

        Assert.True(DestroyWindow(window));
        Assert.False(target.IsStillValid());
        Assert.False(target.TryRestore());
    }

    private sealed class FakeClipboard : IClipboardTextBridge
    {
        private readonly ClipboardTextSnapshot _snapshot;

        public FakeClipboard(ClipboardTextSnapshot snapshot)
        {
            _snapshot = snapshot;
        }

        public List<string> SetTexts { get; } = new();
        public ClipboardTextSnapshot? Restored { get; private set; }
        public string Events { get; private set; } = "";

        public Task<ClipboardTextSnapshot> CaptureAsync(CancellationToken cancellationToken)
        {
            Events += "capture;";
            return Task.FromResult(_snapshot);
        }

        public Task SetTextAsync(string text, CancellationToken cancellationToken)
        {
            Events += "set;";
            SetTexts.Add(text);
            return Task.CompletedTask;
        }

        public Task RestoreAsync(ClipboardTextSnapshot snapshot, CancellationToken cancellationToken)
        {
            Events += "restore;";
            Restored = snapshot;
            return Task.CompletedTask;
        }
    }

    private sealed class FakePasteCommandSender : IPasteCommandSender
    {
        public bool ThrowOnSend { get; init; }
        public int SendCount { get; private set; }

        public Task SendPasteAsync(CancellationToken cancellationToken)
        {
            SendCount++;
            if (ThrowOnSend) throw new InvalidOperationException("paste failed");
            return Task.CompletedTask;
        }
    }

    private sealed class FakeTextInsertionService : ITextInsertionService
    {
        public string? InsertedText { get; private set; }

        public Task<TextInsertionResult> InsertAsync(string text, CancellationToken cancellationToken)
        {
            InsertedText = text;
            return Task.FromResult(TextInsertionResult.Ok());
        }
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern nint CreateWindowExW(
        uint extendedStyle, string className, string windowName, uint style,
        int x, int y, int width, int height, nint parent, nint menu, nint instance, nint parameter);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(nint hWnd);
}
