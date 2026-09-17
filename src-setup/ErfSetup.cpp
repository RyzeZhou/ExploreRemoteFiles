// 易远传（ERF）安装器 / 卸载器 —— 一个可执行文件，按**文件名**区分行为：
//   Setup.exe      → 安装（调用随包的 install.ps1，再写"应用和功能"与登录自启）
//   Uninstall.exe  → 卸载（调用随包的 uninstall.ps1，再清理上面两项）
//
// 为什么是"调用 PowerShell 脚本"而不是在 C++ 里重写一遍注册逻辑：
// 注册/注销的语义（尤其 erf:// 的所有权检查、桌面图标键只动自己的值、
// 卸载默认保留凭据）已经在 install.ps1 / uninstall.ps1 里打磨了很久，
// **一份实现**才不会两边漂移。本程序只做"安装器该做而脚本不该做"的事：
// 交互确认、重启资源管理器的提示、"应用和功能"条目、登录自启项、
// 以及卸载后把安装目录（含自身）清干净。
//
// 这是过渡方案：以后迁到 Inno Setup 时，.iss 只需要调用同样的两个脚本，
// 本文件里的注册表项语义可以直接搬过去。
//
// 用法：
//   Setup.exe                     安装（默认 %LOCALAPPDATA%\ExplorerRemoteFs，带确认框）
//   Setup.exe --dir <路径>        指定安装目录
//   Setup.exe --silent            静默（无对话框，失败靠退出码）
//   Uninstall.exe [--silent] [--purge-config]
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stdio.h>

#define APP_NAME      L"易远传 (Explorer Remote Files)"
#define APP_VERSION   L"0.1-Alpha"
#define APP_PUBLISHER L"RyzeZhou"
#define ARP_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ExplorerRemoteFs"
#define RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE     L"ExplorerRemoteFs"

static bool g_silent = false;

// 每条输出同时落一份 UTF-8 日志（%TEMP%\erf-setup.log）。
// 为什么必须要有：--silent 安装时没人看控制台，而把 Setup.exe 的 stdout 重定向到文件时，
// CRT 用的是控制台代码页 → 中文全变乱码（实测 2026-09-17，警告看得见、错误码看不清）。
// 日志文件里写的是明确的 UTF-8（带 BOM），排障时直接 Get-Content -Encoding UTF8。
static void LogToFile(const wchar_t *text)
{
    wchar_t temp[MAX_PATH] = {}, path[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, temp)) return;
    StringCchPrintfW(path, MAX_PATH, L"%serf-setup.log", temp);
    const bool fresh = !PathFileExistsW(path);
    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f) return;
    if (fresh) fwrite("\xEF\xBB\xBF", 1, 3, f);
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char stamp[64] = {};
    sprintf_s(stamp, "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond);
    char utf8[4096] = {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8) - 1, NULL, NULL);
    if (n > 1)
    {
        fwrite(stamp, 1, strlen(stamp), f);
        fwrite(utf8, 1, (size_t)(n - 1), f);
        fwrite("\r\n", 1, 2, f);
    }
    fclose(f);
}

static void Say(const wchar_t *fmt, ...)
{
    wchar_t buf[1024] = {};
    va_list a;
    va_start(a, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, a);
    va_end(a);
    wprintf(L"%s\n", buf);
    LogToFile(buf);
    if (!g_silent)
    {
        // 有 UI 时也弹出来（安装器最常见的失败原因就是"点了没反应"）
        MessageBoxW(NULL, buf, L"易远传 安装程序", MB_OK | MB_ICONINFORMATION);
    }
}

static bool IsUninstallMode(const wchar_t *exePath)
{
    const wchar_t *name = PathFindFileNameW(exePath);
    return name && StrStrIW(name, L"uninstall") != NULL;
}

static bool HasArg(const wchar_t *arg)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc && !found; ++i)
        if (0 == StrCmpIW(argv[i], arg)) found = true;
    LocalFree(argv);
    return found;
}

static bool ArgValue(const wchar_t *arg, wchar_t *out, UINT cch)
{
    out[0] = 0;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc; ++i)
        if (0 == StrCmpIW(argv[i], arg) && i + 1 < argc)
        {
            StringCchCopyW(out, cch, argv[i + 1]);
            found = true;
            break;
        }
    LocalFree(argv);
    return found;
}

