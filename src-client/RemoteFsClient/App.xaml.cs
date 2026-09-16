using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Threading;
using WinForms = System.Windows.Forms;
using Drawing = System.Drawing;
using RemoteFsClient.Services;

namespace RemoteFsClient;

/// <summary>用户会话内的常驻控制中心：通知区入口、站点管理器，以及 Shell 本地桥接服务。</summary>
public partial class App : System.Windows.Application
{
    private const string ErfScheme = "erf:";
    private const string ExplorerRemoteFsClsid = "{C816CE0E-728C-4FC9-98E5-D0B35B384597}";
    private static readonly Guid ShellWindowsClsid = new("9BA05972-F6A8-11CF-A442-00A0C90A8F39");
    private const string SingleInstanceName = @"Local\ExplorerRemoteFs.Client";
    private const string ShowManagerEventName = @"Local\ExplorerRemoteFs.ShowManager";
    private const string ShowTransfersEventName = @"Local\ExplorerRemoteFs.ShowTransfers";
    private Mutex? _singleInstance;
    private EventWaitHandle? _showManagerEvent;
    private EventWaitHandle? _showTransfersEvent;
    private CancellationTokenSource? _shutdown;
    private WinForms.NotifyIcon? _trayIcon;
    private WinForms.ContextMenuStrip? _trayMenu;
    private MainWindow? _manager;
    private RemoteBridgeService? _bridge;
    private TransferTaskService? _transfers;
    private RemoteOperationQueueService? _operationQueue;
    private readonly RemoteStatusService _status = new();
    private bool _isExiting;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        Ui.SetLanguage(AppSettings.Load().ServiceLanguage);
        ShutdownMode = ShutdownMode.OnExplicitShutdown;

        // 常驻服务必须有兜底：一个 WPF 异常（例如 XAML 绑定附加失败）默认会直接
        // 终止进程 —— 实测过一次（进度条绑定到只读属性 → XamlParseException →
        // 整个托盘服务连同桥接管道一起消失，资源管理器那边只看到连接被断开）。
        // 这里记录并继续运行；真正"无法继续"的错误仍会通过 ErfLog 留下证据。
        DispatcherUnhandledException += (_, args) =>
        {
            ErfLog($"Unhandled UI exception: {args.Exception}");
            args.Handled = true;
        };

        // 自检开关：不建任何连接、不显示窗口，只把操作队列窗口的汇总/条目状态
        // 算一遍并打印（见 QueueSelfTest）。给"汇总明细算得对不对"提供后端证据，
        // 不必靠肉眼看窗口。必须在单实例/托盘之前返回。
        if (e.Args.Any(a => string.Equals(a, "--queue-selftest", StringComparison.OrdinalIgnoreCase)))
        {
            Shutdown(QueueSelfTest.Run());
            return;
        }

        // 图标生成器：导出静态 .ico（默认全绿，供资源管理器 / 将来的安装程序用）
        // 与 3×3 状态对照图，让图标设计可复现、可复核。
        // 用法：RemoteFsClient.exe --make-icon <输出目录>
        int iconArg = Array.FindIndex(e.Args, a => string.Equals(a, "--make-icon", StringComparison.OrdinalIgnoreCase));
        if (iconArg >= 0)
        {
            string outDir = e.Args.Length > iconArg + 1 ? e.Args[iconArg + 1] : ".";
            Shutdown(IconTool.MakeIcons(outDir));
            return;
        }
        var erfAddress = GetArgumentValue(e.Args, "--open-erf");
        ErfNavigationRequest? pendingErfNavigation = null;
        if (erfAddress is not null)
        {
            var sourceWindow = GetAncestor(GetForegroundWindow(), GaRoot);
            pendingErfNavigation = new ErfNavigationRequest(erfAddress, sourceWindow.ToInt64());
            ErfLog($"protocol entry invoked; address='{erfAddress}'; source=0x{sourceWindow.ToInt64():X}");

            // The normal path: hand work to the long-lived service and leave.
            // This process is only a URL-protocol shim, never a second engine.
            var forwardResult = TryForwardErfNavigation(pendingErfNavigation.Value, attempts: 1, out var forwardError);
            if (forwardResult == ErfForwardResult.Queued)
            {
                Shutdown();
                return;
            }
            if (forwardResult == ErfForwardResult.Rejected)
            {
                MessageBox.Show(forwardError ?? "The ERF navigation request was rejected.", "Explorer Remote FS", MessageBoxButton.OK, MessageBoxImage.Warning);
                Shutdown();
                return;
            }
        }
        bool background = pendingErfNavigation is not null || e.Args.Any(a => string.Equals(a, "--background", StringComparison.OrdinalIgnoreCase));
        bool showRequested = e.Args.Any(a => string.Equals(a, "--show", StringComparison.OrdinalIgnoreCase));
        bool transfersRequested = e.Args.Any(a => string.Equals(a, "--transfers", StringComparison.OrdinalIgnoreCase));

