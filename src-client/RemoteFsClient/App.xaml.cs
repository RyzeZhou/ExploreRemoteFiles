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
    private bool _isExiting;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        Ui.SetLanguage(AppSettings.Load().ServiceLanguage);
        ShutdownMode = ShutdownMode.OnExplicitShutdown;
        bool background = e.Args.Any(a => string.Equals(a, "--background", StringComparison.OrdinalIgnoreCase));
        bool showRequested = e.Args.Any(a => string.Equals(a, "--show", StringComparison.OrdinalIgnoreCase));
        bool transfersRequested = e.Args.Any(a => string.Equals(a, "--transfers", StringComparison.OrdinalIgnoreCase));

        _singleInstance = new Mutex(true, SingleInstanceName, out bool isFirstInstance);
        if (!isFirstInstance)
        {
            if (transfersRequested)
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
        _manager = new MainWindow();
        _manager.AttachTasks(_transfers);
        _manager.Closing += OnManagerClosing;
        MainWindow = _manager;
        CreateTrayIcon();
        _bridge = new RemoteBridgeService();
        _bridge.Start();
        ListenForShowRequests();
        if (transfersRequested) ShowTransfers();
        else if (!background || showRequested) ShowManager();
    }

    private void CreateTrayIcon()
    {
        _trayMenu = new WinForms.ContextMenuStrip();
        _trayIcon = new WinForms.NotifyIcon
        {
            Icon = Drawing.SystemIcons.Application,
            Text = "Explorer Remote FS",
            ContextMenuStrip = _trayMenu,
            Visible = true
        };
        _trayIcon.DoubleClick += (_, _) => ShowTransfers();
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
        if (_trayIcon is not null) _trayIcon.Text = "Explorer Remote FS";
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

    /// <summary>Tray reflects transfer state: while jobs run the icon becomes a
    /// transfer glyph and the tooltip counts them, so "something is happening"
    /// is visible in Explorer's notification area; double-click / the menu item
    /// jumps to the Transfers tab in the site manager.</summary>
    private void UpdateTrayTransferState()
    {
        if (_trayIcon is null) return;
        int running = _transfers?.RunningCount ?? 0;
        if (running > 0)
        {
            _trayIcon.Icon = BusyTransferIcon();
            _trayIcon.Text = (Ui.IsEnglish ? "Explorer Remote FS — transferring " : "Explorer Remote FS — 正在传输 ")
                             + running + (Ui.IsEnglish ? " file(s)" : " 个文件");
        }
        else
        {
            _trayIcon.Icon = Drawing.SystemIcons.Application;
            _trayIcon.Text = "Explorer Remote FS";
        }
    }

    private void OnTransferJobStarted()
    {
        bool first = (_transfers?.RunningCount ?? 0) <= 1;
        UpdateTrayTransferState();
        if (first && _trayIcon is not null)
            _trayIcon.ShowBalloonTip(2500, "Explorer Remote FS",
                Ui.IsEnglish ? "Transfer started — click here for the queue" : "已开始传输 — 点击此处查看传输队列",
                WinForms.ToolTipIcon.Info);
    }

    private void OnAllTransfersFinished() => UpdateTrayTransferState();

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