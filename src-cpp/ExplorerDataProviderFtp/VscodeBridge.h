#pragma once
// VS Code 复用判据 + 请求文件协议（与 src-vscode-extension/extension.js 成对维护）。
//
// 背景：VS Code 没有「在已连接的窗口里新开终端」的命令行接口，扩展 URI 在本机
// VS Code 1.137 上也不派发给 handler。因此走本地文件协议：
//   请求：%LOCALAPPDATA%\ExplorerRemoteFs\vscode-request.json
//         {"authority":"ssh-remote+<别名>","path":"<远端目录>","ts":<epoch ms>}
//   心跳：%LOCALAPPDATA%\ExplorerRemoteFs\windows\win-<pid>.json
//         {"pid":<扩展宿主 pid>,"authority":"ssh-remote+<别名>","ts":<epoch ms>}
//
// **判据的意义**：只要已有窗口连着这个主机，就绝不能再去启动窗口 —— 否则 VS Code
// 会因为目录不同而新开一个窗口，而每个新窗口都要重新问一次「是否信任此工作区」。
// 这正是用户实测到的「每次都会弹出是否信任此工作区」。
#include <windows.h>
#include <string>

// 心跳每 2 秒刷新；超过这个时间视为窗口已关闭。
#define ERF_HEARTBEAT_STALE_MS 10000ULL

inline unsigned long long ErfNowEpochMs()
{
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}

// 别名 → 心跳文件里 authority 字段的取值（只保留安全字符，和扩展写出的格式一致）。
inline std::string ErfAuthorityForAlias(PCWSTR alias)
{
    std::string want = "ssh-remote+";
    for (PCWSTR p = alias; p && *p; ++p)
        if (iswalnum((wint_t)*p) || *p == L'-' || *p == L'_' || *p == L'.') want += (char)*p;
    return want;
}

// 找到「连着指定主机的 VS Code 主窗口」。
// VS Code 主窗口类名是 Chrome_WidgetWin_1；默认标题形如
//   "… [SSH: erf-WSL-SFTP] - Visual Studio Code"
// 用户自定义 window.title 时退化为「标题里出现别名」——两种都试，
// 但绝不退化成"任意一个 VS Code 窗口"（那会跳到别的项目窗口上）。
inline HWND ErfFindVscodeWindowForAlias(PCWSTR alias)
{
    if (!alias || !alias[0]) return NULL;
    struct Ctx { const WCHAR *alias; HWND found; };
    Ctx ctx = { alias, NULL };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        Ctx *c = reinterpret_cast<Ctx *>(lp);
        if (!IsWindowVisible(hwnd)) return TRUE;

        WCHAR cls[128] = {};
        GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
        if (0 != lstrcmpW(cls, L"Chrome_WidgetWin_1")) return TRUE;

        int len = GetWindowTextLengthW(hwnd);
        if (len <= 0) return TRUE;
        std::wstring title((size_t)len + 1, L'\0');
        GetWindowTextW(hwnd, &title[0], len + 1);
        title.resize((size_t)len);

        if (title.find(c->alias) == std::wstring::npos) return TRUE;   // 别名（含 [SSH: 别名]）
        c->found = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// 返回 TRUE 时调用方只写请求文件、不启动窗口。
// 是否已有存活的 VS Code 窗口连着这个 SSH 别名。
// 返回 TRUE 时调用方只写请求文件、不启动窗口。
inline BOOL ErfHasLiveVscodeWindow(PCWSTR alias)
{
    if (!alias || !alias[0]) return FALSE;
    WCHAR local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local))) return FALSE;

    std::wstring dir = std::wstring(local) + L"\\ExplorerRemoteFs\\windows";
    std::wstring pattern = dir + L"\\win-*.json";
    const std::string want = ErfAuthorityForAlias(alias);
    const unsigned long long nowMs = ErfNowEpochMs();

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    BOOL found = FALSE;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring file = dir + L"\\" + fd.cFileName;
        HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        char buf[1024] = {};
        DWORD got = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(f);
        std::string text(buf, got);

        size_t a = text.find("\"authority\"");
        size_t t = text.find("\"ts\"");
        if (a == std::string::npos || t == std::string::npos) continue;
        if (text.find(want, a) == std::string::npos) continue;

        size_t colon = text.find(':', t + 4);
        if (colon == std::string::npos) continue;
        unsigned long long ts = 0;
        for (size_t k = colon + 1; k < text.size() && text[k] >= '0' && text[k] <= '9'; ++k)
            ts = ts * 10 + (unsigned long long)(text[k] - '0');
        if (ts && nowMs > ts && nowMs - ts < ERF_HEARTBEAT_STALE_MS)
        {
            found = TRUE;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}
