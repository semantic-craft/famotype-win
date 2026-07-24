using Famo.Settings.Core.Selection;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class SelectedTextCaptureServiceTests
{
    [Fact]
    public async Task CaptureAsync_UsesFocusedTextBeforeClipboardFallback()
    {
        var focused = new FakeFocusedReader(FocusedTextSelectionResult.SelectedText("已选中文本"));
        var clipboard = new FakeClipboardReader(SelectedTextCaptureResult.Success("剪贴板文本", SelectedTextCaptureSource.Clipboard));
        var service = new SelectedTextCaptureService(focused, clipboard);

        SelectedTextCaptureResult result = await service.CaptureAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.Success, result.Status);
        Assert.Equal(SelectedTextCaptureSource.FocusedControl, result.Source);
        Assert.Equal("已选中文本", result.Text);
        Assert.Equal(1, focused.Calls);
        Assert.Equal(0, clipboard.Calls);
    }

    [Fact]
    public async Task CaptureAsync_DoesNotFallBackWhenFocusedControlIsSecure()
    {
        var focused = new FakeFocusedReader(FocusedTextSelectionResult.SecureField());
        var clipboard = new FakeClipboardReader(SelectedTextCaptureResult.Success("不应读取", SelectedTextCaptureSource.Clipboard));
        var service = new SelectedTextCaptureService(focused, clipboard);

        SelectedTextCaptureResult result = await service.CaptureAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.SecureField, result.Status);
        Assert.Null(result.Text);
        Assert.Equal(0, clipboard.Calls);
    }

    [Fact]
    public async Task CaptureAsync_FallsBackToClipboardWhenFocusedControlHasNoUsableText()
    {
        var focused = new FakeFocusedReader(FocusedTextSelectionResult.Unavailable());
        var clipboard = new FakeClipboardReader(SelectedTextCaptureResult.Success("剪贴板复制结果", SelectedTextCaptureSource.Clipboard));
        var service = new SelectedTextCaptureService(focused, clipboard);

        SelectedTextCaptureResult result = await service.CaptureAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.Success, result.Status);
        Assert.Equal(SelectedTextCaptureSource.Clipboard, result.Source);
        Assert.Equal("剪贴板复制结果", result.Text);
        Assert.Equal(1, clipboard.Calls);
    }

    [Fact]
    public async Task CaptureAsync_TreatsWhitespaceFromBothReadersAsNoSelection()
    {
        var focused = new FakeFocusedReader(FocusedTextSelectionResult.SelectedText("   \r\n"));
        var clipboard = new FakeClipboardReader(SelectedTextCaptureResult.Success("\t", SelectedTextCaptureSource.Clipboard));
        var service = new SelectedTextCaptureService(focused, clipboard);

        SelectedTextCaptureResult result = await service.CaptureAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.NoSelection, result.Status);
        Assert.Null(result.Text);
        Assert.Equal(1, clipboard.Calls);
    }

    [Fact]
    public async Task CaptureAsync_ReturnsClipboardFailureWhenFallbackCannotReadSelection()
    {
        var focused = new FakeFocusedReader(FocusedTextSelectionResult.NoSelection());
        var clipboard = new FakeClipboardReader(SelectedTextCaptureResult.Failed("clipboard unavailable"));
        var service = new SelectedTextCaptureService(focused, clipboard);

        SelectedTextCaptureResult result = await service.CaptureAsync(CancellationToken.None);

        Assert.Equal(SelectedTextCaptureStatus.Failed, result.Status);
        Assert.Contains("clipboard", result.Error);
    }

    private sealed class FakeFocusedReader : IFocusedTextSelectionReader
    {
        private readonly FocusedTextSelectionResult _result;

        public int Calls { get; private set; }

        public FakeFocusedReader(FocusedTextSelectionResult result)
        {
            _result = result;
        }

        public Task<FocusedTextSelectionResult> ReadAsync(CancellationToken cancellationToken)
        {
            Calls++;
            return Task.FromResult(_result);
        }
    }

    private sealed class FakeClipboardReader : IClipboardCopySelectionReader
    {
        private readonly SelectedTextCaptureResult _result;

        public int Calls { get; private set; }

        public FakeClipboardReader(SelectedTextCaptureResult result)
        {
            _result = result;
        }

        public Task<SelectedTextCaptureResult> ReadAsync(CancellationToken cancellationToken)
        {
            Calls++;
            return Task.FromResult(_result);
        }
    }
}
