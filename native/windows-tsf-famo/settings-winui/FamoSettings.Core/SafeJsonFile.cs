namespace Famo.Settings.Core;

/// <summary>
/// 各 store（SettingsStore/AiProviderProfileStore/ClipboardHistoryStore/QuickPhraseStore/
/// EmojiRecentsStore/PromptLibraryStore）共用的磁盘读写基础操作，避免同一段
/// 「写临时文件再 rename」被复制 7 份、且读取失败时各自处理不一致（有的静默清空重写，
/// 有的没有异常保护直接崩溃）。
/// </summary>
public static class SafeJsonFile
{
    /// <summary>
    /// 耐久原子写：用唯一、句柄固定且由内核负责崩溃清理的临时文件，
    /// flush 后再从同一句柄 rename 替换目标路径。
    /// </summary>
    public static void WriteAtomic(string path, string content)
    {
        using IDisposable held = UserDataTransactionLock.Acquire(
            Path.GetDirectoryName(Path.GetFullPath(path)));
        SeedFileTransaction.WriteDurableAtomic(
            path, content, overwrite: true);
    }

    /// <summary>
    /// 读取 <paramref name="path"/> 并用 <paramref name="parse"/> 解析。解析失败时：
    /// 把原文件备份成 <c>.bak</c>（不存在 .bak 或已有 .bak 早于原文件才覆盖，绝不清空/覆盖原文件本身），
    /// 然后把异常原样向上抛出——不在此处吞掉，由调用方决定是回退默认值还是让用户可见地失败。
    /// </summary>
    public static T Read<T>(string path, Func<string, T> parse)
    {
        using IDisposable held = UserDataTransactionLock.Acquire(
            Path.GetDirectoryName(Path.GetFullPath(path)));
        string content = File.ReadAllText(path);
        try
        {
            return parse(content);
        }
        catch
        {
            BackupCorrupt(path);
            throw;
        }
    }

    private static void BackupCorrupt(string path)
    {
        try
        {
            string bak = path + ".bak";
            if (!File.Exists(bak) || File.GetLastWriteTimeUtc(bak) < File.GetLastWriteTimeUtc(path))
            {
                File.Copy(path, bak, overwrite: true);
            }
        }
        catch
        {
            // 备份失败不能盖过原始解析异常。
        }
    }
}
