using System.IO;

namespace RemoteFsClient.Services;

/// <summary>Reads only the language-section names from Explorer's optional YAML translation file.</summary>
public static class TranslationCatalog
{
    public static string FilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "ExplorerRemoteFs", "explorer-translations.yaml");

    public static IReadOnlyList<string> GetExplorerLanguages()
    {
        var languages = new List<string> { "zh-CN", "en-US" };
        try
        {
            if (!File.Exists(FilePath)) return languages;
            foreach (var raw in File.ReadLines(FilePath))
            {
                if (string.IsNullOrWhiteSpace(raw) || char.IsWhiteSpace(raw[0]) || raw.TrimStart().StartsWith('#')) continue;
                var line = raw.Trim();
                if (!line.EndsWith(':')) continue;
                var code = line[..^1].Trim();
                if (code.Length is < 2 or > 32 || code.Any(char.IsWhiteSpace) || languages.Contains(code, StringComparer.OrdinalIgnoreCase)) continue;
                languages.Add(code);
            }
        }
        catch { }
        return languages;
    }
}