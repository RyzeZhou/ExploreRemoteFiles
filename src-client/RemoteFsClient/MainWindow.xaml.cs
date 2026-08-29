using System.Collections.ObjectModel;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using RemoteFsClient.Services;

namespace RemoteFsClient;

public partial class MainWindow : Window
{
    private readonly ObservableCollection<Models.SiteInfo> _sites = new();
    private Models.SiteInfo? _selected;

    public MainWindow()
    {
        InitializeComponent();
        SiteList.ItemsSource = _sites;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        foreach (var s in SiteStore.Load()) _sites.Add(s);
        if (_sites.Count > 0) SiteList.SelectedIndex = 0;
        StatusText.Text = $"配置文件：{SiteStore.FilePathForDisplay}";
    }

    private void OnSiteSelected(object sender, SelectionChangedEventArgs e)
    {
        _selected = SiteList.SelectedItem as Models.SiteInfo;
        ShowDetails();
    }

    private void ShowDetails()
    {
        var s = _selected;
        if (s == null)
        {
            DName.Text = DType.Text = DHost.Text = DUser.Text = DPath.Text = "";
            DPwd.Text = ""; DShared.Text = "";
            return;
        }
        DName.Text = s.Name;
        DType.Text = s.Type;
        DHost.Text = $"{s.Host}:{s.EffectivePort}";
        DUser.Text = s.Username;
        DPath.Text = s.StartPath;
        DPwd.Text = CredentialStore.Exists(s.Name) ? "已保存（Windows 凭据管理器）" : "未保存";
        DShared.Text = RegistryShared(s.Name) ? "是（与 WinSCP 同名站点）" : "否";
    }

    private static bool RegistryShared(string name)
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                $@"Software\Martin Prikryl\WinSCP 2\Sessions\{name}");
            return key != null;
        }
        catch { return false; }
    }

    private void OnNew(object sender, RoutedEventArgs e)
        => OpenEditor(new Models.SiteInfo { Name = SuggestName() }, isNew: true);

    private void OnEdit(object sender, RoutedEventArgs e)
    {
        if (_selected == null) { StatusText.Text = "请先选择站点"; return; }
        OpenEditor(Clone(_selected), isNew: false);
    }

    private void OnDelete(object sender, RoutedEventArgs e)
    {
        if (_selected == null) return;
        if (MessageBox.Show($"删除站点「{_selected.Name}」？（凭据管理器中的密码一并删除）",
                "确认", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
        try { CredentialStore.Delete(_selected.Name); } catch { }
        _sites.Remove(_selected);
        SiteStore.Save(_sites);
        _selected = null;
        ShowDetails();
        StatusText.Text = "已删除";
    }

    /// <summary>WinSCP 设置：指定 WinSCP.com 路径（支持绿色版/便携版），存入注册表。</summary>
    private void OnWinScpSettings(object sender, RoutedEventArgs e)
    {
        var current = ConnectionTester.FindWinScp();
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "选择 WinSCP.com（绿色版请选解压目录里的 WinSCP.com）",
            Filter = "WinSCP 命令行程序|WinSCP.com|所有文件|*.*",
            FileName = current ?? "WinSCP.com",
            CheckFileExists = true,
        };
        if (dlg.ShowDialog() == true)
        {
            try
            {
                Microsoft.Win32.Registry.CurrentUser
                    .CreateSubKey(@"Software\ExplorerRemoteFs")?
                    .SetValue("WinScpPath", dlg.FileName);
                StatusText.Text = $"WinSCP.com 已设置: {dlg.FileName}";
            }
            catch (Exception ex)
            {
                MessageBox.Show($"保存失败: {ex.Message}", "WinSCP 设置", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private async void OnTest(object sender, RoutedEventArgs e)
    {
        if (_selected == null) { StatusText.Text = "请先选择站点"; return; }
        TestOutput.Text = "正在测试连接…";
        StatusText.Text = $"测试中：{_selected.Name}";
        var result = await ConnectionTester.TestAsync(_selected);
        TestOutput.Text = result.Message;
        StatusText.Text = result.Ok ? "连接成功" : "连接失败";
    }

    // ---- editor -----------------------------------------------------------

    private void OpenEditor(Models.SiteInfo draft, bool isNew)
    {
        var dlg = new SiteEditWindow(draft) { Owner = this };
        if (dlg.ShowDialog() != true) return;

        try
        {
            // 密码：明文只在内存，写入凭据管理器（DPAPI）
            if (dlg.EnteredPassword != null)
                CredentialStore.Write(draft.Name, draft.Username, dlg.EnteredPassword);

            // 改名时迁移凭据
            string? oldName = isNew ? null : _selected?.Name;
            if (oldName != null && oldName != draft.Name && CredentialStore.Exists(oldName))
            {
                CredentialStore.TryRead(oldName, out var u, out var pwd);
                if (pwd.Length > 0 && !CredentialStore.Exists(draft.Name))
                    CredentialStore.Write(draft.Name, u.Length > 0 ? u : draft.Username, pwd);
                try { CredentialStore.Delete(oldName); } catch { }
            }

            if (isNew)
            {
                _sites.Add(draft);
                SiteList.SelectedItem = draft;
            }
            else if (_selected != null)
            {
                CopyInto(draft, _selected);
                SiteList.Items.Refresh();
            }

            SiteStore.Save(_sites);
            ShowDetails();
            StatusText.Text = "已保存";
        }
        catch (Exception ex)
        {
            MessageBox.Show("保存失败：" + ex.Message, "错误", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private static void CopyInto(Models.SiteInfo from, Models.SiteInfo to)
    {
        to.Name = from.Name; to.Type = from.Type; to.Host = from.Host;
        to.Port = from.Port; to.Username = from.Username; to.PrivateKeyPath = from.PrivateKeyPath;
        to.StartPath = from.StartPath;
    }

    private static Models.SiteInfo Clone(Models.SiteInfo s) => new()
    { Name = s.Name, Type = s.Type, Host = s.Host, Port = s.Port, Username = s.Username,
      Password = s.Password, PrivateKeyPath = s.PrivateKeyPath, StartPath = s.StartPath };

    private string SuggestName()
    {
        for (int i = 1; ; i++)
        {
            string n = $"server-{i}";
            if (!_sites.Any(s => s.Name == n)) return n;
        }
    }
}
