using Xunit;

namespace Famo.Settings.Tests;

public sealed class KeyboardPageParityContractTests
{
    [Fact]
    public void KeyboardPage_ContainsLatestMacKeyboardGroupsWithRealApplyPaths()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/KeyboardPage.cs"));

        Assert.Contains("保存并应用当前配置", page);
        Assert.Contains("输入方式", page);
        Assert.Contains("拼音输入", page);
        Assert.Contains("双拼输入", page);
        Assert.Contains("五笔输入", page);
        Assert.Contains("双拼方案", page);
        Assert.Contains("五笔方案", page);
        Assert.Contains("WubiScheme", page);
        Assert.Contains("App.SaveAndApplySchema();", page);

        Assert.Contains("默认状态", page);
        Assert.Contains("中英文", page);
        Assert.Contains("简繁体", page);
        Assert.Contains("全半角", page);
        Assert.Contains("App.SaveAndApplyOption();", page);

        Assert.Contains("五笔专属", page);
        Assert.Contains("候选编码提示", page);
        Assert.Contains("空码自动清码", page);
        Assert.Contains("单字候选", page);
        Assert.Contains("z 临时拼音反查", page);
        Assert.Contains("App.SaveAndApplyDeploy();", page);

        Assert.Contains("模糊拼音", page);
        Assert.Contains("符号", page);
        Assert.Contains("中文下使用英文标点", page);
        Assert.Contains("输入中文时", page);
        Assert.Contains("数字间标点", page);
        Assert.Contains("成对标点自动补全", page);
        Assert.Contains("中英文之间自动加空格", page);
        Assert.Contains("中文与数字之间自动加空格", page);
        Assert.Contains("C.CjkEnglishSpacing", page);
        Assert.Contains("C.CjkNumberSpacing", page);
        Assert.Contains("在候选中显示 emoji", page);
        Assert.Contains("App 默认英文", page);
        Assert.Contains("AppEnglishExes", page);
        Assert.Contains("NormalizeAppEnglishExe", page);
        Assert.Contains("RemoveAll", page);
        Assert.Contains("已在列表中", page);
        Assert.Contains("删除", page);
        Assert.DoesNotContain("AcceptsReturn = true", page);
        Assert.DoesNotContain("每行一个程序 exe 名", page);
        Assert.DoesNotContain("输入方式请到", page);
    }

    private static string RepoFile(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath);
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate {relativePath}");
    }
}
