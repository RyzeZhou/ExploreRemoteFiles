using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Data.Sqlite;

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
/// 只存在**数据库**里（<c>&lt;MetadataCachePath&gt;\erf-cache.db</c> 的 tickets/ticket_items 两张表，
/// 与扩展 DLL 的目录缓存共用一个库）。这样即使票据被拿走，也看不出内容；被篡改也无用
/// （jobId 查不到就拒绝）。
/// </summary>
public sealed class TransferTicketService
{
    private const string Magic = "ERFDL";
    private const int Version = 1;

    private readonly Dispatcher _dispatcher;
    private readonly RemoteBridgeService _bridge;
    private readonly string _dbPath;
    private readonly string _connectionString;
    private readonly object _gate = new();

    public TransferTicketService(Dispatcher dispatcher, RemoteBridgeService bridge)
    {
        _dispatcher = dispatcher;
        _bridge = bridge;
        // 与扩展 DLL 读的是**同一个注册表值**，因此两边用的是同一个库文件。
        string dir = AppSettings.Load().MetadataCachePath;
        if (string.IsNullOrWhiteSpace(dir)) dir = AppSettings.DefaultMetadataCachePath;
        try { Directory.CreateDirectory(dir); } catch { }
        _dbPath = Path.Combine(dir, "erf-cache.db");
        _connectionString = new SqliteConnectionStringBuilder
        {
            DataSource = _dbPath,
            Mode = SqliteOpenMode.ReadWriteCreate,
        }.ToString();
        Init();
    }

    private SqliteConnection Open()
    {
        var connection = new SqliteConnection(_connectionString);
        connection.Open();
        using var pragma = connection.CreateCommand();
        pragma.CommandText = "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=4000;";
        pragma.ExecuteNonQuery();
        return connection;
    }

    private void Init()
    {
        try
        {
            lock (_gate)
            {
                using var connection = Open();
                using var cmd = connection.CreateCommand();
                // 与 DLL 侧 kSchema 完全一致（谁先跑谁建表）。
                cmd.CommandText =
                    "CREATE TABLE IF NOT EXISTS dir_cache(" +
                    "  site TEXT NOT NULL, path TEXT NOT NULL, tick INTEGER NOT NULL," +
                    "  items BLOB NOT NULL, PRIMARY KEY(site, path));" +
                    "CREATE TABLE IF NOT EXISTS tickets(" +
                    "  job_id TEXT PRIMARY KEY, created INTEGER NOT NULL, direction TEXT NOT NULL," +
                    "  last_dest TEXT NOT NULL DEFAULT '', done_files TEXT NOT NULL DEFAULT '');" +
                    "CREATE TABLE IF NOT EXISTS ticket_items(" +
                    "  job_id TEXT NOT NULL, idx INTEGER NOT NULL, site TEXT NOT NULL, remote TEXT NOT NULL," +
                    "  name TEXT NOT NULL, size INTEGER NOT NULL, mtime INTEGER NOT NULL," +
                    "  is_folder INTEGER NOT NULL, PRIMARY KEY(job_id, idx));";
                cmd.ExecuteNonQuery();
                using var stats = connection.CreateCommand();
                stats.CommandText = "SELECT (SELECT COUNT(*) FROM dir_cache), (SELECT COUNT(*) FROM tickets)";
                using var reader = stats.ExecuteReader();
                if (reader.Read())
                    Log($"tickets db ready '{_dbPath}' dir_cache={reader.GetInt64(0)} tickets={reader.GetInt64(1)}");
            }
            MigrateLegacyJson();
        }
        catch (Exception ex) { Log($"tickets db init failed: {ex.Message}"); }
    }

    /// <summary>把旧版 tickets.json 一次性搬进数据库（搬完改名，不再看它）。</summary>
    private void MigrateLegacyJson()
    {
        try
        {
            string legacy = Path.Combine(Path.GetDirectoryName(_dbPath)!, "tickets.json");
            if (!File.Exists(legacy)) return;
            var list = JsonSerializer.Deserialize<List<TicketRecord>>(File.ReadAllText(legacy));
            int imported = 0;
            foreach (var rec in list ?? new List<TicketRecord>())
            {
                if (string.IsNullOrEmpty(rec.JobId) || rec.Items.Count == 0) continue;
                if (GetRecord(rec.JobId) is not null) continue;
                ImportRecord(rec);
                imported++;
            }
            File.Move(legacy, legacy + ".migrated", overwrite: true);
            Log($"legacy tickets.json imported n={imported}");
        }
        catch (Exception ex) { Log($"legacy tickets import failed: {ex.Message}"); }
    }