// 运行 powershell -File <script> <args...>，返回退出码（-1 = 起不来）。
static int RunPowerShell(const wchar_t *script, const wchar_t *args)
{
    wchar_t cmd[2048] = {};
    StringCchPrintfW(cmd, ARRAYSIZE(cmd),
                     L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" %s",
                     script, args ? args : L"");
    Say(L"执行：%s", cmd);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        Say(L"无法启动 PowerShell（错误 %u）。", GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static void WriteArpEntry(const wchar_t *installDir)
{
    HKEY key = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ARP_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;
    auto setStr = [&](PCWSTR name, PCWSTR value)
    {
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value,
                       (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
    };
    wchar_t uninstaller[MAX_PATH] = {}, icon[MAX_PATH] = {};
    StringCchPrintfW(uninstaller, MAX_PATH, L"\"%s\\Uninstall.exe\"", installDir);
    StringCchPrintfW(icon, MAX_PATH, L"%s\\client\\RemoteFsClient.exe", installDir);

    setStr(L"DisplayName", APP_NAME);
    setStr(L"DisplayVersion", APP_VERSION);
    setStr(L"Publisher", APP_PUBLISHER);
    setStr(L"InstallLocation", installDir);
    setStr(L"UninstallString", uninstaller);
    setStr(L"QuietUninstallString", L"");   // 占位，下面覆盖
    setStr(L"QuietUninstallString", uninstaller);
    setStr(L"DisplayIcon", icon);
    setStr(L"InstallDate", L"");
    DWORD one = 1;
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
    // 估计大小（KB）：让"应用和功能"里能显示体积，顺手算一下
    ULARGE_INTEGER total = {};
    wchar_t pattern[MAX_PATH] = {};
    StringCchPrintfW(pattern, MAX_PATH, L"%s\\*", installDir);
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                ULARGE_INTEGER fsz = {};
                fsz.LowPart = fd.nFileSizeLow;
                fsz.HighPart = fd.nFileSizeHigh;
                total.QuadPart += fsz.QuadPart;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    DWORD est = (DWORD)(total.QuadPart / 1024 + 200000);   // 子目录没细算，给个下界
    RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD, (const BYTE *)&est, sizeof(est));
    RegCloseKey(key);
}

static void WriteRunEntry(const wchar_t *installDir)
{
    HKEY key = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;
    wchar_t cmd[MAX_PATH * 2] = {};
    StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"\"%s\\client\\RemoteFsClient.exe\" --background", installDir);
    RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd,
                   (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
    RegCloseKey(key);
}

static void RemoveArpAndRun()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, ARP_KEY);
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        RegDeleteValueW(key, RUN_VALUE);
        RegCloseKey(key);
    }
}

static void ReadArpInstallDir(wchar_t *out, UINT cch)
{
    out[0] = 0;
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ARP_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return;
    DWORD cb = cch * sizeof(WCHAR), type = 0;
    RegQueryValueExW(key, L"InstallLocation", NULL, &type, (LPBYTE)out, &cb);
    RegCloseKey(key);
}

// 卸载收尾：从 %TEMP% 跑一份自己，等原进程退出后把安装目录（含原 Uninstall.exe）删掉。
static void SpawnPurge(const wchar_t *installDir)
{
    wchar_t self[MAX_PATH] = {}, tempDir[MAX_PATH] = {}, tempExe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, self, MAX_PATH);
    GetTempPathW(MAX_PATH, tempDir);
    StringCchPrintfW(tempExe, MAX_PATH, L"%serf-uninstall-purge.exe", tempDir);
    if (!CopyFileW(self, tempExe, FALSE))
    {
        Say(L"警告：无法把清理程序复制到 %s（错误 %u），安装目录可能残留。", tempExe, GetLastError());
        return;
    }
    wchar_t cmd[MAX_PATH * 2] = {};
    StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"\"%s\" --purge \"%s\"", tempExe, installDir);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// 递归删一个文件或目录（目录走 SHFileOperation，文件直接删）。
static void DeletePath(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (attr & FILE_ATTRIBUTE_DIRECTORY)
    {
        // SHFileOperation 要求双 NUL 结尾的路径列表
        wchar_t buf[MAX_PATH + 2] = {};
        StringCchCopyW(buf, MAX_PATH, path);
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = buf;
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        SHFileOperationW(&op);
    }
    else
    {
        DeleteFileW(path);
    }
}

