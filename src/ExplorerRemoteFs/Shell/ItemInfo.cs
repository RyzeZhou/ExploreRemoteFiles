using ExplorerRemoteFs.Config;
using ExplorerRemoteFs.Providers;

namespace ExplorerRemoteFs.Shell;

/// <summary>
/// 命名空间条目模型：把连接/远程目录/远程文件统一编码为 PIDL 字符串标识。
/// 编码规则：c:{name} = 连接；d:{path} = 远程目录；f:{path} = 远程文件。
/// </summary>
public sealed class ItemInfo
{
    public enum ItemKind
    {
        Connection,
        Directory,
        File
    }

    public ItemKind Kind { get; }
    public string Id { get; }
    public ConnectionConfig? Connection { get; }
    public RemoteEntry? Entry { get; }

    private ItemInfo(ItemKind kind, string id, ConnectionConfig? conn, RemoteEntry? entry)
    {
        Kind = kind;
        Id = id;
        Connection = conn;
        Entry = entry;
    }

    public static ItemInfo FromConnection(ConnectionConfig c) =>
        new(ItemKind.Connection, "c:" + c.Name, c, null);

    public static ItemInfo FromDirectory(ConnectionConfig conn, RemoteEntry e) =>
        new(ItemKind.Directory, "d:" + e.Path, conn, e);

    public static ItemInfo FromFile(ConnectionConfig conn, RemoteEntry e) =>
        new(ItemKind.File, "f:" + e.Path, conn, e);

    /// <summary>从 PIDL 字符串标识解码。</summary>
    public static ItemInfo? FromId(string id, ConnectionConfig? parentConn, string parentPath)
    {
        if (id.StartsWith("c:", StringComparison.Ordinal))
        {
            var name = id[2..];
            var conn = ConnectionStore.Load().FirstOrDefault(c => c.Name == name);
            return conn is null ? null : FromConnection(conn);
        }
        if (id.StartsWith("d:", StringComparison.Ordinal))
        {
            var path = id[2..];
            var e = new RemoteEntry { Name = NameOf(path), Path = path, IsDirectory = true };
            return FromDirectory(parentConn!, e);
        }
        if (id.StartsWith("f:", StringComparison.Ordinal))
        {
            var path = id[2..];
            var e = new RemoteEntry { Name = NameOf(path), Path = path, IsDirectory = false };
            return FromFile(parentConn!, e);
        }
        return null;
    }

    public bool IsFolder => Kind is ItemKind.Connection or ItemKind.Directory;

    public string DisplayName => Kind switch
    {
        ItemKind.Connection => Connection!.Name,
        _ => Entry!.Name
    };

    /// <summary>文件夹类型标识（列显示用）。</summary>
    public string TypeText => Kind switch
    {
        ItemKind.Connection => "连接",
        ItemKind.Directory => "文件夹",
        _ => "文件"
    };

    private static string NameOf(string path)
    {
        path = path.TrimEnd('/');
        var i = path.LastIndexOf('/');
        return i >= 0 ? path[(i + 1)..] : path;
    }
}
