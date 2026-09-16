using System.Windows;
using System.Windows.Controls;
using RemoteFsClient.Models;
using RemoteFsClient.Services;

namespace RemoteFsClient;

public partial class SiteEditWindow : Window
{
    private readonly SiteInfo _draft;
    private readonly bool _isNew;
    private readonly string? _originalName;

    /// <summary>最近一次解析出的 SSH 配置（点「刷新」会重新读文件）。</summary>
    private IReadOnlyList<SshHostEntry> _sshEntries = Array.Empty<SshHostEntry>();

    /// <summary>下拉里「自动匹配」项的哨兵值。</summary>
    private const string AutoBind = "";

    /// <summary>保存动作中录入的密码（明文只在内存；由 MainWindow 写入凭据管理器）。</summary>
    public string? EnteredPassword { get; private set; }

    public SiteEditWindow(SiteInfo draft, bool isNew = true)
    {
        InitializeComponent();
        _draft = draft;
        _isNew = isNew;
        _originalName = draft.Name;
        ApplyLanguage();

        TbName.Text = draft.Name;
        CbType.SelectedItem = CbType.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (string?)i.Content == draft.Type);
        TbHost.Text = draft.Host;
        TbPort.Text = draft.Port?.ToString() ?? "";
        TbUser.Text = draft.Username;
        TbPath.Text = draft.StartPath;
        CbFtpUtf8.IsChecked = draft.FtpUseUtf8;

        BuildTerminalChoices(draft.Terminal);
        ReloadSshEntries(selectAlias: draft.SshHostAlias);

