using System.Threading;
using System.Windows;
using WinForms = System.Windows.Forms;
using Drawing = System.Drawing;
using RemoteFsClient.Services;

namespace RemoteFsClient;

/// <summary>用户会话内的常驻控制中心：通知区入口、站点管理器，以及 Shell 本地桥接服务。</summary>
public partial class App : System.Windows.Application
{
    private const string SingleInstanceName = @"Local\ExplorerRemoteFs.Client";
    private const string ShowManagerEventName = @"Local\ExplorerRemoteFs.ShowManager";
    private Mutex? _singleInstance;
    private EventWaitHandle? _showManagerEvent;
    private CancellationTokenSource? _shutdown;
    private WinForms.NotifyIcon? _trayIcon;
    private WinForms.ContextMenuStrip? _trayMenu;
    private MainWindow? _manager;
    private RemoteBridgeService? _bridge;
    private bool _isExiting;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        Ui.SetLanguage(AppSettings.Load().ServiceLanguage);
        ShutdownMode = ShutdownMode.OnExplicitShutdown;
        bool background = e.Args.Any(a => string.Equals(a, "--background", StringComparison.OrdinalIgnoreCase));
        bool showRequested = e.Args.Any(a => string.Equals(a, "--show", StringComparison.OrdinalIgnoreCase));

        _singleInstance = new Mutex(true, SingleInstanceName, out bool isFirstInstance);
        if (!isFirstInstance)
        {
            if (showRequested || !background)
            {
                try { EventWaitHandle.OpenExisting(ShowManagerEventName).Set(); }
                catch { }
            }
            Shutdown();
            return;
        }

        _shutdown = new CancellationTokenSource();
        _showManagerEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowManagerEventName, out _);
        _manager = new MainWindow();
        _manager.Closing += OnManagerClosing;
        MainWindow = _manager;
        CreateTrayIcon();
        _bridge = new RemoteBridgeService();
        _bridge.Start();
        ListenForShowRequests();
        if (!background || showRequested) ShowManager();
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
        _trayIcon.DoubleClick += (_, _) => ShowManager();
        RefreshLocalizedShell();
    }

    internal void RefreshLocalizedShell()
    {
        if (_trayMenu is not null)
        {
            _trayMenu.Items.Clear();
            _trayMenu.Items.Add(Ui.T("TrayManage"), null, (_, _) => ShowManager());
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
                while (!_shutdown!.IsCancellationRequested)
                {
                    _showManagerEvent!.WaitOne();
                    if (!_shutdown.IsCancellationRequested) Dispatcher.BeginInvoke(ShowManager);
                }
            }
            catch (ObjectDisposedException) { }
        });
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