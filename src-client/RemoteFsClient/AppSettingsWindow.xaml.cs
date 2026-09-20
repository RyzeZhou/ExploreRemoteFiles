using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using RemoteFsClient.Services;

namespace RemoteFsClient;

/// <summary>应用设置：左侧导航 + 右侧具体页。功能会越加越多，所以按"一页一栏"组织。</summary>
public partial class AppSettingsWindow : Window
{
    public AppSettings Settings { get; }

    public AppSettingsWindow()
    {
        InitializeComponent();
        Settings = AppSettings.Load();
        foreach (var code in TranslationCatalog.GetExplorerLanguages())
        {
            if (!ExplorerLanguageBox.Items.Cast<ComboBoxItem>().Any(item => string.Equals(item.Tag as string, code, StringComparison.OrdinalIgnoreCase)))
                ExplorerLanguageBox.Items.Add(new ComboBoxItem { Tag = code, Content = code });
        }
        WinScpPathBox.Text = Settings.WinScpPath;
        MetadataCachePathBox.Text = Settings.MetadataCachePath;
        FileCachePathBox.Text = Settings.FileCachePath;
        EditorPathBox.Text = Settings.EditorPath;
        DownloadDirBox.Text = Settings.DownloadDir;
        ExplorerLanguageBox.SelectedValue = Settings.ExplorerLanguage;
        ServiceLanguageBox.SelectedValue = Settings.ServiceLanguage;
        DefaultViewBox.SelectedValue = Settings.DefaultViewMode;
        SizeFormatBox.SelectedValue = Settings.SizeFormat;
        TerminalBox.SelectedValue = AppSettings.NormalizeTerminal(Settings.Terminal);
        ApplyLanguage();
        RefreshAssocStatus();
        NavList.SelectedIndex = 0;   // 触发 OnNavChanged
    }

    private void ApplyLanguage()
    {
        Title = Ui.T("ApplicationSettingsTitle");
        NavGeneral.Content = Ui.IsEnglish ? "General" : "通用";
        NavCaches.Content = Ui.IsEnglish ? "Caches & editor" : "缓存与编辑器";
        NavDownload.Content = Ui.IsEnglish ? "Downloads" : "下载";
        NavAssoc.Content = Ui.IsEnglish ? "File association" : "文件关联";

        ExplorerLanguageLabel.Text = Ui.T("ExplorerLanguage"); ServiceLanguageLabel.Text = Ui.T("ServiceLanguage");
        DefaultViewLabel.Text = Ui.IsEnglish ? "Default view" : "默认视图";
        var viewNames = Ui.IsEnglish
            ? new[] { "Icons", "List", "Details", "Tiles", "Content" }
            : new[] { "图标", "列表", "详细信息", "平铺", "内容" };
        for (int i = 0; i < DefaultViewBox.Items.Count && i < viewNames.Length; i++)
            ((ComboBoxItem)DefaultViewBox.Items[i]).Content = viewNames[i];
        SizeFormatLabel.Text = Ui.IsEnglish ? "File size format" : "文件大小格式";
        var sizeNames = Ui.IsEnglish
            ? new[] { "Auto — same as Explorer (Windows formats it: 1.00 KB / 976 KB / 1.39 GB)",
                      "KB (1234 KB — whole KB, 1024-based)",
                      "SI (1.0 MB — 1000-based, like ls --si)",
                      "IEC (1.0 MiB — 1024-based, strict KiB/MiB)" }
            : new[] { "自动 —— 与资源管理器一致（交给 Windows 格式化：1.00 KB / 976 KB / 1.39 GB）",
                      "固定 KB（1234 KB，1024 进制，整 KB）",
                      "十进制（1.0 MB，1000 进制，同 ls --si / Nautilus）",
                      "IEC（1.0 MiB，1024 进制，严格 KiB/MiB）" };
        for (int i = 0; i < SizeFormatBox.Items.Count && i < sizeNames.Length; i++)
            ((ComboBoxItem)SizeFormatBox.Items[i]).Content = sizeNames[i];
        TerminalLabel.Text = Ui.T("TerminalGlobalLabel");
        var termNames = new[] { Ui.T("TerminalWt"), Ui.T("TerminalPwsh"), Ui.T("TerminalVsCode") };
        for (int i = 0; i < TerminalBox.Items.Count && i < termNames.Length; i++)
            ((ComboBoxItem)TerminalBox.Items[i]).Content = termNames[i];
        GeneralHintText.Text = Ui.T("TerminalGlobalHint") + Environment.NewLine + Environment.NewLine + Ui.T("RestartExplorerHint");

        WinScpPathLabel.Text = Ui.T("WinScpPath"); BrowseButton.Content = Ui.T("Browse");
        MetadataCachePathLabel.Text = Ui.T("MetadataCachePath"); FileCachePathLabel.Text = Ui.T("FileCachePath");
        BrowseMetadataCacheButton.Content = Ui.T("Browse"); BrowseFileCacheButton.Content = Ui.T("Browse");
        DefaultEditorLabel.Text = Ui.T("DefaultEditor"); BrowseEditorButton.Content = Ui.T("Browse");
        CachesHintText.Text = Ui.T("CacheDirectoryHint");

        DownloadDirLabel.Text = Ui.IsEnglish ? "Default download directory" : "默认下载目录";
        BrowseDownloadDirButton.Content = Ui.T("Browse");
        DownloadHintText.Text = Ui.IsEnglish
            ? "The context-menu 'Download' command saves straight here — no save dialog. If a file with the same name already exists, a \" (2)\" suffix is appended instead of overwriting."
            : "右键「下载」会直接保存到这里，不再弹保存对话框；同名文件自动加 “ (2)”，不会覆盖已有文件。";

        AssocHintText.Text = Ui.IsEnglish
            ? ".erfdl files are ERF transfer tickets. Double-clicking one starts the transfer into the folder where the ticket currently is. Re-register the association if it was lost (e.g. after reinstalling Explorer, another tool stole the extension, or you moved the program)."
            : ".erfdl 是易远传的「传输票据」：双击它就会把任务下载到**票据当前所在目录**。若关联丢失（重装资源管理器、被其它软件抢占、程序搬家等），点下面的按钮重新关联。";
        AssocReassocButton.Content = Ui.IsEnglish ? "Re-register .erfdl" : "重新关联 .erfdl";

        SaveButton.Content = Ui.T("Save"); CancelButton.Content = Ui.T("Cancel");
    }

