using Windows.ApplicationModel.DataTransfer;

namespace Famo.Settings.Interop;

public static class ClipboardReader
{
    public static async Task<string?> ReadTextAsync()
    {
        try
        {
            DataPackageView content = Windows.ApplicationModel.DataTransfer.Clipboard.GetContent();
            if (content.Contains("ExcludeClipboardContentFromMonitorProcessing")) return null;
            if (!content.Contains(StandardDataFormats.Text)) return null;
            return await content.GetTextAsync().AsTask();
        }
        catch
        {
            return null;
        }
    }
}
