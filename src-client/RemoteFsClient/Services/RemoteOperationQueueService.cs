using System.Windows;
using System.Windows.Threading;

namespace RemoteFsClient.Services;

/// <summary>远程操作队列：**一个站点一个窗口**，窗口内多个条目（删除 / 递归改权限）并存。
///
/// 调用方（RemoteBridgeService）在管道线程上调用；所有 WPF 访问都在这里
/// 编组到 UI 线程，所以远程操作永远不会跑到 UI 线程上。
/// 窗口一旦全空并关闭，下次会重新创建；同一个站点连做几件事只会有**一个**窗口。</summary>
public sealed class RemoteOperationQueueService
{
    private readonly Dispatcher _dispatcher;
    private readonly Dictionary<string, RemoteOperationQueueWindow> _windows = new(StringComparer.OrdinalIgnoreCase);

    public RemoteOperationQueueService(Dispatcher dispatcher) => _dispatcher = dispatcher;

    public OperationHandle Begin(string site, string path,
                                 RemoteOperationQueueWindow.OperationKind kind, Action cancel) =>
        _dispatcher.Invoke(() =>
        {
            if (!_windows.TryGetValue(site, out var window) || !window.IsVisible)
            {
                window = new RemoteOperationQueueWindow(site);
                var created = window;
                window.Closed += (_, _) =>
                {
                    if (_windows.TryGetValue(site, out var current) && ReferenceEquals(current, created))
                        _windows.Remove(site);
                };
                _windows[site] = window;
                window.Show();
            }
            else if (window.WindowState == WindowState.Minimized)
            {
                window.WindowState = WindowState.Normal;
            }

            var entry = window.AddEntry(path, kind, cancel);
            window.Activate();
            return new OperationHandle(_dispatcher, window, entry);
        });
}

/// <summary>一个队列条目的句柄：进度与结果都只作用于它自己，
/// 而不是整个窗口（同站点可能存在多个并发操作）。</summary>
public sealed class OperationHandle
{
    private readonly Dispatcher _dispatcher;
    private readonly RemoteOperationQueueWindow _window;
    private readonly RemoteOperationQueueWindow.Entry _entry;

    internal OperationHandle(Dispatcher dispatcher, RemoteOperationQueueWindow window,
                             RemoteOperationQueueWindow.Entry entry)
    {
        _dispatcher = dispatcher;
        _window = window;
        _entry = entry;
    }

    public void Update(long done, long total, string current) =>
        _dispatcher.BeginInvoke(() => _entry.Update(done, total, current));

    public void Complete(bool ok, bool cancelled, string? detail) =>
        _dispatcher.BeginInvoke(() => _entry.Complete(ok, cancelled, detail));
}