static int PurgeDirectory(const wchar_t *dir)
{
    // 等原进程退出、文件句柄释放
    Sleep(1200);
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        if (RemoveDirectoryW(dir)) return 0;                 // 空目录：成功
        if (!PathFileExistsW(dir)) return 0;                 // 已经不在了
        // 还有东西：递归删内容
        wchar_t pattern[MAX_PATH] = {};
        StringCchPrintfW(pattern, MAX_PATH, L"%s\\*", dir);
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do {
                if (0 == wcscmp(fd.cFileName, L".") || 0 == wcscmp(fd.cFileName, L"..")) continue;
                wchar_t child[MAX_PATH] = {};
                StringCchPrintfW(child, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    DeletePath(child);
                else
                    DeleteFileW(child);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        Sleep(400);
    }
    return PathFileExistsW(dir) ? 1 : 0;
}

int wmain(int argc, wchar_t **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    g_silent = HasArg(L"--silent") || HasArg(L"/S") || HasArg(L"/VERYSILENT");

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t selfDir[MAX_PATH] = {};
    StringCchCopyW(selfDir, MAX_PATH, self);
    PathRemoveFileSpecW(selfDir);

    // 内部模式：清理残留目录（由 SpawnPurge 启动）
    if (HasArg(L"--purge"))
    {
        wchar_t dir[MAX_PATH] = {};
        if (ArgValue(L"--purge", dir, MAX_PATH)) return PurgeDirectory(dir);
        return 1;
    }

    if (IsUninstallMode(self))
    {
        wchar_t installDir[MAX_PATH] = {};
        ReadArpInstallDir(installDir, MAX_PATH);
        if (!installDir[0]) StringCchCopyW(installDir, MAX_PATH, selfDir);

        bool purgeConfig = HasArg(L"--purge-config");
        if (!g_silent)
        {
            wchar_t msg[1024] = {};
            StringCchPrintfW(msg, ARRAYSIZE(msg),
                L"要卸载「%s」吗？\n\n安装目录：%s\n\n"
                L"· 站点配置与凭据默认**保留**（%s\\ExplorerRemoteFs）\n"
                L"· 卸载会重启资源管理器（扩展 DLL 被加载时文件是锁着的）",
                APP_NAME, installDir, _wgetenv(L"APPDATA"));
            if (MessageBoxW(NULL, msg, L"易远传 卸载", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 2;
        }

        wchar_t script[MAX_PATH] = {}, args[MAX_PATH * 2] = {};
        StringCchPrintfW(script, MAX_PATH, L"%s\\uninstall.ps1", installDir);
        if (!PathFileExistsW(script))
        {
            // 脚本可能已被删：退回到"只清注册表 + 删目录"，并明确告知
            Say(L"找不到 %s —— 只做注册表清理与目录删除。", script);
        }
        else
        {
            StringCchPrintfW(args, ARRAYSIZE(args), L"-InstallDir \"%s\"%s",
                             installDir, purgeConfig ? L" -RemoveConfig" : L"");
            int code = RunPowerShell(script, args);
            if (code != 0) Say(L"卸载脚本返回 %d（继续清理注册表与目录）。", code);
        }

        RemoveArpAndRun();
        Say(L"卸载完成。站点配置%s。", purgeConfig ? L"已删除" : L"已保留（凭据管理器里的密码未动）");

        // 目录（含本程序自己）交给 %TEMP% 里的副本清理
        SpawnPurge(installDir);
        return 0;
    }

    // ── 安装 ────────────────────────────────────────────────────────────────
    wchar_t installDir[MAX_PATH] = {};
    if (!ArgValue(L"--dir", installDir, MAX_PATH))
    {
        wchar_t local[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local)))
        {
            Say(L"取不到 %%LOCALAPPDATA%%，请用 --dir 指定安装目录。");
            return 1;
        }
        StringCchPrintfW(installDir, MAX_PATH, L"%s\\ExplorerRemoteFs", local);
    }

    // 安装器必须在"随包目录"里跑：DLL/CLI/客户端/脚本都在它旁边
    const wchar_t *needed[] = { L"install.ps1", L"ExplorerDataProviderFtp.dll", L"client", L"cli" };
    for (const wchar_t *item : needed)
    {
        wchar_t probe[MAX_PATH] = {};
        StringCchPrintfW(probe, MAX_PATH, L"%s\\%s", selfDir, item);
        if (!PathFileExistsW(probe))
        {
            Say(L"这个目录里缺少 %s。\n\n请把 Setup.exe 与解压出来的其它文件放在同一个目录里再运行。", item);
            return 1;
        }
    }

    if (!g_silent)
    {
        wchar_t msg[1024] = {};
        StringCchPrintfW(msg, ARRAYSIZE(msg),
            L"安装「%s」%s\n\n安装到：%s\n\n"
            L"· 仅当前用户，不需要管理员权限\n"
            L"· 安装/升级会重启资源管理器（扩展 DLL 被加载时文件是锁着的，这是 Windows 的硬要求）\n"
            L"· 会在登录时自动启动常驻服务（托盘）\n\n继续吗？",
            APP_NAME, APP_VERSION, installDir);
        if (MessageBoxW(NULL, msg, L"易远传 安装", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 2;
    }

    wchar_t script[MAX_PATH] = {}, args[MAX_PATH * 3] = {};
    StringCchPrintfW(script, MAX_PATH, L"%s\\install.ps1", selfDir);
    StringCchPrintfW(args, ARRAYSIZE(args), L"-InstallDir \"%s\"", installDir);
    int code = RunPowerShell(script, args);
    if (code != 0)
    {
        Say(L"安装脚本返回 %d，安装未完成。\n\n可以看脚本输出，或直接运行：\n%s", code, script);
        return code;
    }

    // 卸载器必须**住在安装目录里**（"应用和功能"的 UninstallString 指向它），
    // 而且它要能独立跑 —— 所以把 Uninstall.exe 与 uninstall.ps1 一起复制过去。
    // 早先没做这一步：ARP 里的 UninstallString 指向一个不存在的文件（实测点卸载没反应）。
    //
    // 注意路径字面量必须是**双反斜杠**：`L"%s\Uninstall.exe"` 会被 MSVC 当成未知转义
    // （C4129）**把反斜杠悄悄吃掉**，于是目标变成 "...ExplorerRemoteFs-TestUninstall.exe"
    // —— 复制"成功"但文件没进安装目录，ARP 的卸载按钮点不动。实测 2026-09-17：
    // %LOCALAPPDATA% 下真躺着一个 ExplorerRemoteFs-TestUninstall.exe 就是它干的。
    // 现在 build.ps1 用 /we4129 把这条警告升级为错误，编译期就拦住。
    {
        wchar_t dst[MAX_PATH] = {};
        StringCchPrintfW(dst, MAX_PATH, L"%s\\Uninstall.exe", installDir);
        if (!CopyFileW(self, dst, FALSE))
            Say(L"警告：无法把卸载器复制到 %s（错误 %u）。", dst, GetLastError());
        // 安装目录要能"自给自足"：两个脚本也一起放进去 —— 万一命名空间注册坏了，
        // 用户只靠这个目录就能重注册（install.ps1）或卸载（uninstall.ps1）。
        // 复制失败只警告、不中断：注册已经完成，不该为辅助文件回滚整次安装。
        const wchar_t *scriptItems[] = { L"install.ps1", L"uninstall.ps1" };
        for (const wchar_t *item : scriptItems)
        {
            wchar_t srcScript[MAX_PATH] = {}, dstScript[MAX_PATH] = {};
            StringCchPrintfW(srcScript, MAX_PATH, L"%s\\%s", selfDir, item);
            StringCchPrintfW(dstScript, MAX_PATH, L"%s\\%s", installDir, item);
            if (PathFileExistsW(srcScript) && !CopyFileW(srcScript, dstScript, FALSE))
                Say(L"警告：无法把 %s 复制到 %s（错误 %u）。", item, dstScript, GetLastError());
        }
    }

    WriteArpEntry(installDir);
    WriteRunEntry(installDir);
    Say(L"安装完成。\n\n· 资源管理器导航窗格里的「易远传」就是入口\n"
        L"· 卸载：设置 → 应用 → 已安装的应用 → 易远传，或 %s\\Uninstall.exe\n"
        L"· 常驻服务已设为登录自启", installDir);
    return 0;
}
