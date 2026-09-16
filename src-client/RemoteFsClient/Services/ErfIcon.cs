using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

namespace RemoteFsClient.Services;

/// <summary>易远传（ERF = Explorer Remote Files）的图标：中间一个大写 <b>E</b>，
/// 左下角 <b>R</b>、右下角 <b>F</b>。R/F 各自用颜色表示状态：
///
/// <list type="bullet">
/// <item><b>R</b>（Remote，远程站点连接）：绿 = 正在访问的站点连接正常；
/// 红 = 至少有一个正在使用的站点连接不正常。</item>
/// <item><b>F</b>（File transfer，传输）：绿 = 没有正在传输的文件；
/// 黄 = 正在传输且无错误；红 = 出现过传输错误。</item>
/// </list>
///
/// 图标不是静态资源而是**画出来的**：状态一共 3×3 种组合，
/// 与其打包 9 个 .ico，不如按状态实时生成（GDI+ → HICON / PNG / WPF ImageSource）。
/// 静态 .ico（供资源管理器与将来的安装程序用）由 <c>--make-icon</c> 导出，默认全绿。
/// </summary>
public static class ErfIcon
{
    public enum ColorState { Green, Yellow, Red }

    /// <summary>远程站点连接状态。</summary>
    public enum RemoteState { Ok, Problem }

    /// <summary>传输状态。</summary>
    public enum TransferState { Idle, Active, Error }

    public static readonly Color Green = Color.FromArgb(0x2E, 0xA0, 0x43);
    public static readonly Color Yellow = Color.FromArgb(0xE8, 0xA3, 0x1C);
    public static readonly Color Red = Color.FromArgb(0xD1, 0x34, 0x38);
    private static readonly Color Plate = Color.FromArgb(0x1F, 0x3B, 0x63);   // 底板：深蓝
    private static readonly Color PlateEdge = Color.FromArgb(0x14, 0x28, 0x45);

    public static ColorState ColorFor(RemoteState state) => state == RemoteState.Ok ? ColorState.Green : ColorState.Red;

    public static ColorState ColorFor(TransferState state) => state switch
    {
        TransferState.Active => ColorState.Yellow,
        TransferState.Error => ColorState.Red,
        _ => ColorState.Green,
    };

    public static Color Brush(ColorState state) => state switch
    {
        ColorState.Yellow => Yellow,
        ColorState.Red => Red,
        _ => Green,
    };

    /// <summary>按状态画一张正方形位图。size 建议 16/20/24/32/48/64/128/256。</summary>
    public static Bitmap Render(int size, ColorState remote, ColorState transfer)
    {
        var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using var g = Graphics.FromImage(bmp);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
        g.Clear(Color.Transparent);

        float s = size;
        // 圆角底板：让白色字母在任何背景（深色任务栏 / 浅色资源管理器）上都读得出来。
        float pad = s * 0.04f;
        var plate = new RectangleF(pad, pad, s - 2 * pad, s - 2 * pad);
        float radius = s * 0.22f;
        using (var path = RoundedRect(plate, radius))
        using (var fill = new LinearGradientBrush(plate, Plate, PlateEdge, 60f))
        using (var edge = new Pen(Color.FromArgb(90, 255, 255, 255), Math.Max(1f, s * 0.02f)))
        {
            g.FillPath(fill, path);
            g.DrawPath(edge, path);
        }

        // 版面：E 占上方大半，R/F 占下方一条带（各占一半宽）。
        // 字母用**测量后缩放到框内**的方式画（见 DrawGlyph），
        // 这样任何尺寸都不会被画布/底板裁掉 —— 第一版按字号猜，结果三个字母互相压、
        // 底部还被圆角切掉。
        var eBox = new RectangleF(s * 0.14f, s * 0.06f, s * 0.72f, s * 0.46f);
        var rBox = new RectangleF(s * 0.08f, s * 0.57f, s * 0.38f, s * 0.33f);
        var fBox = new RectangleF(s * 0.54f, s * 0.57f, s * 0.38f, s * 0.33f);
        using (var white = new SolidBrush(Color.White))
        using (var rBrush = new SolidBrush(Brush(remote)))
        using (var fBrush = new SolidBrush(Brush(transfer)))
        using (var halo = new Pen(PlateEdge, Math.Max(1f, s * 0.030f)) { LineJoin = LineJoin.Round })
        {
            DrawGlyph(g, "E", eBox, white, halo);
            DrawGlyph(g, "R", rBox, rBrush, halo);
            DrawGlyph(g, "F", fBox, fBrush, halo);
        }
        return bmp;
    }

    /// <summary>把一个字母画进指定矩形：先按任意字号取字形轮廓，量出包围盒后
    /// 缩放到正好放进 box（留 2% 余量）并居中。与字号/DPI 无关，永不出框。</summary>
    private static void DrawGlyph(Graphics g, string text, RectangleF box, Brush fill, Pen halo)
    {
        using var family = GlyphFamily();
        using var path = new GraphicsPath();
        path.AddString(text, family, (int)FontStyle.Bold, 100f, new PointF(0, 0),
                       StringFormat.GenericTypographic);
        RectangleF b = path.GetBounds();
        if (b.Width <= 0 || b.Height <= 0) return;
        float scale = Math.Min(box.Width / b.Width, box.Height / b.Height) * 0.98f;
        // 变换顺序很重要：Matrix 的 Scale/Translate 默认是 **Prepend**（后调用的先作用于点），
        // 用默认顺序会把平移量也乘上缩放系数，字母于是跑到框外互相压住（这个坑踩过）。
        float tx = box.X + (box.Width - b.Width * scale) / 2f - b.X * scale;
        float ty = box.Y + (box.Height - b.Height * scale) / 2f - b.Y * scale;
        using var m = new Matrix();
        m.Scale(scale, scale, MatrixOrder.Append);
        m.Translate(tx, ty, MatrixOrder.Append);
        path.Transform(m);
        if (halo.Width > 0.6f) g.DrawPath(halo, path);
        g.FillPath(fill, path);
    }

