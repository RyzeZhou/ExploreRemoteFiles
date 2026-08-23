using ExplorerRemoteFs.Config;
using ExplorerRemoteFs.Providers;

// ExplorerRemoteFs.Cli — 冒烟测试工具：不依赖 Explorer 验证 Provider 层完整链路。
// Usage:
//   dotnet run -- list <name> [path]      列出目录（连接复用池）
//   dotnet run -- add <name> <type> <host> <user> <pass> [port]
//   dotnet run -- test <name>             连接测试
//   dotnet run -- show                    显示已配置连接

var argv = Environment.GetCommandLineArgs().Skip(1).ToArray();
if (argv.Length == 0) { PrintUsage(); return; }

switch (argv[0].ToLowerInvariant())
{
    case "list":
        CmdList(argv);
        break;
    case "add":
        CmdAdd(argv);
        break;
    case "test":
        CmdTest(argv);
        break;
    case "show":
        CmdShow();
        break;
    case "pipe":
        CmdPipe(argv);
        break;
    case "delete":
        CmdDelete(argv);
        break;
    case "rename":
        CmdRename(argv);
        break;
    case "mkdir":
        CmdMkdir(argv);
        break;
    default:
        PrintUsage();
        break;
}

static void PrintUsage()
{
    Console.WriteLine("""
        ExplorerRemoteFs.Cli — Provider 冒烟测试
          list <name> [path]    List directory (path default = StartPath or "/")
          add <name> <type> <host> <user> <pass> [port]
          test <name>           Connection test
          show                  Show configured connections
          pipe <name> <path>    Pipe-format listing (tab-separated, for C++ bridge)
          delete <name> <path>  Delete remote file/dir
          rename <name> <old> <new>  Rename/move remote item
          mkdir <name> <path>   Create remote directory
        """);
}

static ConnectionConfig Find(string name)
{
    var conn = ConnectionStore.Load().FirstOrDefault(c => c.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
    if (conn is null)
    {
        Console.Error.WriteLine($"Connection '{name}' not found. Use: add ...");
        Environment.Exit(1);
    }
    return conn;
}

static void CmdList(string[] args)
{
    if (args.Length < 2) { PrintUsage(); return; }
    var conn = Find(args[1]);
    var path = args.Length > 2 ? args[2] : conn.StartPath;

    try
    {
        var fs = ProviderFactory.Get(conn);
        Console.WriteLine($"== {conn.Name} ({conn.Type}) {path} ==");
        foreach (var e in fs.List(path))
        {
            var type = e.IsDirectory ? "D" : e.IsSymlink ? "L" : "F";
            Console.WriteLine($"{type} {e.ModeDisplay,-10} {e.OwnerDisplay,-8} {e.GroupDisplay,-8} {e.SizeDisplay,-10} {e.ModifiedDisplay,-16} {e.Name}");
        }
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"FAIL: {ex.Message}");
        ProviderFactory.Invalidate(conn.Name);
        Environment.Exit(2);
    }
}

static void CmdTest(string[] args)
{
    if (args.Length < 2) { PrintUsage(); return; }
    var conn = Find(args[1]);
    try
    {
        var fs = ProviderFactory.Get(conn);
        Console.WriteLine($"OK: connected to {fs.DisplayName}");
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"FAIL: {ex.Message}");
        ProviderFactory.Invalidate(conn.Name);
        Environment.Exit(2);
    }
}

static void CmdAdd(string[] args)
{
    if (args.Length < 6) { PrintUsage(); return; }
    var conns = ConnectionStore.Load();
    conns.RemoveAll(c => c.Name.Equals(args[1], StringComparison.OrdinalIgnoreCase));
    var conn = new ConnectionConfig
    {
        Name = args[1],
        Type = args[2].ToLowerInvariant(),
        Host = args[3],
        Username = args[4],
        Password = args[5]
    };
    if (args.Length > 6 && int.TryParse(args[6], out var port)) conn.Port = port;
    conns.Add(conn);
    ConnectionStore.Save(conns);
    Console.WriteLine($"Saved '{conn.Name}' to connections.json");
}

static void CmdShow()
{
    foreach (var c in ConnectionStore.Load())
        Console.WriteLine($"{c.Name,-16} {c.Type,-6} {c.Username}@{c.Host}:{c.EffectivePort} start={c.StartPath}");
}

// Pipe 格式（供 C++ Shell 桥消费）：每行一个条目，tab 分隔，字段顺序固定：
//   ITEM\tmode\tmtimeUnix\tsize\towner\tgroup\tisFolder\tisSymlink\tremotePath\tname
// mode 为 Unix 风格字符串（-rw-r--r--），mtime 为 Unix epoch 秒。
static void CmdPipe(string[] args)
{
    if (args.Length < 2) { PrintUsage(); return; }
    var conn = Find(args[1]);
    var path = args.Length > 2 ? args[2] : conn.StartPath;
    if (string.IsNullOrEmpty(path)) path = "/";

    try
    {
        var fs = ProviderFactory.Get(conn);
        foreach (var e in fs.List(path))
        {
            var mode = e.ModeDisplay ?? (e.IsDirectory ? "drwxr-xr-x" : "-rw-r--r--");
            var mtime = e.LastWriteTime.HasValue
                ? new DateTimeOffset(e.LastWriteTime.Value.ToUniversalTime()).ToUnixTimeSeconds().ToString()
                : "0";
            Console.WriteLine($"ITEM\t{mode}\t{mtime}\t{e.Size}\t{e.OwnerDisplay}\t{e.GroupDisplay}\t{(e.IsDirectory ? 1 : 0)}\t{(e.IsSymlink ? 1 : 0)}\t{path}\t{e.Name}");
        }
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"FAIL: {ex.Message}");
        Console.Error.WriteLine($"DETAIL: {ex.GetType().FullName}");
        Console.Error.WriteLine($"INNER: {ex.InnerException?.GetType().FullName}: {ex.InnerException?.Message}");
        ProviderFactory.Invalidate(conn.Name);
        Environment.Exit(2);
    }
}

static void CmdDelete(string[] args)
{
    if (args.Length < 3) { PrintUsage(); return; }
    var conn = Find(args[1]);
    try
    {
        var fs = ProviderFactory.Get(conn);
        fs.Delete(args[2]);
        Console.WriteLine($"DELETED: {args[2]}");
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"FAIL: {ex.Message}");
        ProviderFactory.Invalidate(conn.Name);
        Environment.Exit(2);
    }
}

static void CmdRename(string[] args)
{
    if (args.Length < 4) { PrintUsage(); return; }
    var conn = Find(args[1]);
    try
    {
        var fs = ProviderFactory.Get(conn);
        fs.Rename(args[2], args[3]);
        Console.WriteLine($"RENAMED: {args[2]} -> {args[3]}");
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"FAIL: {ex.Message}");
        ProviderFactory.Invalidate(conn.Name);
        Environment.Exit(2);
    }
}

static void CmdMkdir(string[] args)
{
    if (args.Length < 3) { PrintUsage(); return; }
    var conn = Find(args[1]);
    try
    {
        var fs = ProviderFactory.Get(conn);
        fs.CreateDirectory(args[2]);
        Console.WriteLine($"MKDIR: {args[2]}");
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"FAIL: {ex.Message}");
        ProviderFactory.Invalidate(conn.Name);
        Environment.Exit(2);
    }
}
