using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Controls;
using RemoteFsClient.Services;

namespace RemoteFsClient;

public partial class MainWindow : Window
{
    private readonly ObservableCollection<Models.SiteInfo> _sites = new();
    private Models.SiteInfo? _selected;
    private ObservableCollection<TransferTask>? _tasks;
    private TransferTaskService? _transferService;
    private System.Windows.Threading.DispatcherTimer? _transferSummaryTimer;

    public MainWindow()
    {
        InitializeComponent();
        SiteList.ItemsSource = _sites;
        ApplyLanguage();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (_sites.Count == 0)
            foreach (var s in SiteStore.Load()) _sites.Add(s);
        if (_sites.Count > 0) SiteList.SelectedIndex = 0;
        StatusText.Text = $"{Ui.T("Ready")} · {SiteStore.FilePathForDisplay}";
    }

    internal void ApplyLanguage()
    {
        Title = Ui.T("AppTitle");
        NewButton.Content = Ui.T("New"); EditButton.Content = Ui.T("Edit"); DeleteButton.Content = Ui.T("Delete");
        TestButton.Content = Ui.T("Test"); SettingsButton.Content = Ui.T("Settings");
        DetailsTab.Header = Ui.T("SiteDetails"); SiteSettingsTab.Header = Ui.T("SiteSettings");
        if (TransferTab != null)
        {
            TransferTab.Header = Ui.IsEnglish ? "Transfers" : "传输队列";
            TColServer.Header = Ui.IsEnglish ? "Server" : "服务器";
            TColDirection.Header = Ui.IsEnglish ? "Direction" : "方向";
            TColFile.Header = Ui.IsEnglish ? "File" : "文件";
            TColCurFile.Header = Ui.IsEnglish ? "Current file" : "当前文件";
            TColProgress.Header = Ui.IsEnglish ? "Progress" : "进度";
            TColDone.Header = Ui.IsEnglish ? "Transferred" : "已传输";
            TColSpeed.Header = Ui.IsEnglish ? "Speed" : "速度";
            TColStatus.Header = Ui.IsEnglish ? "Status" : "状态";
            TransferClearButton.Content = Ui.IsEnglish ? "Clear finished" : "清除已完成";
            TColActions.Header = Ui.IsEnglish ? "Actions" : "操作";
        }
        NameLabel.Text = Ui.T("Name"); ProtocolLabel.Text = Ui.T("Protocol"); HostPortLabel.Text = Ui.T("HostPort");
        UsernameLabel.Text = Ui.T("Username"); StartPathLabel.Text = Ui.T("StartPath"); PasswordLabel.Text = Ui.T("Password");
        WinScpSiteLabel.Text = Ui.T("WinScpSite"); TestResultGroup.Header = Ui.T("TestResult");
        SiteSettingsHelpText.Text = Ui.T("SiteSettingsHelp"); FtpEncodingLabel.Text = Ui.T("FtpEncoding");
        PrivateKeyLabel.Text = Ui.IsEnglish ? "Private key" : "私钥文件";
        EditSiteSettingsButton.Content = Ui.T("EditSiteSettings");
        ShowDetails();
    }

    /// <summary>Binds the transfer queue (owned by the resident service) to the
    /// Transfers tab. Called once from App at startup.</summary>
    internal void AttachTasks(TransferTaskService service)
    {
        _transferService = service;
        ObservableCollection<TransferTask> tasks = service.Tasks;
        _tasks = tasks;
        TransferList.ItemsSource = tasks;
        _transferSummaryTimer = new System.Windows.Threading.DispatcherTimer
        {
            Interval = TimeSpan.FromSeconds(1),
        };
        _transferSummaryTimer.Tick += (_, _) => UpdateTransferSummary();
        _transferSummaryTimer.Start();
        UpdateTransferSummary();
    }

    /// <summary>Brings the Transfers tab to the front (tray "Transfer queue").</summary>
    internal void SelectTransferTab()
    {
        TransferTab.IsSelected = true;
        UpdateTransferSummary();
    }

    private void OnPauseTask(object sender, RoutedEventArgs e)
    {
        if ((sender as System.Windows.Controls.Button)?.Tag is TransferTask task) _transferService?.TogglePause(task);
        UpdateTransferSummary();
    }

    private void OnCancelTask(object sender, RoutedEventArgs e)
    {
        if ((sender as System.Windows.Controls.Button)?.Tag is TransferTask task) _transferService?.Cancel(task);
        UpdateTransferSummary();
    }

    private void OnClearTransfers(object sender, RoutedEventArgs e)
    {
        if (_tasks is null) return;
        for (int i = _tasks.Count - 1; i >= 0; i--)
            if (_tasks[i].IsFinished) _tasks.RemoveAt(i);
        UpdateTransferSummary();
    }

