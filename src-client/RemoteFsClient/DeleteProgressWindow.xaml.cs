using System.ComponentModel;
using System.Windows;
using System.Windows.Threading;
using RemoteFsClient.Services;

namespace RemoteFsClient;

/// <summary>A small foreground status window for a destructive remote delete.
/// It is intentionally separate from the transfer queue: deletion has no
/// source/destination and deserves a clearer, risk-oriented presentation.</summary>
public partial class DeleteProgressWindow : Window
{
    private readonly Action _cancel;
    private bool _cancelRequested;
    private bool _mayClose;

    public DeleteProgressWindow(string site, string path, Action cancel)
    {
        InitializeComponent();
        _cancel = cancel;
        Title = "Explorer Remote FS";
        TitleText.Text = Ui.IsEnglish ? "Deleting remote items" : "正在删除远程项目";
        PathText.Text = site + ":" + path;
        StatusText.Text = Ui.IsEnglish ? "Preparing deletion…" : "正在准备删除…";
        CancelButton.Content = Ui.IsEnglish ? "Cancel deletion" : "取消删除";
        Loaded += (_, _) =>
        {
            var area = SystemParameters.WorkArea;
            Left = area.Right - ActualWidth - 24;
            Top = area.Bottom - ActualHeight - 24;
        };
    }

    public void Update(long done, long total, string current)
    {
        if (total > 0)
        {
            Progress.IsIndeterminate = false;
            Progress.Value = Math.Min(100, Math.Max(0, done * 100.0 / total));
            StatusText.Text = (Ui.IsEnglish ? $"Deleted {done} of {total}: " : $"已删除 {done} / {total}：") + current;
        }
        else
        {
            Progress.IsIndeterminate = true;
            StatusText.Text = current;
        }
    }

    public void Complete(bool ok, bool cancelled, string? detail)
    {
        Progress.IsIndeterminate = false;
        Progress.Value = ok ? 100 : Progress.Value;
        CancelButton.IsEnabled = false;
        StatusText.Text = cancelled
            ? (Ui.IsEnglish ? "Deletion cancelled" : "已取消删除")
            : ok ? (Ui.IsEnglish ? "Deletion completed" : "删除完成")
            : (Ui.IsEnglish ? "Deletion failed: " : "删除失败：") + (detail ?? "");
        var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(ok ? 1.2 : 3) };
        timer.Tick += (_, _) => { timer.Stop(); _mayClose = true; Close(); };
        timer.Start();
    }

    private void OnCancel(object sender, RoutedEventArgs e) => RequestCancel();

    private void RequestCancel()
    {
        if (_cancelRequested) return;
        _cancelRequested = true;
        CancelButton.IsEnabled = false;
        StatusText.Text = Ui.IsEnglish ? "Cancelling…" : "正在取消…";
        _cancel();
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (!_mayClose)
        {
            e.Cancel = true;
            RequestCancel();
        }
        base.OnClosing(e);
    }
}
