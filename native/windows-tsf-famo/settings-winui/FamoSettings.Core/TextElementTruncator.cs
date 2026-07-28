using System.Globalization;

namespace Famo.Settings.Core;

public static class TextElementTruncator
{
    public static string Truncate(string value, int maxUtf16Length)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(maxUtf16Length);
        if (value.Length <= maxUtf16Length) return value;

        // A grapheme boundary depends on the text to its left and the next
        // scalar, not on an arbitrarily large tail. Probe at most two UTF-16
        // units beyond the limit so a following surrogate pair is complete.
        int probeLength = (int)Math.Min(value.Length, (long)maxUtf16Length + 2);
        string probe = value[..probeLength];
        int[] elementStarts = StringInfo.ParseCombiningCharacters(probe);
        int boundaryIndex = Array.BinarySearch(elementStarts, maxUtf16Length);
        int boundary = boundaryIndex >= 0
            ? elementStarts[boundaryIndex]
            : elementStarts[~boundaryIndex - 1];
        return value[..boundary];
    }
}