    private void ImportRecord(TicketRecord rec)
    {
        lock (_gate)
        {
            using var connection = Open();
            using var tx = connection.BeginTransaction();
            InsertTicketRow(connection, tx, rec);
            InsertItems(connection, tx, rec);
            tx.Commit();
        }
    }

    private static void InsertTicketRow(SqliteConnection c, SqliteTransaction tx, TicketRecord rec)
    {
        using var cmd = c.CreateCommand();
        cmd.Transaction = tx;
        cmd.CommandText = "INSERT OR REPLACE INTO tickets(job_id, created, direction, last_dest, done_files) " +
                          "VALUES($id, $created, $dir, $dest, $done)";
        cmd.Parameters.AddWithValue("$id", rec.JobId);
        cmd.Parameters.AddWithValue("$created", rec.CreatedUnix);
        cmd.Parameters.AddWithValue("$dir", rec.Direction);
        cmd.Parameters.AddWithValue("$dest", rec.LastDest ?? "");
        cmd.Parameters.AddWithValue("$done", JsonSerializer.Serialize(rec.DoneFiles ?? new List<string>()));
        cmd.ExecuteNonQuery();
    }

    private static void InsertItems(SqliteConnection c, SqliteTransaction tx, TicketRecord rec)
    {
        using var cmd = c.CreateCommand();
        cmd.Transaction = tx;
        cmd.CommandText = "INSERT OR REPLACE INTO ticket_items(job_id, idx, site, remote, name, size, mtime, is_folder) " +
                          "VALUES($id, $idx, $site, $remote, $name, $size, $mtime, $folder)";
        var pId = cmd.Parameters.Add("$id", SqliteType.Text);
        var pIdx = cmd.Parameters.Add("$idx", SqliteType.Integer);
        var pSite = cmd.Parameters.Add("$site", SqliteType.Text);
        var pRemote = cmd.Parameters.Add("$remote", SqliteType.Text);
        var pName = cmd.Parameters.Add("$name", SqliteType.Text);
        var pSize = cmd.Parameters.Add("$size", SqliteType.Integer);
        var pMtime = cmd.Parameters.Add("$mtime", SqliteType.Integer);
        var pFolder = cmd.Parameters.Add("$folder", SqliteType.Integer);
        cmd.Prepare();
        for (int i = 0; i < rec.Items.Count; i++)
        {
            var it = rec.Items[i];
            pId.Value = rec.JobId; pIdx.Value = i;
            pSite.Value = it.Site ?? ""; pRemote.Value = it.Remote ?? ""; pName.Value = it.Name ?? "";
            pSize.Value = it.Size; pMtime.Value = it.Mtime; pFolder.Value = it.IsFolder ? 1 : 0;
            cmd.ExecuteNonQuery();
        }
    }

    private TicketRecord? GetRecord(string jobId)
    {
        try
        {
            lock (_gate)
            {
                using var connection = Open();
                var rec = new TicketRecord { JobId = jobId };
                using (var cmd = connection.CreateCommand())
                {
                    cmd.CommandText = "SELECT created, direction, last_dest, done_files FROM tickets WHERE job_id=$id";
                    cmd.Parameters.AddWithValue("$id", jobId);
                    using var reader = cmd.ExecuteReader();
                    if (!reader.Read()) return null;
                    rec.CreatedUnix = reader.GetInt64(0);
                    rec.Direction = reader.GetString(1);
                    rec.LastDest = reader.GetString(2);
                    try { rec.DoneFiles = JsonSerializer.Deserialize<List<string>>(reader.GetString(3)) ?? new(); }
                    catch { rec.DoneFiles = new(); }
                }
                using (var cmd = connection.CreateCommand())
                {
                    cmd.CommandText = "SELECT site, remote, name, size, mtime, is_folder FROM ticket_items " +
                                      "WHERE job_id=$id ORDER BY idx";
                    cmd.Parameters.AddWithValue("$id", jobId);
                    using var reader = cmd.ExecuteReader();
                    while (reader.Read())
                    {
                        rec.Items.Add(new TicketItem
                        {
                            Site = reader.GetString(0), Remote = reader.GetString(1), Name = reader.GetString(2),
                            Size = reader.GetInt64(3), Mtime = reader.GetInt64(4), IsFolder = reader.GetInt64(5) != 0,
                        });
                    }
                }
                return rec.Items.Count > 0 ? rec : null;
            }
        }
        catch (Exception ex) { Log($"tickets read failed job={jobId}: {ex.Message}"); return null; }
    }

