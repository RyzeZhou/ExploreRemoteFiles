using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;
using RemoteFsClient.Services;

namespace RemoteFsClient;

/// <summary>远程长耗时/破坏性操作的**队列窗口**：一个站点一个窗口，
/// 窗口内多个条目共存（删除 / 递归改权限），每条目各自取消，末尾汇总失败明细，
/// 并在任务栏上显示整体进度。
///
/// 为什么合并成一个窗口：删除与递归改权限都是"没有字节总量、可能要跑很久、
/// 还可能部分失败"的远程操作。以前每次操作各弹一个小窗，同时做两件事就会
/// 出现两个窗互相盖住、也分不清谁是谁；合并到「操作队列：&lt;站点名&gt;」后
/// 条目和取消是逐条的，站点则写在标题上。
///
/// 线程模型：所有方法都必须在 UI 线程上调用（服务层用 Dispatcher 封好）；
/// 远程操作本身在后台线程上跑，只通过 Update/Complete 回报。</summary>
public partial class RemoteOperationQueueWindow : Window
{
    /// <summary>操作种类，决定条目文案与颜色。</summary>
    public enum OperationKind { Delete, Chmod }

    private readonly string _site;
    private readonly ObservableCollection<Entry> _entries = new();
    private readonly DispatcherTimer _autoClose;
    private bool _allowClose;

    public string Site => _site;

    public RemoteOperationQueueWindow(string site)
    {
        InitializeComponent();
        _site = site;
        Entries.ItemsSource = _entries;

        Title = Ui.T("OpQueueTitle").Replace("{0}", site);
        TitleText.Text = Title;
        CancelAllButton.Content = Ui.T("OpCancelAll");
        CloseButton.Content = Ui.T("OpClose");

        // 全部结束后自动收起（仍在队列里的新条目会让计时器重来）。
        _autoClose = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
        _autoClose.Tick += (_, _) =>
        {
            _autoClose.Stop();
            if (_entries.Any(e => !e.IsFinished)) return;
            _allowClose = true;
            Close();
        };

        // 右下角：不挡住资源管理器里正在做的事，也不置顶（任务栏按钮 + 任务栏进度
        // 已经回答了"还在忙吗"），同时比"居中的置顶窗"更容易被忽略掉。
        Loaded += (_, _) =>
        {
            var area = SystemParameters.WorkArea;
            Left = Math.Max(area.Left, area.Right - ActualWidth - 24);
            Top = Math.Max(area.Top, area.Bottom - ActualHeight - 24);
        };

        Refresh();
    }

    /// <summary>追加一个条目，返回它的句柄（服务层用它回报进度/结果）。</summary>
    public Entry AddEntry(string path, OperationKind kind, Action cancel)
    {
        var entry = new Entry(path, kind, cancel);
        // 条目自己发生变化就刷新汇总/任务栏：不依赖调用方记得再调一次 Refresh
        // （自检就抓到过这个漏洞——直接 Complete 会让汇总一直停在"进行中"）。
        entry.Changed = Refresh;
        _entries.Add(entry);
        _autoClose.Stop();
        Refresh();
        return entry;
    }

    /// <summary>重算标题、汇总、按钮可用性与任务栏进度。UI 线程专用。</summary>
    public void Refresh()
    {
        int active = _entries.Count(e => !e.IsFinished);
        int done = _entries.Count(e => e.IsFinished && e.Succeeded);
        int failed = _entries.Count(e => e.IsFinished && !e.Succeeded && !e.WasCancelled);
        int cancelled = _entries.Count(e => e.WasCancelled);

        if (_entries.Count == 0)
            SummaryText.Text = Ui.T("OpQueueEmpty");
        else if (active > 0)
            SummaryText.Text = Ui.T("OpQueueRunning")
                .Replace("{0}", active.ToString())
                .Replace("{1}", (done + failed + cancelled).ToString());
        else
        {
            var text = Ui.T("OpQueueFinished")
                .Replace("{0}", done.ToString())
                .Replace("{1}", failed.ToString())
                .Replace("{2}", cancelled.ToString());
            var failures = _entries.Where(e => !e.Succeeded && !e.WasCancelled && !string.IsNullOrWhiteSpace(e.Detail))
                                   .Select(e => e.Detail!).Take(3).ToList();
            if (failures.Count > 0)
                text += Environment.NewLine + Ui.T("OpQueueFailures").Replace("{0}", string.Join("; ", failures));
            SummaryText.Text = text;
        }

        CancelAllButton.IsEnabled = active > 0;
        CloseButton.Content = active > 0 ? Ui.T("OpCloseBlocked") : Ui.T("OpClose");
        CloseButton.IsEnabled = active == 0;

        // 任务栏进度：有总量的按比例，没有总量的走不确定态；全失败则显示错误色。
        if (active > 0)
        {
            long doneSum = 0, totalSum = 0;
            bool known = true;
            foreach (var e in _entries.Where(e => !e.IsFinished))
            {
                if (e.Total <= 0) { known = false; break; }
                doneSum += e.Done;
                totalSum += e.Total;
            }
            if (known && totalSum > 0)
                TaskbarProgress.Set(this, TaskbarProgress.State.Normal, (ulong)doneSum, (ulong)totalSum);
            else
                TaskbarProgress.Set(this, TaskbarProgress.State.Indeterminate);
        }
        else if (failed > 0)
        {
            TaskbarProgress.Set(this, TaskbarProgress.State.Error);
        }
        else
        {
            TaskbarProgress.Clear(this);
            // 只有真的显示着才自动收起（自检时窗口没 Show 过）。
            if (_entries.Count > 0 && IsVisible) _autoClose.Start();
        }
    }

