using Famo.Settings.Core.Ai;
using Famo.Settings.Theming;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;

namespace Famo.Settings.Views;

/// <summary>AI 助手（智）—— 供应商资料和安全密钥入口。</summary>
public sealed class AiPage : UserControl
{
    private static readonly AiProviderPreset[] Presets =
    {
        new("阿里云百炼", "阿里云百炼", "", "qwen3.6-flash", RequiresWorkspaceId: true),
        new("火山引擎 · 豆包 Seed", "火山引擎 · 豆包 Seed", "https://ark.cn-beijing.volces.com/api/v3/chat/completions", "doubao-seed-2-1-turbo-260628"),
        new("小米 MiMo", "小米 MiMo", "https://api.xiaomimimo.com/v1/chat/completions", "mimo-v2.5"),
        new("DeepSeek", "DeepSeek", DeepSeekResponsesApi.Endpoint, DeepSeekResponsesApi.FlashModel),
        new("MiniMax", "MiniMax", "https://api.minimaxi.com/v1/chat/completions", "MiniMax-M2.7-highspeed"),
        new("Google Gemini", "Google Gemini", "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions", "gemini-3.5-flash-lite"),
        new("OpenAI", "OpenAI", "https://api.openai.com/v1/chat/completions", "chat-latest"),
        new("+ 自定义（OpenAI 兼容）", "自定义 OpenAI 兼容", "", ""),
    };

    private readonly AiProviderProfileStore _providerStore = new();
    private readonly ISecretStore _secretStore = new WindowsCredentialSecretStore();
    private readonly AiProviderProfileService _providerService;

    private TextBlock _status = null!;
    private ComboBox _preset = null!;
    private TextBox _displayName = null!;
    private TextBox _workspaceId = null!;
    private FrameworkElement _workspaceIdRow = null!;
    private TextBox _endpoint = null!;
    private TextBox _model = null!;
    private PasswordBox _apiKey = null!;
    private FrameworkElement _deepSeekModelRow = null!;
    private StackPanel _providerList = null!;
    private ComboBox _searchBackend = null!;
    private PasswordBox _searchKey = null!;
    private TextBlock _searchKeyState = null!;
    private TextBlock _searchHint = null!;
    private FrameworkElement _searchCredentialDetails = null!;
    private FrameworkElement _searchDetails = null!;

    public AiPage()
    {
        _providerService = new AiProviderProfileService(_providerStore, _secretStore);
        BuildContent();
    }

