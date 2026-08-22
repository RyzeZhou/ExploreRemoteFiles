namespace ExplorerRemoteFs.Shell;

/// <summary>
/// Details 视图列定义：根节点（Servers）与远程目录使用不同列。
/// 索引约定：0=Name, 1=Permissions, 2=Owner, 3=Group, 4=Size, 5=Modified。
/// </summary>
public static class ColumnManager
{
    public const uint ColName = 0;
    public const uint ColPermissions = 1;
    public const uint ColOwner = 2;
    public const uint ColGroup = 3;
    public const uint ColSize = 4;
    public const uint ColModified = 5;

    /// <summary>当前文件夹（根=Servers）的列定义。</summary>
    public static (string Title, int Width)[] GetColumns(bool isRoot)
    {
        return isRoot
            ? new[] { ("Name", 180), ("Type", 80), ("Host", 160), ("User", 120) }
            : new[] { ("Name", 220), ("Permissions", 110), ("Owner", 80), ("Group", 80), ("Size", 90), ("Modified", 140) };
    }

    public static uint ColumnCount(bool isRoot) => (uint)GetColumns(isRoot).Length;

    /// <summary>返回某列的显示值（列 0=Name 走 DisplayName）。</summary>
    public static string GetValue(ItemInfo item, uint column, bool isRoot)
    {
        if (isRoot)
        {
            return column switch
            {
                0 => item.DisplayName,
                1 => item.TypeText,
                2 => item.Connection?.Host ?? "",
                3 => item.Connection?.Username ?? "",
                _ => ""
            };
        }

        var e = item.Entry;
        if (e is null) return column == 0 ? item.DisplayName : "";
        return column switch
        {
            0 => item.DisplayName,
            1 => e.ModeDisplay,
            2 => e.OwnerDisplay,
            3 => e.GroupDisplay,
            4 => e.SizeDisplay,
            5 => e.ModifiedDisplay,
            _ => ""
        };
    }
}