    private void SaveState(TicketRecord rec)
    {
        try
        {
            lock (_gate)
            {
                using var connection = Open();
                using var cmd = connection.CreateCommand();
                cmd.CommandText = "UPDATE tickets SET last_dest=$dest, done_files=$done WHERE job_id=$id";
                cmd.Parameters.AddWithValue("$dest", rec.LastDest ?? "");
                cmd.Parameters.AddWithValue("$done", JsonSerializer.Serialize(rec.DoneFiles ?? new List<string>()));
                cmd.Parameters.AddWithValue("$id", rec.JobId);
                cmd.ExecuteNonQuery();
            }
        }
        catch (Exception ex) { Log($"tickets save failed job={rec.JobId}: {ex.Message}"); }
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
        lock (_gate)
        {
            using var connection = Open();
            using var tx = connection.BeginTransaction();
            InsertTicketRow(connection, tx, rec);
            InsertItems(connection, tx, rec);
            tx.Commit();
        }
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
            TicketRecord? rec = GetRecord(jobId);
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
                var choice = Ask("传输票据：目录已改变",
                    $"该任务此前下载到：\n{previous}\n\n现在票据在：\n{dest}\n\n已下载的文件怎么处理？",
                    "迁移到新目录", "在新目录重新下载", "取消");
                if (choice == TicketChoice.Cancel) return "FAIL: 用户取消";
                if (choice == TicketChoice.Primary) MigrateDoneFiles(rec, dest);
            }
            else if (previous.Length > 0)
            {
                var again = Ask("传输票据",
                    $"该任务此前已下载到：\n{dest}\n\n要重新下载一次吗？",
                    "重新下载", null, "取消");
                if (again != TicketChoice.Primary) return "FAIL: 用户取消";
            }

            // 冲突检查：同名 / 大小写同名 —— 只有冲突才问用户
            var plan = new List<(TicketItem item, string target)>();
            foreach (var item in rec.Items)
            {
                string target = Path.Combine(dest, item.Name);
                string? existing = FindConflict(dest, item.Name);
                if (existing is null) { plan.Add((item, target)); continue; }
                var c = Ask("传输票据：命名冲突",
                    $"目标目录已存在同名项：\n{existing}\n\n怎么处理？",
                    "覆盖它", "保留两者（新的加序号）", "取消");
                if (c == TicketChoice.Cancel) return "FAIL: 用户取消";
                if (c == TicketChoice.Primary) plan.Add((item, target));
                else plan.Add((item, UniquePath(dest, item.Name)));
            }

            string batchId = "ticket-" + rec.JobId.Substring(0, Math.Min(8, rec.JobId.Length));
            foreach (var (item, target) in plan)
                _bridge.StartTicketDownload(item.Site, item.Remote, target, item.IsFolder, batchId);
            rec.LastDest = dest;
            rec.DoneFiles = plan.Select(p => p.target).ToList();
            SaveState(rec);
            Log($"OPEN-TICKET job={rec.JobId} dest='{dest}' items={plan.Count} changed={destChanged}");
            return $"OK 已开始 {plan.Count} 项下载到 {dest}";
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
                if (!File.Exists(old)) continue;
                string target = Path.Combine(newDest, Path.GetFileName(old));
                if (File.Exists(target)) continue;
                File.Move(old, target);
                moved.Add(target);
            }
            catch (Exception ex) { Log($"migrate failed '{old}': {ex.Message}"); }
        }
        rec.DoneFiles = moved;
        Log($"migrate done moved={moved.Count} -> '{newDest}'");
    }

    /// <summary>
    /// 走 UI 线程弹决策框。**按钮文字就是选项本身**（不是"是/否/取消"），
    /// <paramref name="secondary"/> 传 null 就只显示两个选项。
    /// </summary>
    private TicketChoice Ask(string title, string message, string primary, string? secondary = null, string? cancel = null)
    {
        try
        {
            return _dispatcher.Invoke(() =>
            {
                var window = new TicketChoiceWindow(title, message, primary, secondary, cancel);
                window.ShowDialog();
                return window.Choice;
            });
        }
        catch { return TicketChoice.Cancel; }
    }
}
