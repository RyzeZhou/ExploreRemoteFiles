using System.ComponentModel;
using System.Windows;
using System.Windows.Threading;
using RemoteFsClient.Services;

namespace RemoteFsClient;

/// <summary>远程长耗时/破坏性操作的进度窗口（删除、递归修改权限）。
/// 这类操作没有"源→目标"的字节总量，用"已完成 N 项"更直观，措辞也应当比普通传输更醒目；
/// 有明确源/目标的传输仍然走主窗口的传输队列。</summary>
public partial class DeleteProgressWindow : Window
{
    /// <summary>操作类型，决定文案。</summary>
    public enum Operation { Delete, Chmod }

    private readonly Action _cancel;
    private readonly Operation _operation;
    private bool _cancelRequested;
    private bool _mayClose;

    public DeleteProgressWindow(string site, string path, Action cancel, Operation operation = Operation.Delete)
    {
        InitializeComponent();
        _cancel = cancel;
        _operation = operation;
        bool en = Ui.IsEnglish;
        // 标题带上站点名：同时挂多个站点/多个操作时，一眼能看出这是谁的任务。
        Title = (en ? "Operation queue: " : "操作队列：") + site;
        TitleText.Text = operation == Operation.Delete
            ? (en ? "Deleting remote items" : "正在删除远程项目")
            : (en ? "Applying permissions recursively" : "正在递归修改权限");
        PathText.Text = site + ":" + path;
        StatusText.Text = operation == Operation.Delete
            ? (en ? "Preparing deletion…" : "正在准备删除…")
            : (en ? "Preparing permission change…" : "正在准备修改权限…");
        CancelButton.Content = operation == Operation.Delete
            ? (en ? "Cancel deletion" : "取消删除")
            : (en ? "Cancel change" : "取消修改");
        Loaded += (_, _) =>
        {
            var area = SystemParameters.WorkArea;
            Left = area.Right - ActualWidth - 24;
            Top = area.Bottom - ActualHeight - 24;
        };
    }

    public void Update(long done, long total, string current)
    {
        bool en = Ui.IsEnglish;
        if (total > 0)
        {
            Progress.IsIndeterminate = false;
            Progress.Value = Math.Min(100, Math.Max(0, done * 100.0 / total));
            StatusText.Text = _operation == Operation.Delete
                ? (en ? $"Deleted {done} of {total}: " : $"已删除 {done} / {total}：") + current
                : (en ? $"Updated {done} of {total}: " : $"已修改 {done} / {total}：") + current;
        }
        else
        {
            Progress.IsIndeterminate = true;
            StatusText.Text = current;
        }
    }

    public void Complete(bool ok, bool cancelled, string? detail)
    {
        bool en = Ui.IsEnglish;
        string okText = _operation == Operation.Delete
            ? (en ? "Deletion completed" : "删除完成")
            : (en ? "Permissions updated" : "权限修改完成");
        string cancelText = _operation == Operation.Delete
            ? (en ? "Deletion cancelled" : "已取消删除")
            : (en ? "Permission change cancelled" : "已取消修改");
        string failPrefix = _operation == Operation.Delete
            ? (en ? "Deletion failed: " : "删除失败：")
            : (en ? "Permission change failed: " : "权限修改失败：");
        Progress.IsIndeterminate = false;
        Progress.Value = ok ? 100 : Progress.Value;
        CancelButton.IsEnabled = false;
        StatusText.Text = cancelled ? cancelText : ok ? okText : failPrefix + (detail ?? "");
        if (!string.IsNullOrWhiteSpace(detail)) StatusText.ToolTip = detail;
        var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(ok ? 1.2 : 4) };
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