    private void OnNavChanged(object sender, SelectionChangedEventArgs e)
    {
        if (SectionGeneral is null) return;   // XAML 还没加载完
        SectionGeneral.Visibility = NavList.SelectedIndex == 0 ? Visibility.Visible : Visibility.Collapsed;
        SectionCaches.Visibility = NavList.SelectedIndex == 1 ? Visibility.Visible : Visibility.Collapsed;
        SectionDownload.Visibility = NavList.SelectedIndex == 2 ? Visibility.Visible : Visibility.Collapsed;
        SectionAssoc.Visibility = NavList.SelectedIndex == 3 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnBrowse(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = Ui.IsEnglish ? "Select WinSCP.com (portable installs are supported)" : "选择 WinSCP.com（支持绿色版）",
            Filter = "WinSCP.com|WinSCP.com|All files|*.*",
            FileName = string.IsNullOrWhiteSpace(WinScpPathBox.Text) ? "WinSCP.com" : WinScpPathBox.Text,
            CheckFileExists = true,
        };
        if (dlg.ShowDialog(this) == true) WinScpPathBox.Text = dlg.FileName;
    }

    private void OnBrowseMetadataCache(object sender, RoutedEventArgs e) => BrowseFolder(MetadataCachePathBox, Ui.IsEnglish ? "Select metadata cache directory" : "选择元数据缓存目录");
    private void OnBrowseFileCache(object sender, RoutedEventArgs e) => BrowseFolder(FileCachePathBox, Ui.IsEnglish ? "Select file cache directory" : "选择文件缓存目录");
    private void OnBrowseDownloadDir(object sender, RoutedEventArgs e) => BrowseFolder(DownloadDirBox, Ui.IsEnglish ? "Select the default download directory" : "选择默认下载目录");