        _singleInstance = new Mutex(true, SingleInstanceName, out bool isFirstInstance);
        if (!isFirstInstance)
        {
            if (pendingErfNavigation is not null)
            {
                // The resident process may have the mutex before its pipe
                // listeners are ready.  Briefly retry rather than losing the
                // first ERF request during service startup.
                var forwardResult = TryForwardErfNavigation(pendingErfNavigation.Value, attempts: 8, out var forwardError);
                if (forwardResult != ErfForwardResult.Queued)
                {
                    ErfLog("resident service did not accept ERF navigation request");
                    MessageBox.Show(
                        forwardResult == ErfForwardResult.Rejected
                            ? forwardError ?? "The ERF navigation request was rejected."
                            : "The Explorer Remote FS service is starting but did not accept the ERF navigation request.",
                        "Explorer Remote FS", MessageBoxButton.OK, MessageBoxImage.Warning);
                }
            }
            else if (transfersRequested)
            {
                try { EventWaitHandle.OpenExisting(ShowTransfersEventName).Set(); }
                catch { }
            }
            else if (showRequested || !background)
            {
                try { EventWaitHandle.OpenExisting(ShowManagerEventName).Set(); }
                catch { }
            }
            Shutdown();
            return;
        }

        _shutdown = new CancellationTokenSource();
        _showManagerEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowManagerEventName, out _);
        _showTransfersEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowTransfersEventName, out _);
        _transfers = new TransferTaskService(Dispatcher);
        _transfers.JobStarted += OnTransferJobStarted;
        _transfers.AllFinished += OnAllTransfersFinished;
        _transfers.Start();
        _operationQueue = new RemoteOperationQueueService(Dispatcher);

        _manager = new MainWindow { Icon = _status.WindowIcon(48) };
        _manager.AttachTasks(_transfers);
        _manager.Closing += OnManagerClosing;
        MainWindow = _manager;
        CreateTrayIcon();
        _bridge = new RemoteBridgeService(QueueErfNavigationAsync, _operationQueue, _status);
        _bridge.Start();
        ListenForShowRequests();
        if (pendingErfNavigation is not null)
        {
            // The first ERF request also started this resident process.  Send
            // it through our own pipe so it receives exactly the same cache
            // preflight and validation as later protocol requests.
            var firstRequest = pendingErfNavigation.Value;
            _ = Task.Run(() =>
            {
                var forwardResult = TryForwardErfNavigation(firstRequest, attempts: 8, out var forwardError);
                if (forwardResult == ErfForwardResult.Queued) return;
                ErfLog("first resident ERF request was not accepted");
                Dispatcher.BeginInvoke(() => MessageBox.Show(
                    forwardResult == ErfForwardResult.Rejected
                        ? forwardError ?? "The ERF navigation request was rejected."
                        : "The Explorer Remote FS service did not accept the ERF navigation request.",
                    "Explorer Remote FS", MessageBoxButton.OK, MessageBoxImage.Warning));
            });
        }
        if (transfersRequested) ShowTransfers();
        else if (!background || showRequested) ShowManager();
    }

    private static string? GetArgumentValue(IReadOnlyList<string> args, string option)
    {
        for (var i = 0; i + 1 < args.Count; i++)
        {
            if (string.Equals(args[i], option, StringComparison.OrdinalIgnoreCase)) return args[i + 1];
        }
        return null;
    }

    private Task QueueErfNavigationAsync(ErfNavigationRequest request)
    {
        ErfLog($"resident accepted ERF navigation; address='{request.Address}'; source=0x{request.SourceWindowHandle:X}");
        Dispatcher.BeginInvoke(() => OpenErfAddress(request.Address, new IntPtr(request.SourceWindowHandle)));
        return Task.CompletedTask;
    }

    private static ErfForwardResult TryForwardErfNavigation(ErfNavigationRequest request, int attempts, out string? error)
    {
        error = null;
        for (var attempt = 0; attempt < attempts; attempt++)
        {
            try
            {
                using var pipe = new NamedPipeClientStream(".", RemoteBridgeService.PipeName, PipeDirection.InOut, PipeOptions.None);
                pipe.Connect(250);
                using var writer = new StreamWriter(pipe, new UTF8Encoding(false), 4096, leaveOpen: true) { AutoFlush = true };
                using var reader = new StreamReader(pipe, new UTF8Encoding(false), false, 4096, leaveOpen: true);
                writer.WriteLine("ERF-NAVIGATE");
                writer.WriteLine(request.SourceWindowHandle.ToString(System.Globalization.CultureInfo.InvariantCulture));
                writer.WriteLine(request.Address);
                var response = reader.ReadLine();
                if (string.Equals(response, "QUEUED", StringComparison.Ordinal))
                {
                    ErfLog($"ERF request forwarded to resident service on attempt {attempt + 1}");
                    return ErfForwardResult.Queued;
                }
                if (!string.IsNullOrWhiteSpace(response) && response.StartsWith("FAIL:", StringComparison.Ordinal))
                {
                    error = response[5..].Trim();
                    ErfLog($"ERF request rejected by resident service; {error}");
                    return ErfForwardResult.Rejected;
                }
            }
            catch (Exception ex)
            {
                ErfLog($"ERF handoff attempt {attempt + 1} failed; {ex.Message}");
            }

            if (attempt + 1 < attempts) Thread.Sleep(125);
        }
        return ErfForwardResult.Unavailable;
    }

    private enum ErfForwardResult { Queued, Unavailable, Rejected }

    private static void OpenErfAddress(string address, IntPtr sourceWindow)
    {
        if (!TryBuildErfTarget(address, out var target))
        {
            ErfLog($"invalid address; address='{address}'");
            MessageBox.Show(
                "Invalid ERF address. Expected erf:<site>:/absolute/unix/path.",
                "Explorer Remote FS", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        IntPtr rootPidl = IntPtr.Zero;
        IntPtr relativePidl = IntPtr.Zero;
        IntPtr folderPidl = IntPtr.Zero;
        IntPtr folderUnknown = IntPtr.Zero;
        IShellFolderNative? rootFolder = null;
        try
        {
            ErfLog($"parsed public address; target='{target}'");
            // ShellExecute can consume the private parsing name "::{CLSID}\\…",
            // but IWebBrowser2.Navigate2 rejects it with E_INVALIDARG.  The
            // browser automation contract expects the equivalent shell URL.
            var parsingName = $"::{ExplorerRemoteFsClsid}\\{target}";
            var browserAddress = $"shell:::{ExplorerRemoteFsClsid}\\{target}";
            if (TryNavigateForegroundExplorer(browserAddress, sourceWindow))
            {
                ErfLog("navigated the foreground Explorer window");
                return;
            }

            ErfLog("foreground Explorer window was unavailable; using new-window fallback");
            // Resolve only the namespace root through the general Shell parser.
            // A suffix such as "WSL:/home" is our extension's grammar, not a
            // general Windows parsing name.  Feed that suffix directly to the
            // root IShellFolder, whose ParseDisplayName already binds every
            // Unix path component using the remote directory semantics.
            var rootResult = SHParseDisplayName($"::{ExplorerRemoteFsClsid}", IntPtr.Zero, out rootPidl, 0, out _);
            ErfLog($"resolve namespace root; hr=0x{rootResult:X8}; pidl=0x{rootPidl.ToInt64():X}");
            if (rootResult < 0)
            {
                MessageBox.Show(
                    $"Unable to find the Explorer Remote FS namespace (0x{rootResult:X8}).",
                    "Explorer Remote FS", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            var shellFolderId = typeof(IShellFolderNative).GUID;
            // A null parent means that rootPidl is absolute (relative to the
            // desktop).  SHBindToObject's first parameter is the parent
            // IShellFolder pointer, not the PIDL itself.
            var bindResult = SHBindToObject(IntPtr.Zero, rootPidl, IntPtr.Zero, ref shellFolderId, out folderUnknown);
            ErfLog($"bind namespace root; hr=0x{bindResult:X8}; unknown=0x{folderUnknown.ToInt64():X}");
            if (bindResult < 0)
            {
                MessageBox.Show(
                    $"Unable to bind the Explorer Remote FS namespace (0x{bindResult:X8}).",
                    "Explorer Remote FS", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            rootFolder = (IShellFolderNative)Marshal.GetObjectForIUnknown(folderUnknown);
            var attributes = 0u;
            var targetResult = rootFolder.ParseDisplayName(IntPtr.Zero, IntPtr.Zero, target, out _, out relativePidl, ref attributes);
            ErfLog($"resolve target through namespace; hr=0x{targetResult:X8}; relative=0x{relativePidl.ToInt64():X}");
            if (targetResult < 0)
            {
                MessageBox.Show(
                    $"Unable to resolve ERF address '{address}' (0x{targetResult:X8}).",
                    "Explorer Remote FS", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            folderPidl = ILCombine(rootPidl, relativePidl);
            ErfLog($"combine target PIDL; folder=0x{folderPidl.ToInt64():X}");
            if (folderPidl == IntPtr.Zero)
            {
                MessageBox.Show("Unable to prepare the ERF folder location.", "Explorer Remote FS", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            // SHOpenFolderAndSelectItems(folderPidl, 0, ...) opens the PARENT
            // and merely selects folderPidl, which is not navigation.  Execute
            // the virtual folder's normal Shell verb against its fully resolved
            // PIDL so Explorer opens the folder itself.
            var executeInfo = new ShellExecuteInfo
            {
                cbSize = Marshal.SizeOf<ShellExecuteInfo>(),
                fMask = SeeMaskIdList,
                lpVerb = "open",
                lpIDList = folderPidl,
                nShow = 1, // SW_SHOWNORMAL
            };
            var shellExecuted = ShellExecuteEx(ref executeInfo);
            ErfLog($"shell open; result={shellExecuted}; win32={Marshal.GetLastWin32Error()}; instance=0x{executeInfo.hInstApp.ToInt64():X}");
            if (!shellExecuted)
            {
                var error = Marshal.GetLastWin32Error();
                MessageBox.Show(
                    $"Unable to open ERF address (Win32 error {error}).",
                    "Explorer Remote FS", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
        catch (Exception ex)
        {
            ErfLog($"unhandled exception; {ex}");
            MessageBox.Show(ex.Message, "Explorer Remote FS", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        finally
        {
            if (rootFolder is not null) Marshal.ReleaseComObject(rootFolder);
            if (folderUnknown != IntPtr.Zero) Marshal.Release(folderUnknown);
            if (folderPidl != IntPtr.Zero) Marshal.FreeCoTaskMem(folderPidl);
            if (relativePidl != IntPtr.Zero) Marshal.FreeCoTaskMem(relativePidl);
            if (rootPidl != IntPtr.Zero) Marshal.FreeCoTaskMem(rootPidl);
        }
    }

    private static void ErfLog(string message)
    {
        try
        {
            File.AppendAllText(
                Path.Combine(Path.GetTempPath(), "remotefs-erf.log"),
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] pid={Environment.ProcessId} {message}{Environment.NewLine}");
        }
        catch
        {
            // ERF navigation must remain usable when the temporary log cannot be written.
        }
    }

    private static bool TryNavigateForegroundExplorer(string parsingName, IntPtr sourceWindow)
    {
        // A URI protocol handler is launched out-of-process.  By the time the
        // resident service has warmed the remote path, Explorer may have
        // changed foreground ownership, even though the HWND captured by the
        // short-lived protocol shim is still the correct source browser.
        // Prefer that original HWND, but also consider the Explorer window
        // currently in the foreground.  Do not use only one of them: doing so
        // was the reason a valid request fell through to ShellExecuteEx and
        // opened a second Explorer window.
        var candidates = new HashSet<IntPtr>();
        var original = sourceWindow == IntPtr.Zero ? IntPtr.Zero : GetAncestor(sourceWindow, GaRoot);
        var current = GetAncestor(GetForegroundWindow(), GaRoot);
        if (original != IntPtr.Zero) candidates.Add(original);
        if (current != IntPtr.Zero) candidates.Add(current);
        if (candidates.Count == 0)
        {
            ErfLog("no source or foreground window available for in-place navigation");
            return false;
        }

        ErfLog($"in-place navigation candidates: source=0x{original.ToInt64():X}; current=0x{current.ToInt64():X}");

        object? shellWindows = null;
        try
        {
            var type = Type.GetTypeFromCLSID(ShellWindowsClsid);
            if (type is null) return false;
            shellWindows = Activator.CreateInstance(type);
            if (shellWindows is null) return false;

            var count = Convert.ToInt32(shellWindows.GetType().InvokeMember(
                "Count",
                System.Reflection.BindingFlags.GetProperty,
                null,
                shellWindows,
                null));
            ErfLog($"enumerating ShellWindows; count={count}");
            for (var index = 0; index < count; index++)
            {
                object? browserObject = null;
                try
                {
                    // Do not invoke IShellWindows::Item through the C# dynamic
                    // COM binder.  On current Explorer it marshals the VARIANT
                    // index incorrectly and returns E_INVALIDARG ("Value does
                    // not fall within the expected range").  IDispatch
                    // InvokeMember uses the same ordinary value invocation as
                    // PowerShell's Shell.Application.Windows().Item(index).
                    browserObject = shellWindows.GetType().InvokeMember(
                        "Item",
                        System.Reflection.BindingFlags.InvokeMethod | System.Reflection.BindingFlags.GetProperty,
                        null,
                        shellWindows,
                        [index]);
                    if (browserObject is null) continue;
                    var browserHandle = Convert.ToInt64(browserObject.GetType().InvokeMember(
                        "HWND",
                        System.Reflection.BindingFlags.GetProperty,
                        null,
                        browserObject,
                        null));
                    var browserWindow = GetAncestor(new IntPtr(browserHandle), GaRoot);
                    var matched = candidates.Contains(browserWindow);
                    ErfLog($"ShellWindows[{index}] hwnd=0x{browserWindow.ToInt64():X}; matched={matched}");
                    if (!matched) continue;

                    // Navigate2 accepts Shell parsing names and keeps the
                    // matched browser window/tab rather than ShellExecuteEx
                    // creating a new Explorer window.  Invoke it exactly as
                    // Shell.Application automation does: pass only the URL and
                    // let COM supply the optional VARIANT arguments.  Explicit
                    // Type.Missing values are rejected by this Explorer host.
                    browserObject.GetType().InvokeMember(
                        "Navigate2",
                        System.Reflection.BindingFlags.InvokeMethod | System.Reflection.BindingFlags.OptionalParamBinding,
                        null,
                        browserObject,
                        [parsingName]);
                    ErfLog($"requested in-place navigation; hwnd=0x{browserWindow.ToInt64():X}; target='{parsingName}'");
                    return true;
                }
                catch (Exception ex)
                {
                    // A collection entry may vanish while Explorer is opening
                    // or closing.  It must not prevent us from checking the
                    // remaining Explorer windows.
                    var detail = ex is System.Reflection.TargetInvocationException { InnerException: not null }
                        ? ex.InnerException.ToString()
                        : ex.ToString();
                    ErfLog($"ShellWindows[{index}] skipped; {detail}");
                }
                finally
                {
                    if (browserObject is not null && Marshal.IsComObject(browserObject))
                        Marshal.FinalReleaseComObject(browserObject);
                }
            }
        }
        catch (Exception ex)
        {
            ErfLog($"foreground Explorer navigation failed; {ex.Message}");
        }
        finally
        {
            if (shellWindows is not null && Marshal.IsComObject(shellWindows))
                Marshal.FinalReleaseComObject(shellWindows);
        }

        return false;
    }

    private static bool TryBuildErfTarget(string address, out string target)
    {
        target = string.Empty;
        if (!address.StartsWith(ErfScheme, StringComparison.OrdinalIgnoreCase)) return false;

        var addressTarget = address[ErfScheme.Length..];
        var separator = addressTarget.IndexOf(':');
        if (separator <= 0 || separator == addressTarget.Length - 1) return false;

        var site = Uri.UnescapeDataString(addressTarget[..separator]);
        var remotePath = Uri.UnescapeDataString(addressTarget[(separator + 1)..]);
        if (site.IndexOfAny(['/', '\\', ':']) >= 0 || !remotePath.StartsWith('/') || remotePath.Contains('\\')) return false;

        target = $"{site}:{remotePath}";
        return true;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHParseDisplayName(
        string name,
        IntPtr bindingContext,
        out IntPtr pidl,
        uint attributes,
        out uint attributesOut);

    [DllImport("shell32.dll")]
    private static extern int SHBindToObject(
        IntPtr parentShellFolder,
        IntPtr relativeOrAbsolutePidl,
        IntPtr bindingContext,
        ref Guid interfaceId,
        out IntPtr result);

    [DllImport("shell32.dll")]
    private static extern IntPtr ILCombine(IntPtr parentPidl, IntPtr childPidl);

    private const uint SeeMaskIdList = 0x00000004;
    private const uint GaRoot = 2;

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern IntPtr GetAncestor(IntPtr window, uint flags);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShellExecuteEx(ref ShellExecuteInfo executeInfo);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ShellExecuteInfo
    {
        public int cbSize;
        public uint fMask;
        public IntPtr hwnd;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpVerb;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpFile;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpParameters;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpDirectory;
        public int nShow;
        public IntPtr hInstApp;
        public IntPtr lpIDList;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpClass;
        public IntPtr hkeyClass;
        public uint dwHotKey;
        public IntPtr hIconOrMonitor;
        public IntPtr hProcess;
    }

    [ComImport]
    [Guid("000214E6-0000-0000-C000-000000000046")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellFolderNative
    {
        [PreserveSig]
        int ParseDisplayName(
            IntPtr hwnd,
            IntPtr bindingContext,
            [MarshalAs(UnmanagedType.LPWStr)] string displayName,
            out uint eaten,
            out IntPtr relativePidl,
            ref uint attributes);
    }

    private void CreateTrayIcon()
    {
        _trayMenu = new WinForms.ContextMenuStrip();
        _trayIcon = new WinForms.NotifyIcon
        {
            Icon = _status.TrayIcon(),
            Text = Ui.T("AppName"),
            ContextMenuStrip = _trayMenu,
            Visible = true
        };
        _trayIcon.DoubleClick += (_, _) => ShowTransfers();
        _trayIcon.BalloonTipClicked += (_, _) => ShowTransfers();
        // 状态（R/F 两个字母的颜色）变了就换图标；事件可能来自后台线程，编组到 UI 线程。
        _status.Changed += () => Dispatcher.BeginInvoke(UpdateTrayStatus);
        RefreshLocalizedShell();
    }

    internal void RefreshLocalizedShell()
    {
        if (_trayMenu is not null)
        {
            _trayMenu.Items.Clear();
            _trayMenu.Items.Add(Ui.T("TrayManage"), null, (_, _) => ShowManager());
            _trayMenu.Items.Add(Ui.IsEnglish ? "Transfer queue" : "传输队列", null, (_, _) => ShowTransfers());
            _trayMenu.Items.Add(new WinForms.ToolStripSeparator());
            _trayMenu.Items.Add(Ui.T("Exit"), null, (_, _) => ExitApplication());
        }
        UpdateTrayStatus();
    }

    /// <summary>托盘：图标 = ERF 状态图标，提示文字 = 应用名 + 当前状态。
    /// 标题栏的窗口图标也用同一套（见 MainWindow/子窗口设置）。</summary>
    private void UpdateTrayStatus()
    {
        if (_trayIcon is null) return;
        int running = _transfers?.RunningCount ?? 0;
        _trayIcon.Icon = _status.TrayIcon(16);

        string name = Ui.T("AppName");
        string detail = _status.Transfer switch
        {
            ErfIcon.TransferState.Error => Ui.T("TrayTransferError"),
            ErfIcon.TransferState.Active => Ui.T("TrayTransferring").Replace("{0}", Math.Max(running, 1).ToString()),
            _ => Ui.T("TrayIdle"),
        };
        if (_status.Remote == ErfIcon.RemoteState.Problem) detail = Ui.T("TrayRemoteProblem") + " · " + detail;
        _trayIcon.Text = name + " — " + detail;
    }

    private void ListenForShowRequests()
    {
        _ = Task.Run(() =>
        {
            try
            {
                WaitHandle[] waits = { _showManagerEvent!, _showTransfersEvent! };
                while (!_shutdown!.IsCancellationRequested)
                {
                    int which = WaitHandle.WaitAny(waits);
                    if (_shutdown.IsCancellationRequested) break;
                    if (which == 1) Dispatcher.BeginInvoke(ShowTransfers);
                    else Dispatcher.BeginInvoke(ShowManager);
                }
            }
            catch (ObjectDisposedException) { }
        });
    }

    /// 每来一个传输任务/每批结束都把状态交给 RemoteStatusService，
    /// 托盘图标（R/F 颜色）与提示文字由 UpdateTrayStatus 统一刷新。
    private void UpdateTrayTransferState()
    {
        if (_transfers is null) { UpdateTrayStatus(); return; }
        if (_transfers.RunningCount > 0)
            _status.SetTransfer(ErfIcon.TransferState.Active);
        else if (_transfers.Tasks.Any(t => t.IsFailed))
            _status.SetTransfer(ErfIcon.TransferState.Error);
        else
            _status.SetTransfer(ErfIcon.TransferState.Idle);
        UpdateTrayStatus();
    }

    /// <summary>No per-job balloon: copying a folder runs one job per file, so
    /// notifying on start (or on every file's completion) spams the user. The
    /// tray icon/tooltip still shows activity; the balloon is emitted ONCE, by
    /// OnAllTransfersFinished, when the whole batch has drained.</summary>
    private void OnTransferJobStarted() => UpdateTrayTransferState();

    private void OnAllTransfersFinished()
    {
        UpdateTrayTransferState();
        if (_trayIcon is null || _transfers is null) return;
        int done = _transfers.BatchDone, failed = _transfers.BatchFailed, cancelled = _transfers.BatchCancelled;
        int notes = failed + cancelled;
        if (done + notes <= 0) return;
        string text = Ui.IsEnglish
            ? (notes > 0 ? $"{done} completed · {failed} failed · {cancelled} cancelled"
                         : $"{done} transfer(s) completed")
            : (notes > 0 ? $"{done} 个已完成 · {failed} 个失败 · {cancelled} 个已取消"
                         : $"{done} 个传输已完成");
        _trayIcon.ShowBalloonTip(3000, Ui.T("AppName"), text,
            notes > 0 ? WinForms.ToolTipIcon.Warning : WinForms.ToolTipIcon.Info);
    }

    /// <summary>Blue disc with up/down arrows, drawn once and cached.</summary>
    private static Drawing.Icon? _busyIcon;

    private static Drawing.Icon BusyTransferIcon()
    {
        if (_busyIcon is not null) return _busyIcon;
        try
        {
            using var bmp = new Drawing.Bitmap(16, 16);
            using (Drawing.Graphics g = Drawing.Graphics.FromImage(bmp))
            {
                g.SmoothingMode = Drawing.Drawing2D.SmoothingMode.AntiAlias;
                g.Clear(Drawing.Color.Transparent);
                using var disc = new Drawing.SolidBrush(Drawing.Color.FromArgb(0, 120, 215));
                g.FillEllipse(disc, 0, 0, 15, 15);
                using var pen = new Drawing.Pen(Drawing.Color.White, 1.6f);
                g.DrawLine(pen, 5.5f, 10.5f, 5.5f, 5f);
                g.DrawLine(pen, 3.5f, 7f, 5.5f, 5f);
                g.DrawLine(pen, 7.5f, 7f, 5.5f, 5f);
                g.DrawLine(pen, 10.5f, 5f, 10.5f, 10.5f);
                g.DrawLine(pen, 8.5f, 8.5f, 10.5f, 10.5f);
                g.DrawLine(pen, 12.5f, 8.5f, 10.5f, 10.5f);
            }
            _busyIcon = Drawing.Icon.FromHandle(bmp.GetHicon());
        }
        catch { _busyIcon = Drawing.SystemIcons.Application; }
        return _busyIcon;
    }

    /// <summary>Open the site manager focused on the transfer queue.</summary>
    private void ShowTransfers()
    {
        ShowManager();
        _manager?.SelectTransferTab();
    }

    private void ShowManager()
    {
        if (_manager is null) return;
        if (!_manager.IsVisible) _manager.Show();
        if (_manager.WindowState == WindowState.Minimized) _manager.WindowState = WindowState.Normal;
        _manager.ShowInTaskbar = true;
        _manager.Activate();
        _manager.Topmost = true;
        _manager.Topmost = false;
        _manager.Focus();
    }

    private void OnManagerClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (_isExiting) return;
        e.Cancel = true;
        if (_manager is not null) { _manager.Hide(); _manager.ShowInTaskbar = false; }
    }

    private void ExitApplication() { _isExiting = true; Shutdown(); }

    protected override void OnExit(ExitEventArgs e)
    {
        _isExiting = true;
        _shutdown?.Cancel();
        _bridge?.Dispose();
        if (_trayIcon is not null) { _trayIcon.Visible = false; _trayIcon.Dispose(); }
        _trayMenu?.Dispose();
        _showManagerEvent?.Dispose();
        _singleInstance?.Dispose();
        _shutdown?.Dispose();
        base.OnExit(e);
    }
}
