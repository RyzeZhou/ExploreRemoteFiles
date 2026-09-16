using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace RemoteFsClient.Services;

/// <summary>资源管理器里那个命名空间条目显示的名字（**只有它跟着语言变**）：
/// 中文「易远传」、英文「ERF sites」。其余窗口标题、托盘提示不受影响。
///
/// 名字来自注册表两处（都是我们自己的 CLSID，安装脚本写的）：
/// <list type="bullet">
/// <item><c>HKCU\...\Explorer\Desktop\NameSpace\{CLSID}</c> 默认值 —— 桌面命名空间条目的名字；</item>
/// <item><c>HKCU\Software\Classes\CLSID\{CLSID}</c> 默认值 —— 导航窗格里固定条目用的名字。</item>
/// </list>
/// 写法上不引入 MUI 资源（那要按语言分目录重编 DLL），而是**由常驻客户端按当前语言改写这两个值**，
/// 值真的变了才发 <c>SHChangeNotify(SHCNE_ASSOCCHANGED)</c> 让资源管理器刷新缓存。
/// </summary>
internal static class ShellNamespaceName
{
    private const string FolderClsid = "{C816CE0E-728C-4FC9-98E5-D0B35B384597}";
    private const string DesktopKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\" + FolderClsid;
    private const string ClsidKey = @"Software\Classes\CLSID\" + FolderClsid;

    private const uint SHCNE_ASSOCCHANGED = 0x08000000;
    private const uint SHCNF_IDLIST = 0x0000;

    [DllImport("shell32.dll")]
    private static extern void SHChangeNotify(uint wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);

    public const string ChineseName = "易远传";
    public const string EnglishName = "ERF sites";

    public static string Display(bool english) => english ? EnglishName : ChineseName;

    /// <summary>按语言同步注册表名字。返回是否真的改了（改了才通知 shell 刷新）。</summary>
    public static bool Apply(bool english)
    {
        string want = Display(english);
        bool desktop = WriteValue(DesktopKey, want);
        bool clsid = WriteValue(ClsidKey, want);
        Log($"apply english={english} want='{want}' desktopChanged={desktop} clsidChanged={clsid} now='{Current()}'");
        if (desktop || clsid)
        {
            try { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, IntPtr.Zero, IntPtr.Zero); }
            catch { /* 刷新失败不影响功能：下次登录/重启资源管理器时名字会是对的 */ }
        }
        return desktop || clsid;
    }

    /// <summary>落盘日志：改名这件事发生在常驻进程启动早期，出问题时得能查。</summary>
    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(Path.Combine(Path.GetTempPath(), "erf-namesync.log"),
                $"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
        }
        catch { }
    }

    private static bool WriteValue(string subKeyPath, string value)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(subKeyPath, writable: true);
            if (key is null) return false;                    // 还没安装：不创建，交给安装脚本
            if (key.GetValue(null) as string == value) return false;
            key.SetValue(null, value, RegistryValueKind.String);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>当前注册表里的名字（自检/诊断用：null = 键不存在）。</summary>
    public static string? Current()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(ClsidKey);
            return key?.GetValue(null) as string;
        }
        catch { return null; }
    }
}