    private void OnCancelEntry(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is Entry entry) entry.RequestCancel();
        Refresh();
    }

    // ── 自检用的只读访问器（QueueSelfTest）：只暴露"用户看得到的东西"，
    //    让汇总/关闭规则可以用断言验证，而不是靠肉眼看窗口。──
    internal string Summary() => SummaryText.Text ?? string.Empty;
    internal IReadOnlyList<string> VisibleEntries() =>
        _entries.Select(e => e.KindLabel + " " + e.Path).ToList();
    internal bool CanCloseNow() => CloseButton.IsEnabled;
    internal void CloseForSelfTest()
    {
        _allowClose = true;
        Close();
    }

    private void OnCancelAll(object sender, RoutedEventArgs e)
    {
        foreach (var entry in _entries.Where(x => !x.IsFinished).ToList()) entry.RequestCancel();
        Refresh();
    }

    private void OnClose(object sender, RoutedEventArgs e)
    {
        if (_entries.Any(x => !x.IsFinished)) return;   // 进行中不允许关闭
        _allowClose = true;
        Close();
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        // 关窗不等于取消：正在跑的远程操作**必须**由用户显式取消，
        // 否则一次递归删除会在用户以为"关掉了"的情况下继续改远端。
        if (!_allowClose && _entries.Any(x => !x.IsFinished))
        {
            e.Cancel = true;
            SummaryText.Text = Ui.T("OpQueueCannotClose");
            Activate();
            return;
        }
        TaskbarProgress.Clear(this);
        base.OnClosing(e);
    }

    /// <summary>队列里的一个条目。所有 setter 都通知绑定（进度条/状态/按钮）。</summary>
    public sealed class Entry : INotifyPropertyChanged
    {
        private readonly Action _cancel;
        private string _status;
        private string _cancelLabel;
        private double _percent;
        private bool _isIndeterminate = true;
        private bool _canCancel = true;
        private bool _cancelRequested;

        public string Path { get; }
        public OperationKind Kind { get; }
        public long Done { get; private set; }
        public long Total { get; private set; }
        public bool IsFinished { get; private set; }
        public bool Succeeded { get; private set; }
        public bool WasCancelled { get; private set; }
        public string? Detail { get; private set; }

        public string KindLabel => Kind == OperationKind.Delete ? Ui.T("OpKindDelete") : Ui.T("OpKindChmod");

        // 用全名：本项目同时引用了 WinForms（托盘图标），Brush/Color 会与 System.Drawing 撞名。
        public System.Windows.Media.Brush KindBrush => Kind == OperationKind.Delete
            ? new SolidColorBrush(System.Windows.Media.Color.FromRgb(0xB0, 0x30, 0x2C))    // 删除：偏红，破坏性
            : new SolidColorBrush(System.Windows.Media.Color.FromRgb(0x1F, 0x5C, 0x99));   // 改权限：偏蓝

        public string Status
        {
            get => _status;
            private set => Set(ref _status, value);
        }

        public string CancelLabel
        {
            get => _cancelLabel;
            private set => Set(ref _cancelLabel, value);
        }

        public double Percent
        {
            get => _percent;
            private set => Set(ref _percent, value);
        }

        public bool IsIndeterminate
        {
            get => _isIndeterminate;
            private set => Set(ref _isIndeterminate, value);
        }

        public bool CanCancel
        {
            get => _canCancel;
            private set => Set(ref _canCancel, value);
        }

        internal Entry(string path, OperationKind kind, Action cancel)
        {
            Path = path;
            Kind = kind;
            _cancel = cancel;
            _status = Ui.T("OpQueued");   // 先"排队中"，真正开始连远端时服务层会改成"等待远程连接"
            _cancelLabel = Ui.T("OpCancel");
        }

        /// <summary>回报进度。done/total：total&gt;0 表示已知总量（进度条按比例）。</summary>
        public void Update(long done, long total, string current)
        {
            if (IsFinished) return;
            Done = done;
            Total = total;
            if (total > 0)
            {
                IsIndeterminate = false;
                Percent = Math.Min(100, Math.Max(0, done * 100.0 / total));
                Status = Ui.T(Kind == OperationKind.Delete ? "OpStatusDeleted" : "OpStatusUpdated")
                    .Replace("{0}", done.ToString())
                    .Replace("{1}", total.ToString())
                    .Replace("{2}", current);
            }
            else
            {
                IsIndeterminate = true;
                Status = current;
            }
            Changed?.Invoke();
        }

        public void Complete(bool ok, bool cancelled, string? detail)
        {
            if (IsFinished) return;
            IsFinished = true;
            Succeeded = ok;
            WasCancelled = cancelled;
            Detail = detail;
            CanCancel = false;
            CancelLabel = Ui.T("OpCancel");
            IsIndeterminate = false;
            if (ok)
            {
                Percent = 100;
                Status = Ui.T("OpDone");
            }
            else if (cancelled)
            {
                Status = Ui.T("OpCancelled");
            }
            else
            {
                Status = Ui.T("OpFailed").Replace("{0}", detail ?? "");
                if (Total > 0) Percent = Percent;   // 保留已完成比例，便于看出"改到一半"
            }
            Changed?.Invoke();
        }

        internal void RequestCancel()
        {
            if (IsFinished || _cancelRequested) return;
            _cancelRequested = true;
            CanCancel = false;
            Status = Ui.T("OpCancelling");
            try { _cancel(); } catch (ObjectDisposedException) { }
        }

        /// <summary>条目状态变化时通知窗口刷新汇总（由窗口在 AddEntry 时挂上）。</summary>
        internal Action? Changed;

        public event PropertyChangedEventHandler? PropertyChanged;

        private void Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
        {
            if (EqualityComparer<T>.Default.Equals(field, value)) return;
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }
    }
}