    private static FontFamily GlyphFamily()
    {
        try { return new FontFamily("Arial Black"); }
        catch { /* 系统没有该字体时退回通用无衬线 */ }
        return FontFamily.GenericSansSerif;
    }

    private static GraphicsPath RoundedRect(RectangleF r, float radius)
    {
        var path = new GraphicsPath();
        float d = radius * 2;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }

    /// <summary>状态图标（缓存）。给托盘 / 窗口标题栏用。</summary>
    public static Icon ToIcon(int size, ColorState remote, ColorState transfer)
    {
        lock (Cache)
        {
            string key = $"{size}:{remote}:{transfer}";
            if (Cache.TryGetValue(key, out var cached)) return cached;
            using var bmp = Render(size, remote, transfer);
            // Icon.FromHandle 不拥有句柄，必须克隆一份由 Icon 自己持有，否则会泄漏/失效。
            using var tmp = Icon.FromHandle(bmp.GetHicon());
            var icon = (Icon)tmp.Clone();
            Cache[key] = icon;
            return icon;
        }
    }

    private static readonly Dictionary<string, Icon> Cache = new();

    /// <summary>多尺寸 ICO（PNG 压缩条目，Vista+ 支持）。</summary>
    public static void SaveIco(string path, ColorState remote, ColorState transfer, params int[] sizes)
    {
        if (sizes.Length == 0) sizes = new[] { 16, 20, 24, 32, 48, 64, 128, 256 };
        var pngs = new List<byte[]>();
        foreach (int size in sizes)
        {
            using var bmp = Render(size, remote, transfer);
            using var ms = new MemoryStream();
            bmp.Save(ms, ImageFormat.Png);
            pngs.Add(ms.ToArray());
        }

        using var fs = File.Create(path);
        using var w = new BinaryWriter(fs);
        w.Write((ushort)0);                 // reserved
        w.Write((ushort)1);                 // type: icon
        w.Write((ushort)sizes.Length);
        int offset = 6 + 16 * sizes.Length;
        for (int i = 0; i < sizes.Length; ++i)
        {
            w.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i]));   // 0 表示 256
            w.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i]));
            w.Write((byte)0);               // 调色板数
            w.Write((byte)0);               // reserved
            w.Write((ushort)1);             // 色彩平面
            w.Write((ushort)32);            // 位深
            w.Write(pngs[i].Length);
            w.Write(offset);
            offset += pngs[i].Length;
        }
        foreach (var png in pngs) w.Write(png);
    }

    /// <summary>把 3×3 种状态拼成一张对照图（给人工确认设计用）。</summary>
    public static void SaveMontage(string path, int cell = 96, int scale = 2)
    {
        var remotes = new[] { RemoteState.Ok, RemoteState.Problem };
        var transfers = new[] { TransferState.Idle, TransferState.Active, TransferState.Error };
        int rows = transfers.Length, cols = remotes.Length;
        int labelW = 176, labelH = 46, footH = 170;
        int w = labelW + cols * cell * scale, h = labelH + rows * cell * scale + footH;
        using var bmp = new Bitmap(w, h);
        using var g = Graphics.FromImage(bmp);
        g.Clear(Color.FromArgb(0xF3, 0xF3, 0xF3));
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
        using var small = new Font("Segoe UI", 9f);
        using var ink = new SolidBrush(Color.FromArgb(0x22, 0x22, 0x22));
        using var caption = new Font("Segoe UI", 11f, FontStyle.Bold);

        g.DrawString("易远传 ERF 图标状态", caption, ink, 8, 6);
        for (int c = 0; c < cols; ++c)
        {
            string t = remotes[c] == RemoteState.Ok ? "R 绿（站点连接正常）" : "R 红（有站点连接异常）";
            g.DrawString(t, small, ink, labelW + c * cell * scale + 6, 28);
        }
        for (int r = 0; r < rows; ++r)
        {
            string t = transfers[r] switch
            {
                TransferState.Idle => "F 绿：无传输",
                TransferState.Active => "F 黄：传输中",
                _ => "F 红：传输出错",
            };
            g.DrawString(t, small, ink, 8, labelH + r * cell * scale + cell * scale / 2 - 8);
            for (int c = 0; c < cols; ++c)
            {
                using var icon = Render(cell, ColorFor(remotes[c]), ColorFor(transfers[r]));
                var dst = new Rectangle(labelW + c * cell * scale + 4, labelH + r * cell * scale + 4,
                                        cell * scale - 8, cell * scale - 8);
                g.InterpolationMode = InterpolationMode.NearestNeighbor;   // 看像素细节
                g.DrawImage(icon, dst);
            }
        }
        // 再画一排实尺寸 + 6 倍放大：托盘里就是 16px，必须确认这个尺寸下还分得清 E/R/F。
        int y = labelH + rows * cell * scale + 10;
        g.DrawString("16px（上）与 6 倍放大（下）—— 托盘里的样子：", small, ink, 8, y);
        int x = labelW - 40;
        foreach (var t in transfers)
        {
            using var icon = Render(16, ColorState.Green, ColorFor(t));
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.DrawImage(icon, x, y + 14, 16, 16);
            g.DrawImage(icon, new Rectangle(x - 8, y + 36, 96, 96));
            x += 112;
        }
        bmp.Save(path, ImageFormat.Png);
    }
}
