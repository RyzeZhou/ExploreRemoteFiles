using Microsoft.Win32;
using System.IO;

namespace RemoteFsClient.Services;

/// <summary>Per-user application settings shared by the tray manager and the Explorer extension.</summary>
public sealed class AppSettings
{
    private const string RegistryPath = @"Software\ExplorerRemoteFs";
    public string WinScpPath { get; set; } = "";
    public string ExplorerLanguage { get; set; } = "zh-CN";
    public string ServiceLanguage { get; set; } = "zh-CN";
    public string MetadataCachePath { get; set; } = DefaultMetadataCachePath;
    public string FileCachePath { get; set; } = DefaultFileCachePath;
    /// <summary>Explorer folder default view: icons | list | details | tiles | content.</summary>
    public string DefaultViewMode { get; set; } = "details";
    /// <summary>File-size column format: "auto" (Linux -h style) or "kb" (Windows style).</summary>
    public string SizeFormat { get; set; } = "auto";

    public static string DefaultMetadataCachePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ExplorerRemoteFs", "MetadataCache");
    public static string DefaultFileCachePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ExplorerRemoteFs", "FileCache");

    public static AppSettings Load()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RegistryPath);
            return new AppSettings
            {
                WinScpPath = key?.GetValue("WinScpPath") as string ?? "",
                ExplorerLanguage = NormalizeLanguage(key?.GetValue("ExplorerLanguage") as string),
                ServiceLanguage = NormalizeLanguage(key?.GetValue("ServiceLanguage") as string),
                MetadataCachePath = NormalizeDirectory(key?.GetValue("MetadataCachePath") as string, DefaultMetadataCachePath),
                FileCachePath = NormalizeDirectory(key?.GetValue("FileCachePath") as string, DefaultFileCachePath),
                DefaultViewMode = NormalizeViewMode(key?.GetValue("DefaultViewMode") as string),
                SizeFormat = NormalizeSizeFormat(key?.GetValue("SizeFormat") as string),
            };
        }
        catch { return new AppSettings(); }
    }

    public void Save()
    {
        using var key = Registry.CurrentUser.CreateSubKey(RegistryPath)
            ?? throw new InvalidOperationException("Unable to open the ExplorerRemoteFs settings registry key.");
        key.SetValue("WinScpPath", WinScpPath ?? "", RegistryValueKind.String);
        key.SetValue("ExplorerLanguage", NormalizeLanguage(ExplorerLanguage), RegistryValueKind.String);
        key.SetValue("ServiceLanguage", NormalizeLanguage(ServiceLanguage), RegistryValueKind.String);
        MetadataCachePath = NormalizeDirectory(MetadataCachePath, DefaultMetadataCachePath); Directory.CreateDirectory(MetadataCachePath);
        FileCachePath = NormalizeDirectory(FileCachePath, DefaultFileCachePath); Directory.CreateDirectory(FileCachePath);
        key.SetValue("MetadataCachePath", MetadataCachePath, RegistryValueKind.String);
        key.SetValue("FileCachePath", FileCachePath, RegistryValueKind.String);
        key.SetValue("DefaultViewMode", NormalizeViewMode(DefaultViewMode), RegistryValueKind.String);
        key.SetValue("SizeFormat", NormalizeSizeFormat(SizeFormat), RegistryValueKind.String);
    }

    public static string NormalizeDirectory(string? path, string fallback)
    {
        try { return string.IsNullOrWhiteSpace(path) ? fallback : Path.GetFullPath(path.Trim()); } catch { return fallback; }
    }

    /// <summary>icons | list | details | tiles | content; anything else falls back to details.</summary>
    public static string NormalizeViewMode(string? mode) => (mode ?? "").Trim().ToLowerInvariant() switch
    {
        "icons" => "icons",
        "list" => "list",
        "tiles" => "tiles",
        "content" => "content",
        _ => "details",
    };

    /// <summary>auto = human readable (1.2 MB); kb = Windows Explorer style (1234 KB).</summary>
    public static string NormalizeSizeFormat(string? format) =>
        string.Equals((format ?? "").Trim(), "kb", StringComparison.OrdinalIgnoreCase) ? "kb" : "auto";

    public static string NormalizeLanguage(string? language) =>
        string.Equals(language, "en-US", StringComparison.OrdinalIgnoreCase) ? "en-US" : "zh-CN";
}

public static class Ui
{
    private static string _language = "zh-CN";

