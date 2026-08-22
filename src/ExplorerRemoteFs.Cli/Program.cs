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
