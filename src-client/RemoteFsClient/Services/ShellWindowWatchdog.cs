using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace RemoteFsClient.Services;

/// <summary>
/// 跨进程的 shell 窗口看门狗（2026-09-19，针对 Win11「属性页锁住其他窗口」）。
///
/// 为什么要有它：扩展自己的探针（src-cpp 的 PropSheetProbe.h）跑在 explorer 进程**内部**，
/// 万一 explorer 的 UI 线程被卡到采样线程都排不上，或者扩展压根没被加载，
/// 日志就一片空白 —— 而那正是最需要证据的时候。
/// 这个看门狗跑在**常驻服务的独立进程**里，只做两件很便宜的事：
///
///   1. 枚举 explorer.exe 的顶层窗口，记录 <c>IsWindowEnabled</c> 的**变化**
///      → 区分"其他窗口是被 EnableWindow(FALSE) 禁用的"（模态行为 / 禁用错了对象）；
///   2. 对每个窗口做一次 <c>SendMessageTimeout(WM_NULL, SMTO_ABORTIFHUNG)</c>
///      → 区分"窗口是 hung 的"（UI 线程被阻塞；Win11 的多标签共用 UI 线程，
///         一处同步等远程就是一整排窗口失去响应）。
///
/// 这两种机制在用户眼里都是"窗口变灰、点不动"，但修法完全不同：
/// 前者是窗口所有权 / 禁用对象的问题，后者是**绝不能在 UI 线程上干重活**的问题。
/// 不把两者分开，就只能靠猜。
///
/// 常态零输出：只在状态**变化**（enabled 翻转 / hung 翻转 / 窗口出现消失）
/// 或响应时间异常（接近卡死）时写一行到 %TEMP%\rfs-shell-watch.log。
/// 设环境变量 ERF_PROBE_SHELLWATCH=0 可关闭。
/// </summary>
internal static class ShellWindowWatchdog
{
    private const uint WM_NULL = 0x0000;
    private const uint SMTO_BLOCK = 0x0001;
    private const uint SMTO_ABORTIFHUNG = 0x0002;
    private const uint ERROR_TIMEOUT = 1460;

    private const int ProbeTimeoutMs = 300;      // 单窗口探活上限
    private const int SlowWarnMs = 150;          // 超过这个响应时间就记一笔（"接近卡"）
    private const int MaxProbePerRound = 10;     // 每轮最多探这么多个窗口，免得自己拖慢 shell
    private static readonly TimeSpan Interval = TimeSpan.FromSeconds(2);

    private sealed class Snap
    {
        public bool Enabled;
        public bool Hung;
        public long RespMs;
        public string Cls = "";
        public string Title = "";
    }

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")]
    private static extern bool IsWindowEnabled(IntPtr hWnd);
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SendMessageTimeout(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam,
        uint fuFlags, uint uTimeout, out IntPtr lpdwResult);

    private static readonly EnumWindowsProc EnumProc = OnEnum;   // 保引用，别被 GC 掉
    private static readonly object Gate = new();
    private static readonly Dictionary<IntPtr, Snap> Prev = new();
    private static readonly List<(IntPtr Hwnd, uint Pid, string Cls, string Title)> Round = new();

    private static System.Threading.Timer? _timer;
    private static string _logPath = "";
    private static int _busy;
    private static HashSet<uint> _explorerPids = new();
    private static DateTime _pidsAt = DateTime.MinValue;

    public static void Start()
    {
        if (Environment.GetEnvironmentVariable("ERF_PROBE_SHELLWATCH") == "0") return;
        if (_timer != null) return;
        _logPath = Path.Combine(Path.GetTempPath(), "rfs-shell-watch.log");
        Log("watchdog start (interval=" + Interval.TotalSeconds + "s)");
        _timer = new System.Threading.Timer(_ => Tick(), null, TimeSpan.FromSeconds(3), Interval);
    }

    /// <summary>让别的组件（例如桥接服务）在关键动作前后主动要一份快照。</summary>
    public static void DumpNow(string tag) => Tick(tag);

    private static void Tick() => Tick(null);

