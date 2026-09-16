using System.Windows.Threading;

namespace RemoteFsClient.Services;

/// <summary>Owns the one visible long-running/destructive-operation progress window
/// (delete, recursive chmod). RemoteBridgeService calls this from pipe/worker
/// threads; all WPF access is marshalled here, so remote operations never run
/// on the UI thread.</summary>
public sealed class DeleteProgressService
{
    private readonly Dispatcher _dispatcher;

    public DeleteProgressService(Dispatcher dispatcher) => _dispatcher = dispatcher;

    public DeleteProgressHandle Begin(string site, string path, Action cancel,
                                      DeleteProgressWindow.Operation operation = DeleteProgressWindow.Operation.Delete) =>
        _dispatcher.Invoke(() =>
        {
            var window = new DeleteProgressWindow(site, path, cancel, operation);
            window.Show();
            return new DeleteProgressHandle(_dispatcher, window);
        });
}

public sealed class DeleteProgressHandle
{
    private readonly Dispatcher _dispatcher;
    private readonly DeleteProgressWindow _window;

    internal DeleteProgressHandle(Dispatcher dispatcher, DeleteProgressWindow window)
    {
        _dispatcher = dispatcher;
        _window = window;
    }

    public void Update(long done, long total, string current) =>
        _dispatcher.BeginInvoke(() => { if (_window.IsVisible) _window.Update(done, total, current); });

    public void Complete(bool ok, bool cancelled, string? detail) =>
        _dispatcher.BeginInvoke(() => { if (_window.IsVisible) _window.Complete(ok, cancelled, detail); });
}