        UpdateTerminalTabVisibility();
        PwdHint.Text = CredentialStore.Exists(draft.Name)
            ? (Ui.IsEnglish ? "Password is saved in Windows Credential Manager. Leave blank to keep it." : "已保存密码（Windows 凭据管理器）。留空表示不修改。")
            : (Ui.IsEnglish ? "The password is stored in Windows Credential Manager, not JSON." : "密码将存入 Windows 凭据管理器（DPAPI），不会写入 JSON。");
    }

    private void ApplyLanguage()
    {
        Title = Ui.IsEnglish ? "Edit site" : "编辑站点";
        SiteTab.Header = Ui.T("SiteTab");
        TerminalTab.Header = Ui.T("TerminalTab");
        NameLabel.Text = Ui.T("Name"); ProtocolLabel.Text = Ui.T("Protocol"); HostLabel.Text = Ui.IsEnglish ? "Host" : "主机";
        PortLabel.Text = Ui.IsEnglish ? "Port (optional)" : "端口（可空）"; UserLabel.Text = Ui.T("Username");
        PathLabel.Text = Ui.T("StartPath"); LblFtpUtf8.Text = Ui.T("FtpEncoding"); CbFtpUtf8.Content = Ui.T("ForceUtf8");
        PasswordLabel.Text = Ui.T("Password"); SaveButton.Content = Ui.T("Save"); CancelButton.Content = Ui.T("Cancel");

        TerminalProgramLabel.Text = Ui.T("TerminalProgram");
        SshBindingLabel.Text = Ui.T("SshBinding");
        RefreshSshButton.Content = Ui.T("Refresh");
        TerminalHint.Text = Ui.T("TerminalTabHint");
    }

    // ── 终端程序 ────────────────────────────────────────────────────────────
    private void BuildTerminalChoices(string? current)
    {
        CbTerminal.Items.Clear();
        var global = AppSettings.NormalizeTerminal(AppSettings.Load().Terminal);
        CbTerminal.Items.Add(new ComboBoxItem { Tag = AutoBind, Content = Ui.T("TerminalFollowGlobal").Replace("{0}", TerminalDisplayName(global)) });
        CbTerminal.Items.Add(new ComboBoxItem { Tag = "wt", Content = Ui.T("TerminalWt") });
        CbTerminal.Items.Add(new ComboBoxItem { Tag = "powershell", Content = Ui.T("TerminalPwsh") });
        CbTerminal.Items.Add(new ComboBoxItem { Tag = "vscode", Content = Ui.T("TerminalVsCode") });

        var want = (current ?? "").Trim().ToLowerInvariant();
        var selected = CbTerminal.Items.Cast<ComboBoxItem>()
            .FirstOrDefault(i => string.Equals(i.Tag as string, want, StringComparison.OrdinalIgnoreCase));
        CbTerminal.SelectedItem = selected ?? CbTerminal.Items[0];
    }

    private static string TerminalDisplayName(string key) => key switch
    {
        "powershell" => Ui.T("TerminalPwsh"),
        "vscode" => Ui.T("TerminalVsCode"),
        _ => Ui.T("TerminalWt"),
    };

    // ── SSH 绑定 ────────────────────────────────────────────────────────────
    private void ReloadSshEntries(string? selectAlias)
    {
        _sshEntries = SshConfigReader.Read();

        int port = int.TryParse(TbPort.Text.Trim(), out var p) && p > 0
            ? p
            : (IsSftp() ? 22 : 21);
        string host = TbHost.Text.Trim();
        string user = TbUser.Text.Trim();

        var compatible = _sshEntries
            .Where(e => SshConfigReader.MatchesSite(e, host, port, user))
            .OrderBy(e => e.Alias, StringComparer.OrdinalIgnoreCase)
            .ToList();
        var excluded = SshConfigReader.SameHostButDifferentIdentity(_sshEntries, host, port, user);

        CbSshHost.Items.Clear();
        CbSshHost.Items.Add(new ComboBoxItem { Tag = AutoBind, Content = Ui.T("SshBindingAuto") });
        foreach (var e in compatible)
            CbSshHost.Items.Add(new ComboBoxItem { Tag = e.Alias, Content = e.Summary });

        var pick = CbSshHost.Items.Cast<ComboBoxItem>()
            .FirstOrDefault(i => string.Equals(i.Tag as string, selectAlias ?? "", StringComparison.OrdinalIgnoreCase));
        CbSshHost.SelectedItem = pick ?? CbSshHost.Items[0];

        UpdateSshStatus(compatible, excluded);
    }

    private void UpdateSshStatus(IReadOnlyList<SshHostEntry> compatible, IReadOnlyList<SshHostEntry> excluded)
    {
        var lines = new List<string>();
        var selectedAlias = (CbSshHost.SelectedItem as ComboBoxItem)?.Tag as string;
        var selected = compatible.FirstOrDefault(e => string.Equals(e.Alias, selectedAlias, StringComparison.OrdinalIgnoreCase));
        if (selected != null)
            lines.Add(Ui.T("SshBindingSelected").Replace("{0}", selected.Summary));

        if (compatible.Count == 0 && excluded.Count == 0)
            lines.Add(Ui.T("SshBindingNoConfig"));
        else
            lines.Add(Ui.T("SshBindingHint"));

        if (excluded.Count > 0)
            lines.Add(Ui.T("SshBindingExcluded")
                .Replace("{0}", excluded.Count.ToString())
                .Replace("{1}", string.Join("; ", excluded.Select(e => e.Summary))));

        SshStatus.Text = string.Join(Environment.NewLine, lines);
    }

    private void OnSshHostChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!IsLoaded) return;
        int port = int.TryParse(TbPort.Text.Trim(), out var p) && p > 0 ? p : (IsSftp() ? 22 : 21);
        var compatible = _sshEntries.Where(x => SshConfigReader.MatchesSite(x, TbHost.Text.Trim(), port, TbUser.Text.Trim())).ToList();
        var excluded = SshConfigReader.SameHostButDifferentIdentity(_sshEntries, TbHost.Text.Trim(), port, TbUser.Text.Trim());
        UpdateSshStatus(compatible, excluded);
    }

    private void OnRefreshSsh(object sender, RoutedEventArgs e)
    {
        var keep = (CbSshHost.SelectedItem as ComboBoxItem)?.Tag as string;
        ReloadSshEntries(keep);
    }

    private bool IsSftp()
    {
        var type = (CbType.SelectedItem as ComboBoxItem)?.Content as string ?? "sftp";
        return type.StartsWith("sftp", StringComparison.OrdinalIgnoreCase) || type.StartsWith("scp", StringComparison.OrdinalIgnoreCase);
    }

    private void OnTypeChanged(object sender, SelectionChangedEventArgs e)
    {
        UpdateFtpEncodingVisibility();
        UpdateTerminalTabVisibility();
    }

    /// <summary>主机/端口/用户名改动后，重新过滤可绑定的 SSH 配置（校验必须同步）。</summary>
    private void OnIdentityChanged(object sender, TextChangedEventArgs e)
    {
        if (!IsLoaded) return;
        ReloadSshEntries((CbSshHost.SelectedItem as ComboBoxItem)?.Tag as string);
    }

    private void UpdateFtpEncodingVisibility()
    {
        var type = (CbType.SelectedItem as ComboBoxItem)?.Content as string ?? "sftp";
        bool ftp = type is "ftp" or "ftps";
        LblFtpUtf8.Visibility = ftp ? Visibility.Visible : Visibility.Collapsed;
        CbFtpUtf8.Visibility = ftp ? Visibility.Visible : Visibility.Collapsed;
    }

    /// <summary>只有 SFTP/SCP（有 shell 通道）才显示「终端配置」标签页。</summary>
    private void UpdateTerminalTabVisibility()
    {
        bool ssh = IsSftp();
        TerminalTab.Visibility = ssh ? Visibility.Visible : Visibility.Collapsed;
        if (!ssh && Tabs.SelectedItem == TerminalTab) Tabs.SelectedItem = SiteTab;
    }

    private void OnSave(object sender, RoutedEventArgs e)
    {
        string name = TbName.Text.Trim();
        if (name.Length == 0) { MessageBox.Show(Ui.IsEnglish ? "Name is required." : "名称不能为空"); return; }
        if (name.Contains(':') || name.Contains('/') || name.Contains('\\'))
        { MessageBox.Show(Ui.IsEnglish ? "Name cannot contain : / \\ characters." : "名称不能包含 : / \\ 等字符"); return; }

        if (!int.TryParse(TbPort.Text.Trim(), out int port) || port <= 0) port = 0;

        _draft.Name = name;
        _draft.Type = (CbType.SelectedItem as ComboBoxItem)?.Content as string ?? "sftp";
        _draft.Host = TbHost.Text.Trim();
        _draft.Port = port > 0 ? port : null;
        _draft.Username = TbUser.Text;
        _draft.StartPath = string.IsNullOrWhiteSpace(TbPath.Text) ? "/" : TbPath.Text.Trim();
        _draft.FtpUseUtf8 = CbFtpUtf8.IsChecked != false;

        // 终端配置（只有 SSH 类型才有意义；FTP 站点清空，避免留下误导性配置）
        bool ssh = IsSftp();
        _draft.Terminal = ssh ? ((CbTerminal.SelectedItem as ComboBoxItem)?.Tag as string) is { Length: > 0 } t ? t : null : null;

        string? alias = ssh ? (CbSshHost.SelectedItem as ComboBoxItem)?.Tag as string : null;
        if (!string.IsNullOrEmpty(alias))
        {
            // 保存前再校验一次：主机/端口/用户名必须一致，防止改了站点信息后留下错绑
            int effectivePort = _draft.Port ?? (_draft.IsSshCapable ? 22 : 21);
            var entry = _sshEntries.FirstOrDefault(x => string.Equals(x.Alias, alias, StringComparison.OrdinalIgnoreCase));
            if (entry == null || !SshConfigReader.MatchesSite(entry, _draft.Host, effectivePort, _draft.Username))
            {
                MessageBox.Show(
                    Ui.IsEnglish
                        ? $"The selected SSH config '{alias}' no longer matches this site's host, port, or user. Binding was cleared."
                        : $"所选 SSH 配置「{alias}」与本站点的主机/端口/用户名不一致，绑定已清除。",
                    Ui.IsEnglish ? "Terminal configuration" : "终端配置", MessageBoxButton.OK, MessageBoxImage.Warning);
                alias = null;
            }
        }
        _draft.SshHostAlias = string.IsNullOrEmpty(alias) ? null : alias;

        var pwd = PbPassword.Password;
        EnteredPassword = pwd.Length > 0 ? pwd : null;

        // 改名时由 MainWindow 处理凭据迁移；这里只处理"新名称与 WinSCP 站点冲突"提示
        if (_isNew || _originalName != name)
        {
            try
            {
                using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                    $@"Software\Martin Prikryl\WinSCP 2\Sessions\{name}");
                if (key != null)
                    MessageBox.Show("该名称与 WinSCP 中已保存的站点同名，连接测试时将直接复用 WinSCP 的凭据。",
                        "提示", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch { }
        }
        DialogResult = true;
    }
}