    private void BuildContent()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("AI 助手", "只在你主动触发时工作；打字候选始终来自本地 Rime。"));
        sp.Children.Add(FamoUI.Banner(false, "设置页只保存供应商资料和密钥，不会向供应商发送请求"));

        _status = new TextBlock
        {
            Text = "选择一个供应商预设，填入 API Key 后保存；不会显示已保存的 API Key。",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            Margin = new Thickness(0, 0, 0, 12),
            TextWrapping = TextWrapping.Wrap,
        };
        sp.Children.Add(_status);

        _preset = BuildPresetCombo();
        _displayName = new TextBox { PlaceholderText = "供应商名称", MinWidth = 260 };
        _workspaceId = new TextBox
        {
            PlaceholderText = "从百炼业务空间详情复制",
            MinWidth = 300,
        };
        _endpoint = new TextBox { PlaceholderText = "https://...", MinWidth = 360 };
        _workspaceId.TextChanged += (_, _) => UpdateQwenEndpointPreview();
        _model = new TextBox { PlaceholderText = "模型 ID", MinWidth = 260 };
        _apiKey = new PasswordBox { PlaceholderText = "API Key", MinWidth = 260 };
        _deepSeekModelRow = FamoUI.Row("DeepSeek 模型", "V4 Flash 使用 Responses API；V4 Pro 当前继续使用 Chat Completions。",
            FamoUI.SegBar(new[] { "Chat · V4 Flash", "Reasoner · V4 Pro" }, 0, idx =>
            {
                _model.Text = idx == 0 ? DeepSeekResponsesApi.FlashModel : "deepseek-v4-pro";
                _endpoint.Text = idx == 0
                    ? DeepSeekResponsesApi.Endpoint
                    : "https://api.deepseek.com/chat/completions";
            }));
        _workspaceIdRow = FamoUI.Row(
            "Workspace ID",
            "必填；程序据此生成 https://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/responses，不内置项目方 ID。",
            _workspaceId);

        sp.Children.Add(FamoUI.Card("供应商（配一个就行）",
            FamoUI.Row("预设", "可直接填充 OpenAI-compatible endpoint 与模型。", _preset, divider: false),
            _deepSeekModelRow,
            FamoUI.Row("名称", "显示在 AI 对话和菜单状态里。", _displayName),
            _workspaceIdRow,
            FamoUI.Row("Endpoint", "必须是 HTTPS，或本机 HTTP 调试地址。", _endpoint),
            FamoUI.Row("模型", "必填；按供应商文档填写模型 ID。", _model),
            FamoUI.Row("API Key", "保存到 Windows Credential Manager；不会写入 JSON。", _apiKey),
            FamoUI.RowFull(BuildProviderActions(), divider: true)));

        _providerList = new StackPanel { Spacing = 8 };
        sp.Children.Add(FamoUI.Card("已保存供应商", FamoUI.RowFull(_providerList)));
        sp.Children.Add(BuildWebSearchCard());

        Content = sp;
        FillPreset(Presets[0]);
        RenderProviderList();
    }

    private FrameworkElement BuildWebSearchCard()
    {
        _searchBackend = new ComboBox { MinWidth = 180 };
        foreach (string backend in WebSearchBackends.All)
        {
            _searchBackend.Items.Add(new ComboBoxItem
            {
                Content = WebSearchBackends.DisplayName(backend),
                Tag = backend,
            });
        }
        string selected = WebSearchBackends.Normalize(App.Settings.Ai.WebSearchBackend);
        _searchBackend.SelectedIndex = Array.IndexOf(WebSearchBackends.All, selected);
        _searchBackend.SelectionChanged += (_, _) =>
        {
            App.Settings.Ai.WebSearchBackend = SelectedSearchBackend();
            App.Store.Save(App.Settings);
            _searchKey.Password = "";
            UpdateSearchBackendUi();
        };

        _searchKey = new PasswordBox
        {
            PlaceholderText = "搜索服务 API Key",
            MinWidth = 300,
        };
        _searchKeyState = new TextBlock
        {
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink3"),
            VerticalAlignment = VerticalAlignment.Center,
        };
        _searchHint = new TextBlock
        {
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink3"),
            TextWrapping = TextWrapping.Wrap,
        };

        var details = new StackPanel();
        details.Children.Add(FamoUI.Row(
            "搜索服务",
            "千问或 DeepSeek 内置搜索复用默认供应商配置；豆包与 Perplexity 使用独立搜索密钥。",
            _searchBackend,
            divider: false));
        var credentialDetails = new StackPanel();
        credentialDetails.Children.Add(FamoUI.Row(
            "搜索服务 API Key",
            "独立于上面的模型密钥，保存到 Windows Credential Manager。",
            _searchKey));
        credentialDetails.Children.Add(FamoUI.RowFull(BuildSearchKeyActions(), divider: true));
        _searchCredentialDetails = credentialDetails;
        details.Children.Add(_searchCredentialDetails);
        details.Children.Add(FamoUI.RowFull(_searchHint));
        _searchDetails = details;

        var enabled = FamoUI.Pill(App.Settings.Ai.AskWebSearchEnabled, value =>
        {
            App.Settings.Ai.AskWebSearchEnabled = value;
            App.Store.Save(App.Settings);
            _searchDetails.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
        });

        _searchDetails.Visibility = App.Settings.Ai.AskWebSearchEnabled
            ? Visibility.Visible
            : Visibility.Collapsed;
        UpdateSearchBackendUi();
        return FamoUI.Card(
            "任意提问 · 联网搜索",
            FamoUI.Row(
                "联网搜索",
                "开启后按所选搜索服务联网作答；只作用于任意提问，不影响划词技能与输入候选。",
                enabled,
                divider: false),
            FamoUI.RowFull(_searchDetails, divider: true));
    }

    private FrameworkElement BuildSearchKeyActions()
    {
        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            Margin = new Thickness(0, 12, 0, 0),
        };
        var paste = new Button { Content = "粘贴" };
        paste.Click += async (_, _) =>
        {
            try
            {
                DataPackageView data = Clipboard.GetContent();
                if (data.Contains(StandardDataFormats.Text))
                    _searchKey.Password = (await data.GetTextAsync()).Trim();
            }
            catch
            {
                SetStatus("读取剪贴板失败。");
            }
        };
        var save = new Button { Content = "保存" };
        save.Click += (_, _) => SaveSearchKey();
        var clear = new Button { Content = "清除" };
        clear.Click += (_, _) => ClearSearchKey();
        row.Children.Add(paste);
        row.Children.Add(save);
        row.Children.Add(clear);
        row.Children.Add(_searchKeyState);
        return row;
    }

    private void SaveSearchKey()
    {
        string key = _searchKey.Password.Trim();
        if (key.Length == 0)
        {
            SetStatus($"请输入{WebSearchBackends.DisplayName(SelectedSearchBackend())}的 API Key。");
            return;
        }
        try
        {
            _secretStore.SetSecret(WebSearchBackends.SecretName(SelectedSearchBackend()), key);
            _searchKey.Password = "";
            UpdateSearchBackendUi();
            SetStatus("搜索服务 API Key 已保存；联网提问会先检索，再由默认供应商作答。");
        }
        catch (Exception ex)
        {
            SetStatus("保存搜索服务 API Key 失败：" + ex.Message);
        }
    }

    private void ClearSearchKey()
    {
        try
        {
            _secretStore.DeleteSecret(WebSearchBackends.SecretName(SelectedSearchBackend()));
            _searchKey.Password = "";
            UpdateSearchBackendUi();
            SetStatus("搜索服务 API Key 已清除。");
        }
        catch (Exception ex)
        {
            SetStatus("清除搜索服务 API Key 失败：" + ex.Message);
        }
    }

    private string SelectedSearchBackend() =>
        (_searchBackend.SelectedItem as ComboBoxItem)?.Tag as string
        ?? WebSearchBackends.Doubao;

    private void UpdateSearchBackendUi()
    {
        string backend = SelectedSearchBackend();
        bool usesProviderCredential = WebSearchBackends.UsesProviderCredential(backend);
        _searchCredentialDetails.Visibility = usesProviderCredential
            ? Visibility.Collapsed
            : Visibility.Visible;
        if (usesProviderCredential)
        {
            _searchKeyState.Text = "";
            _searchHint.Text =
                $"{WebSearchBackends.KeyHint(backend)}\n" +
                "联网时按供应商 Responses API 发送 tools:[{\"type\":\"web_search\"}]；" +
                "默认供应商与所选搜索服务不一致时会直接提示。";
            return;
        }

        bool configured;
        try
        {
            configured = !string.IsNullOrWhiteSpace(
                _secretStore.GetSecret(WebSearchBackends.SecretName(backend)));
        }
        catch
        {
            configured = false;
        }
        _searchKeyState.Text = configured ? "已设置" : "未设置";
        _searchHint.Text =
            $"端点：{WebSearchBackends.Endpoint(backend)}\n{WebSearchBackends.KeyHint(backend)}" +
            $"\n配好后由{WebSearchBackends.DisplayName(backend)}取回网页、交给你的默认供应商作答；" +
            "两家的 Key 分开存，切换不用重填。未配置专用 Key 时自动退回普通问答。";
    }

    private ComboBox BuildPresetCombo()
    {
        var combo = new ComboBox { MinWidth = 260 };
        foreach (AiProviderPreset preset in Presets)
        {
            combo.Items.Add(new ComboBoxItem { Content = preset.Label });
        }

        combo.SelectedIndex = 0;
        combo.SelectionChanged += (_, _) =>
        {
            if (combo.SelectedIndex >= 0)
            {
                FillPreset(Presets[combo.SelectedIndex]);
            }
        };
        return combo;
    }

    private FrameworkElement BuildProviderActions()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };
        var save = new Button { Content = "保存供应商" };
        save.Click += (_, _) => SaveProviderProfile();
        var clear = new Button { Content = "清空表单" };
        clear.Click += (_, _) => ClearProviderForm();
        row.Children.Add(save);
        row.Children.Add(clear);
        return row;
    }

    private void FillPreset(AiProviderPreset preset)
    {
        _displayName.Text = preset.DisplayName;
        _workspaceId.Text = "";
        _endpoint.Text = preset.Endpoint;
        _model.Text = preset.Model;
        _workspaceIdRow.Visibility = preset.RequiresWorkspaceId
            ? Visibility.Visible
            : Visibility.Collapsed;
        _endpoint.IsReadOnly = preset.RequiresWorkspaceId;
        _deepSeekModelRow.Visibility = preset.DisplayName == "DeepSeek" ? Visibility.Visible : Visibility.Collapsed;
        SetStatus("已填充 " + preset.Label + " 预设，请输入 API Key 后保存。");
    }

    private void SaveProviderProfile()
    {
        try
        {
            string endpoint = IsQwenPresetSelected()
                ? QwenResponsesApi.BuildBeijingEndpoint(_workspaceId.Text)
                : _endpoint.Text;
            AiProviderProfile profile = _providerService.AddProfile(new AiProviderProfileDraft
            {
                DisplayName = _displayName.Text,
                Endpoint = endpoint,
                Model = _model.Text,
                ApiKey = _apiKey.Password,
                MakeDefault = true,
            });

            _apiKey.Password = "";
            RenderProviderList();
            SetStatus($"{profile.DisplayName} 密钥已保存；不会显示已保存的 API Key。");
        }
        catch (InvalidDataException ex)
        {
            SetStatus(ex.Message);
        }
        catch (Exception ex)
        {
            SetStatus("保存 AI 供应商失败：" + ex.Message);
        }
    }

    private void SetDefaultProvider(string id)
    {
        try
        {
            AiProviderProfile profile = _providerService.SetDefault(id);
            RenderProviderList();
            SetStatus($"{profile.DisplayName} 已设为默认 AI 供应商。");
        }
        catch (Exception ex)
        {
            SetStatus("设置默认 AI 供应商失败：" + ex.Message);
        }
    }

    private void DeleteProviderProfile(string id)
    {
        try
        {
            if (_providerService.DeleteProfile(id))
            {
                RenderProviderList();
                SetStatus("AI 供应商已删除。");
                return;
            }

            SetStatus("找不到要删除的 AI 供应商。");
        }
        catch (Exception ex)
        {
            SetStatus("删除 AI 供应商失败：" + ex.Message);
        }
    }

    private void RenderProviderList()
    {
        if (_providerList is null) return;

        _providerList.Children.Clear();
        IReadOnlyList<AiProviderProfile> profiles = _providerStore.Load();
        if (profiles.Count == 0)
        {
            _providerList.Children.Add(new TextBlock
            {
                Text = "暂无供应商；保存一个后，AI 对话会使用默认供应商。",
                FontSize = 13,
                Foreground = FamoUI.Br("Famo.Ink2"),
                Margin = new Thickness(0, 8, 0, 4),
                TextWrapping = TextWrapping.Wrap,
            });
            return;
        }

        foreach (AiProviderProfile profile in profiles)
        {
            _providerList.Children.Add(BuildProviderEntry(profile));
        }
    }

    private UIElement BuildProviderEntry(AiProviderProfile profile)
    {
        var info = new StackPanel { Spacing = 4 };
        info.Children.Add(new TextBlock
        {
            Text = profile.DisplayName + (profile.IsDefault ? " · 默认" : ""),
            FontSize = 13.5,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = FamoUI.Br("Famo.Ink"),
        });
        info.Children.Add(new TextBlock
        {
            Text = (string.IsNullOrWhiteSpace(profile.Model) ? "缺少模型" : profile.Model) + " · " + profile.Endpoint,
            FontSize = 12,
            FontFamily = FamoUI.Mono,
            Foreground = FamoUI.Br("Famo.Ink2"),
            TextWrapping = TextWrapping.Wrap,
        });
        info.Children.Add(new TextBlock
        {
            Text = HasSecret(profile) ? "密钥已保存（不会显示已保存的 API Key）" : "需要重新保存 API Key",
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink3"),
        });

        var makeDefault = new Button { Content = "设为默认", IsEnabled = !profile.IsDefault };
        makeDefault.Click += (_, _) => SetDefaultProvider(profile.Id);
        var delete = new Button { Content = "删除" };
        delete.Click += (_, _) => DeleteProviderProfile(profile.Id);

        var actions = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            VerticalAlignment = VerticalAlignment.Center,
        };
        actions.Children.Add(makeDefault);
        actions.Children.Add(delete);

        var grid = new Grid { ColumnSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(info, 0);
        Grid.SetColumn(actions, 1);
        grid.Children.Add(info);
        grid.Children.Add(actions);

        return new Border
        {
            CornerRadius = new CornerRadius(7),
            Background = FamoUI.Br("Famo.Field"),
            BorderBrush = FamoUI.Br("Famo.Line2"),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(10),
            Child = grid,
        };
    }

    private bool HasSecret(AiProviderProfile profile)
    {
        try
        {
            return _secretStore.GetSecret(profile.SecretName) is { Length: > 0 };
        }
        catch
        {
            return false;
        }
    }

    private void ClearProviderForm()
    {
        _displayName.Text = "";
        _workspaceId.Text = "";
        _endpoint.Text = "";
        _model.Text = "";
        _apiKey.Password = "";
        SetStatus("表单已清空。");
    }

    private void SetStatus(string text)
    {
        if (_status != null) _status.Text = text;
    }

    private bool IsQwenPresetSelected() =>
        _preset.SelectedIndex >= 0
        && Presets[_preset.SelectedIndex].RequiresWorkspaceId;

    private void UpdateQwenEndpointPreview()
    {
        if (_endpoint is null || !IsQwenPresetSelected()) return;

        try
        {
            _endpoint.Text = QwenResponsesApi.BuildBeijingEndpoint(_workspaceId.Text);
        }
        catch (InvalidDataException)
        {
            _endpoint.Text = "";
        }
    }

    private sealed record AiProviderPreset(
        string Label,
        string DisplayName,
        string Endpoint,
        string Model,
        bool RequiresWorkspaceId = false);
}
