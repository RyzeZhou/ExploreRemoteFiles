using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.CompilerServices;
using System.Text;
using System.Windows.Threading;

namespace RemoteFsClient.Services;

/// <summary>One remote transfer (upload/download), shown in the task window.</summary>
public sealed class TransferTask : INotifyPropertyChanged
{
    public string Id { get; init; } = "";
    public string Direction { get; init; } = "upload";     // upload | download
    /// <summary>Saved-site (server) name this transfer belongs to.</summary>
    public string Server { get; init; } = "";
    public string Name { get; init; } = "";
    public string LocalPath { get; init; } = "";
    public string RemotePath { get; init; } = "";
    /// <summary>CLI process performing the transfer (0 when unknown). Cancel kills it.</summary>
    public int Pid { get; init; }

    private long _done;
    private long _total;
    private long _lastBytes;
    private DateTime _lastAt = DateTime.UtcNow;
    private double _speed;          // bytes/second
    private bool _paused;
    private string _status = "running";
    private string _currentFile = "";

    public event PropertyChangedEventHandler? PropertyChanged;

    private void Raise([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    public string DirectionLabel => Direction == "download" ? "↓ 下载" : "↑ 上传";
    public string TargetText => Direction == "download" ? LocalPath : RemotePath;
    public bool IsFinished => _status is "done" or "fail" or "cancel";
    public bool IsFailed => _status == "fail";
    public bool IsPaused => _paused;

    /// <summary>For a multi-file (batch) job, the file currently being transferred. Empty for single-file jobs and after completion.</summary>
    public string CurrentFile
    {
        get => _currentFile;
        set { _currentFile = value ?? ""; Raise(nameof(CurrentFileText)); }
    }
    public string CurrentFileText => _currentFile;
    public string PauseLabel => _paused ? (Ui.IsEnglish ? "Resume" : "继续") : (Ui.IsEnglish ? "Pause" : "暂停");
    public string CancelLabel => Ui.IsEnglish ? "Cancel" : "取消";

    /// <summary>Reflects the pause gate state (the transfer thread blocks while paused).</summary>
    public void SetPaused(bool paused)
    {
        _paused = paused;
        Raise(nameof(IsPaused));
        Raise(nameof(PauseLabel));
        Raise(nameof(StatusText));
    }

    /// <summary>Cancel = the CLI process was killed; the E frame will never arrive.</summary>
    public void MarkCancelled()
    {
        _status = "cancel";
        _speed = 0;
        Raise(nameof(Percent));
        Raise(nameof(SpeedText));
        Raise(nameof(StatusText));
    }

    public double Percent => _total > 0
        ? (_done >= _total ? 100.0 : Math.Max(0, _done * 100.0 / _total))
        : (IsFinished ? 100.0 : 0.0);

    public string ProgressText => _total > 0 ? $"{Format(_done)} / {Format(_total)}" : Format(_done);

    public string SpeedText => _status == "running" && _speed > 1 ? Format((long)_speed) + "/s" : "";

    public string StatusText => _status switch
    {
        "done" => "完成",
        "fail" => "失败",
        "cancel" => "已取消",
        _ => _paused ? "已暂停" : (Percent > 0 ? $"{Percent:0}%" : "…"),
    };

    public string Failure { get; private set; } = "";

    public void Update(long done, long total)
    {
        DateTime now = DateTime.UtcNow;
        double dt = (now - _lastAt).TotalSeconds;
        if (dt >= 0.4 && done > _lastBytes)
        {
            _speed = (done - _lastBytes) / dt;
            _lastBytes = done;
            _lastAt = now;
        }
        _done = done;
        if (total > 0) _total = total;
        Raise(nameof(Percent));
        Raise(nameof(ProgressText));
        Raise(nameof(SpeedText));
        Raise(nameof(StatusText));
    }

    public void Finish(bool ok, string message)
    {
        _status = ok ? "done" : "fail";
        if (ok && _total > 0) _done = _total;
        _speed = 0;
        _currentFile = "";
        if (!ok && !string.IsNullOrWhiteSpace(message)) Failure = message;
        Raise(nameof(Percent));
        Raise(nameof(ProgressText));
        Raise(nameof(SpeedText));
        Raise(nameof(StatusText));
        Raise(nameof(CurrentFileText));
    }

    internal static string Format(long bytes)
    {
        string[] unit = { "B", "KB", "MB", "GB", "TB" };
        double value = bytes;
        int i = 0;
        while (value >= 1024 && i < unit.Length - 1) { value /= 1024; i++; }
        return i == 0 ? $"{value:0} {unit[i]}" : $"{value:0.0} {unit[i]}";
    }
}

/// <summary>
/// Receives transfer jobs from the CLI (which performs the actual transfer)
/// over a dedicated named pipe and exposes them as an observable task list.
/// The CLI drops events silently when the service is not running, so this is
/// a pure UI-side observer: it never participates in the transfer itself.
/// </summary>
public sealed class TransferTaskService
{
    private const string PipeName = "ExplorerRemoteFs.Tasks.v1";
    private readonly Dispatcher _dispatcher;
    private CancellationTokenSource? _cts;

    // ---- batch bookkeeping -------------------------------------------------
    // A "batch" is one USER-level operation. Copying a folder runs one CLI job
    // per file and the shell launches them with small gaps, so "every listed
    // task finished" happens after EACH file; notifying there would fire once
    // per file (the reported spam). The batch therefore stays open across a
    // grace window and the notification is emitted once, when it really drains.
    private bool _batchActive;
    private int _batchDone, _batchFailed, _batchCancelled;
    private DispatcherTimer? _batchTimer;
    private static readonly TimeSpan BatchGrace = TimeSpan.FromSeconds(1.5);

    /// <summary>Completed / failed / cancelled jobs of the batch that just
    /// drained (valid inside the AllFinished handler).</summary>
    public int BatchDone => _batchDone;
    public int BatchFailed => _batchFailed;
    public int BatchCancelled => _batchCancelled;

    public ObservableCollection<TransferTask> Tasks { get; } = new();

    /// <summary>Raised (on the UI thread) when a new job begins.</summary>
    public event Action? JobStarted;

    /// <summary>Raised (on the UI thread) when every listed job has finished.</summary>
    public event Action? AllFinished;

    public TransferTaskService(Dispatcher dispatcher) => _dispatcher = dispatcher;

    public void Start()
    {
        _cts = new CancellationTokenSource();
        _ = Task.Run(() => ListenAsync(_cts.Token));
    }

    public void Stop()
    {
        try { _cts?.Cancel(); } catch { }
    }

    private async Task ListenAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                using var server = new NamedPipeServerStream(
                    PipeName, PipeDirection.In, NamedPipeServerStream.MaxAllowedServerInstances,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
                await server.WaitForConnectionAsync(ct);
                using var reader = new StreamReader(server, Encoding.UTF8);
                string? line;
                while (!ct.IsCancellationRequested && (line = await reader.ReadLineAsync(ct)) is not null)
                    Handle(line);
            }
            catch (OperationCanceledException) { break; }
            catch
            {
                try { await Task.Delay(500, ct); } catch { break; }
            }
        }
    }

