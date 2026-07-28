using System.Buffers;
using System.Text;

namespace Famo.Settings.Core.Ai;

internal static class BoundedHttpContent
{
    internal const int MaxResponseBytes = 8 * 1024 * 1024;
    private const int BufferBytes = 64 * 1024;

    internal static async Task<byte[]> ReadBytesAsync(
        HttpContent content,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(content);
        long? declaredLength = content.Headers.ContentLength;
        if (declaredLength is < 0 or > MaxResponseBytes)
        {
            throw new InvalidDataException(
                "HTTP response exceeds the 8 MiB limit.");
        }

        using Stream source =
            await content.ReadAsStreamAsync(cancellationToken);
        int capacity = declaredLength is > 0
            ? checked((int)declaredLength.Value)
            : 0;
        using var destination = new MemoryStream(capacity);
        byte[] buffer = ArrayPool<byte>.Shared.Rent(BufferBytes);
        try
        {
            while (true)
            {
                int remaining = MaxResponseBytes - checked((int)destination.Length);
                int requested = Math.Min(buffer.Length, remaining + 1);
                int read = await source.ReadAsync(
                    buffer.AsMemory(0, requested), cancellationToken);
                if (read == 0)
                {
                    return destination.ToArray();
                }
                if (read > remaining)
                {
                    throw new InvalidDataException(
                        "HTTP response exceeds the 8 MiB limit.");
                }
                destination.Write(buffer, 0, read);
            }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer, clearArray: true);
        }
    }

    internal static async Task<string> ReadUtf8Async(
        HttpContent content,
        CancellationToken cancellationToken)
    {
        byte[] bytes = await ReadBytesAsync(content, cancellationToken);
        try
        {
            return Encoding.UTF8.GetString(bytes);
        }
        finally
        {
            Array.Clear(bytes);
        }
    }
}
