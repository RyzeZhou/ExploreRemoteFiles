using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Threading;

namespace RemoteFsClient.Services;

/// <summary>票据里的一个待传项。**只存在服务侧**（票据本身不写路径）。</summary>
public sealed class TicketItem
{
    public string Site { get; set; } = "";
    public string Remote { get; set; } = "";
    public string Name { get; set; } = "";
    public long Size { get; set; }
    public long Mtime { get; set; }
    public bool IsFolder { get; set; }
}

/// <summary>一次 Ctrl+C 对应的传输任务记录。jobId 是票据里的唯一线索。</summary>
public sealed class TicketRecord
{
    public string JobId { get; set; } = "";
    public long CreatedUnix { get; set; }
    public string Direction { get; set; } = "remote2local";
    public List<TicketItem> Items { get; set; } = new();
    /// <summary>上一次真正下载到的目录（用于"目录变了"时询问是否迁移）。</summary>
    public string LastDest { get; set; } = "";
    /// <summary>已经下好的目标全路径（迁移时可搬）。</summary>
    public List<string> DoneFiles { get; set; } = new();
}

/// <summary>
/// 「传输票据」(.erfdl)：票据本身只有 magic/version/jobId；**要传什么、从哪来、原先下到哪**
/// 全部只存在这里（%LOCALAPPDATA%\ExplorerRemoteFs\tickets.json）。这样即使票据被拿走，
/// 也看不出内容；被篡改也无用（jobId 查不到就拒绝）。
/// </summary>
public sealed class TransferTicketService
{
    private const string Magic = "ERFDL";
    private const int Version = 1;

