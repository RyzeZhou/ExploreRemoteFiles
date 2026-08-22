using System.Text.Json;

namespace ExplorerRemoteFs.Config;

/// <summary>
/// 连接配置存储：%APPDATA%\ExplorerRemoteFs\connections.json。
/// </summary>
public static class ConnectionStore
{
    private static readonly string Dir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "ExplorerRemoteFs");

    private static readonly string FilePath = Path.Combine(Dir, "connections.json");

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true
    };

    public static List<ConnectionConfig> Load()
    {
        try
        {
            if (!File.Exists(FilePath)) return new List<ConnectionConfig>();
            var json = File.ReadAllText(FilePath);
            return JsonSerializer.Deserialize<List<ConnectionConfig>>(json, JsonOptions) ?? new List<ConnectionConfig>();
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"ConnectionStore.Load failed: {ex.Message}");
            return new List<ConnectionConfig>();
        }
    }

    public static void Save(IEnumerable<ConnectionConfig> connections)
    {
        try
        {
            Directory.CreateDirectory(Dir);
            var json = JsonSerializer.Serialize(connections.ToList(), JsonOptions);
            File.WriteAllText(FilePath, json);
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"ConnectionStore.Save failed: {ex.Message}");
        }
    }
}
