using System.Windows;
using System.Windows.Controls;
using RemoteFsClient.Services;

namespace RemoteFsClient;

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
        ExplorerLanguageBox.SelectedValue = Settings.ExplorerLanguage;
        ServiceLanguageBox.SelectedValue = Settings.ServiceLanguage;
        DefaultViewBox.SelectedValue = Settings.DefaultViewMode;
        ApplyLanguage();
    }

    private void ApplyLanguage()
    {
        Title = Ui.T("ApplicationSettings");
        WinScpPathLabel.Text = Ui.T("WinScpPath"); BrowseButton.Content = Ui.T("Browse");
        MetadataCachePathLabel.Text = Ui.T("MetadataCachePath"); FileCachePathLabel.Text = Ui.T("FileCachePath");
        BrowseMetadataCacheButton.Content = Ui.T("Browse"); BrowseFileCacheButton.Content = Ui.T("Browse");
        ExplorerLanguageLabel.Text = Ui.T("ExplorerLanguage"); ServiceLanguageLabel.Text = Ui.T("ServiceLanguage");
        DefaultViewLabel.Text = Ui.IsEnglish ? "Default view" : "默认视图";
        var viewNames = Ui.IsEnglish
            ? new[] { "Icons", "List", "Details", "Tiles", "Content" }
            : new[] { "图标", "列表", "详细信息", "平铺", "内容" };
        for (int i = 0; i < DefaultViewBox.Items.Count && i < viewNames.Length; i++)
            ((ComboBoxItem)DefaultViewBox.Items[i]).Content = viewNames[i];
        HintText.Text = Ui.T("CacheDirectoryHint") + Environment.NewLine + Environment.NewLine + Ui.T("RestartExplorerHint"); SaveButton.Content = Ui.T("Save"); CancelButton.Content = Ui.T("Cancel");
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

    private void OnBrowseMetadataCache(object sender, RoutedEventArgs e) => BrowseFolder(MetadataCachePathBox);
    private void OnBrowseFileCache(object sender, RoutedEventArgs e) => BrowseFolder(FileCachePathBox);

    private void BrowseFolder(System.Windows.Controls.TextBox target)
    {
        using var dlg = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = Ui.IsEnglish ? "Select cache directory" : "选择缓存目录",
            SelectedPath = target.Text.Trim(),
            UseDescriptionForTitle = true,
        };
        if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK) target.Text = dlg.SelectedPath;
    }
    private void OnSave(object sender, RoutedEventArgs e)
    {
        Settings.WinScpPath = WinScpPathBox.Text.Trim();
        Settings.MetadataCachePath = MetadataCachePathBox.Text.Trim();
        Settings.FileCachePath = FileCachePathBox.Text.Trim();
        Settings.ExplorerLanguage = (ExplorerLanguageBox.SelectedItem as ComboBoxItem)?.Tag as string ?? "zh-CN";
        Settings.ServiceLanguage = (ServiceLanguageBox.SelectedItem as ComboBoxItem)?.Tag as string ?? "zh-CN";
        Settings.DefaultViewMode = (DefaultViewBox.SelectedItem as ComboBoxItem)?.Tag as string ?? "details";
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