    private readonly Dispatcher _dispatcher;
    private readonly RemoteBridgeService _bridge;
    private readonly object _gate = new();
    private readonly Dictionary<string, TicketRecord> _records = new(StringComparer.Ordinal);
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true };

    private static string StorePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "ExplorerRemoteFs", "tickets.json");

    public TransferTicketService(Dispatcher dispatcher, RemoteBridgeService bridge)
    {
        _dispatcher = dispatcher;
        _bridge = bridge;
        Load();
    }

    private void Load()
    {
        try
        {
            if (!File.Exists(StorePath)) return;
            var list = JsonSerializer.Deserialize<List<TicketRecord>>(File.ReadAllText(StorePath), Json);
            if (list is null) return;
            foreach (var r in list) if (!string.IsNullOrEmpty(r.JobId)) _records[r.JobId] = r;
            Log($"tickets loaded n={_records.Count}");
        }
        catch (Exception ex) { Log($"tickets load failed: {ex.Message}"); }
    }

    private void Save()
    {
        try
        {
            lock (_gate)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(StorePath)!);
                File.WriteAllText(StorePath, JsonSerializer.Serialize(_records.Values.ToList(), Json));
            }
        }
        catch (Exception ex) { Log($"tickets save failed: {ex.Message}"); }
    }

    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(Path.Combine(Path.GetTempPath(), "rfs-tasks.log"),
                DateTime.Now.ToString("HH:mm:ss.fff") + " [ticket] " + message + Environment.NewLine);
        }
        catch { }
    }

    // ── 签发（Ctrl+C 时由扩展 DLL 调用）────────────────────────────────────
    /// <summary>登记一次传输并返回 jobId。票据文本由调用方按 jobId 生成（只有三个字段）。</summary>
    public string CreateTicket(IReadOnlyList<TicketItem> items, string direction = "remote2local")
    {
        var rec = new TicketRecord
        {
            JobId = Guid.NewGuid().ToString("N"),
            CreatedUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
            Direction = direction,
            Items = items.ToList(),
        };
        lock (_gate) _records[rec.JobId] = rec;
        Save();
        Log($"MKTICKET job={rec.JobId} items={rec.Items.Count} dir={direction} first='{(items.Count > 0 ? items[0].Remote : "")}'");
        return rec.JobId;
    }

    /// <summary>票据文本（**只有 magic/version/jobId**，不含任何路径）。</summary>
    public static string BuildTicketText(string jobId)
        => "{\"magic\":\"" + Magic + "\",\"version\":" + Version + ",\"jobId\":\"" + jobId + "\"}";

    /// <summary>票据文件名：ERF_&lt;来源站点名&gt;_&lt;jobId&gt;.erfdl（用户定的命名）。</summary>
    public static string BuildTicketFileName(string site, string jobId)
    {
        string safe = new string((site ?? "").Select(c => Path.GetInvalidFileNameChars().Contains(c) ? '_' : c).ToArray());
        if (string.IsNullOrWhiteSpace(safe)) safe = "site";
        return $"ERF_{safe}_{jobId}.erfdl";
    }

    // ── 双击票据（RemoteFsClient.exe --open-ticket <path>）──────────────────
    /// <summary>解析并开始下载。目标 = **票据当前所在目录**。返回 "OK ..." / "FAIL: ..."。</summary>
    public string OpenTicket(string ticketPath)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(ticketPath) || !File.Exists(ticketPath))
                return "FAIL: 票据文件不存在";
            string text = File.ReadAllText(ticketPath);
            string? jobId = ParseJobId(text);
            if (string.IsNullOrEmpty(jobId))
                return "FAIL: 这不是易远传的传输票据";
            TicketRecord? rec;
            lock (_gate) _records.TryGetValue(jobId, out rec);
            if (rec is null)
                return "FAIL: 票据无法识别（可能是别台机器生成的，或本机任务记录已丢失；请重新复制一次）";

            string? dest = Path.GetDirectoryName(Path.GetFullPath(ticketPath));
            if (string.IsNullOrEmpty(dest)) return "FAIL: 无法确定目标目录";
            Directory.CreateDirectory(dest);

            // 「目录变了」——问用户：迁移到新目录 / 重新下载（忽略原目录）/ 取消
            string previous = rec.LastDest;
            bool destChanged = previous.Length > 0 && !string.Equals(previous, dest, StringComparison.OrdinalIgnoreCase);
            if (destChanged)
            {
                var choice = AskOnUi(
                    $"该任务此前下载到：\n{previous}\n\n现在票据在：\n{dest}\n\n要把已下载的文件**迁移**到新目录，\n还是在新目录**重新下载**（忽略原目录）？",
                    "传输票据：目录已改变", MessageBoxButton.YesNoCancel);
                if (choice == MessageBoxResult.Cancel) return "FAIL: 用户取消";
                if (choice == MessageBoxResult.Yes) MigrateDoneFiles(rec, dest);
            }
            else if (previous.Length > 0)
            {
                var again = AskOnUi(
                    $"该任务此前已下载到：\n{dest}\n\n要重新下载一次吗？",
                    "传输票据", MessageBoxButton.YesNo);
                if (again != MessageBoxResult.Yes) return "FAIL: 用户取消";
            }

            // 冲突检查：同名 / 大小写同名 —— 只有冲突才问用户
            var plan = new List<(TicketItem item, string target, bool overwrite)>();
            foreach (var item in rec.Items)
            {
                string target = Path.Combine(dest, item.Name);
                string? existing = FindConflict(dest, item.Name);
                if (existing is null) { plan.Add((item, target, true)); continue; }
                var c = AskOnUi(
                    $"目标目录已存在同名项：\n{existing}\n\n要覆盖它，还是保留两者（重命名新文件）？",
                    "传输票据：命名冲突", MessageBoxButton.YesNoCancel);
                if (c == MessageBoxResult.Cancel) return "FAIL: 用户取消";
                if (c == MessageBoxResult.Yes) plan.Add((item, target, true));
                else plan.Add((item, UniquePath(dest, item.Name), true));
            }

            string batchId = "ticket-" + rec.JobId.Substring(0, 8);
            int started = 0;
            foreach (var (item, target, overwrite) in plan)
            {
                _bridge.StartTicketDownload(item.Site, item.Remote, target, item.IsFolder, batchId);
                started++;
            }
            rec.LastDest = dest;
            rec.DoneFiles = plan.Select(p => p.target).ToList();
            Save();
            Log($"OPEN-TICKET job={rec.JobId} dest='{dest}' items={started} changed={destChanged}");
            return $"OK 已开始 {started} 项下载到 {dest}";
        }
        catch (Exception ex)
        {
            Log($"OPEN-TICKET failed: {ex}");
            return "FAIL: " + ex.Message;
        }
    }

    private static string? ParseJobId(string text)
    {
        try
        {
            using var doc = JsonDocument.Parse(text);
            var root = doc.RootElement;
            if (!root.TryGetProperty("magic", out var magic) || magic.GetString() != Magic) return null;
            if (!root.TryGetProperty("version", out var ver) || ver.GetInt32() != Version) return null;
            return root.TryGetProperty("jobId", out var id) ? id.GetString() : null;
        }
        catch { return null; }
    }

    private static string? FindConflict(string dest, string name)
    {
        try
        {
            if (File.Exists(Path.Combine(dest, name)) || Directory.Exists(Path.Combine(dest, name)))
                return Path.Combine(dest, name);
            // 大小写同名（Windows 上不能共存）
            foreach (var p in Directory.EnumerateFileSystemEntries(dest))
                if (string.Equals(Path.GetFileName(p), name, StringComparison.OrdinalIgnoreCase))
                    return p;
        }
        catch { }
        return null;
    }

    private static string UniquePath(string dest, string name)
    {
        string stem = Path.GetFileNameWithoutExtension(name);
        string ext = Path.GetExtension(name);
        for (int i = 2; i < 1000; i++)
        {
            string candidate = Path.Combine(dest, $"{stem} ({i}){ext}");
            if (!File.Exists(candidate) && !Directory.Exists(candidate)) return candidate;
        }
        return Path.Combine(dest, name);
    }

    private static void MigrateDoneFiles(TicketRecord rec, string newDest)
    {
        var moved = new List<string>();
        foreach (var old in rec.DoneFiles)
        {
            try
            {
                if (!File.Exists(old)) { continue; }
                string target = Path.Combine(newDest, Path.GetFileName(old));
                if (File.Exists(target)) { continue; }
                File.Move(old, target);
                moved.Add(target);
            }
            catch (Exception ex) { Log($"migrate failed '{old}': {ex.Message}"); }
        }
        rec.DoneFiles = moved;
        Log($"migrate done moved={moved.Count} -> '{newDest}'");
    }

    private MessageBoxResult AskOnUi(string message, string title, MessageBoxButton buttons)
    {
        try
        {
            if (_dispatcher.CheckAccess())
                return MessageBox.Show(message, title, buttons, MessageBoxImage.Question);
            return _dispatcher.Invoke(() => MessageBox.Show(message, title, buttons, MessageBoxImage.Question));
        }
        catch { return MessageBoxResult.Cancel; }
    }
}