    private void UpdateTransferSummary()
    {
        if (_tasks is null || TransferSummary is null) return;
        int running = 0, done = 0, failed = 0;
        foreach (TransferTask t in _tasks)
        {
            if (!t.IsFinished) running++;
            else if (t.IsFailed) failed++;
            else done++;
        }
        TransferSummary.Text = Ui.IsEnglish
            ? $"{running} in progress · {done} completed · {failed} failed"
            : $"{running} 个进行中 · {done} 个已完成 · {failed} 个失败";
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
            DPwd.Text = DShared.Text = DFtpEncoding.Text = DPrivateKey.Text = "";
            return;
        }
        DName.Text = s.Name;
        DType.Text = s.Type;
        DHost.Text = $"{s.Host}:{s.EffectivePort}";
        DUser.Text = s.Username;
        DPath.Text = s.StartPath;
        DPwd.Text = CredentialStore.Exists(s.Name) ? Ui.T("PasswordSaved") : Ui.T("PasswordMissing");
        DShared.Text = RegistryShared(s.Name) ? Ui.T("SharedYes") : Ui.T("SharedNo");
        DFtpEncoding.Text = s.Type is "ftp" or "ftps" ? (s.FtpUseUtf8 ? Ui.T("ForceUtf8") : (Ui.IsEnglish ? "Server default" : "服务器默认编码")) : "—";
        DPrivateKey.Text = string.IsNullOrWhiteSpace(s.PrivateKeyPath) ? "—" : s.PrivateKeyPath;
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
        if (_selected == null) { StatusText.Text = Ui.T("SelectSite"); return; }
        OpenEditor(Clone(_selected), isNew: false);
    }

    private void OnDelete(object sender, RoutedEventArgs e)
    {
        if (_selected == null) return;
        if (MessageBox.Show(string.Format(Ui.T("DeletePrompt"), _selected.Name), Ui.T("Confirm"),
                MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
        try { CredentialStore.Delete(_selected.Name); } catch { }
        _sites.Remove(_selected);
        SiteStore.Save(_sites);
        _selected = null;
        ShowDetails();
        StatusText.Text = Ui.T("Deleted");
    }

    private void OnSettings(object sender, RoutedEventArgs e)
    {
        var dlg = new AppSettingsWindow { Owner = this };
        if (dlg.ShowDialog() != true) return;
        Ui.SetLanguage(dlg.Settings.ServiceLanguage);
        ApplyLanguage();
        (System.Windows.Application.Current as App)?.RefreshLocalizedShell();
        StatusText.Text = Ui.T("SettingsSaved");
    }

    private async void OnTest(object sender, RoutedEventArgs e)
    {
        if (_selected == null) { StatusText.Text = Ui.T("SelectSite"); return; }
        TestOutput.Text = Ui.T("Testing");
        StatusText.Text = $"{Ui.T("Testing")} {_selected.Name}";
        var result = await ConnectionTester.TestAsync(_selected);
        TestOutput.Text = result.Message;
        StatusText.Text = result.Ok ? Ui.T("TestSuccess") : Ui.T("TestFailed");
    }

    private void OpenEditor(Models.SiteInfo draft, bool isNew)
    {
        var dlg = new SiteEditWindow(draft) { Owner = this };
        if (dlg.ShowDialog() != true) return;

        try
        {
            if (dlg.EnteredPassword != null)
                CredentialStore.Write(draft.Name, draft.Username, dlg.EnteredPassword);

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
            StatusText.Text = Ui.T("Saved");
        }
        catch (Exception ex)
        {
            MessageBox.Show((Ui.IsEnglish ? "Save failed: " : "保存失败：") + ex.Message,
                Ui.IsEnglish ? "Error" : "错误", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private static void CopyInto(Models.SiteInfo from, Models.SiteInfo to)
    {
        to.Name = from.Name; to.Type = from.Type; to.Host = from.Host;
        to.Port = from.Port; to.Username = from.Username; to.PrivateKeyPath = from.PrivateKeyPath;
        to.StartPath = from.StartPath; to.FtpUseUtf8 = from.FtpUseUtf8;
        to.Terminal = from.Terminal; to.SshHostAlias = from.SshHostAlias;
    }

    private static Models.SiteInfo Clone(Models.SiteInfo s) => new()
    { Name = s.Name, Type = s.Type, Host = s.Host, Port = s.Port, Username = s.Username,
      Password = s.Password, PrivateKeyPath = s.PrivateKeyPath, StartPath = s.StartPath, FtpUseUtf8 = s.FtpUseUtf8,
      Terminal = s.Terminal, SshHostAlias = s.SshHostAlias };

    private string SuggestName()
    {
        for (int i = 1; ; i++)
        {
            string n = $"server-{i}";
            if (!_sites.Any(s => s.Name == n)) return n;
        }
    }
}