using Famo.Settings.Core;
using Famo.Settings.Core.Ai;
using Famo.Settings.Core.Selection;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class SelectedTextCaptureServiceTests
{
    [Fact]
    public void ToolboxEligibility_RejectsEveryUnusableCaptureBeforeWindowCreation()
    {
        FamoSettings settings = SettingsStore.CreateDefault();

        Assert.Contains("未选中", AiSelectionToolboxEligibility.RejectionReason(
            settings, SelectedTextCaptureResult.NoSelection()));
        Assert.Contains("安全输入框", AiSelectionToolboxEligibility.RejectionReason(
            settings, SelectedTextCaptureResult.SecureField()));
        Assert.Contains("capture failed", AiSelectionToolboxEligibility.RejectionReason(
            settings, SelectedTextCaptureResult.Failed("capture failed")));
        Assert.Contains("未选中", AiSelectionToolboxEligibility.RejectionReason(
            settings, SelectedTextCaptureResult.Success(" \r\n ", SelectedTextCaptureSource.Clipboard)));
        Assert.Contains("2000", AiSelectionToolboxEligibility.RejectionReason(
            settings,
            SelectedTextCaptureResult.Success(
                new string('法', AiSelectionSkillService.MaxSelectionLength + 1),
                SelectedTextCaptureSource.FocusedControl)));
        Assert.Null(AiSelectionToolboxEligibility.RejectionReason(
            settings,
            SelectedTextCaptureResult.Success(
                new string('法', AiSelectionSkillService.MaxSelectionLength),
                SelectedTextCaptureSource.FocusedControl)));
    }

    [Fact]
    public void ToolboxEligibility_RequiresAskOrAnotherEnabledSkill()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.AskAnythingSkillEnabled = false;
        settings.Ai.PolishSkillEnabled = false;
        settings.Ai.SourceCheckSkillEnabled = false;
        settings.Ai.ResearchAssistSkillEnabled = false;
        settings.Ai.PublishFormattingSkillEnabled = false;
        settings.Ai.TranslationSkillEnabled = false;
        settings.Ai.PromptOptimizeSkillEnabled = false;
        SelectedTextCaptureResult capture = SelectedTextCaptureResult.Success(
            "选中文本", SelectedTextCaptureSource.FocusedControl);

        Assert.Contains("没有已启用", AiSelectionToolboxEligibility.RejectionReason(settings, capture));
        settings.Ai.PolishSkillEnabled = true;
        Assert.Null(AiSelectionToolboxEligibility.RejectionReason(settings, capture));
        settings.Ai.PolishSkillEnabled = false;
        settings.Ai.AskAnythingSkillEnabled = true;
        Assert.Null(AiSelectionToolboxEligibility.RejectionReason(settings, capture));
    }

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
