using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class TextElementTruncatorTests
{
    [Fact]
    public void Truncate_DropsAnEmojiThatCrossesTheUtf16Limit()
    {
        const int MaxLength = 80;
        string prefix = new('a', MaxLength - 1);

        string truncated = TextElementTruncator.Truncate(prefix + "😀", MaxLength);

        Assert.Equal(prefix, truncated);
        Assert.True(truncated.Length <= MaxLength);
    }

    [Fact]
    public void Truncate_KeepsAnEmojiThatEndsAtTheUtf16Limit()
    {
        const int MaxLength = 80;
        string expected = new string('a', MaxLength - 2) + "😀";

        string truncated = TextElementTruncator.Truncate(expected + "x", MaxLength);

        Assert.Equal(expected, truncated);
        Assert.Equal(MaxLength, truncated.Length);
    }

    [Fact]
    public void Truncate_DoesNotLetOneLargeCombiningSequenceExceedTheUtf16Limit()
    {
        const int MaxLength = 80;
        string oneTextElement = "a" + new string('\u0301', MaxLength);

        string truncated = TextElementTruncator.Truncate(oneTextElement, MaxLength);

        Assert.Empty(truncated);
    }

    [Fact]
    public void Truncate_DoesNotAllocateForTheUnboundedTail()
    {
        const int MaxLength = 80;
        _ = TextElementTruncator.Truncate(new string('a', 200), MaxLength);
        string largeValue = new('a', 1_000_000);

        long before = GC.GetAllocatedBytesForCurrentThread();
        string truncated = TextElementTruncator.Truncate(largeValue, MaxLength);
        long allocated = GC.GetAllocatedBytesForCurrentThread() - before;

        Assert.Equal(MaxLength, truncated.Length);
        Assert.InRange(allocated, 0, 64 * 1024);
    }
}
