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
    /// <summary>Executable used for remote Edit/New file. Empty means Notepad.</summary>
    public string EditorPath { get; set; } = "notepad.exe";
    /// <summary>Explorer folder default view: icons | list | details | tiles | content.</summary>
    public string DefaultViewMode { get; set; } = "details";
    /// <summary>File-size column format: "auto" (Linux -h style) or "kb" (Windows style).</summary>
    public string SizeFormat { get; set; } = "auto";

    /// <summary>
    /// Default terminal program for the Explorer "Open terminal here" command:
    /// wt | powershell | vscode. Sites may override this individually (connections.json: Terminal).
    /// Stored in HKCU\Software\ExplorerRemoteFs\Terminal — the same value the shell extension reads.
    /// </summary>
    public string Terminal { get; set; } = "wt";

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
                EditorPath = NormalizeEditorPath(key?.GetValue("EditorPath") as string),
                DefaultViewMode = NormalizeViewMode(key?.GetValue("DefaultViewMode") as string),
                SizeFormat = NormalizeSizeFormat(key?.GetValue("SizeFormat") as string),
                Terminal = NormalizeTerminal(key?.GetValue("Terminal") as string),
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
        EditorPath = NormalizeEditorPath(EditorPath);
        key.SetValue("EditorPath", EditorPath, RegistryValueKind.String);
        key.SetValue("DefaultViewMode", NormalizeViewMode(DefaultViewMode), RegistryValueKind.String);
        key.SetValue("SizeFormat", NormalizeSizeFormat(SizeFormat), RegistryValueKind.String);
        key.SetValue("Terminal", NormalizeTerminal(Terminal), RegistryValueKind.String);
    }

    public static string NormalizeDirectory(string? path, string fallback)
    {
        try { return string.IsNullOrWhiteSpace(path) ? fallback : Path.GetFullPath(path.Trim()); } catch { return fallback; }
    }

    public static string NormalizeEditorPath(string? path) =>
        string.IsNullOrWhiteSpace(path) ? "notepad.exe" : path.Trim();

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

    /// <summary>wt (Windows Terminal) | powershell (console) | vscode (Remote-SSH).</summary>
    public static string NormalizeTerminal(string? value) => (value ?? "").Trim().ToLowerInvariant() switch
    {
        "powershell" or "pwsh" => "powershell",
        "vscode" or "code" => "vscode",
        _ => "wt",
    };

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
            ["DefaultEditor"] = ("默认编辑器", "Default editor"),
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

            // ── 「终端配置」标签页（站点编辑） ──────────────────────────────
            ["SiteTab"] = ("站点", "Site"),
            ["TerminalTab"] = ("终端配置", "Terminal"),
            ["TerminalProgram"] = ("默认终端程序", "Default terminal program"),
            ["TerminalFollowGlobal"] = ("跟随全局设置（{0}）", "Follow global setting ({0})"),
            ["TerminalWt"] = ("Windows Terminal", "Windows Terminal"),
            ["TerminalPwsh"] = ("PowerShell 控制台", "PowerShell console"),
            ["TerminalVsCode"] = ("VS Code（Remote-SSH）", "VS Code (Remote-SSH)"),
            ["TerminalGlobalLabel"] = ("全局默认终端", "Global default terminal"),
            ["TerminalGlobalHint"] = ("站点未单独指定时使用；站点的「终端配置」标签页可逐个覆盖。", "Used when a site does not override it; override per site on the Terminal tab."),
            ["TerminalTabHint"] = ("右键站点或目录 →「在此打开终端」时使用的终端程序。FTP 站点没有 shell 通道，不提供此配置。", "Terminal program used by the context-menu command 'Open terminal here'. FTP sites have no shell channel and are not configurable here."),
            ["SshBinding"] = ("绑定现有 SSH 连接", "Bind an existing SSH connection"),
            ["SshBindingAuto"] = ("自动匹配（按主机 + 端口 + 用户名）", "Automatic (match host + port + user)"),
            ["SshBindingHint"] = ("只列出与本站点主机、端口、用户名完全一致的 SSH 配置，避免登进错误的机器或账号；没有匹配项时会自动新建受管配置。", "Only SSH configs whose host, port, and user match this site are listed, so you never sign in to the wrong machine or account. When nothing matches, a managed entry is created automatically."),
            ["SshBindingExcluded"] = ("已排除 {0} 项主机相同但端口/用户名不同的配置：{1}", "Excluded {0} config(s) with the same host but a different port or user: {1}"),
            ["SshBindingNoConfig"] = ("未找到 %USERPROFILE%\\.ssh\\config，或其中没有可用的主机配置。", "No %USERPROFILE%\\.ssh\\config found, or it contains no usable host entries."),
            ["SshBindingSelected"] = ("已绑定：{0}", "Bound: {0}"),
            ["Refresh"] = ("刷新", "Refresh"),
            ["TerminalNotSshCapable"] = ("仅 SFTP（SSH）站点可配置终端。", "Terminal options apply to SFTP (SSH) sites only."),

            // ── 「列显示」标签页（列顺序自定义） ────────────────────────────
            ["ColumnTab"] = ("列显示", "Columns"),
            ["ColumnHint"] = ("调整该站点在资源管理器里的默认列顺序。注意：资源管理器会按文件夹记住你自己拖过的顺序与列宽，这里的设置只影响**新建或重置后的视图**；最前面的「站点选择」页列固定，不受影响。", "Sets the default column order for this site in Explorer. Explorer remembers the order and widths you drag per folder, so this only affects new or reset views; the site-picker columns are fixed."),
            ["ColumnMoveUp"] = ("上移", "Move up"),
            ["ColumnMoveDown"] = ("下移", "Move down"),
            ["ColumnReset"] = ("恢复默认顺序", "Reset to default"),
            ["ColName"] = ("名称", "Name"),
            ["ColType"] = ("类型", "Type"),
            ["ColSize"] = ("大小", "Size"),
            ["ColModified"] = ("修改时间", "Date modified"),
            ["ColPermissions"] = ("权限", "Permissions"),
            ["ColOwner"] = ("所有者", "Owner"),
            ["ColOwnerId"] = ("所有者 ID（UID）", "Owner ID (UID)"),
            ["ColGroup"] = ("组", "Group"),
            ["ColGroupId"] = ("组 ID（GID）", "Group ID (GID)"),
        };

    public static bool IsEnglish => string.Equals(_language, "en-US", StringComparison.OrdinalIgnoreCase);
    public static void SetLanguage(string? language) => _language = AppSettings.NormalizeLanguage(language);
    public static string T(string key) => Text.TryGetValue(key, out var value) ? (IsEnglish ? value.En : value.Zh) : key;
}