    /// <summary>Diagnostic log (%TEMP%/rfs-tasks.log): job begin/end only, so a
    /// missing/working progress pipe can be verified without a debugger.</summary>
    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(Path.Combine(Path.GetTempPath(), "rfs-tasks.log"),
                DateTime.Now.ToString("HH:mm:ss.fff") + " " + message + Environment.NewLine);
        }
        catch { }
    }

    private void Handle(string line)
    {
        string[] f = line.Split('\t');
        if (f.Length < 3) return;
        if (f[0] == "B" && f.Length >= 8)
            Log("BEGIN id=" + f[1] + " server=" + f[3] + " dir=" + f[2] + " name=" + f[4] + " total=" + f[7] + " pid=" + (f.Length > 8 ? f[8] : "-"));
        else if (f[0] == "E" && f.Length >= 3)
            Log("END status=" + f[2]);
        _dispatcher.BeginInvoke(() =>
        {
            switch (f[0])
            {
                case "B" when f.Length >= 8:
                {
                    _batchTimer?.Stop();     // still the same user operation
                    if (!_batchActive)
                    {
                        _batchActive = true; _batchDone = _batchFailed = _batchCancelled = 0;
                        Log("BATCH BEGIN");
                    }
                    var task = new TransferTask
                    {
                        Id = f[1], Direction = f[2], Server = f[3], Name = f[4],
                        LocalPath = f[5], RemotePath = f[6],
                        Pid = (f.Length > 8 && int.TryParse(f[8], out int pid)) ? pid : 0,
                    };
                    if (long.TryParse(f[7], out long total) && total > 0) task.Update(0, total);
                    Tasks.Insert(0, task);      // newest first
                    JobStarted?.Invoke();
                    break;
                }
                case "P" when f.Length >= 4:
                {
                    TransferTask? task = Find(f[1]);
                    if (task is null) break;
                    if (long.TryParse(f[2], out long done) && long.TryParse(f[3], out long total))
                        task.Update(done, total);
                    if (f.Length >= 5) task.CurrentFile = f[4];
                    break;
                }
                case "E" when f.Length >= 3:
                {
                    TransferTask? task = Find(f[1]);
                    if (task is null) break;
                    bool ok = string.Equals(f[2], "done", StringComparison.Ordinal);
                    if (!_batchActive) { _batchActive = true; _batchDone = _batchFailed = _batchCancelled = 0; }
                    if (ok) _batchDone++; else _batchFailed++;
                    task.Finish(ok, f.Length > 3 ? f[3] : "");
                    MaybeEndBatch();
                    break;
                }
            }
        });
    }

    /// <summary>Named pause gate created by the CLI for a job.
    /// Signaled = running, reset = paused.</summary>
    private const string GatePrefix = @"Local\ExplorerRemoteFs.Job.";

    /// <summary>Pause/resume by flipping the job's named gate; the CLI blocks in
    /// its progress callback while the gate is reset, so the transfer thread
    /// stops reading/writing without tearing the connection down.</summary>
    public void TogglePause(TransferTask task)
    {
        if (task is null || task.IsFinished) return;
        try
        {
            using EventWaitHandle gate = EventWaitHandle.OpenExisting(GatePrefix + task.Id + ".Gate");
            if (task.IsPaused) { gate.Set(); task.SetPaused(false); }
            else { gate.Reset(); task.SetPaused(true); }
        }
        catch
        {
            // No gate (old CLI, or the job already ended): nothing to toggle.
        }
    }

    /// <summary>Cancel = terminate the CLI process performing the transfer.</summary>
    public void Cancel(TransferTask task)
    {
        if (task is null || task.IsFinished) return;
        try { if (task.Pid > 0) Process.GetProcessById(task.Pid).Kill(); } catch { }
        if (!_batchActive) { _batchActive = true; _batchDone = _batchFailed = _batchCancelled = 0; }
        _batchCancelled++;
        task.MarkCancelled();
        MaybeEndBatch();
    }

    /// <summary>Everything listed is finished - but hold the batch open for a
    /// grace window: the shell starts the next file of a multi-file copy shortly
    /// after the previous one ends, so declaring the batch done immediately
    /// would notify once per file.</summary>
    private void MaybeEndBatch()
    {
        if (Tasks.Count == 0 || !Tasks.All(t => t.IsFinished)) return;
        _batchTimer ??= new DispatcherTimer(DispatcherPriority.Normal, _dispatcher)
        {
            Interval = BatchGrace,
        };
        _batchTimer.Tick -= OnBatchGraceElapsed;
        _batchTimer.Tick += OnBatchGraceElapsed;
        _batchTimer.Stop();
        _batchTimer.Start();
    }

    private void OnBatchGraceElapsed(object? sender, EventArgs e)
    {
        _batchTimer?.Stop();
        if (RunningCount > 0) return;   // a straggler arrived inside the window
        Log("BATCH END done=" + _batchDone + " failed=" + _batchFailed + " cancelled=" + _batchCancelled);
        AllFinished?.Invoke();
        _batchActive = false;           // the next job opens a fresh batch
    }

    /// <summary>Number of jobs that have not finished yet.</summary>
    public int RunningCount
    {
        get
        {
            int n = 0;
            foreach (TransferTask t in Tasks) if (!t.IsFinished) n++;
            return n;
        }
    }

    private TransferTask? Find(string id)
    {
        foreach (TransferTask t in Tasks)
            if (t.Id == id) return t;
        return null;
    }
}