    private static readonly IReadOnlyDictionary<string, (string Zh, string En)> Text =
        new Dictionary<string, (string, string)>
        {
            ["AppTitle"] = ("Explorer Remote FS - FTP 站点管理", "Explorer Remote FS - FTP Site Manager"),
            ["New"] = ("新建站点", "New site"), ["Edit"] = ("编辑", "Edit"), ["Delete"] = ("删除", "Delete"),
            ["Settings"] = ("设置", "Settings"), ["Test"] = ("测试连接", "Test connection"),
            ["SiteDetails"] = ("站点详情", "Site details"), ["SiteSettings"] = ("站点设置", "Site settings"),
            ["Name"] = ("名称", "Name"), ["Protocol"] = ("协议", "Protocol"), ["HostPort"] = ("主机:端口", "Host: port"),
            ["Username"] = ("用户名", "Username"), ["StartPath"] = ("起始路径", "Start path"), ["Password"] = ("密码", "Password"),
            ["WinScpSite"] = ("WinSCP 站点", "WinSCP site"), ["TestResult"] = ("连接测试结果", "Connection test result"),
            ["SiteSettingsHelp"] = ("站点的连接与传输选项在编辑窗口中管理。", "Manage this site's connection and transfer options in the editor."),
            ["FtpEncoding"] = ("FTP 文件名编码", "FTP file-name encoding"), ["ForceUtf8"] = ("强制 UTF-8（推荐）", "Force UTF-8 (recommended)"),
            ["EditSiteSettings"] = ("编辑站点设置", "Edit site settings"),
            ["ApplicationSettings"] = ("应用设置", "Application settings"), ["WinScpBackend"] = ("WinSCP 后端", "WinSCP backend"),
            ["WinScpPath"] = ("WinSCP.com 路径", "WinSCP.com path"), ["Browse"] = ("浏览...", "Browse..."),
            ["MetadataCachePath"] = ("元数据缓存目录", "Metadata cache directory"), ["FileCachePath"] = ("文件缓存目录", "File cache directory"),
            ["CacheDirectoryHint"] = ("元数据缓存保存目录列表和属性快照；文件缓存用于打开、编辑及跨站点复制的临时文件。保存后请重新打开 Explorer 窗口。", "Metadata cache stores directory listings and property snapshots; file cache stages Open, Edit, and cross-site copies. Reopen Explorer windows after saving."),
            ["ExplorerLanguage"] = ("Explorer 扩展显示语言", "Explorer extension language"),
            ["ServiceLanguage"] = ("服务程序界面语言", "Service program language"),
            ["RestartExplorerHint"] = ("Explorer 的菜单文字会在下次打开菜单时更新；如仍显示旧文字，请重启 Explorer。", "Explorer menu text updates when the menu is reopened; restart Explorer if old text remains."),
            ["Save"] = ("保存", "Save"), ["Cancel"] = ("取消", "Cancel"), ["Close"] = ("关闭", "Close"),
            ["Ready"] = ("就绪", "Ready"), ["SelectSite"] = ("请先选择站点", "Select a site first"),
            ["Testing"] = ("正在测试连接…", "Testing connection…"), ["TestSuccess"] = ("连接成功", "Connection succeeded"), ["TestFailed"] = ("连接失败", "Connection failed"),
            ["Saved"] = ("已保存", "Saved"), ["Deleted"] = ("已删除", "Deleted"),
            ["PasswordSaved"] = ("已保存（Windows 凭据管理器）", "Saved (Windows Credential Manager)"), ["PasswordMissing"] = ("未保存", "Not saved"),
            ["SharedYes"] = ("是（与 WinSCP 同名站点）", "Yes (same name as a WinSCP site)"), ["SharedNo"] = ("否", "No"),
            ["Confirm"] = ("确认", "Confirm"), ["DeletePrompt"] = ("删除站点「{0}」？（凭据管理器中的密码一并删除）", "Delete site '{0}'? Its stored password will also be removed."),
            ["SettingsSaved"] = ("应用设置已保存", "Application settings saved"),
            ["TrayManage"] = ("FTP 站点管理...", "FTP site manager..."), ["Exit"] = ("退出", "Exit"),
        };

    public static bool IsEnglish => string.Equals(_language, "en-US", StringComparison.OrdinalIgnoreCase);
    public static void SetLanguage(string? language) => _language = AppSettings.NormalizeLanguage(language);
    public static string T(string key) => Text.TryGetValue(key, out var value) ? (IsEnglish ? value.En : value.Zh) : key;
}