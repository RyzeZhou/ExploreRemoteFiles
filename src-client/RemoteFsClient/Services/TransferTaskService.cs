using System.Collections.ObjectModel;
using System.ComponentModel;
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

    private long _done;
    private long _total;
    private long _lastBytes;
    private DateTime _lastAt = DateTime.UtcNow;
    private double _speed;          // bytes/second
    private string _status = "running";

    public event PropertyChangedEventHandler? PropertyChanged;

    private void Raise([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    public string DirectionLabel => Direction == "download" ? "↓ 下载" : "↑ 上传";
    public string TargetText => Direction == "download" ? LocalPath : RemotePath;
    public bool IsFinished => _status is "done" or "fail";
    public bool IsFailed => _status == "fail";

    public double Percent => _total > 0
        ? (_done >= _total ? 100.0 : Math.Max(0, _done * 100.0 / _total))
        : (IsFinished ? 100.0 : 0.0);

    public string ProgressText => _total > 0 ? $"{Format(_done)} / {Format(_total)}" : Format(_done);

    public string SpeedText => _status == "running" && _speed > 1 ? Format((long)_speed) + "/s" : "";

    public string StatusText => _status switch
    {
        "done" => "完成",
        "fail" => "失败",
        _ => Percent > 0 ? $"{Percent:0}%" : "…",
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
        if (!ok && !string.IsNullOrWhiteSpace(message)) Failure = message;
        Raise(nameof(Percent));
        Raise(nameof(ProgressText));
        Raise(nameof(SpeedText));
        Raise(nameof(StatusText));
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
            Log("BEGIN server=" + f[3] + " dir=" + f[2] + " name=" + f[4] + " total=" + f[7]);
        else if (f[0] == "E" && f.Length >= 3)
            Log("END status=" + f[2]);
        _dispatcher.BeginInvoke(() =>
        {
            switch (f[0])
            {
                case "B" when f.Length >= 8:
                {
                    var task = new TransferTask
                    {
                        Id = f[1], Direction = f[2], Server = f[3], Name = f[4],
                        LocalPath = f[5], RemotePath = f[6],
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
                    break;
                }
                case "E" when f.Length >= 3:
                {
                    TransferTask? task = Find(f[1]);
                    if (task is null) break;
                    task.Finish(string.Equals(f[2], "done", StringComparison.Ordinal), f.Length > 3 ? f[3] : "");
                    if (Tasks.Count > 0 && Tasks.All(t => t.IsFinished)) AllFinished?.Invoke();
                    break;
                }
            }
        });
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
