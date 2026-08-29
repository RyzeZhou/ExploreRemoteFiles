using System.Diagnostics;
using System.IO;
using Microsoft.Win32;

namespace RemoteFsClient.Services;

public sealed class TestResult
{
    public bool Ok { get; init; }
    public string Message { get; init; } = "";
}

/// <summary>
/// 连接测试（第一版）：通过 winscp.com 脚本模式执行。
/// 策略：
///   1) 若 WinSCP 注册表里存在同名站点，直接 "open 站点名"——密码与 host key
///      由 WinSCP 自行解密（凭据不落任何临时文件）；
///   2) 否则用显式参数拼 URL，密码取自凭据管理器，写入 %TEMP% 临时脚本
///      （执行完立即删除；正式版 Engine 将改为 WinSCPnet 进程内会话，无临时文件）。
/// </summary>
public static class ConnectionTester
{
    private static readonly string[] CandidatePaths =
    {
        @"C:\Program Files (x86)\WinSCP\WinSCP.com",
        @"C:\Program Files\WinSCP\WinSCP.com",
    };

    public static async Task<TestResult> TestAsync(Models.SiteInfo site)
        => await Task.Run(() => Test(site));

    private static TestResult Test(Models.SiteInfo site)
    {
        string? winscp = CandidatePaths.FirstOrDefault(File.Exists);
        if (winscp == null)
            return Fail("未找到 WinSCP.com。请先安装 WinSCP。");

        string scriptPath = Path.Combine(Path.GetTempPath(), $"rfc-test-{Guid.NewGuid():N}.txt");
        try
        {
            string openCmd = BuildOpenCommand(site);
            File.WriteAllLines(scriptPath, new[]
            {
                "option batch abort",
                "option confirm off",
                openCmd,
                "pwd",
                "close",
                "exit",
            });

            var psi = new ProcessStartInfo
            {
                FileName = winscp,
                Arguments = $"/script=\"{scriptPath}\" /xmllog=nul",
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                StandardOutputEncoding = System.Text.Encoding.UTF8,
                StandardErrorEncoding = System.Text.Encoding.UTF8,
            };
            using var p = Process.Start(psi);
            if (p == null) return Fail("无法启动 WinSCP.com");
            string stdout = p.StandardOutput.ReadToEnd();
            p.WaitForExit(30000);
            if (!p.HasExited) { p.Kill(); return Fail("连接测试超时（30 秒）"); }

            bool ok = p.ExitCode == 0;
            string tail = string.Join("\n", stdout.Split('\n').Select(l => l.TrimEnd())
                .Where(l => l.Length > 0).TakeLast(8));
            return new TestResult
            {
                Ok = ok,
                Message = (ok ? "连接成功。\n" : "连接失败（exit=" + p.ExitCode + "）。\n") + tail,
            };
        }
        catch (Exception ex)
        {
            return Fail(ex.Message);
        }
        finally
        {
            try { if (File.Exists(scriptPath)) File.Delete(scriptPath); } catch { /* best effort */ }
        }
    }

    private static string BuildOpenCommand(Models.SiteInfo site)
    {
        // 1) 与 WinSCP 共享同名站点：凭据与 host key 由 WinSCP 自己管。
        if (RegistrySiteExists(site.Name))
            return $"open \"{site.Name}\"";

        // 2) 显式参数；密码取自凭据管理器。
        string scheme = site.Type.ToLowerInvariant() switch
        {
            "ftp" => "ftp",
            "ftps" => "ftps",
            _ => "sftp",
        };
        string url = $"{scheme}://{Uri.EscapeDataString(site.Username)}@{site.Host}:{site.EffectivePort}{site.StartPath}";
        string pwdSwitch = "";
        if (CredentialStore.TryRead(site.Name, out _, out string secret))
            pwdSwitch = $" -password={secret}";
        else
            pwdSwitch = " -hostkey=*";   // 无密码时至少跳过 host key 确认（仅测试模式）
        return $"open \"{url}\"{pwdSwitch}";
    }

    private static bool RegistrySiteExists(string name)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(
                $@"Software\Martin Prikryl\WinSCP 2\Sessions\{name}");
            return key != null;
        }
        catch { return false; }
    }

    private static TestResult Fail(string msg) => new() { Ok = false, Message = msg };
}
