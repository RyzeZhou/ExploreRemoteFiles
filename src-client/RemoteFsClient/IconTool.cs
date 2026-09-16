using System.IO;
using System.Text;
using RemoteFsClient.Services;

namespace RemoteFsClient;

/// <summary>图标生成器（`RemoteFsClient.exe --make-icon &lt;目录&gt;`）。
///
/// 导出两样东西，都不需要联网、不建窗口：
/// <list type="bullet">
/// <item><c>erf.ico</c> —— 多尺寸静态图标（默认全绿状态），给资源管理器命名空间、
/// 可执行文件、将来的安装程序用；</item>
/// <item><c>erf-icon-states.png</c> —— 3×3 状态对照图（R/F 各自三种颜色）+ 一排 16px 实尺寸，
/// 用来人工确认设计，也方便回归时对比。</item>
/// </list>
/// </summary>
internal static class IconTool
{
    internal static string LogPath => Path.Combine(Path.GetTempPath(), "erf-make-icon.log");

    private static string Name(ErfIcon.ColorState c) => c switch
    {
        ErfIcon.ColorState.Yellow => "黄",
        ErfIcon.ColorState.Red => "红",
        _ => "绿",
    };

    public static int MakeIcons(string outDir)
    {
        var log = new List<string>();
        void Say(string line)
        {
            log.Add(line);
            try { Console.WriteLine(line); } catch { }
        }
        int fail = 0;
        try
        {
            Directory.CreateDirectory(outDir);
            Say($"易远传 ERF 图标生成 -> {outDir}");

            string ico = Path.Combine(outDir, "erf.ico");
            ErfIcon.SaveIco(ico, ErfIcon.ColorState.Green, ErfIcon.ColorState.Green);
            Say($"erf.ico: {new FileInfo(ico).Length} 字节（16/20/24/32/48/64/128/256，PNG 压缩条目）");

            string montage = Path.Combine(outDir, "erf-icon-states.png");
            ErfIcon.SaveMontage(montage);
            Say($"erf-icon-states.png: {new FileInfo(montage).Length} 字节（3×3 状态 + 16px 实尺寸）");

            // 单独导出 256px 全绿版：README / 文档里贴图用
            string png256 = Path.Combine(outDir, "erf-256.png");
            using (var bmp = ErfIcon.Render(256, ErfIcon.ColorState.Green, ErfIcon.ColorState.Green))
                bmp.Save(png256, System.Drawing.Imaging.ImageFormat.Png);
            Say($"erf-256.png: {new FileInfo(png256).Length} 字节");

            // 状态→颜色规则的断言：R（连接）与 F（传输）各三态必须映射到约定的颜色。
            Say("");
            Say("状态映射断言（R=站点连接，F=传输）：");
            int bad = 0;
            void Expect(string label, ErfIcon.ColorState got, ErfIcon.ColorState want)
            {
                bool ok = got == want;
                if (!ok) bad++;
                Say($"  {(ok ? "PASS" : "FAIL")}  {label} -> {Name(got)}{(ok ? "" : $"（应为 {Name(want)}）")}");
            }
            Expect("R 绿：站点连接正常", ErfIcon.ColorFor(ErfIcon.RemoteState.Ok), ErfIcon.ColorState.Green);
            Expect("R 红：有站点连接异常", ErfIcon.ColorFor(ErfIcon.RemoteState.Problem), ErfIcon.ColorState.Red);
            Expect("F 绿：无传输", ErfIcon.ColorFor(ErfIcon.TransferState.Idle), ErfIcon.ColorState.Green);
            Expect("F 黄：传输中无错", ErfIcon.ColorFor(ErfIcon.TransferState.Active), ErfIcon.ColorState.Yellow);
            Expect("F 红：传输出错", ErfIcon.ColorFor(ErfIcon.TransferState.Error), ErfIcon.ColorState.Red);

            // 再看 RemoteStatusService 的聚合规则：任一站点失败即红；成功即恢复。
            var status = new RemoteStatusService();
            bool ok1 = status.Remote == ErfIcon.RemoteState.Ok;                       // 初始：绿
            status.ReportSite("WSL-SFTP", true);
            bool ok2 = status.Remote == ErfIcon.RemoteState.Ok;                       // 一个站点正常：绿
            status.ReportSite("WSL", false);
            bool ok3 = status.Remote == ErfIcon.RemoteState.Problem;                  // 另一个站点失败：红
            status.ReportSite("WSL", true);
            bool ok4 = status.Remote == ErfIcon.RemoteState.Ok;                       // 恢复后：绿
            void Expect2(string label, bool ok)
            {
                if (!ok) bad++;
                Say($"  {(ok ? "PASS" : "FAIL")}  {label}");
            }
            Expect2("初始（没有任何站点记录）R 为绿", ok1);
            Expect2("一个站点正常时 R 为绿", ok2);
            Expect2("只要有一个站点失败 R 即为红", ok3);
            Expect2("该站点恢复后 R 回到绿", ok4);

            // 把每个状态的托盘图标（16px）也导出，方便直接看"托盘里长什么样"
            var tray = Path.Combine(outDir, "erf-tray-states.png");
            using (var sheet = new System.Drawing.Bitmap(12 + 6 * 58, 96))
            using (var g = System.Drawing.Graphics.FromImage(sheet))
            {
                g.Clear(System.Drawing.Color.FromArgb(0xF3, 0xF3, 0xF3));
                using var font = new System.Drawing.Font("Segoe UI", 8f);
                using var ink = new System.Drawing.SolidBrush(System.Drawing.Color.FromArgb(0x22, 0x22, 0x22));
                g.DrawString("16px 托盘图标（6 倍放大）：F 绿/黄/红 × R 绿/红", font, ink, 4, 4);
                int i = 0;
                foreach (var t in new[] { ErfIcon.TransferState.Idle, ErfIcon.TransferState.Active, ErfIcon.TransferState.Error })
                    foreach (var r in new[] { ErfIcon.RemoteState.Ok, ErfIcon.RemoteState.Problem })
                    {
                        using var ic = ErfIcon.Render(16, ErfIcon.ColorFor(r), ErfIcon.ColorFor(t));
                        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.NearestNeighbor;
                        g.DrawImage(ic, 6 + i * 58, 40, 48, 48);
                        i++;
                    }
                sheet.Save(tray, System.Drawing.Imaging.ImageFormat.Png);
            }
            Say($"erf-tray-states.png: {new FileInfo(tray).Length} 字节（6 种状态 × 16px 放大）");

            if (bad > 0) { fail++; Say($"状态断言有 {bad} 项失败"); }
        }
        catch (Exception ex)
        {
            fail++;
            Say($"失败：{ex}");
        }

        Say(fail == 0 ? "完成" : $"{fail} 项失败");
        try { File.WriteAllLines(LogPath, log, new UTF8Encoding(true)); } catch { }
        return fail == 0 ? 0 : 1;
    }
}
