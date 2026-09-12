using System.Globalization;
using System.IO;

/// <summary>
/// Tracks transfers that did NOT finish, so the next attempt at the same item
/// can resume from the breakpoint instead of starting over.
///
/// Why a record instead of "always resume": resume is only safe for a partial
/// file WE created. Blindly resuming onto an existing same-named file could
/// append onto somebody else's complete file. Files are written to the temp
/// dir and cleared on success, so the presence of a record means "this exact
/// transfer was interrupted".
/// </summary>
internal static class ResumeStore
{
    private static readonly object Gate = new();
    private static readonly string StorePath =
        Path.Combine(Path.GetTempPath(), "rfs-resume.tsv");
    private static readonly string[] Empty = Array.Empty<string>();

    private static string Key(string direction, string server, string path)
        => direction + "\t" + server + "\t" + path;

    /// <summary>TRUE when this exact transfer was interrupted before.</summary>
    public static bool HasPending(string direction, string server, string path, long sourceSize)
    {
        lock (Gate)
        {
            string want = Key(direction, server, path) + "\t";
            foreach (string line in ReadLines())
            {
                if (!line.StartsWith(want, StringComparison.Ordinal)) continue;
                string[] f = line.Split('\t');
                // f[3] = source size, f[4] = bytes done
                if (f.Length >= 5 && long.TryParse(f[3], out long size) && size == sourceSize)
                    return true;
            }
            return false;
        }
    }

    /// <summary>Records/updates the progress of an interrupted transfer.</summary>
    public static void Mark(string direction, string server, string path, long sourceSize, long done)
    {
        lock (Gate)
        {
            string key = Key(direction, server, path);
            var lines = new List<string>();
            foreach (string line in ReadLines())
                if (!line.StartsWith(key + "\t", StringComparison.Ordinal)) lines.Add(line);
            lines.Add(key + "\t" + sourceSize + "\t" + done);
            Write(lines);
        }
    }

    /// <summary>Drops the record (transfer finished successfully).</summary>
    public static void Clear(string direction, string server, string path)
    {
        lock (Gate)
        {
            string key = Key(direction, server, path);
            var lines = new List<string>();
            foreach (string line in ReadLines())
                if (!line.StartsWith(key + "\t", StringComparison.Ordinal)) lines.Add(line);
            Write(lines);
        }
    }

    private static string[] ReadLines()
    {
        try
        {
            return File.Exists(StorePath) ? File.ReadAllLines(StorePath) : Empty;
        }
        catch { return Empty; }
    }

    private static void Write(List<string> lines)
    {
        try { File.WriteAllLines(StorePath, lines); } catch { }
    }
}
