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

    /// <summary>保存动作中录入的密码（明文只在内存；由 MainWindow 写入凭据管理器）。</summary>
    public string? EnteredPassword { get; private set; }

    public SiteEditWindow(SiteInfo draft, bool isNew = true)
    {
        InitializeComponent();
        _draft = draft;
        _isNew = isNew;
        _originalName = draft.Name;

        TbName.Text = draft.Name;
        CbType.SelectedItem = CbType.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (string?)i.Content == draft.Type);
        TbHost.Text = draft.Host;
        TbPort.Text = draft.Port?.ToString() ?? "";
        TbUser.Text = draft.Username;
        TbPath.Text = draft.StartPath;
        PwdHint.Text = CredentialStore.Exists(draft.Name)
            ? "已保存密码（Windows 凭据管理器）。留空表示不修改。"
            : "密码将存入 Windows 凭据管理器（DPAPI），不会写入 JSON。";
    }

    private void OnSave(object sender, RoutedEventArgs e)
    {
        string name = TbName.Text.Trim();
        if (name.Length == 0) { MessageBox.Show("名称不能为空"); return; }
        if (name.Contains(':') || name.Contains('/') || name.Contains('\\'))
        { MessageBox.Show("名称不能包含 : / \\ 等字符"); return; }

        if (!int.TryParse(TbPort.Text.Trim(), out int port) || port <= 0) port = 0;

        _draft.Name = name;
        _draft.Type = (CbType.SelectedItem as ComboBoxItem)?.Content as string ?? "sftp";
        _draft.Host = TbHost.Text.Trim();
        _draft.Port = port > 0 ? port : null;
        _draft.Username = TbUser.Text;
        _draft.StartPath = string.IsNullOrWhiteSpace(TbPath.Text) ? "/" : TbPath.Text.Trim();

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