    private void OnBrowseEditor(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = Ui.IsEnglish ? "Select an editor executable" : "选择编辑器程序",
            Filter = Ui.IsEnglish ? "Programs|*.exe|All files|*.*" : "程序|*.exe|所有文件|*.*",
            FileName = EditorPathBox.Text.Trim(), CheckFileExists = true,
        };
        if (dlg.ShowDialog(this) == true) EditorPathBox.Text = dlg.FileName;
    }

    private void BrowseFolder(System.Windows.Controls.TextBox target, string description)
    {
        using var dlg = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = description,
            SelectedPath = target.Text.Trim(),
            UseDescriptionForTitle = true,
        };
        if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK) target.Text = dlg.SelectedPath;
    }

    // ── 文件关联 ────────────────────────────────────────────────────────────
    private const string TicketExt = ".erfdl";
    private const string TicketProgId = "ERF.TransferTicket";

    private void RefreshAssocStatus()
    {
        string? cmd = null;
        try
        {
            using var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey($@"Software\Classes\{TicketProgId}\shell\open\command");
            cmd = k?.GetValue("") as string;
        }
        catch { }
        AssocStatusText.Text = string.IsNullOrWhiteSpace(cmd)
            ? (Ui.IsEnglish ? "Status: not associated yet." : "当前状态：尚未关联。")
            : (Ui.IsEnglish ? $"Status: {cmd}" : $"当前状态：{cmd}");
    }

    private void OnReassociate(object sender, RoutedEventArgs e)
    {
        try
        {
            string exe = Environment.ProcessPath ?? "";
            if (string.IsNullOrWhiteSpace(exe))
                throw new InvalidOperationException(Ui.IsEnglish ? "Cannot determine the program path." : "无法确定程序路径。");
            using (var ext = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"Software\Classes\{TicketExt}"))
                ext.SetValue("", TicketProgId, Microsoft.Win32.RegistryValueKind.String);
            using (var prog = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"Software\Classes\{TicketProgId}"))
                prog.SetValue("", Ui.IsEnglish ? "ERF transfer ticket" : "易远传 传输票据", Microsoft.Win32.RegistryValueKind.String);
            using (var cmd = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"Software\Classes\{TicketProgId}\shell\open\command"))
                cmd.SetValue("", $"\"{exe}\" --open-ticket \"%1\"", Microsoft.Win32.RegistryValueKind.String);
            // 让资源管理器立刻重新读取关联（否则要等它自己刷新）
            SHChangeNotify(ShcneAssocChanged, ShcnfIdList, IntPtr.Zero, IntPtr.Zero);
            RefreshAssocStatus();
            System.Windows.MessageBox.Show(
                Ui.IsEnglish ? "The .erfdl association has been re-registered." : ".erfdl 关联已重新注册。",
                Ui.T("Settings"), MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception ex)
        {
            System.Windows.MessageBox.Show(
                (Ui.IsEnglish ? "Failed to register .erfdl: " : "注册 .erfdl 关联失败：") + ex.Message,
                Ui.T("Settings"), MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private const int ShcneAssocChanged = 0x08000000;
    private const uint ShcnfIdList = 0x0000;

    [DllImport("shell32.dll")]
    private static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);

    private void OnSave(object sender, RoutedEventArgs e)
    {
        Settings.WinScpPath = WinScpPathBox.Text.Trim();
        Settings.MetadataCachePath = MetadataCachePathBox.Text.Trim();
        Settings.FileCachePath = FileCachePathBox.Text.Trim();
        Settings.EditorPath = EditorPathBox.Text.Trim();
        Settings.DownloadDir = DownloadDirBox.Text.Trim();
        Settings.ExplorerLanguage = (ExplorerLanguageBox.SelectedItem as ComboBoxItem)?.Tag as string ?? "zh-CN";
        Settings.ServiceLanguage = (ServiceLanguageBox.SelectedItem as ComboBoxItem)?.Tag as string ?? "zh-CN";
        Settings.DefaultViewMode = (DefaultViewBox.SelectedItem as ComboBoxItem)?.Tag as string ?? "details";
        Settings.SizeFormat = (SizeFormatBox.SelectedItem as ComboBoxItem)?.Tag as string ?? "auto";
        Settings.Terminal = AppSettings.NormalizeTerminal((TerminalBox.SelectedItem as ComboBoxItem)?.Tag as string);
        try
        {
            Settings.Save();
            DialogResult = true;
        }
        catch (Exception ex)
        {
            System.Windows.MessageBox.Show((Ui.IsEnglish ? "Unable to save settings: " : "保存设置失败：") + ex.Message,
                Ui.IsEnglish ? "Settings" : "设置", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }
}