    private static void Tick(string? tag)
    {
        // 上一轮还没跑完（explorer 卡着，探活没返回）就别再排一轮
        if (Interlocked.Exchange(ref _busy, 1) == 1) return;
        try
        {
            var pids = ExplorerPids();
            if (pids.Count == 0) return;

            lock (Gate) Round.Clear();
            EnumWindows(EnumProc, IntPtr.Zero);

            var now = new Dictionary<IntPtr, Snap>();
            int probed = 0;
            foreach (var w in Round)
            {
                if (!pids.Contains(w.Pid)) continue;
                var s = new Snap { Cls = w.Cls, Title = w.Title };
                s.Enabled = IsWindowEnabled(w.Hwnd);
                if (probed < MaxProbePerRound)
                {
                    probed++;
                    var sw = Stopwatch.StartNew();
                    IntPtr res;
                    bool ok = SendMessageTimeout(w.Hwnd, WM_NULL, IntPtr.Zero, IntPtr.Zero,
                                                 SMTO_BLOCK | SMTO_ABORTIFHUNG, ProbeTimeoutMs, out res);
                    sw.Stop();
                    s.RespMs = sw.ElapsedMilliseconds;
                    if (!ok && Marshal.GetLastWin32Error() == ERROR_TIMEOUT) s.Hung = true;
                }
                else
                {
                    s.RespMs = -1;    // 本轮没探（窗口太多，留给下一轮）
                }
                now[w.Hwnd] = s;
            }

            lock (Gate)
            {
                foreach (var kv in now)
                {
                    Prev.TryGetValue(kv.Key, out var old);
                    string who = Describe(kv.Key, kv.Value);
                    if (old == null)
                    {
                        if (tag != null || kv.Value.Hung || !kv.Value.Enabled)
                            Log($"{(tag ?? "appear")}: {who} enabled={B(kv.Value.Enabled)} hung={B(kv.Value.Hung)} resp={kv.Value.RespMs}ms");
                        continue;
                    }
                    if (old.Enabled != kv.Value.Enabled)
                        Log($"CHANGED enabled {B(old.Enabled)} -> {B(kv.Value.Enabled)}  <== 「变暗 / 恢复」就在这一刻 | {who}");
                    if (old.Hung != kv.Value.Hung)
                        Log($"CHANGED hung {B(old.Hung)} -> {B(kv.Value.Hung)}  <== UI 线程被阻塞 / 恢复 | {who}");
                    if (!kv.Value.Hung && kv.Value.RespMs >= SlowWarnMs && old.RespMs < SlowWarnMs)
                        Log($"SLOW resp={kv.Value.RespMs}ms (>= {SlowWarnMs}ms) | {who}");
                    if (tag != null)
                        Log($"{tag}: {who} enabled={B(kv.Value.Enabled)} hung={B(kv.Value.Hung)} resp={kv.Value.RespMs}ms");
                }
                foreach (var kv in Prev)
                    if (!now.ContainsKey(kv.Key))
                        Log($"gone: {Describe(kv.Key, kv.Value)}");
                Prev.Clear();
                foreach (var kv in now) Prev[kv.Key] = kv.Value;
            }
        }
        catch (Exception ex)
        {
            Log("watchdog error: " + ex.Message);
        }
        finally
        {
            Interlocked.Exchange(ref _busy, 0);
        }
    }

    /// <summary>输入法、工具提示、缩略图宿主这类窗口不是用户眼里的"资源管理器窗口"，
    /// 它们常年 enabled=0，记进来只会把真正有用的变化淹掉。</summary>
    private static readonly HashSet<string> IgnoredClasses = new(StringComparer.OrdinalIgnoreCase)
    {
        "IME", "MSCTFIME UI", "TooltipWindow", "SysShadow", "GDI+ Hook Window Class",
        "TaskListThumbnailWnd", "ForegroundStaging", "Windows.UI.Input.InputSite.WindowClass",
        "EdgeUiInputTopWndClass", "MSCTF.AsmListCache.FMPWin32", "Default IME",
    };

    private static bool OnEnum(IntPtr hWnd, IntPtr lParam)
    {
        // 只关心**用户看得见**的顶层窗口：隐藏窗口（输入法、提示气泡）与"变暗"
        // 这个症状无关，还会把日志刷满。
        if (!IsWindowVisible(hWnd)) return true;
        GetWindowThreadProcessId(hWnd, out uint pid);
        var cls = new StringBuilder(128);
        GetClassName(hWnd, cls, cls.Capacity);
        if (IgnoredClasses.Contains(cls.ToString())) return true;
        var title = new StringBuilder(256);
        GetWindowText(hWnd, title, title.Capacity);
        lock (Gate)
            Round.Add((hWnd, pid, cls.ToString(), title.ToString()));
        return true;
    }

    private static HashSet<uint> ExplorerPids()
    {
        if ((DateTime.UtcNow - _pidsAt).TotalSeconds < 10 && _explorerPids.Count > 0) return _explorerPids;
        var set = new HashSet<uint>();
        try
        {
            foreach (var p in Process.GetProcessesByName("explorer"))
            {
                set.Add((uint)p.Id);
                p.Dispose();
            }
        }
        catch { /* 拿不到就当没有，下一轮再试 */ }
        _explorerPids = set;
        _pidsAt = DateTime.UtcNow;
        return set;
    }

    private static string Describe(IntPtr h, Snap s)
    {
        string t = s.Title.Length > 48 ? s.Title[..48] + "…" : s.Title;
        return $"hwnd=0x{h.ToInt64():X} cls='{s.Cls}' title='{t}'";
    }

    private static string B(bool v) => v ? "1" : "0";

    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(_logPath,
                $"{DateTime.Now:HH:mm:ss.fff} {message}{Environment.NewLine}", Encoding.UTF8);
        }
        catch { /* 诊断日志绝不能影响功能 */ }
    }
}
