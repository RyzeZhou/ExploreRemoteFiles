using System.IO;
using System.Text;
using ExplorerRemoteFs.Utils;

namespace RemoteFsClient;

/// <summary>大小显示口径的自检（`RemoteFsClient.exe --size-selftest`）。
///
/// 口径有两份实现：扩展 DLL 的 C++（<c>SizeFormat.h</c>，列与属性页用它）与
/// CLI/客户端的 C#（<c>Utils/SizeFormat.cs</c>）。两份必须给出**逐字相同**的字符串，
/// 否则同一个文件在列里和属性页/命令行里会对不上账。
/// 保证办法是同一张期望表：这里与 `research/tools/sizeformat-test/sizeformat-test.cpp`
/// 的 kCases 一一对应，任何一边改了规则，两边的自检都会红。
/// </summary>
internal static class SizeSelfTest
{
    private static int _fail, _pass;
    private static readonly List<string> Lines = new();

    internal static string LogPath => Path.Combine(Path.GetTempPath(), "erf-size-selftest.log");

    private static void Say(string line)
    {
        Lines.Add(line);
        try { Console.WriteLine(line); } catch { }
    }

    private static void Check(long bytes, string mode, SizeFormatMode m, string expect)
    {
        string got = SizeFormat.Format(bytes, m);
        bool ok = got == expect;
        if (ok) _pass++; else _fail++;
        Say($"  {(ok ? "PASS" : "FAIL")}  {bytes,12} B  {mode,-4} -> {got}{(ok ? "" : $"   （期望 {expect}）")}");
    }

    public static int Run()
    {
        Say($"== 大小口径自检（C# 实现，与 sizeformat-test 同一张期望表） {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
        // 与 sizeformat-test.cpp 的 kCases 完全一致
        var cases = new (long Bytes, string Auto, string Kb, string Si, string Iec)[]
        {
            (41,          "41 B",     "1 KB",       "41 B",       "41 B"),
            (999,         "999 B",    "1 KB",       "999 B",      "999 B"),
            (1000,        "1000 B",   "1 KB",       "1.0 kB",     "1000 B"),
            (1023,        "1023 B",   "1 KB",       "1.0 kB",     "1023 B"),
            (1024,        "1.0 KB",   "1 KB",       "1.0 kB",     "1.0 KiB"),
            (1536,        "1.5 KB",   "2 KB",       "1.5 kB",     "1.5 KiB"),
            (1000000,     "976.6 KB", "977 KB",     "1.0 MB",     "976.6 KiB"),
            (1048576,     "1.0 MB",   "1024 KB",    "1.0 MB",     "1.0 MiB"),
            (1500000000,  "1.4 GB",   "1464844 KB", "1.5 GB",     "1.4 GiB"),
        };

        Say($"  {"字节",-14} {"auto(1024)",-12} {"kb(整KB)",-12} {"si(1000)",-12} {"iec(KiB)",-12}");
        foreach (var c in cases)
            Say($"  {c.Bytes,-14} " +
                $"{SizeFormat.Format(c.Bytes, SizeFormatMode.Auto),-12} " +
                $"{SizeFormat.Format(c.Bytes, SizeFormatMode.WholeKb),-12} " +
                $"{SizeFormat.Format(c.Bytes, SizeFormatMode.Si),-12} " +
                $"{SizeFormat.Format(c.Bytes, SizeFormatMode.Iec),-12}");

        Say("");
        foreach (var c in cases)
        {
            Check(c.Bytes, "auto", SizeFormatMode.Auto, c.Auto);
            Check(c.Bytes, "kb", SizeFormatMode.WholeKb, c.Kb);
            Check(c.Bytes, "si", SizeFormatMode.Si, c.Si);
            Check(c.Bytes, "iec", SizeFormatMode.Iec, c.Iec);
        }

        // 目录：列里与属性页都显示 "-"
        bool dirOk = SizeFormat.Format(12345, SizeFormatMode.Auto, isFolder: true) == "-";
        if (dirOk) { _pass++; Say("  PASS  目录 -> -"); } else { _fail++; Say("  FAIL  目录不是 -"); }

        // 属性页形态："口径 + 精确字节"；本身就是字节数时不重复
        string withExact = SizeFormat.FormatWithExact(1234567);
        bool exOk = withExact.Contains("1,234,567 B", StringComparison.Ordinal);
        if (exOk) { _pass++; Say($"  PASS  属性页形态 -> {withExact}"); }
        else { _fail++; Say($"  FAIL  属性页形态 -> {withExact}（应含精确字节）"); }

        string small = SizeFormat.FormatWithExact(41);
        bool smallOk = small == "41 B";
        if (smallOk) { _pass++; Say($"  PASS  小文件不重复 -> {small}"); }
        else { _fail++; Say($"  FAIL  小文件 -> {small}（应为 41 B）"); }

        // 非法设置串一律回退默认（与 C++ 侧 ErfParseSizeFormat 一致）
        var parseOk = SizeFormat.Parse("bogus") == SizeFormatMode.Auto
                   && SizeFormat.Parse("") == SizeFormatMode.Auto
                   && SizeFormat.Parse(null) == SizeFormatMode.Auto
                   && SizeFormat.Parse("IEC") == SizeFormatMode.Iec;
        if (parseOk) { _pass++; Say("  PASS  非法/空/大小写 → 回退默认或正确识别"); }
        else { _fail++; Say("  FAIL  设置串解析不符合预期"); }

        Say($"\n{_pass} 通过, {_fail} 失败");
        try { File.WriteAllLines(LogPath, Lines, new UTF8Encoding(true)); } catch { }
        return _fail == 0 ? 0 : 1;
    }
}
