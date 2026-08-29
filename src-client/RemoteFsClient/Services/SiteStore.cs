using System.IO;
using System.Text.Json;

namespace RemoteFsClient.Services;

/// <summary>
/// 站点存储：%APPDATA%\ExplorerRemoteFs\connections.json
/// （与 ExplorerRemoteFs.Config.ConnectionStore 同一份文件、同一格式）。
/// </summary>
public static class SiteStore
{
    private static readonly string Dir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "ExplorerRemoteFs");
    private static readonly string FilePath = Path.Combine(Dir, "connections.json");

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    public static List<Models.SiteInfo> Load()
    {
        try
        {
            if (!File.Exists(FilePath)) return new List<Models.SiteInfo>();
            var json = File.ReadAllText(FilePath);
            return JsonSerializer.Deserialize<List<Models.SiteInfo>>(json, JsonOptions) ?? new List<Models.SiteInfo>();
        }
        catch
        {
            return new List<Models.SiteInfo>();
        }
    }

    public static void Save(IEnumerable<Models.SiteInfo> sites)
    {
        Directory.CreateDirectory(Dir);
        var json = JsonSerializer.Serialize(sites.ToList(), JsonOptions);
        File.WriteAllText(FilePath, json);
    }

    public static string FilePathForDisplay => FilePath;
}
